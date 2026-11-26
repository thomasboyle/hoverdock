// TrayIconPolicy unit tests (portable; also built on Windows with -DDOCK_BUILD_TESTS=ON).

#include "TrayIconPolicy.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                       \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            std::fprintf(stderr, "%s:%d: EXPECT failed: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                                       \
        }                                                                                       \
    } while (false)

void TestCanonicalKeyAndHelperMatching() {
    using tray_policy::CanonicalAppKey;
    using tray_policy::MatchesTrayAppExe;
    using tray_policy::PreferExecutablePath;
    using tray_policy::PrettyTrayName;

    EXPECT(CanonicalAppKey(L"C:\\Program Files (x86)\\Steam\\steam.exe") == L"steam");
    EXPECT(CanonicalAppKey(L"C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win64\\steamwebhelper.exe") ==
        L"steam");
    EXPECT(CanonicalAppKey(L"C:\\Windows\\System32\\SecurityHealthSystray.exe") == L"securityhealth");
    // Short stems must not be over-stripped.
    EXPECT(CanonicalAppKey(L"C:\\Tools\\nvtray.exe") == L"nvtray");

    EXPECT(PrettyTrayName(L"C:\\Program Files (x86)\\Steam\\steamwebhelper.exe") == L"Steam");
    EXPECT(PrettyTrayName(L"C:\\Program Files (x86)\\Steam\\steam.exe") == L"Steam");

    // Steam main window is hosted by steamwebhelper.exe under Steam's directory tree.
    EXPECT(MatchesTrayAppExe(
        L"C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win64\\steamwebhelper.exe",
        L"C:\\Program Files (x86)\\Steam\\steam.exe"));
    EXPECT(MatchesTrayAppExe(
        L"C:\\Program Files (x86)\\Steam\\steam.exe",
        L"C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win64\\steamwebhelper.exe"));
    // Unrelated app with a different directory must not match.
    EXPECT(!MatchesTrayAppExe(
        L"D:\\Other\\steamwebhelper.exe",
        L"C:\\Program Files (x86)\\Steam\\steam.exe"));
    EXPECT(!MatchesTrayAppExe(
        L"C:\\Program Files\\Spotify\\Spotify.exe",
        L"C:\\Program Files (x86)\\Steam\\steam.exe"));

    // Prefer primary executable over helper executable when merging.
    EXPECT(PreferExecutablePath(
        L"C:\\Program Files (x86)\\Steam\\steam.exe",
        L"C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win64\\steamwebhelper.exe"));
    EXPECT(!PreferExecutablePath(
        L"C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win64\\steamwebhelper.exe",
        L"C:\\Program Files (x86)\\Steam\\steam.exe"));
}

void TestShellButtonMatching() {
    using tray_policy::BestShellButtonMatchScore;
    using tray_policy::IsStrongShellButtonMatch;
    using tray_policy::ShellButtonMatchScore;
    using tray_policy::TrayIdentity;

    // Case-insensitive exact match (previously failed for "steam" vs "Steam").
    EXPECT(ShellButtonMatchScore(L"steam", L"Steam") == 100000);
    EXPECT(IsStrongShellButtonMatch(ShellButtonMatchScore(L"steam", L"Steam")));

    // Dynamic status suffixes after " - " or newline still match the same app.
    EXPECT(IsStrongShellButtonMatch(ShellButtonMatchScore(L"Steam", L"Steam - Online")));
    EXPECT(IsStrongShellButtonMatch(
        ShellButtonMatchScore(L"OneDrive - Up to date", L"OneDrive - Personal")));
    EXPECT(IsStrongShellButtonMatch(
        ShellButtonMatchScore(L"Windows Security - No actions needed.", L"Windows Security - Actions recommended.")));

    // Whole-word prefix matches without conflating distinct apps (Steam vs SteamVR).
    EXPECT(IsStrongShellButtonMatch(ShellButtonMatchScore(L"steam", L"Steam Client")));
    EXPECT(IsStrongShellButtonMatch(ShellButtonMatchScore(L"Discord", L"Discord (2 unread)")));
    EXPECT(!IsStrongShellButtonMatch(ShellButtonMatchScore(L"Steam", L"SteamVR")));
    EXPECT(!IsStrongShellButtonMatch(ShellButtonMatchScore(L"Steam", L"Spotify")));

    // Compact alphanumeric match handles spacing differences.
    EXPECT(IsStrongShellButtonMatch(ShellButtonMatchScore(L"Logi Options+", L"LogiOptions+")));

    // NotifyIconSettings entry matches its UIA button.
    const TrayIdentity steam{
        L"C:\\Program Files (x86)\\Steam\\steam.exe",
        L"steam",
        L"Steam Client Bootstrapper",
        L"",
    };
    EXPECT(IsStrongShellButtonMatch(BestShellButtonMatchScore(steam, L"Steam")));
}

void TestSameTrayAppDeduplication() {
    using tray_policy::AreSameTrayApp;
    using tray_policy::TrayIdentity;

    // Steam from NotifyIconSettings vs steamwebhelper from EnumerateHostIcons.
    const TrayIdentity steamSettings{
        L"C:\\Program Files (x86)\\Steam\\steam.exe",
        L"steam",
        L"Steam",
        L"",
    };
    const TrayIdentity steamHelper{
        L"C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win64\\steamwebhelper.exe",
        L"steamwebhelper",
        L"Steam",
        L"",
    };
    EXPECT(AreSameTrayApp(steamSettings, steamHelper));

    // Steam from NotifyIconSettings vs Steam from EnumerateShellButtons after AssignShellIcons.
    const TrayIdentity steamButton{
        L"C:\\Program Files (x86)\\Steam\\steam.exe",
        L"steam",
        L"Steam",
        L"Steam",
    };
    EXPECT(AreSameTrayApp(steamSettings, steamButton));

    // Explorer-hosted buttons with no executablePath deduplicate by title.
    const TrayIdentity bt1{L"", L"", L"Bluetooth Devices", L"Bluetooth Devices"};
    const TrayIdentity bt2{L"", L"", L"Bluetooth Devices", L"bluetooth devices"};
    EXPECT(AreSameTrayApp(bt1, bt2));

    // Distinct apps must never merge.
    const TrayIdentity discord{
        L"C:\\Users\\user\\AppData\\Local\\Discord\\app-1.0\\Discord.exe",
        L"Discord",
        L"Discord",
        L"Discord",
    };
    EXPECT(!AreSameTrayApp(steamSettings, discord));
}

}  // namespace

int main() {
    TestCanonicalKeyAndHelperMatching();
    TestShellButtonMatching();
    TestSameTrayAppDeduplication();
    if (g_failures == 0) {
        std::printf("TrayIconPolicy tests: all passed\n");
        return 0;
    }
    std::fprintf(stderr, "TrayIconPolicy tests: %d failure(s)\n", g_failures);
    return 1;
}
