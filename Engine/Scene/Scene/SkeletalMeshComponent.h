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
// MVP 限制：无 LOD/缓存动画、Forward 路径。
// 任务 21 起支持**剪辑混合**（多层权重 + 交叉淡入）：设置 `blendLayerCount > 0` 走混合路径，
// 否则保持原来的单剪辑路径（既有行为不变）。
// ============================================================

namespace he {

class SkeletalMeshComponent : public MeshComponent {
    HE_COMPONENT()
public:
    /// 混合层上限（超过的层被忽略；模型层的层数远超实际需求，4 层够"基础 + 上半身 + 两个过渡"）
    static constexpr u32 kMaxBlendLayers = 4;

    /// 设置骨架资产：上传蒙皮顶点/索引到 GPU（含 JOINTS/WEIGHTS 布局）
    void SetSkeleton(std::shared_ptr<asset::SkeletonAsset> skeleton);

    /// 播放剪辑（-1 = 绑定姿势）；越界下标安全降级为绑定姿势
    void PlayClip(i32 clipIndex, bool loop = true);

    // ── 剪辑混合（任务 21）──────────────────────────────────────────────
    /// 设置全部混合层（count 截到 kMaxBlendLayers；权重由采样端归一化）
    /// 【与单剪辑路径的关系】一旦 count>0，系统按层混合；`currentClip/clipTime` 只作显示用。
    void SetBlendLayers(const asset::AnimationBlendLayer* layers, u32 count);
    /// 设置第 index 层（index≥kMaxBlendLayers 或剪辑越界 → 返回 false 且不改状态）
    bool SetBlendLayer(u32 index, i32 clipIndex, float weight, float time = 0.0f,
                       float speed = 1.0f, bool loop = true);
    /// 清空混合层 → 回到单剪辑路径
    void ClearBlendLayers();
    /// 从"当前播放状态"交叉淡入到 toClip（duration≤0 = 立即切换）；越界 toClip 忽略
    void CrossFadeTo(i32 toClip, float duration, bool loop = true);
    /// 取归一化后的各层权重（调试/判据用；capacity 之外不写）
    void GetBlendWeights(float* out, u32 capacity) const;

    std::shared_ptr<asset::SkeletonAsset> skeleton;   // 骨架资产（关节/剪辑/蒙皮网格）

    // --- 播放状态 ---
    i32   currentClip = -1;     // 当前剪辑（-1 = 绑定姿势）；混合时仅作显示
    float clipTime    = 0.0f;   // 剪辑时间（秒）；混合时仅作显示
    float playSpeed   = 1.0f;
    bool  playing     = true;
    bool  looping     = true;

    // --- 混合状态（任务 21；blendLayerCount>0 时取代上面的单剪辑状态）---
    asset::AnimationBlendLayer blendLayers[kMaxBlendLayers];
    u32   blendLayerCount = 0;
    /// 交叉淡入：把 blendLayers[0]（出）权重 1→0、blendLayers[1]（入）0→1
    bool  bCrossFading      = false;
    float crossFadeTime     = 0.0f;   // 已经历时间（秒）
    float crossFadeDuration = 0.0f;   // 总时长（≤0 = 立即完成）

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
