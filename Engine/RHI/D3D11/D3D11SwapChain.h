// ============================================================================
//  D3D11SwapChain.h —— D3D11 交换链
//
//  FLIP_DISCARD 模型（规划 3.8）：
//    · 运行时自动轮换后缓冲，应用永远只访问 **buffer 0**（不能像 D3D12 那样每个缓冲建一个 RTV）；
//    · 后缓冲包装成 D3D11Texture，视图句柄与普通渲染目标一样登记，引擎拿去当附件 / 拷贝都能查回；
//    · 垂直同步关闭且系统支持时用 ALLOW_TEARING（与 D3D12 一致）；
//    · HE_CAPTURE_FRAME=N 或 RequestCapture(frame)：第 N 帧在 Present 之前把后缓冲存成 PNG
//      （FLIP 模型 Present 之后内容未定义，截图必须在 Present 之前）。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D11Common.h"
#include <memory>

namespace he::rhi::d3d11 {

class D3D11Device;
class D3D11Texture;

class D3D11SwapChain final : public IRHISwapChain {
public:
    ~D3D11SwapChain() override;

    bool Initialize(D3D11Device* device, const SwapChainDesc& desc);

    void   Resize(u32 width, u32 height) override;
    /// FLIP 模型下应用只访问 buffer 0
    u32    GetCurrentBackBufferIndex() const override { return 0; }
    u32    GetWidth()  const override { return m_Width; }
    u32    GetHeight() const override { return m_Height; }
    /// 窗口最小化（尺寸为 0）时返回 false，调用方跳过本帧
    bool   AcquireNextImage() override { return m_Width > 0 && m_Height > 0 && m_BackBuffer != nullptr; }
    void   Present(bool vsync) override;
    /// 真正呈现（模式 2 下由 RHI 线程经 D3D11Device::ExecutePresent 调用）
    void   PresentNow(bool vsync);
    void*  GetCurrentBackBufferView() const override;
    void*  GetDepthBufferView()       const override { return nullptr; }
    u32    GetBackendFormat()    const override { return static_cast<u32>(m_DxgiFormat); }
    Format GetColorFormat() const override { return m_Format; }
    /// 按帧号截图（P8 统一接口，见 RHI/SwapChain.h 的 RequestCapture）：第 frameIndex 帧
    /// 在 Present 之前把后缓冲写成 PNG；frameIndex <= 0 表示取消。显式请求优先于 HE_CAPTURE_FRAME
    void   RequestCapture(u32 frameIndex, const char* path = nullptr) override;

    /// 当前后缓冲的 RTV（主 render pass 的目标）
    ID3D11RenderTargetView* GetRtv() const;

private:
    /// 取 buffer 0 并包装成纹理
    bool CreateBackBuffer();
    /// HE_CAPTURE_FRAME=N 时在第 N 帧把后缓冲存成 PNG（必须在 Present 之前）
    void CaptureIfRequested();

    D3D11Device*                  m_Device = nullptr;
    ComPtr<IDXGISwapChain1>       m_SwapChain;
    std::unique_ptr<D3D11Texture> m_BackBuffer;
    u32         m_Width = 0, m_Height = 0;
    u32         m_BufferCount = 2;
    Format      m_Format = Format::RGBA8_UNORM;
    DXGI_FORMAT m_DxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool        m_Tearing = false;   // 系统支持 ALLOW_TEARING
    u64         m_FrameCount = 0;
    int         m_CaptureAt = -1;    // -1 = 未读取环境变量；0 = 不截图
    std::string m_CapturePath;       // 显式请求的落盘路径（空 = 用默认路径）
};

} // namespace he::rhi::d3d11

#endif // _WIN32
