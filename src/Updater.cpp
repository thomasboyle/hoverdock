#include "Updater.h"

#include "Version.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <shellapi.h>
#include <sstream>
#include <vector>

namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr DWORD kConnectTimeoutMs = 8000;
constexpr DWORD kSendTimeoutMs = 15000;
constexpr DWORD kReceiveTimeoutMs = 30000;
constexpr DWORD kMaxDownloadBytes = 256U * 1024U * 1024U;  // 256 MB sanity cap.

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

bool HttpGet(const std::wstring& host, INTERNET_PORT port, bool secure, const std::wstring& path,
    const std::wstring& extraHeaders, std::string& body, DWORD maxBytes, std::wstring& error) {
    WinHttpHandle session(WinHttpOpen(L"Hoverdock/1.0 (auto-update)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = L"Could not open an HTTPS session.";
        return false;
    }
    WinHttpSetTimeouts(session.Get(), kConnectTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
        kReceiveTimeoutMs);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
        sizeof(redirectPolicy));

    WinHttpHandle connection(WinHttpConnect(session.Get(), host.c_str(), port, 0));
    if (!connection) {
        error = L"Could not reach the update server.";
        return false;
    }
    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connection.Get(), L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) {
        error = L"Could not create the update request.";
        return false;
    }
    DWORD redirectCount = 0;
    constexpr DWORD kMaxRedirects = 5;
    for (;;) {
        std::wstring headers = L"User-Agent: Hoverdock/1.0\r\nAccept: */*";
        if (!extraHeaders.empty()) {
            headers += L"\r\n" + extraHeaders;
        }
        if (WinHttpSendRequest(request.Get(), headers.c_str(), static_cast<DWORD>(-1L),
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE) {
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
        if ((status == 301 || status == 302 || status == 303 || status == 307 ||
                status == 308) &&
            redirectCount < kMaxRedirects) {
            wchar_t location[4096]{};
            DWORD locationSize = sizeof(location);
            if (WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_LOCATION,
                    WINHTTP_HEADER_NAME_BY_INDEX, location, &locationSize,
                    WINHTTP_NO_HEADER_INDEX) == FALSE) {
                error = L"Update redirect had no location.";
                return false;
            }
            std::wstring nextHost;
            std::wstring nextPath;
            INTERNET_PORT nextPort = INTERNET_DEFAULT_HTTPS_PORT;
            bool nextSecure = true;
            if (!CrackUrl(WideToUtf8Simple(location), nextHost, nextPath, nextPort,
                    nextSecure)) {
                error = L"Update redirect URL was invalid.";
                return false;
            }
            // Follow manually so host changes (api.github.com ->
            // objects.githubusercontent.com) are honored.
            WinHttpHandle nextConnection(
                WinHttpConnect(session.Get(), nextHost.c_str(), nextPort, 0));
            if (!nextConnection) {
                error = L"Could not follow the update redirect.";
                return false;
            }
            const DWORD nextFlags = nextSecure ? WINHTTP_FLAG_SECURE : 0;
            WinHttpHandle nextRequest(WinHttpOpenRequest(nextConnection.Get(), L"GET",
                nextPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                nextFlags));
            if (!nextRequest) {
                error = L"Could not follow the update redirect.";
                return false;
            }
            // Transfer ownership for the next loop iteration.
            connection = std::move(nextConnection);
            request = std::move(nextRequest);
            ++redirectCount;
            continue;
        }
        if (status < 200 || status > 299) {
            error = L"Update check failed (" + std::to_wstring(status) + L").";
            return false;
        }
        return ReadResponseBody(request.Get(), body, maxBytes, error);
    }
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
    std::wstring path = L"/repos/";
    path += Utf8ToWideSimple(DockVersion::kRepoOwner);
    path += L"/";
    path += Utf8ToWideSimple(DockVersion::kRepoName);
    path += L"/releases/latest";

    std::string body;
    if (!HttpGet(kApiHost, INTERNET_DEFAULT_HTTPS_PORT, true, path,
            L"Accept: application/vnd.github+json", body, 1024U * 1024U, error)) {
        return false;
    }
    if (body.empty()) {
        error = L"Update server returned an empty release.";
        return false;
    }

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

    if (out.version.empty()) {
        error = L"Release had no version.";
        return false;
    }
    if (out.setupUrl.empty() && out.exeUrl.empty()) {
        error = L"Release has no downloadable .exe asset.";
        return false;
    }
    out.hasUpdate = IsNewerVersion(out.version, CurrentVersion());
    return true;
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
    if (!HttpGet(host, port, secure, path, L"", body, kMaxDownloadBytes, error)) {
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

bool Updater::IsInstalledCopy() {
    wchar_t buffer[32768]{};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0) {
        return false;
    }
    std::wstring path(buffer, length);
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return lower.find(L"\\programs\\hoverdock\\") != std::wstring::npos ||
        lower.find(L"\\program files\\") != std::wstring::npos ||
        lower.find(L"\\program files (x86)\\") != std::wstring::npos;
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
    const std::wstring batch = TempDirectory() + L"\\hoverdock-update.bat";
    // Wait for the dock to exit (PID gate), replace the binary, relaunch, and
    // delete the helper. /F fallback covers a hung exit.
    const DWORD pid = GetCurrentProcessId();
    std::wstring script =
        L"@echo off\r\n"
        L"tasklist /FI \"PID eq " +
        std::to_wstring(pid) +
        L"\" | find \"" + std::to_wstring(pid) +
        L"\" >nul\r\n"
        L"if not errorlevel 1 (\r\n"
        L"  timeout /t 1 /nobreak >nul\r\n"
        L"  tasklist /FI \"PID eq " +
        std::to_wstring(pid) + L"\" | find \"" + std::to_wstring(pid) +
        L"\" >nul\r\n"
        L"  if not errorlevel 1 timeout /t 2 /nobreak >nul\r\n"
        L")\r\n"
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
