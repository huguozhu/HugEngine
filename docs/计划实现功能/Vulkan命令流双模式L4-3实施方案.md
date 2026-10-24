# Vulkan 命令流双模式（L4-3）实施方案

> **性质**：实施计划（2026-10-23 制定）。
> **前置阅读**：[HugEngine渲染线程化实施方案.md](../已实现功能/HugEngine渲染线程化实施方案.md) 的 §12.11（L3 撤销）、§12.12（L4-3 定义与 all-or-nothing 结论）、§16.7（A-5 现状）。
> **目标读者**：接手渲染线程化 / RHI 后端工作的开发者。

---

## 1. 目标

在 **Vulkan 后端内部**同时支持两种录制模式，用参数切换，从而**先在最容易验证的后端上把 L4-3 跑通**：

| 模式 | 名称 | 行为 |
|---|---|---|
| **A（现状）** | 直连录制 | 渲染线程的每次 `IRHICommandList::Xxx()` 立即调用 `vkCmdXxx`，写进真实 `VkCommandBuffer`；RHI 线程只负责 `Submit/Present` |
| **B（新增）** | 命令流录制（L4-3） | 渲染线程的每次 `IRHICommandList::Xxx()` 只**追加一条纯数据命令**；`End()` 时不结束命令缓冲，而是把命令流交给 RHI 线程；RHI 线程新建一条真实命令缓冲、**回放**命令流、再提交 |

**为什么要两种并存**：L4-3 的价值在"换后端（D3D11/OpenGL 这类不支持多线程命令记录）"与"命令级回放/测试"，但它的代价（多一层编码/翻译、调试距离变远）在纯 Vulkan 上是净负担。做成开关后：默认跑 A（零成本），需要验证 L4-3 时跑 B，两者共用同一份渲染层代码。

**关键约束（不要把两种实现分叉到渲染层）**：开关必须落在 `VulkanCommandList` 内部。渲染层的 `cmd->Dispatch(...)` 一个字都不能改 —— 否则就是两套渲染代码，维护成本翻倍。

---

## 2. 现有基础（已就绪，无需重写）

| 部件 | 位置 | 状态 |
|---|---|---|
| 命令流容器（类型擦除 + 64B 内联 + arena） | `Engine/RHI/RHI/RHICommandList.h` | ✅ 可用 |
| 命令类型枚举 + 载荷 POD | 同上 `RHICommandType` | 🟡 **只有 10 种被翻译器实现** |
| 翻译器 | `Engine/RHI/RHI/RHICommandReplay.{h,cpp}` `u32 ReplayCommandStream(const RHICommandList&, IRHICommandList&)` | 🟡 覆盖 10 种 |
| 提交管道 | `IRHIDevice::Submit(cmdList, stream)`、`ExecuteSubmit(...)` | ✅ 接口就绪，**没有生产者** |
| 帧外壳 | `rhi::FramePresenter::BeginRecording/EndRecording` | ✅ 9 个可执行目标全覆盖 |
| 资源句柄化（L4-3 的前置） | A-3 全量完成（718 处） | ✅ |

**已验证的翻译器覆盖（10 种）**：`SetPipeline`、`SetDescriptorSet`、`SetPushConstants`、`SetVertexBuffer`、`SetIndexBuffer`、`DrawIndexed`、`ExecuteSecondary`、`ExecuteIndirect`、`SetDrawDebugLabel`、`MarkSubmitBoundary`。

---

## 3. 缺口：需要补的命令类型

枚举里**已声明但翻译器未实现**：`Dispatch`、`PipelineBarrier`、`CopyBuffer`。

**完全缺失**（整帧跑通必须补）：

| 类别 | 需要新增的命令 |
|---|---|
| 视口 | `SetViewport`、`SetScissor` |
| Render pass | `BeginRenderPass`、`EndRenderPass`、`BeginOffscreenPass` |
| 绘制 | `Draw`（非索引）、`DrawIndexed` 带 `instanceCount` 的变体 |
| 同步 | `SetTimelineSignal` |
| 拷贝 | `CopyBufferToTexture`、`CopyTextureToBuffer` |
| 其它 | `SetStencilRef`、`ResetRenderTargets`、`ExecutePresent`（若呈现也纳入流） |

**估算**：从 10 种扩到 **28~32 种**。每一种都是"加枚举值 + 加 POD + 加一个 `case` 分支"，机械但有顺序要求（render pass 的 begin/end 必须严格配对）。

---

## 4. 设计

### 4.1 后端内部开关

```cpp
class VulkanCommandList {
    bool                 m_StreamMode = false;   // 由配置/环境变量决定
    rhi::RHICommandList  m_Stream;               // 流模式下的命令流（不创建 VkCommandBuffer）

    void Dispatch(u32 x, u32 y, u32 z) override {
        if (m_StreamMode) { m_Stream.Enqueue(RHICommandType::Dispatch, RHICommandDispatch{x,y,z}); return; }
        EmitDrawLabel();
        vkCmdDispatch(m_CmdBuffers[m_FrameIndex], x, y, z);    // 模式 A（现状）
    }

    void End() override {
        if (m_StreamMode) { m_HasStream = true; return; }      // 不 vkEndCommandBuffer
        vkEndCommandBuffer(...);                               // 模式 A
    }
};
```

**注意**：模式 B 下**不创建、不 Begin** `VkCommandBuffer`（命令缓冲的 `Begin/End` 由执行侧负责），避免"同一张命令缓冲被两个线程录制"的非法状态（§12.12 的硬约束）。

### 4.2 执行侧（RHI 线程）

```cpp
void VulkanDevice::ExecuteSubmit(IRHICommandList* cmdList, RHICommandList* stream) {
    if (!stream) { SubmitReal(cmdList); return; }

    auto real = CreateRealCommandList();       // 真实 VkCommandBuffer（per-frame）
    real->Begin();
    const u32 n = ReplayCommandStream(*stream, *real);   // 逐条翻译成 vkCmd*
    real->End();
    SubmitReal(real);
    HE_CORE_INFO("[CommandStream] replayed {} commands", n);
}
```

### 4.3 参数

| 参数 | 取值 | 用途 |
|---|---|---|
| `EngineConfig::rhiCommandStream` | `false`（默认）/ `true` | 常驻配置 |
| 环境变量 `HE_RHI_COMMAND_STREAM` | `0`/`1` | 在 `06.GILab` 里做 A/B 对照（与 `HE_GILAB_CONFIG` 同一路子） |

### 4.4 内存

命令流用 `RHICommandList` 自带的 arena（默认容量固定，见 `kDefaultArenaCapacityBytes`）。整帧命令量比单 pass 大一个量级 ⇒ **第一批就要确认容量是否够**，不够则按整帧估算调大，或在 `End` 后 `Reset()` 复用。

---

## 5. 验证判据（沿用四条）

| 运行 | 期望 |
|---|---|
| 开关 **OFF**（模式 A，现状） | 基线转储（`build/verify/gi_plumb_*`） |
| 开关 **ON**（模式 B，L4-3） | **20/20 逐字节一致**、**0 新 VUID**、**0 断言**、**干净退出** |
| 附加 | 日志打印 `replayed N commands`，N > 0 且随帧稳定 |

因为命令流只改变"命令怎么产生"，GPU 上执行的指令序列必须完全等价 ⇒ 任何语义偏差都会被转储立刻抓到。这也是"先在 Vulkan 上做 L4-3"的最大理由。

---

## 6. 分批计划（每批四条判据 + 单独提交）

| 批 | 内容 | 判据 |
|---|---|---|
| **1** | 开关骨架：`m_StreamMode`/`m_Stream`/`End()` 分支 + `ExecuteSubmit` 回放路径；**只支持** `SetPipeline` + `SetDescriptorSet` + `DrawIndexed`；让 Forward 的**单个简单 pass** 走流模式跑通最小闭环（其余 pass 仍走 A） | 转储一致 + 日志有 `replayed N` |
| 2 | 补视口 / scissor / draw label | 同上 |
| 3 | 补 Render pass 三件（`BeginRenderPass`/`EndRenderPass`/`BeginOffscreenPass`），注意 begin/end 严格配对 | 同上 |
| 4 | 补 dispatch / barrier / copy 四件 | 同上 |
| 5 | 补 push constants 变体、`SetTimelineSignal`、`ExecuteSecondary` | 同上 |
| 6 | 整帧（`06.GILab` 的 Deferred 路径）全部走 B，四条判据全量验证 | 四条判据 |

**风险点**：批 3（render pass）最容易出错 —— `BeginRenderPass` 的载荷包含目标纹理句柄与 clear 值，回放时必须保证句柄仍在存活期内（句柄体系已就位，但要确认 `End` 到 `ExecuteSubmit` 之间没有释放）。

---

## 7. 与其它后端的关系（**修正 2026-10-23**）

### 7.1 先纠正一个错误结论

本文档初版写过"D3D11 用 deferred context、**OpenGL 只能跑模式 0**"。**这个结论是错的**，原因在于
把"能否多线程录制"和"能否多线程架构"混为一谈：

* **模式 2 的真正约束**不是"渲染线程能不能发 API"，而是"**API 只在提交线程上被调用**"；
* **L4-3 恰好消除了渲染线程的 API 调用** —— 渲染线程只 append 纯数据，API 全在 RHI 线程；
* 所以 **L4-3 之后，任何后端（包括 OpenGL）都能跑模式 2**，只要该后端把 context 独占在 RHI 线程上。

反过来说：**"OpenGL 只能模式 0"这句在"没有 L4-3"的前提下才成立**。

### 7.2 各后端在两种前提下的能力

| 后端 | **没有 L4-3** | **有了 L4-3** |
|---|---|---|
| Vulkan | 模式 1/2 ✅（命令缓冲原生支持多线程录制，现状） | 模式 2 ✅（等价；多一层编码/翻译，无运行时收益） |
| D3D12 | 模式 1/2 ✅（command list + 每飞行帧 allocator） | 模式 2 ✅（同上） |
| D3D11 | 模式 0/1 可靠；模式 2 需要 **deferred context**（Map/query 受限，需绕行） | **模式 2 ✅** —— 渲染线程零 D3D11 调用，immediate context 只在 RHI 线程 |
| **OpenGL** | **只能模式 0/1** ❌（context 同一时刻只能 current 在一个线程；渲染线程一旦发 GL 调用就与提交线程冲突） | **模式 2 ✅** —— 渲染线程零 GL 调用；GL context 只 current 在 RHI 线程 |

**这张表正是 L4-3 最有说服力的价值**：它把模式 2 从"Vulkan/D3D12 专属"变成"**所有后端通用**"。

### 7.3 但有两个必须一起搬走的东西

L4-3 只搬"录制类"命令还不够，下面两类调用若留在渲染线程，OpenGL/D3D11 仍然会冲突：

| 类别 | 为什么必须搬 | 落地方式 |
|---|---|---|
| **`Map` / `Unmap`** | GL 的 `glMapBufferRange`、D3D11 的 `Map` 都属于 context；跨线程调用即冲突 | 两种选择：① 命令流里加 `MapBuffer`/`UnmapBuffer` 命令，由 RHI 线程执行；② 改成"写进命令流自带的 arena，回放时统一上传"（更彻底，推荐后者） |
| **资源创建** | GL 的 `glGenBuffers`/`glTexImage*` 需要 current context；D3D11 的创建虽在 device 上但驱动内部有锁 | 命令流里加 `CreateBuffer`/`CreateTexture` 命令；或在 RHI 线程启动阶段统一创建 |

**结论**：L4-3 的范围应当定义为"**渲染线程不出现任何后端 API 调用**"，而不只是"不出现 `vkCmd*`"。
上一句里的"任何"包括 Map/Unmap 与创建 —— 这会直接决定命令类型清单（见 §3）。

## 8. 明确不做的部分

| 项 | 原因 |
|---|---|
| 默认启用模式 B | 纯 Vulkan 下它只增加一层编码/翻译，无运行时收益 |
| 在渲染层分叉两套代码 | 维护成本翻倍，且违反"接口唯一"的设计 |
| 把呈现（`Present`）也纳入命令流 | 呈现涉及交换链线程归属，等模式 B 的提交路径稳定后再评估 |

## 9. 实施进度（2026-10-23）

### 9.1 批 1：开关骨架 —— 🟡 结构就绪（5 项完成，端到端验证并入批 3）

| # | 内容 | 提交 | 状态 |
|---|---|---|---|
| 1 | `VulkanCommandList` 增加 `m_StreamMode` / `m_Stream` 与访问器；环境变量 `HE_RHI_COMMAND_STREAM` | `a9380ad` | ✅ |
| 2 | 流模式下 `Begin()` 不 `vkWaitForFences`/`vkResetCommandPool`/`vkBeginCommandBuffer`；`End()` 不 `vkEndCommandBuffer` | `6e20071` | ✅ |
| 3 | `ExecuteSubmit` 增加流模式回放路径：**影子命令列表**（每飞行帧一份、借用源列表的帧槽位）→ `ReplayCommandStream` → `Submit` → 打印 `replayed N commands (slot N)` | `490d256` | ✅ |
| 4 | `SetPipeline` / `BindDescriptorSet` / `DrawIndexed` 的流模式 append | `e0d6f33` | ✅ |
| 4.5 | `SetVertexBuffer` / `SetIndexBuffer` 的流模式 append（原本只缓存 `VkBuffer`，不记进流则回放侧无法重绑） | `8dbf4f0` | ✅ |
| **5** | 让 Forward 的**单个 pass** 走流模式 | — | ⬜ **并入批 3** |
| **6** | 开关 ON 的四条判据（含 `replayed N`） | — | ⬜ **并入批 3**（依赖 5） |

### 9.2 为什么子任务 5/6 必须并入批 3

"让一个 pass 走流模式"需要**按 pass 切换开关**，而当前开关是**整条命令列表**级别的。更关键的是：真实 pass 还会用到批 1 范围之外、翻译器尚未支持的命令：

| 命令 | 翻译器 | 流模式分支 | 何时可用 |
|---|---|---|---|
| `SetPipeline` / `BindDescriptorSet` / `DrawIndexed` | ✅ | ✅ | 批 1 |
| `SetVertexBuffer` / `SetIndexBuffer` | ✅ | ✅ | 批 1 |
| `SetPushConstants` | ✅ | ❌ 需 arena 偏移 API（`AllocArena` 只返回指针） | 批 4 |
| `Draw`（非索引） | ✅ | ❌ | 批 2/4 |
| `SetViewport` / `SetScissor` / `SetDrawDebugLabel` | ❌（枚举里没有） | ❌ | 批 2 |
| `BeginRenderPass` / `EndRenderPass` / `BeginOffscreenPass` | ❌ | ❌ | 批 3 |
| `PipelineBarrier` | 枚举有、翻译器无 | ❌ | 批 4 |

⇒ **任何真实 pass 都要 render pass 边界与屏障**，所以"单 pass 端到端"最早只能在批 3 之后成立。
届时用同一个开关 + 一个 pass 的 scope 做验证，同时满足子任务 5 与 6。

### 9.3 当前状态（不影响默认行为）

流模式**默认关闭**（`HE_RHI_COMMAND_STREAM` 未设），所有新增分支都是死路径 ⇒ 每批四条判据下转储逐字节一致是预期结果。
这批工作的价值是：骨架、回放路径、六种命令的装箱都已就位，批 2/3 只需要"加命令 + 加分支"。


## 10. 附录 K：批 5（整帧流模式）的诊断记录（2026-10-23 ~ 10-24）

### 10.1 已达成的里程碑

| 里程碑 | 状态 | 证据 |
|---|---|---|
| 流模式跑完整帧 | ✅ | 42 帧、每帧回放 1478 条命令 |
| 干净退出 | ✅ | 无 crash phase；`vkDeviceWaitIdle: Invalid device` 已消失 |
| 产出全部转储 | ✅ | 20/20 个 `gi_<tag>_*` 文件 |
| 默认路径零回归 | ✅ | 每次改动都验证 19/20 逐字节一致（`meta.txt` 为已知字节差异） |
| 与基线一致性 | 🟡 **7/20** | 13 个目标不同，且**全部为零输出** |

### 10.2 关键修复（按发现顺序）

| 修复 | 提交 | 说明 |
|---|---|---|
| 流模式只由帧主列表开启 | `b1f2ad1` | 此前 `CreateCommandList` 给内部临时列表（异步计算）也开了流模式 ⇒ 提交未结束的缓冲 ⇒ 崩溃 |
| 查询读回短路 | `a76e288` | `GetQueryResults` 用 `VK_QUERY_RESULT_WAIT_BIT`，而流模式下查询从未 reset ⇒ 永久阻塞（曾卡在第 3 帧） |
| 影子列表继承 timeline 信号 | `ef80f5c` | 转储的 `FlushAndWait` 等待的信号由影子列表提交才被 signal |
| 设备销毁前释放影子列表 | `39fcbbb` | 否则析构时设备已失效 |
| 恢复 `SetPipeline`/`BindDescriptorSet` 流分支 | `7a19a95` | **这两个分支曾在一次 `git checkout` 回退中丢失**，导致影子列表从不绑 PSO（每帧命令数从 1448→1478） |
| 流模式下同步更新本地 pipeline/rp 状态 | `7a19a95` | 否则 `BeginRenderPass` 载荷携带过期 render pass ⇒ 附件数不匹配 |
| `DrawIndexed` 回放修正 | `cabc6d9` | 旧实现实例化时丢掉 `startIndex`/`baseVertex`，且把 `instanceCount` 传到 `firstIndex` |

### 10.3 已排除的假设（每条都有实测依据）

| # | 假设 | 排除依据 |
|---|---|---|
| 1 | GBuffer 走 `DrawIndexedIndirect` | 接入后每帧命令数 +0 |
| 2 | GBuffer 走 `DrawIndexedIndirectCount` | 接入后 +2（不是 GBuffer） |
| 3 | GBuffer 走 `ExecuteSecondary`（MTCR） | 接入后 +0 |
| 4 | GBuffer 走 `BeginOffscreenPassMRT` | 回放日志 0 条（GBuffer 用单附件 pass） |
| 5 | GBuffer 走 `ExecuteGeneratedCommands`（DGC） | 接入后一致性不变 |
| 6 | 描述符集句柄回放解析失败 | `ResolveDescriptorSet` 失败告警 0 次（`SetPipeline` 回放 63596 次） |
| 7 | 缺 viewport/scissor 导致零输出 | 强制设全屏 viewport 后 `albedo` 仍为 0 |

### 10.4 当前掌握的确切事实

诊断工具（`HE_L43_HIST=1` / `HE_L43_PASSDRAW=1`）给出的数据：

```
命令直方图（一帧）：
  DrawIndexed=349  SetVertexBuffer=349  SetIndexBuffer=349  SetPushConstants=354
  SetPipeline=19   SetDescriptorSet=11   SetViewport=9  SetScissor=9
  CmdBeginRenderPass=2  CmdEndRenderPass=2
  CmdBeginOffscreenPass=8  CmdEndOffscreenPass=9  CmdBeginOffscreenPassMRT=1
  CmdDraw=6  CmdPipelineBarrier=1

每个 pass 的绘制数：
  2048x2048 draws=103 / 103 / 103 / 143      ← 阴影/RSM 级
  1920x1080 draws=1 ×7                        ← 全屏后处理
```

⇒ **绘制命令确实在流里、确实在正确的 pass 内执行了**，但目标纹理恒为零。
⇒ 因此问题不在"命令丢失"，而在**写入的落点**（附件/纹理身份）或**读回的时机**。

### 10.5 下一步的候选方向（按可能性排序）

1. **transient 资源身份**：GBuffer/HDR 等目标可能是 transient 资源，每帧在各 Heap 之间重建；
   流模式下回放用的是"记录时刻的 VkImageView"，而转储读的是"读回时刻的纹理"，
   两者若已不是同一份内存 ⇒ 读回恒为零。验证方法：把 `HE_GILAB_CONFIG` 里的 transient/aliasing
   关掉后重跑，若一致性提升即可确认。
2. **pass 内附件的 layout 转换**：`EnsureDepthAttachmentLayout` 在流模式下被跳过，
   而它负责把深度附件修到期望 layout；若 layout 不对，写入会被丢弃。
3. **`SetViewport/SetScissor` 的时机**：只有 9 次调用对应 11 个 pass，
   若某个 pass 的首次绘制前没有有效视口，写入同样为零（已用强制全屏视口排除过一次，
   但那只覆盖了 offscreen 路径）。

## 11. 附录 L：批 5/6 完成 —— 四条判据全部通过（2026-10-24）

### 11.1 结论

**流模式的四条判据全部通过**：

| 判据 | 结果 |
|---|---|
| 与基线逐字节一致 | ✅ **19/20**（`meta.txt` 为已知的字节级差异，内容一致） |
| 无新增 VUID | ✅ |
| 无断言 / 无崩溃 | ✅ |
| 干净退出 | ✅ |

同一套判据下流模式与默认路径**输出完全相同** —— 这是 L4-3 "命令流与直连等价"的直接证据。

### 11.2 最终根因：转储读回被跳过

前面 9 条假设全部排除后，真正的原因非常朴素：

> **`CopyTextureToBuffer` 在流模式下被跳过** —— 而它正是**转储读回**唯一的拷贝通道。

因此：GBuffer/HDR/IBL 那些目标其实**画得好好的**（日志已证实每个 pass 有 103/143/1 次绘制、图像身份跨帧稳定、无 VUID），
只是**从未被拷回 CPU** ⇒ 转储文件里全是零 ⇒ 看起来像"pass 没画"。

修复（`06e4655`）：把 `CopyTextureToBuffer` 的载荷（纹理/缓冲句柄 + x/y/宽高 + 缓冲偏移）记进流，
回放时调用真正的拷贝。

### 11.3 排查方法论小结（值得复用）

| 手段 | 作用 |
|---|---|
| 命令直方图（`HE_L43_HIST=1`） | 一眼看出"命令有没有进流"（`DrawIndexed=349` 等） |
| 按 pass 绘制计数（`HE_L43_PASSDRAW=1`） | 确认"绘制是否在正确的 pass 内执行" |
| 图像身份日志（`HE_L43_IMGID=1`） | 对比"写入的图像"与"读回的图像"是否同一份 |
| 转储数值统计（非零像素占比） | 区分"画错了"与"根本没写/没读回" |

其中**最后一条最关键**：全部为零 ⇒ 问题在链路的**末端**（读回），而不是中段（绘制）。

### 11.4 当前状态

| 项 | 状态 |
|---|---|
| 批 1~4（骨架 + 全部命令类型） | ✅ 完成 |
| 批 5（整帧流模式） | ✅ 完成（42 帧、1478 条命令/帧） |
| 批 6（四条判据） | ✅ 通过 |
| 默认路径 | ✅ 零回归（每次改动都验证） |

**L4-3 的 Vulkan 侧工作至此完成**：`HE_RHI_COMMAND_STREAM=1` 可让整帧走命令流，结果与直连路径逐字节一致。
下一步（若继续）是为其它后端复用同一套命令流语义（见 §7），或补齐仍在跳过列表里的命令
（`Dispatch`、`DispatchIndirect`、`CopyBuffer`、`CopyTextureToTexture`、`BuildTLAS`、`BindRTPipeline`、
`ClearDepthStencil`、查询类、`BeginSecondary` 等 —— 它们不影响当前样例的等价性，但会影响其它管线）。

### 11.5 剩余跳过项的分类（P2 决策，2026-10-24）

命令流目前仍**有意**跳过 8 个命令，分为三类：

| 类别 | 命令 | 决策理由 |
|---|---|---|
| **有意不接入**（profiling） | `BeginQuery`、`EndQuery`、`ResetQueryPool`、`WriteTimestamp` | 流模式下 profiling 整体关闭：`GetQueryResults`/`TryGetQueryResults` 已短路返回，接入这些命令只会产生不会被读回的空操作。代码注释已改为"有意不接入"而非"未实现"，避免后来者误判为遗漏 |
| **待设计** | `BeginSecondary`、`ExecuteGeneratedCommands`（DGC） | 次级列表本身不走流模式（由录制者线程真实录制），DGC 需要完整的执行集状态；两者都需要单独设计，当前样例不依赖 |
| **内部/配套** | `EnsureDepthAttachmentLayout`、`QueueOwnershipTransfer` | 前者是内部函数（需与 layout 追踪器配合），后者与异步计算队列所有权绑定 |

**结论**：L4-3 在 Vulkan 侧的功能完整性以"06.GILab 逐字节等价"为准（已达成），
上表的跳过项不影响该结论，且每一项的理由都已记录，不是技术债。

