#pragma once

#include "DockConfig.h"
#include "Renderer.h"
#include "WindowCatalog.h"

#include <Windows.h>

#include <chrono>
#include <string>
#include <vector>

class DockApp {
public:
    explicit DockApp(HINSTANCE instance);
    ~DockApp();

    DockApp(const DockApp&) = delete;
    DockApp& operator=(const DockApp&) = delete;

    int Run();

private:
    enum class VisibilityState {
        Hidden,
        Showing,
        Visible,
        Hiding,
    };

    struct DisplayApp {
        PinnedApp app;
        HWND runningWindow = nullptr;
        int persistentPinIndex = -1;
    };

    static constexpr UINT kPointerMessage = WM_APP + 1;
    static constexpr UINT kRenderMessage = WM_APP + 2;
    static constexpr UINT kContextOpen = 1;
    static constexpr UINT kContextOpenLocation = 2;
    static constexpr UINT kContextClose = 3;
    static constexpr UINT kContextPin = 4;
    static constexpr UINT kContextUnpin = 5;
    static constexpr UINT kContextPinForeground = 6;
    static constexpr UINT kContextToggleBounds = 7;

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK InputWindowProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK MouseHook(int code, WPARAM wParam, LPARAM lParam);
    static BOOL CALLBACK FindTaskbarWindow(HWND window, LPARAM data);

    LRESULT HandleRendererMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleInputMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void CreateOverlayWindow();
    void RebuildLayout(bool reloadIcons);
    void UpdateInputRegion();
    void PositionOverlayWindows();
    void LoadIconTextures();
    void UpdatePrimaryMonitor();
    void BeginShow();
    void BeginHide();
    void AdvanceAnimation();
    void RenderFrame();
    void HandlePointer(POINT cursor);
    void HandleContextMenu(POINT screenPoint);
    void ActivatePressedApp();
    void CompleteDrag();
    void RefreshRunningWindows(bool force = false);
    bool RebuildDisplayApps();
    void HideTaskbar();
    void RestoreTaskbar();
    void LogInputMouse(UINT message, POINT screenPoint, int icon) const;
    void Log(const std::wstring& message) const;

    [[nodiscard]] bool IsCursorInBottomHotZone(POINT cursor) const noexcept;
    [[nodiscard]] int IconAtScreenPoint(POINT cursor) const noexcept;
    [[nodiscard]] int InsertionIndexFor(POINT cursor) const noexcept;
    [[nodiscard]] bool IsPersistentDisplayIcon(int icon) const noexcept;
    [[nodiscard]] LONG CurrentY() const noexcept;
    [[nodiscard]] bool IsAnimating() const noexcept;
    [[nodiscard]] double SecondsSinceAnimationStarted() const noexcept;
    [[nodiscard]] static double QpcSeconds();

    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    HWND m_inputWindow = nullptr;
    HHOOK m_mouseHook = nullptr;
    HANDLE m_singleInstanceMutex = nullptr;
    RECT m_primaryBounds{};
    POINT m_lastCursor{};
    std::vector<HWND> m_hiddenTaskbars;
    DockConfig m_config;
    WindowCatalog m_windows;
    Renderer m_renderer;
    std::vector<DisplayApp> m_displayApps;
    std::vector<DockIconRenderData> m_iconRenderData;
    VisibilityState m_visibility = VisibilityState::Hidden;
    double m_animationStartedAt = 0.0;
    LONG m_animationFromY = 0;
    LONG m_animationToY = 0;
    LONG m_windowX = 0;
    LONG m_visibleY = 0;
    LONG m_hiddenY = 0;
    LONG m_currentY = 0;
    UINT m_dockWidth = 1;
    UINT m_dockHeight = 1;
    int m_hoveredIcon = -1;
    int m_pressedIcon = -1;
    int m_draggedIcon = -1;
    int m_dragInsertion = -1;
    POINT m_pressedAt{};
    bool m_taskbarHidden = false;
    bool m_rendererInitialized = false;
    bool m_renderQueued = false;
    double m_lastWindowRefresh = 0.0;

    static DockApp* s_instance;
};
