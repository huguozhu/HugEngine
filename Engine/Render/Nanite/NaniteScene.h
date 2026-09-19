#pragma once

// ============================================================
// Nanite/NaniteScene.h — Nanite 的数据宿主（实例表 / cluster 表 / 几何量化缓冲 / LOD 错误）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由后续任务填充】
//   任务 1 只有生命周期桩：Initialize / Shutdown / OnResize / IsReady。
//   **不创建任何 GPU 资源、不参与渲染**。后续在这里追加：
//     任务 3  模块自持的"计数 → 间接绘制"链所需缓冲（§14.8 任务 3）
//     任务 5  Nanite 段的实例表（分区契约见 `NaniteTypes.h`）
//     任务 12 合并几何/量化缓冲与 LOD 错误的 GPU 侧宿主
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法，属于后续任务的 .cpp 细节）；
//   不得依赖 `MeshBatcher` 的运行时状态 —— 它只当**一次性输入**（合并几何）。
//   本类尤其不得反向依赖任何 GI/Lumen 类型：它是"数据放在哪"的答案，不是"GI 怎么算"。
// ============================================================

#include "Nanite/NaniteTypes.h"   // 实例槽分配器 + objectIndex 分区契约（RHI-free）
#include "RHI/RHI.h"

namespace he::render {

class NaniteScene {
public:
    NaniteScene() = default;
    ~NaniteScene() = default;

    NaniteScene(const NaniteScene&) = delete;
    NaniteScene& operator=(const NaniteScene&) = delete;

    /// 骨架就绪：只记住设备与视口尺寸。任务 12 起在这里建实例表/cluster 表/几何缓冲
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);

    /// 释放自持资源。任务 1 无资源可释放，只清指针（**不动开关真值**）
    void Shutdown();

    /// 视口变化：只影响与屏幕尺寸相关的资源（世界空间资源不在此重建）
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const { return m_Device != nullptr; }

    // ── §14.8 任务 5：Nanite 段实例槽分配器 ──
    // 【为什么在 NaniteScene】实例表的宿主在这里；"槽位"就是 Nanite 段的一个 objectIndex
    //   （全局值 = kNaniteObjectIndexBegin + 本地下标，见 NaniteTypes.h 的分区表）。

    /// 分配一个实例槽：返回**全局** objectIndex；容量耗尽返回 kInvalidObjectIndex。
    /// 耗尽时打印**一次**中文告警（`m_SlotExhaustedWarned` 保证不刷屏）—— 不静默越界、不崩。
    u32 AllocateInstanceSlot();

    /// 回收一个实例槽：只接受 Nanite 段里当前确实被占用的索引（普通段/哨兵/重复回收返回 false）
    bool FreeInstanceSlot(u32 objectIndex);

    [[nodiscard]] u32 AllocatedInstanceSlotCount() const { return m_Slots.AllocatedCount(); }
    [[nodiscard]] static constexpr u32 InstanceSlotCapacity() {
        return NaniteInstanceSlotAllocator::kCapacity;
    }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;

    /// Nanite 段的实例槽位图（RHI-free，逻辑全部在 NaniteTypes.h，便于单测）
    NaniteInstanceSlotAllocator m_Slots;
    /// 耗尽告警只打一次（与"每个实例一行日志"相比，避免每帧刷屏）
    bool m_SlotExhaustedWarned = false;
};

} // namespace he::render
