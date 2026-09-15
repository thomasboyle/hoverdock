#include "DockApp.h"

#include <ShellScalingApi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

DockApp* DockApp::s_instance = nullptr;

namespace {

constexpr double kSlideDurationSeconds = 0.200;
constexpr int kBottomHotZonePixels = 2;
constexpr int kDragThresholdPixels = 4;

std::wstring ConfigDirectory(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? L"" : path.substr(0, separator);
}

bool IsInside(const RECT& bounds, LONG x, LONG y) {
    return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
}

}  // namespace

DockApp::DockApp(HINSTANCE instance)
    : m_instance(instance) {
}

DockApp::~DockApp() {
    if (m_mouseHook != nullptr) {
        UnhookWindowsHookEx(m_mouseHook);
    }
    RestoreTaskbar();
    if (m_singleInstanceMutex != nullptr) {
        CloseHandle(m_singleInstanceMutex);
    }
}

int DockApp::Run() {
    m_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\LiquidGlassDock.SingleInstance");
    if (m_singleInstanceMutex == nullptr) {
        MessageBoxW(nullptr, L"Liquid Glass Dock could not create its single-instance lock.",
            L"Liquid Glass Dock", MB_ICONERROR | MB_OK);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m_singleInstanceMutex);
        m_singleInstanceMutex = nullptr;
        return 0;
    }

    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE) {
        Log(L"Per-Monitor V2 DPI awareness was already set or unavailable.");
    }

    s_instance = this;
    m_config.LoadOrCreate();
    RestoreTaskbar();
    CreateWindow();
    UpdatePrimaryMonitor();
    m_windows.Refresh();
    RebuildLayout(false);

    try {
        m_renderer.Initialize(m_window, m_dockWidth, m_dockHeight);
        m_rendererInitialized = true;
        LoadIconTextures();
    } catch (...) {
        DestroyWindow(m_window);
        throw;
    }

    HideTaskbar();
    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &DockApp::MouseHook, m_instance, 0);
    if (m_mouseHook == nullptr) {
        Log(L"Low-level mouse hook unavailable; the dock can still be shown by moving over its window.");
    }

    GetCursorPos(&m_lastCursor);
    HandlePointer(m_lastCursor);
    Log(L"Dock initialized.");

    for (;;) {
        HANDLE frameWaitable = IsAnimating() ? m_renderer.FrameLatencyWaitableObject() : nullptr;
        const DWORD count = frameWaitable == nullptr ? 0 : 1;
        const DWORD timeout = IsAnimating() ? 50 : INFINITE;
        const DWORD wait = MsgWaitForMultipleObjectsEx(count, &frameWaitable, timeout, QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);

        if (IsAnimating() && (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT)) {
            AdvanceAnimation();
        }

        if (wait == WAIT_OBJECT_0 + count || wait == WAIT_FAILED) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                if (message.message == WM_QUIT) {
                    return static_cast<int>(message.wParam);
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
}

LRESULT CALLBACK DockApp::WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    DockApp* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<DockApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->m_window = window;
    } else {
        app = reinterpret_cast<DockApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return app == nullptr ? DefWindowProcW(window, message, wParam, lParam)
                          : app->HandleMessage(message, wParam, lParam);
}

LRESULT CALLBACK DockApp::MouseHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_MOUSEMOVE && s_instance != nullptr &&
        s_instance->m_window != nullptr) {
        const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        PostMessageW(s_instance->m_window, kPointerMessage,
            static_cast<WPARAM>(static_cast<INT_PTR>(mouse->pt.x)),
            static_cast<LPARAM>(mouse->pt.y));
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL CALLBACK DockApp::FindTaskbarWindow(HWND window, LPARAM data) {
    wchar_t className[64]{};
    if (GetClassNameW(window, className, static_cast<int>(std::size(className))) == 0) {
        return TRUE;
    }

    const std::wstring classValue(className);
    if (classValue == L"Shell_TrayWnd" || classValue == L"Shell_SecondaryTrayWnd") {
        auto* taskbars = reinterpret_cast<std::vector<HWND>*>(data);
        taskbars->push_back(window);
    }
    return TRUE;
}

LRESULT DockApp::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return m_visibility == VisibilityState::Hidden ? HTTRANSPARENT : HTCLIENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETCURSOR:
        if (m_visibility != VisibilityState::Hidden) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lParam) + m_windowX, GET_Y_LPARAM(lParam) + m_currentY};
        m_pressedIcon = IconAtScreenPoint(point);
        if (m_pressedIcon >= 0) {
            m_pressedAt = point;
            SetCapture(m_window);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT point{};
        GetCursorPos(&point);
        HandlePointer(point);
        if (m_pressedIcon >= 0 &&
            (std::abs(point.x - m_pressedAt.x) >= kDragThresholdPixels ||
                std::abs(point.y - m_pressedAt.y) >= kDragThresholdPixels)) {
            m_draggedIcon = m_pressedIcon;
            m_dragInsertion = InsertionIndexFor(point);
            if (m_rendererInitialized) {
                RenderFrame();
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (GetCapture() == m_window) {
            ReleaseCapture();
        }
        if (m_draggedIcon >= 0) {
            CompleteDrag();
        } else {
            ActivatePressedApp();
        }
        m_pressedIcon = -1;
        m_draggedIcon = -1;
        m_dragInsertion = -1;
        return 0;

    case WM_CAPTURECHANGED:
        m_pressedIcon = -1;
        m_draggedIcon = -1;
        m_dragInsertion = -1;
        return 0;

    case WM_RBUTTONUP: {
        POINT point{};
        GetCursorPos(&point);
        HandleContextMenu(point);
        return 0;
    }

    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        UpdatePrimaryMonitor();
        RebuildLayout(false);
        return 0;

    case WM_ENDSESSION:
        if (wParam != FALSE) {
            RestoreTaskbar();
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            BeginHide();
            return 0;
        }
        if (wParam == VK_F12) {
            m_config.SetShowDevBounds(!m_config.ShowDevBounds());
            m_config.Save();
            RenderFrame();
            return 0;
        }
        break;

    case kPointerMessage: {
        const POINT point{static_cast<LONG>(static_cast<INT_PTR>(wParam)),
            static_cast<LONG>(lParam)};
        HandlePointer(point);
        return 0;
    }

    case kRenderMessage:
        m_renderQueued = false;
        if (m_visibility != VisibilityState::Hidden && !IsAnimating()) {
            RenderFrame();
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(m_window);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(m_window, message, wParam, lParam);
}

void DockApp::CreateWindow() {
    const wchar_t className[] = L"LiquidGlassDockWindow";
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = &DockApp::WindowProcedure;
    windowClass.hInstance = m_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = className;
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&windowClass);

    constexpr DWORD style = WS_POPUP;
    constexpr DWORD extendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
        WS_EX_NOREDIRECTIONBITMAP;
    m_window = CreateWindowExW(extendedStyle, className, L"Liquid Glass Dock", style, 0, 0, 1, 1,
        nullptr, nullptr, m_instance, this);
    if (m_window == nullptr) {
        throw std::runtime_error("CreateWindowExW failed.");
    }
}

void DockApp::RebuildLayout(bool reloadIcons) {
    const UINT dpi = GetDpiForWindow(m_window);
    const float scale = static_cast<float>(dpi) / 96.0F;
    const LONG iconSize = std::lround(56.0F * scale);
    const LONG padding = std::lround(20.0F * scale);
    const LONG gap = std::lround(10.0F * scale);
    const LONG margin = std::lround(10.0F * scale);
    const size_t pinCount = m_config.Pins().size();

    m_dockWidth = static_cast<UINT>(padding * 2 +
        static_cast<LONG>(pinCount) * iconSize +
        static_cast<LONG>(pinCount > 0 ? pinCount - 1 : 0) * gap);
    m_dockHeight = static_cast<UINT>(std::lround(88.0F * scale));
    m_visibleY = m_primaryBounds.bottom - static_cast<LONG>(m_dockHeight);
    m_hiddenY = m_visibleY + static_cast<LONG>(m_dockHeight) + margin;
    if (m_visibility == VisibilityState::Hidden) {
        m_currentY = m_hiddenY;
    }
    m_windowX = m_primaryBounds.left +
        ((m_primaryBounds.right - m_primaryBounds.left) - static_cast<LONG>(m_dockWidth)) / 2;

    m_iconRenderData.clear();
    m_iconRenderData.reserve(pinCount);
    const LONG top = (static_cast<LONG>(m_dockHeight) - iconSize) / 2;
    for (size_t index = 0; index < pinCount; ++index) {
        const LONG left = padding + static_cast<LONG>(index) * (iconSize + gap);
        DockIconRenderData data;
        data.bounds = {left, top, left + iconSize, top + iconSize};
        data.running = m_windows.IsRunning(m_config.Pins()[index]);
        data.textureIndex = static_cast<UINT>(index);
        m_iconRenderData.push_back(data);
    }

    SetWindowPos(m_window, HWND_TOPMOST, m_windowX, m_currentY, static_cast<int>(m_dockWidth),
        static_cast<int>(m_dockHeight), SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (m_rendererInitialized) {
        m_renderer.Resize(m_dockWidth, m_dockHeight);
    }
    if (reloadIcons && m_rendererInitialized) {
        LoadIconTextures();
    }
}

void DockApp::LoadIconTextures() {
    std::vector<std::wstring> targets;
    targets.reserve(m_config.Pins().size());
    for (const PinnedApp& app : m_config.Pins()) {
        targets.push_back(app.target);
    }
    m_renderer.LoadIcons(targets);
}

void DockApp::UpdatePrimaryMonitor() {
    const HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO information{sizeof(information)};
    if (GetMonitorInfoW(primary, &information) == FALSE) {
        throw std::runtime_error("GetMonitorInfoW for primary monitor failed.");
    }
    m_primaryBounds = information.rcMonitor;
}

void DockApp::BeginShow() {
    if (m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Visible) {
        return;
    }

    RefreshRunningWindows();
    RebuildLayout(false);
    ShowWindow(m_window, SW_SHOWNOACTIVATE);
    m_visibility = VisibilityState::Showing;
    m_animationFromY = m_currentY;
    m_animationToY = m_visibleY;
    m_animationStartedAt = QpcSeconds();
}

void DockApp::BeginHide() {
    if (m_visibility == VisibilityState::Hidden || m_visibility == VisibilityState::Hiding) {
        return;
    }

    if (GetCapture() == m_window) {
        ReleaseCapture();
    }
    m_pressedIcon = -1;
    m_draggedIcon = -1;
    m_visibility = VisibilityState::Hiding;
    m_animationFromY = m_currentY;
    m_animationToY = m_hiddenY;
    m_animationStartedAt = QpcSeconds();
}

void DockApp::AdvanceAnimation() {
    const double elapsed = SecondsSinceAnimationStarted();
    const double linear = std::clamp(elapsed / kSlideDurationSeconds, 0.0, 1.0);
    const double eased = linear * linear * (3.0 - 2.0 * linear);
    m_currentY = std::lround(static_cast<double>(m_animationFromY) +
        static_cast<double>(m_animationToY - m_animationFromY) * eased);
    SetWindowPos(m_window, HWND_TOPMOST, m_windowX, m_currentY, static_cast<int>(m_dockWidth),
        static_cast<int>(m_dockHeight), SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSIZE);
    RenderFrame();

    if (linear < 1.0) {
        return;
    }

    if (m_visibility == VisibilityState::Showing) {
        m_visibility = VisibilityState::Visible;
        m_currentY = m_visibleY;
        return;
    }

    m_visibility = VisibilityState::Hidden;
    m_currentY = m_hiddenY;
    m_pointerInsideDock = false;
    ShowWindow(m_window, SW_HIDE);
}

void DockApp::RenderFrame() {
    if (!m_rendererInitialized || m_visibility == VisibilityState::Hidden) {
        return;
    }

    for (size_t index = 0; index < m_iconRenderData.size(); ++index) {
        DockIconRenderData& icon = m_iconRenderData[index];
        icon.hovered = static_cast<int>(index) == m_hoveredIcon;
        icon.dragged = static_cast<int>(index) == m_draggedIcon;
    }

    DockRenderState state;
    state.width = m_dockWidth;
    state.height = m_dockHeight;
    state.glassAlpha = 0.90F;
    state.slideProgress = m_dockHeight == 0 ? 0.0F :
        static_cast<float>(m_visibleY - m_currentY) / static_cast<float>(m_dockHeight);
    state.timeSeconds = static_cast<float>(QpcSeconds());
    state.showDevBounds = m_config.ShowDevBounds();
    state.icons = m_iconRenderData;
    m_renderer.Render(state);
}

void DockApp::HandlePointer(POINT cursor) {
    const bool wasInsideDock = m_pointerInsideDock;
    m_lastCursor = cursor;
    if (m_visibility == VisibilityState::Hidden) {
        if (IsCursorInBottomHotZone(cursor)) {
            BeginShow();
        }
        return;
    }

    if (m_visibility == VisibilityState::Hiding && IsCursorInBottomHotZone(cursor)) {
        BeginShow();
        return;
    }

    const bool isInsideDock = cursor.x >= m_windowX &&
        cursor.x < m_windowX + static_cast<LONG>(m_dockWidth) &&
        cursor.y >= m_currentY &&
        cursor.y < m_currentY + static_cast<LONG>(m_dockHeight);
    if (wasInsideDock && cursor.y < m_currentY) {
        BeginHide();
        m_pointerInsideDock = false;
        return;
    }
    m_pointerInsideDock = isInsideDock;

    const int hovered = IconAtScreenPoint(cursor);
    if (hovered != m_hoveredIcon) {
        m_hoveredIcon = hovered;
        if (!IsAnimating()) {
            RenderFrame();
        }
    }
    RefreshRunningWindows();
}

void DockApp::HandleContextMenu(POINT screenPoint) {
    const int icon = IconAtScreenPoint(screenPoint);
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    m_windows.Refresh();
    if (icon >= 0) {
        const PinnedApp& app = m_config.Pins()[static_cast<size_t>(icon)];
        const bool running = m_windows.IsRunning(app);
        AppendMenuW(menu, MF_STRING, kContextOpen, L"Open");
        if (app.target.rfind(L"shell:", 0) != 0) {
            AppendMenuW(menu, MF_STRING, kContextOpenLocation, L"Open location");
        }
        AppendMenuW(menu, MF_STRING | (running ? MF_ENABLED : MF_GRAYED), kContextClose, L"Close");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextUnpin, L"Unpin");
    } else {
        AppendMenuW(menu, MF_STRING, kContextPinForeground, L"Pin foreground application");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextToggleBounds,
        m_config.ShowDevBounds() ? L"Hide developer bounds" : L"Show developer bounds");

    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screenPoint.x, screenPoint.y, m_window, nullptr);
    DestroyMenu(menu);

    if (command == 0) {
        return;
    }

    bool configChanged = false;
    if (command == kContextOpen && icon >= 0) {
        m_windows.ActivateOrLaunch(m_config.Pins()[static_cast<size_t>(icon)]);
    } else if (command == kContextOpenLocation && icon >= 0) {
        m_windows.OpenLocation(m_config.Pins()[static_cast<size_t>(icon)]);
    } else if (command == kContextClose && icon >= 0) {
        m_windows.Close(m_config.Pins()[static_cast<size_t>(icon)]);
    } else if (command == kContextUnpin && icon >= 0) {
        m_config.Pins().erase(m_config.Pins().begin() + icon);
        configChanged = true;
    } else if (command == kContextPinForeground) {
        configChanged = m_windows.AddForegroundApplication(m_config.Pins());
    } else if (command == kContextToggleBounds) {
        m_config.SetShowDevBounds(!m_config.ShowDevBounds());
        configChanged = true;
    }

    if (configChanged) {
        m_config.Save();
        m_windows.Refresh();
        RebuildLayout(command == kContextUnpin || command == kContextPinForeground);
    }
    RefreshRunningWindows();
    RenderFrame();
}

void DockApp::ActivatePressedApp() {
    if (m_pressedIcon < 0 || static_cast<size_t>(m_pressedIcon) >= m_config.Pins().size()) {
        return;
    }
    m_windows.Refresh();
    m_windows.ActivateOrLaunch(m_config.Pins()[static_cast<size_t>(m_pressedIcon)]);
    m_lastWindowRefresh = 0.0;
    RefreshRunningWindows();
}

void DockApp::CompleteDrag() {
    if (m_draggedIcon < 0 || m_dragInsertion < 0 || m_draggedIcon == m_dragInsertion ||
        static_cast<size_t>(m_draggedIcon) >= m_config.Pins().size()) {
        return;
    }

    std::vector<PinnedApp>& pins = m_config.Pins();
    PinnedApp app = std::move(pins[static_cast<size_t>(m_draggedIcon)]);
    pins.erase(pins.begin() + m_draggedIcon);
    int target = m_dragInsertion;
    if (target > m_draggedIcon) {
        --target;
    }
    target = std::clamp(target, 0, static_cast<int>(pins.size()));
    pins.insert(pins.begin() + target, std::move(app));
    m_config.Save();
    RebuildLayout(true);
    RenderFrame();
}

void DockApp::RefreshRunningWindows() {
    const double now = QpcSeconds();
    if (now - m_lastWindowRefresh < 0.5) {
        return;
    }
    m_lastWindowRefresh = now;
    m_windows.Refresh();
    for (size_t index = 0; index < m_iconRenderData.size(); ++index) {
        m_iconRenderData[index].running = m_windows.IsRunning(m_config.Pins()[index]);
    }
}

void DockApp::HideTaskbar() {
    m_hiddenTaskbars.clear();
    EnumWindows(&DockApp::FindTaskbarWindow, reinterpret_cast<LPARAM>(&m_hiddenTaskbars));
    for (HWND taskbar : m_hiddenTaskbars) {
        ShowWindow(taskbar, SW_HIDE);
    }
    m_taskbarHidden = !m_hiddenTaskbars.empty();
    Log(m_taskbarHidden ? L"Native taskbar hidden." : L"Native taskbar was not found.");
}

void DockApp::RestoreTaskbar() {
    std::vector<HWND> taskbars;
    EnumWindows(&DockApp::FindTaskbarWindow, reinterpret_cast<LPARAM>(&taskbars));
    for (HWND taskbar : taskbars) {
        ShowWindow(taskbar, SW_SHOWNA);
    }
    if (m_taskbarHidden) {
        Log(L"Native taskbar restored.");
    }
    m_taskbarHidden = false;
}

void DockApp::Log(const std::wstring& message) const {
    const std::wstring directory = ConfigDirectory(m_config.Path());
    if (directory.empty()) {
        OutputDebugStringW((message + L"\n").c_str());
        return;
    }

    std::wofstream log(directory + L"\\dock.log", std::ios::app);
    if (log) {
        log << message << L'\n';
    }
    OutputDebugStringW((message + L"\n").c_str());
}

bool DockApp::IsCursorInBottomHotZone(POINT cursor) const noexcept {
    return cursor.x >= m_primaryBounds.left && cursor.x < m_primaryBounds.right &&
        cursor.y >= m_primaryBounds.bottom - kBottomHotZonePixels &&
        cursor.y < m_primaryBounds.bottom;
}

int DockApp::IconAtScreenPoint(POINT cursor) const noexcept {
    const LONG localX = cursor.x - m_windowX;
    const LONG localY = cursor.y - m_currentY;
    for (size_t index = 0; index < m_iconRenderData.size(); ++index) {
        if (IsInside(m_iconRenderData[index].bounds, localX, localY)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int DockApp::InsertionIndexFor(POINT cursor) const noexcept {
    const LONG localX = cursor.x - m_windowX;
    for (size_t index = 0; index < m_iconRenderData.size(); ++index) {
        const RECT& bounds = m_iconRenderData[index].bounds;
        const LONG center = bounds.left + (bounds.right - bounds.left) / 2;
        if (localX < center) {
            return static_cast<int>(index);
        }
    }
    return static_cast<int>(m_iconRenderData.size());
}

LONG DockApp::CurrentY() const noexcept {
    return m_currentY;
}

bool DockApp::IsAnimating() const noexcept {
    return m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Hiding;
}

double DockApp::SecondsSinceAnimationStarted() const noexcept {
    return QpcSeconds() - m_animationStartedAt;
}

double DockApp::QpcSeconds() {
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER result{};
        QueryPerformanceFrequency(&result);
        return result;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}
