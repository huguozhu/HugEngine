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

## 7. 与 D3D11 / OpenGL 的关系（后续）

模式 B 跑通后，其它后端可以**复用同一套命令流语义**，但不必照搬 Vulkan 的实现：

| 后端 | 建议实现 |
|---|---|
| D3D12 | 不需要命令流：直接用 command list（与 Vulkan 模式 A 同构），只在 `DeviceCaps` 上声明支持多线程录制 |
| D3D11 | 后端内部用 **deferred context** 实现 `IRHICommandList`；若某些命令（Map、query）受限，则在后端内部收集成命令记录再在 immediate context 执行 —— **等价于把模式 B 藏在 D3D11 后端里** |
| OpenGL | 无法多线程录制 ⇒ 只能跑模式 0（单线程）或在后端内收集后单线程执行 |

**结论**：`IRHICommandList` 这一层**已经就是命令流抽象**；模式 B 是"把它真正用作跨线程交接单位"的落地形式。

---

## 8. 明确不做的部分

| 项 | 原因 |
|---|---|
| 默认启用模式 B | 纯 Vulkan 下它只增加一层编码/翻译，无运行时收益 |
| 在渲染层分叉两套代码 | 维护成本翻倍，且违反"接口唯一"的设计 |
| 把呈现（`Present`）也纳入命令流 | 呈现涉及交换链线程归属，等模式 B 的提交路径稳定后再评估 |
