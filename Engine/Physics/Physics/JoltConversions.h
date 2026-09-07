#pragma once

// ============================================================
// Physics/JoltConversions.h — glm（float3/quat）↔ Jolt（RVec3/Quat）转换
//
// 集中收口所有坐标/单位换算。Jolt 同为 Y-up 米制，与 glm（RH、Z 深度）一致：
//   - 位置：glm::float3（米）      ↔ JPH::RVec3（米）
//   - 旋转：glm::quat               ↔ JPH::Quat
//   - 向量：glm::float3             ↔ JPH::Vec3
// ============================================================

#include "Math/Math.h"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Mat44.h>

namespace he::physics {

inline JPH::Vec3 ToJolt(const he::float3& v) { return JPH::Vec3(v.x, v.y, v.z); }
inline he::float3 ToGlm(const JPH::Vec3& v) { return he::float3(v.GetX(), v.GetY(), v.GetZ()); }
inline JPH::RVec3 ToJoltPos(const he::float3& v) { return JPH::RVec3(v.x, v.y, v.z); }
inline he::float3 ToGlmPos(const JPH::RVec3& v) { return he::float3((float)v.GetX(), (float)v.GetY(), (float)v.GetZ()); }
inline JPH::Quat ToJolt(const he::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
inline he::quat ToGlm(const JPH::Quat& q) {
    he::quat r;
    r.x = q.GetX(); r.y = q.GetY(); r.z = q.GetZ(); r.w = q.GetW();   // glm::quat 构造序易混，直接赋分量
    return r;
}

} // namespace he::physics
