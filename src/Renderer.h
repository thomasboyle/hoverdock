#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_6.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <wrl/client.h>

struct DockIconRenderData {
    RECT bounds{};
    bool running = false;
    bool dragged = false;
    bool hovered = false;
    UINT textureIndex = 0;
};

struct DockRenderState {
    UINT width = 1;
    UINT height = 1;
    float glassAlpha = 0.92F;
    float slideProgress = 0.0F;
    float timeSeconds = 0.0F;
    bool showDevBounds = false;
    std::span<const DockIconRenderData> icons;
};

class Renderer {
public:
    ~Renderer();

    void Initialize(HWND window, UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void LoadIcons(const std::vector<std::wstring>& targets);
    void Render(const DockRenderState& state);
    void Flush();

    [[nodiscard]] HANDLE FrameLatencyWaitableObject() const noexcept;
    [[nodiscard]] D3D_FEATURE_LEVEL FeatureLevel() const noexcept;
    [[nodiscard]] D3D_SHADER_MODEL ShaderModel() const noexcept;

private:
    static constexpr UINT kBufferCount = 3;
    static constexpr UINT kMaximumIcons = 32;

    struct alignas(256) FrameConstants {
        float scene0[4]{};
        float scene1[4]{};
        std::byte padding[224]{};
    };

    struct IconInstanceConstants {
        float iconRect[4]{};
        float iconMeta[4]{};
    };

    struct FrameResource {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12Resource> constants;
        Microsoft::WRL::ComPtr<ID3D12Resource> iconInstances;
        FrameConstants* mappedConstants = nullptr;
        IconInstanceConstants* mappedIcons = nullptr;
        UINT64 fenceValue = 0;
    };

    void CreateDevice();
    void CreateCompositionSwapChain(HWND window, UINT width, UINT height);
    void CreateFrameResources();
    void CreateRootSignatureAndPipelines();
    void CreateRenderTargets();
    void WaitForFrame(FrameResource& frame);
    void UploadIcon(UINT textureIndex, const std::wstring& target,
        ID3D12GraphicsCommandList* commandList);
    void CreateFallbackIcon(UINT textureIndex, ID3D12GraphicsCommandList* commandList);
    void UploadIconTexture(UINT textureIndex, const uint8_t* pixels, UINT width, UINT height,
        ID3D12GraphicsCommandList* commandList);
    void SignalFrame(FrameResource& frame);

    HWND m_window = nullptr;
    UINT m_width = 1;
    UINT m_height = 1;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_12_0;
    D3D_SHADER_MODEL m_shaderModel = D3D_SHADER_MODEL_6_0;
    UINT64 m_fenceValue = 0;
    UINT m_rtvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_compositionDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_compositionTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_compositionVisual;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_glassPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_iconPipeline;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferCount> m_backBuffers;
    std::array<FrameResource, kBufferCount> m_frames;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_iconAtlas;
    UINT m_iconCount = 0;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_pendingUploads;
    HANDLE m_fenceEvent = nullptr;
    HANDLE m_frameLatencyWaitableObject = nullptr;
};
