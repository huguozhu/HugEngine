#pragma once

// ============================================================
// Math — GLM wrappers with HugEngine conventions
// ============================================================

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
#define GLM_ENABLE_EXPERIMENTAL

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace he {

// --- Vector types ---
using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;

using int2   = glm::ivec2;
using int3   = glm::ivec3;
using int4   = glm::ivec4;

using uint2  = glm::uvec2;
using uint3  = glm::uvec3;
using uint4  = glm::uvec4;

using double2 = glm::dvec2;
using double3 = glm::dvec3;
using double4 = glm::dvec4;

// --- 矩阵类型 ---
using float2x2 = glm::mat2;
using float3x3 = glm::mat3;
using float4x4 = glm::mat4;
using float3x4 = glm::mat3x4;  // 3×4 行主序仿射变换矩阵（用于 Ray Tracing TLAS Instance）

// --- Quaternion ---
using quat = glm::quat;

// --- Constants ---
constexpr float HE_PI       = glm::pi<float>();
constexpr float HE_2PI      = glm::two_pi<float>();
constexpr float HE_HALF_PI  = glm::half_pi<float>();
constexpr float HE_EPSILON  = glm::epsilon<float>();

} // namespace he
