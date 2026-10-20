// Windows integration test + benchmark for FileSearchService on the real disk:
// index build (MFT in-process when elevated), query latency, live journal
// updates, and the elevated-helper path (task + named pipe) from a
// non-admin child process.
//   FileSearchServiceTests.exe              full run (needs an elevated token)
//   FileSearchServiceTests.exe --child      service only, prints the mode it got
//   FileSearchServiceTests.exe --mft-dump   helper entry point used by the task
#include "FileSearch.h"

#include <sddl.h>
#include <winsafer.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT kReplyMessage = WM_APP + 1;
int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

HWND MessageWindow() {
    return CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
}

std::unique_ptr<FileSearchReply> RunQuery(FileSearchService& service, HWND window, const std::wstring& query,
    double& roundTripMs, size_t files = 40) {
    static uint64_t id = 0;
    const uint64_t want = ++id;
    const auto start = Clock::now();
    service.Query(window, kReplyMessage, want, query, files, 6);
    const ULONGLONG deadline = GetTickCount64() + 10000;
    MSG message{};
    while (GetTickCount64() < deadline) {
        if (PeekMessageW(&message, window, kReplyMessage, kReplyMessage, PM_REMOVE) != FALSE) {
            std::unique_ptr<FileSearchReply> reply(reinterpret_cast<FileSearchReply*>(message.lParam));
            if (reply->requestId == want) {
                roundTripMs = MsSince(start);
                return reply;
            }
            continue;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_POSTMESSAGE);
    }
    roundTripMs = -1.0;
    return nullptr;
}

bool WaitIndexed(FileSearchService& service, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        const auto status = service.GetStatus();
        if (!status.indexing && status.entries > 0) {
            return true;
        }
        Sleep(100);
    }
    return false;
}

const char* ModeName(FileSearchMode mode) {
    switch (mode) {
    case FileSearchMode::Mft: return "MFT";
    case FileSearchMode::Folders: return "Folders";
    case FileSearchMode::Starting: return "Starting";
    }
    return "?";
}

bool Contains(const FileSearchReply& reply, const std::wstring& path) {
    return std::ranges::any_of(reply.files, [&](const FileSearchItem& item) {
        return CompareStringOrdinal(item.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
    });
}

std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH + 1]{};
    GetTempPathW(MAX_PATH + 1, buffer);
    std::wstring path(buffer);
    if (!path.empty() && path.back() == L'\\') {
        path.pop_back();
    }
    // Resolve 8.3 segments (RUNNER~1) so paths compare equal to the index's.
    wchar_t longPath[MAX_PATH + 1]{};
    if (GetLongPathNameW(path.c_str(), longPath, MAX_PATH + 1) != 0) {
        path = longPath;
    }
    return path;
}

int Child() {
    FileSearchService service;
    service.Start(nullptr, 0);
    const bool indexed = WaitIndexed(service, 240000);
    const auto status = service.GetStatus();
    std::printf("child: indexed=%d mode=%s entries=%zu buildMs=%.0f canEnableMft=%d\n", indexed ? 1 : 0,
        ModeName(status.mode), status.entries, status.buildMs, status.canEnableMft ? 1 : 0);
    if (status.mode == FileSearchMode::Mft) {
        HWND window = MessageWindow();
        double ms = 0.0;
        auto reply = RunQuery(service, window, L"notepad.exe", ms);
        std::printf("child: 'notepad.exe' files=%zu searchMs=%.2f\n", reply ? reply->files.size() : 0,
            reply ? reply->searchMs : -1.0);
        DestroyWindow(window);
    }
    service.Stop();
    return status.mode == FileSearchMode::Mft ? 0 : 2;
}

// Runs this exe as a non-admin (Safer "normal user") child: the dock's usual
// situation, where only the registered helper task can read the MFT.
int RunNonAdminChild() {
    SAFER_LEVEL_HANDLE level = nullptr;
    HANDLE token = nullptr;
    if (SaferCreateLevel(SAFER_SCOPEID_USER, SAFER_LEVELID_NORMALUSER, SAFER_LEVEL_OPEN, &level, nullptr) == FALSE ||
        SaferComputeTokenFromLevel(level, nullptr, &token, 0, nullptr) == FALSE) {
        std::printf("helper path: could not build a normal-user token (%lu)\n", GetLastError());
        return -1;
    }
    SaferCloseLevel(level);
    PSID medium = nullptr;
    ConvertStringSidToSidW(L"S-1-16-8192", &medium);
    TOKEN_MANDATORY_LABEL label{};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = medium;
    SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(medium));
    LocalFree(medium);
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = std::wstring(L"\"") + self + L"\" --child";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessAsUserW(token, nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
            &process) == FALSE) {
        std::printf("helper path: CreateProcessAsUser failed (%lu)\n", GetLastError());
        CloseHandle(token);
        return -1;
    }
    CloseHandle(token);
    WaitForSingleObject(process.hProcess, 300000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}

std::wstring CurrentSid() {
    HANDLE token = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    LPWSTR text = nullptr;
    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text);
    std::wstring sid = text != nullptr ? text : L"";
    LocalFree(text);
    return sid;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::wstring(argv[1]) == L"--mft-dump") {
        return RunMftDumpHelper();
    }
    if (argc > 1 && std::wstring(argv[1]) == L"--child") {
        return Child();
    }

    FileSearchService service;
    const auto start = Clock::now();
    service.Start(nullptr, 0);
    CHECK(WaitIndexed(service, 240000));
    const auto status = service.GetStatus();
    std::printf("index: mode=%s entries=%zu buildMs=%.0f (wall %.0f) memory=%.1f MB\n", ModeName(status.mode),
        status.entries, status.buildMs, MsSince(start), static_cast<double>(status.memoryBytes) / 1048576.0);
    CHECK(status.mode == FileSearchMode::Mft);

    HWND window = MessageWindow();
    service.SetApps({L"Notepad", L"Calculator", L"Microsoft Edge", L"Mail"});
    service.SetActive(true);

    // Known system file is found with its full path.
    double ms = 0.0;
    auto reply = RunQuery(service, window, L"notepad.exe", ms);
    CHECK(reply != nullptr);
    if (reply != nullptr) {
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        CHECK(Contains(*reply, std::wstring(windows) + L"\\System32\\notepad.exe") ||
            Contains(*reply, std::wstring(windows) + L"\\notepad.exe"));
        CHECK(!reply->apps.empty() && reply->apps.front().name == L"Notepad");
    }

    // Latency per query (search thread time and post/round trip).
    const wchar_t* queries[] = {L"a", L"re", L"mail", L"notepad", L"system32", L"visual studio", L"zzqxv",
        L"microsoft edge"};
    std::printf("%-16s %8s %10s %10s %10s\n", "query", "files", "search ms", "p50 rt ms", "max rt ms");
    for (const wchar_t* query : queries) {
        std::vector<double> roundTrips;
        double searchMs = 0.0;
        size_t files = 0;
        for (int run = 0; run < 9; ++run) {
            auto r = RunQuery(service, window, std::wstring(query) + std::wstring(run % 2, L' '), ms);
            if (r != nullptr) {
                roundTrips.push_back(ms);
                searchMs = r->searchMs;
                files = r->files.size();
            }
        }
        std::sort(roundTrips.begin(), roundTrips.end());
        CHECK(!roundTrips.empty());
        if (!roundTrips.empty()) {
            std::printf("%-16ls %8zu %10.2f %10.2f %10.2f\n", query, files, searchMs,
                roundTrips[roundTrips.size() / 2], roundTrips.back());
        }
    }
    // Typing, one keystroke at a time.
    {
        const std::wstring word = L"powershell";
        std::wstring typed;
        double worst = 0.0;
        double total = 0.0;
        for (const wchar_t c : word) {
            typed.push_back(c);
            auto r = RunQuery(service, window, typed, ms);
            CHECK(r != nullptr);
            worst = std::max(worst, ms);
            total += ms;
        }
        std::printf("typing 'powershell': total %.2f ms, worst keystroke %.2f ms\n", total, worst);
    }

    // Live updates from the USN journal: create, rename, delete.
    const std::wstring directory = TempDirectory();
    wchar_t unique[64]{};
    swprintf_s(unique, L"hdsearch_%08lx", GetTickCount());
    const std::wstring created = directory + L"\\" + unique + L"_alpha.txt";
    const std::wstring renamed = directory + L"\\" + unique + L"_omega.txt";
    auto waitFor = [&](const std::wstring& query, const std::wstring& path, bool present) {
        const auto t0 = Clock::now();
        while (MsSince(t0) < 10000.0) {
            auto r = RunQuery(service, window, query, ms, 200);
            if (r != nullptr && Contains(*r, path) == present) {
                return MsSince(t0);
            }
            Sleep(50);
        }
        return -1.0;
    };
    HANDLE file = CreateFileW(created.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    const double seenCreate = waitFor(std::wstring(unique) + L" alpha", created, true);
    CHECK(seenCreate >= 0.0);
    CHECK(MoveFileW(created.c_str(), renamed.c_str()) != FALSE);
    const double seenRename = waitFor(std::wstring(unique) + L" omega", renamed, true);
    CHECK(seenRename >= 0.0);
    CHECK(waitFor(std::wstring(unique) + L" alpha", created, false) >= 0.0);
    CHECK(DeleteFileW(renamed.c_str()) != FALSE);
    const double seenDelete = waitFor(std::wstring(unique) + L" omega", renamed, false);
    CHECK(seenDelete >= 0.0);
    std::printf("journal: create visible after %.0f ms, rename %.0f ms, delete %.0f ms\n", seenCreate, seenRename,
        seenDelete);

    DestroyWindow(window);
    service.Stop();

    // Elevated helper path: register the per-user task (as the Search panel's
    // UAC step does), then index from a non-admin child via task + pipe.
    const std::wstring sid = CurrentSid();
    const int registered = RegisterMftTask(sid.c_str());
    std::printf("helper path: RegisterMftTask -> %d\n", registered);
    CHECK(registered == 0);
    const int child = RunNonAdminChild();
    std::printf("helper path: non-admin child exit=%d (0 = indexed via helper, 2 = folder fallback)\n", child);
    CHECK(child == 0);
    const std::wstring remove = L"schtasks.exe /Delete /F /TN \"HoverdockMftIndex-" + sid + L"\"";
    _wsystem(remove.c_str());
    _wsystem(L"cmd /c rd /s /q \"%ProgramFiles%\\Hoverdock\"");

    if (g_failures == 0) {
        std::printf("FileSearchService tests: all passed\n");
        return 0;
    }
    std::printf("FileSearchService tests: %d failure(s)\n", g_failures);
    return 1;
}
