#include "GroqClient.h"

#include "AiMarkdown.h"
#include "GroqProtocol.h"

#include <objbase.h>
#include <wincodec.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <thread>
#include <utility>

namespace {

constexpr wchar_t kApiHost[] = L"api.groq.com";
constexpr wchar_t kApiPath[] = L"/openai/v1/chat/completions";
constexpr wchar_t kUserAgent[] = L"Hoverdock/1.0";
constexpr DWORD kResolveTimeoutMs = 10000;
constexpr DWORD kConnectTimeoutMs = 10000;
constexpr DWORD kSendTimeoutMs = 20000;
constexpr DWORD kReceiveTimeoutMs = 90000;
constexpr size_t kMaxErrorBody = 64U * 1024U;
constexpr size_t kMaxStreamBytes = 8U * 1024U * 1024U;
constexpr size_t kMaxImageBytes = 8U * 1024U * 1024U;
constexpr UINT kMaxImageSide = 16384;
constexpr ULONGLONG kPublishIntervalMs = 50;
// Tool rounds before the model must answer without tools.
constexpr int kMaxToolRounds = 3;
constexpr size_t kMaxCallsPerRound = 3;

class WinHttpHandle {
public:
    explicit WinHttpHandle(HINTERNET handle = nullptr) noexcept : m_handle(handle) {}
    ~WinHttpHandle() {
        if (m_handle != nullptr) {
            WinHttpCloseHandle(m_handle);
        }
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&&) = delete;
    [[nodiscard]] HINTERNET Get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

private:
    HINTERNET m_handle;
};

struct HttpResult {
    DWORD status = 0;
    std::string errorBody;
    std::wstring error;
    bool cancelled = false;
};

WinHttpHandle OpenSession() {
    WinHttpHandle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0));
    if (session) {
        WinHttpSetTimeouts(session.Get(), static_cast<int>(kResolveTimeoutMs), static_cast<int>(kConnectTimeoutMs),
            static_cast<int>(kSendTimeoutMs), static_cast<int>(kReceiveTimeoutMs));
        // Never hand Windows credentials to arbitrary hosts (image URLs come from the model).
        DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        WinHttpSetOption(session.Get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon));
    }
    return session;
}

// Reads the response body, streaming 2xx bodies to `onData` and buffering
// error bodies into result.errorBody.
bool ReadResponse(HINTERNET request, const std::function<void(std::string_view)>& onData,
    const std::atomic<bool>* cancelled, size_t maxBytes, HttpResult& result) {
    DWORD statusSize = sizeof(result.status);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &statusSize, WINHTTP_NO_HEADER_INDEX) == FALSE) {
        result.error = L"The server returned an unreadable response.";
        return false;
    }
    const bool success = result.status >= 200 && result.status <= 299;
    std::string buffer(16U * 1024U, '\0');
    size_t total = 0;
    while (true) {
        if (cancelled != nullptr && cancelled->load()) {
            result.cancelled = true;
            return false;
        }
        DWORD read = 0;
        if (WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read) == FALSE) {
            result.error = GetLastError() == ERROR_WINHTTP_TIMEOUT ? L"The request timed out."
                                                                   : L"The connection was interrupted.";
            return false;
        }
        if (read == 0) {
            break;
        }
        total += read;
        if (total > maxBytes) {
            result.error = L"The response was too large.";
            return false;
        }
        const std::string_view chunk(buffer.data(), read);
        if (success) {
            onData(chunk);
        } else if (result.errorBody.size() < kMaxErrorBody) {
            result.errorBody.append(chunk.substr(0, kMaxErrorBody - result.errorBody.size()));
        }
    }
    return success;
}

bool PostChat(const std::wstring& apiKey, const std::string& body, const std::function<void(std::string_view)>& onData,
    const std::atomic<bool>& cancelled, HttpResult& result) {
    WinHttpHandle session = OpenSession();
    if (!session) {
        result.error = L"Could not open an HTTPS session.";
        return false;
    }
    WinHttpHandle connection(WinHttpConnect(session.Get(), kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) {
        result.error = L"Could not reach api.groq.com.";
        return false;
    }
    WinHttpHandle request(WinHttpOpenRequest(connection.Get(), L"POST", kApiPath, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        result.error = L"Could not create the request.";
        return false;
    }
    std::wstring headers = L"Authorization: Bearer ";
    for (const wchar_t c : apiKey) {
        if (c > 0x20 && c < 0x7F) {
            headers.push_back(c);
        }
    }
    headers += L"\r\nContent-Type: application/json\r\nAccept: text/event-stream, application/json";
    const bool headersAdded = WinHttpAddRequestHeaders(request.Get(), headers.c_str(), static_cast<DWORD>(-1L),
                                  WINHTTP_ADDREQ_FLAG_ADD) != FALSE;
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!headersAdded) {
        result.error = L"Could not set request headers.";
        return false;
    }
    const auto size = static_cast<DWORD>(body.size());
    if (WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<char*>(body.data()), size,
            size, 0) == FALSE ||
        WinHttpReceiveResponse(request.Get(), nullptr) == FALSE) {
        const DWORD error = GetLastError();
        result.error = error == ERROR_WINHTTP_TIMEOUT ? L"Groq did not respond in time."
            : error == ERROR_WINHTTP_NAME_NOT_RESOLVED || error == ERROR_WINHTTP_CANNOT_CONNECT
            ? L"Could not connect to Groq. Check your internet connection."
            : L"The request to Groq failed.";
        return false;
    }
    return ReadResponse(request.Get(), onData, &cancelled, kMaxStreamBytes, result);
}

std::wstring DescribeFailure(const HttpResult& result, const std::wstring& model) {
    if (result.status == 0) {
        return result.error.empty() ? L"The request to Groq failed." : result.error;
    }
    const std::wstring detail = groq::Utf8ToWide(groq::ErrorMessage(result.errorBody));
    switch (result.status) {
    case 401:
    case 403:
        return L"Groq rejected the API key. Update it in Dock Settings \u203A AI.";
    case 404:
        return L"The model \u201C" + model + L"\u201D isn't available on Groq. Change it in Dock Settings \u203A AI.";
    case 413:
        return L"The question is too long for the model.";
    case 429:
        return L"Groq's rate limit was reached. Try again in a moment." +
            (detail.empty() ? std::wstring() : L"\n\n" + detail);
    default:
        break;
    }
    if (result.status >= 500) {
        return L"Groq is having trouble right now (HTTP " + std::to_wstring(result.status) + L"). Try again shortly.";
    }
    return L"Groq returned HTTP " + std::to_wstring(result.status) + (detail.empty() ? L"." : L": " + detail);
}

std::string TodayText() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t text[96]{};
    if (GetDateFormatEx(LOCALE_NAME_INVARIANT, 0, &now, L"dddd, d MMMM yyyy", text, 96, nullptr) == 0) {
        swprintf_s(text, L"%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    }
    return groq::WideToUtf8(text);
}

class Publisher {
public:
    Publisher(std::shared_ptr<AiSession> session, HWND reply, UINT message)
        : m_session(std::move(session)), m_reply(reply), m_message(message) {}

    template <typename Mutate>
    void Update(Mutate&& mutate) {
        {
            const std::lock_guard lock(m_session->mutex);
            mutate(m_session->state);
        }
        if (!m_session->messagePending.exchange(true) &&
            PostMessageW(m_reply, m_message, static_cast<WPARAM>(m_session->id), 0) == FALSE) {
            m_session->messagePending.store(false);
        }
    }

    void Fail(std::wstring error) {
        Update([&](AiSnapshot& state) {
            state.error = std::move(error);
            state.status.clear();
            state.done = true;
        });
    }

    [[nodiscard]] bool Cancelled() const noexcept { return m_session->cancelled.load(); }
    [[nodiscard]] const std::atomic<bool>& CancelFlag() const noexcept { return m_session->cancelled; }

private:
    std::shared_ptr<AiSession> m_session;
    HWND m_reply;
    UINT m_message;
};

std::vector<AiSource> WideSources(const std::vector<groq::Source>& sources) {
    std::vector<AiSource> out;
    out.reserve(sources.size());
    for (const groq::Source& source : sources) {
        out.push_back({groq::Utf8ToWide(source.title), groq::Utf8ToWide(source.url), groq::Utf8ToWide(source.snippet)});
    }
    return out;
}

// One browser_search round trip on the GPT-OSS search model.
bool RunWebSearch(const std::wstring& apiKey, const std::string& query, const std::atomic<bool>& cancelled,
    groq::SearchOutcome& outcome, std::wstring& error) {
    std::string body;
    HttpResult result;
    const bool ok = PostChat(apiKey, groq::BuildSearchRequest(query),
        [&](std::string_view chunk) { body.append(chunk); }, cancelled, result);
    if (!ok) {
        error = result.cancelled ? std::wstring() : DescribeFailure(result, groq::Utf8ToWide(groq::kSearchModel));
        return false;
    }
    if (!groq::ParseSearchResponse(body, outcome)) {
        error = L"The web search returned an unreadable reply.";
        return false;
    }
    return true;
}

void AskWorker(std::shared_ptr<AiSession> session, HWND reply, UINT message, std::wstring apiKey, std::wstring model,
    std::wstring prompt) {
    Publisher publisher(session, reply, message);
    const std::string modelUtf8 = groq::WideToUtf8(model);
    groq::SearchMode searchMode =
        groq::HasBuiltinSearch(modelUtf8) ? groq::SearchMode::Builtin : groq::SearchMode::Function;

    std::vector<groq::Message> messages;
    messages.push_back({"system", groq::SystemPrompt(TodayText(), searchMode), {}, {}});
    messages.push_back({"user", groq::WideToUtf8(prompt), {}, {}});
    std::vector<groq::Source> sources;
    bool searched = false;
    bool toolsRetried = false;

    for (int round = 0; round <= kMaxToolRounds; ++round) {
        const groq::SearchMode mode =
            round == kMaxToolRounds && searchMode == groq::SearchMode::Function ? groq::SearchMode::None : searchMode;
        const std::string body = groq::BuildChatRequest(modelUtf8, messages, mode, true);
        groq::StreamParser parser;
        ULONGLONG lastPublish = 0;
        size_t publishedBytes = 0;
        auto publishContent = [&](bool force) {
            const ULONGLONG now = GetTickCount64();
            if (!force && (now - lastPublish < kPublishIntervalMs || parser.Content().size() == publishedBytes)) {
                return;
            }
            lastPublish = now;
            publishedBytes = parser.Content().size();
            std::wstring markdown = groq::Utf8ToWide(groq::StripThinking(parser.Content()));
            const bool thinking = markdown.empty() && (parser.Reasoning() || !parser.Content().empty());
            publisher.Update([&](AiSnapshot& state) {
                state.markdown = std::move(markdown);
                if (state.markdown.empty()) {
                    state.status = thinking ? L"Thinking\u2026" : state.status;
                } else {
                    state.status = L"Writing\u2026";
                }
            });
        };
        HttpResult result;
        const bool ok = PostChat(apiKey, body,
            [&](std::string_view chunk) {
                parser.Feed(chunk);
                publishContent(false);
            },
            publisher.CancelFlag(), result);
        parser.Finish();
        if (publisher.Cancelled()) {
            return;
        }
        if (!ok) {
            // Some models reject tool definitions: answer without web search instead.
            if (result.status == 400 && mode != groq::SearchMode::None && !toolsRetried &&
                result.errorBody.find("tool") != std::string::npos) {
                toolsRetried = true;
                searchMode = groq::SearchMode::None;
                messages.front().content = groq::SystemPrompt(TodayText(), groq::SearchMode::None);
                --round;
                continue;
            }
            publisher.Fail(DescribeFailure(result, model));
            return;
        }
        if (!parser.Error().empty()) {
            publisher.Fail(L"Groq reported an error: " + groq::Utf8ToWide(parser.Error()));
            return;
        }
        if (mode == groq::SearchMode::Builtin) {
            for (const groq::Source& source : parser.Sources()) {
                groq::AddSource(sources, source);
            }
            searched = searched || parser.Searched();
        }

        std::vector<groq::ToolCall> calls = parser.ToolCalls();
        std::erase_if(calls, [](const groq::ToolCall& call) { return call.name.empty(); });
        if (mode == groq::SearchMode::Function && !calls.empty()) {
            if (calls.size() > kMaxCallsPerRound) {
                calls.resize(kMaxCallsPerRound);
            }
            for (size_t i = 0; i < calls.size(); ++i) {
                if (calls[i].id.empty()) {
                    calls[i].id = "call_" + std::to_string(round) + "_" + std::to_string(i);
                }
            }
            messages.push_back({"assistant", parser.Content(), {}, calls});
            for (const groq::ToolCall& call : calls) {
                std::string toolContent;
                if (call.name == groq::kSearchToolName) {
                    std::string query = groq::SearchQueryFromArguments(call.arguments);
                    if (query.empty()) {
                        query = groq::WideToUtf8(prompt);
                    }
                    const std::wstring wideQuery = groq::Utf8ToWide(query);
                    publisher.Update([&](AiSnapshot& state) {
                        state.markdown.clear();
                        state.status = L"Searching the web for \u201C" + wideQuery + L"\u201D\u2026";
                    });
                    groq::SearchOutcome outcome;
                    std::wstring error;
                    if (RunWebSearch(apiKey, query, publisher.CancelFlag(), outcome, error)) {
                        std::vector<size_t> numbers;
                        for (const groq::Source& source : outcome.sources) {
                            groq::AddSource(sources, source);
                            const auto found = std::find_if(sources.begin(), sources.end(),
                                [&](const groq::Source& known) { return known.url == source.url; });
                            numbers.push_back(static_cast<size_t>(found - sources.begin()) + 1);
                        }
                        searched = true;
                        toolContent = groq::BuildToolResult(query, outcome, numbers);
                    } else {
                        if (publisher.Cancelled()) {
                            return;
                        }
                        toolContent = R"({"error":)";
                        groq::json::AppendQuoted(toolContent,
                            "Web search failed: " + groq::WideToUtf8(error) + " Answer from your own knowledge.");
                        toolContent += "}";
                    }
                } else {
                    toolContent = R"({"error":"Unknown tool."})";
                }
                messages.push_back({"tool", std::move(toolContent), call.id, {}});
            }
            std::vector<AiSource> wide = WideSources(sources);
            publisher.Update([&](AiSnapshot& state) {
                state.sources = std::move(wide);
                state.searched = searched;
                state.status = L"Reading results\u2026";
            });
            continue;
        }

        std::wstring markdown = groq::Utf8ToWide(groq::StripThinking(parser.Content()));
        if (markdown.empty()) {
            publisher.Fail(L"The model returned an empty answer. Try rephrasing the question.");
            return;
        }
        std::vector<AiSource> wide = WideSources(sources);
        publisher.Update([&](AiSnapshot& state) {
            state.markdown = std::move(markdown);
            state.sources = std::move(wide);
            state.searched = searched;
            state.status.clear();
            state.done = true;
        });
        return;
    }
    publisher.Fail(L"The model kept searching without answering. Try a more specific question.");
}

// ---- images -------------------------------------------------------------------------

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        if (m_ptr != nullptr) {
            m_ptr->Release();
        }
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** Put() noexcept { return &m_ptr; }
    T* Get() const noexcept { return m_ptr; }
    T* operator->() const noexcept { return m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

private:
    T* m_ptr = nullptr;
};

bool DownloadImage(const std::wstring& url, std::string& bytes) {
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{};
    wchar_t path[4096]{};
    wchar_t extra[4096]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 4096;
    if (WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) == FALSE ||
        (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP)) {
        return false;
    }
    WinHttpHandle session = OpenSession();
    if (!session) {
        return false;
    }
    WinHttpHandle connection(WinHttpConnect(session.Get(), host, parts.nPort, 0));
    if (!connection) {
        return false;
    }
    const std::wstring object = std::wstring(path) + extra;
    WinHttpHandle request(WinHttpOpenRequest(connection.Get(), L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        return false;
    }
    constexpr wchar_t kAccept[] = L"Accept: image/png, image/jpeg, image/gif, image/bmp, image/webp, image/*;q=0.8";
    WinHttpAddRequestHeaders(request.Get(), kAccept, static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);
    if (WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ==
            FALSE ||
        WinHttpReceiveResponse(request.Get(), nullptr) == FALSE) {
        return false;
    }
    HttpResult result;
    return ReadResponse(request.Get(), [&](std::string_view chunk) { bytes.append(chunk); }, nullptr, kMaxImageBytes,
        result);
}

bool DecodeImage(const std::string& bytes, UINT maxWidth, UINT maxHeight, AiImageReply& image) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.Put())))) {
        return false;
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.Put())) ||
        FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size())))) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
            decoder.Put()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.Put()))) {
        return false;
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 || width > kMaxImageSide ||
        height > kMaxImageSide) {
        return false;
    }
    const double fit = std::min({1.0, static_cast<double>(maxWidth) / width, static_cast<double>(maxHeight) / height});
    const UINT outWidth = std::max(1U, static_cast<UINT>(width * fit + 0.5));
    const UINT outHeight = std::max(1U, static_cast<UINT>(height * fit + 0.5));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.Put())) ||
        FAILED(scaler->Initialize(frame.Get(), outWidth, outHeight, WICBitmapInterpolationModeHighQualityCubic))) {
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.Put())) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
            0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }
    image.pixels.resize(static_cast<size_t>(outWidth) * outHeight * 4U);
    if (FAILED(converter->CopyPixels(nullptr, outWidth * 4U, static_cast<UINT>(image.pixels.size()),
            image.pixels.data()))) {
        image.pixels.clear();
        return false;
    }
    image.width = outWidth;
    image.height = outHeight;
    return true;
}

}  // namespace

namespace GroqClient {

void AskAsync(std::shared_ptr<AiSession> session, HWND reply, UINT message, std::wstring apiKey, std::wstring model,
    std::wstring prompt) {
    std::thread([session = std::move(session), reply, message, apiKey = std::move(apiKey), model = std::move(model),
                    prompt = std::move(prompt)]() mutable {
        try {
            AskWorker(session, reply, message, apiKey, model, prompt);
        } catch (...) {
            Publisher(session, reply, message).Fail(L"Something went wrong while asking AI.");
        }
        SecureZeroMemory(apiKey.data(), apiKey.size() * sizeof(wchar_t));
    }).detach();
}

void FetchImageAsync(HWND reply, UINT message, std::wstring url, UINT maxWidth, UINT maxHeight) {
    if (!aimd::IsSafeUrl(url) || url.starts_with(L"mailto:")) {
        return;
    }
    std::thread([reply, message, url = std::move(url), maxWidth, maxHeight]() {
        auto image = std::make_unique<AiImageReply>();
        image->url = url;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            std::string bytes;
            if (DownloadImage(url, bytes) && !bytes.empty()) {
                DecodeImage(bytes, std::max(1U, maxWidth), std::max(1U, maxHeight), *image);
            }
        } catch (...) {
            image->pixels.clear();
        }
        if (SUCCEEDED(init)) {
            CoUninitialize();
        }
        AiImageReply* raw = image.release();
        if (PostMessageW(reply, message, 0, reinterpret_cast<LPARAM>(raw)) == FALSE) {
            delete raw;
        }
    }).detach();
}

}  // namespace GroqClient
