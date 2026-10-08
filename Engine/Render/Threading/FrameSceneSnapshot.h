#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include "Pipeline/Camera.h"
// 材质（`GPUMaterialData`）：快照按 materialID 去重后携带，渲染期因此不必再遍历组件取材质
#include "Pipeline/Material.h"
// 粒子参数（`ParticleSystemParam`）：粒子发射器快照条目要按值携带它
#include "Scene/ParticleComponent.h"

#include <cstddef>     // offsetof
#include <cstring>
#include <type_traits>
#include <vector>

// ============================================================
// FrameSceneSnapshot —— 游戏线程产出的**不可变渲染输入**（方案 B §4.2 / 阶段 1 任务 T1.1）
//
// 【它解决什么问题】渲染侧现在直接读 ECS：`DeferredPipeline::CollectLights(pc, world, sg, camera)`、
// `GPUScene::Collect(world, sg, camera)`、`ForwardPipeline::CollectLights(...)`。一旦渲染跑到另一根
// 线程，这些遍历就会与游戏线程的 tick 竞争（方案 §2 的 C1，本方案最大的障碍）。
// 快照把"渲染真正需要的输入"在**游戏线程 tick 结束后**一次取齐，交接后视为只读。
//
// 【两条硬约束】
//   1. **只放值，不放指针**：不能出现 `Entity` / 组件指针 / `World*` / `SceneGraph*`，
//      否则"不可变"就无从谈起（渲染线程仍会顺着指针读世界）。需要资源时放**索引/ID**，
//      由渲染线程自己查表（`meshIndex` / `materialID` 就是这个用途）。
//   2. **布局与着色器一致**：能直接 memcpy 进 SSBO 的，就不要在快照里再抄一份字段定义 ——
//      抄一份必然漂移。因此：
//      · 光源（小、要逐元素写进光源 SSBO）用**逐字段镜像** + `static_assert` 逐字段锁偏移；
//      · 物体数据（大、已经是着色器布局）直接**内嵌 `GPUObjectData`**，从根上杜绝漂移。
// ============================================================

namespace he::rhi { class IRHITexture; class IRHISampler; }   // 天空盒条目携带 RHI 资源指针

namespace he::render {

// C++/Slang 共享的 GPU 布局单一真源。
// 【必须在命名空间内包含】这些结构（GPULight / GPUObjectData …）定义在 `he::render` 里，
// 且它们的字段用 `float4` / `float4x4`（同样在 `he` 命名空间）—— 与 `Pipeline/Material.h`
// 的用法完全一致；放到全局作用域包含会变成"未知类型"。
#include "ShaderTypes.slang"

/// 网格组件**类别**（收集侧判定）：让消费侧按"与旧路径逐条一致"的范围过滤条目，
/// 而不必各自持有组件指针。多消费侧的旧遍历范围本来就不同，用类别表达才不会把口径改掉：
///   · 阴影技术：`Base`/`Cube`/`Sphere` 且 `castShadow`（旧的 `ForEach<MeshComponent/Cube/Sphere>`）；
///   · RSM pass：**只有** `Base`（旧实现只遍历 `ForEach<MeshComponent>`，不收 Cube/Sphere）；
///   · RT（`RTPass`）：`Base`/`Cube`/`Sphere`（与其 `CollectMeshList` 一致）；
///   · `MeshBatcher`：`Base`/`Cube`/`Sphere`/`Billboard`/`Text`/`Decal`/`Instanced`（**不含** `Spline`/`Skeletal`）。
/// 【为什么不用一串 bool】每多一个消费者就多一个 bool 会迅速失控；类别是稳定的一组，且与
/// `World::ForEach<T>` 的精确类型分桶一一对应。
enum class SnapshotMeshClass : u8 {
    Base = 0,   // he::MeshComponent（精确类型桶）
    Cube,
    Sphere,
    Billboard,
    Text,
    Decal,
    Spline,
    Instanced,
    Skeletal,
};

/// RT/PT 专用材质补充（阶段 1 §15.1 第③段第 5 批）
///
/// 【为什么单独一组字段】`RTPass::BuildSceneMaterialTexture` 原先直接读 `MeshComponent` 的一批
/// 字段：贴图均值回落（采样到真实贴图前用"贴图均值 × 因子"）、介质吸收（Beer-Lambert）、
/// IOR/透射。它们**不在** `GPUObjectData` 里（那是光栅化路径的打包），而 RT 材质纹理必须有
/// 这些值才能脱离组件构建。只对 RT 可见子集（`SnapshotMeshClass::Base`/`Cube`/`Sphere`）填。
struct SnapshotRTMaterial {
    /// 是否已有贴图均值统计（`MeshComponent::hasMaterialAvg`）
    bool   hasMaterialAvg = false;
    float3 baseColorAvg{0.0f};       ///< 贴图 baseColor 均值（无贴图/无统计时消费侧回落到因子）
    float  metallicAvg  = 0.0f;
    float  roughnessAvg = 0.0f;
    float  ior          = 1.5f;      ///< 折射率（介质/透射用）
    float  transmission = 0.0f;      ///< 透射（>0 时 PT 走折射/介质分支）
    float3 attenuationColor{1.0f};   ///< 介质吸收色（Beer-Lambert）
    float  attenuationDistance = 0.0f;   ///< 介质吸收距离（<=0 = 不衰减）
};

/// 单个可见物体的渲染输入（与 ECS 完全解耦：只有值，没有指针）
struct SnapshotDrawItem {
    /// 世界矩阵 + 材质参数 + 世界 AABB（与着色器逐字段一致，可直接 memcpy 进对象 SSBO）
    GPUObjectData object{};
    /// 上一帧世界矩阵：TAA / 运动矢量 / 重投影需要（骨骼物体由渲染线程按 skinMatrices 重算）
    float4x4 prevWorldMatrix{1.0f};

    /// 网格资源索引：**不是指针** —— 渲染线程据此去查自己的顶点/索引缓冲表
    u32 meshIndex = 0;
    /// 实例化/骨骼网格标记：顶点由**专用路径**提供（实例化 Pass / 蒙皮 Pass），普通绘制循环跳过。
    /// 【为什么要进快照】原先这个标记由 `SceneRenderer::Prepare` 现场判定（它自己遍历世界）；
    /// 第②段起 `Prepare` 改吃快照，标记就必须由收集侧给出 —— 否则消费侧无从区分，
    /// 会把实例化网格当普通网格再画一遍。
    bool bInstanced = false;
    /// **阴影投射者标记**（阶段 1 第③段第 2 批）：该条目的网格要进阴影 Pass。
    /// 【口径】= 组件类属**精确** `MeshComponent` / `CubeComponent` / `SphereComponent`
    /// 且 `castShadow == true` —— 与旧阴影路径的遍历范围逐条一致（它只枚举这三类；
    /// 广告牌/文字/贴花/样条/实例化/骨骼都不进阴影，`SplineMeshComponent` 虽派生自
    /// `MeshComponent` 但 `ForEach<T>` 按精确类型分桶，故天然排除）。
    /// 【已由 meshClass + castsShadow 取代？】保留本字段：它是"该类条目进阴影"的**单一判据**，
    /// 消费侧（四个技术）只需读它；`meshClass` 是更一般的类别信息，供 RSM/RT/合批等不同口径使用。
    bool bShadowCaster = false;
    /// 网格组件类别（见 `SnapshotMeshClass`）：各消费侧按自己的旧口径过滤
    SnapshotMeshClass meshClass = SnapshotMeshClass::Base;
    /// 组件的 `castShadow` 标志（阴影技术的口径里要用；与 `bShadowCaster` 的区别是后者已含"类别"过滤）
    bool castsShadow = false;
    /// RT/PT 专用材质补充（只对 RT 可见子集 `Base`/`Cube`/`Sphere` 填，见 `SnapshotRTMaterial`）
    SnapshotRTMaterial rtMaterial;
    /// 场景物体唯一 ID（调试、剔除统计、与 GPU Culling 的 objectID 对应）
    u32 objectID = 0;
    /// **来源实体 id**（`he::Entity::id`）。渲染侧仍有少量"逐实体"的状态机（骨骼缓冲的
    /// 脏标记/容量/退役队列、动画驱动的重建等），它们需要"从快照条目找回对应的实体状态"
    /// —— 这就是 E-2② 能做到"矩阵走快照、生命周期留渲染侧"的前提。
    /// 【注意】它不是指针，交接后只读 ✓（与"快照不放指针"不冲突）。
    u64 sourceEntity = 0;
    /// 可见性/实例化位标记（与 `GPUSceneObject::visibilityFlags` 同源）
    u32 visibilityFlags = 0;

    /// 间接绘制参数（与 `GPUSceneObject` 的三元组一致；**由 MeshBatcher 填充**，收集阶段恒为 0）
    u32 indexCount = 0;
    u32 firstIndex = 0;
    i32 vertexOffset = 0;
    /// 材质索引（与 `GPUSceneObject::materialIndex` 对应；组件里的 `materialID`）。
    /// 【与 `object.materialID` 不是一回事】后者是 bindless 纹理数组的**基索引**（着色器用它采样纹理），
    /// 前者是 GPU 剔除/间接绘制路径里的材质槽。两条路径都要，故都留在快照里。
    u32 materialIndex = 0;

    /// 蒙皮矩阵在 `FrameSceneSnapshot::skinMatrices` 里的**切片**（仅骨骼网格非 0）。
    /// 【为什么用扁平数组 + 偏移】快照里不放容器嵌套，渲染线程按 `[offset, offset+count)` 切片即可；
    /// 这也是 §4.2 里 `skinMatrices` 的设计意图（替代"读组件拿骨骼矩阵"）。
    u32 skinMatrixOffset = 0;
    u32 skinMatrixCount  = 0;
    /// 骨骼矩阵**版本号**（`SkeletalMeshComponent::boneMatrixVersion`）：渲染侧据此判断要不要重传
    /// 骨骼 SSBO（缓冲状态在渲染侧 `SkinnedMeshBuffers` 里，与第①段的实例缓冲同款）
    u32 skinMatrixVersion = 0;
    /// 骨架已加载（`SkeletalMeshComponent::skeleton != nullptr`）：旧骨骼循环的 `sm.skeleton` 判据；
    /// 未加载骨架的骨骼组件不进蒙皮 Pass（与旧行为一致）
    bool bHasSkeleton = false;
};

// 【布局钉子】物体数据必须能整块拷贝、且 16B 对齐（std430 要求）
static_assert(std::is_trivially_copyable_v<SnapshotDrawItem>,
              "SnapshotDrawItem 必须可平凡拷贝：渲染线程要整块上传");
static_assert(alignof(SnapshotDrawItem) % 16 == 0, "SnapshotDrawItem 必须满足 std430 的 16B 对齐");

/// 光源条目：**逐字段镜像** `GPULight`（64B，std430）。
/// 为什么不用内嵌（与物体数据相反）：光源要按元素写进光源 SSBO，逐字段定义能让下面的
/// `static_assert` 把每个字段的偏移都钉死 —— 一旦着色器侧改了字段顺序或类型，这里立刻编译失败。
struct SnapshotLight {
    float4 colorIntensity{0.0f};    // xyz=颜色, w=强度（物理模式下为照度/光强）
    float4 directionType{0.0f};     // xyz=方向, w=类型（0=Dir, 1=Point, 2=Spot）
    float4 positionRange{0.0f};     // xyz=位置, w=范围（**负值** = 物理模式标记，着色器取 abs）
    float2 coneAngles{0.0f};        // x=内锥角, y=外锥角（Spot 专用）
    i32    shadowIndex = -1;        // -1 = 无阴影；>=0 指向阴影数据数组
    float  shadowRadius = 0.0f;     // 光源半径（软阴影用，0=硬阴影）

    /// 转成 GPU 结构：两者布局由下面的 static_assert 保证一致，故整块拷贝即可
    [[nodiscard]] GPULight ToGpu() const {
        GPULight gpu{};
        std::memcpy(&gpu, this, sizeof(GPULight));
        return gpu;
    }
    [[nodiscard]] static SnapshotLight FromGpu(const GPULight& gpu) {
        SnapshotLight out{};
        std::memcpy(&out, &gpu, sizeof(GPULight));
        return out;
    }
};

// 【布局钉子】逐字段锁死：大小 + 每个字段的偏移都必须与 GPULight 完全一致。
// 只断言 sizeof 不够 —— 两个字段互换位置时大小不变，但着色器会读到错的数据。
static_assert(sizeof(SnapshotLight) == sizeof(GPULight), "SnapshotLight 必须与 GPULight 同大小");
static_assert(offsetof(SnapshotLight, colorIntensity) == offsetof(GPULight, colorIntensity), "colorIntensity 偏移漂移");
static_assert(offsetof(SnapshotLight, directionType)  == offsetof(GPULight, directionType),  "directionType 偏移漂移");
static_assert(offsetof(SnapshotLight, positionRange)  == offsetof(GPULight, positionRange),  "positionRange 偏移漂移");
static_assert(offsetof(SnapshotLight, coneAngles)     == offsetof(GPULight, coneAngles),     "coneAngles 偏移漂移");
static_assert(offsetof(SnapshotLight, shadowIndex)    == offsetof(GPULight, shadowIndex),    "shadowIndex 偏移漂移");
static_assert(offsetof(SnapshotLight, shadowRadius)   == offsetof(GPULight, shadowRadius),   "shadowRadius 偏移漂移");
static_assert(std::is_trivially_copyable_v<SnapshotLight>, "SnapshotLight 必须可平凡拷贝");

/// 天空盒条目（T1.4/T2.4）：渲染期读取天空盒的**唯一**入口。
/// 【为什么携带 RHI 资源裸指针】GI 的 IBL 设置接口（`SetIBLSkybox(texture, sampler)`）要的就是指针；
/// 它们指向**渲染资源**、不是 ECS 组件，因此不违反"快照不放组件指针"。句柄化
/// （T0.7 的 `RHITextureHandle`）会在资源表落地后替换这两个字段。
struct SnapshotSkybox {
    const rhi::IRHITexture* cubemap = nullptr;
    const rhi::IRHISampler* sampler = nullptr;
    bool                    enabled = false;   // 找到"启用且真的有 cubemap"的天空盒组件时为真
    /// 天空盒亮度倍率（`SkyboxComponent::intensity`）：`SkyboxPass` 的 push constant 要用它
    ///（阶段 1 §15.1 第③段 —— 该 Pass 改为只读快照，组件字段必须随快照一起走）
    float                   intensity = 1.0f;
};

/// 贴花条目（T1.4）：`DecalPass` 原先遍历 `DecalComponent` 两次（判空 + 逐贴花绘制），并按
/// `SceneGraph::GetWorldMatrix(e)` 取矩阵 —— 这些值在这里按值带走，渲染侧因此不再读 ECS。
/// 【为什么矩阵要单独带】Deferred 的物体收集会 `excludeDecals`（贴花卡片不进 `draws`），
/// 所以贴花的世界矩阵不能从 `draws` 里找。
struct SnapshotDecal {
    float4x4 worldMatrix{1.0f};        // 原 `sg.GetWorldMatrix(e)`（DecalPass.cpp:249）
    float2   size{0.0f};               // 组件 size（float2；:247/:272 只用到 x/y）
    float    projectionDepth = 0.0f;   // :273
    float    rotation        = 0.0f;   // :257
    float3   baseColorFactor{0.0f};    // :274
    float    metallicFactor  = 0.0f;   // :275
    float    roughnessFactor = 0.0f;   // :277
    float    opacity         = 0.0f;   // :208/:245/:274
    u32      materialID      = 0;      // :276/:278
    bool     hasBaseColorTexture = false;  // 由 `!baseColorTexture.empty()` 预先算好（:276）
};

/// 粒子发射器条目（T1.4）：渲染侧只按 id 驱动**自己**的缓冲，因此不必再缓存 `ParticleComponent*`。
/// 【为什么连参数一起收】渲染器的发射路径原本每帧从组件读**约 15 个字段**（方向/形状/速度/寿命/
/// 尺寸/纹理行列…，见 `ParticleRenderer::DispatchCompute` 的 emit 分支），只搬"位置"不足以让消费侧
/// 停止读组件；把 `ParticleSystemParam` 整份按值带进快照后，这一步才是机械替换。
struct SnapshotParticleEmitter {
    u32                     rendererId   = 0;   // 渲染器注册时分配的索引（组件上的 `rendererId`）
    he::ParticleSystemParam params{};           // 发射/模拟参数（按值拷贝，交接后只读）
    float3                  emitPosition{0.0f}; // 本帧发射原点（世界空间）
};

/// 实例化网格条目（阶段 1 第①段 / §15.1）：把"实例变换"与绘制所需的值按值带进快照，
/// 渲染侧因此不必再遍历 `InstancedMeshComponent`（B1 与组件指针两项闸门的收敛对象）。
/// 【为什么缓冲状态不在这里】逐网格的 GPU 实例缓冲（实例 SSBO、容量、退役队列、逐实例剔除的
/// 命令缓冲）是**渲染侧资源**，其生命周期留在渲染侧的状态表里（按 `meshIndex` 索引，见
/// `InstanceCuller::InstanceState`）—— 与骨骼缓冲 E-2② 同一处理原则：
/// **矩阵数据走快照、缓冲生命周期留渲染侧**。
struct SnapshotInstance {
    /// 渲染侧网格注册表索引：顶点/索引缓冲与索引数都据此去注册表查（是索引，不是指针）
    u32  meshIndex = 0;
    /// 实例变换在 `FrameSceneSnapshot::instanceTransforms` 里的切片（与 `skinMatrices` 同款：
    /// 快照里不放容器嵌套，渲染线程按 `[offset, offset+count)` 切片即可）
    u32  transformOffset = 0;
    u32  transformCount  = 0;
    /// 组件侧的**变换版本号**（`InstancedMeshComponent::instanceTransformVersion`）。
    /// 【为什么要它】缓冲状态搬到渲染侧之后，渲染侧不能再回读组件上的脏标记；它只比较
    /// "本帧版本 ≠ 上次上传的版本"来决定要不要重传 —— 这个判据跨帧幂等，适合另一根线程。
    u32  transformVersion = 0;
    /// 逐实例视锥剔除开关（原组件字段 `enableFrustumCull`）
    bool enableFrustumCull = false;
    /// 网格**局部** AABB（逐实例剔除 shader 的输入；原渲染期读 `组件.GetBounds()`）
    float3 localBoundsMin{0.0f};
    float3 localBoundsMax{0.0f};
    /// 来源实体 id：渲染侧状态表据此识别"注册表索引被回收后复用给了新网格"
    /// （换了主人就必须丢弃旧缓冲并强制重传，否则会画出上一个组件的实例）
    u64  sourceEntity = 0;
};

/// 物理天空（Preetham 解析模型）参数：`SkyboxPass` 渲染期读它的**唯一**入口，
/// 替代原先在 `SkyboxPass::Update(ctx)` 里的 `ctx.world->ForEach<PhysicalSkyComponent>`。
/// 【为什么整份按值带走】该 Pass 要用 intensity / sunDirection / turbidity / groundAlbedo /
/// sunIntensity 五个字段填 push constant —— 只带 `atmosphere` 里的"太阳方向 + 浑浊度"不够。
/// 【口径】取**第一个启用**的组件（与 `he::GetPhysicalSkySun` 逐条一致）。
struct SnapshotPhysicalSky {
    bool   enabled      = false;                  // 是否找到启用且可用的物理天空
    float3 sunDirection{0.0f, 1.0f, 0.0f};        // 世界空间太阳方向
    float  turbidity    = 0.0f;                   // 大气浑浊度
    float  groundAlbedo = 0.0f;                   // 地面反照率
    float  intensity    = 0.0f;                   // 天空整体亮度倍率
    float  sunIntensity = 0.0f;                   // 太阳盘亮度倍率
};

/// 阴影投射光源条目（阶段 1 第③段第 2 批）：四个阴影技术的 `CollectLights` 渲染期读它的
/// **唯一**入口，替代原先各自 `world.ForEach<XxxLight>` + `sg.GetWorldPosition(e)`。
/// 【为什么按值带全部字段】四个技术实际用到的字段一并列出（一处不漏）：
///   方向光 CSM：direction / shadowBias / shadowNormalBias / shadowStrength；
///   点光：position / range / 三个阴影参数；
///   聚光：direction / position / range / outerConeAngle / 三个阴影参数；
///   面光：normal（= `direction` 字段）/ position / range / softness / 三个阴影参数。
/// 【方向**不归一化**】旧口径是"取组件原值，由消费侧决定要不要 `glm::normalize`"
///（CSM 与 Spot 归一化、Rect 也归一化）—— 快照照抄原值，避免把归一化搬进收集侧而改变数值。
/// 【只收 `enabled && castShadow`】四个技术的过滤条件完全相同，故在收集侧一次过滤。
struct SnapshotShadowLight {
    u32    type = 0;               // `he::LightType`（0=Directional, 1=Point, 2=Spot, 3=Rect）
    float3 direction{0.0f};        // 方向光/聚光的方向；面光为法线（均**未归一化**）
    float3 position{0.0f};         // 世界位置（方向光为 0）
    float  range = 0.0f;           // 点/聚光/面光的范围
    float  outerConeAngle = 0.0f;  // 仅聚光
    float  softness = 0.0f;        // 仅面光（软阴影系数）
    float  shadowBias = 0.0f;
    float  shadowNormalBias = 0.0f;
    float  shadowStrength = 0.0f;
    u64    sourceEntity = 0;       // 来源实体 id（还原 `he::Entity` 供阴影下标映射用）
};

/// 一帧的完整渲染输入。游戏线程在 tick 结束后构造，交接后**只读**（铁律 2）。
struct FrameSceneSnapshot {
    u64        frameIndex = 0;              // 与 CommandQueue 的帧号对应（对账用）
    u32        frameSlot  = 0;              // 帧槽位（按帧轮换的资源用）
    CameraData camera{};                    // 本帧相机（TAA 抖动在渲染线程叠加，见 §5 阶段 2）
    f32        deltaTime  = 0.0f;
    u32        viewportWidth  = 0;
    u32        viewportHeight = 0;

    /// 空中透视（大气）参数：xyz = 太阳方向（指向太阳），w = 浑浊度（0 = 关闭）。    /// 与 `PushConstantData::atmosphere` / `DeferredLightingPushConstant::atmosphere` 逐字段一致；
    /// 由游戏线程从物理天空组件取（T1.4），渲染期因此不必再读世界。
    float4     atmosphere{0.0f, 1.0f, 0.0f, 0.0f};

    /// 物理天空（Preetham）参数的完整副本：`SkyboxPass` 渲染期的唯一入口（天空盒 Pass 与
    /// 光照用的 `atmosphere` 是两个消费者，前者还要 intensity/groundAlbedo/sunIntensity）
    SnapshotPhysicalSky physicalSky{};

    /// 天空盒（IBL 天空源）：渲染期读它的**唯一**入口，替代原先在帧图里 `world.ForEach<SkyboxComponent>`。
    SnapshotSkybox skybox{};

    /// 贴花（T1.4）：`DecalPass` 读它的**唯一**入口，替代原先两处 `world.ForEach<DecalComponent>`。
    std::vector<SnapshotDecal> decals;

    /// bindless 材质 SSBO 的内容：按 `materialID >> 2` 作槽位、已去重、已补齐空槽（值初始化）。
    /// 【为什么要进快照】`UploadMaterialBindless` 原先自己遍历组件收集材质 —— 那是"渲染期读 ECS"的一处；
    /// 搬进快照后，该函数不再需要 `World&`（B1 因此下降），且材质收集与物体收集共用同一份口径。
    std::vector<GPUMaterialData> materials;

    std::vector<SnapshotDrawItem> draws;
    std::vector<SnapshotLight>    lights;
    /// 光源的**来源实体 id**（与 `lights` 同序同长）。
    /// 【为什么要它】`SnapshotLight::shadowIndex` 只能在阴影收集（`ShadowSystem::Update`）**之后**解析，
    /// 而那时渲染侧手里只有快照；带上实体 id，就能用一个**不收 World** 的
    /// `ResolveLightShadowIndices(snapshot, resolver)` 事后补齐（见 `FrameSnapshotAssembler`）。
    std::vector<u64>              lightSourceEntities;
    /// 场景包围盒（网格包围盒 × **局部**变换，与 RSM 固定光锥的既有口径一致；无效 = min>max）。
    /// 【为什么要它】`ForwardPipeline::RefreshRSMFrustum` 原先自己遍历世界算它（每 30 帧一次），
    /// 那是渲染期读世界的一处 ⇒ 改由收集侧算好带进快照。
    float4                        sceneBoundsMin{0.0f};
    float4                        sceneBoundsMax{0.0f};
    /// 粒子发射器（渲染侧按 `rendererId` 驱动自己的缓冲；见 `SnapshotParticleEmitter`）
    std::vector<SnapshotParticleEmitter> particles;
    /// 骨骼矩阵（扁平数组：`boneOffset..boneOffset+boneCount` 属于某个 draw，
    /// 这样快照里不需要任何指针/容器嵌套，渲染线程按偏移切片即可）
    std::vector<float4x4>         skinMatrices;

    /// 实例化网格条目（阶段 1 第①段）：每个 `InstancedMeshComponent` 一条，
    /// **包括实例数为 0 的组件** —— 渲染侧靠"本帧又见到这个 meshIndex"来推进退役队列并回收
    /// 已销毁组件留下的缓冲（见 `InstanceCuller::BeginInstancesFrame`），漏掉空组件会让它的状态悬挂。
    std::vector<SnapshotInstance> instances;
    /// 实例变换的扁平数组（`SnapshotInstance::transformOffset` 切片；与 `skinMatrices` 同款）
    std::vector<float4x4>         instanceTransforms;

    /// 阴影投射光源（第③段第 2 批）：按 方向光 → 点光 → 聚光 → 面光 分类型、同类型按实体顺序。
    /// 【跨类型顺序无关紧要】四个技术各自只过滤自己的类型 ⇒ 只要同类型内保持实体顺序，
    /// 每个技术推出的 `GPUShadowData` 序列就与旧实现逐条一致（下标即 shader 的 `shadowIndex`）。
    std::vector<SnapshotShadowLight> shadowLights;

    /// 世界版本号（游戏线程每次结构性改动 +1）：渲染线程可据此判断"快照是否落后于世界"，
    /// 流式/缓存类模块（Lumen 表面缓存、Nanite 页表）用它做失效判断，避免又去读世界。
    u64 sourceWorldVersion = 0;

    [[nodiscard]] bool IsEmpty() const { return draws.empty() && lights.empty(); }

    /// 复用快照（下一帧重新填充）：清空内容但**保留容量**，避免每帧重新分配。
    void Clear() {
        draws.clear();
        lights.clear();
        particles.clear();
        skinMatrices.clear();
        instances.clear();
        instanceTransforms.clear();
        shadowLights.clear();
    }

    /// 预留容量（首帧/场景规模变化时调用一次，之后每帧 `Clear()` 复用）
    /// 【为什么要预留粒子】稳态下每帧只做 `Clear()` + 填充，**不允许在帧内分配**（帧内分配会引入
    /// 不可预期的耗时与锁，与"帧内不做同步等待"同一条纪律）。粒子数组此前漏了预留，见下方重载。
    /// 【实例数组的预留】实例变换是万级 `float4x4`（10000 实例 = 640KB）⇒ 逐帧重新分配代价明显，
    /// 调用方按首帧的实际规模自校准一次（与 `draws`/`skinMatrices` 同款做法）。
    void Reserve(u32 maxDraws, u32 maxLights, u32 maxSkinMatrices = 0, u32 maxParticles = 0,
                 u32 maxInstances = 0, u32 maxInstanceTransforms = 0) {
        draws.reserve(maxDraws);
        lights.reserve(maxLights);
        skinMatrices.reserve(maxSkinMatrices);
        particles.reserve(maxParticles);
        instances.reserve(maxInstances);
        instanceTransforms.reserve(maxInstanceTransforms);
    }
};

/// 快照本身**允许**带容器（它由游戏线程构造、整份移交），但不能含指针 —— 这一点由上面两个
/// 元素类型的静态断言 + 代码评审保证：元素里只有值、索引与矩阵。
static_assert(std::is_trivially_copyable_v<CameraData>, "CameraData 必须是值类型（快照要按值携带它）");

} // namespace he::render
