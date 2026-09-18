// ============================================================
// CollisionDebugSystem.cpp — 碰撞体调试线框（任务 26）
//
// 形状语义全部来自 CollisionSystem::ExtractWorldShape（与检测同一份），
// 本文件只负责"把世界形状三角化成线框几何"。
// ============================================================

#include "Scene/CollisionDebugSystem.h"

#include "Scene/CollisionDebugComponent.h"
#include "Scene/CollisionComponent.h"
#include "Scene/MeshComponent.h"
#include "Scene/Transform.h"
#include "Scene/World.h"

#include <algorithm>
#include <cmath>

namespace he {

namespace {

// 单段线 → 十字交叉的两片细带（8 顶点 / 12 索引）
//
// 为什么是两片而不是一片 billboard 四边形：billboard 需要相机参数（系统调用方就得每帧传相机），
// 两片互相垂直的细带在任意视角下至少有一片接近正对相机，视觉效果等价而实现与调用都更简单。
void AppendSegment(const float3& a, const float3& b, float thickness,
                   TArray<StaticVertex>& verts, TArray<u32>& indices) {
    const float3 dir = b - a;
    const float len = glm::length(dir);
    if (len < 1e-6f) return;                       // 退化线段：跳过（不产生 NaN）
    const float3 d = dir / len;

    // 选一个与 d 不平行的参考轴，再构造两组正交侧向量（两片细带的朝向）
    const float3 ref = (std::abs(d.y) < 0.9f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 s1 = glm::cross(d, ref);
    const float s1len = glm::length(s1);
    if (s1len < 1e-6f) return;
    s1 = (s1 / s1len) * (thickness * 0.5f);
    float3 s2 = glm::cross(d, s1);
    const float s2len = glm::length(s2);
    if (s2len < 1e-6f) return;
    s2 = (s2 / s2len) * (thickness * 0.5f);

    const float3 sides[2] = { s1, s2 };
    for (int f = 0; f < 2; ++f) {
        const float3 s = sides[f];
        const u32 base = (u32)verts.size();
        const float3 n = glm::normalize(glm::cross(d, s));
        verts.push_back({ a - s, n, float2(0.0f) });
        verts.push_back({ a + s, n, float2(1.0f, 0.0f) });
        verts.push_back({ b + s, n, float2(1.0f, 1.0f) });
        verts.push_back({ b - s, n, float2(0.0f, 1.0f) });
        indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
        indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
    }
}

// 圆环（在由 u/v 张成的平面内，圆心 c、半径 r、segments 段）
void AppendCircle(const float3& c, const float3& u, const float3& v, float r, u32 segments,
                  float thickness, TArray<StaticVertex>& verts, TArray<u32>& indices) {
    if (r <= 1e-6f || segments < 3) return;
    const float step = 6.28318530718f / (float)segments;
    for (u32 i = 0; i < segments; ++i) {
        const float a0 = step * (float)i;
        const float a1 = step * (float)(i + 1);
        const float3 p0 = c + (u * std::cos(a0) + v * std::sin(a0)) * r;
        const float3 p1 = c + (u * std::cos(a1) + v * std::sin(a1)) * r;
        AppendSegment(p0, p1, thickness, verts, indices);
    }
}

// 半球弧（从 u 方向经 +w 转到 -u 方向；12 点方向的半圆）
void AppendArc(const float3& c, const float3& u, const float3& w, float r, u32 segments,
               float thickness, TArray<StaticVertex>& verts, TArray<u32>& indices) {
    if (r <= 1e-6f || segments < 2) return;
    const float step = 3.14159265359f / (float)segments;
    for (u32 i = 0; i < segments; ++i) {
        const float a0 = step * (float)i;
        const float a1 = step * (float)(i + 1);
        const float3 p0 = c + (u * std::cos(a0) + w * std::sin(a0)) * r;
        const float3 p1 = c + (u * std::cos(a1) + w * std::sin(a1)) * r;
        AppendSegment(p0, p1, thickness, verts, indices);
    }
}

} // namespace

u32 CollisionDebugSystem::BuildWireframe(const CollisionWorldShape& shape, float thickness,
                                         TArray<StaticVertex>& outVertices, TArray<u32>& outIndices) {
    outVertices.clear();
    outIndices.clear();
    const float t = std::max(thickness, 1e-4f);   // 线宽下限（避免退化/不可见）
    const u32 before = 0;
    (void)before;

    switch (shape.shape) {
    case CollisionShape::AABB: {
        // 12 条棱：世界 AABB（旋转后重轴，与检测一致）
        const float3& mn = shape.min;
        const float3& mx = shape.max;
        const float3 c[8] = {
            { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z },
            { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z }, { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z },
        };
        const u32 edges[12][2] = {
            {0,1},{1,2},{2,3},{3,0},   // 底面
            {4,5},{5,6},{6,7},{7,4},   // 顶面
            {0,4},{1,5},{2,6},{3,7},   // 竖棱
        };
        for (auto& e : edges) AppendSegment(c[e[0]], c[e[1]], t, outVertices, outIndices);
        return 12;
    }
    case CollisionShape::Sphere: {
        // 3 个正交大圆
        const float3& c = shape.center;
        const float r = std::max(shape.radius, 0.0f);
        AppendCircle(c, float3(1,0,0), float3(0,1,0), r, kCircleSegments, t, outVertices, outIndices); // XY
        AppendCircle(c, float3(0,1,0), float3(0,0,1), r, kCircleSegments, t, outVertices, outIndices); // YZ
        AppendCircle(c, float3(1,0,0), float3(0,0,1), r, kCircleSegments, t, outVertices, outIndices); // XZ
        return kCircleSegments * 3;
    }
    case CollisionShape::Capsule: {
        const float3& a = shape.segA;   // 上端
        const float3& b = shape.segB;   // 下端
        const float r = std::max(shape.radius, 0.0f);
        float3 axis = a - b;
        const float axisLen = glm::length(axis);
        float3 up = (axisLen > 1e-6f) ? axis / axisLen : float3(0.0f, 1.0f, 0.0f);
        // 与 up 正交的两个方向
        const float3 ref = (std::abs(up.y) < 0.9f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        float3 u = glm::cross(up, ref);
        const float ul = glm::length(u);
        if (ul < 1e-6f) return 0;
        u = u / ul;
        const float3 v = glm::cross(up, u);

        // 上下两个圆 + 两条竖线（u、v 方向）
        AppendCircle(a, u, v, r, kCircleSegments, t, outVertices, outIndices);
        AppendCircle(b, u, v, r, kCircleSegments, t, outVertices, outIndices);
        AppendSegment(a + u * r, b + u * r, t, outVertices, outIndices);
        AppendSegment(a - u * r, b - u * r, t, outVertices, outIndices);
        AppendSegment(a + v * r, b + v * r, t, outVertices, outIndices);
        AppendSegment(a - v * r, b - v * r, t, outVertices, outIndices);
        // 两端半球弧（各两个正交方向，共 4 条半圆）
        AppendArc(a, u,  up, r, kCapArcSegments, t, outVertices, outIndices);
        AppendArc(a, -u, up, r, kCapArcSegments, t, outVertices, outIndices);
        AppendArc(a, v,  up, r, kCapArcSegments, t, outVertices, outIndices);
        AppendArc(a, -v, up, r, kCapArcSegments, t, outVertices, outIndices);
        AppendArc(b, u, -up, r, kCapArcSegments, t, outVertices, outIndices);
        AppendArc(b, -u, -up, r, kCapArcSegments, t, outVertices, outIndices);
        AppendArc(b, v, -up, r, kCapArcSegments, t, outVertices, outIndices);
        AppendArc(b, -v, -up, r, kCapArcSegments, t, outVertices, outIndices);
        return kCircleSegments * 2 + 4 + kCapArcSegments * 8;
    }
    }
    return 0;
}

CollisionDebugComponent* CollisionDebugSystem::Ensure(World& world, Entity e) {
    if (!world.GetComponent<CollisionComponent>(e)) return nullptr;   // 没有碰撞体就不需要线框
    auto* dbg = world.GetComponent<CollisionDebugComponent>(e);
    if (!dbg) {
        dbg = world.AddComponent<CollisionDebugComponent>(e);
        if (dbg) {
            // 调试几何：无光照 + 半透明 + 双面 + 不投影（不影响光照与阴影）
            dbg->unlit       = true;
            dbg->alphaMode   = 2;      // AlphaMode::Blend
            dbg->doubleSided = true;
            dbg->castShadow  = false;
            dbg->metallicFactor  = 0.0f;
            dbg->roughnessFactor = 1.0f;
        }
    }
    return dbg;
}

u32 CollisionDebugSystem::Update(World& world, bool enabled) {
    u32 touched = 0;

    world.ForEach<CollisionComponent>([&](Entity e, CollisionComponent&) {
        auto* dbg = world.GetComponent<CollisionDebugComponent>(e);

        // 关闭：只清空几何（组件留下，便于再打开；没有组件就什么都不做）
        if (!enabled) {
            if (!dbg) return;
            if (dbg->segmentCount != 0 || dbg->GetVertexCount() != 0) {
                dbg->SetMeshData({}, {});
                dbg->segmentCount = 0;
                dbg->bHasCache = false;
                ++touched;
            }
            return;
        }

        // 打开：没有线框组件就补一个（调试可视化的默认项）
        if (!dbg) dbg = Ensure(world, e);
        if (!dbg) return;

        CollisionWorldShape shape;
        if (!CollisionSystem::ExtractWorldShape(world, e, shape)) {
            if (dbg->segmentCount != 0) {          // 碰撞体被禁用/移除 → 线框也要消失
                dbg->SetMeshData({}, {});
                dbg->segmentCount = 0;
                dbg->bHasCache = false;
                ++touched;
            }
            return;
        }

        // 形状没变（含线宽）就不重建网格：避免每帧重建 GPU 缓冲
        const bool sameShape = dbg->bHasCache &&
            glm::all(glm::epsilonEqual(shape.min,    dbg->cachedShape.min,    1e-4f)) &&
            glm::all(glm::epsilonEqual(shape.max,    dbg->cachedShape.max,    1e-4f)) &&
            glm::all(glm::epsilonEqual(shape.center, dbg->cachedShape.center, 1e-4f)) &&
            glm::all(glm::epsilonEqual(shape.segA,   dbg->cachedShape.segA,   1e-4f)) &&
            glm::all(glm::epsilonEqual(shape.segB,   dbg->cachedShape.segB,   1e-4f)) &&
            std::abs(shape.radius - dbg->cachedShape.radius) < 1e-4f &&
            shape.shape == dbg->cachedShape.shape &&
            std::abs(dbg->lineThickness - dbg->cachedThickness) < 1e-6f;
        if (sameShape) return;

        TArray<StaticVertex> verts;
        TArray<u32> indices;
        const u32 segments = BuildWireframe(shape, dbg->lineThickness, verts, indices);
        dbg->SetMeshData(verts, indices);
        dbg->segmentCount = segments;

        dbg->cachedShape     = shape;
        dbg->cachedThickness = dbg->lineThickness;
        dbg->bHasCache       = true;
        ++touched;
    });

    return touched;
}

} // namespace he
