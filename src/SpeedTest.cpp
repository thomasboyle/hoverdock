#include "SpeedTest.h"

#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
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
// Each stream sends / receives exactly its share of the cap in one request,
// so the server can never push more than the cap.
constexpr uint64_t kDownloadShareBytes = kDownloadCapBytes / kStreams;
constexpr DWORD kUploadShareBytes = static_cast<DWORD>(kUploadCapBytes / kStreams);

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

    // REFRESH: never satisfied from a cache, always on the wire. closeAfter:
    // "Connection: close", so abandoning an unfinished body resets the socket
    // instead of WinHTTP draining the rest of it for keep-alive (MEASURED
    // 2-3x the counted bytes on the wire before).
    [[nodiscard]] InternetHandle Request(const wchar_t* verb, const wchar_t* path, bool closeAfter = false) const {
        InternetHandle request(WinHttpOpenRequest(connect.Get(), verb, path, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH));
        if (request && closeAfter) {
            DWORD feature = WINHTTP_DISABLE_KEEP_ALIVE;
            WinHttpSetOption(request.Get(), WINHTTP_OPTION_DISABLE_FEATURE, &feature, sizeof(feature));
        }
        return request;
    }
};

bool StatusOk(HINTERNET request);
bool Drain(HINTERNET request);

// Opens the connection and completes its TCP + TLS handshake with an empty
// keep-alive request, so the next request on it starts transferring at once.
bool Warm(Connection& connection) {
    if (!connection.connect && !connection.Open()) {
        return false;
    }
    InternetHandle request = connection.Request(L"GET", L"/__down?bytes=0");
    return request &&
        WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) !=
            FALSE &&
        WinHttpReceiveResponse(request.Get(), nullptr) != FALSE && StatusOk(request.Get()) && Drain(request.Get());
}

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

int64_t NowUs() noexcept {
    return static_cast<int64_t>(NowMs() * 1000.0);
}

struct StreamShared {
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    std::atomic<int> finished{0};
    // Precise phase window: first byte moved and last stream finished.
    std::atomic<int64_t> firstByteUs{0};
    std::atomic<int64_t> lastDoneUs{0};
    int streams = kStreams;
    HANDLE allDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    StreamShared() = default;
    StreamShared(const StreamShared&) = delete;
    StreamShared& operator=(const StreamShared&) = delete;
    ~StreamShared() {
        if (allDone != nullptr) {
            CloseHandle(allDone);
        }
    }
    void Count(DWORD moved) {
        if (moved != 0 && bytes.fetch_add(moved) == 0) {
            int64_t none = 0;
            firstByteUs.compare_exchange_strong(none, NowUs());
        }
    }
    void Finish() {
        const int64_t now = NowUs();
        int64_t previous = lastDoneUs.load();
        while (previous < now && !lastDoneUs.compare_exchange_weak(previous, now)) {
        }
        if (++finished >= streams && allDone != nullptr) {
            SetEvent(allDone);
        }
    }
};

void DownloadStream(Connection& connection, StreamShared& shared) {
    if (!connection.connect && !connection.Open()) {
        ++shared.failures;
        return;
    }
    static const std::wstring path = L"/__down?bytes=" + std::to_wstring(kDownloadShareBytes);
    InternetHandle request = connection.Request(L"GET", path.c_str(), true);
    if (!request ||
        WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ==
            FALSE ||
        WinHttpReceiveResponse(request.Get(), nullptr) == FALSE || !StatusOk(request.Get())) {
        ++shared.failures;
        return;
    }
    std::vector<uint8_t> buffer(kChunkBytes);
    while (!shared.stop.load()) {
        DWORD read = 0;
        if (WinHttpReadData(request.Get(), buffer.data(), kChunkBytes, &read) == FALSE) {
            ++shared.failures;
            return;
        }
        if (read == 0) {
            return;  // share complete
        }
        shared.Count(read);
    }
}

void UploadStream(Connection& connection, StreamShared& shared) {
    if (!connection.connect && !connection.Open()) {
        ++shared.failures;
        return;
    }
    const std::vector<uint8_t> payload(kChunkBytes, static_cast<uint8_t>(0x5A));
    InternetHandle request = connection.Request(L"POST", L"/__up", true);
    if (!request ||
        WinHttpSendRequest(request.Get(), L"Content-Type: application/octet-stream\r\n", static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, kUploadShareBytes, 0) == FALSE) {
        ++shared.failures;
        return;
    }
    DWORD sent = 0;
    while (sent < kUploadShareBytes) {
        if (shared.stop.load()) {
            return;  // abandoned body: closing the request resets the socket
        }
        const DWORD chunk = std::min(kChunkBytes, kUploadShareBytes - sent);
        DWORD written = 0;
        if (WinHttpWriteData(request.Get(), payload.data(), chunk, &written) == FALSE || written == 0) {
            ++shared.failures;
            return;
        }
        sent += written;
        shared.Count(written);
    }
    if (WinHttpReceiveResponse(request.Get(), nullptr) == FALSE || !Drain(request.Get())) {
        ++shared.failures;
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

    // Parallel streams over `connections` (pre-warmed or not) for at most
    // `durationMs`, each moving its share of the byte cap in one request.
    // The window is first byte -> last stream finished (exact, event-driven),
    // with the first second excluded on long runs (TCP ramp-up). Long runs
    // also stop early once the estimate is stable: >= 2 s past the warm-up
    // and the last second within +-5% of the running mean for 3 samples.
    // Mbps is published live every kSampleMs. -1 when nothing moved.
    double MeasureThroughput(bool upload, ULONGLONG durationMs, std::vector<Connection>& connections) {
        StreamShared shared;
        shared.streams = static_cast<int>(connections.size());
        std::vector<std::thread> streams;
        try {
            for (Connection& connection : connections) {
                streams.emplace_back([&connection, &shared, upload] {
                    upload ? UploadStream(connection, shared) : DownloadStream(connection, shared);
                    shared.Finish();
                });
            }
        } catch (...) {
            shared.stop.store(true);
        }
        shared.streams = static_cast<int>(streams.size());
        if (shared.finished.load() >= shared.streams && shared.allDone != nullptr) {
            SetEvent(shared.allDone);
        }
        const double start = NowMs();
        double warmAt = 0.0;
        uint64_t warmBytes = 0;
        // (time ms, bytes) samples covering the last second, for stability.
        std::vector<std::pair<double, uint64_t>> recent;
        int stableSamples = 0;
        bool allFinished = false;
        double end = start;
        uint64_t endBytes = 0;
        while (!shared.stop.load()) {
            allFinished = shared.allDone == nullptr
                ? (Sleep(static_cast<DWORD>(kSampleMs)), shared.finished.load() >= shared.streams)
                : WaitForSingleObject(shared.allDone, static_cast<DWORD>(kSampleMs)) == WAIT_OBJECT_0;
            const double now = NowMs();
            const uint64_t bytes = shared.bytes.load();
            const int64_t firstUs = shared.firstByteUs.load();
            const double firstByteAt = firstUs != 0 ? static_cast<double>(firstUs) / 1000.0 : 0.0;
            end = now;
            endBytes = bytes;
            if (firstByteAt > 0.0 && warmAt == 0.0 && now - firstByteAt >= static_cast<double>(kWarmupMs)) {
                warmAt = now;
                warmBytes = bytes;
            }
            const bool warmWindow = warmAt != 0.0 && now - warmAt >= 500.0 && bytes > warmBytes;
            const double since = warmWindow ? warmAt : firstByteAt;
            const uint64_t base = warmWindow ? warmBytes : 0;
            double mean = -1.0;
            if (since > 0.0 && now > since && bytes > base) {
                mean = static_cast<double>(bytes - base) * 8.0 / ((now - since) * 1000.0);
                Update([mean](Snapshot& s) { s.liveValue = mean; });
            }
            if (allFinished || cancel.load() || now - start >= static_cast<double>(durationMs)) {
                break;
            }
            recent.emplace_back(now, bytes);
            while (recent.size() > 1 && now - recent.front().first > 1000.0) {
                recent.erase(recent.begin());
            }
            if (warmWindow && now - warmAt >= 2000.0 && mean > 0.0 && recent.size() > 1 &&
                now - recent.front().first >= 800.0) {
                const double last = static_cast<double>(recent.back().second - recent.front().second) * 8.0 /
                    ((recent.back().first - recent.front().first) * 1000.0);
                stableSamples = std::abs(last - mean) <= 0.05 * mean ? stableSamples + 1 : 0;
                if (stableSamples >= 3) {
                    break;
                }
            }
        }
        shared.stop.store(true);
        for (std::thread& stream : streams) {
            stream.join();
        }
        const int64_t firstUs = shared.firstByteUs.load();
        const double firstByteAt = firstUs != 0 ? static_cast<double>(firstUs) / 1000.0 : 0.0;
        if (allFinished) {
            // Exact: the last stream's completion, not the sample that saw it.
            end = static_cast<double>(shared.lastDoneUs.load()) / 1000.0;
            endBytes = shared.bytes.load();
        }
        const bool warmWindow = warmAt != 0.0 && end - warmAt >= 500.0 && endBytes > warmBytes;
        const double since = warmWindow ? warmAt : firstByteAt;
        const uint64_t base = warmWindow ? warmBytes : 0;
        if (since <= 0.0 || end <= since || endBytes <= base) {
            return -1.0;
        }
        return static_cast<double>(endBytes - base) * 8.0 / ((end - since) * 1000.0);
    }

    void RunTest() {
        Connection connection;
        if (!connection.Open()) {
            Fail(L"Couldn't reach the test server");
            return;
        }
        // Download connections complete their handshakes while ping runs, so
        // the download phase starts on warm sockets (one handshake RTT saved).
        std::vector<Connection> downloadConnections(kStreams);
        std::vector<std::thread> warmers;
        struct JoinAll {
            std::vector<std::thread>& threads;
            ~JoinAll() {
                for (std::thread& thread : threads) {
                    if (thread.joinable()) {
                        thread.join();
                    }
                }
            }
        } joinWarmers{warmers};
        try {
            for (Connection& pending : downloadConnections) {
                warmers.emplace_back([&pending] { static_cast<void>(Warm(pending)); });
            }
        } catch (...) {
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
        for (std::thread& warmer : warmers) {
            warmer.join();
        }
        const double download = MeasureThroughput(false, kDownloadMs, downloadConnections);
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
        std::vector<Connection> uploadConnections(kStreams);
        const double upload = MeasureThroughput(true, kUploadMs, uploadConnections);
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
