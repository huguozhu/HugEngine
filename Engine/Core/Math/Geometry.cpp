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
    // Vulkan [0,1]: 近平面 z>=0 → row2，非 OpenGL 的 row3+row2 (z>=-w)
    f.planes[4] = float4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);  // Near: row2
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
// Frustum::Intersects(AABB) — p-vertex/n-vertex 优化实现
//
// 每平面只测 2 个角点（最正/最负），从 48 次 dot 降到 12 次。
// 平面公式：dot(normal, P) + d >= 0 表示内部。
// ============================================================
bool Frustum::Intersects(const AABB& box) const {
    for (u32 i = 0; i < 6; ++i) {
        const float3& n = *reinterpret_cast<const float3*>(&planes[i]);
        float d = planes[i].w;

        // p-vertex：平面法线各分量正负对应的 AABB 角点（离平面最远的角点）
        float3 pVertex(
            n.x > 0.0f ? box.max.x : box.min.x,
            n.y > 0.0f ? box.max.y : box.min.y,
            n.z > 0.0f ? box.max.z : box.min.z
        );

        // p-vertex 都在外侧 → 完全剔除
        // 加小 epsilon 防止近平面精度问题和边界误判
        if (glm::dot(n, pVertex) + d + 0.001f < 0.0f)
            return false;
    }
    return true;
}

// ============================================================
// Frustum::Intersects(Sphere) — 球体与视锥体相交测试
//
// 计算球心到每个平面的有符号距离，若距离 < -radius 则完全在外。
// ============================================================
bool Frustum::Intersects(const Sphere& sphere) const {
    for (u32 i = 0; i < 6; ++i) {
        const float3& n = *reinterpret_cast<const float3*>(&planes[i]);
        float d = planes[i].w;
        float dist = glm::dot(n, sphere.center) + d;
        if (dist < -sphere.radius)
            return false;
    }
    return true;
}

// ============================================================
// Frustum::Contains — 点是否在视锥体内
// ============================================================
bool Frustum::Contains(const float3& point) const {
    for (u32 i = 0; i < 6; ++i) {
        const float3& n = *reinterpret_cast<const float3*>(&planes[i]);
        if (glm::dot(n, point) + planes[i].w < 0.0f)
            return false;
    }
    return true;
}

// ============================================================
// Ray::IntersectsAABB — slab 方法 AABB 相交测试
//
// 对每个轴计算进入/离开参数，返回最近交点范围 [tMin, tMax]。
// tMin <= tMax 且 tMax >= 0 表示相交。
// ============================================================
bool Ray::IntersectsAABB(const AABB& box, float& tMin, float& tMax) const {
    tMin = 0.0f;
    tMax = FLT_MAX;

    // X 轴
    {
        float invD = 1.0f / direction.x;
        float t0 = (box.min.x - origin.x) * invD;
        float t1 = (box.max.x - origin.x) * invD;
        if (invD < 0.0f) std::swap(t0, t1);
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) return false;
    }
    // Y 轴
    {
        float invD = 1.0f / direction.y;
        float t0 = (box.min.y - origin.y) * invD;
        float t1 = (box.max.y - origin.y) * invD;
        if (invD < 0.0f) std::swap(t0, t1);
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) return false;
    }
    // Z 轴
    {
        float invD = 1.0f / direction.z;
        float t0 = (box.min.z - origin.z) * invD;
        float t1 = (box.max.z - origin.z) * invD;
        if (invD < 0.0f) std::swap(t0, t1);
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) return false;
    }

    return tMax >= 0.0f;
}

// ============================================================
// Ray::IntersectsSphere — 射线-球体相交
// ============================================================
bool Ray::IntersectsSphere(const Sphere& sphere, float& t) const {
    float3 oc = origin - sphere.center;
    float b  = glm::dot(oc, direction);
    float c  = glm::dot(oc, oc) - sphere.radius * sphere.radius;
    float disc = b * b - c;

    if (disc < 0.0f) return false;

    float sqrtDisc = std::sqrt(disc);
    float t0 = -b - sqrtDisc;
    float t1 = -b + sqrtDisc;

    t = (t0 >= 0.0f) ? t0 : t1;
    return t >= 0.0f;
}

// ============================================================
// Ray::IntersectsTriangle — Möller-Trumbore 算法
//
// 射线与三角形相交，返回交点参数 t 和重心坐标 (u, v)。
// ============================================================
bool Ray::IntersectsTriangle(const float3& v0, const float3& v1,
                              const float3& v2, float& t, float& u, float& v) const {
    float3 e1 = v1 - v0;
    float3 e2 = v2 - v0;
    float3 h  = glm::cross(direction, e2);
    float  a  = glm::dot(e1, h);

    // 射线平行于三角形
    if (std::abs(a) < 1e-7f) return false;

    float f = 1.0f / a;
    float3 s = origin - v0;
    u = f * glm::dot(s, h);

    if (u < 0.0f || u > 1.0f) return false;

    float3 q = glm::cross(s, e1);
    v = f * glm::dot(direction, q);

    if (v < 0.0f || u + v > 1.0f) return false;

    t = f * glm::dot(e2, q);
    return t >= 0.0f;
}

// ============================================================
// Sphere::Intersects(AABB) — 最近点法
//
// 计算 AABB 上离球心最近的点，检测其距离是否 <= radius。
// ============================================================
bool Sphere::Intersects(const AABB& box) const {
    float3 closest(
        glm::clamp(center.x, box.min.x, box.max.x),
        glm::clamp(center.y, box.min.y, box.max.y),
        glm::clamp(center.z, box.min.z, box.max.z)
    );
    return glm::distance2(center, closest) <= radius * radius;
}

// ============================================================
// Sphere::Intersects(Frustum) — 球体与视锥体相交
// ============================================================
bool Sphere::Intersects(const Frustum& frustum) const {
    return frustum.Intersects(*this);
}

} // namespace he
