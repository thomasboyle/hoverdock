#include "AiAttachments.h"

#include "GroqProtocol.h"

#include <objbase.h>
#include <ShObjIdl.h>
#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <memory>
#include <thread>
#include <utility>

namespace {

constexpr size_t kMaxSourceBytes = 20U * 1024U * 1024U;
constexpr UINT kMaxSourceSide = 16384;
// Long edge sent to the model. Qwen spends ~1 input token per 28x28 px on
// every round, so 1280 keeps screenshot text legible at ~2/3 the cost of 1568.
constexpr UINT kMaxSentSide = 1280;
constexpr float kJpegQuality = 0.86F;

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        if (m_ptr != nullptr) {
            m_ptr->Release();
        }
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** Put() noexcept { return &m_ptr; }
    T* Get() const noexcept { return m_ptr; }
    T* operator->() const noexcept { return m_ptr; }

private:
    T* m_ptr = nullptr;
};

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool ReadFileBytes(const std::wstring& path, std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0 &&
        static_cast<unsigned long long>(size.QuadPart) <= kMaxSourceBytes;
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE &&
            read == bytes.size();
    }
    CloseHandle(file);
    return ok;
}

// A clipboard DIB as a .bmp file image so WIC can decode it.
std::string BmpFromDib(const uint8_t* dib, size_t size) {
    if (dib == nullptr || size < sizeof(BITMAPINFOHEADER)) {
        return {};
    }
    BITMAPINFOHEADER header{};
    std::memcpy(&header, dib, sizeof(header));
    if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biSize > size) {
        return {};
    }
    size_t offset = header.biSize;
    if (header.biCompression == BI_BITFIELDS && header.biSize == sizeof(BITMAPINFOHEADER)) {
        offset += 12;
    }
    if (header.biBitCount <= 8) {
        const size_t colors = header.biClrUsed != 0 ? header.biClrUsed : (1U << header.biBitCount);
        offset += colors * 4U;
    }
    if (offset > size) {
        return {};
    }
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;  // "BM"
    file.bfSize = static_cast<DWORD>(sizeof(file) + size);
    file.bfOffBits = static_cast<DWORD>(sizeof(file) + offset);
    std::string bytes(sizeof(file) + size, '\0');
    std::memcpy(bytes.data(), &file, sizeof(file));
    std::memcpy(bytes.data() + sizeof(file), dib, size);
    return bytes;
}

bool ClipboardBytes(UINT format, std::string& bytes) {
    HANDLE data = GetClipboardData(format);
    if (data == nullptr) {
        return false;
    }
    const SIZE_T size = GlobalSize(data);
    const void* source = GlobalLock(data);
    if (source == nullptr || size == 0 || size > kMaxSourceBytes) {
        if (source != nullptr) {
            GlobalUnlock(data);
        }
        return false;
    }
    bytes.assign(static_cast<const char*>(source), size);
    GlobalUnlock(data);
    return true;
}

bool ClipboardHasText() {
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) {
        return false;
    }
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    bool any = false;
    if (text != nullptr) {
        for (const wchar_t* c = text; *c != L'\0' && !any; ++c) {
            any = std::iswspace(static_cast<wint_t>(*c)) == 0;
        }
        GlobalUnlock(data);
    }
    return any;
}

bool Decode(IWICImagingFactory* factory, const std::string& bytes, std::vector<uint8_t>& pixels, UINT& width,
    UINT& height) {
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.Put())) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
            static_cast<DWORD>(bytes.size())))) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
            decoder.Put())) ||
        FAILED(decoder->GetFrame(0, frame.Put())) || FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0 || sourceWidth > kMaxSourceSide || sourceHeight > kMaxSourceSide) {
        return false;
    }
    const double fit = std::min(1.0, static_cast<double>(kMaxSentSide) / std::max(sourceWidth, sourceHeight));
    width = std::max(1U, static_cast<UINT>(sourceWidth * fit + 0.5));
    height = std::max(1U, static_cast<UINT>(sourceHeight * fit + 0.5));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateBitmapScaler(scaler.Put())) ||
        FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(factory->CreateFormatConverter(converter.Put())) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
            0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }
    pixels.resize(static_cast<size_t>(width) * height * 4U);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4U, static_cast<UINT>(pixels.size()), pixels.data()));
}

// Premultiplied BGRA over white -> JPEG bytes.
bool EncodeJpeg(IWICImagingFactory* factory, const std::vector<uint8_t>& pixels, UINT width, UINT height,
    std::string& jpeg) {
    const UINT stride = (width * 3U + 3U) & ~3U;
    std::vector<uint8_t> bgr(static_cast<size_t>(stride) * height, 0);
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* source = pixels.data() + static_cast<size_t>(y) * width * 4U;
        uint8_t* target = bgr.data() + static_cast<size_t>(y) * stride;
        for (UINT x = 0; x < width; ++x, source += 4, target += 3) {
            const unsigned inverse = 255U - source[3];
            target[0] = static_cast<uint8_t>(std::min(255U, source[0] + inverse));
            target[1] = static_cast<uint8_t>(std::min(255U, source[1] + inverse));
            target[2] = static_cast<uint8_t>(std::min(255U, source[2] + inverse));
        }
    }
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.Put()))) {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.Put())) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.Put(), options.Put()))) {
        return false;
    }
    PROPBAG2 option{};
    wchar_t qualityName[] = L"ImageQuality";
    option.pstrName = qualityName;
    VARIANT quality{};
    quality.vt = VT_R4;
    quality.fltVal = kJpegQuality;
    static_cast<void>(options->Write(1, &option, &quality));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->Initialize(options.Get())) || FAILED(frame->SetSize(width, height)) ||
        FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR ||
        FAILED(frame->WritePixels(height, stride, static_cast<UINT>(bgr.size()), bgr.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return false;
    }
    ULARGE_INTEGER end{};
    const LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_CUR, &end)) || end.QuadPart == 0 ||
        FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
        return false;
    }
    jpeg.resize(static_cast<size_t>(end.QuadPart));
    ULONG read = 0;
    return SUCCEEDED(stream->Read(jpeg.data(), static_cast<ULONG>(jpeg.size()), &read)) && read == jpeg.size();
}

// Centre-cropped square thumbnail with rounded corners (premultiplied).
std::vector<uint8_t> MakeThumb(const std::vector<uint8_t>& pixels, UINT width, UINT height, UINT extent) {
    std::vector<uint8_t> thumb(static_cast<size_t>(extent) * extent * 4U, 0);
    const UINT side = std::min(width, height);
    const UINT left = (width - side) / 2U;
    const UINT top = (height - side) / 2U;
    const float radius = static_cast<float>(extent) * 0.22F;
    for (UINT y = 0; y < extent; ++y) {
        const UINT y0 = top + y * side / extent;
        const UINT y1 = std::max(y0 + 1U, top + (y + 1U) * side / extent);
        for (UINT x = 0; x < extent; ++x) {
            const UINT x0 = left + x * side / extent;
            const UINT x1 = std::max(x0 + 1U, left + (x + 1U) * side / extent);
            unsigned sum[4]{};
            unsigned count = 0;
            for (UINT sy = y0; sy < y1 && sy < height; ++sy) {
                for (UINT sx = x0; sx < x1 && sx < width; ++sx) {
                    const uint8_t* p = pixels.data() + (static_cast<size_t>(sy) * width + sx) * 4U;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                    ++count;
                }
            }
            // Corner coverage of a circle of `radius` in each corner square.
            const float px = static_cast<float>(x) + 0.5F;
            const float py = static_cast<float>(y) + 0.5F;
            const float cx = std::clamp(px, radius, static_cast<float>(extent) - radius);
            const float cy = std::clamp(py, radius, static_cast<float>(extent) - radius);
            const float distance = std::hypot(px - cx, py - cy);
            const float coverage = std::clamp(radius - distance + 0.5F, 0.0F, 1.0F);
            uint8_t* out = thumb.data() + (static_cast<size_t>(y) * extent + x) * 4U;
            for (int c = 0; c < 4; ++c) {
                const float value = count == 0 ? 0.0F : static_cast<float>(sum[c]) / static_cast<float>(count);
                out[c] = static_cast<uint8_t>(std::lround(value * coverage));
            }
        }
    }
    return thumb;
}

}  // namespace

namespace AiAttachments {

bool IsImagePath(const std::wstring& path) {
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos) {
        return false;
    }
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) {
        c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    }
    static const wchar_t* const kImageExtensions[] = {L"png", L"jpg", L"jpeg", L"jfif", L"gif", L"bmp", L"webp",
        L"tif", L"tiff", L"heic", L"heif", L"ico", L"jxr", L"avif"};
    return std::any_of(std::begin(kImageExtensions), std::end(kImageExtensions),
        [&](const wchar_t* known) { return ext == known; });
}

bool ReadClipboard(HWND owner, std::vector<AiAttachmentSource>& out) {
    if (OpenClipboard(owner) == FALSE) {
        return false;
    }
    bool found = false;
    if (IsClipboardFormatAvailable(CF_HDROP) != FALSE) {
        if (auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(length + 1U, L'\0');
                path.resize(DragQueryFileW(drop, i, path.data(), length + 1U));
                if (IsImagePath(path)) {
                    out.push_back({FileName(path), path, {}});
                    found = true;
                }
            }
        }
    }
    if (!found && !ClipboardHasText()) {
        const UINT png = RegisterClipboardFormatW(L"PNG");
        AiAttachmentSource source;
        source.name = L"Pasted image";
        if (png != 0 && IsClipboardFormatAvailable(png) != FALSE && ClipboardBytes(png, source.bytes)) {
            found = true;
        } else {
            // CF_DIB first: CF_DIBV5 screenshots often carry a zero alpha channel.
            for (const UINT format : {static_cast<UINT>(CF_DIB), static_cast<UINT>(CF_DIBV5)}) {
                std::string dib;
                if (IsClipboardFormatAvailable(format) != FALSE && ClipboardBytes(format, dib)) {
                    source.bytes = BmpFromDib(reinterpret_cast<const uint8_t*>(dib.data()), dib.size());
                    if (!source.bytes.empty()) {
                        found = true;
                        break;
                    }
                }
            }
        }
        if (found) {
            out.push_back(std::move(source));
        }
    }
    CloseClipboard();
    return found;
}

std::vector<std::wstring> PickFiles(HWND owner) {
    std::vector<std::wstring> paths;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.Put())))) {
        return paths;
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"Images", L"*.png;*.jpg;*.jpeg;*.jfif;*.gif;*.bmp;*.webp;*.tif;*.tiff;*.heic;*.heif;*.avif"},
        {L"All files", L"*.*"},
    };
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetFileTypes(2, filters);
    dialog->SetTitle(L"Attach images for AI");
    if (FAILED(dialog->Show(owner))) {
        return paths;
    }
    ComPtr<IShellItemArray> items;
    DWORD count = 0;
    if (FAILED(dialog->GetResults(items.Put())) || FAILED(items->GetCount(&count))) {
        return paths;
    }
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, item.Put())) &&
            SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
            paths.emplace_back(path);
        }
        CoTaskMemFree(path);
    }
    return paths;
}

void PrepareAsync(HWND reply, UINT message, uint64_t id, AiAttachmentSource source, UINT thumbExtent) {
    std::thread([reply, message, id, source = std::move(source), thumbExtent]() mutable {
        auto result = std::make_unique<AiAttachmentReply>();
        result->id = id;
        result->name = source.name;
        result->thumbExtent = std::max(1U, thumbExtent);
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            if (source.bytes.empty() && !ReadFileBytes(source.path, source.bytes)) {
                result->error = L"Couldn't read \u201C" + source.name + L"\u201D (images up to 20 MB).";
            } else {
                ComPtr<IWICImagingFactory> factory;
                std::vector<uint8_t> pixels;
                UINT width = 0;
                UINT height = 0;
                std::string jpeg;
                if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(factory.Put()))) ||
                    !Decode(factory.Get(), source.bytes, pixels, width, height)) {
                    result->error = L"\u201C" + source.name + L"\u201D isn't an image Windows can open.";
                } else if (!EncodeJpeg(factory.Get(), pixels, width, height, jpeg)) {
                    result->error = L"Couldn't prepare \u201C" + source.name + L"\u201D.";
                } else {
                    result->dataUrl = groq::DataUrl("image/jpeg", jpeg);
                    result->thumb = MakeThumb(pixels, width, height, result->thumbExtent);
                }
            }
        } catch (...) {
            result->dataUrl.clear();
            result->error = L"Couldn't prepare \u201C" + source.name + L"\u201D.";
        }
        if (SUCCEEDED(init)) {
            CoUninitialize();
        }
        AiAttachmentReply* raw = result.release();
        if (PostMessageW(reply, message, 0, reinterpret_cast<LPARAM>(raw)) == FALSE) {
            delete raw;
        }
    }).detach();
}

}  // namespace AiAttachments
