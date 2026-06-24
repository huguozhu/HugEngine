#pragma once

#include "Math/Math.h"
#include "Core/Types.h"

#include <array>

// ============================================================
// Geometry primitives — AABB, Frustum, Ray, Sphere
// ============================================================

namespace he {

// --- Axis-Aligned Bounding Box ---
struct AABB {
    float3 min = float3(FLT_MAX);
    float3 max = float3(-FLT_MAX);

    AABB() = default;
    AABB(const float3& min_, const float3& max_) : min(min_), max(max_) {}

    void Expand(const float3& point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    void Expand(const AABB& other) {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }

    float3 Center()  const { return (min + max) * 0.5f; }
    float3 Extent()  const { return (max - min) * 0.5f; }
    float3 Size()    const { return max - min; }
    float  Volume()  const { auto s = Size(); return s.x * s.y * s.z; }
    bool   IsValid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }

    std::array<float3, 8> Corners() const {
        return {{
            float3(min.x, min.y, min.z), float3(max.x, min.y, min.z),
            float3(min.x, max.y, min.z), float3(max.x, max.y, min.z),
            float3(min.x, min.y, max.z), float3(max.x, min.y, max.z),
            float3(min.x, max.y, max.z), float3(max.x, max.y, max.z),
        }};
    }

    AABB Transform(const float4x4& m) const {
        auto corners = Corners();
        AABB result;
        for (auto& c : corners) {
            float4 t = m * float4(c, 1.0f);
            result.Expand(float3(t) / t.w);
        }
        return result;
    }
};

// --- Frustum (6 planes, view-projection space) ---
struct Frustum {
    // Plane: normal.xyz, distance.w (ax + by + cz + d = 0)
    float4 planes[6]; // Left, Right, Top, Bottom, Near, Far

    // Extract frustum planes from a view-projection matrix
    static Frustum FromViewProj(const float4x4& vp);

    // Culling tests
    bool Intersects(const AABB& box) const;
    bool Contains(const float3& point) const;
};

// --- Ray ---
struct Ray {
    float3 origin;
    float3 direction;

    Ray() = default;
    Ray(const float3& o, const float3& d) : origin(o), direction(glm::normalize(d)) {}

    float3 PointAt(float t) const { return origin + direction * t; }

    // Ray-AABB intersection (slab method)
    bool IntersectsAABB(const AABB& box, float& tMin, float& tMax) const;
};

// --- Sphere ---
struct Sphere {
    float3 center = float3(0.0f);
    float  radius = 0.0f;

    Sphere() = default;
    Sphere(const float3& c, float r) : center(c), radius(r) {}

    bool Contains(const float3& point) const {
        return glm::distance2(center, point) <= radius * radius;
    }

    bool Intersects(const AABB& box) const;
    bool Intersects(const Frustum& frustum) const;
};

} // namespace he
