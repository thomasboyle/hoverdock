#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace tray_policy {

[[nodiscard]] inline std::wstring LowerCopy(std::wstring_view value) {
    std::wstring out(value);
    for (wchar_t& character : out) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return out;
}

[[nodiscard]] inline std::wstring TrimEdges(std::wstring_view value) {
    size_t begin = 0;
    while (begin < value.size() && std::iswspace(value[begin]) != 0) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::iswspace(value[end - 1]) != 0) {
        --end;
    }
    return std::wstring(value.substr(begin, end - begin));
}

[[nodiscard]] inline std::wstring FileStem(std::wstring_view path) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring_view file = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
    const size_t dot = file.find_last_of(L'.');
    return std::wstring(dot == std::wstring_view::npos ? file : file.substr(0, dot));
}

[[nodiscard]] inline std::wstring CompactLower(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t character : text) {
        if (std::iswalnum(character) != 0) {
            out.push_back(static_cast<wchar_t>(std::towlower(character)));
        }
    }
    return out;
}

inline constexpr std::wstring_view kHelperSuffixes[] = {
    L"webhelper", L"systray", L"helper", L"launcher", L"updater", L"service", L"tray"};

[[nodiscard]] inline std::wstring StripHelperSuffix(std::wstring_view stem, size_t minRemaining) {
    const std::wstring lowered = LowerCopy(stem);
    for (const std::wstring_view suffix : kHelperSuffixes) {
        if (lowered.size() >= suffix.size() + minRemaining &&
            lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return std::wstring(stem.substr(0, stem.size() - suffix.size()));
        }
    }
    return std::wstring(stem);
}

[[nodiscard]] inline bool HasHelperSuffix(std::wstring_view stem) {
    return StripHelperSuffix(stem, 1).size() < stem.size();
}

// Display name for a tray host executable: strip helper suffixes (steamwebhelper
// identifies as Steam, not as a second app) and capitalize the stem.
[[nodiscard]] inline std::wstring PrettyTrayName(std::wstring_view exePath) {
    std::wstring stem = StripHelperSuffix(FileStem(exePath), 1);
    if (!stem.empty()) {
        stem[0] = static_cast<wchar_t>(std::towupper(stem[0]));
    }
    return stem;
}

// Canonical lowercase app key used to merge helper processes with their parent
// executable (e.g. steam.exe and steamwebhelper.exe both map to "steam").
// Keeps at least 3 chars when stripping so short stems (e.g. "nvtray") stay distinct.
[[nodiscard]] inline std::wstring CanonicalAppKey(std::wstring_view exePath) {
    return LowerCopy(StripHelperSuffix(FileStem(exePath), 3));
}

[[nodiscard]] inline bool IsPathUnderParentDir(std::wstring_view childPathLower,
    std::wstring_view parentExeLower) {
    const size_t sep = parentExeLower.find_last_of(L"\\/");
    if (sep == std::wstring_view::npos || sep == 0) {
        return false;
    }
    const std::wstring_view dir = parentExeLower.substr(0, sep);
    return childPathLower.size() > dir.size() &&
        childPathLower.compare(0, dir.size(), dir) == 0 &&
        (childPathLower[dir.size()] == L'\\' || childPathLower[dir.size()] == L'/');
}

// True when candidateExePath is the same executable as targetExePath or a
// helper process under the same app directory tree (e.g. steamwebhelper.exe for steam.exe).
[[nodiscard]] inline bool MatchesTrayAppExe(std::wstring_view candidateExePath,
    std::wstring_view targetExePath) {
    if (candidateExePath.empty() || targetExePath.empty()) {
        return false;
    }
    const std::wstring candLower = LowerCopy(candidateExePath);
    const std::wstring targetLower = LowerCopy(targetExePath);
    if (candLower == targetLower) {
        return true;
    }
    const std::wstring candKey = CanonicalAppKey(candidateExePath);
    return candKey.size() >= 3 && candKey == CanonicalAppKey(targetExePath) &&
        (IsPathUnderParentDir(candLower, targetLower) ||
            IsPathUnderParentDir(targetLower, candLower));
}

// Extracts the app title segment from a tray tooltip: first line, before " - "
// or " (" status suffixes, trimmed and lowercased.
[[nodiscard]] inline std::wstring FirstTitleSegmentLower(std::wstring_view text) {
    std::wstring trimmed = TrimEdges(text);
    for (const std::wstring_view sep : {std::wstring_view(L"\r"), std::wstring_view(L"\n"),
             std::wstring_view(L" - "), std::wstring_view(L" (")}) {
        const size_t pos = trimmed.find(sep);
        if (pos != std::wstring::npos && pos > 0) {
            trimmed.resize(pos);
        }
    }
    return LowerCopy(TrimEdges(trimmed));
}

// Case-insensitive score for matching a tray query against a Windows UIA
// NotifyItemIcon button name. Scores >= 12 are strong matches.
[[nodiscard]] inline int ShellButtonMatchScore(std::wstring_view query,
    std::wstring_view buttonName) {
    const std::wstring q = LowerCopy(TrimEdges(query));
    const std::wstring b = LowerCopy(TrimEdges(buttonName));
    if (q.empty() || b.empty()) {
        return 0;
    }
    if (q == b) {
        return 100000;
    }

    const std::wstring qTitle = FirstTitleSegmentLower(q);
    const std::wstring bTitle = FirstTitleSegmentLower(b);
    if (!qTitle.empty() && qTitle == bTitle) {
        return 90000;
    }

    const std::wstring qCompact = CompactLower(qTitle);
    if (qCompact.size() >= 4 && qCompact == CompactLower(bTitle)) {
        return 80000;
    }

    // Whole-word prefix match (e.g. "steam" matches "steam client", not "steamvr").
    auto wholeWordPrefix = [](std::wstring_view shorter, std::wstring_view longer) -> bool {
        return shorter.size() >= 3 && longer.size() > shorter.size() &&
            longer.compare(0, shorter.size(), shorter) == 0 &&
            std::iswalnum(longer[shorter.size()]) == 0;
    };
    if (wholeWordPrefix(qTitle, bTitle)) {
        return 50000 + static_cast<int>(qTitle.size());
    }
    if (wholeWordPrefix(bTitle, qTitle)) {
        return 50000 + static_cast<int>(bTitle.size());
    }

    size_t prefixLen = 0;
    const size_t limit = std::min(q.size(), b.size());
    while (prefixLen < limit && q[prefixLen] == b[prefixLen]) {
        ++prefixLen;
    }
    return static_cast<int>(prefixLen);
}

[[nodiscard]] inline bool IsStrongShellButtonMatch(int score) noexcept {
    return score >= 12;
}

struct TrayIdentity {
    std::wstring executablePath;
    std::wstring exeName;
    std::wstring tip;
    std::wstring shellName;
};

[[nodiscard]] inline bool AreSameTrayApp(const TrayIdentity& left, const TrayIdentity& right) {
    if (!left.shellName.empty() && !right.shellName.empty() &&
        LowerCopy(TrimEdges(left.shellName)) == LowerCopy(TrimEdges(right.shellName))) {
        return true;
    }
    const std::wstring leftKey = CanonicalAppKey(
        left.executablePath.empty() ? left.exeName : left.executablePath);
    const std::wstring rightKey = CanonicalAppKey(
        right.executablePath.empty() ? right.exeName : right.executablePath);
    if (!leftKey.empty() && !rightKey.empty()) {
        return leftKey == rightKey;
    }
    if (leftKey.empty() && rightKey.empty()) {
        const std::wstring leftTitle = FirstTitleSegmentLower(left.tip);
        return !leftTitle.empty() && leftTitle == FirstTitleSegmentLower(right.tip);
    }
    return false;
}

// Prefer a primary executable (e.g. steam.exe) over a helper (steamwebhelper.exe).
[[nodiscard]] inline bool PreferExecutablePath(std::wstring_view candidate,
    std::wstring_view existing) {
    if (candidate.empty()) {
        return false;
    }
    return existing.empty() ||
        (!HasHelperSuffix(FileStem(candidate)) && HasHelperSuffix(FileStem(existing)));
}

[[nodiscard]] inline int BestShellButtonMatchScore(const TrayIdentity& icon,
    std::wstring_view buttonName) {
    return std::max({
        ShellButtonMatchScore(icon.shellName, buttonName),
        ShellButtonMatchScore(icon.tip, buttonName),
        ShellButtonMatchScore(PrettyTrayName(icon.executablePath), buttonName),
        ShellButtonMatchScore(icon.exeName, buttonName),
    });
}

}  // namespace tray_policy
