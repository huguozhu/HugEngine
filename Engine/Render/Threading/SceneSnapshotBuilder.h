#pragma once

#include "Scene/Entity.h"   // he::Entity（按值出现在下面的解析器签名里，需要完整类型）
#include "Threading/FrameSceneSnapshot.h"

#include <functional>

namespace he {
class World;
class SceneGraph;
class SkeletalMeshComponent;   // 蒙皮矩阵的收集入口（实现里才需要完整类型）
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

namespace he::render {

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
    template <typename TComponent>
    static void CollectObjectItem(he::Entity entity, TComponent& comp, const float4x4& worldMatrix,
                                  u32 objectID, const FrameSceneSnapshot* prev,
                                  FrameSceneSnapshot& out) {
        if (comp.GetIndexCount() == 0u) return;      // 旧路径：无索引的组件不进场景物体列表

        SnapshotDrawItem item{};
        // 来源实体 id（附录 E / E-2②）：渲染侧逐实体状态机（骨骼缓冲的脏标记/容量/退役队列）
        // 靠它从快照条目找回对应状态 ⇒ "矩阵走快照、生命周期留渲染侧"才成立。
        item.sourceEntity = entity.id;
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
        item.visibilityFlags   = 1u;                 // 与 `FillObj` 一致（"可见"，剔除在渲染线程做）
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

    // ── 粒子发射器（T1.4）────────────────────────────────────────

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
