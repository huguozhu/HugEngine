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
};

} // namespace he::rhi
