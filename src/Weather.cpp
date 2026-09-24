#include "Weather.h"
#include "WeatherIconsData.h"

#include <objbase.h>
#include <winhttp.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <wrl/client.h>


using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kConnectTimeoutMs = 8000;
constexpr DWORD kSendTimeoutMs = 8000;
constexpr DWORD kReceiveTimeoutMs = 12000;

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) noexcept : m_handle(handle) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = nullptr;
    }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }
    ~WinHttpHandle() { Reset(); }

    explicit operator bool() const noexcept { return m_handle != nullptr; }
    HINTERNET Get() const noexcept { return m_handle; }
    void Reset() noexcept {
        if (m_handle != nullptr) {
            WinHttpCloseHandle(m_handle);
            m_handle = nullptr;
        }
    }

private:
    HINTERNET m_handle = nullptr;
};

bool HttpGet(const wchar_t* host, INTERNET_PORT port, bool secure, const wchar_t* path,
    std::string& body, std::wstring& error) {
    body.clear();
    WinHttpHandle session(WinHttpOpen(L"HoverdockWeather/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = L"WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(session.Get(), kConnectTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
        kReceiveTimeoutMs);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session.Get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    WinHttpHandle connection(WinHttpConnect(session.Get(), host, port, 0));
    if (!connection) {
        error = L"WinHttpConnect failed";
        return false;
    }
    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connection.Get(), L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) {
        error = L"WinHttpOpenRequest failed";
        return false;
    }
    if (WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
            0, 0, 0) == FALSE) {
        error = L"WinHttpSendRequest failed";
        return false;
    }
    if (WinHttpReceiveResponse(request.Get(), nullptr) == FALSE) {
        error = L"WinHttpReceiveResponse failed";
        return false;
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) == FALSE ||
        status < 200 || status >= 300) {
        error = L"HTTP status " + std::to_wstring(status);
        return false;
    }

    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request.Get(), &available) == FALSE) {
            error = L"WinHttpQueryDataAvailable failed";
            return false;
        }
        if (available == 0) {
            break;
        }
        if (body.size() + available > 512 * 1024) {
            error = L"Response too large";
            return false;
        }
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (WinHttpReadData(request.Get(), body.data() + offset, available, &read) == FALSE) {
            error = L"WinHttpReadData failed";
            return false;
        }
        body.resize(offset + read);
    }
    return true;
}

bool ParseJsonNumber(const std::string& json, const char* key, double& out) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    size_t i = pos + needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) {
        ++i;
    }
    char* end = nullptr;
    const double value = std::strtod(json.c_str() + i, &end);
    if (end == json.c_str() + i) {
        return false;
    }
    out = value;
    return true;
}

bool ParseJsonInt(const std::string& json, const char* key, int& out) {
    double value = 0.0;
    if (!ParseJsonNumber(json, key, value)) {
        return false;
    }
    out = static_cast<int>(std::lround(value));
    return true;
}


}  // namespace

void WeatherService::Start(HWND notifyWindow, UINT notifyMessage) {
    Stop();
    m_notifyWindow = notifyWindow;
    m_notifyMessage = notifyMessage;
    LoadCachedLocation();
    m_wakeEvent = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    if (m_wakeEvent == nullptr) {
        return;
    }
    m_running.store(true);
    m_refreshRequested.store(true);
    m_thread = CreateThread(nullptr, 0, &WeatherService::ThreadProc, this, 0, nullptr);
    if (m_thread == nullptr) {
        m_running.store(false);
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
}

void WeatherService::Stop() noexcept {
    m_running.store(false);
    if (m_wakeEvent != nullptr) {
        SetEvent(m_wakeEvent);
    }
    if (m_thread != nullptr) {
        WaitForSingleObject(m_thread, 15000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_wakeEvent != nullptr) {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
    m_notifyWindow = nullptr;
    m_notifyMessage = 0;
}

void WeatherService::RequestRefresh() {
    m_refreshRequested.store(true);
    if (m_wakeEvent != nullptr) {
        SetEvent(m_wakeEvent);
    }
}

WeatherService::Snapshot WeatherService::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

bool WeatherService::HasIcon() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot.valid && WeatherIconsData::Find(m_snapshot.slug.c_str()) != nullptr;
}

std::vector<uint8_t> WeatherService::RasterizeIcon(UINT extent) const {
    Snapshot snap = GetSnapshot();
    const char* slug = snap.valid ? snap.slug.c_str() : "cloudy";
    const WeatherIconsData::Entry* entry = WeatherIconsData::Find(slug);
    if (entry == nullptr) {
        entry = WeatherIconsData::Find("cloudy");
    }
    if (entry == nullptr) {
        return {};
    }
    return DecodePngToPremul(entry->bytes, entry->size, std::max(1U, extent));
}

void WeatherService::WorkerLoop() {
    // COM not required for WinHTTP/WIC factory (WIC used on UI thread for raster).
    while (m_running) {
        (void)m_refreshRequested.exchange(false);
        {
            double lat = kFallbackLat;
            double lon = kFallbackLon;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_haveLocation) {
                    lat = m_lat;
                    lon = m_lon;
                }
            }
            if (!m_haveLocation) {
                ResolveLocation(lat, lon);
            }

            Snapshot next;
            std::wstring error;
            if (FetchForecast(lat, lon, next, error)) {
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    changed = !m_snapshot.valid || m_snapshot.slug != next.slug ||
                        m_snapshot.weatherCode != next.weatherCode ||
                        m_snapshot.isDay != next.isDay ||
                        std::fabs(m_snapshot.temperatureC - next.temperatureC) >= 0.4F;
                    m_snapshot = next;
                    m_lat = lat;
                    m_lon = lon;
                    m_haveLocation = true;
                }
                SaveCachedLocation(lat, lon);
                if (changed && m_notifyWindow != nullptr && IsWindow(m_notifyWindow) != FALSE &&
                    m_notifyMessage != 0) {
                    PostMessageW(m_notifyWindow, m_notifyMessage, 0, 0);
                }
            }
            // Fail soft: keep last snapshot; never clear on transient errors.
        }

        if (!m_running) {
            break;
        }
        const DWORD wait = WaitForSingleObject(m_wakeEvent, kRefreshIntervalMs);
        if (!m_running) {
            break;
        }
        if (wait == WAIT_TIMEOUT) {
            m_refreshRequested.store(true);
        }
    }
}

bool WeatherService::ResolveLocation(double& lat, double& lon) {
    // Prefer last cached coords already loaded. Then IP geolocation. Then Sheffield.
    if (m_haveLocation) {
        lat = m_lat;
        lon = m_lon;
        return true;
    }

    std::string body;
    std::wstring error;
    // ip-api.com: free, no key, HTTP. fields=status,lat,lon
    if (HttpGet(L"ip-api.com", INTERNET_DEFAULT_HTTP_PORT, false,
            L"/json/?fields=status,lat,lon", body, error)) {
        // {"status":"success","lat":53.38,"lon":-1.47}
        if (body.find("\"success\"") != std::string::npos) {
            double parsedLat = 0.0;
            double parsedLon = 0.0;
            if (ParseJsonNumber(body, "lat", parsedLat) && ParseJsonNumber(body, "lon", parsedLon) &&
                parsedLat >= -90.0 && parsedLat <= 90.0 && parsedLon >= -180.0 &&
                parsedLon <= 180.0) {
                lat = parsedLat;
                lon = parsedLon;
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lat = lat;
                m_lon = lon;
                m_haveLocation = true;
                return true;
            }
        }
    }

    lat = kFallbackLat;
    lon = kFallbackLon;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lat = lat;
    m_lon = lon;
    m_haveLocation = true;
    return true;
}

bool WeatherService::FetchForecast(double lat, double lon, Snapshot& out, std::wstring& error) {
    out = Snapshot{};
    wchar_t path[256]{};
    swprintf_s(path,
        L"/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,is_day&timezone=auto",
        lat, lon);

    std::string body;
    if (!HttpGet(L"api.open-meteo.com", INTERNET_DEFAULT_HTTPS_PORT, true, path, body, error)) {
        return false;
    }

    // Prefer the "current" object: find "current":{ ... } then parse inside.
    const size_t currentPos = body.find("\"current\"");
    std::string scope = body;
    if (currentPos != std::string::npos) {
        const size_t brace = body.find('{', currentPos);
        if (brace != std::string::npos) {
            scope = body.substr(brace);
        }
    }

    int code = 0;
    int isDay = 1;
    double temp = 0.0;
    if (!ParseJsonInt(scope, "weather_code", code) && !ParseJsonInt(scope, "weathercode", code)) {
        error = L"Missing weather_code";
        return false;
    }
    ParseJsonInt(scope, "is_day", isDay);
    ParseJsonNumber(scope, "temperature_2m", temp);

    out.valid = true;
    out.weatherCode = code;
    out.isDay = isDay != 0;
    out.temperatureC = static_cast<float>(temp);
    out.slug = SlugForWmo(code, out.isDay);
    out.condition = ConditionForWmo(code);
    const int rounded = static_cast<int>(std::lround(out.temperatureC));
    out.tip = out.condition + L" · " + std::to_wstring(rounded) + L"°C";
    return true;
}

DWORD WINAPI WeatherService::ThreadProc(LPVOID param) {
    static_cast<WeatherService*>(param)->WorkerLoop();
    return 0;
}

std::wstring WeatherService::CachePath() {
    wchar_t base[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    std::wstring dir = std::wstring(base) + L"\\LiquidGlassDock";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\weather_location.ini";
}

void WeatherService::LoadCachedLocation() {
    const std::wstring path = CachePath();
    if (path.empty()) {
        return;
    }
    wchar_t latText[64]{};
    wchar_t lonText[64]{};
    GetPrivateProfileStringW(L"Weather", L"Lat", L"", latText, static_cast<DWORD>(std::size(latText)),
        path.c_str());
    GetPrivateProfileStringW(L"Weather", L"Lon", L"", lonText, static_cast<DWORD>(std::size(lonText)),
        path.c_str());
    if (latText[0] == L'\0' || lonText[0] == L'\0') {
        return;
    }
    const double lat = _wtof(latText);
    const double lon = _wtof(lonText);
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lat = lat;
    m_lon = lon;
    m_haveLocation = true;
}

void WeatherService::SaveCachedLocation(double lat, double lon) const {
    const std::wstring path = CachePath();
    if (path.empty()) {
        return;
    }
    wchar_t latText[64]{};
    wchar_t lonText[64]{};
    swprintf_s(latText, L"%.5f", lat);
    swprintf_s(lonText, L"%.5f", lon);
    WritePrivateProfileStringW(L"Weather", L"Lat", latText, path.c_str());
    WritePrivateProfileStringW(L"Weather", L"Lon", lonText, path.c_str());
}

const char* WeatherService::SlugForWmo(int code, bool isDay) noexcept {
    switch (code) {
    case 0:
        return isDay ? "clear-day" : "clear-night";
    case 1:
        return isDay ? "partly-cloudy-day" : "partly-cloudy-night";
    case 2:
        return isDay ? "partly-cloudy-day" : "partly-cloudy-night";
    case 3:
        return isDay ? "overcast-day" : "overcast-night";
    case 45:
    case 48:
        return "fog";
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
        return "drizzle";
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
    case 80:
    case 81:
    case 82:
        return "rain";
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return "snow";
    case 95:
        return isDay ? "thunderstorms-day" : "thunderstorms-night";
    case 96:
    case 99:
        return "thunderstorms-rain";
    default:
        return "cloudy";
    }
}

const wchar_t* WeatherService::ConditionForWmo(int code) noexcept {
    switch (code) {
    case 0:
        return L"Clear";
    case 1:
        return L"Mainly clear";
    case 2:
        return L"Partly cloudy";
    case 3:
        return L"Overcast";
    case 45:
    case 48:
        return L"Fog";
    case 51:
    case 53:
    case 55:
        return L"Drizzle";
    case 56:
    case 57:
        return L"Freezing drizzle";
    case 61:
    case 63:
    case 65:
        return L"Rain";
    case 66:
    case 67:
        return L"Freezing rain";
    case 71:
    case 73:
    case 75:
        return L"Snow";
    case 77:
        return L"Snow grains";
    case 80:
    case 81:
    case 82:
        return L"Rain showers";
    case 85:
    case 86:
        return L"Snow showers";
    case 95:
        return L"Thunderstorm";
    case 96:
    case 99:
        return L"Thunderstorm with hail";
    default:
        return L"Cloudy";
    }
}

std::vector<uint8_t> WeatherService::DecodePngToPremul(const uint8_t* bytes, size_t size,
    UINT extent) {
    if (bytes == nullptr || size == 0 || extent == 0) {
        return {};
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)))) {
        return {};
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), static_cast<DWORD>(size)))) {
        return {};
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad, &decoder))) {
        return {};
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return {};
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return {};
    }

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(converter.Get(), extent, extent, WICBitmapInterpolationModeFant))) {
        return {};
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(extent) * extent * 4U, 0);
    const UINT stride = extent * 4U;
    if (FAILED(scaler->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) {
        return {};
    }
    return pixels;
}
