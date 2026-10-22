#pragma once

#include <Windows.h>
#include <objbase.h>

#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

// One long-lived COM single-threaded-apartment thread that runs posted jobs in
// order. Replaces a new thread (and a new COM apartment) per job. Between jobs
// it sleeps in MsgWaitForMultipleObjectsEx and pumps its own messages, as an
// STA thread must, so it costs no wakeups while idle.
class StaWorker {
public:
    StaWorker() = default;
    ~StaWorker() {
        Stop();
    }

    StaWorker(const StaWorker&) = delete;
    StaWorker& operator=(const StaWorker&) = delete;

    // Starts the thread on first use. Jobs posted after Stop() are dropped.
    void Post(std::function<void()> job) {
        {
            const std::lock_guard lock(m_mutex);
            if (m_stopping) {
                return;
            }
            if (m_wake == nullptr) {
                m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (m_wake == nullptr) {
                    // No event, no worker: one-off STA thread, as before.
                    std::thread([job = std::move(job)]() {
                        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                        job();
                        if (SUCCEEDED(com)) {
                            CoUninitialize();
                        }
                    }).detach();
                    return;
                }
                m_thread = std::thread(&StaWorker::Main, this);
            }
            m_jobs.push_back(std::move(job));
        }
        SetEvent(m_wake);
    }

    // Runs the jobs already queued, then joins the thread.
    void Stop() noexcept {
        {
            const std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        if (m_wake != nullptr) {
            SetEvent(m_wake);
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_wake != nullptr) {
            CloseHandle(m_wake);
            m_wake = nullptr;
        }
    }

private:
    void Main() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            std::function<void()> job;
            {
                const std::lock_guard lock(m_mutex);
                if (!m_jobs.empty()) {
                    job = std::move(m_jobs.front());
                    m_jobs.pop_front();
                } else if (m_stopping) {
                    break;
                }
            }
            if (job) {
                job();
                continue;
            }
            MsgWaitForMultipleObjectsEx(1, &m_wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    }

    std::mutex m_mutex;
    std::deque<std::function<void()>> m_jobs;
    HANDLE m_wake = nullptr;
    std::thread m_thread;
    bool m_stopping = false;
};
