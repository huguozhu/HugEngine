// ============================================================
// Nanite/NaniteScene.cpp — 数据宿主的生命周期桩（任务 1）+ 实例槽分配器（任务 5）
//                            + 资产缓冲宿主/上传/读回校验（任务 12）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 3/5/12 填充】
//   任务 1 只有"记住设备与尺寸 / 清空"这几个动作，**没有任何 GPU 资源**。
//   任务 5 加了 Nanite 段的实例槽分配器（逻辑在 NaniteTypes.h，这里只做宿主 + 告警）。
//   任务 12 加了 `.nanite` 字节镜像的 GPU 缓冲宿主与一次性上传 + 真实读回校验
//     （`UploadPackedAsset`）。这是本任务里**唯一**碰 device 的一侧：
//     纯 CPU 的"合并几何 → 字节镜像"落在 `NaniteUpload.cpp`（必须保持 RHI-free，
//     它被 `HugEngineTests` 直接编译，见 `Tests/CMakeLists.txt:50-54`）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。本文件里没有、也不允许有这些依赖。
// ============================================================

#include "Nanite/NaniteScene.h"

#include "Core/Log.h"

#include <cstring>   // std::memcmp（任务 12：读回字节与 CPU 镜像逐字节比较）

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

    // 任务 12：旧的资产缓冲随重新初始化一并作废（新资产由 UploadPackedAsset 建立）
    m_Asset = AssetBuffers{};

    // 任务 24：留存的资产 CPU 副本同样作废（重新初始化 = 重新建资产）
    m_AssetCPU = NanitePackedAsset{};
    // 【任务 25】材质 bin 与资产同生命周期（它是资产的派生视图 ⇒ 必须一起作废）
    m_MaterialBin = NaniteMaterialBin{};

    return m_Device != nullptr;
}

void NaniteScene::Shutdown() {
    // 任务 1 没有自持资源；后续任务在这里释放实例表/cluster 表/几何缓冲。
    // 任务 5：槽位表一并清空 —— 不这样做的话，重新 Initialize 后旧编号会被"重复占用"。
    m_Slots.Reset();
    m_SlotExhaustedWarned = false;

    // 任务 12：资产缓冲随 unique_ptr 一起释放（重新 Initialize 会重新上传一次）
    m_Asset = AssetBuffers{};

    // 任务 24：留存的 CPU 副本一并释放（它可能有十几 MB，不能留在 shutdown 之后）
    m_AssetCPU = NanitePackedAsset{};

    // 【任务 25】材质 bin 一并释放（几万个 u32；同样不该留在 shutdown 之后）
    m_MaterialBin = NaniteMaterialBin{};

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

// ============================================================
// §14.8 任务 12：资产缓冲宿主 + 一次性上传 + 真实 GPU 读回校验
//
// 口径与分段理由写在 `NaniteScene.h` 的同名小节里；这里只留与代码逐句对应的短注释。
// 一句话：按任务 10 的段表切 6 片 → 各建一个 Storage 缓冲（`initialData` 上传）→
// 一条一次性命令表把每片 GPU 拷贝到读回缓冲的同偏移 → 等 GPU 完成后 Map → 逐字节比较。
// ============================================================
bool NaniteScene::UploadAssetInto(const NanitePackedAsset& asset, AssetBuffers& outAsset,
                                  bool waitForReadbackVerify) {
    if (!m_Device) {
        HE_CORE_ERROR("NaniteScene: 资产上传失败 —— 设备为空（Initialize 未成功？）");
        return false;
    }
    // 镜像至少要装得下 96B 文件头（打包器保证；这里是防御性判据，不做"部分上传"）
    if (asset.bytes.size() < kNaniteFileHeaderBytes) {
        HE_CORE_ERROR("NaniteScene: 资产上传失败 —— 字节镜像只有 {} B（至少要 {} B 的文件头）",
                      (unsigned long long)asset.bytes.size(),
                      (unsigned long long)kNaniteFileHeaderBytes);
        return false;
    }

    // 释放上一份（允许重复调用；正常路径由门面的"只做一次"门闩保证只调一次）
    outAsset = AssetBuffers{};

    // ── 1. 段切片：偏移/长度全部取自打包器的 `layout`（本函数**不重算**任何偏移 ⇒ 单一真值），
    //      6 片恰好无缝覆盖 `[0, totalBytes)`：头[0,96) + 五段首尾相接 ──
    struct Slice {
        const char*                        name;    ///< 诊断用段名
        usize                              offset;  ///< 在 `asset.bytes` 内的起点
        usize                              bytes;   ///< 段长（含 16B 对齐填充；0 = 空段）
        std::unique_ptr<rhi::IRHIBuffer>*  slot;    ///< 落到 `outAsset` 的哪个成员
    };
    const Slice slices[6] = {
        { "header",    0,                            kNaniteFileHeaderBytes,      &outAsset.header     },
        { "clusters",  asset.layout.clusterOffset,   asset.layout.clusterBytes,   &outAsset.clusters   },
        { "vertices",  asset.layout.vertexOffset,    asset.layout.vertexBytes,    &outAsset.vertices   },
        { "indices",   asset.layout.indexOffset,     asset.layout.indexBytes,     &outAsset.indices    },
        { "materials", asset.layout.materialOffset,  asset.layout.materialBytes,  &outAsset.materials  },
        { "lod",       asset.layout.lodOffset,       asset.layout.lodBytes,       &outAsset.lodOffsets },
    };

    for (const Slice& s : slices) {
        if (s.bytes == 0) continue;   // 空段（空资产）不建缓冲：Vulkan 不接受 0 长度的缓冲
        if (s.offset + s.bytes > asset.bytes.size()) {
            HE_CORE_ERROR("NaniteScene: 资产上传失败 —— 段 {} 越出镜像（{} + {} > {}）",
                          s.name, (unsigned long long)s.offset, (unsigned long long)s.bytes,
                          (unsigned long long)asset.bytes.size());
            outAsset = AssetBuffers{};
            return false;
        }
        rhi::BufferDesc desc;
        desc.size = s.bytes;
        // Storage：shader 侧按 SSBO 读；TransferSrc：本函数要把它当作 `CopyBuffer` 的**源**读回。
        //（RHI 的缓冲 usage 映射原本没有 TransferSrc 位 —— 任务 12 在
        //  `Engine/RHI/Vulkan/VulkanResources.cpp` 的 `ToVkBufferUsage` 补上该位：只增不改，
        //  且当前没有别的调用方请求它 ⇒ 既有全部缓冲的 VkBufferUsageFlags 一个位都不变。）
        desc.usage       = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc;
        desc.cpuAccess   = false;
        desc.initialData = asset.bytes.data() + s.offset;   // RHI 既有的上传入口
        *s.slot = m_Device->CreateBuffer(desc);
        if (!*s.slot) {
            HE_CORE_ERROR("NaniteScene: 资产上传失败 —— 段 {} 的缓冲创建失败（{} B）",
                          s.name, (unsigned long long)s.bytes);
            outAsset = AssetBuffers{};
            return false;
        }
    }

    // 【T8】帧内路径到这里就返回：段缓冲已建好并上传，同步读回校验留到工具/单测（true 分支）。
    // 继续往下会在"帧录制期" `Submit + WaitIdle` —— 实测 04/07 都死等在这一步
    // （队列被主帧占用，`vkQueueWaitIdle` 永远等不到）。
    if (!waitForReadbackVerify) {
        HE_CORE_INFO("NaniteScene: 资产段缓冲已上传（跳过同步读回校验：帧内路径）");
        return true;
    }

    // ── 2. 读回缓冲：一张覆盖整份镜像的 host 可见缓冲（GPU 拷贝的**目标**）──
    rhi::BufferDesc readbackDesc;
    readbackDesc.size      = asset.bytes.size();
    readbackDesc.usage     = rhi::BufferUsage::Storage;   // 该 usage 恒定带 TRANSFER_DST（可作拷贝目标）
    readbackDesc.cpuAccess = true;                        // 读回要 Map
    auto readback = m_Device->CreateBuffer(readbackDesc);
    if (!readback) {
        HE_CORE_ERROR("NaniteScene: 资产上传失败 —— 读回缓冲创建失败（{} B）",
                      (unsigned long long)readbackDesc.size);
        outAsset = AssetBuffers{};
        return false;
    }

    // ── 3. 一次性命令表：逐段 GPU 拷贝到读回缓冲的**同一偏移** ⇒ 整张读回缓冲就是镜像本身 ──
    // 【为什么用 BeginLightweight 而不是 Begin】`VulkanCommandList::Begin()` 会等待飞行帧栅栏并
    //   `AdvanceFrame()`（推进全局帧计数）。本上传发生在帧**内部**（帧图构建期，主命令表正在录制），
    //   再推进一次帧计数会让飞行帧槽位与延迟销毁队列错位。`BeginLightweight()` 只开始录制，
    //   正是为这种"帧内临时命令表"准备的入口（VulkanCommandList.cpp:270-280）。
    auto cmd = m_Device->CreateCommandList();
    if (!cmd) {
        HE_CORE_ERROR("NaniteScene: 资产上传失败 —— 一次性命令表创建失败");
        outAsset = AssetBuffers{};
        return false;
    }
    cmd->BeginLightweight();
    for (const Slice& s : slices) {
        if (s.bytes == 0) continue;
        cmd->CopyBuffer(s.slot->get(), readback.get(), s.bytes, 0, s.offset);
    }
    cmd->End();
    m_Device->Submit(cmd.get());
    m_Device->WaitIdle();   // 一次性路径：明确等 GPU 完成后再 Map（与既有白炉探针/落盘读回同一口径）

    // ── 4. 逐字节比较：**GPU 拷贝回来的字节** vs CPU 侧镜像 ──
    usize mismatch = 0;
    bool  mapped   = false;
    if (void* p = readback->Map()) {
        mapped = true;
        const u8* gpu = static_cast<const u8*>(p);
        // 【只比较已上传的固定五段】v2 文件在五段之后还有扩展段（材质描述/低模/归属），
        // 它们不参与 GPU 上传，拿它们去和读回缓冲比会必然错（实测 mismatch=755890）。
        const usize fiveSectionBytes = asset.layout.lodOffset + asset.layout.lodBytes;
        const usize compareBytes = std::min(asset.bytes.size(), fiveSectionBytes);
        for (usize i = 0; i < compareBytes; ++i) {
            if (gpu[i] != asset.bytes[i]) ++mismatch;
        }
        readback->Unmap();
    }
    if (!mapped) {
        HE_CORE_ERROR("NaniteScene: 资产读回失败 —— 读回缓冲不可映射（{} B）",
                      (unsigned long long)asset.bytes.size());
        outAsset = AssetBuffers{};
        return false;
    }

    outAsset.totalBytes    = asset.layout.lodOffset + asset.layout.lodBytes;   // 只算已上传的固定五段
    outAsset.mismatchBytes = mismatch;
    outAsset.verified      = (mismatch == 0u);

    // 【恰好一行】任务 12 的验收出口：`upload_bytes` = 镜像总字节（6 段之和），
    // `readback_match` = 1 表示逐字节一致；其余四个计数取自资产文件头。
    HE_CORE_INFO("[Nanite] upload_bytes={} readback_match={} mismatch_bytes={} "
                 "clusters={} vertices={} materials={} lod_levels={}",
                 (unsigned long long)outAsset.totalBytes,
                 outAsset.verified ? 1 : 0,
                 (unsigned long long)outAsset.mismatchBytes,
                 asset.header.clusterCount, asset.header.vertexCount,
                 asset.header.materialCount, asset.header.lodLevelCount);
    return outAsset.verified;
}

// T8：单资产兼容入口（等价于"清空注册表 + 注册一份"）
// 【为什么走 RegisterAsset】光栅端消费的是**聚合视图**（多资产的唯一视图）；单资产若只写
// `m_Asset`，聚合视图为空、光栅会读到空缓冲。走注册表保证"单资产 = 聚合视图里只有一份"，
// 也就保证与 T8 之前的渲染结果逐字节相同。
bool NaniteScene::UploadPackedAsset(const NanitePackedAsset& asset, bool waitForReadbackVerify) {
    ClearAssets();
    return RegisterAsset(asset, waitForReadbackVerify) != kInvalidAssetIndex;
}

// T8：多资产入口 —— 每份资产独立的 6 段缓冲，追加进注册表
u32 NaniteScene::RegisterAsset(const NanitePackedAsset& asset, bool waitForReadbackVerify) {
    if (m_Assets.size() >= 4096u) {   // 防御：注册表不设上限会被坏数据拖爆
        HE_CORE_ERROR("NaniteScene: 资产注册表已满（{}），拒绝注册", m_Assets.size());
        return kInvalidAssetIndex;
    }
    NaniteAssetEntry entry;
    if (!UploadAssetInto(asset, entry.buffers, waitForReadbackVerify)) return kInvalidAssetIndex;
    // CPU 副本只留页池要用的段：`bytes` 是五段的第二份拷贝，注册表不留
    entry.cpu = asset;
    entry.cpu.bytes.clear();
    entry.cpu.bytes.shrink_to_fit();
    m_Assets.push_back(std::move(entry));

    // ── T8：把这份资产追加进聚合五段，并把资产局部偏移改写成全局值 ──
    const u32 clusterBase  = (u32)m_MergedClusters.size();
    const u32 triangleBase = (u32)m_MergedTriangles.size();
    const u32 vertexBase   = (u32)m_MergedVertices.size();
    const u32 materialBase = (u32)m_MergedMaterials.size();

    for (const NaniteClusterRecord& src : asset.clusters) {
        NaniteClusterRecord r = src;
        r.triangleOffset += triangleBase;   // 三角形下标（× 8B = 索引段字节偏移）
        r.vertexOffset   += vertexBase;     // 顶点段下标（× 16B）
        r.materialID     += materialBase;   // 材质段下标
        if (r.childClusterOffset != 0u) r.childClusterOffset += clusterBase;   // 子簇是**簇下标**
        m_MergedClusters.push_back(r);
    }
    m_MergedVertices.insert(m_MergedVertices.end(), asset.vertices.begin(), asset.vertices.end());
    m_MergedTriangles.insert(m_MergedTriangles.end(), asset.triangles.begin(), asset.triangles.end());
    m_MergedMaterials.insert(m_MergedMaterials.end(), asset.materials.begin(), asset.materials.end());
    m_MergedLodOffsets.insert(m_MergedLodOffsets.end(), asset.lodOffsets.begin(), asset.lodOffsets.end());

    // 【GPU 缓冲不在这里建】延迟到 `BuildMergedBuffers()`（调用方在进帧图之前调一次）：
    // 注册期反复重建会让已缓存的描述符指向被销毁的 VkBuffer，实测直接访问违例。
    m_MergedValid = false;

    return (u32)m_Assets.size() - 1u;
}

// ============================================================
//  T8：聚合五段 → GPU 缓冲（一次性）
// ============================================================
bool NaniteScene::BuildMergedBuffers() {
    if (!m_Device) return false;
    if (m_MergedClusters.empty()) {
        m_MergedValid = false;
        return false;
    }

    // 【T8 修复：首帧卡死】用 `cpuAccess=true + Map/memcpy` 而不是 `initialData`：
    //   `initialData` 走"创建 staging + 提交拷贝 + 等待"的内部路径，在帧构建期调用会卡住
    //   （实测 04/07 都停在"离线资产加载成功"之后不再出帧）。主机写与 `NaniteCull` 的
    //   资产缓冲上传口径一致，都是一次性启动路径。
    auto uploadSection = [this](std::unique_ptr<rhi::IRHIBuffer>& slot, const void* data, usize bytes,
                                const char* debugName) {
        slot.reset();
        if (bytes == 0u) return true;
        rhi::BufferDesc desc;
        desc.size      = bytes;
        desc.usage     = rhi::BufferUsage::Storage;
        desc.cpuAccess = true;
        desc.debugName = debugName;
        slot = m_Device->CreateBuffer(desc);
        if (!slot) return false;
        if (void* dst = slot->Map()) {
            std::memcpy(dst, data, bytes);
            slot->Unmap();
        }
        return true;
    };

    const bool ok =
        uploadSection(m_Merged.clusters, m_MergedClusters.data(),
                      m_MergedClusters.size() * sizeof(NaniteClusterRecord), "Nanite.MergedClusters") &&
        uploadSection(m_Merged.vertices, m_MergedVertices.data(),
                      m_MergedVertices.size() * sizeof(NaniteVertex), "Nanite.MergedVertices") &&
        uploadSection(m_Merged.indices, m_MergedTriangles.data(),
                      m_MergedTriangles.size() * sizeof(NanitePackedTriangle), "Nanite.MergedIndices") &&
        uploadSection(m_Merged.materials, m_MergedMaterials.data(),
                      m_MergedMaterials.size() * sizeof(NaniteMaterialRecord), "Nanite.MergedMaterials") &&
        uploadSection(m_Merged.lodOffsets, m_MergedLodOffsets.data(),
                      m_MergedLodOffsets.size() * sizeof(u32), "Nanite.MergedLod");
    m_MergedValid = ok;
    if (!ok) {
        HE_CORE_ERROR("NaniteScene: 聚合资产缓冲创建失败（{} 簇 / {} 顶点）",
                      m_MergedClusters.size(), m_MergedVertices.size());
        return false;
    }
    HE_CORE_INFO("NaniteScene: 聚合资产视图就绪（资产 {}：簇 {} / 顶点 {} / 三角形 {} / 材质 {}）",
                 m_Assets.size(), m_MergedClusters.size(), m_MergedVertices.size(),
                 m_MergedTriangles.size(), m_MergedMaterials.size());
    return true;
}

// ============================================================
// §14.8 任务 24：资产 CPU 留存（页池的数据源；口径与代价见 `NaniteScene.h`）
// ============================================================
bool NaniteScene::StoreAssetCPUCopy(NanitePackedAsset&& asset, bool materialBin) {
    // 释放上一份（允许重复调用；正常路径由门面的"只做一次"门闩保证只调一次）
    m_AssetCPU = NanitePackedAsset{};
    m_MaterialBin = NaniteMaterialBin{};   // 【任务 25】bin 与资产同生命周期：资产换 ⇒ bin 作废
    m_AssetCPU = std::move(asset);

    // 【丢掉字节镜像】它是五段的第二份拷贝，唯一消费者（上传时的逐字节读回校验）已经跑完；
    //   留着只会让常驻内存翻倍。`shrink_to_fit` 之后那份 buffer 真正还给分配器。
    const usize droppedBytes = m_AssetCPU.bytes.size();
    m_AssetCPU.bytes.clear();
    m_AssetCPU.bytes.shrink_to_fit();

    const usize clusters  = m_AssetCPU.clusters.size()  * sizeof(NaniteClusterRecord);
    const usize vertices  = m_AssetCPU.vertices.size()  * sizeof(NaniteVertex);
    const usize triangles = m_AssetCPU.triangles.size() * sizeof(NanitePackedTriangle);
    const usize materials = m_AssetCPU.materials.size() * sizeof(NaniteMaterialRecord);
    const usize total     = clusters + vertices + triangles + materials;
    HE_CORE_INFO("[Nanite] asset_retained clusters={} vertices={} triangles={} materials={} "
                 "retained_bytes={} dropped_mirror_bytes={}",
                 (u32)m_AssetCPU.clusters.size(), (u32)m_AssetCPU.vertices.size(),
                 (u32)m_AssetCPU.triangles.size(), (u32)m_AssetCPU.materials.size(),
                 (unsigned long long)total, (unsigned long long)droppedBytes);

    // ── 【§14.8 任务 25】材质 bin（**只在开关打开时**建；默认档连一个字节都不分配）──
    // 【为什么在这里、为什么只一次】本函数是"资产进模块"的唯一入口、且由门面的 `m_AssetUploaded`
    //   门闩保证只跑一次 ⇒ 与"上传期一次"这条口径天然重合，不需要再加一个门闩，也不可能每帧重建。
    // 【为什么数据源是刚留存的 CPU 资产】不另持有第二份几何：bin 只是一个 `u32[簇数]` 的下标排列，
    //   由 `clusters[i].materialID` 直接算出（排序键就在簇记录里）。
    // 【不动的东西】资产本体（含 `bytes` 镜像已丢弃后的分段视图）、BVH/DAG、页池/页表、
    //   `.nanite` 格式**一个字节都不动**：这里是**新增**一个派生数组，没有任何回写。
    if (materialBin) {
        const u32 materialCount = (u32)m_AssetCPU.materials.size();
        const NaniteMaterialBinStats binStats =
            BuildNaniteMaterialBin(m_AssetCPU.clusters, materialCount, m_MaterialBin);
        // 如实报告三个数（不隐藏越界）；越界时 bin 为空 ⇒ 读数行会打 `bin=none`。
        HE_CORE_INFO("[Nanite] material_bin clusters={} distinct={} materials={} out_of_range={} "
                     "bin_bytes={}",
                     binStats.clusterCount, binStats.distinctCount, binStats.materialCount,
                     binStats.outOfRangeCount,
                     (unsigned long long)(m_MaterialBin.bins.size() * sizeof(u32)));
        if (binStats.outOfRangeCount != 0u) {
            HE_CORE_WARN("[Nanite] 材质 bin 未生成：有 {} 个簇的 materialID 越出材质段 [0, {})"
                         "（不 clamp、不静默：越界的 bin 会把簇分进错材质组）",
                         binStats.outOfRangeCount, materialCount);
        }
    }

    return HasAssetCPUCopy();
}

} // namespace he::render
