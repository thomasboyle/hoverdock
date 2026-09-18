#include "TypeSafeClient.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kApiHost[] = L"api.typesafe.ai";
constexpr wchar_t kApiPath[] = L"/v1/systemone";
constexpr DWORD kConnectTimeoutMs = 8000;
constexpr DWORD kSendTimeoutMs = 15000;
constexpr DWORD kReceiveTimeoutMs = 30000;
constexpr double kExistsLaunch = 0.35;
// Capped to stay under server choice-option / payload limits that previously
// surfaced as generic HTTP 400. Local pre-ranking keeps the most relevant
// candidates when the catalog is larger than this.
constexpr size_t kMaxAppsPerChoice = 100;
constexpr size_t kGroupSize = 80;
constexpr size_t kMaxRequestChars = 200;
constexpr size_t kMaxCriteriaChars = 220;
constexpr size_t kMaxStateFieldChars = 120;
constexpr size_t kMaxErrorSnippetChars = 300;

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }

    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }

    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring TrimKey(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](wchar_t character) { return std::iswspace(character) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](wchar_t character) { return std::iswspace(character) != 0; })
                          .base();
    return first >= last ? L"" : std::wstring(first, last);
}

std::wstring ToLowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return value;
}

// Defensive: copy-pasted keys often arrive with quotes, whitespace, or a
// duplicated "Bearer " prefix, which the server reports as generic 400
// instead of 401. Normalizing here keeps header construction valid.
std::wstring SanitizeApiKey(const std::wstring& raw) {
    std::wstring key = TrimKey(raw);
    if (key.size() >= 2 &&
        ((key.front() == L'"' && key.back() == L'"') ||
            (key.front() == L'\'' && key.back() == L'\''))) {
        key = TrimKey(key.substr(1, key.size() - 2));
    }
    constexpr std::wstring_view kBearerPrefix = L"Bearer ";
    if (key.size() > kBearerPrefix.size() &&
        ToLowerWide(key.substr(0, kBearerPrefix.size())) == kBearerPrefix) {
        key = TrimKey(key.substr(kBearerPrefix.size()));
    }
    // Embedded whitespace/newlines can never be part of a bearer token.
    key.erase(std::remove_if(key.begin(), key.end(),
                  [](wchar_t character) { return std::iswspace(character) != 0; }),
        key.end());
    return key;
}

std::wstring TruncateWide(const std::wstring& value, size_t maxChars) {
    if (value.size() <= maxChars) {
        return value;
    }
    if (maxChars <= 1) {
        return L"…";
    }
    return value.substr(0, maxChars - 1) + L"…";
}

std::wstring NormalizeRequest(const std::wstring& raw) {
    return TruncateWide(TrimKey(raw), kMaxRequestChars);
}

// Server error bodies (e.g. {"error":...,"field":...}) name the offending
// field. Previously PostRawSystemOne discarded them and only showed the
// status code, making 400s undebuggable. Keep a short single-line snippet.
std::wstring ErrorSnippetFromBody(const std::string& body) {
    if (body.empty()) {
        return {};
    }
    std::string clipped = body.substr(0, 1024);
    for (char& character : clipped) {
        if (character == '\r' || character == '\n' || character == '\t') {
            character = ' ';
        } else if (static_cast<unsigned char>(character) < 0x20) {
            character = '?';
        }
    }
    std::wstring wide = Utf8ToWide(clipped);
    if (wide.empty()) {
        return {};
    }
    wide = TrimKey(wide);
    return TruncateWide(wide, kMaxErrorSnippetChars);
}

std::wstring StatusErrorWithBody(
    const wchar_t* prefix, DWORD status, const std::string& response) {
    std::wstring message = std::wstring(prefix) + L" (" + std::to_wstring(status) + L")";
    const std::wstring snippet = ErrorSnippetFromBody(response);
    if (!snippet.empty()) {
        message += L": " + snippet;
    } else {
        message += L".";
    }
    return message;
}

void AppendEscaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const unsigned char character : text) {
        switch (character) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (character < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[character >> 4]);
                out.push_back(hex[character & 0x0F]);
            } else {
                out.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    out.push_back('"');
}

void AppendUtf8Field(std::string& out, std::string_view key, const std::wstring& value) {
    AppendEscaped(out, key);
    out += ':';
    AppendEscaped(out, WideToUtf8(value));
}

void AppendOptionalField(std::string& out, bool& first, std::string_view key,
    const std::wstring& value) {
    if (value.empty()) {
        return;
    }
    if (!first) {
        out += ',';
    }
    first = false;
    AppendUtf8Field(out, key, value);
}

void AppendAlias(std::vector<std::wstring>& aliases, const std::wstring& name,
    const std::wstring& alias) {
    if (alias.empty()) {
        return;
    }
    for (const std::wstring& existing : aliases) {
        if (existing == alias || existing == name) {
            return;
        }
    }
    if (alias == name) {
        return;
    }
    aliases.push_back(alias);
}

void AppendCandidateObject(std::string& out, const LaunchCandidate& candidate) {
    // Compact: the same facts are repeated in questions.criteria, so the
    // structured copy keeps only discriminative fields. Long filesystem
    // paths, Start Menu folders, comments, and volatile window titles are
    // dropped here (they remain summarized in the criteria text when useful).
    out += '{';
    AppendEscaped(out, "id");
    out += ':';
    AppendEscaped(out, candidate.id);
    bool first = false;
    auto field = [&out, &first](std::string_view key, const std::wstring& value) {
        AppendOptionalField(out, first, key, value);
    };
    field("name", TruncateWide(candidate.name, kMaxStateFieldChars));

    std::vector<std::wstring> aliases;
    AppendAlias(aliases, candidate.name, candidate.pinName);
    AppendAlias(aliases, candidate.name, candidate.shortcutName);
    AppendAlias(aliases, candidate.name, candidate.productName);
    if (!aliases.empty()) {
        out += ",\"aliases\":[";
        for (size_t index = 0; index < aliases.size(); ++index) {
            if (index > 0) {
                out += ',';
            }
            AppendEscaped(out, WideToUtf8(TruncateWide(aliases[index], kMaxStateFieldChars)));
        }
        out += ']';
    }

    field("exe", TruncateWide(candidate.executable, kMaxStateFieldChars));
    field("aumid", TruncateWide(candidate.aumid, kMaxStateFieldChars));
    field("description", TruncateWide(candidate.description, kMaxStateFieldChars));
    field("product", TruncateWide(candidate.productName, kMaxStateFieldChars));
    field("publisher", TruncateWide(candidate.publisher, kMaxStateFieldChars));
    out += ",\"running\":";
    out += candidate.running ? "true" : "false";
    out += '}';
}

std::string CandidateDescription(const LaunchCandidate& candidate) {
    // Compact single-line rubric text for the choice criteria. Previously this
    // duplicated every verbose field (paths, targets, folders, comments,
    // full window titles), doubling payload size and token cost. Keep only
    // identity + kind signals, capped so one noisy app cannot blow the budget.
    std::wstring base = candidate.name;
    if (base.empty()) {
        base = candidate.executable;
    }
    if (base.empty()) {
        base = Utf8ToWide(candidate.id);
    }
    std::wstring text = TruncateWide(base, 80);
    auto append = [&text](std::wstring_view label, const std::wstring& value, size_t maxChars) {
        if (value.empty() || value == text) {
            return;
        }
        std::wstring clipped = TruncateWide(value, maxChars);
        if (clipped.empty() || text.find(clipped) != std::wstring::npos) {
            return;
        }
        text += L"; ";
        text += label;
        text += clipped;
    };
    if (candidate.shortcutName != candidate.name) {
        append(L"aka ", candidate.shortcutName, 40);
    }
    if (candidate.pinName != candidate.name && candidate.pinName != candidate.shortcutName) {
        append(L"aka ", candidate.pinName, 40);
    }
    append(L"exe ", candidate.executable, 40);
    append(L"", candidate.description, 100);
    append(L"product ", candidate.productName, 60);
    append(L"by ", candidate.publisher, 60);
    if (text.empty()) {
        text = Utf8ToWide(candidate.id);
    }
    text += candidate.running ? L"; currently running" : L"";
    return WideToUtf8(TruncateWide(text, kMaxCriteriaChars));
}

std::wstring LauncherSurface() {
    return L"The user typed this into an application launcher over every installed app, not only "
        L"dock pins. Interpret `request` as the app they want to open. A single noun naming a "
        L"product or a kind of app is a valid launch query. Use names, aliases, executable, "
        L"AUMID, description, publisher, and product.";
}

// Local pre-ranking: sending the full catalog (200+ apps x2 copies) caused
// huge payloads and generic HTTP 400s. Score cheaply on lexical overlap and
// keep the top-N so truncation drops irrelevant apps, not arbitrary ones.
// Semantic kind queries ("browser") still work when descriptions mention the
// kind ("Web browser"); zero-score fallbacks keep diverse coverage.
double CandidateRelevance(const LaunchCandidate& candidate, const std::wstring& queryLower,
    const std::vector<std::wstring>& tokens) {
    if (tokens.empty()) {
        return candidate.running ? 1.0 : 0.0;
    }
    const std::wstring name = ToLowerWide(candidate.name);
    const std::wstring exe = ToLowerWide(candidate.executable);
    const std::wstring shortcut = ToLowerWide(candidate.shortcutName);
    const std::wstring pin = ToLowerWide(candidate.pinName);
    const std::wstring product = ToLowerWide(candidate.productName);
    const std::wstring publisher = ToLowerWide(candidate.publisher);
    const std::wstring description = ToLowerWide(candidate.description);
    double score = 0.0;
    for (const std::wstring& token : tokens) {
        if (token.empty()) {
            continue;
        }
        if (name == token) {
            score += 100.0;
        } else if (!name.empty() && name.starts_with(token)) {
            score += 80.0;
        } else if (name.find(token) != std::wstring::npos) {
            score += 50.0;
        }
        if (!exe.empty()) {
            if (exe == token || exe == token + L".exe") {
                score += 70.0;
            } else if (exe.find(token) != std::wstring::npos) {
                score += 40.0;
            }
        }
        if (!shortcut.empty() && shortcut.find(token) != std::wstring::npos) {
            score += 45.0;
        }
        if (!pin.empty() && pin.find(token) != std::wstring::npos) {
            score += 45.0;
        }
        if (!product.empty() && product.find(token) != std::wstring::npos) {
            score += 30.0;
        }
        if (!description.empty() && description.find(token) != std::wstring::npos) {
            score += 12.0;
        }
        if (!publisher.empty() && publisher.find(token) != std::wstring::npos) {
            score += 8.0;
        }
    }
    if (!queryLower.empty() && !name.empty() && name.find(queryLower) != std::wstring::npos) {
        score += 25.0;
    }
    if (candidate.running) {
        score += 2.0;
    }
    return score;
}

std::vector<LaunchCandidate> SelectTopCandidates(
    const std::vector<LaunchCandidate>& candidates, const std::wstring& request, size_t limit) {
    if (candidates.size() <= limit) {
        return candidates;
    }
    const std::wstring queryLower = ToLowerWide(TrimKey(request));
    std::vector<std::wstring> tokens;
    size_t start = 0;
    while (start < queryLower.size()) {
        while (start < queryLower.size() && std::iswspace(queryLower[start]) != 0) {
            ++start;
        }
        size_t end = start;
        while (end < queryLower.size() && std::iswspace(queryLower[end]) == 0) {
            ++end;
        }
        if (end > start) {
            tokens.push_back(queryLower.substr(start, end - start));
        }
        start = end;
    }
    std::vector<std::pair<double, size_t>> ranked;
    ranked.reserve(candidates.size());
    for (size_t index = 0; index < candidates.size(); ++index) {
        ranked.emplace_back(CandidateRelevance(candidates[index], queryLower, tokens), index);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
        [](const auto& left, const auto& right) { return left.first > right.first; });
    std::vector<LaunchCandidate> top;
    top.reserve(limit);
    for (size_t rank = 0; rank < limit && rank < ranked.size(); ++rank) {
        top.push_back(candidates[ranked[rank].second]);
    }
    return top;
}

std::string BuildAppQuestions(const std::vector<LaunchCandidate>& candidates) {
    std::string body = "\"questions\":{\"app\":{\"type\":\"choice\",\"instructions\":";
    AppendEscaped(body,
        "Which entry in `candidates` should be opened for `request`? Use every candidate field. "
        "Match product names, nicknames, publishers, and the kind of application (browser, music "
        "player, AI assistant, settings, file manager). When several candidates could fit, pick "
        "the one whose primary purpose matches the request: a browser request should prefer a web "
        "browser, and an AI or chatbot request should prefer a dedicated AI assistant over an "
        "editor or other tool that merely includes AI features. Choose none only when no "
        "candidate is a reasonable match.");
    body += ",\"criteria\":{";
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) {
            body += ',';
        }
        AppendEscaped(body, candidates[index].id);
        body += ':';
        AppendEscaped(body, CandidateDescription(candidates[index]));
    }
    if (!candidates.empty()) {
        body += ',';
    }
    AppendEscaped(body, "none");
    body += ':';
    AppendEscaped(body, "No candidate is a reasonable match for the request.");
    body += "}},\"exists\":{\"type\":\"noul\",\"instructions\":";
    AppendEscaped(body,
        "Does `candidates` contain a reasonable match for `request`, including matching the "
        "kind of application rather than only the exact product name?");
    body += ",\"criteria\":{\"true\":";
    AppendEscaped(body,
        "A listed candidate is the named app, a synonym, or the obvious app of that kind in "
        "this catalog.");
    body += ",\"false\":";
    AppendEscaped(body, "Nothing in the catalog is a reasonable match for the request.");
    body += "}}}";
    return body;
}

std::string BuildRequestBody(const std::wstring& request,
    const std::vector<LaunchCandidate>& candidates) {
    std::string body;
    body.reserve(4096 + candidates.size() * 256);
    body += "{\"model\":\"jev-latest\",\"state\":{";
    AppendUtf8Field(body, "surface", LauncherSurface());
    body += ',';
    AppendUtf8Field(body, "request", request);
    body += ",\"candidates\":[";
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) {
            body += ',';
        }
        AppendCandidateObject(body, candidates[index]);
    }
    body += "]},";
    body += BuildAppQuestions(candidates);
    body += '}';
    return body;
}

std::string GroupSummary(const std::vector<LaunchCandidate>& group) {
    std::wstring text;
    for (size_t index = 0; index < group.size(); ++index) {
        if (index > 0) {
            text += L" | ";
        }
        text += group[index].name;
        if (!group[index].description.empty() && group[index].description != group[index].name) {
            text += L" (";
            text += group[index].description;
            text += L")";
        } else if (!group[index].publisher.empty()) {
            text += L" (";
            text += group[index].publisher;
            text += L")";
        }
        if (!group[index].aumid.empty()) {
            text += L" [";
            text += group[index].aumid;
            text += L"]";
        }
    }
    if (text.size() > 4000) {
        text.resize(4000);
    }
    return WideToUtf8(text);
}

std::string BuildGroupRequestBody(const std::wstring& request,
    const std::vector<std::vector<LaunchCandidate>>& groups) {
    std::string body = "{\"model\":\"jev-latest\",\"state\":{";
    AppendUtf8Field(body, "surface", LauncherSurface());
    body += ',';
    AppendUtf8Field(body, "request", request);
    body += ",\"groups\":[";
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        if (groupIndex > 0) {
            body += ',';
        }
        body += "{\"id\":";
        AppendEscaped(body, "g" + std::to_string(groupIndex));
        body += ",\"apps\":[";
        for (size_t index = 0; index < groups[groupIndex].size(); ++index) {
            if (index > 0) {
                body += ',';
            }
            AppendCandidateObject(body, groups[groupIndex][index]);
        }
        body += "]}";
    }
    body += "]},\"questions\":{\"app\":{\"type\":\"choice\",\"instructions\":";
    AppendEscaped(body,
        "Which group in `groups` contains the application that should be opened for `request`? "
        "Use names, aliases, AUMIDs, descriptions, and publishers. Choose none only if no group "
        "contains a reasonable match, including a kind-of-app match such as a browser or AI "
        "assistant.");
    body += ",\"criteria\":{";
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        if (groupIndex > 0) {
            body += ',';
        }
        AppendEscaped(body, "g" + std::to_string(groupIndex));
        body += ':';
        AppendEscaped(body, GroupSummary(groups[groupIndex]));
    }
    body += ',';
    AppendEscaped(body, "none");
    body += ':';
    AppendEscaped(body, "No group contains a reasonable match for the request.");
    body += "}},\"exists\":{\"type\":\"noul\",\"instructions\":";
    AppendEscaped(body,
        "Does any group contain a reasonable match for `request`, including matching the kind "
        "of application rather than only the exact product name?");
    body += ",\"criteria\":{\"true\":";
    AppendEscaped(body, "At least one listed app is a reasonable match.");
    body += ",\"false\":";
    AppendEscaped(body, "No listed app is a reasonable match.");
    body += "}}}}";
    return body;
}

struct JsonParser {
    std::string_view text;
    size_t index = 0;

    void SkipWhitespace() noexcept {
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
        }
    }

    [[nodiscard]] bool Consume(char expected) noexcept {
        SkipWhitespace();
        if (index >= text.size() || text[index] != expected) {
            return false;
        }
        ++index;
        return true;
    }

    [[nodiscard]] std::optional<std::string> ParseString() {
        SkipWhitespace();
        if (index >= text.size() || text[index] != '"') {
            return std::nullopt;
        }
        ++index;
        std::string value;
        while (index < text.size()) {
            const char character = text[index];
            ++index;
            if (character == '"') {
                return value;
            }
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            if (index >= text.size()) {
                return std::nullopt;
            }
            const char escaped = text[index];
            ++index;
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u': {
                if (index + 4 > text.size()) {
                    return std::nullopt;
                }
                unsigned code = 0;
                for (int digit = 0; digit < 4; ++digit) {
                    const unsigned char hex = static_cast<unsigned char>(text[index]);
                    ++index;
                    code <<= 4;
                    if (hex >= '0' && hex <= '9') {
                        code += static_cast<unsigned>(hex - '0');
                    } else if (hex >= 'a' && hex <= 'f') {
                        code += static_cast<unsigned>(hex - 'a' + 10);
                    } else if (hex >= 'A' && hex <= 'F') {
                        code += static_cast<unsigned>(hex - 'A' + 10);
                    } else {
                        return std::nullopt;
                    }
                }
                if (code < 0x80) {
                    value.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    value.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    value.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    value.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    value.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<double> ParseNumber() {
        SkipWhitespace();
        if (index >= text.size()) {
            return std::nullopt;
        }
        const size_t start = index;
        if (text[index] == '-') {
            ++index;
        }
        if (index >= text.size() || std::isdigit(static_cast<unsigned char>(text[index])) == 0) {
            index = start;
            return std::nullopt;
        }
        while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
        }
        if (index < text.size() && text[index] == '.') {
            ++index;
            while (index < text.size() &&
                std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
                ++index;
            }
        }
        if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
            ++index;
            if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
                ++index;
            }
            while (index < text.size() &&
                std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
                ++index;
            }
        }

        try {
            return std::stod(std::string(text.substr(start, index - start)));
        } catch (...) {
            return std::nullopt;
        }
    }

    bool SkipValue();

    bool SkipObject() {
        if (!Consume('{')) {
            return false;
        }
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        while (true) {
            if (!ParseString()) {
                return false;
            }
            if (!Consume(':') || !SkipValue()) {
                return false;
            }
            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
        }
    }

    bool SkipArray() {
        if (!Consume('[')) {
            return false;
        }
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        while (true) {
            if (!SkipValue()) {
                return false;
            }
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
        }
    }

    bool SkipLiteral(std::string_view literal) {
        SkipWhitespace();
        if (text.substr(index, literal.size()) != literal) {
            return false;
        }
        index += literal.size();
        return true;
    }

    [[nodiscard]] bool FindField(std::string_view key) {
        const size_t objectStart = index;
        if (!Consume('{')) {
            return false;
        }
        SkipWhitespace();
        if (Consume('}')) {
            index = objectStart;
            return false;
        }
        while (true) {
            const std::optional<std::string> parsedKey = ParseString();
            if (!parsedKey || !Consume(':')) {
                index = objectStart;
                return false;
            }
            if (*parsedKey == key) {
                return true;
            }
            if (!SkipValue()) {
                index = objectStart;
                return false;
            }
            SkipWhitespace();
            if (Consume('}')) {
                index = objectStart;
                return false;
            }
            if (!Consume(',')) {
                index = objectStart;
                return false;
            }
        }
    }
};

bool JsonParser::SkipValue() {
    SkipWhitespace();
    if (index >= text.size()) {
        return false;
    }
    const char character = text[index];
    if (character == '"') {
        return ParseString().has_value();
    }
    if (character == '{') {
        return SkipObject();
    }
    if (character == '[') {
        return SkipArray();
    }
    if (character == 't') {
        return SkipLiteral("true");
    }
    if (character == 'f') {
        return SkipLiteral("false");
    }
    if (character == 'n') {
        return SkipLiteral("null");
    }
    return ParseNumber().has_value();
}

struct ParsedAnswers {
    std::string choice;
    double confidence = 0.0;
    double exists = 0.0;
};

std::optional<ParsedAnswers> ParseAnswers(const std::string& body) {
    JsonParser parser{body, 0};
    if (!parser.FindField("answers")) {
        return std::nullopt;
    }

    const size_t answersStart = parser.index;
    ParsedAnswers answers;
    parser.index = answersStart;
    if (!parser.FindField("app")) {
        return std::nullopt;
    }
    const size_t appStart = parser.index;
    parser.index = appStart;
    if (!parser.FindField("choice")) {
        return std::nullopt;
    }
    const std::optional<std::string> choice = parser.ParseString();
    if (!choice) {
        return std::nullopt;
    }
    answers.choice = *choice;

    parser.index = appStart;
    if (parser.FindField("confidence")) {
        const std::optional<double> confidence = parser.ParseNumber();
        if (confidence) {
            answers.confidence = *confidence;
        }
    }

    parser.index = answersStart;
    if (!parser.FindField("exists")) {
        return std::nullopt;
    }
    if (!parser.FindField("noul")) {
        return std::nullopt;
    }
    const std::optional<double> exists = parser.ParseNumber();
    if (!exists) {
        return std::nullopt;
    }
    answers.exists = *exists;
    return answers;
}

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) noexcept : m_handle(handle) {
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = nullptr;
    }

    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    ~WinHttpHandle() {
        Reset();
    }

    [[nodiscard]] HINTERNET Get() const noexcept {
        return m_handle;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return m_handle != nullptr;
    }

private:
    void Reset() noexcept {
        if (m_handle != nullptr) {
            WinHttpCloseHandle(m_handle);
            m_handle = nullptr;
        }
    }

    HINTERNET m_handle = nullptr;
};

LaunchJudgment HttpError(std::wstring message) {
    LaunchJudgment judgment;
    judgment.action = LaunchJudgment::Action::Error;
    judgment.error = std::move(message);
    return judgment;
}

bool PostRawSystemOne(const std::wstring& apiKey, const std::string& body,
    std::string& response, std::wstring& error) {
    WinHttpHandle session(WinHttpOpen(L"LiquidGlassDock/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = L"Could not open an HTTPS session.";
        return false;
    }

    WinHttpSetTimeouts(session.Get(), kConnectTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
        kReceiveTimeoutMs);

    WinHttpHandle connection(WinHttpConnect(session.Get(), kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) {
        error = L"Could not reach api.typesafe.ai.";
        return false;
    }

    WinHttpHandle request(WinHttpOpenRequest(connection.Get(), L"POST", kApiPath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        error = L"Could not create the TypeSafe request.";
        return false;
    }

    std::wstring headers = L"Authorization: Bearer ";
    headers += SanitizeApiKey(apiKey);
    headers += L"\r\nContent-Type: application/json";
    if (WinHttpAddRequestHeaders(request.Get(), headers.c_str(), static_cast<DWORD>(-1L),
            WINHTTP_ADDREQ_FLAG_ADD) == FALSE) {
        error = L"Could not set TypeSafe request headers.";
        return false;
    }

    if (body.size() > static_cast<size_t>((std::numeric_limits<DWORD>::max)())) {
        error = L"TypeSafe request is too large to send.";
        return false;
    }
    const DWORD bodySize = static_cast<DWORD>(body.size());
    if (WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            body.empty() ? nullptr : const_cast<char*>(body.data()), bodySize, bodySize, 0) ==
        FALSE) {
        error = L"Could not send the TypeSafe request.";
        return false;
    }
    if (WinHttpReceiveResponse(request.Get(), nullptr) == FALSE) {
        error = L"TypeSafe did not respond.";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(request.Get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &statusSize, WINHTTP_NO_HEADER_INDEX) == FALSE) {
        error = L"TypeSafe returned an unreadable status.";
        return false;
    }

    response.clear();
    while (true) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request.Get(), &available) == FALSE) {
            error = L"TypeSafe response was truncated.";
            return false;
        }
        if (available == 0) {
            break;
        }
        const size_t offset = response.size();
        response.resize(offset + available);
        DWORD read = 0;
        if (WinHttpReadData(request.Get(), response.data() + offset, available, &read) == FALSE) {
            error = L"TypeSafe response could not be read.";
            return false;
        }
        response.resize(offset + read);
        if (read == 0) {
            break;
        }
    }

    if (status == 401) {
        error = L"TypeSafe API key was rejected.";
        return false;
    }
    if (status == 429 || status == 529) {
        error = L"TypeSafe is busy. Try again in a moment.";
        return false;
    }
    if (status == 422) {
        error = StatusErrorWithBody(L"TypeSafe rejected the request", status, response);
        return false;
    }
    if (status == 400) {
        error = StatusErrorWithBody(L"TypeSafe rejected the request", status, response);
        return false;
    }
    if (status < 200 || status > 299) {
        error = StatusErrorWithBody(L"TypeSafe request failed", status, response);
        return false;
    }
    return true;
}

LaunchJudgment PostSystemOne(const std::wstring& apiKey, const std::string& body) {
    std::string response;
    std::wstring error;
    if (!PostRawSystemOne(apiKey, body, response, error)) {
        return HttpError(error);
    }

    const std::optional<ParsedAnswers> answers = ParseAnswers(response);
    if (!answers) {
        return HttpError(L"TypeSafe returned an unexpected response.");
    }

    LaunchJudgment judgment;
    judgment.chosenId = answers->choice;
    judgment.exists = answers->exists;
    judgment.confidence = answers->confidence;

    if (answers->choice == "none" || answers->choice.empty() ||
        answers->exists < kExistsLaunch) {
        judgment.action = LaunchJudgment::Action::None;
        return judgment;
    }

    judgment.action = LaunchJudgment::Action::Launch;
    return judgment;
}

// --- Performance Boost (Quick Settings) ----------------------------------
// One narrow judgment per question, asked together over the same state:
// a Noul for "safe to close" and a Noul for "idle background work" per
// process. Code composes the answers with high thresholds and owns every
// close; uncertain Noul values near 0.5 never act.

constexpr size_t kBoostBatchSize = 25;

void AppendJsonDouble(std::string& out, double value) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.1f", value);
    out += text;
}

void AppendBoostProcess(std::string& out, const BoostCandidate& candidate) {
    out += '{';
    AppendEscaped(out, "id");
    out += ':';
    AppendEscaped(out, candidate.id);
    out += ',';
    AppendUtf8Field(out, "name", candidate.name);
    out += ',';
    AppendUtf8Field(out, "exe", candidate.exePath);
    out += ",\"memory_mb\":";
    AppendJsonDouble(out, candidate.memoryMb);
    out += ",\"cpu_pct\":";
    AppendJsonDouble(out, candidate.cpuPercent);
    out += ",\"has_window\":";
    out += candidate.hasWindow ? "true" : "false";
    out += ',';
    AppendUtf8Field(out, "window_title", candidate.windowTitle);
    out += ",\"age_min\":";
    out += std::to_string(candidate.ageSeconds / 60ULL);
    out += '}';
}

std::string BuildBoostRequestBody(const std::vector<BoostCandidate>& candidates) {
    std::string body = "{\"model\":\"jev-latest\",\"state\":{\"surface\":";
    AppendEscaped(body,
        "Windows Quick Settings Boost performance in a desktop dock. Code enumerated "
        "candidate processes with memory, CPU, window, and age. Code enforces its own "
        "denylist and owns all closing; Jev only supplies safe/idle probabilities.");
    body += ",\"focus\":";
    AppendEscaped(body,
        "Keep the foreground app and important user work fast by reclaiming memory "
        "from idle background work that is safe to close.");
    body += ',';
    AppendEscaped(body, "policy");
    body += ':';
    AppendEscaped(body,
        "Never approve closing operating-system components, drivers, security "
        "software, shell hosts, console hosts, or anything holding important unsaved "
        "work. When the evidence is ambiguous, answer no.");
    body += ",\"processes\":[";
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) {
            body += ',';
        }
        AppendBoostProcess(body, candidates[index]);
    }
    body += "]},\"questions\":{";
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) {
            body += ',';
        }
        const std::string ref = "`processes[" + std::to_string(index) + "]`";
        AppendEscaped(body, "safe_" + candidates[index].id);
        body += ":{\"type\":\"noul\",\"instructions\":";
        AppendEscaped(body,
            "Is " + ref + " safe to close right now? `policy` forbids harming Windows "
            "stability, drivers, security software, shell hosts, or important unsaved "
            "user work. Answer yes only when the process is ordinary user software "
            "whose close is harmless or recoverable by reopening the app.");
        body += ",\"criteria\":{\"true\":";
        AppendEscaped(body,
            "Ordinary user app or background agent; closing is harmless or recoverable.");
        body += ",\"false\":";
        AppendEscaped(body,
            "OS, driver, security, shell, or actively important work; closing risks "
            "stability or data loss.");
        body += "}},";
        AppendEscaped(body, "idle_" + candidates[index].id);
        body += ":{\"type\":\"noul\",\"instructions\":";
        AppendEscaped(body,
            "Is " + ref + " idle background work rather than actively used? An empty "
            "`window_title` means it shows no window. Near-zero `cpu_pct` with a large "
            "`age_min` suggests idle. A `window_title` naming media, meetings, calls, "
            "downloads, gaming, recording, or an edited document means in use even at "
            "low CPU.");
        body += ",\"criteria\":{\"true\":";
        AppendEscaped(body,
            "No window and near-zero CPU, or a background agent with no sign of use.");
        body += ",\"false\":";
        AppendEscaped(body,
            "Has a window suggesting current use, notable CPU, or just started.");
        body += "}}";
    }
    body += "}}";
    return body;
}

// Generic Noul map: every answer object carrying a "noul" number is
// collected under its question id. Choice/Score answers are ignored.
bool ParseNoulAnswers(const std::string& body, std::map<std::string, double>& out) {
    JsonParser parser{body, 0};
    if (!parser.FindField("answers")) {
        return false;
    }
    if (!parser.Consume('{')) {
        return false;
    }
    parser.SkipWhitespace();
    if (parser.Consume('}')) {
        return true;
    }
    while (true) {
        const std::optional<std::string> key = parser.ParseString();
        if (!key || !parser.Consume(':')) {
            return false;
        }
        parser.SkipWhitespace();
        if (parser.index < parser.text.size() && parser.text[parser.index] == '{') {
            if (!parser.Consume('{')) {
                return false;
            }
            std::optional<double> noul;
            parser.SkipWhitespace();
            if (parser.Consume('}')) {
                // Empty answer object: nothing to record.
            } else {
                while (true) {
                    const std::optional<std::string> field = parser.ParseString();
                    if (!field || !parser.Consume(':')) {
                        return false;
                    }
                    if (*field == "noul") {
                        noul = parser.ParseNumber();
                        if (!noul) {
                            return false;
                        }
                    } else if (!parser.SkipValue()) {
                        return false;
                    }
                    parser.SkipWhitespace();
                    if (parser.Consume('}')) {
                        break;
                    }
                    if (!parser.Consume(',')) {
                        return false;
                    }
                }
            }
            if (noul) {
                out.emplace(*key, *noul);
            }
        } else if (!parser.SkipValue()) {
            return false;
        }
        parser.SkipWhitespace();
        if (parser.Consume('}')) {
            return true;
        }
        if (!parser.Consume(',')) {
            return false;
        }
    }
}

struct NoulBatchResult {
    bool ok = false;
    std::map<std::string, double> values;
    std::wstring error;
};

NoulBatchResult PostNoulBatch(const std::wstring& apiKey, const std::string& body) {
    NoulBatchResult result;
    std::string response;
    if (!PostRawSystemOne(apiKey, body, response, result.error)) {
        return result;
    }
    if (!ParseNoulAnswers(response, result.values)) {
        result.error = L"TypeSafe returned an unexpected response.";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace

LaunchJudgment TypeSafeClient::ResolveApp(const std::wstring& apiKey, const std::wstring& request,
    const std::vector<LaunchCandidate>& candidates) {
    const std::wstring cleanKey = SanitizeApiKey(apiKey);
    if (cleanKey.empty()) {
        return HttpError(
            L"Set TYPESAFE_API_KEY or TypeSafeApiKey in dock.ini.");
    }
    const std::wstring cleanRequest = NormalizeRequest(request);
    if (cleanRequest.empty() || candidates.empty()) {
        LaunchJudgment judgment;
        judgment.action = LaunchJudgment::Action::None;
        return judgment;
    }

    const std::vector<LaunchCandidate> ranked = SelectTopCandidates(candidates, cleanRequest,
        kMaxAppsPerChoice);
    if (ranked.size() <= kMaxAppsPerChoice) {
        return PostSystemOne(cleanKey, BuildRequestBody(cleanRequest, ranked));
    }

    std::vector<std::vector<LaunchCandidate>> groups;
    for (size_t start = 0; start < ranked.size(); start += kGroupSize) {
        const size_t end = std::min(ranked.size(), start + kGroupSize);
        groups.emplace_back(ranked.begin() + static_cast<std::ptrdiff_t>(start),
            ranked.begin() + static_cast<std::ptrdiff_t>(end));
    }

    LaunchJudgment group = PostSystemOne(cleanKey, BuildGroupRequestBody(cleanRequest, groups));
    if (group.action != LaunchJudgment::Action::Launch || group.chosenId.size() < 2 ||
        group.chosenId.front() != 'g') {
        if (group.action == LaunchJudgment::Action::Error) {
            return group;
        }
        group.action = LaunchJudgment::Action::None;
        return group;
    }

    size_t groupIndex = 0;
    try {
        groupIndex = static_cast<size_t>(std::stoul(group.chosenId.substr(1)));
    } catch (...) {
        group.action = LaunchJudgment::Action::None;
        return group;
    }
    if (groupIndex >= groups.size()) {
        group.action = LaunchJudgment::Action::None;
        return group;
    }

    return PostSystemOne(cleanKey, BuildRequestBody(cleanRequest, groups[groupIndex]));
}

BoostResult TypeSafeClient::ClassifyForBoost(const std::wstring& apiKey,
    const std::vector<BoostCandidate>& candidates) {
    BoostResult result;
    const std::wstring cleanKey = SanitizeApiKey(apiKey);
    if (cleanKey.empty()) {
        result.error = L"Set TYPESAFE_API_KEY or TypeSafeApiKey in dock.ini.";
        return result;
    }
    if (candidates.empty()) {
        result.ok = true;
        return result;
    }

    std::map<std::string, BoostJudgment> merged;
    for (size_t start = 0; start < candidates.size(); start += kBoostBatchSize) {
        const size_t end = std::min(candidates.size(), start + kBoostBatchSize);
        const std::vector<BoostCandidate> batch(candidates.begin() +
                static_cast<std::ptrdiff_t>(start),
            candidates.begin() + static_cast<std::ptrdiff_t>(end));
        const NoulBatchResult reply = PostNoulBatch(cleanKey, BuildBoostRequestBody(batch));
        if (!reply.ok) {
            result.error = reply.error;
            return result;
        }
        for (const BoostCandidate& candidate : batch) {
            const auto safe = reply.values.find("safe_" + candidate.id);
            const auto idle = reply.values.find("idle_" + candidate.id);
            if (safe == reply.values.end() || idle == reply.values.end()) {
                continue;  // Unanswered: treat as uncertain, never close.
            }
            merged.emplace(candidate.id, BoostJudgment{candidate.id, safe->second, idle->second});
        }
    }

    result.ok = true;
    result.items.reserve(merged.size());
    for (const BoostCandidate& candidate : candidates) {
        const auto judgment = merged.find(candidate.id);
        if (judgment != merged.end()) {
            result.items.push_back(judgment->second);
        }
    }
    return result;
}
