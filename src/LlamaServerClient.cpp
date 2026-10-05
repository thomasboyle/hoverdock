#include "LlamaServerClient.h"

#include <Windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <limits>
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
constexpr size_t kMaxCandidates = 48;
constexpr size_t kMaxRequestChars = 200;
constexpr size_t kMaxFieldChars = 96;
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

bool HttpExchange(const ParsedUrl& url, const wchar_t* method, const wchar_t* path,
    const std::string* body, DWORD& status, std::string& response, std::wstring& error) {
    status = 0;
    response.clear();
    WinHttpHandle session(WinHttpOpen(L"HoverdockLlama/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.Get() == nullptr) {
        error = L"Could not open HTTP session.";
        return false;
    }
    WinHttpSetTimeouts(session.Get(), kConnectTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
        kReceiveTimeoutMs);
    WinHttpHandle connection(WinHttpConnect(session.Get(), url.host.c_str(), url.port, 0));
    if (connection.Get() == nullptr) {
        error = L"Could not connect to llama-server.";
        return false;
    }
    const DWORD flags = url.https ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connection.Get(), method, path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (request.Get() == nullptr) {
        error = L"Could not create HTTP request.";
        return false;
    }
    std::wstring headers = L"Accept: application/json\r\n";
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
    constexpr char kSystem[] =
        "You are Hoverdock Search. Pick the best installed app for the user request, "
        "or a filesystem path to open. Reply with ONLY compact JSON: "
        "{\"id\":\"cN\"} to launch candidate id, {\"id\":\"none\"} if nothing fits, "
        "or {\"path\":\"C:\\\\...\"} for a local file/folder. Prefer primary-purpose matches "
        "(browser vs editor with AI). Never explain. Thinking off.";

    std::string body = "{\"model\":\"local\",\"temperature\":0,\"max_tokens\":80,"
                       "\"chat_template_kwargs\":{\"enable_thinking\":false},"
                       "\"reasoning_format\":\"none\",\"messages\":[";
    body += "{\"role\":\"system\",\"content\":";
    AppendEscaped(body, kSystem);
    body += "},{\"role\":\"user\",\"content\":";
    std::string user = "/no_think\nrequest=";
    user += WideToUtf8(TruncateWide(TrimWide(request), kMaxRequestChars));
    user += "\ncandidates=[";
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i > 0) {
            user += ',';
        }
        AppendCandidate(user, candidates[i]);
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
        LaunchJudgment judgment;
        judgment.action = LaunchJudgment::Action::Launch;
        judgment.openPath = Utf8ToWide(*path);
        judgment.exists = 1.0;
        judgment.confidence = 0.85;
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

    const std::vector<LaunchCandidate> ranked =
        SelectTopCandidates(candidates, cleanRequest, kMaxCandidates);

    if (!IsServerReachable(baseUrl)) {
        return ResolveAppFuzzy(cleanRequest, ranked);
    }

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
