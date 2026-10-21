// Spotlight-style Search: a liquid-glass panel centred above the dock that
// matches apps and every file/folder name (FileSearchService) as you type.
// Opened by the dock's Search tile or Win+Space.

#include "DockApp.h"
#include "DockTheme.hlsli"

#include <ShlObj.h>
#include <Shellapi.h>
#include <dwrite.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <thread>
#include <utility>

namespace dock_detail {
void CompositePremul(uint8_t* dest, int destWidth, int destHeight, int destX, int destY,
    const uint8_t* source, int sourceWidth, int sourceHeight);
void DrawFlyoutText(uint8_t* dest, int destWidth, int destHeight, RECT bounds, HFONT font,
    const std::wstring& text, UINT format, uint8_t gray, int underlineFrom = -1);
void FillRectColorPremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds, float alpha,
    uint8_t blue, uint8_t green, uint8_t red);
void FillSquircleColorPremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds,
    float radius, float alpha, uint8_t blue, uint8_t green, uint8_t red);
HFONT CreateFlyoutFont(int pixelHeight, int weight);
void SetFlyoutChromeInkForGlass(const uint8_t* pixels, int width, int height) noexcept;
IDWriteFactory* FlyoutDWriteFactory();
bool EnsureWindowCapturable(HWND window);
void ClearPopupLayeredCorners(HWND window);
LONG DockShadowMarginPx(float scale) noexcept;
DWORD_PTR SafeShellFileInfo(const wchar_t* target, DWORD fileAttributes, SHFILEINFOW* information,
    UINT flags) noexcept;
extern uint8_t g_flyoutInkR;
extern uint8_t g_flyoutInkG;
extern uint8_t g_flyoutInkB;
}  // namespace dock_detail
using namespace dock_detail;

namespace {

constexpr wchar_t kSearchClassName[] = L"LiquidGlassDockSearch";
constexpr UINT_PTR kCaretTimerId = 1;
constexpr UINT kCaretBlinkMs = 530;
// "Thinking..." dots while an AI answer has no text yet.
constexpr UINT_PTR kAiSpinnerTimerId = 2;
constexpr UINT kAiSpinnerMs = 360;
// Reverts a code block's "Copied" label.
constexpr UINT_PTR kAiCopiedTimerId = 3;
constexpr UINT kAiCopiedMs = 1600;
constexpr int kMaxVisibleRows = 8;
constexpr size_t kFileLimit = 40;
constexpr size_t kAppLimit = 6;
// Segoe MDL2 / Fluent icon code points.
constexpr wchar_t kGlyphSearch[] = L"\uE721";
constexpr wchar_t kGlyphGlobe[] = L"\uE774";
constexpr wchar_t kGlyphDrive[] = L"\uEDA2";
constexpr wchar_t kGlyphInfo[] = L"\uE946";
constexpr wchar_t kGlyphFolder[] = L"\uE8B7";
constexpr wchar_t kGlyphFile[] = L"\uE8A5";
constexpr wchar_t kGlyphApp[] = L"\uE71D";
constexpr wchar_t kGlyphChat[] = L"\uE8BD";
constexpr wchar_t kGlyphKey[] = L"\uE8D7";
constexpr wchar_t kGlyphSettings[] = L"\uE713";
constexpr wchar_t kGlyphLink[] = L"\uE71B";

int Px(float value, float scale) noexcept {
    return static_cast<int>(std::lround(value * scale));
}

struct SearchMetrics {
    float scale = 1.0F;
    LONG width = 0;
    LONG pad = 0;
    LONG barHeight = 0;
    LONG rowHeight = 0;
    LONG listTop = 0;
    LONG listBottomPad = 0;
    LONG iconSize = 0;
    LONG glyphBox = 0;
    LONG radius = 0;

    [[nodiscard]] LONG HeightFor(int rows) const noexcept {
        return rows <= 0 ? barHeight : listTop + rows * rowHeight + listBottomPad;
    }
};

SearchMetrics MetricsFor(float scale, LONG availableWidth) {
    SearchMetrics m;
    m.scale = scale;
    m.width = std::min<LONG>(Px(680.0F, scale), std::max<LONG>(Px(360.0F, scale), availableWidth));
    m.pad = Px(10.0F, scale);
    m.barHeight = Px(60.0F, scale);
    m.rowHeight = Px(54.0F, scale);
    m.listTop = m.barHeight + 1 + Px(6.0F, scale);
    m.listBottomPad = Px(8.0F, scale);
    m.iconSize = Px(34.0F, scale);
    m.glyphBox = Px(40.0F, scale);
    m.radius = Px(18.0F, scale);
    return m;
}

std::wstring Trimmed(const std::wstring& text) {
    size_t first = 0;
    size_t last = text.size();
    while (first < last && std::iswspace(text[first]) != 0) {
        ++first;
    }
    while (last > first && std::iswspace(text[last - 1]) != 0) {
        --last;
    }
    return text.substr(first, last - first);
}

std::wstring LowerExtension(const std::wstring& name) {
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 == name.size()) {
        return {};
    }
    std::wstring ext = name.substr(dot);
    for (wchar_t& c : ext) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return ext;
}

// Explorer-style "Type" column text, cached per extension (UI thread only).
std::wstring TypeName(const std::wstring& name, bool folder) {
    if (folder) {
        return L"Folder";
    }
    static std::unordered_map<std::wstring, std::wstring> cache;
    const std::wstring ext = LowerExtension(name);
    const auto found = cache.find(ext);
    if (found != cache.end()) {
        return found->second;
    }
    std::wstring type;
    SHFILEINFOW info{};
    const std::wstring probe = ext.empty() ? std::wstring(L"file") : L"file" + ext;
    if (SafeShellFileInfo(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &info,
            SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) != 0) {
        type = info.szTypeName;
    }
    if (type.empty()) {
        type = ext.empty() ? std::wstring(L"File") : ext.substr(1) + L" file";
    }
    cache.emplace(ext, type);
    return type;
}

std::wstring FormatSize(ULONGLONG bytes) {
    wchar_t text[32]{};
    if (bytes < 1024ULL) {
        swprintf_s(text, L"%llu bytes", bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        swprintf_s(text, L"%llu KB", (bytes + 1023ULL) / 1024ULL);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        swprintf_s(text, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        swprintf_s(text, L"%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return text;
}

std::wstring FormatTime(const FILETIME& time) {
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (FileTimeToSystemTime(&time, &utc) == FALSE ||
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) == FALSE) {
        return {};
    }
    wchar_t date[64]{};
    wchar_t clock[32]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, clock, 32);
    return std::wstring(date) + L", " + clock;
}

// Parent folder, shown relative to the user profile when inside it.
std::wstring FriendlyLocation(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    std::wstring parent = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    if (parent.size() == 2 && parent[1] == L':') {
        parent += L'\\';
    }
    static const std::wstring profile = [] {
        PWSTR value = nullptr;
        std::wstring result;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DONT_VERIFY, nullptr, &value)) &&
            value != nullptr) {
            result = value;
        }
        CoTaskMemFree(value);
        return result;
    }();
    if (!profile.empty() && parent.size() > profile.size() + 1 &&
        CompareStringOrdinal(parent.c_str(), static_cast<int>(profile.size()), profile.c_str(),
            static_cast<int>(profile.size()), TRUE) == CSTR_EQUAL &&
        parent[profile.size()] == L'\\') {
        return parent.substr(profile.size() + 1);
    }
    return parent;
}

// "Type · size · modified · folder" (size/date once the metadata reply lands).
std::wstring FileSubtitle(const FileSearchItem& item) {
    const bool folder = item.kind == FileSearchItem::Kind::Folder;
    std::wstring subtitle = TypeName(item.name, folder);
    if (item.haveMetadata) {
        subtitle += L" \u00B7 " + FormatSize(item.size);
        const std::wstring when = FormatTime(item.modified);
        if (!when.empty()) {
            subtitle += L" \u00B7 " + when;
        }
    }
    const std::wstring location = FriendlyLocation(item.path);
    if (!location.empty()) {
        subtitle += L" \u00B7 " + location;
    }
    return subtitle;
}

std::wstring UrlEncode(const std::wstring& text) {
    std::string utf8;
    filesearch::AppendUtf8(utf8, reinterpret_cast<const char16_t*>(text.data()), text.size());
    std::wstring out;
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    for (const char c : utf8) {
        const auto b = static_cast<unsigned char>(c);
        if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '-' ||
            b == '_' || b == '.' || b == '~') {
            out.push_back(static_cast<wchar_t>(b));
        } else if (b == ' ') {
            out.push_back(L'+');
        } else {
            out.push_back(L'%');
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 15U]);
        }
    }
    return out;
}

// Width of `text` in `font` as DirectWrite lays it out (caret placement).
float MeasureText(HFONT font, const std::wstring& text) {
    if (text.empty() || font == nullptr) {
        return 0.0F;
    }
    IDWriteFactory* factory = FlyoutDWriteFactory();
    LOGFONTW logFont{};
    if (factory == nullptr || GetObjectW(font, sizeof(logFont), &logFont) == 0) {
        return 0.0F;
    }
    const float size = static_cast<float>(logFont.lfHeight < 0 ? -logFont.lfHeight : logFont.lfHeight);
    IDWriteTextFormat* format = nullptr;
    if (FAILED(factory->CreateTextFormat(logFont.lfFaceName, nullptr,
            static_cast<DWRITE_FONT_WEIGHT>(std::clamp(logFont.lfWeight, 1L, 999L)), DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"", &format)) ||
        format == nullptr) {
        return 0.0F;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteTextLayout* layout = nullptr;
    float width = 0.0F;
    if (SUCCEEDED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 100000.0F,
            1000.0F, &layout)) &&
        layout != nullptr) {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) {
            width = metrics.widthIncludingTrailingWhitespace;
        }
        layout->Release();
    }
    format->Release();
    return width;
}

HFONT CreateGlyphFont(int pixelHeight) {
    // Segoe Fluent Icons (Windows 11) is a superset of Segoe MDL2 Assets.
    const wchar_t* face = L"Segoe Fluent Icons";
    HDC screen = GetDC(nullptr);
    if (screen != nullptr) {
        LOGFONTW probe{};
        wcscpy_s(probe.lfFaceName, face);
        probe.lfCharSet = DEFAULT_CHARSET;
        bool found = false;
        EnumFontFamiliesExW(screen, &probe,
            [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM data) -> int {
                *reinterpret_cast<bool*>(data) = true;
                return 0;
            },
            reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, screen);
        if (!found) {
            face = L"Segoe MDL2 Assets";
        }
    }
    return CreateFontW(-pixelHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

std::vector<uint8_t> DownsamplePremul(const std::vector<uint8_t>& source, UINT destExtent) {
    const auto sourceExtent =
        static_cast<UINT>(std::lround(std::sqrt(static_cast<double>(source.size() / 4U))));
    if (sourceExtent == 0 || static_cast<size_t>(sourceExtent) * sourceExtent * 4U != source.size()) {
        return {};
    }
    if (sourceExtent == destExtent) {
        return source;
    }
    std::vector<uint8_t> dest(static_cast<size_t>(destExtent) * destExtent * 4U, 0);
    for (UINT y = 0; y < destExtent; ++y) {
        const UINT y0 = y * sourceExtent / destExtent;
        const UINT y1 = std::max(y0 + 1U, (y + 1U) * sourceExtent / destExtent);
        for (UINT x = 0; x < destExtent; ++x) {
            const UINT x0 = x * sourceExtent / destExtent;
            const UINT x1 = std::max(x0 + 1U, (x + 1U) * sourceExtent / destExtent);
            UINT sum[4]{};
            UINT count = 0;
            for (UINT sy = y0; sy < y1; ++sy) {
                for (UINT sx = x0; sx < x1; ++sx) {
                    const size_t offset = (static_cast<size_t>(sy) * sourceExtent + sx) * 4U;
                    for (size_t c = 0; c < 4; ++c) {
                        sum[c] += source[offset + c];
                    }
                    ++count;
                }
            }
            const size_t offset = (static_cast<size_t>(y) * destExtent + x) * 4U;
            for (size_t c = 0; c < 4; ++c) {
                dest[offset + c] = static_cast<uint8_t>(sum[c] / count);
            }
        }
    }
    return dest;
}

// Brings `window` to the foreground even when another process owns it (the
// hotkey arrives while some other app is active). The foreground lock only
// yields to the process with the latest input, so post a no-op input first
// (as PowerToys Run does), then fall back to joining the foreground thread and
// finally to an Alt tap.
bool ForceForeground(HWND window) {
    INPUT nudge{};
    nudge.type = INPUT_MOUSE;
    SendInput(1, &nudge, sizeof(nudge));
    if (SetForegroundWindow(window) != FALSE && GetForegroundWindow() == window) {
        SetFocus(window);
        return true;
    }
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD thisThread = GetCurrentThreadId();
    const bool attached = foregroundThread != 0 && foregroundThread != thisThread &&
        AttachThreadInput(thisThread, foregroundThread, TRUE) != FALSE;
    SetForegroundWindow(window);
    BringWindowToTop(window);
    SetActiveWindow(window);
    SetFocus(window);
    if (attached) {
        AttachThreadInput(thisThread, foregroundThread, FALSE);
    }
    if (GetForegroundWindow() != window) {
        INPUT alt[2]{};
        alt[0].type = INPUT_KEYBOARD;
        alt[0].ki.wVk = VK_MENU;
        alt[1] = alt[0];
        alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, alt, sizeof(INPUT));
        SetForegroundWindow(window);
        SetFocus(window);
    }
    return GetForegroundWindow() == window;
}

void ShellOpenAsync(std::wstring target, bool reveal) {
    AllowSetForegroundWindow(ASFW_ANY);
    std::thread([target = std::move(target), reveal]() {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool done = false;
        if (reveal) {
            PIDLIST_ABSOLUTE item = ILCreateFromPathW(target.c_str());
            if (item != nullptr) {
                done = SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
                ILFree(item);
            }
        }
        if (!done) {
            ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (SUCCEEDED(init)) {
            CoUninitialize();
        }
    }).detach();
}

bool IsWordChar(wchar_t c) noexcept {
    return std::iswalnum(c) != 0 || c == L'_';
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty() || OpenClipboard(owner) == FALSE) {
        return false;
    }
    EmptyClipboard();
    bool copied = false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            if (!copied) {
                GlobalFree(memory);
            }
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return copied;
}

// "https://www.example.com/a/b" -> "example.com".
std::wstring UrlHost(const std::wstring& url) {
    size_t start = url.find(L"://");
    start = start == std::wstring::npos ? 0 : start + 3;
    const size_t end = url.find_first_of(L"/?#", start);
    std::wstring host = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    if (host.starts_with(L"www.")) {
        host.erase(0, 4);
    }
    return host;
}

std::wstring FontFace(HFONT font) {
    LOGFONTW logFont{};
    if (font == nullptr || GetObjectW(font, sizeof(logFont), &logFont) == 0) {
        return {};
    }
    return logFont.lfFaceName;
}

bool PointIn(const RECT& rect, POINT point) noexcept {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

}  // namespace

// ---- window ---------------------------------------------------------------------

LRESULT CALLBACK DockApp::SearchWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    DockApp* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<DockApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<DockApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (app == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            const HWND newlyActive = reinterpret_cast<HWND>(lParam);
            if (newlyActive != app->m_window && newlyActive != app->m_inputWindow) {
                app->CloseSearch();
            }
        } else if (app->IsSearchOpen()) {
            app->m_searchCaretVisible = true;
            app->PaintSearch();  // caret appears with focus
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (app->HandleSearchKey(wParam)) {
            return 0;
        }
        break;

    case WM_CHAR:
        app->HandleSearchChar(static_cast<wchar_t>(wParam));
        return 0;

    case WM_TIMER:
        if (wParam == kCaretTimerId) {
            app->m_searchCaretVisible = !app->m_searchCaretVisible;
            app->PaintSearch();
            return 0;
        }
        if (wParam == kAiSpinnerTimerId) {
            ++app->m_aiSpinner;
            if (app->m_searchMode == SearchMode::Ai && app->m_aiState.markdown.empty()) {
                app->PaintSearch();
            }
            return 0;
        }
        if (wParam == kAiCopiedTimerId) {
            KillTimer(window, kAiCopiedTimerId);
            if (app->m_aiView != nullptr) {
                app->m_aiView->ClearCopied();
                app->PaintSearch();
            }
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && app->m_searchMode == SearchMode::Ai) {
            POINT cursor{};
            if (GetCursorPos(&cursor) != FALSE && ScreenToClient(window, &cursor) != FALSE &&
                app->AiPointerIsLink(cursor)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const bool moved = app->m_searchMouse.x >= 0 &&
            (point.x != app->m_searchMouse.x || point.y != app->m_searchMouse.y);
        app->m_searchMouse = point;
        if (app->m_searchMode == SearchMode::Ai) {
            const int hover = app->AiHoverTarget(point);
            if (hover != app->m_aiHover) {
                app->m_aiHover = hover;
                app->PaintSearch();
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        const int row = app->SearchRowAt(point);
        if (moved && row >= 0 && row != app->m_searchSelected &&
            app->m_searchRows[static_cast<size_t>(row)].kind != SearchRow::Kind::Info) {
            app->m_searchSelected = row;
            app->PaintSearch();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (app->m_aiHover >= 0) {
            app->m_aiHover = -1;
            app->PaintSearch();
        }
        return 0;

    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (app->m_searchMode == SearchMode::Ai) {
            app->HandleAiClick(point);
            return 0;
        }
        const int row = app->SearchRowAt(point);
        if (row >= 0) {
            app->ActivateSearchRow(row, (wParam & MK_CONTROL) != 0);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (app->m_searchMode == SearchMode::Ai) {
            const float scale = static_cast<float>(app->HostDpi() == 0 ? 96U : app->HostDpi()) / 96.0F;
            const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            const int step = notches != 0 ? notches : (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1);
            app->ScrollAi(-step * Px(56.0F, scale));
            return 0;
        }
        const int rows = static_cast<int>(app->m_searchRows.size());
        const int maxScroll = std::max(0, rows - kMaxVisibleRows);
        const int step = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1;
        const int next = std::clamp(app->m_searchScroll + step, 0, maxScroll);
        if (next != app->m_searchScroll) {
            app->m_searchScroll = next;
            app->PaintSearch();
        }
        return 0;
    }

    case WM_CLOSE:
        app->CloseSearch();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool DockApp::IsSearchOpen() const noexcept {
    return m_searchWindow != nullptr && IsWindowVisible(m_searchWindow) != FALSE;
}

void DockApp::ToggleSearch() {
    if (IsSearchOpen()) {
        CloseSearch();
    } else if (!OpenSearch()) {
        Log(L"Search could not be opened.");
    }
}

bool DockApp::OpenSearch() {
    const ULONGLONG openStarted = GetTickCount64();
    if (m_visibility == VisibilityState::Hidden || m_visibility == VisibilityState::Hiding) {
        BeginShow();
    }
    HideHoverLabel();
    CloseContextMenu();
    BeginOverflowHide(false);
    CloseDockSettings();

    if (m_searchWindow == nullptr) {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = &DockApp::SearchWindowProcedure;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kSearchClassName;
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        constexpr DWORD extendedStyle =
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
        m_searchWindow = CreateWindowExW(extendedStyle, kSearchClassName, L"Search", WS_POPUP, 0, 0, 1, 1,
            m_window, nullptr, m_instance, this);
        if (m_searchWindow == nullptr) {
            return false;
        }
        EnsureWindowCapturable(m_searchWindow);
        ClearPopupLayeredCorners(m_searchWindow);
    }

    // Like Spotlight: reopen with the previous query selected.
    m_searchSelectAll = !m_searchText.empty();
    m_searchCaret = m_searchText.size();
    m_searchCaretVisible = true;
    m_searchMouse = {-1, -1};
    m_searchRows.clear();
    m_searchSelected = 0;
    m_searchScroll = 0;
    m_aiHover = -1;
    m_searchGlass.clear();
    m_searchLayerDib.contentValid = false;
    PaintSearch();
    const ULONGLONG painted = GetTickCount64();
    ShowWindow(m_searchWindow, SW_SHOW);
    const bool focused = ForceForeground(m_searchWindow);
    SetTimer(m_searchWindow, kCaretTimerId, kCaretBlinkMs, nullptr);
    Log(L"Search opened: first paint " + std::to_wstring(painted - openStarted) + L" ms, total " +
        std::to_wstring(GetTickCount64() - openStarted) + L" ms, focused=" + (focused ? L"1" : L"0"));
    m_fileSearch.SetActive(true);
    RefreshSearchApps();
    if (m_searchMode == SearchMode::Ai) {
        // An answer (or one still streaming) survives closing the panel.
        if (m_aiSession != nullptr) {
            SetTimer(m_searchWindow, kAiSpinnerTimerId, kAiSpinnerMs, nullptr);
        }
        RequestAiImages();
    } else if (m_searchMode == SearchMode::Search && !m_searchText.empty()) {
        SearchTextChanged();
    }
    return true;
}

void DockApp::CloseSearch(bool hideDockIfAway) {
    if (!IsSearchOpen() || m_searchClosing) {
        return;
    }
    m_searchClosing = true;
    KillTimer(m_searchWindow, kCaretTimerId);
    KillTimer(m_searchWindow, kAiSpinnerTimerId);
    ShowWindow(m_searchWindow, SW_HIDE);
    m_searchClosing = false;
    if (m_searchMode == SearchMode::KeyEntry || m_searchMode == SearchMode::ModelEntry) {
        ExitSearchEntry(false);
    }
    m_fileSearch.SetActive(false);
    ++m_searchRequestId;
    m_searchRows.clear();
    m_searchRowRects.clear();
    m_searchGlass.clear();
    std::vector<uint8_t>().swap(m_searchBits);

    if (!hideDockIfAway || m_visibility == VisibilityState::Hidden || m_visibility == VisibilityState::Hiding) {
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    if (cursor.y < m_visibleY + DockShadowMarginPx(static_cast<float>(HostDpi()) / 96.0F) &&
        !IsCursorOverDock(cursor)) {
        BeginHide();
    }
}

void DockApp::PositionSearch() {
    if (!IsSearchOpen()) {
        return;
    }
    POINT origin{};
    SIZE size{};
    float scale = 1.0F;
    if (SearchScreenOrigin(origin, size, scale) &&
        (origin.x != m_searchOrigin.x || origin.y != m_searchOrigin.y)) {
        PaintSearch();
    }
}

bool DockApp::SearchScreenOrigin(POINT& origin, SIZE& size, float& scale) const {
    const UINT dpi = HostDpi();
    scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
    const LONG margin = Px(12.0F, scale);
    const LONG hostWidth = m_hostBounds.right - m_hostBounds.left;
    if (hostWidth <= 0) {
        return false;
    }
    const SearchMetrics m = MetricsFor(scale, hostWidth - margin * 2);
    const int rows = m_searchMode == SearchMode::Ai ? kMaxVisibleRows
                                                    : std::min(static_cast<int>(m_searchRows.size()), kMaxVisibleRows);
    size = {m.width, m.HeightFor(rows)};
    const LONG maxHeight = m.HeightFor(kMaxVisibleRows);
    const bool haveDock = m_dockWidth > 1 && m_visibleY != 0;
    const LONG centerX = haveDock ? m_windowX + static_cast<LONG>(m_dockWidth) / 2
                                  : m_hostBounds.left + hostWidth / 2;
    const LONG dockTop = haveDock ? m_visibleY + DockShadowMarginPx(scale) : m_hostBounds.bottom;
    // The search bar stays put; results grow downward towards the dock.
    LONG y = dockTop - Px(16.0F, scale) - maxHeight;
    y = std::max(y, m_hostBounds.top + margin);
    LONG x = centerX - m.width / 2;
    x = std::clamp(x, m_hostBounds.left + margin, std::max(m_hostBounds.left + margin, m_hostBounds.right - margin - m.width));
    origin = {x, y};
    return true;
}

int DockApp::SearchRowAt(POINT client) const noexcept {
    for (size_t i = 0; i < m_searchRowRects.size(); ++i) {
        const RECT& r = m_searchRowRects[i];
        if (client.x >= r.left && client.x < r.right && client.y >= r.top && client.y < r.bottom) {
            const int row = m_searchScroll + static_cast<int>(i);
            return row < static_cast<int>(m_searchRows.size()) ? row : -1;
        }
    }
    return -1;
}

void DockApp::EnsureSearchFonts(float scale) {
    if (m_searchQueryFont != nullptr && m_searchFontScale == scale) {
        return;
    }
    DestroySearchFonts();
    m_searchQueryFont = CreateFlyoutFont(Px(24.0F, scale), FW_LIGHT);
    m_searchTitleFont = CreateFlyoutFont(Px(16.0F, scale), FW_NORMAL);
    m_searchSubFont = CreateFlyoutFont(Px(12.5F, scale), FW_NORMAL);
    m_searchGlyphFont = CreateGlyphFont(Px(20.0F, scale));
    m_searchBigGlyphFont = CreateGlyphFont(Px(22.0F, scale));
    m_searchSmallGlyphFont = CreateGlyphFont(Px(13.0F, scale));
    m_searchFontScale = scale;
}

void DockApp::DestroySearchFonts() noexcept {
    for (HFONT* font : {&m_searchQueryFont, &m_searchTitleFont, &m_searchSubFont, &m_searchGlyphFont,
             &m_searchBigGlyphFont, &m_searchSmallGlyphFont}) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    m_searchFontScale = 0.0F;
}

// ---- painting ---------------------------------------------------------------------

void DockApp::PaintSearch() {
    if (m_searchWindow == nullptr) {
        return;
    }
    POINT origin{};
    SIZE size{};
    float scale = 1.0F;
    if (!SearchScreenOrigin(origin, size, scale)) {
        return;
    }
    EnsureSearchFonts(scale);
    const SearchMetrics m = MetricsFor(scale, size.cx);
    const int width = static_cast<int>(size.cx);
    const int height = static_cast<int>(size.cy);
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4U;

    if (origin.x != m_searchGlassOrigin.x || origin.y != m_searchGlassOrigin.y) {
        m_searchGlass.clear();
        m_searchGlassOrigin = origin;
    }
    std::vector<uint8_t>& glass = m_searchGlass[size.cy];
    if (glass.size() != bytes) {
        glass.assign(bytes, 0);
        if (!TryBakePopupGlass(origin, size.cx, size.cy, glass.data(), bytes, false, false, m_searchWindow)) {
            std::fill(glass.begin(), glass.end(), static_cast<uint8_t>(0));
            FillSquircleColorPremul(glass.data(), width, height, {0, 0, size.cx, size.cy},
                static_cast<float>(m.radius), 0.94F, 46, 42, 42);
        }
    }
    m_searchBits = glass;
    uint8_t* pixels = m_searchBits.data();
    SetFlyoutChromeInkForGlass(pixels, width, height);
    const uint8_t inkB = g_flyoutInkB;
    const uint8_t inkG = g_flyoutInkG;
    const uint8_t inkR = g_flyoutInkR;

    // ---- search bar ----
    const LONG barLeft = Px(14.0F, scale);
    const RECT glyphRect{barLeft, 0, barLeft + m.glyphBox, m.barHeight};
    const wchar_t* barGlyph = m_searchMode == SearchMode::KeyEntry ? kGlyphKey
        : m_searchMode == SearchMode::ModelEntry                   ? kGlyphSettings
                                                                   : kGlyphSearch;
    DrawFlyoutText(pixels, width, height, glyphRect, m_searchBigGlyphFont, barGlyph,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE, 200);
    const LONG textLeft = glyphRect.right + Px(6.0F, scale);
    LONG textRight = width - Px(18.0F, scale);
    // Right-hand key hint: how to ask AI / leave the answer / save an entry.
    const wchar_t* hint = nullptr;
    if (m_searchMode == SearchMode::Search && !Trimmed(m_searchText).empty()) {
        hint = L"Ask AI  Ctrl+\u21B5";
    } else if (m_searchMode == SearchMode::Ai) {
        hint = L"Esc  Back to results";
    } else if (m_searchMode == SearchMode::KeyEntry || m_searchMode == SearchMode::ModelEntry) {
        hint = L"\u21B5  Save";
    }
    if (hint != nullptr) {
        const LONG hintWidth = static_cast<LONG>(std::ceil(MeasureText(m_searchSubFont, hint))) + 2;
        DrawFlyoutText(pixels, width, height, {textRight - hintWidth, 0, textRight, m.barHeight}, m_searchSubFont,
            hint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, 130);
        textRight -= hintWidth + Px(14.0F, scale);
    }
    const LONG textWidth = std::max<LONG>(1, textRight - textLeft);
    const LONG lineHeight = Px(34.0F, scale);
    const LONG lineTop = (m.barHeight - lineHeight) / 2;
    const bool focused = GetForegroundWindow() == m_searchWindow;
    // The API key is never painted: bullets stand in, one per character.
    const std::wstring masked =
        m_searchMode == SearchMode::KeyEntry ? std::wstring(m_searchText.size(), L'\u2022') : std::wstring();
    const std::wstring& shown = m_searchMode == SearchMode::KeyEntry ? masked : m_searchText;
    if (shown.empty()) {
        const wchar_t* placeholder = L"Search apps, files and folders";
        if (m_searchMode == SearchMode::KeyEntry) {
            placeholder = L"Paste your Groq API key (gsk_\u2026)";
        } else if (m_searchMode == SearchMode::ModelEntry) {
            placeholder = L"Groq model ID (default qwen/qwen3.8-27b)";
        }
        DrawFlyoutText(pixels, width, height, {textLeft, lineTop, textRight, lineTop + lineHeight},
            m_searchQueryFont, placeholder, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 120);
        if (focused && m_searchCaretVisible) {
            FillRectColorPremul(pixels, width, height,
                {textLeft, lineTop + Px(5.0F, scale), textLeft + std::max(1, Px(1.5F, scale)),
                    lineTop + lineHeight - Px(5.0F, scale)},
                1.0F, inkB, inkG, inkR);
        }
    } else {
        const float full = MeasureText(m_searchQueryFont, shown);
        const float caretX = MeasureText(m_searchQueryFont, shown.substr(0, m_searchCaret));
        // Scroll long queries so the caret stays visible.
        const float shift = std::max(0.0F, caretX - static_cast<float>(textWidth) + 4.0F);
        const LONG drawLeft = -static_cast<LONG>(std::lround(shift));
        std::vector<uint8_t> line(static_cast<size_t>(textWidth) * static_cast<size_t>(lineHeight) * 4U, 0);
        if (m_searchSelectAll) {
            FillSquircleColorPremul(line.data(), static_cast<int>(textWidth), static_cast<int>(lineHeight),
                {drawLeft, Px(3.0F, scale), drawLeft + static_cast<LONG>(std::lround(full)) + Px(3.0F, scale),
                    lineHeight - Px(3.0F, scale)},
                static_cast<float>(Px(5.0F, scale)), 0.42F, 0xD6, 0x84, 0x3A);
        }
        DrawFlyoutText(line.data(), static_cast<int>(textWidth), static_cast<int>(lineHeight),
            {drawLeft, 0, drawLeft + static_cast<LONG>(std::ceil(full)) + Px(8.0F, scale), lineHeight},
            m_searchQueryFont, shown, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 255);
        if (focused && m_searchCaretVisible && !m_searchSelectAll) {
            const LONG x = drawLeft + static_cast<LONG>(std::lround(caretX));
            FillRectColorPremul(line.data(), static_cast<int>(textWidth), static_cast<int>(lineHeight),
                {x, Px(5.0F, scale), x + std::max(1, Px(1.5F, scale)), lineHeight - Px(5.0F, scale)}, 1.0F, inkB,
                inkG, inkR);
        }
        CompositePremul(pixels, width, height, static_cast<int>(textLeft), static_cast<int>(lineTop), line.data(),
            static_cast<int>(textWidth), static_cast<int>(lineHeight));
    }

    // ---- results ----
    m_searchRowRects.clear();
    const int rows = m_searchMode == SearchMode::Ai ? 0 : static_cast<int>(m_searchRows.size());
    if (m_searchMode == SearchMode::Ai) {
        PaintSearchAi(pixels, width, height, scale, m.listTop);
    }
    if (rows > 0) {
        FillRectColorPremul(pixels, width, height, {Px(16.0F, scale), m.barHeight, width - Px(16.0F, scale),
            m.barHeight + 1}, 0.14F, inkB, inkG, inkR);
        m_searchScroll = std::clamp(m_searchScroll, 0, std::max(0, rows - kMaxVisibleRows));
        const int visible = std::min(rows - m_searchScroll, kMaxVisibleRows);
        for (int v = 0; v < visible; ++v) {
            const int index = m_searchScroll + v;
            const SearchRow& row = m_searchRows[static_cast<size_t>(index)];
            const RECT rect{m.pad, m.listTop + v * m.rowHeight, width - m.pad, m.listTop + (v + 1) * m.rowHeight};
            m_searchRowRects.push_back(rect);
            const bool selected = index == m_searchSelected;
            if (selected) {
                FillSquircleColorPremul(pixels, width, height, rect, static_cast<float>(Px(11.0F, scale)), 0.16F,
                    inkB, inkG, inkR);
            }
            // Icon (shell icon when cached, else a glyph).
            const LONG iconLeft = rect.left + Px(10.0F, scale);
            const LONG iconTop = rect.top + (m.rowHeight - m.iconSize) / 2;
            const auto icon = row.iconKey.empty() ? m_searchIcons.end() : m_searchIcons.find(row.iconKey);
            if (icon != m_searchIcons.end() && icon->second.size() ==
                    static_cast<size_t>(m.iconSize) * static_cast<size_t>(m.iconSize) * 4U) {
                CompositePremul(pixels, width, height, static_cast<int>(iconLeft), static_cast<int>(iconTop),
                    icon->second.data(), static_cast<int>(m.iconSize), static_cast<int>(m.iconSize));
            } else {
                const wchar_t* glyph = kGlyphFile;
                switch (row.kind) {
                case SearchRow::Kind::App: glyph = kGlyphApp; break;
                case SearchRow::Kind::Folder: glyph = kGlyphFolder; break;
                case SearchRow::Kind::Web: glyph = kGlyphGlobe; break;
                case SearchRow::Kind::EnableMft: glyph = kGlyphDrive; break;
                case SearchRow::Kind::Info: glyph = kGlyphInfo; break;
                case SearchRow::Kind::Link: glyph = kGlyphLink; break;
                case SearchRow::Kind::Ai: glyph = kGlyphChat; break;
                case SearchRow::Kind::File: break;
                }
                DrawFlyoutText(pixels, width, height, {iconLeft, iconTop, iconLeft + m.iconSize, iconTop + m.iconSize},
                    m_searchGlyphFont, glyph, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 220);
            }
            const LONG labelLeft = iconLeft + m.iconSize + Px(12.0F, scale);
            LONG labelRight = rect.right - Px(12.0F, scale);
            if (selected) {
                const wchar_t* action = L"Open \u21B5";
                if (row.kind == SearchRow::Kind::Web) {
                    action = L"Search \u21B5";
                } else if (row.kind == SearchRow::Kind::EnableMft) {
                    action = L"Enable \u21B5";
                } else if (row.kind == SearchRow::Kind::Ai) {
                    action = L"Ask \u21B5";
                } else if (row.kind == SearchRow::Kind::Link) {
                    action = L"Open";
                }
                const LONG hintWidth = static_cast<LONG>(std::ceil(MeasureText(m_searchSubFont, action))) + 2;
                DrawFlyoutText(pixels, width, height, {labelRight - hintWidth, rect.top, labelRight, rect.bottom},
                    m_searchSubFont, action, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, 150);
                labelRight -= hintWidth + Px(12.0F, scale);
            }
            const LONG titleHeight = Px(21.0F, scale);
            const LONG subHeight = Px(17.0F, scale);
            if (row.subtitle.empty()) {
                DrawFlyoutText(pixels, width, height, {labelLeft, rect.top, labelRight, rect.bottom}, m_searchTitleFont,
                    row.title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 250);
            } else {
                const LONG top = rect.top + (m.rowHeight - titleHeight - subHeight) / 2;
                DrawFlyoutText(pixels, width, height, {labelLeft, top, labelRight, top + titleHeight}, m_searchTitleFont,
                    row.title, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 250);
                DrawFlyoutText(pixels, width, height, {labelLeft, top + titleHeight, labelRight,
                    top + titleHeight + subHeight}, m_searchSubFont, row.subtitle,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 150);
            }
        }
    }

    m_searchOrigin = origin;
    m_searchSize = size;
    PresentLayeredBits(m_searchWindow, origin, size.cx, size.cy, m_searchBits.data(), m_searchBits.size(),
        m_searchLayerDib);
}

// ---- input ----------------------------------------------------------------------------

void DockApp::HandleSearchChar(wchar_t character) {
    if (character < 0x20 || character == 0x7F) {
        return;
    }
    if (m_searchSelectAll) {
        m_searchText.clear();
        m_searchCaret = 0;
        m_searchSelectAll = false;
    }
    m_searchText.insert(m_searchCaret, 1, character);
    ++m_searchCaret;
    SearchTextChanged();
}

bool DockApp::HandleSearchKey(WPARAM key) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    const size_t length = m_searchText.size();
    auto wordLeft = [&](size_t pos) {
        while (pos > 0 && !IsWordChar(m_searchText[pos - 1])) {
            --pos;
        }
        while (pos > 0 && IsWordChar(m_searchText[pos - 1])) {
            --pos;
        }
        return pos;
    };
    auto wordRight = [&](size_t pos) {
        while (pos < length && IsWordChar(m_searchText[pos])) {
            ++pos;
        }
        while (pos < length && !IsWordChar(m_searchText[pos])) {
            ++pos;
        }
        return pos;
    };
    // Keep surrogate pairs together.
    auto prevChar = [&](size_t pos) {
        if (pos == 0) {
            return pos;
        }
        --pos;
        if (pos > 0 && IS_LOW_SURROGATE(m_searchText[pos]) && IS_HIGH_SURROGATE(m_searchText[pos - 1])) {
            --pos;
        }
        return pos;
    };
    auto nextChar = [&](size_t pos) {
        if (pos >= length) {
            return length;
        }
        ++pos;
        if (pos < length && IS_LOW_SURROGATE(m_searchText[pos]) && IS_HIGH_SURROGATE(m_searchText[pos - 1])) {
            ++pos;
        }
        return pos;
    };
    auto moveCaret = [&](size_t pos) {
        m_searchSelectAll = false;
        m_searchCaret = pos;
        m_searchCaretVisible = true;
        PaintSearch();
    };

    const bool entry = m_searchMode == SearchMode::KeyEntry || m_searchMode == SearchMode::ModelEntry;
    const bool ai = m_searchMode == SearchMode::Ai;
    const float scale = static_cast<float>(HostDpi() == 0 ? 96U : HostDpi()) / 96.0F;
    const int page = std::max(1, static_cast<int>(m_aiViewport.bottom - m_aiViewport.top) - Px(40.0F, scale));
    switch (key) {
    case VK_ESCAPE:
        if (entry) {
            ExitSearchEntry(true);
        } else if (ai) {
            ExitAiState();
            SearchTextChanged();
        } else if (!m_searchText.empty()) {
            m_searchText.clear();
            m_searchCaret = 0;
            m_searchSelectAll = false;
            SearchTextChanged();
        } else {
            CloseSearch();
        }
        return true;
    case VK_RETURN:
        if (entry) {
            CommitSearchEntry();
        } else if (ctrl) {
            AskAi();
        } else if (ai) {
            if (m_aiNeedsKey) {
                m_aiAskAfterKey = true;
                OpenSearchEntry(SearchMode::KeyEntry);
            }
        } else {
            // Shift+Enter shows the item in File Explorer (Ctrl+click too).
            ActivateSearchRow(m_searchSelected, shift);
        }
        return true;
    case VK_UP:
        if (ai) {
            ScrollAi(-Px(40.0F, scale));
        } else if (!entry) {
            MoveSearchSelection(-1);
        }
        return true;
    case VK_DOWN:
        if (ai) {
            ScrollAi(Px(40.0F, scale));
        } else if (!entry) {
            MoveSearchSelection(1);
        }
        return true;
    case VK_TAB:
        if (!ai && !entry) {
            MoveSearchSelection(shift ? -1 : 1);
        }
        return true;
    case VK_PRIOR:
        if (ai) {
            ScrollAi(-page);
        } else if (!entry) {
            MoveSearchSelection(-kMaxVisibleRows);
        }
        return true;
    case VK_NEXT:
        if (ai) {
            ScrollAi(page);
        } else if (!entry) {
            MoveSearchSelection(kMaxVisibleRows);
        }
        return true;
    case VK_LEFT:
        moveCaret(m_searchSelectAll ? 0 : (ctrl ? wordLeft(m_searchCaret) : prevChar(m_searchCaret)));
        return true;
    case VK_RIGHT:
        moveCaret(m_searchSelectAll ? length : (ctrl ? wordRight(m_searchCaret) : nextChar(m_searchCaret)));
        return true;
    case VK_HOME:
        if (ai && ctrl) {
            ScrollAi(-1000000);
        } else {
            moveCaret(0);
        }
        return true;
    case VK_END:
        if (ai && ctrl) {
            ScrollAi(1000000);
        } else {
            moveCaret(length);
        }
        return true;
    case VK_BACK:
    case VK_DELETE: {
        if (m_searchSelectAll) {
            m_searchText.clear();
            m_searchCaret = 0;
            m_searchSelectAll = false;
        } else if (key == VK_BACK && m_searchCaret > 0) {
            const size_t from = ctrl ? wordLeft(m_searchCaret) : prevChar(m_searchCaret);
            m_searchText.erase(from, m_searchCaret - from);
            m_searchCaret = from;
        } else if (key == VK_DELETE && m_searchCaret < length) {
            const size_t to = ctrl ? wordRight(m_searchCaret) : nextChar(m_searchCaret);
            m_searchText.erase(m_searchCaret, to - m_searchCaret);
        } else {
            return true;
        }
        SearchTextChanged();
        return true;
    }
    default:
        break;
    }

    if (!ctrl) {
        return false;
    }
    if (key == 'A') {
        m_searchSelectAll = !m_searchText.empty();
        PaintSearch();
        return true;
    }
    if (key == 'V') {
        std::wstring pasted;
        if (OpenClipboard(m_searchWindow) != FALSE) {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                    pasted = text;
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        for (wchar_t& c : pasted) {
            if (c == L'\r' || c == L'\n' || c == L'\t') {
                c = L' ';
            }
        }
        std::erase_if(pasted, [](wchar_t c) { return c < 0x20; });
        if (!pasted.empty()) {
            if (m_searchSelectAll) {
                m_searchText.clear();
                m_searchCaret = 0;
                m_searchSelectAll = false;
            }
            m_searchText.insert(m_searchCaret, pasted);
            m_searchCaret += pasted.size();
            SearchTextChanged();
        }
        return true;
    }
    if (key == 'C') {
        if (m_searchMode == SearchMode::KeyEntry) {
            return true;  // never put the API key on the clipboard
        }
        if (m_searchMode == SearchMode::Ai) {
            CopyAiAnswer();
            return true;
        }
        // Copy the selected item's path (or the query).
        std::wstring text = m_searchText;
        if (m_searchSelected >= 0 && static_cast<size_t>(m_searchSelected) < m_searchRows.size() &&
            !m_searchRows[static_cast<size_t>(m_searchSelected)].path.empty()) {
            text = m_searchRows[static_cast<size_t>(m_searchSelected)].path;
        }
        CopyTextToClipboard(m_searchWindow, text);
        return true;
    }
    return false;
}

void DockApp::MoveSearchSelection(int delta) {
    const int rows = static_cast<int>(m_searchRows.size());
    if (rows == 0) {
        return;
    }
    int next = std::clamp(m_searchSelected + delta, 0, rows - 1);
    // Skip informational rows.
    while (next > 0 && m_searchRows[static_cast<size_t>(next)].kind == SearchRow::Kind::Info) {
        --next;
    }
    m_searchSelected = next;
    if (m_searchSelected < m_searchScroll) {
        m_searchScroll = m_searchSelected;
    } else if (m_searchSelected >= m_searchScroll + kMaxVisibleRows) {
        m_searchScroll = m_searchSelected - kMaxVisibleRows + 1;
    }
    PaintSearch();
    RequestSearchIcons();
}

// ---- querying -------------------------------------------------------------------------

void DockApp::SearchTextChanged() {
    m_searchCaretVisible = true;
    if (m_searchWindow != nullptr) {
        SetTimer(m_searchWindow, kCaretTimerId, kCaretBlinkMs, nullptr);
    }
    if (m_searchMode == SearchMode::KeyEntry || m_searchMode == SearchMode::ModelEntry) {
        PaintSearch();
        return;
    }
    // Editing the question leaves the answer for live results again.
    if (m_searchMode == SearchMode::Ai) {
        ExitAiState();
    }
    const uint64_t request = ++m_searchRequestId;
    if (Trimmed(m_searchText).empty()) {
        m_searchRows.clear();
        m_searchSelected = 0;
        m_searchScroll = 0;
    } else {
        m_fileSearch.Query(m_window, kSearchResultMessage, request, m_searchText, kFileLimit, kAppLimit);
    }
    // Echo the keystroke now; results repaint when they arrive (~ms later).
    PaintSearch();
}

void DockApp::ApplySearchReply(std::unique_ptr<FileSearchReply> reply) {
    if (reply == nullptr || reply->requestId != m_searchRequestId || !IsSearchOpen() ||
        m_searchMode != SearchMode::Search) {
        return;
    }
    if (reply->metadata) {
        // Same rows, now with size / modified time: refresh subtitles only.
        bool changed = false;
        for (const FileSearchItem& item : reply->files) {
            if (!item.haveMetadata) {
                continue;
            }
            for (SearchRow& row : m_searchRows) {
                if (row.kind == SearchRow::Kind::File && row.path == item.path) {
                    row.subtitle = FileSubtitle(item);
                    changed = true;
                    break;
                }
            }
        }
        if (changed) {
            PaintSearch();
        }
        return;
    }
    if (reply->searchMs > 50.0) {
        Log(L"Search slow query ms=" + std::to_wstring(reply->searchMs) + L" entries=" +
            std::to_wstring(reply->indexedEntries));
    }
    std::vector<SearchRow> rows;
    for (const FileSearchItem& item : reply->apps) {
        if (item.appIndex >= m_searchApps.size()) {
            continue;
        }
        const LaunchTarget& target = m_searchApps[item.appIndex];
        SearchRow row;
        row.kind = SearchRow::Kind::App;
        row.title = item.name;
        row.subtitle = target.runningWindow != nullptr ? L"Application \u00B7 Running" : L"Application";
        row.appIndex = item.appIndex;
        row.iconSource = target.app.target;
        row.iconKey = L"app|" + target.app.target;
        rows.push_back(std::move(row));
    }
    for (const FileSearchItem& item : reply->files) {
        SearchRow row;
        const bool folder = item.kind == FileSearchItem::Kind::Folder;
        row.kind = folder ? SearchRow::Kind::Folder : SearchRow::Kind::File;
        row.title = item.name;
        row.path = item.path;
        row.subtitle = FileSubtitle(item);
        row.iconSource = item.path;
        if (folder) {
            row.iconKey = L"folder";
        } else {
            const std::wstring ext = LowerExtension(item.name);
            // Types whose files carry their own icon.
            static const std::wstring ownIcon[] = {L".exe", L".lnk", L".ico", L".url", L".msc", L".cpl",
                L".scr", L".appref-ms"};
            const bool own = std::find(std::begin(ownIcon), std::end(ownIcon), ext) != std::end(ownIcon);
            row.iconKey = own ? L"path|" + item.path : L"ext|" + ext;
        }
        rows.push_back(std::move(row));
    }
    if (reply->files.empty() && reply->indexing && reply->indexedEntries == 0) {
        SearchRow info;
        info.kind = SearchRow::Kind::Info;
        info.title = L"Indexing files\u2026";
        info.subtitle = L"File results appear as soon as the index is ready.";
        rows.push_back(std::move(info));
    }
    const std::wstring query = Trimmed(reply->query);
    if (!query.empty()) {
        SearchRow web;
        web.kind = SearchRow::Kind::Web;
        web.title = L"Search the web for \u201C" + query + L"\u201D";
        rows.push_back(std::move(web));
        SearchRow ai;
        ai.kind = SearchRow::Kind::Ai;
        ai.title = L"Ask AI \u201C" + query + L"\u201D";
        ai.subtitle = m_config.GroqApiKey().empty()
            ? std::wstring(L"Ctrl+Enter \u00B7 Add a Groq API key in Dock Settings to get started")
            : L"Ctrl+Enter \u00B7 Groq \u00B7 " + m_config.AiModel();
        rows.push_back(std::move(ai));
    }
    const FileSearchService::Status status = m_fileSearch.GetStatus();
    if (status.canEnableMft && !m_searchMftPrompting) {
        SearchRow enable;
        enable.kind = SearchRow::Kind::EnableMft;
        enable.title = L"Search this entire PC";
        enable.subtitle = L"Only your user folders are indexed. Enable instant full-disk search "
                          L"(asks for administrator approval once).";
        rows.push_back(std::move(enable));
    }
    m_searchRows = std::move(rows);
    m_searchSelected = 0;
    while (m_searchSelected + 1 < static_cast<int>(m_searchRows.size()) &&
        m_searchRows[static_cast<size_t>(m_searchSelected)].kind == SearchRow::Kind::Info) {
        ++m_searchSelected;
    }
    m_searchScroll = 0;
    PaintSearch();
    RequestSearchIcons();
}

void DockApp::ActivateSearchRow(int index, bool reveal) {
    if (index < 0 || static_cast<size_t>(index) >= m_searchRows.size()) {
        return;
    }
    const SearchRow row = m_searchRows[static_cast<size_t>(index)];
    switch (row.kind) {
    case SearchRow::Kind::App: {
        if (row.appIndex >= m_searchApps.size()) {
            return;
        }
        const LaunchTarget target = m_searchApps[row.appIndex];
        CloseSearch();
        if (reveal && !target.executablePath.empty()) {
            ShellOpenAsync(target.executablePath, true);
        } else {
            static_cast<void>(ActivateLaunchTarget(target));
        }
        return;
    }
    case SearchRow::Kind::File:
    case SearchRow::Kind::Folder:
        CloseSearch();
        ShellOpenAsync(row.path, reveal);
        return;
    case SearchRow::Kind::Web:
        CloseSearch();
        ShellOpenAsync(L"https://www.google.com/search?q=" + UrlEncode(Trimmed(m_searchText)), false);
        return;
    case SearchRow::Kind::EnableMft: {
        m_searchMftPrompting = true;
        CloseSearch();
        const HWND reply = m_window;
        std::thread([reply]() {
            const bool enabled = FileSearchService::RequestMftAccess();
            PostMessageW(reply, kSearchMftAccessMessage, enabled ? 1 : 0, 0);
        }).detach();
        return;
    }
    case SearchRow::Kind::Link:
        if (aimd::IsSafeUrl(row.path)) {
            CloseSearch();
            ShellOpenAsync(row.path, false);
        }
        return;
    case SearchRow::Kind::Ai:
        AskAi();
        return;
    case SearchRow::Kind::Info:
        return;
    }
}

// ---- apps + icons ---------------------------------------------------------------------

void DockApp::RefreshSearchApps() {
    if (m_searchAppsInFlight) {
        return;
    }
    m_searchAppsInFlight = true;
    const HWND reply = m_window;
    std::thread([this, reply, displayApps = m_displayApps, running = m_windows.RunningWindows()]() {
        auto result = std::make_unique<SearchAppsReply>();
        try {
            m_installedApps.EnsureLoaded();
            // One row per app name (a pin, a classic copy and a Store package
            // can all be "Notepad"); the first target wins, as in the dock.
            std::unordered_set<std::wstring> seen;
            for (LaunchTarget& target : CollectLaunchTargets(displayApps, running)) {
                if (target.isStart) {
                    continue;
                }
                std::wstring name = MakeLaunchCandidate(target).name;
                std::wstring key = name;
                CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
                if (name.empty() || !seen.insert(std::move(key)).second) {
                    continue;
                }
                result->names.push_back(std::move(name));
                result->targets.push_back(std::move(target));
            }
        } catch (...) {
            result->targets.clear();
            result->names.clear();
        }
        SearchAppsReply* raw = result.release();
        if (PostMessageW(reply, kSearchAppsMessage, 0, reinterpret_cast<LPARAM>(raw)) == FALSE) {
            delete raw;
        }
    }).detach();
}

void DockApp::ApplySearchApps(std::unique_ptr<SearchAppsReply> reply) {
    m_searchAppsInFlight = false;
    if (reply == nullptr || reply->targets.empty()) {
        return;
    }
    m_searchApps = std::move(reply->targets);
    m_fileSearch.SetApps(reply->names);
    if (IsSearchOpen() && !Trimmed(m_searchText).empty()) {
        SearchTextChanged();
    }
}

void DockApp::RequestSearchIcons() {
    if (m_searchIconsInFlight || !IsSearchOpen()) {
        return;
    }
    const UINT dpi = HostDpi();
    const float scale = static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
    const auto extent = static_cast<UINT>(MetricsFor(scale, 10000).iconSize);
    if (extent != m_searchIconExtent) {
        m_searchIcons.clear();
        m_searchIconsMissing.clear();
        m_searchIconExtent = extent;
    }
    std::vector<std::pair<std::wstring, std::wstring>> wanted;
    const int first = m_searchScroll;
    const int last = std::min(static_cast<int>(m_searchRows.size()), m_searchScroll + kMaxVisibleRows * 2);
    for (int i = first; i < last && wanted.size() < 12; ++i) {
        const SearchRow& row = m_searchRows[static_cast<size_t>(i)];
        if (row.iconKey.empty() || row.iconSource.empty() || m_searchIcons.contains(row.iconKey) ||
            m_searchIconsMissing.contains(row.iconKey) ||
            std::ranges::any_of(wanted, [&](const auto& w) { return w.first == row.iconKey; })) {
            continue;
        }
        wanted.emplace_back(row.iconKey, row.iconSource);
    }
    if (wanted.empty()) {
        return;
    }
    m_searchIconsInFlight = true;
    const HWND reply = m_window;
    std::thread([reply, extent, wanted = std::move(wanted)]() {
        auto result = std::make_unique<SearchIconReply>();
        result->extent = extent;
        for (const auto& [key, source] : wanted) {
            std::vector<uint8_t> pixels;
            try {
                pixels = DownsamplePremul(Renderer::ExtractIconPixels({source}, extent), extent);
            } catch (...) {
                pixels.clear();
            }
            result->icons.emplace_back(key, std::move(pixels));
        }
        SearchIconReply* raw = result.release();
        if (PostMessageW(reply, kSearchIconMessage, 0, reinterpret_cast<LPARAM>(raw)) == FALSE) {
            delete raw;
        }
    }).detach();
}

void DockApp::ApplySearchIcons(std::unique_ptr<SearchIconReply> reply) {
    m_searchIconsInFlight = false;
    if (reply == nullptr || reply->extent != m_searchIconExtent) {
        RequestSearchIcons();
        return;
    }
    for (auto& [key, pixels] : reply->icons) {
        if (pixels.empty()) {
            m_searchIconsMissing.insert(key);
        } else {
            m_searchIcons[key] = std::move(pixels);
        }
    }
    // Bound the cache: per-file icons (exe/lnk) accumulate across queries.
    if (m_searchIcons.size() > 512) {
        m_searchIcons.clear();
    }
    if (IsSearchOpen()) {
        PaintSearch();
        RequestSearchIcons();
    }
}

// ---- AI answers -------------------------------------------------------------------------

void DockApp::AskAi() {
    const std::wstring prompt = Trimmed(m_searchText);
    if (prompt.empty() || m_searchWindow == nullptr) {
        return;
    }
    ExitAiState();
    m_searchMode = SearchMode::Ai;
    m_searchSelectAll = false;
    m_searchCaret = m_searchText.size();
    ++m_searchRequestId;  // drop file results still in flight
    if (m_aiView == nullptr) {
        m_aiView = std::make_unique<AiAnswerView>();
    }
    std::wstring apiKey = m_config.GroqApiKey();
    if (apiKey.empty()) {
        m_aiNeedsKey = true;
        m_aiState.error = L"Hoverdock answers with your own Groq API key. Add one to ask AI: it's free to "
                          L"create and is stored encrypted for your Windows account.";
        m_aiState.done = true;
        PaintSearch();
        return;
    }
    m_aiModel = m_config.AiModel();
    auto session = std::make_shared<AiSession>();
    session->id = ++m_aiSessionCounter;
    m_aiSession = session;
    m_aiState.status = L"Thinking\u2026";
    GroqClient::AskAsync(std::move(session), m_window, kAiUpdateMessage, std::move(apiKey), m_aiModel, prompt);
    SetTimer(m_searchWindow, kAiSpinnerTimerId, kAiSpinnerMs, nullptr);
    Log(L"AI: asking " + m_aiModel + L" (" + std::to_wstring(prompt.size()) + L" chars).");
    PaintSearch();
}

void DockApp::ApplyAiUpdate(uint64_t sessionId) {
    if (m_aiSession == nullptr || m_aiSession->id != sessionId) {
        return;
    }
    m_aiState = m_aiSession->Snapshot();
    if (m_aiView != nullptr) {
        m_aiView->SetMarkdown(m_aiState.markdown);
    }
    if (m_aiState.done) {
        m_aiSession.reset();
        if (m_searchWindow != nullptr) {
            KillTimer(m_searchWindow, kAiSpinnerTimerId);
        }
        if (!m_aiState.error.empty()) {
            Log(L"AI: failed: " + m_aiState.error);
        } else {
            Log(L"AI: answered (" + std::to_wstring(m_aiState.markdown.size()) + L" chars, " +
                std::to_wstring(m_aiState.sources.size()) + L" sources, searched=" +
                (m_aiState.searched ? L"1)." : L"0)."));
        }
    }
    if (IsSearchOpen() && m_searchMode == SearchMode::Ai) {
        PaintSearch();
        RequestAiImages();
    }
}

void DockApp::RequestAiImages() {
    if (m_aiView == nullptr || m_searchMode != SearchMode::Ai || m_window == nullptr) {
        return;
    }
    const float scale = static_cast<float>(HostDpi() == 0 ? 96U : HostDpi()) / 96.0F;
    for (std::wstring& url : m_aiView->TakeImageRequests()) {
        GroqClient::FetchImageAsync(m_window, kAiImageMessage, std::move(url),
            static_cast<UINT>(Px(640.0F, scale)), static_cast<UINT>(Px(420.0F, scale)));
    }
}

void DockApp::ApplyAiImage(std::unique_ptr<AiImageReply> image) {
    if (image == nullptr || m_aiView == nullptr) {
        return;
    }
    if (m_aiView->SetImage(*image) && IsSearchOpen() && m_searchMode == SearchMode::Ai) {
        PaintSearch();
    }
}

void DockApp::ExitAiState() noexcept {
    if (m_aiSession != nullptr) {
        m_aiSession->cancelled.store(true);
        m_aiSession.reset();
    }
    if (m_searchWindow != nullptr) {
        KillTimer(m_searchWindow, kAiSpinnerTimerId);
        KillTimer(m_searchWindow, kAiCopiedTimerId);
    }
    if (m_searchMode == SearchMode::Ai) {
        m_searchMode = SearchMode::Search;
    }
    m_aiState = AiSnapshot{};
    m_aiNeedsKey = false;
    m_aiShowSources = false;
    m_aiScroll = 0;
    m_aiSourcesScroll = 0;
    m_aiHover = -1;
    m_aiSourceRects.clear();
    m_aiBadge = {};
    m_aiKeyButton = {};
    if (m_aiView != nullptr) {
        m_aiView->Clear();
    }
}

void DockApp::PaintSearchAi(uint8_t* pixels, int width, int height, float scale, LONG listTop) {
    const uint8_t inkB = g_flyoutInkB;
    const uint8_t inkG = g_flyoutInkG;
    const uint8_t inkR = g_flyoutInkR;
    const LONG separatorY = listTop - 1 - Px(6.0F, scale);
    FillRectColorPremul(pixels, width, height, {Px(16.0F, scale), separatorY, width - Px(16.0F, scale),
        separatorY + 1}, 0.14F, inkB, inkG, inkR);
    const LONG footerHeight = Px(42.0F, scale);
    const LONG footerTop = height - footerHeight;
    const LONG sideX = Px(26.0F, scale);
    m_aiViewport = {sideX, listTop + Px(6.0F, scale), width - sideX, footerTop - Px(4.0F, scale)};
    const RECT& view = m_aiViewport;
    const int viewHeight = static_cast<int>(view.bottom - view.top);
    m_aiSourceRects.clear();
    m_aiBadge = {};
    m_aiKeyButton = {};
    FillRectColorPremul(pixels, width, height, {Px(16.0F, scale), footerTop, width - Px(16.0F, scale),
        footerTop + 1}, 0.10F, inkB, inkG, inkR);

    if (m_aiShowSources) {
        const LONG rowHeight = Px(54.0F, scale);
        const int visible = std::max(1, viewHeight / static_cast<int>(rowHeight));
        const int count = static_cast<int>(m_aiState.sources.size());
        m_aiSourcesScroll = std::clamp(m_aiSourcesScroll, 0, std::max(0, count - visible));
        if (count == 0) {
            DrawFlyoutText(pixels, width, height, view, m_searchTitleFont,
                L"The web was searched, but no source links were returned.", DT_LEFT | DT_TOP | DT_WORDBREAK, 200);
        }
        for (int v = 0; v < visible && m_aiSourcesScroll + v < count; ++v) {
            const int index = m_aiSourcesScroll + v;
            const AiSource& source = m_aiState.sources[static_cast<size_t>(index)];
            const RECT rect{Px(10.0F, scale), view.top + v * rowHeight, width - Px(10.0F, scale),
                view.top + (v + 1) * rowHeight};
            m_aiSourceRects.push_back(rect);
            if (m_aiHover == 100 + v) {
                FillSquircleColorPremul(pixels, width, height, rect, static_cast<float>(Px(11.0F, scale)), 0.16F,
                    inkB, inkG, inkR);
            }
            const LONG numberLeft = rect.left + Px(10.0F, scale);
            const LONG numberSize = Px(26.0F, scale);
            const LONG numberTop = rect.top + (rowHeight - numberSize) / 2;
            const RECT numberRect{numberLeft, numberTop, numberLeft + numberSize, numberTop + numberSize};
            FillSquircleColorPremul(pixels, width, height, numberRect, static_cast<float>(numberSize) * 0.5F, 0.12F,
                inkB, inkG, inkR);
            DrawFlyoutText(pixels, width, height, numberRect, m_searchSubFont, std::to_wstring(index + 1),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, 220);
            const LONG labelLeft = numberRect.right + Px(12.0F, scale);
            const LONG labelRight = rect.right - Px(12.0F, scale);
            const LONG titleHeight = Px(21.0F, scale);
            const LONG subHeight = Px(17.0F, scale);
            const LONG top = rect.top + (rowHeight - titleHeight - subHeight) / 2;
            DrawFlyoutText(pixels, width, height, {labelLeft, top, labelRight, top + titleHeight}, m_searchTitleFont,
                source.title.empty() ? source.url : source.title,
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 250);
            std::wstring subtitle = UrlHost(source.url);
            if (!source.snippet.empty()) {
                std::wstring snippet = source.snippet.substr(0, 200);
                std::replace(snippet.begin(), snippet.end(), L'\n', L' ');
                subtitle += L" \u00B7 " + snippet;
            }
            DrawFlyoutText(pixels, width, height, {labelLeft, top + titleHeight, labelRight, top + titleHeight + subHeight},
                m_searchSubFont, subtitle, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 150);
        }
    } else if (!m_aiState.markdown.empty() && m_aiView != nullptr) {
        const int scrollbarRoom = Px(12.0F, scale);
        const int contentWidth = std::max(1, static_cast<int>(view.right - view.left) - scrollbarRoom);
        m_aiView->Layout(contentWidth, scale, FontFace(m_searchTitleFont));
        const int contentHeight = m_aiView->ContentHeight();
        const int maxScroll = std::max(0, contentHeight - viewHeight);
        m_aiScroll = std::clamp(m_aiScroll, 0, maxScroll);
        m_aiView->Paint(pixels, width, height, RECT{view.left, view.top, view.left + contentWidth, view.bottom},
            m_aiScroll, AiInk{inkR, inkG, inkB});
        if (maxScroll > 0) {
            const int thumbHeight = std::max(Px(28.0F, scale), viewHeight * viewHeight / std::max(1, contentHeight));
            const int thumbTop = static_cast<int>(view.top) +
                static_cast<int>(static_cast<long long>(viewHeight - thumbHeight) * m_aiScroll / maxScroll);
            const LONG thumbX = width - Px(15.0F, scale);
            FillSquircleColorPremul(pixels, width, height,
                {thumbX, thumbTop, thumbX + Px(4.0F, scale), thumbTop + thumbHeight}, static_cast<float>(Px(2.0F, scale)),
                0.30F, inkB, inkG, inkR);
        }
    } else if (!m_aiState.error.empty()) {
        const LONG glyphSize = Px(28.0F, scale);
        DrawFlyoutText(pixels, width, height, {view.left, view.top, view.left + glyphSize, view.top + glyphSize},
            m_searchGlyphFont, kGlyphInfo, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 220);
        const RECT textRect{view.left + glyphSize + Px(10.0F, scale), view.top + Px(3.0F, scale), view.right,
            view.top + Px(120.0F, scale)};
        DrawFlyoutText(pixels, width, height, textRect, m_searchTitleFont, m_aiState.error,
            DT_LEFT | DT_TOP | DT_WORDBREAK, 235);
        if (m_aiNeedsKey) {
            const std::wstring label = L"Add Groq API key";
            const LONG buttonWidth = static_cast<LONG>(std::ceil(MeasureText(m_searchTitleFont, label))) +
                Px(36.0F, scale);
            const LONG buttonTop = textRect.bottom + Px(8.0F, scale);
            m_aiKeyButton = {textRect.left, buttonTop, textRect.left + buttonWidth, buttonTop + Px(36.0F, scale)};
            FillSquircleColorPremul(pixels, width, height, m_aiKeyButton,
                static_cast<float>(m_aiKeyButton.bottom - m_aiKeyButton.top) * 0.5F, m_aiHover == 2 ? 0.26F : 0.16F,
                inkB, inkG, inkR);
            DrawFlyoutText(pixels, width, height, m_aiKeyButton, m_searchTitleFont, label,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, 255);
        }
    } else {
        std::wstring status = m_aiState.status.empty() ? std::wstring(L"Thinking") : m_aiState.status;
        if (!status.empty() && status.back() == L'\u2026') {
            status.pop_back();
        }
        status += std::wstring(static_cast<size_t>(1 + m_aiSpinner % 3), L'.');
        DrawFlyoutText(pixels, width, height, {view.left, view.top, view.right, view.top + Px(28.0F, scale)},
            m_searchTitleFont, status, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 190);
        // Skeleton lines hint where the answer will appear.
        const float pulse = 0.05F + 0.03F * static_cast<float>(m_aiSpinner % 3);
        for (int line = 0; line < 3; ++line) {
            const LONG top = view.top + Px(44.0F, scale) + line * Px(24.0F, scale);
            const LONG right = view.left + (view.right - view.left) * (line == 2 ? 55 : 92 - line * 8) / 100;
            FillSquircleColorPremul(pixels, width, height, {view.left, top, right, top + Px(12.0F, scale)},
                static_cast<float>(Px(6.0F, scale)), pulse, inkB, inkG, inkR);
        }
    }

    // ---- footer: model + state on the left, Sources badge on the right ----
    LONG footerRight = width - Px(20.0F, scale);
    if (m_aiState.searched) {
        const size_t count = m_aiState.sources.size();
        const std::wstring label = m_aiShowSources ? std::wstring(L"Back to answer")
            : count == 1                           ? std::wstring(L"1 source")
            : count == 0                           ? std::wstring(L"Searched the web")
                                                   : std::to_wstring(count) + L" sources";
        const LONG glyphWidth = Px(16.0F, scale);
        const LONG textWidth = static_cast<LONG>(std::ceil(MeasureText(m_searchSubFont, label))) + 2;
        const LONG badgeHeight = Px(26.0F, scale);
        const LONG badgeWidth = Px(10.0F, scale) + glyphWidth + Px(6.0F, scale) + textWidth + Px(12.0F, scale);
        const LONG badgeTop = footerTop + (footerHeight - badgeHeight) / 2;
        m_aiBadge = {footerRight - badgeWidth, badgeTop, footerRight, badgeTop + badgeHeight};
        const float alpha = m_aiShowSources ? 0.24F : m_aiHover == 1 ? 0.22F : 0.13F;
        FillSquircleColorPremul(pixels, width, height, m_aiBadge, static_cast<float>(badgeHeight) * 0.5F, alpha, inkB,
            inkG, inkR);
        const LONG glyphLeft = m_aiBadge.left + Px(10.0F, scale);
        DrawFlyoutText(pixels, width, height, {glyphLeft, m_aiBadge.top, glyphLeft + glyphWidth, m_aiBadge.bottom},
            m_searchSmallGlyphFont, kGlyphGlobe, DT_CENTER | DT_VCENTER | DT_SINGLELINE, 230);
        DrawFlyoutText(pixels, width, height,
            {glyphLeft + glyphWidth + Px(6.0F, scale), m_aiBadge.top, m_aiBadge.right, m_aiBadge.bottom},
            m_searchSubFont, label, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 235);
        footerRight = m_aiBadge.left - Px(10.0F, scale);
    }
    std::wstring footer = L"\u2726 " + (m_aiModel.empty() ? m_config.AiModel() : m_aiModel);
    if (!m_aiState.done && !m_aiState.status.empty()) {
        footer += L" \u00B7 " + m_aiState.status;
    } else if (m_aiNeedsKey) {
        footer = L"\u2726 Groq \u00B7 Enter to add a key";
    } else if (!m_aiState.error.empty()) {
        footer += L" \u00B7 Ctrl+Enter to retry";
    } else if (m_aiState.done) {
        footer += L" \u00B7 Ctrl+C copies the answer";
    }
    DrawFlyoutText(pixels, width, height, {Px(24.0F, scale), footerTop, footerRight, height}, m_searchSubFont, footer,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 150);
}

int DockApp::AiHoverTarget(POINT client) const {
    if (PointIn(m_aiBadge, client)) {
        return 1;
    }
    if (m_aiNeedsKey && PointIn(m_aiKeyButton, client)) {
        return 2;
    }
    if (m_aiShowSources) {
        for (size_t i = 0; i < m_aiSourceRects.size(); ++i) {
            if (PointIn(m_aiSourceRects[i], client)) {
                return 100 + static_cast<int>(i);
            }
        }
    }
    return -1;
}

bool DockApp::AiPointerIsLink(POINT client) const {
    if (AiHoverTarget(client) >= 0) {
        return true;
    }
    if (m_aiShowSources || m_aiView == nullptr || !PointIn(m_aiViewport, client)) {
        return false;
    }
    const POINT content{client.x - m_aiViewport.left, client.y - m_aiViewport.top + m_aiScroll};
    return m_aiView->HitTest(content).kind != AiAnswerView::Hit::Kind::None;
}

bool DockApp::HandleAiClick(POINT client) {
    if (PointIn(m_aiBadge, client)) {
        m_aiShowSources = !m_aiShowSources;
        m_aiSourcesScroll = 0;
        m_aiHover = AiHoverTarget(client);
        PaintSearch();
        return true;
    }
    if (m_aiNeedsKey && PointIn(m_aiKeyButton, client)) {
        m_aiAskAfterKey = true;
        OpenSearchEntry(SearchMode::KeyEntry);
        return true;
    }
    if (m_aiShowSources) {
        for (size_t i = 0; i < m_aiSourceRects.size(); ++i) {
            const size_t index = static_cast<size_t>(m_aiSourcesScroll) + i;
            if (PointIn(m_aiSourceRects[i], client) && index < m_aiState.sources.size() &&
                aimd::IsSafeUrl(m_aiState.sources[index].url)) {
                const std::wstring url = m_aiState.sources[index].url;
                CloseSearch();
                ShellOpenAsync(url, false);
                return true;
            }
        }
        return false;
    }
    if (m_aiView == nullptr || !PointIn(m_aiViewport, client)) {
        return false;
    }
    const POINT content{client.x - m_aiViewport.left, client.y - m_aiViewport.top + m_aiScroll};
    const AiAnswerView::Hit hit = m_aiView->HitTest(content);
    if (hit.kind == AiAnswerView::Hit::Kind::Link && aimd::IsSafeUrl(hit.url)) {
        CloseSearch();
        ShellOpenAsync(hit.url, false);
        return true;
    }
    if (hit.kind == AiAnswerView::Hit::Kind::CopyCode &&
        CopyTextToClipboard(m_searchWindow, m_aiView->CodeText(hit.code))) {
        m_aiView->SetCopied(hit.code);
        SetTimer(m_searchWindow, kAiCopiedTimerId, kAiCopiedMs, nullptr);
        PaintSearch();
        return true;
    }
    return false;
}

void DockApp::ScrollAi(int delta) {
    if (m_aiShowSources) {
        const int step = delta > 0 ? 1 : -1;
        const int before = m_aiSourcesScroll;
        m_aiSourcesScroll = std::max(0, m_aiSourcesScroll + step);
        if (m_aiSourcesScroll != before) {
            PaintSearch();  // the paint clamps to the list length
        }
        return;
    }
    if (m_aiView == nullptr) {
        return;
    }
    const int viewHeight = static_cast<int>(m_aiViewport.bottom - m_aiViewport.top);
    const int maxScroll = std::max(0, m_aiView->ContentHeight() - viewHeight);
    const int next = std::clamp(m_aiScroll + delta, 0, maxScroll);
    if (next != m_aiScroll) {
        m_aiScroll = next;
        PaintSearch();
    }
}

void DockApp::CopyAiAnswer() {
    if (m_aiState.markdown.empty()) {
        CopyTextToClipboard(m_searchWindow, m_searchText);
        return;
    }
    std::wstring text = m_aiState.markdown;
    if (!m_aiState.sources.empty()) {
        text += L"\n\nSources:\n";
        for (size_t i = 0; i < m_aiState.sources.size(); ++i) {
            const AiSource& source = m_aiState.sources[i];
            text += std::to_wstring(i + 1) + L". " + source.title + L" \u2014 " + source.url + L"\n";
        }
    }
    CopyTextToClipboard(m_searchWindow, text);
}

// ---- Dock Settings entries (Groq API key / AI model) ------------------------------------

void DockApp::OpenSearchEntry(SearchMode mode) {
    if (mode != SearchMode::KeyEntry && mode != SearchMode::ModelEntry) {
        return;
    }
    const bool askAfterKey = m_aiAskAfterKey;
    if (m_searchMode == SearchMode::KeyEntry || m_searchMode == SearchMode::ModelEntry) {
        SecureZeroMemory(m_searchText.data(), m_searchText.size() * sizeof(wchar_t));
    } else {
        m_searchSavedText = m_searchText;
    }
    ExitAiState();
    m_aiAskAfterKey = askAfterKey && mode == SearchMode::KeyEntry;
    m_searchMode = mode;
    m_searchText = mode == SearchMode::ModelEntry ? m_config.AiModel() : std::wstring();
    m_searchCaret = m_searchText.size();
    if (!IsSearchOpen() && !OpenSearch()) {
        ExitSearchEntry(false);
        return;
    }
    m_searchSelectAll = !m_searchText.empty();
    m_searchCaretVisible = true;
    BuildSearchEntryRows();
    PaintSearch();
}

void DockApp::BuildSearchEntryRows() {
    m_searchRows.clear();
    m_searchScroll = 0;
    m_searchSelected = -1;
    SearchRow info;
    info.kind = SearchRow::Kind::Info;
    SearchRow link;
    link.kind = SearchRow::Kind::Link;
    if (m_searchMode == SearchMode::KeyEntry) {
        info.title = L"Paste your Groq API key and press Enter";
        info.subtitle = m_config.GroqApiKeyFromEnvironment()
            ? L"The GROQ_API_KEY environment variable is set and takes priority over a saved key."
            : L"Stored encrypted for your Windows account. Save an empty key to remove it.";
        link.title = L"Get a free API key at console.groq.com/keys";
        link.subtitle = L"Opens GroqCloud in your browser";
        link.path = L"https://console.groq.com/keys";
    } else {
        info.title = L"Type a Groq model ID and press Enter";
        info.subtitle = L"Leave it empty to use the default, qwen/qwen3.8-27b.";
        link.title = L"Browse models on GroqCloud";
        link.subtitle = L"Opens the Groq model list in your browser";
        link.path = L"https://console.groq.com/docs/models";
    }
    m_searchRows.push_back(std::move(info));
    m_searchRows.push_back(std::move(link));
}

void DockApp::CommitSearchEntry() {
    const SearchMode mode = m_searchMode;
    std::wstring value = Trimmed(m_searchText);
    SearchRow notice;
    notice.kind = SearchRow::Kind::Info;
    bool askNow = false;
    if (mode == SearchMode::KeyEntry) {
        const bool removed = value.empty();
        m_config.SetGroqApiKey(std::move(value));
        notice.title = removed ? L"Groq API key removed." : L"Groq API key saved.";
        notice.subtitle = removed ? L"" : L"Type a question and press Ctrl+Enter to ask AI.";
        askNow = m_aiAskAfterKey && !removed;
        Log(removed ? L"AI: Groq API key removed." : L"AI: Groq API key saved.");
    } else if (mode == SearchMode::ModelEntry) {
        m_config.SetAiModel(std::move(value));
        notice.title = L"AI model set to " + m_config.AiModel() + L".";
        notice.subtitle = L"Used for Ctrl+Enter answers in Search.";
        Log(L"AI: model set to " + m_config.AiModel() + L".");
    } else {
        return;
    }
    ScheduleConfigSave();
    ExitSearchEntry(false);
    RefreshSettingsControls();
    if (askNow && !Trimmed(m_searchText).empty()) {
        AskAi();
        return;
    }
    m_searchRows.clear();
    m_searchRows.push_back(std::move(notice));
    m_searchSelected = 0;
    m_searchScroll = 0;
    m_searchSelectAll = !m_searchText.empty();
    PaintSearch();
}

void DockApp::ExitSearchEntry(bool refresh) {
    if (m_searchMode != SearchMode::KeyEntry && m_searchMode != SearchMode::ModelEntry) {
        return;
    }
    SecureZeroMemory(m_searchText.data(), m_searchText.size() * sizeof(wchar_t));
    m_searchText = std::move(m_searchSavedText);
    m_searchSavedText.clear();
    m_searchCaret = m_searchText.size();
    m_searchSelectAll = false;
    m_searchMode = SearchMode::Search;
    m_aiAskAfterKey = false;
    m_searchRows.clear();
    m_searchSelected = 0;
    m_searchScroll = 0;
    if (refresh && IsSearchOpen()) {
        SearchTextChanged();
    }
}

// ---- lifetime -------------------------------------------------------------------------

void DockApp::StartSearchServices() {
    m_fileSearch.Start(m_window, kSearchStatusMessage);
    if (m_config.SearchHotkey() && !m_searchHotkey.Start(m_window, kSearchHotkeyMessage)) {
        Log(L"Search: Win+Space hook unavailable; use the dock Search tile.");
    }
    RefreshSearchApps();
}

void DockApp::StopSearchServices() noexcept {
    ExitAiState();
    m_aiView.reset();
    m_searchHotkey.Stop();
    if (m_searchWindow != nullptr) {
        KillTimer(m_searchWindow, kCaretTimerId);
        DestroyWindow(m_searchWindow);
        m_searchWindow = nullptr;
    }
    ReleaseLayerPresentDib(m_searchLayerDib);
    DestroySearchFonts();
    m_fileSearch.Stop();
}
