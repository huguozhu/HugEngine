#pragma once

#include "Scene/MeshComponent.h"
#include "Scene/CollisionSystem.h"   // CollisionWorldShape（缓存用于"形状变了才重建"）

// ============================================================
// CollisionDebugComponent — 碰撞体调试线框（任务 26）
//
// 挂在**同一个实体**上，由 CollisionDebugSystem 每帧（形状变化时）把它对应的
// CollisionComponent 画成线框网格：AABB / 球 / 胶囊。
//
// 【为什么做成 MeshComponent 而不是"新 PSO + 线段绘制"】线框本质是"带颜色的几何"：
// 复用现有网格路径（顶点/索引缓冲 + 材质 + Forward/Deferred 两条管线）就不用再往 RHI 里
// 塞一套线拓扑管线；代价是"线"被三角化成十字交叉的两片细带（见 CollisionDebugSystem），
// 对调试可视化完全够用，而且不需要相机参数（交叉片在任意视角都能看到）。
//
// 默认渲染方式：无光照 + 半透明混合 + 双面 + 不投影（调试几何不该影响阴影与光照）。
// ============================================================

namespace he {

class CollisionDebugComponent : public MeshComponent {
    HE_COMPONENT()
public:
    /// 线框颜色（默认青绿色，alpha<1 便于叠加观察）
    float4 color = float4(0.15f, 1.0f, 0.45f, 0.8f);
    /// 线宽（世界单位，米）：十字片的半宽
    float  lineThickness = 0.02f;

    // --- 运行时（CollisionDebugSystem 管理，勿手动改）---
    u32 segmentCount = 0;                 // 当前线框的线段数（0 = 未绘制）
    /// 上一次生成的形状快照（用于"形状没变就不重建网格"，避免每帧重建 GPU 缓冲）
    CollisionWorldShape cachedShape;
    float cachedThickness = 0.0f;
    bool  bHasCache = false;
};

} // namespace he
