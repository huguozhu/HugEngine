// ============================================================
// Nanite/NaniteCull.cpp — 「计数 → 间接绘制」链的计数端（§14.8 任务 3）
//   + 实例剔除（任务 13）+ per-instance cluster BVH（任务 14）
//   + 三阶段簇剔除（任务 15：Phase 1 掩码 → Phase 2 Hi-Z → Phase 3 LOD 选择）
//
// 【本文件由 §14.8 任务 1 建立骨架，任务 3 填充 "计数端"】
//   ① 自持四个缓冲（假簇输入 / 间接命令 / 计数 / 光栅化簇计数）；
//   ② compute（Nanite_Cull.comp.slang）逐簇压缩写间接命令 + 原子累加计数；
//   ③ 屏障 `ComputeShader → DrawIndirect`，供绘制端的 `DrawIndexedIndirectCount` 消费。
//
// 【清零一律在命令缓冲内做（任务 13 的教训，任务 15 已推及全部计数器）】
//   录制期的**主机写**与派发之间没有排序，而引擎允许多帧在飞、CPU 领先 GPU ⇒ 第 N+1 帧写的 0
//   会落在第 N 帧派发**之前**，两帧原子累加叠加（任务 13 实测恰为 CPU 参考的 **2 倍**）。
//   因此本文件里**每一个每帧计数器**的清零都改成命令缓冲内的拷贝（常驻 0 的 TransferSrc 源 +
//   `Transfer → Compute` 屏障，末尾屏障补 `CopyDst` 消 WAR）；任务 3 的三个每帧重置（计数 /
//   光栅化簇计数 / 间接命令的哨兵填充）在任务 15 一并改掉 —— 过去那三处是**主机写**。
//   【唯一保留的主机写】假簇输入表本身（内容每帧逐位相同 ⇒ 竞争无害，已注明）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态。
//   本文件不得 include `Pipeline/GPUCulling.h`（Hi-Z 的构建以回调形式传入）。
// ============================================================

#include "Nanite/NaniteCull.h"

#include "Core/Log.h"

#include "Nanite_Cull.comp.spv.h"             // 由 Shader 编译管线生成（slangc → SPIR-V → spv_to_header.py）
#include "Nanite_InstanceCull.comp.spv.h"     // 【任务 13】同上
#include "Nanite_ClusterBVH.comp.spv.h"       // 【任务 14】per-instance cluster BVH 的 DFS 遍历
#include "Nanite_HiZDownsample.comp.spv.h"    // 【任务 15】模块自持的 Hi-Z 下采样（与既有 HiZDownsample 同口径）

// 【任务 13 的契约交叉验证】`NaniteInstanceGpuObject` 是 `GPUSceneObject` 的 RHI-free 镜像。
// 只有本 .cpp（Render 目标）能 include 真身，因此把"逐字段同布局"这条硬契约钉在这里：
// 任何一边改字段/顺序/对齐都会在编译期炸掉，而不是在运行期悄悄错位。
#include "Pipeline/GPUScene.h"

#include <algorithm>
#include <cmath>      // std::ceil / std::sqrt（合成实例网格）
#include <cstddef>    // offsetof
#include <cstring>

// ── GPUSceneObject（128B）契约的编译期钉子（真身见 Pipeline/GPUScene.h:26-40）──
static_assert(sizeof(he::render::NaniteInstanceGpuObject) == sizeof(he::render::GPUSceneObject),
              "实例表镜像必须与 GPUSceneObject 同尺寸（128B）");
#define HE_NANITE_ASSERT_FIELD_OFFSET(field)                                                   \
    static_assert(offsetof(he::render::NaniteInstanceGpuObject, field)                          \
                      == offsetof(he::render::GPUSceneObject, field),                           \
                  "NaniteInstanceGpuObject::" #field " 的偏移必须与 GPUSceneObject 一致")
HE_NANITE_ASSERT_FIELD_OFFSET(localToWorld);
HE_NANITE_ASSERT_FIELD_OFFSET(boundsMin);
HE_NANITE_ASSERT_FIELD_OFFSET(boundsMax);
HE_NANITE_ASSERT_FIELD_OFFSET(meshIndex);
HE_NANITE_ASSERT_FIELD_OFFSET(materialIndex);
HE_NANITE_ASSERT_FIELD_OFFSET(objectID);
HE_NANITE_ASSERT_FIELD_OFFSET(visibilityFlags);
HE_NANITE_ASSERT_FIELD_OFFSET(indexCount);
HE_NANITE_ASSERT_FIELD_OFFSET(firstIndex);
HE_NANITE_ASSERT_FIELD_OFFSET(vertexOffset);
HE_NANITE_ASSERT_FIELD_OFFSET(_pad);
#undef HE_NANITE_ASSERT_FIELD_OFFSET

namespace he::render {

// 未写入槽位的哨兵值：读回时用它区分"GPU 真写过"与"还留着上一帧或初始值"。
// 取 0xFFFFFFFF 是因为合法的 indexCount/instanceCount 不可能同时为该值。
static constexpr u32 kNaniteCmdSentinel = 0xFFFFFFFFu;

// 【任务 13】可见实例列表未写槽位的哨兵 = 0xFFFFFFFF（合法的实例下标不可能是它）。
// 只在 `ResetInstanceCullBuffers`（启动初值）里用来填充；每帧不再重置列表，理由见该函数。
static constexpr u32 kNaniteVisibleSentinel = 0xFFFFFFFFu;

// 【任务 15】任务 3 那三个"每帧重置"的哨兵源缓冲大小：间接命令缓冲整块 = 1024 × 20B = 20KB。
static constexpr u64 kNaniteIndirectSentinelBytes =
    (u64)sizeof(NaniteIndirectCommand) * (u64)kNaniteMaxFakeClusters;

// 【任务 15】**唯一的清零源**（常驻 0，TransferSrc）里的槽位偏移：每帧用命令缓冲内的拷贝把
//   各计数器清零。集中成一个缓冲（而不是任务 13/14 那样每个计数器一个源）是为了让"清零"这件事
//   只有一处常驻资源、一处写法，也顺带把任务 3 的两个主机写清零一并收进来。
static constexpr u64 kNaniteZeroSlotVisibleInstances = 0u;    ///< 4B：任务 13 的可见实例计数
static constexpr u64 kNaniteZeroSlotFakeCount        = 4u;    ///< 4B：任务 3 的命令条数
static constexpr u64 kNaniteZeroSlotFakeRaster       = 8u;    ///< 4B：任务 3 的光栅化簇计数
static constexpr u64 kNaniteZeroSlotClusterCount     = 12u;   ///< 4B：任务 14/15 的最终可见簇计数
static constexpr u64 kNaniteZeroSlotStats            = 16u;   ///< 64B：任务 15 的三阶段读数（整块）
static constexpr u64 kNaniteZeroSlotDrawCount        = 80u;   ///< 4B：【任务 16】绘制计数
static constexpr u64 kNaniteZeroSourceBytes          = 96u;   ///< 清零源总大小（16B 对齐）
static_assert(kNaniteZeroSlotStats + kNaniteCullStatsBytes <= kNaniteZeroSourceBytes,
              "读数零块必须落在清零源缓冲内");
static_assert(kNaniteZeroSlotDrawCount + sizeof(u32) <= kNaniteZeroSourceBytes,
              "绘制计数的零块必须落在清零源缓冲内");

bool NaniteCull::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    if (!m_Device) return false;

    // ── 1. 四个自持缓冲 ──
    {
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteFakeCluster) * kNaniteMaxFakeClusters;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // CPU 每帧上传假簇
        m_FakeClusterBuf = m_Device->CreateBuffer(d);
        if (!m_FakeClusterBuf) { HE_CORE_ERROR("NaniteCull: 假簇输入缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteIndirectCommand) * kNaniteMaxFakeClusters;
        // Indirect：vkCmdDrawIndexedIndirectCount 要求命令缓冲带 INDIRECT_BUFFER usage
        d.usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect;
        d.cpuAccess = true;   // 每帧填哨兵 + dump 帧读回
        m_IndirectCmdBuf = m_Device->CreateBuffer(d);
        if (!m_IndirectCmdBuf) { HE_CORE_ERROR("NaniteCull: 间接命令缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = kNaniteCountBufferSize;
        // 计数缓冲同时是 `DrawIndexedIndirectCount` 的 countBuffer ⇒ 必须带 INDIRECT usage
        d.usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect;
        d.cpuAccess = true;   // 每帧清零 + dump 帧读回
        m_CountBuf = m_Device->CreateBuffer(d);
        if (!m_CountBuf) { HE_CORE_ERROR("NaniteCull: 计数缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32);
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // 每帧清零 + dump 帧读回
        m_RasterCountBuf = m_Device->CreateBuffer(d);
        if (!m_RasterCountBuf) { HE_CORE_ERROR("NaniteCull: 光栅化簇计数缓冲创建失败"); return false; }
    }

    // ── 1b. 【任务 13】实例剔除的四个自持缓冲（与任务 3 的四条完全独立）──
    {
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteInstanceGpuObject) * kNaniteMaxTestInstances;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // CPU 每帧上传合成实例表
        m_InstanceBuf = m_Device->CreateBuffer(d);
        if (!m_InstanceBuf) { HE_CORE_ERROR("NaniteCull: 实例表缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteInstanceSphere) * kNaniteMaxTestInstances;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // CPU 每帧上传包围球
        m_InstanceSphereBuf = m_Device->CreateBuffer(d);
        if (!m_InstanceSphereBuf) { HE_CORE_ERROR("NaniteCull: 包围球缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32) * kNaniteMaxTestInstances;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // 启动时填哨兵 + dump 帧读回可见列表（每帧不再重置，见 ResetInstanceCullBuffers）
        m_VisibleInstanceBuf = m_Device->CreateBuffer(d);
        if (!m_VisibleInstanceBuf) { HE_CORE_ERROR("NaniteCull: 可见实例列表创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32);
        // TransferDst：每帧开头的"计数清零"是命令缓冲里的 4B 拷贝（见 RecordInstanceCullPass）。
        // 说明：`ToVkBufferUsage` 对**所有**缓冲都硬编码了 TRANSFER_DST，这里显式写出只为表达意图。
        d.usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst;
        d.cpuAccess = true;   // 启动时写 0 + dump 帧读回可见计数
        m_VisibleInstanceCountBuf = m_Device->CreateBuffer(d);
        if (!m_VisibleInstanceCountBuf) { HE_CORE_ERROR("NaniteCull: 可见实例计数缓冲创建失败"); return false; }
    }
    {
        // 【任务 15】可见实例掩码（Phase 1 → Phase 2 的接线；每线程写 0/1，**不需要清零**）
        rhi::BufferDesc d;
        d.size      = sizeof(u32) * kNaniteMaxTestInstances;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // 启动时填哨兵 + dump 帧可读回对照
        m_VisibleMaskBuf = m_Device->CreateBuffer(d);
        if (!m_VisibleMaskBuf) { HE_CORE_ERROR("NaniteCull: 可见实例掩码缓冲创建失败"); return false; }
    }
    {
        // 【任务 15】**唯一的清零源**（常驻 0）：任务 3/13/14/15 的所有每帧计数器都从这里拷 0；
        //   另外附一段 20KB 的 0xFF 源，用来在命令缓冲内重置任务 3 的间接命令缓冲（哨兵）。
        rhi::BufferDesc d;
        d.size      = kNaniteZeroSourceBytes + kNaniteIndirectSentinelBytes;
        d.usage     = rhi::BufferUsage::TransferSrc;   // 只被 GPU 当 TRANSFER_SRC 读
        d.cpuAccess = true;   // 只在 Initialize 写一次（0 与 0xFF）
        m_ClearZeroBuf = m_Device->CreateBuffer(d);
        if (!m_ClearZeroBuf) { HE_CORE_ERROR("NaniteCull: 清零/哨兵源缓冲创建失败"); return false; }
    }

    // ── 2. 描述符集：显式绑定三个 SSBO（不走 bindless，避免与 GBuffer 的 Flush 时序耦合）──
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 假簇（只读）
        { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 间接命令（读写）
        { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 命令条数（读写）
    };
    m_Layout = m_Device->CreateDescriptorSetLayout(layout);
    m_Set    = m_Device->AllocateDescriptorSet(m_Layout);
    m_Device->UpdateDescriptorSet(m_Set, 0, rhi::DescriptorType::StorageBuffer, m_FakeClusterBuf.get());
    m_Device->UpdateDescriptorSet(m_Set, 1, rhi::DescriptorType::StorageBuffer, m_IndirectCmdBuf.get());
    m_Device->UpdateDescriptorSet(m_Set, 2, rhi::DescriptorType::StorageBuffer, m_CountBuf.get());

    // ── 3. compute PSO ──
    m_CS.stage      = rhi::ShaderStage::Compute;
    m_CS.spirv      = k_Nanite_Cull_comp_spv;
    m_CS.entryPoint = "main";

    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskCompute;
    pc.size      = sizeof(NaniteCullParams);

    rhi::PipelineStateDesc desc;
    desc.computeShader        = &m_CS;
    desc.bindPoint            = rhi::PipelineBindPoint::Compute;
    desc.pushConstantRanges   = { pc };
    desc.descriptorSetLayouts = { m_Layout };
    desc.debugName            = "NaniteCull";
    m_PSO = m_Device->CreatePipelineState(desc);
    if (!m_PSO) { HE_CORE_ERROR("NaniteCull: compute PSO 创建失败"); return false; }

    // ── 3b. 【任务 13】实例剔除：5 个显式 SSBO 绑定 + compute PSO ──
    // 与任务 3 同样**不走 bindless**：模块私有缓冲、生命周期清晰，显式绑定最简单也最稳。
    {
        rhi::DescriptorSetLayoutDesc cullLayout;
        cullLayout.bindings = {
            { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 实例表（只读）
            { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 包围球（只读）
            { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 可见列表（读写）
            { 3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 可见计数（读写）
            { 4, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 【任务 15】可见掩码（读写）
        };
        m_InstanceCullLayout = m_Device->CreateDescriptorSetLayout(cullLayout);
        m_InstanceCullSet    = m_Device->AllocateDescriptorSet(m_InstanceCullLayout);
        m_Device->UpdateDescriptorSet(m_InstanceCullSet, 0, rhi::DescriptorType::StorageBuffer, m_InstanceBuf.get());
        m_Device->UpdateDescriptorSet(m_InstanceCullSet, 1, rhi::DescriptorType::StorageBuffer, m_InstanceSphereBuf.get());
        m_Device->UpdateDescriptorSet(m_InstanceCullSet, 2, rhi::DescriptorType::StorageBuffer, m_VisibleInstanceBuf.get());
        m_Device->UpdateDescriptorSet(m_InstanceCullSet, 3, rhi::DescriptorType::StorageBuffer, m_VisibleInstanceCountBuf.get());
        m_Device->UpdateDescriptorSet(m_InstanceCullSet, 4, rhi::DescriptorType::StorageBuffer, m_VisibleMaskBuf.get());

        m_InstanceCullCS.stage      = rhi::ShaderStage::Compute;
        m_InstanceCullCS.spirv      = k_Nanite_InstanceCull_comp_spv;
        m_InstanceCullCS.entryPoint = "main";

        rhi::PushConstantRange cullPc;
        cullPc.stageMask = rhi::kStageMaskCompute;
        cullPc.size      = sizeof(NaniteInstanceCullParams);   // 112B（6×float4 + 4×u32）

        rhi::PipelineStateDesc cullDesc;
        cullDesc.computeShader        = &m_InstanceCullCS;
        cullDesc.bindPoint            = rhi::PipelineBindPoint::Compute;
        cullDesc.pushConstantRanges   = { cullPc };
        cullDesc.descriptorSetLayouts = { m_InstanceCullLayout };
        cullDesc.debugName            = "NaniteInstanceCull";
        m_InstanceCullPSO = m_Device->CreatePipelineState(cullDesc);
        if (!m_InstanceCullPSO) { HE_CORE_ERROR("NaniteCull: 实例剔除 compute PSO 创建失败"); return false; }
    }

    // ── 1c. 【任务 14】cluster BVH 的自持缓冲（与任务 3/13 的链完全独立）──
    // 容量全部取编译期常量（`kNaniteMaxBVH*`）：可见簇引用表必须**一次分配、容量恒定**
    //（GPU 原子压缩写入无法中途扩容）。三个只读缓冲在 `SetClusterBVH` 里一次性上传。
    {
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteBVHNode) * kNaniteMaxBVHNodes;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // 只在 SetClusterBVH 写一次
        m_BVHNodeBuf = m_Device->CreateBuffer(d);
        if (!m_BVHNodeBuf) { HE_CORE_ERROR("NaniteCull: BVH 节点缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32) * kNaniteMaxBVHClusters;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;
        m_BVHLeafBuf = m_Device->CreateBuffer(d);
        if (!m_BVHLeafBuf) { HE_CORE_ERROR("NaniteCull: BVH 叶子簇表缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteClusterSphere) * kNaniteMaxBVHClusters;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;
        m_BVHSphereBuf = m_Device->CreateBuffer(d);
        if (!m_BVHSphereBuf) { HE_CORE_ERROR("NaniteCull: BVH 簇球缓冲创建失败"); return false; }
    }
    {
        // 【任务 15】每簇 LOD 元数据（16B/条；`SetClusterBVH` 一次性上传）
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteClusterLODInfo) * kNaniteMaxBVHClusters;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;
        m_LODInfoBuf = m_Device->CreateBuffer(d);
        if (!m_LODInfoBuf) { HE_CORE_ERROR("NaniteCull: LOD 元数据缓冲创建失败"); return false; }
    }
    {
        // 【任务 16】每簇绘制参数（16B/条；`SetClusterBVH` 一次性上传）
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteClusterDrawRange) * kNaniteMaxBVHClusters;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;
        m_ClusterDrawRangeBuf = m_Device->CreateBuffer(d);
        if (!m_ClusterDrawRangeBuf) { HE_CORE_ERROR("NaniteCull: 绘制参数缓冲创建失败"); return false; }
    }
    {
        // 【任务 16】间接绘制命令（20B/条）。**Indirect** usage 是硬要求：
        //   `vkCmdDrawIndexedIndirectCount` 把这块内存当 `VkDrawIndexedIndirectCommand` 数组读
        //   （缺 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT 会报 VUID-vkCmdDrawIndexedIndirectCount-
        //   buffer-00547）。cpuAccess 仅供 dump 帧逐条读回比对。
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteIndirectCommand) * kNaniteMaxIndirectDraws;
        d.usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect;
        d.cpuAccess = true;
        m_IndirectDrawBuf = m_Device->CreateBuffer(d);
        if (!m_IndirectDrawBuf) { HE_CORE_ERROR("NaniteCull: 间接绘制命令缓冲创建失败"); return false; }
    }
    {
        // 【任务 16】绘制计数（单个 u32）：GPU 只在"真的写了命令"时原子 +1 ⇒ 它的值恰好是
        //   命令缓冲里 [0, count) 的有效条数。Indirect 是 countBuffer 的要求，
        //   TransferDst 是每帧"命令缓冲内清零"拷贝的目标。
        rhi::BufferDesc d;
        d.size      = sizeof(u32);
        d.usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect
                    | rhi::BufferUsage::TransferDst;
        d.cpuAccess = true;
        m_DrawCountBuf = m_Device->CreateBuffer(d);
        if (!m_DrawCountBuf) { HE_CORE_ERROR("NaniteCull: 绘制计数缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteVisibleClusterRef) * kNaniteMaxVisibleClusterRefs;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // dump 帧读回可见簇列表（每帧不重置，理由同任务 13）
        m_VisibleClusterBuf = m_Device->CreateBuffer(d);
        if (!m_VisibleClusterBuf) { HE_CORE_ERROR("NaniteCull: 可见簇列表缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32);
        // TransferDst：每帧开头的"计数清零"是命令缓冲里的 4B 拷贝（见 RecordCullChainPass）
        d.usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst;
        d.cpuAccess = true;   // dump 帧读回
        m_VisibleClusterCountBuf = m_Device->CreateBuffer(d);
        if (!m_VisibleClusterCountBuf) { HE_CORE_ERROR("NaniteCull: 可见簇计数缓冲创建失败"); return false; }
    }
    {
        // 【任务 15】三阶段读数（扁平 u32；整块 64B ⇒ 一次 64B 拷贝清零）
        rhi::BufferDesc d;
        d.size      = kNaniteCullStatsBytes;
        d.usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst;
        d.cpuAccess = true;   // dump 帧读回（通过视锥/遮挡/级直方图/访问节点数）
        m_CullStatsBuf = m_Device->CreateBuffer(d);
        if (!m_CullStatsBuf) { HE_CORE_ERROR("NaniteCull: 三阶段读数缓冲创建失败"); return false; }
    }
    {
        // 【任务 15】三阶段参数（112B；CPU 每帧上传，与实例表同款的主机可见缓冲）
        rhi::BufferDesc d;
        d.size      = sizeof(NaniteCullChainParams);
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;
        m_ChainParamBuf = m_Device->CreateBuffer(d);
        if (!m_ChainParamBuf) { HE_CORE_ERROR("NaniteCull: 三阶段参数缓冲创建失败"); return false; }
    }

    // ── 3c. 【任务 14/15/16】cluster BVH 三阶段遍历：14 个绑定 + compute PSO ──
    // 绑定顺序与 `Nanite_ClusterBVH.comp.slang` 的 [[vk::binding]] 逐条对应。
    {
        rhi::DescriptorSetLayoutDesc bvhLayout;
        bvhLayout.bindings = {
            { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 节点（只读）
            { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 叶子簇表（只读）
            { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 簇球（只读）
            { 3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 实例表（只读，复用任务 13 的）
            { 4, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 可见簇列表（读写）
            { 5, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 可见簇计数（读写）
            { 6, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 【任务 15】可见实例掩码（只读）
            { 7, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 【任务 15】LOD 元数据（只读）
            { 8, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 【任务 15】三阶段参数（只读）
            { 9, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 【任务 15】三阶段读数（读写）
            {10, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute, false },  // 【任务 15】Hi-Z 金字塔
            {11, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 【任务 16】绘制参数（只读）
            {12, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 【任务 16】间接命令（读写）
            {13, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 【任务 16】绘制计数（读写）
        };
        m_BVHLayout = m_Device->CreateDescriptorSetLayout(bvhLayout);
        m_BVHSet    = m_Device->AllocateDescriptorSet(m_BVHLayout);
        m_Device->UpdateDescriptorSet(m_BVHSet, 0, rhi::DescriptorType::StorageBuffer, m_BVHNodeBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 1, rhi::DescriptorType::StorageBuffer, m_BVHLeafBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 2, rhi::DescriptorType::StorageBuffer, m_BVHSphereBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 3, rhi::DescriptorType::StorageBuffer, m_InstanceBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 4, rhi::DescriptorType::StorageBuffer, m_VisibleClusterBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 5, rhi::DescriptorType::StorageBuffer, m_VisibleClusterCountBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 6, rhi::DescriptorType::StorageBuffer, m_VisibleMaskBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 7, rhi::DescriptorType::StorageBuffer, m_LODInfoBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 8, rhi::DescriptorType::StorageBuffer, m_ChainParamBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 9, rhi::DescriptorType::StorageBuffer, m_CullStatsBuf.get());
        // 【任务 16】可见簇 → 间接绘制参数的三条绑定
        m_Device->UpdateDescriptorSet(m_BVHSet, 11, rhi::DescriptorType::StorageBuffer,
                                      m_ClusterDrawRangeBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 12, rhi::DescriptorType::StorageBuffer,
                                      m_IndirectDrawBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 13, rhi::DescriptorType::StorageBuffer,
                                      m_DrawCountBuf.get());

        // 【任务 15】Hi-Z 采样器（本类自建；口径与既有 Hi-Z 采样器一致：点采样 + ClampToEdge
        //   + mip 0..8）。**为什么不借用 GPUCulling 的采样器**：§14.3 禁止模块 include
        //   `GPUCulling.h`，模块只接收一个**纹理**句柄 ⇒ 采样器必须自己持有，
        //   否则 binding 10 的生命周期会跨模块。层间过滤保持默认（整数 LOD 下等价于点选层；
        //   金字塔存的是"最小深度"，1 ULP 级的混合只会更保守）。
        {
            rhi::SamplerDesc sd;
            sd.minFilter = sd.magFilter = rhi::FilterMode::Nearest;
            sd.addressU  = sd.addressV = sd.addressW = rhi::AddressMode::ClampToEdge;
            sd.minLod    = 0.0f;
            sd.maxLod    = (float)kNaniteMaxHiZMips;
            m_HiZSampler = m_Device->CreateSampler(sd);
            if (!m_HiZSampler) { HE_CORE_ERROR("NaniteCull: Hi-Z 采样器创建失败"); return false; }
        }
        // 【任务 15】Hi-Z 占位纹理：binding 10 必须**永远**是合法描述符（Vulkan 不接受指向空纹理的
        //   采样器绑定）⇒ 没有外部金字塔时绑这张 1×1。层数传 0 ⇒ shader 第一句就返回"不遮挡"，
        //   因此它永远不会被真的采样（只为满足描述符合法性）。
        {
            rhi::TextureDesc td;
            td.format    = rhi::Format::R32_FLOAT;
            td.width     = 1u;
            td.height    = 1u;
            td.mipLevels = 1u;
            td.usage     = rhi::TextureUsage::ShaderResource;
            m_HiZPlaceholderTex = m_Device->CreateTexture(td);
            if (!m_HiZPlaceholderTex) { HE_CORE_ERROR("NaniteCull: Hi-Z 占位纹理创建失败"); return false; }
            m_Device->UpdateDescriptorSet(m_BVHSet, 10, rhi::DescriptorType::CombinedImageSampler,
                                          m_HiZPlaceholderTex.get(), m_HiZSampler.get());
        }

        m_BVHCS.stage      = rhi::ShaderStage::Compute;
        m_BVHCS.spirv      = k_Nanite_ClusterBVH_comp_spv;
        m_BVHCS.entryPoint = "main";

        rhi::PushConstantRange bvhPc;
        bvhPc.stageMask = rhi::kStageMaskCompute;
        bvhPc.size      = sizeof(NaniteClusterBVHParams);   // 112B（6×float4 + 4×u32）

        rhi::PipelineStateDesc bvhDesc;
        bvhDesc.computeShader        = &m_BVHCS;
        bvhDesc.bindPoint            = rhi::PipelineBindPoint::Compute;
        bvhDesc.pushConstantRanges   = { bvhPc };
        bvhDesc.descriptorSetLayouts = { m_BVHLayout };
        bvhDesc.debugName            = "NaniteClusterBVH";
        m_BVHPSO = m_Device->CreatePipelineState(bvhDesc);
        if (!m_BVHPSO) { HE_CORE_ERROR("NaniteCull: cluster BVH compute PSO 创建失败"); return false; }
    }

    // ── 3d. 【任务 15】模块自持的 Hi-Z 金字塔构建：布局 + **每目标 mip 一个描述符集** + PSO ──
    // 【为什么每个目标 mip 一个集合】见头文件；一句话：本引擎的 GPU 在**执行期**读描述符，
    //   同一个集合在一段命令缓冲里被多次更新时"最后一次主机写"对全部派发生效 ⇒ 逐 mip 复用
    //   一个集合的写法会让所有派发写同一个 mip（既有 `BuildHiZPyramid` 就是这样失效的）。
    {
        rhi::DescriptorSetLayoutDesc hizLayout;
        hizLayout.bindings = {
            { 0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute, false },  // 源：深度（srcMip==0）
            { 1, rhi::DescriptorType::StorageImage,         1, rhi::kStageMaskCompute, false },  // 源：上一级金字塔（srcMip>0）
            { 2, rhi::DescriptorType::StorageImage,         1, rhi::kStageMaskCompute, false },  // 目标：下一级金字塔
        };
        m_HiZBuildLayout = m_Device->CreateDescriptorSetLayout(hizLayout);
        if (m_HiZBuildLayout == rhi::kInvalidLayout) {
            HE_CORE_ERROR("NaniteCull: Hi-Z 构建描述符布局创建失败"); return false;
        }
        for (u32 level = 0; level < kNaniteHiZBuildMaxViews; ++level) {
            m_HiZBuildSets[level] = m_Device->AllocateDescriptorSet(m_HiZBuildLayout);
            if (m_HiZBuildSets[level] == rhi::kInvalidSet) {
                HE_CORE_ERROR("NaniteCull: Hi-Z 构建描述符集分配失败（第 {} 级；每目标 mip 一个）", level);
                return false;
            }
        }

        m_HiZBuildCS.stage      = rhi::ShaderStage::Compute;
        m_HiZBuildCS.spirv      = k_Nanite_HiZDownsample_comp_spv;
        m_HiZBuildCS.entryPoint = "main";

        rhi::PushConstantRange hizPc;
        hizPc.stageMask = rhi::kStageMaskCompute;
        hizPc.size      = sizeof(NaniteHiZDownsampleParams);   // 24B（uint2 srcSize + uint2 dstSize + srcMip + pad）

        rhi::PipelineStateDesc hizDesc;
        hizDesc.computeShader        = &m_HiZBuildCS;
        hizDesc.bindPoint            = rhi::PipelineBindPoint::Compute;
        hizDesc.pushConstantRanges   = { hizPc };
        hizDesc.descriptorSetLayouts = { m_HiZBuildLayout };
        hizDesc.debugName            = "NaniteHiZDownsample";
        m_HiZBuildPSO = m_Device->CreatePipelineState(hizDesc);
        if (!m_HiZBuildPSO) { HE_CORE_ERROR("NaniteCull: Hi-Z 下采样 compute PSO 创建失败"); return false; }
    }

    // 初始清零（**只在启动时**：此时还没有任何已提交的 GPU 工作 ⇒ 主机写安全）
    ResetFrameBuffers();
    UploadFakeClusters();
    ResetInstanceCullBuffers();

    // 【任务 15】清零源恒为 0 + 哨兵源恒为 0xFF（只在这里写一次，之后只被 GPU 当 TRANSFER_SRC 读）
    if (void* p = m_ClearZeroBuf ? m_ClearZeroBuf->Map() : nullptr) {
        auto* bytes = static_cast<u8*>(p);
        std::memset(bytes, 0x00, (size_t)kNaniteZeroSourceBytes);                          // 计数器清零源
        std::memset(bytes + kNaniteZeroSourceBytes, 0xFF, (size_t)kNaniteIndirectSentinelBytes);  // 命令哨兵源
        m_ClearZeroBuf->Unmap();
    }

    HE_CORE_INFO("NaniteCull: 初始化完成（计数→间接绘制链；容量 {} 簇，当前 {} 簇）",
                 kNaniteMaxFakeClusters, m_FakeClusterCount);
    HE_CORE_INFO("NaniteCull: 实例剔除通道就绪（视锥 → 可见实例列表 + 可见性掩码；容量 {} 实例）",
                 kNaniteMaxTestInstances);
    HE_CORE_INFO("NaniteCull: cluster BVH 通道就绪（容量 {} 节点 / {} 簇 / {} 实例域 / {} 条可见引用；"
                 "叶子容量 {}，深度上限 {}，显式栈 {}）",
                 kNaniteMaxBVHNodes, kNaniteMaxBVHClusters, kNaniteMaxBVHInstances,
                 kNaniteMaxVisibleClusterRefs, kNaniteBVHLeafCapacity, kNaniteBVHMaxDepth,
                 kNaniteBVHMaxStackDepth);
    HE_CORE_INFO("NaniteCull: 三阶段剔除就绪（Phase 1 掩码 → Phase 2 视锥+Hi-Z → Phase 3 LOD 选择；"
                 "Hi-Z 层数上限 {}，可采样层下限 {}，LOD 阈值 {} 像素，直方图 {} 级）",
                 kNaniteMaxHiZMips, kNaniteHiZMinMip, kNaniteLODThresholdPixels, kNaniteLODHistogramLevels);
    // 【任务 16】可见簇 → 间接绘制参数的容量（命令缓冲与可见簇引用**同容量** ⇒ 槽位一一对应）
    HE_CORE_INFO("NaniteCull: 可见簇 → 间接绘制参数就绪（每簇 {}B 参数表 + {} 条命令（{}B/条）"
                 "+ 绘制计数；命令与可见簇引用同槽位、同容量）",
                 (u32)sizeof(NaniteClusterDrawRange), kNaniteMaxIndirectDraws,
                 (u32)sizeof(NaniteIndirectCommand));
    return true;
}

void NaniteCull::Shutdown() {
    m_PSO.reset();
    m_InstanceCullPSO.reset();
    m_BVHPSO.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_Layout);
    if (m_Device && m_InstanceCullLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_InstanceCullLayout);
    if (m_Device && m_BVHLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_BVHLayout);
    if (m_Device && m_HiZBuildLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_HiZBuildLayout);

    m_FakeClusterBuf.reset();
    m_IndirectCmdBuf.reset();
    m_CountBuf.reset();
    m_RasterCountBuf.reset();

    // 【任务 13】实例剔除资源一并释放
    m_InstanceBuf.reset();
    m_InstanceSphereBuf.reset();
    m_VisibleInstanceBuf.reset();
    m_VisibleInstanceCountBuf.reset();
    m_VisibleMaskBuf.reset();
    m_TestInstances.clear();
    m_TestSpheres.clear();
    m_CpuVisibleInstances.clear();
    m_CpuVisibleMask.clear();
    m_TestInstanceCount = 0u;

    // 【任务 14/15】cluster BVH 资源与状态一并释放（`m_BVHData` 的容器也清空，避免留下悬空语义）
    m_BVHNodeBuf.reset();
    m_BVHLeafBuf.reset();
    m_BVHSphereBuf.reset();
    m_LODInfoBuf.reset();
    m_ClusterDrawRangeBuf.reset();     // 【任务 16】每簇绘制参数
    m_IndirectDrawBuf.reset();         // 【任务 16】间接绘制命令
    m_DrawCountBuf.reset();            // 【任务 16】绘制计数
    m_VisibleClusterBuf.reset();
    m_VisibleClusterCountBuf.reset();
    m_CullStatsBuf.reset();
    m_ChainParamBuf.reset();
    m_ClearZeroBuf.reset();
    // 【任务 15】Hi-Z 构建的视图是**借用**金字塔纹理建的，必须显式销毁（否则纹理换尺寸后视图泄漏）
    for (u32 i = 0; i < kNaniteHiZBuildMaxViews; ++i) {
        if (m_HiZDestViews[i] != nullptr) {
            if (m_Device) m_Device->DestroyTextureMipView(m_HiZDestViews[i]);
            m_HiZDestViews[i] = nullptr;
        }
        m_HiZBuildSets[i] = rhi::kInvalidSet;
    }
    m_HiZViewOwner = nullptr;
    m_HiZViewCount = 0u;
    m_HiZBuildPSO.reset();
    m_HiZPlaceholderTex.reset();
    m_HiZSampler.reset();
    m_BVHData = NaniteClusterBVH{};
    m_LODInfo.clear();
    m_ClusterDrawRanges.clear();       // 【任务 16】绘制参数的 CPU 镜像
    m_FrameDrawCapacity = kNaniteMaxIndirectDraws;
    m_BVHReady = false;
    m_BVHInstanceDomain = 0u;
    m_FrameHiZRequested = false;
    m_FrameHiZTextureBound = false;
    m_FrameHiZMipCount = 0u;

    m_Layout = rhi::kInvalidLayout;
    m_Set    = rhi::kInvalidSet;
    m_InstanceCullLayout = rhi::kInvalidLayout;
    m_InstanceCullSet    = rhi::kInvalidSet;
    m_BVHLayout          = rhi::kInvalidLayout;
    m_BVHSet             = rhi::kInvalidSet;
    m_HiZBuildLayout     = rhi::kInvalidLayout;
    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
}

void NaniteCull::OnResize(u32 width, u32 height) {
    // 本条链（假簇/命令/计数）与视口无关；Hi-Z 金字塔随视口变化是后续任务的事，
    // 这里只记录尺寸保持骨架语义一致。
    m_Width  = width;
    m_Height = height;
}

void NaniteCull::SetFakeClusterCount(u32 count) {
    m_FakeClusterCount = std::min(count, kNaniteMaxFakeClusters);
}

void NaniteCull::ResetFrameBuffers() {
    // 【任务 15：本函数现在**只在 `Initialize` 里调一次**（启动初值），不再每帧调用】
    // 每帧的重置全部改成命令缓冲内的拷贝（见 `RecordCullPass` 里的负向验证与三处 `CopyBuffer`）：
    // 录制期的主机写与派发之间没有排序，引擎允许多帧在飞 ⇒ 第 N+1 帧的 0 会落在第 N 帧派发之前，
    // 两帧原子累加叠加（任务 13 实测恰为 2 倍）。这里保留主机写只是因为"启动时还没有任何已提交的
    // GPU 工作"，主机写是安全的。
    // 计数清零：GPU 的 InterlockedAdd 从 0 开始，最终值 = 实际写入的命令条数
    if (void* p = m_CountBuf ? m_CountBuf->Map() : nullptr) {
        *static_cast<u32*>(p) = 0u;
        m_CountBuf->Unmap();
    }
    // 光栅化簇计数清零（绘制端每帧重新累加）
    if (void* p = m_RasterCountBuf ? m_RasterCountBuf->Map() : nullptr) {
        *static_cast<u32*>(p) = 0u;
        m_RasterCountBuf->Unmap();
    }
    // 间接命令缓冲填哨兵：读回时"非哨兵且字段合法"的条目数即 GPU 真正写过的命令数
    if (void* p = m_IndirectCmdBuf ? m_IndirectCmdBuf->Map() : nullptr) {
        std::memset(p, 0xFF, sizeof(NaniteIndirectCommand) * kNaniteMaxFakeClusters);
        m_IndirectCmdBuf->Unmap();
    }
}

void NaniteCull::UploadFakeClusters() {
    void* p = m_FakeClusterBuf ? m_FakeClusterBuf->Map() : nullptr;
    if (!p) return;
    auto* clusters = static_cast<NaniteFakeCluster*>(p);
    for (u32 i = 0; i < m_FakeClusterCount; ++i) {
        clusters[i].clusterId     = i;      // 簇号 = 顺序编号
        clusters[i].instanceId    = 0u;     // 任务 3：只有 1 个实例
        clusters[i].triangleCount = 1u;     // 每簇 1 个三角形
        clusters[i]._pad          = 0u;
    }
    m_FakeClusterBuf->Unmap();
}

void NaniteCull::RecordCullPass(rhi::IRHICommandList* cmd) {
    // 【任务 15：每帧重置改为命令缓冲内的拷贝】过去这里是 `ResetFrameBuffers()`（主机写）——
    // 那是**与任务 13 同一处竞态**：主机写与派发之间没有排序，而引擎允许多帧在飞、CPU 领先 GPU
    // ⇒ 第 N+1 帧写的 0 会落在第 N 帧派发**之前**，两次派发的原子累加叠加到同一个计数上
    // （任务 13 实测读回恰为 CPU 参考的 2 倍）。本次把任务 3 的三处一起修掉：
    //   ① 命令条数计数 → 清零源 4B 拷贝；
    //   ② 光栅化簇计数 → 清零源 4B 拷贝；
    //   ③ 间接命令缓冲的哨兵填充 → 20KB 的 0xFF 源缓冲整块拷贝（它同样只影响读回读数，
    //      但同样会被"后一帧的主机写"抹掉前一帧 GPU 刚写的命令 ⇒ 一并进命令缓冲）。
    // 【只保留一处主机写】下面的 `UploadFakeClusters()`：假簇输入表的内容**每帧逐位相同**，
    //   竞争最坏只是把同一批字节再写一遍 ⇒ 无害（已注明）。
    UploadFakeClusters();

    if (!cmd || !m_ClearZeroBuf) return;   // 防御：Initialize 未成功时不发拷贝/屏障

    // ① 三个每帧重置（全部在命令缓冲内；源缓冲在 Initialize 里写好 0 / 0xFF）
    cmd->CopyBuffer(m_ClearZeroBuf.get(), m_CountBuf.get(),
                    sizeof(u32), kNaniteZeroSlotFakeCount, 0u);
    cmd->CopyBuffer(m_ClearZeroBuf.get(), m_RasterCountBuf.get(),
                    sizeof(u32), kNaniteZeroSlotFakeRaster, 0u);
    cmd->CopyBuffer(m_ClearZeroBuf.get(), m_IndirectCmdBuf.get(),
                    kNaniteIndirectSentinelBytes,
                    kNaniteZeroSourceBytes,   // 0xFF 段紧跟在 0 段之后
                    0u);
    // ② 传输写 → 计算/间接读：真实内存屏障（`PipelineBarrier` 无资源重载发的是 VkMemoryBarrier）
    cmd->PipelineBarrier(rhi::PipelineStage::Transfer,
                         rhi::PipelineStage::ComputeShader | rhi::PipelineStage::DrawIndirect,
                         rhi::ResourceState::CopyDst,
                         rhi::ResourceState::UnorderedAccess | rhi::ResourceState::IndirectArgument);

    if (!m_PSO) return;
    if (m_FakeClusterCount == 0) return;   // 0 簇 ⇒ 不派发，计数保持 0（绘制端也会画 0 条）

    NaniteCullParams pc{};
    pc.clusterCount          = m_FakeClusterCount;
    pc.vertexCountPerCluster = kNaniteFakeClusterIndexCount;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    char label[64];
    snprintf(label, sizeof(label), "Nanite_Cull (%u fake clusters)", m_FakeClusterCount);
    cmd->SetDrawDebugLabel(label);
    cmd->Dispatch((m_FakeClusterCount + 63u) / 64u, 1, 1);

    // 屏障：compute 写的间接命令 + 计数 → 之后 `DrawIndexedIndirectCount` 的间接读取。
    //
    // 【为什么 dstStage 只给 DrawIndirect】RHI 的 `ToVkPipelineStageFlags` 目前没有把
    //   `PipelineStage::DrawIndirect` 映射到 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT，
    //   整组位映射为空时它会退化为 `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT`；同理
    //   `ResourceState::IndirectArgument` 会退化为 MEMORY_READ|MEMORY_WRITE。这个保守映射
    //   **覆盖**间接命令读取所需的同步面，因此这里无需改动 RHI 的映射表（从而不影响既有 pass）。
    // 【srcStage 带上 Transfer】上面那次哨兵拷贝写的是**同一个**间接命令缓冲 ⇒ 它的写也必须
    //   在间接读取前可见（拷贝先于派发，但两个阶段之间不能只用 compute 作为源阶段）。
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader | rhi::PipelineStage::Transfer,
                         rhi::PipelineStage::DrawIndirect,
                         rhi::ResourceState::UnorderedAccess | rhi::ResourceState::CopyDst,
                         rhi::ResourceState::IndirectArgument);
}

// ============================================================
// §14.8 任务 13：实例剔除（视锥 → 可见实例列表 + 计数）
//   + 任务 15：同一 pass 里再产出"可见性掩码"，作为 Phase 2 的实例域
//
// 数据流：`SetCullChainFrame`（每帧，帧图构建期）
//           → 提取 6 平面 + 生成合成实例表/包围球（世界空间）+ LOD/Hi-Z 参数
//         `RecordCullChainPass`（每帧，pass 执行期）
//           → Phase 1：命令缓冲内清计数 → 上传实例与球 → CPU 参考剔除 → Dispatch → 屏障
//           → Phase 2/3：Hi-Z 构建 → 遍历（掩码 → 视锥 → Hi-Z → LOD 选择）→ 屏障
//         `NaniteRenderer::LogCull3Readback`（dump 帧）
//           → GPU 读回（实例数 / 三阶段读数 / 可见簇集合）vs CPU 参考
// ============================================================

void NaniteCull::SetCullChainFrame(const float4x4& viewProj, const float3& cameraPosition,
                                   u32 screenWidth, u32 screenHeight, float fovYDegrees,
                                   u32 testInstanceCount) {
    m_TestInstanceCount = std::min(testInstanceCount, kNaniteMaxTestInstances);
    m_FrameViewProj     = viewProj;
    m_FrameCameraPos    = cameraPosition;
    // `&viewProj[0][0]` 就是 glm::mat4 的列主序首地址（16 个 float）
    m_FrameFrustum      = NaniteExtractFrustumPlanes(&viewProj[0][0]);

    // 【任务 15】相位 3 的像素焦距 + 相位 2 的 Hi-Z 选层所需的屏幕尺寸。
    // 【为什么焦距从 fov 算而不是反解 viewProj】(P×V) 的 (1,1) 元素被视图旋转污染，
    //   反解出来的焦距在相机有俯仰/偏航时是错的（`CameraData::GetProjMatrix` 的 m11 = 1/tan(fov/2)）。
    m_FrameScreenW     = screenWidth;
    m_FrameScreenH     = screenHeight;
    m_FrameFocalPixels = NaniteClusterLODFocalPixels((float)screenHeight, fovYDegrees);
    m_FrameLODThreshold = kNaniteLODThresholdPixels;   // 设计 §5.1 的 "threshold = 1 pixel"

    // view-proj 的 4 个行（row 优先；`rows[r*4+c] = viewProj[c][r]`）——与 GPU 参数缓冲同一份比特。
    // 【为什么按行拆开】Slang 的 `float4x4` 行/列主序依赖编译选项（任务 14 已为 localToWorld 踩过），
    //   拆成 4 个 float4 + 显式点积后，CPU 与 GPU 乘的是同一个表达式。
    {
        const float* m = &viewProj[0][0];
        for (u32 row = 0u; row < 4u; ++row) {
            for (u32 col = 0u; col < 4u; ++col) {
                m_FrameViewProjRows[row * 4u + col] = m[col * 4u + row];
            }
        }
    }
    BuildTestInstances();
}

void NaniteCull::BuildTestInstances() {
    m_TestInstances.clear();
    m_TestSpheres.clear();
    if (m_TestInstanceCount == 0u) return;

    m_TestInstances.resize(m_TestInstanceCount);
    m_TestSpheres.resize(m_TestInstanceCount);

    // 反投影：NDC（view-projection 空间）→ 世界空间。
    // 用逆矩阵而不是硬编码世界坐标：相机移动时样本集合依然覆盖"里/外/跨越"三类。
    const glm::mat4 invViewProj = glm::inverse(m_FrameViewProj);
    const auto unproject = [&invViewProj](float ndcX, float ndcY, float ndcZ) -> float3 {
        const float4 clip(ndcX, ndcY, ndcZ, 1.0f);
        const float4 world = invViewProj * clip;
        float3 point(world);
        if (std::fabs(world.w) > 1.0e-6f) point /= world.w;
        return point;
    };

    // 网格分辨率：尽量接近方阵（64 → 8×8）
    const u32 gridX = (u32)std::ceil(std::sqrt((float)m_TestInstanceCount));
    const u32 gridY = (m_TestInstanceCount + gridX - 1u) / gridX;

    for (u32 i = 0; i < m_TestInstanceCount; ++i) {
        const u32 ix = i % gridX;
        const u32 iy = i / gridX;
        float ndcX = (gridX > 1u) ? (2.0f * (float)ix / (float)(gridX - 1u) - 1.0f) : 0.0f;
        float ndcY = (gridY > 1u) ? (2.0f * (float)iy / (float)(gridY - 1u) - 1.0f) : 0.0f;
        // 整体外扩 1.25：边缘行/列必然落到视锥外（样本集合因此天然含"外"这一类）
        ndcX *= 1.25f;
        ndcY *= 1.25f;
        // 5 层深度，落在开区间 (0,1) 内 ⇒ 不贴近平/远平面（避免边界浮点判定的争议）
        const float ndcZ = 0.2f + 0.6f * (float)(i % 5u) / 4.0f;

        float3 center      = unproject(ndcX, ndcY, ndcZ);
        float radiusScale  = kNaniteTestInstanceRadiusScale;

        // 三个"故意样本"：方向/大小都远离数值边界，保证 CPU 与 GPU 的判据不会因末位差异翻转。
        //   0 号：远在视锥外（NDC 2.5）+ 极小半径 ⇒ 不可见
        //   1 号：球心恰在右平面（NDC x = 1.0）+ 较大半径 ⇒ **跨越平面** ⇒ 可见
        //   2 号：视锥外（NDC -1.6）+ 极小半径 ⇒ 不可见
        if (i == 0u)      { center = unproject(2.5f, 2.5f, ndcZ);  radiusScale = 0.02f; }
        else if (i == 1u) { center = unproject(1.0f, 0.0f, ndcZ);  radiusScale = 0.25f; }
        else if (i == 2u) { center = unproject(-1.6f, 0.0f, ndcZ); radiusScale = 0.02f; }

        const float distance = glm::length(center - m_FrameCameraPos);
        const float radius   = radiusScale * distance + 0.01f;

        NaniteInstanceGpuObject& instance = m_TestInstances[i];
        instance = NaniteInstanceGpuObject{};   // 未用字段保持确定（全 0），避免未初始化读
        const glm::mat4 translation = glm::translate(glm::mat4(1.0f), center);
        std::memcpy(instance.localToWorld, &translation[0][0], sizeof(instance.localToWorld));
        for (u32 axis = 0; axis < 3u; ++axis) {
            instance.boundsMin[axis] = center[axis] - radius;
            instance.boundsMax[axis] = center[axis] + radius;
        }
        instance.boundsMin[3]   = 0.0f;   // w 分量不用（与 GPUSceneObject 一致）
        instance.boundsMax[3]   = 0.0f;
        instance.meshIndex      = i;
        instance.materialIndex  = 0u;
        instance.objectID       = i;
        instance.visibilityFlags = 1u;
        // 3 号是"空实例"（indexCount = 0）：CPU 参考与 GPU 必须**同时**跳过它
        instance.indexCount     = (i == 3u) ? 0u : 36u;
        instance.firstIndex     = 0u;
        instance.vertexOffset   = 0;
        instance._pad           = 0u;

        // 球从 128B 契约的 boundsMin/boundsMax 推出 ⇒ "包围球来自 GPUSceneObject 契约"
        m_TestSpheres[i] = NaniteSphereFromInstanceBounds(instance);
    }
}

void NaniteCull::ResetInstanceCullBuffers() {
    // 【只在 `Initialize` 调一次：这是**启动时的初值**，不是每帧的清零】
    // 每帧的清零必须走命令缓冲（见 `RecordInstanceCullPass` 里的 4B 拷贝），原因见那里的注释。
    // 可见计数清零：GPU 的 InterlockedAdd 从 0 开始，最终值 = 实际写入的可见实例数
    if (void* p = m_VisibleInstanceCountBuf ? m_VisibleInstanceCountBuf->Map() : nullptr) {
        *static_cast<u32*>(p) = 0u;
        m_VisibleInstanceCountBuf->Unmap();
    }
    // 可见列表填哨兵：让"未写过的槽位"在调试时一眼可辨。
    // 【注意】每帧**不再**重置列表：读回只取 `[0, 计数)`，而这些槽位必定由**同一次派发**写入
    //   （计数与列表写在同一个着色器里），因此列表不需要逐帧清 —— 这同时消掉了"主机 memset
    //   与 GPU 派发竞争"的隐患（清早了/清晚了都会让读回读到被抹掉的槽位）。
    if (void* p = m_VisibleInstanceBuf ? m_VisibleInstanceBuf->Map() : nullptr) {
        std::memset(p, 0xFF, sizeof(u32) * kNaniteMaxTestInstances);
        m_VisibleInstanceBuf->Unmap();
    }
    // 【任务 15】可见性掩码填哨兵（只在启动时；每帧 Phase 1 会对**每个**实例显式写 0/1 ⇒ 无需清零）
    if (void* p = m_VisibleMaskBuf ? m_VisibleMaskBuf->Map() : nullptr) {
        std::memset(p, 0xFF, sizeof(u32) * kNaniteMaxTestInstances);
        m_VisibleMaskBuf->Unmap();
    }
}

void NaniteCull::UploadInstanceCullInputs() {
    if (m_TestInstanceCount > 0u) {
        if (void* p = m_InstanceBuf ? m_InstanceBuf->Map() : nullptr) {
            std::memcpy(p, m_TestInstances.data(),
                        sizeof(NaniteInstanceGpuObject) * m_TestInstanceCount);
            m_InstanceBuf->Unmap();
        }
        if (void* p = m_InstanceSphereBuf ? m_InstanceSphereBuf->Map() : nullptr) {
            std::memcpy(p, m_TestSpheres.data(),
                        sizeof(NaniteInstanceSphere) * m_TestInstanceCount);
            m_InstanceSphereBuf->Unmap();
        }
    }
}

void NaniteCull::RecordInstanceCullPass(rhi::IRHICommandList* cmd) {
    // ── 每帧上传合成实例表与包围球 ──
    // 【口径与任务 3 的假簇上传一致】主机可见缓冲 + `Map/memcpy/Unmap`。
    // 【如实记录的残留风险】这份上传是**主机写**，引擎允许 2 帧在飞（CPU 领先 GPU），
    //   因此理论上可能出现"GPU 还在读第 N 帧的实例表、CPU 已经写入第 N+1 帧"的交错。
    //   本任务的验收读回在同一帧内比较（dump 帧：CPU 参考 = 第 N 帧，GPU 结果 = 第 N 帧派发），
    //   且合成实例表在相机静止时逐位相同，故实测逐项一致；真正干净的修法是把上传也做进
    //   随帧轮转的暂存环（属引擎 TransientAllocator 的范畴），不在本任务改动面内。
    UploadInstanceCullInputs();

    // ── CPU 参考剔除：与 GPU **同一份输入**（同样的平面、同样的实例表与球、同样的跳过规则）──
    m_CpuVisibleInstances.resize(m_TestInstanceCount);
    const u32 cpuVisible = NaniteCullInstancesCPU(
        m_FrameFrustum,
        m_TestInstances.empty() ? nullptr : m_TestInstances.data(),
        m_TestSpheres.empty()   ? nullptr : m_TestSpheres.data(),
        m_TestInstanceCount,
        m_CpuVisibleInstances.empty() ? nullptr : m_CpuVisibleInstances.data(),
        (u32)m_CpuVisibleInstances.size());
    m_CpuVisibleInstances.resize(cpuVisible);   // 紧凑到真实可见数（升序）

    // 【任务 15】把 CPU 参考的可见集合展开成与 GPU 同语义的**掩码**（Phase 2 的实例域）。
    // 【口径与 GPU 逐条一致】空实例写 0、视锥外写 0、可见写 1（GPU 侧对每个实例都显式写一次）。
    // 【为什么展开成掩码而不是直接用升序列表】遍历的实例域是 `[0, min(实例数, 64))`，掩码按实例下标
    //   寻址 ⇒ 钳制后的子集与 GPU 完全一致；压缩列表的槽位顺序由 GPU 原子决定，"取前 k 个"在可见数
    //   超过上限时是不确定的子集（会让逐项比较失去意义）。
    m_CpuVisibleMask.assign(m_TestInstanceCount, 0u);
    for (u32 i = 0u; i < cpuVisible; ++i) {
        const u32 index = m_CpuVisibleInstances[i];
        if (index < m_CpuVisibleMask.size()) m_CpuVisibleMask[index] = 1u;
    }

    if (!cmd || !m_InstanceCullPSO) return;

    // ── 【本任务最关键的一处同步】可见计数的"每帧清零"必须**在命令缓冲内**完成（GPU 有序）──
    //
    // 【负向验证（实测，2026-09-20）】最初照任务 3 假簇链的写法在录制期用 `Map/Unmap` 主机写 0，
    //   读回**恰好是 CPU 参考的 2 倍**：`nanite_instance_test_count=8` → gpu=10/cpu=5；
    //   `=64` → gpu=122/cpu=61；且可见列表里同一批下标连续出现两次（`1,4,5,6,7,1,4,5,6,7`）。
    //   【根因（已用两次对照实验钉死）】主机写与派发之间没有排序，而引擎允许若干帧在飞、
    //   CPU 领先 GPU ⇒ 第 N+1 帧录制期写下的 0 会落在**第 N 帧派发执行之前**，于是第 N、N+1
    //   两次派发的原子累加叠加到同一个计数上。
    //     · 对照 A：把 `Map/Unmap` 主机写换成同一位置的一次设备 `WaitIdle()` 后再写 ⇒ 立刻恢复
    //       `gpu=5/cpu=5/mismatch=0`，证明竞争确实出在"主机写 vs 派发"的排序上。
    //     · 对照 B：把清零改成命令缓冲里的 4B 拷贝（本实现）⇒ 同样 `gpu==cpu`，且不引入停顿。
    //   【任务 15 的收尾】假簇链的三个每帧主机写（命令计数 / 光栅化簇计数 / 哨兵填充）已按同一
    //   修法改掉（见 `RecordCullPass`）；本文件里剩下的每帧主机写只有"内容逐帧相同的输入表"。
    // 【为什么用拷贝而不是 `vkCmdFillBuffer`】RHI 目前只暴露 `CopyBuffer`（任务 12 已用它做上传），
    //   不为本任务扩 RHI 面；4B 拷贝的源缓冲常驻 0，语义与 fill 等价。
    cmd->CopyBuffer(m_ClearZeroBuf.get(), m_VisibleInstanceCountBuf.get(),
                    sizeof(u32), kNaniteZeroSlotVisibleInstances, 0u);
    // 拷贝写发生在 Transfer 阶段：到计算着色器读之间补一次**真实内存屏障**
    //（`PipelineBarrier` 无资源重载发的是 `VkMemoryBarrier`，不是仅执行依赖）
    cmd->PipelineBarrier(rhi::PipelineStage::Transfer,
                         rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::CopyDst,
                         rhi::ResourceState::UnorderedAccess | rhi::ResourceState::ShaderResource);

    if (m_TestInstanceCount == 0u) return;   // 0 实例 ⇒ 不派发；上面已把计数清 0，读回 0 与 CPU 参考 0 一致

    // push constant：6 个平面（由本帧 viewProj 提取）+ 实例数
    m_InstanceCullParams = NaniteInstanceCullParams{};
    std::memcpy(m_InstanceCullParams.planes, m_FrameFrustum.planes, sizeof(m_FrameFrustum.planes));
    m_InstanceCullParams.instanceCount = m_TestInstanceCount;

    cmd->SetPipeline(m_InstanceCullPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_InstanceCullSet);
    cmd->SetPushConstants(0, sizeof(m_InstanceCullParams), &m_InstanceCullParams);

    char label[64];
    snprintf(label, sizeof(label), "Nanite_InstanceCull (%u instances)", m_TestInstanceCount);
    cmd->SetDrawDebugLabel(label);
    cmd->Dispatch((m_TestInstanceCount + 63u) / 64u, 1, 1);

    // 屏障：compute 写的可见列表/计数 → 后续 compute 读（任务 14 的按实例簇剔除会消费它）。
    // 本任务还没有同帧消费者，但先把同步面写对，避免任务 14 接入时出现难查的顺序依赖。
    // 【dstStage 多带一个 Transfer】同时为**下一帧开头那次清零拷贝**消掉 WAR：
    //   计数会被下一帧的 `vkCmdCopyBuffer` 覆盖，必须让本帧派发的读/写在拷贝前可见。
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader,
                         rhi::PipelineStage::ComputeShader | rhi::PipelineStage::Transfer,
                         rhi::ResourceState::UnorderedAccess,
                         rhi::ResourceState::ShaderResource | rhi::ResourceState::CopyDst);
}

// ============================================================
// §14.8 任务 14/15：per-instance cluster BVH（构建入库）+ 三阶段簇剔除（每帧）
//
// 数据流：`SetClusterBVH`（一次性，任务 12 的资产构建点）
//           → CPU 构建 BVH（`BuildNaniteClusterBVH`）+ LOD 元数据（`BuildNaniteClusterLODInfo`）
//             + 上传节点/叶子簇表/簇球/LOD 元数据四个只读缓冲
//         `SetCullChainFrame`（每帧）→ 更新视锥、合成实例表、像素焦距与屏幕尺寸（任务 13/15）
//         `RecordCullChainPass`（每帧，**单个帧图 pass**）
//           → Phase 1 实例剔除（写列表 + 掩码）→ Hi-Z 金字塔构建 → Phase 2/3 遍历 → 屏障
//         `RunCullChainCPUReference`（dump 帧）
//           → CPU 参考（同一棵 BVH、同一张球表、同一张 LOD 元数据、同一份实例表与视锥；
//             Hi-Z 在 CPU 侧恒关闭，理由见头文件）
//         `NaniteRenderer::LogCull3Readback`（dump 帧）
//           → GPU 读回（实例数 / 三阶段读数 / 可见簇集合）vs CPU 参考，逐项比较 + 差异解释
// ============================================================

bool NaniteCull::SetClusterBVH(std::span<const NaniteClusterRecord> clusters,
                               std::span<const u32>                 lodOffsets) {
    if (!m_Device || !m_BVHNodeBuf || !m_BVHLeafBuf || !m_BVHSphereBuf || !m_LODInfoBuf) {
        HE_CORE_ERROR("NaniteCull: cluster BVH 入库失败（设备或自持缓冲未就绪）");
        return false;
    }

    // ── ① 簇数钳制（超出容量时**如实告警**，不静默截断）──
    u32 clusterCount = (u32)clusters.size();
    if (clusterCount > kNaniteMaxBVHClusters) {
        HE_CORE_WARN("NaniteCull: 簇数 {} 超出 cluster BVH 容量 {}，只对前 {} 个簇建 BVH"
                     "（可见簇引用表按容量分配，超出的簇本轮不参与遍历）",
                     clusterCount, kNaniteMaxBVHClusters, kNaniteMaxBVHClusters);
        clusterCount = kNaniteMaxBVHClusters;
    }
    // LOD 段与簇表同源（`.nanite` 的同一份镜像）；被截断时同步裁掉越出部分，避免元数据错位。
    if (!lodOffsets.empty() && lodOffsets.back() > clusterCount) {
        HE_CORE_WARN("NaniteCull: LOD 段末尾偏移 {} 超出参与剔除的簇数 {} ⇒ 按簇数截断",
                     lodOffsets.back(), clusterCount);
    }

    // ── ② CPU 构建（RHI-free 的构建器；失败不改写出参）──
    NaniteClusterBVH built;
    if (!BuildNaniteClusterBVH(clusters.first(clusterCount), built)) {
        HE_CORE_ERROR("NaniteCull: cluster BVH 构建失败（簇数 {}）", clusterCount);
        return false;
    }
    // 【任务 15】LOD 元数据：簇记录 + LOD 段 → own/parent 误差 + LOD 级 + 根标志
    std::vector<NaniteClusterLODInfo> lodInfo;
    if (!BuildNaniteClusterLODInfo(clusters.first(clusterCount), lodOffsets, lodInfo)) {
        HE_CORE_ERROR("NaniteCull: LOD 元数据构建失败（簇数 {}，LOD 段 {} 级）",
                      clusterCount, (u32)lodOffsets.size());
        return false;
    }

    // ── ③ 一次性上传四个只读缓冲 ──
    // 【为什么这里是安全的主机写】本函数只在任务 12 的一次性启动路径上被调用，这些缓冲还从未
    //   被任何已提交的 GPU 工作引用 ⇒ 不存在"主机写 vs 派发"的竞争（与任务 13 的计数清零不同）。
    if (!built.nodes.empty()) {
        if (void* p = m_BVHNodeBuf->Map()) {
            std::memcpy(p, built.nodes.data(), sizeof(NaniteBVHNode) * built.nodes.size());
            m_BVHNodeBuf->Unmap();
        }
    }
    if (!built.leafClusterIndices.empty()) {
        if (void* p = m_BVHLeafBuf->Map()) {
            std::memcpy(p, built.leafClusterIndices.data(),
                        sizeof(u32) * built.leafClusterIndices.size());
            m_BVHLeafBuf->Unmap();
        }
    }
    if (!built.clusterSpheres.empty()) {
        if (void* p = m_BVHSphereBuf->Map()) {
            std::memcpy(p, built.clusterSpheres.data(),
                        sizeof(NaniteClusterSphere) * built.clusterSpheres.size());
            m_BVHSphereBuf->Unmap();
        }
    }
    if (!lodInfo.empty()) {
        if (void* p = m_LODInfoBuf->Map()) {
            std::memcpy(p, lodInfo.data(), sizeof(NaniteClusterLODInfo) * lodInfo.size());
            m_LODInfoBuf->Unmap();
        }
    }

    // ── ③b.【任务 16】每簇绘制参数（`triangleOffset/triangleCount/vertexOffset` → 间接命令字段）
    // 【为什么在 CPU 侧建表】与簇球表/ LOD 元数据同一个理由：GPU 只需三个 u32，紧凑表让访存
    //   步长小 4 倍；而且表由纯函数 `NaniteMakeClusterDrawRange` 从簇记录**逐位搬运** ⇒
    //   CPU 参考打包与 GPU 打包读的是**同一份比特**（不一致就一定是 bug，不是舍入差异）。
    std::vector<NaniteClusterDrawRange> drawRanges(clusterCount);
    for (u32 i = 0u; i < clusterCount; ++i) {
        drawRanges[i] = NaniteMakeClusterDrawRange(clusters[i]);
    }
    if (!drawRanges.empty()) {
        if (void* p = m_ClusterDrawRangeBuf->Map()) {
            std::memcpy(p, drawRanges.data(), sizeof(NaniteClusterDrawRange) * drawRanges.size());
            m_ClusterDrawRangeBuf->Unmap();
        }
    }

    m_BVHData   = std::move(built);
    m_LODInfo   = std::move(lodInfo);
    m_ClusterDrawRanges = std::move(drawRanges);
    m_BVHReady  = !m_BVHData.Empty();
    m_BVHInstanceDomain = 0u;   // 每帧在 RecordCullChainPass 里按本帧实例数重算

    // 【任务 15】级分布读数（入库时打一次，人工可核对；每帧的分布由 cull3 行给出）
    u32 levelCounts[kNaniteLODHistogramLevels] = { 0u };
    for (const NaniteClusterLODInfo& info : m_LODInfo) {
        const u32 level = (info.lodLevel < kNaniteLODHistogramLevels)
                        ? info.lodLevel : (kNaniteLODHistogramLevels - 1u);
        ++levelCounts[level];
    }
    HE_CORE_INFO("NaniteCull: cluster BVH 入库完成（簇 {} → 节点 {} / 叶子 {} / 深度 {} / "
                 "最大叶子簇数 {} / 栈上界 {}；叶子容量 {}，深度上限 {}）",
                 m_BVHData.clusterCount, (u32)m_BVHData.nodes.size(), m_BVHData.leafCount,
                 m_BVHData.depth, m_BVHData.maxLeafClusterCount, m_BVHData.maxStackDepthUpperBound,
                 kNaniteBVHLeafCapacity, kNaniteBVHMaxDepth);
    HE_CORE_INFO("NaniteCull: LOD 元数据入库完成（每簇 {}B；级分布 0..7 = {},{},{},{},{},{},{},{}；"
                 "LOD 段 {} 级）",
                 (u32)sizeof(NaniteClusterLODInfo),
                 levelCounts[0], levelCounts[1], levelCounts[2], levelCounts[3],
                 levelCounts[4], levelCounts[5], levelCounts[6], levelCounts[7],
                 (u32)lodOffsets.size());
    // 【任务 16】绘制参数的规模读数：`indexEnd` = 全部簇里最大的 `firstIndex + indexCount`
    //   ⇒ 光栅端的占位索引缓冲必须覆盖它（否则 IA 会越界读索引）。
    {
        u64 indexEnd = 0u;
        u64 maxIndexCount = 0u;
        for (const NaniteClusterDrawRange& r : m_ClusterDrawRanges) {
            const u64 end = (u64)r.firstIndex + (u64)r.indexCount;
            if (end > indexEnd) indexEnd = end;
            if (r.indexCount > maxIndexCount) maxIndexCount = r.indexCount;
        }
        HE_CORE_INFO("NaniteCull: 每簇绘制参数入库完成（{} 条 × {}B；最大 indexCount={}，"
                     "索引位置上界={}；光栅端占位索引缓冲必须覆盖该上界）",
                     (u32)m_ClusterDrawRanges.size(), (u32)sizeof(NaniteClusterDrawRange),
                     maxIndexCount, indexEnd);
    }
    return true;
}

NaniteClusterBVHTraversalStats NaniteCull::RunCullChainCPUReference(
        std::vector<NaniteVisibleClusterRef>& outVisible) const {
    outVisible.clear();
    if (!m_BVHReady) return NaniteClusterBVHTraversalStats{};

    // 容量 = 声明的常量（与 GPU 的 push constant `visibleCapacity` **同一个数**）；
    // 正常运行下 64 实例 × 8287 簇 = 53 万 < 105 万 ⇒ 永不截断（截断会让集合比较失去意义）。
    outVisible.resize(kNaniteMaxVisibleClusterRefs);

    // 【任务 15】三阶段输入：Phase 1 掩码（CPU 侧同一集合）、LOD 元数据、相机/焦距/阈值、
    //   以及 Hi-Z（**空采样器 ⇒ 关闭**：CPU 拿不到金字塔的逐 texel 内容，理由见头文件）。
    NaniteCullChainDesc chain;
    for (u32 i = 0u; i < 16u; ++i) chain.vpRows[i] = m_FrameViewProjRows[i];
    chain.visibleMask      = m_CpuVisibleMask.empty() ? nullptr : m_CpuVisibleMask.data();
    chain.lodInfo          = m_LODInfo.empty() ? nullptr : m_LODInfo.data();
    chain.cameraPos[0]     = m_FrameCameraPos[0];
    chain.cameraPos[1]     = m_FrameCameraPos[1];
    chain.cameraPos[2]     = m_FrameCameraPos[2];
    chain.focalPixels      = m_FrameFocalPixels;
    chain.lodThresholdPixels = m_FrameLODThreshold;
    chain.screenW          = (float)m_FrameScreenW;
    chain.screenH          = (float)m_FrameScreenH;
    chain.hizMipCount      = 0u;   // CPU 侧恒关闭 Hi-Z（与"Hi-Z 关闭档"同一口径）
    chain.hiz              = NaniteHiZSampler{};   // 空采样器

    NaniteClusterBVHTraversalStats stats{};
    const u32 written = NaniteTraverseClusterBVHCPU(
        m_FrameFrustum,
        m_BVHData.View(),
        m_TestInstances.empty() ? nullptr : m_TestInstances.data(),
        m_TestInstanceCount,
        kNaniteMaxBVHInstances,
        outVisible.empty() ? nullptr : outVisible.data(),
        (u32)outVisible.size(),
        &stats,
        chain);
    outVisible.resize(written);
    return stats;
}

// 【任务 15】把"CPU 参考可见、GPU 未见"的簇做一次选层分布统计（Hi-Z 两档差异的量化解释）。
// 口径与失败条件写在 `NaniteCull.h` 的同名声明里；这里只留与代码逐句对应的短注释。
u32 NaniteCull::CountOccludedClustersByMip(
        const std::vector<NaniteVisibleClusterRef>& cpuVisible,
        const std::vector<NaniteVisibleClusterRef>& gpuVisible,
        u32 outMipHistogram[kNaniteMaxHiZMips],
        u32* outProjectedOffscreen) const {
    if (outProjectedOffscreen != nullptr) *outProjectedOffscreen = 0u;
    if (outMipHistogram == nullptr || !m_BVHReady || m_FrameHiZMipCount < 2u) return 0u;

    const NaniteClusterBVHView view = m_BVHData.View();
    if (view.clusterSpheres == nullptr || view.nodeCount == 0u) return 0u;

    u32 missing = 0u;
    usize gi = 0u;
    for (usize ci = 0u; ci < cpuVisible.size(); ++ci) {
        const NaniteVisibleClusterRef& ref = cpuVisible[ci];
        // 归并跳过 GPU 列表里"比它小"的元素（两个列表都按 (instance, cluster) 升序）
        while (gi < gpuVisible.size()
               && (gpuVisible[gi].instance < ref.instance
                   || (gpuVisible[gi].instance == ref.instance
                       && gpuVisible[gi].cluster < ref.cluster))) {
            ++gi;
        }
        if (gi < gpuVisible.size() && gpuVisible[gi].instance == ref.instance
            && gpuVisible[gi].cluster == ref.cluster) {
            continue;   // 两边都有
        }
        ++missing;

        if (ref.cluster >= view.clusterCount || ref.instance >= m_TestInstances.size()) continue;
        // 世界球 = 实例平移列（列主序 localToWorld[12..14]）+ 网格空间球心、半径不变
        const NaniteInstanceGpuObject& instance = m_TestInstances[ref.instance];
        float center[3] = {
            view.clusterSpheres[ref.cluster].center[0] + instance.localToWorld[12],
            view.clusterSpheres[ref.cluster].center[1] + instance.localToWorld[13],
            view.clusterSpheres[ref.cluster].center[2] + instance.localToWorld[14],
        };
        float minUV[2], maxUV[2], nearest = 0.0f;
        if (!NaniteProjectSphereToScreen(m_FrameViewProjRows, center,
                                         view.clusterSpheres[ref.cluster].radius,
                                         minUV, maxUV, &nearest)) {
            if (outProjectedOffscreen != nullptr) ++(*outProjectedOffscreen);
            continue;
        }
        const float sizeX = (maxUV[0] - minUV[0]) * (float)m_FrameScreenW;
        const float sizeY = (maxUV[1] - minUV[1]) * (float)m_FrameScreenH;
        const u32 mip = NaniteHiZSelectMip(sizeX, sizeY, m_FrameHiZMipCount);
        if (mip < kNaniteMaxHiZMips) ++outMipHistogram[mip];
    }
    return missing;
}

// 【任务 15】按既有 Hi-Z 口径构建金字塔（正确性靠"每个目标 mip 一个专属描述符集"）。
// 口径与失败条件下的说明写在 `NaniteCull.h` 的同名声明里；这里只留与代码逐句对应的短注释。
u32 NaniteCull::BuildHiZPyramid(rhi::IRHICommandList* cmd, rhi::IRHITexture* pyramid,
                                rhi::IRHITexture* depth, u32 screenW, u32 screenH) {
    if (!cmd || !pyramid || !depth || !m_HiZBuildPSO) return 0u;
    const u32 mipCount = NaniteHiZPyramidMipCount(screenW, screenH);
    if (mipCount < 2u) return 0u;   // 没有可写的目标层 ⇒ 关闭（shader 的遮挡测试也要求 ≥ 2）

    EnsureHiZBuildViews(pyramid);
    const u32 writable = (m_HiZViewCount < mipCount - 1u) ? m_HiZViewCount : (mipCount - 1u);
    if (writable == 0u) return 0u;

    cmd->SetPipeline(m_HiZBuildPSO.get());
    u32 srcW = screenW;
    u32 srcH = screenH;
    for (u32 level = 0u; level < writable; ++level) {
        const u32 dstW = (srcW > 1u) ? (srcW >> 1u) : 1u;
        const u32 dstH = (srcH > 1u) ? (srcH >> 1u) : 1u;
        const rhi::DescriptorSetHandle set = m_HiZBuildSets[level];
        if (set == rhi::kInvalidSet) break;

        // ── 每个集合的每个绑定**每帧只写一次**（这是本实现正确的关键；见头注释）──
        m_Device->UpdateDescriptorSet(set, 0, rhi::DescriptorType::CombinedImageSampler,
                                      depth, m_HiZSampler.get());
        // srcMip == 0 时 binding 1 不被使用，但必须是**合法且布局正确**的描述符 ⇒ 绑一个金字塔
        // 层的存储视图（整图此刻处于 GENERAL，正是 StorageImage 期望的布局）。
        const u32 srcView = (level == 0u) ? 1u : level;             // 目标层索引 = level+1
        const u32 srcIndex = (srcView < kNaniteHiZBuildMaxViews) ? srcView : 1u;
        m_Device->UpdateDescriptorSetWithImageView(set, 1, rhi::DescriptorType::StorageImage,
                                                   m_HiZDestViews[srcIndex]);
        m_Device->UpdateDescriptorSetWithImageView(set, 2, rhi::DescriptorType::StorageImage,
                                                   m_HiZDestViews[level + 1u]);

        NaniteHiZDownsampleParams pc{};
        pc.srcW   = srcW;
        pc.srcH   = srcH;
        pc.dstW   = dstW;
        pc.dstH   = dstH;
        pc.srcMip = level;   // level == 0 ⇒ 源是深度纹理；否则源是金字塔的第 level 层
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, set);
        cmd->SetPushConstants(0, sizeof(pc), &pc);

        char label[64];
        snprintf(label, sizeof(label), "Nanite_HiZ mip%u", level + 1u);
        cmd->SetDrawDebugLabel(label);
        cmd->Dispatch((dstW + 15u) / 16u, (dstH + 15u) / 16u, 1u);

        srcW = dstW;
        srcH = dstH;
    }
    // 层数 = 写入层数 + 1（mip0 不存在/不采样）；构建与采样之间的布局转换由调用方负责
    return writable + 1u;
}

void NaniteCull::EnsureHiZBuildViews(rhi::IRHITexture* pyramid) {
    if (pyramid == m_HiZViewOwner && m_HiZViewCount > 0u) return;   // 已按这张纹理建好

    // 换纹理（窗口尺寸变化会重建金字塔）⇒ 先销毁旧视图，再按新纹理重建
    for (u32 i = 0; i < kNaniteHiZBuildMaxViews; ++i) {
        if (m_HiZDestViews[i] != nullptr) {
            if (m_Device) m_Device->DestroyTextureMipView(m_HiZDestViews[i]);
            m_HiZDestViews[i] = nullptr;
        }
    }
    m_HiZViewOwner = pyramid;
    m_HiZViewCount = 0u;
    if (!m_Device || !pyramid) return;

    // 只建 mip1..（mip0 从不被写入/采样；见 `kNaniteHiZMinMip`）
    const u32 mipLevels = pyramid->GetMipLevels();
    const u32 maxView = (mipLevels < kNaniteHiZBuildMaxViews) ? mipLevels : kNaniteHiZBuildMaxViews;
    for (u32 mip = 1u; mip < maxView; ++mip) {
        m_HiZDestViews[mip] = m_Device->CreateTextureMipStorageView(pyramid, mip);
        if (m_HiZDestViews[mip] == nullptr) break;   // 上面那层建不出来就不再往下（回退到更短的金字塔）
        ++m_HiZViewCount;
    }
}

void NaniteCull::RecordCullChainPass(rhi::IRHICommandList* cmd,
                                     rhi::IRHITexture* hizTexture, rhi::IRHITexture* depthTexture,
                                     bool enableOcclusion) {
    if (!cmd) return;

    // ── Phase 1：实例剔除（写可见列表 + 可见性掩码）──
    // 【为什么和 Phase 2/3 挤在同一个 pass 里】帧图对"两个零资源 pass"的顺序**无法表达**
    //   （`RenderGraph::TopologicalSort` 对 inDegree=0 的 pass 按 LIFO 出队 ⇒ 注册顺序 ≠ 执行顺序）。
    //   把两个派发录在**同一个 pass 体**内，顺序由命令缓冲里的 `PipelineBarrier` 显式给出 ——
    //   这比"声明一条帧图依赖"更强（不依赖任何图排序规则）。下游另有一重保险：Phase 2 只读掩码 /
    //   只写 [0, 计数)，任何时刻读到的 (计数, 列表) 对都来自同一次派发，不会撕裂。
    RecordInstanceCullPass(cmd);

    if (!m_BVHReady || !m_BVHPSO) return;

    // 本帧实例域：与 CPU 参考、与可见簇引用表的容量口径**同一个钳制**
    m_BVHInstanceDomain = (m_TestInstanceCount < kNaniteMaxBVHInstances)
                        ? m_TestInstanceCount : kNaniteMaxBVHInstances;

    // ── Hi-Z 金字塔（Phase 2 的遮挡测试输入）──
    // 【口径复用】金字塔**资源**是 `GPUCulling` 的那张 `R32_FLOAT` / ≤8 层纹理（`SetDepthTexture`
    //   创建），下采样口径也与既有 `Culling/HiZDownsample.comp.slang` 逐字相同（2×2 取最小深度，
    //   层 L 覆盖 2^L×2^L 足迹，mip0 不写）。模块只**借用**纹理、不持有它。
    // 【为什么构建也由模块自己做】既有 `GPUCulling::BuildHiZPyramid` 逐 mip 更新同一个描述符集，
    //   而本引擎的 GPU 在**执行期**读取描述符、最后一次主机写对整段命令缓冲生效 ⇒ 实测金字塔全 0
    //   （详见 `NaniteRenderer.h` 的 `NaniteHiZSource` 注释与任务 15 实施记录）。
    // 【布局处理（本 pass 自己做，不通过帧图声明）】
    //   ① 整图（含全部 mip）转到 `UnorderedAccess`：金字塔是用存储图像逐层写出来的。
    //      **srcState 用 `Undefined`**：内容每帧整块重写，且第一帧这张纹理的真实布局就是 UNDEFINED
    //      （刚创建、从未转换）；声明 SHADER_READ_ONLY 会与真实布局不符（Vulkan 下 oldLayout 必须与
    //      实际一致）。引擎的布局追踪器若已有记录会用真实布局纠正 oldLayout。
    //   ② 逐层下采样（每个目标层用专属描述符集）。
    //   ③ 整图转回 `ShaderResource`：Phase 2 要**采样**它，这一步同时是采样前的内存屏障。
    //   【为什么不把 Hi-Z 纹理 import 进帧图】既有 `HiZ_Build`（SSR 档）也会写同一张纹理且不声明，
    //   让帧图只跟踪其中一条会与另一条的真实布局打架。
    const bool hizRequested = enableOcclusion && (hizTexture != nullptr) && (depthTexture != nullptr)
                           && (hizTexture->GetMipLevels() >= 2u)   // 单层纹理不是金字塔（防护）
                           && (m_FrameScreenW > 0u) && (m_FrameScreenH > 0u);
    m_FrameHiZRequested    = hizRequested;
    m_FrameHiZTextureBound = hizRequested;
    m_FrameHiZMipCount     = 0u;
    if (hizRequested) {
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::Undefined, rhi::ResourceState::UnorderedAccess,
                             hizTexture);
        m_FrameHiZMipCount = BuildHiZPyramid(cmd, hizTexture, depthTexture,
                                             m_FrameScreenW, m_FrameScreenH);
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource,
                             hizTexture);
    }

    if (m_FrameHiZMipCount >= 2u) {
        m_Device->UpdateDescriptorSet(m_BVHSet, 10, rhi::DescriptorType::CombinedImageSampler,
                                      hizTexture, m_HiZSampler.get());
    } else {
        // 没有可用金字塔（开关关闭 / 纹理缺失 / 层数不足）⇒ 绑占位纹理 + 层数 0
        //（shader 的遮挡测试第一句就返回 false）。占位纹理必须先转成"只读"布局，
        // 否则绑定它的派发会报 VUID-vkCmdDispatch-None-09600（采样图像的布局必须与描述符一致）。
        m_FrameHiZMipCount = 0u;
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::Undefined, rhi::ResourceState::ShaderResource,
                             m_HiZPlaceholderTex.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 10, rhi::DescriptorType::CombinedImageSampler,
                                      m_HiZPlaceholderTex.get(), m_HiZSampler.get());
    }

    // ── 【与任务 13 同一处修法】计数器的"每帧清零"必须在命令缓冲内完成（GPU 有序）──
    // 录制期的主机写会与派发竞争（CPU 领先 GPU ⇒ 两帧原子累加叠加，任务 13 实测恰为 2 倍）。
    // 本次：可见簇计数（4B）+ 三阶段读数整块（64B）+【任务 16】绘制计数（4B），
    // 都从同一个常驻 0 源缓冲拷贝。
    cmd->CopyBuffer(m_ClearZeroBuf.get(), m_VisibleClusterCountBuf.get(),
                    sizeof(u32), kNaniteZeroSlotClusterCount, 0u);
    cmd->CopyBuffer(m_ClearZeroBuf.get(), m_CullStatsBuf.get(),
                    kNaniteCullStatsBytes, kNaniteZeroSlotStats, 0u);
    // 【任务 16】绘制计数清零：它同时是"绘制端本帧要画的条数"与"命令条数"的载体，
    //   必须在本帧派发**之前**为 0（否则会残留上一帧的条数 ⇒ 空转/越界画旧命令）。
    cmd->CopyBuffer(m_ClearZeroBuf.get(), m_DrawCountBuf.get(),
                    sizeof(u32), kNaniteZeroSlotDrawCount, 0u);
    cmd->PipelineBarrier(rhi::PipelineStage::Transfer,
                         rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::CopyDst,
                         rhi::ResourceState::UnorderedAccess | rhi::ResourceState::ShaderResource);

    // ── 三阶段参数（主机可见缓冲，每帧上传；与任务 13 的实例表同款口径）──
    m_ChainParams = NaniteCullChainParams{};
    for (u32 i = 0u; i < 16u; ++i) m_ChainParams.vpRows[i] = m_FrameViewProjRows[i];
    m_ChainParams.cameraPos[0] = m_FrameCameraPos[0];
    m_ChainParams.cameraPos[1] = m_FrameCameraPos[1];
    m_ChainParams.cameraPos[2] = m_FrameCameraPos[2];
    m_ChainParams.focalPixels  = m_FrameFocalPixels;
    m_ChainParams.lodThreshold = m_FrameLODThreshold;
    m_ChainParams.screenW      = (float)m_FrameScreenW;
    m_ChainParams.screenH      = (float)m_FrameScreenH;
    m_ChainParams.hizMipCount  = m_FrameHiZMipCount;
    m_ChainParams.lodEnabled   = (m_FrameFocalPixels > 0.0f) ? 1u : 0u;
    // 【任务 16】绘制容量（`misc.z`）：间接命令与绘制计数都按它设门 ⇒ 计数恒 ≤ 容量。
    m_ChainParams._pad1        = m_FrameDrawCapacity;
    if (void* p = m_ChainParamBuf ? m_ChainParamBuf->Map() : nullptr) {
        std::memcpy(p, &m_ChainParams, sizeof(m_ChainParams));
        m_ChainParamBuf->Unmap();
    }

    if (m_BVHInstanceDomain == 0u) return;   // 0 实例 ⇒ 不派发；上面已把计数与读数清 0

    // push constant：6 个平面（本帧 viewProj 提取，与实例剔除**同一份**）+ 三个计数 + 容量
    NaniteClusterBVHParams pc{};
    std::memcpy(pc.planes, m_FrameFrustum.planes, sizeof(m_FrameFrustum.planes));
    pc.instanceCount   = m_BVHInstanceDomain;
    pc.clusterCount    = m_BVHData.clusterCount;
    pc.nodeCount       = (u32)m_BVHData.nodes.size();
    pc.visibleCapacity = kNaniteMaxVisibleClusterRefs;

    cmd->SetPipeline(m_BVHPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_BVHSet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    char label[96];
    snprintf(label, sizeof(label), "Nanite_CullChain3 (%u nodes, %u clusters, %u instances, hiz=%u)",
             pc.nodeCount, pc.clusterCount, pc.instanceCount, m_FrameHiZMipCount);
    cmd->SetDrawDebugLabel(label);
    cmd->Dispatch((m_BVHInstanceDomain + 63u) / 64u, 1u, 1u);

    // 屏障：compute 写的可见列表/计数/读数 → 后续 compute|transfer 读（下一帧的清零拷贝要消 WAR）
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader,
                         rhi::PipelineStage::ComputeShader | rhi::PipelineStage::Transfer,
                         rhi::ResourceState::UnorderedAccess,
                         rhi::ResourceState::ShaderResource | rhi::ResourceState::CopyDst);
}

} // namespace he::render
