#pragma once

#include "Scene/Entity.h"   // he::Entity（按值出现在下面的解析器签名里，需要完整类型）
#include "Threading/FrameSceneSnapshot.h"

#include <functional>

namespace he {
class World;
class SceneGraph;
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

class SceneSnapshotBuilder {
public:
    /// 收集光源到快照，返回实际收集数（按 `kGPUMaxLights` 截断）。
    /// 口径与 `CollectLights` 逐字段对齐：遍历顺序（方向光 → 点光 → 聚光，同一组件类型按实体顺序）、
    /// 色温叠加、物理模式的**负范围标记**、聚光的锥角与（默认归一化的）方向、阴影索引解析。
    static u32 BuildLights(he::World& world, he::SceneGraph& sg,
                           const SceneSnapshotResolvers& resolvers,
                           FrameSceneSnapshot& out,
                           const SceneSnapshotLightOptions& options = {});
};

} // namespace he::render
