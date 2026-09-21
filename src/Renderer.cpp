#include "Renderer.h"
#include "DockTheme.hlsli"
#include "Profile.h"

#include <Shellapi.h>
#include <ShObjIdl.h>
#include <ShlObj.h>
#include <CommonControls.h>
#include <dwmapi.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "glass_ps_60.h"
#include "glass_ps_66.h"
#include "glass_vs_60.h"
#include "glass_vs_66.h"
#include "icon_ps_60.h"
#include "icon_ps_66.h"
#include "icon_vs_60.h"
#include "icon_vs_66.h"
#include "blur_ps_60.h"
#include "blur_ps_66.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kStartTarget[] = L"dock:start";
constexpr wchar_t kSearchTarget[] = L"dock:search";

UINT16 TextureArraySize(UINT arraySize) {
    return static_cast<UINT16>(std::min(arraySize, static_cast<UINT>(UINT16_MAX)));
}

class ComApartment {
public:
    ComApartment()
        : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
    }

    ~ComApartment() {
        if (m_result == S_OK) {
            CoUninitialize();
        }
    }

private:
    HRESULT m_result = E_FAIL;
};

[[noreturn]] void ThrowFailure(HRESULT result, const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<unsigned long>(result)) + ")");
}

void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        ThrowFailure(result, operation);
    }
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferDescription(UINT64 size) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;
    return description;
}

D3D12_RESOURCE_DESC TextureDescription(UINT width, UINT height, UINT16 arraySize = 1) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = arraySize;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;
    return description;
}

D3D12_RASTERIZER_DESC RasterizerDescription() {
    D3D12_RASTERIZER_DESC description{};
    description.FillMode = D3D12_FILL_MODE_SOLID;
    description.CullMode = D3D12_CULL_MODE_NONE;
    description.FrontCounterClockwise = FALSE;
    description.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.DepthClipEnable = TRUE;
    description.MultisampleEnable = FALSE;
    description.AntialiasedLineEnable = FALSE;
    description.ForcedSampleCount = 0;
    description.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return description;
}

D3D12_BLEND_DESC PremultipliedBlendDescription() {
    D3D12_BLEND_DESC description{};
    description.AlphaToCoverageEnable = FALSE;
    description.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& target = description.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.LogicOpEnable = FALSE;
    target.SrcBlend = D3D12_BLEND_ONE;
    target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return description;
}

UINT IconAtlasExtent(UINT displayExtent) {
    return std::min(256U, std::max(1U, displayExtent * 2U));
}

UINT IconSourceExtent(UINT displayExtent) {
    return std::min(256U, std::max(128U, IconAtlasExtent(displayExtent)));
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

void ClearTransparentRgb(std::vector<uint8_t>& pixels) {
    for (size_t index = 0; index + 3 < pixels.size(); index += 4) {
        if (pixels[index + 3] == 0) {
            pixels[index] = 0;
            pixels[index + 1] = 0;
            pixels[index + 2] = 0;
        }
    }
}

bool HasAnyNonZeroAlpha(const std::vector<uint8_t>& pixels) {
    for (size_t index = 3; index < pixels.size(); index += 4) {
        if (pixels[index] != 0) {
            return true;
        }
    }
    return false;
}

bool LooksPremultiplied(const std::vector<uint8_t>& pixels) {
    size_t partial = 0;
    size_t exceeds = 0;
    for (size_t index = 0; index + 3 < pixels.size(); index += 4) {
        const unsigned alpha = pixels[index + 3];
        if (alpha == 0 || alpha == 255) {
            continue;
        }
        ++partial;
        if (pixels[index] > alpha || pixels[index + 1] > alpha || pixels[index + 2] > alpha) {
            ++exceeds;
        }
    }
    return partial == 0 || exceeds * 4 <= partial;
}

void ConvertToPremultiplied(std::vector<uint8_t>& pixels) {
    if (!LooksPremultiplied(pixels)) {
        for (size_t index = 0; index + 3 < pixels.size(); index += 4) {
            const unsigned alpha = pixels[index + 3];
            pixels[index] = static_cast<uint8_t>(pixels[index] * alpha / 255U);
            pixels[index + 1] = static_cast<uint8_t>(pixels[index + 1] * alpha / 255U);
            pixels[index + 2] = static_cast<uint8_t>(pixels[index + 2] * alpha / 255U);
        }
    }
    ClearTransparentRgb(pixels);
}

bool CopyBitmapPixels(HBITMAP bitmap, std::vector<uint8_t>& out, UINT& width, UINT& height) {
    BITMAP info{};
    if (GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight == 0) {
        return false;
    }

    width = static_cast<UINT>(info.bmWidth);
    height = static_cast<UINT>(std::abs(info.bmHeight));
    out.resize(static_cast<size_t>(width) * height * 4U);

    if (info.bmBitsPixel == 32 && info.bmBits != nullptr) {
        const size_t stride = static_cast<size_t>(info.bmWidthBytes);
        const auto* src = static_cast<const uint8_t*>(info.bmBits);
        const bool bottomUp = info.bmHeight > 0;
        for (UINT y = 0; y < height; ++y) {
            const UINT srcY = bottomUp ? (height - 1U - y) : y;
            std::memcpy(out.data() + static_cast<size_t>(y) * width * 4U, src + srcY * stride,
                static_cast<size_t>(width) * 4U);
        }
        return true;
    }

    BITMAPV5HEADER header = IconBitmapHeader(width, height);
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        return false;
    }
    const int lines = GetDIBits(dc, bitmap, 0, height, out.data(), reinterpret_cast<BITMAPINFO*>(&header),
        DIB_RGB_COLORS);
    DeleteDC(dc);
    return lines > 0;
}

std::vector<uint8_t> ScalePremultipliedPixels(const std::vector<uint8_t>& source, UINT sourceWidth,
    UINT sourceHeight, UINT extent) {
    if (source.empty() || sourceWidth == 0 || sourceHeight == 0 || extent == 0) {
        return {};
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) ||
        factory == nullptr) {
        return {};
    }

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(sourceWidth, sourceHeight, GUID_WICPixelFormat32bppPBGRA,
            sourceWidth * 4U, static_cast<UINT>(source.size()), const_cast<BYTE*>(source.data()),
            &bitmap))) {
        return {};
    }

    const UINT maxDim = std::max(sourceWidth, sourceHeight);
    const UINT destWidth = std::max(1U, extent * sourceWidth / maxDim);
    const UINT destHeight = std::max(1U, extent * sourceHeight / maxDim);
    ComPtr<IWICBitmapSource> sized = bitmap;
    ComPtr<IWICBitmapScaler> scaler;
    if (destWidth != sourceWidth || destHeight != sourceHeight) {
        if (FAILED(factory->CreateBitmapScaler(&scaler))) {
            return {};
        }
        const WICBitmapInterpolationMode filter =
            destWidth < sourceWidth || destHeight < sourceHeight
            ? WICBitmapInterpolationModeFant
            : WICBitmapInterpolationModeHighQualityCubic;
        if (FAILED(scaler->Initialize(bitmap.Get(), destWidth, destHeight, filter))) {
            return {};
        }
        sized = scaler;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(sized.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return {};
    }

    std::vector<uint8_t> fitted(static_cast<size_t>(destWidth) * destHeight * 4U);
    if (FAILED(converter->CopyPixels(nullptr, destWidth * 4U, static_cast<UINT>(fitted.size()),
            fitted.data()))) {
        return {};
    }
    if (destWidth == extent && destHeight == extent) {
        return fitted;
    }

    std::vector<uint8_t> canvas(static_cast<size_t>(extent) * extent * 4U, 0);
    const UINT offsetX = (extent - destWidth) / 2U;
    const UINT offsetY = (extent - destHeight) / 2U;
    for (UINT y = 0; y < destHeight; ++y) {
        std::memcpy(canvas.data() + ((offsetY + y) * extent + offsetX) * 4U,
            fitted.data() + static_cast<size_t>(y) * destWidth * 4U,
            static_cast<size_t>(destWidth) * 4U);
    }
    return canvas;
}

std::vector<uint8_t> RasterizeIconGdi(HICON icon, UINT extent) {
    BITMAPV5HEADER header = IconBitmapHeader(extent, extent);
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return {};
    }
    HDC memory = CreateCompatibleDC(screen);
    void* bitmapBits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS,
        &bitmapBits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr || bitmap == nullptr || bitmapBits == nullptr) {
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
    std::memset(bitmapBits, 0, static_cast<size_t>(extent) * static_cast<size_t>(extent) * 4U);
    const BOOL drawn = DrawIconEx(memory, 0, 0, icon, static_cast<int>(extent), static_cast<int>(extent),
        0, nullptr, DI_NORMAL);

    std::vector<uint8_t> pixels;
    if (drawn != FALSE) {
        pixels.resize(static_cast<size_t>(extent) * static_cast<size_t>(extent) * 4U);
        std::memcpy(pixels.data(), bitmapBits, pixels.size());
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    if (pixels.empty()) {
        return {};
    }

    if (!HasAnyNonZeroAlpha(pixels)) {
        ICONINFO info{};
        if (GetIconInfo(icon, &info) != FALSE) {
            if (info.hbmMask != nullptr) {
                UINT maskWidth = 0;
                UINT maskHeight = 0;
                std::vector<uint8_t> maskPixels;
                if (CopyBitmapPixels(info.hbmMask, maskPixels, maskWidth, maskHeight) && maskWidth > 0 &&
                    maskHeight > 0) {
                    const UINT maskRows = info.hbmColor == nullptr ? maskHeight / 2U : maskHeight;
                    for (UINT y = 0; y < extent; ++y) {
                        const UINT maskY = std::min(maskRows - 1U, y * maskRows / extent);
                        for (UINT x = 0; x < extent; ++x) {
                            const UINT maskX = std::min(maskWidth - 1U, x * maskWidth / extent);
                            const size_t maskOffset = (static_cast<size_t>(maskY) * maskWidth + maskX) * 4U;
                            const bool transparent =
                                (maskPixels[maskOffset] | maskPixels[maskOffset + 1] |
                                    maskPixels[maskOffset + 2]) != 0;
                            const size_t offset = (static_cast<size_t>(y) * extent + x) * 4U;
                            if (transparent) {
                                pixels[offset] = 0;
                                pixels[offset + 1] = 0;
                                pixels[offset + 2] = 0;
                                pixels[offset + 3] = 0;
                            } else {
                                pixels[offset + 3] = 255;
                            }
                        }
                    }
                }
            }
            if (info.hbmColor != nullptr) {
                DeleteObject(info.hbmColor);
            }
            if (info.hbmMask != nullptr) {
                DeleteObject(info.hbmMask);
            }
        }
    }

    if (!HasAnyNonZeroAlpha(pixels)) {
        return {};
    }
    ConvertToPremultiplied(pixels);
    return pixels;
}

std::vector<uint8_t> RasterizeIcon(HICON icon, UINT extent) {
    if (icon == nullptr) {
        return {};
    }

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
                        std::vector<uint8_t> scaled = ScalePremultipliedPixels(source, width, height, extent);
                        if (!scaled.empty()) {
                            return scaled;
                        }
                    }
                }
            }
        }
    }

    return RasterizeIconGdi(icon, extent);
}

std::vector<uint8_t> RasterizeBitmap(HBITMAP sourceBitmap, UINT extent) {
    if (sourceBitmap == nullptr) {
        return {};
    }

    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> source;
    if (!CopyBitmapPixels(sourceBitmap, source, width, height) || !HasAnyNonZeroAlpha(source)) {
        return {};
    }
    ConvertToPremultiplied(source);
    return ScalePremultipliedPixels(source, width, height, extent);
}

void SetGlyphPixel(std::vector<uint8_t>& pixels, UINT extent, int x, int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(extent) || y >= static_cast<int>(extent)) {
        return;
    }
    // Icon atlas buffers are RGB-ordered: byte0=R, byte1=G, byte2=B.
    const size_t offset = (static_cast<size_t>(y) * extent + static_cast<UINT>(x)) * 4U;
    pixels[offset] = DOCK_INK_R;
    pixels[offset + 1] = DOCK_INK_G;
    pixels[offset + 2] = DOCK_INK_B;
    pixels[offset + 3] = 255;
}

void FillGlyphRectangle(std::vector<uint8_t>& pixels, UINT extent, int left, int top, int width,
    int height) {
    for (int y = top; y < top + height; ++y) {
        for (int x = left; x < left + width; ++x) {
            SetGlyphPixel(pixels, extent, x, y);
        }
    }
}

std::vector<uint8_t> CreateDockGlyph(const std::wstring& target, UINT extent) {
    if (target != kStartTarget && target != kSearchTarget) {
        return {};
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(extent) * static_cast<size_t>(extent) * 4U, 0);
    if (target == kStartTarget) {
        const int cell = std::max(2, static_cast<int>(extent) * 20 / 100);
        const int gap = std::max(1, static_cast<int>(extent) * 6 / 100);
        const int total = cell * 2 + gap;
        const int left = (static_cast<int>(extent) - total) / 2;
        const int top = (static_cast<int>(extent) - total) / 2;
        FillGlyphRectangle(pixels, extent, left, top, cell, cell);
        FillGlyphRectangle(pixels, extent, left + cell + gap, top, cell, cell);
        FillGlyphRectangle(pixels, extent, left, top + cell + gap, cell, cell);
        FillGlyphRectangle(pixels, extent, left + cell + gap, top + cell + gap, cell, cell);
        return pixels;
    }

    const float center = static_cast<float>(extent) * 0.42F;
    const float radius = static_cast<float>(extent) * 0.20F;
    const float ringWidth = std::max(1.0F, static_cast<float>(extent) * 0.045F);
    const float handleStart = radius * 0.62F;
    const float handleEnd = radius + static_cast<float>(extent) * 0.19F;
    for (int y = 0; y < static_cast<int>(extent); ++y) {
        for (int x = 0; x < static_cast<int>(extent); ++x) {
            const float horizontal = static_cast<float>(x) - center;
            const float vertical = static_cast<float>(y) - center;
            const float distance = std::sqrt(horizontal * horizontal + vertical * vertical);
            const float alongHandle = (horizontal + vertical) * 0.70710678F;
            const float acrossHandle = std::abs(horizontal - vertical) * 0.70710678F;
            if (std::abs(distance - radius) <= ringWidth ||
                (alongHandle >= handleStart && alongHandle <= handleEnd && acrossHandle <= ringWidth)) {
                SetGlyphPixel(pixels, extent, x, y);
            }
        }
    }
    return pixels;
}

HBITMAP SafeShellImageBitmap(const wchar_t* target, UINT sourceExtent, SIIGBF flags) noexcept {
    // SEH guard: shell image extraction runs third-party handler code
    // in-process (notably on the background refresh worker triggered after a
    // click). A corrupt handler must not take the dock down. This helper uses
    // only POD/raw pointers so it can legally use __try (C2712); references
    // intentionally leak on a fault and the caller falls through.
    __try {
        IShellItem* item = nullptr;
        if (FAILED(SHCreateItemFromParsingName(target, nullptr, IID_PPV_ARGS(&item))) ||
            item == nullptr) {
            return nullptr;
        }
        IShellItemImageFactory* factory = nullptr;
        const HRESULT queried = item->QueryInterface(IID_PPV_ARGS(&factory));
        item->Release();
        if (FAILED(queried) || factory == nullptr) {
            return nullptr;
        }
        SIZE size{static_cast<LONG>(sourceExtent), static_cast<LONG>(sourceExtent)};
        HBITMAP bitmap = nullptr;
        const HRESULT imaged = factory->GetImage(size, flags, &bitmap);
        factory->Release();
        if (FAILED(imaged) || bitmap == nullptr) {
            return nullptr;
        }
        return bitmap;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

HICON SafeSystemImageListIcon(int iconIndex) noexcept {
    __try {
        IImageList* imageList = nullptr;
        if (FAILED(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&imageList)))) {
            if (FAILED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&imageList)))) {
                return nullptr;
            }
        }
        if (imageList == nullptr) {
            return nullptr;
        }
        HICON icon = nullptr;
        const HRESULT got = imageList->GetIcon(iconIndex, ILD_TRANSPARENT, &icon);
        imageList->Release();
        if (FAILED(got) || icon == nullptr) {
            return nullptr;
        }
        return icon;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::vector<uint8_t> ExtractShellItemImage(const std::wstring& target, UINT sourceExtent,
    UINT displayExtent, SIIGBF flags) {
    HBITMAP bitmap = SafeShellImageBitmap(target.c_str(), sourceExtent, flags);
    if (bitmap == nullptr) {
        return {};
    }
    const std::vector<uint8_t> pixels = RasterizeBitmap(bitmap, displayExtent);
    DeleteObject(bitmap);
    return pixels;
}

std::vector<uint8_t> ExtractShellItemImage(const std::wstring& target, UINT sourceExtent,
    UINT displayExtent) {
    const std::array<SIIGBF, 2> flagSets = {
        static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK),
        static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK),
    };
    for (const SIIGBF flags : flagSets) {
        const std::vector<uint8_t> pixels = ExtractShellItemImage(target, sourceExtent, displayExtent, flags);
        if (!pixels.empty()) {
            return pixels;
        }
    }
    return {};
}

DWORD_PTR SafeShellFileInfo(const wchar_t* target, DWORD fileAttributes,
    SHFILEINFOW* information, UINT flags) noexcept {
    // Shell icon lookup executes third-party handler code in-process; a corrupt
    // handler must not take the dock down. SHFILEINFOW is POD, so it can cross
    // the SEH boundary safely. Overlays are deliberately never requested: the
    // overlay manager has faulted here before, and the overlay index is never
    // consumed (only hIcon is rasterized).
    __try {
        return SHGetFileInfoW(target, fileAttributes, information, sizeof(SHFILEINFOW), flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::vector<uint8_t> ExtractSystemImageListIcon(const std::wstring& target, UINT displayExtent) {
    SHFILEINFOW information{};
    if (SafeShellFileInfo(target.c_str(), FILE_ATTRIBUTE_NORMAL, &information,
            SHGFI_SYSICONINDEX) == 0) {
        return {};
    }

    HICON icon = SafeSystemImageListIcon(information.iIcon);
    if (icon == nullptr) {
        return {};
    }
    const std::vector<uint8_t> pixels = RasterizeIcon(icon, displayExtent);
    DestroyIcon(icon);
    return pixels;
}

std::vector<uint8_t> ExtractExecutableIcon(const std::wstring& target, UINT sourceExtent,
    UINT displayExtent) {
    if (target.empty() || target.rfind(L"shell:", 0) == 0) {
        return {};
    }

    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {};
    }

    const int sourceSize = static_cast<int>(sourceExtent);
    HICON icon = nullptr;
    UINT iconId = 0;
    if (PrivateExtractIconsW(target.c_str(), 0, sourceSize, sourceSize, &icon, &iconId, 1,
            LR_DEFAULTCOLOR) > 0 &&
        icon != nullptr) {
        const std::vector<uint8_t> pixels = RasterizeIcon(icon, displayExtent);
        DestroyIcon(icon);
        if (!pixels.empty()) {
            return pixels;
        }
    }

    HICON largeIcon = nullptr;
    const UINT extracted = ExtractIconExW(target.c_str(), 0, &largeIcon, nullptr, 1);
    if (extracted > 0 && largeIcon != nullptr) {
        const std::vector<uint8_t> pixels = RasterizeIcon(largeIcon, displayExtent);
        DestroyIcon(largeIcon);
        if (!pixels.empty()) {
            return pixels;
        }
    }

    return {};
}

std::vector<uint8_t> ExtractShellFallbackIcon(const std::wstring& target, UINT displayExtent) {
    SHFILEINFOW information{};
    if (SafeShellFileInfo(target.c_str(), FILE_ATTRIBUTE_NORMAL, &information,
            SHGFI_ICON | SHGFI_LARGEICON) == 0 || information.hIcon == nullptr) {
        return {};
    }
    const std::vector<uint8_t> pixels = RasterizeIcon(information.hIcon, displayExtent);
    DestroyIcon(information.hIcon);
    return pixels;
}

std::vector<uint8_t> ExtractIconPixelsImpl(const std::wstring& target, UINT sourceExtent,
    UINT displayExtent) {
    ComApartment apartment;
    std::vector<uint8_t> pixels = CreateDockGlyph(target, displayExtent);
    if (!pixels.empty()) {
        return pixels;
    }
    pixels = ExtractShellItemImage(target, sourceExtent, displayExtent);
    if (!pixels.empty()) {
        return pixels;
    }
    pixels = ExtractSystemImageListIcon(target, displayExtent);
    if (!pixels.empty()) {
        return pixels;
    }
    pixels = ExtractExecutableIcon(target, sourceExtent, displayExtent);
    if (!pixels.empty()) {
        return pixels;
    }
    pixels = ExtractShellFallbackIcon(target, displayExtent);
    if (!pixels.empty()) {
        return pixels;
    }

    SHFILEINFOW information{};
    if (SafeShellFileInfo(target.c_str(), FILE_ATTRIBUTE_NORMAL, &information,
            SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES) != 0 &&
        information.hIcon != nullptr) {
        pixels = RasterizeIcon(information.hIcon, displayExtent);
        DestroyIcon(information.hIcon);
    }
    return pixels;
}

std::vector<uint8_t> ExtractIconPixelsFromCandidates(const std::vector<std::wstring>& candidates,
    UINT sourceExtent, UINT displayExtent) {
    for (const std::wstring& candidate : candidates) {
        const std::vector<uint8_t> pixels = ExtractIconPixelsImpl(candidate, sourceExtent, displayExtent);
        if (!pixels.empty()) {
            return pixels;
        }
    }
    return {};
}

const D3D12_SHADER_BYTECODE Shader(const unsigned char* bytes, size_t size) {
    return {bytes, size};
}

}  // namespace

Renderer::~Renderer() {
    try {
        Flush();
    } catch (...) {
    }
    ReleaseBackdropResources();
    if (m_frameLatencyWaitableObject != nullptr) {
        CloseHandle(m_frameLatencyWaitableObject);
    }
    if (m_fenceEvent != nullptr) {
        CloseHandle(m_fenceEvent);
    }
}

void Renderer::Initialize(HWND window, UINT width, UINT height) {
    m_window = window;
    m_width = std::max(width, 1U);
    m_height = std::max(height, 1U);
    m_dpiScale = static_cast<float>(std::max(GetDpiForWindow(window), 96U)) / 96.0F;
    CreateDevice();
    CreateCompositionSwapChain(window, m_width, m_height);
    CreateFrameResources();
    CreateRootSignatureAndPipelines();
    CreateRenderTargets();
    CreateBackdropResources();
}

void Renderer::Resize(UINT width, UINT height) {
    width = std::max(width, 1U);
    height = std::max(height, 1U);
    if (m_width == width && m_height == height) {
        return;
    }

    Flush();
    for (ComPtr<ID3D12Resource>& backBuffer : m_backBuffers) {
        backBuffer.Reset();
    }
    ReleaseBackdropResources();

    const UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    Check(m_swapChain->ResizeBuffers(kBufferCount, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, flags),
        "IDXGISwapChain::ResizeBuffers");
    m_width = width;
    m_height = height;
    m_dpiScale = static_cast<float>(std::max(GetDpiForWindow(m_window), 96U)) / 96.0F;
    CreateRenderTargets();
    CreateBackdropResources();
}

void Renderer::LoadIcons(const std::vector<std::wstring>& cacheKeys,
    const std::vector<std::vector<std::wstring>>& iconCandidates, UINT iconPixelExtent) {
    static_assert(kMaximumIcons <= UINT16_MAX,
        "The maximum icon count must fit in a D3D12 texture array.");

    if (cacheKeys.size() != iconCandidates.size()) {
        throw std::runtime_error("Icon cache keys do not match candidate lists.");
    }
    if (cacheKeys.size() > kMaximumIcons) {
        throw std::runtime_error("The dock supports at most 512 visible icons.");
    }

    const UINT displayExtent = std::max(1U, iconPixelExtent);
    m_iconPixelExtent = IconAtlasExtent(displayExtent);
    const UINT sourceExtent = IconSourceExtent(displayExtent);
    std::vector<std::vector<uint8_t>> pixelBuffers;
    pixelBuffers.reserve(cacheKeys.size());
    for (const std::vector<std::wstring>& candidates : iconCandidates) {
        pixelBuffers.push_back(
            ExtractIconPixelsFromCandidates(candidates, sourceExtent, m_iconPixelExtent));
    }
    UploadIcons(cacheKeys, pixelBuffers);
}

void Renderer::UploadIcons(const std::vector<std::wstring>& targets,
    const std::vector<std::vector<uint8_t>>& pixelBuffers) {
    static_assert(kMaximumIcons <= UINT16_MAX,
        "The maximum icon count must fit in a D3D12 texture array.");

    if (targets.size() != pixelBuffers.size()) {
        throw std::runtime_error("Icon upload buffers do not match targets.");
    }
    if (targets.size() > kMaximumIcons) {
        throw std::runtime_error("The dock supports at most 512 visible icons.");
    }

    m_iconTextureByTarget.clear();
    m_iconPixelCache.clear();
    for (UINT index = 0; index < targets.size(); ++index) {
        m_iconTextureByTarget[targets[index]] = index;
        if (!pixelBuffers[index].empty()) {
            m_iconPixelCache[targets[index]] = pixelBuffers[index];
        }
    }
    RebuildIconAtlasFromCache();
}

void Renderer::RebuildIconAtlasFromCache() {
    m_iconCount = m_iconTextureByTarget.empty()
        ? 1U
        : static_cast<UINT>(m_iconTextureByTarget.size());

    ComApartment apartment;
    WaitForAllFrames();
    Flush();
    m_iconAtlas.Reset();
    m_pendingUploads.clear();
    const UINT displayExtent = m_iconPixelExtent;

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    Check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "Create icon upload allocator");
    Check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
              IID_PPV_ARGS(&commandList)),
        "Create icon upload command list");

    const D3D12_RESOURCE_DESC atlas = TextureDescription(displayExtent, displayExtent, TextureArraySize(m_iconCount));
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    Check(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &atlas,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_iconAtlas)),
        "Create icon texture array");

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.ArraySize = m_iconCount;
    D3D12_CPU_DESCRIPTOR_HANDLE iconDescriptor = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    iconDescriptor.ptr += static_cast<SIZE_T>(kIconTextureDescriptor) * m_srvDescriptorSize;
    m_device->CreateShaderResourceView(m_iconAtlas.Get(), &view, iconDescriptor);

    if (m_iconTextureByTarget.empty()) {
        CreateFallbackIcon(0, commandList.Get());
    } else {
        std::vector<std::pair<UINT, std::wstring>> slots;
        slots.reserve(m_iconTextureByTarget.size());
        for (const auto& entry : m_iconTextureByTarget) {
            slots.emplace_back(entry.second, entry.first);
        }
        std::ranges::sort(slots, {}, &std::pair<UINT, std::wstring>::first);
        for (const auto& [slot, target] : slots) {
            const auto cached = m_iconPixelCache.find(target);
            if (cached != m_iconPixelCache.end() && !cached->second.empty()) {
                UploadIconTexture(slot, cached->second.data(), displayExtent, displayExtent, commandList.Get());
            } else {
                CreateFallbackIcon(slot, commandList.Get());
            }
        }
    }

    D3D12_RESOURCE_BARRIER atlasBarrier{};
    atlasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    atlasBarrier.Transition.pResource = m_iconAtlas.Get();
    atlasBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    atlasBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    atlasBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &atlasBarrier);

    Check(commandList->Close(), "Close icon upload command list");
    ID3D12CommandList* lists[] = {commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    Flush();
    m_pendingUploads.clear();
}

void Renderer::AppendMissingIcons(const std::vector<std::wstring>& targets,
    const std::vector<std::vector<uint8_t>>& pixelBuffers) {
    if (targets.size() != pixelBuffers.size()) {
        throw std::runtime_error("Append icon upload buffers do not match targets.");
    }

    bool added = false;
    for (size_t index = 0; index < targets.size(); ++index) {
        if (m_iconPixelCache.contains(targets[index])) {
            continue;
        }
        if (m_iconCount >= kMaximumIcons) {
            throw std::runtime_error("The dock supports at most 512 visible icons.");
        }
        const UINT slot = m_iconCount++;
        m_iconPixelCache.emplace(targets[index], pixelBuffers[index]);
        m_iconTextureByTarget[targets[index]] = slot;
        added = true;
    }
    if (added) {
        RebuildIconAtlasFromCache();
    }
}

void Renderer::UpdateCachedIcons(const std::vector<std::wstring>& targets,
    const std::vector<std::vector<uint8_t>>& pixelBuffers) {
    if (targets.size() != pixelBuffers.size() || m_device == nullptr || m_iconAtlas == nullptr) {
        return;
    }

    std::vector<std::wstring> missingTargets;
    std::vector<std::vector<uint8_t>> missingPixels;
    std::vector<UINT> replaceSlots;
    missingTargets.reserve(targets.size());
    missingPixels.reserve(targets.size());
    replaceSlots.reserve(targets.size());

    for (size_t index = 0; index < targets.size(); ++index) {
        const auto found = m_iconTextureByTarget.find(targets[index]);
        if (found == m_iconTextureByTarget.end()) {
            missingTargets.push_back(targets[index]);
            missingPixels.push_back(pixelBuffers[index]);
        } else {
            m_iconPixelCache[targets[index]] = pixelBuffers[index];
            replaceSlots.push_back(found->second);
        }
    }

    if (!missingTargets.empty()) {
        AppendMissingIcons(missingTargets, missingPixels);
        return;
    }
    if (replaceSlots.empty()) {
        return;
    }

    WaitForAllFrames();
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    Check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "Create tray icon update allocator");
    Check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
              IID_PPV_ARGS(&commandList)),
        "Create tray icon update command list");

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = m_iconAtlas.Get();
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commandList->ResourceBarrier(1, &toCopy);

    const UINT displayExtent = m_iconPixelExtent;
    for (size_t index = 0; index < targets.size(); ++index) {
        const auto found = m_iconTextureByTarget.find(targets[index]);
        if (found == m_iconTextureByTarget.end() || pixelBuffers[index].empty()) {
            continue;
        }
        UploadIconTexture(found->second, pixelBuffers[index].data(), displayExtent, displayExtent,
            commandList.Get());
    }

    std::swap(toCopy.Transition.StateBefore, toCopy.Transition.StateAfter);
    commandList->ResourceBarrier(1, &toCopy);
    Check(commandList->Close(), "Close tray icon update command list");
    ID3D12CommandList* lists[] = {commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    Flush();
    m_pendingUploads.clear();
}

UINT Renderer::IconAtlasPixelExtent() const noexcept {
    return std::max(1U, m_iconPixelExtent);
}

std::vector<std::wstring> Renderer::IconTargetKeys() const {
    std::vector<std::wstring> keys;
    keys.reserve(m_iconTextureByTarget.size());
    for (const auto& entry : m_iconTextureByTarget) {
        keys.push_back(entry.first);
    }
    return keys;
}

bool Renderer::BackdropValid() const noexcept {
    return m_backdropValid;
}

UINT Renderer::TextureIndexForTarget(const std::wstring& target) const noexcept {
    const auto found = m_iconTextureByTarget.find(target);
    return found != m_iconTextureByTarget.end() ? found->second : 0;
}

bool Renderer::HasIconForTarget(const std::wstring& target) const noexcept {
    return m_iconTextureByTarget.contains(target);
}

bool Renderer::HasCachedIconPixels(const std::wstring& target) const noexcept {
    const auto found = m_iconPixelCache.find(target);
    return found != m_iconPixelCache.end() && !found->second.empty();
}

const std::vector<uint8_t>* Renderer::CachedIconPixels(const std::wstring& target) const noexcept {
    const auto found = m_iconPixelCache.find(target);
    if (found == m_iconPixelCache.end() || found->second.empty()) {
        return nullptr;
    }
    return &found->second;
}

std::vector<uint8_t> Renderer::ExtractIconPixels(const std::vector<std::wstring>& candidates,
    UINT iconPixelExtent) {
    const UINT displayExtent = std::max(1U, iconPixelExtent);
    return ExtractIconPixelsFromCandidates(candidates, IconSourceExtent(displayExtent),
        IconAtlasExtent(displayExtent));
}

bool Renderer::NeedsBackdropBitBlt(const RECT& screenRectangle) const noexcept {
    const LONG width = screenRectangle.right - screenRectangle.left;
    const LONG height = screenRectangle.bottom - screenRectangle.top;
    if (m_backdropDc == nullptr || m_backdropDibPixels == nullptr || m_backdropTexture == nullptr ||
        width != static_cast<LONG>(m_width) || height != static_cast<LONG>(m_height)) {
        return false;
    }
    if (!m_backdropValid) {
        return true;
    }
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing))) {
        if (m_backdropDwmFrameValid && timing.cFrame == m_backdropDwmFrame) {
            return false;
        }
    }
    return true;
}

namespace {

// Reads the desktop into a memory DC while hiding one window from legacy GDI
// capture. POD-only so __try is legal here (C2712): __finally restores the
// affinity even on fault, so an exception can never leave the dock stuck
// invisible to Snipping Tool / Game Bar (which a naive toggle did before).
BOOL BitBltDesktopExcluding(HWND exclude, HDC destDc, LONG width, LONG height, HDC screen,
    LONG left, LONG top) noexcept {
    DWORD previousAffinity = WDA_NONE;
    const BOOL affinityRead = GetWindowDisplayAffinity(exclude, &previousAffinity) != FALSE;
    BOOL copied = FALSE;
    __try {
        if (affinityRead != FALSE) {
            SetWindowDisplayAffinity(exclude, WDA_EXCLUDEFROMCAPTURE);
        }
        copied = BitBlt(destDc, 0, 0, width, height, screen, left, top, SRCCOPY);
    } __finally {
        if (affinityRead != FALSE) {
            SetWindowDisplayAffinity(exclude, previousAffinity);
        }
    }
    return copied;
}

}  // namespace

bool Renderer::CaptureBackdrop(const RECT& screenRectangle, bool* changed) {
    ProfileScope scope("Renderer::CaptureBackdrop");
    const LONG width = screenRectangle.right - screenRectangle.left;
    const LONG height = screenRectangle.bottom - screenRectangle.top;
    if (m_backdropDc == nullptr || m_backdropDibPixels == nullptr || m_backdropTexture == nullptr ||
        width != static_cast<LONG>(m_width) || height != static_cast<LONG>(m_height)) {
        return false;
    }

    // Idle fast path: any pixel change behind the dock requires a DWM composition,
    // which advances cFrame. When DWM hasn't composed since the last capture and a
    // valid backdrop already exists, the pixels cannot have changed: skip the
    // BitBlt + hash entirely. The 8 ms timer still fires (FPS preserved); idle ticks
    // cost one DWM query (~0.01 ms) instead of a screen readback (measured 2-10 ms).
    // When content moves or video plays, cFrame advances every vsync and captures
    // continue at the full rate, so the glass stays pixel-identical to always-capture.
    // Bound: cFrame can also stay frozen while the cache is stale (black frames
    // validated before first composition at logon/resume, then a static desktop),
    // so at most kBackdropForcedCaptureSkips ticks pass before a live BitBlt.
    if (m_backdropValid) {
        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(timing);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing))) {
            if (m_backdropDwmFrameValid && timing.cFrame == m_backdropDwmFrame) {
                if (++m_backdropIdleSkips < kBackdropForcedCaptureSkips) {
                    if (changed != nullptr) {
                        *changed = false;
                    }
                    return true;
                }
            }
            m_backdropDwmFrame = timing.cFrame;
            m_backdropDwmFrameValid = true;
        }
        // DWM timing unavailable (composition off): fall through to BitBlt.
    } else {
        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(timing);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing))) {
            m_backdropDwmFrame = timing.cFrame;
            m_backdropDwmFrameValid = true;
        }
    }

    // Any BitBlt attempt (forced or regular) resets the idle-skip budget,
    // whether the readback succeeds or not: persistent failure retries at
    // the forced cadence instead of every tick.
    m_backdropIdleSkips = 0;

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return false;
    }

    // SRCCOPY only (CAPTUREBLT forces sync composition of layered windows and
    // measured 2-10 ms per 8 ms tick; layered content under the dock is rare).
    // The render window is excluded for exactly this BitBlt: despite
    // WS_EX_NOREDIRECTIONBITMAP its swapchain otherwise leaks into the legacy
    // GDI surface, and captured icons refract back through the glass as
    // ghost smears. Affinity is restored in __finally (see helper above).
    const BOOL copied = BitBltDesktopExcluding(m_window, m_backdropDc, width, height, screen,
        screenRectangle.left, screenRectangle.top);
    const int released = ReleaseDC(nullptr, screen);
    if (copied == FALSE || released == 0) {
        return false;
    }

    const uint64_t hash = HashBackdropPixels();
    if (m_backdropValid && hash == m_backdropHash) {
        if (changed != nullptr) {
            *changed = false;
        }
        return true;
    }

    {
        ProfileScope uploadScope("Renderer::UploadBackdropPixels");
        if (!UploadBackdropPixels()) {
            // GPU saturated: keep the previous backdrop; the next capture tick
            // retries instead of stalling the UI thread.
            ProfileScope::Mark("Renderer::SkipBackdropUpload");
            return false;
        }
    }
    m_backdropHash = hash;
    m_backdropValid = true;
    if (changed != nullptr) {
        *changed = true;
    }
    return true;
}

bool Renderer::Render(const DockRenderState& state) {
    ProfileScope scope("Renderer::Render");
    if (state.width == 0 || state.height == 0) {
        return false;
    }

    if (m_frameLatencyWaitableObject != nullptr) {
        // Never block: a signaled waitable returns immediately (~us) and an
        // unsignaled one means the GPU is a frame behind, in which case waiting
        // here only stalls input on the UI thread. Pacing is preserved by the
        // blocking Present below; the fence check after this still guards
        // resource hazards. Keeps this scope at microseconds max.
        const DWORD timeout = 0;
        DWORD waitResult = WAIT_OBJECT_0;
        {
            ProfileScope waitScope("Renderer::WaitLatency");
            waitResult = WaitForSingleObject(m_frameLatencyWaitableObject, timeout);
        }
        if (waitResult == WAIT_TIMEOUT && state.skipIfGpuBusy) {
            ProfileScope::Mark("Renderer::SkipWaitable");
            return false;
        }
    }

    const UINT frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    FrameResource& frame = m_frames[frameIndex];
    if (frame.fenceValue != 0 && m_fence->GetCompletedValue() < frame.fenceValue) {
        if (state.skipIfGpuBusy) {
            ProfileScope::Mark("Renderer::SkipFence");
            return false;
        }
        // Bounded even for blocking frames: under GPU saturation the UI thread
        // must keep pumping input. Animations are time-based, so a dropped
        // frame merely skips ahead instead of wedging the loop.
        static constexpr DWORD kFrameFenceWaitMs = 50;
        if (!WaitForFrame(frame, kFrameFenceWaitMs)) {
            ProfileScope::Mark("Renderer::DropContention");
            return false;
        }
    }

    Check(frame.allocator->Reset(), "Reset frame allocator");
    Check(m_commandList->Reset(frame.allocator.Get(), nullptr), "Reset command list");

    frame.mappedConstants->scene0[0] = static_cast<float>(state.width);
    frame.mappedConstants->scene0[1] = static_cast<float>(state.height);
    frame.mappedConstants->scene0[2] = state.glassAlpha;
    frame.mappedConstants->scene0[3] = state.dockScale;
    frame.mappedConstants->scene1[0] = static_cast<float>(state.fxFlags);
    frame.mappedConstants->scene1[1] = m_dpiScale;
    frame.mappedConstants->scene1[2] = state.showDevBounds ? 1.0F : 0.0F;
    frame.mappedConstants->scene1[3] = m_backdropValid ? 1.0F : 0.0F;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_backBuffers[frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE renderTarget = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    renderTarget.ptr += static_cast<SIZE_T>(frameIndex) * m_rtvDescriptorSize;
    m_commandList->OMSetRenderTargets(1, &renderTarget, FALSE, nullptr);
    constexpr float transparent[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    m_commandList->ClearRenderTargetView(renderTarget, transparent, 0, nullptr);

    D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(state.width),
        static_cast<float>(state.height), 0.0F, 1.0F};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(state.width), static_cast<LONG>(state.height)};
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, frame.constants->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRootShaderResourceView(1, frame.iconInstances->GetGPUVirtualAddress());
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetGraphicsRootDescriptorTable(2,
        m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Separable frost: pass 1 blurs horizontally into the temp target when
    // frost is on and the backdrop is live; the glass pass below finishes
    // vertically from temp. Skipped otherwise (stale temp is never sampled:
    // GlassPS takes the direct path when frost is off or invalid).
    const bool frostPass =
        (state.fxFlags & DOCK_FX_BLUR) != 0 && m_backdropValid && m_blurTemp != nullptr;
    if (frostPass) {
        if (m_blurTempIsShaderResource) {
            D3D12_RESOURCE_BARRIER toRender{};
            toRender.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRender.Transition.pResource = m_blurTemp.Get();
            toRender.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toRender.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            toRender.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            m_commandList->ResourceBarrier(1, &toRender);
            m_blurTempIsShaderResource = false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE blurTarget = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        blurTarget.ptr += static_cast<SIZE_T>(kBufferCount) * m_rtvDescriptorSize;
        m_commandList->OMSetRenderTargets(1, &blurTarget, FALSE, nullptr);
        m_commandList->SetPipelineState(m_blurPipeline.Get());
        m_commandList->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER toShader{};
        toShader.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toShader.Transition.pResource = m_blurTemp.Get();
        toShader.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toShader.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toShader.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_commandList->ResourceBarrier(1, &toShader);
        m_blurTempIsShaderResource = true;

        m_commandList->OMSetRenderTargets(1, &renderTarget, FALSE, nullptr);
    }

    m_commandList->SetPipelineState(m_glassPipeline.Get());
    m_commandList->DrawInstanced(3, 1, 0, 0);

    m_commandList->SetPipelineState(m_iconPipeline.Get());
    const UINT iconCount = std::min(static_cast<UINT>(state.icons.size()), kMaximumIcons);
    for (UINT index = 0; index < iconCount; ++index) {
        const DockIconRenderData& icon = state.icons[index];
        IconInstanceConstants& instance = frame.mappedIcons[index];
        instance.iconRect[0] = static_cast<float>(icon.bounds.left);
        instance.iconRect[1] = static_cast<float>(icon.bounds.top);
        instance.iconRect[2] = static_cast<float>(icon.bounds.right - icon.bounds.left);
        instance.iconRect[3] = static_cast<float>(icon.bounds.bottom - icon.bounds.top);
        instance.iconMeta[0] = icon.running ? 1.0F : 0.0F;
        float mode = 0.0F;
        switch (icon.kind) {
        case DockIconKind::Divider:
            mode = 2.0F;
            break;
        case DockIconKind::TrayDivider:
            mode = 2.0F;
            break;
        case DockIconKind::Clock:
            mode = icon.pressed ? 5.0F : 4.0F;
            break;
        default:
            mode = icon.pressed ? 1.0F : 0.0F;
            break;
        }
        instance.iconMeta[1] = mode;
        instance.iconMeta[2] = icon.dragged ? 1.0F : 0.0F;
        const UINT safeIconCount = std::max(m_iconCount, 1U);
        instance.iconMeta[3] = static_cast<float>(std::min(icon.textureIndex, safeIconCount - 1));
    }
    if (iconCount > 0) {
        m_commandList->DrawInstanced(6, iconCount, 0, 0);
    }

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    m_commandList->ResourceBarrier(1, &barrier);
    Check(m_commandList->Close(), "Close command list");
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    {
        ProfileScope presentScope("Renderer::Present");
        Check(m_swapChain->Present(state.allowBlockingGpuWait ? 1 : 0, 0), "Present composition frame");
    }
    SignalFrame(frame);
    return true;
}

void Renderer::Flush() {
    ProfileScope scope("Renderer::Flush");
    if (m_queue == nullptr || m_fence == nullptr || m_fenceEvent == nullptr) {
        return;
    }

    const UINT64 value = ++m_fenceValue;
    Check(m_queue->Signal(m_fence.Get(), value), "Signal GPU flush fence");
    if (m_fence->GetCompletedValue() < value) {
        Check(m_fence->SetEventOnCompletion(value, m_fenceEvent), "Set GPU flush fence event");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

HANDLE Renderer::FrameLatencyWaitableObject() const noexcept {
    return m_frameLatencyWaitableObject;
}

D3D_FEATURE_LEVEL Renderer::FeatureLevel() const noexcept {
    return m_featureLevel;
}

D3D_SHADER_MODEL Renderer::ShaderModel() const noexcept {
    return m_shaderModel;
}

void Renderer::CreateDevice() {
    ComPtr<IDXGIFactory2> factory;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    Check(factory.As(&m_factory), "Query IDXGIFactory6");

    constexpr std::array featureLevels = {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
    };
    HRESULT lastError = E_FAIL;
    for (const D3D_FEATURE_LEVEL level : featureLevels) {
        ComPtr<ID3D12Device> device;
        const HRESULT result = D3D12CreateDevice(nullptr, level, IID_PPV_ARGS(&device));
        if (SUCCEEDED(result)) {
            m_device = device;
            m_featureLevel = level;
            break;
        }
        lastError = result;
    }
    if (m_device == nullptr) {
        ThrowFailure(lastError, "D3D12CreateDevice for feature levels 12_2, 12_1, or 12_0");
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_6};
    const HRESULT shaderQuery = m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
        &shaderModel, sizeof(shaderModel));
    if (FAILED(shaderQuery) || shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_0) {
        throw std::runtime_error("This GPU driver does not expose Shader Model 6.0.");
    }
    m_shaderModel = shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_6
        ? D3D_SHADER_MODEL_6_6
        : D3D_SHADER_MODEL_6_0;

    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue.NodeMask = 0;
    Check(m_device->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_queue)), "Create direct queue");
    Check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "Create fence");
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr) {
        throw std::runtime_error("CreateEventW for D3D12 fence failed.");
    }
}

void Renderer::CreateCompositionSwapChain(HWND window, UINT width, UINT height) {
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.Stereo = FALSE;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kBufferCount;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swapChain;
    Check(m_factory->CreateSwapChainForComposition(m_queue.Get(), &description, nullptr, &swapChain),
        "Create flip-discard composition swap chain");
    Check(swapChain.As(&m_swapChain), "Query IDXGISwapChain3");

    ComPtr<IDXGISwapChain2> swapChain2;
    Check(m_swapChain.As(&swapChain2), "Query IDXGISwapChain2");
    Check(swapChain2->SetMaximumFrameLatency(1), "Set maximum frame latency");
    m_frameLatencyWaitableObject = swapChain2->GetFrameLatencyWaitableObject();
    if (m_frameLatencyWaitableObject == nullptr) {
        throw std::runtime_error("Composition swap chain did not provide a frame latency waitable object.");
    }

    Check(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&m_compositionDevice)),
        "DCompositionCreateDevice");
    Check(m_compositionDevice->CreateTargetForHwnd(window, TRUE, &m_compositionTarget),
        "Create DirectComposition target");
    Check(m_compositionDevice->CreateVisual(&m_compositionVisual), "Create DirectComposition visual");
    Check(m_compositionVisual->SetContent(m_swapChain.Get()), "Set composition visual swap chain");
    Check(m_compositionTarget->SetRoot(m_compositionVisual.Get()), "Set composition root");
    Check(m_compositionDevice->Commit(), "Commit DirectComposition tree");
}

void Renderer::CreateFrameResources() {
    for (FrameResource& frame : m_frames) {
        Check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                  IID_PPV_ARGS(&frame.allocator)),
            "Create frame allocator");
        const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC constantBuffer = BufferDescription(sizeof(FrameConstants));
        Check(m_device->CreateCommittedResource(&uploadHeap,
                  D3D12_HEAP_FLAG_NONE, &constantBuffer,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&frame.constants)),
            "Create frame constant buffer");
        Check(frame.constants->Map(0, nullptr, reinterpret_cast<void**>(&frame.mappedConstants)),
            "Map frame constant buffer");

        const D3D12_RESOURCE_DESC instanceBuffer =
            BufferDescription(sizeof(IconInstanceConstants) * kMaximumIcons);
        Check(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &instanceBuffer,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&frame.iconInstances)),
            "Create frame icon instance buffer");
        Check(frame.iconInstances->Map(0, nullptr, reinterpret_cast<void**>(&frame.mappedIcons)),
            "Map frame icon instance buffer");
    }

    Check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator.Get(),
              nullptr, IID_PPV_ARGS(&m_commandList)),
        "Create reusable command list");
    Check(m_commandList->Close(), "Close initial command list");
}

void Renderer::CreateRootSignatureAndPipelines() {
    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 3;
    range.BaseShaderRegister = 1;
    range.RegisterSpace = 0;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    range.OffsetInDescriptorsFromTableStart = 0;

    std::array<D3D12_ROOT_PARAMETER1, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].Descriptor.RegisterSpace = 0;
    parameters[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].Descriptor.ShaderRegister = 0;
    parameters[1].Descriptor.RegisterSpace = 0;
    parameters[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;

    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &range;

    D3D12_STATIC_SAMPLER_DESC linearSampler{};
    linearSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.MipLODBias = 0.0F;
    linearSampler.MaxAnisotropy = 1;
    linearSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    linearSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    linearSampler.MinLOD = 0.0F;
    linearSampler.MaxLOD = D3D12_FLOAT32_MAX;
    linearSampler.ShaderRegister = 0;
    linearSampler.RegisterSpace = 0;
    linearSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC pointSampler = linearSampler;
    pointSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    pointSampler.ShaderRegister = 1;

    const std::array samplers = {linearSampler, pointSampler};

    D3D12_ROOT_SIGNATURE_DESC1 root{};
    root.NumParameters = static_cast<UINT>(parameters.size());
    root.pParameters = parameters.data();
    root.NumStaticSamplers = static_cast<UINT>(samplers.size());
    root.pStaticSamplers = samplers.data();
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned.Desc_1_1 = root;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    const HRESULT serializedResult = D3D12SerializeVersionedRootSignature(&versioned, &serialized, &errors);
    if (FAILED(serializedResult)) {
        ThrowFailure(serializedResult, "Serialize D3D12 root signature");
    }
    Check(m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
              IID_PPV_ARGS(&m_rootSignature)),
        "Create root signature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = m_rootSignature.Get();
    pipeline.BlendState = PremultipliedBlendDescription();
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState = RasterizerDescription();
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.StencilEnable = FALSE;
    pipeline.InputLayout = {nullptr, 0};
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pipeline.SampleDesc.Count = 1;

    if (m_shaderModel == D3D_SHADER_MODEL_6_6) {
        pipeline.VS = Shader(gGlassVs66, sizeof(gGlassVs66));
        pipeline.PS = Shader(gGlassPs66, sizeof(gGlassPs66));
    } else {
        pipeline.VS = Shader(gGlassVs60, sizeof(gGlassVs60));
        pipeline.PS = Shader(gGlassPs60, sizeof(gGlassPs60));
    }
    Check(m_device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_glassPipeline)),
        "Create glass pipeline");

    // Separable frost pass 1: horizontal Gaussian into the temp target.
    // Blending disabled (full overdraw each frame); same root signature.
    pipeline.BlendState = D3D12_BLEND_DESC{};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (m_shaderModel == D3D_SHADER_MODEL_6_6) {
        pipeline.VS = Shader(gGlassVs66, sizeof(gGlassVs66));
        pipeline.PS = Shader(gBlurPs66, sizeof(gBlurPs66));
    } else {
        pipeline.VS = Shader(gGlassVs60, sizeof(gGlassVs60));
        pipeline.PS = Shader(gBlurPs60, sizeof(gBlurPs60));
    }
    Check(m_device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_blurPipeline)),
        "Create blur pipeline");
    pipeline.BlendState = PremultipliedBlendDescription();

    if (m_shaderModel == D3D_SHADER_MODEL_6_6) {
        pipeline.VS = Shader(gIconVs66, sizeof(gIconVs66));
        pipeline.PS = Shader(gIconPs66, sizeof(gIconPs66));
    } else {
        pipeline.VS = Shader(gIconVs60, sizeof(gIconVs60));
        pipeline.PS = Shader(gIconPs60, sizeof(gIconPs60));
    }
    Check(m_device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_iconPipeline)),
        "Create icon pipeline");

    D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
    descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptors.NumDescriptors = 3;
    descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(m_device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&m_srvHeap)),
        "Create shader resource descriptor heap");
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(descriptors.Type);
}

void Renderer::CreateRenderTargets() {
    if (m_rtvHeap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
        descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        descriptors.NumDescriptors = kBufferCount + 1;
        descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Check(m_device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&m_rtvHeap)),
            "Create render target descriptor heap");
        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(descriptors.Type);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < kBufferCount; ++index) {
        Check(m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_backBuffers[index])),
            "Get swap chain buffer");
        m_device->CreateRenderTargetView(m_backBuffers[index].Get(), nullptr, handle);
        handle.ptr += m_rtvDescriptorSize;
    }
}

void Renderer::CreateBackdropResources() {
    ReleaseBackdropResources();

    try {
        HDC screen = GetDC(nullptr);
        if (screen == nullptr) {
            throw std::runtime_error("GetDC for desktop backdrop failed.");
        }

        m_backdropDc = CreateCompatibleDC(screen);
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(m_width);
        bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(m_height);
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_backdropBitmap = CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
        const int released = ReleaseDC(nullptr, screen);
        if (released == 0) {
            throw std::runtime_error("ReleaseDC for desktop backdrop failed.");
        }
        if (m_backdropDc == nullptr || m_backdropBitmap == nullptr || bits == nullptr) {
            throw std::runtime_error("Create desktop backdrop DIB failed.");
        }

        m_backdropDibPixels = static_cast<uint8_t*>(bits);
        m_backdropPreviousBitmap = SelectObject(m_backdropDc, m_backdropBitmap);
        if (m_backdropPreviousBitmap == nullptr || m_backdropPreviousBitmap == HGDI_ERROR) {
            throw std::runtime_error("Select desktop backdrop DIB failed.");
        }
        std::memset(m_backdropDibPixels, 0,
            static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4U);

        const D3D12_RESOURCE_DESC texture = TextureDescription(m_width, m_height);
        const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        Check(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texture,
                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_backdropTexture)),
            "Create desktop backdrop texture");

        UINT64 rowSize = 0;
        UINT64 uploadSize = 0;
        m_device->GetCopyableFootprints(&texture, 0, 1, 0, &m_backdropFootprint,
            &m_backdropRowCount, &rowSize, &uploadSize);
        const D3D12_RESOURCE_DESC upload = BufferDescription(uploadSize);
        const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        Check(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &upload,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_backdropUpload)),
            "Create desktop backdrop upload resource");
        Check(m_backdropUpload->Map(0, nullptr,
                  reinterpret_cast<void**>(&m_backdropUploadPixels)),
            "Map desktop backdrop upload resource");

        Check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                  IID_PPV_ARGS(&m_backdropCopyAllocator)),
            "Create desktop backdrop command allocator");
        Check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                  m_backdropCopyAllocator.Get(), nullptr,
                  IID_PPV_ARGS(&m_backdropCopyCommandList)),
            "Create desktop backdrop command list");
        Check(m_backdropCopyCommandList->Close(), "Close initial desktop backdrop command list");

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE backdropDescriptor =
            m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        backdropDescriptor.ptr += static_cast<SIZE_T>(kBackdropTextureDescriptor) *
            m_srvDescriptorSize;
        m_device->CreateShaderResourceView(m_backdropTexture.Get(), &view, backdropDescriptor);

        // Separable-frost temp target (pass 1 horizontal blur lands here).
        // Same size/format as the backdrop; recreated on every resize. Starts
        // life as a render target (see m_blurTempIsShaderResource).
        D3D12_RESOURCE_DESC blurDesc = TextureDescription(m_width, m_height);
        blurDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const D3D12_HEAP_PROPERTIES blurHeapProps = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        Check(m_device->CreateCommittedResource(&blurHeapProps, D3D12_HEAP_FLAG_NONE, &blurDesc,
                  D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&m_blurTemp)),
            "Create frost blur temp texture");

        D3D12_CPU_DESCRIPTOR_HANDLE blurRtv =
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        blurRtv.ptr += static_cast<SIZE_T>(kBufferCount) * m_rtvDescriptorSize;
        m_device->CreateRenderTargetView(m_blurTemp.Get(), nullptr, blurRtv);

        D3D12_SHADER_RESOURCE_VIEW_DESC blurView{};
        blurView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        blurView.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        blurView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        blurView.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE blurSrv =
            m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        blurSrv.ptr += static_cast<SIZE_T>(kTempBlurDescriptor) * m_srvDescriptorSize;
        m_device->CreateShaderResourceView(m_blurTemp.Get(), &blurView, blurSrv);
        m_blurTempIsShaderResource = false;

        // Best effort: under GPU saturation the upload defers to the first
        // capture tick instead of stalling setup.
        static_cast<void>(UploadBackdropPixels());
    } catch (...) {
        ReleaseBackdropResources();
        throw;
    }
}

void Renderer::ReleaseBackdropResources() noexcept {
    if (m_backdropUpload != nullptr && m_backdropUploadPixels != nullptr) {
        m_backdropUpload->Unmap(0, nullptr);
    }
    m_backdropUploadPixels = nullptr;
    m_backdropCopyCommandList.Reset();
    m_backdropCopyAllocator.Reset();
    m_backdropUpload.Reset();
    m_backdropTexture.Reset();
    m_blurTemp.Reset();
    m_blurTempIsShaderResource = false;
    m_backdropFootprint = {};
    m_backdropRowCount = 0;
    m_backdropInitialized = false;
    m_backdropValid = false;
    m_backdropHash = 0;
    m_backdropDwmFrame = 0;
    m_backdropDwmFrameValid = false;
    m_backdropIdleSkips = 0;

    if (m_backdropDc != nullptr && m_backdropPreviousBitmap != nullptr &&
        m_backdropPreviousBitmap != HGDI_ERROR) {
        SelectObject(m_backdropDc, m_backdropPreviousBitmap);
    }
    m_backdropPreviousBitmap = nullptr;
    if (m_backdropBitmap != nullptr) {
        DeleteObject(m_backdropBitmap);
    }
    m_backdropBitmap = nullptr;
    m_backdropDibPixels = nullptr;
    if (m_backdropDc != nullptr) {
        DeleteDC(m_backdropDc);
    }
    m_backdropDc = nullptr;
}

uint64_t Renderer::HashBackdropPixels() const noexcept {
    if (m_backdropDibPixels == nullptr || m_width == 0 || m_height == 0) {
        return 0;
    }

    const uint32_t* words = reinterpret_cast<const uint32_t*>(m_backdropDibPixels);
    const size_t count = static_cast<size_t>(m_width) * static_cast<size_t>(m_height);
    uint64_t hash = 14695981039346656037ull;
    constexpr size_t stride = 8;
    for (size_t index = 0; index < count; index += stride) {
        hash ^= words[index];
        hash *= 1099511628211ull;
    }
    hash ^= words[count - 1];
    hash ^= static_cast<uint64_t>(m_width) << 32;
    hash ^= m_height;
    return hash;
}

bool Renderer::UploadBackdropPixels() {
    // Bounded: the UI thread must keep pumping even when the GPU is saturated;
    // a skipped upload simply retries on the next capture tick.
    static constexpr DWORD kCopyWaitMs = 50;
    if (!WaitForBackdropCopy(kCopyWaitMs)) {
        return false;
    }
    const size_t rowBytes = static_cast<size_t>(m_width) * 4U;
    for (UINT row = 0; row < m_backdropRowCount; ++row) {
        std::memcpy(m_backdropUploadPixels + m_backdropFootprint.Offset +
                static_cast<size_t>(row) * m_backdropFootprint.Footprint.RowPitch,
            m_backdropDibPixels + static_cast<size_t>(row) * rowBytes, rowBytes);
    }

    Check(m_backdropCopyAllocator->Reset(), "Reset desktop backdrop command allocator");
    Check(m_backdropCopyCommandList->Reset(m_backdropCopyAllocator.Get(), nullptr),
        "Reset desktop backdrop command list");

    if (m_backdropInitialized) {
        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = m_backdropTexture.Get();
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        m_backdropCopyCommandList->ResourceBarrier(1, &toCopy);
    }

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = m_backdropUpload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = m_backdropFootprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = m_backdropTexture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    m_backdropCopyCommandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    D3D12_RESOURCE_BARRIER toShader{};
    toShader.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toShader.Transition.pResource = m_backdropTexture.Get();
    toShader.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toShader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toShader.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_backdropCopyCommandList->ResourceBarrier(1, &toShader);

    Check(m_backdropCopyCommandList->Close(), "Close desktop backdrop command list");
    ID3D12CommandList* lists[] = {m_backdropCopyCommandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    m_backdropCopyFenceValue = ++m_fenceValue;
    Check(m_queue->Signal(m_fence.Get(), m_backdropCopyFenceValue), "Signal backdrop copy fence");
    m_backdropInitialized = true;
    return true;
}

bool Renderer::WaitForBackdropCopy(DWORD timeoutMs) {
    if (m_backdropCopyFenceValue == 0 || m_fence->GetCompletedValue() >= m_backdropCopyFenceValue) {
        return true;
    }
    Check(m_fence->SetEventOnCompletion(m_backdropCopyFenceValue, m_fenceEvent),
        "Set backdrop copy fence event");
    return WaitForSingleObject(m_fenceEvent, timeoutMs) == WAIT_OBJECT_0;
}

bool Renderer::WaitForFrame(FrameResource& frame, DWORD timeoutMs) {
    ProfileScope scope("Renderer::WaitForFrame");
    if (frame.fenceValue == 0 || m_fence->GetCompletedValue() >= frame.fenceValue) {
        return true;
    }

    Check(m_fence->SetEventOnCompletion(frame.fenceValue, m_fenceEvent), "Set frame fence event");
    return WaitForSingleObject(m_fenceEvent, timeoutMs) == WAIT_OBJECT_0;
}

void Renderer::WaitForAllFrames() {
    ProfileScope scope("Renderer::WaitForAllFrames");
    for (FrameResource& frame : m_frames) {
        static_cast<void>(WaitForFrame(frame));
    }
}

void Renderer::UploadIcon(UINT textureIndex, const std::wstring& target,
    ID3D12GraphicsCommandList* commandList) {
    const UINT atlasExtent = m_iconPixelExtent;
    const UINT sourceExtent = IconSourceExtent(std::max(1U, atlasExtent / 2U));
    const std::vector<uint8_t> pixels = ExtractIconPixelsImpl(target, sourceExtent, atlasExtent);
    if (pixels.empty()) {
        CreateFallbackIcon(textureIndex, commandList);
        return;
    }
    UploadIconTexture(textureIndex, pixels.data(), atlasExtent, atlasExtent, commandList);
}

void Renderer::CreateFallbackIcon(UINT textureIndex, ID3D12GraphicsCommandList* commandList) {
    const UINT displayExtent = m_iconPixelExtent;
    std::vector<uint8_t> pixels;
    if (HICON icon = LoadIconW(nullptr, IDI_APPLICATION)) {
        pixels = RasterizeIcon(icon, displayExtent);
    }
    if (pixels.empty()) {
        pixels.assign(static_cast<size_t>(displayExtent) * static_cast<size_t>(displayExtent) * 4U, 0);
        const int margin = static_cast<int>(displayExtent) / 5;
        for (int y = margin; y < static_cast<int>(displayExtent) - margin; ++y) {
            for (int x = margin; x < static_cast<int>(displayExtent) - margin; ++x) {
                const size_t offset = (static_cast<size_t>(y) * displayExtent + static_cast<UINT>(x)) * 4U;
                pixels[offset] = 180;
                pixels[offset + 1] = 188;
                pixels[offset + 2] = 204;
                pixels[offset + 3] = 255;
            }
        }
    }
    UploadIconTexture(textureIndex, pixels.data(), displayExtent, displayExtent, commandList);
}

void Renderer::UploadIconTexture(UINT textureIndex, const uint8_t* pixels, UINT width, UINT height,
    ID3D12GraphicsCommandList* commandList) {
    const D3D12_RESOURCE_DESC texture = TextureDescription(width, height, TextureArraySize(m_iconCount));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 rowSize = 0;
    UINT64 uploadSize = 0;
    m_device->GetCopyableFootprints(&texture, textureIndex, 1, 0, &footprint, &rowCount, &rowSize,
        &uploadSize);

    ComPtr<ID3D12Resource> upload;
    const D3D12_RESOURCE_DESC buffer = BufferDescription(uploadSize);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    Check(m_device->CreateCommittedResource(&uploadHeap,
              D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
              IID_PPV_ARGS(&upload)),
        "Create icon upload buffer");

    uint8_t* mapped = nullptr;
    Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map icon upload buffer");
    for (UINT row = 0; row < rowCount; ++row) {
        std::memcpy(mapped + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
            pixels + static_cast<size_t>(row) * width * 4U, static_cast<size_t>(rowSize));
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = m_iconAtlas.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = textureIndex;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    m_pendingUploads.push_back(std::move(upload));
}

void Renderer::SignalFrame(FrameResource& frame) {
    frame.fenceValue = ++m_fenceValue;
    Check(m_queue->Signal(m_fence.Get(), frame.fenceValue), "Signal frame fence");
}
