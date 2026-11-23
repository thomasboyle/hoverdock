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
    constexpr size_t kMaxResponseBytes = 4U * 1024U * 1024U;
    while (true) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request.Get(), &available) == FALSE) {
            error = L"TypeSafe response was truncated.";
            return false;
        }
        if (available == 0) {
            break;
        }
        // `available` is server-driven: cap growth so a hostile or broken
        // response cannot force a huge allocation and terminate the caller.
        if (static_cast<size_t>(available) > kMaxResponseBytes ||
            response.size() > kMaxResponseBytes - static_cast<size_t>(available)) {
            error = L"TypeSafe response was too large.";
            return false;
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

void AppendCandidateIdentity(std::string& out, const BoostCandidate& candidate) {
    out += '{';
    AppendEscaped(out, "id");
    out += ':';
    AppendEscaped(out, candidate.id);
    out += ',';
    AppendUtf8Field(out, "name", candidate.name);
    out += ',';
    AppendUtf8Field(out, "exe", candidate.exePath);
}

void AppendBoostProcess(std::string& out, const BoostCandidate& candidate) {
    AppendCandidateIdentity(out, candidate);
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

void AppendStartupBoostApp(std::string& out, const BoostCandidate& candidate) {
    AppendCandidateIdentity(out, candidate);
    out += ",\"running\":";
    out += candidate.ageSeconds > 0 || candidate.memoryMb > 0.0 || candidate.hasWindow
        ? "true"
        : "false";
    out += ",\"has_window\":";
    out += candidate.hasWindow ? "true" : "false";
    out += ",\"memory_mb\":";
    AppendJsonDouble(out, candidate.memoryMb);
    out += ",\"cpu_pct\":";
    AppendJsonDouble(out, candidate.cpuPercent);
    out += '}';
}

std::string BeginBoostRequestBody(std::string_view surface, std::string_view focus,
    std::string_view policy, std::string_view listKey) {
    std::string body = "{\"model\":\"jev-latest\",\"state\":{\"surface\":";
    AppendEscaped(body, surface);
    body += ",\"focus\":";
    AppendEscaped(body, focus);
    body += ",\"policy\":";
    AppendEscaped(body, policy);
    body += ',';
    AppendEscaped(body, listKey);
    body += ":[";
    return body;
}

void AppendNoulQuestion(std::string& body, std::string_view id, std::string_view instructions,
    std::string_view criteriaTrue, std::string_view criteriaFalse) {
    AppendEscaped(body, id);
    body += ":{\"type\":\"noul\",\"instructions\":";
    AppendEscaped(body, instructions);
    body += ",\"criteria\":{\"true\":";
    AppendEscaped(body, criteriaTrue);
    body += ",\"false\":";
    AppendEscaped(body, criteriaFalse);
    body += "}}";
}

std::string BuildBoostRequestBody(const std::vector<BoostCandidate>& candidates) {
    std::string body = BeginBoostRequestBody(
        "Windows Quick Settings Boost performance in a desktop dock. Code enumerated "
        "candidate processes with memory, CPU, window, and age. Code enforces its own "
        "denylist and owns all closing; Jev only supplies safe/idle probabilities.",
        "Keep the foreground app and important user work fast by reclaiming memory "
        "from idle background work that is safe to close.",
        "Never approve closing operating-system components, drivers, security "
        "software, shell hosts, console hosts, or anything holding important unsaved "
        "work. When the evidence is ambiguous, answer no.",
        "processes");
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
        AppendNoulQuestion(body, "safe_" + candidates[index].id,
            "Is " + ref + " safe to close right now? `policy` forbids harming Windows "
            "stability, drivers, security software, shell hosts, or important unsaved "
            "user work. Answer yes only when the process is ordinary user software "
            "whose close is harmless or recoverable by reopening the app.",
            "Ordinary user app or background agent; closing is harmless or recoverable.",
            "OS, driver, security, shell, or actively important work; closing risks "
            "stability or data loss.");
        body += ',';
        AppendNoulQuestion(body, "idle_" + candidates[index].id,
            "Is " + ref + " idle background work rather than actively used? An empty "
            "`window_title` means it shows no window. Near-zero `cpu_pct` with a large "
            "`age_min` suggests idle. A `window_title` naming media, meetings, calls, "
            "downloads, gaming, recording, or an edited document means in use even at "
            "low CPU.",
            "No window and near-zero CPU, or a background agent with no sign of use.",
            "Has a window suggesting current use, notable CPU, or just started.");
    }
    body += "}}";
    return body;
}


std::string BuildStartupBoostRequestBody(const std::vector<BoostCandidate>& candidates) {
    std::string body = BeginBoostRequestBody(
        "Windows Quick Settings Boost performance in a desktop dock. Code enumerated "
        "currently enabled Windows startup apps (Run keys and Startup folder). Code "
        "hard-protects only the dock itself and Windows SecurityHealthSystray; Jev "
        "decides which other startup entries are safe to disable via StartupApproved "
        "and end if running. Code owns every registry write and process end.",
        "Speed up logon and free RAM by disabling non-essential startup apps that are "
        "safe to turn off for a gaming/performance boost, without harming security or "
        "the shell.",
        "Never approve disabling operating-system security, shell hosts, accessibility "
        "helpers the user relies on, disk encryption agents, or anything whose absence "
        "would break Windows stability. When evidence is ambiguous, answer no. Chat "
        "apps, browsers auto-launch, game launchers, and vendor updaters are typical "
        "yes candidates when nothing indicates they are critical.",
        "startup_apps");
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) {
            body += ',';
        }
        AppendStartupBoostApp(body, candidates[index]);
    }
    body += "]},\"questions\":{";
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) {
            body += ',';
        }
        const std::string ref = "`startup_apps[" + std::to_string(index) + "]`";
        AppendNoulQuestion(body, "safe_" + candidates[index].id,
            "Is " + ref + " safe to disable from Windows startup right now for a "
            "performance boost? `policy` forbids harming Windows security, shell "
            "stability, or critical system agents. Answer yes only for ordinary user "
            "software that can be re-enabled later without risk.",
            "Ordinary chat, browser, game launcher, updater, or similar; safe to disable.",
            "OS security, shell, accessibility, encryption, or other critical agent.");
        body += ',';
        AppendNoulQuestion(body, "idle_" + candidates[index].id,
            "Is " + ref + " non-essential for day-to-day system health — i.e. optional "
            "background/chat/browser/game-launcher startup rather than something the "
            "user likely needs at every logon for security or core workflow?",
            "Optional launcher, chat, browser auto-start, or vendor updater.",
            "Likely important tray/security/workflow agent the user would miss.");
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

BoostResult TypeSafeClient::ClassifyForStartupBoost(const std::wstring& apiKey,
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
        const NoulBatchResult reply =
            PostNoulBatch(cleanKey, BuildStartupBoostRequestBody(batch));
        if (!reply.ok) {
            result.error = reply.error;
            return result;
        }
        for (const BoostCandidate& candidate : batch) {
            const auto safe = reply.values.find("safe_" + candidate.id);
            const auto idle = reply.values.find("idle_" + candidate.id);
            if (safe == reply.values.end() || idle == reply.values.end()) {
                continue;
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
