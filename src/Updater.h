#pragma once

#include <string>

// GitHub-Releases based auto updater.
//
// Feed: https://api.github.com/repos/<owner>/<repo>/releases/latest
// Asset preference: "*Setup*.exe" (the NSIS installer) first, then the
// portable "Dock.exe". Installer downloads are launched silent ("/S") so the
// update installs without prompts; portable builds self-replace via a helper
// batch file. In both cases the new build is reopened after install.
class Updater {
public:
    struct ReleaseInfo {
        std::string version;      // Normalized, no leading 'v'. Empty = unknown.
        std::string setupUrl;     // Preferred NSIS installer asset, if any.
        std::string exeUrl;       // Portable Dock.exe asset, if any.
        std::string pageUrl;      // Release html_url fallback.
        bool hasUpdate = false;
    };

    [[nodiscard]] static std::string CurrentVersion();
    // True when `latest` is strictly newer than `current`. Handles an
    // optional leading 'v' and numeric dot-separated comparison; a trailing
    // pre-release suffix ("-rc1") is older than the bare release.
    [[nodiscard]] static bool IsNewerVersion(const std::string& latest,
        const std::string& current);

    // Network calls below. Never call on the UI thread: they block on HTTPS.
    [[nodiscard]] static bool FetchLatestRelease(ReleaseInfo& out, std::wstring& error);
    [[nodiscard]] static bool DownloadFile(const std::string& url, const std::wstring& destPath,
        std::wstring& error);

    [[nodiscard]] static std::wstring DefaultDownloadPath(const std::string& version,
        bool isSetup);
    // Installed = running from a per-user or per-machine install directory
    // (LocalAppData\Programs\Hoverdock or Program Files). Portable builds
    // self-replace instead of expecting an installer.
    [[nodiscard]] static bool IsInstalledCopy();
    // Launches the downloaded installer silently and returns true if the
    // caller should now exit so files can be replaced. The installer
    // relaunches the dock when finished.
    [[nodiscard]] static bool LaunchInstallerAndExit(const std::wstring& installerPath);
    // Portable path: stages `downloadedExe` over the running binary via a
    // short-lived batch file, relaunches, and returns true if the caller
    // should now exit.
    [[nodiscard]] static bool StagePortableUpdateAndRestart(const std::wstring& downloadedExe);
};
