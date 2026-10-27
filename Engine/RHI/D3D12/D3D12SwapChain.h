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
    void  Present(bool vsync) override;
    void* GetCurrentBackBufferView() const override;
    void* GetDepthBufferView()       const override { return nullptr; }
    u32   GetBackendFormat()    const override { return static_cast<u32>(m_Format); }
    Format GetColorFormat() const override { return m_Format; }

    /// 供命令列表使用：拿到当前后缓冲的 RTV（CPU 句柄）
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRtv() const;

    /// 标记后缓冲已经被写成 RENDER_TARGET（命令列表 BeginRenderPass 时调用）。
    /// D3D12 的屏障要求 StateBefore 严格等于资源**当前**状态，猜错会让设备挂起
    /// （实测截图时猜 RENDER_TARGET、实际是 PRESENT ⇒ DEVICE_HUNG）。
    void MarkBackBufferRendered() { m_BackBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET; }

    /// 【调试】按环境变量 HE_CAPTURE_FRAME=N 在第 N 帧把后缓冲存成 PNG。
    /// 必须在 **Present 之前** 调用：FLIP_DISCARD 交换链在 Present 之后内容未定义。
    void CaptureIfRequested();

private:
    u64  m_FrameCount  = 0;
    int  m_CaptureAt   = -1;    // -1 = 未读取环境变量；0 = 不截图
    bool m_CaptureInit = false;
    /// 后缓冲的当前 D3D12 状态（未渲染过时是 PRESENT）
    D3D12_RESOURCE_STATES m_BackBufferState = D3D12_RESOURCE_STATE_PRESENT;

    bool CreateBuffers();

    ComPtr<IDXGISwapChain3>   m_SwapChain;
    ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
    std::vector<ComPtr<ID3D12Resource>> m_Buffers;
    D3D12Device* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    u32 m_Index = 0;
    u32 m_BufferCount = 2;
    UINT m_RtvSize = 0;
    Format m_Format = Format::BGRA8_UNORM;
    bool m_Vsync = true;
};

} // namespace he::rhi::d3d12

#endif // _WIN32
