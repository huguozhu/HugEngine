# HugEngine Entity Component 开发计划（对照 UE5）

> 日期：2026-09-01（初版）| 状态：✅ S0/P1/P2/P3 全部完成（2026-09-06）
> 修订记录：
>   - 2026-09-04：对齐代码基线（`404de09`），新增 **Phase S0（已有组件补齐 AI 一等公民）**
>   - 2026-09-06：S0~P3 全部落地（提交 `8813211` → `56696ea` 共 9 个），doctest 85 用例 / 509 断言通过；
>     剩余仅 Phase C 大工程（依赖路线图 P6/P3）
>   - 2026-09-18：任务 22（动画重定向）落地（提交 `98b94d4` + 演示 `6c3f33e`），见 §十五
>   - 2026-09-18：任务 21（骨骼剪辑混合）落地（提交 `af86387` + 演示 `2b07df7`），见 §十四
>   - 2026-09-18：**任务统一编号（从 1 开始）** —— 原先用阶段前缀编号（S0.1 / A7 / B3 / C2），
>     跨阶段无法一眼看出"总共有多少任务、下一个做哪个"。现在全部任务从 1 连续编号（**1~20 已落地，
>     21~28 待办**），**原编号一律保留在括号里**作为别名（代码注释里引用的是原编号，靠 §三 的任务
>     总表对照）。总表见 **§三 · 任务总表**，各处小节标题同步改为新编号。
> 目标：补齐 UE5 Actor 组件体系在 HugEngine 的对应实现；**每个组件（含已有组件）都是 AI 可读写的一等公民**（`HE_ATTR_AI_*` 注解 + SceneBuilder 词表同步），并自动获得编辑器 Details 面板的反射编辑能力。

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
| DecalComponent | UDecalComponent | ✅（P2 A3，MVP 半透明投射片） | ✅ 5 属性 | ✅ | ✅ Decal |
| BillboardComponent | UBillboardComponent | ✅（P2 A4，billboard 矩阵对齐相机） | ✅ 3 属性 | ✅ | — |
| TextRenderComponent | UTextRenderComponent | ✅（P2 A5，stb_truetype + 系统字体兜底） | ✅ 4 属性 | ✅ | — |
| CollisionComponent | UCapsuleComponent/UBoxComponent | ✅（P3 B5，`CollisionSystem`） | ✅ 5 属性 | ✅ | — |
| CharacterMovementComponent | UCharacterMovementComponent | ✅（P3 B3，`MovementSystem`，地面射线检测） | ✅ 5 属性 | ✅ | — |
| AbilityComponent | UAbilitySystemComponent | ✅（P3 B4，`AbilitySystem` + Action op CastAbility） | ✅ 2 属性 | ✅ | — |
| SplineComponent | USplineComponent | ✅（P3 B2，Hermite+自动切线，弧长求值/闭环回绕） | ✅ 2 属性 | ✅ | — |
| InstancedMeshComponent | UInstancedStaticMeshComponent | ✅（P3 B1，单次 DrawIndexed 万级实例） | ✅ 2 属性 | ✅ | — |
| SkeletalMesh / Physics / NavMesh | UE5 对应组件 | ✅ 均已落地（SkeletalMesh `4d94460` / Physics Jolt C2 / NavMesh `97aaa00`）；SkeletalMesh 于 2026-09-18 追加**剪辑混合**（任务 21，`af86387`，§十四）与**动画重定向**（任务 22，`98b94d4`，§十五） | — | — | — |

**结论**：计划内组件全部落地。LLM 词表 5 → 10 组件（新增 SpotLight/RectLight/Camera/Health/Decal）；所有新组件按「一个组件 = 四件事」补齐类定义/反射/AI 注解/系统接入。Phase C 的后续扩展（SkeletalMesh 剪辑混合/动画重定向）已于 2026-09-18 补齐（任务 21/22）；Audio（C3）已从本计划移除。既有组件 Animation/Particle/Memory/Goal 的反射与 AI 注解 ✅ 已补齐。

## 二、总体原则

1. **一个组件 = 四件事**：`Component` 类定义（`Engine/Scene/Scene/` 或 `Engine/AI/`）+ 反射注册（`SceneReflect.cpp` / `AgentReflect.cpp`）+ AI 注解（`HE_ATTR_AI_VISIBLE/WRITABLE/DESCRIPTION`）+ 系统接入（渲染/更新）。
2. **AI 可读写**：数值类属性全部打 `HE_ATTR_AI_*` 注解，并同步进 `SceneBuilder` 的 `TypeSchema` 词表——LLM 生成场景即刻可用（协议速查表 §六扩展规则）。
3. **反射自动生效**：注册后编辑器 Details 面板、WorldModel 快照、AI 动作（SetProperty）自动支持，无需额外代码。
4. **验证标配**：doctest 用例（组件属性读写/容错）+ 在 `05.AISamples`（已合并原 05~10）增加对应 Feature 或扩展现有场景。
5. **新组件必须追溯旧债**：每新增一个组件，先检查既有同名组件是否满足上述四件事（参照 Phase S0 补齐清单），避免"代码有、AI 看不见"的断链。

## 三、阶段规划

| 阶段 | 内容 | 预估成本 | 说明 |
|---|---|---|---|
| **S0（基线补齐）** | 已有组件补齐反射/AI 注解/词表/主相机接入 | 1~2 天 | ✅ 已完成（2026-09-06）：S0.1~S0.4 全部落地（**现编号 1~4**），doctest 38 用例通过 |
| **A（低成本）** | Camera(系统接入) / Decal / Billboard / TextRender / SpringArm / ProjectileMovement / Health | 2~3 天/个 | ✅ 已完成（2026-09-06）：A3~A8 落地（**现编号 7~12**）；A1/A2 并入 S0（**现编号 5~6**） |
| **B（中成本）** | InstancedMesh / Spline / CharacterMovement / Ability(简化 GAS) / Collision | 1~2 周/个 | ✅ 已完成（2026-09-06）：B1~B5 落地（**现编号 13~17**）；前置重构经评估非必要（见 §六注记） |
| **C（大工程）** | SkeletalMesh / Physics / NavMesh | 数周~数月 | ✅ 均已落地（**现编号 18~20**；SkeletalMesh `4d94460` / Physics Jolt C2 / NavMesh `97aaa00`）；Audio（原 C3）已移除 |
| **后续（待办）** | SkeletalMesh 扩展 / MVP 技术债 / 架构触发项 | 见总表 | ⏳ **现编号 21~28**（21、22 已于 2026-09-18 完成；23~28 待办），见下表与 §十二 |

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

**B. 待办（21~28）** —— 来源：§十一 已知 MVP 限制/技术债 + §十二 "仍待办" + §十二 架构触发项
（**21、22 已于 2026-09-18 完成**，保留在表里以便追溯）

| 新编号 | 任务 | 类型 | 依赖 / 触发条件 | 详见 |
|:---:|---|---|---|---|
| **21** | ~~SkeletalMesh **剪辑混合**（多动画片段混合 / Blend Space）~~ —— ✅ **已完成**（2026-09-18，`af86387` + 演示 `2b07df7`） | 功能扩展 | 无（独立） | §十四 |
| **22** | ~~SkeletalMesh **动画重定向**（不同骨架共用动画）~~ —— ✅ **已完成**（2026-09-18，`98b94d4` + 演示 `6c3f33e`） | 功能扩展 | 无（独立） | §十五 |
| **23** | **bindless 堆环形化**（TextRender / InstancedMesh 高频更新不再靠"旧资源保活"） | 技术债 | 无（独立，涉 RHI 堆管理） | §十一 ①②/§十二 |
| **24** | **Decal GBuffer 投影 Pass**（替代半透明投射片 MVP） | 技术债 → 功能 | 无（独立，需 GBuffer 可写 Pass） | §十一 ②/§十二 |
| **25** | **InstancedMesh 接 GPU-Culling + 逐实例剔除**（现在仅 Forward 非 GPU-Culling 路径） | 技术债 | 无（独立） | §十一 ②/§十二 |
| **26** | **Collision 调试线框**（可视化 AABB/球/胶囊） | 技术债 | 无（独立，可复用 Billboard/线框） | §十一 ④/§十二 |
| **27** | **渲染类型注册表化**（替换"派生渲染组件显式列举"） | 架构 | **触发**：出现下一个渲染组件（InstancedMesh 是第 7 个） | §十二 |
| **28** | **CollectLights 数据驱动抽取**（现在 4 份复制） | 架构 | **触发**：出现下一个光源类型 | §十二 |

> **怎么用这张表**：`23~26` 无外部依赖、可随时开工（建议顺序：26 → 25 → 24 → 23，
> 由"改动面小 → 大"排列）；`27/28` 是**触发式**任务，条件未到之前不动（提前做属于"没有消费方的
> 泛化"，与 GI 那边的取舍一致）。每完成一项：把状态改成 ✅ 并补提交号，**不要改动已有编号**。

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
- **验证**：doctest（LLM 场景 JSON 含"一盏路灯"→ 生成 SpotLight 组件断言方向/锥角）；05.AISamples 冒烟（prompt「一个路灯照着的街角」）。

### 4. 主相机系统接入（原 S0.4，= 任务 5 的系统接入部分；✅ 已完成 2026-09-06）

- **改动**：`Engine/Scene/Scene/World.h/.cpp` 新增 `GetPrimaryCamera()`（遍历 CameraComponent，返回首个 `isMain`）；各渲染管线帧入口优先取主相机组装 ViewMatrix，无相机实体时回退现有 `CameraController`。
- **现状**：`render::MakeCameraData(CameraComponent, Transform)`（`Pipeline/Camera.h:72`）已存在，缺的是"从 World 选主相机"这层。
- **验证**：02.Cube 或 05.AISamples 添加带 CameraComponent 的实体后视角生效；移除后回退 CameraController。

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
- **验证**：doctest（创建相机实体 → 取主相机）+ 05.AISamples 场景生成"带相机"的关卡

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
- **属性**：`decalTexture(String 路径) / size / rotation / opacity / blendMode`
- **系统接入**：Deferred 管线贴花 Pass（GBuffer 修改）或前向投影盒绘制；MVP 可用简化版（半透明立方投射）
- **SceneBuilder 词表**：`"Decal": {"fields": ["decalTexture", "size", "opacity"]}`
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
7. [ ] `05.AISamples`：LLM 生成含该组件的场景用例验证（或独立 Feature）
8. [ ] 中文注释齐备

> 追溯清单（任务 1~4 的做法复用）：对"已存在但未注册"的组件执行 2/5/7 三步即可补齐。

---

## 九、与 AI / Editor 的结合总结

- **LLM 生成**：词表 10 组件同步完成；已实测（真实 DeepSeek）：「一个路灯照着的街角」生成路灯几何 + 光源 + Camera 实体并驱动主相机；SpotLight/Decal/Health 等组件可被 LLM 生成
- **智能体动作**：`CastAbility` op 已落地（Action→Command 可撤销，doctest 覆盖执行/撤销）；SetProperty/SpawnEntity/SetTransform 原有 op 保持不变
- **编辑器**：所有新组件反射注册后自动获得 Details 面板编辑能力；`AgentInspector` 可显示 AI 可读字段
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
待办        : 21 ✅ / 22 ✅ 已完成；23~26（无依赖，建议 26 → 25 → 24 → 23）
              27/28（触发式：出现下一个渲染组件 / 下一个光源类型时再做）
```

---

## 十一、完成总结（2026-09-06）

- **组件总数**：新增 11 个组件（A 组 6：Decal/Billboard/TextRender/SpringArm/ProjectileMovement/Health；B 组 5：Collision/CharacterMovement/Ability/Spline/InstancedMesh）+ S0 补齐 3 个既有组件（SpotLight/RectLight/Camera）的反射/词表 + 主相机接入（GetPrimaryCamera/ResolveFrameCamera）；doctest **85 用例 / 509 断言**全部通过（2026-09-06 基线；C1 SkeletalMesh 见 §十二）
- **词表**：LLM 组件词表 5 → 10（SpotLight/RectLight/Camera/Health/Decal 新增，截至 2026-09-06），后 Animation 补齐（`98c4080`）→ **11 种**，RigidBody 补齐（`1d4de6e`，Jolt C2）→ **12 种**，NavMesh/NavAgent 补齐（`97aaa00`，C4）→ **14 种**；SceneBuilder 全部带安全降级解析
- **提交序列**：`8813211`（S0）→ `7683686`（P1）→ `3eb963a`（P2）→ `8299254`/`11e4f14`/`1ae6637`/`3c0f571`/`56696ea`（P3 五件）
- **交互验证**：02.Cube（广告牌/文字/贴花/碰撞变色/角色移动/万级实例）、05.AISamples（真实 LLM 场景生成、主相机、火球技能、样条巡逻）
- **已知 MVP 限制（技术债清单，均已成任务，见 §三 任务总表 21~26）**：
  1. bindless 堆 append-only：TextRender/InstancedMesh 动态更新以「旧资源保活」换安全，高频更新需 Heap 环形化改造（**任务 23**）
  2. Decal 为投射片 MVP（无 GBuffer 投影 Pass，**任务 24**）；InstancedMesh 仅支持 Forward 非 GPU-Culling 路径，逐实例剔除未接（**任务 25**）
  3. 实体引用类属性（homingTarget/targetEntity/cameraEntity 等）不进反射/词表（u64 无法快照序列化）——**这是策略而非待办**
  4. Collision 调试线框（**任务 26**）、SplineMesh 沿条生成 ✅ 已落地（`28ae06b` 坡度过滤、`bc14c57` 碰撞缩臂亦已落地）
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

**仍待办（已编号，见 §三 任务总表 21~28）**：
- ✅ 任务 18~20 已全部落地（SkeletalMesh / Physics C2 / NavMesh C4）；Audio（原 C3）已从本计划移除
- ✅ **21** SkeletalMesh 剪辑混合（多片段混合 / Blend Space）—— 已完成（2026-09-18，`af86387` + 演示 `2b07df7`，见 §十四）
- ✅ **22** SkeletalMesh 动画重定向（不同骨架共用动画）—— 已完成（2026-09-18，`98b94d4` + 演示 `6c3f33e`，见 §十五）
- **23** bindless 堆环形化（TextRender/InstancedMesh 高频更新）
- **24** Decal GBuffer 投影 Pass（替代投射片 MVP）
- **25** InstancedMesh 接 GPU-Culling + 逐实例剔除
- **26** Collision 调试线框
- **27** 渲染类型注册表化（**触发**：下一个渲染组件；InstancedMesh 是第 7 个）
- **28** CollectLights 数据驱动抽取（**触发**：下一个光源类型）
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

