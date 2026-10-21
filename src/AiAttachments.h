#pragma once

// Image attachments for Search's AI answers: read from the clipboard or a
// file picker, then prepared off the UI thread (WIC decode, downscale, JPEG
// data URL for Groq, rounded premultiplied thumbnail for the panel).

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct AiAttachmentSource {
    std::wstring name;
    // Either a file to read or already-encoded image bytes (clipboard PNG/BMP).
    std::wstring path;
    std::string bytes;
};

struct AiAttachmentReply {
    uint64_t id = 0;
    std::wstring name;
    std::string dataUrl;          // empty on failure
    std::vector<uint8_t> thumb;   // premultiplied BGRA, thumbExtent x thumbExtent
    UINT thumbExtent = 0;
    std::wstring error;
};

namespace AiAttachments {

[[nodiscard]] bool IsImagePath(const std::wstring& path);
// Images on the clipboard: copied image files first, then bitmap data (only
// when there is no text, so a spreadsheet copy still pastes as text).
[[nodiscard]] bool ReadClipboard(HWND owner, std::vector<AiAttachmentSource>& out);
// Modal multi-select image picker owned by `owner`.
[[nodiscard]] std::vector<std::wstring> PickFiles(HWND owner);
// Posts `message` with lParam = AiAttachmentReply* (receiver owns it).
void PrepareAsync(HWND reply, UINT message, uint64_t id, AiAttachmentSource source, UINT thumbExtent);

}  // namespace AiAttachments
