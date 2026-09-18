#pragma once

#include "Core/Types.h"   // u32
#include "Math/Math.h"    // float4

namespace he::render {

// ============================================================
// PTMaterialParams — Disney 材质参数的三元组打包（光栅化 / 路径追踪共用）
//
// 为什么单独抽出这一份：同一组 Disney 参数在本仓库有三条打包路径
// （光栅化 `Material.h` 的 `FillObjectData` / `FillMaterialData`、RT 场景材质
// 纹理 `RTPass::BuildSceneMaterialTexture` 的 row4~row6、路径追踪载荷
// `PathPayload` 的 disneyA/disneyB/surfaceParams），而 Slang 侧统一按
// `pbr_common.PBR_BRDF(albedo, metallic, roughness, N, V, L,
//                     dielectricF0, envBRDF, disneyA, disneyB, disneyC)`
// 求值。三处若各写一份打包，很容易出现「PT 与光栅化对同一材质算出的
// BRDF 不同」这种隐蔽偏差（PT 是参考渲染器，偏差会直接污染对照结论）。
//
// 字段含义（与 ShaderTypes.slang / pbr_common.slang 的约定一致）：
//   disneyA = (anisotropic, subsurface, specular, sheen)
//   disneyB = (clearcoat, clearcoatGloss, specularTint.r, specularTint.g)
//   surfaceParams = (disneyC = specularTint.b, dielectricF0, ior, transmission)
// 其中 transmission 目前只记录、不参与折射（PT 任务 4 才启用）。
// ============================================================

/// PBR_BRDF 的默认 Disney 参数（与 pbr_common.slang 的默认实参逐字段一致：
/// aniso=0, subsurface=0, specular=0.5, sheen=0, clearcoat=0, clearcoatGloss=1,
/// specularTint=(1,1,1)）。材质没有 Disney 扩展时打包结果必须等于这组默认值，
/// 这样「PT 传显式参数」与「PT 吃默认实参」在数值上完全等价。
inline constexpr float4 kDefaultDisneyA = float4(0.0f, 0.0f, 0.5f, 0.0f);
inline constexpr float4 kDefaultDisneyB = float4(0.0f, 1.0f, 1.0f, 1.0f);
inline constexpr float  kDefaultDisneyC = 1.0f;

/// 默认电介质折射率与由它推导的 F0（0.04）
inline constexpr float kDefaultIOR = 1.5f;

/// 由 IOR 推导电介质 F0（与 pbr_common.slang 的 dielectricF0 语义一致）
inline float DielectricF0FromIOR(float ior) {
    return (ior - 1.0f) * (ior - 1.0f) / ((ior + 1.0f) * (ior + 1.0f));
}

/// Disney 参数打包结果（与 PathPayload.disneyA / disneyB / surfaceParams 一一对应）
struct PTMaterialParams {
    float4 disneyA       = kDefaultDisneyA;
    float4 disneyB       = kDefaultDisneyB;
    float4 surfaceParams = float4(kDefaultDisneyC, DielectricF0FromIOR(kDefaultIOR),
                                  kDefaultIOR, 0.0f);
};

/// 按字段打包（各处调用点传自己的材质字段，打包规则只此一份）
inline PTMaterialParams PackDisneyParams(
        float anisotropic, float subsurface, float specular, float sheen,
        float clearcoat, float clearcoatGloss,
        float specularTintR, float specularTintG, float specularTintB,
        float ior, float transmission) {
    PTMaterialParams p;
    p.disneyA       = float4(anisotropic, subsurface, specular, sheen);
    p.disneyB       = float4(clearcoat, clearcoatGloss, specularTintR, specularTintG);
    p.surfaceParams = float4(specularTintB, DielectricF0FromIOR(ior), ior, transmission);
    return p;
}

} // namespace he::render
