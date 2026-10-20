#pragma once

#include <Windows.h>

#include <memory>
#include <string>

// Internet speed test for the Quick Settings Wi-Fi page. Measures latency,
// download and upload against Cloudflare's public speed endpoints
// (speed.cloudflare.com/__down and /__up) on a background thread. Results and
// live progress are read with GetSnapshot; notifyMessage is posted to
// notifyWindow (about 5 Hz while running) whenever they change.
class SpeedTestService {
public:
    enum class Phase : uint8_t {
        Idle,
        Latency,
        Download,
        Upload,
        Done,
        Failed,
    };

    struct Snapshot {
        Phase phase = Phase::Idle;
        // Final values; -1 until measured.
        double pingMs = -1.0;
        double downloadMbps = -1.0;
        double uploadMbps = -1.0;
        // Rolling value for the phase in progress (ms or Mbps); -1 when none yet.
        double liveValue = -1.0;
        std::wstring error;
        ULONGLONG finishedAt = 0;  // GetTickCount64 when the last run ended
    };

    SpeedTestService();
    ~SpeedTestService();
    SpeedTestService(const SpeedTestService&) = delete;
    SpeedTestService& operator=(const SpeedTestService&) = delete;

    // Starts a run unless one is in progress. Never blocks.
    void Run(HWND notifyWindow, UINT notifyMessage);
    [[nodiscard]] Snapshot GetSnapshot() const;
    [[nodiscard]] bool Running() const;

    struct State;

private:
    std::shared_ptr<State> m_state;
};
