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
    // Remembers the last version whose installer/mover was actually launched,
    // with the wall-clock time of the attempt (unix seconds, 0 = none). Used
    // to break reinstall loops: if the install did not take effect (wrong
    // location, locked file, ...), the same version is not re-offered for a
    // cooldown period. Keyed by version so newer releases still surface.
    [[nodiscard]] std::wstring LastInstalledVersion() const;
    [[nodiscard]] long long LastInstalledTime() const noexcept;
    void SetLastInstalledVersion(const std::wstring& version, long long unixTime);
    // Bounded auto-retry budget for an install that did not take effect
    // (running version still older than LastInstalledVersion at startup).
    // Reset to 0 whenever a different version is recorded.
    [[nodiscard]] int LastInstalledAttempts() const noexcept;
    void SetLastInstalledAttempts(int attempts) noexcept;

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
    std::wstring m_lastInstalledVersion;
    long long m_lastInstalledTime = 0;
    int m_lastInstalledAttempts = 0;
};
