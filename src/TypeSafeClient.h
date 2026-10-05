#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct LaunchCandidate {
    std::string id;
    std::wstring name;
    std::wstring pinName;
    std::wstring shortcutName;
    std::wstring executable;
    std::wstring executablePath;
    std::wstring target;
    std::wstring aumid;
    std::wstring description;
    std::wstring productName;
    std::wstring publisher;
    std::wstring startMenuFolder;
    std::wstring comment;
    std::wstring windowTitle;
    bool running = false;
};

struct LaunchJudgment {
    enum class Action {
        Launch,
        None,
        Uncertain,
        Error,
    };

    Action action = Action::Error;
    std::string chosenId;
    // Optional filesystem path from local AI (open via ShellExecute).
    std::wstring openPath;
    double exists = 0.0;
    double confidence = 0.0;
    std::wstring error;
    // Set when ResolveApp fell back to lexical fuzzy ranking.
    bool usedFuzzyFallback = false;
};

struct BoostCandidate {
    std::string id;
    std::wstring name;
    std::wstring exePath;
    std::wstring windowTitle;
    bool hasWindow = false;
    double memoryMb = 0.0;
    double cpuPercent = 0.0;
    uint64_t ageSeconds = 0;
};

struct BoostJudgment {
    std::string id;
    double safe = 0.0;
    double idle = 0.0;
};

struct BoostResult {
    bool ok = false;
    std::wstring error;
    std::vector<BoostJudgment> items;
};

class TypeSafeClient {
public:
    [[nodiscard]] static LaunchJudgment ResolveApp(const std::wstring& apiKey,
        const std::wstring& request, const std::vector<LaunchCandidate>& candidates);
    [[nodiscard]] static BoostResult ClassifyForBoost(const std::wstring& apiKey,
        const std::vector<BoostCandidate>& candidates);
    // Same Jev/Noul path as ClassifyForBoost, but questions ask whether each
    // Windows startup entry is safe to disable for a performance boost.
    // Reuses BoostCandidate/BoostJudgment: safe=disable-safe, idle=nonessential.
    [[nodiscard]] static BoostResult ClassifyForStartupBoost(const std::wstring& apiKey,
        const std::vector<BoostCandidate>& candidates);
};
