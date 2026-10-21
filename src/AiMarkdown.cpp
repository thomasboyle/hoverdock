#include "AiMarkdown.h"

#include <algorithm>
#include <cwctype>
#include <unordered_set>

namespace aimd {
namespace {

constexpr int kMaxInlineDepth = 8;
constexpr int kMaxQuoteDepth = 4;

bool IsSpace(wchar_t c) noexcept {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f' || c == L'\v';
}

bool IsAlnum(wchar_t c) noexcept {
    return std::iswalnum(static_cast<wint_t>(c)) != 0;
}

bool IsAsciiPunct(wchar_t c) noexcept {
    return c < 128 && std::iswpunct(static_cast<wint_t>(c)) != 0;
}

std::wstring_view TrimView(std::wstring_view text) {
    size_t first = 0;
    size_t last = text.size();
    while (first < last && IsSpace(text[first])) {
        ++first;
    }
    while (last > first && IsSpace(text[last - 1])) {
        --last;
    }
    return text.substr(first, last - first);
}

std::wstring Lower(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& c : out) {
        c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    }
    return out;
}

bool StartsWith(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool StartsWithNoCase(std::wstring_view text, std::wstring_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(text[i])) != std::towlower(static_cast<wint_t>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// ---- inline -------------------------------------------------------------------

class InlineParser {
public:
    InlineParser(InlineText& out, std::vector<InlineImage>* images) : m_out(out), m_images(images) {}

    void Run(std::wstring_view s, uint8_t flags, const std::wstring& url, int depth) {
        size_t i = 0;
        const size_t n = s.size();
        while (i < n) {
            const wchar_t c = s[i];
            if (c == L'\\' && i + 1 < n && IsAsciiPunct(s[i + 1])) {
                Emit(s.substr(i + 1, 1), flags, url);
                i += 2;
                continue;
            }
            if (c == L'`') {
                i = CodeSpan(s, i, flags, url);
                continue;
            }
            if (c == L'!' && i + 1 < n && s[i + 1] == L'[') {
                std::wstring_view label;
                std::wstring target;
                size_t end = 0;
                if (ParseLink(s, i + 1, label, target, end)) {
                    if (m_images != nullptr && IsSafeUrl(target)) {
                        m_images->push_back({std::wstring(TrimView(label)), target});
                    } else if (IsSafeUrl(target)) {
                        Emit(label.empty() ? std::wstring_view(L"image") : label, flags | kLink, target);
                    } else {
                        Emit(label, flags, url);
                    }
                    i = end;
                    continue;
                }
            }
            if (c == L'[' && depth < kMaxInlineDepth) {
                std::wstring_view label;
                std::wstring target;
                size_t end = 0;
                if (ParseLink(s, i, label, target, end)) {
                    if (IsSafeUrl(target) && (flags & kLink) == 0) {
                        Run(label, static_cast<uint8_t>(flags | kLink), target, depth + 1);
                    } else {
                        Run(label, flags, url, depth + 1);
                    }
                    i = end;
                    continue;
                }
            }
            if (c == L'<') {
                const size_t close = s.find(L'>', i + 1);
                if (close != std::wstring_view::npos && close - i < 2048) {
                    const std::wstring_view inner = s.substr(i + 1, close - i - 1);
                    const std::wstring lowered = Lower(inner);
                    if (lowered == L"br" || lowered == L"br/" || lowered == L"br /") {
                        Emit(L"\n", flags, url);
                        i = close + 1;
                        continue;
                    }
                    if (IsSafeUrl(inner) && inner.find(L' ') == std::wstring_view::npos) {
                        Emit(inner, (flags & kLink) != 0 ? flags : static_cast<uint8_t>(flags | kLink),
                            (flags & kLink) != 0 ? url : std::wstring(inner));
                        i = close + 1;
                        continue;
                    }
                }
            }
            if ((c == L'h' || c == L'w') && (flags & kLink) == 0 && (i == 0 || !IsAlnum(s[i - 1]))) {
                const size_t end = BareUrlEnd(s, i);
                if (end > i) {
                    const std::wstring_view raw = s.substr(i, end - i);
                    std::wstring target(raw);
                    if (StartsWithNoCase(raw, L"www.")) {
                        target = L"https://" + target;
                    }
                    Emit(raw, static_cast<uint8_t>(flags | kLink), target);
                    i = end;
                    continue;
                }
            }
            if (c == 0x3010) {  // 【4†L1-L9】 citation marks from browser-search models
                const size_t close = s.find(static_cast<wchar_t>(0x3011), i + 1);
                if (close != std::wstring_view::npos && close - i <= 48) {
                    i = close + 1;
                    continue;
                }
            }
            if ((c == L'*' || c == L'_' || c == L'~') && depth < kMaxInlineDepth) {
                const size_t next = Emphasis(s, i, flags, url, depth);
                if (next != i) {
                    i = next;
                    continue;
                }
                size_t run = i;
                while (run < n && s[run] == c) {
                    ++run;
                }
                Emit(s.substr(i, run - i), flags, url);
                i = run;
                continue;
            }
            Emit(s.substr(i, 1), flags, url);
            ++i;
        }
    }

private:
    void Emit(std::wstring_view chars, uint8_t flags, const std::wstring& url) {
        if (chars.empty()) {
            return;
        }
        const size_t start = m_out.text.size();
        m_out.text.append(chars);
        if (flags == 0) {
            return;
        }
        if (!m_out.spans.empty()) {
            Span& last = m_out.spans.back();
            if (last.start + last.length == start && last.flags == flags && last.url == url) {
                last.length += chars.size();
                return;
            }
        }
        m_out.spans.push_back({start, chars.size(), flags, (flags & kLink) != 0 ? url : std::wstring()});
    }

    size_t CodeSpan(std::wstring_view s, size_t i, uint8_t flags, const std::wstring& url) {
        size_t run = i;
        while (run < s.size() && s[run] == L'`') {
            ++run;
        }
        const size_t ticks = run - i;
        size_t j = run;
        while (j < s.size()) {
            if (s[j] != L'`') {
                ++j;
                continue;
            }
            size_t closeRun = j;
            while (closeRun < s.size() && s[closeRun] == L'`') {
                ++closeRun;
            }
            if (closeRun - j == ticks) {
                std::wstring content(s.substr(run, j - run));
                std::replace(content.begin(), content.end(), L'\n', L' ');
                if (content.size() >= 2 && content.front() == L' ' && content.back() == L' ' &&
                    content.find_first_not_of(L' ') != std::wstring::npos) {
                    content = content.substr(1, content.size() - 2);
                }
                Emit(content, static_cast<uint8_t>(flags | kCode), url);
                return closeRun;
            }
            j = closeRun;
        }
        Emit(s.substr(i, ticks), flags, url);
        return run;
    }

    // [label](target "title") starting at the '['.
    static bool ParseLink(std::wstring_view s, size_t open, std::wstring_view& label, std::wstring& target,
        size_t& end) {
        int nesting = 0;
        size_t close = std::wstring_view::npos;
        for (size_t j = open; j < s.size(); ++j) {
            const wchar_t c = s[j];
            if (c == L'\\') {
                ++j;
            } else if (c == L'`') {
                const size_t tick = s.find(L'`', j + 1);
                if (tick != std::wstring_view::npos) {
                    j = tick;
                }
            } else if (c == L'[') {
                ++nesting;
            } else if (c == L']') {
                if (--nesting == 0) {
                    close = j;
                    break;
                }
            }
        }
        if (close == std::wstring_view::npos || close + 1 >= s.size() || s[close + 1] != L'(') {
            return false;
        }
        size_t j = close + 2;
        while (j < s.size() && s[j] == L' ') {
            ++j;
        }
        std::wstring dest;
        if (j < s.size() && s[j] == L'<') {
            const size_t gt = s.find(L'>', j + 1);
            if (gt == std::wstring_view::npos) {
                return false;
            }
            dest = std::wstring(s.substr(j + 1, gt - j - 1));
            j = gt + 1;
        } else {
            int parens = 0;
            const size_t from = j;
            while (j < s.size() && !IsSpace(s[j])) {
                if (s[j] == L'(') {
                    ++parens;
                } else if (s[j] == L')') {
                    if (parens == 0) {
                        break;
                    }
                    --parens;
                }
                ++j;
            }
            dest = std::wstring(s.substr(from, j - from));
        }
        while (j < s.size() && s[j] == L' ') {
            ++j;
        }
        if (j < s.size() && (s[j] == L'"' || s[j] == L'\'')) {
            const size_t quote = s.find(s[j], j + 1);
            if (quote == std::wstring_view::npos) {
                return false;
            }
            j = quote + 1;
            while (j < s.size() && s[j] == L' ') {
                ++j;
            }
        }
        if (j >= s.size() || s[j] != L')') {
            return false;
        }
        label = s.substr(open + 1, close - open - 1);
        target = std::move(dest);
        end = j + 1;
        return true;
    }

    static size_t BareUrlEnd(std::wstring_view s, size_t i) {
        const std::wstring_view rest = s.substr(i);
        if (!StartsWithNoCase(rest, L"https://") && !StartsWithNoCase(rest, L"http://") &&
            !StartsWithNoCase(rest, L"www.")) {
            return i;
        }
        size_t j = i;
        int parens = 0;
        while (j < s.size() && !IsSpace(s[j]) && s[j] != L'<' && s[j] != L'>' && s[j] != L'"' &&
               s[j] != L'`' && s[j] != 0x3010) {
            if (s[j] == L'(') {
                ++parens;
            } else if (s[j] == L')') {
                if (parens == 0) {
                    break;
                }
                --parens;
            }
            ++j;
        }
        while (j > i && std::wstring_view(L".,:;!?'*_").find(s[j - 1]) != std::wstring_view::npos) {
            --j;
        }
        const size_t minimum = StartsWithNoCase(rest, L"www.") ? 5 : 9;
        return j - i >= minimum ? j : i;
    }

    // Finds the closing delimiter run of exactly `length` `c`s, skipping code
    // spans and escapes. Returns npos when there is none.
    static size_t FindClosing(std::wstring_view s, size_t from, wchar_t c, size_t length) {
        size_t j = from;
        while (j < s.size()) {
            const wchar_t ch = s[j];
            if (ch == L'\\') {
                j += 2;
                continue;
            }
            if (ch == L'`') {
                size_t run = j;
                while (run < s.size() && s[run] == L'`') {
                    ++run;
                }
                const std::wstring_view ticks = s.substr(j, run - j);
                const size_t close = s.find(ticks, run);
                j = close == std::wstring_view::npos ? run : close + ticks.size();
                continue;
            }
            if (ch != c) {
                ++j;
                continue;
            }
            size_t run = j;
            while (run < s.size() && s[run] == c) {
                ++run;
            }
            const size_t runLength = run - j;
            // A longer run can close a shorter opener only from its end ("**a*" style is rare).
            if (runLength == length && j > from && !IsSpace(s[j - 1]) &&
                (c != L'_' || run >= s.size() || !IsAlnum(s[run]))) {
                return j;
            }
            j = run;
        }
        return std::wstring_view::npos;
    }

    size_t Emphasis(std::wstring_view s, size_t i, uint8_t flags, const std::wstring& url, int depth) {
        const wchar_t c = s[i];
        size_t run = i;
        while (run < s.size() && s[run] == c) {
            ++run;
        }
        const size_t available = run - i;
        if (run >= s.size() || IsSpace(s[run])) {
            return i;
        }
        if (c == L'_' && i > 0 && IsAlnum(s[i - 1])) {
            return i;  // snake_case
        }
        if (c == L'~') {
            if (available != 2) {
                return i;
            }
            const size_t close = FindClosing(s, run, c, 2);
            if (close == std::wstring_view::npos) {
                return i;
            }
            Run(s.substr(run, close - run), static_cast<uint8_t>(flags | kStrike), url, depth + 1);
            return close + 2;
        }
        for (size_t length = std::min<size_t>(available, 3); length >= 1; --length) {
            const size_t close = FindClosing(s, i + length, c, length);
            if (close == std::wstring_view::npos) {
                continue;
            }
            const auto style = static_cast<uint8_t>(length == 1 ? kItalic : length == 2 ? kBold : (kBold | kItalic));
            // Leftover opening markers (e.g. "***a*") stay literal.
            if (available > length) {
                Emit(s.substr(i, available - length), flags, url);
            }
            Run(s.substr(i + available, close - (i + available)), static_cast<uint8_t>(flags | style), url,
                depth + 1);
            return close + length;
        }
        return i;
    }

    InlineText& m_out;
    std::vector<InlineImage>* m_images;
};

// ---- blocks -------------------------------------------------------------------

size_t IndentOf(std::wstring_view line) {
    size_t columns = 0;
    for (const wchar_t c : line) {
        if (c == L' ') {
            ++columns;
        } else if (c == L'\t') {
            columns += 4 - columns % 4;
        } else {
            break;
        }
    }
    return columns;
}

std::wstring_view LeftTrim(std::wstring_view line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) {
        ++i;
    }
    return line.substr(i);
}

bool IsBlank(std::wstring_view line) {
    return TrimView(line).empty();
}

bool IsRule(std::wstring_view line) {
    const std::wstring_view t = TrimView(line);
    if (t.size() < 3) {
        return false;
    }
    const wchar_t c = t[0];
    if (c != L'-' && c != L'*' && c != L'_') {
        return false;
    }
    size_t count = 0;
    for (const wchar_t ch : t) {
        if (ch == c) {
            ++count;
        } else if (ch != L' ') {
            return false;
        }
    }
    return count >= 3;
}

bool IsFence(std::wstring_view line, wchar_t& fenceChar, size_t& fenceLength) {
    const std::wstring_view t = LeftTrim(line);
    if (t.size() < 3 || (t[0] != L'`' && t[0] != L'~')) {
        return false;
    }
    size_t n = 0;
    while (n < t.size() && t[n] == t[0]) {
        ++n;
    }
    if (n < 3) {
        return false;
    }
    if (t[0] == L'`' && t.substr(n).find(L'`') != std::wstring_view::npos) {
        return false;
    }
    fenceChar = t[0];
    fenceLength = n;
    return true;
}

bool IsHeading(std::wstring_view line, int& level, std::wstring_view& text) {
    const std::wstring_view t = LeftTrim(line);
    size_t n = 0;
    while (n < t.size() && t[n] == L'#') {
        ++n;
    }
    if (n == 0 || n > 6 || (n < t.size() && t[n] != L' ' && t[n] != L'\t')) {
        return false;
    }
    std::wstring_view body = TrimView(t.substr(n));
    size_t end = body.size();
    while (end > 0 && body[end - 1] == L'#') {
        --end;
    }
    if (end < body.size() && (end == 0 || body[end - 1] == L' ')) {
        body = TrimView(body.substr(0, end));
    }
    level = static_cast<int>(n);
    text = body;
    return true;
}

struct ListMarker {
    size_t indent = 0;
    size_t contentColumn = 0;
    bool ordered = false;
    std::wstring number;
    std::wstring_view content;
};

bool IsListItem(std::wstring_view line, ListMarker& marker) {
    const size_t indent = IndentOf(line);
    const std::wstring_view t = LeftTrim(line);
    if (t.empty()) {
        return false;
    }
    size_t markerEnd = 0;
    if (t[0] == L'-' || t[0] == L'*' || t[0] == L'+') {
        markerEnd = 1;
        marker.ordered = false;
    } else {
        size_t digits = 0;
        while (digits < t.size() && digits < 9 && t[digits] >= L'0' && t[digits] <= L'9') {
            ++digits;
        }
        if (digits == 0 || digits >= t.size() || (t[digits] != L'.' && t[digits] != L')')) {
            return false;
        }
        marker.ordered = true;
        marker.number = std::wstring(t.substr(0, digits));
        markerEnd = digits + 1;
    }
    if (markerEnd < t.size() && t[markerEnd] != L' ' && t[markerEnd] != L'\t') {
        return false;
    }
    if (markerEnd >= t.size() && !marker.ordered) {
        marker.content = {};
    } else {
        marker.content = TrimView(t.substr(std::min(markerEnd, t.size())));
    }
    marker.indent = indent;
    marker.contentColumn = indent + markerEnd + 1;
    return true;
}

bool IsTableDelimiter(std::wstring_view line) {
    const std::wstring_view t = TrimView(line);
    if (t.empty() || t.find(L'-') == std::wstring_view::npos) {
        return false;
    }
    bool sawPipe = false;
    for (const wchar_t c : t) {
        if (c == L'|') {
            sawPipe = true;
        } else if (c != L'-' && c != L':' && c != L' ' && c != L'\t') {
            return false;
        }
    }
    return sawPipe || t.find(L"--") != std::wstring_view::npos;
}

std::vector<std::wstring_view> SplitRow(std::wstring_view line) {
    std::wstring_view t = TrimView(line);
    if (!t.empty() && t.front() == L'|') {
        t.remove_prefix(1);
    }
    if (!t.empty() && t.back() == L'|' && (t.size() < 2 || t[t.size() - 2] != L'\\')) {
        t.remove_suffix(1);
    }
    std::vector<std::wstring_view> cells;
    size_t start = 0;
    bool inCode = false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == L'\\') {
            ++i;
            continue;
        }
        if (t[i] == L'`') {
            inCode = !inCode;
        } else if (t[i] == L'|' && !inCode) {
            cells.push_back(TrimView(t.substr(start, i - start)));
            start = i + 1;
        }
    }
    cells.push_back(TrimView(t.substr(start)));
    return cells;
}

// Source lines; at the top level it records how far the parser has looked, so
// an incremental update knows which earlier decisions cannot change.
struct LineList {
    std::vector<std::wstring_view> lines;
    bool track = false;
    mutable size_t seen = 0;  // highest index read + 1

    std::wstring_view operator[](size_t i) const {
        if (track && i + 1 > seen) {
            seen = i + 1;
        }
        return lines[i];
    }
    [[nodiscard]] size_t size() const noexcept { return lines.size(); }
};

bool StartsTable(const LineList& lines, size_t i) {
    return i + 1 < lines.size() && lines[i].find(L'|') != std::wstring_view::npos &&
        IsTableDelimiter(lines[i + 1]) && SplitRow(lines[i]).size() >= 1;
}

}  // namespace

struct ParseCheckpoint {
    size_t line = 0;
    std::vector<size_t> listIndents;
    size_t blocks = 0;
    size_t seen = 0;
};

namespace {

class BlockParser {
public:
    explicit BlockParser(std::vector<Block>& out, std::vector<ParseCheckpoint>* checkpoints = nullptr)
        : m_out(out), m_checkpoints(checkpoints) {}

    void Parse(const LineList& lines, int quote, size_t start = 0, std::vector<size_t> listIndents = {}) {
        size_t i = start;
        while (i < lines.size()) {
            if (quote == 0 && m_checkpoints != nullptr) {
                m_checkpoints->push_back({i, listIndents, m_out.size(), lines.seen});
            }
            const std::wstring_view line = lines[i];
            if (IsBlank(line)) {
                ++i;
                continue;
            }
            ListMarker marker;
            const bool listItem = IsListItem(line, marker) && !IsRule(line);
            if (!listItem) {
                listIndents.clear();
            }

            wchar_t fenceChar = 0;
            size_t fenceLength = 0;
            if (IsFence(line, fenceChar, fenceLength)) {
                i = ParseFence(lines, i, fenceChar, fenceLength, quote);
                continue;
            }
            const std::wstring_view trimmed = LeftTrim(line);
            if (StartsWith(trimmed, L"$$")) {
                i = ParseMath(lines, i, quote);
                continue;
            }
            int level = 0;
            std::wstring_view headingText;
            if (IsHeading(line, level, headingText)) {
                Block block;
                block.kind = BlockKind::Heading;
                block.level = level;
                block.quote = quote;
                AddInline(block, headingText);
                ++i;
                continue;
            }
            if (IsRule(line)) {
                Block block;
                block.kind = BlockKind::Rule;
                block.quote = quote;
                m_out.push_back(std::move(block));
                ++i;
                continue;
            }
            if (trimmed.front() == L'>') {
                LineList inner;
                while (i < lines.size() && !IsBlank(lines[i]) && !LeftTrim(lines[i]).empty() &&
                       LeftTrim(lines[i]).front() == L'>') {
                    std::wstring_view body = LeftTrim(lines[i]).substr(1);
                    if (!body.empty() && body.front() == L' ') {
                        body.remove_prefix(1);
                    }
                    inner.lines.push_back(body);
                    ++i;
                }
                if (quote < kMaxQuoteDepth) {
                    Parse(inner, quote + 1);
                }
                continue;
            }
            if (listItem) {
                i = ParseListItem(lines, i, marker, listIndents, quote);
                continue;
            }
            if (StartsTable(lines, i)) {
                i = ParseTable(lines, i, quote);
                continue;
            }
            i = ParseParagraph(lines, i, quote);
        }
    }

private:
    void AddInline(Block& block, std::wstring_view text) {
        std::vector<InlineImage> images;
        block.text = ParseInline(TrimView(text), &images);
        const int quote = block.quote;
        if (!TrimView(block.text.text).empty() || block.kind == BlockKind::ListItem) {
            m_out.push_back(std::move(block));
        }
        for (InlineImage& image : images) {
            Block imageBlock;
            imageBlock.kind = BlockKind::Image;
            imageBlock.quote = quote;
            imageBlock.text.text = std::move(image.alt);
            imageBlock.url = std::move(image.url);
            m_out.push_back(std::move(imageBlock));
        }
    }

    size_t ParseFence(const LineList& lines, size_t i, wchar_t fenceChar,
        size_t fenceLength, int quote) {
        const size_t indent = IndentOf(lines[i]);
        const std::wstring_view info = TrimView(LeftTrim(lines[i]).substr(fenceLength));
        Block block;
        block.kind = BlockKind::Code;
        block.quote = quote;
        const size_t space = info.find_first_of(L" \t{");
        block.language = Lower(info.substr(0, space));
        ++i;
        std::wstring code;
        bool first = true;
        while (i < lines.size()) {
            const std::wstring_view t = LeftTrim(lines[i]);
            size_t run = 0;
            while (run < t.size() && t[run] == fenceChar) {
                ++run;
            }
            if (run >= fenceLength && TrimView(t.substr(run)).empty()) {
                ++i;
                break;
            }
            std::wstring_view body = lines[i];
            size_t strip = 0;
            while (strip < indent && strip < body.size() && body[strip] == L' ') {
                ++strip;
            }
            body.remove_prefix(strip);
            if (!first) {
                code += L'\n';
            }
            code.append(body);
            first = false;
            ++i;
        }
        std::wstring expanded;
        expanded.reserve(code.size());
        size_t column = 0;
        for (const wchar_t c : code) {
            if (c == L'\t') {
                const size_t pad = 4 - column % 4;
                expanded.append(pad, L' ');
                column += pad;
            } else {
                expanded.push_back(c);
                column = c == L'\n' ? 0 : column + 1;
            }
        }
        code = std::move(expanded);
        while (!code.empty() && (code.back() == L'\n' || code.back() == L' ')) {
            code.pop_back();
        }
        block.code = std::move(code);
        m_out.push_back(std::move(block));
        return i;
    }

    size_t ParseMath(const LineList& lines, size_t i, int quote) {
        Block block;
        block.kind = BlockKind::Code;
        block.language = L"math";
        block.quote = quote;
        std::wstring_view first = TrimView(LeftTrim(lines[i]).substr(2));
        const size_t inlineClose = first.find(L"$$");
        if (inlineClose != std::wstring_view::npos) {
            block.code = std::wstring(TrimView(first.substr(0, inlineClose)));
            m_out.push_back(std::move(block));
            return i + 1;
        }
        std::wstring code(first);
        ++i;
        while (i < lines.size()) {
            const std::wstring_view t = lines[i];
            const size_t close = t.find(L"$$");
            if (close != std::wstring_view::npos) {
                const std::wstring_view tail = TrimView(t.substr(0, close));
                if (!tail.empty()) {
                    if (!code.empty()) {
                        code += L'\n';
                    }
                    code.append(tail);
                }
                ++i;
                break;
            }
            if (!code.empty()) {
                code += L'\n';
            }
            code.append(TrimView(t));
            ++i;
        }
        block.code = std::move(code);
        m_out.push_back(std::move(block));
        return i;
    }

    static bool InterruptsParagraph(const LineList& lines, size_t i) {
        const std::wstring_view line = lines[i];
        wchar_t fenceChar = 0;
        size_t fenceLength = 0;
        int level = 0;
        std::wstring_view text;
        ListMarker marker;
        const std::wstring_view t = LeftTrim(line);
        return IsBlank(line) || IsFence(line, fenceChar, fenceLength) || IsHeading(line, level, text) ||
            IsRule(line) || (!t.empty() && t.front() == L'>') || StartsWith(t, L"$$") ||
            (IsListItem(line, marker) && !marker.content.empty()) || StartsTable(lines, i);
    }

    size_t ParseListItem(const LineList& lines, size_t i, const ListMarker& marker,
        std::vector<size_t>& listIndents, int quote) {
        while (!listIndents.empty() && marker.indent + 1 < listIndents.back()) {
            listIndents.pop_back();
        }
        if (listIndents.empty() || marker.indent >= listIndents.back() + 2) {
            listIndents.push_back(marker.indent);
        }
        const int level = static_cast<int>(std::min<size_t>(listIndents.size() - 1, 5));

        std::wstring text(marker.content);
        ++i;
        while (i < lines.size()) {
            const std::wstring_view line = lines[i];
            if (IsBlank(line)) {
                // A blank line then an indented non-item line continues the item.
                size_t next = i + 1;
                while (next < lines.size() && IsBlank(lines[next])) {
                    ++next;
                }
                ListMarker nested;
                wchar_t fenceChar = 0;
                size_t fenceLength = 0;
                if (next < lines.size() && IndentOf(lines[next]) >= marker.contentColumn &&
                    !IsListItem(lines[next], nested) && !IsFence(lines[next], fenceChar, fenceLength)) {
                    text += L'\n';
                    text.append(TrimView(lines[next]));
                    i = next + 1;
                    continue;
                }
                break;
            }
            ListMarker nested;
            if (IsListItem(line, nested) || InterruptsParagraph(lines, i)) {
                break;
            }
            AppendLine(text, line);
            ++i;
        }

        Block block;
        block.kind = BlockKind::ListItem;
        block.level = level;
        block.quote = quote;
        std::wstring_view content = text;
        if (StartsWith(content, L"[ ] ") || content == L"[ ]") {
            block.marker = L"\u2610";
            content = TrimView(content.substr(3));
        } else if (StartsWithNoCase(content, L"[x] ") || StartsWithNoCase(content, L"[x]")) {
            block.marker = L"\u2611";
            content = TrimView(content.substr(3));
        } else if (marker.ordered) {
            block.marker = marker.number + L".";
        } else {
            static constexpr const wchar_t* kBullets[] = {L"\u2022", L"\u25E6", L"\u25AA"};
            block.marker = kBullets[level % 3];
        }
        AddInline(block, content);
        return i;
    }

    static void AppendLine(std::wstring& text, std::wstring_view line) {
        if (!text.empty()) {
            const bool hard = text.back() == L'\\' ||
                (text.size() >= 2 && text[text.size() - 1] == L' ' && text[text.size() - 2] == L' ');
            if (text.back() == L'\\') {
                text.pop_back();
            }
            while (!text.empty() && text.back() == L' ') {
                text.pop_back();
            }
            text += hard ? L'\n' : L' ';
        }
        std::wstring_view body = LeftTrim(line);
        // Keep a trailing double space (hard break marker) for the next join.
        size_t end = body.size();
        while (end > 0 && (body[end - 1] == L'\t' || body[end - 1] == L'\r')) {
            --end;
        }
        text.append(body.substr(0, end));
    }

    size_t ParseTable(const LineList& lines, size_t i, int quote) {
        Block block;
        block.kind = BlockKind::Table;
        block.quote = quote;
        const std::vector<std::wstring_view> header = SplitRow(lines[i]);
        const std::vector<std::wstring_view> delimiter = SplitRow(lines[i + 1]);
        const size_t columns = std::max<size_t>(1, header.size());
        for (size_t c = 0; c < columns; ++c) {
            Align align = Align::Left;
            if (c < delimiter.size()) {
                const std::wstring_view d = delimiter[c];
                const bool left = !d.empty() && d.front() == L':';
                const bool right = !d.empty() && d.back() == L':';
                align = left && right ? Align::Center : right ? Align::Right : Align::Left;
            }
            block.align.push_back(align);
        }
        auto addRow = [&](const std::vector<std::wstring_view>& cells) {
            std::vector<InlineText> row;
            for (size_t c = 0; c < columns; ++c) {
                row.push_back(c < cells.size() ? ParseInline(cells[c]) : InlineText{});
            }
            block.rows.push_back(std::move(row));
        };
        addRow(header);
        i += 2;
        while (i < lines.size() && !IsBlank(lines[i]) && lines[i].find(L'|') != std::wstring_view::npos) {
            addRow(SplitRow(lines[i]));
            ++i;
        }
        m_out.push_back(std::move(block));
        return i;
    }

    size_t ParseParagraph(const LineList& lines, size_t i, int quote) {
        std::wstring text;
        AppendLine(text, lines[i]);
        ++i;
        while (i < lines.size()) {
            const std::wstring_view t = TrimView(lines[i]);
            // Setext headings: "Title" underlined with === or ---.
            if (!t.empty() && (t.find_first_not_of(L'=') == std::wstring_view::npos ||
                                  (t.size() >= 2 && t.find_first_not_of(L'-') == std::wstring_view::npos))) {
                Block block;
                block.kind = BlockKind::Heading;
                block.level = t.front() == L'=' ? 1 : 2;
                block.quote = quote;
                AddInline(block, text);
                return i + 1;
            }
            if (InterruptsParagraph(lines, i)) {
                break;
            }
            AppendLine(text, lines[i]);
            ++i;
        }
        Block block;
        block.kind = BlockKind::Paragraph;
        block.quote = quote;
        AddInline(block, text);
        return i;
    }

    std::vector<Block>& m_out;
    std::vector<ParseCheckpoint>* m_checkpoints;
};

LineList SplitLines(std::wstring_view markdown) {
    LineList list;
    size_t start = 0;
    while (start <= markdown.size()) {
        size_t end = markdown.find(L'\n', start);
        if (end == std::wstring_view::npos) {
            end = markdown.size();
        }
        std::wstring_view line = markdown.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') {
            line.remove_suffix(1);
        }
        list.lines.push_back(line);
        if (end == markdown.size()) {
            break;
        }
        start = end + 1;
    }
    return list;
}

// ---- code highlighting -----------------------------------------------------------

struct LangInfo {
    bool slash = false;
    bool hash = false;
    bool dash = false;
    bool block = false;
    bool html = false;
    bool backtick = false;
    bool tripleQuotes = false;
    bool sql = false;
};

LangInfo LanguageInfo(std::wstring_view language) {
    static const std::unordered_set<std::wstring> cLike = {L"c", L"cpp", L"c++", L"cc", L"h", L"hpp", L"cs",
        L"csharp", L"java", L"js", L"javascript", L"jsx", L"ts", L"typescript", L"tsx", L"go", L"golang",
        L"rust", L"rs", L"swift", L"kotlin", L"kt", L"scala", L"dart", L"php", L"css", L"scss", L"less",
        L"jsonc", L"json5", L"groovy", L"objc", L"objective-c", L"zig", L"glsl", L"hlsl", L"solidity", L"proto",
        L"protobuf", L"cuda", L"f#", L"fsharp", L"verilog"};
    static const std::unordered_set<std::wstring> hashLangs = {L"python", L"py", L"python3", L"sh", L"bash",
        L"zsh", L"shell", L"console", L"ruby", L"rb", L"yaml", L"yml", L"toml", L"r", L"perl", L"pl", L"ps1",
        L"powershell", L"pwsh", L"dockerfile", L"docker", L"makefile", L"make", L"cmake", L"nix", L"elixir",
        L"ex", L"conf", L"properties", L"gitignore", L"julia", L"nim", L"crystal", L"tcl", L"awk", L"fish"};
    static const std::unordered_set<std::wstring> dashLangs = {L"sql", L"lua", L"haskell", L"hs", L"elm",
        L"ada", L"plsql", L"postgresql", L"mysql", L"sqlite", L"tsql"};
    static const std::unordered_set<std::wstring> htmlLangs = {L"html", L"xml", L"svg", L"vue", L"xaml",
        L"svelte", L"md", L"markdown", L"htm", L"xhtml", L"plist"};
    const std::wstring lang(language);
    LangInfo info;
    if (cLike.contains(lang)) {
        info.slash = true;
        info.block = true;
        info.backtick = lang == L"js" || lang == L"javascript" || lang == L"jsx" || lang == L"ts" ||
            lang == L"typescript" || lang == L"tsx" || lang == L"go" || lang == L"golang";
        info.hash = lang == L"php";
    } else if (hashLangs.contains(lang)) {
        info.hash = true;
        info.tripleQuotes = lang == L"python" || lang == L"py" || lang == L"python3" || lang == L"julia";
        info.backtick = lang == L"sh" || lang == L"bash" || lang == L"zsh" || lang == L"shell";
    } else if (dashLangs.contains(lang)) {
        info.dash = true;
        info.block = lang != L"lua" && lang != L"haskell" && lang != L"hs" && lang != L"elm";
        info.sql = lang != L"lua" && lang != L"haskell" && lang != L"hs" && lang != L"elm" && lang != L"ada";
    } else if (htmlLangs.contains(lang)) {
        info.html = true;
    } else if (lang == L"json") {
        // strings and numbers only
    } else if (lang == L"ini" || lang == L"reg" || lang == L"asm" || lang == L"nasm") {
        info.hash = true;
    } else if (lang == L"math" || lang == L"text" || lang == L"txt" || lang == L"plaintext" ||
               lang == L"output") {
        return LangInfo{};
    } else {
        info.slash = true;
        info.hash = true;
        info.block = true;
    }
    return info;
}

bool IsKeyword(std::wstring_view word, bool sql) {
    static const std::unordered_set<std::wstring> keywords = {L"if", L"else", L"elif", L"for", L"while", L"do",
        L"return", L"function", L"func", L"fn", L"def", L"class", L"struct", L"enum", L"interface", L"trait",
        L"impl", L"const", L"let", L"var", L"val", L"mut", L"static", L"public", L"private", L"protected",
        L"internal", L"import", L"from", L"export", L"package", L"use", L"using", L"namespace", L"module",
        L"new", L"delete", L"try", L"catch", L"except", L"finally", L"throw", L"throws", L"raise", L"async",
        L"await", L"yield", L"break", L"continue", L"switch", L"case", L"default", L"match", L"when", L"in",
        L"is", L"as", L"with", L"lambda", L"pass", L"not", L"and", L"or", L"true", L"false", L"null",
        L"nullptr", L"nil", L"None", L"True", L"False", L"undefined", L"this", L"self", L"super", L"void",
        L"int", L"long", L"short", L"char", L"bool", L"boolean", L"float", L"double", L"string", L"auto",
        L"unsigned", L"signed", L"typedef", L"typename", L"template", L"virtual", L"override", L"final",
        L"extends", L"implements", L"abstract", L"type", L"go", L"defer", L"select", L"chan", L"map",
        L"where", L"pub", L"crate", L"mod", L"loop", L"unsafe", L"ref", L"out", L"readonly", L"sealed",
        L"then", L"fi", L"done", L"esac", L"echo", L"local", L"export", L"end", L"begin", L"until", L"unless",
        L"elsif", L"require", L"include", L"constexpr", L"noexcept", L"operator", L"friend", L"inline",
        L"extern", L"volatile", L"register", L"goto", L"sizeof", L"typeof", L"instanceof", L"of", L"get",
        L"set", L"global", L"nonlocal", L"assert", L"del", L"print", L"println", L"Self", L"dyn", L"move",
        L"where", L"union", L"object", L"data", L"fun", L"suspend", L"param", L"process"};
    static const std::unordered_set<std::wstring> sqlKeywords = {L"select", L"from", L"where", L"insert",
        L"update", L"delete", L"join", L"left", L"right", L"inner", L"outer", L"full", L"cross", L"on",
        L"group", L"by", L"order", L"having", L"limit", L"offset", L"create", L"table", L"view", L"values",
        L"into", L"set", L"and", L"or", L"not", L"null", L"as", L"distinct", L"union", L"all", L"case",
        L"when", L"then", L"else", L"end", L"alter", L"drop", L"index", L"primary", L"key", L"foreign",
        L"references", L"default", L"is", L"in", L"like", L"between", L"exists", L"with", L"asc", L"desc",
        L"count", L"sum", L"avg", L"min", L"max", L"begin", L"commit", L"rollback", L"returning", L"unique",
        L"varchar", L"int", L"integer", L"text", L"boolean", L"timestamp", L"if", L"over", L"partition"};
    if (sql) {
        return sqlKeywords.contains(Lower(word));
    }
    return keywords.contains(std::wstring(word));
}

bool IsIdentStart(wchar_t c) noexcept {
    return c == L'_' || (c < 128 && std::iswalpha(static_cast<wint_t>(c)) != 0);
}

bool IsIdentChar(wchar_t c) noexcept {
    return c == L'_' || (c < 128 && std::iswalnum(static_cast<wint_t>(c)) != 0);
}

}  // namespace

InlineText ParseInline(std::wstring_view text, std::vector<InlineImage>* images) {
    InlineText out;
    InlineParser parser(out, images);
    parser.Run(text, 0, std::wstring(), 0);
    return out;
}

std::vector<Block> Parse(std::wstring_view markdown) {
    const LineList lines = SplitLines(markdown);
    std::vector<Block> blocks;
    BlockParser(blocks).Parse(lines, 0);
    return blocks;
}

IncrementalParser::IncrementalParser() = default;
IncrementalParser::~IncrementalParser() = default;

const std::vector<Block>& IncrementalParser::Update(std::wstring_view markdown) {
    size_t resume = 0;
    const bool appended = !m_text.empty() && markdown.size() >= m_text.size() && markdown.starts_with(m_text);
    // Line starts: only the old last line onwards can have changed.
    if (!appended || m_lineStarts.empty()) {
        m_lineStarts.assign(1, 0);
    } else {
        m_lineStarts.pop_back();
        if (m_lineStarts.empty()) {
            m_lineStarts.push_back(0);
        }
    }
    for (size_t at = markdown.find(L'\n', m_lineStarts.back()); at != std::wstring_view::npos;
         at = markdown.find(L'\n', at + 1)) {
        m_lineStarts.push_back(at + 1);
    }
    LineList lines;
    lines.track = true;
    lines.lines.reserve(m_lineStarts.size());
    for (size_t i = 0; i < m_lineStarts.size(); ++i) {
        const size_t end = i + 1 < m_lineStarts.size() ? m_lineStarts[i + 1] - 1 : markdown.size();
        std::wstring_view line = markdown.substr(m_lineStarts[i], end - m_lineStarts[i]);
        if (!line.empty() && line.back() == L'\r') {
            line.remove_suffix(1);
        }
        lines.lines.push_back(line);
    }
    if (appended) {
        // Steps that never read the old last line (which may have grown) are final.
        while (resume < m_checkpoints.size() && m_checkpoints[resume].seen < m_lineCount) {
            ++resume;
        }
        resume = resume == 0 ? 0 : resume - 1;
    }
    if (!appended || resume == 0) {
        m_blocks.clear();
        m_checkpoints.clear();
        BlockParser(m_blocks, &m_checkpoints).Parse(lines, 0);
    } else {
        ParseCheckpoint from = std::move(m_checkpoints[resume]);
        m_checkpoints.resize(resume);
        m_blocks.resize(from.blocks);
        lines.seen = from.seen;
        BlockParser(m_blocks, &m_checkpoints).Parse(lines, 0, from.line, std::move(from.listIndents));
    }
    m_text.assign(markdown);
    m_lineCount = lines.size();
    return m_blocks;
}

bool IsSafeUrl(std::wstring_view url) {
    if (url.empty() || url.size() > 4096) {
        return false;
    }
    for (const wchar_t c : url) {
        if (c < 0x21 || c == 0x7F) {
            return false;
        }
    }
    if (StartsWithNoCase(url, L"mailto:")) {
        return url.size() > 7;
    }
    const bool https = StartsWithNoCase(url, L"https://");
    const bool http = StartsWithNoCase(url, L"http://");
    return (https && url.size() > 8) || (http && url.size() > 7);
}

std::vector<Token> HighlightCode(std::wstring_view code, std::wstring_view language) {
    std::vector<Token> tokens;
    const LangInfo info = LanguageInfo(language);
    const bool plain = !info.slash && !info.hash && !info.dash && !info.block && !info.html;
    if (plain && language != L"json") {
        return tokens;
    }
    const size_t n = code.size();
    auto lineEnd = [&](size_t from) {
        const size_t end = code.find(L'\n', from);
        return end == std::wstring_view::npos ? n : end;
    };
    auto push = [&](size_t start, size_t end, TokenClass cls) {
        if (end > start) {
            tokens.push_back({start, end - start, cls});
        }
    };
    size_t i = 0;
    while (i < n) {
        const wchar_t c = code[i];
        const std::wstring_view rest = code.substr(i);
        if (info.block && StartsWith(rest, L"/*")) {
            const size_t close = code.find(L"*/", i + 2);
            const size_t end = close == std::wstring_view::npos ? n : close + 2;
            push(i, end, TokenClass::Comment);
            i = end;
            continue;
        }
        if (info.html && StartsWith(rest, L"<!--")) {
            const size_t close = code.find(L"-->", i + 4);
            const size_t end = close == std::wstring_view::npos ? n : close + 3;
            push(i, end, TokenClass::Comment);
            i = end;
            continue;
        }
        if ((info.slash && StartsWith(rest, L"//")) || (info.dash && StartsWith(rest, L"--")) ||
            (info.hash && c == L'#' && (i == 0 || IsSpace(code[i - 1])))) {
            const size_t end = lineEnd(i);
            push(i, end, TokenClass::Comment);
            i = end;
            continue;
        }
        if (c == L'"' || c == L'\'' || (c == L'`' && info.backtick)) {
            if (info.tripleQuotes && (StartsWith(rest, L"\"\"\"") || StartsWith(rest, L"'''"))) {
                const size_t close = code.find(rest.substr(0, 3), i + 3);
                const size_t end = close == std::wstring_view::npos ? n : close + 3;
                push(i, end, TokenClass::String);
                i = end;
                continue;
            }
            size_t j = i + 1;
            bool closed = false;
            while (j < n) {
                if (code[j] == L'\\') {
                    j += 2;
                    continue;
                }
                if (code[j] == c) {
                    closed = true;
                    break;
                }
                if (code[j] == L'\n' && c != L'`') {
                    break;
                }
                ++j;
            }
            if (closed) {
                push(i, j + 1, TokenClass::String);
                i = j + 1;
            } else {
                ++i;
            }
            continue;
        }
        if (c >= L'0' && c <= L'9' && (i == 0 || !IsIdentChar(code[i - 1]))) {
            size_t j = i + 1;
            while (j < n && (IsIdentChar(code[j]) || code[j] == L'.')) {
                ++j;
            }
            push(i, j, TokenClass::Number);
            i = j;
            continue;
        }
        if (IsIdentStart(c)) {
            size_t j = i + 1;
            while (j < n && IsIdentChar(code[j])) {
                ++j;
            }
            if (!plain && IsKeyword(code.substr(i, j - i), info.sql)) {
                push(i, j, TokenClass::Keyword);
            }
            i = j;
            continue;
        }
        ++i;
    }
    return tokens;
}

}  // namespace aimd
