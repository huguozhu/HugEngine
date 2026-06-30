#include "Math/Geometry.h"
#include <glm/gtc/matrix_access.hpp>

namespace he {

// ============================================================
// Frustum::FromViewProj — 从视图投影矩阵提取 6 个裁剪平面
//
// 使用 Gribb/Hartmann 方法：列组合提取平面系数，
// 适用于 Vulkan [0,1] 深度范围。
// ============================================================
Frustum Frustum::FromViewProj(const float4x4& vp) {
    Frustum f;

    // Gribb/Hartmann 平面提取：使用行组合（row3 ± rowN）。
    // GLM 列主序：vp[col][row]，row3 的 4 个分量分布在不同列的第 3 元素。
    // 左平面 = row3 + row0 — 所有分量取自 row3(即 vp[col][3]) + row0(vp[col][0])
    // 辅助：提取 row3 ± rowN 的平面系数（Gribb/Hartmann 行组合法）
    auto makePlane = [&](u32 rowN, bool add) -> float4 {
        float sign = add ? 1.0f : -1.0f;
        return float4(
            vp[0][3] + sign * vp[0][rowN],
            vp[1][3] + sign * vp[1][rowN],
            vp[2][3] + sign * vp[2][rowN],
            vp[3][3] + sign * vp[3][rowN]
        );
    };

    f.planes[0] = makePlane(0, true);   // Left:   row3 + row0
    f.planes[1] = makePlane(0, false);  // Right:  row3 - row0
    f.planes[2] = makePlane(1, true);   // Bottom: row3 + row1
    f.planes[3] = makePlane(1, false);  // Top:    row3 - row1
    f.planes[4] = makePlane(2, true);   // Near:   row3 + row2
    f.planes[5] = makePlane(2, false);  // Far:    row3 - row2

    // 归一化（不取反，保留 Gribb/Hartmann 原始法线方向）
    for (u32 i = 0; i < 6; ++i) {
        float len = glm::length(float3(f.planes[i]));
        if (len > 0.0001f)
            f.planes[i] /= len;
    }

    return f;
}

// ============================================================
// Frustum::Intersects — AABB 与视锥体相交测试
//
// 对每个裁剪平面，测试 AABB 全部 8 个角点。
// 若所有角点都在任一平面外侧 → 完全剔除。
// Gribb/Hartmann 提取的平面：内部点满足 dot(normal, P) + d >= 0。
// ============================================================
bool Frustum::Intersects(const AABB& box) const {
    // 预计算 8 个角点
    float3 corners[8] = {
        float3(box.min.x, box.min.y, box.min.z),
        float3(box.max.x, box.min.y, box.min.z),
        float3(box.min.x, box.max.y, box.min.z),
        float3(box.max.x, box.max.y, box.min.z),
        float3(box.min.x, box.min.y, box.max.z),
        float3(box.max.x, box.min.y, box.max.z),
        float3(box.min.x, box.max.y, box.max.z),
        float3(box.max.x, box.max.y, box.max.z),
    };

    for (u32 i = 0; i < 6; ++i) {
        const float4& p = planes[i];
        bool anyInside = false;
        for (u32 c = 0; c < 8; ++c) {
            // 内部：dot(normal, corner) + d >= 0
            if (glm::dot(float3(p), corners[c]) + p.w >= 0.0f) {
                anyInside = true;
                break;
            }
        }
        if (!anyInside)
            return false;  // 所有角点都在此平面外侧 → 剔除
    }
    return true;
}

} // namespace he
