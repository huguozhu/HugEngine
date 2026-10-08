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
| **T0.5** | `EngineConfig` 新增**三态渲染线程模式** `RenderThreadingMode`（`SingleThreaded` 默认 / `RenderThread` / `RenderThreadAndRHI`，取代原 `enableRenderThread` 布尔 —— 见下方说明）与 `renderThreadSpinWaitUs`；运行期可切换（`SetRenderThreadingMode`）+ `HE_RENDER_THREADING_MODE` 覆盖 | `Engine/Core/Core/RenderThreadingMode.h`（新增）、`Engine/Core/Core/Engine.{h,cpp}` | 编译期与运行期开关均可切换；关闭时走旧路径 |
| **T0.6** | **（为 §12 预埋，可选但强烈建议）** 定义 RHI 命令流的**记录端契约**与 `RHICommand` 载荷形态（类型擦除 + 内联参数），暂不改变执行方式 | `Engine/RHI/RHI/RHICommandList.h`（新增） | 契约评审通过；本轮不接入生产路径，只落头文件与单测 |
| **T0.7** | **（为 §12 预埋，可选但强烈建议）** 资源句柄化：新增 `RHIBufferHandle` / `RHITextureHandle`（含 generation），并让新代码优先用句柄；`unique_ptr<IRHIBuffer>` 保留为兼容层 | `Engine/RHI/RHI/RHI.h`、`Engine/RHI/RHI/RHIHandles.h`（新增） | 新代码不再新增 `unique_ptr<IRHIBuffer>` 成员（grep 断言）；旧代码可渐进迁移 |

**退出判据**：阶段 0 全绿 = 现有样例在 `enableRenderThread=false/true`（壳模式）下**输出一致**、帧时间不退化 > 3%。
> ✅ **已达成（2026-09-24，multi_thread 分支）**：
> - **输出一致**：`06.GILab`（Cornell Box，121 帧）在单线程与渲染线程壳下，关掉已知噪声源
>   （`HE_LUMEN_PROBE_FILTER=off`）后 **28 个转储目标全部逐位相同**；默认滤波下差异**仅**出现在已有的
>   Lumen 屏幕探针家族（`lumen_irradiance` + 4×`prov6_*` + `hdr`），与本次改动无关。
> - **帧时间不退化**：样例自测的每帧均值（最近 120 帧）—— 墙钟 465.190 → 465.143 ms、
>   CPU 侧 465.620 → 465.526 ms、管线 `Render` 464.057 → 464.279 ms（**+0.05%**，远低于 3% 预算）。
> - 逐任务记录见 §9（T0.1~T0.7 全部勾选）与 §13 附录 D（帧内同步调用清点）。
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

> **阶段 0 实测数字（2026-09-24，multi_thread 分支，`06.GILab` Cornell Box，121 帧，样例自测每帧均值）**：
>
> | 指标 | 单线程渲染 | 渲染线程壳（`HE_RENDER_THREADING_MODE=1`） | 变化 |
> |---|---|---|---|
> | 墙钟 / 帧 | 465.190 ms | 465.143 ms | −0.01% |
> | CPU 侧 / 帧 | 465.620 ms | 465.526 ms | −0.02% |
> | 管线 `Render` / 帧 | 464.057 ms | 464.279 ms | **+0.05%**（判据：不退化 > 3%） |
> | 转储一致性 | 28 个目标逐位相同（关掉已知 Lumen 噪声源后） | 同左 | 0 差异 |
>
> 说明：这两次运行都是**壳模式**（命令在同一线程执行），因此这里的差值只反映"入队 + 发布 + 回收票据"
> 的开销；真正把渲染搬到另一根线程是阶段 2，届时"游戏 tick 与渲染并行"才会体现在墙钟帧时上。

> **阶段 0 / 1 累计实测（截至 2026-09-24，`multi_thread` 分支）**
>
> | 项 | 数字 | 说明 / 复现 |
> |---|---|---|
> | 单测规模 | **384 例 / 71703 断言全通过** | 起点 331 例 / 71291（阶段 0 之前） |
> | 阶段 0 输出一致性 | 28 个转储目标**逐位相同** | 关掉已知噪声源（`HE_LUMEN_PROBE_FILTER=off`），单线程 vs 渲染线程壳 |
> | 阶段 0 帧时间 | 管线 `Render` **+0.05%**（判据：不退化 > 3%） | `06.GILab` 121 帧、样例自测每帧均值 |
> | 附录 B1 世界依赖 | 渲染期 **82** 处 / 加载期 15 处（目标 = 渲染期 0） | `Tools/check_threading.py --world-deps`；快照层在白名单内 |
> | T0.7 资源持有者 | **277** 处 / 76 个文件（只允许下降） | `Tools/check_threading.py --handles` |
> | 帧内同步 RHI 调用 | `Engine/Render` **376** 处、`Samples` **127** 处 | `Tools/check_threading.py` + §13 附录 D |
> | 全帧转储噪声底噪 | **≈ 4.5k 像素**（同二进制两趟） | 见 §9 T1.3a；因此单条路径迁移改用"参考实现 + 逐位比较"判据 |
>
> 判据口径的两点提醒（都已写进 §9）：① 迁移类改动一律用**旧实现逐行转写 + `memcmp` 逐位比较**
> （全帧转储在单条路径尺度上不可判定）；② 闸门基线（B1 82、持有者 277）是**上限**，随收敛手动下调。
>
> **端到端复核（2026-09-24，阶段 1 收尾时的绿灯基线）**：7 个目标（`HugEngineTests` / `06.GILab` /
> `07.Nanite` / `03.Sponza-Forward` / `05.Sponza-PathTracing` / `02.Cube` / `04.Sponza-Deferred`）
> 全部编译通过；单测 **384 例 / 71703 断言**；两项闸门都在基线（B1 渲染期 82、持有者 277）；
> `acceptance_sweep.ps1 -OnlyNanite` **PASS**（两类 pass 指纹 `1C15AB72E688B530` / `750CC247BF8B9C3D`
> 未变，CULL DIFF / PIC CMP / TAKEOVER CMP 全过）⇒ 阶段 0 与本阶段改动**未破坏既有验收**。

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

**总开关**：`EngineConfig::renderThreadingMode` 必须始终可回退到 `RenderThreadingMode::SingleThreaded`（默认值），
且三种模式共享同一套快照与命令队列代码（差异只在"谁来消费"）。
> **实现口径变更（2026-09-24，T0.5 落地时）**：原方案写的是布尔 `enableRenderThread`；实施时改为**三态参数**
> `RenderThreadingMode{SingleThreaded, RenderThread, RenderThreadAndRHI}`。理由：该参数同时决定 §12 附录 C 的
> 三线程升级路径，一次定清可以避免以后再改配置语义；且"关 = 就地执行 / 开 = 另一根线程执行"这种两段式开关
> 本就在 §12.8 的 A2 里要再引一个 `enableRHIThread`，改成三态后单参数即可覆盖三种形态。
> 运行期入口：`SetRenderThreadingMode()` / 环境变量 `HE_RENDER_THREADING_MODE`（`0|1|2` 或名称；非法值只告警、保持原模式，不静默退回）。

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

- [x] T0.1 线程归属断言（`ThreadAffinity` + `HE_ASSERT_RENDER_THREAD`）
  - 落地：`Engine/RHI/RHI/ThreadAffinity.h`（原子拥有者 + 宏）；认领点 `CreateDevice`，注销点 `~VulkanDevice`；
    断言点 `Shutdown` / `Submit` / `SubmitAll` / `SignalFenceOnQueue` / `WaitFenceOnQueue` /
    `AcquireNextImage` / `Present` / `VulkanCommandList::Begin` / `BeginSecondary` / `BeginLightweight` / `End` / `Submit`。
  - 实测（2026-09-24，multi_thread 分支）：单测 **334 例 / 71303 断言全通过**（含新增的
    `TestThreadAffinity.cpp` 三态用例与 `TestThreadAffinityDisabled.cpp` 关闭路径用例）；
    `06.GILab` 实跑 120 帧零断言触发、28 个转储正常、`vuid_lines=42` 无新增。
- [x] T0.2 帧内同步 RHI 调用清点清单
  - 产出 **§13 附录 D**：`Engine/Render` 帧内 **376** 处 / 加载期 247 处，`Samples` 帧内 **127** 处；
    按 `SWAPCHAIN / WAIT / CREATE / MAP / UPLOAD_DESC` 分类，每类给出处置与所属阶段、代表 `file:line`
    与最集中的文件（方向与量级已按文件聚合排序）。
  - 配套脚本 `Tools/check_threading.py`（`--detail` 明细、`--root` 换目录、`--gate` 闸门），
    是附录 B 各阶段退出判据的可执行版本。
- [x] T0.3 `RenderCommandQueue` + `RenderThread` 壳实现（行为等价）
  - 落地：`Engine/Render/Threading/{RenderThreadContext.h, RenderCommandQueue.h/.cpp, RenderThread.h/.cpp}`
    （**RHI-free**，单测直接编译这几个 .cpp）+ `Samples/06.GILab` 接线 —— 模式 ≠ 单线程时，每帧的
    `curPipeline->Render(...)` 经 `BeginFrame/Enqueue/Publish` 交给 `FrameScheduler::SubmitAndPump()`
    在**调用线程**上消费；单线程模式走旧路径，两条路径共用同一份渲染代码。
  - 判据实测（2026-09-24）：关掉已知噪声源（`HE_LUMEN_PROBE_FILTER=off`）后，单线程与壳模式两次运行的
    **28 个转储目标全部逐位相同（`total differing pixels = 0`）**；默认滤波下差异**仅**出现在已知的
    Lumen 屏幕探针家族（`lumen_irradiance` + 4×`prov6_*` + `hdr`），与本次改动无关。
  - 遗留：其余 6 个样例与编辑器的接线随 **T2.4**（同源改造：循环体 → 投递 + `SubmitFrame`）。
  - 已知并记录的偏离：阶段 0 的命令载荷仍按**引用**捕获，铁律 2 要求的"按值捕获"要等阶段 1 的
    `FrameSceneSnapshot` 把渲染输入变成不可变数据后才能真正满足（T1.2/T2.4）。
- [x] T0.4 帧票据与背压骨架
  - 落地：`FrameTicket`（帧号 + 槽位）、在飞计数（发布计入 / `RetireFrame` 递减并唤醒）、
    背压两种语义 —— `SubmitFrameBlocking`（游戏线程**唯一**允许的等待点）与 `TrySubmitFrame`
    （在飞满时拒绝并把该帧计入丢弃数）；观测项：已发布 / 已回收 / 丢弃 / 背压等待次数。
  - 判据实测（2026-09-24）：单测把上限设为 1 ⇒ 第 2 次非阻塞提交被拒且 `DroppedFrameCount()==1`；
    **阻塞提交在子线程上确实等到 `RetireFrame` 才返回**（轮询观测 + 背压等待计数 = 1），
    即 T0.4 要求的"人为把上限设为 1 时能观察到游戏线程阻塞"有可测形态。
- [x] T0.5 `EngineConfig::enableRenderThread` / `renderThreadSpinWaitUs`
  - **实施口径**：改为三态参数 `RenderThreadingMode{SingleThreaded=0, RenderThread=1, RenderThreadAndRHI=2}`
    （单线程渲染 / 游戏线程+渲染线程 / 游戏线程+渲染线程+RHI 线程），默认 `SingleThreaded`；
    `EngineConfig::renderThreadingMode` + `renderThreadSpinWaitUs`（默认 0 = 不自旋，空闲不烧核），
    并提供 `UsesRenderThread()` / `UsesRHIThread()` 判据。
  - 落地：`Engine/Core/Core/RenderThreadingMode.h`（枚举 + 规范名 + 解析 + 进程级读写）、
    `Engine/Core/Core/Engine.{h,cpp}`（配置字段 + `Initialize` 落地 + 环境变量覆盖 + 非法值告警）。
  - 实测（2026-09-24）：单测 **341 例 / 71345 断言全通过**（新增 6 例：默认值、三态×判据真值表、
    规范名、解析（数字/名称/别名/大小写）、非法输入必须 `nullopt`、进程级读写一致性）；
    `06.GILab` 实跑三种配置的日志分别为 `渲染线程模式 = game+render+rhi（2）`、
    非法值 `triple` → 告警并保持 `single-threaded`、不设环境变量 → `single-threaded（0）`，28 个转储正常。
- [x] T0.6 **（预埋，见 §12）** RHI 命令流记录端契约 + `RHICommand` 载荷形态
  - 落地：`Engine/RHI/RHI/RHICommandList.h`（头文件实现，**不接入生产路径**）：
    `RHICommand`（类型擦除 + 64B 内联载荷 + arena 偏移）、`RHICommandType`（骨架代表值）、
    `RHICommandList`（`Enqueue` / `EnqueueArena` / `AllocArena` / `MarkSubmitBoundary` / `Reset`
    + 执行端读取用的 `PayloadAt` / `ArenaPayloadAt`，不匹配返回 nullptr 以便容错）。
  - 7 例单测：内联按值拷贝与读回、64B 边界、错类型/越界读取为 nullptr、arena 不重叠与偏移读回、
    **子对齐载荷的绝对地址对齐**、提交边界、`Reset` 复用不残留。
  - 实测（2026-09-24）：单测 **356 例 / 71443 断言全通过**（改前 349 / 71383）。
  - 【单测当场抓到的契约缺陷】初版用 `std::vector<u8>` 存 arena、只对齐**偏移量** ⇒ 绝对地址不保证
    对齐（64B 用例量到 48）。已改为**固定容量、64B 对齐的块**（对齐建立在绝对地址上；容量耗尽即断言，
    可增长块分配器随 §12 的 A-1 落地）。
- [x] T0.7 **（预埋，见 §12）** 资源句柄化（`RHIBufferHandle` / `RHITextureHandle` + generation）
  - 落地：`Engine/RHI/RHI/RHIHandles.h`（头文件实现）：两种句柄（`index == 0` 为无效、`generation` 代次）
    + 句柄表 `RHIResourceTable<THandle, TResource>`（`Add` / `Get` / `Remove` / `LiveCount` /
    `SlotCount` / `FreeSlotCount`）+ `RHIBufferTable` / `RHITextureTable` 别名。
    **本阶段不强制迁移**任何现有持有者，`unique_ptr` 保持为兼容层（§12.5 的"渐进迁移"）。
  - 为什么代次是关键：槽位会被回收复用，只有下标的句柄在复用后会**静默指向另一个资源**
    （不崩、画面莫名错乱，最难查的一类 bug）。`Remove` 提升槽位代次 ⇒ 旧句柄一律解析为 nullptr，
    错误立刻表现为"资源为空"。
  - 6 例单测：登记/解析（下标从 1 开始）、`Remove` 后失效且幂等、**槽位复用后代次前进且旧句柄
    绝不指向新资源**、越界/伪造代次/跨表句柄一律 nullptr、多轮复用槽位不增长、两种句柄互不干扰。
  - 实测（2026-09-24）：单测 **362 例 / 71495 断言全通过**（改前 356 / 71443）。
  - grep 闸门已接入 `Tools/check_threading.py --handles`：实测 **277 处 / 76 个文件**（正是 §12.5
    所说"200+ 处持有者"）。该基数是**上限而非目标** —— 迁移可以渐进，但**新增一处**即说明新代码
    没走句柄，闸门会失败。
- [x] T1.1 `FrameSceneSnapshot` 定义（与 `ShaderTypes.slang` 对齐 + `static_assert`）
  - 落地：`Engine/Render/Threading/FrameSceneSnapshot.h`（值语义、无指针、RHI-free）：
    `SnapshotDrawItem`（内嵌 `GPUObjectData` + `prevWorldMatrix` + `meshIndex`/`objectID`/
    `visibilityFlags` + 间接绘制三元组）、`SnapshotLight`（**逐字段镜像** `GPULight`，
    带 `ToGpu()` / `FromGpu()`）、`FrameSceneSnapshot`（帧号/槽位/相机/dt/视口/draws/lights/
    skinMatrices/`sourceWorldVersion` + `Clear()` / `Reserve()` / `IsEmpty()`）。
  - 两条硬约束写进文件头：① **只放值不放指针**（资源只放索引与 ID，由渲染线程自查表），
    否则"交接后只读"是假的；② **布局与着色器同源** —— 小结构逐字段镜像 + 静态断言，
    大结构直接内嵌 GPU 结构，杜绝两份字段定义漂移。
  - 布局钉子（本任务的核心判据）：`SnapshotLight` 与 `GPULight` 的**大小与每个字段偏移**逐一
    `static_assert`（只锁大小不够：字段互换位置时大小不变但着色器会读错）；
    `SnapshotDrawItem` 锁"可平凡拷贝 + 16B 对齐"，`CameraData` 锁"值类型"。
  - 实测（2026-09-24）：单测 **366 例 / 71530 断言全通过**（改前 362 / 71495）。
  - 实现细节留痕：`ShaderTypes.slang` 必须在 `he::render` 命名空间内包含（与
    `Pipeline/Material.h` 同一用法），否则 `float4` / `float4x4` 在全局作用域不可见。
- [ ] T1.2 `SceneSnapshotBuilder`（集中现有 Collect 遍历）
  - **T1.2a 已完成（光源，2026-09-24）**：`Engine/Render/Threading/SceneSnapshotBuilder.{h,cpp}` ——
    `BuildLights(world, sg, resolvers, snapshot)`，口径与 `CollectLights` 逐字段对齐
    （遍历顺序 方向光→点光→聚光、关闭跳过、`kGPUMaxLights` 截断、色温叠加、物理模式**负范围/`-1` 标记**、
    聚光归一化方向 + 内外锥角、阴影索引，且"未知类型也落一条"的旧行为一并保留）。
    新增 `SceneSnapshotResolvers`（全局物理光开关 + 阴影索引回调），使快照层**不读全局 CVar、
    不反向依赖阴影系统**，收集逻辑成为纯函数、可单测；`PhysicalLight.h` 的物理光判据拆出
    "全局开关"重载，保证唯一判据。
    单测 8 例，实测 **374 例 / 71574 断言全通过**。
  - **T1.2b 已完成（物体收集，2026-09-24）**：`SceneSnapshotBuilder::BuildObjects` 集中
    `GPUScene::Collect` 的首次全量遍历（组件类型顺序、无索引跳过、贴花排除开关、广告牌/文字的
    相机对齐矩阵、世界 AABB、`objectID`、`visibilityFlags`，与 `FillObj` 逐字段对齐）；
    单组件映射抽成模板 `CollectObjectItem<TComponent>` 以便单测（组件存储按精确类型分桶，
    假组件无法进入遍历）。`SnapshotDrawItem` 增加 `materialIndex`（≠ `object.materialID`）。
    实测：单测 **379 例 / 71666 断言全通过**。
  - **T1.2c 待做（已查明设计阻塞点，2026-09-24）**：材质参数（`GPUObjectData` 的非矩阵/非 AABB 字段）
    目前仍由 `SceneRenderer::Prepare(world, sg, camera, objectBuffer, excludeDecals)` 在**带视锥剔除的
    渲染侧遍历**里填。查清后有 3 个必须先定的问题：
    ① **`SceneRenderer` 有自己的第二套遍历**，且与 `GPUScene::Collect`/`BuildObjects` **口径不一致**：
    它额外包含 `SplineMeshComponent`（样条网格），而后者不包含 —— **新发现的第 5 处收集口径漂移**
    （前四处见 `SceneSnapshotBuilder.h`：Rect 光、点光方向、聚光归一化、shadowRadius）；
    ② 它的 DrawList 元素携带 **`MeshComponent*` 指针**，而快照**不允许带指针** ⇒ 需要先有一个
    **mesh 注册表**（`meshIndex` ↔ 顶点/索引缓冲 + 材质路径），把"指针"换成索引；这也是 §4.2 里
    `SnapshotDrawItem::meshId` 的隐含前提；
    ③ 材质填充依赖 **5 条纹理路径字符串**（决定 `textureMask`）⇒ 快照化的更简做法是在**收集侧**就跑
    `FillObjectData`（纯计算），把**算好的 `GPUObjectData` 字段**放进快照（`SnapshotDrawItem::object`
    生来就是这个用途），而不是把字符串搬进快照。
    结论：T1.2c 应先定 **mesh 注册表**，再谈材质快照化；在此之前 `SceneRenderer` 保持现状。
  - **E-1 已完成（mesh 注册表本体，2026-09-24）**：`Engine/Render/Threading/MeshRegistry.h`
    （头文件实现）—— `MeshRegistryEntry` + `Register`（同 key 重复=更新，索引不变）/`Unregister`
    （幂等、索引复用、旧索引解析为 nullptr）/`Find`（越界/已注销 ⇒ nullptr）/`Count`/`Clear`，
    索引从 1 起（0 为哨兵）。头文件写明"只借不拥有""**禁止帧内注销**"两条纪律，并显式记录
    **已知限制**：索引复用后旧索引会指向新条目 ⇒ 将来把条目换成带 generation 的 `RHIBufferHandle`
    （与 §12 A-3 对齐）。单测 4 例；实测 **388 例 / 71729 断言全通过**。
    下一步 **E-2**：骨骼上传改遍历快照的骨骼条目 + 用 `meshIndex` 从注册表取缓冲。
  - **E-2 已完成（2026-09-24）**：
    - 前置：`MeshComponent::meshIndex` 字段（注册时回填）；`CollectObjectItem` 透传它（+ 断言钉子）；
      `MeshRegistryEntry::skinMatrixBuffer`（骨骼上传的**写入目标**，只登记顶点/索引缓冲不够）；
      注册改为**每帧刷新**（骨骼缓冲会重建 ⇒ 一次性注册会留过期指针；`Register` 同 key = 更新、索引不变）
      + 语义钉子测试；`SnapshotDrawItem::sourceEntity`（逐实体状态机的对齐依据）。
    - 本体：`ForwardPipeline` 的骨骼循环按 `sourceEntity` 取 `m_Snapshot.skinMatrices` 切片作为**矩阵来源**，
      三处使用点改用该切片、边界显式退回组件；**缓冲生命周期（脏标记/容量/扩建/bindless/退役队列）
      完全不动**（属渲染侧资源管理，将来归 T2.3）。
    - 为让快照在骨骼循环之前完整，`ForwardPipeline::Render` 增加了 `BuildObjects` 调用。
    - 判据说明（如实）：该路径**没有转储 harness**（`03.Sponza-Forward` 无 dump 脚本），因此是
      「编译 + 单测 + 同帧同源论证」；`BuildObjects` 先于该循环、中间无游戏 tick ⇒ 数据逐位相同。
  - **E-3 已做一半（2026-09-24）**：① 把「组件 → `PBRMaterial`」10 项映射从 `SceneRenderer::Prepare`
    抽成 `SceneSnapshotBuilder::MakePBRMaterial`（**唯一实现**，两侧共用，避免将来漂移），并加**逐字段
    参考比对**测试（旧映射逐行转写）；② `CollectObjectItem` 用 `if constexpr (is_base_of_v<MeshComponent,…>)`
    在**收集侧**调用 `FillObjectData(item.object, MakePBRMaterial(comp))` —— 材质字段在收集侧算完，
    渲染侧因此不必拿 `MeshComponent*`，也不必把 5 条纹理路径搬进快照。
    **E-3 后半待做**：`SceneRenderer::Prepare` 改为消费快照（`entry` 去掉 `MeshComponent*`、材质取
    `item.object`、`DrawItem` 用 `meshIndex` 从注册表取缓冲；**剔除仍留渲染线程**），判据为
    逐字段/逐位 + `06.GILab` 28 个转储粗筛。
  - **T1.2b 待做**：物体收集（`GPUScene::Collect` 的遍历 + 材质参数 + 间接绘制参数），
    以及本任务退出判据要求的"与旧路径并行跑一帧、逐字段比对（`HE_SNAPSHOT_VERIFY`）" ——
    该判据在 T1.3 让管线消费快照时最自然（可直接对比 UBO/SSBO 字节）。
  - **顺带发现的三处口径问题**（已登记，不在本任务内改）：
    ① 聚光方向：Deferred 归一化、Forward 不归一化；
    ② 点光 `directionType.xyz`：Forward 写 (0,-1,0)、Deferred 留 0；
    ③ `PhysicalLight.h::KelvinToRGB` 的二次近似在 6500K 给出 (1, 0.46, 0)、2000K 给出 (1,0,1)，
       与黑体常识不符，疑似系数抄错（仅 `colorTemperature > 0` 时生效）——建议单独立项核查。
- [ ] T1.3 `CollectLights` / `GPUScene::Collect` 改消费快照
  - **T1.3a 已完成（Deferred 光源，2026-09-24）**：`DeferredPipeline::CollectLights` 改走
    `SceneSnapshotBuilder::BuildLights` + **一次性**上传（旧实现每个光源 Map/Unmap 一次）；
    新增 `SceneSnapshotLightOptions` 口径开关，默认值 = Deferred 现行行为（迁移先保一致，
    "统一两条管线的口径"留作另一次可单独验证/回退的改动）。
    等价性判据：单测把**改动前的内联逻辑逐行转写**为参考实现，用覆盖全部分支的场景
    逐元素 `memcmp` 比较 ⇒ **逐位一致**。
  - **重要实测（影响本方案的验收口径）**：当前构建下 `06.GILab` **同一二进制两趟**、
    即使带 `HE_LUMEN_PROBE_FILTER=off`，也差约 **4.5k 像素**（`hdr` 719 px / maxULP 21 /
    meanAbs 5.9e-8；`lumen_irradiance` 764 px / maxULP 552）⇒ 本方案里"逐像素一致"这类判据
    在**单条收集路径迁移**的尺度上不可判定（本次改动与基线的 4574 px 差异正落在该噪声底噪之内）。
    根因与画质线登记的「Lumen 屏幕探针逐趟不确定」同源（根因已定位为 mesh 级 SDF 泛洪的**就地**
    读-写竞争；修复曾以 `f6566b6` 提交后按要求回退，补丁留在 `build/verify/jfa_only.patch`）。
    **在此之前，迁移类改动一律用"参考实现 + 逐位比较"的单元级判据**，全帧转储只用于粗筛并如实标注噪声。
  - **T1.3b-1 已完成（口径开关 + 三口径等价性，2026-09-24）**：`SceneSnapshotBuilder` 支持
    `RectLight`（`directionType.w=3`、`coneAngles` 复用为宽×高）与四个口径开关
    （`includeRectLights` / `pointLightWritesDirection` / `normalizeSpotDirection` / `writeShadowRadius`），
    默认值 = Deferred 现状。**三条管线的光源口径共有 4 处历史漂移**（Rect 光、点光 `directionType.xyz`、
    聚光归一化、`shadowRadius`），迁移先逐条保持，统一另立改动。
    等价性：把 Deferred 与 Forward 的收集逻辑**逐行转写**为两份参考实现，场景覆盖三类光源 + Rect +
    色温 + 物理照度/光强 + 关闭 + 超上限，在物理开关两态下**三口径全部逐位一致**（376 例 / 71647 断言）。
  - **T1.3b-2 已完成（Forward / PathTracing 接线，2026-09-24）**：两条管线的 `CollectLights` 都改走
    `SceneSnapshotBuilder`（Forward 显式打开三处历史口径开关；PathTracing `writeShadowRadius=true`
    且不设阴影解析器 ⇒ `shadowIndex` 恒 -1），各新增 `m_LightSnapshot` 成员。**至此三条管线的光源
    收集全部集中到一处**。Forward 的 `pc.atmosphere`（读物理天空组件，待 T1.4 快照化）与"无光源补
    默认方向光"（策略）保留在管线内。
    实测：三管线样例（03/05/06）编译通过；Deferred 冒烟与 T1.3a 之后逐位对比 = 4539 像素 = 该构建
    同二进制两趟的噪声底噪 ⇒ 无新差异。**未做**：03/05 的实跑（缺转储对照 harness，已记为待办）。
  - **T1.3b-3 已完成（`GPUScene` 消费快照，2026-09-24）**：`GPUScene::Collect(world, sg, camera)`
    改为"构建快照 → `CollectFromSnapshot`"（过渡签名保留，调用点不必一次全改）；新增内联
    `MakeObjectRecord`（与旧 `FillObj` 逐字段对齐、`meshIndex`/间接参数可透传）并**删除旧的约 60 行
    遍历/增量代码**（不并存，避免两份口径）。增量逻辑保留（集合变化 ⇒ 整表重建；否则只更新矩阵变化的条目）。
    验收：单测 **381 例 / 71672 断言全通过**（`MakeObjectRecord` 与逐行转写的旧 `FillObj`
    `memcmp` **逐位一致**）；`06.GILab` 冒烟与上一提交逐位对比 = **15 像素**，远低于同二进制噪声底噪
    （4539 像素）⇒ 无可测差异。
    > **判据替换说明**：T1.2 原要求 `HE_SNAPSHOT_VERIFY`（与旧路径并行跑一帧逐字段比对）。因全帧转储
    > 当前底噪约 4.5k 像素（见 T1.3a 记录），改用**更强**的判据：旧实现逐行转写为参考实现后做**逐位**
    > 比较，并用冒烟转储粗筛。后续所有迁移沿用此法。
  - **T1.3b 待做**：Forward / PathTracing 的光源收集同样改走快照（各自带历史口径开关）；
    `GPUScene::Collect` 改消费快照（依赖 T1.2b 的物体收集）。
- [ ] T1.4 骨骼/材质/Decal/粒子快照化
  - **起点（环境参数，2026-09-24）**：新增 `FrameSceneSnapshot::atmosphere`（xyz = 太阳方向、
    w = 浑浊度，与两个 PushConstant 的 `atmosphere` 逐字段一致）与
    `SceneSnapshotBuilder::BuildEnvironment`（找不到/未启用物理天空时复位为关闭，与旧行为逐字段一致）；
    `ForwardPipeline` 与 `DeferredPipeline_FrameGraph` 均已改用它 ⇒ **两条光栅管线都不再直接读世界**
    （PathTracing 不使用空中透视）。单测 1 例 3 子用例；实测 **382 例 / 71681 断言全通过**；
    `06.GILab` 冒烟与上一提交逐位对比 4554 像素（噪声底噪 4539）⇒ 无可测差异。
    **未做**：骨骼矩阵 / 材质参数（T1.2c）/ Decal / 粒子的快照化。
  - **骨骼矩阵进快照（2026-09-24）**：`SnapshotDrawItem` 增加 `skinMatrixOffset`/`skinMatrixCount`，
    `SceneSnapshotBuilder::AppendSkinMatrices` 把 `SkeletalMeshComponent::boneMatrices` 追加进
    `FrameSceneSnapshot::skinMatrices` 扁平数组（条目只记切片），`BuildObjects` 在骨骼网格支调用。
    单测 1 例（两段互不覆盖 / 共享数组内容 / 无骨骼不污染）；实测 **383 例 / 71691 断言全通过**。
    **消费侧未做**：把骨骼矩阵写进 `boneBuffer` 的那段遍历仍读组件 —— 它依赖"组件 → 缓冲"的指针映射
    即 T1.2c 待定的 **mesh 注册表**，两者一并做（届时快照已是现成数据源）。
  - **粒子发射器进快照（2026-09-24）**：`ParticleComponent` 增加 `rendererId`（注册方回填；
    两个注册粒子的样例已补），`FrameSceneSnapshot` 新增 `SnapshotParticleEmitter{rendererId,
    emitPosition}` 与 `particles`，`SceneSnapshotBuilder::BuildParticles` 按组件收集。
    **只需这两个字段**：粒子模拟/绘制早就按 id 索引驱动渲染器自有缓冲（`DispatchCompute(cmd, id, dt,
    viewProj)`），渲染期唯一读组件的地方就是"发射位置"。
    单测 1 例；实测 **384 例 / 71701 断言全通过**；`02.Cube` / `04.Sponza-Deferred` 编译通过。
    **消费侧未做**：`DispatchCompute` 仍从 `CompState::comp` 取位置与参数，下一步改从快照取
    （对外接口只需多两个入参，之后 `CompState` 不必在帧内读组件指针）。
  - **粒子消费侧闭环（2026-09-24）**：`DispatchCompute` 增加 `params` / `emitPosition` 两个入参，
    emit 分支里约 15 处 `comp->GetParam()` / `comp->GetWorldEmitPosition()` 改为用入参；
    `DeferredPipeline` / `PathTracingPipeline` 的模拟循环改为"先 `BuildParticles` → 遍历
    `m_Snapshot.particles` 按 `rendererId` 派发"。**帧内模拟路径不再读 `ParticleComponent`**
    （`CompState::comp` 仍在，但只被绘制路径的加载期语义数据使用）。
    实测：单测 384 例 / 71703 断言；5 个目标编译通过；冒烟 4615 像素（底噪 4539）⇒ 无连带回归。
    > 查清：emit 分支每帧读**约 15 个字段**（`ParticleComponent::GetParam()` 的方向/形状/速度/寿命/
    > 尺寸/纹理行列……），只搬"位置"不够 ⇒ 快照条目已改为**整份携带 `ParticleSystemParam`**，
    > 消费侧切换因此降为机械替换（实测单测 384 例 / 71703 断言）。
- [ ] T1.5 渲染期移除 World/SceneGraph 引用（grep 断言）
  - **闸门与基线已就位（2026-09-24）**：`Tools/check_threading.py --world-deps` 统计 `Engine/Render/`
    内 `World&` / `SceneGraph&` 的出现处，按**所属函数名**分"渲染期 / 加载期"（与调用点清点同一份
    白名单；快照层 `Engine/Render/Threading/` 在**白名单内** —— 它是渲染侧唯一允许读世界的地方）。
    **实测：渲染期 82 处 / 加载期 15 处**（阶段 1 退出目标 = 渲染期 **0**）。
    > 口径说明：同一函数的**声明与定义各算一处**（.h + .cpp），故数值大于函数个数；作为闸门只需
    > 前后一致、单调下降。迁移期它是**上限**（`--gate` 超基线即失败），随每次收敛手动下调。
  - 渲染期命中最多的文件（= T1.4/T1.5 的收敛对象）：`Pipeline/ForwardPipeline.h`(12)、
    `Pipeline/ForwardPipeline.cpp`(7)、`Pipeline/DeferredPipeline.h`(6)、
    `Pipeline/PathTracingPipeline.h`(5)、`Pipeline/RTPass.cpp`(4)、`Pipeline/GBufferRenderer.h`(3)、
    `Pipeline/RTPass.h`(3)、`Shadow/CSMTechnique.{h,cpp}`（各 3）、`Pipeline/DeferredPipeline.cpp`(2)、
    `Pipeline/DeferredPipeline_FrameGraph.cpp`(2)、`Pipeline/ForwardPipeline_FrameGraph.cpp`(2)……
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

## 13. 附录 D：阶段 0 T0.2 帧内同步 RHI 调用清单

> **性质**：阶段 0 T0.2 的产出（**只读盘点，不改代码**）。这张清单是阶段 2（T2.2/T2.3/T2.6）的派工单，
> 也是附录 B 各阶段退出闸门的可执行版本。

### 13.1 怎么复现

```powershell
python Tools\check_threading.py                  # 汇总（Engine/Render）
python Tools\check_threading.py --detail         # 附每条 file:line + 所属函数
python Tools\check_threading.py --root Samples   # 样例侧（附录 B 的 B2 口径）
python Tools\check_threading.py --gate           # 闸门：帧内命中 > 0 时退出码 1（各阶段退出用）
```

### 13.2 判定口径（刻意保守）

- **类别**：`SWAPCHAIN`（拿图/呈现）、`WAIT`（等待型提交 / 空闲等待）、`CREATE`（设备调用形态
  `x->Create*()`，含 `CreateTransient*`）、`MAP`（映射缓冲访问：直写上传或 GPU→CPU 读回）、
  `UPLOAD_DESC`（`desc.initialData` 创建期上传）。
- **帧内 vs 加载期**：取该行**所属函数名**，含 `Initialize / Init / Shutdown / Resize / Load / Upload /
  Setup / Construct / OnCreate` 之一 ⇒ 加载期，其余一律帧内。白名单**故意不含 `Build` / `Create`**：
  `BuildFrameGraph` 这类名字里有 Build 的函数**每帧都跑**，含进去会把真正的帧内创建藏起来。
- **已知两类噪声**（清单用于排序与派工，不当作证明）：
  1. **辅助函数误判为帧内**：如 `AA_TAA::CreateHistoryTextures` 只在 `Initialize` 里被调用，但函数名
     不含白名单词 ⇒ 记成帧内（保守方向：多报，不会漏报）；
  2. **头文件声明/文件作用域**：如 `RenderGraph.h` 内的声明行记为 `<文件作用域>` ⇒ 记成帧内。
  反向的漏报风险同样存在（多行函数签名会让"所属函数"退化成上一个可识别的函数名）。

### 13.3 清点结果（2026-09-24，multi_thread 分支）

`Engine/Render`（196 个文件）：

| 类别 | 帧内 | 加载期 | 处置（方案对应任务） |
|---|---|---|---|
| `SWAPCHAIN` | **0** | 0 | 交换链调用全在样例侧 ⇒ 见下表 + C2 |
| `WAIT` | **2** | 2 | 阶段 2：只允许渲染线程，帧内禁止（铁律 3）；加载期保留 |
| `CREATE` | **211** | 209 | 阶段 2 T2.3：帧内改走 `ResourceCreationService`（步 1 同步转发，仅限加载/重建） |
| `MAP` | **142** | 22 | 阶段 2 T2.6：读回改"登记 + N 帧后取值"（`FrameRetireQueue`）；直写改入队拷贝（铁律 2） |
| `UPLOAD_DESC` | **21** | 14 | 阶段 2 T2.3：创建期上传经创建服务在渲染线程执行 |
| **合计** | **376** | 247 | 阶段 2 退出时帧内必须为 0（加载期用例保留但需经创建服务） |

`Samples`（7 个样例 + 编辑器）：

| 类别 | 帧内 | 加载期 | 说明 |
|---|---|---|---|
| `SWAPCHAIN` | **18** | 0 | 与 C2 一致：`AcquireNextImage` / `Present` 全在样例主循环，T2.2 整体迁入渲染线程 |
| `WAIT` | **15** | 1 | 样例里的等待型提交，随 T2.4 的循环改造一起收口 |
| `CREATE` | **66** | 2 | 样例侧资源创建（多为加载期语义，但因函数名未命中白名单而记帧内） |
| `MAP` | **6** | 0 | — |
| `UPLOAD_DESC` | **22** | 0 | — |
| **合计** | **127** | 3 | 附录 B 的 **B2**：阶段 2 后样例侧这些调用应为 0 |

### 13.4 代表点与优先级（帧内命中按文件聚合）

| 类别 | 最集中的文件（帧内命中数） | 已核对的具体点 |
|---|---|---|
| `CREATE` | `Lumen/LumenScene_SurfaceCache.cpp×32`、`Nanite/NaniteRaster.cpp×26`、`Lumen/LumenSDF.cpp×18`、`Pipeline/ParticleRenderer.cpp×17`、`Pipeline/RTPass.cpp×12`、`Pipeline/LightingPass.cpp×11` | `RenderGraph.cpp:441/447` —— 帧图 `Execute` 内瞬态分配失败时**直接调用 `device->CreateTexture/CreateBuffer`**；`AA_FXAA.cpp:114`、`AA_SMAA.cpp:285` 等 PSO 辅助函数 |
| `MAP` | `Pipeline/ParticleRenderer.cpp×20`、`Lumen/LumenScene_SurfaceCache.cpp×18`、`Nanite/NaniteRenderer.cpp×15`、`Nanite/NaniteRaster.cpp×13`、`Nanite/NaniteCull.cpp×12`、`Lumen/LumenSDF.cpp×9` | `SceneRenderer.cpp:102`（对象数据直写）、`AA_TAA.cpp:198`（TAA 常量直写）、`DDGITracePass.cpp:152`/`GI_RSM.cpp:201`（GI 常量直写）、`NaniteRenderer.cpp:736` 起（GPU 计数读回） |
| `UPLOAD_DESC` | `Pipeline/RTPass.cpp×6`、`Lumen/LumenScene_SurfaceCache.cpp×3`、`Nanite/NaniteStream.cpp×3`、`Pipeline/LightingPass.cpp×3` | `LumenSDF.cpp:2249`、`NaniteRaster.cpp:145`（dummy VB 零填充） |
| `WAIT` | `Lumen/LumenScene_Irradiance.cpp:166`、`Nanite/NaniteStream.cpp:388` | 两处都在帧内路径上 ⇒ 阶段 2 起必须改造（铁律 3） |

**结论**：阶段 2 的主要工作量集中在 **Lumen 表面缓存 / Nanite（光栅、剔除、流式）/ 粒子 / RT**，以及
**帧图 `Execute` 的兜底创建路径**；这三块正好也是《Lumen设计与实现》里 CPU 录制耗时的大头，
因此 T2.3 与 T3.2 应当合起来排期，避免"先搬到渲染线程、再为并行录制改一遍"。

### 13.5 作为闸门

阶段退出时用 `--gate`：帧内命中非 0 即失败。各阶段期望：

| 阶段 | 期望 |
|---|---|
| 阶段 0（本阶段） | 只登记、不设闸门（本附录即产出） |
| 阶段 1 | `Engine/Render` 渲染期不再出现 `he::World&` / `SceneGraph&`（附录 B 的 B1，另行纳入脚本） |
| 阶段 2 | `--root Engine/Render --gate` 与 `--root Samples --gate` 均通过（帧内为 0） |
| 阶段 3/4 | 保持为 0，且 `WAIT` 帧内不得回升（铁律 3） |

---

---

## 14. 附录 E：mesh 注册表设计草案（T1.2c / T1.4 骨骼消费侧的前置）

> **为什么单独一节**：T1.2c（材质参数快照化）与骨骼消费侧都卡在同一件事上 —— 渲染侧目前通过
> **`MeshComponent*` 指针**拿到顶点/索引缓冲与材质数据（`SceneRenderer::Prepare` 的 `DrawItem`、
> `ForwardPipeline` 的骨骼上传），而**快照不允许带指针**（§4.2 的硬约束）。本节把这块的设计一次定清，
> 后续按它实施即可；实施前**不需要**再改口径。

### 14.1 目标与边界

- **目标**：让"快照里的 `meshIndex`"成为渲染侧取网格资源的唯一入口 ⇒ 删除渲染期对
  `MeshComponent*` 的依赖（这正是附录 B1 里 82 处的**主要来源**）。
- **不做**：不改顶点布局、不改绘制路径的绑定顺序、不改实例化/蒙皮的数据来源（它们已经索引化或另有入口）。

### 14.2 API 草案

```cpp
// Engine/Render/Threading/MeshRegistry.h —— 渲染侧网格注册表（阶段 1）
namespace he::render {

/// 一条网格记录（只存**渲染侧可达**的值；不持有组件所有权）
struct MeshRegistryEntry {
    rhi::IRHIBuffer* vertexBuffer = nullptr;   // 顶点缓冲（所有权仍在组件/资产，注册表只借指针）
    rhi::IRHIBuffer* indexBuffer  = nullptr;
    u32  indexCount = 0;                       // 索引数（间接绘制参数的兜底来源）
    u32  materialID = 0;                       // bindless 纹理基索引（与组件同源）
    bool instanced  = false;                   // 实例化网格：顶点由实例路径提供
};

class MeshRegistry {
public:
    /// 注册或**更新**（同一 key 重复注册 = 更新，用于组件重建缓冲的场景）；返回从 1 起的 meshIndex
    u32  Register(const void* key, const MeshRegistryEntry& entry);
    /// 注销：索引回到空闲表，且**旧 meshIndex 从此解析为 nullptr**（悬挂引用立刻可见，而不是指错资源）
    void Unregister(const void* key);
    [[nodiscard]] const MeshRegistryEntry* Find(u32 meshIndex) const;   // 越界/已注销 ⇒ nullptr
    [[nodiscard]] u32 Count() const;
    void Clear();
};

} // namespace he::render
```

### 14.3 生命周期规则（必须遵守，否则又是一类悬挂）

1. **谁注册**：持有组件/资产的一侧（加载期或组件创建时）；`key` 用组件地址，注册表**只借不拥有**缓冲。
2. **禁止帧内注销**：注销要在"确认渲染侧不再引用该索引"之后（沿用 `FrameRetireQueue` 的 N 帧延迟思路）；
   帧内注销会让当帧已录制的绘制指到空记录 ⇒ 与"快照按帧轮换"同一类问题。
3. **索引从 1 起**，0 保留为"未注册"哨兵（与 `RHIBufferHandle` 的约定一致）。
4. 与 **T0.7 句柄化**的关系：注册表内部将来换成 `RHIBufferHandle`（而不是裸 `IRHIBuffer*`），
   这样缓冲释放后的悬挂引用由**代次**检出；本阶段的裸指针版本是过渡（已在注释里标明）。

### 14.4 两个首个消费者（实施顺序）

| 步骤 | 内容 | 判据 |
|---|---|---|
| **E-1** | **骨骼上传**（`ForwardPipeline` 里把 `sm.boneMatrices` 写进 `sm.boneBuffer` 的那段）改为：遍历快照的骨骼条目（`skinMatrixOffset/Count`），用 `meshIndex` 从注册表取骨骼缓冲 | 与旧实现**逐位一致**（旧实现逐行转写为参考实现 + `memcmp`） |
| **E-2** | **`SceneRenderer::Prepare`** 的材质填充：材质输入改为"收集侧跑 `FillObjectData` 并把结果放进 `SnapshotDrawItem::object`"（§9 T1.2c 已定），`DrawItem` 不再携带 `MeshComponent*`；**视锥剔除仍在渲染线程**（它只需要快照里的世界 AABB） | 同上；并核对 `06.GILab` 的 28 个转储目标 |
| **E-3** | **第 5 处口径漂移的裁决**：`SceneRenderer` 收集 `SplineMeshComponent` 而 `GPUScene`/`BuildObjects` 不收集 —— 二者必须取其一（建议**统一为收集**，并在提交里给出前后对比） | 提交里写明"修正"还是"改版"及其依据 |

**E-3 配方（已查清，照做即可；2026-09-24）**：

- 可复用的现成件（都在 `Engine/Render/Pipeline/Material.h`，全部 `inline`）：
  `PBRMaterial`（:77）、`GetDefaultMaterial()`（:109）、`FillObjectData(GPUObjectData&, const PBRMaterial&)`（:129）。
- 现在要抽出来的映射在 `Engine/Render/SceneRenderer.cpp:110-125`（组件字段 → `PBRMaterial` 的
  10 项：`baseColorFactor / emissiveFactor / metallicFactor / roughnessFactor / aoFactor /
  alphaCutoff / alphaMode / doubleSided / unlit / 5 条纹理路径`）。
- **落点**：把该映射做成 `SceneSnapshotBuilder::MakePBRMaterial(const he::MeshComponent&)`
  （`static`，实现在 builder 的 .cpp 里；`SceneRenderer` 改为调用它 ⇒ 只有一份口径）。
  `CollectObjectItem` 在填完 `materialID` 后调用它并 `FillObjectData(item.object, mat)` ——
  **在收集侧把材质算完**（纯计算），快照因此不必携带纹理路径字符串。
- `SceneRenderer::Prepare` 改为消费快照（`entry` 不再需要 `MeshComponent*`）：
  剔除仍用快照的世界 AABB 在渲染线程做；`DrawItem` 里的网格引用换成 `meshIndex`
  （顶点/索引缓冲从 `MeshRegistry::Find` 取，E-1/E-2 已就绪）。
- 判据：① `FillObjectData` 的输出与旧路径**逐位**比较（把 `SceneRenderer.cpp:110-125` 的映射
  逐行转写为参考实现 + `memcmp`）；② `06.GILab` 的 28 个转储目标粗筛（噪声底噪 ≈4.5k 像素）。

**注册所有权（E-2 开工前必须先定 —— 已定，避免下一轮再决策）**：注册表由**创建/替换网格缓冲的那一侧**
填充，即 `MeshComponent::SetMeshData` 的调用点（资产加载器 / 样例的加载期），**不在渲染帧内**注册；
组件销毁时在同一处注销（且必须帧外，见 §14.3 第 2 条）。渲染侧只做 `Find`。
这样"谁创建谁注册"与缓冲所有权一致，也保证注册表在帧内**只读**（与快照同一条纪律）。

**E-2 的具体步骤（建议按此顺序，每步单独提交 + 逐位判据）**：1. 给 `ForwardPipeline` 加 `MeshRegistry m_MeshRegistry;`（成员）与最小注册点：**加载期**遍历一次
   `SkeletalMeshComponent`，用组件地址作 key 注册其顶点/索引缓冲与 `indexCount`；
2. 骨骼上传改为：遍历快照里 `skinMatrixCount > 0` 的条目 → `Find(item.meshIndex)` 取缓冲 →
   写入 `skinMatrices[offset, offset+count)`；旧实现（遍历组件 + `sm.boneBuffer`）逐行转写为参考实现
   做 `memcmp` 逐位比较；
3. 完成 E-2 后复测 `--world-deps`（预期 `ForwardPipeline.cpp` 的 7 处与 `.h` 的 12 处各降若干），
   并把 `WORLD_DEP_BASELINE` 手动下调到新值。

**E-2 代码骨架（真实字段名/调用，可直接照抄；`meshIndex` 字段已于 2026-09-24 落在 `MeshComponent` 上）**：

```cpp
// ① 注册（加载期；只做一次 —— 用 bool m_MeshRegistryReady 守；必须在首次构建快照之前）
//    ForwardPipeline.cpp / DeferredPipeline.cpp 同款
if (!m_MeshRegistryReady) {
    world.ForEach<he::SkeletalMeshComponent>([&](he::Entity, he::SkeletalMeshComponent& sm) {
        if (sm.GetIndexCount() == 0u) return;                 // 与收集口径一致：无索引不登记
        MeshRegistryEntry e;
        e.vertexBuffer = sm.GetVertexBuffer().get();          // 只借指针，所有权仍在组件
        e.indexBuffer  = sm.GetIndexBuffer().get();
        e.indexCount   = sm.GetIndexCount();
        e.materialID   = sm.materialID;
        e.instanced    = true;                                // 骨骼网格：顶点由蒙皮路径提供
        sm.meshIndex   = m_MeshRegistry.Register(&sm, e);      // 回填：快照靠它索引
    });
    m_MeshRegistryReady = true;
}

// ② 消费（骨骼上传：遍历快照条目而不是组件）
SceneSnapshotBuilder::BuildObjects(world, sg, camera, {}, nullptr, m_Snapshot);   // 若尚未构建
for (const SnapshotDrawItem& item : m_Snapshot.draws) {
    if (item.skinMatrixCount == 0u) continue;
    const MeshRegistryEntry* entry = m_MeshRegistry.Find(item.meshIndex);
    if (!entry) continue;                                     // 未注册/已注销 ⇒ 跳过（可见化，而不是指错资源）
    // …把 m_Snapshot.skinMatrices[item.skinMatrixOffset, +skinMatrixCount) 写进 entry 对应的骨骼缓冲
}
```

> 判据：把"遍历组件 + `sm.boneBuffer`"的旧实现**逐行转写**成参考实现，与上面①②的结果做 `memcmp`
> 逐位比较（全帧转储只作粗筛，原因见 §9 T1.3a 的噪声底噪记录）。

### 14.6 会话交接点（2026-09-24，`multi_thread` 分支，56 条提交未推送）

**当前绿灯状态（已复测）**：单测 **388 例 / 71730 断言全通过**；两项闸门都在基线
（B1 渲染期 **82** / 加载期 15；资源持有者 **277** / 76 文件）；`acceptance_sweep.ps1 -OnlyNanite`
此前 **PASS**（指纹 `1C15AB72E688B530` / `750CC247BF8B9C3D` 未变）。

**已就绪、可直接开工的下一步（E-2②）**：所有前置都已落地 ——
`MeshComponent::meshIndex`（字段）、`SceneSnapshotBuilder::CollectObjectItem` 的透传 + 断言、
`ForwardPipeline` 的 `MeshRegistry` 成员与首帧注册点、附录 E §14.4 的代码骨架与判据。
**唯一待做**：把骨骼上传从"遍历组件 + `sm.boneBuffer`"换成"遍历 `m_Snapshot.draws` 的
`skinMatrixCount > 0` 条目 + `m_MeshRegistry.Find(item.meshIndex)`"，并配一份逐行转写的参考实现
做 `memcmp` 逐位比较；完成后复测 `--world-deps` 并手动下调 `WORLD_DEP_BASELINE`。

**开工前建议的三条命令**（顺序固定，避免踩本会话踩过的坑）：
```powershell
# 1) 确认没有并发构建（否则会出现"假挂死"）
Get-Process cl,MSBuild -ErrorAction SilentlyContinue
# 2) 后台构建（前台会被 600s 上限截断并留下孤儿 cl，让后续构建看起来卡死）
cmd /c "cmake --build build --config Release --target HugEngineTests 06.GILab 03.Sponza-Forward > build\verify\b.log 2>&1"
# 3) 两项闸门 + 单测
python Tools\check_threading.py --world-deps --handles --gate
```

### 14.5 判据与闸门
- 注册表本体：单测覆盖"注册/更新/注销/复用/越界与已注销返回 nullptr"（与 `TestRHIHandles.cpp` 同款）。
- 两个消费者：沿用本方案统一判据 —— **旧实现逐行转写为参考实现 + 逐位比较**（全帧转储只做粗筛）。
- **B1 计数必须下降**：`Tools/check_threading.py --world-deps` 每完成一步就复测并把基线手动下调
  （当前 82）。
  > **口径修正（2026-09-24，E-2② 实测）**：E-1/E-2 消除的是**组件指针依赖**（`MeshComponent*`），
  > 而 B1 统计的是**签名里的 `World&` / `SceneGraph&`** —— 两者不是同一个度量。实测 E-2② 完成后
  > B1 仍为 82（骨骼循环本身仍在 `Render(world, …)` 之内）。**B1 真正开始下降是在 T1.5**
  > （把管线帧入口从"收 `world/sg`"改成"收快照"）。本附录的 E-1/E-2 因此按"组件依赖是否消失"
  > 衡量（代码检查 + 逐位判据），不按 B1 计数衡量。

---

> **文档版本**：v1.3（2026-09-24）
> **性质**：实施计划（**已开工**）。开工后每完成一个任务，回到 §9 勾选并在 §6 记录实测数字。
> **v1.1 变更**：新增 §12 附录 C「升级到 UE 三线程模型的增量路径」；阶段 0 增加预埋任务 **T0.6（RHI 命令流契约）** 与 **T0.7（资源句柄化）**，二者是 §12 所列升级路径的前置条件。
> **v1.2 变更**：T0.5 的开关改为**三态** `RenderThreadingMode`（单线程 / 游戏+渲染 / 游戏+渲染+RHI，见 §5 与 §7 的口径说明）；新增 §13 附录 D「阶段 0 T0.2 帧内同步 RHI 调用清单」与配套脚本 `Tools/check_threading.py`（含 `--gate` 闸门模式）。
> **v1.3 变更**：新增 §14 附录 E「mesh 注册表设计草案」（T1.2c 与 T1.4 骨骼消费侧的前置：API、生命周期规则、两个首个消费者、第 5 处口径漂移的裁决与判据）；§6 增补"阶段 0/1 累计实测"与端到端绿灯复核记录。
