#pragma once

// Groq chat client for Search's AI answers (WinHTTP, streamed). Each ask runs
// on its own worker thread and publishes into a shared AiSession; the UI is
// poked with one coalesced message at a time and copies the latest state.

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AiSource {
    std::wstring title;
    std::wstring url;
    std::wstring snippet;
};

struct AiSnapshot {
    std::wstring markdown;
    std::wstring status;
    std::wstring error;
    std::vector<AiSource> sources;
    // Image URLs returned by web search: the only ones the UI fetches automatically.
    std::vector<std::wstring> imageUrls;
    bool searched = false;
    bool done = false;
};

struct AiSession {
    uint64_t id = 0;
    std::atomic<bool> cancelled{false};
    // Set by the worker when it posts; cleared by the UI before it reads.
    std::atomic<bool> messagePending{false};
    std::mutex mutex;
    AiSnapshot state;  // guarded by mutex

    [[nodiscard]] AiSnapshot Snapshot() {
        messagePending.store(false);
        const std::lock_guard lock(mutex);
        return state;
    }
};

struct AiImageReply {
    std::wstring url;
    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> pixels;  // premultiplied BGRA; empty on failure
};

namespace GroqClient {

inline constexpr wchar_t kDefaultModel[] = L"qwen/qwen3.8-27b";

// Streams an answer for `prompt` (plus attached images as data: URLs). Posts
// `message` (wParam = session->id) to `reply` whenever the session state changes.
void AskAsync(std::shared_ptr<AiSession> session, HWND reply, UINT message, std::wstring apiKey,
    std::wstring model, std::wstring prompt, std::vector<std::string> images = {});

// Downloads an http(s) image and decodes it (WIC) to fit maxWidth x maxHeight.
// Posts `message` with lParam = AiImageReply* (receiver owns it).
void FetchImageAsync(HWND reply, UINT message, std::wstring url, UINT maxWidth, UINT maxHeight);

}  // namespace GroqClient
