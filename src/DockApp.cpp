#include "DockApp.h"

#include <ShellScalingApi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
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

constexpr double kShowDurationSeconds = 0.050;
constexpr double kHideDurationSeconds = 0.200;
constexpr int kBottomHotZonePixels = 2;
constexpr int kDragThresholdPixels = 4;
constexpr BYTE kInputWindowAlpha = 1;
constexpr wchar_t kStartTarget[] = L"dock:start";
constexpr wchar_t kSearchTarget[] = L"dock:search";

std::wstring ConfigDirectory(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? L"" : path.substr(0, separator);
}

bool IsInside(const RECT& bounds, LONG x, LONG y) {
    return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
}

POINT ScreenPointFromClient(HWND window, LPARAM lParam) {
    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (ClientToScreen(window, &point) == FALSE) {
        GetCursorPos(&point);
    }
    return point;
}

std::wstring DisplayNameFromExecutable(const std::wstring& path) {
    const size_t fileStart = path.find_last_of(L"\\/") + 1;
    const size_t extension = path.find_last_of(L'.');
    const size_t fileEnd = extension == std::wstring::npos || extension < fileStart
        ? path.size()
        : extension;
    return path.substr(fileStart, fileEnd - fileStart);
}

bool IsSpecialDockTarget(const std::wstring& target) {
    return target == kStartTarget || target == kSearchTarget;
}

INPUT KeyboardInput(WORD key, DWORD flags) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = flags;
    return input;
}

bool OpenStartMenu() {
    const std::array inputs = {
        KeyboardInput(VK_LWIN, 0),
        KeyboardInput(VK_LWIN, KEYEVENTF_KEYUP),
    };
    return SendInput(static_cast<UINT>(inputs.size()), const_cast<INPUT*>(inputs.data()),
        sizeof(INPUT)) == static_cast<UINT>(inputs.size());
}

bool OpenSearch() {
    const std::array inputs = {
        KeyboardInput(VK_LWIN, 0),
        KeyboardInput(L'S', 0),
        KeyboardInput(L'S', KEYEVENTF_KEYUP),
        KeyboardInput(VK_LWIN, KEYEVENTF_KEYUP),
    };
    return SendInput(static_cast<UINT>(inputs.size()), const_cast<INPUT*>(inputs.data()),
        sizeof(INPUT)) == static_cast<UINT>(inputs.size());
}

bool OpenSpecialDockTarget(const std::wstring& target) {
    if (target == kStartTarget) {
        return OpenStartMenu();
    }
    if (target == kSearchTarget) {
        return OpenSearch();
    }
    return false;
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
    CreateOverlayWindow();
    UpdatePrimaryMonitor();
    m_windows.Refresh();
    RebuildDisplayApps();
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
                          : app->HandleRendererMessage(window, message, wParam, lParam);
}

LRESULT CALLBACK DockApp::InputWindowProcedure(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam) {
    DockApp* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<DockApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->m_inputWindow = window;
    } else {
        app = reinterpret_cast<DockApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return app == nullptr ? DefWindowProcW(window, message, wParam, lParam)
                          : app->HandleInputMessage(window, message, wParam, lParam);
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

LRESULT DockApp::HandleRendererMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        UpdatePrimaryMonitor();
        RebuildLayout(true);
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
        if (DestroyWindow(window) == FALSE) {
            Log(L"Could not destroy the renderer window.");
        }
        return 0;

    case WM_DESTROY:
        if (m_inputWindow != nullptr && DestroyWindow(m_inputWindow) == FALSE) {
            Log(L"Could not destroy the dock input window.");
        }
        m_inputWindow = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT DockApp::HandleInputMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Visible
            ? HTCLIENT
            : HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETCURSOR:
        if (m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Visible) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC paintDc = BeginPaint(window, &paint);
        if (paintDc != nullptr) {
            EndPaint(window, &paint);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const POINT point = ScreenPointFromClient(window, lParam);
        m_pressedIcon = IconAtScreenPoint(point);
        LogInputMouse(message, point, m_pressedIcon);
        if (m_pressedIcon >= 0) {
            m_pressedAt = point;
            SetCapture(window);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        const POINT point = ScreenPointFromClient(window, lParam);
        HandlePointer(point);
        if (m_pressedIcon >= 0 && IsPersistentDisplayIcon(m_pressedIcon) &&
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

    case WM_LBUTTONUP: {
        const POINT point = ScreenPointFromClient(window, lParam);
        LogInputMouse(message, point, IconAtScreenPoint(point));
        if (m_draggedIcon >= 0) {
            CompleteDrag();
        } else {
            ActivatePressedApp();
        }
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        m_pressedIcon = -1;
        m_draggedIcon = -1;
        m_dragInsertion = -1;
        return 0;
    }

    case WM_CAPTURECHANGED:
        m_pressedIcon = -1;
        m_draggedIcon = -1;
        m_dragInsertion = -1;
        return 0;

    case WM_RBUTTONUP: {
        const POINT point = ScreenPointFromClient(window, lParam);
        LogInputMouse(message, point, IconAtScreenPoint(point));
        HandleContextMenu(point);
        return 0;
    }

    case WM_DPICHANGED:
        RebuildLayout(true);
        return 0;

    case WM_CLOSE:
        if (DestroyWindow(m_window) == FALSE) {
            Log(L"Could not destroy the renderer window from the input window.");
        }
        return 0;

    case WM_DESTROY:
        if (m_inputWindow == window) {
            m_inputWindow = nullptr;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void DockApp::CreateOverlayWindow() {
    const wchar_t rendererClassName[] = L"LiquidGlassDockWindow";
    WNDCLASSEXW rendererClass{sizeof(rendererClass)};
    rendererClass.lpfnWndProc = &DockApp::WindowProcedure;
    rendererClass.hInstance = m_instance;
    rendererClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    rendererClass.lpszClassName = rendererClassName;
    rendererClass.style = CS_HREDRAW | CS_VREDRAW;
    if (RegisterClassExW(&rendererClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("Register renderer window class failed.");
    }

    const wchar_t inputClassName[] = L"LiquidGlassDockInputWindow";
    WNDCLASSEXW inputClass{sizeof(inputClass)};
    inputClass.lpfnWndProc = &DockApp::InputWindowProcedure;
    inputClass.hInstance = m_instance;
    inputClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    inputClass.lpszClassName = inputClassName;
    inputClass.style = CS_HREDRAW | CS_VREDRAW;
    if (RegisterClassExW(&inputClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("Register dock input window class failed.");
    }

    constexpr DWORD style = WS_POPUP;
    constexpr DWORD rendererExtendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    m_window = CreateWindowExW(rendererExtendedStyle, rendererClassName, L"Liquid Glass Dock", style,
        0, 0, 1, 1, nullptr, nullptr, m_instance, this);
    if (m_window == nullptr) {
        throw std::runtime_error("Create renderer window failed.");
    }

    constexpr DWORD inputExtendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    m_inputWindow = CreateWindowExW(inputExtendedStyle, inputClassName, L"", style, 0, 0, 1, 1,
        nullptr, nullptr, m_instance, this);
    if (m_inputWindow == nullptr) {
        DestroyWindow(m_window);
        throw std::runtime_error("Create dock input window failed.");
    }
    if (SetLayeredWindowAttributes(m_inputWindow, 0, kInputWindowAlpha, LWA_ALPHA) == FALSE) {
        DestroyWindow(m_inputWindow);
        m_inputWindow = nullptr;
        DestroyWindow(m_window);
        throw std::runtime_error("Set dock input window alpha failed.");
    }
}

void DockApp::RebuildLayout(bool reloadIcons) {
    const UINT dpi = GetDpiForWindow(m_window);
    const float scale = static_cast<float>(dpi) / 96.0F;
    const LONG iconSize = std::lround(56.0F * scale);
    const LONG padding = std::lround(20.0F * scale);
    const LONG gap = std::lround(10.0F * scale);
    const LONG margin = std::lround(10.0F * scale);
    const size_t displayCount = m_displayApps.size();

    m_dockWidth = static_cast<UINT>(padding * 2 +
        static_cast<LONG>(displayCount) * iconSize +
        static_cast<LONG>(displayCount > 0 ? displayCount - 1 : 0) * gap);
    m_dockHeight = static_cast<UINT>(std::lround(88.0F * scale));
    m_visibleY = m_primaryBounds.bottom - static_cast<LONG>(m_dockHeight);
    m_hiddenY = m_visibleY + static_cast<LONG>(m_dockHeight) + margin;
    if (m_visibility == VisibilityState::Hidden) {
        m_currentY = m_hiddenY;
    }
    m_windowX = m_primaryBounds.left +
        ((m_primaryBounds.right - m_primaryBounds.left) - static_cast<LONG>(m_dockWidth)) / 2;

    m_iconRenderData.clear();
    m_iconRenderData.reserve(displayCount);
    const LONG top = (static_cast<LONG>(m_dockHeight) - iconSize) / 2;
    for (size_t index = 0; index < displayCount; ++index) {
        const LONG left = padding + static_cast<LONG>(index) * (iconSize + gap);
        DockIconRenderData data;
        data.bounds = {left, top, left + iconSize, top + iconSize};
        data.running = m_displayApps[index].runningWindow != nullptr;
        data.textureIndex = static_cast<UINT>(index);
        m_iconRenderData.push_back(data);
    }

    UpdateInputRegion();
    PositionOverlayWindows();
    if (m_rendererInitialized) {
        m_renderer.Resize(m_dockWidth, m_dockHeight);
    }
    if (reloadIcons && m_rendererInitialized) {
        LoadIconTextures();
    }
}

void DockApp::UpdateInputRegion() {
    if (m_inputWindow == nullptr) {
        return;
    }

    const LONG radius = std::max(2L, std::lround(static_cast<float>(m_dockHeight) * 0.47F));
    HRGN region = CreateRoundRectRgn(0, 0, static_cast<int>(m_dockWidth) + 1,
        static_cast<int>(m_dockHeight) + 1, static_cast<int>(radius * 2),
        static_cast<int>(radius * 2));
    if (region == nullptr) {
        Log(L"Could not create the dock input region.");
        return;
    }
    if (SetWindowRgn(m_inputWindow, region, FALSE) == 0) {
        DeleteObject(region);
        Log(L"Could not apply the dock input region.");
    }
}

void DockApp::PositionOverlayWindows() {
    const UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (SetWindowPos(m_window, HWND_TOPMOST, m_windowX, m_currentY,
            static_cast<int>(m_dockWidth), static_cast<int>(m_dockHeight), flags) == FALSE) {
        Log(L"Could not position the renderer window.");
    }
    if (m_inputWindow != nullptr && SetWindowPos(m_inputWindow, m_window, m_windowX, m_currentY,
            static_cast<int>(m_dockWidth), static_cast<int>(m_dockHeight), flags) == FALSE) {
        Log(L"Could not position the dock input window.");
    }
}

void DockApp::LoadIconTextures() {
    std::vector<std::wstring> targets;
    targets.reserve(m_displayApps.size());
    for (const DisplayApp& app : m_displayApps) {
        targets.push_back(app.app.target);
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

    const bool wasHidden = m_visibility == VisibilityState::Hidden;
    RefreshRunningWindows(true);
    RebuildLayout(false);
    if (wasHidden) {
        const RECT captureBounds{m_windowX, m_visibleY,
            m_windowX + static_cast<LONG>(m_dockWidth),
            m_visibleY + static_cast<LONG>(m_dockHeight)};
        if (!m_renderer.CaptureBackdrop(captureBounds)) {
            Log(L"Desktop backdrop capture failed; retaining the prior safe backdrop.");
        }
    }

    m_visibility = VisibilityState::Showing;
    ShowWindow(m_window, SW_SHOWNOACTIVATE);
    ShowWindow(m_inputWindow, SW_SHOWNOACTIVATE);
    PositionOverlayWindows();
    m_animationFromY = m_currentY;
    m_animationToY = m_visibleY;
    m_animationStartedAt = QpcSeconds();
}

void DockApp::BeginHide() {
    if (m_visibility == VisibilityState::Hidden || m_visibility == VisibilityState::Hiding) {
        return;
    }

    if (GetCapture() == m_inputWindow) {
        ReleaseCapture();
    }
    m_pressedIcon = -1;
    m_draggedIcon = -1;
    m_visibility = VisibilityState::Hiding;
    ShowWindow(m_inputWindow, SW_HIDE);
    m_animationFromY = m_currentY;
    m_animationToY = m_hiddenY;
    m_animationStartedAt = QpcSeconds();
}

void DockApp::AdvanceAnimation() {
    const double duration = m_visibility == VisibilityState::Hiding
        ? kHideDurationSeconds
        : kShowDurationSeconds;
    const double elapsed = SecondsSinceAnimationStarted();
    const double linear = std::clamp(elapsed / duration, 0.0, 1.0);
    const double eased = linear * linear * (3.0 - 2.0 * linear);
    m_currentY = std::lround(static_cast<double>(m_animationFromY) +
        static_cast<double>(m_animationToY - m_animationFromY) * eased);
    PositionOverlayWindows();
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
    ShowWindow(m_inputWindow, SW_HIDE);
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
    state.glassAlpha = 0.60F;
    state.slideProgress = m_dockHeight == 0 ? 0.0F :
        static_cast<float>(m_visibleY - m_currentY) / static_cast<float>(m_dockHeight);
    state.timeSeconds = static_cast<float>(QpcSeconds());
    state.showDevBounds = m_config.ShowDevBounds();
    state.icons = m_iconRenderData;
    m_renderer.Render(state);
}

void DockApp::HandlePointer(POINT cursor) {
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

    if ((m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Visible) &&
        cursor.y < m_visibleY) {
        BeginHide();
        return;
    }

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
    RefreshRunningWindows(true);
    const int icon = IconAtScreenPoint(screenPoint);
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    DisplayApp app;
    const bool hasApp = icon >= 0 && static_cast<size_t>(icon) < m_displayApps.size();
    const bool isSpecial = hasApp && IsSpecialDockTarget(m_displayApps[static_cast<size_t>(icon)].app.target);
    if (hasApp) {
        app = m_displayApps[static_cast<size_t>(icon)];
        if (isSpecial) {
            AppendMenuW(menu, MF_STRING, kContextOpen, L"Open");
        } else {
            const bool running = app.runningWindow != nullptr;
            AppendMenuW(menu, MF_STRING, kContextOpen, L"Open");
            if (app.app.target.rfind(L"shell:", 0) != 0) {
                AppendMenuW(menu, MF_STRING, kContextOpenLocation, L"Open location");
            }
            AppendMenuW(menu, MF_STRING | (running ? MF_ENABLED : MF_GRAYED), kContextClose, L"Close");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING,
                app.persistentPinIndex >= 0 ? kContextUnpin : kContextPin,
                app.persistentPinIndex >= 0 ? L"Unpin" : L"Pin");
        }
    } else {
        AppendMenuW(menu, MF_STRING, kContextPinForeground, L"Pin foreground application");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextToggleBounds,
        m_config.ShowDevBounds() ? L"Hide developer bounds" : L"Show developer bounds");

    const HWND menuOwner = m_inputWindow == nullptr ? m_window : m_inputWindow;
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screenPoint.x, screenPoint.y, menuOwner, nullptr);
    DestroyMenu(menu);

    if (command == 0) {
        return;
    }

    bool configChanged = false;
    bool actionSucceeded = true;
    if (command == kContextOpen && hasApp) {
        actionSucceeded = isSpecial ? OpenSpecialDockTarget(app.app.target)
                                    : m_windows.ActivateOrLaunch(app.app, app.runningWindow);
    } else if (command == kContextOpenLocation && hasApp && !isSpecial) {
        actionSucceeded = m_windows.OpenLocation(app.app);
    } else if (command == kContextClose && hasApp && !isSpecial) {
        actionSucceeded = m_windows.Close(app.app, app.runningWindow);
    } else if (command == kContextUnpin && !isSpecial && app.persistentPinIndex >= 0 &&
        static_cast<size_t>(app.persistentPinIndex) < m_config.Pins().size()) {
        m_config.Pins().erase(m_config.Pins().begin() + app.persistentPinIndex);
        configChanged = true;
    } else if (command == kContextPin && !isSpecial && app.persistentPinIndex < 0) {
        const bool alreadyPinned = std::ranges::any_of(m_config.Pins(), [&app](const PinnedApp& pin) {
            return WindowCatalog::TargetsMatch(pin.target, app.app.target);
        });
        if (!alreadyPinned) {
            m_config.Pins().push_back(app.app);
            configChanged = true;
        }
    } else if (command == kContextPinForeground) {
        configChanged = m_windows.AddForegroundApplication(m_config.Pins());
    } else if (command == kContextToggleBounds) {
        m_config.SetShowDevBounds(!m_config.ShowDevBounds());
        configChanged = true;
    }

    if (!actionSucceeded) {
        Log(L"Dock context action did not complete.");
    }
    if (configChanged) {
        m_config.Save();
        m_lastWindowRefresh = 0.0;
        RefreshRunningWindows(true);
    }
    RefreshRunningWindows();
    RenderFrame();
}

void DockApp::ActivatePressedApp() {
    if (m_pressedIcon < 0 || static_cast<size_t>(m_pressedIcon) >= m_displayApps.size()) {
        return;
    }

    const DisplayApp app = m_displayApps[static_cast<size_t>(m_pressedIcon)];
    if (IsSpecialDockTarget(app.app.target)) {
        if (!OpenSpecialDockTarget(app.app.target)) {
            Log(L"Special dock target did not accept input.");
        }
        return;
    }

    m_windows.Refresh();
    if (!m_windows.ActivateOrLaunch(app.app, app.runningWindow)) {
        Log(L"Application did not launch or accept focus.");
    }
    m_lastWindowRefresh = 0.0;
    RefreshRunningWindows();
}

void DockApp::CompleteDrag() {
    if (m_draggedIcon < 0 || m_dragInsertion < 0 || !IsPersistentDisplayIcon(m_draggedIcon) ||
        static_cast<size_t>(m_draggedIcon) >= m_displayApps.size()) {
        return;
    }

    const int draggedPin = m_displayApps[static_cast<size_t>(m_draggedIcon)].persistentPinIndex;
    if (draggedPin < 0 || static_cast<size_t>(draggedPin) >= m_config.Pins().size()) {
        return;
    }

    const int insertion = std::clamp(m_dragInsertion, 0, static_cast<int>(m_displayApps.size()));
    int target = 0;
    for (int index = 0; index < insertion; ++index) {
        if (m_displayApps[static_cast<size_t>(index)].persistentPinIndex >= 0) {
            ++target;
        }
    }
    if (target == draggedPin || target == draggedPin + 1) {
        return;
    }

    std::vector<PinnedApp>& pins = m_config.Pins();
    PinnedApp app = std::move(pins[static_cast<size_t>(draggedPin)]);
    pins.erase(pins.begin() + draggedPin);
    if (target > draggedPin) {
        --target;
    }
    target = std::clamp(target, 0, static_cast<int>(pins.size()));
    pins.insert(pins.begin() + target, std::move(app));
    m_config.Save();
    m_windows.Refresh();
    RebuildDisplayApps();
    RebuildLayout(true);
    m_lastWindowRefresh = QpcSeconds();
    RenderFrame();
}

void DockApp::RefreshRunningWindows(bool force) {
    const double now = QpcSeconds();
    if (!force && now - m_lastWindowRefresh < 0.5) {
        return;
    }

    m_lastWindowRefresh = now;
    m_windows.Refresh();
    if (RebuildDisplayApps()) {
        RebuildLayout(true);
        return;
    }

    const size_t iconCount = std::min(m_iconRenderData.size(), m_displayApps.size());
    for (size_t index = 0; index < iconCount; ++index) {
        m_iconRenderData[index].running = m_displayApps[index].runningWindow != nullptr;
    }
}

bool DockApp::RebuildDisplayApps() {
    std::vector<DisplayApp> displayApps;
    displayApps.reserve(m_config.Pins().size() + m_windows.RunningWindows().size() + 2U);
    std::vector<std::wstring> displayedTargets;
    displayedTargets.reserve(m_config.Pins().size() + m_windows.RunningWindows().size() + 2U);

    displayApps.push_back({{L"Start", kStartTarget, L"", L""}, nullptr, -1});
    displayApps.push_back({{L"Search", kSearchTarget, L"", L""}, nullptr, -1});
    displayedTargets.push_back(kStartTarget);
    displayedTargets.push_back(kSearchTarget);

    for (size_t index = 0; index < m_config.Pins().size(); ++index) {
        const PinnedApp& pin = m_config.Pins()[index];
        if (IsSpecialDockTarget(pin.target)) {
            continue;
        }
        const bool alreadyDisplayed = std::ranges::any_of(displayedTargets,
            [&pin](const std::wstring& target) {
                return WindowCatalog::TargetsMatch(target, pin.target);
            });
        if (alreadyDisplayed) {
            continue;
        }
        displayedTargets.push_back(pin.target);
        displayApps.push_back({pin, m_windows.FindWindowFor(pin), static_cast<int>(index)});
    }

    for (const RunningWindow& window : m_windows.RunningWindows()) {
        const bool alreadyDisplayed = std::ranges::any_of(displayedTargets,
            [&window](const std::wstring& target) {
                return WindowCatalog::TargetsMatch(target, window.executablePath);
            });
        if (alreadyDisplayed) {
            continue;
        }
        displayedTargets.push_back(window.executablePath);

        PinnedApp app;
        app.name = window.title.empty() ? DisplayNameFromExecutable(window.executablePath) : window.title;
        app.target = window.executablePath;
        displayApps.push_back({std::move(app), window.handle, -1});
    }

    const bool changed = displayApps.size() != m_displayApps.size() ||
        !std::equal(displayApps.begin(), displayApps.end(), m_displayApps.begin(),
            [](const DisplayApp& left, const DisplayApp& right) {
                return left.persistentPinIndex == right.persistentPinIndex &&
                    left.app.target == right.app.target;
            });
    m_displayApps = std::move(displayApps);
    return changed;
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

void DockApp::LogInputMouse(UINT message, POINT screenPoint, int icon) const {
    std::wstring event;
    switch (message) {
    case WM_LBUTTONDOWN:
        event = L"WM_LBUTTONDOWN";
        break;
    case WM_LBUTTONUP:
        event = L"WM_LBUTTONUP";
        break;
    case WM_RBUTTONUP:
        event = L"WM_RBUTTONUP";
        break;
    default:
        return;
    }
    Log(L"Input " + event + L" at (" + std::to_wstring(screenPoint.x) + L", " +
        std::to_wstring(screenPoint.y) + L"), icon " + std::to_wstring(icon));
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
    if (ScreenToClient(m_window, &cursor) == FALSE) {
        return -1;
    }

    for (size_t index = 0; index < m_iconRenderData.size(); ++index) {
        if (IsInside(m_iconRenderData[index].bounds, cursor.x, cursor.y)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int DockApp::InsertionIndexFor(POINT cursor) const noexcept {
    if (ScreenToClient(m_window, &cursor) == FALSE) {
        return -1;
    }

    for (size_t index = 0; index < m_iconRenderData.size(); ++index) {
        const RECT& bounds = m_iconRenderData[index].bounds;
        const LONG center = bounds.left + (bounds.right - bounds.left) / 2;
        if (cursor.x < center) {
            return static_cast<int>(index);
        }
    }
    return static_cast<int>(m_iconRenderData.size());
}

bool DockApp::IsPersistentDisplayIcon(int icon) const noexcept {
    return icon >= 0 && static_cast<size_t>(icon) < m_displayApps.size() &&
        !IsSpecialDockTarget(m_displayApps[static_cast<size_t>(icon)].app.target) &&
        m_displayApps[static_cast<size_t>(icon)].persistentPinIndex >= 0;
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
