#include "DockConfig.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>

namespace {

constexpr wchar_t kConfigDirectoryName[] = L"LiquidGlassDock";
constexpr wchar_t kConfigFileName[] = L"dock.ini";
constexpr size_t kMaximumPins = 16;

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

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
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
    if (text.empty()) {
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

std::wstring AppDataConfigPath() {
    wchar_t appData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }

    std::wstring directory = std::wstring(appData) + L"\\" + kConfigDirectoryName;
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\" + kConfigFileName;
}

bool ParseBoolean(const std::wstring& value) {
    const std::wstring lower = ToLower(Trim(value));
    return lower == L"1" || lower == L"true" || lower == L"yes";
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

}  // namespace

bool DockConfig::LoadOrCreate() {
    m_path = AppDataConfigPath();
    if (m_path.empty()) {
        SetDefaults();
        return false;
    }

    if (GetFileAttributesW(m_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SetDefaults();
        return Save();
    }

    if (!Load()) {
        SetDefaults();
        return Save();
    }
    return true;
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
    m_showDevBounds = ParseBoolean(sections[L"dock"][L"showdevbounds"]);

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

    if (m_pins.empty()) {
        SetDefaults();
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
    contents << L"ShowDevBounds=" << (m_showDevBounds ? L"1" : L"0") << L"\n\n";

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

const std::wstring& DockConfig::Path() const noexcept {
    return m_path;
}

void DockConfig::SetDefaults() {
    m_pins = {
        {L"File Explorer", ExpandTarget(L"%SystemRoot%\\explorer.exe"), L"", L""},
        {L"Notepad", ExpandTarget(L"%SystemRoot%\\System32\\notepad.exe"), L"", L""},
        {L"Calculator", L"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", L"", L""},
    };
    m_showDevBounds = false;
}
