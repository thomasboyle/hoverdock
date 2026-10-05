#pragma once

#include "TypeSafeClient.h"

#include <string>
#include <vector>

// Local OpenAI-compatible client for llama-server (llama.cpp).
// Used by dock Search / launch prompt. Falls back to classic fuzzy ranking
// when the server is unreachable so Search still works offline.
class LlamaServerClient {
public:
    // baseUrl e.g. http://127.0.0.1:8080 (no trailing slash). Empty uses default.
    [[nodiscard]] static LaunchJudgment ResolveApp(const std::wstring& baseUrl,
        const std::wstring& request, const std::vector<LaunchCandidate>& candidates);

    [[nodiscard]] static bool IsServerReachable(const std::wstring& baseUrl);

    // Lexical / substring ranking over installed apps (no network).
    [[nodiscard]] static LaunchJudgment ResolveAppFuzzy(const std::wstring& request,
        const std::vector<LaunchCandidate>& candidates);
};
