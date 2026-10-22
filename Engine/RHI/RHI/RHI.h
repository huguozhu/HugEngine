#pragma once

#include "RHI/Types.h"
#include "RHI/RHIHandles.h"   // A-3：句柄式资源（带代次校验）
#include "RHI/SwapChain.h"
#include "RHI/CommandList.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "RHI/QueryPool.h"
#include "RHI/RayTracing.h"
#include "RHI/MeshShader.h"
#include "RHI/Bindless.h"
#include "RHI/SubmitRouter.h"   // 提交路由（模式 2：RHI 线程唯一提交者）
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
    u32         backBufferWidth     = kDefaultBackBufferWidth;
    u32         backBufferHeight    = kDefaultBackBufferHeight;
};

class IRHIDevice {
public:
    IRHIDevice();
    virtual ~IRHIDevice();

    virtual void Initialize(const DeviceInitDesc& desc);
    virtual void Shutdown();

    virtual Backend    GetBackend() const = 0;
    /// 设备能力查询。**返回只读引用**：实现方须在内部缓存一次结果（原先按值返回 ⇒
    /// 每次调用都拷贝整个结构体；Vulkan 实现里还会重复驱动查询）。
    virtual const DeviceCaps& GetCaps() const = 0;

    // --- 格式能力查询（§14.8 任务 18 新增；最小扩展，性质同任务 3/6/12 的 RHI 补充）---
    // 该格式能否作为**存储图像**（UAV / `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT`）被着色器读写。
    // 【为什么需要】任务 18 的软光栅是 compute（写 GBuffer 颜色 UAV），而"compute 写深度"能否
    //   成立取决于 `D32_SFLOAT` 是否支持存储图像：实测本机 NVIDIA RTX 4060 支持、
    //   同机 AMD 核显**不支持**（§14.14 的 A1 裁决与 §14.5）。故模块必须**运行时查**这个能力，
    //   不支持时走"深度不写 + 明确告警 + 读数标出"的降级路径，而不是静默失败。
    // 默认返回 false：后端未实现时按"不支持"处理（保守方向 = 不走存储图像路径）。
    virtual bool SupportsStorageImage(Format /*format*/) const { return false; }

    // --- Resource creation ---
    virtual std::unique_ptr<IRHISwapChain>      CreateSwapChain(const SwapChainDesc& desc) = 0;
    virtual std::unique_ptr<IRHICommandList>    CreateCommandList(QueueType queue = QueueType::Graphics) = 0;
    virtual std::unique_ptr<IRHICommandList>    CreateSecondaryCommandList() = 0;
    virtual std::unique_ptr<IRHIBuffer>         CreateBuffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<IRHITexture>        CreateTexture(const TextureDesc& desc) = 0;

    // === A-3：句柄式创建/解析/销毁（与上面的 unique_ptr 版**并存**，迁移期两套都可用）===
    // 【为什么并存】`unique_ptr` 版把"所有权"和"句柄"绑在一起，迁移期逐个持有者替换；
    // 句柄版让资源登记进设备资源表，用**代次**检出悬挂引用（见 RHIHandles.h）。
    // 默认实现 = 该后端不支持（返回无效句柄 / nullptr），避免其它后端被迫改动。
    virtual RHIBufferHandle  CreateBufferHandle(const BufferDesc& /*desc*/) { return {}; }
    virtual RHITextureHandle CreateTextureHandle(const TextureDesc& /*desc*/) { return {}; }
    virtual IRHIBuffer*      ResolveBuffer(RHIBufferHandle /*handle*/) const { return nullptr; }
    virtual IRHITexture*     ResolveTexture(RHITextureHandle /*handle*/) const { return nullptr; }
    /// 按句柄销毁：从资源表移除 ⇒ 代次前进，该句柄与所有更早的句柄一律解析为 nullptr（幂等）
    virtual void DestroyBufferHandle(RHIBufferHandle /*handle*/) {}
    virtual void DestroyTextureHandle(RHITextureHandle /*handle*/) {}

    // --- A-3 第二批：PipelineState / Sampler（同样与 unique_ptr 版并存）---
    // 【为什么 DescriptorSet 不在此列】它本来就是设备侧的 `u64` 句柄（`m_DescSets`），
    // 已经具备"句柄 + 显式销毁"的语义，不需要再包一层。
    virtual RHIPipelineStateHandle CreatePipelineStateHandle(const PipelineStateDesc& /*desc*/) { return {}; }
    virtual RHISamplerHandle       CreateSamplerHandle(const SamplerDesc& /*desc*/) { return {}; }
    virtual IRHIPipelineState*     ResolvePipelineState(RHIPipelineStateHandle /*handle*/) const { return nullptr; }
    virtual IRHISampler*           ResolveSampler(RHISamplerHandle /*handle*/) const { return nullptr; }
    virtual void DestroyPipelineStateHandle(RHIPipelineStateHandle /*handle*/) {}
    virtual void DestroySamplerHandle(RHISamplerHandle /*handle*/) {}

    // --- A-3 第二批·扩展：光线追踪管线与加速结构（同样与 unique_ptr 版并存）---
    virtual RHIRayTracingPipelineHandle CreateRTPipelineStateHandle(const RTPipelineStateDesc& /*desc*/) { return {}; }
    virtual RHIAccelerationStructureHandle CreateBLASHandle(const BLASBuildDesc& /*desc*/) { return {}; }
    virtual RHIAccelerationStructureHandle CreateTLASHandle(const TLASBuildDesc& /*desc*/) { return {}; }
    virtual IRHIRayTracingPipelineState* ResolveRTPipelineState(RHIRayTracingPipelineHandle /*handle*/) const { return nullptr; }
    virtual IRHIAccelerationStructure*   ResolveAccelerationStructure(RHIAccelerationStructureHandle /*handle*/) const { return nullptr; }
    virtual void DestroyRTPipelineStateHandle(RHIRayTracingPipelineHandle /*handle*/) {}
    virtual void DestroyAccelerationStructureHandle(RHIAccelerationStructureHandle /*handle*/) {}
    virtual std::unique_ptr<IRHISampler>        CreateSampler(const SamplerDesc& desc) = 0;
    virtual std::unique_ptr<IRHIPipelineState>  CreatePipelineState(const PipelineStateDesc& desc) = 0;

    // --- Bindless Heap ---
    // 返回设备级 bindless 堆（懒创建；Vulkan 侧为 VulkanBindlessHeap）
    virtual IRHIBindlessHeap* GetBindlessHeap() = 0;

    // --- Transient Resource ---
    // 创建使用瞬态内存的纹理（GPU 完成前帧后内存被回收）
    // 返回 nullptr 表示后端不支持瞬态资源（回退到普通 CreateTexture）
    virtual std::unique_ptr<IRHITexture>        CreateTransientTexture(const TextureDesc& desc) { return nullptr; }
    // 推进瞬态资源帧：回收上一帧内存，切换到下一帧的 Heap
    // 默认空实现：后端不支持瞬态资源时安全跳过
    virtual void AdvanceTransientResources() {}

    // --- PSO 预热管理器 ---
    // 注册 PSO 描述符到后台预热队列（非阻塞，仅记录描述符）
    virtual void PrecompileQueuePSO(const PipelineStateDesc& desc) {}
    // 启动后台 PSO 预热线程（从主缓存派生独立 VkPipelineCache 编译）
    virtual void StartPSOPrecompile() {}
    // 获取预热进度（0.0 ~ 1.0，1.0 表示全部完成）
    virtual float GetPSOPrecompileProgress() const { return 1.0f; }

    // --- PSO 创建限流器 ---
    // 入队一个 PSO 描述符到限流队列（非阻塞，帧循环内不直接创建）
    virtual void EnqueuePSOCreate(const PipelineStateDesc& desc) {}
    // 每帧从队列取最多 maxPerFrame 个创建，返回本次创建的 PSO（供调用方持有）
    virtual std::vector<std::unique_ptr<IRHIPipelineState>> ProcessPSOCreateQueue(u32 maxPerFrame) { return {}; }
    // 队列中待创建的 PSO 数量
    virtual u32 GetPendingPSOCreateCount() const { return 0; }
    // 【A-5】句柄版异步 PSO 创建：立即返回一个"已登记、尚未就绪"的占位句柄，
    // ProcessPSOCreateQueue 在拥有线程上真正创建后填充它。未实现该能力的后端返回空句柄。
    virtual RHIPipelineStateHandle EnqueuePSOCreateHandle(const PipelineStateDesc& /*desc*/) { return {}; }

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
    /// 更新描述符数组（bindless SSBO 数组）
    virtual void                      UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                                          DescriptorType type,
                                                          IRHIBuffer** buffers, u32 count) = 0;
    virtual void                      DestroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) = 0;

    /// 直接绑定 ImageView 到描述符集（用于 SwapChain BackBuffer 等非 IRHITexture 图像）
    virtual void                      UpdateDescriptorSetWithImageView(DescriptorSetHandle set, u32 binding,
                                                                       DescriptorType type, void* imageView) = 0;
    /// 更新描述符集：绑定 AccelerationStructure（用于 RT PSO 的 TLAS 绑定）
    virtual void                      UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                                          DescriptorType type,
                                                          IRHIAccelerationStructure* as) = 0;

    // --- Per-Mip ImageView 支持（用于 Hi-Z 金字塔构建等写入特定 mip 的场景） ---
    /// 创建纹理指定 mip level 的存储图像视图（仅 Color format 纹理，默认 layer 0）
    virtual void*                     CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel) = 0;
    /// 创建纹理指定 mip level + array layer 的存储图像视图（用于 Cubemap 逐面渲染等）
    virtual void*                     CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) = 0;
    /// 创建纹理指定 mip level 的采样图像视图（仅 Color format 纹理，默认 layer 0）
    virtual void*                     CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel) = 0;
    /// 创建纹理指定 mip level + array layer 的采样图像视图（用于 Cubemap 逐面采样等）
    virtual void*                     CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) = 0;
    /// 销毁通过 CreateTextureMip*View 创建的图像视图
    virtual void                      DestroyTextureMipView(void* view) = 0;
    // --- GPU Query ---
    virtual std::unique_ptr<IRHIQueryPool> CreateQueryPool(u32 queryCount,
        QueryType type = QueryType::Timestamp) = 0;
    virtual float GetTimestampPeriod() = 0;  // 时间戳单位（纳秒）

    // --- Debug Object Naming ---
    // 为 GPU 资源设置调试名称，在 RenderDoc / NSight 等调试工具中显示
    // 默认空实现：后端不支持时安全跳过
    virtual void SetResourceDebugName(IRHIBuffer* resource, const char* name);
    virtual void SetResourceDebugName(IRHITexture* resource, const char* name);
    virtual void SetResourceDebugName(IRHIPipelineState* resource, const char* name);
    virtual void SetResourceDebugName(IRHISampler* resource, const char* name);
    virtual void SetResourceDebugName(IRHIAccelerationStructure* resource, const char* name);

    // --- Device Generated Commands (DGC) ---
    // 默认空实现，后端不支持 DGC 时安全跳过
    virtual bool  InitializeDGC(IRHIPipelineState* pso, u32 maxSequences, u32 maxDraws) { return false; }
    virtual void  ShutdownDGC() {}
    virtual bool  IsDGCReady() const { return false; }
    /// 以下 getter 返回 Vulkan/D3D12 后端特定句柄（void* / u64），调用方直接赋值到 DGCContext
    virtual void* GetDGCLayout()           const { return nullptr; }
    virtual void* GetDGCExecutionSet()     const { return nullptr; }
    virtual u64   GetDGCPreprocessAddr()   const { return 0; }
    virtual u64   GetDGCPreprocessSize()   const { return 0; }
    virtual u32   GetDGCMaxSequences()     const { return 0; }

    // --- ImGui 后端辅助（封装 Vulkan/D3D12 特定资源创建） ---
    virtual void* CreateImGuiDescriptorPool() { return nullptr; }
    virtual void  DestroyImGuiDescriptorPool(void* pool) {}
    /// @param swapchainFormat 交换链格式（VkFormat / DXGI_FORMAT，通过 IRHISwapChain::GetBackendFormat 查询）
    virtual void* CreateImGuiRenderPass(u32 swapchainFormat) { return nullptr; }
    virtual void  DestroyImGuiRenderPass(void* rp) {}
    /// 获取命令列表后端原生句柄（VkCommandBuffer / ID3D12CommandList），供 ImGui 渲染使用
    virtual void* GetImGuiCommandBuffer(IRHICommandList* cmd) const { return nullptr; }

    // --- Commands ---
    /// `stream`（A-1，可选）：纯数据命令流，由执行端在提交前翻译进 `cmdList`。
    /// 默认 `nullptr` ⇒ 现有调用点一行不改、行为不变。
    virtual void Submit(IRHICommandList* cmdList, RHICommandList* stream = nullptr) = 0;
    virtual void WaitIdle() = 0;

    // === 提交路由（模式 2：RHI 线程是唯一提交者，见 SubmitRouter.h）===
    // 默认实现 = 不路由（模式 0/1 行为不变）；Vulkan 后端覆写。
    virtual void SetSubmitRouter(ISubmitRouter* /*router*/) {}
    virtual ISubmitRouter* GetSubmitRouter() const { return nullptr; }
    /// 【仅 RHI 线程】绕过路由真正提交（由路由器在 RHI 线程上调用；断言提交者角色）
    virtual void ExecuteSubmit(IRHICommandList* /*cmdList*/, RHICommandList* /*stream*/) {}
    /// 【仅 RHI 线程】绕过路由真正呈现（imageIndex / waitSemaphore 由渲染线程在入队时捕获）
    virtual void ExecutePresent(IRHISwapChain* /*swapChain*/, u32 /*imageIndex*/,
                                void* /*waitSemaphore*/, bool /*vsync*/) {}

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
