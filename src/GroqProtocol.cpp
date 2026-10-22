#include "GroqProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace groq {
namespace json {
namespace {

constexpr int kMaxDepth = 64;

class Reader {
public:
    explicit Reader(std::string_view text) : m_text(text) {}

    bool ParseDocument(Value& out) {
        if (!ParseValue(out, 0)) {
            return false;
        }
        SkipSpace();
        return m_pos == m_text.size();
    }

private:
    void SkipSpace() {
        while (m_pos < m_text.size() &&
               (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r')) {
            ++m_pos;
        }
    }

    bool Literal(std::string_view word) {
        if (m_text.substr(m_pos, word.size()) != word) {
            return false;
        }
        m_pos += word.size();
        return true;
    }

    bool ParseValue(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return false;
        }
        SkipSpace();
        if (m_pos >= m_text.size()) {
            return false;
        }
        const char c = m_text[m_pos];
        if (c == '{') {
            out.type = Value::Type::Object;
            ++m_pos;
            SkipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            while (true) {
                SkipSpace();
                std::string key;
                if (m_pos >= m_text.size() || m_text[m_pos] != '"' || !ParseString(key)) {
                    return false;
                }
                SkipSpace();
                if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                    return false;
                }
                ++m_pos;
                Value member;
                if (!ParseValue(member, depth + 1)) {
                    return false;
                }
                out.members.emplace_back(std::move(key), std::move(member));
                SkipSpace();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                    ++m_pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            out.type = Value::Type::Array;
            ++m_pos;
            SkipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            while (true) {
                Value item;
                if (!ParseValue(item, depth + 1)) {
                    return false;
                }
                out.items.push_back(std::move(item));
                SkipSpace();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                    ++m_pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out.type = Value::Type::String;
            return ParseString(out.string);
        }
        if (Literal("true")) {
            out.type = Value::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (Literal("false")) {
            out.type = Value::Type::Bool;
            return true;
        }
        if (Literal("null")) {
            out.type = Value::Type::Null;
            return true;
        }
        return ParseNumber(out);
    }

    bool ParseNumber(Value& out) {
        const size_t start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            ++m_pos;
        }
        bool digits = false;
        while (m_pos < m_text.size() &&
               ((m_text[m_pos] >= '0' && m_text[m_pos] <= '9') || m_text[m_pos] == '.' || m_text[m_pos] == 'e' ||
                   m_text[m_pos] == 'E' || m_text[m_pos] == '+' || m_text[m_pos] == '-')) {
            digits = true;
            ++m_pos;
        }
        if (!digits) {
            return false;
        }
        const std::string number(m_text.substr(start, m_pos - start));
        char* end = nullptr;
        out.type = Value::Type::Number;
        out.number = std::strtod(number.c_str(), &end);
        return end != nullptr && *end == '\0';
    }

    static void AppendCodePoint(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool Hex4(uint32_t& value) {
        if (m_pos + 4 > m_text.size()) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = m_text[m_pos++];
            value <<= 4U;
            if (h >= '0' && h <= '9') {
                value |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                value |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                value |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool ParseString(std::string& out) {
        ++m_pos;  // opening quote
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_pos >= m_text.size()) {
                return false;
            }
            const char e = m_text[m_pos++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!Hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 6 <= m_text.size() && m_text[m_pos] == '\\' &&
                    m_text[m_pos + 1] == 'u') {
                    m_pos += 2;
                    uint32_t low = 0;
                    if (!Hex4(low)) {
                        return false;
                    }
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10U) + (low - 0xDC00);
                    } else {
                        AppendCodePoint(out, 0xFFFD);
                        cp = low;
                    }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                AppendCodePoint(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    std::string_view m_text;
    size_t m_pos = 0;
};

}  // namespace

const Value* Value::Get(std::string_view key) const noexcept {
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto& [name, value] : members) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

std::string_view Value::Str(std::string_view key) const noexcept {
    const Value* value = Get(key);
    return value != nullptr && value->type == Type::String ? std::string_view(value->string) : std::string_view();
}

std::optional<Value> Parse(std::string_view text) {
    Value value;
    if (!Reader(text).ParseDocument(value)) {
        return std::nullopt;
    }
    return value;
}

void AppendQuoted(std::string& out, std::string_view utf8) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    size_t i = 0;
    while (i < utf8.size()) {
        // Copy the run of bytes that need no escaping in one append (image
        // data URLs are megabytes of plain base64).
        size_t run = i;
        while (run < utf8.size()) {
            const auto c = static_cast<unsigned char>(utf8[run]);
            if (c < 0x20 || c == '"' || c == '\\') {
                break;
            }
            ++run;
        }
        out.append(utf8.data() + i, run - i);
        if (run >= utf8.size()) {
            break;
        }
        const auto c = static_cast<unsigned char>(utf8[run]);
        i = run + 1;
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            out += "\\u00";
            out.push_back(kHex[c >> 4U]);
            out.push_back(kHex[c & 15U]);
        }
    }
    out.push_back('"');
}

}  // namespace json

// ---- text ---------------------------------------------------------------------------

std::string WideToUtf8(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(text[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
                const auto low = static_cast<uint32_t>(text[i + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10U) + (low - 0xDC00);
                    ++i;
                }
            }
        }
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::wstring Utf8ToWide(std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0xFFFD;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1FU;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0FU;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07U;
            extra = 3;
        }
        size_t consumed = 1;
        if (extra > 0) {
            bool valid = i + extra < text.size();
            for (size_t k = 1; valid && k <= extra; ++k) {
                const auto next = static_cast<unsigned char>(text[i + k]);
                if ((next & 0xC0) != 0x80) {
                    valid = false;
                } else {
                    cp = (cp << 6U) | (next & 0x3FU);
                }
            }
            const bool overlong = (extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000);
            if (!valid || overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                cp = 0xFFFD;
            } else {
                consumed = extra + 1;
            }
        } else if (c >= 0x80) {
            cp = 0xFFFD;
        }
        i += consumed;
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10U)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FFU)));
                continue;
            }
        }
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
}

std::string TruncateUtf8(std::string_view text, size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return std::string(text);
    }
    size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return std::string(text.substr(0, end)) + "\xE2\x80\xA6";
}

namespace {

std::string StripCitationMarks(std::string_view text) {
    // 【...】 (U+3010 / U+3011) markers that browser_search leaves in prose.
    static constexpr std::string_view kOpen = "\xE3\x80\x90";
    static constexpr std::string_view kClose = "\xE3\x80\x91";
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const size_t open = text.find(kOpen, i);
        if (open == std::string_view::npos) {
            out.append(text.substr(i));
            break;
        }
        const size_t close = text.find(kClose, open + kOpen.size());
        out.append(text.substr(i, open - i));
        if (close == std::string_view::npos || close - open > 64) {
            out.append(kOpen);
            i = open + kOpen.size();
            continue;
        }
        i = close + kClose.size();
    }
    return out;
}

bool IsHttpUrl(std::string_view url) {
    return url.starts_with("https://") || url.starts_with("http://");
}

// web_search results are re-sent as input on every later round, so keep them
// to the summary plus the top few results (the full list still shows in Sources).
constexpr size_t kToolSummaryBytes = 2400;
constexpr size_t kToolResults = 5;
constexpr size_t kToolSnippetBytes = 280;
constexpr size_t kToolImages = 2;

void AppendTools(std::string& out, SearchMode search) {
    if (search == SearchMode::Function) {
        out += R"(,"tools":[{"type":"function","function":{"name":"web_search","description":)"
               R"("Search the web. Returns a summary, numbered results and image URLs.","parameters":)"
               R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}}}])"
               R"(,"tool_choice":"auto")";
    } else if (search == SearchMode::Builtin) {
        out += R"(,"tools":[{"type":"browser_search"}])";
    }
}

}  // namespace

// ---- requests ---------------------------------------------------------------------------

bool HasBuiltinSearch(std::string_view model) noexcept {
    return model.starts_with("openai/gpt-oss");
}

bool SupportsReasoningEffort(std::string_view model) noexcept {
    return HasBuiltinSearch(model) || model.starts_with("qwen/qwen3.8");
}

std::string SystemPrompt(std::string_view today, SearchMode search) {
    // Stable text first and the date last, so the prompt prefix stays cacheable.
    std::string prompt =
        "You are Hoverdock's assistant, answering a question typed into a Windows dock's search bar. Lead with the "
        "answer and keep it brief; expand only when asked or when the task needs it.\n"
        "Use GitHub Markdown: short paragraphs, **bold** key terms, lists for steps, tables for comparisons, fenced "
        "code with a language tag, [links](url). No LaTeX or HTML; write maths in plain Unicode (x\xC2\xB2, "
        "\xE2\x88\x9A" "2).";
    if (search == SearchMode::Function) {
        prompt +=
            "\nCall web_search only for current events, live data or facts you are unsure of; otherwise answer "
            "from knowledge. Cite results inline as [n](url). You may embed one returned image as ![alt](url).";
    } else if (search == SearchMode::Builtin) {
        prompt +=
            "\nUse browser_search only for current or uncertain facts. Cite pages inline as [n](url).";
    }
    prompt += "\nToday is ";
    prompt += today;
    prompt += '.';
    return prompt;
}

std::string BuildChatRequest(std::string_view model, const std::vector<Message>& messages, SearchMode search,
    bool stream, uint32_t maxTokens) {
    std::string out = R"({"model":)";
    json::AppendQuoted(out, model);
    out += R"(,"messages":[)";
    bool first = true;
    for (const Message& message : messages) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += R"({"role":)";
        json::AppendQuoted(out, message.role);
        out += R"(,"content":)";
        if (!message.images.empty()) {
            out += R"([{"type":"text","text":)";
            json::AppendQuoted(out, message.content);
            out.push_back('}');
            for (size_t i = 0; i < message.images.size() && i < kMaxImages; ++i) {
                out += R"(,{"type":"image_url","image_url":{"url":)";
                json::AppendQuoted(out, message.images[i]);
                out += "}}";
            }
            out.push_back(']');
        } else if (message.role == "assistant") {
            // History carries final outputs only, never reasoning (Groq multi-turn guidance).
            const std::string finalOutput = StripThinking(message.content);
            if (finalOutput.empty() && !message.toolCalls.empty()) {
                out += "null";
            } else {
                json::AppendQuoted(out, finalOutput);
            }
        } else {
            json::AppendQuoted(out, message.content);
        }
        if (!message.toolCallId.empty()) {
            out += R"(,"tool_call_id":)";
            json::AppendQuoted(out, message.toolCallId);
        }
        if (!message.toolCalls.empty()) {
            out += R"(,"tool_calls":[)";
            for (size_t i = 0; i < message.toolCalls.size(); ++i) {
                const ToolCall& call = message.toolCalls[i];
                if (i > 0) {
                    out.push_back(',');
                }
                out += R"({"id":)";
                json::AppendQuoted(out, call.id);
                out += R"(,"type":"function","function":{"name":)";
                json::AppendQuoted(out, call.name);
                out += R"(,"arguments":)";
                json::AppendQuoted(out, call.arguments.empty() ? std::string_view("{}") : call.arguments);
                out += "}}";
            }
            out.push_back(']');
        }
        out.push_back('}');
    }
    out.push_back(']');
    AppendTools(out, search);
    // Reasoning is billed at the output rate and medium effort spent several
    // times the answer's tokens on short dock questions, so think briefly.
    // Qwen returns it in a separate `reasoning` field (parsed) so `content` is
    // the final answer only; GPT-OSS always does.
    if (SupportsReasoningEffort(model)) {
        out += R"(,"reasoning_effort":"low")";
        if (!HasBuiltinSearch(model)) {
            out += R"(,"reasoning_format":"parsed")";
        }
    }
    out += R"(,"temperature":0.8)";
    // Groq's Qwen 3.8 thinking-mode nucleus setting.
    if (model.find("qwen") != std::string_view::npos) {
        out += R"(,"top_p":0.95)";
    }
    // Room for brief reasoning plus the answer; caps runaway generations.
    out += R"(,"max_completion_tokens":)";
    out += std::to_string(maxTokens);
    out += stream ? R"(,"stream":true})" : R"(,"stream":false})";
    return out;
}

std::string BuildSearchRequest(std::string_view query, std::string_view model, uint32_t maxTokens) {
    std::string out = R"({"model":)";
    json::AppendQuoted(out, model);
    out += R"(,"messages":[{"role":"user","content":)";
    // Every page browser_search opens is loaded into the model's context and
    // billed against the (small) per-minute token limit, so answer from the
    // result snippets and open pages only as a last resort.
    std::string prompt = "Search the web once for: ";
    prompt += query;
    prompt +=
        "\n\nAnswer from the search result snippets. Open at most one page, and only if the snippets lack the "
        "answer. Reply with at most 8 terse bullets of the key facts (names, figures, dates), each noting its page.";
    json::AppendQuoted(out, prompt);
    out += R"(}],"tools":[{"type":"browser_search"}],"tool_choice":"required","reasoning_effort":"low",)"
           R"("max_completion_tokens":)";
    out += std::to_string(maxTokens);
    out += R"(,"stream":false})";
    return out;
}

std::string SearchCacheKey(std::string_view query) {
    std::string key;
    key.reserve(query.size());
    bool space = false;
    for (const char c : query) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = !key.empty();
            continue;
        }
        if (space) {
            key.push_back(' ');
            space = false;
        }
        key.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return key;
}

namespace {

// "2", "7.66s", "1m2.5s", "450ms" -> milliseconds; nullopt when unparsable.
std::optional<uint64_t> ParseDuration(std::string_view text) {
    double total = 0.0;
    bool any = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t start = i;
        while (i < text.size() && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) {
            ++i;
        }
        if (i == start) {
            break;
        }
        const double value = std::strtod(std::string(text.substr(start, i - start)).c_str(), nullptr);
        if (text.substr(i).starts_with("ms")) {
            total += value;
            i += 2;
        } else if (text.substr(i).starts_with("h")) {
            total += value * 3600000.0;
            ++i;
        } else if (text.substr(i).starts_with("m")) {
            total += value * 60000.0;
            ++i;
        } else {
            total += value * 1000.0;
            if (i < text.size() && text[i] == 's') {
                ++i;
            }
        }
        any = true;
    }
    if (!any || total < 0.0 || total > 86400000.0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(total + 0.5);
}

}  // namespace

RateLimit ParseRateLimit(std::string_view retryAfter, std::string_view errorBody) {
    RateLimit limit;
    const std::string message = ErrorMessage(errorBody);
    limit.daily = message.find("per day") != std::string::npos || message.find("(TPD)") != std::string::npos ||
        message.find("(RPD)") != std::string::npos;
    // Groq: "Request too large for model `m` ... on output tokens per minute (OTPM): Limit 1000, Requested 1282."
    if (const size_t otpm = message.find("(OTPM)"); otpm != std::string::npos &&
        message.find("Request too large") != std::string::npos) {
        constexpr std::string_view kLimit = "Limit ";
        if (const size_t at = message.find(kLimit, otpm); at != std::string::npos) {
            uint64_t value = 0;
            size_t i = at + kLimit.size();
            while (i < message.size() && message[i] >= '0' && message[i] <= '9' && value < 100000000) {
                value = value * 10 + static_cast<uint64_t>(message[i] - '0');
                ++i;
            }
            limit.outputTokenLimit = static_cast<uint32_t>(value);
        }
    }
    while (!retryAfter.empty() && retryAfter.front() == ' ') {
        retryAfter.remove_prefix(1);
    }
    if (const auto header = ParseDuration(retryAfter)) {
        limit.retryAfterMs = *header;
        return limit;
    }
    // Groq's message: "... Please try again in 7.66s. Need more tokens? ..."
    constexpr std::string_view kMarker = "try again in ";
    if (const size_t at = message.find(kMarker); at != std::string::npos) {
        std::string_view rest = std::string_view(message).substr(at + kMarker.size());
        rest = rest.substr(0, rest.find_first_of(" ,"));
        if (!rest.empty() && rest.back() == '.') {
            rest.remove_suffix(1);
        }
        if (const auto parsed = ParseDuration(rest)) {
            limit.retryAfterMs = *parsed;
        }
    }
    return limit;
}

std::string Base64(std::string_view bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out((bytes.size() + 2) / 3 * 4, '\0');
    char* o = out.data();
    const auto* in = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3, o += 4) {
        const uint32_t v = (static_cast<uint32_t>(in[i]) << 16U) | (static_cast<uint32_t>(in[i + 1]) << 8U) |
            static_cast<uint32_t>(in[i + 2]);
        o[0] = kAlphabet[(v >> 18U) & 63U];
        o[1] = kAlphabet[(v >> 12U) & 63U];
        o[2] = kAlphabet[(v >> 6U) & 63U];
        o[3] = kAlphabet[v & 63U];
    }
    const size_t rest = bytes.size() - i;
    if (rest > 0) {
        uint32_t v = static_cast<uint32_t>(in[i]) << 16U;
        if (rest == 2) {
            v |= static_cast<uint32_t>(in[i + 1]) << 8U;
        }
        o[0] = kAlphabet[(v >> 18U) & 63U];
        o[1] = kAlphabet[(v >> 12U) & 63U];
        o[2] = rest == 2 ? kAlphabet[(v >> 6U) & 63U] : '=';
        o[3] = '=';
    }
    return out;
}

std::string DataUrl(std::string_view mime, std::string_view bytes) {
    std::string out;
    out.reserve(5 + mime.size() + 8 + (bytes.size() + 2) / 3 * 4);
    out += "data:";
    out += mime;
    out += ";base64,";
    out += Base64(bytes);
    return out;
}

std::string DefaultImagePrompt(size_t imageCount) {
    return imageCount > 1 ? "Describe these images and point out anything notable."
                          : "Describe this image and point out anything notable.";
}

std::string SearchQueryFromArguments(std::string_view arguments) {
    const std::optional<json::Value> parsed = json::Parse(arguments);
    if (parsed.has_value() && parsed->IsObject()) {
        return std::string(parsed->Str("query"));
    }
    return {};
}

void AddSource(std::vector<Source>& sources, Source source) {
    if (!IsHttpUrl(source.url)) {
        return;
    }
    for (const Source& existing : sources) {
        if (existing.url == source.url) {
            return;
        }
    }
    if (source.title.empty()) {
        source.title = source.url;
    }
    sources.push_back(std::move(source));
}

bool CollectExecutedTools(const json::Value& executedTools, std::vector<Source>& sources,
    std::vector<std::string>* images) {
    if (!executedTools.IsArray()) {
        return false;
    }
    bool searched = false;
    for (const json::Value& tool : executedTools.items) {
        const std::string_view type = tool.Str("type");
        if (type.find("search") != std::string_view::npos || type.find("browser") != std::string_view::npos) {
            searched = true;
        }
        auto addResults = [&](const json::Value* list) {
            if (list == nullptr || !list->IsArray()) {
                return;
            }
            for (const json::Value& result : list->items) {
                Source source;
                source.title = std::string(result.Str("title"));
                source.url = std::string(result.Str("url"));
                source.snippet = TruncateUtf8(result.Str("content"), 700);
                searched = true;
                AddSource(sources, std::move(source));
            }
        };
        if (const json::Value* search = tool.Get("search_results"); search != nullptr && search->IsObject()) {
            addResults(search->Get("results"));
            const json::Value* imageList = search->Get("images");
            if (images != nullptr && imageList != nullptr && imageList->IsArray()) {
                for (const json::Value& image : imageList->items) {
                    if (image.IsString() && IsHttpUrl(image.string) && images->size() < 6) {
                        images->push_back(image.string);
                    }
                }
            }
        }
        addResults(tool.Get("browser_results"));
    }
    return searched;
}

bool ParseSearchResponse(std::string_view body, SearchOutcome& outcome) {
    const std::optional<json::Value> root = json::Parse(body);
    if (!root.has_value()) {
        return false;
    }
    const json::Value* choices = root->Get("choices");
    if (choices == nullptr || !choices->IsArray() || choices->items.empty()) {
        return false;
    }
    const json::Value* message = choices->items.front().Get("message");
    if (message == nullptr) {
        return false;
    }
    outcome.summary = StripCitationMarks(StripThinking(message->Str("content")));
    if (const json::Value* tools = message->Get("executed_tools")) {
        CollectExecutedTools(*tools, outcome.sources, &outcome.images);
    }
    return true;
}

std::string BuildToolResult(std::string_view query, const SearchOutcome& outcome,
    const std::vector<size_t>& numbers) {
    std::string out = R"({"query":)";
    json::AppendQuoted(out, query);
    out += R"(,"summary":)";
    json::AppendQuoted(out, TruncateUtf8(outcome.summary, kToolSummaryBytes));
    out += R"(,"results":[)";
    for (size_t i = 0; i < outcome.sources.size() && i < kToolResults; ++i) {
        const Source& source = outcome.sources[i];
        if (i > 0) {
            out.push_back(',');
        }
        out += R"({"n":)";
        out += std::to_string(i < numbers.size() ? numbers[i] : i + 1);
        out += R"(,"title":)";
        json::AppendQuoted(out, source.title);
        out += R"(,"url":)";
        json::AppendQuoted(out, source.url);
        out += R"(,"snippet":)";
        json::AppendQuoted(out, TruncateUtf8(source.snippet, kToolSnippetBytes));
        out.push_back('}');
    }
    out += R"(],"images":[)";
    for (size_t i = 0; i < outcome.images.size() && i < kToolImages; ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        json::AppendQuoted(out, outcome.images[i]);
    }
    out += "]}";
    return out;
}

std::string ErrorMessage(std::string_view body) {
    const std::optional<json::Value> root = json::Parse(body);
    if (root.has_value()) {
        if (const json::Value* error = root->Get("error")) {
            if (error->IsString()) {
                return error->string;
            }
            const std::string_view message = error->Str("message");
            if (!message.empty()) {
                return TruncateUtf8(message, 400);
            }
        }
    }
    std::string snippet;
    for (const char c : body.substr(0, 300)) {
        snippet.push_back(c == '\n' || c == '\r' ? ' ' : c);
    }
    return snippet;
}

std::string StripThinking(std::string_view content) {
    std::string out;
    size_t i = 0;
    while (i < content.size()) {
        const size_t open = content.find("<think>", i);
        if (open == std::string_view::npos) {
            out.append(content.substr(i));
            break;
        }
        out.append(content.substr(i, open - i));
        const size_t close = content.find("</think>", open + 7);
        if (close == std::string_view::npos) {
            break;
        }
        i = close + 8;
    }
    const size_t first = out.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : out.substr(first);
}

// ---- streaming -----------------------------------------------------------------------------

const std::wstring& DisplayText::Update(const std::string& content) {
    if (content.size() < m_converted) {
        m_text.clear();
        m_converted = 0;
        m_thinking = false;
    }
    // A <think> block can hide or reveal earlier text: convert the whole thing.
    if (!m_thinking && content.find("<think", m_converted >= 6 ? m_converted - 6 : 0) != std::string::npos) {
        m_thinking = true;
    }
    if (m_thinking) {
        m_text = Utf8ToWide(StripThinking(content));
        m_converted = content.size();
        return m_text;
    }
    // Stop before an incomplete trailing UTF-8 sequence; it completes later.
    size_t end = content.size();
    size_t lead = end;
    while (lead > m_converted && (static_cast<unsigned char>(content[lead - 1]) & 0xC0) == 0x80) {
        --lead;
    }
    if (lead > m_converted) {
        const auto c = static_cast<unsigned char>(content[lead - 1]);
        const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (end - (lead - 1) < need) {
            end = lead - 1;
        }
    }
    std::string_view fresh = std::string_view(content).substr(m_converted, end - m_converted);
    if (m_text.empty()) {
        // StripThinking also drops leading whitespace.
        const size_t first = fresh.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            m_converted = end;
            return m_text;
        }
        m_converted += first;
        fresh.remove_prefix(first);
    }
    m_text += Utf8ToWide(fresh);
    m_converted = end;
    return m_text;
}

void StreamParser::Feed(std::string_view bytes) {
    size_t start = 0;
    while (start < bytes.size()) {
        const size_t newline = bytes.find('\n', start);
        const size_t end = newline == std::string_view::npos ? bytes.size() : newline;
        m_line.append(bytes.data() + start, end - start);
        if (newline == std::string_view::npos) {
            break;
        }
        // SSE lines end in \n or \r\n; drop any carriage returns.
        if (m_line.find('\r') != std::string::npos) {
            std::erase(m_line, '\r');
        }
        HandleLine(m_line);
        m_line.clear();
        start = newline + 1;
    }
}

void StreamParser::Finish() {
    if (!m_line.empty()) {
        HandleLine(m_line);
        m_line.clear();
    }
}

void StreamParser::HandleLine(std::string_view line) {
    if (!line.starts_with("data:")) {
        // A non-SSE error body (e.g. JSON on an error status) arrives as plain lines.
        if (line.starts_with("{") && m_error.empty()) {
            const std::string message = ErrorMessage(line);
            if (!message.empty() && line.find("\"error\"") != std::string_view::npos) {
                m_error = message;
            }
        }
        return;
    }
    std::string_view data = line.substr(5);
    while (!data.empty() && data.front() == ' ') {
        data.remove_prefix(1);
    }
    if (data == "[DONE]") {
        m_done = true;
        return;
    }
    HandleEvent(data);
}

void StreamParser::HandleEvent(std::string_view data) {
    const std::optional<json::Value> root = json::Parse(data);
    if (!root.has_value()) {
        return;
    }
    if (const json::Value* error = root->Get("error")) {
        m_error = error->IsString() ? error->string : std::string(error->Str("message"));
        if (m_error.empty()) {
            m_error = "The AI service reported an error.";
        }
        return;
    }
    const json::Value* choices = root->Get("choices");
    if (choices == nullptr || !choices->IsArray() || choices->items.empty()) {
        return;
    }
    const json::Value& choice = choices->items.front();
    const std::string_view finish = choice.Str("finish_reason");
    if (!finish.empty()) {
        m_finishReason = std::string(finish);
    }
    const json::Value* delta = choice.Get("delta");
    if (delta == nullptr) {
        delta = choice.Get("message");
    }
    if (delta == nullptr) {
        return;
    }
    const std::string_view content = delta->Str("content");
    m_content.append(content);
    if (!delta->Str("reasoning").empty()) {
        m_reasoning = true;
    }
    if (const json::Value* tools = delta->Get("executed_tools")) {
        m_searched = CollectExecutedTools(*tools, m_sources, nullptr) || m_searched;
    }
    const json::Value* calls = delta->Get("tool_calls");
    if (calls == nullptr || !calls->IsArray()) {
        return;
    }
    for (const json::Value& call : calls->items) {
        size_t index = m_toolCalls.size();
        if (const json::Value* position = call.Get("index");
            position != nullptr && position->type == json::Value::Type::Number && position->number >= 0.0 &&
            position->number < 16.0) {
            index = static_cast<size_t>(position->number);
        }
        if (index >= m_toolCalls.size()) {
            m_toolCalls.resize(index + 1);
        }
        ToolCall& target = m_toolCalls[index];
        const std::string_view id = call.Str("id");
        if (!id.empty()) {
            target.id = std::string(id);
        }
        if (const json::Value* function = call.Get("function")) {
            const std::string_view name = function->Str("name");
            if (target.name.empty()) {
                target.name = std::string(name);
            }
            target.arguments += function->Str("arguments");
        }
    }
}

}  // namespace groq
