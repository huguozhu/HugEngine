# HugEngine Entity Component 开发计划（对照 UE5）

> 日期：2026-09-01（初版）| 状态：✅ S0/P1/P2/P3 全部完成（2026-09-06）
> 修订记录：
>   - 2026-09-04：对齐代码基线（`404de09`），新增 **Phase S0（已有组件补齐 AI 一等公民）**
>   - 2026-09-06：S0~P3 全部落地（提交 `8813211` → `56696ea` 共 9 个），doctest 85 用例 / 509 断言通过；
>     剩余仅 Phase C 大工程（依赖路线图 P6/P3）
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
| SkeletalMesh / Physics / Audio / NavMesh | UE5 对应组件 | ❌（依赖路线图 P6/P3） | — | — | — |

**结论**：计划内组件全部落地。LLM 词表 5 → 10 组件（新增 SpotLight/RectLight/Camera/Health/Decal）；所有新组件按「一个组件 = 四件事」补齐类定义/反射/AI 注解/系统接入。剩余缺口：Phase C 大工程（SkeletalMesh/Physics/Audio/NavMesh，依赖路线图 P6/P3）；既有组件 Animation/Particle/Memory/Goal 的反射与 AI 注解未补齐（当前无阻断，归追溯清单待办）。

## 二、总体原则

1. **一个组件 = 四件事**：`Component` 类定义（`Engine/Scene/Scene/` 或 `Engine/AI/`）+ 反射注册（`SceneReflect.cpp` / `AgentReflect.cpp`）+ AI 注解（`HE_ATTR_AI_VISIBLE/WRITABLE/DESCRIPTION`）+ 系统接入（渲染/更新）。
2. **AI 可读写**：数值类属性全部打 `HE_ATTR_AI_*` 注解，并同步进 `SceneBuilder` 的 `TypeSchema` 词表——LLM 生成场景即刻可用（协议速查表 §六扩展规则）。
3. **反射自动生效**：注册后编辑器 Details 面板、WorldModel 快照、AI 动作（SetProperty）自动支持，无需额外代码。
4. **验证标配**：doctest 用例（组件属性读写/容错）+ 在 `05.AISamples`（已合并原 05~10）增加对应 Feature 或扩展现有场景。
5. **新组件必须追溯旧债**：每新增一个组件，先检查既有同名组件是否满足上述四件事（参照 Phase S0 补齐清单），避免"代码有、AI 看不见"的断链。

## 三、阶段规划

| 阶段 | 内容 | 预估成本 | 说明 |
|---|---|---|---|
| **S0（基线补齐）** | 已有组件补齐反射/AI 注解/词表/主相机接入 | 1~2 天 | ✅ 已完成（2026-09-06）：S0.1~S0.4 全部落地，doctest 38 用例通过 |
| **A（低成本）** | Camera(系统接入) / Decal / Billboard / TextRender / SpringArm / ProjectileMovement / Health | 2~3 天/个 | ✅ 已完成（2026-09-06）：A3~A8 落地；A1/A2 并入 S0 |
| **B（中成本）** | InstancedMesh / Spline / CharacterMovement / Ability(简化 GAS) / Collision | 1~2 周/个 | ✅ 已完成（2026-09-06）：B1~B5 落地；前置重构经评估非必要（见 §六注记） |
| **C（大工程）** | SkeletalMesh / Physics / Audio / NavMesh | 数周~数月 | 依赖路线图 P6/P3 或第三方库 |

---

## 四、Phase S0 详细设计（基线补齐，建议先行）

> 目的：兑现总则"一个组件 = 四件事"，让**已实现**组件立即可被编辑器与 AI 使用。

### S0.1 SpotLight 反射注册 + AI 注解（✅ 已完成 2026-09-06）

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

### S0.2 CameraComponent 反射注册 + AI 注解（✅ 已完成 2026-09-06）

- **改动**：`Engine/Scene/Scene/SceneReflect.cpp`（`HE_BEGIN_REGISTER(he::CameraComponent)` 空注册 → 注册属性）
- **注册属性**：`fov`（默认 60°）/ `nearPlane` / `farPlane` / `isMain`（bool，仅 AI_VISIBLE）——全部数值类加 AI 注解。
- **验证**：doctest + Details 面板可编辑。

### S0.3 LLM 词表与 SceneBuilder 扩展（5 组件 → 8 组件）（✅ 已完成 2026-09-06）

- **改动**：`Engine/AI/TypeSchema.cpp`（词表加 SpotLight/RectLight/Camera）；`Engine/AI/SceneBuilder.cpp`（`else if (type == ...)` 分支，安全降级：字段逐一类型检查、未知字段跳过）
- **词表新增**：
  ```json
  "SpotLight": {"fields": ["direction","color","intensity","range","innerConeAngle","outerConeAngle","castShadow"]},
  "RectLight": {"fields": ["normal","color","intensity","width","height","range","softness","castShadow"]},
  "Camera":    {"fields": ["fov","nearPlane","farPlane","isMain"]}
  ```
- **验证**：doctest（LLM 场景 JSON 含"一盏路灯"→ 生成 SpotLight 组件断言方向/锥角）；05.AISamples 冒烟（prompt「一个路灯照着的街角」）。

### S0.4 主相机系统接入（A1 的系统接入部分）（✅ 已完成 2026-09-06）

- **改动**：`Engine/Scene/Scene/World.h/.cpp` 新增 `GetPrimaryCamera()`（遍历 CameraComponent，返回首个 `isMain`）；各渲染管线帧入口优先取主相机组装 ViewMatrix，无相机实体时回退现有 `CameraController`。
- **现状**：`render::MakeCameraData(CameraComponent, Transform)`（`Pipeline/Camera.h:72`）已存在，缺的是"从 World 选主相机"这层。
- **验证**：02.Cube 或 05.AISamples 添加带 CameraComponent 的实体后视角生效；移除后回退 CameraController。

---

## 五、Phase A 详细设计（低成本）

> 状态注记：**A2 SpotLight 本体已完成**（光照/阴影/方向修复，`86856de`），剩余词表工作归 S0.3；
> **A1 CameraComponent 类已完成**，剩余系统接入归 S0.4。以下保留原始设计供参考。

### A1. CameraComponent（✅ 已完成 2026-09-06，注册归 S0.2、接入归 S0.4）

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

### A2. SpotLightComponent（✅ 已完成 2026-09-06，本体 `86856de` + S0.1 注册 + S0.3 词表）

- **对应 UE5**：USpotLightComponent
- **用途**：聚光灯（手电/路灯/舞台），LLM 场景生成高需求
- **属性**：`color / intensity / range / innerConeAngle / outerConeAngle / castShadow`（S0.1 打全 AI 注解）
- **系统接入**：✅ 已完成（Forward/Deferred 灯光收集 + 阴影 `SpotShadowTechnique`）
- **SceneBuilder 词表**：S0.3 补 `"SpotLight": {"fields": [...]}`
- **验证**：S0.3 的 LLM"一盏路灯"用例

### A3. DecalComponent（✅ 已完成 2026-09-06）

- **对应 UE5**：UDecalComponent
- **用途**：贴花（弹孔/污渍/路面标线），绘制到场景表面
- **属性**：`decalTexture(String 路径) / size / rotation / opacity / blendMode`
- **系统接入**：Deferred 管线贴花 Pass（GBuffer 修改）或前向投影盒绘制；MVP 可用简化版（半透明立方投射）
- **SceneBuilder 词表**：`"Decal": {"fields": ["decalTexture", "size", "opacity"]}`
- **验证**：贴花可见性 + 大小/透明度参数生效

### A4. BillboardComponent（✅ 已完成 2026-09-06）

- **对应 UE5**：UBillboardComponent
- **用途**：始终面向相机的四边形（粒子替代、UI 指示、调试标记）
- **属性**：`color / size / texturePath`
- **系统接入**：渲染管线每帧把 Billboard 的旋转对齐相机（计算 billboard 矩阵）
- **验证**：任意相机角度下四边形朝向正确

### A5. TextRenderComponent（✅ 已完成 2026-09-06）

- **对应 UE5**：UTextRenderComponent
- **用途**：3D 世界文字（标签/数值/调试）
- **属性**：`text(String) / color / size / fontPath`
- **系统接入**：MVP 用 CPU 生成文字纹理（stb_truetype 或位图字体）→ Billboard 渲染
- **验证**：实体名称/血量实时显示

### A6. SpringArmComponent（✅ 已完成 2026-09-06）

- **对应 UE5**：USpringArmComponent
- **用途**：第三人称相机跟随（延迟 + 平滑回弹 + 碰撞防穿墙）
- **属性**：`targetOffset / armLength / rotationLagSpeed / bUsePawnControlRotation`
- **系统接入**：每帧把目标 Transform + 弹簧臂长度合成相机 Transform（写入关联 CameraComponent，依赖 S0.4 主相机）
- **验证**：目标移动时相机平滑跟随，碰撞时缩短臂长

### A7. ProjectileMovementComponent（✅ 已完成 2026-09-06）

- **对应 UE5**：UProjectileMovementComponent
- **用途**：抛射物运动（直线/抛物线/命中回调/生命周期）
- **属性**：`initialSpeed / maxSpeed / gravityScale / bHoming / homingTarget`
- **系统接入**：系统级 `ProjectileSystem::Update`（每帧积分位置 + 超时销毁 + 命中事件）
- **验证**：doctest（给定初速/重力 → 位置符合抛物线）

### A8. HealthComponent（✅ 已完成 2026-09-06）

- **对应 UE5**：UHealthComponent（Gameplay 基础）
- **用途**：生命值/伤害/死亡事件（玩法数值底座）
- **属性**：`maxHealth / currentHealth / bInvincible`（AI 注解：LLM 可"给敌人 100 点血"）
- **系统接入**：纯数据组件 + `DamageSystem`（ApplyDamage 函数 + 事件回调）
- **验证**：doctest（扣血/回血/死亡边界）+ AI 生成"高血量守卫"用例

---

## 六、Phase B 详细设计（中成本，按需推进）

> 前置提示（已评估，2026-09-06）：原计划建议 B 组落地前做 CollectLights/词表数据驱动化重构。
> 实测评估结论：B 组无新光源类型（CollectLights 4 份复制不受影响）；B2~B5 为数据+系统组件，
> 走 4 处低成本路径；仅 B1 触渲染枚举（3 文件约 8 行一次性成本）——**重构非必要，直接落地**。
> 渲染枚举收敛为注册表留待第 8+ 个渲染类型出现时再做（规则三）；CollectLights 抽取留待下一个光源类型。

### B1. InstancedMeshComponent（✅ 已完成 2026-09-06）

- **对应 UE5**：UInstancedStaticMeshComponent / HISM（植被）
- **用途**：同一网格大量实例（草丛/森林/建筑群），单次 DrawIndexedIndirect
- **属性**：`meshPath / instanceCount / 实例变换数组（CPU 提供或程序化）`
- **系统接入**：复用 `MeshBatcher` + GPU Instancing（现有管线已有 InstanceBuffer 支持）；`enableFrustumCull` 按实例剔除
- **验证**：万级实例 FPS 对比

### B2. SplineComponent / SplineMeshComponent（SplineComponent ✅ 已完成 2026-09-06；SplineMesh 后续扩展）

- **对应 UE5**：USplineComponent / USplineMeshComponent
- **用途**：样条路径（道路/管线/摄像机轨道/Agent 巡逻路线）
- **属性**：`控制点数组（位置+切向）/ bClosedLoop / bShowPath`
- **系统接入**：`SplineSystem` 提供 `EvaluateAtDistance/GetTangent`；SplineMesh 沿样条生成 MeshSection
- **验证**：Agent 沿样条巡逻（与 AgentSystem 对接）

### B3. CharacterMovementComponent（✅ 已完成 2026-09-06）

- **对应 UE5**：UCharacterMovementComponent
- **用途**：角色移动（走/跑/跳/重力/地面检测）
- **属性**：`walkSpeed / runSpeed / jumpHeight / gravity / maxSlopeAngle`
- **系统接入**：`MovementSystem` 每帧积分（输入方向 + 重力 + 碰撞（依赖 Collision））；MVP 可无碰撞地面投影
- **验证**：角色在场景中可移动跳跃

### B4. AbilityComponent（简化 GAS）（✅ 已完成 2026-09-06）

- **对应 UE5**：UAbilitySystemComponent
- **用途**：技能注册/冷却/释放/消耗（智能体动作链对接点）
- **属性**：`技能列表（名称/冷却/消耗）/ 当前激活技能`
- **系统接入**：`AbilitySystem::Update`（冷却计时）+ Agent 动作 `CastAbility`（Action→Command 新增 op）
- **验证**：Agent 每 5 秒施放一次"火球"（SpawnEntity + 移动）

### B5. CollisionComponent（✅ 已完成 2026-09-06）

- **对应 UE5**：UCapsuleComponent / UBoxComponent（碰撞）
- **用途**：基础碰撞体积（AABB/Sphere/Capsule）——物理与移动的前置
- **属性**：`shape / halfExtents / radius / height / bEnabled`
- **系统接入**：`CollisionSystem`（CPU 检测：AABB-AABB/球-球/射线）；渲染调试线框（Billboard/线框 mesh）
- **验证**：doctest（相交/包含/射线命中）

---

## 七、Phase C 大工程（依赖路线图，仅列出）

| Component | 依赖 | 备注 |
|---|---|---|
| SkeletalMeshComponent | 骨骼动画系统（路线图 P6 缺项） | ✅ 已完成（2026-09-07，提交 `4d94460`）：glTF skins/动画解析 + GPU 蒙皮（骨骼 SSBO + 蒙皮 PSO）+ Fox 演示 |
| PhysicsComponent / RigidBody | 物理引擎集成（BEPU/PhysX/Jolt） | 刚体 + 约束 |
| AudioComponent | 音频系统（引擎尚无） | 3D 声源 + 衰减 |
| NavMesh 寻路 | 导航网格 + A*/Recast | Agent 移动寻路 |

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

> 追溯清单（Phase S0 复用）：对"已存在但未注册"的组件执行 2/5/7 三步即可补齐。

---

## 九、与 AI / Editor 的结合总结

- **LLM 生成**：词表 10 组件同步完成；已实测（真实 DeepSeek）：「一个路灯照着的街角」生成路灯几何 + 光源 + Camera 实体并驱动主相机；SpotLight/Decal/Health 等组件可被 LLM 生成
- **智能体动作**：`CastAbility` op 已落地（Action→Command 可撤销，doctest 覆盖执行/撤销）；SetProperty/SpawnEntity/SetTransform 原有 op 保持不变
- **编辑器**：所有新组件反射注册后自动获得 Details 面板编辑能力；`AgentInspector` 可显示 AI 可读字段
- **观察面板**：WorldModel 快照自动包含所有带 AI_VISIBLE 注解的新组件（Health 血量、CharacterMovement 参数等 LLM 大脑可见）

---

## 十、优先级建议（修订版）

```
P0（立即）: Phase S0 全部 —— ✅ 已完成（2026-09-06）
             S0.1 SpotLight 反射+AI 注解
             S0.2 Camera 反射+AI 注解
             S0.3 词表 + SceneBuilder 扩到 8 组件
             S0.4 主相机接入（World::GetPrimaryCamera / ResolveFrameCamera）
P1（随后）: ✅ 已完成（2026-09-06）—— A7 ProjectileMovement + A8 Health（玩法底座）+ A6 SpringArm
P2（视需要）: ✅ 已完成（2026-09-06）—— A3 Decal（MVP 投射片）/ A4 Billboard / A5 TextRender
P3（中成本）: ✅ 已完成（2026-09-06）—— B5 Collision → B3 CharacterMovement → B4 Ability → B2 Spline → B1 InstancedMesh
P4（大工程）: Phase C（等待路线图）
```

---

## 十一、完成总结（2026-09-06）

- **组件总数**：新增 11 个组件（A 组 6：Decal/Billboard/TextRender/SpringArm/ProjectileMovement/Health；B 组 5：Collision/CharacterMovement/Ability/Spline/InstancedMesh）+ S0 补齐 3 个既有组件（SpotLight/RectLight/Camera）的反射/词表 + 主相机接入（GetPrimaryCamera/ResolveFrameCamera）；doctest **85 用例 / 509 断言**全部通过
- **词表**：LLM 组件词表 5 → 10（SpotLight/RectLight/Camera/Health/Decal 新增），SceneBuilder 全部带安全降级解析
- **提交序列**：`8813211`（S0）→ `7683686`（P1）→ `3eb963a`（P2）→ `8299254`/`11e4f14`/`1ae6637`/`3c0f571`/`56696ea`（P3 五件）
- **交互验证**：02.Cube（广告牌/文字/贴花/碰撞变色/角色移动/万级实例）、05.AISamples（真实 LLM 场景生成、主相机、火球技能、样条巡逻）
- **已知 MVP 限制（技术债清单）**：
  1. bindless 堆 append-only：TextRender/InstancedMesh 动态更新以「旧资源保活」换安全，高频更新需 Heap 环形化改造
  2. Decal 为投射片 MVP（无 GBuffer 投影 Pass）；InstancedMesh 仅支持 Forward 非 GPU-Culling 路径，逐实例剔除未接
  3. 实体引用类属性（homingTarget/targetEntity/cameraEntity 等）不进反射/词表（u64 无法快照序列化）
  4. maxSlopeAngle（坡度过滤）、SpringArm 碰撞缩臂、Collision 调试线框、SplineMesh 沿样条生成等预留未接
- **遗留**：Phase C 剩余（Physics/Audio/NavMesh，依赖路线图 P6/P3）；SkeletalMesh 后续扩展（剪辑混合、动画重定向、SplineMesh 沿样条生成）；既有组件 Animation/Particle/Memory/Goal 的反射/AI 补齐
