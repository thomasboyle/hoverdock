#pragma once

#include "DockConfig.h"

#include <Windows.h>

#include <mutex>
#include <string>
#include <vector>

struct InstalledApp {
    PinnedApp launch;
    std::wstring shortcutName;
    std::wstring executableName;
    std::wstring executablePath;
    std::wstring aumid;
    std::wstring description;
    std::wstring productName;
    std::wstring publisher;
    std::wstring startMenuFolder;
    std::wstring comment;
};

class InstalledAppCatalog {
public:
    void EnsureLoaded();
    // Drop the catalogue when search UI closes so idle dock does not keep every
    // Start Menu shortcut string resident.
    void Clear() noexcept;
    [[nodiscard]] std::vector<InstalledApp> Snapshot() const;

private:
    mutable std::mutex m_mutex;
    std::vector<InstalledApp> m_apps;
    bool m_loaded = false;
};
