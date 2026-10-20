#pragma once

// Dock Search engine: an always-current index of every file and folder name
// on the fixed NTFS volumes, read straight from the Master File Table and
// kept live from the USN change journal (Everything-style), plus the app
// catalogue. Queries run on a dedicated thread and post results back.
//
// MFT enumeration needs elevation (MEASURED: FSCTL_ENUM_USN_DATA is denied to
// filtered-admin and standard tokens), while reading the journal does not
// (FSCTL_READ_UNPRIVILEGED_USN_JOURNAL). So:
//   - elevated dock          -> reads the MFT in-process
//   - otherwise, once the user approved it (one UAC prompt), the on-demand
//     task "HoverdockMftIndex-<SID>" runs `Dock.exe --mft-dump` elevated; it
//     streams the raw MFT records over a per-user named pipe and exits
//   - otherwise              -> crawls the user's folders (fallback)
// In every mode the dock itself follows the journal without elevation.

#include "FileSearchIndex.h"

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

enum class FileSearchMode : uint8_t {
    Starting,
    Mft,       // whole fixed NTFS volumes from the MFT
    Folders,   // fallback: user folders crawled
};

struct FileSearchItem {
    enum class Kind : uint8_t { App, File, Folder };
    Kind kind = Kind::File;
    uint32_t appIndex = 0;        // Kind::App: index into the list given to SetApps
    std::wstring name;
    std::wstring path;            // files / folders
    ULONGLONG size = 0;
    FILETIME modified{};
    bool haveMetadata = false;
};

struct FileSearchReply {
    uint64_t requestId = 0;
    std::wstring query;
    std::vector<FileSearchItem> apps;
    std::vector<FileSearchItem> files;
    double searchMs = 0.0;
    // Second reply for the same request: `files` now carry size/date.
    bool metadata = false;
    bool indexing = false;
    size_t indexedEntries = 0;
};

class FileSearchService {
public:
    FileSearchService() = default;
    ~FileSearchService();
    FileSearchService(const FileSearchService&) = delete;
    FileSearchService& operator=(const FileSearchService&) = delete;

    // Starts background indexing (idempotent). `statusMessage` is posted to
    // `notify` whenever the index (re)builds or its mode changes.
    void Start(HWND notify, UINT statusMessage);
    void Stop() noexcept;
    // Reindex from scratch (after the user enabled MFT access).
    void Restart();
    // Fast journal polling while Search is open; slow trickle otherwise.
    void SetActive(bool active) noexcept;
    // Replaces the app catalogue matched alongside files.
    void SetApps(const std::vector<std::wstring>& appNames);
    // Async query; only the newest pending request is executed. The reply
    // (heap FileSearchReply*) is posted as lParam of `message`.
    void Query(HWND reply, UINT message, uint64_t requestId, std::wstring query, size_t fileLimit,
        size_t appLimit);

    struct Status {
        FileSearchMode mode = FileSearchMode::Starting;
        bool indexing = true;
        size_t entries = 0;
        size_t progress = 0;
        double buildMs = 0.0;
        size_t memoryBytes = 0;
        // Offer the one-time "enable full-disk search" step.
        bool canEnableMft = false;
    };
    [[nodiscard]] Status GetStatus() const;

    // Prompts (UAC) to register the elevated MFT task. Blocking; call from a
    // worker thread. True when the task is now registered.
    [[nodiscard]] static bool RequestMftAccess();

private:
    struct Request {
        HWND reply = nullptr;
        UINT message = 0;
        uint64_t id = 0;
        std::wstring query;
        size_t fileLimit = 0;
        size_t appLimit = 0;
    };
    struct Journal {
        uint8_t volume = 0;
        wchar_t letter = L'C';
        HANDLE handle = INVALID_HANDLE_VALUE;
        uint64_t journalId = 0;
        int64_t nextUsn = 0;
        bool privileged = false;  // use FSCTL_READ_USN_JOURNAL
    };

    void IndexThread();
    void SearchThread();
    bool BuildIndex(std::unique_ptr<filesearch::FileIndex>& index, std::vector<Journal>& journals,
        FileSearchMode& mode);
    // Returns false when a journal wrapped / was reset (rebuild needed).
    bool PollJournals();
    void CloseJournals() noexcept;
    void NotifyStatus() const noexcept;
    [[nodiscard]] bool StopRequested() const noexcept { return m_stop.load(); }

    HWND m_notify = nullptr;
    UINT m_statusMessage = 0;

    mutable std::shared_mutex m_indexLock;
    std::unique_ptr<filesearch::FileIndex> m_index;
    filesearch::FileIndex m_apps;
    std::vector<Journal> m_journals;  // index thread only

    std::thread m_indexThread;
    std::thread m_searchThread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_rebuild{false};
    std::atomic<bool> m_active{false};
    HANDLE m_wake = nullptr;  // auto-reset: poll journals / rebuild now

    mutable std::mutex m_statusLock;
    Status m_status;
    std::atomic<size_t> m_progress{0};

    std::mutex m_requestLock;
    std::condition_variable m_requestReady;
    bool m_hasRequest = false;
    Request m_request;
};

// Dock.exe --mft-dump: elevated helper run by the on-demand task.
int RunMftDumpHelper() noexcept;
// Dock.exe --register-mft-task <SID>: elevated (UAC) one-time registration.
int RegisterMftTask(const wchar_t* userSid) noexcept;
