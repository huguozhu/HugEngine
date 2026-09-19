// ============================================================
// Nanite/NaniteScene.cpp — 数据宿主的生命周期桩（任务 1）+ 实例槽分配器（任务 5）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 3/5/12 填充】
//   任务 1 只有"记住设备与尺寸 / 清空"这几个动作，**没有任何 GPU 资源**。
//   任务 5 加了 Nanite 段的实例槽分配器（逻辑在 NaniteTypes.h，这里只做宿主 + 告警）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。本文件里没有、也不允许有这些依赖。
// ============================================================

#include "Nanite/NaniteScene.h"

#include "Core/Log.h"

// 【任务 5 的编译期交叉验证】普通段容量必须等于既有 GPUObjectData 缓冲上限 MAX_OBJECTS
// （= kGPUMaxObjects）。这个断言**故意放在这里而不是 NaniteTypes.h**：Material.h 会牵入
// RHI 头，而 NaniteTypes.h 必须保持 RHI-free（Scene 侧要 include 它，§14.7）。
// 本文件本来就在 Render 目标里，离 MAX_OBJECTS 最近，是放这条兜底最合适的位置。
#include "Pipeline/Material.h"
static_assert(he::render::kNormalObjectIndexCapacity == he::render::MAX_OBJECTS,
              "普通段容量必须等于 MAX_OBJECTS(=kGPUMaxObjects)：GPUObjectData 缓冲就是这么大，"
              "不一致就会让按普通段解码的索引越界");

namespace he::render {

bool NaniteScene::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    // 任务 1：骨架就绪的判据就是"拿到设备"。后续任务在这里创建缓冲，
    // 并把"资源是否真的建成"纳入这个返回值。
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    // 任务 5：实例槽表回到全空闲（重新初始化 = 重建场景，旧实例编号一律作废）
    m_Slots.Reset();
    m_SlotExhaustedWarned = false;

    return m_Device != nullptr;
}

void NaniteScene::Shutdown() {
    // 任务 1 没有自持资源；后续任务在这里释放实例表/cluster 表/几何缓冲。
    // 任务 5：槽位表一并清空 —— 不这样做的话，重新 Initialize 后旧编号会被"重复占用"。
    m_Slots.Reset();
    m_SlotExhaustedWarned = false;

    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
}

void NaniteScene::OnResize(u32 width, u32 height) {
    m_Width  = width;
    m_Height = height;
}

// ============================================================
// §14.8 任务 5：Nanite 段实例槽分配器（宿主侧）
// ============================================================

u32 NaniteScene::AllocateInstanceSlot() {
    const u32 objectIndex = m_Slots.Allocate();
    if (objectIndex == kInvalidObjectIndex) {
        // 【只告警一次】容量耗尽是**可恢复**的：返回哨兵，调用方跳过该实例即可。
        // 不静默（否则混排场景里实例会凭空消失）、不崩（否则一个超量场景就杀掉进程）。
        if (!m_SlotExhaustedWarned) {
            m_SlotExhaustedWarned = true;
            HE_CORE_WARN("NaniteScene: Nanite 段实例槽已耗尽（容量 {}，objectIndex 段 [{}, {})）"
                         "—— 本次及后续分配返回哨兵 {}，不再重复告警；"
                         "要更多实例必须换 MRT7 格式并同步 NaniteTypes.h 的分区表",
                         NaniteInstanceSlotAllocator::Capacity(),
                         kNaniteObjectIndexBegin, kObjectIndexTotalCapacity,
                         kInvalidObjectIndex);
        }
    }
    return objectIndex;
}

bool NaniteScene::FreeInstanceSlot(u32 objectIndex) {
    // 只回收 Nanite 段里当前被占用的槽；普通段索引/哨兵/重复回收一律拒绝
    //（把"普通段的索引"误当实例槽回收，正是分区契约要挡住的错误之一）。
    return m_Slots.Free(objectIndex);
}

} // namespace he::render
