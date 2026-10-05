#include "LlamaServerClient.h"

#include <Windows.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kDefaultHost[] = L"127.0.0.1";
constexpr INTERNET_PORT kDefaultPort = 8080;
constexpr DWORD kConnectTimeoutMs = 1500;
constexpr DWORD kSendTimeoutMs = 8000;
constexpr DWORD kReceiveTimeoutMs = 45000;
constexpr size_t kMaxCandidates = 16;
constexpr size_t kMaxRequestChars = 200;
constexpr size_t kMaxFieldChars = 64;
constexpr size_t kMaxResponseBytes = 256 * 1024;
constexpr double kFuzzyMinScore = 40.0;

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
        count);
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
        count, nullptr, nullptr);
    return result;
}

std::wstring TrimWide(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](wchar_t c) { return std::iswspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](wchar_t c) { return std::iswspace(c) != 0; })
                          .base();
    return first >= last ? L"" : std::wstring(first, last);
}

std::wstring ToLowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring TruncateWide(const std::wstring& value, size_t maxChars) {
    if (value.size() <= maxChars) {
        return value;
    }
    if (maxChars <= 1) {
        return L"?";
    }
    return value.substr(0, maxChars - 1) + L"?";
}

void AppendEscaped(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
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
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(ch) & 0xff);
                out += buf;
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back('"');
}

void AppendUtf8Field(std::string& out, const char* key, const std::wstring& value) {
    out.push_back('"');
    out += key;
    out += "\":";
    AppendEscaped(out, WideToUtf8(TruncateWide(value, kMaxFieldChars)));
}

struct ParsedUrl {
    std::wstring host = kDefaultHost;
    INTERNET_PORT port = kDefaultPort;
    bool https = false;
};

ParsedUrl ParseBaseUrl(const std::wstring& raw) {
    ParsedUrl result;
    std::wstring url = TrimWide(raw);
    if (url.empty()) {
        return result;
    }
    std::wstring lower = ToLowerWide(url);
    size_t hostStart = 0;
    if (lower.starts_with(L"https://")) {
        result.https = true;
        hostStart = 8;
    } else if (lower.starts_with(L"http://")) {
        hostStart = 7;
    }
    size_t hostEnd = url.find(L'/', hostStart);
    std::wstring hostPort =
        hostEnd == std::wstring::npos ? url.substr(hostStart) : url.substr(hostStart, hostEnd - hostStart);
    const size_t colon = hostPort.find(L':');
    if (colon == std::wstring::npos) {
        result.host = hostPort.empty() ? kDefaultHost : hostPort;
        result.port = result.https ? INTERNET_DEFAULT_HTTPS_PORT : kDefaultPort;
    } else {
        result.host = hostPort.substr(0, colon);
        try {
            result.port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(colon + 1)));
        } catch (...) {
            result.port = kDefaultPort;
        }
    }
    if (result.host.empty()) {
        result.host = kDefaultHost;
    }
    return result;
}

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) noexcept : m_handle(handle) {}
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
    ~WinHttpHandle() { Reset(); }
    [[nodiscard]] HINTERNET Get() const noexcept { return m_handle; }
    void Reset() noexcept {
        if (m_handle != nullptr) {
            WinHttpCloseHandle(m_handle);
            m_handle = nullptr;
        }
    }

private:
    HINTERNET m_handle = nullptr;
};

LaunchJudgment MakeError(std::wstring message) {
    LaunchJudgment judgment;
    judgment.action = LaunchJudgment::Action::Error;
    judgment.error = std::move(message);
    return judgment;
}

// One WinHTTP session per worker thread so TCP/TLS stay warm across health,
// ranking, and agent steps (Connection: keep-alive by default).
thread_local WinHttpHandle g_httpSession;
thread_local std::wstring g_httpSessionHost;
thread_local INTERNET_PORT g_httpSessionPort = 0;

HINTERNET HttpSessionFor(const ParsedUrl& url, std::wstring& error) {
    if (g_httpSession.Get() == nullptr || g_httpSessionHost != url.host ||
        g_httpSessionPort != url.port) {
        g_httpSession.Reset();
        g_httpSession = WinHttpHandle(WinHttpOpen(L"HoverdockLlama/1.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (g_httpSession.Get() == nullptr) {
            error = L"Could not open HTTP session.";
            return nullptr;
        }
        g_httpSessionHost = url.host;
        g_httpSessionPort = url.port;
    }
    return g_httpSession.Get();
}

bool HttpExchange(const ParsedUrl& url, const wchar_t* method, const wchar_t* path,
    const std::string* body, DWORD& status, std::string& response, std::wstring& error,
    DWORD receiveTimeoutMs = kReceiveTimeoutMs) {
    status = 0;
    response.clear();
    HINTERNET session = HttpSessionFor(url, error);
    if (session == nullptr) {
        return false;
    }
    WinHttpSetTimeouts(session, kConnectTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
        static_cast<int>(receiveTimeoutMs));
    WinHttpHandle connection(WinHttpConnect(session, url.host.c_str(), url.port, 0));
    if (connection.Get() == nullptr) {
        error = L"Could not connect to llama-server.";
        // Drop the cached session so the next try re-opens cleanly.
        g_httpSession.Reset();
        g_httpSessionHost.clear();
        g_httpSessionPort = 0;
        return false;
    }
    const DWORD flags = url.https ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connection.Get(), method, path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (request.Get() == nullptr) {
        error = L"Could not create HTTP request.";
        return false;
    }
    std::wstring headers = L"Accept: application/json\r\nConnection: keep-alive\r\n";
    if (body != nullptr) {
        headers += L"Content-Type: application/json\r\n";
    }
    if (WinHttpAddRequestHeaders(request.Get(), headers.c_str(), static_cast<DWORD>(-1L),
            WINHTTP_ADDREQ_FLAG_ADD) == FALSE) {
        error = L"Could not set HTTP headers.";
        return false;
    }
    const LPVOID bodyPtr = body == nullptr
        ? WINHTTP_NO_REQUEST_DATA
        : reinterpret_cast<LPVOID>(const_cast<char*>(body->data()));
    const DWORD bodyLen = body == nullptr ? 0 : static_cast<DWORD>(body->size());
    if (WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyPtr, bodyLen, bodyLen,
            0) == FALSE) {
        error = L"llama-server request failed (is the server running?).";
        g_httpSession.Reset();
        g_httpSessionHost.clear();
        g_httpSessionPort = 0;
        return false;
    }
    if (WinHttpReceiveResponse(request.Get(), nullptr) == FALSE) {
        error = L"llama-server did not respond.";
        return false;
    }
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(request.Get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &statusSize, WINHTTP_NO_HEADER_INDEX) == FALSE) {
        error = L"Could not read llama-server status.";
        return false;
    }
    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request.Get(), &available) == FALSE) {
            error = L"Could not read llama-server body.";
            return false;
        }
        if (available == 0) {
            break;
        }
        if (response.size() + available > kMaxResponseBytes) {
            error = L"llama-server response too large.";
            response.clear();
            return false;
        }
        const size_t offset = response.size();
        response.resize(offset + available);
        DWORD read = 0;
        if (WinHttpReadData(request.Get(), response.data() + offset, available, &read) == FALSE) {
            error = L"Could not read llama-server body.";
            return false;
        }
        response.resize(offset + read);
    }
    return true;
}

std::vector<std::wstring> Tokenize(const std::wstring& queryLower) {
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
    return tokens;
}

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

std::vector<LaunchCandidate> SelectTopCandidates(const std::vector<LaunchCandidate>& candidates,
    const std::wstring& request, size_t limit) {
    if (candidates.size() <= limit) {
        return candidates;
    }
    const std::wstring queryLower = ToLowerWide(TrimWide(request));
    const std::vector<std::wstring> tokens = Tokenize(queryLower);
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

void AppendCandidate(std::string& out, const LaunchCandidate& candidate) {
    out += '{';
    AppendUtf8Field(out, "id", Utf8ToWide(candidate.id));
    out += ',';
    AppendUtf8Field(out, "name", candidate.name);
    if (!candidate.executable.empty()) {
        out += ',';
        AppendUtf8Field(out, "exe", candidate.executable);
    }
    if (!candidate.productName.empty()) {
        out += ',';
        AppendUtf8Field(out, "product", candidate.productName);
    }
    if (!candidate.description.empty()) {
        out += ',';
        AppendUtf8Field(out, "desc", candidate.description);
    }
    if (candidate.running) {
        out += ",\"running\":true";
    }
    out += '}';
}

std::string BuildChatBody(const std::wstring& request, const std::vector<LaunchCandidate>& candidates) {
    // Keep byte-identical across requests so llama-server prompt cache hits.
    constexpr char kSystem[] =
        "Hoverdock Search. Pick best installed app. JSON only: "
        "{\"id\":\"cN\"} or {\"id\":\"none\"} or {\"path\":\"C:\\\\folder\"}. "
        "Prefer primary purpose. No prose.";

    std::string body = "{\"model\":\"local\",\"temperature\":0,\"max_tokens\":48,"
                       "\"cache_prompt\":true,"
                       "\"chat_template_kwargs\":{\"enable_thinking\":false},"
                       "\"reasoning_format\":\"none\",\"messages\":[";
    body += "{\"role\":\"system\",\"content\":";
    AppendEscaped(body, kSystem);
    body += "},{\"role\":\"user\",\"content\":";
    std::string user = "/no_think\nrequest=";
    user += WideToUtf8(TruncateWide(TrimWide(request), kMaxRequestChars));
    user += "\napps=[";
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i > 0) {
            user += ',';
        }
        const LaunchCandidate& c = candidates[i];
        user += '{';
        AppendUtf8Field(user, "id", Utf8ToWide(c.id));
        user += ',';
        AppendUtf8Field(user, "name", TruncateWide(c.name, 40));
        if (!c.executable.empty()) {
            user += ',';
            AppendUtf8Field(user, "exe", TruncateWide(c.executable, 28));
        }
        if (c.running) {
            user += ",\"running\":true";
        }
        user += '}';
    }
    user += "]";
    AppendEscaped(body, user);
    body += "}]}";
    return body;
}

std::optional<std::string> ExtractJsonObject(const std::string& text) {
    const size_t start = text.find('{');
    if (start == std::string::npos) {
        return std::nullopt;
    }
    int depth = 0;
    bool inString = false;
    bool escape = false;
    for (size_t i = start; i < text.size(); ++i) {
        const char ch = text[i];
        if (inString) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return text.substr(start, i - start + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> ExtractStringField(const std::string& json, std::string_view key) {
    const std::string needle = std::string("\"") + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string value;
    bool escape = false;
    for (; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (escape) {
            value.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

std::optional<std::string> ExtractAssistantContent(const std::string& response) {
    const std::string key = "\"content\"";
    size_t pos = 0;
    while ((pos = response.find(key, pos)) != std::string::npos) {
        size_t colon = response.find(':', pos + key.size());
        if (colon == std::string::npos) {
            return std::nullopt;
        }
        ++colon;
        while (colon < response.size() && (response[colon] == ' ' || response[colon] == '\t')) {
            ++colon;
        }
        if (colon < response.size() && response[colon] == '"') {
            ++colon;
            std::string value;
            bool escape = false;
            for (; colon < response.size(); ++colon) {
                const char ch = response[colon];
                if (escape) {
                    if (ch == 'n') {
                        value.push_back('\n');
                    } else if (ch == 'r') {
                        value.push_back('\r');
                    } else if (ch == 't') {
                        value.push_back('\t');
                    } else {
                        value.push_back(ch);
                    }
                    escape = false;
                    continue;
                }
                if (ch == '\\') {
                    escape = true;
                    continue;
                }
                if (ch == '"') {
                    if (!value.empty()) {
                        return value;
                    }
                    break;
                }
                value.push_back(ch);
            }
        }
        pos += key.size();
    }
    return std::nullopt;
}

std::optional<std::wstring> ValidateOpenPath(std::wstring raw, std::wstring& why);

bool IsKnownCandidateId(const std::string& id, const std::vector<LaunchCandidate>& candidates) {
    return std::ranges::any_of(candidates, [&](const LaunchCandidate& c) { return c.id == id; });
}

LaunchJudgment ParseModelJudgment(const std::string& response,
    const std::vector<LaunchCandidate>& candidates) {
    const auto content = ExtractAssistantContent(response);
    if (!content.has_value()) {
        return MakeError(L"llama-server returned no assistant content.");
    }
    const auto object = ExtractJsonObject(*content);
    const std::string& json = object.has_value() ? *object : *content;

    if (const auto path = ExtractStringField(json, "path"); path.has_value() && !path->empty()) {
        std::wstring why;
        if (const auto safe = ValidateOpenPath(Utf8ToWide(*path), why); safe.has_value()) {
            LaunchJudgment judgment;
            judgment.action = LaunchJudgment::Action::Launch;
            judgment.openPath = *safe;
            judgment.exists = 1.0;
            judgment.confidence = 0.85;
            return judgment;
        }
        // Model suggested an unsafe/missing path: treat as uncertain so fuzzy can try.
        LaunchJudgment judgment;
        judgment.action = LaunchJudgment::Action::Uncertain;
        return judgment;
    }

    std::optional<std::string> id = ExtractStringField(json, "id");
    if (!id.has_value()) {
        id = ExtractStringField(json, "choice");
    }
    if (!id.has_value()) {
        id = ExtractStringField(json, "chosenId");
    }
    if (!id.has_value() || *id == "none" || *id == "null") {
        LaunchJudgment judgment;
        judgment.action = LaunchJudgment::Action::None;
        return judgment;
    }
    if (!IsKnownCandidateId(*id, candidates)) {
        LaunchJudgment judgment;
        judgment.action = LaunchJudgment::Action::Uncertain;
        return judgment;
    }
    LaunchJudgment judgment;
    judgment.action = LaunchJudgment::Action::Launch;
    judgment.chosenId = *id;
    judgment.exists = 1.0;
    judgment.confidence = 0.9;
    return judgment;
}

// ---------------------------------------------------------------------------
// Agent-in-search helpers
// ---------------------------------------------------------------------------

// CPU-only 27B runs ~1-2 tok/s generation and ~6 tok/s prompt eval, so keep
// every agent prompt tiny and the loop short. The static system prompt (which
// also carries the user's known folders) stays byte-identical across requests
// so llama-server's prompt cache can skip re-evaluating it.
constexpr int kAgentMaxSteps = 3;
constexpr size_t kAgentMaxActions = 3;
constexpr size_t kAgentContextApps = 6;
constexpr size_t kAgentSearchResults = 6;
constexpr int kAgentMaxTokens = 64;
constexpr DWORD kAgentReceiveTimeoutMs = 120000;
constexpr size_t kAgentMaxReplyChars = 120;
constexpr size_t kAgentMaxUrlChars = 2048;
constexpr double kConfidentMinScore = 80.0;
constexpr double kConfidentMargin = 30.0;

bool IsWordChar(wchar_t c) {
    return std::iswalnum(c) != 0 || c == L'_' || c == L'-';
}

std::wstring StripLeadingPhrases(std::wstring text) {
    // Longest phrases first so "show me " wins over "show ".
    static constexpr std::wstring_view prefixes[] = {
        L"please ", L"can you ", L"could you ", L"would you ", L"i want to ", L"i'd like to ",
        L"take me to ", L"navigate to ", L"bring up ", L"switch to ", L"browse to ", L"show me ",
        L"go to ", L"goto ", L"open up ", L"open ", L"launch ", L"start ", L"run ", L"visit ",
        L"browse ", L"show ", L"focus ",
    };
    text = TrimWide(std::move(text));
    bool stripped = true;
    while (stripped && !text.empty()) {
        stripped = false;
        const std::wstring lower = ToLowerWide(text);
        for (const std::wstring_view prefix : prefixes) {
            if (lower.starts_with(prefix)) {
                text = TrimWide(text.substr(prefix.size()));
                stripped = true;
                break;
            }
        }
    }
    const std::wstring lower = ToLowerWide(text);
    for (const std::wstring_view article : {std::wstring_view(L"the "), std::wstring_view(L"my ")}) {
        if (lower.starts_with(article)) {
            text = TrimWide(text.substr(article.size()));
            break;
        }
    }
    while (!text.empty() && (text.back() == L'.' || text.back() == L'!' || text.back() == L'?')) {
        text.pop_back();
    }
    return TrimWide(std::move(text));
}

bool LooksLikeDomain(const std::wstring& token) {
    std::wstring lower = ToLowerWide(token);
    if (lower.starts_with(L"http://") || lower.starts_with(L"https://")) {
        return lower.size() > 8 && lower.find_first_of(L" \t\r\n") == std::wstring::npos;
    }
    if (lower.empty() || lower.find_first_of(L" \t\r\n\\\"<>") != std::wstring::npos) {
        return false;
    }
    if (lower.starts_with(L"www.")) {
        return lower.size() > 5;
    }
    const size_t slash = lower.find(L'/');
    const std::wstring host = slash == std::wstring::npos ? lower : lower.substr(0, slash);
    const size_t dot = host.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= host.size()) {
        return false;
    }
    const std::wstring tld = host.substr(dot + 1);
    static constexpr std::wstring_view tlds[] = {
        L"com", L"org", L"net", L"io", L"dev", L"co", L"uk", L"ai", L"app", L"gg", L"tv",
        L"me", L"edu", L"gov", L"info", L"xyz", L"so", L"de", L"fr", L"eu", L"us", L"ca",
        L"au", L"ly", L"to", L"fm", L"sh", L"page", L"site", L"wiki",
    };
    return std::ranges::any_of(tlds, [&](std::wstring_view t) { return tld == t; });
}

std::optional<std::wstring> NormalizeUrl(std::wstring raw) {
    raw = TrimWide(std::move(raw));
    if (raw.empty() || raw.size() > kAgentMaxUrlChars) {
        return std::nullopt;
    }
    for (const wchar_t c : raw) {
        if (c < 0x20 || c == L'"' || c == L'<' || c == L'>' || c == L'\\') {
            return std::nullopt;
        }
    }
    std::wstring lower = ToLowerWide(raw);
    if (!lower.starts_with(L"http://") && !lower.starts_with(L"https://")) {
        if (lower.find(L"://") != std::wstring::npos || lower.find(L':') == 1) {
            return std::nullopt;  // other schemes (file:, ms-settings:, javascript:) or drive paths
        }
        if (!LooksLikeDomain(raw)) {
            return std::nullopt;
        }
        raw = L"https://" + raw;
    }
    std::wstring encoded;
    encoded.reserve(raw.size());
    for (const wchar_t c : raw) {
        if (c == L' ') {
            encoded += L"%20";
        } else {
            encoded.push_back(c);
        }
    }
    return encoded;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) && path != nullptr) {
        result = path;
    }
    if (path != nullptr) {
        CoTaskMemFree(path);
    }
    return result;
}

struct NamedFolder {
    const wchar_t* label;
    std::wstring path;
};

const std::vector<NamedFolder>& UserFolders() {
    static const std::vector<NamedFolder> folders = [] {
        std::vector<NamedFolder> list;
        const auto add = [&list](const wchar_t* label, REFKNOWNFOLDERID id) {
            std::wstring path = KnownFolder(id);
            if (!path.empty()) {
                list.push_back({label, std::move(path)});
            }
        };
        add(L"Downloads", FOLDERID_Downloads);
        add(L"Documents", FOLDERID_Documents);
        add(L"Desktop", FOLDERID_Desktop);
        add(L"Pictures", FOLDERID_Pictures);
        add(L"Music", FOLDERID_Music);
        add(L"Videos", FOLDERID_Videos);
        add(L"Home", FOLDERID_Profile);
        return list;
    }();
    return folders;
}

std::optional<std::wstring> FolderByName(std::wstring name) {
    name = ToLowerWide(TrimWide(std::move(name)));
    for (const std::wstring_view suffix : {std::wstring_view(L" folder"), std::wstring_view(L" directory"),
             std::wstring_view(L" dir")}) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            name = TrimWide(name.substr(0, name.size() - suffix.size()));
            break;
        }
    }
    struct Alias {
        std::wstring_view alias;
        std::wstring_view label;
    };
    constexpr Alias aliases[] = {
        {L"downloads", L"Downloads"}, {L"download", L"Downloads"},
        {L"documents", L"Documents"}, {L"document", L"Documents"}, {L"docs", L"Documents"},
        {L"desktop", L"Desktop"},
        {L"pictures", L"Pictures"}, {L"picture", L"Pictures"}, {L"photos", L"Pictures"},
        {L"music", L"Music"},
        {L"videos", L"Videos"}, {L"video", L"Videos"},
        {L"home", L"Home"}, {L"user", L"Home"}, {L"profile", L"Home"},
    };
    for (const Alias& alias : aliases) {
        if (name != alias.alias) {
            continue;
        }
        for (const NamedFolder& folder : UserFolders()) {
            if (alias.label == folder.label) {
                return folder.path;
            }
        }
    }
    return std::nullopt;
}

bool IsBlockedOpenExtension(const std::wstring& path) {
    const size_t dot = path.rfind(L'.');
    const size_t sep = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep)) {
        return false;
    }
    const std::wstring ext = ToLowerWide(path.substr(dot));
    // Anything that would execute code via ShellExecute "open". Apps must go
    // through launch_app (the curated catalog), never open_path.
    static constexpr std::wstring_view blocked[] = {
        L".exe", L".com", L".bat", L".cmd", L".ps1", L".psm1", L".psd1", L".vbs", L".vbe",
        L".js", L".jse", L".wsf", L".wsh", L".msi", L".msp", L".msc", L".scr", L".pif",
        L".lnk", L".url", L".reg", L".hta", L".cpl", L".jar", L".appref-ms", L".application",
        L".gadget", L".inf", L".sys", L".dll", L".scf", L".settingcontent-ms", L".library-ms",
    };
    return std::ranges::any_of(blocked, [&](std::wstring_view b) { return ext == b; });
}

// Returns a canonical existing path that is safe to ShellExecute("open").
std::optional<std::wstring> ValidateOpenPath(std::wstring raw, std::wstring& why) {
    raw = TrimWide(std::move(raw));
    while (raw.size() >= 2 && (raw.front() == L'"' || raw.front() == L'\'') && raw.back() == raw.front()) {
        raw = TrimWide(raw.substr(1, raw.size() - 2));
    }
    if (raw.empty()) {
        why = L"empty path";
        return std::nullopt;
    }
    if (const auto folder = FolderByName(raw); folder.has_value()) {
        return folder;
    }
    wchar_t expanded[MAX_PATH * 2] = {};
    const DWORD expandedLen = ExpandEnvironmentStringsW(raw.c_str(), expanded,
        static_cast<DWORD>(std::size(expanded)));
    if (expandedLen > 0 && expandedLen <= std::size(expanded)) {
        raw = expanded;
    }
    if (raw.starts_with(L"\\\\") || raw.starts_with(L"//") || raw.find(L"://") != std::wstring::npos) {
        why = L"network/URL paths not allowed";
        return std::nullopt;
    }
    if (raw.size() < 3 || raw[1] != L':' || (raw[2] != L'\\' && raw[2] != L'/')) {
        why = L"need an absolute path";
        return std::nullopt;
    }
    wchar_t full[MAX_PATH * 2] = {};
    const DWORD fullLen = GetFullPathNameW(raw.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    if (fullLen == 0 || fullLen >= std::size(full)) {
        why = L"bad path";
        return std::nullopt;
    }
    std::wstring path(full, fullLen);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        why = L"path does not exist";
        return std::nullopt;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && IsBlockedOpenExtension(path)) {
        why = L"executables/scripts are not opened; use launch_app";
        return std::nullopt;
    }
    return path;
}

std::vector<std::wstring> GoalKeywords(const std::wstring& request) {
    static constexpr std::wstring_view stop[] = {
        L"open", L"launch", L"start", L"run", L"play", L"find", L"search", L"for", L"go",
        L"to", L"the", L"a", L"an", L"my", L"me", L"on", L"in", L"and", L"then", L"please",
        L"show", L"app", L"application", L"some", L"up", L"with", L"of", L"it", L"i", L"want",
    };
    std::vector<std::wstring> words;
    std::wstring cleaned = ToLowerWide(request);
    for (wchar_t& c : cleaned) {
        if (!IsWordChar(c) && c != L'.') {
            c = L' ';
        }
    }
    for (std::wstring& token : Tokenize(cleaned)) {
        if (!std::ranges::any_of(stop, [&](std::wstring_view s) { return token == s; })) {
            words.push_back(std::move(token));
        }
    }
    return words;
}

std::vector<const LaunchCandidate*> RankForAgent(const std::vector<LaunchCandidate>& candidates,
    const std::wstring& query, size_t limit) {
    std::wstring joined;
    for (const std::wstring& word : GoalKeywords(query)) {
        if (!joined.empty()) {
            joined.push_back(L' ');
        }
        joined += word;
    }
    const std::vector<std::wstring> tokens = Tokenize(joined);
    std::vector<std::pair<double, const LaunchCandidate*>> ranked;
    ranked.reserve(candidates.size());
    for (const LaunchCandidate& candidate : candidates) {
        const double score = CandidateRelevance(candidate, joined, tokens);
        if (score >= 12.0) {
            ranked.emplace_back(score, &candidate);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(),
        [](const auto& left, const auto& right) { return left.first > right.first; });
    std::vector<const LaunchCandidate*> top;
    for (size_t index = 0; index < ranked.size() && top.size() < limit; ++index) {
        top.push_back(ranked[index].second);
    }
    return top;
}

void AppendCompactCandidate(std::string& out, const LaunchCandidate& candidate) {
    out += '{';
    AppendUtf8Field(out, "id", Utf8ToWide(candidate.id));
    out += ',';
    AppendUtf8Field(out, "name", TruncateWide(candidate.name, 40));
    if (!candidate.executable.empty() &&
        ToLowerWide(candidate.executable).find(ToLowerWide(candidate.name)) == std::wstring::npos) {
        out += ',';
        AppendUtf8Field(out, "exe", TruncateWide(candidate.executable, 32));
    }
    if (candidate.running) {
        out += ",\"running\":true";
    }
    out += '}';
}

std::string CandidateListJson(const std::vector<const LaunchCandidate*>& list) {
    std::string out = "[";
    for (size_t index = 0; index < list.size(); ++index) {
        if (index > 0) {
            out += ',';
        }
        AppendCompactCandidate(out, *list[index]);
    }
    out += ']';
    return out;
}

const std::string& AgentSystemPrompt() {
    static const std::string prompt = [] {
        std::string text =
            "Hoverdock Search agent. JSON lines only:\n"
            "{\"tool\":\"launch_app\",\"id\":\"c3\"}\n"
            "{\"tool\":\"open_url\",\"url\":\"https://...\"}\n"
            "{\"tool\":\"open_path\",\"path\":\"C:\\\\...\"}\n"
            "{\"tool\":\"search_apps\",\"q\":\"words\"}\n"
            "{\"tool\":\"done\",\"say\":\"short\"}\n"
            "Use exact ids. open_url opens the browser (do not also launch_app). "
            "Web: youtube.com/results?search_query=... or google.com/search?q=... "
            "End with done unless search_apps.\nfolders:";
        for (const NamedFolder& folder : UserFolders()) {
            text += ' ';
            text += WideToUtf8(folder.label);
            text += '=';
            text += WideToUtf8(folder.path);
            text += ';';
        }
        return text;
    }();
    return prompt;
}

struct ChatMessage {
    const char* role;
    std::string content;
};

std::string BuildAgentBody(const std::vector<ChatMessage>& messages) {
    std::string body = "{\"model\":\"local\",\"temperature\":0,\"max_tokens\":";
    body += std::to_string(kAgentMaxTokens);
    body += ",\"cache_prompt\":true,\"chat_template_kwargs\":{\"enable_thinking\":false},"
            "\"reasoning_format\":\"none\",\"messages\":[{\"role\":\"system\",\"content\":";
    AppendEscaped(body, AgentSystemPrompt());
    body += '}';
    for (const ChatMessage& message : messages) {
        body += ",{\"role\":\"";
        body += message.role;
        body += "\",\"content\":";
        AppendEscaped(body, message.content);
        body += '}';
    }
    body += "]}";
    return body;
}

std::vector<std::string> ExtractJsonObjects(const std::string& text) {
    std::vector<std::string> objects;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const auto object = ExtractJsonObject(text.substr(cursor));
        if (!object.has_value()) {
            break;
        }
        const size_t at = text.find(*object, cursor);
        objects.push_back(*object);
        cursor = (at == std::string::npos ? cursor : at) + object->size();
    }
    return objects;
}

std::string ToolNameOf(const std::string& json) {
    static constexpr std::string_view tools[] = {
        "launch_app", "open_url", "open_path", "search_apps", "done", "say", "reply",
    };
    for (const std::string_view key : {std::string_view("tool"), std::string_view("name"),
             std::string_view("action"), std::string_view("function")}) {
        if (const auto value = ExtractStringField(json, key); value.has_value()) {
            for (const std::string_view tool : tools) {
                if (*value == tool) {
                    return *value;
                }
            }
        }
    }
    // {"launch_app":{"id":"c3"}} style.
    for (const std::string_view tool : tools) {
        if (json.find("\"" + std::string(tool) + "\"") != std::string::npos) {
            return std::string(tool);
        }
    }
    return {};
}

std::wstring FirstField(const std::string& json, std::initializer_list<std::string_view> keys) {
    for (const std::string_view key : keys) {
        if (const auto value = ExtractStringField(json, key); value.has_value() && !value->empty()) {
            return TrimWide(Utf8ToWide(*value));
        }
    }
    return {};
}

const LaunchCandidate* FindCandidate(const std::vector<LaunchCandidate>& candidates, const std::string& id) {
    for (const LaunchCandidate& candidate : candidates) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

std::wstring Sentence(std::wstring text) {
    text = TruncateWide(TrimWide(std::move(text)), kAgentMaxReplyChars);
    return text;
}

}  // namespace

bool LlamaServerClient::IsServerReachable(const std::wstring& baseUrl) {
    const ParsedUrl url = ParseBaseUrl(baseUrl);
    DWORD status = 0;
    std::string response;
    std::wstring error;
    if (!HttpExchange(url, L"GET", L"/health", nullptr, status, response, error)) {
        if (!HttpExchange(url, L"GET", L"/v1/models", nullptr, status, response, error)) {
            return false;
        }
    }
    return status >= 200 && status < 300;
}

LaunchJudgment LlamaServerClient::ResolveAppFuzzy(const std::wstring& request,
    const std::vector<LaunchCandidate>& candidates) {
    LaunchJudgment judgment;
    if (candidates.empty()) {
        judgment.action = LaunchJudgment::Action::None;
        return judgment;
    }
    const std::wstring queryLower = ToLowerWide(TrimWide(request));
    const std::vector<std::wstring> tokens = Tokenize(queryLower);
    double bestScore = -1.0;
    const LaunchCandidate* best = nullptr;
    for (const LaunchCandidate& candidate : candidates) {
        const double score = CandidateRelevance(candidate, queryLower, tokens);
        if (score > bestScore) {
            bestScore = score;
            best = &candidate;
        }
    }
    if (best == nullptr || bestScore < kFuzzyMinScore) {
        judgment.action = LaunchJudgment::Action::None;
        return judgment;
    }
    judgment.action = LaunchJudgment::Action::Launch;
    judgment.chosenId = best->id;
    judgment.exists = 1.0;
    judgment.confidence = std::min(1.0, bestScore / 100.0);
    judgment.usedFuzzyFallback = true;
    return judgment;
}

LaunchJudgment LlamaServerClient::ResolveApp(const std::wstring& baseUrl,
    const std::wstring& request, const std::vector<LaunchCandidate>& candidates) {
    const std::wstring cleanRequest = TruncateWide(TrimWide(request), kMaxRequestChars);
    if (cleanRequest.empty() || candidates.empty()) {
        LaunchJudgment judgment;
        judgment.action = LaunchJudgment::Action::None;
        return judgment;
    }

    // Lexical confident hit: skip the model entirely for clear app names.
    if (const std::string quickId = ConfidentAppId(cleanRequest, candidates); !quickId.empty()) {
        LaunchJudgment judgment;
        judgment.action = LaunchJudgment::Action::Launch;
        judgment.chosenId = quickId;
        judgment.exists = 1.0;
        judgment.confidence = 0.95;
        judgment.usedFuzzyFallback = true;
        return judgment;
    }

    const std::vector<LaunchCandidate> ranked =
        SelectTopCandidates(candidates, cleanRequest, kMaxCandidates);

    // No separate /health round-trip: a failed POST falls back to fuzzy.
    const ParsedUrl url = ParseBaseUrl(baseUrl);
    const std::string body = BuildChatBody(cleanRequest, ranked);
    DWORD status = 0;
    std::string response;
    std::wstring error;
    if (!HttpExchange(url, L"POST", L"/v1/chat/completions", &body, status, response, error)) {
        LaunchJudgment fuzzy = ResolveAppFuzzy(cleanRequest, ranked);
        if (fuzzy.action == LaunchJudgment::Action::Launch) {
            return fuzzy;
        }
        return MakeError(error.empty() ? L"llama-server unreachable; no fuzzy match." : error);
    }
    if (status < 200 || status >= 300) {
        LaunchJudgment fuzzy = ResolveAppFuzzy(cleanRequest, ranked);
        if (fuzzy.action == LaunchJudgment::Action::Launch) {
            return fuzzy;
        }
        return MakeError(L"llama-server HTTP " + std::to_wstring(status));
    }

    LaunchJudgment judgment = ParseModelJudgment(response, ranked);
    if (judgment.action == LaunchJudgment::Action::Error ||
        judgment.action == LaunchJudgment::Action::Uncertain) {
        LaunchJudgment fuzzy = ResolveAppFuzzy(cleanRequest, ranked);
        if (fuzzy.action == LaunchJudgment::Action::Launch) {
            return fuzzy;
        }
    }
    return judgment;
}

// ---------------------------------------------------------------------------
// Agent-in-search
// ---------------------------------------------------------------------------

std::wstring LlamaServerClient::StripGoalVerbs(const std::wstring& request) {
    return StripLeadingPhrases(request);
}

bool LlamaServerClient::LooksLikeAgentGoal(const std::wstring& request) {
    const std::wstring lower = ToLowerWide(TrimWide(request));
    const std::vector<std::wstring> tokens = Tokenize(lower);
    if (tokens.empty()) {
        return false;
    }
    if (lower.find(L"://") != std::wstring::npos ||
        std::ranges::any_of(tokens, [](const std::wstring& token) { return LooksLikeDomain(token); })) {
        return true;
    }
    if (tokens.size() >= 4) {
        return true;
    }
    static constexpr std::wstring_view verbs[] = {
        L"open", L"launch", L"run", L"start", L"play", L"find", L"search", L"go", L"goto",
        L"visit", L"browse", L"navigate", L"show", L"take", L"look", L"google", L"watch",
        L"listen", L"switch", L"bring", L"focus", L"please", L"can", L"could",
    };
    if (tokens.size() >= 2 &&
        std::ranges::any_of(verbs, [&](std::wstring_view verb) { return tokens.front() == verb; })) {
        return true;
    }
    static constexpr std::wstring_view nouns[] = {L"folder", L"directory", L"website", L"site", L"url"};
    return std::ranges::any_of(tokens, [&](const std::wstring& token) {
        return std::ranges::any_of(nouns, [&](std::wstring_view noun) { return token == noun; });
    });
}


std::wstring UrlEncodeQuery(const std::wstring& value) {
    auto appendHex = [](std::wstring& out, unsigned char byte) {
        constexpr wchar_t kHex[] = L"0123456789ABCDEF";
        out.push_back(L'%');
        out.push_back(kHex[(byte >> 4) & 0xf]);
        out.push_back(kHex[byte & 0xf]);
    };
    std::wstring out;
    out.reserve(value.size() * 3);
    for (const wchar_t c : value) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
            c == L'-' || c == L'_' || c == L'.' || c == L'~') {
            out.push_back(c);
        } else if (c == L' ') {
            out.push_back(L'+');
        } else if (c < 0x80) {
            appendHex(out, static_cast<unsigned char>(c));
        } else {
            const std::string utf8 = WideToUtf8(std::wstring(1, c));
            for (const unsigned char byte : utf8) {
                appendHex(out, byte);
            }
        }
    }
    return out;
}

std::optional<SearchAgentAction> WebGoalAction(const std::wstring& request) {
    const std::wstring lower = ToLowerWide(TrimWide(request));
    if (lower.empty()) {
        return std::nullopt;
    }

    auto makeUrl = [](std::wstring url, std::wstring label) -> SearchAgentAction {
        SearchAgentAction action;
        action.kind = SearchAgentAction::Kind::OpenUrl;
        action.target = std::move(url);
        action.label = std::move(label);
        return action;
    };

    // "play/search/watch/find/listen X on/in youtube" or "... youtube for X"
    const std::wstring_view ytMarkers[] = {
        L" on youtube", L" in youtube", L" youtube for ", L" on yt", L" in yt",
    };
    for (const std::wstring_view marker : ytMarkers) {
        const size_t at = lower.find(marker);
        if (at == std::wstring::npos) {
            continue;
        }
        std::wstring query;
        if (marker == L" youtube for ") {
            query = TrimWide(lower.substr(at + marker.size()));
        } else {
            // Strip leading verb from the prefix.
            query = StripLeadingPhrases(lower.substr(0, at));
            // Also drop a trailing "videos"/"music" filler.
            for (const std::wstring_view tail : {std::wstring_view(L" videos"), std::wstring_view(L" music"),
                     std::wstring_view(L" songs")}) {
                if (query.size() > tail.size() && query.ends_with(tail)) {
                    query = TrimWide(query.substr(0, query.size() - tail.size()));
                }
            }
        }
        if (query.empty() || query.size() > 120) {
            continue;
        }
        return makeUrl(L"https://www.youtube.com/results?search_query=" + UrlEncodeQuery(query),
            L"YouTube: " + Utf8ToWide(WideToUtf8(query)));
    }

    // "youtube <query>" / "yt <query>" when first token is youtube/yt
    {
        const std::vector<std::wstring> tokens = Tokenize(lower);
        if (tokens.size() >= 2 && (tokens[0] == L"youtube" || tokens[0] == L"yt")) {
            std::wstring query = TrimWide(lower.substr(tokens[0].size()));
            if (!query.empty() && query.size() <= 120) {
                return makeUrl(L"https://www.youtube.com/results?search_query=" + UrlEncodeQuery(query),
                    L"YouTube: " + query);
            }
        }
    }

    // "google X" / "search google for X" / "search for X on google"
    auto googleQuery = [&]() -> std::wstring {
        if (lower.starts_with(L"google ")) {
            return TrimWide(lower.substr(7));
        }
        for (const std::wstring_view prefix : {std::wstring_view(L"search google for "),
                 std::wstring_view(L"google search for "), std::wstring_view(L"search on google for "),
                 std::wstring_view(L"look up "), std::wstring_view(L"lookup ")}) {
            if (lower.starts_with(prefix)) {
                return TrimWide(lower.substr(prefix.size()));
            }
        }
        const size_t onGoogle = lower.rfind(L" on google");
        if (onGoogle != std::wstring::npos && onGoogle > 0) {
            return StripLeadingPhrases(lower.substr(0, onGoogle));
        }
        // "search for X" / "search X" — only when clearly a web search (no folder/app cues).
        if (lower.starts_with(L"search for ") || lower.starts_with(L"search ")) {
            std::wstring q = lower.starts_with(L"search for ")
                ? TrimWide(lower.substr(11))
                : TrimWide(lower.substr(7));
            if (q.empty() || q.find(L"folder") != std::wstring::npos ||
                q.find(L"app") != std::wstring::npos) {
                return {};
            }
            // Avoid stealing "search photos" style catalog goals with 1 short token.
            const auto words = Tokenize(q);
            if (words.size() >= 2) {
                return q;
            }
        }
        return {};
    };
    if (const std::wstring q = googleQuery(); !q.empty() && q.size() <= 120) {
        return makeUrl(L"https://www.google.com/search?q=" + UrlEncodeQuery(q), L"Google: " + q);
    }

    return std::nullopt;
}

std::wstring NormalizeGoalKey(const std::wstring& request) {
    return ToLowerWide(TrimWide(request));
}

struct LastGoalMemory {
    std::mutex mutex;
    std::wstring key;
    std::vector<SearchAgentAction> actions;
};

LastGoalMemory& GoalMemory() {
    static LastGoalMemory memory;
    return memory;
}

std::optional<SearchAgentAction> LlamaServerClient::DirectAction(const std::wstring& request) {
    const std::wstring trimmed = TrimWide(request);
    if (auto web = WebGoalAction(trimmed); web.has_value()) {
        return web;
    }
    const std::wstring rest = StripLeadingPhrases(trimmed);
    if (rest.empty()) {
        return std::nullopt;
    }
    if (rest.find(L' ') == std::wstring::npos && LooksLikeDomain(rest)) {
        if (const auto url = NormalizeUrl(rest); url.has_value()) {
            SearchAgentAction action;
            action.kind = SearchAgentAction::Kind::OpenUrl;
            action.target = *url;
            action.label = rest;
            return action;
        }
    }
    const std::wstring restLower = ToLowerWide(rest);
    const bool commanded = ToLowerWide(trimmed) != restLower;
    const bool saysFolder = restLower.ends_with(L" folder") || restLower.ends_with(L" directory");
    if (commanded || saysFolder) {
        if (const auto folder = FolderByName(rest); folder.has_value()) {
            SearchAgentAction action;
            action.kind = SearchAgentAction::Kind::OpenPath;
            action.target = *folder;
            action.label = rest;
            return action;
        }
    }
    if (rest.size() >= 3 && rest[1] == L':' && (rest[2] == L'\\' || rest[2] == L'/')) {
        std::wstring why;
        if (const auto path = ValidateOpenPath(rest, why); path.has_value()) {
            SearchAgentAction action;
            action.kind = SearchAgentAction::Kind::OpenPath;
            action.target = *path;
            action.label = *path;
            return action;
        }
    }
    return std::nullopt;
}

std::string LlamaServerClient::ConfidentAppId(const std::wstring& appText,
    const std::vector<LaunchCandidate>& candidates) {
    const std::wstring queryLower = ToLowerWide(TrimWide(appText));
    const std::vector<std::wstring> tokens = Tokenize(queryLower);
    if (tokens.empty() || tokens.size() > 3) {
        return {};
    }
    double best = -1.0;
    double second = -1.0;
    const LaunchCandidate* winner = nullptr;
    for (const LaunchCandidate& candidate : candidates) {
        const double score = CandidateRelevance(candidate, queryLower, tokens);
        if (score > best) {
            second = best;
            best = score;
            winner = &candidate;
        } else if (score > second) {
            second = score;
        }
    }
    if (winner == nullptr || best < kConfidentMinScore || best - second < kConfidentMargin) {
        return {};
    }
    return winner->id;
}

SearchAgentResult LlamaServerClient::RunAgent(const std::wstring& baseUrl,
    const std::wstring& request, const std::vector<LaunchCandidate>& candidates,
    const StatusCallback& status, const CancelCallback& cancelled) {
    SearchAgentResult result;
    const std::wstring goal = TruncateWide(TrimWide(request), kMaxRequestChars);
    if (goal.empty()) {
        result.reply = L"Type an app or a goal.";
        return result;
    }
    const auto notify = [&status](const std::wstring& text) {
        if (status) {
            status(text);
        }
    };
    const auto isCancelled = [&cancelled]() { return cancelled && cancelled(); };

    // Skip /health: first failed completion marks the server unavailable.
    const ParsedUrl url = ParseBaseUrl(baseUrl);
    std::vector<ChatMessage> messages;
    {
        std::string user = "/no_think\ngoal=";
        user += WideToUtf8(goal);
        user += "\napps=";
        user += CandidateListJson(RankForAgent(candidates, goal, kAgentContextApps));
        messages.push_back({"user", std::move(user)});
    }

    for (int step = 1; step <= kAgentMaxSteps; ++step) {
        if (isCancelled()) {
            return result;
        }
        result.steps = step;
        notify(L"Agent working (step " + std::to_wstring(step) + L"/" +
            std::to_wstring(kAgentMaxSteps) + L", local CPU)...");

        const std::string body = BuildAgentBody(messages);
        DWORD httpStatus = 0;
        std::string response;
        std::wstring error;
        const bool exchanged = HttpExchange(url, L"POST", L"/v1/chat/completions", &body, httpStatus,
            response, error, kAgentReceiveTimeoutMs);
        std::optional<std::string> content;
        if (exchanged && httpStatus >= 200 && httpStatus < 300) {
            content = ExtractAssistantContent(response);
        }
        if (!content.has_value()) {
            if (result.actions.empty() && step == 1) {
                // No agent claims: let the caller run classic fuzzy search.
                result.serverUnavailable = true;
                return result;
            }
            result.reply = L"Agent stopped: " +
                (error.empty() ? L"no usable reply (HTTP " + std::to_wstring(httpStatus) + L")" : error);
            return result;
        }
        if (isCancelled()) {
            return result;
        }

        const std::vector<std::string> calls = ExtractJsonObjects(*content);
        if (calls.empty()) {
            std::string prose = *content;
            if (const size_t endThink = prose.find("</think>"); endThink != std::string::npos) {
                prose = prose.substr(endThink + 8);
            }
            result.reply = Sentence(Utf8ToWide(prose));
            if (result.reply.empty()) {
                result.reply = L"The agent had nothing to do for that.";
            }
            return result;
        }

        std::string toolResults;
        std::string assistantEcho;
        bool needsAnotherStep = false;
        for (const std::string& call : calls) {
            const std::string tool = ToolNameOf(call);
            if (tool.empty()) {
                continue;
            }
            assistantEcho += call;
            assistantEcho += '\n';
            if (tool == "search_apps") {
                std::wstring query = FirstField(call, {"q", "query", "text", "name"});
                if (query.empty() || query == L"search_apps") {
                    query = goal;
                }
                notify(L"Searching apps: " + TruncateWide(query, 60));
                toolResults += "search_apps \"" + WideToUtf8(TruncateWide(query, 60)) + "\" -> " +
                    CandidateListJson(RankForAgent(candidates, query, kAgentSearchResults)) + "\n";
                needsAnotherStep = true;
            } else if (tool == "launch_app") {
                if (result.actions.size() >= kAgentMaxActions) {
                    toolResults += "launch_app: action limit reached\n";
                    continue;
                }
                const std::wstring idText = FirstField(call, {"id", "app_id", "app", "target"});
                const LaunchCandidate* candidate = FindCandidate(candidates, WideToUtf8(idText));
                if (candidate == nullptr && !idText.empty()) {
                    // Tolerate {"id":"Steam"}: map a name to a confident catalog hit.
                    candidate = FindCandidate(candidates, ConfidentAppId(idText, candidates));
                }
                if (candidate == nullptr) {
                    toolResults += "launch_app \"" + WideToUtf8(TruncateWide(idText, 40)) +
                        "\": unknown id; use an id from apps or search_apps\n";
                    needsAnotherStep = true;
                    continue;
                }
                const bool duplicate = std::ranges::any_of(result.actions, [&](const SearchAgentAction& a) {
                    return a.kind == SearchAgentAction::Kind::LaunchApp && a.appId == candidate->id;
                });
                if (!duplicate) {
                    SearchAgentAction action;
                    action.kind = SearchAgentAction::Kind::LaunchApp;
                    action.appId = candidate->id;
                    action.label = candidate->name;
                    result.actions.push_back(std::move(action));
                }
                notify(L"Launching " + candidate->name + L"...");
                toolResults += "launch_app " + candidate->id + ": ok (" +
                    WideToUtf8(TruncateWide(candidate->name, 40)) + ")\n";
            } else if (tool == "open_url") {
                if (result.actions.size() >= kAgentMaxActions) {
                    toolResults += "open_url: action limit reached\n";
                    continue;
                }
                const auto target = NormalizeUrl(FirstField(call, {"url", "href", "link", "target"}));
                if (!target.has_value()) {
                    toolResults += "open_url: rejected (only http/https URLs)\n";
                    needsAnotherStep = true;
                    continue;
                }
                SearchAgentAction action;
                action.kind = SearchAgentAction::Kind::OpenUrl;
                action.target = *target;
                action.label = TruncateWide(*target, 60);
                result.actions.push_back(std::move(action));
                notify(L"Opening " + TruncateWide(*target, 60));
                toolResults += "open_url: ok\n";
            } else if (tool == "open_path") {
                if (result.actions.size() >= kAgentMaxActions) {
                    toolResults += "open_path: action limit reached\n";
                    continue;
                }
                std::wstring why;
                const auto target = ValidateOpenPath(FirstField(call, {"path", "folder", "file", "target"}), why);
                if (!target.has_value()) {
                    toolResults += "open_path: rejected (" + WideToUtf8(why) + ")\n";
                    needsAnotherStep = true;
                    continue;
                }
                SearchAgentAction action;
                action.kind = SearchAgentAction::Kind::OpenPath;
                action.target = *target;
                action.label = *target;
                result.actions.push_back(std::move(action));
                notify(L"Opening " + TruncateWide(*target, 60));
                toolResults += "open_path: ok\n";
            } else {  // done / say / reply
                const std::wstring say = FirstField(call, {"say", "text", "reply", "message"});
                if (!say.empty()) {
                    result.reply = Sentence(say);
                    notify(result.reply);
                }
            }
        }

        if (!needsAnotherStep) {
            // done, or actions/say without an explicit done: nothing left to learn.
            return result;
        }
        if (step == kAgentMaxSteps || isCancelled()) {
            break;
        }
        messages.push_back({"assistant", assistantEcho});
        messages.push_back({"user", "/no_think\nresults:\n" + toolResults + "Continue. End with done."});
    }
    if (result.actions.empty() && result.reply.empty()) {
        result.reply = L"The agent could not finish that. Try naming the app.";
    }
    return result;
}


bool LlamaServerClient::IsSafeShellOpenTarget(bool isUrl, const std::wstring& target) {
    if (target.empty()) {
        return false;
    }
    if (isUrl) {
        return NormalizeUrl(target).has_value();
    }
    std::wstring why;
    return ValidateOpenPath(target, why).has_value();
}

std::optional<SearchAgentResult> LlamaServerClient::TryReplayLastGoal(const std::wstring& request) {
    const std::wstring key = NormalizeGoalKey(request);
    if (key.empty()) {
        return std::nullopt;
    }
    LastGoalMemory& memory = GoalMemory();
    std::lock_guard<std::mutex> lock(memory.mutex);
    if (memory.key != key || memory.actions.empty()) {
        return std::nullopt;
    }
    SearchAgentResult result;
    result.actions = memory.actions;
    result.reply = L"Replaying last goal.";
    result.steps = 0;
    return result;
}

void LlamaServerClient::RememberSuccessfulGoal(const std::wstring& request,
    const std::vector<SearchAgentAction>& actions) {
    if (actions.empty()) {
        return;
    }
    const std::wstring key = NormalizeGoalKey(request);
    if (key.empty()) {
        return;
    }
    LastGoalMemory& memory = GoalMemory();
    std::lock_guard<std::mutex> lock(memory.mutex);
    memory.key = key;
    memory.actions = actions;
}

void LlamaServerClient::RememberSuccessfulLaunch(const std::wstring& request, const std::string& appId,
    const std::wstring& label) {
    if (appId.empty()) {
        return;
    }
    SearchAgentAction action;
    action.kind = SearchAgentAction::Kind::LaunchApp;
    action.appId = appId;
    action.label = label;
    RememberSuccessfulGoal(request, {action});
}
