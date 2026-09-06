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

namespace asset { struct SkeletonAsset; }

class SkeletalMeshSystem {
public:
    /// 采样关节本地 TRS（clipIndex=-1 或无该分量通道时用关节静态 TRS）
    static void SampleJointTRS(const asset::SkeletonAsset& skel, i32 clipIndex,
                               float time, i32 jointIndex,
                               float3& outT, quat& outR, float3& outS);

    /// 合成全部关节的蒙皮矩阵（world × inverseBind；长度 = joints 数）
    /// @param outWorldMatrices 可选：同时输出关节世界矩阵（调试用）
    static void ComputeSkinMatrices(const asset::SkeletonAsset& skel, i32 clipIndex, float time,
                                    std::vector<float4x4>& outSkinMatrices,
                                    std::vector<float4x4>* outWorldMatrices = nullptr);

    /// 驱动所有 SkeletalMeshComponent 一帧
    static void Update(World& world, f32 dt);
};

} // namespace he
