#pragma once

// Rich rendering of an AI answer (Markdown) for the Search panel: DirectWrite
// layouts for every block, painted into a cached viewport-sized premultiplied
// BGRA layer so scrolling only re-rasterizes what is visible.

#include "AiMarkdown.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct IDWriteFactory;
struct IDWriteTextFormat;
struct IDWriteTextLayout;
struct IDWriteGdiInterop;
struct IDWriteRenderingParams;
struct IDWriteBitmapRenderTarget;
struct AiImageReply;

struct AiInk {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
};

class AiAnswerView {
public:
    struct Hit {
        enum class Kind : uint8_t { None, Link, CopyCode };
        Kind kind = Kind::None;
        std::wstring url;
        size_t code = 0;
    };

    AiAnswerView() = default;
    ~AiAnswerView();
    AiAnswerView(const AiAnswerView&) = delete;
    AiAnswerView& operator=(const AiAnswerView&) = delete;

    // Re-parses when the text changed.
    void SetMarkdown(const std::wstring& markdown);
    // Only these image URLs (from web search results) are fetched; any other
    // image renders as a link, so injected text can't trigger requests.
    void SetAllowedImages(const std::vector<std::wstring>& urls) { m_allowedImages = urls; }
    void Clear();
    [[nodiscard]] bool Empty() const noexcept { return m_parser.Blocks().empty(); }
    [[nodiscard]] const std::wstring& Markdown() const noexcept { return m_markdown; }

    // Image URLs (http/https) still to fetch; each is returned once.
    [[nodiscard]] std::vector<std::wstring> TakeImageRequests();
    // Returns true when the image belongs to this answer.
    bool SetImage(AiImageReply& image);

    void Layout(int width, float scale, const std::wstring& bodyFace);
    [[nodiscard]] int ContentHeight() const noexcept { return m_height; }
    // Paints content rows [scroll, scroll + viewport height) into `viewport` of dest.
    void Paint(uint8_t* dest, int destWidth, int destHeight, RECT viewport, int scroll, AiInk ink);
    // `point` is in content coordinates.
    [[nodiscard]] Hit HitTest(POINT point) const;
    [[nodiscard]] std::wstring CodeText(size_t code) const;
    void SetCopied(size_t code);
    void ClearCopied();

private:
    struct TextItem {
        IDWriteTextLayout* layout = nullptr;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        uint8_t gray = 255;
        uint8_t classMask = 1;
    };
    struct RectItem {
        RECT rect{};
        float radius = 0.0F;
        float alpha = 0.0F;
    };
    struct ImageItem {
        RECT rect{};
        size_t image = 0;
    };
    struct LinkArea {
        RECT rect{};
        std::wstring url;
    };
    struct CopyArea {
        RECT rect{};
        size_t code = 0;
    };
    // One block's layout in block-relative coordinates (y = 0 at the block top).
    // Kept across streaming updates while the block is unchanged.
    struct BlockLayout {
        aimd::Block block;
        bool copied = false;
        int top = 0;
        int height = 0;
        std::vector<TextItem> texts;
        std::vector<RectItem> rects;
        std::vector<ImageItem> images;
        std::vector<LinkArea> links;
        std::vector<CopyArea> copies;
    };
    struct Image {
        std::wstring url;
        bool requested = false;
        bool failed = false;
        UINT width = 0;
        UINT height = 0;
        std::vector<uint8_t> pixels;
        int scaledWidth = 0;
        int scaledHeight = 0;
        std::vector<uint8_t> scaled;
    };

    void ReleaseLayout() noexcept;
    static void ReleaseBlock(BlockLayout& layout) noexcept;
    // Lays out Blocks()[index] at y = 0 into the scratch item lists; returns its height.
    int LayoutBlock(size_t index, size_t codeIndex, int x, int width);
    void ReleaseFormats() noexcept;
    bool EnsureFactory();
    IDWriteTextFormat* Format(float size, int weight, bool mono);
    IDWriteTextLayout* MakeLayout(const std::wstring& text, float size, int weight, bool mono, int maxWidth);
    IDWriteTextLayout* InlineLayout(const aimd::InlineText& text, float size, int weight, int maxWidth,
        uint8_t& classMask);
    // Inline-code chips and link hit areas for a layout placed at (x, y).
    void AddInlineDecorations(const aimd::InlineText& text, IDWriteTextLayout* layout, int x, int y);
    void AddText(IDWriteTextLayout* layout, int x, int y, int width, uint8_t gray, uint8_t classMask);
    int LayoutCode(size_t blockIndex, size_t codeIndex, int x, int y, int width);
    int LayoutTable(const aimd::Block& block, int x, int y, int width);
    int LayoutImage(const aimd::Block& block, int x, int y, int width);
    size_t ImageIndex(const std::wstring& url);
    bool RasterizeText(const TextItem& item, uint8_t* dest, int destWidth, int destHeight, int scroll,
        const AiInk& ink);

    std::wstring m_markdown;
    aimd::IncrementalParser m_parser;
    [[nodiscard]] const std::vector<aimd::Block>& Blocks() const noexcept { return m_parser.Blocks(); }
    std::vector<Image> m_imagesData;
    std::vector<std::wstring> m_allowedImages;
    size_t m_copiedCode = static_cast<size_t>(-1);

    bool m_dirty = true;
    int m_layoutWidth = 0;
    float m_layoutScale = 0.0F;
    std::wstring m_face;
    std::wstring m_monoFace;
    int m_height = 0;
    std::vector<BlockLayout> m_layouts;
    // Quote bars span block gaps, so they live in content coordinates.
    std::vector<RectItem> m_quoteBars;
    // Scratch output of LayoutBlock, moved into a BlockLayout.
    std::vector<TextItem> m_texts;
    std::vector<RectItem> m_rects;
    std::vector<ImageItem> m_images;
    std::vector<LinkArea> m_links;
    std::vector<CopyArea> m_copies;

    IDWriteFactory* m_factory = nullptr;
    IDWriteGdiInterop* m_interop = nullptr;
    IDWriteRenderingParams* m_params = nullptr;
    IDWriteBitmapRenderTarget* m_target = nullptr;
    int m_targetWidth = 0;
    int m_targetHeight = 0;
    std::unordered_map<std::wstring, IDWriteTextFormat*> m_formats;

    // Cached rendered viewport (caret blinks repaint the panel, not the answer).
    std::vector<uint8_t> m_layer;
    std::vector<uint8_t> m_scratch;
    uint64_t m_generation = 1;
    uint64_t m_layerGeneration = 0;
    int m_layerScroll = -1;
    SIZE m_layerSize{};
    uint32_t m_layerInk = 0;
};
