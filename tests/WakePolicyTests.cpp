// WakePolicy unit tests (portable; also built on Windows with -DDOCK_BUILD_TESTS=ON).

#include "WakePolicy.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                   \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                                   \
        }                                                                                   \
    } while (false)

void TestIconReloadAndWakeActivationPolicy() {
    using wake_policy::CatalogCatchUpBudgetSeconds;
    using wake_policy::ShouldReloadIconsForExtent;
    using wake_policy::ShouldUsePostWakeCursorWatch;

    // WM_DISPLAYCHANGE on wake with unchanged DPI/scale must NOT reload icons.
    EXPECT(!ShouldReloadIconsForExtent(84U, 84U));
    EXPECT(ShouldReloadIconsForExtent(84U, 0U));
    EXPECT(ShouldReloadIconsForExtent(112U, 84U));
    EXPECT(!ShouldReloadIconsForExtent(0U, 84U));

    // Post-wake window enables fast cursor watch while hidden + unlocked, then
    // returns to zero-timer event-driven mode afterwards.
    EXPECT(ShouldUsePostWakeCursorWatch(true, false, 102.0, 110.0));
    EXPECT(!ShouldUsePostWakeCursorWatch(true, true, 102.0, 110.0));
    EXPECT(!ShouldUsePostWakeCursorWatch(false, false, 102.0, 110.0));
    EXPECT(!ShouldUsePostWakeCursorWatch(true, false, 110.5, 110.0));

    // BeginShow must not stall on background catalog I/O right after wake.
    EXPECT(CatalogCatchUpBudgetSeconds(105.0, 110.0) == 0.0);
    EXPECT(CatalogCatchUpBudgetSeconds(115.0, 110.0) == wake_policy::kSteadyCatalogCatchUpBudgetSeconds);
}

void TestDeadMouseHookSelfHeal() {
    using wake_policy::ShouldReinstallDeadMouseHook;

    // Silent kernel eviction: cursor moved on unlocked, non-elevated desktop
    // while hook handle existed, yet zero hook callbacks arrived.
    EXPECT(ShouldReinstallDeadMouseHook(true, false, false, true, 42ULL, 42ULL));

    // Healthy hook advanced event count: never reinstall.
    EXPECT(!ShouldReinstallDeadMouseHook(true, false, false, true, 42ULL, 45ULL));

    // Elevated foreground (UIPI) or locked Winlogon desktop legitimately blocks
    // WH_MOUSE_LL without meaning the hook was evicted.
    EXPECT(!ShouldReinstallDeadMouseHook(true, true, false, true, 42ULL, 42ULL));
    EXPECT(!ShouldReinstallDeadMouseHook(true, false, true, true, 42ULL, 42ULL));
    EXPECT(!ShouldReinstallDeadMouseHook(true, false, false, false, 42ULL, 42ULL));
    EXPECT(!ShouldReinstallDeadMouseHook(false, false, false, true, 42ULL, 42ULL));
}

void TestShellExclusionAndTaskbarCollapse() {
    using wake_policy::IsShellWorkAreaWindowClass;
    using wake_policy::ShouldCollapseTaskbarAppBar;

    EXPECT(IsShellWorkAreaWindowClass(L"Shell_TrayWnd"));
    EXPECT(IsShellWorkAreaWindowClass(L"Shell_SecondaryTrayWnd"));
    EXPECT(IsShellWorkAreaWindowClass(L"Progman"));
    EXPECT(IsShellWorkAreaWindowClass(L"WorkerW"));
    EXPECT(!IsShellWorkAreaWindowClass(L"Chrome_WidgetWin_1"));

    const RECT monitor{0, 0, 2560, 1440};
    const RECT staleTrayRect{0, 1392, 2560, 1440};
    const RECT zeroTrayRect{0, 1440, 2560, 1440};
    const RECT reservedAppBar{0, 1392, 2560, 1440};
    const RECT collapsedAppBar{0, 1440, 2560, 1440};

    // Steady idle: hidden taskbar whose stale rect matches lastCollapsed and
    // whose AppBar height is 0 must stay quiet (no redundant SHAppBarMessage).
    EXPECT(!ShouldCollapseTaskbarAppBar(
        false, false, staleTrayRect, monitor, true, true, collapsedAppBar, &staleTrayRect));
    EXPECT(!ShouldCollapseTaskbarAppBar(
        false, false, zeroTrayRect, monitor, true, true, collapsedAppBar, nullptr));

    // Regression: on wake, Explorer re-shows or re-reserves the 48 px AppBar
    // with the SAME {0, 1392, 2560, 1440} rect cached before sleep.
    EXPECT(ShouldCollapseTaskbarAppBar(
        true, false, staleTrayRect, monitor, false, true, reservedAppBar, &staleTrayRect));
    EXPECT(ShouldCollapseTaskbarAppBar(
        false, true, staleTrayRect, monitor, false, true, reservedAppBar, &staleTrayRect));
    EXPECT(ShouldCollapseTaskbarAppBar(
        false, false, zeroTrayRect, monitor, false, true, reservedAppBar, &zeroTrayRect));
}

void TestWindowRestoreAntiOscillation() {
    using wake_policy::ComputeExpandedZoomedRect;
    using wake_policy::SavedWindowState;
    using wake_policy::ShouldRestoreWakeWindow;
    using wake_policy::WorkAreaStormState;

    const RECT monitor{0, 0, 2560, 1440};
    const RECT shrunkWork{0, 0, 2560, 1392};
    const RECT fullWork{0, 0, 2560, 1440};

    SavedWindowState snapped{};
    snapped.savedRect = {-7, 0, 1287, 1447};
    snapped.zoomed = false;
    const RECT pulledUp{-7, 0, 1287, 1399};

    EXPECT(!ShouldRestoreWakeWindow(snapped, pulledUp, monitor, shrunkWork, 48));
    EXPECT(snapped.restoreCount == 0);

    EXPECT(ShouldRestoreWakeWindow(snapped, pulledUp, monitor, fullWork, 48));
    EXPECT(snapped.restoreCount == 1);

    const RECT gridClamped{-7, 0, 1287, 1432};
    EXPECT(!ShouldRestoreWakeWindow(snapped, gridClamped, monitor, fullWork, 48));
    EXPECT(!ShouldRestoreWakeWindow(snapped, gridClamped, monitor, fullWork, 48));
    EXPECT(snapped.restoreCount == 1);

    EXPECT(!ShouldRestoreWakeWindow(snapped, pulledUp, monitor, shrunkWork, 48));
    EXPECT(ShouldRestoreWakeWindow(snapped, pulledUp, monitor, fullWork, 48));
    EXPECT(snapped.restoreCount == 2);
    EXPECT(!ShouldRestoreWakeWindow(snapped, pulledUp, monitor, fullWork, 48));

    SavedWindowState userMoved{};
    userMoved.savedRect = {100, 200, 1100, 1440};
    EXPECT(!ShouldRestoreWakeWindow(
        userMoved, {320, 200, 1320, 1392}, monitor, fullWork, 48));

    const RECT clampedZoomed{-8, -8, 2568, 1400};
    RECT expanded{};
    EXPECT(!ComputeExpandedZoomedRect(clampedZoomed, monitor, shrunkWork, 48, expanded));
    EXPECT(ComputeExpandedZoomedRect(clampedZoomed, monitor, fullWork, 48, expanded));
    EXPECT((wake_policy::RectEqual(expanded, {-8, -8, 2568, 1448})));

    WorkAreaStormState storm{};
    for (int i = 0; i < wake_policy::kWorkAreaStormMaxEvents; ++i) {
        EXPECT(storm.NoteShrinkEvent(10.0 + i * 0.1));
    }
    EXPECT(!storm.NoteShrinkEvent(10.8));
    EXPECT(storm.NoteShrinkEvent(15.0));
}

}  // namespace

int main() {
    TestIconReloadAndWakeActivationPolicy();
    TestDeadMouseHookSelfHeal();
    TestShellExclusionAndTaskbarCollapse();
    TestWindowRestoreAntiOscillation();
    if (g_failures != 0) {
        std::fprintf(stderr, "WakePolicyTests: %d failure(s).\n", g_failures);
        return 1;
    }
    std::printf("WakePolicyTests: all passed.\n");
    return 0;
}
