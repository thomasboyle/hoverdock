#include "SystemTray.h"
#include "DockTheme.hlsli"

#include <Unknwn.h>
#include <dwmapi.h>
#include <endpointvolume.h>
#include <highlevelmonitorconfigurationapi.h>
#include <mmdeviceapi.h>
#include <netlistmgr.h>
#include <physicalmonitorenumerationapi.h>
#include <powrprof.h>
#include <shellapi.h>
#include <wlanapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
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

enum NOTIFYITEM_PREFERENCE {
    PREFERENCE_SHOW_WHEN_ACTIVE = 0,
    PREFERENCE_SHOW_NEVER = 1,
    PREFERENCE_SHOW_ALWAYS = 2,
};

struct NOTIFYITEM {
    PWSTR pszExeName;
    PWSTR pszTip;
    HICON hIcon;
    HWND hWnd;
    NOTIFYITEM_PREFERENCE dwPreference;
    UINT uID;
    GUID guidItem;
};

struct __declspec(uuid("D782CCBA-AFB0-43F1-94DB-FDA3779EACCB")) INotificationCB : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Notify(ULONG event, NOTIFYITEM* item) = 0;
};

struct __declspec(uuid("FB852B2C-6BAD-4605-9551-F15F87830935")) ITrayNotify : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterCallback(INotificationCB* callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreference(const NOTIFYITEM* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL enabled) = 0;
};

struct __declspec(uuid("D133CE13-3537-48BA-93A7-AFCD5D2053B4")) ITrayNotifyWin8 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterCallback(INotificationCB* callback, ULONG* cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterCallback(ULONG* cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreference(const NOTIFYITEM* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE DoAction(BOOL enabled) = 0;
};

constexpr CLSID kClsidTrayNotify = {
    0x25DEAD04, 0x1EAC, 0x4911, {0x9E, 0x3A, 0xAD, 0x0A, 0x4A, 0xB5, 0x60, 0xFD}};

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

HFONT CreateTrayFont(int pixelHeight, int weight) {
    return CreateFontW(-pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

int ClockTimePx(float layoutScale) {
    return std::max(16, static_cast<int>(std::lround(20.0F * layoutScale)));
}

int ClockDatePx(float layoutScale) {
    return std::max(15, static_cast<int>(std::lround(17.0F * layoutScale)));
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

void PremultiplyFromLuminance(std::vector<uint8_t>& pixels, UINT width, UINT height) {
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4U;
            const unsigned alpha = std::max({pixels[offset], pixels[offset + 1], pixels[offset + 2]});
            pixels[offset + 3] = static_cast<uint8_t>(alpha);
        }
    }
}

std::vector<uint8_t> ScalePremultiplied(const std::vector<uint8_t>& source, UINT sourceWidth,
    UINT sourceHeight, UINT destWidth, UINT destHeight) {
    std::vector<uint8_t> dest(static_cast<size_t>(destWidth) * destHeight * 4U, 0);
    if (sourceWidth == 0 || sourceHeight == 0 || destWidth == 0 || destHeight == 0 ||
        source.size() < static_cast<size_t>(sourceWidth) * sourceHeight * 4U) {
        return dest;
    }
    for (UINT y = 0; y < destHeight; ++y) {
        const UINT sourceY0 = y * sourceHeight / destHeight;
        const UINT sourceY1 = std::max(sourceY0 + 1U, (y + 1U) * sourceHeight / destHeight);
        for (UINT x = 0; x < destWidth; ++x) {
            const UINT sourceX0 = x * sourceWidth / destWidth;
            const UINT sourceX1 = std::max(sourceX0 + 1U, (x + 1U) * sourceWidth / destWidth);
            UINT blue = 0;
            UINT green = 0;
            UINT red = 0;
            UINT alpha = 0;
            UINT count = 0;
            for (UINT sourceY = sourceY0; sourceY < sourceY1; ++sourceY) {
                for (UINT sourceX = sourceX0; sourceX < sourceX1; ++sourceX) {
                    const size_t offset = (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4U;
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
            const size_t destOffset = (static_cast<size_t>(y) * destWidth + x) * 4U;
            dest[destOffset] = static_cast<uint8_t>(blue / count);
            dest[destOffset + 1] = static_cast<uint8_t>(green / count);
            dest[destOffset + 2] = static_cast<uint8_t>(red / count);
            dest[destOffset + 3] = static_cast<uint8_t>(alpha / count);
        }
    }
    return dest;
}

IAudioEndpointVolume* OpenEndpointVolume() {
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
        static_cast<void>(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&volume)));
        device->Release();
    }
    enumerator->Release();
    return volume;
}

void QueryVolume(TrayStatus& status) {
    IAudioEndpointVolume* volume = OpenEndpointVolume();
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

TrayNotifyIcon::TrayNotifyIcon(TrayNotifyIcon&& other) noexcept
    : window(other.window),
      id(other.id),
      guid(other.guid),
      icon(other.icon),
      tip(std::move(other.tip)),
      exeName(std::move(other.exeName)),
      executablePath(std::move(other.executablePath)),
      preference(other.preference) {
    other.icon = nullptr;
    other.window = nullptr;
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
    other.icon = nullptr;
    other.window = nullptr;
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
    QueryVolume(next);
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

std::wstring SystemTray::NetworkStatusText() const {
    if (m_status.network == TrayNetworkKind::Disconnected) {
        return L"Not connected";
    }
    if (!m_status.networkName.empty()) {
        return m_status.networkName;
    }
    return m_status.network == TrayNetworkKind::Ethernet ? L"Ethernet" : L"Connected";
}

std::wstring SystemTray::VolumeStatusText() const {
    if (m_status.volumeMuted) {
        return L"Muted";
    }
    return std::to_wstring(static_cast<int>(std::lround(m_status.volumeLevel * 100.0F))) + L"%";
}

std::wstring SystemTray::BatteryStatusText() const {
    if (!m_status.hasBattery) {
        return L"Plugged in";
    }
    std::wstring text = std::to_wstring(m_status.batteryPercent) + L"%";
    if (m_status.batteryCharging) {
        text += L" charging";
    }
    return text;
}

std::wstring SystemTray::BrightnessStatusText() const {
    return std::to_wstring(m_status.brightnessPercent) + L"%";
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
    SetTextColor(memory, RGB(210, 210, 214));
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
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return {static_cast<LONG>(88 * layoutScale), static_cast<LONG>(40 * layoutScale)};
    }
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr) {
        return {static_cast<LONG>(88 * layoutScale), static_cast<LONG>(40 * layoutScale)};
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
    return {std::max(timeSize.cx, dateSize.cx) + padding,
        timeSize.cy + gap + dateSize.cy + padding};
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

std::wstring LowerCopy(std::wstring value) {
    for (wchar_t& character : value) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return value;
}

std::wstring FileStem(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = file.find_last_of(L'.');
    return dot == std::wstring::npos ? file : file.substr(0, dot);
}

// Display name for a tray host executable: strip helper suffixes (steamwebhelper
// identifies as Steam, not as a second app) and capitalize the stem.
std::wstring PrettyTrayName(const std::wstring& exePath) {
    std::wstring stem = FileStem(exePath);
    if (stem.empty()) {
        return {};
    }
    static constexpr std::wstring_view kHelperSuffixes[] = {
        L"webhelper", L"helper", L"launcher", L"updater", L"service", L"tray"};
    const std::wstring lowered = LowerCopy(stem);
    for (const std::wstring_view suffix : kHelperSuffixes) {
        if (lowered.size() > suffix.size() &&
            lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    if (!stem.empty()) {
        stem[0] = static_cast<wchar_t>(std::towupper(stem[0]));
    }
    return stem;
}

bool IsTrayHostWindowClass(const std::wstring& className) {
    const std::wstring lowered = LowerCopy(className);
    if (lowered == L"electron_notifyiconhostwindow" || lowered == L"chrome_statustraywindow" ||
        lowered == L"sunawttrayicon") {
        return true;
    }
    // Catches Qt (QTrayIcon...), Vanguard and similar per-toolkit hosts while
    // excluding their menu/context hosts.
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
    return nullptr;
}

struct MainWindowSearch {
    std::wstring exePath;
    HWND found = nullptr;
    bool includeHidden = false;
};

bool EqualExePath(const std::wstring& left, const std::wstring& right) {
    if (left.size() != right.size()) {
        return false;
    }
    return LowerCopy(left) == LowerCopy(right);
}

BOOL CALLBACK FindMainWindowForExe(HWND window, LPARAM data) {
    auto* search = reinterpret_cast<MainWindowSearch*>(data);
    if (search->found != nullptr || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    const bool visible = IsWindowVisible(window) != FALSE;
    if (!visible && !search->includeHidden) {
        return TRUE;
    }
    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_TOOLWINDOW) != 0 || (extendedStyle & WS_EX_NOACTIVATE) != 0) {
        return TRUE;
    }
    if (!visible) {
        // Hidden candidates must still look like a main window, not an aux
        // popup (e.g. Steam's 64x24 hidden vgui windows).
        RECT bounds{};
        if (GetWindowRect(window, &bounds) == FALSE ||
            bounds.right - bounds.left < 240 || bounds.bottom - bounds.top < 180) {
            return TRUE;
        }
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    std::wstring path = ProcessImagePath(pid);
    if (path.empty()) {
        return TRUE;
    }
    if (!EqualExePath(path, search->exePath)) {
        return TRUE;
    }
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return TRUE;
    }
    search->found = window;
    return FALSE;
}

// Brings the app's main window forward. With includeHidden, tray-hidden main
// windows are restored too (native single-click semantics). Returns false
// when the app has no focusable window.
bool FocusTrayAppWindow(const std::wstring& exePath, bool includeHidden) {
    if (exePath.empty()) {
        return false;
    }
    MainWindowSearch search;
    search.exePath = exePath;
    search.includeHidden = includeHidden;
    EnumWindows(&FindMainWindowForExe, reinterpret_cast<LPARAM>(&search));
    if (search.found == nullptr || IsWindow(search.found) == FALSE) {
        return false;
    }
    if (IsIconic(search.found) != FALSE) {
        ShowWindowAsync(search.found, SW_RESTORE);
    } else if (IsWindowVisible(search.found) == FALSE) {
        ShowWindowAsync(search.found, SW_SHOW);
    }
    BringWindowToTop(search.found);
    DWORD pid = 0;
    GetWindowThreadProcessId(search.found, &pid);
    if (pid != 0) {
        AllowSetForegroundWindow(pid);
    }
    return SetForegroundWindow(search.found) != FALSE;
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

std::vector<TrayNotifyIcon> SystemTray::EnumerateNotifyIcons() const {
    // NOTE: the ITrayNotify callback enumeration is intentionally not used:
    // it delivers zero icons on current Windows builds, and its reversed
    // vtable has crashed hosts (see CrashDumps). Population comes solely from
    // the apps' live notify host windows below.
    std::vector<TrayNotifyIcon> icons;

    // On builds where the callback enumeration stays silent, fall back to the
    // apps' own notify host windows (always present while the app runs).
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

    // Resolve executable paths so activation can focus app windows directly.
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

bool SystemTray::ToggleMute() {
    IAudioEndpointVolume* volume = OpenEndpointVolume();
    if (volume == nullptr) {
        return OpenSoundMixer();
    }
    const BOOL muted = m_status.volumeMuted ? FALSE : TRUE;
    const HRESULT result = volume->SetMute(muted, nullptr);
    volume->Release();
    if (FAILED(result)) {
        return false;
    }
    m_status.volumeMuted = muted != FALSE;
    return true;
}

bool SystemTray::AdjustVolume(float delta) {
    IAudioEndpointVolume* volume = OpenEndpointVolume();
    if (volume == nullptr) {
        return false;
    }
    float level = m_status.volumeLevel;
    if (FAILED(volume->GetMasterVolumeLevelScalar(&level))) {
        volume->Release();
        return false;
    }
    level = std::clamp(level + delta, 0.0F, 1.0F);
    const HRESULT result = volume->SetMasterVolumeLevelScalar(level, nullptr);
    if (SUCCEEDED(result)) {
        // Scrolled volume implies intent to hear it.
        volume->SetMute(FALSE, nullptr);
        m_status.volumeLevel = level;
        m_status.volumeMuted = false;
    }
    volume->Release();
    return SUCCEEDED(result);
}

bool SystemTray::AdjustBrightness(int deltaPercent) {
    if (!m_status.brightnessAvailable) {
        return OpenDisplaySettings();
    }
    // Step from tracked state, not a live re-read: on DDC units the read can be
    // stale or bogus (and costs a bus round-trip per call), while tracked state
    // always reflects the last confirmed or commanded level.
    const int next = std::clamp(m_status.brightnessPercent + deltaPercent, 0, 100);
    if (next == m_status.brightnessPercent) {
        return true;
    }
    const bool written =
        WriteLcdBrightness(next) || WriteDdcBrightness(next) || WriteSchemeBrightness(next);
    if (!written) {
        return OpenDisplaySettings();
    }
    m_status.brightnessPercent = next;
    m_status.brightnessAvailable = true;
    m_ddcPercent = next;
    return true;
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

bool SystemTray::OpenFlyout(TraySlot slot) {
    switch (slot) {
    case TraySlot::Network:
        return OpenNetworkPanel();
    case TraySlot::Volume:
        return OpenSoundMixer();
    case TraySlot::Power:
        return OpenPowerSettings();
    case TraySlot::Clock:
        return OpenNotificationCenter();
    case TraySlot::Overflow:
        break;
    }
    return false;
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

bool SystemTray::OpenWindowsSettings() {
    return OpenShellTarget(L"ms-settings:");
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

// Probes Explorer's icon model for a usable rect: the registered GUID first,
// then the window handle with the small icon IDs single-icon apps use.
bool FindTrayIconRect(const TrayNotifyIcon& icon, RECT& rect) {
    if (icon.window != nullptr && IsWindow(icon.window) != FALSE) {
        if (!IsEqualGUID(icon.guid, GUID_NULL)) {
            NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
            identifier.guidItem = icon.guid;
            RECT found{};
            if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &found)) &&
                found.right > found.left && found.bottom > found.top &&
                found.right - found.left <= 256 && found.bottom - found.top <= 256) {
                rect = found;
                return true;
            }
        }
        static constexpr UINT kProbeIds[] = {1, 0, 2, 100, 101, 1000};
        for (const UINT id : kProbeIds) {
            NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
            identifier.hWnd = icon.window;
            identifier.uID = id;
            RECT found{};
            if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &found)) &&
                found.right > found.left && found.bottom > found.top &&
                found.right - found.left <= 256 && found.bottom - found.top <= 256) {
                const LONG cx = found.left + (found.right - found.left) / 2;
                const LONG cy = found.top + (found.bottom - found.top) / 2;
                if (MonitorFromPoint({cx, cy}, MONITOR_DEFAULTTONULL) != nullptr) {
                    rect = found;
                    return true;
                }
            }
        }
    }
    return false;
}

bool IsForegroundFullscreen() {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    wchar_t className[64]{};
    if (GetClassNameW(foreground, className, static_cast<int>(std::size(className))) > 0 &&
        LowerCopy(className).find(L"liquidglassdock") != std::wstring::npos) {
        return false;
    }
    RECT windowRect{};
    if (GetWindowRect(foreground, &windowRect) == FALSE) {
        return false;
    }
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST), &info) == FALSE) {
        return false;
    }
    return windowRect.left <= info.rcMonitor.left && windowRect.top <= info.rcMonitor.top &&
        windowRect.right >= info.rcMonitor.right && windowRect.bottom >= info.rcMonitor.bottom;
}

void HideTaskbarAgain(HWND taskbar) noexcept {
    if (taskbar != nullptr && IsWindow(taskbar) != FALSE) {
        SetWindowPos(taskbar, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// Clicks the genuine icon by briefly showing the suppressed taskbar. Explorer
// then routes with full knowledge, so real context menus (with correct
// tracking/dismissal) appear. The dock's own taskbar monitor re-hides within
// a tick as a backstop.
bool ClickTrayIconRect(const TrayNotifyIcon& icon, bool rightClick) {
    RECT rect{};
    if (!FindTrayIconRect(icon, rect)) {
        return false;
    }
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar == nullptr || IsWindow(taskbar) == FALSE) {
        return false;
    }
    DWORD trayPid = 0;
    GetWindowThreadProcessId(taskbar, &trayPid);
    if (trayPid != 0) {
        AllowSetForegroundWindow(trayPid);
    }
    DWORD appPid = 0;
    GetWindowThreadProcessId(icon.window, &appPid);
    if (appPid != 0) {
        AllowSetForegroundWindow(appPid);
    }

    POINT saved{};
    GetCursorPos(&saved);
    SetWindowPos(taskbar, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Sleep(150);
    bool clicked = false;
    if (FindTrayIconRect(icon, rect)) {
        const LONG x = rect.left + (rect.right - rect.left) / 2;
        const LONG y = rect.top + (rect.bottom - rect.top) / 2;
        const int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        INPUT press{};
        press.type = INPUT_MOUSE;
        press.mi.dx = static_cast<LONG>(
            (static_cast<double>(x - virtualX) * 65535.0) / std::max(1, virtualW - 1));
        press.mi.dy = static_cast<LONG>(
            (static_cast<double>(y - virtualY) * 65535.0) / std::max(1, virtualH - 1));
        press.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE |
            (rightClick ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
        INPUT release = press;
        release.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
            (rightClick ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
        const std::array<INPUT, 2> inputs{press, release};
        clicked = SendInput(static_cast<UINT>(inputs.size()), const_cast<INPUT*>(inputs.data()),
                      sizeof(INPUT)) == inputs.size();
    }
    SetCursorPos(saved.x, saved.y);
    std::thread([taskbar] {
        Sleep(400);
        HideTaskbarAgain(taskbar);
    }).detach();
    return clicked;
}

bool SystemTray::InvokeNotifyIcon(const TrayNotifyIcon& icon, UINT mouseMessage) {
    const bool rightClick = mouseMessage == WM_RBUTTONUP;

    // Left click prefers focusing the app's main window directly: instant and
    // exact, restoring tray-hidden windows too (native single-click semantics).
    if (!rightClick && !icon.executablePath.empty() &&
        FocusTrayAppWindow(icon.executablePath, true)) {
        return true;
    }

    // Otherwise click the real icon: Explorer then routes with full knowledge
    // (callback IDs, foreground, menu tracking). Skipped under a fullscreen
    // foreground app, where a synthetic click could land in the game.
    if (icon.window != nullptr && IsWindow(icon.window) != FALSE && !IsForegroundFullscreen() &&
        ClickTrayIconRect(icon, rightClick)) {
        return true;
    }

    if (icon.window == nullptr || IsWindow(icon.window) == FALSE) {
        return false;
    }

    // Best-effort fallback: foreground the app's visible window when there is
    // one (menus need it to track and dismiss), then forward across the
    // callback IDs in common use.
    AllowSetForegroundWindow(ASFW_ANY);
    DWORD processId = 0;
    GetWindowThreadProcessId(icon.window, &processId);
    if (processId != 0) {
        AllowSetForegroundWindow(processId);
    }
    if (rightClick && !icon.executablePath.empty()) {
        FocusTrayAppWindow(icon.executablePath, false);
    } else {
        SetForegroundWindow(icon.window);
    }

    // Forward the click across the callback message IDs in common use. Apps
    // listen on exactly one registered message and ignore the rest, so this
    // is safe to fan out. (Explorer's own routing table is not exposed, and
    // synthetic clicks can't reach a suppressed taskbar.)
    static constexpr UINT kCallbackIds[] = {WM_USER, WM_APP, WM_USER + 1, WM_USER + 2};
    // Qt uses its own registered message rather than WM_USER-family IDs.
    static const UINT kQtTrayMessage = RegisterWindowMessageW(L"QSystemTrayIconSysMsg");
    const UINT iconId = icon.id != 0 ? icon.id : 1;
    const LPARAM down = rightClick ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
    const LPARAM up = rightClick ? WM_RBUTTONUP : WM_LBUTTONUP;
    for (const UINT callback : kCallbackIds) {
        PostMessageW(icon.window, callback, iconId, down);
        PostMessageW(icon.window, callback, iconId, up);
    }
    if (kQtTrayMessage != 0) {
        PostMessageW(icon.window, kQtTrayMessage, iconId, down);
        PostMessageW(icon.window, kQtTrayMessage, iconId, up);
    }
    if (rightClick) {
        PostMessageW(icon.window, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(icon.window),
            MAKELPARAM(-1, -1));
    }
    return true;
}
