#include "NotificationBadges.h"

#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// Directory change notifications deliver promptly for size/name changes, but
// NTFS can defer last-write updates for files held open (the WAL is rewritten
// in place after a checkpoint). The data_version poll covers that case; it
// only reads the WAL index in shared memory, so each tick costs microseconds.
constexpr DWORD kPollIntervalMs = 3000;
// The notification service writes in short bursts (toast + badge + tile).
constexpr DWORD kDebounceMs = 120;
constexpr DWORD kRetryIntervalMs = 30000;
constexpr long long kUnixEpochFileTime = 116444736000000000LL;

// Minimal winsqlite3 surface. Loaded at runtime from System32 so a missing or
// broken DLL only disables badges instead of blocking startup.
struct Sqlite3;
struct Sqlite3Stmt;
using Sqlite3Int64 = long long;

constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadOnly = 0x00000001;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenNoMutex = 0x00008000;

struct SqliteApi {
    using OpenV2 = int(__stdcall*)(const char*, Sqlite3**, int, const char*);
    using CloseV2 = int(__stdcall*)(Sqlite3*);
    using PrepareV2 = int(__stdcall*)(Sqlite3*, const char*, int, Sqlite3Stmt**, const char**);
    using Step = int(__stdcall*)(Sqlite3Stmt*);
    using Reset = int(__stdcall*)(Sqlite3Stmt*);
    using Finalize = int(__stdcall*)(Sqlite3Stmt*);
    using ColumnBlob = const void*(__stdcall*)(Sqlite3Stmt*, int);
    using ColumnBytes = int(__stdcall*)(Sqlite3Stmt*, int);
    using ColumnInt64 = Sqlite3Int64(__stdcall*)(Sqlite3Stmt*, int);
    using BusyTimeout = int(__stdcall*)(Sqlite3*, int);

    HMODULE module = nullptr;
    OpenV2 open = nullptr;
    CloseV2 close = nullptr;
    PrepareV2 prepare = nullptr;
    Step step = nullptr;
    Reset reset = nullptr;
    Finalize finalize = nullptr;
    ColumnBlob columnBlob = nullptr;
    ColumnBlob columnText16 = nullptr;
    ColumnBytes columnBytes = nullptr;
    ColumnBytes columnBytes16 = nullptr;
    ColumnInt64 columnInt64 = nullptr;
    BusyTimeout busyTimeout = nullptr;

    template <typename T>
    bool Resolve(T& slot, const char* name) noexcept {
        slot = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
        return slot != nullptr;
    }

    bool Load() noexcept {
        module = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            return false;
        }
        const bool ok = Resolve(open, "sqlite3_open_v2") && Resolve(close, "sqlite3_close_v2") &&
            Resolve(prepare, "sqlite3_prepare_v2") && Resolve(step, "sqlite3_step") &&
            Resolve(reset, "sqlite3_reset") && Resolve(finalize, "sqlite3_finalize") &&
            Resolve(columnBlob, "sqlite3_column_blob") &&
            Resolve(columnText16, "sqlite3_column_text16") &&
            Resolve(columnBytes, "sqlite3_column_bytes") &&
            Resolve(columnBytes16, "sqlite3_column_bytes16") &&
            Resolve(columnInt64, "sqlite3_column_int64") &&
            Resolve(busyTimeout, "sqlite3_busy_timeout");
        if (!ok) {
            Unload();
        }
        return ok;
    }

    void Unload() noexcept {
        if (module != nullptr) {
            FreeLibrary(module);
            module = nullptr;
        }
    }
};

// Badges and toasts. Latest badge per handler wins (ArrivalTime ascending,
// later rows overwrite); toasts are counted per handler.
constexpr char kBadgeQuery[] =
    "SELECT h.PrimaryId, n.Type, n.Payload, n.ExpiryTime, n.ArrivalTime FROM Notification n "
    "JOIN NotificationHandler h ON h.RecordId = n.HandlerId "
    "WHERE n.Type IN ('badge', 'toast') ORDER BY n.ArrivalTime";

std::wstring LowerAumid(std::wstring aumid) {
    for (wchar_t& c : aumid) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return aumid;
}

// Toasts arriving after this FILETIME count as unread for an app.
using SeenMap = std::unordered_map<std::wstring, long long>;

class BadgeStore {
public:
    explicit BadgeStore(const SqliteApi& api) noexcept : m_api(api) {}
    BadgeStore(const BadgeStore&) = delete;
    BadgeStore& operator=(const BadgeStore&) = delete;
    ~BadgeStore() { Close(); }

    bool Open(const std::wstring& path, bool readOnly) {
        Close();
        const int length = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0) {
            return false;
        }
        std::string utf8(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), length, nullptr, nullptr);
        const int access = readOnly ? kSqliteOpenReadOnly : kSqliteOpenReadWrite;
        if (m_api.open(utf8.c_str(), &m_db, access | kSqliteOpenNoMutex, nullptr) != kSqliteOk) {
            Close();
            return false;
        }
        m_api.busyTimeout(m_db, 250);
        if (m_api.prepare(m_db, "PRAGMA data_version", -1, &m_version, nullptr) != kSqliteOk ||
            m_api.prepare(m_db, kBadgeQuery, -1, &m_query, nullptr) != kSqliteOk) {
            Close();
            return false;
        }
        return true;
    }

    void Close() noexcept {
        if (m_version != nullptr) {
            m_api.finalize(m_version);
            m_version = nullptr;
        }
        if (m_query != nullptr) {
            m_api.finalize(m_query);
            m_query = nullptr;
        }
        if (m_db != nullptr) {
            m_api.close(m_db);
            m_db = nullptr;
        }
    }

    [[nodiscard]] bool IsOpen() const noexcept { return m_db != nullptr; }

    // Changes whenever another connection (the notification service) commits.
    bool DataVersion(Sqlite3Int64& version) {
        const bool ok = m_api.step(m_version) == kSqliteRow;
        if (ok) {
            version = m_api.columnInt64(m_version, 0);
        }
        m_api.reset(m_version);
        return ok;
    }

    // nextExpiry receives the earliest future ExpiryTime (FILETIME), 0 if none.
    bool ReadBadges(std::vector<NotificationBadges::Badge>& out, Sqlite3Int64& nextExpiry, const SeenMap& seen,
        Sqlite3Int64 since);

private:
    const SqliteApi& m_api;
    Sqlite3* m_db = nullptr;
    Sqlite3Stmt* m_version = nullptr;
    Sqlite3Stmt* m_query = nullptr;
};

Sqlite3Int64 FileTimeNow() noexcept {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return static_cast<Sqlite3Int64>((static_cast<ULONGLONG>(now.dwHighDateTime) << 32U) | now.dwLowDateTime);
}

// Payloads are UTF-8 XML (<badge value="3"/>); tolerate UTF-16 too.
std::wstring PayloadText(const void* data, int bytes) {
    if (data == nullptr || bytes <= 0) {
        return {};
    }
    const auto* raw = static_cast<const unsigned char*>(data);
    const size_t size = static_cast<size_t>(bytes);
    if (size >= 2 && (raw[1] == 0 || (raw[0] == 0xFF && raw[1] == 0xFE))) {
        std::wstring text(reinterpret_cast<const wchar_t*>(raw), size / sizeof(wchar_t));
        if (!text.empty() && text.front() == L'\uFEFF') {
            text.erase(0, 1);
        }
        return text;
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(raw), bytes, nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(raw), bytes, text.data(), length);
    return text;
}

// value attribute of the <badge> element; empty when absent.
std::wstring BadgeValue(const std::wstring& xml) {
    const size_t element = xml.find(L"<badge");
    if (element == std::wstring::npos) {
        return {};
    }
    const size_t end = xml.find(L'>', element);
    size_t position = element + 6;
    while (true) {
        position = xml.find(L"value", position);
        if (position == std::wstring::npos || (end != std::wstring::npos && position > end)) {
            return {};
        }
        const wchar_t before = xml[position - 1];
        size_t cursor = position + 5;
        position = cursor;
        if (std::iswspace(before) == 0) {
            continue;
        }
        while (cursor < xml.size() && std::iswspace(xml[cursor]) != 0) {
            ++cursor;
        }
        if (cursor >= xml.size() || xml[cursor] != L'=') {
            continue;
        }
        ++cursor;
        while (cursor < xml.size() && std::iswspace(xml[cursor]) != 0) {
            ++cursor;
        }
        if (cursor >= xml.size() || (xml[cursor] != L'"' && xml[cursor] != L'\'')) {
            return {};
        }
        const wchar_t quote = xml[cursor];
        const size_t close = xml.find(quote, cursor + 1);
        if (close == std::wstring::npos) {
            return {};
        }
        return xml.substr(cursor + 1, close - cursor - 1);
    }
}

bool BadgeStore::ReadBadges(std::vector<NotificationBadges::Badge>& out, Sqlite3Int64& nextExpiry,
    const SeenMap& seen, Sqlite3Int64 since) {
    out.clear();
    nextExpiry = 0;
    const Sqlite3Int64 now = FileTimeNow();
    // Apps that ever published a badge row manage their own unread state;
    // their toasts never produce a count.
    std::unordered_map<std::wstring, bool> badgeApps;
    std::unordered_map<std::wstring, std::pair<std::wstring, int>> toastCounts;
    int result = kSqliteOk;
    while ((result = m_api.step(m_query)) == kSqliteRow) {
        const auto* id = static_cast<const wchar_t*>(m_api.columnText16(m_query, 0));
        const int idBytes = m_api.columnBytes16(m_query, 0);
        if (id == nullptr || idBytes <= 0) {
            continue;
        }
        std::wstring aumid(id, static_cast<size_t>(idBytes) / sizeof(wchar_t));
        const auto* type = static_cast<const wchar_t*>(m_api.columnText16(m_query, 1));
        const bool isBadge = type != nullptr && CompareStringOrdinal(type, -1, L"badge", -1, TRUE) == CSTR_EQUAL;
        // FILETIME; anything before 1970 is not a real deadline (0 = never).
        Sqlite3Int64 expiry = m_api.columnInt64(m_query, 3);
        if (expiry < kUnixEpochFileTime) {
            expiry = 0;
        }
        const bool live = expiry == 0 || expiry > now;
        if (!isBadge) {
            const Sqlite3Int64 arrival = m_api.columnInt64(m_query, 4);
            std::wstring key = LowerAumid(aumid);
            Sqlite3Int64 floor = since;
            if (const auto found = seen.find(key); found != seen.end()) {
                floor = std::max(floor, static_cast<Sqlite3Int64>(found->second));
            }
            if (live && arrival > floor) {
                if (expiry != 0 && (nextExpiry == 0 || expiry < nextExpiry)) {
                    nextExpiry = expiry;
                }
                auto& entry = toastCounts[std::move(key)];
                if (entry.first.empty()) {
                    entry.first = std::move(aumid);
                }
                ++entry.second;
            }
            continue;
        }
        badgeApps[LowerAumid(aumid)] = true;
        const void* payload = m_api.columnBlob(m_query, 2);
        const int payloadBytes = m_api.columnBytes(m_query, 2);

        const auto existing = std::ranges::find_if(out,
            [&aumid](const NotificationBadges::Badge& badge) {
                return CompareStringOrdinal(badge.aumid.c_str(), static_cast<int>(badge.aumid.size()),
                    aumid.c_str(), static_cast<int>(aumid.size()), TRUE) == CSTR_EQUAL;
            });
        std::wstring text;
        if (!live || !NotificationBadges::DisplayTextForValue(
                BadgeValue(PayloadText(payload, payloadBytes)), text)) {
            if (existing != out.end()) {
                out.erase(existing);
            }
            continue;
        }
        if (expiry != 0 && (nextExpiry == 0 || expiry < nextExpiry)) {
            nextExpiry = expiry;
        }
        if (existing != out.end()) {
            existing->text = std::move(text);
        } else {
            out.push_back({std::move(aumid), std::move(text), false});
        }
    }
    m_api.reset(m_query);
    if (result != kSqliteDone) {
        return false;
    }
    for (auto& [key, entry] : toastCounts) {
        if (badgeApps.contains(key) || entry.second <= 0) {
            continue;
        }
        std::wstring text;
        if (NotificationBadges::DisplayTextForValue(std::to_wstring(entry.second), text)) {
            out.push_back({std::move(entry.first), std::move(text), true});
        }
    }
    std::ranges::sort(out, {}, &NotificationBadges::Badge::aumid);
    return true;
}

std::wstring NotificationsDirectory() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData))) {
        return {};
    }
    std::wstring path(localAppData);
    CoTaskMemFree(localAppData);
    return path + L"\\Microsoft\\Windows\\Notifications";
}

// Mirrors Settings > Personalization > Taskbar > "Show badges on taskbar apps".
bool TaskbarBadgesEnabled() noexcept {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarBadges",
            RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return true;
    }
    return value != 0;
}

// Size + last-write of the database and its WAL: the snapshot fallback's
// cheap "did anything change" check (no data_version across fresh copies).
struct StoreSignature {
    ULONGLONG databaseSize = 0;
    ULONGLONG databaseWrite = 0;
    ULONGLONG walSize = 0;
    ULONGLONG walWrite = 0;

    [[nodiscard]] bool operator==(const StoreSignature&) const = default;
};

void ReadFileStamp(const std::wstring& path, ULONGLONG& size, ULONGLONG& write) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) {
        size = 0;
        write = 0;
        return;
    }
    size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32U) | data.nFileSizeLow;
    write = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32U) |
        data.ftLastWriteTime.dwLowDateTime;
}

StoreSignature ReadStoreSignature(const std::wstring& databasePath) noexcept {
    StoreSignature signature;
    ReadFileStamp(databasePath, signature.databaseSize, signature.databaseWrite);
    ReadFileStamp(databasePath + L"-wal", signature.walSize, signature.walWrite);
    return signature;
}

void DeleteSnapshot(const std::wstring& snapshotPath) noexcept {
    DeleteFileW(snapshotPath.c_str());
    DeleteFileW((snapshotPath + L"-wal").c_str());
    DeleteFileW((snapshotPath + L"-shm").c_str());
}

// The service keeps the store open for writing, so the source must be opened
// with full sharing (CopyFile's share mode can be refused).
bool CopySharedFile(const std::wstring& source, const std::wstring& destination) noexcept {
    HANDLE input = CreateFileW(source.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        return false;
    }
    HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        CloseHandle(input);
        return false;
    }
    // Worker-thread stack (1 MB default); keeps this path allocation-free.
    std::array<char, 64U * 1024U> buffer;
    bool ok = true;
    while (true) {
        DWORD read = 0;
        if (ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == FALSE) {
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        DWORD written = 0;
        if (WriteFile(output, buffer.data(), read, &written, nullptr) == FALSE || written != read) {
            ok = false;
            break;
        }
    }
    CloseHandle(output);
    CloseHandle(input);
    return ok;
}

// Private copy of the store (plus its WAL, which SQLite replays on open) for
// when the live file cannot be opened or read in place.
bool CopySnapshot(const std::wstring& databasePath, const std::wstring& snapshotPath) noexcept {
    DeleteSnapshot(snapshotPath);
    if (!CopySharedFile(databasePath, snapshotPath)) {
        return false;
    }
    static_cast<void>(CopySharedFile(databasePath + L"-wal", snapshotPath + L"-wal"));
    return true;
}

std::wstring SnapshotPath() {
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    if (length == 0 || length > MAX_PATH) {
        return {};
    }
    std::wstring path(temp, length);
    path += L"hoverdock-badges.db";
    return path;
}

}  // namespace

NotificationBadges::~NotificationBadges() {
    Stop();
}

bool NotificationBadges::DisplayTextForValue(const std::wstring& value, std::wstring& text) {
    text.clear();
    if (value.empty()) {
        return false;
    }
    if (std::ranges::all_of(value, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
        const std::wstring_view digits = std::wstring_view(value).substr(
            std::min(value.find_first_not_of(L'0'), value.size()));
        if (digits.empty()) {
            return false;
        }
        text = digits.size() > 2 ? L"99+" : std::wstring(digits);
        return true;
    }
    const auto is = [&value](const wchar_t* glyph) {
        return CompareStringOrdinal(value.c_str(), -1, glyph, -1, TRUE) == CSTR_EQUAL;
    };
    if (is(L"alert") || is(L"alarm") || is(L"attention") || is(L"error")) {
        text = L"!";
        return true;
    }
    // Plain dot. Presence (available/away/busy/unavailable) and media
    // (playing/paused) glyphs are status, not unread state, so they show nothing.
    return is(L"newMessage") || is(L"activity");
}

void NotificationBadges::Start(HWND notifyWindow, UINT notifyMessage) {
    Stop();
    m_notifyWindow = notifyWindow;
    m_notifyMessage = notifyMessage;
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_stopEvent == nullptr) {
        return;
    }
    {
        const std::lock_guard lock(m_seenMutex);
        m_since = FileTimeNow();
        m_seen.clear();
    }
    m_thread = CreateThread(nullptr, 0, &NotificationBadges::ThreadProc, this, 0, nullptr);
    if (m_thread == nullptr) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
        return;
    }
    SetThreadPriority(m_thread, THREAD_PRIORITY_BELOW_NORMAL);
}

void NotificationBadges::Stop() noexcept {
    if (m_stopEvent != nullptr) {
        SetEvent(m_stopEvent);
    }
    if (m_thread != nullptr) {
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_stopEvent != nullptr) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
    if (m_wakeEvent != nullptr) {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
    m_notifyWindow = nullptr;
    m_notifyMessage = 0;
}

void NotificationBadges::MarkSeen(const std::wstring& aumid) {
    if (aumid.empty()) {
        return;
    }
    {
        const std::lock_guard lock(m_seenMutex);
        m_seen[LowerAumid(aumid)] = FileTimeNow();
    }
    m_forceRead.store(true);
    if (m_wakeEvent != nullptr) {
        SetEvent(m_wakeEvent);
    }
}

std::vector<NotificationBadges::Badge> NotificationBadges::Snapshot() const {
    const std::lock_guard lock(m_mutex);
    return m_badges;
}

void NotificationBadges::Publish(std::vector<Badge> badges) {
    {
        const std::lock_guard lock(m_mutex);
        if (badges == m_badges) {
            return;
        }
        m_badges = std::move(badges);
    }
    if (m_notifyWindow != nullptr) {
        PostMessageW(m_notifyWindow, m_notifyMessage, 0, 0);
    }
}

DWORD WINAPI NotificationBadges::ThreadProc(LPVOID param) {
    static_cast<NotificationBadges*>(param)->WorkerLoop();
    return 0;
}

void NotificationBadges::WorkerLoop() {
    SqliteApi api;
    const std::wstring directory = NotificationsDirectory();
    if (directory.empty() || !api.Load()) {
        return;
    }
    const std::wstring databasePath = directory + L"\\wpndatabase.db";
    HANDLE change = FindFirstChangeNotificationW(directory.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_FILE_NAME);
    if (change == INVALID_HANDLE_VALUE) {
        change = nullptr;
    }

    const std::wstring snapshotPath = SnapshotPath();
    bool snapshotMode = false;
    {
        BadgeStore store(api);
        Sqlite3Int64 lastVersion = -1;
        Sqlite3Int64 nextExpiry = 0;
        StoreSignature lastSignature;
        bool haveSignature = false;
        int liveFailures = 0;
        std::vector<Badge> badges;
        DWORD wait = 0;
        while (true) {
            // stop, MarkSeen wake, then the directory change handle (optional).
            HANDLE handles[3]{m_stopEvent};
            DWORD count = 1;
            DWORD changeIndex = MAXDWORD;
            if (m_wakeEvent != nullptr) {
                handles[count++] = m_wakeEvent;
            }
            if (change != nullptr) {
                changeIndex = count;
                handles[count++] = change;
            }
            const DWORD signaled = WaitForMultipleObjects(count, handles, FALSE, wait);
            if (signaled == WAIT_OBJECT_0) {
                break;
            }
            if (changeIndex != MAXDWORD && signaled == WAIT_OBJECT_0 + changeIndex) {
                if (FindNextChangeNotification(change) == FALSE) {
                    FindCloseChangeNotification(change);
                    change = nullptr;
                }
                if (WaitForSingleObject(m_stopEvent, kDebounceMs) == WAIT_OBJECT_0) {
                    break;
                }
            }

            wait = kPollIntervalMs;
            if (!TaskbarBadgesEnabled()) {
                lastVersion = -1;
                haveSignature = false;
                Publish({});
                continue;
            }
            // MarkSeen changes the result without a database write.
            const bool expired = (nextExpiry != 0 && FileTimeNow() >= nextExpiry) || m_forceRead.exchange(false);
            SeenMap seen;
            Sqlite3Int64 since = 0;
            {
                const std::lock_guard lock(m_seenMutex);
                seen = m_seen;
                since = m_since;
            }

            if (snapshotMode) {
                const StoreSignature signature = ReadStoreSignature(databasePath);
                if (haveSignature && signature == lastSignature && !expired) {
                    continue;
                }
                const bool read = CopySnapshot(databasePath, snapshotPath) &&
                    store.Open(snapshotPath, false) && store.ReadBadges(badges, nextExpiry, seen, since);
                store.Close();
                if (!read) {
                    haveSignature = false;
                    wait = kRetryIntervalMs;
                    continue;
                }
                lastSignature = signature;
                haveSignature = true;
                Publish(badges);
                continue;
            }

            // Live read-only connection: no copies, data_version gates queries.
            Sqlite3Int64 version = 0;
            const bool opened = store.IsOpen() || store.Open(databasePath, true);
            const bool versioned = opened && store.DataVersion(version);
            if (versioned && version == lastVersion && !expired) {
                liveFailures = 0;
                continue;
            }
            if (versioned && store.ReadBadges(badges, nextExpiry, seen, since)) {
                liveFailures = 0;
                lastVersion = version;
                Publish(badges);
                continue;
            }
            store.Close();
            lastVersion = -1;
            // A transient SQLITE_BUSY retries live; repeated failures (the
            // service holding the file exclusively) switch to snapshots.
            if (!opened || ++liveFailures >= 3) {
                snapshotMode = !snapshotPath.empty();
                wait = snapshotMode ? 0U : kRetryIntervalMs;
            }
        }
    }
    if (snapshotMode) {
        DeleteSnapshot(snapshotPath);
    }

    if (change != nullptr) {
        FindCloseChangeNotification(change);
    }
    api.Unload();
}
