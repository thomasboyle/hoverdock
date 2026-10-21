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
//   - Game      a focused game (known library path, Game Bar registered,
//               exclusive fullscreen, or borderless fullscreen with real 3D
//               load). Held briefly so an Alt+Tab glance does not drop it.
//   - Creative  a focused creative tool while the user is driving it
//               (timeline scrub, viewport orbit, brush strokes).
//   - Render    a creative tool doing sustained CPU/GPU work, focused or not
//               (export, render, encode, simulation bake).
//   - Heavy     any focused app with sustained heavy CPU load (compiles etc.).
// Each demand is held for a reason-specific time after it stops so power
// does not flap between samples. Games and creative tools are also exempt
// from Windows background throttling (EcoQoS) for as long as they run.
namespace autopower {

enum class AppKind : uint8_t {
    Other,
    Game,
    Creative,
    // Fullscreen is normal for these (browsers, players, slideshows, remote
    // desktops), so they never count as a game by the fullscreen heuristic.
    NeverGame,
};

// Ordered by priority: a higher value wins when several apps demand power.
enum class BoostReason : uint8_t {
    None,
    Heavy,
    Render,
    Creative,
    Game,
};

struct AppSample {
    // Stable grouping key (lowercase executable name of the app's root).
    std::wstring key;
    std::wstring name;
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
    // Busiest GPU engine of any type, and the busiest 3D engine (0-100).
    double gpuPercent = 0.0;
    double gpu3dPercent = 0.0;
};

struct Inputs {
    uint64_t nowMs = 0;
    // Time since the last keyboard/mouse/pen input in the session.
    uint64_t idleMs = 0;
    std::vector<AppSample> apps;
};

struct Decision {
    bool fullPower = false;
    BoostReason reason = BoostReason::None;
    // App the boost is credited to (foreground first, then priority).
    std::wstring appKey;
    std::wstring appName;
    // App keys whose processes must never be power throttled right now.
    std::vector<std::wstring> unthrottle;
};

struct Tuning {
    // Creative tool counts as "being driven" within this much input idle.
    uint64_t interactIdleMs = 5000;
    // Heavy foreground work stops counting once the user walks away.
    uint64_t heavyIdleMs = 5 * 60 * 1000;
    // Sustained load thresholds: above the threshold for at least sustainMs
    // (two load samples apart) so an app launch or a single spike never boosts.
    double renderCpuPercent = 15.0;
    double renderGpuPercent = 25.0;
    double heavyCpuPercent = 35.0;
    uint64_t sustainMs = 1500;
    // Borderless fullscreen counts as a game above this 3D engine load.
    double fullscreenGame3dPercent = 20.0;
    // Hold after demand stops, per reason.
    uint64_t gameHoldMs = 3000;
    uint64_t creativeHoldMs = 8000;
    uint64_t renderHoldMs = 20000;
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
    void Reset() noexcept;

    [[nodiscard]] const Tuning& Settings() const noexcept {
        return m_tuning;
    }

private:
    struct Track {
        std::wstring name;
        BoostReason reason = BoostReason::None;
        uint64_t holdUntilMs = 0;
        uint64_t lastDemandMs = 0;
        // When the current busy run started (0 = not busy).
        uint64_t renderSinceMs = 0;
        uint64_t heavySinceMs = 0;
    };

    [[nodiscard]] BoostReason Demand(const AppSample& app, uint64_t nowMs, uint64_t idleMs, Track& track) const;
    [[nodiscard]] uint64_t HoldMs(BoostReason reason) const noexcept;

    Tuning m_tuning;
    std::unordered_map<std::wstring, Track> m_tracks;
};

}  // namespace autopower
