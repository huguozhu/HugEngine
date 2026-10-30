// ============================================================================
//  D3D11SwapChain.h —— D3D11 交换链
//
//  M2 骨架阶段的占位实现：只记录尺寸，AcquireNextImage 恒成功、Present 不做事，
//  保证样例的帧循环能跑起来。真正的 DXGI 交换链在 M2.7 实现。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D11Common.h"

namespace he::rhi::d3d11 {

class D3D11SwapChain final : public IRHISwapChain {
public:
    explicit D3D11SwapChain(const SwapChainDesc& desc)
        : m_Width(desc.width), m_Height(desc.height), m_Format(desc.format) {}

    void   Resize(u32 width, u32 height) override { m_Width = width; m_Height = height; }
    u32    GetCurrentBackBufferIndex() const override { return 0; }
    u32    GetWidth()  const override { return m_Width; }
    u32    GetHeight() const override { return m_Height; }
    bool   AcquireNextImage() override { return true; }
    void   Present(bool) override {}
    void*  GetCurrentBackBufferView() const override { return nullptr; }
    void*  GetDepthBufferView()       const override { return nullptr; }
    u32    GetBackendFormat()    const override { return 0; }
    Format GetColorFormat() const override { return m_Format; }

private:
    u32    m_Width  = 0;
    u32    m_Height = 0;
    Format m_Format = Format::RGBA8_UNORM;
};

} // namespace he::rhi::d3d11

#endif // _WIN32
