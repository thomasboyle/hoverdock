#include "SystemTray.h"
#include "AudioPolicy.h"
#include "DockTheme.hlsli"
#include "TrayIconPolicy.h"

#include <Unknwn.h>
#include <dwmapi.h>
#include <endpointvolume.h>
#include <highlevelmonitorconfigurationapi.h>
#include <mmdeviceapi.h>
#include <netlistmgr.h>
#include <physicalmonitorenumerationapi.h>
#include <powrprof.h>
#include <UIAutomation.h>
#include <oleauto.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wlanapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kOverflowTarget[] = L"dock:tray:overflow";
constexpr wchar_t kNetworkTarget[] = L"dock:tray:network";
constexpr wchar_t kVolumeTarget[] = L"dock:tray:volume";
constexpr wchar_t kPowerTarget[] = L"dock:tray:power";
constexpr wchar_t kClockTarget[] = L"dock:tray:clock";

constexpr float kGlyphR = DOCK_INK_R;
constexpr float kGlyphG = DOCK_INK_G;
constexpr float kGlyphB = DOCK_INK_B;

constexpr GUID kVideoSubgroupGuid = {
    0x7516b95f, 0xf776, 0x4464, {0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99}};
constexpr GUID kVideoBrightnessGuid = {
    0xaded5e82, 0xb909, 0x4619, {0x99, 0x49, 0xf5, 0xd7, 0x1d, 0xac, 0x0b, 0xcb}};

constexpr DWORD kQueryDisplayBrightness = 0x00230498;
constexpr DWORD kSetDisplayBrightness = 0x0023049C;

struct DisplayBrightness {
    UCHAR policy = 0;
    UCHAR ac = 0;
    UCHAR dc = 0;
};

INPUT KeyboardInput(WORD key, DWORD flags) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = flags;
    return input;
}

bool OpenShellTarget(const wchar_t* target, const wchar_t* parameters = nullptr) {
    if (target == nullptr || target[0] == 0) {
        return false;
    }
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", target, parameters, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool SendWinChord(WORD key) {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD processId = 0;
    if (tray != nullptr) {
        GetWindowThreadProcessId(tray, &processId);
    }
    if (processId != 0) {
        AllowSetForegroundWindow(processId);
    } else {
        AllowSetForegroundWindow(ASFW_ANY);
    }

    const std::array<INPUT, 4> inputs = {
        KeyboardInput(VK_LWIN, KEYEVENTF_EXTENDEDKEY),
        KeyboardInput(key, 0),
        KeyboardInput(key, KEYEVENTF_KEYUP),
        KeyboardInput(VK_LWIN, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP),
    };
    return SendInput(static_cast<UINT>(inputs.size()), const_cast<INPUT*>(inputs.data()),
        sizeof(INPUT)) == static_cast<UINT>(inputs.size());
}

bool FontFamilyInstalled(const wchar_t* faceName) noexcept {
    HDC dc = GetDC(nullptr);
    if (dc == nullptr) {
        return false;
    }
    LOGFONTW probe{};
    probe.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(probe.lfFaceName, faceName, _TRUNCATE);
    BOOL found = FALSE;
    EnumFontFamiliesExW(
        dc, &probe,
        [](const LOGFONTW* /*lf*/, const TEXTMETRICW* /*tm*/, DWORD /*type*/, LPARAM lParam) -> int {
            *reinterpret_cast<BOOL*>(lParam) = TRUE;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found != FALSE;
}

const wchar_t* ResolveDockTextFontFace() noexcept {
    // Prefer Windows default UI face; fall back only if Segoe UI is missing.
    static const wchar_t* face = []() -> const wchar_t* {
        if (FontFamilyInstalled(L"Segoe UI")) {
            return L"Segoe UI";
        }
        if (FontFamilyInstalled(L"Segoe UI Variable")) {
            return L"Segoe UI Variable";
        }
        return L"Microsoft Sans Serif";
    }();
    return face;
}

HFONT CreateTrayFont(int pixelHeight, int weight) {
    return CreateFontW(-pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        ResolveDockTextFontFace());
}

int ClockTimePx(float layoutScale) {
    return std::max(18, static_cast<int>(std::lround(23.0F * layoutScale)));
}

int ClockDatePx(float layoutScale) {
    return std::max(17, static_cast<int>(std::lround(20.0F * layoutScale)));
}

LONG ClockGapPx(float layoutScale) {
    return std::max(1L, static_cast<LONG>(std::lround(2.0F * layoutScale)));
}

SIZE MeasureText(HDC dc, const std::wstring& text) {
    SIZE size{};
    if (!text.empty()) {
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    }
    return size;
}

void ConvertCoverageToInkPremul(uint8_t* pixels, size_t byteCount) {
    // Glyph buffers are BGR-ordered: byte0=B, byte1=G, byte2=R.
    for (size_t index = 0; index + 3 < byteCount; index += 4) {
        const unsigned coverage =
            (static_cast<unsigned>(pixels[index]) * 19U +
                static_cast<unsigned>(pixels[index + 1]) * 183U +
                static_cast<unsigned>(pixels[index + 2]) * 54U) >>
            8U;
        pixels[index] = static_cast<uint8_t>((DOCK_INK_B * coverage + 127U) / 255U);
        pixels[index + 1] = static_cast<uint8_t>((DOCK_INK_G * coverage + 127U) / 255U);
        pixels[index + 2] = static_cast<uint8_t>((DOCK_INK_R * coverage + 127U) / 255U);
        pixels[index + 3] = static_cast<uint8_t>(coverage);
    }
}

bool HasCoverage(const std::vector<uint8_t>& pixels) {
    for (size_t index = 3; index < pixels.size(); index += 4) {
        if (pixels[index] > 24) {
            return true;
        }
    }
    return false;
}

HFONT CreateIconFont(int pixelHeight, const wchar_t* family) {
    return CreateFontW(-pixelHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        family);
}

std::vector<uint8_t> RasterizeFontGlyph(UINT extent, wchar_t symbol, const wchar_t* family) {
    std::vector<uint8_t> pixels(static_cast<size_t>(extent) * extent * 4U, 0);
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return pixels;
    }
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr) {
        return pixels;
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
        return pixels;
    }

    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    std::memset(bits, 0, pixels.size());
    const int fontHeight = std::max(12, static_cast<int>(extent * 78U / 100U));
    HFONT font = CreateIconFont(fontHeight, family);
    HGDIOBJ previousFont = SelectObject(memory, font);
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(255, 255, 255));
    RECT bounds{0, 0, static_cast<LONG>(extent), static_cast<LONG>(extent)};
    const wchar_t text[2] = {symbol, 0};
    DrawTextW(memory, text, 1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    std::memcpy(pixels.data(), bits, pixels.size());
    ConvertCoverageToInkPremul(pixels.data(), pixels.size());
    SelectObject(memory, previousFont);
    if (font != nullptr) {
        DeleteObject(font);
    }
    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    return pixels;
}

std::vector<uint8_t> RasterizeFontGlyph(UINT extent, wchar_t symbol) {
    std::vector<uint8_t> pixels = RasterizeFontGlyph(extent, symbol, L"Segoe Fluent Icons");
    if (HasCoverage(pixels)) {
        return pixels;
    }
    return RasterizeFontGlyph(extent, symbol, L"Segoe MDL2 Assets");
}

wchar_t SymbolForSlot(TraySlot slot, const TrayStatus& status) {
    switch (slot) {
    case TraySlot::Overflow:
        return L'\uE70E';
    case TraySlot::Network:
        if (status.network == TrayNetworkKind::Ethernet) {
            return L'\uE839';
        }
        if (status.network == TrayNetworkKind::Disconnected) {
            return L'\uEB55';
        }
        return L'\uE701';
    case TraySlot::Volume:
        return status.volumeMuted ? L'\uE74F' : L'\uE767';
    case TraySlot::Power:
        if (!status.hasBattery) {
            return L'\uE7E8';
        }
        if (status.batteryCharging) {
            return L'\uEBB5';
        }
        return static_cast<wchar_t>(0xEBA0 + std::clamp(status.batteryPercent / 10, 0, 10));
    case TraySlot::Clock:
        break;
    }
    return L'\uE70E';
}

void StampCoverage(std::vector<uint8_t>& pixels, UINT extent, int x, int y, float coverage) {
    if (coverage <= 0.0F || x < 0 || y < 0 || x >= static_cast<int>(extent) ||
        y >= static_cast<int>(extent)) {
        return;
    }
    coverage = std::min(coverage, 1.0F);
    const size_t offset = (static_cast<size_t>(y) * extent + static_cast<UINT>(x)) * 4U;
    const float srcA = coverage;
    const float dstA = static_cast<float>(pixels[offset + 3]) / 255.0F;
    const float outA = srcA + dstA * (1.0F - srcA);
    if (outA <= 0.0F) {
        return;
    }
    const auto blend = [srcA, dstA, outA](float src, uint8_t dst) {
        const float dstN = static_cast<float>(dst) / 255.0F;
        return static_cast<uint8_t>(std::lround((src * srcA + dstN * dstA * (1.0F - srcA)) / outA * 255.0F));
    };
    pixels[offset] = blend(kGlyphB / 255.0F, pixels[offset]);
    pixels[offset + 1] = blend(kGlyphG / 255.0F, pixels[offset + 1]);
    pixels[offset + 2] = blend(kGlyphR / 255.0F, pixels[offset + 2]);
    pixels[offset + 3] = static_cast<uint8_t>(std::lround(outA * 255.0F));
}

void StampSdf(std::vector<uint8_t>& pixels, UINT extent, float cx, float cy, float distance,
    float halfWidth) {
    const float coverage = 1.0F - std::clamp((distance - halfWidth) * 1.35F + 0.5F, 0.0F, 1.0F);
    StampCoverage(pixels, extent, static_cast<int>(std::floor(cx)), static_cast<int>(std::floor(cy)),
        coverage);
}

float DistanceToSegment(float px, float py, float ax, float ay, float bx, float by) {
    const float abx = bx - ax;
    const float aby = by - ay;
    const float lengthSq = abx * abx + aby * aby;
    float t = 0.0F;
    if (lengthSq > 1.0e-6F) {
        t = std::clamp(((px - ax) * abx + (py - ay) * aby) / lengthSq, 0.0F, 1.0F);
    }
    const float dx = px - (ax + abx * t);
    const float dy = py - (ay + aby * t);
    return std::sqrt(dx * dx + dy * dy);
}

void StrokeSegment(std::vector<uint8_t>& pixels, UINT extent, float ax, float ay, float bx, float by,
    float halfWidth) {
    const int left = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - halfWidth - 2.0F)));
    const int top = std::max(0, static_cast<int>(std::floor(std::min(ay, by) - halfWidth - 2.0F)));
    const int right = std::min(static_cast<int>(extent),
        static_cast<int>(std::ceil(std::max(ax, bx) + halfWidth + 2.0F)));
    const int bottom = std::min(static_cast<int>(extent),
        static_cast<int>(std::ceil(std::max(ay, by) + halfWidth + 2.0F)));
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            StampSdf(pixels, extent, static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                DistanceToSegment(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, ax, ay,
                    bx, by),
                halfWidth);
        }
    }
}

void StrokeCircle(std::vector<uint8_t>& pixels, UINT extent, float cx, float cy, float radius,
    float halfWidth) {
    const int left = std::max(0, static_cast<int>(std::floor(cx - radius - halfWidth - 2.0F)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - halfWidth - 2.0F)));
    const int right =
        std::min(static_cast<int>(extent), static_cast<int>(std::ceil(cx + radius + halfWidth + 2.0F)));
    const int bottom =
        std::min(static_cast<int>(extent), static_cast<int>(std::ceil(cy + radius + halfWidth + 2.0F)));
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - cx;
            const float dy = static_cast<float>(y) + 0.5F - cy;
            StampSdf(pixels, extent, static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                std::abs(std::sqrt(dx * dx + dy * dy) - radius), halfWidth);
        }
    }
}

void FillCircle(std::vector<uint8_t>& pixels, UINT extent, float cx, float cy, float radius) {
    const int left = std::max(0, static_cast<int>(std::floor(cx - radius - 2.0F)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 2.0F)));
    const int right = std::min(static_cast<int>(extent), static_cast<int>(std::ceil(cx + radius + 2.0F)));
    const int bottom = std::min(static_cast<int>(extent), static_cast<int>(std::ceil(cy + radius + 2.0F)));
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - cx;
            const float dy = static_cast<float>(y) + 0.5F - cy;
            StampSdf(pixels, extent, static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                std::sqrt(dx * dx + dy * dy), radius);
        }
    }
}

void StrokeArc(std::vector<uint8_t>& pixels, UINT extent, float cx, float cy, float radius,
    float halfWidth, float startDeg, float endDeg) {
    const int left = std::max(0, static_cast<int>(std::floor(cx - radius - halfWidth - 2.0F)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - halfWidth - 2.0F)));
    const int right =
        std::min(static_cast<int>(extent), static_cast<int>(std::ceil(cx + radius + halfWidth + 2.0F)));
    const int bottom =
        std::min(static_cast<int>(extent), static_cast<int>(std::ceil(cy + radius + halfWidth + 2.0F)));
    const float start = startDeg * 3.14159265F / 180.0F;
    const float end = endDeg * 3.14159265F / 180.0F;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - cx;
            const float dy = static_cast<float>(y) + 0.5F - cy;
            const float angle = std::atan2(-dy, dx);
            if (angle < start || angle > end) {
                continue;
            }
            StampSdf(pixels, extent, static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                std::abs(std::sqrt(dx * dx + dy * dy) - radius), halfWidth);
        }
    }
}

void StrokeRect(std::vector<uint8_t>& pixels, UINT extent, float left, float top, float right,
    float bottom, float halfWidth) {
    StrokeSegment(pixels, extent, left, top, right, top, halfWidth);
    StrokeSegment(pixels, extent, left, bottom, right, bottom, halfWidth);
    StrokeSegment(pixels, extent, left, top, left, bottom, halfWidth);
    StrokeSegment(pixels, extent, right, top, right, bottom, halfWidth);
}

void FillRectCoverage(std::vector<uint8_t>& pixels, UINT extent, float left, float top, float right,
    float bottom) {
    const int x0 = std::max(0, static_cast<int>(std::floor(left)));
    const int y0 = std::max(0, static_cast<int>(std::floor(top)));
    const int x1 = std::min(static_cast<int>(extent), static_cast<int>(std::ceil(right)));
    const int y1 = std::min(static_cast<int>(extent), static_cast<int>(std::ceil(bottom)));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            StampCoverage(pixels, extent, x, y, 1.0F);
        }
    }
}

std::vector<uint8_t> RasterizeOverflow(UINT extent) {
    std::vector<uint8_t> pixels(static_cast<size_t>(extent) * extent * 4U, 0);
    const float c = static_cast<float>(extent) * 0.5F;
    const float stroke = std::max(3.0F, static_cast<float>(extent) * 0.09F);
    const float rise = static_cast<float>(extent) * 0.16F;
    const float spread = static_cast<float>(extent) * 0.20F;
    StrokeSegment(pixels, extent, c - spread, c + rise * 0.35F, c, c - rise, stroke);
    StrokeSegment(pixels, extent, c + spread, c + rise * 0.35F, c, c - rise, stroke);
    return pixels;
}

std::vector<uint8_t> RasterizeSun(UINT extent) {
    std::vector<uint8_t> pixels(static_cast<size_t>(extent) * extent * 4U, 0);
    const float c = static_cast<float>(extent) * 0.5F;
    const float stroke = std::max(1.15F, static_cast<float>(extent) * 0.045F);
    FillCircle(pixels, extent, c, c, static_cast<float>(extent) * 0.14F);
    StrokeCircle(pixels, extent, c, c, static_cast<float>(extent) * 0.22F, stroke);
    const float inner = static_cast<float>(extent) * 0.30F;
    const float outer = static_cast<float>(extent) * 0.40F;
    for (int spoke = 0; spoke < 8; ++spoke) {
        const float angle = static_cast<float>(spoke) * 3.14159265F / 4.0F;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        StrokeSegment(pixels, extent, c + cosine * inner, c + sine * inner, c + cosine * outer,
            c + sine * outer, stroke);
    }
    return pixels;
}

std::vector<uint8_t> RasterizeWifi(UINT extent, int bars, bool connected) {
    std::vector<uint8_t> pixels(static_cast<size_t>(extent) * extent * 4U, 0);
    const float cx = static_cast<float>(extent) * 0.5F;
    const float cy = static_cast<float>(extent) * 0.62F;
    const float stroke = std::max(1.2F, static_cast<float>(extent) * 0.048F);
    FillCircle(pixels, extent, cx, cy, std::max(1.6F, static_cast<float>(extent) * 0.055F));
    const float radii[] = {extent * 0.16F, extent * 0.28F, extent * 0.40F};
    const int shown = connected ? std::clamp(bars, 0, 3) : 3;
    for (int index = 0; index < 3; ++index) {
        if (index >= shown && connected) {
            continue;
        }
        StrokeArc(pixels, extent, cx, cy, radii[index], stroke, 45.0F, 135.0F);
    }
    if (!connected) {
        StrokeSegment(pixels, extent, extent * 0.22F, extent * 0.78F, extent * 0.78F, extent * 0.22F,
            stroke * 1.15F);
    }
    return pixels;
}

std::vector<uint8_t> RasterizeEthernet(UINT extent) {
    std::vector<uint8_t> pixels(static_cast<size_t>(extent) * extent * 4U, 0);
    const float stroke = std::max(1.15F, static_cast<float>(extent) * 0.048F);
    const float left = extent * 0.22F;
    const float right = extent * 0.78F;
    const float top = extent * 0.28F;
    const float bottom = extent * 0.58F;
    StrokeRect(pixels, extent, left, top, right, bottom, stroke);
    StrokeSegment(pixels, extent, extent * 0.50F, bottom, extent * 0.50F, extent * 0.72F, stroke);
    StrokeSegment(pixels, extent, extent * 0.34F, extent * 0.72F, extent * 0.66F, extent * 0.72F,
        stroke);
    return pixels;
}

std::vector<uint8_t> RasterizeVolume(UINT extent, bool muted, float level) {
    std::vector<uint8_t> pixels(static_cast<size_t>(extent) * extent * 4U, 0);
    const float stroke = std::max(1.15F, static_cast<float>(extent) * 0.048F);
    const float bodyLeft = extent * 0.18F;
    const float bodyRight = extent * 0.38F;
    const float bodyTop = extent * 0.38F;
    const float bodyBottom = extent * 0.62F;
    StrokeSegment(pixels, extent, bodyLeft, bodyTop, bodyRight, bodyTop, stroke);
    StrokeSegment(pixels, extent, bodyLeft, bodyBottom, bodyRight, bodyBottom, stroke);
    StrokeSegment(pixels, extent, bodyLeft, bodyTop, bodyLeft, bodyBottom, stroke);
    StrokeSegment(pixels, extent, bodyRight, bodyTop, extent * 0.54F, extent * 0.26F, stroke);
    StrokeSegment(pixels, extent, bodyRight, bodyBottom, extent * 0.54F, extent * 0.74F, stroke);
    StrokeSegment(pixels, extent, extent * 0.54F, extent * 0.26F, extent * 0.54F, extent * 0.74F,
        stroke);
    if (muted) {
        StrokeSegment(pixels, extent, extent * 0.64F, extent * 0.36F, extent * 0.84F, extent * 0.64F,
            stroke);
        StrokeSegment(pixels, extent, extent * 0.84F, extent * 0.36F, extent * 0.64F, extent * 0.64F,
            stroke);
        return pixels;
    }
    if (level > 0.05F) {
        StrokeArc(pixels, extent, extent * 0.52F, extent * 0.50F, extent * 0.16F, stroke, -50.0F,
            50.0F);
    }
    if (level > 0.45F) {
        StrokeArc(pixels, extent, extent * 0.52F, extent * 0.50F, extent * 0.28F, stroke, -55.0F,
            55.0F);
    }
    return pixels;
}

std::vector<uint8_t> RasterizeBattery(UINT extent, bool charging, int percent) {
    std::vector<uint8_t> pixels(static_cast<size_t>(extent) * extent * 4U, 0);
    const float stroke = std::max(1.1F, static_cast<float>(extent) * 0.045F);
    const float left = extent * 0.18F;
    const float right = extent * 0.74F;
    const float top = extent * 0.34F;
    const float bottom = extent * 0.66F;
    StrokeRect(pixels, extent, left, top, right, bottom, stroke);
    StrokeSegment(pixels, extent, right + extent * 0.02F, extent * 0.42F, right + extent * 0.02F,
        extent * 0.58F, stroke * 1.2F);
    StrokeSegment(pixels, extent, right + extent * 0.02F, extent * 0.42F, right + extent * 0.08F,
        extent * 0.42F, stroke);
    StrokeSegment(pixels, extent, right + extent * 0.02F, extent * 0.58F, right + extent * 0.08F,
        extent * 0.58F, stroke);
    StrokeSegment(pixels, extent, right + extent * 0.08F, extent * 0.42F, right + extent * 0.08F,
        extent * 0.58F, stroke);
    const float inset = stroke * 2.2F;
    const float fillRight =
        left + inset + (right - left - inset * 2.0F) * (std::clamp(percent, 8, 100) / 100.0F);
    FillRectCoverage(pixels, extent, left + inset, top + inset, fillRight, bottom - inset);
    if (charging) {
        const float cx = (left + right) * 0.5F;
        StrokeSegment(pixels, extent, cx + extent * 0.04F, top + extent * 0.04F, cx - extent * 0.02F,
            (top + bottom) * 0.5F, stroke);
        StrokeSegment(pixels, extent, cx - extent * 0.02F, (top + bottom) * 0.5F, cx + extent * 0.04F,
            bottom - extent * 0.04F, stroke);
    }
    return pixels;
}

std::vector<uint8_t> RasterizeAcPower(UINT extent) {
    std::vector<uint8_t> pixels(static_cast<size_t>(extent) * extent * 4U, 0);
    const float stroke = std::max(1.15F, static_cast<float>(extent) * 0.048F);
    StrokeRect(pixels, extent, extent * 0.30F, extent * 0.22F, extent * 0.70F, extent * 0.58F,
        stroke);
    StrokeSegment(pixels, extent, extent * 0.40F, extent * 0.58F, extent * 0.40F, extent * 0.72F,
        stroke);
    StrokeSegment(pixels, extent, extent * 0.60F, extent * 0.58F, extent * 0.60F, extent * 0.72F,
        stroke);
    return pixels;
}

struct SavedEndpointBalance {
    std::mutex mutex;
    std::wstring deviceId;
    audio_policy::SavedChannelBalance balance;
};

SavedEndpointBalance& EndpointBalanceState() {
    static SavedEndpointBalance state;
    return state;
}

IAudioEndpointVolume* OpenEndpointVolume(bool* outIsBluetooth = nullptr,
    std::wstring* outDeviceId = nullptr) {
    if (outIsBluetooth != nullptr) {
        *outIsBluetooth = false;
    }
    if (outDeviceId != nullptr) {
        outDeviceId->clear();
    }
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator))) ||
        enumerator == nullptr) {
        return nullptr;
    }
    IMMDevice* device = nullptr;
    IAudioEndpointVolume* volume = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) &&
        device != nullptr) {
        if (outIsBluetooth != nullptr) {
            *outIsBluetooth = audio_policy::IsBluetoothEndpoint(device);
        }
        if (outDeviceId != nullptr) {
            *outDeviceId = audio_policy::EndpointDeviceId(device);
        }
        static_cast<void>(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&volume)));
        device->Release();
    }
    enumerator->Release();
    return volume;
}

bool WriteMasterVolumePreservingOrRepairingBalance(IAudioEndpointVolume* volume, float level,
    bool isBluetooth, const std::wstring& deviceId) {
    if (volume == nullptr) {
        return false;
    }
    level = std::clamp(level, 0.0F, 1.0F);
    UINT channelCount = 0;
    std::array<float, audio_policy::kMaxAudioChannels> before{};
    size_t tracked = 0;
    if (SUCCEEDED(volume->GetChannelCount(&channelCount)) && channelCount >= 2) {
        tracked = (std::min)(static_cast<size_t>(channelCount), audio_policy::kMaxAudioChannels);
        for (size_t i = 0; i < tracked; ++i) {
            if (FAILED(volume->GetChannelVolumeLevelScalar(static_cast<UINT>(i), &before[i]))) {
                tracked = 0;
                break;
            }
        }
    }

    std::array<float, audio_policy::kMaxAudioChannels> targets{};
    if (tracked >= 2) {
        SavedEndpointBalance& state = EndpointBalanceState();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.deviceId != deviceId) {
            state.deviceId = deviceId;
            state.balance = {};
        }
        audio_policy::ComputeChannelVolumeTargets(
            level, isBluetooth, before.data(), tracked, state.balance, targets.data());
    }

    if (FAILED(volume->SetMasterVolumeLevelScalar(level, &audio_policy::kVolumeEventContext))) {
        return false;
    }
    volume->SetMute(FALSE, &audio_policy::kVolumeEventContext);

    for (size_t i = 0; i < tracked; ++i) {
        float after = targets[i];
        if (SUCCEEDED(volume->GetChannelVolumeLevelScalar(static_cast<UINT>(i), &after)) &&
            std::fabs(after - targets[i]) > audio_policy::kBluetoothBalanceTolerance) {
            static_cast<void>(volume->SetChannelVolumeLevelScalar(
                static_cast<UINT>(i), targets[i], &audio_policy::kVolumeEventContext));
        }
    }
    return true;
}

void QueryVolume(TrayStatus& status) {
    bool isBluetooth = false;
    IAudioEndpointVolume* volume = OpenEndpointVolume(&isBluetooth, nullptr);
    if (volume == nullptr) {
        return;
    }
    BOOL muted = FALSE;
    float level = 0.0F;
    if (SUCCEEDED(volume->GetMute(&muted))) {
        status.volumeMuted = muted != FALSE;
    }
    if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level))) {
        status.volumeLevel = std::clamp(level, 0.0F, 1.0F);
    }
    if (isBluetooth) {
        audio_policy::RepairBluetoothBalance(volume, &status.volumeLevel);
    }
    volume->Release();
}

std::wstring SsidToString(const DOT11_SSID& ssid) {
    if (ssid.uSSIDLength == 0 || ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH) {
        return {};
    }
    const char* bytes = reinterpret_cast<const char*>(ssid.ucSSID);
    const int length = static_cast<int>(ssid.uSSIDLength);
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, length, nullptr, 0);
    UINT codePage = CP_UTF8;
    if (needed <= 0) {
        needed = MultiByteToWideChar(CP_ACP, 0, bytes, length, nullptr, 0);
        codePage = CP_ACP;
    }
    if (needed <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(codePage, 0, bytes, length, text.data(), needed);
    return text;
}

bool QueryWifi(TrayStatus& status) {
    HANDLE client = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS || client == nullptr) {
        return false;
    }
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    bool connected = false;
    if (WlanEnumInterfaces(client, nullptr, &interfaces) == ERROR_SUCCESS && interfaces != nullptr) {
        for (DWORD index = 0; index < interfaces->dwNumberOfItems; ++index) {
            const WLAN_INTERFACE_INFO& info = interfaces->InterfaceInfo[index];
            if (info.isState == wlan_interface_state_not_ready) {
                continue;
            }
            status.wifiRadioOn = true;
            if (info.isState != wlan_interface_state_connected) {
                continue;
            }
            connected = true;
            DWORD size = sizeof(WLAN_CONNECTION_ATTRIBUTES);
            PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;
            if (WlanQueryInterface(client, &info.InterfaceGuid, wlan_intf_opcode_current_connection,
                    nullptr, &size, reinterpret_cast<PVOID*>(&attributes), nullptr) == ERROR_SUCCESS &&
                attributes != nullptr) {
                const ULONG quality = attributes->wlanAssociationAttributes.wlanSignalQuality;
                status.wifiBars = quality >= 80 ? 3 : quality >= 50 ? 2 : quality >= 20 ? 1 : 0;
                status.networkName = SsidToString(attributes->wlanAssociationAttributes.dot11Ssid);
                if (status.networkName.empty() && attributes->strProfileName[0] != 0) {
                    status.networkName = attributes->strProfileName;
                }
                WlanFreeMemory(attributes);
            } else {
                status.wifiBars = 3;
            }
            break;
        }
        WlanFreeMemory(interfaces);
    }
    WlanCloseHandle(client, nullptr);
    return connected;
}

bool QueryLcdBrightness(int& percent) {
    HANDLE lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (lcd == INVALID_HANDLE_VALUE) {
        return false;
    }
    DisplayBrightness brightness{};
    DWORD written = 0;
    const BOOL ok = DeviceIoControl(lcd, kQueryDisplayBrightness, nullptr, 0, &brightness,
        sizeof(brightness), &written, nullptr);
    CloseHandle(lcd);
    if (ok == FALSE) {
        return false;
    }
    SYSTEM_POWER_STATUS power{};
    GetSystemPowerStatus(&power);
    percent = static_cast<int>(power.ACLineStatus == 0 ? brightness.dc : brightness.ac);
    percent = std::clamp(percent, 0, 100);
    return true;
}

bool WriteLcdBrightness(int percent) {
    HANDLE lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (lcd == INVALID_HANDLE_VALUE) {
        return false;
    }
    DisplayBrightness brightness{};
    brightness.policy = 0;
    brightness.ac = static_cast<UCHAR>(percent);
    brightness.dc = static_cast<UCHAR>(percent);
    DWORD written = 0;
    const BOOL ok = DeviceIoControl(lcd, kSetDisplayBrightness, &brightness, sizeof(brightness),
        nullptr, 0, &written, nullptr);
    CloseHandle(lcd);
    return ok != FALSE;
}

bool QuerySchemeBrightness(int& percent) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || scheme == nullptr) {
        return false;
    }
    DWORD ac = 0;
    DWORD dc = 0;
    const DWORD acResult = PowerReadACValueIndex(nullptr, scheme, &kVideoSubgroupGuid,
        &kVideoBrightnessGuid, &ac);
    const DWORD dcResult = PowerReadDCValueIndex(nullptr, scheme, &kVideoSubgroupGuid,
        &kVideoBrightnessGuid, &dc);
    LocalFree(scheme);
    if (acResult != ERROR_SUCCESS && dcResult != ERROR_SUCCESS) {
        return false;
    }
    SYSTEM_POWER_STATUS power{};
    GetSystemPowerStatus(&power);
    percent = static_cast<int>(power.ACLineStatus == 0 ? dc : ac);
    percent = std::clamp(percent, 0, 100);
    return true;
}

bool WriteSchemeBrightness(int percent) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || scheme == nullptr) {
        return false;
    }
    const DWORD acResult = PowerWriteACValueIndex(nullptr, scheme, &kVideoSubgroupGuid,
        &kVideoBrightnessGuid, static_cast<DWORD>(percent));
    const DWORD dcResult = PowerWriteDCValueIndex(nullptr, scheme, &kVideoSubgroupGuid,
        &kVideoBrightnessGuid, static_cast<DWORD>(percent));
    const DWORD apply = PowerSetActiveScheme(nullptr, scheme);
    LocalFree(scheme);
    return acResult == ERROR_SUCCESS || dcResult == ERROR_SUCCESS || apply == ERROR_SUCCESS;
}

BOOL CALLBACK CollectMonitorHandles(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<HMONITOR>*>(data);
    monitors->push_back(monitor);
    return TRUE;
}

// Invokes visitor(hPhysicalMonitor, min, current, max) for every external
// monitor that reports DDC/CI brightness support. Handles are closed before
// returning; no COM required.
template <typename Visitor>
void ForEachDdcBrightnessMonitor(Visitor&& visitor) {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitorHandles,
        reinterpret_cast<LPARAM>(&monitors));
    for (HMONITOR monitor : monitors) {
        DWORD count = 0;
        if (GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) == FALSE || count == 0) {
            continue;
        }
        std::vector<PHYSICAL_MONITOR> physical(count);
        if (GetPhysicalMonitorsFromHMONITOR(monitor, count, physical.data()) == FALSE) {
            continue;
        }
        for (DWORD index = 0; index < count; ++index) {
            DWORD capabilities = 0;
            DWORD colorTemps = 0;
            DWORD minimum = 0;
            DWORD current = 0;
            DWORD maximum = 100;
            if (GetMonitorCapabilities(physical[index].hPhysicalMonitor, &capabilities,
                    &colorTemps) != FALSE &&
                (capabilities & MC_CAPS_BRIGHTNESS) != 0 &&
                GetMonitorBrightness(physical[index].hPhysicalMonitor, &minimum, &current,
                    &maximum) != FALSE &&
                maximum > minimum) {
                visitor(physical[index].hPhysicalMonitor, minimum, current, maximum);
            }
        }
        DestroyPhysicalMonitors(count, physical.data());
    }
}

bool QueryDdcBrightness(int& percent) {
    int total = 0;
    int monitors = 0;
    ForEachDdcBrightnessMonitor([&](HANDLE, DWORD minimum, DWORD current, DWORD maximum) {
        total += static_cast<int>((current - minimum) * 100 / (maximum - minimum));
        ++monitors;
    });
    if (monitors == 0) {
        return false;
    }
    percent = std::clamp(total / monitors, 0, 100);
    return true;
}

bool WriteDdcBrightness(int percent) {
    bool written = false;
    ForEachDdcBrightnessMonitor([&](HANDLE monitor, DWORD minimum, DWORD, DWORD maximum) {
        const DWORD value = minimum +
            static_cast<DWORD>(
                std::lround(static_cast<double>(percent) * (maximum - minimum) / 100.0));
        if (SetMonitorBrightness(monitor, std::min(value, maximum)) != FALSE) {
            written = true;
        }
    });
    return written;
}

void QueryBrightness(TrayStatus& status, int ddcPercent) {
    // NOTE: no live DDC here — monitor round-trips stall far too long for the
    // 1 Hz UI-thread poll (measured 2.5 s once). DDC state arrives via
    // RefreshBrightnessFromDdc on worker threads; ddcPercent carries the last
    // worker-confirmed level (-1 when unknown) so this poll never overwrites
    // fresh adjustments with stale power-scheme values.
    int percent = status.brightnessPercent;
    bool known = QueryLcdBrightness(percent);
    if (!known && ddcPercent >= 0) {
        percent = ddcPercent;
        known = true;
    }
    if (known || QuerySchemeBrightness(percent)) {
        status.brightnessPercent = percent;
        status.brightnessAvailable = true;
    }
}

void QueryNetwork(TrayStatus& status) {
    if (QueryWifi(status)) {
        status.network = TrayNetworkKind::Wifi;
        if (status.networkName.empty()) {
            status.networkName = L"Wi-Fi";
        }
        return;
    }

    INetworkListManager* manager = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&manager))) ||
        manager == nullptr) {
        status.network = TrayNetworkKind::Disconnected;
        status.networkName = L"Not connected";
        return;
    }
    NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
    if (SUCCEEDED(manager->GetConnectivity(&connectivity)) &&
        (connectivity & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET |
                            NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK)) !=
            0) {
        status.network = TrayNetworkKind::Ethernet;
        status.networkName = L"Ethernet";
    } else {
        status.network = TrayNetworkKind::Disconnected;
        status.networkName = L"Not connected";
    }
    manager->Release();
}

void QueryBattery(TrayStatus& status) {
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power) == FALSE) {
        return;
    }
    status.hasBattery = (power.BatteryFlag & 128) == 0 && power.BatteryLifePercent != 255;
    status.batteryCharging = power.ACLineStatus == 1 && status.hasBattery;
    status.batteryPercent = status.hasBattery ? static_cast<int>(power.BatteryLifePercent) : 100;
}

void QueryClock(TrayStatus& status) {
    wchar_t timeText[64]{};
    wchar_t dateText[64]{};
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, timeText,
            static_cast<int>(std::size(timeText))) > 0) {
        status.timeText = timeText;
    }
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"MMM d, yyyy", dateText,
            static_cast<int>(std::size(dateText)), nullptr) > 0) {
        status.dateText = dateText;
    }
}

}  // namespace

// Dock text face: Segoe UI (Windows default) for labels and clock.
// Icons stay on Segoe Fluent/MDL2; this is text only (clock, labels, flyouts).
const wchar_t* DockTextFontFace() noexcept {
    return ResolveDockTextFontFace();
}

TrayNotifyIcon::TrayNotifyIcon(TrayNotifyIcon&& other) noexcept
    : window(other.window),
      id(other.id),
      guid(other.guid),
      icon(other.icon),
      tip(std::move(other.tip)),
      exeName(std::move(other.exeName)),
      executablePath(std::move(other.executablePath)),
      preference(other.preference),
      shellButton(other.shellButton),
      shellName(std::move(other.shellName)) {
    other.icon = nullptr;
    other.window = nullptr;
    other.shellButton = false;
}

TrayNotifyIcon& TrayNotifyIcon::operator=(TrayNotifyIcon&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (icon != nullptr) {
        DestroyIcon(icon);
    }
    window = other.window;
    id = other.id;
    guid = other.guid;
    icon = other.icon;
    tip = std::move(other.tip);
    exeName = std::move(other.exeName);
    executablePath = std::move(other.executablePath);
    preference = other.preference;
    shellButton = other.shellButton;
    shellName = std::move(other.shellName);
    other.icon = nullptr;
    other.window = nullptr;
    other.shellButton = false;
    return *this;
}

TrayNotifyIcon::~TrayNotifyIcon() {
    if (icon != nullptr) {
        DestroyIcon(icon);
        icon = nullptr;
    }
}

bool SystemTray::Refresh() {
    TrayStatus next;
    QueryClock(next);
    QueryBattery(next);
    if (GetTickCount64() < m_volumeProjectedUntil) {
        next.volumeLevel = m_status.volumeLevel;
        next.volumeMuted = m_status.volumeMuted;
    } else {
        QueryVolume(next);
    }
    QueryNetwork(next);
    QueryBrightness(next, m_ddcPercent);
    const bool changed = next.network != m_status.network || next.wifiBars != m_status.wifiBars ||
        next.wifiRadioOn != m_status.wifiRadioOn || next.volumeMuted != m_status.volumeMuted ||
        std::abs(next.volumeLevel - m_status.volumeLevel) > 0.04F ||
        next.hasBattery != m_status.hasBattery || next.batteryCharging != m_status.batteryCharging ||
        next.batteryPercent != m_status.batteryPercent ||
        // Deadband: live DDC reads can dither a level; only real moves repaint.
        std::abs(next.brightnessPercent - m_status.brightnessPercent) > 1 ||
        next.brightnessAvailable != m_status.brightnessAvailable ||
        next.networkName != m_status.networkName || next.timeText != m_status.timeText ||
        next.dateText != m_status.dateText;
    m_status = std::move(next);
    return changed;
}

bool SystemTray::RefreshForDock() {
    TrayStatus next = m_status;
    QueryClock(next);
    QueryBattery(next);
    const bool changed = next.hasBattery != m_status.hasBattery ||
        next.batteryCharging != m_status.batteryCharging ||
        next.batteryPercent != m_status.batteryPercent || next.timeText != m_status.timeText ||
        next.dateText != m_status.dateText;
    m_status.timeText = std::move(next.timeText);
    m_status.dateText = std::move(next.dateText);
    m_status.hasBattery = next.hasBattery;
    m_status.batteryCharging = next.batteryCharging;
    m_status.batteryPercent = next.batteryPercent;
    return changed;
}

const TrayStatus& SystemTray::Status() const noexcept {
    return m_status;
}

const wchar_t* SystemTray::TargetForSlot(TraySlot slot) noexcept {
    switch (slot) {
    case TraySlot::Overflow:
        return kOverflowTarget;
    case TraySlot::Network:
        return kNetworkTarget;
    case TraySlot::Volume:
        return kVolumeTarget;
    case TraySlot::Power:
        return kPowerTarget;
    case TraySlot::Clock:
        return kClockTarget;
    }
    return kOverflowTarget;
}

const wchar_t* SystemTray::LabelForSlot(TraySlot slot, const TrayStatus& status) noexcept {
    switch (slot) {
    case TraySlot::Overflow:
        return L"Quick Settings";
    case TraySlot::Network:
        return status.network == TrayNetworkKind::Ethernet
            ? L"Network"
            : status.network == TrayNetworkKind::Disconnected ? L"Not connected" : L"Network";
    case TraySlot::Volume:
        return status.volumeMuted ? L"Speakers (muted)" : L"Speakers";
    case TraySlot::Power:
        return status.hasBattery ? L"Battery" : L"Power";
    case TraySlot::Clock:
        return L"Date and time";
    }
    return L"";
}

bool SystemTray::SlotVisible(TraySlot slot, const TrayStatus& status) noexcept {
    return slot != TraySlot::Power || status.hasBattery;
}

std::vector<uint8_t> SystemTray::RasterizeGlyph(TraySlot slot, UINT extent) const {
    extent = std::max(1U, extent);
    std::vector<uint8_t> fontGlyph = RasterizeFontGlyph(extent, SymbolForSlot(slot, m_status));
    if (HasCoverage(fontGlyph)) {
        return fontGlyph;
    }
    switch (slot) {
    case TraySlot::Overflow:
        return RasterizeOverflow(extent);
    case TraySlot::Network:
        if (m_status.network == TrayNetworkKind::Ethernet) {
            return RasterizeEthernet(extent);
        }
        return RasterizeWifi(extent, m_status.wifiBars,
            m_status.network == TrayNetworkKind::Wifi);
    case TraySlot::Volume:
        return RasterizeVolume(extent, m_status.volumeMuted, m_status.volumeLevel);
    case TraySlot::Power:
        return m_status.hasBattery
            ? RasterizeBattery(extent, m_status.batteryCharging, m_status.batteryPercent)
            : RasterizeAcPower(extent);
    case TraySlot::Clock:
        break;
    }
    return {};
}

std::vector<uint8_t> SystemTray::RasterizeSymbol(wchar_t symbol, UINT extent) const {
    extent = std::max(1U, extent);
    std::vector<uint8_t> fontGlyph = RasterizeFontGlyph(extent, symbol);
    if (HasCoverage(fontGlyph)) {
        return fontGlyph;
    }
    if (symbol == L'\uE706' || symbol == L'\uE9A6') {
        return RasterizeSun(extent);
    }
    return {};
}

std::vector<uint8_t> SystemTray::RasterizeClock(UINT atlasExtent, UINT displayWidth,
    UINT displayHeight, float layoutScale) const {
    atlasExtent = std::max(1U, atlasExtent);
    displayWidth = std::max(1U, displayWidth);
    displayHeight = std::max(1U, displayHeight);
    std::vector<uint8_t> atlas(static_cast<size_t>(atlasExtent) * atlasExtent * 4U, 0);

    const UINT drawWidth = std::min(displayWidth, atlasExtent);
    const UINT drawHeight = std::min(displayHeight, atlasExtent);

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return atlas;
    }
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr) {
        return atlas;
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(drawWidth);
    header.bV5Height = -static_cast<LONG>(drawHeight);
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
        return atlas;
    }
    HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
    std::memset(bits, 0, static_cast<size_t>(drawWidth) * drawHeight * 4U);
    SetBkMode(memory, TRANSPARENT);

    int timeHeight = ClockTimePx(layoutScale);
    int dateHeight = ClockDatePx(layoutScale);
    LONG gap = ClockGapPx(layoutScale);
    if (drawWidth < displayWidth || drawHeight < displayHeight) {
        const float fit = std::min(static_cast<float>(drawWidth) / static_cast<float>(displayWidth),
            static_cast<float>(drawHeight) / static_cast<float>(displayHeight));
        timeHeight = std::max(8, static_cast<int>(std::lround(static_cast<float>(timeHeight) * fit)));
        dateHeight = std::max(8, static_cast<int>(std::lround(static_cast<float>(dateHeight) * fit)));
        gap = std::max(1L, static_cast<LONG>(std::lround(static_cast<float>(gap) * fit)));
    }

    HFONT timeFont = CreateTrayFont(timeHeight, FW_NORMAL);
    HFONT dateFont = CreateTrayFont(dateHeight, FW_NORMAL);
    HGDIOBJ previousFont = SelectObject(memory, timeFont);
    SIZE timeSize = MeasureText(memory, m_status.timeText);
    SelectObject(memory, dateFont);
    SIZE dateSize = MeasureText(memory, m_status.dateText);

    auto recreateFonts = [&]() {
        if (timeFont != nullptr) {
            DeleteObject(timeFont);
        }
        if (dateFont != nullptr) {
            DeleteObject(dateFont);
        }
        timeFont = CreateTrayFont(timeHeight, FW_NORMAL);
        dateFont = CreateTrayFont(dateHeight, FW_NORMAL);
        SelectObject(memory, timeFont);
        timeSize = MeasureText(memory, m_status.timeText);
        SelectObject(memory, dateFont);
        dateSize = MeasureText(memory, m_status.dateText);
    };

    LONG blockHeight = timeSize.cy + gap + dateSize.cy;
    LONG blockWidth = std::max(timeSize.cx, dateSize.cx);
    while ((blockHeight > static_cast<LONG>(drawHeight) || blockWidth > static_cast<LONG>(drawWidth)) &&
        (timeHeight > 8 || dateHeight > 8)) {
        if (timeHeight > dateHeight && timeHeight > 8) {
            --timeHeight;
        } else if (dateHeight > 8) {
            --dateHeight;
        } else if (timeHeight > 8) {
            --timeHeight;
        }
        recreateFonts();
        gap = std::max(1L, gap - (gap > 1 ? 1L : 0L));
        blockHeight = timeSize.cy + gap + dateSize.cy;
        blockWidth = std::max(timeSize.cx, dateSize.cx);
    }

    const LONG originY = std::max(0L, (static_cast<LONG>(drawHeight) - blockHeight) / 2L);
    const LONG contentWidth = std::min(static_cast<LONG>(drawWidth), blockWidth);
    RECT timeBounds{0, originY, contentWidth, originY + timeSize.cy};
    RECT dateBounds{0, timeBounds.bottom + gap, contentWidth,
        std::min(static_cast<LONG>(drawHeight), timeBounds.bottom + gap + dateSize.cy)};

    SelectObject(memory, timeFont);
    SetTextColor(memory, RGB(255, 255, 255));
    DrawTextW(memory, m_status.timeText.c_str(), static_cast<int>(m_status.timeText.size()),
        &timeBounds, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(memory, dateFont);
    SetTextColor(memory, RGB(255, 255, 255));
    DrawTextW(memory, m_status.dateText.c_str(), static_cast<int>(m_status.dateText.size()),
        &dateBounds, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(memory, previousFont);
    if (timeFont != nullptr) {
        DeleteObject(timeFont);
    }
    if (dateFont != nullptr) {
        DeleteObject(dateFont);
    }

    ConvertCoverageToInkPremul(static_cast<uint8_t*>(bits),
        static_cast<size_t>(drawWidth) * drawHeight * 4U);
    for (UINT y = 0; y < drawHeight; ++y) {
        std::memcpy(atlas.data() + static_cast<size_t>(y) * atlasExtent * 4U,
            static_cast<uint8_t*>(bits) + static_cast<size_t>(y) * drawWidth * 4U,
            static_cast<size_t>(drawWidth) * 4U);
    }

    SelectObject(memory, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    return atlas;
}

SIZE SystemTray::MeasureClock(float layoutScale) const {
    // Cache hit: same texts + scale measure identically. RebuildLayout calls
    // this on every invocation (clock text changes at most once a minute), so
    // this saves ~1ms of GDI font/DC churn per call.
    if (layoutScale == m_clockMeasureScale && m_status.timeText == m_clockMeasureTime &&
        m_status.dateText == m_clockMeasureDate) {
        return m_clockMeasureSize;
    }
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return {static_cast<LONG>(96 * layoutScale), static_cast<LONG>(44 * layoutScale)};
    }
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr) {
        return {static_cast<LONG>(96 * layoutScale), static_cast<LONG>(44 * layoutScale)};
    }

    const int timeHeight = ClockTimePx(layoutScale);
    const int dateHeight = ClockDatePx(layoutScale);
    HFONT timeFont = CreateTrayFont(timeHeight, FW_NORMAL);
    HFONT dateFont = CreateTrayFont(dateHeight, FW_NORMAL);
    HGDIOBJ previous = SelectObject(memory, timeFont);
    SIZE timeSize = MeasureText(memory, m_status.timeText.empty() ? L"00:00 PM" : m_status.timeText);
    SelectObject(memory, dateFont);
    SIZE dateSize = MeasureText(memory, m_status.dateText.empty() ? L"Jan 00, 0000" : m_status.dateText);
    SelectObject(memory, previous);
    if (timeFont != nullptr) {
        DeleteObject(timeFont);
    }
    if (dateFont != nullptr) {
        DeleteObject(dateFont);
    }
    DeleteDC(memory);

    const LONG gap = ClockGapPx(layoutScale);
    const LONG padding = std::max(2L, static_cast<LONG>(std::lround(4.0F * layoutScale)));
    m_clockMeasureTime = m_status.timeText;
    m_clockMeasureDate = m_status.dateText;
    m_clockMeasureScale = layoutScale;
    m_clockMeasureSize = {std::max(timeSize.cx, dateSize.cx) + padding,
        timeSize.cy + gap + dateSize.cy + padding};
    return m_clockMeasureSize;
}

std::wstring ProcessImagePath(DWORD pid) {
    if (pid == 0) {
        return {};
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (ok == FALSE) {
        return {};
    }
    path.resize(length);
    return path;
}

using tray_policy::CompactLower;
using tray_policy::FileStem;
using tray_policy::LowerCopy;
using tray_policy::PrettyTrayName;
using tray_policy::TrimEdges;

bool IsTrayHostWindowClass(const std::wstring& className) {
    const std::wstring lowered = LowerCopy(className);
    if (lowered == L"electron_notifyiconhostwindow" || lowered == L"chrome_statustraywindow" ||
        lowered == L"sunawttrayicon" || lowered == L"vanguardtraywindowclass") {
        return true;
    }
    // Catches Qt (QTrayIcon / Qt683TrayIconMessageWindowClass) and similar
    // per-toolkit hosts while excluding their menu/context hosts.
    return lowered.find(L"trayicon") != std::wstring::npos &&
        lowered.find(L"menu") == std::wstring::npos &&
        lowered.find(L"context") == std::wstring::npos;
}

HICON ExtractExeIcon(const std::wstring& exePath, int size) {
    if (exePath.empty()) {
        return nullptr;
    }
    HICON icon = nullptr;
    UINT iconId = 0;
    if (PrivateExtractIconsW(exePath.c_str(), 0, size, size, &icon, &iconId, 1, LR_DEFAULTCOLOR) >
            0 &&
        icon != nullptr) {
        return icon;
    }
    HICON largeIcon = nullptr;
    if (ExtractIconExW(exePath.c_str(), 0, &largeIcon, nullptr, 1) > 0 && largeIcon != nullptr) {
        return largeIcon;
    }
    SHFILEINFOW info{};
    const UINT flags = size >= 48 ? (SHGFI_ICON | SHGFI_LARGEICON) : (SHGFI_ICON | SHGFI_SMALLICON);
    if (SHGetFileInfoW(exePath.c_str(), 0, &info, sizeof(info), flags) != 0 &&
        info.hIcon != nullptr) {
        return info.hIcon;
    }
    return nullptr;
}

std::wstring SystemDirectoryFile(const wchar_t* fileName) {
    wchar_t directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH || fileName == nullptr || fileName[0] == 0) {
        return {};
    }
    std::wstring path(directory, length);
    if (!path.empty() && path.back() != L'\\') {
        path.push_back(L'\\');
    }
    path += fileName;
    return path;
}

// Shell button tips for icons hosted by explorer/system DLLs (explorer itself is
// ignored by AssignShellIcons). Map common tip fragments to a file that carries
// the real glyph so the System Tray list never shows an empty slot.
HICON IconForKnownTrayTip(const std::wstring& tipLower, int size) {
    struct KnownTip {
        const wchar_t* needle;
        const wchar_t* file;
    };
    static constexpr KnownTip kKnown[] = {
        {L"bluetooth", L"bthprops.cpl"},
        {L"safely remove", L"hotplug.dll"},
        {L"hardware and eject", L"hotplug.dll"},
        {L"windows security", L"SecurityHealthSystray.exe"},
        {L"virus & threat", L"SecurityHealthSystray.exe"},
        {L"network", L"ncpa.cpl"},
        {L"volume", L"mmsys.cpl"},
        {L"sound", L"mmsys.cpl"},
        {L"action center", L"ActionCenter.dll"},
        {L"location", L"locctrl.dll"},
        {L"touch keyboard", L"tabtip.exe"},
        {L"ink workspace", L"ShellExperienceHost.exe"},
    };
    for (const KnownTip& known : kKnown) {
        if (tipLower.find(known.needle) == std::wstring::npos) {
            continue;
        }
        if (HICON icon = ExtractExeIcon(SystemDirectoryFile(known.file), size)) {
            return icon;
        }
    }
    // Security Health usually lives under Program Files, not System32.
    if (tipLower.find(L"windows security") != std::wstring::npos ||
        tipLower.find(L"virus") != std::wstring::npos) {
        wchar_t programFiles[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, programFiles))) {
            std::wstring path = programFiles;
            path += L"\\Windows Defender\\SecurityHealthSystray.exe";
            if (HICON icon = ExtractExeIcon(path, size)) {
                return icon;
            }
        }
    }
    return nullptr;
}

struct MainWindowSearch {
    std::wstring exePath;
    HWND visibleFound = nullptr;
    HWND hiddenFound = nullptr;
    bool includeHidden = false;
};

BOOL CALLBACK FindMainWindowForExe(HWND window, LPARAM data) {
    auto* search = reinterpret_cast<MainWindowSearch*>(data);
    if (search->visibleFound != nullptr || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    const bool visible = IsWindowVisible(window) != FALSE;
    if (!visible && (!search->includeHidden || search->hiddenFound != nullptr)) {
        return TRUE;
    }
    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_TOOLWINDOW) != 0 || (extendedStyle & WS_EX_NOACTIVATE) != 0) {
        return TRUE;
    }
    if (!visible) {
        // Hidden candidates must look like a real titled app window, never an
        // invisible CEF/GPU/message helper window.
        if (IsWindowEnabled(window) == FALSE || GetWindowTextLengthW(window) <= 0) {
            return TRUE;
        }
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        if ((style & (WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU)) == 0) {
            return TRUE;
        }
        RECT bounds{};
        if (GetWindowRect(window, &bounds) == FALSE ||
            bounds.right - bounds.left < 240 || bounds.bottom - bounds.top < 180) {
            return TRUE;
        }
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    const std::wstring path = ProcessImagePath(pid);
    if (path.empty() || !tray_policy::MatchesTrayAppExe(path, search->exePath)) {
        return TRUE;
    }
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return TRUE;
    }
    if (visible) {
        search->visibleFound = window;
        return FALSE;
    }
    search->hiddenFound = window;
    return TRUE;
}

// Brings a window to the foreground reliably even when invoked from Hoverdock's
// WS_EX_NOACTIVATE popup (where direct SetForegroundWindow can be denied).
bool ForceForegroundWindow(HWND target, bool showWindow) {
    if (target == nullptr || IsWindow(target) == FALSE) {
        return false;
    }
    if (showWindow) {
        if (IsIconic(target) != FALSE) {
            ShowWindowAsync(target, SW_RESTORE);
        } else if (IsWindowVisible(target) == FALSE) {
            ShowWindowAsync(target, SW_SHOW);
        }
        SetWindowPos(target, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (pid != 0) {
        AllowSetForegroundWindow(pid);
    }
    BringWindowToTop(target);
    if (SetForegroundWindow(target) != FALSE) {
        return true;
    }
    const HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg != nullptr ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD curThread = GetCurrentThreadId();
    if (fgThread != 0 && fgThread != curThread &&
        AttachThreadInput(curThread, fgThread, TRUE) != FALSE) {
        BringWindowToTop(target);
        SetForegroundWindow(target);
        AttachThreadInput(curThread, fgThread, FALSE);
    }
    if (showWindow && GetForegroundWindow() != target) {
        SwitchToThisWindow(target, TRUE);
    }
    return true;
}

// Brings the app's main window forward (visible windows preferred over titled
// tray-hidden windows). Returns false when the app has no focusable window.
bool FocusTrayAppWindow(const std::wstring& exePath, bool includeHidden) {
    if (exePath.empty()) {
        return false;
    }
    MainWindowSearch search;
    search.exePath = exePath;
    search.includeHidden = includeHidden;
    EnumWindows(&FindMainWindowForExe, reinterpret_cast<LPARAM>(&search));
    HWND target = search.visibleFound != nullptr ? search.visibleFound : search.hiddenFound;
    return ForceForegroundWindow(target, true);
}

struct HostIconSearch {
    std::vector<TrayNotifyIcon> icons;
    DWORD ownPid = 0;
};
BOOL CALLBACK CollectTrayHostIcon(HWND window, LPARAM data) {
    auto* search = reinterpret_cast<HostIconSearch*>(data);
    if (GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    wchar_t className[128]{};
    if (GetClassNameW(window, className, static_cast<int>(std::size(className))) == 0 ||
        !IsTrayHostWindowClass(className)) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0 || pid == search->ownPid) {
        return TRUE;
    }
    const std::wstring exePath = ProcessImagePath(pid);
    if (exePath.empty() || LowerCopy(FileStem(exePath)) == L"explorer") {
        return TRUE;
    }
    TrayNotifyIcon icon;
    icon.window = window;
    icon.id = 1;
    icon.icon = ExtractExeIcon(exePath, 64);
    icon.tip = PrettyTrayName(exePath);
    icon.exeName = FileStem(exePath);
    icon.executablePath = exePath;
    search->icons.push_back(std::move(icon));
    return TRUE;
}

struct ClassSearch {
    const wchar_t* fragment = nullptr;
    std::vector<HWND> found;
};

BOOL CALLBACK FindChildClassFragment(HWND window, LPARAM data) {
    auto* search = reinterpret_cast<ClassSearch*>(data);
    wchar_t name[200]{};
    if (GetClassNameW(window, name, static_cast<int>(std::size(name))) > 0 &&
        std::wstring_view(name).find(search->fragment) != std::wstring_view::npos) {
        search->found.push_back(window);
    }
    return TRUE;
}

std::vector<HWND> ChildrenByClassFragment(HWND parent, const wchar_t* fragment) {
    if (parent == nullptr) {
        return {};
    }
    ClassSearch search{fragment, {}};
    EnumChildWindows(parent, FindChildClassFragment, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

std::vector<std::wstring> AlphaWords(const std::wstring& lower) {
    std::vector<std::wstring> words;
    std::wstring current;
    for (wchar_t character : lower) {
        if (std::iswalnum(character) != 0) {
            current.push_back(character);
        } else if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }
    return words;
}

int LongestCommonSubstring(std::wstring_view left, std::wstring_view right) {
    int best = 0;
    std::vector<int> previous(right.size() + 1);
    std::vector<int> current(right.size() + 1);
    for (size_t i = 1; i <= left.size(); ++i) {
        for (size_t j = 1; j <= right.size(); ++j) {
            if (left[i - 1] == right[j - 1]) {
                current[j] = previous[j - 1] + 1;
                best = std::max(best, current[j]);
            } else {
                current[j] = 0;
            }
        }
        previous.swap(current);
        std::fill(current.begin(), current.end(), 0);
    }
    return best;
}

bool IsIgnoredIconPath(const std::wstring& lowerPath) {
    static constexpr std::wstring_view kIgnored[] = {
        L"\\explorer.exe",
        L"\\svchost.exe",
        L"\\dllhost.exe",
        L"\\runtimebroker.exe",
        L"\\searchhost.exe",
        L"\\startmenuexperiencehost.exe",
        L"\\textinputhost.exe",
        L"\\applicationframehost.exe",
        L"\\shellexperiencehost.exe",
        L"\\sihost.exe",
        L"\\dwm.exe",
    };
    for (const std::wstring_view suffix : kIgnored) {
        if (lowerPath.size() >= suffix.size() &&
            lowerPath.compare(lowerPath.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
    }
    return false;
}

bool IsGenericTrayWord(const std::wstring& word) {
    static constexpr std::wstring_view kGeneric[] = {
        L"windows", L"microsoft", L"system", L"application", L"program", L"settings",
        L"devices", L"manager", L"control", L"client", L"update", L"service", L"helper",
        L"launcher", L"software", L"desktop", L"window", L"notify", L"hidden", L"icons",
        L"media", L"hardware", L"remove", L"safely", L"actions", L"needed", L"recommended",
        L"network", L"memory", L"disk",
    };
    return std::ranges::find(kGeneric, std::wstring_view(word)) != std::end(kGeneric);
}

struct IconCandidate {
    std::wstring path;
    std::wstring lowerPath;
    std::wstring compactStem;
    HWND window = nullptr;
};

int IconMatchScore(const std::wstring& compactTitle, const std::vector<std::wstring>& words,
    const IconCandidate& candidate) {
    int best = LongestCommonSubstring(compactTitle, candidate.compactStem);
    if (candidate.compactStem.size() >= 4 &&
        compactTitle.find(candidate.compactStem) != std::wstring::npos) {
        best = std::max(best, static_cast<int>(candidate.compactStem.size()) + 30);
    }
    for (const std::wstring& word : words) {
        if (word.size() >= 5 && !IsGenericTrayWord(word) &&
            candidate.lowerPath.find(word) != std::wstring::npos) {
            best = std::max(best, static_cast<int>(word.size()) + 10);
        }
    }
    return best;
}

void CollectProcessCandidates(std::vector<IconCandidate>& candidates) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == GetCurrentProcessId()) {
                continue;
            }
            std::wstring path = ProcessImagePath(entry.th32ProcessID);
            if (path.empty()) {
                continue;
            }
            std::wstring lower = LowerCopy(path);
            if (IsIgnoredIconPath(lower)) {
                continue;
            }
            const bool known = std::ranges::any_of(candidates, [&lower](const IconCandidate& candidate) {
                return candidate.lowerPath == lower;
            });
            if (known) {
                continue;
            }
            IconCandidate candidate;
            candidate.compactStem = CompactLower(FileStem(path));
            candidate.lowerPath = std::move(lower);
            candidate.path = std::move(path);
            candidates.push_back(std::move(candidate));
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
}

void AssignShellIcons(std::vector<TrayNotifyIcon>& icons) {
    if (icons.empty()) {
        return;
    }
    std::vector<IconCandidate> candidates;
    HostIconSearch hosts;
    hosts.ownPid = GetCurrentProcessId();
    EnumWindows(&CollectTrayHostIcon, reinterpret_cast<LPARAM>(&hosts));
    for (TrayNotifyIcon& host : hosts.icons) {
        if (host.executablePath.empty()) {
            continue;
        }
        std::wstring lower = LowerCopy(host.executablePath);
        if (IsIgnoredIconPath(lower)) {
            continue;
        }
        IconCandidate* existing = nullptr;
        for (IconCandidate& candidate : candidates) {
            if (candidate.lowerPath == lower) {
                existing = &candidate;
                break;
            }
        }
        if (existing == nullptr) {
            IconCandidate candidate;
            candidate.path = host.executablePath;
            candidate.lowerPath = std::move(lower);
            candidate.compactStem = CompactLower(FileStem(host.executablePath));
            candidate.window = host.window;
            candidates.push_back(std::move(candidate));
        } else if (existing->window == nullptr) {
            existing->window = host.window;
        }
    }
    CollectProcessCandidates(candidates);

    for (TrayNotifyIcon& icon : icons) {
        const std::wstring display = LowerCopy(TrimEdges(icon.tip));
        const size_t line = display.find_first_of(L"\r\n");
        const std::wstring title = line == std::wstring::npos ? display : display.substr(0, line);
        const std::wstring compact = CompactLower(title);
        const std::vector<std::wstring> words = AlphaWords(title);
        const IconCandidate* best = nullptr;
        int bestScore = 0;
        for (const IconCandidate& candidate : candidates) {
            const int score = IconMatchScore(compact, words, candidate);
            if (score > bestScore) {
                bestScore = score;
                best = &candidate;
            }
        }
        if (best != nullptr && bestScore >= 5) {
            icon.executablePath = best->path;
            icon.exeName = FileStem(best->path);
            icon.window = best->window;
            icon.icon = ExtractExeIcon(best->path, 64);
        }
        if (icon.icon == nullptr) {
            icon.icon = IconForKnownTrayTip(display, 64);
        }
    }
}

struct ShellButtonCollector {
    IUIAutomation* automation = nullptr;
    IUIAutomationCondition* buttons = nullptr;

    ~ShellButtonCollector() {
        if (buttons != nullptr) {
            buttons->Release();
        }
        if (automation != nullptr) {
            automation->Release();
        }
    }

    bool Open() {
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&automation))) ||
            automation == nullptr) {
            return false;
        }
        VARIANT className;
        VariantInit(&className);
        className.vt = VT_BSTR;
        className.bstrVal = SysAllocString(L"SystemTray.NormalButton");
        const HRESULT created =
            automation->CreatePropertyCondition(UIA_ClassNamePropertyId, className, &buttons);
        VariantClear(&className);
        return SUCCEEDED(created) && buttons != nullptr;
    }

    void Collect(HWND bridge, std::vector<TrayNotifyIcon>& icons) const {
        if (bridge == nullptr || automation == nullptr || buttons == nullptr) {
            return;
        }
        IUIAutomationElement* root = nullptr;
        if (FAILED(automation->ElementFromHandle(bridge, &root)) || root == nullptr) {
            return;
        }
        IUIAutomationElementArray* found = nullptr;
        if (FAILED(root->FindAll(TreeScope_Descendants, buttons, &found)) || found == nullptr) {
            root->Release();
            return;
        }
        int length = 0;
        found->get_Length(&length);
        for (int index = 0; index < length; ++index) {
            IUIAutomationElement* element = nullptr;
            if (FAILED(found->GetElement(index, &element)) || element == nullptr) {
                continue;
            }
            BSTR automationId = nullptr;
            BSTR name = nullptr;
            element->get_CurrentAutomationId(&automationId);
            element->get_CurrentName(&name);
            const bool notifyItem = automationId != nullptr &&
                std::wstring_view(automationId) == L"NotifyItemIcon";
            std::wstring raw = name == nullptr ? std::wstring() : std::wstring(name);
            SysFreeString(automationId);
            SysFreeString(name);
            element->Release();
            raw = TrimEdges(std::move(raw));
            if (!notifyItem || raw.empty()) {
                continue;
            }
            const std::wstring loweredRaw = LowerCopy(raw);
            const bool duplicate = std::ranges::any_of(icons, [&loweredRaw](const TrayNotifyIcon& known) {
                return LowerCopy(known.shellName) == loweredRaw;
            });
            if (duplicate) {
                continue;
            }
            TrayNotifyIcon icon;
            icon.shellButton = true;
            icon.shellName = raw;
            icon.tip = raw;
            icons.push_back(std::move(icon));
        }
        found->Release();
        root->Release();
    }
};

tray_policy::TrayIdentity ToTrayIdentity(const TrayNotifyIcon& icon) {
    return {icon.executablePath, icon.exeName, icon.tip, icon.shellName};
}

TrayNotifyIcon* FindSameTrayApp(std::vector<TrayNotifyIcon>& icons, const TrayNotifyIcon& candidate) {
    const tray_policy::TrayIdentity candId = ToTrayIdentity(candidate);
    for (TrayNotifyIcon& existing : icons) {
        if (candidate.window != nullptr && existing.window == candidate.window) {
            return &existing;
        }
        if (tray_policy::AreSameTrayApp(ToTrayIdentity(existing), candId)) {
            return &existing;
        }
    }
    return nullptr;
}

void MergeTrayIcon(TrayNotifyIcon& target, TrayNotifyIcon& source, bool preferSourceTip) {
    if (tray_policy::PreferExecutablePath(source.executablePath, target.executablePath)) {
        target.executablePath = std::move(source.executablePath);
        target.exeName = FileStem(target.executablePath);
        if (source.icon != nullptr) {
            if (target.icon != nullptr) {
                DestroyIcon(target.icon);
            }
            target.icon = source.icon;
            source.icon = nullptr;
        }
    }
    if (target.exeName.empty() && !source.exeName.empty()) {
        target.exeName = std::move(source.exeName);
    }
    if (target.window == nullptr && source.window != nullptr) {
        target.window = source.window;
        source.window = nullptr;
    }
    if (target.id == 0 && source.id != 0) {
        target.id = source.id;
    }
    if (source.shellButton) {
        target.shellButton = true;
        if (!source.shellName.empty()) {
            target.shellName = std::move(source.shellName);
        }
    }
    if (!source.tip.empty() && (target.tip.empty() || preferSourceTip)) {
        target.tip = std::move(source.tip);
    }
    if (target.icon == nullptr && source.icon != nullptr) {
        target.icon = source.icon;
        source.icon = nullptr;
    }
}

void UpsertTrayIcon(std::vector<TrayNotifyIcon>& icons, TrayNotifyIcon& candidate,
    bool preferCandidateTip) {
    if (TrayNotifyIcon* existing = FindSameTrayApp(icons, candidate); existing != nullptr) {
        MergeTrayIcon(*existing, candidate, preferCandidateTip);
    } else {
        icons.push_back(std::move(candidate));
    }
}

std::vector<TrayNotifyIcon> EnumerateShellButtons(bool assignIcons = true) {
    std::vector<TrayNotifyIcon> icons;
    ShellButtonCollector collector;
    if (!collector.Open()) {
        return icons;
    }
    for (HWND bridge : ChildrenByClassFragment(
             FindWindowW(L"Shell_TrayWnd", nullptr), L"DesktopWindowContentBridge")) {
        collector.Collect(bridge, icons);
    }
    for (HWND bridge : ChildrenByClassFragment(
             FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr),
             L"DesktopWindowContentBridge")) {
        collector.Collect(bridge, icons);
    }
    if (assignIcons) {
        AssignShellIcons(icons);
    }
    return icons;
}

// Explorer stores tray executables relative to a known folder:
// "{F38BF404-1D43-42F2-9305-67DE0B28FC23}\explorer.exe" is %WINDIR%\explorer.exe.
std::wstring ResolveKnownFolderPath(std::wstring path) {
    constexpr size_t kGuidLength = 38;
    if (path.size() <= kGuidLength || path[0] != L'{' || path[kGuidLength - 1] != L'}') {
        return path;
    }
    GUID folderId{};
    if (FAILED(CLSIDFromString(path.substr(0, kGuidLength).c_str(), &folderId))) {
        return path;
    }
    PWSTR folder = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folderId, KF_FLAG_DONT_VERIFY, nullptr, &folder)) && folder != nullptr) {
        path = std::wstring(folder) + path.substr(kGuidLength);
    }
    CoTaskMemFree(folder);
    return path;
}

std::vector<TrayNotifyIcon> EnumerateHostIcons() {
    std::vector<TrayNotifyIcon> icons;
    HostIconSearch search;
    search.ownPid = GetCurrentProcessId();
    EnumWindows(&CollectTrayHostIcon, reinterpret_cast<LPARAM>(&search));
    for (TrayNotifyIcon& scanned : search.icons) {
        const bool duplicate = std::ranges::any_of(icons, [&scanned](const TrayNotifyIcon& known) {
            return known.window != nullptr && known.window == scanned.window;
        });
        if (!duplicate) {
            icons.push_back(std::move(scanned));
        }
    }
    for (TrayNotifyIcon& icon : icons) {
        if (icon.executablePath.empty() && icon.window != nullptr) {
            DWORD pid = 0;
            GetWindowThreadProcessId(icon.window, &pid);
            icon.executablePath = ProcessImagePath(pid);
        }
        if (icon.exeName.empty() && !icon.executablePath.empty()) {
            icon.exeName = FileStem(icon.executablePath);
        }
    }
    return icons;
}

struct RunningProcessIndex {
    std::vector<std::wstring> paths;
    std::vector<std::wstring> stems;

    [[nodiscard]] bool Matches(const std::wstring& resolvedPath) const {
        const std::wstring lower = LowerCopy(resolvedPath);
        if (std::ranges::find(paths, lower) != paths.end()) {
            return true;
        }
        // Exact image path exists but is not running: do not keep a stale
        // NotifyIconSettings row alive just because some other process shares
        // the file stem (e.g. many powershell.exe hosts).
        if (GetFileAttributesW(resolvedPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return false;
        }
        // Versioned / moved install dirs leave a dead path in the registry;
        // accept a same-stem process only in that case.
        const std::wstring stem = LowerCopy(FileStem(resolvedPath));
        return !stem.empty() && std::ranges::find(stems, stem) != stems.end();
    }
};

RunningProcessIndex CollectRunningProcesses() {
    RunningProcessIndex index;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return index;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == GetCurrentProcessId()) {
                continue;
            }
            index.stems.push_back(LowerCopy(FileStem(entry.szExeFile)));
            const std::wstring path = ProcessImagePath(entry.th32ProcessID);
            if (!path.empty()) {
                index.paths.push_back(LowerCopy(path));
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return index;
}

std::wstring ReadRegString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) !=
        ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

// Win11 stores every notification icon under NotifyIconSettings. Entries linger
// after quit, so keep only those whose executable is running. This list is
// populated without the overflow flyout ever existing — unlike UIA on the
// XAML tray island, which stays empty until that flyout has been shown once.
std::vector<TrayNotifyIcon> EnumerateNotifyIconSettings(const RunningProcessIndex& running) {
    std::vector<TrayNotifyIcon> icons;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) !=
        ERROR_SUCCESS) {
        return icons;
    }
    for (DWORD index = 0;; ++index) {
        wchar_t keyName[64]{};
        DWORD keyLen = static_cast<DWORD>(std::size(keyName));
        if (RegEnumKeyExW(root, index, keyName, &keyLen, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS) {
            break;
        }
        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, keyName, 0, KEY_READ, &sub) != ERROR_SUCCESS) {
            continue;
        }
        const std::wstring rawExe = ReadRegString(sub, L"ExecutablePath");
        DWORD uid = 0;
        DWORD uidBytes = sizeof(uid);
        RegQueryValueExW(sub, L"UID", nullptr, nullptr, reinterpret_cast<BYTE*>(&uid), &uidBytes);
        std::wstring tip = ReadRegString(sub, L"IconTopTip");
        if (tip.empty()) {
            tip = ReadRegString(sub, L"InitialTooltip");
        }
        RegCloseKey(sub);
        if (rawExe.empty()) {
            continue;
        }
        std::wstring path = ResolveKnownFolderPath(rawExe);
        if (path.empty() || path[0] == L'{') {
            continue;
        }
        const std::wstring stem = LowerCopy(FileStem(path));
        if (stem == L"explorer") {
            continue;
        }
        if (!running.Matches(path)) {
            continue;
        }

        TrayNotifyIcon icon;
        icon.executablePath = std::move(path);
        icon.exeName = FileStem(icon.executablePath);
        icon.id = uid;
        tip = TrimEdges(tip);
        const size_t line = tip.find_first_of(L"\r\n");
        icon.tip = TrimEdges(line == std::wstring::npos ? tip : tip.substr(0, line));
        if (icon.tip.empty()) {
            icon.tip = PrettyTrayName(icon.executablePath);
        }
        icon.icon = ExtractExeIcon(icon.executablePath, 64);
        UpsertTrayIcon(icons, icon, false);
    }
    RegCloseKey(root);
    return icons;
}

void LinkShellButtons(std::vector<TrayNotifyIcon>& icons, std::vector<TrayNotifyIcon>& buttons) {
    std::vector<TrayNotifyIcon> unmatched;
    for (TrayNotifyIcon& button : buttons) {
        TrayNotifyIcon* best = nullptr;
        int bestScore = 0;
        for (TrayNotifyIcon& icon : icons) {
            if (icon.shellButton) {
                continue;
            }
            const int score = tray_policy::BestShellButtonMatchScore(
                ToTrayIdentity(icon), button.shellName);
            if (score > bestScore && tray_policy::IsStrongShellButtonMatch(score)) {
                best = &icon;
                bestScore = score;
            }
        }
        if (best != nullptr) {
            MergeTrayIcon(*best, button, true);
        } else {
            unmatched.push_back(std::move(button));
        }
    }
    if (!unmatched.empty()) {
        AssignShellIcons(unmatched);
        for (TrayNotifyIcon& button : unmatched) {
            UpsertTrayIcon(icons, button, true);
        }
    }
}

void DumpTrayIconsIfRequested(const std::vector<TrayNotifyIcon>& icons, double enumMs) {
    wchar_t dumpPath[MAX_PATH]{};
    const DWORD got =
        GetEnvironmentVariableW(L"HOVERDOCK_TRAY_DUMP", dumpPath, static_cast<DWORD>(std::size(dumpPath)));
    if (got == 0 || got >= std::size(dumpPath)) {
        // Optional profile line (inert unless HOVERDOCK_PROFILE is set).
        wchar_t profile[8]{};
        if (GetEnvironmentVariableW(L"HOVERDOCK_PROFILE", profile, static_cast<DWORD>(std::size(profile))) != 0) {
            wchar_t line[128]{};
            swprintf_s(line, L"tray_enum tid=%lu ms=%.2f count=%zu\n",
                GetCurrentThreadId(), enumMs, icons.size());
            OutputDebugStringW(line);
        }
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, dumpPath, L"w, ccs=UTF-8") != 0 || file == nullptr) {
        return;
    }
    fwprintf(file, L"tray_count=%zu\nenum_ms=%.2f\nworker_tid=%lu\n", icons.size(), enumMs,
        GetCurrentThreadId());
    for (const TrayNotifyIcon& icon : icons) {
        fwprintf(file, L"tip=%s\texe=%s\tid=%u\twindow=%p\tshell=%d\tshellName=%s\n",
            icon.tip.c_str(), icon.executablePath.c_str(), icon.id, static_cast<void*>(icon.window),
            icon.shellButton ? 1 : 0, icon.shellName.c_str());
    }
    fclose(file);
}

std::vector<TrayNotifyIcon> SystemTray::EnumerateNotifyIcons() const {
    // Win11 with the taskbar hidden: UIA only sees promoted buttons until the
    // overflow island has been created once, so the complete list without
    // opening the flyout comes from NotifyIconSettings filtered to running
    // processes, linked to host HWNDs and any realized shell buttons.
    const ULONGLONG started = GetTickCount64();
    const RunningProcessIndex running = CollectRunningProcesses();
    std::vector<TrayNotifyIcon> icons = EnumerateNotifyIconSettings(running);

    for (TrayNotifyIcon& host : EnumerateHostIcons()) {
        if (host.window != nullptr) {
            UpsertTrayIcon(icons, host, false);
        }
    }

    std::vector<TrayNotifyIcon> buttons = EnumerateShellButtons(false);
    LinkShellButtons(icons, buttons);

    const double enumMs = static_cast<double>(GetTickCount64() - started);
    DumpTrayIconsIfRequested(icons, enumMs);
    return icons;
}

bool SystemTray::ToggleMute() {
    IAudioEndpointVolume* volume = OpenEndpointVolume();
    if (volume == nullptr) {
        return OpenSoundMixer();
    }
    const BOOL muted = m_status.volumeMuted ? FALSE : TRUE;
    const HRESULT result = volume->SetMute(muted, &audio_policy::kVolumeEventContext);
    volume->Release();
    if (FAILED(result)) {
        return false;
    }
    m_volumeProjectedUntil = 0;
    m_status.volumeMuted = muted != FALSE;
    return true;
}

bool SystemTray::AdjustVolume(float delta) {
    bool isBluetooth = false;
    std::wstring deviceId;
    IAudioEndpointVolume* volume = OpenEndpointVolume(&isBluetooth, &deviceId);
    if (volume == nullptr) {
        return false;
    }
    float level = m_status.volumeLevel;
    if (GetTickCount64() >= m_volumeProjectedUntil &&
        FAILED(volume->GetMasterVolumeLevelScalar(&level))) {
        volume->Release();
        return false;
    }
    level = std::clamp(level + delta, 0.0F, 1.0F);
    const bool ok = WriteMasterVolumePreservingOrRepairingBalance(volume, level, isBluetooth, deviceId);
    if (ok) {
        m_status.volumeLevel = level;
        m_status.volumeMuted = false;
        m_volumeProjectedUntil = GetTickCount64() + 350ULL;
    }
    volume->Release();
    return ok;
}

void SystemTray::ProjectVolumeStatus(float level) noexcept {
    m_status.volumeLevel = std::clamp(level, 0.0F, 1.0F);
    m_status.volumeMuted = false;
    m_volumeProjectedUntil = GetTickCount64() + 350ULL;
}

bool SystemTray::WriteSystemVolumeLevel(float level) {
    bool isBluetooth = false;
    std::wstring deviceId;
    IAudioEndpointVolume* volume = OpenEndpointVolume(&isBluetooth, &deviceId);
    if (volume == nullptr) {
        return false;
    }
    const bool ok = WriteMasterVolumePreservingOrRepairingBalance(volume, level, isBluetooth, deviceId);
    volume->Release();
    return ok;
}

bool SystemTray::RefreshBrightnessFromDdc() {
    int percent = m_status.brightnessPercent;
    if (!QueryDdcBrightness(percent)) {
        return false;
    }
    m_ddcPercent = percent;
    if (percent == m_status.brightnessPercent && m_status.brightnessAvailable) {
        return false;
    }
    m_status.brightnessPercent = percent;
    m_status.brightnessAvailable = true;
    return true;
}

bool SystemTray::SetBrightnessLevel(int percent) {
    percent = std::clamp(percent, 0, 100);
    const bool written =
        WriteLcdBrightness(percent) || WriteDdcBrightness(percent) || WriteSchemeBrightness(percent);
    if (!written) {
        return false;
    }
    m_status.brightnessPercent = percent;
    m_status.brightnessAvailable = true;
    m_ddcPercent = percent;
    return true;
}

bool SystemTray::OpenNetworkPanel() {
    if (OpenShellTarget(L"ms-availablenetworks:")) {
        return true;
    }
    return OpenShellTarget(L"ms-settings:network-wifi");
}

bool SystemTray::OpenSoundMixer() {
    wchar_t systemDirectory[MAX_PATH]{};
    if (GetSystemDirectoryW(systemDirectory, MAX_PATH) > 0) {
        std::wstring path(systemDirectory);
        path += L"\\sndvol.exe";
        if (OpenShellTarget(path.c_str(), L"-f")) {
            return true;
        }
    }
    return OpenShellTarget(L"ms-settings:sound");
}

bool SystemTray::OpenPowerSettings() {
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power) != FALSE && (power.BatteryFlag & 128) == 0 &&
        power.BatteryLifePercent != 255) {
        return OpenShellTarget(L"ms-settings:batterysaver");
    }
    return OpenShellTarget(L"ms-settings:powersleep");
}

bool SystemTray::OpenDisplaySettings() {
    return OpenShellTarget(L"ms-settings:display");
}

bool SystemTray::OpenDateTimeSettings() {
    return OpenShellTarget(L"ms-settings:dateandtime");
}

bool SystemTray::OpenNotificationCenter() {
    if (SendWinChord('N')) {
        return true;
    }
    return OpenDateTimeSettings();
}

void HideShellTrayIfShown() noexcept {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray != nullptr && IsWindowVisible(tray) != FALSE) {
        SetWindowPos(tray, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    HWND overflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
    if (overflow != nullptr && IsWindowVisible(overflow) != FALSE) {
        ShowWindow(overflow, SW_HIDE);
    }
}

int FindBestShellButtonInBridge(const ShellButtonCollector& collector, HWND bridge,
    const tray_policy::TrayIdentity& identity, IUIAutomationElement*& best, HWND& bestBridge,
    int& bestScore) {
    if (bridge == nullptr || collector.automation == nullptr || collector.buttons == nullptr) {
        return 0;
    }
    IUIAutomationElement* root = nullptr;
    if (FAILED(collector.automation->ElementFromHandle(bridge, &root)) || root == nullptr) {
        return 0;
    }
    IUIAutomationElementArray* found = nullptr;
    if (FAILED(root->FindAll(TreeScope_Descendants, collector.buttons, &found)) ||
        found == nullptr) {
        root->Release();
        return 0;
    }
    int notifyItemCount = 0;
    int length = 0;
    found->get_Length(&length);
    for (int index = 0; index < length; ++index) {
        IUIAutomationElement* element = nullptr;
        if (FAILED(found->GetElement(index, &element)) || element == nullptr) {
            continue;
        }
        BSTR automationId = nullptr;
        BSTR name = nullptr;
        element->get_CurrentAutomationId(&automationId);
        element->get_CurrentName(&name);
        const bool notifyItem = automationId != nullptr &&
            std::wstring_view(automationId) == L"NotifyItemIcon";
        const std::wstring current =
            TrimEdges(name == nullptr ? std::wstring() : std::wstring(name));
        SysFreeString(automationId);
        SysFreeString(name);
        if (!notifyItem) {
            element->Release();
            continue;
        }
        ++notifyItemCount;
        const int score = tray_policy::BestShellButtonMatchScore(identity, current);
        if (score > bestScore) {
            if (best != nullptr) {
                best->Release();
            }
            best = element;
            bestBridge = bridge;
            bestScore = score;
        } else {
            element->Release();
        }
    }
    found->Release();
    root->Release();
    return notifyItemCount;
}

// On Win11 with the taskbar hidden, TopLevelWindowForOverflowXamlIsland is not
// populated until the system tray chevron has been invoked once. Invoking it
// via UIA on demand realizes the overflow buttons without showing Shell_TrayWnd.
bool InvokeOverflowChevron(const ShellButtonCollector& collector,
    const std::vector<HWND>& trayBridges) {
    if (collector.automation == nullptr) {
        return false;
    }
    struct Probe {
        PROPERTYID id;
        const wchar_t* value;
    };
    const Probe probes[] = {
        {UIA_AutomationIdPropertyId, L"SystemTrayChevron"},
        {UIA_ClassNamePropertyId, L"SystemTray.ChevronButton"},
        {UIA_AutomationIdPropertyId, L"ShowHiddenIcons"},
    };
    for (HWND trayBridge : trayBridges) {
        IUIAutomationElement* root = nullptr;
        if (FAILED(collector.automation->ElementFromHandle(trayBridge, &root)) || root == nullptr) {
            continue;
        }
        IUIAutomationElement* chevron = nullptr;
        for (const Probe& probe : probes) {
            VARIANT var{};
            VariantInit(&var);
            var.vt = VT_BSTR;
            var.bstrVal = SysAllocString(probe.value);
            IUIAutomationCondition* cond = nullptr;
            if (SUCCEEDED(collector.automation->CreatePropertyCondition(probe.id, var, &cond)) &&
                cond != nullptr) {
                root->FindFirst(TreeScope_Descendants, cond, &chevron);
                cond->Release();
            }
            VariantClear(&var);
            if (chevron != nullptr) {
                break;
            }
        }
        root->Release();
        if (chevron == nullptr) {
            continue;
        }
        bool invoked = false;
        IUIAutomationInvokePattern* invoke = nullptr;
        if (SUCCEEDED(chevron->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) &&
            invoke != nullptr) {
            invoked = SUCCEEDED(invoke->Invoke());
            invoke->Release();
        } else {
            IUIAutomationTogglePattern* toggle = nullptr;
            if (SUCCEEDED(chevron->GetCurrentPatternAs(
                    UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) &&
                toggle != nullptr) {
                invoked = SUCCEEDED(toggle->Toggle());
                toggle->Release();
            }
        }
        chevron->Release();
        if (invoked) {
            return true;
        }
    }
    return false;
}

bool PostRightClickToShellButton(IUIAutomationElement* button, HWND bridge) {
    if (button == nullptr || bridge == nullptr || IsWindow(bridge) == FALSE) {
        return false;
    }
    RECT rect{};
    if (FAILED(button->get_CurrentBoundingRectangle(&rect)) ||
        rect.right <= rect.left || rect.bottom <= rect.top) {
        return false;
    }
    POINT center{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    if (ScreenToClient(bridge, &center) == FALSE) {
        return false;
    }
    const LPARAM lParam = MAKELPARAM(center.x, center.y);
    PostMessageW(bridge, WM_RBUTTONDOWN, MK_RBUTTON, lParam);
    PostMessageW(bridge, WM_RBUTTONUP, 0, lParam);
    return true;
}

// Activates a shell notification button through UI Automation. The Windows
// tray and its overflow island stay hidden, so the dock's taskbar suppression
// is never torn down by a show/hide cycle.
bool ActivateShellButton(const tray_policy::TrayIdentity& identity, bool rightClick) {
    ShellButtonCollector collector;
    if (!collector.Open()) {
        return false;
    }
    const std::vector<HWND> trayBridges = ChildrenByClassFragment(
        FindWindowW(L"Shell_TrayWnd", nullptr), L"DesktopWindowContentBridge");
    std::vector<HWND> overflowBridges = ChildrenByClassFragment(
        FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr),
        L"DesktopWindowContentBridge");

    IUIAutomationElement* best = nullptr;
    HWND bestBridge = nullptr;
    int bestScore = 0;
    for (HWND bridge : trayBridges) {
        FindBestShellButtonInBridge(collector, bridge, identity, best, bestBridge, bestScore);
    }
    int overflowNotifyCount = 0;
    for (HWND bridge : overflowBridges) {
        overflowNotifyCount +=
            FindBestShellButtonInBridge(collector, bridge, identity, best, bestBridge, bestScore);
    }

    bool realizedOverflow = false;
    if ((best == nullptr || !tray_policy::IsStrongShellButtonMatch(bestScore)) &&
        overflowNotifyCount == 0 && !trayBridges.empty() &&
        InvokeOverflowChevron(collector, trayBridges)) {
        realizedOverflow = true;
        for (int attempt = 0; attempt < 6; ++attempt) {
            Sleep(20);
            overflowBridges = ChildrenByClassFragment(
                FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr),
                L"DesktopWindowContentBridge");
            for (HWND bridge : overflowBridges) {
                FindBestShellButtonInBridge(
                    collector, bridge, identity, best, bestBridge, bestScore);
            }
            if (best != nullptr && tray_policy::IsStrongShellButtonMatch(bestScore)) {
                break;
            }
        }
    }

    if (best == nullptr || !tray_policy::IsStrongShellButtonMatch(bestScore)) {
        if (best != nullptr) {
            best->Release();
        }
        if (realizedOverflow) {
            HideShellTrayIfShown();
        }
        return false;
    }

    AllowSetForegroundWindow(ASFW_ANY);
    bool activated = false;
    if (rightClick) {
        IUIAutomationElement3* element3 = nullptr;
        if (SUCCEEDED(best->QueryInterface(IID_PPV_ARGS(&element3))) && element3 != nullptr) {
            activated = SUCCEEDED(element3->ShowContextMenu());
            element3->Release();
        }
        if (!activated) {
            activated = PostRightClickToShellButton(best, bestBridge);
        }
    } else {
        IUIAutomationInvokePattern* invoke = nullptr;
        if (SUCCEEDED(best->GetCurrentPatternAs(
                UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) &&
            invoke != nullptr) {
            activated = SUCCEEDED(invoke->Invoke());
            invoke->Release();
        }
    }
    best->Release();
    if (realizedOverflow) {
        Sleep(40);
    }
    HideShellTrayIfShown();
    return activated;
}

bool PostTrayCallback(const TrayNotifyIcon& icon, bool rightClick) {
    if (icon.window == nullptr || IsWindow(icon.window) == FALSE) {
        return false;
    }

    // Foreground the tray owner window so Win32 TrackPopupMenu tracks and
    // dismisses properly when clicking outside.
    AllowSetForegroundWindow(ASFW_ANY);
    ForceForegroundWindow(icon.window, false);

    // Forward the click across the callback message IDs in common use. Apps
    // listen on exactly one registered message and ignore the rest, so this
    // is safe to fan out. (Explorer's own routing table is not exposed, and
    // synthetic clicks can't reach a suppressed taskbar.)
    static constexpr UINT kCallbackIds[] = {
        WM_USER, WM_APP, WM_USER + 1, WM_USER + 2, WM_APP + 1, WM_USER + 1024};
    // Qt uses its own registered message rather than WM_USER-family IDs.
    static const UINT kQtTrayMessage = RegisterWindowMessageW(L"QSystemTrayIconSysMsg");
    const UINT iconId = icon.id != 0 ? icon.id : 1;
    const LPARAM down = rightClick ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
    const LPARAM up = rightClick ? WM_RBUTTONUP : WM_LBUTTONUP;
    POINT cursor{};
    GetCursorPos(&cursor);
    const WPARAM anchor = MAKEWPARAM(static_cast<WORD>(cursor.x), static_cast<WORD>(cursor.y));
    const LPARAM v4ContextMenu =
        MAKELPARAM(WM_CONTEXTMENU, static_cast<WORD>(iconId & 0xFFFFU));
    for (const UINT callback : kCallbackIds) {
        PostMessageW(icon.window, callback, iconId, down);
        PostMessageW(icon.window, callback, iconId, up);
        if (rightClick) {
            PostMessageW(icon.window, callback, anchor, v4ContextMenu);
        }
    }
    if (kQtTrayMessage != 0) {
        PostMessageW(icon.window, kQtTrayMessage, iconId, down);
        PostMessageW(icon.window, kQtTrayMessage, iconId, up);
    }
    if (rightClick) {
        PostMessageW(icon.window, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(icon.window),
            MAKELPARAM(cursor.x, cursor.y));
    }
    return true;
}

bool LaunchTrayAppExecutable(const std::wstring& executablePath) {
    if (executablePath.empty() || IsIgnoredIconPath(LowerCopy(executablePath))) {
        return false;
    }
    if (GetFileAttributesW(executablePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    std::wstring directory;
    const size_t slash = executablePath.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0) {
        directory = executablePath.substr(0, slash);
    }
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    // ASYNCOK keeps the UI thread (which services WH_MOUSE_LL) responsive.
    launch.fMask = SEE_MASK_ASYNCOK | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"open";
    launch.lpFile = executablePath.c_str();
    launch.lpDirectory = directory.empty() ? nullptr : directory.c_str();
    launch.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&launch) != FALSE;
}

bool SystemTray::InvokeNotifyIcon(const TrayNotifyIcon& icon, UINT mouseMessage) {
    const bool rightClick = mouseMessage == WM_RBUTTONUP;
    if (!rightClick) {
        if (!icon.executablePath.empty() &&
            (FocusTrayAppWindow(icon.executablePath, true) ||
                LaunchTrayAppExecutable(icon.executablePath))) {
            return true;
        }
        return ActivateShellButton(ToTrayIdentity(icon), false) || PostTrayCallback(icon, false);
    }
    return ActivateShellButton(ToTrayIdentity(icon), true) || PostTrayCallback(icon, true);
}
