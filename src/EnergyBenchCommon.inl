// Shared by the energy benchmark (EnergyBench.cpp) and the power profiler
// (PowerProfiler.cpp). Include inside an anonymous namespace, after the
// Windows, PDH, power, SetupAPI and standard headers EnergyBench.cpp includes:
// each file gets its own copy, abort flag included.

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---- small helpers ----------------------------------------------------------------------

double Now() {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / frequency;
}

std::atomic<uint64_t> g_sink{0};

// Fixed integer work (xorshift); the result is kept so it cannot be optimised away.
uint64_t Spin(uint64_t iterations, uint64_t seed) {
    uint64_t x = seed | 1U;
    for (uint64_t i = 0; i < iterations; ++i) {
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
    }
    g_sink.fetch_xor(x, std::memory_order_relaxed);
    return x;
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(size, 0)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr,
            nullptr);
    }
    return out;
}

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return text;
}

std::string GuidText(const GUID& guid) {
    char text[40]{};
    std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
        guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

std::atomic<bool> g_abort{false};

// Sleeps in short slices; false when the run was aborted.
bool SleepFor(double seconds) {
    const double until = Now() + seconds;
    while (!g_abort.load()) {
        const double left = until - Now();
        if (left <= 0.0) {
            return true;
        }
        Sleep(static_cast<DWORD>(std::clamp(left * 1000.0, 1.0, 100.0)));
    }
    return false;
}

// ---- power plans ------------------------------------------------------------------------

// {a5cc14c9-7d82-4ffc-a698-225beed1e73f} "Hoverdock Automatic" (AutoPower.cpp).
const GUID kAutoPlan = {0xa5cc14c9, 0x7d82, 0x4ffc, {0xa6, 0x98, 0x22, 0x5b, 0xee, 0xd1, 0xe7, 0x3f}};
const GUID kBalancedPlan = {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
const GUID kPowerSaverPlan = {0xa1841308, 0x3541, 0x4fab, {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};

bool ActivePlan(GUID& plan) {
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS || active == nullptr) {
        return false;
    }
    plan = *active;
    LocalFree(active);
    return true;
}

bool SetPlan(const GUID& plan) {
    return PowerSetActiveScheme(nullptr, &plan) == ERROR_SUCCESS;
}

bool PlanExists(const GUID& plan) {
    DWORD size = 0;
    return PowerReadFriendlyName(nullptr, &plan, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

std::string PlanName(const GUID& plan) {
    DWORD size = 0;
    if (PowerReadFriendlyName(nullptr, &plan, nullptr, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) {
        return GuidText(plan);
    }
    std::vector<UCHAR> buffer(size + sizeof(wchar_t), 0);
    if (PowerReadFriendlyName(nullptr, &plan, nullptr, nullptr, buffer.data(), &size) != ERROR_SUCCESS) {
        return GuidText(plan);
    }
    return Utf8(reinterpret_cast<const wchar_t*>(buffer.data()));
}


std::string RegistryText(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t text[256]{};
    DWORD size = sizeof(text);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS) {
        return {};
    }
    std::string out = Utf8(text);
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// ---- energy meters ----------------------------------------------------------------------

// GUID_DEVICE_ENERGY_METER and the EMI IOCTLs (emi.h), spelled out so the
// build does not depend on the SDK's NTDDI gating of that header.
const GUID kEnergyMeterInterface = {0x45bd8344, 0x7ed6, 0x49cf, {0xa4, 0x40, 0xc2, 0x76, 0xc9, 0x33, 0xb0, 0x53}};
constexpr DWORD kIoctlEmiGetVersion = CTL_CODE(FILE_DEVICE_UNKNOWN, 0, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr DWORD kIoctlEmiGetMetadataSize = CTL_CODE(FILE_DEVICE_UNKNOWN, 1, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr DWORD kIoctlEmiGetMetadata = CTL_CODE(FILE_DEVICE_UNKNOWN, 2, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr DWORD kIoctlEmiGetMeasurement = CTL_CODE(FILE_DEVICE_UNKNOWN, 3, METHOD_BUFFERED, FILE_READ_ACCESS);
// EMI_METADATA_V1: unit (4) + OEM (32) + model (32) + revision (2) + name size (2), then the name.
constexpr size_t kEmiV1NameSizeOffset = 70;
constexpr size_t kEmiV1NameOffset = 72;
// EMI_METADATA_V2: OEM (32) + model (32) + revision (2) + channel count (2), then channels of
// unit (4) + name size (2) + name, packed back to back.
constexpr size_t kEmiV2CountOffset = 66;
constexpr size_t kEmiV2ChannelsOffset = 68;
// One EMI_CHANNEL_MEASUREMENT_DATA: AbsoluteEnergy (pWh) + AbsoluteTime (100 ns).
constexpr size_t kEmiMeasurementSize = 16;
constexpr double kJoulesPerPicowattHour = 3.6e-9;

template <typename T>
T ReadAt(const std::vector<BYTE>& bytes, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= bytes.size()) {
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
    }
    return value;
}

std::wstring WideAt(const std::vector<BYTE>& bytes, size_t offset, size_t byteCount) {
    std::wstring out;
    for (size_t i = 0; i + 1 < byteCount && offset + i + 1 < bytes.size(); i += 2) {
        const auto c = ReadAt<wchar_t>(bytes, offset + i);
        if (c == L'\0') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

class EmiMeter {
public:
    EmiMeter() = default;
    ~EmiMeter() {
        for (const Device& device : m_devices) {
            CloseHandle(device.handle);
        }
    }
    EmiMeter(const EmiMeter&) = delete;
    EmiMeter& operator=(const EmiMeter&) = delete;

    // Opens every energy meter device; error explains an empty result.
    bool Open(std::string& error) {
        const HDEVINFO set = SetupDiGetClassDevsW(&kEnergyMeterInterface, nullptr, nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) {
            error = "no energy meter devices";
            return false;
        }
        bool accessDenied = false;
        SP_DEVICE_INTERFACE_DATA data{};
        data.cbSize = sizeof(data);
        for (DWORD index = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &kEnergyMeterInterface, index, &data);
             ++index) {
            DWORD size = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &size, nullptr);
            if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
                continue;
            }
            std::vector<BYTE> buffer(size, 0);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &data, detail, size, nullptr, nullptr)) {
                continue;
            }
            const HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                accessDenied = accessDenied || GetLastError() == ERROR_ACCESS_DENIED;
                continue;
            }
            if (!AddDevice(handle)) {
                CloseHandle(handle);
            }
        }
        SetupDiDestroyDeviceInfoList(set);
        if (m_names.empty()) {
            error = accessDenied ? "access denied (run from an elevated prompt)" : "no readable energy meter channels";
            return false;
        }
        return true;
    }

    [[nodiscard]] const std::vector<std::wstring>& Names() const noexcept { return m_names; }

    // Cumulative picowatt-hours for every channel, in Names() order.
    bool Read(std::vector<uint64_t>& energy) const {
        energy.clear();
        for (const Device& device : m_devices) {
            std::vector<BYTE> buffer(device.channels * kEmiMeasurementSize, 0);
            DWORD returned = 0;
            if (!DeviceIoControl(device.handle, kIoctlEmiGetMeasurement, nullptr, 0, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &returned, nullptr) ||
                returned < buffer.size()) {
                return false;
            }
            for (size_t channel = 0; channel < device.channels; ++channel) {
                energy.push_back(ReadAt<uint64_t>(buffer, channel * kEmiMeasurementSize));
            }
        }
        return true;
    }

private:
    struct Device {
        HANDLE handle;
        size_t channels;
    };

    bool AddDevice(HANDLE handle) {
        USHORT version = 0;
        DWORD returned = 0;
        ULONG metadataSize = 0;
        if (!DeviceIoControl(handle, kIoctlEmiGetVersion, nullptr, 0, &version, sizeof(version), &returned,
                nullptr) ||
            !DeviceIoControl(handle, kIoctlEmiGetMetadataSize, nullptr, 0, &metadataSize, sizeof(metadataSize),
                &returned, nullptr) ||
            metadataSize == 0 || metadataSize > 65536) {
            return false;
        }
        std::vector<BYTE> metadata(metadataSize, 0);
        if (!DeviceIoControl(handle, kIoctlEmiGetMetadata, nullptr, 0, metadata.data(), metadataSize, &returned,
                nullptr)) {
            return false;
        }
        const std::wstring prefix = m_devices.empty() ? L"" : L"meter" + std::to_wstring(m_devices.size()) + L":";
        std::vector<std::wstring> names;
        if (version == 1) {
            const auto nameSize = ReadAt<USHORT>(metadata, kEmiV1NameSizeOffset);
            names.push_back(prefix + WideAt(metadata, kEmiV1NameOffset, nameSize));
        } else if (version == 2) {
            const auto count = ReadAt<USHORT>(metadata, kEmiV2CountOffset);
            size_t offset = kEmiV2ChannelsOffset;
            for (USHORT channel = 0; channel < count && offset + 6 <= metadata.size(); ++channel) {
                const auto nameSize = ReadAt<USHORT>(metadata, offset + 4);
                std::wstring name = WideAt(metadata, offset + 6, nameSize);
                names.push_back(prefix + (name.empty() ? L"channel" + std::to_wstring(channel) : name));
                offset += 6 + nameSize;
            }
        } else {
            return false;
        }
        if (names.empty()) {
            return false;
        }
        m_devices.push_back({handle, names.size()});
        m_names.insert(m_names.end(), names.begin(), names.end());
        return true;
    }

    std::vector<Device> m_devices;
    std::vector<std::wstring> m_names;
};

// Prefers whole-platform power (PSYS), then the CPU package; skips DRAM / graphics-only rails.
int ChannelScore(const std::wstring& name) {
    const std::wstring lower = Lower(name);
    const auto has = [&](const wchar_t* part) { return lower.find(part) != std::wstring::npos; };
    if (has(L"_total") || has(L"dram") || has(L"pp1")) {
        return 0;
    }
    if (has(L"psys") || has(L"platform")) {
        return 5;
    }
    if (has(L"pkg") || has(L"package")) {
        return 4;
    }
    if (has(L"rapl") || has(L"soc") || has(L"cpu")) {
        return 2;
    }
    if (has(L"pp0") || has(L"core")) {
        return 1;
    }
    return 0;
}

// \Energy Meter(*)\Power (milliwatts), as SystemTemps reads it; works unelevated.
class PdhPowerMeter {
public:
    PdhPowerMeter() = default;
    ~PdhPowerMeter() {
        if (m_query != nullptr) {
            PdhCloseQuery(m_query);
        }
    }
    PdhPowerMeter(const PdhPowerMeter&) = delete;
    PdhPowerMeter& operator=(const PdhPowerMeter&) = delete;

    bool Open() {
        if (PdhOpenQueryW(nullptr, 0, &m_query) != ERROR_SUCCESS) {
            m_query = nullptr;
            return false;
        }
        if (PdhAddEnglishCounterW(m_query, L"\\Energy Meter(*)\\Power", 0, &m_counter) != ERROR_SUCCESS) {
            PdhCloseQuery(m_query);
            m_query = nullptr;
            return false;
        }
        PdhCollectQueryData(m_query);
        Sleep(250);
        double watts = 0.0;
        return Read(watts);
    }

    // Watts of the best-scoring instance; the chosen instance name is kept.
    bool Read(double& watts) {
        if (m_query == nullptr || PdhCollectQueryData(m_query) != ERROR_SUCCESS) {
            return false;
        }
        DWORD size = 0;
        DWORD count = 0;
        if (PdhGetFormattedCounterArrayW(m_counter, PDH_FMT_DOUBLE | PDH_FMT_NOSCALE, &size, &count, nullptr) !=
                static_cast<PDH_STATUS>(PDH_MORE_DATA) ||
            size == 0) {
            return false;
        }
        std::vector<BYTE> buffer(size, 0);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(m_counter, PDH_FMT_DOUBLE | PDH_FMT_NOSCALE, &size, &count, items) !=
            ERROR_SUCCESS) {
            return false;
        }
        int bestScore = 0;
        double bestMilliwatts = 0.0;
        std::wstring bestName;
        for (DWORD index = 0; index < count; ++index) {
            const PDH_FMT_COUNTERVALUE& value = items[index].FmtValue;
            if ((value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) ||
                !std::isfinite(value.doubleValue) || value.doubleValue < 0.0 || items[index].szName == nullptr) {
                continue;
            }
            const std::wstring name = items[index].szName;
            // Stay on one instance once chosen so every run reads the same rail.
            const int score = !m_instance.empty() ? (name == m_instance ? 100 : 0) : ChannelScore(name);
            if (score > bestScore) {
                bestScore = score;
                bestMilliwatts = value.doubleValue;
                bestName = name;
            }
        }
        if (bestScore <= 0) {
            return false;
        }
        m_instance = bestName;
        watts = bestMilliwatts / 1000.0;
        return true;
    }

    [[nodiscard]] const std::wstring& Instance() const noexcept { return m_instance; }

private:
    PDH_HQUERY m_query = nullptr;
    PDH_HCOUNTER m_counter = nullptr;
    std::wstring m_instance;
};

struct BatteryReading {
    bool present = false;
    bool onAc = true;
    bool discharging = false;
    // Positive while discharging, in watts.
    double dischargeWatts = 0.0;
    double remainingMwh = 0.0;
};

bool ReadBattery(BatteryReading& reading) {
    SYSTEM_BATTERY_STATE state{};
    if (CallNtPowerInformation(SystemBatteryState, nullptr, 0, &state, sizeof(state)) != 0) {
        return false;
    }
    reading.present = state.BatteryPresent != FALSE;
    reading.onAc = state.AcOnLine != FALSE;
    reading.discharging = state.Discharging != FALSE;
    // Rate is a signed milliwatt value stored in a DWORD: negative while discharging.
    const auto rate = static_cast<int32_t>(state.Rate);
    reading.dischargeWatts = rate < 0 ? -static_cast<double>(rate) / 1000.0 : 0.0;
    reading.remainingMwh = static_cast<double>(state.RemainingCapacity);
    return reading.present;
}

double CpuBusyFraction(const FILETIME& idle0, const FILETIME& kernel0, const FILETIME& user0, const FILETIME& idle1,
    const FILETIME& kernel1, const FILETIME& user1) {
    const auto value = [](const FILETIME& time) {
        return (static_cast<uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    };
    const uint64_t idle = value(idle1) - value(idle0);
    // Kernel time includes idle time.
    const uint64_t total = value(kernel1) - value(kernel0) + value(user1) - value(user0);
    return total == 0 ? 0.0 : static_cast<double>(total - std::min(idle, total)) / static_cast<double>(total);
}

double MetricValue(const std::string& metrics, const std::string& key) {
    if (key.empty()) {
        return kNaN;
    }
    const std::string needle = key + "=";
    size_t at = metrics.find(needle);
    while (at != std::string::npos && at != 0 && metrics[at - 1] != ' ') {
        at = metrics.find(needle, at + 1);
    }
    return at == std::string::npos ? kNaN : std::strtod(metrics.c_str() + at + needle.size(), nullptr);
}

std::string CsvField(const std::string& text) {
    if (text.find_first_of(",\"\n") == std::string::npos) {
        return text;
    }
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out.push_back(c);
        }
    }
    return out + "\"";
}

std::string Number(double value, int decimals = 3) {
    if (!std::isfinite(value)) {
        return "";
    }
    char text[48]{};
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

// ---- workloads (parent side) ------------------------------------------------------------

class WorkerProcess {
public:
    WorkerProcess() = default;
    ~WorkerProcess() { Close(); }
    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;

    bool Start(const std::wstring& exe, const std::wstring& arguments, std::string& error) {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE childInput = nullptr;
        HANDLE childOutput = nullptr;
        if (!CreatePipe(&childInput, &m_input, &inherit, 0) || !CreatePipe(&m_output, &childOutput, &inherit, 0)) {
            error = "could not create pipes";
            return false;
        }
        SetHandleInformation(m_input, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(m_output, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childInput;
        startup.hStdOutput = childOutput;
        startup.hStdError = childOutput;
        std::wstring commandLine = L"\"" + exe + L"\" " + arguments;
        PROCESS_INFORMATION info{};
        const BOOL created = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &info);
        CloseHandle(childInput);
        CloseHandle(childOutput);
        if (created == FALSE) {
            error = "could not start the worker process (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        CloseHandle(info.hThread);
        m_process = info.hProcess;
        return true;
    }

    void Mark() {
        DWORD written = 0;
        if (m_input != nullptr) {
            WriteFile(m_input, "m", 1, &written, nullptr);
        }
    }

    void RequestStop() {
        if (m_input != nullptr) {
            CloseHandle(m_input);
            m_input = nullptr;
        }
    }

    // Waits for exit, polling the abort flag; false on timeout or abort.
    bool WaitExit(double seconds) {
        const double until = Now() + seconds;
        while (m_process != nullptr) {
            if (WaitForSingleObject(m_process, 100) == WAIT_OBJECT_0) {
                return true;
            }
            if (g_abort.load() || Now() > until) {
                return false;
            }
        }
        return false;
    }

    // The worker's single result line, read after it exits.
    std::string Output() {
        std::string out;
        char buffer[512];
        DWORD read = 0;
        DWORD available = 0;
        while (m_output != nullptr && PeekNamedPipe(m_output, nullptr, 0, nullptr, &available, nullptr) &&
               available > 0 && ReadFile(m_output, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            out.append(buffer, read);
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
            out.pop_back();
        }
        return out;
    }

    void Close() {
        RequestStop();
        if (m_process != nullptr) {
            if (WaitForSingleObject(m_process, 3000) != WAIT_OBJECT_0) {
                TerminateProcess(m_process, 1);
                WaitForSingleObject(m_process, 2000);
            }
            CloseHandle(m_process);
            m_process = nullptr;
        }
        if (m_output != nullptr) {
            CloseHandle(m_output);
            m_output = nullptr;
        }
    }

private:
    HANDLE m_process = nullptr;
    HANDLE m_input = nullptr;
    HANDLE m_output = nullptr;
};

double CalibrateIterationsPerMs() {
    constexpr uint64_t kChunk = 1U << 18U;
    const double warm = Now();
    while (Now() - warm < 0.3) {
        Spin(kChunk, 5);
    }
    uint64_t total = 0;
    const double start = Now();
    while (Now() - start < 0.5) {
        Spin(kChunk, total + 1);
        total += kChunk;
    }
    return static_cast<double>(total) / ((Now() - start) * 1000.0);
}

bool WriteFileText(const std::wstring& path, const std::string& text, bool append = false) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), append ? L"ab" : L"wb") != 0 || file == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    std::fclose(file);
    return ok;
}

std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(size, 0)), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    }
    return out;
}

std::wstring ModulePath() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return length == 0 || length >= MAX_PATH ? std::wstring() : std::wstring(path, length);
}

std::wstring LocalStamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[32]{};
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond);
    return stamp;
}

