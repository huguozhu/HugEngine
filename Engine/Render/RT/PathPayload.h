#pragma once

#include "Core/Types.h"   // u32（Tests 目标不链接 RHI，故这里显式依赖 Core）
#include "Math/Math.h"    // float4

#include <cstddef>
#include <type_traits>

namespace he::render {

// ============================================================
// PathPayload — 路径追踪载荷的 C++ 镜像
// （着色器侧定义：Engine/Shader/Shaders/PT_Common.slang 的 struct PathPayload）
//
// 为什么需要这份镜像：Slang 与 C++ 各自按自己的规则布局结构体，本仓库已经
// 踩过「数组元素步长 16B vs 12B 导致其后所有字段偏移全错」的坑（见
// Material.h 里 GIBlendParams 的注释）。载荷虽然只在 GPU 侧读写，但它的
// **字节数**要传给 CreateEffectPipeline（maxPayloadSize），两端一旦不一致
// 就会出现「rchit 写的字段被截断、rgen 读到垃圾」这种极难定位的问题。
//
// 这里把大小与逐字段偏移全部钉死，与 Slang 侧的 static_assert 对称；
// 改动任一字段都必须同时改另一侧，否则编译期就会拦下。
//
// 112B = 7 × float4（float4 在 Slang/C++ 均为 16B 对齐，无隐式填充）
// ============================================================
struct PathPayload {
    float4 albedoMetallic = float4(0.0f);                     // 0  rgb=albedo, a=metallic
    float4 normalRough    = float4(0.0f, 0.0f, 1.0f, 1.0f);   // 16 xyz=normal(世界空间), w=roughness
    float4 emissiveT      = float4(0.0f, 0.0f, 0.0f, -1.0f);  // 32 rgb=emissive, a=hitT（-1=miss）
    // ── Disney 参数（与 Material.h 的 disneyA/disneyB/disneyC、RTPass 材质纹理 row4~row6 同源）──
    float4 disneyA        = float4(0.0f, 0.0f, 0.5f, 0.0f);   // 48 x=anisotropic, y=subsurface, z=specular, w=sheen
    float4 disneyB        = float4(0.0f, 1.0f, 1.0f, 1.0f);   // 64 x=clearcoat, y=clearcoatGloss, z=specularTint.r, w=specularTint.g
    float4 surfaceParams  = float4(1.0f, 0.04f, 1.5f, 0.0f);  // 80 x=disneyC, y=dielectricF0, z=ior, w=transmission
    float4 volumeParams   = float4(0.0f);                     // 96 x/y/z=σ_t（Beer-Lambert 吸收系数）, w=预留
};

static_assert(sizeof(PathPayload) == 112,
              "PathPayload must be 112 bytes（与 PT_Common.slang 的 struct 一致）");
static_assert(offsetof(PathPayload, albedoMetallic) == 0,  "albedoMetallic 偏移必须为 0");
static_assert(offsetof(PathPayload, normalRough)    == 16, "normalRough 偏移必须为 16");
static_assert(offsetof(PathPayload, emissiveT)      == 32, "emissiveT 偏移必须为 32");
static_assert(offsetof(PathPayload, disneyA)        == 48, "disneyA 偏移必须为 48");
static_assert(offsetof(PathPayload, disneyB)        == 64, "disneyB 偏移必须为 64");
static_assert(offsetof(PathPayload, surfaceParams)  == 80, "surfaceParams 偏移必须为 80");
static_assert(offsetof(PathPayload, volumeParams)   == 96, "volumeParams 偏移必须为 96");
static_assert(std::is_trivially_copyable_v<PathPayload>,
              "PathPayload 必须是可平凡复制的 POD（作为 RT 载荷按字节传递）");
static_assert(std::is_standard_layout_v<PathPayload>,
              "PathPayload 必须是标准布局（offsetof 断言的前提）");

/// RT 载荷大小（PTPass 创建 RT 管线时作为 maxPayloadSize 传入）
inline constexpr u32 kPathPayloadSize = sizeof(PathPayload);

} // namespace he::render
