#include "Startup.h"

#include <Windows.h>

#include <array>

namespace {

constexpr wchar_t kRunKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"Hoverdock";

std::wstring Quoted(const std::wstring& path) {
    return L"\"" + path + L"\"";
}

}  // namespace

std::wstring Startup::CurrentExecutablePath() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

std::wstring Startup::StartupCommand() {
    const std::wstring exe = CurrentExecutablePath();
    if (exe.empty()) {
        return {};
    }
    return Quoted(exe);
}

bool Startup::IsEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[32768]{};
    DWORD byteCount = sizeof(value);
    DWORD type = 0;
    const LONG queried =
        RegQueryValueExW(key, kRunValueName, nullptr, &type, reinterpret_cast<BYTE*>(value),
            &byteCount);
    RegCloseKey(key);
    if (queried != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }
    // Any value counts as enabled; SyncWithConfig repairs stale paths below.
    return value[0] != L'\0';
}

bool Startup::SetEnabled(bool enabled) {
    if (!enabled) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) !=
            ERROR_SUCCESS) {
            return false;
        }
        const LONG deleted = RegDeleteValueW(key, kRunValueName);
        RegCloseKey(key);
        return deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND;
    }

    const std::wstring command = StartupCommand();
    if (command.empty()) {
        return false;
    }
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
            &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG written = RegSetValueExW(key, kRunValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1U) * sizeof(wchar_t)));
    RegCloseKey(key);
    return written == ERROR_SUCCESS;
}

void Startup::SyncWithConfig(bool configEnabled) {
    const std::wstring command = StartupCommand();
    if (command.empty()) {
        return;
    }

    HKEY key = nullptr;
    const bool opened =
        RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) ==
        ERROR_SUCCESS;
    if (!opened) {
        if (configEnabled) {
            static_cast<void>(SetEnabled(true));
        }
        return;
    }

    wchar_t current[32768]{};
    DWORD byteCount = sizeof(current);
    DWORD type = 0;
    const LONG queried = RegQueryValueExW(key, kRunValueName, nullptr, &type,
        reinterpret_cast<BYTE*>(current), &byteCount);
    const bool hasValue = queried == ERROR_SUCCESS && type == REG_SZ && current[0] != L'\0';
    RegCloseKey(key);

    if (!configEnabled) {
        if (hasValue) {
            // The toggle is authoritative: a leftover installer/startup entry is
            // removed so "off" really means off.
            static_cast<void>(SetEnabled(false));
        }
        return;
    }

    if (!hasValue || command != std::wstring(current)) {
        static_cast<void>(SetEnabled(true));
    }
}
