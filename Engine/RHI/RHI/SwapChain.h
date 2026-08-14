#pragma once

#include "RHI/Types.h"

namespace he::rhi {

// SwapChain 图像数量常量（Triple Buffering）
constexpr u32 kSwapchainImageCount = 3;

struct SwapChainDesc {
    void*   windowHandle    = nullptr;
    u32     width           = kDefaultBackBufferWidth;
    u32     height          = kDefaultBackBufferHeight;
    u32     bufferCount     = kSwapchainImageCount;  // Double/Triple buffering
    Format  format          = Format::RGBA8_UNORM;
    bool    vsync           = true;
    bool    hdr             = false;   // HDR10 输出（A2B10G10R10 + ST.2084）
};

class IRHISwapChain {
public:
    virtual ~IRHISwapChain() = default;

    virtual void Resize(u32 width, u32 height) = 0;
    virtual u32  GetCurrentBackBufferIndex() const = 0;
    virtual u32  GetWidth()  const = 0;
    virtual u32  GetHeight() const = 0;

    // Acquire next image (Vulkan) / back buffer (D3D12)
    virtual bool AcquireNextImage() = 0;
    // Present to screen
    virtual void Present(bool vsync) = 0;
    // 获取当前 BackBuffer ImageView（供 RenderGraph 导入）
    virtual void* GetCurrentBackBufferView() const = 0;
    virtual void* GetDepthBufferView()       const = 0;
    // 后端像素格式（VkFormat / DXGI_FORMAT），供 ImGui 等后端代码查询
    virtual u32  GetBackendFormat()    const = 0;
    // RHI 颜色格式（供 ToneMap 等查询实际交换链格式，HDR 时为 A2B10G10R10）
    virtual Format GetColorFormat() const = 0;
};

} // namespace he::rhi
