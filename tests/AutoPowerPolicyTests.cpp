// AutoPowerPolicy unit tests (portable; also built on Windows with -DDOCK_BUILD_TESTS=ON).

#include "AutoPowerPolicy.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                              \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (false)

using autopower::AppKind;
using autopower::AppSample;
using autopower::BoostReason;
using autopower::ClassifyApp;
using autopower::Decision;
using autopower::Inputs;
using autopower::Policy;

AppSample App(const wchar_t* key, AppKind kind, bool foreground = false) {
    AppSample app;
    app.key = key;
    app.name = key;
    app.kind = kind;
    app.foreground = foreground;
    return app;
}

Inputs At(uint64_t nowMs, uint64_t idleMs, std::vector<AppSample> apps) {
    Inputs inputs;
    inputs.nowMs = nowMs;
    inputs.idleMs = idleMs;
    inputs.apps = std::move(apps);
    return inputs;
}

bool Unthrottled(const Decision& decision, const wchar_t* key) {
    return std::find(decision.unthrottle.begin(), decision.unthrottle.end(), key) != decision.unthrottle.end();
}

void TestClassification() {
    EXPECT(ClassifyApp(L"adobe premiere pro.exe", L"") == AppKind::Creative);
    EXPECT(ClassifyApp(L"blender.exe", L"c:\\program files\\blender foundation\\blender 4.2\\blender.exe") ==
        AppKind::Creative);
    EXPECT(ClassifyApp(L"resolve.exe", L"") == AppKind::Creative);
    EXPECT(ClassifyApp(L"vegas210.exe", L"") == AppKind::Creative);
    EXPECT(ClassifyApp(L"ableton live 12 suite.exe", L"") == AppKind::Creative);
    EXPECT(ClassifyApp(L"photo.exe", L"c:\\program files\\affinity\\photo 2\\photo.exe") == AppKind::Creative);
    EXPECT(ClassifyApp(L"photo.exe", L"c:\\tools\\photo.exe") == AppKind::Other);

    EXPECT(ClassifyApp(L"eldenring.exe",
               L"d:\\steamlibrary\\steamapps\\common\\elden ring\\game\\eldenring.exe") == AppKind::Game);
    EXPECT(ClassifyApp(L"fortniteclient-win64-shipping.exe",
               L"c:\\program files\\epic games\\fortnite\\fortnitegame\\binaries\\win64\\"
               L"fortniteclient-win64-shipping.exe") == AppKind::Game);
    EXPECT(ClassifyApp(L"epicgameslauncher.exe",
               L"c:\\program files (x86)\\epic games\\launcher\\portal\\binaries\\win64\\epicgameslauncher.exe") ==
        AppKind::Other);
    EXPECT(ClassifyApp(L"unitycrashhandler64.exe", L"d:\\steamapps\\common\\game\\unitycrashhandler64.exe") ==
        AppKind::Other);
    EXPECT(ClassifyApp(L"wallpaper32.exe", L"c:\\steam\\steamapps\\common\\wallpaper_engine\\wallpaper32.exe") ==
        AppKind::Other);

    EXPECT(ClassifyApp(L"chrome.exe", L"") == AppKind::NeverGame);
    EXPECT(ClassifyApp(L"powerpnt.exe", L"") == AppKind::NeverGame);
    EXPECT(ClassifyApp(L"notepad.exe", L"c:\\windows\\notepad.exe") == AppKind::Other);
    EXPECT(ClassifyApp(L"", L"") == AppKind::Other);
}

void TestIdleSystemStaysEfficient() {
    Policy policy;
    const Decision decision = policy.Update(At(1000, 60000, {App(L"notepad.exe", AppKind::Other, true)}));
    EXPECT(!decision.fullPower);
    EXPECT(decision.reason == BoostReason::None);
    EXPECT(decision.unthrottle.empty());
}

void TestFocusedGameNeverThrottles() {
    Policy policy;
    // Focused game: full power immediately, even with no input and no load.
    Decision decision = policy.Update(At(1000, 600000, {App(L"eldenring.exe", AppKind::Game, true)}));
    EXPECT(decision.fullPower);
    EXPECT(decision.reason == BoostReason::Game);
    EXPECT(decision.appKey == L"eldenring.exe");
    EXPECT(Unthrottled(decision, L"eldenring.exe"));

    // Alt+Tab glance: the hold keeps it boosted.
    decision = policy.Update(At(2500, 0,
        {App(L"eldenring.exe", AppKind::Game), App(L"discord.exe", AppKind::NeverGame, true)}));
    EXPECT(decision.fullPower);
    EXPECT(decision.reason == BoostReason::Game);
    EXPECT(Unthrottled(decision, L"eldenring.exe"));

    // Hold expired while out of focus: back to efficient, still exempt from EcoQoS.
    decision = policy.Update(At(5000, 0,
        {App(L"eldenring.exe", AppKind::Game), App(L"discord.exe", AppKind::NeverGame, true)}));
    EXPECT(!decision.fullPower);
    EXPECT(Unthrottled(decision, L"eldenring.exe"));
}

void TestGameDetectionHeuristics() {
    Policy policy;
    AppSample registered = App(L"indie.exe", AppKind::Other, true);
    registered.registeredGame = true;
    EXPECT(policy.Update(At(1000, 0, {registered})).reason == BoostReason::Game);

    Policy exclusive;
    AppSample d3d = App(L"oldgame.exe", AppKind::Other, true);
    d3d.exclusiveFullscreen = true;
    EXPECT(exclusive.Update(At(1000, 0, {d3d})).reason == BoostReason::Game);

    // Borderless fullscreen only counts with real 3D load.
    Policy borderless;
    AppSample quiet = App(L"kiosk.exe", AppKind::Other, true);
    quiet.fullscreen = true;
    quiet.gpu3dPercent = 3.0;
    EXPECT(!borderless.Update(At(1000, 0, {quiet})).fullPower);
    quiet.gpu3dPercent = 65.0;
    EXPECT(borderless.Update(At(2000, 0, {quiet})).reason == BoostReason::Game);

    // Fullscreen video in a browser is never a game.
    Policy browser;
    AppSample video = App(L"chrome.exe", AppKind::NeverGame, true);
    video.fullscreen = true;
    video.exclusiveFullscreen = true;
    video.gpu3dPercent = 40.0;
    EXPECT(!browser.Update(At(1000, 30000, {video})).fullPower);
}

void TestCreativeInteractionBoosts() {
    Policy policy;
    const AppSample premiere = App(L"adobe premiere pro.exe", AppKind::Creative, true);

    // Opened but untouched: efficient, yet exempt from background throttling.
    Decision decision = policy.Update(At(1000, 60000, {premiere}));
    EXPECT(!decision.fullPower);
    EXPECT(Unthrottled(decision, L"adobe premiere pro.exe"));

    // Scrubbing the timeline (fresh input): full power right away.
    decision = policy.Update(At(2000, 40, {premiere}));
    EXPECT(decision.fullPower);
    EXPECT(decision.reason == BoostReason::Creative);

    // Short pause between scrubs stays boosted inside the hold.
    decision = policy.Update(At(8000, 6000, {premiere}));
    EXPECT(decision.fullPower);

    // Hold (8 s after the last driven sample) has lapsed.
    decision = policy.Update(At(10500, 8500, {premiere}));
    EXPECT(!decision.fullPower);
}

void TestBackgroundRenderBoosts() {
    Policy policy;
    AppSample blender = App(L"blender.exe", AppKind::Creative);
    blender.cpuPercent = 85.0;
    const AppSample browser = App(L"chrome.exe", AppKind::NeverGame, true);

    // One busy sample is not sustained yet, and fast re-evaluations that
    // reuse it (input ticks between load samples) do not count as more load.
    Decision decision = policy.Update(At(1000, 0, {blender, browser}));
    EXPECT(!decision.fullPower);
    EXPECT(Unthrottled(decision, L"blender.exe"));
    AppSample staleBlender = blender;
    staleBlender.freshLoad = false;
    EXPECT(!policy.Update(At(1500, 0, {staleBlender, browser})).fullPower);
    EXPECT(!policy.Update(At(2000, 0, {staleBlender, browser})).fullPower);

    decision = policy.Update(At(2500, 0, {blender, browser}));
    EXPECT(decision.fullPower);
    EXPECT(decision.reason == BoostReason::Render);
    EXPECT(decision.appKey == L"blender.exe");

    // GPU-only renders (Cycles on GPU, NVENC exports) count too.
    Policy gpuPolicy;
    AppSample resolve = App(L"resolve.exe", AppKind::Creative);
    resolve.gpuPercent = 90.0;
    static_cast<void>(gpuPolicy.Update(At(1000, 0, {resolve})));
    EXPECT(gpuPolicy.Update(At(2500, 0, {resolve})).reason == BoostReason::Render);

    // Render finishes: held, then released.
    blender.cpuPercent = 1.0;
    decision = policy.Update(At(4000, 0, {blender, browser}));
    EXPECT(decision.fullPower);
    decision = policy.Update(At(23000, 0, {blender, browser}));
    EXPECT(!decision.fullPower);
}

void TestHeavyForegroundWork() {
    Policy policy;
    AppSample build = App(L"devenv.exe", AppKind::Other, true);
    build.cpuPercent = 70.0;
    EXPECT(!policy.Update(At(1000, 1000, {build})).fullPower);
    const Decision decision = policy.Update(At(2500, 1000, {build}));
    EXPECT(decision.fullPower);
    EXPECT(decision.reason == BoostReason::Heavy);
    // Heavy work alone does not exempt an ordinary app from EcoQoS once released.
    build.cpuPercent = 2.0;
    const Decision after = policy.Update(At(20000, 1000, {build}));
    EXPECT(!after.fullPower);
    EXPECT(!Unthrottled(after, L"devenv.exe"));

    // Background heavy load from an ordinary app never boosts.
    Policy background;
    AppSample indexer = App(L"indexer.exe", AppKind::Other);
    indexer.cpuPercent = 90.0;
    static_cast<void>(background.Update(At(1000, 0, {indexer})));
    EXPECT(!background.Update(At(2500, 0, {indexer})).fullPower);
}

void TestFastProbeConfirmsInMilliseconds() {
    // After input the engine samples the focused app every 100 ms: sustained
    // load is confirmed ~300 ms after it starts.
    Policy policy;
    AppSample build = App(L"devenv.exe", AppKind::Other, true);
    build.cpuPercent = 80.0;
    EXPECT(!policy.Update(At(1000, 50, {build})).fullPower);
    EXPECT(!policy.Update(At(1100, 150, {build})).fullPower);
    EXPECT(!policy.Update(At(1200, 250, {build})).fullPower);
    const Decision decision = policy.Update(At(1300, 350, {build}));
    EXPECT(decision.fullPower);
    EXPECT(decision.reason == BoostReason::Heavy);

    // A creative export started from the UI is confirmed just as fast.
    Policy render;
    AppSample resolve = App(L"resolve.exe", AppKind::Creative);
    resolve.gpuPercent = 95.0;
    static_cast<void>(render.Update(At(1000, 60000, {resolve})));
    EXPECT(!render.Update(At(1100, 60000, {resolve})).fullPower);
    EXPECT(render.Update(At(1300, 60000, {resolve})).reason == BoostReason::Render);
}

void TestShortSpikeNeverBoosts() {
    Policy policy;
    AppSample app = App(L"explorer2.exe", AppKind::Other, true);
    app.cpuPercent = 90.0;
    static_cast<void>(policy.Update(At(1000, 0, {app})));
    static_cast<void>(policy.Update(At(1100, 0, {app})));
    static_cast<void>(policy.Update(At(1200, 0, {app})));
    app.cpuPercent = 2.0;
    EXPECT(!policy.Update(At(1300, 0, {app})).fullPower);
    app.cpuPercent = 90.0;
    // The run restarts from scratch after the dip.
    EXPECT(!policy.Update(At(1400, 0, {app})).fullPower);
    EXPECT(!policy.Update(At(1600, 0, {app})).fullPower);
    EXPECT(policy.Update(At(1700, 0, {app})).fullPower);
}

void TestForegroundWinsAttribution() {
    Policy policy;
    AppSample blender = App(L"blender.exe", AppKind::Creative);
    blender.cpuPercent = 80.0;
    const AppSample game = App(L"game.exe", AppKind::Game, true);
    static_cast<void>(policy.Update(At(1000, 0, {blender, game})));
    const Decision decision = policy.Update(At(2500, 0, {blender, game}));
    EXPECT(decision.fullPower);
    EXPECT(decision.appKey == L"game.exe");
    EXPECT(decision.reason == BoostReason::Game);
    EXPECT(Unthrottled(decision, L"blender.exe"));
    EXPECT(Unthrottled(decision, L"game.exe"));
}

void TestExitedAppReleasesImmediately() {
    Policy policy;
    EXPECT(policy.Update(At(1000, 0, {App(L"game.exe", AppKind::Game, true)})).fullPower);
    const Decision decision = policy.Update(At(1500, 0, {App(L"explorer.exe", AppKind::NeverGame, true)}));
    EXPECT(!decision.fullPower);
    EXPECT(decision.unthrottle.empty());
}

void TestResetClearsHolds() {
    Policy policy;
    EXPECT(policy.Update(At(1000, 0, {App(L"game.exe", AppKind::Game, true)})).fullPower);
    policy.Reset();
    EXPECT(!policy.Update(At(1100, 0, {App(L"game.exe", AppKind::Game)})).fullPower);
}

}  // namespace

int main() {
    TestClassification();
    TestIdleSystemStaysEfficient();
    TestFocusedGameNeverThrottles();
    TestGameDetectionHeuristics();
    TestCreativeInteractionBoosts();
    TestBackgroundRenderBoosts();
    TestHeavyForegroundWork();
    TestFastProbeConfirmsInMilliseconds();
    TestShortSpikeNeverBoosts();
    TestForegroundWinsAttribution();
    TestExitedAppReleasesImmediately();
    TestResetClearsHolds();
    if (g_failures != 0) {
        std::fprintf(stderr, "AutoPowerPolicyTests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("AutoPowerPolicyTests: all passed\n");
    return 0;
}
