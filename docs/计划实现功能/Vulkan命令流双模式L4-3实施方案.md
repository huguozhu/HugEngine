# Vulkan 命令流双模式（L4-3）实施方案

> **性质**：实施计划（2026-10-23 制定）。
> **前置阅读**：[HugEngine渲染线程化实施方案.md](HugEngine渲染线程化实施方案.md) 的 §12.11（L3 撤销）、§12.12（L4-3 定义与 all-or-nothing 结论）、§16.7（A-5 现状）。
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
