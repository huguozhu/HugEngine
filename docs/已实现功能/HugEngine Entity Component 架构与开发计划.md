# HugEngine Entity Component 开发计划（对照 UE5）

> ## ✅ 全部完成
>
> **本文档计划内的任务已全部收口**（§三 任务总表 **1~26 没有任何未完成项**）：
> · 阶段 S0 / P1 / P2 / P3 / P4 全部落地（原编号 S0.1~C4 → 现编号 1~20）；
> · 后续扩展与 MVP 技术债也全部完成（**21** 剪辑混合、**22** 动画重定向、**23** bindless 堆环形化、
> **24** Decal GBuffer 投影、**25** 逐实例 GPU 剔除、**26** Collision 调试线框，见 §十四~§十九）；
> · 原 **27/28**（架构触发项：渲染类型注册表化 / CollectLights 数据驱动抽取）**已按决定删除**：
> 两者都是"没有消费方就不提前泛化"的改动，触发条件（下一个渲染组件 / 下一个光源类型）未到，
> **编号 27/28 不复用** —— 后续新任务从 **29** 起编号（见 §三 表注与文首修订记录）；
> · 复核手段与结果：§三 任务总表逐项 ✅ 且带提交号、`check_tables` **0 处不一致**、
> 各任务的判据（doctest + 示例冒烟）在本会话中逐项实测通过（最终 doctest **200 例 / 5482 断言**）。
>
> **本文件已归档到 `docs/已实现功能/`**（原在 `docs/计划实现功能/`）。组件功能与架构的当前状态，
> 另见《`docs/HugEngine引擎介绍/` 下的 Entity-Component 架构与组件功能文档》。
>
> 日期：2026-09-01（初版）| 状态：✅ 全部完成（2026-09-18）
> 修订记录：
>   - 2026-09-04：对齐代码基线（`404de09`），新增 **Phase S0（已有组件补齐 AI 一等公民）**
>   - 2026-09-06：S0~P3 全部落地（提交 `8813211` → `56696ea` 共 9 个），doctest 85 用例 / 509 断言通过；
>     剩余仅 Phase C 大工程（依赖路线图 P6/P3）
>   - 2026-09-18：**删除任务 27/28**（渲染类型注册表化 / CollectLights 数据驱动抽取）——
>     两项都是"没有消费方就不提前泛化"的架构改动，用户决定不做；**编号 27/28 不复用**，
>     后续新任务从 **29** 起编号（与删除原 C3/Audio 时的处理一致）
>   - 2026-09-18：任务 26（Collision 调试线框）落地（见 §十九）；**21~26 全部完成**
>   - 2026-09-18：任务 25（InstancedMesh 逐实例 GPU 剔除 + 接入间接绘制）落地（见 §十八）
>   - 2026-09-18：任务 24（Decal GBuffer 投影 Pass）落地（见 §十七）
>   - 2026-09-18：任务 23（bindless 堆环形化）落地（RHI 基础件 + 消费方接入，见 §十六）
>   - 2026-09-18：任务 22（动画重定向）落地（提交 `98b94d4` + 演示 `6c3f33e`），见 §十五
>   - 2026-09-18：任务 21（骨骼剪辑混合）落地（提交 `af86387` + 演示 `2b07df7`），见 §十四
>   - 2026-09-18：**任务统一编号（从 1 开始）** —— 原先用阶段前缀编号（S0.1 / A7 / B3 / C2），
>     跨阶段无法一眼看出"总共有多少任务、下一个做哪个"。现在全部任务从 1 连续编号（**1~20 已落地，
>     21~28 待办**），**原编号一律保留在括号里**作为别名（代码注释里引用的是原编号，靠 §三 的任务
>     总表对照）。总表见 **§三 · 任务总表**，各处小节标题同步改为新编号。
> 目标：补齐 UE5 Actor 组件体系在 HugEngine 的对应实现；**每个组件（含已有组件）都是 AI 可读写的一等公民**（`HE_ATTR_AI_*` 注解 + SceneBuilder 词表同步），并自动获得编辑器 Details 面板的反射编辑能力。

> **归档补注（2026-09-19 复核）**：计划内任务（1~26）的实现文件已逐个核对**全部存在**（`Scene/SplineComponent`、`Scene/SplineMeshComponent`、`Scene/SplineMeshSystem`、
> `Scene/SkeletalMeshComponent`、`Scene/SkeletalMeshSystem`、`Scene/InstancedMeshComponent`、`Scene/CharacterMovementComponent`、`Scene/AbilityComponent`、
> `Scene/NavMeshComponent`，以及 `Tests/TestSplineMesh.cpp`、`Tests/TestSkeletalMesh.cpp`），文档的 ✅ 记录成立。**唯一未落地的子项**是 §九 提到的编辑器 `AgentInspector`（代码 0 处命中，已就地标注）。
> 另有一处**记录在案的验证余额**（不是未实现，属判据强度）：§十七 的贴花投影"未做逐像素 A/B 对照"。

---

## 一、现状盘点（2026-09-04 代码基线修订版）

| HugEngine 组件 | 对应 UE5 | 代码 | 反射属性注册 | AI 注解 | LLM 词表(SceneBuilder) |
|---|---|---|---|---|---|
| TransformComponent | USceneComponent | ✅ | ✅ position/rotation/scale | ✅ | —（随实体创建） |
| MeshComponent（Cube/Sphere） | UStaticMeshComponent | ✅ | 仅类型注册 | ❌（材质类属性未注册） | ✅ Cube/Sphere |
| DirectionalLight | UDirectionalLightComponent | ✅ | ✅ 4 属性 | ✅ | ✅ |
| PointLight | UPointLightComponent | ✅ | ✅ 3 属性 | ✅ | ✅ |
| SpotLight | USpotLightComponent | ✅（`86856de` 光照+阴影） | ✅ 7 属性（S0.1） | ✅ | ✅ SpotLight（S0.3） |
| RectLight | URectLightComponent | ✅（`a7cde15`~`404de09` P1+P2） | ✅ 8 属性（S0 补齐 color/intensity/castShadow/softness） | ✅ | ✅ RectLight（S0.3） |
| CameraComponent | UCameraComponent | ✅（类 + `MakeCameraData`） | ✅ 4 属性（S0.2） | ✅ | ✅ Camera（S0.3） |
| PhysicalSkyComponent | 引擎级天空 | ✅ | 仅类型注册 | ❌ | ✅ PhysicalSky |
| AnimationComponent | 简化动画 | ✅（Transform 关键帧） | 仅类型注册 | ❌ | ❌ |
| ParticleComponent | Niagara 简化 | ✅（GPU 粒子） | 仅类型注册 | ❌ | ❌ |
| AgentComponent | Pawn+AI Controller+BehaviorTree | ✅ `Engine/AI/Agent/` | ✅ brainType/systemPrompt/thinkInterval/enabled | ✅ | ❌ |
| MemoryComponent / GoalComponent | （AI 配套） | ✅ | 仅类型注册 | ❌ | ❌ |
| ProjectileMovementComponent | UProjectileMovementComponent | ✅（P1 A7，`ProjectileSystem`） | ✅ 5 属性 | ✅ | ❌（实体引用无法表达） |
| HealthComponent | UHealthComponent | ✅（P1 A8，`DamageSystem`） | ✅ 3 属性 | ✅ | ✅ Health |
| SpringArmComponent | USpringArmComponent | ✅（P1 A6，`SpringArmSystem`） | ✅ 4 属性 | ✅ | ❌（实体引用无法表达） |
| DecalComponent | UDecalComponent | ✅（P2 A3 投射片 MVP；任务 24 追加 Deferred **GBuffer 投影**，见 §十七） | ✅ 6 属性 | ✅ | ✅ Decal |
| BillboardComponent | UBillboardComponent | ✅（P2 A4，billboard 矩阵对齐相机） | ✅ 3 属性 | ✅ | — |
| TextRenderComponent | UTextRenderComponent | ✅（P2 A5，stb_truetype + 系统字体兜底） | ✅ 4 属性 | ✅ | — |
| CollisionComponent | UCapsuleComponent/UBoxComponent | ✅（P3 B5，`CollisionSystem`；任务 26 追加**调试线框** `CollisionDebugSystem`，见 §十九） | ✅ 5 属性 | ✅ | — |
| CharacterMovementComponent | UCharacterMovementComponent | ✅（P3 B3，`MovementSystem`，地面射线检测） | ✅ 5 属性 | ✅ | — |
| AbilityComponent | UAbilitySystemComponent | ✅（P3 B4，`AbilitySystem` + Action op CastAbility） | ✅ 2 属性 | ✅ | — |
| SplineComponent | USplineComponent | ✅（P3 B2，Hermite+自动切线，弧长求值/闭环回绕） | ✅ 2 属性 | ✅ | — |
| InstancedMeshComponent | UInstancedStaticMeshComponent | ✅（P3 B1 单次 DrawIndexed 万级实例；任务 25 追加**逐实例 GPU 剔除 + 间接绘制**，Forward/Deferred 双路径，见 §十八） | ✅ 2 属性 | ✅ | — |
| SkeletalMesh / Physics / NavMesh | UE5 对应组件 | ✅ 均已落地（SkeletalMesh `4d94460` / Physics Jolt C2 / NavMesh `97aaa00`）；SkeletalMesh 于 2026-09-18 追加**剪辑混合**（任务 21，`af86387`，§十四）与**动画重定向**（任务 22，`98b94d4`，§十五） | — | — | — |

**结论**：计划内组件全部落地。LLM 词表 5 → 10 组件（新增 SpotLight/RectLight/Camera/Health/Decal）；所有新组件按「一个组件 = 四件事」补齐类定义/反射/AI 注解/系统接入。Phase C 的后续扩展（SkeletalMesh 剪辑混合/动画重定向）已于 2026-09-18 补齐（任务 21/22）；Audio（C3）已从本计划移除。既有组件 Animation/Particle/Memory/Goal 的反射与 AI 注解 ✅ 已补齐。

## 二、总体原则

1. **一个组件 = 四件事**：`Component` 类定义（`Engine/Scene/Scene/` 或 `Engine/AI/`）+ 反射注册（`SceneReflect.cpp` / `AgentReflect.cpp`）+ AI 注解（`HE_ATTR_AI_VISIBLE/WRITABLE/DESCRIPTION`）+ 系统接入（渲染/更新）。
2. **AI 可读写**：数值类属性全部打 `HE_ATTR_AI_*` 注解，并同步进 `SceneBuilder` 的 `TypeSchema` 词表——LLM 生成场景即刻可用（协议速查表 §六扩展规则）。
3. **反射自动生效**：注册后编辑器 Details 面板、WorldModel 快照、AI 动作（SetProperty）自动支持，无需额外代码。
4. **验证标配**：doctest 用例（组件属性读写/容错）+ 在 `07.AISamples`（已合并原 05~10）增加对应 Feature 或扩展现有场景。
5. **新组件必须追溯旧债**：每新增一个组件，先检查既有同名组件是否满足上述四件事（参照 Phase S0 补齐清单），避免"代码有、AI 看不见"的断链。

## 三、阶段规划

| 阶段 | 内容 | 预估成本 | 说明 |
|---|---|---|---|
| **S0（基线补齐）** | 已有组件补齐反射/AI 注解/词表/主相机接入 | 1~2 天 | ✅ 已完成（2026-09-06）：S0.1~S0.4 全部落地（**现编号 1~4**），doctest 38 用例通过 |
| **A（低成本）** | Camera(系统接入) / Decal / Billboard / TextRender / SpringArm / ProjectileMovement / Health | 2~3 天/个 | ✅ 已完成（2026-09-06）：A3~A8 落地（**现编号 7~12**）；A1/A2 并入 S0（**现编号 5~6**） |
| **B（中成本）** | InstancedMesh / Spline / CharacterMovement / Ability(简化 GAS) / Collision | 1~2 周/个 | ✅ 已完成（2026-09-06）：B1~B5 落地（**现编号 13~17**）；前置重构经评估非必要（见 §六注记） |
| **C（大工程）** | SkeletalMesh / Physics / NavMesh | 数周~数月 | ✅ 均已落地（**现编号 18~20**；SkeletalMesh `4d94460` / Physics Jolt C2 / NavMesh `97aaa00`）；Audio（原 C3）已移除 |
| **后续（已完结）** | SkeletalMesh 扩展 / MVP 技术债 | 见总表 | ✅ **现编号 21~26 全部完成**（2026-09-18）；原 27/28（架构触发项）已删除，见下表表注 |

### 任务总表（从 1 重新计数）

> **编号规则**：全部任务从 1 连续编号；**原编号（S0.x / Ax / Bx / Cx）保留为别名**，因为代码注释与
> 历史提交记录里引用的是它们（例如 `TypeSchema.cpp` 的"S0.3"、`Action.cpp` 的"B4"）。
> 括号里的"原 …"即该别名。**新增任务接着往下编号，不重排、不复用已删除的编号**（原 C3 Audio 已移除，
> 其编号不再使用）。

**A. 已落地（1~20）**

| 新编号 | 原编号 | 任务 | 状态 | 详见 |
|:---:|:---:|---|---|---|
| **1** | S0.1 | SpotLight 反射注册 + AI 注解 | ✅ 2026-09-06 | §四 |
| **2** | S0.2 | CameraComponent 反射注册 + AI 注解 | ✅ 2026-09-06 | §四 |
| **3** | S0.3 | LLM 词表与 SceneBuilder 扩展（5 → 8 组件） | ✅ 2026-09-06 | §四 |
| **4** | S0.4 | 主相机系统接入（`World::GetPrimaryCamera` / `ResolveFrameCamera`） | ✅ 2026-09-06 | §四 |
| **5** | A1 | CameraComponent（组件本体，注册归 2、接入归 4） | ✅ 2026-09-06 | §五 |
| **6** | A2 | SpotLightComponent（光照/阴影本体，注册归 1、词表归 3） | ✅ 2026-09-06 | §五 |
| **7** | A3 | DecalComponent（投射片 MVP） | ✅ 2026-09-06 | §五 |
| **8** | A4 | BillboardComponent | ✅ 2026-09-06 | §五 |
| **9** | A5 | TextRenderComponent | ✅ 2026-09-06 | §五 |
| **10** | A6 | SpringArmComponent（含碰撞缩臂收尾 `bc14c57`） | ✅ 2026-09-06 / 09-07 | §五 |
| **11** | A7 | ProjectileMovementComponent | ✅ 2026-09-06 | §五 |
| **12** | A8 | HealthComponent | ✅ 2026-09-06 | §五 |
| **13** | B1 | InstancedMeshComponent | ✅ 2026-09-06 | §六 |
| **14** | B2 | SplineComponent / SplineMeshComponent（SplineMesh 收尾见 §十三） | ✅ 2026-09-06 | §六 / §十三 |
| **15** | B3 | CharacterMovementComponent（含坡度过滤收尾 `28ae06b`） | ✅ 2026-09-06 / 09-07 | §六 |
| **16** | B4 | AbilityComponent（简化 GAS + `CastAbility` op） | ✅ 2026-09-06 | §六 |
| **17** | B5 | CollisionComponent | ✅ 2026-09-06 | §六 |
| **18** | C1 | SkeletalMeshComponent（glTF skins + GPU 蒙皮） | ✅ 2026-09-07 | §七 |
| **19** | C2 | PhysicsComponent / RigidBody（Jolt 集成） | ✅ 2026-09-07 | §七 |
| **20** | C4 | NavMesh 寻路（NavMesh + A* + NavAgent） | ✅ 2026-09-07 | §七 |

**B. 任务（21~26）** —— 来源：§十一 已知 MVP 限制/技术债 + §十二 "仍待办"
（**21~26 已于 2026-09-18 全部完成**，保留在表里以便追溯）

| 新编号 | 任务 | 类型 | 依赖 / 触发条件 | 详见 |
|:---:|---|---|---|---|
| **21** | ~~SkeletalMesh **剪辑混合**（多动画片段混合 / Blend Space）~~ —— ✅ **已完成**（2026-09-18，`af86387` + 演示 `2b07df7`） | 功能扩展 | 无（独立） | §十四 |
| **22** | ~~SkeletalMesh **动画重定向**（不同骨架共用动画）~~ —— ✅ **已完成**（2026-09-18，`98b94d4` + 演示 `6c3f33e`） | 功能扩展 | 无（独立） | §十五 |
| **23** | ~~**bindless 堆环形化**（TextRender / InstancedMesh 高频更新不再靠"旧资源保活"）~~ —— ✅ **已完成**（2026-09-18，见 §十六） | 技术债 | 无（独立，涉 RHI 堆管理） | §十六 |
| **24** | ~~**Decal GBuffer 投影 Pass**（替代半透明投射片 MVP）~~ —— ✅ **已完成**（2026-09-18，见 §十七；Forward 仍为投射片） | 技术债 → 功能 | 无（独立，需 GBuffer 可写 Pass） | §十七 |
| **25** | ~~**InstancedMesh 接 GPU-Culling + 逐实例剔除**（现在仅 Forward 非 GPU-Culling 路径）~~ —— ✅ **已完成**（2026-09-18，见 §十八） | 技术债 | 无（独立） | §十八 |
| **26** | ~~**Collision 调试线框**（可视化 AABB/球/胶囊）~~ —— ✅ **已完成**（2026-09-18，见 §十九） | 技术债 | 无（独立，可复用 Billboard/线框） | §十九 |

> **怎么用这张表**：`21~26` 已全部完成（2026-09-18），本表**已无待办项**。
> **27/28 已删除**（用户 2026-09-18 决定）：这两项原是"触发式"架构改动（渲染类型注册表化、
> CollectLights 数据驱动抽取），触发条件（下一个渲染组件 / 下一个光源类型）未到，提前做属于
> "没有消费方的泛化"，与 GI 那边的取舍一致 —— 因此不作为任务保留。
> **编号不复用**：27/28 为空号，后续新任务从 **29** 起编号；已有的 1~26 一律不改号。

---

## 四、任务 1~4 详细设计（原 Phase S0：基线补齐，建议先行）

> 目的：兑现总则"一个组件 = 四件事"，让**已实现**组件立即可被编辑器与 AI 使用。

### 1. SpotLight 反射注册 + AI 注解（原 S0.1；✅ 已完成 2026-09-06）

- **改动**：`Engine/Scene/Scene/SceneReflect.cpp`（`HE_BEGIN_REGISTER(he::SpotLight)` 空注册 → 注册属性）
- **注册属性（全部 `AI_VISIBLE + AI_WRITABLE + AI_DESCRIPTION`）**：
  | 属性 | 类型 | 说明 |
  |---|---|---|
  | direction | float3 | 聚光锥轴方向（世界空间） |
  | color | float3 | 光照颜色 [r,g,b] 0~1 |
  | intensity | float | 光照强度 |
  | range | float | 影响范围（米） |
  | innerConeAngle | float | 内锥角（弧度，全亮区） |
  | outerConeAngle | float | 外锥角（弧度，衰减边缘） |
  | castShadow | bool | 是否投射阴影 |
- **验证**：doctest（`WorldModel::TypeSchema()` 输出含 SpotLight 字段）；编辑器 Details 面板可编辑；AI Snapshot 可见。

### 2. CameraComponent 反射注册 + AI 注解（原 S0.2；✅ 已完成 2026-09-06）

- **改动**：`Engine/Scene/Scene/SceneReflect.cpp`（`HE_BEGIN_REGISTER(he::CameraComponent)` 空注册 → 注册属性）
- **注册属性**：`fov`（默认 60°）/ `nearPlane` / `farPlane` / `isMain`（bool，仅 AI_VISIBLE）——全部数值类加 AI 注解。
- **验证**：doctest + Details 面板可编辑。

### 3. LLM 词表与 SceneBuilder 扩展（5 组件 → 8 组件）（原 S0.3；✅ 已完成 2026-09-06）

- **改动**：`Engine/AI/TypeSchema.cpp`（词表加 SpotLight/RectLight/Camera）；`Engine/AI/SceneBuilder.cpp`（`else if (type == ...)` 分支，安全降级：字段逐一类型检查、未知字段跳过）
- **词表新增**：
  ```json
  "SpotLight": {"fields": ["direction","color","intensity","range","innerConeAngle","outerConeAngle","castShadow"]},
  "RectLight": {"fields": ["normal","color","intensity","width","height","range","softness","castShadow"]},
  "Camera":    {"fields": ["fov","nearPlane","farPlane","isMain"]}
  ```
- **验证**：doctest（LLM 场景 JSON 含"一盏路灯"→ 生成 SpotLight 组件断言方向/锥角）；07.AISamples 冒烟（prompt「一个路灯照着的街角」）。

### 4. 主相机系统接入（原 S0.4，= 任务 5 的系统接入部分；✅ 已完成 2026-09-06）

- **改动**：`Engine/Scene/Scene/World.h/.cpp` 新增 `GetPrimaryCamera()`（遍历 CameraComponent，返回首个 `isMain`）；各渲染管线帧入口优先取主相机组装 ViewMatrix，无相机实体时回退现有 `CameraController`。
- **现状**：`render::MakeCameraData(CameraComponent, Transform)`（`Pipeline/Camera.h:72`）已存在，缺的是"从 World 选主相机"这层。
- **验证**：02.Cube 或 07.AISamples 添加带 CameraComponent 的实体后视角生效；移除后回退 CameraController。

---

## 五、任务 5~12 详细设计（原 Phase A，低成本）

> 状态注记：**任务 6（原 A2）SpotLight 本体已完成**（光照/阴影/方向修复，`86856de`），词表部分归任务 3（原 S0.3）；
> **任务 5（原 A1）CameraComponent 类已完成**，系统接入归任务 4（原 S0.4）。以下保留原始设计供参考。

### 5. CameraComponent（原 A1；✅ 已完成 2026-09-06，注册归任务 2、系统接入归任务 4）
（原 A1）

- **对应 UE5**：UCameraComponent
- **用途**：相机作为 Entity 属性（替代全局 CameraController），多相机切换、过场、AI 观察视角
- **属性（反射 + AI 注解）**：
  | 属性 | 类型 | AI 注解 |
  |---|---|---|
  | fov | float（默认 60°） | AI_VISIBLE + AI_WRITABLE |
  | nearPlane / farPlane | float | AI_VISIBLE + AI_WRITABLE |
  | isMain | bool | AI_VISIBLE |
- **系统接入**（= S0.4）：`Render` 管线读 `World::GetPrimaryCamera()`（首个 isMain 相机）组装 ViewMatrix；无相机实体时回退 CameraController
- **SceneBuilder 词表**（= S0.3）：`"Camera": {"fields": ["fov", "nearPlane", "farPlane"]}`
- **验证**：doctest（创建相机实体 → 取主相机）+ 07.AISamples 场景生成"带相机"的关卡

### 6. SpotLightComponent（原 A2；✅ 已完成 2026-09-06，本体 `86856de` + 任务 1 注册 + 任务 3 词表）
（原 A2）

- **对应 UE5**：USpotLightComponent
- **用途**：聚光灯（手电/路灯/舞台），LLM 场景生成高需求
- **属性**：`color / intensity / range / innerConeAngle / outerConeAngle / castShadow`（S0.1 打全 AI 注解）
- **系统接入**：✅ 已完成（Forward/Deferred 灯光收集 + 阴影 `SpotShadowTechnique`）
- **SceneBuilder 词表**：S0.3 补 `"SpotLight": {"fields": [...]}`
- **验证**：S0.3 的 LLM"一盏路灯"用例

### 7. DecalComponent（原 A3；✅ 已完成 2026-09-06）
（原 A3）

- **对应 UE5**：UDecalComponent
- **用途**：贴花（弹孔/污渍/路面标线），绘制到场景表面
- **属性**：`decalTexture(String 路径) / size / rotation / opacity / projectionDepth(任务 24 追加) / blendMode`
- **系统接入**：Deferred 管线投影贴花 Pass（GBuffer 修改，任务 24 ✅ 见 §十七）；Forward 为投射片卡片
- **SceneBuilder 词表**：`"Decal": {"fields": ["decalTexture", "size", "opacity", "projectionDepth"]}`
- **验证**：贴花可见性 + 大小/透明度参数生效

### 8. BillboardComponent（原 A4；✅ 已完成 2026-09-06）
（原 A4）

- **对应 UE5**：UBillboardComponent
- **用途**：始终面向相机的四边形（粒子替代、UI 指示、调试标记）
- **属性**：`color / size / texturePath`
- **系统接入**：渲染管线每帧把 Billboard 的旋转对齐相机（计算 billboard 矩阵）
- **验证**：任意相机角度下四边形朝向正确

### 9. TextRenderComponent（原 A5；✅ 已完成 2026-09-06）
（原 A5）

- **对应 UE5**：UTextRenderComponent
- **用途**：3D 世界文字（标签/数值/调试）
- **属性**：`text(String) / color / size / fontPath`
- **系统接入**：MVP 用 CPU 生成文字纹理（stb_truetype 或位图字体）→ Billboard 渲染
- **验证**：实体名称/血量实时显示

### 10. SpringArmComponent（原 A6；✅ 已完成 2026-09-06 / 09-07 碰撞缩臂收尾）
（原 A6）

- **对应 UE5**：USpringArmComponent
- **用途**：第三人称相机跟随（延迟 + 平滑回弹 + 碰撞防穿墙）
- **属性**：`targetOffset / armLength / rotationLagSpeed / bUsePawnControlRotation`
- **系统接入**：每帧把目标 Transform + 弹簧臂长度合成相机 Transform（写入关联 CameraComponent，依赖 S0.4 主相机）
- **验证**：目标移动时相机平滑跟随，碰撞时缩短臂长

### 11. ProjectileMovementComponent（原 A7；✅ 已完成 2026-09-06）
（原 A7）

- **对应 UE5**：UProjectileMovementComponent
- **用途**：抛射物运动（直线/抛物线/命中回调/生命周期）
- **属性**：`initialSpeed / maxSpeed / gravityScale / bHoming / homingTarget`
- **系统接入**：系统级 `ProjectileSystem::Update`（每帧积分位置 + 超时销毁 + 命中事件）
- **验证**：doctest（给定初速/重力 → 位置符合抛物线）

### 12. HealthComponent（原 A8；✅ 已完成 2026-09-06）
（原 A8）

- **对应 UE5**：UHealthComponent（Gameplay 基础）
- **用途**：生命值/伤害/死亡事件（玩法数值底座）
- **属性**：`maxHealth / currentHealth / bInvincible`（AI 注解：LLM 可"给敌人 100 点血"）
- **系统接入**：纯数据组件 + `DamageSystem`（ApplyDamage 函数 + 事件回调）
- **验证**：doctest（扣血/回血/死亡边界）+ AI 生成"高血量守卫"用例

---

## 六、任务 13~17 详细设计（原 Phase B，中成本）

> 前置提示（已评估，2026-09-06）：原计划建议 B 组落地前做 CollectLights/词表数据驱动化重构。
> 实测评估结论：B 组无新光源类型（CollectLights 4 份复制不受影响）；B2~B5 为数据+系统组件，
> 走 4 处低成本路径；仅 B1 触渲染枚举（3 文件约 8 行一次性成本）——**重构非必要，直接落地**。
> 渲染枚举收敛为注册表留待第 8+ 个渲染类型出现时再做（规则三）；CollectLights 抽取留待下一个光源类型。
> （2026-09-18 补注：这两个触发项原以任务 27/28 记录，现已按用户决定从计划中删除 —— 触发条件真正到来时
> 再作为新任务（从 29 起编号）评估。）

### 13. InstancedMeshComponent（原 B1；✅ 已完成 2026-09-06）
（原 B1）

- **对应 UE5**：UInstancedStaticMeshComponent / HISM（植被）
- **用途**：同一网格大量实例（草丛/森林/建筑群），单次 DrawIndexedIndirect
- **属性**：`meshPath / instanceCount / 实例变换数组（CPU 提供或程序化）`
- **系统接入**：复用 `MeshBatcher` + GPU Instancing（现有管线已有 InstanceBuffer 支持）；`enableFrustumCull` 按实例剔除
- **验证**：万级实例 FPS 对比

### 14. SplineComponent / SplineMeshComponent（原 B2；✅ 全部完成：SplineComponent 2026-09-06，SplineMeshComponent 见 §十三）
（原 B2）

- **对应 UE5**：USplineComponent / USplineMeshComponent
- **用途**：样条路径（道路/管线/摄像机轨道/Agent 巡逻路线）
- **属性**：`控制点数组（位置+切向）/ bClosedLoop / bShowPath`（SplineComponent）；`width / segments / uvTiling / enabled`（SplineMeshComponent）
- **系统接入**：`SplineSystem` 提供 `EvaluateAtDistance/GetTangent`；`SplineMeshSystem` 按样条版本脏检测沿样条生成条带网格
- **验证**：Agent 沿样条巡逻（与 AgentSystem 对接）；02.Cube 道路条带演示 ✅

### 15. CharacterMovementComponent（原 B3；✅ 已完成 2026-09-06 / 09-07 坡度过滤收尾）
（原 B3）

- **对应 UE5**：UCharacterMovementComponent
- **用途**：角色移动（走/跑/跳/重力/地面检测）
- **属性**：`walkSpeed / runSpeed / jumpHeight / gravity / maxSlopeAngle`
- **系统接入**：`MovementSystem` 每帧积分（输入方向 + 重力 + 碰撞（依赖 Collision））；MVP 可无碰撞地面投影
- **验证**：角色在场景中可移动跳跃

### 16. AbilityComponent（简化 GAS）（原 B4；✅ 已完成 2026-09-06）
（原 B4）

- **对应 UE5**：UAbilitySystemComponent
- **用途**：技能注册/冷却/释放/消耗（智能体动作链对接点）
- **属性**：`技能列表（名称/冷却/消耗）/ 当前激活技能`
- **系统接入**：`AbilitySystem::Update`（冷却计时）+ Agent 动作 `CastAbility`（Action→Command 新增 op）
- **验证**：Agent 每 5 秒施放一次"火球"（SpawnEntity + 移动）

### 17. CollisionComponent（原 B5；✅ 已完成 2026-09-06）
（原 B5）

- **对应 UE5**：UCapsuleComponent / UBoxComponent（碰撞）
- **用途**：基础碰撞体积（AABB/Sphere/Capsule）——物理与移动的前置
- **属性**：`shape / halfExtents / radius / height / bEnabled`
- **系统接入**：`CollisionSystem`（CPU 检测：AABB-AABB/球-球/射线）；渲染调试线框（Billboard/线框 mesh）
- **验证**：doctest（相交/包含/射线命中）

---

## 七、任务 18~20 详细设计（原 Phase C 大工程，依赖路线图）

| 编号 | 原编号 | Component | 依赖 | 备注 |
|---|---|---|---|---|
| **18** | C1 | SkeletalMeshComponent | 骨骼动画系统（路线图 P6 缺项） | ✅ 已完成（2026-09-07，提交 `4d94460`）：glTF skins/动画解析 + GPU 蒙皮（骨骼 SSBO + 蒙皮 PSO）+ Fox 演示 |
| **19** | C2 | PhysicsComponent / RigidBody | 物理引擎集成（BEPU/PhysX/Jolt） | ✅ 已完成（2026-09-07，Jolt 提交 `7eb1a66`~C2 竖切）：Engine/Physics 模块（PhysicsWorld/JoltConversions/RigidBodyComponent/PhysicsSystem）+ 彩球下落/碰撞/回写演示 + doctest（100+ 用例） |
| **20** | C4 | NavMesh 寻路 | 导航网格 + A*/Recast | ✅ 已完成（2026-09-07，提交 `97aaa00` C4 竖切）：NavMeshComponent + A* 寻路（绕阻挡墙）+ NavAgent 沿路径移动 + 词表/SceneBuilder + 02 演示 |

> Audio（原 C3）已从本计划移除（引擎暂不接入音频系统）——**该编号不再使用，后续任务不占用它**。

---

## 八、新增组件的开发检查清单（规范）

1. [ ] `Engine/Scene/Scene/<Name>Component.h/.cpp`（或 `Engine/AI/`）：类定义 + `HE_COMPONENT()` + `OnCreate`（默认值）
2. [ ] 反射注册：`SceneReflect.cpp` / `AgentReflect.cpp` 加 `HE_REGISTER_PROPERTY(<Name>Component, ...)` + `HE_END_PROPERTY()`，每个属性打 `HE_ATTR_AI_VISIBLE()`；数值类打 `HE_ATTR_AI_WRITABLE()` + `HE_ATTR_AI_DESCRIPTION(中文说明)`
3. [ ] 需要每帧更新的：新建 `<Name>System`（静态 Update，仿 `AgentSystem`）或挂进现有管线子系统
4. [ ] 渲染相关的：接入 Forward/Deferred 管线（灯光收集/绘制/材质）
5. [ ] `SceneBuilder`：`TypeSchema` 词表加类型与字段 + `BuildScene` 加 `else if (type == "<Name>")` 分支（安全降级）
6. [ ] `Tests/`：doctest 用例（组件创建/属性读写/容错）
7. [ ] `07.AISamples`：LLM 生成含该组件的场景用例验证（或独立 Feature）
8. [ ] 中文注释齐备

> 追溯清单（任务 1~4 的做法复用）：对"已存在但未注册"的组件执行 2/5/7 三步即可补齐。

---

## 九、与 AI / Editor 的结合总结

- **LLM 生成**：词表 10 组件同步完成；已实测（真实 DeepSeek）：「一个路灯照着的街角」生成路灯几何 + 光源 + Camera 实体并驱动主相机；SpotLight/Decal/Health 等组件可被 LLM 生成
- **智能体动作**：`CastAbility` op 已落地（Action→Command 可撤销，doctest 覆盖执行/撤销）；SetProperty/SpawnEntity/SetTransform 原有 op 保持不变
- **编辑器**：所有新组件反射注册后自动获得 Details 面板编辑能力；`AgentInspector` 可显示 AI 可读字段
  > ⚠ **未落地（2026-09-19 核对）**：`AgentInspector` 在整个代码库检索 **0 处命中**（`Engine/`、`Samples/`，排除 `External/`）——本句里"Details 面板编辑能力"由反射自动获得（成立），但 `AgentInspector` 这个面板尚未实现。若要补，按文首规则**新开编号（29 起）**，不复用旧编号。
- **观察面板**：WorldModel 快照自动包含所有带 AI_VISIBLE 注解的新组件（Health 血量、CharacterMovement 参数等 LLM 大脑可见）

---

## 十、优先级建议（修订版）

```
已落地（按落地顺序；括号里是原编号）
P0（基线）  : 1~4   —— ✅ 已完成（2026-09-06，原 Phase S0）
P1（玩法底座）: 11 ProjectileMovement(原A7) → 12 Health(原A8) → 10 SpringArm(原A6) —— ✅ 已完成
P2（表现）  : 7 Decal(原A3) / 8 Billboard(原A4) / 9 TextRender(原A5) —— ✅ 已完成
P3（中成本）: 17 Collision(原B5) → 15 CharacterMovement(原B3) → 16 Ability(原B4)
              → 14 Spline(原B2) → 13 InstancedMesh(原B1) —— ✅ 已完成
P4（大工程）: 18 SkeletalMesh(原C1) / 19 Physics(原C2) / 20 NavMesh(原C4) —— ✅ 已完成
待办        : 21 ✅ / 22 ✅ / 23 ✅ / 24 ✅ / 25 ✅ / 26 ✅ **全部完成**
              原 27/28（架构触发项）已删除（编号不复用；后续新任务从 29 起）
```

---

## 十一、完成总结（2026-09-06）

- **组件总数**：新增 11 个组件（A 组 6：Decal/Billboard/TextRender/SpringArm/ProjectileMovement/Health；B 组 5：Collision/CharacterMovement/Ability/Spline/InstancedMesh）+ S0 补齐 3 个既有组件（SpotLight/RectLight/Camera）的反射/词表 + 主相机接入（GetPrimaryCamera/ResolveFrameCamera）；doctest **85 用例 / 509 断言**全部通过（2026-09-06 基线；C1 SkeletalMesh 见 §十二）
- **词表**：LLM 组件词表 5 → 10（SpotLight/RectLight/Camera/Health/Decal 新增，截至 2026-09-06），后 Animation 补齐（`98c4080`）→ **11 种**，RigidBody 补齐（`1d4de6e`，Jolt C2）→ **12 种**，NavMesh/NavAgent 补齐（`97aaa00`，C4）→ **14 种**；SceneBuilder 全部带安全降级解析
- **提交序列**：`8813211`（S0）→ `7683686`（P1）→ `3eb963a`（P2）→ `8299254`/`11e4f14`/`1ae6637`/`3c0f571`/`56696ea`（P3 五件）
- **交互验证**：02.Cube（广告牌/文字/贴花/碰撞变色/角色移动/万级实例）、07.AISamples（真实 LLM 场景生成、主相机、火球技能、样条巡逻）
- **已知 MVP 限制（技术债清单，均已成任务，见 §三 任务总表 21~26）**：
  1. ~~bindless 堆 append-only：TextRender/InstancedMesh 动态更新以「旧资源保活」换安全，高频更新需 Heap 环形化改造~~ ✅ **已落地（任务 23，见 §十六）**
  2. ~~Decal 为投射片 MVP（无 GBuffer 投影 Pass）~~ ✅ **Deferred 已落地 GBuffer 投影（任务 24，见 §十七）**；Forward 无 GBuffer 可投影 ⇒ 仍为投射片（已知边界）；~~InstancedMesh 仅支持 Forward 非 GPU-Culling 路径，逐实例剔除未接~~ ✅ **已落地（任务 25，见 §十八）：Forward/Deferred 双路径 + 逐实例 GPU 剔除 + 间接绘制**
  3. 实体引用类属性（homingTarget/targetEntity/cameraEntity 等）不进反射/词表（u64 无法快照序列化）——**这是策略而非待办**
  4. ~~Collision 调试线框~~ ✅ **已落地（任务 26，见 §十九）**；SplineMesh 沿条生成 ✅ 已落地（`28ae06b` 坡度过滤、`bc14c57` 碰撞缩臂亦已落地）
- **遗留**：任务 18~20（原 Phase C）全部落地（Physics C2 ✅ `7eb1a66`~`2b0ae5c`；NavMesh C4 ✅ `97aaa00`；SkeletalMesh ✅ `4d94460`）；Audio（C3）已从本计划移除。SkeletalMesh 后续扩展（剪辑混合、动画重定向）；**既有组件 Particle/Memory/Goal 的反射/AI 补齐 ✅ 已完成**（Animation `98c4080`；Particle emitRate 深补 `fa1410c`；Memory/Goal 类型注册——内部结构按文档不暴露）

---

## 十二、后续状态更新（2026-09-07，对齐 HEAD `bc14c57`）

上一节为 2026-09-06 完成快照；本节记录其后落地项（文档内相关行已就地标注 ✅）：

| 提交 | 内容 | 对应文档条目 |
|---|---|---|
| `4d94460` + `ef8b75d` | **任务 18（原 C1）SkeletalMeshComponent 完成**：glTF skins/动画解析 + GPU 蒙皮（骨骼 SSBO + 蒙皮 PSO）+ Fox 演示 | §七 SkeletalMesh 行 ✅ |
| `98c4080` | **Animation 组件反射 + AI 注解 + 词表补齐**（词表 10 → 11） | §十一 词表行 / 遗留行 ✅ |
| `28ae06b` | **maxSlopeAngle 坡度过滤落地**（前向渲染移动系统，**任务 15** 原 B3 收尾） | §十一 技术债④ ✅ |
| `bc14c57` | **SpringArm 碰撞缩臂落地（防穿墙）**（**任务 10** 原 A6 收尾） | §十一 技术债④ ✅ |
| `7eb1a66`~`2b0ae5c` | **任务 19（原 C2）Physics（Jolt）完整落地**：Engine/Physics 模块（PhysicsWorld/JoltConversions/RigidBodyComponent/PhysicsSystem）+ 静态碰撞体 + 词表（RigidBody→12）+ 02 彩球演示 + ImGui 激活刚体数 + doctest（103 用例） | §七 Physics 行 ✅ / §十一 词表+遗留 ✅ |
| `fa1410c` | **ParticleComponent 反射/AI 发射参数补齐（emitRate 深补）**：emitaRate（>0 覆盖默认发射率）+ 粒子逻辑接入（兼容旧调用）；Memory/Goal 类型注册（AgentReflect，内部结构不暴露） | §十一 遗留 ✅ |
| `97aaa00` | **任务 20（原 C4）NavMesh 寻路落地**：NavMeshComponent（格子+阻挡）+ NavMeshSystem（8 向 A* 绕墙）+ NavAgentComponent/System（沿路径移动）+ 词表（NavMesh/NavAgent→14）+ 02 演示（紫球绕墙走到目标）+ doctest（108 用例） | §七 NavMesh 行 ✅ / §十一 词表 ✅ |

**任务清单（已编号，见 §三 任务总表 21~26；本清单已无待办）**：
- ✅ 任务 18~20 已全部落地（SkeletalMesh / Physics C2 / NavMesh C4）；Audio（原 C3）已从本计划移除
- ✅ **21** SkeletalMesh 剪辑混合（多片段混合 / Blend Space）—— 已完成（2026-09-18，`af86387` + 演示 `2b07df7`，见 §十四）
- ✅ **22** SkeletalMesh 动画重定向（不同骨架共用动画）—— 已完成（2026-09-18，`98b94d4` + 演示 `6c3f33e`，见 §十五）
- ✅ **23** bindless 堆环形化（TextRender/InstancedMesh 高频更新）—— 已完成（2026-09-18，见 §十六）
- ✅ **24** Decal GBuffer 投影 Pass（替代投射片 MVP）—— 已完成（2026-09-18，见 §十七）
- ✅ **25** InstancedMesh 接 GPU-Culling + 逐实例剔除 —— 已完成（2026-09-18，见 §十八）
- ✅ **26** Collision 调试线框 —— 已完成（2026-09-18，见 §十九）
- ~~27 渲染类型注册表化~~ / ~~28 CollectLights 数据驱动抽取~~ —— **已删除**（2026-09-18，按用户决定；
  两项均为触发式架构改动，条件未到不做。**编号不复用**，后续新任务从 29 起）
- 组件 AI 补齐已完成（Animation/RigidBody/Particle 反射+词表；Memory/Goal 类型注册）——后续仅当需暴露内部结构时再议

---

## 十三、任务 14（原 B2）遗留落地：SplineMeshComponent（沿样条生成条带网格）

承接 §十二 待办中的"SplineMesh 沿样条生成"（即任务 **14** 的后半），本次完成：

**新增**
- `Engine/Scene/Scene/SplineMeshComponent.h/.cpp`：继承 `MeshComponent`（对齐 UE5 USplineMeshComponent）；生成参数 `width / segments / uvTiling / enabled`，关联样条 `splineEntity`（实体 ID，按既有策略不入反射/词表）
- `Engine/Scene/Scene/SplineMeshSystem.h/.cpp`：静态 `Update(World&, IRHIDevice*)`，按**样条数据版本**脏检测重建（未变化零成本；无效样条清空网格不崩溃）
- `Tests/TestSplineMesh.cpp`：3 用例 / 19 断言（顶点索引数量、包围盒宽度、版本脏检测、无效样条容错）

**修改**
- `SplineComponent.h/.cpp`：新增数据版本号 `GetVersion()`（AddPoint/Clear/Rebuild 递增），供下游做内容变化检测
- `SceneReflect.cpp`：注册 `SplineMeshComponent` 属性（全部带 `HE_ATTR_AI_*` 注解）
- `SceneRenderer.cpp` / `ForwardPipeline.cpp`：按既有"派生渲染组件显式列举"惯例接入绘制收集
- `Samples/02.Cube`：新增绕场景的样条道路演示（6 控制点 → 48 段、宽 2m 条带），主循环调用 `SplineMeshSystem::Update`

**验证**
- 全量 doctest：111 用例 / 3316 断言全部通过
- 02.Cube 运行冒烟：`样条网格（B2）: 沿 6 控制点样条生成 48 段条带` → 运行期 `MeshComponent: 98 vertices, 288 indices`（=(48+1)×2 / 48×6）、包围盒覆盖样条范围、场景渲染 24 draws 正常、退出无崩溃

**说明**：条带为单面几何，默认 `doubleSided = true`（便于从下方观察）；实体引用类依赖（`splineEntity`）保持不可由 LLM 生成，与 `homingTarget` 等同一策略。

---

## 十四、任务 21 落地：骨骼剪辑混合（Blend Space / 交叉淡入）

承接 §十二 待办中的"SkeletalMesh 后续扩展：剪辑混合"，本次完成。

**问题**：原实现是**单剪辑播放** —— `SkeletalMeshComponent` 只有 `currentClip / clipTime`，
`SkeletalMeshSystem` 每帧采样这一个剪辑。于是"走路切跑步"只能硬切（姿态跳变），
也无法表达 Blend Space（按速度混合两个剪辑）。

**新增能力（引擎侧）**

| 位置 | 内容 |
|---|---|
| `Scene/SkeletonAsset.h` | `AnimationBlendLayer`：剪辑下标 + **自己的**时间/速度/循环 + 权重（纯数据，无 RHI 依赖，单测可只对着数学写） |
| `Scene/SkeletalMeshSystem.h/.cpp` | `SampleJointTRSBlended` / `ComputeSkinMatricesBlended`；层级合成与蒙皮公式抽成共用的 `ComposeSkinMatrices`（两条路径必须逐字一致，否则"权重=1 的混合"将不等于单剪辑） |
| `Scene/SkeletalMeshComponent.h/.cpp` | `blendLayers[kMaxBlendLayers=4]` + `SetBlendLayers` / `SetBlendLayer` / `ClearBlendLayers` / `CrossFadeTo` / `GetBlendWeights` |

**混合规则（每条都有理由）**

1. **权重按 Σw 归一化**：平移/缩放线性加权，旋转"符号对齐到首个参与层 → 加权求和 →
   归一化"（加权 nlerp）。
   · 为什么要符号对齐：`q` 与 `-q` 是**同一个旋转**，直接相加会互相抵消（典型现象是混合到
   中途姿态抽搐或塌陷到单位四元数）；对齐到同一半球后加权和才有意义。
2. **`clipIndex < 0`、权重 ≤ 0 的层不参与**；**全部层都不参与时退回关节静态 TRS**
   （即绑定姿势）——与单剪辑路径 `clipIndex = -1` 的语义完全一致。
3. **每层推进自己的时间**：速度/循环逐层独立；**权重为 0 的层也推进**（否则淡入的那一层
   会永远停在起点，淡入完成时姿态从第一帧开始跳）。两层 Blend Space 的"时间同步"由调用方
   负责（示例里把两层的 `time` 设成同一个值）。
4. **交叉淡入会收敛成单层**：`CrossFadeTo` 布下出/入两层，`Update` 每帧把出层权重 1→0、
   入层 0→1，淡完只留入层（`blendLayerCount = 1`）。不收敛的话会**长期付两倍的采样成本**，
   而权重 0 的层对结果没有任何贡献。
5. **单剪辑与混合两条路径互斥**：`blendLayerCount == 0` 时走原来的单剪辑路径（既有行为
   逐字不变），`PlayClip` 会显式清空混合层，避免"面板选了剪辑但混合层还在"的状态含糊。

**判据**

- **doctest**：`Tests/TestSkeletalMesh.cpp` 新增 4 例（全量 **176 例 / 5273 断言**全过）：
  · 单层权重 1 的混合 == `SampleJointTRS` 的单剪辑采样（等价性，保证没有引入行为变化）；
  · 权重 1:3 的平移混合 == `(1·x0 + 3·x1)/4`（加权平均而不是求和）；
  · 全零权重 / 越界剪辑层 / 空层 都退回绑定姿势；非法关节下标给单位值不崩溃；
  · 四元数半球对齐（同一旋转写两遍，结果仍是该旋转，`|dot| = 1`）；
  · 组件 API 与交叉淡入状态机：越界剪辑与超上限被拒绝、权重读数归一化、每层按自己的速度
    推进、淡入一半时权重 (0.5, 0.5)、淡完收敛成 1 层且旧字段同步、`duration = 0` 一帧内切换、
    越界目标忽略、`PlayClip` 退出混合。
- **示例冒烟**（02.Cube，Release，跑 20 秒）：日志出现
  `[任务 21] 骨骼剪辑混合：Walk → Run 交叉淡入 1.5s（最多 4 层…）`，1.5 秒后出现
  `[任务 21] 交叉淡入完成：收敛为单层剪辑 #2（权重 1.00）`（证明混合路径真的执行并收敛）；
  校验层 error/VUID 计数 **0**、无崩溃。面板新增"剪辑混合"一节（Blend Space 滑块 +
  两个交叉淡入按钮 + 实时层数/权重显示）。

**已知边界**

- 只做**逐关节 TRS 混合**（nlerp），没有动画曲线、没有 Additive 层、没有骨骼遮罩（上半身/
  下半身分离）——那属于"动画系统"层的内容，本任务只补"多剪辑混合"这一层。
- 混合**在 CPU 采样后做**（每层采样全部关节再混合），层数上限 4；层数越多每帧采样次数线性
  上升。真实项目里会在烘焙/压缩后的轨道上做，这里保持与既有单剪辑路径同一套采样器。
- 没有 Blend Space **资产**（1D/2D 混合空间定义、阈值表）——调用方自己算权重（示例里是
  滑块或速度），引擎只提供"多层 + 权重"这一原语。

---

## 十五、任务 22 落地：动画重定向（不同骨架共用同一套剪辑）

承接 §十二 待办中的"SkeletalMesh 后续扩展：动画重定向"，本次完成。

**问题**：上一版（含任务 21）里"剪辑"和"骨架"是绑死的 —— 剪辑的下标/关键帧通道直接按
**自身关节下标**采样。于是想给"另一副骨架"（不同绑定姿势、不同骨骼长度、不同关节朝向）
用同一套 Walk/Run，只能硬搬源骨架的 TRS，结果是把目标骨架拉成源骨架的形状（关节翻转、
四肢错位）。这在实际项目里等价于"每个模型都要重做一套动画"。

**核心思路：只借"相对绑定姿势的偏移"**

```
目标关节旋转 = 目标绑定旋转 × (源绑定旋转⁻¹ × 源动画旋转)
目标关节平移 = 目标绑定平移 + (源动画平移 − 源绑定平移) × k
目标关节缩放 = 目标绑定缩放 × (源动画缩放 / 源绑定缩放)
```

- 旋转那一行是重定向的全部要点：`(源绑定⁻¹ × 源动画)` 是**源相对它自己的偏移量**，
  叠加到目标自己的绑定旋转上 ⇒ 目标骨架保留自己的朝向/体型，动画的"动作"照搬。
  源关节没有旋转通道时偏移是单位四元数 ⇒ 结果就是目标绑定姿势（不会把源绑定姿势搬过来）。
- 平移默认 **不重定向**：平移编码的是"骨骼长度/体型"，直接搬会把不同体型的骨架拉变形。
  打开后按各关节**绑定长度比** `k = |目标绑定平移| / |源绑定平移|` 自动缩放（`autoProportion`），
  适合"同比例放大/缩小的骨架"；还有 `translationScale` 做全局微调。
- 缩放同理（默认关，多数骨架没有缩放通道），逐分量取倍率，源绑定缩放为 0 时退化为 1（不除零）。

**关节对应关系：按名字匹配**

`BuildRetargetProfile(target, source)` 按 `SkeletonJoint::name` 逐一对应（glTF/UE 的关节名
是稳定的，索引在不同骨架之间**没有**任何对应关系）。名字对不上或无名关节 → `targetToSource = -1`
⇒ 该关节保持目标绑定姿势。也支持调用方传自制映射（只映射一部分、故意让某些关节不动）。

**新增能力**

| 位置 | 内容 |
|---|---|
| `Scene/SkeletonAsset.h` | `RetargetProfile`：`targetToSource` 映射表 + 逐分量开关（`retargetTranslation` / `retargetScale` / `autoProportion` / `translationScale`）+ `MappedJointCount()` |
| `Scene/SkeletalMeshSystem.h/.cpp` | `BuildRetargetProfile` / `SampleJointTRSRetargeted` / `ComputeSkinMatricesRetargeted`，以及**重定向 × 多层混合**的 `SampleJointTRSBlendedRetargeted` / `ComputeSkinMatricesBlendedRetargeted`；另补 `RebuildInverseBindMatrices`（改过绑定姿势后重算逆绑定矩阵） |
| `Scene/SkeletalMeshComponent.h/.cpp` | `SetAnimationSource(source, profile=nullptr)` / `ClearAnimationSource()` / `AnimationSource()`；`skeleton`（网格 + 绑定姿势 + 逆绑定）与 `sourceSkeleton`（动画剪辑）分离 |
| 共用实现 | `ComposeSkinMatrices` / `BlendLayerSamples` 两个模板：单剪辑 / 混合 / 重定向三条路径共用同一套层级合成与混合规则，避免"权重=1 的混合 ≠ 单剪辑"这类分叉 |

**语义要点（易踩的坑）**

1. **层级合成与 `inverseBind` 始终用目标骨架**：蒙皮矩阵必须落在目标骨架上
   （`world_target × invBind_target`），重定向只影响"每帧的本地 TRS 从哪里来"。
2. **设置动画来源后，剪辑下标一律指源骨架的剪辑表**：`PlayClip` / `SetBlendLayer` /
   `CrossFadeTo` 的越界校验、`Update` 的时长回绕都走 `AnimationSource()`。
   同时支持"目标骨架自己一个剪辑都没有"（示例里的高个骨架就是这样）。
3. **重定向与多层混合是可组合的**：每层先做重定向采样，再按任务 21 的规则（权重归一化、
   四元数半球对齐、全不参与⇒目标绑定姿势）混合；基准值取**目标关节的静态 TRS**。
4. **程序化改绑定姿势后必须 `RebuildInverseBindMatrices`**：否则 `world × invBind ≠ 单位`，
   连绑定姿势都会变形/炸开。示例里把 Fox 骨架所有关节绑定平移 ×1.5 造出"高个骨架"，
   就是先重算逆绑定矩阵再挂动画。

**判据**

- **doctest**：`Tests/TestSkeletalMesh.cpp` 新增 6 例（全量 **182 例 / 5350 断言**全过）：
  · 名字匹配（命中 / 对不上 / 无名关节 / 目标关节比源多）；
  · 源处于**自己的绑定姿势**时，目标保持**自己的绑定姿势**（偏移为单位四元数）；
  · t=1 时目标旋转 == `目标绑定 × (源绑定⁻¹ × 源动画)`（绑定姿势差值，而不是源姿态直搬）；
  · 未映射关节 / 越界关节 → 目标绑定姿势 / 单位值，不崩溃；
  · **同骨架时重定向结果与单剪辑采样逐项相等**（全开关打开）——保证没引入行为分叉；
  · 平移/缩放开关：`k = |目标绑定|/|源绑定|`、`autoProportion=false`、`translationScale`、
    源缩放为 0 不退化成 NaN；
  · 重定向 × 混合：权重 1:3 得到加权平均 5，全零权重退回**目标**绑定姿势；
  · 改绑定姿势 → 重算逆绑定矩阵后绑定姿势蒙皮矩阵回到单位阵（不重算则不是）；
  · 组件：`SetAnimationSource` 自动建映射（2/2）、目标骨架无剪辑也能 `PlayClip(0)`、
    重定向结果与直接调用一致且**不是**源姿态、越界剪辑与 `ClearAnimationSource` 安全降级。
- **示例冒烟**（02.Cube，Release，跑 20 秒）：日志出现
  `[重定向] 动画来源: Skin_0 → Skin_0_Tall（映射 24/24 关节，仅旋转）`、
  `[任务 22] 动画重定向：… 播放源骨架的 Walk 剪辑`，1 秒后出现
  `[任务 22] 重定向生效：同一套 Walk 剪辑下，源尾尖世界位置 (…)、目标 (…)`；
  校验层 error/VUID 计数 **0**、无崩溃。面板新增"动画重定向"一节（源→目标与映射数、
  平移重定向开关 + 自动比例开关、当前时间）。

**已知边界**

- 只做**逐关节 TRS 重定向**：没有 IK 修正（脚底滑动/穿地不会自动纠正）、没有
  "手/脚对齐"这类姿态偏移、没有骨骼长度归一化以外的比例映射（UE 的 Retarget Pose 资产）。
- 映射靠**关节名字**：同一套模型换命名规则（"Bip01_L_Hand" vs "mixamorig:LeftHand"）
  需要调用方自己给映射表，引擎不做名字模糊匹配/别名表。
- 重定向后仍走 CPU 逐关节采样（与任务 21 同一套采样器），没有烘焙成目标骨架自己的剪辑
  （真实项目里会在导入期烘焙离线剪辑以避免每帧重定向成本）。

---

## 十六、任务 23 落地：bindless 堆槽位环形化（高频更新不再靠"旧资源保活"）

承接 §十一 技术债①。**问题**：bindless 堆原来是 append-only —— 每次 `RegisterTexture` /
`RegisterBuffer` 都往后追加一个槽位，旧槽位永远指着老资源。于是：

- 描述符数组只增不减（纹理/SSBO 数组各有容量上限），**文字每变一次、实例变换每变一次就吃一个槽位**；
- 调用方只能"把旧资源一直保活"（`TextRenderComponent` 的历史纹理 vector、
  `InstancedMeshComponent::retiredBuffers`），显存无界增长；
- 02.Cube 甚至因此把 FPS 文字刷新**从每帧降到 2 秒**（代码注释里写着"更新会追加槽位"）。

**方案：三段式，各管一件事**

| 层 | 件 | 职责 |
|---|---|---|
| RHI | `BindlessSlotRing`（新） | 槽位**复用**：空闲表 + `graceFrames` 保护期。释放的槽位要过 N 帧才回到空闲表；`Acquire()` 有空闲槽就复用（数组长度不增），否则追加 |
| RHI | `FrameRetireQueue<T>`（新） | 资源**有界延迟释放**：`Retire` 进当前槽位，每帧 `Advance` 释放最老槽位。槽位数固定 = `2 × kMaxFramesInFlight` |
| RHI | `IRHIBindlessHeap` + `VulkanBindlessHeap` | `ReleaseTexture/Sampler/Buffer` 登记回收、`BeginFrame` 帧边界推进、槽位统计查询；Vulkan 侧用两组环形（纹理/缓冲） |
| 引擎 | `VulkanDevice::AdvanceDeferredDestroy` | 每帧（内部去重）顺带推进 bindless 环 —— 与延迟销毁同一帧边界，安全前提完全相同 |
| 场景/渲染 | TextRender / InstancedMesh / SkeletalMesh | 内容变化时 `Release` 旧槽位 + 旧资源进退役队列；缓冲**容量够就 Map 原地复用**（句柄不变），只在扩容时重建并退役旧缓冲 |

**为什么释放要等 N 帧（而不是立刻清空描述符）**：可能有最多 `kMaxFramesInFlight` 个"在飞"的帧
仍会用该槽位里的描述符取资源。保护期内槽位内容保持原样、只是不再分配给新资源 —— 立即改写会让
在飞的帧采到错误的纹理（画面闪烁），立即销毁资源更是直接悬垂。

**为什么退役队列槽位数取 2 × 飞行帧数**：沿用 `DeferredDestructionQueue` 的事故教训（槽位数正好
等于飞行帧数时，一帧内多次推进会让释放退化为"同帧"，见该文件头注释）。

**判据**

- **doctest**：新增 `Tests/TestBindlessHeap.cpp`（8 例；全量 **192 例 / 5405 断言**全过）：
  · `BindlessSlotRing`：空闲表为空时新分配；释放后**保护期内不复用**、过 grace 帧后复用同一槽且
    槽位总数不增；越界/重复释放/已空闲槽重复释放均安全忽略；`graceFrames=0` 归一为 1；
  · **高频压力**：模拟 200 帧"每帧释放上一槽 + 申请新槽"，槽位总数稳定在 `飞行帧数 + 1` 以内
    （append-only 时会是 201）；
  · `FrameRetireQueue`：入队后仍持有资源、轮转一圈后真的析构（`weak_ptr` 观测）、每帧退役 1 个
    持续 500 帧时队列长度 ≤ `kSlots`、`FlushAll` 立即释放；
  · 组件级（Mock 纹理/缓冲，无需 GPU）：TextRender 重建 100 次后存活纹理 ≤ `kSlots + 1`、
    `FlushAll` 后只剩当前这张；InstancedMesh/SkeletalMesh 缓冲退役有界、容量与句柄保持。
- **示例冒烟**（02.Cube，Release，跑 25 秒，0 error / 0 VUID）：
  · 文字纹理**每 0.25 秒**重建（任务 23 前是 2 秒）：日志显示重建 #2 ~ #80 一直用**同一个槽 7**、
    `槽位总数` 恒为 **8**（append-only 会涨到 ~87）；收尾 flush 恒为 `8 textures / 4 buffers`；
  · 实例变换**连续 3000 帧**每帧写入 10000 个实例：日志显示 `SSBO 句柄 1（容量 10000），退役缓冲 0`
    —— 原地复用，未新建缓冲；
  · 面板新增"每帧更新实例变换 (任务 23)"开关与句柄/容量/退役数实时显示。
- **回归**：06.GILab（重 bindless 场景，416 张纹理）跑 22 秒，无新增校验告警（该示例的告警族
  均为既有的设备扩展/RT/DGC/描述符布局项，历史日志同样存在；本任务在 GILab 里**零槽位回收发生**，
  因为它没有高频重建的 TextRender）。

**已知边界**

- 槽位复用的是**整槽覆盖**：`Flush()` 仍会重写整个数组（没有做"只更新脏槽"的增量写入）。
  数组长度现在稳定了，但每次 Flush 的写入量随已注册资源数增长 —— 真要省，需要改成
  `vkUpdateDescriptorSets` 只写变更范围（后续可做）。
- 保护期按**帧数**而非"资源是否真的还用着"判断（没有 per-frame in-flight 引用计数/时间线信号量
  查询）。这是与延迟销毁队列一致的取舍：多等 1~2 帧，换实现简单且安全。
- 独立注册的 `RegisterSampler`（不与纹理配对）仍走追加：它的下标空间与纹理槽位语义不同，混用会
  互相踩踏；引擎内目前没有调用点。

---

## 十七、任务 24 落地：Decal GBuffer 投影 Pass（Deferred 路径）

承接 §十一 技术债②。**问题**：原来的贴花是一张**半透明四边形**（`DecalComponent::OnCreate`
生成的卡片），靠 Transform 摆放去"贴"表面 —— 平面凑合，曲面、台阶、斜坡上会穿模/悬空/被
深度裁掉；而且它在 Deferred 里是作为普通网格进 GBuffer 的，会把地面 albedo 直接覆盖成
"卡片自己的平面 + 卡片自己的世界坐标"，从光照角度看根本不是投影。

**做法：把贴花当成投影体积投到 GBuffer 上（新 `DecalPass`）**

| 步骤 | 内容 |
|---|---|
| ① 几何 | 逐贴花画一个**盒子**（单位立方体 × 贴花尺寸/厚度），只覆盖"可能被影响"的屏幕区域 |
| ② 取真实表面 | 片段着色器采样 GBuffer **MRT4（世界坐标）** 拿到该像素上的真实表面点；采样**深度**做天空判定（`depth >= 1` ⇒ 无几何体，discard） |
| ③ 体积裁剪 | 把世界点变换到贴花局部空间，`abs(local) > halfExtents` ⇒ discard —— 这一步就是"投影"与"投射片"的本质差别 |
| ④ 采样与混合 | 盒内点按局部 `xy` 反算 UV 采样贴花纹理（bindless 句柄走 push constant），颜色/法线/粗糙度按 `alpha` **硬件混合**写回 MRT0/MRT1 |

**通道与状态设计（每条都有原因）**

1. **与 GBuffer 共用 8 个 MRT，但只写 MRT0/1**：PSO 的 `colorBlend[2..7].writeMask = None`，
   emissive/velocity/worldPos/disney/lightmapKey 原样保留（贴花不改变几何、光照参数以外的量）。
2. **`colorLoadOp = Load`**：这四个字符是关键 —— Clear 会把整个 GBuffer 清掉。
3. **无深度附件（`depthFormat = Unknown`）**：本 Pass 要把深度当**纹理采样**（天空判定）；
   同一图像既做附件又做采样 = feedback loop，校验层会直接报错。遮挡关系由②③的世界坐标裁剪保证。
4. **不做"读 MRT0/MRT1 再写回"**：那同样是附件读写冲突。最终颜色交给混合方程
   `dst = src·srcAlpha + dst·(1−srcAlpha)`；法线在 GBuffer 里是 `N*0.5+0.5` 的**仿射**编码，
   逐分量线性混合与解码后混合等价，所以直接混编码值是正确且省事的选择。
5. **逐贴花一次 `DrawIndexed(36)`，参数走 push constant**（240 字节）：贴花数量通常在几十量级，
   不值得为它引入实例化/对象缓冲。`DecalPushConstants` 的**逐字段偏移**由 doctest 静态断言钉死，
   防止 C++ 结构与 Slang cbuffer 悄悄错位。
6. **Deferred 必须排除贴花卡片**：否则卡片会作为不透明网格写进 GBuffer（重复且错误）。
   排除必须**三处口径一致** —— `SceneRenderer::Prepare` / `MeshBatcher::Build` / `GPUScene::Collect`，
   否则三者的 objectIndex 会错位（`FillGPUScene` 按顺序对齐）。GPUScene 的口径在
   **首次 Collect 之前**设定（它首次全量收集后走增量分支，中途改口径会让缓存列表错位）。
7. **开关 `r.Decal.Project`（默认 1）**：只门控投影 Pass。关掉后 Deferred 不再画贴花
   （卡片已排除），想对比"投射片 vs 投影"切 Forward 路径即可（Forward 用卡片）。

**新增/改动**

| 位置 | 内容 |
|---|---|
| `Shader/Shaders/Decal/DecalProject.{vert,frag}.slang` | 盒体顶点着色器 + 投影/裁剪/混合片段着色器 |
| `Render/Pipeline/DecalPass.{h,cpp}` | 立方体几何、PSO（8 附件 + Load + 无深度附件 + per-MRT writeMask）、描述符集（bindless 纹理/采样器 + MRT4/深度采样）、逐贴花绘制 |
| `Render/Pipeline/DeferredPipeline.*` | 持有 `m_DecalPass`；Init/Shutdown/OnResize；GBuffer 排除贴花卡片 |
| `Render/Pipeline/DeferredPipeline_FrameGraph.cpp` | 新 pass `Decal_Project`：读 `gbWorldPos`+`gbDepth`，写 `gbA`+`gbB`，位置在 GBuffer 之后、GI/Lighting 之前 |
| `DecalComponent` | 新参数 `projectionDepth`（投影体积厚度，默认 0.5m）+ 反射注册 + AI 注解 + 词表（`TypeSchema`） |
| 收集口径 | `SceneRenderer::Prepare(..., excludeDecals)`、`MeshBatcher::Build(world, excludeDecals)`、`GPUScene::SetExcludeDecals` |

**判据**

- **doctest**：`Tests/TestDecal.cpp` 追加 2 例（全量 **194 例 / 5426 断言**全过）：
  · `projectionDepth` 默认 0.5、反射已登记（偏移与 `offsetof` 一致、`AiWritable`/`AiVisible` 注解齐全）、
    词表含 `projectionDepth`；
  · `DecalPushConstants`：`sizeof == 256 ≤ kMaxPushConstantSize`，且 13 个字段的 `offsetof`
    逐个对齐 Slang cbuffer 布局（这是最容易静默出错的地方）。
- **示例冒烟**（02.Cube，Release，两种路径对照，各 0 error / 0 VUID）：
  · Deferred（`render.pipelineMode=1`）：
    `[任务 24] 贴花 #0：中心 (0.00, 0.12, 0.00)，半尺寸 (3.00, 3.00, 0.25)，投影法线 (0.00, 1.00, 0.00)，不透明度 0.60，纹理槽 4（有贴花纹理）`
    —— 与场景设定（6×6 平放地面、抬高 0.12m、地面顶面 y=0.1）逐项吻合：盒子沿法线 ±0.25m **确实包住地面**；
    随后 `[任务 24] GBuffer 投影贴花：本帧 1 个贴花（累计 2700 帧…）` 证明 pass 持续执行；
  · Forward（`render.pipelineMode=0`）：**0 条** `[任务 24]` 日志（投影 Pass 不参与）、贴花卡片照常创建，
    说明两条路径隔离正确；面板新增"贴花: 投射片卡片（Forward）/ GBuffer 投影（Deferred）+ 已跑帧数/贴花数"。
- **校验层**：Deferred 下 20 秒 0 条校验告警 —— 说明 Load 通道、per-MRT writeMask、
  无深度附件 + 深度采样（无 feedback loop）、以及 RenderGraph 的屏障/layout 全部正确。

**已知边界**

- **Forward 仍是投射片卡片**：Forward 没有 GBuffer 可投影（它的 albedo 是即时着色，不存在"可写的
  中间表面"）。要做 Forward 贴花得走"深度重建世界坐标 + 在 PBR 着色里叠加"的路线，属于另一个设计。
- **只写 MRT0/1**：贴花不改变自发光/AO/迪士尼参数/运动矢量（运动矢量保持不变意味着 TAA 下贴花
  区域仍按地面自身的速度重投影，这恰好是想要的）。
- ****未做逐像素 A/B 对照**：本轮判据止于"参数几何一致 + pass 持续执行 + 校验层零告警 + 布局断言"，
  没有把 GBuffer albedo 读回做像素级比对（该示例没有纹理读回设施，新增读回与同步的成本高于本任务
  的收益）。若后续要自动化像素判据，可复用 GI 那边的纹理落盘设施。
- 投影盒厚度需要调用方保证"包住要贴的表面"（`projectionDepth` 默认 0.5m）；贴花盒没包到的部分
  会被正确裁剪掉 —— 表现为"贴花缺一块"，而不是错误地投到别处。

---

## 十八、任务 25 落地：InstancedMesh 逐实例 GPU 剔除 + 间接绘制

承接 §十一 技术债②的后半句。**问题**：实例化网格是"一个组件 = 上万个实例"，而原路径是
`DrawIndexed(indexCount, instanceCount)` —— 粒度是**整个组件**：只要组件（甚至不经任何测试）
在场景里，10000 个实例就全部被顶点着色器处理，背对相机、画面外、视锥外的部分纯属浪费；
而且这条路只在 Forward 非 GPU-Culling 分支里有（Deferred 的 GBuffer 里实例网格只被当普通网格
画一次，等于没画实例）。

**做法：把剔除粒度下推到实例，并让绘制走间接命令**

| 步骤 | 内容 |
|---|---|
| ① 上传 | `InstanceCuller::UploadInstanceTransforms`：容量够就 Map 原地复用实例变换 SSBO（任务 23 的"环形化"），需要扩容才重建并把旧缓冲放进有界退役队列 |
| ② 剔除 | 新 compute `InstancedCull.comp.slang`：每实例把局部包围盒 8 角变换求世界 AABB → **六平面支持点测试** → 通过者 `InterlockedAdd` 拿位置、压缩写入**可见索引列表**，同时原子累加命令里的 `instanceCount` |
| ③ 绘制 | 全局屏障后 `DrawIndexedIndirect(cmd, 0, 1, 20)`；顶点着色器 `SV_InstanceID` 先查可见列表（`可见列表[i]`）再取实例变换 —— 只有通过的实例被处理 |
| ④ 双路径 | Forward 与 Deferred 的 GBuffer 都接同一套（上传/剔除/绘制三处共用一份实现） |

**设计要点（每条都有原因）**

1. **用 bindless SSBO 句柄寻址，而不是每组件更新描述符集**：Vulkan 描述符是**执行时**读取的，
   同一个命令缓冲里"改描述符 → dispatch A → 改描述符 → dispatch B"会让 A 用到 B 的缓冲。
   传句柄（`u_Instances[handle]`）彻底绕开这个问题，也是引擎里实例/骨骼数据一贯的做法。
2. **可见列表与命令缓冲都按飞行帧分槽**：单份会让"本帧 cull 写列表/清命令"与"上帧 GPU 仍在
   读列表/读命令做间接绘制"打架（表现为实例闪烁或整批消失）。命令缓冲由组件持有 3 份，
   可见列表由 `InstanceCuller` 持有 3 份。
3. **屏障**：cull 是 compute 写、draw 是 `DrawIndirect`+顶点着色器读，中间必须有
   `ComputeShader → (DrawIndirect|VertexShader)` 的 `UnorderedAccess → ShaderResource` 全局屏障。
4. **读回顺序**：命令里的 `instanceCount` 由 GPU 原子写、CPU 每帧清零 —— 必须**先读回上一帧的值
   再清零**。反过来（先清零后读）读到的永远是 0：这正是本轮第一次冒烟看到"可见实例恒 0"的原因，
   与剔除逻辑无关。
5. **保守余量**：平面测试取支撑点并留 1cm 余量（与 `GPUCull.comp.slang` 同参数），
   跨视锥边界的实例判为**可见**——宁可多画，不许误剔。
6. **单一真值来源**：实例变换上传逻辑抽成 `InstanceCuller::UploadInstanceTransforms`，
   Forward/Deferred 共用；否则两条路径的缓冲生命周期会各写一份、各自出错（本轮实测：
   Deferred 忘了建实例 SSBO 时句柄为 0，剔除 shader 读到空句柄 → 全部被剔，画面里一个实例都不剩）。

**判据**

- **doctest**：`Tests/TestInstancedMesh.cpp` 追加 3 例（全量 **197 例 / 5448 断言**全过）：
  · 组件状态（`enableFrustumCull` 默认关、命令缓冲按飞行帧分槽、可见计数初值 0、容量常量）；
  · **布局对齐**：`InstanceIndirectCommand` 20 字节且 5 个字段偏移 = VkDrawIndexedIndirectCommand；
    `InstancedCullParams` 144 字节且 7 个字段偏移与 `InstancedCull.comp.slang` 的 cbuffer 一致；
  · **剔除数学**：正前方可见 / 相机背后剔除 / 超远平面剔除 / 侧面很远剔除 / 跨近平面保守可见。
- **示例冒烟**（02.Cube，Release，Forward 与 Deferred 各 20 秒，均 0 error / 0 VUID）：
  · Forward：`[任务 25] 逐实例剔除：1 个实例网格，可见实例 3081 / 10000（剔除 69.2%）；CPU 参考复算 3081`
  · Deferred：`[任务 25] Deferred 实例化绘制首帧：实例 10000（对象 #22），剔除槽 1，实例 SSBO 句柄 6，命令句柄 7，可见列表句柄 4`
    → `[任务 25] 逐实例剔除：可见实例 3085 / 10000（剔除 69.2%）；CPU 参考复算 3086`
  · **GPU 与 CPU 参考复算逐帧一致**（示例用同一套平面测试在 CPU 上复算，只差"读回滞后一帧"的 0~1 个），
    这比"数值看起来合理"强得多：它证明 GPU 侧的实例矩阵变换、AABB 求取、平面判定与压缩写入都对；
  · 面板新增"逐实例 GPU 剔除 (任务 25)"开关与"可见 N / M 实例"读数，可现场 A/B。
- 校验层 0 告警：说明间接绘制屏障、描述符句柄、飞行帧分槽都正确。

**已知边界**

- **剔除只做视锥（六平面）**：没有 Hi-Z 遮挡剔除（逐物体那套 `GPUCull` 有，实例粒度上没有做）；
  视野内被墙挡住的实例仍会被画。要做需要按实例深度建/查 Hi-Z，成本与收益要另评估。
- **可见列表容量 100000 实例/组件**：超出部分不画（不是崩溃），超大批量需要分块或扩大容量。
- **命令缓冲的 CPU 复位依赖帧同步**：与 `GPUCulling` 的 DrawCount 同级做法（每飞行帧一份命令缓冲
  + `vkWaitForFences` 同槽位等待）。若要更严格，应改为"GPU 自己清零"或时间线信号量。
- **Deferred 的 GPU-Driven（间接批次）路径不含实例化**：实例化在 Deferred 走的是 GBuffer 渲染器的
  逐组件绘制段（CPU 模式与 GPU 模式的回退分支都接了）；批处理（MeshBatcher + ExecuteIndirect）
  那条路仍未把实例并入，属后续工作。

---

## 十九、任务 26 落地：Collision 调试线框（AABB / 球 / 胶囊）

承接 §十一 技术债④，也是本计划 `21~26` 的最后一项。**问题**：碰撞体是纯数据组件
（`CollisionComponent`，检测走 `CollisionSystem`），场景里只有一个"看不见的盒子"——
调试移动/物理问题时只能猜"到底检测的是哪个形状、多大、在哪"。

**做法：把碰撞形状三角化成线框网格，复用现有网格渲染路径**

| 步骤 | 内容 |
|---|---|
| ① 取形状 | `CollisionSystem::ExtractWorldShape`（**本轮把它从文件内部提升为公开 API**）：检测与可视化共用同一份世界形状语义 |
| ② 三角化 | 新 `CollisionDebugSystem::BuildWireframe`：AABB → 12 条棱；球 → 3 个正交大圆（3×24 段）；胶囊 → 上下圆（2×24）+ 4 条竖线 + 两端半球弧（8 弧 × 8 段）= 116 段 |
| ③ 落成几何 | 每段线生成**两片互相垂直的细四边形**（十字片，8 顶点 / 12 索引）写入同实体上的 `CollisionDebugComponent`（MeshComponent） |
| ④ 渲染 | 走现有网格路径：顶点/索引缓冲 + 材质（无光照 + 半透明 + 双面 + 不投影），Forward/Deferred 两条管线都自动生效 |

**设计要点（每条都有原因）**

1. **线与检测共用 `ExtractWorldShape`**：这是整个任务的价值前提 —— 如果线框自己再算一遍形状
   （尤其 AABB 的"旋转后重轴保守盒"、胶囊的"段半长 = max(0, h/2 − r)"、半径乘最大缩放分量），
   迟早与检测语义漂移，调试可视化就会骗人。为此把提取函数提升为公开 API，并补了注释说明。
2. **十字片而不是 billboard**：billboard 需要相机参数（系统调用方就得每帧传相机、还得处理朝向），
   两片互相垂直的细带在任意视角至少有一片接近正对，效果等价而调用面为零。
3. **不引入线拓扑管线**：RHI 里没有 LineList 管线，为调试线框新加一条（PSO/RenderPass/深度状态）
   性价比很低；而线框本质就是"带颜色的几何"，复用网格路径后连 Forward/Deferred 的分支都不用碰。
4. **形状不变就不重建网格**：组件缓存上一次的世界形状（min/max/center/radius/segA/segB + 形状类型
   + 线宽），`epsilonEqual` 比较后才 `SetMeshData` —— 否则每帧为每个碰撞体重建 GPU 缓冲纯属浪费。
5. **开关与禁用都收敛到"零几何"**：关闭开关或碰撞体 `bEnabled=false` 时清空网格（组件保留，便于再打开），
   与检测的"禁用即跳过"行为保持一致。
6. **调试组件空注册**：`CollisionDebugComponent` 在 `SceneReflect.cpp` 里只做空注册（让工厂/类型系统
   有 `StaticClass`），**不注册属性、不进 LLM 词表** —— 避免 AI 生成"调试线框"这种无意义实体。

**判据**

- **doctest**：`Tests/TestCollision.cpp` 追加 3 例（全量 **200 例 / 5482 断言**全过）：
  · AABB：12 段 / 96 顶点 / 144 索引，且**线框包围盒 == 检测用的世界 AABB ± 半线宽**（逐个分量的近似断言）；
  · 球：3×24 段，所有顶点到球心距离都在 `2 ± 0.02` 内（聚合 min/max 检查）；
  · 胶囊：`segA-segB` 长度 == `height − 2r`（3 − 1 = 2），竖直范围 == 总高 3，段数 = 2×24+4+8×8；
  · 退化输入（`radius=0`、`height<2r`、线宽 0）不产生 NaN；
  · 系统行为：打开 → 生成；形状没变 → 不重建；移动实体 → 线框跟随（包围盒随之变化）；
    关闭开关 → 几何清零且组件保留；碰撞体被禁用 → 线框消失。
- **示例冒烟**（02.Cube，Release，20 秒，**0 error / 0 VUID**）：
  `[任务 26] 碰撞调试线框：4 个碰撞体，共 212 条线段 / 1696 顶点（线与检测共用同一份世界形状）`
  —— 212 = 地板 AABB 12 + 碰撞盒 AABB 12 + 探测球 3×24 + 角色胶囊 116，与场景里的四个碰撞体逐一对上；
  面板新增"显示碰撞调试线框 (任务 26)"开关与"N 个碰撞体 / M 条线段"读数。

**已知边界**

- 线框走的是普通网格路径，因此**线宽是世界单位**（不会随距离变细），远处线框看起来会偏粗；
  想要屏幕恒定线宽需要真正的线段管线（LineList + 屏幕空间宽度）。
- 线框不参与遮挡剔除/深度前置（半透明路径），大量碰撞体时会有一定 overdraw；
  当前用法（几十个碰撞体）无所谓。
- 球/胶囊的圆环段数是固定常量（24 / 8），超大半径时能看出一圈折线；需要的话按半径自适应分段。
- 只在**示例/调用方**驱动（`CollisionDebugSystem::Update`），没有做成"编辑器全局开关"——
  编辑器侧接入属于编辑器自己的可视化层。

