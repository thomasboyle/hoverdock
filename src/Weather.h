#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Live tray weather for the dock clock area.
// Data: Open-Meteo forecast API (no key). Icons: vendored Meteocons fill PNGs.
class WeatherService {
public:
    static constexpr wchar_t kTarget[] = L"dock:tray:weather";
    // Sheffield, GB — Thomas's default when location is unknown.
    static constexpr double kFallbackLat = 53.38;
    static constexpr double kFallbackLon = -1.47;
    static constexpr UINT kRefreshIntervalMs = 12U * 60U * 1000U;  // 12 min

    struct Snapshot {
        bool valid = false;
        int weatherCode = 0;
        bool isDay = true;
        float temperatureC = 0.0F;
        std::string slug = "cloudy";
        std::wstring condition;
        std::wstring tip;  // e.g. L"Overcast · 19°C"
    };

    void Start(HWND notifyWindow, UINT notifyMessage);
    void Stop() noexcept;
    void RequestRefresh();  // non-blocking; posts notifyMessage when state changes

    [[nodiscard]] Snapshot GetSnapshot() const;
    [[nodiscard]] bool HasIcon() const;
    // Premultiplied BGRA atlas-sized buffer (extent x extent), empty on failure.
    [[nodiscard]] std::vector<uint8_t> RasterizeIcon(UINT extent) const;
    [[nodiscard]] static const wchar_t* Target() noexcept { return kTarget; }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void WorkerLoop();
    bool ResolveLocation(double& lat, double& lon);
    bool FetchForecast(double lat, double lon, Snapshot& out, std::wstring& error);
    void LoadCachedLocation();
    void SaveCachedLocation(double lat, double lon) const;
    static std::wstring CachePath();
    static const char* SlugForWmo(int code, bool isDay) noexcept;
    static const wchar_t* ConditionForWmo(int code) noexcept;
    static std::vector<uint8_t> DecodePngToPremul(const uint8_t* bytes, size_t size, UINT extent);

    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
    HWND m_notifyWindow = nullptr;
    UINT m_notifyMessage = 0;
    HANDLE m_wakeEvent = nullptr;
    HANDLE m_thread = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_refreshRequested{false};
    double m_lat = kFallbackLat;
    double m_lon = kFallbackLon;
    bool m_haveLocation = false;
};
