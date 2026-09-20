#include "Startup.h"

#include <Windows.h>

#include <OleAuto.h>
#include <taskschd.h>

#include <array>
#include <string>

namespace {
constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"Hoverdock";
constexpr wchar_t kTaskName[] = L"Hoverdock";
constexpr wchar_t kSerializeKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize";
constexpr wchar_t kStartupDelayValue[] = L"StartupDelayInMSec";

std::wstring Quoted(const std::wstring& path) {
    return L"\"" + path + L"\"";
}

bool RunValuePresent() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[32768]{};
    DWORD byteCount = sizeof(value);
    DWORD type = 0;
    const LONG queried =
        RegQueryValueExW(key, kRunValueName, nullptr, &type, reinterpret_cast<BYTE*>(value),
            &byteCount);
    RegCloseKey(key);
    if (queried != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }
    // Any value counts as enabled; SyncWithConfig repairs stale paths below.
    return value[0] != L'\0';
}

bool ReadRunCommand(std::wstring& command) {
    command.clear();
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[32768]{};
    DWORD byteCount = sizeof(value);
    DWORD type = 0;
    const LONG queried =
        RegQueryValueExW(key, kRunValueName, nullptr, &type, reinterpret_cast<BYTE*>(value),
            &byteCount);
    RegCloseKey(key);
    if (queried != ERROR_SUCCESS || type != REG_SZ || value[0] == L'\0') {
        return false;
    }
    command.assign(value);
    return true;
}

bool WriteRunCommand(const std::wstring& command) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
            &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG written = RegSetValueExW(key, kRunValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1U) * sizeof(wchar_t)));
    RegCloseKey(key);
    return written == ERROR_SUCCESS;
}

bool DeleteRunValue() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return true;  // No key: already disabled.
    }
    const LONG deleted = RegDeleteValueW(key, kRunValueName);
    RegCloseKey(key);
    return deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND;
}

// Windows delays Run-key apps after logon (the "Startup delay"). Zeroing it
// makes the Run fallback fire as early as Explorer allows. Only applied when
// startup is enabled; disabling startup leaves the system value alone.
void DisableStartupDelay() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSerializeKeyPath, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    constexpr DWORD kNoDelay = 0;
    static_cast<void>(RegSetValueExW(key, kStartupDelayValue, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&kNoDelay), sizeof(kNoDelay)));
    RegCloseKey(key);
}

class TaskService {
public:
    TaskService() = default;
    TaskService(const TaskService&) = delete;
    TaskService& operator=(const TaskService&) = delete;

    ~TaskService() {
        if (m_folder != nullptr) {
            m_folder->Release();
        }
        if (m_service != nullptr) {
            m_service->Release();
        }
        if (m_ownedCom) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool Connected() const noexcept {
        return m_service != nullptr && m_folder != nullptr;
    }

    bool Connect() {
        if (Connected()) {
            return true;
        }
        // The dock's UI thread already runs STA COM, so this is usually
        // S_FALSE (already initialized): usable, but not owned.
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (init == S_OK) {
            m_ownedCom = true;
        } else if (init == S_FALSE || init == RPC_E_CHANGED_MODE) {
            m_ownedCom = false;
        } else {
            return false;
        }

        ITaskService* service = nullptr;
        const HRESULT created = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITaskService, reinterpret_cast<void**>(&service));
        if (FAILED(created) || service == nullptr) {
            CleanupCom();
            return false;
        }
        VARIANT empty{};
        VariantInit(&empty);
        const HRESULT connected = service->Connect(empty, empty, empty, empty);
        VariantClear(&empty);
        if (FAILED(connected)) {
            service->Release();
            CleanupCom();
            return false;
        }
        BSTR root = SysAllocString(L"\\");
        if (root == nullptr) {
            service->Release();
            CleanupCom();
            return false;
        }
        ITaskFolder* folder = nullptr;
        const HRESULT foldered = service->GetFolder(root, &folder);
        SysFreeString(root);
        if (FAILED(foldered) || folder == nullptr) {
            service->Release();
            CleanupCom();
            return false;
        }
        m_service = service;
        m_folder = folder;
        return true;
    }

    [[nodiscard]] ITaskService* Service() const noexcept {
        return m_service;
    }

    [[nodiscard]] ITaskFolder* Folder() const noexcept {
        return m_folder;
    }

private:
    void CleanupCom() noexcept {
        if (m_ownedCom) {
            CoUninitialize();
            m_ownedCom = false;
        }
    }

    ITaskService* m_service = nullptr;
    ITaskFolder* m_folder = nullptr;
    bool m_ownedCom = false;
};

bool TaskExists() {
    TaskService task;
    if (!task.Connect()) {
        return false;
    }
    BSTR name = SysAllocString(kTaskName);
    if (name == nullptr) {
        return false;
    }
    IRegisteredTask* registered = nullptr;
    const HRESULT got = task.Folder()->GetTask(name, &registered);
    SysFreeString(name);
    if (SUCCEEDED(got) && registered != nullptr) {
        registered->Release();
        return true;
    }
    return false;
}

bool DeleteLogonTask() {
    TaskService task;
    if (!task.Connect()) {
        return false;
    }
    BSTR name = SysAllocString(kTaskName);
    if (name == nullptr) {
        return false;
    }
    const HRESULT deleted = task.Folder()->DeleteTask(name, 0);
    SysFreeString(name);
    return deleted == S_OK || deleted == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

bool CreateLogonTask(const std::wstring& exePath) {
    if (exePath.empty()) {
        return false;
    }
    TaskService task;
    if (!task.Connect()) {
        return false;
    }

    ITaskDefinition* definition = nullptr;
    if (FAILED(task.Service()->NewTask(0, &definition)) || definition == nullptr) {
        return false;
    }
    bool ok = false;

    IRegistrationInfo* registration = nullptr;
    IPrincipal* principal = nullptr;
    ITaskSettings* settings = nullptr;
    ITriggerCollection* triggers = nullptr;
    IActionCollection* actions = nullptr;
    IRegisteredTask* registered = nullptr;

    do {
        if (FAILED(definition->get_RegistrationInfo(&registration)) || registration == nullptr) {
            break;
        }
        BSTR author = SysAllocString(L"Hoverdock");
        BSTR description =
            SysAllocString(L"Launch Hoverdock at logon with no delay (fast startup).");
        if (author != nullptr) {
            static_cast<void>(registration->put_Author(author));
            SysFreeString(author);
        }
        if (description != nullptr) {
            static_cast<void>(registration->put_Description(description));
            SysFreeString(description);
        }

        if (FAILED(definition->get_Principal(&principal)) || principal == nullptr) {
            break;
        }
        static_cast<void>(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));
        static_cast<void>(principal->put_RunLevel(TASK_RUNLEVEL_LUA));

        if (FAILED(definition->get_Settings(&settings)) || settings == nullptr) {
            break;
        }
        static_cast<void>(settings->put_Enabled(VARIANT_TRUE));
        static_cast<void>(settings->put_AllowDemandStart(VARIANT_TRUE));
        static_cast<void>(settings->put_StartWhenAvailable(VARIANT_TRUE));
        static_cast<void>(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));
        static_cast<void>(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
        BSTR noLimit = SysAllocString(L"PT0S");
        if (noLimit != nullptr) {
            static_cast<void>(settings->put_ExecutionTimeLimit(noLimit));
            SysFreeString(noLimit);
        }
        static_cast<void>(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));

        if (FAILED(definition->get_Triggers(&triggers)) || triggers == nullptr) {
            break;
        }
        ITrigger* triggerBase = nullptr;
        if (FAILED(triggers->Create(TASK_TRIGGER_LOGON, &triggerBase)) || triggerBase == nullptr) {
            break;
        }
        ILogonTrigger* logon = nullptr;
        const HRESULT queried =
            triggerBase->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(&logon));
        triggerBase->Release();
        triggerBase = nullptr;
        if (FAILED(queried) || logon == nullptr) {
            break;
        }
        BSTR triggerId = SysAllocString(L"HoverdockLogon");
        if (triggerId != nullptr) {
            static_cast<void>(logon->put_Id(triggerId));
            SysFreeString(triggerId);
        }
        // Zero delay: fire immediately at logon instead of joining the
        // delayed Startup-apps queue.
        BSTR delay = SysAllocString(L"PT0S");
        if (delay != nullptr) {
            static_cast<void>(logon->put_Delay(delay));
            SysFreeString(delay);
        }
        // Scope the trigger to the current user when the name is available;
        // leaving it blank (any user) is an acceptable fallback.
        wchar_t userName[257]{};
        DWORD userNameSize = 257;
        if (GetUserNameW(userName, &userNameSize) != FALSE && userName[0] != L'\0') {
            BSTR userId = SysAllocString(userName);
            if (userId != nullptr) {
                static_cast<void>(logon->put_UserId(userId));
                SysFreeString(userId);
            }
        }
        static_cast<void>(logon->put_Enabled(VARIANT_TRUE));
        logon->Release();

        if (FAILED(definition->get_Actions(&actions)) || actions == nullptr) {
            break;
        }
        IAction* actionBase = nullptr;
        if (FAILED(actions->Create(TASK_ACTION_EXEC, &actionBase)) || actionBase == nullptr) {
            break;
        }
        IExecAction* exec = nullptr;
        const HRESULT execQueried =
            actionBase->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&exec));
        actionBase->Release();
        actionBase = nullptr;
        if (FAILED(execQueried) || exec == nullptr) {
            break;
        }
        BSTR path = SysAllocString(exePath.c_str());
        if (path == nullptr) {
            exec->Release();
            break;
        }
        const HRESULT pathed = exec->put_Path(path);
        SysFreeString(path);
        if (FAILED(pathed)) {
            exec->Release();
            break;
        }
        const size_t separator = exePath.find_last_of(L"\\/");
        if (separator != std::wstring::npos && separator > 0) {
            std::wstring workingDir = exePath.substr(0, separator);
            BSTR work = SysAllocString(workingDir.c_str());
            if (work != nullptr) {
                static_cast<void>(exec->put_WorkingDirectory(work));
                SysFreeString(work);
            }
        }
        exec->Release();

        BSTR name = SysAllocString(kTaskName);
        if (name == nullptr) {
            break;
        }
        VARIANT user{};
        VariantInit(&user);
        VARIANT password{};
        VariantInit(&password);
        VARIANT sddl{};
        VariantInit(&sddl);
        const HRESULT registeredResult = task.Folder()->RegisterTaskDefinition(name, definition,
            TASK_CREATE_OR_UPDATE, user, password, TASK_LOGON_INTERACTIVE_TOKEN, sddl,
            &registered);
        VariantClear(&user);
        VariantClear(&password);
        VariantClear(&sddl);
        SysFreeString(name);
        if (FAILED(registeredResult)) {
            break;
        }
        ok = true;
    } while (false);

    if (registered != nullptr) {
        registered->Release();
    }
    if (actions != nullptr) {
        actions->Release();
    }
    if (triggers != nullptr) {
        triggers->Release();
    }
    if (settings != nullptr) {
        settings->Release();
    }
    if (principal != nullptr) {
        principal->Release();
    }
    if (registration != nullptr) {
        registration->Release();
    }
    definition->Release();
    return ok;
}

}  // namespace

std::wstring Startup::CurrentExecutablePath() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

std::wstring Startup::StartupCommand() {
    const std::wstring exe = CurrentExecutablePath();
    if (exe.empty()) {
        return {};
    }
    return Quoted(exe);
}

bool Startup::IsEnabled() {
    if (RunValuePresent()) {
        return true;
    }
    return TaskExists();
}

bool Startup::SetEnabled(bool enabled) {
    if (!enabled) {
        const bool runGone = DeleteRunValue();
        const bool taskGone = DeleteLogonTask();
        // Task Scheduler may be unavailable (service disabled): the Run key
        // is the fallback, so success there is enough to report success.
        // When both channels are reachable, both must be cleared.
        if (RunValuePresent() || TaskExists()) {
            return runGone && taskGone;
        }
        return true;
    }

    const std::wstring command = StartupCommand();
    const std::wstring exe = CurrentExecutablePath();
    if (command.empty() || exe.empty()) {
        return false;
    }
    const bool runOk = WriteRunCommand(command);
    const bool taskOk = CreateLogonTask(exe);
    DisableStartupDelay();
    return runOk || taskOk;
}

void Startup::SyncWithConfig(bool configEnabled) {
    const std::wstring command = StartupCommand();
    const std::wstring exe = CurrentExecutablePath();
    if (command.empty() || exe.empty()) {
        return;
    }

    std::wstring current;
    const bool hasRun = ReadRunCommand(current);
    const bool runOk = hasRun && current == command;

    if (!configEnabled) {
        if (hasRun) {
            // The toggle is authoritative: a leftover installer/startup entry is
            // removed so "off" really means off.
            static_cast<void>(DeleteRunValue());
        }
        if (TaskExists()) {
            static_cast<void>(DeleteLogonTask());
        }
        return;
    }

    if (!runOk) {
        static_cast<void>(WriteRunCommand(command));
    }
    // Fast path: Run already correct and the logon task exists (the common
    // steady state) — nothing to repair.
    if (runOk && TaskExists()) {
        DisableStartupDelay();
        return;
    }
    // Missing task, or the exe moved (Run was stale): (re)create the task so
    // it points at this copy. TASK_CREATE_OR_UPDATE makes this idempotent.
    static_cast<void>(CreateLogonTask(exe));
    DisableStartupDelay();
}
