#include "AutoPowerPolicy.h"

#include <algorithm>
#include <unordered_set>

namespace autopower {

namespace {

bool StartsWith(std::wstring_view text, std::wstring_view prefix) noexcept {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool Contains(std::wstring_view text, std::wstring_view needle) noexcept {
    return text.find(needle) != std::wstring_view::npos;
}

template <size_t N>
bool MatchesAny(std::wstring_view exe, const std::wstring_view (&names)[N]) noexcept {
    return std::find(std::begin(names), std::end(names), exe) != std::end(names);
}

template <size_t N>
bool StartsWithAny(std::wstring_view exe, const std::wstring_view (&prefixes)[N]) noexcept {
    return std::any_of(std::begin(prefixes), std::end(prefixes),
        [exe](std::wstring_view prefix) { return StartsWith(exe, prefix); });
}

template <size_t N>
bool ContainsAny(std::wstring_view text, const std::wstring_view (&needles)[N]) noexcept {
    return std::any_of(std::begin(needles), std::end(needles),
        [text](std::wstring_view needle) { return Contains(text, needle); });
}

// Video, motion, 3D, CAD, game engines, audio and photo tools whose timeline,
// viewport and render work should never be throttled.
constexpr std::wstring_view kCreativeExes[] = {
    // Adobe
    L"adobe premiere pro.exe",
    L"adobe premiere rush.exe",
    L"afterfx.exe",
    L"adobe media encoder.exe",
    L"photoshop.exe",
    L"illustrator.exe",
    L"indesign.exe",
    L"animate.exe",
    L"lightroom.exe",
    L"adobe audition.exe",
    L"character animator.exe",
    L"adobe substance 3d painter.exe",
    L"adobe substance 3d designer.exe",
    L"adobe substance 3d sampler.exe",
    L"adobe substance 3d stager.exe",
    // Video and motion
    L"resolve.exe",
    L"fusion.exe",
    L"avid media composer.exe",
    L"capcut.exe",
    L"filmora.exe",
    L"kdenlive.exe",
    L"shotcut.exe",
    L"openshot-qt.exe",
    L"lightworks.exe",
    L"edius.exe",
    L"handbrake.exe",
    L"obs64.exe",
    L"natron.exe",
    L"topaz video ai.exe",
    // 3D, CAD and game engines
    L"blender.exe",
    L"cinema 4d.exe",
    L"maya.exe",
    L"3dsmax.exe",
    L"houdini.exe",
    L"houdinifx.exe",
    L"houdinicore.exe",
    L"zbrush.exe",
    L"unrealeditor.exe",
    L"ue4editor.exe",
    L"unity.exe",
    L"sketchup.exe",
    L"rhino.exe",
    L"fusion360.exe",
    L"twinmotion.exe",
    L"lumion.exe",
    L"marvelous designer.exe",
    L"revit.exe",
    L"acad.exe",
    L"sldworks.exe",
    L"inventor.exe",
    L"modo.exe",
    // Audio
    L"fl64.exe",
    L"fl.exe",
    L"reaper.exe",
    L"studio one.exe",
    L"bitwig studio.exe",
    L"protools.exe",
    // Photo and paint
    L"captureone.exe",
    L"krita.exe",
    L"darktable.exe",
    L"rawtherapee.exe",
    L"clipstudiopaint.exe",
};

// Versioned executables (Vegas200.exe, Cubase13.exe, Nuke15.0.exe, ...).
constexpr std::wstring_view kCreativePrefixes[] = {
    L"vegas",
    L"cubase",
    L"nuendo",
    L"ableton live",
    L"nuke",
    L"godot",
    L"keyshot",
    L"gimp-",
    L"hitfilm",
    L"cinema 4d",
};

constexpr std::wstring_view kCreativePaths[] = {
    L"\\affinity\\",
    L"\\blackmagic design\\",
    L"\\maxon cinema 4d",
    L"\\side effects software\\",
};

constexpr std::wstring_view kGameLibraryPaths[] = {
    L"\\steamapps\\common\\",
    L"\\epic games\\",
    L"\\gog galaxy\\games\\",
    L"\\gog games\\",
    L"\\xboxgames\\",
    L"\\riot games\\",
    L"\\ubisoft game launcher\\games\\",
    L"\\ea games\\",
    L"\\rockstar games\\",
};

// Launchers, crash reporters, installers and anti-cheat living next to games.
constexpr std::wstring_view kGameLibraryNoise[] = {
    L"launcher",
    L"crash",
    L"setup",
    L"install",
    L"redist",
    L"vc_redist",
    L"anticheat",
    L"easyanticheat",
    L"battleye",
    L"riotclient",
    L"epicgameslauncher",
    L"epicwebhelper",
    L"unrealcefsubprocess",
    L"cefsharp",
    L"webhelper",
    L"wallpaper",
};

constexpr std::wstring_view kNeverGameExes[] = {
    L"chrome.exe",
    L"msedge.exe",
    L"firefox.exe",
    L"brave.exe",
    L"opera.exe",
    L"opera_gx.exe",
    L"vivaldi.exe",
    L"arc.exe",
    L"iexplore.exe",
    L"vlc.exe",
    L"mpv.exe",
    L"mpc-hc64.exe",
    L"mpc-be64.exe",
    L"potplayermini64.exe",
    L"wmplayer.exe",
    L"microsoft.media.player.exe",
    L"video.ui.exe",
    L"spotify.exe",
    L"powerpnt.exe",
    L"explorer.exe",
    L"applicationframehost.exe",
    L"mstsc.exe",
    L"msrdc.exe",
    L"vmconnect.exe",
    L"vmware.exe",
    L"virtualboxvm.exe",
    L"ms-teams.exe",
    L"teams.exe",
    L"zoom.exe",
    L"discord.exe",
    L"steamwebhelper.exe",
    L"steam.exe",
};

}  // namespace

AppKind ClassifyApp(std::wstring_view exeLower, std::wstring_view pathLower) {
    if (exeLower.empty()) {
        return AppKind::Other;
    }
    if (MatchesAny(exeLower, kCreativeExes) || StartsWithAny(exeLower, kCreativePrefixes) ||
        (!pathLower.empty() && ContainsAny(pathLower, kCreativePaths))) {
        return AppKind::Creative;
    }
    if (MatchesAny(exeLower, kNeverGameExes)) {
        return AppKind::NeverGame;
    }
    if (!pathLower.empty() && ContainsAny(pathLower, kGameLibraryPaths) &&
        !ContainsAny(exeLower, kGameLibraryNoise)) {
        return AppKind::Game;
    }
    return AppKind::Other;
}

const wchar_t* ReasonLabel(BoostReason reason) noexcept {
    switch (reason) {
    case BoostReason::Game:
        return L"Game";
    case BoostReason::Creative:
        return L"Creative";
    case BoostReason::Render:
        return L"Render";
    case BoostReason::Heavy:
        return L"Heavy";
    case BoostReason::None:
        break;
    }
    return L"None";
}

uint64_t Policy::HoldMs(BoostReason reason) const noexcept {
    switch (reason) {
    case BoostReason::Game:
        return m_tuning.gameHoldMs;
    case BoostReason::Creative:
        return m_tuning.creativeHoldMs;
    case BoostReason::Render:
        return m_tuning.renderHoldMs;
    case BoostReason::Heavy:
        return m_tuning.heavyHoldMs;
    case BoostReason::None:
        break;
    }
    return 0;
}

BoostReason Policy::Demand(const AppSample& app, uint64_t nowMs, uint64_t idleMs, Track& track) const {
    // Busy-run start times; nowMs is never 0 in practice, keep 0 as "idle".
    const uint64_t stamp = (std::max)(nowMs, uint64_t{1});
    const bool renderBusy = app.kind == AppKind::Creative &&
        (app.cpuPercent >= m_tuning.renderCpuPercent || app.gpuPercent >= m_tuning.renderGpuPercent);
    track.renderSinceMs = renderBusy ? (track.renderSinceMs != 0 ? track.renderSinceMs : stamp) : 0;
    const bool heavyBusy = app.foreground && app.cpuPercent >= m_tuning.heavyCpuPercent;
    track.heavySinceMs = heavyBusy ? (track.heavySinceMs != 0 ? track.heavySinceMs : stamp) : 0;

    if (app.foreground) {
        const bool fullscreenGame = app.fullscreen && app.kind == AppKind::Other &&
            app.gpu3dPercent >= m_tuning.fullscreenGame3dPercent;
        const bool isGame = app.kind == AppKind::Game ||
            (app.kind != AppKind::Creative && app.kind != AppKind::NeverGame &&
                (app.registeredGame || app.exclusiveFullscreen)) ||
            fullscreenGame;
        if (isGame) {
            return BoostReason::Game;
        }
        if (app.kind == AppKind::Creative && idleMs <= m_tuning.interactIdleMs) {
            return BoostReason::Creative;
        }
    }
    if (track.renderSinceMs != 0 && stamp - track.renderSinceMs >= m_tuning.sustainMs) {
        return BoostReason::Render;
    }
    if (track.heavySinceMs != 0 && stamp - track.heavySinceMs >= m_tuning.sustainMs &&
        idleMs <= m_tuning.heavyIdleMs) {
        return BoostReason::Heavy;
    }
    return BoostReason::None;
}

Decision Policy::Update(const Inputs& inputs) {
    const uint64_t now = inputs.nowMs;
    std::unordered_set<std::wstring> seen;
    seen.reserve(inputs.apps.size());

    Decision decision;
    for (const AppSample& app : inputs.apps) {
        if (app.key.empty() || !seen.insert(app.key).second) {
            continue;
        }
        Track& track = m_tracks[app.key];
        track.name = app.name;
        const BoostReason demand = Demand(app, now, inputs.idleMs, track);
        if (demand != BoostReason::None) {
            const bool expired = track.holdUntilMs <= now;
            if (expired || demand >= track.reason) {
                track.reason = demand;
            }
            track.holdUntilMs = (std::max)(track.holdUntilMs, now + HoldMs(demand));
            track.lastDemandMs = now;
        } else if (track.holdUntilMs <= now) {
            track.reason = BoostReason::None;
        }
        if (app.kind == AppKind::Game || app.kind == AppKind::Creative || app.registeredGame) {
            decision.unthrottle.push_back(app.key);
        }
    }

    // Exited apps lose their hold immediately: there is nothing left to boost.
    for (auto it = m_tracks.begin(); it != m_tracks.end();) {
        if (seen.find(it->first) == seen.end()) {
            it = m_tracks.erase(it);
        } else {
            ++it;
        }
    }

    const AppSample* foreground = nullptr;
    for (const AppSample& app : inputs.apps) {
        if (app.foreground) {
            foreground = &app;
            break;
        }
    }

    const std::wstring* bestKey = nullptr;
    const Track* best = nullptr;
    for (const auto& [key, track] : m_tracks) {
        if (track.reason == BoostReason::None || track.holdUntilMs <= now) {
            continue;
        }
        if (std::find(decision.unthrottle.begin(), decision.unthrottle.end(), key) == decision.unthrottle.end()) {
            decision.unthrottle.push_back(key);
        }
        const bool isForeground = foreground != nullptr && foreground->key == key;
        const bool bestIsForeground = bestKey != nullptr && foreground != nullptr && foreground->key == *bestKey;
        bool better = best == nullptr;
        if (!better && isForeground != bestIsForeground) {
            better = isForeground;
        } else if (!better && track.reason != best->reason) {
            better = track.reason > best->reason;
        } else if (!better && track.lastDemandMs != best->lastDemandMs) {
            better = track.lastDemandMs > best->lastDemandMs;
        } else if (!better) {
            better = key < *bestKey;
        }
        if (better) {
            best = &track;
            bestKey = &key;
        }
    }
    if (best != nullptr) {
        decision.fullPower = true;
        decision.reason = best->reason;
        decision.appKey = *bestKey;
        decision.appName = best->name;
    }
    std::sort(decision.unthrottle.begin(), decision.unthrottle.end());
    return decision;
}

void Policy::Reset() noexcept {
    m_tracks.clear();
}

}  // namespace autopower
