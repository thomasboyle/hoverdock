#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Decision core of the Automatic energy mode (portable, no Win32; unit tested
// in tests/AutoPowerPolicyTests.cpp). Modelled on macOS Automatic: the system
// stays efficient by default and gives demanding user work full performance
// the moment it needs it, without the user picking a mode.
//
// Full power is demanded by:
//   - Game      a focused game (known library path, emulator, Game Bar
//               registered, exclusive fullscreen, or borderless fullscreen
//               with real 3D load) while the user is playing it. Held briefly
//               so an Alt+Tab glance does not drop it; released once the
//               player has been away for a few minutes.
//   - Creative  a focused creative tool the user is driving (timeline scrub,
//               viewport orbit, brush strokes): input plus real load.
//   - Audio     a creative tool with a live audio stream (DAW playback or
//               recording), which must never underrun.
//   - Render    a creative tool doing sustained CPU/GPU work, focused or not
//               (export, render, encode, simulation bake).
//   - Call      a video call (camera in use) doing real work (background
//               blur, encoding), focused or not.
//   - Ai        local AI work: a known AI runtime under sustained load, or any
//               app keeping a GPU compute engine busy.
//   - Heavy     any focused app with sustained heavy CPU load, including a
//               single saturated thread (compiles, scripts, recalcs).
// Each demand is held for a reason-specific time after it stops so power
// does not flap between samples. Games, creative tools, AI runtimes, and apps
// playing audio or using the camera or microphone are also exempt from
// Windows background throttling (EcoQoS) while that is true.
namespace autopower {

enum class AppKind : uint8_t {
    Other,
    Game,
    Creative,
    // Fullscreen is normal for these (browsers, players, slideshows, remote
    // desktops), so they never count as a game by the fullscreen heuristic.
    NeverGame,
    // Local AI runtimes (LLM servers, diffusion UIs).
    Ai,
};

// Ordered by priority: a higher value wins when several apps demand power.
enum class BoostReason : uint8_t {
    None,
    Heavy,
    Ai,
    Call,
    Render,
    Audio,
    Creative,
    Game,
};

// Plain data (views, flags and numbers): copying one is a memcpy, so the
// engine can refresh its inputs every tick for free. key and name view
// strings the caller owns, which must outlive the Update call.
struct AppSample {
    // Stable grouping key (lowercase executable name of the app's root).
    std::wstring_view key;
    std::wstring_view name;
    AppKind kind = AppKind::Other;
    bool foreground = false;
    // Foreground window covers its whole monitor (borderless or exclusive).
    bool fullscreen = false;
    // Direct3D exclusive fullscreen (QUNS_RUNNING_D3D_FULL_SCREEN).
    bool exclusiveFullscreen = false;
    // Registered as a game with Windows Game Bar (GameConfigStore).
    bool registeredGame = false;
    // Share of the whole machine (0-100) across the app's process group.
    double cpuPercent = 0.0;
    // Logical processors kept busy (0-N): 1.0 is one saturated thread no
    // matter how many cores the machine has.
    double cpuCores = 0.0;
    // Busiest GPU engine of any type, the busiest 3D engine and the busiest
    // compute (Compute/CUDA) engine (0-100).
    double gpuPercent = 0.0;
    double gpu3dPercent = 0.0;
    double gpuComputePercent = 0.0;
    // Live audio stream (playback or capture) and camera / microphone use.
    bool audioActive = false;
    bool cameraActive = false;
    bool micActive = false;
    // Learned: this app has needed full power for its work before, so its
    // next busy sample is trusted without waiting for a sustained run.
    bool knownDemanding = false;
    // Load figures were measured for this update. Re-evaluations that reuse
    // an earlier reading (input or focus ticks) must not count as new load.
    bool freshLoad = true;
};

struct Inputs {
    uint64_t nowMs = 0;
    // Time since the last keyboard/mouse/pen/gamepad input in the session.
    uint64_t idleMs = 0;
    std::vector<AppSample> apps;
    // Optional promise: a nonzero value equal to the previous update's means
    // apps holds the same keys in the same order (only loads and flags
    // changed), so keys need not be compared. 0 = no promise.
    uint64_t layout = 0;
};

struct Decision {
    bool fullPower = false;
    BoostReason reason = BoostReason::None;
    // App the boost is credited to (foreground first, then priority).
    std::wstring appKey;
    std::wstring appName;
    // App keys whose processes must never be power throttled right now
    // (sorted), and a version that changes whenever the list may have:
    // compare versions to detect a change without comparing the strings.
    std::vector<std::wstring> unthrottle;
    uint64_t unthrottleVersion = 0;
    // Apps whose sustained work (Heavy, Render, Ai, Call) was newly confirmed
    // by this update: worth remembering as demanding (see History).
    std::vector<std::wstring> confirmed;
};

struct Tuning {
    // Creative tool counts as "being driven" within this much input idle,
    // and only with real load behind the input (not a hovering pointer).
    uint64_t interactIdleMs = 5000;
    double creativeCores = 0.3;
    double creativeGpuPercent = 10.0;
    // Heavy foreground work stops counting once the user walks away.
    uint64_t heavyIdleMs = 5 * 60 * 1000;
    // A focused game stops counting after this long without any input.
    uint64_t gameAwayMs = 3 * 60 * 1000;
    // Sustained load: at least sustainSamples fresh busy samples spanning
    // sustainMs, so an app launch or a single spike never boosts. With the
    // engine's 100 ms probe after input this confirms in ~300 ms. Apps known
    // to be demanding confirm on their first busy sample.
    // Machine share catches parallel work; cores catches one busy thread on
    // a many-core machine (3% of a 32-thread CPU). Thresholds sit below 1.0
    // because CPU time is charged in clock ticks: one saturated thread reads
    // 0.85-1.0 over a 100 ms probe.
    double renderCpuPercent = 15.0;
    double renderCores = 0.7;
    double renderGpuPercent = 25.0;
    double heavyCpuPercent = 35.0;
    double heavyCores = 0.8;
    double callCores = 0.5;
    double callGpuPercent = 15.0;
    double aiCores = 1.0;
    double aiGpuPercent = 20.0;
    double computeGpuPercent = 30.0;
    uint64_t sustainMs = 300;
    int sustainSamples = 2;
    // Borderless fullscreen counts as a game above this 3D engine load.
    double fullscreenGame3dPercent = 20.0;
    // Hold after demand stops, per reason.
    uint64_t gameHoldMs = 3000;
    uint64_t creativeHoldMs = 8000;
    uint64_t audioHoldMs = 8000;
    uint64_t renderHoldMs = 20000;
    uint64_t callHoldMs = 10000;
    uint64_t aiHoldMs = 20000;
    uint64_t heavyHoldMs = 10000;
};

// Classify an executable by lowercase file name and lowercase full path
// (path may be empty when the process could not be opened).
[[nodiscard]] AppKind ClassifyApp(std::wstring_view exeLower, std::wstring_view pathLower);

[[nodiscard]] const wchar_t* ReasonLabel(BoostReason reason) noexcept;

class Policy {
public:
    Policy() = default;
    explicit Policy(const Tuning& tuning) : m_tuning(tuning) {}

    [[nodiscard]] Decision Update(const Inputs& inputs);
    // Same, into a caller-owned decision whose buffers are reused (the
    // engine's per-tick path: no allocations once warmed up).
    void Update(const Inputs& inputs, Decision& decision);
    void Reset() noexcept;

    [[nodiscard]] const Tuning& Settings() const noexcept {
        return m_tuning;
    }

private:
    // A run of consecutive fresh busy samples; confirmed once it first
    // demanded power (reported to History once per run).
    struct Run {
        uint64_t sinceMs = 0;
        int samples = 0;
        bool confirmed = false;
    };

    struct Track {
        std::wstring name;
        BoostReason reason = BoostReason::None;
        // Hold of the app as a whole, and of the reason shown for it: a lower
        // demand outlasting a higher one takes over the label.
        uint64_t holdUntilMs = 0;
        uint64_t reasonUntilMs = 0;
        BoostReason lastDemand = BoostReason::None;
        uint64_t lastDemandMs = 0;
        Run render;
        Run heavy;
        Run call;
        Run ai;
        // Update generation that last saw this app (exited apps are dropped).
        uint64_t seenGeneration = 0;
        bool exemptNow = false;
        // On the exempt list (exempt, or holding a boost).
        bool listed = false;
    };

    // An input position's track (pointers into m_tracks stay valid until
    // the track is erased, which only happens to apps absent from the last
    // update, whose slots were overwritten by it).
    struct Slot {
        const std::wstring* key = nullptr;
        Track* track = nullptr;
    };

    // newRun: the returned demand is a sustained run demanding for the first time.
    [[nodiscard]] BoostReason Demand(
        const AppSample& app, uint64_t nowMs, uint64_t idleMs, Track& track, bool& newRun) const;
    [[nodiscard]] bool Sustained(const Run& run, uint64_t stamp, bool known) const noexcept;
    static bool FirstDemand(Run& run) noexcept;
    [[nodiscard]] uint64_t HoldMs(BoostReason reason) const noexcept;

    Tuning m_tuning;
    std::unordered_map<std::wstring, Track> m_tracks;
    uint64_t m_generation = 0;
    std::vector<Slot> m_slots;
    uint64_t m_layout = 0;
    std::vector<Slot> m_seen;
    std::vector<std::wstring> m_unthrottle;
    uint64_t m_unthrottleVersion = 0;
};

// Per-app memory of confirmed demanding work, persisted between sessions so
// an app that needed full power before gets it on its first busy sample.
// Days are whole days since any fixed epoch; entries fade after maxAgeDays.
class History {
public:
    static constexpr uint32_t kKnownAfter = 2;
    static constexpr uint32_t kMaxAgeDays = 30;
    static constexpr size_t kMaxEntries = 256;

    // Count one confirmed episode for key; true when the store changed.
    bool Record(const std::wstring& key, uint32_t day);
    [[nodiscard]] bool IsKnown(const std::wstring& key, uint32_t day) const;
    // Drop entries older than kMaxAgeDays and keep at most kMaxEntries.
    void Prune(uint32_t day);

    // One entry per line: "<count>\t<lastDay>\t<key>".
    [[nodiscard]] std::wstring Serialize() const;
    void Parse(std::wstring_view text);

    [[nodiscard]] size_t Size() const noexcept {
        return m_entries.size();
    }

private:
    struct Entry {
        uint32_t count = 0;
        uint32_t lastDay = 0;
    };
    std::unordered_map<std::wstring, Entry> m_entries;
};

}  // namespace autopower
