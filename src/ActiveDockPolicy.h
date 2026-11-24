#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#ifndef HOVERDOCK_PORTABLE_WIN_TYPES
#define HOVERDOCK_PORTABLE_WIN_TYPES
using HMONITOR = void*;
struct POINT {
    long x;
    long y;
};
struct RECT {
    long left;
    long top;
    long right;
    long bottom;
};
#endif
#endif

namespace active_dock_policy {

inline constexpr uint64_t kSelfPresentSuppressWindowMs = 45ULL;
inline constexpr uint64_t kSelfPresentCatchUpDelayMs = 65ULL;
inline constexpr uint32_t kCalmHealthyHookWatchMs = 1000U;

[[nodiscard]] constexpr bool PointInRect(POINT pt, const RECT& r) noexcept {
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

[[nodiscard]] constexpr bool RectContains(const RECT& outer, const RECT& inner) noexcept {
    return inner.right > inner.left && inner.bottom > inner.top &&
        inner.left >= outer.left && inner.top >= outer.top &&
        inner.right <= outer.right && inner.bottom <= outer.bottom;
}

[[nodiscard]] inline size_t FindNearestMonitorIndex(POINT pt,
    std::span<const std::pair<HMONITOR, RECT>> monitors) noexcept {
    if (monitors.empty()) {
        return static_cast<size_t>(-1);
    }
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (PointInRect(pt, monitors[i].second)) {
            return i;
        }
    }
    size_t bestIndex = 0;
    long long bestDist2 = -1;
    for (size_t i = 0; i < monitors.size(); ++i) {
        const RECT& r = monitors[i].second;
        const long long dx = pt.x < r.left ? (static_cast<long long>(r.left) - pt.x)
            : (pt.x >= r.right ? (static_cast<long long>(pt.x) - (r.right - 1)) : 0LL);
        const long long dy = pt.y < r.top ? (static_cast<long long>(r.top) - pt.y)
            : (pt.y >= r.bottom ? (static_cast<long long>(pt.y) - (r.bottom - 1)) : 0LL);
        const long long dist2 = dx * dx + dy * dy;
        if (bestDist2 < 0 || dist2 < bestDist2) {
            bestDist2 = dist2;
            bestIndex = i;
        }
    }
    return bestIndex;
}

[[nodiscard]] inline bool IsPointInMonitorBottomHotZone(POINT pt,
    std::span<const std::pair<HMONITOR, RECT>> monitors, const RECT& fallbackBounds,
    long hotZonePx) noexcept {
    if (monitors.empty()) {
        return pt.y >= fallbackBounds.bottom - hotZonePx && pt.y < fallbackBounds.bottom &&
            pt.x >= fallbackBounds.left && pt.x < fallbackBounds.right;
    }
    // Check Y first: >99% of desktop mouse moves are above the 8 px bottom
    // strip and reject on the very first comparison without squared-distance math.
    for (const auto& entry : monitors) {
        const RECT& r = entry.second;
        if (pt.y >= r.bottom - hotZonePx && pt.y < r.bottom &&
            pt.x >= r.left && pt.x < r.right) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool ShouldPostHookPointer(bool hiddenOrHiding,
    bool inHotZone, bool borderlessFullscreenAtCursor, bool overDockPill,
    bool holdPopupOpen, bool hasDockHoverOrGlint) noexcept {
    if (hiddenOrHiding) {
        return inHotZone && !borderlessFullscreenAtCursor;
    }
    if (overDockPill) {
        return false;
    }
    return holdPopupOpen ? hasDockHoverOrGlint : true;
}

[[nodiscard]] constexpr bool RectIsEmpty(const RECT& r) noexcept {
    return r.right <= r.left || r.bottom <= r.top;
}

[[nodiscard]] constexpr RECT UnionRects(const RECT& a, const RECT& b) noexcept {
    if (RectIsEmpty(a)) {
        return b;
    }
    if (RectIsEmpty(b)) {
        return a;
    }
    return RECT{
        a.left < b.left ? a.left : b.left,
        a.top < b.top ? a.top : b.top,
        a.right > b.right ? a.right : b.right,
        a.bottom > b.bottom ? a.bottom : b.bottom,
    };
}

[[nodiscard]] constexpr bool IsSuppressedSelfDirt(const RECT& dirty, const RECT& selfBounds,
    uint64_t nowMs, uint64_t lastSelfPresentMs) noexcept {
    return lastSelfPresentMs != 0 && nowMs >= lastSelfPresentMs &&
        (nowMs - lastSelfPresentMs) <= kSelfPresentSuppressWindowMs &&
        RectContains(selfBounds, dirty);
}

[[nodiscard]] constexpr bool IsSuppressedDockOrHoverDirt(const RECT& dirty, const RECT& dockBounds,
    uint64_t nowMs, uint64_t lastDockSelfPresentMs, const RECT& hoverBounds,
    uint64_t lastHoverSelfPresentMs) noexcept {
    const bool dockRecent = lastDockSelfPresentMs != 0 && nowMs >= lastDockSelfPresentMs &&
        (nowMs - lastDockSelfPresentMs) <= kSelfPresentSuppressWindowMs;
    const bool hoverRecent = !RectIsEmpty(hoverBounds) && lastHoverSelfPresentMs != 0 &&
        nowMs >= lastHoverSelfPresentMs &&
        (nowMs - lastHoverSelfPresentMs) <= kSelfPresentSuppressWindowMs;
    if (dockRecent && RectContains(dockBounds, dirty)) {
        return true;
    }
    if (hoverRecent && RectContains(hoverBounds, dirty)) {
        return true;
    }
    if (dockRecent && hoverRecent && RectContains(UnionRects(dockBounds, hoverBounds), dirty)) {
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool EqualsIgnoreCaseAscii(std::wstring_view a,
    std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    constexpr auto lower = [](wchar_t c) constexpr noexcept {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    };
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool IsGameWindowClass(std::wstring_view cls) noexcept {
    if (cls.size() >= 4 && EqualsIgnoreCaseAscii(cls.substr(0, 4), L"GLFW")) {
        return std::all_of(cls.begin() + 4, cls.end(),
            [](wchar_t c) constexpr noexcept { return c >= L'0' && c <= L'9'; });
    }
    for (std::wstring_view name :
        {L"LWJGL", L"SDL_app", L"UnityWndClass", L"UnrealWindow", L"Godot_Engine"}) {
        if (EqualsIgnoreCaseAscii(cls, name)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool IsCursorClippedToWindow(const RECT& clip,
    const RECT& window, const RECT& virtualScreen) noexcept {
    constexpr long kTol = 4;
    const RECT expanded{
        window.left - kTol, window.top - kTol, window.right + kTol, window.bottom + kTol};
    return window.right > window.left && window.bottom > window.top &&
        virtualScreen.right > virtualScreen.left && virtualScreen.bottom > virtualScreen.top &&
        (clip.left > virtualScreen.left || clip.top > virtualScreen.top ||
            clip.right < virtualScreen.right || clip.bottom < virtualScreen.bottom) &&
        RectContains(expanded, clip);
}

struct ForegroundWindowSnapshot {
    bool hasForeground = false;
    bool hasCaptionOrThickFrame = false;
    bool isZoomed = false;
    bool coversMonitor = false;
    bool noChrome = false;
    bool isGameWindowClass = false;
    bool cursorClippedToWindow = false;
    bool d3dFullscreen = false;
    bool qunsBusy = false;
    bool fillsHalfMonitor = false;
};

[[nodiscard]] constexpr bool ShouldSuppressDockForForeground(
    const ForegroundWindowSnapshot& fg) noexcept {
    return fg.hasForeground && (
        (!fg.hasCaptionOrThickFrame && fg.coversMonitor) ||
        (fg.noChrome && fg.coversMonitor) ||
        (fg.isGameWindowClass && (fg.isZoomed || fg.coversMonitor)) ||
        fg.cursorClippedToWindow);
}

[[nodiscard]] constexpr bool ShouldSuppressSearchHotkeyForForeground(
    const ForegroundWindowSnapshot& fg) noexcept {
    return fg.d3dFullscreen || (fg.hasForeground && (
        ShouldSuppressDockForForeground(fg) ||
        fg.isGameWindowClass ||
        (fg.noChrome && fg.qunsBusy && fg.fillsHalfMonitor)));
}

inline void BoxBlurTruncated2Pass(float* data, int width, int height, int radius) {
    if (data == nullptr || width <= 0 || height <= 0 || radius <= 0) {
        return;
    }
    std::vector<float> temp(static_cast<size_t>(width) * static_cast<size_t>(height));
    std::vector<double> prefix(static_cast<size_t>((std::max)(width, height)) + 1U);

    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < height; ++y) {
            const float* srcRow = data + static_cast<size_t>(y) * static_cast<size_t>(width);
            float* dstRow = temp.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
            prefix[0] = 0.0;
            for (int x = 0; x < width; ++x) {
                prefix[static_cast<size_t>(x) + 1U] = prefix[static_cast<size_t>(x)] + srcRow[x];
            }
            for (int x = 0; x < width; ++x) {
                const int x0 = (std::max)(0, x - radius);
                const int x1 = (std::min)(width - 1, x + radius);
                dstRow[x] = static_cast<float>(
                    (prefix[static_cast<size_t>(x1) + 1U] - prefix[static_cast<size_t>(x0)]) /
                    static_cast<double>(x1 - x0 + 1));
            }
        }
        for (int x = 0; x < width; ++x) {
            prefix[0] = 0.0;
            for (int y = 0; y < height; ++y) {
                prefix[static_cast<size_t>(y) + 1U] = prefix[static_cast<size_t>(y)] +
                    temp[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
            }
            for (int y = 0; y < height; ++y) {
                const int y0 = (std::max)(0, y - radius);
                const int y1 = (std::min)(height - 1, y + radius);
                data[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                    static_cast<float>(
                        (prefix[static_cast<size_t>(y1) + 1U] - prefix[static_cast<size_t>(y0)]) /
                        static_cast<double>(y1 - y0 + 1));
            }
        }
    }
}

enum class StartMenuAction : uint8_t {
    None = 0,
    SysCommand = 1,
    WinKey = 2,
    CtrlEsc = 3,
};

[[nodiscard]] constexpr StartMenuAction NextStartMenuAction(uint8_t nextStage,
    bool startNowVisible, bool expectedVisible, bool foregroundChangedToOtherWindow,
    bool foregroundElevated) noexcept {
    if (startNowVisible == expectedVisible) {
        return StartMenuAction::None;
    }
    if (expectedVisible && foregroundChangedToOtherWindow) {
        return StartMenuAction::None;
    }
    if (nextStage == 1U) {
        // SendInput is silently dropped by UIPI when an elevated window holds
        // foreground; retry WM_SYSCOMMAND/SC_TASKLIST after fullscreen-claim
        // unmarking settles instead of injecting blocked keyboard input.
        return foregroundElevated ? StartMenuAction::SysCommand : StartMenuAction::WinKey;
    }
    if (nextStage == 2U) {
        return foregroundElevated ? StartMenuAction::None : StartMenuAction::CtrlEsc;
    }
    return StartMenuAction::None;
}

}  // namespace active_dock_policy
