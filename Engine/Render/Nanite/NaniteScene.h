#pragma once

// ============================================================
// Nanite/NaniteScene.h — Nanite 的数据宿主（实例表 / cluster 表 / 几何量化缓冲 / LOD 错误）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由后续任务填充】
//   任务 1 只有生命周期桩：Initialize / Shutdown / OnResize / IsReady。
//   **不创建任何 GPU 资源、不参与渲染**。后续在这里追加：
//     任务 3  模块自持的"计数 → 间接绘制"链所需缓冲（§14.8 任务 3）
//     任务 5  Nanite 段的实例表（分区契约见 `NaniteTypes.h`）
//     任务 12 合并几何/量化缓冲与 LOD 错误的 GPU 侧宿主（本任务：`UploadPackedAsset`）
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法，属于后续任务的 .cpp 细节）；
//   不得依赖 `MeshBatcher` 的运行时状态 —— 它只当**一次性输入**（合并几何）。
//   本类尤其不得反向依赖任何 GI/Lumen 类型：它是"数据放在哪"的答案，不是"GI 怎么算"。
// ============================================================

#include "Nanite/NaniteTypes.h"    // 实例槽分配器 + objectIndex 分区契约（RHI-free）
#include "Nanite/NaniteUpload.h"   // 任务 12：`NanitePackedAsset`（CPU 侧字节镜像，RHI-free 头）
#include "RHI/RHI.h"

#include <memory>   // 任务 12：GPU 缓冲的 unique_ptr 宿主

namespace he::render {

class NaniteScene {
public:
    NaniteScene() = default;
    ~NaniteScene() = default;

    NaniteScene(const NaniteScene&) = delete;
    NaniteScene& operator=(const NaniteScene&) = delete;

    /// 骨架就绪：只记住设备与视口尺寸。任务 12 起在这里建实例表/cluster 表/几何缓冲
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);

    /// 释放自持资源（含任务 12 的资产缓冲）。**不动开关真值**
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

    // ============================================================
    // §14.8 任务 12：`.nanite` 字节镜像的 GPU 侧宿主 + 一次性上传 + 读回校验
    //
    // 【段划分：为什么是 1 + 5 个 `StorageBuffer`（而不是一整块）】
    //   任务 10 的镜像布局是 `[96B 头][簇][顶点][索引][材质][LOD]`，各段在文件里连续、16B 对齐。
    //   本类把它切成 **6 个独立缓冲**（头 + 五段），理由：
    //     ① 后续 pass（任务 13+ 的剔除读簇表、光栅读顶点/索引、材质解析读材质段）各自只绑
    //        自己要用的那一段 —— `UpdateDescriptorSet` 不带偏移，分段建缓冲才不必额外做
    //        "基址 + 段内偏移"的搬运与对齐假设；
    //     ② 段长与镜像里的段长**逐字节相同**（含 16B 对齐填充）⇒ 上传校验可以按"切片"逐字节比，
    //        不需要任何重排；
    //     ③ 头也建缓冲（96B）：`clusterCount` / `bboxMin/Max` / `maxLODError` 是 shader 侧
    //        解码要用的量，且这样 6 段恰好**无缝覆盖** `[0, totalBytes)` ⇒ 校验覆盖整份镜像。
    //   零字节段（空资产）**不建缓冲、不拷贝**（Vulkan 不允许 0 长度的缓冲/拷贝）。
    //
    // 【上传路径与读回口径】上传 = `BufferDesc::initialData`（RHI 既有的上传入口，与
    //   `LumenSDF::UploadGeometry` 同款）；读回 = 一次性的 `CopyBuffer(段 → host 可见缓冲)`
    //   命令 + `Submit` + `WaitIdle` + `Map`，与 `NaniteRaster` 的
    //   `CopyTextureToBuffer → Map`（§14.8 任务 6）以及 `LumenSDF` 的探针读回同一套既有做法，
    //   不新造同步机制。**注意本引擎的缓冲都是持久映射的 host-visible 内存**
    //   （`VulkanResources.cpp:121-135` 的 VMA 参数），所以"GPU 读回"的证据来自那条
    //   **GPU 拷贝命令**，而不是来自 CPU 直接读自己写过的映射指针。
    // ============================================================

    /// GPU 侧的资产缓冲组（每个成员为空表示该段不存在：空资产或创建失败）
    struct AssetBuffers {
        std::unique_ptr<rhi::IRHIBuffer> header;      ///< [0, 96)           文件头（计数/范围/误差）
        std::unique_ptr<rhi::IRHIBuffer> clusters;    ///< 簇段（每次出现 64B）
        std::unique_ptr<rhi::IRHIBuffer> vertices;    ///< 顶点段（共享内容 16B/条）
        std::unique_ptr<rhi::IRHIBuffer> indices;     ///< 索引段（3×u16 进 u32[2]，8B/三角形）
        std::unique_ptr<rhi::IRHIBuffer> materials;   ///< 材质段（8B/条）
        std::unique_ptr<rhi::IRHIBuffer> lodOffsets;  ///< LOD 段（每级一个 u32）

        usize totalBytes    = 0;      ///< 已上传的镜像字节数（= `asset.bytes.size()`）
        usize mismatchBytes = 0;      ///< 读回逐字节比较的不一致字节数（0 = 完全一致）
        bool  verified      = false;  ///< 是否做过一次成功的读回校验（mismatchBytes == 0）
    };

    /// 【§14.8 任务 12】按任务 10 的镜像布局建 6 个段缓冲 → 一次性上传 → **真实 GPU 读回**
    /// 与 `asset.bytes` 逐字节比较 → 打印**恰好一行**
    /// `[Nanite] upload_bytes=<N> readback_match=<0|1> mismatch_bytes=<M> clusters=<C> vertices=<V> materials=<M2> lod_levels=<L>`。
    ///
    /// 【调用时机与次数】由 `NaniteRenderer::EnsureAssetUploaded()` 在开关开启时**只调一次**。
    /// 【失败】设备/镜像无效、任一缓冲创建失败 ⇒ 返回 false（不打印成功行；不一致时打印
    ///   实际的不一致字节数，不撒谎）。重复调用会先释放上一份。
    /// 【同步约定】函数内部自己 `Submit` + `WaitIdle`（一次性，不在每帧路径上）。
    [[nodiscard]] bool UploadPackedAsset(const NanitePackedAsset& asset);

    /// 已上传的资产缓冲组（任务 13+ 绑定用；本任务只创建 + 校验）
    [[nodiscard]] const AssetBuffers& GetAssetBuffers() const { return m_Asset; }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;

    /// Nanite 段的实例槽位图（RHI-free，逻辑全部在 NaniteTypes.h，便于单测）
    NaniteInstanceSlotAllocator m_Slots;
    /// 耗尽告警只打一次（与"每个实例一行日志"相比，避免每帧刷屏）
    bool m_SlotExhaustedWarned = false;

    /// 【§14.8 任务 12】资产缓冲组（GPU 资源宿主；`Shutdown` 时随 unique_ptr 一起释放）
    AssetBuffers m_Asset;
};

} // namespace he::render
