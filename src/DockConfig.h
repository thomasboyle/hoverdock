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
    [[nodiscard]] const std::wstring& Path() const noexcept;

private:
    bool Load();
    void SetDefaults();

    std::wstring m_path;
    std::vector<PinnedApp> m_pins;
    bool m_showDevBounds = false;
};
