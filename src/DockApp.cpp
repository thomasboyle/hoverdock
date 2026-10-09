#include "DockApp.h"
#include "CodeEditor.h"
#include "DockTheme.hlsli"
#include "PerfBoost.h"
#include "Profile.h"
#include "RecycleBin.h"
#include "Startup.h"
#include "Updater.h"
#include "Version.h"

#include <ShellScalingApi.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <WtsApi32.h>
#include <CommCtrl.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <windowsx.h>
#include <oleidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <malloc.h>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

DockApp* DockApp::s_instance = nullptr;

std::vector<uint8_t> BoxDownsamplePremultiplied(const std::vector<uint8_t>& source, UINT sourceExtent,
    UINT destExtent) {
    if (sourceExtent == destExtent) {
        return source;
    }

    std::vector<uint8_t> dest(static_cast<size_t>(destExtent) * destExtent * 4U, 0);
    if (sourceExtent == 0 || destExtent == 0 ||
        source.size() < static_cast<size_t>(sourceExtent) * sourceExtent * 4U) {
        return dest;
    }

    for (UINT y = 0; y < destExtent; ++y) {
        const UINT sourceY0 = y * sourceExtent / destExtent;
        const UINT sourceY1 = std::max(sourceY0 + 1U, (y + 1U) * sourceExtent / destExtent);
        for (UINT x = 0; x < destExtent; ++x) {
            const UINT sourceX0 = x * sourceExtent / destExtent;
            const UINT sourceX1 = std::max(sourceX0 + 1U, (x + 1U) * sourceExtent / destExtent);
            UINT blue = 0;
            UINT green = 0;
            UINT red = 0;
            UINT alpha = 0;
            UINT count = 0;
            for (UINT sourceY = sourceY0; sourceY < sourceY1; ++sourceY) {
                for (UINT sourceX = sourceX0; sourceX < sourceX1; ++sourceX) {
                    const size_t offset =
                        (static_cast<size_t>(sourceY) * sourceExtent + sourceX) * 4U;
                    blue += source[offset];
                    green += source[offset + 1];
                    red += source[offset + 2];
                    alpha += source[offset + 3];
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const size_t destOffset = (static_cast<size_t>(y) * destExtent + x) * 4U;
            dest[destOffset] = static_cast<uint8_t>(blue / count);
            dest[destOffset + 1] = static_cast<uint8_t>(green / count);
            dest[destOffset + 2] = static_cast<uint8_t>(red / count);
            dest[destOffset + 3] = static_cast<uint8_t>(alpha / count);
        }
    }
    return dest;
}

namespace dock_detail {

constexpr double kShowDurationSeconds = 0.050;
constexpr double kHideDurationSeconds = 0.050;
constexpr double kDragSnapDurationSeconds = 0.050;
constexpr double kDragThresholdLogicalPixels = 20.0;
constexpr double kMinDragPressSeconds = 0.100;
constexpr double kMaxPointerStallSeconds = 0.220;
constexpr size_t kMaxMissingIconsPerRefresh = 2;
constexpr float kMinDockScale = 0.75F;
constexpr float kMaxDockScale = 1.5F;
constexpr int kBottomHotZonePixels = 8;
constexpr UINT kRefreshIntervalMs = 15000;
constexpr BYTE kInputWindowAlpha = 1;
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif
constexpr wchar_t kStartTarget[] = L"dock:start";
constexpr wchar_t kSearchTarget[] = L"dock:search";
constexpr wchar_t kDividerTarget[] = L"dock:divider";

template <typename T>
constexpr T LesserOf(T left, T right) noexcept {
    return right < left ? right : left;
}

template <typename T>
constexpr T GreaterOf(T left, T right) noexcept {
    return left < right ? right : left;
}

constexpr int SaturatedInt(LONG value) noexcept {
    return value > static_cast<LONG>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : value < static_cast<LONG>(std::numeric_limits<int>::min())
        ? std::numeric_limits<int>::min()
        : static_cast<int>(value);
}

std::wstring ConfigDirectory(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? L"" : path.substr(0, separator);
}

bool IsInside(const RECT& bounds, LONG x, LONG y) {
    return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
}

POINT ClientFromDockOrigin(POINT screen, LONG windowX, LONG currentY) noexcept {
    return {screen.x - windowX, screen.y - currentY};
}


POINT ScreenFromDockClient(POINT client, LONG windowX, LONG currentY) noexcept {
    return {client.x + windowX, client.y + currentY};
}


POINT ScreenPointFromClient(HWND window, LPARAM lParam) {
    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (ClientToScreen(window, &point) == FALSE) {
        GetCursorPos(&point);
    }
    return point;
}

bool HasPostedMessage(HWND window, UINT message) noexcept {
    if (window == nullptr) {
        return false;
    }
    MSG pending{};
    return PeekMessageW(&pending, window, message, message, PM_NOREMOVE) != FALSE;
}

POINT CoalescePostedClientMoves(HWND window, POINT current) {
    MSG pending{};
    POINT point = current;
    while (PeekMessageW(&pending, window, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE) != FALSE) {
        point = ScreenPointFromClient(window, pending.lParam);
    }
    return point;
}

POINT CoalescePostedPointers(HWND window, UINT message, POINT current) {
    MSG pending{};
    POINT point = current;
    while (PeekMessageW(&pending, window, message, message, PM_REMOVE) != FALSE) {
        point.x = static_cast<LONG>(static_cast<INT_PTR>(pending.wParam));
        point.y = static_cast<LONG>(pending.lParam);
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

bool IsDividerTarget(const std::wstring& target) {
    return target == kDividerTarget;
}

bool IsLayoutOnlyTarget(const std::wstring& target) {
    return IsDividerTarget(target);
}

bool EnsureWindowCapturable(HWND window) {
    // Stay WDA_NONE so Snipping Tool, Game Bar, and desktop capture see the
    // window on every frame. The glass sample excludes it with the magnifier
    // filter list instead of toggling WDA_EXCLUDEFROMCAPTURE (that toggle
    // blanked the dock in recordings).
    return window != nullptr && SetWindowDisplayAffinity(window, WDA_NONE) != FALSE;
}

void ApplyPopupDwmPlate(HWND window) noexcept {
    if (window == nullptr) {
        return;
    }
    const DWORD corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const DWORD backdrop = DWMSBT_NONE;
    DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    // Blur-behind was painting an opaque black plate in the corner wedges.
    // Leave it off. An empty-but-enabled blur region is what kept the squares.
    DWM_BLURBEHIND blur{};
    blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    blur.fEnable = FALSE;
    blur.hRgnBlur = nullptr;
    DwmEnableBlurBehindWindow(window, &blur);
    const DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(window, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    const COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

BOOL CALLBACK ApplyPopupChildPlate(HWND child, LPARAM) {
    ApplyPopupDwmPlate(child);
    return TRUE;
}

void ClearPopupLayeredCorners(HWND window) {
    if (window == nullptr) {
        return;
    }
    // Windows 11 DWM rounds top-level HWNDs and fills the clipped corner
    // wedges with an opaque black plate, including on WS_EX_LAYERED popups.
    // Quick Settings already draws a frosted squircle in per-pixel alpha, so
    // that plate reads as black squares. Opt out of system rounding, Mica,
    // acrylic, and the non-client frame, and do the same for any child HWND
    // (the magnifier host's child is one of these) so it cannot paint an
    // opaque rectangle outside the radius.
    ApplyPopupDwmPlate(window);
    EnumChildWindows(window, ApplyPopupChildPlate, 0);
}

float PopupSquircleDistance(float x, float y, float width, float height, float radius) noexcept {
    const float halfW = width * 0.5F;
    const float halfH = height * 0.5F;
    const float corner = std::min(radius, std::min(halfW, halfH));
    const float qx = std::fabs(x - halfW) - (halfW - corner);
    const float qy = std::fabs(y - halfH) - (halfH - corner);
    const float outside = std::sqrt(std::max(qx, 0.0F) * std::max(qx, 0.0F) +
        std::max(qy, 0.0F) * std::max(qy, 0.0F));
    const float inside = std::min(std::max(qx, qy), 0.0F);
    return outside + inside - corner;
}

// GlassPS draws the panel drop shadow into the square wedges outside the
// squircle (panels have no shadow margin, so the shade is clipped by the HWND
// into a dark rectangle). Force those pixels to exact transparent black so
// UpdateLayeredWindow and the window region both let the wallpaper through.
void PunchPopupSquircleWedges(HWND window, uint8_t* pixels, LONG width, LONG height,
    bool preservePopupShadow) noexcept {
    if (preservePopupShadow || pixels == nullptr || width <= 1 || height <= 1) {
        return;
    }
    UINT dpi = 96;
    if (window != nullptr) {
        const UINT windowDpi = GetDpiForWindow(window);
        if (windowDpi > 0) {
            dpi = windowDpi;
        }
    }
    const float dpiScale = std::max(static_cast<float>(dpi) / 96.0F, 1.0F);
    const float radius = DOCK_CORNER_RADIUS_PT * dpiScale;
    const float aa = 1.35F * dpiScale;
    const int reach = std::max(2, static_cast<int>(std::ceil(radius + aa + 1.0F)));
    const int origins[4][2] = {{0, 0}, {std::max(0, static_cast<int>(width) - reach), 0},
        {0, std::max(0, static_cast<int>(height) - reach)},
        {std::max(0, static_cast<int>(width) - reach), std::max(0, static_cast<int>(height) - reach)}};
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    for (const auto& origin : origins) {
        const int x0 = origin[0];
        const int y0 = origin[1];
        const int x1 = std::min(static_cast<int>(width), x0 + reach);
        const int y1 = std::min(static_cast<int>(height), y0 + reach);
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = pixels + (static_cast<size_t>(y) * static_cast<size_t>(width)) * 4U;
            for (int x = x0; x < x1; ++x) {
                const float distance = PopupSquircleDistance(static_cast<float>(x) + 0.5F,
                    static_cast<float>(y) + 0.5F, fw, fh, radius);
                if (distance < aa) {
                    continue;
                }
                uint8_t* pixel = row + static_cast<size_t>(x) * 4U;
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                pixel[3] = 0;
            }
        }
    }
}

void ClipLayeredPopupToCoverage(HWND window, const uint8_t* pixels, LONG width, LONG height) noexcept {
    if (window == nullptr || pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    try {
        std::vector<RECT> spans;
        spans.reserve(static_cast<size_t>(height));
        for (LONG y = 0; y < height; ++y) {
            const uint8_t* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(width) * 4U;
            LONG x = 0;
            while (x < width) {
                while (x < width && row[static_cast<size_t>(x) * 4U + 3U] == 0) {
                    ++x;
                }
                if (x >= width) {
                    break;
                }
                const LONG startX = x;
                while (x < width && row[static_cast<size_t>(x) * 4U + 3U] != 0) {
                    ++x;
                }
                spans.push_back(RECT{startX, y, x, y + 1});
            }
        }
        if (spans.empty()) {
            return;
        }
        uint32_t hash = 2166136261U;
        hash ^= static_cast<uint32_t>(width);
        hash *= 16777619U;
        hash ^= static_cast<uint32_t>(height);
        hash *= 16777619U;
        hash ^= static_cast<uint32_t>(spans.size());
        hash *= 16777619U;
        const size_t sample = std::min<size_t>(spans.size(), 8U);
        for (size_t index = 0; index < sample; ++index) {
            const RECT& span = spans[(index * (spans.size() - 1)) / std::max<size_t>(sample - 1, 1U)];
            hash ^= static_cast<uint32_t>(span.left);
            hash *= 16777619U;
            hash ^= static_cast<uint32_t>(span.right);
            hash *= 16777619U;
        }
        struct RegionCache {
            HWND window = nullptr;
            uint32_t hash = 0;
        };
        static RegionCache cache[4]{};
        for (RegionCache& entry : cache) {
            if (entry.window == window && entry.hash == hash) {
                return;
            }
        }
        const DWORD count = static_cast<DWORD>(spans.size());
        const DWORD bytes = sizeof(RGNDATAHEADER) + count * sizeof(RECT);
        std::vector<uint8_t> buffer(bytes);
        auto* data = reinterpret_cast<RGNDATA*>(buffer.data());
        data->rdh.dwSize = sizeof(RGNDATAHEADER);
        data->rdh.iType = RDH_RECTANGLES;
        data->rdh.nCount = count;
        data->rdh.nRgnSize = count * sizeof(RECT);
        data->rdh.rcBound = RECT{0, 0, width, height};
        std::memcpy(data->Buffer, spans.data(), static_cast<size_t>(count) * sizeof(RECT));
        HRGN region = ExtCreateRegion(nullptr, bytes, data);
        if (region == nullptr) {
            return;
        }
        if (SetWindowRgn(window, region, FALSE) == 0) {
            DeleteObject(region);
            return;
        }
        for (RegionCache& entry : cache) {
            if (entry.window == nullptr || entry.window == window) {
                entry.window = window;
                entry.hash = hash;
                return;
            }
        }
        cache[0] = RegionCache{window, hash};
    } catch (...) {
    }
}

HRGN CreateDockInputRegion(int width, int height, int cornerDiameter) {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    // Match the shared DOCK_CORNER_RADIUS_PT used by the glass shader.
    const int rounding = std::max(2, cornerDiameter);
    return CreateRoundRectRgn(0, 0, width + 1, height + 1, rounding, rounding);
}

// Shadow margin around the pill, in device px (shared DOCK_SHADOW_MARGIN_PT).
LONG DockShadowMarginPx(float scale) noexcept {
    return std::lround(DOCK_SHADOW_MARGIN_PT * scale);
}

void CompositePremul(uint8_t* dest, int destWidth, int destHeight, int destX, int destY,
    const uint8_t* source, int sourceWidth, int sourceHeight) {
    for (int y = 0; y < sourceHeight; ++y) {
        const int pixelY = destY + y;
        if (pixelY < 0 || pixelY >= destHeight) {
            continue;
        }
        for (int x = 0; x < sourceWidth; ++x) {
            const int pixelX = destX + x;
            if (pixelX < 0 || pixelX >= destWidth) {
                continue;
            }
            uint8_t* dst = dest + (static_cast<size_t>(pixelY) * destWidth + pixelX) * 4U;
            const uint8_t* src = source + (static_cast<size_t>(y) * sourceWidth + x) * 4U;
            const float srcA = static_cast<float>(src[3]) / 255.0F;
            if (srcA <= 0.0F) {
                continue;
            }
            const float inv = 1.0F - srcA;
            dst[0] = static_cast<uint8_t>(std::lround(static_cast<float>(src[0]) +
                static_cast<float>(dst[0]) * inv));
            dst[1] = static_cast<uint8_t>(std::lround(static_cast<float>(src[1]) +
                static_cast<float>(dst[1]) * inv));
            dst[2] = static_cast<uint8_t>(std::lround(static_cast<float>(src[2]) +
                static_cast<float>(dst[2]) * inv));
            dst[3] = static_cast<uint8_t>(std::lround(static_cast<float>(src[3]) +
                static_cast<float>(dst[3]) * inv));
        }
    }
}


// CompositePremul of one constant BGRA pixel over a clamped rect. Per-channel
// tables evaluate the exact CompositePremul expression once per dest value.
void CompositeConstantPremulRect(uint8_t* dest, int destWidth, int destHeight, RECT bounds,
    const uint8_t (&pixel)[4]) {
    const int left = std::max(0L, bounds.left);
    const int top = std::max(0L, bounds.top);
    const int right = std::min(static_cast<LONG>(destWidth), bounds.right);
    const int bottom = std::min(static_cast<LONG>(destHeight), bounds.bottom);
    const float srcA = static_cast<float>(pixel[3]) / 255.0F;
    if (srcA <= 0.0F || left >= right || top >= bottom) {
        return;
    }
    const float inv = 1.0F - srcA;
    uint8_t table[4][256];
    for (int channel = 0; channel < 4; ++channel) {
        for (int value = 0; value < 256; ++value) {
            table[channel][value] = static_cast<uint8_t>(std::lround(
                static_cast<float>(pixel[channel]) + static_cast<float>(value) * inv));
        }
    }
    for (int y = top; y < bottom; ++y) {
        uint8_t* dst = dest + (static_cast<size_t>(y) * destWidth + left) * 4U;
        for (int x = left; x < right; ++x, dst += 4) {
            dst[0] = table[0][dst[0]];
            dst[1] = table[1][dst[1]];
            dst[2] = table[2][dst[2]];
            dst[3] = table[3][dst[3]];
        }
    }
}

std::vector<uint8_t> ScalePremultipliedNearest(const std::vector<uint8_t>& source, int sourceWidth,
    int sourceHeight, int destWidth, int destHeight) {
    std::vector<uint8_t> dest(static_cast<size_t>(destWidth) * static_cast<size_t>(destHeight) * 4U, 0);
    if (sourceWidth <= 0 || sourceHeight <= 0 || destWidth <= 0 || destHeight <= 0 ||
        source.size() < static_cast<size_t>(sourceWidth) * static_cast<size_t>(sourceHeight) * 4U) {
        return dest;
    }
    for (int y = 0; y < destHeight; ++y) {
        const int srcY = y * sourceHeight / destHeight;
        for (int x = 0; x < destWidth; ++x) {
            const int srcX = x * sourceWidth / destWidth;
            const size_t srcOffset = (static_cast<size_t>(srcY) * static_cast<size_t>(sourceWidth) +
                static_cast<size_t>(srcX)) * 4U;
            const size_t dstOffset = (static_cast<size_t>(y) * static_cast<size_t>(destWidth) +
                static_cast<size_t>(x)) * 4U;
            dest[dstOffset] = source[srcOffset];
            dest[dstOffset + 1] = source[srcOffset + 1];
            dest[dstOffset + 2] = source[srcOffset + 2];
            dest[dstOffset + 3] = source[srcOffset + 3];
        }
    }
    return dest;
}

void BoxBlurRgb(uint8_t* pixels, int width, int height, int radius) {
    if (pixels == nullptr || width <= 0 || height <= 0 || radius <= 0) {
        return;
    }
    std::vector<uint8_t> temp(static_cast<size_t>(width) * height * 4U);
    const int diameter = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        int blue = 0;
        int green = 0;
        int red = 0;
        for (int x = -radius; x <= radius; ++x) {
            const int sample = std::clamp(x, 0, width - 1);
            const uint8_t* src = pixels + (static_cast<size_t>(y) * width + sample) * 4U;
            blue += src[0];
            green += src[1];
            red += src[2];
        }
        for (int x = 0; x < width; ++x) {
            uint8_t* dst = temp.data() + (static_cast<size_t>(y) * width + x) * 4U;
            dst[0] = static_cast<uint8_t>(blue / diameter);
            dst[1] = static_cast<uint8_t>(green / diameter);
            dst[2] = static_cast<uint8_t>(red / diameter);
            dst[3] = 255;
            const int leave = std::clamp(x - radius, 0, width - 1);
            const int enter = std::clamp(x + radius + 1, 0, width - 1);
            const uint8_t* left = pixels + (static_cast<size_t>(y) * width + leave) * 4U;
            const uint8_t* right = pixels + (static_cast<size_t>(y) * width + enter) * 4U;
            blue += right[0] - left[0];
            green += right[1] - left[1];
            red += right[2] - left[2];
        }
    }
    for (int x = 0; x < width; ++x) {
        int blue = 0;
        int green = 0;
        int red = 0;
        for (int y = -radius; y <= radius; ++y) {
            const int sample = std::clamp(y, 0, height - 1);
            const uint8_t* src = temp.data() + (static_cast<size_t>(sample) * width + x) * 4U;
            blue += src[0];
            green += src[1];
            red += src[2];
        }
        for (int y = 0; y < height; ++y) {
            uint8_t* dst = pixels + (static_cast<size_t>(y) * width + x) * 4U;
            dst[0] = static_cast<uint8_t>(blue / diameter);
            dst[1] = static_cast<uint8_t>(green / diameter);
            dst[2] = static_cast<uint8_t>(red / diameter);
            dst[3] = 255;
            const int leave = std::clamp(y - radius, 0, height - 1);
            const int enter = std::clamp(y + radius + 1, 0, height - 1);
            const uint8_t* top = temp.data() + (static_cast<size_t>(leave) * width + x) * 4U;
            const uint8_t* bottom = temp.data() + (static_cast<size_t>(enter) * width + x) * 4U;
            blue += bottom[0] - top[0];
            green += bottom[1] - top[1];
            red += bottom[2] - top[2];
        }
    }
}

void BoxBlurFloatPlane(std::vector<float>& pixels, int width, int height, int radius) {
    if (width <= 0 || height <= 0 || radius <= 0 ||
        pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(height)) {
        return;
    }
    std::vector<float> temp(static_cast<size_t>(width) * height);
    const int diameter = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        float sum = 0.0F;
        for (int x = -radius; x <= radius; ++x) {
            sum += pixels[static_cast<size_t>(y) * width + std::clamp(x, 0, width - 1)];
        }
        for (int x = 0; x < width; ++x) {
            temp[static_cast<size_t>(y) * width + x] = sum / static_cast<float>(diameter);
            sum += pixels[static_cast<size_t>(y) * width + std::clamp(x + radius + 1, 0, width - 1)] -
                pixels[static_cast<size_t>(y) * width + std::clamp(x - radius, 0, width - 1)];
        }
    }
    for (int x = 0; x < width; ++x) {
        float sum = 0.0F;
        for (int y = -radius; y <= radius; ++y) {
            sum += temp[static_cast<size_t>(std::clamp(y, 0, height - 1)) * width + x];
        }
        for (int y = 0; y < height; ++y) {
            pixels[static_cast<size_t>(y) * width + x] = sum / static_cast<float>(diameter);
            sum += temp[static_cast<size_t>(std::clamp(y + radius + 1, 0, height - 1)) * width + x] -
                temp[static_cast<size_t>(std::clamp(y - radius, 0, height - 1)) * width + x];
        }
    }
}


// Liquid-glass face for CPU popups (Quick Settings, Dock Settings, context).
// Matches GlassPS: heavy blur is done by the caller; this maps each blurred
// backdrop sample through the calibrated lift (#000->#3a, #fff->#e1) and
// finishes with the same rim polish / dither / shape alpha as before.
int PopupFrostRadiusPx(float frostAmount, float scale) noexcept
{
    // Cap radius: 3 stacked box blurs already approximate a wide Gaussian.
    // The old (2+22*frost)*scale formula hit ~36px at 150% DPI and froze the
    // UI thread when the frost slider rebaked on every mouse move.
    const float amt = std::clamp(frostAmount, 0.0F, 1.0F);
    const int radius = static_cast<int>(std::lround((1.5F + 10.0F * amt) * scale));
    return std::clamp(radius, 1, 16);
}

void ApplyLiquidGlassFace(uint8_t* pixels, int width, int height,
    const std::vector<float>& coverage, const std::vector<float>& blurredCoverage,
    float frostAmount, bool dockFace, bool lightPlate)
{
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    // CPU popup fallback only (QS / Dock Settings / context). Match GlassPS:
    // dark or light text-panel veil for QS / Dock Settings, charcoal for context.
    const float overBlack = (!dockFace ? DOCK_PANEL_FACE_OVER_BLACK
        : (lightPlate ? DOCK_TEXT_PANEL_LIGHT_OVER_BLACK : DOCK_TEXT_PANEL_FACE_OVER_BLACK)) *
        255.0F;
    const float overWhite = (!dockFace ? DOCK_PANEL_FACE_OVER_WHITE
        : (lightPlate ? DOCK_TEXT_PANEL_LIGHT_OVER_WHITE : DOCK_TEXT_PANEL_FACE_OVER_WHITE)) *
        255.0F;
    const float frost = std::clamp(
        dockFace ? (std::max)(frostAmount, DOCK_TEXT_PANEL_FROST_FLOOR) : frostAmount, 0.0F, 1.0F);
    float plateMix = frost <= 0.5F ? (0.10F + 0.90F * (frost * 2.0F)) : 1.0F;
    plateMix = (std::max)(plateMix,
        dockFace ? DOCK_TEXT_PANEL_PLATE_MIX_FLOOR : DOCK_PANEL_PLATE_MIX_FLOOR);
    const float alpha = dockFace ? DOCK_QS_SETTINGS_GLASS_ALPHA
                                : DOCK_PANEL_GLASS_ALPHA + (1.0F - DOCK_PANEL_GLASS_ALPHA) * frost;
    constexpr float kChannelK[3] = {0.99F, 0.985F, 0.98F}; // DIB B,G,R
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t flat = static_cast<size_t>(y) * static_cast<size_t>(width) + x;
            const float shape = coverage[flat];
            const float rim =
                std::clamp((blurredCoverage[flat] - shape) * 2.0F, 0.0F, 1.0F);
            const float rim4 = rim * rim * rim * rim;
            float noise = static_cast<float>(x) * 0.06711056F + static_cast<float>(y) * 0.00583715F;
            noise = noise - std::floor(noise);
            noise = 52.9829189F * noise;
            noise = (noise - std::floor(noise)) - 0.5F;
            uint8_t* pixel = pixels + flat * 4U;
            for (int channel = 0; channel < 3; ++channel) {
                const float frosted = static_cast<float>(pixel[channel]);
                // Neutral charcoal face tone map (same as GlassPS), blended by plateMix.
                const float toneMapped =
                    overBlack + (overWhite - overBlack) * (frosted / 255.0F);
                float mapped = frosted + (toneMapped - frosted) * plateMix;
                const float target = mapped * kChannelK[channel] + overWhite * 0.08F;
                float shaded = mapped + 0.22F * (target - mapped);
                shaded += overWhite * rim * 0.12F + 255.0F * rim4 * 0.18F;
                shaded = std::clamp(shaded + noise, 0.0F, 255.0F);
                pixel[channel] = static_cast<uint8_t>(std::lround(shaded * shape * alpha));
            }
            pixel[3] = static_cast<uint8_t>(std::lround(255.0F * shape * alpha));
        }
    }
}

void ApplyPopupOuterShadow(uint8_t* pixels, int width, int height, int margin, float radius,
    float scale) noexcept
{
    if (pixels == nullptr || width <= 0 || height <= 0 || margin <= 0) {
        return;
    }
    const int innerWidth = width - margin * 2;
    const int innerHeight = height - margin * 2;
    if (innerWidth <= 2 || innerHeight <= 2) {
        return;
    }
    // Softer than the old 16px/0.20 contact shade so the Quick Settings plate
    // falls off the way the mock's outer shadow does.
    const float shadowWidth = std::max(1.0F, 22.0F * scale);
    const float shadowOffset = 3.0F * scale;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float localX = static_cast<float>(x - margin) + 0.5F;
            const float localY = static_cast<float>(y - margin) + 0.5F;
            const float edge = PopupSquircleDistance(localX, localY,
                static_cast<float>(innerWidth), static_cast<float>(innerHeight), radius);
            if (edge <= 0.0F) {
                continue;
            }
            // Offset the virtual plate down, matching GlassPS's contact shade.
            const float shadowDistance = PopupSquircleDistance(localX, localY - shadowOffset,
                static_cast<float>(innerWidth), static_cast<float>(innerHeight), radius);
            if (shadowDistance >= shadowWidth) {
                continue;
            }
            const float t = 1.0F - std::clamp(std::max(shadowDistance, 0.0F) / shadowWidth,
                0.0F, 1.0F);
            const float smooth = t * t * (3.0F - 2.0F * t);
            uint8_t* pixel = pixels +
                (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4U;
            if (pixel[3] == 0) {
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                pixel[3] = static_cast<uint8_t>(std::lround(255.0F * 0.11F * smooth));
            }
        }
    }
}

HFONT CreateFlyoutFont(int pixelHeight, int weight) {
    // LOGFONT metrics only - DrawFlyoutText rasterizes via DirectWrite grayscale
    // (NATURAL_SYMMETRIC + FLAT) so GDI lfQuality is unused for flyout ink.
    return CreateFontW(-pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        DockTextFontFace());
}

// Active flyout chrome ink (mirrors AdaptiveChromeInk). Set once per menu paint.
uint8_t g_flyoutInkR = DOCK_INK_R;
uint8_t g_flyoutInkG = DOCK_INK_G;
uint8_t g_flyoutInkB = DOCK_INK_B;

void SetFlyoutChromeInk(uint8_t r, uint8_t g, uint8_t b) noexcept {
    g_flyoutInkR = r;
    g_flyoutInkG = g;
    g_flyoutInkB = b;
}

// AdaptiveChromeInk cut (dark ink over a light face) measured on the baked
// glass so glyphs follow whatever the panel actually sits over.
void SetFlyoutChromeInkForGlass(const uint8_t* pixels, int width, int height) noexcept {
    constexpr int kGrid = 8;
    constexpr float kLightFaceLuma = 0.55F;
    float lumaSum = 0.0F;
    int samples = 0;
    if (pixels != nullptr && width > 0 && height > 0) {
        for (int gy = 0; gy < kGrid; ++gy) {
            for (int gx = 0; gx < kGrid; ++gx) {
                const int x = (2 * gx + 1) * width / (2 * kGrid);
                const int y = (2 * gy + 1) * height / (2 * kGrid);
                const uint8_t* p = pixels + (static_cast<size_t>(y) * width + x) * 4U;
                if (p[3] == 0) {
                    continue;
                }
                lumaSum += (0.0722F * p[0] + 0.7152F * p[1] + 0.2126F * p[2]) / p[3];
                ++samples;
            }
        }
    }
    if (samples > 0 && lumaSum / static_cast<float>(samples) >= kLightFaceLuma) {
        SetFlyoutChromeInk(DOCK_INK_R, DOCK_INK_G, DOCK_INK_B);
    } else {
        SetFlyoutChromeInk(DOCK_CHROME_INK_R, DOCK_CHROME_INK_G, DOCK_CHROME_INK_B);
    }
}

void RemapPremulInkColor(std::vector<uint8_t>& pixels, uint8_t r, uint8_t g, uint8_t b) {
    for (size_t index = 0; index + 3 < pixels.size(); index += 4) {
        const unsigned alpha = pixels[index + 3];
        if (alpha == 0U) {
            continue;
        }
        pixels[index] = static_cast<uint8_t>((b * alpha + 127U) / 255U);
        pixels[index + 1] = static_cast<uint8_t>((g * alpha + 127U) / 255U);
        pixels[index + 2] = static_cast<uint8_t>((r * alpha + 127U) / 255U);
    }
}

float Srgb8ToLinear(uint8_t value) noexcept {
    const float c = static_cast<float>(value) / 255.0F;
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

void CoverageToPremulInk(uint8_t* pixels, size_t byteCount, uint8_t gray) {
    // Popup DIBs are BGR-ordered: byte0=B, byte1=G, byte2=R.
    // GDI writes sRGB-ish AA; convert to linear coverage before alpha so edges
    // do not look like a second dark halo over premul glass.
    for (size_t index = 0; index + 3 < byteCount; index += 4) {
        const float linear =
            Srgb8ToLinear(pixels[index]) * (19.0F / 256.0F) +
            Srgb8ToLinear(pixels[index + 1]) * (183.0F / 256.0F) +
            Srgb8ToLinear(pixels[index + 2]) * (54.0F / 256.0F);
        const unsigned coverage =
            static_cast<unsigned>(std::lround(std::clamp(linear, 0.0F, 1.0F) * 255.0F));
        const unsigned alpha = (coverage * gray + 127U) / 255U;
        pixels[index] = static_cast<uint8_t>((g_flyoutInkB * alpha + 127U) / 255U);
        pixels[index + 1] = static_cast<uint8_t>((g_flyoutInkG * alpha + 127U) / 255U);
        pixels[index + 2] = static_cast<uint8_t>((g_flyoutInkR * alpha + 127U) / 255U);
        pixels[index + 3] = static_cast<uint8_t>(alpha);
    }
}

IDWriteFactory* FlyoutDWriteFactory() {
    static IDWriteFactory* factory = nullptr;
    if (factory == nullptr) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&factory)))) {
            factory = nullptr;
        }
    }
    return factory;
}

// Stack-resident COM renderer: refcount is a no-op (DrawFlyoutText owns lifetime).
struct FlyoutTextRenderer final : IDWriteTextRenderer {
    IDWriteBitmapRenderTarget* target = nullptr;
    IDWriteRenderingParams* params = nullptr;
    COLORREF color = RGB(255, 255, 255);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
            riid == __uuidof(IDWriteTextRenderer)) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* isDisabled) override {
        if (isDisabled == nullptr) {
            return E_POINTER;
        }
        *isDisabled = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        if (transform == nullptr || target == nullptr) {
            return E_POINTER;
        }
        target->GetCurrentTransform(transform);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override {
        if (pixelsPerDip == nullptr || target == nullptr) {
            return E_POINTER;
        }
        *pixelsPerDip = target->GetPixelsPerDip();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT baselineOriginX, FLOAT baselineOriginY,
        DWRITE_MEASURING_MODE measuringMode, DWRITE_GLYPH_RUN const* glyphRun,
        DWRITE_GLYPH_RUN_DESCRIPTION const*, IUnknown*) override {
        if (target == nullptr || params == nullptr || glyphRun == nullptr) {
            return E_POINTER;
        }
        return target->DrawGlyphRun(baselineOriginX, baselineOriginY, measuringMode, glyphRun,
            params, color, nullptr);
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, DWRITE_UNDERLINE const*,
        IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const*,
        IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL,
        BOOL, IUnknown*) override {
        return S_OK;
    }
};

void LinearCoverageToPremulInk(uint8_t* pixels, int width, int height, int strideBytes,
    uint8_t gray) {
    for (int y = 0; y < height; ++y) {
        uint8_t* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(strideBytes);
        for (int x = 0; x < width; ++x) {
            uint8_t* px = row + static_cast<size_t>(x) * 4U;
            // BitmapRenderTarget is BGRX; gamma=1.0 rendering params emit linear coverage.
            const unsigned coverage =
                (static_cast<unsigned>(px[0]) * 19U +
                    static_cast<unsigned>(px[1]) * 183U +
                    static_cast<unsigned>(px[2]) * 54U) >>
                8U;
            const unsigned alpha = (coverage * gray + 127U) / 255U;
            px[0] = static_cast<uint8_t>((g_flyoutInkB * alpha + 127U) / 255U);
            px[1] = static_cast<uint8_t>((g_flyoutInkG * alpha + 127U) / 255U);
            px[2] = static_cast<uint8_t>((g_flyoutInkR * alpha + 127U) / 255U);
            px[3] = static_cast<uint8_t>(alpha);
        }
    }
}

bool DrawFlyoutTextDirectWrite(uint8_t* dest, int destWidth, int destHeight, RECT bounds,
    HFONT font, const std::wstring& text, UINT format, uint8_t gray, int underlineFrom) {
    IDWriteFactory* factory = FlyoutDWriteFactory();
    if (factory == nullptr) {
        return false;
    }

    LOGFONTW logFont{};
    if (GetObjectW(font, sizeof(logFont), &logFont) == 0) {
        return false;
    }
    const float fontEmSize = logFont.lfHeight < 0 ? static_cast<float>(-logFont.lfHeight)
                                                  : static_cast<float>(logFont.lfHeight);
    if (fontEmSize <= 0.0F) {
        return false;
    }

    const int width = std::max(1L, bounds.right - bounds.left);
    const int height = std::max(1L, bounds.bottom - bounds.top);

    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    if (logFont.lfWeight > 0) {
        weight = static_cast<DWRITE_FONT_WEIGHT>(std::clamp(logFont.lfWeight, 1L, 999L));
    }

    const wchar_t* face =
        logFont.lfFaceName[0] != L'\0' ? logFont.lfFaceName : DockTextFontFace();
    IDWriteTextFormat* textFormat = nullptr;
    if (FAILED(factory->CreateTextFormat(face, nullptr, weight,
            logFont.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, fontEmSize, L"en-us", &textFormat)) ||
        textFormat == nullptr) {
        return false;
    }

    if ((format & DT_CENTER) != 0U) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if ((format & DT_RIGHT) != 0U) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    if ((format & DT_VCENTER) != 0U) {
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else if ((format & DT_BOTTOM) != 0U) {
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    } else {
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    textFormat->SetWordWrapping(
        (format & DT_SINGLELINE) != 0U ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);

    if ((format & DT_END_ELLIPSIS) != 0U) {
        IDWriteInlineObject* ellipsis = nullptr;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(textFormat, &ellipsis)) &&
            ellipsis != nullptr) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_WORD, 0, 0};
            textFormat->SetTrimming(&trimming, ellipsis);
            ellipsis->Release();
        }
    }

    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), textFormat,
            static_cast<FLOAT>(width), static_cast<FLOAT>(height), &layout)) ||
        layout == nullptr) {
        textFormat->Release();
        return false;
    }
    if (underlineFrom >= 0 && static_cast<size_t>(underlineFrom) < text.size()) {
        const DWRITE_TEXT_RANGE range{
            static_cast<UINT32>(underlineFrom),
            static_cast<UINT32>(text.size() - static_cast<size_t>(underlineFrom))};
        layout->SetUnderline(TRUE, range);
    }

    IDWriteGdiInterop* interop = nullptr;
    if (FAILED(factory->GetGdiInterop(&interop)) || interop == nullptr) {
        layout->Release();
        textFormat->Release();
        return false;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        interop->Release();
        layout->Release();
        textFormat->Release();
        return false;
    }
    IDWriteBitmapRenderTarget* target = nullptr;
    const HRESULT targetHr =
        interop->CreateBitmapRenderTarget(screen, width, height, &target);
    ReleaseDC(nullptr, screen);
    if (FAILED(targetHr) || target == nullptr) {
        interop->Release();
        layout->Release();
        textFormat->Release();
        return false;
    }
    target->SetPixelsPerDip(1.0F);

    // gamma=1 â†’ linear coverage in RGB; clearTypeLevel=0 + FLAT â†’ grayscale AA
    // (no RGB fringes). enhancedContrast=0 avoids stem fattening that reads soft.
    IDWriteRenderingParams* params = nullptr;
    if (FAILED(factory->CreateCustomRenderingParams(1.0F, 0.0F, 0.0F, DWRITE_PIXEL_GEOMETRY_FLAT,
            DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &params)) ||
        params == nullptr) {
        target->Release();
        interop->Release();
        layout->Release();
        textFormat->Release();
        return false;
    }

    HDC memory = target->GetMemoryDC();
    RECT fill{0, 0, width, height};
    FillRect(memory, &fill, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    // Force opaque black in case FillRect left alpha=0 on the DIB.
    {
        HBITMAP dib = static_cast<HBITMAP>(GetCurrentObject(memory, OBJ_BITMAP));
        BITMAP bm{};
        if (dib != nullptr && GetObjectW(dib, sizeof(bm), &bm) != 0 && bm.bmBits != nullptr) {
            auto* bits = static_cast<uint8_t*>(bm.bmBits);
            for (int y = 0; y < height; ++y) {
                uint8_t* row =
                    bits + static_cast<size_t>(y) * static_cast<size_t>(bm.bmWidthBytes);
                for (int x = 0; x < width; ++x) {
                    uint8_t* px = row + static_cast<size_t>(x) * 4U;
                    px[0] = 0;
                    px[1] = 0;
                    px[2] = 0;
                    px[3] = 255;
                }
            }
        }
    }

    FlyoutTextRenderer renderer;
    renderer.target = target;
    renderer.params = params;
    renderer.color = RGB(255, 255, 255);
    const HRESULT drawHr = layout->Draw(nullptr, &renderer, 0.0F, 0.0F);

    bool ok = false;
    if (SUCCEEDED(drawHr)) {
        HBITMAP dib = static_cast<HBITMAP>(GetCurrentObject(memory, OBJ_BITMAP));
        BITMAP bm{};
        if (dib != nullptr && GetObjectW(dib, sizeof(bm), &bm) != 0 && bm.bmBits != nullptr &&
            bm.bmBitsPixel == 32) {
            auto* bits = static_cast<uint8_t*>(bm.bmBits);
            LinearCoverageToPremulInk(bits, width, height, bm.bmWidthBytes, gray);
            // CompositePremul expects tightly packed rows; copy if stride padded.
            if (bm.bmWidthBytes == width * 4) {
                CompositePremul(dest, destWidth, destHeight, bounds.left, bounds.top, bits, width,
                    height);
            } else {
                std::vector<uint8_t> packed(static_cast<size_t>(width) * static_cast<size_t>(height) *
                    4U);
                for (int y = 0; y < height; ++y) {
                    std::memcpy(packed.data() + static_cast<size_t>(y) * width * 4U,
                        bits + static_cast<size_t>(y) * static_cast<size_t>(bm.bmWidthBytes),
                        static_cast<size_t>(width) * 4U);
                }
                CompositePremul(dest, destWidth, destHeight, bounds.left, bounds.top, packed.data(),
                    width, height);
            }
            ok = true;
        }
    }

    params->Release();
    target->Release();
    interop->Release();
    layout->Release();
    textFormat->Release();
    return ok;
}

void DrawFlyoutTextGdiFallback(uint8_t* dest, int destWidth, int destHeight, RECT bounds,
    HFONT font, const std::wstring& text, UINT format, uint8_t gray, int underlineFrom) {
    // 1x ANTIALIASED + linear coverage (no box downsample). Used only if DWrite fails.
    const int width = std::max(1L, bounds.right - bounds.left);
    const int height = std::max(1L, bounds.bottom - bounds.top);

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return;
    }
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr) {
        return;
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = width;
    header.bV5Height = -height;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        DeleteDC(memory);
        return;
    }
    HFONT underlineFont = nullptr;
    HFONT drawFont = font;
    if (underlineFrom >= 0) {
        LOGFONTW logFont{};
        if (GetObjectW(font, sizeof(logFont), &logFont) != 0) {
            logFont.lfUnderline = TRUE;
            underlineFont = CreateFontIndirectW(&logFont);
            if (underlineFont != nullptr) {
                drawFont = underlineFont;
            }
        }
    }
    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    HGDIOBJ previousFont = SelectObject(memory, drawFont);
    {
        auto* px = static_cast<uint8_t*>(bits);
        const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
        for (size_t i = 0; i < count; ++i) {
            px[i * 4U + 0] = 0;
            px[i * 4U + 1] = 0;
            px[i * 4U + 2] = 0;
            px[i * 4U + 3] = 255;
        }
    }
    SetBkColor(memory, RGB(0, 0, 0));
    SetBkMode(memory, OPAQUE);
    SetTextColor(memory, RGB(255, 255, 255));
    RECT local{0, 0, width, height};
    DrawTextW(memory, text.c_str(), static_cast<int>(text.size()), &local, format | DT_NOPREFIX);

    auto* src = static_cast<uint8_t*>(bits);
    const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4U;
    for (size_t index = 0; index + 3 < byteCount; index += 4) {
        const float linear =
            Srgb8ToLinear(src[index]) * (19.0F / 256.0F) +
            Srgb8ToLinear(src[index + 1]) * (183.0F / 256.0F) +
            Srgb8ToLinear(src[index + 2]) * (54.0F / 256.0F);
        const unsigned coverage =
            static_cast<unsigned>(std::lround(std::clamp(linear, 0.0F, 1.0F) * 255.0F));
        const unsigned alpha = (coverage * gray + 127U) / 255U;
        src[index] = static_cast<uint8_t>((g_flyoutInkB * alpha + 127U) / 255U);
        src[index + 1] = static_cast<uint8_t>((g_flyoutInkG * alpha + 127U) / 255U);
        src[index + 2] = static_cast<uint8_t>((g_flyoutInkR * alpha + 127U) / 255U);
        src[index + 3] = static_cast<uint8_t>(alpha);
    }
    CompositePremul(dest, destWidth, destHeight, bounds.left, bounds.top, src, width, height);
    SelectObject(memory, previousFont);
    SelectObject(memory, previousBitmap);
    if (underlineFont != nullptr) {
        DeleteObject(underlineFont);
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
}

void DrawFlyoutText(uint8_t* dest, int destWidth, int destHeight, RECT bounds, HFONT font,
    const std::wstring& text, UINT format, uint8_t gray, int underlineFrom = -1) {
    if (dest == nullptr || font == nullptr || text.empty()) {
        return;
    }
    // Device-pixel DirectWrite grayscale alpha mask: sharp at HostDpi without
    // ClearType RGB fringes and without the soft 2x box-downsample of 1.1.38.
    // Flyout HWND sizes already match the layered DIB (PerMonitorV2); no DIP stretch.
    // underlineFrom is a character index; negative leaves the run plain.
    if (!DrawFlyoutTextDirectWrite(dest, destWidth, destHeight, bounds, font, text, format, gray,
            underlineFrom)) {
        DrawFlyoutTextGdiFallback(dest, destWidth, destHeight, bounds, font, text, format, gray,
            underlineFrom);
    }
}

void FillCirclePremul(uint8_t* dest, int destWidth, int destHeight, float cx, float cy,
    float radius, float alpha, bool useInk = false) {
    const float inkB = useInk ? static_cast<float>(g_flyoutInkB) / 255.0F : 1.0F;
    const float inkG = useInk ? static_cast<float>(g_flyoutInkG) / 255.0F : 1.0F;
    const float inkR = useInk ? static_cast<float>(g_flyoutInkR) / 255.0F : 1.0F;
    const int left = std::max(0, static_cast<int>(std::floor(cx - radius - 1.0F)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 1.0F)));
    const int right = std::min(destWidth, static_cast<int>(std::ceil(cx + radius + 1.0F)));
    const int bottom = std::min(destHeight, static_cast<int>(std::ceil(cy + radius + 1.0F)));
    const float aa = 1.15F;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - cx;
            const float dy = static_cast<float>(y) + 0.5F - cy;
            const float coverage =
                1.0F - std::clamp((std::sqrt(dx * dx + dy * dy) - radius) / aa + 0.5F, 0.0F, 1.0F);
            if (coverage <= 0.0F) {
                continue;
            }
            const float srcA = coverage * alpha;
            uint8_t pixel[4] = {
                static_cast<uint8_t>(std::lround(255.0F * srcA * inkB)),
                static_cast<uint8_t>(std::lround(255.0F * srcA * inkG)),
                static_cast<uint8_t>(std::lround(255.0F * srcA * inkR)),
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
            };
            CompositePremul(dest, destWidth, destHeight, x, y, pixel, 1, 1);
        }
    }
}

void FillCircleLevelPremul(uint8_t* dest, int destWidth, int destHeight, float cx, float cy,
    float radius, float level, float alpha) {
    level = std::clamp(level, 0.0F, 1.0F);
    if (level <= 0.0F || radius <= 0.0F) {
        return;
    }
    const float fillTop = cy + radius - 2.0F * radius * level;
    const int left = std::max(0, static_cast<int>(std::floor(cx - radius - 1.0F)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 1.0F)));
    const int right = std::min(destWidth, static_cast<int>(std::ceil(cx + radius + 1.0F)));
    const int bottom = std::min(destHeight, static_cast<int>(std::ceil(cy + radius + 1.0F)));
    const float aa = 1.15F;
    for (int y = top; y < bottom; ++y) {
        if (static_cast<float>(y) + 0.5F < fillTop) {
            continue;
        }
        for (int x = left; x < right; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - cx;
            const float dy = static_cast<float>(y) + 0.5F - cy;
            const float coverage =
                1.0F - std::clamp((std::sqrt(dx * dx + dy * dy) - radius) / aa + 0.5F, 0.0F, 1.0F);
            if (coverage <= 0.0F) {
                continue;
            }
            const float srcA = coverage * alpha;
            uint8_t levelPixel[4] = {
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
            };
            CompositePremul(dest, destWidth, destHeight, x, y, levelPixel, 1, 1);
        }
    }
}

void FillRectPremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds, float alpha) {
    const float a = std::clamp(alpha, 0.0F, 1.0F);
    const uint8_t pixel[4] = {
        static_cast<uint8_t>(std::lround(static_cast<float>(g_flyoutInkB) * a)),
        static_cast<uint8_t>(std::lround(static_cast<float>(g_flyoutInkG) * a)),
        static_cast<uint8_t>(std::lround(static_cast<float>(g_flyoutInkR) * a)),
        static_cast<uint8_t>(std::lround(255.0F * a)),
    };
    CompositeConstantPremulRect(dest, destWidth, destHeight, bounds, pixel);
}

void FillRectColorPremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds, float alpha,
    uint8_t blue, uint8_t green, uint8_t red) {
    const float a = std::clamp(alpha, 0.0F, 1.0F);
    const uint8_t pixel[4] = {
        static_cast<uint8_t>(std::lround(static_cast<float>(blue) * a)),
        static_cast<uint8_t>(std::lround(static_cast<float>(green) * a)),
        static_cast<uint8_t>(std::lround(static_cast<float>(red) * a)),
        static_cast<uint8_t>(std::lround(255.0F * a)),
    };
    CompositeConstantPremulRect(dest, destWidth, destHeight, bounds, pixel);
}

// Concept D Minimal Sage - panel toggles/slider (#98A869).
// Dock running-indicator dot is black in Shaders.hlsl. DIB order: B, G, R.
constexpr uint8_t kAmberB = DOCK_PANEL_TOGGLE_B;
constexpr uint8_t kAmberG = DOCK_PANEL_TOGGLE_G;
constexpr uint8_t kAmberR = DOCK_PANEL_TOGGLE_R;

void FillPillColorPremul(uint8_t* dest, int destWidth, int destHeight, float cxLeft,
    float cxRight, float cy, float radius, float alpha, uint8_t blue, uint8_t green,
    uint8_t red) {
    // Single-pass "stadium" rasterization: distance to the horizontal segment
    // [cxLeft, cxRight], composited exactly once per pixel. Drawing a pill as
    // rect + two circles instead double-paints the overlap (src-over applied
    // twice), which leaves a visible semicircle seam where each cap meets the
    // middle.
    if (cxRight < cxLeft) {
        const float swap = cxLeft;
        cxLeft = cxRight;
        cxRight = swap;
    }
    const int left = std::max(0, static_cast<int>(std::floor(cxLeft - radius - 1.0F)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 1.0F)));
    const int right = std::min(destWidth, static_cast<int>(std::ceil(cxRight + radius + 1.0F)));
    const int bottom = std::min(destHeight, static_cast<int>(std::ceil(cy + radius + 1.0F)));
    const float aa = 1.15F;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            const float py = static_cast<float>(y) + 0.5F;
            const float dx = px < cxLeft ? cxLeft - px : (px > cxRight ? px - cxRight : 0.0F);
            const float dy = py - cy;
            const float coverage =
                1.0F - std::clamp((std::sqrt(dx * dx + dy * dy) - radius) / aa + 0.5F, 0.0F, 1.0F);
            if (coverage <= 0.0F) {
                continue;
            }
            const float srcA = coverage * alpha;
            uint8_t pixel[4] = {
                static_cast<uint8_t>(std::lround(static_cast<float>(blue) * srcA)),
                static_cast<uint8_t>(std::lround(static_cast<float>(green) * srcA)),
                static_cast<uint8_t>(std::lround(static_cast<float>(red) * srcA)),
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
            };
            CompositePremul(dest, destWidth, destHeight, x, y, pixel, 1, 1);
        }
    }
}

// Same superellipse as SdSquircleBox in Shaders.hlsl (n = 4). Negative inside.
float SdSquircleBox(float px, float py, float cx, float cy, float halfW, float halfH,
    float radius) noexcept {
    const float cap = std::min(halfW, halfH);
    radius = std::clamp(radius, 0.0F, std::max(0.0F, cap));
    const float qx = std::abs(px - cx) - (halfW - radius);
    const float qy = std::abs(py - cy) - (halfH - radius);
    const float ax = std::max(qx, 0.0F);
    const float ay = std::max(qy, 0.0F);
    const float quartic = ax * ax * ax * ax + ay * ay * ay * ay;
    const float outside = std::sqrt(std::sqrt(std::max(quartic, 0.0F)));
    const float inside = std::min(std::max(qx, qy), 0.0F);
    return outside + inside - radius;
}

float SquircleCoverage(float signedDistance) noexcept {
    constexpr float aa = 1.15F;
    return 1.0F - std::clamp(signedDistance / aa + 0.5F, 0.0F, 1.0F);
}

void CompositeCoverage(uint8_t* dest, int destWidth, int destHeight, int x, int y,
    float coverage, float alpha, uint8_t blue, uint8_t green, uint8_t red) {
    if (coverage <= 0.0F || alpha <= 0.0F) {
        return;
    }
    const float srcA = coverage * alpha;
    uint8_t pixel[4] = {
        static_cast<uint8_t>(std::lround(static_cast<float>(blue) * srcA)),
        static_cast<uint8_t>(std::lround(static_cast<float>(green) * srcA)),
        static_cast<uint8_t>(std::lround(static_cast<float>(red) * srcA)),
        static_cast<uint8_t>(std::lround(255.0F * srcA)),
    };
    CompositePremul(dest, destWidth, destHeight, x, y, pixel, 1, 1);
}

// Nested panel controls. The plate uses DOCK_CORNER_RADIUS_PT (20); chips inside
// it use 12pt so corners stay in the same family without swallowing a short row.
float ContentSquircleRadius(const RECT& bounds, float scale) noexcept {
    const float w = static_cast<float>(std::max(1L, bounds.right - bounds.left));
    const float h = static_cast<float>(std::max(1L, bounds.bottom - bounds.top));
    const float cap = 0.5F * std::min(w, h);
    return std::min(cap, std::max(6.0F * scale, 12.0F * scale));
}

RECT InsetContentRect(RECT bounds, LONG inset) noexcept {
    bounds.left += inset;
    bounds.top += inset;
    bounds.right -= inset;
    bounds.bottom -= inset;
    if (bounds.right - bounds.left < 8 || bounds.bottom - bounds.top < 8) {
        bounds.left -= inset;
        bounds.top -= inset;
        bounds.right += inset;
        bounds.bottom += inset;
    }
    return bounds;
}

void FillSquircleColorPremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds,
    float radius, float alpha, uint8_t blue, uint8_t green, uint8_t red) {
    if (alpha <= 0.0F || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return;
    }
    const float halfW = 0.5F * static_cast<float>(bounds.right - bounds.left);
    const float halfH = 0.5F * static_cast<float>(bounds.bottom - bounds.top);
    const float cx = 0.5F * static_cast<float>(bounds.left + bounds.right);
    const float cy = 0.5F * static_cast<float>(bounds.top + bounds.bottom);
    const int left = std::max(0, static_cast<int>(bounds.left) - 1);
    const int top = std::max(0, static_cast<int>(bounds.top) - 1);
    const int right = std::min(destWidth, static_cast<int>(bounds.right) + 1);
    const int bottom = std::min(destHeight, static_cast<int>(bounds.bottom) + 1);
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const float dist = SdSquircleBox(static_cast<float>(x) + 0.5F,
                static_cast<float>(y) + 0.5F, cx, cy, halfW, halfH, radius);
            CompositeCoverage(dest, destWidth, destHeight, x, y, SquircleCoverage(dist), alpha,
                blue, green, red);
        }
    }
}

void FillSquirclePremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds, float radius,
    float alpha) {
    FillSquircleColorPremul(dest, destWidth, destHeight, bounds, radius, alpha, g_flyoutInkB,
        g_flyoutInkG, g_flyoutInkR);
}

void FillSquircleLevelColorPremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds,
    float radius, float level, float alpha, uint8_t blue, uint8_t green, uint8_t red) {
    level = std::clamp(level, 0.0F, 1.0F);
    if (level <= 0.0F || alpha <= 0.0F || bounds.right <= bounds.left ||
        bounds.bottom <= bounds.top) {
        return;
    }
    const float halfW = 0.5F * static_cast<float>(bounds.right - bounds.left);
    const float halfH = 0.5F * static_cast<float>(bounds.bottom - bounds.top);
    const float cx = 0.5F * static_cast<float>(bounds.left + bounds.right);
    const float cy = 0.5F * static_cast<float>(bounds.top + bounds.bottom);
    const float fillTop = static_cast<float>(bounds.bottom) -
        static_cast<float>(bounds.bottom - bounds.top) * level;
    const int left = std::max(0, static_cast<int>(bounds.left) - 1);
    const int top = std::max(0, static_cast<int>(std::floor(fillTop)));
    const int right = std::min(destWidth, static_cast<int>(bounds.right) + 1);
    const int bottom = std::min(destHeight, static_cast<int>(bounds.bottom) + 1);
    for (int y = top; y < bottom; ++y) {
        if (static_cast<float>(y) + 0.5F < fillTop) {
            continue;
        }
        for (int x = left; x < right; ++x) {
            const float dist = SdSquircleBox(static_cast<float>(x) + 0.5F,
                static_cast<float>(y) + 0.5F, cx, cy, halfW, halfH, radius);
            CompositeCoverage(dest, destWidth, destHeight, x, y, SquircleCoverage(dist), alpha,
                blue, green, red);
        }
    }
}

std::wstring NotifyIconTitle(const TrayNotifyIcon& icon) {
    if (!icon.tip.empty()) {
        const size_t cut = icon.tip.find_first_of(L"\r\n");
        std::wstring title = cut == std::wstring::npos ? icon.tip : icon.tip.substr(0, cut);
        const size_t dash = title.find(L" - ");
        if (dash != std::wstring::npos && dash > 0) {
            title.resize(dash);
        }
        return title;
    }
    std::wstring name = icon.exeName;
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        name = name.substr(slash + 1);
    }
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        name.resize(dot);
    }
    return name;
}

std::wstring NotifyIconStatus(const TrayNotifyIcon& icon) {
    std::wstring status;
    const size_t cut = icon.tip.find_first_of(L"\r\n");
    if (cut != std::wstring::npos && cut + 1 < icon.tip.size()) {
        size_t start = cut + 1;
        while (start < icon.tip.size() &&
            (icon.tip[start] == L'\n' || icon.tip[start] == L'\r' ||
                std::iswspace(icon.tip[start]) != 0)) {
            ++start;
        }
        status = icon.tip.substr(start);
    } else {
        const size_t dash = icon.tip.find(L" - ");
        if (dash != std::wstring::npos && dash + 3 < icon.tip.size()) {
            status = icon.tip.substr(dash + 3);
        }
    }
    const size_t line = status.find_first_of(L"\r\n");
    if (line != std::wstring::npos) {
        status.resize(line);
    }
    const size_t paren = status.find(L" (");
    if (paren != std::wstring::npos && paren > 0) {
        status.resize(paren);
    }
    const size_t end = status.find_last_not_of(L" \t");
    if (end == std::wstring::npos) {
        return {};
    }
    status.resize(end + 1);
    return status;
}

HWND FindNamedCoreWindow(const wchar_t* title) {
    HWND window = FindWindowW(L"Windows.UI.Core.CoreWindow", title);
    if (window != nullptr) {
        return window;
    }
    return FindWindowExW(nullptr, nullptr, L"Windows.UI.Core.CoreWindow", title);
}

bool IsVisibleUncloakedWindow(HWND window) {
    if (window == nullptr || IsWindowVisible(window) == FALSE) {
        return false;
    }
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return false;
    }
    return true;
}

bool RectsEqual(const RECT& left, const RECT& right) noexcept {
    return left.left == right.left && left.top == right.top && left.right == right.right &&
        left.bottom == right.bottom;
}

bool MonitorRect(HMONITOR monitor, RECT& bounds) noexcept {
    MONITORINFO information{sizeof(information)};
    if (monitor == nullptr || GetMonitorInfoW(monitor, &information) == FALSE) {
        return false;
    }
    bounds = information.rcMonitor;
    return true;
}

const GUID kMonitorPowerOnGuid{0x02731015, 0x4510, 0x4526,
    {0x99, 0xe6, 0xe5, 0xa1, 0x7e, 0xbd, 0x1a, 0xea}};

UINT GetTaskbarAppBarState() noexcept {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    return static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &data));
}

void SetTaskbarAppBarState(UINT state) noexcept {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    data.lParam = static_cast<LPARAM>(state);
    SHAppBarMessage(ABM_SETSTATE, &data);
}

void HideTaskbarWindow(HWND taskbar) noexcept {
    if (taskbar == nullptr || IsWindow(taskbar) == FALSE) {
        return;
    }
    SetWindowPos(taskbar, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

constexpr wchar_t kExplorerAdvancedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr wchar_t kMultiMonTaskbarValue[] = L"MMTaskbarEnabled";

bool ReadDwordValue(const wchar_t* valueName, DWORD& value) noexcept {
    DWORD size = sizeof(value);
    DWORD type = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kExplorerAdvancedKey, valueName, RRF_RT_REG_DWORD, &type,
               &value, &size) == ERROR_SUCCESS;
}

void WriteDwordValue(const wchar_t* valueName, DWORD value) noexcept {
    RegSetKeyValueW(HKEY_CURRENT_USER, kExplorerAdvancedKey, valueName, REG_DWORD, &value,
        sizeof(value));
}

BOOL CALLBACK CollectMonitorBounds(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFO information{sizeof(information)};
    if (GetMonitorInfoW(monitor, &information) == FALSE) {
        return TRUE;
    }
    auto* monitors = reinterpret_cast<std::vector<std::pair<HMONITOR, RECT>>*>(data);
    monitors->push_back({monitor, information.rcMonitor});
    return TRUE;
}

LRESULT CALLBACK FullscreenClaimProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

INPUT KeyboardInput(WORD key, DWORD flags) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = flags;
    return input;
}

bool SendWinKey() {
    const std::array inputs = {
        KeyboardInput(VK_LWIN, KEYEVENTF_EXTENDEDKEY),
        KeyboardInput(VK_LWIN, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP),
    };
    return SendInput(static_cast<UINT>(inputs.size()), const_cast<INPUT*>(inputs.data()),
        sizeof(INPUT)) == static_cast<UINT>(inputs.size());
}


bool IsWindowProcessElevated(HWND window) noexcept {
    if (window == nullptr) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        // Access denied often means a higher-integrity process when we are not elevated.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    HANDLE token = nullptr;
    bool elevated = false;
    if (OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE) {
        TOKEN_ELEVATION elevation{};
        DWORD returned = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation),
                &returned) != FALSE) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

bool GrantExplorerForeground() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD processId = 0;
    if (tray != nullptr) {
        GetWindowThreadProcessId(tray, &processId);
    }
    if (processId != 0) {
        return AllowSetForegroundWindow(processId) != FALSE;
    }
    return AllowSetForegroundWindow(ASFW_ANY) != FALSE;
}

bool IsStartLauncherVisible() {
    if (IsVisibleUncloakedWindow(FindNamedCoreWindow(L"Start"))) {
        return true;
    }
    return IsVisibleUncloakedWindow(FindWindowW(L"XamlExplorerHostIslandWindow", L"Start"));
}

BOOL CALLBACK FindNotificationCenterWindow(HWND window, LPARAM data) {
    // Win+N calendar/notifications live in ShellExperienceHost (Win10 action
    // center lineage) as a top-level Windows.UI.Core.CoreWindow; newer builds
    // may host it in Explorer instead. Titles localize, so match class +
    // owning process, never the title. UWP app windows are
    // ApplicationFrameWindow at top level, so a top-level CoreWindow is
    // effectively a shell flyout. "Windows Input Experience" is cloaked and
    // never reaches the process check below.
    if (IsVisibleUncloakedWindow(window) == false) {
        return TRUE;
    }
    wchar_t className[64]{};
    if (GetClassNameW(window, className, static_cast<int>(std::size(className))) == 0 ||
        std::wstring_view(className) != L"Windows.UI.Core.CoreWindow") {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return TRUE;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return TRUE;
    }
    wchar_t imagePath[MAX_PATH]{};
    DWORD pathSize = static_cast<DWORD>(std::size(imagePath));
    const BOOL queried = QueryFullProcessImageNameW(process, 0, imagePath, &pathSize);
    CloseHandle(process);
    if (queried == FALSE) {
        return TRUE;
    }
    std::wstring_view image(imagePath, pathSize);
    const size_t slash = image.find_last_of(L"\\/");
    const std::wstring_view fileName =
        slash == std::wstring_view::npos ? image : image.substr(slash + 1);
    // Case-insensitive compare without EqualInsensitiveWide (defined below).
    const auto matches = [](std::wstring_view name, std::wstring_view want) {
        return name.size() == want.size() &&
            std::equal(name.begin(), name.end(), want.begin(), [](wchar_t lhs, wchar_t rhs) {
                   return std::towlower(lhs) == std::towlower(rhs);
               });
    };
    if (matches(fileName, L"ShellExperienceHost.exe") || matches(fileName, L"explorer.exe")) {
        *reinterpret_cast<bool*>(data) = true;
        return FALSE;
    }
    return TRUE;
}

bool IsNotificationCenterVisible() {
    bool found = false;
    EnumWindows(&FindNotificationCenterWindow, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool IsAnyShellFlyoutVisible() {
    return IsStartLauncherVisible() || IsNotificationCenterVisible();
}

std::wstring TrimWide(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t character) {
        return std::iswspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t character) {
        return std::iswspace(character) != 0;
    }).base();
    return first >= last ? L"" : std::wstring(first, last);
}

bool EqualInsensitiveWide(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](wchar_t lhs, wchar_t rhs) {
            return std::towlower(lhs) == std::towlower(rhs);
        });
}

bool StartsWithInsensitiveWide(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
        EqualInsensitiveWide(value.substr(0, prefix.size()), prefix);
}

std::wstring StripLaunchPhrases(std::wstring text) {
    text = TrimWide(std::move(text));
    constexpr std::wstring_view prefixes[] = {
        L"open ", L"launch ", L"start ", L"switch to ", L"focus ", L"show ", L"bring up ",
    };
    bool stripped = true;
    while (stripped && !text.empty()) {
        stripped = false;
        for (const std::wstring_view prefix : prefixes) {
            if (StartsWithInsensitiveWide(text, prefix)) {
                text = TrimWide(text.substr(prefix.size()));
                stripped = true;
                break;
            }
        }
    }
    if (StartsWithInsensitiveWide(text, L"the ")) {
        text = TrimWide(text.substr(4));
    }
    return text;
}

std::wstring WindowText(HWND window) {
    if (window == nullptr) {
        return {};
    }
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(window, text.data(), length + 1);
    if (copied <= 0) {
        return {};
    }
    text.resize(static_cast<size_t>(copied));
    return text;
}

class ComApartment {
public:
    ComApartment()
        : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
    }

    ~ComApartment() {
        if (m_result == S_OK) {
            CoUninitialize();
        }
    }

private:
    HRESULT m_result = E_FAIL;
};

std::vector<uint8_t> RasterizeIconHandle(HICON icon, UINT extent) {
    if (icon == nullptr) {
        return {};
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return {};
    }
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr) {
        return {};
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(extent);
    header.bV5Height = -static_cast<LONG>(extent);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        DeleteDC(memory);
        return {};
    }
    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(memory);
        return {};
    }

    std::memset(bits, 0, static_cast<size_t>(extent) * static_cast<size_t>(extent) * sizeof(DWORD));
    DrawIconEx(memory, 0, 0, icon, static_cast<int>(extent), static_cast<int>(extent), 0, nullptr,
        DI_NORMAL);
    const size_t pixelCount = static_cast<size_t>(extent) * static_cast<size_t>(extent);
    const auto* pixels = static_cast<const DWORD*>(bits);
    std::vector<uint8_t> result(pixelCount * 4U);
    for (size_t index = 0; index < pixelCount; ++index) {
        const DWORD pixel = pixels[index];
        result[index * 4U + 0U] = static_cast<uint8_t>((pixel >> 16) & 0xffU);
        result[index * 4U + 1U] = static_cast<uint8_t>((pixel >> 8) & 0xffU);
        result[index * 4U + 2U] = static_cast<uint8_t>(pixel & 0xffU);
        result[index * 4U + 3U] = static_cast<uint8_t>((pixel >> 24) & 0xffU);
    }

    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    return result;
}

std::vector<uint8_t> RasterizeBitmapHandle(HBITMAP bitmap, UINT extent) {
    if (bitmap == nullptr) {
        return {};
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return {};
    }
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr) {
        return {};
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(extent);
    header.bV5Height = -static_cast<LONG>(extent);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    void* bits = nullptr;
    HBITMAP target = CreateDIBSection(memory, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    if (target == nullptr || bits == nullptr) {
        DeleteDC(memory);
        return {};
    }
    HGDIOBJ previousTarget = SelectObject(memory, target);
    HDC source = CreateCompatibleDC(screen);
    if (source == nullptr) {
        SelectObject(memory, previousTarget);
        DeleteObject(target);
        DeleteDC(memory);
        return {};
    }
    HGDIOBJ previousSource = SelectObject(source, bitmap);
    if (previousTarget == nullptr || previousTarget == HGDI_ERROR || previousSource == nullptr ||
        previousSource == HGDI_ERROR) {
        if (previousSource != nullptr && previousSource != HGDI_ERROR) {
            SelectObject(source, previousSource);
        }
        DeleteDC(source);
        if (previousTarget != nullptr && previousTarget != HGDI_ERROR) {
            SelectObject(memory, previousTarget);
        }
        DeleteObject(target);
        DeleteDC(memory);
        return {};
    }

    std::memset(bits, 0, static_cast<size_t>(extent) * static_cast<size_t>(extent) * sizeof(DWORD));
    BitBlt(memory, 0, 0, static_cast<int>(extent), static_cast<int>(extent), source, 0, 0,
        SRCCOPY);
    const size_t pixelCount = static_cast<size_t>(extent) * static_cast<size_t>(extent);
    const auto* pixels = static_cast<const DWORD*>(bits);
    std::vector<uint8_t> result(pixelCount * 4U);
    for (size_t index = 0; index < pixelCount; ++index) {
        const DWORD pixel = pixels[index];
        result[index * 4U + 0U] = static_cast<uint8_t>((pixel >> 16) & 0xffU);
        result[index * 4U + 1U] = static_cast<uint8_t>((pixel >> 8) & 0xffU);
        result[index * 4U + 2U] = static_cast<uint8_t>(pixel & 0xffU);
        result[index * 4U + 3U] = static_cast<uint8_t>((pixel >> 24) & 0xffU);
    }

    SelectObject(source, previousSource);
    DeleteDC(source);
    SelectObject(memory, previousTarget);
    DeleteObject(target);
    DeleteDC(memory);
    return result;
}

DWORD_PTR SafeShellFileInfo(const wchar_t* target, DWORD fileAttributes,
    SHFILEINFOW* information, UINT flags) noexcept {
    // Shell icon lookup executes third-party handler code in-process; a corrupt
    // handler must not take the dock down. SHFILEINFOW is POD, so it can cross
    // the SEH boundary safely. Overlays are deliberately never requested: the
    // overlay manager has faulted here before, and the overlay index is never
    // consumed (only hIcon is rasterized).
    __try {
        return SHGetFileInfoW(target, fileAttributes, information, sizeof(SHFILEINFOW), flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

HBITMAP SafeDragShellBitmap(const wchar_t* target, UINT extent) noexcept {
    // SEH guard: same third-party shell-handler rationale as SafeShellFileInfo.
    // POD-only so __try is legal (C2712); references leak on a fault.
    __try {
        IShellItem* item = nullptr;
        if (FAILED(SHCreateItemFromParsingName(target, nullptr, IID_PPV_ARGS(&item))) ||
            item == nullptr) {
            return nullptr;
        }
        IShellItemImageFactory* factory = nullptr;
        const HRESULT queried = item->QueryInterface(IID_PPV_ARGS(&factory));
        if (FAILED(queried) || factory == nullptr) {
            item->Release();
            return nullptr;
        }
        SIZE size{static_cast<LONG>(extent), static_cast<LONG>(extent)};
        HBITMAP bitmap = nullptr;
        const HRESULT imaged = factory->GetImage(size,
            static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK), &bitmap);
        factory->Release();
        item->Release();
        if (FAILED(imaged) || bitmap == nullptr) {
            return nullptr;
        }
        return bitmap;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::vector<uint8_t> ExtractDragIconPixels(const std::wstring& target, UINT extent) {
    ComApartment apartment;
    if (HBITMAP bitmap = SafeDragShellBitmap(target.c_str(), extent)) {
        const std::vector<uint8_t> pixels = RasterizeBitmapHandle(bitmap, extent);
        DeleteObject(bitmap);
        if (!pixels.empty()) {
            return pixels;
        }
    }

    SHFILEINFOW information{};
    if (SafeShellFileInfo(target.c_str(), FILE_ATTRIBUTE_NORMAL, &information,
            SHGFI_ICON | SHGFI_LARGEICON) != 0 &&
        information.hIcon != nullptr) {
        const std::vector<uint8_t> pixels = RasterizeIconHandle(information.hIcon, extent);
        DestroyIcon(information.hIcon);
        return pixels;
    }
    return {};
}

HBITMAP CreateDragGhostBitmapFromPixels(const std::vector<uint8_t>& pixels, UINT extent) {
    if (pixels.empty() || extent == 0) {
        return nullptr;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return nullptr;
    }
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr) {
        return nullptr;
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(extent);
    header.bV5Height = -static_cast<LONG>(extent);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        DeleteDC(memory);
        return nullptr;
    }
    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(memory);
        return nullptr;
    }

    const size_t pixelCount = pixels.size() / 4U;
    const UINT sourceExtent =
        static_cast<UINT>(std::lround(std::sqrt(static_cast<double>(pixelCount))));
    if (sourceExtent == 0 ||
        static_cast<size_t>(sourceExtent) * sourceExtent * 4U != pixels.size()) {
        SelectObject(memory, previousBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        return nullptr;
    }

    const std::vector<uint8_t> displayPixels =
        BoxDownsamplePremultiplied(pixels, sourceExtent, extent);
    const size_t bytes = static_cast<size_t>(extent) * extent * 4U;
    if (displayPixels.size() < bytes) {
        SelectObject(memory, previousBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        return nullptr;
    }
    std::memcpy(bits, displayPixels.data(), bytes);

    SelectObject(memory, previousBitmap);
    DeleteDC(memory);
    return bitmap;
}

HBITMAP CreateDragGhostBitmap(const PinnedApp& app, HWND runningWindow, UINT extent) {
    const std::vector<std::wstring> candidates =
        WindowCatalog::IconResolutionCandidates(app, runningWindow);
    std::vector<uint8_t> pixels = Renderer::ExtractIconPixels(candidates, extent);
    if (pixels.empty() && !candidates.empty()) {
        pixels = ExtractDragIconPixels(candidates.front(), extent);
    }
    if (pixels.empty()) {
        return nullptr;
    }
    return CreateDragGhostBitmapFromPixels(pixels, extent);
}

}  // namespace dock_detail
using namespace dock_detail;

// --- Trash OLE drop target (shell files -> Recycle Bin) ---------------------
// Registered on the dock input window. Only the Trash icon accepts drops;
// all other dock areas return DROPEFFECT_NONE so Explorer keeps its image.
struct TrashDropReply {
    std::wstring error;
    bool moved = false;
};

class DockAppTrashDropTarget : public IDropTarget {
public:
    explicit DockAppTrashDropTarget(DockApp* app) : m_app(app) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (count == 0) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(
        IDataObject* data, DWORD /*keys*/, POINTL pt, DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        m_accepting = data != nullptr && RecycleBin::DataObjectHasFiles(data);
        if (!m_accepting) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        const DWORD allowed = *effect;
        const POINT screen{pt.x, pt.y};
        const bool overTrash =
            m_app != nullptr && m_app->IsPointOverTrash(screen);
        if (overTrash) {
            m_app->OnTrashDragEnter();
        }
        // AND with the source's allowed mask. Prefer MOVE; accept COPY when
        // that is all the source offers (cross-volume) so the drop still fires.
        *effect = overTrash ? RecycleBin::ChooseTrashDropEffect(allowed)
                            : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD /*keys*/, POINTL pt, DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        if (!m_accepting || m_app == nullptr) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        const DWORD allowed = *effect;
        const POINT screen{pt.x, pt.y};
        const bool overTrash = m_app->IsPointOverTrash(screen);
        const DWORD chosen = overTrash ? RecycleBin::ChooseTrashDropEffect(allowed)
                                       : DROPEFFECT_NONE;
        if (chosen != DROPEFFECT_NONE) {
            m_app->OnTrashDragOver(screen);
            *effect = chosen;
        } else {
            m_app->OnTrashDragLeave();
            *effect = DROPEFFECT_NONE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        m_accepting = false;
        if (m_app != nullptr) {
            m_app->OnTrashDragLeave();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(
        IDataObject* data, DWORD /*keys*/, POINTL pt, DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        // Capture source-allowed mask before any early exit overwrites *effect.
        const DWORD allowed = *effect;
        const bool wasAccepting = m_accepting;
        m_accepting = false;
        if (!wasAccepting || m_app == nullptr || data == nullptr) {
            *effect = DROPEFFECT_NONE;
            if (m_app != nullptr) {
                m_app->OnTrashDragLeave();
            }
            return S_OK;
        }
        const POINT screen{pt.x, pt.y};
        if (!m_app->IsPointOverTrash(screen)) {
            *effect = DROPEFFECT_NONE;
            m_app->OnTrashDragLeave();
            return S_OK;
        }
        // Hold the data object through recycle + SetData (optimized-move signal).
        data->AddRef();
        std::vector<std::wstring> paths = RecycleBin::FilesFromDataObject(data);
        m_app->OnTrashDragLeave();
        if (paths.empty()) {
            *effect = DROPEFFECT_NONE;
            data->Release();
            return S_OK;
        }
        // Recycle synchronously so *pdwEffect matches the real outcome. Returning
        // MOVE before DeleteItem finished left Explorer/desktop views stale.
        const bool recycled = m_app->OnTrashDrop(paths);
        if (recycled) {
            RecycleBin::SignalOptimizedRecycle(data);
            RecycleBin::NotifyPathsDeleted(paths);
            // Success: prefer MOVE (masked). COPY-only sources get COPY; the
            // TARGETCLSID + SHChangeNotify above still clear their views.
            *effect = RecycleBin::ChooseTrashDropEffect(allowed);
        } else {
            *effect = DROPEFFECT_NONE;
        }
        data->Release();
        return S_OK;
    }

private:
    DockApp* m_app = nullptr;
    LONG m_ref = 1;
    bool m_accepting = false;
};

DockApp::DockApp(HINSTANCE instance)
    : m_instance(instance) {
    // OleInitialize (superset of CoInitializeEx STA) is required for the Trash
    // IDropTarget (RegisterDragDrop) while keeping the existing STA model.
    m_comResult = OleInitialize(nullptr);
}

DockApp::~DockApp() {
    m_bluetooth.Stop();
    RevokeTrashDropTarget();
    UnregisterTrashNotify();
    CloseLaunchPrompt(false);
    m_perfProfiler.Stop();
    DestroyDockSettings();
    DestroyOverflowPopup();
    DestroyContextMenu();
    DestroyHoverLabelWindow();
    DestroyHoverLabelFont();
    DestroyContextFonts();
    DestroyDragGhostWindow();
    StopCursorWatch();
    UnregisterForegroundWatch();
    if (m_mouseHook != nullptr) {
        UnhookWindowsHookEx(m_mouseHook);
    }
    UnregisterSystemResumeNotifications();
    RestoreTaskbar();
    if (m_singleInstanceMutex != nullptr) {
        CloseHandle(m_singleInstanceMutex);
    }
    if (m_backdropWaitable != nullptr) {
        CancelWaitableTimer(m_backdropWaitable);
        CloseHandle(m_backdropWaitable);
        m_backdropWaitable = nullptr;
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
    if (m_comResult == S_OK) {
        OleUninitialize();
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
    m_dockScale = m_config.DockScale();
    // A registered install newer than this binary means this copy is stale
    // (Setup updated the install dir while an older copy kept launching).
    // Hand off before touching taskbar/startup state; the new copy owns it.
    if (const std::optional<int> handOff = HandOffToNewerInstalledCopy()) {
        return *handOff;
    }
    m_renderer.BeginDeviceCreation();
    // Startup entries are authoritative for logon launch, but the toggle owns
    // them: repair a stale path (portable copy moved) or clear leftover
    // entries so the persisted setting and the system never disagree after
    // restart.
    Startup::SyncWithConfig(m_config.LaunchAtStartup());
    {
        std::string current = Updater::CurrentVersion();
        std::wstring wide(current.begin(), current.end());
        m_updateStatus = m_config.CheckForUpdates()
            ? L"Version " + wide + L" - checking for updates..."
            : L"Version " + wide + L" - automatic updates off.";
    }
    // Clear a fulfilled install record, or reclaim one retry when the last
    // launched install never took effect on this copy.
    ReconcileLastUpdate();
    RestoreTaskbar();
    CreateOverlayWindow();
    UpdatePrimaryMonitor();
    // Hide the native taskbar BEFORE any heavy startup work (window
    // enumeration, D3D12 device creation, shell icon extraction). Those steps
    // can take a second or more on a busy logon, and every millisecond before
    // HideTaskbar is time the old taskbar stays visible. The taskbar monitor
    // keeps it suppressed while the rest of init proceeds below.
    m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    m_openPerfMenusMessage = RegisterWindowMessageW(L"Hoverdock.OpenPerfMenus");
    m_closePerfMenusMessage = RegisterWindowMessageW(L"Hoverdock.ClosePerfMenus");
    m_openQsPageMessage = RegisterWindowMessageW(L"Hoverdock.OpenQsPage");
    m_bluetoothTestMessage = RegisterWindowMessageW(L"Hoverdock.BluetoothTest");
    RegisterSystemResumeNotifications();
    HideTaskbar();
    // Profiles first: enrichment (AppUserModelId coverage) depends on knowing
    // which pins need it before windows are enriched.
    m_windows.RebuildPinProfiles(m_config.Pins());
    static_cast<void>(m_windows.Refresh());
    static_cast<void>(m_tray.Refresh());
    m_weather.Start(m_window, kWeatherMessage);
    m_bluetooth.Start(m_window, kBluetoothMessage);
    SetTimer(m_window, kWeatherTimerId, WeatherService::kRefreshIntervalMs, nullptr);
    RebuildDisplayApps();
    RebuildLayout(false);

    // Shell icon extraction and D3D12 device creation are each a few hundred
    // ms and independent, so extract on a worker while the device comes up.
    const IconLoadRequest iconLoad = PrepareIconLoad();
    std::future<std::vector<std::vector<uint8_t>>> iconPixels =
        std::async(std::launch::async, [&iconLoad] {
            return Renderer::ExtractIconPixelBuffers(iconLoad.candidates, iconLoad.extent);
        });
    try {
        m_renderer.Initialize(m_window, m_dockWidth, m_dockHeight);
        m_rendererInitialized = true;
        ApplyIconLoad(iconLoad, iconPixels.get());
        AssignIconTextureIndices();
    } catch (...) {
        DestroyWindow(m_window);
        throw;
    }

    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &DockApp::MouseHook, m_instance, 0);
    if (m_mouseHook == nullptr) {
        Log(L"Low-level mouse hook unavailable; the dock can still be shown by moving over its window.");
    }
    RegisterTrashNotify();
    RegisterTrashDropTarget();
    EnsureTrashIcons();
    RefreshTrash(true);
    RegisterForegroundWatch();
    StartCursorWatch();

    GetCursorPos(&m_lastCursor);
    m_lastPointerSampleAt = QpcSeconds();
    HandlePointer(m_lastCursor);
    Log(L"Dock initialized.");
    // Startup transient allocations (icon extraction temps, shell queries) stay
    // committed in the CRT heap; hand fully-free pages back to the OS once.
    static_cast<void>(_heapmin());
    // Prime the DDC brightness level off-thread (monitor round-trips stall far
    // too long for the UI thread); the tile updates on arrival when it moved.
    RefreshBrightnessAsync();
    std::thread([this] {
        m_installedApps.EnsureLoaded();
    }).detach();
    // Delayed first update check (lets the dock settle), then every 6 h while
    // running. The worker skips itself when auto-checks are disabled.
    StartUpdateTimer(kUpdateInitialDelayMs);

    for (;;) {
        const bool slideAnimating = m_visibility == VisibilityState::Showing ||
            m_visibility == VisibilityState::Hiding;
        HANDLE frameWaitable = slideAnimating ? m_renderer.FrameLatencyWaitableObject() : nullptr;
        HANDLE waits[2]{};
        DWORD waitCount = 0;
        DWORD frameSlot = static_cast<DWORD>(-1);
        DWORD backdropSlot = static_cast<DWORD>(-1);
        if (frameWaitable != nullptr) {
            frameSlot = waitCount;
            waits[waitCount++] = frameWaitable;
        }
        if (m_backdropFastArmed && m_backdropWaitable != nullptr) {
            backdropSlot = waitCount;
            waits[waitCount++] = m_backdropWaitable;
        }
        const DWORD timeout = (slideAnimating || m_dragSnapAnimating) ? 16 : INFINITE;
        // WH_MOUSE_LL is delivered by a sent message, which sets QS_SENDMESSAGE.
        // That bit is in QS_ALLINPUT but not QS_INPUT, so an idle wait missed the
        // hook until LowLevelHooksTimeout and the system cursor stalled. The dock
        // stayed smooth because its 16 ms glint timer still woke this thread.
        const DWORD wakeMask = (slideAnimating || m_dragSnapAnimating)
            ? QS_ALLINPUT
            : (QS_SENDMESSAGE | QS_INPUT | QS_POSTMESSAGE | QS_TIMER | QS_PAINT);
        const DWORD wait = MsgWaitForMultipleObjectsEx(waitCount,
            waitCount == 0 ? nullptr : waits, timeout, wakeMask, MWMO_INPUTAVAILABLE);

        const bool objectSignaled = waitCount > 0 && wait >= WAIT_OBJECT_0 &&
            wait < WAIT_OBJECT_0 + waitCount;
        if (wait == WAIT_TIMEOUT ||
            (objectSignaled && frameSlot != static_cast<DWORD>(-1) &&
                wait == WAIT_OBJECT_0 + frameSlot)) {
            if (m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Hiding) {
                AdvanceAnimation();
            }
            if (m_dragSnapAnimating) {
                AdvanceDragSnapBack();
            }
        } else if (m_dragSnapAnimating && wait != WAIT_FAILED) {
            // The 8 ms backdrop timer wakes this wait before the 16 ms snap
            // timeout, so the ghost would otherwise stay at the drop point.
            AdvanceDragSnapBack();
        }
        if (objectSignaled && backdropSlot != static_cast<DWORD>(-1) &&
            wait == WAIT_OBJECT_0 + backdropSlot) {
            OnBackdropTick();
        }

        if (wait == WAIT_OBJECT_0 + waitCount || wait == WAIT_FAILED || objectSignaled ||
            wait == WAIT_TIMEOUT) {
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

LRESULT CALLBACK DockApp::HoverLabelWindowProcedure(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK DockApp::DragGhostWindowProcedure(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK DockApp::LaunchPromptWindowProcedure(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam) {
    DockApp* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<DockApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<DockApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    switch (message) {
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(window, &client);
        static HBRUSH background = CreateSolidBrush(RGB(42, 42, 46));
        FillRect(reinterpret_cast<HDC>(wParam), &client, background);
        return 1;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        const HDC context = reinterpret_cast<HDC>(wParam);
        SetBkColor(context, RGB(42, 42, 46));
        // Answer strip (child id 3): slightly cooler text so it reads as a reply.
        if (message == WM_CTLCOLOREDIT && app != nullptr &&
            reinterpret_cast<HWND>(lParam) == app->m_launchAnswer) {
            SetTextColor(context, RGB(186, 220, 255));
        } else {
            SetTextColor(context, RGB(245, 245, 247));
        }
        static HBRUSH background = CreateSolidBrush(RGB(42, 42, 46));
        return reinterpret_cast<LRESULT>(background);
    }

    case WM_ACTIVATE:
        if (app != nullptr && LOWORD(wParam) == WA_INACTIVE) {
            const HWND newlyActive = reinterpret_cast<HWND>(lParam);
            if (newlyActive != app->m_window && newlyActive != app->m_inputWindow &&
                newlyActive != app->m_launchEdit) {
                app->CloseLaunchPrompt();
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (app != nullptr && wParam == VK_ESCAPE) {
            app->CloseLaunchPrompt();
            return 0;
        }
        break;

    case WM_CLOSE:
        if (app != nullptr) {
            app->CloseLaunchPrompt();
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK DockApp::LaunchEditProcedure(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam) {
    DockApp* app = reinterpret_cast<DockApp*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
    WNDPROC previous = app == nullptr ? nullptr : app->m_launchEditPrevious;
    if (message == WM_KEYDOWN && wParam == VK_RETURN) {
        if (app != nullptr) {
            app->SubmitLaunchPrompt();
        }
        return 0;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        if (app != nullptr) {
            app->CloseLaunchPrompt();
        }
        return 0;
    }
    if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
        return 0;
    }
    if (message == WM_GETDLGCODE) {
        const LRESULT code = previous == nullptr
            ? DefWindowProcW(window, message, wParam, lParam)
            : CallWindowProcW(previous, window, message, wParam, lParam);
        return code | DLGC_WANTALLKEYS;
    }
    if (previous != nullptr) {
        return CallWindowProcW(previous, window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK DockApp::OverflowWindowProcedure(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam) {
    DockApp* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<DockApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<DockApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN: {
        if (app == nullptr) {
            break;
        }
        const POINT downPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int downHit = app->OverflowHitIndex(downPoint);
        if (downHit >= 0 && static_cast<size_t>(downHit) < app->m_overflowHits.size()) {
            const TrayFlyoutHit& slider = app->m_overflowHits[static_cast<size_t>(downHit)];
            if (slider.kind == TrayFlyoutHitKind::VolumeSlider ||
                slider.kind == TrayFlyoutHitKind::BrightnessSlider ||
                slider.kind == TrayFlyoutHitKind::CaptureGain ||
                slider.kind == TrayFlyoutHitKind::MediaSeek) {
                app->m_qsDragging = true;
                app->m_qsDragMoved = false;
                app->m_qsDragKind = slider.kind;
                app->m_qsDragBounds = slider.bounds;
                SetCapture(window);
                app->ApplyQuickSettingsSlider(slider.kind, slider.bounds, downPoint.x);
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (app == nullptr) {
            break;
        }
        if (app->m_qsDragging) {
            app->m_qsDragMoved = true;
            const POINT dragPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            app->ApplyQuickSettingsSlider(app->m_qsDragKind, app->m_qsDragBounds, dragPoint.x);
            return 0;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int hover = app->OverflowHitIndex(point);
        if (hover != app->m_overflowHover) {
            app->m_overflowHover = hover;
            app->QueueOverflowPaint(true);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        if (app == nullptr) {
            break;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (message == WM_LBUTTONUP && app->m_qsDragging) {
            const bool moved = app->m_qsDragMoved;
            const TrayFlyoutHitKind dragKind = app->m_qsDragKind;
            const RECT dragBounds = app->m_qsDragBounds;
            app->m_qsDragging = false;
            app->m_qsDragMoved = false;
            ReleaseCapture();
            if (!moved) {
                app->ApplyQuickSettingsSlider(dragKind, dragBounds, point.x);
            }
            return 0;
        }
        const int hit = app->OverflowHitIndex(point);
        if (hit >= 0 && static_cast<size_t>(hit) < app->m_overflowHits.size()) {
            app->HandleOverflowClick(app->m_overflowHits[static_cast<size_t>(hit)], message);
        }
        return 0;
    }

    case WM_KEYDOWN:
        // NOACTIVATE, so this only fires if the popup actually has focus.
        // The low-level hook covers Escape while another app is foreground.
        if (wParam == VK_ESCAPE && app != nullptr) {
            app->DismissQuickSettings();
            return 0;
        }
        break;

    case WM_MOUSELEAVE:
        // Leaving the popup only clears hover. Drill-ins resize away from the
        // cursor; closing here would hide the new page before the pointer enters.
        if (app != nullptr && app->m_overflowHover >= 0) {
            app->m_overflowHover = -1;
            app->QueueOverflowPaint(true);
        }
        return 0;

    case kQsCacheResultMessage:
        if (app != nullptr && app->IsOverflowOpen()) {
            if (ApplyQuickSettingsCacheResult(app->m_qsCache)) {
                // Home already has a presented frame (fast reopen or prewarm).
                // PaintQuickSettings is still ~95ms of squircle/text even with
                // warm glass - never spend that on a background cache fill while
                // Home is up. Subpages that list wifi/devices still need it.
                if (app->m_qsPage != QuickSettingsPage::Home) {
                    app->QueueOverflowPaint(false);
                }
            }
        }
        return 0;

    case kQsEnergyResultMessage:
        if (app != nullptr && app->IsOverflowOpen()) {
            if (ApplyEnergyAppsResult(app->m_qsCache)) {
                app->QueueOverflowPaint(false);
            }
        }
        return 0;

    case kQsTempsResultMessage:
        // Worker posted a changed CPU/GPU reading: present just the two value
        // rects over the cached underlay (no blur / layout rebuild).
        if (app != nullptr && app->IsOverflowOpen() && app->m_qsPage == QuickSettingsPage::Home) {
            if (ApplyQuickSettingsTemps(app->m_qsCache)) {
                app->m_qsTempsDirty = true;
                app->PaintOverflowLiveFast();
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == kEnergySampleTimerId && app != nullptr) {
            if (app->m_qsPage != QuickSettingsPage::Power || !app->IsOverflowOpen()) {
                KillTimer(window, kEnergySampleTimerId);
                return 0;
            }
            RequestEnergyAppsAsync(window, DockApp::kQsEnergyResultMessage);
            return 0;
        }
        if (wParam == kQsTempsTimerId && app != nullptr) {
            if (app->m_qsPage != QuickSettingsPage::Home || !app->IsOverflowOpen()) {
                KillTimer(window, kQsTempsTimerId);
                return 0;
            }
            // Keep-alive only; sensor reads stay on the temperature worker.
            RequestQuickSettingsTemps(window, DockApp::kQsTempsResultMessage);
            return 0;
        }
        if (wParam == kQsLiveTimerId && app != nullptr) {
            if (!app->IsOverflowOpen()) {
                KillTimer(window, kQsLiveTimerId);
                return 0;
            }
            const QsLiveChange change = RefreshQuickSettingsLive(app->m_qsCache);
            if (change == QsLiveChange::Labels) {
                app->QueueOverflowPaint();
            } else if (change == QsLiveChange::MetersOnly) {
                app->PaintOverflowLiveFast();
            }
            return 0;
        }
        break;

    case WM_MOUSEWHEEL: {
        if (app == nullptr) {
            break;
        }
        // Wheel routing is decided on the main thread (see
        // kOverflowWheelMessage); the hook path posts there too.
        PostMessageW(app->m_window, kOverflowWheelMessage,
            static_cast<WPARAM>(static_cast<INT_PTR>(GET_WHEEL_DELTA_WPARAM(wParam))), 0);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK DockApp::ContextWindowProcedure(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam) {
    DockApp* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<DockApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<DockApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE: {
        if (app == nullptr) {
            break;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int hover = app->ContextHitIndex(point);
        if (hover >= 0 && static_cast<size_t>(hover) < app->m_contextItems.size() &&
            app->m_contextItems[static_cast<size_t>(hover)].disabled) {
            hover = -1;
        }
        if (hover != app->m_contextHover) {
            app->m_contextHover = hover;
            app->QueueContextPaint(true);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (app == nullptr) {
            break;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int hit = app->ContextHitIndex(point);
        if (hit >= 0 && static_cast<size_t>(hit) < app->m_contextHits.size() &&
            static_cast<size_t>(hit) < app->m_contextItems.size()) {
            if (app->m_contextItems[static_cast<size_t>(hit)].disabled) {
                return 0;
            }
            const UINT command = app->m_contextHits[static_cast<size_t>(hit)].command;
            app->ExecuteContextCommand(command);
        } else {
            app->CloseContextMenu();
        }
        return 0;
    }

    case WM_RBUTTONUP:
        if (app != nullptr) {
            app->CloseContextMenu();
        }
        return 0;

    case WM_MOUSELEAVE:
        // The concept menu dismisses as soon as the cursor leaves it.
        // Clearing hover alone left the popup stranded when moving off-menu,
        // so close outright (click-outside and flyout-zone leave also close).
        if (app != nullptr) {
            app->CloseContextMenu();
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && app != nullptr) {
            app->CloseContextMenu();
            return 0;
        }
        break;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK DockApp::MouseHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && s_instance != nullptr && s_instance->m_window != nullptr) {
        const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (wParam == WM_MOUSEMOVE && !s_instance->IsDragActive() &&
            s_instance->NeedsHookPointerPost(mouse->pt)) {
            static POINT s_lastPostedCursor{std::numeric_limits<LONG>::min(),
                std::numeric_limits<LONG>::min()};
            if (mouse->pt.x != s_lastPostedCursor.x || mouse->pt.y != s_lastPostedCursor.y) {
                s_lastPostedCursor = mouse->pt;
                PostMessageW(s_instance->m_window, kPointerMessage,
                    static_cast<WPARAM>(static_cast<INT_PTR>(mouse->pt.x)),
                    static_cast<LPARAM>(mouse->pt.y));
            }
        } else if ((wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
                       wParam == WM_MBUTTONDOWN) &&
            s_instance->IsContextMenuOpen() &&
            !s_instance->IsCursorOverContextMenu(mouse->pt)) {
            // Clicking elsewhere dismisses the context menu. A right-button
            // press on another icon is allowed through so its RBUTTONUP can
            // open a fresh menu for that icon.
            s_instance->CloseContextMenu();
        } else if (wParam == WM_LBUTTONDOWN && s_instance->IsOverflowOpen() &&
            !s_instance->m_bluetooth.IsPairing() &&
            !s_instance->IsCursorOverOverflow(mouse->pt) &&
            !s_instance->IsCursorOverSettings(mouse->pt) &&
            !s_instance->IsCursorOverContextMenu(mouse->pt) &&
            s_instance->IconAtScreenPoint(mouse->pt) < 0) {
            s_instance->CloseOverflowPopup();
        } else if (wParam == WM_MOUSEWHEEL && s_instance->IsOverflowOpen() &&
            s_instance->IsCursorOverOverflow(mouse->pt) &&
            s_instance->m_overflowWindow != nullptr) {
            // Wheel events go to the focus window, not the cursor window, so a
            // NOACTIVATE popup would never see them. Intercept here instead.
            POINT client = mouse->pt;
            if (ScreenToClient(s_instance->m_overflowWindow, &client) != FALSE) {
                const int hit = s_instance->OverflowHitIndex(client);
                if (hit >= 0 &&
                    static_cast<size_t>(hit) < s_instance->m_overflowHits.size() &&
                    (s_instance->m_overflowHits[static_cast<size_t>(hit)].kind ==
                            TrayFlyoutHitKind::Sound ||
                        s_instance->m_overflowHits[static_cast<size_t>(hit)].kind ==
                            TrayFlyoutHitKind::VolumeSlider ||
                        s_instance->m_overflowHits[static_cast<size_t>(hit)].kind ==
                            TrayFlyoutHitKind::Brightness ||
                        s_instance->m_overflowHits[static_cast<size_t>(hit)].kind ==
                            TrayFlyoutHitKind::BrightnessSlider ||
                        s_instance->m_overflowHits[static_cast<size_t>(hit)].kind ==
                            TrayFlyoutHitKind::CaptureGain)) {
                    PostMessageW(s_instance->m_window, kOverflowWheelMessage,
                        static_cast<WPARAM>(static_cast<INT_PTR>(
                            GET_WHEEL_DELTA_WPARAM(mouse->mouseData))),
                        0);
                    return 1;
                }
            }
        } else if (wParam == WM_LBUTTONUP && s_instance->IsDragActive() &&
            s_instance->m_inputWindow != nullptr) {
            PostMessageW(s_instance->m_inputWindow, WM_LBUTTONUP, 0,
                MAKELPARAM(mouse->pt.x, mouse->pt.y));
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}


LRESULT CALLBACK DockApp::OverflowDismissHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && s_instance != nullptr &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (key != nullptr && key->vkCode == VK_ESCAPE &&
            (s_instance->IsContextMenuOpen() || s_instance->IsDockSettingsOpen() ||
                s_instance->IsOverflowOpen())) {
            PostMessageW(s_instance->m_window, kOverflowDismissMessage, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void DockApp::InstallOverflowDismissHook() noexcept {
    if (m_overflowDismissHook != nullptr || m_instance == nullptr) {
        return;
    }
    m_overflowDismissHook = SetWindowsHookExW(WH_KEYBOARD_LL, &DockApp::OverflowDismissHook,
        m_instance, 0);
}

void DockApp::RemoveOverflowDismissHook() noexcept {
    if (m_overflowDismissHook == nullptr) {
        return;
    }
    UnhookWindowsHookEx(m_overflowDismissHook);
    m_overflowDismissHook = nullptr;
}

void DockApp::DismissQuickSettings() {
    if (IsContextMenuOpen()) {
        CloseContextMenu();
        return;
    }
    if (IsDockSettingsOpen()) {
        CloseDockSettings();
        return;
    }
    if (IsOverflowOpen()) {
        CloseOverflowPopup();
    }
}

BOOL CALLBACK DockApp::FindTaskbarWindow(HWND window, LPARAM data) {
    wchar_t className[64]{};
    if (GetClassNameW(window, className, static_cast<int>(std::size(className))) == 0) {
        return TRUE;
    }

    // Compare in place. A temporary wstring here is allocated once per top-level
    // window on every taskbar-monitor pass (hidden or visible) and was a steady
    // source of soft faults while the dock was resting.
    if (lstrcmpW(className, L"Shell_TrayWnd") != 0 &&
        lstrcmpW(className, L"Shell_SecondaryTrayWnd") != 0) {
        return TRUE;
    }
    auto* taskbars = reinterpret_cast<std::vector<HWND>*>(data);
    taskbars->push_back(window);
    return TRUE;
}

LRESULT DockApp::HandleRendererMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (m_openPerfMenusMessage != 0 && message == m_openPerfMenusMessage) {
        // Hidden agent/perf hook: open Quick Settings + Dock Settings so live
        // glass CPU can be sampled without synthetic mouse hit-testing.
        // Stagger Dock Settings by 300 ms so the two full panel bakes do not
        // land in the same 200 ms CPU sample (each alone stays near budget).
        if (m_visibility != VisibilityState::Visible) {
            BeginShow();
        }
        if (!IsOverflowOpen()) {
            BeginOverflowShow();
        }
        if (!IsDockSettingsOpen()) {
            SetTimer(m_window, kPerfOpenSettingsTimerId, 300, nullptr);
        }
        return 0;
    }
    if (m_closePerfMenusMessage != 0 && message == m_closePerfMenusMessage) {
        CloseDockSettings();
        if (IsOverflowOpen()) {
            BeginOverflowHide(false);
        }
        return 0;
    }
    if (m_openQsPageMessage != 0 && message == m_openQsPageMessage) {
        // Hidden agent hook: open Quick Settings (and optionally a drill-in)
        // without synthetic mouse input, for screenshots / profile runs.
        if (m_visibility != VisibilityState::Visible) {
            BeginShow();
        }
        if (!IsOverflowOpen()) {
            BeginOverflowShow();
        }
        if (IsOverflowOpen()) {
            if (wParam == 1) {
                OpenQuickSettingsPage(QuickSettingsPage::Bluetooth);
            } else if (wParam == 0 && m_qsPage != QuickSettingsPage::Home) {
                m_qsReturn = QuickSettingsPage::Home;
                CloseQuickSettingsPage();
            }
        }
        return 0;
    }
    if (m_bluetoothTestMessage != 0 && message == m_bluetoothTestMessage) {
        // Dry-run harness only: drives the same HandleOverflowClick path a
        // real click takes, but BluetoothService simulates every mutating
        // job (radio / connect / disconnect / pair) instead of touching
        // hardware. Ignored entirely unless HOVERDOCK_BT_DRYRUN=1.
        if (!BluetoothService::DryRun() || !IsOverflowOpen()) {
            return 0;
        }
        TrayFlyoutHit hit;
        hit.index = static_cast<int>(lParam);
        switch (wParam) {
        case 1:
            hit.kind = TrayFlyoutHitKind::BluetoothRadio;
            break;
        case 2:
            hit.kind = TrayFlyoutHitKind::BluetoothConnect;
            break;
        case 3:
            hit.kind = TrayFlyoutHitKind::BluetoothDiscover;
            break;
        case 4:
            hit.kind = TrayFlyoutHitKind::BluetoothPair;
            break;
        default:
            return 0;
        }
        Log(L"BluetoothTest hook kind=" + std::to_wstring(wParam) + L" index=" +
            std::to_wstring(static_cast<long long>(lParam)));
        HandleOverflowClick(hit, WM_LBUTTONUP);
        return 0;
    }

    switch (message) {
    case WM_NCHITTEST: {
        if (m_visibility != VisibilityState::Showing && m_visibility != VisibilityState::Visible) {
            return HTTRANSPARENT;
        }
        const POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (m_scalingDivider || DividerAtScreenPoint(cursor) >= 0) {
            return HTCLIENT;
        }
        return HTTRANSPARENT;
    }

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETCURSOR:
        if (m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Visible) {
            POINT cursor{};
            GetCursorPos(&cursor);
            if (m_scalingDivider || DividerAtScreenPoint(cursor) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDOWN: {
        const POINT point = ScreenPointFromClient(window, lParam);
        if (DividerAtScreenPoint(point) >= 0) {
            ClearPressState();
            m_scalingDivider = true;
            m_hoveredDivider = m_dividerIndex;
            m_scaleDragStartY = point.y;
            m_scaleDragStartValue = m_dockScale;
            SetCapture(m_inputWindow != nullptr ? m_inputWindow : window);
            RenderFrame();
            return 0;
        }
        break;
    }

    case WM_DPICHANGED:
        UpdatePrimaryMonitor();
        RebuildLayout(true);
        HideTaskbar();
        return 0;

    case WM_DISPLAYCHANGE:
        UpdatePrimaryMonitor();
        RebuildLayout(true);
        HideTaskbar();
        return 0;

    case WM_POWERBROADCAST:
        return HandlePowerBroadcast(wParam, lParam);

    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_UNLOCK || wParam == WTS_CONSOLE_CONNECT) {
            HideTaskbar();
        }
        return 0;

    case WM_ENDSESSION:
        if (wParam != FALSE) {
            RestoreTaskbar();
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (IsContextMenuOpen()) {
                CloseContextMenu();
            } else if (IsDockSettingsOpen()) {
                CloseDockSettings();
            } else if (IsOverflowOpen()) {
                CloseOverflowPopup();
            } else {
                BeginHide();
            }
            return 0;
        }
        if (wParam == VK_F12) {
            m_config.SetShowDevBounds(!m_config.ShowDevBounds());
            m_config.Save();
            RenderFrame();
            return 0;
        }
        break;

    case kCursorWatchSyncMessage: {
        // Foreground just changed. Drop the elevation cache before the cadence
        // sync so Task Manager still promotes the 33 ms watch on this message.
        m_elevatedForegroundValid = false;
        EnsureMouseHook();
        SyncCursorWatchInterval();
        // Foreground changes (e.g. the notification panel closing) must
        // resample even when Hidden: with a healthy hook the cursor timer is
        // off and the hook only posts on movement, so a stationary hot-zone
        // cursor would otherwise never reveal until the next wiggle.
        if (!IsDragActive()) {
            POINT cursor{};
            if (GetCursorPos(&cursor) != FALSE) {
                HandlePointer(cursor);
            }
        }
        // App launch / focus while the dock is tucked away: refresh the running
        // set now so RebuildLayout lands before the next reveal (no icon flash).
        if (m_visibility == VisibilityState::Hidden) {
            ScheduleDeferredRefresh();
        }
        return 0;
    }
    case kPointerMessage: {
        POINT point{static_cast<LONG>(static_cast<INT_PTR>(wParam)),
            static_cast<LONG>(lParam)};
        point = CoalescePostedPointers(window, kPointerMessage, point);
        if (IsDragActive() && HasPostedMessage(m_inputWindow, WM_LBUTTONUP)) {
            return 0;
        }
        if (!NeedsHookPointerPost(point)) {
            return 0;
        }
        HandlePointer(point);
        return 0;
    }

    case kRenderMessage:
        m_renderQueued = false;
        {
            // A coalesced frame may mix input-driven and backdrop-timer requests.
            // Bias toward non-blocking so the 120Hz backdrop timer can never stall
            // this thread (which also services the low-level mouse hook) on vsync.
            const bool allowBlocking = m_renderAllowBlockingGpuWait;
            m_renderAllowBlockingGpuWait = true;
            if (m_visibility != VisibilityState::Hidden && !IsAnimating()) {
                FinishDropPresent(RenderFrame(allowBlocking && !m_dropPresentPending));
            }
        }
        return 0;

    case kBeginShowDeferredMessage: {
        // Deferred half of BeginShow: full tray refresh (COM/IPC + forced layout)
        // runs after the first animation tick has positioned and painted, so the
        // reveal itself is just the two ShowWindow calls. Stale-show guard: a
        // hide flicker between PostMessage and dispatch must not refresh a
        // hidden dock.
        if (m_visibility != VisibilityState::Showing &&
            m_visibility != VisibilityState::Visible) {
            return 0;
        }
        if (static_cast<UINT>(wParam) != m_showSessionId) {
            return 0;
        }
        // Tray timer is stopped while hidden; pull a fresh clock/battery now that
        // the first frame is on its way instead of before it.
        RefreshTray(true);
        // Warm Quick Settings HWND/glass/Home frame off the chevron click path.
        PrewarmOverflowPopup();
        return 0;
    }

    case kLayoutApplyMessage:
        if (m_layoutApplyPending) {
            m_layoutApplyPending = false;
            UpdateInputRegion();
            PositionOverlayWindows();
            if (m_rendererInitialized) {
                m_renderer.Resize(m_dockWidth, m_dockHeight);
            }
        }
        if (m_trayIconsApplyPending) {
            m_trayIconsApplyPending = false;
            EnsureTrayIcons();
            QueueRenderFrame();
        }
        return 0;

    case kDeferredClickMessage: {
        const POINT point{static_cast<LONG>(static_cast<SHORT>(LOWORD(lParam))),
            static_cast<LONG>(static_cast<SHORT>(HIWORD(lParam)))};
        if (m_dragSnapAnimating) {
            return 0;
        }
        const int branch = static_cast<int>(wParam);
        if (branch == 0) {
            ScheduleConfigSave();
            ReloadIconsIfExtentChanged();
            if (m_visibility == VisibilityState::Visible ||
                m_visibility == VisibilityState::Showing) {
                static_cast<void>(CaptureLiveBackdrop());
            }
            QueueRenderFrame();
        } else if (branch == 1) {
            FinishDrag(point);
        } else {
            ActivatePressedApp();
        }
        if (GetCapture() == m_inputWindow) {
            ReleaseCapture();
        }
        if (!m_dragSnapAnimating) {
            ClearPressState();
        }
        if (branch != 1) {
            QueueRenderFrame();
        }
        return 0;
    }

    case kRefreshApplyMessage:
        ApplyBackgroundRefresh(static_cast<UINT>(wParam));
        return 0;

    case kPinIconMessage:
        ApplyPinIcons(static_cast<UINT>(wParam));
        return 0;

        case kOverflowPaintMessage:
        m_overflowPaintQueued = false;
        if (IsOverflowOpen()) {
            const bool hoverOnly = m_overflowHoverPaintOnly;
            m_overflowHoverPaintOnly = false;
            if (hoverOnly && !m_overflowBaseBits.empty() &&
                m_overflowBaseBits.size() == m_overflowPresentBits.size() &&
                m_overflowPresentSize.cx == m_overflowSize.cx &&
                m_overflowPresentSize.cy == m_overflowSize.cy) {
                PaintOverflowHoverFast();
            } else {
                PaintOverflowPopup();
            }
        }
        return 0;

    case kOpenDockSettingsMessage:
        // Deferred from ToggleDockSettings (QS cog) so the click/hook path
        // never sync-bakes Dock Settings.
        if (!IsDockSettingsOpen()) {
            OpenDockSettings();
        }
        return 0;

    case kSettingsPaintMessage:
        m_settingsPaintQueued = false;
        // Guarded: a paint queued before the panel closed must not re-show it.
        if (IsDockSettingsOpen()) {
            const bool hoverOnly = m_settingsHoverPaintOnly;
            m_settingsHoverPaintOnly = false;
            if (hoverOnly && !m_settingsBaseBits.empty() &&
                m_settingsBaseBits.size() == m_settingsPresentBits.size() &&
                m_settingsPresentSize.cx == m_settingsSize.cx &&
                m_settingsPresentSize.cy == m_settingsSize.cy) {
                PaintSettingsHoverFast();
            } else {
                PaintSettingsPopup();
            }
        }
        return 0;

    case kTrashNotifyMessage: {
        // SHCNRF_NewDelivery: lock, discard pidls, unlock, then refresh.
        if (wParam != 0 && lParam != 0) {
            PIDLIST_ABSOLUTE* pidls = nullptr;
            LONG eventId = 0;
            HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wParam),
                static_cast<DWORD>(lParam), &pidls, &eventId);
            if (lock != nullptr) {
                SHChangeNotification_Unlock(lock);
            }
        }
        PostMessageW(m_window, kTrashRefreshMessage, 0, 0);
        return 0;
    }

    case kTrashRefreshMessage:
        RefreshTrash(false);
        return 0;

    case kTrashDropMessage: {
        auto* reply = reinterpret_cast<TrashDropReply*>(lParam);
        if (reply == nullptr) {
            m_trashDropInFlight = false;
            RefreshTrash(true);
            return 0;
        }
        HandleTrashDropResult(reply->error, reply->moved);
        delete reply;
        return 0;
    }

    case kContextPaintMessage:
        m_contextPaintQueued = false;
        // Guarded: a paint queued before the menu closed must not re-show it.
        if (IsContextMenuOpen()) {
            const bool hoverOnly = m_contextHoverPaintOnly;
            m_contextHoverPaintOnly = false;
            if (hoverOnly && !m_contextBaseBits.empty() &&
                m_contextBaseBits.size() == m_contextPresentBits.size() &&
                m_contextPresentSize.cx == m_contextSize.cx &&
                m_contextPresentSize.cy == m_contextSize.cy) {
                PaintContextHoverFast();
            } else {
                PaintContextMenu();
            }
        }
        return 0;

    case kOverflowDismissMessage:
        DismissQuickSettings();
        return 0;

    case kOverflowWheelMessage: {
        // Scroll wheel over the volume/brightness circles adjusts that level.
        // Other positions ignore the wheel so underlying apps keep scrolling.
        if (!IsOverflowOpen()) {
            return 0;
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        if (!IsCursorOverOverflow(cursor)) {
            return 0;
        }
        POINT client = cursor;
        if (ScreenToClient(m_overflowWindow, &client) == FALSE) {
            return 0;
        }
        const int hit = OverflowHitIndex(client);
        if (hit < 0 || static_cast<size_t>(hit) >= m_overflowHits.size()) {
            return 0;
        }
        const TrayFlyoutHitKind kind = m_overflowHits[static_cast<size_t>(hit)].kind;
        if (kind == TrayFlyoutHitKind::CaptureGain) {
            m_flyoutWheelAccum += static_cast<int>(static_cast<INT_PTR>(wParam));
            while (m_flyoutWheelAccum >= WHEEL_DELTA) {
                m_flyoutWheelAccum -= WHEEL_DELTA;
                AdjustCaptureGain(0.05F);
            }
            while (m_flyoutWheelAccum <= -WHEEL_DELTA) {
                m_flyoutWheelAccum += WHEEL_DELTA;
                AdjustCaptureGain(-0.05F);
            }
            return 0;
        }
        const bool volumeHit = kind == TrayFlyoutHitKind::Sound ||
            kind == TrayFlyoutHitKind::VolumeSlider;
        const bool brightnessHit = kind == TrayFlyoutHitKind::Brightness ||
            kind == TrayFlyoutHitKind::BrightnessSlider;
        if (!volumeHit && !brightnessHit) {
            return 0;
        }
        if (volumeHit) {
            m_flyoutWheelAccum += static_cast<int>(static_cast<INT_PTR>(wParam));
            bool adjusted = false;
            while (m_flyoutWheelAccum >= WHEEL_DELTA) {
                m_flyoutWheelAccum -= WHEEL_DELTA;
                if (m_tray.AdjustVolume(0.05F)) {
                    adjusted = true;
                }
            }
            while (m_flyoutWheelAccum <= -WHEEL_DELTA) {
                m_flyoutWheelAccum += WHEEL_DELTA;
                if (m_tray.AdjustVolume(-0.05F)) {
                    adjusted = true;
                }
            }
            if (adjusted) {
                EnsureTrayIcons();
                QueueOverflowPaint();
                QueueRenderFrame();
            }
            return 0;
        }
        ScrollBrightness(static_cast<int>(static_cast<INT_PTR>(wParam)));
        return 0;
    }

    case kLaunchResultMessage: {
        std::unique_ptr<LaunchReply> reply(reinterpret_cast<LaunchReply*>(lParam));
        if (reply != nullptr) {
            Log(L"Search route=" + reply->route + L" worker_ms=" + std::to_wstring(reply->workerMs));
            if (!reply->targets.empty()) {
                m_launchTargets = std::move(reply->targets);
            }
            if (reply->agentMode) {
                ApplyAgentResult(reply->generation, reply->agent);
            } else {
                ApplyLaunchJudgment(reply->generation, reply->judgment);
            }
        }
        return 0;
    }

    case kAgentStatusMessage: {
        std::unique_ptr<AgentStatusReply> reply(reinterpret_cast<AgentStatusReply*>(lParam));
        if (reply != nullptr && reply->generation == m_launchGeneration.load() && m_launchInFlight) {
            SetLaunchPromptStatus(reply->text);
        }
        return 0;
    }

    case kBoostResultMessage: {
        std::unique_ptr<BoostReply> reply(reinterpret_cast<BoostReply*>(lParam));
        if (reply != nullptr) {
            ApplyBoostResult(reply->status, reply->finished);
        }
        return 0;
    }

    case kUpdateResultMessage: {
        std::unique_ptr<UpdateReply> reply(reinterpret_cast<UpdateReply*>(lParam));
        if (reply != nullptr) {
            ApplyUpdateResult(*reply);
        }
        return 0;
    }

    case kWeatherMessage:
        OnWeatherUpdated();
        return 0;

    case kBluetoothMessage:
        ApplyBluetoothSnapshot();
        return 0;

    case kOpenStartMenuMessage:
        // Legacy async entry: OpenStartMenuFromDock now sends synchronously.
        // Keep the handler as a no-broadcast fallback so any in-flight message
        // still just delivers the keystroke instead of racing Explorer with
        // work-area/registry changes.
        if (!m_shellFlyoutHold) {
            return 0;
        }
        EnsureTaskbarList();
        if (m_taskbarList2 != nullptr) {
            for (FullscreenClaim& claim : m_fullscreenClaims) {
                if (claim.window != nullptr) {
                    m_taskbarList2->MarkFullscreenWindow(claim.window, FALSE);
                }
            }
        }
        static_cast<void>(GrantExplorerForeground());
        if (!SendWinKey()) {
            ReleaseShellFlyoutHold();
            Log(L"Start menu did not accept input.");
        }
        return 0;

    case WM_TIMER:
        if (wParam == kRefreshTimerId) {
            RefreshRunningWindows();
        } else if (wParam == kDeferredRefreshTimerId) {
            CancelDeferredRefresh();
            if (m_visibility == VisibilityState::Visible ||
                m_visibility == VisibilityState::Hidden) {
                RefreshRunningWindows(true);
            }
        } else if (wParam == kConfigSaveTimerId) {
            KillTimer(window, kConfigSaveTimerId);
            // Disk write off the UI/hook thread so pin/unpin never stalls the cursor.
            const DockConfig snapshot = m_config;
            std::thread([snapshot]() {
                static_cast<void>(snapshot.Save());
            }).detach();
        } else if (wParam == kCursorWatchTimerId) {
            PumpCursorWatch();
        } else if (wParam == kGlintTimerId) {
            TickGlint();
        } else if (wParam == kBackdropTimerId) {
            OnBackdropTick();
        } else if (wParam == kPerfOpenSettingsTimerId) {
            KillTimer(window, kPerfOpenSettingsTimerId);
            if (!IsDockSettingsOpen()) {
                OpenDockSettings();
            }
        } else if (wParam == kTaskbarMonitorTimerId) {
            // Adaptive cadence: poll fast while suppression is actively fighting
            // Explorer, then back off 10x when steady. Suppression latency in the
            // worst steady case is one slow tick; TaskbarCreated/display-change
            // events still re-hide immediately via HideTaskbar().
            if (MaintainNativeTaskbarSuppression()) {
                m_taskbarMonitorQuietPasses = 0;
                if (!m_taskbarMonitorFast) {
                    m_taskbarMonitorFast = true;
                    SetTimer(window, kTaskbarMonitorTimerId, kTaskbarMonitorIntervalMs, nullptr);
                }
            } else if (++m_taskbarMonitorQuietPasses >= kTaskbarMonitorCalmPasses &&
                m_taskbarMonitorFast) {
                m_taskbarMonitorFast = false;
                SetTimer(window, kTaskbarMonitorTimerId, kTaskbarMonitorSlowIntervalMs, nullptr);
            }
            // Hook reinstall + cursor-watch cadence sync. When Hidden with a healthy
            // hook the cursor timer is off (0 Hz); elevated FG / missing hook re-arms
            // 33 ms. Foreground WinEvent also posts kCursorWatchSyncMessage for lag-free
            // promote (Task Manager UIPI case) without a standing poll.
            EnsureMouseHook();
            SyncCursorWatchInterval();
        } else if (wParam == kTrayTimerId) {
            RefreshTray(false);
            // Re-arm: overflow wants 1 Hz IPC; dock-face clock has no seconds.
            StartTrayTimer();
        } else if (wParam == kWeatherTimerId) {
            m_weather.RequestRefresh();
        } else if (wParam == kBluetoothTimerId) {
            // Paired list is only on the Bluetooth QS page; skip CollectPaired
            // polling elsewhere. While pointer is hot, defer refresh; otherwise
            // 12s idle / 4s after recent BT UI interaction.
            if (IsOverflowOpen() && m_qsPage == QuickSettingsPage::Bluetooth &&
                !m_bluetoothSnapshot.discovering && !m_bluetooth.IsPairing()) {
                constexpr double kBluetoothHotPointerSec = 0.35;
                constexpr double kBluetoothIdleRefreshSec = 12.0;
                constexpr double kBluetoothActiveRefreshSec = 4.0;
                constexpr double kBluetoothUiHotSec = 8.0;
                const double now = QpcSeconds();
                const bool pointerHot = m_lastPointerMotionAt > 0.0 &&
                    (now - m_lastPointerMotionAt) < kBluetoothHotPointerSec;
                const bool btUiHot = m_lastBluetoothUiAt > 0.0 &&
                    (now - m_lastBluetoothUiAt) < kBluetoothUiHotSec;
                const double minInterval =
                    btUiHot ? kBluetoothActiveRefreshSec : kBluetoothIdleRefreshSec;
                if (!pointerHot &&
                    (m_lastBluetoothRefreshAt <= 0.0 ||
                        (now - m_lastBluetoothRefreshAt) >= minInterval)) {
                    m_bluetooth.RequestRefresh(false);
                    m_lastBluetoothRefreshAt = now;
                }
            }
        } else if (wParam == kUpdateTimerId) {
            // First tick is the delayed startup check; re-arm for the steady
            // 6 h cadence afterwards.
            KillTimer(window, kUpdateTimerId);
            SetTimer(window, kUpdateTimerId, kUpdateIntervalMs, nullptr);
            if (m_config.CheckForUpdates() && !m_updateInFlight.load() &&
                !m_updateInstalling.load()) {
                CheckForUpdatesAsync(false);
            } else {
                m_lastUpdateCheck = QpcSeconds();
            }
        }
        return 0;

    case WM_CLOSE:
        if (DestroyWindow(window) == FALSE) {
            Log(L"Could not destroy the renderer window.");
        }
        return 0;

    case WM_DESTROY:
        KillTimer(window, kWeatherTimerId);
        KillTimer(window, kBluetoothTimerId);
        m_weather.Stop();
        m_bluetooth.Stop();
        StopTaskbarMonitor();
        StopUpdateTimer();
        UnregisterSystemResumeNotifications();
        StopShellFlyoutWatch();
        CloseLaunchPrompt(false);
        m_perfProfiler.Stop();
        DestroyDockSettings();
        BeginOverflowHide(false);
        DestroyOverflowPopup();
        DestroyContextMenu();
        HideHoverLabel();
        DestroyHoverLabelWindow();
        ClearPressState();
        if (m_inputWindow != nullptr && DestroyWindow(m_inputWindow) == FALSE) {
            Log(L"Could not destroy the dock input window.");
        }
        m_inputWindow = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        if (m_taskbarCreatedMessage != 0 && message == m_taskbarCreatedMessage) {
            HideTaskbar();
            return 0;
        }
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
            POINT cursor{};
            GetCursorPos(&cursor);
            if (m_scalingDivider || DividerAtScreenPoint(cursor) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
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
        ClearPressState();
        // Dismiss the tooltip on press, not just on the deferred click: the
        // Quick Settings popup captures its glass right after, and a still-
        // visible bubble would bake into (or show through) that glass.
        HideHoverLabel();
        const int divider = DividerAtScreenPoint(point);
        if (divider >= 0) {
            m_scalingDivider = true;
            m_hoveredDivider = divider;
            m_scaleDragStartY = point.y;
            m_scaleDragStartValue = m_dockScale;
            SetCapture(window);
            RenderFrame();
            return 0;
        }
        m_pressedIcon = IconAtScreenPoint(point);
        LogInputMouse(message, point, m_pressedIcon);
        if (m_pressedIcon >= 0) {
            m_pressedAt = point;
            m_pressedAtTime = QpcSeconds();
            m_lastPointerSampleAt = m_pressedAtTime;
            m_suppressDragUntilRelease = false;
            m_pressedTarget =
                static_cast<size_t>(m_pressedIcon) < m_displayApps.size()
                    ? m_displayApps[static_cast<size_t>(m_pressedIcon)].app.target
                    : std::wstring{};
            m_draggedTarget.clear();
            SetCapture(window);
            RenderFrame();
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT point = CoalescePostedClientMoves(window, ScreenPointFromClient(window, lParam));
        if (m_dragSnapAnimating) {
            return 0;
        }
        if (HasPostedMessage(window, WM_LBUTTONUP)) {
            return 0;
        }
        if (m_scalingDivider) {
            UpdateDividerScaleDrag(point);
            return 0;
        }
        if (m_draggedIcon >= 0) {
            UpdateDrag(point);
            return 0;
        }
        HandlePointer(point);
        const double nowMove = QpcSeconds();
        if (m_pressedIcon >= 0 && m_lastPointerSampleAt > 0.0 &&
            nowMove - m_lastPointerSampleAt > kMaxPointerStallSeconds) {
            ClearPressState();
            m_suppressDragUntilRelease = true;
        } else {
            m_lastPointerSampleAt = nowMove;
        }
        if (!m_suppressDragUntilRelease && !m_launchClickInProgress && m_pressedIcon >= 0 &&
            IsPersistentDisplayIcon(m_pressedIcon) &&
            (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 &&
            (nowMove - m_pressedAtTime) >= kMinDragPressSeconds &&
            HasCrossedDragThreshold(point)) {
            BeginDrag(point);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        ProfileScope scope("WM_LBUTTONUP");
        POINT point{};
        GetCursorPos(&point);
        LogInputMouse(message, point, IconAtScreenPoint(point));
        if (m_dragSnapAnimating) {
            return 0;
        }
        // Input-critical work only: the actions below (app Focus/launch via
        // shell IPC, drag-finish, scale-save + live capture, sync render with
        // GPU waits) measured up to 72ms. Snapshot the branch + UP point and
        // defer past this scope so the hook thread stays under 1ms. Same-thread
        // FIFO preserves ordering; the point travels in lParam because the
        // cursor may move before dispatch.
        const WPARAM branch = m_scalingDivider ? 0 : (m_draggedIcon >= 0 ? 1 : 2);
        PostMessageW(m_window, kDeferredClickMessage, branch, MAKELPARAM(point.x, point.y));
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (!IsDragActive()) {
            ClearPressState();
            if (QpcSeconds() >= m_suppressBackdropUntil) {
                QueueRenderFrame();
            }
        }
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
        ClearPressState();
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
    constexpr DWORD rendererExtendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
        WS_EX_NOREDIRECTIONBITMAP;
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
    EnsureWindowCapturable(m_window);
    EnsureWindowCapturable(m_inputWindow);
    CreateHoverLabelWindow();
}

void DockApp::CreateHoverLabelWindow() {
    const wchar_t hoverLabelClassName[] = L"LiquidGlassDockHoverLabel";
    WNDCLASSEXW hoverLabelClass{sizeof(hoverLabelClass)};
    hoverLabelClass.lpfnWndProc = &DockApp::HoverLabelWindowProcedure;
    hoverLabelClass.hInstance = m_instance;
    hoverLabelClass.lpszClassName = hoverLabelClassName;
    if (RegisterClassExW(&hoverLabelClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log(L"Could not register the hover label window class.");
        return;
    }

    constexpr DWORD extendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
        WS_EX_TRANSPARENT;
    m_hoverLabelWindow = CreateWindowExW(extendedStyle, hoverLabelClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, m_instance, nullptr);
    if (m_hoverLabelWindow == nullptr) {
        Log(L"Could not create the hover label window.");
    }
}

void DockApp::DestroyHoverLabelFont() noexcept {
    if (m_hoverLabelFont != nullptr) {
        DeleteObject(m_hoverLabelFont);
        m_hoverLabelFont = nullptr;
        m_hoverLabelFontDpi = 0;
        m_hoverLabelFontPx = 0;
    }
}

HFONT DockApp::HoverLabelFont() {
    const UINT dpi = m_window != nullptr ? GetDpiForWindow(m_window) : 96U;
    const float scale =
        static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F * std::max(0.75F, m_dockScale);
    // macOS dock labels are a small semibold system face (~16px at 96 DPI).
    const int pixelHeight = std::max(14, static_cast<int>(std::lround(16.0F * scale)));
    if (m_hoverLabelFont != nullptr && dpi == m_hoverLabelFontDpi &&
        pixelHeight == m_hoverLabelFontPx) {
        return m_hoverLabelFont;
    }

    DestroyHoverLabelFont();
    m_hoverLabelFont = CreateFontW(-pixelHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, DockTextFontFace());
    m_hoverLabelFontDpi = dpi;
    m_hoverLabelFontPx = pixelHeight;
    return m_hoverLabelFont == nullptr
        ? static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))
        : m_hoverLabelFont;
}

void DockApp::DestroyHoverLabelWindow() {
    if (m_hoverLabelWindow == nullptr) {
        return;
    }

    if (DestroyWindow(m_hoverLabelWindow) == FALSE) {
        Log(L"Could not destroy the hover label window.");
    }
    m_hoverLabelWindow = nullptr;
}

void DockApp::RebuildLayout(bool reloadIcons) {
    ProfileScope scope("RebuildLayout");
    const UINT previousWidth = m_dockWidth;
    const UINT previousHeight = m_dockHeight;
    const UINT dpi = HostDpi();
    const float scale = static_cast<float>(dpi) / 96.0F;
    const float layoutScale = scale * m_dockScale;
    const LONG iconSize = std::lround(56.0F * layoutScale);
    const LONG dotSize = std::max(3L, std::lround(6.0F * layoutScale));
    const LONG dotGap = std::max(3L, std::lround(6.0F * layoutScale));
    const LONG iconSlotHeight = iconSize + dotGap + dotSize;
    // Horizontal inset stays roomy. The glyph is centered: the margin above it
    // equals the dot gap, the activity dot, and the floor under it.
    const LONG padding = std::lround(18.0F * layoutScale);
    const LONG padBottom = std::lround(6.0F * layoutScale);
    const LONG padTop = dotGap + dotSize + padBottom;
    const LONG gap = std::lround(8.0F * layoutScale);
    const LONG dividerSlotWidth = std::lround(10.0F * layoutScale);
    const LONG margin = std::lround(10.0F * scale);
    const LONG trayGlyph = std::max(22L, std::lround(28.0F * layoutScale));
    const LONG trayGap = std::lround(10.0F * layoutScale);
    const LONG trayDividerWidth = dividerSlotWidth;
    const LONG trayLeadGap = std::lround(10.0F * layoutScale);
    const LONG trayClockGap = std::lround(16.0F * layoutScale);
    const SIZE clockSize = m_tray.MeasureClock(layoutScale);
    const LONG atlasExtent = std::min(256L, std::max(1L, static_cast<LONG>(IconPixelExtent()) * 2L));
    const LONG clockWidth = std::min(atlasExtent,
        std::max(clockSize.cx, std::lround(80.0F * layoutScale)));
    const LONG clockHeight = std::min(atlasExtent,
        std::min(iconSlotHeight, std::max(clockSize.cy, std::lround(40.0F * layoutScale))));
    const size_t displayCount = m_displayApps.size();
    constexpr TraySlot kTrayGlyphSlots[] = {
        TraySlot::Overflow, TraySlot::Power};

    // Trash sits immediately left of the Quick Settings caret (Overflow) at
    // full dock app-icon size (not tray-glyph metrics).
    LONG trayWidth = trayDividerWidth + trayLeadGap + iconSize + trayGap;
    LONG visibleGlyphs = 0;
    for (const TraySlot slot : kTrayGlyphSlots) {
        if (!SystemTray::SlotVisible(slot, m_tray.Status())) {
            continue;
        }
        if (visibleGlyphs > 0) {
            trayWidth += trayGap;
        }
        trayWidth += trayGlyph;
        ++visibleGlyphs;
    }
    // Weather glyph uses full pin metrics (iconSize) and shares the pin baseline
    // (top), while still sitting beside the clock chronologically.
    const LONG weatherSize = iconSize;
    const LONG weatherGap = std::max(4L, std::lround(6.0F * layoutScale));
    const LONG weatherReserve = weatherSize + weatherGap;
    trayWidth += trayClockGap + weatherReserve + clockWidth;

    LONG contentWidth = 0;
    for (size_t index = 0; index < displayCount; ++index) {
        if (IsLayoutOnlyTarget(m_displayApps[index].app.target)) {
            contentWidth += dividerSlotWidth;
        } else {
            contentWidth += iconSize;
        }
        if (index + 1 < displayCount) {
            contentWidth += gap;
        }
    }
    if (displayCount > 0) {
        contentWidth += gap;
    }
    contentWidth += trayWidth;

    // The window carries a shadow margin around the pill (shared
    // DOCK_SHADOW_MARGIN_PT): icons stay pill-relative, the shader draws the
    // drop shade into the margin ring.
    const LONG shadowMargin = DockShadowMarginPx(scale);
    m_dockWidth = static_cast<UINT>(padding * 2 + contentWidth + shadowMargin * 2);
    m_dockHeight = static_cast<UINT>(padTop + padBottom + iconSlotHeight + shadowMargin * 2);
    // The window carries a shadow ring around the pill; anchoring the window
    // bottom to the screen bottom would leave the pill floating a full
    // shadowMargin above the edge. Sink the window so only a small breathing
    // gap remains between the pill and the screen edge (the bottom shadow
    // clips off-screen, as with a native taskbar).
    const LONG bottomGap = std::lround(4.0F * scale);
    m_visibleY =
        m_hostBounds.bottom - static_cast<LONG>(m_dockHeight) + shadowMargin - bottomGap;
    m_hiddenY = m_visibleY + static_cast<LONG>(m_dockHeight) + margin;
    if (m_visibility == VisibilityState::Hidden) {
        m_currentY = m_hiddenY;
    } else if (m_visibility == VisibilityState::Visible || m_scalingDivider) {
        m_currentY = m_visibleY;
    } else if (m_visibility == VisibilityState::Showing) {
        m_animationToY = m_visibleY;
    } else if (m_visibility == VisibilityState::Hiding) {
        m_animationToY = m_hiddenY;
    }
    m_windowX = m_hostBounds.left +
        ((m_hostBounds.right - m_hostBounds.left) - static_cast<LONG>(m_dockWidth)) / 2;

    m_iconRenderData.clear();
    m_trashIndex = -1;
    m_iconRenderData.reserve(displayCount + 8U);
    const LONG top = shadowMargin + padTop;
    LONG left = padding + shadowMargin;
    for (size_t index = 0; index < displayCount; ++index) {
        DockIconRenderData data;
        if (IsLayoutOnlyTarget(m_displayApps[index].app.target)) {
            data.bounds = {left, top, left + dividerSlotWidth, top + iconSize};
            data.kind = DockIconKind::Divider;
            data.adaptiveInk = true;
            left += dividerSlotWidth + gap;
        } else {
            data.bounds = {left, top, left + iconSize, top + iconSlotHeight};
            data.running = m_displayApps[index].runningWindow != nullptr;
            // Start carries the Windows 11 logo artwork (blue gradient, no halo)
            // and Search its own two-tone artwork (charcoal fill + white halo),
            // so they must not be remapped to flat adaptive ink.
            data.adaptiveInk = false;
            left += iconSize + gap;
        }
        m_iconRenderData.push_back(data);
    }

    DockIconRenderData trayDivider;
    trayDivider.kind = DockIconKind::TrayDivider;
    trayDivider.adaptiveInk = true;
    trayDivider.bounds = {left, top, left + trayDividerWidth, top + iconSize};
    m_iconRenderData.push_back(trayDivider);
    left += trayDividerWidth + trayLeadGap;

    const LONG trayTop = top + (iconSize - trayGlyph) / 2;
    // Trash / Recycle Bin: full dock icon cell, immediately left of Quick Settings.
    {
        DockIconRenderData trash;
        trash.kind = DockIconKind::Trash;
        trash.adaptiveInk = false;
        trash.bounds = {left, top, left + iconSize, top + iconSlotHeight};
        m_trashIndex = static_cast<int>(m_iconRenderData.size());
        m_iconRenderData.push_back(trash);
        left += iconSize + trayGap;
    }
    for (const TraySlot slot : kTrayGlyphSlots) {
        if (!SystemTray::SlotVisible(slot, m_tray.Status())) {
            continue;
        }
        DockIconRenderData data;
        data.kind = DockIconKind::Tray;
        data.traySlot = slot;
        data.adaptiveInk = true;
        data.bounds = {left, trayTop, left + trayGlyph, trayTop + trayGlyph};
        m_iconRenderData.push_back(data);
        left += trayGlyph + trayGap;
    }
    left += trayClockGap - trayGap;
    const LONG clockTop = top + (iconSlotHeight - clockHeight) / 2;
    {
        // Always reserve the slot so the clock does not jump when the first
        // Open-Meteo fetch lands; RasterizeIcon falls back to cloudy.
        // Full pin size at the same top/baseline as dock app icons (not the
        // smaller time-row alignment from 1.1.33).
        const LONG weatherTop = top;
        DockIconRenderData weather;
        weather.kind = DockIconKind::Weather;
        weather.adaptiveInk = false;
        weather.bounds = {left, weatherTop, left + weatherSize, weatherTop + weatherSize};
        m_iconRenderData.push_back(weather);
        left += weatherSize + weatherGap;
    }
    DockIconRenderData clock;
    clock.kind = DockIconKind::Clock;
    clock.traySlot = TraySlot::Clock;
    clock.adaptiveInk = true;
    clock.bounds = {left, clockTop, left + clockWidth, clockTop + clockHeight};
    m_iconRenderData.push_back(clock);

    CacheLayoutSlotBounds();

    const bool sizeChanged = previousWidth != m_dockWidth || previousHeight != m_dockHeight;
    m_hoverLabelIcon = -1;
    if (sizeChanged) {
        if (m_window != nullptr) {
            // GPU-backed apply (region + reposition + swapchain Resize with its
            // unbounded Flush wait) leaves via kLayoutApplyMessage so this
            // function stays under 1ms. Posts coalesce: rapid bursts
            // (scale-drag) set the flag repeatedly and the handler applies the
            // latest dims once.
            m_layoutApplyPending = true;
            PostMessageW(m_window, kLayoutApplyMessage, 0, 0);
        } else {
            UpdateInputRegion();
            PositionOverlayWindows();
            if (m_rendererInitialized) {
                m_renderer.Resize(m_dockWidth, m_dockHeight);
            }
        }
    }
    if (reloadIcons && m_rendererInitialized) {
        LoadIconTextures();
    }
    AssignIconTextureIndices();
    if (TrayIconsNeedApply()) {
        // Glyph raster + atlas re-upload (GPU waits) leaves via
        // kLayoutApplyMessage; the key check itself is microseconds.
        m_trayIconsApplyPending = true;
        if (m_window != nullptr) {
            PostMessageW(m_window, kLayoutApplyMessage, 0, 0);
        }
    }
    if (IsOverflowOpen()) {
        PositionOverflowPopup();
    }
}

void DockApp::UpdateInputRegion() {
    if (m_inputWindow == nullptr) {
        return;
    }

    const int width = static_cast<int>(m_dockWidth);
    const int height = static_cast<int>(m_dockHeight);
    // Hit-testing matches the shared DOCK_CORNER_RADIUS_PT (diameter = 2 x radius).
    // The region covers the pill only: inset by the shadow margin so clicks
    // fall through the drop shade to the desktop.
    const float scale = static_cast<float>(HostDpi()) / 96.0F;
    const int shadowMargin = static_cast<int>(DockShadowMarginPx(scale));
    HRGN region = CreateDockInputRegion(width - shadowMargin * 2, height - shadowMargin * 2,
        static_cast<int>(std::lround(2.0F * DOCK_CORNER_RADIUS_PT * scale)));
    if (region == nullptr) {
        Log(L"Could not create the dock input region.");
        return;
    }
    OffsetRgn(region, shadowMargin, shadowMargin);
    if (SetWindowRgn(m_inputWindow, region, FALSE) == 0) {
        DeleteObject(region);
        Log(L"Could not apply the dock input region.");
    }
}

void DockApp::PositionOverlayWindows() {
    ProfileScope scope("PositionOverlayWindows");
    // Change-cache: animation ticks and layout callers invoke this far more
    // often than the rect actually moves. Skipping the DWM round-trips when
    // nothing moved drops steady calls to microseconds; the hover/popup tail
    // below still runs every time.
    const LONG width = static_cast<LONG>(m_dockWidth);
    const LONG height = static_cast<LONG>(m_dockHeight);
    // Desktop capture omits a WS_EX_NOREDIRECTIONBITMAP window for as long as
    // its HWND is moving. Keep the renderer parked over the whole slide and
    // move only the composition visual. The input window still tracks the pill.
    const bool sliding = m_visibility == VisibilityState::Showing ||
        m_visibility == VisibilityState::Hiding;
    LONG renderY = m_currentY;
    LONG renderH = height;
    float contentOffsetY = 0.0F;
    if (sliding) {
        const LONG top = std::min(m_visibleY, m_hiddenY);
        const LONG bottom = std::max(m_visibleY, m_hiddenY) + height;
        renderY = top;
        renderH = std::max(height, bottom - top);
        contentOffsetY = static_cast<float>(m_currentY - top);
    }
    const bool renderMoved = !m_overlayPosValid || m_overlayX != m_windowX ||
        m_overlayW != width || m_overlayRenderY != renderY || m_overlayRenderH != renderH;
    const bool inputMoved = !m_overlayPosValid || m_inputWindow == nullptr ||
        m_overlayX != m_windowX || m_overlayY != m_currentY || m_overlayW != width ||
        m_overlayH != height;
    if (renderMoved || inputMoved) {
        // One DWM round-trip for both windows instead of two: DeferWindowPos
        // batches the topmost renderer + layered input moves atomically.
        // NOREDRAW + NOCOPYBITS: frames come from the D3D present, so GDI
        // invalidation and bit-shifting on move are pure overhead.
        constexpr UINT kMoveFlags =
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW | SWP_NOCOPYBITS;
        ProfileScope moveScope("PositionOverlayWindows::Move");
        bool positioned = false;
        // Input HWND last + HWND_TOPMOST so it sits above the DComp renderer.
        // OLE WindowFromPoint then resolves to the same window that owns mouse
        // input and RegisterDragDrop (divider handling already lives on input).
        if (HDWP batch = BeginDeferWindowPos(m_inputWindow != nullptr ? 2 : 1)) {
            if (DeferWindowPos(batch, m_window, HWND_TOPMOST, m_windowX, renderY, width,
                    renderH, kMoveFlags) != nullptr &&
                (m_inputWindow == nullptr ||
                    DeferWindowPos(batch, m_inputWindow, HWND_TOPMOST, m_windowX, m_currentY,
                        width, height, kMoveFlags) != nullptr) &&
                EndDeferWindowPos(batch) != FALSE) {
                positioned = true;
            }
        }
        if (!positioned) {
            // Batch unavailable: fall back to direct moves (previous behavior).
            if (SetWindowPos(m_window, HWND_TOPMOST, m_windowX, renderY, width, renderH,
                    kMoveFlags) == FALSE) {
                Log(L"Could not position the renderer window.");
            } else if (m_inputWindow != nullptr &&
                SetWindowPos(m_inputWindow, HWND_TOPMOST, m_windowX, m_currentY, width, height,
                    kMoveFlags) == FALSE) {
                Log(L"Could not position the dock input window.");
            }
        }
        m_overlayX = m_windowX;
        m_overlayY = m_currentY;
        m_overlayW = width;
        m_overlayH = height;
        m_overlayRenderY = renderY;
        m_overlayRenderH = renderH;
        m_overlayPosValid = true;
    }
    if (m_rendererInitialized) {
        m_renderer.SetContentOffsetY(contentOffsetY);
    }
    if (IsDragActive()) {
        BringDragGhostToFront();
    } else {
        UpdateHoverLabel();
    }
    PositionLaunchPrompt();
    PositionOverflowPopup();
    PositionDockSettings();
    PositionContextMenu();
}

struct HoverBubbleLayout {
    LONG width = 0;
    LONG body = 0;
    LONG tail = 0;
    LONG tailBase = 0;
    LONG height = 0;
    LONG shadow = 0;
    LONG padX = 0;
};

HoverBubbleLayout LayoutHoverBubble(LONG textWidth, float scale) {
    const LONG padX = GreaterOf(8L, static_cast<LONG>(std::lround(8.0F * scale)));
    const LONG maxWidth = GreaterOf(260L, static_cast<LONG>(std::lround(300.0F * scale)));
    HoverBubbleLayout layout;
    // Shorter than a circle, with a flat top and bottom, and a small sharp tail.
    layout.body = GreaterOf(28L, static_cast<LONG>(std::lround(32.0F * scale)));
    layout.tail = GreaterOf(6L, static_cast<LONG>(std::lround(8.0F * scale)));
    layout.tailBase = GreaterOf(12L, static_cast<LONG>(std::lround(16.0F * scale)));
    layout.shadow = GreaterOf(6L, static_cast<LONG>(std::lround(8.0F * scale)));
    layout.padX = padX;
    // Short names stay a pill. Wider than this, the side gap is padX.
    const LONG minWidth = layout.body * 2L;
    LONG width = textWidth + padX * 2L;
    if (width < minWidth) {
        width = minWidth;
    }
    if (width > maxWidth) {
        width = maxWidth;
    }
    layout.width = width;
    layout.height = layout.body + layout.tail;
    return layout;
}

// Coverage of a rounded bar with a small downward triangle on its flat floor.
float HoverBubbleCoverage(float x, float y, float width, float bodyH, float tailH, float tailBase) {
    const float cx = width * 0.5F;
    const float radius = std::min(bodyH * 0.42F, width * 0.5F);
    const float dx = std::fabs(x - cx) - std::max(0.0F, width * 0.5F - radius);
    const float dy = std::fabs(y - bodyH * 0.5F) - std::max(0.0F, bodyH * 0.5F - radius);
    const float ax = std::max(dx, 0.0F);
    const float ay = std::max(dy, 0.0F);
    const float pill = std::sqrt(ax * ax + ay * ay) + std::min(std::max(dx, dy), 0.0F) - radius;

    float tail = 1.0e6F;
    const float baseY = bodyH - 1.5F;
    const float tipY = bodyH + tailH;
    if (y >= baseY && y <= tipY && tipY > baseY) {
        const float v = (y - baseY) / (tipY - baseY);
        tail = std::fabs(x - cx) - tailBase * 0.5F * (1.0F - v);
    } else if (y > tipY) {
        tail = y - tipY;
    }
    return std::clamp(0.5F - std::min(pill, tail), 0.0F, 1.0F);
}

void DockApp::UpdateHoverLabel(bool forceGlassRefresh) {
    if (IsDragActive() || m_draggedIcon >= 0 || m_hoveredDivider >= 0 || IsOverflowOpen() ||
        IsContextMenuOpen()) {
        HideHoverLabel();
        return;
    }

    if (m_hoverLabelWindow == nullptr || m_visibility != VisibilityState::Visible ||
        m_hoveredIcon < 0 || static_cast<size_t>(m_hoveredIcon) >= m_iconRenderData.size()) {
        HideHoverLabel();
        return;
    }

    if (m_hoveredIcon == m_hoverLabelIcon && !forceGlassRefresh) {
        // Label sits above the dock; dock HWND_TOPMOST bumps can still sink it.
        // Re-assert topmost without re-baking glass.
        if (m_hoverLabelWindow != nullptr && IsWindowVisible(m_hoverLabelWindow) != FALSE) {
            SetWindowPos(m_hoverLabelWindow, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_NOREDRAW);
        }
        return;
    }

    std::wstring text;
    if (IsTrashRenderIndex(m_hoveredIcon)) {
        text = TrashHoverText();
    } else if (IsWeatherRenderIndex(m_hoveredIcon)) {
        const WeatherService::Snapshot snap = m_weather.GetSnapshot();
        text = snap.tip.empty() ? L"Weather" : snap.tip;
    } else if (IsTrayRenderIndex(m_hoveredIcon)) {
        text = SystemTray::LabelForSlot(m_iconRenderData[static_cast<size_t>(m_hoveredIcon)].traySlot,
            m_tray.Status());
    } else if (static_cast<size_t>(m_hoveredIcon) < m_displayApps.size()) {
        text = WindowCatalog::DisplayNameForApp(
            m_displayApps[static_cast<size_t>(m_hoveredIcon)].app,
            m_displayApps[static_cast<size_t>(m_hoveredIcon)].runningWindow);
    }
    if (text.empty() || text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        HideHoverLabel();
        return;
    }

    const UINT dpi = GetDpiForWindow(m_window);
    const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;

    POINT iconTopLeft{m_iconRenderData[static_cast<size_t>(m_hoveredIcon)].bounds.left,
        m_iconRenderData[static_cast<size_t>(m_hoveredIcon)].bounds.top};
    POINT iconBottomRight{m_iconRenderData[static_cast<size_t>(m_hoveredIcon)].bounds.right,
        m_iconRenderData[static_cast<size_t>(m_hoveredIcon)].bounds.bottom};
    if (ClientToScreen(m_window, &iconTopLeft) == FALSE ||
        ClientToScreen(m_window, &iconBottomRight) == FALSE) {
        HideHoverLabel();
        return;
    }
    RECT dockScreen{};
    if (GetWindowRect(m_window, &dockScreen) == FALSE) {
        HideHoverLabel();
        return;
    }
    // Speech bubble against the glass top (window top + shadow margin), not the
    // HWND top — otherwise labels float in the shadow band.
    const LONG gap = GreaterOf(4L, static_cast<LONG>(std::lround(8.0F * scale)));
    const LONG shadowMargin = DockShadowMarginPx(scale);
    const LONG glassTop = dockScreen.top + shadowMargin;
    LONG textWidth = 0;
    {
        HFONT font = HoverLabelFont();
        HDC measureDc = GetDC(nullptr);
        if (measureDc != nullptr) {
            HDC memory = CreateCompatibleDC(measureDc);
            if (memory != nullptr) {
                HGDIOBJ prev = SelectObject(memory, font);
                SIZE textSize{};
                if (GetTextExtentPoint32W(memory, text.c_str(), static_cast<int>(text.size()),
                        &textSize) != FALSE) {
                    textWidth = textSize.cx;
                }
                if (prev != nullptr && prev != HGDI_ERROR) {
                    SelectObject(memory, prev);
                }
                DeleteDC(memory);
            }
            ReleaseDC(nullptr, measureDc);
        }
    }
    const HoverBubbleLayout bubble = LayoutHoverBubble(textWidth, scale);
    const LONG outerWidth = bubble.width + bubble.shadow * 2L;
    const LONG outerHeight = bubble.height + bubble.shadow * 2L;
    POINT destination{
        iconTopLeft.x + (iconBottomRight.x - iconTopLeft.x) / 2L - outerWidth / 2L,
        glassTop - outerHeight + bubble.shadow - gap};

    // Hide before bake on icon change so the previous bubble is not captured.
    // Glint refresh keeps the window up to avoid flicker.
    if (!forceGlassRefresh && m_hoverLabelWindow != nullptr) {
        ShowWindow(m_hoverLabelWindow, SW_HIDE);
    }

    SIZE labelSize{};
    std::vector<uint8_t> rasterized;
    if (!RasterizeHoverLabel(text, scale, destination, labelSize, rasterized)) {
        HideHoverLabel();
        return;
    }

    const size_t byteCount =
        static_cast<size_t>(labelSize.cx) * static_cast<size_t>(labelSize.cy) * 4U;
    if (rasterized.size() != byteCount || labelSize.cx <= 0 || labelSize.cy <= 0) {
        HideHoverLabel();
        return;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        HideHoverLabel();
        return;
    }
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = labelSize.cx;
    header.bV5Height = -labelSize.cy;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    void* dibBits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (bitmap == nullptr || dibBits == nullptr) {
        ReleaseDC(nullptr, screen);
        HideHoverLabel();
        return;
    }
    std::memcpy(dibBits, rasterized.data(), byteCount);
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr) {
        DeleteObject(bitmap);
        HideHoverLabel();
        return;
    }
    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(memory);
        HideHoverLabel();
        return;
    }
    POINT source{0L, 0L};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL updated = UpdateLayeredWindow(m_hoverLabelWindow, nullptr, &destination, &labelSize,
        memory, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    if (updated == FALSE) {
        Log(L"Could not update the hover label window.");
        HideHoverLabel();
        return;
    }
    if (SetWindowPos(m_hoverLabelWindow, HWND_TOPMOST, SaturatedInt(destination.x),
            SaturatedInt(destination.y), SaturatedInt(labelSize.cx), SaturatedInt(labelSize.cy),
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW) == FALSE) {
        Log(L"Could not position the hover label window.");
    }
    m_hoverLabelIcon = m_hoveredIcon;
}

bool DockApp::RasterizeHoverLabel(const std::wstring& text, float scale, POINT screenOrigin,
    SIZE& labelSize, std::vector<uint8_t>& bits) {
    HFONT font = HoverLabelFont();
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return false;
    }
    HDC memory = CreateCompatibleDC(screen);
    if (memory == nullptr) {
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ previousFont = SelectObject(memory, font);
    if (previousFont == nullptr || previousFont == HGDI_ERROR) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }

    SIZE textSize{};
    const int textLength = static_cast<int>(text.size());
    if (GetTextExtentPoint32W(memory, text.c_str(), textLength, &textSize) == FALSE) {
        SelectObject(memory, previousFont);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }
    SelectObject(memory, previousFont);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    const HoverBubbleLayout bubble = LayoutHoverBubble(textSize.cx, scale);
    labelSize = {bubble.width + bubble.shadow * 2L, bubble.height + bubble.shadow * 2L};
    const size_t pixelCount =
        static_cast<size_t>(labelSize.cx) * static_cast<size_t>(labelSize.cy);
    const size_t byteCount = pixelCount * 4U;
    bits.assign(byteCount, 0);

    // Same GlassPS stack as the dock bar (LABEL_FACE), milked +50% for glyphs.
    const RECT screenRect{screenOrigin.x, screenOrigin.y, screenOrigin.x + labelSize.cx,
        screenOrigin.y + labelSize.cy};
    const float frostAmount = m_config.FrostAmount();
    const float glassAlpha = DOCK_GLASS_ALPHA + (1.0F - DOCK_GLASS_ALPHA) * frostAmount;
    const float dpiScale = static_cast<float>(HostDpi()) / 96.0F;
    UINT labelFx = DOCK_FX_PANEL | DOCK_FX_LABEL_FACE | DOCK_FX_TINT | DOCK_FX_BLUR;
    if (m_config.RimLight()) {
        labelFx |= DOCK_FX_RIM;
    }
    if (m_config.Lensing()) {
        labelFx |= DOCK_FX_LENS;
    }
    if (m_config.Dispersion()) {
        labelFx |= DOCK_FX_DISPERSION;
    }
    if (m_config.Specular()) {
        labelFx |= DOCK_FX_SPECULAR;
    }
    if (m_config.DropShadow()) {
        labelFx |= DOCK_FX_SHADOW;
    }
    if (m_config.DepthShade()) {
        labelFx |= DOCK_FX_THICKNESS;
    }
    const UINT frostByte =
        static_cast<UINT>(std::lround(std::clamp(frostAmount, 0.0F, 1.0F) * 255.0F));
    labelFx |= (frostByte << 16);
    bool baked = false;
    if (m_rendererInitialized) {
        std::vector<uint8_t> glass;
        // Map dock-client glint into label-local pixels for pointer-reactive rim.
        const float labelGlintX = m_glintX + static_cast<float>(m_windowX - screenOrigin.x);
        const float labelGlintY = m_glintY + static_cast<float>(m_currentY - screenOrigin.y);
        const float labelGlintStrength =
            (m_config.Specular() && m_glintStrength > 0.001F) ? m_glintStrength : 0.0F;
        baked = m_renderer.BakeGlassPanel(screenRect, static_cast<UINT>(labelSize.cx),
            static_cast<UINT>(labelSize.cy), labelFx, glassAlpha, dpiScale, m_settingsWindow,
            m_overflowWindow, m_hoverLabelWindow, m_config.LightPanels(), glass,
            0.5F /* faceMilkBoost */, labelGlintX, labelGlintY, labelGlintStrength);
        if (baked && glass.size() == byteCount) {
            bits = std::move(glass);
        } else {
            baked = false;
        }
    }
    if (!baked) {
        constexpr uint8_t kFallbackA = 204;
        constexpr uint8_t kFallbackC = static_cast<uint8_t>(247U * kFallbackA / 255U);
        for (size_t i = 0; i + 3 < byteCount; i += 4) {
            bits[i + 0] = kFallbackC;
            bits[i + 1] = kFallbackC;
            bits[i + 2] = kFallbackC;
            bits[i + 3] = kFallbackA;
        }
    }

    // Capsule + tail coverage, then a soft drop shadow under it.
    std::vector<float> coverage(pixelCount, 0.0F);
    const float originX = static_cast<float>(bubble.shadow);
    const float originY = static_cast<float>(bubble.shadow);
    const float shapeW = static_cast<float>(bubble.width);
    const float bodyH = static_cast<float>(bubble.body);
    const float tailH = static_cast<float>(bubble.tail);
    const float tailBase = static_cast<float>(bubble.tailBase);
    for (LONG y = 0; y < labelSize.cy; ++y) {
        for (LONG x = 0; x < labelSize.cx; ++x) {
            const float localX = static_cast<float>(x) + 0.5F - originX;
            const float localY = static_cast<float>(y) + 0.5F - originY;
            coverage[static_cast<size_t>(y) * static_cast<size_t>(labelSize.cx) +
                static_cast<size_t>(x)] =
                HoverBubbleCoverage(localX, localY, shapeW, bodyH, tailH, tailBase);
        }
    }

    std::vector<float> shadow = coverage;
    const int blurRadius = std::max(3, static_cast<int>(std::lround(5.0F * scale)));
    std::vector<float> blurTemp(pixelCount, 0.0F);
    for (int pass = 0; pass < 2; ++pass) {
        for (LONG y = 0; y < labelSize.cy; ++y) {
            for (LONG x = 0; x < labelSize.cx; ++x) {
                float sum = 0.0F;
                int count = 0;
                const int x0 = std::max(0L, x - blurRadius);
                const int x1 = std::min(labelSize.cx - 1L, x + blurRadius);
                for (int sx = x0; sx <= x1; ++sx) {
                    sum += shadow[static_cast<size_t>(y) * static_cast<size_t>(labelSize.cx) +
                        static_cast<size_t>(sx)];
                    ++count;
                }
                blurTemp[static_cast<size_t>(y) * static_cast<size_t>(labelSize.cx) +
                    static_cast<size_t>(x)] = count > 0 ? sum / static_cast<float>(count) : 0.0F;
            }
        }
        for (LONG y = 0; y < labelSize.cy; ++y) {
            for (LONG x = 0; x < labelSize.cx; ++x) {
                float sum = 0.0F;
                int count = 0;
                const int y0 = std::max(0L, y - blurRadius);
                const int y1 = std::min(labelSize.cy - 1L, y + blurRadius);
                for (int sy = y0; sy <= y1; ++sy) {
                    sum += blurTemp[static_cast<size_t>(sy) * static_cast<size_t>(labelSize.cx) +
                        static_cast<size_t>(x)];
                    ++count;
                }
                shadow[static_cast<size_t>(y) * static_cast<size_t>(labelSize.cx) +
                    static_cast<size_t>(x)] = count > 0 ? sum / static_cast<float>(count) : 0.0F;
            }
        }
    }

    constexpr float kShadowAlpha = 0.28F;
    const LONG shadowDrop = std::max(1L, static_cast<LONG>(std::lround(2.0F * scale)));
    for (LONG y = 0; y < labelSize.cy; ++y) {
        for (LONG x = 0; x < labelSize.cx; ++x) {
            const size_t index =
                static_cast<size_t>(y) * static_cast<size_t>(labelSize.cx) + static_cast<size_t>(x);
            const float cov = coverage[index];
            const LONG shadowY = y - shadowDrop;
            const float shade = shadowY >= 0
                ? shadow[static_cast<size_t>(shadowY) * static_cast<size_t>(labelSize.cx) +
                      static_cast<size_t>(x)]
                : 0.0F;
            uint8_t* p = bits.data() + index * 4U;
            const float glassA = (static_cast<float>(p[3]) / 255.0F) * cov;
            const float shadowA = shade * kShadowAlpha * (1.0F - glassA);
            const float outA = glassA + shadowA;
            p[0] = static_cast<uint8_t>(std::lround(static_cast<float>(p[0]) * cov));
            p[1] = static_cast<uint8_t>(std::lround(static_cast<float>(p[1]) * cov));
            p[2] = static_cast<uint8_t>(std::lround(static_cast<float>(p[2]) * cov));
            p[3] = static_cast<uint8_t>(std::lround(std::clamp(outA, 0.0F, 1.0F) * 255.0F));
        }
    }

    // Hover labels stay dark on the light speech bubble, on every wallpaper.
    SetFlyoutChromeInk(DOCK_INK_R, DOCK_INK_G, DOCK_INK_B);
    const LONG textBottom = bubble.shadow + bubble.body;
    RECT textBounds{bubble.shadow + bubble.padX / 2L, bubble.shadow,
        bubble.shadow + bubble.width - bubble.padX / 2L, textBottom};
    DrawFlyoutText(bits.data(), SaturatedInt(labelSize.cx), SaturatedInt(labelSize.cy), textBounds,
        font, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
    return true;
}

void DockApp::HideHoverLabel() noexcept {
    m_hoverLabelIcon = -1;
    if (m_hoverLabelWindow != nullptr) {
        ShowWindow(m_hoverLabelWindow, SW_HIDE);
    }
}

void DockApp::LoadIconTextures() {
    const IconLoadRequest request = PrepareIconLoad();
    ApplyIconLoad(request, Renderer::ExtractIconPixelBuffers(request.candidates, request.extent));
}

DockApp::IconLoadRequest DockApp::PrepareIconLoad() {
    // A full reload supersedes any in-flight incremental pin-icon work, which
    // was extracted for a possibly different extent.
    ++m_pinIconGeneration;
    {
        const std::lock_guard lock(m_pinIconMutex);
        m_pinIconPending.reset();
    }
    IconLoadRequest request;
    request.cacheKeys.reserve(m_displayApps.size());
    request.candidates.reserve(m_displayApps.size());
    for (const DisplayApp& app : m_displayApps) {
        if (IsLayoutOnlyTarget(app.app.target)) {
            continue;
        }
        request.cacheKeys.push_back(WindowCatalog::IconCacheKey(app.app));
        request.candidates.push_back(
            WindowCatalog::IconResolutionCandidates(app.app, app.runningWindow));
    }
    m_loadedIconExtent = IconPixelExtent();
    request.extent = m_loadedIconExtent;
    return request;
}

void DockApp::ApplyIconLoad(const IconLoadRequest& request,
    const std::vector<std::vector<uint8_t>>& pixels) {
    m_renderer.BeginIconBatch();
    try {
        m_renderer.LoadIcons(request.cacheKeys, pixels, request.extent);
        m_trayVisualKey.clear();
        EnsureTrayIcons();
        EnsureTrashIcons();
    } catch (...) {
        m_renderer.CancelIconBatch();
        throw;
    }
    m_renderer.EndIconBatch();
    RefreshTrash(true);
}

std::wstring DockApp::TrayIconsKey() const {
    RECT clockBounds{};
    for (const DockIconRenderData& icon : m_iconRenderData) {
        if (icon.kind == DockIconKind::Clock) {
            clockBounds = icon.bounds;
            break;
        }
    }
    const UINT atlas = m_renderer.IconAtlasPixelExtent();
    const UINT clockWidth = clockBounds.right > clockBounds.left
        ? static_cast<UINT>(clockBounds.right - clockBounds.left)
        : 128U;
    const UINT clockHeight = clockBounds.bottom > clockBounds.top
        ? static_cast<UINT>(clockBounds.bottom - clockBounds.top)
        : 48U;
    const TrayStatus& status = m_tray.Status();
    const WeatherService::Snapshot weather = m_weather.GetSnapshot();
    std::wstring weatherKey = weather.valid
        ? (std::wstring(weather.slug.begin(), weather.slug.end()) + L"|" +
            std::to_wstring(static_cast<int>(std::lround(weather.temperatureC))))
        : L"none";
    return std::to_wstring(atlas) + L"|" + std::to_wstring(clockWidth) + L"x" +
        std::to_wstring(clockHeight) + L"|" + status.timeText + L"|" + status.dateText + L"|" +
        (status.hasBattery ? L"1" : L"0") + L"|" + (status.batteryCharging ? L"1" : L"0") + L"|" +
        std::to_wstring(status.batteryPercent) + L"|" + weatherKey;
}

bool DockApp::TrayIconsNeedApply() const {
    // Microseconds: key compare (+ icon-exists check mirroring EnsureTrayIcons
    // so a missing clock glyph is never skipped). The raster + atlas upload
    // half runs deferred via kLayoutApplyMessage.
    return m_rendererInitialized &&
        (TrayIconsKey() != m_trayVisualKey ||
            !m_renderer.HasIconForTarget(SystemTray::TargetForSlot(TraySlot::Clock)));
}

void DockApp::AssignIconTextureIndices() {
    if (!m_rendererInitialized) {
        return;
    }

    const size_t appCount = LesserOf(m_iconRenderData.size(), m_displayApps.size());
    for (size_t index = 0; index < appCount; ++index) {
        if (IsLayoutOnlyTarget(m_displayApps[index].app.target)) {
            continue;
        }
        m_iconRenderData[index].textureIndex = m_renderer.TextureIndexForTarget(
            WindowCatalog::IconCacheKey(m_displayApps[index].app));
    }
    for (size_t index = appCount; index < m_iconRenderData.size(); ++index) {
        const DockIconRenderData& icon = m_iconRenderData[index];
        if (icon.kind == DockIconKind::Trash) {
            m_iconRenderData[index].textureIndex = m_renderer.TextureIndexForTarget(
                RecycleBin::TargetForState(m_trashFull));
            continue;
        }
        if (icon.kind == DockIconKind::Weather) {
            m_iconRenderData[index].textureIndex =
                m_renderer.TextureIndexForTarget(WeatherService::Target());
            continue;
        }
        if (icon.kind != DockIconKind::Tray && icon.kind != DockIconKind::Clock) {
            continue;
        }
        m_iconRenderData[index].textureIndex =
            m_renderer.TextureIndexForTarget(SystemTray::TargetForSlot(icon.traySlot));
    }
}

void DockApp::EnsureTrayIcons() {
    if (!m_rendererInitialized) {
        return;
    }

    const std::wstring key = TrayIconsKey();
    if (key == m_trayVisualKey && m_renderer.HasIconForTarget(
            SystemTray::TargetForSlot(TraySlot::Clock))) {
        AssignIconTextureIndices();
        return;
    }
    m_trayVisualKey = key;

    const TrayStatus& status = m_tray.Status();
    const UINT atlas = m_renderer.IconAtlasPixelExtent();
    RECT clockBounds{};
    for (const DockIconRenderData& icon : m_iconRenderData) {
        if (icon.kind == DockIconKind::Clock) {
            clockBounds = icon.bounds;
            break;
        }
    }
    const UINT clockWidth = clockBounds.right > clockBounds.left
        ? static_cast<UINT>(clockBounds.right - clockBounds.left)
        : 128U;
    const UINT clockHeight = clockBounds.bottom > clockBounds.top
        ? static_cast<UINT>(clockBounds.bottom - clockBounds.top)
        : 48U;

    std::vector<std::wstring> targets;
    std::vector<std::vector<uint8_t>> pixels;
    constexpr TraySlot kSlots[] = {
        TraySlot::Overflow, TraySlot::Power, TraySlot::Clock};
    for (const TraySlot slot : kSlots) {
        if (slot != TraySlot::Clock && !SystemTray::SlotVisible(slot, status)) {
            continue;
        }
        targets.push_back(SystemTray::TargetForSlot(slot));
        if (slot == TraySlot::Clock) {
            const UINT dpi = m_window == nullptr ? 96U : std::max(GetDpiForWindow(m_window), 96U);
            const float layoutScale = static_cast<float>(dpi) / 96.0F * m_dockScale;
            pixels.push_back(m_tray.RasterizeClock(atlas, clockWidth, clockHeight, layoutScale));
        } else {
            pixels.push_back(m_tray.RasterizeGlyph(slot, atlas));
        }
    }
    {
        // Colorful Meteocons fill glyph (not adaptive ink). Upload even when
        // the last fetch failed so a prior/neutral cloudy frame can show.
        const UINT weatherExtent = std::max(1U, atlas);
        std::vector<uint8_t> weatherPixels = m_weather.RasterizeIcon(weatherExtent);
        if (!weatherPixels.empty()) {
            targets.push_back(WeatherService::Target());
            pixels.push_back(std::move(weatherPixels));
        }
    }
    try {
        m_renderer.UpdateCachedIcons(targets, pixels);
    } catch (const std::exception&) {
        Log(L"Tray icon upload failed.");
    }
    AssignIconTextureIndices();
}

void DockApp::RefreshTray(bool forceLayout) {
    // Steady visible-idle fast path: the dock face only shows clock/battery, so
    // the 1 Hz tick skips WLAN/COM-audio/power-scheme IPC unless Quick Settings
    // is open (its tiles need volume/network/brightness). Interval unchanged.
    const bool popupOpen = IsOverflowOpen();
    const bool changed =
        (popupOpen || forceLayout ? m_tray.Refresh() : m_tray.RefreshForDock()) || forceLayout;
    // Trash polls on the same 1 Hz tick (SHQueryRecycleBin is one cheap syscall).
    RefreshTrash(false);
    if (!changed) {
        return;
    }
    RebuildLayout(false);
    if (IsOverflowOpen()) {
        QueueOverflowPaint();
    }
    // Non-blocking: the 1 Hz clock tick must not Present(1)/vsync-wait on the
    // UI thread that services WH_MOUSE_LL (hitch while hovering the dock).
    QueueRenderFrame(false);
}

// --- Trash / Recycle Bin ----------------------------------------------------

bool DockApp::IsTrashRenderIndex(int icon) const noexcept {
    return icon >= 0 && icon == m_trashIndex &&
        static_cast<size_t>(icon) < m_iconRenderData.size() &&
        m_iconRenderData[static_cast<size_t>(icon)].kind == DockIconKind::Trash;
}

int DockApp::TrashRenderIndex() const noexcept {
    return m_trashIndex;
}

bool DockApp::IsPointOverTrash(POINT screen) const noexcept {
    if (m_trashIndex < 0 ||
        static_cast<size_t>(m_trashIndex) >= m_iconRenderData.size()) {
        return false;
    }
    if (m_visibility != VisibilityState::Showing &&
        m_visibility != VisibilityState::Visible) {
        return false;
    }
    const DockIconRenderData& icon =
        m_iconRenderData[static_cast<size_t>(m_trashIndex)];
    if (icon.kind != DockIconKind::Trash) {
        return false;
    }
    // Hit the full dock-height trash slot (same as IconAtScreenPoint), using
    // live layout bounds rather than a stale or half-size glyph rect.
    const POINT client = ClientFromDockOrigin(screen, m_windowX, m_currentY);
    RECT hit = icon.bounds;
    hit.top = 0;
    hit.bottom = static_cast<LONG>(m_dockHeight);
    return IsInside(hit, client.x, client.y);
}

std::wstring DockApp::TrashHoverText() const {
    RecycleBinState shell{};
    shell.itemCount = m_trashState.itemCount;
    shell.byteSize = m_trashState.byteSize;
    shell.isEmpty = m_trashState.isEmpty;
    return RecycleBin::HoverLabel(shell);
}

void DockApp::EnsureTrashIcons() {
    if (!m_rendererInitialized) {
        return;
    }
    const UINT atlas = m_renderer.IconAtlasPixelExtent();
    const bool hasEmpty = m_renderer.HasIconForTarget(RecycleBin::kEmptyTarget);
    const bool hasFull = m_renderer.HasIconForTarget(RecycleBin::kFullTarget);
    if (hasEmpty && hasFull && atlas == m_loadedTrashExtent) {
        return;
    }
    std::vector<std::wstring> targets;
    targets.reserve(2);
    targets.emplace_back(RecycleBin::kEmptyTarget);
    targets.emplace_back(RecycleBin::kFullTarget);
    std::vector<std::vector<uint8_t>> pixels;
    pixels.reserve(2);
    pixels.push_back(RecycleBin::IconPixels(false, atlas));
    pixels.push_back(RecycleBin::IconPixels(true, atlas));
    // If the stock icons fail (very old Windows / stripped SKU), fall back to
    // the live Recycle Bin folder image for both states so the slot never blanks.
    if (pixels[0].empty() || pixels[1].empty()) {
        const std::vector<std::wstring> fallbackCandidates = {L"shell:RecycleBinFolder"};
        const std::vector<uint8_t> fallback =
            Renderer::ExtractIconPixels(fallbackCandidates, atlas);
        if (pixels[0].empty()) {
            pixels[0] = fallback;
        }
        if (pixels[1].empty()) {
            pixels[1] = fallback;
        }
    }
    if (pixels[0].empty() && pixels[1].empty()) {
        return;
    }
    try {
        m_renderer.UpdateCachedIcons(targets, pixels);
    } catch (const std::exception&) {
        Log(L"Trash icon upload failed.");
        return;
    }
    m_loadedTrashExtent = atlas;
    AssignIconTextureIndices();
}

void DockApp::UpdateTrashIconState(bool full) {
    if (m_trashFull == full) {
        return;
    }
    m_trashFull = full;
    AssignIconTextureIndices();
    QueueRenderFrame();
    // Refresh the hover bubble immediately when it is showing for Trash.
    if (m_hoveredIcon == m_trashIndex) {
        UpdateHoverLabel();
    }
}

void DockApp::RefreshTrash(bool forceLayout) {
    RecycleBinState shell{};
    if (!RecycleBin::QueryState(shell)) {
        return;
    }
    const bool full = !shell.isEmpty;
    const bool changed = (full != m_trashFull) ||
        (shell.itemCount != m_trashState.itemCount) ||
        (shell.byteSize != m_trashState.byteSize) || forceLayout;
    m_trashState.itemCount = shell.itemCount;
    m_trashState.byteSize = shell.byteSize;
    m_trashState.isEmpty = shell.isEmpty;
    if (!changed) {
        // Still ensure icons exist after a full texture reload (extent change).
        if (!m_renderer.HasIconForTarget(RecycleBin::kEmptyTarget) ||
            !m_renderer.HasIconForTarget(RecycleBin::kFullTarget)) {
            EnsureTrashIcons();
            QueueRenderFrame();
        }
        return;
    }
    EnsureTrashIcons();
    if (full != m_trashFull) {
        m_trashFull = full;
        AssignIconTextureIndices();
    }
    // Count/size changes update the hover label + context menu text.
    if (m_hoveredIcon == m_trashIndex) {
        UpdateHoverLabel();
    }
    if (IsContextMenuOpen() && m_contextIsTrash) {
        // Rebuild the open Trash menu so Empty shows the fresh count.
        POINT anchor = m_contextAnchor;
        const int trash = m_trashIndex;
        ShowContextMenu(anchor, trash);
    }
    QueueRenderFrame();
}

void DockApp::OpenTrash() {
    if (!RecycleBin::Open()) {
        Log(L"Recycle Bin did not open.");
        MessageBoxW(m_window, L"The Recycle Bin could not be opened.",
            L"Recycle Bin", MB_OK | MB_ICONWARNING);
    }
}

void DockApp::EmptyTrashWithConfirm() {
    RecycleBinState shell{};
    if (!RecycleBin::QueryState(shell) || shell.isEmpty) {
        return;
    }
    const std::wstring confirm = RecycleBin::EmptyConfirmText(shell);
    const int answer = MessageBoxW(m_window, confirm.c_str(), L"Empty Recycle Bin",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
        return;
    }
    // Custom confirmation already shown: suppress the shell's duplicate.
    if (!RecycleBin::Empty(m_window, true)) {
        Log(L"Empty Recycle Bin failed.");
        MessageBoxW(m_window, L"The Recycle Bin could not be emptied.",
            L"Recycle Bin", MB_OK | MB_ICONWARNING);
        return;
    }
    // Immediate empty-state feedback; the SHChangeNotify + poll confirms.
    m_trashState = {};
    m_trashState.isEmpty = true;
    UpdateTrashIconState(false);
    RefreshTrash(true);
}

void DockApp::ShowTrashProperties() {
    if (!RecycleBin::ShowProperties(m_window)) {
        Log(L"Recycle Bin properties did not open.");
    }
}

void DockApp::RegisterTrashNotify() {
    if (m_trashNotifyCookie != 0 || m_window == nullptr) {
        return;
    }
    LPITEMIDLIST pidl = nullptr;
    if (FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &pidl)) ||
        pidl == nullptr) {
        return;
    }
    SHChangeNotifyEntry entry{};
    entry.pidl = pidl;
    entry.fRecursive = TRUE;
    const LONG events = SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
        SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM | SHCNE_UPDATEDIR;
    const ULONG cookie = SHChangeNotifyRegister(m_window,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        events, kTrashNotifyMessage, 1, &entry);
    if (cookie == 0) {
        CoTaskMemFree(pidl);
        return;
    }
    m_trashNotifyCookie = cookie;
    m_trashNotifyPidl = reinterpret_cast<TrashPidl>(pidl);
}

void DockApp::UnregisterTrashNotify() noexcept {
    if (m_trashNotifyCookie != 0) {
        (void)SHChangeNotifyDeregister(m_trashNotifyCookie);
        m_trashNotifyCookie = 0;
    }
    if (m_trashNotifyPidl != nullptr) {
        CoTaskMemFree(m_trashNotifyPidl);
        m_trashNotifyPidl = nullptr;
    }
}

void DockApp::RegisterTrashDropTarget() {
    if (m_trashDropTarget != nullptr) {
        return;
    }
    if (m_window == nullptr && m_inputWindow == nullptr) {
        return;
    }
    auto* target = new (std::nothrow) DockAppTrashDropTarget(this);
    if (target == nullptr) {
        return;
    }
    // Register on the topmost DComp renderer AND the layered input window.
    // OLE's drop-target walk uses WindowFromPoint then parents only: when the
    // renderer sits above the input HWND (HWND_TOPMOST) and still reports as
    // the hit window despite HTTRANSPARENT, the input-only registration never
    // receives DragEnter/Drop. Dual registration covers both hit paths.
    bool any = false;
    if (m_window != nullptr) {
        const HRESULT hr = RegisterDragDrop(m_window, target);
        if (SUCCEEDED(hr)) {
            m_trashDropOnRenderer = true;
            any = true;
        } else {
            Log(L"Trash drop target registration failed on renderer HWND.");
        }
    }
    if (m_inputWindow != nullptr) {
        const HRESULT hr = RegisterDragDrop(m_inputWindow, target);
        if (SUCCEEDED(hr)) {
            m_trashDropOnInput = true;
            any = true;
        } else {
            Log(L"Trash drop target registration failed on input HWND.");
        }
    }
    if (!any) {
        target->Release();
        Log(L"Trash drop target registration failed.");
        return;
    }
    // RegisterDragDrop AddRefs per HWND; keep ours for Revoke.
    m_trashDropTarget = target;
    Log(L"Trash drop target registered (renderer=" +
        std::wstring(m_trashDropOnRenderer ? L"yes" : L"no") + L", input=" +
        std::wstring(m_trashDropOnInput ? L"yes" : L"no") + L").");
}

void DockApp::RevokeTrashDropTarget() noexcept {
    if (m_trashDropTarget == nullptr) {
        return;
    }
    if (m_trashDropOnInput && m_inputWindow != nullptr) {
        (void)RevokeDragDrop(m_inputWindow);
    }
    if (m_trashDropOnRenderer && m_window != nullptr) {
        (void)RevokeDragDrop(m_window);
    }
    m_trashDropOnInput = false;
    m_trashDropOnRenderer = false;
    m_trashDropTarget->Release();
    m_trashDropTarget = nullptr;
}

void DockApp::OnTrashDragEnter() {
    if (m_trashDragOver) {
        return;
    }
    m_trashDragOver = true;
    // Pressed-path darken (~50%); skip hover bubble so it cannot steal OLE hits.
    HideHoverLabel();
    QueueRenderFrame(false);
}

void DockApp::OnTrashDragOver(POINT screen) {
    const bool overTrash = IsPointOverTrash(screen);
    if (overTrash) {
        if (!m_trashDragOver) {
            m_trashDragOver = true;
            HideHoverLabel();
            QueueRenderFrame(false);
        }
    } else if (m_trashDragOver) {
        OnTrashDragLeave();
    }
}

void DockApp::OnTrashDragLeave() {
    if (!m_trashDragOver) {
        return;
    }
    m_trashDragOver = false;
    QueueRenderFrame(false);
}

bool DockApp::OnTrashDrop(const std::vector<std::wstring>& paths) {
    if (paths.empty() || m_trashDropInFlight) {
        return false;
    }
    m_trashDropInFlight = true;
    // Optimistic full-state; HandleTrashDropResult refreshes from the shell.
    UpdateTrashIconState(true);

    // Run on the OLE/UI STA (OleInitialize). Must finish before IDropTarget::Drop
    // returns so *pdwEffect and CFSTR_PERFORMEDDROPEFFECT match reality.
    RecycleBin::MoveResult result = RecycleBin::MoveToRecycleBin(m_window, paths);

    auto* reply = new (std::nothrow) TrashDropReply();
    bool moved = false;
    if (reply == nullptr) {
        m_trashDropInFlight = false;
        RefreshTrash(true);
        return result.moved > 0 && !result.aborted;
    }
    if (result.aborted) {
        reply->error.clear();
        reply->moved = false;
    } else if (result.moved > 0 && result.failed == 0) {
        reply->moved = true;
        moved = true;
    } else if (result.moved > 0) {
        reply->moved = true;
        reply->error = result.error;
        moved = true;
    } else {
        reply->moved = false;
        reply->error = result.error.empty()
            ? L"Windows could not move these items to the Recycle Bin."
            : result.error;
    }
    // Defer MessageBox / icon refresh so we do not re-enter OLE during Drop.
    if (PostMessageW(m_window, kTrashDropMessage, 0,
            reinterpret_cast<LPARAM>(reply)) == FALSE) {
        delete reply;
        m_trashDropInFlight = false;
    }
    return moved;
}

void DockApp::HandleTrashDropResult(const std::wstring& error, bool moved) {
    m_trashDropInFlight = false;
    m_trashDragOver = false;
    if (!moved && !error.empty()) {
        Log(L"Move to Recycle Bin failed: " + error);
        MessageBoxW(m_window, error.c_str(), L"Recycle Bin",
            MB_OK | MB_ICONWARNING);
    }
    // Authoritative refresh (covers partial success, locked files, etc.).
    RefreshTrash(true);
    QueueRenderFrame();
}

void DockApp::OpenTraySlot(TraySlot slot) {
    if (slot == TraySlot::Overflow) {
        ToggleOverflowPopup();
        return;
    }
    if (slot == TraySlot::Volume) {
        if (!m_tray.ToggleMute()) {
            CloseOverflowPopup();
            if (!SystemTray::OpenSoundMixer()) {
                Log(L"Volume control did not accept input.");
            }
            return;
        }
        EnsureTrayIcons();
        if (IsOverflowOpen()) {
            QueueOverflowPaint();
        }
        QueueRenderFrame();
        return;
    }

    CloseOverflowPopup();
    bool opened = false;
    if (slot == TraySlot::Network) {
        opened = SystemTray::OpenNetworkPanel();
    } else if (slot == TraySlot::Power) {
        opened = SystemTray::OpenPowerSettings();
    } else if (slot == TraySlot::Clock) {
        PrepareShellForStartMenu();
        // Same fullscreen-claim refusal as the Start menu (see
        // OpenStartMenuFromDock): unmark first or Explorer can swallow Win+N.
        UnmarkFullscreenClaims();
        opened = SystemTray::OpenNotificationCenter();
        if (opened) {
            m_shellFlyoutIsSearch = false;
            m_shellFlyoutIsTray = true;
            return;
        }
        ReleaseShellFlyoutHold();
    }
    if (!opened) {
        Log(L"System flyout did not accept input.");
    }
}

void DockApp::ToggleOverflowPopup() {
    if (IsOverflowOpen()) {
        BeginOverflowHide(false);
        return;
    }
    BeginOverflowShow();
}

void DockApp::QueueOverflowPaint(bool hoverOnly) {
    // Coalesce rapid hover transitions into a single repaint so fast cursor
    // movement can't stack full synchronous paints on this thread (which also
    // services the low-level mouse hook). The latest m_overflowHover wins.
    if (m_overflowWindow == nullptr) {
        return;
    }
    if (!hoverOnly) {
        m_overflowHoverPaintOnly = false;
    } else if (!m_overflowPaintQueued) {
        m_overflowHoverPaintOnly = true;
    }
    if (m_overflowPaintQueued) {
        return;
    }
    m_overflowPaintQueued = true;
    PostMessageW(m_window, kOverflowPaintMessage, 0, 0);
}

void DockApp::RefreshBrightnessAsync() {
    // One-shot worker refresh of the DDC level (never on the UI thread).
    // Repaints through the usual coalesced path when anything changed. Only
    // the popup shows brightness, so no dock icon/render work is needed.
    if (m_brightnessRefreshInFlight.exchange(true)) {
        return;
    }
    std::thread([this] {
        const bool changed = m_tray.RefreshBrightnessFromDdc();
        m_brightnessRefreshInFlight.store(false);
        if (changed) {
            QueueOverflowPaint();
        }
    }).detach();
}

void DockApp::RunPerformanceBoost() {
    // Confidence-gated, high-stakes action: Jev supplies safe/idle
    // probabilities, but code owns the thresholds, the denylist, and every
    // close. Noul values near 0.5 (uncertain) never act.
    constexpr double kBoostSafeClose = 0.85;
    constexpr double kBoostIdleClose = 0.60;
    constexpr size_t kBoostMaxCloses = 20;
    constexpr size_t kHeuristicMaxCloses = 10;
    constexpr uint64_t kHeuristicMinAgeSeconds = 30;

    if (m_boostInFlight.exchange(true)) {
        return;
    }
    m_boostStatus = L"Profiling...";
    // Status-only: live overlay, never a full PaintOverflowPopup on click.
    if (IsOverflowOpen()) {
        m_qsBoostStatusDirty = true;
        PaintOverflowLiveFast();
    }

    const std::wstring apiKey = m_config.TypeSafeApiKey();
    const HWND replyWindow = m_window;
    std::thread([this, apiKey, replyWindow]() {
        auto completed = std::make_shared<std::atomic<bool>>(false);
        auto postStatus = [this, replyWindow, completed](std::wstring status, bool finished) {
            if (finished) {
                completed->store(true);
            }
            auto* reply = new BoostReply{std::move(status), finished};
            if (replyWindow == nullptr ||
                PostMessageW(replyWindow, kBoostResultMessage, 0,
                    reinterpret_cast<LPARAM>(reply)) == FALSE) {
                delete reply;
                // PostMessage failed: ApplyBoostResult will never clear the latch.
                if (finished) {
                    m_boostInFlight.store(false);
                }
            }
        };
        // If the worker returns/throws without a finished postStatus, recover UI.
        struct EnsureBoostFinished {
            std::function<void(std::wstring, bool)> post;
            std::shared_ptr<std::atomic<bool>> completed;
            ~EnsureBoostFinished() {
                if (completed && !completed->load()) {
                    try {
                        post(L"Unavailable", true);
                    } catch (...) {
                    }
                }
            }
        } ensureFinished{postStatus, completed};

        const auto waitForJudgment = [](std::chrono::milliseconds budget,
                                          auto&& work) -> BoostResult {
            auto state = std::make_shared<std::pair<std::mutex, BoostResult>>();
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::thread([state, done, work = std::forward<decltype(work)>(work)]() mutable {
                BoostResult result = work();
                {
                    std::lock_guard<std::mutex> lock(state->first);
                    state->second = std::move(result);
                }
                done->store(true);
            }).detach();
            const auto deadline = std::chrono::steady_clock::now() + budget;
            while (!done->load()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    BoostResult timedOut;
                    timedOut.error = L"TypeSafe classification timed out.";
                    return timedOut;
                }
                Sleep(50);
            }
            std::lock_guard<std::mutex> lock(state->first);
            return state->second;
        };

        const auto selectHeuristic = [](const std::vector<BoostProcess>& candidates) {
            std::vector<BoostProcess> selected;
            for (const BoostProcess& candidate : candidates) {
                if (selected.size() >= kHeuristicMaxCloses) {
                    break;
                }
                // Graceful WM_CLOSE only, so only windowed, settled apps.
                if (candidate.hasWindow && candidate.ageSeconds >= kHeuristicMinAgeSeconds) {
                    selected.push_back(candidate);
                }
            }
            return selected;
        };

        // Profiling (enumeration + CPU sample) runs here, off the UI thread.
        // Guarded: an uncaught throw in a detached thread terminates the dock.
        try {
            // Startup apps: enumerate → Jev classifies safe/nonessential to
            // disable → code applies StartupApproved + ends processes.
            // Without an API key, a soft chat/browser/game-launcher name
            // heuristic runs (not a personal allow/deny list).
            postStatus(L"Startup apps...", false);
            BoostStartupResult startup{};
            const std::vector<BoostStartupEntry> startupCandidates =
                PerfBoost::EnumerateStartupCandidates();
            if (!startupCandidates.empty()) {
                std::vector<BoostStartupEntry> startupSelected;
                if (apiKey.empty()) {
                    startupSelected = PerfBoost::SelectStartupHeuristic(startupCandidates);
                    Log(L"Boost startup heuristic: " +
                        std::to_wstring(startupSelected.size()) + L" of " +
                        std::to_wstring(startupCandidates.size()) + L" candidates.");
                } else {
                    std::vector<BoostCandidate> startupInputs;
                    startupInputs.reserve(startupCandidates.size());
                    for (size_t index = 0; index < startupCandidates.size(); ++index) {
                        const BoostStartupEntry& entry = startupCandidates[index];
                        BoostCandidate input;
                        input.id = "s" + std::to_string(index);
                        input.name = entry.valueName;
                        input.exePath = entry.exePath.empty() ? entry.exeName : entry.exePath;
                        input.hasWindow = entry.hasWindow;
                        input.memoryMb =
                            static_cast<double>(entry.memoryBytes) / 1048576.0;
                        input.cpuPercent = entry.cpuPercent;
                        input.ageSeconds = entry.hasRunningProcess && entry.ageSeconds == 0
                            ? 1ULL
                            : entry.ageSeconds;
                        startupInputs.push_back(std::move(input));
                    }
                    // Hard budget so a hung/slow api.typesafe.ai cannot leave
                    // the UI on "Startup apps..." forever (WinHTTP alone is not
                    // enough when the server keeps the socket alive).
                    const BoostResult startupJudged = waitForJudgment(
                        std::chrono::seconds(12), [&]() {
                            return TypeSafeClient::ClassifyForStartupBoost(
                                apiKey, startupInputs);
                        });
                    if (!startupJudged.ok) {
                        Log(L"Boost startup judgment failed: " + startupJudged.error);
                        startupSelected = PerfBoost::SelectStartupHeuristic(startupCandidates);
                    } else {
                        for (const BoostJudgment& judgment : startupJudged.items) {
                            if (judgment.safe >= kBoostSafeClose &&
                                judgment.idle >= kBoostIdleClose) {
                                try {
                                    const size_t index =
                                        static_cast<size_t>(std::stoul(judgment.id.substr(1)));
                                    if (index < startupCandidates.size()) {
                                        startupSelected.push_back(startupCandidates[index]);
                                    }
                                } catch (...) {
                                    continue;
                                }
                            }
                        }
                        Log(L"Boost startup judgments: " +
                            std::to_wstring(startupJudged.items.size()) + L" judged, " +
                            std::to_wstring(startupSelected.size()) + L" selected.");
                    }
                }
                if (!startupSelected.empty()) {
                    startup = PerfBoost::DisableStartupTargets(startupSelected);
                    Log(L"Boost startup: disabled " + std::to_wstring(startup.disabled) +
                        L", ended " + std::to_wstring(startup.ended) + L", freed " +
                        PerfBoost::FormatMegabytes(startup.freedBytes));
                }
            }

            // Leave "Startup apps..." as soon as startup work finishes so a
            // slow process snapshot cannot look like a startup hang.
            postStatus(L"Profiling...", false);
            const std::vector<BoostProcess> candidates = PerfBoost::EnumerateClosableCandidates();
            if (candidates.empty()) {
                if (startup.ended > 0) {
                    postStatus(L"Freed " + PerfBoost::FormatMegabytes(startup.freedBytes), true);
                } else if (startup.disabled > 0) {
                    postStatus(L"Startup cleared", true);
                } else {
                    postStatus(L"All clear", true);
                }
                return;
            }

            std::vector<BoostProcess> selected;
            bool allowTerminate = false;
            if (apiKey.empty()) {
                selected = selectHeuristic(candidates);
                if (selected.empty() && startup.disabled <= 0 && startup.ended <= 0) {
                    postStatus(L"Need API key", true);
                    return;
                }
                if (selected.empty()) {
                    if (startup.ended > 0) {
                        postStatus(L"Freed " + PerfBoost::FormatMegabytes(startup.freedBytes),
                            true);
                    } else {
                        postStatus(L"Startup cleared", true);
                    }
                    return;
                }
            } else {
                postStatus(std::to_wstring(candidates.size()) + L" apps...", false);
                std::vector<BoostCandidate> inputs;
                inputs.reserve(candidates.size());
                for (size_t index = 0; index < candidates.size(); ++index) {
                    const BoostProcess& candidate = candidates[index];
                    BoostCandidate input;
                    input.id = "p" + std::to_string(index);
                    input.name = candidate.name;
                    input.exePath = candidate.exePath;
                    input.windowTitle = candidate.windowTitle;
                    input.hasWindow = candidate.hasWindow;
                    input.memoryMb = static_cast<double>(candidate.memoryBytes) / 1048576.0;
                    input.cpuPercent = candidate.cpuPercent;
                    input.ageSeconds = candidate.ageSeconds;
                    inputs.push_back(std::move(input));
                }
                const BoostResult judged = waitForJudgment(std::chrono::seconds(20), [&]() {
                    return TypeSafeClient::ClassifyForBoost(apiKey, inputs);
                });
                if (!judged.ok) {
                    Log(L"Boost judgment failed: " + judged.error);
                    selected = selectHeuristic(candidates);
                    if (selected.empty() && startup.disabled <= 0 && startup.ended <= 0) {
                        postStatus(L"Unavailable", true);
                        return;
                    }
                    if (selected.empty()) {
                        if (startup.ended > 0) {
                            postStatus(
                                L"Freed " + PerfBoost::FormatMegabytes(startup.freedBytes), true);
                        } else {
                            postStatus(L"Startup cleared", true);
                        }
                        return;
                    }
                } else {
                    for (const BoostJudgment& judgment : judged.items) {
                        if (judgment.safe >= kBoostSafeClose && judgment.idle >= kBoostIdleClose) {
                            try {
                                const size_t index =
                                    static_cast<size_t>(std::stoul(judgment.id.substr(1)));
                                if (index < candidates.size() &&
                                    selected.size() < kBoostMaxCloses) {
                                    selected.push_back(candidates[index]);
                                }
                            } catch (...) {
                                continue;
                            }
                        }
                    }
                    Log(L"Boost judgments: " + std::to_wstring(judged.items.size()) +
                        L" judged, " + std::to_wstring(selected.size()) + L" selected.");
                    allowTerminate = true;
                    if (selected.empty()) {
                        if (startup.ended > 0) {
                            postStatus(
                                L"Freed " + PerfBoost::FormatMegabytes(startup.freedBytes), true);
                        } else if (startup.disabled > 0) {
                            postStatus(L"Startup cleared", true);
                        } else {
                            postStatus(L"All clear", true);
                        }
                        return;
                    }
                }
            }

            postStatus(L"Closing...", false);
            const BoostCloseResult closed =
                PerfBoost::CloseTargets(selected, allowTerminate);
            const uint64_t freedTotal = closed.freedBytes + startup.freedBytes;
            if (closed.closed <= 0 && startup.ended <= 0) {
                if (startup.disabled > 0) {
                    postStatus(L"Startup cleared", true);
                } else {
                    postStatus(L"All clear", true);
                }
                return;
            }
            Log(L"Boost closed " + std::to_wstring(closed.closed) + L", ended startup " +
                std::to_wstring(startup.ended) + L", freed " +
                PerfBoost::FormatMegabytes(freedTotal));
            postStatus(L"Freed " + PerfBoost::FormatMegabytes(freedTotal), true);
        } catch (...) {
            postStatus(L"Unavailable", true);
        }
    }).detach();
}

void DockApp::ApplyBoostResult(const std::wstring& status, bool finished) {
    if (finished) {
        m_boostInFlight.store(false);
    }
    if (!status.empty()) {
        m_boostStatus = status;
    }
    Log(L"Boost: " + m_boostStatus);
    if (IsOverflowOpen()) {
        // Worker status posts used to QueueOverflowPaint -> ~100ms
        // PaintQuickSettings on the WH_MOUSE_LL thread (cursor hitch every
        // "Profiling..." / "Closing..." / "Freed N MB"). Live-fast only.
        m_qsBoostStatusDirty = true;
        PaintOverflowLiveFast();
    }
}

void DockApp::DrainBrightnessWheel() {
    // Single-flight worker: each DDC set can block ~1 s on some monitors, so
    // bursts settle here and only the latest projected value is ever written.
    // The UI already shows the projection; this just makes the monitor follow.
    for (;;) {
        Sleep(150);
        const int target = m_brightnessTarget.exchange(-1);
        if (target >= 0) {
            if (m_tray.SetBrightnessLevel(target)) {
                QueueOverflowPaint();
            }
        }
        m_brightnessAdjustInFlight.store(false);
        if (m_brightnessTarget.load() < 0 || m_brightnessAdjustInFlight.exchange(true)) {
            return;
        }
    }
}

void DockApp::ScrollBrightness(int delta) {
    // UI thread only: project the new level instantly (fill + % repaint below)
    // and let the worker persist it in the background.
    m_brightnessWheelRemainder += delta;
    const int steps = m_brightnessWheelRemainder / WHEEL_DELTA;
    if (steps == 0) {
        return;
    }
    m_brightnessWheelRemainder %= WHEEL_DELTA;
    int target = m_brightnessTarget.load();
    if (target < 0) {
        if (!m_tray.Status().brightnessAvailable) {
            return;
        }
        target = m_tray.Status().brightnessPercent;
    }
    target = std::clamp(target + steps * 5, 0, 100);
    m_brightnessTarget.store(target);
    QueueOverflowPaint();
    if (!m_brightnessAdjustInFlight.exchange(true)) {
        std::thread([this] { DrainBrightnessWheel(); }).detach();
    }
}

void DockApp::EnsureOverflowGlyphs(UINT gearExtent, UINT tileExtent) {
    const TrayStatus& status = m_tray.Status();
    std::wstring key = std::to_wstring(gearExtent) + L"|" + std::to_wstring(tileExtent) + L"|" +
        std::to_wstring(static_cast<int>(status.network)) +
        L"|" + std::to_wstring(status.wifiBars) + L"|" + (status.volumeMuted ? L"1" : L"0") + L"|" +
        std::to_wstring(static_cast<int>(std::lround(status.volumeLevel * 100.0F))) + L"|" +
        std::to_wstring(g_flyoutInkR) + L"|" + std::to_wstring(g_flyoutInkG) + L"|" +
        std::to_wstring(g_flyoutInkB);
    if (key == m_overflowGlyphKey) {
        return;
    }
    m_overflowGlyphKey = std::move(key);
    m_overflowGlyphGear = m_tray.RasterizeSymbol(L'\uE713', gearExtent);
    m_overflowGlyphWifi = m_tray.RasterizeGlyph(TraySlot::Network, tileExtent);
    m_overflowGlyphSound = m_tray.RasterizeGlyph(TraySlot::Volume, tileExtent);
    m_overflowGlyphBrightness = m_tray.RasterizeSymbol(L'\uE706', tileExtent);
    // E945 is the Segoe MDL2 Assets lightning bolt: the Boost mark.
    m_overflowGlyphBoost = m_tray.RasterizeSymbol(L'\uE945', tileExtent);

    // Tray rasterizer bakes DOCK_INK; remap to the live adaptive chrome ink so
    // Quick Settings glyphs match Start/Search on the dock.
    RemapPremulInkColor(m_overflowGlyphGear, g_flyoutInkR, g_flyoutInkG, g_flyoutInkB);
    RemapPremulInkColor(m_overflowGlyphWifi, g_flyoutInkR, g_flyoutInkG, g_flyoutInkB);
    RemapPremulInkColor(m_overflowGlyphSound, g_flyoutInkR, g_flyoutInkG, g_flyoutInkB);
    RemapPremulInkColor(m_overflowGlyphBrightness, g_flyoutInkR, g_flyoutInkG, g_flyoutInkB);
    RemapPremulInkColor(m_overflowGlyphBoost, g_flyoutInkR, g_flyoutInkG, g_flyoutInkB);
}

void DockApp::FinishOverflowHide() noexcept {
    RemoveOverflowDismissHook();
    if (m_window != nullptr) {
        KillTimer(m_window, kBluetoothTimerId);
    }
    m_bluetooth.StopDiscovery();
    CloseDockSettings();
    if (m_overflowWindow != nullptr) {
        KillTimer(m_overflowWindow, kEnergySampleTimerId);
        KillTimer(m_overflowWindow, kQsLiveTimerId);
        KillTimer(m_overflowWindow, kQsTempsTimerId);
        ShowWindow(m_overflowWindow, SW_HIDE);
    }
    m_overflowVisibility = VisibilityState::Hidden;
    StopQuickSettingsCapture();
    // Keep the last Home present/base/underlay + glass across hide so the next
    // chevron click can UpdateLayeredWindow without a BakeGlassPanel / full
    // Quick Settings rebuild on the UI thread (was the remaining cursor hitch).
    const bool keepHomeFrame = m_qsPage == QuickSettingsPage::Home &&
        !m_overflowPresentBits.empty() && m_overflowPresentSize.cx > 0 &&
        m_overflowPresentSize.cy > 0 && m_overflowPresentSize.cx == m_overflowSize.cx &&
        m_overflowPresentSize.cy == m_overflowSize.cy;
    m_overflowHover = -1;
    m_qsPage = QuickSettingsPage::Home;
    m_qsReturn = QuickSettingsPage::Home;
    m_qsDragging = false;
    if (!keepHomeFrame) {
        std::vector<uint8_t>().swap(m_overflowPresentBits);
        std::vector<uint8_t>().swap(m_overflowBaseBits);
        std::vector<uint8_t>().swap(m_overflowUnderlayBits);
        m_overflowPresentSize = {};
        m_qsMeterValid = false;
        m_qsScrubValid = false;
        m_qsTempsValid = false;
        m_qsTempsDirty = false;
        m_qsBoostStatusValid = false;
        m_qsBoostStatusDirty = false;
        m_qsBtStatusValid = false;
        m_qsBtStatusDirty = false;
        m_qsBtStatusRect = {};
        m_qsMeterRect = {};
        m_qsScrubRect = {};
        m_qsTempCpuRect = {};
        m_qsTempGpuRect = {};
        m_qsBoostStatusRect = {};
    }
    // keepHomeFrame: retain meter/scrub/temps rects + validity so the 40ms live
    // timer uses PaintOverflowLiveFast instead of falling back to a ~100ms
    // PaintOverflowPopup (that fallback was the post-chevron cursor hitch).
    m_overflowHoverPaintOnly = false;
    m_overflowHoverDirtyValid = false;
    // Glass stays: origin/caret/frost validity is checked on the next paint.
    if (m_visibility == VisibilityState::Visible) {
        StartTrayTimer();
    }
}


bool DockApp::EnsureLayerPresentDib(LayerPresentDib& slot, LONG width, LONG height) noexcept
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (slot.dc != nullptr && slot.bitmap != nullptr && slot.bits != nullptr &&
        slot.width == width && slot.height == height) {
        return true;
    }
    ReleaseLayerPresentDib(slot);
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return false;
    }
    SetICMMode(screen, ICM_OFF);
    slot.dc = CreateCompatibleDC(screen);
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = width;
    header.bV5Height = -height;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    void* bits = nullptr;
    slot.bitmap = CreateDIBSection(screen, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (slot.dc == nullptr || slot.bitmap == nullptr || bits == nullptr) {
        ReleaseLayerPresentDib(slot);
        return false;
    }
    slot.previous = SelectObject(slot.dc, slot.bitmap);
    if (slot.previous == nullptr || slot.previous == HGDI_ERROR) {
        ReleaseLayerPresentDib(slot);
        return false;
    }
    slot.bits = bits;
    slot.width = width;
    slot.height = height;
    return true;
}

void DockApp::ReleaseLayerPresentDib(LayerPresentDib& slot) noexcept
{
    if (slot.dc != nullptr && slot.previous != nullptr && slot.previous != HGDI_ERROR) {
        SelectObject(slot.dc, slot.previous);
    }
    slot.previous = nullptr;
    if (slot.bitmap != nullptr) {
        DeleteObject(slot.bitmap);
        slot.bitmap = nullptr;
    }
    slot.bits = nullptr;
    if (slot.dc != nullptr) {
        DeleteDC(slot.dc);
        slot.dc = nullptr;
    }
    slot.width = 0;
    slot.height = 0;
    slot.contentValid = false;
}

bool DockApp::PresentLayeredBits(HWND window, const POINT& origin, LONG width, LONG height,
    const uint8_t* pixels, size_t byteCount, LayerPresentDib& slot,
    const RECT* dirty, bool preservePopupShadow) noexcept
{
    if (window == nullptr || pixels == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    ClearPopupLayeredCorners(window);
    const size_t need = static_cast<size_t>(width) * static_cast<size_t>(height) * 4U;
    if (byteCount < need) {
        return false;
    }
    if (!EnsureLayerPresentDib(slot, width, height) || slot.bits == nullptr || slot.dc == nullptr) {
        return false;
    }
    // Last gate before ULW: the panel shadow (and any stale bake) leaves
    // non-zero alpha in the square wedges. Zero them on the source so a later
    // dirty present cannot copy the dark rectangle back in.
    PunchPopupSquircleWedges(window, const_cast<uint8_t*>(pixels), width, height,
        preservePopupShadow);
    // A dirty rect is only valid when the DIB already matches the screen except
    // for that rect. Live glass clears contentValid; clipping the composite to
    // the hover highlight would leave the rest of the plate frozen.
    const bool partial = dirty != nullptr && slot.contentValid;
    if (partial) {
        const LONG left = (std::max)(0L, dirty->left);
        const LONG top = (std::max)(0L, dirty->top);
        const LONG right = (std::min)(width, dirty->right);
        const LONG bottom = (std::min)(height, dirty->bottom);
        if (right > left && bottom > top) {
            const size_t rowBytes = static_cast<size_t>(right - left) * 4U;
            for (LONG y = top; y < bottom; ++y) {
                const size_t offset =
                    (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(left)) *
                    4U;
                std::memcpy(static_cast<uint8_t*>(slot.bits) + offset, pixels + offset, rowBytes);
            }
        }
    } else {
        std::memcpy(slot.bits, pixels, need);
        slot.contentValid = true;
        // Clip the HWND to the frosted shape so DWM cannot paint black in the
        // transparent corner wedges outside the squircle (or the caret).
        ClipLayeredPopupToCoverage(window, pixels, width, height);
    }
    POINT source{0, 0};
    POINT destination{origin.x, origin.y};
    SIZE present{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (partial) {
        UPDATELAYEREDWINDOWINFO info{};
        info.cbSize = sizeof(info);
        info.pptDst = &destination;
        info.psize = &present;
        info.hdcSrc = slot.dc;
        info.pptSrc = &source;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = dirty;
        return UpdateLayeredWindowIndirect(window, &info) != FALSE;
    }
    const BOOL ok = UpdateLayeredWindow(window, nullptr, &destination, &present, slot.dc, &source, 0,
        &blend, ULW_ALPHA);
    return ok != FALSE;
}

void DockApp::PresentOverflowLayer() noexcept {
    ProfileScope scope("PresentOverflowLayer");
    if (m_overflowWindow == nullptr || m_overflowPresentBits.empty() ||
        m_overflowPresentSize.cx <= 0 || m_overflowPresentSize.cy <= 0) {
        return;
    }
    POINT origin{};
    LONG caret = m_overflowCaretX;
    if (!OverflowScreenOrigin(origin, caret)) {
        return;
    }
    m_overflowCaretX = caret;
    const LONG width = m_overflowPresentSize.cx;
    const LONG height = m_overflowPresentSize.cy;
    if (!PresentLayeredBits(m_overflowWindow, origin, width, height, m_overflowPresentBits.data(),
            m_overflowPresentBits.size(), m_overflowLayerDib, nullptr, true)) {
        return;
    }
    // Do not call PositionDockSettings here: UpdateLayeredWindow already placed
    // the overflow window, and repositioning Dock Settings on every QS hover
    // previously stormed InvalidateSettingsGlass / full panel rebuilds.
    if (!m_overflowPresentSuppressShow && !IsWindowVisible(m_overflowWindow)) {
        ShowWindow(m_overflowWindow, SW_SHOWNA);
    }
}

void DockApp::BeginOverflowHide(bool animate) noexcept {
    static_cast<void>(animate);
    if (m_overflowVisibility == VisibilityState::Hidden) {
        return;
    }
    FinishOverflowHide();
}

void DockApp::CloseOverflowPopup() noexcept {
    BeginOverflowHide(false);
}

void DockApp::BeginOverflowShow() {
    ProfileScope scope("BeginOverflowShow");
    if (IsOverflowOpen()) {
        return;
    }

    LARGE_INTEGER freq{};
    LARGE_INTEGER t0{};
    LARGE_INTEGER prev{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    prev = t0;
    auto qsOpenMark = [&](const wchar_t* step) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double stepMs =
            (now.QuadPart - prev.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
        const double totalMs =
            (now.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
        wchar_t line[192]{};
        swprintf_s(line, L"QsOpen %-28s step=%6.2fms total=%6.2fms", step, stepMs, totalMs);
        Log(line);
        prev = now;
    };

    HideHoverLabel();
    qsOpenMark(L"HideHoverLabel");
    // First paint must not block on tray WLAN/COM/DDC or a QS cache refill.
    // Brightness DDC stays on its worker; network/audio/power fill is kicked
    // after Present from RefreshQuickSettingsCache / PaintOverflowPopup.
    RefreshBrightnessAsync();
    qsOpenMark(L"RefreshBrightnessAsync");
    m_qsPage = QuickSettingsPage::Home;
    m_qsReturn = QuickSettingsPage::Home;
    m_overflowVisibility = VisibilityState::Visible;

    bool presented = false;
    if (TryPresentOverflowFromCache()) {
        qsOpenMark(L"TryPresentOverflowFromCache");
        presented = true;
        // Cached pixels without a hit map: Settings cog / tiles would silently
        // ignore clicks. Rebuild once (warm glass) rather than leave hits empty.
        if (m_overflowHits.empty()) {
            RebuildOverflowPopup();
            qsOpenMark(L"RebuildOverflowPopup-emptyHits");
            presented = m_overflowWindow != nullptr && !m_overflowPresentBits.empty();
        }
        // Cached Home pixels are already on-screen. Do not QueueOverflowPaint -
        // a full rebuild is still ~100ms even with warm glass and would hitch
        // the cursor one message later. Live meters/temps/network refresh via
        // the usual async workers + PaintOverflowLiveFast.
        if (presented) {
            ArmOverflowLiveWorkers();
            qsOpenMark(L"ArmOverflowLiveWorkers");
            // The kept Home frame may show a stale Bluetooth status (snapshots
            // that land while QS is hidden are not painted). One rect present.
            if (m_qsBtStatusValid) {
                m_qsBtStatusDirty = true;
                PaintOverflowLiveFast();
                qsOpenMark(L"BluetoothStatusLiveFast");
            }
        }
    } else {
        RebuildOverflowPopup();
        qsOpenMark(L"RebuildOverflowPopup");
        presented = m_overflowWindow != nullptr && !m_overflowPresentBits.empty();
        // PaintOverflowPopup already PresentOverflowLayer'd - no second ULW.
    }

    if (!presented) {
        m_overflowVisibility = VisibilityState::Hidden;
        qsOpenMark(L"abort-no-present");
        return;
    }
    InstallOverflowDismissHook();
    qsOpenMark(L"InstallOverflowDismissHook");
    // Soft refresh for radio/ready; periodic CollectPaired only while on BT page.
    m_bluetooth.RequestRefresh(false);
    qsOpenMark(L"BluetoothRequestRefresh");
    if (m_visibility == VisibilityState::Visible) {
        StartTrayTimer();
    }
    qsOpenMark(L"BeginOverflowShow-done");
}

void DockApp::ArmOverflowLiveWorkers() noexcept {
    if (m_overflowWindow == nullptr) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (m_qsCache.stampNetwork == 0 || now - m_qsCache.stampNetwork >= 2500ULL ||
        m_qsCache.stampNight == 0 || now - m_qsCache.stampNight >= 8000ULL) {
        RequestQuickSettingsCacheAsync(m_overflowWindow, kQsCacheResultMessage, m_qsPage);
    }
    if (m_qsPage == QuickSettingsPage::Power) {
        SetTimer(m_overflowWindow, kEnergySampleTimerId, 500, nullptr);
        RequestEnergyAppsAsync(m_overflowWindow, kQsEnergyResultMessage);
    } else {
        KillTimer(m_overflowWindow, kEnergySampleTimerId);
    }
    if (m_qsPage == QuickSettingsPage::Home) {
        SetTimer(m_overflowWindow, kQsTempsTimerId, 500, nullptr);
        RequestQuickSettingsTemps(m_overflowWindow, kQsTempsResultMessage);
    } else {
        KillTimer(m_overflowWindow, kQsTempsTimerId);
    }
    const bool fastMeter = m_qsPage == QuickSettingsPage::Home ||
        m_qsPage == QuickSettingsPage::Microphone;
    SetTimer(m_overflowWindow, kQsLiveTimerId, fastMeter ? 40U : 500U, nullptr);
    EnsureQuickSettingsCaptureAsync();
}

bool DockApp::TryPresentOverflowFromCache() noexcept {
    if (m_overflowWindow == nullptr || m_overflowPresentBits.empty() ||
        m_overflowPresentSize.cx <= 0 || m_overflowPresentSize.cy <= 0 ||
        m_overflowSize.cx != m_overflowPresentSize.cx ||
        m_overflowSize.cy != m_overflowPresentSize.cy) {
        return false;
    }
    POINT origin{};
    LONG caret = m_overflowCaretX;
    if (!OverflowScreenOrigin(origin, caret)) {
        return false;
    }
    m_overflowCaretX = caret;
    PresentOverflowLayer();
    return true;
}

void DockApp::PrewarmOverflowPopup() noexcept {
    if (IsOverflowOpen()) {
        return;
    }
    if (m_overflowWindow != nullptr && !m_overflowPresentBits.empty() &&
        m_overflowPresentSize.cx == m_overflowSize.cx &&
        m_overflowPresentSize.cy == m_overflowSize.cy && m_overflowPresentSize.cx > 0) {
        return;
    }
    ProfileScope scope("PrewarmOverflowPopup");
    Log(L"QsOpen PrewarmOverflowPopup start");
    m_qsPage = QuickSettingsPage::Home;
    m_qsReturn = QuickSettingsPage::Home;
    m_overflowPresentSuppressShow = true;
    m_overflowVisibility = VisibilityState::Visible;
    PaintOverflowPopup();
    m_overflowVisibility = VisibilityState::Hidden;
    m_overflowPresentSuppressShow = false;
    if (m_overflowWindow != nullptr) {
        KillTimer(m_overflowWindow, kEnergySampleTimerId);
        KillTimer(m_overflowWindow, kQsLiveTimerId);
        KillTimer(m_overflowWindow, kQsTempsTimerId);
        ShowWindow(m_overflowWindow, SW_HIDE);
    }
    StopQuickSettingsCapture();
}

void DockApp::DestroyOverflowPopup() noexcept {
    ReleaseLayerPresentDib(m_overflowLayerDib);
    BeginOverflowHide(false);
    m_overflowPaintQueued = false;
    m_overflowIcons.clear();
    m_overflowIconsLoaded = false;
    m_overflowHits.clear();
    InvalidateOverflowGlass();
    DestroyOverflowFonts();
    if (m_overflowWindow != nullptr) {
        DestroyWindow(m_overflowWindow);
        m_overflowWindow = nullptr;
    }
}

bool DockApp::IsDockSettingsOpen() const noexcept {
    return m_settingsWindow != nullptr && IsWindowVisible(m_settingsWindow) != FALSE;
}

bool DockApp::IsCursorOverSettings(POINT cursor) const noexcept {
    if (!IsDockSettingsOpen()) {
        return false;
    }
    RECT bounds{};
    GetWindowRect(m_settingsWindow, &bounds);
    return IsInside(bounds, cursor.x, cursor.y);
}

void DockApp::StartUpdateTimer(UINT delayMs) noexcept {
    if (m_window != nullptr) {
        SetTimer(m_window, kUpdateTimerId, delayMs, nullptr);
    }
}

void DockApp::StopUpdateTimer() noexcept {
    if (m_window != nullptr) {
        KillTimer(m_window, kUpdateTimerId);
    }
}

void DockApp::SetUpdateStatus(const std::wstring& status) {
    m_updateStatus = status;
    Log(L"Update: " + status);
    if (IsDockSettingsOpen()) {
        QueueSettingsPaint();
    }
}

std::optional<int> DockApp::HandOffToNewerInstalledCopy() {
    // Local/dev builds report the CMake project version and would otherwise
    // hand off to the installed release on every launch. Set
    // HOVERDOCK_NO_HANDOFF=1 to run them in place.
    wchar_t noHandoff[8]{};
    if (GetEnvironmentVariableW(L"HOVERDOCK_NO_HANDOFF", noHandoff,
            static_cast<DWORD>(std::size(noHandoff))) != 0) {
        return std::nullopt;
    }
    const std::string current = Updater::CurrentVersion();
    const Updater::InstalledCopy installed = Updater::InstalledCopyInfo();
    if (installed.version.empty() || installed.exePath.empty()) {
        return std::nullopt;
    }
    if (!Updater::IsNewerVersion(installed.version, current)) {
        return std::nullopt;
    }
    const std::wstring currentExe = Updater::CurrentExecutablePath();
    if (currentExe.empty() || Updater::IsSamePath(currentExe, installed.exePath)) {
        return std::nullopt;
    }
    if (GetFileAttributesW(installed.exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return std::nullopt;
    }
    const std::wstring wideCurrent(current.begin(), current.end());
    const std::wstring wideInstalled(installed.version.begin(), installed.version.end());
    Log(L"Installed v" + wideInstalled + L" is newer than running v" + wideCurrent +
        L"; handing off to " + installed.exePath);
    // Release the single-instance lock first so the new copy does not quit on
    // startup; this process exits right after launching it.
    if (m_singleInstanceMutex != nullptr) {
        CloseHandle(m_singleInstanceMutex);
        m_singleInstanceMutex = nullptr;
    }
    const HINSTANCE launched = ShellExecuteW(nullptr, nullptr, installed.exePath.c_str(),
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launched) > 32) {
        return 0;
    }
    Log(L"Hand-off to the installed copy failed; continuing with this copy.");
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
    return std::nullopt;
}

void DockApp::ReconcileLastUpdate() {
    const std::string current = Updater::CurrentVersion();
    const std::wstring record = m_config.LastInstalledVersion();
    if (record.empty()) {
        return;
    }
    // Version strings are ASCII (digits, dots, 'v', pre-release tags).
    std::string narrow;
    narrow.reserve(record.size());
    for (const wchar_t c : record) {
        narrow.push_back(static_cast<char>(c));
    }
    if (!Updater::IsNewerVersion(narrow, current)) {
        // The recorded install took effect (or is stale history): clear it so
        // a future mismatch is judged on its own retry budget.
        m_config.SetLastInstalledVersion(L"", 0);
        m_config.SetLastInstalledAttempts(0);
        static_cast<void>(m_config.Save());
        return;
    }
    // The recorded install is newer than this binary: the replace did not
    // take effect on this copy. Without a reclaim the 24 h reinstall guard
    // stays silent about it; spend one retry from a small budget so the next
    // check re-offers promptly. The budget (not the cooldown) is what still
    // protects against a systematically stale asset looping forever.
    constexpr int kMaxUpdateRetries = 2;
    const std::wstring wideCurrent(current.begin(), current.end());
    const int attempts = m_config.LastInstalledAttempts();
    if (attempts < kMaxUpdateRetries) {
        m_config.SetLastInstalledAttempts(attempts + 1);
        m_config.SetLastInstalledVersion(record, 0);
        static_cast<void>(m_config.Save());
        m_updateStatus = L"Update v" + record + L" didn't take effect (still v" + wideCurrent +
            L") - retrying...";
        Log(L"Last update to v" + record + L" did not take effect; retry " +
            std::to_wstring(attempts + 1));
        return;
    }
    m_updateStatus = L"Update v" + record + L" was installed but this app still reports v" +
        wideCurrent + L". Run the installer from the release page manually.";
    Log(L"Update to v" + record + L" did not take effect after retries; manual install needed.");
}

void DockApp::CheckForUpdatesAsync(bool manual) {
    if (m_updateInFlight.exchange(true)) {
        if (manual) {
            SetUpdateStatus(L"Already checking for updates...");
        }
        return;
    }
    if (manual) {
        SetUpdateStatus(L"Checking for updates...");
    }
    m_lastUpdateCheck = QpcSeconds();
    // Snapshot the reinstall-loop guard on the UI thread (DockConfig is not
    // thread-safe): version + time of the last launched install.
    const std::wstring lastInstalledVersion = m_config.LastInstalledVersion();
    const long long lastInstalledTime = m_config.LastInstalledTime();
    const long long checkTime = static_cast<long long>(std::time(nullptr));
    const HWND replyWindow = m_window;
    std::thread([this, replyWindow, manual, lastInstalledVersion, lastInstalledTime,
        checkTime] {
        auto postReply = [replyWindow](UpdateReply reply) {
            auto* owned = new UpdateReply(std::move(reply));
            if (replyWindow == nullptr ||
                PostMessageW(replyWindow, kUpdateResultMessage, 0,
                    reinterpret_cast<LPARAM>(owned)) == FALSE) {
                delete owned;
            }
        };

        Updater::ReleaseInfo release;
        std::wstring error;
        if (!Updater::FetchLatestRelease(release, error)) {
            UpdateReply reply;
            reply.finished = true;
            if (manual) {
                reply.status = error.empty() ? L"Update check failed."
                                             : L"Update check failed: " + error;
            } else {
                // Quiet background failure: keep the last known status, log only.
                Log(error.empty() ? L"Background update check failed."
                                  : L"Background update check failed: " + error);
                m_updateInFlight.store(false);
                return;
            }
            postReply(std::move(reply));
            return;
        }

        if (!release.hasUpdate) {
            UpdateReply reply;
            reply.finished = true;
            std::string current = Updater::CurrentVersion();
            std::wstring wide(current.begin(), current.end());
            reply.status = L"You are up to date (v" + wide + L").";
            postReply(std::move(reply));
            return;
        }

        // Reinstall-loop guard: this exact version already had its
        // installer/mover launched recently but the running build still
        // reports an older version, so the install did not take effect
        // (portable vs installed location, locked file, ...). Do not
        // download + reinstall it again; that is the observed infinite loop.
        constexpr long long kReinstallCooldownSeconds = 24LL * 60LL * 60LL;
        const std::string version = release.version;
        const std::wstring wideVersion(version.begin(), version.end());
        if (!wideVersion.empty() && wideVersion == lastInstalledVersion &&
            lastInstalledTime > 0 && checkTime >= lastInstalledTime &&
            checkTime - lastInstalledTime < kReinstallCooldownSeconds) {
            if (manual) {
                UpdateReply reply;
                reply.finished = true;
                std::string current = Updater::CurrentVersion();
                std::wstring wideCurrent(current.begin(), current.end());
                reply.status = L"Update v" + wideVersion +
                    L" was already installed but this app still reports v" + wideCurrent +
                    L". Run the installer from the release page manually.";
                postReply(std::move(reply));
            } else {
                Log(L"Background update check skipped: v" + wideVersion +
                    L" was installed recently but did not take effect.");
                m_updateInFlight.store(false);
            }
            return;
        }

        // An update is available: automatically download and stage the install.
        // The UI thread performs the final launch + exit so file locks are clean.
        {
            UpdateReply downloading;
            downloading.finished = false;
            downloading.status = L"Downloading update v" + wideVersion + L"...";
            postReply(downloading);
        }

        // Installed copies update via the Setup installer; portable copies
        // self-replace in place (see Updater::SelectAssetUrls).
        std::string url;
        bool isSetup = true;
        Updater::SelectAssetUrls(release, url, isSetup);
        if (url.empty()) {
            UpdateReply reply;
            reply.finished = true;
            reply.status = L"Release has no downloadable .exe asset.";
            postReply(std::move(reply));
            return;
        }
        std::wstring dest = Updater::DefaultDownloadPath(version, isSetup);
        bool downloaded = Updater::DownloadFile(url, dest, error);
        if (!downloaded && isSetup && !release.exeUrl.empty()) {
            // The fallback feed synthesizes conventional asset names; if the
            // Setup name drifts (or the installer asset is missing), retry the
            // portable Dock.exe before surfacing a download failure.
            error.clear();
            url = release.exeUrl;
            isSetup = false;
            dest = Updater::DefaultDownloadPath(version, isSetup);
            downloaded = Updater::DownloadFile(url, dest, error);
        }
        if (!downloaded) {
            UpdateReply reply;
            reply.finished = true;
            reply.status = error.empty() ? L"Update download failed."
                                         : L"Update download failed: " + error;
            postReply(std::move(reply));
            return;
        }

        UpdateReply ready;
        ready.finished = false;
        ready.readyToInstall = true;
        ready.isSetup = isSetup;
        ready.path = dest;
        ready.version = version;
        ready.status = L"Installing update v" + wideVersion + L"...";
        postReply(std::move(ready));
    }).detach();
}

void DockApp::ApplyUpdateResult(const UpdateReply& reply) {
    if (!reply.status.empty()) {
        SetUpdateStatus(reply.status);
    }
    if (reply.readyToInstall) {
        if (m_updateInstalling.exchange(true)) {
            m_updateInFlight.store(false);
            return;
        }
        // Defensive: a stale queued reply must never reinstall the running
        // (or an older) version over a newer build.
        if (!Updater::IsNewerVersion(reply.version, Updater::CurrentVersion())) {
            m_updateInstalling.store(false);
            m_updateInFlight.store(false);
            std::string current = Updater::CurrentVersion();
            std::wstring wide(current.begin(), current.end());
            SetUpdateStatus(L"You are up to date (v" + wide + L").");
            return;
        }
        bool launched = false;
        if (reply.isSetup) {
            launched = Updater::LaunchInstallerAndExit(reply.path);
        } else {
            launched = Updater::StagePortableUpdateAndRestart(reply.path);
        }
        if (launched) {
            std::wstring wide(reply.version.begin(), reply.version.end());
            // Record the launched install synchronously (not via the deferred
            // config-save timer, which would not fire before WM_CLOSE exits):
            // if the new process still reports the old version, the next
            // check skips this version for a cooldown instead of looping.
            // A different version restarts the retry budget; re-launching the
            // same one keeps the attempts spent so far.
            if (m_config.LastInstalledVersion() != wide) {
                m_config.SetLastInstalledAttempts(0);
            }
            m_config.SetLastInstalledVersion(
                wide, static_cast<long long>(std::time(nullptr)));
            static_cast<void>(m_config.Save());
            SetUpdateStatus(L"Restarting into v" + wide + L"...");
            Log(L"Update installer launched; exiting for replace.");
            // Give the status paint a beat, then exit cleanly so the
            // installer / mover can replace Dock.exe and reopen the build.
            if (m_window != nullptr) {
                PostMessageW(m_window, WM_CLOSE, 0, 0);
            }
            return;
        }
        m_updateInstalling.store(false);
        m_updateInFlight.store(false);
        SetUpdateStatus(L"Update install did not start. Try again.");
        return;
    }
    if (reply.finished) {
        m_updateInFlight.store(false);
    }
}

LRESULT CALLBACK DockApp::DockSettingsProcedure(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam) {
    DockApp* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<DockApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<DockApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    switch (message) {
    case WM_DESTROY:
        if (app != nullptr && app->m_settingsWindow == window) {
            app->m_settingsWindow = nullptr;
            app->m_settingsHits.clear();
            app->m_settingsHover = -1;
            app->m_settingsPaintQueued = false;
            app->m_settingsHoverPaintOnly = false;
            std::vector<uint8_t>().swap(app->m_settingsBaseBits);
            std::vector<uint8_t>().swap(app->m_settingsPresentBits);
            app->m_settingsPresentSize = {};
            app->InvalidateSettingsGlass();
            app->ReleaseLayerPresentDib(app->m_settingsLayerDib);
        }
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN: {
        if (app == nullptr) {
            break;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (PtInRect(&app->m_frostSliderTrack, point)) {
            SetCapture(window);
            app->m_frostSliderDragging = true;
            app->ApplyFrostSliderAt(point.x);
            return 0;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (app == nullptr) {
            break;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (app->m_frostSliderDragging) {
            app->ApplyFrostSliderAt(point.x);
            return 0;
        }
        const int hover = app->SettingsHitIndex(point);
        if (hover != app->m_settingsHover) {
            app->m_settingsHover = hover;
            app->QueueSettingsPaint(true);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        if (app == nullptr) {
            break;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (app->m_frostSliderDragging) {
            app->m_frostSliderDragging = false;
            ReleaseCapture();
            app->ApplyFrostSliderAt(point.x);
            app->ScheduleConfigSave();
            return 0;
        }
        const int hit = app->SettingsHitIndex(point);
        if (hit >= 0 && static_cast<size_t>(hit) < app->m_settingsHits.size()) {
            // Frost is a drag slider (handled above); ignore click toggles.
            if (app->m_settingsHits[static_cast<size_t>(hit)].kind == SettingsHitKind::Frost) {
                return 0;
            }
            app->HandleSettingsClick(app->m_settingsHits[static_cast<size_t>(hit)], message);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (app != nullptr && app->m_settingsHover >= 0) {
            app->m_settingsHover = -1;
            app->QueueSettingsPaint(true);
        }
        return 0;

    case WM_SETCURSOR:
        if (app != nullptr) {
            POINT cursor{};
            if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor)) {
                const int hit = app->SettingsHitIndex(cursor);
                if (hit >= 0 &&
                    app->m_settingsHits[static_cast<size_t>(hit)].kind == SettingsHitKind::PerfLog) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
        }
        break;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void DockApp::RefreshSettingsControls() {
    // State lives in m_config / m_updateStatus; the glass panel reads it at
    // paint time, so a refresh is just a repaint when open.
    if (IsDockSettingsOpen()) {
        QueueSettingsPaint();
    }
}

int DockApp::SettingsHitIndex(POINT point) const noexcept {
    for (size_t index = 0; index < m_settingsHits.size(); ++index) {
        if (IsInside(m_settingsHits[index].bounds, point.x, point.y)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool DockApp::SettingsScreenOrigin(POINT& origin) const noexcept {
    if (m_window == nullptr || m_settingsSize.cx <= 0 || m_settingsSize.cy <= 0) {
        return false;
    }
    const UINT dpi = HostDpi();
    const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
    const LONG gap = std::max(8L, std::lround(10.0F * scale));
    const LONG margin = std::max(8L, std::lround(8.0F * scale));
    if (IsOverflowOpen()) {
        RECT quick{};
        GetWindowRect(m_overflowWindow, &quick);
        const LONG width = m_settingsSize.cx;
        const LONG height = m_settingsSize.cy;
        LONG x = quick.right + gap;
        LONG y = quick.top;
        // Prefer the right side; fall back to the left when there is no room.
        if (x + width > m_hostBounds.right - margin &&
            quick.left - gap - width >= m_hostBounds.left + margin) {
            x = quick.left - gap - width;
        }
        const LONG minX = m_hostBounds.left + margin;
        const LONG maxX = m_hostBounds.right - width - margin;
        if (maxX >= minX) {
            x = std::clamp(x, minX, maxX);
        } else {
            x = minX;
        }
        const LONG minY = m_hostBounds.top + margin;
        const LONG maxY = m_hostBounds.bottom - height - margin;
        if (maxY >= minY) {
            y = std::clamp(y, minY, maxY);
        } else {
            y = minY;
        }
        origin = {x, y};
        return true;
    }
    // Fallback when Quick Settings is closed: center above the dock.
    const LONG width = m_settingsSize.cx;
    const LONG height = m_settingsSize.cy;
    const LONG dockCenter = m_windowX + static_cast<LONG>(m_dockWidth) / 2L;
    LONG x = dockCenter - width / 2L;
    LONG y = m_currentY - gap - height;
    const LONG minX = m_hostBounds.left + margin;
    const LONG maxX = m_hostBounds.right - width - margin;
    if (maxX >= minX) {
        x = std::clamp(x, minX, maxX);
    } else {
        x = minX;
    }
    y = std::max(m_hostBounds.top + margin, y);
    origin = {x, y};
    return true;
}

void DockApp::InvalidateSettingsGlass() noexcept {
    std::vector<uint8_t>().swap(m_settingsGlass);
    m_settingsGlassSize = {};
    m_settingsGlassOrigin = {};
    m_settingsGlassFrost = -1.0F;
}

bool DockApp::SettingsGlassValid(POINT origin) const noexcept {
    return !m_settingsGlass.empty() && m_settingsGlassSize.cx == m_settingsSize.cx &&
        m_settingsGlassSize.cy == m_settingsSize.cy && m_settingsGlassOrigin.x == origin.x &&
        m_settingsGlassOrigin.y == origin.y &&
        m_settingsGlass.size() ==
            static_cast<size_t>(m_settingsSize.cx) * static_cast<size_t>(m_settingsSize.cy) * 4U &&
        std::abs(m_settingsGlassFrost - m_config.FrostAmount()) < 0.001F;
}

void DockApp::QueueSettingsPaint(bool hoverOnly) {
    // Coalesce rapid hover transitions into a single repaint, mirroring the
    // Quick Settings popup path. Hover moves only need the cheap base-copy +
    // highlight overlay; full repaints rebuild the cached base frame.
    if (m_settingsWindow == nullptr) {
        return;
    }
    if (!hoverOnly) {
        m_settingsHoverPaintOnly = false;
    } else if (!m_settingsPaintQueued) {
        m_settingsHoverPaintOnly = true;
    }
    if (m_settingsPaintQueued) {
        return;
    }
    m_settingsPaintQueued = true;
    PostMessageW(m_window, kSettingsPaintMessage, 0, 0);
}

void DockApp::ApplySettingsHoverHighlight(uint8_t* pixels, int width, int height,
    const SettingsHit& hit) const {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    switch (hit.kind) {
    case SettingsHitKind::Close: {
        // Squircle behind the X, centered on the glyph rather than the wide hit.
        const UINT dpi = HostDpi();
        const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
        const float closeExtent = static_cast<float>(std::max(22L, std::lround(24.0F * scale)));
        const float cx = static_cast<float>(hit.bounds.right) - closeExtent * 0.5F - 4.0F;
        const float cy = 0.5F * static_cast<float>(hit.bounds.top + hit.bounds.bottom);
        const LONG side = std::max(8L, std::lround(closeExtent * 1.24F));
        const LONG left = std::lround(cx) - side / 2L;
        const LONG top = std::lround(cy) - side / 2L;
        const RECT bubble{left, top, left + side, top + side};
        FillSquirclePremul(pixels, width, height, bubble, static_cast<float>(side) * 0.5F, 0.55F);
        break;
    }
    case SettingsHitKind::CheckNow:
    case SettingsHitKind::PerfProfile:
    case SettingsHitKind::PerfLog:
    case SettingsHitKind::Startup:
    case SettingsHitKind::Updates:
    case SettingsHitKind::RimLight:
    case SettingsHitKind::Lensing:
    case SettingsHitKind::Dispersion:
    case SettingsHitKind::Frost:
    case SettingsHitKind::Specular:
    case SettingsHitKind::DropShadow:
    case SettingsHitKind::DepthShade:
    case SettingsHitKind::LightPanels: {
        const float scale = static_cast<float>(HostDpi() == 0 ? 96U : HostDpi()) / 96.0F;
        const float wash = (hit.kind == SettingsHitKind::CheckNow ||
                               hit.kind == SettingsHitKind::PerfProfile)
            ? 0.14F
            : 0.10F;
        FillSquirclePremul(pixels, width, height, hit.bounds, ContentSquircleRadius(hit.bounds, scale),
            wash);
        break;
    }
    case SettingsHitKind::None:
        break;
    }
}

void DockApp::PresentSettingsLayer() noexcept {
    if (m_settingsWindow == nullptr || m_settingsPresentBits.empty() ||
        m_settingsPresentSize.cx <= 0 || m_settingsPresentSize.cy <= 0) {
        return;
    }
    POINT origin{};
    if (!SettingsScreenOrigin(origin)) {
        return;
    }
    const LONG width = m_settingsPresentSize.cx;
    const LONG height = m_settingsPresentSize.cy;
    if (!PresentLayeredBits(m_settingsWindow, origin, width, height, m_settingsPresentBits.data(),
            m_settingsPresentBits.size(), m_settingsLayerDib)) {
        return;
    }
    // Hover/glass presents must not PositionDockSettings(): that path can
    // InvalidateSettingsGlass on origin jitter and pays SetWindowPos every time.
    // OpenDockSettings / dock-move handlers position explicitly.
    if (!IsWindowVisible(m_settingsWindow)) {
        ShowWindow(m_settingsWindow, SW_SHOWNA);
    }
}

void DockApp::PaintSettingsHoverFast() {
    POINT origin{};
    if (m_settingsBaseBits.empty() || m_settingsWindow == nullptr ||
        m_settingsPresentSize.cx <= 0 || m_settingsPresentSize.cy <= 0 ||
        m_settingsPresentSize.cx != m_settingsSize.cx ||
        m_settingsPresentSize.cy != m_settingsSize.cy ||
        !SettingsScreenOrigin(origin) || !SettingsGlassValid(origin)) {
        PaintSettingsPopup();
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const int width = SaturatedInt(m_settingsPresentSize.cx);
    const int height = SaturatedInt(m_settingsPresentSize.cy);
    m_settingsPresentSize = m_settingsSize;
    if (m_settingsPresentBits.size() != m_settingsBaseBits.size()) {
        m_settingsPresentBits = m_settingsBaseBits;
        m_settingsHoverDirtyValid = false;
    }

    auto restoreRect = [&](const RECT& rect) {
        const LONG left = (std::max)(0L, rect.left);
        const LONG top = (std::max)(0L, rect.top);
        const LONG right = (std::min)(static_cast<LONG>(width), rect.right);
        const LONG bottom = (std::min)(static_cast<LONG>(height), rect.bottom);
        if (right <= left || bottom <= top) {
            return;
        }
        const size_t rowBytes = static_cast<size_t>(right - left) * 4U;
        for (LONG y = top; y < bottom; ++y) {
            const size_t offset =
                (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(left)) * 4U;
            std::memcpy(m_settingsPresentBits.data() + offset, m_settingsBaseBits.data() + offset,
                rowBytes);
        }
    };

    RECT dirty{};
    bool haveDirty = false;
    if (m_settingsHoverDirtyValid) {
        // Undo previous highlight from base (not full-panel memcpy).
        RECT prev = m_settingsHoverDirty;
        prev.left = (std::max)(0L, prev.left - 8);
        prev.top = (std::max)(0L, prev.top - 8);
        prev.right = (std::min)(static_cast<LONG>(width), prev.right + 8);
        prev.bottom = (std::min)(static_cast<LONG>(height), prev.bottom + 8);
        restoreRect(prev);
        dirty = prev;
        haveDirty = true;
    }

    if (m_settingsHover >= 0 && static_cast<size_t>(m_settingsHover) < m_settingsHits.size()) {
        const RECT& hit = m_settingsHits[static_cast<size_t>(m_settingsHover)].bounds;
        ApplySettingsHoverHighlight(m_settingsPresentBits.data(), width, height,
            m_settingsHits[static_cast<size_t>(m_settingsHover)]);
        RECT next = hit;
        next.left = (std::max)(0L, next.left - 8);
        next.top = (std::max)(0L, next.top - 8);
        next.right = (std::min)(static_cast<LONG>(width), next.right + 8);
        next.bottom = (std::min)(static_cast<LONG>(height), next.bottom + 8);
        if (!haveDirty) {
            dirty = next;
            haveDirty = true;
        } else {
            dirty.left = (std::min)(dirty.left, next.left);
            dirty.top = (std::min)(dirty.top, next.top);
            dirty.right = (std::max)(dirty.right, next.right);
            dirty.bottom = (std::max)(dirty.bottom, next.bottom);
        }
        m_settingsHoverDirty = hit;
        m_settingsHoverDirtyValid = true;
    } else {
        m_settingsHoverDirtyValid = false;
    }

    // PresentLayeredBits still uploads the full DIB to the pooled bitmap; dirty
    // only shrinks the UpdateLayeredWindowIndirect composition region.
    if (!PresentLayeredBits(m_settingsWindow, origin, m_settingsPresentSize.cx,
            m_settingsPresentSize.cy, m_settingsPresentBits.data(), m_settingsPresentBits.size(),
            m_settingsLayerDib, haveDirty ? &dirty : nullptr)) {
        return;
    }
    m_lastSettingsHoverPresentMs = now;
    if (!IsWindowVisible(m_settingsWindow)) {
        ShowWindow(m_settingsWindow, SW_SHOWNA);
    }
}

void DockApp::RebuildSettingsPopup() {
    m_settingsHover = -1;
    m_settingsHoverPaintOnly = false;
    std::vector<uint8_t>().swap(m_settingsBaseBits);
    std::vector<uint8_t>().swap(m_settingsPresentBits);
    m_settingsPresentSize = {};
    InvalidateSettingsGlass();
    PaintSettingsPopup();
}


UINT DockApp::PackPopupGlassFxFlags(bool dockFace, bool popupShadow) const noexcept
{
    // Same packing as the dock frame (frost<<16 | halo<<8 | fx), plus PANEL so
    // GlassPS fills the plate without the dock shadow margin. Halo slots stay 0
    // (no icons on the menu plate).
    const float frostAmount = m_config.FrostAmount();
    UINT glassFx = DOCK_FX_PANEL | DOCK_FX_TINT;
    if (dockFace) {
        glassFx |= DOCK_FX_DOCK_FACE;
    }
    if (m_config.RimLight()) {
        glassFx |= DOCK_FX_RIM;
    }
    if (m_config.Lensing()) {
        glassFx |= DOCK_FX_LENS;
    }
    if (m_config.Dispersion()) {
        glassFx |= DOCK_FX_DISPERSION;
    }
    // Panels always run the mica stack (Concept A strong backdrop blur) so
    // busy wallpaper detail dissolves under glyphs even at clear Frost.
    glassFx |= DOCK_FX_BLUR;
    if (m_config.Specular()) {
        glassFx |= DOCK_FX_SPECULAR;
    }
    if (m_config.DropShadow()) {
        glassFx |= DOCK_FX_SHADOW;
    }
    if (m_config.DepthShade()) {
        glassFx |= DOCK_FX_THICKNESS;
    }
    if (popupShadow) {
        glassFx |= DOCK_FX_POPUP_SHADOW;
    }
    constexpr UINT haloSlots = 0u;
    const UINT frostByte =
        static_cast<UINT>(std::lround(std::clamp(frostAmount, 0.0f, 1.0f) * 255.0f));
    return (frostByte << 16) | (haloSlots << 8) | glassFx;
}


bool DockApp::ApplyLivePopupGlass(std::vector<uint8_t>& glassBits, SIZE size,
    std::vector<uint8_t>& cachedGlass, std::vector<uint8_t>& baseBits,
    std::vector<uint8_t>& presentBits, SIZE& presentSize)
{
    const size_t bytes = static_cast<size_t>(size.cx) * static_cast<size_t>(size.cy) * 4U;
    if (glassBits.size() != bytes || cachedGlass.size() != bytes || baseBits.size() != bytes) {
        return false;
    }
    if (std::memcmp(glassBits.data(), cachedGlass.data(), bytes) == 0) {
        // Identical glass bake: skip merge + UpdateLayeredWindow present.
        return false;
    }
    auto* oldGlass = reinterpret_cast<uint32_t*>(cachedGlass.data());
    auto* newGlass = reinterpret_cast<uint32_t*>(glassBits.data());
    auto* base = reinterpret_cast<uint32_t*>(baseBits.data());
    for (size_t i = 0, n = bytes / 4U; i < n; ++i) {
        if (base[i] == oldGlass[i]) {
            base[i] = newGlass[i];
        }
    }
    cachedGlass.swap(glassBits);
    presentBits = baseBits;
    presentSize = size;
    return true;
}

bool DockApp::TickLivePopupGlass()
{
    if (!m_rendererInitialized || m_frostSliderDragging) {
        return false;
    }

    const bool settingsOpen = IsDockSettingsOpen();
    const bool overflowOpen = IsOverflowOpen();
    const bool contextOpen = IsContextMenuOpen();
    if (!settingsOpen && !overflowOpen && !contextOpen) {
        if (m_livePopupGlassPending >= 0) {
            m_renderer.CancelLiveGlassPanelBake();
            m_livePopupGlassPending = -1;
        }
        m_livePopupGlassStatic = false;
        m_livePopupBackdropSerial = 0;
        return false;
    }

    bool didWork = false;

    std::vector<uint8_t> glass;
    int done = -1;
    while (m_renderer.TakeLiveGlassPanelResult(glass, &done)) {
        if (done == 0 && settingsOpen && !m_settingsBaseBits.empty()) {
            POINT origin{};
            if (SettingsScreenOrigin(origin) &&
                glass.size() == m_settingsGlass.size() &&
                m_settingsGlassSize.cx == m_settingsSize.cx &&
                m_settingsGlassSize.cy == m_settingsSize.cy) {
                if (ApplyLivePopupGlass(glass, m_settingsSize, m_settingsGlass, m_settingsBaseBits,
                        m_settingsPresentBits, m_settingsPresentSize)) {
                    m_settingsLayerDib.contentValid = false;
                    m_settingsHoverDirtyValid = false;
                    PaintSettingsHoverFast();
                    didWork = true;
                } else if (m_settingsHover >= 0 || m_settingsHoverDirtyValid) {
                    PaintSettingsHoverFast();
                }
            }
        } else if (done == 1 && overflowOpen && !m_overflowBaseBits.empty()) {
            if (glass.size() == m_overflowGlass.size() &&
                m_overflowGlassSize.cx == m_overflowSize.cx &&
                m_overflowGlassSize.cy == m_overflowSize.cy) {
                if (ApplyLivePopupGlass(glass, m_overflowSize, m_overflowGlass, m_overflowBaseBits,
                        m_overflowPresentBits, m_overflowPresentSize)) {
                    m_overflowLayerDib.contentValid = false;
                    m_overflowHoverDirtyValid = false;
                    PaintOverflowHoverFast();
                    didWork = true;
                } else if (m_overflowHover >= 0 || m_overflowHoverDirtyValid) {
                    PaintOverflowHoverFast();
                }
            }
        } else if (done == 2 && contextOpen && !m_contextBaseBits.empty()) {
            if (glass.size() == m_contextGlass.size() &&
                m_contextGlassSize.cx == m_contextSize.cx &&
                m_contextGlassSize.cy == m_contextSize.cy) {
                if (ApplyLivePopupGlass(glass, m_contextSize, m_contextGlass, m_contextBaseBits,
                        m_contextPresentBits, m_contextPresentSize)) {
                    m_contextLayerDib.contentValid = false;
                    m_contextHoverDirtyValid = false;
                    PaintContextHoverFast();
                    didWork = true;
                } else if (m_contextHover >= 0 || m_contextHoverDirtyValid) {
                    PaintContextHoverFast();
                }
            }
        }
    }

    if (!settingsOpen) {
        m_popupGlassDirty[0] = false;
    }
    if (!overflowOpen) {
        m_popupGlassDirty[1] = false;
    }
    if (!contextOpen) {
        m_popupGlassDirty[2] = false;
    }

    const float frostAmount = m_config.FrostAmount();
    const float settingsAlpha = DOCK_QS_SETTINGS_GLASS_ALPHA;
    const float contextAlpha =
        DOCK_PANEL_GLASS_ALPHA + (1.0f - DOCK_PANEL_GLASS_ALPHA) * frostAmount;
    const float dpiScale = static_cast<float>(HostDpi()) / 96.0F;
    const UINT dockFaceFlags = PackPopupGlassFxFlags(true);
    const UINT contextFlags = PackPopupGlassFxFlags(false);

    for (int target = 0; target < 3; ++target) {
        const UINT fxFlags = target == 1 ? PackPopupGlassFxFlags(true, true)
            : (target == 2 ? contextFlags : dockFaceFlags);
        const float glassAlpha = target == 2 ? contextAlpha : settingsAlpha;
        POINT origin{};
        LONG width = 0;
        LONG height = 0;
        bool ready = false;
        if (target == 0 && settingsOpen && !m_settingsGlass.empty() && !m_settingsBaseBits.empty()) {
            if (SettingsScreenOrigin(origin) && SettingsGlassValid(origin)) {
                width = m_settingsSize.cx;
                height = m_settingsSize.cy;
                ready = width > 0 && height > 0;
            }
        } else if (target == 1 && overflowOpen && !m_overflowGlass.empty() &&
            !m_overflowBaseBits.empty()) {
            LONG caret = 0;
            if (OverflowScreenOrigin(origin, caret) && OverflowGlassValid(origin)) {
                width = m_overflowSize.cx;
                height = m_overflowSize.cy;
                ready = width > 0 && height > 0;
            }
        } else if (target == 2 && contextOpen && !m_contextGlass.empty() &&
            !m_contextBaseBits.empty()) {
            if (ContextScreenOrigin(origin) && ContextGlassValid(origin)) {
                width = m_contextSize.cx;
                height = m_contextSize.cy;
                ready = width > 0 && height > 0;
            }
        }
        if (!ready) {
            m_popupGlassDirty[target] = false;
            continue;
        }
        const RECT screenRect{origin.x, origin.y, origin.x + width, origin.y + height};
        if (m_renderer.DesktopRegionChanged(screenRect)) {
            m_popupGlassDirty[target] = true;
        }
        if (!m_popupGlassDirty[target]) {
            continue;
        }
        bool unchanged = false;
        if (m_renderer.BeginLiveGlassPanelBake(screenRect, static_cast<UINT>(width),
                static_cast<UINT>(height), fxFlags, glassAlpha, dpiScale, m_settingsWindow,
                m_overflowWindow, m_contextWindow, target != 2 && m_config.LightPanels(), target,
                &unchanged)) {
            m_popupGlassDirty[target] = false;
            m_lastPopupGlassRefreshMs = GetTickCount64();
            didWork = true;
        } else if (unchanged) {
            m_popupGlassDirty[target] = false;
        } else {
            didWork = true;
        }
    }
    return didWork;
}

bool DockApp::TryBakePopupGlass(POINT origin, LONG width, LONG height, uint8_t* pixels,
    size_t byteCount, bool dockFace, bool popupShadow)
{
    if (!m_rendererInitialized || pixels == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (byteCount < static_cast<size_t>(width) * static_cast<size_t>(height) * 4U) {
        return false;
    }
    const RECT screenRect{origin.x, origin.y, origin.x + width, origin.y + height};
    const float frostAmount = m_config.FrostAmount();
    const float glassAlpha = dockFace ? DOCK_QS_SETTINGS_GLASS_ALPHA
                                      : DOCK_PANEL_GLASS_ALPHA +
            (1.0f - DOCK_PANEL_GLASS_ALPHA) * frostAmount;
    const float dpiScale = static_cast<float>(HostDpi()) / 96.0F;
    std::vector<uint8_t> glass;
    if (!m_renderer.BakeGlassPanel(screenRect, static_cast<UINT>(width), static_cast<UINT>(height),
            PackPopupGlassFxFlags(dockFace, popupShadow), glassAlpha, dpiScale, m_settingsWindow,
            m_overflowWindow, m_contextWindow, dockFace && m_config.LightPanels(), glass)) {
        return false;
    }
    if (glass.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4U) {
        return false;
    }
    std::memcpy(pixels, glass.data(), glass.size());
    return true;
}

void DockApp::ApplyFrostSliderAt(LONG clientX) {
    const LONG left = m_frostSliderTrack.left;
    const LONG right = m_frostSliderTrack.right;
    const LONG span = std::max(1L, right - left);
    float amount = std::clamp(static_cast<float>(clientX - left) / static_cast<float>(span), 0.0F, 1.0F);
    // Magnetic snap to clear (0), milky plate (0.5), full frost (1).
    constexpr float kFrostSnapThreshold = 0.06F;
    constexpr float kFrostSnaps[3] = {0.0F, 0.5F, 1.0F};
    for (float snap : kFrostSnaps) {
        if (std::abs(amount - snap) < kFrostSnapThreshold) {
            amount = snap;
            break;
        }
    }
    if (std::abs(amount - m_config.FrostAmount()) < 0.001F && !m_frostSliderDragging) {
        return;
    }
    m_config.SetFrostAmount(amount);
    if (m_frostSliderDragging) {
        const ULONGLONG now = GetTickCount64();
        // ~8ms cadence with non-blocking GPU waits (skipIfGpuBusy on busy frames).
        if (now - m_frostSliderLastRenderMs >= 8ULL) {
            m_frostSliderLastRenderMs = now;
            InvalidateSettingsGlass();
            InvalidateOverflowGlass();
            InvalidateContextGlass();
            QueueSettingsPaint();
            QueueOverflowPaint();
            QueueRenderFrame(false);
        } else {
            QueueSettingsPaint();
        }
        return;
    }
    // Slider released: queue full GPU GlassPS bake (do not sync-block click/hook).
    InvalidateSettingsGlass();
    InvalidateOverflowGlass();
    InvalidateContextGlass();
    QueueSettingsPaint();
    if (m_overflowWindow != nullptr && IsWindowVisible(m_overflowWindow)) {
        QueueOverflowPaint();
    }
    QueueRenderFrame();
}
void DockApp::HandleSettingsClick(const SettingsHit& hit, UINT message) {
    (void)message;
    switch (hit.kind) {
    case SettingsHitKind::None:
        break;
    case SettingsHitKind::Close:
        CloseDockSettings();
        break;
    case SettingsHitKind::Startup: {
        const bool enabled = !m_config.LaunchAtStartup();
        m_config.SetLaunchAtStartup(enabled);
        if (!Startup::SetEnabled(enabled)) {
            Log(L"Startup registry update failed.");
        }
        ScheduleConfigSave();
        QueueSettingsPaint();
        break;
    }
    case SettingsHitKind::Updates: {
        const bool enabled = !m_config.CheckForUpdates();
        m_config.SetCheckForUpdates(enabled);
        ScheduleConfigSave();
        if (!enabled) {
            std::string current = Updater::CurrentVersion();
            std::wstring wide(current.begin(), current.end());
            SetUpdateStatus(L"Version " + wide + L" - automatic updates off.");
        } else {
            QueueSettingsPaint();
            if (!m_updateInFlight.load()) {
                CheckForUpdatesAsync(false);
            }
        }
        break;
    }
    case SettingsHitKind::RimLight: {
        const bool enabled = !m_config.RimLight();
        m_config.SetRimLight(enabled);
        ScheduleConfigSave();
        QueueSettingsPaint();
        QueueRenderFrame();
        break;
    }
    case SettingsHitKind::Lensing: {
        const bool enabled = !m_config.Lensing();
        m_config.SetLensing(enabled);
        ScheduleConfigSave();
        QueueSettingsPaint();
        QueueRenderFrame();
        break;
    }
    case SettingsHitKind::Dispersion: {
        const bool enabled = !m_config.Dispersion();
        m_config.SetDispersion(enabled);
        ScheduleConfigSave();
        QueueSettingsPaint();
        QueueRenderFrame();
        break;
    }
    case SettingsHitKind::Frost: {
        // Click/drag on the frost slider sets the amount from the cursor X
        // within the track (0 = clear glass, 1 = full mica).
        break;
    }
    case SettingsHitKind::Specular: {
        const bool enabled = !m_config.Specular();
        m_config.SetSpecular(enabled);
        ScheduleConfigSave();
        QueueSettingsPaint();
        QueueRenderFrame();
        break;
    }
    case SettingsHitKind::DropShadow: {
        const bool enabled = !m_config.DropShadow();
        m_config.SetDropShadow(enabled);
        ScheduleConfigSave();
        QueueSettingsPaint();
        QueueRenderFrame();
        break;
    }
    case SettingsHitKind::DepthShade: {
        const bool enabled = !m_config.DepthShade();
        m_config.SetDepthShade(enabled);
        ScheduleConfigSave();
        QueueSettingsPaint();
        QueueRenderFrame();
        break;
    }
    case SettingsHitKind::LightPanels: {
        m_config.SetLightPanels(!m_config.LightPanels());
        ScheduleConfigSave();
        InvalidateSettingsGlass();
        InvalidateOverflowGlass();
        QueueSettingsPaint();
        if (IsOverflowOpen()) {
            QueueOverflowPaint();
        }
        break;
    }
    case SettingsHitKind::CheckNow:
        if (!m_updateInFlight.load() && !m_updateInstalling.load()) {
            CheckForUpdatesAsync(true);
        } else {
            SetUpdateStatus(L"Already checking for updates...");
        }
        break;
    case SettingsHitKind::PerfProfile: {
        if (m_perfProfiler.IsRunning()) {
            m_perfProfiler.Stop();
            const std::wstring logPath = m_perfProfiler.LogPath();
            if (!logPath.empty()) {
                m_perfSavedLog = logPath;
                m_perfStatus = L"Saved " + logPath;
                Log(L"Performance profile saved: " + logPath);
            } else {
                m_perfSavedLog.clear();
                m_perfStatus = L"Profiling stopped.";
            }
        } else {
            m_perfSavedLog.clear();
            if (m_perfProfiler.Start()) {
                m_perfStatus = L"Profiling... " + m_perfProfiler.LogPath();
                Log(L"Performance profile started: " + m_perfProfiler.LogPath());
            } else {
                m_perfStatus = L"Could not start performance profile.";
                Log(L"Performance profile failed to start.");
            }
        }
        QueueSettingsPaint();
        break;
    }
    case SettingsHitKind::PerfLog: {
        if (m_perfSavedLog.empty()) {
            break;
        }
        const std::wstring logPath = m_perfSavedLog;
        std::thread([logPath]() {
            const std::wstring parameters = L"/select,\"" + logPath + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr,
                SW_SHOWNORMAL);
        }).detach();
        break;
    }
    }
}

void DockApp::PositionDockSettings() {
    if (m_settingsWindow == nullptr || !IsDockSettingsOpen()) {
        return;
    }
    POINT origin{};
    if (!SettingsScreenOrigin(origin)) {
        return;
    }
    // The cached glass is keyed by origin; a move needs a fresh capture.
    if (m_settingsGlassOrigin.x != origin.x || m_settingsGlassOrigin.y != origin.y) {
        InvalidateSettingsGlass();
        QueueSettingsPaint();
    }
    SetWindowPos(m_settingsWindow, HWND_TOPMOST, SaturatedInt(origin.x), SaturatedInt(origin.y),
        SaturatedInt(m_settingsSize.cx), SaturatedInt(m_settingsSize.cy), SWP_NOACTIVATE);
}

void DockApp::OpenDockSettings() {
    // The settings panel lives beside Quick Settings: keep the popup open and
    // paint the panel to its right (or left when there is no room).
    HideHoverLabel();
    RebuildSettingsPopup();
    if (m_settingsWindow != nullptr && IsWindow(m_settingsWindow) != FALSE) {
        ShowWindow(m_settingsWindow, SW_SHOWNA);
        PositionDockSettings();
        // Ensure visible even if Present skipped ShowWindow on a cached path.
        if (IsWindowVisible(m_settingsWindow) == FALSE) {
            ShowWindow(m_settingsWindow, SW_SHOWNA);
        }
        Log(L"Dock Settings opened.");
    } else {
        Log(L"Dock Settings open failed (no window).");
    }
}

void DockApp::ToggleDockSettings() {
    if (IsDockSettingsOpen()) {
        CloseDockSettings();
        return;
    }
    // Cog click runs on the overflow UI thread that also services WH_MOUSE_LL.
    // RebuildSettingsPopup / PaintSettingsPopup is a full glass bake (~tens of
    // ms). Defer via the same timer OpenPerfMenus uses so the click handler
    // returns immediately and OpenDockSettings runs on the next WM_TIMER.
    if (m_window != nullptr) {
        SetTimer(m_window, kPerfOpenSettingsTimerId, 1, nullptr);
        return;
    }
    OpenDockSettings();
}

void DockApp::CloseDockSettings() noexcept {
    m_settingsHover = -1;
    if (m_settingsWindow != nullptr) {
        ShowWindow(m_settingsWindow, SW_HIDE);
    }
    std::vector<uint8_t>().swap(m_settingsBaseBits);
    std::vector<uint8_t>().swap(m_settingsPresentBits);
    m_settingsPresentSize = {};
    m_settingsHoverPaintOnly = false;
    InvalidateSettingsGlass();
}

void DockApp::DestroyDockSettings() noexcept {
    CloseDockSettings();
    ReleaseLayerPresentDib(m_settingsLayerDib);
    m_settingsPaintQueued = false;
    m_settingsHits.clear();
    std::vector<uint8_t>().swap(m_settingsBaseBits);
    std::vector<uint8_t>().swap(m_settingsPresentBits);
    m_settingsPresentSize = {};
    m_settingsHoverPaintOnly = false;
    InvalidateSettingsGlass();
    if (m_settingsWindow != nullptr) {
        DestroyWindow(m_settingsWindow);
        m_settingsWindow = nullptr;
    }
}

void DockApp::PaintSettingsPopup() {
    const UINT dpi = HostDpi();
    const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
    const LONG padding = std::max(16L, std::lround(18.0F * scale));
    const LONG radius = std::max(16L, std::lround(DOCK_CORNER_RADIUS_PT * scale));
    const LONG headerHeight = std::max(28L, std::lround(32.0F * scale));
    const LONG closeExtent = std::max(22L, std::lround(24.0F * scale));
    const LONG rowHeight = std::max(56L, std::lround(60.0F * scale));
    const LONG rowGap = std::max(6L, std::lround(8.0F * scale));
    const LONG buttonHeight = std::max(34L, std::lround(38.0F * scale));
    const LONG dividerGap = std::max(10L, std::lround(12.0F * scale));
    const LONG statusHeight = std::max(40L, std::lround(46.0F * scale));
    const LONG switchWidth = std::max(40L, std::lround(46.0F * scale));
    const LONG switchHeight = std::max(22L, std::lround(24.0F * scale));
    const LONG panelWidth = std::max(280L, std::lround(308.0F * scale));

    // Switch rows live here (above the size math) so the panel always fits
    // exactly the rows it paints - adding a row never clips again.
    struct SettingsRow {
        SettingsHitKind kind;
        const wchar_t* label;
        const wchar_t* sublabel;
        bool slider;
        bool enabled;
        float amount;
    };
    const SettingsRow rows[] = {
        {SettingsHitKind::LightPanels, L"Light mode",
            L"Light glass for Quick Settings and Dock Settings", false, m_config.LightPanels(),
            0.0F},
        {SettingsHitKind::Startup, L"Launch at startup", L"Start Hoverdock with Windows", false,
            m_config.LaunchAtStartup(), 0.0F},
        {SettingsHitKind::Updates, L"Check for updates", L"Auto-download and install builds", false,
            m_config.CheckForUpdates(), 0.0F},
        {SettingsHitKind::RimLight, L"Rim light", L"Edge glow and caustic on the glass", false,
            m_config.RimLight(), 0.0F},
        {SettingsHitKind::Lensing, L"Lensing", L"Refraction warp through the bevel", false,
            m_config.Lensing(), 0.0F},
        {SettingsHitKind::Dispersion, L"Dispersion", L"Spectral fringe at glass edges", false,
            m_config.Dispersion(), 0.0F},
        {SettingsHitKind::Frost, L"Frost", L"Clear / milky plate / full frost", true, false,
            m_config.FrostAmount()},
        {SettingsHitKind::Specular, L"Speculars", L"Key and fill glints on the surface", false,
            m_config.Specular(), 0.0F},
        {SettingsHitKind::DropShadow, L"Drop shadow", L"Soft contact shade under the dock", false,
            m_config.DropShadow(), 0.0F},
        {SettingsHitKind::DepthShade, L"Depth shade", L"Inner shading at the glass edge", false,
            m_config.DepthShade(), 0.0F},
    };
    const LONG switchRowCount = static_cast<LONG>(sizeof(rows) / sizeof(rows[0]));

    LONG contentY = padding;
    contentY += headerHeight;
    contentY += dividerGap;
    const LONG statusLineHeight = std::max(14L, std::lround(16.0F * scale));
    contentY += (rowHeight + rowGap) * switchRowCount;
    contentY += buttonHeight;
    contentY += rowGap;
    contentY += buttonHeight;
    contentY += dividerGap;
    contentY += statusHeight + statusLineHeight;  // update status + perf path
    contentY += padding;
    m_settingsSize.cx = panelWidth;
    m_settingsSize.cy = contentY;

    // WM_CLOSE / external destroy can kill the HWND while leaving a dangling
    // m_settingsWindow; without this check OpenDockSettings silently no-ops
    // (create is skipped) and the QS cog appears dead.
    if (m_settingsWindow != nullptr && IsWindow(m_settingsWindow) == FALSE) {
        m_settingsWindow = nullptr;
        m_settingsHits.clear();
        std::vector<uint8_t>().swap(m_settingsBaseBits);
        std::vector<uint8_t>().swap(m_settingsPresentBits);
        m_settingsPresentSize = {};
        InvalidateSettingsGlass();
        ReleaseLayerPresentDib(m_settingsLayerDib);
    }
    if (m_settingsWindow == nullptr) {
        const wchar_t className[] = L"LiquidGlassDockSettings";
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = &DockApp::DockSettingsProcedure;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = className;
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Log(L"Could not register the dock settings window class.");
            return;
        }
        constexpr DWORD extendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
            WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
        m_settingsWindow = CreateWindowExW(extendedStyle, className, L"", WS_POPUP, 0, 0, 1, 1,
            m_window, nullptr, m_instance, this);
        if (m_settingsWindow == nullptr) {
            Log(L"Could not create the dock settings window.");
            return;
        }
        EnsureWindowCapturable(m_settingsWindow);
        ClearPopupLayeredCorners(m_settingsWindow);
    }

    POINT origin{};
    if (!SettingsScreenOrigin(origin)) {
        return;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return;
    }
    HDC memory = CreateCompatibleDC(screen);
    if (memory == nullptr) {
        ReleaseDC(nullptr, screen);
        return;
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = m_settingsSize.cx;
    header.bV5Height = -m_settingsSize.cy;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    auto* pixels = static_cast<uint8_t*>(bits);
    const size_t pixelCount =
        static_cast<size_t>(m_settingsSize.cx) * static_cast<size_t>(m_settingsSize.cy);
    ReleaseDC(nullptr, screen);
    if (SettingsGlassValid(origin)) {
        std::memcpy(pixels, m_settingsGlass.data(), m_settingsGlass.size());
    } else {
        std::memset(pixels, 0, pixelCount * 4U);
        const bool gpuGlass = TryBakePopupGlass(origin, m_settingsSize.cx, m_settingsSize.cy, pixels,
            pixelCount * 4U, true);
        if (!gpuGlass) {
        screen = GetDC(nullptr);
        if (screen == nullptr) {
            SelectObject(memory, previousBitmap);
            DeleteObject(bitmap);
            DeleteDC(memory);
            return;
        }
        BitBlt(memory, 0, 0, SaturatedInt(m_settingsSize.cx), SaturatedInt(m_settingsSize.cy),
            screen, SaturatedInt(origin.x), SaturatedInt(origin.y), SRCCOPY);
        ReleaseDC(nullptr, screen);
        // CPU fallback when GlassPS bake is unavailable.
        const float frostAmt = (std::max)(std::clamp(m_config.FrostAmount(), 0.0F, 1.0F),
            DOCK_TEXT_PANEL_FROST_FLOOR);
        const int frostRadius = PopupFrostRadiusPx(frostAmt, scale);
        BoxBlurRgb(pixels, SaturatedInt(m_settingsSize.cx), SaturatedInt(m_settingsSize.cy),
            frostRadius);
        BoxBlurRgb(pixels, SaturatedInt(m_settingsSize.cx), SaturatedInt(m_settingsSize.cy),
            frostRadius);
        BoxBlurRgb(pixels, SaturatedInt(m_settingsSize.cx), SaturatedInt(m_settingsSize.cy),
            frostRadius);

        // Antialiased rounded-rect mask at 2x, then box-downsampled to
        // per-pixel coverage. No caret: this panel docks beside Quick
        // Settings rather than pointing at the dock.
        constexpr LONG kMaskSupersample = 2;
        const LONG maskW = m_settingsSize.cx * kMaskSupersample;
        const LONG maskH = m_settingsSize.cy * kMaskSupersample;
        HDC maskDc = CreateCompatibleDC(memory);
        void* maskBits = nullptr;
        HBITMAP maskBitmap = nullptr;
        if (maskDc != nullptr) {
            BITMAPV5HEADER maskHeader = header;
            maskHeader.bV5Width = maskW;
            maskHeader.bV5Height = -maskH;
            maskBitmap = CreateDIBSection(maskDc, reinterpret_cast<const BITMAPINFO*>(&maskHeader),
                DIB_RGB_COLORS, &maskBits, nullptr, 0);
        }
        if (maskDc != nullptr && maskBitmap != nullptr && maskBits != nullptr) {
            HGDIOBJ previousMask = SelectObject(maskDc, maskBitmap);
            RECT all{0, 0, maskW, maskH};
            FillRect(maskDc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            HBRUSH whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
            HPEN whitePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            HGDIOBJ previousBrush = SelectObject(maskDc, whiteBrush);
            HGDIOBJ previousPen = SelectObject(maskDc, whitePen);
            RoundRect(maskDc, 0, 0, maskW, maskH,
                SaturatedInt(radius * 2L * kMaskSupersample),
                SaturatedInt(radius * 2L * kMaskSupersample));
            SelectObject(maskDc, previousPen);
            SelectObject(maskDc, previousBrush);
            DeleteObject(whitePen);
            DeleteObject(whiteBrush);
            auto* mask = static_cast<uint8_t*>(maskBits);
            const LONG glassW = m_settingsSize.cx;
            const LONG glassH = m_settingsSize.cy;
            std::vector<float> coverage(pixelCount);
            const float kMaskSamples =
                static_cast<float>(kMaskSupersample * kMaskSupersample);
            for (LONG y = 0; y < glassH; ++y) {
                for (LONG x = 0; x < glassW; ++x) {
                    unsigned covered = 0;
                    for (LONG sampleY = 0; sampleY < kMaskSupersample; ++sampleY) {
                        const size_t maskRow = (static_cast<size_t>(y) * kMaskSupersample +
                            static_cast<size_t>(sampleY)) *
                            static_cast<size_t>(maskW);
                        for (LONG sampleX = 0; sampleX < kMaskSupersample; ++sampleX) {
                            covered += mask[(maskRow + static_cast<size_t>(x) * kMaskSupersample +
                                static_cast<size_t>(sampleX)) *
                                4U];
                        }
                    }
                    coverage[static_cast<size_t>(y) * glassW + x] =
                        static_cast<float>(covered) / (255.0F * kMaskSamples);
                }
            }
            std::vector<float> blurredCoverage = coverage;
            BoxBlurFloatPlane(blurredCoverage, SaturatedInt(glassW), SaturatedInt(glassH),
                std::max(1, static_cast<int>(std::lround(2.6F * scale))));
            // Same liquid-glass face as the dock (calibrated tone map + rim).
            ApplyLiquidGlassFace(pixels, SaturatedInt(glassW), SaturatedInt(glassH), coverage,
                blurredCoverage, m_config.FrostAmount(), true, m_config.LightPanels());
            SelectObject(maskDc, previousMask);
            DeleteObject(maskBitmap);
            DeleteDC(maskDc);
        }
        } // !gpuGlass

        m_settingsGlass.assign(pixels, pixels + pixelCount * 4U);
        m_settingsGlassSize = m_settingsSize;
        m_settingsGlassOrigin = origin;
        m_settingsGlassFrost = m_config.FrostAmount();
    }

    const int width = SaturatedInt(m_settingsSize.cx);
    const int height = SaturatedInt(m_settingsSize.cy);
    EnsureOverflowFonts(scale);
    HFONT titleFont = m_overflowTitleFont;
    HFONT labelFont = m_overflowLabelFont;
    HFONT statusFont = m_overflowStatusFont;
    // Hover chrome is applied after a base frame is cached so mouse moves can
    // repaint with PaintSettingsHoverFast (no text / switch redraw, mirroring
    // the Quick Settings and context menu fast paths).
    const int pendingHover = m_settingsHover;
    m_settingsHits.clear();

    auto pushHit = [this](SettingsHitKind kind, RECT bounds) {
        SettingsHit hit;
        hit.kind = kind;
        hit.bounds = bounds;
        m_settingsHits.push_back(hit);
    };
    auto drawSwitch = [&](LONG centerY, LONG right, bool enabled, bool hovered) {
        const LONG trackLeft = right - switchWidth;
        const LONG trackTop = centerY - switchHeight / 2L;
        const float trackRadius = static_cast<float>(switchHeight) * 0.5F;
        const float trackCxL = static_cast<float>(trackLeft) + trackRadius;
        const float trackCxR = static_cast<float>(right) - trackRadius;
        const float trackCy = static_cast<float>(trackTop) + trackRadius;
        const float wash = enabled ? (hovered ? 0.96F : 0.90F) : (hovered ? 0.72F : 0.62F);
        if (enabled) {
            // On = #98A869 toggle fill (Concept D Minimal Sage).
            FillPillColorPremul(pixels, width, height, trackCxL, trackCxR, trackCy, trackRadius,
                wash, kAmberB, kAmberG, kAmberR);
        } else {
            FillPillColorPremul(pixels, width, height, trackCxL, trackCxR, trackCy, trackRadius,
                wash, g_flyoutInkB, g_flyoutInkG, g_flyoutInkR);
        }
        const float knobRadius = trackRadius - std::max(2.0F, 2.0F * scale);
        const float knobCx = enabled ? trackCxR : trackCxL;
        FillCirclePremul(pixels, width, height, knobCx, trackCy, knobRadius, 0.95F);
    };
    auto drawSlider = [&](LONG centerY, LONG left, LONG right, float amount, bool hovered) {
        const LONG trackHeight = std::max(6L, std::lround(8.0F * scale));
        const float trackRadius = static_cast<float>(trackHeight) * 0.5F;
        const float trackCy = static_cast<float>(centerY);
        const float trackCxL = static_cast<float>(left) + trackRadius;
        const float trackCxR = static_cast<float>(right) - trackRadius;
        FillPillColorPremul(pixels, width, height, trackCxL, trackCxR, trackCy, trackRadius,
            hovered ? 0.78F : 0.68F, g_flyoutInkB, g_flyoutInkG, g_flyoutInkR);
        const float filled = std::clamp(amount, 0.0F, 1.0F);
        const float fillRight = trackCxL + (trackCxR - trackCxL) * filled;
        if (fillRight > trackCxL + 0.5F) {
            FillPillColorPremul(pixels, width, height, trackCxL, fillRight, trackCy, trackRadius,
                hovered ? 0.96F : 0.90F, kAmberB, kAmberG, kAmberR);
        }
        const float knobRadius = std::max(7.0F, 8.0F * scale);
        const float knobCx = trackCxL + (trackCxR - trackCxL) * filled;
        FillCirclePremul(pixels, width, height, knobCx, trackCy, knobRadius, 0.95F);
    };

    if (m_config.LightPanels()) {
        SetFlyoutChromeInk(DOCK_INK_R, DOCK_INK_G, DOCK_INK_B);
    } else {
        SetFlyoutChromeInk(DOCK_CHROME_INK_R, DOCK_CHROME_INK_G, DOCK_CHROME_INK_B);
    }

    LONG y = padding;
    RECT titleBounds{padding, y, panelWidth - padding - closeExtent - 8, y + headerHeight};
    DrawFlyoutText(pixels, width, height, titleBounds, titleFont, L"Dock Settings",
        DT_LEFT | DT_VCENTER | DT_SINGLELINE, 250);
    RECT closeBounds{panelWidth - padding - closeExtent, y + (headerHeight - closeExtent) / 2L,
        panelWidth - padding, y + (headerHeight - closeExtent) / 2L + closeExtent};
    DrawFlyoutText(pixels, width, height, closeBounds, titleFont, L"\u00D7",
        DT_CENTER | DT_VCENTER | DT_SINGLELINE, 250);
    pushHit(SettingsHitKind::Close, {closeBounds.left - 6, y, panelWidth - padding + 4,
        y + headerHeight});
    y += headerHeight;

    FillRectPremul(pixels, width, height, {padding, y - dividerGap / 2L, panelWidth - padding,
        y - dividerGap / 2L + 1}, 0.16F);

    // (Switch rows are defined above the size computation.)
    const LONG labelHeight = std::max(16L, std::lround(18.0F * scale));
    const LONG subHeight = std::max(14L, std::lround(16.0F * scale));
    for (const SettingsRow& row : rows) {
        const RECT rowBounds{padding, y, panelWidth - padding, y + rowHeight};
        FillSquirclePremul(pixels, width, height, rowBounds, ContentSquircleRadius(rowBounds, scale),
            0.10F);
        if (row.slider) {
            const LONG sliderLeft = padding + 4L;
            const LONG sliderRight = panelWidth - padding - 4L;
            RECT labelBounds{sliderLeft, y + 4L, sliderRight,
                y + 4L + labelHeight};
            RECT subBounds{sliderLeft, labelBounds.bottom, sliderRight,
                labelBounds.bottom + subHeight};
            DrawFlyoutText(pixels, width, height, labelBounds, labelFont, row.label,
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
            DrawFlyoutText(pixels, width, height, subBounds, statusFont, row.sublabel,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 240);
            const LONG sliderY = y + rowHeight - std::max(14L, std::lround(16.0F * scale));
            m_frostSliderTrack = {sliderLeft, sliderY - 10L, sliderRight, sliderY + 10L};
            drawSlider(sliderY, sliderLeft, sliderRight, row.amount, m_frostSliderDragging);
            pushHit(row.kind, rowBounds);
        } else {
            const LONG textRight = panelWidth - padding - switchWidth - 12L;
            RECT labelBounds{padding + 4, y + (rowHeight - labelHeight - subHeight) / 2L, textRight,
                y + (rowHeight - labelHeight - subHeight) / 2L + labelHeight};
            RECT subBounds{labelBounds.left, labelBounds.bottom, textRight,
                labelBounds.bottom + subHeight};
            DrawFlyoutText(pixels, width, height, labelBounds, labelFont, row.label,
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
            DrawFlyoutText(pixels, width, height, subBounds, statusFont, row.sublabel,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 240);
            drawSwitch(y + rowHeight / 2L, panelWidth - padding - 4L, row.enabled, false);
            pushHit(row.kind, rowBounds);
        }
        y += rowHeight + rowGap;
    }

    const bool checking = m_updateInFlight.load() || m_updateInstalling.load();
    RECT buttonBounds{padding, y, panelWidth - padding, y + buttonHeight};
    // #98A869 squircle + light chrome label ink (already set).
    FillSquircleColorPremul(pixels, width, height, buttonBounds,
        ContentSquircleRadius(buttonBounds, scale), 0.50F, DOCK_PANEL_TOGGLE_B, DOCK_PANEL_TOGGLE_G,
        DOCK_PANEL_TOGGLE_R);
    DrawFlyoutText(pixels, width, height, buttonBounds, labelFont,
        checking ? L"Checking..." : L"Check for updates now",
        DT_CENTER | DT_VCENTER | DT_SINGLELINE, checking ? 170 : 245);
    pushHit(SettingsHitKind::CheckNow, buttonBounds);
    y += buttonHeight + rowGap;

    const bool profiling = m_perfProfiler.IsRunning();
    RECT perfBounds{padding, y, panelWidth - padding, y + buttonHeight};
    FillSquircleColorPremul(pixels, width, height, perfBounds,
        ContentSquircleRadius(perfBounds, scale), profiling ? 0.70F : 0.50F, DOCK_PANEL_TOGGLE_B,
        DOCK_PANEL_TOGGLE_G, DOCK_PANEL_TOGGLE_R);
    DrawFlyoutText(pixels, width, height, perfBounds, labelFont,
        profiling ? L"Stop profiling" : L"Start performance profile",
        DT_CENTER | DT_VCENTER | DT_SINGLELINE, profiling ? 250 : 245);
    pushHit(SettingsHitKind::PerfProfile, perfBounds);
    y += buttonHeight + dividerGap;
    FillRectPremul(pixels, width, height, {padding, y - dividerGap / 2L, panelWidth - padding,
        y - dividerGap / 2L + 1}, 0.16F);

    std::string current = Updater::CurrentVersion();
    std::wstring wideVersion(current.begin(), current.end());
    RECT versionBounds{padding, y, panelWidth - padding, y + subHeight};
    DrawFlyoutText(pixels, width, height, versionBounds, statusFont,
        L"Hoverdock v" + wideVersion, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 240);
    RECT statusBounds{padding, versionBounds.bottom + 2, panelWidth - padding,
        versionBounds.bottom + 2 + subHeight + 4};
    DrawFlyoutText(pixels, width, height, statusBounds, statusFont, m_updateStatus,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 245);
    if (!m_perfStatus.empty() || profiling) {
        std::wstring perfLine = m_perfStatus;
        if (profiling && perfLine.empty()) {
            perfLine = L"Profiling... " + m_perfProfiler.LogPath();
        }
        RECT perfStatusBounds{padding, statusBounds.bottom, panelWidth - padding,
            y + statusHeight + subHeight};
        const bool savedLink = !profiling && !m_perfSavedLog.empty();
        const std::wstring savedPrefix = L"Saved ";
        const int underlineFrom = savedLink && perfLine.starts_with(savedPrefix)
            ? static_cast<int>(savedPrefix.size())
            : -1;
        DrawFlyoutText(pixels, width, height, perfStatusBounds, statusFont, perfLine,
            DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS, 245, underlineFrom);
        if (savedLink) {
            pushHit(SettingsHitKind::PerfLog, perfStatusBounds);
        }
    }

    const size_t bytes = pixelCount * 4U;
    m_settingsBaseBits.resize(bytes);
    std::memcpy(m_settingsBaseBits.data(), pixels, bytes);
    m_settingsPresentBits.resize(bytes);
    std::memcpy(m_settingsPresentBits.data(), pixels, bytes);
    m_settingsPresentSize = m_settingsSize;
    if (pendingHover >= 0 && static_cast<size_t>(pendingHover) < m_settingsHits.size()) {
        ApplySettingsHoverHighlight(pixels, width, height,
            m_settingsHits[static_cast<size_t>(pendingHover)]);
        std::memcpy(m_settingsPresentBits.data(), pixels, bytes);
    }

    POINT source{0, 0};
    POINT destination = origin;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(m_settingsWindow, nullptr, &destination, &m_settingsSize, memory, &source,
        0, &blend, ULW_ALPHA);
    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ShowWindow(m_settingsWindow, SW_SHOWNA);
    PositionDockSettings();
}

bool DockApp::IsOverflowOpen() const noexcept {
    return m_overflowWindow != nullptr && m_overflowVisibility != VisibilityState::Hidden;
}

bool DockApp::IsCursorOverOverflow(POINT cursor) const noexcept {
    if (!IsOverflowOpen()) {
        return false;
    }
    RECT bounds{};
    GetWindowRect(m_overflowWindow, &bounds);
    return IsInside(bounds, cursor.x, cursor.y);
}

bool DockApp::IsCursorWithinFlyoutZone(POINT cursor) const noexcept {
    // Union of the dock and the open Quick Settings popup, inflated by a margin
    // so travelling between the two (across the gap) does not dismiss anything.
    // Leaving the zone dismisses the popup and hides the dock. With no popup
    // open there is no gap to bridge: return false so the dock hides the
    // moment the cursor leaves the pill instead of lingering through the
    // inflated dock halo.
    if (!IsOverflowOpen() && !IsDockSettingsOpen() && !IsContextMenuOpen()) {
        return false;
    }
    const UINT dpi = HostDpi();
    const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
    const LONG margin = std::max(24L, static_cast<LONG>(std::lround(48.0F * scale)));
    const RECT dockZone{m_windowX - margin, m_currentY - margin,
        m_windowX + static_cast<LONG>(m_dockWidth) + margin,
        m_currentY + static_cast<LONG>(m_dockHeight) + margin};
    if (IsInside(dockZone, cursor.x, cursor.y)) {
        return true;
    }
    if (IsOverflowOpen()) {
        RECT popup{};
        GetWindowRect(m_overflowWindow, &popup);
        const RECT popupZone{popup.left - margin, popup.top - margin, popup.right + margin,
            popup.bottom + margin};
        if (IsInside(popupZone, cursor.x, cursor.y)) {
            return true;
        }
    }
    if (IsDockSettingsOpen()) {
        RECT settings{};
        GetWindowRect(m_settingsWindow, &settings);
        const RECT settingsZone{settings.left - margin, settings.top - margin,
            settings.right + margin, settings.bottom + margin};
        if (IsInside(settingsZone, cursor.x, cursor.y)) {
            return true;
        }
    }
    if (IsContextMenuOpen()) {
        RECT menu{};
        GetWindowRect(m_contextWindow, &menu);
        const RECT menuZone{menu.left - margin, menu.top - margin, menu.right + margin,
            menu.bottom + margin};
        if (IsInside(menuZone, cursor.x, cursor.y)) {
            return true;
        }
    }
    return false;
}

bool DockApp::IsTrayRenderIndex(int index) const noexcept {
    return index >= 0 && static_cast<size_t>(index) < m_iconRenderData.size() &&
        (m_iconRenderData[static_cast<size_t>(index)].kind == DockIconKind::Tray ||
            m_iconRenderData[static_cast<size_t>(index)].kind == DockIconKind::Clock);
}

bool DockApp::IsWeatherRenderIndex(int index) const noexcept {
    return index >= 0 && static_cast<size_t>(index) < m_iconRenderData.size() &&
        m_iconRenderData[static_cast<size_t>(index)].kind == DockIconKind::Weather;
}

void DockApp::OnWeatherUpdated() {
    // Snapshot changed on the worker; rebuild so the glyph appears/moves and
    // re-upload the Meteocons atlas entry. Failures are soft â€” last icon stays.
    RebuildLayout(false);
    EnsureTrayIcons();
    QueueRenderFrame();
}

void DockApp::ApplyBluetoothSnapshot() {
    m_bluetooth.AllowNextNotify();
    BluetoothSnapshot next = m_bluetooth.GetSnapshot();
    auto deviceEqual = [](const BluetoothDeviceInfo& a, const BluetoothDeviceInfo& b) {
        return a.id == b.id && a.name == b.name && a.status == b.status && a.connected == b.connected &&
            a.busy == b.busy;
    };
    auto listEqual = [&](const std::vector<BluetoothDeviceInfo>& a,
                         const std::vector<BluetoothDeviceInfo>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (!deviceEqual(a[i], b[i])) {
                return false;
            }
        }
        return true;
    };
    const bool unchanged = next.ready == m_bluetoothSnapshot.ready &&
        next.radioPresent == m_bluetoothSnapshot.radioPresent &&
        next.radioOn == m_bluetoothSnapshot.radioOn &&
        next.discovering == m_bluetoothSnapshot.discovering &&
        next.hiddenPaired == m_bluetoothSnapshot.hiddenPaired &&
        next.hint == m_bluetoothSnapshot.hint && listEqual(next.paired, m_bluetoothSnapshot.paired) &&
        listEqual(next.discovered, m_bluetoothSnapshot.discovered);
    m_bluetoothSnapshot = std::move(next);
    if (!IsOverflowOpen() || unchanged) {
        return;
    }
    RepaintBluetoothStatus();
}

void DockApp::RepaintBluetoothStatus() {
    // Called on the UI thread (which also owns WH_MOUSE_LL) after a BT
    // snapshot or an optimistic toggle. Home only shows the tile status, so
    // present that one rect from the underlay; pages that list BT state
    // (Bluetooth, Airplane) coalesce a full repaint through the queue.
    if (!IsOverflowOpen()) {
        return;
    }
    switch (m_qsPage) {
    case QuickSettingsPage::Home:
        if (m_qsBtStatusValid) {
            m_qsBtStatusDirty = true;
            PaintOverflowLiveFast();
        }
        break;
    case QuickSettingsPage::Bluetooth:
    case QuickSettingsPage::Airplane:
        m_overflowHover = -1;
        QueueOverflowPaint();
        break;
    default:
        break;
    }
}

int DockApp::AppSlotCount() const noexcept {
    return static_cast<int>(m_displayApps.size());
}

int DockApp::OverflowHitIndex(POINT point) const noexcept {
    for (size_t index = 0; index < m_overflowHits.size(); ++index) {
        if (IsInside(m_overflowHits[index].bounds, point.x, point.y)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool DockApp::OverflowScreenOrigin(POINT& origin, LONG& caretX) const noexcept {
    if (m_window == nullptr || m_overflowSize.cx <= 0 || m_overflowSize.cy <= 0) {
        return false;
    }

    RECT chevron{};
    for (const DockIconRenderData& icon : m_iconRenderData) {
        if (icon.kind == DockIconKind::Tray && icon.traySlot == TraySlot::Overflow) {
            chevron = icon.bounds;
            break;
        }
    }
    POINT chevronTopLeft{chevron.left, chevron.top};
    POINT chevronBottomRight{chevron.right, chevron.bottom};
    if (ClientToScreen(m_window, &chevronTopLeft) == FALSE ||
        ClientToScreen(m_window, &chevronBottomRight) == FALSE) {
        return false;
    }

    const UINT dpi = HostDpi();
    const float scale = static_cast<float>(dpi) / 96.0F;
    const LONG gap = std::max(8L, std::lround(10.0F * scale));
    const LONG margin = std::max(8L, std::lround(8.0F * scale));
    const LONG shadowMargin = DockShadowMarginPx(scale);
    const LONG chevronCenter = chevronTopLeft.x + (chevronBottomRight.x - chevronTopLeft.x) / 2L;
    const LONG popupMargin = std::max(1L, std::lround(DOCK_SHADOW_MARGIN_PT * scale));
    LONG x = chevronCenter - m_overflowSize.cx / 2L;
    // Keep the inner plate anchored where the pre-margin panel lived.
    LONG y = m_currentY + shadowMargin - gap - m_overflowSize.cy + popupMargin;
    const LONG minX = m_hostBounds.left + margin;
    const LONG maxX = m_hostBounds.right - m_overflowSize.cx - margin;
    if (maxX >= minX) {
        x = std::clamp(x, minX, maxX);
    } else {
        x = minX;
    }
    y = std::max(m_hostBounds.top + margin, y);
    const LONG radius = std::max(18L, std::lround(22.0F * scale));
    caretX = std::clamp(chevronCenter - x, popupMargin + radius,
        m_overflowSize.cx - popupMargin - radius);
    origin = {x, y};
    return true;
}

void DockApp::HandleOverflowClick(const TrayFlyoutHit& hit, UINT message) {
    switch (hit.kind) {
    case TrayFlyoutHitKind::None:
        break;
    case TrayFlyoutHitKind::Settings:
        // The gear is the dock's own settings (startup + updates), not
        // Windows Settings. The panel pops out beside Quick Settings, which
        // stays open; clicking the gear again dismisses it.
        ToggleDockSettings();
        break;
    case TrayFlyoutHitKind::Wifi:
        if (message == WM_RBUTTONUP) {
            CloseOverflowPopup();
            if (!SystemTray::OpenNetworkPanel()) {
                Log(L"Network panel did not open.");
            }
            break;
        }
        OpenQuickSettingsPage(QuickSettingsPage::Wifi);
        break;
    case TrayFlyoutHitKind::Sound:
        if (message == WM_RBUTTONUP) {
            CloseOverflowPopup();
            if (!SystemTray::OpenSoundMixer()) {
                Log(L"Volume mixer did not open.");
            }
            break;
        }
        OpenQuickSettingsPage(QuickSettingsPage::Sound);
        break;
    case TrayFlyoutHitKind::Brightness: {
        // Clicks project like wheel input (a click is two notches): instant
        // fill feedback, monitor persisted by the worker.
        if (m_brightnessTarget.load() < 0 && !m_tray.Status().brightnessAvailable) {
            CloseOverflowPopup();
            if (!SystemTray::OpenDisplaySettings()) {
                Log(L"Brightness did not change.");
            }
            break;
        }
        ScrollBrightness(message == WM_RBUTTONUP ? -240 : 240);
        break;
    }
    case TrayFlyoutHitKind::Boost:
        // Stays open: progress and the result report in the Boost tile itself.
        RunPerformanceBoost();
        break;
    case TrayFlyoutHitKind::BluetoothRadio:
        // Home tile body or drill-in header switch. Optimistic flip, then the
        // WinRT Radio.SetStateAsync runs on the BT worker (never here).
        ProfileScope::Mark(BluetoothService::ThreadTag("UI BluetoothRadio click").c_str());
        m_lastBluetoothUiAt = QpcSeconds();
        if (message == WM_RBUTTONUP && m_qsPage == QuickSettingsPage::Home) {
            // Right-click on the tile mirrors Wi-Fi: open the system page.
            OpenSettingsPage(L"ms-settings:bluetooth");
            break;
        }
        if (!m_bluetoothSnapshot.ready || !m_bluetoothSnapshot.radioPresent) {
            break;
        }
        m_bluetoothSnapshot.radioOn = !m_bluetoothSnapshot.radioOn;
        if (!m_bluetoothSnapshot.radioOn) {
            m_bluetoothSnapshot.discovering = false;
            m_bluetoothSnapshot.discovered.clear();
            m_bluetoothSnapshot.hint.clear();
        }
        m_bluetooth.SetRadioEnabled(m_bluetoothSnapshot.radioOn);
        RepaintBluetoothStatus();
        break;
    case TrayFlyoutHitKind::BluetoothPage:
        ApplyQuickSettingsCommand(hit, message);
        break;
    case TrayFlyoutHitKind::BluetoothConnect:
        ProfileScope::Mark(BluetoothService::ThreadTag("UI BluetoothConnect click").c_str());
        m_lastBluetoothUiAt = QpcSeconds();
        if (hit.index < 0 ||
            static_cast<size_t>(hit.index) >= m_bluetoothSnapshot.paired.size()) {
            break;
        }
        {
            const BluetoothDeviceInfo& device =
                m_bluetoothSnapshot.paired[static_cast<size_t>(hit.index)];
            if (!device.busy && !device.id.empty()) {
                if (device.connected) {
                    m_bluetooth.Disconnect(device.id);
                } else {
                    m_bluetooth.Connect(device.id);
                }
            }
        }
        break;
    case TrayFlyoutHitKind::BluetoothPair:
        ProfileScope::Mark(BluetoothService::ThreadTag("UI BluetoothPair click").c_str());
        m_lastBluetoothUiAt = QpcSeconds();
        if (m_bluetooth.IsPairing() || hit.index < 0 ||
            static_cast<size_t>(hit.index) >= m_bluetoothSnapshot.discovered.size()) {
            break;
        }
        {
            const BluetoothDeviceInfo& device =
                m_bluetoothSnapshot.discovered[static_cast<size_t>(hit.index)];
            if (!device.busy && !device.id.empty()) {
                // Real pairing may raise a Windows confirm/PIN prompt that
                // needs a foreground owner; the dry-run harness never steals focus.
                if (!BluetoothService::DryRun()) {
                    SetForegroundWindow(m_window);
                }
                m_bluetooth.Pair(device.id);
            }
        }
        break;
    case TrayFlyoutHitKind::BluetoothDiscover:
        ProfileScope::Mark(BluetoothService::ThreadTag("UI BluetoothDiscover click").c_str());
        m_lastBluetoothUiAt = QpcSeconds();
        if (m_window != nullptr) {
            // Keep the 4 s refresh timer alive while a scan runs.
            SetTimer(m_window, kBluetoothTimerId, 4000, nullptr);
        }
        if (m_bluetoothSnapshot.discovering) {
            m_bluetoothSnapshot.discovering = false;
            m_bluetoothSnapshot.discovered.clear();
            m_bluetoothSnapshot.hint.clear();
            m_bluetooth.StopDiscovery();
        } else if (m_bluetoothSnapshot.radioOn) {
            m_bluetoothSnapshot.discovering = true;
            m_bluetoothSnapshot.hint = L"Searching...";
            m_bluetooth.StartDiscovery();
        }
        QueueOverflowPaint();
        break;
    case TrayFlyoutHitKind::BluetoothSettings:
        CloseOverflowPopup();
        SetForegroundWindow(m_window);
        ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case TrayFlyoutHitKind::Back:
    case TrayFlyoutHitKind::Vpn:
    case TrayFlyoutHitKind::Microphone:
    case TrayFlyoutHitKind::Display:
    case TrayFlyoutHitKind::Hdr:
    case TrayFlyoutHitKind::Power:
    case TrayFlyoutHitKind::Airplane:
    case TrayFlyoutHitKind::SystemTrayPage:
    case TrayFlyoutHitKind::VolumeSlider:
    case TrayFlyoutHitKind::BrightnessSlider:
    case TrayFlyoutHitKind::CaptureGain:
    case TrayFlyoutHitKind::Toggle:
    case TrayFlyoutHitKind::PowerMode:
    case TrayFlyoutHitKind::WifiNetwork:
    case TrayFlyoutHitKind::AudioOutput:
    case TrayFlyoutHitKind::AudioInput:
    case TrayFlyoutHitKind::VpnEntry:
    case TrayFlyoutHitKind::MoreSettings:
    case TrayFlyoutHitKind::MediaTransport:
    case TrayFlyoutHitKind::MediaSeek:
        ApplyQuickSettingsCommand(hit, message);
        break;
    case TrayFlyoutHitKind::NotifyIcon:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < m_overflowIcons.size()) {
            SystemTray::InvokeNotifyIcon(m_overflowIcons[static_cast<size_t>(hit.index)], message);
            // Right-click opens the tray icon's context menu; keep Quick Settings
            // open so the menu is usable. Left-click still dismisses after invoke.
            if (message != WM_RBUTTONUP) {
                CloseOverflowPopup();
            }
        }
        break;
    }
}

void DockApp::PositionOverflowPopup() {
    if (m_overflowWindow == nullptr || !IsOverflowOpen()) {
        return;
    }
    if (!m_overflowPresentBits.empty()) {
        PresentOverflowLayer();
        return;
    }
    POINT origin{};
    LONG caret = m_overflowCaretX;
    if (!OverflowScreenOrigin(origin, caret)) {
        return;
    }
    m_overflowCaretX = caret;
    SetWindowPos(m_overflowWindow, HWND_TOPMOST, SaturatedInt(origin.x), SaturatedInt(origin.y),
        SaturatedInt(m_overflowSize.cx), SaturatedInt(m_overflowSize.cy), SWP_NOACTIVATE);
    PositionDockSettings();
}


void DockApp::InvalidateOverflowGlass() noexcept {
    // Swap idiom: release the ~1 MB backing store, not just the size. It is
    // rebuilt on demand by the next paint.
    std::vector<uint8_t>().swap(m_overflowGlass);
    m_overflowGlassSize = {};
    m_overflowGlassOrigin = {};
    m_overflowGlassCaretX = 0;
    m_overflowGlassFrost = -1.0F;
}

bool DockApp::OverflowGlassValid(POINT origin) const noexcept {
    return !m_overflowGlass.empty()
        && m_overflowGlassSize.cx == m_overflowSize.cx
        && m_overflowGlassSize.cy == m_overflowSize.cy
        && m_overflowGlassOrigin.x == origin.x
        && m_overflowGlassOrigin.y == origin.y
        && m_overflowGlassCaretX == m_overflowCaretX
        && m_overflowGlass.size()
            == static_cast<size_t>(m_overflowSize.cx) * static_cast<size_t>(m_overflowSize.cy) * 4U
        && std::abs(m_overflowGlassFrost - m_config.FrostAmount()) < 0.001F;
}

void DockApp::DestroyOverflowFonts() noexcept {
    if (m_overflowTitleFont != nullptr) {
        DeleteObject(m_overflowTitleFont);
        m_overflowTitleFont = nullptr;
    }
    if (m_overflowSectionFont != nullptr) {
        DeleteObject(m_overflowSectionFont);
        m_overflowSectionFont = nullptr;
    }
    if (m_overflowLabelFont != nullptr) {
        DeleteObject(m_overflowLabelFont);
        m_overflowLabelFont = nullptr;
    }
    if (m_overflowStatusFont != nullptr) {
        DeleteObject(m_overflowStatusFont);
        m_overflowStatusFont = nullptr;
    }
    m_overflowFontScale = 0.0F;
}

void DockApp::EnsureOverflowFonts(float scale) {
    if (m_overflowTitleFont != nullptr && m_overflowSectionFont != nullptr
        && m_overflowLabelFont != nullptr && m_overflowStatusFont != nullptr
        && m_overflowFontScale == scale) {
        return;
    }
    DestroyOverflowFonts();
    m_overflowTitleFont = CreateFlyoutFont(std::max(16, static_cast<int>(std::lround(18.0F * scale))),
        FW_SEMIBOLD);
    m_overflowSectionFont = CreateFlyoutFont(std::max(13, static_cast<int>(std::lround(14.0F * scale))),
        FW_SEMIBOLD);
    m_overflowLabelFont = CreateFlyoutFont(std::max(13, static_cast<int>(std::lround(14.0F * scale))),
        FW_SEMIBOLD);
    m_overflowStatusFont = CreateFlyoutFont(std::max(11, static_cast<int>(std::lround(12.0F * scale))),
        FW_NORMAL);
    m_overflowFontScale = scale;
}

void DockApp::RebuildOverflowPopup() {
    // UIA Shell_TrayWnd scrape is expensive (~tens of ms). Home/open never
    // needs it; OpenQuickSettingsPage(SystemTray) loads icons on demand.
    // Do not InvalidateOverflowGlass here: a warm glass buffer is what keeps
    // chevron open off the BakeGlassPanel / CPU blur path.
    m_overflowHover = -1;
    PaintOverflowPopup();
}

void DockApp::ApplyOverflowHoverHighlight(uint8_t* pixels, int width, int height,
    const TrayFlyoutHit& hit) const {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    switch (hit.kind) {
    case TrayFlyoutHitKind::Settings:
        // The cog stays its idle size. A hover plate or scale-up covered the
        // tile underneath.
        break;
    case TrayFlyoutHitKind::BluetoothRadio:
        // Home tile body: no plate (matches Wi-Fi / VPN tiles). The drill-in
        // header switch keeps its subtle plate.
        if (m_qsPage == QuickSettingsPage::Home) {
            break;
        }
        [[fallthrough]];
    case TrayFlyoutHitKind::BluetoothSettings: {
        const float scale = static_cast<float>(HostDpi() == 0 ? 96U : HostDpi()) / 96.0F;
        FillSquirclePremul(pixels, width, height, hit.bounds, ContentSquircleRadius(hit.bounds, scale),
            0.12F);
        break;
    }
    case TrayFlyoutHitKind::BluetoothConnect:
    case TrayFlyoutHitKind::BluetoothPair:
    case TrayFlyoutHitKind::BluetoothDiscover: {
        const float scale = static_cast<float>(HostDpi() == 0 ? 96U : HostDpi()) / 96.0F;
        FillSquirclePremul(pixels, width, height, hit.bounds, ContentSquircleRadius(hit.bounds, scale),
            0.10F);
        break;
    }
    case TrayFlyoutHitKind::NotifyIcon: {
        // Full-row highlight like other list pages. A centered icon plate on a
        // wide row looked like a grey placeholder square over the app name.
        const float scale = static_cast<float>(HostDpi() == 0 ? 96U : HostDpi()) / 96.0F;
        FillSquirclePremul(pixels, width, height, hit.bounds, ContentSquircleRadius(hit.bounds, scale),
            0.10F);
        break;
    }
    case TrayFlyoutHitKind::Wifi:
    case TrayFlyoutHitKind::Sound:
    case TrayFlyoutHitKind::Boost:
    case TrayFlyoutHitKind::Brightness:
    case TrayFlyoutHitKind::Back:
    case TrayFlyoutHitKind::BluetoothPage:
    case TrayFlyoutHitKind::Vpn:
    case TrayFlyoutHitKind::Microphone:
    case TrayFlyoutHitKind::Display:
    case TrayFlyoutHitKind::Hdr:
    case TrayFlyoutHitKind::Power:
    case TrayFlyoutHitKind::Airplane:
    case TrayFlyoutHitKind::SystemTrayPage:
    case TrayFlyoutHitKind::VolumeSlider:
    case TrayFlyoutHitKind::BrightnessSlider:
    case TrayFlyoutHitKind::CaptureGain:
    case TrayFlyoutHitKind::Toggle:
    case TrayFlyoutHitKind::PowerMode:
    case TrayFlyoutHitKind::WifiNetwork:
    case TrayFlyoutHitKind::AudioOutput:
    case TrayFlyoutHitKind::AudioInput:
    case TrayFlyoutHitKind::VpnEntry:
    case TrayFlyoutHitKind::MoreSettings:
    case TrayFlyoutHitKind::MediaSeek:
        break;
    case TrayFlyoutHitKind::MediaTransport: {
        const float scale = static_cast<float>(HostDpi() == 0 ? 96U : HostDpi()) / 96.0F;
        FillSquirclePremul(pixels, width, height, hit.bounds, ContentSquircleRadius(hit.bounds, scale),
            0.10F);
        break;
    }
    default:
        break;
    }
}

void DockApp::PaintOverflowHoverFast() {
    POINT origin{};
    LONG caret = m_overflowCaretX;
    if (m_overflowBaseBits.empty() || m_overflowWindow == nullptr ||
        m_overflowPresentSize.cx <= 0 || m_overflowPresentSize.cy <= 0 ||
        m_overflowPresentSize.cx != m_overflowSize.cx ||
        m_overflowPresentSize.cy != m_overflowSize.cy ||
        !OverflowScreenOrigin(origin, caret)) {
        PaintOverflowPopup();
        return;
    }
    m_overflowCaretX = caret;
    const ULONGLONG now = GetTickCount64();
    const int width = SaturatedInt(m_overflowPresentSize.cx);
    const int height = SaturatedInt(m_overflowPresentSize.cy);
    m_overflowPresentSize = m_overflowSize;
    if (m_overflowPresentBits.size() != m_overflowBaseBits.size()) {
        m_overflowPresentBits = m_overflowBaseBits;
        m_overflowHoverDirtyValid = false;
    }

    auto restoreRect = [&](const RECT& rect) {
        const LONG left = (std::max)(0L, rect.left);
        const LONG top = (std::max)(0L, rect.top);
        const LONG right = (std::min)(static_cast<LONG>(width), rect.right);
        const LONG bottom = (std::min)(static_cast<LONG>(height), rect.bottom);
        if (right <= left || bottom <= top) {
            return;
        }
        const size_t rowBytes = static_cast<size_t>(right - left) * 4U;
        for (LONG y = top; y < bottom; ++y) {
            const size_t offset =
                (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(left)) * 4U;
            std::memcpy(m_overflowPresentBits.data() + offset, m_overflowBaseBits.data() + offset,
                rowBytes);
        }
    };

    auto expandDirty = [&](RECT rect) -> RECT {
        rect.left = (std::max)(0L, rect.left - 8);
        rect.top = (std::max)(0L, rect.top - 8);
        rect.right = (std::min)(static_cast<LONG>(width), rect.right + 8);
        rect.bottom = (std::min)(static_cast<LONG>(height), rect.bottom + 8);
        return rect;
    };

    auto gearDirty = [&]() -> RECT {
        // Gear hover scales ~30% about its center; dirty must cover the wipe.
        constexpr float kGearHoverScale = 1.30F;
        const int idle = static_cast<int>(m_overflowGearExtent);
        const int hoverExt = std::max(1, static_cast<int>(std::lround(
            static_cast<float>(idle) * kGearHoverScale)));
        const int half = (std::max)(idle, hoverExt) / 2 + 4;
        const int centerX = m_overflowGearX + idle / 2;
        const int centerY = m_overflowGearY + idle / 2;
        RECT r{
            static_cast<LONG>((std::max)(0, centerX - half)),
            static_cast<LONG>((std::max)(0, centerY - half)),
            static_cast<LONG>((std::min)(width, centerX + half + 1)),
            static_cast<LONG>((std::min)(height, centerY + half + 1))};
        return r;
    };

    RECT dirty{};
    bool haveDirty = false;
    if (m_overflowHoverDirtyValid) {
        restoreRect(m_overflowHoverDirty);
        dirty = m_overflowHoverDirty;
        haveDirty = true;
    }

    if (m_overflowHover >= 0 && static_cast<size_t>(m_overflowHover) < m_overflowHits.size()) {
        const TrayFlyoutHit& hit = m_overflowHits[static_cast<size_t>(m_overflowHover)];
        ApplyOverflowHoverHighlight(m_overflowPresentBits.data(), width, height, hit);
        RECT next = (hit.kind == TrayFlyoutHitKind::Settings && m_overflowGearExtent > 0U)
            ? gearDirty()
            : expandDirty(hit.bounds);
        if (!haveDirty) {
            dirty = next;
            haveDirty = true;
        } else {
            dirty.left = (std::min)(dirty.left, next.left);
            dirty.top = (std::min)(dirty.top, next.top);
            dirty.right = (std::max)(dirty.right, next.right);
            dirty.bottom = (std::max)(dirty.bottom, next.bottom);
        }
        m_overflowHoverDirty = next;
        m_overflowHoverDirtyValid = true;
    } else {
        m_overflowHoverDirtyValid = false;
    }

    if (!PresentLayeredBits(m_overflowWindow, origin, m_overflowPresentSize.cx,
            m_overflowPresentSize.cy, m_overflowPresentBits.data(), m_overflowPresentBits.size(),
            m_overflowLayerDib, haveDirty ? &dirty : nullptr)) {
        return;
    }
    m_lastOverflowHoverPresentMs = now;
    if (!IsWindowVisible(m_overflowWindow)) {
        ShowWindow(m_overflowWindow, SW_SHOWNA);
    }
}

void DockApp::PaintOverflowLiveFast() {
    POINT origin{};
    LONG caret = m_overflowCaretX;
    if (m_overflowUnderlayBits.empty() || m_overflowWindow == nullptr ||
        m_overflowPresentSize.cx <= 0 || m_overflowPresentSize.cy <= 0 ||
        m_overflowPresentSize.cx != m_overflowSize.cx ||
        m_overflowPresentSize.cy != m_overflowSize.cy ||
        (!m_qsMeterValid && !m_qsScrubValid && !m_qsTempsValid &&
            !m_qsBoostStatusValid && !m_qsBtStatusValid) ||
        m_overflowUnderlayBits.size() != m_overflowPresentBits.size() ||
        !OverflowScreenOrigin(origin, caret)) {
        PaintOverflowPopup();
        return;
    }
    m_overflowCaretX = caret;
    const int width = SaturatedInt(m_overflowPresentSize.cx);
    const int height = SaturatedInt(m_overflowPresentSize.cy);
    const float scale = static_cast<float>(HostDpi()) / 96.0F;

    auto restoreRect = [&](const RECT& rect) {
        const LONG left = (std::max)(0L, rect.left);
        const LONG top = (std::max)(0L, rect.top);
        const LONG right = (std::min)(static_cast<LONG>(width), rect.right);
        const LONG bottom = (std::min)(static_cast<LONG>(height), rect.bottom);
        if (right <= left || bottom <= top) {
            return;
        }
        const size_t rowBytes = static_cast<size_t>(right - left) * 4U;
        for (LONG y = top; y < bottom; ++y) {
            const size_t offset =
                (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(left)) * 4U;
            std::memcpy(m_overflowPresentBits.data() + offset, m_overflowUnderlayBits.data() + offset,
                rowBytes);
            if (m_overflowBaseBits.size() == m_overflowPresentBits.size()) {
                std::memcpy(m_overflowBaseBits.data() + offset, m_overflowUnderlayBits.data() + offset,
                    rowBytes);
            }
        }
    };

    auto expandDirty = [&](RECT rect) -> RECT {
        rect.left = (std::max)(0L, rect.left - 10);
        rect.top = (std::max)(0L, rect.top - 10);
        rect.right = (std::min)(static_cast<LONG>(width), rect.right + 10);
        rect.bottom = (std::min)(static_cast<LONG>(height), rect.bottom + 10);
        return rect;
    };

    RECT dirty{};
    bool haveDirty = false;
    auto addDirty = [&](RECT rect) {
        rect = expandDirty(rect);
        restoreRect(rect);
        if (!haveDirty) {
            dirty = rect;
            haveDirty = true;
        } else {
            dirty.left = (std::min)(dirty.left, rect.left);
            dirty.top = (std::min)(dirty.top, rect.top);
            dirty.right = (std::max)(dirty.right, rect.right);
            dirty.bottom = (std::max)(dirty.bottom, rect.bottom);
        }
    };

    if (m_qsMeterValid) {
        addDirty(m_qsMeterRect);
    }
    if (m_qsScrubValid) {
        addDirty(m_qsScrubRect);
    }
    // Temperature values only when a new reading arrived; mic-meter frames
    // leave those rects (and their text) untouched.
    const bool temps = m_qsTempsValid && m_qsTempsDirty;
    m_qsTempsDirty = false;
    if (temps) {
        addDirty(m_qsTempCpuRect);
        addDirty(m_qsTempGpuRect);
    }
    const bool boost = m_qsBoostStatusValid && m_qsBoostStatusDirty;
    m_qsBoostStatusDirty = false;
    if (boost) {
        addDirty(m_qsBoostStatusRect);
    }
    const bool bluetooth = m_qsBtStatusValid && m_qsBtStatusDirty;
    m_qsBtStatusDirty = false;
    if (bluetooth) {
        addDirty(m_qsBtStatusRect);
    }
    if (!haveDirty) {
        return;
    }

    // includeBoost / includeBluetooth only when their rect was restored;
    // otherwise meter ticks would DrawFlyoutText over existing glyphs
    // (fake-bold regression from PR #26).
    PaintQsLiveOverlays(m_overflowPresentBits.data(), width, height, scale, temps, boost, bluetooth);
    if (m_overflowBaseBits.size() == m_overflowPresentBits.size()) {
        // Keep base in sync so hover-fast restores still show live meters.
        const LONG left = dirty.left;
        const LONG top = dirty.top;
        const LONG right = dirty.right;
        const LONG bottom = dirty.bottom;
        const size_t rowBytes = static_cast<size_t>(right - left) * 4U;
        for (LONG y = top; y < bottom; ++y) {
            const size_t offset =
                (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(left)) * 4U;
            std::memcpy(m_overflowBaseBits.data() + offset, m_overflowPresentBits.data() + offset, rowBytes);
        }
    }
    if (m_overflowHover >= 0 && static_cast<size_t>(m_overflowHover) < m_overflowHits.size()) {
        ApplyOverflowHoverHighlight(m_overflowPresentBits.data(), width, height,
            m_overflowHits[static_cast<size_t>(m_overflowHover)]);
        m_overflowHoverDirtyValid = false;
    }

    if (!PresentLayeredBits(m_overflowWindow, origin, m_overflowPresentSize.cx,
            m_overflowPresentSize.cy, m_overflowPresentBits.data(), m_overflowPresentBits.size(),
            m_overflowLayerDib, &dirty)) {
        return;
    }
    if (!IsWindowVisible(m_overflowWindow)) {
        ShowWindow(m_overflowWindow, SW_SHOWNA);
    }
}

void DockApp::PaintOverflowPopup() {
    ProfileScope scope("PaintOverflowPopup");
    const UINT dpi = HostDpi();
    const float scale = static_cast<float>(dpi) / 96.0F;
    const LONG padding = std::max(16L, std::lround(18.0F * scale));
    const LONG caretHeight = std::max(10L, std::lround(12.0F * scale));
    const LONG caretWidth = std::max(16L, std::lround(18.0F * scale));
    const LONG radius =
        std::max(16L, std::lround(DOCK_CORNER_RADIUS_PT * scale));
    const LONG shadowMargin = std::max(1L, std::lround(DOCK_SHADOW_MARGIN_PT * scale));
    const LONG gearSize = std::max(18L, std::lround(22.0F * scale));
    const LONG headerHeight = std::max(gearSize, std::max(28L, std::lround(32.0F * scale)));
    RefreshQuickSettingsCache();
    LONG panelWidth = 0;
    LONG contentHeight = 0;
    MeasureQuickSettings(scale, padding, gearSize, headerHeight, panelWidth, contentHeight);
    // Reserve a transparent device-pixel ring outside the actual panel. The
    // panel layout remains the same size; only its layered HWND grows.
    m_overflowSize.cx = panelWidth + shadowMargin * 2L;
    m_overflowSize.cy = contentHeight + caretHeight + shadowMargin * 2L;

    if (m_overflowWindow == nullptr) {
        const wchar_t className[] = L"LiquidGlassDockOverflow";
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = &DockApp::OverflowWindowProcedure;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = className;
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Log(L"Could not register the tray overflow window class.");
            return;
        }
        constexpr DWORD extendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
            WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
        m_overflowWindow = CreateWindowExW(extendedStyle, className, L"", WS_POPUP, 0, 0, 1, 1,
            m_window, nullptr, m_instance, this);
        if (m_overflowWindow == nullptr) {
            Log(L"Could not create the tray overflow window.");
            return;
        }
        EnsureWindowCapturable(m_overflowWindow);
        ClearPopupLayeredCorners(m_overflowWindow);
    }

    POINT origin{};
    if (!OverflowScreenOrigin(origin, m_overflowCaretX)) {
        return;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return;
    }
    HDC memory = CreateCompatibleDC(screen);
    if (memory == nullptr) {
        ReleaseDC(nullptr, screen);
        return;
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = m_overflowSize.cx;
    header.bV5Height = -m_overflowSize.cy;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    auto* pixels = static_cast<uint8_t*>(bits);
    const size_t pixelCount =
        static_cast<size_t>(m_overflowSize.cx) * static_cast<size_t>(m_overflowSize.cy);
    ReleaseDC(nullptr, screen);
    bool glassCacheHit = false;
    if (OverflowGlassValid(origin)) {
        ProfileScope scopeGlassHit("OverflowGlass-cache-hit");
        std::memcpy(pixels, m_overflowGlass.data(), m_overflowGlass.size());
        glassCacheHit = true;
    } else {
        ProfileScope scopeGlassMiss("OverflowGlass-bake");
        std::memset(pixels, 0, pixelCount * 4U);
        const bool gpuGlass = TryBakePopupGlass(origin, m_overflowSize.cx, m_overflowSize.cy, pixels,
            pixelCount * 4U, true, true);
        if (!gpuGlass) {
        screen = GetDC(nullptr);
        if (screen == nullptr) {
            SelectObject(memory, previousBitmap);
            DeleteObject(bitmap);
            DeleteDC(memory);
            return;
        }
        BitBlt(memory, 0, 0, SaturatedInt(m_overflowSize.cx), SaturatedInt(m_overflowSize.cy), screen,
            SaturatedInt(origin.x), SaturatedInt(origin.y), SRCCOPY);
        ReleaseDC(nullptr, screen);
        // CPU fallback when GlassPS bake is unavailable.
        const float frostAmt = (std::max)(std::clamp(m_config.FrostAmount(), 0.0F, 1.0F),
            DOCK_TEXT_PANEL_FROST_FLOOR);
        const int frostRadius = PopupFrostRadiusPx(frostAmt, scale);
        BoxBlurRgb(pixels, SaturatedInt(m_overflowSize.cx), SaturatedInt(m_overflowSize.cy),
            frostRadius);
        BoxBlurRgb(pixels, SaturatedInt(m_overflowSize.cx), SaturatedInt(m_overflowSize.cy),
            frostRadius);
        BoxBlurRgb(pixels, SaturatedInt(m_overflowSize.cx), SaturatedInt(m_overflowSize.cy),
            frostRadius);

        // Antialiased shape mask: rasterize the rounded body + caret triangle at
        // 2x with GDI, then box-downsample to per-pixel coverage. A 1x GDI mask
        // has hard stair-stepped edges; supersampling smooths them.
        constexpr LONG kMaskSupersample = 2;
        const LONG maskW = m_overflowSize.cx * kMaskSupersample;
        const LONG maskH = m_overflowSize.cy * kMaskSupersample;
        HDC maskDc = CreateCompatibleDC(memory);
        void* maskBits = nullptr;
        HBITMAP maskBitmap = nullptr;
        if (maskDc != nullptr) {
            BITMAPV5HEADER maskHeader = header;
            maskHeader.bV5Width = maskW;
            maskHeader.bV5Height = -maskH;
            maskBitmap = CreateDIBSection(maskDc, reinterpret_cast<const BITMAPINFO*>(&maskHeader),
                DIB_RGB_COLORS, &maskBits, nullptr, 0);
        }
        if (maskDc != nullptr && maskBitmap != nullptr && maskBits != nullptr) {
            HGDIOBJ previousMask = SelectObject(maskDc, maskBitmap);
            RECT all{0, 0, maskW, maskH};
            FillRect(maskDc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            HBRUSH whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
            HPEN whitePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            HGDIOBJ previousBrush = SelectObject(maskDc, whiteBrush);
            HGDIOBJ previousPen = SelectObject(maskDc, whitePen);
            const LONG bodyBottom = m_overflowSize.cy - shadowMargin - caretHeight;
            const LONG bodyLeft = shadowMargin;
            const LONG bodyRight = m_overflowSize.cx - shadowMargin;
            RoundRect(maskDc, SaturatedInt(bodyLeft * kMaskSupersample),
                SaturatedInt(shadowMargin * kMaskSupersample),
                SaturatedInt(bodyRight * kMaskSupersample),
                SaturatedInt(bodyBottom * kMaskSupersample),
                SaturatedInt(radius * 2L * kMaskSupersample),
                SaturatedInt(radius * 2L * kMaskSupersample));
            POINT triangle[3] = {
                {(m_overflowCaretX - caretWidth / 2L) * kMaskSupersample,
                    bodyBottom * kMaskSupersample - kMaskSupersample},
                {(m_overflowCaretX + caretWidth / 2L) * kMaskSupersample,
                    bodyBottom * kMaskSupersample - kMaskSupersample},
                {m_overflowCaretX * kMaskSupersample,
                    (m_overflowSize.cy - shadowMargin) * kMaskSupersample - kMaskSupersample},
            };
            Polygon(maskDc, triangle, 3);
            SelectObject(maskDc, previousPen);
            SelectObject(maskDc, previousBrush);
            DeleteObject(whitePen);
            DeleteObject(whiteBrush);
            auto* mask = static_cast<uint8_t*>(maskBits);
            const LONG glassW = m_overflowSize.cx;
            const LONG glassH = m_overflowSize.cy;
            // 1. Per-pixel shape coverage from the supersampled mask.
            std::vector<float> coverage(pixelCount);
            const float kMaskSamples =
                static_cast<float>(kMaskSupersample * kMaskSupersample);
            for (LONG y = 0; y < glassH; ++y) {
                for (LONG x = 0; x < glassW; ++x) {
                    unsigned covered = 0;
                    for (LONG sampleY = 0; sampleY < kMaskSupersample; ++sampleY) {
                        const size_t maskRow = (static_cast<size_t>(y) * kMaskSupersample +
                            static_cast<size_t>(sampleY)) *
                            static_cast<size_t>(maskW);
                        for (LONG sampleX = 0; sampleX < kMaskSupersample; ++sampleX) {
                            covered += mask[(maskRow + static_cast<size_t>(x) * kMaskSupersample +
                                static_cast<size_t>(sampleX)) *
                                4U];
                        }
                    }
                    coverage[static_cast<size_t>(y) * glassW + x] =
                        static_cast<float>(covered) / (255.0F * kMaskSamples);
                }
            }
            // 2. Rim-light field: blurred coverage minus coverage forms an inner
            // edge band, mirroring the shader's exp(-distance/2.6dpi) rim.
            std::vector<float> blurredCoverage = coverage;
            BoxBlurFloatPlane(blurredCoverage, SaturatedInt(glassW), SaturatedInt(glassH),
                std::max(1, static_cast<int>(std::lround(2.6F * scale))));
            // 3. Dock glass recipe per pixel: tint mix, polish lerp, rim light,
            // gradient-noise dither, shape mask, premultiplied alpha. Constants
            // shared with GlassPS via DockTheme.hlsli.
            // Same liquid-glass face as the dock (calibrated tone map + rim).
            ApplyLiquidGlassFace(pixels, SaturatedInt(glassW), SaturatedInt(glassH), coverage,
                blurredCoverage, m_config.FrostAmount(), true, m_config.LightPanels());
            ApplyPopupOuterShadow(pixels, SaturatedInt(glassW), SaturatedInt(glassH),
                static_cast<int>(shadowMargin), static_cast<float>(radius), scale);
            SelectObject(maskDc, previousMask);
            DeleteObject(maskBitmap);
            DeleteDC(maskDc);
        }
        } // !gpuGlass

        m_overflowGlass.assign(pixels, pixels + pixelCount * 4U);
        m_overflowGlassSize = m_overflowSize;
        m_overflowGlassOrigin = origin;
        m_overflowGlassCaretX = m_overflowCaretX;
        m_overflowGlassFrost = m_config.FrostAmount();
    }
    const int width = SaturatedInt(m_overflowSize.cx);
    const int height = SaturatedInt(m_overflowSize.cy);
    EnsureOverflowFonts(scale);
    HFONT titleFont = m_overflowTitleFont;
    HFONT sectionFont = m_overflowSectionFont;
    HFONT labelFont = m_overflowLabelFont;
    HFONT statusFont = m_overflowStatusFont;
    // Hover chrome is applied after a base frame is cached so mouse moves can
    // repaint with PaintOverflowHoverFast (no blur / text / icon redraw).
    const int pendingHover = m_overflowHover;
    m_overflowHits.clear();

    // Underlay pass: meter/scrub tracks at empty levels. Live overlays then paint
    // real peaks/progress so meters can update via UpdateLayeredWindowIndirect.
    m_qsPaintUnderlayPass = true;
    {
        ProfileScope scopeQs("PaintQuickSettings");
        PaintQuickSettings(pixels, width, height, memory, scale, padding, panelWidth, gearSize,
            headerHeight, titleFont, sectionFont, labelFont, statusFont);
    }
    m_qsPaintUnderlayPass = false;
    m_qsTempsDirty = false;

    const size_t bytes = pixelCount * 4U;
    m_overflowUnderlayBits.resize(bytes);
    std::memcpy(m_overflowUnderlayBits.data(), pixels, bytes);
    PaintQsLiveOverlays(pixels, width, height, scale);

    m_overflowBaseBits.resize(bytes);
    std::memcpy(m_overflowBaseBits.data(), pixels, bytes);
    if (pendingHover >= 0 && static_cast<size_t>(pendingHover) < m_overflowHits.size()) {
        ApplyOverflowHoverHighlight(pixels, width, height,
            m_overflowHits[static_cast<size_t>(pendingHover)]);
    }
    m_overflowPresentBits.resize(bytes);
    std::memcpy(m_overflowPresentBits.data(), pixels, bytes);
    m_overflowPresentSize = m_overflowSize;
    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    PresentOverflowLayer();
    if (m_overflowWindow != nullptr) {
        // Window now exists: kick (or re-arm) the network/COM cache worker.
        // RefreshQuickSettingsCache may have run before CreateWindow on first open.
        const ULONGLONG now = GetTickCount64();
        if (m_qsCache.stampNetwork == 0 || now - m_qsCache.stampNetwork >= 2500ULL ||
            m_qsCache.stampNight == 0 || now - m_qsCache.stampNight >= 8000ULL) {
            RequestQuickSettingsCacheAsync(m_overflowWindow, kQsCacheResultMessage, m_qsPage);
        }
        if (m_qsPage == QuickSettingsPage::Power) {
            SetTimer(m_overflowWindow, kEnergySampleTimerId, 500, nullptr);
            RequestEnergyAppsAsync(m_overflowWindow, kQsEnergyResultMessage);
        } else {
            KillTimer(m_overflowWindow, kEnergySampleTimerId);
        }
        if (m_qsPage == QuickSettingsPage::Home) {
            // Temperature worker keep-alive; the worker itself polls every ~500 ms.
            SetTimer(m_overflowWindow, kQsTempsTimerId, 500, nullptr);
            RequestQuickSettingsTemps(m_overflowWindow, kQsTempsResultMessage);
        } else {
            KillTimer(m_overflowWindow, kQsTempsTimerId);
        }
        // UI live timer only publishes background peak / rate-limited media; keep
        // ~40 ms on meter pages so the underlay present stays smooth.
        const bool fastMeter = m_qsPage == QuickSettingsPage::Home ||
            m_qsPage == QuickSettingsPage::Microphone;
        SetTimer(m_overflowWindow, kQsLiveTimerId, fastMeter ? 40U : 500U, nullptr);
    }
}

UINT DockApp::DesiredTrayIntervalMs() const noexcept {
    if (IsOverflowOpen()) {
        return kTrayIntervalMs;
    }
    // No-seconds clock: wake just after the next minute instead of a fixed idle poll.
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);
    const UINT msIntoMinute =
        static_cast<UINT>(localTime.wSecond) * 1000U + static_cast<UINT>(localTime.wMilliseconds);
    const UINT msToBoundary = (msIntoMinute >= 60000U) ? 1000U : (60000U - msIntoMinute);
    // Small pad so GetTimeFormatEx sees the new minute; clamp away from 0.
    return std::clamp(msToBoundary + 75U, 200U, 60000U);
}

void DockApp::StartTrayTimer() noexcept {
    if (m_window != nullptr) {
        SetTimer(m_window, kTrayTimerId, DesiredTrayIntervalMs(), nullptr);
    }
}

void DockApp::StopTrayTimer() noexcept {
    if (m_window != nullptr) {
        KillTimer(m_window, kTrayTimerId);
    }
}

void DockApp::UpdatePrimaryMonitor() {
    const HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO information{sizeof(information)};
    if (GetMonitorInfoW(primary, &information) == FALSE) {
        throw std::runtime_error("GetMonitorInfoW for primary monitor failed.");
    }
    if (m_primaryBounds.left != information.rcMonitor.left ||
        m_primaryBounds.top != information.rcMonitor.top ||
        m_primaryBounds.right != information.rcMonitor.right ||
        m_primaryBounds.bottom != information.rcMonitor.bottom) {
        m_workAreaExpanded = false;
    }
    m_primaryBounds = information.rcMonitor;

    RECT hostBounds{};
    if (m_hostMonitor != nullptr && MonitorRect(m_hostMonitor, hostBounds)) {
        m_hostBounds = hostBounds;
    } else {
        m_hostMonitor = primary;
        m_hostBounds = m_primaryBounds;
    }
}

UINT DockApp::HostDpi() const noexcept {
    if (m_hostMonitor != nullptr) {
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (SUCCEEDED(GetDpiForMonitor(m_hostMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
            return dpiX;
        }
    }
    if (m_window != nullptr) {
        return std::max(GetDpiForWindow(m_window), 96U);
    }
    return 96;
}

bool DockApp::AdoptMonitorForCursor(POINT cursor) {
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    RECT bounds{};
    if (!MonitorRect(monitor, bounds)) {
        return false;
    }
    if (m_hostMonitor == monitor && RectsEqual(m_hostBounds, bounds)) {
        return false;
    }

    const UINT previousDpi = HostDpi();
    m_hostMonitor = monitor;
    m_hostBounds = bounds;
    RebuildLayout(previousDpi != HostDpi());
    PositionOverlayWindows();
    if (m_visibility == VisibilityState::Visible) {
        static_cast<void>(CaptureLiveBackdrop());
        QueueRenderFrame();
    } else if (m_visibility == VisibilityState::Showing) {
        const RECT captureBounds{m_windowX, m_visibleY,
            m_windowX + static_cast<LONG>(m_dockWidth),
            m_visibleY + static_cast<LONG>(m_dockHeight)};
        static_cast<void>(m_renderer.CaptureBackdrop(captureBounds));
        QueueRenderFrame();
    }
    return true;
}

bool DockApp::CaptureLiveBackdrop() {
    ProfileScope scope("CaptureLiveBackdrop");
    m_backdropCaptureWasIdle = false;
    if (m_shellFlyoutHold || !m_rendererInitialized || m_visibility == VisibilityState::Hidden) {
        return false;
    }
    // Always BitBlt while visible: GetDC(nullptr)+SRCCOPY reads the composed
    // desktop (wallpaper / windows behind the dock strip) and does not need
    // UIPI access to an elevated foreground app. Skipping here when Task
    // Manager was FG left frost/lensing blank or frozen for the whole session.
    // Elevated-FG cursor lag is handled by the 33 ms watch + WinEvent sync, not
    // by starving the glass timer.
    const RECT captureBounds{m_windowX, m_currentY,
        m_windowX + static_cast<LONG>(m_dockWidth),
        m_currentY + static_cast<LONG>(m_dockHeight)};

    // Affinity stays WDA_NONE so a recording sees the dock. CaptureBackdrop
    // omits this HWND from the glass sample without toggling that.
    bool changed = true;
    const bool captured = m_renderer.CaptureBackdrop(captureBounds, &changed);
    m_backdropCaptureWasIdle = captured && !changed;
    return captured && changed;
}

void DockApp::BeginShow() {
    ProfileScope scope("BeginShow");
    if (m_shellFlyoutHold || m_visibility == VisibilityState::Showing ||
        m_visibility == VisibilityState::Visible) {
        return;
    }

    HideHoverLabel();
    ++m_showSessionId;

    // Drain a catalog refresh that finished while Hidden so the first revealed
    // frame already matches the current app set (avoids a mid-slide width jump).
    {
        bool havePending = false;
        {
            const std::lock_guard lock(m_refreshSnapshotMutex);
            havePending = m_refreshSnapshot.has_value();
        }
        if (havePending) {
            ApplyBackgroundRefresh(m_refreshGeneration.load());
        }
    }

    // Fresh desktop under the dock before the first painted frame. The prior
    // hot-path diet started the slide on the cached backdrop and let the timer
    // catch up, which left a few frames of stale frost after the user hid the
    // dock, scrolled, and revealed again. Capture while still SW_HIDE at the
    // visible rect (~6ms typical), including when an elevated app (Task Manager)
    // is FG - desktop BitBlt does not require UIPI into that process.
    m_renderer.InvalidateBackdrop();
    if (m_rendererInitialized) {
        const RECT captureBounds{m_windowX, m_visibleY,
            m_windowX + static_cast<LONG>(m_dockWidth),
            m_visibleY + static_cast<LONG>(m_dockHeight)};
        static_cast<void>(m_renderer.CaptureBackdrop(captureBounds));
    }

    // Tray COM/IPC refresh stays deferred (can be 2-12ms); first paint only
    // needs the backdrop above.
    m_visibility = VisibilityState::Showing;
    // Park the renderer over the slide before it is shown, so capture sees the
    // reveal instead of a moving HWND.
    PositionOverlayWindows();
    ShowWindow(m_window, SW_SHOWNOACTIVATE);
    ShowWindow(m_inputWindow, SW_SHOWNOACTIVATE);
    m_animationFromY = m_currentY;
    m_animationToY = m_visibleY;
    m_animationStartedAt = QpcSeconds();
    StartCursorWatch();
    QueueRenderFrame(false);
    PostMessageW(m_window, kBeginShowDeferredMessage, static_cast<WPARAM>(m_showSessionId), 0);
}

void DockApp::BeginHide() {
    ProfileScope scope("BeginHide");
    if (m_visibility == VisibilityState::Hidden || m_visibility == VisibilityState::Hiding) {
        return;
    }

    ++m_showSessionId;
    ++m_refreshGeneration;
    m_renderQueued = false;
    StopShellFlyoutWatch();
    CancelDeferredRefresh();
    if (GetCapture() == m_inputWindow) {
        ReleaseCapture();
    }
    ClearPressState();
    m_dropPresentPending = false;
    HideDragGhost();
    HideHoverLabel();
    BeginOverflowHide(false);
    CloseContextMenu();
    StopRefreshTimer();
    StopTrayTimer();
    StopBackdropTimer();
    StopGlint();
    m_visibility = VisibilityState::Hiding;
    ShowWindow(m_inputWindow, SW_HIDE);
    // Grow the renderer downward before the first tick so the slide is an
    // offset inside a stationary window.
    PositionOverlayWindows();
    m_animationFromY = m_currentY;
    m_animationToY = m_hiddenY;
    m_animationStartedAt = QpcSeconds();
}

void DockApp::AdvanceAnimation() {
    ProfileScope scope("AdvanceAnimation");
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
        PositionOverlayWindows();
        StartRefreshTimer();
        StartTrayTimer();
        StartBackdropTimer();
        ScheduleDeferredRefresh();
        UpdateHoverLabel();
        UpdateGlintTarget(m_lastCursor);
        return;
    }

    m_visibility = VisibilityState::Hidden;
    m_currentY = m_hiddenY;
    StopBackdropTimer();
    StopGlint();
    // Keep the 15 s catalog timer while Hidden so new apps still reshape the
    // dock off-screen. Tray/backdrop stay stopped (nothing is painted).
    StartRefreshTimer();
    StopTrayTimer();
    BeginOverflowHide(false);
    CloseContextMenu();
    ClearPressState();
    HideHoverLabel();
    ShowWindow(m_inputWindow, SW_HIDE);
    ShowWindow(m_window, SW_HIDE);
    PositionOverlayWindows();
    SyncCursorWatchInterval();
}

void DockApp::QueueRenderFrame(bool allowBlockingGpuWait) {
    // Sticky non-blocking: once any coalesced request opts out of the vsync/GPU
    // wait, the shared frame stays non-blocking until it is consumed.
    if (!allowBlockingGpuWait) {
        m_renderAllowBlockingGpuWait = false;
    }
    if (m_renderQueued || !m_rendererInitialized || m_visibility == VisibilityState::Hidden) {
        return;
    }
    m_renderQueued = true;
    PostMessageW(m_window, kRenderMessage, 0, 0);
}

bool DockApp::RenderFrame(bool allowBlockingGpuWait) {
    ProfileScope scope("RenderFrame");
    if (!m_rendererInitialized || m_visibility == VisibilityState::Hidden) {
        return false;
    }

    for (size_t index = 0; index < m_iconRenderData.size(); ++index) {
        DockIconRenderData& icon = m_iconRenderData[index];
        icon.hovered = m_draggedIcon < 0 && static_cast<int>(index) == m_hoveredIcon;
        icon.dragged = static_cast<int>(index) == m_draggedIcon;
        // Reuse the pressed 50% darken path for Trash OLE drag-over feedback.
        icon.pressed = m_draggedIcon < 0 &&
            (static_cast<int>(index) == m_pressedIcon ||
                (m_trashDragOver && static_cast<int>(index) == m_trashIndex));
    }

    DockRenderState state;
    state.width = m_dockWidth;
    state.height = m_dockHeight;
    // FrostAmount 0 = clear glass (legacy toggle off): translucent optics.
    // FrostAmount 1 = full mica (legacy toggle on): opaque frosted plate.
    // In between, alpha and blur strength track the slider continuously.
    const float frostAmount = m_config.FrostAmount();
    state.glassAlpha = DOCK_GLASS_ALPHA + (1.0f - DOCK_GLASS_ALPHA) * frostAmount;
    UINT glassFx = 0;
    if (m_config.RimLight()) {
        glassFx |= DOCK_FX_RIM;
    }
    if (m_config.Lensing()) {
        glassFx |= DOCK_FX_LENS;
    }
    if (m_config.Dispersion()) {
        glassFx |= DOCK_FX_DISPERSION;
    }
    if (frostAmount > 0.001f) {
        glassFx |= DOCK_FX_BLUR;
    }
    // Tint is no longer a settings toggle Ã¢â‚¬â€ the calibrated face tone map is
    // always applied; keep the FX bit set so older shader paths stay armed.
    glassFx |= DOCK_FX_TINT;
    if (m_config.Specular()) {
        glassFx |= DOCK_FX_SPECULAR;
    }
    if (m_config.DropShadow()) {
        glassFx |= DOCK_FX_SHADOW;
    }
    if (m_config.DepthShade()) {
        glassFx |= DOCK_FX_THICKNESS;
    }
    // Pack live icon count in bits 8-13 (halos) and frost amount 0..255 in
    // bits 16-23 so the glass/blur shaders can scale mica continuously.
    // Low 8 bits stay the DOCK_FX_* toggle mask.
    const UINT haloSlots = std::min(static_cast<UINT>(m_iconRenderData.size()), 63u);
    const UINT frostByte = static_cast<UINT>(std::lround(std::clamp(frostAmount, 0.0f, 1.0f) * 255.0f));
    state.fxFlags = (frostByte << 16) | (haloSlots << 8) | glassFx;
    state.dockScale = m_dockScale;
    if (m_config.Specular() && m_glintStrength > 0.001F) {
        state.glintX = m_glintX;
        state.glintY = m_glintY;
        state.glintStrength = m_glintStrength;
    }
    state.showDevBounds = m_config.ShowDevBounds();
    // Glint easing frames are cosmetic: drop one rather than wait on the GPU
    // (the settle frame in TickGlint is allowed to wait).
    state.skipIfGpuBusy = m_dropPresentPending || (IsDragActive() && !m_dragSnapAnimating) ||
        (m_glintTimerRunning && !IsAnimating());
    state.allowBlockingGpuWait = allowBlockingGpuWait && !IsAnimating() && !IsDragActive() &&
        !m_dragSnapAnimating && !m_dropPresentPending;
    state.icons = m_iconRenderData;
    return m_renderer.Render(state);
}

void DockApp::OnBackdropTick() {
    if (m_visibility == VisibilityState::Hidden ||
        m_visibility == VisibilityState::Hiding) {
        StopBackdropTimer();
        return;
    }
    if (m_rendererInitialized) {
        m_renderer.PollDesktopChanges();
    }
    bool wantFast = false;
    if (m_visibility == VisibilityState::Visible && !IsDragActive() &&
        QpcSeconds() >= m_suppressBackdropUntil) {
        if (CaptureLiveBackdrop()) {
            QueueRenderFrame(false);
            wantFast = true;
            m_backdropIdleStreak = 0;
        } else if (m_backdropCaptureWasIdle) {
            ++m_backdropIdleStreak;
        } else {
            wantFast = true;
            m_backdropIdleStreak = 0;
        }
    }
    if (TickLivePopupGlass()) {
        wantFast = true;
        m_backdropIdleStreak = 0;
    }
    if (m_rendererInitialized) {
        m_renderer.FinishDesktopChangePoll();
    }
    const bool holdFast = wantFast || m_backdropIdleStreak < kBackdropIdleHysteresisTicks;
    SyncBackdropTimerInterval(holdFast, !holdFast);
}

void DockApp::StartBackdropTimer() noexcept {
    m_backdropIdleStreak = 0;
    m_backdropTimerAppliedMs = 0;
    if (m_backdropFastArmed) {
        DisarmBackdropFastTimer();
    }
    SyncBackdropTimerInterval(true);
}

void DockApp::StopBackdropTimer() noexcept {
    m_backdropTimerAppliedMs = 0;
    m_backdropIdleStreak = 0;
    DisarmBackdropFastTimer();
    if (m_window != nullptr) {
        KillTimer(m_window, kBackdropTimerId);
    }
}

bool DockApp::ArmBackdropFastTimer() noexcept {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    if (m_backdropWaitable == nullptr) {
        m_backdropWaitable = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (m_backdropWaitable == nullptr) {
        return false;
    }
    LARGE_INTEGER due{};
    due.QuadPart = -80000LL;
    if (SetWaitableTimer(m_backdropWaitable, &due, 8, nullptr, nullptr, FALSE) == FALSE) {
        return false;
    }
    m_backdropFastArmed = true;
    return true;
}

void DockApp::DisarmBackdropFastTimer() noexcept {
    if (m_backdropWaitable != nullptr) {
        CancelWaitableTimer(m_backdropWaitable);
    }
    m_backdropFastArmed = false;
}

void DockApp::SyncBackdropTimerInterval(bool wantFast, bool wantResting) noexcept {
    if (m_window == nullptr) {
        return;
    }
    if (wantFast) {
        if (m_backdropFastArmed) {
            return;
        }
        KillTimer(m_window, kBackdropTimerId);
        m_backdropTimerAppliedMs = kBackdropIntervalMs;
        if (!ArmBackdropFastTimer()) {
            m_backdropFastArmed = false;
            SetTimer(m_window, kBackdropTimerId, kBackdropIntervalMs, nullptr);
        }
        return;
    }
    if (m_backdropFastArmed) {
        DisarmBackdropFastTimer();
        m_backdropTimerAppliedMs = 0;
    }
    const UINT desired = wantResting ? kBackdropRestingIntervalMs : kBackdropIdleIntervalMs;
    if (desired == m_backdropTimerAppliedMs) {
        return;
    }
    m_backdropTimerAppliedMs = desired;
    SetTimer(m_window, kBackdropTimerId, desired, nullptr);
}

void DockApp::HandlePointer(POINT cursor) {
    if (cursor.x != m_lastCursor.x || cursor.y != m_lastCursor.y) {
        m_lastPointerMotionAt = QpcSeconds();
    }
    m_lastCursor = cursor;
    m_lastPointerSampleAt = QpcSeconds();
    UpdateGlintTarget(cursor);
    if (IsDragActive()) {
        if (m_draggedIcon >= 0) {
            UpdateDrag(cursor);
        }
        return;
    }

    const bool inHotZone = IsCursorInBottomHotZone(cursor);
    const bool borderlessFullscreen = IsForegroundBorderlessFullscreenAt(cursor);
    if (m_shellFlyoutHold) {
        return;
    }
    if (inHotZone && !borderlessFullscreen) {
        static_cast<void>(AdoptMonitorForCursor(cursor));
    }

    if (m_visibility == VisibilityState::Hidden) {
        HideHoverLabel();
        if (inHotZone && !borderlessFullscreen) {
            BeginShow();
        }
        return;
    }

    if (m_visibility == VisibilityState::Hiding && inHotZone && !borderlessFullscreen) {
        BeginShow();
        return;
    }

    // If a borderless fullscreen game/app took focus while the dock was up,
    // slide away unless the user is actively over the dock or a flyout.
    if ((m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Visible) &&
        borderlessFullscreen && !IsLaunchPromptOpen() && !IsCursorOverDock(cursor) &&
        !IsCursorWithinFlyoutZone(cursor) && !IsOverflowOpen() && !IsDockSettingsOpen() &&
        !IsContextMenuOpen()) {
        BeginHide();
        return;
    }

    if ((m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Visible) &&
        !inHotZone && !IsLaunchPromptOpen() && !IsCursorOverDock(cursor) &&
        !IsCursorWithinFlyoutZone(cursor) && !IsOverflowOpen() &&
        (cursor.y < m_visibleY + DockShadowMarginPx(static_cast<float>(HostDpi()) / 96.0F) ||
            MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST) != m_hostMonitor)) {
        // Quick Settings stays up when a drill-in resizes out from under the
        // cursor. Dismiss is an outside click, Escape, or Back - not mouse-leave.
        BeginHide();
        return;
    }

    const int hovered = IconAtScreenPoint(cursor);
    const int hoveredDivider = DividerAtScreenPoint(cursor);
    if (hovered == m_hoveredIcon && hoveredDivider == m_hoveredDivider) {
        return;
    }
    // Commit both hits before the label decision. Unpinned icons sit past the
    // separator, so the event that enters one still had the divider hovered;
    // UpdateHoverLabel bails while that flag is set, and a later event with
    // the same icon never retries.
    m_hoveredIcon = hovered;
    m_hoveredDivider = hoveredDivider;
    UpdateHoverLabel();
}

void DockApp::UpdateGlintTarget(POINT cursor) {
    // Visible only: the show/hide slide renders every frame itself and must
    // never share the thread or frame-skip policy with glint easing.
    const bool active = m_visibility == VisibilityState::Visible && !IsDragActive() &&
        m_config.Specular() && IsCursorOverDock(cursor);
    if (active) {
        const POINT client = ClientFromDockOrigin(cursor, m_windowX, m_currentY);
        m_glintTargetX = static_cast<float>(client.x);
        m_glintTargetY = static_cast<float>(client.y);
        if (m_glintStrength <= 0.001F) {
            // Fade in at the cursor instead of sliding from the last exit point.
            m_glintX = m_glintTargetX;
            m_glintY = m_glintTargetY;
        }
    }
    m_glintTargetStrength = active ? 1.0F : 0.0F;
    const bool fadedOut = m_glintTargetStrength <= 0.0F && m_glintStrength <= 0.001F;
    const bool atTarget = std::abs(m_glintTargetX - m_glintX) < 0.5F &&
        std::abs(m_glintTargetY - m_glintY) < 0.5F &&
        std::abs(m_glintTargetStrength - m_glintStrength) < 0.002F;
    if (fadedOut || atTarget || m_glintTimerRunning || m_window == nullptr ||
        !m_rendererInitialized) {
        return;
    }
    m_glintLastTickAt = QpcSeconds();
    m_glintTimerRunning = true;
    SetTimer(m_window, kGlintTimerId, kGlintIntervalMs, nullptr);
}

void DockApp::TickGlint() {
    if (m_visibility != VisibilityState::Visible) {
        StopGlint();
        return;
    }
    const double now = QpcSeconds();
    const float dt = static_cast<float>(std::clamp(now - m_glintLastTickAt, 0.0, 0.1));
    m_glintLastTickAt = now;
    // Exponential easing: the glint trails the cursor slightly (liquid feel)
    // and fades in/out over ~0.2 s.
    constexpr float kFollowSeconds = 0.06F;
    constexpr float kFadeSeconds = 0.18F;
    const float follow = 1.0F - std::exp(-dt / kFollowSeconds);
    const float fade = 1.0F - std::exp(-dt / kFadeSeconds);
    m_glintX += (m_glintTargetX - m_glintX) * follow;
    m_glintY += (m_glintTargetY - m_glintY) * follow;
    m_glintStrength += (m_glintTargetStrength - m_glintStrength) * fade;

    const bool positionSettled = (m_glintTargetStrength <= 0.0F && m_glintStrength < 0.002F) ||
        (std::abs(m_glintTargetX - m_glintX) < 0.5F && std::abs(m_glintTargetY - m_glintY) < 0.5F);
    const bool strengthSettled = std::abs(m_glintTargetStrength - m_glintStrength) < 0.002F;
    if (positionSettled && strengthSettled) {
        m_glintX = m_glintTargetX;
        m_glintY = m_glintTargetY;
        m_glintStrength = m_glintTargetStrength;
        KillTimer(m_window, kGlintTimerId);
        m_glintTimerRunning = false;
    }
    QueueRenderFrame(false);
    // BakeGlassPanel is expensive; rebake the hover label only once glint settles.
    // Icon changes already call UpdateHoverLabel().
    if (positionSettled && strengthSettled && m_hoverLabelIcon >= 0 &&
        m_hoveredIcon == m_hoverLabelIcon && m_hoverLabelWindow != nullptr &&
        IsWindowVisible(m_hoverLabelWindow) != FALSE) {
        UpdateHoverLabel(true);
    }
}

void DockApp::StopGlint() noexcept {
    if (m_glintTimerRunning && m_window != nullptr) {
        KillTimer(m_window, kGlintTimerId);
    }
    m_glintTimerRunning = false;
    m_glintStrength = 0.0F;
    m_glintTargetStrength = 0.0F;
}

void DockApp::UpdateDividerScaleDrag(POINT screenCursor) {
    const UINT dpi = GetDpiForWindow(m_window);
    const float dpiScale = dpi == 0 ? 1.0F : static_cast<float>(dpi) / 96.0F;
    const float deltaY = static_cast<float>(m_scaleDragStartY - screenCursor.y);
    const float deltaScale = deltaY / (250.0F * dpiScale);
    const float newScale = std::clamp(m_scaleDragStartValue + deltaScale, kMinDockScale, kMaxDockScale);
    if (std::abs(newScale - m_dockScale) < 0.001F) {
        return;
    }

    m_dockScale = newScale;
    m_config.SetDockScale(m_dockScale);
    RebuildLayout(false);
    QueueRenderFrame();
}

std::vector<DockApp::ContextItem> DockApp::BuildContextItems(int icon, DisplayApp& outApp,
    bool& outHasApp, bool& outIsSpecial) const {
    std::vector<ContextItem> items;
    outApp = {};
    outHasApp = icon >= 0 && static_cast<size_t>(icon) < m_displayApps.size() &&
        !IsLayoutOnlyTarget(m_displayApps[static_cast<size_t>(icon)].app.target) &&
        !IsTrayRenderIndex(icon) && !IsTrashRenderIndex(icon);
    outIsSpecial = outHasApp && IsSpecialDockTarget(m_displayApps[static_cast<size_t>(icon)].app.target);
    const bool showBounds = m_config.ShowDevBounds();
    if (IsTrashRenderIndex(icon)) {
        ContextItem open;
        open.command = kContextTrashOpen;
        open.label = L"Open";
        open.glyph = L'\uE8A7';
        items.push_back(std::move(open));
        ContextItem empty;
        empty.command = kContextTrashEmpty;
        empty.label = L"Empty Recycle Bin";
        empty.glyph = L'\uE74D';
        empty.disabled = m_trashState.isEmpty || m_trashState.itemCount == 0;
        items.push_back(std::move(empty));
        ContextItem props;
        props.command = kContextTrashProperties;
        props.label = L"Properties";
        props.glyph = L'\uE90F';
        props.separatorBefore = true;
        items.push_back(std::move(props));
        return items;
    }
    if (outHasApp) {
        outApp = m_displayApps[static_cast<size_t>(icon)];
        if (outIsSpecial) {
            ContextItem open;
            open.command = kContextOpen;
            open.label = L"Open";
            open.glyph = L'\uE8A7';
            items.push_back(std::move(open));
        } else {
            const bool running = outApp.runningWindow != nullptr;
            ContextItem pin;
            if (outApp.persistentPinIndex >= 0) {
                pin.command = kContextUnpin;
                pin.label = L"Unpin from taskbar";
                pin.glyph = L'\uE719';
            } else {
                pin.command = kContextPin;
                pin.label = L"Pin to taskbar";
                pin.glyph = L'\uE718';
            }
            items.push_back(std::move(pin));
            if (running) {
                ContextItem end;
                end.command = kContextEndTask;
                end.label = L"End task";
                end.glyph = L'\uE71A';
                items.push_back(std::move(end));
                ContextItem close;
                close.command = kContextClose;
                close.label = L"Close window";
                close.glyph = L'\uE8BB';
                items.push_back(std::move(close));
            }
            if (outApp.app.target.rfind(L"shell:", 0) != 0) {
                ContextItem location;
                location.command = kContextOpenLocation;
                location.label = L"Open location";
                location.glyph = L'\uE825';
                location.separatorBefore = true;
                items.push_back(std::move(location));
            }
        }
    } else {
        ContextItem foreground;
        foreground.command = kContextPinForeground;
        foreground.label = L"Pin foreground application";
        foreground.glyph = L'\uE718';
        items.push_back(std::move(foreground));
    }
    ContextItem bounds;
    bounds.command = kContextToggleBounds;
    bounds.label = showBounds ? L"Hide developer bounds" : L"Show developer bounds";
    bounds.glyph = L'\uE943';
    bounds.separatorBefore = true;
    items.push_back(std::move(bounds));
    return items;
}

void DockApp::HandleContextMenu(POINT screenPoint) {
    const int icon = IconAtScreenPoint(screenPoint);
    // Toggle: right-clicking while the menu is open for the same spot just
    // re-anchors; a plain re-open keeps hover state sane.
    ShowContextMenu(screenPoint, icon);
}

void DockApp::ShowContextMenu(POINT screenPoint, int icon) {
    DisplayApp app;
    bool hasApp = false;
    bool isSpecial = false;
    std::vector<ContextItem> items = BuildContextItems(icon, app, hasApp, isSpecial);

    // Anchor to the clicked icon's screen center so the menu sits above it
    // like the concept; fall back to the cursor for empty glass.
    POINT anchor = screenPoint;
    if (icon >= 0 && static_cast<size_t>(icon) < m_iconRenderData.size()) {
        RECT bounds = m_iconRenderData[static_cast<size_t>(icon)].bounds;
        POINT topLeft{bounds.left, bounds.top};
        POINT bottomRight{bounds.right, bounds.bottom};
        if (ClientToScreen(m_window, &topLeft) != FALSE &&
            ClientToScreen(m_window, &bottomRight) != FALSE) {
            anchor.x = topLeft.x + (bottomRight.x - topLeft.x) / 2L;
            anchor.y = topLeft.y;
        }
    }

    CloseContextMenu();
    BeginOverflowHide(false);
    CloseDockSettings();
    m_contextItems = std::move(items);
    m_contextApp = app;
    m_contextHasApp = hasApp;
    m_contextIsSpecial = isSpecial;
    m_contextIsTrash = IsTrashRenderIndex(icon);
    m_contextAnchor = anchor;
    m_contextHover = -1;
    InvalidateContextGlass();
    HideHoverLabel();
    PaintContextMenu();
    SyncCursorWatchInterval();
}

void DockApp::CloseContextMenu() noexcept {
    m_contextHover = -1;
    m_contextIsTrash = false;
    m_contextPaintQueued = false;
    m_contextHoverPaintOnly = false;
    if (m_contextWindow != nullptr) {
        ShowWindow(m_contextWindow, SW_HIDE);
    }
    InvalidateContextGlass();
    std::vector<uint8_t>().swap(m_contextBaseBits);
    std::vector<uint8_t>().swap(m_contextPresentBits);
    m_contextPresentSize = {};
    SyncCursorWatchInterval();
}

void DockApp::DestroyContextMenu() noexcept {
    ReleaseLayerPresentDib(m_contextLayerDib);
    CloseContextMenu();
    m_contextItems.clear();
    m_contextHits.clear();
    m_contextGlyphs.clear();
    DestroyContextFonts();
    if (m_contextWindow != nullptr) {
        DestroyWindow(m_contextWindow);
        m_contextWindow = nullptr;
    }
}

void DockApp::PositionContextMenu() {
    if (m_contextWindow == nullptr || !IsContextMenuOpen()) {
        return;
    }
    // The menu floats above the dock, so slide animation must carry it along.
    // Recompute the origin from the current dock Y; a move needs fresh glass.
    const POINT previous = m_contextOrigin;
    POINT origin{};
    if (!ContextScreenOrigin(origin)) {
        return;
    }
    if (origin.x != previous.x || origin.y != previous.y) {
        InvalidateContextGlass();
        PaintContextMenu();
        return;
    }
    if (!m_contextPresentBits.empty()) {
        PaintContextHoverFast();
        return;
    }
    SetWindowPos(m_contextWindow, HWND_TOPMOST, SaturatedInt(origin.x), SaturatedInt(origin.y),
        SaturatedInt(m_contextSize.cx), SaturatedInt(m_contextSize.cy), SWP_NOACTIVATE);
}

bool DockApp::ContextScreenOrigin(POINT& origin) const {
    if (m_contextSize.cx <= 0 || m_contextSize.cy <= 0) {
        return false;
    }
    const UINT dpi = HostDpi();
    const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
    const LONG gap = std::max(8L, std::lround(10.0F * scale));
    const LONG margin = std::max(8L, std::lround(8.0F * scale));
    const LONG shadowMargin = DockShadowMarginPx(scale);
    LONG x = m_contextAnchor.x - m_contextSize.cx / 2L;
    LONG y = m_currentY + shadowMargin - gap - m_contextSize.cy;
    // If the dock geometry is not ready yet, fall back above the cursor.
    if (m_currentY == 0) {
        y = m_contextAnchor.y - gap - m_contextSize.cy;
    }
    const LONG minX = m_hostBounds.left + margin;
    const LONG maxX = m_hostBounds.right - m_contextSize.cx - margin;
    if (maxX >= minX) {
        x = std::clamp(x, minX, maxX);
    } else {
        x = minX;
    }
    y = std::max(m_hostBounds.top + margin, y);
    origin = {x, y};
    // Remember for the glass cache + layered present.
    const_cast<DockApp*>(this)->m_contextOrigin = origin;
    return true;
}

int DockApp::ContextHitIndex(POINT point) const noexcept {
    for (size_t index = 0; index < m_contextHits.size(); ++index) {
        if (IsInside(m_contextHits[index].bounds, point.x, point.y)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool DockApp::IsContextMenuOpen() const noexcept {
    return m_contextWindow != nullptr && IsWindowVisible(m_contextWindow) != FALSE &&
        !m_contextItems.empty();
}

bool DockApp::IsCursorOverContextMenu(POINT cursor) const noexcept {
    if (!IsContextMenuOpen()) {
        return false;
    }
    RECT bounds{};
    GetWindowRect(m_contextWindow, &bounds);
    return IsInside(bounds, cursor.x, cursor.y);
}

void DockApp::InvalidateContextGlass() noexcept {
    std::vector<uint8_t>().swap(m_contextGlass);
    m_contextGlassSize = {};
    m_contextGlassOrigin = {};
    m_contextGlassFrost = -1.0F;
}

bool DockApp::ContextGlassValid(POINT origin) const noexcept {
    return !m_contextGlass.empty() && m_contextGlassSize.cx == m_contextSize.cx &&
        m_contextGlassSize.cy == m_contextSize.cy && m_contextGlassOrigin.x == origin.x &&
        m_contextGlassOrigin.y == origin.y &&
        m_contextGlass.size() ==
            static_cast<size_t>(m_contextSize.cx) * static_cast<size_t>(m_contextSize.cy) * 4U &&
        std::abs(m_contextGlassFrost - m_config.FrostAmount()) < 0.001F;
}

void DockApp::EnsureContextFonts(float scale) {
    if (m_contextLabelFont != nullptr && m_contextFontScale == scale) {
        return;
    }
    DestroyContextFonts();
    m_contextLabelFont = CreateFlyoutFont(std::max(14, static_cast<int>(std::lround(15.0F * scale))),
        FW_NORMAL);
    m_contextFontScale = scale;
}

void DockApp::DestroyContextFonts() noexcept {
    if (m_contextLabelFont != nullptr) {
        DeleteObject(m_contextLabelFont);
        m_contextLabelFont = nullptr;
    }
    m_contextFontScale = 0.0F;
}

void DockApp::QueueContextPaint(bool hoverOnly) {
    if (m_contextWindow == nullptr || !IsContextMenuOpen()) {
        return;
    }
    if (!hoverOnly) {
        m_contextHoverPaintOnly = false;
    } else if (!m_contextPaintQueued) {
        m_contextHoverPaintOnly = true;
    }
    if (m_contextPaintQueued) {
        return;
    }
    m_contextPaintQueued = true;
    PostMessageW(m_window, kContextPaintMessage, 0, 0);
}

void DockApp::PaintContextHoverFast() {
    if (m_contextBaseBits.empty() || m_contextWindow == nullptr ||
        m_contextPresentSize.cx <= 0 || m_contextPresentSize.cy <= 0 ||
        m_contextPresentSize.cx != m_contextSize.cx ||
        m_contextPresentSize.cy != m_contextSize.cy) {
        PaintContextMenu();
        return;
    }
    POINT origin = m_contextOrigin;
    if (origin.x == 0 && origin.y == 0) {
        if (!ContextScreenOrigin(origin)) {
            return;
        }
    }
    const int width = SaturatedInt(m_contextPresentSize.cx);
    const int height = SaturatedInt(m_contextPresentSize.cy);
    m_contextPresentSize = m_contextSize;
    if (m_contextPresentBits.size() != m_contextBaseBits.size()) {
        m_contextPresentBits = m_contextBaseBits;
        m_contextHoverDirtyValid = false;
    }

    auto restoreRect = [&](const RECT& rect) {
        const LONG left = (std::max)(0L, rect.left);
        const LONG top = (std::max)(0L, rect.top);
        const LONG right = (std::min)(static_cast<LONG>(width), rect.right);
        const LONG bottom = (std::min)(static_cast<LONG>(height), rect.bottom);
        if (right <= left || bottom <= top) {
            return;
        }
        const size_t rowBytes = static_cast<size_t>(right - left) * 4U;
        for (LONG y = top; y < bottom; ++y) {
            const size_t offset =
                (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(left)) * 4U;
            std::memcpy(m_contextPresentBits.data() + offset, m_contextBaseBits.data() + offset,
                rowBytes);
        }
    };

    RECT dirty{};
    bool haveDirty = false;
    if (m_contextHoverDirtyValid) {
        RECT prev = m_contextHoverDirty;
        prev.left = (std::max)(0L, prev.left - 8);
        prev.top = (std::max)(0L, prev.top - 8);
        prev.right = (std::min)(static_cast<LONG>(width), prev.right + 8);
        prev.bottom = (std::min)(static_cast<LONG>(height), prev.bottom + 8);
        restoreRect(prev);
        dirty = prev;
        haveDirty = true;
    }

    if (m_contextHover >= 0 && static_cast<size_t>(m_contextHover) < m_contextHits.size() &&
        static_cast<size_t>(m_contextHover) < m_contextItems.size() &&
        static_cast<size_t>(m_contextHover) < m_contextGlyphs.size()) {
        const RECT& hit = m_contextHits[static_cast<size_t>(m_contextHover)].bounds;
        const UINT dpi = HostDpi();
        const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
        const float radius = std::max(8.0F, 10.0F * scale);
        const float cxL = static_cast<float>(hit.left) + radius;
        const float cxR = static_cast<float>(hit.right) - radius;
        const float cy = 0.5F * static_cast<float>(hit.top + hit.bottom);
        // Concept hover: pale periwinkle blue (#DDE6FB-ish in BGR).
        FillPillColorPremul(m_contextPresentBits.data(), width, height, cxL, cxR, cy, radius, 0.85F,
            251, 234, 221);
        // Re-draw the hovered row over the highlight so glyph + text stay crisp.
        const LONG outerPad = std::max(6L, std::lround(8.0F * scale));
        const LONG rowHeight = std::max(36L, std::lround(44.0F * scale));
        const LONG iconBox = std::max(28L, std::lround(32.0F * scale));
        const LONG iconGap = std::max(8L, std::lround(10.0F * scale));
        const UINT glyphExtent =
            static_cast<UINT>(std::max(14L, std::lround(20.0F * scale)));
        const LONG panelWidth = SaturatedInt(m_contextSize.cx);
        const size_t rowIndex = static_cast<size_t>(m_contextHover);
        const ContextItem& item = m_contextItems[rowIndex];
        const LONG rowTop = hit.top;
        const std::vector<uint8_t>& glyph = m_contextGlyphs[rowIndex];
        if (!glyph.empty()) {
            const LONG iconLeft = outerPad + (iconBox - static_cast<LONG>(glyphExtent)) / 2L;
            const LONG iconTop = rowTop + (rowHeight - static_cast<LONG>(glyphExtent)) / 2L;
            CompositePremul(m_contextPresentBits.data(), width, height, SaturatedInt(iconLeft),
                SaturatedInt(iconTop), glyph.data(), SaturatedInt(glyphExtent),
                SaturatedInt(glyphExtent));
        }
        RECT label{outerPad + iconBox + iconGap, rowTop, panelWidth - outerPad - 8,
            rowTop + rowHeight};
        DrawFlyoutText(m_contextPresentBits.data(), width, height, label, m_contextLabelFont,
            item.label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 250);
        RECT next = hit;
        next.left = (std::max)(0L, next.left - 8);
        next.top = (std::max)(0L, next.top - 8);
        next.right = (std::min)(static_cast<LONG>(width), next.right + 8);
        next.bottom = (std::min)(static_cast<LONG>(height), next.bottom + 8);
        if (!haveDirty) {
            dirty = next;
            haveDirty = true;
        } else {
            dirty.left = (std::min)(dirty.left, next.left);
            dirty.top = (std::min)(dirty.top, next.top);
            dirty.right = (std::max)(dirty.right, next.right);
            dirty.bottom = (std::max)(dirty.bottom, next.bottom);
        }
        m_contextHoverDirty = hit;
        m_contextHoverDirtyValid = true;
    } else {
        m_contextHoverDirtyValid = false;
    }

    if (!PresentLayeredBits(m_contextWindow, origin, m_contextPresentSize.cx, m_contextPresentSize.cy,
            m_contextPresentBits.data(), m_contextPresentBits.size(), m_contextLayerDib,
            haveDirty ? &dirty : nullptr)) {
        return;
    }
    if (!IsWindowVisible(m_contextWindow)) {
        ShowWindow(m_contextWindow, SW_SHOWNA);
    }
}

void DockApp::PaintContextMenu() {
    if (m_contextItems.empty()) {
        return;
    }
    const UINT dpi = HostDpi();
    const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
    EnsureContextFonts(scale);
    const LONG outerPad = std::max(6L, std::lround(8.0F * scale));
    const LONG rowHeight = std::max(36L, std::lround(44.0F * scale));
    const LONG iconBox = std::max(28L, std::lround(32.0F * scale));
    const LONG iconGap = std::max(8L, std::lround(10.0F * scale));
    const LONG sepGap = std::max(6L, std::lround(7.0F * scale));
    const LONG radius = std::max(14L, std::lround(18.0F * scale));

    // Measure the longest label so the menu hugs its content like the concept.
    LONG maxText = 0;
    {
        HDC screen = GetDC(nullptr);
        if (screen != nullptr) {
            HGDIOBJ previous = SelectObject(screen, m_contextLabelFont);
            for (const ContextItem& item : m_contextItems) {
                SIZE extent{};
                if (GetTextExtentPoint32W(screen, item.label.c_str(),
                        static_cast<int>(item.label.size()), &extent) != FALSE) {
                    maxText = std::max(maxText, extent.cx);
                }
            }
            SelectObject(screen, previous);
            ReleaseDC(nullptr, screen);
        }
    }
    if (maxText <= 0) {
        maxText = std::lround(180.0F * scale);
    }
    const LONG textPad = std::max(12L, std::lround(16.0F * scale));
    LONG panelWidth = outerPad * 2L + iconBox + iconGap + maxText + textPad;
    panelWidth = std::clamp(panelWidth, std::max(220L, std::lround(260.0F * scale)),
        std::max(300L, std::lround(360.0F * scale)));

    LONG contentY = outerPad;
    for (const ContextItem& item : m_contextItems) {
        if (item.separatorBefore) {
            contentY += sepGap * 2L + 1L;
        }
        contentY += rowHeight;
    }
    contentY += outerPad;
    m_contextSize.cx = panelWidth;
    m_contextSize.cy = contentY;

    if (m_contextWindow == nullptr) {
        const wchar_t className[] = L"LiquidGlassDockContextMenu";
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = &DockApp::ContextWindowProcedure;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = className;
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Log(L"Could not register the context menu window class.");
            return;
        }
        constexpr DWORD extendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
            WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
        m_contextWindow = CreateWindowExW(extendedStyle, className, L"", WS_POPUP, 0, 0, 1, 1,
            m_window, nullptr, m_instance, this);
        if (m_contextWindow == nullptr) {
            Log(L"Could not create the context menu window.");
            return;
        }
        EnsureWindowCapturable(m_contextWindow);
        ClearPopupLayeredCorners(m_contextWindow);
    }

    POINT origin{};
    if (!ContextScreenOrigin(origin)) {
        return;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return;
    }
    HDC memory = CreateCompatibleDC(screen);
    if (memory == nullptr) {
        ReleaseDC(nullptr, screen);
        return;
    }
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = m_contextSize.cx;
    header.bV5Height = -m_contextSize.cy;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    auto* pixels = static_cast<uint8_t*>(bits);
    const size_t pixelCount =
        static_cast<size_t>(m_contextSize.cx) * static_cast<size_t>(m_contextSize.cy);
    ReleaseDC(nullptr, screen);
    if (ContextGlassValid(origin)) {
        std::memcpy(pixels, m_contextGlass.data(), m_contextGlass.size());
    } else {
        std::memset(pixels, 0, pixelCount * 4U);
        const bool gpuGlass = TryBakePopupGlass(origin, m_contextSize.cx, m_contextSize.cy, pixels,
            pixelCount * 4U, false);
        if (!gpuGlass) {
        screen = GetDC(nullptr);
        if (screen == nullptr) {
            SelectObject(memory, previousBitmap);
            DeleteObject(bitmap);
            DeleteDC(memory);
            return;
        }
        BitBlt(memory, 0, 0, SaturatedInt(m_contextSize.cx), SaturatedInt(m_contextSize.cy), screen,
            SaturatedInt(origin.x), SaturatedInt(origin.y), SRCCOPY);
        ReleaseDC(nullptr, screen);
        const float frostAmt = (std::max)(std::clamp(m_config.FrostAmount(), 0.0F, 1.0F),
            DOCK_PANEL_FROST_BLUR_FLOOR);
        const int frostRadius = PopupFrostRadiusPx(frostAmt, scale);
        const int width = SaturatedInt(m_contextSize.cx);
        const int height = SaturatedInt(m_contextSize.cy);
        BoxBlurRgb(pixels, width, height, frostRadius);
        BoxBlurRgb(pixels, width, height, frostRadius);
        BoxBlurRgb(pixels, width, height, frostRadius);

        constexpr LONG kMaskSupersample = 2;
        const LONG maskW = m_contextSize.cx * kMaskSupersample;
        const LONG maskH = m_contextSize.cy * kMaskSupersample;
        HDC maskDc = CreateCompatibleDC(memory);
        void* maskBits = nullptr;
        HBITMAP maskBitmap = nullptr;
        if (maskDc != nullptr) {
            BITMAPV5HEADER maskHeader = header;
            maskHeader.bV5Width = maskW;
            maskHeader.bV5Height = -maskH;
            maskBitmap = CreateDIBSection(maskDc, reinterpret_cast<const BITMAPINFO*>(&maskHeader),
                DIB_RGB_COLORS, &maskBits, nullptr, 0);
        }
        if (maskDc != nullptr && maskBitmap != nullptr && maskBits != nullptr) {
            HGDIOBJ previousMask = SelectObject(maskDc, maskBitmap);
            RECT all{0, 0, maskW, maskH};
            FillRect(maskDc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            HBRUSH whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
            HPEN whitePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            HGDIOBJ previousBrush = SelectObject(maskDc, whiteBrush);
            HGDIOBJ previousPen = SelectObject(maskDc, whitePen);
            RoundRect(maskDc, 0, 0, maskW, maskH, SaturatedInt(radius * 2L * kMaskSupersample),
                SaturatedInt(radius * 2L * kMaskSupersample));
            SelectObject(maskDc, previousPen);
            SelectObject(maskDc, previousBrush);
            DeleteObject(whitePen);
            DeleteObject(whiteBrush);
            auto* mask = static_cast<uint8_t*>(maskBits);
            const LONG glassW = m_contextSize.cx;
            const LONG glassH = m_contextSize.cy;
            std::vector<float> coverage(pixelCount);
            const float kMaskSamples = static_cast<float>(kMaskSupersample * kMaskSupersample);
            for (LONG y = 0; y < glassH; ++y) {
                for (LONG x = 0; x < glassW; ++x) {
                    unsigned covered = 0;
                    for (LONG sampleY = 0; sampleY < kMaskSupersample; ++sampleY) {
                        const size_t maskRow = (static_cast<size_t>(y) * kMaskSupersample +
                            static_cast<size_t>(sampleY)) *
                            static_cast<size_t>(maskW);
                        for (LONG sampleX = 0; sampleX < kMaskSupersample; ++sampleX) {
                            covered += mask[(maskRow + static_cast<size_t>(x) * kMaskSupersample +
                                static_cast<size_t>(sampleX)) *
                                4U];
                        }
                    }
                    coverage[static_cast<size_t>(y) * glassW + x] =
                        static_cast<float>(covered) / (255.0F * kMaskSamples);
                }
            }
            std::vector<float> blurredCoverage = coverage;
            BoxBlurFloatPlane(blurredCoverage, SaturatedInt(glassW), SaturatedInt(glassH),
                std::max(1, static_cast<int>(std::lround(2.6F * scale))));
            // Same liquid-glass face as the dock (calibrated tone map + rim).
            ApplyLiquidGlassFace(pixels, SaturatedInt(glassW), SaturatedInt(glassH), coverage,
                blurredCoverage, m_config.FrostAmount(), false, false);
            SelectObject(maskDc, previousMask);
            DeleteObject(maskBitmap);
            DeleteDC(maskDc);
        }
        } // !gpuGlass
        m_contextGlass.assign(pixels, pixels + pixelCount * 4U);
        m_contextGlassSize = m_contextSize;
        m_contextGlassOrigin = origin;
        m_contextGlassFrost = m_config.FrostAmount();
    }

    const int width = SaturatedInt(m_contextSize.cx);
    const int height = SaturatedInt(m_contextSize.cy);
    const int pendingHover = m_contextHover;
    m_contextHits.clear();
    m_contextGlyphs.clear();
    m_contextGlyphs.reserve(m_contextItems.size());

    // Same charcoal plate as QS/Dock Settings: light chrome for labels/glyphs.
    SetFlyoutChromeInk(DOCK_CHROME_INK_R, DOCK_CHROME_INK_G, DOCK_CHROME_INK_B);

    const UINT glyphExtent =
        static_cast<UINT>(std::max(14L, std::lround(20.0F * scale)));
    for (const ContextItem& item : m_contextItems) {
        if (item.glyph != 0) {
            m_contextGlyphs.push_back(m_tray.RasterizeSymbol(item.glyph, glyphExtent));
            RemapPremulInkColor(m_contextGlyphs.back(), DOCK_CHROME_INK_R, DOCK_CHROME_INK_G,
                DOCK_CHROME_INK_B);
        } else {
            m_contextGlyphs.emplace_back();
        }
    }

    LONG y = outerPad;
    for (size_t index = 0; index < m_contextItems.size(); ++index) {
        const ContextItem& item = m_contextItems[index];
        if (item.separatorBefore) {
            const LONG lineY = y + sepGap;
            FillRectPremul(pixels, width, height,
                {outerPad + 4, lineY, panelWidth - outerPad - 4, lineY + 1}, 0.16F);
            y += sepGap * 2L + 1L;
        }
        const RECT rowBounds{outerPad, y, panelWidth - outerPad, y + rowHeight};
        m_contextHits.push_back({item.command, rowBounds});
        y += rowHeight;
    }

    // Base frame without hover, then hover applied to the present copy so
    // mouse moves can use the fast path.
    const size_t bytes = pixelCount * 4U;
    m_contextBaseBits.resize(bytes);
    std::memcpy(m_contextBaseBits.data(), pixels, bytes);

    // Content: icons + labels + separators already have glass; draw text/icons.
    y = outerPad;
    for (size_t index = 0; index < m_contextItems.size(); ++index) {
        const ContextItem& item = m_contextItems[index];
        if (item.separatorBefore) {
            y += sepGap * 2L + 1L;
        }
        const LONG rowTop = y;
        const LONG iconLeft = outerPad + (iconBox - static_cast<LONG>(glyphExtent)) / 2L;
        const LONG iconTop = rowTop + (rowHeight - static_cast<LONG>(glyphExtent)) / 2L;
        const std::vector<uint8_t>& glyph = m_contextGlyphs[index];
        if (!glyph.empty()) {
            CompositePremul(pixels, width, height, SaturatedInt(iconLeft), SaturatedInt(iconTop),
                glyph.data(), SaturatedInt(glyphExtent), SaturatedInt(glyphExtent));
        } else if (item.command == kContextToggleBounds) {
            RECT fallback{outerPad, rowTop, outerPad + iconBox, rowTop + rowHeight};
            DrawFlyoutText(pixels, width, height, fallback, m_contextLabelFont, L"</>",
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, 230);
        }
        RECT labelBounds{outerPad + iconBox + iconGap, rowTop, panelWidth - outerPad - 8,
            rowTop + rowHeight};
        const BYTE labelAlpha = item.disabled ? static_cast<BYTE>(110) : static_cast<BYTE>(250);
        DrawFlyoutText(pixels, width, height, labelBounds, m_contextLabelFont, item.label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, labelAlpha);
        y += rowHeight;
    }
    std::memcpy(m_contextBaseBits.data(), pixels, bytes);
    m_contextPresentBits.resize(bytes);
    std::memcpy(m_contextPresentBits.data(), pixels, bytes);
    m_contextPresentSize = m_contextSize;
    if (pendingHover >= 0 && static_cast<size_t>(pendingHover) < m_contextHits.size()) {
        const RECT& hit = m_contextHits[static_cast<size_t>(pendingHover)].bounds;
        const float hoverRadius = std::max(8.0F, 10.0F * scale);
        const float cxL = static_cast<float>(hit.left) + hoverRadius;
        const float cxR = static_cast<float>(hit.right) - hoverRadius;
        const float cy = 0.5F * static_cast<float>(hit.top + hit.bottom);
        FillPillColorPremul(m_contextPresentBits.data(), width, height, cxL, cxR, cy, hoverRadius,
            0.85F, 251, 234, 221);
        // Redraw the hovered row's content above the highlight so text stays crisp.
        // Icons/text were already in base; re-composite them onto present.
        const size_t rowIndex = static_cast<size_t>(pendingHover);
        const ContextItem& item = m_contextItems[rowIndex];
        // Re-draw highlight-aware content: copy base row then re-draw text/icon.
        // Simplest correct: re-draw text + icon from base composition.
        // Since base already has them, composite the highlight first would have
        // been ideal; instead re-draw text over the highlight now.
        LONG rowTop = m_contextHits[rowIndex].bounds.top;
        const std::vector<uint8_t>& glyph = m_contextGlyphs[rowIndex];
        if (!glyph.empty()) {
            const LONG hlIconLeft = outerPad + (iconBox - static_cast<LONG>(glyphExtent)) / 2L;
            const LONG hlIconTop = rowTop + (rowHeight - static_cast<LONG>(glyphExtent)) / 2L;
            CompositePremul(m_contextPresentBits.data(), width, height, SaturatedInt(hlIconLeft),
                SaturatedInt(hlIconTop), glyph.data(), SaturatedInt(glyphExtent),
                SaturatedInt(glyphExtent));
        }
        RECT hlLabel{outerPad + iconBox + iconGap, rowTop, panelWidth - outerPad - 8,
            rowTop + rowHeight};
        DrawFlyoutText(m_contextPresentBits.data(), width, height, hlLabel, m_contextLabelFont,
            item.label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 250);
        std::memcpy(pixels, m_contextPresentBits.data(), bytes);
    }

    POINT source{0, 0};
    POINT destination = origin;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(m_contextWindow, nullptr, &destination, &m_contextSize, memory, &source,
        0, &blend, ULW_ALPHA);
    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    // m_contextBaseBits already holds the clean no-hover frame (captured before
    // the pending-hover highlight above), so the fast hover path stays valid.
    ShowWindow(m_contextWindow, SW_SHOWNA);
}

void DockApp::ExecuteContextCommand(UINT command) {
    if (!IsContextMenuOpen()) {
        return;
    }
    const DisplayApp app = m_contextApp;
    const bool hasApp = m_contextHasApp;
    const bool isSpecial = m_contextIsSpecial;
    const bool isTrash = m_contextIsTrash;
    CloseContextMenu();

    if (command == 0) {
        return;
    }

    if (isTrash || command == kContextTrashOpen || command == kContextTrashEmpty ||
        command == kContextTrashProperties) {
        if (command == kContextTrashOpen) {
            OpenTrash();
        } else if (command == kContextTrashEmpty) {
            EmptyTrashWithConfirm();
        } else if (command == kContextTrashProperties) {
            ShowTrashProperties();
        }
        return;
    }

    bool configChanged = false;
    bool actionSucceeded = true;
    if (command == kContextOpen && hasApp) {
        if (app.app.target == kSearchTarget) {
            actionSucceeded = OpenLaunchPrompt();
        } else if (isSpecial) {
            actionSucceeded = OpenStartMenuFromDock();
        } else {
            actionSucceeded = m_windows.ActivateOrLaunch(app.app, app.runningWindow);
        }
    } else if (command == kContextOpenLocation && hasApp && !isSpecial) {
        actionSucceeded = m_windows.OpenLocation(app.app);
    } else if (command == kContextEndTask && hasApp && !isSpecial) {
        actionSucceeded = m_windows.EndTask(app.app, app.runningWindow);
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
        m_config.StopFollowingTaskbarPins();
        ScheduleConfigSave();
        ++m_refreshGeneration;
        ApplyPinUnpinLayoutChange();
        m_lastWindowRefresh = QpcSeconds();
        ScheduleDeferredRefresh();
    } else {
        RefreshRunningWindows();
    }
    QueueRenderFrame();
}

void DockApp::ActivatePressedApp() {
    if (m_pressedIcon < 0 || static_cast<size_t>(m_pressedIcon) >= m_iconRenderData.size()) {
        return;
    }
    if (IsTrashRenderIndex(m_pressedIcon)) {
        m_suppressDragUntilRelease = true;
        ClearPressState();
        OpenTrash();
        return;
    }
    if (IsWeatherRenderIndex(m_pressedIcon)) {
        m_suppressDragUntilRelease = true;
        ClearPressState();
        m_weather.RequestRefresh();
        return;
    }
    if (IsTrayRenderIndex(m_pressedIcon)) {
        const TraySlot slot = m_iconRenderData[static_cast<size_t>(m_pressedIcon)].traySlot;
        m_suppressDragUntilRelease = true;
        ClearPressState();
        OpenTraySlot(slot);
        return;
    }
    if (static_cast<size_t>(m_pressedIcon) >= m_displayApps.size()) {
        return;
    }

    const DisplayApp app = m_displayApps[static_cast<size_t>(m_pressedIcon)];
    // Clear press before launch so nested ShellExecute message pumps cannot start a drag.
    m_suppressDragUntilRelease = true;
    m_launchClickInProgress = true;
    ClearPressState();
    struct LaunchClickGuard {
        DockApp* app;
        ~LaunchClickGuard() { app->m_launchClickInProgress = false; }
    } launchGuard{this};

    if (app.app.target == kSearchTarget) {
        if (IsLaunchPromptOpen()) {
            CloseLaunchPrompt();
        } else if (!OpenLaunchPrompt()) {
            Log(L"Launch prompt could not be opened.");
        }
        return;
    }
    if (IsLaunchPromptOpen() && app.app.target != kStartTarget) {
        CloseLaunchPrompt(false);
    }
    if (IsSpecialDockTarget(app.app.target)) {
        static_cast<void>(OpenStartMenuFromDock());
        return;
    }

    // Activate on the UI thread; ShellExecuteEx launches go to a worker so WH_MOUSE_LL stays responsive.
    // No preferred window: macOS-style activation brings every window forward
    // with the frontmost focused. (Voice launch passes its specific window.)
    if (m_windows.TryActivate(app.app)) {
        m_lastWindowRefresh = 0.0;
        ScheduleDeferredRefresh();
        return;
    }
    const PinnedApp launchApp = app.app;
    std::thread([launchApp]() {
        if (!WindowCatalog::LaunchApp(launchApp)) {
            OutputDebugStringW(L"Application did not launch.\n");
        }
    }).detach();
    m_lastWindowRefresh = 0.0;
    // Defer refresh so Settings' window storm does not rebuild the dock mid-click.
    ScheduleDeferredRefresh();
}

LaunchCandidate DockApp::MakeLaunchCandidate(const LaunchTarget& target) const {
    LaunchCandidate candidate;
    candidate.id = target.id;
    candidate.pinName = target.app.name;
    candidate.shortcutName = target.shortcutName;
    candidate.target = target.app.target;
    candidate.aumid = target.aumid;
    candidate.description = target.description;
    candidate.productName = target.productName;
    candidate.publisher = target.publisher;
    candidate.startMenuFolder = target.startMenuFolder;
    candidate.comment = target.comment;
    candidate.executablePath = target.executablePath;
    candidate.executable = target.executableName;
    candidate.windowTitle = target.windowTitle;
    candidate.running = target.runningWindow != nullptr;
    candidate.name = target.isStart ? L"Start"
        : !target.app.name.empty() ? target.app.name
        : WindowCatalog::DisplayNameForApp(target.app, target.runningWindow);
    if (candidate.name.empty()) {
        candidate.name = candidate.pinName;
    }
    if (candidate.executable.empty() && !target.isStart &&
        target.app.target.rfind(L"shell:", 0) != 0) {
        candidate.executable = DisplayNameFromExecutable(target.app.target);
    }
    return candidate;
}

bool DockApp::OpenLaunchPrompt() {
    if (m_visibility == VisibilityState::Hidden || m_visibility == VisibilityState::Hiding) {
        BeginShow();
    }
    HideHoverLabel();

    if (m_launchPromptWindow != nullptr) {
        PositionLaunchPrompt();
        if (m_launchEdit != nullptr) {
            SetForegroundWindow(m_launchPromptWindow);
            SetFocus(m_launchEdit);
        }
        return true;
    }

    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&commonControls);

    const wchar_t className[] = L"LiquidGlassDockLaunchPrompt";
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = &DockApp::LaunchPromptWindowProcedure;
    windowClass.hInstance = m_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = className;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log(L"Could not register the launch prompt window class.");
        return false;
    }

    constexpr DWORD extendedStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    m_launchPromptWindow = CreateWindowExW(extendedStyle, className, L"Launch", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, m_instance, this);
    if (m_launchPromptWindow == nullptr) {
        Log(L"Could not create the launch prompt window.");
        return false;
    }
    EnsureWindowCapturable(m_launchPromptWindow);

    const DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(m_launchPromptWindow, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
        sizeof(corner));

    HFONT font = HoverLabelFont();
    // Read-only multi-line strip above the edit for factual / specs answers.
    m_launchAnswer = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_LEFT, 0, 0, 1, 1,
        m_launchPromptWindow, reinterpret_cast<HMENU>(3), m_instance, nullptr);
    m_launchEdit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 1, 1, m_launchPromptWindow,
        reinterpret_cast<HMENU>(1), m_instance, nullptr);
    m_launchStatus = CreateWindowExW(0, L"STATIC", L"Type an app name, folder, URL, or site search",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 1, 1, m_launchPromptWindow,
        reinterpret_cast<HMENU>(2), m_instance, nullptr);
    if (m_launchEdit == nullptr || m_launchStatus == nullptr || m_launchAnswer == nullptr) {
        Log(L"Could not create the launch prompt controls.");
        CloseLaunchPrompt(false);
        return false;
    }

    SendMessageW(m_launchAnswer, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(m_launchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(m_launchStatus, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(m_launchEdit, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(L"chrome, Apple Watch Ultra 5 specs, open downloads..."));

    m_launchEditPrevious = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(m_launchEdit, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&DockApp::LaunchEditProcedure)));
    PositionLaunchPrompt();
    ShowWindow(m_launchPromptWindow, SW_SHOW);
    SetForegroundWindow(m_launchPromptWindow);
    SetFocus(m_launchEdit);
    return true;
}

void DockApp::CloseLaunchPrompt(bool hideDockIfAway) {
    if (m_launchPromptWindow == nullptr) {
        return;
    }

    ++m_launchGeneration;
    m_launchInFlight = false;
    m_launchTargets.clear();

    if (m_launchEdit != nullptr && m_launchEditPrevious != nullptr) {
        SetWindowLongPtrW(m_launchEdit, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(m_launchEditPrevious));
    }
    m_launchEditPrevious = nullptr;
    m_launchAnswer = nullptr;
    m_launchEdit = nullptr;
    m_launchStatus = nullptr;

    if (m_launchPromptWindow != nullptr) {
        const HWND prompt = m_launchPromptWindow;
        m_launchPromptWindow = nullptr;
        DestroyWindow(prompt);
    }

    if (!hideDockIfAway || m_visibility == VisibilityState::Hidden ||
        m_visibility == VisibilityState::Hiding) {
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    if (cursor.y < m_visibleY + DockShadowMarginPx(static_cast<float>(HostDpi()) / 96.0F) &&
        !IsCursorOverDock(cursor)) {
        BeginHide();
    }
}

void DockApp::PositionLaunchPrompt() {
    if (m_launchPromptWindow == nullptr) {
        return;
    }

    const UINT dpi = m_window == nullptr ? 96U : std::max(GetDpiForWindow(m_window), 96U);
    const float scale = static_cast<float>(dpi) / 96.0F;
    const LONG padding = GreaterOf(10L, static_cast<LONG>(std::lround(12.0F * scale)));
    const LONG editHeight = GreaterOf(22L, static_cast<LONG>(std::lround(24.0F * scale)));
    const LONG statusHeight = GreaterOf(16L, static_cast<LONG>(std::lround(18.0F * scale)));
    const LONG gap = GreaterOf(4L, static_cast<LONG>(std::lround(6.0F * scale)));
    const LONG width = std::clamp(static_cast<LONG>(m_dockWidth) - padding * 2L,
        GreaterOf(240L, static_cast<LONG>(std::lround(280.0F * scale))),
        GreaterOf(320L, static_cast<LONG>(std::lround(420.0F * scale))));

    std::wstring answerText;
    if (m_launchAnswer != nullptr) {
        answerText = WindowText(m_launchAnswer);
    }
    const bool showAnswer = !answerText.empty();
    LONG answerHeight = 0;
    if (showAnswer) {
        // ~3 wrapped lines above the edit for short specs / Q&A answers.
        answerHeight = GreaterOf(48L, static_cast<LONG>(std::lround(54.0F * scale)));
        HDC dc = GetDC(m_launchPromptWindow);
        if (dc != nullptr) {
            HFONT font = HoverLabelFont();
            HGDIOBJ old = font != nullptr ? SelectObject(dc, font) : nullptr;
            RECT calc{0, 0, width - padding * 2L, 0};
            DrawTextW(dc, answerText.c_str(), -1, &calc,
                DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            if (old != nullptr) {
                SelectObject(dc, old);
            }
            ReleaseDC(m_launchPromptWindow, dc);
            const LONG measured = (calc.bottom - calc.top) + GreaterOf(4L, static_cast<LONG>(std::lround(6.0F * scale)));
            answerHeight = std::clamp(measured, answerHeight,
                GreaterOf(96L, static_cast<LONG>(std::lround(110.0F * scale))));
        }
    }

    const LONG height = padding + (showAnswer ? answerHeight + gap : 0L) + editHeight + gap +
        statusHeight + padding;
    const LONG x = m_windowX + (static_cast<LONG>(m_dockWidth) - width) / 2L;
    const LONG y = m_visibleY - height - GreaterOf(8L, static_cast<LONG>(std::lround(10.0F * scale)));

    SetWindowPos(m_launchPromptWindow, HWND_TOPMOST, SaturatedInt(x), SaturatedInt(y),
        SaturatedInt(width), SaturatedInt(height), SWP_NOOWNERZORDER);

    LONG yCursor = padding;
    if (m_launchAnswer != nullptr) {
        if (showAnswer) {
            ShowWindow(m_launchAnswer, SW_SHOW);
            SetWindowPos(m_launchAnswer, nullptr, SaturatedInt(padding), SaturatedInt(yCursor),
                SaturatedInt(width - padding * 2L), SaturatedInt(answerHeight),
                SWP_NOZORDER | SWP_NOACTIVATE);
            yCursor += answerHeight + gap;
        } else {
            ShowWindow(m_launchAnswer, SW_HIDE);
            SetWindowPos(m_launchAnswer, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (m_launchEdit != nullptr) {
        SetWindowPos(m_launchEdit, nullptr, SaturatedInt(padding), SaturatedInt(yCursor),
            SaturatedInt(width - padding * 2L), SaturatedInt(editHeight),
            SWP_NOZORDER | SWP_NOACTIVATE);
        yCursor += editHeight + gap;
    }
    if (m_launchStatus != nullptr) {
        SetWindowPos(m_launchStatus, nullptr, SaturatedInt(padding), SaturatedInt(yCursor),
            SaturatedInt(width - padding * 2L), SaturatedInt(statusHeight),
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void DockApp::SetLaunchPromptStatus(const std::wstring& text) {
    if (m_launchStatus != nullptr) {
        SetWindowTextW(m_launchStatus, text.c_str());
    }
}

void DockApp::SetLaunchPromptAnswer(const std::wstring& text) {
    if (m_launchAnswer == nullptr) {
        return;
    }
    SetWindowTextW(m_launchAnswer, text.c_str());
    PositionLaunchPrompt();
}

void DockApp::SubmitLaunchPrompt() {
    if (m_launchInFlight || m_launchEdit == nullptr) {
        return;
    }

    const std::wstring request = TrimWide(WindowText(m_launchEdit));
    if (request.empty()) {
        SetLaunchPromptStatus(L"Type an app name, folder, URL, or site search");
        return;
    }

    const UINT generation = ++m_launchGeneration;
    m_launchInFlight = true;
    m_launchRequest = request;
    SetLaunchPromptAnswer(L"");
    SetLaunchPromptStatus(L"Looking...");

    const HWND replyWindow = m_window;
    const std::vector<DisplayApp> displayApps = m_displayApps;
    const std::vector<RunningWindow> runningWindows = m_windows.RunningWindows();
    std::thread([this, generation, request, replyWindow, displayApps, runningWindows]() {
        // Never let an exception escape: an uncaught throw in a detached
        // thread calls std::terminate and the dock vanishes with no log.
        std::vector<LaunchTarget> targets;
        LaunchJudgment judgment;
        bool agentMode = false;
        SearchAgentResult agent;
        const auto postStatus = [replyWindow, generation](const std::wstring& text) {
            if (replyWindow == nullptr) {
                return;
            }
            AgentStatusReply* status = nullptr;
            try {
                status = new AgentStatusReply{generation, text};
            } catch (...) {
                return;
            }
            if (PostMessageW(replyWindow, kAgentStatusMessage, 0, reinterpret_cast<LPARAM>(status)) ==
                FALSE) {
                delete status;
            }
        };
        const auto superseded = [this, generation]() { return m_launchGeneration.load() != generation; };
        const auto launchJudgment = [](const std::string& id, double confidence) {
            LaunchJudgment picked;
            picked.action = LaunchJudgment::Action::Launch;
            picked.chosenId = id;
            picked.exists = 1.0;
            picked.confidence = confidence;
            return picked;
        };
        const auto started = std::chrono::steady_clock::now();
        std::wstring route = L"none";
        try {
            m_installedApps.EnsureLoaded();
            targets = CollectLaunchTargets(displayApps, runningWindows);
            const std::string exactId = ExactLaunchId(request, targets);
            if (!exactId.empty()) {
                // Fast path 1: exact app name.
                judgment = launchJudgment(exactId, 1.0);
                route = L"exact";
            } else {
                std::vector<LaunchCandidate> candidates;
                candidates.reserve(targets.size());
                for (const LaunchTarget& target : targets) {
                    candidates.push_back(MakeLaunchCandidate(target));
                }
                // Coding goals open in the most recently used IDE (focused when
                // running); resolved lazily, it only touches window Z-order and
                // a few file timestamps.
                std::optional<CodeEditorChoice> editor;
                const auto pickEditor = [&]() -> const CodeEditorChoice& {
                    if (!editor.has_value()) {
                        editor = FindPreferredCodeEditor(candidates);
                    }
                    return *editor;
                };
                const auto attachEditor = [&](SearchAgentResult& result) {
                    for (SearchAgentAction& action : result.actions) {
                        if (action.kind == SearchAgentAction::Kind::OpenInEditor) {
                            const CodeEditorChoice& chosen = pickEditor();
                            action.editor = chosen.exePath;
                            action.editorArgs = chosen.args;
                            action.editorWindow = reinterpret_cast<std::uintptr_t>(chosen.window);
                            route += L" editor=" + chosen.name + L"(" + chosen.source + L")";
                        }
                    }
                };
                if (LlamaServerClient::LooksLikeCodingGoal(request)) {
                    // Coding agent needs the local LLM; left out for now. Replays
                    // of a previously written file still reopen it without a model.
                    agentMode = true;
                    auto replay = LlamaServerClient::TryReplayLastGoal(request);
                    if (replay.has_value() && !replay->actions.empty() &&
                        std::ranges::all_of(replay->actions, [](const SearchAgentAction& a) {
                            return a.kind == SearchAgentAction::Kind::OpenInEditor &&
                                LlamaServerClient::IsSafeEditorFile(a.target);
                        })) {
                        agent = std::move(*replay);
                        route = L"code-replay";
                        attachEditor(agent);
                    } else {
                        agent.reply = L"Local coding agent is disabled for now.";
                        route = L"code-disabled";
                    }
                } else if (auto replay = LlamaServerClient::TryReplayLastGoal(request); replay.has_value()) {
                    // Exact repeat of the last successful goal (no model).
                    agentMode = true;
                    agent = std::move(*replay);
                    route = L"replay";
                    attachEditor(agent);
                } else if (auto plan = LlamaServerClient::PlanWithoutModel(request, candidates);
                    plan.has_value()) {
                    // Model-free: URL/domain, site search/home, folder/path, confident
                    // catalog app (alias/acronym/typo), question -> web, keyword match,
                    // or a compound of those.
                    route = plan->route;
                    if (plan->actions.size() == 1 &&
                        plan->actions.front().kind == SearchAgentAction::Kind::LaunchApp) {
                        judgment = launchJudgment(plan->actions.front().appId, 0.95);
                    } else {
                        agentMode = true;
                        agent.actions = std::move(plan->actions);
                    }
                } else if (LlamaServerClient::LooksLikeAgentGoal(request)) {
                    judgment = LlamaServerClient::ResolveAppFuzzy(
                        LlamaServerClient::StripGoalVerbs(request), candidates);
                    route = L"fuzzy-goal";
                } else {
                    // Plain search: lexical / substring ranking only (no llama-server).
                    judgment = LlamaServerClient::ResolveAppFuzzy(request, candidates);
                    route = L"fuzzy";
                }
            }
        } catch (const std::exception&) {
            agentMode = false;
            judgment.action = LaunchJudgment::Action::Error;
            judgment.error = L"Launch failed before a match could be picked.";
        } catch (...) {
            agentMode = false;
            judgment.action = LaunchJudgment::Action::Error;
            judgment.error = L"Launch failed unexpectedly.";
        }
        const long long workerMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        LaunchReply* reply = nullptr;
        try {
            reply = new LaunchReply{generation, std::move(judgment), std::move(targets), agentMode,
                std::move(agent), std::move(route), workerMs};
        } catch (...) {
            return;
        }
        if (replyWindow == nullptr ||
            PostMessageW(replyWindow, kLaunchResultMessage, 0,
                reinterpret_cast<LPARAM>(reply)) == FALSE) {
            delete reply;
        }
    }).detach();
}

void DockApp::ApplyLaunchJudgment(UINT generation, const LaunchJudgment& judgment) {
    if (generation != m_launchGeneration.load()) {
        return;
    }

    m_launchInFlight = false;
    if (m_launchEdit != nullptr) {
        SetFocus(m_launchEdit);
    }

    Log(L"Launch judgment action=" + std::to_wstring(static_cast<int>(judgment.action)) +
        L" choice=" + std::wstring(judgment.chosenId.begin(), judgment.chosenId.end()) +
        L" path=" + judgment.openPath +
        L" fuzzy=" + std::to_wstring(judgment.usedFuzzyFallback ? 1 : 0) +
        L" exists=" + std::to_wstring(judgment.exists) +
        L" confidence=" + std::to_wstring(judgment.confidence));

    if (judgment.action == LaunchJudgment::Action::Error) {
        SetLaunchPromptStatus(judgment.error.empty() ? L"Launch request failed." : judgment.error);
        return;
    }
    if (judgment.action == LaunchJudgment::Action::Uncertain) {
        SetLaunchPromptStatus(L"Not sure which app. Try a more specific name.");
        return;
    }
    if (judgment.action == LaunchJudgment::Action::None) {
        SetLaunchPromptStatus(L"No matching installed app.");
        return;
    }

    if (!judgment.openPath.empty()) {
        if (!LlamaServerClient::IsSafeShellOpenTarget(false, judgment.openPath)) {
            SetLaunchPromptStatus(L"Blocked unsafe path.");
            return;
        }
        const HINSTANCE opened = ShellExecuteW(nullptr, L"open", judgment.openPath.c_str(), nullptr,
            nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(opened) <= 32) {
            SetLaunchPromptStatus(L"Could not open that path.");
            return;
        }
        SearchAgentAction remembered;
        remembered.kind = SearchAgentAction::Kind::OpenPath;
        remembered.target = judgment.openPath;
        remembered.label = judgment.openPath;
        LlamaServerClient::RememberSuccessfulGoal(m_launchRequest, {remembered});
        CloseLaunchPrompt(false);
        BeginHide();
        return;
    }

    const LaunchTarget* target = FindLaunchTarget(judgment.chosenId);
    if (target == nullptr) {
        SetLaunchPromptStatus(L"No matching installed app.");
        return;
    }
    if (!ActivateLaunchTarget(*target)) {
        SetLaunchPromptStatus(L"The selected app could not be opened.");
        return;
    }
    LlamaServerClient::RememberSuccessfulLaunch(m_launchRequest, judgment.chosenId, target->app.name);
    CloseLaunchPrompt(false);
    BeginHide();
}

void DockApp::ApplyAgentResult(UINT generation, const SearchAgentResult& result) {
    if (generation != m_launchGeneration.load()) {
        return;
    }

    m_launchInFlight = false;
    if (m_launchEdit != nullptr) {
        SetFocus(m_launchEdit);
    }

    Log(L"Search agent steps=" + std::to_wstring(result.steps) +
        L" actions=" + std::to_wstring(result.actions.size()) + L" reply=" + result.reply);

    size_t performed = 0;
    std::wstring failure;
    // write_file results: planned path -> file actually created (CREATE_NEW may
    // pick name-2.ext). OpenInEditor opens the created file only.
    std::vector<std::pair<std::wstring, std::wstring>> written;
    std::vector<std::wstring> failedWrites;
    std::vector<SearchAgentAction> remember;
    for (const SearchAgentAction& action : result.actions) {
        switch (action.kind) {
        case SearchAgentAction::Kind::WriteFile: {
            std::wstring path;
            std::wstring why;
            if (LlamaServerClient::WriteAgentFile(action, path, why)) {
                Log(L"Search agent wrote " + path + L" bytes=" + std::to_wstring(action.content.size()));
                written.emplace_back(action.target, path);
                ++performed;
            } else {
                Log(L"Search agent write blocked: " + action.target + L" (" + why + L")");
                failedWrites.push_back(action.target);
                failure = L"Could not write " + action.label + L" (" + why + L").";
            }
            break;
        }
        case SearchAgentAction::Kind::OpenInEditor: {
            if (std::ranges::find(failedWrites, action.target) != failedWrites.end()) {
                break;
            }
            std::wstring file = action.target;
            for (const auto& [planned, actual] : written) {
                if (planned == action.target) {
                    file = actual;
                }
            }
            if (!LlamaServerClient::IsSafeEditorFile(file)) {
                failure = L"Blocked unsafe file.";
                break;
            }
            HWND editorWindow = reinterpret_cast<HWND>(action.editorWindow);
            bool opened = OpenFileInCodeEditor(action.editor, action.editorArgs, editorWindow, file);
            if (!opened && !action.editor.empty()) {
                opened = OpenFileInCodeEditor(L"", L"", nullptr, file);  // Notepad fallback
            }
            Log(L"Search agent open in editor=" + (action.editor.empty() ? std::wstring(L"notepad") : action.editor) +
                L" file=" + file + L" ok=" + std::to_wstring(opened ? 1 : 0));
            if (opened) {
                ++performed;
                SearchAgentAction again = action;
                again.target = file;
                remember.push_back(std::move(again));
            } else {
                failure = L"Wrote " + file + L" but could not open an editor.";
            }
            break;
        }
        case SearchAgentAction::Kind::LaunchApp: {
            const LaunchTarget* target = FindLaunchTarget(action.appId);
            Log(L"Search agent launch id=" + std::wstring(action.appId.begin(), action.appId.end()) +
                L" label=" + action.label);
            if (target != nullptr && ActivateLaunchTarget(*target)) {
                ++performed;
            } else {
                failure = L"Could not open " + action.label + L".";
            }
            break;
        }
        case SearchAgentAction::Kind::OpenUrl:
        case SearchAgentAction::Kind::OpenPath: {
            // Defence in depth: re-validate before ShellExecute.
            const bool isUrl = action.kind == SearchAgentAction::Kind::OpenUrl;
            if (!LlamaServerClient::IsSafeShellOpenTarget(isUrl, action.target)) {
                failure = L"Blocked unsafe target.";
                break;
            }
            Log(L"Search agent ShellExecute " + std::wstring(isUrl ? L"url=" : L"path=") + action.target);
            const HINSTANCE opened = ShellExecuteW(nullptr, L"open", action.target.c_str(), nullptr,
                nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(opened) > 32) {
                ++performed;
            } else {
                failure = L"Could not open " + action.label + L".";
            }
            break;
        }
        }
    }

    if (performed > 0) {
        // A code goal is remembered as "reopen the file it wrote", never as a
        // second write.
        const bool hasEditorStep = std::ranges::any_of(result.actions, [](const SearchAgentAction& a) {
            return a.kind == SearchAgentAction::Kind::WriteFile || a.kind == SearchAgentAction::Kind::OpenInEditor;
        });
        if (!hasEditorStep) {
            LlamaServerClient::RememberSuccessfulGoal(m_launchRequest, result.actions);
        } else if (!remember.empty()) {
            LlamaServerClient::RememberSuccessfulGoal(m_launchRequest, remember);
        }
        CloseLaunchPrompt(false);
        BeginHide();
        return;
    }
    if (failure.empty() && !result.reply.empty()) {
        // Status / disabled-feature reply: keep Search open and show text above
        // the edit (no local LLM answers or tok/s).
        Log(L"Search reply shown chars=" + std::to_wstring(result.reply.size()));
        SetLaunchPromptAnswer(result.reply);
        SetLaunchPromptStatus(L"-");
        return;
    }
    SetLaunchPromptStatus(!failure.empty() ? failure
        : L"No matching app, folder, or site for that.");
}

std::vector<DockApp::LaunchTarget> DockApp::CollectLaunchTargets(
    const std::vector<DisplayApp>& displayApps, const std::vector<RunningWindow>& runningWindows) const {
    std::vector<LaunchTarget> targets;
    size_t nextId = 0;
    const auto assignId = [&nextId](LaunchTarget& target) {
        target.id = "c" + std::to_string(nextId);
        ++nextId;
    };

    LaunchTarget start;
    start.app = {L"Start", kStartTarget, L"", L""};
    start.isStart = true;
    assignId(start);
    targets.push_back(std::move(start));

    const auto sameApp = [](const LaunchTarget& target, const std::wstring& launchTarget,
        const std::wstring& aumid, const std::wstring& executablePath) {
        if (target.isStart) {
            return false;
        }
        if (!launchTarget.empty() && WindowCatalog::TargetsMatch(target.app.target, launchTarget)) {
            return true;
        }
        if (!aumid.empty() && !target.aumid.empty() && EqualInsensitiveWide(target.aumid, aumid)) {
            return true;
        }
        if (!executablePath.empty() && !target.executablePath.empty() &&
            WindowCatalog::TargetsMatch(target.executablePath, executablePath)) {
            return true;
        }
        return false;
    };

    const auto findExisting = [&targets, &sameApp](const std::wstring& launchTarget,
        const std::wstring& aumid, const std::wstring& executablePath) -> LaunchTarget* {
        for (LaunchTarget& target : targets) {
            if (sameApp(target, launchTarget, aumid, executablePath)) {
                return &target;
            }
        }
        return nullptr;
    };

    const auto takeIfEmpty = [](std::wstring& dest, const std::wstring& source) {
        if (dest.empty() && !source.empty()) {
            dest = source;
        }
    };

    auto addInstalled = [&](InstalledApp app) {
        LaunchTarget* existing = findExisting(app.launch.target, app.aumid, app.executablePath);
        if (existing != nullptr) {
            takeIfEmpty(existing->shortcutName, app.shortcutName);
            takeIfEmpty(existing->executableName, app.executableName);
            takeIfEmpty(existing->executablePath, app.executablePath);
            takeIfEmpty(existing->aumid, app.aumid);
            takeIfEmpty(existing->description, app.description);
            takeIfEmpty(existing->productName, app.productName);
            takeIfEmpty(existing->publisher, app.publisher);
            takeIfEmpty(existing->startMenuFolder, app.startMenuFolder);
            takeIfEmpty(existing->comment, app.comment);
            takeIfEmpty(existing->app.name, app.launch.name);
            return;
        }
        LaunchTarget target;
        target.app = std::move(app.launch);
        target.shortcutName = std::move(app.shortcutName);
        target.executableName = std::move(app.executableName);
        target.executablePath = std::move(app.executablePath);
        target.aumid = std::move(app.aumid);
        target.description = std::move(app.description);
        target.productName = std::move(app.productName);
        target.publisher = std::move(app.publisher);
        target.startMenuFolder = std::move(app.startMenuFolder);
        target.comment = std::move(app.comment);
        assignId(target);
        targets.push_back(std::move(target));
    };

    for (InstalledApp& app : m_installedApps.Snapshot()) {
        addInstalled(std::move(app));
    }

    for (const DisplayApp& app : displayApps) {
        if (IsSpecialDockTarget(app.app.target) || IsLayoutOnlyTarget(app.app.target)) {
            continue;
        }
        InstalledApp installed;
        installed.launch = app.app;
        addInstalled(std::move(installed));
    }

    for (const RunningWindow& window : runningWindows) {
        LaunchTarget* existing = findExisting(window.executablePath, window.appUserModelId,
            window.executablePath);
        if (existing != nullptr) {
            if (existing->runningWindow == nullptr) {
                existing->runningWindow = window.handle;
            }
            if (existing->windowTitle.empty()) {
                existing->windowTitle = window.title;
            } else if (!window.title.empty() &&
                existing->windowTitle.find(window.title) == std::wstring::npos) {
                existing->windowTitle += L" | ";
                existing->windowTitle += window.title;
            }
            takeIfEmpty(existing->executableName, window.executableName);
            takeIfEmpty(existing->executablePath, window.executablePath);
            takeIfEmpty(existing->aumid, window.appUserModelId);
            continue;
        }
        LaunchTarget target;
        target.app.name = window.executableName;
        target.app.target = window.executablePath;
        target.runningWindow = window.handle;
        target.executableName = window.executableName;
        target.executablePath = window.executablePath;
        target.aumid = window.appUserModelId;
        target.windowTitle = window.title;
        assignId(target);
        targets.push_back(std::move(target));
    }

    for (LaunchTarget& target : targets) {
        if (target.isStart || target.runningWindow != nullptr) {
            continue;
        }
        for (const RunningWindow& window : runningWindows) {
            if ((!target.executablePath.empty() &&
                    WindowCatalog::TargetsMatch(target.executablePath, window.executablePath)) ||
                (!target.aumid.empty() && EqualInsensitiveWide(target.aumid, window.appUserModelId)) ||
                WindowCatalog::TargetsMatch(target.app.target, window.executablePath)) {
                target.runningWindow = window.handle;
                target.windowTitle = window.title;
                takeIfEmpty(target.executableName, window.executableName);
                takeIfEmpty(target.executablePath, window.executablePath);
                takeIfEmpty(target.aumid, window.appUserModelId);
                break;
            }
        }
    }

    return targets;
}

const DockApp::LaunchTarget* DockApp::FindLaunchTarget(const std::string& id) const noexcept {
    for (const LaunchTarget& target : m_launchTargets) {
        if (target.id == id) {
            return &target;
        }
    }
    return nullptr;
}

std::string DockApp::ExactLaunchId(const std::wstring& request,
    const std::vector<LaunchTarget>& targets) const {
    const std::wstring stripped = StripLaunchPhrases(request);
    if (stripped.empty()) {
        return {};
    }

    const LaunchTarget* matched = nullptr;
    for (const LaunchTarget& target : targets) {
        const LaunchCandidate candidate = MakeLaunchCandidate(target);
        if (EqualInsensitiveWide(stripped, candidate.name) ||
            EqualInsensitiveWide(stripped, candidate.pinName) ||
            EqualInsensitiveWide(stripped, candidate.shortcutName) ||
            EqualInsensitiveWide(stripped, candidate.productName) ||
            EqualInsensitiveWide(stripped, candidate.executable) ||
            EqualInsensitiveWide(stripped, candidate.windowTitle) ||
            (target.isStart && EqualInsensitiveWide(stripped, L"start menu"))) {
            if (matched != nullptr) {
                return {};
            }
            matched = &target;
        }
    }
    return matched == nullptr ? std::string{} : matched->id;
}

bool DockApp::ActivateLaunchTarget(const LaunchTarget& target) {
    if (target.isStart) {
        return OpenStartMenuFromDock();
    }
    // Same split as ActivatePressedApp: activate synchronously, launch off the
    // UI thread so the low-level mouse hook keeps pumping and the cursor never
    // freezes while a heavy app starts.
    if (m_windows.TryActivate(target.app, target.runningWindow)) {
        m_lastWindowRefresh = 0.0;
        RefreshRunningWindows();
        return true;
    }
    const PinnedApp launchApp = target.app;
    std::thread([launchApp]() {
        if (!WindowCatalog::LaunchApp(launchApp)) {
            OutputDebugStringW(L"Application did not launch.\n");
        }
    }).detach();
    m_lastWindowRefresh = 0.0;
    RefreshRunningWindows();
    return true;
}

bool DockApp::IsLaunchPromptOpen() const noexcept {
    return m_launchPromptWindow != nullptr && IsWindowVisible(m_launchPromptWindow) != FALSE;
}

void DockApp::CacheLayoutSlotBounds() {
    m_layoutSlotBounds.clear();
    m_layoutSlotBounds.reserve(m_iconRenderData.size());
    for (const DockIconRenderData& icon : m_iconRenderData) {
        m_layoutSlotBounds.push_back(icon.bounds);
    }
}

void DockApp::EnsureDragGhostWindow() {
    if (m_dragGhostWindow != nullptr) {
        return;
    }

    const wchar_t dragGhostClassName[] = L"LiquidGlassDockDragGhost";
    WNDCLASSEXW dragGhostClass{sizeof(dragGhostClass)};
    dragGhostClass.lpfnWndProc = &DockApp::DragGhostWindowProcedure;
    dragGhostClass.hInstance = m_instance;
    dragGhostClass.lpszClassName = dragGhostClassName;
    if (RegisterClassExW(&dragGhostClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log(L"Could not register the drag ghost window class.");
        return;
    }

    constexpr DWORD extendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
        WS_EX_TRANSPARENT | WS_EX_TOPMOST;
    m_dragGhostWindow = CreateWindowExW(extendedStyle, dragGhostClassName, L"", WS_POPUP, 0, 0, 1,
        1, nullptr, nullptr, m_instance, nullptr);
    if (m_dragGhostWindow == nullptr) {
        Log(L"Could not create the drag ghost window.");
        return;
    }
    EnsureWindowCapturable(m_dragGhostWindow);
}

void DockApp::UpdateDragGhostContent() {
    if (m_dragGhostWindow == nullptr || m_draggedIcon < 0 ||
        static_cast<size_t>(m_draggedIcon) >= m_displayApps.size()) {
        return;
    }

    if (m_dragGhostBitmap != nullptr) {
        if (m_dragGhostMemoryDc != nullptr && m_dragGhostPreviousBitmap != nullptr &&
            m_dragGhostPreviousBitmap != HGDI_ERROR) {
            SelectObject(m_dragGhostMemoryDc, m_dragGhostPreviousBitmap);
            m_dragGhostPreviousBitmap = nullptr;
        }
        DeleteObject(m_dragGhostBitmap);
        m_dragGhostBitmap = nullptr;
        m_dragGhostSize = {};
    }

    const UINT extent = IconPixelExtent();
    const std::wstring cacheKey =
        WindowCatalog::IconCacheKey(m_displayApps[static_cast<size_t>(m_draggedIcon)].app);
    if (const std::vector<uint8_t>* cached = m_renderer.CachedIconPixels(cacheKey);
        cached != nullptr) {
        m_dragGhostBitmap = CreateDragGhostBitmapFromPixels(*cached, extent);
    }
    if (m_dragGhostBitmap == nullptr) {
        m_dragGhostBitmap = CreateDragGhostBitmap(m_displayApps[static_cast<size_t>(m_draggedIcon)].app,
            m_displayApps[static_cast<size_t>(m_draggedIcon)].runningWindow, extent);
    }
    if (m_dragGhostBitmap == nullptr) {
        return;
    }

    m_dragGhostSize.cx = static_cast<LONG>(extent);
    m_dragGhostSize.cy = static_cast<LONG>(extent);
}

void DockApp::SnapDragGhostToInsertionSlot() {
    if (m_draggedIcon < 0 || m_dragInsertion < 0 || m_layoutSlotBounds.empty()) {
        return;
    }

    int slot = m_dragInsertion;
    if (slot > m_draggedIcon) {
        --slot;
    }
    slot = std::clamp(slot, 0, static_cast<int>(m_layoutSlotBounds.size()) - 1);
    const RECT& bounds = m_layoutSlotBounds[static_cast<size_t>(slot)];
    const POINT topLeft =
        ScreenFromDockClient({bounds.left, bounds.top}, m_windowX, m_currentY);
    UpdateDragGhostPosition({topLeft.x + m_dragGrabOffset.x, topLeft.y + m_dragGrabOffset.y});
}

void DockApp::FinishDropPresent(bool presented) {
    ProfileScope scope("FinishDropPresent");
    if (!m_dropPresentPending) {
        return;
    }
    if (presented) {
        HideDragGhost();
        m_dropPresentPending = false;
        return;
    }
    QueueRenderFrame();
}

void DockApp::UpdateDragGhostPosition(POINT screenCursor) {
    ProfileScope scope("UpdateDragGhostPosition");
    if (m_dragGhostWindow == nullptr || m_dragGhostBitmap == nullptr ||
        m_dragGhostSize.cx <= 0 || m_dragGhostSize.cy <= 0) {
        return;
    }

    if (m_dragGhostMemoryDc == nullptr) {
        HDC screen = GetDC(nullptr);
        if (screen == nullptr) {
            return;
        }
        m_dragGhostMemoryDc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (m_dragGhostMemoryDc == nullptr) {
            return;
        }
    }

    HGDIOBJ previousBitmap = SelectObject(m_dragGhostMemoryDc, m_dragGhostBitmap);
    if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
        return;
    }
    m_dragGhostPreviousBitmap = previousBitmap;

    POINT destination{screenCursor.x - m_dragGrabOffset.x, screenCursor.y - m_dragGrabOffset.y};
    POINT source{0L, 0L};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL updated = UpdateLayeredWindow(m_dragGhostWindow, nullptr, &destination,
        &m_dragGhostSize, m_dragGhostMemoryDc, &source, 0, &blend, ULW_ALPHA);
    if (updated == FALSE) {
        Log(L"Could not update the drag ghost window.");
    }
}

void DockApp::BringDragGhostToFront() {
    ProfileScope scope("BringDragGhostToFront");
    if (m_dragGhostWindow == nullptr || m_dragGhostBitmap == nullptr ||
        m_dragGhostSize.cx <= 0 || m_dragGhostSize.cy <= 0) {
        return;
    }

    RECT bounds{};
    if (GetWindowRect(m_dragGhostWindow, &bounds) == FALSE) {
        return;
    }

    if (SetWindowPos(m_dragGhostWindow, HWND_TOPMOST, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOACTIVATE | SWP_SHOWWINDOW) == FALSE) {
        Log(L"Could not raise the drag ghost window.");
    }
}

void DockApp::HideDragGhost() noexcept {
    if (m_dragGhostWindow != nullptr) {
        ShowWindow(m_dragGhostWindow, SW_HIDE);
    }
}

void DockApp::DestroyDragGhostWindow() {
    HideDragGhost();
    if (m_dragGhostMemoryDc != nullptr) {
        if (m_dragGhostPreviousBitmap != nullptr && m_dragGhostPreviousBitmap != HGDI_ERROR) {
            SelectObject(m_dragGhostMemoryDc, m_dragGhostPreviousBitmap);
        }
        DeleteDC(m_dragGhostMemoryDc);
        m_dragGhostMemoryDc = nullptr;
        m_dragGhostPreviousBitmap = nullptr;
    }
    if (m_dragGhostBitmap != nullptr) {
        DeleteObject(m_dragGhostBitmap);
        m_dragGhostBitmap = nullptr;
        m_dragGhostSize = {};
    }
    if (m_dragGhostWindow != nullptr) {
        if (DestroyWindow(m_dragGhostWindow) == FALSE) {
            Log(L"Could not destroy the drag ghost window.");
        }
        m_dragGhostWindow = nullptr;
    }
}

void DockApp::ApplyDragPreviewLayout() {
    if (m_draggedIcon < 0 || m_layoutSlotBounds.size() != m_iconRenderData.size()) {
        return;
    }

    const int appCount = AppSlotCount();
    const int iconCount = m_dividerIndex >= 0 ? m_dividerIndex : appCount;
    const LONG iconWidth = m_layoutSlotBounds[static_cast<size_t>(m_draggedIcon)].right -
        m_layoutSlotBounds[static_cast<size_t>(m_draggedIcon)].left;
    const LONG iconHeight = m_layoutSlotBounds[static_cast<size_t>(m_draggedIcon)].bottom -
        m_layoutSlotBounds[static_cast<size_t>(m_draggedIcon)].top;
    const RECT offscreen = {-32000L, -32000L, -32000L + iconWidth, -32000L + iconHeight};
    m_iconRenderData[static_cast<size_t>(m_draggedIcon)].bounds = offscreen;

    const bool overDock = IsCursorOverDock(m_lastCursor) && m_dragInsertion >= 0;
    if (!overDock) {
        for (int displayIndex = 0; displayIndex < iconCount; ++displayIndex) {
            if (displayIndex == m_draggedIcon) {
                continue;
            }
            m_iconRenderData[static_cast<size_t>(displayIndex)].bounds =
                m_layoutSlotBounds[static_cast<size_t>(displayIndex)];
        }
        return;
    }

    const int insertion = std::clamp(m_dragInsertion, 2, iconCount);
    int slot = 0;
    for (int displayIndex = 0; displayIndex < iconCount; ++displayIndex) {
        if (displayIndex == m_draggedIcon) {
            continue;
        }
        if (displayIndex == insertion) {
            ++slot;
        }
        m_iconRenderData[static_cast<size_t>(displayIndex)].bounds =
            m_layoutSlotBounds[static_cast<size_t>(slot)];
        ++slot;
    }
}

void DockApp::BeginDrag(POINT screenCursor) {
    ProfileScope scope("BeginDrag");
    if (m_suppressDragUntilRelease || m_launchClickInProgress || m_pressedIcon < 0 ||
        !IsPersistentDisplayIcon(m_pressedIcon)) {
        return;
    }
    // ShellExecute can pump messages; do not start a drag if the button is already up.
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
        ClearPressState();
        m_suppressDragUntilRelease = true;
        return;
    }
    if (QpcSeconds() - m_pressedAtTime < kMinDragPressSeconds) {
        return;
    }

    CacheLayoutSlotBounds();
    m_draggedIcon = m_pressedIcon;
    m_dragOriginIndex = m_pressedIcon;
    m_draggedTarget = m_pressedTarget;
    if (m_draggedTarget.empty() && static_cast<size_t>(m_draggedIcon) < m_displayApps.size()) {
        m_draggedTarget = m_displayApps[static_cast<size_t>(m_draggedIcon)].app.target;
    }
    m_dragOriginBounds = m_iconRenderData[static_cast<size_t>(m_draggedIcon)].bounds;
    m_dragInsertion = InsertionIndexForDrag(screenCursor);

    // Same origin as hit-testing (m_windowX/m_currentY); keep press pixel under the cursor.
    const POINT iconTopLeft =
        ScreenFromDockClient({m_dragOriginBounds.left, m_dragOriginBounds.top}, m_windowX,
            m_currentY);
    m_dragGrabOffset = {m_pressedAt.x - iconTopLeft.x, m_pressedAt.y - iconTopLeft.y};
    // Slot bounds include running dots; ghost is the square icon face.
    const LONG iconFace = std::max(1L, m_dragOriginBounds.right - m_dragOriginBounds.left);
    m_dragGrabOffset.x = std::clamp(m_dragGrabOffset.x, 0L, iconFace - 1L);
    m_dragGrabOffset.y = std::clamp(m_dragGrabOffset.y, 0L, iconFace - 1L);

    HideHoverLabel();
    EnsureDragGhostWindow();
    UpdateDragGhostContent();
    UpdateDragGhostPosition(screenCursor);
    BringDragGhostToFront();
    if (m_dragGhostBitmap != nullptr) {
        ApplyDragPreviewLayout();
    }
    m_lastDragPresentAt = QpcSeconds();
    RenderFrame();
}

void DockApp::UpdateDrag(POINT screenCursor) {
    ProfileScope scope("UpdateDrag");
    if (m_draggedIcon < 0) {
        return;
    }

    m_lastCursor = screenCursor;
    if (IsCursorOverDock(screenCursor)) {
        m_dragInsertion = InsertionIndexForDrag(screenCursor);
    } else {
        m_dragInsertion = -1;
    }
    UpdateDragGhostPosition(screenCursor);
    if (m_dragGhostBitmap != nullptr) {
        ApplyDragPreviewLayout();
    }

    const double now = QpcSeconds();
    if (now - m_lastDragPresentAt < 0.016) {
        return;
    }
    m_lastDragPresentAt = now;
    RenderFrame();
}

void DockApp::CancelDragWithSnapBack(POINT releaseCursor) {
    if (m_draggedIcon < 0) {
        return;
    }

    const POINT originTopLeft =
        ScreenFromDockClient({m_dragOriginBounds.left, m_dragOriginBounds.top}, m_windowX,
            m_currentY);

    m_dragSnapFrom = {releaseCursor.x - m_dragGrabOffset.x, releaseCursor.y - m_dragGrabOffset.y};
    m_dragSnapTo = originTopLeft;
    m_dragSnapStartedAt = QpcSeconds();
    m_dragSnapAnimating = true;
    m_dragInsertion = -1;
    m_suppressBackdropUntil = QpcSeconds() + 0.35;
    if (m_dragGhostBitmap != nullptr) {
        ApplyDragPreviewLayout();
    }
    RenderFrame();
}

void DockApp::AdvanceDragSnapBack() {
    if (!m_dragSnapAnimating) {
        return;
    }

    const double elapsed = QpcSeconds() - m_dragSnapStartedAt;
    const double linear = std::clamp(elapsed / kDragSnapDurationSeconds, 0.0, 1.0);
    const double eased = linear * linear * (3.0 - 2.0 * linear);
    const POINT current{
        std::lround(static_cast<double>(m_dragSnapFrom.x) +
            static_cast<double>(m_dragSnapTo.x - m_dragSnapFrom.x) * eased),
        std::lround(static_cast<double>(m_dragSnapFrom.y) +
            static_cast<double>(m_dragSnapTo.y - m_dragSnapFrom.y) * eased),
    };
    UpdateDragGhostPosition({current.x + m_dragGrabOffset.x, current.y + m_dragGrabOffset.y});

    if (linear < 1.0) {
        return;
    }

    HideDragGhost();
    m_dragSnapAnimating = false;
    ClearPressState();
    for (size_t index = 0; index < m_iconRenderData.size() && index < m_layoutSlotBounds.size();
        ++index) {
        m_iconRenderData[index].bounds = m_layoutSlotBounds[index];
    }
    RenderFrame();
}

void DockApp::FinishDrag(POINT screenCursor) {
    ProfileScope scope("FinishDrag");
    if (m_draggedIcon < 0) {
        return;
    }

    m_lastCursor = screenCursor;
    if (IsCursorOverDock(screenCursor)) {
        m_dragInsertion = InsertionIndexForDrag(screenCursor);
    } else {
        m_dragInsertion = -1;
    }

    if (!IsCursorOverDock(screenCursor) || m_dragInsertion < 0) {
        CancelDragWithSnapBack(screenCursor);
        return;
    }

    const int draggedPin = m_displayApps[static_cast<size_t>(m_draggedIcon)].persistentPinIndex;
    const int maxInsertion = m_dividerIndex >= 0 ? m_dividerIndex : static_cast<int>(m_displayApps.size());
    const int insertion = std::clamp(m_dragInsertion, 0, maxInsertion);
    int target = 0;
    for (int index = 0; index < insertion; ++index) {
        if (m_displayApps[static_cast<size_t>(index)].persistentPinIndex >= 0) {
            ++target;
        }
    }
    if (target == draggedPin || target == draggedPin + 1) {
        CancelDragWithSnapBack(screenCursor);
        return;
    }

    SnapDragGhostToInsertionSlot();
    CompleteDrag();
    ClearPressState();
    m_dropPresentPending = true;
    m_suppressBackdropUntil = QpcSeconds() + 0.35;
    FinishDropPresent(RenderFrame(false));
}

void DockApp::CompleteDrag() {
    ProfileScope scope("CompleteDrag");
    if (m_draggedIcon < 0 || m_dragInsertion < 0 || !IsPersistentDisplayIcon(m_draggedIcon) ||
        static_cast<size_t>(m_draggedIcon) >= m_displayApps.size()) {
        return;
    }

    const int draggedPin = m_displayApps[static_cast<size_t>(m_draggedIcon)].persistentPinIndex;
    if (draggedPin < 0 || static_cast<size_t>(draggedPin) >= m_config.Pins().size()) {
        return;
    }

    const int maxInsertion = m_dividerIndex >= 0 ? m_dividerIndex : static_cast<int>(m_displayApps.size());
    const int insertion = std::clamp(m_dragInsertion, 0, maxInsertion);
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
    ScheduleConfigSave();

    DisplayApp dragged = std::move(m_displayApps[static_cast<size_t>(m_draggedIcon)]);
    m_displayApps.erase(m_displayApps.begin() + m_draggedIcon);
    int displayTarget = insertion;
    if (displayTarget > m_draggedIcon) {
        --displayTarget;
    }
    displayTarget = std::clamp(displayTarget, 0, static_cast<int>(m_displayApps.size()));
    m_displayApps.insert(m_displayApps.begin() + displayTarget, std::move(dragged));
    int pinIndex = 0;
    for (DisplayApp& displayApp : m_displayApps) {
        if (displayApp.persistentPinIndex >= 0) {
            displayApp.persistentPinIndex = pinIndex++;
        }
    }
    m_dividerIndex = DividerIndexFromDisplayApps(m_displayApps);
    ++m_refreshGeneration;
    {
        const std::lock_guard lock(m_refreshSnapshotMutex);
        m_refreshSnapshot.reset();
    }
    RebuildLayout(false);
    m_lastWindowRefresh = QpcSeconds();
}

void DockApp::ClearPressState() noexcept {
    const bool restoreSlots = m_draggedIcon >= 0 || m_dragSnapAnimating;
    m_pressedIcon = -1;
    m_draggedIcon = -1;
    m_dragInsertion = -1;
    m_dragOriginIndex = -1;
    m_dragOriginBounds = {};
    m_dragGrabOffset = {};
    m_pressedAt = {};
    m_pressedAtTime = 0.0;
    m_pressedTarget.clear();
    m_draggedTarget.clear();
    m_scalingDivider = false;
    m_hoveredDivider = -1;
    if (restoreSlots) {
        HideDragGhost();
        m_dragSnapAnimating = false;
        for (size_t index = 0; index < m_iconRenderData.size() && index < m_layoutSlotBounds.size();
            ++index) {
            m_iconRenderData[index].bounds = m_layoutSlotBounds[index];
        }
    }
}

void DockApp::StartRefreshTimer() noexcept {
    if (m_window != nullptr) {
        SetTimer(m_window, kRefreshTimerId, kRefreshIntervalMs, nullptr);
    }
}

void DockApp::ScheduleDeferredRefresh() noexcept {
    if (m_window == nullptr) {
        return;
    }
    SetTimer(m_window, kDeferredRefreshTimerId, kDeferredRefreshDelayMs, nullptr);
}

void DockApp::CancelDeferredRefresh() noexcept {
    if (m_window != nullptr) {
        KillTimer(m_window, kDeferredRefreshTimerId);
    }
}

void DockApp::StopRefreshTimer() noexcept {
    if (m_window != nullptr) {
        KillTimer(m_window, kRefreshTimerId);
    }
    CancelDeferredRefresh();
}


void DockApp::ApplyPinUnpinLayoutChange() {
    ProfileScope scope("ApplyPinUnpinLayoutChange");
    // CRITICAL: do NOT call RebuildDisplayApps/BuildDisplayAppsSnapshot on the UI
    // thread. After a pin change, pin profiles are stale, so MatchesAnyPin misses and
    // the ResolveLauncherProcessPath fallback walks every window Ãƒâ€” pin via COM/.lnk Ã¢â‚¬â€
    // that stalls WH_MOUSE_LL (same thread) and freezes the system cursor.
    // Optimistically splice m_displayApps from the already-updated pin list instead.
    const std::wstring pressedTarget = m_pressedTarget;
    const std::wstring draggedTarget = m_draggedTarget;

    std::vector<DisplayApp> byReuse;
    byReuse.reserve(m_displayApps.size());
    for (DisplayApp& entry : m_displayApps) {
        if (IsSpecialDockTarget(entry.app.target) || IsLayoutOnlyTarget(entry.app.target)) {
            continue;
        }
        byReuse.push_back(std::move(entry));
    }

    const auto takeMatching = [&byReuse](const PinnedApp& pin) -> DisplayApp {
        for (auto it = byReuse.begin(); it != byReuse.end(); ++it) {
            if (WindowCatalog::TargetsMatch(it->app.target, pin.target)) {
                DisplayApp found = std::move(*it);
                byReuse.erase(it);
                found.app = pin;
                return found;
            }
        }
        return DisplayApp{pin, nullptr, -1};
    };

    std::vector<DisplayApp> next;
    next.reserve(m_config.Pins().size() + byReuse.size() + 3U);
    next.push_back({{L"Start", kStartTarget, L"", L""}, nullptr, -1});
    next.push_back({{L"Search", kSearchTarget, L"", L""}, nullptr, -1});

    int pinIndex = 0;
    for (const PinnedApp& pin : m_config.Pins()) {
        if (IsSpecialDockTarget(pin.target) || IsLayoutOnlyTarget(pin.target)) {
            continue;
        }
        DisplayApp entry = takeMatching(pin);
        entry.persistentPinIndex = pinIndex++;
        next.push_back(std::move(entry));
    }

    std::vector<DisplayApp> unpinned;
    unpinned.reserve(byReuse.size());
    for (DisplayApp& entry : byReuse) {
        entry.persistentPinIndex = -1;
        if (entry.runningWindow != nullptr) {
            unpinned.push_back(std::move(entry));
        }
    }
    if (!unpinned.empty()) {
        next.push_back({{L"", kDividerTarget, L"", L""}, nullptr, -1});
        next.insert(next.end(), std::make_move_iterator(unpinned.begin()),
            std::make_move_iterator(unpinned.end()));
    }

    m_displayApps = std::move(next);
    m_dividerIndex = DividerIndexFromDisplayApps(m_displayApps);
    RemapInteractionAfterLayoutChange(pressedTarget, draggedTarget);

    if (IconPixelExtent() != m_loadedIconExtent) {
        ScheduleDeferredRefresh();
        RebuildLayout(false);
        return;
    }
    RebuildLayout(false);
    EnsureMissingPinIconsAsync();
}

void DockApp::RemapInteractionAfterLayoutChange(const std::wstring& pressedTarget,
    const std::wstring& draggedTarget) {
    const auto indexOfTarget = [this](const std::wstring& target) -> int {
        if (target.empty()) {
            return -1;
        }
        for (size_t index = 0; index < m_displayApps.size(); ++index) {
            if (WindowCatalog::TargetsMatch(m_displayApps[index].app.target, target)) {
                return static_cast<int>(index);
            }
        }
        return -1;
    };

    if (!pressedTarget.empty() || m_pressedIcon >= 0) {
        const int remapped = indexOfTarget(pressedTarget);
        if (remapped < 0 || !IsPersistentDisplayIcon(remapped)) {
            if (GetCapture() == m_inputWindow) {
                ReleaseCapture();
            }
            ClearPressState();
            m_suppressDragUntilRelease = true;
            HideDragGhost();
            return;
        }
        m_pressedIcon = remapped;
        m_pressedTarget = m_displayApps[static_cast<size_t>(remapped)].app.target;
    }

    if (!draggedTarget.empty() || m_draggedIcon >= 0) {
        const int remapped = indexOfTarget(draggedTarget.empty() ? pressedTarget : draggedTarget);
        if (remapped < 0 || !IsPersistentDisplayIcon(remapped)) {
            if (GetCapture() == m_inputWindow) {
                ReleaseCapture();
            }
            ClearPressState();
            m_suppressDragUntilRelease = true;
            HideDragGhost();
            return;
        }
        m_draggedIcon = remapped;
        m_dragOriginIndex = remapped;
        m_draggedTarget = m_displayApps[static_cast<size_t>(remapped)].app.target;
        if (static_cast<size_t>(remapped) < m_iconRenderData.size()) {
            m_dragOriginBounds = m_iconRenderData[static_cast<size_t>(remapped)].bounds;
        }
    }
}

void DockApp::ScheduleConfigSave() noexcept {
    if (m_window != nullptr) {
        SetTimer(m_window, kConfigSaveTimerId, 400, nullptr);
    }
}

DockApp::RefreshSnapshot DockApp::BuildDisplayAppsSnapshot(const WindowCatalog& windows,
    const std::vector<PinnedApp>& pins, const std::vector<DockApp::DisplayApp>& currentApps) {
    DockApp::RefreshSnapshot snapshot;
    std::vector<DockApp::DisplayApp>& displayApps = snapshot.displayApps;
    displayApps.reserve(pins.size() + windows.RunningWindows().size() + 3U);
    std::vector<DockApp::DisplayApp> unpinnedApps;
    unpinnedApps.reserve(windows.RunningWindows().size());

    displayApps.push_back({{L"Start", kStartTarget, L"", L""}, nullptr, -1});
    displayApps.push_back({{L"Search", kSearchTarget, L"", L""}, nullptr, -1});

    for (size_t index = 0; index < pins.size(); ++index) {
        const PinnedApp& pin = pins[index];
        if (IsSpecialDockTarget(pin.target) || IsLayoutOnlyTarget(pin.target)) {
            continue;
        }
        displayApps.push_back({pin, windows.FindWindowFor(pin), static_cast<int>(index)});
    }

    std::unordered_set<HWND> representedHandles;
    representedHandles.reserve(displayApps.size() + windows.RunningWindows().size());
    for (const DockApp::DisplayApp& app : displayApps) {
        if (app.runningWindow != nullptr) {
            representedHandles.insert(app.runningWindow);
        }
    }

    const auto alreadyRepresentedWindow = [&displayApps, &unpinnedApps, &windows,
                                           &representedHandles](const RunningWindow& window) {
        if (representedHandles.contains(window.handle)) {
            return true;
        }
        if (windows.MatchesAnyPin(window)) {
            return true;
        }
        // No ResolveLauncherProcessPath here: background refresh already rebuilt pin
        // profiles, and MatchesAnyPin/MatchesPin/FindWindowFor cover matching. The
        // COM/.lnk resolve fallback froze WH_MOUSE_LL when this ran on the UI thread
        // via optimistic pin RebuildDisplayApps.
        const auto referencesWindow = [&window, &windows](const DockApp::DisplayApp& app) {
            if (app.runningWindow == window.handle) {
                return true;
            }
            if (app.persistentPinIndex >= 0) {
                return windows.MatchesPin(app.app, window) ||
                    WindowCatalog::TargetsMatch(app.app.target, window.executablePath);
            }
            return WindowCatalog::TargetsMatch(app.app.target, window.executablePath);
        };
        return std::ranges::any_of(displayApps, referencesWindow) ||
            std::ranges::any_of(unpinnedApps, referencesWindow);
    };

    for (const DockApp::DisplayApp& existing : currentApps) {
        if (IsSpecialDockTarget(existing.app.target) || IsLayoutOnlyTarget(existing.app.target) ||
            existing.persistentPinIndex >= 0) {
            continue;
        }
        const auto matchingWindow = std::ranges::find_if(windows.RunningWindows(),
            [&existing, &windows](const RunningWindow& running) {
                return windows.MatchesPin(existing.app, running) ||
                    WindowCatalog::TargetsMatch(existing.app.target, running.executablePath);
            });
        if (matchingWindow == windows.RunningWindows().end() || alreadyRepresentedWindow(*matchingWindow)) {
            continue;
        }
        unpinnedApps.push_back({existing.app, matchingWindow->handle, -1});
        representedHandles.insert(matchingWindow->handle);
    }

    for (const RunningWindow& window : windows.RunningWindows()) {
        if (alreadyRepresentedWindow(window)) {
            continue;
        }

        PinnedApp app;
        app.target = window.executablePath;
        app.name = WindowCatalog::DisplayNameForApp(app, window.handle);
        unpinnedApps.push_back({std::move(app), window.handle, -1});
        representedHandles.insert(window.handle);
    }

    if (!unpinnedApps.empty()) {
        displayApps.push_back({{L"", kDividerTarget, L"", L""}, nullptr, -1});
        displayApps.insert(displayApps.end(), unpinnedApps.begin(), unpinnedApps.end());
    }

    snapshot.layoutChanged = displayApps.size() != currentApps.size() ||
        !std::equal(displayApps.begin(), displayApps.end(), currentApps.begin(),
            [](const DockApp::DisplayApp& left, const DockApp::DisplayApp& right) {
                return left.persistentPinIndex == right.persistentPinIndex &&
                    left.app.target == right.app.target;
            });
    snapshot.runningChanged = !snapshot.layoutChanged &&
        !std::equal(displayApps.begin(), displayApps.end(), currentApps.begin(),
            [](const DockApp::DisplayApp& left, const DockApp::DisplayApp& right) {
                return left.persistentPinIndex == right.persistentPinIndex &&
                    left.app.target == right.app.target &&
                    left.runningWindow == right.runningWindow;
            });
    return snapshot;
}

std::vector<std::wstring> DockApp::IconCacheKeysFromDisplayApps(
    const std::vector<DockApp::DisplayApp>& displayApps) {
    std::vector<std::wstring> keys;
    keys.reserve(displayApps.size());
    for (const DockApp::DisplayApp& app : displayApps) {
        if (IsLayoutOnlyTarget(app.app.target)) {
            continue;
        }
        keys.push_back(WindowCatalog::IconCacheKey(app.app));
    }
    return keys;
}

int DockApp::DividerIndexFromDisplayApps(const std::vector<DockApp::DisplayApp>& displayApps) {
    for (size_t index = 0; index < displayApps.size(); ++index) {
        if (IsLayoutOnlyTarget(displayApps[index].app.target)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void DockApp::RefreshRunningWindows(bool force) {
    BeginBackgroundRefresh(force);
}

void DockApp::BeginBackgroundRefresh(bool force) {
    // Still refresh while Hidden so an app that opens off-screen updates
    // m_displayApps / layout before the next reveal (no width flash). Skip
    // only while Hiding - BeginHide bumps the generation and the slide owns
    // the frame.
    if (m_visibility == VisibilityState::Hiding) {
        return;
    }

    const double now = QpcSeconds();
    if (!force && now - m_lastWindowRefresh < static_cast<double>(kRefreshIntervalMs) / 1000.0) {
        return;
    }

    if (m_refreshInFlight.exchange(true)) {
        return;
    }

    const UINT generation = m_refreshGeneration.load();
    const std::vector<PinnedApp> pins = m_config.Pins();
    const std::vector<DisplayApp> currentApps = m_displayApps;
    const UINT iconPixelExtent = m_loadedIconExtent != 0 ? m_loadedIconExtent : IconPixelExtent();
    std::vector<std::wstring> cachedIconTargets;
    cachedIconTargets.reserve(m_renderer.IconTargetKeys().size());
    for (const std::wstring& target : m_renderer.IconTargetKeys()) {
        if (m_renderer.HasCachedIconPixels(target)) {
            cachedIconTargets.push_back(target);
        }
    }

    std::thread([this, force, generation, pins, currentApps, iconPixelExtent, cachedIconTargets]() {
        struct RefreshScope {
            DockApp* app;
            ~RefreshScope() {
                app->m_refreshInFlight = false;
            }
        } refreshScope{this};

        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        struct ComScope {
            HRESULT result = E_FAIL;
            ~ComScope() {
                if (SUCCEEDED(result)) {
                    CoUninitialize();
                }
            }
        } comScope{.result = comResult};

        WindowCatalog windows;
        // Profiles before refresh: enrichment needs the flag set first, and a
        // fresh catalog always starts unenriched.
        windows.RebuildPinProfiles(pins);
        if (!windows.Refresh() && !force) {
            return;
        }

        RefreshSnapshot snapshot = DockApp::BuildDisplayAppsSnapshot(windows, pins, currentApps);
        snapshot.windows = std::move(windows);
        if (!snapshot.layoutChanged && !snapshot.runningChanged) {
            return;
        }

        if (snapshot.layoutChanged || snapshot.runningChanged) {
            snapshot.missingIconTargets.clear();
            snapshot.missingIconPixels.clear();
            size_t extracted = 0;
            for (const DisplayApp& app : snapshot.displayApps) {
                if (app.app.target == kDividerTarget) {
                    continue;
                }
                const std::wstring key = WindowCatalog::IconCacheKey(app.app);
                if (generation != m_refreshGeneration.load()) {
                    return;
                }
                const bool alreadyCached = std::ranges::any_of(cachedIconTargets,
                    [&key](const std::wstring& cached) {
                        return cached == key;
                    });
                if (alreadyCached) {
                    continue;
                }
                if (extracted >= kMaxMissingIconsPerRefresh) {
                    break;
                }
                snapshot.missingIconTargets.push_back(key);
                snapshot.missingIconPixels.push_back(Renderer::ExtractIconPixels(
                    WindowCatalog::IconResolutionCandidates(app.app, app.runningWindow), iconPixelExtent));
                ++extracted;
            }
        }

        if (generation != m_refreshGeneration.load()) {
            return;
        }

        {
            const std::lock_guard lock(m_refreshSnapshotMutex);
            m_refreshSnapshot = std::move(snapshot);
        }
        PostMessageW(m_window, kRefreshApplyMessage, static_cast<WPARAM>(generation), 0);
    }).detach();
}

void DockApp::ApplyBackgroundRefresh(UINT generation) {
    ProfileScope scope("ApplyBackgroundRefresh");
    if (generation != m_refreshGeneration.load()) {
        return;
    }
    // Apply while Hidden too: RebuildLayout + icon uploads update the dormant
    // dock so BeginShow already has the correct width/icons. QueueRenderFrame
    // no-ops when Hidden. Skip Hiding so we do not fight the slide.
    if (m_visibility == VisibilityState::Hiding) {
        return;
    }

    DockApp::RefreshSnapshot snapshot;
    {
        const std::lock_guard lock(m_refreshSnapshotMutex);
        if (!m_refreshSnapshot.has_value()) {
            return;
        }
        snapshot = std::move(*m_refreshSnapshot);
        m_refreshSnapshot.reset();
    }

    m_lastWindowRefresh = QpcSeconds();
    m_windows = std::move(snapshot.windows);
    m_displayApps = std::move(snapshot.displayApps);
    m_dividerIndex = DividerIndexFromDisplayApps(m_displayApps);

    if (snapshot.layoutChanged) {
        // Prefer remapping by app.target; if the pressed/dragged app vanished or is
        // no longer a persistent pin, cancel so a Settings tile cannot follow the cursor.
        const std::wstring pressedTarget = m_pressedTarget;
        const std::wstring draggedTarget = m_draggedTarget;
        RemapInteractionAfterLayoutChange(pressedTarget, draggedTarget);
        RebuildLayout(false);
    }

    if (!snapshot.missingIconTargets.empty()) {
        try {
            m_renderer.AppendMissingIcons(snapshot.missingIconTargets, snapshot.missingIconPixels);
        } catch (const std::exception&) {
            Log(L"Incremental icon upload failed; reloading icons.");
            LoadIconTextures();
        }
        AssignIconTextureIndices();
        QueueRenderFrame();
        // Refresh worker only extracts a few icons per wave; finish the rest
        // on the dedicated pin-icon worker so first-open storms stay frame-budgeted.
        EnsureMissingPinIconsAsync();
        if (!snapshot.runningChanged) {
            return;
        }
    } else if (snapshot.layoutChanged) {
        AssignIconTextureIndices();
        EnsureMissingPinIconsAsync();
        QueueRenderFrame();
        return;
    }

    if (snapshot.runningChanged) {
        bool runningVisualChanged = false;
        const size_t iconCount = LesserOf(m_iconRenderData.size(), m_displayApps.size());
        for (size_t index = 0; index < iconCount; ++index) {
            const bool running = m_displayApps[index].runningWindow != nullptr;
            if (m_iconRenderData[index].running != running) {
                m_iconRenderData[index].running = running;
                runningVisualChanged = true;
            }
        }
        if (runningVisualChanged) {
            QueueRenderFrame();
        }
    }
}

bool DockApp::RebuildDisplayApps(bool* structureChanged) {
    RefreshSnapshot snapshot = BuildDisplayAppsSnapshot(m_windows, m_config.Pins(), m_displayApps);
    if (structureChanged != nullptr) {
        *structureChanged = snapshot.layoutChanged;
    }
    m_displayApps = std::move(snapshot.displayApps);
    m_dividerIndex = DividerIndexFromDisplayApps(m_displayApps);
    return snapshot.layoutChanged || snapshot.runningChanged;
}

void DockApp::EnsureMissingPinIconsAsync() {
    if (!m_rendererInitialized || m_window == nullptr) {
        return;
    }
    // Collect only the icons absent from the GPU pixel cache (typically zero
    // for unpin, one for pin) so the UI thread never re-extracts the full set.
    struct MissingIcon {
        std::wstring key;
        std::vector<std::wstring> candidates;
    };
    std::vector<MissingIcon> missing;
    missing.reserve(2);
    for (const DisplayApp& app : m_displayApps) {
        if (IsLayoutOnlyTarget(app.app.target)) {
            continue;
        }
        const std::wstring key = WindowCatalog::IconCacheKey(app.app);
        if (m_renderer.HasCachedIconPixels(key)) {
            continue;
        }
        missing.push_back(
            {key, WindowCatalog::IconResolutionCandidates(app.app, app.runningWindow)});
    }
    if (missing.empty()) {
        return;
    }

    const UINT generation = ++m_pinIconGeneration;
    const UINT extent =
        m_loadedIconExtent != 0 ? m_loadedIconExtent : IconPixelExtent();
    const HWND replyWindow = m_window;
    {
        const std::lock_guard lock(m_pinIconMutex);
        m_pinIconPending.reset();
    }
    std::thread([this, generation, replyWindow, extent, work = std::move(missing)]() {
        PinIconResult result;
        result.generation = generation;
        result.targets.reserve(work.size());
        result.pixels.reserve(work.size());
        for (const MissingIcon& icon : work) {
            if (generation != m_pinIconGeneration.load()) {
                return;
            }
            result.targets.push_back(icon.key);
            result.pixels.push_back(
                Renderer::ExtractIconPixels(icon.candidates, extent));
        }
        if (generation != m_pinIconGeneration.load()) {
            return;
        }
        {
            const std::lock_guard lock(m_pinIconMutex);
            m_pinIconPending = std::move(result);
        }
        PostMessageW(replyWindow, kPinIconMessage, static_cast<WPARAM>(generation), 0);
    }).detach();
}

void DockApp::ApplyPinIcons(UINT generation) {
    if (generation != m_pinIconGeneration.load()) {
        return;
    }
    PinIconResult result;
    {
        const std::lock_guard lock(m_pinIconMutex);
        if (!m_pinIconPending.has_value() || m_pinIconPending->generation != generation) {
            return;
        }
        result = std::move(*m_pinIconPending);
        m_pinIconPending.reset();
    }
    if (result.targets.empty()) {
        return;
    }
    try {
        m_renderer.AppendMissingIcons(result.targets, result.pixels);
    } catch (const std::exception&) {
        Log(L"Incremental pin icon upload failed; reloading icons.");
        LoadIconTextures();
    }
    AssignIconTextureIndices();
    QueueRenderFrame();
}

void DockApp::HideTaskbar() {
    SuppressNativeTaskbar();
    StartTaskbarMonitor();
}

void DockApp::RestoreTaskbar() {
    m_shellFlyoutHold = false;
    m_shellFlyoutHoldUntil = 0.0;
    m_shellFlyoutSeen = false;
    StopTaskbarMonitor();
    DestroyFullscreenClaimWindows();
    if (m_taskbarStateSaved) {
        SetTaskbarAppBarState(m_savedTaskbarState);
        m_taskbarStateSaved = false;
    }

    std::vector<HWND> taskbars;
    EnumWindows(&DockApp::FindTaskbarWindow, reinterpret_cast<LPARAM>(&taskbars));
    for (HWND taskbar : taskbars) {
        // Re-register placement first: a bare SHOWWINDOW is ignored while the
        // appbar is still collapsed to a zero rect from suppression.
        for (const auto& [knownWindow, knownRect] : m_hiddenTaskbarRects) {
            if (knownWindow == taskbar && knownRect.bottom > knownRect.top &&
                knownRect.right > knownRect.left) {
                APPBARDATA data{};
                data.cbSize = sizeof(data);
                data.hWnd = taskbar;
                data.uEdge = ABE_BOTTOM;
                data.rc = {knownRect.left, knownRect.top, knownRect.right, knownRect.bottom};
                SHAppBarMessage(ABM_QUERYPOS, &data);
                SHAppBarMessage(ABM_SETPOS, &data);
                SHAppBarMessage(ABM_WINDOWPOSCHANGED, &data);
                break;
            }
        }
        SetWindowPos(taskbar, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (m_taskbarHidden) {
        Log(L"Native taskbar restored.");
    }
    m_taskbarHidden = false;
    m_hiddenTaskbars.clear();
    m_hiddenTaskbarRects.clear();
}

void DockApp::SuppressNativeTaskbar() {
    if (!m_taskbarStateSaved) {
        m_savedTaskbarState = GetTaskbarAppBarState();
        m_taskbarStateSaved = true;
    }
    // ITaskbarList::HrInit can fail once the taskbar is hidden/collapsed, so
    // establish the list first while Explorer is still in its normal state.
    EnsureTaskbarList();

    std::vector<HWND> visibleCheck;
    EnumWindows(&DockApp::FindTaskbarWindow, reinterpret_cast<LPARAM>(&visibleCheck));
    const bool anyTaskbarVisible = std::ranges::any_of(visibleCheck, [](HWND taskbar) {
        return IsWindowVisible(taskbar) != FALSE;
    });
    if (!m_workAreaSaved) {
        if (anyTaskbarVisible &&
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &m_savedWorkArea, 0) != FALSE) {
            // Normal case: capture the reserved area while it exists.
        } else {
            // Abnormal case (previous session ended with the taskbar hidden):
            // persisting the unreserved area would make restoration impossible
            // (Explorer vetoes SHOW while nothing is reserved), so derive it.
            const UINT dpi = HostDpi();
            const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
            LONG strip = std::max(40L, static_cast<LONG>(std::lround(48.0F * scale)));
            for (const auto& [knownWindow, knownRect] : m_hiddenTaskbarRects) {
                const LONG height = knownRect.bottom - knownRect.top;
                if (height > 0 && height <= m_primaryBounds.bottom - m_primaryBounds.top) {
                    strip = height;
                    break;
                }
            }
            m_savedWorkArea = m_primaryBounds;
            m_savedWorkArea.bottom -= strip;
        }
        m_workAreaSaved = true;
    }

    const UINT taskbarState = GetTaskbarAppBarState();
    if ((taskbarState & ABS_AUTOHIDE) != 0) {
        SetTaskbarAppBarState(taskbarState & ~ABS_AUTOHIDE);
    }

    m_hiddenTaskbars.clear();
    EnumWindows(&DockApp::FindTaskbarWindow, reinterpret_cast<LPARAM>(&m_hiddenTaskbars));
    m_hiddenTaskbarRects.clear();
    m_hiddenTaskbarRects.reserve(m_hiddenTaskbars.size());
    for (HWND taskbar : m_hiddenTaskbars) {
        RECT bounds{};
        GetWindowRect(taskbar, &bounds);
        m_hiddenTaskbarRects.emplace_back(taskbar, bounds);
        HideTaskbarWindow(taskbar);
    }
    static_cast<void>(CollapseNativeTaskbarAppBar());
    DisableMultiMonitorTaskbars();
    EnsureFullscreenClaimWindows();

    const bool found = !m_hiddenTaskbars.empty();
    if (found != m_taskbarHidden) {
        Log(found ? L"Native taskbar hidden." : L"Native taskbar was not found.");
    }
    m_taskbarHidden = found;
    static_cast<void>(ExpandPrimaryWorkArea(!m_workAreaExpanded));
    static_cast<void>(ExpandSecondaryMonitorWorkAreas());
}

bool DockApp::ExpandPrimaryWorkArea(bool notify) {
    if (m_primaryBounds.right <= m_primaryBounds.left ||
        m_primaryBounds.bottom <= m_primaryBounds.top) {
        return false;
    }

    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) == FALSE) {
        return false;
    }
    if (work.left == m_primaryBounds.left && work.top == m_primaryBounds.top &&
        work.right == m_primaryBounds.right && work.bottom == m_primaryBounds.bottom) {
        m_workAreaExpanded = true;
        m_hasLastObservedWorkArea = true;
        m_lastObservedWorkArea = work;
        return false;
    }

    // Explorer often leaves the same non-full work area in place. Retrying the
    // set is fine, but reporting "corrected" every pass pinned the monitor at
    // 100 ms and re-enumerated every top-level window for the life of the process.
    const bool repeat =
        m_hasLastObservedWorkArea && RectsEqual(work, m_lastObservedWorkArea);
    m_hasLastObservedWorkArea = true;
    m_lastObservedWorkArea = work;

    RECT full = m_primaryBounds;
    const UINT flags = notify ? SPIF_SENDCHANGE : 0;
    if (SystemParametersInfoW(SPI_SETWORKAREA, 0, &full, flags) != FALSE && notify) {
        Log(L"Desktop work area expanded.");
    }
    m_workAreaExpanded = true;
    return !repeat;
}

void DockApp::RestoreDesktopWorkArea() {
    if (!m_workAreaSaved) {
        return;
    }
    static_cast<void>(SystemParametersInfoW(SPI_SETWORKAREA, 0, &m_savedWorkArea, SPIF_SENDCHANGE));
    m_workAreaSaved = false;
    m_workAreaExpanded = false;
    m_hasLastObservedWorkArea = false;
}

void DockApp::NotifyExplorerTraySettings() noexcept {
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
        reinterpret_cast<LPARAM>(L"TraySettings"));
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray != nullptr) {
        SendNotifyMessageW(tray, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"));
        PostMessageW(tray, 0x05B4, 0, 0);
    }
}

void DockApp::DisableMultiMonitorTaskbars() {
    if (!m_multiMonTaskbarSaved) {
        m_multiMonTaskbarHadValue = ReadDwordValue(kMultiMonTaskbarValue, m_savedMultiMonTaskbar);
        if (!m_multiMonTaskbarHadValue) {
            m_savedMultiMonTaskbar = 1;
        }
        m_multiMonTaskbarSaved = true;
    }

    DWORD current = 1;
    if (ReadDwordValue(kMultiMonTaskbarValue, current) && current == 0) {
        return;
    }

    WriteDwordValue(kMultiMonTaskbarValue, 0);
    NotifyExplorerTraySettings();
}

void DockApp::RestoreMultiMonitorTaskbars() {
    if (!m_multiMonTaskbarSaved) {
        return;
    }
    if (m_multiMonTaskbarHadValue) {
        WriteDwordValue(kMultiMonTaskbarValue, m_savedMultiMonTaskbar);
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kExplorerAdvancedKey, kMultiMonTaskbarValue);
    }
    m_multiMonTaskbarSaved = false;
    NotifyExplorerTraySettings();
}

void DockApp::CollectTaskbarWindows(std::vector<HWND>& taskbars) {
    taskbars.clear();
    EnumWindows(&DockApp::FindTaskbarWindow, reinterpret_cast<LPARAM>(&taskbars));
}

bool DockApp::ExpandSecondaryMonitorWorkAreas() {
    CollectTaskbarWindows(m_taskbarEnumScratch);
    const std::vector<HWND>& taskbars = m_taskbarEnumScratch;

    m_monitorEnumScratch.clear();
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitorBounds,
        reinterpret_cast<LPARAM>(&m_monitorEnumScratch));
    const std::vector<std::pair<HMONITOR, RECT>>& monitors = m_monitorEnumScratch;
    bool changed = false;
    for (const auto& [monitor, bounds] : monitors) {
        MONITORINFO information{sizeof(information)};
        if (GetMonitorInfoW(monitor, &information) == FALSE) {
            continue;
        }
        if ((information.dwFlags & MONITORINFOF_PRIMARY) != 0) {
            continue;
        }
        if (RectsEqual(information.rcWork, information.rcMonitor)) {
            continue;
        }

        for (HWND tray : taskbars) {
            wchar_t className[64]{};
            if (GetClassNameW(tray, className, static_cast<int>(std::size(className))) == 0) {
                continue;
            }
            if (std::wstring_view(className) != L"Shell_SecondaryTrayWnd") {
                continue;
            }
            if (MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST) != monitor) {
                continue;
            }

            // Already collapsed: another SHAppBarMessage every pass kept the
            // monitor at 100 ms on any secondary display Explorer would not
            // give a full work area.
            RECT trayRect{};
            GetWindowRect(tray, &trayRect);
            if (IsWindowVisible(tray) == FALSE && trayRect.bottom - trayRect.top <= 0 &&
                trayRect.top >= information.rcMonitor.bottom) {
                continue;
            }

            APPBARDATA data{};
            data.cbSize = sizeof(data);
            data.hWnd = tray;
            SHAppBarMessage(ABM_REMOVE, &data);
            HideTaskbarWindow(tray);
            SetWindowPos(tray, HWND_BOTTOM, information.rcMonitor.left, information.rcMonitor.bottom,
                information.rcMonitor.right - information.rcMonitor.left, 0,
                SWP_NOACTIVATE | SWP_HIDEWINDOW);
            changed = true;
        }
    }
    return changed;
}

void DockApp::EnsureTaskbarList() {
    if (m_taskbarList2 != nullptr) {
        return;
    }
    HRESULT created = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_taskbarList2));
    if (FAILED(created)) {
        m_taskbarList2 = nullptr;
        Log(L"ITaskbarList2 is unavailable (HRESULT " + std::to_wstring(static_cast<unsigned long>(created)) +
            L").");
        return;
    }
    const HRESULT initialized = m_taskbarList2->HrInit();
    if (FAILED(initialized)) {
        m_taskbarList2->Release();
        m_taskbarList2 = nullptr;
        Log(L"ITaskbarList2 initialization failed (HRESULT " +
            std::to_wstring(static_cast<unsigned long>(initialized)) + L").");
    }
}

void DockApp::EnsureFullscreenClaimWindows() {
    EnsureTaskbarList();

    const wchar_t className[] = L"LiquidGlassDockFullscreenClaim";
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = &FullscreenClaimProcedure;
    windowClass.hInstance = m_instance;
    windowClass.lpszClassName = className;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log(L"Could not register the fullscreen claim window class.");
        return;
    }

    std::vector<std::pair<HMONITOR, RECT>> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitorBounds, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) {
        return;
    }

    for (size_t index = 0; index < m_fullscreenClaims.size();) {
        FullscreenClaim& claim = m_fullscreenClaims[index];
        const bool stillPresent = std::ranges::any_of(monitors, [&](const auto& entry) {
            return entry.first == claim.monitor;
        });
        if (stillPresent) {
            ++index;
            continue;
        }
        if (m_taskbarList2 != nullptr && claim.window != nullptr) {
            m_taskbarList2->MarkFullscreenWindow(claim.window, FALSE);
        }
        if (claim.window != nullptr) {
            DestroyWindow(claim.window);
        }
        m_fullscreenClaims.erase(m_fullscreenClaims.begin() + static_cast<std::ptrdiff_t>(index));
    }

    constexpr DWORD extendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
        WS_EX_TRANSPARENT;
    for (const auto& [monitor, bounds] : monitors) {
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        if (width <= 0 || height <= 0) {
            continue;
        }

        FullscreenClaim* claim = nullptr;
        for (FullscreenClaim& existing : m_fullscreenClaims) {
            if (existing.monitor == monitor) {
                claim = &existing;
                break;
            }
        }
        if (claim == nullptr) {
            HWND window = CreateWindowExW(extendedStyle, className, L"", WS_POPUP, bounds.left,
                bounds.top, width, height, nullptr, nullptr, m_instance, nullptr);
            if (window == nullptr) {
                Log(L"Could not create a fullscreen claim window.");
                continue;
            }
            SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
            m_fullscreenClaims.push_back({monitor, bounds, window});
            claim = &m_fullscreenClaims.back();
        } else if (!RectsEqual(claim->bounds, bounds)) {
            claim->bounds = bounds;
        }

        SetWindowPos(claim->window, HWND_BOTTOM, bounds.left, bounds.top, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (m_taskbarList2 != nullptr) {
            m_taskbarList2->MarkFullscreenWindow(claim->window, TRUE);
        }
    }
}

void DockApp::DestroyFullscreenClaimWindows() noexcept {
    for (FullscreenClaim& claim : m_fullscreenClaims) {
        if (m_taskbarList2 != nullptr && claim.window != nullptr) {
            m_taskbarList2->MarkFullscreenWindow(claim.window, FALSE);
        }
        if (claim.window != nullptr) {
            DestroyWindow(claim.window);
        }
    }
    m_fullscreenClaims.clear();
    if (m_taskbarList2 != nullptr) {
        m_taskbarList2->Release();
        m_taskbarList2 = nullptr;
    }
}

bool DockApp::CollapseNativeTaskbarAppBar() {
    CollectTaskbarWindows(m_taskbarEnumScratch);
    const std::vector<HWND>& taskbars = m_taskbarEnumScratch;
    bool collapsed = false;
    m_collapsedTaskbarScratch.clear();
    if (m_collapsedTaskbarScratch.capacity() < taskbars.size()) {
        m_collapsedTaskbarScratch.reserve(taskbars.size());
    }
    for (HWND tray : taskbars) {
        RECT trayRect{};
        GetWindowRect(tray, &trayRect);
        RECT monitorBounds = m_primaryBounds;
        static_cast<void>(MonitorRect(MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST), monitorBounds));
        if (trayRect.bottom - trayRect.top <= 0 && IsWindowVisible(tray) == FALSE &&
            trayRect.top >= monitorBounds.bottom) {
            continue;
        }

        // Idempotence: a hidden taskbar keeps a stale non-zero rect, so act only
        // when its geometry or visibility changed since we last collapsed it.
        // Otherwise every pass would resend appbar traffic pointlessly.
        const bool visible = IsWindowVisible(tray) != FALSE;
        bool unchanged = false;
        for (const auto& [knownWindow, knownRect] : m_collapsedTaskbars) {
            if (knownWindow == tray && RectsEqual(knownRect, trayRect)) {
                unchanged = !visible;
                break;
            }
        }
        if (unchanged) {
            m_collapsedTaskbarScratch.emplace_back(tray, trayRect);
            continue;
        }

        APPBARDATA data{};
        data.cbSize = sizeof(data);
        data.hWnd = tray;
        data.uEdge = ABE_BOTTOM;
        data.rc = {monitorBounds.left, monitorBounds.bottom, monitorBounds.right, monitorBounds.bottom};
        SHAppBarMessage(ABM_QUERYPOS, &data);
        data.rc.top = data.rc.bottom;
        SHAppBarMessage(ABM_SETPOS, &data);
        SHAppBarMessage(ABM_WINDOWPOSCHANGED, &data);
        SetWindowPos(tray, HWND_BOTTOM, monitorBounds.left, monitorBounds.bottom,
            monitorBounds.right - monitorBounds.left, 0, SWP_NOACTIVATE | SWP_HIDEWINDOW);
        m_collapsedTaskbarScratch.emplace_back(tray, trayRect);
        collapsed = true;
    }
    m_collapsedTaskbars.swap(m_collapsedTaskbarScratch);
    return collapsed;
}

bool DockApp::MaintainNativeTaskbarSuppression() {
    if (m_shellFlyoutHold) {
        CollectTaskbarWindows(m_taskbarEnumScratch);
        for (HWND taskbar : m_taskbarEnumScratch) {
            if (IsWindowVisible(taskbar) != FALSE) {
                HideTaskbarWindow(taskbar);
            }
        }
        // Adaptive hold for the Win+N calendar/notifications panel (plus the
        // Start island, same as before). The old code held a blind 2.5 s from
        // click and only tracked Start, so a quickly-dismissed panel â€” or one
        // Explorer refused â€” kept the dock hidden for seconds after it was
        // gone. Instead: wait briefly for the panel to appear, hold while it
        // is visible, then release after a short slide-out grace.
        static constexpr double kFlyoutCloseGraceSeconds = 0.35;
        if (IsAnyShellFlyoutVisible()) {
            m_shellFlyoutSeen = true;
            m_shellFlyoutHoldUntil = 0.0;
            return true;
        }
        const double now = QpcSeconds();
        if (m_shellFlyoutSeen) {
            if (m_shellFlyoutHoldUntil == 0.0) {
                m_shellFlyoutHoldUntil = now + kFlyoutCloseGraceSeconds;
            }
            if (now < m_shellFlyoutHoldUntil) {
                return true;
            }
        } else if (m_shellFlyoutHoldUntil > 0.0 && now < m_shellFlyoutHoldUntil) {
            return true;
        }
        ReleaseShellFlyoutHold();
        return true;
    }

    bool corrected = false;
    const UINT taskbarState = GetTaskbarAppBarState();
    if ((taskbarState & ABS_AUTOHIDE) != 0) {
        SetTaskbarAppBarState(taskbarState & ~ABS_AUTOHIDE);
        corrected = true;
    }

    CollectTaskbarWindows(m_taskbarEnumScratch);
    for (HWND taskbar : m_taskbarEnumScratch) {
        if (IsWindowVisible(taskbar) != FALSE) {
            HideTaskbarWindow(taskbar);
            corrected = true;
        }
    }
    corrected = CollapseNativeTaskbarAppBar() || corrected;
    corrected = ExpandPrimaryWorkArea(false) || corrected;
    corrected = ExpandSecondaryMonitorWorkAreas() || corrected;
    return corrected;
}

void DockApp::StartTaskbarMonitor() noexcept {
    // HideTaskbar() already applied suppression synchronously; start calm and
    // promote to 100 ms only while MaintainNativeTaskbarSuppression fights Explorer.
    m_taskbarMonitorQuietPasses = kTaskbarMonitorCalmPasses;
    m_taskbarMonitorFast = false;
    if (m_window != nullptr) {
        SetTimer(m_window, kTaskbarMonitorTimerId, kTaskbarMonitorSlowIntervalMs, nullptr);
    }
}

void DockApp::StopTaskbarMonitor() noexcept {
    if (m_window != nullptr) {
        KillTimer(m_window, kTaskbarMonitorTimerId);
    }
}

void DockApp::StartCursorWatch() noexcept {
    m_cursorWatchArmed = true;
    m_cursorWatchAppliedMs = 0;
    SyncCursorWatchInterval();
}

void DockApp::StopCursorWatch() noexcept {
    m_cursorWatchArmed = false;
    m_cursorWatchAppliedMs = 0;
    m_cursorWatchTimerRunning = false;
    if (m_window != nullptr) {
        KillTimer(m_window, kCursorWatchTimerId);
    }
}

void DockApp::EnsureMouseHook() noexcept {
    if (m_mouseHook != nullptr || m_instance == nullptr) {
        return;
    }
    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &DockApp::MouseHook, m_instance, 0);
    if (m_mouseHook == nullptr) {
        Log(L"Low-level mouse hook reinstall failed.");
    }
}

UINT DockApp::DesiredCursorWatchIntervalMs() const noexcept {
    // Fast poll while interacting, when the LL hook is missing, or when an elevated
    // foreground window may UIPI-block WH_MOUSE_LL. When Hidden with a healthy hook,
    // return 0: no timer Ã¢â‚¬â€ hot-zone entry arrives via the hook; FG changes arrive via
    // WinEvent. Visible + stationary pointer uses a calmer poll (hover still works;
    // leave/hide remains hook-assisted), unless the context menu is open where a
    // prompt leave-dismiss needs the fast cadence. Does not change kBackdropIntervalMs.
    if (m_visibility == VisibilityState::Hidden) {
        if (m_mouseHook == nullptr || IsElevatedForeground()) {
            return kCursorWatchIntervalMs;
        }
        return kCursorWatchHiddenIntervalMs;
    }
    if (m_visibility == VisibilityState::Visible && m_cursorWatchCalm && !IsDragActive() &&
        !IsContextMenuOpen()) {
        return kCursorWatchCalmIntervalMs;
    }
    return kCursorWatchIntervalMs;
}

void DockApp::SyncCursorWatchInterval() noexcept {
    if (m_window == nullptr || !m_cursorWatchArmed) {
        return;
    }
    const UINT interval = DesiredCursorWatchIntervalMs();
    if (interval == 0) {
        if (m_cursorWatchTimerRunning) {
            KillTimer(m_window, kCursorWatchTimerId);
            m_cursorWatchTimerRunning = false;
            m_cursorWatchAppliedMs = 0;
        }
        return;
    }
    if (m_cursorWatchTimerRunning && interval == m_cursorWatchAppliedMs) {
        return;
    }
    m_cursorWatchAppliedMs = interval;
    m_cursorWatchTimerRunning = true;
    SetTimer(m_window, kCursorWatchTimerId, interval, nullptr);
}

void DockApp::RegisterForegroundWatch() noexcept {
    if (m_foregroundHook != nullptr) {
        return;
    }
    m_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        &DockApp::ForegroundWinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (m_foregroundHook == nullptr) {
        Log(L"Foreground WinEvent hook unavailable; elevated FG uses timer/taskbar sync only.");
    }
}

void DockApp::UnregisterForegroundWatch() noexcept {
    if (m_foregroundHook != nullptr) {
        UnhookWinEvent(m_foregroundHook);
        m_foregroundHook = nullptr;
    }
}

void CALLBACK DockApp::ForegroundWinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (s_instance == nullptr || s_instance->m_window == nullptr) {
        return;
    }
    s_instance->m_renderer.RequestBackdropRefresh();
    PostMessageW(s_instance->m_window, kCursorWatchSyncMessage, 0, 0);
}

bool DockApp::IsElevatedForeground() const noexcept {
    const double now = QpcSeconds();
    constexpr double kCacheSeconds = 0.5;
    if (m_elevatedForegroundValid && (now - m_elevatedForegroundCheckedAt) < kCacheSeconds) {
        return m_elevatedForegroundCached;
    }
    m_elevatedForegroundCached = IsWindowProcessElevated(GetForegroundWindow());
    m_elevatedForegroundCheckedAt = now;
    m_elevatedForegroundValid = true;
    return m_elevatedForegroundCached;
}

void DockApp::PumpCursorWatch() {
    EnsureMouseHook();
    SyncCursorWatchInterval();
    if (IsDragActive()) {
        return;
    }
    POINT cursor{};
    if (GetCursorPos(&cursor) == FALSE) {
        return;
    }
    const double now = QpcSeconds();
    // Large dt between samples means the UI thread hitch'd; drop any in-progress
    // press so a post-stall jitter cannot begin an accidental reorder drag.
    if (m_pressedIcon >= 0 && m_lastPointerSampleAt > 0.0 &&
        now - m_lastPointerSampleAt > kMaxPointerStallSeconds) {
        ClearPressState();
        m_suppressDragUntilRelease = true;
    }
    m_lastPointerSampleAt = now;
    if (cursor.x == m_lastCursor.x && cursor.y == m_lastCursor.y) {
        if (m_visibility == VisibilityState::Visible && !IsDragActive()) {
            if (m_cursorWatchStationaryPumps < 0xFFFFu) {
                ++m_cursorWatchStationaryPumps;
            }
            // ~330 ms of stillness at 33 ms before calming (reduces visible idle wakes).
            if (!m_cursorWatchCalm && m_cursorWatchStationaryPumps >= 10) {
                m_cursorWatchCalm = true;
                SyncCursorWatchInterval();
            }
        }
        return;
    }
    if (m_cursorWatchCalm || m_cursorWatchStationaryPumps != 0) {
        m_cursorWatchCalm = false;
        m_cursorWatchStationaryPumps = 0;
        SyncCursorWatchInterval();
    }
    HandlePointer(cursor);
}


void DockApp::RegisterSystemResumeNotifications() {
    if (m_window == nullptr) {
        return;
    }

    if (m_suspendNotify == nullptr) {
        m_suspendNotify = RegisterSuspendResumeNotification(m_window, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (m_suspendNotify == nullptr) {
            Log(L"Suspend/resume notification registration failed.");
        }
    }
    if (m_monitorNotify == nullptr) {
        m_monitorNotify =
            RegisterPowerSettingNotification(m_window, &kMonitorPowerOnGuid, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (m_monitorNotify == nullptr) {
            Log(L"Monitor power notification registration failed.");
        }
    }
    if (!m_sessionNotifyRegistered) {
        m_sessionNotifyRegistered = WTSRegisterSessionNotification(m_window, NOTIFY_FOR_THIS_SESSION) != FALSE;
        if (!m_sessionNotifyRegistered) {
            Log(L"Session notification registration failed.");
        }
    }
}

void DockApp::UnregisterSystemResumeNotifications() noexcept {
    if (m_suspendNotify != nullptr) {
        UnregisterSuspendResumeNotification(m_suspendNotify);
        m_suspendNotify = nullptr;
    }
    if (m_monitorNotify != nullptr) {
        UnregisterPowerSettingNotification(m_monitorNotify);
        m_monitorNotify = nullptr;
    }
    if (m_sessionNotifyRegistered) {
        if (m_window != nullptr && IsWindow(m_window) != FALSE) {
            WTSUnRegisterSessionNotification(m_window);
        }
        m_sessionNotifyRegistered = false;
    }
}

LRESULT DockApp::HandlePowerBroadcast(WPARAM wParam, LPARAM lParam) {
    if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND ||
        wParam == PBT_APMRESUMECRITICAL) {
        HideTaskbar();
        return TRUE;
    }
    if (wParam == PBT_POWERSETTINGCHANGE) {
        const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
        if (setting != nullptr && setting->DataLength >= sizeof(DWORD) &&
            IsEqualGUID(setting->PowerSetting, kMonitorPowerOnGuid) &&
            *reinterpret_cast<const DWORD*>(setting->Data) != 0) {
            HideTaskbar();
        }
        return TRUE;
    }
    return TRUE;
}

bool DockApp::OpenStartMenuFromDock() {
    // The dock stays visible on a Start click: it hides only via the normal
    // cursor-above-threshold rule in HandlePointer/BeginHide. Do not set the
    // shell-flyout hold or hide the overlay here (that froze pointer handling
    // and dropped the dock immediately on click).
    // Send synchronously on the click thread: the old async PostMessage path
    // raced Explorer with work-area/registry broadcasts issued immediately
    // before SendInput, so the Win keystroke was lost and Start never
    // appeared. Start opens fine with the taskbar hidden and the work area
    // expanded, so no restore/hide dance belongs on this path.
    // Still unmark the fullscreen claims first: while marked, Explorer treats
    // the dock's claim windows as fullscreen apps and can refuse the menu.
    if (GetCapture() != nullptr) {
        ReleaseCapture();
    }
    UnmarkFullscreenClaims();
    static_cast<void>(GrantExplorerForeground());
    if (!SendWinKey()) {
        Log(L"Start menu did not accept input.");
        return false;
    }
    return true;
}

void DockApp::UnmarkFullscreenClaims() {
    EnsureTaskbarList();
    if (m_taskbarList2 != nullptr) {
        for (FullscreenClaim& claim : m_fullscreenClaims) {
            if (claim.window != nullptr) {
                m_taskbarList2->MarkFullscreenWindow(claim.window, FALSE);
            }
        }
    }
}

void DockApp::PrepareShellForStartMenu() {
    if (GetCapture() != nullptr) {
        ReleaseCapture();
    }
    m_shellFlyoutHold = true;
    m_shellFlyoutIsSearch = false;
    m_shellFlyoutIsTray = false;
    // Wait budget for the panel to appear (adaptive hold takes over from
    // there); keeps a refused/missed panel to a sub-second hide.
    m_shellFlyoutHoldUntil = QpcSeconds() + 0.8;
    m_shellFlyoutSeen = false;
    // The taskbar monitor idles at a 5 s cadence when steady; without an
    // immediate promotion the hold release would wait up to 5 s for the next
    // tick.
    m_taskbarMonitorQuietPasses = 0;
    if (!m_taskbarMonitorFast && m_window != nullptr) {
        m_taskbarMonitorFast = true;
        SetTimer(m_window, kTaskbarMonitorTimerId, kTaskbarMonitorIntervalMs, nullptr);
    }
    HideOverlayForShellFlyout();
}

void DockApp::HideOverlayForShellFlyout() {
    m_overlayHiddenForFlyout = true;
    StopBackdropTimer();
    HideHoverLabel();
    CloseOverflowPopup();
    CloseContextMenu();
    if (m_window != nullptr) {
        ShowWindow(m_window, SW_HIDE);
    }
    if (m_inputWindow != nullptr) {
        ShowWindow(m_inputWindow, SW_HIDE);
    }
}

void DockApp::RestoreOverlayAfterShellFlyout() {
    if (!m_overlayHiddenForFlyout) {
        return;
    }
    m_overlayHiddenForFlyout = false;
    if (m_visibility != VisibilityState::Visible && m_visibility != VisibilityState::Showing) {
        return;
    }
    ShowWindow(m_window, SW_SHOWNOACTIVATE);
    if (m_inputWindow != nullptr) {
        ShowWindow(m_inputWindow, SW_SHOWNOACTIVATE);
    }
    PositionOverlayWindows();
    StartBackdropTimer();
    QueueRenderFrame();
}

void DockApp::ReleaseShellFlyoutHold() {
    if (!m_shellFlyoutHold) {
        return;
    }
    m_shellFlyoutHold = false;
    m_shellFlyoutHoldUntil = 0.0;
    m_shellFlyoutSeen = false;
    RestoreOverlayAfterShellFlyout();
    SuppressNativeTaskbar();
    // The panel just closed: sample now so a cursor already sitting in the
    // hot zone reveals instantly instead of waiting for the next mouse move
    // (when Hidden the cursor timer is off and the hook only posts on
    // movement) or the next foreground event.
    if (!IsDragActive() && m_window != nullptr) {
        POINT cursor{};
        if (GetCursorPos(&cursor) != FALSE) {
            HandlePointer(cursor);
        }
    }
    SyncCursorWatchInterval();
}

void DockApp::StopShellFlyoutWatch() noexcept {
    m_shellFlyoutAttempts = 0;
    m_shellFlyoutWindow = nullptr;
    if (m_window != nullptr) {
        KillTimer(m_window, kStartMenuTimerId);
    }
}

void DockApp::LogInputMouse(UINT message, POINT screenPoint, int icon) const {
    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"HOVERDOCK_LOG", flag, static_cast<DWORD>(std::size(flag))) == 0 ||
        flag[0] == L'0') {
        return;
    }

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

bool DockApp::IsForegroundBorderlessFullscreenAt(POINT cursor) const noexcept {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || IsWindowVisible(foreground) == FALSE ||
        IsIconic(foreground) != FALSE) {
        return false;
    }
    if (foreground == m_window || foreground == m_inputWindow ||
        foreground == m_hoverLabelWindow || foreground == m_dragGhostWindow ||
        foreground == m_launchPromptWindow || foreground == m_overflowWindow ||
        foreground == m_settingsWindow || foreground == m_contextWindow) {
        return false;
    }
    wchar_t className[64]{};
    if (GetClassNameW(foreground, className, static_cast<int>(std::size(className))) > 0) {
        // All dock-owned windows use the LiquidGlassDock* class prefix.
        if (_wcsnicmp(className, L"LiquidGlassDock", 15) == 0) {
            return false;
        }
        // Explorer desktop hosts (Progman / WorkerW / DefView) are popup-sized to
        // the virtual desktop with no caption. Treating them as borderless
        // fullscreen permanently suppressed edge-show after a desktop right-click
        // (FG stays on Progman once the context menu closes).
        if (_wcsicmp(className, L"Progman") == 0 ||
            _wcsicmp(className, L"WorkerW") == 0 ||
            _wcsicmp(className, L"SHELLDLL_DefView") == 0) {
            return false;
        }
    }

    const LONG_PTR style = GetWindowLongPtrW(foreground, GWL_STYLE);
    // Borderless / exclusive fullscreen: no caption chrome and no thick resize
    // frame (typically WS_POPUP). Maximized windowed apps keep WS_CAPTION /
    // WS_THICKFRAME and must still be allowed to edge-show the dock.
    if ((style & WS_CAPTION) != 0 || (style & WS_THICKFRAME) != 0) {
        return false;
    }

    RECT windowRect{};
    if (GetWindowRect(foreground, &windowRect) == FALSE) {
        return false;
    }
    const HMONITOR cursorMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    const HMONITOR windowMonitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    if (cursorMonitor == nullptr || windowMonitor == nullptr || cursorMonitor != windowMonitor) {
        return false;
    }
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(windowMonitor, &info) == FALSE) {
        return false;
    }
    // Allow a couple of pixels of slack for DPI/rounding quirks; require the
    // window to essentially cover the full monitor rect (not just the work area).
    constexpr LONG kTol = 4;
    const RECT& mon = info.rcMonitor;
    return windowRect.left <= mon.left + kTol && windowRect.top <= mon.top + kTol &&
        windowRect.right >= mon.right - kTol && windowRect.bottom >= mon.bottom - kTol;
}

bool DockApp::IsCursorInBottomHotZone(POINT cursor) const noexcept {
    RECT bounds{};
    if (!MonitorRect(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), bounds)) {
        bounds = m_hostBounds;
    }
    return cursor.x >= bounds.left && cursor.x < bounds.right &&
        cursor.y >= bounds.bottom - kBottomHotZonePixels &&
        cursor.y < bounds.bottom;
}

int DockApp::IconAtScreenPoint(POINT cursor) const noexcept {
    const POINT clientCursor = ClientFromDockOrigin(cursor, m_windowX, m_currentY);
    for (size_t index = 0; index < m_iconRenderData.size(); ++index) {
        if (m_iconRenderData[index].kind == DockIconKind::Divider ||
            m_iconRenderData[index].kind == DockIconKind::TrayDivider) {
            continue;
        }
        RECT hit = m_iconRenderData[index].bounds;
        if (m_iconRenderData[index].kind == DockIconKind::Tray ||
            m_iconRenderData[index].kind == DockIconKind::Clock ||
            m_iconRenderData[index].kind == DockIconKind::Weather ||
            m_iconRenderData[index].kind == DockIconKind::Trash) {
            hit.top = 0;
            hit.bottom = static_cast<LONG>(m_dockHeight);
        }
        if (IsInside(hit, clientCursor.x, clientCursor.y)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int DockApp::DividerAtScreenPoint(POINT cursor) const noexcept {
    if (m_dividerIndex < 0 || static_cast<size_t>(m_dividerIndex) >= m_iconRenderData.size()) {
        return -1;
    }

    const POINT clientCursor = ClientFromDockOrigin(cursor, m_windowX, m_currentY);
    const RECT& slotBounds = m_iconRenderData[static_cast<size_t>(m_dividerIndex)].bounds;
    const LONG slotWidth = std::max(1L, slotBounds.right - slotBounds.left);
    const LONG slotPadding = std::max(8L, slotWidth);
    const RECT hitBounds{slotBounds.left - slotPadding, 0, slotBounds.right + slotPadding,
        static_cast<LONG>(m_dockHeight)};
    if (IsInside(hitBounds, clientCursor.x, clientCursor.y)) {
        return m_dividerIndex;
    }
    return -1;
}

int DockApp::InsertionIndexForDrag(POINT cursor) const noexcept {
    if (!IsCursorOverDock(cursor) || m_layoutSlotBounds.size() != m_iconRenderData.size() ||
        m_draggedIcon < 0) {
        return -1;
    }
    POINT clientCursor = ClientFromDockOrigin(cursor, m_windowX, m_currentY);
    constexpr int kFixedIconCount = 2;
    const int appCount = AppSlotCount();
    const int iconCount = m_dividerIndex >= 0 ? m_dividerIndex : appCount;
    int slot = 0;
    for (int displayIndex = 0; displayIndex < iconCount; ++displayIndex) {
        if (displayIndex == m_draggedIcon) {
            continue;
        }
        if (slot >= static_cast<int>(m_layoutSlotBounds.size())) {
            break;
        }
        const RECT& bounds = m_layoutSlotBounds[static_cast<size_t>(slot)];
        const LONG center = bounds.left + (bounds.right - bounds.left) / 2;
        if (displayIndex >= kFixedIconCount && clientCursor.x < center) {
            return displayIndex;
        }
        ++slot;
    }
    return iconCount;
}

bool DockApp::IsCursorOverDock(POINT cursor) const noexcept {
    // Hit-test the glass pill, not the full window: the window carries a
    // transparent shadow ring (DOCK_SHADOW_MARGIN_PT) on every side. Counting
    // that ring as "over dock" forced the cursor a full margin above the
    // visible top before a leave/hide could trigger, and disagreed with the
    // input region (already pill-only). The hook's ShouldPostPointerUpdate
    // uses this too, so pill-testing also makes the leave post immediately.
    const float scale = static_cast<float>(HostDpi()) / 96.0F;
    const LONG inset = DockShadowMarginPx(scale);
    const LONG left = m_windowX + inset;
    const LONG top = m_currentY + inset;
    const LONG right = m_windowX + static_cast<LONG>(m_dockWidth) - inset;
    const LONG bottom = m_currentY + static_cast<LONG>(m_dockHeight) - inset;
    if (right <= left || bottom <= top) {
        return cursor.x >= m_windowX && cursor.x < m_windowX + static_cast<LONG>(m_dockWidth) &&
            cursor.y >= m_currentY && cursor.y < m_currentY + static_cast<LONG>(m_dockHeight);
    }
    return cursor.x >= left && cursor.x < right && cursor.y >= top && cursor.y < bottom;
}

bool DockApp::ShouldPostPointerUpdate(POINT cursor) const noexcept {
    switch (m_visibility) {
    case VisibilityState::Hidden:
    case VisibilityState::Hiding:
        return IsCursorInBottomHotZone(cursor) && !IsForegroundBorderlessFullscreenAt(cursor);
    case VisibilityState::Showing:
    case VisibilityState::Visible:
        return !IsCursorOverDock(cursor);
    default:
        return false;
    }
}

bool DockApp::NeedsHookPointerPost(POINT cursor) const noexcept {
    if (!ShouldPostPointerUpdate(cursor)) {
        return false;
    }
    switch (m_visibility) {
    case VisibilityState::Hidden:
    case VisibilityState::Hiding:
        return true;
    case VisibilityState::Showing:
    case VisibilityState::Visible:
        if (IsCursorOverDock(cursor) || IsCursorInBottomHotZone(cursor)) {
            return true;
        }
        if (IsCursorWithinFlyoutZone(cursor) || IsOverflowOpen() || IsDockSettingsOpen() ||
            IsContextMenuOpen() || IsLaunchPromptOpen()) {
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool DockApp::IsDragActive() const noexcept {
    return m_draggedIcon >= 0 || m_dragSnapAnimating || m_scalingDivider;
}

bool DockApp::HasCrossedDragThreshold(POINT cursor) const noexcept {
    UINT dpi = GetDpiForWindow(m_inputWindow == nullptr ? m_window : m_inputWindow);
    if (dpi == 0) {
        dpi = 96;
    }
    const double threshold = kDragThresholdLogicalPixels * static_cast<double>(dpi) / 96.0;
    const double deltaX = static_cast<double>(cursor.x) - static_cast<double>(m_pressedAt.x);
    const double deltaY = static_cast<double>(cursor.y) - static_cast<double>(m_pressedAt.y);
    return deltaX * deltaX + deltaY * deltaY >= threshold * threshold;
}

bool DockApp::IsPersistentDisplayIcon(int icon) const noexcept {
    return icon >= 0 && static_cast<size_t>(icon) < m_displayApps.size() &&
        !IsSpecialDockTarget(m_displayApps[static_cast<size_t>(icon)].app.target) &&
        !IsLayoutOnlyTarget(m_displayApps[static_cast<size_t>(icon)].app.target) &&
        m_displayApps[static_cast<size_t>(icon)].persistentPinIndex >= 0;
}

LONG DockApp::CurrentY() const noexcept {
    return m_currentY;
}

bool DockApp::IsAnimating() const noexcept {
    return m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Hiding ||
        m_dragSnapAnimating;
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

UINT DockApp::IconPixelExtent() const noexcept {
    const UINT dpi = m_window == nullptr ? 96U : std::max(GetDpiForWindow(m_window), 96U);
    return std::max(1U, static_cast<UINT>(
        std::lround(56.0F * static_cast<float>(dpi) / 96.0F * m_dockScale)));
}

void DockApp::ReloadIconsIfExtentChanged() {
    if (!m_rendererInitialized || IconPixelExtent() == m_loadedIconExtent) {
        return;
    }
    ++m_refreshGeneration;
    LoadIconTextures();
    AssignIconTextureIndices();
    QueueRenderFrame();
}
