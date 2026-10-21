#pragma once

// Global Ctrl+Space hotkey for dock Search, registered with RegisterHotKey on
// its own thread (no keyboard hook: nothing sits in the system input path).
// MOD_NOREPEAT keeps a held chord from re-toggling the panel.

#include <Windows.h>

#include <atomic>
#include <thread>

class SearchHotkey {
public:
    SearchHotkey() = default;
    ~SearchHotkey();
    SearchHotkey(const SearchHotkey&) = delete;
    SearchHotkey& operator=(const SearchHotkey&) = delete;

    // Posts `message` to `target` on every Ctrl+Space press. False when the
    // chord is already registered by another app.
    bool Start(HWND target, UINT message);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept { return m_thread.joinable(); }

private:
    void Run(HANDLE ready);

    std::thread m_thread;
    DWORD m_threadId = 0;
    HWND m_target = nullptr;
    UINT m_message = 0;
    std::atomic<bool> m_installed{false};
};
