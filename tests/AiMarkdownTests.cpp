// AiMarkdown unit tests (portable; also built on Windows with -DDOCK_BUILD_TESTS=ON).

#include "AiMarkdown.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                              \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (false)

const aimd::Span* SpanAt(const aimd::InlineText& text, size_t position) {
    for (const aimd::Span& span : text.spans) {
        if (position >= span.start && position < span.start + span.length) {
            return &span;
        }
    }
    return nullptr;
}

void TestInlineStyles() {
    const aimd::InlineText t = aimd::ParseInline(L"a **bold** and *it* `co*de*` ~~gone~~ snake_case_name");
    EXPECT(t.text == L"a bold and it co*de* gone snake_case_name");
    const size_t bold = t.text.find(L"bold");
    EXPECT(SpanAt(t, bold) != nullptr && SpanAt(t, bold)->flags == aimd::kBold);
    const size_t it = t.text.find(L"it ");
    EXPECT(SpanAt(t, it) != nullptr && SpanAt(t, it)->flags == aimd::kItalic);
    const size_t code = t.text.find(L"co*de*");
    EXPECT(SpanAt(t, code) != nullptr && SpanAt(t, code)->flags == aimd::kCode &&
        SpanAt(t, code)->length == 6);
    const size_t gone = t.text.find(L"gone");
    EXPECT(SpanAt(t, gone) != nullptr && SpanAt(t, gone)->flags == aimd::kStrike);
    EXPECT(SpanAt(t, t.text.find(L"snake")) == nullptr);
}

void TestNestedEmphasis() {
    const aimd::InlineText t = aimd::ParseInline(L"*a **b** c* and ***both***");
    EXPECT(t.text == L"a b c and both");
    EXPECT(SpanAt(t, 0)->flags == aimd::kItalic);
    EXPECT(SpanAt(t, 2)->flags == (aimd::kItalic | aimd::kBold));
    const size_t both = t.text.find(L"both");
    EXPECT(SpanAt(t, both)->flags == (aimd::kItalic | aimd::kBold));
    const aimd::InlineText math = aimd::ParseInline(L"2 * 3 * 4");
    EXPECT(math.text == L"2 * 3 * 4" && math.spans.empty());
}

void TestLinks() {
    const aimd::InlineText t = aimd::ParseInline(
        L"See [the **docs**](https://example.com/a_(b)) or https://x.io/path. Also <https://y.dev> and [1].");
    EXPECT(t.text == L"See the docs or https://x.io/path. Also https://y.dev and [1].");
    const aimd::Span* docs = SpanAt(t, t.text.find(L"docs"));
    EXPECT(docs != nullptr && docs->flags == (aimd::kLink | aimd::kBold) &&
        docs->url == L"https://example.com/a_(b)");
    const aimd::Span* bare = SpanAt(t, t.text.find(L"https://x.io"));
    EXPECT(bare != nullptr && bare->url == L"https://x.io/path" && bare->length == 17);
    EXPECT(SpanAt(t, t.text.find(L"[1]")) == nullptr);
    const aimd::InlineText unsafe = aimd::ParseInline(L"[click](javascript:alert(1)) [f](file:///c:/x)");
    EXPECT(unsafe.text == L"click f");
    EXPECT(unsafe.spans.empty());
    const aimd::InlineText cite = aimd::ParseInline(L"Fact\u30104\u2020L1-L9\u3011 here");
    EXPECT(cite.text == L"Fact here");
}

void TestBlocks() {
    const std::wstring md =
        L"# Title\n"
        L"Some *text*\nwrapped line.\n\n"
        L"- one\n"
        L"- two\n"
        L"  - nested\n"
        L"3. third\n"
        L"- [x] done\n\n"
        L"> quoted **bold**\n\n"
        L"```Python\n"
        L"def f():\n"
        L"    return \"hi\"  # note\n"
        L"```\n"
        L"---\n"
        L"| Name | Qty |\n"
        L"|:-----|----:|\n"
        L"| a | 1 |\n"
        L"| `b|c` | 2 |\n\n"
        L"![cat](https://img.example/cat.png)\n\n"
        L"Sub\n===\n";
    const std::vector<aimd::Block> blocks = aimd::Parse(md);
    size_t i = 0;
    EXPECT(blocks.size() == 13);
    if (blocks.size() != 13) {
        return;
    }
    EXPECT(blocks[i].kind == aimd::BlockKind::Heading && blocks[i].level == 1 && blocks[i].text.text == L"Title");
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::Paragraph && blocks[i].text.text == L"Some text wrapped line.");
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::ListItem && blocks[i].marker == L"\u2022" && blocks[i].level == 0);
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::ListItem && blocks[i].text.text == L"two");
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::ListItem && blocks[i].level == 1 && blocks[i].marker == L"\u25E6");
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::ListItem && blocks[i].marker == L"3." && blocks[i].level == 0);
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::ListItem && blocks[i].marker == L"\u2611" &&
        blocks[i].text.text == L"done");
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::Paragraph && blocks[i].quote == 1 &&
        blocks[i].text.text == L"quoted bold");
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::Code && blocks[i].language == L"python" &&
        blocks[i].code == L"def f():\n    return \"hi\"  # note");
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::Rule);
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::Table && blocks[i].rows.size() == 3 && blocks[i].align.size() == 2 &&
        blocks[i].align[1] == aimd::Align::Right && blocks[i].rows[2][0].text == L"b|c");
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::Image && blocks[i].url == L"https://img.example/cat.png" &&
        blocks[i].text.text == L"cat");
    ++i;
    EXPECT(blocks[i].kind == aimd::BlockKind::Heading && blocks[i].level == 1 && blocks[i].text.text == L"Sub");
}

void TestStreamingFence() {
    // An unterminated fence (mid-stream) still renders as code.
    const std::vector<aimd::Block> blocks = aimd::Parse(L"Intro\n\n```js\nconst a = 1;\n");
    EXPECT(blocks.size() == 2);
    if (blocks.size() == 2) {
        EXPECT(blocks[1].kind == aimd::BlockKind::Code && blocks[1].code == L"const a = 1;");
    }
}

void TestHighlight() {
    const std::wstring code = L"def f(x):\n    return \"a#b\" + 42  # done";
    const std::vector<aimd::Token> tokens = aimd::HighlightCode(code, L"python");
    bool keyword = false;
    bool string = false;
    bool number = false;
    bool comment = false;
    for (const aimd::Token& token : tokens) {
        const std::wstring text = code.substr(token.start, token.length);
        keyword |= token.cls == aimd::TokenClass::Keyword && text == L"def";
        string |= token.cls == aimd::TokenClass::String && text == L"\"a#b\"";
        number |= token.cls == aimd::TokenClass::Number && text == L"42";
        comment |= token.cls == aimd::TokenClass::Comment && text == L"# done";
    }
    EXPECT(keyword && string && number && comment);
    EXPECT(aimd::HighlightCode(L"plain words", L"text").empty());
    const std::vector<aimd::Token> sql = aimd::HighlightCode(L"SELECT * FROM t -- x", L"sql");
    EXPECT(sql.size() == 3 && sql[0].cls == aimd::TokenClass::Keyword && sql[2].cls == aimd::TokenClass::Comment);
}

void TestBlockEquality() {
    // Appending text only changes the last block (streaming reuses the rest).
    const std::wstring head = L"# T\n\nPara **b** [l](https://x.y)\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n- item";
    const std::vector<aimd::Block> before = aimd::Parse(head);
    const std::vector<aimd::Block> after = aimd::Parse(head + L" more");
    EXPECT(before.size() == after.size() && before.size() == 4);
    if (before.size() == 4 && after.size() == 4) {
        EXPECT(before[0] == after[0] && before[1] == after[1] && before[2] == after[2]);
        EXPECT(!(before[3] == after[3]));
    }
}

// Streaming equivalence: IncrementalParser::Update(prefix) == Parse(prefix) for
// every prefix of random structured Markdown cut into random chunks.
void TestIncrementalParser(unsigned seed, int documents) {
    static const wchar_t* const kPieces[] = {L"# Title\n", L"## Sub\n", L"Para **bold** and `code` ",
        L"[link](https://a.b) ", L"text\n", L"\n", L"\n\n", L"- item\n", L"  - nested\n", L"    - deep\n",
        L"1. one\n", L"2) two\n", L"- [x] done\n", L"> quote\n", L"> > deeper\n", L"```py\n", L"```\n",
        L"~~~\n", L"x = 1  # c\n", L"$$\n", L"$$ a $$\n", L"| a | b |\n", L"|---|:-:|\n", L"| 1 | 2 |\n",
        L"---\n", L"***\n", L"Setext\n===\n", L"Under\n--\n", L"![img](https://i.x/a.png)\n", L"  indented cont\n",
        L"line with two spaces  \n", L"trailing\\\n", L"*a **b** c*", L"_snake_case_ ", L"\t- tab item\n",
        L"\r\n", L"\u4E2D\u6587 ", L"\U0001F600", L"<br>", L"| x |\n"};
    std::mt19937 rng(seed);
    for (int doc = 0; doc < documents; ++doc) {
        std::wstring text;
        const int pieces = 1 + static_cast<int>(rng() % 40);
        for (int i = 0; i < pieces; ++i) {
            text += kPieces[rng() % (sizeof(kPieces) / sizeof(kPieces[0]))];
        }
        aimd::IncrementalParser incremental;
        size_t at = 0;
        while (at < text.size()) {
            at = std::min(text.size(), at + 1 + rng() % 12);
            const std::wstring prefix = text.substr(0, at);
            if (!(incremental.Update(prefix) == aimd::Parse(prefix))) {
                std::fprintf(stderr, "incremental mismatch: seed %u doc %d at %zu\n", seed, doc, at);
                ++g_failures;
                return;
            }
        }
        // A non-append edit falls back to a full parse.
        const std::wstring edited = L"Different start\n\n" + text;
        EXPECT(incremental.Update(edited) == aimd::Parse(edited));
    }
}

void TestSafeUrl() {
    EXPECT(aimd::IsSafeUrl(L"https://a.b"));
    EXPECT(aimd::IsSafeUrl(L"mailto:x@y.z"));
    EXPECT(!aimd::IsSafeUrl(L"file:///C:/Windows"));
    EXPECT(!aimd::IsSafeUrl(L"javascript:alert(1)"));
    EXPECT(!aimd::IsSafeUrl(L"https://a b"));
    EXPECT(!aimd::IsSafeUrl(L"\\\\server\\share"));
}

}  // namespace

int main() {
    TestInlineStyles();
    TestNestedEmphasis();
    TestLinks();
    TestBlocks();
    TestStreamingFence();
    TestHighlight();
    TestSafeUrl();
    TestBlockEquality();
    const char* fuzzDocs = std::getenv("AIMD_FUZZ_DOCS");
    TestIncrementalParser(1, fuzzDocs != nullptr ? std::atoi(fuzzDocs) : 400);
    if (g_failures == 0) {
        std::printf("AiMarkdownTests: all passed\n");
        return 0;
    }
    std::fprintf(stderr, "AiMarkdownTests: %d failure(s)\n", g_failures);
    return 1;
}
