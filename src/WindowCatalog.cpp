#include "WindowCatalog.h"
#include "Profile.h"

#include <ShObjIdl.h>
#include <Shellapi.h>
#include <appmodel.h>
#include <dwmapi.h>
#include <propkey.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <vector>

#include <winver.h>

namespace {

constexpr wchar_t kAppsFolderPrefix[] = L"shell:AppsFolder\\";
constexpr size_t kMaximumPathLength = 32768;

bool EqualInsensitive(const std::wstring& left, const std::wstring& right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](wchar_t lhs, wchar_t rhs) {
            return std::towlower(lhs) == std::towlower(rhs);
        });
}

std::wstring FileNameWithoutExtension(const std::wstring& path) {
    const std::filesystem::path file(path);
    return file.stem().wstring();
}

bool IsTitleWordCharacter(wchar_t character) {
    return std::iswalnum(character) != 0 || character == L'_';
}

bool IsPictureInPictureTitle(const std::wstring& title) {
    std::wstring lowercase(title);
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    if (lowercase.find(L"picture-in-picture") != std::wstring::npos ||
        lowercase.find(L"picture in picture") != std::wstring::npos) {
        return true;
    }

    size_t position = lowercase.find(L"pip");
    while (position != std::wstring::npos) {
        const bool startsWord = position == 0 || !IsTitleWordCharacter(lowercase[position - 1]);
        const size_t after = position + 3;
        const bool endsWord = after == lowercase.size() || !IsTitleWordCharacter(lowercase[after]);
        if (startsWord && endsWord) {
            return true;
        }
        position = lowercase.find(L"pip", after);
    }
    return false;
}

bool IsSmallAuxiliaryWindow(HWND window, LONG_PTR style, LONG_PTR extendedStyle) {
    if ((style & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW ||
        (extendedStyle & WS_EX_APPWINDOW) != 0) {
        return false;
    }

    RECT bounds{};
    if (GetWindowRect(window, &bounds) == FALSE) {
        return false;
    }
    const LONG width = bounds.right - bounds.left;
    const LONG height = bounds.bottom - bounds.top;
    return width < 240 || height < 180;
}

bool EndsWithInsensitive(const std::wstring& value, std::wstring_view suffix) {
    return value.size() >= suffix.size() &&
        EqualInsensitive(value.substr(value.size() - suffix.size()), std::wstring(suffix));
}

bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }

    for (size_t index = 0; index + needle.size() <= haystack.size(); ++index) {
        if (EqualInsensitive(haystack.substr(index, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

bool PathMatchesPackageFamily(const std::wstring& normalizedPath,
    const std::wstring& packageFamily) {
    if (packageFamily.empty()) {
        return false;
    }

    const size_t underscore = packageFamily.rfind(L'_');
    if (underscore == std::wstring::npos || underscore + 1 >= packageFamily.size()) {
        return ContainsInsensitive(normalizedPath, packageFamily);
    }

    const std::wstring packageName = packageFamily.substr(0, underscore);
    const std::wstring publisherId = packageFamily.substr(underscore + 1);
    return ContainsInsensitive(normalizedPath, packageName) &&
        ContainsInsensitive(normalizedPath, publisherId);
}

bool AumidMatchesPackageFamily(const std::wstring& aumid, const std::wstring& packageFamily) {
    if (aumid.empty() || packageFamily.empty()) {
        return false;
    }
    if (EqualInsensitive(aumid, packageFamily)) {
        return true;
    }

    const size_t bang = aumid.find(L'!');
    const std::wstring aumidFamily = bang == std::wstring::npos ? aumid : aumid.substr(0, bang);
    return EqualInsensitive(aumidFamily, packageFamily);
}

class ComApartment {
public:
    ComApartment()
        : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
    }

    ~ComApartment() {
        if (m_result == S_OK) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool CanUse() const noexcept {
        return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT m_result = E_FAIL;
};

bool IsExistingFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring StripExtendedPathPrefix(std::wstring path) {
    constexpr std::wstring_view kLongPathPrefix = L"\\\\?\\";
    if (path.size() >= kLongPathPrefix.size() &&
        EqualInsensitive(path.substr(0, kLongPathPrefix.size()), std::wstring(kLongPathPrefix))) {
        if (path.size() >= kLongPathPrefix.size() + 4 &&
            EqualInsensitive(path.substr(kLongPathPrefix.size(), 4), L"UNC\\")) {
            return L"\\\\" + path.substr(kLongPathPrefix.size() + 4);
        }
        return path.substr(kLongPathPrefix.size());
    }
    return path;
}

bool IsAumidCharacter(wchar_t character) {
    return std::iswalnum(character) != 0 || character == L'.' || character == L'_' ||
        character == L'-' || character == L'!';
}

bool IsAppsFolderTarget(const std::wstring& target) {
    return target.size() > std::size(kAppsFolderPrefix) - 1 &&
        EqualInsensitive(target.substr(0, std::size(kAppsFolderPrefix) - 1),
            std::wstring(kAppsFolderPrefix, std::size(kAppsFolderPrefix) - 1)) &&
        target.find(L'!') != std::wstring::npos;
}

std::wstring ExtractAppsFolderFromText(const std::wstring& text) {
    const std::wstring lower = [&text]() {
        std::wstring value(text);
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return value;
    }();
    const std::wstring_view prefix(kAppsFolderPrefix, std::size(kAppsFolderPrefix) - 1);
    const size_t position = lower.find(prefix);
    if (position == std::wstring::npos) {
        return {};
    }

    const size_t start = position + prefix.size();
    size_t end = start;
    while (end < text.size() && IsAumidCharacter(text[end])) {
        ++end;
    }
    if (end <= start) {
        return {};
    }

    const std::wstring target = std::wstring(kAppsFolderPrefix) + text.substr(start, end - start);
    return IsAppsFolderTarget(target) ? target : std::wstring{};
}

std::wstring AppsFolderAumid(const std::wstring& target) {
    if (!IsAppsFolderTarget(target)) {
        return {};
    }
    return target.substr(std::size(kAppsFolderPrefix) - 1);
}

std::wstring ExtractProcessStartArgument(const std::wstring& arguments) {
    std::wstring lowercase(arguments);
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });

    constexpr std::wstring_view token = L"--processstart";
    const size_t position = lowercase.find(token);
    if (position == std::wstring::npos) {
        return {};
    }

    size_t start = position + token.size();
    while (start < arguments.size() && std::iswspace(arguments[start]) != 0) {
        ++start;
    }
    if (start >= arguments.size()) {
        return {};
    }

    size_t end = start;
    while (end < arguments.size() && std::iswspace(arguments[end]) == 0) {
        ++end;
    }
    return arguments.substr(start, end - start);
}

std::wstring CombinePath(const std::wstring& directory, const std::wstring& fileName) {
    if (directory.empty()) {
        return fileName;
    }
    if (directory.back() == L'\\' || directory.back() == L'/') {
        return directory + fileName;
    }
    return directory + L"\\" + fileName;
}

void AppendCandidatePath(std::vector<std::wstring>& candidates, const std::wstring& candidate) {
    if (candidate.empty()) {
        return;
    }
    for (const std::wstring& existing : candidates) {
        if (EqualInsensitive(existing, candidate)) {
            return;
        }
    }
    candidates.push_back(candidate);
}

std::wstring FindProcessStartInSiblingAppFolders(const std::wstring& anchorDirectory,
    const std::wstring& processStart) {
    if (anchorDirectory.empty() || processStart.empty()) {
        return {};
    }

    const std::filesystem::path parent = std::filesystem::path(anchorDirectory).parent_path();
    if (parent.empty() || !std::filesystem::is_directory(parent)) {
        return {};
    }

    std::filesystem::path bestPath;
    std::filesystem::file_time_type bestTime{};
    bool found = false;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(parent)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::wstring folderName = entry.path().filename().wstring();
        if (folderName.size() < 4 || !EqualInsensitive(folderName.substr(0, 4), L"app-")) {
            continue;
        }
        const std::filesystem::path candidate = entry.path() / processStart;
        if (!IsExistingFile(candidate.wstring())) {
            continue;
        }
        const std::filesystem::file_time_type writeTime = entry.last_write_time();
        if (!found || writeTime > bestTime) {
            bestPath = candidate;
            bestTime = writeTime;
            found = true;
        }
    }

    return found ? bestPath.wstring() : std::wstring{};
}

void AppendProcessStartCandidates(std::vector<std::wstring>& candidates, const PinnedApp& app) {
    const std::wstring processStart = ExtractProcessStartArgument(app.arguments);
    if (processStart.empty()) {
        return;
    }

    if (!app.workingDirectory.empty()) {
        // The direct combination may reference a removed version folder (e.g. a
        // Squirrel app-* directory from a previous update). Only offer it when it
        // exists: otherwise the generic-icon fallback for the missing path would
        // poison resolution and hide the real executable's icon.
        const std::wstring direct = CombinePath(app.workingDirectory, processStart);
        if (IsExistingFile(direct)) {
            AppendCandidatePath(candidates, direct);
        }
        AppendCandidatePath(candidates,
            FindProcessStartInSiblingAppFolders(app.workingDirectory, processStart));
    }

    if (IsExistingFile(app.target)) {
        const std::filesystem::path targetDirectory = std::filesystem::path(app.target).parent_path();
        if (!targetDirectory.empty()) {
            AppendCandidatePath(candidates,
                FindProcessStartInSiblingAppFolders(targetDirectory.wstring(), processStart));
        }
    }
}

bool IsApplicationFrameHostPath(const std::wstring& path) {
    return EqualInsensitive(FileNameWithoutExtension(path), L"ApplicationFrameHost");
}

bool IsApplicationFrameHostWindow(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL success = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (success == FALSE) {
        return false;
    }
    path.resize(length);
    return IsApplicationFrameHostPath(path);
}

std::wstring HostedAppUserModelId(HWND frame) {
    struct Search {
        DWORD framePid = 0;
        std::wstring aumid;
    };
    Search search;
    GetWindowThreadProcessId(frame, &search.framePid);

    EnumChildWindows(frame,
        [](HWND child, LPARAM data) -> BOOL {
            auto* search = reinterpret_cast<Search*>(data);
            DWORD pid = 0;
            GetWindowThreadProcessId(child, &pid);
            if (pid == 0 || pid == search->framePid) {
                return TRUE;
            }
            HANDLE process =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process == nullptr) {
                return TRUE;
            }
            UINT32 length = 256;
            std::wstring candidate(length, L'\0');
            LONG result = GetApplicationUserModelId(process, &length, candidate.data());
            if (result == ERROR_INSUFFICIENT_BUFFER && length > 1) {
                candidate.assign(length, L'\0');
                result = GetApplicationUserModelId(process, &length, candidate.data());
            }
            CloseHandle(process);
            if (result != ERROR_SUCCESS || length == 0) {
                return TRUE;
            }
            candidate.resize(length);
            while (!candidate.empty() && candidate.back() == L'\0') {
                candidate.pop_back();
            }
            if (!candidate.empty()) {
                search->aumid = std::move(candidate);
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.aumid;
}

std::wstring AppUserModelId(HWND window) {
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store)))) {
        return {};
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT result = store->GetValue(PKEY_AppUserModel_ID, &value);
    store->Release();
    if (FAILED(result) || value.vt != VT_LPWSTR || value.pwszVal == nullptr) {
        PropVariantClear(&value);
        return {};
    }

    std::wstring aumid(value.pwszVal);
    PropVariantClear(&value);
    if (!aumid.empty()) {
        return aumid;
    }

    // Immersive apps (Settings, Store, ...) run hosted inside
    // ApplicationFrameHost.exe, whose frame windows expose no AppUserModelId of
    // their own. Drill into the hosted child process, which does.
    if (IsApplicationFrameHostWindow(window)) {
        return HostedAppUserModelId(window);
    }
    return {};
}

std::wstring ResolveShortcutTarget(const std::wstring& shortcut) {
    ComApartment apartment;
    if (!apartment.CanUse()) {
        return {};
    }

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&link)))) {
        return {};
    }

    IPersistFile* persist = nullptr;
    const HRESULT persistResult = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (FAILED(persistResult)) {
        link->Release();
        return {};
    }

    const bool loaded = SUCCEEDED(persist->Load(shortcut.c_str(), STGM_READ));
    persist->Release();
    if (!loaded) {
        link->Release();
        return {};
    }

    link->Resolve(nullptr, SLR_NO_UI | SLR_NOSEARCH | SLR_NOTRACK);
    std::array<wchar_t, kMaximumPathLength> target{};
    WIN32_FIND_DATAW findData{};
    const bool hasTarget = SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()),
        &findData, SLGP_RAWPATH));
    std::array<wchar_t, kMaximumPathLength> arguments{};
    const bool hasArguments = SUCCEEDED(link->GetArguments(arguments.data(),
        static_cast<int>(arguments.size())));
    link->Release();
    if (!hasTarget && !hasArguments) {
        return {};
    }

    if (hasTarget && target.front() != L'\0') {
        const std::wstring appsFolder = ExtractAppsFolderFromText(target.data());
        if (!appsFolder.empty()) {
            return appsFolder;
        }
        if (IsAppsFolderTarget(target.data())) {
            return target.data();
        }
        return target.data();
    }

    if (hasArguments && arguments.front() != L'\0') {
        return ExtractAppsFolderFromText(arguments.data());
    }

    return {};
}

bool ActivateWindow(HWND window) {
    if (IsIconic(window) != FALSE) {
        ShowWindowAsync(window, SW_RESTORE);
    }
    BringWindowToTop(window);
    if (SetForegroundWindow(window) != FALSE) {
        return true;
    }

    FLASHWINFO flash{sizeof(flash)};
    flash.hwnd = window;
    flash.dwFlags = FLASHW_TRAY;
    flash.uCount = 3;
    FlashWindowEx(&flash);
    return false;
}

std::wstring SanitizeAppDisplayName(std::wstring name) {
    const size_t urlSuffix = name.find(L" - http");
    if (urlSuffix != std::wstring::npos) {
        name.resize(urlSuffix);
    }
    while (!name.empty() && std::iswspace(name.back()) != 0) {
        name.pop_back();
    }
    return name;
}

bool IsGenericDisplayName(const std::wstring& name) {
    if (name.empty()) {
        return true;
    }
    if (ContainsInsensitive(name, L"installation package")) {
        return true;
    }
    return ContainsInsensitive(name, L"http://") || ContainsInsensitive(name, L"https://");
}

bool IsUsableDisplayName(const std::wstring& name) {
    return !name.empty() && !IsGenericDisplayName(name);
}

std::wstring QueryVersionString(const std::wstring& path, const wchar_t* valueName) {
    const DWORD versionSize = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    if (versionSize == 0) {
        return {};
    }

    std::vector<BYTE> data(versionSize);
    if (GetFileVersionInfoW(path.c_str(), 0, versionSize, data.data()) == FALSE) {
        return {};
    }

    struct LangAndCodePage {
        WORD language;
        WORD codePage;
    };
    LangAndCodePage* translations = nullptr;
    UINT translationSize = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
            reinterpret_cast<void**>(&translations), &translationSize) == FALSE ||
        translationSize < sizeof(LangAndCodePage) || translations == nullptr) {
        return {};
    }

    wchar_t subBlock[128]{};
    swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\%s", translations[0].language,
        translations[0].codePage, valueName);
    void* value = nullptr;
    UINT valueSize = 0;
    if (VerQueryValueW(data.data(), subBlock, &value, &valueSize) == FALSE || value == nullptr) {
        return {};
    }
    return SanitizeAppDisplayName(std::wstring(static_cast<wchar_t*>(value)));
}

std::wstring ExecutableDisplayName(const std::wstring& path) {
    if (path.empty() || !IsExistingFile(path)) {
        return {};
    }

    for (const wchar_t* key : {L"FileDescription", L"ProductName"}) {
        const std::wstring candidate = QueryVersionString(path, key);
        if (IsUsableDisplayName(candidate)) {
            return candidate;
        }
    }

    const std::wstring fileName = FileNameWithoutExtension(path);
    return IsUsableDisplayName(fileName) ? fileName : std::wstring{};
}

std::wstring ShellItemDisplayName(const std::wstring& target) {
    ComApartment apartment;
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return {};
    }

    const std::array<SIGDN, 2> formats = {SIGDN_PARENTRELATIVEFORUI, SIGDN_NORMALDISPLAY};
    for (const SIGDN format : formats) {
        PWSTR name = nullptr;
        const HRESULT result = item->GetDisplayName(format, &name);
        if (FAILED(result) || name == nullptr) {
            continue;
        }
        const std::wstring displayName = SanitizeAppDisplayName(name);
        CoTaskMemFree(name);
        if (IsUsableDisplayName(displayName)) {
            item->Release();
            return displayName;
        }
    }

    item->Release();
    return {};
}

}  // namespace

bool WindowCatalog::WindowsSnapshotEqual(const std::vector<RunningWindow>& left,
    const std::vector<RunningWindow>& right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].handle != right[index].handle ||
            left[index].executablePath != right[index].executablePath ||
            left[index].title != right[index].title) {
            return false;
        }
    }
    return true;
}

std::wstring WindowCatalog::CachedNormalizedPath(const std::wstring& path) {
    const auto cached = m_normalizedPathCache.find(path);
    if (cached != m_normalizedPathCache.end()) {
        return cached->second;
    }
    const std::wstring normalized = NormalizedPath(path);
    m_normalizedPathCache.emplace(path, normalized);
    return normalized;
}

void WindowCatalog::EnrichWindows() {
    for (RunningWindow& window : m_windows) {
        GetWindowThreadProcessId(window.handle, &window.processId);
        window.normalizedPath = CachedNormalizedPath(window.executablePath);
        window.executableName = FileNameWithoutExtension(window.executablePath);
        if (m_pinMatchingNeedsAumid) {
            window.appUserModelId = AppUserModelId(window.handle);
        } else {
            window.appUserModelId.clear();
        }
    }
    m_enrichedAumid = m_pinMatchingNeedsAumid;
}

bool WindowCatalog::Refresh() {
    ProfileScope scope("WindowCatalog::Refresh");
    std::vector<RunningWindow> previous = std::move(m_windows);
    m_windows.clear();
    EnumWindows(&WindowCatalog::EnumerateWindows, reinterpret_cast<LPARAM>(this));
    std::ranges::sort(m_windows, {}, &RunningWindow::handle);

    if (!previous.empty()) {
        std::ranges::sort(previous, {}, &RunningWindow::handle);
        // The snapshot comparison ignores enrichment, so a stable window set must
        // still be re-enriched when the AppUserModelId coverage changed (e.g. an
        // apps-folder pin appeared after the windows were first seen); otherwise
        // AUMID matching would never engage for them.
        if (WindowsSnapshotEqual(previous, m_windows) &&
            m_enrichedAumid == m_pinMatchingNeedsAumid) {
            m_windows = std::move(previous);
            return false;
        }
    }

    EnrichWindows();
    return true;
}

void WindowCatalog::RebuildPinProfiles(const std::vector<PinnedApp>& pins) {
    if (pins.size() == m_cachedPinProfileSources.size() &&
        std::equal(pins.begin(), pins.end(), m_cachedPinProfileSources.begin(),
            [](const PinnedApp& left, const PinnedApp& right) {
                return left.name == right.name && left.target == right.target &&
                    left.arguments == right.arguments && left.workingDirectory == right.workingDirectory;
            })) {
        return;
    }
    m_cachedPinProfileSources = pins;
    m_pinMatchingNeedsAumid = false;

    ComApartment apartment;
    static_cast<void>(apartment);

    m_pinProfiles.clear();
    m_pinProfiles.reserve(pins.size());
    for (const PinnedApp& pin : pins) {
        if (IsShellTarget(pin.target) && !IsAppsFolderTarget(pin.target)) {
            continue;
        }

        PinMatchProfile profile;
        profile.sourceTarget = pin.target;

        const auto addExecutableName = [&profile](const std::wstring& executableName) {
            if (executableName.empty()) {
                return;
            }
            const bool nameKnown = std::ranges::any_of(profile.executableNames,
                [&executableName](const std::wstring& existing) {
                    return EqualInsensitive(existing, executableName);
                });
            if (!nameKnown) {
                profile.executableNames.push_back(executableName);
            }
        };

        const auto addCandidate = [this, &profile, &addExecutableName](const std::wstring& candidate) {
            if (candidate.empty()) {
                return;
            }
            if (IsAppsFolderTarget(candidate)) {
                profile.appsFolderAumid = AppsFolderAumid(candidate);
                m_pinMatchingNeedsAumid = true;
                const size_t bang = profile.appsFolderAumid.find(L'!');
                if (bang != std::wstring::npos) {
                    profile.packageFamily = profile.appsFolderAumid.substr(0, bang);
                    addExecutableName(profile.appsFolderAumid.substr(bang + 1));
                }
                return;
            }
            if (IsShellTarget(candidate)) {
                return;
            }

            const std::wstring normalized = NormalizedPath(candidate);
            const bool pathKnown = std::ranges::any_of(profile.normalizedPaths,
                [&normalized](const std::wstring& existing) {
                    return EqualInsensitive(existing, normalized);
                });
            if (!pathKnown) {
                profile.normalizedPaths.push_back(normalized);
            }

            const std::wstring executableName = FileNameWithoutExtension(normalized);
            const bool nameKnown = std::ranges::any_of(profile.executableNames,
                [&executableName](const std::wstring& existing) {
                    return EqualInsensitive(existing, executableName);
                });
            if (!nameKnown) {
                profile.executableNames.push_back(executableName);
            }
        };

        addCandidate(pin.target);
        const std::wstring launchedProcess = ResolveLauncherProcessPath(pin);
        if (!TargetsMatch(launchedProcess, pin.target)) {
            addCandidate(launchedProcess);
        }
        if (EndsWithInsensitive(pin.target, L".lnk") && IsExistingFile(pin.target)) {
            addCandidate(ResolveShortcutTarget(pin.target));
        }
        if (IsAppsFolderTarget(pin.target)) {
            profile.appsFolderAumid = AppsFolderAumid(pin.target);
            m_pinMatchingNeedsAumid = true;
            const size_t bang = profile.appsFolderAumid.find(L'!');
            if (bang != std::wstring::npos) {
                profile.packageFamily = profile.appsFolderAumid.substr(0, bang);
                addExecutableName(profile.appsFolderAumid.substr(bang + 1));
            }
        }

        m_pinProfiles.push_back(std::move(profile));
    }
}

const std::vector<RunningWindow>& WindowCatalog::RunningWindows() const noexcept {
    return m_windows;
}

bool WindowCatalog::IsRunning(const PinnedApp& app) const {
    return FindWindowFor(app) != nullptr;
}

HWND WindowCatalog::FindWindowFor(const PinnedApp& app) const {
    if (IsShellTarget(app.target) && !IsAppsFolderTarget(app.target)) {
        return nullptr;
    }

    for (const RunningWindow& window : m_windows) {
        if (MatchesPin(app, window)) {
            return window.handle;
        }
    }
    return nullptr;
}

bool IsHelperExecutableAlias(const std::wstring& windowName, const std::wstring& windowPath,
    const std::wstring& pinPath) {
    // Matches helper processes that live under their app's folder tree, e.g.
    // Steam's bin\cef\cef.win64\steamwebhelper.exe for a steam.exe pin. The
    // suffix allowlist keeps adjacent-but-distinct apps (e.g. putty/puttygen)
    // from merging.
    const std::wstring pinName = FileNameWithoutExtension(pinPath);
    if (pinName.empty() || windowName.size() <= pinName.size() ||
        !EqualInsensitive(windowName.substr(0, pinName.size()), pinName)) {
        return false;
    }
    const std::wstring suffix = windowName.substr(pinName.size());
    static constexpr std::wstring_view kHelperSuffixes[] = {
        L"webhelper", L"helper", L"launcher", L"updater", L"service", L"tray"};
    bool knownSuffix = false;
    for (const std::wstring_view known : kHelperSuffixes) {
        if (EqualInsensitive(suffix, std::wstring(known))) {
            knownSuffix = true;
            break;
        }
    }
    if (!knownSuffix) {
        return false;
    }
    const size_t separator = pinPath.find_last_of(L"\\/");
    if (separator == std::wstring::npos || separator == 0) {
        return false;
    }
    const std::wstring pinDirectory = pinPath.substr(0, separator);
    return windowPath.size() > pinDirectory.size() &&
        EqualInsensitive(windowPath.substr(0, pinDirectory.size()), pinDirectory) &&
        (windowPath[pinDirectory.size()] == L'\\' || windowPath[pinDirectory.size()] == L'/');
}

bool WindowCatalog::MatchWindowAgainstProfile(const PinMatchProfile& profile,
    const RunningWindow& window) const noexcept {
    if (!profile.packageFamily.empty()) {
        if (PathMatchesPackageFamily(window.normalizedPath, profile.packageFamily)) {
            return true;
        }
        if (AumidMatchesPackageFamily(window.appUserModelId, profile.packageFamily)) {
            return true;
        }
    }

    for (const std::wstring& path : profile.normalizedPaths) {
        if (EqualInsensitive(path, window.normalizedPath)) {
            return true;
        }
    }

    for (const std::wstring& name : profile.executableNames) {
        if (EqualInsensitive(name, window.executableName)) {
            return true;
        }
    }

    for (const std::wstring& path : profile.normalizedPaths) {
        if (IsHelperExecutableAlias(window.executableName, window.normalizedPath, path)) {
            return true;
        }
    }

    if (!window.appUserModelId.empty()) {
        if (!profile.appsFolderAumid.empty() &&
            EqualInsensitive(profile.appsFolderAumid, window.appUserModelId)) {
            return true;
        }
        for (const std::wstring& path : profile.normalizedPaths) {
            if (EqualInsensitive(path, window.appUserModelId)) {
                return true;
            }
        }
    }

    return false;
}

const WindowCatalog::PinMatchProfile* WindowCatalog::ProfileForPin(
    const PinnedApp& pin) const noexcept {
    for (const PinMatchProfile& profile : m_pinProfiles) {
        if (EqualInsensitive(profile.sourceTarget, pin.target) ||
            TargetsMatch(profile.sourceTarget, pin.target)) {
            return &profile;
        }
    }
    return nullptr;
}

bool WindowCatalog::MatchesPin(const PinnedApp& pin, const RunningWindow& window) const {
    const PinMatchProfile* profile = ProfileForPin(pin);
    if (profile == nullptr) {
        return false;
    }
    return MatchWindowAgainstProfile(*profile, window);
}

bool WindowCatalog::MatchesAnyPin(const RunningWindow& window) const {
    for (const PinMatchProfile& profile : m_pinProfiles) {
        if (MatchWindowAgainstProfile(profile, window)) {
            return true;
        }
    }
    return false;
}

bool WindowCatalog::ActivateOrLaunch(const PinnedApp& app, HWND preferredWindow) const {
    HWND window = preferredWindow;
    if (window == nullptr || IsWindow(window) == FALSE) {
        window = FindWindowFor(app);
    }
    if (window != nullptr) {
        return ActivateWindow(window);
    }

    // A stale working directory (e.g. a Squirrel app-* folder removed by an
    // update) makes ShellExecute fail outright, so a tray-hidden app could never
    // be restored. Fall back to the resolved launcher's folder, else inherit.
    std::wstring workingDirectory = app.workingDirectory;
    if (!workingDirectory.empty()) {
        std::error_code statusError;
        if (!std::filesystem::is_directory(workingDirectory, statusError)) {
            workingDirectory.clear();
            const std::wstring resolved = ResolveLauncherProcessPath(app);
            if (!resolved.empty() && !IsShellTarget(resolved)) {
                const std::filesystem::path parent =
                    std::filesystem::path(resolved).parent_path();
                std::error_code parentError;
                if (!parent.empty() && std::filesystem::is_directory(parent, parentError)) {
                    workingDirectory = parent.wstring();
                }
            }
        }
    }

    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"open";
    launch.lpFile = app.target.c_str();
    launch.lpParameters = app.arguments.empty() ? nullptr : app.arguments.c_str();
    launch.lpDirectory = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
    launch.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&launch) != FALSE;
}

bool WindowCatalog::Close(const PinnedApp& app, HWND preferredWindow) const {
    HWND window = preferredWindow;
    if (window == nullptr || IsWindow(window) == FALSE) {
        window = FindWindowFor(app);
    }
    return window != nullptr && PostMessageW(window, WM_CLOSE, 0, 0) != FALSE;
}

bool WindowCatalog::EndTask(const PinnedApp& app, HWND preferredWindow) const {
    HWND window = preferredWindow;
    if (window == nullptr || IsWindow(window) == FALSE) {
        window = FindWindowFor(app);
    }
    if (window == nullptr) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, processId);
    if (process == nullptr) {
        return false;
    }
    const BOOL terminated = TerminateProcess(process, 1);
    CloseHandle(process);
    return terminated != FALSE;
}

bool WindowCatalog::OpenLocation(const PinnedApp& app) const {
    if (IsShellTarget(app.target)) {
        return false;
    }

    const std::wstring parameters = L"/select,\"" + app.target + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe",
        parameters.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

bool WindowCatalog::AddForegroundApplication(std::vector<PinnedApp>& pins) const {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || !IsApplicationWindow(foreground)) {
        return false;
    }

    const std::wstring path = ExecutablePath(foreground);
    if (path.empty()) {
        return false;
    }

    const bool alreadyPinned = std::ranges::any_of(pins, [&path](const PinnedApp& app) {
        return WindowCatalog::TargetsMatch(app.target, path);
    });
    if (alreadyPinned) {
        return false;
    }

    PinnedApp app;
    app.target = path;
    app.name = WindowCatalog::DisplayNameForApp(app, foreground);
    pins.push_back(std::move(app));
    return true;
}

bool WindowCatalog::TargetsMatch(const std::wstring& left, const std::wstring& right) {
    return EqualInsensitive(NormalizedPath(left), NormalizedPath(right));
}

std::wstring WindowCatalog::ResolveLauncherProcessPath(const PinnedApp& app) {
    const std::wstring processStart = ExtractProcessStartArgument(app.arguments);
    if (processStart.empty()) {
        return app.target;
    }

    if (!app.workingDirectory.empty()) {
        const std::wstring direct =
            NormalizedPath(CombinePath(app.workingDirectory, processStart));
        if (IsExistingFile(direct)) {
            return direct;
        }

        const std::wstring sibling = FindProcessStartInSiblingAppFolders(app.workingDirectory, processStart);
        if (!sibling.empty()) {
            return sibling;
        }
    }

    if (IsExistingFile(app.target)) {
        const std::filesystem::path targetDirectory = std::filesystem::path(app.target).parent_path();
        if (!targetDirectory.empty()) {
            const std::wstring sibling =
                FindProcessStartInSiblingAppFolders(targetDirectory.wstring(), processStart);
            if (!sibling.empty()) {
                return sibling;
            }
        }
    }

    return app.target;
}

std::wstring WindowCatalog::IconCacheKey(const PinnedApp& app) {
    return app.target;
}

void AppendUniqueCandidate(std::vector<std::wstring>& candidates, const std::wstring& candidate) {
    if (candidate.empty()) {
        return;
    }
    for (const std::wstring& existing : candidates) {
        if (existing == candidate || WindowCatalog::TargetsMatch(existing, candidate)) {
            return;
        }
    }
    candidates.push_back(candidate);
}

std::vector<std::wstring> WindowCatalog::IconResolutionCandidates(const PinnedApp& app,
    HWND runningWindow) {
    std::vector<std::wstring> candidates;
    candidates.reserve(12);

    if (runningWindow != nullptr) {
        const std::wstring runningPath = NormalizedPath(ExecutablePath(runningWindow));
        // A frame host's own executable icon would mislabel (and outrank) the
        // hosted app; the AppUserModelId candidate below carries the real icon.
        if (!IsApplicationFrameHostPath(runningPath)) {
            AppendUniqueCandidate(candidates, runningPath);
        }
        const std::wstring aumid = AppUserModelId(runningWindow);
        if (!aumid.empty()) {
            AppendUniqueCandidate(candidates, std::wstring(kAppsFolderPrefix) + aumid);
        }
    }

    AppendProcessStartCandidates(candidates, app);

    if (IsAppsFolderTarget(app.target)) {
        AppendUniqueCandidate(candidates, app.target);
    }

    if (EndsWithInsensitive(app.target, L".lnk") && IsExistingFile(app.target)) {
        AppendUniqueCandidate(candidates, app.target);
        AppendUniqueCandidate(candidates, ResolveShortcutTarget(app.target));
    }

    const std::wstring launchedProcess = ResolveLauncherProcessPath(app);
    if (!launchedProcess.empty() && !IsShellTarget(launchedProcess)) {
        AppendUniqueCandidate(candidates, launchedProcess);
    }

    AppendUniqueCandidate(candidates, app.target);

    return candidates;
}

std::wstring WindowCatalog::DisplayNameForApp(const PinnedApp& app, HWND runningWindow) {
    if (runningWindow != nullptr &&
        IsApplicationFrameHostPath(NormalizedPath(ExecutablePath(runningWindow)))) {
        // The frame host's product name ("Application Frame Host") would mislabel
        // the entry; identify the hosted app through its AppUserModelId instead.
        const std::wstring hosted = AppUserModelId(runningWindow);
        if (!hosted.empty()) {
            const std::wstring shellName =
                ShellItemDisplayName(std::wstring(kAppsFolderPrefix) + hosted);
            if (IsUsableDisplayName(shellName)) {
                return shellName;
            }
        }
    }
    std::wstring resolvedPath;
    if (runningWindow != nullptr) {
        resolvedPath = NormalizedPath(ExecutablePath(runningWindow));
        if (!resolvedPath.empty()) {
            // A helper process (e.g. steamwebhelper for a Steam pin) carries the
            // helper's product name; identify the entry by the pin instead so the
            // app keeps a single stable name.
            const std::wstring pinResolved = ResolveLauncherProcessPath(app);
            if (!pinResolved.empty() && !IsShellTarget(pinResolved) &&
                !EqualInsensitive(FileNameWithoutExtension(resolvedPath),
                    FileNameWithoutExtension(pinResolved))) {
                resolvedPath.clear();
            }
        }
    }
    if (resolvedPath.empty()) {
        resolvedPath = ResolveLauncherProcessPath(app);
    }
    if (!resolvedPath.empty() && !IsShellTarget(resolvedPath)) {
        const std::wstring executableName = ExecutableDisplayName(resolvedPath);
        if (IsUsableDisplayName(executableName)) {
            return executableName;
        }
    }

    if (IsAppsFolderTarget(app.target)) {
        const std::wstring shellName = ShellItemDisplayName(app.target);
        if (IsUsableDisplayName(shellName)) {
            return shellName;
        }
    }

    const std::wstring sanitizedStoredName = SanitizeAppDisplayName(app.name);
    if (IsUsableDisplayName(sanitizedStoredName)) {
        return sanitizedStoredName;
    }

    if (!IsShellTarget(app.target)) {
        const std::wstring targetName = ExecutableDisplayName(app.target);
        if (IsUsableDisplayName(targetName)) {
            return targetName;
        }
    }

    if (!resolvedPath.empty()) {
        const std::wstring fallback = FileNameWithoutExtension(resolvedPath);
        if (IsUsableDisplayName(fallback)) {
            return fallback;
        }
    }

    return FileNameWithoutExtension(app.target);
}

BOOL CALLBACK WindowCatalog::EnumerateWindows(HWND window, LPARAM data) {
    auto* catalog = reinterpret_cast<WindowCatalog*>(data);
    if (!IsApplicationWindow(window)) {
        return TRUE;
    }

    const std::wstring title = WindowTitle(window);
    if (IsPictureInPictureTitle(title)) {
        return TRUE;
    }

    const std::wstring path = ExecutablePath(window);
    if (path.empty()) {
        return TRUE;
    }

    catalog->m_windows.push_back({window, path, {}, {}, title});
    return TRUE;
}

bool WindowCatalog::IsApplicationWindow(HWND window) {
    if (IsWindowVisible(window) == FALSE || GetWindow(window, GW_OWNER) != nullptr) {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((style & WS_CHILD) != 0 || (extendedStyle & WS_EX_TOOLWINDOW) != 0 ||
        (extendedStyle & WS_EX_NOACTIVATE) != 0) {
        return false;
    }

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return false;
    }
    return !IsSmallAuxiliaryWindow(window, style, extendedStyle);
}

std::wstring WindowCatalog::ExecutablePath(HWND window) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0) {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return {};
    }

    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL success = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (success == FALSE) {
        return {};
    }

    path.resize(length);
    return path;
}

std::wstring WindowCatalog::WindowTitle(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, title.data(), length + 1);
    title.resize(static_cast<size_t>(length));
    return title;
}

std::wstring WindowCatalog::NormalizedPath(const std::wstring& path) {
    if (path.empty() || WindowCatalog::IsShellTarget(path)) {
        return StripExtendedPathPrefix(path);
    }

    const std::wstring stripped = StripExtendedPathPrefix(path);
    wchar_t fullPath[32768]{};
    const DWORD length = GetFullPathNameW(stripped.c_str(), static_cast<DWORD>(std::size(fullPath)),
        fullPath, nullptr);
    std::wstring normalized = length == 0 || length >= std::size(fullPath)
        ? stripped
        : std::wstring(fullPath, length);

    wchar_t longPath[32768]{};
    const DWORD longLength = GetLongPathNameW(normalized.c_str(), longPath,
        static_cast<DWORD>(std::size(longPath)));
    if (longLength > 0 && longLength < std::size(longPath)) {
        normalized.assign(longPath, longLength);
    }
    return StripExtendedPathPrefix(normalized);
}

bool WindowCatalog::IsShellTarget(const std::wstring& target) {
    return target.rfind(L"shell:", 0) == 0;
}
