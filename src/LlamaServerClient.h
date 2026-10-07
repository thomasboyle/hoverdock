#pragma once

#include "TypeSafeClient.h"

#include <cstdint>
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
        // write_file / create_file: target = planned path inside the HoverDock
        // code sandbox (Documents\HoverDock), content = UTF-8 text. Never
        // overwrites: DockApp writes with CREATE_NEW and picks name-2.ext etc.
        WriteFile,
        // target = code file just written; editor = validated IDE executable
        // (empty = Notepad); editorWindow = running IDE top-level window to
        // focus first (0 = none). The file is passed to the editor explicitly,
        // never ShellExecute'd with its default (possibly script-host) verb.
        OpenInEditor,
    };

    Kind kind = Kind::LaunchApp;
    std::string appId;
    std::wstring target;
    std::wstring label;
    std::string content;
    std::wstring editor;
    std::wstring editorArgs;  // e.g. "/Edit" for Visual Studio
    std::uintptr_t editorWindow = 0;
};

struct SearchAgentResult {
    // llama-server could not be reached before the first step: caller falls
    // back to classic fuzzy search and must not show agent replies.
    bool serverUnavailable = false;
    std::vector<SearchAgentAction> actions;
    // Short reply from the model ("done.say") or a status/error line.
    std::wstring reply;
    int steps = 0;
    // From llama-server timings.predicted_per_second (0 = unknown / not a model round).
    double predictedPerSecond = 0.0;
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

    // True for factual / specs / definition questions that should be answered
    // as text above Search ("Apple Watch Ultra 5 specs", "what is HTTP").
    // Explicit open/launch/start/run/play goals stay false so tools still run.
    [[nodiscard]] static bool LooksLikeInfoQuery(const std::wstring& request);

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
    // Coding goals are NOT planned here (no Google + editor): see
    // LooksLikeCodingGoal / RunCodeAgent.
    // Compound goals ("open spotify and discord") resolve when every part does.
    [[nodiscard]] static std::optional<SearchFastPlan> PlanWithoutModel(const std::wstring& request,
        const std::vector<LaunchCandidate>& candidates);

    // Primes llama-server's prompt cache with the shared static system prompt
    // so the first model request only evaluates its short user message.
    // Blocking; call from a worker thread. No-op once warm / while backing off.
    static void WarmPromptCache(const std::wstring& baseUrl);

    // Single-shot agent against llama-server: one grammar-constrained line
    // (L id / A text / W query / Y query / U url / P folder / S words / C task / N).
    // S is answered locally and only falls through to a second, launch-only round
    // when no app clearly wins; C ("write code") hands over to the code round
    // (RunCodeAgent); A shows a short answer above Search (no ShellExecute).
    // Blocking; call from a worker thread. status is invoked from that worker
    // thread; cancelled is polled between rounds.
    [[nodiscard]] static SearchAgentResult RunAgent(const std::wstring& baseUrl,
        const std::wstring& request, const std::vector<LaunchCandidate>& candidates,
        const StatusCallback& status, const CancelCallback& cancelled,
        const std::wstring& codeLanguageHint = {});

    // Factual / specs Q&A: WinHTTP fetches DuckDuckGo/Bing snippets, then the
    // local model answers with grammar A <answer> grounded in that context
    // (search summary alone if llama-server is down). No L/W/Y/... tools.
    // result.actions stays empty; result.reply is shown above the Search box;
    // result.predictedPerSecond comes from llama timings when available.
    [[nodiscard]] static SearchAgentResult RunAnswerAgent(const std::wstring& baseUrl,
        const std::wstring& request, const StatusCallback& status,
        const CancelCallback& cancelled);

    // Defence-in-depth for ShellExecute targets already queued by the agent.
    [[nodiscard]] static bool IsSafeShellOpenTarget(bool isUrl, const std::wstring& target);

    // ---- Coding goals (write_file + open in editor) -----------------------

    // "write hello world program", "code a snake game in python", "create a
    // python script that renames files", "hello world in rust".
    [[nodiscard]] static bool LooksLikeCodingGoal(const std::wstring& request);

    // Model writes the program (grammar: "F <name.ext>" line + source), the
    // client validates the file name/extension and queues WriteFile +
    // OpenInEditor (editor fields are filled by the caller). languageHint is
    // the default language when the goal names none (from the chosen IDE).
    // Blocking; worker thread. serverUnavailable when llama-server is down.
    [[nodiscard]] static SearchAgentResult RunCodeAgent(const std::wstring& baseUrl,
        const std::wstring& request, const std::wstring& languageHint,
        const StatusCallback& status, const CancelCallback& cancelled);

    // Documents\HoverDock (created on demand). Empty when unavailable.
    [[nodiscard]] static std::wstring CodeSandboxFolder(bool create);

    // True for an absolute path to a regular file name with an allowed text /
    // source extension (.txt .md .py .cpp .c .h .hpp .js .ts .html .css .json
    // .java .cs .rs .go .kt .sql .lua .rb) directly inside the HoverDock
    // sandbox or the user's Documents / Desktop / Downloads folder. Executables,
    // scripts run by the shell (.bat .ps1 .cmd .vbs .lnk ...), UNC/device paths,
    // reserved names and alternate data streams are refused.
    [[nodiscard]] static bool IsSafeWriteTarget(const std::wstring& path, std::wstring& why);

    // Validates and writes action.content (CREATE_NEW, never overwrites:
    // falls back to name-2.ext ... name-50.ext). writtenPath is the final file.
    [[nodiscard]] static bool WriteAgentFile(const SearchAgentAction& action, std::wstring& writtenPath,
        std::wstring& why);

    // OpenInEditor target check: existing file passing IsSafeWriteTarget.
    [[nodiscard]] static bool IsSafeEditorFile(const std::wstring& path);

    // Process-local replay of the last successful goal (exact normalized match).
    // Avoids a model round when the user repeats the same goal.
    [[nodiscard]] static std::optional<SearchAgentResult> TryReplayLastGoal(
        const std::wstring& request);
    static void RememberSuccessfulGoal(const std::wstring& request,
        const std::vector<SearchAgentAction>& actions);
    static void RememberSuccessfulLaunch(const std::wstring& request, const std::string& appId,
        const std::wstring& label);
};
