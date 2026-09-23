#pragma once

#include <Windows.h>

struct IDataObject;

#include <cstdint>
#include <string>
#include <vector>

struct RecycleBinState {
    ULONGLONG itemCount = 0;
    ULONGLONG byteSize = 0;
    bool isEmpty = true;
};

class RecycleBin {
public:
    static constexpr wchar_t kEmptyTarget[] = L"dock:trash-empty";
    static constexpr wchar_t kFullTarget[] = L"dock:trash-full";
    static constexpr wchar_t kLogicalTarget[] = L"dock:trash";

    [[nodiscard]] static bool QueryState(RecycleBinState& state) noexcept;
    [[nodiscard]] static bool IsEmpty() noexcept;
    [[nodiscard]] static ULONGLONG ItemCount() noexcept;

    // Opens the Recycle Bin in Explorer and brings it to the front.
    [[nodiscard]] static bool Open() noexcept;
    // Shows the native Recycle Bin properties dialog; falls back to an info box.
    static bool ShowProperties(HWND owner) noexcept;
    // Empties via the official shell API (confirmation + progress + sound).
    // When confirmAlreadyShown is true the shell confirmation is suppressed
    // because the caller already asked ("X items" dialog).
    [[nodiscard]] static bool Empty(HWND owner, bool confirmAlreadyShown = false) noexcept;

    struct MoveResult {
        size_t moved = 0;
        size_t failed = 0;
        bool aborted = false;
        std::wstring error;
    };
    // Moves paths to the Recycle Bin (never permanently deletes). Uses
    // IFileOperation::DeleteItem with FOF_ALLOWUNDO on a worker-friendly STA.
    [[nodiscard]] static MoveResult MoveToRecycleBin(
        HWND owner, const std::vector<std::wstring>& paths) noexcept;

    // Prefer MOVE, else COPY, else NONE (AND-masked with source-allowed effects).
    [[nodiscard]] static DWORD ChooseTrashDropEffect(DWORD allowed) noexcept;
    // Optimized-recycle OLE signal: CFSTR_TARGETCLSID=RecycleBin +
    // PERFORMEDDROPEFFECT=NONE (source must not delete again) +
    // LOGICALPERFORMEDDROPEFFECT=MOVE (user-visible outcome).
    static void SignalOptimizedRecycle(IDataObject* data) noexcept;
    // SHChangeNotify delete + parent UPDATEDIR so stubborn Explorer views refresh.
    static void NotifyPathsDeleted(const std::vector<std::wstring>& paths) noexcept;

    // Shell drag-drop helpers (CF_HDROP / shell ID list).
    [[nodiscard]] static std::vector<std::wstring> FilesFromDataObject(
        IDataObject* data) noexcept;
    [[nodiscard]] static bool DataObjectHasFiles(IDataObject* data) noexcept;

    // Human-readable helpers for dialogs / hover labels.
    [[nodiscard]] static std::wstring FormatByteSize(ULONGLONG bytes);
    [[nodiscard]] static std::wstring HoverLabel(const RecycleBinState& state);
    [[nodiscard]] static std::wstring EmptyConfirmText(const RecycleBinState& state);
    [[nodiscard]] static std::wstring InfoText(const RecycleBinState& state);

    // Icon pixels (BGRA premultiplied, extent x extent) for the D3D atlas.
    [[nodiscard]] static std::vector<uint8_t> IconPixels(bool full, UINT extent) noexcept;
    [[nodiscard]] static const wchar_t* TargetForState(bool full) noexcept;
};
