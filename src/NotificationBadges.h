#pragma once

#include <Windows.h>

#include <mutex>
#include <string>
#include <vector>

// App icon badges (the red unread counts the Windows 11 taskbar draws).
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
    HANDLE m_thread = nullptr;
};
