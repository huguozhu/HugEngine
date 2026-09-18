#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include "Scene/MeshComponent.h"     // StaticVertex / TArray
#include "Scene/CollisionSystem.h"   // CollisionWorldShape（与检测共用的世界形状）

// ============================================================
// CollisionDebugSystem — 碰撞体调试线框生成（任务 26）
//
// 对每个带 CollisionComponent 的实体：用 CollisionSystem::ExtractWorldShape 取**同一个**
// 世界形状（与检测共用语义，避免"线框画得好看、实际碰撞不是那样"），三角化成十字交叉的
// 细带线段，写进同实体上的 CollisionDebugComponent（MeshComponent）→ 走现有网格渲染路径。
//
// 形状线框：
//   AABB   ：12 条棱
//   Sphere ：3 个正交大圆（XY / YZ / XZ）
//   Capsule：上下两个圆 + 两条竖线 + 两端半球弧（弧用 8 段近似）
//
// 【为什么十字片】每段线生成两片互相垂直的细四边形 ⇒ 不需要相机信息（billboard 需要），
// 任意视角都能看到；代价是顶点数 ×8，但调试线框的量级（每形状数百顶点）可以忽略。
// ============================================================

namespace he {
struct Entity;
class World;
class CollisionDebugComponent;

class CollisionDebugSystem {
public:
    /// 大圆/圆环的段数（越大越圆；24 段在 1~2 米尺度上已看不出折线）
    static constexpr u32 kCircleSegments = 24;
    /// 胶囊半球弧的段数（8 段足够表达弧顶）
    static constexpr u32 kCapArcSegments = 8;

    /// 刷新全部碰撞体的调试线框。
    /// @param enabled false = 清空已有线框几何（组件保留，便于再打开）
    /// @return 本次绘制（或清空）的碰撞体数
    static u32 Update(World& world, bool enabled);

    /// 确保实体上存在调试线框组件（示例/编辑器按需调用），返回该组件（失败为 nullptr）
    static CollisionDebugComponent* Ensure(World& world, Entity e);

    /// 单形状线框生成（纯几何，无 RHI 依赖；单元测试直接调它做判据）
    /// @param outVertices/outIndices 追加写入（调用方先清空）
    /// @return 线段数
    static u32 BuildWireframe(const CollisionWorldShape& shape, float thickness,
                              TArray<StaticVertex>& outVertices, TArray<u32>& outIndices);
};

} // namespace he
