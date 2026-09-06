// ============================================================
// CollisionSystem.cpp — CPU 碰撞检测实现
//
// 形状世界空间提取 + 三形状全组合 Overlap + Contains + Raycast。
// 全部纯数学实现，无 RHI 依赖，可单元测试。
// ============================================================

#include "Scene/CollisionSystem.h"

#include "Scene/CollisionComponent.h"
#include "Scene/Transform.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Math/Geometry.h"

#include <algorithm>
#include <cmath>

namespace he {

namespace {

// 提取后的世界空间形状（供各检测函数统一使用）
struct WorldShape {
    CollisionShape shape = CollisionShape::AABB;
    float3 min, max;        // AABB 世界轴对齐范围（旋转后重轴，保守）
    float3 center;          // Sphere 球心 / Capsule 段中点
    float  radius = 0.0f;   // Sphere / Capsule 半径
    float3 segA, segB;      // Capsule 段两端点
};

// 从实体提取世界形状（缺失组件/无 Transform/禁用时返回 false，容错）
bool ExtractShape(World& world, Entity e, WorldShape& out) {
    auto* cc = world.GetComponent<CollisionComponent>(e);
    auto* xf = world.GetComponent<TransformComponent>(e);
    if (!cc || !xf || !cc->bEnabled) return false;

    // 世界矩阵：优先场景图（支持父层级），无场景图回退局部矩阵
    float4x4 wm = xf->GetLocalMatrix();
    if (auto* sg = world.GetSceneGraph()) {
        wm = sg->GetWorldMatrix(e);
    }
    // 半径/半尺寸统一乘最大缩放分量（MVP 约定）
    float scale = std::max({xf->scale.x, xf->scale.y, xf->scale.z});
    if (scale <= 0.0f) scale = 1.0f;

    out.shape = cc->shape;
    out.center = float3(wm[3]);
    out.radius = cc->radius * scale;

    switch (cc->shape) {
    case CollisionShape::AABB: {
        AABB local(-cc->halfExtents, cc->halfExtents);
        AABB worldBox = local.Transform(wm);
        out.min = worldBox.min;
        out.max = worldBox.max;
        break;
    }
    case CollisionShape::Sphere:
        // 球体：仅球心与半径（与旋转无关）
        break;
    case CollisionShape::Capsule: {
        // 垂直胶囊：段沿变换上方向，段半长 = max(0, height/2 - radius)
        float halfSeg = std::max(0.0f, cc->height * 0.5f - cc->radius) * scale;
        float3 up = glm::normalize(xf->rotation * float3(0.0f, 1.0f, 0.0f));
        out.segA = out.center + up * halfSeg;
        out.segB = out.center - up * halfSeg;
        break;
    }
    }
    return true;
}

// --- 基础几何工具 ---

// 点 p 到线段 ab 的最近点
float3 ClosestPointOnSegment(const float3& p, const float3& a, const float3& b) {
    float3 ab = b - a;
    float lenSq = glm::dot(ab, ab);
    float t = lenSq > 1e-12f ? std::clamp(glm::dot(p - a, ab) / lenSq, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

// 点 p 到 AABB [min,max] 的最近点
float3 ClosestPointOnAABB(const float3& p, const float3& boxMin, const float3& boxMax) {
    return glm::clamp(p, boxMin, boxMax);
}

// 两条线段之间的最近距离（经典最近点算法）
float SegmentSegmentDistance(const float3& a0, const float3& a1,
                             const float3& b0, const float3& b1) {
    float3 d1 = a1 - a0, d2 = b1 - b0, r = a0 - b0;
    float a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    float c = glm::dot(d1, r), b = glm::dot(d1, d2);
    float denom = a * e - b * b;
    float s = 0.0f, t = 0.0f;
    if (denom > 1e-12f) {
        s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
        t = (b * s + f) / e;
        if (t < 0.0f) { t = 0.0f; s = std::clamp(-c / a, 0.0f, 1.0f); }
        else if (t > 1.0f) { t = 1.0f; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
    } else {
        // 平行线段：任取一端投影
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
    }
    float3 c1 = a0 + d1 * s, c2 = b0 + d2 * t;
    return glm::length(c1 - c2);
}

// --- 形状对重叠（全组合）---

bool OverlapShapes(const WorldShape& A, const WorldShape& B) {
    if (A.shape == CollisionShape::AABB && B.shape == CollisionShape::AABB) {
        return A.min.x <= B.max.x && A.max.x >= B.min.x &&
               A.min.y <= B.max.y && A.max.y >= B.min.y &&
               A.min.z <= B.max.z && A.max.z >= B.min.z;
    }
    if (A.shape == CollisionShape::AABB && B.shape == CollisionShape::Sphere) {
        float3 p = ClosestPointOnAABB(B.center, A.min, A.max);
        return glm::length(p - B.center) <= B.radius;
    }
    if (A.shape == CollisionShape::Sphere && B.shape == CollisionShape::AABB) {
        return OverlapShapes(B, A);   // 对称交换
    }
    if (A.shape == CollisionShape::AABB && B.shape == CollisionShape::Capsule) {
        // 盒与胶囊：段上最近点钳入盒内，再与段比较（单次迭代近似，MVP 足够）
        float3 p = ClosestPointOnAABB(ClosestPointOnSegment(A.center, B.segA, B.segB), A.min, A.max);
        float3 segClosest = ClosestPointOnSegment(p, B.segA, B.segB);
        return glm::length(p - segClosest) <= B.radius;
    }
    if (A.shape == CollisionShape::Capsule && B.shape == CollisionShape::AABB) {
        return OverlapShapes(B, A);
    }
    if (A.shape == CollisionShape::Sphere && B.shape == CollisionShape::Sphere) {
        return glm::length(A.center - B.center) <= A.radius + B.radius;
    }
    if (A.shape == CollisionShape::Sphere && B.shape == CollisionShape::Capsule) {
        float3 p = ClosestPointOnSegment(A.center, B.segA, B.segB);
        return glm::length(A.center - p) <= A.radius + B.radius;
    }
    if (A.shape == CollisionShape::Capsule && B.shape == CollisionShape::Sphere) {
        return OverlapShapes(B, A);
    }
    if (A.shape == CollisionShape::Capsule && B.shape == CollisionShape::Capsule) {
        return SegmentSegmentDistance(A.segA, A.segB, B.segA, B.segB) <= A.radius + B.radius;
    }
    return false;
}

// --- 点包含 ---
bool ContainsShape(const WorldShape& s, const float3& p) {
    switch (s.shape) {
    case CollisionShape::AABB:
        return p.x >= s.min.x && p.x <= s.max.x &&
               p.y >= s.min.y && p.y <= s.max.y &&
               p.z >= s.min.z && p.z <= s.max.z;
    case CollisionShape::Sphere:
        return glm::length(p - s.center) <= s.radius;
    case CollisionShape::Capsule:
        return glm::length(p - ClosestPointOnSegment(p, s.segA, s.segB)) <= s.radius;
    }
    return false;
}

// --- 射线检测（返回命中距离；未命中返回 < 0）---
float RaycastShape(const WorldShape& s, const float3& origin, const float3& dir, float maxDist) {
    switch (s.shape) {
    case CollisionShape::AABB: {
        // slab 法：逐轴求进出区间，求交集
        float t0 = 0.0f, t1 = maxDist;
        const float3 boxMin = s.min, boxMax = s.max;
        for (int i = 0; i < 3; ++i) {
            if (std::abs(dir[i]) < 1e-8f) {
                if (origin[i] < boxMin[i] || origin[i] > boxMax[i]) return -1.0f;
            } else {
                float ta = (boxMin[i] - origin[i]) / dir[i];
                float tb = (boxMax[i] - origin[i]) / dir[i];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
                if (t0 > t1) return -1.0f;
            }
        }
        return t0;
    }
    case CollisionShape::Sphere: {
        float3 oc = origin - s.center;
        float b = glm::dot(oc, dir);
        float c = glm::dot(oc, oc) - s.radius * s.radius;
        float disc = b * b - c;
        if (disc < 0.0f) return -1.0f;
        float t = -b - std::sqrt(disc);
        if (t < 0.0f) t = -b + std::sqrt(disc);   // 起点在球内时取远交点
        return (t >= 0.0f && t <= maxDist) ? t : -1.0f;
    }
    case CollisionShape::Capsule: {
        // MVP：射线与胶囊 = 射线与段的最近距离 <= 半径（端点半球近似，掠射角有误差）
        float3 d = dir;   // 入口已归一化
        // 射线上离段最近的点：参数化 ray(t) = origin + d*t，最小化 dist(ray(t), seg)
        float3 w0 = origin - s.segA;
        float3 v  = s.segB - s.segA;
        float a = glm::dot(d, d), b = glm::dot(d, v), c = glm::dot(v, v);
        float e = glm::dot(d, w0), f = glm::dot(v, w0);
        float denom = a * c - b * b;
        float t = 0.0f;
        if (denom > 1e-12f) {
            t = std::clamp((b * f - c * e) / denom, 0.0f, maxDist);
        }
        float3 closest = origin + d * t;
        float dist = glm::length(closest - ClosestPointOnSegment(closest, s.segA, s.segB));
        return dist <= s.radius ? t : -1.0f;
    }
    }
    return -1.0f;
}

} // namespace

bool CollisionSystem::Overlap(World& world, Entity a, Entity b) {
    WorldShape sa, sb;
    if (!ExtractShape(world, a, sa) || !ExtractShape(world, b, sb)) return false;
    return OverlapShapes(sa, sb);
}

bool CollisionSystem::Contains(World& world, Entity e, const float3& point) {
    WorldShape s;
    if (!ExtractShape(world, e, s)) return false;
    return ContainsShape(s, point);
}

bool CollisionSystem::Raycast(World& world, const float3& origin, const float3& dir,
                              float maxDistance, Entity& outHit, float& outT) {
    // 归一化方向（球体/胶囊公式基于单位向量；零向量直接返回未命中）
    float lenSq = glm::dot(dir, dir);
    if (lenSq < 1e-12f) return false;
    float3 d = dir / std::sqrt(lenSq);

    float bestT = maxDistance + 1.0f;
    Entity best = Entity{kInvalidEntity};

    world.ForEach<CollisionComponent>([&](Entity e, CollisionComponent& cc) {
        WorldShape s;
        if (!ExtractShape(world, e, s)) return;   // 禁用/缺失 Transform 跳过
        float t = RaycastShape(s, origin, d, maxDistance);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best = e;
        }
    });

    if (!best.IsValid()) return false;
    outHit = best;
    outT = bestT;
    return true;
}

} // namespace he
