#include "AutoPowerPolicy.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <utility>

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

// Local AI runtimes: LLM servers and chat apps, diffusion and speech UIs.
constexpr std::wstring_view kAiExes[] = {
    L"ollama.exe",
    L"ollama app.exe",
    L"ollama_llama_server.exe",
    L"lm studio.exe",
    L"lms.exe",
    L"llama-server.exe",
    L"llama-cli.exe",
    L"koboldcpp.exe",
    L"jan.exe",
    L"gpt4all.exe",
    L"comfyui.exe",
    L"amuse.exe",
    L"anythingllm.exe",
    L"msty.exe",
};

// Install folders of AI UIs that run on a bundled python.exe or node.exe.
constexpr std::wstring_view kAiPaths[] = {
    L"\\lm studio\\",
    L"\\.lmstudio\\",
    L"\\ollama\\",
    L"\\comfyui",
    L"\\stable-diffusion-webui",
    L"\\text-generation-webui",
    L"\\invokeai",
    L"\\fooocus",
    L"\\koboldcpp",
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
    if (MatchesAny(exeLower, kAiExes) || (!pathLower.empty() && ContainsAny(pathLower, kAiPaths))) {
        return AppKind::Ai;
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
    case BoostReason::Audio:
        return L"Audio";
    case BoostReason::Render:
        return L"Render";
    case BoostReason::Call:
        return L"Call";
    case BoostReason::Ai:
        return L"AI";
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
    case BoostReason::Audio:
        return m_tuning.audioHoldMs;
    case BoostReason::Render:
        return m_tuning.renderHoldMs;
    case BoostReason::Call:
        return m_tuning.callHoldMs;
    case BoostReason::Ai:
        return m_tuning.aiHoldMs;
    case BoostReason::Heavy:
        return m_tuning.heavyHoldMs;
    case BoostReason::None:
        break;
    }
    return 0;
}

bool Policy::Sustained(const Run& run, uint64_t stamp, bool known) const noexcept {
    if (run.samples <= 0) {
        return false;
    }
    if (known) {
        return true;
    }
    return run.samples >= m_tuning.sustainSamples && stamp - run.sinceMs >= m_tuning.sustainMs;
}

bool Policy::FirstDemand(Run& run) noexcept {
    const bool first = !run.confirmed;
    run.confirmed = true;
    return first;
}

BoostReason Policy::Demand(
    const AppSample& app, uint64_t nowMs, uint64_t idleMs, Track& track, bool& newRun) const {
    newRun = false;
    // Busy runs advance only on fresh load samples; nowMs is never 0 in
    // practice, keep 0 as "idle".
    const uint64_t stamp = (std::max)(nowMs, uint64_t{1});
    const auto advance = [stamp](Run& run, bool busy) {
        run.sinceMs = busy ? (run.sinceMs != 0 ? run.sinceMs : stamp) : 0;
        run.samples = busy ? run.samples + 1 : 0;
        run.confirmed = busy && run.confirmed;
    };
    const bool gameKind = app.kind == AppKind::Game || app.registeredGame;
    if (app.freshLoad) {
        advance(track.render, app.kind == AppKind::Creative &&
                (app.cpuPercent >= m_tuning.renderCpuPercent || app.cpuCores >= m_tuning.renderCores ||
                    app.gpuPercent >= m_tuning.renderGpuPercent));
        advance(track.call, app.cameraActive &&
                (app.cpuCores >= m_tuning.callCores || app.gpuPercent >= m_tuning.callGpuPercent));
        const bool aiRuntimeBusy = app.kind == AppKind::Ai &&
            (app.cpuCores >= m_tuning.aiCores || app.gpuPercent >= m_tuning.aiGpuPercent);
        const bool computeBusy = !gameKind && app.kind != AppKind::Creative &&
            app.gpuComputePercent >= m_tuning.computeGpuPercent;
        advance(track.ai, aiRuntimeBusy || computeBusy);
    }
    // Heavy work only counts in the foreground; losing focus ends the run.
    if (!app.foreground) {
        track.heavy = {};
    } else if (app.freshLoad) {
        advance(track.heavy,
            app.cpuPercent >= m_tuning.heavyCpuPercent || app.cpuCores >= m_tuning.heavyCores);
    }

    bool isGame = false;
    if (app.foreground) {
        const bool fullscreenGame = app.fullscreen && app.kind == AppKind::Other &&
            app.gpu3dPercent >= m_tuning.fullscreenGame3dPercent;
        isGame = app.kind == AppKind::Game ||
            (app.kind != AppKind::Creative && app.kind != AppKind::NeverGame && app.kind != AppKind::Ai &&
                (app.registeredGame || app.exclusiveFullscreen)) ||
            fullscreenGame;
        // A game nobody has touched for minutes (paused, AFK in a menu) does
        // not need full power; the next input brings it straight back.
        if (isGame && idleMs <= m_tuning.gameAwayMs) {
            return BoostReason::Game;
        }
        // Driving needs real work behind the input, not a pointer hovering
        // over the window.
        if (app.kind == AppKind::Creative && idleMs <= m_tuning.interactIdleMs &&
            (app.cpuCores >= m_tuning.creativeCores || app.gpuPercent >= m_tuning.creativeGpuPercent)) {
            return BoostReason::Creative;
        }
    }
    // Live audio in a creative tool (DAW playback or recording) is real-time
    // work: a slow clock ramp means dropouts.
    if (app.kind == AppKind::Creative && app.audioActive) {
        return BoostReason::Audio;
    }
    const bool known = app.knownDemanding;
    if (Sustained(track.render, stamp, known)) {
        newRun = FirstDemand(track.render);
        return BoostReason::Render;
    }
    if (Sustained(track.call, stamp, known)) {
        newRun = FirstDemand(track.call);
        return BoostReason::Call;
    }
    if (Sustained(track.ai, stamp, known)) {
        newRun = FirstDemand(track.ai);
        return BoostReason::Ai;
    }
    // Games are handled above (an away player must not fall back to Heavy).
    if (!isGame && !gameKind && Sustained(track.heavy, stamp, known) && idleMs <= m_tuning.heavyIdleMs) {
        newRun = FirstDemand(track.heavy);
        return BoostReason::Heavy;
    }
    return BoostReason::None;
}

Decision Policy::Update(const Inputs& inputs) {
    Decision decision;
    Update(inputs, decision);
    return decision;
}

namespace {
// Exempt-list versions are unique across every Policy in the process, so a
// decision can only ever match the list of the policy that filled it.
std::atomic<uint64_t> g_unthrottleVersion{0};
}  // namespace

void Policy::Update(const Inputs& inputs, Decision& decision) {
    const uint64_t now = inputs.nowMs;
    const uint64_t generation = ++m_generation;

    decision.fullPower = false;
    decision.reason = BoostReason::None;
    decision.appKey.clear();
    decision.appName.clear();
    decision.confirmed.clear();
    // The engine passes the same apps in the same order tick after tick (the
    // groups only change on a process scan): each input slot remembers its
    // track, so a key is only hashed when the slot's app changes.
    const bool sameLayout =
        inputs.layout != 0 && inputs.layout == m_layout && m_slots.size() == inputs.apps.size();
    if (m_slots.size() != inputs.apps.size()) {
        m_slots.assign(inputs.apps.size(), Slot{});
    }
    m_layout = inputs.layout;
    m_seen.clear();
    bool listChanged = false;
    const Track* foreground = nullptr;
    for (size_t index = 0; index < inputs.apps.size(); ++index) {
        const AppSample& app = inputs.apps[index];
        Slot& slot = m_slots[index];
        if (app.key.empty()) {
            slot = {};
            continue;
        }
        if (slot.track == nullptr || (!sameLayout && *slot.key != app.key)) {
            const auto it = m_tracks.try_emplace(std::wstring(app.key)).first;
            slot = {&it->first, &it->second};
            // A key's display name is fixed (both come from its executable).
            if (it->second.name != app.name) {
                it->second.name = app.name;
            }
        }
        Track& track = *slot.track;
        if (track.seenGeneration == generation) {
            continue;
        }
        track.seenGeneration = generation;
        m_seen.push_back(slot);
        if (app.foreground && foreground == nullptr) {
            foreground = &track;
        }
        bool newRun = false;
        const BoostReason demand = Demand(app, now, inputs.idleMs, track, newRun);
        if (newRun) {
            decision.confirmed.emplace_back(app.key);
        }
        if (demand != BoostReason::None) {
            if (demand >= track.reason || track.reasonUntilMs <= now) {
                track.reason = demand;
            }
            if (demand == track.reason) {
                track.reasonUntilMs = (std::max)(track.reasonUntilMs, now + HoldMs(demand));
            }
            track.holdUntilMs = (std::max)(track.holdUntilMs, now + HoldMs(demand));
            track.lastDemand = demand;
            track.lastDemandMs = now;
        } else if (track.holdUntilMs <= now) {
            track.reason = BoostReason::None;
        } else if (track.reasonUntilMs <= now && track.lastDemand != BoostReason::None) {
            track.reason = track.lastDemand;
            track.reasonUntilMs = track.holdUntilMs;
        }
        track.exemptNow = app.kind == AppKind::Game || app.kind == AppKind::Creative || app.kind == AppKind::Ai ||
            app.registeredGame || app.audioActive || app.cameraActive || app.micActive;
        const bool listed = track.exemptNow || (track.reason != BoostReason::None && track.holdUntilMs > now);
        listChanged = listChanged || listed != track.listed;
        track.listed = listed;
    }

    // Exited apps lose their hold immediately: there is nothing left to boost.
    if (m_tracks.size() != m_seen.size()) {
        for (auto it = m_tracks.begin(); it != m_tracks.end();) {
            if (it->second.seenGeneration != generation) {
                listChanged = listChanged || it->second.listed;
                it = m_tracks.erase(it);
            } else {
                ++it;
            }
        }
    }

    const std::wstring* bestKey = nullptr;
    const Track* best = nullptr;
    for (const Slot& slot : m_seen) {
        const Track& track = *slot.track;
        if (track.reason == BoostReason::None || track.holdUntilMs <= now) {
            continue;
        }
        const std::wstring& key = *slot.key;
        const bool isForeground = &track == foreground;
        const bool bestIsForeground = best != nullptr && best == foreground;
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

    // The exempt list is rebuilt (and sorted) only when membership changes,
    // and copied out only to a decision that does not hold it already.
    if (listChanged) {
        m_unthrottle.clear();
        for (const Slot& slot : m_seen) {
            if (slot.track->listed) {
                m_unthrottle.push_back(*slot.key);
            }
        }
        std::sort(m_unthrottle.begin(), m_unthrottle.end());
        m_unthrottleVersion = ++g_unthrottleVersion;
    }
    if (decision.unthrottleVersion != m_unthrottleVersion) {
        decision.unthrottle = m_unthrottle;
        decision.unthrottleVersion = m_unthrottleVersion;
    }
}

void Policy::Reset() noexcept {
    m_tracks.clear();
    m_slots.clear();
    m_layout = 0;
    m_seen.clear();
    m_unthrottle.clear();
    m_unthrottleVersion = ++g_unthrottleVersion;
}

bool History::Record(const std::wstring& key, uint32_t day) {
    if (key.empty() || key.find_first_of(L"\t\r\n") != std::wstring::npos) {
        return false;
    }
    Entry& entry = m_entries[key];
    if (entry.count < UINT32_MAX) {
        ++entry.count;
    }
    entry.lastDay = day;
    if (m_entries.size() > kMaxEntries) {
        Prune(day);
    }
    return true;
}

bool History::IsKnown(const std::wstring& key, uint32_t day) const {
    const auto it = m_entries.find(key);
    return it != m_entries.end() && it->second.count >= kKnownAfter &&
        (day < it->second.lastDay || day - it->second.lastDay <= kMaxAgeDays);
}

void History::Prune(uint32_t day) {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (day >= it->second.lastDay && day - it->second.lastDay > kMaxAgeDays) {
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    while (m_entries.size() > kMaxEntries) {
        // Least recently demanding first, then least often.
        auto oldest = m_entries.begin();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->second.lastDay < oldest->second.lastDay ||
                (it->second.lastDay == oldest->second.lastDay && it->second.count < oldest->second.count)) {
                oldest = it;
            }
        }
        m_entries.erase(oldest);
    }
}

std::wstring History::Serialize() const {
    std::vector<std::pair<std::wstring, Entry>> sorted(m_entries.begin(), m_entries.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    std::wstring text;
    for (const auto& [key, entry] : sorted) {
        text += std::to_wstring(entry.count) + L"\t" + std::to_wstring(entry.lastDay) + L"\t" + key + L"\n";
    }
    return text;
}

void History::Parse(std::wstring_view text) {
    m_entries.clear();
    const auto number = [](std::wstring_view field, uint32_t& out) {
        if (field.empty() || field.size() > 10) {
            return false;
        }
        uint64_t value = 0;
        for (const wchar_t ch : field) {
            if (ch < L'0' || ch > L'9') {
                return false;
            }
            value = value * 10 + static_cast<uint64_t>(ch - L'0');
        }
        if (value > UINT32_MAX) {
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    };
    while (!text.empty()) {
        const size_t end = text.find(L'\n');
        std::wstring_view line = text.substr(0, end);
        text = end == std::wstring_view::npos ? std::wstring_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == L'\r') {
            line.remove_suffix(1);
        }
        const size_t first = line.find(L'\t');
        const size_t second = first == std::wstring_view::npos ? first : line.find(L'\t', first + 1);
        if (second == std::wstring_view::npos || second + 1 >= line.size()) {
            continue;
        }
        Entry entry;
        if (!number(line.substr(0, first), entry.count) ||
            !number(line.substr(first + 1, second - first - 1), entry.lastDay) || entry.count == 0) {
            continue;
        }
        m_entries[std::wstring(line.substr(second + 1))] = entry;
    }
    if (m_entries.size() > kMaxEntries) {
        uint32_t newest = 0;
        for (const auto& [key, entry] : m_entries) {
            newest = (std::max)(newest, entry.lastDay);
        }
        Prune(newest);
    }
}

}  // namespace autopower
