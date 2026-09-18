#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

#include <vector>

// ============================================================
// SkeletonAsset — 蒙皮网格资产（Phase C 骨骼动画数据层）
//
// 由 glTFLoader 从 glTF skins/animations/JOINTS_0/WEIGHTS_0 解析：
//   joints   ：关节层级（名称/父下标/逆绑定矩阵/静态 TRS）
//   clips    ：关节动画剪辑（TRS 关键帧，线性插值采样）
//   vertices ：蒙皮网格顶点（绑定姿势 + 关节索引/权重，GPU 蒙皮输入）
// ============================================================

namespace he::asset {

/// 蒙皮顶点（GPU 顶点布局：位置/法线/UV + 关节索引/权重）
struct SkinnedVertex {
    float3 position;
    float3 normal;
    float2 uv;
    u8     joint[4];     // 关节索引（最多 4 个影响，0 填充）
    float  weight[4];    // 权重（归一化到 1）
};

/// 关节（骨架节点）
struct SkeletonJoint {
    String   name;
    i32      parent = -1;               // 父关节下标（-1 = 根）
    float4x4 inverseBind;               // 逆绑定矩阵（蒙皮：Σ w·(world·invBind)·v）
    // 静态本地 TRS（无动画通道时使用）
    float3   translation = float3(0.0f);
    quat     rotation    = glm::identity<quat>();
    float3   scale       = float3(1.0f);
};

/// 单关节动画通道（TRS 关键帧；缺某分量的通道保持静态 TRS）
struct JointAnimationChannel {
    i32  jointIndex = -1;
    std::vector<float>  times;              // 关键帧时间（秒，升序）
    std::vector<float3> translations;       // 空 = 无该分量动画
    std::vector<quat>   rotations;          // 空 = 无该分量动画
    std::vector<float3> scales;             // 空 = 无该分量动画
};

/// 动画剪辑
struct AnimationClip {
    String name;
    float  duration = 0.0f;
    std::vector<JointAnimationChannel> channels;
};

/// 动画混合层（任务 21：剪辑混合 / Blend Space 的单层状态）
///
/// 一个层 = 一个剪辑 + **它自己的**播放时间 + 权重。系统按 Σweight 归一化后逐关节混合：
/// 平移/缩放线性加权，旋转"符号对齐到首个参与层后加权求和、再归一化"（加权 nlerp，
/// 避免 q 与 -q 的双覆盖把加权和拉向 0）；`clipIndex < 0`、权重 ≤ 0 的层不参与，
/// **全部层都不参与时退回关节静态 TRS**（即绑定姿势，与单剪辑路径的 clipIndex=-1 一致）。
///
/// 【为什么把层状态放在资产头】它是纯数据、无 RHI 依赖：单元测试不必链接渲染模块，
/// 采样/混合的判据可以只对着数学写（见 Tests/TestSkeletalMesh.cpp）。
struct AnimationBlendLayer {
    i32   clipIndex = -1;      // 剪辑下标（-1 = 绑定姿势/静态 TRS，不参与混合）
    float weight    = 0.0f;    // 相对权重（≤0 视为不参与）
    float time      = 0.0f;    // 该层自己的剪辑时间（秒）
    float speed     = 1.0f;    // 该层播放速度
    bool  looping   = true;    // 该层是否循环
};

/// 骨架资产（一个 glTF skin = 一份资产；多个 primitive 可共享）
struct SkeletonAsset {
    String name;
    std::vector<SkeletonJoint> joints;
    std::vector<AnimationClip> clips;
    std::vector<SkinnedVertex> vertices;    // 蒙皮网格（绑定姿势）
    std::vector<u32>           indices;
};

} // namespace he::asset
