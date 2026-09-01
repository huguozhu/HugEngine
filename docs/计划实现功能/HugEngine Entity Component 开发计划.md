# HugEngine Entity Component 开发计划（对照 UE5）

> 日期：2026-09-01 | 状态：计划（待评审）
> 目标：补齐 UE5 Actor 组件体系在 HugEngine 的对应实现；**每个新组件从第一天起就是 AI 可读写的一等公民**（`HE_ATTR_AI_*` 注解 + SceneBuilder 词表同步），并自动获得编辑器 Details 面板的反射编辑能力。

---

## 一、现状盘点

| HugEngine 现有组件 | 对应 UE5 | 备注 |
|---|---|---|
| TransformComponent | USceneComponent | 位置/旋转/缩放，AI 注解已打 |
| MeshComponent（Cube/Sphere） | UStaticMeshComponent | baseColor/metallic/roughness/emissive |
| LightComponent（DirectionalLight/PointLight） | ULightComponent | 缺 SpotLight / RectLight |
| PhysicalSkyComponent | 引擎级天空 | 与 UE5 天空组件同构 |
| AnimationComponent | 简化动画 | Transform 关键帧（UE5 = Skeletal + AnimBP） |
| ParticleComponent | Niagara 简化 | GPU 粒子（Init/Emit/Simulate） |
| AgentComponent / MemoryComponent / GoalComponent | （UE5 无直接对应） | AI 智能体 = Pawn + AI Controller + BehaviorTree |

## 二、总体原则

1. **一个组件 = 四件事**：`Component` 类定义（`Engine/Scene/Scene/`）+ 反射注册（`SceneReflect.cpp` / `AgentReflect.cpp`）+ AI 注解（`HE_ATTR_AI_VISIBLE/WRITABLE/DESCRIPTION`）+ 系统接入（渲染/更新）。
2. **AI 可读写**：数值类属性全部打 `HE_ATTR_AI_*` 注解，并同步进 `SceneBuilder` 的 `TypeSchema` 词表——LLM 生成场景即刻可用（协议速查表 §六扩展规则）。
3. **反射自动生效**：注册后编辑器 Details 面板、WorldModel 快照、AI 动作（SetProperty）自动支持，无需额外代码。
4. **验证标配**：doctest 用例（组件属性读写/容错）+ 在 `05.AISamples` 增加对应 Feature 或扩展现有场景。

## 三、阶段规划

| 阶段 | 组件 | 预估成本 | 说明 |
|---|---|---|---|
| **A（低成本）** | Camera / SpotLight / Decal / Billboard / TextRender / SpringArm / ProjectileMovement / Health | 2~3 天/个 | 独立属性 + 少量系统代码，AI 收益最大 |
| **B（中成本）** | InstancedMesh / Spline / CharacterMovement / Ability(简化 GAS) / Collision | 1~2 周/个 | 涉及渲染实例化/移动物理/技能循环 |
| **C（大工程）** | SkeletalMesh / Physics / Audio / NavMesh | 数周~数月 | 依赖路线图 P6/P3 或第三方库 |

---

## 四、Phase A 详细设计（低成本，建议先行）

### A1. CameraComponent

- **对应 UE5**：UCameraComponent
- **用途**：相机作为 Entity 属性（替代全局 CameraController），多相机切换、过场、AI 观察视角
- **属性（反射 + AI 注解）**：
  | 属性 | 类型 | AI 注解 |
  |---|---|---|
  | fov | float（默认 60°） | AI_VISIBLE + AI_WRITABLE |
  | nearPlane / farPlane | float | AI_VISIBLE + AI_WRITABLE |
  | isPrimary | bool | AI_VISIBLE |
- **系统接入**：`Render` 管线读 `World::GetPrimaryCamera()`（首个 isPrimary 相机）组装 ViewMatrix；无相机实体时回退 CameraController
- **SceneBuilder 词表**：`"Camera": {"fields": ["fov", "nearPlane", "farPlane"]}`
- **验证**：doctest（创建相机实体 → 取主相机）+ 05.AISamples 场景生成"带相机"的关卡

### A2. SpotLightComponent

- **对应 UE5**：USpotLightComponent
- **用途**：聚光灯（手电/路灯/舞台），LLM 场景生成高需求
- **属性**：`color / intensity / range / innerConeAngle / outerConeAngle / castShadow`（全部 AI 注解）
- **系统接入**：前向/延迟管线的灯光收集（现有 LightComponent 列表扩展，ForwardPipeline 的灯光 SSBO 加聚光字段——锥角/方向衰减）
- **SceneBuilder 词表**：`"SpotLight": {"fields": [...]}`
- **验证**：渲染验证（锥形光照可见）+ AI 生成"一盏路灯"用例

### A3. DecalComponent

- **对应 UE5**：UDecalComponent
- **用途**：贴花（弹孔/污渍/路面标线），绘制到场景表面
- **属性**：`decalTexture(String 路径) / size / rotation / opacity / blendMode`
- **系统接入**：Deferred 管线贴花 Pass（GBuffer 修改）或前向投影盒绘制；MVP 可用简化版（半透明立方投射）
- **SceneBuilder 词表**：`"Decal": {"fields": ["decalTexture", "size", "opacity"]}`
- **验证**：贴花可见性 + 大小/透明度参数生效

### A4. BillboardComponent

- **对应 UE5**：UBillboardComponent
- **用途**：始终面向相机的四边形（粒子替代、UI 指示、调试标记）
- **属性**：`color / size / texturePath`
- **系统接入**：渲染管线每帧把 Billboard 的旋转对齐相机（计算 billboard 矩阵）
- **验证**：任意相机角度下四边形朝向正确

### A5. TextRenderComponent

- **对应 UE5**：UTextRenderComponent
- **用途**：3D 世界文字（标签/数值/调试）
- **属性**：`text(String) / color / size / fontPath`
- **系统接入**：MVP 用 CPU 生成文字纹理（stb_truetype 或位图字体）→ Billboard 渲染
- **验证**：实体名称/血量实时显示

### A6. SpringArmComponent

- **对应 UE5**：USpringArmComponent
- **用途**：第三人称相机跟随（延迟 + 平滑回弹 + 碰撞防穿墙）
- **属性**：`targetOffset / armLength / rotationLagSpeed / bUsePawnControlRotation`
- **系统接入**：每帧把目标 Transform + 弹簧臂长度合成相机 Transform（写入关联 CameraComponent）
- **验证**：目标移动时相机平滑跟随，碰撞时缩短臂长

### A7. ProjectileMovementComponent

- **对应 UE5**：UProjectileMovementComponent
- **用途**：抛射物运动（直线/抛物线/命中回调/生命周期）
- **属性**：`initialSpeed / maxSpeed / gravityScale / bHoming / homingTarget`
- **系统接入**：系统级 `ProjectileSystem::Update`（每帧积分位置 + 超时销毁 + 命中事件）
- **验证**：doctest（给定初速/重力 → 位置符合抛物线）

### A8. HealthComponent

- **对应 UE5**：UHealthComponent（Gameplay 基础）
- **用途**：生命值/伤害/死亡事件（玩法数值底座）
- **属性**：`maxHealth / currentHealth / bInvincible`（AI 注解：LLM 可"给敌人 100 点血"）
- **系统接入**：纯数据组件 + `DamageSystem`（ApplyDamage 函数 + 事件回调）
- **验证**：doctest（扣血/回血/死亡边界）+ AI 生成"高血量守卫"用例

---

## 五、Phase B 详细设计（中成本，按需推进）

### B1. InstancedMeshComponent

- **对应 UE5**：UInstancedStaticMeshComponent / HISM（植被）
- **用途**：同一网格大量实例（草丛/森林/建筑群），单次 DrawIndexedIndirect
- **属性**：`meshPath / instanceCount / 实例变换数组（CPU 提供或程序化）`
- **系统接入**：复用 `MeshBatcher` + GPU Instancing（现有管线已有 InstanceBuffer 支持）；`enableFrustumCull` 按实例剔除
- **验证**：万级实例 FPS 对比

### B2. SplineComponent / SplineMeshComponent

- **对应 UE5**：USplineComponent / USplineMeshComponent
- **用途**：样条路径（道路/管线/摄像机轨道/Agent 巡逻路线）
- **属性**：`控制点数组（位置+切向）/ bClosedLoop / bShowPath`
- **系统接入**：`SplineSystem` 提供 `EvaluateAtDistance/GetTangent`；SplineMesh 沿样条生成 MeshSection
- **验证**：Agent 沿样条巡逻（与 AgentSystem 对接）

### B3. CharacterMovementComponent

- **对应 UE5**：UCharacterMovementComponent
- **用途**：角色移动（走/跑/跳/重力/地面检测）
- **属性**：`walkSpeed / runSpeed / jumpHeight / gravity / maxSlopeAngle`
- **系统接入**：`MovementSystem` 每帧积分（输入方向 + 重力 + 碰撞（依赖 Collision））；MVP 可无碰撞地面投影
- **验证**：角色在场景中可移动跳跃

### B4. AbilityComponent（简化 GAS）

- **对应 UE5**：UAbilitySystemComponent
- **用途**：技能注册/冷却/释放/消耗（智能体动作链对接点）
- **属性**：`技能列表（名称/冷却/消耗）/ 当前激活技能`
- **系统接入**：`AbilitySystem::Update`（冷却计时）+ Agent 动作 `CastAbility`（Action→Command 新增 op）
- **验证**：Agent 每 5 秒施放一次"火球"（SpawnEntity + 移动）

### B5. CollisionComponent

- **对应 UE5**：UCapsuleComponent / UBoxComponent（碰撞）
- **用途**：基础碰撞体积（AABB/Sphere/Capsule）——物理与移动的前置
- **属性**：`shape / halfExtents / radius / height / bEnabled`
- **系统接入**：`CollisionSystem`（CPU 检测：AABB-AABB/球-球/射线）；渲染调试线框（Billboard/线框 mesh）
- **验证**：doctest（相交/包含/射线命中）

---

## 六、Phase C 大工程（依赖路线图，仅列出）

| Component | 依赖 | 备注 |
|---|---|---|
| SkeletalMeshComponent | 骨骼动画系统（路线图 P6 缺项） | glTF 骨骼 + 蒙皮 + AnimBP 简化 |
| PhysicsComponent / RigidBody | 物理引擎集成（BEPU/PhysX/Jolt） | 刚体 + 约束 |
| AudioComponent | 音频系统（引擎尚无） | 3D 声源 + 衰减 |
| NavMesh 寻路 | 导航网格 + A*/Recast | Agent 移动寻路 |

---

## 七、新增组件的开发检查清单（规范）

1. [ ] `Engine/Scene/Scene/<Name>Component.h/.cpp`：类定义 + `HE_COMPONENT()` + `OnCreate`（默认值）
2. [ ] 反射注册：`SceneReflect.cpp` 加 `HE_REGISTER_PROPERTY(<Name>Component, ...)` + `HE_END_PROPERTY()`，每个属性打 `HE_ATTR_AI_VISIBLE()`；数值类打 `HE_ATTR_AI_WRITABLE()` + `HE_ATTR_AI_DESCRIPTION(中文说明)`
3. [ ] 需要每帧更新的：新建 `<Name>System`（静态 Update，仿 `AgentSystem`）或挂进现有管线子系统
4. [ ] 渲染相关的：接入 Forward/Deferred 管线（灯光收集/绘制/材质）
5. [ ] `SceneBuilder`：`TypeSchema` 词表加类型与字段 + `BuildScene` 加 `else if (type == "<Name>")` 分支（安全降级）
6. [ ] `Tests/`：doctest 用例（组件创建/属性读写/容错）
7. [ ] `05.AISamples`：LLM 生成含该组件的场景用例验证（或独立 Feature）
8. [ ] 中文注释齐备

## 八、与 AI / Editor 的结合总结

- **LLM 生成**：词表同步后，"一个手电筒照着的山洞"（SpotLight）、"门口有贴花的仓库"（Decal）即可生成
- **智能体动作**：`Action` 增加 `CastAbility`/`SetHealth` 等 op，走 Action→Command 可撤销
- **编辑器**：反射注册自动获得 Details 编辑；`AgentInspector` 面板可显示新组件的 AI 可读字段
- **观察面板**：WorldModel 快照自动包含新组件（带 AI_VISIBLE 注解的属性）

## 九、优先级建议

```
P0（立即）: A1 CameraComponent + A2 SpotLightComponent（低成本 + LLM 场景生成立刻受益）
P1（随后）: A7 ProjectileMovement + A8 Health（玩法底座）+ A6 SpringArm（第三人称）
P2（视需要）: A3 Decal / A4 Billboard / A5 TextRender
P3（中成本）: B1 InstancedMesh → B2 Spline → B5 Collision → B3 CharacterMovement → B4 Ability
P4（大工程）: Phase C（等待路线图）
```
