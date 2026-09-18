# HugEngine 多线程架构分析

> 分析日期：2026-07-17 | 分析范围：Core/Threading、Render/SceneRenderer、Render/Pipeline、Render/RenderGraph

---

## 一、总体架构概览

HugEngine 的多线程架构分为 **五个层次**，从上到下：

```
┌─────────────────────────────────────────────────────────────┐
│ 5. 应用层 (Samples)                                          │
│    主线程事件循环 + 帧渲染                                     │
├─────────────────────────────────────────────────────────────┤
│ 4. 渲染管线层 (ForwardPipeline / DeferredPipeline)            │
│    ParallelInvoke 多线程录制命令到 Secondary CommandBuffer    │
├─────────────────────────────────────────────────────────────┤
│ 3. 场景准备层 (SceneRenderer::Prepare)                        │
│    ParallelForChunked 并行视锥剔除                             │
├─────────────────────────────────────────────────────────────┤
│ 2. 辅助服务层 (ShaderHotReload)                               │
│    独立 std::thread 文件监控线程                               │
├─────────────────────────────────────────────────────────────┤
│ 1. 任务系统层 (JobSystem → Taskflow)                          │
│    全局工作窃取线程池                                          │
└─────────────────────────────────────────────────────────────┘
```

**线程总数**：
- 主线程 × 1
- Taskflow 工作线程 × N（N = `std::thread::hardware_concurrency()`，通常 8-16）
- Shader HotReload 监控线程 × 1
- **总计**: 约 10-18 个系统线程

---

## 二、任务系统：JobSystem（Core 层）

### 2.1 设计

`Engine/Core/Threading/JobSystem.h` 是对 **Taskflow** 的薄封装，提供引擎内统一的任务并行接口。

```cpp
class JobSystem {
private:
    u32                              m_ThreadCount;     // 工作线程数
    std::unique_ptr<tf::Taskflow>    m_Taskflow;        // Taskflow 任务图
    std::unique_ptr<tf::Executor>    m_Executor;        // 工作窃取执行器
    static std::unique_ptr<JobSystem> s_Instance;       // 全局单例
};
```

### 2.2 API 矩阵

| API | 语义 | 底层实现 | 使用场景 |
|-----|------|----------|----------|
| `Submit(job)` | 发射后忘记 | `executor->silent_async()` | 不需要等待的一次性工作 |
| `ParallelFor(count, body)` | 等分并行 | 拆分为 count 个任务 → `ParallelInvoke` | 简单逐元素并行 |
| `ParallelForChunked(count, chunkSize, body)` | 分块并行 | `numChunks = (count+chunkSize-1)/chunkSize` | **主要使用的 API** |
| `ParallelInvoke(tasks)` | 一批任务全部并行，等待全部完成 | `silent_async` × N + `wait_for_all` | 多线程命令录制 |
| `Async<T>(task)` | 异步 + future | `executor->async()` | 需要返回值的异步工作 |
| `WaitAll()` | 等待全部 | `executor->wait_for_all()` | 帧边界同步 |
| `IsWorkerThread()` | 查询当前线程身份 | 始终返回 `false`（TF 未暴露此信息） | 调试诊断 |

### 2.3 线程数管理

```cpp
// 默认构造 → hardware_concurrency
JobSystem::JobSystem() : JobSystem(std::thread::hardware_concurrency()) {}

// 显式构造
JobSystem::JobSystem(u32 threadCount)
    : m_ThreadCount(threadCount > 0 ? threadCount : 1)  // 最少 1 线程

// EngineConfig 中配置
EngineConfig config;
config.jobThreads = 0;  // 0 = auto-detect
```

### 2.4 Taskflow 集成细节

```cmake
# Engine/Core/CMakeLists.txt
target_link_libraries(HugEngineCore PUBLIC Taskflow)
```

Taskflow 的核心特性：
- **工作窃取 (Work Stealing)** — 线程间自动负载均衡
- **无锁任务队列** — 每线程独立队列 + 窃取
- **任务图 (Task Graph)** — `tf::Taskflow` 支持声明式 DAG 依赖

> 注：HugEngine 当前未直接使用 `tf::Taskflow` 的图能力，均通过 `silent_async` + `wait_for_all` 模式使用简单并行。

### 2.5 便捷模板

```cpp
// ParallelForEach — 自动判断是否并行
template<typename Container, typename Func>
void ParallelForEach(Container& container, Func&& func) {
    auto count = static_cast<u32>(std::size(container));
    if (count > 1024)  // 阈值判断：小数据量串行避免调度开销
        JobSystem::Instance().ParallelFor(count, [&](u32 i) { func(container[i]); });
    else
        for (u32 i = 0; i < count; ++i) func(container[i]);
}
```

### 2.6 启动与关闭顺序

```cpp
Engine::Initialize():
  ├─ 1. Logger::Initialize        // 日志系统最先
  ├─ 2. JobSystem::Initialize()   // 任务系统第二，创建线程池
  └─ 3. Window (GLFW)             // 窗口创建

Engine::Shutdown():
  ├─ 1. Window.~()
  ├─ 2. JobSystem::Shutdown()     // 调用 executor->wait_for_all() 等待所有任务完成
  └─ 3. Logger::Shutdown()
```

---

## 三、场景准备：并行视锥剔除

`Engine/Render/SceneRenderer.cpp` — 帧开始的 CPU 并行准备阶段。

### 3.1 执行流程

```
SceneRenderer::Prepare(world, sg, camera, objectBuffer)
  │
  ├─ Step 1: 单线程遍历 Entity → 收集所有 MeshComponent/Cube/Sphere
  │    └─ world.ForEach<MeshComponent>() → entries[]
  │
  ├─ Step 2: 并行视锥剔除
  │    └─ JobSystem::Instance().ParallelForChunked(total, 64, [&](start, end) {
  │         // 每个 chunk: 独立 local 向量，避免锁争用
  │         std::vector<u32> local;
  │         for (i = start; i < end; ++i)
  │             if (frustum.Intersects(entries[i].worldBounds))
  │                 local.push_back(i);
  │         // 只在需要合并结果时加锁
  │         if (!local.empty()) {
  │             std::lock_guard<std::mutex> lk(mtx);
  │             visibleIdx.insert(visibleIdx.end(), local.begin(), local.end());
  │         }
  │       })
  │
  └─ Step 3: 单线程上传 GPU 数据
       ├─ objectBuffer->Map() → GPUObjectData*
       ├─ 遍历 visibleIdx → PBRMaterial 数据填充
       └─ 构建 DrawItem 列表 → 返回给管线
```

### 3.2 多线程优化要点

| 设计 | 说明 |
|------|------|
| **chunkSize = 64** | 每个线程处理 64 个实体，避免任务过多导致的调度开销 |
| **本地合并** | `local` 向量收集本 chunk 的结果，只在 chunk 有产出时才加锁写入全局 |
| **std::mutex** | 简单的排他锁，只在产生实体时才加锁，热点数据（空 chunk）几乎无竞争 |
| **简化版 AABB-Frustum** | `frustum.Intersects()` 基于 NDC 空间的符号位简化计算，适合快速剔除 |

### 3.3 潜在优化点

| 问题 | 当前状态 | 改进方向 |
|------|----------|----------|
| **Entity 遍历单线程** | 串行遍历 World 所有实体 | 对大型场景可能成为瓶颈 |
| **GPU 数据上传串行** | 只有剔除是并行的，Upload 在主线程 | 可使用 staging buffer + 多线程 memcpy |
| **无 SIMD** | `frustum.Intersects()` 无 SIMD 优化 | 可加入 SSE/AVX 加速 |

---

## 四、多线程命令录制（MTCR）

这是 HugEngine 最核心的渲染多线程机制。实现在 `ForwardPipeline::Render()` 中。

### 4.1 预分配阶段（管线初始化时）

```cpp
// ForwardPipeline::Initialize()
if (m_MultiThreadRecord) {
    u32 threadCount = JobSystem::Instance().GetThreadCount();  // 如 16
    u32 secCount = std::min(kMaxSecRecordLists, std::max(threadCount, 1u));
    // secCount = min(8, max(16, 1)) = 8

    for (u32 i = 0; i < secCount; ++i) {
        auto secCL = device->CreateSecondaryCommandList();  // Vulkan Secondary CB
        m_SecRecordLists.push_back(std::move(secCL));
    }
}
```

每个 Secondary CommandBuffer **整个帧生命周期复用**，不分配不销毁。

### 4.2 录制阶段（每帧 Render 时）

```
ForwardPipeline::Render(cmd, ...)
  │
  ├─ 1. SceneRenderer::Prepare() → filteredItems[]
  │      └─ 并行视锥剔除（见第三章）
  │
  ├─ 2. 如果启用多线程录制:
  │    ├─ numThreads = min(secRecLists.size(), totalDraws)  // 最多 8 线程
  │    ├─ chunkSize = (totalDraws + numThreads - 1) / numThreads
  │    │
  │    └─ ParallelInvoke([&](t=0..numThreads-1) {
  │         ├─ start = t * chunkSize, end = min(start+chunk, totalDraws)
  │         ├─ auto& secCmd = m_SecRecordLists[t]
  │         │
  │         ├─ secCmd->BeginSecondary(m_PBR_PSO)  // 设置 RP 继承信息
  │         ├─ secCmd->SetViewport(...)
  │         ├─ secCmd->SetScissor(...)
  │         │
  │         ├─ for (i = start; i < end; ++i) {
  │         │     auto& item = filteredItems[i]
  │         │     secCmd->BindDescriptorSet(0, bindlessSet)
  │         │     secCmd->SetPushConstants(...)
  │         │     secCmd->BindDescriptorSet(1, perDrawSet)
  │         │     secCmd->DrawIndexed(...)
  │         │   }
  │         │
  │         └─ secCmd->End()
  │       })
  │
  ├─ 3. 主命令缓冲合并:
  │    for (t = 0; t < numThreads; ++t)
  │        cmd->ExecuteSecondary(m_SecRecordLists[t].get())
  │
  └─ 4. (else) 单线程回退路径
       for (auto& item : filteredItems)
           cmd->DrawIndexed(...)
```

### 4.3 Secondary CB 的多线程安全

```
VulkanCommandList 的 Sec CB 设计:

构造时预分配 kMaxSecondaryCBs = 3 个 VkCommandBuffer:
  m_SecCmdBuffers[0], m_SecCmdBuffers[1], m_SecCmdBuffers[2]

BeginSecondary(PSO) → 轮转选择一个空闲 CB:
  idx = (m_SecSlot++) % kMaxSecondaryCBs
  设置 VkCommandBufferInheritanceInfo { renderPass, subpass }

ExecuteSecondary(other) → vkCmdExecuteCommands(cb, 1, &other->m_SecCmdBuffers[idx])

关键: 每个线程独占一个 m_SecRecordLists[t]，不存在竞争
```

### 4.4 MTCR 架构总结

| 维度 | 值 |
|------|-----|
| 并行粒度 | Draw 命令录制 |
| 线程数上限 | 8 (`kMaxSecRecordLists`) |
| 每线程录制 | VkCommandBufferLevel::SECONDARY |
| 合并方式 | 主线程串行 `vkCmdExecuteCommands` |
| 同步原语 | `ParallelInvoke` 隐式 barrier（等待全部完成） |
| 开关 | `EngineConfig::enableMultiThreadRecord`（默认 true） |
| 回退 | ≤ 0 个绘制时自动跳过 |

### 4.5 ForwardPipeline vs DeferredPipeline

| 管线 | 多线程录制 |
|------|:---:|
| **ForwardPipeline** | ✅ 完整实现 |
| **DeferredPipeline** | ❌ 未实现（走 RenderGraph 路径，通过 AsyncCompute 实现 GPU 并行） |

---

## 五、GPU 并行：AsyncCompute

实现在 RenderGraph 层（非 Core 线程层），但属于多线程架构的关键部分。

### 5.1 多阶段提交模型

```
RenderGraph::ExecuteWithAsyncCompute(mainCmd, device, ...)
  │
  ├─ Phase 1: 创建 computeCmd (独立 Compute 队列)
  │    └─ device->CreateCommandList(QueueType::Compute)
  │
  ├─ Phase 2: 在 computeCmd 上录制所有 Compute Pass
  │    ├─ GPU Culling、SSAO、AutoExposure 等
  │    ├─ 跨队列 Barrier: Graphics→Compute (QueueOwnershipTransfer)
  │    └─ 跨队列 Barrier: Compute→Graphics (Release)
  │
  ├─ Phase 3: 同步点
  │    ├─ computeCmd->SetTimelineSignal(fence, timelineValue)
  │    └─ mainCmd->SetTimelineWait(fence, timelineValue)
  │
  ├─ Phase 4: computeCmd->End() + Submit()
  │    └─ 异步提交到 Compute VkQueue
  │
  └─ Phase 5: 在 mainCmd 上录制所有 Graphics Pass
       └─ mainCmd->Submit() 时自动等待 Timeline Wait
```

### 5.2 调度分析

`ScheduleAsyncPasses()` 在编译阶段分析每个 Pass：

```
for each Compute Pass:
  ├─ 检查输出是否被后续 Graphics Pass 读取 (RAW)
  │   └─ 是 → requiresSync = true (不可异步，需要 GPU 同步点)
  │   └─ 否 → asyncSchedule = true (可安全异步)
  │
  └─ 对可异步 Pass: 自动插入 crossQueueAcquire/crossQueueRelease Barrier
```

---

## 六、Shader 热重载线程

`Engine/Render/ShaderHotReload.h` — 独立的专用线程。

### 6.1 架构

```
┌─────────────────────────────────┐
│  m_WatchThread (独立 std::thread) │
│  ├─ ReadDirectoryChangesW        │
│  ├─ 检测 .slang 文件变化          │
│  ├─ 调用 slangc 编译              │
│  └─ lock(m_Mutex) →              │
│      m_Pending.push({name, spirv})│
└───────────┬─────────────────────┘
            │
    ┌───────▼──────────┐
    │  主线程 Poll()    │
    │  └─ swap(m_Pending)│ → 消费重载的 SPIR-V
    │     └─ onReload    │ → 重建 PSO
    └──────────────────┘
```

### 6.2 关键设计

| 设计 | 说明 |
|------|------|
| **独立线程** | `std::thread` 而非 Taskflow，生命周期独立于帧循环 |
| **200ms Debounce** | `lastChange[name] + 200ms` 判断，避免文件保存过程中反复触发编译 |
| **无锁通知** | `m_Running` 为 `std::atomic<bool>`，线程循环用 `WaitForSingleObject(hEvent, 500ms)` 定期检查退出标志 |
| **主线程回调** | 编译完成的 SPIR-V 通过 `m_Pending` 队列传递，主线程 `Poll()` 消费（无竞争） |
| **slangc 进程** | 每个变动的 shader 启动一次 `CreateProcess("slangc ...")`，等待编译完成读取 .spv |

---

## 七、线程架构全景图

```
                            ┌──────────────────────────────────┐
                            │   Taskflow Thread Pool (N 线程)  │
                            │  ┌─────┐┌─────┐┌───┐┌───┐┌───┐  │
                            │  │ W 0 ││ W 1 ││...││...││W N│  │
                            │  └──┬──┘└──┬──┘└─┬─┘└─┬─┘└─┬─┘  │
                            └─────┼──────┼─────┼────┼────┼─────┘
                                  │      │     │    │    │
                    ┌─────────────┤  ┌───┘     │    └────┤
                    │             │  │         │         │
         SceneRenderer      ForwardPipeline  其他并行任务
         (ParallelFor       (ParallelInvoke
          Chunked)           8× SecCB 录制)
              │
    ┌─────────┴────────────────────────────┐
    │           Main Thread                 │
    │  ┌─────────────────────────────────┐  │
    │  │  1. Engine::Initialize          │  │
    │  │  2. Game Loop                   │  │
    │  │     ├─ SceneRenderer::Prepare   │  │ ← 会派发到线程池
    │  │     ├─ Pipeline::Render         │  │ ← 会派发到线程池
    │  │     ├─ ExecuteSecondary × N     │  │ ← 串行合并
    │  │     ├─ cmdList->Submit          │  │ ← 串行提交
    │  │     └─ ShaderHotReload::Poll()  │  │ ← 消费热重载
    │  └─────────────────────────────────┘  │
    └────────────────────────────────────────┘
              │                     
    ┌─────────┴──────────────┐
    │  ShaderHotReload Thread │
    │  ReadDirectoryChangesW  │
    │  slangc 编译            │
    └─────────────────────────┘

    ┌─────────────────────────┐
    │   GPU Queues             │
    │   Graphics Queue ←──主提交
    │   Compute  Queue ←──AsyncCompute
    └─────────────────────────┘
```

---

## 八、已实现 vs 未实现

### 8.1 已实现

| 功能 | 位置 | 成熟度 |
|------|------|:------:|
| **工作窃取线程池** | `JobSystem` + Taskflow | ✅ |
| **并行视锥剔除** | `SceneRenderer::Prepare` | ✅ |
| **多线程命令录制** | `ForwardPipeline::Render` (8× SecCB) | ✅ |
| **AsyncCompute GPU 并行** | `RenderGraph::ExecuteWithAsyncCompute` | ✅ |
| **Shader 热重载** | 独立 `std::thread` + `ReadDirectoryChangesW` | ✅ |
| **任务阈值自适应** | `ParallelForEach` count > 1024 自动选择 | ✅ |
| **分块并行** | `ParallelForChunked` 可调 chunk size | ✅ |
| **帧配置** | `EngineConfig::enableMultiThreadRecord` | ✅ |

### 8.2 未实现

| 功能 | UE5 对应 | 说明 |
|------|----------|------|
| **专用 RHI 线程** | `FRHIThread` | GPU API 调用在主线程录制+提交，没有独立的 RHI 调度线程 |
| **Render Thread** | `FRenderThread` | 剔除在 JobSystem 线程执行但没有持久化的 Render 线程 |
| **Game Thread 分离** | `FGameThread` | 所有逻辑和渲染都在同一线程 |
| **任务图依赖调度** | `tf::Taskflow` DAG | 尽管 Taskflow 支持图，但当前只用了 `silent_async` + `wait_for_all` 的简单模式 |
| **流水线并行** | `ParallelFor` + pipe | 当前帧必须完成所有任务才能提交，无帧间重叠 |
| **GPU 端剔除** | `GPUScene` | 所有剔除在 CPU 端（SceneRenderer），GPU Culling 仅用于粒子系统 |
| **DeferredPipeline MTCR** | — | Deferred 管线未实现多线程命令录制 |
| **WorkerThread 识别** | `IsInRHIThread()` | `JobSystem::IsWorkerThread()` 总是返回 false |
| **线程亲和性/优先级** | `FThreadAffinity` | 无 |
| **任务优先级** | `TaskPriority` | 无 |
| **PipelineState 并行创建** | PSO cache + ParallelCreate | PSO 在主线程串行创建 |

### 8.3 当前瓶颈

```
每帧主线程耗时:
  ┌──────────────────┬─────────────────────────────────┐
  │ SceneRenderer    │ 并行剔除 (JobSystem)              │
  ├──────────────────┤                                  │
  │ Pipeline         │                                  │
  │   Render         │ Draws 并行录制 (JobSystem)       │ ← 这部分是异步的
  ├──────────────────┼─────────────────────────────────┤
  │ ExecuteSecondary │ ★ 串行瓶颈: vkCmdExecuteCommands  │ ← 在主线程
  ├──────────────────┤                                  │
  │ cmdList->Submit  │ ★ 串行瓶颈: vkQueueSubmit         │ ← 在主线程
  ├──────────────────┤                                  │
  │ Present          │ ★ 串行瓶颈: vkQueuePresentKHR     │ ← 在主线程
  └──────────────────┴─────────────────────────────────┘
```

**核心瓶颈**: `ExecuteSecondary × N` 的 N 次 `vkCmdExecuteCommands` 调用是串行的，但这 N 次调用的开销通常远小于 N 个线程的录制时间，所以总体仍然是加速的。真正的瓶颈是 **Submit 和 Present 之间的 GPU 等待**，这属于 GPU 端问题而非 CPU 多线程问题。
