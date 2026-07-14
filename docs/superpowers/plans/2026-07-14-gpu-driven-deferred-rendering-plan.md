# GPU Driven Deferred Rendering 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 HugEngine DeferredPipeline 的 GPU Driven 能力从 ~80% 提升至 100%，覆盖 DGC、两阶段剔除、持久化线程组、Work Graph、AsyncCompute 和 Forward+

**Architecture:** 按 M1→M4 四个里程碑推进。M1 修复渲染流程缺陷并开启 AsyncCompute；M2 提升剔除精度和效率；M3 实现 GPU 完全自主生成命令；M4 补齐 Forward+ 管线

**Tech Stack:** Vulkan 1.3+, Slang, VK_EXT_device_generated_commands, Timeline Semaphore, Compute Shader

**Spec:** `docs/superpowers/specs/2026-07-14-gpu-driven-deferred-rendering-design.md`

## Global Constraints

- 所有代码添加中文注释
- 不自动执行 `git commit`，需用户确认
- Commit log 使用中文，不含 AI 相关信息
- Vulkan 1.3.296+ 作为 DGC 的最低版本要求
- 所有 DGC/Work Graph 高级功能需保留传统路径 fallback

---

### Task 1: 修复 GPU_Cull Dispatch 时序（M1 · Item 3）

**Files:**
- Modify: `Engine/Render/Pipeline/DeferredPipeline.cpp:569-582` (GPU_Cull pass 定义)
- Modify: `Engine/Render/Pipeline/DeferredPipeline.cpp:644-653` (GB_Clear pass 定义)

**Interfaces:**
- Consumes: `RenderGraph::AddPass`, `GPUCulling::Dispatch`, `GBufferContext`
- Produces: 正确的 pass 执行顺序，vkCmdDispatch 在 vkCmdBeginRenderPass 之前

**背景：** 当前 BuildFrameGraph 中 GPU_Cull pass 已标记 `RGPassQueue::Compute`，但在 AsyncCompute 关闭时（当前默认），Compute pass 和 GB_Clear pass 都在同一 graphics cmdList 上录制。需要验证并确保 GPU_Cull 的 `vkCmdDispatch` 在 `GB_Clear` 的 `vkCmdBeginRenderPass` 之前执行。如果存在顺序问题，需在 GB_Clear 的 execute lambda 开始处显式结束任何活跃的 RenderPass。

- [ ] **Step 1: 验证当前执行顺序**

在 `DeferredPipeline::Render` 中开启调试日志，确认 GPU_Cull 和 GB_Clear 的录制顺序：

```cpp
// 在 DeferredPipeline.cpp 的 Render 方法中，rg.Execute 调用前添加
HE_CORE_INFO("[GPU Driven] 开始帧 {} — AsyncCompute: {}", m_FrameCounter, useAsyncCompute);
```

运行 `04.Deferred` sample，检查日志输出中 GPU_Cull 和 GB_Clear 的先后关系。若 GPU_Cull 先于 GB_Clear，则 Item 3 实际无问题；若反之则修复。

- [ ] **Step 2: 如需要，调整 GBufferRenderer_GPU 确保不在 RenderPass 内 Dispatch**

检查 `Engine/Render/Pipeline/GBufferRenderer_GPU.cpp` 的 `Render` 方法，确保在 `BeginOffscreenPass` 调用前已完成所有 Compute Dispatch：

```cpp
// GBufferRenderer_GPU::Render 中的关键路径应保持：
// 1. 任何 Compute Dispatch（如有）
// 2. c->BeginOffscreenPass(...)
// 3. c->DrawIndexedIndirect(...)
// 4. c->EndOffscreenPass()
```

- [ ] **Step 3: 编译并运行验证**

```bash
cmake --build build --target 04_Deferred
./build/Samples/Debug/04_Deferred.exe --validate
```

预期：Vulkan 校验层零警告，渲染画面正确

---

### Task 2: AsyncCompute 多阶段提交（M1 · Item 6）

**Files:**
- Modify: `Engine/Render/Pipeline/DeferredPipeline.cpp:478-506` (Render 方法 AsyncCompute 逻辑)
- Modify: `Engine/Render/Pipeline/DeferredPipeline.cpp:527-531` (FlushComputeWork)
- Modify: `Engine/Render/RenderGraph.cpp:133-134` (Execute 方法)
- Modify: `Engine/Render/RenderGraph.cpp:521-547` (ScheduleAsyncPasses)

**Interfaces:**
- Consumes: `IRHIDevice::HasAsyncComputeQueue`, `IRHICommandList::SetTimelineSignal/SetTimelineWait/Submit`, `RHIFenceHandle`, `RGPassQueue::Compute`
- Produces: AsyncCompute 默认开启，Compute Pass 真正在异步队列执行

**核心改动：** 将 RenderGraph::Execute 从单次 `vkQueueSubmit` 改为多阶段提交：

```
Phase 1: Submit Graphics cmdList #1 (Shadow + 前期 Pass)
    → Signal TimelineSemaphore(value=N)
Phase 2: Submit Compute cmdList (GPUCull, SSAO, SSGI, DDGI, AutoExposure)
    → Wait TimelineSemaphore(value=N)
    → Signal TimelineSemaphore(value=N+1)
Phase 3: Submit Graphics cmdList #2 (GBuffer, Lighting, PostProcess)
    → Wait TimelineSemaphore(value=N+1)
```

- [ ] **Step 1: 在 RenderGraph 中添加多阶段提交结构**

修改 `Engine/Render/RenderGraph.cpp`，在 `Execute` 方法中将 Pass 按队列类型分组：

```cpp
// RenderGraph.cpp Execute 方法中，替换原有的单 cmdList 执行逻辑
void RenderGraph::Execute() {
    // ... 前置校验 ...
    
    if (m_AsyncComputeEnabled && HasComputePasses()) {
        ExecuteWithAsyncCompute();
    } else {
        ExecuteSingleQueue();
    }
}

void RenderGraph::ExecuteWithAsyncCompute() {
    auto* device = m_Device;
    
    // Phase 1: 录制所有在第一个 Compute Pass 之前的 Graphics Pass
    auto graphicsCmd1 = device->CreateCommandList(rhi::QueueType::Graphics);
    auto computeCmd = device->CreateCommandList(rhi::QueueType::Compute);
    auto graphicsCmd2 = device->CreateCommandList(rhi::QueueType::Graphics);
    
    u64 timelineValue = ++m_TimelineCounter;
    bool graphics1Done = false;
    bool computeDone = false;
    
    for (auto* pass : m_PassOrder) {
        bool isCompute = (pass->queueHint == RGPassQueue::Compute);
        
        if (!isCompute && !computeDone) {
            // Phase 1: Graphics Pass（在 Compute 之前）
            ExecutePassOnCmdList(pass, graphicsCmd1.get());
        } else if (isCompute) {
            // Phase 2: Compute Pass
            if (!graphics1Done) {
                // 在第一个 Compute Pass 之前，设置跨队列同步
                // Graphics → Compute: Signal on graphics, Wait on compute
                graphicsCmd1->SetTimelineSignal(m_CrossQueueFence, timelineValue);
                computeCmd->SetTimelineWait(m_CrossQueueFence, timelineValue);
                graphics1Done = true;
            }
            ExecutePassOnCmdList(pass, computeCmd.get());
        } else {
            // Phase 3: 后续 Graphics Pass（在 Compute 之后）
            if (!computeDone && graphics1Done) {
                // Compute → Graphics: Signal on compute, Wait on graphics
                computeCmd->SetTimelineSignal(m_CrossQueueFence, timelineValue + 1);
                graphicsCmd2->SetTimelineWait(m_CrossQueueFence, timelineValue + 1);
                computeDone = true;
            }
            ExecutePassOnCmdList(pass, graphicsCmd2.get());
        }
    }
    
    // 按序 Submit
    graphicsCmd1->Submit();  // Phase 1
    computeCmd->Submit();    // Phase 2（异步）
    graphicsCmd2->Submit();  // Phase 3
    
    m_TimelineCounter += 2;
}
```

- [ ] **Step 2: 标记可异步 Pass**

在 `DeferredPipeline::BuildFrameGraph` 中，确保以下 Pass 已标记为 `RGPassQueue::Compute`：

当前已标记：
- `GPU_Cull` → `RGPassQueue::Compute` ✅ (line 582)
- `DDGI_Update` → `RGPassQueue::Compute` ✅ (line 674)

需新增标记：
```cpp
// SSAO Pass (当前约 line 678)
rg.AddPass("SSAO", {}, {{ssaoOut, ResourceAccess::Write}},
    [&, w, h](rhi::IRHICommandList* c) {
        // ... SSAO 逻辑 ...
    },
    RGPassQueue::Compute);  // 新增：标记为可异步执行

// AutoExposure Pass (当前约 line 850+)
rg.AddPass("AutoExposure", {}, {},
    [&](rhi::IRHICommandList* c) {
        // ... AutoExposure 逻辑 ...
    },
    RGPassQueue::Compute);  // 新增：标记为可异步执行
```

- [ ] **Step 3: 开启 AsyncCompute 默认值**

修改 `DeferredPipeline::Render` 中的 AsyncCompute 开关：

```cpp
// 原代码 (line 494-498):
#if 0  // AsyncCompute: 设为 1 启用
    bool useAsyncCompute = m_Device->HasAsyncComputeQueue() || m_AsyncComputeOverride;
#else
    bool useAsyncCompute = false;
#endif

// 改为:
bool useAsyncCompute = m_Device->HasAsyncComputeQueue();
```

同时移除 `m_AsyncComputeOverride` 调试变量（或保留为 CVar 控制）。

- [ ] **Step 4: 清理 FlushComputeWork 旧逻辑**

`FlushComputeWork` 方法（line 527-531）是旧的单次提交方式，替换为 RenderGraph 内部的多阶段提交后，此方法简化为：

```cpp
void DeferredPipeline::FlushComputeWork() {
    // 多阶段提交已在 RenderGraph::Execute 内部处理
    // 保留方法签名以兼容外部调用，但不再需要手动 Submit
}
```

- [ ] **Step 5: 编译并验证**

```bash
cmake --build build --target 04_Deferred
./build/Samples/Debug/04_Deferred.exe --validate
```

验证项：
1. Vulkan 校验层零错误零警告
2. GPU Profiler 面板应显示 Async Compute 队列有时间戳活动
3. 画面与 AsyncCompute 关闭时像素级一致

---

### Task 3: 两阶段遮挡剔除（M2 · Item 2）

**Files:**
- Create: `Engine/Shader/Shaders/GPUCull_TwoPhase.comp.slang`
- Modify: `Engine/Render/Pipeline/GPUCulling.h:30-77`
- Modify: `Engine/Render/Pipeline/GPUCulling.cpp` (Dispatch 方法 + 新增 Phase2 逻辑)
- Modify: `Engine/Render/Pipeline/GPUScene.h:42-68` (新增中间 buffer)

**Interfaces:**
- Consumes: `CullObjectBounds`, `HiZDownsample` (现有), `GPUSceneObject`
- Produces: `GPUCulling::DispatchTwoPhase`, `GPUCulling::m_Phase2PSO`, `GPUScene::m_Phase1CandidateBuf`

- [ ] **Step 1: 创建两阶段剔除 Compute Shader**

```hlsl
// Engine/Shader/Shaders/GPUCull_TwoPhase.comp.slang
// 两阶段遮挡剔除 Phase 2：使用当前帧 Hi-Z 精筛 Phase 1 候选物体
//
// Dispatch: (candidateCount + 63) / 64, 1, 1

[[vk::binding(0, 0)]] cbuffer CullParams {
    float4x4 u_ViewProj;
    uint    u_ObjectCount;
    uint    u_HiZMips;
    float2  u_ScreenSize;
};

// Phase 1 产出的候选物体索引（Compact 格式）
[[vk::binding(1, 0)]] StructuredBuffer<uint> u_Candidates;

// 当前帧 Hi-Z 深度金字塔
[[vk::binding(2, 0)]] Texture2D<float> u_HiZ;

// 输出：最终可见物体的 IndirectDraw 命令
[[vk::binding(3, 0)]] RWStructuredBuffer<IndirectDrawCommand> u_IndirectCmds;
// 输出：最终可见物体计数
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> u_DrawCount;

struct IndirectDrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= u_ObjectCount) return;
    
    uint objectIndex = u_Candidates[tid.x];
    
    // 从 GPUScene 读取 AABB（通过 bindless SSBO 或直接索引）
    // 此处假设已有 Phase 1 传下来的 AABB 数据
    // 执行当前帧 Hi-Z 精确遮挡测试
    // 若通过测试：atomicAdd → u_DrawCount → 写入 u_IndirectCmds
}
```

- [ ] **Step 2: 扩展 GPUCulling 类接口**

修改 `Engine/Render/Pipeline/GPUCulling.h`：

```cpp
class GPUCulling {
public:
    // ... 现有成员保持不变 ...
    
    /// 启用两阶段剔除（默认关闭，向后兼容）
    bool useTwoPhase = false;
    
    /// 获取 Phase 1 候选 buffer（供 Phase 2 读取）
    rhi::IRHIBuffer* GetCandidateBuffer() const { return m_CandidateBuf.get(); }
    
    /// 两阶段剔除入口（替代单阶段 Dispatch 调用）
    // Phase 1 在当前帧开始时执行（使用上帧 Hi-Z）
    // Phase 2 在 GBuffer 之后执行（使用当前帧 Hi-Z）
    void DispatchPhase1(rhi::IRHICommandList* cmd, const float4x4& viewProj, 
                        u32 objectCount, u32 screenW, u32 screenH);
    void DispatchPhase2(rhi::IRHICommandList* cmd, u32 candidateCount, 
                        u32 screenW, u32 screenH);

private:
    // Phase 2 专用资源
    std::unique_ptr<rhi::IRHIBuffer> m_CandidateBuf;       // Phase 1 → Phase 2 候选列表
    std::unique_ptr<rhi::IRHIBuffer> m_CandidateCountBuf;  // 候选数量
    std::unique_ptr<rhi::IRHIPipelineState> m_Phase2PSO;
    rhi::DescriptorSetLayoutHandle m_Phase2Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Phase2Set    = rhi::kInvalidSet;
};
```

- [ ] **Step 3: 实现 Phase 1 → Phase 2 的数据流**

修改 `Engine/Render/Pipeline/GPUCulling.cpp`：

```cpp
void GPUCulling::DispatchPhase1(rhi::IRHICommandList* cmd, const float4x4& viewProj,
                                 u32 objectCount, u32 screenW, u32 screenH) {
    // Phase 1: 使用上帧 Hi-Z 做保守粗筛
    // 输出: m_CandidateBuf（候选列表）+ m_CandidateCountBuf（候选数量）
    // 重用现有 Dispatch 的逻辑，但输出写入 Candidate buffer 而非 IndirectDraw buffer
    // ... 
}

void GPUCulling::DispatchPhase2(rhi::IRHICommandList* cmd, u32 candidateCount,
                                 u32 screenW, u32 screenH) {
    // Phase 2: 使用当前帧 Hi-Z 做精确验证
    // 输入: m_CandidateBuf, m_HiZTexture（当前帧）
    // 输出: m_IndirectCmdBuf（最终绘制命令）+ m_DrawCountBuf
    
    cmd->SetPipeline(m_Phase2PSO.get());
    // 绑定描述符集...
    uint32_t groupCountX = (candidateCount + 63) / 64;
    cmd->Dispatch(groupCountX, 1, 1);
}
```

- [ ] **Step 4: 在 DeferredPipeline 中集成两阶段剔除**

修改 `Engine/Render/Pipeline/DeferredPipeline.cpp` BuildFrameGraph：

```cpp
// GPU_Cull Phase 1（使用上帧深度，在 Async Compute Queue）
rg.AddPass("GPU_Cull_Phase1",
    {{gbDepth, ResourceAccess::Read}},
    {},
    [&, w, h](rhi::IRHICommandList* c) {
        if (!m_GPUCulling.enabled) return;
        if (m_GPUCulling.useTwoPhase) {
            m_GPUCulling.DispatchPhase1(c, camera.GetViewProjMatrix(), 
                          m_GPUScene.GetObjectCount(), w, h);
        } else {
            // 单阶段模式：直接输出到 IndirectDraw
            m_GPUCulling.Dispatch(c, camera.GetViewProjMatrix(), 
                          m_GPUScene.GetObjectCount(), w, h);
        }
    },
    RGPassQueue::Compute);

// ... GBuffer Pass ...

// GPU_Cull Phase 2（使用当前帧 Hi-Z，验证 Phase 1 候选）
rg.AddPass("GPU_Cull_Phase2",
    {},
    {},
    [&, w, h](rhi::IRHICommandList* c) {
        if (!m_GPUCulling.enabled || !m_GPUCulling.useTwoPhase) return;
        u32 candidateCount = m_GPUCulling.GetLastVisibleCount();
        if (candidateCount > 0) {
            m_GPUCulling.DispatchPhase2(c, candidateCount, w, h);
        }
    });
```

- [ ] **Step 5: 编译 shader 并验证**

```bash
# 编译新 shader
cd Engine/Shader
slangc Shaders/GPUCull_TwoPhase.comp.slang -target spirv -o Compiled/GPUCull_TwoPhase.comp.spv -I Shaders/

# 编译 C++ 代码并运行
cmake --build build --target 04_Deferred
./build/Samples/Debug/04_Deferred.exe --validate
```

---

### Task 4: 持久化线程组剔除（M2 · Item 4）

**Files:**
- Create: `Engine/Shader/Shaders/PersistentCull.comp.slang`
- Modify: `Engine/Render/Pipeline/GPUCulling.h` (新增 PTG 模式)
- Modify: `Engine/Render/Pipeline/GPUCulling.cpp` (PTG 初始化/触发/关闭)

**Interfaces:**
- Consumes: `GPUCulling::m_PSO`, `GPUCulling::m_IndirectCmdBuf`
- Produces: `GPUCulling::m_UsePTG`, `GPUCulling::m_PTG_PSO`, `GPUCulling::m_FrameSignalBuf`

- [ ] **Step 1: 创建持久化线程组 Shader**

```hlsl
// Engine/Shader/Shaders/PersistentCull.comp.slang
// 持久化线程组：Dispatch 一次后，每帧通过 u_FrameSignal 触发处理
// 减少 GPU 调度开销，适合物体数量波动不大的场景

// 常量
static const uint PTG_TOTAL_THREADS = 64;
static const uint PTG_GROUP_COUNT   = 4;   // 4 组 × 64 线程 = 256 线程
static const uint MAX_ITERATIONS    = 1024; // 单帧最大迭代次数（TDR 保护）

[[vk::binding(0, 0)]] cbuffer CullParams {
    float4x4 u_ViewProj;
    uint    u_ObjectCount;
    uint    u_HiZMips;
    uint    u_FrameIndex;     // 当前帧索引（触发信号）
    float2  u_ScreenSize;
};

// 场景物体数据（bindless SSBO）
[[vk::binding(1, 0)]] StructuredBuffer<GPUSceneObject> u_Objects;  // 无界数组
// Hi-Z 深度金字塔
[[vk::binding(2, 0)]] Texture2D<float> u_HiZ;
// 输出缓冲
[[vk::binding(3, 0)]] RWStructuredBuffer<IndirectDrawCommand> u_IndirectCmds;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> u_DrawCount;

// 持久化线程组共享变量
groupshared uint g_VisibleCount;
groupshared uint g_ProcessedFrameIndex;

[shader("compute")]
[numthreads(PTG_TOTAL_THREADS, 1, 1)]
void PersistentCull(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    // 初始化共享变量（仅主线程执行）
    if (gtid.x == 0) {
        g_VisibleCount = 0;
        g_ProcessedFrameIndex = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    
    while (true) {
        // Spin-wait：等待新帧信号
        uint frameIdx;
        do {
            frameIdx = u_FrameIndex;
        } while (frameIdx == g_ProcessedFrameIndex);
        
        g_ProcessedFrameIndex = frameIdx;
        if (gtid.x == 0) g_VisibleCount = 0;
        GroupMemoryBarrierWithGroupSync();
        
        // 处理本线程组负责的物体（跨线程组全局索引）
        uint globalThreadIndex = gid.x * PTG_TOTAL_THREADS + gtid.x;
        uint objectsInThisBatch = u_ObjectCount;
        
        // TDR 保护：限制迭代次数
        for (uint iter = 0; iter < MAX_ITERATIONS && globalThreadIndex < objectsInThisBatch; 
             iter++, globalThreadIndex += PTG_GROUP_COUNT * PTG_TOTAL_THREADS) {
            
            GPUSceneObject obj = u_Objects[globalThreadIndex];
            
            // 视锥剔除 + Hi-Z 遮挡剔除（复用现有 GPUCull 逻辑）
            // ... 
            
            // 原子写入 IndirectDrawCommand
            uint slot;
            InterlockedAdd(g_VisibleCount, 1, slot);
            u_IndirectCmds[slot] = /* draw command */;
        }
        
        DeviceMemoryBarrierWithGroupSync();
        
        // 主线程写入最终计数
        if (gtid.x == 0) {
            u_DrawCount[0] = g_VisibleCount;
        }
    }
}
```

- [ ] **Step 2: 扩展 GPUCulling 类**

修改 `Engine/Render/Pipeline/GPUCulling.h`：

```cpp
class GPUCulling {
public:
    // ... 现有成员 ...
    
    /// 持久化线程组模式
    bool usePTG = false;
    static constexpr u32 kPTGGroupCount = 4;
    
    /// 初始化 PTG（一次性 Dispatch，之后通过 Signal 触发）
    bool InitializePTG(rhi::IRHIDevice* device);
    
    /// 每帧触发 PTG 处理
    void SignalPTG(rhi::IRHICommandList* cmd, u32 frameIndex);
    
    /// 关闭 PTG 线程（发送退出信号）
    void ShutdownPTG(rhi::IRHIDevice* device);

private:
    // PTG 资源
    std::unique_ptr<rhi::IRHIPipelineState> m_PTG_PSO;
    std::unique_ptr<rhi::IRHIBuffer> m_FrameSignalBuf;  // 帧触发信号
    rhi::DescriptorSetLayoutHandle m_PTGLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_PTGSet    = rhi::kInvalidSet;
    bool m_PTGActive = false;
};
```

- [ ] **Step 3: 实现 PTG 初始化与触发逻辑**

修改 `Engine/Render/Pipeline/GPUCulling.cpp`：

```cpp
bool GPUCulling::InitializePTG(rhi::IRHIDevice* device) {
    // 1. 创建帧信号 buffer（upload buffer，CPU 每帧写入帧号）
    rhi::BufferDesc signalDesc;
    signalDesc.size = sizeof(u32);
    signalDesc.usage = rhi::BufferUsage::UniformBuffer | rhi::BufferUsage::Upload;
    m_FrameSignalBuf = device->CreateBuffer(signalDesc);
    
    // 2. 创建 PTG PSO
    // ... 类似现有 PSO 创建逻辑，但使用 PersistentCull.comp.spv ...
    
    // 3. 一次性 Dispatch PTG 线程组
    // 注意：这里需要一个临时 command list 来执行初始 Dispatch
    auto initCmd = device->CreateCommandList(rhi::QueueType::Compute);
    initCmd->SetPipeline(m_PTG_PSO.get());
    // ... 绑定描述符集 ...
    initCmd->Dispatch(kPTGGroupCount, 1, 1);
    device->Submit(initCmd.get());
    
    m_PTGActive = true;
    return true;
}

void GPUCulling::SignalPTG(rhi::IRHICommandList* cmd, u32 frameIndex) {
    if (!m_PTGActive) return;
    // 写入帧信号（CPU → GPU），触发 PTG 处理
    void* mapped = m_FrameSignalBuf->Map();
    memcpy(mapped, &frameIndex, sizeof(u32));
    m_FrameSignalBuf->Unmap();
}
```

- [ ] **Step 4: DeferredPipeline 集成 PTG**

修改 `Engine/Render/Pipeline/DeferredPipeline.cpp` Initialize 和 BuildFrameGraph：

```cpp
// Initialize 中：
if (m_GPUCulling.usePTG) {
    m_GPUCulling.InitializePTG(device);
}

// BuildFrameGraph GPU_Cull pass 中：
if (m_GPUCulling.usePTG) {
    m_GPUCulling.SignalPTG(c, m_CurrentFrameSlot);
} else {
    m_GPUCulling.Dispatch(c, viewProj, objectCount, w, h);
}

// Shutdown 中：
if (m_GPUCulling.usePTG) {
    m_GPUCulling.ShutdownPTG(m_Device);
}
```

- [ ] **Step 5: 编译并验证**

```bash
slangc Engine/Shader/Shaders/PersistentCull.comp.slang -target spirv -o Engine/Shader/Compiled/PersistentCull.comp.spv -I Engine/Shader/Shaders/
cmake --build build --target 04_Deferred
```

验证 PTG 模式与普通 Dispatch 模式渲染结果一致（通过 CVar 切换）。

---

### Task 5: Device Generated Commands 集成（M3 · Item 1）

**Files:**
- Create: `Engine/RHI/Vulkan/VulkanDGC.h` (DGC 扩展封装)
- Create: `Engine/RHI/Vulkan/VulkanDGC.cpp`
- Modify: `Engine/Render/Pipeline/MeshBatcher.h:27-53` (新增 DGC 模式)
- Modify: `Engine/Render/Pipeline/MeshBatcher.cpp` (DGC 执行路径)
- Modify: `Engine/RHI/Vulkan/VulkanDevice.cpp` (DGC 扩展检测)
- Modify: `Engine/RHI/Public/RHI/RHI.h` (Capabilities 扩展)
- Modify: `Engine/Render/Pipeline/DeferredPipeline.cpp` (DGC vs Indirect 切换)

**Interfaces:**
- Consumes: `VK_EXT_device_generated_commands`, `VK_KHR_buffer_device_address`, `vkCmdExecuteGeneratedCommandsEXT`
- Produces: `VulkanDGC::Initialize/Execute`, `MeshBatcher::UseDGC`, `RHIDeviceCaps::supportsDGC`

- [ ] **Step 1: 扩展 DeviceCaps 查询**

修改 `Engine/RHI/Public/RHI/RHI.h`：

```cpp
struct RHIDeviceCaps {
    // ... 现有成员 ...
    bool supportsDGC = false;  // VK_EXT_device_generated_commands
};
```

修改 `Engine/RHI/Vulkan/VulkanDevice.cpp`，在设备初始化时查询 DGC 支持：

```cpp
// 在 VulkanDevice 初始化中添加
bool CheckDGC_Support(VkPhysicalDevice pd) {
    // 查询 VK_EXT_device_generated_commands 扩展
    uint32_t extCount;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());
    
    for (auto& ext : exts) {
        if (strcmp(ext.extensionName, VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME) == 0) {
            return true;
        }
    }
    return false;
}

// 同时检查 buffer_device_address
bool CheckBDA_Support(VkPhysicalDevice pd) {
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{};
    bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    // ... 查询 ...
    return bda.bufferDeviceAddress;
}
```

- [ ] **Step 2: 创建 VulkanDGC 封装**

创建 `Engine/RHI/Vulkan/VulkanDGC.h`：

```cpp
#pragma once
#include "RHI/RHI.h"
#include <vulkan/vulkan.h>

namespace he::rhi {

// DGC 封装：管理 IndirectCommandsLayout + Preprocess Buffer + Sequences Buffer
class VulkanDGC {
public:
    /// 查询设备是否支持 DGC
    static bool IsSupported(VkPhysicalDevice pd);
    
    /// 创建 DGC 管线
    bool Initialize(VkDevice device, uint32_t maxSequences, 
                    uint32_t maxDraws);
    void Shutdown(VkDevice device);
    
    /// 每帧准备：生成 Preprocess Buffer
    void GenerateCommands(VkCommandBuffer cmd, 
                          VkBuffer cullResultBuffer,
                          VkBuffer vertexBuffer,
                          VkBuffer indexBuffer);
    
    /// 执行 DGC
    void Execute(VkCommandBuffer cmd);
    
    VkIndirectCommandsLayoutEXT GetLayout() const { return m_Layout; }

private:
    VkIndirectCommandsLayoutEXT m_Layout = VK_NULL_HANDLE;
    VkBuffer m_PreprocessBuffer = VK_NULL_HANDLE;
    VkBuffer m_SequencesBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_PreprocessMem = VK_NULL_HANDLE;
    VkDeviceMemory m_SequencesMem = VK_NULL_HANDLE;
    uint32_t m_MaxSequences = 0;
    bool m_Initialized = false;
};

} // namespace he::rhi
```

- [ ] **Step 3: 扩展 MeshBatcher 支持 DGC 路径**

修改 `Engine/Render/Pipeline/MeshBatcher.h`：

```cpp
class MeshBatcher {
public:
    // ... 现有成员 ...
    
    /// DGC 模式开关
    bool useDGC = false;
    
    /// 检查硬件是否支持 DGC（由管线在初始化时查询）
    static bool IsDGC_Available();
    
    /// 获取 DGC 所需的 preprocess buffer（GPU Cull 输出写入此 buffer）
    rhi::IRHIBuffer* GetDGCPreprocessBuffer() const { return m_DGC_PreprocessBuf.get(); }
    
private:
    // DGC 相关 buffer
    std::unique_ptr<rhi::IRHIBuffer> m_DGC_PreprocessBuf;
};

// DGC 模式下 GPU Cull 输出格式（替代 IndirectDrawCommand）
struct alignas(16) DGCDrawToken {
    u32 indexCount;
    u32 instanceCount;
    u32 firstIndex;
    i32 vertexOffset;
    u32 firstInstance;
    u32 objectIndex;     // 额外：物体索引（供 shader 查询材质等）
    u32 _pad;
};
```

- [ ] **Step 4: 集成到 DeferredPipeline**

修改 `Engine/Render/Pipeline/DeferredPipeline.cpp`，在 GBuffer 渲染路径中：

```cpp
// GB_Clear pass 中，根据 DGC 支持选择执行方式
if (m_MeshBatcher.useDGC && m_Device->GetCaps().supportsDGC) {
    // DGC 路径：vkCmdExecuteGeneratedCommandsEXT
    c->ExecuteGeneratedCommands(...);
} else {
    // 传统路径：vkCmdDrawIndexedIndirect
    c->DrawIndexedIndirect(m_GPUCulling.GetIndirectBuffer(), ...);
}
```

- [ ] **Step 5: 添加 DGC CVar 控制**

```cpp
// 通过 CVar 在运行时切换 DGC/Indirect 模式
static int32_t cvDGC_Enable = 0;
HE_CVAR("r.DGC.Enable", cvDGC_Enable, "启用 Device Generated Commands (0=关闭, 1=开启)");
```

- [ ] **Step 6: 验证**

```bash
cmake --build build --target 04_Deferred
./build/Samples/Debug/04_Deferred.exe --validate
# 在 Console 中输入: r.DGC.Enable 1
# 确认 DGC 路径渲染与 Indirect 路径像素级一致
```

---

### Task 6: GPU Work Graph 软件模拟（M3 · Item 5）

**Files:**
- Create: `Engine/Render/Pipeline/GPUWorkGraph.h`
- Create: `Engine/Render/Pipeline/GPUWorkGraph.cpp`
- Create: `Engine/Shader/Shaders/WorkGraph_Entry.comp.slang` (入口节点)

**Interfaces:**
- Consumes: `GPUCulling`, `MeshBatcher` (现有), `IRHIBuffer`, 原子计数器
- Produces: `GPUWorkGraph::AddNode/Execute`, `WorkGraphNode` 类型系统

- [ ] **Step 1: 定义 WorkGraph 基础设施**

创建 `Engine/Render/Pipeline/GPUWorkGraph.h`：

```cpp
#pragma once
#include "RHI/RHI.h"
#include <vector>
#include <memory>
#include <functional>

namespace he::render {

// Work Graph 节点类型
enum class WGNodeType : u8 {
    Entry,     // 入口节点（Compute）
    Compute,   // 通用 Compute 节点
    Draw,      // 绘制节点（Mesh/Draw）
};

// 节点间传递的 Record（GPU 端格式）
struct alignas(16) WGRecord {
    u32 nodeID;       // 目标节点 ID
    u32 payload[7];   // 负载数据（最大 28 bytes）
};

// 节点描述符
struct WGNodeDesc {
    const char* name;
    WGNodeType  type;
    // Shader 路径（用于 Compute/Draw 节点）
    const char* shaderPath;
    u32         maxRecords;     // 最大输入 Record 数
    u32         maxOutputs;     // 最大输出 Record 数
};

// GPU Work Graph — 软件模拟实现
// 使用 Compute Shader + 原子计数器模拟 GPU 端工作调度
class GPUWorkGraph {
public:
    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown(rhi::IRHIDevice* device);
    
    /// 注册节点（返回节点 ID）
    u32 AddNode(const WGNodeDesc& desc);
    
    /// 每帧：将初始工作推入入口节点
    void PushEntryWork(u32 nodeID, const void* records, u32 count);
    
    /// 执行 Work Graph（单帧）
    void Execute(rhi::IRHICommandList* cmd);
    
private:
    struct NodeState {
        WGNodeDesc desc;
        u32 nodeID;
        // GPU 端 Record 队列
        std::unique_ptr<rhi::IRHIBuffer> inputBuffer;   // 输入 Record[]
        std::unique_ptr<rhi::IRHIBuffer> outputBuffer;  // 输出 Record[]
        std::unique_ptr<rhi::IRHIBuffer> counterBuffer; // 原子计数器
    };
    
    std::vector<NodeState> m_Nodes;
    std::unique_ptr<rhi::IRHIBuffer> m_GlobalCounter; // 全局原子计数
    rhi::IRHIDevice* m_Device = nullptr;
    bool m_Initialized = false;
};

} // namespace he::render
```

- [ ] **Step 2: 实现简单 Cull→Draw 链**

创建 `Engine/Render/Pipeline/GPUWorkGraph.cpp`：

```cpp
// 将现有 GPUCulling → MeshBatcher 流程建模为 Work Graph:
//
//  EntryNode (Cull)                     → 输出: 每个可见物体一个 WGRecord
//       records[]
//         ↓
//  DrawNode (Draw)                      → 每个 record 对应一个 Draw
//
// 软件模拟: EntryNode = GPUCull Compute, DrawNode = vkCmdDrawIndexedIndirect

bool GPUWorkGraph::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    m_Initialized = true;
    return true;
}

void GPUWorkGraph::Execute(rhi::IRHICommandList* cmd) {
    // 遍历节点拓扑序，依次执行
    for (auto& node : m_Nodes) {
        switch (node.desc.type) {
        case WGNodeType::Entry:
        case WGNodeType::Compute:
            // Dispatch compute shader
            // 读: node.inputBuffer（上游 Record）
            // 写: node.outputBuffer（下游 Record）
            break;
        case WGNodeType::Draw:
            // 读: node.inputBuffer（Record 中的绘制参数）
            // 执行: vkCmdDrawIndexedIndirect
            break;
        }
    }
}
```

- [ ] **Step 3: 验证软件模拟路径**

创建简单测试：Entry Node 做视锥剔除 → Draw Node 执行绘制，与现有非 Work Graph 路径画面一致。

```bash
cmake --build build --target 04_Deferred
./build/Samples/Debug/04_Deferred.exe
```

---

### Task 7: Forward+ Pipeline（M4 · Item 7）

**Files:**
- Modify: `Engine/Render/Pipeline/ForwardPipeline.cpp` (BuildFrameGraph + Lighting)
- Modify: `Engine/Shader/Shaders/ForwardPBR.frag.slang` (或新增变体)
- Modify: `Engine/Render/Pipeline/ForwardPipeline.h` (新增 ClusteredShading 成员)

**Interfaces:**
- Consumes: `ClusteredShading::BuildClusters/CullLights`, `LightGridCell`, `LightIndexList`
- Produces: Forward+ 模式（Tile-based 光源剔除），与传统 Forward 共存

- [ ] **Step 1: ForwardPipeline 集成 ClusteredShading**

修改 `Engine/Render/Pipeline/ForwardPipeline.h`：

```cpp
#include "Pipeline/ClusteredShading.h"

class ForwardPipeline : public IRenderPipeline {
    // ... 现有成员 ...
    
    /// Forward+ 模式开关
    bool m_UseForwardPlus = true;  // 默认开启
    ClusteredShading m_ClusteredShading;
    std::unique_ptr<rhi::IRHIBuffer> m_LightGridBuffer;
    std::unique_ptr<rhi::IRHIBuffer> m_LightIndexListBuffer;
};
```

- [ ] **Step 2: ForwardPipeline BuildFrameGraph 增加 Light Culling Dispatch**

修改 `Engine/Render/Pipeline/ForwardPipeline.cpp`，在 HDR Pass 定义前增加：

```cpp
// Forward+ Clustered Light Culling（Compute Pass）
if (m_UseForwardPlus) {
    rg.AddPass("ForwardPlus_LightCull",
        {{hdrDepth, ResourceAccess::Read}},
        {},
        [&, w, h](rhi::IRHICommandList* c) {
            // 复用 ClusteredShading 的剔除逻辑
            m_ClusteredShading.BuildClusters(camera.GetInvViewProjMatrix(), w, h, 
                                              camera.GetNear(), camera.GetFar());
            m_ClusteredShading.CullLights(m_CachedLights.data(), 
                                          (u32)m_CachedLights.size());
            
            // 上传 LightGrid + LightIndexList 到 GPU
            // ... 与 DeferredPipeline 相同逻辑 ...
        },
        RGPassQueue::Compute);  // 可异步执行
}
```

- [ ] **Step 3: Forward PBR Shader 增加 LightGrid 路径**

修改 `Engine/Shader/Shaders/ForwardPBR.frag.slang`（或创建变体 `ForwardPBR_Plus.frag.slang`）：

```hlsl
// 新增 bindings（仅 Forward+ 模式使用）
#ifdef FORWARD_PLUS
[[vk::binding(7, 0)]] StructuredBuffer<LightGridCell> u_LightGrid;
[[vk::binding(8, 0)]] StructuredBuffer<uint> u_LightIndexList;

// Cluster 参数
uniform uint u_TilesX;
uniform uint u_TilesY;
uniform uint u_ClusterCount;
uniform float2 u_ScreenSize;
#endif

float3 ShadeForwardPlus(float3 worldPos, float3 albedo, float3 N, float3 V,
                        float metallic, float roughness) {
#ifdef FORWARD_PLUS
    // 从世界坐标计算 cluster 索引
    uint2 tileCoord = uint2(gl_FragCoord.xy / 64);  // kTileSize = 64
    // Z slice 计算（对数分布）
    float depth = /* linear depth */;
    uint slice = /* compute from kDepthSlices */;
    uint clusterIdx = (slice * u_TilesY + tileCoord.y) * u_TilesX + tileCoord.x;
    
    LightGridCell cell = u_LightGrid[clusterIdx];
    float3 color = float3(0.0);
    
    for (uint i = 0; i < cell.count; i++) {
        uint lightIdx = u_LightIndexList[cell.offset + i];
        // PBR 光照计算（仅处理该 cluster 内的光源）
        color += CalculatePBR(u_Lights[lightIdx], albedo, N, V, metallic, roughness);
    }
    return color;
#else
    // 传统路径：遍历所有光源
    // ...
#endif
}
```

- [ ] **Step 4: 创建两种 Shader 变体**

ForwardPipeline 使用两个 PSO 变体：
- `ForwardPBR.frag.slang` — 传统路径（不定义 `FORWARD_PLUS`）
- `ForwardPBR_Plus.frag.slang` — Forward+ 路径（定义 `FORWARD_PLUS`）

或使用 Slang 的特殊化常量在运行时选择。

- [ ] **Step 5: 编译并验证**

```bash
slangc Engine/Shader/Shaders/ForwardPBR.frag.slang -target spirv -o Engine/Shader/Compiled/ForwardPBR.frag.spv -I Engine/Shader/Shaders/
slangc Engine/Shader/Shaders/ForwardPBR_Plus.frag.slang -target spirv -o Engine/Shader/Compiled/ForwardPBR_Plus.frag.spv -I Engine/Shader/Shaders/ -DFORWARD_PLUS
cmake --build build --target Samples  # 构建所有 Sample
./build/Samples/Debug/03_Sponza.exe   # Forward Pipeline
```

验证：Forward+ 路径的光照结果与 Deferred Pipeline 光照结果视觉一致（允许轻微差异由于管线架构不同）。

---

### Task 8: 全链路集成验证与文档更新

**Files:**
- Modify: `docs/HugEngine_Development_Progress.md` (更新 Phase 2 完成度)

**验证清单：**

- [ ] **Step 1: M1 验证（Vulkan 校验层 + AsyncCompute）**

```bash
./build/Samples/Debug/04_Deferred.exe --validate
# 检查项:
# 1. Vulkan 校验层输出无 ERROR/WARNING
# 2. GPU Profiler 面板显示 Async Compute 队列活动
# 3. 帧率不低于 AsyncCompute 关闭时
```

- [ ] **Step 2: M2 验证（剔除对比）**

```bash
# 切换剔除模式对比
# CVar: r.GPUCull.TwoPhase 0/1
# CVar: r.GPUCull.PTG 0/1
# 对比四种组合的渲染结果一致性和帧率
```

- [ ] **Step 3: M3 验证（DGC + Work Graph）**

```bash
# CVar: r.DGC.Enable 0/1
# 对比 DGC 与 Indirect 路径渲染结果像素级一致
# CVar: r.WorkGraph.Enable 0/1
# 对比 Work Graph 路径渲染正确性
```

- [ ] **Step 4: M4 验证（Forward+）**

```bash
./build/Samples/Debug/03_Sponza.exe
# 多光源场景（> 50 点光源）下 Forward+ 帧率应显著高于传统 Forward
```

- [ ] **Step 5: 更新进度文档**

更新 `docs/HugEngine_Development_Progress.md`：
- Phase 2 GPU Driven 完成度: 80% → 100%
- 新增 DGC / Two-Phase / PTG / Work Graph / Forward+ 条目

---

## 完成标准

| 里程碑 | 核心交付 | 验证标准 |
|--------|----------|----------|
| M1 | GPUCull 时序修复 + AsyncCompute 开启 | 校验层零告警 + Async 队列有 GPU Profiler 时间戳 |
| M2 | Two-Phase + PTG 剔除 | 剔除对比视图无差异 + Dispatch 次数显著减少 |
| M3 | DGC + Work Graph 软件模拟 | DGC/Indirect 切换无视觉差异 + Work Graph 路径正确 |
| M4 | Forward+ | 多光源场景帧率提升 + 与 Deferred 光照一致 |
