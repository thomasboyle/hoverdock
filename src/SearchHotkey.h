#pragma once

// Global Win+Space hotkey for dock Search. Win+Space is owned by the shell
// (input language switch), so RegisterHotKey cannot claim it; a low-level
// keyboard hook on its own thread swallows the Space press instead. The hook
// thread does nothing else, so a busy dock UI thread can never delay
// system-wide keyboard input.

#include <Windows.h>

#include <atomic>
#include <thread>

class SearchHotkey {
public:
    SearchHotkey() = default;
    ~SearchHotkey();
    SearchHotkey(const SearchHotkey&) = delete;
    SearchHotkey& operator=(const SearchHotkey&) = delete;

    // Posts `message` to `target` on every Win+Space press.
    bool Start(HWND target, UINT message);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept { return m_thread.joinable(); }

private:
    static LRESULT CALLBACK Hook(int code, WPARAM wParam, LPARAM lParam);
    void Run(HANDLE ready);

    std::thread m_thread;
    DWORD m_threadId = 0;
    HWND m_target = nullptr;
    UINT m_message = 0;
    bool m_spaceHeld = false;
    std::atomic<bool> m_installed{false};
};
