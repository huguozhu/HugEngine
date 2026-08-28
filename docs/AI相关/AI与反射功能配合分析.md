# HugEngine 中 AI 与反射系统的配合分析

> 基于实际代码与设计文档（`docs/AI相关/1~7` 系列）的分析，只读未修改任何源码。

## 一、核心设计原则："反射即世界模型"

HugEngine 的 AI 架构（`docs/AI相关/1.HugEngine AI一等公民架构设计.md`）有一个关键设计决策：

> **不另造"AI 专用场景格式"**。`World`（ECS）+ `TypeRegistry`（反射注册表）本身就是世界模型，AI 读（观察）与写（动作）都走**同一条反射通道**。

这意味着 AI 层不感知渲染数据结构，它只通过反射元数据来内省世界——这正是反射系统对 AI 的全部价值所在。工程实际落地了两套互补的实现（`Engine/AI/` 模块，已接入构建的 `HugEngineAI` 库）：

1. **LLM 场景生成 MVP**（`SceneBuilder` + `PromptToScene` + `DeepSeekClient`）——"写"方向，prompt → 场景 JSON → 真实 ECS 世界；
2. **反射驱动世界模型**（`WorldModel` + `HE_ATTR_AI_*` 注解）——"读"方向，世界 → LLM 可读语义快照。

## 二、反射侧：AI 注解如何挂进属性系统

反射宏体系（`Engine/Reflect/Reflect/ReflectionMacros.h`）在原有的 `HE_REGISTER_PROPERTY` 基础上扩展了 4 个 AI 注解宏：

```cpp
HE_ATTR_AI_VISIBLE()          // 该属性进入世界模型快照（WorldModel 只导出带此注解的属性）
HE_ATTR_AI_DESCRIPTION(text)  // 自然语言说明（注入 LLM 帮助理解）
HE_ATTR_AI_WRITABLE()         // 允许 AI 写入（否则只读）
HE_ATTR_AI_TOOL(name)         // 该方法暴露为 AI 可调用的工具
```

这些宏实现为往 `PropertyInfo::attributes` 追加键值对（`("AiVisible","1")` 等），键名统一收敛在 `Engine/Reflect/Reflect/Attribute.h` 的 `AttrKey::Ai*` 常量（`AiVisible/AiDescription/AiWritable/AiTool`），与 `Category/Range/Tooltip` 等编辑器注解共用同一套属性存储——**AI 注解是反射属性系统的"一等公民"，而非另开机制**。

**已实际落地的注解**（`Engine/Scene/Scene/SceneReflect.cpp`）：
- `TransformComponent`：`position`（"世界空间位置，单位米"）、`rotation`、`scale`，均 `AI_VISIBLE + AI_WRITABLE + AI_DESCRIPTION`；
- `DirectionalLight`：`direction/color/intensity/castShadow`，均带 AI 注解；
- `PointLight`：`color/intensity/range`，均带 AI 注解。

## 三、AI 侧（读方向）：WorldModel 消费反射

`Engine/AI/WorldModel/WorldModel.cpp` 是反射驱动的核心，两个函数展示了完整的"反射 → AI"数据通路：

**① `Snapshot(World&, filter)` — 世界 → LLM 语义快照**
```
遍历 World 所有实体
  └─ ForEachComponent → comp->GetClass()  ← 每个组件对象都持有反射 ClassInfo
       ├─ 按 AI_VISIBLE 注解过滤（无可见属性的组件整组跳过）
       └─ 遍历 cls->properties：
            ├─ PropertyInfo::offset  → 直接从组件对象内存读值
            ├─ PropertyInfo::typeName → 字符串分派序列化（float/float3/quat/String...）
            └─ 输出 JSON：{entity:{id, components:[{type, fields}]}}
```

**② `TypeSchema()` — 反射注册表 → LLM 词汇表**
```
遍历 TypeRegistry 全部注册类
  └─ 只输出含 AI_VISIBLE 属性的组件类型
       ├─ 字段名 + 类型（来自 PropertyInfo::name/typeName）
       ├─ writable（来自 AiWritable 注解）
       └─ description（来自 AiDescription 注解）
```

**配套过滤器**（`Observation.h` 的 `ObservationFilter`）：`targetEntity` / `componentTypes` / 空间半径，控制快照范围，避免把上千实体的整帧世界塞进 LLM prompt（token 预算控制）。

## 四、AI 侧（写方向）：LLM 端到端场景生成

`Engine/AI/PromptToScene.cpp` 把上面两层串起来，构成完整闭环：

```
用户 prompt（如"一个黄昏下的中世纪村庄"）
  │
  ▼
PromptToScene: system prompt = 角色设定 + 输出格式 + BuildTypeSchema(词汇表) + 规则
  │                                 ▲
  │                  （"只使用上述组件类型和字段，不要发明新类型"）
  ▼
DeepSeekClient（WinHTTP → api.deepseek.com，OpenAI 兼容 chat/completions，
               json_object 强制合法 JSON，API Key 来自环境变量 DEEPSEEK_API_KEY）
  │
  ▼
解析 choices[0].message.content → 场景 JSON
  │
  ▼
SceneBuilder::BuildScene：JSON → 真实 Entity/Component 树
  └─ 每个实体：CreateEntity + AddComponent<TransformComponent> + 按 type 创建组件
     （Cube/Sphere/DirectionalLight/PointLight/PhysicalSky，含材质参数）
     └─ 全程容错：字段类型检查、缺省降级、未知组件类型跳过不崩溃
  │
  ▼
05.LLMScene.exe 用 ForwardPipeline 直接渲染出 LLM 生成的场景
```

## 五、两条 schema 路径：现状与演进目标

| 路径 | 实现 | 现状 |
|------|------|------|
| 硬编码 MVP | `TypeSchema.cpp`（`BuildTypeSchema`）+ `SceneBuilder` 硬编码组件映射 | ✅ 已落地，注释明确"不碰反射属性注册" |
| **反射驱动** | `WorldModel::TypeSchema()` / `Snapshot()` | ✅ 已实现且组件已打注解，`Tests/TestWorldModel.cpp` 验证 |
| 泛型化（目标） | `SceneBuilder` 迁移到 `ClassInfo::factory` 按名构造组件 + `PropertyInfo` 写字段 | ⬜ 设计文档明确为后续演进（见 `7.HugEngine LLM创建场景实现计划.md` 结尾） |

即：**当前 MVP 用硬编码映射保证"能跑"，反射通路（WorldModel + 注解）已就位并测试通过，两者未来会在 SceneBuilder 泛型化时合流**——届时"AI 创建组件"与"编辑器添加组件"走的是完全相同的反射路径。

## 六、推理运行时：与 RHI 同构（A1 基座）

`Engine/AI/Runtime/` 进一步把 AI 提升为与渲染平级的抽象：
- `IAIDevice` 是 `IRHIDevice` 的镜像（`AIDevice.h` 注释原话："抽象『模型执行』而非『图元绘制』"），按模型格式 + 能力选择后端（GPU/CPU/Remote，当前仅 RemoteBackend 可用）；
- **零拷贝互操作**：`WrapRHITexture` / `WrapRHIBuffer` / `ExportBuffer` 让神经渲染权重以 bindless 缓冲直接接入推理，不复制 GPU↔CPU；
- `AIModule` 单例（进程级入口）+ `InferenceScheduler`（流式 token 按序投递主线程）。

设计文档中的目标形态（`docs/AI相关/3.HugEngine AI统一基座设计规格.md`）：AI 写世界封装为 `he::Command` 走 `CommandHistory`（**可撤销/可审查**）；AIGC 平台生成的内容与人工编辑走完全相同的 Entity/Component/Asset/Command/Archive 管线。

## 七、配合关系总览（数据流闭环）

```
                反射系统（单一事实源）
   ┌──────────────────────────────────────────────┐
   │  HE_REGISTER_PROPERTY + HE_ATTR_AI_*          │
   │  → TypeRegistry → ClassInfo / PropertyInfo    │
   └───────┬───────────────────────────┬──────────┘
           │ 读：内省                   │ 写：构造/赋值
           ▼                           ▼
   WorldModel::Snapshot        SceneBuilder（现状硬编码
   WorldModel::TypeSchema      → 未来 ClassInfo::factory）
           │                           │
           ▼                           ▼
   LLM（DeepSeek）◄── 场景 JSON ──► ECS World ──► RenderPipeline
```

- **反射是 AI 的"眼睛"**（观察：快照 + 词汇表）与**"手"**（动作：建场景、写属性）；
- **AI 是反射的"消费者"**（也是编辑器 Details 面板之外反射系统的第二个主要消费方）；
- 两者通过 `PropertyInfo`（offset/typeName/attributes）这一个数据契约解耦——AI 层完全不需要包含任何 `Scene/*Component.h` 的具体头文件依赖。

## 八、现状评估

**已落地**（git 记录 `4ca47f1` / `d75d56d`）：
- 反射侧：4 个 `HE_ATTR_AI_*` 宏 + `AttrKey::Ai*` 键 + 3 个组件类型的实际注解；
- AI 侧：`WorldModel`（反射读）、`SceneBuilder`/`PromptToScene`/`DeepSeekClient`（LLM 写）、`IAIDevice`/`InferenceScheduler`/`AIModule`（推理运行时骨架）；
- 6 组 doctest 单元测试（含 `TestWorldModel` 直接验证 AI 注解驱动快照/词表）+ `05.LLMScene` 端到端示例。

**未落地**（设计文档中的后续路线）：SceneBuilder 反射泛型化、AI 动作 → `he::Command` 可撤销接入、`HE_ATTR_AI_TOOL` 工具调用、Agent 组件（LLM/RL/行为树可插拔）、编辑器 AI 观察面板、神经渲染零拷贝接入 `IAIDevice`。
