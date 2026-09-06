#pragma once

#include "Scene/MeshComponent.h"
#include "Scene/SkeletonAsset.h"

#include <memory>

// ============================================================
// SkeletalMeshComponent — 骨骼网格（对应 UE5 USkeletalMeshComponent）
//
// GPU 蒙皮管线（Phase C C1b）：
//   1. SkeletalMeshSystem::Update 每帧推进剪辑时间 → 采样关节 TRS
//      → 层级合成世界矩阵 → 蒙皮矩阵 = world × inverseBind
//   2. 骨骼矩阵写入 SSBO（bindless 注册，useInstanceID=3 模式）
//   3. 顶点着色器按 JOINTS/WEIGHTS 加权 4 个骨骼矩阵完成蒙皮
//
// 网格数据来自 SkeletonAsset（glTFLoader 解析的蒙皮顶点/索引）。
// MVP 限制：单剪辑播放（无混合）、无 LOD/缓存动画、Forward 路径。
// ============================================================

namespace he {

class SkeletalMeshComponent : public MeshComponent {
    HE_COMPONENT()
public:
    /// 设置骨架资产：上传蒙皮顶点/索引到 GPU（含 JOINTS/WEIGHTS 布局）
    void SetSkeleton(std::shared_ptr<asset::SkeletonAsset> skeleton);

    /// 播放剪辑（-1 = 绑定姿势）；越界下标安全降级为绑定姿势
    void PlayClip(i32 clipIndex, bool loop = true);

    std::shared_ptr<asset::SkeletonAsset> skeleton;   // 骨架资产（关节/剪辑/蒙皮网格）

    // --- 播放状态 ---
    i32   currentClip = -1;     // 当前剪辑（-1 = 绑定姿势）
    float clipTime    = 0.0f;   // 剪辑时间（秒）
    float playSpeed   = 1.0f;
    bool  playing     = true;
    bool  looping     = true;

    // --- GPU 侧状态（ForwardPipeline 管理，勿手动改）---
    bool  bBonesDirty = false;              // 骨骼矩阵已更新，待上传
    u32   boneSSBOHandle = 0;               // bindless SSBO 句柄
    std::unique_ptr<rhi::IRHIBuffer> boneBuffer;   // 骨骼矩阵缓冲（随组件存活）
    // 退役缓冲（bindless 堆 append-only：旧缓冲不销毁避免悬垂指针）
    std::vector<std::unique_ptr<rhi::IRHIBuffer>> retiredBoneBuffers;

    // --- 关节矩阵缓存（SkeletalMeshSystem 计算）---
    std::vector<float4x4> jointWorldMatrices;   // 世界矩阵（调试/层级用）
    std::vector<float4x4> boneMatrices;         // 蒙皮矩阵 = world × inverseBind（GPU SSBO 数据）
};

} // namespace he
