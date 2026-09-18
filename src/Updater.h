#pragma once

#include <string>

// GitHub-Releases based auto updater.
//
// Version feed: https://github.com/<owner>/<repo>/releases/latest (302 to the
// latest tag; carries no API quota). Asset URLs come from the JSON API
// (https://api.github.com/repos/<owner>/<repo>/releases/latest) when
// available, with conventional github.com download URLs as fallback when the
// unauthenticated API quota (60/hour per IP -> HTTP 403) is exhausted.
// Installed copies update via the "*Setup*.exe" NSIS installer, launched
// silent ("/S"); portable copies self-replace the running "Dock.exe" via a
// helper batch file (running the installer from a portable copy would leave
// the running binary stale and re-offer the same version forever). In both
// cases the new build is reopened after install.
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
    // Chooses the asset matching this copy: installed -> Setup installer
    // first, portable -> Dock.exe first, each falling back to the other.
    static void SelectAssetUrls(const ReleaseInfo& release, std::string& urlOut,
        bool& isSetupOut);
    // Launches the downloaded installer silently and returns true if the
    // caller should now exit so files can be replaced. The installer
    // relaunches the dock when finished.
    [[nodiscard]] static bool LaunchInstallerAndExit(const std::wstring& installerPath);
    // Portable path: stages `downloadedExe` over the running binary via a
    // short-lived batch file, relaunches, and returns true if the caller
    // should now exit.
    [[nodiscard]] static bool StagePortableUpdateAndRestart(const std::wstring& downloadedExe);
};
