#include "Renderer.h"

#include <Shellapi.h>
#include <ShObjIdl.h>

#include <algorithm>
#include <array>
#include <cmath>
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
        if (SUCCEEDED(m_result)) {
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

UINT IconTextureExtent(HWND window) {
    const UINT dpi = window == nullptr ? 96U : std::max(GetDpiForWindow(window), 96U);
    return std::max(96U, static_cast<UINT>(std::lround(static_cast<float>(dpi))));
}

void NormalizeAlpha(std::vector<uint8_t>& pixels) {
    bool hasAlpha = false;
    for (size_t index = 3; index < pixels.size(); index += 4) {
        hasAlpha = hasAlpha || pixels[index] != 0;
    }
    if (!hasAlpha) {
        for (size_t index = 0; index < pixels.size(); index += 4) {
            pixels[index + 3] = (pixels[index] | pixels[index + 1] | pixels[index + 2]) == 0 ? 0 : 255;
        }
    }
}

std::vector<uint8_t> RasterizeIcon(HICON icon, UINT extent) {
    if (icon == nullptr) {
        return {};
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(extent);
    header.bV5Height = -static_cast<LONG>(extent);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_RGB;

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
        NormalizeAlpha(pixels);
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    return pixels;
}

std::vector<uint8_t> RasterizeBitmap(HBITMAP sourceBitmap, UINT extent) {
    if (sourceBitmap == nullptr) {
        return {};
    }

    BITMAP sourceInfo{};
    if (GetObjectW(sourceBitmap, sizeof(sourceInfo), &sourceInfo) == 0 || sourceInfo.bmWidth <= 0 ||
        sourceInfo.bmHeight == 0) {
        return {};
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(extent);
    header.bV5Height = -static_cast<LONG>(extent);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_RGB;

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return {};
    }
    HDC source = CreateCompatibleDC(screen);
    HDC destination = CreateCompatibleDC(screen);
    void* destinationBits = nullptr;
    HBITMAP destinationBitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &destinationBits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (source == nullptr || destination == nullptr || destinationBitmap == nullptr ||
        destinationBits == nullptr) {
        if (source != nullptr) {
            DeleteDC(source);
        }
        if (destination != nullptr) {
            DeleteDC(destination);
        }
        if (destinationBitmap != nullptr) {
            DeleteObject(destinationBitmap);
        }
        return {};
    }

    const HGDIOBJ previousSource = SelectObject(source, sourceBitmap);
    const HGDIOBJ previousDestination = SelectObject(destination, destinationBitmap);
    if (previousSource == nullptr || previousSource == HGDI_ERROR || previousDestination == nullptr ||
        previousDestination == HGDI_ERROR) {
        if (previousSource != nullptr && previousSource != HGDI_ERROR) {
            SelectObject(source, previousSource);
        }
        if (previousDestination != nullptr && previousDestination != HGDI_ERROR) {
            SelectObject(destination, previousDestination);
        }
        DeleteObject(destinationBitmap);
        DeleteDC(destination);
        DeleteDC(source);
        return {};
    }

    const int sourceWidth = sourceInfo.bmWidth;
    const int sourceHeight = std::abs(sourceInfo.bmHeight);
    const int maximumDimension = std::max(sourceWidth, sourceHeight);
    const int destinationWidth = std::max(1, static_cast<int>(extent) * sourceWidth / maximumDimension);
    const int destinationHeight = std::max(1, static_cast<int>(extent) * sourceHeight / maximumDimension);
    const int destinationX = (static_cast<int>(extent) - destinationWidth) / 2;
    const int destinationY = (static_cast<int>(extent) - destinationHeight) / 2;
    std::memset(destinationBits, 0, static_cast<size_t>(extent) * static_cast<size_t>(extent) * 4U);
    SetStretchBltMode(destination, HALFTONE);
    const BOOL copied = StretchBlt(destination, destinationX, destinationY, destinationWidth,
        destinationHeight, source, 0, 0, sourceWidth, sourceHeight, SRCCOPY);

    std::vector<uint8_t> pixels;
    if (copied != FALSE) {
        pixels.resize(static_cast<size_t>(extent) * static_cast<size_t>(extent) * 4U);
        std::memcpy(pixels.data(), destinationBits, pixels.size());
        NormalizeAlpha(pixels);
    }
    SelectObject(source, previousSource);
    SelectObject(destination, previousDestination);
    DeleteObject(destinationBitmap);
    DeleteDC(destination);
    DeleteDC(source);
    return pixels;
}

void SetGlyphPixel(std::vector<uint8_t>& pixels, UINT extent, int x, int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(extent) || y >= static_cast<int>(extent)) {
        return;
    }
    const size_t offset = (static_cast<size_t>(y) * extent + static_cast<UINT>(x)) * 4U;
    pixels[offset] = 255;
    pixels[offset + 1] = 235;
    pixels[offset + 2] = 220;
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

std::vector<uint8_t> ExtractShellItemImage(const std::wstring& target, UINT extent) {
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return {};
    }

    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(item.As(&factory))) {
        return {};
    }
    SIZE size{static_cast<LONG>(extent), static_cast<LONG>(extent)};
    HBITMAP bitmap = nullptr;
    const HRESULT result = factory->GetImage(size,
        static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK), &bitmap);
    if (FAILED(result) || bitmap == nullptr) {
        return {};
    }
    const std::vector<uint8_t> pixels = RasterizeBitmap(bitmap, extent);
    DeleteObject(bitmap);
    return pixels;
}

std::vector<uint8_t> ExtractShellFallbackIcon(const std::wstring& target, UINT extent) {
    SHFILEINFOW information{};
    if (SHGetFileInfoW(target.c_str(), FILE_ATTRIBUTE_NORMAL, &information, sizeof(information),
            SHGFI_ICON | SHGFI_LARGEICON | SHGFI_ADDOVERLAYS) == 0 || information.hIcon == nullptr) {
        return {};
    }
    const std::vector<uint8_t> pixels = RasterizeIcon(information.hIcon, extent);
    DestroyIcon(information.hIcon);
    return pixels;
}

std::vector<uint8_t> ExtractIconPixels(const std::wstring& target, UINT extent) {
    std::vector<uint8_t> pixels = CreateDockGlyph(target, extent);
    if (!pixels.empty()) {
        return pixels;
    }
    pixels = ExtractShellItemImage(target, extent);
    return pixels.empty() ? ExtractShellFallbackIcon(target, extent) : pixels;
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
    CreateRenderTargets();
    CreateBackdropResources();
}

void Renderer::LoadIcons(const std::vector<std::wstring>& targets) {
    static_assert(kMaximumIcons <= UINT16_MAX,
        "The maximum icon count must fit in a D3D12 texture array.");

    if (targets.size() > kMaximumIcons) {
        throw std::runtime_error("The dock supports at most 512 visible icons.");
    }

    ComApartment apartment;
    Flush();
    m_iconAtlas.Reset();
    m_iconCount = static_cast<UINT>(std::max<size_t>(targets.size(), 1));
    m_pendingUploads.clear();
    const UINT extent = IconTextureExtent(m_window);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    Check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "Create icon upload allocator");
    Check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
              IID_PPV_ARGS(&commandList)),
        "Create icon upload command list");

    const D3D12_RESOURCE_DESC atlas = TextureDescription(extent, extent, TextureArraySize(m_iconCount));
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

    if (targets.empty()) {
        CreateFallbackIcon(0, commandList.Get());
    } else {
        for (UINT index = 0; index < targets.size(); ++index) {
            UploadIcon(index, targets[index], commandList.Get());
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

bool Renderer::CaptureBackdrop(const RECT& screenRectangle) {
    const LONG width = screenRectangle.right - screenRectangle.left;
    const LONG height = screenRectangle.bottom - screenRectangle.top;
    if (m_backdropDc == nullptr || m_backdropDibPixels == nullptr || m_backdropTexture == nullptr ||
        width != static_cast<LONG>(m_width) || height != static_cast<LONG>(m_height)) {
        return false;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return false;
    }

    const BOOL copied = BitBlt(m_backdropDc, 0, 0, width, height, screen, screenRectangle.left,
        screenRectangle.top, SRCCOPY | CAPTUREBLT);
    const int released = ReleaseDC(nullptr, screen);
    if (copied == FALSE || released == 0) {
        return false;
    }

    UploadBackdropPixels();
    m_backdropValid = true;
    return true;
}

void Renderer::Render(const DockRenderState& state) {
    if (state.width == 0 || state.height == 0) {
        return;
    }

    const UINT frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    FrameResource& frame = m_frames[frameIndex];
    WaitForFrame(frame);

    Check(frame.allocator->Reset(), "Reset frame allocator");
    Check(m_commandList->Reset(frame.allocator.Get(), nullptr), "Reset command list");

    frame.mappedConstants->scene0[0] = static_cast<float>(state.width);
    frame.mappedConstants->scene0[1] = static_cast<float>(state.height);
    frame.mappedConstants->scene0[2] = state.glassAlpha;
    frame.mappedConstants->scene0[3] = state.timeSeconds;
    frame.mappedConstants->scene1[0] = state.slideProgress;
    frame.mappedConstants->scene1[1] = static_cast<float>(GetDpiForWindow(m_window)) / 96.0F;
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
        instance.iconMeta[1] = icon.dragged ? 1.0F : 0.0F;
        instance.iconMeta[2] = icon.hovered ? 1.0F : 0.0F;
        instance.iconMeta[3] = static_cast<float>(std::min(icon.textureIndex, m_iconCount - 1));
    }
    if (iconCount > 0) {
        m_commandList->DrawInstanced(6, iconCount, 0, 0);
    }

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    m_commandList->ResourceBarrier(1, &barrier);
    Check(m_commandList->Close(), "Close command list");
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    Check(m_swapChain->Present(1, 0), "Present composition frame");
    SignalFrame(frame);
}

void Renderer::Flush() {
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
    range.NumDescriptors = 2;
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

    D3D12_STATIC_SAMPLER_DESC iconSampler = linearSampler;
    iconSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    iconSampler.ShaderRegister = 1;
    const std::array samplers = {linearSampler, iconSampler};

    D3D12_ROOT_SIGNATURE_DESC1 root{};
    root.NumParameters = static_cast<UINT>(parameters.size());
    root.pParameters = parameters.data();
    root.NumStaticSamplers = static_cast<UINT>(samplers.size());
    root.pStaticSamplers = samplers.data();
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D12_ROOT_SIGNATURE_VERSION_1_1;
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
    descriptors.NumDescriptors = 2;
    descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(m_device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&m_srvHeap)),
        "Create shader resource descriptor heap");
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(descriptors.Type);
}

void Renderer::CreateRenderTargets() {
    if (m_rtvHeap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
        descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        descriptors.NumDescriptors = kBufferCount;
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

        UploadBackdropPixels();
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
    m_backdropFootprint = {};
    m_backdropRowCount = 0;
    m_backdropInitialized = false;
    m_backdropValid = false;

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

void Renderer::UploadBackdropPixels() {
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
    Flush();
    m_backdropInitialized = true;
}

void Renderer::WaitForFrame(FrameResource& frame) {
    if (frame.fenceValue == 0 || m_fence->GetCompletedValue() >= frame.fenceValue) {
        return;
    }

    Check(m_fence->SetEventOnCompletion(frame.fenceValue, m_fenceEvent), "Set frame fence event");
    WaitForSingleObject(m_fenceEvent, INFINITE);
}

void Renderer::UploadIcon(UINT textureIndex, const std::wstring& target,
    ID3D12GraphicsCommandList* commandList) {
    const UINT extent = IconTextureExtent(m_window);
    const std::vector<uint8_t> pixels = ExtractIconPixels(target, extent);
    if (pixels.empty()) {
        CreateFallbackIcon(textureIndex, commandList);
        return;
    }
    UploadIconTexture(textureIndex, pixels.data(), extent, extent, commandList);
}

void Renderer::CreateFallbackIcon(UINT textureIndex, ID3D12GraphicsCommandList* commandList) {
    const UINT extent = IconTextureExtent(m_window);
    const std::vector<uint8_t> pixels(static_cast<size_t>(extent) * static_cast<size_t>(extent) * 4U, 0);
    UploadIconTexture(textureIndex, pixels.data(), extent, extent, commandList);
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
    Check(m_queue->Signal(m_fence.Get(), "Signal frame fence");
}
