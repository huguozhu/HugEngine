# HugEngine AI 一等公民架构设计（总纲）

> 本文回答"为什么"和"改哪里"：如何从根本上重新思考传统引擎的模块划分与数据流，把 AI 从"事后集成的功能插件"变成"一等公民"。
> 详细设计见两份分册：`HugEngine AI统一基座设计.md`（方案 1）、`HugEngine AIGC创作平台设计.md`（方案 4）。

---

## 一、核心判断

把 AI 视为一等公民，落到两个根本动作：

1. **AI 与渲染平级**：新增一个与 RHI 平级的「AI 运行时层」。RHI 之于渲染 = AI 运行时之于 AI —— 推理成为和绘制同等级的硬件抽象，而不是渲染或游戏性下面的一个子系统。
2. **反射即世界模型**：不另造"AI 专用场景格式"。用引擎已有的宏驱动反射 + ECS 作为 AI 读写的**唯一事实源**。

传统引擎把"渲染数据（mesh/light/camera）"和"游戏性数据（AI/行为/状态）"分成两套互不相干的存储，所以 AI 只能是"事后挂上去的系统"。而 HugEngine 的反射系统（`TypeRegistry` + `ClassInfo::factory` + `PropertyInfo`）已经让**任何组件都能按名内省、按名构造**——等于世界模型已经就位，缺的只是把它接到 AI 身上的那层运行时。

---

## 二、现状：AI 被放在两处"插件位"

| 位置 | 形态 | 问题 |
|---|---|---|
| 神经渲染（技术全景 Phase 5，`M73~M79`） | DLSS/FSR/XeSS、NRC、神经材质、RTXNS、LinAlg 被当作渲染管线里一串各自为政的加速插件 | 没有统一推理运行时，各 SDK 各自对接，`rhi::DeviceCaps` 里只是几个散落的布尔位 |
| 游戏性 AI（架构文档 L7 游戏逻辑层） | 一句 `Gameplay Systems (Physics, Audio, AI...)` | AI 与物理、音频并列成一个"系统"，正是"事后集成的功能插件" |

**代码层面：`Engine/` 下没有任何 AI/ML/推理模块**（`grep -r "Neural|Inference|Agent|LLM|AI" Engine/` 无结果）。AI 目前 100% 只存在于规划文档里，且被切成"渲染的 AI"和"游戏的 AI"两块互不相干的东西。

根因：**引擎里没有一个与 RHI 平级的推理运行时，也没有一个 AI 可读写的世界表示**。

---

## 三、两条路线

| | 方案 1：统一 AI 基座（底座） | 方案 4：AIGC 创作平台（应用层） |
|---|---|---|
| 定位 | 与 RHI 平级的推理硬件抽象 + 世界模型 + 智能体 | AI 原生内容创作工具 |
| 目录 | `Engine/AI/`（Runtime / WorldModel / Agent / Neural） | `Engine/AI/AIGC/` |
| 解决 | 推理/智能体/神经渲染都建立在一个基座上 | AI 生成内容走与人工编辑相同的生产管线 |
| 依赖 | 只依赖 RHI + 反射 + JobSystem | 依赖方案 1 的推理运行时（可先用远程后端解耦先行） |

**关系**：方案 4 建立在方案 1 之上，但二者可解耦开发 —— AIGC 先用远程 LLM 后端即可跑通，不必等本地 GPU 推理就绪。

---

## 四、数据流重构

**现状（单向）**：

```
Asset → AssetRegistry → Entity+Components → RenderSystem::OnUpdate → RenderGraph → Present
```

**目标（AI 与渲染共享 World，双向闭环）**：

```
                       ┌──────────────────────────────────────────────┐
                       │        WorldModel（反射驱动语义快照）           │
  Asset ──► ECS World  │  ┌────────┐ ┌────────┐ ┌────────┐            │
       ▲               │  │ 观察    │ │ 记忆    │ │ 目标    │ ← Agent 组件│
       │               │  └────────┘ └────────┘ └────────┘            │
       │               └─────────────┬────────────────▲──────────────┘
       │                             │                │
  AIGC(方案4)  ◄── 生成 ◄── Brain(LLM/RL) ── 动作 ──► he::Command(可撤销)
       │                             │                │
       └─────── IAIDevice 推理运行时 ◄┘                ▼
                       │                      World::AddComponent / SetProperty
              ┌────────┴─────────┐
              ▼                  ▼
      Neural(收编 Phase5)   RenderSystem → RenderGraph → Present
```

关键点：AI 读（`WorldModel::Snapshot`）与写（`Action → he::Command`）都与渲染走**同一条反射通道**，而不是在边上另开一个"AI 系统"。

---

## 五、分层变化：插入 L2.5 AI 运行时层

在现有 L0~L8 分层中，插入一个与 L2 RHI 平级的新层：

```
┌─────────────────────────────────────────────────────────────┐
│ L5 组件层   Agent/Memory/Goal 组件（与 MeshComponent 平级）     │
├─────────────────────────────────────────────────────────────┤
│ L4 渲染层   Neural（NRC/超分/神经材质）—— 作为 IRenderSubsystem   │
├─────────────────────────────────────────────────────────────┤
│ L2.5 AI 运行时层  ★ 新增 ★                                    │
│   IAIDevice ─ WorldModel ─ InferenceScheduler                │
│   ├ GPU Backend（CoopVec/linalg/RTXNS/DirectML）             │
│   ├ CPU Backend（ONNX Runtime / GGUF）                       │
│   └ Remote Backend（LLM：OpenAI/Anthropic/Ollama）            │
├─────────────────────────────────────────────────────────────┤
│ L2 RHI 层   Vulkan / D3D12 / Metal / WebGPU                 │
├─────────────────────────────────────────────────────────────┤
│ L1 反射层   TypeRegistry / ClassInfo / PropertyInfo          │
└─────────────────────────────────────────────────────────────┘
```

---

## 六、落地顺序与依赖

```
方案 1（基座）─► 是方案 4（AIGC）的引擎，但 AIGC 可用远程后端先行

① A1 基座：IAIDevice 门面 + CPU/Remote 后端 + 流式调度 + WorldModel + HE_ATTR_AI_*  （P1末~P2初）
② G1 最小闭环：AIGCProvider + PromptToScene + 生成命令 + 面板                      （P2末，可与 P3/P4 并行）
③ A2 智能体：Agent/Memory/Goal 组件 + IBrain + ToolUse + Action→Command + AI 面板   （P4）
④ A3 神经收编：GPUBackend + 神经渲染落地为 IRenderSubsystem                         （P5）
⑤ G2 资产生成：TextToTexture/Mesh/Material/Animation 接本地 GPU 后端                （P5）
⑥ G3 深度编辑：AI 驱动材质编辑/场景重排/批量修饰                                    （P6+）
```

> 关键调整：把 AI 运行时 + 世界模型**提前到 Phase 1 末 / Phase 2 初**，否则 Phase 5 的神经渲染仍是"事后插件"。

---

## 七、对现有代码的整体影响面

| 类别 | 内容 |
|---|---|
| 新增 | `Engine/AI/**`（含 `Engine/AI/AIGC/**`） |
| 修改 | `Engine/Core/Core/Engine.h`（持有 `IAIDevice`）、`Engine/Core/Threading/JobSystem.h`（流式/优先级车道）、`Engine/Reflect/Reflect/Attribute.h` + `ReflectionMacros.h`（`HE_ATTR_AI_*`）、`Engine/Scene/Scene/SceneReflect.cpp`（注册 AI 组件）、`Engine/Render/**`（神经渲染改为走 `IAIDevice`）、`Engine/Editor/**`（AI 面板） |
| 复用（零改动） | `he::reflect::TypeRegistry`/`ClassInfo`/`PropertyInfo`、`he::World`（ECS）、`he::Command`/`he::CommandHistory`/`he::PropertyChangeCommand`（`Engine/Editor/Editor/Command.h`）、`he::asset::LoadGLTF`（作为生成器的返回形状对齐基准）、`he::render::IRenderSubsystem`（神经渲染子系统模式） |

**结论**：AI 一等公民的改造重心不是"新造多少代码"，而是在正确位置插入一层抽象，把已存在的反射 / ECS / 命令 / RHI 能力串成一条 AI 可用的数据通路。

---

## 关联文档

- 方案 1 详细设计与实现原理：`HugEngine AI统一基座设计.md`
- 方案 4 详细设计与实现原理：`HugEngine AIGC创作平台设计.md`
- 过程稿（superpowers spec）：`docs/superpowers/specs/2026-08-26-ai-foundation-design.md`、`docs/superpowers/specs/2026-08-26-aigc-authoring-design.md`
