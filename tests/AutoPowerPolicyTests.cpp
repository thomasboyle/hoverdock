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
using autopower::History;
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

    EXPECT(ClassifyApp(L"ollama.exe", L"") == AppKind::Ai);
    EXPECT(ClassifyApp(L"lm studio.exe", L"") == AppKind::Ai);
    EXPECT(ClassifyApp(L"python.exe", L"d:\\ai\\comfyui\\python_embeded\\python.exe") == AppKind::Ai);
    EXPECT(ClassifyApp(L"python.exe", L"c:\\python312\\python.exe") == AppKind::Other);
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
    // Focused game: full power immediately, even with no load.
    Decision decision = policy.Update(At(1000, 0, {App(L"eldenring.exe", AppKind::Game, true)}));
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
    AppSample premiere = App(L"adobe premiere pro.exe", AppKind::Creative, true);
    premiere.cpuCores = 0.05;

    // Opened but untouched: efficient, yet exempt from background throttling.
    Decision decision = policy.Update(At(1000, 60000, {premiere}));
    EXPECT(!decision.fullPower);
    EXPECT(Unthrottled(decision, L"adobe premiere pro.exe"));

    // Pointer moving over the window with no work behind it: stays efficient.
    decision = policy.Update(At(1500, 40, {premiere}));
    EXPECT(!decision.fullPower);

    // Scrubbing the timeline (fresh input and real load): full power right away.
    premiere.cpuCores = 1.6;
    decision = policy.Update(At(2000, 40, {premiere}));
    EXPECT(decision.fullPower);
    EXPECT(decision.reason == BoostReason::Creative);

    // Short pause between scrubs stays boosted inside the hold.
    premiere.cpuCores = 0.05;
    decision = policy.Update(At(8000, 6000, {premiere}));
    EXPECT(decision.fullPower);

    // Hold (8 s after the last driven sample) has lapsed.
    decision = policy.Update(At(10500, 8500, {premiere}));
    EXPECT(!decision.fullPower);

    // GPU-bound viewport work counts as real load too.
    Policy viewport;
    AppSample blender = App(L"blender.exe", AppKind::Creative, true);
    blender.gpuPercent = 45.0;
    EXPECT(viewport.Update(At(1000, 100, {blender})).reason == BoostReason::Creative);
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

void TestSingleThreadedWorkBoosts() {
    // One saturated thread on a 32-thread machine is ~3% of it: too little by
    // machine share, but it is exactly the work that needs the turbo clock.
    Policy policy;
    AppSample script = App(L"python.exe", AppKind::Other, true);
    script.cpuPercent = 3.1;
    script.cpuCores = 1.0;
    EXPECT(!policy.Update(At(1000, 100, {script})).fullPower);
    const Decision decision = policy.Update(At(1300, 400, {script}));
    EXPECT(decision.fullPower);
    EXPECT(decision.reason == BoostReason::Heavy);

    // A creative export pinned to one thread counts as a render.
    Policy render;
    AppSample encoder = App(L"handbrake.exe", AppKind::Creative);
    encoder.cpuPercent = 3.0;
    encoder.cpuCores = 0.95;
    static_cast<void>(render.Update(At(1000, 60000, {encoder})));
    EXPECT(render.Update(At(1300, 60000, {encoder})).reason == BoostReason::Render);

    // Half a core of UI work is not heavy.
    Policy light;
    AppSample editor = App(L"code.exe", AppKind::Other, true);
    editor.cpuPercent = 1.5;
    editor.cpuCores = 0.5;
    static_cast<void>(light.Update(At(1000, 0, {editor})));
    EXPECT(!light.Update(At(1300, 0, {editor})).fullPower);
}

void TestKnownDemandingAppsBoostSooner() {
    Policy policy;
    AppSample build = App(L"cargo.exe", AppKind::Other, true);
    build.cpuCores = 6.0;
    build.cpuPercent = 40.0;
    build.knownDemanding = true;
    Decision decision = policy.Update(At(1000, 50, {build}));
    EXPECT(decision.fullPower);
    EXPECT(decision.reason == BoostReason::Heavy);
    // Newly confirmed sustained work is reported once, for the history.
    EXPECT(decision.confirmed.size() == 1 && decision.confirmed[0] == L"cargo.exe");
    decision = policy.Update(At(1100, 150, {build}));
    EXPECT(decision.fullPower);
    EXPECT(decision.confirmed.empty());

    // Unknown apps still wait for a sustained run, then get confirmed.
    Policy fresh;
    build.knownDemanding = false;
    decision = fresh.Update(At(1000, 50, {build}));
    EXPECT(!decision.fullPower);
    EXPECT(decision.confirmed.empty());
    decision = fresh.Update(At(1300, 350, {build}));
    EXPECT(decision.fullPower);
    EXPECT(decision.confirmed.size() == 1);

    // Interaction-driven boosts are not "demanding work" to remember.
    Policy game;
    EXPECT(game.Update(At(1000, 0, {App(L"game.exe", AppKind::Game, true)})).confirmed.empty());
}

void TestAwayFromGameReleases() {
    Policy policy;
    AppSample game = App(L"eldenring.exe", AppKind::Game, true);
    game.cpuCores = 3.0;
    game.cpuPercent = 20.0;
    game.gpu3dPercent = 90.0;
    EXPECT(policy.Update(At(1000, 0, {game})).reason == BoostReason::Game);
    EXPECT(policy.Update(At(179000, 178500, {game})).fullPower);
    // Paused in a menu for minutes: game demand stops, and its CPU load does
    // not count as Heavy instead. Released once the short hold lapses.
    EXPECT(policy.Update(At(181000, 180500, {game})).fullPower);
    const Decision away = policy.Update(At(185000, 184500, {game}));
    EXPECT(!away.fullPower);
    EXPECT(Unthrottled(away, L"eldenring.exe"));
    // Back on the controller: full power at once.
    EXPECT(policy.Update(At(186000, 10, {game})).reason == BoostReason::Game);
}

void TestCallsAudioAndAi() {
    // Video call in the background doing real work (blur, encode).
    Policy call;
    AppSample teams = App(L"ms-teams.exe", AppKind::NeverGame);
    teams.cameraActive = true;
    teams.cpuCores = 1.2;
    EXPECT(Unthrottled(call.Update(At(1000, 0, {teams})), L"ms-teams.exe"));
    EXPECT(call.Update(At(1300, 0, {teams})).reason == BoostReason::Call);
    // Audio-only call: never throttled, but no boost.
    Policy voice;
    AppSample discord = App(L"discord.exe", AppKind::NeverGame);
    discord.micActive = true;
    discord.cpuCores = 1.2;
    static_cast<void>(voice.Update(At(1000, 0, {discord})));
    const Decision voiceDecision = voice.Update(At(1300, 0, {discord}));
    EXPECT(!voiceDecision.fullPower);
    EXPECT(Unthrottled(voiceDecision, L"discord.exe"));

    // Music playback: never throttled, never boosted.
    Policy music;
    AppSample spotify = App(L"spotify.exe", AppKind::NeverGame);
    spotify.audioActive = true;
    const Decision musicDecision = music.Update(At(1000, 0, {spotify}));
    EXPECT(!musicDecision.fullPower);
    EXPECT(Unthrottled(musicDecision, L"spotify.exe"));

    // A DAW playing back in the background must not underrun.
    Policy daw;
    AppSample ableton = App(L"ableton live 12 suite.exe", AppKind::Creative);
    ableton.audioActive = true;
    EXPECT(daw.Update(At(1000, 60000, {ableton})).reason == BoostReason::Audio);

    // Local LLM generating in the background.
    Policy llm;
    AppSample ollama = App(L"ollama.exe", AppKind::Ai);
    ollama.gpuPercent = 95.0;
    ollama.gpuComputePercent = 95.0;
    EXPECT(Unthrottled(llm.Update(At(1000, 0, {ollama})), L"ollama.exe"));
    EXPECT(llm.Update(At(1300, 0, {ollama})).reason == BoostReason::Ai);
    // An idle AI runtime stays efficient.
    Policy idleLlm;
    AppSample waiting = App(L"ollama.exe", AppKind::Ai);
    static_cast<void>(idleLlm.Update(At(1000, 0, {waiting})));
    EXPECT(!idleLlm.Update(At(1300, 0, {waiting})).fullPower);

    // Any app keeping a compute engine busy (a PyTorch script) is AI work.
    Policy torch;
    AppSample python = App(L"python.exe", AppKind::Other);
    python.gpuPercent = 80.0;
    python.gpuComputePercent = 80.0;
    static_cast<void>(torch.Update(At(1000, 0, {python})));
    EXPECT(torch.Update(At(1300, 0, {python})).reason == BoostReason::Ai);
}

void TestLongJobConfirmedOnceAndRelabelled() {
    // Driving a creative tool, then letting an export run: the export is
    // remembered once (not every sample), and shows as a render once the
    // interaction hold lapses.
    Policy policy;
    AppSample blender = App(L"blender.exe", AppKind::Creative, true);
    blender.cpuCores = 4.0;
    blender.cpuPercent = 50.0;
    size_t confirmations = 0;
    Decision decision;
    for (uint64_t tick = 0; tick < 20; ++tick) {
        const uint64_t now = 1000 + tick * 1500;
        decision = policy.Update(At(now, tick < 2 ? 50 : (tick - 1) * 1500, {blender}));
        confirmations += decision.confirmed.size();
        EXPECT(decision.fullPower);
    }
    EXPECT(confirmations == 1);
    EXPECT(decision.reason == BoostReason::Render);

    // A dip ends the run; the next run is a new episode.
    blender.cpuCores = 0.0;
    blender.cpuPercent = 0.0;
    static_cast<void>(policy.Update(At(40000, 60000, {blender})));
    blender.cpuCores = 4.0;
    blender.cpuPercent = 50.0;
    confirmations = 0;
    for (uint64_t tick = 0; tick < 4; ++tick) {
        confirmations += policy.Update(At(41000 + tick * 1500, 60000, {blender})).confirmed.size();
    }
    EXPECT(confirmations == 1);
}

void TestReusedDecisionMatchesFresh() {
    // The engine reuses one Decision across updates; it must always equal a
    // freshly built one, including when the exempt set shrinks.
    Policy fresh;
    Policy reused;
    Decision decision;
    // The reused path also promises a layout, bumped whenever the app list
    // (keys or order) differs from the previous update's.
    uint64_t layout = 1;
    std::vector<std::wstring_view> lastKeys;
    AppSample blender = App(L"blender.exe", AppKind::Creative);
    AppSample game = App(L"a-very-long-game-executable-name.exe", AppKind::Game, true);
    AppSample spotify = App(L"spotify.exe", AppKind::NeverGame);
    AppSample code = App(L"code.exe", AppKind::Other);
    for (uint64_t tick = 0; tick < 60; ++tick) {
        blender.cpuCores = tick % 7 < 4 ? 3.0 : 0.0;
        spotify.audioActive = tick % 11 < 5;
        code.foreground = tick % 13 > 8;
        game.foreground = !code.foreground;
        std::vector<AppSample> apps = {blender, code, spotify};
        if (tick % 17 < 12) {
            apps.push_back(game);
        } else if (tick % 2 == 0) {
            // Same size, different app in the last slot.
            apps.push_back(App(L"notepad.exe", AppKind::Other));
        }
        if (tick % 3 == 0) {
            std::reverse(apps.begin(), apps.end());
        }
        Inputs inputs = At(1000 + tick * 700, (tick % 5) * 2000, apps);
        const Decision expected = fresh.Update(inputs);
        std::vector<std::wstring_view> keys;
        for (const AppSample& app : apps) {
            keys.push_back(app.key);
        }
        layout += keys != lastKeys ? 1 : 0;
        lastKeys = keys;
        inputs.layout = layout;
        reused.Update(inputs, decision);
        EXPECT(decision.fullPower == expected.fullPower);
        EXPECT(decision.reason == expected.reason);
        EXPECT(decision.appKey == expected.appKey);
        EXPECT(decision.appName == expected.appName);
        EXPECT(decision.unthrottle == expected.unthrottle);
        EXPECT(decision.confirmed == expected.confirmed);
    }
}

void TestHistory() {
    History history;
    EXPECT(!history.IsKnown(L"cargo.exe", 100));
    EXPECT(history.Record(L"cargo.exe", 100));
    EXPECT(!history.IsKnown(L"cargo.exe", 100));
    EXPECT(history.Record(L"cargo.exe", 101));
    EXPECT(history.IsKnown(L"cargo.exe", 101));
    EXPECT(history.IsKnown(L"cargo.exe", 101 + History::kMaxAgeDays));
    EXPECT(!history.IsKnown(L"cargo.exe", 102 + History::kMaxAgeDays));
    EXPECT(!history.Record(L"bad\tkey", 100));

    history.Record(L"blender.exe", 90);
    History loaded;
    loaded.Parse(history.Serialize());
    EXPECT(loaded.Size() == 2);
    EXPECT(loaded.IsKnown(L"cargo.exe", 105));
    EXPECT(!loaded.IsKnown(L"blender.exe", 105));

    // Garbage lines are skipped; CRLF is tolerated.
    History messy;
    messy.Parse(L"x\t1\tfoo.exe\r\n3\t200\tbar baz.exe\r\n\n2\t\tnope.exe\n");
    EXPECT(messy.Size() == 1);
    EXPECT(messy.IsKnown(L"bar baz.exe", 200));

    // Old entries fade, and the store stays bounded.
    loaded.Prune(200);
    EXPECT(loaded.Size() == 0);
    History big;
    for (size_t index = 0; index < History::kMaxEntries + 20; ++index) {
        big.Record(L"app" + std::to_wstring(index) + L".exe", static_cast<uint32_t>(index));
    }
    EXPECT(big.Size() <= History::kMaxEntries);
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
    TestSingleThreadedWorkBoosts();
    TestKnownDemandingAppsBoostSooner();
    TestAwayFromGameReleases();
    TestCallsAudioAndAi();
    TestLongJobConfirmedOnceAndRelabelled();
    TestReusedDecisionMatchesFresh();
    TestHistory();
    if (g_failures != 0) {
        std::fprintf(stderr, "AutoPowerPolicyTests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("AutoPowerPolicyTests: all passed\n");
    return 0;
}
