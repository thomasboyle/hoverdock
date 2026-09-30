#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_6.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
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
    // Pointer-reactive rim glint: dock-client px position and 0..1 strength
    // (scene2). Strength 0 disables it.
    float glintX = 0.0F;
    float glintY = 0.0F;
    float glintStrength = 0.0F;
    bool showDevBounds = false;
    bool allowBlockingGpuWait = true;
    bool skipIfGpuBusy = false;
    std::span<const DockIconRenderData> icons;
};

class Renderer {
public:
    ~Renderer();

    // Starts D3D12 device creation on a worker; Initialize joins it.
    void BeginDeviceCreation();
    void Initialize(HWND window, UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void LoadIcons(const std::vector<std::wstring>& cacheKeys,
        const std::vector<std::vector<uint8_t>>& pixelBuffers, UINT iconPixelExtent);
    // Thread-safe; runs shell icon extraction for LoadIcons off the UI thread.
    [[nodiscard]] static std::vector<std::vector<uint8_t>> ExtractIconPixelBuffers(
        const std::vector<std::vector<std::wstring>>& iconCandidates, UINT iconPixelExtent);
    // Between Begin and End, icon cache updates defer the atlas rebuild so a
    // full reload uploads and flushes once instead of once per icon group.
    void BeginIconBatch() noexcept;
    void EndIconBatch();
    void CancelIconBatch() noexcept;
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
    // One DXGI duplication acquire for this tick. Dock strip and open menu plates
    // share it so a 120 Hz backdrop tick does not consume the frame twice.
    void PollDesktopChanges() noexcept;
    void FinishDesktopChangePoll() noexcept;
    [[nodiscard]] bool DesktopRegionChanged(const RECT& region) const noexcept;
    // One-shot: the next CaptureBackdrop BitBlts even if the strip looks idle.
    // Used for focus changes. Does not allocate.
    void RequestBackdropRefresh() noexcept;
    [[nodiscard]] bool NeedsBackdropBitBlt(const RECT& screenRectangle) const noexcept;
    [[nodiscard]] bool BackdropValid() const noexcept;
    void InvalidateBackdrop() noexcept;
    [[nodiscard]] bool Render(const DockRenderState& state);
    // Slide the swap chain inside the HWND. Moving the window itself drops it
    // from desktop capture for the whole reveal/hide.
    void SetContentOffsetY(float offsetY) noexcept;
    // Bake the dock GlassPS stack into a BGRA8 buffer for layered menus
    // (Quick Settings / Dock Settings / context). Uses DOCK_FX_PANEL so the
    // plate fills the surface (no dock shadow margin ring).
    [[nodiscard]] bool BakeGlassPanel(const RECT& screenRect, UINT width, UINT height,
        UINT fxFlags, float glassAlpha, float dpiScale, HWND excludeA, HWND excludeB,
        HWND excludeC, bool lightPlate, std::vector<uint8_t>& outBgra);
    // Non-blocking GlassPS bake for open menus (Quick/Dock Settings / context).
    // Begin submits BitBlt + GPU work without waiting; Take copies readback when
    // the fence is signaled. Dedicated panel command list so this never races the
    // dock swap-chain Present path (the old shared-list rebake hitched the cursor).
    // True when DWM has not composed since the last live panel capture attempt
    // (and the forced-capture skip budget remains). TickLivePopupGlass uses this
    // once per timer tick so round-robin targets do not burn the idle budget.
    [[nodiscard]] bool ShouldSkipLivePanelCapture() noexcept;
    // Skip BitBlt when this exact WxH was already confirmed unchanged for the
    // current DWM composed frame (cursor motion advances cFrame without
    // changing wallpaper under the menu).
    [[nodiscard]] bool ShouldSkipLivePanelBitBlt(UINT width, UINT height) noexcept;
    // tag is the caller's plate id (0 settings, 1 quick settings, 2 context) so two
    // plates can be in flight and taken independently. skippedUnchanged is set when
    // the BitBlt matched the last bake and no GPU work was submitted.
    [[nodiscard]] bool BeginLiveGlassPanelBake(const RECT& screenRect, UINT width, UINT height,
        UINT fxFlags, float glassAlpha, float dpiScale, HWND excludeA, HWND excludeB,
        HWND excludeC, bool lightPlate, int tag = -1, bool* skippedUnchanged = nullptr);
    [[nodiscard]] bool TakeLiveGlassPanelResult(std::vector<uint8_t>& outBgra, int* tag = nullptr);
    [[nodiscard]] bool IsLiveGlassPanelPending() const noexcept;
    void CancelLiveGlassPanelBake() noexcept;
    // Increments only when CaptureBackdrop uploads new pixels (desktop changed).
    [[nodiscard]] uint64_t BackdropChangeSerial() const noexcept { return m_backdropChangeSerial; }
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
    // Sparse scanline probe before a full dock-strip BitBlt. Hover/Present
    // advances DWM cFrame without changing wallpaper; probing a few rows is
    // ~height/kProbeRows cheaper and avoids the 2-10 ms kernel BitBlt.
    static constexpr UINT kBackdropProbeRows = 6;
    // Allow a few flapping pixels (DWM subpixel / cursor-adjacent) before a
    // probe miss or a committed-sample "changed" decision. Stops hash flap
    // from bumping BackdropChangeSerial and storming live menu rebakes.
    static constexpr UINT kBackdropProbeNoisePixels = 24;
    static constexpr UINT kBackdropChangeMinSamples = 96;
    static constexpr UINT kIconTextureDescriptor = 0;
    static constexpr UINT kBackdropTextureDescriptor = 1;
    static constexpr UINT kTempBlurDescriptor = 2;
    static constexpr UINT kTempBlurDescriptor2 = 3;

    struct alignas(256) FrameConstants {
        float scene0[4]{};
        float scene1[4]{};
        float scene2[4]{};
        std::byte padding[208]{};
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
    [[nodiscard]] bool EnsureBackdropProbe(UINT width);
    void ReleaseBackdropProbe() noexcept;
    [[nodiscard]] bool ProbeBackdropUnchanged(const RECT& screenRectangle) noexcept;
    void CommitBackdropProbeFromDib() noexcept;
    [[nodiscard]] size_t CountBackdropSampleDiffs() const noexcept;
    void CommitBackdropSamplesFromDib() noexcept;
    [[nodiscard]] bool WaitForBackdropCopy(DWORD timeoutMs = INFINITE);
    [[nodiscard]] bool WaitForFrame(FrameResource& frame, DWORD timeoutMs = INFINITE);
    void WaitForAllFrames();
    void RebuildIconAtlasFromCache();
    void RequestIconAtlasRebuild();
    // Next atlas slice that does not collide with a key already assigned.
    // m_iconCount stays 0 until the deferred batch rebuild, so callers must
    // not hand out slices from that stale counter alone.
    [[nodiscard]] UINT NextFreeIconSlot() const noexcept;
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
    float m_contentOffsetY = 0.0F;
    bool m_contentOffsetValid = false;
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
    bool m_iconBatchActive = false;
    bool m_iconAtlasRebuildPending = false;
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
    // Screen origin of the pixels in m_backdropDibPixels. Menu captures paste
    // this over the dock so the plate does not sample the dock's swap chain.
    LONG m_backdropOriginX = 0;
    LONG m_backdropOriginY = 0;
    bool m_backdropOriginValid = false;
    bool m_backdropInitialized = false;
    bool m_backdropValid = false;
    uint64_t m_backdropHash = 0;
    uint64_t m_backdropChangeSerial = 0;
    // Last DWM composed-frame count seen by CaptureBackdrop. Used for the idle
    // fast path: when DWM hasn't composed since the last capture, the backdrop
    // pixels cannot have changed and the BitBlt is skipped (timer still fires).
    uint64_t m_backdropDwmFrame = 0;
    bool m_backdropDwmFrameValid = false;
    bool m_backdropRefreshRequested = false;
    // Last BitBlt of a "dirty" frame did not change pixels. Further dirty-rect
    // spam must not BitBlt again until a quiet frame or an explicit refresh.
    bool m_stripConfirmedClean = false;
    bool m_stripDirtyPending = false;
    ULONGLONG m_lastStripReadMs = 0;
    // While the backdrop timer is on the 120 Hz cadence, re-read a dirty strip
    // on the next tick instead of holding a stale plate for 200 ms.
    static constexpr ULONGLONG kStripRecheckMs = 8;
    uint8_t m_stripRechecksLeft = 0;
    static constexpr uint8_t kStripRechecks = 3;
    uint8_t m_stripPendingTicks = 0;
    static constexpr uint8_t kStripPendingMaxTicks = 4;
    // Sparse probe DIB (width x kBackdropProbeRows) + reference from last commit.
    HDC m_backdropProbeDc = nullptr;
    HBITMAP m_backdropProbeBitmap = nullptr;
    HGDIOBJ m_backdropProbePrevious = nullptr;
    uint8_t* m_backdropProbePixels = nullptr;
    UINT m_backdropProbeWidth = 0;
    bool m_backdropProbeValid = false;
    std::vector<uint8_t> m_backdropProbeReference;
    // Stride-8 samples of the last uploaded/accepted backdrop (noise-tolerant
    // change detection after a full BitBlt).
    std::vector<uint32_t> m_backdropCommittedSamples;
    ULONGLONG m_lastBackdropSerialBumpMs = 0;
    UINT64 m_backdropCopyFenceValue = 0;
    HANDLE m_fenceEvent = nullptr;

    void ReleasePanelGlassResources() noexcept;
    [[nodiscard]] bool EnsurePanelGlassResources(UINT width, UINT height);
    [[nodiscard]] bool EnsurePanelCaptureDib(UINT width, UINT height);
    // Menu plates are much larger than the dock strip. Sampling them with the
    // magnifier blocks the UI thread (and the low-level mouse hook) long enough
    // to hitch the cursor. BitBlt the plate, hide only the layered popups, and
    // paste the dock strip from the magnifier backdrop where they overlap.
    [[nodiscard]] bool CapturePanelScreen(const RECT& screen, uint8_t* destBits, HDC destDc,
        HWND excludeA, HWND excludeB, HWND excludeC) noexcept;
    void StampDockBackdropInto(const RECT& panel, uint8_t* panelPixels) const noexcept;
    void ReleasePanelCaptureDib() noexcept;
    void ReleasePanelCapturePool() noexcept;
    [[nodiscard]] uint64_t HashPanelCapturePixels(UINT width, UINT height) const noexcept;
    void ReleasePanelGlassPool() noexcept;
    [[nodiscard]] bool StashActivePanelGlass() noexcept;
    [[nodiscard]] bool ParkInFlightPanelBake() noexcept;
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
        // Set when this set was parked mid-frame so a second plate can bake
        // without waiting. Resources stay alive until gpuFence completes.
        bool gpuPending = false;
        UINT64 gpuFence = 0;
        int gpuTag = -1;
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
    // Exact-size GDI capture DIB pool (mirrors D3D panel pool). Live menus
    // round-robin different WxH; without this, EnsurePanelCaptureDib destroyed
    // and CreateDIBSection+ICM'd every tick. Active slot is mirrored in the
    // m_panelCapture* aliases below for BitBlt/upload call sites.
    static constexpr size_t kPanelCapturePoolSize = 3;
    struct PanelCapturePoolEntry {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        uint8_t* pixels = nullptr;
        UINT width = 0;
        UINT height = 0;
        uint64_t contentHash = 0;
        bool hashValid = false;
        // DWM cFrame when contentHash was last confirmed. Same frame => skip BitBlt.
        uint64_t dwmAtHash = 0;
        UINT64 lastUsed = 0;
    };
    std::array<PanelCapturePoolEntry, kPanelCapturePoolSize> m_panelCapturePool{};
    UINT64 m_panelCapturePoolClock = 0;
    HDC m_panelCaptureDc = nullptr;
    HBITMAP m_panelCaptureBitmap = nullptr;
    HGDIOBJ m_panelCapturePrevious = nullptr;
    uint8_t* m_panelCapturePixels = nullptr;
    UINT m_panelCaptureWidth = 0;
    UINT m_panelCaptureHeight = 0;
    // DWM composed-frame gate for live panel BitBlt (same idea as dock backdrop).
    uint64_t m_panelCaptureDwmFrame = 0;
    bool m_panelCaptureDwmFrameValid = false;
    UINT m_panelCaptureIdleSkips = 0;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_panelAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_panelCommandList;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_panelAllocator2;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_panelCommandList2;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelConstants;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelConstants2;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_panelIconInstances;
    FrameConstants* m_panelMappedConstants = nullptr;
    FrameConstants* m_panelMappedConstants2 = nullptr;
    UINT64 m_panelListFence[2] = {};
    UINT64 m_panelFenceValue = 0;
    bool m_panelBakePending = false;
    int m_panelBakeTag = -1;
    UINT m_panelBakeWidth = 0;
    UINT m_panelBakeHeight = 0;

    HANDLE m_frameLatencyWaitableObject = nullptr;
    std::future<void> m_deviceCreation;
};
