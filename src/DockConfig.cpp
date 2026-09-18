#include "DockConfig.h"

#include <Windows.h>
#include <ShObjIdl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kConfigDirectoryName[] = L"LiquidGlassDock";
constexpr wchar_t kConfigFileName[] = L"dock.ini";
constexpr size_t kMaximumPins = 512;
constexpr size_t kMaximumPathLength = 32768;
constexpr wchar_t kAppsFolderPrefix[] = L"shell:AppsFolder\\";
constexpr wchar_t kCloudStorePath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\Cache\\DefaultAccount";

class ComApartment {
public:
    ComApartment()
        : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
    }

    ~ComApartment() {
        if (SUCCEEDED(m_result)) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool CanUseShellLinks() const noexcept {
        return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT m_result = E_FAIL;
};

std::wstring Trim(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t value) {
        return std::iswspace(value) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t value) {
        return std::iswspace(value) != 0;
    }).base();
    return first >= last ? L"" : std::wstring(first, last);
}

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

bool EndsWithInsensitive(std::wstring_view value, std::wstring_view suffix) {
    return value.size() >= suffix.size() &&
        EqualInsensitive(value.substr(value.size() - suffix.size()), suffix);
}

bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() && EqualInsensitive(value.substr(0, prefix.size()), prefix);
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }

    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }

    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring EnvironmentVariable(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required <= 1) {
        return {};
    }

    std::wstring value(static_cast<size_t>(required), L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required) {
        return {};
    }
    value.resize(copied);
    return value;
}

std::wstring AppDataConfigPath() {
    const std::wstring appData = EnvironmentVariable(L"LOCALAPPDATA");
    if (appData.empty()) {
        return {};
    }

    const std::wstring directory = appData + L"\\" + kConfigDirectoryName;
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\" + kConfigFileName;
}

bool ParseBoolean(const std::wstring& value) {
    const std::wstring lower = ToLower(Trim(value));
    return lower == L"1" || lower == L"true" || lower == L"yes";
}

float ParseFloat(const std::wstring& value, float fallback) {
    const std::wstring trimmed = Trim(value);
    if (trimmed.empty()) {
        return fallback;
    }
    try {
        return std::stof(trimmed);
    } catch (...) {
        return fallback;
    }
}

std::wstring ExpandTarget(const std::wstring& target) {
    if (target.empty()) {
        return {};
    }

    const DWORD required = ExpandEnvironmentStringsW(target.c_str(), nullptr, 0);
    if (required == 0) {
        return target;
    }

    std::wstring expanded(required, L'\0');
    ExpandEnvironmentStringsW(target.c_str(), expanded.data(), required);
    expanded.pop_back();
    return expanded;
}

std::wstring NormalizedTarget(const std::wstring& target) {
    std::wstring normalized = ExpandTarget(target);
    if (normalized.empty() || normalized.rfind(L"shell:", 0) == 0) {
        return ToLower(std::move(normalized));
    }

    std::array<wchar_t, kMaximumPathLength> fullPath{};
    const DWORD length = GetFullPathNameW(normalized.c_str(), static_cast<DWORD>(fullPath.size()),
        fullPath.data(), nullptr);
    if (length != 0 && length < fullPath.size()) {
        normalized.assign(fullPath.data(), length);
    }
    return ToLower(std::move(normalized));
}

std::wstring FileNameWithoutExtension(const std::wstring& path) {
    const size_t fileStart = path.find_last_of(L"\\/") + 1;
    const size_t extension = path.find_last_of(L'.');
    const size_t fileEnd = extension == std::wstring::npos || extension < fileStart
        ? path.size()
        : extension;
    return path.substr(fileStart, fileEnd - fileStart);
}

bool IsAumidCharacter(wchar_t character) {
    return std::iswalnum(character) != 0 || character == L'.' || character == L'_' ||
        character == L'-' || character == L'!';
}

bool IsPrintableTextCharacter(wchar_t character) {
    return character >= L' ' && character != 0x7f && (character < 0x80 || character > 0x9f) &&
        character != 0xfffe && character != 0xffff;
}

bool IsAppsFolderTarget(const std::wstring& target) {
    return target.size() > std::size(kAppsFolderPrefix) - 1 &&
        EqualInsensitive(std::wstring_view(target).substr(0, std::size(kAppsFolderPrefix) - 1),
            std::wstring_view(kAppsFolderPrefix, std::size(kAppsFolderPrefix) - 1)) &&
        target.find(L'!') != std::wstring::npos;
}

void AppendUniquePin(std::vector<PinnedApp>& pins, PinnedApp app) {
    app.target = ExpandTarget(app.target);
    if (app.target.empty() || pins.size() >= kMaximumPins) {
        return;
    }

    const std::wstring normalized = NormalizedTarget(app.target);
    const bool duplicate = std::ranges::any_of(pins, [&normalized](const PinnedApp& existing) {
        return EqualInsensitive(NormalizedTarget(existing.target), normalized);
    });
    if (duplicate) {
        return;
    }

    if (app.name.empty()) {
        app.name = FileNameWithoutExtension(app.target);
    }
    pins.push_back(std::move(app));
}

std::wstring ShellDisplayName(const std::wstring& target) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return {};
    }

    PWSTR name = nullptr;
    const HRESULT result = item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
    item->Release();
    if (FAILED(result) || name == nullptr) {
        return {};
    }
    std::wstring displayName(name);
    CoTaskMemFree(name);
    return displayName;
}

void AppendAppsFolderPin(std::vector<PinnedApp>& pins, const std::wstring& target,
    bool canResolveShellLinks) {
    if (!IsAppsFolderTarget(target)) {
        return;
    }

    PinnedApp app;
    app.target = target;
    if (canResolveShellLinks) {
        app.name = ShellDisplayName(target);
    }
    AppendUniquePin(pins, std::move(app));
}

void ExtractAppsFolderTargets(const std::wstring& text, std::vector<std::wstring>& targets) {
    const std::wstring lower = ToLower(text);
    const std::wstring_view prefix(kAppsFolderPrefix, std::size(kAppsFolderPrefix) - 1);
    size_t position = lower.find(prefix);
    while (position != std::wstring::npos) {
        const size_t start = position + prefix.size();
        size_t end = start;
        while (end < text.size() && IsAumidCharacter(text[end])) {
            ++end;
        }
        if (end > start) {
            const std::wstring target = std::wstring(kAppsFolderPrefix) + text.substr(start, end - start);
            if (IsAppsFolderTarget(target)) {
                targets.push_back(target);
            }
        }
        position = lower.find(prefix, end);
    }
}

using ShellLinkTextMethod = HRESULT(STDMETHODCALLTYPE IShellLinkW::*)(LPWSTR, int);

std::wstring ShellLinkText(IShellLinkW* link, ShellLinkTextMethod method) {
    std::array<wchar_t, kMaximumPathLength> text{};
    if (FAILED((link->*method)(text.data(), static_cast<int>(text.size())))) {
        return {};
    }
    return text.data();
}

bool ResolveShortcut(const std::wstring& shortcut, PinnedApp& app) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&link)))) {
        return false;
    }

    IPersistFile* persist = nullptr;
    const HRESULT persistResult = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (FAILED(persistResult)) {
        link->Release();
        return false;
    }

    const bool loaded = SUCCEEDED(persist->Load(shortcut.c_str(), STGM_READ));
    persist->Release();
    if (!loaded) {
        link->Release();
        return false;
    }

    link->Resolve(nullptr, SLR_NO_UI | SLR_NOSEARCH | SLR_NOTRACK);
    std::array<wchar_t, kMaximumPathLength> target{};
    WIN32_FIND_DATAW findData{};
    const bool hasTarget = SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()),
        &findData, SLGP_RAWPATH));
    const std::wstring arguments = ShellLinkText(link, &IShellLinkW::GetArguments);
    const std::wstring workingDirectory = ShellLinkText(link, &IShellLinkW::GetWorkingDirectory);
    const std::wstring description = ShellLinkText(link, &IShellLinkW::GetDescription);
    link->Release();

    std::vector<std::wstring> appsFolderTargets;
    ExtractAppsFolderTargets(target.data(), appsFolderTargets);
    ExtractAppsFolderTargets(arguments, appsFolderTargets);
    if (!appsFolderTargets.empty()) {
        app.target = std::move(appsFolderTargets.front());
        app.arguments.clear();
        app.workingDirectory.clear();
        app.name = description.empty() ? FileNameWithoutExtension(shortcut) : description;
        return true;
    }

    if (!hasTarget || target.front() == L'\0') {
        return false;
    }

    app.target = target.data();
    app.arguments = arguments;
    app.workingDirectory = workingDirectory;
    app.name = description.empty() ? FileNameWithoutExtension(shortcut) : description;
    if (app.name.empty()) {
        app.name = FileNameWithoutExtension(app.target);
    }
    return true;
}

bool IsExistingFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void ImportShortcutFolder(std::vector<PinnedApp>& pins, bool canResolveShellLinks) {
    if (!canResolveShellLinks) {
        return;
    }

    const std::wstring appData = EnvironmentVariable(L"APPDATA");
    if (appData.empty()) {
        return;
    }

    const std::wstring folder = appData +
        L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";
    WIN32_FIND_DATAW entry{};
    const HANDLE search = FindFirstFileW((folder + L"\\*.lnk").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }

        PinnedApp app;
        if (ResolveShortcut(folder + L"\\" + entry.cFileName, app)) {
            AppendUniquePin(pins, std::move(app));
        }
    } while (FindNextFileW(search, &entry) != FALSE);
    FindClose(search);
}

std::vector<BYTE> ReadRegistryBinaryValue(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD byteCount = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &byteCount) != ERROR_SUCCESS ||
        type != REG_BINARY || byteCount == 0) {
        return {};
    }

    std::vector<BYTE> value(byteCount);
    if (RegQueryValueExW(key, name, nullptr, &type, value.data(), &byteCount) != ERROR_SUCCESS ||
        type != REG_BINARY) {
        return {};
    }
    value.resize(byteCount);
    return value;
}

bool IsPathSeparator(wchar_t character) {
    return character == L'\\' || character == L'/';
}

bool IsTaskbandPathStart(const std::wstring& text, size_t index) {
    if (index + 2 < text.size() && std::iswalpha(text[index]) != 0 && text[index + 1] == L':' &&
        IsPathSeparator(text[index + 2])) {
        return true;
    }
    if (index + 1 < text.size() && text[index] == L'\\' && text[index + 1] == L'\\') {
        return true;
    }
    if (text[index] != L'%') {
        return false;
    }

    const size_t variableEnd = text.find(L'%', index + 1);
    return variableEnd != std::wstring::npos && variableEnd + 1 < text.size() &&
        IsPathSeparator(text[variableEnd + 1]);
}

bool HasExtensionAt(const std::wstring& text, size_t offset, std::wstring_view extension) {
    return offset + extension.size() <= text.size() &&
        EqualInsensitive(std::wstring_view(text).substr(offset, extension.size()), extension);
}

void ExtractTargetsFromText(const std::wstring& text, std::vector<std::wstring>& targets) {
    struct LocatedTarget {
        size_t position = 0;
        std::wstring target;
    };

    std::vector<LocatedTarget> locatedTargets;
    const std::wstring lower = ToLower(text);
    const std::wstring_view appsFolderPrefix(kAppsFolderPrefix, std::size(kAppsFolderPrefix) - 1);
    for (size_t position = lower.find(appsFolderPrefix); position != std::wstring::npos;) {
        const size_t start = position + appsFolderPrefix.size();
        size_t end = start;
        while (end < text.size() && IsAumidCharacter(text[end])) {
            ++end;
        }
        if (end > start) {
            const std::wstring target = std::wstring(kAppsFolderPrefix) + text.substr(start, end - start);
            if (IsAppsFolderTarget(target)) {
                locatedTargets.push_back({position, target});
            }
        }
        position = lower.find(appsFolderPrefix, end);
    }

    for (size_t bang = text.find(L'!'); bang != std::wstring::npos;
         bang = text.find(L'!', bang + 1U)) {
        size_t start = bang;
        while (start > 0 && IsAumidCharacter(text[start - 1U])) {
            --start;
        }
        size_t end = bang + 1U;
        while (end < text.size() && IsAumidCharacter(text[end])) {
            ++end;
        }
        const std::wstring aumid = text.substr(start, end - start);
        if (aumid.find(L'_') != std::wstring::npos) {
            const std::wstring target = std::wstring(kAppsFolderPrefix) + aumid;
            if (IsAppsFolderTarget(target)) {
                locatedTargets.push_back({start, target});
            }
        }
    }

    for (size_t start = 0; start < text.size(); ++start) {
        if (!IsTaskbandPathStart(text, start)) {
            continue;
        }

        for (size_t extension = start; extension < text.size(); ++extension) {
            const bool lnk = HasExtensionAt(text, extension, L".lnk");
            const bool executable = HasExtensionAt(text, extension, L".exe");
            if (!lnk && !executable) {
                continue;
            }

            const size_t end = extension + 4U;
            if (end < text.size() && IsPathSeparator(text[end])) {
                continue;
            }
            locatedTargets.push_back({start, text.substr(start, end - start)});
            start = end - 1U;
            break;
        }
    }

    std::stable_sort(locatedTargets.begin(), locatedTargets.end(), [](const LocatedTarget& left,
        const LocatedTarget& right) {
        return left.position < right.position;
    });
    for (LocatedTarget& target : locatedTargets) {
        targets.push_back(std::move(target.target));
    }
}

void ExtractUtf16Targets(const std::vector<BYTE>& value, size_t alignment,
    std::vector<std::wstring>& targets) {
    std::wstring text;
    for (size_t offset = alignment; offset + sizeof(uint16_t) <= value.size();
         offset += sizeof(uint16_t)) {
        uint16_t codeUnit = 0;
        std::memcpy(&codeUnit, value.data() + offset, sizeof(codeUnit));
        if (IsPrintableTextCharacter(static_cast<wchar_t>(codeUnit))) {
            text.push_back(static_cast<wchar_t>(codeUnit));
            if (text.size() == kMaximumPathLength) {
                ExtractTargetsFromText(text, targets);
                text.clear();
            }
        } else if (!text.empty()) {
            ExtractTargetsFromText(text, targets);
            text.clear();
        }
    }
    ExtractTargetsFromText(text, targets);
}

void ExtractAsciiTargets(const std::vector<BYTE>& value, std::vector<std::wstring>& targets) {
    std::wstring text;
    for (const BYTE byte : value) {
        if (byte >= 0x20U && byte <= 0x7eU) {
            text.push_back(static_cast<wchar_t>(byte));
            if (text.size() == kMaximumPathLength) {
                ExtractTargetsFromText(text, targets);
                text.clear();
            }
        } else if (!text.empty()) {
            ExtractTargetsFromText(text, targets);
            text.clear();
        }
    }
    ExtractTargetsFromText(text, targets);
}

void ExtractUtf8Targets(const std::vector<BYTE>& value, std::vector<std::wstring>& targets) {
    constexpr size_t maximumUtf8TextLength = kMaximumPathLength * 4U;
    std::string text;
    for (const BYTE byte : value) {
        if (byte >= 0x20U && byte != 0x7fU) {
            text.push_back(static_cast<char>(byte));
            if (text.size() == maximumUtf8TextLength) {
                const std::wstring decoded = Utf8ToWide(text);
                if (!decoded.empty() && std::ranges::all_of(decoded, IsPrintableTextCharacter)) {
                    ExtractTargetsFromText(decoded, targets);
                }
                text.clear();
            }
        } else if (!text.empty()) {
            const std::wstring decoded = Utf8ToWide(text);
            if (!decoded.empty() && std::ranges::all_of(decoded, IsPrintableTextCharacter)) {
                ExtractTargetsFromText(decoded, targets);
            }
            text.clear();
        }
    }
    const std::wstring decoded = Utf8ToWide(text);
    if (!decoded.empty() && std::ranges::all_of(decoded, IsPrintableTextCharacter)) {
        ExtractTargetsFromText(decoded, targets);
    }
}

std::vector<std::wstring> ExtractTaskbarTargets(const std::vector<BYTE>& value) {
    std::vector<std::wstring> targets;
    for (size_t alignment = 0; alignment < 2; ++alignment) {
        ExtractUtf16Targets(value, alignment, targets);
    }
    ExtractAsciiTargets(value, targets);
    ExtractUtf8Targets(value, targets);
    return targets;
}

void ImportTaskbarTarget(std::vector<PinnedApp>& pins, const std::wstring& candidate,
    bool canResolveShellLinks) {
    if (IsAppsFolderTarget(candidate)) {
        AppendAppsFolderPin(pins, candidate, canResolveShellLinks);
        return;
    }

    const std::wstring path = ExpandTarget(candidate);
    if (EndsWithInsensitive(path, L".lnk")) {
        PinnedApp app;
        if (canResolveShellLinks && IsExistingFile(path) && ResolveShortcut(path, app)) {
            AppendUniquePin(pins, std::move(app));
        }
        return;
    }

    if (EndsWithInsensitive(path, L".exe") && IsExistingFile(path)) {
        AppendUniquePin(pins, {FileNameWithoutExtension(path), path, L"", L""});
    }
}

void ImportTaskbandValues(std::vector<PinnedApp>& pins, bool canResolveShellLinks) {
    HKEY taskband = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband", 0,
            KEY_QUERY_VALUE, &taskband) != ERROR_SUCCESS) {
        return;
    }

    constexpr std::array valueNames = {L"Favorites", L"FavoritesResolve"};
    for (const wchar_t* name : valueNames) {
        for (const std::wstring& target : ExtractTaskbarTargets(ReadRegistryBinaryValue(taskband, name))) {
            ImportTaskbarTarget(pins, target, canResolveShellLinks);
        }
    }
    RegCloseKey(taskband);
}

void ImportCloudStoreTaskbarKey(HKEY key, bool isTaskbarKey, std::vector<PinnedApp>& pins,
    bool canResolveShellLinks) {
    DWORD subkeyCount = 0;
    DWORD longestSubkey = 0;
    DWORD valueCount = 0;
    DWORD longestValueName = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeyCount, &longestSubkey, nullptr,
            &valueCount, &longestValueName, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        return;
    }

    if (isTaskbarKey) {
        std::vector<wchar_t> valueName(static_cast<size_t>(longestValueName) + 1U, L'\0');
        for (DWORD index = 0; index < valueCount; ++index) {
            DWORD nameLength = static_cast<DWORD>(valueName.size());
            DWORD type = 0;
            DWORD byteCount = 0;
            const LONG query = RegEnumValueW(key, index, valueName.data(), &nameLength, nullptr,
                &type, nullptr, &byteCount);
            if (query != ERROR_SUCCESS || type != REG_BINARY || byteCount == 0) {
                continue;
            }

            std::vector<BYTE> value(byteCount);
            nameLength = static_cast<DWORD>(valueName.size());
            if (RegEnumValueW(key, index, valueName.data(), &nameLength, nullptr, &type,
                    value.data(), &byteCount) != ERROR_SUCCESS || type != REG_BINARY) {
                continue;
            }
            value.resize(byteCount);
            for (const std::wstring& target : ExtractTaskbarTargets(value)) {
                ImportTaskbarTarget(pins, target, canResolveShellLinks);
            }
        }
    }

    std::vector<wchar_t> subkeyName(static_cast<size_t>(longestSubkey) + 1U, L'\0');
    for (DWORD index = 0; index < subkeyCount; ++index) {
        DWORD nameLength = static_cast<DWORD>(subkeyName.size());
        const LONG enumerated = RegEnumKeyExW(key, index, subkeyName.data(), &nameLength, nullptr,
            nullptr, nullptr, nullptr);
        if (enumerated != ERROR_SUCCESS) {
            continue;
        }

        const std::wstring name(subkeyName.data(), nameLength);
        HKEY child = nullptr;
        if (RegOpenKeyExW(key, name.c_str(), 0, KEY_READ, &child) != ERROR_SUCCESS) {
            continue;
        }
        const bool childIsTaskbarKey = isTaskbarKey ||
            ToLower(name).find(L"taskbar") != std::wstring::npos;
        ImportCloudStoreTaskbarKey(child, childIsTaskbarKey, pins, canResolveShellLinks);
        RegCloseKey(child);
    }
}

void ImportCloudStoreTaskbarValues(std::vector<PinnedApp>& pins, bool canResolveShellLinks) {
    HKEY cloudStore = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kCloudStorePath, 0, KEY_READ, &cloudStore) != ERROR_SUCCESS) {
        return;
    }
    ImportCloudStoreTaskbarKey(cloudStore, false, pins, canResolveShellLinks);
    RegCloseKey(cloudStore);
}

std::wstring ReadLayoutText(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 2U) {
        const auto first = static_cast<unsigned char>(bytes[0]);
        const auto second = static_cast<unsigned char>(bytes[1]);
        const bool littleEndian = first == 0xffU && second == 0xfeU;
        const bool bigEndian = first == 0xfeU && second == 0xffU;
        if (littleEndian || bigEndian) {
            if ((bytes.size() - 2U) % 2U != 0U) {
                return {};
            }

            std::wstring text;
            text.reserve((bytes.size() - 2U) / 2U);
            for (size_t offset = 2U; offset < bytes.size(); offset += 2U) {
                const auto low = static_cast<uint16_t>(static_cast<unsigned char>(bytes[offset]));
                const auto high =
                    static_cast<uint16_t>(static_cast<unsigned char>(bytes[offset + 1U]));
                const uint16_t codeUnit = littleEndian
                    ? static_cast<uint16_t>(low | static_cast<uint16_t>(high << 8U))
                    : static_cast<uint16_t>(high | static_cast<uint16_t>(low << 8U));
                text.push_back(static_cast<wchar_t>(codeUnit));
            }
            return text;
        }
    }

    const size_t utf8BomLength = bytes.size() >= 3U &&
            static_cast<unsigned char>(bytes[0]) == 0xefU &&
            static_cast<unsigned char>(bytes[1]) == 0xbbU &&
            static_cast<unsigned char>(bytes[2]) == 0xbfU
        ? 3U
        : 0U;
    return Utf8ToWide(bytes.substr(utf8BomLength));
}

bool IsXmlAttributeStart(const std::wstring& text, size_t position) {
    return position == 0 || std::iswspace(text[position - 1U]) != 0 || text[position - 1U] == L'<';
}

std::wstring DecodeXmlEntities(const std::wstring& value) {
    std::wstring decoded;
    decoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const std::wstring_view remaining(value.data() + index, value.size() - index);
        if (StartsWithInsensitive(remaining, L"&amp;")) {
            decoded.push_back(L'&');
            index += 4U;
        } else if (StartsWithInsensitive(remaining, L"&quot;")) {
            decoded.push_back(L'\"');
            index += 5U;
        } else if (StartsWithInsensitive(remaining, L"&apos;")) {
            decoded.push_back(L'\'');
            index += 5U;
        } else if (StartsWithInsensitive(remaining, L"&lt;")) {
            decoded.push_back(L'<');
            index += 3U;
        } else if (StartsWithInsensitive(remaining, L"&gt;")) {
            decoded.push_back(L'>');
            index += 3U;
        } else {
            decoded.push_back(value[index]);
        }
    }
    return decoded;
}

void ImportLayoutTargets(const std::wstring& text, std::vector<PinnedApp>& pins,
    bool canResolveShellLinks) {
    constexpr std::wstring_view appUserModelIdAttribute = L"appusermodelid";
    constexpr std::wstring_view desktopLinkPathAttribute = L"desktopapplicationlinkpath";

    size_t cursor = 0;
    while (cursor < text.size()) {
        const std::wstring_view remaining(text.data() + cursor, text.size() - cursor);
        const bool appUserModelId = IsXmlAttributeStart(text, cursor) &&
            StartsWithInsensitive(remaining, appUserModelIdAttribute);
        const bool desktopLinkPath = IsXmlAttributeStart(text, cursor) &&
            StartsWithInsensitive(remaining, desktopLinkPathAttribute);
        if (!appUserModelId && !desktopLinkPath) {
            ++cursor;
            continue;
        }

        const size_t attributeLength =
            appUserModelId ? appUserModelIdAttribute.size() : desktopLinkPathAttribute.size();
        size_t equals = cursor + attributeLength;
        while (equals < text.size() && std::iswspace(text[equals]) != 0) {
            ++equals;
        }
        if (equals == text.size() || text[equals] != L'=') {
            cursor += attributeLength;
            continue;
        }

        ++equals;
        while (equals < text.size() && std::iswspace(text[equals]) != 0) {
            ++equals;
        }
        if (equals == text.size()) {
            return;
        }

        const wchar_t quote = text[equals];
        const bool quoted = quote == L'\"' || quote == L'\'';
        const size_t valueStart = quoted ? equals + 1U : equals;
        size_t valueEnd = valueStart;
        if (quoted) {
            valueEnd = text.find(quote, valueStart);
            if (valueEnd == std::wstring::npos) {
                return;
            }
        } else {
            while (valueEnd < text.size() && std::iswspace(text[valueEnd]) == 0 &&
                text[valueEnd] != L'>' && text[valueEnd] != L'/') {
                ++valueEnd;
            }
        }

        const std::wstring value = DecodeXmlEntities(text.substr(valueStart, valueEnd - valueStart));
        if (appUserModelId) {
            const std::wstring target = IsAppsFolderTarget(value)
                ? value
                : std::wstring(kAppsFolderPrefix) + value;
            ImportTaskbarTarget(pins, target, canResolveShellLinks);
        } else if (EndsWithInsensitive(value, L".lnk")) {
            ImportTaskbarTarget(pins, value, canResolveShellLinks);
        }
        cursor = quoted ? valueEnd + 1U : valueEnd;
    }
}

void ImportTaskbarLayouts(std::vector<PinnedApp>& pins, bool canResolveShellLinks) {
    const std::wstring appData = EnvironmentVariable(L"LOCALAPPDATA");
    if (appData.empty()) {
        return;
    }

    const std::wstring directory = appData + L"\\Microsoft\\Windows\\Shell\\";
    constexpr std::array layoutFiles = {L"LayoutModification.xml", L"DefaultLayouts.xml"};
    for (const wchar_t* file : layoutFiles) {
        ImportLayoutTargets(ReadLayoutText(directory + file), pins, canResolveShellLinks);
    }
}

std::vector<PinnedApp> ImportTaskbarPins() {
    std::vector<PinnedApp> pins;
    ComApartment apartment;
    const bool canResolveShellLinks = apartment.CanUseShellLinks();

    // CloudStore and Taskband preserve serialized taskbar order. Filesystem-based sources are
    // fallbacks so directory enumeration never changes a successful serialized order.
    ImportCloudStoreTaskbarValues(pins, canResolveShellLinks);
    if (pins.empty()) {
        ImportTaskbandValues(pins, canResolveShellLinks);
    }
    if (pins.empty()) {
        ImportShortcutFolder(pins, canResolveShellLinks);
    }
    if (pins.empty()) {
        ImportTaskbarLayouts(pins, canResolveShellLinks);
    }
    return pins;
}

bool SamePins(const std::vector<PinnedApp>& left, const std::vector<PinnedApp>& right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](const PinnedApp& lhs,
            const PinnedApp& rhs) {
            return lhs.name == rhs.name && lhs.target == rhs.target &&
                lhs.arguments == rhs.arguments && lhs.workingDirectory == rhs.workingDirectory;
        });
}

}  // namespace

bool DockConfig::LoadOrCreate() {
    m_path = AppDataConfigPath();
    if (m_path.empty()) {
        m_followsTaskbarPins = true;
        m_pins = ImportTaskbarPins();
        m_showDevBounds = false;
        m_launchAtStartup = false;
        m_checkForUpdates = true;
        if (m_pins.empty()) {
            SetDefaults();
        }
        return false;
    }

    if (GetFileAttributesW(m_path.c_str()) == INVALID_FILE_ATTRIBUTES || !Load()) {
        m_followsTaskbarPins = true;
        m_pins = ImportTaskbarPins();
        m_showDevBounds = false;
        m_launchAtStartup = false;
        m_checkForUpdates = true;
        if (m_pins.empty()) {
            SetDefaults();
        }
        return Save();
    }

    if (!m_followsTaskbarPins) {
        return true;
    }

    std::vector<PinnedApp> imported = ImportTaskbarPins();
    if (!imported.empty() && !SamePins(m_pins, imported)) {
        m_pins = std::move(imported);
    }

    // Rewriting configurations that follow taskbar pins also migrates configurations created
    // before FollowTaskbarPins existed.
    return Save();
}

bool DockConfig::Load() {
    std::ifstream file(m_path, std::ios::binary);
    if (!file) {
        return false;
    }

    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::wstring contents = Utf8ToWide(bytes);
    if (contents.empty() && !bytes.empty()) {
        return false;
    }

    std::map<std::wstring, std::map<std::wstring, std::wstring>> sections;
    std::wstring currentSection;
    std::wistringstream lines(contents);
    std::wstring line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty() || line.front() == L';' || line.front() == L'#') {
            continue;
        }

        if (line.front() == L'[' && line.back() == L']') {
            currentSection = ToLower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const size_t separator = line.find(L'=');
        if (separator == std::wstring::npos || currentSection.empty()) {
            continue;
        }

        sections[currentSection][ToLower(Trim(line.substr(0, separator)))] =
            Trim(line.substr(separator + 1));
    }

    m_pins.clear();
    m_showDevBounds = false;
    m_followsTaskbarPins = true;
    m_dockScale = 1.0F;
    m_typeSafeApiKey.clear();
    m_launchAtStartup = false;
    m_checkForUpdates = true;
    m_lastInstalledVersion.clear();
    m_lastInstalledTime = 0;
    m_lastInstalledAttempts = 0;
    const auto dockSection = sections.find(L"dock");
    if (dockSection != sections.end()) {
        const auto showDevBounds = dockSection->second.find(L"showdevbounds");
        if (showDevBounds != dockSection->second.end()) {
            m_showDevBounds = ParseBoolean(showDevBounds->second);
        }
        const auto followTaskbarPins = dockSection->second.find(L"followtaskbarpins");
        if (followTaskbarPins != dockSection->second.end()) {
            m_followsTaskbarPins = ParseBoolean(followTaskbarPins->second);
        }
        const auto dockScale = dockSection->second.find(L"scale");
        if (dockScale != dockSection->second.end()) {
            m_dockScale = std::clamp(ParseFloat(dockScale->second, 1.0F), 0.75F, 1.5F);
        }
        const auto typeSafeApiKey = dockSection->second.find(L"typesafeapikey");
        if (typeSafeApiKey != dockSection->second.end()) {
            m_typeSafeApiKey = typeSafeApiKey->second;
        }
        const auto launchAtStartup = dockSection->second.find(L"launchatstartup");
        if (launchAtStartup != dockSection->second.end()) {
            m_launchAtStartup = ParseBoolean(launchAtStartup->second);
        }
        const auto checkForUpdates = dockSection->second.find(L"checkforupdates");
        if (checkForUpdates != dockSection->second.end()) {
            m_checkForUpdates = ParseBoolean(checkForUpdates->second);
        }
        const auto lastInstalledVersion =
            dockSection->second.find(L"lastinstalledversion");
        if (lastInstalledVersion != dockSection->second.end()) {
            m_lastInstalledVersion = lastInstalledVersion->second;
        }
        const auto lastInstalledTime = dockSection->second.find(L"lastinstalledtime");
        if (lastInstalledTime != dockSection->second.end()) {
            try {
                m_lastInstalledTime = std::stoll(lastInstalledTime->second);
            } catch (...) {
                m_lastInstalledTime = 0;
            }
            if (m_lastInstalledTime < 0) {
                m_lastInstalledTime = 0;
            }
        }
        const auto lastInstalledAttempts =
            dockSection->second.find(L"lastinstalledattempts");
        if (lastInstalledAttempts != dockSection->second.end()) {
            try {
                m_lastInstalledAttempts =
                    std::clamp(std::stoi(lastInstalledAttempts->second), 0, 1000);
            } catch (...) {
                m_lastInstalledAttempts = 0;
            }
        }
    }

    for (size_t index = 0; index < kMaximumPins; ++index) {
        const std::wstring sectionName = L"pin" + std::to_wstring(index);
        const auto section = sections.find(sectionName);
        if (section == sections.end()) {
            continue;
        }

        const auto& values = section->second;
        const auto target = values.find(L"target");
        if (target == values.end() || target->second.empty()) {
            continue;
        }

        PinnedApp app;
        app.name = values.contains(L"name") ? values.at(L"name") : target->second;
        app.target = ExpandTarget(target->second);
        app.arguments = values.contains(L"arguments") ? values.at(L"arguments") : L"";
        app.workingDirectory =
            values.contains(L"workingdirectory") ? ExpandTarget(values.at(L"workingdirectory")) : L"";
        m_pins.push_back(std::move(app));
    }

    return true;
}

bool DockConfig::Save() const {
    if (m_path.empty()) {
        return false;
    }

    std::wostringstream contents;
    contents << L"; Liquid Glass Dock v1 configuration\n";
    contents << L"; UTF-8 INI. Pins are ordered left to right.\n\n";
    contents << L"[Dock]\n";
    contents << L"ShowDevBounds=" << (m_showDevBounds ? L"1" : L"0") << L"\n";
    contents << L"FollowTaskbarPins=" << (m_followsTaskbarPins ? L"1" : L"0") << L"\n";
    contents << L"Scale=" << m_dockScale << L"\n";
    contents << L"LaunchAtStartup=" << (m_launchAtStartup ? L"1" : L"0") << L"\n";
    contents << L"CheckForUpdates=" << (m_checkForUpdates ? L"1" : L"0") << L"\n";
    if (!m_lastInstalledVersion.empty()) {
        contents << L"LastInstalledVersion=" << m_lastInstalledVersion << L"\n";
        contents << L"LastInstalledTime=" << m_lastInstalledTime << L"\n";
        contents << L"LastInstalledAttempts=" << m_lastInstalledAttempts << L"\n";
    }
    if (!m_typeSafeApiKey.empty()) {
        contents << L"TypeSafeApiKey=" << m_typeSafeApiKey << L"\n";
    }
    contents << L"\n";

    for (size_t index = 0; index < m_pins.size(); ++index) {
        const PinnedApp& app = m_pins[index];
        contents << L"[Pin" << index << L"]\n";
        contents << L"Name=" << app.name << L"\n";
        contents << L"Target=" << app.target << L"\n";
        contents << L"Arguments=" << app.arguments << L"\n";
        contents << L"WorkingDirectory=" << app.workingDirectory << L"\n\n";
    }

    const std::string bytes = WideToUtf8(contents.str());
    std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

const std::vector<PinnedApp>& DockConfig::Pins() const noexcept {
    return m_pins;
}

std::vector<PinnedApp>& DockConfig::Pins() noexcept {
    return m_pins;
}

bool DockConfig::ShowDevBounds() const noexcept {
    return m_showDevBounds;
}

void DockConfig::SetShowDevBounds(bool enabled) noexcept {
    m_showDevBounds = enabled;
}

bool DockConfig::FollowsTaskbarPins() const noexcept {
    return m_followsTaskbarPins;
}

void DockConfig::StopFollowingTaskbarPins() noexcept {
    m_followsTaskbarPins = false;
}

float DockConfig::DockScale() const noexcept {
    return m_dockScale;
}

void DockConfig::SetDockScale(float scale) noexcept {
    m_dockScale = std::clamp(scale, 0.75F, 1.5F);
}

bool DockConfig::LaunchAtStartup() const noexcept {
    return m_launchAtStartup;
}

void DockConfig::SetLaunchAtStartup(bool enabled) noexcept {
    m_launchAtStartup = enabled;
}

bool DockConfig::CheckForUpdates() const noexcept {
    return m_checkForUpdates;
}

void DockConfig::SetCheckForUpdates(bool enabled) noexcept {
    m_checkForUpdates = enabled;
}

std::wstring DockConfig::LastInstalledVersion() const {
    return m_lastInstalledVersion;
}

long long DockConfig::LastInstalledTime() const noexcept {
    return m_lastInstalledTime;
}

void DockConfig::SetLastInstalledVersion(const std::wstring& version, long long unixTime) {
    m_lastInstalledVersion = version;
    m_lastInstalledTime = unixTime < 0 ? 0 : unixTime;
}

int DockConfig::LastInstalledAttempts() const noexcept {
    return m_lastInstalledAttempts;
}

void DockConfig::SetLastInstalledAttempts(int attempts) noexcept {
    m_lastInstalledAttempts = std::clamp(attempts, 0, 1000);
}

const std::wstring& DockConfig::Path() const noexcept {
    return m_path;
}

std::wstring DockConfig::TypeSafeApiKey() const {
    const std::wstring fromEnvironment = Trim(EnvironmentVariable(L"TYPESAFE_API_KEY"));
    if (!fromEnvironment.empty()) {
        return fromEnvironment;
    }
    return Trim(m_typeSafeApiKey);
}

void DockConfig::SetDefaults() {
    m_pins = {
        {L"File Explorer", ExpandTarget(L"%SystemRoot%\\explorer.exe"), L"", L""},
        {L"Notepad", ExpandTarget(L"%SystemRoot%\\System32\\notepad.exe"), L"", L""},
        {L"Calculator", L"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", L"", L""},
    };
    m_showDevBounds = false;
    m_dockScale = 1.0F;
    m_launchAtStartup = false;
    m_checkForUpdates = true;
}
