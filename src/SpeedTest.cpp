#include "SpeedTest.h"

#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kHost[] = L"speed.cloudflare.com";
constexpr int kStreams = 4;
constexpr int kPingSamples = 8;
// TCP slow start: bytes in the first second are excluded from the result.
constexpr ULONGLONG kWarmupMs = 1000;
constexpr ULONGLONG kDownloadMs = 8000;
constexpr ULONGLONG kUploadMs = 6000;
constexpr ULONGLONG kSampleMs = 200;
// Data ceilings so a gigabit link does not pull gigabytes per run.
constexpr uint64_t kDownloadCapBytes = 250ULL * 1024ULL * 1024ULL;
constexpr uint64_t kUploadCapBytes = 100ULL * 1024ULL * 1024ULL;
constexpr DWORD kChunkBytes = 64U * 1024U;
constexpr DWORD kUploadRequestBytes = 8U * 1024U * 1024U;

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : m_handle(handle) {}
    ~InternetHandle() { Reset(nullptr); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    InternetHandle(InternetHandle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            Reset(std::exchange(other.m_handle, nullptr));
        }
        return *this;
    }
    void Reset(HINTERNET handle) noexcept {
        if (m_handle != nullptr) {
            WinHttpCloseHandle(m_handle);
        }
        m_handle = handle;
    }
    [[nodiscard]] HINTERNET Get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

private:
    HINTERNET m_handle = nullptr;
};

// One keep-alive HTTPS connection to the test host.
struct Connection {
    InternetHandle session;
    InternetHandle connect;

    bool Open() {
        session.Reset(WinHttpOpen(L"HoverdockSpeedTest/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) {
            return false;
        }
        WinHttpSetTimeouts(session.Get(), 5000, 5000, 10000, 10000);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        WinHttpSetOption(session.Get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        connect.Reset(WinHttpConnect(session.Get(), kHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
        return static_cast<bool>(connect);
    }

    [[nodiscard]] InternetHandle Request(const wchar_t* verb, const wchar_t* path) const {
        // REFRESH: never satisfied from a cache, always on the wire.
        return InternetHandle(WinHttpOpenRequest(connect.Get(), verb, path, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH));
    }
};

bool StatusOk(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    return WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
               WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != FALSE &&
        status >= 200 && status < 300;
}

bool Drain(HINTERNET request) {
    uint8_t buffer[4096];
    for (;;) {
        DWORD read = 0;
        if (WinHttpReadData(request, buffer, sizeof(buffer), &read) == FALSE) {
            return false;
        }
        if (read == 0) {
            return true;
        }
    }
}

double NowMs() noexcept {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 / frequency;
}

// Request round trip (send -> response headers) for an empty body, in ms;
// -1 on failure.
double TimedRequest(const Connection& connection) {
    InternetHandle request = connection.Request(L"GET", L"/__down?bytes=0");
    if (!request) {
        return -1.0;
    }
    const double start = NowMs();
    if (WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ==
            FALSE ||
        WinHttpReceiveResponse(request.Get(), nullptr) == FALSE) {
        return -1.0;
    }
    const double elapsed = NowMs() - start;
    if (!StatusOk(request.Get()) || !Drain(request.Get())) {
        return -1.0;
    }
    return elapsed;
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2U;
    return values.size() % 2U == 1U ? values[middle] : 0.5 * (values[middle - 1U] + values[middle]);
}

struct StreamShared {
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
};

void DownloadStream(StreamShared& shared) {
    Connection connection;
    if (!connection.Open()) {
        ++shared.failures;
        return;
    }
    std::vector<uint8_t> buffer(kChunkBytes);
    while (!shared.stop.load()) {
        InternetHandle request = connection.Request(L"GET", L"/__down?bytes=25000000");
        if (!request ||
            WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ==
                FALSE ||
            WinHttpReceiveResponse(request.Get(), nullptr) == FALSE || !StatusOk(request.Get())) {
            ++shared.failures;
            return;
        }
        for (;;) {
            if (shared.stop.load()) {
                return;
            }
            DWORD read = 0;
            if (WinHttpReadData(request.Get(), buffer.data(), kChunkBytes, &read) == FALSE) {
                ++shared.failures;
                return;
            }
            if (read == 0) {
                break;
            }
            shared.bytes += read;
        }
    }
}

void UploadStream(StreamShared& shared) {
    Connection connection;
    if (!connection.Open()) {
        ++shared.failures;
        return;
    }
    const std::vector<uint8_t> payload(kChunkBytes, static_cast<uint8_t>(0x5A));
    while (!shared.stop.load()) {
        InternetHandle request = connection.Request(L"POST", L"/__up");
        if (!request ||
            WinHttpSendRequest(request.Get(), L"Content-Type: application/octet-stream\r\n", static_cast<DWORD>(-1),
                WINHTTP_NO_REQUEST_DATA, 0, kUploadRequestBytes, 0) == FALSE) {
            ++shared.failures;
            return;
        }
        DWORD sent = 0;
        while (sent < kUploadRequestBytes) {
            if (shared.stop.load()) {
                return;
            }
            const DWORD chunk = std::min(kChunkBytes, kUploadRequestBytes - sent);
            DWORD written = 0;
            if (WinHttpWriteData(request.Get(), payload.data(), chunk, &written) == FALSE || written == 0) {
                ++shared.failures;
                return;
            }
            sent += written;
            shared.bytes += written;
        }
        if (WinHttpReceiveResponse(request.Get(), nullptr) == FALSE || !Drain(request.Get())) {
            ++shared.failures;
            return;
        }
    }
}

}  // namespace

struct SpeedTestService::State {
    mutable std::mutex mutex;
    Snapshot snapshot;
    HWND window = nullptr;
    UINT message = 0;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};

    void Post() {
        HWND target = nullptr;
        UINT id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            target = window;
            id = message;
        }
        if (target != nullptr && id != 0) {
            PostMessageW(target, id, 0, 0);
        }
    }

    template <typename Mutate>
    void Update(Mutate&& mutate) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            mutate(snapshot);
        }
        Post();
    }

    void Fail(const wchar_t* text) {
        Update([text](Snapshot& s) {
            s.phase = Phase::Failed;
            s.liveValue = -1.0;
            s.error = text;
            s.finishedAt = GetTickCount64();
        });
    }

    // Parallel streams for `durationMs` (or until `capBytes`); Mbps after the
    // warm-up second, published live every kSampleMs. -1 when nothing moved.
    double MeasureThroughput(bool upload, ULONGLONG durationMs, uint64_t capBytes) {
        StreamShared shared;
        std::vector<std::thread> streams;
        try {
            for (int index = 0; index < kStreams; ++index) {
                streams.emplace_back([&shared, upload] { upload ? UploadStream(shared) : DownloadStream(shared); });
            }
        } catch (...) {
            shared.stop.store(true);
        }
        const ULONGLONG start = GetTickCount64();
        ULONGLONG warmAt = 0;
        uint64_t warmBytes = 0;
        ULONGLONG end = start;
        uint64_t endBytes = 0;
        while (!shared.stop.load()) {
            Sleep(static_cast<DWORD>(kSampleMs));
            const ULONGLONG now = GetTickCount64();
            const uint64_t bytes = shared.bytes.load();
            if (warmAt == 0 && now - start >= kWarmupMs) {
                warmAt = now;
                warmBytes = bytes;
            }
            const ULONGLONG since = warmAt != 0 ? warmAt : start;
            const uint64_t base = warmAt != 0 ? warmBytes : 0;
            if (now > since && bytes > base) {
                const double mbps = static_cast<double>(bytes - base) * 8.0 /
                    (static_cast<double>(now - since) * 1000.0);
                Update([mbps](Snapshot& s) { s.liveValue = mbps; });
            }
            end = now;
            endBytes = bytes;
            if (cancel.load() || now - start >= durationMs || bytes >= capBytes ||
                shared.failures.load() >= kStreams) {
                break;
            }
        }
        shared.stop.store(true);
        for (std::thread& stream : streams) {
            stream.join();
        }
        const ULONGLONG since = warmAt != 0 ? warmAt : start;
        const uint64_t base = warmAt != 0 ? warmBytes : 0;
        if (end <= since || endBytes <= base) {
            return -1.0;
        }
        return static_cast<double>(endBytes - base) * 8.0 / (static_cast<double>(end - since) * 1000.0);
    }

    void RunTest() {
        Connection connection;
        if (!connection.Open()) {
            Fail(L"Couldn't reach the test server");
            return;
        }
        std::vector<double> samples;
        // Sample 0 includes the TCP + TLS handshake and is discarded.
        for (int index = 0; index <= kPingSamples && !cancel.load(); ++index) {
            const double ms = TimedRequest(connection);
            if (ms < 0.0) {
                if (index == 0) {
                    Fail(L"Couldn't reach the test server");
                    return;
                }
                continue;
            }
            if (index == 0) {
                continue;
            }
            samples.push_back(ms);
            const double live = Median(samples);
            Update([live](Snapshot& s) { s.liveValue = live; });
        }
        if (cancel.load()) {
            return;
        }
        if (samples.empty()) {
            Fail(L"Couldn't measure latency");
            return;
        }
        const double ping = Median(samples);
        Update([ping](Snapshot& s) {
            s.pingMs = ping;
            s.phase = Phase::Download;
            s.liveValue = -1.0;
        });
        const double download = MeasureThroughput(false, kDownloadMs, kDownloadCapBytes);
        if (cancel.load()) {
            return;
        }
        if (download < 0.0) {
            Fail(L"Download test failed");
            return;
        }
        Update([download](Snapshot& s) {
            s.downloadMbps = download;
            s.phase = Phase::Upload;
            s.liveValue = -1.0;
        });
        const double upload = MeasureThroughput(true, kUploadMs, kUploadCapBytes);
        if (cancel.load()) {
            return;
        }
        // A failed upload still reports ping and download (shown as a dash).
        Update([upload](Snapshot& s) {
            s.uploadMbps = upload;
            s.phase = Phase::Done;
            s.liveValue = -1.0;
            s.finishedAt = GetTickCount64();
        });
    }
};

SpeedTestService::SpeedTestService() : m_state(std::make_shared<State>()) {}

SpeedTestService::~SpeedTestService() {
    // The detached run holds its own reference; it stops at the next check.
    m_state->cancel.store(true);
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->window = nullptr;
    m_state->message = 0;
}

void SpeedTestService::Run(HWND notifyWindow, UINT notifyMessage) {
    bool expected = false;
    if (!m_state->running.compare_exchange_strong(expected, true)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->window = notifyWindow;
        m_state->message = notifyMessage;
        m_state->snapshot = Snapshot{};
        m_state->snapshot.phase = Phase::Latency;
    }
    m_state->Post();
    const std::shared_ptr<State> state = m_state;
    try {
        std::thread([state] {
            state->RunTest();
            state->running.store(false);
            state->Post();
        }).detach();
    } catch (...) {
        m_state->running.store(false);
        m_state->Fail(L"Couldn't start the test");
    }
}

SpeedTestService::Snapshot SpeedTestService::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->snapshot;
}

bool SpeedTestService::Running() const {
    return m_state->running.load();
}
