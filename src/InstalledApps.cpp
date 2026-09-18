#include "InstalledApps.h"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <propidl.h>
#include <propkey.h>
#include <propsys.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kAppsFolderPrefix[] = L"shell:AppsFolder\\";
constexpr size_t kMaximumPathLength = 32768;

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

    [[nodiscard]] bool Ok() const noexcept {
        return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT m_result = E_FAIL;
};

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool EqualInsensitive(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](wchar_t lhs, wchar_t rhs) {
            return std::towlower(lhs) == std::towlower(rhs);
        });
}

bool ContainsInsensitive(std::wstring_view value, std::wstring_view needle) {
    if (needle.empty() || value.size() < needle.size()) {
        return false;
    }
    const std::wstring haystack = ToLower(std::wstring(value));
    const std::wstring find = ToLower(std::wstring(needle));
    return haystack.find(find) != std::wstring::npos;
}

std::wstring Trim(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t character) {
        return std::iswspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t character) {
        return std::iswspace(character) != 0;
    }).base();
    return first >= last ? L"" : std::wstring(first, last);
}

std::wstring FileStem(const std::wstring& path) {
    const std::filesystem::path file(path);
    return file.stem().wstring();
}

std::wstring FileName(const std::wstring& path) {
    const std::filesystem::path file(path);
    return file.filename().wstring();
}

bool IsExistingFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsUninstaller(const InstalledApp& app) {
    const std::wstring name = ToLower(app.launch.name + L" " + app.shortcutName + L" " +
        app.executableName + L" " + FileName(app.launch.target));
    return ContainsInsensitive(name, L"uninstall") || ContainsInsensitive(name, L"unins000") ||
        ContainsInsensitive(name, L"remove ");
}

bool IsNoiseAumid(const std::wstring& aumid) {
    const std::wstring value = ToLower(aumid);
    constexpr std::wstring_view noise[] = {
        L"shellexperiencehost", L"startmenuexperiencehost", L"searchhost", L"searchapp",
        L"cortana", L"textinputhost", L"lockapp", L"narratordisplay", L"xboxgameoverlay",
        L"windowsinternal.", L"inputapp", L"accountscontrol", L"addusertowork",
        L"contentdeliverymanager", L"parentalcontrols",
    };
    for (const std::wstring_view token : noise) {
        if (value.find(token) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

std::wstring QueryVersionString(const std::wstring& path, const wchar_t* valueName) {
    if (path.empty() || !IsExistingFile(path)) {
        return {};
    }
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
    return Trim(std::wstring(static_cast<wchar_t*>(value)));
}

void FillVersionInfo(InstalledApp& app) {
    const std::wstring path = app.executablePath.empty() ? app.launch.target : app.executablePath;
    if (path.empty() || path.rfind(L"shell:", 0) == 0 || !IsExistingFile(path)) {
        return;
    }
    if (app.executablePath.empty()) {
        app.executablePath = path;
    }
    if (app.executableName.empty()) {
        app.executableName = FileName(path);
    }
    if (app.description.empty()) {
        app.description = QueryVersionString(path, L"FileDescription");
    }
    if (app.productName.empty()) {
        app.productName = QueryVersionString(path, L"ProductName");
    }
    if (app.publisher.empty()) {
        app.publisher = QueryVersionString(path, L"CompanyName");
    }
}

std::wstring ShellItemString(IShellItem2* item, const PROPERTYKEY& key) {
    if (item == nullptr) {
        return {};
    }
    PWSTR value = nullptr;
    if (FAILED(item->GetString(key, &value)) || value == nullptr) {
        return {};
    }
    std::wstring result(value);
    CoTaskMemFree(value);
    return Trim(result);
}

std::wstring KnownFolderPath(REFKNOWNFOLDERID folder) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &path)) || path == nullptr) {
        return {};
    }
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

using ShellLinkTextMethod = HRESULT(STDMETHODCALLTYPE IShellLinkW::*)(LPWSTR, int);

std::wstring ShellLinkText(IShellLinkW* link, ShellLinkTextMethod method) {
    std::array<wchar_t, kMaximumPathLength> text{};
    if (FAILED((link->*method)(text.data(), static_cast<int>(text.size())))) {
        return {};
    }
    return Trim(text.data());
}

bool ResolveShortcut(const std::wstring& shortcut, InstalledApp& app) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        return false;
    }
    ComPtr<IPersistFile> persist;
    if (FAILED(link.As(&persist))) {
        return false;
    }
    if (FAILED(persist->Load(shortcut.c_str(), STGM_READ))) {
        return false;
    }
    link->Resolve(nullptr, SLR_NO_UI | SLR_NOSEARCH | SLR_NOTRACK);

    std::array<wchar_t, kMaximumPathLength> target{};
    WIN32_FIND_DATAW findData{};
    const bool hasTarget = SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()),
        &findData, SLGP_RAWPATH));
    const std::wstring arguments = ShellLinkText(link.Get(), &IShellLinkW::GetArguments);
    const std::wstring workingDirectory = ShellLinkText(link.Get(), &IShellLinkW::GetWorkingDirectory);
    const std::wstring comment = ShellLinkText(link.Get(), &IShellLinkW::GetDescription);

    std::wstring aumid;
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(link.As(&store))) {
        PROPVARIANT variant{};
        if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &variant))) {
            if (variant.vt == VT_LPWSTR && variant.pwszVal != nullptr) {
                aumid = Trim(variant.pwszVal);
            }
            PropVariantClear(&variant);
        }
    }

    app.shortcutName = FileStem(shortcut);
    app.comment = comment;
    app.aumid = aumid;
    app.launch.arguments = arguments;
    app.launch.workingDirectory = workingDirectory;
    app.launch.name = app.shortcutName;

    if (!aumid.empty()) {
        app.launch.target = std::wstring(kAppsFolderPrefix) + aumid;
        app.launch.arguments.clear();
        return true;
    }

    if (!hasTarget || target.front() == L'\0') {
        return false;
    }

    const std::wstring resolved(target.data());
    if (IsDirectory(resolved)) {
        return false;
    }
    app.launch.target = resolved;
    if (IsExistingFile(resolved)) {
        app.executablePath = resolved;
        app.executableName = FileName(resolved);
    }
    return !app.launch.target.empty();
}

void AppendUnique(std::unordered_map<std::wstring, size_t>& index, std::vector<InstalledApp>& apps,
    InstalledApp app) {
    if (app.launch.name.empty()) {
        app.launch.name = !app.shortcutName.empty() ? app.shortcutName
            : !app.productName.empty() ? app.productName
            : FileStem(app.launch.target);
    }
    if (app.launch.name.empty() || IsUninstaller(app) || IsNoiseAumid(app.aumid)) {
        return;
    }

    std::wstring key;
    if (!app.aumid.empty()) {
        key = L"aumid:" + ToLower(app.aumid);
    } else if (app.launch.target.rfind(L"shell:", 0) == 0) {
        key = L"shell:" + ToLower(app.launch.target);
    } else if (!app.executablePath.empty()) {
        key = L"exe:" + ToLower(app.executablePath) + L"|" + ToLower(app.launch.arguments);
    } else {
        key = L"target:" + ToLower(app.launch.target) + L"|" + ToLower(app.launch.arguments);
    }

    const auto existing = index.find(key);
    if (existing == index.end()) {
        index.emplace(key, apps.size());
        apps.push_back(std::move(app));
        return;
    }

    InstalledApp& current = apps[existing->second];
    auto takeIfEmpty = [](std::wstring& dest, std::wstring& source) {
        if (dest.empty() && !source.empty()) {
            dest = std::move(source);
        }
    };
    takeIfEmpty(current.shortcutName, app.shortcutName);
    takeIfEmpty(current.executableName, app.executableName);
    takeIfEmpty(current.executablePath, app.executablePath);
    takeIfEmpty(current.aumid, app.aumid);
    takeIfEmpty(current.description, app.description);
    takeIfEmpty(current.productName, app.productName);
    takeIfEmpty(current.publisher, app.publisher);
    takeIfEmpty(current.startMenuFolder, app.startMenuFolder);
    takeIfEmpty(current.comment, app.comment);
    if (current.launch.arguments.empty()) {
        current.launch.arguments = std::move(app.launch.arguments);
    }
    if (current.launch.workingDirectory.empty()) {
        current.launch.workingDirectory = std::move(app.launch.workingDirectory);
    }
}

void WalkStartMenu(const std::wstring& root, std::unordered_map<std::wstring, size_t>& index,
    std::vector<InstalledApp>& apps) {
    if (root.empty()) {
        return;
    }
    std::error_code error;
    const std::filesystem::path rootPath(root);
    std::filesystem::recursive_directory_iterator entries(rootPath,
        std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; entries != end; entries.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!entries->is_regular_file(error)) {
            error.clear();
            continue;
        }
        if (!EqualInsensitive(entries->path().extension().wstring(), L".lnk")) {
            continue;
        }

        InstalledApp app;
        if (!ResolveShortcut(entries->path().wstring(), app)) {
            continue;
        }
        FillVersionInfo(app);
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(entries->path().parent_path(),
            rootPath, relativeError);
        if (!relativeError && !relative.empty() && relative != L".") {
            app.startMenuFolder = relative.wstring();
        }
        if (app.description.empty() && !app.comment.empty()) {
            app.description = app.comment;
        }
        AppendUnique(index, apps, std::move(app));
    }
}

void EnumerateAppsFolder(std::unordered_map<std::wstring, size_t>& index,
    std::vector<InstalledApp>& apps) {
    ComPtr<IShellItem> folder;
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder)))) {
        return;
    }
    ComPtr<IEnumShellItems> items;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) {
        return;
    }

    ComPtr<IShellItem> item;
    while (items->Next(1, &item, nullptr) == S_OK) {
        ComPtr<IShellItem2> item2;
        if (FAILED(item.As(&item2))) {
            item.Reset();
            continue;
        }

        PWSTR display = nullptr;
        item->GetDisplayName(SIGDN_NORMALDISPLAY, &display);
        const std::wstring name = display == nullptr ? L"" : Trim(display);
        CoTaskMemFree(display);

        const std::wstring aumid = ShellItemString(item2.Get(), PKEY_AppUserModel_ID);
        if (name.empty() || aumid.empty() || IsNoiseAumid(aumid)) {
            item.Reset();
            continue;
        }

        InstalledApp app;
        app.launch.name = name;
        app.shortcutName = name;
        app.aumid = aumid;
        app.launch.target = std::wstring(kAppsFolderPrefix) + aumid;
        app.description = ShellItemString(item2.Get(), PKEY_FileDescription);
        app.publisher = ShellItemString(item2.Get(), PKEY_Company);
        app.comment = ShellItemString(item2.Get(), PKEY_Comment);
        FillVersionInfo(app);
        AppendUnique(index, apps, std::move(app));
        item.Reset();
    }
}

std::vector<InstalledApp> EnumerateInstalledApps() {
    ComApartment apartment;
    if (!apartment.Ok()) {
        return {};
    }

    std::vector<InstalledApp> apps;
    std::unordered_map<std::wstring, size_t> index;
    apps.reserve(256);
    WalkStartMenu(KnownFolderPath(FOLDERID_Programs), index, apps);
    WalkStartMenu(KnownFolderPath(FOLDERID_CommonPrograms), index, apps);
    EnumerateAppsFolder(index, apps);
    return apps;
}

}  // namespace

void InstalledAppCatalog::EnsureLoaded() {
    std::lock_guard lock(m_mutex);
    if (m_loaded) {
        return;
    }
    m_apps = EnumerateInstalledApps();
    m_loaded = true;
}

void InstalledAppCatalog::Clear() noexcept {
    std::lock_guard lock(m_mutex);
    m_apps.clear();
    m_apps.shrink_to_fit();
    m_loaded = false;
}


std::vector<InstalledApp> InstalledAppCatalog::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_apps;
}
