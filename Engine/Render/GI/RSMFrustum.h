#pragma once

// ============================================================
// GI/RSMFrustum.h — RSM 光源正交视锥的**纯几何**拟合 + VPL 采样面积（RHI-free）
//
// 为什么单独一个头：这一版修的是 §9.2-AA 里"量级"那一项——RSM 间接光的累加项是
// `L_v · A · cosθ_s · cosθ_r / d²`，其中 **A（每个 VPL 采样点代表的世界面积）**必须由
// 光源正交投影的实际覆盖范围推出。此前这段是硬编码的 `RSM_VPL_ENERGY = 0.046875` 与
// 硬编码的 `sceneCenter=(0,3,0) / sceneRadius=60`：两者一起把结果钉死在"一个约 60 世界单位
// 的场景"上，而 Sponza 是 3720 单位宽 ⇒ `1/d²` 让整项掉到 1e-8（16 位浮点转储的噪声底），
// 于是"pass 在跑、成本在付、画面里没有"。
//
// 抽成纯算术之后，两件关键性质可以直接被单元测试覆盖，而不必跑 GPU：
//   · 光锥**真的罩住**包围盒（逐角点验证投影后的 UV ∈ [0,1]）；
//   · 面积项与 1/d² 的乘积在**均匀缩放场景**下不变（辐射度是尺度不变量）。
//
// 约束：只依赖 Core/Types.h 与 Math/（Math.h + Geometry.h），不得引入任何 RHI 类型。
// ============================================================

#include "Core/Types.h"
#include "Math/Math.h"
#include "Math/Geometry.h"   // he::AABB::Corners()

#include <algorithm>
#include <cmath>
#include <optional>

namespace he::render {

/// RSM 光源正交视锥的拟合结果（纯数据）
struct RSMFrustumFit {
    float4x4 viewProj    = float4x4(1.0f);
    float3   center      = float3(0.0f);   // 包围盒中心（光源视图空间的原点）
    /// 正交半宽 = 半高（**恒取方形**：RSM 的 512×512 贴图必须对应方形世界区域，
    /// 否则 texel 是长方形、Poisson 盘在世界上变成椭圆）。
    float    halfExtent  = 1.0f;
    /// 视图空间近/远平面（供文档与调试；着色器只用 viewProj）
    float    nearPlane   = 0.1f;
    float    farPlane    = 1.0f;
};

/// 用场景包围盒拟合**固定**（不随相机）的光源正交视锥
///
/// 规则（每条都有理由）：
///   · **固定**：CSM 的 lightViewProj 拟合相机视锥，视角一变 RSM 内容就变 ⇒ 探针辐射度
///     视角相关（DDGI 的 B 路径）。RSM 必须用覆盖场景的固定视锥。
///   · 方向：`lightDir` 为光的**传播方向**（与 GPULight::directionType.xyz 同向）。
///   · 视点放在包围球外侧 `radius + margin` 处，正交盒在各轴取 ±halfExtent、
///     `halfExtent = max(逐轴紧致半宽, 逐轴紧致半高)` ⇒ **保证罩住，且 texel 为方形**。
///   · 包围盒退化（任一轴 `size` 非正、含 NaN）或方向为零向量 → `std::nullopt`，
///     调用方**保持原参数**（与 `FitProbeGridToBounds` 同一约定）。
inline std::optional<RSMFrustumFit> FitRSMFrustumToBounds(const float3& mn, const float3& mx,
                                                           const float3& lightDir) {
    const float3 size = mx - mn;
    // `!(x >= 0)` 而非 `x < 0`：NaN 走同一条"退化"分支（并允许某轴为 0 的平面场景）
    if (!(size.x >= 0.0f) || !(size.y >= 0.0f) || !(size.z >= 0.0f)) return std::nullopt;
    if (!(glm::dot(lightDir, lightDir) > 0.0f)) return std::nullopt;

    const float3 center = (mn + mx) * 0.5f;
    const float  radius = glm::length(size) * 0.5f;        // 包围球半径
    if (!(radius > 0.0f)) return std::nullopt;

    const float3 dir   = glm::normalize(lightDir);
    const float  margin = std::max(radius * 0.05f, 0.1f);
    const float3 eye   = center - dir * (radius + margin);
    const float3 up    = (std::abs(dir.y) > 0.99f) ? float3(0.0f, 0.0f, 1.0f)
                                                   : float3(0.0f, 1.0f, 0.0f);
    const float4x4 view = glm::lookAt(eye, center, up);

    // 逐角点求视图空间紧致范围：远平面必须覆盖整个包围盒（RH 视图空间里前方是 −z）
    float maxAbsX = 0.0f, maxAbsY = 0.0f, maxDepth = 0.0f;
    for (const float3& c : he::AABB(mn, mx).Corners()) {
        const float3 v = float3(view * float4(c, 1.0f));
        maxAbsX  = std::max(maxAbsX, std::abs(v.x));
        maxAbsY  = std::max(maxAbsY, std::abs(v.y));
        maxDepth = std::max(maxDepth, -v.z);
    }
    RSMFrustumFit fit;
    fit.center     = center;
    fit.halfExtent = std::max(std::max(maxAbsX, maxAbsY), 1e-3f);
    fit.nearPlane  = 0.1f;
    fit.farPlane   = maxDepth + margin;
    const float4x4 proj = glm::orthoRH_ZO(-fit.halfExtent, fit.halfExtent,
                                          -fit.halfExtent, fit.halfExtent,
                                          fit.nearPlane, fit.farPlane);
    fit.viewProj = proj * view;
    return fit;
}

/// RSM 间接光的采样图案参数（**必须与着色器里的同名常量一致**）
/// 圆盘图案：`GI/RSM_Indirect.frag.slang` 的 `RSM_VPL_COUNT` / `RSM_VPL_RADIUS_UV`
inline constexpr u32   kRSMIndirectVplCount = 16;
inline constexpr float kRSMIndirectRadiusUV = 0.0125f;
/// 方形图案：`Lighting/PBR.frag.slang` 内联 5×5 网格的步长与跨度（±2 步）
inline constexpr float kRSMInlineStepUV   = 0.005f;
inline constexpr u32   kRSMInlineSampleCount = 25;

/// 从正交光源 VP 反推两个方向的正交半宽（世界单位）
///
/// `glm` 是列主序：数学意义上的"第 i 行"= `(M[0][i], M[1][i], M[2][i])`；正交投影 + 无斜切
/// ⇒ `view` 的行是正交基、`proj` 是对角阵 ⇒ 该行的长度 = `1/halfExtent_i`。
/// 返回 false 表示矩阵退化（长度为 0）。
/// 【谁用】Forward 的内联 RSM 路径用它把自己的 CSM 光锥尺度换成采样面积（任务 30）。
/// 着色器不自己反推矩阵（避免矩阵行/列约定的坑）：C++ 算好经 UBO 传入。
inline bool RSMHalfExtentsOfProjection(const float4x4& vp, float& halfX, float& halfY) {
    const float3 row0(vp[0][0], vp[1][0], vp[2][0]);
    const float3 row1(vp[0][1], vp[1][1], vp[2][1]);
    const float  len0 = glm::length(row0);
    const float  len1 = glm::length(row1);
    if (!(len0 > 1e-12f) || !(len1 > 1e-12f)) return false;
    halfX = 1.0f / len0;
    halfY = 1.0f / len1;
    return true;
}

/// 每个采样点代表的世界面积 → 采样缩放（`E/π = scale · Σ L_v·cos·cos/d²`）
inline float RSMVplScaleFromArea(float gatherAreaWorld, u32 sampleCount) {
    if (!(gatherAreaWorld > 0.0f) || sampleCount == 0u) return 0.0f;
    return gatherAreaWorld / (HE_PI * float(sampleCount));
}

/// 圆盘采样图案覆盖的世界面积（半径按 UV 给 ⇒ 两个方向各自换算成世界长度）
inline float RSMDiskArea(float halfX, float halfY, float radiusUV) {
    if (!(halfX > 0.0f) || !(halfY > 0.0f) || !(radiusUV > 0.0f)) return 0.0f;
    return HE_PI * (radiusUV * 2.0f * halfX) * (radiusUV * 2.0f * halfY);
}

/// 方形采样图案覆盖的世界面积（`halfSideUV` = 从中心到边缘的 UV 跨度）
inline float RSMSquareArea(float halfX, float halfY, float halfSideUV) {
    if (!(halfX > 0.0f) || !(halfY > 0.0f) || !(halfSideUV > 0.0f)) return 0.0f;
    return (2.0f * halfSideUV * 2.0f * halfX) * (2.0f * halfSideUV * 2.0f * halfY);
}

/// 每个 VPL 采样点代表的**世界面积**（任务 30 的量级归一）
///
/// 约定：N 个采样点均分它们覆盖的那片区域，故单点面积 = 区域面积 / N。
/// `halfExtent` 为正交半宽（世界单位）⇒ 1 单位 RSM UV 对应 `2·halfExtent` 世界长度
/// （见 `FitRSMFrustumToBounds`：UV [0,1] 覆盖 ±halfExtent）。
/// 【着色器怎么拿到这个尺度】正交投影 + 无斜切 ⇒ `|viewProj 第 0 行| = 1/halfExtent`
/// （view 的行是正交基、proj 是对角阵），故 C++ 侧可直接反推（`RSMHalfExtentsOfProjection`）；
/// 单测锁住这条关系。着色器只接收算好的 `scale`，不自己碰矩阵。
/// Poisson 盘的 UV 半径为 `radiusUV` ⇒ 世界上是个半径 `radiusUV·2·halfExtent` 的圆。
inline float RSMVplSampleArea(float halfExtent, float radiusUV, u32 sampleCount) {
    if (!(halfExtent > 0.0f) || !(radiusUV > 0.0f) || sampleCount == 0u) return 0.0f;
    const float worldRadius = radiusUV * 2.0f * halfExtent;
    return HE_PI * worldRadius * worldRadius / float(sampleCount);
}

/// RSM 间接光的采样缩放：`E/π = scale · Σ (L_v · cosθ_s · cosθ_r / d²)`
///
/// 推导（量纲必须闭合，否则"归一化加权合成"的前提不成立）：
///   辐照度 `E(x) = Σ L_v · A · cosθ_s · cosθ_r / d²`（Lambertian VPL 的辐射强度 = L·A）
///   调用方要的是 `E/π`（与 IBL/DDGI 同约定，见 DeferredLighting 的 `SampleDiffuseSource`），
///   故 `scale = A / π`。代入 `A = π·worldRadius²/N` ⇒ **π 约掉**：`scale = worldRadius² / N`。
/// 【这一版修的就是这里】原先这个 scale 是硬编码常数，隐含"场景约 60 单位"；
/// 现在它由光锥实际覆盖范围推出 ⇒ 场景整体缩放 k 倍时 `scale ∝ k²` 而 `1/d² ∝ 1/k²`，
/// 估计量**保持不变**（辐射度是尺度不变量）。单测锁住这条性质。
inline float RSMVplScale(float halfExtent, float radiusUV, u32 sampleCount) {
    if (!(halfExtent > 0.0f) || !(radiusUV > 0.0f) || sampleCount == 0u) return 0.0f;
    return RSMVplScaleFromArea(RSMDiskArea(halfExtent, halfExtent, radiusUV), sampleCount);
}

} // namespace he::render
