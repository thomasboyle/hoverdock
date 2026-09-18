#include "Updater.h"

#include "Version.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <shellapi.h>
#include <sstream>
#include <vector>

namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kWebHost[] = L"github.com";
constexpr DWORD kConnectTimeoutMs = 8000;
constexpr DWORD kSendTimeoutMs = 15000;
constexpr DWORD kReceiveTimeoutMs = 30000;
constexpr DWORD kMaxDownloadBytes = 256U * 1024U * 1024U;  // 256 MB sanity cap.
constexpr DWORD kMaxApiBytes = 1024U * 1024U;
constexpr DWORD kMaxErrorBodyBytes = 64U * 1024U;

std::string WideToUtf8Simple(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count,
        nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWideSimple(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string TrimVersion(std::string version) {
    while (!version.empty() &&
        (std::isspace(static_cast<unsigned char>(version.front())) != 0)) {
        version.erase(version.begin());
    }
    while (!version.empty() &&
        (std::isspace(static_cast<unsigned char>(version.back())) != 0)) {
        version.pop_back();
    }
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) {
        version.erase(version.begin());
    }
    return version;
}

struct VersionParts {
    std::vector<unsigned long> numbers;
    std::string prerelease;
};

VersionParts SplitVersion(const std::string& version) {
    VersionParts parts;
    std::string core = version;
    const size_t dash = core.find('-');
    if (dash != std::string::npos) {
        parts.prerelease = core.substr(dash + 1);
        core = core.substr(0, dash);
    }
    // Drop build metadata (+...) for comparison.
    const size_t plus = core.find('+');
    if (plus != std::string::npos) {
        core = core.substr(0, plus);
    }
    std::istringstream stream(core);
    std::string token;
    while (std::getline(stream, token, '.')) {
        try {
            parts.numbers.push_back(token.empty() ? 0UL : std::stoul(token));
        } catch (...) {
            parts.numbers.push_back(0UL);
        }
    }
    return parts;
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ToLowerContains(const std::string& haystack, const char* needle);

// Minimal JSON string-field scan: finds "key" : "value" without a full parser.
// GitHub release payloads are flat enough for this; values never contain
// escaped quotes in tag_name / browser_download_url / html_url.
bool FindJsonStringField(const std::string& body, size_t& cursor, const std::string& key,
    std::string& value) {
    const std::string quoted = "\"" + key + "\"";
    const size_t found = body.find(quoted, cursor);
    if (found == std::string::npos) {
        return false;
    }
    size_t colon = body.find(':', found + quoted.size());
    if (colon == std::string::npos) {
        return false;
    }
    ++colon;
    while (colon < body.size() &&
        (body[colon] == ' ' || body[colon] == '\t' || body[colon] == '\r' ||
            body[colon] == '\n')) {
        ++colon;
    }
    if (colon >= body.size() || body[colon] != '"') {
        cursor = colon;
        return false;
    }
    ++colon;
    std::string out;
    for (size_t i = colon; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            out.push_back(body[i + 1]);
            ++i;
            continue;
        }
        if (c == '"') {
            value = out;
            cursor = i + 1;
            return true;
        }
        out.push_back(c);
    }
    return false;
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

bool ReadResponseBody(HINTERNET request, std::string& body, DWORD maxBytes,
    std::wstring& error) {
    body.clear();
    while (true) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request, &available) == FALSE) {
            error = L"Update response was truncated.";
            return false;
        }
        if (available == 0) {
            return true;
        }
        if (body.size() + available > maxBytes) {
            error = L"Update download is unexpectedly large.";
            return false;
        }
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (WinHttpReadData(request, body.data() + offset, available, &read) == FALSE) {
            error = L"Update response could not be read.";
            return false;
        }
        body.resize(offset + read);
        if (read == 0) {
            return true;
        }
    }
}

bool CrackUrl(const std::string& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port,
    bool& secure) {
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t hostName[256]{};
    wchar_t urlPath[4096]{};
    parts.lpszHostName = hostName;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(hostName));
    parts.lpszUrlPath = urlPath;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(urlPath));
    const std::wstring wide = Utf8ToWideSimple(url);
    if (wide.empty()) {
        return false;
    }
    if (WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) == FALSE) {
        return false;
    }
    host.assign(hostName);
    path.assign(urlPath);
    port = parts.nPort;
    secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (path.empty()) {
        path = L"/";
    }
    return !host.empty();
}

std::wstring UserAgent() {
    std::string version = TrimVersion(DockVersion::kVersion);
    if (version.empty()) {
        version = "1.0";
    }
    return Utf8ToWideSimple("Hoverdock/" + version + " (auto-update)");
}

bool QueryCustomHeader(HINTERNET request, const wchar_t* name, std::wstring& value) {
    value.clear();
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, WINHTTP_NO_OUTPUT_BUFFER, &size,
        WINHTTP_NO_HEADER_INDEX);
    if (size == 0 || size > 16U * 1024U) {
        return false;
    }
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, buffer.data(), &size,
            WINHTTP_NO_HEADER_INDEX) == FALSE) {
        return false;
    }
    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    value = buffer;
    return !value.empty();
}

bool QueryLocationHeader(HINTERNET request, std::wstring& location) {
    location.clear();
    wchar_t buffer[4096]{};
    DWORD size = sizeof(buffer);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
            buffer, &size, WINHTTP_NO_HEADER_INDEX) == FALSE) {
        return false;
    }
    location.assign(buffer);
    return !location.empty();
}

std::wstring TruncateWide(std::wstring text, size_t maxChars) {
    if (text.size() > maxChars) {
        text.resize(maxChars);
    }
    for (wchar_t& c : text) {
        if (c < 0x20 && c != L'\n' && c != L'\r' && c != L'\t') {
            c = L' ';
        }
    }
    return text;
}

// Builds a user-facing HTTP error that surfaces GitHub's JSON "message" and,
// for rate limiting, the reset time. `contextNoun` is "check" or "download".
std::wstring FormatHttpError(DWORD status, const std::string& body, HINTERNET request,
    const std::wstring& contextNoun) {
    std::wstring prefix =
        contextNoun == L"download" ? L"Update download failed " : L"Update check failed ";
    const std::wstring statusText = L"(" + std::to_wstring(status) + L")";

    std::string serverMessage;
    if (!body.empty()) {
        size_t cursor = 0;
        FindJsonStringField(body, cursor, "message", serverMessage);
    }

    const std::string lowerMessage = ToLowerAscii(serverMessage);
    const bool messageSaysRateLimit = lowerMessage.find("rate limit") != std::string::npos ||
        lowerMessage.find("abuse") != std::string::npos;

    std::wstring remaining;
    std::wstring reset;
    std::wstring retryAfter;
    if (request != nullptr) {
        QueryCustomHeader(request, L"X-RateLimit-Remaining", remaining);
        QueryCustomHeader(request, L"X-RateLimit-Reset", reset);
        QueryCustomHeader(request, L"Retry-After", retryAfter);
    }
    const bool rateLimited = status == 429 || messageSaysRateLimit ||
        (status == 403 && (remaining == L"0" || messageSaysRateLimit));
    if (rateLimited) {
        // X-RateLimit-Reset is unix seconds; Retry-After is seconds to wait.
        long waitMinutes = -1;
        try {
            if (!retryAfter.empty()) {
                const long seconds = std::stol(retryAfter);
                if (seconds >= 0) {
                    waitMinutes = (seconds + 59) / 60;
                }
            } else if (!reset.empty()) {
                const long long resetUnix = std::stoll(reset);
                const long long nowUnix = static_cast<long long>(std::time(nullptr));
                const long long delta = resetUnix - nowUnix;
                if (delta > 0 && delta < 24LL * 3600LL) {
                    waitMinutes = static_cast<long>((delta + 59) / 60);
                }
            }
        } catch (...) {
            waitMinutes = -1;
        }
        if (waitMinutes >= 0) {
            return prefix + statusText +
                L": GitHub update rate limit exceeded, try again in " +
                std::to_wstring(waitMinutes + 1) + L" min.";
        }
        return prefix + statusText + L": GitHub update rate limit exceeded, try again later.";
    }

    if (!serverMessage.empty()) {
        std::wstring wide = Utf8ToWideSimple(serverMessage);
        if (!wide.empty()) {
            wide = TruncateWide(wide, 160);
            return prefix + statusText + L": " + wide;
        }
    }
    if (status == 404) {
        return prefix + statusText + L": no releases published yet.";
    }
    return prefix + statusText + L".";
}

struct HttpResult {
    DWORD status = 0;
    std::string body;
    std::wstring location;  // Set when the final response is a redirect.
};

// Core HTTPS GET. When `followRedirects` is false the first 3xx response is
// returned (with `location`) instead of being followed; this is how the
// rate-limit-free github.com/releases/latest probe resolves the tag.
bool HttpGetEx(const std::wstring& host, INTERNET_PORT port, bool secure,
    const std::wstring& path, const std::wstring& extraHeaders, DWORD maxBytes,
    bool followRedirects, HttpResult& result, std::wstring& error,
    const std::wstring& contextNoun) {
    result = HttpResult{};
    const std::wstring agent = UserAgent();
    WinHttpHandle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = L"Could not open an HTTPS session.";
        return false;
    }
    WinHttpSetTimeouts(session.Get(), kConnectTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
        kReceiveTimeoutMs);
    // GitHub requires TLS 1.2+; pin the floor explicitly so older OS defaults
    // cannot negotiate TLS 1.0/1.1 and fail the handshake.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(
        session.Get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    // Follow redirects manually so host changes (api.github.com ->
    // objects.githubusercontent.com, github.com -> release assets) are
    // honored and so the version probe can observe Location without fetching
    // HTML. Disable WinHTTP's automatic handling to avoid double-follows.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
        sizeof(redirectPolicy));

    std::wstring curHost = host;
    INTERNET_PORT curPort = port;
    bool curSecure = secure;
    std::wstring curPath = path;

    DWORD redirectCount = 0;
    constexpr DWORD kMaxRedirects = 5;
    for (;;) {
        WinHttpHandle connection(WinHttpConnect(session.Get(), curHost.c_str(), curPort, 0));
        if (!connection) {
            error = L"Could not reach the update server.";
            return false;
        }
        const DWORD flags = curSecure ? WINHTTP_FLAG_SECURE : 0;
        WinHttpHandle request(WinHttpOpenRequest(connection.Get(), L"GET", curPath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!request) {
            error = L"Could not create the update request.";
            return false;
        }
        const wchar_t* headersPtr = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                         : extraHeaders.c_str();
        DWORD headersLen = extraHeaders.empty() ? 0 : static_cast<DWORD>(-1L);
        if (WinHttpSendRequest(request.Get(), headersPtr, headersLen, WINHTTP_NO_REQUEST_DATA,
                0, 0, 0) == FALSE) {
            error = L"Could not send the update request.";
            return false;
        }
        if (WinHttpReceiveResponse(request.Get(), nullptr) == FALSE) {
            error = L"The update server did not respond.";
            return false;
        }
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (WinHttpQueryHeaders(request.Get(),
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                WINHTTP_NO_HEADER_INDEX) == FALSE) {
            error = L"Update server returned an unreadable status.";
            return false;
        }
        result.status = status;
        const bool isRedirect =
            status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (isRedirect) {
            std::wstring location;
            if (!QueryLocationHeader(request.Get(), location)) {
                error = L"Update redirect had no location.";
                return false;
            }
            if (!followRedirects || redirectCount >= kMaxRedirects) {
                result.location = location;
                return true;
            }
            // Resolve relative redirects ("/owner/repo/...") against the
            // current host; absolute URLs may change host/port/scheme.
            if (!location.empty() && location.front() == L'/') {
                curPath = location;
            } else {
                std::wstring nextHost;
                std::wstring nextPath;
                INTERNET_PORT nextPort = INTERNET_DEFAULT_HTTPS_PORT;
                bool nextSecure = true;
                if (!CrackUrl(WideToUtf8Simple(location), nextHost, nextPath, nextPort,
                        nextSecure)) {
                    error = L"Update redirect URL was invalid.";
                    return false;
                }
                curHost = nextHost;
                curPath = nextPath;
                curPort = nextPort;
                curSecure = nextSecure;
            }
            ++redirectCount;
            continue;
        }
        if (status < 200 || status > 299) {
            // Capture GitHub's JSON error body (rate-limit message, abuse
            // notice, ...) so the user sees more than a bare status code.
            std::string errorBody;
            std::wstring ignored;
            if (ReadResponseBody(request.Get(), errorBody, kMaxErrorBodyBytes, ignored)) {
                result.body = errorBody;
            }
            error = FormatHttpError(status, result.body, request.Get(), contextNoun);
            return false;
        }
        return ReadResponseBody(request.Get(), result.body, maxBytes, error);
    }
}

bool HttpGet(const std::wstring& host, INTERNET_PORT port, bool secure, const std::wstring& path,
    const std::wstring& extraHeaders, std::string& body, DWORD maxBytes, std::wstring& error) {
    HttpResult result;
    // Only the JSON API is a "check"; release-asset downloads (which also live
    // under /releases/download/...) must report as downloads.
    const bool isApi = ToLowerContains(WideToUtf8Simple(host), "api.github.com");
    if (!HttpGetEx(host, port, secure, path, extraHeaders, maxBytes, true, result, error,
            isApi ? L"check" : L"download")) {
        return false;
    }
    body = result.body;
    return true;
}

std::wstring ApiHeaders() {
    // Single Accept header (a duplicated "Accept: */*" + json line confuses
    // content negotiation) plus the pinned API version GitHub recommends.
    return L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28";
}

// Resolves the latest tag without touching the rate-limited JSON API:
// GET https://github.com/<owner>/<repo>/releases/latest returns
// 302 Location: .../releases/tag/<tag>. No auth, no API quota.
bool ResolveLatestViaWebRedirect(
    std::string& tagOut, std::string& versionOut, std::string& pageUrlOut, std::wstring& error) {
    tagOut.clear();
    versionOut.clear();
    pageUrlOut.clear();
    std::wstring path = L"/";
    path += Utf8ToWideSimple(DockVersion::kRepoOwner);
    path += L"/";
    path += Utf8ToWideSimple(DockVersion::kRepoName);
    path += L"/releases/latest";

    HttpResult result;
    if (!HttpGetEx(kWebHost, INTERNET_DEFAULT_HTTPS_PORT, true, path,
            L"Accept: text/html,*/*;q=0.8", kMaxErrorBodyBytes, false, result, error,
            L"check")) {
        return false;
    }
    if (result.status == 404) {
        error = L"Update check failed (404): no releases published yet.";
        return false;
    }
    const bool isRedirect = result.status == 301 || result.status == 302 ||
        result.status == 303 || result.status == 307 || result.status == 308;
    if (!isRedirect) {
        error = L"Update check failed (" + std::to_wstring(result.status) + L").";
        return false;
    }
    if (result.location.empty()) {
        error = L"Update redirect had no location.";
        return false;
    }
    const std::string location = WideToUtf8Simple(result.location);
    std::string clean = location;
    const size_t hash = clean.find('#');
    if (hash != std::string::npos) {
        clean.resize(hash);
    }
    const size_t query = clean.find('?');
    if (query != std::string::npos) {
        clean.resize(query);
    }
    const std::string marker = "/tag/";
    const size_t markerPos = clean.rfind(marker);
    if (markerPos == std::string::npos) {
        error = L"Update redirect URL was invalid.";
        return false;
    }
    std::string tag = clean.substr(markerPos + marker.size());
    while (!tag.empty() && tag.back() == '/') {
        tag.pop_back();
    }
    if (tag.empty()) {
        error = L"Update redirect URL was invalid.";
        return false;
    }
    tagOut = tag;
    versionOut = TrimVersion(tag);
    pageUrlOut = location;
    if (versionOut.empty()) {
        error = L"Release had no version.";
        return false;
    }
    return true;
}

// Synthesizes release-asset URLs from the known release.yml naming
// (Hoverdock-Setup-<version>.exe + Dock.exe). Used when the JSON API is
// rate-limited: github.com download URLs carry no API quota.
void BuildFallbackRelease(
    const std::string& tag, const std::string& version, const std::string& pageUrl,
    Updater::ReleaseInfo& out) {
    out = Updater::ReleaseInfo{};
    out.version = version;
    out.pageUrl = pageUrl.empty()
        ? std::string("https://github.com/") + DockVersion::kRepoOwner + "/" +
            DockVersion::kRepoName + "/releases/tag/" + tag
        : pageUrl;
    const std::string base = std::string("https://github.com/") + DockVersion::kRepoOwner +
        "/" + DockVersion::kRepoName + "/releases/download/" + tag + "/";
    out.setupUrl = base + "Hoverdock-Setup-" + version + ".exe";
    out.exeUrl = base + "Dock.exe";
}

bool ParseApiReleaseBody(const std::string& body, Updater::ReleaseInfo& out) {
    out = Updater::ReleaseInfo{};
    size_t cursor = 0;
    std::string tag;
    if (FindJsonStringField(body, cursor, "tag_name", tag)) {
        out.version = TrimVersion(tag);
    }
    std::string page;
    size_t pageCursor = 0;
    if (FindJsonStringField(body, pageCursor, "html_url", page)) {
        out.pageUrl = page;
    }

    // Collect every asset URL, then prefer a Setup installer.
    size_t assetCursor = 0;
    std::string url;
    std::vector<std::string> urls;
    while (FindJsonStringField(body, assetCursor, "browser_download_url", url)) {
        urls.push_back(url);
    }
    for (const std::string& candidate : urls) {
        if (ToLowerContains(candidate, ".exe") && ToLowerContains(candidate, "setup")) {
            if (out.setupUrl.empty()) {
                out.setupUrl = candidate;
            }
        }
    }
    for (const std::string& candidate : urls) {
        const std::string lower = ToLowerAscii(candidate);
        const bool isExe = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0;
        if (!isExe) {
            continue;
        }
        if (!out.setupUrl.empty()) {
            break;
        }
        // No Setup asset: accept Dock.exe / Hoverdock.exe as portable payload.
        if (ToLowerContains(candidate, "dock")) {
            out.exeUrl = candidate;
            break;
        }
    }
    if (out.setupUrl.empty() && out.exeUrl.empty()) {
        // Last resort: any .exe asset so a renamed uploader still updates.
        for (const std::string& candidate : urls) {
            if (ToLowerContains(candidate, ".exe")) {
                out.exeUrl = candidate;
                break;
            }
        }
    }
    return !out.version.empty() && (!out.setupUrl.empty() || !out.exeUrl.empty());
}

std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    if (length == 0 || length >= std::size(buffer)) {
        return L".";
    }
    std::wstring dir(buffer, length);
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
        dir.pop_back();
    }
    return dir.empty() ? L"." : dir;
}

bool ToLowerContains(const std::string& haystack, const char* needle) {
    return ToLowerAscii(haystack).find(needle) != std::string::npos;
}

bool EqualInsensitivePath(const std::wstring& left, const std::wstring& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        wchar_t x = left[i] == L'/' ? L'\\' : left[i];
        wchar_t y = right[i] == L'/' ? L'\\' : right[i];
        if (std::towlower(x) != std::towlower(y)) {
            return false;
        }
    }
    return true;
}

std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* valueName) {
    DWORD type = 0;
    wchar_t buffer[32768]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, subkey, valueName, RRF_RT_REG_SZ, &type, buffer, &size) !=
            ERROR_SUCCESS ||
        type != REG_SZ) {
        return {};
    }
    // size includes the nul terminator; guard against a missing one.
    const size_t chars = size >= sizeof(wchar_t) ? (size / sizeof(wchar_t)) - 1U : 0U;
    return std::wstring(buffer, (std::min)(chars, std::size(buffer) - 1U));
}

}  // namespace

std::string Updater::CurrentVersion() {
    return TrimVersion(DockVersion::kVersion);
}

bool Updater::IsNewerVersion(const std::string& latest, const std::string& current) {
    const std::string left = TrimVersion(latest);
    const std::string right = TrimVersion(current);
    if (left.empty() || right.empty() || left == right) {
        return false;
    }
    const VersionParts l = SplitVersion(left);
    const VersionParts r = SplitVersion(right);
    const size_t count = (std::max)(l.numbers.size(), r.numbers.size());
    for (size_t i = 0; i < count; ++i) {
        const unsigned long lv = i < l.numbers.size() ? l.numbers[i] : 0UL;
        const unsigned long rv = i < r.numbers.size() ? r.numbers[i] : 0UL;
        if (lv != rv) {
            return lv > rv;
        }
    }
    // Same numbers: bare release beats any pre-release; otherwise lexical.
    if (l.prerelease.empty() != r.prerelease.empty()) {
        return r.prerelease.empty() == false && l.prerelease.empty();
    }
    return l.prerelease > r.prerelease;
}

bool Updater::FetchLatestRelease(ReleaseInfo& out, std::wstring& error) {
    out = ReleaseInfo{};
    const std::string current = CurrentVersion();

    // Prefer the rate-limit-free github.com redirect for version resolution:
    // unauthenticated api.github.com calls are capped at 60/hour per IP and
    // shared NATs exhaust that quota, surfacing as HTTP 403.
    std::string webTag;
    std::string webVersion;
    std::string webPage;
    std::wstring webError;
    const bool webOk = ResolveLatestViaWebRedirect(webTag, webVersion, webPage, webError);

    auto fetchViaApi = [&](ReleaseInfo& apiOut, std::wstring& apiError) -> bool {
        apiOut = ReleaseInfo{};
        std::wstring apiPath = L"/repos/";
        apiPath += Utf8ToWideSimple(DockVersion::kRepoOwner);
        apiPath += L"/";
        apiPath += Utf8ToWideSimple(DockVersion::kRepoName);
        apiPath += L"/releases/latest";

        std::string body;
        if (!HttpGet(kApiHost, INTERNET_DEFAULT_HTTPS_PORT, true, apiPath, ApiHeaders(), body,
                kMaxApiBytes, apiError)) {
            return false;
        }
        if (body.empty()) {
            apiError = L"Update server returned an empty release.";
            return false;
        }
        if (!ParseApiReleaseBody(body, apiOut)) {
            if (apiOut.version.empty()) {
                apiError = L"Release had no version.";
            } else {
                apiError = L"Release has no downloadable .exe asset.";
            }
            return false;
        }
        apiOut.hasUpdate = IsNewerVersion(apiOut.version, current);
        return true;
    };

    if (webOk) {
        if (!IsNewerVersion(webVersion, current)) {
            // Up to date: no JSON API call needed, so routine checks never
            // consume the 60/hour quota.
            BuildFallbackRelease(webTag, webVersion, webPage, out);
            out.hasUpdate = false;
            return true;
        }
        // A newer tag exists: prefer exact API asset URLs, but fall back to
        // the conventional github.com download URLs when the API is
        // rate-limited (403/429) or unreachable.
        ReleaseInfo apiOut;
        std::wstring apiError;
        if (fetchViaApi(apiOut, apiError)) {
            out = apiOut;
            return true;
        }
        BuildFallbackRelease(webTag, webVersion, webPage, out);
        out.hasUpdate = true;
        return true;
    }

    // Web redirect failed (offline, no releases, unexpected HTML): try the
    // JSON API so proxied setups still have a path.
    ReleaseInfo apiOut;
    std::wstring apiError;
    if (fetchViaApi(apiOut, apiError)) {
        out = apiOut;
        return true;
    }
    // Prefer the more actionable error. A 404 "no releases" from the web
    // probe beats a 403 rate-limit from the API, and vice versa.
    if (!webError.empty() &&
        (webError.find(L"no releases") != std::wstring::npos || apiError.empty())) {
        error = webError;
    } else {
        error = apiError.empty() ? webError : apiError;
    }
    if (error.empty()) {
        error = L"Update check failed.";
    }
    return false;
}

// Picks the download asset for THIS copy. Installed copies (LocalAppData\
// Programs\Hoverdock, Program Files) update via the NSIS installer, which
// replaces the install dir and relaunches it. Portable copies must
// self-replace in place: running the installer would plant a second copy in
// the install dir while the running portable binary stays old, so the next
// check re-offers the same version forever.
void Updater::SelectAssetUrls(const ReleaseInfo& release, std::string& urlOut, bool& isSetupOut) {
    urlOut.clear();
    isSetupOut = false;
    const bool installed = IsInstalledCopy();
    if (installed) {
        if (!release.setupUrl.empty()) {
            urlOut = release.setupUrl;
            isSetupOut = true;
            return;
        }
        urlOut = release.exeUrl;
        isSetupOut = false;
        return;
    }
    if (!release.exeUrl.empty()) {
        urlOut = release.exeUrl;
        isSetupOut = false;
        return;
    }
    urlOut = release.setupUrl;
    isSetupOut = !release.setupUrl.empty();
}

bool Updater::DownloadFile(const std::string& url, const std::wstring& destPath,
    std::wstring& error) {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
    if (!CrackUrl(url, host, path, port, secure)) {
        error = L"Update URL was invalid.";
        return false;
    }
    std::string body;
    if (!HttpGet(host, port, secure, path, L"Accept: */*", body, kMaxDownloadBytes, error)) {
        return false;
    }
    if (body.empty()) {
        error = L"Update download was empty.";
        return false;
    }
    std::ofstream file(destPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"Could not write the update file.";
        return false;
    }
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    file.close();
    if (!file) {
        error = L"Could not save the update file.";
        DeleteFileW(destPath.c_str());
        return false;
    }
    return true;
}

std::wstring Updater::DefaultDownloadPath(const std::string& version, bool isSetup) {
    std::wstring name = L"Hoverdock-Update-";
    name += Utf8ToWideSimple(TrimVersion(version).empty() ? std::string("latest")
                                                          : TrimVersion(version));
    name += isSetup ? L"-Setup.exe" : L".exe";
    for (wchar_t& c : name) {
        if (c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
            c == L'<' || c == L'>' || c == L'|') {
            c = L'_';
        }
    }
    return TempDirectory() + L"\\" + name;
}

bool Updater::IsSamePath(const std::wstring& left, const std::wstring& right) {
    return EqualInsensitivePath(left, right);
}

std::wstring Updater::CurrentExecutablePath() {
    wchar_t buffer[32768]{};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0) {
        return {};
    }
    return std::wstring(buffer, length);
}

Updater::InstalledCopy Updater::InstalledCopyInfo() {
    InstalledCopy info;
    std::wstring dir = ReadRegString(HKEY_CURRENT_USER, L"Software\\Hoverdock", L"InstallDir");
    std::wstring version = ReadRegString(HKEY_CURRENT_USER, L"Software\\Hoverdock", L"Version");
    if (dir.empty()) {
        dir = ReadRegString(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Hoverdock",
            L"InstallLocation");
    }
    if (version.empty()) {
        version = ReadRegString(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Hoverdock",
            L"DisplayVersion");
    }
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
        dir.pop_back();
    }
    if (!dir.empty()) {
        info.exePath = dir + L"\\Dock.exe";
    }
    info.version = TrimVersion(WideToUtf8Simple(version));
    return info;
}

bool Updater::IsInstalledCopy() {
    const std::wstring path = CurrentExecutablePath();
    if (path.empty()) {
        return false;
    }
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    if (lower.find(L"\\programs\\hoverdock\\") != std::wstring::npos ||
        lower.find(L"\\program files\\") != std::wstring::npos ||
        lower.find(L"\\program files (x86)\\") != std::wstring::npos) {
        return true;
    }
    // Custom install directories miss the path heuristic: the registry record
    // written by the installer is authoritative instead.
    const InstalledCopy installed = InstalledCopyInfo();
    return !installed.exePath.empty() && EqualInsensitivePath(path, installed.exePath);
}

bool Updater::LaunchInstallerAndExit(const std::wstring& installerPath) {
    if (installerPath.empty() || GetFileAttributesW(installerPath.c_str()) ==
            INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    // Silent NSIS install (/S) which closes the running dock, replaces files,
    // and relaunches it. Quote the path for ShellExecute.
    HINSTANCE launched = ShellExecuteW(nullptr, nullptr, installerPath.c_str(), L"/S", nullptr,
        SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(launched) > 32;
}

bool Updater::StagePortableUpdateAndRestart(const std::wstring& downloadedExe) {
    wchar_t current[32768]{};
    const DWORD length =
        GetModuleFileNameW(nullptr, current, static_cast<DWORD>(std::size(current)));
    if (length == 0 || downloadedExe.empty()) {
        return false;
    }
    const std::wstring currentPath(current, length);
    if (GetFileAttributesW(downloadedExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    // The helper below is a batch file, which cmd.exe reads in the ANSI code
    // page: non-ASCII bytes in either path would be mojibake and the move
    // would silently target the wrong file (relaunching the old build =
    // an update loop that reports success). Refuse honestly instead.
    auto isAscii = [](const std::wstring& text) {
        for (wchar_t c : text) {
            if (c > 127) {
                return false;
            }
        }
        return true;
    };
    if (!isAscii(downloadedExe) || !isAscii(currentPath)) {
        return false;
    }
    const std::wstring batch = TempDirectory() + L"\\hoverdock-update.bat";
    // Wait for the dock to exit (PID gate), replace the binary, relaunch, and
    // delete the helper. The wait loops (~20 s) instead of a fixed short sleep:
    // replacing Dock.exe while the old process still holds it silently keeps
    // the old version in place, and the relaunch then reports the stale
    // version as if the update had succeeded.
    const DWORD pid = GetCurrentProcessId();
    std::wstring script =
        L"@echo off\r\n"
        L"for /L %%i in (1,1,20) do (\r\n"
        L"  tasklist /FI \"PID eq " +
        std::to_wstring(pid) + L"\" | find \"" + std::to_wstring(pid) +
        L"\" >nul\r\n"
        L"  if errorlevel 1 goto replaced\r\n"
        L"  timeout /t 1 /nobreak >nul\r\n"
        L")\r\n"
        L":replaced\r\n"
        L"move /y \"" +
        downloadedExe + L"\" \"" + currentPath +
        L"\" >nul\r\n"
        L"start \"\" \"" +
        currentPath +
        L"\"\r\n"
        L"del \"%~f0\"\r\n";
    std::string narrow = WideToUtf8Simple(script);
    // Batch files are read in the ANSI code page; paths here are ASCII-safe
    // (TEMP + exe path). Non-ASCII TEMP paths fall back to the installer flow.
    std::ofstream file(batch, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(narrow.data(), static_cast<std::streamsize>(narrow.size()));
    file.close();
    if (!file) {
        return false;
    }
    HINSTANCE launched = ShellExecuteW(nullptr, L"open", batch.c_str(), nullptr, nullptr,
        SW_HIDE);
    return reinterpret_cast<INT_PTR>(launched) > 32;
}
