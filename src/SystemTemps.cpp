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

bool PlausibleTemp(double celsius) noexcept {
    return std::isfinite(celsius) && celsius >= 5.0 && celsius <= 125.0;
}

bool PlausibleWatts(double watts) noexcept {
    return std::isfinite(watts) && watts >= 0.5 && watts <= 600.0;
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
// Both publish Sensor while running. LHM needs its kernel driver (WinRing0 /
// similar) for Intel package/die MSR and RAPL; that driver is installed once
// by LHM itself (often with a single admin elevation). Dock only reads the
// WMI namespace — it never loads WinRing0 or asks for admin at dock launch.
struct HwmonSource {
    ComPtr<IWbemServices> services;
    std::wstring name;
    ULONGLONG retryAt = 0;

    static int CpuTempScore(const std::wstring& label) {
        if (Has(label, L"distance")) {
            return 0;  // "Distance to TjMax" is a Temperature-typed delta
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

    static int GpuTempScore(const std::wstring& label) {
        if (label == L"gpu core") {
            return 3;
        }
        if (Has(label, L"hot spot") || Has(label, L"memory") || Has(label, L"junction")) {
            return 0;
        }
        return 1;
    }

    static int CpuPowerScore(const std::wstring& label) {
        if (label == L"cpu package" || label == L"package") {
            return 4;
        }
        if (Has(label, L"package")) {
            return 3;
        }
        if (label == L"cpu cores" || Has(label, L"cores power")) {
            return 1;
        }
        return 0;
    }

    static int GpuPowerScore(const std::wstring& label) {
        if (label == L"gpu package" || label == L"gpu power" || label == L"power") {
            return 3;
        }
        if (Has(label, L"board") || Has(label, L"total")) {
            return 4;
        }
        if (Has(label, L"gpu")) {
            return 2;
        }
        return 0;
    }

    void Poll(ULONGLONG now, double& cpuTemp, double& gpuTemp, double& cpuPower, double& gpuPower) {
        cpuTemp = kNaN;
        gpuTemp = kNaN;
        cpuPower = kNaN;
        gpuPower = kNaN;
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
        int cpuTempBest = 0;
        int gpuTempBest = 0;
        int cpuPowerBest = 0;
        int gpuPowerBest = 0;
        const HRESULT hr = ForEachRow(services.Get(),
            L"SELECT Identifier, Name, Value, SensorType FROM Sensor "
            L"WHERE SensorType = 'Temperature' OR SensorType = 'Power'",
            [&](IWbemClassObject* row) {
                const double value = NumberProp(row, L"Value");
                const std::wstring type = Lower(StringProp(row, L"SensorType"));
                const std::wstring id = Lower(StringProp(row, L"Identifier"));
                const std::wstring label = Lower(StringProp(row, L"Name"));
                if (type == L"temperature") {
                    if (!PlausibleTemp(value)) {
                        return;
                    }
                    if (Has(id, L"cpu/")) {
                        const int score = CpuTempScore(label);
                        if (score > cpuTempBest ||
                            (score == cpuTempBest && score > 0 && value > cpuTemp)) {
                            cpuTempBest = score;
                            cpuTemp = value;
                        }
                    } else if (Has(id, L"gpu")) {
                        const int score = GpuTempScore(label);
                        if (score > gpuTempBest ||
                            (score == gpuTempBest && score > 0 && value > gpuTemp)) {
                            gpuTempBest = score;
                            gpuTemp = value;
                        }
                    }
                } else if (type == L"power") {
                    if (!PlausibleWatts(value)) {
                        return;
                    }
                    if (Has(id, L"cpu/")) {
                        const int score = CpuPowerScore(label);
                        if (score > cpuPowerBest ||
                            (score == cpuPowerBest && score > 0 &&
                                (!std::isfinite(cpuPower) || value > cpuPower))) {
                            cpuPowerBest = score;
                            cpuPower = value;
                        }
                    } else if (Has(id, L"gpu")) {
                        const int score = GpuPowerScore(label);
                        if (score > gpuPowerBest ||
                            (score == gpuPowerBest && score > 0 &&
                                (!std::isfinite(gpuPower) || value > gpuPower))) {
                            gpuPowerBest = score;
                            gpuPower = value;
                        }
                    }
                }
            });
        if (FAILED(hr)) {
            services.Reset();
            name.clear();
            retryAt = now + kHwmonRetryMs;
            cpuTemp = kNaN;
            gpuTemp = kNaN;
            cpuPower = kNaN;
            gpuPower = kNaN;
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
    using PowerFn = int (*)(Device, unsigned int*);

    HMODULE module = nullptr;
    TempFn temperature = nullptr;
    PowerFn powerUsage = nullptr;
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
        powerUsage = reinterpret_cast<PowerFn>(GetProcAddress(module, "nvmlDeviceGetPowerUsage"));
        unsigned int deviceCount = 0;
        // Temperature is required; power is best-effort (unsupported on some GPUs).
        if (init == nullptr || count == nullptr || handle == nullptr || temperature == nullptr ||
            init() != 0 || count(&deviceCount) != 0) {
            temperature = nullptr;
            powerUsage = nullptr;
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

    double ReadTemp() {
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
            if (temperature(device, 0, &value) == 0 && PlausibleTemp(static_cast<double>(value))) {
                best = std::isfinite(best) ? std::max(best, static_cast<double>(value))
                                           : static_cast<double>(value);
            }
        }
        if (!std::isfinite(best) && ++failures >= 5) {
            devices.clear();
        } else if (std::isfinite(best)) {
            failures = 0;
        }
        return best;
    }

    // Watts for the entire board on pre-Turing GPUs (GTX 1070 Ti / Pascal):
    // nvmlDeviceGetPowerUsage matches nvidia-smi power.draw ("entire board").
    // Turing+ may report GPU-only here; MODULE-scope APIs are not in this
    // driver build, so board total via GetPowerUsage is what we ship.
    double ReadPower() {
        if (!tried) {
            Load();
        }
        if (powerUsage == nullptr || devices.empty()) {
            return kNaN;
        }
        double best = kNaN;
        for (Device device : devices) {
            unsigned int milliwatts = 0;
            if (powerUsage(device, &milliwatts) == 0) {
                const double watts = static_cast<double>(milliwatts) / 1000.0;
                if (PlausibleWatts(watts)) {
                    best = std::isfinite(best) ? std::max(best, watts) : watts;
                }
            }
        }
        return best;
    }
};

// --- nvidia-smi fallback (temp and/or power when NVML cannot supply them) ---
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

    std::string RunQuery(const wchar_t* query) const {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
            return {};
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

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

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdOutput = writePipe;
        startup.StartupInfo.hStdError = writePipe;
        if (haveAttributes) {
            startup.lpAttributeList = attributes;
        }

        std::wstring command = L"\"" + exe + L"\" --query-gpu=" + query +
            L" --format=csv,noheader,nounits";
        PROCESS_INFORMATION process{};
        const DWORD flags = CREATE_NO_WINDOW |
            (haveAttributes ? EXTENDED_STARTUPINFO_PRESENT : 0);
        std::vector<wchar_t> cmdline(command.begin(), command.end());
        cmdline.push_back(L'\0');
        const BOOL created = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, flags,
            nullptr, nullptr, &startup.StartupInfo, &process);
        CloseHandle(writePipe);
        if (haveAttributes) {
            DeleteProcThreadAttributeList(attributes);
        }
        if (!created) {
            CloseHandle(readPipe);
            return {};
        }

        std::string output;
        char buffer[256];
        const ULONGLONG deadline = GetTickCount64() + kSmiTimeoutMs;
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) {
                break;
            }
            if (available > 0) {
                DWORD read = 0;
                if (!ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
                    break;
                }
                output.append(buffer, buffer + read);
                continue;
            }
            const DWORD wait = WaitForSingleObject(process.hProcess, 50);
            if (wait == WAIT_OBJECT_0) {
                DWORD read = 0;
                while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
                    output.append(buffer, buffer + read);
                }
                break;
            }
            if (GetTickCount64() >= deadline) {
                TerminateProcess(process.hProcess, 1);
                break;
            }
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(readPipe);
        return output;
    }

    static double ParseFirstNumber(const std::string& output) {
        double best = kNaN;
        const char* cursor = output.c_str();
        while (*cursor != '\0') {
            char* end = nullptr;
            const double value = std::strtod(cursor, &end);
            if (end == cursor) {
                ++cursor;
                continue;
            }
            best = value;
            break;
        }
        return best;
    }

    double ReadTemp(ULONGLONG now) {
        if (!resolved) {
            Resolve();
        }
        if (exe.empty() || now < retryAt) {
            return kNaN;
        }
        const double value = ParseFirstNumber(RunQuery(L"temperature.gpu"));
        if (!PlausibleTemp(value)) {
            if (++failures >= 3) {
                failures = 0;
                retryAt = now + kSmiRetryMs;
            }
            return kNaN;
        }
        failures = 0;
        return value;
    }

    double ReadPower(ULONGLONG now) {
        if (!resolved) {
            Resolve();
        }
        if (exe.empty() || now < retryAt) {
            return kNaN;
        }
        // power.draw = entire board (nvidia-smi docs); nounits → watts.
        const double value = ParseFirstNumber(RunQuery(L"power.draw"));
        if (!PlausibleWatts(value)) {
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

// --- Core Temp shared memory ------------------------------------------------
// https://www.alcpu.com/CoreTemp/developers.html -- present only while Core Temp
// is running. Structure is 4-byte aligned. We take the hottest core as a stand-in
// for package when Core Temp does not publish a separate package field.
#pragma pack(push, 4)
struct CoreTempSharedDataEx {
    unsigned int uiLoad[256];
    unsigned int uiTjMax[128];
    unsigned int uiCoreCnt;
    unsigned int uiCPUCnt;
    float fTemp[256];
    float fVID;
    float fCPUSpeed;
    float fFSBSpeed;
    float fMultiplier;
    char sCPUName[100];
    unsigned char ucFahrenheit;
    unsigned char ucDeltaToTjMax;
    unsigned char ucTdpSupported;
    unsigned char ucPowerSupported;
    unsigned int uiStructVersion;
    unsigned int uiTdp[128];
    float fPower[128];
    float fMultipliers[256];
};
#pragma pack(pop)

struct CoreTempSource {
    ULONGLONG retryAt = 0;

    double Read(ULONGLONG now) {
        if (now < retryAt) {
            return kNaN;
        }
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"CoreTempMappingObject");
        if (mapping == nullptr) {
            mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\CoreTempMappingObject");
        }
        if (mapping == nullptr) {
            retryAt = now + kHwmonRetryMs;
            return kNaN;
        }
        void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(CoreTempSharedDataEx));
        double best = kNaN;
        if (view != nullptr) {
            const auto* data = static_cast<const CoreTempSharedDataEx*>(view);
            const unsigned int cores = std::min(data->uiCoreCnt, 256U);
            const unsigned int cpus = std::max(1U, std::min(data->uiCPUCnt, 128U));
            for (unsigned int index = 0; index < cores; ++index) {
                double celsius = static_cast<double>(data->fTemp[index]);
                if (data->ucDeltaToTjMax) {
                    const unsigned int cpu = std::min(index / std::max(1U, cores / cpus), cpus - 1U);
                    celsius = static_cast<double>(data->uiTjMax[cpu]) - celsius;
                }
                if (data->ucFahrenheit) {
                    celsius = (celsius - 32.0) * (5.0 / 9.0);
                }
                if (PlausibleTemp(celsius)) {
                    best = std::isfinite(best) ? std::max(best, celsius) : celsius;
                }
            }
            // Optional package power when Core Temp exposes it (struct v2+).
            UnmapViewOfFile(view);
        }
        CloseHandle(mapping);
        if (!std::isfinite(best)) {
            retryAt = now + kHwmonRetryMs;
        }
        return best;
    }

    double ReadPower(ULONGLONG /*now*/) {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"CoreTempMappingObject");
        if (mapping == nullptr) {
            mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\CoreTempMappingObject");
        }
        if (mapping == nullptr) {
            return kNaN;
        }
        void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(CoreTempSharedDataEx));
        double best = kNaN;
        if (view != nullptr) {
            const auto* data = static_cast<const CoreTempSharedDataEx*>(view);
            if (data->uiStructVersion >= 2 && data->ucPowerSupported) {
                const unsigned int cpus = std::min(data->uiCPUCnt, 128U);
                for (unsigned int index = 0; index < cpus; ++index) {
                    const double watts = static_cast<double>(data->fPower[index]);
                    if (PlausibleWatts(watts)) {
                        best = std::isfinite(best) ? std::max(best, watts) : watts;
                    }
                }
            }
            UnmapViewOfFile(view);
        }
        CloseHandle(mapping);
        return best;
    }
};

// --- HWiNFO shared memory ---------------------------------------------------
// Settings -> HWiNFO Gadget / Shared Memory Support must be enabled. Layout
// matches HWiNFO's published HWiSENS SM2 header (pack 1). Version 2 strings are
// UTF-8; version 1 are ANSI -- both fit in the same char buffers.
#pragma pack(push, 1)
struct HwiHeader {
    DWORD signature;  // 'HWiS' when active
    DWORD version;
    DWORD revision;
    __int64 pollTime;
    DWORD sensorOffset;
    DWORD sensorSize;
    DWORD sensorCount;
    DWORD readingOffset;
    DWORD readingSize;
    DWORD readingCount;
    DWORD pollingPeriod;
};

struct HwiReading {
    DWORD type;  // 1 = Temperature, 5 = Power
    DWORD sensorIndex;
    DWORD readingId;
    char labelOrig[128];
    char labelUser[128];
    char unit[16];
    double value;
    double valueMin;
    double valueMax;
    double valueAvg;
};
#pragma pack(pop)

struct HwinfoSource {
    ULONGLONG retryAt = 0;

    static std::wstring NarrowToWide(const char* text, DWORD version) {
        if (text == nullptr || text[0] == '\0') {
            return {};
        }
        const UINT cp = version >= 2 ? CP_UTF8 : CP_ACP;
        const int needed = MultiByteToWideChar(cp, 0, text, -1, nullptr, 0);
        if (needed <= 1) {
            return {};
        }
        std::wstring out(static_cast<size_t>(needed - 1), L'\0');
        MultiByteToWideChar(cp, 0, text, -1, out.data(), needed);
        return out;
    }

    static int CpuTempScore(const std::wstring& label) {
        if (Has(label, L"distance") || Has(label, L"tjmax")) {
            return 0;
        }
        if (label == L"cpu package" || label == L"cpu (tctl/tdie)" || label == L"cpu tdie" ||
            label == L"core (tctl/tdie)" || label == L"core (tdie)") {
            return 4;
        }
        if (label == L"cpu (tctl)" || label == L"core (tctl)" || label == L"cpu") {
            return 3;
        }
        if (Has(label, L"package") && Has(label, L"cpu")) {
            return 3;
        }
        if (label == L"core max" || Has(label, L"core average")) {
            return 2;
        }
        return Has(label, L"core") ? 1 : 0;
    }

    static int CpuPowerScore(const std::wstring& label) {
        if (label == L"cpu package" || label == L"cpu package power" || label == L"package power") {
            return 4;
        }
        if (Has(label, L"package") && Has(label, L"cpu")) {
            return 3;
        }
        if (Has(label, L"package")) {
            return 2;
        }
        return 0;
    }

    void Poll(ULONGLONG now, double& cpuTemp, double& cpuPower) {
        cpuTemp = kNaN;
        cpuPower = kNaN;
        if (now < retryAt) {
            return;
        }
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
        if (mapping == nullptr) {
            mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"HWiNFO_SENS_SM2");
        }
        if (mapping == nullptr) {
            retryAt = now + kHwmonRetryMs;
            return;
        }
        // Map header first, then full section sizes from the header fields.
        void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (view == nullptr) {
            CloseHandle(mapping);
            retryAt = now + kHwmonRetryMs;
            return;
        }
        const auto* header = static_cast<const HwiHeader*>(view);
        constexpr DWORD kSig = 0x53695748U;  // 'HWiS' little-endian
        if (header->signature != kSig || header->readingSize < sizeof(HwiReading) ||
            header->readingCount == 0 || header->readingCount > 100000) {
            UnmapViewOfFile(view);
            CloseHandle(mapping);
            retryAt = now + kHwmonRetryMs;
            return;
        }
        const auto* base = static_cast<const unsigned char*>(view);
        int tempBest = 0;
        int powerBest = 0;
        for (DWORD index = 0; index < header->readingCount; ++index) {
            const auto* reading = reinterpret_cast<const HwiReading*>(
                base + header->readingOffset + static_cast<size_t>(header->readingSize) * index);
            const std::wstring label = Lower(NarrowToWide(reading->labelOrig, header->version));
            if (reading->type == 1) {  // Temperature
                if (!PlausibleTemp(reading->value)) {
                    continue;
                }
                const int score = CpuTempScore(label);
                if (score > tempBest || (score == tempBest && score > 0 && reading->value > cpuTemp)) {
                    tempBest = score;
                    cpuTemp = reading->value;
                }
            } else if (reading->type == 5) {  // Power
                if (!PlausibleWatts(reading->value)) {
                    continue;
                }
                const int score = CpuPowerScore(label);
                if (score > powerBest ||
                    (score == powerBest && score > 0 &&
                        (!std::isfinite(cpuPower) || reading->value > cpuPower))) {
                    powerBest = score;
                    cpuPower = reading->value;
                }
            }
        }
        if (tempBest <= 0) {
            cpuTemp = kNaN;
        }
        if (powerBest <= 0) {
            cpuPower = kNaN;
        }
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        if (!std::isfinite(cpuTemp) && !std::isfinite(cpuPower)) {
            retryAt = now + kHwmonRetryMs;
        }
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
    return PlausibleTemp(celsius) ? celsius : kNaN;
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
struct PdhThermalSource {
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

// PDH Energy Meter RAPL package power — undelevated on modern Windows when
// the kernel exposes RAPL_Package*_PKG. Values are milliwatts.
struct RaplPdhSource {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool disabled = false;
    bool primed = false;

    bool Open() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            return false;
        }
        if (PdhAddEnglishCounterW(query, L"\\Energy Meter(*)\\Power", 0, &counter) != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            counter = nullptr;
            return false;
        }
        return true;
    }

    static int InstanceScore(const wchar_t* name) {
        if (name == nullptr) {
            return 0;
        }
        const std::wstring lower = Lower(name);
        if (Has(lower, L"_total") || Has(lower, L"dram") || Has(lower, L"pp1")) {
            return 0;
        }
        if (Has(lower, L"pkg")) {
            return 4;
        }
        if (Has(lower, L"pp0") || Has(lower, L"core")) {
            return 1;
        }
        if (Has(lower, L"rapl") || Has(lower, L"package")) {
            return 2;
        }
        return 0;
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
        // First collect after Open often has no valid data yet.
        if (!primed) {
            primed = true;
            PdhCollectQueryData(query);
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
        int bestScore = 0;
        double bestMw = kNaN;
        for (DWORD index = 0; index < count; ++index) {
            const PDH_FMT_COUNTERVALUE& value = items[index].FmtValue;
            if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) {
                continue;
            }
            const int score = InstanceScore(items[index].szName);
            if (score <= 0 || !std::isfinite(value.doubleValue) || value.doubleValue <= 0.0) {
                continue;
            }
            if (score > bestScore || (score == bestScore && value.doubleValue > bestMw)) {
                bestScore = score;
                bestMw = value.doubleValue;
            }
        }
        if (bestScore <= 0 || !std::isfinite(bestMw)) {
            return kNaN;
        }
        const double watts = bestMw / 1000.0;
        return PlausibleWatts(watts) ? watts : kNaN;
    }
};

struct Sensors {
    HwmonSource hwmon;
    CoreTempSource coreTemp;
    HwinfoSource hwinfo;
    NvmlSource nvml;
    SmiSource smi;
    AcpiWmiSource acpi;
    PdhThermalSource pdhThermal;
    RaplPdhSource rapl;

    SystemTempsReading Poll() {
        const ULONGLONG now = GetTickCount64();
        SystemTempsReading reading;
        double hwCpuTemp = kNaN;
        double hwGpuTemp = kNaN;
        double hwCpuPower = kNaN;
        double hwGpuPower = kNaN;
        hwmon.Poll(now, hwCpuTemp, hwGpuTemp, hwCpuPower, hwGpuPower);

        double cpuTemp = kNaN;
        if (PlausibleTemp(hwCpuTemp)) {
            cpuTemp = hwCpuTemp;
            reading.cpuSource = hwmon.name;
        }
        if (!PlausibleTemp(cpuTemp)) {
            cpuTemp = coreTemp.Read(now);
            if (PlausibleTemp(cpuTemp)) {
                reading.cpuSource = L"Core Temp (shared memory)";
            }
        }
        double hwiCpuTemp = kNaN;
        double hwiCpuPower = kNaN;
        if (!PlausibleTemp(cpuTemp) || !PlausibleWatts(hwCpuPower)) {
            hwinfo.Poll(now, hwiCpuTemp, hwiCpuPower);
        }
        if (!PlausibleTemp(cpuTemp) && PlausibleTemp(hwiCpuTemp)) {
            cpuTemp = hwiCpuTemp;
            reading.cpuSource = L"HWiNFO (shared memory)";
        }
        // ACPI / PDH thermal zones: often the fixed 27.85 C Z390 placeholder.
        // Never prefer them over a real die sensor; still try when nothing else.
        if (!PlausibleTemp(cpuTemp)) {
            cpuTemp = acpi.Read();
            if (PlausibleTemp(cpuTemp)) {
                reading.cpuSource = L"ACPI thermal zone (WMI)";
            }
        }
        if (!PlausibleTemp(cpuTemp)) {
            cpuTemp = pdhThermal.Read();
            if (PlausibleTemp(cpuTemp)) {
                reading.cpuSource = L"ACPI thermal zone (PDH)";
            }
        }

        double cpuPower = kNaN;
        if (PlausibleWatts(hwCpuPower)) {
            cpuPower = hwCpuPower;
            reading.cpuPowerSource = hwmon.name;
        }
        if (!PlausibleWatts(cpuPower)) {
            const double ctPower = coreTemp.ReadPower(now);
            if (PlausibleWatts(ctPower)) {
                cpuPower = ctPower;
                reading.cpuPowerSource = L"Core Temp (shared memory)";
            }
        }
        if (!PlausibleWatts(cpuPower) && PlausibleWatts(hwiCpuPower)) {
            cpuPower = hwiCpuPower;
            reading.cpuPowerSource = L"HWiNFO (shared memory)";
        }
        if (!PlausibleWatts(cpuPower)) {
            cpuPower = rapl.Read();
            if (PlausibleWatts(cpuPower)) {
                reading.cpuPowerSource = L"RAPL Energy Meter (PDH)";
            }
        }

        double gpuTemp = nvml.ReadTemp();
        if (PlausibleTemp(gpuTemp)) {
            reading.gpuSource = L"NVML";
        } else if (PlausibleTemp(hwGpuTemp)) {
            gpuTemp = hwGpuTemp;
            reading.gpuSource = hwmon.name;
        } else {
            gpuTemp = smi.ReadTemp(now);
            if (PlausibleTemp(gpuTemp)) {
                reading.gpuSource = L"nvidia-smi";
            }
        }

        double gpuPower = nvml.ReadPower();
        if (PlausibleWatts(gpuPower)) {
            reading.gpuPowerSource = L"NVML (board power.draw)";
        } else if (PlausibleWatts(hwGpuPower)) {
            gpuPower = hwGpuPower;
            reading.gpuPowerSource = hwmon.name;
        } else {
            gpuPower = smi.ReadPower(now);
            if (PlausibleWatts(gpuPower)) {
                reading.gpuPowerSource = L"nvidia-smi power.draw";
            }
        }

        reading.cpuC = PlausibleTemp(cpuTemp) ? static_cast<int>(std::lround(cpuTemp)) : -1;
        reading.gpuC = PlausibleTemp(gpuTemp) ? static_cast<int>(std::lround(gpuTemp)) : -1;
        reading.cpuW = PlausibleWatts(cpuPower) ? static_cast<int>(std::lround(cpuPower)) : -1;
        reading.gpuW = PlausibleWatts(gpuPower) ? static_cast<int>(std::lround(gpuPower)) : -1;
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
    std::wstring loggedCpuW = L"?";
    std::wstring loggedGpuW = L"?";
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(bridge.mutex);
            bridge.wake.wait(lock, [&] { return GetTickCount64() < bridge.activeUntil; });
        }
        SystemTempsReading reading = sensors.Poll();
        if (reading.cpuSource != loggedCpu || reading.gpuSource != loggedGpu ||
            reading.cpuPowerSource != loggedCpuW || reading.gpuPowerSource != loggedGpuW) {
            loggedCpu = reading.cpuSource;
            loggedGpu = reading.gpuSource;
            loggedCpuW = reading.cpuPowerSource;
            loggedGpuW = reading.gpuPowerSource;
            const std::wstring line = L"[Dock] temps: CPU via " +
                (loggedCpu.empty() ? std::wstring(L"(none)") : loggedCpu) + L", GPU via " +
                (loggedGpu.empty() ? std::wstring(L"(none)") : loggedGpu) + L"; power CPU via " +
                (loggedCpuW.empty() ? std::wstring(L"(none)") : loggedCpuW) + L", GPU via " +
                (loggedGpuW.empty() ? std::wstring(L"(none)") : loggedGpuW) + L"\n";
            OutputDebugStringW(line.c_str());
        }
        HWND hwnd = nullptr;
        UINT msg = 0;
        {
            std::lock_guard<std::mutex> lock(bridge.mutex);
            const bool changed = !bridge.published || bridge.latest.cpuC != reading.cpuC ||
                bridge.latest.gpuC != reading.gpuC || bridge.latest.cpuW != reading.cpuW ||
                bridge.latest.gpuW != reading.gpuW;
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
