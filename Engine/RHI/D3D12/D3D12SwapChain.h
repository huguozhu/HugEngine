// ============================================================================
//  D3D12SwapChain.h —— D3D12 交换链
//
//  使用 FLIP_DISCARD 模型（D3D12 唯一支持现代特性的模式）：
//    · DXGI_SWAP_EFFECT_FLIP_DISCARD
//    · 每个 back buffer 一个 RTV（从 RTV 堆分配）
//    · AcquireNextImage 取当前索引；Present 后轮换
//  注意：FLIP 模型下 Present 之后 back buffer 内容未定义，截图必须在 Present 之前。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D12Common.h"
#include <string>
#include <vector>

namespace he::rhi::d3d12 {

class D3D12Device;

class D3D12SwapChain final : public IRHISwapChain {
public:
    ~D3D12SwapChain() override;

    bool Initialize(D3D12Device* device, const SwapChainDesc& desc);

    void  Resize(u32 width, u32 height) override;
    u32   GetCurrentBackBufferIndex() const override { return m_Index; }
    u32   GetWidth()  const override { return m_Width; }
    u32   GetHeight() const override { return m_Height; }
    bool  AcquireNextImage() override;
    /// 呈现（渲染线程）：记下当前后缓冲序号与状态，有提交路由时交给 RHI 线程，否则就地 PresentNow
    void  Present(bool vsync) override;
    /// 真正呈现（提交线程或就地）：把后缓冲 index 从 before 转到 PRESENT，再 IDXGISwapChain::Present。
    /// 只用参数里的序号与状态，不读渲染线程正在改的 m_BackBufferState
    void  PresentNow(u32 index, D3D12_RESOURCE_STATES before, bool vsync);
    void* GetCurrentBackBufferView() const override;
    void* GetDepthBufferView()       const override { return nullptr; }
    u32   GetBackendFormat()    const override { return static_cast<u32>(m_Format); }
    Format GetColorFormat() const override { return m_Format; }

    /// 供命令列表使用：拿到当前后缓冲的 RTV（CPU 句柄）
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRtv() const;
    /// 按序号取后缓冲的 RTV / 资源（命令流回放用录制时登记的序号，见 D3D12CommandList::SetRenderPassHint）
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtv(u32 index) const;
    ID3D12Resource*             GetBackBuffer(u32 index) const { return index < m_Buffers.size() ? m_Buffers[index].Get() : nullptr; }
    /// 录制用的后缓冲序号（模式 2 下为渲染线程推进的序号）
    u32 GetRecordIndex() const { return RecordIndex(); }
    /// 当前后缓冲的资源与状态（命令列表做 PRESENT<->RENDER_TARGET 屏障时要用）
    ID3D12Resource*      GetCurrentBackBufferResource() const;
    D3D12_RESOURCE_STATES GetBackBufferState() const { return m_BackBufferState; }
    void                 SetBackBufferState(D3D12_RESOURCE_STATES st) { m_BackBufferState = st; }
    /// 某个资源是否为本交换链的后缓冲（命令列表据此把后缓冲的状态同步给交换链）
    bool                 IsBackBuffer(const ID3D12Resource* res) const;

    /// 标记后缓冲已经被写成 RENDER_TARGET（命令列表 BeginRenderPass 时调用）。
    /// D3D12 的屏障要求 StateBefore 严格等于资源**当前**状态，猜错会让设备挂起
    /// （实测截图时猜 RENDER_TARGET、实际是 PRESENT ⇒ DEVICE_HUNG）。
    /// 【T9】自后缓冲登记进设备资源状态表后，真正的事实来源是设备表（GetResourceState）；
    /// 这个镜像仍要维护，因为命令流回放的 hint 走的是它。
    void MarkBackBufferRendered() { m_BackBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET; }

    /// 【调试】按帧号截图（P8：与 Vulkan 共用的 RHI 接口）。
    /// 第 frameIndex 帧（1 起）在 Present 之前把后缓冲存成 PNG；path 为空 = 默认路径
    /// `<exe 目录>/capture_frameN.png`（见 RHI/ScreenCapture.h）。frameIndex <= 0 取消。
    /// 未调用本接口时，行为与原来完全一致：读环境变量 HE_CAPTURE_FRAME。
    void RequestCapture(u32 frameIndex, const char* path = nullptr) override;

    /// 【调试】按环境变量 HE_CAPTURE_FRAME=N 在第 N 帧把后缓冲存成 PNG。
    /// 必须在 **Present 之前** 调用：FLIP_DISCARD 交换链在 Present 之后内容未定义。
    /// index / state：要截的后缓冲序号及其当前状态（由 PresentNow 传入，不读 m_BackBufferState）
    void CaptureIfRequested(u32 index, D3D12_RESOURCE_STATES state);

private:
    u64  m_FrameCount  = 0;
    int  m_CaptureAt   = -1;    // -1 = 未读取环境变量；0 = 不截图
    bool m_CaptureInit = false;
    /// 截图输出路径覆盖（UTF-8）；空 = 用 rhi::DefaultCapturePath(m_FrameCount)
    std::string m_CapturePath;
    /// 后缓冲的当前 D3D12 状态（未渲染过时是 PRESENT）
    D3D12_RESOURCE_STATES m_BackBufferState = D3D12_RESOURCE_STATE_PRESENT;

    bool CreateBuffers();

    ComPtr<IDXGISwapChain3>   m_SwapChain;
    ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
    std::vector<ComPtr<ID3D12Resource>> m_Buffers;
    D3D12Device* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    u32 m_Index = 0;
    /// 呈现是否经提交路由（模式 2）：此时 DXGI 的实时序号要等 RHI 线程呈现后才更新，
    /// 渲染线程改用自己推进的 m_Index（翻转模型的后缓冲按顺序轮换）
    bool m_PresentRouted = false;
    /// 录制用的后缓冲序号：路由时取 m_Index，否则取 DXGI 实时序号
    u32  RecordIndex() const;
    u32 m_BufferCount = 2;
    UINT m_RtvSize = 0;
    Format m_Format = Format::BGRA8_UNORM;
    bool m_Vsync = true;
    /// 【P10】交换链是否带 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`（= 允许无垂直同步呈现）。
    /// 只有它为真时才能用 syncInterval=0 + DXGI_PRESENT_ALLOW_TEARING，否则 Present 每帧
    /// 返回 DXGI_ERROR_INVALID_CALL（实测：HE_NO_VSYNC 下 04/06/07 全部中招）。
    bool m_TearingEnabled = false;

    /// Present 前「后缓冲 → PRESENT」屏障用的命令列表。必须常驻并按帧槽位轮换：
    /// 之前每帧临时创建、提交后立即析构，分配器在 GPU 执行完之前就被销毁
    /// （调试层 id=552 LUCPrepareForDestruction）⇒ GPU 读到已释放的命令内存。
    ComPtr<ID3D12CommandAllocator>    m_PresentAllocs[kMaxFramesInFlight];
    ComPtr<ID3D12GraphicsCommandList> m_PresentList;
    ComPtr<ID3D12Fence>               m_PresentFence;
    u64                               m_PresentFenceValues[kMaxFramesInFlight]{};
    u64                               m_PresentNextValue = 1;
    u32                               m_PresentSlot = 0;
};

} // namespace he::rhi::d3d12

#endif // _WIN32
