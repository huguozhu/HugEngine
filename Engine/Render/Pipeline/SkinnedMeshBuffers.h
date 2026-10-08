#pragma once

#include "RHI/RHI.h"
#include "RHI/FrameRetireQueue.h"

#include <memory>
#include <unordered_map>

// ============================================================
// SkinnedMeshBuffers — 骨骼矩阵缓冲的**渲染侧**状态表（阶段 1 §15.1 第③段）
//
// 【它解决什么问题】骨骼矩阵缓冲原先整份挂在 `SkeletalMeshComponent` 上
//（`boneBuffer` / `boneBufferCapacity` / `bBonesDirty` / `boneSSBOHandle` / 退役队列）——
// 只要它们还在组件上，骨骼上传就必须在渲染期遍历世界（B1 与组件指针两项闸门都降不下来）。
// 本类就是 E-2② 里"**矩阵数据走快照、缓冲生命周期留渲染侧**"的落点，与第①段的实例缓冲
// （`InstanceCuller::InstanceState`）**完全同款**：
//   · **数据**（`float4x4` 数组）由 `FrameSceneSnapshot::skinMatrices` 按值送达；
//   · **生命周期**（创建 / 扩容 / 退役 / 回收）留在渲染侧，按 `meshIndex` 索引。
//
// 【为什么按 meshIndex 而不是组件地址】快照里只有 `meshIndex`（不带指针）；注册表也按
// `meshIndex` 寻址，顶点/索引缓冲同样从那里取 —— 三者口径一致。
//
// 【版本号取代脏标记】组件侧只留 `boneMatrixVersion`（动画系统每次重算骨骼矩阵 +1）。
// 渲染侧比较"本帧版本 ≠ 上次上传版本"决定是否重传 —— 该判据跨帧幂等，适合另一根线程。
// ============================================================

namespace he::render {

class SkinnedMeshBuffers {
public:
    /// 逐 meshIndex 的状态（与 `InstanceCuller::InstanceState` 同构）
    struct State {
        std::unique_ptr<rhi::IRHIBuffer> buffer;   // 骨骼矩阵 SSBO
        u32  capacity        = 0;                  // 已分配缓冲可容纳的矩阵数
        u32  ssboHandle      = 0;                  // bindless SSBO 句柄（容量不变则句柄不变）
        u32  uploadedVersion = 0;                  // 已上传的骨骼版本（= 快照里的 skinMatrixVersion）
        u32  boneCount       = 0;                  // 上次上传的矩阵数（统计/判据）
        u64  ownerEntity     = 0;                  // 来源实体 id（注册表索引复用后据此重置）
        bool hasUpload       = false;              // 是否已成功上传过至少一次

        /// 退役缓冲（任务 23 的**有界** N 帧延迟释放）
        rhi::FrameRetireQueue<std::unique_ptr<rhi::IRHIBuffer>> retired;

        // 本帧/上帧是否在快照里出现过（用于组件销毁后的资源回收）
        bool seenThisFrame = false;
        bool seenLastFrame = false;
    };

    /// 只读统计（样例调试面板 / 判据）
    struct Stats {
        bool valid      = false;
        u32  ssboHandle = 0;
        u32  capacity   = 0;
        u32  retired    = 0;   // 待释放的退役缓冲数（验证"有界"）
        u32  boneCount  = 0;
    };

    /// 帧边界（每帧在遍历骨骼网格之前调用一次）：推进各条目的退役队列，
    /// 并释放"上一帧起就没再出现"的条目（组件已销毁）⇒ 避免缓冲与 bindless 槽位泄漏。
    void BeginFrame(rhi::IRHIDevice* device);

    /// 上传/复用某个 meshIndex 的骨骼矩阵缓冲（按值吃数据 ⇒ 渲染侧不再读组件）
    /// · 版本未变且容量够 → 直接返回既有句柄（不重传）
    /// · 容量不够 → 新建 + 旧缓冲进有界退役队列 + 释放旧 bindless 槽位
    /// · 容量够但版本变了 → Map 原地更新（句柄与槽位都不变）
    /// · 同一 meshIndex 换了来源实体（注册表索引被回收后复用）→ 丢弃旧缓冲并强制重传
    /// @return 骨骼矩阵 SSBO 的 bindless 句柄（0 = 不可用）
    u32 Upload(rhi::IRHIDevice* device, u32 meshIndex, u64 ownerEntity,
               const float4x4* matrices, u32 count, u32 version);

    /// 取某 meshIndex 的状态（不存在返回 nullptr）
    [[nodiscard]] State* Find(u32 meshIndex);

    /// 只读统计（不改变状态）
    [[nodiscard]] Stats GetStats(u32 meshIndex) const;

    /// 释放全部（设备仍有效时调用：回收 bindless 槽位）
    void Shutdown(rhi::IRHIDevice* device);

    [[nodiscard]] u32 Count() const { return static_cast<u32>(m_States.size()); }

private:
    /// 【为什么用 unordered_map】`meshIndex` 由网格注册表分配（稀疏、可能复用）；
    /// unordered_map 的元素地址稳定 ⇒ `Find` 返回的指针在插入/删除其它条目后依然有效
    ///（回收只发生在 `BeginFrame` 里，那时还没有人持有本帧的指针）。
    std::unordered_map<u32, State> m_States;
};

} // namespace he::render
