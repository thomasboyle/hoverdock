#pragma once

// Markdown subset for AI answers in Search: parsed into flat blocks with
// styled inline runs so the DirectWrite view (AiAnswerView) can lay them out.
// Portable (std only): unit-tested off Windows.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aimd {

enum SpanFlags : uint8_t {
    kBold = 1U << 0U,
    kItalic = 1U << 1U,
    kCode = 1U << 2U,
    kStrike = 1U << 3U,
    kLink = 1U << 4U,
};

// A styled run inside InlineText::text. Unstyled text has no span.
struct Span {
    size_t start = 0;
    size_t length = 0;
    uint8_t flags = 0;
    std::wstring url;
};

struct InlineText {
    std::wstring text;
    std::vector<Span> spans;
};

enum class BlockKind : uint8_t { Paragraph, Heading, ListItem, Code, Rule, Image, Table };

enum class Align : uint8_t { Left, Center, Right };

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    // Heading: 1..6. ListItem: nesting depth from 0.
    int level = 0;
    // Blockquote nesting depth (0 = not quoted).
    int quote = 0;
    // ListItem marker: "•", "◦", "▪", "3.", "☐", "☑".
    std::wstring marker;
    // Paragraph / Heading / ListItem text; Image alt text.
    InlineText text;
    // Code block body and its fence language (lower-case).
    std::wstring code;
    std::wstring language;
    // Image source.
    std::wstring url;
    // Table: rows[0] is the header row; every row has align.size() cells.
    std::vector<std::vector<InlineText>> rows;
    std::vector<Align> align;
};

// Inline images found while parsing a run of text (hoisted to Image blocks).
struct InlineImage {
    std::wstring alt;
    std::wstring url;
};

[[nodiscard]] std::vector<Block> Parse(std::wstring_view markdown);
[[nodiscard]] InlineText ParseInline(std::wstring_view text, std::vector<InlineImage>* images = nullptr);

// True for URLs the UI may open or fetch: http(s) and mailto only.
[[nodiscard]] bool IsSafeUrl(std::wstring_view url);

// ---- code highlighting ----
enum class TokenClass : uint8_t { Plain, Comment, String, Keyword, Number };

struct Token {
    size_t start = 0;
    size_t length = 0;
    TokenClass cls = TokenClass::Plain;
};

// Lightweight lexer for fenced code (comments, strings, numbers, keywords).
// Returns only non-plain tokens, ordered and non-overlapping.
[[nodiscard]] std::vector<Token> HighlightCode(std::wstring_view code, std::wstring_view language);

}  // namespace aimd
