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

    // ========================================================
    // 按帧号截图（P8：D3D12 / Vulkan 统一通路）
    //
    // 语义：第 frameIndex 帧（从 1 开始计，与呈现帧计数一致）**在 Present 之前**
    //       把当前后缓冲/交换链图像写成 PNG。两个后端都遵守同一套语义：
    //         · 环境变量 `HE_CAPTURE_FRAME=N`（未显式调用本接口时生效）；
    //         · 默认文件名 `<exe 目录>/capture_frameN.png`，格式 PNG-RGBA8。
    //       必须在 Present 之前：D3D12 的 FLIP_DISCARD 交换链在 Present 后内容未定义；
    //       Vulkan 交换链图像在 PRESENT_SRC 布局下也不能直接读。
    //
    // frameIndex <= 0 表示取消截图；path 为 nullptr / 空串表示用默认路径。
    // 【实现要求】后端需要把图像内容读回成 RGBA8（BGRA8 后缓冲按内存原样即可，
    // 见 RHI/ScreenCapture.h 的通道顺序说明），再调用 rhi::SaveRgbaToPng 落盘。
    // ========================================================
    virtual void RequestCapture(u32 frameIndex, const char* path = nullptr) = 0;
};

} // namespace he::rhi
