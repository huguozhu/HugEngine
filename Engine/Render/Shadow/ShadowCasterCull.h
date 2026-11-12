// ============================================================
// ShadowCasterCull.h — T10：阴影投射者的**逐光源/逐级联**剔除（纯函数，RHI-free）
//
// 【为什么单独抽出来】CSM 之前把"所有 `bShadowCaster` 物体"逐个画进**每个**级联：
//   级联之间只差裁剪矩阵，物体集合完全一样。T10 要求"逐光源剔除与派发参数"，
//   第一步就是把"某个级联的视锥里到底有哪些物体"变成可判定的纯函数 —— 这样
//   剔除口径能被单测钉死，不必靠截图对比。
//
// 【口径】与 Nanite 侧完全一致：平面 `(nx,ny,nz,d)`，判据 `dot(n, p) + d >= 0` 为内侧；
//   AABB 用**正顶点**（每个平面取最靠近外侧的角）做保守判定 —— 只要正顶点在平面外侧，
//   整个盒子就完全在外侧。
// ============================================================
#pragma once

#include "Nanite/NaniteTypes.h"   // `NaniteFrustumPlanes` / `NaniteExtractFrustumPlanes`

namespace he::render {

/// 一个阴影投射者的世界空间 AABB（与 `SnapshotDrawItem::object.boundsMin/Max` 同口径）
struct ShadowCasterBounds {
    float min[3] = { 0.0f, 0.0f, 0.0f };
    float max[3] = { 0.0f, 0.0f, 0.0f };
};

/// 世界 AABB 是否与视锥相交（保守：宁可多留不可漏掉）
[[nodiscard]] inline bool ShadowAabbVisibleInFrustum(const NaniteFrustumPlanes& frustum,
                                                     const ShadowCasterBounds& bounds) {
    for (u32 p = 0u; p < 6u; ++p) {
        const float* n = frustum.planes[p];
        // 正顶点：每个轴取"使 dot(n, ·) 最大"的那个端角
        const float px = (n[0] >= 0.0f) ? bounds.max[0] : bounds.min[0];
        const float py = (n[1] >= 0.0f) ? bounds.max[1] : bounds.min[1];
        const float pz = (n[2] >= 0.0f) ? bounds.max[2] : bounds.min[2];
        if (n[0] * px + n[1] * py + n[2] * pz + n[3] < 0.0f) return false;   // 整个盒子在外侧
    }
    return true;
}

/// 从一组投射者里挑出与视锥相交的下标。
/// @param outCapacity  输出容量（超出时**不**写，返回值仍是"可见总数"，便于调用方判断截断）
/// @return 可见总数（未截断）
[[nodiscard]] inline u32 CullShadowCasters(const NaniteFrustumPlanes& frustum,
                                           const ShadowCasterBounds* casters,
                                           u32 casterCount,
                                           u32* outIndices,
                                           u32 outCapacity) {
    if (casters == nullptr || casterCount == 0u) return 0u;
    u32 written = 0u;
    for (u32 i = 0u; i < casterCount; ++i) {
        if (!ShadowAabbVisibleInFrustum(frustum, casters[i])) continue;
        if (outIndices != nullptr && written < outCapacity) outIndices[written] = i;
        ++written;
    }
    return written;
}

}  // namespace he::render
