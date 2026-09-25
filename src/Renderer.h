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
#include <unordered_map>
#include <vector>

#include <wrl/client.h>

#include "SystemTray.h"
#include "DockTheme.hlsli"

enum class DockIconKind : uint8_t {
    App = 0,
    Divider = 1,
    TrayDivider = 2,
    Tray = 3,
    Clock = 4,
    Trash = 5,
    Weather = 6,
};

struct DockIconRenderData {
    RECT bounds{};
    bool running = false;
    bool dragged = false;
    bool hovered = false;
    bool pressed = false;
    DockIconKind kind = DockIconKind::App;
    TraySlot traySlot = TraySlot::Overflow;
    UINT textureIndex = 0;
    bool adaptiveInk = false;
};

struct DockRenderState {
    UINT width = 1;
    UINT height = 1;
    float glassAlpha = DOCK_GLASS_ALPHA;
    // Packed glass-effect toggles (DOCK_FX_* bits): written to scene1.x.
    UINT fxFlags = DOCK_FX_ALL;
    // Dock content scale (1.0 = default). Drives the shader's optical bevel
    // width so icons always clear the lensing band (see Shaders.hlsl).
    // (Previously wall-clock seconds; the shader never consumed it.)
    float dockScale = 1.0F;
    bool showDevBounds = false;
    bool allowBlockingGpuWait = true;
    bool skipIfGpuBusy = false;
    std::span<const DockIconRenderData> icons;
};

class Renderer {
public:
    ~Renderer();

    void Initialize(HWND window, UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void LoadIcons(const std::vector<std::wstring>& cacheKeys,
        const std::vector<std::vector<std::wstring>>& iconCandidates, UINT iconPixelExtent);
    void UploadIcons(const std::vector<std::wstring>& targets,
        const std::vector<std::vector<uint8_t>>& pixelBuffers);
    [[nodiscard]] UINT TextureIndexForTarget(const std::wstring& target) const noexcept;
    [[nodiscard]] bool HasIconForTarget(const std::wstring& target) const noexcept;
    [[nodiscard]] bool HasCachedIconPixels(const std::wstring& target) const noexcept;
    [[nodiscard]] const std::vector<uint8_t>* CachedIconPixels(const std::wstring& target) const noexcept;
    [[nodiscard]] std::vector<std::wstring> IconTargetKeys() const;
    void AppendMissingIcons(const std::vector<std::wstring>& targets,
        const std::vector<std::vector<uint8_t>>& pixelBuffers);
    void UpdateCachedIcons(const std::vector<std::wstring>& targets,
        const std::vector<std::vector<uint8_t>>& pixelBuffers);
    [[nodiscard]] UINT IconAtlasPixelExtent() const noexcept;
    [[nodiscard]] static std::vector<uint8_t> ExtractIconPixels(
        const std::vector<std::wstring>& candidates, UINT iconPixelExtent);
    [[nodiscard]] bool CaptureBackdrop(const RECT& screenRectangle, bool* changed = nullptr);
    [[nodiscard]] bool NeedsBackdropBitBlt(const RECT& screenRectangle) const noexcept;
    [[nodiscard]] bool BackdropValid() const noexcept;
    void InvalidateBackdrop() noexcept;
    [[nodiscard]] bool Render(const DockRenderState& state);
    // Bake the dock GlassPS stack into a BGRA8 buffer for layered menus
    // (Quick Settings / Dock Settings / context). Uses DOCK_FX_PANEL so the
    // plate fills the surface (no dock shadow margin ring).
    [[nodiscard]] bool BakeGlassPanel(const RECT& screenRect, UINT width, UINT height,
        UINT fxFlags, float glassAlpha, float dpiScale, HWND excludeA, HWND excludeB,
        HWND excludeC, std::vector<uint8_t>& outBgra);
    // Non-blocking GlassPS bake for open menus (Quick/Dock Settings / context).
    // Begin submits BitBlt + GPU work without waiting; Take copies readback when
    // the fence is signaled. Dedicated panel command list so this never races the
    // dock swap-chain Present path (the old shared-list rebake hitched the cursor).
    [[nodiscard]] bool BeginLiveGlassPanelBake(const RECT& screenRect, UINT width, UINT height,
        UINT fxFlags, float glassAlpha, float dpiScale, HWND excludeA, HWND excludeB,
        HWND excludeC);
    [[nodiscard]] bool TakeLiveGlassPanelResult(std::vector<uint8_t>& outBgra);
    [[nodiscard]] bool IsLiveGlassPanelPending() const noexcept;
    void CancelLiveGlassPanelBake() noexcept;
    // Same wallpaper-luma cut as AdaptiveChromeInk in Shaders.hlsl so Quick /
    // Dock Settings text matches Start/Search/clock chrome on the dock.
    void SampleAdaptiveChromeInk(uint8_t& r, uint8_t& g, uint8_t& b) const noexcept;
    void Flush();

    [[nodiscard]] HANDLE FrameLatencyWaitableObject() const noexcept;
    [[nodiscard]] D3D_FEATURE_LEVEL FeatureLevel() const noexcept;
    [[nodiscard]] D3D_SHADER_MODEL ShaderModel() const noexcept;

private:
    static constexpr UINT kBufferCount = 3;
    static constexpr UINT kMaximumIcons = 512;
    // Idle BitBlt skips before a forced live re-capture (~1 s at the
    // 8 ms/120 Hz backdrop cadence). Bounds the DWM fast path so a stale
    // cache (e.g. black frames validated before first composition at
    // logon/resume) always heals while a static desktop still skips ~99%.
    static constexpr UINT kBackdropForcedCaptureSkips = 120;
    static constexpr UINT kIconTextureDescriptor = 0;
    static constexpr UINT kBackdropTextureDescriptor = 1;
    static constexpr UINT kTempBlurDescriptor = 2;
    static constexpr UINT kTempBlurDescriptor2 = 3;

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
    void CreateBackdropResources();
    void ReleaseBackdropResources() noexcept;
    [[nodiscard]] bool UploadBackdropPixels();
    [[nodiscard]] uint64_t HashBackdropPixels() const noexcept;
    [[nodiscard]] bool WaitForBackdropCopy(DWORD timeoutMs = INFINITE);
    [[nodiscard]] bool WaitForFrame(FrameResource& frame, DWORD timeoutMs = INFINITE);
    void WaitForAllFrames();
    void RebuildIconAtlasFromCache();
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
    UINT m_srvDescriptorSize = 0;

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
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_blurPipeline;
    // Iteration passes for the compounded frost (pass 2 vertical from temp,
    // pass 3 horizontal from temp2 back into temp).
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_blurVPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_blurH2Pipeline;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferCount> m_backBuffers;
    std::array<FrameResource, kBufferCount> m_frames;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_iconAtlas;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backdropTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_blurTemp;
    // Tracks the temp target state across frames (starts as render target).
    bool m_blurTempIsShaderResource = false;
    // Second ping-pong target for the iterated frost (same size/format as
    // temp; the final iteration lands back in temp so GlassPS is unchanged).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_blurTemp2;
    bool m_blurTemp2IsShaderResource = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backdropUpload;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_backdropCopyAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_backdropCopyCommandList;
    UINT m_iconCount = 0;
    UINT m_iconPixelExtent = 56;
    float m_dpiScale = 1.0F;
    std::unordered_map<std::wstring, UINT> m_iconTextureByTarget;
    std::unordered_map<std::wstring, std::vector<uint8_t>> m_iconPixelCache;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_pendingUploads;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_backdropFootprint{};
    UINT m_backdropRowCount = 0;
    uint8_t* m_backdropUploadPixels = nullptr;
    HDC m_backdropDc = nullptr;
    HBITMAP m_backdropBitmap = nullptr;
    HGDIOBJ m_backdropPreviousBitmap = nullptr;
    uint8_t* m_backdropDibPixels = nullptr;
    bool m_backdropInitialized = false;
    bool m_backdropValid = false;
    uint64_t m_backdropHash = 0;
    // Last DWM composed-frame count seen by CaptureBackdrop. Used for the idle
    // fast path: when DWM hasn't composed since the last capture, the backdrop
    // pixels cannot have changed and the BitBlt is skipped (timer still fires).
    uint64_t m_backdropDwmFrame = 0;
    bool m_backdropDwmFrameValid = false;
    // Consecutive idle skips since the last live BitBlt (see
    // kBackdropForcedCaptureSkips). Reset on every capture attempt.
    UINT m_backdropIdleSkips = 0;
    UINT64 m_backdropCopyFenceValue = 0;
    HANDLE m_fenceEvent = nullptr;

    void ReleasePanelGlassResources() noexcept;
    [[nodiscard]] bool EnsurePanelGlassResources(UINT width, UINT height);
    [[nodiscard]] bool EnsurePanelCaptureDib(UINT width, UINT height);
    void ReleasePanelCaptureDib() noexcept;
    void ReleasePanelGlassPool() noexcept;
    void StashActivePanelGlass() noexcept;
    [[nodiscard]] bool CreatePanelGlassResourcesExact(UINT width, UINT height);

    // Exact-size D3D panel glass pool. Live menus round-robin different sizes;
    // stash up to 3 exact WxH sets so CreateCommittedResource is not paid every
    // tick. Active set lives in m_panel* below; pool holds the rest.
    static constexpr size_t kPanelGlassPoolSize = 3;
    struct PanelGlassPoolEntry {
        UINT width = 0;
        UINT height = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> backdrop;
        Microsoft::WRL::ComPtr<ID3D12Resource> backdropUpload;
        Microsoft::WRL::ComPtr<ID3D12Resource> blurTemp;
        Microsoft::WRL::ComPtr<ID3D12Resource> blurTemp2;
        Microsoft::WRL::ComPtr<ID3D12Resource> color;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        uint8_t* uploadPixels = nullptr;
        bool blurTempIsSrv = false;
        bool blurTemp2IsSrv = false;
        UINT64 lastUsed = 0;
    };
    std::array<PanelGlassPoolEntry, kPanelGlassPoolSize> m_panelPool{};
    UINT64 m_panelPoolClock = 0;

    UINT m_panelWidth = 0;
    UINT m_panelHeight = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelBackdrop;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelBackdropUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelBlurTemp;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelBlurTemp2;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelReadback;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_panelSrvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_panelRtvHeap;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_panelBackdropFootprint{};
    UINT m_panelBackdropRowCount = 0;
    uint8_t* m_panelBackdropUploadPixels = nullptr;
    bool m_panelBlurTempIsSrv = false;
    bool m_panelBlurTemp2IsSrv = false;
    // Pooled GDI capture DIB for panel glass BitBlt (sRGB V5 → skip ICM probes).
    HDC m_panelCaptureDc = nullptr;
    HBITMAP m_panelCaptureBitmap = nullptr;
    HGDIOBJ m_panelCapturePrevious = nullptr;
    uint8_t* m_panelCapturePixels = nullptr;
    UINT m_panelCaptureWidth = 0;
    UINT m_panelCaptureHeight = 0;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_panelAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_panelCommandList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelConstants;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelIconInstances;
    FrameConstants* m_panelMappedConstants = nullptr;
    UINT64 m_panelFenceValue = 0;
    bool m_panelBakePending = false;
    UINT m_panelBakeWidth = 0;
    UINT m_panelBakeHeight = 0;

    HANDLE m_frameLatencyWaitableObject = nullptr;
};
