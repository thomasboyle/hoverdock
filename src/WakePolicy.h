#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#ifdef _WIN32
#include <Windows.h>
#else
struct RECT {
    long left;
    long top;
    long right;
    long bottom;
};
#endif

namespace wake_policy {

inline constexpr double kPostWakeFastWatchSeconds = 10.0;
inline constexpr double kSteadyCatalogCatchUpBudgetSeconds = 0.012;
inline constexpr uint32_t kWakeLayoutSettleMs = 140U;
inline constexpr uint32_t kWakeLayoutFollowUpMs = 350U;
inline constexpr int kWakeLayoutFollowUpPasses = 6;
inline constexpr int kMaxWindowRestoresPerWake = 2;
inline constexpr double kWorkAreaStormWindowSeconds = 2.0;
inline constexpr int kWorkAreaStormMaxEvents = 6;

[[nodiscard]] constexpr bool RectEqual(const RECT& a, const RECT& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

[[nodiscard]] constexpr long RectWidth(const RECT& r) noexcept {
    return r.right - r.left;
}

[[nodiscard]] constexpr long RectHeight(const RECT& r) noexcept {
    return r.bottom - r.top;
}

[[nodiscard]] constexpr bool RectNonEmpty(const RECT& r) noexcept {
    return r.right > r.left && r.bottom > r.top;
}

// Shell windows that own desktop/taskbar layout and must never be sent
// synthetic WM_SETTINGCHANGE(SPI_SETWORKAREA) notifications or moved by wake
// layout repair (doing so pokes Explorer into re-reserving the taskbar strip).
[[nodiscard]] constexpr bool IsShellWorkAreaWindowClass(std::wstring_view className) noexcept {
    return className == L"Shell_TrayWnd" ||
        className == L"Shell_SecondaryTrayWnd" ||
        className == L"Progman" ||
        className == L"WorkerW" ||
        className == L"XamlExplorerHostIslandWindow";
}

[[nodiscard]] constexpr bool ShouldReloadIconsForExtent(uint32_t currentExtent,
    uint32_t loadedExtent) noexcept {
    return currentExtent > 0 && (loadedExtent == 0 || currentExtent != loadedExtent);
}

[[nodiscard]] constexpr bool ShouldUsePostWakeCursorWatch(bool hidden, bool sessionLocked,
    double nowSeconds, double postWakeUntilSeconds) noexcept {
    return hidden && !sessionLocked && postWakeUntilSeconds > 0.0 &&
        nowSeconds < postWakeUntilSeconds;
}

[[nodiscard]] constexpr double CatalogCatchUpBudgetSeconds(double nowSeconds,
    double postWakeUntilSeconds) noexcept {
    return (postWakeUntilSeconds > 0.0 && nowSeconds < postWakeUntilSeconds)
        ? 0.0
        : kSteadyCatalogCatchUpBudgetSeconds;
}

// Detects silent kernel eviction of WH_MOUSE_LL (LowLevelHooksTimeout): cursor
// moved on an unlocked, non-elevated desktop with zero hook callbacks seen.
[[nodiscard]] constexpr bool ShouldReinstallDeadMouseHook(bool hasHook,
    bool elevatedForeground, bool sessionLocked, bool cursorMoved,
    uint64_t prevHookEventCount, uint64_t currentHookEventCount) noexcept {
    return hasHook && !elevatedForeground && !sessionLocked && cursorMoved &&
        currentHookEventCount == prevHookEventCount;
}

[[nodiscard]] constexpr bool ShouldCollapseTaskbarAppBar(bool forceCollapse,
    bool visibleBeforeHide, const RECT& windowRect, const RECT& monitorRect,
    bool monitorWorkAreaFull, bool hasAppBarRect, const RECT& appBarRect,
    const RECT* lastCollapsedWindowRect) noexcept {
    if (forceCollapse || visibleBeforeHide) {
        return true;
    }
    if (hasAppBarRect && RectHeight(appBarRect) > 0 && appBarRect.top < monitorRect.bottom) {
        if (!monitorWorkAreaFull || lastCollapsedWindowRect == nullptr) {
            return true;
        }
    }
    if (!monitorWorkAreaFull && lastCollapsedWindowRect == nullptr) {
        return true;
    }
    if (RectHeight(windowRect) <= 0 && windowRect.top >= monitorRect.bottom) {
        return false;
    }
    return lastCollapsedWindowRect == nullptr || !RectEqual(*lastCollapsedWindowRect, windowRect);
}

struct SavedWindowState {
    RECT savedRect{};
    bool zoomed = false;
    int restoreCount = 0;
    bool appliedWhileWorkAreaFull = false;
    long bottomAtLastApply = 0;
};

// Restores a window pulled out of the taskbar strip only once its monitor's
// rcWork is full-screen, and never loops if the window itself clamps height.
[[nodiscard]] inline bool ShouldRestoreWakeWindow(SavedWindowState& state,
    const RECT& currentRect, const RECT& monitorRect, const RECT& monitorWorkRect,
    long taskbarStripPx) noexcept {
    if (!RectNonEmpty(state.savedRect) || !RectNonEmpty(currentRect) ||
        !RectNonEmpty(monitorRect) || taskbarStripPx <= 0) {
        return false;
    }
    if (!RectEqual(monitorWorkRect, monitorRect)) {
        state.appliedWhileWorkAreaFull = false;
        return false;
    }
    if (RectEqual(currentRect, state.savedRect)) {
        state.appliedWhileWorkAreaFull = true;
        state.bottomAtLastApply = currentRect.bottom;
        return false;
    }
    if (state.savedRect.bottom <= monitorRect.bottom - taskbarStripPx ||
        currentRect.bottom >= state.savedRect.bottom) {
        return false;
    }
    constexpr long kHorizontalTolerancePx = 12;
    if (std::labs(currentRect.left - state.savedRect.left) > kHorizontalTolerancePx ||
        std::labs(RectWidth(currentRect) - RectWidth(state.savedRect)) > kHorizontalTolerancePx) {
        return false;
    }
    if (state.restoreCount >= kMaxWindowRestoresPerWake) {
        return false;
    }
    if (state.appliedWhileWorkAreaFull && currentRect.bottom >= state.bottomAtLastApply) {
        state.bottomAtLastApply = currentRect.bottom;
        return false;
    }
    ++state.restoreCount;
    state.appliedWhileWorkAreaFull = true;
    state.bottomAtLastApply = currentRect.bottom;
    return true;
}

[[nodiscard]] inline bool ComputeExpandedZoomedRect(const RECT& currentRect,
    const RECT& monitorRect, const RECT& monitorWorkRect, long maxTaskbarStripPx,
    RECT& outTargetRect) noexcept {
    if (!RectNonEmpty(currentRect) || !RectNonEmpty(monitorRect) ||
        maxTaskbarStripPx <= 0 || !RectEqual(monitorWorkRect, monitorRect)) {
        return false;
    }
    constexpr long kEdgeTol = 16;
    const long deficit = monitorRect.bottom - currentRect.bottom;
    if (currentRect.left > monitorRect.left + kEdgeTol ||
        currentRect.top > monitorRect.top + kEdgeTol ||
        currentRect.right < monitorRect.right - kEdgeTol ||
        deficit <= 0 || deficit > maxTaskbarStripPx + kEdgeTol) {
        return false;
    }
    const long overhangLeft = std::max<long>(0, monitorRect.left - currentRect.left);
    const long overhangTop = std::max<long>(0, monitorRect.top - currentRect.top);
    const long overhangRight = std::max<long>(0, currentRect.right - monitorRect.right);
    const long overhangBottom = overhangTop > 0 ? overhangTop : overhangRight;
    outTargetRect = {
        monitorRect.left - overhangLeft,
        monitorRect.top - overhangTop,
        monitorRect.right + overhangRight,
        monitorRect.bottom + overhangBottom,
    };
    return !RectEqual(currentRect, outTargetRect);
}

struct WorkAreaStormState {
    double windowStartSeconds = 0.0;
    int eventsInWindow = 0;

    void Reset() noexcept {
        windowStartSeconds = 0.0;
        eventsInWindow = 0;
    }

    [[nodiscard]] bool NoteShrinkEvent(double nowSeconds) noexcept {
        if (windowStartSeconds <= 0.0 ||
            (nowSeconds - windowStartSeconds) > kWorkAreaStormWindowSeconds) {
            windowStartSeconds = nowSeconds;
            eventsInWindow = 1;
            return true;
        }
        return ++eventsInWindow <= kWorkAreaStormMaxEvents;
    }
};

}  // namespace wake_policy
