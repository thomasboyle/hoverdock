#pragma once

#include <Windows.h>

#include <cstdio>
#include <mutex>

class ProfileScope {
public:
    explicit ProfileScope(const char* name) noexcept : m_name(name) {
        if (!Enabled()) {
            return;
        }
        QueryPerformanceCounter(&m_start);
        m_active = true;
    }

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

    ~ProfileScope() {
        if (!m_active) {
            return;
        }

        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        const double milliseconds = static_cast<double>(end.QuadPart - m_start.QuadPart) * 1000.0 /
            static_cast<double>(Frequency());
        Log(m_name, milliseconds);
    }

    static void Mark(const char* name) {
        if (!Enabled()) {
            return;
        }
        Log(name, 0.0);
    }

private:
    static bool Enabled() noexcept {
        static const bool enabled = []() {
            wchar_t value[8]{};
            return GetEnvironmentVariableW(L"HOVERDOCK_PROFILE", value,
                       static_cast<DWORD>(std::size(value))) != 0 &&
                value[0] != L'0';
        }();
        return enabled;
    }

    static long long Frequency() noexcept {
        static const long long frequency = []() {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        return frequency;
    }

    static void Log(const char* name, double milliseconds) {
        static std::mutex mutex;
        const std::lock_guard lock(mutex);
        static FILE* file = nullptr;
        static LARGE_INTEGER origin{};
        if (file == nullptr) {
            if (fopen_s(&file, "D:\\C++\\hoverdock\\.system-analysis\\profile.log", "a") != 0 ||
                file == nullptr) {
                return;
            }
            QueryPerformanceCounter(&origin);
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = static_cast<double>(now.QuadPart - origin.QuadPart) * 1000.0 /
            static_cast<double>(Frequency());

        if (milliseconds <= 0.0) {
            std::fprintf(file, "MARK %s\t+%.3f ms\n", name, elapsed);
        } else {
            std::fprintf(file, "%.3f ms\t%s\t+%.3f ms\n", milliseconds, name, elapsed);
        }
        std::fflush(file);
    }

    const char* m_name = nullptr;
    LARGE_INTEGER m_start{};
    bool m_active = false;
};
