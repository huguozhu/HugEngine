#pragma once

#include "Scene/Entity.h"   // he::Entity（按值出现在下面的解析器签名里，需要完整类型）
#include "Pipeline/Material.h"   // PBRMaterial / GetDefaultMaterial（E-3：材质映射的唯一实现）
#include "Threading/FrameSceneSnapshot.h"

#include <functional>
#include <type_traits>   // if constexpr + is_base_of（E-3：只对真正的网格组件算材质）

namespace he {
class World;
class SceneGraph;
class SkeletalMeshComponent;   // 蒙皮矩阵的收集入口（实现里才需要完整类型）
class MeshComponent;           // E-3：材质映射的输入（实现里才需要完整类型）
class CubeComponent;           // 第③段第 2 批：阴影投射者的精确类型判定（`is_same_v` 只需声明）
class SphereComponent;         // 同上
} // namespace he

// ============================================================
// SceneSnapshotBuilder —— 游戏线程侧的快照构造器（方案 B §4.2 / 阶段 1 任务 T1.2）
//
// 【为什么集中到这里】光源收集原本有三份实现（`DeferredPipeline::CollectLights`、
// `ForwardPipeline::CollectLights`、`PathTracingPipeline` 各一份），且已经**出现漂移**：
//   · 聚光方向：Deferred 归一化、Forward 直接写原值；
//   · 点光的 `directionType.xyz`：Forward 写 (0,-1,0)、Deferred 留 0。
// 集中到一份之后，三管线共用同一口径，漂移不会再悄悄发生（差异要显式讨论）。
//
// 【本阶段覆盖范围】先落**光源**（T1.2a）；物体（`GPUScene::Collect` 的遍历 + 材质参数）
// 与骨骼/贴花/粒子在 T1.2b/T1.4 补齐 —— 它们要连带的资源解析（网格、材质槽、实例 SSBO）更多，
// 分开提交便于逐个对照。
//
// 【为什么注入而不是直读全局】快照层不读全局 CVar、也不依赖阴影系统：
// 物理光开关由调用方解析一次传进来（收集逻辑因此是纯函数、可单测），
// 阴影索引由调用方用它的阴影系统解析（快照层不反向依赖管线状态）。
// ============================================================

namespace he::rhi { class IRHIBuffer; }   // 注册器签名里用到（前向声明足够）

namespace he::render {

class MeshRegistry;   // 注册器签名里只用到引用（实现在 .cpp 里，需要完整类型）

/// 收集光源时的外部输入
struct SceneSnapshotResolvers {
    /// 全局"物理光照单位"开关（`r.Light.PhysicalUnits`）—— 调用方解析一次，避免收集逻辑读全局状态
    bool physicalUnitsEnabled = false;
    /// ECS 实体 → 阴影数据索引（-1 = 不投影）。为空时一律 -1（与"阴影系统未创建"的旧行为一致）
    std::function<i32(he::Entity)> shadowIndex;
};

/// 光源收集的**口径开关**：默认值是 Deferred 管线的现行行为（= 逐像素基线）。
/// 【为什么要有它】三条管线在历史上出现了 **4 处漂移**（逐一列在下面），而 T1.3 的迁移必须
/// **先保持各管线逐像素一致**、再单独决定"统一成哪一个"。把差异做成显式开关，而不是让某条管线
/// 悄悄跟着另一条变：这样"迁移"与"口径变更"是两次可分别验证、可分别回退的改动。
/// 【已登记的四处理差异】（迁移完成后应单独立项统一，并如实标注是"修正"还是"改版"）
///   ① `RectLight`：Forward 收集（`directionType.w=3`、`coneAngles` 复用为宽×高），
///      Deferred / PathTracing **完全不收集**；
///   ② 点光 `directionType.xyz`：Forward 写 (0,-1,0)，Deferred / PathTracing 留 0；
///   ③ 聚光方向：Deferred / PathTracing **归一化**，Forward 写原值；
///   ④ `shadowRadius`：PathTracing 写 `lc.shadowRadius`，Deferred / Forward 不写（保持 0）。
struct SceneSnapshotLightOptions {
    /// 是否收集 `RectLight`（默认 false = Deferred / PathTracing 的现状）
    bool includeRectLights = false;
    /// 点光的 `directionType.xyz` 是否写 (0,-1,0)（默认 false = Deferred / PathTracing 的现状）
    bool pointLightWritesDirection = false;
    /// 聚光方向是否归一化（默认 true = Deferred / PathTracing 的现状，也更正确）
    bool normalizeSpotDirection = true;
    /// 是否把 `lc.shadowRadius` 写进 `shadowRadius`（默认 false；PathTracing 需要 true）
    bool writeShadowRadius = false;
};

/// 物体收集的口径开关（与光源同理：先保调用方现状，统一另立改动）
struct SceneSnapshotObjectOptions {
    /// 是否排除贴花（Deferred 用 `DecalPass` 把贴花投影进 GBuffer ⇒ 它不进场景物体列表；
    /// 其它管线按组件收集）。默认 false = 包含。
    bool excludeDecals = false;
};

class SceneSnapshotBuilder {
public:
    // ── 光源（T1.2a）──────────────────────────────────────────────

    /// 收集光源到快照，返回实际收集数（按 `kGPUMaxLights` 截断）。
    /// 口径与 `CollectLights` 逐字段对齐：遍历顺序（方向光 → 点光 → 聚光，同一组件类型按实体顺序）、
    /// 色温叠加、物理模式的**负范围标记**、聚光的锥角与（默认归一化的）方向、阴影索引解析。
    static u32 BuildLights(he::World& world, he::SceneGraph& sg,
                           const SceneSnapshotResolvers& resolvers,
                           FrameSceneSnapshot& out,
                           const SceneSnapshotLightOptions& options = {});

    // ── 物体（T1.2b）──────────────────────────────────────────────

    /// 单个组件 → 一条 draw item 的映射（与 `GPUScene::Collect` 的 `FillObj` 逐字段对齐）。
    /// 【为什么把它单独暴露成模板】组件存储按**精确类型**分桶（`ForEach<T>` 用 `type_index`），
    /// 所以"假组件"派生自 `CubeComponent` 也进不了同一桶 ⇒ 遍历部分无法单测。
    /// 把映射抽成模板后，单测可以用一个只提供 `GetIndexCount()/GetBounds()/materialID` 的假类型
    /// 直接验证它（duck typing），遍历部分则由代码检查 + T1.3b-3 的集成验证覆盖。
    /// @param worldMatrix 已算好的世界矩阵（广告牌会被调用方换成对齐相机的矩阵）
    /// @param objectID    收集序号（= 在 `out.draws` 里的下标，与旧路径一致）
    /// @param prev        上一帧快照（可为空）：用于取 `prevWorldMatrix`（TAA/运动矢量需要）
    /// @param bInstanced  实例化/骨骼网格（顶点由专用路径提供，普通绘制循环跳过）
    template <typename TComponent>
    static void CollectObjectItem(he::Entity entity, TComponent& comp, const float4x4& worldMatrix,
                                  u32 objectID, const FrameSceneSnapshot* prev,
                                  FrameSceneSnapshot& out, bool bInstanced = false) {
        if (comp.GetIndexCount() == 0u) return;      // 旧路径：无索引的组件不进场景物体列表

        SnapshotDrawItem item{};
        // 来源实体 id（附录 E / E-2②）：渲染侧逐实体状态机（骨骼缓冲的脏标记/容量/退役队列）
        // 靠它从快照条目找回对应状态 ⇒ "矩阵走快照、生命周期留渲染侧"才成立。
        item.sourceEntity = entity.id;
        item.bInstanced   = bInstanced;
        item.object.worldMatrix = worldMatrix;
        const he::AABB worldBounds = comp.GetBounds().Transform(worldMatrix);
        item.object.boundsMin = float4(worldBounds.min, 0.0f);
        item.object.boundsMax = float4(worldBounds.max, 0.0f);
        item.object.materialID = comp.materialID;    // bindless 纹理基索引（着色器采样用）
        item.materialIndex     = comp.materialID;    // GPU 剔除/间接绘制路径的材质槽
        // 渲染侧网格注册表索引（附录 E / E-2）：组件上的 `meshIndex` 由加载期注册时回填，
        // 0 = 未注册 ⇒ 消费侧 `Find(0)` 返回空并跳过（可见化，而不是指错资源）。
        item.meshIndex         = comp.meshIndex;
        item.objectID          = objectID;

        // 材质参数（E-3）：**在收集侧就算完** —— 复用 `MakePBRMaterial` + `FillObjectData`（纯计算），
        // 把结果直接放进 `item.object`。这样渲染侧消费快照时不必再拿 `MeshComponent*`，
        // 也不必把 5 条纹理路径字符串搬进快照。
        // `if constexpr` 保护：模板会被非网格组件（以及单测里的假类型）实例化，那些类型没有材质字段；
        // 真正的网格组件（`MeshComponent` 及其派生 Cube/Sphere/Billboard/Decal/…）都会走这一支。
        if constexpr (std::is_base_of_v<he::MeshComponent, TComponent>) {
            FillObjectData(item.object, MakePBRMaterial(comp));
        }
        item.visibilityFlags   = 1u;                 // 与 `FillObj` 一致（"可见"，剔除在渲染线程做）
        // 阴影投射者（第③段第 2 批）：口径与旧阴影路径的遍历范围逐条一致 ——
        // **精确**的 `MeshComponent` / `CubeComponent` / `SphereComponent` + `castShadow`。
        // 用 `if constexpr` 判定（`SplineMeshComponent` 虽派生自 MeshComponent，但 `ForEach<T>`
        // 按精确类型分桶，走的是它自己的实例化，故不会命中这一支）。
        if constexpr (std::is_same_v<TComponent, he::MeshComponent> ||
                      std::is_same_v<TComponent, he::CubeComponent> ||
                      std::is_same_v<TComponent, he::SphereComponent>) {
            item.bShadowCaster = comp.castShadow;
        }
        // meshIndex / indexCount / firstIndex / vertexOffset 由 MeshBatcher 在 Prepare 阶段填充
        // 上一帧世界矩阵：按**下标**对齐（与 GPUScene 的 `m_CachedMatrices[idx]` 同一假设）。
        // 首帧（prev 为空或该下标不存在）取当前矩阵 ⇒ 运动矢量为 0，与既有行为一致。
        if (prev && objectID < prev->draws.size()) {
            item.prevWorldMatrix = prev->draws[objectID].object.worldMatrix;
        } else {
            item.prevWorldMatrix = worldMatrix;
        }
        out.draws.push_back(item);
    }

    /// 收集全部可渲染物体（遍历顺序与 `GPUScene::Collect` 的**首次全量**分支逐条对齐）。
    /// @param prev 上一帧快照（可为空）
    /// @return 收集到的物体数
    static u32 BuildObjects(he::World& world, he::SceneGraph& sg, const CameraData& camera,
                            const SceneSnapshotObjectOptions& options,
                            const FrameSceneSnapshot* prev, FrameSceneSnapshot& out);

    // ── 蒙皮（T1.4：骨骼矩阵）────────────────────────────────────

    /// 把骨骼网格的**蒙皮矩阵**追加到快照的扁平数组，并给该 draw item 记下切片。
    /// 【数据来源】`SkeletalMeshComponent::boneMatrices`（CPU 侧，= world × inverseBind，正是 GPU SSBO
    /// 需要的形态）；渲染侧因此不必再遍历组件去取它。
    /// 【为什么单列一个函数】`CollectObjectItem` 是"单组件 → 单条目"的纯映射，而蒙皮要写**共享的**
    /// 扁平数组、还要回填条目的切片 ⇒ 分成两步，且这一步可以单独单测（不需要网格索引数据）。
    static void AppendSkinMatrices(SnapshotDrawItem& item, const he::SkeletalMeshComponent& comp,
                                   FrameSceneSnapshot& out);

    // ── 材质映射（E-3）────────────────────────────────────────────

    /// 组件 → `PBRMaterial` 的**唯一实现**（原先内联在 `SceneRenderer::Prepare` 里）。
    /// 抽出来的目的：让"收集侧算材质"（E-3 的目标）与渲染侧共用一份口径，避免各自漂移。
    static PBRMaterial MakePBRMaterial(const he::MeshComponent& comp);

    // ── 粒子发射器（T1.4）────────────────────────────────────────

    // ── 材质（T1.4/T1.5：让 bindless 上传不再读 ECS）──────────────

    /// 收集 bindless 材质数组：按 `materialID` 去重后摊平成 `GPUMaterialData`，并按
    /// `materialID >> 2` 作槽位补齐空槽（空槽值初始化 = 全 0），保证 shader 侧的索引对齐。
    /// 【口径来源】原先是 `ForwardPipeline::UploadMaterialBindless` 里的 collect+去重+补槽逻辑
    /// （`MeshComponent / Cube / Sphere / Instanced / Skeletal / Spline` 都收集）；搬到这里后
    /// **渲染侧不再为了材质遍历世界**。每次调用会清空重填（逐帧复用安全）。
    /// @return 材质槽位数（= 上传时的 buffer 元素数）
    static u32 BuildMaterials(he::World& world, FrameSceneSnapshot& out);

    // ── 贴花（T1.4：渲染期不再读 DecalComponent）──────────────────

    /// 收集贴花：把 `DecalPass` 原先两处遍历读到的字段按值带走（含 `sg.GetWorldMatrix(e)`）。
    /// 【为什么矩阵要单独带】Deferred 的物体收集 `excludeDecals` ⇒ 贴花不在 `draws` 里，取不到矩阵。
    /// 每次调用清空重填（逐帧复用安全）。保留全部贴花（含 `opacity<=0`），由消费侧按原口径过滤 ——
    /// 这样"判空"与"绘制"的过滤条件仍在同一处（`DecalPass`），迁移不改语义。
    /// @return 贴花数
    static u32 BuildDecals(he::World& world, he::SceneGraph& sg, FrameSceneSnapshot& out);

    // ── 天空盒（T1.4：渲染期不再读 SkyboxComponent）──────────────

    /// 收集天空盒（IBL 天空源）：取"启用且真的有 cubemap"的组件。
    /// 【口径】与原先帧图里的循环逐条一致：逐个赋值 ⇒ 后一个符合条件的组件覆盖前一个；
    /// 一个都没有时 `enabled=false`、指针为空（调用方据此跳过 IBL 天空设置）。
    /// @return 是否找到可用天空盒
    static bool BuildSkybox(he::World& world, FrameSceneSnapshot& out);

    // ── 实例化网格（阶段 1 第①段 / §15.1）────────────────────────

    /// 收集实例化网格：把**实例变换**（扁平数组 + 切片）与绘制所需的开关/局部 AABB/版本号
    /// 按值带进快照，渲染侧因此不必再遍历 `InstancedMeshComponent`。
    /// 【必须包含实例数为 0 的组件】渲染侧靠"本帧又见到这个 meshIndex"回收已销毁组件留下的
    /// 缓冲与 bindless 槽位（见 `InstanceCuller::BeginInstancesFrame`）；只收集非空组件会让
    /// 空的（或刚被清空的）组件的状态悬挂。
    /// 【调用顺序】必须在 `RegisterMeshes`（回填 `meshIndex`）之后 —— 否则条目带的是 0（未注册）。
    /// 每次调用清空重填（逐帧复用安全）。
    /// @return 条目数（= 世界里的实例化网格组件数）
    static u32 BuildInstances(he::World& world, FrameSceneSnapshot& out);

    // ── 阴影投射光源（阶段 1 第③段第 2 批）────────────────────────

    /// 收集**投射阴影**的光源（`enabled && castShadow`）：四个阴影技术的 `CollectLights`
    /// 原先各自 `world.ForEach<XxxLight>` + `sg.GetWorldPosition(e)`，现由本函数在收集侧一次取齐。
    /// 【顺序】方向光 → 点光 → 聚光 → 面光（与 `BuildLights` 同序、同类型内按实体顺序）：
    /// 各技术只过滤自己的类型，故该顺序足以保证每个技术推出的序列与旧实现逐条一致。
    /// 【方向**不归一化**】照抄组件原值（旧口径由消费侧 `glm::normalize`）。
    /// 每次调用清空重填（逐帧复用安全）。
    /// @return 条目数
    static u32 BuildShadowLights(he::World& world, he::SceneGraph& sg, FrameSceneSnapshot& out);

    // ── 网格注册（附录 E / E-2①，唯一实现）──────────────────────

    /// 把世界中**所有**可绘制网格组件登记/更新进注册表，并回填组件上的 `meshIndex`。
    /// 【为什么必须覆盖全部类型】绘制路径（E-3②）要靠 `meshIndex` 从注册表取顶点/索引缓冲；
    /// 只登记骨骼网格的话，普通网格 `meshIndex == 0` ⇒ 消费侧只能继续留指针兜底，闸门降不下来。
    /// 【为什么每帧调用】骨骼缓冲会重建（`RetireBoneBuffer` 走 N 帧延迟队列后新建）⇒ 一次性注册会
    /// 留过期指针；`Register` 同 key = 更新、索引不变，因此每帧刷新廉价且安全。
    /// 【为什么收在这里】原先三条管线各抄一份只登记骨骼网格的代码 —— 三份口径必然漂移。
    /// @return 登记到的网格数
    static u32 RegisterMeshes(he::World& world, MeshRegistry& registry);

    /// 收集粒子发射器列表（`{rendererId, 发射位置}`）。
    /// 【为什么只需要这两个字段】粒子模拟与绘制早就按 id 索引驱动渲染器自有缓冲
    /// （`ParticleRenderer::DispatchCompute(cmd, id, dt, viewProj)`），渲染期唯一读组件的地方是
    /// "发射位置"（`CompState` 里缓存了 `ParticleComponent*`）。把 `{id, 位置}` 放进快照后，
    /// 渲染侧不再需要组件指针。
    /// @return 发射器数量
    static u32 BuildParticles(he::World& world, FrameSceneSnapshot& out);

    // ── 环境（T1.4 起）────────────────────────────────────────────

    /// 收集"空中透视"参数（太阳方向 + 浑浊度）到快照。
    /// 原来两处渲染期代码直接读世界（`ForwardPipeline::CollectLights` 与
    /// `DeferredPipeline` 的帧图），T1.4 起统一走快照 —— 渲染期不再出现这次世界读。
    /// @return 是否找到启用的物理天空组件（false 时 `atmosphere` 复位为"关闭"：方向 (0,1,0)、浑浊度 0）
    static bool BuildEnvironment(he::World& world, FrameSceneSnapshot& out);
};

} // namespace he::render
