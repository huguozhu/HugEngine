#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

#include <vector>

// ============================================================
// SkeletalMeshSystem — 骨骼动画驱动（Phase C C1b，静态系统）
//
// 每帧：推进剪辑时间 → 采样关节本地 TRS（线性插值/四元数 slerp）
//       → 父子层级合成世界矩阵 → 蒙皮矩阵 = world × inverseBind
//       → 标记组件脏（渲染管线上传骨骼 SSBO，GPU 蒙皮）
// ============================================================

namespace he {
class World;

namespace asset {
struct SkeletonAsset;
struct AnimationBlendLayer;
struct RetargetProfile;
}

class SkeletalMeshSystem {
public:
    /// 采样关节本地 TRS（clipIndex=-1 或无该分量通道时用关节静态 TRS）
    static void SampleJointTRS(const asset::SkeletonAsset& skel, i32 clipIndex,
                               float time, i32 jointIndex,
                               float3& outT, quat& outR, float3& outS);

    /// 采样**多层混合**后的关节本地 TRS（任务 21：剪辑混合 / Blend Space）
    /// · 权重按 Σw 归一化；`clipIndex<0` 或 w≤0 的层不参与；全部不参与 ⇒ 关节静态 TRS
    /// · 平移/缩放线性加权；旋转按"符号对齐到首个参与层 → 加权求和 → 归一化"
    static void SampleJointTRSBlended(const asset::SkeletonAsset& skel,
                                      const asset::AnimationBlendLayer* layers, u32 layerCount,
                                      i32 jointIndex, float3& outT, quat& outR, float3& outS);

    /// 合成全部关节的蒙皮矩阵（world × inverseBind；长度 = joints 数）
    /// @param outWorldMatrices 可选：同时输出关节世界矩阵（调试用）
    static void ComputeSkinMatrices(const asset::SkeletonAsset& skel, i32 clipIndex, float time,
                                    std::vector<float4x4>& outSkinMatrices,
                                    std::vector<float4x4>* outWorldMatrices = nullptr);

    /// 用**多层混合**合成蒙皮矩阵（层级合成与蒙皮公式与上面完全一致，只换采样器）
    static void ComputeSkinMatricesBlended(const asset::SkeletonAsset& skel,
                                           const asset::AnimationBlendLayer* layers, u32 layerCount,
                                           std::vector<float4x4>& outSkinMatrices,
                                           std::vector<float4x4>* outWorldMatrices = nullptr);

    // ── 动画重定向（任务 22：不同骨架共用同一套剪辑）──────────────────────────

    /// 按**关节名字**构建重定向配置：目标关节 name 与源关节 name 相同即映射。
    /// · 名字对不上 / 空名 → 该目标关节保持自己的绑定姿势（targetToSource = -1）
    /// · 名字重名时取源里第一个同名关节（glTF 里同名关节不常见，保持确定性）
    /// · 不开启平移/缩放重定向（默认只借旋转；调用方按需打开开关）
    static asset::RetargetProfile BuildRetargetProfile(const asset::SkeletonAsset& target,
                                                       const asset::SkeletonAsset& source);

    /// 重定向采样：把源骨架 clipIndex/time 的动作搬到目标关节 targetJointIndex 上
    ///   目标旋转 = 目标绑定旋转 × (源绑定旋转⁻¹ × 源动画旋转)
    ///   目标平移 = 目标绑定平移 + (源动画平移 − 源绑定平移) × k   （k 见 RetargetProfile）
    /// 未映射的目标关节 / 越界下标 ⇒ 目标关节的静态 TRS（即绑定姿势，不崩溃）
    static void SampleJointTRSRetargeted(const asset::SkeletonAsset& target,
                                         const asset::SkeletonAsset& source,
                                         const asset::RetargetProfile& profile,
                                         i32 clipIndex, float time, i32 targetJointIndex,
                                         float3& outT, quat& outR, float3& outS);

    /// 用**重定向采样器**合成目标骨架的蒙皮矩阵（world × inverseBind 用目标骨架的）
    static void ComputeSkinMatricesRetargeted(const asset::SkeletonAsset& target,
                                              const asset::SkeletonAsset& source,
                                              const asset::RetargetProfile& profile,
                                              i32 clipIndex, float time,
                                              std::vector<float4x4>& outSkinMatrices,
                                              std::vector<float4x4>* outWorldMatrices = nullptr);

    /// 重定向 + 多层混合：每层先做重定向采样，再按任务 21 的规则混合（权重/归一化不变）
    static void SampleJointTRSBlendedRetargeted(const asset::SkeletonAsset& target,
                                                const asset::SkeletonAsset& source,
                                                const asset::RetargetProfile& profile,
                                                const asset::AnimationBlendLayer* layers,
                                                u32 layerCount, i32 targetJointIndex,
                                                float3& outT, quat& outR, float3& outS);

    /// 重定向 + 多层混合的蒙皮矩阵
    static void ComputeSkinMatricesBlendedRetargeted(const asset::SkeletonAsset& target,
                                                     const asset::SkeletonAsset& source,
                                                     const asset::RetargetProfile& profile,
                                                     const asset::AnimationBlendLayer* layers,
                                                     u32 layerCount,
                                                     std::vector<float4x4>& outSkinMatrices,
                                                     std::vector<float4x4>* outWorldMatrices = nullptr);

    /// 驱动所有 SkeletalMeshComponent 一帧
    static void Update(World& world, f32 dt);

    /// 按当前关节层级与静态 TRS **重算逆绑定矩阵**（invBind = inverse(绑定世界矩阵)）
    /// 【用途】程序化改过绑定姿势之后（缩放骨骼长度、改关节朝向等）必须重算，
    /// 否则蒙皮矩阵 = world × invBind ≠ 单位，连绑定姿势都会变形/炸开。
    static void RebuildInverseBindMatrices(asset::SkeletonAsset& skel);
};

} // namespace he
