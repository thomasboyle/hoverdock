// DockConfig persistence unit tests (portable INI tests + Windows DPAPI round-trip).

#include "DockConfig.h"
#include "DockConfigIni.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                      \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                                      \
        }                                                                                      \
    } while (false)

std::string BuildIniBytes(std::wstring_view lastInstalledVersion,
    std::wstring_view typeSafeKey, std::wstring_view groqKey, std::wstring_view aiModel) {
    std::wostringstream out;
    out << L"[Dock]\nFollowTaskbarPins=0\n";
    if (!lastInstalledVersion.empty()) {
        out << L"LastInstalledVersion=" << dockconfig::SanitizeIniValue(lastInstalledVersion) << L"\n";
    }
    dockconfig::AppendSecretAndAiLines(out, typeSafeKey, groqKey, aiModel, true);
    return groq::WideToUtf8(out.str());
}

void TestTypeSafeAndGroqKeysPersistAcrossUpdateCycle() {
    const std::string savedPreUpdate = BuildIniBytes(
        L"1.2.0", L"dpapi:AQAAANCMnd8TypeSafeBlob==", L"dpapi:AQAAANCMnd8GroqKeyBlob==",
        L"llama-3.3-70b-versatile");
    EXPECT(!savedPreUpdate.empty());
    EXPECT(savedPreUpdate.find("\\nGroqApiKey=") == std::string::npos);

    std::wstring decoded;
    EXPECT(dockconfig::DecodeIniBytes(savedPreUpdate, decoded));
    const dockconfig::IniSections loadedAfterRestart = dockconfig::ParseIniSections(decoded);
    const auto dockIt = loadedAfterRestart.find(L"dock");
    EXPECT(dockIt != loadedAfterRestart.end());
    if (dockIt == loadedAfterRestart.end()) {
        return;
    }
    EXPECT(dockIt->second.at(L"typesafeapikey") == L"dpapi:AQAAANCMnd8TypeSafeBlob==");
    EXPECT(dockIt->second.at(L"groqapikey") == L"dpapi:AQAAANCMnd8GroqKeyBlob==");
    EXPECT(dockIt->second.at(L"aimodel") == L"llama-3.3-70b-versatile");
    EXPECT(dockIt->second.at(L"lastinstalledversion") == L"1.2.0");

    // Simulate ReconcileLastUpdate clearing LastInstalledVersion and saving again.
    const std::string savedPostReconcile = BuildIniBytes(
        L"", dockIt->second.at(L"typesafeapikey"), dockIt->second.at(L"groqapikey"),
        dockIt->second.at(L"aimodel"));
    std::wstring decodedPost;
    EXPECT(dockconfig::DecodeIniBytes(savedPostReconcile, decodedPost));
    const dockconfig::IniSections finalSections = dockconfig::ParseIniSections(decodedPost);
    const auto finalDock = finalSections.find(L"dock");
    EXPECT(finalDock != finalSections.end());
    if (finalDock != finalSections.end()) {
        EXPECT(finalDock->second.at(L"typesafeapikey") == L"dpapi:AQAAANCMnd8TypeSafeBlob==");
        EXPECT(finalDock->second.at(L"groqapikey") == L"dpapi:AQAAANCMnd8GroqKeyBlob==");
        EXPECT(!finalDock->second.contains(L"lastinstalledversion"));
    }
}

void TestRecoversLegacyEscapedNewlineCorruption() {
    // Older builds wrote L"\\n" after TypeSafeApiKey=, gluing GroqApiKey= onto the same line.
    const std::wstring legacyGluedGroq =
        L"[Dock]\n"
        L"FollowTaskbarPins=0\n"
        L"TypeSafeApiKey=dpapi:AAA111==\\nGroqApiKey=dpapi:BBB222==\n"
        L"AiModel=openai/gpt-oss-120b\n";
    const dockconfig::IniSections sections = dockconfig::ParseIniSections(legacyGluedGroq);
    const auto dockIt = sections.find(L"dock");
    EXPECT(dockIt != sections.end());
    if (dockIt != sections.end()) {
        EXPECT(dockIt->second.contains(L"typesafeapikey"));
        EXPECT(dockIt->second.contains(L"groqapikey"));
        if (dockIt->second.contains(L"typesafeapikey") && dockIt->second.contains(L"groqapikey")) {
            EXPECT(dockIt->second.at(L"typesafeapikey") == L"dpapi:AAA111==");
            EXPECT(dockIt->second.at(L"groqapikey") == L"dpapi:BBB222==");
        }
        EXPECT(dockIt->second.at(L"aimodel") == L"openai/gpt-oss-120b");
    }

    // Also cover when GroqApiKey was absent and AiModel or trailing "\n" followed TypeSafeApiKey.
    const std::wstring legacyGluedModel =
        L"[Dock]\n"
        L"TypeSafeApiKey=dpapi:AAA111==\\nAiModel=llama-3.3-70b-versatile\n";
    const dockconfig::IniSections modelSections = dockconfig::ParseIniSections(legacyGluedModel);
    EXPECT(modelSections.at(L"dock").at(L"typesafeapikey") == L"dpapi:AAA111==");
    EXPECT(modelSections.at(L"dock").at(L"aimodel") == L"llama-3.3-70b-versatile");

    const std::wstring legacyTrailingSlashN =
        L"[Dock]\n"
        L"TypeSafeApiKey=dpapi:AAA111==\\n\n";
    const dockconfig::IniSections trailingSections =
        dockconfig::ParseIniSections(legacyTrailingSlashN);
    EXPECT(trailingSections.at(L"dock").at(L"typesafeapikey") == L"dpapi:AAA111==");
}

void TestBomAndInvalidUtf8DoNotDropGroqApiKey() {
    // UTF-8 BOM right before [Dock] (e.g. hand-edited in Windows Notepad).
    const std::string utf8WithBom =
        "\xEF\xBB\xBF[Dock]\r\nGroqApiKey=dpapi:SurvivedBom==\r\n";
    std::wstring wideFromBom;
    EXPECT(dockconfig::DecodeIniBytes(utf8WithBom, wideFromBom));
    const dockconfig::IniSections bomSections = dockconfig::ParseIniSections(wideFromBom);
    EXPECT(bomSections.contains(L"dock") &&
        bomSections.at(L"dock").at(L"groqapikey") == L"dpapi:SurvivedBom==");

    // UTF-16 LE BOM.
    const std::u16string utf16Text = u"[Dock]\r\nGroqApiKey=dpapi:SurvivedUtf16==\r\n";
    std::string utf16LeBytes = "\xFF\xFE";
    for (const char16_t ch : utf16Text) {
        utf16LeBytes.push_back(static_cast<char>(ch & 0xFFU));
        utf16LeBytes.push_back(static_cast<char>((ch >> 8U) & 0xFFU));
    }
    std::wstring wideFromUtf16;
    EXPECT(dockconfig::DecodeIniBytes(utf16LeBytes, wideFromUtf16));
    const dockconfig::IniSections utf16Sections = dockconfig::ParseIniSections(wideFromUtf16);
    EXPECT(utf16Sections.contains(L"dock") &&
        utf16Sections.at(L"dock").at(L"groqapikey") == L"dpapi:SurvivedUtf16==");

    // Stray invalid UTF-8 byte in a comment or pin must not discard GroqApiKey.
    const std::string invalidUtf8 =
        "; comment with \xFF byte\n[Dock]\nGroqApiKey=dpapi:SurvivedBadByte==\n";
    std::wstring wideFromInvalid;
    EXPECT(dockconfig::DecodeIniBytes(invalidUtf8, wideFromInvalid));
    const dockconfig::IniSections invalidSections = dockconfig::ParseIniSections(wideFromInvalid);
    EXPECT(invalidSections.contains(L"dock") &&
        invalidSections.at(L"dock").at(L"groqapikey") == L"dpapi:SurvivedBadByte==");
}

void TestRevealDurationSliderAndOverrideParsing() {
    // 5 notch intervals from 0s (instant) to 1s in 0.2s (200ms) steps.
    EXPECT(dockconfig::SnapRevealSliderFractionToMs(-0.2F) == 0);
    EXPECT(dockconfig::SnapRevealSliderFractionToMs(0.0F) == 0);
    EXPECT(dockconfig::SnapRevealSliderFractionToMs(0.09F) == 0);
    EXPECT(dockconfig::SnapRevealSliderFractionToMs(0.11F) == 200);
    EXPECT(dockconfig::SnapRevealSliderFractionToMs(0.2F) == 200);
    EXPECT(dockconfig::SnapRevealSliderFractionToMs(0.4F) == 400);
    EXPECT(dockconfig::SnapRevealSliderFractionToMs(0.6F) == 600);
    EXPECT(dockconfig::SnapRevealSliderFractionToMs(0.8F) == 800);
    EXPECT(dockconfig::SnapRevealSliderFractionToMs(1.0F) == 1000);
    EXPECT(dockconfig::SnapRevealSliderFractionToMs(1.4F) == 1000);

    // Manual override values > 1s clamp the slider thumb to 1.0 while keeping the ms value.
    EXPECT(dockconfig::RevealDurationMsToSliderFraction(0) == 0.0F);
    EXPECT(std::abs(dockconfig::RevealDurationMsToSliderFraction(50) - 0.05F) < 0.0001F);
    EXPECT(std::abs(dockconfig::RevealDurationMsToSliderFraction(600) - 0.6F) < 0.0001F);
    EXPECT(dockconfig::RevealDurationMsToSliderFraction(1000) == 1.0F);
    EXPECT(dockconfig::RevealDurationMsToSliderFraction(1500) == 1.0F);
    EXPECT(dockconfig::RevealDurationMsToSliderFraction(2000) == 1.0F);

    // Manual override parsing: 0ms to 2s in ms, s, decimal s, or "instant".
    EXPECT(dockconfig::ParseRevealDurationMs(L"0") == 0);
    EXPECT(dockconfig::ParseRevealDurationMs(L"0ms") == 0);
    EXPECT(dockconfig::ParseRevealDurationMs(L"0 ms") == 0);
    EXPECT(dockconfig::ParseRevealDurationMs(L"0s") == 0);
    EXPECT(dockconfig::ParseRevealDurationMs(L"instant") == 0);
    EXPECT(dockconfig::ParseRevealDurationMs(L"  INSTANT ") == 0);
    EXPECT(dockconfig::ParseRevealDurationMs(L"50") == 50);
    EXPECT(dockconfig::ParseRevealDurationMs(L"50ms") == 50);
    EXPECT(dockconfig::ParseRevealDurationMs(L"250 ms") == 250);
    EXPECT(dockconfig::ParseRevealDurationMs(L"0.2") == 200);
    EXPECT(dockconfig::ParseRevealDurationMs(L"0.2s") == 200);
    EXPECT(dockconfig::ParseRevealDurationMs(L"0.35 s") == 350);
    EXPECT(dockconfig::ParseRevealDurationMs(L"1s") == 1000);
    EXPECT(dockconfig::ParseRevealDurationMs(L"1.5") == 1500);
    EXPECT(dockconfig::ParseRevealDurationMs(L"1.5s") == 1500);
    EXPECT(dockconfig::ParseRevealDurationMs(L"2000") == 2000);
    EXPECT(dockconfig::ParseRevealDurationMs(L"2000ms") == 2000);
    EXPECT(dockconfig::ParseRevealDurationMs(L"2s") == 2000);
    EXPECT(dockconfig::ParseRevealDurationMs(L"2.0") == 2000);

    // Invalid / out-of-range manual override inputs are rejected.
    EXPECT(!dockconfig::ParseRevealDurationMs(L"").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"   ").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"-10").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"-0.5s").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"2001ms").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"2.1s").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"3s").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"abc").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"100x").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"1.2.3s").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L".").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"ms").has_value());
    EXPECT(!dockconfig::ParseRevealDurationMs(L"s").has_value());

    // Formatting helpers.
    EXPECT(dockconfig::FormatRevealDurationLabel(0) == L"Instant");
    EXPECT(dockconfig::FormatRevealDurationLabel(50) == L"50 ms");
    EXPECT(dockconfig::FormatRevealDurationLabel(200) == L"200 ms");
    EXPECT(dockconfig::FormatRevealDurationLabel(1000) == L"1.0 s");
    EXPECT(dockconfig::FormatRevealDurationLabel(1250) == L"1.25 s");
    EXPECT(dockconfig::FormatRevealDurationLabel(1255) == L"1255 ms");
    EXPECT(dockconfig::FormatRevealDurationLabel(2000) == L"2.0 s");
}

#ifdef _WIN32
std::wstring EnvSnapshot(const wchar_t* name, bool& defined) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    defined = needed > 0;
    if (needed <= 1) {
        return {};
    }
    std::wstring val(needed, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, val.data(), needed);
    val.resize(copied);
    return val;
}

void RestoreEnv(const wchar_t* name, bool defined, const std::wstring& value) {
    SetEnvironmentVariableW(name, defined ? value.c_str() : nullptr);
}

void TestWindowsDockConfigDpapiAndUpdatePersistence() {
    bool hadLocalAppData = false;
    const std::wstring origLocalAppData = EnvSnapshot(L"LOCALAPPDATA", hadLocalAppData);
    bool hadGroqEnv = false;
    const std::wstring origGroqEnv = EnvSnapshot(L"GROQ_API_KEY", hadGroqEnv);
    bool hadTypeSafeEnv = false;
    const std::wstring origTypeSafeEnv = EnvSnapshot(L"TYPESAFE_API_KEY", hadTypeSafeEnv);

    wchar_t tempBase[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempBase);
    const std::wstring testRoot = std::wstring(tempBase) + L"HoverdockConfigTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(testRoot.c_str(), nullptr);

    SetEnvironmentVariableW(L"LOCALAPPDATA", testRoot.c_str());
    // Set to empty in process env so any developer machine HKCU\Environment key is masked.
    SetEnvironmentVariableW(L"GROQ_API_KEY", L"");
    SetEnvironmentVariableW(L"TYPESAFE_API_KEY", L"");

    {
        DockConfig cfg;
        EXPECT(cfg.LoadOrCreate());
        EXPECT(!cfg.MacosLiquidGlass());
        EXPECT(!cfg.MacosControlCentreGlass());
        EXPECT(cfg.RevealDurationMs() == 50);
        cfg.StopFollowingTaskbarPins();
        cfg.SetMacosLiquidGlass(true);
        cfg.SetMacosControlCentreGlass(true);
        cfg.SetRevealDurationMs(1750);
        cfg.SetGroqApiKey(L"gsk_unit_test_secret_key_987654");
        EXPECT(cfg.Save());

        // Simulate auto-update pre-exit save (ApplyUpdateResult).
        cfg.SetLastInstalledVersion(L"9.9.9", 1763156500LL);
        EXPECT(cfg.Save());
    }

    {
        // Simulate updated Dock.exe startup + ReconcileLastUpdate.
        DockConfig restarted;
        EXPECT(restarted.LoadOrCreate());
        EXPECT(restarted.MacosLiquidGlass());
        EXPECT(restarted.MacosControlCentreGlass());
        EXPECT(restarted.RevealDurationMs() == 1750);
        EXPECT(restarted.GroqApiKey() == L"gsk_unit_test_secret_key_987654");
        EXPECT(restarted.LastInstalledVersion() == L"9.9.9");
        restarted.SetMacosLiquidGlass(false);
        restarted.SetMacosControlCentreGlass(false);
        restarted.SetRevealDurationMs(0);
        restarted.SetLastInstalledVersion(L"", 0);
        restarted.SetLastInstalledAttempts(0);
        EXPECT(restarted.Save());
    }

    {
        // Verify key still persists on subsequent launches and can be intentionally cleared.
        DockConfig afterUpdate;
        EXPECT(afterUpdate.LoadOrCreate());
        EXPECT(!afterUpdate.MacosLiquidGlass());
        EXPECT(!afterUpdate.MacosControlCentreGlass());
        EXPECT(afterUpdate.RevealDurationMs() == 0);
        EXPECT(afterUpdate.GroqApiKey() == L"gsk_unit_test_secret_key_987654");

        // Write a legacy glued TypeSafeApiKey + GroqApiKey file and verify full recovery.
        const std::wstring iniPath = afterUpdate.Path();
        {
            std::ofstream raw(iniPath, std::ios::binary | std::ios::trunc);
            const std::string legacy =
                "[Dock]\nFollowTaskbarPins=0\nTypeSafeApiKey=ts_plain_123\\nGroqApiKey=gsk_legacy_recovered_456\n";
            raw.write(legacy.data(), static_cast<std::streamsize>(legacy.size()));
        }

        DockConfig recovered;
        EXPECT(recovered.LoadOrCreate());
        EXPECT(recovered.TypeSafeApiKey() == L"ts_plain_123");
        EXPECT(recovered.GroqApiKey() == L"gsk_legacy_recovered_456");
        EXPECT(recovered.Save());

        DockConfig reEncrypted;
        EXPECT(reEncrypted.LoadOrCreate());
        EXPECT(reEncrypted.TypeSafeApiKey() == L"ts_plain_123");
        EXPECT(reEncrypted.GroqApiKey() == L"gsk_legacy_recovered_456");

        // Intentional empty key removal still works.
        reEncrypted.SetGroqApiKey(L"");
        EXPECT(reEncrypted.Save());

        DockConfig cleared;
        EXPECT(cleared.LoadOrCreate());
        EXPECT(cleared.GroqApiKey().empty());
    }

    const std::wstring cfgDir = testRoot + L"\\LiquidGlassDock";
    DeleteFileW((cfgDir + L"\\dock.ini").c_str());
    DeleteFileW((cfgDir + L"\\dock.ini.tmp").c_str());
    RemoveDirectoryW(cfgDir.c_str());
    RemoveDirectoryW(testRoot.c_str());

    RestoreEnv(L"LOCALAPPDATA", hadLocalAppData, origLocalAppData);
    RestoreEnv(L"GROQ_API_KEY", hadGroqEnv, origGroqEnv);
    RestoreEnv(L"TYPESAFE_API_KEY", hadTypeSafeEnv, origTypeSafeEnv);
}
#endif

}  // namespace

int main() {
    TestTypeSafeAndGroqKeysPersistAcrossUpdateCycle();
    TestRecoversLegacyEscapedNewlineCorruption();
    TestBomAndInvalidUtf8DoNotDropGroqApiKey();
    TestRevealDurationSliderAndOverrideParsing();
#ifdef _WIN32
    TestWindowsDockConfigDpapiAndUpdatePersistence();
#endif
    if (g_failures != 0) {
        std::fprintf(stderr, "%d DockConfig test expectation(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("DockConfigTests passed.");
    return EXIT_SUCCESS;
}
