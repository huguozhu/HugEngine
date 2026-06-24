#pragma once

#include "RHI/Types.h"
#include "RHI/SwapChain.h"
#include "RHI/CommandList.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "Core/Types.h"

#include <memory>

// ============================================================
// RHI device interface
// ============================================================

namespace he::rhi {

struct DeviceInitDesc {
    Backend     backend             = Backend::Vulkan;
    i32         preferredAdapter    = -1;
    bool        enableValidation    = true;
    void*       windowHandle        = nullptr;
    u32         backBufferWidth     = 1920;
    u32         backBufferHeight    = 1080;
};

class IRHIDevice {
public:
    IRHIDevice();
    virtual ~IRHIDevice();

    virtual void Initialize(const DeviceInitDesc& desc);
    virtual void Shutdown();

    virtual Backend    GetBackend() const = 0;
    virtual DeviceCaps GetCaps()    const = 0;

    // --- Resource creation ---
    virtual std::unique_ptr<IRHISwapChain>      CreateSwapChain(const SwapChainDesc& desc) = 0;
    virtual std::unique_ptr<IRHICommandList>    CreateCommandList(QueueType queue = QueueType::Graphics) = 0;
    virtual std::unique_ptr<IRHIBuffer>         CreateBuffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<IRHITexture>        CreateTexture(const TextureDesc& desc) = 0;
    virtual std::unique_ptr<IRHISampler>        CreateSampler(const SamplerDesc& desc) = 0;
    virtual std::unique_ptr<IRHIPipelineState>  CreatePipelineState(const PipelineStateDesc& desc) = 0;

    // --- Commands ---
    virtual void Submit(IRHICommandList* cmdList) = 0;
    virtual void WaitIdle() = 0;
};

// --- Global device access ---
IRHIDevice*                     GetDevice();
void                            SetDevice(IRHIDevice* device);
std::unique_ptr<IRHIDevice>     CreateDevice(Backend backend);

} // namespace he::rhi
