#include "AiAnswerView.h"

#include "GroqClient.h"

#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dock_detail {
void CompositePremul(uint8_t* dest, int destWidth, int destHeight, int destX, int destY,
    const uint8_t* source, int sourceWidth, int sourceHeight);
void FillRectColorPremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds, float alpha,
    uint8_t blue, uint8_t green, uint8_t red);
void FillSquircleColorPremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds,
    float radius, float alpha, uint8_t blue, uint8_t green, uint8_t red);
}  // namespace dock_detail
using dock_detail::CompositePremul;
using dock_detail::FillRectColorPremul;
using dock_detail::FillSquircleColorPremul;

namespace {

// Colour classes carried by DirectWrite drawing effects; text is rasterized
// once per class present in a layout.
enum ColorClass : uint8_t { kInk, kLinkInk, kComment, kString, kKeyword, kNumber, kFaint, kClassCount };

constexpr size_t kMaxImages = 4;
constexpr auto kEmergencyBreak = static_cast<DWRITE_WORD_WRAPPING>(2);  // DWRITE_WORD_WRAPPING_EMERGENCY_BREAK

class ColorEffect final : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown)) {
            *object = static_cast<IUnknown*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

ColorEffect g_effects[kClassCount];

uint8_t EffectClass(IUnknown* effect) noexcept {
    for (uint8_t i = 0; i < kClassCount; ++i) {
        if (effect == static_cast<IUnknown*>(&g_effects[i])) {
            return i;
        }
    }
    return kInk;
}

struct PassRenderer final : IDWriteTextRenderer {
    IDWriteBitmapRenderTarget* target = nullptr;
    IDWriteRenderingParams* params = nullptr;
    uint8_t pass = kInk;

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
        if (transform == nullptr) {
            return E_POINTER;
        }
        target->GetCurrentTransform(transform);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override {
        if (pixelsPerDip == nullptr) {
            return E_POINTER;
        }
        *pixelsPerDip = target->GetPixelsPerDip();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT baselineOriginX, FLOAT baselineOriginY,
        DWRITE_MEASURING_MODE measuringMode, DWRITE_GLYPH_RUN const* glyphRun, DWRITE_GLYPH_RUN_DESCRIPTION const*,
        IUnknown* effect) override {
        if (glyphRun == nullptr || EffectClass(effect) != pass) {
            return S_OK;
        }
        return target->DrawGlyphRun(baselineOriginX, baselineOriginY, measuringMode, glyphRun, params,
            RGB(255, 255, 255), nullptr);
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT baselineOriginX, FLOAT baselineOriginY,
        DWRITE_UNDERLINE const* underline, IUnknown* effect) override {
        if (underline != nullptr && EffectClass(effect) == pass) {
            FillLine(baselineOriginX, baselineOriginY + underline->offset, underline->width, underline->thickness);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT baselineOriginX, FLOAT baselineOriginY,
        DWRITE_STRIKETHROUGH const* strikethrough, IUnknown* effect) override {
        if (strikethrough != nullptr && EffectClass(effect) == pass) {
            FillLine(baselineOriginX, baselineOriginY + strikethrough->offset, strikethrough->width,
                strikethrough->thickness);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL,
        IUnknown*) override {
        return S_OK;
    }

private:
    void FillLine(float x, float y, float width, float thickness) const {
        const int top = static_cast<int>(std::lround(y));
        const RECT line{static_cast<LONG>(std::lround(x)), top, static_cast<LONG>(std::lround(x + width)),
            top + std::max(1, static_cast<int>(std::lround(thickness)))};
        FillRect(target->GetMemoryDC(), &line, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }
};

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

Rgb ClassColor(uint8_t cls, const AiInk& ink) {
    const float luma = (0.2126F * ink.r + 0.7152F * ink.g + 0.0722F * ink.b) / 255.0F;
    const bool lightText = luma >= 0.5F;
    switch (cls) {
    case kLinkInk: return lightText ? Rgb{0x7C, 0xC0, 0xFF} : Rgb{0x0A, 0x58, 0xCA};
    case kString: return lightText ? Rgb{0xA8, 0xD8, 0x8C} : Rgb{0x1A, 0x7F, 0x37};
    case kKeyword: return lightText ? Rgb{0xD2, 0xA8, 0xFF} : Rgb{0x8A, 0x2B, 0xC2};
    case kNumber: return lightText ? Rgb{0xFF, 0xBE, 0x7A} : Rgb{0xB3, 0x5C, 0x00};
    default: return Rgb{ink.r, ink.g, ink.b};
    }
}

uint8_t ClassGray(uint8_t cls, uint8_t gray) noexcept {
    if (cls == kComment) {
        return 140;
    }
    if (cls == kFaint) {
        return 150;
    }
    return gray;
}

int TextHeight(IDWriteTextLayout* layout) {
    DWRITE_TEXT_METRICS metrics{};
    if (layout == nullptr || FAILED(layout->GetMetrics(&metrics))) {
        return 0;
    }
    return static_cast<int>(std::ceil(metrics.height));
}

std::vector<DWRITE_HIT_TEST_METRICS> RangeRects(IDWriteTextLayout* layout, size_t start, size_t length) {
    std::vector<DWRITE_HIT_TEST_METRICS> rects;
    UINT32 count = 0;
    const HRESULT probe = layout->HitTestTextRange(static_cast<UINT32>(start), static_cast<UINT32>(length), 0.0F,
        0.0F, nullptr, 0, &count);
    if (count == 0 || (FAILED(probe) && probe != E_NOT_SUFFICIENT_BUFFER)) {
        return rects;
    }
    rects.resize(count);
    if (FAILED(layout->HitTestTextRange(static_cast<UINT32>(start), static_cast<UINT32>(length), 0.0F, 0.0F,
            rects.data(), count, &count))) {
        rects.clear();
    } else {
        rects.resize(count);
    }
    return rects;
}

// Area-average resize of premultiplied BGRA (downscale only).
std::vector<uint8_t> ResizePremul(const std::vector<uint8_t>& source, int sourceWidth, int sourceHeight, int width,
    int height) {
    if (width == sourceWidth && height == sourceHeight) {
        return source;
    }
    std::vector<uint8_t> out(static_cast<size_t>(width) * static_cast<size_t>(height) * 4U, 0);
    for (int y = 0; y < height; ++y) {
        const int y0 = y * sourceHeight / height;
        const int y1 = std::max(y0 + 1, (y + 1) * sourceHeight / height);
        for (int x = 0; x < width; ++x) {
            const int x0 = x * sourceWidth / width;
            const int x1 = std::max(x0 + 1, (x + 1) * sourceWidth / width);
            unsigned sum[4]{};
            unsigned count = 0;
            for (int sy = y0; sy < y1 && sy < sourceHeight; ++sy) {
                const uint8_t* row = source.data() + (static_cast<size_t>(sy) * sourceWidth + x0) * 4U;
                for (int sx = x0; sx < x1 && sx < sourceWidth; ++sx, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                    ++count;
                }
            }
            uint8_t* dest = out.data() + (static_cast<size_t>(y) * width + x) * 4U;
            for (int c = 0; c < 4; ++c) {
                dest[c] = static_cast<uint8_t>(count == 0 ? 0U : sum[c] / count);
            }
        }
    }
    return out;
}

std::wstring PickMonoFace(IDWriteFactory* factory) {
    IDWriteFontCollection* collection = nullptr;
    std::wstring face = L"Consolas";
    if (SUCCEEDED(factory->GetSystemFontCollection(&collection, FALSE)) && collection != nullptr) {
        for (const wchar_t* candidate : {L"Cascadia Mono", L"Cascadia Code", L"Consolas"}) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(collection->FindFamilyName(candidate, &index, &exists)) && exists != FALSE) {
                face = candidate;
                break;
            }
        }
        collection->Release();
    }
    return face;
}

}  // namespace

AiAnswerView::~AiAnswerView() {
    ReleaseLayout();
    ReleaseFormats();
    if (m_target != nullptr) {
        m_target->Release();
    }
    if (m_params != nullptr) {
        m_params->Release();
    }
    if (m_interop != nullptr) {
        m_interop->Release();
    }
    if (m_factory != nullptr) {
        m_factory->Release();
    }
}

void AiAnswerView::ReleaseBlock(BlockLayout& layout) noexcept {
    for (TextItem& item : layout.texts) {
        if (item.layout != nullptr) {
            item.layout->Release();
        }
    }
    layout.texts.clear();
}

void AiAnswerView::ReleaseLayout() noexcept {
    for (BlockLayout& layout : m_layouts) {
        ReleaseBlock(layout);
    }
    m_layouts.clear();
    m_quoteBars.clear();
    for (TextItem& item : m_texts) {
        if (item.layout != nullptr) {
            item.layout->Release();
        }
    }
    m_texts.clear();
    m_rects.clear();
    m_images.clear();
    m_links.clear();
    m_copies.clear();
}

void AiAnswerView::ReleaseFormats() noexcept {
    for (auto& [key, format] : m_formats) {
        format->Release();
    }
    m_formats.clear();
}

bool AiAnswerView::EnsureFactory() {
    if (m_factory == nullptr &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_factory)))) {
        m_factory = nullptr;
        return false;
    }
    if (m_interop == nullptr && FAILED(m_factory->GetGdiInterop(&m_interop))) {
        m_interop = nullptr;
        return false;
    }
    if (m_params == nullptr &&
        FAILED(m_factory->CreateCustomRenderingParams(1.0F, 0.0F, 0.0F, DWRITE_PIXEL_GEOMETRY_FLAT,
            DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &m_params))) {
        m_params = nullptr;
        return false;
    }
    if (m_monoFace.empty()) {
        m_monoFace = PickMonoFace(m_factory);
    }
    return true;
}

void AiAnswerView::SetMarkdown(const std::wstring& markdown) {
    if (markdown == m_markdown) {
        return;
    }
    m_markdown = markdown;
    // Streaming appends: only the trailing blocks are re-parsed.
    size_t codes = 0;
    for (const aimd::Block& block : m_parser.Update(m_markdown)) {
        if (block.kind == aimd::BlockKind::Code) {
            ++codes;
        } else if (block.kind == aimd::BlockKind::Image) {
            static_cast<void>(ImageIndex(block.url));
        }
    }
    if (m_copiedCode != static_cast<size_t>(-1) && m_copiedCode >= codes) {
        m_copiedCode = static_cast<size_t>(-1);
    }
    m_dirty = true;
}

void AiAnswerView::Clear() {
    m_markdown.clear();
    m_parser.Update(m_markdown);
    m_imagesData.clear();
    m_allowedImages.clear();
    m_copiedCode = static_cast<size_t>(-1);
    ReleaseLayout();
    m_height = 0;
    m_dirty = true;
    ++m_generation;
    std::vector<uint8_t>().swap(m_layer);
    std::vector<uint8_t>().swap(m_scratch);
}

size_t AiAnswerView::ImageIndex(const std::wstring& url) {
    for (size_t i = 0; i < m_imagesData.size(); ++i) {
        if (m_imagesData[i].url == url) {
            return i;
        }
    }
    Image image;
    image.url = url;
    // Only the first few search-result images are fetched; the rest render as links.
    const bool allowed = url.starts_with(L"https://") &&
        std::find(m_allowedImages.begin(), m_allowedImages.end(), url) != m_allowedImages.end();
    image.failed = m_imagesData.size() >= kMaxImages || !allowed;
    m_imagesData.push_back(std::move(image));
    return m_imagesData.size() - 1;
}

std::vector<std::wstring> AiAnswerView::TakeImageRequests() {
    std::vector<std::wstring> urls;
    for (Image& image : m_imagesData) {
        if (!image.requested && !image.failed) {
            image.requested = true;
            urls.push_back(image.url);
        }
    }
    return urls;
}

bool AiAnswerView::SetImage(AiImageReply& reply) {
    for (Image& image : m_imagesData) {
        if (image.url != reply.url) {
            continue;
        }
        if (reply.pixels.empty() || reply.width == 0 || reply.height == 0) {
            image.failed = true;
        } else {
            image.width = reply.width;
            image.height = reply.height;
            image.pixels = std::move(reply.pixels);
            image.scaled.clear();
            image.scaledWidth = 0;
            image.scaledHeight = 0;
        }
        m_dirty = true;
        return true;
    }
    return false;
}

std::wstring AiAnswerView::CodeText(size_t code) const {
    size_t index = 0;
    for (const aimd::Block& block : m_parser.Blocks()) {
        if (block.kind == aimd::BlockKind::Code && index++ == code) {
            return block.code;
        }
    }
    return {};
}

void AiAnswerView::SetCopied(size_t code) {
    if (m_copiedCode != code) {
        m_copiedCode = code;
        m_dirty = true;
    }
}

void AiAnswerView::ClearCopied() {
    SetCopied(static_cast<size_t>(-1));
}

IDWriteTextFormat* AiAnswerView::Format(float size, int weight, bool mono) {
    const std::wstring& face = mono ? m_monoFace : m_face;
    const std::wstring key = face + L"|" + std::to_wstring(static_cast<int>(size * 16.0F)) + L"|" +
        std::to_wstring(weight);
    const auto found = m_formats.find(key);
    if (found != m_formats.end()) {
        return found->second;
    }
    IDWriteTextFormat* format = nullptr;
    if (FAILED(m_factory->CreateTextFormat(face.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format)) ||
        format == nullptr) {
        return nullptr;
    }
    if (FAILED(format->SetWordWrapping(kEmergencyBreak))) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    m_formats.emplace(key, format);
    return format;
}

IDWriteTextLayout* AiAnswerView::MakeLayout(const std::wstring& text, float size, int weight, bool mono,
    int maxWidth) {
    IDWriteTextFormat* format = Format(size, weight, mono);
    if (format == nullptr) {
        return nullptr;
    }
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(m_factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
            static_cast<FLOAT>(std::max(1, maxWidth)), 100000.0F, &layout))) {
        return nullptr;
    }
    return layout;
}

IDWriteTextLayout* AiAnswerView::InlineLayout(const aimd::InlineText& text, float size, int weight, int maxWidth,
    uint8_t& classMask) {
    classMask = 1U << kInk;
    IDWriteTextLayout* layout = MakeLayout(text.text.empty() ? std::wstring(L" ") : text.text, size, weight, false,
        maxWidth);
    if (layout == nullptr) {
        return nullptr;
    }
    for (const aimd::Span& span : text.spans) {
        const DWRITE_TEXT_RANGE range{static_cast<UINT32>(span.start), static_cast<UINT32>(span.length)};
        if ((span.flags & aimd::kBold) != 0) {
            layout->SetFontWeight(weight >= 600 ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
        }
        if ((span.flags & aimd::kItalic) != 0) {
            layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        }
        if ((span.flags & aimd::kCode) != 0) {
            layout->SetFontFamilyName(m_monoFace.c_str(), range);
            layout->SetFontSize(size * 0.88F, range);
        }
        if ((span.flags & aimd::kStrike) != 0) {
            layout->SetStrikethrough(TRUE, range);
        }
        if ((span.flags & aimd::kLink) != 0) {
            layout->SetUnderline(TRUE, range);
            layout->SetDrawingEffect(&g_effects[kLinkInk], range);
            classMask = static_cast<uint8_t>(classMask | (1U << kLinkInk));
        }
    }
    return layout;
}

void AiAnswerView::AddText(IDWriteTextLayout* layout, int x, int y, int width, uint8_t gray, uint8_t classMask) {
    if (layout == nullptr) {
        return;
    }
    TextItem item;
    item.layout = layout;
    item.x = x;
    item.y = y;
    item.width = std::max(1, width);
    item.height = std::max(1, TextHeight(layout) + 2);
    item.gray = gray;
    item.classMask = classMask;
    m_texts.push_back(item);
}

int AiAnswerView::LayoutCode(size_t blockIndex, size_t codeIndex, int x, int y, int width) {
    const float scale = m_layoutScale;
    auto P = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
    const aimd::Block& block = Blocks()[blockIndex];
    const int pad = P(12.0F);
    const int header = P(28.0F);
    const float labelSize = 12.0F * scale;
    const int rectIndex = static_cast<int>(m_rects.size());

    const std::wstring language = block.language.empty() ? std::wstring(L"code") : block.language;
    if (IDWriteTextLayout* label = MakeLayout(language, labelSize, 500, false, width / 2)) {
        label->SetDrawingEffect(&g_effects[kFaint], DWRITE_TEXT_RANGE{0, static_cast<UINT32>(language.size())});
        AddText(label, x + pad, y + (header - TextHeight(label)) / 2 + P(2.0F), width / 2, 255, 1U << kFaint);
    }
    const bool copied = codeIndex == m_copiedCode;
    const std::wstring copyText = copied ? L"Copied \u2713" : L"Copy";
    const int copyWidth = P(90.0F);
    if (IDWriteTextLayout* copy = MakeLayout(copyText, labelSize, 500, false, copyWidth)) {
        copy->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        const uint8_t cls = copied ? kInk : kFaint;
        copy->SetDrawingEffect(&g_effects[cls], DWRITE_TEXT_RANGE{0, static_cast<UINT32>(copyText.size())});
        const int top = y + (header - TextHeight(copy)) / 2 + P(2.0F);
        AddText(copy, x + width - pad - copyWidth, top, copyWidth, 255, static_cast<uint8_t>(1U << cls));
        m_copies.push_back({RECT{x + width - pad - P(64.0F), y, x + width, y + header}, codeIndex});
    }

    const int codeWidth = width - pad * 2;
    int codeHeight = 0;
    const std::wstring code = block.code.empty() ? std::wstring(L" ") : block.code;
    if (IDWriteTextLayout* layout = MakeLayout(code, 13.5F * scale, 400, true, codeWidth)) {
        uint8_t mask = 1U << kInk;
        for (const aimd::Token& token : aimd::HighlightCode(block.code, block.language)) {
            uint8_t cls = kInk;
            switch (token.cls) {
            case aimd::TokenClass::Comment: cls = kComment; break;
            case aimd::TokenClass::String: cls = kString; break;
            case aimd::TokenClass::Keyword: cls = kKeyword; break;
            case aimd::TokenClass::Number: cls = kNumber; break;
            case aimd::TokenClass::Plain: break;
            }
            layout->SetDrawingEffect(&g_effects[cls],
                DWRITE_TEXT_RANGE{static_cast<UINT32>(token.start), static_cast<UINT32>(token.length)});
            mask = static_cast<uint8_t>(mask | (1U << cls));
        }
        codeHeight = TextHeight(layout);
        AddText(layout, x + pad, y + header, codeWidth, 240, mask);
    }
    const int total = header + codeHeight + pad;
    m_rects.insert(m_rects.begin() + rectIndex,
        RectItem{RECT{x, y, x + width, y + total}, static_cast<float>(P(10.0F)), 0.085F});
    return total;
}

int AiAnswerView::LayoutTable(const aimd::Block& block, int x, int y, int width) {
    const float scale = m_layoutScale;
    auto P = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
    const size_t columns = block.align.size();
    if (columns == 0 || block.rows.empty()) {
        return 0;
    }
    const int padX = P(10.0F);
    const int padY = P(7.0F);
    const float size = 14.0F * scale;
    std::vector<int> natural(columns, P(24.0F));
    std::vector<int> minimum(columns, P(24.0F));
    for (size_t r = 0; r < block.rows.size(); ++r) {
        for (size_t c = 0; c < columns; ++c) {
            IDWriteTextLayout* probe = MakeLayout(block.rows[r][c].text.empty() ? std::wstring(L" ")
                                                                                 : block.rows[r][c].text,
                size, r == 0 ? 600 : 400, false, 100000);
            if (probe == nullptr) {
                continue;
            }
            DWRITE_TEXT_METRICS metrics{};
            FLOAT minWidth = 0.0F;
            if (SUCCEEDED(probe->GetMetrics(&metrics))) {
                natural[c] = std::max(natural[c], static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace)) + 2);
            }
            if (SUCCEEDED(probe->DetermineMinWidth(&minWidth))) {
                minimum[c] = std::max(minimum[c], static_cast<int>(std::ceil(minWidth)) + 2);
            }
            probe->Release();
        }
    }
    const int available = std::max(static_cast<int>(columns) * P(28.0F), width - static_cast<int>(columns) * padX * 2);
    std::vector<int> widths = natural;
    int naturalSum = 0;
    for (const int value : natural) {
        naturalSum += value;
    }
    if (naturalSum > available) {
        int minimumSum = 0;
        for (size_t c = 0; c < columns; ++c) {
            minimum[c] = std::min(minimum[c], natural[c]);
            minimumSum += minimum[c];
        }
        if (minimumSum >= available) {
            for (size_t c = 0; c < columns; ++c) {
                widths[c] = std::max(P(20.0F), minimum[c] * available / std::max(1, minimumSum));
            }
        } else {
            const int spare = available - minimumSum;
            const int want = naturalSum - minimumSum;
            for (size_t c = 0; c < columns; ++c) {
                widths[c] = minimum[c] + (natural[c] - minimum[c]) * spare / std::max(1, want);
            }
        }
    }
    int tableWidth = 0;
    for (const int value : widths) {
        tableWidth += value + padX * 2;
    }
    tableWidth = std::min(tableWidth, width);

    const size_t backgroundIndex = m_rects.size();
    int rowTop = y;
    for (size_t r = 0; r < block.rows.size(); ++r) {
        int cellX = x;
        int rowHeight = 0;
        const size_t rectsBefore = m_rects.size();
        for (size_t c = 0; c < columns; ++c) {
            uint8_t mask = 0;
            IDWriteTextLayout* layout =
                InlineLayout(block.rows[r][c], size, r == 0 ? 600 : 400, widths[c], mask);
            if (layout != nullptr) {
                const aimd::Align align = block.align[c];
                layout->SetTextAlignment(align == aimd::Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                        : align == aimd::Align::Right                ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                                     : DWRITE_TEXT_ALIGNMENT_LEADING);
                AddInlineDecorations(block.rows[r][c], layout, cellX + padX, rowTop + padY);
                rowHeight = std::max(rowHeight, TextHeight(layout));
                AddText(layout, cellX + padX, rowTop + padY, widths[c], 255, mask);
            }
            cellX += widths[c] + padX * 2;
        }
        rowHeight += padY * 2;
        if (r == 0) {
            m_rects.insert(m_rects.begin() + static_cast<ptrdiff_t>(rectsBefore),
                RectItem{RECT{x, rowTop, x + tableWidth, rowTop + rowHeight}, 0.0F, 0.07F});
        } else {
            m_rects.insert(m_rects.begin() + static_cast<ptrdiff_t>(rectsBefore),
                RectItem{RECT{x + P(4.0F), rowTop, x + tableWidth - P(4.0F), rowTop + 1}, 0.0F, 0.12F});
        }
        rowTop += rowHeight;
    }
    m_rects.insert(m_rects.begin() + static_cast<ptrdiff_t>(backgroundIndex),
        RectItem{RECT{x, y, x + tableWidth, rowTop}, static_cast<float>(P(8.0F)), 0.035F});
    return rowTop - y;
}

void AiAnswerView::AddInlineDecorations(const aimd::InlineText& text, IDWriteTextLayout* layout, int x, int y) {
    const float scale = m_layoutScale;
    auto P = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
    for (const aimd::Span& span : text.spans) {
        const bool code = (span.flags & aimd::kCode) != 0;
        const bool link = (span.flags & aimd::kLink) != 0;
        if (!code && !link) {
            continue;
        }
        for (const DWRITE_HIT_TEST_METRICS& m : RangeRects(layout, span.start, span.length)) {
            const LONG left = x + static_cast<LONG>(std::floor(m.left));
            const LONG top = y + static_cast<LONG>(std::floor(m.top));
            const LONG right = x + static_cast<LONG>(std::ceil(m.left + m.width));
            const LONG bottom = y + static_cast<LONG>(std::ceil(m.top + m.height));
            if (code) {
                m_rects.push_back(RectItem{RECT{left - P(3.0F), top + P(1.0F), right + P(3.0F), bottom - P(1.0F)},
                    static_cast<float>(P(4.0F)), 0.12F});
            }
            if (link) {
                m_links.push_back({RECT{left, top, right, bottom}, span.url});
            }
        }
    }
}

int AiAnswerView::LayoutImage(const aimd::Block& block, int x, int y, int width) {
    const float scale = m_layoutScale;
    auto P = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
    const size_t index = ImageIndex(block.url);
    Image& image = m_imagesData[index];
    const std::wstring& alt = block.text.text;
    if (!image.pixels.empty()) {
        int displayWidth = std::min(static_cast<int>(image.width), width);
        int displayHeight = static_cast<int>(static_cast<long long>(image.height) * displayWidth / image.width);
        const int maxHeight = P(360.0F);
        if (displayHeight > maxHeight) {
            displayHeight = maxHeight;
            displayWidth = static_cast<int>(static_cast<long long>(image.width) * displayHeight / image.height);
        }
        displayWidth = std::max(1, displayWidth);
        displayHeight = std::max(1, displayHeight);
        if (image.scaledWidth != displayWidth || image.scaledHeight != displayHeight || image.scaled.empty()) {
            image.scaled = ResizePremul(image.pixels, static_cast<int>(image.width), static_cast<int>(image.height),
                displayWidth, displayHeight);
            image.scaledWidth = displayWidth;
            image.scaledHeight = displayHeight;
        }
        const RECT rect{x, y, x + displayWidth, y + displayHeight};
        m_images.push_back({rect, index});
        m_links.push_back({rect, image.url});
        int total = displayHeight;
        if (!alt.empty()) {
            if (IDWriteTextLayout* caption = MakeLayout(alt, 12.5F * scale, 400, false, width)) {
                caption->SetDrawingEffect(&g_effects[kFaint], DWRITE_TEXT_RANGE{0, static_cast<UINT32>(alt.size())});
                total += P(4.0F);
                const int captionHeight = TextHeight(caption);
                AddText(caption, x, y + total, width, 255, 1U << kFaint);
                total += captionHeight;
            }
        }
        return total;
    }
    if (image.failed) {
        aimd::InlineText link;
        link.text = L"\U0001F5BC " + (alt.empty() ? image.url : alt);
        link.spans.push_back({0, link.text.size(), aimd::kLink, image.url});
        uint8_t mask = 0;
        IDWriteTextLayout* layout = InlineLayout(link, 15.0F * scale, 400, width, mask);
        if (layout == nullptr) {
            return 0;
        }
        AddInlineDecorations(link, layout, x, y);
        const int height = TextHeight(layout);
        AddText(layout, x, y, width, 255, mask);
        return height;
    }
    const int boxWidth = std::min(width, P(360.0F));
    const int boxHeight = P(160.0F);
    m_rects.push_back(RectItem{RECT{x, y, x + boxWidth, y + boxHeight}, static_cast<float>(P(10.0F)), 0.06F});
    const std::wstring loading = L"Loading image\u2026";
    if (IDWriteTextLayout* label = MakeLayout(loading, 12.5F * scale, 400, false, boxWidth)) {
        label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        label->SetDrawingEffect(&g_effects[kFaint], DWRITE_TEXT_RANGE{0, static_cast<UINT32>(loading.size())});
        AddText(label, x, y + (boxHeight - TextHeight(label)) / 2, boxWidth, 255, 1U << kFaint);
    }
    return boxHeight;
}

int AiAnswerView::LayoutBlock(size_t index, size_t codeIndex, int x, int w) {
    const float scale = m_layoutScale;
    auto P = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
    const float body = 15.0F * scale;
    const aimd::Block& block = Blocks()[index];
    const int quote = std::min(block.quote, 4);
    switch (block.kind) {
    case aimd::BlockKind::Paragraph:
    case aimd::BlockKind::Heading: {
        float size = body;
        int weight = 400;
        if (block.kind == aimd::BlockKind::Heading) {
            size = (block.level == 1 ? 21.0F : block.level == 2 ? 18.0F : 16.0F) * scale;
            weight = 600;
        }
        uint8_t mask = 0;
        IDWriteTextLayout* layout = InlineLayout(block.text, size, weight, w, mask);
        if (layout == nullptr) {
            return 0;
        }
        AddInlineDecorations(block.text, layout, x, 0);
        const int height = TextHeight(layout);
        AddText(layout, x, 0, w, quote > 0 ? 200 : 255, mask);
        return height;
    }
    case aimd::BlockKind::ListItem: {
        const int listX = x + std::min(block.level, 5) * P(18.0F);
        const bool ordered = !block.marker.empty() && block.marker.back() == L'.';
        const int markerWidth = ordered ? P(26.0F) : P(16.0F);
        const int contentX = listX + markerWidth + P(8.0F);
        const int contentWidth = std::max(P(40.0F), x + w - contentX);
        int height = 0;
        if (IDWriteTextLayout* marker = MakeLayout(block.marker, body, ordered ? 500 : 400, false, markerWidth)) {
            marker->SetTextAlignment(ordered ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER);
            height = TextHeight(marker);
            AddText(marker, listX, 0, markerWidth, ordered ? 200 : 220, 1U << kInk);
        }
        uint8_t mask = 0;
        IDWriteTextLayout* layout = InlineLayout(block.text, body, 400, contentWidth, mask);
        if (layout != nullptr) {
            AddInlineDecorations(block.text, layout, contentX, 0);
            height = std::max(height, TextHeight(layout));
            AddText(layout, contentX, 0, contentWidth, quote > 0 ? 200 : 255, mask);
        }
        return height;
    }
    case aimd::BlockKind::Code:
        return LayoutCode(index, codeIndex, x, 0, w);
    case aimd::BlockKind::Rule:
        m_rects.push_back(RectItem{RECT{x, P(4.0F), x + w, P(4.0F) + std::max(1, P(1.0F))}, 0.0F, 0.16F});
        return P(9.0F);
    case aimd::BlockKind::Image:
        return LayoutImage(block, x, 0, w);
    case aimd::BlockKind::Table:
        return LayoutTable(block, x, 0, w);
    }
    return 0;
}

void AiAnswerView::Layout(int width, float scale, const std::wstring& bodyFace) {
    const std::wstring face = bodyFace.empty() ? std::wstring(L"Segoe UI") : bodyFace;
    const bool geometryChanged = width != m_layoutWidth || scale != m_layoutScale || face != m_face;
    if (!m_dirty && !geometryChanged) {
        return;
    }
    if (scale != m_layoutScale || face != m_face) {
        ReleaseFormats();
    }
    if (geometryChanged) {
        ReleaseLayout();
    }
    m_layoutWidth = width;
    m_layoutScale = scale;
    m_face = face;
    m_dirty = false;
    ++m_generation;
    m_height = 0;
    m_quoteBars.clear();
    if (width <= 0 || !EnsureFactory()) {
        ReleaseLayout();
        return;
    }
    auto P = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
    // Streaming only appends, so all but the last block or two are unchanged:
    // reuse their layouts (DirectWrite layout, hit rects, highlighting).
    std::vector<BlockLayout> next;
    next.reserve(Blocks().size());
    int y = 0;
    bool first = true;
    aimd::BlockKind previous = aimd::BlockKind::Paragraph;
    int previousQuote = 0;
    int previousBottom = 0;
    size_t codeIndex = 0;
    for (size_t index = 0; index < Blocks().size(); ++index) {
        const aimd::Block& block = Blocks()[index];
        if (!first) {
            int gap = P(10.0F);
            if (block.kind == aimd::BlockKind::ListItem && previous == aimd::BlockKind::ListItem) {
                gap = P(5.0F);
            } else if (block.kind == aimd::BlockKind::Heading) {
                gap = P(16.0F);
            } else if (previous == aimd::BlockKind::Heading) {
                gap = P(6.0F);
            }
            y += gap;
        }
        const int quote = std::min(block.quote, 4);
        const bool isCode = block.kind == aimd::BlockKind::Code;
        const bool copied = isCode && codeIndex == m_copiedCode;
        BlockLayout entry;
        if (index < m_layouts.size() && block.kind != aimd::BlockKind::Image && m_layouts[index].copied == copied &&
            m_layouts[index].block == block) {
            entry = std::move(m_layouts[index]);
            m_layouts[index].texts.clear();
        } else {
            const int x = quote * P(14.0F);
            entry.height = LayoutBlock(index, codeIndex, x, std::max(P(40.0F), width - x));
            entry.block = block;
            entry.copied = copied;
            entry.texts = std::move(m_texts);
            entry.rects = std::move(m_rects);
            entry.images = std::move(m_images);
            entry.links = std::move(m_links);
            entry.copies = std::move(m_copies);
            m_texts.clear();
            m_rects.clear();
            m_images.clear();
            m_links.clear();
            m_copies.clear();
        }
        entry.top = y;
        y += entry.height;
        // Quote bars, joined across consecutive quoted blocks.
        for (int level = 1; level <= quote; ++level) {
            const int barX = (level - 1) * P(14.0F) + P(1.0F);
            const int barTop = previousQuote >= level && !first ? previousBottom : entry.top;
            m_quoteBars.push_back(RectItem{RECT{barX, barTop, barX + std::max(2, P(3.0F)), y},
                static_cast<float>(P(1.5F)), 0.28F});
        }
        next.push_back(std::move(entry));
        codeIndex += isCode ? 1U : 0U;
        previous = block.kind;
        previousQuote = quote;
        previousBottom = y;
        first = false;
    }
    for (BlockLayout& stale : m_layouts) {
        ReleaseBlock(stale);
    }
    m_layouts = std::move(next);
    m_height = y + P(4.0F);
}

AiAnswerView::Hit AiAnswerView::HitTest(POINT point) const {
    auto inside = [](const RECT& r, LONG x, LONG y) {
        return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    };
    // First block whose bottom is below the point (blocks are sorted by top).
    auto block = std::partition_point(m_layouts.begin(), m_layouts.end(),
        [&](const BlockLayout& layout) { return layout.top + layout.height < point.y; });
    for (; block != m_layouts.end() && block->top <= point.y; ++block) {
        const LONG localY = point.y - block->top;
        for (const CopyArea& copy : block->copies) {
            if (inside(copy.rect, point.x, localY)) {
                return Hit{Hit::Kind::CopyCode, {}, copy.code};
            }
        }
        for (const LinkArea& link : block->links) {
            if (inside(link.rect, point.x, localY)) {
                return Hit{Hit::Kind::Link, link.url, 0};
            }
        }
    }
    return {};
}

bool AiAnswerView::RasterizeText(const TextItem& item, uint8_t* dest, int destWidth, int destHeight, int scroll,
    const AiInk& ink) {
    const int top = item.y - scroll;
    const int clipTop = std::max(0, top);
    const int clipBottom = std::min(destHeight, top + item.height);
    const int width = std::min(item.width, destWidth - item.x);
    if (clipTop >= clipBottom || width <= 0 || item.x < 0) {
        return true;
    }
    const int rows = clipBottom - clipTop;
    if (m_target == nullptr) {
        HDC screen = GetDC(nullptr);
        if (screen == nullptr) {
            return false;
        }
        const int initialWidth = std::max(width, 512);
        const int initialHeight = std::max(rows, 256);
        const HRESULT hr = m_interop->CreateBitmapRenderTarget(screen, static_cast<UINT32>(initialWidth),
            static_cast<UINT32>(initialHeight), &m_target);
        ReleaseDC(nullptr, screen);
        if (FAILED(hr) || m_target == nullptr) {
            m_target = nullptr;
            return false;
        }
        m_target->SetPixelsPerDip(1.0F);
        m_targetWidth = initialWidth;
        m_targetHeight = initialHeight;
    } else if (width > m_targetWidth || rows > m_targetHeight) {
        const int grownWidth = std::max(width, m_targetWidth);
        const int grownHeight = std::max(rows, m_targetHeight);
        if (FAILED(m_target->Resize(static_cast<UINT32>(grownWidth), static_cast<UINT32>(grownHeight)))) {
            return false;
        }
        m_targetWidth = grownWidth;
        m_targetHeight = grownHeight;
    }
    HDC memory = m_target->GetMemoryDC();
    const auto dib = static_cast<HBITMAP>(GetCurrentObject(memory, OBJ_BITMAP));
    BITMAP bitmap{};
    if (dib == nullptr || GetObjectW(dib, sizeof(bitmap), &bitmap) == 0 || bitmap.bmBits == nullptr ||
        bitmap.bmBitsPixel != 32) {
        return false;
    }
    auto* bits = static_cast<uint8_t*>(bitmap.bmBits);
    const size_t stride = static_cast<size_t>(bitmap.bmWidthBytes);
    m_scratch.resize(static_cast<size_t>(width) * static_cast<size_t>(rows) * 4U);
    PassRenderer renderer;
    renderer.target = m_target;
    renderer.params = m_params;
    for (uint8_t cls = 0; cls < kClassCount; ++cls) {
        if ((item.classMask & (1U << cls)) == 0) {
            continue;
        }
        for (int row = 0; row < rows; ++row) {
            std::memset(bits + static_cast<size_t>(row) * stride, 0, static_cast<size_t>(width) * 4U);
        }
        renderer.pass = cls;
        if (FAILED(item.layout->Draw(nullptr, &renderer, 0.0F, static_cast<FLOAT>(top - clipTop)))) {
            continue;
        }
        const Rgb color = ClassColor(cls, ink);
        const unsigned gray = ClassGray(cls, item.gray);
        for (int row = 0; row < rows; ++row) {
            const uint8_t* source = bits + static_cast<size_t>(row) * stride;
            uint8_t* target = m_scratch.data() + static_cast<size_t>(row) * static_cast<size_t>(width) * 4U;
            for (int column = 0; column < width; ++column, source += 4, target += 4) {
                // Linear grayscale coverage (gamma 1 rendering params) -> premultiplied colour.
                const unsigned coverage =
                    (static_cast<unsigned>(source[0]) * 19U + static_cast<unsigned>(source[1]) * 183U +
                        static_cast<unsigned>(source[2]) * 54U) >> 8U;
                const unsigned alpha = (coverage * gray + 127U) / 255U;
                target[0] = static_cast<uint8_t>((color.b * alpha + 127U) / 255U);
                target[1] = static_cast<uint8_t>((color.g * alpha + 127U) / 255U);
                target[2] = static_cast<uint8_t>((color.r * alpha + 127U) / 255U);
                target[3] = static_cast<uint8_t>(alpha);
            }
        }
        CompositePremul(dest, destWidth, destHeight, item.x, clipTop, m_scratch.data(), width, rows);
    }
    return true;
}

void AiAnswerView::Paint(uint8_t* dest, int destWidth, int destHeight, RECT viewport, int scroll, AiInk ink) {
    const int width = static_cast<int>(viewport.right - viewport.left);
    const int height = static_cast<int>(viewport.bottom - viewport.top);
    if (dest == nullptr || width <= 0 || height <= 0) {
        return;
    }
    const uint32_t inkKey = (static_cast<uint32_t>(ink.r) << 16U) | (static_cast<uint32_t>(ink.g) << 8U) | ink.b;
    const bool cached = m_layerGeneration == m_generation && m_layerScroll == scroll && m_layerSize.cx == width &&
        m_layerSize.cy == height && m_layerInk == inkKey &&
        m_layer.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4U;
    if (!cached) {
        m_layer.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4U, 0);
        uint8_t* layer = m_layer.data();
        auto fillRect = [&](const RectItem& item, int offset) {
            RECT rect = item.rect;
            rect.top -= offset;
            rect.bottom -= offset;
            if (rect.bottom <= 0 || rect.top >= height) {
                return;
            }
            if (item.radius <= 0.0F) {
                FillRectColorPremul(layer, width, height, rect, item.alpha, ink.b, ink.g, ink.r);
            } else {
                FillSquircleColorPremul(layer, width, height, rect, item.radius, item.alpha, ink.b, ink.g, ink.r);
            }
        };
        for (const RectItem& bar : m_quoteBars) {
            fillRect(bar, scroll);
        }
        // Only blocks overlapping [scroll, scroll + height); glyphs may overhang a few px.
        const int overhang = static_cast<int>(std::lround(6.0F * std::max(1.0F, m_layoutScale)));
        auto block = std::partition_point(m_layouts.begin(), m_layouts.end(),
            [&](const BlockLayout& layout) { return layout.top + layout.height + overhang <= scroll; });
        for (; block != m_layouts.end() && block->top - overhang < scroll + height; ++block) {
            const int offset = scroll - block->top;
            for (const RectItem& item : block->rects) {
                fillRect(item, offset);
            }
            for (const ImageItem& item : block->images) {
                const Image& image = m_imagesData[item.image];
                if (!image.scaled.empty()) {
                    CompositePremul(layer, width, height, static_cast<int>(item.rect.left),
                        static_cast<int>(item.rect.top) - offset, image.scaled.data(), image.scaledWidth,
                        image.scaledHeight);
                }
            }
            for (const TextItem& item : block->texts) {
                static_cast<void>(RasterizeText(item, layer, width, height, offset, ink));
            }
        }
        m_layerGeneration = m_generation;
        m_layerScroll = scroll;
        m_layerSize = {width, height};
        m_layerInk = inkKey;
    }
    CompositePremul(dest, destWidth, destHeight, static_cast<int>(viewport.left), static_cast<int>(viewport.top),
        m_layer.data(), width, height);
}
