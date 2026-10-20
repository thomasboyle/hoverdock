#include "SearchHotkey.h"

namespace {

SearchHotkey* g_hotkey = nullptr;
HHOOK g_hook = nullptr;
// Unassigned virtual key: tells the shell "Win was used in a chord" so
// releasing Win does not open Start (same trick as PowerToys/AutoHotkey).
constexpr WORD kMaskKey = 0xE8;

bool Down(int key) noexcept {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

void SendMaskKey() noexcept {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = kMaskKey;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
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
    return m_installed.load();
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
}

void SearchHotkey::Run(HANDLE ready) {
    m_threadId = GetCurrentThreadId();
    MSG message{};
    // Create this thread's message queue before Start() can post WM_QUIT.
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_hotkey = this;
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &SearchHotkey::Hook, GetModuleHandleW(nullptr), 0);
    m_installed.store(g_hook != nullptr);
    SetEvent(ready);
    if (g_hook == nullptr) {
        g_hotkey = nullptr;
        return;
    }
    // Input-latency critical: never let normal work preempt the hook thread.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    g_hotkey = nullptr;
}

LRESULT CALLBACK SearchHotkey::Hook(int code, WPARAM wParam, LPARAM lParam) {
    SearchHotkey* self = g_hotkey;
    if (code == HC_ACTION && self != nullptr) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        // Injected Space is honoured too (PowerToys / AutoHotkey remaps); our
        // own masking key is not Space, so it cannot re-trigger this.
        if (key->vkCode == VK_SPACE) {
            const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            if (keyDown && (Down(VK_LWIN) || Down(VK_RWIN)) && !Down(VK_CONTROL) && !Down(VK_MENU) &&
                !Down(VK_SHIFT)) {
                if (!self->m_spaceHeld) {
                    self->m_spaceHeld = true;
                    SendMaskKey();
                    PostMessageW(self->m_target, self->m_message, 0, 0);
                }
                return 1;
            }
            if (!keyDown && self->m_spaceHeld) {
                self->m_spaceHeld = false;
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
