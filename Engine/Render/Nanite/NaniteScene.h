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

    // ============================================================
    // 【§14.8 任务 24】资产的 **CPU 留存**（页池的数据源；阶段一的硬前置）
    //
    // 【为什么必须有它（§14.32 ⑦ 的第三处追加）】流式阶段一的口径是"页数据源 = **已在内存的**
    //   完整资产"，但复核生命周期后确认这句话原先**不成立**：`NaniteRenderer::EnsureAssetUploaded`
    //   里的 `NanitePackedAsset asset;` 是**局部变量**，只以 `const&` 传给 `UploadPackedAsset`
    //   （本类**不保留**它），整条路径又被 `m_AssetUploaded` 门闩保证只跑一次
    //   ⇒ 上传之后 CPU 侧资产数据即被销毁，页池无页可拷。本函数就是那条硬前置的落地。
    //
    // 【为什么只留"三段 + 材质 + LOD 段"而不留整个 `NanitePackedAsset`】资产的 `bytes` 是
    //   五段的**字节镜像**（与那几个强类型容器是同一批数据的第二份拷贝）⇒ 一起留就是双倍内存，
    //   而它的唯一消费者（上传时的逐字节读回校验）在 `UploadPackedAsset` 内已经跑完。
    //   本函数因此**丢掉 `bytes`**，只留页池真正要用的段。
    //
    // 【代价（如实记录；Sponza 实测见 §14.37）】留存 = 簇段 + 顶点段 + 三角形段的常驻内存
    //   （**实测 13,405,960 字节 = 12.78 MiB**，由 `asset_retained … retained_bytes=` 量出）。
    //   它是"只开 `enabled` 也不变"这条不变式的**内存侧例外**：
    //   帧图、pass 集合与转储**逐位不变**（不变式 1 管的是这三件事），但进程内存会多这一份。
    //   替代路径（不留存、按需重建）需要原始几何，而 `MeshBatcher` 只被 `const&` 用过一次 ——
    //   两条路的裁决与后果写在 §14.37。
    // ============================================================

    /// 把资产的 CPU 副本留存下来（只留页池要用的段；`bytes` 镜像被丢弃）
    /// @param asset 通过 `std::move` 交进来的资产（调用方此后不得再使用它）
    /// @param materialBin 【§14.8 任务 25】是否**顺带**生成材质 bin（默认 false）
    ///   —— 默认档**不分配任何内存**（`m_MaterialBin.bins` 保持空）；只有开关打开时才建。
    ///   生成时机与资产留存同处（一次性），因此不需要门面另记一个门闩、也不会每帧重建。
    /// @return 是否留下了非空的三段
    [[nodiscard]] bool StoreAssetCPUCopy(NanitePackedAsset&& asset, bool materialBin = false);

    /// 留存的资产 CPU 副本（页池的数据源；未留存时为一份空资产）
    [[nodiscard]] const NanitePackedAsset& GetAssetCPUCopy() const { return m_AssetCPU; }

    // ============================================================
    // 【§14.8 任务 25】材质 bin（只读辅助数组；不是资产的一部分）
    //
    // 【存放位置的取舍】它**不放进** `NanitePackedAsset`：那个结构是"`.nanite` 字节镜像 + 分段
    //   强类型视图"，加一个字段就等于让资产本体多一份数据（与"不动资产本体"的硬性要求相悖，
    //   也会让 `ValidateNaniteFile` 之外多出一条"镜像与视图是否一致"的口径）。放在本类（资源的
    //   宿主）里，语义就是"由资产**派生**的只读索引表"，与 `m_Asset` 的 GPU 缓冲同级。
    // ============================================================

    /// 材质 bin（未生成时为空；`materialBin=false` 时永远为空）
    [[nodiscard]] const NaniteMaterialBin& GetMaterialBin() const { return m_MaterialBin; }
    /// 是否已经生成了可用的材质 bin（读数行据此决定打印 `bin=none` 还是真实长度）
    [[nodiscard]] bool HasMaterialBin() const { return !m_MaterialBin.bins.empty(); }

    /// 是否已经留存了可用的资产 CPU 副本
    [[nodiscard]] bool HasAssetCPUCopy() const {
        return !m_AssetCPU.clusters.empty() && !m_AssetCPU.vertices.empty()
            && !m_AssetCPU.triangles.empty();
    }

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

    /// 【§14.8 任务 24】留存的资产 CPU 副本（页池的数据源；`bytes` 镜像被丢弃）
    NanitePackedAsset m_AssetCPU;

    /// 【§14.8 任务 25】由留存资产派生的材质 bin（**只读辅助数组**；`materialBin=false` 时为空）
    NaniteMaterialBin m_MaterialBin;
};

} // namespace he::render
