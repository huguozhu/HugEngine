# HugEngine Jolt Physics 集成计划（C2 最小竖切）

> 状态：计划（待执行）| 关联：`HugEngine Entity Component 开发计划.md` §七 Phase C2（PhysicsComponent/RigidBody）
> 目标：接入 Jolt Physics，落地 `Engine/Physics` 模块 + `RigidBodyComponent`，打通"刚体下落 → 碰撞 → 变换回写 TransformComponent → 渲染可见"最小闭环。

---

## 一、目标与范围

**Goal**：接入 Jolt Physics，落地 `Engine/Physics` 模块 + `RigidBodyComponent`，打通"刚体下落 → 碰撞 → 变换回写 TransformComponent → 渲染可见"最小闭环，作为文档 §七 C2 的 MVP（对齐 Entity Component 计划的 8 步检查清单）。

**范围内**：球/盒/胶囊刚体、静态碰撞体（地面/障碍）、固定步长、组件生命周期同步、doctest + 02.Cube/05.AISamples 演示。

**范围外（明确不做）**：关节/车辆/布料/破碎、双精度大世界、Taskflow 对接 Jolt JobSystem、DebugRenderer 线框（列入后续，与 Collision 线框技术债合并）、CharacterMovement 物理化（仅文档化策略）。

## 二、选型说明（为何 Jolt）

| 维度 | Jolt Physics |
|---|---|
| 协议 | MIT（最宽松，商用自由） |
| 集成 | C++ 原生、零绑定、无第三方依赖，CMake `add_subdirectory` 与现有 External submodule 模式一致 |
| 语言标准 | C++17 基线（HugEngine C++20 兼容） |
| 质量背书 | Godot 4 大规模生产验证、Horizon Forbidden West、GDC 关注；文档/示例在同类中最好 |
| 多线程 | 自带 JobSystemThreadPool（MVP 直接用；Taskflow 对接列后续） |
| 坐标系 | 同为 Y-up 米制，与 glm（RH、Z 深度）约定一致 |
| 备选 | PhysX 5（BSD-3，功能最全但 SDK 重、接入成本高，仅在需要布料/破碎时再评估） |

> 注：Entity Component 计划 §七 原文提到 "BEPU/PhysX/Jolt"——BEPUphysics 为 C# 库，不适配纯 C++ 引擎，予以剔除。

## 三、模块与依赖架构

```
Engine/Physics/                     ← 新增模块（依赖 Scene，单向；Scene 不依赖 Physics）
├── CMakeLists.txt                  静态库 HugEnginePhysics（链 HugEngineScene/HugEngineCore/Jolt）
├── Physics/PhysicsWorld.h/.cpp     Jolt PhysicsSystem 封装（初始化 / Step / Shutdown）
├── Physics/RigidBodyComponent.h/.cpp  纯数据组件（不包含任何 Jolt 类型）
├── Physics/PhysicsSystem.h/.cpp    静态 Update：固定步积累积 + Entity↔Body 双向同步
├── Physics/PhysicsReflect.cpp      反射注册 + AI 注解（仿 AgentReflect，避免 Scene→Physics 反向依赖）
└── Physics/JoltConversions.h       glm float3/quat ↔ Jolt RVec3/Quat 转换（集中收口）
```

**组件放 Physics 模块内**（与 AI 模块的 Agent 组件同模式）：字段全部纯数据（shape/radius/mass/...），保证 Scene 层不依赖 Jolt 头文件；`Engine/CMakeLists.txt` 在 Scene 之后加 `add_subdirectory(Physics)`。

## 四、文件结构（新增/修改）

| 操作 | 文件 |
|---|---|
| 新增 | `Engine/Physics/CMakeLists.txt` |
| 新增 | `Engine/Physics/Physics/PhysicsWorld.h/.cpp` |
| 新增 | `Engine/Physics/Physics/RigidBodyComponent.h/.cpp` |
| 新增 | `Engine/Physics/Physics/PhysicsSystem.h/.cpp` |
| 新增 | `Engine/Physics/Physics/PhysicsReflect.cpp` |
| 新增 | `Engine/Physics/Physics/JoltConversions.h` |
| 新增 | `Tests/TestPhysicsBasics.cpp` |
| 修改 | `.gitmodules` + submodule `Engine/External/JoltPhysics`（https://github.com/jrouwe/JoltPhysics） |
| 修改 | `Engine/External/CMakeLists.txt`（`add_subdirectory(JoltPhysics)`，选项 `JPH_ENABLE_INSTALL=OFF`、`JPH_ENABLE_OBJECT_LAYER_PROPERTIES=OFF`、Debug 开 `JPH_ENABLE_ASSERTS` 等） |
| 修改 | `Engine/CMakeLists.txt`、`Tests/CMakeLists.txt`、根 `CMakeLists.txt`（folder 分组加 HugEnginePhysics） |
| 修改 | `Engine/AI/TypeSchema.cpp` + `Engine/AI/SceneBuilder.cpp`（词表加 `"RigidBody"` 与 `else if` 分支，安全降级） |
| 修改 | `Samples/02.Cube`（PhysicsDemo 区）或 `05.AISamples` 新 Feature |

## 五、接口约定（跨任务契约，按此签名实现）

```cpp
// PhysicsWorld.h —— Jolt 世界封装
class PhysicsWorld {
public:
    bool Initialize(const PhysicsInitDesc& desc);   // maxBodies / maxBodyPairs / maxContactConstraints
    void Step(f32 fixedDt);                          // 内部 JPH::PhysicsSystem::Update
    void Shutdown();
};

// RigidBodyComponent.h —— 纯数据组件，不包含 Jolt 头文件
class RigidBodyComponent : public Component {
    HE_COMPONENT()
public:
    u8    shape = 0;                 // 0=Sphere 1=Box 2=Capsule（与 CollisionComponent.shape 枚举对齐）
    float radius = 0.5f, halfExtent = 0.5f, height = 1.0f;  // 按 shape 取值
    bool  isDynamic = true;
    float mass = 1.0f, friction = 0.6f, restitution = 0.1f;
    float linearDamping = 0.05f, angularDamping = 0.05f;
    bool  enabled = true;            // false = 移除/冻结对应 body
};

// PhysicsSystem.h —— 仿 AgentSystem 的静态入口
class PhysicsSystem {
public:
    // 每帧调用；内部固定步积累积器（fixedDt = 1/120s，每帧步数上限防 spiral）
    static void Update(World& world, SceneGraph& sg, f32 dt);
    // 供管线/工具查询：某实体是否已在物理世界
    static bool HasBody(World& world, Entity e);
};
```

## 六、任务分解（逐任务验证门控，全中文注释）

### T1 — Jolt 引入与冒烟（0.5 天）

- submodule 拉取（`git submodule add https://github.com/jrouwe/JoltPhysics Engine/External/JoltPhysics`）→ External CMake 接入
- 20 行冒烟：建 Jolt PhysicsSystem + 一个球 + 静态地面，`Step` 数帧后球位移 ≈ ½gt²
- **验证**：编译通过；位移符合重力（先不接引擎，验证坐标系/单位/基本链路）

### T2 — PhysicsWorld + 转换层（0.5 天）

- 实现 Initialize/Step/Shutdown；`JoltConversions`（glm↔Jolt：位置/四元数/矩阵）；broadphase layer 简化（静态/动态两层）
- **验证**：doctest 断言位置/四元数往返转换一致

### T3 — RigidBodyComponent + 反射/AI 注册（0.5 天，走 8 步清单前 2 步）

- 纯数据组件 + `PhysicsReflect.cpp` 注册（shape/radius/halfExtent/height/mass/friction/restitution/linearDamping/angularDamping/isDynamic/enabled 全打 `HE_ATTR_AI_VISIBLE/WRITABLE/DESCRIPTION`）
- **验证**：doctest（属性读写 + `WorldModel::TypeSchema()` 出现 RigidBody 字段）

### T4 — PhysicsSystem 同步核心（1 天，★关键路径）

每帧：
1. **新增**：遍历带 RigidBodyComponent 的实体 → 无 body 则按 Transform（位置/旋转；形状尺寸）创建 Jolt Body；
2. **变更**：参数/开关 diff（`enabled=false` → 移除或冻结；shape/尺寸变化 → 重建 body）；
3. **Step**：固定步长（accumulator，单帧步数上限）；
4. **回写**：每步后把激活 body 的世界变换写回实体 TransformComponent（position/rotation；scale 保持不变）。

- **验证**：doctest（丢球 → 下落位移 ≈ ½gt²；落地后静止；销毁实体 → body 删除，无泄漏计数）

### T5 — 静态碰撞体（0.5 天）

- 地面与障碍：给带 `CollisionComponent`（shape=Box 且 bEnabled）但无 RigidBodyComponent 的实体注册为 **static body**；MVP 亦可内置地面
- **验证**：doctest（球落在静态盒上停住）

### T6 — 词表 + SceneBuilder（0.5 天）

- `TypeSchema.cpp` 加 `"RigidBody": {"fields": ["shape", "radius", "halfExtent", "height", "mass", "friction", "restitution", "isDynamic", "enabled"]}`
- `SceneBuilder.cpp` 加 `else if (type == "RigidBody")`（安全降级：字段类型检查、缺省默认值）
- **验证**：doctest（LLM 场景 JSON 生成"从高处掉落的球"→ 含 RigidBody 组件断言）

### T7 — 演示（0.5 天）

- 02.Cube 加 PhysicsDemo 场景/按钮：N 个彩球（SphereComponent + RigidBodyComponent）从高处落下，撞击静态地面/盒后弹跳（restitution 区分）；ImGui 显示激活 body 数
- **验证**：运行肉眼确认下落/碰撞/静止

### T8 — 文档与收尾（0.5 天）

- 更新 `HugEngine Entity Component 开发计划.md`：§七 C2 行标 ✅（引提交）、§十二 增补
- 补"后续集成策略"小节：CharacterMovement 地面检测可切换物理回退、DebugRenderer 线框并入 Collision 线框债、Taskflow 对接 Jolt JobSystem
- **验证**：文档核对无过时项

## 七、风险与缓解

| 风险 | 缓解 |
|---|---|
| Jolt 编译选项/宏（多线程、double、CPU 特性）不匹配 | T1 独立冒烟先行；Debug 开 `JPH_ENABLE_ASSERTS` |
| 坐标/单位不一致 | Jolt 同为 Y-up 米制；T1 冒烟即验证 ½gt²；转换层集中收口 |
| 父子层级/非均匀缩放实体被物理化 | MVP 规则：带父级或被非均匀缩放的实体不允许 isDynamic（告警跳过），文档化 |
| 与 MovementSystem/CharacterMovement 并存冲突 | 演示场景互斥使用；集成策略写入 T8 文档 |
| body 泄漏/悬挂（实体销毁后） | T4 在实体销毁路径统一回收；doctest 断言 body 计数归零 |

## 八、验证汇总

- 每个任务：编译通过（`cmake --build build --config Debug`）+ 对应 doctest 全绿
- 端到端：`02.Cube.exe` PhysicsDemo 场景或 `05.AISamples` 对应 Feature
- 总门槛：doctest 新增用例 ≥ 5（转换 / 重力 / 碰撞 / 生命周期 / 词表），全部通过

## 九、后续集成策略（T8 落文档，不在本计划实现）

1. **CharacterMovement 物理化**：B3 地面射线检测可切换到物理查询（RayCast），走/跳积分逐步让位给 Jolt 角色控制器（CharacterVirtual 备选）；
2. **DebugRenderer**：实现 Jolt DebugRenderer → 引擎线框渲染，与"Collision 调试线框"技术债合并偿还；
3. **JobSystem 对接**：Jolt JobSystemThreadPool → 引擎 Taskflow 池（Jolt 支持注入自定义 JobSystem）；
4. **抽象层演进**：首个物理后端跑通后按需抽 `IPhysicsBackend`（仿 RHI 理念），备选 PhysX 5（布料/破碎需求出现时再评估）。
