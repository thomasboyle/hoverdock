#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
    [[nodiscard]] static BoostResult ClassifyForBoost(const std::wstring& apiKey,
        const std::vector<BoostCandidate>& candidates);
    // Same Jev/Noul path as ClassifyForBoost, but questions ask whether each
    // Windows startup entry is safe to disable for a performance boost.
    // Reuses BoostCandidate/BoostJudgment: safe=disable-safe, idle=nonessential.
    [[nodiscard]] static BoostResult ClassifyForStartupBoost(const std::wstring& apiKey,
        const std::vector<BoostCandidate>& candidates);
};
