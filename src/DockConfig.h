#pragma once

#include <string>
#include <vector>

struct PinnedApp {
    std::wstring name;
    std::wstring target;
    std::wstring arguments;
    std::wstring workingDirectory;
};

class DockConfig {
public:
    bool LoadOrCreate();
    bool Save() const;

    [[nodiscard]] const std::vector<PinnedApp>& Pins() const noexcept;
    [[nodiscard]] std::vector<PinnedApp>& Pins() noexcept;
    [[nodiscard]] bool ShowDevBounds() const noexcept;
    void SetShowDevBounds(bool enabled) noexcept;
    [[nodiscard]] bool FollowsTaskbarPins() const noexcept;
    void StopFollowingTaskbarPins() noexcept;
    [[nodiscard]] float DockScale() const noexcept;
    void SetDockScale(float scale) noexcept;
    [[nodiscard]] const std::wstring& Path() const noexcept;
    [[nodiscard]] std::wstring TypeSafeApiKey() const;
    [[nodiscard]] bool LaunchAtStartup() const noexcept;
    void SetLaunchAtStartup(bool enabled) noexcept;
    [[nodiscard]] bool CheckForUpdates() const noexcept;
    void SetCheckForUpdates(bool enabled) noexcept;

private:
    bool Load();
    void SetDefaults();

    std::wstring m_path;
    std::vector<PinnedApp> m_pins;
    bool m_showDevBounds = false;
    bool m_followsTaskbarPins = true;
    float m_dockScale = 1.0F;
    std::wstring m_typeSafeApiKey;
    bool m_launchAtStartup = false;
    bool m_checkForUpdates = true;
};
