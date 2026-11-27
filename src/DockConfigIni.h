#pragma once

#include "GroqProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace dockconfig {

inline constexpr int kMinRevealDurationMs = 0;
inline constexpr int kMaxRevealDurationMs = 2000;
inline constexpr int kDefaultRevealDurationMs = 50;
inline constexpr int kRevealSliderMaxMs = 1000;
inline constexpr int kRevealSliderNotchCount = 5;
inline constexpr int kRevealSliderStepMs = 200;

inline int ClampRevealDurationMs(int ms) noexcept {
    return std::clamp(ms, kMinRevealDurationMs, kMaxRevealDurationMs);
}

inline int SnapRevealSliderFractionToMs(float fraction) noexcept {
    const int step = static_cast<int>(
        std::lround(std::clamp(fraction, 0.0F, 1.0F) * static_cast<float>(kRevealSliderNotchCount)));
    return std::clamp(step, 0, kRevealSliderNotchCount) * kRevealSliderStepMs;
}

inline float RevealDurationMsToSliderFraction(int ms) noexcept {
    return std::clamp(static_cast<float>(ms) / static_cast<float>(kRevealSliderMaxMs), 0.0F, 1.0F);
}

using IniSections = std::map<std::wstring, std::map<std::wstring, std::wstring>>;

inline bool IsIniSpace(wchar_t c) noexcept {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\v' || c == L'\f' ||
        std::iswspace(static_cast<wint_t>(c)) != 0;
}

inline std::wstring TrimIniText(std::wstring_view value) {
    size_t first = 0;
    while (first < value.size() && IsIniSpace(value[first])) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && IsIniSpace(value[last - 1])) {
        --last;
    }
    return std::wstring(value.substr(first, last - first));
}

inline wchar_t LowerIniChar(wchar_t c) noexcept {
    if (c >= L'A' && c <= L'Z') {
        return static_cast<wchar_t>(c - L'A' + L'a');
    }
    return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
}

inline std::wstring ToLowerIni(std::wstring_view value) {
    std::wstring out;
    out.reserve(value.size());
    for (const wchar_t c : value) {
        out.push_back(LowerIniChar(c));
    }
    return out;
}

inline bool StartsWithInsensitiveIni(std::wstring_view value, std::wstring_view prefix) noexcept {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (LowerIniChar(value[i]) != LowerIniChar(prefix[i])) {
            return false;
        }
    }
    return true;
}

inline std::wstring SanitizeIniValue(std::wstring_view value) {
    std::wstring clean;
    clean.reserve(value.size());
    for (const wchar_t c : value) {
        if (c != L'\r' && c != L'\n' && c != L'\0') {
            clean.push_back(c);
        }
    }
    return clean;
}

// Decodes UTF-8 (with optional BOM) or UTF-16 LE/BE (with BOM) INI bytes.
// Malformed UTF-8 sequences become U+FFFD via groq::Utf8ToWide so a stray byte
// in a comment or pin path never discards the configuration and saved API keys.
inline bool DecodeIniBytes(std::string_view bytes, std::wstring& out) {
    out.clear();
    if (bytes.empty()) {
        return true;
    }

    if (bytes.size() >= 2U) {
        const auto b0 = static_cast<unsigned char>(bytes[0]);
        const auto b1 = static_cast<unsigned char>(bytes[1]);
        const bool utf16Le = b0 == 0xFFU && b1 == 0xFEU;
        const bool utf16Be = b0 == 0xFEU && b1 == 0xFFU;
        if (utf16Le || utf16Be) {
            if ((bytes.size() - 2U) % 2U != 0U) {
                return false;
            }
            out.reserve((bytes.size() - 2U) / 2U);
            auto readUnit = [&](size_t offset) noexcept -> uint16_t {
                const auto lo = static_cast<uint16_t>(static_cast<unsigned char>(bytes[offset]));
                const auto hi = static_cast<uint16_t>(static_cast<unsigned char>(bytes[offset + 1U]));
                return utf16Le ? static_cast<uint16_t>(lo | static_cast<uint16_t>(hi << 8U))
                               : static_cast<uint16_t>(hi | static_cast<uint16_t>(lo << 8U));
            };
            for (size_t offset = 2U; offset + 1U < bytes.size(); offset += 2U) {
                const uint16_t unit = readUnit(offset);
                if constexpr (sizeof(wchar_t) > 2) {
                    if (unit >= 0xD800U && unit <= 0xDBFFU && offset + 3U < bytes.size()) {
                        const uint16_t low = readUnit(offset + 2U);
                        if (low >= 0xDC00U && low <= 0xDFFFU) {
                            offset += 2U;
                            const uint32_t cp = 0x10000U +
                                ((static_cast<uint32_t>(unit - 0xD800U) << 10U) |
                                    static_cast<uint32_t>(low - 0xDC00U));
                            out.push_back(static_cast<wchar_t>(cp));
                            continue;
                        }
                    }
                }
                out.push_back(static_cast<wchar_t>(unit));
            }
            if (!out.empty() && out.front() == L'\uFEFF') {
                out.erase(out.begin());
            }
            return true;
        }
    }

    if (bytes.size() >= 3U &&
        static_cast<unsigned char>(bytes[0]) == 0xEFU &&
        static_cast<unsigned char>(bytes[1]) == 0xBBU &&
        static_cast<unsigned char>(bytes[2]) == 0xBFU) {
        bytes.remove_prefix(3U);
    }
    out = groq::Utf8ToWide(bytes);
    if (!out.empty() && out.front() == L'\uFEFF') {
        out.erase(out.begin());
    }
    return true;
}

inline IniSections ParseIniSections(std::wstring_view contents) {
    IniSections sections;
    std::wstring currentSection;
    size_t cursor = 0;
    bool firstLine = true;
    while (cursor <= contents.size()) {
        const size_t newline = contents.find(L'\n', cursor);
        std::wstring_view rawLine = newline == std::wstring_view::npos
            ? contents.substr(cursor)
            : contents.substr(cursor, newline - cursor);
        if (firstLine && !rawLine.empty() && rawLine.front() == L'\uFEFF') {
            rawLine.remove_prefix(1);
        }
        firstLine = false;

        const std::wstring line = TrimIniText(rawLine);
        if (!line.empty() && line.front() != L';' && line.front() != L'#') {
            if (line.front() == L'[' && line.back() == L']') {
                currentSection = ToLowerIni(
                    TrimIniText(std::wstring_view(line).substr(1, line.size() - 2)));
            } else if (!currentSection.empty()) {
                if (const size_t separator = line.find(L'='); separator != std::wstring::npos) {
                    const std::wstring key =
                        ToLowerIni(TrimIniText(std::wstring_view(line).substr(0, separator)));
                    const std::wstring value =
                        TrimIniText(std::wstring_view(line).substr(separator + 1));

                    // Recover files written by an older build that emitted a literal
                    // "\n" after TypeSafeApiKey=, gluing GroqApiKey= (or the next
                    // [Dock] entry) onto TypeSafeApiKey's value.
                    if (currentSection == L"dock" && key == L"typesafeapikey") {
                        if (const size_t escaped = value.find(L"\\n"); escaped != std::wstring::npos) {
                            const std::wstring head =
                                TrimIniText(std::wstring_view(value).substr(0, escaped));
                            const std::wstring tail =
                                TrimIniText(std::wstring_view(value).substr(escaped + 2));
                            if (StartsWithInsensitiveIni(head, L"dpapi:") || tail.empty() ||
                                StartsWithInsensitiveIni(tail, L"groqapikey=") ||
                                StartsWithInsensitiveIni(tail, L"aimodel=") ||
                                StartsWithInsensitiveIni(tail, L"searchhotkey=")) {
                                sections[currentSection][key] = head;
                                if (const size_t eq = tail.find(L'='); eq != std::wstring::npos) {
                                    const std::wstring tailKey = ToLowerIni(
                                        TrimIniText(std::wstring_view(tail).substr(0, eq)));
                                    if (!tailKey.empty()) {
                                        sections[currentSection].try_emplace(
                                            tailKey, TrimIniText(std::wstring_view(tail).substr(eq + 1)));
                                    }
                                }
                                cursor = newline == std::wstring_view::npos ? contents.size() + 1U : newline + 1U;
                                continue;
                            }
                        }
                    }

                    if (!key.empty()) {
                        sections[currentSection][key] = value;
                    }
                }
            }
        }

        if (newline == std::wstring_view::npos) {
            break;
        }
        cursor = newline + 1U;
    }
    return sections;
}

inline void AppendSecretAndAiLines(std::wostream& contents,
    std::wstring_view persistedTypeSafeApiKey, std::wstring_view persistedGroqApiKey,
    std::wstring_view aiModel, bool searchHotkey) {
    if (!persistedTypeSafeApiKey.empty()) {
        contents << L"TypeSafeApiKey=" << SanitizeIniValue(persistedTypeSafeApiKey) << L"\n";
    }
    if (!persistedGroqApiKey.empty()) {
        contents << L"GroqApiKey=" << SanitizeIniValue(persistedGroqApiKey) << L"\n";
    }
    if (!aiModel.empty()) {
        contents << L"AiModel=" << SanitizeIniValue(aiModel) << L"\n";
    }
    if (!searchHotkey) {
        contents << L"SearchHotkey=0\n";
    }
}

// Parses a manual reveal/hide duration in [0ms, 2000ms]: "instant", "250ms", "0.4s",
// decimal seconds ("0.35"), or integer ms ("50"). Rejects negative or > 2s inputs.
inline std::optional<int> ParseRevealDurationMs(std::wstring_view raw) {
    const std::wstring lower = ToLowerIni(TrimIniText(raw));
    if (lower.empty()) {
        return std::nullopt;
    }
    if (lower == L"instant") {
        return 0;
    }

    std::wstring_view body(lower);
    bool explicitMs = false;
    bool explicitSec = false;
    if (body.ends_with(L"ms")) {
        explicitMs = true;
        body.remove_suffix(2U);
    } else if (body.ends_with(L's')) {
        explicitSec = true;
        body.remove_suffix(1U);
    }

    const std::wstring num = TrimIniText(body);
    bool sawDigit = false;
    bool sawDot = false;
    for (const wchar_t ch : num) {
        if (ch >= L'0' && ch <= L'9') {
            sawDigit = true;
        } else if (ch == L'.' && !sawDot) {
            sawDot = true;
        } else {
            return std::nullopt;
        }
    }
    if (!sawDigit) {
        return std::nullopt;
    }

    try {
        size_t consumed = 0;
        const double value = std::stod(num, &consumed);
        if (consumed != num.size() || !std::isfinite(value) || value < 0.0) {
            return std::nullopt;
        }
        const double msDouble = (explicitSec || (!explicitMs && sawDot)) ? value * 1000.0 : value;
        if (msDouble > static_cast<double>(kMaxRevealDurationMs) + 0.5) {
            return std::nullopt;
        }
        const int ms = static_cast<int>(std::lround(msDouble));
        return ms <= kMaxRevealDurationMs ? std::optional<int>(ms) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

inline std::wstring FormatRevealDurationLabel(int ms) {
    const int c = ClampRevealDurationMs(ms);
    if (c == 0) {
        return L"Instant";
    }
    if (c >= 1000 && c % 10 == 0) {
        const int frac = (c % 1000) / (c % 100 == 0 ? 100 : 10);
        const wchar_t* pad = (c % 100 != 0 && frac < 10) ? L"0" : L"";
        return std::to_wstring(c / 1000) + L"." + pad + std::to_wstring(frac) + L" s";
    }
    return std::to_wstring(c) + L" ms";
}

}  // namespace dockconfig
