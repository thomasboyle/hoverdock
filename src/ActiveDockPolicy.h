#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
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
inline constexpr uint32_t kBackdropRestingPollMs = 33U;
inline constexpr uint32_t kBackdropIdleTicksBeforeRest = 20U;

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
    const size_t idx = FindNearestMonitorIndex(pt, monitors);
    const RECT& bounds = (idx != static_cast<size_t>(-1)) ? monitors[idx].second : fallbackBounds;
    return pt.x >= bounds.left && pt.x < bounds.right &&
        pt.y >= bounds.bottom - hotZonePx && pt.y < bounds.bottom;
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

[[nodiscard]] constexpr bool IsSuppressedSelfDirt(const RECT& dirty, const RECT& selfBounds,
    uint64_t nowMs, uint64_t lastSelfPresentMs) noexcept {
    return lastSelfPresentMs != 0 && nowMs >= lastSelfPresentMs &&
        (nowMs - lastSelfPresentMs) <= kSelfPresentSuppressWindowMs &&
        RectContains(selfBounds, dirty);
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

}  // namespace active_dock_policy
