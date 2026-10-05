#pragma once

#include "TypeSafeClient.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// One side effect the search agent decided on. The agent loop only validates
// and queues these on its worker thread; DockApp performs them on the UI thread
// through the normal launch / ShellExecute paths.
struct SearchAgentAction {
    enum class Kind {
        LaunchApp,  // appId = catalog candidate id (cN)
        OpenUrl,    // target = validated http(s) URL
        OpenPath,   // target = existing local folder or non-executable file
    };

    Kind kind = Kind::LaunchApp;
    std::string appId;
    std::wstring target;
    std::wstring label;
};

struct SearchAgentResult {
    // llama-server could not be reached before the first step: caller falls
    // back to classic fuzzy search and must not show agent replies.
    bool serverUnavailable = false;
    std::vector<SearchAgentAction> actions;
    // Short reply from the model ("done.say") or a status/error line.
    std::wstring reply;
    int steps = 0;
};

// Model-free resolution of a Search query (first-time goals included).
struct SearchFastPlan {
    std::vector<SearchAgentAction> actions;
    // Which fast path matched (direct, app-confident, site, web-question,
    // folder, app-keywords, compound) for latency logs.
    std::wstring route;
};

// Local OpenAI-compatible client for llama-server (llama.cpp).
// Used by dock Search / launch prompt. Falls back to classic fuzzy ranking
// when the server is unreachable so Search still works offline.
class LlamaServerClient {
public:
    using StatusCallback = std::function<void(const std::wstring&)>;
    using CancelCallback = std::function<bool()>;

    // baseUrl e.g. http://127.0.0.1:8080 (no trailing slash). Empty uses default.
    [[nodiscard]] static LaunchJudgment ResolveApp(const std::wstring& baseUrl,
        const std::wstring& request, const std::vector<LaunchCandidate>& candidates);

    [[nodiscard]] static bool IsServerReachable(const std::wstring& baseUrl);

    // Lexical / substring ranking over installed apps (no network).
    [[nodiscard]] static LaunchJudgment ResolveAppFuzzy(const std::wstring& request,
        const std::vector<LaunchCandidate>& candidates);

    // ---- Agent-in-search -------------------------------------------------

    // True when the query reads like a goal/command ("play lofi on youtube",
    // "open downloads folder") rather than a bare app name ("chrome").
    [[nodiscard]] static bool LooksLikeAgentGoal(const std::wstring& request);

    // Goal with leading command verbs/fillers removed ("open the chrome" -> "chrome").
    [[nodiscard]] static std::wstring StripGoalVerbs(const std::wstring& request);

    // Model-free action for explicit URLs/domains, known user folders and
    // existing absolute paths ("go to github.com", "open downloads folder").
    [[nodiscard]] static std::optional<SearchAgentAction> DirectAction(const std::wstring& request);

    // Strong, unambiguous catalog hit after verb/filler stripping: exact or
    // normalized name, alias ("vscode", "word", "task manager"), lexical score
    // with margin, acronym ("vsc"), unique prefix, or a 1-2 edit typo
    // ("chorme"). Empty when unsure.
    [[nodiscard]] static std::string ConfidentAppId(const std::wstring& appText,
        const std::vector<LaunchCandidate>& candidates);

    // Everything that can be answered without the model, in order: direct
    // action (URL/domain, explicit site search, folder, absolute path) ->
    // confident catalog app (exact/alias/lexical/acronym/prefix/typo) -> site
    // home or site search -> question -> bare folder -> strong keyword match.
    // Compound goals ("open spotify and discord") resolve when every part does.
    [[nodiscard]] static std::optional<SearchFastPlan> PlanWithoutModel(const std::wstring& request,
        const std::vector<LaunchCandidate>& candidates);

    // Primes llama-server's prompt cache with the shared static system prompt
    // so the first model request only evaluates its short user message.
    // Blocking; call from a worker thread. No-op once warm / while backing off.
    static void WarmPromptCache(const std::wstring& baseUrl);

    // Single-shot agent against llama-server: one grammar-constrained line
    // (L id / W query / Y query / U url / P folder / S words / N). S is answered
    // locally and only falls through to a second, launch-only round when no
    // app clearly wins. Blocking; call from a worker thread. status is invoked
    // from that worker thread; cancelled is polled between rounds.
    [[nodiscard]] static SearchAgentResult RunAgent(const std::wstring& baseUrl,
        const std::wstring& request, const std::vector<LaunchCandidate>& candidates,
        const StatusCallback& status, const CancelCallback& cancelled);

    // Defence-in-depth for ShellExecute targets already queued by the agent.
    [[nodiscard]] static bool IsSafeShellOpenTarget(bool isUrl, const std::wstring& target);

    // Process-local replay of the last successful goal (exact normalized match).
    // Avoids a model round when the user repeats the same goal.
    [[nodiscard]] static std::optional<SearchAgentResult> TryReplayLastGoal(
        const std::wstring& request);
    static void RememberSuccessfulGoal(const std::wstring& request,
        const std::vector<SearchAgentAction>& actions);
    static void RememberSuccessfulLaunch(const std::wstring& request, const std::string& appId,
        const std::wstring& label);
};
