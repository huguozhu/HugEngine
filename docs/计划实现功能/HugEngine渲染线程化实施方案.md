# HugEngine 渲染线程化实施方案（方案 B：Game Thread → Render+RHI 单线程）

> **版本**：v1.0（2026-09-22 编写）
> **基线提交**：`4af2d15`（引擎介绍目录编号化之后）
> **性质**：**实施计划，未开工**。本文只描述"要做什么、按什么顺序做、怎么验收"，不代表已实现。
> **关联文档**：
> - [15.多线程架构与渲染实现分析.md](../HugEngine引擎介绍/15.多线程架构与渲染实现分析.md)（JobSystem / MTCR / AsyncCompute 现状）
> - [02.架构UML与可扩展性分析.md](../HugEngine引擎介绍/02.架构UML与可扩展性分析.md)（分层、时序、可扩展性债务）
> - [04.功能缺口与对标分析.md](../HugEngine引擎介绍/04.功能缺口与对标分析.md)（§16 有代码证据的架构债务）
> - [05.渲染管线实现分析.md](../HugEngine引擎介绍/05.渲染管线实现分析.md)（三条管线的每帧结构）
> - [20.编辑器实现分析.md](../HugEngine引擎介绍/20.编辑器实现分析.md)（ImGui 集成与后端泄漏点）
>
> **本文怎么读**：
> - 只想了解**选型结论** → §1
> - 要**排期/派活** → §5 分阶段任务 + §9 任务总表
> - 要**动手写代码** → §3 三条铁律 + §4 接口草案 + §5 对应阶段
> - 要**判断风险** → §2 现状约束 + §7 风险与回退

---

## 目录

1. [目标架构](#1-目标架构)
2. [现状约束盘点（迁移必须处理的 8 件事）](#2-现状约束盘点迁移必须处理的-8-件事)
3. [三条线程铁律（契约）](#3-三条线程铁律契约)
4. [核心抽象设计（接口草案）](#4-核心抽象设计接口草案)
5. [分阶段实施（阶段 0~5）](#5-分阶段实施阶段-05)
6. [验收指标](#6-验收指标)
7. [风险与回退](#7-风险与回退)
8. [工作量与排期建议](#8-工作量与排期建议)
9. [附录 A：任务总表（可勾选）](#9-附录-a任务总表可勾选)
10. [附录 B：自动化迁移检查](#10-附录-b自动化迁移检查)
11. [参考](#11-参考)
12. [附录 C：升级到 UE 三线程模型的增量路径](#12-附录-c升级到-ue-三线程模型的增量路径)

---

## 1. 目标架构

### 1.1 一句话目标

> **让渲染的"收集 → 录制 → 提交 → Present"全部离开游戏线程，跑在一根独立的渲染线程上；游戏线程只投递命令与不可变快照，永不触碰 RHI。**

### 1.2 目标线程模型

| 线程 | 数量 | 职责 | 禁止事项 |
|---|---|---|---|
| **游戏线程（主线程）** | 1 | 窗口与输入、ECS 系统 tick（Movement / Physics / Nav / Spline / 相机 / Skeletal / Text / CollisionDebug）、产出**帧快照**、投递渲染命令、ImGui 的 CPU 侧（`NewFrame` + 面板 + `Render()` 产出 `ImDrawData`） | **任何 RHI 调用**；任何对已提交快照数据的写 |
| **渲染线程（本次新增）** | 1 | 消费命令与快照 → 剔除与收集 → 建帧图 → **录制** → **提交** → `AcquireNextImage`/`Present`；持有并驱动所有 RHI 对象 | 读写 ECS 世界；阻塞等待 GPU（除非是明确的帧同步点） |
| **JobSystem 工作线程** | `hardware_concurrency()`（当前 12） | **并行录制**（每线程一个 secondary command list）；场景分块并行 | 触碰 RHI 的**非录制**部分（创建/提交/呈现） |
| **Jolt 物理线程池** | 2 | 物理步进内部并行（不变） | — |
| AI 推理 / 热重载监听 / 崩溃看门狗 | 各 1（按需） | 不变 | 触碰 RHI 创建或提交（热重载的 PSO 替换改为请求渲染线程执行） |

### 1.3 一帧时序（含背压）

```
游戏线程                                    渲染线程
   │                                            │
   │ PollEvents / AcquireNextImage  ──────────► │（交换链归渲染线程）
   │ ECS 系统 tick                              │
   │ 产出 FrameSceneSnapshot（不可变）           │
   │ EnqueueFrame(snapshot, camera, dt) ──────► │  ← 若在飞帧数 ≥ 3，游戏线程在此阻塞
   │ ImGui NewFrame + 面板 + Render()           │
   │ EnqueueUI(ImDrawData 快照) ──────────────► │
   │ 下一帧循环（不必等渲染完成）                 │ 剔除 → 收集 → 建帧图 → 并行录制
   │                                            │ → vkQueueSubmit → Present
   │                                            │ 回收帧票据（唤醒可能阻塞的游戏线程）
```

关键点：**游戏线程与渲染线程并行推进**，两者之间只有"快照交接 + 有界队列"；渲染线程落后超过 `kMaxFramesInFlight`（= **3**，`Engine/RHI/RHI/Types.h:17`）时才对游戏线程施加背压。

### 1.4 与 UE 的职责映射

| UE 概念 | 本方案的对应物 | 说明 |
|---|---|---|
| Game Thread | 游戏线程（主线程） | 不变 |
| Render Thread | **渲染线程**（本次新增） | 承担 UE 渲染线程的全部职责：剔除、MDC/绘制收集、RDG 建图、记录 RHI 命令 |
| RHI Thread | **不存在**（本次明确不做） | 渲染线程即"Render + RHI 合并"，等价于 UE 的 `r.RHIThread.Enable=0`；**将来升级到三线程的增量路径见 §12** |
| `ENQUEUE_RENDER_COMMAND` | `RenderCommandQueue` | 游戏 → 渲染的命令投递 |
| Scene Proxy / FPrimitiveSceneProxy | `FrameSceneSnapshot` | 游戏线程产出的不可变渲染数据 |
| `FParallelCommandListSet` | JobSystem + `m_SecRecordLists` | 已有雏形（仅 ForwardPipeline），本方案推广到全部管线 |
| `FRHIGPUFence` | `RHIFenceHandle`（已存在） | `CreateFence/WaitForFence/GetFenceValue` |

### 1.5 选型依据（为什么不是三线程）

1. **瓶颈在录制不在提交**：《Lumen设计与实现》实测 **CPU 录制 16.4 ms vs GPU 3.4 ms**，RHI 线程只能挪走提交（1~3 ms 量级）；
2. **录制基础已具备**：`VulkanCommandList` 自建命令池（`Engine/RHI/Vulkan/VulkanCommandList.cpp:107,132`），`ForwardPipeline.cpp:366-375` 已预分配每线程 secondary CB；
3. **跨帧释放基础已具备**：`Engine/RHI/RHI/FrameRetireQueue.h`（N 帧延迟释放）+ `Engine/RHI/Vulkan/DeferredDestructionQueue.h`；
4. **三线程的阻塞项都很贵**：阻塞式资源创建、阻塞 `Map()`、等待型提交、ImGui 原生句柄耦合（见 §2）；
5. **可升级性**：本方案阶段 2 要求渲染线程通过**记录/执行分离**的方式持有 RHI（先记命令流、再统一执行），因此将来加第三根 RHI 线程只是"换个线程执行同一命令流"。

---

## 2. 现状约束盘点（迁移必须处理的 8 件事）

| # | 约束 | 代码证据 | 对方案的硬性影响 |
|---|---|---|---|
| **C1** | **渲染路径直接读 ECS World** | `DeferredPipeline::CollectLights(pc, world, sg, camera)`（`Engine/Render/Pipeline/DeferredPipeline.cpp:810`，调用点 `DeferredPipeline_FrameGraph.cpp:554,981,1184`）；`GPUScene::Collect(World&, SceneGraph&, const CameraData&)`（`GPUScene.cpp:52`）；`ForwardPipeline::CollectLights`（`ForwardPipeline.cpp:545`） | **最大障碍**。渲染线程读 World 时游戏线程正在 tick → 数据竞争。必须先做**帧快照**（阶段 1），否则渲染线程不能起 |
| **C2** | **交换链操作在样例主循环** | `AcquireNextImage()`：`Samples/06.GILab/06.GILab.cpp:876`、`Samples/02.Cube/02.Cube.cpp:970`；`Present()`：`06.GILab.cpp:1794`、`02.Cube.cpp:1671` | Vulkan 交换链需外部同步 ⇒ Acquire/Present 必须**整体迁入渲染线程**，样例循环不再直接调用 |
| **C3** | **资源创建同步阻塞 + 每帧仍有上传** | `vmaCreateBuffer`/`vmaCreateImage` 于调用线程立即执行（`Engine/RHI/Vulkan/VulkanResources.cpp:134,365`）；`BufferDesc::initialData` 每次创建 staging 并一次性提交（`VulkanResources.cpp:486,651`）；每帧级上传点遍布 Lumen/Nanite/Decal（如 `LumenSDF.cpp:299,305,1335`、`NaniteScene.cpp:167`、`DecalPass.cpp:74`） | 需要 `ResourceCreationService`（阶段 0），且**上传路径要能跨线程排队**；`initialData` 语义要明确"是否允许来自游戏线程的指针" |
| **C4** | **阻塞读回** | `VulkanBuffer::Map()`（`VulkanResources.cpp:179`）；AutoExposure 读回路径 | 读回必须改为"渲染线程登记 + N 帧后取值"（`FrameRetireQueue` 已有账本） |
| **C5** | **等待型提交** | `SignalFenceOnQueue` / `WaitFenceOnQueue` 提交后立即等待（`Engine/RHI/Vulkan/VulkanDevice.cpp:932,958`） | 这些调用必须**只允许在渲染线程**出现，且不得进入帧循环关键路径 |
| **C6** | **ImGui 与场景共用命令缓冲与 RenderPass，且经原生句柄** | `Engine/RHI/RHI/RHI.h:163-169`（ImGui 专用后门：`CreateImGuiDescriptorPool`/`CreateImGuiRenderPass`/`GetImGuiCommandBuffer`）；`Samples/Editor/EditorApp.cpp:536-542`（场景与 UI 同一 CB） | UI 的 **CPU 侧**（`NewFrame`/面板/`Render()`）可留在游戏线程；**后端录制**必须迁到渲染线程（阶段 4） |
| **C7** | **RHI 无线程归属机制** | 全仓 `GetCurrentThreadId` 只出现在 `Engine/Core/Core/CrashHandler.cpp` | 阶段 0 第一件事：加 `IsOwnerThread()` + 断言，否则违规调用只能靠"偶发崩溃"发现 |
| **C8** | **RHI 调用面极宽** | `IRHICommandList` 有 45+ 个虚函数（`Engine/RHI/RHI/CommandList.h:36-283`），`IRHIDevice` 另有创建/提交/栅栏 API（`RHI.h:172,188-208`） | 不追求"让 RHI 线程安全"，而是**规定只有渲染线程能调**；录制期通过 secondary CB 分发到 worker（CE 已安全，见 C9） |
| **C9** | ✅ **已有的有利条件**：录制期命令池是每 list 独立的 | `m_CmdPools[i]` + `m_SecondaryPool`（`VulkanCommandList.cpp:107,132`）；`ForwardPipeline.cpp:366-375` 每线程 secondary CB 池；`ForwardPipeline.cpp:1290-1300` 并行录制 + 主线程 `ExecuteSecondary` | 并行录制**不需要改 RHI**，只需把方案推广到 Deferred/PathTracing（阶段 3） |

> **C1 是本方案的核心工作量**：快照层不做，渲染线程就只能"和游戏线程抢 World"，那是不可调试的。

---

## 3. 三条线程铁律（契约）

这三条要写进 `Engine/RHI` 的头文件注释，并用断言强制：

| 铁律 | 内容 | 强制手段 |
|---|---|---|
| **铁律 1** | **RHI 只属于渲染线程**：`IRHIDevice::Create*` / `Destroy*`、`IRHICommandList::*`、`Submit` / `SubmitAll`、`SignalFenceOnQueue` / `WaitFenceOnQueue`、交换链 `AcquireNextImage` / `Present` —— **只能在渲染线程调用** | `HE_ASSERT(IsRenderThread())` 加在上述路径入口（Debug/RelWithDebInfo 生效） |
| **铁律 2** | **游戏线程只产出不可变数据**：任何交给渲染线程的结构（快照、命令参数、上传数据）在交接后**不得再被游戏线程修改**；需要复用则用双/三缓冲按帧轮换 | 快照用 `FrameSceneSnapshot` 值语义 + 按 `frameSlot` 轮换；上传数据在入队时**拷贝**到队列自有内存 |
| **铁律 3** | **渲染线程不阻塞等 GPU**：帧循环内不得出现 `WaitForFence(UINT64_MAX)` 之类的无限等待；必须等待时只允许"等待 N-`kMaxFramesInFlight` 帧之前已完成" | Code review + 在 `WaitFenceOnQueue` 加注释说明允许的调用点白名单 |

**游戏线程唯一允许等待的位置**：提交新帧时若在飞帧数已达 `kMaxFramesInFlight`，则等待最老一帧的完成信号（背压，不是 GPU 同步）。

---

## 4. 核心抽象设计（接口草案）

> 以下为**接口草案**，用于确定职责边界；实现细节可在编码阶段调整。所有代码均含中文注释（项目规范）。

### 4.1 `RenderCommandQueue`（游戏线程 → 渲染线程）

**设计要点**：类型化命令 + 一次性 lambda 通道；命令参数**必须按值捕获**（铁律 2）。批量提交整帧命令，减少锁竞争。

```cpp
// Engine/Render/Threading/RenderCommandQueue.h
namespace he::render {

/// 渲染命令队列：游戏线程入队，渲染线程消费。
/// 设计约束：
///   1. 入队的数据必须**按值捕获**（铁律 2：交接后游戏线程不再修改）；
///   2. 整帧提交（BeginFrame/EndFrame），避免每条命令一次锁；
///   3. 队列容量有上限，超限说明渲染线程长期落后 → 由 RenderThread 施加背压。
class RenderCommandQueue {
public:
    /// 游戏线程：开始本帧命令收集（拿到本帧的 slot）
    void BeginFrame(u32 frameSlot);

    /// 游戏线程：入队一条命令。fn 在渲染线程执行，禁止捕获 World/SceneGraph 引用
    void Enqueue(std::function<void(class RenderThreadContext&)> fn);

    /// 游戏线程：结束收集并发布给渲染线程（一次移动，不拷贝）
    void PublishFrame();

    /// 渲染线程：取出下一帧待执行命令；无命令时返回 false（由调用方决定等待/休眠）
    bool TryConsumeFrame(std::vector<std::function<void(class RenderThreadContext&)>>& outCommands);

    /// 在飞帧数（用于背压判断）
    [[nodiscard]] u32 InFlightFrameCount() const;

private:
    std::mutex                          m_Mutex;
    std::condition_variable             m_Cv;
    std::vector<std::function<void(class RenderThreadContext&)>> m_Pending;
    u32                                 m_InFlight = 0;
};

} // namespace he::render
```

### 4.2 `FrameSceneSnapshot`（游戏线程产出的不可变渲染数据）

**设计要点**：这是**替换 `CollectLights(pc, world, sg, camera)` 与 `GPUScene::Collect(world, sg, camera)` 直接读 World 的关键**。快照要覆盖渲染真正需要的全部输入，且**不含任何 Entity/组件指针**。

```cpp
// Engine/Render/Threading/FrameSceneSnapshot.h
namespace he::render {

/// 单个可见物体的渲染输入（与 ECS 完全解耦：只有值，没有指针）
struct SnapshotDrawItem {
    float4x4 worldMatrix;      // 世界矩阵（已含父级与缩放）
    float4x4 prevWorldMatrix;  // 上一帧世界矩阵（TAA/运动矢量/重投影需要）
    u32      meshId;           // 网格资源 ID（渲染线程查表拿 GPU 缓冲）
    u32      materialId;       // 材质 ID（4 个连续 bindless 槽，与 ForwardPipeline 现有约定一致）
    u32      objectIndex;      // SSBO 槽位
    u32      flags;            // 可见性/实例化/骨骼等位标记
};

/// 光源条目（替代 CollectLights 里的 World 遍历）
struct SnapshotLight {
    u32    type;               // 0=Dir 1=Point 2=Spot 3=Rect
    float3 position;
    float3 direction;
    float3 color;
    float  intensity;
    float  range;
    u32    shadowIndex;        // -1 = 不投影
    // …按 GPULight 逐字段对齐，便于渲染线程直接 memcpy 进 UBO
};

/// 一帧的完整渲染输入。由游戏线程在 tick 结束后构造，交接后视为只读。
struct FrameSceneSnapshot {
    u64                 frameIndex = 0;
    CameraData          camera;             // 含本帧 viewProj（TAA 抖动在渲染线程叠加，见阶段 2）
    float               deltaTime  = 0.0f;
    std::vector<SnapshotDrawItem>  draws;
    std::vector<SnapshotLight>     lights;
    std::vector<float4x4>          skinMatrices;   // 骨骼矩阵（扁平数组，替代读组件）
    u32                            viewportWidth  = 0;
    u32                            viewportHeight = 0;
    // 需要时继续扩展；扩展前先确认"渲染是否真的需要它"
};

} // namespace he::render
```

> **判据（阶段 1 退出条件）**：`Engine/Render/` 内不再出现对 `he::World&` / `he::SceneGraph&` 的**渲染期**引用（`Engine/Render/` 目录 grep 断言，见附录 B）。

### 4.3 `RenderThread`（调度器）

```cpp
// Engine/Render/Threading/RenderThread.h
namespace he::render {

/// 渲染线程：唯一的 RHI 消费者。
/// 生命周期与 IRHIDevice / IRHISwapChain 绑定：设备在渲染线程上创建与销毁。
class RenderThread {
public:
    /// 启动：在内部线程上完成设备初始化（含管线 Initialize、资源创建）
    bool Start(rhi::Backend backend, const rhi::DeviceDesc& desc);

    /// 停止：排空命令队列 → WaitIdle → 销毁管线与设备
    void Stop();

    /// 游戏线程：提交一帧（快照 + 命令）。若在飞帧数达上限则阻塞（背压点）
    void SubmitFrame(FrameSceneSnapshot&& snapshot);

    /// 游戏线程：投递一个"资源创建请求"，返回句柄（不阻塞；渲染线程执行创建）
    template <typename T>
    std::shared_ptr<T> RequestResource(const rhi::ResourceDesc& desc);

    /// 游戏线程：请求一次设备级同步（仅用于加载期/退出期，不得进入帧循环）
    void FlushAndWait();

    /// 当前是否运行在渲染线程（供断言使用）
    static bool IsCurrent();

private:
    void ThreadMain();          // 线程主体：命令循环 + 帧节奏
    std::thread              m_Thread;
    RenderCommandQueue       m_Commands;
    // …帧票据、fence、背压计数
};

} // namespace he::render
```

**帧票据与背压**：

```cpp
/// 帧完成票据：渲染线程 Present 后回收，游戏线程据此解除背压
struct FrameTicket {
    u64 frameIndex;
    u64 fenceValue;   // 与渲染线程的 timeline fence 对应
};
```

### 4.4 `ResourceCreationService`（资源创建 → 渲染线程）

**设计要点**：分两步走，降低一次性改造风险。

| 步 | 形态 | 用在哪 | 风险 |
|---|---|---|---|
| **步 1（阶段 2 采用）** | **同步转发**：游戏线程入队创建请求 → 渲染线程执行 → 用 `std::promise` 回传（游戏线程阻塞等待） | 加载期、编辑器导入、resize 重建 | 简单、正确；但**阻塞**，因此**禁止**在帧循环内使用 |
| **步 2（阶段 5 可选）** | **异步**：返回 `Future<Handle>`；帧内使用"上帧请求、下帧就绪"的两段式 | 帧内动态创建（如 Nanite 流式页） | 复杂度高，只在实测需要时做 |

```cpp
// Engine/Render/Threading/ResourceCreationService.h
namespace he::render {

/// 资源创建服务：把"创建 GPU 资源"从游戏线程搬到渲染线程执行。
/// 步 1（本方案默认）：同步转发，仅供加载期/重建期使用；
/// 步 2（后续可选）：异步 Future，供帧内动态资源使用。
class ResourceCreationService {
public:
    /// 同步创建（游戏线程阻塞直到渲染线程完成）。**禁止在帧循环内调用**：
    /// 帧内创建会让游戏线程等待渲染线程，破坏并行前提。
    rhi::IRHIBuffer* CreateBufferSync(const rhi::BufferDesc& desc);
    rhi::IRHITexture* CreateTextureSync(const rhi::TextureDesc& desc);

    /// 上传数据：拷贝入队（铁律 2），由渲染线程在下一帧执行拷贝
    void UploadAsync(rhi::IRHIBuffer* dst, const void* data, u64 size, u64 offset = 0);
};

} // namespace he::render
```

### 4.5 线程归属断言

```cpp
// Engine/RHI/RHI/ThreadAffinity.h
namespace he::rhi {

/// 设备拥有线程（= 渲染线程）。设备创建时记录，销毁时清空。
class ThreadAffinity {
public:
    void Claim()   { m_Owner = std::this_thread::get_id(); m_Claimed = true; }
    void Release() { m_Claimed = false; }
    [[nodiscard]] bool IsOwner() const {
        return !m_Claimed || m_Owner == std::this_thread::get_id();
    }
private:
    std::thread::id m_Owner;
    bool            m_Claimed = false;
};

} // namespace he::rhi

/// 在 RHI 的创建/销毁/录制/提交入口统一使用：
///   HE_ASSERT(IsRenderThread() && "RHI 调用必须在渲染线程（见三条铁律）");
#define HE_ASSERT_RENDER_THREAD() /* Debug/RelWithDebInfo 生效 */
```

---

## 5. 分阶段实施（阶段 0~5）

> 每阶段都满足：**可独立回退**（配置开关）且**有客观验收判据**（不靠"看起来没问题"）。

### 阶段 0 · 地基（不改变现有行为）

**目标**：把"跨线程会炸"的地方先圈出来并封好，为起线程做准备。**本阶段结束时引擎行为与现在逐像素一致。**

| 任务 | 内容 | 主要文件 | 判据 |
|---|---|---|---|
| **T0.1** | 引入 `ThreadAffinity` + `HE_ASSERT_RENDER_THREAD()`，加在 `IRHIDevice` 创建/销毁、`IRHICommandList` 录制、`Submit`/`SubmitAll`、`WaitFenceOnQueue` 入口（先记录拥有线程 = 主线程，行为不变） | `Engine/RHI/RHI/*.h`、`Engine/RHI/Vulkan/VulkanDevice.cpp` | 现有样例全跑通、零断言触发；断言可被 `HE_DISABLE_THREAD_ASSERT` 关闭 |
| **T0.2** | 清点并登记"帧内同步 RHI 调用"（上传 / 读回 / 等待型提交），产出清单（文件:行号 + 所属阶段） | 只读盘点，不改代码 | 清单里每条都标注"迁到渲染线程 / 改为异步 / 保留（加载期）" |
| **T0.3** | `RenderCommandQueue` + `RenderThread` **壳实现**：游戏线程入队，主线程（当前线程）消费——**行为完全等价于现在** | `Engine/Render/Threading/`（新增） | 样例帧序与改前一致；`cmp_dumps` 前后 dump 逐像素相同 |
| **T0.4** | 帧票据与背压骨架（`FrameTicket` + 在飞帧计数），上限 `kMaxFramesInFlight` | 同上 | 人为把上限设为 1 时能观察到游戏线程阻塞（可测） |
| **T0.5** | `EngineConfig` 新增 `enableRenderThread`（默认 **false**）与 `renderThreadSpinWaitUs` | `Engine/Core/Core/Engine.h` | 编译期与运行期开关均可切换；关闭时走旧路径 |
| **T0.6** | **（为 §12 预埋，可选但强烈建议）** 定义 RHI 命令流的**记录端契约**与 `RHICommand` 载荷形态（类型擦除 + 内联参数），暂不改变执行方式 | `Engine/RHI/RHI/RHICommandList.h`（新增） | 契约评审通过；本轮不接入生产路径，只落头文件与单测 |
| **T0.7** | **（为 §12 预埋，可选但强烈建议）** 资源句柄化：新增 `RHIBufferHandle` / `RHITextureHandle`（含 generation），并让新代码优先用句柄；`unique_ptr<IRHIBuffer>` 保留为兼容层 | `Engine/RHI/RHI/RHI.h`、`Engine/RHI/RHI/RHIHandles.h`（新增） | 新代码不再新增 `unique_ptr<IRHIBuffer>` 成员（grep 断言）；旧代码可渐进迁移 |

**退出判据**：阶段 0 全绿 = 现有样例在 `enableRenderThread=false/true`（壳模式）下**输出一致**、帧时间不退化 > 3%。
> **T0.6/T0.7 是本方案唯一的"现在不做、以后要付大代价"的两件事**，理由见 §12：RHI 命令流与资源句柄化是升级到三线程模型的前置条件，而在阶段 0 预埋只需 +5~8 人日，等到阶段 2 之后再补则要动全工程 200+ 处资源持有者。

### 阶段 1 · 场景快照（把渲染与 World 解耦）

**目标**：消除 C1。**这是本方案的核心工作量，也是最值得先做的一步**——即使最终不起渲染线程，快照层也能立刻改善架构（渲染不再穿过 ECS）。

| 任务 | 内容 | 主要文件 | 判据 |
|---|---|---|---|
| **T1.1** | 定义 `FrameSceneSnapshot`（§4.2），字段与现有 `GPULight`/`GPUObjectData`/`PushConstantData` 逐字段对齐 | `Engine/Render/Threading/FrameSceneSnapshot.h` | 字段表评审通过（与 `ShaderTypes.slang` 一致，含 `static_assert`） |
| **T1.2** | 游戏线程侧新增 `SceneSnapshotBuilder::Build(world, sg, camera) → snapshot`（把现在散在 `CollectLights`/`GPUScene::Collect` 里的遍历集中到这里） | `Engine/Render/Threading/SceneSnapshotBuilder.{h,cpp}` | 与旧路径**并行跑一帧双份**，逐字段比对一致（临时 `HE_SNAPSHOT_VERIFY` 开关） |
| **T1.3** | `CollectLights` / `GPUScene::Collect` 改为**消费快照**（保留旧签名做过渡，内部转发到快照实现） | `DeferredPipeline.cpp:810`、`ForwardPipeline.cpp:545`、`GPUScene.cpp:52` | 三条管线（Forward / Deferred / PathTracing）渲染输出与改前**逐像素一致**（`cmp_dumps.py`） |
| **T1.4** | 骨骼矩阵、材质快照、Decal/粒子等其余渲染输入的快照化 | `SceneRenderer.*`、各 Pass | 同上 |
| **T1.5** | 移除渲染期对 World 的引用（保留加载期一次性访问） | `Engine/Render/` | **附录 B 的 grep 断言通过**：渲染期函数签名不再出现 `he::World&` / `SceneGraph&` |

**退出判据**：全部样例在开关两种状态下输出一致；`Engine/Render/` 渲染期无 World 依赖；Lumen/Nanite 的流式路径有明确"谁在何时读世界"的记录。

### 阶段 2 · 起渲染线程

**目标**：真正让渲染跑在独立线程（C2/C3/C5/C6 落地）。

| 任务 | 内容 | 主要文件 | 判据 |
|---|---|---|---|
| **T2.1** | `RenderThread` 实现（线程主体、命令循环、帧节奏、休眠策略：空闲时条件变量等待 + 可配的自旋窗口） | `Engine/Render/Threading/RenderThread.cpp` | 空闲帧 CPU 占用 < 1%（不得自旋烧核） |
| **T2.2** | 设备与交换链**改由渲染线程创建/持有**；`AcquireNextImage` / `Present` 迁入渲染线程 | `VulkanDevice.cpp`、`VulkanSwapChain.*`、样例循环 | 样例里不再出现交换链调用；`HE_ASSERT_RENDER_THREAD` 无触发 |
| **T2.3** | 管线 `Initialize/Shutdown`、`OnResize`、资源创建走 `ResourceCreationService`（步 1 同步转发） | 三条管线、`Engine/Render/Threading/ResourceCreationService.*` | 加载期与 resize 正常；帧循环内无同步创建（grep 断言） |
| **T2.4** | 样例循环改造：`PollEvents` + tick + `SubmitFrame` + ImGui CPU 侧；删除主线程的渲染与提交代码 | `Samples/01~07` | 全部样例可运行；帧时间不退化 |
| **T2.5** | 编辑器改造：`EditorApp` 主循环同样改为投递；ImGui 后端录制先保持"渲染线程内录制" | `Samples/Editor/EditorApp.cpp` | 编辑器可用；UI 与场景仍在同一 RenderPass（阶段 4 再解） |
| **T2.6** | 关键：**渲染线程的 RHI 调用改用"记录 → 执行"两段式**（为将来加 RHI 线程留口）——至少把 `Submit` 与资源创建收口到 `RenderThreadContext` 的两个方法 | `Engine/Render/Threading/RenderThreadContext.h` | 记录/执行边界在代码上可见（`RenderThreadContext` 是唯一 RHI 出口） |

**退出判据**：
1. 主线程**零 RHI 调用**（断言 + grep 双重保证）；
2. 游戏 tick 与渲染**真正并行**（帧时间从"游戏 + 渲染"变为"取较大者"，可用 profiler 观测）；
3. `vkQueueSubmit` 不在游戏线程时间线上；
4. Validation（含 `--sync-validation`）零新增告警；
5. 全部样例 + 编辑器 + 46 个测试文件通过。

### 阶段 3 · 并行录制推广

**目标**：把 ForwardPipeline 已有的 MTCR 推广到全部管线（C9 的有利条件用起来）。

| 任务 | 内容 | 主要文件 | 判据 |
|---|---|---|---|
| **T3.1** | 抽出 `ParallelRecordHelper`（secondary CB 池 + 分块 + `ExecuteSecondary` 合并），三条管线共用 | `Engine/Render/Pipeline/ParallelRecordHelper.{h,cpp}` | Forward 行为不变（重构不改变输出） |
| **T3.2** | Deferred 的 GBuffer 与 Lighting 段接入并行录制 | `DeferredPipeline_FrameGraph.cpp` | 录制耗时下降；输出逐像素一致 |
| **T3.3** | PathTracing / Nanite 光栅段的并行录制评估与接入（Nanite 是 indirect + mesh shader，可能不适合按 draw 分块） | `PathTracingPipeline.cpp`、`NaniteRaster.cpp` | 逐项结论记录在案（含"不做"的理由） |
| **T3.4** | 分块策略与 worker 数调优（按 draw 数 vs 按 index 数分块；避免小规模场景下并行反而变慢） | `ParallelRecordHelper.*` | 小场景（draw < 64）不退化；大场景（Sponza）线性加速 |

**退出判据**：Lumen 场景（06.GILab）的 **CPU 录制耗时显著下降**（目标：16.4 ms → < 8 ms，12 线程可用），且 GPU 时间不变。

### 阶段 4 · 编辑器与 ImGui 收口

| 任务 | 内容 | 判据 |
|---|---|---|
| **T4.1** | ImGui 后端录制归属明确化：CPU 侧（`NewFrame`/面板/`Render()`）留游戏线程，`RenderDrawData` 在渲染线程执行（标准做法） | 编辑器 UI 正常、无竞态 |
| **T4.2** | 消除 `RHI.h:163-169` 的 ImGui 专用后门（改为通过标准 RHI 抽象拿描述符池/渲染通道/命令缓冲） | 后门接口从 `IRHIDevice` 移除 |
| **T4.3** | 面板对渲染数据的访问改为只读快照（Details/Stats/Viewport 拾取等） | 面板仍可用；无跨线程读 ECS |
| **T4.4** | 着色器热重载：文件监控线程只发"重编译请求"，PSO/字节码替换在渲染线程执行（当前 `SHADER_HOT_RELOAD` 替换路径在主线程 `Poll()`） | 热重载仍生效；替换发生在渲染线程 |

### 阶段 5 · 性能验收与调优

| 任务 | 内容 | 判据 |
|---|---|---|
| **T5.1** | `ProfilerManager` 增加"游戏 / 渲染 / 等待（背压）"三段计时并输出到 Stats 面板 | 三段耗时可见 |
| **T5.2** | 背压参数调优（帧在飞数、自旋时长、命令队列容量） | 稳态帧率不低于改前；空闲 CPU 占用 < 1% |
| **T5.3** | 资源创建异步化（步 2）——**仅在实测有帧内创建需求时做** | 若不做，记录原因 |
| **T5.4** | 文档更新：本文标记为已实现；`15.多线程架构与渲染实现分析.md` 补"渲染线程"一节 | 文档与代码一致 |

---

## 6. 验收指标

| 指标 | 现状（实测/推断） | 阶段 2 目标 | 阶段 3 目标 |
|---|---|---|---|
| 渲染所在线程 | 主线程 | **独立渲染线程** | 同左 |
| 游戏 tick 与渲染 | 串行相加 | **并行（取较大者）** | 同左 |
| CPU 录制耗时（Lumen 场景） | **16.4 ms** | < 12 ms | **< 8 ms** |
| `vkQueueSubmit` 位置 | 主线程帧内 | 渲染线程 | 渲染线程 |
| 主线程 RHI 调用 | 大量 | **0**（断言 + grep） | 0 |
| GPU 空闲率 | 偏高（CPU 受限） | 下降 | 进一步下降 |
| Validation 告警（含 sync） | 基线 | **零新增** | 零新增 |
| 渲染输出一致性 | 基线 | 逐像素一致 | 逐像素一致 |
| 空闲帧 CPU 占用 | — | < 1% | < 1% |

> 一致性验收工具：`Tools/pt/dump_pt.ps1` + `Tools/pt/analyze_pt.py`、`Tools/gi/dump_gi.ps1`、`Tools/gi/*check.ps1`（详见 [22.开发与验证手册.md](../HugEngine引擎介绍/22.开发与验证手册.md) §5/§7）。

---

## 7. 风险与回退

| 风险 | 触发征兆 | 缓解 | 回退 |
|---|---|---|---|
| **R1 数据竞争（快照不完整）** | 画面偶发闪烁/物体跳跃；TSan 或 `--sync-validation` 告警 | 阶段 1 强制"双跑比对"；快照字段评审；把"渲染是否还需要读世界"做成 grep 断言 | `enableRenderThread=false` |
| **R2 资源生命周期**（渲染线程仍在用而游戏线程已释放） | 校验层 "object was destroyed"；偶发访问违例 | 全部释放走 `FrameRetireQueue`（已有）；新资源创建经 `ResourceCreationService` 统一登记 | 同上 |
| **R3 帧延迟增加**（多一帧缓冲） | 输入延迟主观变差 | 帧在飞数保持 3；必要时提供"低延迟模式"（帧在飞 2） | 降帧在飞数 |
| **R4 性能反而退化**（线程切换/背压抖动） | 帧时间方差变大 | 命令队列整帧批量交接；空闲用条件变量而非自旋；阶段 0 的壳模式先量基线 | 关开关 |
| **R5 调试困难**（RenderDoc 时序错位） | 抓帧时命令与代码对不上 | 保留 `r.Debug.DrawMarker`（`Engine.h:22`）；渲染线程内打帧边界 marker | — |
| **R6 ImGui 竞态** | 编辑器偶发崩溃/花屏 | 阶段 4 才动 UI；期间 UI 与场景同在渲染线程录制 | 编辑器单独走旧路径 |
| **R7 范围蔓延**（顺手加 RHI 线程/多帧并行） | 阶段 2 迟迟不收敛 | **本文 §1.4 明确不做**；任何扩展先开新文档 | — |

**总开关**：`EngineConfig::enableRenderThread = false` 必须始终可用，且与 `true` 路径共享同一套快照与命令队列代码（差异只在"谁来消费"）。

---

## 8. 工作量与排期建议

| 阶段 | 内容 | 建议投入 | 依赖 |
|---|---|---|---|
| 阶段 0 | 地基（断言 + 队列壳 + 背压骨架） | 3~5 人日 | — |
| 阶段 0 预埋 | T0.6 命令流契约 + T0.7 资源句柄化（**为 §12 的三线程升级留口**） | +5~8 人日 | 与阶段 0 同期 |
| **（可选）三线程升级** | 仅做 A-1/A-2/A-3 三步（§12.8），**在阶段 2 之后才做** | **+6~12 人日**（已预埋）/ +15~30 人日（未预埋） | 阶段 2 起 |
| 阶段 1 | 场景快照（核心） | **8~15 人日** | 阶段 0 |
| 阶段 2 | 起渲染线程（设备/交换链/管线/样例/编辑器） | **10~18 人日** | 阶段 1 |
| 阶段 3 | 并行录制推广 | 5~10 人日 | 阶段 2 |
| 阶段 4 | 编辑器与 ImGui 收口 | 4~8 人日 | 阶段 2 |
| 阶段 5 | 验收与调优 | 3~6 人日 | 阶段 3/4 |

**建议顺序**：`阶段 0 → 阶段 1 → （阶段 3 可与阶段 2 并行验证）→ 阶段 2 → 阶段 4 → 阶段 5`
理由：阶段 1（快照）是唯一**没有它就无法安全起线程**的环节，且它本身独立可交付（改善架构、不改变行为）。

---

## 9. 附录 A：任务总表（可勾选）

- [ ] T0.1 线程归属断言（`ThreadAffinity` + `HE_ASSERT_RENDER_THREAD`）
- [ ] T0.2 帧内同步 RHI 调用清点清单
- [ ] T0.3 `RenderCommandQueue` + `RenderThread` 壳实现（行为等价）
- [ ] T0.4 帧票据与背压骨架
- [ ] T0.5 `EngineConfig::enableRenderThread` / `renderThreadSpinWaitUs`
- [ ] T0.6 **（预埋，见 §12）** RHI 命令流记录端契约 + `RHICommand` 载荷形态
- [ ] T0.7 **（预埋，见 §12）** 资源句柄化（`RHIBufferHandle` / `RHITextureHandle` + generation）
- [ ] T1.1 `FrameSceneSnapshot` 定义（与 `ShaderTypes.slang` 对齐 + `static_assert`）
- [ ] T1.2 `SceneSnapshotBuilder`（集中现有 Collect 遍历）
- [ ] T1.3 `CollectLights` / `GPUScene::Collect` 改消费快照
- [ ] T1.4 骨骼/材质/Decal/粒子快照化
- [ ] T1.5 渲染期移除 World/SceneGraph 引用（grep 断言）
- [ ] T2.1 `RenderThread` 实现（帧节奏 + 休眠策略）
- [ ] T2.2 设备与交换链归渲染线程（Acquire/Present 迁移）
- [ ] T2.3 `ResourceCreationService`（步 1 同步转发）
- [ ] T2.4 样例循环改造（7 个样例）
- [ ] T2.5 编辑器主循环改造
- [ ] T2.6 `RenderThreadContext`：RHI 唯一出口（记录/执行分离）
- [ ] T3.1 `ParallelRecordHelper` 抽取
- [ ] T3.2 Deferred 接入并行录制
- [ ] T3.3 PathTracing / Nanite 并行录制评估
- [ ] T3.4 分块策略与 worker 数调优
- [ ] T4.1 ImGui 后端录制归属
- [ ] T4.2 消除 ImGui 专用 RHI 后门
- [ ] T4.3 面板改读快照
- [ ] T4.4 热重载 PSO 替换改渲染线程执行
- [ ] T5.1 Profiler 三段计时
- [ ] T5.2 背压参数调优
- [ ] T5.3 资源创建异步化（可选）
- [ ] T5.4 文档更新

---

## 10. 附录 B：自动化迁移检查

以下断言建议做成脚本（如 `Tools/check_threading.py`）在阶段退出时跑一遍：

| 检查 | 期望 |
|---|---|
| **B1** `Engine/Render/` 渲染期函数签名是否出现 `he::World&` / `SceneGraph&` | 阶段 1 后为 **0**（加载期例外需逐个白名单） |
| **B2** `Samples/` 是否出现 `AcquireNextImage` / `Present` / `CreateCommandList` / `Submit` | 阶段 2 后为 **0** |
| **B3** `Engine/Render/` 帧循环路径内是否出现 `CreateBuffer` / `CreateTexture` / `CreateGraphicsPipeline` | 阶段 2 后为 **0**（Initialize/OnResize 白名单） |
| **B4** 是否出现无超时的 `WaitForFence(...UINT64_MAX)` 在帧循环内 | 为 **0**（铁律 3） |
| **B5** `IRHIDevice` 的创建/销毁入口是否都有 `HE_ASSERT_RENDER_THREAD()` | 覆盖 100% |
| **B6** 渲染输出一致性（`cmp_dumps.py` 前后对比） | 每阶段退出时**逐像素一致** |

---

## 11. 参考

- [Parallel Rendering Overview for Unreal Engine（Epic 官方）](https://dev.epicgames.com/documentation/unreal-engine/parallel-rendering-overview-for-unreal-engine?application_version=5.4&lang=zh-CN)——Render Thread / RHI Thread / 并行命令录制的职责划分
- 本仓库：[15.多线程架构与渲染实现分析.md](../HugEngine引擎介绍/15.多线程架构与渲染实现分析.md)、[02.架构UML与可扩展性分析.md](../HugEngine引擎介绍/02.架构UML与可扩展性分析.md)（§17 代码级债务）、[04.功能缺口与对标分析.md](../HugEngine引擎介绍/04.功能缺口与对标分析.md)（§16 债务清单）、[22.开发与验证手册.md](../HugEngine引擎介绍/22.开发与验证手册.md)（验证工具与流程）

---

## 12. 附录 C：升级到 UE 三线程模型的增量路径

> 本附录回答："本方案（方案 B）将来若要转成 UE 的 **Game → Render → RHI** 三线程模型，需要改什么、哪些决定必须提前做。"
> 阅读顺序：§12.5（必须提前定的决定）→ §12.6（逐任务改动）→ §12.8（落地策略）。

### 12.1 一句话：分水岭在 T2.6

| 如果 T2.6 的 `RenderThreadContext` 被设计成… | 那么升级到 A 是… |
|---|---|
| **吐命令流**（渲染线程只 push `RHICommand`，不碰 Vulkan API） | **加一层线程 + 换执行位置**（增量 +6~12 人日，见 §12.9） |
| 直接调 Vulkan（`IRHIDevice::Create*`、`vkCmd*`） | **重写渲染层**（命令流与句柄化都要事后补，面极大） |

因此 §5 阶段 0 额外预埋了 **T0.6（命令流契约）** 与 **T0.7（资源句柄化）** 两个任务。

### 12.2 本质差异

```
B（本方案）:   渲染线程 ──► IRHICommandList（直接产 vkCmd*）──► vkQueueSubmit ──► Acquire/Present
A（UE 三线程）: 渲染线程 ──► RHI 命令流（纯数据，零 API 调用）
                              RHI 线程 ──► 翻译成 vkCmd* ──► vkQueueSubmit ──► Acquire/Present
```

职责对照（对应 UE 的 `FRHICommandListExecutor` / `FDynamicRHI` / `FRHIThread`）：

| 环节 | B（渲染线程一人全包） | A（拆成两段） |
|---|---|---|
| 剔除 / 收集 / 建帧图 | 渲染线程 | 渲染线程（不变） |
| 记录"要做什么" | 直接生成 `vkCmd*` | **只 append `RHICommand`**（渲染线程） |
| 翻译成 API 调用 | 记录时立即发生 | **RHI 线程** |
| `vkQueueSubmit` / `Acquire` / `Present` | 渲染线程 | **RHI 线程（唯一提交者）** |
| fence 与背压 | 两层（游戏↔渲染） | **三层**（游戏↔渲染↔RHI） |

### 12.3 必须新增的 5 件东西

| # | 新增件 | 说明 | 难度 |
|---|---|---|---|
| **A-1** | **抽象 RHI 命令流**（记录/执行分离） | 渲染线程 push 类型擦除的命令载荷，RHI 线程 pop 并翻译。**不要用 `std::function`**：每帧数千条命令会造成堆分配与虚调用开销，UE 用"类型擦除 + 内联参数存储"（`FRHICommand`） | **最高** |
| **A-2** | **RHI 线程** | 命令流唯一消费者、唯一提交者；`Submit` / `SubmitAll` / `AcquireNextImage` / `Present` / fence 全归它 | 中 |
| **A-3** | **资源创建命令化 + 句柄化** | `std::unique_ptr<IRHIBuffer>` 的"同步立即给所有权"语义必须换成句柄（创建在 RHI 线程执行） | **面最广** |
| **A-4** | **三层背压与同步点** | 游戏→渲染、渲染→RHI 各自有在飞上限；语义对齐 UE 的 `FRHIThread::Sync()` | 中 |
| **A-5** | **ImGui / PSO / 读回的命令化** | UI 产 `ImDrawData` → 命令流；PSO 走已有异步队列；`Map()` 改轮询或延迟取值 | 中 |

### 12.4 现状接口的改造面（含两个有利条件）

| 现状 | 位置 | 对 A 的含义 |
|---|---|---|
| `CreateSwapChain` / `CreateCommandList` / `CreateBuffer` / `CreateTexture` / `CreateSampler` / `CreatePipelineState` 均返回 **`std::unique_ptr<T>`** | `Engine/RHI/RHI/RHI.h:52-58` | ⚠️ **最大改造面**：同步所有权语义 ⇒ 必须句柄化（A-3） |
| `CreateTextureMipStorageView` / `CreateTextureMipSampledView` 返回 **`void*`**；`CreateImGuiDescriptorPool` / `CreateImGuiRenderPass` 返回 **`void*`** | `RHI.h:127-135`、`RHI.h:163-167` | ⚠️ A 方案中"立即返回原生句柄"全部作废，须改为命令流内的抽象引用 |
| `Submit()` 在命令列表上、`Submit(IRHICommandList*)` / `SubmitAll(Span<...>)` 在 device 上 | `Engine/RHI/RHI/CommandList.h:283`、`RHI.h:172`、`RHI.h:208` | A 方案中只允许 RHI 线程调用 |
| ✅ `CreateDescriptorSetLayout` 返回 **`DescriptorSetLayoutHandle`**、`CreateFence` 返回 **`RHIFenceHandle`** | `RHI.h:99`、`RHI.h:188` | **句柄式 API 已有先例**，可照此推广到 buffer/texture/PSO |
| ✅ 已有 `EnqueuePSOCreate` / `ProcessPSOCreateQueue` / `GetPendingPSOCreateCount` | `RHI.h:82-86` | **PSO 异步创建的雏形已存在**，A-5 只需扩展它，不必从零设计 |

### 12.5 B 方案里"必须提前定"的 5 个决定

| B 阶段的决定 | A 阶段的要求 | 建议提前到 |
|---|---|---|
| 资源创建同步返回 `unique_ptr`，各 Pass 用 `unique_ptr` 成员持有 | 改为**句柄 + 延迟解析**（并由 `FrameRetireQueue` 承担生命周期） | **阶段 0（T0.7）**——否则要二次动全工程 200+ 处持有者 |
| `BufferDesc::initialData` 同步上传（`VulkanResources.cpp:486,651`） | 拷贝入队，RHI 线程执行 | 阶段 0 |
| device 由渲染线程持有并直接调 `Create*` | device 归 RHI 线程；渲染线程只拿"命令接口" | 阶段 2（T2.6，分水岭） |
| 交换链归渲染线程 | 归 **RHI 线程** | 阶段 2 起就抽象成 `ISwapChainController`，不让管线直接持有 |
| ImGui 后端直接写命令缓冲（`EditorApp.cpp:536-542`） | UI 只产 `ImDrawData` → 命令流 | 阶段 4，但接口**不得暴露** `VkCommandBuffer` |

### 12.6 逐任务改动表

| 原任务 | A 方案的改法 | 增量 |
|---|---|---|
| T0.1 线程断言 | 断言分**双角色**：记录期 = 渲染线程、执行期 = RHI 线程 | 小 |
| **T0.6（新增）** | `RHICommandList` 记录端契约 + `RHICommand` 载荷（§12.7） | **大** |
| **T0.7（新增）** | 资源句柄化（`RHIBufferHandle` / `RHITextureHandle` + generation） | **大** |
| T0.3 / T0.4 命令队列与帧票据 | 基本不变，额外加一层 RHI 背压计数 | 小 |
| **T1.x 场景快照（阶段 1）** | ✅ **完全复用，零改动** | **0** |
| T2.2 交换链迁移 | 目标线程从"渲染线程"改为 **RHI 线程** | 小 |
| T2.3 资源创建服务 | 从"转发到渲染线程执行"改为"**命令化，RHI 线程执行**" | 中 |
| **T2.6 `RenderThreadContext`** | 从"RHI 唯一出口"升级为"**只能吐命令流**"，不再暴露 device | **大** |
| **阶段 2.5（新增）** | RHI 线程落地：消费命令流 + 唯一提交 + fence 管理 | **大** |
| T3.1~T3.4 并行录制 | worker 产出的命令流片段由 **RHI 线程合并翻译**（UE 的 `TranslateCommandList` 模式） | 中 |
| T4.1~T4.4 编辑器 / ImGui | UI 命令化；后门接口改为命令流内的描述符池引用（`RHI.h:163-169` 移除） | 中 |
| T5.x 验收调优 | 增加三层背压调优、提交批优化、`enableRHIThread` 运行期开关 | 中 |

### 12.7 命令流接口草案

```cpp
// Engine/RHI/RHI/RHICommandList.h —— 记录端（渲染线程唯一可调，对应 UE 的 FRHICommandList）
namespace he::rhi {

/// 一条命令的载荷：类型擦除 + **内联存储**。
/// 为什么内联：每帧可能有数千条命令，逐条堆分配会成为新的瓶颈（UE 同做法）。
struct RHICommand {
    u32 type;                            // 命令类型（SetPipeline / DrawIndexed / PipelineBarrier / …）
    u32 payloadSize;                     // 实际参数字节数
    alignas(16) u8 inlinePayload[64];    // 小参数直接内联；超出 64B 的走列表私有 arena
};

/// 命令列表 = 命令流 + 参数 arena。渲染线程写，RHI 线程读；交接后只读（铁律 2）。
class RHICommandList {
public:
    /// 记录一条命令：参数按值内联，禁止捕获会被游戏线程继续修改的引用
    template <typename TPayload>
    void Enqueue(u32 type, const TPayload& payload) {
        static_assert(sizeof(TPayload) <= 64, "参数超过 64B，请走 AllocArena 通道");
        // … 追加到 m_Commands
    }

    /// 大块数据（常量缓冲、上传源、SBT 记录）写入本列表私有 arena，RHI 线程执行期读取
    [[nodiscard]] void* AllocArena(u64 size, u64 alignment = 16);

    /// 标记本列表为此帧的提交边界（RHI 线程翻译完成后执行一次 submit）
    void MarkSubmitBoundary() { m_HasSubmit = true; }

private:
    std::vector<RHICommand> m_Commands;
    // … arena / 提交标记 / 目标队列类型
};

/// 资源句柄：取代 unique_ptr 的同步所有权语义；generation 用于检测悬挂引用
struct RHIBufferHandle  { u32 index = 0; u32 generation = 0; };
struct RHITextureHandle { u32 index = 0; u32 generation = 0; };

} // namespace he::rhi
```

```cpp
// Engine/RHI/Threading/RHIThread.h —— 执行端（对应 UE 的 FRHICommandListExecutor + FRHIThread）
namespace he::rhi {

/// RHI 线程：命令流的唯一消费者与唯一提交者。
/// 职责：翻译 RHICommand → 平台 API；执行 queue submit；Acquire/Present；fence 管理。
/// 对应关系：等价于 UE 的 r.RHIThread.Enable=1 模式。
class RHIThread {
public:
    bool Start(IRHIDevice* device);          // 设备由本线程创建与持有
    void Stop();

    /// 渲染线程：提交一帧命令流（有界队列，满则渲染线程阻塞 —— 第二层背压）
    void EnqueueCommandList(RHICommandList&& list);

    /// 渲染线程：同步点（仅当确实需要"GPU 已完成某事"时使用，禁止进入帧循环）
    void Sync();

    static bool IsCurrent();

private:
    void ThreadMain();                        // 消费命令流 → 翻译 → 提交 → 呈现
    std::thread     m_Thread;
    RHICommandQueue m_Queue;                  // 有界
    // … fence / 提交批 / 背压计数
};

} // namespace he::rhi
```

### 12.8 落地策略：把 A 做成"加开关"，而不是重写

| 步骤 | 内容 | 验收 |
|---|---|---|
| **A1** | 命令流落地，**执行位置 = 渲染线程就地执行**（翻译与执行都在渲染线程） | 行为与 B **逐像素一致**、帧时间不退化 ⇒ 证明命令流层本身不引入开销 |
| **A2** | 新增 `RHIThread`，执行位置切到它（`enableRHIThread` 开关）；**两个位置共用同一套命令流代码** | 主线程与渲染线程零 API 调用；提交在 RHI 线程；validation（含 sync）零告警 |
| **A3** | 三层背压调优 + **提交批优化**（合并 barrier、批量描述符更新、批量提交） | `vkQueueSubmit` 调用次数下降；渲染线程可跑前于 RHI 线程 |

> 运行期开关直接对齐 UE 的 `r.RHIThread.Enable`：**关 = 渲染线程就地执行；开 = RHI 线程执行**。
> 这也解释了为何 UE 能把它做成可选——它的结构从第一天起就是"记录/执行分离"。

### 12.9 代价与收益

| 项 | 评估 |
|---|---|
| 额外工作量（不预埋） | 命令流层 + RHI 线程 + 句柄化改造：在 B 的 33~62 人日之上 **+15~30 人日** |
| 额外工作量（阶段 0 预埋 T0.6/T0.7） | 增量降到 **+6~12 人日**（预埋成本约 +5~8 人日） |
| 收益兑现条件 | ① 落地 **D3D12 后端**（描述符堆/PSO 开销大，RHI 线程收益显著）；② 实测"提交 + 驱动时间"占比 > 10~15%；③ 需要渲染线程在提交期间继续跑下一帧 |
| 新增风险 | 三层时序难以调试；**命令流翻译本身可能成为新瓶颈**——这正是必须先做 A1（就地执行版）验证的原因 |
| 与 §7 风险表的关系 | §7 的 R1~R7 全部适用；额外增加 R8「命令流翻译开销」与 R9「三层背压抖动」 |

### 12.10 不建议的做法

| 做法 | 为什么不好 |
|---|---|
| 先把 B 做完，再回头把 `vkCmd*` 调用"包装"成命令流 | 等于把渲染层重写一遍；且 `unique_ptr` 资源持有者已扩散到 200+ 处 |
| 用 `std::function` 作为命令载荷 | 每帧数千次堆分配 + 间接调用；应为类型擦除 + 内联存储 |
| 一开始就上三线程 | 录制耗时（16.4 ms，见 §1.5）才是当前瓶颈，三线程治不了它；且前置改造面最大 |
| 让渲染线程与 RHI 线程都能调 `IRHIDevice::Create*` | 资源创建就必须线程安全化，等于把 RHI 改成两套锁；应保持"创建只在 RHI 线程" |

---

> **文档版本**：v1.1（2026-09-22）
> **性质**：实施计划（未开工）。开工后每完成一个任务，请回到 §9 勾选并在 §6 记录实测数字。
> **v1.1 变更**：新增 §12 附录 C「升级到 UE 三线程模型的增量路径」；阶段 0 增加预埋任务 **T0.6（RHI 命令流契约）** 与 **T0.7（资源句柄化）**，二者是 §12 所列升级路径的前置条件。
