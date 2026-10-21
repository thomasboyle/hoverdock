#include "SearchHotkey.h"

namespace {

constexpr int kHotkeyId = 1;
// Thread message: wParam 1 = release the chord, 0 = register it again.
constexpr UINT kSuspendMessage = WM_APP + 1;

bool RegisterChord() noexcept {
    return RegisterHotKey(nullptr, kHotkeyId, MOD_CONTROL | MOD_NOREPEAT, VK_SPACE) != FALSE;
}

}  // namespace

SearchHotkey::~SearchHotkey() {
    Stop();
}

bool SearchHotkey::Start(HWND target, UINT message) {
    if (m_thread.joinable()) {
        return m_installed.load();
    }
    m_target = target;
    m_message = message;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ready == nullptr) {
        return false;
    }
    m_thread = std::thread([this, ready] { Run(ready); });
    WaitForSingleObject(ready, 2000);
    CloseHandle(ready);
    if (!m_installed.load()) {
        Stop();
        return false;
    }
    return true;
}

void SearchHotkey::Stop() noexcept {
    if (!m_thread.joinable()) {
        return;
    }
    if (m_threadId != 0) {
        PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
    }
    m_thread.join();
    m_threadId = 0;
    m_installed.store(false);
    m_suspended.store(false);
}

void SearchHotkey::SetSuspended(bool suspended) noexcept {
    if (!m_thread.joinable() || m_threadId == 0 || m_suspended.exchange(suspended) == suspended) {
        return;
    }
    PostThreadMessageW(m_threadId, kSuspendMessage, suspended ? 1 : 0, 0);
}

void SearchHotkey::Run(HANDLE ready) {
    m_threadId = GetCurrentThreadId();
    MSG message{};
    // Create this thread's message queue before Start() can post WM_QUIT.
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    // A null window: WM_HOTKEY arrives in this thread's queue.
    const bool registered = RegisterChord();
    m_installed.store(registered);
    SetEvent(ready);
    if (!registered) {
        return;
    }
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_HOTKEY && message.wParam == kHotkeyId) {
            PostMessageW(m_target, m_message, 0, 0);
        } else if (message.message == kSuspendMessage) {
            if (message.wParam != 0) {
                UnregisterHotKey(nullptr, kHotkeyId);
            } else {
                // Fails only if another app took the chord while it was
                // released; it is tried again after the next suspend.
                static_cast<void>(RegisterChord());
            }
        }
    }
    UnregisterHotKey(nullptr, kHotkeyId);
}
