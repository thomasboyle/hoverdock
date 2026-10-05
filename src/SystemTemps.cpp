#include "SystemTemps.h"

#include <objbase.h>
#include <oleauto.h>
#include <Wbemidl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr ULONGLONG kPollIntervalMs = 2000;
// The UI re-arms this every ~2 s while the home page is up; the worker idles
// (no WMI/PDH/NVML calls) once it lapses.
constexpr ULONGLONG kKeepAliveMs = 5000;
// LibreHardwareMonitor / OpenHardwareMonitor may be started after the dock.
constexpr ULONGLONG kHwmonRetryMs = 15000;
constexpr ULONGLONG kSmiRetryMs = 60000;
constexpr DWORD kWmiNextTimeoutMs = 3000;
constexpr DWORD kSmiTimeoutMs = 4000;
// Many desktop boards expose a fixed ACPI _TZ placeholder of 301.0 K (27.85 C)
// that never tracks the die. Only trust that exact value once it has moved.
constexpr int kAcpiPlaceholderDeciKelvin = 3010;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool Plausible(double celsius) noexcept {
    return std::isfinite(celsius) && celsius >= 5.0 && celsius <= 125.0;
}

std::wstring Lower(std::wstring text) {
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return text;
}

bool Has(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

struct ScopedBstr {
    explicit ScopedBstr(const wchar_t* text) noexcept : value(SysAllocString(text)) {}
    ~ScopedBstr() { SysFreeString(value); }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;
    BSTR value;
};

void SetBlanket(IUnknown* proxy) noexcept {
    if (proxy != nullptr) {
        CoSetProxyBlanket(proxy, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    }
}

ComPtr<IWbemServices> ConnectWmi(const wchar_t* path) {
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))) {
        return nullptr;
    }
    ScopedBstr resource(path);
    ComPtr<IWbemServices> services;
    if (FAILED(locator->ConnectServer(resource.value, nullptr, nullptr, nullptr,
            WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &services))) {
        return nullptr;
    }
    SetBlanket(services.Get());
    return services;
}

template <typename RowFn>
HRESULT ForEachRow(IWbemServices* services, const wchar_t* query, RowFn&& onRow) {
    ScopedBstr language(L"WQL");
    ScopedBstr text(query);
    ComPtr<IEnumWbemClassObject> rows;
    HRESULT hr = services->ExecQuery(language.value, text.value,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows);
    if (FAILED(hr)) {
        return hr;
    }
    SetBlanket(rows.Get());
    for (;;) {
        ComPtr<IWbemClassObject> row;
        ULONG returned = 0;
        hr = rows->Next(static_cast<LONG>(kWmiNextTimeoutMs), 1, &row, &returned);
        if (hr == static_cast<HRESULT>(WBEM_S_TIMEDOUT)) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        if (FAILED(hr)) {
            return hr;
        }
        if (returned == 0 || !row) {
            return S_OK;
        }
        onRow(row.Get());
    }
}

std::wstring StringProp(IWbemClassObject* row, const wchar_t* name) {
    std::wstring out;
    VARIANT value;
    VariantInit(&value);
    if (SUCCEEDED(row->Get(name, 0, &value, nullptr, nullptr)) && value.vt == VT_BSTR &&
        value.bstrVal != nullptr) {
        out = value.bstrVal;
    }
    VariantClear(&value);
    return out;
}

double NumberProp(IWbemClassObject* row, const wchar_t* name) {
    double out = kNaN;
    VARIANT value;
    VariantInit(&value);
    if (SUCCEEDED(row->Get(name, 0, &value, nullptr, nullptr)) && value.vt != VT_NULL &&
        value.vt != VT_EMPTY) {
        VARIANT number;
        VariantInit(&number);
        if (SUCCEEDED(VariantChangeType(&number, &value, 0, VT_R8))) {
            out = number.dblVal;
        }
        VariantClear(&number);
    }
    VariantClear(&value);
    return out;
}

// --- LibreHardwareMonitor / OpenHardwareMonitor WMI ------------------------
// Both publish a Sensor class while running (LHM needs admin for CPU MSRs).
// These read the real package/die sensors, so they win for the CPU.
struct HwmonSource {
    ComPtr<IWbemServices> services;
    std::wstring name;
    ULONGLONG retryAt = 0;

    static int CpuScore(const std::wstring& label) {
        if (Has(label, L"distance")) {
            return 0;  // "Distance to TjMax" is a Temperature-typed delta, not a reading
        }
        if (label == L"cpu package" || label == L"core (tctl/tdie)" || label == L"core (tdie)") {
            return 4;
        }
        if (label == L"core (tctl)") {
            return 3;
        }
        if (label == L"core max") {
            return 2;
        }
        return Has(label, L"core") || Has(label, L"ccd") ? 1 : 0;
    }

    static int GpuScore(const std::wstring& label) {
        if (label == L"gpu core") {
            return 3;
        }
        if (Has(label, L"hot spot") || Has(label, L"memory") || Has(label, L"junction")) {
            return 0;
        }
        return 1;
    }

    void Poll(ULONGLONG now, double& cpu, double& gpu) {
        cpu = kNaN;
        gpu = kNaN;
        if (!services) {
            if (now < retryAt) {
                return;
            }
            retryAt = now + kHwmonRetryMs;
            services = ConnectWmi(L"ROOT\\LibreHardwareMonitor");
            name = L"LibreHardwareMonitor";
            if (!services) {
                services = ConnectWmi(L"ROOT\\OpenHardwareMonitor");
                name = L"OpenHardwareMonitor";
            }
            if (!services) {
                name.clear();
                return;
            }
        }
        int cpuBest = 0;
        int gpuBest = 0;
        const HRESULT hr = ForEachRow(services.Get(),
            L"SELECT Identifier, Name, Value FROM Sensor WHERE SensorType = 'Temperature'",
            [&](IWbemClassObject* row) {
                const double value = NumberProp(row, L"Value");
                if (!Plausible(value)) {
                    return;
                }
                const std::wstring id = Lower(StringProp(row, L"Identifier"));
                const std::wstring label = Lower(StringProp(row, L"Name"));
                // LHM: /intelcpu/0/..., /amdcpu/0/..., /gpu-nvidia/0/...
                // OHM: /intelcpu/0/..., /nvidiagpu/0/..., /atigpu/0/...
                if (Has(id, L"cpu/")) {
                    const int score = CpuScore(label);
                    if (score > cpuBest || (score == cpuBest && score > 0 && value > cpu)) {
                        cpuBest = score;
                        cpu = value;
                    }
                } else if (Has(id, L"gpu")) {
                    const int score = GpuScore(label);
                    if (score > gpuBest || (score == gpuBest && score > 0 && value > gpu)) {
                        gpuBest = score;
                        gpu = value;
                    }
                }
            });
        if (FAILED(hr)) {
            // Monitor exited (namespace or provider gone). Reconnect later.
            services.Reset();
            name.clear();
            retryAt = now + kHwmonRetryMs;
            cpu = kNaN;
            gpu = kNaN;
        }
    }
};

// --- NVML (nvml.dll ships with the NVIDIA driver; nvidia-smi is built on it) -
struct NvmlSource {
    using Device = struct NvmlDeviceOpaque*;
    using InitFn = int (*)();
    using CountFn = int (*)(unsigned int*);
    using HandleFn = int (*)(unsigned int, Device*);
    using TempFn = int (*)(Device, int, unsigned int*);

    HMODULE module = nullptr;
    TempFn temperature = nullptr;
    std::vector<Device> devices;
    bool tried = false;
    int failures = 0;

    void Load() {
        tried = true;
        module = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            wchar_t path[MAX_PATH] = {};
            if (ExpandEnvironmentStringsW(L"%ProgramW6432%\\NVIDIA Corporation\\NVSMI\\nvml.dll", path,
                    MAX_PATH) > 0) {
                module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            }
        }
        if (module == nullptr) {
            return;
        }
        const auto init = reinterpret_cast<InitFn>(GetProcAddress(module, "nvmlInit_v2"));
        const auto count = reinterpret_cast<CountFn>(GetProcAddress(module, "nvmlDeviceGetCount_v2"));
        const auto handle =
            reinterpret_cast<HandleFn>(GetProcAddress(module, "nvmlDeviceGetHandleByIndex_v2"));
        temperature = reinterpret_cast<TempFn>(GetProcAddress(module, "nvmlDeviceGetTemperature"));
        unsigned int deviceCount = 0;
        if (init == nullptr || count == nullptr || handle == nullptr || temperature == nullptr ||
            init() != 0 || count(&deviceCount) != 0) {
            temperature = nullptr;
            FreeLibrary(module);
            module = nullptr;
            return;
        }
        for (unsigned int index = 0; index < std::min(deviceCount, 8U); ++index) {
            Device device = nullptr;
            if (handle(index, &device) == 0 && device != nullptr) {
                devices.push_back(device);
            }
        }
    }

    double Read() {
        if (!tried) {
            Load();
        }
        if (temperature == nullptr || devices.empty()) {
            return kNaN;
        }
        double best = kNaN;
        for (Device device : devices) {
            unsigned int value = 0;
            // NVML_TEMPERATURE_GPU = 0 (core die sensor).
            if (temperature(device, 0, &value) == 0 && Plausible(static_cast<double>(value))) {
                best = std::isfinite(best) ? std::max(best, static_cast<double>(value))
                                           : static_cast<double>(value);
            }
        }
        if (!std::isfinite(best) && ++failures >= 5) {
            devices.clear();  // driver reset / GPU gone: hand over to nvidia-smi
        } else if (std::isfinite(best)) {
            failures = 0;
        }
        return best;
    }
};

// --- nvidia-smi fallback (only when NVML cannot be loaded) ------------------
struct SmiSource {
    std::wstring exe;
    bool resolved = false;
    ULONGLONG retryAt = 0;
    int failures = 0;

    void Resolve() {
        resolved = true;
        wchar_t system[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(system, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            std::wstring candidate = std::wstring(system) + L"\\nvidia-smi.exe";
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                exe = std::move(candidate);
                return;
            }
        }
        wchar_t legacy[MAX_PATH] = {};
        if (ExpandEnvironmentStringsW(L"%ProgramW6432%\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe", legacy,
                MAX_PATH) > 0 &&
            GetFileAttributesW(legacy) != INVALID_FILE_ATTRIBUTES) {
            exe = legacy;
        }
    }

    double Run() const {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
            return kNaN;
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        // Inherit only the pipe's write end, not every inheritable dock handle.
        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<unsigned char> attributeBuffer(attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
        bool haveAttributes = attributeSize > 0 &&
            InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) != FALSE;
        if (haveAttributes &&
            !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &writePipe,
                sizeof(writePipe), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(attributes);
            haveAttributes = false;
        }
        if (!haveAttributes) {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return kNaN;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdOutput = writePipe;
        startup.StartupInfo.hStdError = writePipe;
        startup.lpAttributeList = attributes;
        std::wstring command =
            L"\"" + exe + L"\" --query-gpu=temperature.gpu --format=csv,noheader,nounits";
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
            &startup.StartupInfo, &process);
        CloseHandle(writePipe);
        DeleteProcThreadAttributeList(attributes);
        if (!started) {
            CloseHandle(readPipe);
            return kNaN;
        }
        if (WaitForSingleObject(process.hProcess, kSmiTimeoutMs) != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
        }
        std::string output;
        char buffer[256];
        DWORD available = 0;
        while (output.size() < 4096 &&
            PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            if (!ReadFile(readPipe, buffer, std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer))), &got, nullptr) ||
                got == 0) {
                break;
            }
            output.append(buffer, got);
        }
        CloseHandle(readPipe);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        double best = kNaN;
        const char* cursor = output.c_str();
        while (*cursor != '\0') {
            char* end = nullptr;
            const double value = std::strtod(cursor, &end);
            if (end == cursor) {
                ++cursor;
                continue;
            }
            if (Plausible(value)) {
                best = std::isfinite(best) ? std::max(best, value) : value;
            }
            cursor = end;
        }
        return best;
    }

    double Read(ULONGLONG now) {
        if (!resolved) {
            Resolve();
        }
        if (exe.empty() || now < retryAt) {
            return kNaN;
        }
        const double value = Run();
        if (!Plausible(value)) {
            if (++failures >= 3) {
                failures = 0;
                retryAt = now + kSmiRetryMs;
            }
            return kNaN;
        }
        failures = 0;
        return value;
    }
};

// --- ACPI thermal zone -----------------------------------------------------
struct AcpiTrust {
    int first = -1;
    bool moved = false;
    bool Accept(int deciKelvin) noexcept {
        if (first < 0) {
            first = deciKelvin;
        } else if (deciKelvin != first) {
            moved = true;
        }
        return moved || deciKelvin != kAcpiPlaceholderDeciKelvin;
    }
};

double DeciKelvinToCelsius(int deciKelvin) noexcept {
    const double celsius = static_cast<double>(deciKelvin) / 10.0 - 273.15;
    return Plausible(celsius) ? celsius : kNaN;
}

// root\WMI MSAcpi_ThermalZoneTemperature: tenths of a Kelvin. Usually
// admin-only; access denied disables it for this session.
struct AcpiWmiSource {
    ComPtr<IWbemServices> services;
    bool disabled = false;
    int failures = 0;
    AcpiTrust trust;

    double Read() {
        if (disabled) {
            return kNaN;
        }
        if (!services) {
            services = ConnectWmi(L"ROOT\\WMI");
            if (!services) {
                disabled = true;
                return kNaN;
            }
        }
        int best = -1;
        const HRESULT hr = ForEachRow(services.Get(),
            L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature", [&](IWbemClassObject* row) {
                const double value = NumberProp(row, L"CurrentTemperature");
                if (std::isfinite(value) && value > 0.0) {
                    best = std::max(best, static_cast<int>(std::lround(value)));
                }
            });
        if (FAILED(hr)) {
            if (hr == static_cast<HRESULT>(WBEM_E_ACCESS_DENIED) ||
                hr == static_cast<HRESULT>(WBEM_E_INVALID_CLASS) ||
                hr == static_cast<HRESULT>(WBEM_E_NOT_SUPPORTED) ||
                hr == static_cast<HRESULT>(WBEM_E_NOT_FOUND) || ++failures >= 3) {
                disabled = true;
                services.Reset();
            }
            return kNaN;
        }
        failures = 0;
        if (best <= 0 || !trust.Accept(best)) {
            return kNaN;
        }
        return DeciKelvinToCelsius(best);
    }
};

// PDH "Thermal Zone Information" — same ACPI zones, readable without admin.
struct PdhSource {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool highPrecision = true;  // tenths of K; plain "Temperature" is whole K
    bool disabled = false;
    AcpiTrust trust;

    bool Open() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            return false;
        }
        if (PdhAddEnglishCounterW(query, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0,
                &counter) == ERROR_SUCCESS) {
            highPrecision = true;
            return true;
        }
        if (PdhAddEnglishCounterW(query, L"\\Thermal Zone Information(*)\\Temperature", 0, &counter) ==
            ERROR_SUCCESS) {
            highPrecision = false;
            return true;
        }
        PdhCloseQuery(query);
        query = nullptr;
        return false;
    }

    double Read() {
        if (disabled) {
            return kNaN;
        }
        if (query == nullptr && !Open()) {
            disabled = true;
            return kNaN;
        }
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
            return kNaN;
        }
        DWORD size = 0;
        DWORD count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOSCALE, &size, &count, nullptr) !=
                static_cast<PDH_STATUS>(PDH_MORE_DATA) ||
            size == 0) {
            return kNaN;
        }
        std::vector<unsigned char> buffer(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOSCALE, &size, &count, items) !=
            ERROR_SUCCESS) {
            return kNaN;
        }
        int best = -1;
        for (DWORD index = 0; index < count; ++index) {
            const PDH_FMT_COUNTERVALUE& value = items[index].FmtValue;
            if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) {
                continue;
            }
            const double raw = highPrecision ? value.doubleValue : value.doubleValue * 10.0;
            if (std::isfinite(raw) && raw > 0.0) {
                best = std::max(best, static_cast<int>(std::lround(raw)));
            }
        }
        if (best <= 0 || !trust.Accept(best)) {
            return kNaN;
        }
        return DeciKelvinToCelsius(best);
    }
};

struct Sensors {
    HwmonSource hwmon;
    NvmlSource nvml;
    SmiSource smi;
    AcpiWmiSource acpi;
    PdhSource pdh;

    SystemTempsReading Poll() {
        const ULONGLONG now = GetTickCount64();
        SystemTempsReading reading;
        double hwCpu = kNaN;
        double hwGpu = kNaN;
        hwmon.Poll(now, hwCpu, hwGpu);

        double cpu = kNaN;
        if (Plausible(hwCpu)) {
            cpu = hwCpu;
            reading.cpuSource = hwmon.name;
        }
        if (!Plausible(cpu)) {
            cpu = acpi.Read();
            if (Plausible(cpu)) {
                reading.cpuSource = L"ACPI thermal zone (WMI)";
            }
        }
        if (!Plausible(cpu)) {
            cpu = pdh.Read();
            if (Plausible(cpu)) {
                reading.cpuSource = L"ACPI thermal zone (PDH)";
            }
        }

        double gpu = nvml.Read();
        if (Plausible(gpu)) {
            reading.gpuSource = L"NVML";
        } else if (Plausible(hwGpu)) {
            gpu = hwGpu;
            reading.gpuSource = hwmon.name;
        } else {
            gpu = smi.Read(now);
            if (Plausible(gpu)) {
                reading.gpuSource = L"nvidia-smi";
            }
        }

        reading.cpuC = Plausible(cpu) ? static_cast<int>(std::lround(cpu)) : -1;
        reading.gpuC = Plausible(gpu) ? static_cast<int>(std::lround(gpu)) : -1;
        reading.stamp = GetTickCount64();
        return reading;
    }
};

struct TempsBridge {
    std::mutex mutex;
    std::condition_variable wake;
    HWND notifyHwnd = nullptr;
    UINT notifyMsg = 0;
    ULONGLONG activeUntil = 0;
    bool started = false;
    bool published = false;
    SystemTempsReading latest;
};

TempsBridge& Bridge() {
    // Leaked on purpose: the detached worker may still be parked on the
    // condition variable while static destructors run at process exit.
    static TempsBridge* bridge = new TempsBridge();
    return *bridge;
}

void WorkerMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    // MTA so WMI proxies are usable from this thread without a message pump.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::lock_guard<std::mutex> lock(Bridge().mutex);
        Bridge().started = false;
        return;
    }
    Sensors sensors;
    TempsBridge& bridge = Bridge();
    std::wstring loggedCpu = L"?";
    std::wstring loggedGpu = L"?";
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(bridge.mutex);
            bridge.wake.wait(lock, [&] { return GetTickCount64() < bridge.activeUntil; });
        }
        SystemTempsReading reading = sensors.Poll();
        if (reading.cpuSource != loggedCpu || reading.gpuSource != loggedGpu) {
            loggedCpu = reading.cpuSource;
            loggedGpu = reading.gpuSource;
            const std::wstring line = L"[Dock] temps: CPU via " +
                (loggedCpu.empty() ? std::wstring(L"(none)") : loggedCpu) + L", GPU via " +
                (loggedGpu.empty() ? std::wstring(L"(none)") : loggedGpu) + L"\n";
            OutputDebugStringW(line.c_str());
        }
        HWND hwnd = nullptr;
        UINT msg = 0;
        {
            std::lock_guard<std::mutex> lock(bridge.mutex);
            const bool changed = !bridge.published || bridge.latest.cpuC != reading.cpuC ||
                bridge.latest.gpuC != reading.gpuC;
            bridge.latest = std::move(reading);
            bridge.published = true;
            if (changed) {
                hwnd = bridge.notifyHwnd;
                msg = bridge.notifyMsg;
            }
        }
        if (hwnd != nullptr && msg != 0) {
            PostMessageW(hwnd, msg, 0, 0);
        }
        std::unique_lock<std::mutex> lock(bridge.mutex);
        bridge.wake.wait_for(lock, std::chrono::milliseconds(kPollIntervalMs));
    }
}

}  // namespace

void RequestSystemTemps(HWND notifyHwnd, UINT notifyMsg) noexcept {
    try {
        TempsBridge& bridge = Bridge();
        bool spawn = false;
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(bridge.mutex);
            const ULONGLONG now = GetTickCount64();
            bridge.notifyHwnd = notifyHwnd;
            bridge.notifyMsg = notifyMsg;
            // Only nudge an idle worker; an active one keeps its 2 s cadence.
            wake = now >= bridge.activeUntil;
            bridge.activeUntil = now + kKeepAliveMs;
            if (!bridge.started) {
                bridge.started = true;
                spawn = true;
            }
        }
        if (spawn) {
            try {
                std::thread(WorkerMain).detach();
            } catch (...) {
                std::lock_guard<std::mutex> lock(bridge.mutex);
                bridge.started = false;
            }
        }
        if (wake) {
            bridge.wake.notify_one();
        }
    } catch (...) {
    }
}

bool LatestSystemTemps(SystemTempsReading& out) {
    TempsBridge& bridge = Bridge();
    std::lock_guard<std::mutex> lock(bridge.mutex);
    if (!bridge.published) {
        return false;
    }
    out = bridge.latest;
    return true;
}
