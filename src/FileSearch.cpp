#include "FileSearch.h"

#include <KnownFolders.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <sddl.h>
#include <taskschd.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <utility>

using filesearch::FileIndex;

namespace {

constexpr wchar_t kTaskPrefix[] = L"HoverdockMftIndex-";
constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\Hoverdock.MftIndex.";
constexpr uint32_t kDumpMagic = 0x464D4448U;  // "HDMF"
constexpr DWORD kEnumBufferBytes = 1U << 20;
constexpr DWORD kJournalBufferBytes = 256U << 10;
constexpr uint32_t kMaxChunkBytes = 4U << 20;
constexpr DWORD kHelperConnectTimeoutMs = 30000;
constexpr DWORD kPipeReadTimeoutMs = 60000;
constexpr DWORD kActivePollMs = 250;
constexpr DWORD kIdlePollMs = 30000;
constexpr DWORD kRetryBuildMs = 5U * 60U * 1000U;
constexpr DWORD kJournalReasons = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE | USN_REASON_RENAME_NEW_NAME |
    USN_REASON_BASIC_INFO_CHANGE;

// Wire chunk: header then `length` payload bytes.
struct DumpChunk {
    uint8_t tag = 0;   // 'H' magic, 'V' volume, 'R' records, 'E' end volume, 'X' volume failed, 'Z' done
    uint8_t reserved = 0;
    uint16_t letter = 0;
    uint32_t length = 0;
};
static_assert(sizeof(DumpChunk) == 8);

struct DumpVolume {
    uint64_t rootId = 0;
    uint64_t journalId = 0;
    int64_t nextUsn = 0;
};

// ---- small Win32 helpers ----------------------------------------------------

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) noexcept : m_handle(handle) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
    [[nodiscard]] bool Valid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
    void Reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (Valid()) {
            CloseHandle(m_handle);
        }
        m_handle = handle;
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

std::wstring ModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

bool SamePath(const std::wstring& a, const std::wstring& b) noexcept {
    return !a.empty() && CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                             static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring TokenUserSid(HANDLE token) {
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        return {};
    }
    std::vector<BYTE> buffer(needed);
    if (GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed) == FALSE) {
        return {};
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &text) == FALSE || text == nullptr) {
        return {};
    }
    std::wstring sid(text);
    LocalFree(text);
    return sid;
}

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return {};
    }
    std::wstring sid = TokenUserSid(token);
    CloseHandle(token);
    return sid;
}

bool IsElevated() noexcept {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) !=
            FALSE &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

std::wstring ProcessImagePath(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.Valid()) {
        return {};
    }
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process.Get(), 0, path.data(), &size) == FALSE) {
        return {};
    }
    path.resize(size);
    return path;
}

std::wstring ProcessUserSid(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.Valid()) {
        return {};
    }
    HANDLE token = nullptr;
    if (OpenProcessToken(process.Get(), TOKEN_QUERY, &token) == FALSE) {
        return {};
    }
    std::wstring sid = TokenUserSid(token);
    CloseHandle(token);
    return sid;
}

uint8_t FlagsFromAttributes(DWORD attributes) noexcept {
    uint8_t flags = 0;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        flags |= filesearch::kFlagDirectory;
    }
    if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
        flags |= filesearch::kFlagHidden;
    }
    if ((attributes & FILE_ATTRIBUTE_SYSTEM) != 0) {
        flags |= filesearch::kFlagSystem;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        flags |= filesearch::kFlagReparse;
    }
    return flags;
}

bool FileIdOf(const std::wstring& path, uint64_t& id, DWORD* attributes = nullptr) {
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!file.Valid()) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(file.Get(), &info) == FALSE) {
        return false;
    }
    id = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    if (attributes != nullptr) {
        *attributes = info.dwFileAttributes;
    }
    return true;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path != nullptr) {
        result = path;
    }
    CoTaskMemFree(path);
    return result;
}

wchar_t DriveLetterOf(const std::wstring& path) noexcept {
    if (path.size() >= 2 && path[1] == L':') {
        return static_cast<wchar_t>(towupper(path[0]));
    }
    return L'\0';
}

// Fixed NTFS volumes with a drive letter, system drive first.
std::vector<wchar_t> FixedNtfsVolumes() {
    std::vector<wchar_t> letters;
    const wchar_t system = DriveLetterOf(KnownFolder(FOLDERID_Windows));
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((drives & (1U << i)) == 0) {
            continue;
        }
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) != DRIVE_FIXED) {
            continue;
        }
        wchar_t fileSystem[MAX_PATH + 1]{};
        if (GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fileSystem,
                MAX_PATH + 1) == FALSE ||
            _wcsicmp(fileSystem, L"NTFS") != 0) {
            continue;
        }
        letters.push_back(root[0]);
    }
    std::stable_partition(letters.begin(), letters.end(), [system](wchar_t l) { return l == system; });
    return letters;
}

std::string RootName(wchar_t letter) {
    return std::string{static_cast<char>(letter), ':'};
}

std::wstring FromUtf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 0)), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    }
    return wide;
}

std::string ToUtf8(const std::wstring& text) {
    std::string out;
    filesearch::AppendUtf8(out, reinterpret_cast<const char16_t*>(text.data()), text.size());
    return out;
}

// Calls f(id, parentId, attributes, reason, name, nameLength) for every
// well-formed USN record (V2 or V3) in `data` (the bytes after the leading USN).
template <typename F>
void ForEachUsnRecord(const BYTE* data, size_t size, F&& f) {
    size_t offset = 0;
    while (offset + sizeof(USN_RECORD_COMMON_HEADER) <= size) {
        const auto* header = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(data + offset);
        const DWORD length = header->RecordLength;
        if (length < sizeof(USN_RECORD_COMMON_HEADER) || offset + length > size) {
            return;
        }
        if (header->MajorVersion == 2 && length >= sizeof(USN_RECORD_V2)) {
            const auto* record = reinterpret_cast<const USN_RECORD_V2*>(data + offset);
            if (static_cast<size_t>(record->FileNameOffset) + record->FileNameLength <= length) {
                f(record->FileReferenceNumber, record->ParentFileReferenceNumber, record->FileAttributes,
                    record->Reason,
                    reinterpret_cast<const char16_t*>(reinterpret_cast<const BYTE*>(record) + record->FileNameOffset),
                    static_cast<size_t>(record->FileNameLength / 2));
            }
        } else if (header->MajorVersion == 3 && length >= sizeof(USN_RECORD_V3)) {
            const auto* record = reinterpret_cast<const USN_RECORD_V3*>(data + offset);
            if (static_cast<size_t>(record->FileNameOffset) + record->FileNameLength <= length) {
                uint64_t id = 0;
                uint64_t parent = 0;
                std::memcpy(&id, record->FileReferenceNumber.Identifier, sizeof(id));
                std::memcpy(&parent, record->ParentFileReferenceNumber.Identifier, sizeof(parent));
                f(id, parent, record->FileAttributes, record->Reason,
                    reinterpret_cast<const char16_t*>(reinterpret_cast<const BYTE*>(record) + record->FileNameOffset),
                    static_cast<size_t>(record->FileNameLength / 2));
            }
        }
        offset += length;
    }
}

void AddUsnRecords(FileIndex& index, uint8_t volume, const BYTE* data, size_t size, std::atomic<size_t>& progress) {
    size_t added = 0;
    ForEachUsnRecord(data, size,
        [&](uint64_t id, uint64_t parent, DWORD attributes, DWORD, const char16_t* name, size_t length) {
            index.AddUtf16(volume, id, parent, FlagsFromAttributes(attributes), name, length);
            ++added;
        });
    progress.fetch_add(added, std::memory_order_relaxed);
}

// Reads the whole MFT of `letter` (needs an elevated token). `onVolume` runs
// once before the first batch; `onRecords` receives raw USN record batches
// and may return false to abort.
bool EnumerateMft(wchar_t letter, const std::function<void(const DumpVolume&)>& onVolume,
    const std::function<bool(const BYTE*, size_t)>& onRecords, DWORD& error) {
    const wchar_t device[] = {L'\\', L'\\', L'.', L'\\', letter, L':', L'\0'};
    Handle volume(CreateFileW(device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (!volume.Valid()) {
        error = GetLastError();
        return false;
    }
    DumpVolume header;
    const wchar_t root[] = {letter, L':', L'\\', L'\0'};
    if (!FileIdOf(root, header.rootId)) {
        error = GetLastError();
        return false;
    }
    USN_JOURNAL_DATA_V0 journal{};
    DWORD bytes = 0;
    MFT_ENUM_DATA_V0 enumData{};
    enumData.HighUsn = MAXLONGLONG;
    if (DeviceIoControl(volume.Get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal), &bytes,
            nullptr) != FALSE) {
        header.journalId = journal.UsnJournalID;
        header.nextUsn = journal.NextUsn;
        enumData.HighUsn = journal.NextUsn;
    }
    std::vector<BYTE> buffer(kEnumBufferBytes);
    bool announced = false;
    for (;;) {
        if (DeviceIoControl(volume.Get(), FSCTL_ENUM_USN_DATA, &enumData, sizeof(enumData), buffer.data(),
                kEnumBufferBytes, &bytes, nullptr) == FALSE) {
            const DWORD last = GetLastError();
            if (last == ERROR_HANDLE_EOF && announced) {
                return true;
            }
            if (last == ERROR_HANDLE_EOF) {
                onVolume(header);
                return true;
            }
            error = last;
            return false;
        }
        if (!announced) {
            onVolume(header);
            announced = true;
        }
        if (bytes <= sizeof(USN)) {
            return true;
        }
        if (!onRecords(buffer.data() + sizeof(USN), bytes - sizeof(USN))) {
            error = ERROR_OPERATION_ABORTED;
            return false;
        }
        std::memcpy(&enumData.StartFileReferenceNumber, buffer.data(), sizeof(DWORDLONG));
    }
}

// ---- Task Scheduler ---------------------------------------------------------

class TaskFolder {
public:
    TaskFolder() {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_ownsCom = init == S_OK || init == S_FALSE;
        if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                reinterpret_cast<void**>(&m_service))) ||
            m_service == nullptr) {
            return;
        }
        VARIANT empty{};
        VariantInit(&empty);
        if (FAILED(m_service->Connect(empty, empty, empty, empty))) {
            return;
        }
        BSTR root = SysAllocString(L"\\");
        if (root != nullptr) {
            static_cast<void>(m_service->GetFolder(root, &m_folder));
            SysFreeString(root);
        }
    }
    ~TaskFolder() {
        if (m_folder != nullptr) {
            m_folder->Release();
        }
        if (m_service != nullptr) {
            m_service->Release();
        }
        if (m_ownsCom) {
            CoUninitialize();
        }
    }
    TaskFolder(const TaskFolder&) = delete;
    TaskFolder& operator=(const TaskFolder&) = delete;
    [[nodiscard]] ITaskService* Service() const noexcept { return m_service; }
    [[nodiscard]] ITaskFolder* Folder() const noexcept { return m_folder; }

    [[nodiscard]] IRegisteredTask* Get(const std::wstring& name) const {
        if (m_folder == nullptr) {
            return nullptr;
        }
        BSTR taskName = SysAllocString(name.c_str());
        IRegisteredTask* task = nullptr;
        if (taskName != nullptr) {
            if (FAILED(m_folder->GetTask(taskName, &task))) {
                task = nullptr;
            }
            SysFreeString(taskName);
        }
        return task;
    }

private:
    ITaskService* m_service = nullptr;
    ITaskFolder* m_folder = nullptr;
    bool m_ownsCom = false;
};

std::wstring TaskName(const std::wstring& sid) {
    return std::wstring(kTaskPrefix) + sid;
}

// The elevated task runs a copy of Dock.exe from Program Files: the per-user
// install folder is user-writable, so pointing an elevated task there would
// let any process of the user swap the binary and run it elevated silently.
std::wstring HelperPath() {
    const std::wstring programFiles = KnownFolder(FOLDERID_ProgramFiles);
    return programFiles.empty() ? std::wstring() : programFiles + L"\\Hoverdock\\HoverdockSearchIndexer.exe";
}

bool MftTaskExists(const std::wstring& sid) {
    TaskFolder folder;
    IRegisteredTask* task = folder.Get(TaskName(sid));
    if (task == nullptr) {
        return false;
    }
    task->Release();
    return true;
}

bool RunMftTask(const std::wstring& sid) {
    TaskFolder folder;
    IRegisteredTask* task = folder.Get(TaskName(sid));
    if (task == nullptr) {
        return false;
    }
    VARIANT none{};
    VariantInit(&none);
    IRunningTask* running = nullptr;
    const HRESULT ran = task->Run(none, &running);
    if (running != nullptr) {
        running->Release();
    }
    task->Release();
    return SUCCEEDED(ran);
}

// ---- pipe transport -----------------------------------------------------------

bool WriteAll(HANDLE pipe, const void* data, size_t size) {
    const auto* bytes = static_cast<const BYTE*>(data);
    while (size > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1U << 20));
        if (WriteFile(pipe, bytes, chunk, &written, nullptr) == FALSE || written == 0) {
            return false;
        }
        bytes += written;
        size -= written;
    }
    return true;
}

bool WriteChunk(HANDLE pipe, uint8_t tag, wchar_t letter, const void* payload, uint32_t length) {
    DumpChunk chunk;
    chunk.tag = tag;
    chunk.letter = static_cast<uint16_t>(letter);
    chunk.length = length;
    return WriteAll(pipe, &chunk, sizeof(chunk)) && (length == 0 || WriteAll(pipe, payload, length));
}

class PipeReader {
public:
    PipeReader(HANDLE pipe, HANDLE event, const std::atomic<bool>& stop) noexcept
        : m_pipe(pipe), m_event(event), m_stop(stop) {}

    bool Read(void* data, size_t size) {
        auto* bytes = static_cast<BYTE*>(data);
        while (size > 0) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = m_event;
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(size, 1U << 20));
            if (ReadFile(m_pipe, bytes, want, &got, &overlapped) == FALSE) {
                if (GetLastError() != ERROR_IO_PENDING || !Wait(overlapped, got)) {
                    return false;
                }
            } else if (GetOverlappedResult(m_pipe, &overlapped, &got, FALSE) == FALSE) {
                return false;
            }
            if (got == 0) {
                return false;
            }
            bytes += got;
            size -= got;
        }
        return true;
    }

private:
    bool Wait(OVERLAPPED& overlapped, DWORD& got) {
        const ULONGLONG deadline = GetTickCount64() + kPipeReadTimeoutMs;
        for (;;) {
            const DWORD waited = WaitForSingleObject(m_event, 250);
            if (waited == WAIT_OBJECT_0) {
                return GetOverlappedResult(m_pipe, &overlapped, &got, FALSE) != FALSE;
            }
            if (m_stop.load() || GetTickCount64() > deadline) {
                CancelIoEx(m_pipe, &overlapped);
                GetOverlappedResult(m_pipe, &overlapped, &got, TRUE);
                return false;
            }
        }
    }

    HANDLE m_pipe;
    HANDLE m_event;
    const std::atomic<bool>& m_stop;
};

// ---- zones ----------------------------------------------------------------------

struct VolumeSlot {
    wchar_t letter = L'\0';
    uint8_t volume = 0;
};

void ApplyZoneMarkers(FileIndex& index, const std::vector<VolumeSlot>& volumes) {
    struct Marker {
        const KNOWNFOLDERID* folder;
        filesearch::Zone zone;
    };
    static const Marker markers[] = {
        {&FOLDERID_Profile, filesearch::Zone::UserHome},
        {&FOLDERID_Desktop, filesearch::Zone::UserFolder},
        {&FOLDERID_Documents, filesearch::Zone::UserFolder},
        {&FOLDERID_Downloads, filesearch::Zone::UserFolder},
        {&FOLDERID_Pictures, filesearch::Zone::UserFolder},
        {&FOLDERID_Music, filesearch::Zone::UserFolder},
        {&FOLDERID_Videos, filesearch::Zone::UserFolder},
        {&FOLDERID_SkyDrive, filesearch::Zone::UserFolder},
        {&FOLDERID_RoamingAppData, filesearch::Zone::AppData},
        {&FOLDERID_LocalAppData, filesearch::Zone::AppData},
        {&FOLDERID_LocalAppDataLow, filesearch::Zone::AppData},
        // Hidden in Explorer; full of package caches and container layers.
        {&FOLDERID_ProgramData, filesearch::Zone::Hidden},
        // Start menu shortcuts are listed as apps already.
        {&FOLDERID_StartMenu, filesearch::Zone::AppData},
        {&FOLDERID_CommonStartMenu, filesearch::Zone::AppData},
        {&FOLDERID_Windows, filesearch::Zone::Windows},
        {&FOLDERID_ProgramFiles, filesearch::Zone::ProgramFiles},
        {&FOLDERID_ProgramFilesX86, filesearch::Zone::ProgramFiles},
    };
    std::vector<std::pair<std::wstring, filesearch::Zone>> paths;
    for (const Marker& marker : markers) {
        paths.emplace_back(KnownFolder(*marker.folder), marker.zone);
    }
    // Component store and installer caches: thousands of duplicate copies of
    // system files (WinSxS alone mirrors most of System32).
    const std::wstring windows = KnownFolder(FOLDERID_Windows);
    if (!windows.empty()) {
        for (const wchar_t* sub : {L"\\WinSxS", L"\\servicing", L"\\Installer", L"\\assembly",
                 L"\\SoftwareDistribution", L"\\Prefetch", L"\\Temp"}) {
            paths.emplace_back(windows + sub, filesearch::Zone::Hidden);
        }
    }
    for (const auto& [path, zone] : paths) {
        const wchar_t letter = DriveLetterOf(path);
        uint64_t id = 0;
        if (letter == L'\0' || !FileIdOf(path, id)) {
            continue;
        }
        for (const VolumeSlot& slot : volumes) {
            if (slot.letter == letter) {
                index.SetZoneMarker(slot.volume, id, zone);
            }
        }
    }
}

// ---- builders ---------------------------------------------------------------------

struct BuildContext {
    FileIndex& index;
    std::vector<VolumeSlot>& volumes;
    std::atomic<size_t>& progress;
    const std::atomic<bool>& stop;
};

struct JournalStart {
    wchar_t letter = L'\0';
    uint8_t volume = 0;
    uint64_t journalId = 0;
    int64_t nextUsn = 0;
};

// All-or-nothing: a token that can read some volumes but not others (e.g. a
// filtered admin) must fall through to the helper rather than silently
// indexing only part of the disk.
bool BuildFromMftInProcess(BuildContext& ctx, std::vector<JournalStart>& journals) {
    bool any = false;
    for (const wchar_t letter : FixedNtfsVolumes()) {
        if (ctx.stop.load()) {
            return false;
        }
        uint8_t volume = 0;
        bool opened = false;
        DWORD error = 0;
        const bool ok = EnumerateMft(
            letter,
            [&](const DumpVolume& header) {
                volume = ctx.index.AddVolume(RootName(letter), header.rootId);
                opened = true;
                ctx.volumes.push_back({letter, volume});
                journals.push_back({letter, volume, header.journalId, header.nextUsn});
            },
            [&](const BYTE* data, size_t size) {
                AddUsnRecords(ctx.index, volume, data, size, ctx.progress);
                return !ctx.stop.load();
            },
            error);
        if (opened) {
            ctx.index.FinishVolume(volume);
            any = any || ok;
        }
        if (!ok && (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)) {
            return false;
        }
    }
    return any;
}

// Returns true when the helper delivered at least one volume. `denied` is
// set when the helper ran but could not read any MFT (not an admin).
bool BuildFromHelper(BuildContext& ctx, std::vector<JournalStart>& journals, const std::wstring& sid, bool& denied) {
    denied = false;
    const std::wstring pipeName = std::wstring(kPipePrefix) + sid;
    Handle pipe(CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE |
            FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 1U << 20, 0,
        nullptr));
    if (!pipe.Valid()) {
        return false;
    }
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.Valid()) {
        return false;
    }
    OVERLAPPED connect{};
    connect.hEvent = event.Get();
    bool connected = ConnectNamedPipe(pipe.Get(), &connect) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
    if (!connected && GetLastError() != ERROR_IO_PENDING) {
        return false;
    }
    if (!RunMftTask(sid)) {
        CancelIoEx(pipe.Get(), &connect);
        return false;
    }
    if (!connected) {
        const ULONGLONG deadline = GetTickCount64() + kHelperConnectTimeoutMs;
        while (!connected) {
            if (WaitForSingleObject(event.Get(), 250) == WAIT_OBJECT_0) {
                DWORD ignored = 0;
                connected = GetOverlappedResult(pipe.Get(), &connect, &ignored, FALSE) != FALSE;
                break;
            }
            if (ctx.stop.load() || GetTickCount64() > deadline) {
                break;
            }
        }
        if (!connected) {
            CancelIoEx(pipe.Get(), &connect);
            DWORD ignored = 0;
            GetOverlappedResult(pipe.Get(), &connect, &ignored, TRUE);
            return false;
        }
    }
    // Only the admin-protected helper binary may feed the index.
    ULONG clientPid = 0;
    if (GetNamedPipeClientProcessId(pipe.Get(), &clientPid) == FALSE ||
        !SamePath(ProcessImagePath(clientPid), HelperPath())) {
        return false;
    }

    ResetEvent(event.Get());
    PipeReader reader(pipe.Get(), event.Get(), ctx.stop);
    DumpChunk chunk;
    uint32_t magic = 0;
    if (!reader.Read(&chunk, sizeof(chunk)) || chunk.tag != 'H' || chunk.length != sizeof(magic) ||
        !reader.Read(&magic, sizeof(magic)) || magic != kDumpMagic) {
        return false;
    }
    std::vector<BYTE> buffer;
    bool open = false;
    uint8_t volume = 0;
    size_t delivered = 0;
    size_t failed = 0;
    for (;;) {
        if (!reader.Read(&chunk, sizeof(chunk)) || chunk.length > kMaxChunkBytes) {
            break;
        }
        buffer.resize(chunk.length);
        if (chunk.length > 0 && !reader.Read(buffer.data(), chunk.length)) {
            break;
        }
        const auto letter = static_cast<wchar_t>(chunk.letter);
        if (chunk.tag == 'Z') {
            break;
        }
        if (chunk.tag == 'V' && chunk.length == sizeof(DumpVolume) && !open && letter >= L'A' && letter <= L'Z') {
            DumpVolume header;
            std::memcpy(&header, buffer.data(), sizeof(header));
            volume = ctx.index.AddVolume(RootName(letter), header.rootId);
            ctx.volumes.push_back({letter, volume});
            journals.push_back({letter, volume, header.journalId, header.nextUsn});
            open = true;
        } else if (chunk.tag == 'R' && open) {
            AddUsnRecords(ctx.index, volume, buffer.data(), buffer.size(), ctx.progress);
        } else if (chunk.tag == 'E' && open) {
            ctx.index.FinishVolume(volume);
            open = false;
            ++delivered;
        } else if (chunk.tag == 'X') {
            if (open) {
                ctx.index.FinishVolume(volume);
                open = false;
            }
            ++failed;
        }
    }
    if (open) {
        ctx.index.FinishVolume(volume);
    }
    denied = delivered == 0 && failed > 0;
    return delivered > 0;
}

// Fallback without MFT access: crawl the user's folders (not descending into
// hidden/system directories such as AppData).
bool BuildFromCrawl(BuildContext& ctx, std::vector<JournalStart>& journals) {
    std::vector<std::wstring> roots;
    for (const KNOWNFOLDERID* id : {&FOLDERID_Profile, &FOLDERID_Public}) {
        std::wstring path = KnownFolder(*id);
        if (!path.empty() && DriveLetterOf(path) != L'\0') {
            roots.push_back(std::move(path));
        }
    }
    std::vector<BYTE> buffer(64U << 10);
    bool any = false;
    std::vector<wchar_t> letters;
    for (const std::wstring& root : roots) {
        if (std::find(letters.begin(), letters.end(), DriveLetterOf(root)) == letters.end()) {
            letters.push_back(DriveLetterOf(root));
        }
    }
    for (const wchar_t letter : letters) {
        const wchar_t rootPath[] = {letter, L':', L'\\', L'\0'};
        uint64_t rootId = 0;
        if (!FileIdOf(rootPath, rootId)) {
            continue;
        }
        // Capture the journal position before crawling so nothing is missed.
        JournalStart start{letter, 0, 0, 0};
        {
            const wchar_t device[] = {L'\\', L'\\', L'.', L'\\', letter, L':', L'\0'};
            Handle volumeHandle(CreateFileW(device, FILE_TRAVERSE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
            USN_JOURNAL_DATA_V0 journal{};
            DWORD bytes = 0;
            if (volumeHandle.Valid() &&
                DeviceIoControl(volumeHandle.Get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal),
                    &bytes, nullptr) != FALSE) {
                start.journalId = journal.UsnJournalID;
                start.nextUsn = journal.NextUsn;
            }
        }
        const uint8_t volume = ctx.index.AddVolume(RootName(letter), rootId);
        ctx.volumes.push_back({letter, volume});
        start.volume = volume;
        if (start.journalId != 0) {
            journals.push_back(start);
        }
        std::unordered_set<uint64_t> seen;
        for (const std::wstring& root : roots) {
            if (DriveLetterOf(root) != letter) {
                continue;
            }
            // Ancestor chain C:\Users, C:\Users\name so paths resolve.
            uint64_t parent = rootId;
            size_t pos = 3;
            bool chainOk = true;
            while (pos <= root.size()) {
                size_t end = root.find(L'\\', pos);
                if (end == std::wstring::npos) {
                    end = root.size();
                }
                if (end > pos) {
                    const std::wstring prefix = root.substr(0, end);
                    uint64_t id = 0;
                    DWORD attributes = 0;
                    if (!FileIdOf(prefix, id, &attributes)) {
                        chainOk = false;
                        break;
                    }
                    if (seen.insert(id).second) {
                        const std::wstring name = root.substr(pos, end - pos);
                        ctx.index.AddUtf16(volume, id, parent, FlagsFromAttributes(attributes),
                            reinterpret_cast<const char16_t*>(name.data()), name.size());
                    }
                    parent = id;
                }
                pos = end + 1;
            }
            if (!chainOk) {
                continue;
            }
            std::vector<std::pair<std::wstring, uint64_t>> stack{{root, parent}};
            while (!stack.empty()) {
                if (ctx.stop.load()) {
                    return false;
                }
                auto [dir, dirId] = std::move(stack.back());
                stack.pop_back();
                Handle handle(CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (!handle.Valid()) {
                    continue;
                }
                FILE_INFO_BY_HANDLE_CLASS infoClass = FileIdBothDirectoryRestartInfo;
                size_t added = 0;
                while (GetFileInformationByHandleEx(handle.Get(), infoClass, buffer.data(),
                           static_cast<DWORD>(buffer.size())) != FALSE) {
                    infoClass = FileIdBothDirectoryInfo;
                    size_t offset = 0;
                    for (;;) {
                        const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
                        const std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
                        if (name != L"." && name != L"..") {
                            const auto id = static_cast<uint64_t>(entry->FileId.QuadPart);
                            if (seen.insert(id).second) {
                                ctx.index.AddUtf16(volume, id, dirId, FlagsFromAttributes(entry->FileAttributes),
                                    reinterpret_cast<const char16_t*>(name.data()), name.size());
                                ++added;
                                constexpr DWORD kSkip =
                                    FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
                                if ((entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                                    (entry->FileAttributes & kSkip) == 0) {
                                    stack.emplace_back(dir + L"\\" + std::wstring(name), id);
                                }
                            }
                        }
                        if (entry->NextEntryOffset == 0) {
                            break;
                        }
                        offset += entry->NextEntryOffset;
                    }
                }
                ctx.progress.fetch_add(added, std::memory_order_relaxed);
            }
            any = true;
        }
        ctx.index.FinishVolume(volume);
    }
    return any;
}

// MFT enumeration reports one name per record, so system files hard-linked
// into WinSxS can surface only under WinSxS. Re-list the few directories
// people actually look for and add their link names as aliases.
void AddSystemLinkAliases(FileIndex& index, const std::vector<VolumeSlot>& volumes) {
    const std::wstring windows = KnownFolder(FOLDERID_Windows);
    const wchar_t letter = DriveLetterOf(windows);
    const auto slot = std::find_if(volumes.begin(), volumes.end(), [letter](const VolumeSlot& v) {
        return v.letter == letter;
    });
    if (windows.empty() || slot == volumes.end()) {
        return;
    }
    std::vector<BYTE> buffer(64U << 10);
    for (const std::wstring& directory : {windows, KnownFolder(FOLDERID_System), KnownFolder(FOLDERID_SystemX86)}) {
        uint64_t directoryId = 0;
        if (directory.empty() || !FileIdOf(directory, directoryId)) {
            continue;
        }
        Handle handle(CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!handle.Valid()) {
            continue;
        }
        FILE_INFO_BY_HANDLE_CLASS infoClass = FileIdBothDirectoryRestartInfo;
        while (GetFileInformationByHandleEx(handle.Get(), infoClass, buffer.data(),
                   static_cast<DWORD>(buffer.size())) != FALSE) {
            infoClass = FileIdBothDirectoryInfo;
            size_t offset = 0;
            for (;;) {
                const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
                if ((entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    std::string name;
                    filesearch::AppendUtf8(name, reinterpret_cast<const char16_t*>(entry->FileName),
                        entry->FileNameLength / sizeof(wchar_t));
                    static_cast<void>(index.AddAlias(slot->volume, static_cast<uint64_t>(entry->FileId.QuadPart),
                        directoryId, FlagsFromAttributes(entry->FileAttributes), name));
                }
                if (entry->NextEntryOffset == 0) {
                    break;
                }
                offset += entry->NextEntryOffset;
            }
        }
    }
}

void FillMetadata(FileSearchItem& item) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(item.path.c_str(), GetFileExInfoStandard, &data) != FALSE) {
        item.size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        item.modified = data.ftLastWriteTime;
        item.haveMetadata = true;
    }
}

}  // namespace

// ---- FileSearchService ------------------------------------------------------------

FileSearchService::~FileSearchService() {
    Stop();
}

void FileSearchService::Start(HWND notify, UINT statusMessage) {
    if (m_started.exchange(true)) {
        return;
    }
    m_notify = notify;
    m_statusMessage = statusMessage;
    m_stop.store(false);
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_indexThread = std::thread([this] { IndexThread(); });
    m_searchThread = std::thread([this] { SearchThread(); });
}

void FileSearchService::Stop() noexcept {
    if (!m_started.exchange(false)) {
        return;
    }
    m_stop.store(true);
    if (m_wake != nullptr) {
        SetEvent(m_wake);
    }
    {
        std::lock_guard lock(m_requestLock);
        m_hasRequest = false;
    }
    m_requestReady.notify_all();
    if (m_indexThread.joinable()) {
        m_indexThread.join();
    }
    if (m_searchThread.joinable()) {
        m_searchThread.join();
    }
    if (m_wake != nullptr) {
        CloseHandle(m_wake);
        m_wake = nullptr;
    }
}

void FileSearchService::Restart() {
    m_rebuild.store(true);
    if (m_wake != nullptr) {
        SetEvent(m_wake);
    }
}

void FileSearchService::SetActive(bool active) noexcept {
    const bool was = m_active.exchange(active);
    if (active && !was && m_wake != nullptr) {
        SetEvent(m_wake);  // catch up on the journal right away
    }
}

void FileSearchService::SetApps(const std::vector<std::wstring>& appNames) {
    FileIndex apps;
    const uint8_t volume = apps.AddVolume("", 0);
    // Ids start above the reserved NTFS metadata range; entry i+1 = app i.
    for (size_t i = 0; i < appNames.size(); ++i) {
        apps.Add(volume, 1000 + i, 0, 0, ToUtf8(appNames[i]));
    }
    apps.FinishVolume(volume);
    apps.RecomputeZones();
    std::unique_lock lock(m_indexLock);
    m_apps = std::move(apps);
}

void FileSearchService::Query(HWND reply, UINT message, uint64_t requestId, std::wstring query, size_t fileLimit,
    size_t appLimit) {
    {
        std::lock_guard lock(m_requestLock);
        m_request = {reply, message, requestId, std::move(query), fileLimit, appLimit};
        m_hasRequest = true;
    }
    m_requestReady.notify_one();
}

FileSearchService::Status FileSearchService::GetStatus() const {
    std::lock_guard lock(m_statusLock);
    Status status = m_status;
    status.progress = m_progress.load();
    return status;
}

void FileSearchService::NotifyStatus() const noexcept {
    if (m_notify != nullptr && m_statusMessage != 0) {
        PostMessageW(m_notify, m_statusMessage, 0, 0);
    }
}

bool FileSearchService::BuildIndex(std::unique_ptr<FileIndex>& index, std::vector<Journal>& journals,
    FileSearchMode& mode) {
    const std::wstring sid = CurrentUserSid();
    const bool elevated = IsElevated();
    std::vector<JournalStart> starts;
    std::vector<VolumeSlot> volumes;
    bool built = false;
    bool denied = false;
    auto attempt = [&](auto&& builder) {
        index = std::make_unique<FileIndex>();
        starts.clear();
        volumes.clear();
        m_progress.store(0);
        BuildContext ctx{*index, volumes, m_progress, m_stop};
        return builder(ctx);
    };
    // An elevated-looking token can still lack volume access (filtered
    // admin), so fall through to the helper whenever in-process fails.
    if (elevated) {
        built = attempt([&](BuildContext& ctx) { return BuildFromMftInProcess(ctx, starts); });
    }
    if (!built && !m_stop.load() && !sid.empty() && MftTaskExists(sid)) {
        built = attempt([&](BuildContext& ctx) { return BuildFromHelper(ctx, starts, sid, denied); });
    }
    mode = FileSearchMode::Mft;
    if (!built && !m_stop.load()) {
        built = attempt([&](BuildContext& ctx) { return BuildFromCrawl(ctx, starts); });
        mode = FileSearchMode::Folders;
    }
    if (!built) {
        return false;
    }
    if (mode == FileSearchMode::Mft) {
        AddSystemLinkAliases(*index, volumes);
    }
    ApplyZoneMarkers(*index, volumes);
    index->RecomputeZones();
    index->ShrinkToFit();

    journals.clear();
    for (const JournalStart& start : starts) {
        if (start.journalId == 0) {
            continue;
        }
        const wchar_t device[] = {L'\\', L'\\', L'.', L'\\', start.letter, L':', L'\0'};
        constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        bool privileged = true;
        HANDLE handle = CreateFileW(device, GENERIC_READ, kShare, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            // Standard users: traverse-only handle + unprivileged journal reads.
            privileged = false;
            handle = CreateFileW(device, FILE_TRAVERSE, kShare, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }
        Journal journal;
        journal.volume = start.volume;
        journal.letter = start.letter;
        journal.handle = handle;
        journal.journalId = start.journalId;
        journal.nextUsn = start.nextUsn;
        journal.privileged = privileged;
        journals.push_back(journal);
    }
    std::lock_guard lock(m_statusLock);
    m_status.canEnableMft = mode == FileSearchMode::Folders && !elevated && !denied;
    return true;
}

void FileSearchService::CloseJournals() noexcept {
    for (Journal& journal : m_journals) {
        if (journal.handle != INVALID_HANDLE_VALUE) {
            CloseHandle(journal.handle);
            journal.handle = INVALID_HANDLE_VALUE;
        }
    }
    m_journals.clear();
}

bool FileSearchService::PollJournals() {
    struct Update {
        uint8_t volume;
        bool remove;
        uint8_t flags;
        uint64_t id;
        uint64_t parent;
        std::string name;
    };
    std::vector<Update> updates;
    std::vector<BYTE> buffer(kJournalBufferBytes);
    bool healthy = true;
    for (Journal& journal : m_journals) {
        READ_USN_JOURNAL_DATA_V0 read{};
        read.StartUsn = journal.nextUsn;
        read.ReasonMask = kJournalReasons;
        read.UsnJournalID = journal.journalId;
        bool switched = false;
        for (int batch = 0; batch < 256 && !m_stop.load(); ++batch) {
            DWORD bytes = 0;
            const DWORD code = journal.privileged ? FSCTL_READ_USN_JOURNAL : FSCTL_READ_UNPRIVILEGED_USN_JOURNAL;
            if (DeviceIoControl(journal.handle, code, &read, sizeof(read), buffer.data(), kJournalBufferBytes,
                    &bytes, nullptr) == FALSE) {
                const DWORD error = GetLastError();
                if (error == ERROR_JOURNAL_ENTRY_DELETED || error == ERROR_JOURNAL_NOT_ACTIVE ||
                    error == ERROR_JOURNAL_DELETE_IN_PROGRESS) {
                    healthy = false;
                } else if (!switched && (error == ERROR_INVALID_PARAMETER || error == ERROR_ACCESS_DENIED ||
                               error == ERROR_INVALID_FUNCTION)) {
                    journal.privileged = !journal.privileged;
                    switched = true;
                    continue;
                }
                break;
            }
            if (bytes <= sizeof(USN)) {
                break;
            }
            ForEachUsnRecord(buffer.data() + sizeof(USN), bytes - sizeof(USN),
                [&](uint64_t id, uint64_t parent, DWORD attributes, DWORD reason, const char16_t* name, size_t length) {
                    Update update{journal.volume, (reason & USN_REASON_FILE_DELETE) != 0,
                        FlagsFromAttributes(attributes), id, parent, {}};
                    if (!update.remove) {
                        filesearch::AppendUtf8(update.name, name, length);
                    }
                    updates.push_back(std::move(update));
                });
            USN next = 0;
            std::memcpy(&next, buffer.data(), sizeof(next));
            read.StartUsn = next;
            journal.nextUsn = next;
            if (bytes < kJournalBufferBytes / 2) {
                break;
            }
        }
    }
    if (!updates.empty()) {
        std::unique_lock lock(m_indexLock);
        if (m_index != nullptr) {
            for (const Update& update : updates) {
                if (update.remove) {
                    m_index->Remove(update.volume, update.id);
                } else {
                    m_index->Upsert(update.volume, update.id, update.parent, update.flags, update.name);
                }
            }
            m_index->CompactIfWasteful();
            std::lock_guard statusLock(m_statusLock);
            m_status.entries = m_index->LiveEntryCount();
        }
    }
    return healthy;
}

void FileSearchService::IndexThread() {
    while (!m_stop.load()) {
        {
            std::lock_guard lock(m_statusLock);
            m_status.indexing = true;
        }
        NotifyStatus();
        m_rebuild.store(false);
        CloseJournals();
        const auto started = std::chrono::steady_clock::now();
        std::unique_ptr<FileIndex> index;
        std::vector<Journal> journals;
        FileSearchMode mode = FileSearchMode::Starting;
        // Low I/O + CPU priority: indexing must never compete with the user.
        const bool background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
        const bool built = BuildIndex(index, journals, mode);
        if (background) {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        }
        if (m_stop.load()) {
            for (Journal& journal : journals) {
                CloseHandle(journal.handle);
            }
            break;
        }
        const double buildMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        m_journals = std::move(journals);
        {
            std::unique_lock lock(m_indexLock);
            m_index = built ? std::move(index) : nullptr;
            std::lock_guard statusLock(m_statusLock);
            m_status.mode = built ? mode : FileSearchMode::Starting;
            m_status.indexing = false;
            m_status.entries = m_index != nullptr ? m_index->LiveEntryCount() : 0;
            m_status.memoryBytes = m_index != nullptr ? m_index->MemoryBytes() : 0;
            m_status.buildMs = buildMs;
        }
        NotifyStatus();
        if (!built) {
            // Nothing indexable right now (e.g. profile unavailable): retry later.
            WaitForSingleObject(m_wake, kRetryBuildMs);
            continue;
        }
        // Catch up on changes made while building, then follow the journal.
        while (!m_stop.load() && !m_rebuild.load()) {
            if (!PollJournals()) {
                m_rebuild.store(true);
                break;
            }
            WaitForSingleObject(m_wake, m_active.load() ? kActivePollMs : kIdlePollMs);
        }
    }
    CloseJournals();
}

void FileSearchService::SearchThread() {
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2U, 1U, 4U);
    filesearch::SearchState state;
    for (;;) {
        Request request;
        {
            std::unique_lock lock(m_requestLock);
            m_requestReady.wait(lock, [this] { return m_hasRequest || m_stop.load(); });
            if (m_stop.load()) {
                return;
            }
            request = std::move(m_request);
            m_hasRequest = false;
        }
        auto reply = std::make_unique<FileSearchReply>();
        reply->requestId = request.id;
        reply->query = request.query;
        const auto started = std::chrono::steady_clock::now();
        const std::string query = ToUtf8(request.query);
        {
            std::shared_lock lock(m_indexLock);
            for (const filesearch::Hit& hit : m_apps.Search(query, request.appLimit)) {
                FileSearchItem item;
                item.kind = FileSearchItem::Kind::App;
                item.appIndex = hit.entry - 1;
                item.name = FromUtf8(m_apps.Name(hit.entry));
                reply->apps.push_back(std::move(item));
            }
            if (m_index != nullptr) {
                for (const filesearch::Hit& hit : m_index->Search(query, request.fileLimit, &state, threads)) {
                    FileSearchItem item;
                    item.kind = (m_index->Flags(hit.entry) & filesearch::kFlagDirectory) != 0
                        ? FileSearchItem::Kind::Folder
                        : FileSearchItem::Kind::File;
                    item.name = FromUtf8(m_index->Name(hit.entry));
                    item.path = FromUtf8(m_index->Path(hit.entry));
                    reply->files.push_back(std::move(item));
                }
                reply->indexedEntries = m_index->LiveEntryCount();
            }
        }
        reply->searchMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        {
            std::lock_guard lock(m_statusLock);
            reply->indexing = m_status.indexing;
        }
        const auto superseded = [this] {
            std::lock_guard lock(m_requestLock);
            return m_hasRequest;
        };
        const auto post = [&request](std::unique_ptr<FileSearchReply> message) {
            FileSearchReply* raw = message.release();
            if (PostMessageW(request.reply, request.message, 0, reinterpret_cast<LPARAM>(raw)) == FALSE) {
                delete raw;
            }
        };
        // Names first: size/date need file-system round trips (cold disks,
        // cloud placeholders), which must never delay the results.
        if (superseded() || request.reply == nullptr) {
            continue;
        }
        auto details = std::make_unique<FileSearchReply>(*reply);
        post(std::move(reply));
        details->metadata = true;
        bool interrupted = false;
        for (FileSearchItem& item : details->files) {
            if (superseded()) {
                interrupted = true;
                break;
            }
            if (item.kind == FileSearchItem::Kind::File) {
                FillMetadata(item);
            }
        }
        if (!interrupted) {
            post(std::move(details));
        }
    }
}

bool FileSearchService::RequestMftAccess() {
    const std::wstring sid = CurrentUserSid();
    const std::wstring exe = ModulePath();
    if (sid.empty() || exe.empty()) {
        return false;
    }
    const std::wstring parameters = L"--register-mft-task " + sid;
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (ShellExecuteExW(&info) == FALSE || info.hProcess == nullptr) {
        return false;
    }
    WaitForSingleObject(info.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code == 0 && MftTaskExists(sid);
}

// ---- elevated entry points -------------------------------------------------------------

int RunMftDumpHelper() noexcept {
    try {
        const std::wstring sid = CurrentUserSid();
        if (sid.empty()) {
            return 2;
        }
        const std::wstring pipeName = std::wstring(kPipePrefix) + sid;
        if (WaitNamedPipeW(pipeName.c_str(), 10000) == FALSE) {
            return 3;
        }
        Handle pipe(CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (!pipe.Valid()) {
            return 4;
        }
        // Only hand the file-name listing to a process of the same user.
        ULONG serverPid = 0;
        if (GetNamedPipeServerProcessId(pipe.Get(), &serverPid) == FALSE || ProcessUserSid(serverPid) != sid) {
            return 5;
        }
        const uint32_t magic = kDumpMagic;
        if (!WriteChunk(pipe.Get(), 'H', L'\0', &magic, static_cast<uint32_t>(sizeof(magic)))) {
            return 6;
        }
        for (const wchar_t letter : FixedNtfsVolumes()) {
            bool writeFailed = false;
            DWORD error = 0;
            const bool ok = EnumerateMft(
                letter,
                [&](const DumpVolume& header) {
                    writeFailed = !WriteChunk(pipe.Get(), 'V', letter, &header, static_cast<uint32_t>(sizeof(header)));
                },
                [&](const BYTE* data, size_t size) {
                    writeFailed = writeFailed || !WriteChunk(pipe.Get(), 'R', letter, data, static_cast<uint32_t>(size));
                    return !writeFailed;
                },
                error);
            if (writeFailed) {
                return 7;
            }
            if (!WriteChunk(pipe.Get(), ok ? 'E' : 'X', letter, &error, ok ? 0U : static_cast<uint32_t>(sizeof(error)))) {
                return 7;
            }
        }
        WriteChunk(pipe.Get(), 'Z', L'\0', nullptr, 0);
        FlushFileBuffers(pipe.Get());
        return 0;
    } catch (...) {
        return 1;
    }
}

int RegisterMftTask(const wchar_t* userSid) noexcept {
    try {
        if (userSid == nullptr || !IsElevated()) {
            return 2;
        }
        PSID parsed = nullptr;
        if (ConvertStringSidToSidW(userSid, &parsed) == FALSE) {
            return 2;
        }
        LocalFree(parsed);
        const std::wstring sid(userSid);
        const std::wstring exe = HelperPath();
        const std::wstring self = ModulePath();
        if (exe.empty() || self.empty()) {
            return 3;
        }
        const std::wstring directory = exe.substr(0, exe.find_last_of(L'\\'));
        CreateDirectoryW(directory.c_str(), nullptr);
        // A running helper keeps the old copy locked; it still speaks the protocol.
        if (CopyFileW(self.c_str(), exe.c_str(), FALSE) == FALSE &&
            GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return 6;
        }
        TaskFolder folder;
        if (folder.Folder() == nullptr) {
            return 3;
        }
        ITaskDefinition* definition = nullptr;
        if (FAILED(folder.Service()->NewTask(0, &definition)) || definition == nullptr) {
            return 3;
        }
        int result = 4;
        IRegistrationInfo* registration = nullptr;
        IPrincipal* principal = nullptr;
        ITaskSettings* settings = nullptr;
        IActionCollection* actions = nullptr;
        IAction* action = nullptr;
        IExecAction* exec = nullptr;
        auto bstr = [](const std::wstring& text) { return SysAllocString(text.c_str()); };
        do {
            if (SUCCEEDED(definition->get_RegistrationInfo(&registration)) && registration != nullptr) {
                BSTR author = bstr(L"Hoverdock");
                BSTR description = bstr(L"Reads file names from the NTFS Master File Table for Hoverdock Search. "
                                        L"Runs only when Hoverdock starts indexing, then exits.");
                static_cast<void>(registration->put_Author(author));
                static_cast<void>(registration->put_Description(description));
                SysFreeString(author);
                SysFreeString(description);
            }
            if (FAILED(definition->get_Principal(&principal)) || principal == nullptr) {
                break;
            }
            BSTR user = bstr(sid);
            static_cast<void>(principal->put_UserId(user));
            SysFreeString(user);
            static_cast<void>(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));
            static_cast<void>(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST));
            if (FAILED(definition->get_Settings(&settings)) || settings == nullptr) {
                break;
            }
            static_cast<void>(settings->put_Enabled(VARIANT_TRUE));
            static_cast<void>(settings->put_AllowDemandStart(VARIANT_TRUE));
            static_cast<void>(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));
            static_cast<void>(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
            static_cast<void>(settings->put_Hidden(VARIANT_TRUE));
            static_cast<void>(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));
            BSTR limit = bstr(L"PT10M");
            static_cast<void>(settings->put_ExecutionTimeLimit(limit));
            SysFreeString(limit);
            if (FAILED(definition->get_Actions(&actions)) || actions == nullptr ||
                FAILED(actions->Create(TASK_ACTION_EXEC, &action)) || action == nullptr ||
                FAILED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&exec))) ||
                exec == nullptr) {
                break;
            }
            BSTR path = bstr(exe);
            BSTR arguments = bstr(L"--mft-dump");
            static_cast<void>(exec->put_Path(path));
            static_cast<void>(exec->put_Arguments(arguments));
            SysFreeString(path);
            SysFreeString(arguments);

            BSTR name = bstr(TaskName(sid));
            VARIANT empty{};
            VariantInit(&empty);
            // Admins/SYSTEM own it; the user may read, run and delete it, never modify it.
            const std::wstring sddl = L"D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;GRGXSD;;;" + sid + L")";
            VARIANT security{};
            VariantInit(&security);
            security.vt = VT_BSTR;
            security.bstrVal = bstr(sddl);
            IRegisteredTask* registered = nullptr;
            HRESULT hr = folder.Folder()->RegisterTaskDefinition(name, definition, TASK_CREATE_OR_UPDATE, empty,
                empty, TASK_LOGON_INTERACTIVE_TOKEN, security, &registered);
            if (FAILED(hr)) {
                hr = folder.Folder()->RegisterTaskDefinition(name, definition, TASK_CREATE_OR_UPDATE, empty, empty,
                    TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered);
            }
            VariantClear(&security);
            SysFreeString(name);
            if (registered != nullptr) {
                registered->Release();
            }
            result = SUCCEEDED(hr) ? 0 : 5;
        } while (false);
        if (exec != nullptr) {
            exec->Release();
        }
        if (action != nullptr) {
            action->Release();
        }
        if (actions != nullptr) {
            actions->Release();
        }
        if (settings != nullptr) {
            settings->Release();
        }
        if (principal != nullptr) {
            principal->Release();
        }
        if (registration != nullptr) {
            registration->Release();
        }
        definition->Release();
        return result;
    } catch (...) {
        return 1;
    }
}
