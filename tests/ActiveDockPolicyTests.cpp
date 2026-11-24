// ActiveDockPolicy unit tests (portable; also built on Windows with -DDOCK_BUILD_TESTS=ON).

#include "ActiveDockPolicy.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                   \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                                   \
        }                                                                                   \
    } while (false)

void ReferenceBoxBlur2Pass(std::vector<float>& shadow, int width, int height, int blurRadius) {
    std::vector<float> blurTemp(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0F);
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                float sum = 0.0F;
                int count = 0;
                const int x0 = (std::max)(0, x - blurRadius);
                const int x1 = (std::min)(width - 1, x + blurRadius);
                for (int sx = x0; sx <= x1; ++sx) {
                    sum += shadow[static_cast<size_t>(y) * static_cast<size_t>(width) +
                        static_cast<size_t>(sx)];
                    ++count;
                }
                blurTemp[static_cast<size_t>(y) * static_cast<size_t>(width) +
                    static_cast<size_t>(x)] = count > 0 ? sum / static_cast<float>(count) : 0.0F;
            }
        }
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                float sum = 0.0F;
                int count = 0;
                const int y0 = (std::max)(0, y - blurRadius);
                const int y1 = (std::min)(height - 1, y + blurRadius);
                for (int sy = y0; sy <= y1; ++sy) {
                    sum += blurTemp[static_cast<size_t>(sy) * static_cast<size_t>(width) +
                        static_cast<size_t>(x)];
                    ++count;
                }
                shadow[static_cast<size_t>(y) * static_cast<size_t>(width) +
                    static_cast<size_t>(x)] = count > 0 ? sum / static_cast<float>(count) : 0.0F;
            }
        }
    }
}

void TestMonitorHotZoneAndHookGate() {
    using active_dock_policy::FindNearestMonitorIndex;
    using active_dock_policy::IsPointInMonitorBottomHotZone;
    using active_dock_policy::IsSuppressedSelfDirt;
    using active_dock_policy::ShouldPostHookPointer;

    const std::pair<HMONITOR, RECT> monitors[2] = {
        {reinterpret_cast<HMONITOR>(1), {0, 0, 2560, 1440}},
        {reinterpret_cast<HMONITOR>(2), {2560, 180, 4480, 1260}},
    };
    EXPECT(IsPointInMonitorBottomHotZone({1200, 1435}, monitors, monitors[0].second, 8));
    EXPECT(!IsPointInMonitorBottomHotZone({1200, 1430}, monitors, monitors[0].second, 8));
    EXPECT(IsPointInMonitorBottomHotZone({3000, 1255}, monitors, monitors[0].second, 8));
    EXPECT(FindNearestMonitorIndex({4600, 500}, monitors) == 1);

    EXPECT(ShouldPostHookPointer(true, true, false, false, false, false));
    EXPECT(!ShouldPostHookPointer(true, true, true, false, false, false));
    EXPECT(!ShouldPostHookPointer(false, false, false, true, false, true));
    EXPECT(ShouldPostHookPointer(false, false, false, false, true, true));
    EXPECT(!ShouldPostHookPointer(false, false, false, false, true, false));
    EXPECT(ShouldPostHookPointer(false, false, false, false, false, false));

    const RECT dockRect{680, 1310, 1880, 1436};
    EXPECT(IsSuppressedSelfDirt({700, 1320, 1860, 1430}, dockRect, 1020ULL, 1000ULL));
    EXPECT(!IsSuppressedSelfDirt({700, 1320, 1860, 1430}, dockRect, 1200ULL, 1000ULL));
    EXPECT(!IsSuppressedSelfDirt({0, 0, 2560, 1440}, dockRect, 1010ULL, 1000ULL));
}

void TestExactSlidingHoverShadowBlur() {
    constexpr int kWidth = 260;
    constexpr int kHeight = 84;
    constexpr int kRadius = 10;
    std::vector<float> initial(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight), 0.0F);
    for (int y = 18; y < kHeight - 20; ++y) {
        for (int x = 18; x < kWidth - 18; ++x) {
            const float dx = static_cast<float>((std::min)(x - 18, (kWidth - 19) - x));
            const float dy = static_cast<float>((std::min)(y - 18, (kHeight - 21) - y));
            initial[static_cast<size_t>(y) * static_cast<size_t>(kWidth) + static_cast<size_t>(x)] =
                std::clamp((std::min)(dx, dy) / 4.0F, 0.0F, 1.0F);
        }
    }

    std::vector<float> expected = initial;
    ReferenceBoxBlur2Pass(expected, kWidth, kHeight, kRadius);

    std::vector<float> actual = initial;
    active_dock_policy::BoxBlurTruncated2Pass(actual.data(), kWidth, kHeight, kRadius);

    float maxDiff = 0.0F;
    int quantDiffs = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        maxDiff = (std::max)(maxDiff, std::fabs(actual[i] - expected[i]));
        const int qRef = static_cast<int>(std::lround(expected[i] * 0.28F * 255.0F));
        const int qAct = static_cast<int>(std::lround(actual[i] * 0.28F * 255.0F));
        if (qRef != qAct) {
            ++quantDiffs;
        }
    }
    EXPECT(maxDiff < 1.0e-6F);
    EXPECT(quantDiffs == 0);
}

void TestGameAndFullscreenSuppression() {
    using active_dock_policy::ForegroundWindowSnapshot;
    using active_dock_policy::IsCursorClippedToWindow;
    using active_dock_policy::IsGameWindowClass;
    using active_dock_policy::ShouldSuppressDockForForeground;
    using active_dock_policy::ShouldSuppressSearchHotkeyForForeground;

    // Minecraft 1.13+ (GLFW30 on OpenJDK), legacy Minecraft 1.6-1.12.2 (LWJGL),
    // and common native game-engine window classes.
    EXPECT(IsGameWindowClass(L"GLFW30"));
    EXPECT(IsGameWindowClass(L"glfw31"));
    EXPECT(IsGameWindowClass(L"GLFW"));
    EXPECT(IsGameWindowClass(L"LWJGL"));
    EXPECT(IsGameWindowClass(L"SDL_app"));
    EXPECT(IsGameWindowClass(L"UnityWndClass"));
    EXPECT(IsGameWindowClass(L"UnrealWindow"));
    EXPECT(IsGameWindowClass(L"Godot_Engine"));

    // Non-game Java/desktop classes must not be classified as games.
    EXPECT(!IsGameWindowClass(L""));
    EXPECT(!IsGameWindowClass(L"SunAwtFrame"));
    EXPECT(!IsGameWindowClass(L"Chrome_WidgetWin_1"));
    EXPECT(!IsGameWindowClass(L"CabinetWClass"));
    EXPECT(!IsGameWindowClass(L"UnityContainerWndClass"));
    EXPECT(!IsGameWindowClass(L"GLFWHelper"));

    // Cursor confinement checks.
    const RECT virtualSingle{0, 0, 2560, 1440};
    const RECT maximizedMc{-8, -8, 2568, 1448};
    EXPECT(!IsCursorClippedToWindow(virtualSingle, maximizedMc, virtualSingle));
    EXPECT(IsCursorClippedToWindow({0, 31, 2560, 1440}, maximizedMc, virtualSingle));
    EXPECT(IsCursorClippedToWindow({508, 331, 1346, 772}, {500, 300, 1354, 780}, virtualSingle));
    EXPECT(!IsCursorClippedToWindow({100, 100, 300, 300}, {500, 300, 1354, 780}, virtualSingle));

    // Maximized Minecraft modpack (OpenJDK/GLFW30 with caption+frame, cursor
    // free in inventory/hotbar): suppresses both dock edge-show and Ctrl+Space.
    ForegroundWindowSnapshot maximizedMinecraft{};
    maximizedMinecraft.hasForeground = true;
    maximizedMinecraft.hasCaptionOrThickFrame = true;
    maximizedMinecraft.isZoomed = true;
    maximizedMinecraft.coversMonitor = true;
    maximizedMinecraft.noChrome = false;
    maximizedMinecraft.isGameWindowClass = true;
    EXPECT(ShouldSuppressDockForForeground(maximizedMinecraft));
    EXPECT(ShouldSuppressSearchHotkeyForForeground(maximizedMinecraft));

    // Restored (non-maximized) Minecraft window: Ctrl+Space still suppressed so
    // sprint+jump never opens Search; dock remains available when cursor is free,
    // and suppressed while 3D camera pointer-lock clips the cursor.
    ForegroundWindowSnapshot windowedMinecraft{};
    windowedMinecraft.hasForeground = true;
    windowedMinecraft.hasCaptionOrThickFrame = true;
    windowedMinecraft.isGameWindowClass = true;
    EXPECT(!ShouldSuppressDockForForeground(windowedMinecraft));
    EXPECT(ShouldSuppressSearchHotkeyForForeground(windowedMinecraft));
    windowedMinecraft.cursorClippedToWindow = true;
    EXPECT(ShouldSuppressDockForForeground(windowedMinecraft));
    EXPECT(ShouldSuppressSearchHotkeyForForeground(windowedMinecraft));

    // Normal maximized desktop app (browser, VS Code, IntelliJ SunAwtFrame):
    // keeps both dock edge-show and Ctrl+Space.
    ForegroundWindowSnapshot normalMaximized{};
    normalMaximized.hasForeground = true;
    normalMaximized.hasCaptionOrThickFrame = true;
    normalMaximized.isZoomed = true;
    normalMaximized.coversMonitor = true;
    normalMaximized.noChrome = false;
    normalMaximized.isGameWindowClass = false;
    EXPECT(!ShouldSuppressDockForForeground(normalMaximized));
    EXPECT(!ShouldSuppressSearchHotkeyForForeground(normalMaximized));

    // Chromeless F11 / windowed-fullscreen keeps caption/frame flags but has 0
    // non-client chrome: suppresses both dock and Ctrl+Space.
    ForegroundWindowSnapshot chromelessFullscreen = normalMaximized;
    chromelessFullscreen.noChrome = true;
    EXPECT(ShouldSuppressDockForForeground(chromelessFullscreen));
    EXPECT(ShouldSuppressSearchHotkeyForForeground(chromelessFullscreen));

    // Exclusive D3D fullscreen suppresses Ctrl+Space even without a foreground HWND.
    ForegroundWindowSnapshot d3dOnly{};
    d3dOnly.d3dFullscreen = true;
    EXPECT(!ShouldSuppressDockForForeground(d3dOnly));
    EXPECT(ShouldSuppressSearchHotkeyForForeground(d3dOnly));
}

void TestStartMenuFallbackPolicy() {
    using active_dock_policy::NextStartMenuAction;
    using active_dock_policy::StartMenuAction;

    // Already opened (or closed): never fire a second toggle.
    EXPECT(NextStartMenuAction(1U, true, true, false, false) == StartMenuAction::None);
    EXPECT(NextStartMenuAction(1U, false, false, false, false) == StartMenuAction::None);

    // Foreground moved to another window (e.g. Start host or third-party Start
    // menu) while trying to open: treat as opened so we don't toggle it shut.
    EXPECT(NextStartMenuAction(1U, false, true, true, false) == StartMenuAction::None);

    // Non-elevated foreground and Start still closed: escalate WinKey -> CtrlEsc -> None.
    EXPECT(NextStartMenuAction(1U, false, true, false, false) == StartMenuAction::WinKey);
    EXPECT(NextStartMenuAction(2U, false, true, false, false) == StartMenuAction::CtrlEsc);
    EXPECT(NextStartMenuAction(3U, false, true, false, false) == StartMenuAction::None);

    // Elevated foreground blocks SendInput via UIPI: retry SC_TASKLIST once, then stop.
    EXPECT(NextStartMenuAction(1U, false, true, false, true) == StartMenuAction::SysCommand);
    EXPECT(NextStartMenuAction(2U, false, true, false, true) == StartMenuAction::None);
}

void TestHoverLabelSelfDirtSuppression() {
    using active_dock_policy::IsSuppressedDockOrHoverDirt;

    const RECT dockRect{680, 1310, 1880, 1436};
    // Hover bubble overlaps the top shadow margin of dockRect and extends above it.
    const RECT hoverRect{900, 1270, 1100, 1335};

    // Dock-only self-dirt.
    EXPECT(IsSuppressedDockOrHoverDirt({700, 1320, 1860, 1430}, dockRect, 1020ULL, 1000ULL,
        hoverRect, 0ULL));
    // Hover-label-only dirt extending above dockRect.top.
    EXPECT(IsSuppressedDockOrHoverDirt({900, 1270, 1100, 1335}, dockRect, 1020ULL, 0ULL,
        hoverRect, 1000ULL));
    // Merged DWM dirty rect covering both dock and hover label when both presented recently.
    EXPECT(IsSuppressedDockOrHoverDirt({680, 1270, 1880, 1436}, dockRect, 1020ULL, 1005ULL,
        hoverRect, 1010ULL));
    // Merged rect must NOT be suppressed if only the dock (or only the hover label) presented.
    EXPECT(!IsSuppressedDockOrHoverDirt({680, 1270, 1880, 1436}, dockRect, 1020ULL, 1005ULL,
        hoverRect, 0ULL));
    EXPECT(!IsSuppressedDockOrHoverDirt({680, 1270, 1880, 1436}, dockRect, 1020ULL, 0ULL,
        hoverRect, 1010ULL));
    // External window dirt outside both dock and hover label must never be suppressed.
    EXPECT(!IsSuppressedDockOrHoverDirt({0, 0, 2560, 1440}, dockRect, 1020ULL, 1005ULL,
        hoverRect, 1010ULL));
    // Past the suppression window: no longer suppressed.
    EXPECT(!IsSuppressedDockOrHoverDirt({900, 1270, 1100, 1335}, dockRect, 1100ULL, 1005ULL,
        hoverRect, 1010ULL));
}

}  // namespace

int main() {
    TestMonitorHotZoneAndHookGate();
    TestExactSlidingHoverShadowBlur();
    TestGameAndFullscreenSuppression();
    TestStartMenuFallbackPolicy();
    TestHoverLabelSelfDirtSuppression();
    if (g_failures != 0) {
        std::fprintf(stderr, "ActiveDockPolicyTests: %d failure(s).\n", g_failures);
        return 1;
    }
    std::printf("ActiveDockPolicyTests: all passed.\n");
    return 0;
}
