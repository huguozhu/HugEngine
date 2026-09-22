# HugEngine Entity-Component 架构与组件功能

> 定位：本文是**引擎侧 Entity-Component（ECS 风格场景对象模型）的现状说明** —— 讲清
> 「组件模型怎么组织」「每种组件做什么、有哪些参数、由谁驱动」以及「渲染/编辑器/AI 三条消费者链路
> 如何使用这些组件」。计划与逐任务的落地记录见 `docs/已实现功能/HugEngine Entity Component 架构与开发计划.md`。
>
> 日期：2026-09-18 | 最后更新：2026-09-21 | 状态：与代码基线一致（可挂载组件 33 项、反射注册 36 个类 /
> 104 个属性、LLM 词表 14 种类型；计划内任务 1~26 全部完成）。

---

## 一、设计总览

HugEngine 的场景对象模型是**面向数据的组件式架构**（不是缓存友好的 SoA archetype ECS，而更接近
UE 的 Actor-Component，只是把"组件存储"做成按类型分桶的稀疏集合）：

| 概念 | 引擎实现 | 对应 UE5 | 说明 |
|---|---|---|---|
| 世界 | `he::World`（`Engine/Scene/Scene/World.h`） | `UWorld` | 实体注册表 + 按类型分桶的组件存储 + 遍历入口 |
| 实体 | `he::Entity`（`Scene/Entity.h`，单个 `EntityID`(u64)） | `AActor` | 只是一个稳定句柄；**没有**自己的数据，数据全在组件里 |
| 组件 | `he::Component` 及其派生（`Scene/*Component.h`） | `UActorComponent` / `USceneComponent` | 数据 + 少量自更新逻辑（`OnUpdate`）；一个实体同类型组件最多一个 |
| 层级 | `he::SceneGraph`（`Scene/SceneGraph.h`） | `USceneComponent` 的 attach 树 | 父子关系 + 世界矩阵求解（变换不在组件里做） |
| 系统 | 各 `*System` 静态类（`Scene/*System.h`、`AI/Agent/AgentSystem.h`、`Physics/PhysicsSystem.h`） | `FTickFunction` / 子系统 | 每帧由宿主（示例/编辑器主循环）按固定顺序调用 |
| 反射 | `Engine/Reflect`（`ClassInfo` / `PropertyInfo` / 属性注解） | `UProperty` + 反射 | 编辑器 Details 面板与 AI 读写的共同基础 |
| 渲染消费 | `he::render::SceneRenderer::Prepare` + 各渲染 Pass | 场景代理（SceneProxy） | 组件 → `DrawItem` / `GPUObjectData` → GPU |

三条消费者链路都建立在同一份组件数据上：

```
                       ┌───────────────────────────────┐
   宿主主循环 ────────► │  System::Update(world, ...)   │  物理/移动/动画/寻路/文字/线框…
                       └───────────────┬───────────────┘
                                       │ 写回组件
   ┌───────────────────────────────────▼───────────────────────────────────┐
   │                          World（实体 + 组件存储）                       │
   └───────┬───────────────────────────┬───────────────────────────┬───────┘
           │                           │                           │
   ┌───────▼────────┐        ┌─────────▼─────────┐       ┌─────────▼─────────┐
   │ 渲染            │        │ 编辑器             │       │ AI                 │
   │ SceneRenderer   │        │ 反射属性 → Details │       │ TypeSchema 词表    │
   │ → GPUObjectData │        │ 面板编辑          │       │ + SceneBuilder     │
   │ → 各 Pass 绘制   │        │                   │       │ + WorldModel 快照  │
   └────────────────┘        └───────────────────┘       └───────────────────┘
```

---

## 二、核心机制

### 2.1 组件模型

- **声明**：`class XxxComponent : public Component { HE_COMPONENT() ... };`
  `HE_COMPONENT()` 展开为 `HE_CLASS()`，为类型提供 `StaticClass()` / `GetClass()` / 类型哈希
  —— 这是运行时类型信息（工厂、反射查表）的入口。
- **生命周期**（`Scene/Component.h`）：`OnCreate()` → `OnStart()` → 每帧 `OnUpdate(dt)` → `OnDestroy()`；
  另有 `ComponentState`（Created / Active / …）记录状态。
- **挂载语义**（`World::AddComponent<T>`，`World.h`）：校验实体有效 → 同类型去重（已存在返回 `nullptr`）
  → 构造 → 设 `m_Entity` / `m_World` → **调 `OnCreate()` 与 `OnStart()`** → 入桶。
  ⇒ 需要"先配参数再生成几何"的组件（Cube/Sphere/Decal/Billboard…）都是**先 `AddComponent` 之外改字段、
  再手动调 `OnCreate()`**，或按注释"配置须在 AddComponent 之前设置"。
- **存储**：`unordered_map<type_index, vector<ComponentEntry{entityID, unique_ptr<Component>}>>`
  —— 按类型分桶 ⇒ `ForEach<T>` 是"顺序遍历一个紧凑桶"，不需要为每种组件写遍历代码；
  代价是组件间没有内存局部性优化（本项目规模下不成问题）。
- **访问**：`GetComponent<T>` / `HasComponent<T>` / `RemoveComponent<T>` / `ForEach<T>` /
  `ForEachEntity` / `ForEachComponent`（按实体列出其全部组件）。

### 2.2 实体与层级

- `Entity` = `{ id }`（`using EntityID = u64`，常量 `kInvalidEntity = 0`）：`IsValid()` 检查；`World` 用 `m_NextID` 分配，销毁走 `CleanupEntity`。
- `SceneGraph`：`SetParent(child, parent)` / `GetParent` / `GetWorldMatrix(entity)` / `UpdateTransforms()`。
  变换本身放在 `TransformComponent`（position / rotation / scale + `GetLocalMatrix()`），
  **世界矩阵由 SceneGraph 按父链合成** —— 所以子实体的世界位置随父变化自动更新，
  组件不需要自己关心父级。
- 约定：组件里凡是"世界空间"的量（碰撞体、相机、光照方向、弹簧臂）都通过
  `SceneGraph::GetWorldMatrix` 或 `TransformComponent` 的旋转/缩放推导，见各组件小节。

### 2.3 反射与编辑器

`Engine/Scene/Scene/SceneReflect.cpp` 集中注册（当前 **32 个类 / 89 个属性**；另有
`Engine/AI/Agent/AgentReflect.cpp` 3 个类 / 4 个属性、`Engine/Physics/Physics/PhysicsReflect.cpp`
1 个类 / 11 个属性，合计 36 个类 / 104 个属性）：

```cpp
HE_BEGIN_REGISTER(he::SpotLight)                       // 生成 StaticClass()（含工厂）
    HE_REGISTER_PROPERTY(he::SpotLight, float3, direction)
        HE_ATTR_CATEGORY("SpotLight") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE()
        HE_ATTR_AI_DESCRIPTION("聚光锥轴方向（世界空间）")
    HE_END_PROPERTY()
HE_END_REGISTER()
```

- `PropertyInfo` 记录 `name / offset / size / typeName / flags / attributes`；
  属性注解目前有 `Category`、`DisplayName`、`AiVisible`、`AiWritable`、`AiDescription`、`AiTool` 等。
- 编辑器 Details 面板按 `offset` 直接读写成员（`reflect::GetMemberPtr` / `ForEachProperty`），
  **不需要为每种组件手写 UI**。
- 内部结构不暴露的组件（如 `MemoryComponent` / `GoalComponent`）只做**类型注册**不做属性注册；
  纯调试组件（`CollisionDebugComponent`）做**空注册**（只为 `StaticClass()` 有定义）。

### 2.4 AI 一等公民

- **AI 注解 + 词表**：`HE_ATTR_AI_VISIBLE/WRITABLE/DESCRIPTION` 标出"LLM 可读可写"的属性；
  `Engine/AI/TypeSchema.cpp` 维护 LLM 侧词表（当前 **14 种类型**：Cube / Sphere / 四种光源 /
  Camera / Health / Decal / PhysicalSky / Animation / RigidBody / NavMesh / NavAgent）
  与字段清单（如 `Decal: decalTexture, size, opacity, projectionDepth`）。
- **生成场景**：`SceneBuilder`（`Engine/AI/SceneBuilder.cpp`）把 LLM 输出的 JSON 解析成
  实体 + 组件，逐字段都带安全降级（类型不对/字段缺失则用默认值，绝不崩）。
- **快照与动作**：`Engine/AI/WorldModel/*`（`WorldModel.h` / `Observation.h` / `Action.*`）
  负责把世界状态整理成给模型的观测，以及把模型动作落到组件上；
  `AgentComponent` 只保存策略类型与提示词，运行时状态（brain 实例等）**不注册为反射属性**。
- **实体引用类属性**（`homingTarget` / `targetEntity` / `cameraEntity` 等）**刻意不进反射/词表**：
  `EntityID` 无法快照序列化，交给运行时逻辑设置（这是策略，不是待办）。

### 2.5 系统的形态与更新顺序

系统都是**无状态的静态类**，签名统一为 `static void Update(he::World&, ...)`，
由宿主决定调用顺序。以 `Samples/02.Cube` 的主循环为例（顺序即依赖顺序）：

| 顺序 | 调用 | 作用 |
|---|---|---|
| 1 | `he::MovementSystem::Update(world, dt)` | 角色移动（含地面检测，读 `CollisionComponent`） |
| 2 | `he::physics::PhysicsSystem::Update(world, sg, dt)` | Jolt 刚体步进 + 回写 `TransformComponent` |
| 3 | `he::NavAgentSystem::Update(world, sg, dt)` | 沿 A* 路径移动（读 `NavMeshComponent` 的格子） |
| 4 | `he::SplineMeshSystem::Update(world, device)` | 样条 → 条带网格重建（脏检测） |
| 5 | `he::TextRenderSystem::Update(world, device)` | 文字栅格化 → bindless 纹理（脏检测 + 槽位复用） |
| 6 | `he::SkeletalMeshSystem::Update(world, dt)` | 骨骼采样/混合/重定向 → 蒙皮矩阵 |
| 7 | `he::CollisionDebugSystem::Update(world, enabled)` | 碰撞体线框（形状变化才重建） |
| — | `he::AgentSystem::Update(world, sg, ...)` | AI Agent（策略推理 → 动作；07.AISamples 使用） |
| — | `he::DamageSystem` / `AbilitySystem` / `ProjectileSystem` / `SpringArmSystem` / `SplineSystem` | 按示例需要调用 |

> **顺序为什么重要**：物理回写 Transform 之后再采样动画/文字，画面才不会落后一帧；
> 线框放在最后读的是**本帧最终**的碰撞形状。系统之间不互相调用，只通过组件数据耦合。

---

## 三、组件总表

共 **33 个可挂载组件类型**（不含 `Component` / `MeshComponent` / `LightComponent` 三个基类；
含 `DirectionalLight` / `PointLight` / `SpotLight` / `RectLight` 四种光源派生类型），按类别列出；
"驱动"列是负责更新它的系统。各组件的**落地记录、判据与已知边界**见
`docs/已实现功能/HugEngine Entity Component 架构与开发计划.md`（§一 现状盘点 + §四~§十九 逐任务小节）。

### 3.1 变换与层级

| 组件 | 对应 UE5 | 职责 | 关键属性 | 驱动 |
|---|---|---|---|---|
| `TransformComponent` | `USceneComponent` | 局部变换（位置/旋转/缩放）+ 轴向辅助 | `position` / `rotation`(quat) / `scale`；`GetLocalMatrix()` / `GetForward/Up/Right()` | 各系统写回；`SceneGraph` 合成世界矩阵 |

### 3.2 网格与几何

| 组件 | 对应 UE5 | 职责 | 关键属性 | 驱动 |
|---|---|---|---|---|
| `MeshComponent`（基类） | `UStaticMeshComponent` | 顶点/索引缓冲 + glTF PBR 材质 | `baseColorFactor` / `emissiveFactor` / `metallicFactor` / `roughnessFactor` / `aoFactor` / `alphaCutoff` / `doubleSided` / `unlit` / `castShadow` + 五张纹理路径 | `SceneRenderer` + 各 Pass |
| `CubeComponent` | — | 程序化立方体 | `halfExtent` | 同上 |
| `SphereComponent` | — | 程序化球体 | `radius` / `segmentCount` / `ringCount` | 同上 |
| `InstancedMeshComponent` | `UInstancedStaticMeshComponent` | 万级实例、单次 `DrawIndexed`；**逐实例 GPU 剔除** | `instanceTransforms` / `enableFrustumCull` / `meshPath` | Forward / Deferred 的实例化绘制段（`InstanceCuller`） |
| `SkeletalMeshComponent` | `USkeletalMeshComponent` | GPU 蒙皮 + 剪辑播放/**混合**/**重定向** | `skeleton`（+`sourceSkeleton`/`retargetProfile`）/ `currentClip` / `playSpeed` / `blendLayers` / `CrossFadeTo` | `SkeletalMeshSystem` |
| `SplineComponent` | `USplineComponent` | Catmull-Rom 样条求值 | `points` / `bClosedLoop`；`EvaluateAtDistance` / `GetTangent` / `GetVersion` | 只读（下游脏检测用 `GetVersion`） |
| `SplineMeshComponent` | `USplineMeshComponent` | 沿样条生成条带网格（道路/轨道） | `width` / `uvTiling` / `enabled`；`RebuildFrom` | `SplineMeshSystem`（版本脏检测） |

### 3.3 光照与天空

| 组件 | 对应 UE5 | 职责 | 关键属性 | 驱动 |
|---|---|---|---|---|
| `LightComponent`（基类） | `ULightComponent` | 光源公共参数 | `type` / `enabled` / `color` / `intensity` / **物理光照**（`colorTemperature` / `luminousIntensity` / `illuminance`）/ 阴影（`castShadow` / `shadowMapSize` / 两级 bias / `shadowStrength` / `shadowRadius`） | 各管线 `CollectLights` + 阴影系统 |
| `DirectionalLight` | `UDirectionalLightComponent` | 平行光 | `direction` / `syncWithPhysicalSky` | 同上（`SyncPhysicalSkyToSun` 可同步） |
| `PointLight` | `UPointLightComponent` | 点光 | `range` | 同上（任务 2） |
| `SpotLight` | `USpotLightComponent` | 聚光 | `range` / `direction` / `innerConeAngle` / `outerConeAngle` | 同上（任务 1） |
| `RectLight` | `URectLightComponent` | 矩形面光（软阴影） | `width` / `height` / `normal` / `range` / `softness` | 同上（任务 2） |
| `PhysicalSkyComponent` | `USkyAtmosphereComponent` | 物理天空（大气散射）+ 太阳同步 | `sunDirection` / `turbidity` / `groundAlbedo` / `intensity` / `sunIntensity` / `sunIlluminance`；自由函数 `SyncPhysicalSkyToSun` / `GetPhysicalSkySun` | 各管线帧入口 |
| `SkyboxComponent` | `USkyBoxComponent` | Cubemap 天空盒 | `intensity` / `rotation` / `enabled`；`SetCubemap(tex, sampler)` | `SkyboxPass` |

### 3.4 相机与可视化辅助

| 组件 | 对应 UE5 | 职责 | 关键属性 | 驱动 |
|---|---|---|---|---|
| `CameraComponent` | `UCameraComponent` | 投影参数 + 主相机标记 | `fov` / `nearPlane` / `farPlane` / `aspectRatio` / `isMain`；`SetAspectFromWindow` | `ResolveFrameCamera` / `MakeCameraData` |
| `SpringArmComponent` | `USpringArmComponent` | 第三人称相机臂（含碰撞缩臂） | `targetEntity` / `cameraEntity` / `targetOffset` / `armLength` / `rotationLagSpeed` / `bUsePawnControlRotation` | `SpringArmSystem` |
| `BillboardComponent` | `UBillboardComponent` | 始终面向相机的四边形 | `size`；静态 `MakeBillboardMatrix` | `SceneRenderer`（每帧替换世界矩阵） |
| `TextRenderComponent` | `UTextRenderComponent` | 3D 世界文字 | `text` / `fontPath` / `fontSize` / `textColor`；运行时纹理 + 退休队列 | `TextRenderSystem` |
| `CollisionDebugComponent` | —（调试） | 碰撞体线框（AABB/球/胶囊） | `color` / `lineThickness`；`segmentCount` | `CollisionDebugSystem` |

### 3.5 材质表现与动画

| 组件 | 对应 UE5 | 职责 | 关键属性 | 驱动 |
|---|---|---|---|---|
| `DecalComponent` | `UDecalComponent` | 贴花：Deferred 走 **GBuffer 投影**，Forward 走投射片 | `size` / `rotation` / `opacity` / `projectionDepth` / `decalTexture` | `DecalPass`（Deferred）/ 普通网格路径（Forward） |
| `ParticleComponent` | `UParticleSystemComponent` | GPU 粒子系统（发射/模拟/排序/渲染） | `emitRate`；`Play/Pause/Resume/Stop`；`SetCallback`；运行时状态查询 | `ParticleRenderer`（渲染侧） |
| `AnimationComponent` | `UAnimationComponent`（旧式） | CPU 关键帧动画（平移/旋转/缩放通道） | `clips` / `currentClip` / `time` / `speed` / `playing`；`Update(dt, target)` / `Add*Key` | 宿主直接调 `Update`（轻量路径） |

### 3.6 游戏性

| 组件 | 对应 UE5 | 职责 | 关键属性 | 驱动 |
|---|---|---|---|---|
| `CollisionComponent` | `UCapsuleComponent` / `UBoxComponent` | 纯数据碰撞体（AABB/球/胶囊） | `shape` / `halfExtents` / `radius` / `height` / `bEnabled` | `CollisionSystem`（Overlap/Contains/Raycast，被移动/物理/投射物消费） |
| `CharacterMovementComponent` | `UCharacterMovementComponent` | 角色移动（走/跑/跳/重力/坡度） | `walkSpeed` / `runSpeed` / `jumpHeight` / `gravity` / `maxSlopeAngle`；输入 `inputDirection` / `bWantsJump` / `bRunning`；状态 `velocity` / `bOnGround` | `MovementSystem` |
| `ProjectileMovementComponent` | `UProjectileMovementComponent` | 抛射物（初速/重力/追踪/生命周期） | `initialSpeed` / `maxSpeed` / `gravityScale` / `bHoming` / `homingTarget` / `lifetime` / `onHit` | `ProjectileSystem` |
| `HealthComponent` | —（UE 用 GAS 属性集） | 生命值 + 事件 | `maxHealth` / `currentHealth` / `bInvincible` / `onDamaged` / `onDeath` | `DamageSystem` |
| `AbilityComponent` | `UGameplayAbility`（简化） | 技能列表 + 资源 + 冷却 | `skills` / `resource` / `maxResource` / `onCast` / `cooldownRemaining`；`AddSkill` / `CanCast` / `Cast` | `AbilitySystem` |
| `NavMeshComponent` | `ARecastNavMesh`（简化） | 格子导航网格 + 阻挡 | `cellSize` / `origin` / `blocked`；`Resize` / `SetBlocked` / `WorldToCell` / `CellToWorld` / `IsBlocked` | 被 `NavMeshSystem`（A*）与 `NavAgentSystem` 读 |
| `NavAgentComponent` | `UPathFollowingComponent` | 沿路径寻路移动 | `navMeshEntity` / `target` / `speed` / `arriveRadius`；`SetTarget` / `ClearTarget`；运行时 `path` / `pathIndex` | `NavAgentSystem` |
| `LevelComponent` | `ULevelStreaming`（简化） | 关卡资产展开 | `levelPath`；`IsExpanded` / `SetExpanded` / `GetChildren` | 编辑器展开流程 |

### 3.7 物理

| 组件 | 对应 UE5 | 职责 | 关键属性 | 驱动 |
|---|---|---|---|---|
| `RigidBodyComponent` | `UPrimitiveComponent`（Chaos 刚体） | Jolt 刚体（形状/质量/摩擦/阻尼） | `radius` / `halfExtent` / `height` / `isDynamic` / `mass` / `friction` / `restitution` / `linearDamping` / `angularDamping` / `enabled` | `he::physics::PhysicsSystem` |

### 3.8 AI

| 组件 | 对应 UE5 | 职责 | 关键属性 | 驱动 |
|---|---|---|---|---|
| `AgentComponent` | `AAIController`（简化） | 挂一个策略大脑（LLM / Mock / RL） | `brainType` / `systemPrompt` / `enabled` | `he::ai::AgentSystem` |
| `GoalComponent` | —（GOAP/BDI 风格） | 目标栈（按优先级） | `AddGoal` / `MarkAchieved` / `GetCurrentGoal` / `GetActiveGoalCount`（内部结构不暴露） | Agent 策略读写 |
| `MemoryComponent` | —（记忆） | 短期记忆键值 + 重要度 | `AddShortTerm` / `Query` / `ClearShortTerm`（内部结构不暴露） | Agent 策略读写 |

---

## 四、变换、网格与光照组件详解

### 4.1 TransformComponent

- 局部变换三元组 + `GetLocalMatrix() = T * R * S`，轴向辅助 `GetForward()`（**-Z**，Vulkan 约定）、
  `GetUp()`（+Y）、`GetRight()`（+X）。
- **世界矩阵不在这里**：`SceneGraph::GetWorldMatrix(entity)` 沿父链合成（见 §2.2）。
  组件里凡是需要世界空间的模块都显式取一次世界矩阵，避免"局部/世界"混用。

### 4.2 MeshComponent 与派生形状

- 网格数据由 `SetMeshData(vertices, indices)` 写入（`StaticVertex` = position / normal / uv）；
  无 RHI 设备时（单测）只更新计数与包围盒，并打一条 `MeshComponent::SetMeshData: RHI device not available`。
- **材质**：glTF 2.0 PBR 参数（基础色/自发光/金属度/粗糙度/AO/alpha 截断/双面/无光照/是否投影）
  + 五张纹理路径；纹理走 **bindless 纹理数组**，`materialID` 是该材质在数组里的基索引，
  各材质占 4 个槽（baseColor / normal / metallicRoughness / occlusion），
  shader 按 `textureMask` 判断某槽到底有没有纹理。
- `unlit` 用于"不该被光照影响"的几何（贴花卡片、调试线框、光源可视化球）；
  `castShadow=false` 让几何不进入阴影贴图（同样常见于可视化球）。
- `CubeComponent.halfExtent` / `SphereComponent.radius+segmentCount+ringCount` 是**程序化**几何参数，
  在 `OnCreate` 里生成；注释写明"配置须在 AddComponent 之前设置"（因为 `AddComponent` 会立即调 `OnCreate`）。

### 4.3 InstancedMeshComponent（含逐实例剔除）

- CPU 侧保存 `instanceTransforms`（每实例一个世界矩阵），`SetInstanceTransforms` 置脏。
- 渲染：实例变换上传到 **bindless SSBO**；`enableFrustumCull=true` 时先跑
  `InstanceCuller` 的 compute（逐实例六平面测试 → 压缩可见列表 + 原子累加间接命令的 `instanceCount`），
  再 `DrawIndexedIndirect` + 顶点着色器按可见列表取实例；
  关闭时退回"整批 `DrawIndexed`"（便于 A/B）。
- GPU 侧状态（`instanceSSBOHandle` / `instanceCullCmd[3]` / `visibleInstanceCount`）由渲染管线管理，
  组件里的注释标注"勿手动改"。命令缓冲与可见列表**按飞行帧分槽**（避免跨帧读写打架）。

### 4.4 SkeletalMeshComponent（蒙皮 + 混合 + 重定向）

- `SetSkeleton(asset)` 上传蒙皮顶点/索引（`JOINTS_0`/`WEIGHTS_0` 布局），走 GPU 蒙皮（Forward 路径）。
- 播放：`PlayClip(clip, loop)`；**剪辑混合**：`SetBlendLayers` / `SetBlendLayer` / `CrossFadeTo`
  （最多 `kMaxBlendLayers = 4` 层，权重按 $\sum w$ 归一化，旋转加权 nlerp，交叉淡入结束收敛为单层）；
  **动画重定向**：`SetAnimationSource(source, profile)` 后剪辑表切到源骨架、按关节名字映射，
  本组件的网格与绑定姿势仍用自身骨架。
- 每帧由 `SkeletalMeshSystem` 采样关节 TRS → 层级合成世界矩阵 → `skin = world × inverseBind` → 骨骼 SSBO。

### 4.5 光照组件族

- `LightComponent` 是**基类**，`LightType` 枚举 + 公共参数；`DirectionalLight` / `PointLight` /
  `SpotLight` / `RectLight` 是四个派生类（各自追加几何参数）。
- 除了传统 `intensity`，还支持**物理光照**：`colorTemperature`（K，色温→RGB）、
  `luminousIntensity`（cd，点/聚光）、`illuminance`（lux，方向光）——0 表示走传统模式。
- 阴影参数在基类统一：分辨率、深度/法线两级 bias、强度、光源半径（软阴影）。
- `DirectionalLight.syncWithPhysicalSky=true` 时由 `SyncPhysicalSkyToSun(world)` 把
  `PhysicalSkyComponent` 的太阳方向与照度同步过来（渲染管线帧入口调用，幂等），
  这样**空中透视与光照方向天然一致**。

### 4.6 PhysicalSkyComponent / SkyboxComponent

- 物理天空：`sunDirection`（OnCreate 归一化）、`turbidity`（1 极清 … 10 浓霾）、地面反照率、整体亮度、
  太阳盘亮度与照度基准；自由函数 `GetPhysicalSkySun(world, dir, turbidity)` 供其它模块查询。
- 天空盒：`SetCubemap()` 接收外部加载的 cubemap（程序化生成或文件），`intensity` / `rotation` / `enabled`
  控制渲染；与物理天空是**两条独立路径**（一个走大气散射 shader，一个走 cubemap）。

---

## 五、相机、可视化与表现组件详解

### 5.1 CameraComponent

- 投影参数（fov 度数 / near / far / aspect）+ `isMain` 主相机标记；
  `MakeCameraData(camComp, transform)` 组装渲染用 `CameraData`，
  `ResolveFrameCamera(world, fallback)` 优先取场景里的主相机、否则回退自由相机。
- 场景里**没有** `isMain=true` 的 CameraComponent 时，示例继续用自带的自由相机控制器 —— 兼容旧行为。

### 5.2 SpringArmComponent

- `targetEntity`（锚点来源）+ `cameraEntity`（写回其 Transform）+ `armLength` / `targetOffset` /
  `rotationLagSpeed` / `bUsePawnControlRotation`；`SpringArmSystem` 每帧求解并做**碰撞缩臂**
  （用 `CollisionSystem::Raycast` 检测臂长上的遮挡，防止相机穿墙）。
- `targetEntity` / `cameraEntity` 是 `EntityID`，**刻意不进反射/词表**（见 §2.4）。

### 5.3 BillboardComponent / TextRenderComponent

- Billboard：`MakeBillboardMatrix(position, size, forward, up)` 生成对齐相机的矩阵
  （列 0/1 是相机右/上乘尺寸，列 2 朝相机）；`SceneRenderer` 每帧用相机朝向替换实体世界矩阵。
- 文字：`TextRenderSystem` 检测 `text`/`fontPath`/`fontSize` 脏 → stb_truetype 栅格化
  （字体缺失时按"指定字体 → 系统字体 → 内置 ASCII 位图字体"降级）→ 重建纹理并**复用同一个
  bindless 槽位**（旧纹理进 N 帧延迟释放队列），同时按位图宽高比更新广告牌 `size`。
  每 100 像素 = 1 米。

### 5.4 CollisionDebugComponent

- 由 `CollisionDebugSystem::Update(world, enabled)` 驱动：对每个 `CollisionComponent` 用
  `CollisionSystem::ExtractWorldShape`（**与检测同一份形状语义**）取世界形状 → 三角化成线框
  （AABB 12 棱 / 球 3 个正交大圆 / 胶囊两圆+4 竖线+半球弧）→ 写入本组件（MeshComponent）。
- 每段线是两片互相垂直的细带（不需要相机信息）；形状与线宽没变就不重建网格；
  开关关闭或碰撞体 `bEnabled=false` 时几何清零。

### 5.5 DecalComponent

- **Deferred**：`DecalPass` 画贴花**体积盒** → 片段着色器采样 GBuffer 世界坐标（MRT4）拿真实表面点、
  采样深度判定天空 → 盒外 discard、盒内按局部 xy 反算 UV → 按 alpha 混合写回 albedo/法线
  （只写 MRT0/1，其余通道 `writeMask=None`）。`projectionDepth` 是投影体积厚度。
- **Forward**：仍走原来的半透明投射片卡片（Forward 没有可写的中间表面）。

### 5.6 ParticleComponent

- 参数 + 状态机（`Play/Pause/Resume/Stop`）+ 回调；CPU 端累积时间（`Tick`）与发射位置查询，
  真正的模拟在渲染侧 `ParticleRenderer`（GPU：Init/Emit/Simulate/Culling/Sort/Render 一组 compute+frag）。
- `emitRate > 0` 覆盖默认发射率（反射/AI 可调）；`< 0` 时沿用 `m_Param.particlesPerSec`（兼容旧调用）。

### 5.7 AnimationComponent（轻量 CPU 动画）

- 通道式关键帧（平移/旋转/缩放各自的关键帧列表，附带自动排序与 `FinalizeClip` 求总长）；
  `Update(dt, TransformComponent* target)` 直接把插值结果写进目标 Transform。
- 与 `SkeletalMeshComponent` 是**两套东西**：后者是 glTF 骨架 + GPU 蒙皮；前者是"给普通实体做
  简单的 TRS 动画"，无骨架概念。

---

## 六、游戏性组件详解

### 6.1 CollisionComponent 与 CollisionSystem

- 三种形状（`CollisionShape::AABB / Sphere / Capsule`），世界空间形状统一由
  `CollisionSystem::ExtractWorldShape` 提取：
  - AABB：局部盒经世界矩阵变换后取**重轴** AABB（旋转时保守放大）；
  - Sphere：球心 + 半径（与旋转无关）；
  - Capsule：**垂直胶囊**，段沿 Transform 上方向，段半长 = `max(0, height/2 − radius)`。
  - 半径与半尺寸统一乘**最大缩放分量**（MVP 约定）。
- `CollisionSystem` 提供 `Overlap(a, b)`（三形状全组合）、`Contains(e, point)`、
  `Raycast(origin, dir, maxDistance, ...)`（可排除某实体、可输出命中法线）。
  几何算法（点-线段最近点、线段-线段距离、射线-AABB/球/胶囊）都是纯数学、无 RHI 依赖，可单测。

### 6.2 CharacterMovementComponent

- 参数：走跑速度、跳跃高度（起跳初速 $= \sqrt{2gh}$）、重力、最大可站立坡度。
- 每帧输入由调用方写入（`inputDirection` 世界 XZ、`bWantsJump` 跳一次、`bRunning`），
  `MovementSystem` 消费并写回 `velocity` / `bOnGround`；地面检测与坡度过滤依赖
  `CollisionComponent` + `CollisionSystem`。

### 6.3 ProjectileMovementComponent

- 首帧用"实体前向 × `initialSpeed`"初始化速度，之后按 `gravityScale` 积分；
  `bHoming` 时朝 `homingTarget` 转向；`lifetime` 超时自动销毁；`onHit` 回调由碰撞检测触发。
- `homingTarget` 是 `EntityID`（不进反射/词表）。

### 6.4 HealthComponent 与 DamageSystem

- 生命值 + 无敌标记 + 两个回调（`onDamaged(entity, amount)` / `onDeath(entity)`）；
  `currentHealth` 始终钳制在 `[0, maxHealth]`，无敌时免伤但治疗仍生效。

### 6.5 AbilityComponent 与 AbilitySystem

- 技能定义（名字/冷却/消耗）+ 资源池 + 冷却剩余（与 `skills` 下标对齐）；
  `CanCast(i)` 检查资源与冷却，`Cast(i, target)` 扣资源、进冷却、触发 `onCast`
  —— 真正的效果（如生成火球实体 + 抛射物）由 `onCast` 回调里的游戏逻辑实现。

### 6.6 NavMeshComponent / NavAgentComponent

- 导航网格是**格子**表示：`Resize(w, h, cell)` 分配 `blocked` 数组，
  `WorldToCell` / `CellToWorld` / `IsBlocked`（越界视为阻挡）是全部接口。
- Agent 侧：`SetTarget(worldPos)` 触发 `needsRepath`，`NavAgentSystem` 用 A*（8 向、绕墙）算路径，
  沿路径按 `speed` 移动、`arriveRadius` 判到达；路径与进度缓存在组件的 `path` / `pathIndex`。

### 6.7 LevelComponent

- `levelPath`（Level 资产）+ 展开状态与展开后的子实体列表；属于编辑器/关卡流送流程的挂载点。

---

## 七、物理与 AI 组件详解

### 7.1 RigidBodyComponent（Jolt）

- 形状（Box / Sphere / Capsule 语义的 `halfExtent` / `radius` / `height`）+ 质量 / 摩擦 / 弹性 /
  线角阻尼 / `isDynamic` / `enabled`。
- `PhysicsSystem::Update(world, sceneGraph, dt)` 步进 Jolt 世界并把位姿**回写 TransformComponent**；
  `GetActiveBodyCount()` 提供调试读数。刚体与 `CollisionComponent` 是**两套碰撞**：
  前者交给 Jolt（动力学），后者是引擎自带的轻量检测（移动/射线/投射物用）。

### 7.2 AgentComponent / GoalComponent / MemoryComponent

- `AgentComponent`：`brainType`（LLM / Mock；源码注释另把 RL 列为计划项）+ `systemPrompt` +
  `enabled`；`AgentSystem` 按名字分派构造 `IBrain`（`Mock` → MockBrain，其余 → LLMBrain），
  `AgentSystem::Update(world, sg, ...)` 驱动一轮"观测 → 决策 → 动作"。
- `GoalComponent`：目标按优先级插入，`GetCurrentGoal()` 取最高优先级未达成项；
  `MemoryComponent`：短期键值记忆（带重要度、超限淘汰最旧）。
- 这两者的**内部结构不注册反射属性**（`InternalStructure` 不外露），避免 LLM 直接改写内部列表。

---

## 八、渲染如何消费组件

`he::render::SceneRenderer::Prepare(world, sg, camera, objectBuffer, excludeDecals)` 是唯一入口：

1. **收集**（顺序固定，且必须与 `MeshBatcher::Build`、`GPUScene::Collect` 保持一致，
   否则 `objectIndex` 会错位）：普通 `MeshComponent` → Cube → Sphere → Billboard（矩阵对齐相机）
   → TextRender（同上）→ Decal（Deferred 可排除）→ SplineMesh → InstancedMesh → SkeletalMesh。
   Billboard/TextRender 的世界矩阵每帧按相机重算；实例化/骨骼只登记**一个对象条目**（材质与对象数据），
   真正的顶点由各自的 Pass 绘制。
2. **剔除**：CPU 视锥剔除（`enableFrustumCull`，按 64 个一批并行）；GPU 剔除结果（`GPUCulling`）
   可用时优先采用（读回可见索引过滤 draw list）。
3. **上传**：把可见项写进 `GPUObjectData`（世界矩阵、包围盒、材质参数、迪士尼参数、纹理掩码…），
   即 shader 侧 `u_Objects[objectIndex]`。
4. **分 Pass 绘制**：
   - Forward：单 PSO（PBR）+ 逐对象 `DrawIndexed`；实例化/骨骼走 `useInstanceID=2/3` 模式；
   - Deferred：GBuffer 8 MRT（albedo/法线/emissive/velocity/worldPos/disneyA/disneyB/光照图键）
     → **投影贴花**（只写 MRT0/1）→ GI 各源 → Lighting → 天空盒 → 后处理；
   - 实例化：`InstanceCuller`（逐实例剔除）+ `DrawIndexedIndirect`；
   - 线框/贴花卡片等"非光照几何"通过 `unlit` + 半透明材质复用同一条网格路径。

---

## 九、扩展指南：新增一个组件要做四件事

计划文档把这条约定叫「一个组件 = 四件事」，照做即可获得编辑器 + AI 的完整能力：

1. **类定义**：`class XxxComponent : public Component（或 MeshComponent）` + `HE_COMPONENT()`，
   参数给默认值、加中文注释；需要生成几何的写 `OnCreate()`。
2. **反射注册**（`Engine/Scene/Scene/SceneReflect.cpp`）：`HE_BEGIN_REGISTER` + 逐属性
   `HE_REGISTER_PROPERTY`（`name` / `offset` / `typeName`），并加 `HE_ATTR_CATEGORY` /
   `HE_ATTR_AI_VISIBLE` / `HE_ATTR_AI_WRITABLE` / `HE_ATTR_AI_DESCRIPTION`。
   *只想让类型系统认识、不给编辑器/AI 用* → 空注册（如 `CollisionDebugComponent`）。
3. **词表**（`Engine/AI/TypeSchema.cpp`）：如果该组件要能被 LLM 生成，就在
   `component_types` 里加类型 + 字段清单，并在 `SceneBuilder` 里加解析分支（带安全降级）。
4. **系统接入**：要么加入某个 `*System::Update` 的遍历，要么在渲染收集/绘制链路上加一条路径；
   同时在 CMake 源列表登记文件，并补 doctest（CPU 侧可测的部分）。

---

## 十、已知边界（与计划文档一致）

- **Forward 的贴花**仍是投射片卡片（Forward 没有可写的中间表面；Deferred 才是投影贴花）。
- **逐实例剔除只有视锥**（无逐实例 Hi-Z 遮挡）；可见列表容量 10 万实例/组件，超出部分不画；
  Deferred 的 GPU-Driven 批处理路径未并入实例。
- **碰撞调试线框**是世界单位线宽（不随距离变化），走半透明路径、有少量 overdraw。
- **两套碰撞**并存（`CollisionComponent` 轻量检测 vs Jolt `RigidBodyComponent`），二者不自动同步。
- **实体引用类属性**（`homingTarget` / `targetEntity` / `cameraEntity`…）不进反射/词表：不可快照序列化。
- **场景序列化**：`Engine/Editor/Editor/SceneSerializer.cpp` 提供 `SceneSerializer::Save/Load`
  （`.hescene`，magic `HESC` + version + 实体/组件/层级），`Samples/Editor/Panels/LevelLoader.cpp`
  提供 `LevelLoader::LoadLevel` 读取 Level 资产；两者都靠 `ForEachProperty` + typeName 分派，
  但组件构造仍是按类型名的 if-else（各只覆盖一部分组件类型）。示例的场景也可来自代码构造
  或 LLM 生成后的内存世界（面板配置另存为 `Content/Config/*.cfg`）。
- 计划文档中原 **27/28**（渲染类型注册表化 / CollectLights 数据驱动抽取）**已删除**：
  属于"没有消费方就不提前泛化"的架构改动，编号不复用。
