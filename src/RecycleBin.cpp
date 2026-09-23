#include "RecycleBin.h"

#include <Shellapi.h>
#include <CommCtrl.h>
#include <commoncontrols.h>
#include <ShlObj.h>
#include <ObjIdl.h>
#include <ShObjIdl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <sstream>

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kRecycleBinFolder[] = L"shell:RecycleBinFolder";
constexpr wchar_t kRecycleBinClsidPath[] =
    L"::{645FF040-5081-101B-9F08-00AA002F954E}";

void ClearTransparentRgb(std::vector<uint8_t>& pixels) {
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i + 3] == 0) {
            pixels[i] = 0;
            pixels[i + 1] = 0;
            pixels[i + 2] = 0;
        }
    }
}

bool HasAnyNonZeroAlpha(const std::vector<uint8_t>& pixels) {
    for (size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0) {
            return true;
        }
    }
    return false;
}

bool LooksPremultiplied(const std::vector<uint8_t>& pixels) {
    size_t partial = 0;
    size_t exceeds = 0;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const unsigned alpha = pixels[i + 3];
        if (alpha == 0 || alpha == 255) {
            continue;
        }
        ++partial;
        if (pixels[i] > alpha || pixels[i + 1] > alpha || pixels[i + 2] > alpha) {
            ++exceeds;
        }
    }
    return partial == 0 || exceeds * 4 <= partial;
}

void ConvertToPremultiplied(std::vector<uint8_t>& pixels) {
    if (!LooksPremultiplied(pixels)) {
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
            const unsigned alpha = pixels[i + 3];
            pixels[i] = static_cast<uint8_t>(pixels[i] * alpha / 255U);
            pixels[i + 1] = static_cast<uint8_t>(pixels[i + 1] * alpha / 255U);
            pixels[i + 2] = static_cast<uint8_t>(pixels[i + 2] * alpha / 255U);
        }
    }
    ClearTransparentRgb(pixels);
}

BITMAPV5HEADER IconBitmapHeader(UINT width, UINT height) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(width);
    header.bV5Height = -static_cast<LONG>(height);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    return header;
}

std::vector<uint8_t> RasterizeHiconGdi(HICON icon, UINT extent) {
    if (icon == nullptr || extent == 0) {
        return {};
    }
    BITMAPV5HEADER header = IconBitmapHeader(extent, extent);
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return {};
    }
    HDC memory = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr || bitmap == nullptr || bits == nullptr) {
        if (memory != nullptr) {
            DeleteDC(memory);
        }
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        return {};
    }
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    if (previous == nullptr || previous == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(memory);
        return {};
    }
    std::memset(bits, 0, static_cast<size_t>(extent) * extent * 4U);
    const BOOL drawn = DrawIconEx(memory, 0, 0, icon, static_cast<int>(extent),
        static_cast<int>(extent), 0, nullptr, DI_NORMAL);
    std::vector<uint8_t> pixels;
    if (drawn != FALSE) {
        pixels.resize(static_cast<size_t>(extent) * extent * 4U);
        std::memcpy(pixels.data(), bits, pixels.size());
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    if (pixels.empty() || !HasAnyNonZeroAlpha(pixels)) {
        return {};
    }
    ConvertToPremultiplied(pixels);
    return pixels;
}

std::vector<uint8_t> RasterizeHicon(HICON icon, UINT extent) {
    if (icon == nullptr || extent == 0) {
        return {};
    }
    // Prefer WIC (high-quality scaling from the native icon size) like the
    // main renderer, fall back to GDI DrawIconEx at the exact extent.
    ComPtr<IWICImagingFactory> factory;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) &&
        factory != nullptr) {
        ComPtr<IWICBitmap> bitmap;
        if (SUCCEEDED(factory->CreateBitmapFromHICON(icon, &bitmap)) && bitmap != nullptr) {
            UINT width = 0;
            UINT height = 0;
            if (SUCCEEDED(bitmap->GetSize(&width, &height)) && width > 0 && height > 0) {
                ComPtr<IWICFormatConverter> converter;
                if (SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                    SUCCEEDED(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppBGRA,
                        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                    std::vector<uint8_t> source(static_cast<size_t>(width) * height * 4U);
                    if (SUCCEEDED(converter->CopyPixels(nullptr, width * 4U,
                            static_cast<UINT>(source.size()), source.data()))) {
                        ConvertToPremultiplied(source);
                        // Scale via WIC to the atlas extent (fant for downscale).
                        ComPtr<IWICBitmap> memBitmap;
                        if (SUCCEEDED(factory->CreateBitmapFromMemory(width, height,
                                GUID_WICPixelFormat32bppPBGRA, width * 4U,
                                static_cast<UINT>(source.size()), source.data(), &memBitmap)) &&
                            memBitmap != nullptr) {
                            ComPtr<IWICBitmapScaler> scaler;
                            ComPtr<IWICBitmapSource> sized = memBitmap;
                            if (width != extent || height != extent) {
                                if (SUCCEEDED(factory->CreateBitmapScaler(&scaler))) {
                                    const WICBitmapInterpolationMode filter =
                                        (extent < width || extent < height)
                                        ? WICBitmapInterpolationModeFant
                                        : WICBitmapInterpolationModeHighQualityCubic;
                                    if (SUCCEEDED(scaler->Initialize(
                                            memBitmap.Get(), extent, extent, filter))) {
                                        sized = scaler;
                                    }
                                }
                            }
                            ComPtr<IWICFormatConverter> finalConvert;
                            if (SUCCEEDED(factory->CreateFormatConverter(&finalConvert)) &&
                                SUCCEEDED(finalConvert->Initialize(sized.Get(),
                                    GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                    0.0, WICBitmapPaletteTypeCustom))) {
                                std::vector<uint8_t> out(
                                    static_cast<size_t>(extent) * extent * 4U);
                                if (SUCCEEDED(finalConvert->CopyPixels(nullptr, extent * 4U,
                                        static_cast<UINT>(out.size()), out.data())) &&
                                    HasAnyNonZeroAlpha(out)) {
                                    return out;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return RasterizeHiconGdi(icon, extent);
}

HICON StockRecycleIcon(bool full, bool jumbo) noexcept {
    // SEH: stock icon lookup runs shell code; never take the dock down.
    __try {
        if (jumbo) {
            SHSTOCKICONINFO info{sizeof(info)};
            const SHSTOCKICONID id =
                full ? SIID_RECYCLERFULL : SIID_RECYCLER;
            if (SUCCEEDED(SHGetStockIconInfo(
                    id, SHGSI_SYSICONINDEX, &info))) {
                IImageList* list = nullptr;
                if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_IImageList, reinterpret_cast<void**>(&list))) &&
                    list != nullptr) {
                    HICON icon = nullptr;
                    const HRESULT got =
                        list->GetIcon(info.iSysImageIndex, ILD_TRANSPARENT, &icon);
                    list->Release();
                    if (SUCCEEDED(got) && icon != nullptr) {
                        return icon;
                    }
                }
            }
            return nullptr;
        }
        SHSTOCKICONINFO info{sizeof(info)};
        const SHSTOCKICONID id = full ? SIID_RECYCLERFULL : SIID_RECYCLER;
        if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_LARGEICON, &info)) &&
            info.hIcon != nullptr) {
            return info.hIcon;
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

HICON RecycleIconFallback() noexcept {
    __try {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(kRecycleBinClsidPath, 0, &info, sizeof(info),
                SHGFI_ICON | SHGFI_LARGEICON) != 0 &&
            info.hIcon != nullptr) {
            return info.hIcon;
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

}  // namespace

bool RecycleBin::QueryState(RecycleBinState& state) noexcept {
    state = {};
    __try {
        SHQUERYRBINFO info{sizeof(info)};
        if (FAILED(SHQueryRecycleBinW(nullptr, &info))) {
            return false;
        }
        state.itemCount = static_cast<ULONGLONG>(info.i64NumItems);
        state.byteSize = static_cast<ULONGLONG>(info.i64Size);
        state.isEmpty = (info.i64NumItems <= 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RecycleBin::IsEmpty() noexcept {
    RecycleBinState state;
    if (!QueryState(state)) {
        return true;
    }
    return state.isEmpty;
}

ULONGLONG RecycleBin::ItemCount() noexcept {
    RecycleBinState state;
    if (!QueryState(state)) {
        return 0;
    }
    return state.itemCount;
}

bool RecycleBin::Open() noexcept {
    __try {
        // explorer.exe shell:RecycleBinFolder focuses / fronts the window,
        // same as double-clicking the desktop Recycle Bin icon.
        const HINSTANCE launched = ShellExecuteW(nullptr, L"open", L"explorer.exe",
            kRecycleBinFolder, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launched) > 32) {
            return true;
        }
        const HINSTANCE fallback = ShellExecuteW(
            nullptr, L"open", kRecycleBinFolder, nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(fallback) > 32;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RecycleBin::ShowProperties(HWND owner) noexcept {
    {
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.hwnd = owner;
        info.lpVerb = L"properties";
        info.lpFile = kRecycleBinClsidPath;
        info.nShow = SW_SHOW;
        info.fMask = SEE_MASK_INVOKEIDLIST;
        if (ShellExecuteExW(&info) != FALSE) {
            return true;
        }
        // Fallback: an info box with size + count (same data as Get Info).
        RecycleBinState state;
        (void)QueryState(state);
        const std::wstring text = InfoText(state);
        MessageBoxW(owner, text.c_str(), L"Recycle Bin Properties",
            MB_OK | MB_ICONINFORMATION);
        return true;
    }
}

bool RecycleBin::Empty(HWND owner, bool confirmAlreadyShown) noexcept {
    __try {
        DWORD flags = 0;
        if (confirmAlreadyShown) {
            flags |= SHERB_NOCONFIRMATION;
        }
        // SHERB_* with 0 shows the native confirmation + progress + sound.
        // nullptr root empties all drives for the current user (per-user,
        // per-drive settings respected; never touches other users).
        const HRESULT result = SHEmptyRecycleBinW(owner, nullptr, flags);
        return SUCCEEDED(result) || result == E_ABORT;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

RecycleBin::MoveResult RecycleBin::MoveToRecycleBin(
    HWND owner, const std::vector<std::wstring>& paths) noexcept {
    MoveResult result;
    if (paths.empty()) {
        result.error = L"No items to delete.";
        return result;
    }
    {
        ComPtr<IFileOperation> op;
        if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr,
                CLSCTX_ALL, IID_PPV_ARGS(&op))) ||
            op == nullptr) {
            result.error = L"The shell delete service is unavailable.";
            result.failed = paths.size();
            return result;
        }
        // ALLOWUNDO routes through the Recycle Bin; never set FOF_WANTNUKEWARNING
        // paths that would permanently delete. Keep confirmations so network /
        // unsupported locations warn instead of silently nuking.
        if (FAILED(op->SetOperationFlags(
                FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT))) {
            result.error = L"Could not configure the delete operation.";
            result.failed = paths.size();
            return result;
        }
        if (owner != nullptr) {
            (void)op->SetOwnerWindow(owner);
        }
        size_t queued = 0;
        for (const std::wstring& path : paths) {
            if (path.empty()) {
                continue;
            }
            ComPtr<IShellItem> item;
            if (FAILED(SHCreateItemFromParsingName(
                    path.c_str(), nullptr, IID_PPV_ARGS(&item))) ||
                item == nullptr) {
                ++result.failed;
                continue;
            }
            // DeleteItem + FOF_ALLOWUNDO == "move to Recycle Bin".
            if (SUCCEEDED(op->DeleteItem(item.Get(), nullptr))) {
                ++queued;
            } else {
                ++result.failed;
            }
        }
        if (queued == 0 && result.failed > 0) {
            result.error = L"None of the items could be moved to the Recycle Bin.";
            return result;
        }
        const HRESULT performed = op->PerformOperations();
        if (FAILED(performed)) {
            if (performed == COPYENGINE_E_USER_CANCELLED) {
                result.aborted = true;
                result.error = L"The operation was cancelled.";
                return result;
            }
            result.error = L"Windows could not move some items to the Recycle Bin.";
            // Best-effort counts: PerformOperations is atomic per-item; report
            // failures conservatively when the HRESULT fails.
            result.failed += queued;
            return result;
        }
        BOOL aborted = FALSE;
        if (SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && aborted) {
            result.aborted = true;
        }
        result.moved = queued;
        // Per-item failures (locked files, permissions) surface via the shell
        // progress UI; the bin state refresh below picks up partial success.
        return result;
    }
}

std::vector<std::wstring> RecycleBin::FilesFromDataObject(IDataObject* data) noexcept {
    std::vector<std::wstring> paths;
    if (data == nullptr) {
        return paths;
    }

    // Prefer IShellItemArray: covers CF_HDROP and CFSTR_SHELLIDLIST. Win11
    // Explorer often advertises the shell ID list first; CF_HDROP alone can
    // fail QueryGetData during DragEnter even when the drop is a real file.
    {
        ComPtr<IShellItemArray> items;
        if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) &&
            items != nullptr) {
            DWORD count = 0;
            if (SUCCEEDED(items->GetCount(&count)) && count > 0) {
                for (DWORD i = 0; i < count && paths.size() < 512; ++i) {
                    ComPtr<IShellItem> item;
                    if (FAILED(items->GetItemAt(i, &item)) || item == nullptr) {
                        continue;
                    }
                    PWSTR name = nullptr;
                    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || name == nullptr) {
                        continue;
                    }
                    paths.emplace_back(name);
                    CoTaskMemFree(name);
                }
                if (!paths.empty()) {
                    return paths;
                }
            }
        }
    }

    {
        FORMATETC fmt{static_cast<CLIPFORMAT>(CF_HDROP), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (FAILED(data->GetData(&fmt, &medium))) {
            return paths;
        }
        if (medium.tymed != TYMED_HGLOBAL || medium.hGlobal == nullptr) {
            ReleaseStgMedium(&medium);
            return paths;
        }
        HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
        if (drop == nullptr) {
            ReleaseStgMedium(&medium);
            return paths;
        }
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT needed = DragQueryFileW(drop, i, nullptr, 0);
            if (needed == 0) {
                continue;
            }
            std::wstring filePath(needed + 1, L'\0');
            const UINT copied = DragQueryFileW(drop, i, filePath.data(), needed + 1);
            if (copied > 0) {
                filePath.resize(copied);
                paths.push_back(std::move(filePath));
            }
            if (paths.size() >= 512) {
                break;
            }
        }
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        return paths;
    }
}

bool RecycleBin::DataObjectHasFiles(IDataObject* data) noexcept {
    if (data == nullptr) {
        return false;
    }
    {
        ComPtr<IShellItemArray> items;
        if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) &&
            items != nullptr) {
            DWORD count = 0;
            if (SUCCEEDED(items->GetCount(&count)) && count > 0) {
                return true;
            }
        }
    }
    FORMATETC fmt{static_cast<CLIPFORMAT>(CF_HDROP), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SUCCEEDED(data->QueryGetData(&fmt));
}

std::wstring RecycleBin::FormatByteSize(ULONGLONG bytes) {
    constexpr ULONGLONG kKb = 1024ULL;
    constexpr ULONGLONG kMb = 1024ULL * 1024ULL;
    constexpr ULONGLONG kGb = 1024ULL * 1024ULL * 1024ULL;
    wchar_t buffer[64]{};
    if (bytes >= kGb) {
        swprintf(buffer, std::size(buffer), L"%.1f GB",
            static_cast<double>(bytes) / static_cast<double>(kGb));
    } else if (bytes >= kMb) {
        swprintf(buffer, std::size(buffer), L"%.1f MB",
            static_cast<double>(bytes) / static_cast<double>(kMb));
    } else if (bytes >= kKb) {
        swprintf(buffer, std::size(buffer), L"%llu KB",
            static_cast<unsigned long long>(bytes / kKb));
    } else {
        swprintf(buffer, std::size(buffer), L"%llu bytes",
            static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::wstring RecycleBin::HoverLabel(const RecycleBinState& state) {
    if (state.isEmpty) {
        return L"Recycle Bin (Empty)";
    }
    std::wstringstream text;
    text << L"Recycle Bin (";
    if (state.itemCount == 1) {
        text << L"1 item";
    } else {
        text << state.itemCount << L" items";
    }
    if (state.byteSize > 0) {
        text << L", " << FormatByteSize(state.byteSize);
    }
    text << L")";
    return text.str();
}

std::wstring RecycleBin::EmptyConfirmText(const RecycleBinState& state) {
    std::wstringstream text;
    text << L"Are you sure you want to permanently delete ";
    if (state.itemCount == 0) {
        text << L"all items in the Recycle Bin?";
    } else if (state.itemCount == 1) {
        text << L"the 1 item in the Recycle Bin?";
    } else {
        text << L"these " << state.itemCount << L" items?";
    }
    if (state.byteSize > 0) {
        text << L"\n\n(" << FormatByteSize(state.byteSize) << L")";
    }
    text << L"\n\nThis cannot be undone.";
    return text.str();
}

std::wstring RecycleBin::InfoText(const RecycleBinState& state) {
    std::wstringstream text;
    text << L"Recycle Bin\n\n";
    if (state.isEmpty) {
        text << L"Empty";
    } else {
        if (state.itemCount == 1) {
            text << L"1 item";
        } else {
            text << state.itemCount << L" items";
        }
        text << L"\n" << FormatByteSize(state.byteSize);
    }
    return text.str();
}

std::vector<uint8_t> RecycleBin::IconPixels(bool full, UINT extent) noexcept {
    if (extent == 0) {
        return {};
    }
    {
        // Jumbo (256 px) first for crisp high-DPI downscale, then large icon,
        // then the Recycle Bin folder icon as a last resort.
        if (HICON jumbo = StockRecycleIcon(full, true)) {
            std::vector<uint8_t> pixels = RasterizeHicon(jumbo, extent);
            DestroyIcon(jumbo);
            if (!pixels.empty()) {
                return pixels;
            }
        }
        if (HICON large = StockRecycleIcon(full, false)) {
            std::vector<uint8_t> pixels = RasterizeHicon(large, extent);
            DestroyIcon(large);
            if (!pixels.empty()) {
                return pixels;
            }
        }
        if (HICON fallback = RecycleIconFallback()) {
            std::vector<uint8_t> pixels = RasterizeHicon(fallback, extent);
            DestroyIcon(fallback);
            if (!pixels.empty()) {
                return pixels;
            }
        }
        return {};
    }
}

const wchar_t* RecycleBin::TargetForState(bool full) noexcept {
    return full ? kFullTarget : kEmptyTarget;
}
