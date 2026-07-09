#pragma once

#include "RHI/Types.h"
#include "RHI/SwapChain.h"
#include "RHI/CommandList.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "RHI/QueryPool.h"
#include "RHI/RayTracing.h"
#include "RHI/MeshShader.h"
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
    virtual std::unique_ptr<IRHICommandList>    CreateSecondaryCommandList() = 0;
    virtual std::unique_ptr<IRHIBuffer>         CreateBuffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<IRHITexture>        CreateTexture(const TextureDesc& desc) = 0;
    virtual std::unique_ptr<IRHISampler>        CreateSampler(const SamplerDesc& desc) = 0;
    virtual std::unique_ptr<IRHIPipelineState>  CreatePipelineState(const PipelineStateDesc& desc) = 0;

    // --- Ray Tracing 资源创建 ---
    virtual std::unique_ptr<IRHIAccelerationStructure>
        CreateBLAS(const BLASBuildDesc& desc) = 0;                                // 创建 Bottom-Level Acceleration Structure
    virtual std::unique_ptr<IRHIAccelerationStructure>
        CreateTLAS(const TLASBuildDesc& desc) = 0;                                // 创建 Top-Level Acceleration Structure
    virtual ASBuildSizes GetBLASBuildSizes(const BLASBuildDesc& desc) = 0;        // 查询 BLAS 构建所需内存
    virtual ASBuildSizes GetTLASBuildSizes(u32 maxInstanceCount) = 0;             // 查询 TLAS 构建所需内存
    virtual std::unique_ptr<IRHIRayTracingPipelineState>
        CreateRTPipelineState(const RTPipelineStateDesc& desc) = 0;               // 创建 RT Pipeline State

    // --- Descriptor Sets ---
    virtual DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) = 0;
    virtual DescriptorSetHandle       AllocateDescriptorSet(DescriptorSetLayoutHandle layout) = 0;
    virtual void                      UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                                          DescriptorType type, IRHIBuffer* buffer) = 0;
    virtual void                      UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                                          DescriptorType type, IRHITexture* texture,
                                                          IRHISampler* sampler) = 0; // CombinedImageSampler（阴影贴图等）
    /// 更新描述符数组（bindless 纹理/采样器数组）
    virtual void                      UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                                          DescriptorType type,
                                                          IRHITexture** textures, IRHISampler** samplers,
                                                          u32 count) = 0;
    virtual void                      DestroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) = 0;

    // --- GPU Query ---
    virtual std::unique_ptr<IRHIQueryPool> CreateQueryPool(u32 queryCount) = 0;
    virtual float GetTimestampPeriod() = 0;  // 时间戳单位（纳秒）

    // --- Commands ---
    virtual void Submit(IRHICommandList* cmdList) = 0;
    virtual void WaitIdle() = 0;

    // === 多队列支持 ===

    // 查询是否支持独立 Compute 队列（在 CreateDevice 之后调用）
    virtual bool HasAsyncComputeQueue() const = 0;

    // 获取指定队列族的索引（用于跨队列 Barrier 的 ownership transfer）
    virtual u32 GetQueueFamily(QueueType queue) const = 0;

    // === 跨队列同步原语 ===

    // 创建跨队列信号量（Timeline Semaphore / D3D12 Fence）
    // Vulkan: VK_SEMAPHORE_TYPE_TIMELINE + 初始值 0
    // D3D12:  CreateFence(0, D3D12_FENCE_FLAG_SHARED)
    virtual RHIFenceHandle CreateFence() = 0;

    // 销毁信号量
    virtual void DestroyFence(RHIFenceHandle fence) = 0;

    // CPU 端等待信号量到达指定值（用于管线插入点）
    // timeout: 超时(纳秒), 0 表示不等待直接返回, UINT64_MAX 表示无限等待
    virtual bool WaitForFence(RHIFenceHandle fence, u64 value, u64 timeout = UINT64_MAX) = 0;

    // CPU 端查询当前信号量值
    virtual u64 GetFenceValue(RHIFenceHandle fence) const = 0;

    // GPU 端信号: 在指定队列上提交命令后发出信号
    virtual void SignalFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) = 0;

    // GPU 端等待: 指定队列等待信号量到达指定值后才开始执行
    virtual void WaitFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) = 0;

    // 批量提交多个 CommandList（可能来自不同队列）
    // 内部按队列分组后分别提交
    virtual void SubmitAll(Span<IRHICommandList*> cmdLists) = 0;
};

// --- Global device access ---
IRHIDevice*                     GetDevice();
void                            SetDevice(IRHIDevice* device);
std::unique_ptr<IRHIDevice>     CreateDevice(Backend backend);

} // namespace he::rhi
