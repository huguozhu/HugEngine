#pragma once

#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include "RHI/SwapChain.h"
#include "Containers/Array.h"
#include "Core/Types.h"

#include <functional>
#include <string>
#include <vector>
#include <unordered_map>

// ============================================================
// Render Graph — 帧级渲染资源编排器
//
// Phase 2: 自动 Barrier 推导 + 资源别名 + Pass 裁剪
// ============================================================

namespace he::render {

// --- 资源句柄 ---
using ResourceHandle = u32;
constexpr ResourceHandle kInvalidHandle = ~0u;

// --- 资源描述 ---
enum class ResourceType : u8 { Texture, Buffer };

struct ResourceDesc {
    ResourceType     type         = ResourceType::Texture;
    u32              width        = 1;
    u32              height       = 1;
    rhi::Format      format       = rhi::Format::RGBA8_UNORM;
    rhi::BufferUsage bufferUsage  = rhi::BufferUsage::None;
    rhi::TextureUsage textureUsage = rhi::TextureUsage::ShaderResource;
    u64              bufferSize   = 0;  // 用于别名大小计算
    String           name;              // 调试名
};

// --- 资源运行时状态 ---
struct ResourceState {
    rhi::ResourceState layout = rhi::ResourceState::Undefined;
    bool isDepth = false;  // 深度/模板资源用特殊布局
};

// --- 资源访问信息 ---
enum class ResourceAccess : u8 { None, Read, Write, ReadWrite };

struct PassResource {
    ResourceHandle handle;
    ResourceAccess access;
};

// --- 屏障记录 ---
struct BarrierRecord {
    ResourceHandle handle;
    rhi::PipelineStage srcStage;
    rhi::PipelineStage dstStage;
    rhi::ResourceState srcState;
    rhi::ResourceState dstState;
};

// --- Pass 队列类型提示 ---
enum class RGPassQueue : u8 {
    Default  = 0,  // 跟随 RenderGraph 默认队列 (Graphics)
    Graphics = 1,  // 显式 Graphics 队列
    Compute  = 2,  // 显式 Compute 队列 → AsyncCompute 候选
};

// --- 渲染 Pass ---
using PassExecuteFunc = std::function<void(rhi::IRHICommandList* cmdList)>;

struct PassNode {
    String                      name;
    std::vector<PassResource>   reads;
    std::vector<PassResource>   writes;
    PassExecuteFunc             execute;
    // 编译后
    std::vector<PassNode*>      dependencies;
    u32                         order = 0;
    // 屏障：Pass 执行前需要插入的资源转换
    std::vector<BarrierRecord>  preBarriers;
    // 异步调度
    RGPassQueue                 queueHint     = RGPassQueue::Default;  // 队列类型提示
    bool                        asyncSchedule = false;  // 编译后: 是否可并行执行
    bool                        requiresSync  = false;  // 编译后: 是否需要跨队列同步点
    // 跨队列 Barrier（AsyncCompute 专用）
    std::vector<BarrierRecord>  crossQueueAcquire;  // Pass 执行前: Graphics→Compute
    std::vector<BarrierRecord>  crossQueueRelease;  // Pass 执行后: Compute→Graphics
};

// --- 资源别名池（Phase 2: 简单贪心，Phase 3: 完善）---
struct AliasInfo {
    u64  offset = 0;   // 池内偏移
    u32  poolId = 0;   // 所属池（0=未分配）
};

// --- Render Graph ---
class RenderGraph {
public:
    RenderGraph();
    ~RenderGraph();

    // --- 资源管理 ---
    ResourceHandle CreateTexture(StringView name, const rhi::TextureDesc& desc);
    ResourceHandle CreateBuffer(StringView name, const rhi::BufferDesc& desc);
    ResourceHandle ImportTexture(StringView name, rhi::IRHITexture* external);
    void          SetSwapChain(rhi::IRHISwapChain* sc) { m_SwapChain = sc; }
    void          SetProfiler(class ProfilerManager* p) { m_Profiler = p; }
    const ResourceDesc& GetResourceDesc(ResourceHandle h) const;

    // --- Pass 构建 ---
    PassNode* AddPass(StringView name,
                      std::vector<PassResource> reads  = {},
                      std::vector<PassResource> writes = {},
                      PassExecuteFunc execute = nullptr,
                      RGPassQueue queueHint = RGPassQueue::Default);

    ResourceHandle ImportBackBuffer();

    // --- 编译 ---
    void Compile();

    // --- 执行（同步模式，单 Graphics 队列） ---
    void Execute(rhi::IRHICommandList* cmdList, rhi::IRHIDevice* device);

    // --- 执行（异步模式，Graphics + Compute 双队列） ---
    void Execute(rhi::IRHICommandList* graphicsCmd,
                 rhi::IRHICommandList* computeCmd,
                 rhi::IRHIDevice* device);

    // --- AsyncCompute 开关 ---
    bool IsAsyncComputeEnabled() const { return m_AsyncComputeEnabled; }
    void SetAsyncComputeEnabled(bool enabled) { m_AsyncComputeEnabled = enabled; }

    void Reset();
    const std::vector<PassNode*>& GetPassOrder() const { return m_PassOrder; }

private:
    void BuildDependencies();
    void TopologicalSort();
    void DeriveBarriers();       // 自动推导 Barrier（Phase 2）
    void ApplyAliasing();         // 资源别名分配（Phase 2）
    void CullDeadPasses();        // 裁剪未使用的 Pass（Phase 2）
    rhi::ResourceState AccessToState(ResourceAccess access, bool isDepth) const;

    // AsyncCompute 调度（Phase 3）
    void ScheduleAsyncPasses();   // 分析依赖，标记可并行 Pass
    void InsertCrossQueueBarrier(PassNode* pass,
                                  rhi::QueueType srcQueue,
                                  rhi::QueueType dstQueue);
    // 获取 Pass 之后（拓扑序）的所有 Pass
    std::vector<PassNode*> GetSubsequentPasses(PassNode* pass);

    std::vector<ResourceDesc>     m_Resources;
    std::vector<ResourceState>    m_ResourceStates;   // 编译后填充
    std::vector<AliasInfo>        m_AliasInfo;         // 别名分配
    std::vector<PassNode>         m_Passes;
    std::vector<PassNode*>        m_PassOrder;

    // 导入的外部资源
    std::unordered_map<ResourceHandle, rhi::IRHITexture*> m_ImportedTextures;
    rhi::IRHISwapChain* m_SwapChain = nullptr;
    ProfilerManager*    m_Profiler  = nullptr;

    ResourceHandle m_BackBufferHandle = kInvalidHandle;
    bool           m_Compiled = false;
    bool           m_AsyncComputeEnabled = false;
};

#define RG_READ(h)   PassResource{h, ResourceAccess::Read}
#define RG_WRITE(h)  PassResource{h, ResourceAccess::Write}
#define RG_RW(h)     PassResource{h, ResourceAccess::ReadWrite}

} // namespace he::render
