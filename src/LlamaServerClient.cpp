#include "LlamaServerClient.h"

#include <Windows.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
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
constexpr size_t kMaxCandidates = 8;
constexpr size_t kMaxRequestChars = 200;
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

// ---------------------------------------------------------------------------
// Agent-in-search helpers
// ---------------------------------------------------------------------------

// CPU-only 27B: ~6-8 tok/s prompt eval and ~1.3 tok/s generation (~0.77 s per
// output token). Every output token is the dominant cost, so the model speaks
// a one-line compact protocol ("L c3", "W rtx 5090 price") constrained by a
// GBNF grammar instead of JSON tool calls, and the static system prompt is
// shared by ranking + agent requests so the single llama-server slot keeps it
// cached (and WarmPromptCache primes it when Search opens).
constexpr int kAgentMaxRounds = 2;
constexpr size_t kAgentContextApps = 5;
constexpr size_t kAgentSearchResults = 5;
constexpr int kAgentMaxTokens = 40;
constexpr int kRankMaxTokens = 8;
constexpr DWORD kAgentReceiveTimeoutMs = 120000;
constexpr size_t kAgentMaxReplyChars = 120;
constexpr size_t kAgentMaxUrlChars = 2048;
constexpr size_t kMaxQueryChars = 120;
constexpr double kConfidentMinScore = 80.0;
constexpr double kConfidentMargin = 30.0;

// One line, no newline: the grammar ends the reply after a single action so
// generation stops at EOS instead of running to max_tokens.
constexpr char kAgentGrammar[] =
    "root ::= \"L c\" [0-9]{1,4} | \"W \" q | \"Y \" q | \"U http\" \"s\"? \"://\" u | \"P \" p | \"S \" q | \"N\" (\" \" q)?\n"
    "q ::= [^\\n]{1,80}\n"
    "u ::= [^\\n \"<>\\\\]{1,300}\n"
    "p ::= [^\\n\"<>|?*]{1,200}\n";
// Second round (after S) and plain app ranking: pick a listed app or nothing.
constexpr char kLaunchOnlyGrammar[] = "root ::= \"L c\" [0-9]{1,4} | \"N\"\n";

std::atomic<bool> g_promptWarm{false};
std::atomic<bool> g_warmInFlight{false};
std::atomic<ULONGLONG> g_lastWarmAttempt{0};
std::atomic<bool> g_grammarRejected{false};

bool IsWordChar(wchar_t c) {
    return std::iswalnum(c) != 0 || c == L'_' || c == L'-';
}

bool StartsWithAny(const std::wstring& text, std::initializer_list<std::wstring_view> prefixes) {
    return std::ranges::any_of(prefixes, [&](std::wstring_view p) { return text.starts_with(p); });
}

std::wstring StripPolitePrefix(std::wstring lower) {
    static constexpr std::wstring_view polite[] = {
        L"hey ", L"ok ", L"okay ", L"please ", L"pls ", L"plz ", L"can you ", L"could you ",
        L"would you ", L"will you ", L"just ", L"quickly ",
    };
    lower = TrimWide(std::move(lower));
    bool stripped = true;
    while (stripped && !lower.empty()) {
        stripped = false;
        for (const std::wstring_view prefix : polite) {
            if (lower.starts_with(prefix)) {
                lower = TrimWide(lower.substr(prefix.size()));
                stripped = true;
                break;
            }
        }
    }
    return lower;
}

std::wstring StripLeadingPhrases(std::wstring text) {
    // Longest phrases first so "show me " wins over "show ".
    static constexpr std::wstring_view prefixes[] = {
        L"please ", L"pls ", L"plz ", L"hey ", L"can you ", L"could you ", L"would you ",
        L"will you ", L"i want to ", L"i'd like to ", L"i need to ", L"i wanna ", L"let me ",
        L"let's ", L"lets ", L"just ", L"quickly ", L"take me to ", L"get me to ", L"navigate to ",
        L"bring up ", L"pull up ", L"fire up ", L"boot up ", L"switch to ", L"browse to ",
        L"jump to ", L"head to ", L"show me ", L"go to ", L"goto ", L"open up ", L"open ",
        L"launch ", L"start up ", L"start ", L"run ", L"visit ", L"browse ", L"show ", L"focus ",
        L"load ",
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
    for (const std::wstring_view article : {std::wstring_view(L"the "), std::wstring_view(L"my "),
             std::wstring_view(L"a "), std::wstring_view(L"an ")}) {
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

// "steam app please" -> "steam". Never strips to empty.
std::wstring StripTrailingFillers(std::wstring text) {
    static constexpr std::wstring_view tails[] = {
        L" for me", L" please", L" pls", L" now", L" app", L" application", L" program",
        L" again", L" window", L" up",
    };
    bool stripped = true;
    while (stripped) {
        stripped = false;
        const std::wstring lower = ToLowerWide(text);
        for (const std::wstring_view tail : tails) {
            if (lower.size() > tail.size() && lower.ends_with(tail)) {
                text = TrimWide(text.substr(0, text.size() - tail.size()));
                stripped = true;
                break;
            }
        }
    }
    return text;
}

std::wstring CleanAppText(const std::wstring& text) {
    return ToLowerWide(StripTrailingFillers(StripLeadingPhrases(text)));
}

// Lowercase alphanumerics (+ '+' and '#'): "VS Code" == "vscode", "Notepad++".
std::wstring NormalizeKey(const std::wstring& text) {
    std::wstring key;
    key.reserve(text.size());
    for (const wchar_t c : text) {
        if (std::iswalnum(c) != 0 || c == L'+' || c == L'#') {
            key.push_back(static_cast<wchar_t>(std::towlower(c)));
        }
    }
    return key;
}

std::wstring ExeStem(const std::wstring& exe) {
    std::wstring lower = ToLowerWide(exe);
    const size_t sep = lower.find_last_of(L"\\/");
    if (sep != std::wstring::npos) {
        lower = lower.substr(sep + 1);
    }
    if (lower.ends_with(L".exe")) {
        lower.resize(lower.size() - 4);
    }
    return lower;
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
        L"au", L"ly", L"to", L"fm", L"sh", L"page", L"site", L"wiki", L"ie", L"nl", L"se",
        L"no", L"jp", L"in", L"br", L"it", L"es", L"ch", L"at", L"be", L"pl", L"nz", L"biz",
        L"cc", L"gl", L"link", L"top", L"tech", L"store", L"news", L"blog", L"online",
        L"live", L"games", L"cloud", L"art", L"club", L"chat", L"lol", L"dk", L"fi", L"pt",
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

std::wstring UrlEncodeQuery(const std::wstring& value, bool spaceAsPercent = false) {
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
            if (spaceAsPercent) {
                out += L"%20";
            } else {
                out.push_back(L'+');
            }
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

SearchAgentAction MakeUrlAction(std::wstring url, std::wstring label) {
    SearchAgentAction action;
    action.kind = SearchAgentAction::Kind::OpenUrl;
    action.target = std::move(url);
    action.label = std::move(label);
    return action;
}

SearchAgentAction MakePathAction(std::wstring path, std::wstring label) {
    SearchAgentAction action;
    action.kind = SearchAgentAction::Kind::OpenPath;
    action.target = std::move(path);
    action.label = std::move(label);
    return action;
}

SearchAgentAction MakeLaunchAction(const LaunchCandidate& candidate) {
    SearchAgentAction action;
    action.kind = SearchAgentAction::Kind::LaunchApp;
    action.appId = candidate.id;
    action.label = candidate.name;
    return action;
}

SearchAgentAction GoogleSearch(const std::wstring& query) {
    return MakeUrlAction(L"https://www.google.com/search?q=" + UrlEncodeQuery(query), L"Google: " + query);
}

SearchAgentAction YouTubeSearch(const std::wstring& query) {
    return MakeUrlAction(L"https://www.youtube.com/results?search_query=" + UrlEncodeQuery(query),
        L"YouTube: " + query);
}

// ---- Site catalog ---------------------------------------------------------
// keys are '|'-separated lowercase names; longest key wins. search is a URL
// prefix the encoded query is appended to (empty = home only).
struct SiteInfo {
    std::wstring_view keys;
    std::wstring_view label;
    std::wstring_view home;
    std::wstring_view search;
    bool pathQuery = false;  // query lives in the path: encode spaces as %20
};

constexpr SiteInfo kSites[] = {
    {L"youtube|yt|you tube", L"YouTube", L"https://www.youtube.com", L"https://www.youtube.com/results?search_query="},
    {L"youtube music|yt music", L"YouTube Music", L"https://music.youtube.com", L"https://music.youtube.com/search?q="},
    {L"google", L"Google", L"https://www.google.com", L"https://www.google.com/search?q="},
    {L"google images|images", L"Google Images", L"https://images.google.com", L"https://www.google.com/search?tbm=isch&q="},
    {L"google news", L"Google News", L"https://news.google.com", L"https://news.google.com/search?q="},
    {L"google maps|maps|map", L"Google Maps", L"https://www.google.com/maps", L"https://www.google.com/maps/search/", true},
    {L"google translate|translate", L"Google Translate", L"https://translate.google.com", L"https://translate.google.com/?sl=auto&tl=en&text="},
    {L"gmail|google mail|email|mail", L"Gmail", L"https://mail.google.com", L"https://mail.google.com/mail/u/0/#search/", true},
    {L"google drive|drive", L"Google Drive", L"https://drive.google.com", L"https://drive.google.com/drive/search?q="},
    {L"google docs", L"Google Docs", L"https://docs.google.com/document", L""},
    {L"google sheets|sheets", L"Google Sheets", L"https://docs.google.com/spreadsheets", L""},
    {L"google calendar|calendar", L"Google Calendar", L"https://calendar.google.com", L""},
    {L"google photos", L"Google Photos", L"https://photos.google.com", L"https://photos.google.com/search/", true},
    {L"gemini", L"Gemini", L"https://gemini.google.com", L""},
    {L"reddit", L"Reddit", L"https://www.reddit.com", L"https://www.reddit.com/search/?q="},
    {L"github|git hub", L"GitHub", L"https://github.com", L"https://github.com/search?q="},
    {L"stack overflow|stackoverflow", L"Stack Overflow", L"https://stackoverflow.com", L"https://stackoverflow.com/search?q="},
    {L"wikipedia|wiki", L"Wikipedia", L"https://en.wikipedia.org", L"https://en.wikipedia.org/w/index.php?search="},
    {L"amazon", L"Amazon", L"https://www.amazon.co.uk", L"https://www.amazon.co.uk/s?k="},
    {L"ebay", L"eBay", L"https://www.ebay.co.uk", L"https://www.ebay.co.uk/sch/i.html?_nkw="},
    {L"twitter|x.com", L"X", L"https://x.com", L"https://x.com/search?q="},
    {L"twitch", L"Twitch", L"https://www.twitch.tv", L"https://www.twitch.tv/search?term="},
    {L"netflix", L"Netflix", L"https://www.netflix.com", L"https://www.netflix.com/search?q="},
    {L"spotify", L"Spotify", L"https://open.spotify.com", L"https://open.spotify.com/search/", true},
    {L"soundcloud", L"SoundCloud", L"https://soundcloud.com", L"https://soundcloud.com/search?q="},
    {L"instagram|insta", L"Instagram", L"https://www.instagram.com", L""},
    {L"facebook|fb", L"Facebook", L"https://www.facebook.com", L"https://www.facebook.com/search/top?q="},
    {L"linkedin", L"LinkedIn", L"https://www.linkedin.com", L"https://www.linkedin.com/search/results/all/?keywords="},
    {L"tiktok", L"TikTok", L"https://www.tiktok.com", L"https://www.tiktok.com/search?q="},
    {L"pinterest", L"Pinterest", L"https://www.pinterest.com", L"https://www.pinterest.com/search/pins/?q="},
    {L"imdb", L"IMDb", L"https://www.imdb.com", L"https://www.imdb.com/find/?q="},
    {L"bbc|bbc news", L"BBC", L"https://www.bbc.co.uk/news", L"https://www.bbc.co.uk/search?q="},
    {L"bbc weather|weather forecast", L"BBC Weather", L"https://www.bbc.co.uk/weather", L""},
    {L"hacker news|hackernews|hn", L"Hacker News", L"https://news.ycombinator.com", L"https://hn.algolia.com/?q="},
    {L"duckduckgo|ddg", L"DuckDuckGo", L"https://duckduckgo.com", L"https://duckduckgo.com/?q="},
    {L"bing", L"Bing", L"https://www.bing.com", L"https://www.bing.com/search?q="},
    {L"chatgpt|chat gpt|openai", L"ChatGPT", L"https://chatgpt.com", L""},
    {L"claude", L"Claude", L"https://claude.ai", L""},
    {L"grok", L"Grok", L"https://grok.com", L""},
    {L"perplexity", L"Perplexity", L"https://www.perplexity.ai", L"https://www.perplexity.ai/search?q="},
    {L"whatsapp web", L"WhatsApp Web", L"https://web.whatsapp.com", L""},
    {L"outlook web|outlook.com|hotmail", L"Outlook", L"https://outlook.live.com", L""},
    {L"office|microsoft 365|m365", L"Microsoft 365", L"https://www.office.com", L""},
    {L"notion", L"Notion", L"https://www.notion.so", L""},
    {L"figma", L"Figma", L"https://www.figma.com", L""},
    {L"canva", L"Canva", L"https://www.canva.com", L""},
    {L"trello", L"Trello", L"https://trello.com", L""},
    {L"dropbox", L"Dropbox", L"https://www.dropbox.com", L""},
    {L"discord web", L"Discord", L"https://discord.com/app", L""},
    {L"steam store", L"Steam Store", L"https://store.steampowered.com", L"https://store.steampowered.com/search/?term="},
    {L"epic store|epic games store", L"Epic Games Store", L"https://store.epicgames.com", L""},
    {L"steamdb", L"SteamDB", L"https://steamdb.info", L"https://steamdb.info/search/?q="},
    {L"protondb", L"ProtonDB", L"https://www.protondb.com", L"https://www.protondb.com/search?q="},
    {L"speedtest|speed test", L"Speedtest", L"https://www.speedtest.net", L""},
    {L"news", L"Google News", L"https://news.google.com", L"https://news.google.com/search?q="},
};

struct SiteHit {
    const SiteInfo* site = nullptr;
    size_t keyLength = 0;
};

// Site whose key equals `text` exactly (after an optional "the ").
const SiteInfo* SiteByName(std::wstring text) {
    text = TrimWide(std::move(text));
    if (text.starts_with(L"the ")) {
        text = TrimWide(text.substr(4));
    }
    for (const std::wstring_view tail : {std::wstring_view(L" website"), std::wstring_view(L" site"),
             std::wstring_view(L" homepage"), std::wstring_view(L" home page"), std::wstring_view(L" web")}) {
        if (text.size() > tail.size() && text.ends_with(tail)) {
            const std::wstring base = TrimWide(text.substr(0, text.size() - tail.size()));
            // "whatsapp web" / "outlook web" are keys themselves; only strip when the
            // base is a site too.
            bool baseIsSite = false;
            for (const SiteInfo& site : kSites) {
                size_t start = 0;
                while (start <= site.keys.size()) {
                    size_t end = site.keys.find(L'|', start);
                    if (end == std::wstring_view::npos) {
                        end = site.keys.size();
                    }
                    if (site.keys.substr(start, end - start) == base) {
                        baseIsSite = true;
                    }
                    start = end + 1;
                }
            }
            if (baseIsSite) {
                text = base;
            }
            break;
        }
    }
    for (const SiteInfo& site : kSites) {
        size_t start = 0;
        while (start <= site.keys.size()) {
            size_t end = site.keys.find(L'|', start);
            if (end == std::wstring_view::npos) {
                end = site.keys.size();
            }
            if (site.keys.substr(start, end - start) == text) {
                return &site;
            }
            start = end + 1;
        }
    }
    return nullptr;
}

// Longest site key that `text` starts with, followed by a space.
SiteHit SitePrefix(const std::wstring& text) {
    SiteHit best;
    for (const SiteInfo& site : kSites) {
        size_t start = 0;
        while (start <= site.keys.size()) {
            size_t end = site.keys.find(L'|', start);
            if (end == std::wstring_view::npos) {
                end = site.keys.size();
            }
            const std::wstring_view key = site.keys.substr(start, end - start);
            if (!key.empty() && text.size() > key.size() + 1 && text.starts_with(key) &&
                text[key.size()] == L' ' && key.size() > best.keyLength) {
                best.site = &site;
                best.keyLength = key.size();
            }
            start = end + 1;
        }
    }
    return best;
}

std::wstring StripQueryVerbs(std::wstring query) {
    static constexpr std::wstring_view verbs[] = {
        L"search for ", L"search ", L"find me ", L"find ", L"look up ", L"lookup ", L"look for ",
        L"play me ", L"play ", L"watch ", L"listen to ", L"show me ", L"show ", L"open ", L"go to ",
        L"get ", L"buy ", L"shop for ", L"check ", L"browse ", L"put on ", L"stream ",
    };
    query = StripPolitePrefix(std::move(query));
    for (const std::wstring_view verb : verbs) {
        if (query.starts_with(verb)) {
            query = TrimWide(query.substr(verb.size()));
            break;
        }
    }
    for (const std::wstring_view article : {std::wstring_view(L"some "), std::wstring_view(L"the ")}) {
        if (query.starts_with(article) && query.size() > article.size() + 2) {
            query = TrimWide(query.substr(article.size()));
            break;
        }
    }
    while (!query.empty() && (query.back() == L'.' || query.back() == L'!' || query.back() == L'?')) {
        query.pop_back();
    }
    return TrimWide(std::move(query));
}

std::optional<SearchAgentAction> SiteSearch(const SiteInfo& site, std::wstring query) {
    query = TrimWide(std::move(query));
    if (query.empty()) {
        return MakeUrlAction(std::wstring(site.home), std::wstring(site.label));
    }
    if (site.search.empty() || query.size() > kMaxQueryChars) {
        return std::nullopt;
    }
    return MakeUrlAction(std::wstring(site.search) + UrlEncodeQuery(query, site.pathQuery),
        std::wstring(site.label) + L": " + query);
}

// Unambiguous web phrasing; safe to run before the app catalog.
//   "play lofi on youtube", "cats on reddit", "search amazon for usb c cable",
//   "google rtx 5090", "youtube lofi", "search for rtx 5090 price",
//   "images of red pandas", "directions to kings cross"
std::optional<SearchAgentAction> ExplicitWebAction(const std::wstring& request) {
    const std::wstring lower = StripPolitePrefix(ToLowerWide(TrimWide(request)));
    if (lower.empty()) {
        return std::nullopt;
    }

    // "<query> on|in|at|using|via|from <site>"
    for (const std::wstring_view joiner : {std::wstring_view(L" on "), std::wstring_view(L" in "),
             std::wstring_view(L" at "), std::wstring_view(L" using "), std::wstring_view(L" via "),
             std::wstring_view(L" from ")}) {
        const size_t at = lower.rfind(joiner);
        if (at == std::wstring::npos || at == 0) {
            continue;
        }
        const SiteInfo* site = SiteByName(lower.substr(at + joiner.size()));
        if (site == nullptr || site->search.empty()) {
            continue;
        }
        // "news in london" / "map in ..." are too ambiguous as site names after a joiner.
        if (site->keys == L"news" || site->keys.starts_with(L"google maps")) {
            if (joiner != L" on ") {
                continue;
            }
        }
        std::wstring query = StripQueryVerbs(lower.substr(0, at));
        if (site->keys.starts_with(L"youtube") && query.size() > 7 && query.ends_with(L" videos")) {
            query = TrimWide(query.substr(0, query.size() - 7));
        }
        // "what's on netflix" / "something on youtube": just open the site.
        if (query == L"what's" || query == L"whats" || query == L"what" || query == L"something" ||
            query == L"anything" || query == L"stuff" || query == L"me") {
            query.clear();
        }
        if (query.empty()) {
            return MakeUrlAction(std::wstring(site->home), std::wstring(site->label));
        }
        if (auto action = SiteSearch(*site, query); action.has_value()) {
            return action;
        }
    }

    // "search|find|look up|check <site> for <query>"
    for (const std::wstring_view verb : {std::wstring_view(L"search "), std::wstring_view(L"find on "),
             std::wstring_view(L"look up on "), std::wstring_view(L"check ")}) {
        if (!lower.starts_with(verb)) {
            continue;
        }
        const std::wstring rest = lower.substr(verb.size());
        const size_t forAt = rest.find(L" for ");
        if (forAt == std::wstring::npos) {
            continue;
        }
        if (const SiteInfo* site = SiteByName(rest.substr(0, forAt)); site != nullptr && !site->search.empty()) {
            if (auto action = SiteSearch(*site, rest.substr(forAt + 5)); action.has_value()) {
                return action;
            }
        }
    }

    // "<site> search <query>" / "youtube <query>" / "google <query>" / "yt <query>"
    if (const SiteHit hit = SitePrefix(lower);
        hit.site != nullptr && !hit.site->search.empty() && SiteByName(lower) == nullptr) {
        std::wstring query = TrimWide(lower.substr(hit.keyLength));
        const bool saysSearch = query.starts_with(L"search ") || query.starts_with(L"for ");
        if (saysSearch) {
            query = TrimWide(query.substr(query.find(L' ') + 1));
            if (query.starts_with(L"for ")) {
                query = TrimWide(query.substr(4));
            }
        }
        const std::wstring_view keys = hit.site->keys;
        const bool searchEngine = keys.starts_with(L"youtube|") || keys == L"google" ||
            keys.starts_with(L"duckduckgo") || keys == L"bing";
        // "google earth" / "youtube lofi" without "search": defer single words until
        // after the app catalog (SiteAction still catches them if no app matches).
        if ((saysSearch || (searchEngine && Tokenize(query).size() >= 2)) && !query.empty()) {
            if (auto action = SiteSearch(*hit.site, query); action.has_value()) {
                return action;
            }
        }
    }

    // "search for X" / "search X" / "look up X" / "google search for X" -> Google
    for (const std::wstring_view prefix : {std::wstring_view(L"google search for "),
             std::wstring_view(L"search google for "), std::wstring_view(L"search on google for "),
             std::wstring_view(L"search the web for "), std::wstring_view(L"search online for "),
             std::wstring_view(L"web search "), std::wstring_view(L"look up "), std::wstring_view(L"lookup "),
             std::wstring_view(L"search for "), std::wstring_view(L"search ")}) {
        if (!lower.starts_with(prefix)) {
            continue;
        }
        const std::wstring q = TrimWide(lower.substr(prefix.size()));
        // Leave "search for photo editor app" / "search downloads folder" to the catalog.
        if (q.empty() || q.size() > kMaxQueryChars || q.find(L"folder") != std::wstring::npos ||
            q.ends_with(L" app") || q.ends_with(L" apps")) {
            break;
        }
        const bool generic = prefix != L"search for " && prefix != L"search ";
        if (generic || Tokenize(q).size() >= 2) {
            return GoogleSearch(q);
        }
        break;
    }

    // Images / directions / maps phrasing.
    for (const std::wstring_view prefix : {std::wstring_view(L"images of "), std::wstring_view(L"pictures of "),
             std::wstring_view(L"photos of "), std::wstring_view(L"pics of "), std::wstring_view(L"show me pictures of "),
             std::wstring_view(L"show me images of ")}) {
        if (lower.starts_with(prefix) && lower.size() > prefix.size()) {
            const std::wstring q = TrimWide(lower.substr(prefix.size()));
            return MakeUrlAction(L"https://www.google.com/search?tbm=isch&q=" + UrlEncodeQuery(q),
                L"Images: " + q);
        }
    }
    for (const std::wstring_view prefix : {std::wstring_view(L"directions to "), std::wstring_view(L"navigate me to "),
             std::wstring_view(L"route to "), std::wstring_view(L"how do i get to "), std::wstring_view(L"how to get to ")}) {
        if (lower.starts_with(prefix) && lower.size() > prefix.size()) {
            const std::wstring q = TrimWide(lower.substr(prefix.size()));
            return MakeUrlAction(
                L"https://www.google.com/maps/dir/?api=1&destination=" + UrlEncodeQuery(q), L"Directions: " + q);
        }
    }
    for (const std::wstring_view prefix : {std::wstring_view(L"map of "), std::wstring_view(L"where is ")}) {
        if (lower.starts_with(prefix) && lower.size() > prefix.size()) {
            const std::wstring q = TrimWide(lower.substr(prefix.size()));
            return MakeUrlAction(L"https://www.google.com/maps/search/" + UrlEncodeQuery(q, true), L"Maps: " + q);
        }
    }
    return std::nullopt;
}

// Site homes ("open reddit", "gmail") and "<site> <query>" for non-engine
// sites ("amazon usb c cable"). Runs after the app catalog so an installed
// app (Spotify, Discord, a YouTube PWA) wins over the website.
std::optional<SearchAgentAction> SiteAction(const std::wstring& request) {
    const std::wstring rest = ToLowerWide(StripTrailingFillers(StripLeadingPhrases(request)));
    if (rest.empty()) {
        return std::nullopt;
    }
    if (const SiteInfo* site = SiteByName(rest); site != nullptr) {
        return MakeUrlAction(std::wstring(site->home), std::wstring(site->label));
    }
    if (const SiteHit hit = SitePrefix(rest); hit.site != nullptr && !hit.site->search.empty()) {
        const std::wstring query = TrimWide(rest.substr(hit.keyLength));
        // Avoid "maps folder"/"mail app" style noise.
        if (!query.empty() && query != L"folder" && query != L"app" && query != L"website") {
            return SiteSearch(*hit.site, query);
        }
    }
    return std::nullopt;
}

// Questions and lookups the catalog cannot answer -> Google.
std::optional<SearchAgentAction> QuestionAction(const std::wstring& request) {
    std::wstring lower = StripPolitePrefix(ToLowerWide(TrimWide(request)));
    if (Tokenize(lower).size() < 2 || lower.size() > kMaxQueryChars) {
        return std::nullopt;
    }
    static constexpr std::wstring_view starts[] = {
        L"what ", L"what's ", L"whats ", L"who ", L"who's ", L"whos ", L"how ", L"why ", L"when ",
        L"where ", L"which ", L"is ", L"are ", L"does ", L"did ", L"will ", L"should ",
        L"define ", L"definition of ", L"meaning of ", L"weather ", L"time in ", L"convert ",
        L"translate ", L"news about ", L"latest news ", L"price of ", L"score ", L"lyrics ",
        L"recipe ", L"recipes ",
    };
    if (!std::ranges::any_of(starts, [&](std::wstring_view s) { return lower.starts_with(s); })) {
        return std::nullopt;
    }
    while (!lower.empty() && (lower.back() == L'?' || lower.back() == L'.' || lower.back() == L'!')) {
        lower.pop_back();
    }
    return GoogleSearch(TrimWide(lower));
}

// ---- Folders --------------------------------------------------------------

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

bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
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
            if (!path.empty() && IsDirectory(path)) {
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
        add(L"Screenshots", FOLDERID_Screenshots);
        add(L"OneDrive", FOLDERID_SkyDrive);
        add(L"AppData", FOLDERID_RoamingAppData);
        add(L"LocalAppData", FOLDERID_LocalAppData);
        add(L"SavedGames", FOLDERID_SavedGames);
        add(L"CameraRoll", FOLDERID_CameraRoll);
        add(L"ProgramFiles", FOLDERID_ProgramFiles);
        add(L"ProgramFilesX86", FOLDERID_ProgramFilesX86);
        add(L"Startup", FOLDERID_Startup);
        add(L"Recent", FOLDERID_Recent);
        add(L"Public", FOLDERID_Public);
        add(L"Fonts", FOLDERID_Fonts);
        wchar_t temp[MAX_PATH + 1] = {};
        const DWORD tempLen = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
        if (tempLen > 0 && tempLen < std::size(temp)) {
            std::wstring path(temp, tempLen);
            while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
                path.pop_back();
            }
            if (IsDirectory(path)) {
                list.push_back({L"Temp", std::move(path)});
            }
        }
        return list;
    }();
    return folders;
}

std::wstring StripFolderWords(std::wstring name) {
    name = ToLowerWide(TrimWide(std::move(name)));
    for (const std::wstring_view tail : {std::wstring_view(L" in file explorer"), std::wstring_view(L" in explorer"),
             std::wstring_view(L" in files")}) {
        if (name.size() > tail.size() && name.ends_with(tail)) {
            name = TrimWide(name.substr(0, name.size() - tail.size()));
            break;
        }
    }
    for (const std::wstring_view suffix : {std::wstring_view(L" folders"), std::wstring_view(L" folder"),
             std::wstring_view(L" directory"), std::wstring_view(L" dir")}) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            name = TrimWide(name.substr(0, name.size() - suffix.size()));
            break;
        }
    }
    for (const std::wstring_view article : {std::wstring_view(L"the "), std::wstring_view(L"my ")}) {
        if (name.starts_with(article)) {
            name = TrimWide(name.substr(article.size()));
            break;
        }
    }
    return name;
}

// "c drive", "drive d", "d:", "d:\" -> "D:\" (local fixed/removable only).
std::optional<std::wstring> DriveByName(const std::wstring& lower) {
    wchar_t letter = 0;
    if (lower.size() == 2 && lower[1] == L':') {
        letter = lower[0];
    } else if (lower.size() == 3 && lower[1] == L':' && (lower[2] == L'\\' || lower[2] == L'/')) {
        letter = lower[0];
    } else if (lower.size() == 7 && lower.ends_with(L" drive")) {
        letter = lower[0];
    } else if (lower.size() == 7 && lower.starts_with(L"drive ")) {
        letter = lower[6];
    } else if (lower.size() == 8 && lower.ends_with(L": drive")) {
        letter = lower[0];
    }
    if (letter < L'a' || letter > L'z') {
        return std::nullopt;
    }
    std::wstring root;
    root.push_back(static_cast<wchar_t>(std::towupper(letter)));
    root += L":\\";
    const UINT type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_CDROM && type != DRIVE_RAMDISK) {
        return std::nullopt;
    }
    return root;
}

std::optional<std::wstring> FolderByName(std::wstring name) {
    name = StripFolderWords(std::move(name));
    if (name.empty()) {
        return std::nullopt;
    }
    if (auto drive = DriveByName(name); drive.has_value()) {
        return drive;
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
        {L"home", L"Home"}, {L"user", L"Home"}, {L"profile", L"Home"}, {L"user profile", L"Home"},
        {L"screenshots", L"Screenshots"}, {L"screenshot", L"Screenshots"},
        {L"onedrive", L"OneDrive"}, {L"one drive", L"OneDrive"},
        {L"appdata", L"AppData"}, {L"app data", L"AppData"}, {L"roaming", L"AppData"},
        {L"roaming appdata", L"AppData"}, {L"%appdata%", L"AppData"},
        {L"local appdata", L"LocalAppData"}, {L"localappdata", L"LocalAppData"},
        {L"appdata local", L"LocalAppData"}, {L"%localappdata%", L"LocalAppData"},
        {L"saved games", L"SavedGames"}, {L"camera roll", L"CameraRoll"},
        {L"program files", L"ProgramFiles"}, {L"programs", L"ProgramFiles"},
        {L"program files x86", L"ProgramFilesX86"}, {L"program files (x86)", L"ProgramFilesX86"},
        {L"startup", L"Startup"}, {L"startup programs", L"Startup"},
        {L"recent", L"Recent"}, {L"recent files", L"Recent"}, {L"public", L"Public"},
        {L"fonts", L"Fonts"}, {L"temp", L"Temp"}, {L"tmp", L"Temp"}, {L"temporary files", L"Temp"},
        {L"%temp%", L"Temp"},
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

// "projects folder" -> an existing folder named "projects" directly under the
// profile, Documents, Desktop, Downloads, OneDrive or a local drive root.
std::optional<std::wstring> NamedSubfolder(std::wstring name) {
    name = StripFolderWords(std::move(name));
    if (name.empty() || name.size() > 64 || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos ||
        name == L"." || name.find(L"..") != std::wstring::npos) {
        return std::nullopt;
    }
    std::vector<std::wstring> roots;
    for (const NamedFolder& folder : UserFolders()) {
        const std::wstring_view label = folder.label;
        if (label == L"Home" || label == L"Documents" || label == L"Desktop" || label == L"Downloads" ||
            label == L"OneDrive" || label == L"Pictures" || label == L"Videos" || label == L"Music") {
            roots.push_back(folder.path);
        }
    }
    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'C'; letter <= L'Z'; ++letter) {
        if ((drives & (1u << (letter - L'A'))) == 0) {
            continue;
        }
        std::wstring root;
        root.push_back(letter);
        root += L":\\";
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED) {
            roots.push_back(root);
        }
    }
    for (const std::wstring& root : roots) {
        std::wstring candidate = root;
        if (!candidate.ends_with(L"\\")) {
            candidate += L'\\';
        }
        candidate += name;
        if (IsDirectory(candidate)) {
            wchar_t full[MAX_PATH * 2] = {};
            const DWORD fullLen = GetFullPathNameW(candidate.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
            if (fullLen > 0 && fullLen < std::size(full)) {
                return std::wstring(full, fullLen);
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
        // Bare folder name from the model ("P projects"): existing named folder only.
        if (raw.find_first_of(L"\\/:") == std::wstring::npos) {
            if (auto named = NamedSubfolder(raw); named.has_value()) {
                return named;
            }
        }
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

// ---- App catalog matching (no model) ----------------------------------------

std::vector<std::wstring> GoalKeywords(const std::wstring& request) {
    static constexpr std::wstring_view stop[] = {
        L"open", L"launch", L"start", L"run", L"play", L"find", L"search", L"for", L"go",
        L"to", L"the", L"a", L"an", L"my", L"me", L"on", L"in", L"and", L"then", L"please",
        L"show", L"app", L"application", L"some", L"up", L"with", L"of", L"it", L"i", L"want",
        L"need", L"can", L"you", L"could", L"would", L"something", L"program", L"now", L"just",
        L"quickly", L"pls", L"let", L"lets", L"let's", L"get", L"bring", L"fire", L"boot",
        L"switch", L"use", L"using", L"is", L"that", L"this", L"thing", L"new", L"window",
        L"hey", L"wanna", L"like", L"i'd", L"load", L"focus", L"client",
    };
    std::vector<std::wstring> words;
    std::wstring cleaned = ToLowerWide(request);
    for (wchar_t& c : cleaned) {
        if (!IsWordChar(c) && c != L'.' && c != L'+' && c != L'#' && c != L'\'') {
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

std::wstring JoinWords(const std::vector<std::wstring>& words) {
    std::wstring joined;
    for (const std::wstring& word : words) {
        if (!joined.empty()) {
            joined.push_back(L' ');
        }
        joined += word;
    }
    return joined;
}

bool SameApp(const LaunchCandidate& a, const LaunchCandidate& b) {
    if (&a == &b) {
        return true;
    }
    if (!a.executablePath.empty() && !b.executablePath.empty() &&
        ToLowerWide(a.executablePath) == ToLowerWide(b.executablePath)) {
        return true;
    }
    const std::wstring an = NormalizeKey(a.name);
    return !an.empty() && an == NormalizeKey(b.name);
}

// One distinct app among `hits` (duplicates of the same exe/name collapse,
// preferring a running instance). nullptr when two different apps remain.
const LaunchCandidate* UniqueApp(const std::vector<const LaunchCandidate*>& hits) {
    const LaunchCandidate* chosen = nullptr;
    for (const LaunchCandidate* hit : hits) {
        if (chosen == nullptr) {
            chosen = hit;
        } else if (SameApp(*chosen, *hit)) {
            if (!chosen->running && hit->running) {
                chosen = hit;
            }
        } else {
            return nullptr;
        }
    }
    return chosen;
}

// Optimal string alignment distance (adjacent transpositions count once),
// bounded: returns limit + 1 as soon as it cannot be within `limit`.
size_t EditDistance(const std::wstring& a, const std::wstring& b, size_t limit) {
    const size_t n = a.size();
    const size_t m = b.size();
    if ((n > m ? n - m : m - n) > limit) {
        return limit + 1;
    }
    std::vector<size_t> prev2(m + 1), prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) {
        prev[j] = j;
    }
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        size_t rowMin = cur[0];
        for (size_t j = 1; j <= m; ++j) {
            const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = (std::min)({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                cur[j] = (std::min)(cur[j], prev2[j - 2] + 1);
            }
            rowMin = (std::min)(rowMin, cur[j]);
        }
        if (rowMin > limit) {
            return limit + 1;
        }
        prev2.swap(prev);
        prev.swap(cur);
    }
    return prev[m];
}

// Common names people type that do not appear literally in the catalog entry.
struct AppAlias {
    std::wstring_view keys;   // normalized (NormalizeKey) aliases, '|'-separated
    std::wstring_view exes;   // exe stems, '|'-separated
    std::wstring_view names;  // normalized display-name prefixes, '|'-separated
};

constexpr AppAlias kAppAliases[] = {
    {L"vscode|vsc|code|visualstudiocode", L"code", L"visualstudiocode"},
    {L"vs|visualstudio", L"devenv", L"visualstudio20"},
    {L"chrome|googlechrome", L"chrome", L"googlechrome"},
    {L"firefox|ff|mozilla|mozillafirefox", L"firefox", L"firefox|mozillafirefox"},
    {L"edge|msedge|microsoftedge", L"msedge", L"microsoftedge"},
    {L"brave", L"brave", L"brave"},
    {L"opera|operagx", L"opera", L"opera"},
    {L"word|msword|microsoftword", L"winword", L"word|microsoftword"},
    {L"excel|msexcel|microsoftexcel", L"excel", L"excel|microsoftexcel"},
    {L"powerpoint|ppt|microsoftpowerpoint", L"powerpnt", L"powerpoint|microsoftpowerpoint"},
    {L"outlook|microsoftoutlook", L"outlook|olk", L"outlook"},
    {L"onenote", L"onenote", L"onenote"},
    {L"teams|msteams|microsoftteams", L"ms-teams|teams", L"microsoftteams|teams"},
    {L"terminal|windowsterminal|wt", L"windowsterminal|wt", L"terminal|windowsterminal"},
    {L"cmd|commandprompt|commandline", L"cmd", L"commandprompt"},
    {L"powershell|pwsh|ps1", L"pwsh|powershell", L"powershell|windowspowershell"},
    {L"calc|calculator", L"calculatorapp|calc|calculator", L"calculator"},
    {L"explorer|fileexplorer|files|filemanager|mycomputer|thispc|computer", L"explorer", L"fileexplorer"},
    {L"taskmanager|taskmgr|tm", L"taskmgr", L"taskmanager"},
    {L"settings|windowssettings|pcsettings|systemsettings", L"systemsettings", L"settings"},
    {L"controlpanel", L"control", L"controlpanel"},
    {L"notepad", L"notepad", L"notepad"},
    {L"notepad++|npp|notepadplusplus", L"notepad++", L"notepad++"},
    {L"paint|mspaint", L"mspaint", L"paint"},
    {L"snippingtool|snip|snipping|screenshottool|snipandsketch", L"snippingtool", L"snippingtool"},
    {L"store|microsoftstore|msstore|appstore", L"winstore.app", L"microsoftstore"},
    {L"obs|obsstudio", L"obs64|obs", L"obsstudio"},
    {L"photoshop|ps", L"photoshop", L"adobephotoshop|photoshop"},
    {L"premiere|premierepro", L"adobe premiere pro", L"adobepremierepro"},
    {L"lightroom|lr", L"lightroom", L"adobelightroom|lightroom"},
    {L"illustrator", L"illustrator", L"adobeillustrator"},
    {L"acrobat|adobereader|pdfreader", L"acrobat|acrord32", L"adobeacrobat"},
    {L"epic|epicgames|epiclauncher|epicgameslauncher", L"epicgameslauncher", L"epicgames"},
    {L"battlenet|bnet|blizzard", L"battle.net|battle.net launcher", L"battlenet"},
    {L"steam", L"steam", L"steam"},
    {L"discord", L"discord", L"discord"},
    {L"spotify", L"spotify", L"spotify"},
    {L"whatsapp", L"whatsapp", L"whatsapp"},
    {L"telegram", L"telegram", L"telegram"},
    {L"zoom", L"zoom", L"zoom"},
    {L"slack", L"slack", L"slack"},
    {L"vlc|vlcplayer", L"vlc", L"vlc"},
    {L"gimp", L"gimp-2.10|gimp-3.0|gimp", L"gimp"},
    {L"blender", L"blender", L"blender"},
    {L"xbox|xboxapp", L"xboxpcapp|xbox", L"xbox"},
    {L"gamebar|xboxgamebar", L"gamebar", L"xboxgamebar|gamebar"},
    {L"minecraft", L"minecraft|minecraftlauncher", L"minecraft"},
    {L"lmstudio", L"lm studio", L"lmstudio"},
    {L"cursor", L"cursor", L"cursor"},
    {L"gitbash", L"git-bash", L"gitbash"},
    {L"githubdesktop", L"githubdesktop", L"githubdesktop"},
    {L"mediaplayer|windowsmediaplayer|wmp", L"wmplayer|microsoft.media.player", L"mediaplayer|windowsmediaplayer"},
    {L"photos|photosapp", L"photos|microsoft.photos", L"photos"},
    {L"camera", L"windowscamera", L"camera"},
    {L"clock|alarms|timer|stopwatch", L"time", L"clock|alarms"},
    {L"weather", L"microsoft.msn.weather", L"weather|msnweather"},
    {L"regedit|registryeditor", L"regedit", L"registryeditor"},
    {L"devicemanager", L"devmgmt", L"devicemanager"},
    {L"hoverdock", L"dock", L"hoverdock"},
};

bool ListContains(std::wstring_view list, std::wstring_view value) {
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(L'|', start);
        if (end == std::wstring_view::npos) {
            end = list.size();
        }
        if (!value.empty() && list.substr(start, end - start) == value) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

bool ListHasPrefixOf(std::wstring_view list, const std::wstring& value) {
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(L'|', start);
        if (end == std::wstring_view::npos) {
            end = list.size();
        }
        const std::wstring_view prefix = list.substr(start, end - start);
        if (!prefix.empty() && value.starts_with(prefix)) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

const LaunchCandidate* AliasMatch(const std::wstring& key, const std::vector<LaunchCandidate>& candidates) {
    for (const AppAlias& alias : kAppAliases) {
        if (!ListContains(alias.keys, key)) {
            continue;
        }
        std::vector<const LaunchCandidate*> byExe;
        std::vector<const LaunchCandidate*> byName;
        for (const LaunchCandidate& candidate : candidates) {
            const std::wstring exe = ExeStem(candidate.executable.empty() ? candidate.executablePath
                                                                           : candidate.executable);
            if (ListContains(alias.exes, exe)) {
                byExe.push_back(&candidate);
            } else if (ListHasPrefixOf(alias.names, NormalizeKey(candidate.name))) {
                byName.push_back(&candidate);
            }
        }
        if (const LaunchCandidate* hit = UniqueApp(byExe); hit != nullptr) {
            return hit;
        }
        if (byExe.empty()) {
            if (const LaunchCandidate* hit = UniqueApp(byName); hit != nullptr) {
                return hit;
            }
        }
        return nullptr;
    }
    return nullptr;
}

std::wstring Initials(const std::wstring& name) {
    std::wstring initials;
    bool atWord = true;
    for (const wchar_t c : name) {
        if (std::iswalnum(c) != 0) {
            if (atWord) {
                initials.push_back(static_cast<wchar_t>(std::towlower(c)));
            }
            atWord = false;
        } else {
            atWord = true;
        }
    }
    return initials;
}

bool KeywordInIdentity(const LaunchCandidate& c, const std::wstring& keyword) {
    for (const std::wstring* field : {&c.name, &c.pinName, &c.shortcutName, &c.productName, &c.executable}) {
        if (!field->empty() && ToLowerWide(*field).find(keyword) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

// Model-free app pick: exact (normalized) name -> alias -> lexical score with
// a clear margin -> acronym -> unique name prefix -> typo (edit distance).
const LaunchCandidate* ConfidentApp(const std::wstring& appText, const std::vector<LaunchCandidate>& candidates) {
    const std::wstring text = CleanAppText(appText);
    const std::vector<std::wstring> tokens = Tokenize(text);
    if (tokens.empty() || tokens.size() > 4 || candidates.empty()) {
        return nullptr;
    }
    const std::wstring key = NormalizeKey(text);
    if (key.size() < 2) {
        return nullptr;
    }

    // 1. Exact normalized name / pin / shortcut / product / exe.
    {
        std::vector<const LaunchCandidate*> exact;
        for (const LaunchCandidate& c : candidates) {
            if (NormalizeKey(c.name) == key || NormalizeKey(c.pinName) == key ||
                NormalizeKey(c.shortcutName) == key || NormalizeKey(c.productName) == key ||
                NormalizeKey(ExeStem(c.executable)) == key) {
                exact.push_back(&c);
            }
        }
        if (const LaunchCandidate* hit = UniqueApp(exact); hit != nullptr) {
            return hit;
        }
        if (exact.size() > 1) {
            // Two different apps share the exact name: prefer a match on the display name.
            std::vector<const LaunchCandidate*> byName;
            for (const LaunchCandidate* c : exact) {
                if (NormalizeKey(c->name) == key) {
                    byName.push_back(c);
                }
            }
            return UniqueApp(byName);
        }
    }

    // 2. Alias table ("vscode", "word", "cmd", "task manager").
    if (const LaunchCandidate* hit = AliasMatch(key, candidates); hit != nullptr) {
        return hit;
    }

    // 3. Lexical score with a clear margin over the best *different* app.
    double best = -1.0;
    const LaunchCandidate* winner = nullptr;
    std::vector<std::pair<double, const LaunchCandidate*>> scored;
    scored.reserve(candidates.size());
    for (const LaunchCandidate& c : candidates) {
        const double score = CandidateRelevance(c, text, tokens);
        scored.emplace_back(score, &c);
        if (score > best || (score == best && winner != nullptr && !winner->running && c.running)) {
            best = score;
            winner = &c;
        }
    }
    double second = -1.0;
    for (const auto& [score, c] : scored) {
        if (winner != nullptr && !SameApp(*winner, *c)) {
            second = (std::max)(second, score);
        }
    }
    if (winner != nullptr && best >= kConfidentMinScore && best - second >= kConfidentMargin) {
        // Every meaningful word must belong to the winner's identity, so
        // "spotify and discord" / "google maps london" / "steam store" are not
        // collapsed onto Spotify / Chrome / Steam.
        const std::vector<std::wstring> keywords = GoalKeywords(text);
        const size_t covered = static_cast<size_t>(std::ranges::count_if(keywords,
            [&](const std::wstring& keyword) { return KeywordInIdentity(*winner, keyword); }));
        if (covered == keywords.size()) {
            return winner;
        }
    }

    // 4. Acronym ("vsc" -> Visual Studio Code, "gcs" etc.).
    if (tokens.size() == 1 && key.size() >= 2 && key.size() <= 5) {
        std::vector<const LaunchCandidate*> hits;
        for (const LaunchCandidate& c : candidates) {
            const std::wstring initials = Initials(c.name);
            if (initials.size() >= 2 && initials == key) {
                hits.push_back(&c);
            }
        }
        if (const LaunchCandidate* hit = UniqueApp(hits); hit != nullptr) {
            return hit;
        }
    }

    // 5. Unique normalized-name prefix ("photosh" -> Adobe Photoshop is not a prefix,
    //    but "photosho" -> "Photoshop Express" is; "davinci" -> DaVinci Resolve).
    if (key.size() >= 4) {
        std::vector<const LaunchCandidate*> hits;
        for (const LaunchCandidate& c : candidates) {
            if (NormalizeKey(c.name).starts_with(key)) {
                hits.push_back(&c);
            }
        }
        if (const LaunchCandidate* hit = UniqueApp(hits); hit != nullptr) {
            return hit;
        }
    }

    // 6. Typos ("chorme", "spotfy", "dicsord") when nothing matched lexically.
    //    5+ chars only: 4-letter words are one edit away from too many others.
    if (key.size() >= 5 && best < 40.0) {
        const size_t limit = key.size() >= 8 ? 2 : 1;
        size_t bestDistance = limit + 1;
        std::vector<const LaunchCandidate*> hits;
        std::vector<const LaunchCandidate*> fullNameHits;  // whole display name within the limit
        for (const LaunchCandidate& c : candidates) {
            const size_t fullDistance = EditDistance(key, NormalizeKey(c.name), limit);
            size_t distance = fullDistance;
            distance = (std::min)(distance, EditDistance(key, NormalizeKey(ExeStem(c.executable)), limit));
            for (const std::wstring& word : Tokenize(ToLowerWide(c.name))) {
                const std::wstring w = NormalizeKey(word);
                if (w.size() >= 5) {
                    distance = (std::min)(distance, EditDistance(key, w, limit));
                }
            }
            if (distance < bestDistance) {
                bestDistance = distance;
                hits.clear();
                fullNameHits.clear();
            }
            if (distance == bestDistance && distance <= limit) {
                hits.push_back(&c);
                if (fullDistance == distance) {
                    fullNameHits.push_back(&c);
                }
            }
        }
        if (bestDistance <= limit) {
            if (const LaunchCandidate* hit = UniqueApp(hits); hit != nullptr) {
                return hit;
            }
            // "dicsord": Discord (whole name) beats Discord PTB (one word of it).
            if (const LaunchCandidate* hit = UniqueApp(fullNameHits); hit != nullptr) {
                return hit;
            }
        }
    }
    return nullptr;
}

std::vector<std::pair<double, const LaunchCandidate*>> ScoreForKeywords(
    const std::vector<LaunchCandidate>& candidates, const std::vector<std::wstring>& keywords) {
    const std::wstring joined = JoinWords(keywords);
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
    return ranked;
}

std::vector<const LaunchCandidate*> RankForAgent(const std::vector<LaunchCandidate>& candidates,
    const std::wstring& query, size_t limit) {
    std::vector<const LaunchCandidate*> top;
    for (const auto& [score, candidate] : ScoreForKeywords(candidates, GoalKeywords(query))) {
        if (top.size() >= limit) {
            break;
        }
        if (std::ranges::none_of(top, [&](const LaunchCandidate* t) { return SameApp(*t, *candidate); })) {
            top.push_back(candidate);
        }
    }
    return top;
}

// "search_apps"-equivalent local match for goals: launch when one app clearly
// wins and its identity covers the goal keywords ("open the steam client",
// "start elden ring"). purposeQuery relaxes coverage for the model's own
// S <purpose> query (description matches count).
const LaunchCandidate* StrongLocalMatch(const std::wstring& goal, const std::vector<LaunchCandidate>& candidates,
    bool purposeQuery) {
    const std::vector<std::wstring> keywords = GoalKeywords(goal);
    if (keywords.empty() || keywords.size() > 4) {
        return nullptr;
    }
    const auto ranked = ScoreForKeywords(candidates, keywords);
    if (ranked.empty()) {
        return nullptr;
    }
    const auto& [best, winner] = ranked.front();
    double second = 0.0;
    for (const auto& [score, c] : ranked) {
        if (!SameApp(*winner, *c)) {
            second = score;
            break;
        }
    }
    if (purposeQuery) {
        return best >= 45.0 && second < best * 0.6 ? winner : nullptr;
    }
    if (best < kConfidentMinScore || best - second < kConfidentMargin) {
        return nullptr;
    }
    size_t covered = 0;
    for (const std::wstring& keyword : keywords) {
        if (KeywordInIdentity(*winner, keyword)) {
            ++covered;
        }
    }
    const size_t needed = keywords.size() <= 2 ? keywords.size() : keywords.size() - 1;
    return covered >= needed ? winner : nullptr;
}

// ---- Model I/O ----------------------------------------------------------------

// "c3=Steam; c9=Epic Games Launcher" (~4-6 tokens per app vs ~15 for JSON).
std::string CompactAppList(const std::vector<const LaunchCandidate*>& list) {
    if (list.empty()) {
        return "none";
    }
    std::string out;
    for (size_t index = 0; index < list.size(); ++index) {
        if (index > 0) {
            out += "; ";
        }
        out += list[index]->id;
        out += '=';
        std::wstring name = TruncateWide(list[index]->name, 40);
        std::ranges::replace(name, L'\n', L' ');
        std::ranges::replace(name, L';', L',');
        out += WideToUtf8(name);
    }
    return out;
}

// Static and byte-identical across ranking + agent requests: the single
// llama-server slot keeps it in the prompt cache.
const std::string& AgentSystemPrompt() {
    static const std::string prompt =
        "Hoverdock Search. Reply with exactly ONE line, no prose:\n"
        "L <id> launch a listed app (L c3)\n"
        "W <query> Google search\n"
        "Y <query> YouTube search\n"
        "U <url> open an https URL\n"
        "P <folder> open a folder: Downloads, Documents, Desktop, Pictures, Music, Videos, Home, "
        "Screenshots, OneDrive, or an absolute path\n"
        "S <words> search installed apps by purpose\n"
        "N <short reason> nothing fits\n"
        "Prefer L when a listed app fits. Use S only when no listed app fits but an app might.";
    return prompt;
}

std::string BuildModelBody(const std::string& user, int maxTokens, const char* grammar) {
    std::string body = "{\"model\":\"local\",\"temperature\":0,\"max_tokens\":";
    body += std::to_string(maxTokens);
    body += ",\"cache_prompt\":true,\"chat_template_kwargs\":{\"enable_thinking\":false},"
            "\"reasoning_format\":\"none\",\"stop\":[\"\\n\"],";
    if (grammar != nullptr && !g_grammarRejected.load()) {
        body += "\"grammar\":";
        AppendEscaped(body, grammar);
        body += ',';
    }
    body += "\"messages\":[{\"role\":\"system\",\"content\":";
    AppendEscaped(body, AgentSystemPrompt());
    body += "},{\"role\":\"user\",\"content\":";
    // Without the grammar the model may think; the soft switch keeps it short.
    AppendEscaped(body, (grammar == nullptr || g_grammarRejected.load()) ? "/no_think\n" + user : user);
    body += "}]}";
    return body;
}

struct ModelReply {
    bool reached = false;  // got an HTTP response with assistant content
    std::string content;
    std::wstring error;
};

ModelReply AskModel(const ParsedUrl& url, const std::string& user, int maxTokens, const char* grammar,
    DWORD receiveTimeoutMs) {
    ModelReply reply;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool withGrammar = grammar != nullptr && !g_grammarRejected.load();
        const std::string body = BuildModelBody(user, maxTokens, grammar);
        DWORD status = 0;
        std::string response;
        std::wstring error;
        if (!HttpExchange(url, L"POST", L"/v1/chat/completions", &body, status, response, error,
                receiveTimeoutMs)) {
            g_promptWarm = false;
            reply.error = error;
            return reply;
        }
        if (status >= 200 && status < 300) {
            reply.reached = true;
            if (const auto content = ExtractAssistantContent(response); content.has_value()) {
                reply.content = *content;
            }
            return reply;
        }
        // Older servers reject a custom grammar: retry once without it.
        if (withGrammar && status >= 400 && status < 500) {
            g_grammarRejected = true;
            continue;
        }
        reply.error = L"llama-server HTTP " + std::to_wstring(status);
        return reply;
    }
    return reply;
}

struct ModelLine {
    char op = 0;  // L W Y U P S N, or 0 when unparsed
    std::wstring arg;
};

// Compact protocol first; tolerate legacy JSON tool calls when the grammar
// was not applied ({"tool":"launch_app","id":"c3"} etc.).
ModelLine ParseModelLine(std::string content) {
    ModelLine line;
    if (const size_t endThink = content.find("</think>"); endThink != std::string::npos) {
        content = content.substr(endThink + 8);
    }
    std::wstring text = TrimWide(Utf8ToWide(content));
    while (!text.empty() && (text.front() == L'`' || text.front() == L'*' || text.front() == L'-')) {
        text = TrimWide(text.substr(1));
    }
    if (const size_t newline = text.find_first_of(L"\r\n"); newline != std::wstring::npos) {
        text = TrimWide(text.substr(0, newline));
    }
    if (!text.empty() && text.front() != L'{') {
        const wchar_t op = static_cast<wchar_t>(std::towupper(text.front()));
        const bool spaced = text.size() == 1 || text[1] == L' ' || text[1] == L':';
        if (spaced && std::wstring_view(L"LWYUPSN").find(op) != std::wstring_view::npos) {
            line.op = static_cast<char>(op);
            line.arg = text.size() > 1 ? TrimWide(text.substr(2)) : L"";
            return line;
        }
    }
    const auto object = ExtractJsonObject(content);
    if (!object.has_value()) {
        line.arg = text;
        return line;
    }
    const std::string& json = *object;
    std::string tool;
    for (const std::string_view key : {std::string_view("tool"), std::string_view("name"),
             std::string_view("action")}) {
        if (const auto value = ExtractStringField(json, key); value.has_value()) {
            tool = *value;
            break;
        }
    }
    const auto field = [&json](std::initializer_list<std::string_view> keys) -> std::wstring {
        for (const std::string_view key : keys) {
            if (const auto value = ExtractStringField(json, key); value.has_value() && !value->empty()) {
                return TrimWide(Utf8ToWide(*value));
            }
        }
        return {};
    };
    if (tool == "launch_app" || (tool.empty() && json.find("\"id\"") != std::string::npos)) {
        line.op = 'L';
        line.arg = field({"id", "app_id", "app"});
    } else if (tool == "open_url") {
        line.op = 'U';
        line.arg = field({"url", "href", "link"});
    } else if (tool == "open_path") {
        line.op = 'P';
        line.arg = field({"path", "folder"});
    } else if (tool == "search_apps") {
        line.op = 'S';
        line.arg = field({"q", "query", "text"});
    } else if (tool == "done" || tool == "say" || tool == "reply") {
        line.op = 'N';
        line.arg = field({"say", "text", "reply", "message"});
    }
    return line;
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
    return TruncateWide(TrimWide(std::move(text)), kAgentMaxReplyChars);
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
    if (const LaunchCandidate* confident = ConfidentApp(request, candidates); confident != nullptr) {
        judgment.action = LaunchJudgment::Action::Launch;
        judgment.chosenId = confident->id;
        judgment.exists = 1.0;
        judgment.confidence = 0.95;
        judgment.usedFuzzyFallback = true;
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

    // Lexical confident hit (aliases, acronyms, typos): skip the model entirely.
    if (const LaunchCandidate* quick = ConfidentApp(cleanRequest, candidates); quick != nullptr) {
        LaunchJudgment judgment;
        judgment.action = LaunchJudgment::Action::Launch;
        judgment.chosenId = quick->id;
        judgment.exists = 1.0;
        judgment.confidence = 0.95;
        judgment.usedFuzzyFallback = true;
        return judgment;
    }

    // Same cached system prompt as the agent; launch-only grammar so the reply
    // is "L cN" or "N" (~4 output tokens) over <=8 compact candidates.
    std::vector<const LaunchCandidate*> ranked = RankForAgent(candidates, cleanRequest, kMaxCandidates);
    if (ranked.empty()) {
        const std::wstring queryLower = ToLowerWide(cleanRequest);
        const std::vector<std::wstring> tokens = Tokenize(queryLower);
        std::vector<std::pair<double, const LaunchCandidate*>> scored;
        for (const LaunchCandidate& c : candidates) {
            scored.emplace_back(CandidateRelevance(c, queryLower, tokens), &c);
        }
        std::stable_sort(scored.begin(), scored.end(),
            [](const auto& l, const auto& r) { return l.first > r.first; });
        for (size_t i = 0; i < scored.size() && ranked.size() < kMaxCandidates; ++i) {
            ranked.push_back(scored[i].second);
        }
    }
    const ParsedUrl url = ParseBaseUrl(baseUrl);
    std::string user = "goal: open app ";
    user += WideToUtf8(cleanRequest);
    user += "\napps: ";
    user += CompactAppList(ranked);
    const ModelReply reply = AskModel(url, user, kRankMaxTokens, kLaunchOnlyGrammar, kAgentReceiveTimeoutMs);
    if (reply.reached) {
        const ModelLine line = ParseModelLine(reply.content);
        if (line.op == 'L') {
            if (const LaunchCandidate* picked = FindCandidate(candidates, WideToUtf8(line.arg));
                picked != nullptr &&
                std::ranges::any_of(ranked, [&](const LaunchCandidate* r) { return r == picked; })) {
                LaunchJudgment judgment;
                judgment.action = LaunchJudgment::Action::Launch;
                judgment.chosenId = picked->id;
                judgment.exists = 1.0;
                judgment.confidence = 0.9;
                return judgment;
            }
        } else if (line.op == 'N') {
            LaunchJudgment judgment;
            judgment.action = LaunchJudgment::Action::None;
            return judgment;
        }
    }
    LaunchJudgment fuzzy = ResolveAppFuzzy(cleanRequest, candidates);
    if (fuzzy.action == LaunchJudgment::Action::Launch || reply.reached) {
        return fuzzy;
    }
    return MakeError(reply.error.empty() ? L"llama-server unreachable; no fuzzy match." : reply.error);
}

// ---------------------------------------------------------------------------
// Agent-in-search
// ---------------------------------------------------------------------------

std::wstring LlamaServerClient::StripGoalVerbs(const std::wstring& request) {
    return StripTrailingFillers(StripLeadingPhrases(request));
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
        L"listen", L"switch", L"bring", L"focus", L"please", L"can", L"could", L"pull", L"fire",
        L"boot", L"load", L"get", L"what", L"what's", L"who", L"how", L"why", L"where", L"when",
        L"define", L"translate", L"directions", L"youtube", L"yt", L"write", L"create", L"make",
        L"code", L"build", L"generate", L"implement", L"develop", L"draft", L"compose",
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

std::optional<SearchAgentAction> LlamaServerClient::DirectAction(const std::wstring& request) {
    const std::wstring trimmed = TrimWide(request);
    if (auto web = ExplicitWebAction(trimmed); web.has_value()) {
        return web;
    }
    const std::wstring rest = StripLeadingPhrases(trimmed);
    if (rest.empty()) {
        return std::nullopt;
    }
    if (rest.find(L' ') == std::wstring::npos && LooksLikeDomain(rest)) {
        if (const auto url = NormalizeUrl(rest); url.has_value()) {
            return MakeUrlAction(*url, rest);
        }
    }
    const std::wstring restLower = ToLowerWide(rest);
    const bool commanded = ToLowerWide(trimmed) != restLower;
    const std::wstring folderWords = StripFolderWords(restLower);
    const bool saysFolder = folderWords != restLower && !folderWords.empty();
    if (commanded || saysFolder) {
        if (const auto folder = FolderByName(rest); folder.has_value()) {
            return MakePathAction(*folder, rest);
        }
    }
    if (saysFolder) {
        // "open my projects folder" -> D:\projects / %USERPROFILE%\projects ...
        if (const auto named = NamedSubfolder(rest); named.has_value()) {
            return MakePathAction(*named, *named);
        }
    }
    if (rest.size() >= 3 && rest[1] == L':' && (rest[2] == L'\\' || rest[2] == L'/')) {
        std::wstring why;
        if (const auto path = ValidateOpenPath(rest, why); path.has_value()) {
            return MakePathAction(*path, *path);
        }
    }
    return std::nullopt;
}

std::string LlamaServerClient::ConfidentAppId(const std::wstring& appText,
    const std::vector<LaunchCandidate>& candidates) {
    const LaunchCandidate* hit = ConfidentApp(appText, candidates);
    return hit == nullptr ? std::string{} : hit->id;
}

namespace {

// Bare plural folder names that are not commonly app names.
std::optional<std::wstring> BareFolder(const std::wstring& request) {
    const std::wstring lower = StripFolderWords(StripPolitePrefix(ToLowerWide(TrimWide(request))));
    static constexpr std::wstring_view bare[] = {
        L"downloads", L"documents", L"desktop", L"pictures", L"videos", L"screenshots",
        L"appdata", L"%appdata%", L"%localappdata%", L"%temp%", L"temp", L"program files",
        L"saved games", L"camera roll", L"c:", L"d:", L"c drive", L"d drive",
    };
    if (std::ranges::any_of(bare, [&](std::wstring_view b) { return lower == b; }) ||
        DriveByName(lower).has_value()) {
        return FolderByName(lower);
    }
    return std::nullopt;
}

// Create / write / code goals the catalog cannot answer -> Google, and open the
// best installed editor when one is present (Cursor > VS Code > Notepad++ >
// Notepad). Avoids a dead-end "No matching installed app." for first-time
// goals like "write hello world program" / "create a script" / "code a bot".
const LaunchCandidate* BestCodingEditor(const std::vector<LaunchCandidate>& candidates) {
    for (const std::wstring_view key : {std::wstring_view(L"cursor"), std::wstring_view(L"vscode"),
             std::wstring_view(L"code"), std::wstring_view(L"notepad++"), std::wstring_view(L"notepad")}) {
        if (const LaunchCandidate* hit = AliasMatch(std::wstring(key), candidates); hit != nullptr) {
            return hit;
        }
    }
    return nullptr;
}

bool LooksLikeCreateOrCodeGoal(const std::wstring& lower) {
    if (lower.find(L"hello world") != std::wstring::npos) {
        return true;
    }
    static constexpr std::wstring_view starts[] = {
        L"write ", L"write a ", L"write an ", L"write me ", L"write the ", L"write my ",
        L"create a ", L"create an ", L"create me ", L"create the ", L"create my ",
        L"make a ", L"make an ", L"make me ", L"make the ", L"make my ",
        L"code a ", L"code an ", L"code me ", L"code the ", L"code my ", L"code ",
        L"build a ", L"build an ", L"build me ", L"build the ",
        L"generate a ", L"generate an ", L"implement a ", L"implement an ",
        L"develop a ", L"develop an ", L"draft a ", L"draft an ",
        L"compose a ", L"compose an ",
    };
    if (!std::ranges::any_of(starts, [&](std::wstring_view s) { return lower.starts_with(s); })) {
        return false;
    }
    // "code ..." is always a coding intent. For write/create/make/build, require a
    // coding-ish noun so "create a reminder" / "make a playlist" can still fall
    // through to the model or site catalog when useful.
    if (lower.starts_with(L"code ") || lower.starts_with(L"code a ") || lower.starts_with(L"code an ") ||
        lower.starts_with(L"code me ") || lower.starts_with(L"code the ") || lower.starts_with(L"code my ")) {
        return true;
    }
    static constexpr std::wstring_view hints[] = {
        L"program", L"programme", L"script", L"code", L"function", L"class", L"algorithm",
        L"webpage", L"website", L"web page", L"html", L"css", L"javascript", L"typescript",
        L"python", L"java", L"c++", L"cpp", L"csharp", L"c#", L"rust", L"golang", L"go ",
        L"react", L"node", L"api", L"bot", L"game", L"snippet", L"macro", L"batch",
        L"powershell", L"bash", L"shell", L"sql", L"regex", L"parser", L"module",
        L"library", L"package", L"component", L"app", L"application", L"hello", L"world",
        L"firmware", L"plugin", L"addon", L"add-on", L"extension", L"cli", L"tool",
    };
    return std::ranges::any_of(hints, [&](std::wstring_view h) { return lower.find(h) != std::wstring::npos; });
}

std::optional<SearchFastPlan> CreateCodePlan(const std::wstring& request,
    const std::vector<LaunchCandidate>& candidates) {
    std::wstring lower = StripPolitePrefix(ToLowerWide(TrimWide(request)));
    if (Tokenize(lower).size() < 2 || lower.size() > kMaxQueryChars) {
        return std::nullopt;
    }
    while (!lower.empty() && (lower.back() == L'?' || lower.back() == L'.' || lower.back() == L'!')) {
        lower.pop_back();
    }
    lower = TrimWide(lower);
    if (!LooksLikeCreateOrCodeGoal(lower)) {
        return std::nullopt;
    }
    SearchFastPlan plan;
    plan.route = L"web-create";
    if (const LaunchCandidate* editor = BestCodingEditor(candidates); editor != nullptr) {
        plan.actions.push_back(MakeLaunchAction(*editor));
    }
    plan.actions.push_back(GoogleSearch(lower));
    return plan;
}

std::optional<SearchFastPlan> PlanSingle(const std::wstring& text, const std::vector<LaunchCandidate>& candidates) {
    SearchFastPlan plan;
    const auto single = [&plan](SearchAgentAction action, const wchar_t* route) {
        plan.actions.push_back(std::move(action));
        plan.route = route;
        return std::optional<SearchFastPlan>(std::move(plan));
    };
    if (auto direct = LlamaServerClient::DirectAction(text); direct.has_value()) {
        return single(std::move(*direct), L"direct");
    }
    // "go to github" / "visit reddit": web verbs prefer the site over an app
    // that merely shares the name (GitHub Desktop).
    {
        const std::wstring lower = StripPolitePrefix(ToLowerWide(TrimWide(text)));
        if (StartsWithAny(lower, {L"go to ", L"goto ", L"visit ", L"browse to ", L"navigate to ",
                L"take me to ", L"head to ", L"browse "})) {
            if (auto site = SiteAction(text); site.has_value()) {
                return single(std::move(*site), L"site");
            }
        }
    }
    const std::wstring appText = LlamaServerClient::StripGoalVerbs(text);
    if (const LaunchCandidate* app = ConfidentApp(appText, candidates); app != nullptr) {
        return single(MakeLaunchAction(*app), L"app-confident");
    }
    if (auto site = SiteAction(text); site.has_value()) {
        return single(std::move(*site), L"site");
    }
    if (auto question = QuestionAction(text); question.has_value()) {
        return single(std::move(*question), L"web-question");
    }
    if (auto create = CreateCodePlan(text, candidates); create.has_value()) {
        return create;
    }
    if (auto folder = BareFolder(text); folder.has_value()) {
        return single(MakePathAction(*folder, *folder), L"folder");
    }
    if (const LaunchCandidate* app = StrongLocalMatch(text, candidates, false); app != nullptr) {
        return single(MakeLaunchAction(*app), L"app-keywords");
    }
    return std::nullopt;
}

std::vector<std::wstring> SplitCompoundGoal(const std::wstring& request) {
    std::wstring lower = ToLowerWide(TrimWide(request));
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start < lower.size()) {
        size_t best = std::wstring::npos;
        size_t bestLen = 0;
        for (const std::wstring_view sep : {std::wstring_view(L" and then "), std::wstring_view(L" and also "),
                 std::wstring_view(L" and "), std::wstring_view(L" then "), std::wstring_view(L", "),
                 std::wstring_view(L" & "), std::wstring_view(L" + ")}) {
            const size_t at = lower.find(sep, start);
            if (at != std::wstring::npos && (at < best || (at == best && sep.size() > bestLen))) {
                best = at;
                bestLen = sep.size();
            }
        }
        if (best == std::wstring::npos) {
            parts.push_back(TrimWide(request.substr(start)));
            break;
        }
        parts.push_back(TrimWide(request.substr(start, best - start)));
        start = best + bestLen;
    }
    std::erase_if(parts, [](const std::wstring& p) { return p.empty(); });
    return parts;
}

std::optional<SearchFastPlan> PlanCompound(const std::vector<std::wstring>& parts,
    const std::vector<LaunchCandidate>& candidates);

}  // namespace

std::optional<SearchFastPlan> LlamaServerClient::PlanWithoutModel(const std::wstring& request,
    const std::vector<LaunchCandidate>& candidates) {
    const std::wstring goal = TruncateWide(TrimWide(request), kMaxRequestChars);
    if (goal.empty()) {
        return std::nullopt;
    }
    // "open spotify and discord", "open downloads then play lofi on youtube":
    // tried first, but only taken when every part resolves without the model;
    // otherwise the whole goal is planned as one ("rock and roll on youtube").
    const std::vector<std::wstring> parts = SplitCompoundGoal(goal);
    if (parts.size() >= 2 && parts.size() <= 3) {
        if (auto combined = PlanCompound(parts, candidates); combined.has_value()) {
            return combined;
        }
    }
    return PlanSingle(goal, candidates);
}

namespace {

std::optional<SearchFastPlan> PlanCompound(const std::vector<std::wstring>& parts,
    const std::vector<LaunchCandidate>& candidates) {
    SearchFastPlan combined;
    combined.route = L"compound";
    std::wstring verb;
    {
        const std::wstring firstLower = ToLowerWide(parts.front());
        for (const std::wstring_view v : {std::wstring_view(L"open "), std::wstring_view(L"launch "),
                 std::wstring_view(L"start "), std::wstring_view(L"run ")}) {
            if (firstLower.starts_with(v)) {
                verb = std::wstring(v);
                break;
            }
        }
    }
    for (size_t index = 0; index < parts.size(); ++index) {
        std::optional<SearchFastPlan> part = PlanSingle(parts[index], candidates);
        if (!part.has_value() && index > 0 && !verb.empty()) {
            // "open spotify and discord" -> "open discord"
            part = PlanSingle(verb + parts[index], candidates);
        }
        if (!part.has_value()) {
            return std::nullopt;
        }
        for (SearchAgentAction& action : part->actions) {
            const bool duplicate = std::ranges::any_of(combined.actions, [&](const SearchAgentAction& a) {
                return a.kind == action.kind && a.appId == action.appId && a.target == action.target;
            });
            if (!duplicate) {
                combined.actions.push_back(std::move(action));
            }
        }
    }
    if (combined.actions.empty()) {
        return std::nullopt;
    }
    return combined;
}

}  // namespace

void LlamaServerClient::WarmPromptCache(const std::wstring& baseUrl) {
    if (g_promptWarm.load()) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = g_lastWarmAttempt.load();
    if (last != 0 && now - last < 20000) {
        return;  // server likely down/loading: back off
    }
    bool expected = false;
    if (!g_warmInFlight.compare_exchange_strong(expected, true)) {
        return;
    }
    g_lastWarmAttempt = now;
    // Evaluates the shared system prompt (~150 tokens, ~20 s cold on CPU) so the
    // first real model request only pays for its own short user message.
    const ParsedUrl url = ParseBaseUrl(baseUrl);
    const std::string body = BuildModelBody("goal: ok\napps: none", 1, kLaunchOnlyGrammar);
    DWORD status = 0;
    std::string response;
    std::wstring error;
    if (HttpExchange(url, L"POST", L"/v1/chat/completions", &body, status, response, error,
            kAgentReceiveTimeoutMs) &&
        status >= 200 && status < 300) {
        g_promptWarm = true;
    }
    g_warmInFlight = false;
}

namespace {

std::optional<SearchAgentAction> ActionFromModelLine(const ModelLine& line,
    const std::vector<LaunchCandidate>& candidates, std::wstring& note) {
    switch (line.op) {
    case 'L': {
        const LaunchCandidate* candidate = FindCandidate(candidates, WideToUtf8(line.arg));
        if (candidate == nullptr && !line.arg.empty()) {
            candidate = ConfidentApp(line.arg, candidates);  // tolerate "L Steam"
        }
        if (candidate == nullptr) {
            note = L"Model picked an unknown app.";
            return std::nullopt;
        }
        return MakeLaunchAction(*candidate);
    }
    case 'W':
        if (line.arg.empty() || line.arg.size() > kMaxQueryChars) {
            return std::nullopt;
        }
        return GoogleSearch(line.arg);
    case 'Y':
        if (line.arg.empty() || line.arg.size() > kMaxQueryChars) {
            return std::nullopt;
        }
        return YouTubeSearch(line.arg);
    case 'U': {
        const auto target = NormalizeUrl(line.arg);
        if (!target.has_value()) {
            note = L"Blocked: only http/https URLs.";
            return std::nullopt;
        }
        return MakeUrlAction(*target, TruncateWide(*target, 60));
    }
    case 'P': {
        std::wstring why;
        const auto target = ValidateOpenPath(line.arg, why);
        if (!target.has_value()) {
            note = L"Could not open that path (" + why + L").";
            return std::nullopt;
        }
        return MakePathAction(*target, *target);
    }
    default:
        return std::nullopt;
    }
}

}  // namespace

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

    // Round 1: single shot. No /health probe: a failed POST marks the server
    // unavailable and the caller falls back to fuzzy search.
    const ParsedUrl url = ParseBaseUrl(baseUrl);
    result.steps = 1;
    notify(L"Asking local model...");
    std::string user = "goal: ";
    user += WideToUtf8(goal);
    user += "\napps: ";
    user += CompactAppList(RankForAgent(candidates, goal, kAgentContextApps));
    ModelReply reply = AskModel(url, user, kAgentMaxTokens, kAgentGrammar, kAgentReceiveTimeoutMs);
    if (!reply.reached) {
        result.serverUnavailable = true;
        return result;
    }
    if (isCancelled()) {
        return result;
    }
    ModelLine line = ParseModelLine(reply.content);

    if (line.op == 'S') {
        std::wstring query = line.arg.empty() ? goal : TruncateWide(line.arg, 60);
        notify(L"Searching apps: " + query);
        // search_apps-equivalent locally; launch straight away when one app
        // clearly wins instead of spending a second model round.
        const LaunchCandidate* quick = ConfidentApp(query, candidates);
        if (quick == nullptr) {
            quick = StrongLocalMatch(query, candidates, true);
        }
        if (quick != nullptr) {
            result.actions.push_back(MakeLaunchAction(*quick));
            notify(L"Launching " + quick->name + L"...");
            return result;
        }
        const std::vector<const LaunchCandidate*> found = RankForAgent(candidates, query, kAgentSearchResults);
        if (found.empty()) {
            result.reply = Sentence(L"No installed app found for \"" + query + L"\".");
            return result;
        }
        if (kAgentMaxRounds < 2 || isCancelled()) {
            return result;
        }
        result.steps = 2;
        notify(L"Asking local model (round 2)...");
        std::string followUp = "goal: ";
        followUp += WideToUtf8(goal);
        followUp += "\napps: ";
        followUp += CompactAppList(found);
        reply = AskModel(url, followUp, kRankMaxTokens, kLaunchOnlyGrammar, kAgentReceiveTimeoutMs);
        if (!reply.reached) {
            result.reply = L"Agent stopped: " + (reply.error.empty() ? std::wstring(L"no reply") : reply.error);
            return result;
        }
        line = ParseModelLine(reply.content);
    }

    std::wstring note;
    if (auto action = ActionFromModelLine(line, candidates, note); action.has_value()) {
        notify(L"Opening " + TruncateWide(action->label, 60) + L"...");
        result.actions.push_back(std::move(*action));
        return result;
    }
    if (line.op == 'N') {
        result.reply = line.arg.empty() ? L"Nothing to do for that." : Sentence(line.arg);
    } else if (!note.empty()) {
        result.reply = note;
    } else if (line.op == 0 && !line.arg.empty()) {
        result.reply = Sentence(line.arg);
    } else {
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

namespace {

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

}  // namespace

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
