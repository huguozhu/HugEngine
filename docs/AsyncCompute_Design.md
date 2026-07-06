# RHI AsyncCompute 接口设计

> **状态**: Steps 1-5 已实现，AsyncCompute 默认关闭（待多阶段提交架构）
> **最后更新**: 2026-07-06
> **关联文档**: [HugEngine_Architecture_And_Tasks.md](HugEngine_Architecture_And_Tasks.md)

---

## 0. 当前状态与已知问题

### AsyncCompute 默认关闭

**原因**: 正确的跨队列同步需要将 Graphics 命令列表拆分为三个阶段提交：

```
Submit #1 (Graphics): QFOT Release → Timeline Signal(fence_A)
Submit #2 (Compute):  Wait(fence_A) → Acquire → Compute Work → Release → Signal(fence_B)
Submit #3 (Graphics): Wait(fence_B) → Acquire → 后续渲染 Pass
```

当前架构中整个 Graphics 帧在一个命令列表中，无法在中间插入信号量同步点。
单个 vkQueueSubmit 内的 RELEASE→ACQUIRE 背靠背执行，但此时 Compute 还没完成工作，
导致 ACQUIRE 在 Compute 尚未 RELEASE 时就尝试获取所有权 → undefined behavior → 白屏。

**解决方向**:
1. RenderGraph 支持分阶段提交（将 Pass 拆分为多个 Submission Group）
2. 或使用 `VK_SHARING_MODE_CONCURRENT` 创建跨队列资源，避免 QFOT
3. 或使用 Vulkan 1.3 的原生多队列同步扩展

**启用方式**（调试用，已知白屏）: DeferredPipeline.cpp 中 `#if 0` → `#if 1`

## 目录

1. [概述](#1-概述)
2. [RHI Types 层 — 新增类型](#2-rhi-types-层--新增类型)
3. [RHI Device 层 — 接口扩展](#3-rhi-device-层--接口扩展)
4. [RHI Barrier 扩展 — 跨队列所有权转移](#4-rhi-barrier-扩展--跨队列所有权转移)
5. [Vulkan 后端改动](#5-vulkan-后端改动)
6. [RenderGraph 层 — 异步调度](#6-rendergraph-层--异步调度)
7. [管线使用示例](#7-管线使用示例)
8. [实现步骤](#8-实现步骤)

---

## 1. 概述

### 1.1 目标

让 GPU 同时执行 Compute 和 Graphics 工作，利用现代 GPU 的异构多队列能力，将 GPU 占用率从 ~70% 提升到 90%+，帧时间缩短 10-30%。

### 1.2 当前执行模式 vs 目标

```
当前 (全部串行 Graphics Queue):
  ShadowDepth ─► GBuffer ─► DepthPyramid ─► Culling ─► Lighting ─► SSR/SSGI ─► TAA ─► ToneMap

AsyncCompute 后:
  [Graphics Q]  ShadowDepth ─► GBuffer ─►────────► Lighting ─►──► TAA ─► ToneMap
  [Compute  Q]                 D.Pyramid ─► Culling ─►        SSR/SSGI ─► Denoise
                                   (与 GBuffer 并行)    (与 Lighting 并行)
```

### 1.3 设计原则

- **最小侵入** — 复用现有 `QueueType` 枚举和 `CreateCommandList(QueueType)` 签名，不改动 `IRHICommandList` 接口核心方法
- **渐进式** — 向后兼容，默认行为不变（仍是同步 Graphics Queue）
- **RHI 层只提供原语** — Async 调度逻辑在 RenderGraph 层，RHI 只暴露队列 + 信号量 + 跨队列 Barrier

---

## 2. RHI Types 层 — 新增类型

```cpp
// ============================================
// 文件: Engine/RHI/Public/RHI/Types.h 新增内容
// ============================================

// 异步计算能力查询 — 追加到 DeviceCaps
struct DeviceCaps {
    // ... 现有字段保持不变 ...

    // 新增: 异步计算支持
    bool supportsAsyncCompute = false;   // GPU 是否有独立 Compute 队列
    bool supportsTransferQueue = false;  // GPU 是否有独立 Copy 队列 (DMA)
    u32  asyncComputeTier = 0;          // 0=不支持, 1=独立队列族, 2=专用硬件引擎
};
```

```cpp
// ============================================
// 文件: Engine/RHI/Public/RHI/Types.h 新增内容
// ============================================

// 跨队列同步原语 — 平台无关的信号量句柄
// Vulkan: 封装 VkSemaphore (Timeline Semaphore)
// D3D12:  封装 ID3D12Fence
using RHIFenceHandle = u64;  // 内部 opaque handle
constexpr RHIFenceHandle kInvalidFence = 0;
```

---

## 3. RHI Device 层 — 接口扩展

```cpp
// ============================================
// 文件: Engine/RHI/Public/RHI/RHI.h — IRHIDevice 新增方法
// ============================================

class IRHIDevice {
public:
    // --- 现有方法不变 ---

    // === 新增: 多队列支持 ===

    // 查询是否支持独立 Compute 队列 (在 CreateDevice 之后调用)
    virtual bool HasAsyncComputeQueue() const = 0;

    // 获取指定队列族的索引 (用于跨队列 Barrier 时的 ownership transfer)
    virtual u32 GetQueueFamily(QueueType queue) const = 0;

    // === 新增: 跨队列同步原语 ===

    // 创建跨队列信号量 (Timeline Semaphore / D3D12 Fence)
    // Vulkan: VK_SEMAPHORE_TYPE_TIMELINE + 初始值 0
    // D3D12:  CreateFence(0, D3D12_FENCE_FLAG_SHARED)
    virtual RHIFenceHandle CreateFence() = 0;

    // 销毁信号量
    virtual void DestroyFence(RHIFenceHandle fence) = 0;

    // CPU 端等待信号量到达指定值 (用于管线插入点)
    // timeout: 超时(纳秒), 0 表示不等待直接返回, UINT64_MAX 表示无限等待
    virtual bool WaitForFence(RHIFenceHandle fence, u64 value, u64 timeout = UINT64_MAX) = 0;

    // CPU 端查询当前信号量值
    virtual u64 GetFenceValue(RHIFenceHandle fence) const = 0;

    // GPU 端信号: 在指定队列上提交命令后发出信号
    // 由 CommandList::Submit 或内部队列提交后自动处理
    virtual void SignalFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) = 0;

    // GPU 端等待: 指定队列等待信号量到达指定值后才开始执行
    virtual void WaitFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) = 0;

    // === 现有 Submit 保持不变，新增批量提交 (可选) ===

    // 批量提交多个 CommandList (可能来自不同队列)
    // 内部按队列分组后分别 vkQueueSubmit / ExecuteCommandLists
    virtual void SubmitAll(Span<IRHICommandList*> cmdLists) = 0;
};
```

---

## 4. RHI Barrier 扩展 — 跨队列所有权转移

```cpp
// ============================================
// 文件: Engine/RHI/Public/RHI/CommandList.h — IRHICommandList 新增方法
// ============================================

class IRHICommandList {
public:
    // --- 现有方法不变 ---

    // === 新增: 跨队列 Barrier ===

    // 资源跨队列所有权转移
    // 当资源从 Graphics 队列移交给 Compute 队列 (或反向) 时调用
    // Vulkan: PipelineBarrier 中设置 srcQueueFamilyIndex != dstQueueFamilyIndex
    // D3D12:  发出 ResourceBarrier with D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
    virtual void QueueOwnershipTransfer(
        IRHITexture* texture,
        QueueType srcQueue,
        QueueType dstQueue,
        ResourceState currentState,
        ResourceState newState) = 0;

    virtual void QueueOwnershipTransfer(
        IRHIBuffer* buffer,
        QueueType srcQueue,
        QueueType dstQueue,
        ResourceState currentState,
        ResourceState newState) = 0;

    // 简化版: 常用模式 — 释放资源到目标队列，保持原状态
    // srcQueue = 当前命令列表所属队列 (自动推断)
    virtual void ReleaseToQueue(IRHITexture* texture, QueueType dstQueue) = 0;
    virtual void AcquireFromQueue(IRHITexture* texture, QueueType srcQueue) = 0;
};
```

---

## 5. Vulkan 后端改动

```cpp
// ============================================
// 文件: Engine/RHI/Vulkan/VulkanInternal.h — VulkanDevice 改动
// ============================================

class VulkanDevice final : public IRHIDevice {
private:
    // --- 现有成员保持不变 ---
    VkQueue  m_GraphicsQueue = VK_NULL_HANDLE;
    u32      m_GraphicsFamily = 0;

    // === 新增: 独立 Compute 队列 ===
    VkQueue  m_ComputeQueue = VK_NULL_HANDLE;    // 独立 Compute-Only 队列
    u32      m_ComputeFamily = 0;                 // 专用 Compute Queue Family Index
    bool     m_HasAsyncCompute = false;           // 是否真正有独立队列

    // === 新增: Timeline Semaphore 支持 ===
    // 每个 Fence 对应一个 VkSemaphore (TIMELINE 类型)
    struct FenceState {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        u64         currentValue = 0;
    };
    std::vector<FenceState> m_Fences;  // handle → FenceState

    // --- 新增方法 ---
    void FindQueueFamilies();                      // 查询所有队列族
    void CreateComputeQueue();                     // 尝试获取独立 Compute 队列
    VkSemaphore CreateTimelineSemaphore(u64 initialValue);
};
```

```cpp
// 队列族查询新逻辑 (替换现有的单队列查询)
void VulkanDevice::FindQueueFamilies() {
    // 1. 查找 Graphics + Present 队列族 (必须)
    m_GraphicsFamily = FindQueueFamily(m_Physical,
        VK_QUEUE_GRAPHICS_BIT, m_Surface);

    // 2. 尝试查找独立 Compute 队列族 (不带 GRAPHICS_BIT)
    //    优先选只有 COMPUTE_BIT 的族 → 专用硬件引擎
    //    回退: 不同的 COMPUTE_BIT 队列族 → 独立调度
    //    无回退: AsyncCompute = false
    u32 computeOnlyFamily = UINT32_MAX;
    u32 computeDedicatedFamily = UINT32_MAX;

    u32 queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_Physical, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_Physical, &queueFamilyCount, families.data());

    for (u32 i = 0; i < queueFamilyCount; i++) {
        VkQueueFlags flags = families[i].queueFlags;
        bool hasCompute = flags & VK_QUEUE_COMPUTE_BIT;
        bool hasGraphics = flags & VK_QUEUE_GRAPHICS_BIT;
        bool hasTransfer = flags & VK_QUEUE_TRANSFER_BIT;

        if (hasCompute && !hasGraphics && i != m_GraphicsFamily) {
            // 最佳选择: 纯 Compute 队列族 (异步硬件引擎)
            if (!hasTransfer) {
                computeOnlyFamily = i;
                break;  // 找到最优,停止
            }
            // 次选: Compute+Transfer 族
            if (computeDedicatedFamily == UINT32_MAX) {
                computeDedicatedFamily = i;
            }
        }
    }

    if (computeOnlyFamily != UINT32_MAX) {
        m_ComputeFamily = computeOnlyFamily;
        m_HasAsyncCompute = true;
        m_Caps.asyncComputeTier = 2;  // 专用硬件引擎
    } else if (computeDedicatedFamily != UINT32_MAX) {
        m_ComputeFamily = computeDedicatedFamily;
        m_HasAsyncCompute = true;
        m_Caps.asyncComputeTier = 1;  // 独立队列族
    } else {
        // 无独立 Compute 队列 → 回退到 Graphics 队列族
        m_ComputeFamily = m_GraphicsFamily;
        m_HasAsyncCompute = false;
        m_Caps.asyncComputeTier = 0;
    }

    m_Caps.supportsAsyncCompute = m_HasAsyncCompute;
}
```

---

## 6. RenderGraph 层 — 异步调度

```cpp
// ============================================
// 文件: Engine/Render/RenderGraph.h — RenderGraph 改动
// ============================================

// 新增: Pass 队列类型提示
enum class RGPassQueue : u8 {
    Default  = 0,  // 跟随 RenderGraph 默认队列 (Graphics)
    Graphics = 1,  // 显式 Graphics 队列
    Compute  = 2,  // 显式 Compute 队列 → AsyncCompute 候选
};

class RenderGraph {
public:
    // === AddPass 重载: 新增 queueHint 参数 ===
    PassNode* AddPass(
        StringView name,
        std::vector<PassResource> reads = {},
        std::vector<PassResource> writes = {},
        PassExecuteFunc execute = nullptr,
        RGPassQueue queueHint = RGPassQueue::Default  // 新增参数
    );

    // === Execute 签名变更: 支持多命令列表 ===
    // Compile() 阶段决定 Pass 分配到哪个队列
    // Execute() 按队列分组执行
    void Execute(rhi::IRHICommandList* graphicsCmd,      // Graphics 队列
                 rhi::IRHICommandList* computeCmd,        // Compute 队列 (可为同一对象)
                 rhi::IRHIDevice* device);

    // === 新增: 查询是否启用了 AsyncCompute ===
    bool IsAsyncComputeEnabled() const;
    void SetAsyncComputeEnabled(bool enabled);
};
```

```cpp
// ============================================
// 文件: Engine/Render/RenderGraph.cpp — Compile 阶段核心逻辑
// ============================================

void RenderGraph::Compile() {
    // ... 现有逻辑: DAG 构建, 资源生命周期, Barrier 推导 ...

    if (m_AsyncComputeEnabled && m_Device->HasAsyncComputeQueue()) {
        // 新增: 异步调度分析
        ScheduleAsyncPasses();
    }
}

void RenderGraph::ScheduleAsyncPasses() {
    // 对每个 queueHint == Compute 的 Pass:
    //   1. 分析依赖 — 该 Pass 的 reads/writes 与前后 Graphics Pass 的冲突
    //   2. 如果 writes 不与后续 Graphics Pass 的 reads 重叠 → 可异步
    //   3. 在 Compile 的 barrier list 中插入跨队列 ownership transfer:
    //      - Pass 开始前: Graphics→Compute (Acquire)
    //      - Pass 结束后: Compute→Graphics (Release)

    for (auto* pass : m_CompiledPasses) {
        if (pass->queueHint != RGPassQueue::Compute) continue;

        // 检查资源依赖: 该 Pass 的输出是否被后面的 Graphics Pass 读取
        bool canAsync = true;
        for (auto& write : pass->writes) {
            for (auto* subsequentPass : GetSubsequentPasses(pass)) {
                if (subsequentPass->queueHint == RGPassQueue::Compute) continue; // Comp-to-Comp 无需转移
                for (auto& read : subsequentPass->reads) {
                    if (write.resource == read.resource) {
                        canAsync = false; // 有依赖，不可完全异步
                        break;
                    }
                }
            }
        }

        if (canAsync) {
            pass->asyncSchedule = true; // 标记为可并行
            // 自动插入跨队列 Barrier
            InsertCrossQueueBarrier(pass, QueueType::Graphics, QueueType::Compute);
        } else {
            // 存在依赖 → 必须插入同步点 (在 Compute 提交前等待 Graphics)
            pass->requiresSync = true;
        }
    }
}
```

---

## 7. 管线使用示例

```cpp
// ============================================
// DeferredPipeline 使用 AsyncCompute 示例
// ============================================

void DeferredPipeline::BuildFrameGraph(RenderGraph& rg, ...) {
    // GBuffer Pass — Graphics 队列
    rg.AddPass("GBuffer", 
        { /* reads */ }, 
        { gbufferAlbedo, gbufferNormal, gbufferDepth },  // writes
        [&](IRHICommandList* cmd) { /* Draw GBuffer */ },
        RGPassQueue::Graphics  // 显式标记 (默认值)
    );

    // Hi-Z 深度金字塔 — Compute 队列, 可与后续 Graphics Pass 并行
    rg.AddPass("DepthPyramid",
        { gbufferDepth },                                // read depth
        { depthPyramid },                                // write pyramid
        [&](IRHICommandList* cmd) {
            for (u32 mip = 1; mip < kPyramidLevels; mip++)
                cmd->Dispatch(...);  // Compute-only 命令
        },
        RGPassQueue::Compute  // ★ 标记为异步 Compute Pass
    );

    // GPU Culling — Compute 队列, 可与 Shadow Pass 并行
    rg.AddPass("GPU_Cull",
        { depthPyramid, sceneData },
        { indirectCmdBuf, visibleList },
        [&](IRHICommandList* cmd) { /* Dispatch culling */ },
        RGPassQueue::Compute
    );

    // Shadow Pass — Graphics 队列
    rg.AddPass("ShadowDepth", ..., RGPassQueue::Graphics);

    // Lighting Pass — Graphics 队列 (等待 GPU_Cull 完成)
    rg.AddPass("Lighting",
        { gbufferAlbedo, gbufferNormal, visibleList },
        { hdrTarget },
        [&](IRHICommandList* cmd) { /* Clustered Lighting */ },
        RGPassQueue::Graphics
    );

    // SSGI — Compute 队列 (与 PostProcess 并行)
    rg.AddPass("SSGI", ..., RGPassQueue::Compute);

    // Denoise — Compute 队列
    rg.AddPass("Denoise", ..., RGPassQueue::Compute);

    // PostProcess — Graphics 队列
    rg.AddPass("Tonemap", ..., RGPassQueue::Graphics);

    // Compile 自动分析:
    //   DepthPyramid + GPU_Cull 与 ShadowDepth 无直接依赖 → 可并行
    //   GPU_Cull → Lighting 有依赖 → 自动插入同步点
    //   SSGI + Denoise 与 Tonemap 无直接依赖 → 可并行

    rg.Compile();
    rg.Execute(graphicsCmdList, computeCmdList, device);
}
```

执行时间线：
```
[Graphics Q]  GBuffer ─► ShadowDepth ─►─► Lighting ─►────► Tonemap
[Compute  Q]             D.Pyramid ─► Culling ─► SSGI ─► Denoise
                                      ↑                ↑
                                  GPU_Cull→Lighting   (无依赖, 完全并行)
                                  有依赖: 同步等待
```

---

## 8. 实现步骤

| 步骤 | 内容 | 预估 | 状态 |
|------|------|:---:|:---:|
| Step 1 | RHI Types: 补充 `DeviceCaps.asyncCompute*`，新增 `RHIFenceHandle` | 0.5d | ✅ 已完成 |
| Step 2 | Vulkan: 队列族查询 → 独立 Compute 队列创建 → Timeline Semaphore | 1.5d | ✅ 已完成 |
| Step 3 | RHI Device: 实现 `HasAsyncComputeQueue()`, `CreateFence/DestroyFence`, `WaitForFence`, `SignalFenceOnQueue/WaitFenceOnQueue` | 1d | ✅ 已完成 |
| Step 4 | RHI Barrier: 实现 `QueueOwnershipTransfer()` (Vulkan: 设置 queueFamilyIndex; D3D12: ResourceBarrier) | 0.5d | ✅ 已完成 |
| Step 5 | RenderGraph: `RGPassQueue` 枚举 + `AddPass(queueHint)` + `ScheduleAsyncPasses()` | 1.5d | ✅ 已完成 |
| Step 6 | 管线接入: GPU_Cull + SSGI + Denoise + DepthPyramid → 标记 Compute Pass | 0.5d | ✅ 已完成 |
| Step 7 | 测试验证: RenderDoc GPU 时间线验证并行执行 | 0.5d | ⏳ 待验证 |

**总计约 6 个工作日**（仅 Vulkan 后端，不含 D3D12）。

### 8.1 实际接入情况

当前引擎中真正使用 Compute Shader（`Dispatch()`）的 Pass：

| Pass | 管线 | 状态 |
|------|------|:---:|
| GPU_Cull | DeferredPipeline | ✅ 已标记 `RGPassQueue::Compute` |
| DDGI_Update | DeferredPipeline | ✅ 已标记 `RGPassQueue::Compute` |
| GPU_Cull | ForwardPipeline | ⚠️ 未使用 RenderGraph，直接内联 Dispatch |

SSR/SSGI/SSAO/Denoise/Bloom/DOF/MotionBlur 当前使用全屏三角形 + Graphics Pipeline（`BeginOffscreenPass`），并非 Compute Shader。待后续重构为 Compute Shader 后可标记为 `RGPassQueue::Compute`。

---

> **文档版本**: v1.0
> **基于**: [HugEngine_Architecture_And_Tasks.md](HugEngine_Architecture_And_Tasks.md) v1.0
