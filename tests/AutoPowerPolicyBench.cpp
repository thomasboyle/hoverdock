// Automatic energy mode decision benchmark (portable; not a CTest).
// Mirrors the engine's per-tick decision path (AutoPowerEngine Tick: build
// the policy inputs from the app groups, Policy::Update, history lookups,
// unthrottle change check). The engine runs it every 100 ms while probing
// and every 1.5 s otherwise, so its cost is paid continuously at rest.
//
// Build with -DAUTOPOWER_BENCH_FROZEN_ONLY to time only the frozen driver
// against an older AutoPowerPolicy (before Update(inputs, decision)).
//
// Reports median ns per tick and heap allocations per tick for a typical
// (8 app groups) and a busy (64 app groups) desktop.

#include "AutoPowerPolicy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
std::atomic<unsigned long long> g_allocations{0};
}

// GCC cannot see that these replacements pair with each other.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* block = std::malloc(size == 0 ? 1 : size)) {
        return block;
    }
    throw std::bad_alloc();
}
void operator delete(void* block) noexcept {
    std::free(block);
}
void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

namespace {

using autopower::AppKind;
using autopower::AppSample;

// The engine's app group: owns its key and name (the sample may view them).
struct Group {
    std::wstring key;
    std::wstring name;
    AppSample sample;
};

const wchar_t* const kNames[] = {L"chrome.exe", L"blender.exe", L"steam.exe", L"spotify.exe", L"code.exe",
    L"ms-teams.exe", L"ollama.exe", L"explorer.exe", L"adobe premiere pro.exe", L"discord.exe"};
const AppKind kKinds[] = {AppKind::NeverGame, AppKind::Creative, AppKind::NeverGame, AppKind::NeverGame,
    AppKind::Other, AppKind::NeverGame, AppKind::Ai, AppKind::NeverGame, AppKind::Creative, AppKind::NeverGame};

std::unordered_map<std::wstring, Group> MakeGroups(size_t count) {
    std::unordered_map<std::wstring, Group> groups;
    for (size_t index = 0; index < count; ++index) {
        const size_t base = index % std::size(kNames);
        std::wstring key = kNames[base];
        if (index >= std::size(kNames)) {
            key.insert(0, L"app" + std::to_wstring(index) + L"-");
        }
        Group& group = groups[key];
        group.key = key;
        group.name = key.substr(0, key.size() - 4);
        group.sample.key = group.key;
        group.sample.name = group.name;
        group.sample.kind = kKinds[base];
        group.sample.audioActive = base == 3;
        group.sample.micActive = base == 9;
    }
    return groups;
}

// One engine tick's decision path, as AutoPowerEngine::Tick does it.
struct TickDriver {
    autopower::Policy policy;
    autopower::History history;
    autopower::Inputs inputs;
    std::vector<std::wstring> lastUnthrottle;
    unsigned changes = 0;

    void Tick(std::unordered_map<std::wstring, Group>& groups, const std::wstring& foregroundKey, uint64_t now,
        uint64_t idle, uint32_t day) {
        inputs.nowMs = now;
        inputs.idleMs = idle;
        inputs.apps.clear();
        inputs.apps.reserve(groups.size());
        for (const auto& [key, group] : groups) {
            AppSample app = group.sample;
            app.foreground = key == foregroundKey;
            app.knownDemanding = history.IsKnown(key, day);
            inputs.apps.push_back(std::move(app));
        }
        const autopower::Decision decision = policy.Update(inputs);
        for (auto& [key, group] : groups) {
            group.sample.freshLoad = false;
        }
        for (const std::wstring& key : decision.confirmed) {
            static_cast<void>(history.Record(key, day));
        }
        if (decision.unthrottle != lastUnthrottle) {
            lastUnthrottle = decision.unthrottle;
            ++changes;
        }
    }
};

#if !defined(AUTOPOWER_BENCH_FROZEN_ONLY)
// The engine's current tick (reused inputs/decision; knownDemanding resolved
// when groups are built and when history records). Same workload, same
// observable decisions as TickDriver.
struct EngineDriver {
    autopower::Policy policy;
    autopower::History history;
    autopower::Inputs inputs;
    autopower::Decision decision;
    std::vector<std::wstring> lastUnthrottle;
    uint64_t lastVersion = 0;
    unsigned changes = 0;

    void Tick(std::unordered_map<std::wstring, Group>& groups, const std::wstring& foregroundKey, uint64_t now,
        uint64_t idle, uint32_t day) {
        inputs.nowMs = now;
        inputs.idleMs = idle;
        const auto foregroundIt = groups.find(foregroundKey);
        const Group* foreground = foregroundIt != groups.end() ? &foregroundIt->second : nullptr;
        inputs.apps.resize(groups.size());
        inputs.layout = 1;  // the benchmark's groups never change
        size_t slot = 0;
        for (auto& [key, group] : groups) {
            AppSample& app = inputs.apps[slot++];
            app = group.sample;
            group.sample.freshLoad = false;
            app.foreground = &group == foreground;
        }
        policy.Update(inputs, decision);
        for (const std::wstring& key : decision.confirmed) {
            static_cast<void>(history.Record(key, day));
            const auto group = groups.find(key);
            if (group != groups.end()) {
                group->second.sample.knownDemanding = history.IsKnown(key, day);
            }
        }
        // The engine re-applies throttling when the version moves; the
        // content compare only feeds the behaviour fingerprint.
        if (decision.unthrottleVersion != lastVersion) {
            lastVersion = decision.unthrottleVersion;
            if (decision.unthrottle != lastUnthrottle) {
                lastUnthrottle = decision.unthrottle;
                ++changes;
            }
        }
    }
};

#endif

template <typename Driver>
void Run(const char* label, size_t groupCount) {
    constexpr int kTicks = 20000;
    constexpr int kRuns = 7;
    std::vector<double> nsPerTick;
    std::vector<double> allocsPerTick;
    unsigned checksum = 0;
    for (int run = 0; run < kRuns + 1; ++run) {
        auto groups = MakeGroups(groupCount);
        Driver driver;
        const std::wstring foreground = groups.begin()->first;
        const auto start = std::chrono::steady_clock::now();
        const unsigned long long allocsBefore = g_allocations.load();
        for (int tick = 0; tick < kTicks; ++tick) {
            // Loads drift: the focused app works in bursts, a creative tool
            // renders now and then, everything else idles.
            for (auto& [key, group] : groups) {
                group.sample.freshLoad = true;
                const bool burst = (tick / 50) % 3 == 0;
                group.sample.cpuCores = key == foreground && burst ? 1.2 : 0.05;
                group.sample.cpuPercent = group.sample.cpuCores * 100.0 / 16.0;
                group.sample.gpuPercent = group.sample.kind == AppKind::Creative && (tick / 200) % 2 == 0 ? 60.0 : 0.0;
            }
            driver.Tick(groups, foreground, 1000 + static_cast<uint64_t>(tick) * 100, (tick % 40) * 100, 20000);
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        const unsigned long long allocs = g_allocations.load() - allocsBefore;
        checksum += driver.changes;
        if (run == 0) {
            continue;  // warm-up
        }
        nsPerTick.push_back(ns / kTicks);
        allocsPerTick.push_back(static_cast<double>(allocs) / kTicks);
    }
    std::sort(nsPerTick.begin(), nsPerTick.end());
    std::sort(allocsPerTick.begin(), allocsPerTick.end());
    std::printf("%-7s groups=%zu  ns/tick median=%.1f min=%.1f max=%.1f  allocs/tick=%.2f  (check %u)\n", label,
        groupCount,
        nsPerTick[nsPerTick.size() / 2], nsPerTick.front(), nsPerTick.back(), allocsPerTick[allocsPerTick.size() / 2],
        checksum);
}

}  // namespace

int main() {
    // frozen: the tick as first benchmarked (738a384); engine: the tick as
    // AutoPowerEngine runs it now.
    Run<TickDriver>("frozen", 8);
    Run<TickDriver>("frozen", 64);
#if !defined(AUTOPOWER_BENCH_FROZEN_ONLY)
    Run<EngineDriver>("engine", 8);
    Run<EngineDriver>("engine", 64);
#endif
    return 0;
}
