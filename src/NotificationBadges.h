#pragma once

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// App icon badges (the red unread counts the Windows 11 taskbar draws).
//
// Two sources, both read from the per-user notification store:
// - Explicit badges (BadgeUpdateManager / Windows App SDK) for apps that use
//   the badge API; those apps own their unread state.
// - Unread notifications for every other app: Electron apps such as Discord,
//   Slack or Signal show their count with a taskbar overlay icon, which only
//   Explorer can see, but each message also posts a toast. Live toasts that
//   arrived after the app was last in the foreground (MarkSeen) are counted,
//   like a macOS badge that clears once the app is used.
//
// Windows persists every badge an app publishes (BadgeUpdateManager for
// packaged apps, Windows App SDK BadgeNotificationManager for unpackaged ones)
// in the per-user notification store, which is what Explorer's taskbar reads.
// A worker keeps one read-only connection to that store (winsqlite3, shipped
// with Windows) and wakes on directory change notifications, with a slow
// PRAGMA data_version poll as a safety net. Nothing touches the UI thread
// until the badge set actually changes.
class NotificationBadges {
public:
    struct Badge {
        // Source AppUserModelID (NotificationHandler.PrimaryId).
        std::wstring aumid;
        // Display text: "1".."99", "99+", "!" or empty for a plain dot.
        std::wstring text;
        // Counted from unread notifications rather than an explicit badge.
        bool fromToasts = false;

        [[nodiscard]] bool operator==(const Badge&) const = default;
    };

    NotificationBadges() = default;
    NotificationBadges(const NotificationBadges&) = delete;
    NotificationBadges& operator=(const NotificationBadges&) = delete;
    ~NotificationBadges();

    // notifyMessage is posted (no payload) whenever the badge set changes.
    void Start(HWND notifyWindow, UINT notifyMessage);
    void Stop() noexcept;
    [[nodiscard]] std::vector<Badge> Snapshot() const;
    // The app with this AUMID was just used: notifications that arrived before
    // now no longer count toward its badge. Wakes the worker; never blocks.
    void MarkSeen(const std::wstring& aumid);

    // Badge XML value ("3", "alert", "none", ...) to display text. Returns
    // false when the value shows nothing (none, 0, presence/media glyphs).
    [[nodiscard]] static bool DisplayTextForValue(const std::wstring& value, std::wstring& text);

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void WorkerLoop();
    void Publish(std::vector<Badge> badges);

    mutable std::mutex m_mutex;
    std::vector<Badge> m_badges;
    HWND m_notifyWindow = nullptr;
    UINT m_notifyMessage = 0;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_wakeEvent = nullptr;
    HANDLE m_thread = nullptr;
    // Lower-cased AUMID -> FILETIME last seen; the worker counts toasts after
    // max(m_since, seen). m_since = Start time, so old notifications in
    // Notification Center do not light up every icon at launch.
    std::mutex m_seenMutex;
    std::unordered_map<std::wstring, long long> m_seen;
    long long m_since = 0;
    std::atomic<bool> m_forceRead{false};
};
