#include "DockApp.h"
#include "DockTheme.hlsli"
#include "PerfBoost.h"
#include "Profile.h"
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
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

DockApp* DockApp::s_instance = nullptr;

namespace {

constexpr double kShowDurationSeconds = 0.050;
constexpr double kHideDurationSeconds = 0.050;
constexpr double kDragSnapDurationSeconds = 0.050;
constexpr double kDragThresholdLogicalPixels = 6.0;
constexpr float kMinDockScale = 0.75F;
constexpr float kMaxDockScale = 1.5F;
constexpr int kBottomHotZonePixels = 8;
constexpr UINT kRefreshIntervalMs = 5000;
constexpr BYTE kInputWindowAlpha = 1;
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
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

bool TryExcludeWindowFromCapture(HWND window) {
    return window != nullptr && SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE) != FALSE;
}

HRGN CreateDockInputRegion(int width, int height, int cornerDiameter) {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    // Match the shared DOCK_CORNER_RADIUS_PT used by the glass shader.
    const int rounding = std::max(2, cornerDiameter);
    return CreateRoundRectRgn(0, 0, width + 1, height + 1, rounding, rounding);
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

HFONT CreateFlyoutFont(int pixelHeight, int weight) {
    return CreateFontW(-pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

void CoverageToPremulInk(uint8_t* pixels, size_t byteCount, uint8_t gray) {
    // Popup DIBs are BGR-ordered: byte0=B, byte1=G, byte2=R.
    for (size_t index = 0; index + 3 < byteCount; index += 4) {
        const unsigned coverage =
            (static_cast<unsigned>(pixels[index]) * 19U +
                static_cast<unsigned>(pixels[index + 1]) * 183U +
                static_cast<unsigned>(pixels[index + 2]) * 54U) >>
            8U;
        // The gray level becomes opacity so text hierarchy is preserved.
        const unsigned alpha = (coverage * gray + 127U) / 255U;
        pixels[index] = static_cast<uint8_t>((DOCK_INK_B * alpha + 127U) / 255U);
        pixels[index + 1] = static_cast<uint8_t>((DOCK_INK_G * alpha + 127U) / 255U);
        pixels[index + 2] = static_cast<uint8_t>((DOCK_INK_R * alpha + 127U) / 255U);
        pixels[index + 3] = static_cast<uint8_t>(alpha);
    }
}

void DrawFlyoutText(uint8_t* dest, int destWidth, int destHeight, RECT bounds, HFONT font,
    const std::wstring& text, UINT format, uint8_t gray) {
    if (dest == nullptr || font == nullptr || text.empty()) {
        return;
    }
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
    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    HGDIOBJ previousFont = SelectObject(memory, font);
    std::memset(bits, 0, static_cast<size_t>(width) * height * 4U);
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(255, 255, 255));
    RECT local{0, 0, width, height};
    DrawTextW(memory, text.c_str(), static_cast<int>(text.size()), &local,
        format | DT_NOPREFIX);
    CoverageToPremulInk(static_cast<uint8_t*>(bits),
        static_cast<size_t>(width) * height * 4U, gray);
    CompositePremul(dest, destWidth, destHeight, bounds.left, bounds.top,
        static_cast<uint8_t*>(bits), width, height);
    SelectObject(memory, previousFont);
    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

void FillCirclePremul(uint8_t* dest, int destWidth, int destHeight, float cx, float cy,
    float radius, float alpha) {
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
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
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
    const int left = std::max(0L, bounds.left);
    const int top = std::max(0L, bounds.top);
    const int right = std::min(static_cast<LONG>(destWidth), bounds.right);
    const int bottom = std::min(static_cast<LONG>(destHeight), bounds.bottom);
    const uint8_t channel = static_cast<uint8_t>(std::lround(255.0F * alpha));
    uint8_t pixel[4] = {channel, channel, channel, channel};
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            CompositePremul(dest, destWidth, destHeight, x, y, pixel, 1, 1);
        }
    }
}

// Amber accent shared with the dock's running-indicator dot
// (Shaders.hlsl runningDot = float3(1.0, 191/255, 0.0)). DIB order: B, G, R.
constexpr uint8_t kAmberB = 0;
constexpr uint8_t kAmberG = 191;
constexpr uint8_t kAmberR = 255;

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
    const size_t cut = icon.tip.find_first_of(L"\r\n");
    if (cut != std::wstring::npos && cut + 1 < icon.tip.size()) {
        size_t start = cut + 1;
        if (start < icon.tip.size() && icon.tip[start] == L'\n') {
            ++start;
        }
        return icon.tip.substr(start);
    }
    const size_t dash = icon.tip.find(L" - ");
    if (dash != std::wstring::npos && dash + 3 < icon.tip.size()) {
        return icon.tip.substr(dash + 3);
    }
    return {};
}

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

}  // namespace

DockApp::DockApp(HINSTANCE instance)
    : m_instance(instance) {
    m_comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
}

DockApp::~DockApp() {
    CloseLaunchPrompt(false);
    DestroyDockSettings();
    DestroyOverflowPopup();
    DestroyHoverLabelWindow();
    DestroyHoverLabelFont();
    DestroyDragGhostWindow();
    if (m_mouseHook != nullptr) {
        UnhookWindowsHookEx(m_mouseHook);
    }
    UnregisterSystemResumeNotifications();
    RestoreTaskbar();
    if (m_singleInstanceMutex != nullptr) {
        CloseHandle(m_singleInstanceMutex);
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
    if (m_comResult == S_OK) {
        CoUninitialize();
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
    // The Run key is authoritative for startup, but the toggle owns it: repair
    // a stale path (portable copy moved) or clear a leftover entry so the
    // persisted setting and the registry never disagree after restart.
    Startup::SyncWithConfig(m_config.LaunchAtStartup());
    {
        std::string current = Updater::CurrentVersion();
        std::wstring wide(current.begin(), current.end());
        m_updateStatus = m_config.CheckForUpdates()
            ? L"Version " + wide + L" — checking for updates..."
            : L"Version " + wide + L" — automatic updates off.";
    }
    RestoreTaskbar();
    CreateOverlayWindow();
    UpdatePrimaryMonitor();
    // Profiles first: enrichment (AppUserModelId coverage) depends on knowing
    // which pins need it before windows are enriched.
    m_windows.RebuildPinProfiles(m_config.Pins());
    static_cast<void>(m_windows.Refresh());
    static_cast<void>(m_tray.Refresh());
    RebuildDisplayApps();
    RebuildLayout(false);

    try {
        m_renderer.Initialize(m_window, m_dockWidth, m_dockHeight);
        m_rendererInitialized = true;
        LoadIconTextures();
        AssignIconTextureIndices();
    } catch (...) {
        DestroyWindow(m_window);
        throw;
    }

    m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    RegisterSystemResumeNotifications();
    HideTaskbar();
    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &DockApp::MouseHook, m_instance, 0);
    if (m_mouseHook == nullptr) {
        Log(L"Low-level mouse hook unavailable; the dock can still be shown by moving over its window.");
    }

    GetCursorPos(&m_lastCursor);
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
        const DWORD count = frameWaitable == nullptr ? 0 : 1;
        const DWORD timeout = (slideAnimating || m_dragSnapAnimating) ? 16 : INFINITE;
        const DWORD wait = MsgWaitForMultipleObjectsEx(count, &frameWaitable, timeout, QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);

        if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) {
            if (m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Hiding) {
                AdvanceAnimation();
            }
            if (m_dragSnapAnimating) {
                AdvanceDragSnapBack();
            }
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
        SetTextColor(context, RGB(245, 245, 247));
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

    case WM_MOUSEMOVE: {
        if (app == nullptr) {
            break;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int hover = app->OverflowHitIndex(point);
        if (hover != app->m_overflowHover) {
            app->m_overflowHover = hover;
            app->QueueOverflowPaint();
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
        const int hit = app->OverflowHitIndex(point);
        if (hit >= 0 && static_cast<size_t>(hit) < app->m_overflowHits.size()) {
            app->HandleOverflowClick(app->m_overflowHits[static_cast<size_t>(hit)], message);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (app != nullptr && app->m_overflowHover >= 0) {
            app->m_overflowHover = -1;
            app->QueueOverflowPaint();
        }
        return 0;

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

LRESULT CALLBACK DockApp::MouseHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && s_instance != nullptr && s_instance->m_window != nullptr) {
        const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (wParam == WM_MOUSEMOVE && !s_instance->IsDragActive() &&
            s_instance->ShouldPostPointerUpdate(mouse->pt)) {
            static POINT s_lastPostedCursor{std::numeric_limits<LONG>::min(),
                std::numeric_limits<LONG>::min()};
            if (mouse->pt.x != s_lastPostedCursor.x || mouse->pt.y != s_lastPostedCursor.y) {
                s_lastPostedCursor = mouse->pt;
                PostMessageW(s_instance->m_window, kPointerMessage,
                    static_cast<WPARAM>(static_cast<INT_PTR>(mouse->pt.x)),
                    static_cast<LPARAM>(mouse->pt.y));
            }
        } else if (wParam == WM_LBUTTONDOWN && s_instance->IsOverflowOpen() &&
            !s_instance->IsCursorOverOverflow(mouse->pt) &&
            !s_instance->IsCursorOverSettings(mouse->pt) &&
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
                            TrayFlyoutHitKind::Brightness)) {
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
            if (IsDockSettingsOpen()) {
                CloseDockSettings();
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

    case kPointerMessage: {
        POINT point{static_cast<LONG>(static_cast<INT_PTR>(wParam)),
            static_cast<LONG>(lParam)};
        point = CoalescePostedPointers(window, kPointerMessage, point);
        if (IsDragActive() && HasPostedMessage(m_inputWindow, WM_LBUTTONUP)) {
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

    case kRefreshApplyMessage:
        ApplyBackgroundRefresh(static_cast<UINT>(wParam));
        return 0;

    case kPinIconMessage:
        ApplyPinIcons(static_cast<UINT>(wParam));
        return 0;

    case kOverflowPaintMessage:
        m_overflowPaintQueued = false;
        // Guarded: a paint queued before the popup closed must not re-show it.
        if (IsOverflowOpen()) {
            PaintOverflowPopup();
        }
        return 0;

    case kSettingsPaintMessage:
        m_settingsPaintQueued = false;
        // Guarded: a paint queued before the panel closed must not re-show it.
        if (IsDockSettingsOpen()) {
            PaintSettingsPopup();
        }
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
        if (kind != TrayFlyoutHitKind::Sound && kind != TrayFlyoutHitKind::Brightness) {
            return 0;
        }
        if (kind == TrayFlyoutHitKind::Sound) {
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
                PaintOverflowPopup();
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
            if (!reply->targets.empty()) {
                m_launchTargets = std::move(reply->targets);
            }
            ApplyLaunchJudgment(reply->generation, reply->judgment);
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
            if (m_visibility == VisibilityState::Visible) {
                RefreshRunningWindows(true);
            }
        } else if (wParam == kConfigSaveTimerId) {
            KillTimer(window, kConfigSaveTimerId);
            static_cast<void>(m_config.Save());
        } else if (wParam == kBackdropTimerId) {
            // Paused while Quick Settings is open: the 8ms capture + upload budget
            // stays available for input and hover paints instead. The blur refreshes
            // on the next tick after the popup closes.
            if (m_visibility == VisibilityState::Visible && !IsDragActive() && !IsOverflowOpen() &&
                QpcSeconds() >= m_suppressBackdropUntil && CaptureLiveBackdrop()) {
                QueueRenderFrame(false);
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
            // Watchdog for the low-level mouse hook: the system can silently
            // drop LL hooks (e.g. after a timeout), which would otherwise leave
            // the dock permanently hidden with no show/hide updates. Sampling
            // the cursor here keeps the same HandlePointer rules working.
            // Skipped while dragging, mirroring the hook's own gating.
            if (!IsDragActive()) {
                POINT cursor{};
                if (GetCursorPos(&cursor) != FALSE) {
                    HandlePointer(cursor);
                }
            }
        } else if (wParam == kTrayTimerId) {
            RefreshTray(false);
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
        StopTaskbarMonitor();
        StopUpdateTimer();
        UnregisterSystemResumeNotifications();
        StopShellFlyoutWatch();
        CloseLaunchPrompt(false);
        DestroyDockSettings();
        CloseOverflowPopup();
        DestroyOverflowPopup();
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
        if (m_pressedIcon >= 0 && IsPersistentDisplayIcon(m_pressedIcon) &&
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
        const bool dragging = m_draggedIcon >= 0;
        if (m_scalingDivider) {
            ScheduleConfigSave();
            ReloadIconsIfExtentChanged();
            if (m_visibility == VisibilityState::Visible || m_visibility == VisibilityState::Showing) {
                static_cast<void>(CaptureLiveBackdrop());
            }
            QueueRenderFrame();
        } else if (dragging) {
            FinishDrag(point);
        } else {
            ActivatePressedApp();
        }
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        if (!m_dragSnapAnimating) {
            ClearPressState();
        }
        if (!dragging) {
            RenderFrame();
        }
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
    m_backdropCaptureRequiresHide = !TryExcludeWindowFromCapture(m_window);
    TryExcludeWindowFromCapture(m_inputWindow);
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
    }
}

HFONT DockApp::HoverLabelFont() {
    const UINT dpi = GetDpiForWindow(m_window);
    if (m_hoverLabelFont != nullptr && dpi == m_hoverLabelFontDpi) {
        return m_hoverLabelFont;
    }

    DestroyHoverLabelFont();
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) == FALSE) {
        return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }

    m_hoverLabelFont = CreateFontIndirectW(&metrics.lfMessageFont);
    m_hoverLabelFontDpi = dpi;
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
    const LONG dotSize = std::max(2L, std::lround(5.0F * layoutScale));
    const LONG dotGap = std::max(1L, std::lround(2.0F * layoutScale));
    const LONG iconSlotHeight = iconSize + dotGap + dotSize;
    const LONG padding = std::lround(20.0F * layoutScale);
    const LONG gap = std::lround(10.0F * layoutScale);
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
        std::max(clockSize.cx, std::lround(72.0F * layoutScale)));
    const LONG clockHeight = std::min(atlasExtent,
        std::min(iconSlotHeight, std::max(clockSize.cy, std::lround(36.0F * layoutScale))));
    const size_t displayCount = m_displayApps.size();
    constexpr TraySlot kTrayGlyphSlots[] = {
        TraySlot::Overflow, TraySlot::Power};

    LONG trayWidth = trayDividerWidth + trayLeadGap;
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
    trayWidth += trayClockGap + clockWidth;

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

    m_dockWidth = static_cast<UINT>(padding * 2 + contentWidth);
    m_dockHeight = static_cast<UINT>(padding * 2 + iconSlotHeight);
    m_visibleY = m_hostBounds.bottom - static_cast<LONG>(m_dockHeight);
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
    m_iconRenderData.reserve(displayCount + 8U);
    const LONG top = (static_cast<LONG>(m_dockHeight) - iconSlotHeight) / 2;
    LONG left = padding;
    for (size_t index = 0; index < displayCount; ++index) {
        DockIconRenderData data;
        if (IsLayoutOnlyTarget(m_displayApps[index].app.target)) {
            data.bounds = {left, top, left + dividerSlotWidth, top + iconSize};
            data.kind = DockIconKind::Divider;
            left += dividerSlotWidth + gap;
        } else {
            data.bounds = {left, top, left + iconSize, top + iconSlotHeight};
            data.running = m_displayApps[index].runningWindow != nullptr;
            left += iconSize + gap;
        }
        m_iconRenderData.push_back(data);
    }

    DockIconRenderData trayDivider;
    trayDivider.kind = DockIconKind::TrayDivider;
    trayDivider.bounds = {left, top, left + trayDividerWidth, top + iconSize};
    m_iconRenderData.push_back(trayDivider);
    left += trayDividerWidth + trayLeadGap;

    const LONG trayTop = top + (iconSize - trayGlyph) / 2;
    for (const TraySlot slot : kTrayGlyphSlots) {
        if (!SystemTray::SlotVisible(slot, m_tray.Status())) {
            continue;
        }
        DockIconRenderData data;
        data.kind = DockIconKind::Tray;
        data.traySlot = slot;
        data.bounds = {left, trayTop, left + trayGlyph, trayTop + trayGlyph};
        m_iconRenderData.push_back(data);
        left += trayGlyph + trayGap;
    }
    left += trayClockGap - trayGap;
    const LONG clockTop = top + (iconSlotHeight - clockHeight) / 2;
    DockIconRenderData clock;
    clock.kind = DockIconKind::Clock;
    clock.traySlot = TraySlot::Clock;
    clock.bounds = {left, clockTop, left + clockWidth, clockTop + clockHeight};
    m_iconRenderData.push_back(clock);

    CacheLayoutSlotBounds();

    const bool sizeChanged = previousWidth != m_dockWidth || previousHeight != m_dockHeight;
    m_hoverLabelIcon = -1;
    if (sizeChanged) {
        UpdateInputRegion();
        PositionOverlayWindows();
        if (m_rendererInitialized) {
            m_renderer.Resize(m_dockWidth, m_dockHeight);
        }
    }
    if (reloadIcons && m_rendererInitialized) {
        LoadIconTextures();
    }
    AssignIconTextureIndices();
    if (m_rendererInitialized) {
        EnsureTrayIcons();
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
    const float scale = static_cast<float>(HostDpi()) / 96.0F;
    HRGN region = CreateDockInputRegion(width, height,
        static_cast<int>(std::lround(2.0F * DOCK_CORNER_RADIUS_PT * scale)));
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
    ProfileScope scope("PositionOverlayWindows");
    const UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (SetWindowPos(m_window, HWND_TOPMOST, m_windowX, m_currentY,
            static_cast<int>(m_dockWidth), static_cast<int>(m_dockHeight), flags) == FALSE) {
        Log(L"Could not position the renderer window.");
    }
    if (m_inputWindow != nullptr && SetWindowPos(m_inputWindow, m_window, m_windowX, m_currentY,
            static_cast<int>(m_dockWidth), static_cast<int>(m_dockHeight), flags) == FALSE) {
        Log(L"Could not position the dock input window.");
    }
    if (IsDragActive()) {
        BringDragGhostToFront();
    } else {
        UpdateHoverLabel();
    }
    PositionLaunchPrompt();
    PositionOverflowPopup();
    PositionDockSettings();
}

void DockApp::UpdateHoverLabel() {
    if (IsDragActive() || m_draggedIcon >= 0 || m_hoveredDivider >= 0 || IsOverflowOpen()) {
        HideHoverLabel();
        return;
    }

    if (m_hoverLabelWindow == nullptr || m_visibility != VisibilityState::Visible ||
        m_hoveredIcon < 0 || static_cast<size_t>(m_hoveredIcon) >= m_iconRenderData.size()) {
        HideHoverLabel();
        return;
    }

    if (m_hoveredIcon == m_hoverLabelIcon) {
        return;
    }

    std::wstring text;
    if (IsTrayRenderIndex(m_hoveredIcon)) {
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

    POINT iconTopLeft{m_iconRenderData[static_cast<size_t>(m_hoveredIcon)].bounds.left,
        m_iconRenderData[static_cast<size_t>(m_hoveredIcon)].bounds.top};
    POINT iconBottomRight{m_iconRenderData[static_cast<size_t>(m_hoveredIcon)].bounds.right,
        m_iconRenderData[static_cast<size_t>(m_hoveredIcon)].bounds.bottom};
    if (ClientToScreen(m_window, &iconTopLeft) == FALSE ||
        ClientToScreen(m_window, &iconBottomRight) == FALSE) {
        HideHoverLabel();
        return;
    }

    HFONT font = HoverLabelFont();
    HGDIOBJ fontObject = font;
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        HideHoverLabel();
        return;
    }
    HDC memory = CreateCompatibleDC(screen);
    if (memory == nullptr) {
        ReleaseDC(nullptr, screen);
        HideHoverLabel();
        return;
    }
    HGDIOBJ previousFont = SelectObject(memory, fontObject);
    if (previousFont == nullptr || previousFont == HGDI_ERROR) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        HideHoverLabel();
        return;
    }

    SIZE textSize{};
    const int textLength = static_cast<int>(text.size());
    if (GetTextExtentPoint32W(memory, text.c_str(), textLength, &textSize) == FALSE) {
        SelectObject(memory, previousFont);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        HideHoverLabel();
        return;
    }

    const UINT dpi = GetDpiForWindow(m_window);
    const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
    const LONG horizontalPadding = GreaterOf(8L, static_cast<LONG>(std::lround(12.0F * scale)));
    const LONG verticalPadding = GreaterOf(5L, static_cast<LONG>(std::lround(6.0F * scale)));
    const LONG triangleWidth = GreaterOf(10L, static_cast<LONG>(std::lround(12.0F * scale)));
    const LONG triangleHeight = GreaterOf(6L, static_cast<LONG>(std::lround(7.0F * scale)));
    const LONG cornerRadius = GreaterOf(5L, static_cast<LONG>(std::lround(7.0F * scale)));
    const LONG gap = GreaterOf(2L, static_cast<LONG>(std::lround(4.0F * scale)));
    const LONG bubbleWidth = GreaterOf(60L, textSize.cx + horizontalPadding * 2L);
    const LONG bubbleHeight = GreaterOf(24L, textSize.cy + verticalPadding * 2L);
    SIZE labelSize{bubbleWidth, bubbleHeight + triangleHeight};

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
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (bitmap == nullptr || bits == nullptr) {
        SelectObject(memory, previousFont);
        DeleteDC(memory);
        HideHoverLabel();
        return;
    }
    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        SelectObject(memory, previousFont);
        DeleteDC(memory);
        HideHoverLabel();
        return;
    }

    const size_t pixelCount = static_cast<size_t>(labelSize.cx) * static_cast<size_t>(labelSize.cy);
    std::memset(bits, 0, pixelCount * sizeof(DWORD));
    HBRUSH bubbleBrush = CreateSolidBrush(RGB(42, 42, 46));
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(112, 112, 120));
    if (bubbleBrush == nullptr || borderPen == nullptr) {
        if (bubbleBrush != nullptr) {
            DeleteObject(bubbleBrush);
        }
        if (borderPen != nullptr) {
            DeleteObject(borderPen);
        }
        SelectObject(memory, previousBitmap);
        DeleteObject(bitmap);
        SelectObject(memory, previousFont);
        DeleteDC(memory);
        HideHoverLabel();
        return;
    }
    HGDIOBJ previousBrush = SelectObject(memory, bubbleBrush);
    HGDIOBJ previousPen = SelectObject(memory, borderPen);
    if (previousBrush == nullptr || previousBrush == HGDI_ERROR || previousPen == nullptr ||
        previousPen == HGDI_ERROR) {
        if (previousBrush != nullptr && previousBrush != HGDI_ERROR) {
            SelectObject(memory, previousBrush);
        }
        if (previousPen != nullptr && previousPen != HGDI_ERROR) {
            SelectObject(memory, previousPen);
        }
        DeleteObject(borderPen);
        DeleteObject(bubbleBrush);
        SelectObject(memory, previousBitmap);
        DeleteObject(bitmap);
        SelectObject(memory, previousFont);
        DeleteDC(memory);
        HideHoverLabel();
        return;
    }

    RoundRect(memory, 0, 0, SaturatedInt(bubbleWidth), SaturatedInt(bubbleHeight),
        SaturatedInt(cornerRadius * 2L), SaturatedInt(cornerRadius * 2L));
    const LONG center = bubbleWidth / 2L;
    POINT triangle[3] = {
        {center - triangleWidth / 2L, bubbleHeight - 1L},
        {center + triangleWidth / 2L, bubbleHeight - 1L},
        {center, bubbleHeight + triangleHeight - 1L},
    };
    Polygon(memory, triangle, SaturatedInt(static_cast<LONG>(std::size(triangle))));
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(245, 245, 247));
    RECT textBounds{horizontalPadding, 0L, bubbleWidth - horizontalPadding, bubbleHeight};
    DrawTextW(memory, text.c_str(), textLength, &textBounds,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    DWORD* pixels = static_cast<DWORD*>(bits);
    for (size_t index = 0; index < pixelCount; ++index) {
        if ((pixels[index] & 0x00ffffffU) != 0) {
            pixels[index] |= 0xff000000U;
        }
    }

    POINT destination{iconTopLeft.x + (iconBottomRight.x - iconTopLeft.x) / 2L -
            labelSize.cx / 2L,
        iconTopLeft.y - labelSize.cy - gap};
    POINT source{0L, 0L};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL updated = UpdateLayeredWindow(m_hoverLabelWindow, nullptr, &destination, &labelSize,
        memory, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memory, previousPen);
    SelectObject(memory, previousBrush);
    DeleteObject(borderPen);
    DeleteObject(bubbleBrush);
    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    SelectObject(memory, previousFont);
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

void DockApp::HideHoverLabel() noexcept {
    m_hoverLabelIcon = -1;
    if (m_hoverLabelWindow != nullptr) {
        ShowWindow(m_hoverLabelWindow, SW_HIDE);
    }
}

void DockApp::LoadIconTextures() {
    // A full reload supersedes any in-flight incremental pin-icon work, which
    // was extracted for a possibly different extent.
    ++m_pinIconGeneration;
    {
        const std::lock_guard lock(m_pinIconMutex);
        m_pinIconPending.reset();
    }
    std::vector<std::wstring> cacheKeys;
    std::vector<std::vector<std::wstring>> iconCandidates;
    cacheKeys.reserve(m_displayApps.size());
    iconCandidates.reserve(m_displayApps.size());
    for (const DisplayApp& app : m_displayApps) {
        if (IsLayoutOnlyTarget(app.app.target)) {
            continue;
        }
        cacheKeys.push_back(WindowCatalog::IconCacheKey(app.app));
        iconCandidates.push_back(
            WindowCatalog::IconResolutionCandidates(app.app, app.runningWindow));
    }
    m_loadedIconExtent = IconPixelExtent();
    m_renderer.LoadIcons(cacheKeys, iconCandidates, m_loadedIconExtent);
    m_trayVisualKey.clear();
    EnsureTrayIcons();
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
    std::wstring key = std::to_wstring(atlas) + L"|" + std::to_wstring(clockWidth) + L"x" +
        std::to_wstring(clockHeight) + L"|" + status.timeText + L"|" + status.dateText + L"|" +
        (status.hasBattery ? L"1" : L"0") + L"|" + (status.batteryCharging ? L"1" : L"0") + L"|" +
        std::to_wstring(status.batteryPercent);
    if (key == m_trayVisualKey && m_renderer.HasIconForTarget(
            SystemTray::TargetForSlot(TraySlot::Clock))) {
        AssignIconTextureIndices();
        return;
    }
    m_trayVisualKey = std::move(key);

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
    try {
        m_renderer.UpdateCachedIcons(targets, pixels);
    } catch (const std::exception&) {
        Log(L"Tray icon upload failed.");
    }
    AssignIconTextureIndices();
}

void DockApp::RefreshTray(bool forceLayout) {
    if (!m_tray.Refresh() && !forceLayout) {
        return;
    }
    RebuildLayout(false);
    if (IsOverflowOpen()) {
        PaintOverflowPopup();
    }
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
            PaintOverflowPopup();
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
        CloseOverflowPopup();
        return;
    }
    // The hover label would otherwise linger under the popup; dismiss it.
    HideHoverLabel();
    // Refresh the DDC level off-thread; repaints on arrival when it moved.
    RefreshBrightnessAsync();
    RebuildOverflowPopup();
    if (m_overflowWindow != nullptr) {
        ShowWindow(m_overflowWindow, SW_SHOWNA);
        PositionOverflowPopup();
    }
}

void DockApp::QueueOverflowPaint() {
    // Coalesce rapid hover transitions into a single repaint so fast cursor
    // movement can't stack full synchronous paints on this thread (which also
    // services the low-level mouse hook). The latest m_overflowHover wins.
    if (m_overflowPaintQueued || m_overflowWindow == nullptr) {
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
    PaintOverflowPopup();

    const std::wstring apiKey = m_config.TypeSafeApiKey();
    const HWND replyWindow = m_window;
    std::thread([this, apiKey, replyWindow]() {
        auto postStatus = [replyWindow](std::wstring status, bool finished) {
            auto* reply = new BoostReply{std::move(status), finished};
            if (replyWindow == nullptr ||
                PostMessageW(replyWindow, kBoostResultMessage, 0,
                    reinterpret_cast<LPARAM>(reply)) == FALSE) {
                delete reply;
            }
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
            const std::vector<BoostProcess> candidates = PerfBoost::EnumerateClosableCandidates();
        if (candidates.empty()) {
            postStatus(L"All clear", true);
            return;
        }

        std::vector<BoostProcess> selected;
        bool allowTerminate = false;
        if (apiKey.empty()) {
            selected = selectHeuristic(candidates);
            if (selected.empty()) {
                postStatus(L"Need API key", true);
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
            const BoostResult judged = TypeSafeClient::ClassifyForBoost(apiKey, inputs);
            if (!judged.ok) {
                Log(L"Boost judgment failed: " + judged.error);
                selected = selectHeuristic(candidates);
                if (selected.empty()) {
                    // Full error is in the log above; the tile only fits key info.
                    postStatus(L"Unavailable", true);
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
                    postStatus(L"All clear", true);
                    return;
                }
            }
        }

        postStatus(L"Closing...", false);
        // Freshness is re-checked inside: the foreground and the process set
        // may have changed during the Jev round-trip.
        const BoostCloseResult closed =
            PerfBoost::CloseTargets(selected, allowTerminate);
        if (closed.closed <= 0) {
            postStatus(L"All clear", true);
            return;
        }
        // Full detail goes to the log; the tile only fits the freed total.
        Log(L"Boost closed " + std::to_wstring(closed.closed) + L", freed " +
            PerfBoost::FormatMegabytes(closed.freedBytes));
        postStatus(L"Freed " + PerfBoost::FormatMegabytes(closed.freedBytes), true);
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
        PaintOverflowPopup();
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
    PaintOverflowPopup();
    if (!m_brightnessAdjustInFlight.exchange(true)) {
        std::thread([this] { DrainBrightnessWheel(); }).detach();
    }
}

void DockApp::EnsureOverflowGlyphs(UINT gearExtent, UINT tileExtent, UINT notifyExtent) {
    const TrayStatus& status = m_tray.Status();
    std::wstring key = std::to_wstring(gearExtent) + L"|" + std::to_wstring(tileExtent) + L"|" +
        std::to_wstring(notifyExtent) + L"|" + std::to_wstring(static_cast<int>(status.network)) +
        L"|" + std::to_wstring(status.wifiBars) + L"|" + (status.volumeMuted ? L"1" : L"0") + L"|" +
        std::to_wstring(static_cast<int>(std::lround(status.volumeLevel * 100.0F)));
    if (key == m_overflowGlyphKey) {
        return;
    }
    m_overflowGlyphKey = std::move(key);
    m_overflowGlyphGear = m_tray.RasterizeSymbol(L'\uE713', gearExtent);
    m_overflowGlyphWifi = m_tray.RasterizeGlyph(TraySlot::Network, tileExtent);
    m_overflowGlyphSound = m_tray.RasterizeGlyph(TraySlot::Volume, tileExtent);
    m_overflowGlyphBrightness = m_tray.RasterizeSymbol(L'\uE706', tileExtent);
    m_overflowGlyphBell = m_tray.RasterizeSymbol(L'\uE91C', notifyExtent);
    if (m_overflowGlyphBell.empty()) {
        m_overflowGlyphBell = m_tray.RasterizeSymbol(L'\uE7E7', notifyExtent);
    }
    // E945 is the Segoe MDL2 Assets lightning bolt: the Boost mark.
    m_overflowGlyphBoost = m_tray.RasterizeSymbol(L'\uE945', tileExtent);
}

void DockApp::CloseOverflowPopup() noexcept {
    // The settings panel is attached to Quick Settings: dismiss both together.
    CloseDockSettings();
    if (m_overflowWindow != nullptr) {
        ShowWindow(m_overflowWindow, SW_HIDE);
    }
    m_overflowHover = -1;
    InvalidateOverflowGlass();
}

void DockApp::DestroyOverflowPopup() noexcept {
    CloseOverflowPopup();
    m_overflowPaintQueued = false;
    m_overflowIcons.clear();
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
        PaintSettingsPopup();
    }
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
    const HWND replyWindow = m_window;
    std::thread([this, replyWindow, manual] {
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

        // An update is available: automatically download and stage the install.
        // The UI thread performs the final launch + exit so file locks are clean.
        std::string version = release.version;
        std::wstring wideVersion(version.begin(), version.end());
        {
            UpdateReply downloading;
            downloading.finished = false;
            downloading.status = L"Downloading update v" + wideVersion + L"...";
            postReply(downloading);
        }

        std::string url = !release.setupUrl.empty() ? release.setupUrl : release.exeUrl;
        const bool isSetup = !release.setupUrl.empty();
        std::wstring dest = Updater::DefaultDownloadPath(version, isSetup);
        if (!Updater::DownloadFile(url, dest, error)) {
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
        bool launched = false;
        if (reply.isSetup) {
            launched = Updater::LaunchInstallerAndExit(reply.path);
        } else {
            launched = Updater::StagePortableUpdateAndRestart(reply.path);
        }
        if (launched) {
            std::wstring wide(reply.version.begin(), reply.version.end());
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
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE: {
        if (app == nullptr) {
            break;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int hover = app->SettingsHitIndex(point);
        if (hover != app->m_settingsHover) {
            app->m_settingsHover = hover;
            app->QueueSettingsPaint();
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
        const int hit = app->SettingsHitIndex(point);
        if (hit >= 0 && static_cast<size_t>(hit) < app->m_settingsHits.size()) {
            app->HandleSettingsClick(app->m_settingsHits[static_cast<size_t>(hit)], message);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (app != nullptr && app->m_settingsHover >= 0) {
            app->m_settingsHover = -1;
            app->QueueSettingsPaint();
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void DockApp::RefreshSettingsControls() {
    // State lives in m_config / m_updateStatus; the glass panel reads it at
    // paint time, so a refresh is just a repaint when open.
    if (IsDockSettingsOpen()) {
        PaintSettingsPopup();
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
}

bool DockApp::SettingsGlassValid(POINT origin) const noexcept {
    return !m_settingsGlass.empty() && m_settingsGlassSize.cx == m_settingsSize.cx &&
        m_settingsGlassSize.cy == m_settingsSize.cy && m_settingsGlassOrigin.x == origin.x &&
        m_settingsGlassOrigin.y == origin.y &&
        m_settingsGlass.size() ==
            static_cast<size_t>(m_settingsSize.cx) * static_cast<size_t>(m_settingsSize.cy) * 4U;
}

void DockApp::QueueSettingsPaint() {
    // Coalesce rapid hover transitions into a single repaint, mirroring the
    // Quick Settings popup path.
    if (m_settingsPaintQueued || m_settingsWindow == nullptr) {
        return;
    }
    m_settingsPaintQueued = true;
    PostMessageW(m_window, kSettingsPaintMessage, 0, 0);
}

void DockApp::RebuildSettingsPopup() {
    m_settingsHover = -1;
    InvalidateSettingsGlass();
    PaintSettingsPopup();
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
        PaintSettingsPopup();
        break;
    }
    case SettingsHitKind::Updates: {
        const bool enabled = !m_config.CheckForUpdates();
        m_config.SetCheckForUpdates(enabled);
        ScheduleConfigSave();
        if (!enabled) {
            std::string current = Updater::CurrentVersion();
            std::wstring wide(current.begin(), current.end());
            SetUpdateStatus(L"Version " + wide + L" — automatic updates off.");
        } else {
            PaintSettingsPopup();
            if (!m_updateInFlight.load()) {
                CheckForUpdatesAsync(false);
            }
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
    if (m_settingsWindow != nullptr) {
        ShowWindow(m_settingsWindow, SW_SHOWNA);
        PositionDockSettings();
    }
}

void DockApp::ToggleDockSettings() {
    if (IsDockSettingsOpen()) {
        CloseDockSettings();
        return;
    }
    OpenDockSettings();
}

void DockApp::CloseDockSettings() noexcept {
    m_settingsHover = -1;
    if (m_settingsWindow != nullptr) {
        ShowWindow(m_settingsWindow, SW_HIDE);
    }
    InvalidateSettingsGlass();
}

void DockApp::DestroyDockSettings() noexcept {
    CloseDockSettings();
    m_settingsPaintQueued = false;
    m_settingsHits.clear();
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
    const LONG rowHeight = std::max(44L, std::lround(50.0F * scale));
    const LONG rowGap = std::max(6L, std::lround(8.0F * scale));
    const LONG buttonHeight = std::max(34L, std::lround(38.0F * scale));
    const LONG dividerGap = std::max(10L, std::lround(12.0F * scale));
    const LONG statusHeight = std::max(40L, std::lround(46.0F * scale));
    const LONG switchWidth = std::max(40L, std::lround(46.0F * scale));
    const LONG switchHeight = std::max(22L, std::lround(24.0F * scale));
    const LONG panelWidth = std::max(280L, std::lround(308.0F * scale));

    LONG contentY = padding;
    contentY += headerHeight;
    contentY += dividerGap;
    contentY += rowHeight;
    contentY += rowGap;
    contentY += rowHeight;
    contentY += rowGap;
    contentY += buttonHeight;
    contentY += dividerGap;
    contentY += statusHeight;
    contentY += padding;
    m_settingsSize.cx = panelWidth;
    m_settingsSize.cy = contentY;

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
            WS_EX_TOPMOST;
        m_settingsWindow = CreateWindowExW(extendedStyle, className, L"", WS_POPUP, 0, 0, 1, 1,
            m_window, nullptr, m_instance, this);
        if (m_settingsWindow == nullptr) {
            Log(L"Could not create the dock settings window.");
            return;
        }
        TryExcludeWindowFromCapture(m_settingsWindow);
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
        // Same frosted recipe as Quick Settings: stacked box blurs approximate
        // the dock shader's wide kernel at the same texel scale.
        const int frostRadius = std::max(4, static_cast<int>(std::lround(12.0F * scale)));
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
            const float tint = DOCK_GLASS_TINT * 255.0F;
            const float mix = DOCK_GLASS_MIX;
            const float alpha = DOCK_GLASS_ALPHA;
            constexpr float kChannelK[3] = {0.99F, 0.985F, 0.98F};
            for (LONG y = 0; y < glassH; ++y) {
                for (LONG x = 0; x < glassW; ++x) {
                    const size_t flat = static_cast<size_t>(y) * glassW + x;
                    const float shape = coverage[flat];
                    const float rim = std::clamp((blurredCoverage[flat] - shape) * 2.0F, 0.0F, 1.0F);
                    const float rim4 = rim * rim * rim * rim;
                    float noise = static_cast<float>(x) * 0.06711056F +
                        static_cast<float>(y) * 0.00583715F;
                    noise = noise - std::floor(noise);
                    noise = 52.9829189F * noise;
                    noise = (noise - std::floor(noise)) - 0.5F;
                    uint8_t* pixel = pixels + flat * 4U;
                    for (int channel = 0; channel < 3; ++channel) {
                        const float frosted = static_cast<float>(pixel[channel]);
                        const float base = frosted * (1.0F - mix) + tint * mix;
                        const float target = base * kChannelK[channel] + tint * 0.08F;
                        float shaded = base + 0.22F * (target - base);
                        shaded += tint * rim * 0.12F + 255.0F * rim4 * 0.18F;
                        shaded = std::clamp(shaded + noise, 0.0F, 255.0F);
                        pixel[channel] =
                            static_cast<uint8_t>(std::lround(shaded * shape * alpha));
                    }
                    pixel[3] = static_cast<uint8_t>(std::lround(255.0F * shape * alpha));
                }
            }
            SelectObject(maskDc, previousMask);
            DeleteObject(maskBitmap);
            DeleteDC(maskDc);
        }

        m_settingsGlass.assign(pixels, pixels + pixelCount * 4U);
        m_settingsGlassSize = m_settingsSize;
        m_settingsGlassOrigin = origin;
    }

    const int width = SaturatedInt(m_settingsSize.cx);
    const int height = SaturatedInt(m_settingsSize.cy);
    EnsureOverflowFonts(scale);
    HFONT titleFont = m_overflowTitleFont;
    HFONT labelFont = m_overflowLabelFont;
    HFONT statusFont = m_overflowStatusFont;
    SettingsHit hoveredHit{};
    const bool hasHover =
        m_settingsHover >= 0 && static_cast<size_t>(m_settingsHover) < m_settingsHits.size();
    if (hasHover) {
        hoveredHit = m_settingsHits[static_cast<size_t>(m_settingsHover)];
    }
    m_settingsHits.clear();

    auto pushHit = [this](SettingsHitKind kind, RECT bounds) {
        SettingsHit hit;
        hit.kind = kind;
        hit.bounds = bounds;
        m_settingsHits.push_back(hit);
    };
    auto hoveredKind = [&hoveredHit, hasHover](SettingsHitKind kind) {
        return hasHover && hoveredHit.kind == kind;
    };
    auto drawSwitch = [&](LONG centerY, LONG right, bool enabled, bool hovered) {
        const LONG trackLeft = right - switchWidth;
        const LONG trackTop = centerY - switchHeight / 2L;
        const float trackRadius = static_cast<float>(switchHeight) * 0.5F;
        const float trackCxL = static_cast<float>(trackLeft) + trackRadius;
        const float trackCxR = static_cast<float>(right) - trackRadius;
        const float trackCy = static_cast<float>(trackTop) + trackRadius;
        const float wash = enabled ? (hovered ? 0.62F : 0.52F) : (hovered ? 0.22F : 0.16F);
        if (enabled) {
            // On = amber accent, matching the dock's running-indicator dot.
            FillPillColorPremul(pixels, width, height, trackCxL, trackCxR, trackCy, trackRadius,
                wash, kAmberB, kAmberG, kAmberR);
        } else {
            FillPillColorPremul(pixels, width, height, trackCxL, trackCxR, trackCy, trackRadius,
                wash, 255, 255, 255);
        }
        const float knobRadius = trackRadius - std::max(2.0F, 2.0F * scale);
        const float knobCx = enabled ? trackCxR : trackCxL;
        FillCirclePremul(pixels, width, height, knobCx, trackCy, knobRadius, 0.95F);
    };

    LONG y = padding;
    RECT titleBounds{padding, y, panelWidth - padding - closeExtent - 8, y + headerHeight};
    DrawFlyoutText(pixels, width, height, titleBounds, titleFont, L"Dock Settings",
        DT_LEFT | DT_VCENTER | DT_SINGLELINE, 250);
    RECT closeBounds{panelWidth - padding - closeExtent, y + (headerHeight - closeExtent) / 2L,
        panelWidth - padding, y + (headerHeight - closeExtent) / 2L + closeExtent};
    if (hoveredKind(SettingsHitKind::Close)) {
        FillCirclePremul(pixels, width, height,
            static_cast<float>(closeBounds.left + closeExtent / 2L),
            static_cast<float>(closeBounds.top + closeExtent / 2L),
            static_cast<float>(closeExtent) * 0.62F, 0.18F);
    }
    DrawFlyoutText(pixels, width, height, closeBounds, titleFont, L"\u00D7",
        DT_CENTER | DT_VCENTER | DT_SINGLELINE, 250);
    pushHit(SettingsHitKind::Close, {closeBounds.left - 6, y, panelWidth - padding + 4,
        y + headerHeight});
    y += headerHeight;

    FillRectPremul(pixels, width, height, {padding, y - dividerGap / 2L, panelWidth - padding,
        y - dividerGap / 2L + 1}, 0.16F);

    struct SettingsRow {
        SettingsHitKind kind;
        const wchar_t* label;
        const wchar_t* sublabel;
        bool enabled;
    };
    const SettingsRow rows[] = {
        {SettingsHitKind::Startup, L"Launch at startup", L"Start Hoverdock with Windows",
            m_config.LaunchAtStartup()},
        {SettingsHitKind::Updates, L"Check for updates", L"Auto-download and install builds",
            m_config.CheckForUpdates()},
    };
    const LONG labelHeight = std::max(16L, std::lround(18.0F * scale));
    const LONG subHeight = std::max(14L, std::lround(16.0F * scale));
    for (const SettingsRow& row : rows) {
        const RECT rowBounds{padding, y, panelWidth - padding, y + rowHeight};
        if (hoveredKind(row.kind)) {
            FillRectPremul(pixels, width, height, rowBounds, 0.10F);
        }
        const LONG textRight = panelWidth - padding - switchWidth - 12L;
        RECT labelBounds{padding + 4, y + (rowHeight - labelHeight - subHeight) / 2L, textRight,
            y + (rowHeight - labelHeight - subHeight) / 2L + labelHeight};
        RECT subBounds{labelBounds.left, labelBounds.bottom, textRight,
            labelBounds.bottom + subHeight};
        DrawFlyoutText(pixels, width, height, labelBounds, labelFont, row.label,
            DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 245);
        DrawFlyoutText(pixels, width, height, subBounds, statusFont, row.sublabel,
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 210);
        drawSwitch(y + rowHeight / 2L, panelWidth - padding - 4L, row.enabled,
            hoveredKind(row.kind));
        pushHit(row.kind, rowBounds);
        y += rowHeight + rowGap;
    }

    const bool checking = m_updateInFlight.load() || m_updateInstalling.load();
    RECT buttonBounds{padding, y, panelWidth - padding, y + buttonHeight};
    FillRectPremul(pixels, width, height, buttonBounds, hoveredKind(SettingsHitKind::CheckNow) &&
            !checking ? 0.28F : 0.16F);
    DrawFlyoutText(pixels, width, height, buttonBounds, labelFont,
        checking ? L"Checking..." : L"Check for updates now",
        DT_CENTER | DT_VCENTER | DT_SINGLELINE, checking ? 170 : 245);
    pushHit(SettingsHitKind::CheckNow, buttonBounds);
    y += buttonHeight + dividerGap;
    FillRectPremul(pixels, width, height, {padding, y - dividerGap / 2L, panelWidth - padding,
        y - dividerGap / 2L + 1}, 0.16F);

    std::string current = Updater::CurrentVersion();
    std::wstring wideVersion(current.begin(), current.end());
    RECT versionBounds{padding, y, panelWidth - padding, y + subHeight};
    DrawFlyoutText(pixels, width, height, versionBounds, statusFont,
        L"Hoverdock v" + wideVersion, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 210);
    RECT statusBounds{padding, versionBounds.bottom + 2, panelWidth - padding,
        y + statusHeight};
    DrawFlyoutText(pixels, width, height, statusBounds, statusFont, m_updateStatus,
        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS, 225);

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
    return m_overflowWindow != nullptr && IsWindowVisible(m_overflowWindow) != FALSE;
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
    // Leaving the zone dismisses the popup and hides the dock.
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
    return false;
}

bool DockApp::IsTrayRenderIndex(int index) const noexcept {
    return index >= 0 && static_cast<size_t>(index) < m_iconRenderData.size() &&
        (m_iconRenderData[static_cast<size_t>(index)].kind == DockIconKind::Tray ||
            m_iconRenderData[static_cast<size_t>(index)].kind == DockIconKind::Clock);
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
    const LONG chevronCenter = chevronTopLeft.x + (chevronBottomRight.x - chevronTopLeft.x) / 2L;
    LONG x = chevronCenter - m_overflowSize.cx / 2L;
    LONG y = m_currentY - gap - m_overflowSize.cy;
    const LONG minX = m_hostBounds.left + margin;
    const LONG maxX = m_hostBounds.right - m_overflowSize.cx - margin;
    if (maxX >= minX) {
        x = std::clamp(x, minX, maxX);
    } else {
        x = minX;
    }
    y = std::max(m_hostBounds.top + margin, y);
    const LONG radius = std::max(18L, std::lround(22.0F * scale));
    caretX = std::clamp(chevronCenter - x, radius, m_overflowSize.cx - radius);
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
        CloseOverflowPopup();
        if (!SystemTray::OpenNetworkPanel()) {
            Log(L"Network panel did not open.");
        }
        break;
    case TrayFlyoutHitKind::Sound:
        if (message == WM_RBUTTONUP) {
            CloseOverflowPopup();
            if (!SystemTray::OpenSoundMixer()) {
                Log(L"Volume mixer did not open.");
            }
            break;
        }
        if (!m_tray.ToggleMute()) {
            Log(L"Volume mute did not change.");
            break;
        }
        EnsureTrayIcons();
        PaintOverflowPopup();
        QueueRenderFrame();
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
    case TrayFlyoutHitKind::ClearAll:
    case TrayFlyoutHitKind::NotificationCenter:
        CloseOverflowPopup();
        PrepareShellForStartMenu();
        if (!SystemTray::OpenNotificationCenter()) {
            ReleaseShellFlyoutHold();
            Log(L"Notification Center did not open.");
            break;
        }
        m_shellFlyoutIsSearch = false;
        m_shellFlyoutIsTray = true;
        break;
    case TrayFlyoutHitKind::NotifyIcon:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < m_overflowIcons.size()) {
            SystemTray::InvokeNotifyIcon(m_overflowIcons[static_cast<size_t>(hit.index)], message);
            CloseOverflowPopup();
        }
        break;
    }
}

void DockApp::PositionOverflowPopup() {
    if (m_overflowWindow == nullptr || !IsOverflowOpen()) {
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
    // The settings panel is anchored beside Quick Settings; follow its moves.
    PositionDockSettings();
}


void DockApp::InvalidateOverflowGlass() noexcept {
    // Swap idiom: release the ~1 MB backing store, not just the size. It is
    // rebuilt on demand by the next paint.
    std::vector<uint8_t>().swap(m_overflowGlass);
    m_overflowGlassSize = {};
    m_overflowGlassOrigin = {};
    m_overflowGlassCaretX = 0;
}

bool DockApp::OverflowGlassValid(POINT origin) const noexcept {
    return !m_overflowGlass.empty()
        && m_overflowGlassSize.cx == m_overflowSize.cx
        && m_overflowGlassSize.cy == m_overflowSize.cy
        && m_overflowGlassOrigin.x == origin.x
        && m_overflowGlassOrigin.y == origin.y
        && m_overflowGlassCaretX == m_overflowCaretX
        && m_overflowGlass.size()
            == static_cast<size_t>(m_overflowSize.cx) * static_cast<size_t>(m_overflowSize.cy) * 4U;
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
        FW_NORMAL);
    m_overflowSectionFont = CreateFlyoutFont(std::max(13, static_cast<int>(std::lround(14.0F * scale))),
        FW_NORMAL);
    m_overflowLabelFont = CreateFlyoutFont(std::max(11, static_cast<int>(std::lround(12.0F * scale))),
        FW_NORMAL);
    m_overflowStatusFont = CreateFlyoutFont(std::max(10, static_cast<int>(std::lround(11.0F * scale))),
        FW_NORMAL);
    m_overflowFontScale = scale;
}

void DockApp::RebuildOverflowPopup() {
    m_overflowIcons = m_tray.EnumerateNotifyIcons();
    m_overflowHover = -1;
    InvalidateOverflowGlass();
    PaintOverflowPopup();
}

void DockApp::PaintOverflowPopup() {
    const UINT dpi = HostDpi();
    const float scale = static_cast<float>(dpi) / 96.0F;
    const LONG padding = std::max(16L, std::lround(18.0F * scale));
    const LONG caretHeight = std::max(10L, std::lround(12.0F * scale));
    const LONG caretWidth = std::max(16L, std::lround(18.0F * scale));
    const LONG radius =
        std::max(16L, std::lround(DOCK_CORNER_RADIUS_PT * scale));
    const LONG headerHeight = std::max(28L, std::lround(32.0F * scale));
    const LONG gearSize = std::max(18L, std::lround(20.0F * scale));
    const LONG circle = std::max(44L, std::lround(52.0F * scale));
    const LONG tileGap = std::max(8L, std::lround(10.0F * scale));
    const LONG labelHeight = std::max(16L, std::lround(18.0F * scale));
    const LONG statusHeight = std::max(14L, std::lround(16.0F * scale));
    const LONG tileBlock = circle + std::max(6L, std::lround(8.0F * scale)) + labelHeight + statusHeight;
    const LONG sectionHeader = std::max(24L, std::lround(28.0F * scale));
    const LONG notificationRow = std::max(48L, std::lround(54.0F * scale));
    const LONG otherIconHeight = std::max(62L, std::lround(70.0F * scale));
    const LONG otherIconSize = std::max(20L, std::lround(22.0F * scale));
    const LONG dividerGap = std::max(10L, std::lround(12.0F * scale));
    const LONG panelWidth = std::max(320L, std::lround(348.0F * scale));
    const size_t otherCount = std::min(m_overflowIcons.size(), static_cast<size_t>(12));
    const LONG otherColumns = otherCount == 0 ? 1L : std::min(4L, static_cast<LONG>(otherCount));
    const LONG otherRows = otherCount == 0 ? 1L :
        (static_cast<LONG>(otherCount) + otherColumns - 1L) / otherColumns;

    LONG contentY = padding;
    contentY += headerHeight;
    contentY += tileBlock;
    contentY += dividerGap;
    contentY += sectionHeader;
    contentY += notificationRow;
    contentY += dividerGap;
    contentY += sectionHeader;
    contentY += otherRows * otherIconHeight;
    contentY += padding;
    m_overflowSize.cx = panelWidth;
    m_overflowSize.cy = contentY + caretHeight;

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
            WS_EX_TOPMOST;
        m_overflowWindow = CreateWindowExW(extendedStyle, className, L"", WS_POPUP, 0, 0, 1, 1,
            m_window, nullptr, m_instance, this);
        if (m_overflowWindow == nullptr) {
            Log(L"Could not create the tray overflow window.");
            return;
        }
        TryExcludeWindowFromCapture(m_overflowWindow);
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
    if (OverflowGlassValid(origin)) {
        std::memcpy(pixels, m_overflowGlass.data(), m_overflowGlass.size());
    } else {
        std::memset(pixels, 0, pixelCount * 4U);
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
        // Frosted backdrop: stacked box blurs approximate the dock shader's wide
        // 17-tap kernel at the same texel scale; sliding-window passes stay
        // O(pixels) regardless of radius.
        const int frostRadius = std::max(4, static_cast<int>(std::lround(12.0F * scale)));
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
            const LONG bodyBottom = m_overflowSize.cy - caretHeight;
            RoundRect(maskDc, 0, 0, maskW, SaturatedInt(bodyBottom * kMaskSupersample),
                SaturatedInt(radius * 2L * kMaskSupersample),
                SaturatedInt(radius * 2L * kMaskSupersample));
            POINT triangle[3] = {
                {(m_overflowCaretX - caretWidth / 2L) * kMaskSupersample,
                    bodyBottom * kMaskSupersample - kMaskSupersample},
                {(m_overflowCaretX + caretWidth / 2L) * kMaskSupersample,
                    bodyBottom * kMaskSupersample - kMaskSupersample},
                {m_overflowCaretX * kMaskSupersample,
                    m_overflowSize.cy * kMaskSupersample - kMaskSupersample},
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
            const float tint = DOCK_GLASS_TINT * 255.0F;
            const float mix = DOCK_GLASS_MIX;
            const float alpha = DOCK_GLASS_ALPHA;
            constexpr float kChannelK[3] = {0.99F, 0.985F, 0.98F};  // DIB order: B, G, R.
            for (LONG y = 0; y < glassH; ++y) {
                for (LONG x = 0; x < glassW; ++x) {
                    const size_t flat = static_cast<size_t>(y) * glassW + x;
                    const float shape = coverage[flat];
                    const float rim = std::clamp((blurredCoverage[flat] - shape) * 2.0F, 0.0F, 1.0F);
                    const float rim4 = rim * rim * rim * rim;
                    float noise = static_cast<float>(x) * 0.06711056F +
                        static_cast<float>(y) * 0.00583715F;
                    noise = noise - std::floor(noise);
                    noise = 52.9829189F * noise;
                    noise = (noise - std::floor(noise)) - 0.5F;
                    uint8_t* pixel = pixels + flat * 4U;
                    for (int channel = 0; channel < 3; ++channel) {
                        const float frosted = static_cast<float>(pixel[channel]);
                        const float base = frosted * (1.0F - mix) + tint * mix;
                        const float target = base * kChannelK[channel] + tint * 0.08F;
                        float shaded = base + 0.22F * (target - base);
                        shaded += tint * rim * 0.12F + 255.0F * rim4 * 0.18F;
                        shaded = std::clamp(shaded + noise, 0.0F, 255.0F);
                        pixel[channel] =
                            static_cast<uint8_t>(std::lround(shaded * shape * alpha));
                    }
                    pixel[3] = static_cast<uint8_t>(std::lround(255.0F * shape * alpha));
                }
            }
            SelectObject(maskDc, previousMask);
            DeleteObject(maskBitmap);
            DeleteDC(maskDc);
        }

        m_overflowGlass.assign(pixels, pixels + pixelCount * 4U);
        m_overflowGlassSize = m_overflowSize;
        m_overflowGlassOrigin = origin;
        m_overflowGlassCaretX = m_overflowCaretX;
    }
    const int width = SaturatedInt(m_overflowSize.cx);
    const int height = SaturatedInt(m_overflowSize.cy);
    EnsureOverflowFonts(scale);
    HFONT titleFont = m_overflowTitleFont;
    HFONT sectionFont = m_overflowSectionFont;
    HFONT labelFont = m_overflowLabelFont;
    HFONT statusFont = m_overflowStatusFont;
    TrayFlyoutHit hoveredHit{};
    const bool hasHover =
        m_overflowHover >= 0 && static_cast<size_t>(m_overflowHover) < m_overflowHits.size();
    if (hasHover) {
        hoveredHit = m_overflowHits[static_cast<size_t>(m_overflowHover)];
    }
    m_overflowHits.clear();

    auto pushHit = [this](TrayFlyoutHitKind kind, RECT bounds, int index = -1) {
        TrayFlyoutHit hit;
        hit.kind = kind;
        hit.index = index;
        hit.bounds = bounds;
        m_overflowHits.push_back(hit);
    };
    auto hoveredKind = [&hoveredHit, hasHover](TrayFlyoutHitKind kind, int index = -1) {
        return hasHover && hoveredHit.kind == kind && hoveredHit.index == index;
    };

    LONG y = padding;
    RECT titleBounds{padding, y, panelWidth - padding - gearSize - 8, y + headerHeight};
    DrawFlyoutText(pixels, width, height, titleBounds, titleFont, L"Quick Settings",
        DT_LEFT | DT_VCENTER | DT_SINGLELINE, 250);
    RECT gearBounds{panelWidth - padding - gearSize, y + (headerHeight - gearSize) / 2L,
        panelWidth - padding, y + (headerHeight - gearSize) / 2L + gearSize};
    if (hoveredKind(TrayFlyoutHitKind::Settings)) {
        FillCirclePremul(pixels, width, height,
            static_cast<float>(gearBounds.left + gearSize / 2L),
            static_cast<float>(gearBounds.top + gearSize / 2L),
            static_cast<float>(gearSize) * 0.72F, 0.18F);
    }
    const UINT gearExtent =
        static_cast<UINT>(std::max(1L, static_cast<LONG>(std::lround(static_cast<float>(gearSize) * 0.85F))));
    const UINT glyphExtent =
        static_cast<UINT>(std::max(18L, std::lround(static_cast<float>(circle) * 0.38F)));
    const UINT notifyGlyph = static_cast<UINT>(std::max(18L, std::lround(19.0F * scale)));
    // Glyphs don't depend on hover; rasterize once per status/extent combination so
    // hover transitions only pay for compositing, not font rasterization.
    EnsureOverflowGlyphs(gearExtent, glyphExtent, notifyGlyph);
    if (!m_overflowGlyphGear.empty()) {
        CompositePremul(pixels, width, height, SaturatedInt(gearBounds.left),
            SaturatedInt(gearBounds.top), m_overflowGlyphGear.data(), SaturatedInt(gearExtent),
            SaturatedInt(gearExtent));
    }
    pushHit(TrayFlyoutHitKind::Settings, {gearBounds.left - 6, y, panelWidth - padding + 4,
        y + headerHeight});
    y += headerHeight;

    struct QuickTile {
        TrayFlyoutHitKind kind;
        TraySlot slot;
        const wchar_t* label;
        std::wstring status;
        wchar_t symbol;
        bool useSlotGlyph;
    };
    // Optimistic brightness projection (see ScrollBrightness): while the worker
    // is persisting scroll input, fill and % show the projected level so the
    // UI tracks the hand instantly and the monitor follows in its own time.
    const int projectedBrightness = m_brightnessTarget.load();
    const QuickTile tiles[] = {
        {TrayFlyoutHitKind::Wifi, TraySlot::Network, L"Wi-Fi", m_tray.NetworkStatusText(), 0, true},
        {TrayFlyoutHitKind::Sound, TraySlot::Volume, L"Sound", m_tray.VolumeStatusText(), 0, true},
        {TrayFlyoutHitKind::Boost, TraySlot::Overflow, L"Boost", m_boostStatus, L'\uE945', false},
        {TrayFlyoutHitKind::Brightness, TraySlot::Overflow, L"Brightness",
            projectedBrightness >= 0 ? std::to_wstring(projectedBrightness) + L"%"
                                     : m_tray.BrightnessStatusText(),
            L'\uE706', false},
    };
    const LONG tileWidth = (panelWidth - padding * 2L - tileGap * 3L) / 4L;
    for (int index = 0; index < 4; ++index) {
        const LONG left = padding + index * (tileWidth + tileGap);
        const RECT tileBounds{left, y, left + tileWidth, y + tileBlock};
        const float cx = static_cast<float>(left) + static_cast<float>(tileWidth) * 0.5F;
        const float cy = static_cast<float>(y) + static_cast<float>(circle) * 0.5F;
        FillCirclePremul(pixels, width, height, cx, cy, static_cast<float>(circle) * 0.5F,
            hoveredKind(tiles[index].kind) ? 0.28F : 0.16F);
        if (tiles[index].kind == TrayFlyoutHitKind::Sound ||
            tiles[index].kind == TrayFlyoutHitKind::Brightness) {
            // Level meter: fill from the bottom up in a slightly whiter wash.
            // Muted (sound) or unavailable (brightness) renders empty, unless a
            // projected scroll value is being shown.
            const TrayStatus& trayStatus = m_tray.Status();
            float level = 0.0F;
            if (tiles[index].kind == TrayFlyoutHitKind::Sound) {
                level = trayStatus.volumeMuted ? 0.0F : trayStatus.volumeLevel;
            } else if (projectedBrightness >= 0) {
                level = static_cast<float>(projectedBrightness) / 100.0F;
            } else if (trayStatus.brightnessAvailable) {
                level = static_cast<float>(trayStatus.brightnessPercent) / 100.0F;
            }
            FillCircleLevelPremul(pixels, width, height, cx, cy,
                static_cast<float>(circle) * 0.5F, level, 0.36F);
        }
        // Glyph bitmaps are cached by EnsureOverflowGlyphs above; hover repaints only composite.
        const std::vector<uint8_t>* glyph = &m_overflowGlyphBrightness;
        if (tiles[index].kind == TrayFlyoutHitKind::Boost) {
            glyph = &m_overflowGlyphBoost;
        } else if (tiles[index].slot == TraySlot::Network) {
            glyph = &m_overflowGlyphWifi;
        } else if (tiles[index].slot == TraySlot::Volume) {
            glyph = &m_overflowGlyphSound;
        }
        if (!glyph->empty()) {
            CompositePremul(pixels, width, height,
                static_cast<int>(std::lround(cx - static_cast<float>(glyphExtent) * 0.5F)),
                static_cast<int>(std::lround(cy - static_cast<float>(glyphExtent) * 0.5F)),
                glyph->data(), static_cast<int>(glyphExtent), static_cast<int>(glyphExtent));
        }
        RECT labelBounds{left, y + circle + 6, left + tileWidth, y + circle + 6 + labelHeight};
        DrawFlyoutText(pixels, width, height, labelBounds, labelFont, tiles[index].label,
            DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 245);
        RECT statusBounds{left, labelBounds.bottom, left + tileWidth, labelBounds.bottom + statusHeight};
        DrawFlyoutText(pixels, width, height, statusBounds, statusFont, tiles[index].status,
            DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 210);
        pushHit(tiles[index].kind, tileBounds);
    }
    y += tileBlock + dividerGap;
    FillRectPremul(pixels, width, height, {padding, y - dividerGap / 2L, panelWidth - padding,
        y - dividerGap / 2L + 1}, 0.16F);

    RECT notifyHeader{padding, y, panelWidth / 2L, y + sectionHeader};
    DrawFlyoutText(pixels, width, height, notifyHeader, sectionFont, L"Notifications",
        DT_LEFT | DT_VCENTER | DT_SINGLELINE, 248);
    RECT clearBounds{panelWidth / 2L, y, panelWidth - padding, y + sectionHeader};
    DrawFlyoutText(pixels, width, height, clearBounds, statusFont, L"Clear all",
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE, hoveredKind(TrayFlyoutHitKind::ClearAll) ? 255 : 225);
    pushHit(TrayFlyoutHitKind::ClearAll, clearBounds);
    y += sectionHeader;

    RECT notifyRow{padding, y, panelWidth - padding, y + notificationRow};
    if (hoveredKind(TrayFlyoutHitKind::NotificationCenter)) {
        FillRectPremul(pixels, width, height, notifyRow, 0.10F);
    }
    if (!m_overflowGlyphBell.empty()) {
        CompositePremul(pixels, width, height, SaturatedInt(padding + 4),
            SaturatedInt(y + (notificationRow - static_cast<LONG>(notifyGlyph)) / 2L),
            m_overflowGlyphBell.data(), static_cast<int>(notifyGlyph),
            static_cast<int>(notifyGlyph));
    }
    RECT notifyTitle{padding + static_cast<LONG>(notifyGlyph) + 14, y + 8, panelWidth - padding - 8,
        y + notificationRow / 2L};
    RECT notifyStatus{notifyTitle.left, notifyTitle.bottom - 2, notifyTitle.right, y + notificationRow - 8};
    DrawFlyoutText(pixels, width, height, notifyTitle, labelFont, L"Notification Center",
        DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 250);
    DrawFlyoutText(pixels, width, height, notifyStatus, statusFont, L"Calendar, toasts, and alerts",
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 210);
    pushHit(TrayFlyoutHitKind::NotificationCenter, notifyRow);
    y += notificationRow + dividerGap;
    FillRectPremul(pixels, width, height, {padding, y - dividerGap / 2L, panelWidth - padding,
        y - dividerGap / 2L + 1}, 0.16F);

    RECT otherHeader{padding, y, panelWidth - padding, y + sectionHeader};
    DrawFlyoutText(pixels, width, height, otherHeader, sectionFont, L"Other Icons",
        DT_LEFT | DT_VCENTER | DT_SINGLELINE, 248);
    y += sectionHeader;
    if (otherCount == 0) {
        RECT empty{padding, y, panelWidth - padding, y + otherIconHeight};
        DrawFlyoutText(pixels, width, height, empty, statusFont, L"No other icons",
            DT_CENTER | DT_VCENTER | DT_SINGLELINE, 210);
    } else {
        const LONG cellWidth = (panelWidth - padding * 2L) / otherColumns;
        for (size_t index = 0; index < otherCount; ++index) {
            const LONG column = static_cast<LONG>(index) % otherColumns;
            const LONG row = static_cast<LONG>(index) / otherColumns;
            const LONG left = padding + column * cellWidth;
            const LONG top = y + row * otherIconHeight;
            const RECT cell{left, top, left + cellWidth, top + otherIconHeight};
            if (hoveredKind(TrayFlyoutHitKind::NotifyIcon, static_cast<int>(index))) {
                FillRectPremul(pixels, width, height, cell, 0.10F);
            }
            if (m_overflowIcons[index].icon != nullptr) {
                const LONG iconLeft = left + (cellWidth - otherIconSize) / 2L;
                const LONG iconTop = top + 6;
                BITMAPV5HEADER iconHeader = header;
                iconHeader.bV5Width = SaturatedInt(otherIconSize);
                iconHeader.bV5Height = -SaturatedInt(otherIconSize);
                void* iconBits = nullptr;
                HBITMAP iconBitmap = CreateDIBSection(memory,
                    reinterpret_cast<const BITMAPINFO*>(&iconHeader), DIB_RGB_COLORS, &iconBits,
                    nullptr, 0);
                if (iconBitmap != nullptr && iconBits != nullptr) {
                    HDC iconDc = CreateCompatibleDC(memory);
                    if (iconDc != nullptr) {
                        HGDIOBJ previousIcon = SelectObject(iconDc, iconBitmap);
                        std::memset(iconBits, 0,
                            static_cast<size_t>(otherIconSize) * otherIconSize * 4U);
                        DrawIconEx(iconDc, 0, 0, m_overflowIcons[index].icon,
                            SaturatedInt(otherIconSize), SaturatedInt(otherIconSize), 0, nullptr,
                            DI_NORMAL);
                        auto* iconPixels = static_cast<uint8_t*>(iconBits);
                        const size_t iconCount =
                            static_cast<size_t>(otherIconSize) * otherIconSize;
                        for (size_t pixel = 0; pixel < iconCount; ++pixel) {
                            uint8_t* sample = iconPixels + pixel * 4U;
                            if ((sample[0] | sample[1] | sample[2]) != 0 && sample[3] == 0) {
                                sample[3] = 255;
                            }
                        }
                        CompositePremul(pixels, width, height, SaturatedInt(iconLeft),
                            SaturatedInt(iconTop), iconPixels, SaturatedInt(otherIconSize),
                            SaturatedInt(otherIconSize));
                        SelectObject(iconDc, previousIcon);
                        DeleteDC(iconDc);
                    }
                    DeleteObject(iconBitmap);
                }
            }
            RECT nameBounds{left + 4, top + otherIconSize + 10, left + cellWidth - 4,
                top + otherIconSize + 10 + labelHeight};
            RECT statusBounds{left + 4, nameBounds.bottom, left + cellWidth - 4,
                top + otherIconHeight - 4};
            DrawFlyoutText(pixels, width, height, nameBounds, labelFont,
                NotifyIconTitle(m_overflowIcons[index]),
                DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 245);
            const std::wstring status = NotifyIconStatus(m_overflowIcons[index]);
            if (!status.empty()) {
                DrawFlyoutText(pixels, width, height, statusBounds, statusFont, status,
                    DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 210);
            }
            pushHit(TrayFlyoutHitKind::NotifyIcon, cell, static_cast<int>(index));
        }
    }



    POINT source{0, 0};
    POINT destination = origin;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(m_overflowWindow, nullptr, &destination, &m_overflowSize, memory, &source, 0,
        &blend, ULW_ALPHA);
    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ShowWindow(m_overflowWindow, SW_SHOWNA);
    PositionOverflowPopup();
}

void DockApp::StartTrayTimer() noexcept {
    if (m_window != nullptr) {
        SetTimer(m_window, kTrayTimerId, kTrayIntervalMs, nullptr);
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
    if (m_shellFlyoutHold || !m_rendererInitialized || m_visibility == VisibilityState::Hidden) {
        return false;
    }

    const RECT captureBounds{m_windowX, m_currentY,
        m_windowX + static_cast<LONG>(m_dockWidth),
        m_currentY + static_cast<LONG>(m_dockHeight)};

    bool rendererVisible = false;
    bool inputVisible = false;
    bool ghostVisible = false;
    if (m_backdropCaptureRequiresHide) {
        rendererVisible = IsWindowVisible(m_window) != FALSE;
        inputVisible = m_inputWindow != nullptr && IsWindowVisible(m_inputWindow) != FALSE;
        ghostVisible = m_dragGhostWindow != nullptr && IsWindowVisible(m_dragGhostWindow) != FALSE;
        if (rendererVisible) {
            ShowWindow(m_window, SW_HIDE);
        }
        if (inputVisible) {
            ShowWindow(m_inputWindow, SW_HIDE);
        }
        if (ghostVisible) {
            ShowWindow(m_dragGhostWindow, SW_HIDE);
        }
        // DwmFlush never returns while DWM composition is off (e.g. an
        // exclusive-fullscreen game owns the display). Blocking the UI thread
        // here would wedge input, timers, and paints indefinitely, so only
        // flush when composition is actually running.
        BOOL compositionEnabled = FALSE;
        if (SUCCEEDED(DwmIsCompositionEnabled(&compositionEnabled)) && compositionEnabled) {
            DwmFlush();
        }
    }

    bool changed = true;
    const bool captured = m_renderer.CaptureBackdrop(captureBounds, &changed);

    if (m_backdropCaptureRequiresHide) {
        if (rendererVisible) {
            ShowWindow(m_window, SW_SHOWNOACTIVATE);
        }
        if (inputVisible) {
            ShowWindow(m_inputWindow, SW_SHOWNOACTIVATE);
        }
        if (ghostVisible) {
            ShowWindow(m_dragGhostWindow, SW_SHOWNOACTIVATE);
        }
    }

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

    const RECT captureBounds{m_windowX, m_visibleY,
        m_windowX + static_cast<LONG>(m_dockWidth),
        m_visibleY + static_cast<LONG>(m_dockHeight)};
    if (!m_renderer.CaptureBackdrop(captureBounds)) {
        Log(L"Desktop backdrop capture failed; using fallback glass.");
    }

    m_visibility = VisibilityState::Showing;
    ShowWindow(m_window, SW_SHOWNOACTIVATE);
    ShowWindow(m_inputWindow, SW_SHOWNOACTIVATE);
    PositionOverlayWindows();
    m_animationFromY = m_currentY;
    m_animationToY = m_visibleY;
    m_animationStartedAt = QpcSeconds();
    QueueRenderFrame();
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
    CloseOverflowPopup();
    StopRefreshTimer();
    StopTrayTimer();
    StopBackdropTimer();
    m_visibility = VisibilityState::Hiding;
    ShowWindow(m_inputWindow, SW_HIDE);
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
        StartRefreshTimer();
        StartTrayTimer();
        StartBackdropTimer();
        ScheduleDeferredRefresh();
        UpdateHoverLabel();
        return;
    }

    m_visibility = VisibilityState::Hidden;
    m_currentY = m_hiddenY;
    StopBackdropTimer();
    StopRefreshTimer();
    StopTrayTimer();
    CloseOverflowPopup();
    ClearPressState();
    HideHoverLabel();
    ShowWindow(m_inputWindow, SW_HIDE);
    ShowWindow(m_window, SW_HIDE);
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
        icon.pressed = m_draggedIcon < 0 && static_cast<int>(index) == m_pressedIcon;
    }

    DockRenderState state;
    state.width = m_dockWidth;
    state.height = m_dockHeight;
    state.glassAlpha = DOCK_GLASS_ALPHA;
    state.slideProgress = m_dockHeight == 0 ? 0.0F :
        static_cast<float>(m_visibleY - m_currentY) / static_cast<float>(m_dockHeight);
    state.timeSeconds = static_cast<float>(QpcSeconds());
    state.showDevBounds = m_config.ShowDevBounds();
    state.skipIfGpuBusy = m_dropPresentPending || (IsDragActive() && !m_dragSnapAnimating);
    state.allowBlockingGpuWait = allowBlockingGpuWait && !IsAnimating() && !IsDragActive() &&
        !m_dragSnapAnimating && !m_dropPresentPending;
    state.icons = m_iconRenderData;
    return m_renderer.Render(state);
}

void DockApp::StartBackdropTimer() noexcept {
    if (m_window != nullptr) {
        SetTimer(m_window, kBackdropTimerId, kBackdropIntervalMs, nullptr);
    }
}

void DockApp::StopBackdropTimer() noexcept {
    if (m_window != nullptr) {
        KillTimer(m_window, kBackdropTimerId);
    }
}

void DockApp::HandlePointer(POINT cursor) {
    m_lastCursor = cursor;
    if (IsDragActive()) {
        if (m_draggedIcon >= 0) {
            UpdateDrag(cursor);
        }
        return;
    }

    const bool inHotZone = IsCursorInBottomHotZone(cursor);
    if (m_shellFlyoutHold) {
        return;
    }
    if (inHotZone) {
        static_cast<void>(AdoptMonitorForCursor(cursor));
    }

    if (m_visibility == VisibilityState::Hidden) {
        HideHoverLabel();
        if (inHotZone) {
            BeginShow();
        }
        return;
    }

    if (m_visibility == VisibilityState::Hiding && inHotZone) {
        BeginShow();
        return;
    }

    if ((m_visibility == VisibilityState::Showing || m_visibility == VisibilityState::Visible) &&
        !inHotZone && !IsLaunchPromptOpen() && !IsDockSettingsOpen() && !IsCursorOverDock(cursor) &&
        !IsCursorWithinFlyoutZone(cursor) &&
        (cursor.y < m_visibleY ||
            MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST) != m_hostMonitor)) {
        BeginHide();
        return;
    }

    const int hovered = IconAtScreenPoint(cursor);
    const int hoveredDivider = DividerAtScreenPoint(cursor);
    if (hoveredDivider >= 0 && m_hoveredIcon >= 0) {
        m_hoveredIcon = -1;
        HideHoverLabel();
        if (!IsAnimating()) {
            QueueRenderFrame();
        }
    }
    if (hovered != m_hoveredIcon) {
        m_hoveredIcon = hovered;
        if (!IsAnimating()) {
            QueueRenderFrame();
        }
        UpdateHoverLabel();
    }
    if (hoveredDivider != m_hoveredDivider) {
        m_hoveredDivider = hoveredDivider;
    }
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

void DockApp::HandleContextMenu(POINT screenPoint) {
    const int icon = IconAtScreenPoint(screenPoint);
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    DisplayApp app;
    const bool hasApp = icon >= 0 && static_cast<size_t>(icon) < m_displayApps.size() &&
        !IsLayoutOnlyTarget(m_displayApps[static_cast<size_t>(icon)].app.target) &&
        !IsTrayRenderIndex(icon);
    const bool isSpecial = hasApp && IsSpecialDockTarget(m_displayApps[static_cast<size_t>(icon)].app.target);
    if (hasApp) {
        app = m_displayApps[static_cast<size_t>(icon)];
        if (isSpecial) {
            AppendMenuW(menu, MF_STRING, kContextOpen, L"Open");
        } else {
            const bool running = app.runningWindow != nullptr;
            AppendMenuW(menu, MF_STRING,
                app.persistentPinIndex >= 0 ? kContextUnpin : kContextPin,
                app.persistentPinIndex >= 0 ? L"Unpin from taskbar" : L"Pin to taskbar");
            if (running) {
                AppendMenuW(menu, MF_STRING, kContextEndTask, L"End task");
                AppendMenuW(menu, MF_STRING, kContextClose, L"Close window");
            }
            if (app.app.target.rfind(L"shell:", 0) != 0) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, kContextOpenLocation, L"Open location");
            }
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
        m_windows.RebuildPinProfiles(m_config.Pins());
        RebuildDisplayApps();
        // Pin/unpin must stay responsive: RebuildLayout(false) updates geometry
        // immediately without the synchronous full-atlas icon extraction that
        // LoadIconTextures performs on this thread (shell queries + GPU flush).
        // Any icon not yet cached (the newly pinned app) is extracted off-thread
        // and applied incrementally; unpin needs no extraction at all.
        if (IconPixelExtent() != m_loadedIconExtent) {
            RebuildLayout(true);
        } else {
            RebuildLayout(false);
            EnsureMissingPinIconsAsync();
        }
        m_lastWindowRefresh = QpcSeconds();
        RefreshRunningWindows(true);
    } else {
        RefreshRunningWindows();
    }
    QueueRenderFrame();
}

void DockApp::ActivatePressedApp() {
    if (m_pressedIcon < 0 || static_cast<size_t>(m_pressedIcon) >= m_iconRenderData.size()) {
        return;
    }
    if (IsTrayRenderIndex(m_pressedIcon)) {
        OpenTraySlot(m_iconRenderData[static_cast<size_t>(m_pressedIcon)].traySlot);
        return;
    }
    if (static_cast<size_t>(m_pressedIcon) >= m_displayApps.size()) {
        return;
    }

    const DisplayApp app = m_displayApps[static_cast<size_t>(m_pressedIcon)];
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

    if (!m_windows.ActivateOrLaunch(app.app, app.runningWindow)) {
        Log(L"Application did not launch or accept focus.");
    }
    m_lastWindowRefresh = 0.0;
    RefreshRunningWindows();
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
    TryExcludeWindowFromCapture(m_launchPromptWindow);

    const DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(m_launchPromptWindow, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
        sizeof(corner));

    HFONT font = HoverLabelFont();
    m_launchEdit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 1, 1, m_launchPromptWindow,
        reinterpret_cast<HMENU>(1), m_instance, nullptr);
    m_launchStatus = CreateWindowExW(0, L"STATIC", L"Describe the app to open",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 1, 1, m_launchPromptWindow,
        reinterpret_cast<HMENU>(2), m_instance, nullptr);
    if (m_launchEdit == nullptr || m_launchStatus == nullptr) {
        Log(L"Could not create the launch prompt controls.");
        CloseLaunchPrompt(false);
        return false;
    }

    SendMessageW(m_launchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(m_launchStatus, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(m_launchEdit, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(L"the spreadsheet, chrome, start menu..."));

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
    if (cursor.y < m_visibleY && !IsCursorOverDock(cursor)) {
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
    const LONG height = padding + editHeight + gap + statusHeight + padding;
    const LONG x = m_windowX + (static_cast<LONG>(m_dockWidth) - width) / 2L;
    const LONG y = m_visibleY - height - GreaterOf(8L, static_cast<LONG>(std::lround(10.0F * scale)));

    SetWindowPos(m_launchPromptWindow, HWND_TOPMOST, SaturatedInt(x), SaturatedInt(y),
        SaturatedInt(width), SaturatedInt(height), SWP_NOOWNERZORDER);
    if (m_launchEdit != nullptr) {
        SetWindowPos(m_launchEdit, nullptr, SaturatedInt(padding), SaturatedInt(padding),
            SaturatedInt(width - padding * 2L), SaturatedInt(editHeight),
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (m_launchStatus != nullptr) {
        SetWindowPos(m_launchStatus, nullptr, SaturatedInt(padding),
            SaturatedInt(padding + editHeight + gap), SaturatedInt(width - padding * 2L),
            SaturatedInt(statusHeight), SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void DockApp::SetLaunchPromptStatus(const std::wstring& text) {
    if (m_launchStatus != nullptr) {
        SetWindowTextW(m_launchStatus, text.c_str());
    }
}

void DockApp::SubmitLaunchPrompt() {
    if (m_launchInFlight || m_launchEdit == nullptr) {
        return;
    }

    const std::wstring request = TrimWide(WindowText(m_launchEdit));
    if (request.empty()) {
        SetLaunchPromptStatus(L"Describe the app to open");
        return;
    }

    const UINT generation = ++m_launchGeneration;
    m_launchInFlight = true;
    SetLaunchPromptStatus(L"Looking...");

    const std::wstring apiKey = m_config.TypeSafeApiKey();
    const HWND replyWindow = m_window;
    const std::vector<DisplayApp> displayApps = m_displayApps;
    const std::vector<RunningWindow> runningWindows = m_windows.RunningWindows();
    std::thread([this, generation, apiKey, request, replyWindow, displayApps, runningWindows]() {
        // Never let an exception escape: an uncaught throw in a detached
        // thread calls std::terminate and the dock vanishes with no log.
        std::vector<LaunchTarget> targets;
        LaunchJudgment judgment;
        try {
            m_installedApps.EnsureLoaded();
            targets = CollectLaunchTargets(displayApps, runningWindows);
            const std::string exactId = ExactLaunchId(request, targets);
            if (!exactId.empty()) {
                judgment.action = LaunchJudgment::Action::Launch;
                judgment.chosenId = exactId;
                judgment.exists = 1.0;
                judgment.confidence = 1.0;
            } else {
                std::vector<LaunchCandidate> candidates;
                candidates.reserve(targets.size());
                for (const LaunchTarget& target : targets) {
                    candidates.push_back(MakeLaunchCandidate(target));
                }
                judgment = TypeSafeClient::ResolveApp(apiKey, request, candidates);
            }
        } catch (const std::exception&) {
            judgment.action = LaunchJudgment::Action::Error;
            judgment.error = L"Launch failed before a match could be picked.";
        } catch (...) {
            judgment.action = LaunchJudgment::Action::Error;
            judgment.error = L"Launch failed unexpectedly.";
        }
        LaunchReply* reply = nullptr;
        try {
            reply = new LaunchReply{generation, std::move(judgment), std::move(targets)};
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

    const LaunchTarget* target = FindLaunchTarget(judgment.chosenId);
    if (target == nullptr) {
        SetLaunchPromptStatus(L"No matching installed app.");
        return;
    }
    if (!ActivateLaunchTarget(*target)) {
        SetLaunchPromptStatus(L"The selected app could not be opened.");
        return;
    }
    CloseLaunchPrompt(false);
    BeginHide();
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
    if (!m_windows.ActivateOrLaunch(target.app, target.runningWindow)) {
        Log(L"Application did not launch or accept focus.");
        return false;
    }
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
    TryExcludeWindowFromCapture(m_dragGhostWindow);
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
    POINT topLeft{bounds.left, bounds.top};
    if (ClientToScreen(m_window, &topLeft) == FALSE) {
        return;
    }
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
    if (m_pressedIcon < 0 || !IsPersistentDisplayIcon(m_pressedIcon)) {
        return;
    }

    CacheLayoutSlotBounds();
    m_draggedIcon = m_pressedIcon;
    m_dragOriginIndex = m_pressedIcon;
    m_dragOriginBounds = m_iconRenderData[static_cast<size_t>(m_draggedIcon)].bounds;
    m_dragInsertion = InsertionIndexForDrag(screenCursor);

    POINT iconTopLeft{m_dragOriginBounds.left, m_dragOriginBounds.top};
    if (ClientToScreen(m_window, &iconTopLeft) == FALSE) {
        iconTopLeft = screenCursor;
    }
    m_dragGrabOffset = {screenCursor.x - iconTopLeft.x, screenCursor.y - iconTopLeft.y};

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

    POINT originTopLeft{m_dragOriginBounds.left, m_dragOriginBounds.top};
    if (ClientToScreen(m_window, &originTopLeft) == FALSE) {
        originTopLeft = releaseCursor;
    }

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
    m_pressedIcon = -1;
    m_draggedIcon = -1;
    m_dragInsertion = -1;
    m_dragOriginIndex = -1;
    m_dragOriginBounds = {};
    m_dragGrabOffset = {};
    m_pressedAt = {};
    m_scalingDivider = false;
    m_hoveredDivider = -1;
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

    const auto alreadyRepresentedWindow = [&displayApps, &unpinnedApps, &windows, &pins,
                                           &representedHandles](const RunningWindow& window) {
        if (representedHandles.contains(window.handle)) {
            return true;
        }
        if (windows.MatchesAnyPin(window)) {
            return true;
        }
        if (std::ranges::any_of(pins, [&window](const PinnedApp& pin) {
                return WindowCatalog::TargetsMatch(
                           WindowCatalog::ResolveLauncherProcessPath(pin), window.executablePath) ||
                    WindowCatalog::TargetsMatch(pin.target, window.executablePath);
            })) {
            return true;
        }
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
    if (m_visibility == VisibilityState::Hidden || m_visibility == VisibilityState::Hiding) {
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
                snapshot.missingIconTargets.push_back(key);
                snapshot.missingIconPixels.push_back(Renderer::ExtractIconPixels(
                    WindowCatalog::IconResolutionCandidates(app.app, app.runningWindow), iconPixelExtent));
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
    if (generation != m_refreshGeneration.load() ||
        (m_visibility != VisibilityState::Visible && m_visibility != VisibilityState::Showing)) {
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
        if (!snapshot.runningChanged) {
            return;
        }
    } else if (snapshot.layoutChanged) {
        AssignIconTextureIndices();
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
        return false;
    }

    RECT full = m_primaryBounds;
    const UINT flags = notify ? SPIF_SENDCHANGE : 0;
    if (SystemParametersInfoW(SPI_SETWORKAREA, 0, &full, flags) != FALSE && notify) {
        Log(L"Desktop work area expanded.");
    }
    m_workAreaExpanded = true;
    return true;
}

void DockApp::RestoreDesktopWorkArea() {
    if (!m_workAreaSaved) {
        return;
    }
    static_cast<void>(SystemParametersInfoW(SPI_SETWORKAREA, 0, &m_savedWorkArea, SPIF_SENDCHANGE));
    m_workAreaSaved = false;
    m_workAreaExpanded = false;
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

bool DockApp::ExpandSecondaryMonitorWorkAreas() {
    std::vector<HWND> taskbars;
    EnumWindows(&DockApp::FindTaskbarWindow, reinterpret_cast<LPARAM>(&taskbars));

    std::vector<std::pair<HMONITOR, RECT>> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitorBounds, reinterpret_cast<LPARAM>(&monitors));
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
    std::vector<HWND> taskbars;
    EnumWindows(&DockApp::FindTaskbarWindow, reinterpret_cast<LPARAM>(&taskbars));
    bool collapsed = false;
    std::vector<std::pair<HWND, RECT>> stillCollapsed;
    stillCollapsed.reserve(taskbars.size());
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
            stillCollapsed.emplace_back(tray, trayRect);
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
        stillCollapsed.emplace_back(tray, trayRect);
        collapsed = true;
    }
    m_collapsedTaskbars = std::move(stillCollapsed);
    return collapsed;
}

bool DockApp::MaintainNativeTaskbarSuppression() {
    if (m_shellFlyoutHold) {
        std::vector<HWND> taskbars;
        EnumWindows(&DockApp::FindTaskbarWindow, reinterpret_cast<LPARAM>(&taskbars));
        for (HWND taskbar : taskbars) {
            if (IsWindowVisible(taskbar) != FALSE) {
                HideTaskbarWindow(taskbar);
            }
        }
        if (IsStartLauncherVisible()) {
            m_shellFlyoutHoldUntil = 0.0;
            return true;
        }
        if (m_shellFlyoutHoldUntil > 0.0 && QpcSeconds() < m_shellFlyoutHoldUntil) {
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

    std::vector<HWND> taskbars;
    EnumWindows(&DockApp::FindTaskbarWindow, reinterpret_cast<LPARAM>(&taskbars));
    for (HWND taskbar : taskbars) {
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
    m_taskbarMonitorQuietPasses = 0;
    m_taskbarMonitorFast = true;
    if (m_window != nullptr) {
        SetTimer(m_window, kTaskbarMonitorTimerId, kTaskbarMonitorIntervalMs, nullptr);
    }
}

void DockApp::StopTaskbarMonitor() noexcept {
    if (m_window != nullptr) {
        KillTimer(m_window, kTaskbarMonitorTimerId);
    }
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
        Log(L"Start menu did not accept input.");
        return false;
    }
    return true;
}

void DockApp::PrepareShellForStartMenu() {
    if (GetCapture() != nullptr) {
        ReleaseCapture();
    }
    m_shellFlyoutHold = true;
    m_shellFlyoutIsSearch = false;
    m_shellFlyoutIsTray = false;
    m_shellFlyoutHoldUntil = QpcSeconds() + 2.5;
    HideOverlayForShellFlyout();
}

void DockApp::HideOverlayForShellFlyout() {
    m_overlayHiddenForFlyout = true;
    StopBackdropTimer();
    HideHoverLabel();
    CloseOverflowPopup();
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
    RestoreOverlayAfterShellFlyout();
    SuppressNativeTaskbar();
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
            m_iconRenderData[index].kind == DockIconKind::Clock) {
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
    return cursor.x >= m_windowX && cursor.x < m_windowX + static_cast<LONG>(m_dockWidth) &&
        cursor.y >= m_currentY && cursor.y < m_currentY + static_cast<LONG>(m_dockHeight);
}

bool DockApp::ShouldPostPointerUpdate(POINT cursor) const noexcept {
    switch (m_visibility) {
    case VisibilityState::Hidden:
        return IsCursorInBottomHotZone(cursor);
    case VisibilityState::Hiding:
        return IsCursorInBottomHotZone(cursor);
    case VisibilityState::Showing:
    case VisibilityState::Visible:
        return !IsCursorOverDock(cursor);
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
