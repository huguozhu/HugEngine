// ============================================================
// Nanite/NaniteCull.cpp — 「计数 → 间接绘制」链的计数端（§14.8 任务 3）
//
// 【本文件由 §14.8 任务 1 建立骨架，任务 3 填充 "计数端"】
//   ① 自持四个缓冲（假簇输入 / 间接命令 / 计数 / 光栅化簇计数）；
//   ② compute（Nanite_Cull.comp.slang）逐簇压缩写间接命令 + 原子累加计数；
//   ③ 屏障 `ComputeShader → DrawIndirect`，供绘制端的 `DrawIndexedIndirectCount` 消费。
//
// 【为什么用 CPU Map 清零而不是 GPU 清零】计数/命令缓冲是每帧复用的 host-visible 小缓冲，
//   引擎既有的同类清零（`GPUCulling::DispatchPhase2` 的 DrawCount、`InstanceCuller::Cull`
//   的命令头复位）都是 CPU 侧 `Map` 直接写。这里照抄同一做法，不发明新的同步机制。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态。
//   本文件不得 include `Pipeline/GPUCulling.h`。
// ============================================================

#include "Nanite/NaniteCull.h"

#include "Core/Log.h"

#include "Nanite_Cull.comp.spv.h"             // 由 Shader 编译管线生成（slangc → SPIR-V → spv_to_header.py）
#include "Nanite_InstanceCull.comp.spv.h"     // 【任务 13】同上
#include "Nanite_ClusterBVH.comp.spv.h"       // 【任务 14】per-instance cluster BVH 的 DFS 遍历

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
        rhi::BufferDesc d;
        d.size      = sizeof(u32) * 4u;   // 只用前 4B；多留 12B 对齐余量，避免与相邻字段共享尾块
        // 清零拷贝的**源**：常驻 0，只被 GPU 当 TRANSFER_SRC 读
        d.usage     = rhi::BufferUsage::TransferSrc;
        d.cpuAccess = true;   // 只在 Initialize 写一次 0
        m_VisibleCountClearBuf = m_Device->CreateBuffer(d);
        if (!m_VisibleCountClearBuf) { HE_CORE_ERROR("NaniteCull: 计数清零源缓冲创建失败"); return false; }
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

    // ── 3b. 【任务 13】实例剔除：4 个显式 SSBO 绑定 + compute PSO ──
    // 与任务 3 同样**不走 bindless**：模块私有缓冲、生命周期清晰，显式绑定最简单也最稳。
    {
        rhi::DescriptorSetLayoutDesc cullLayout;
        cullLayout.bindings = {
            { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 实例表（只读）
            { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 包围球（只读）
            { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 可见列表（读写）
            { 3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 可见计数（读写）
        };
        m_InstanceCullLayout = m_Device->CreateDescriptorSetLayout(cullLayout);
        m_InstanceCullSet    = m_Device->AllocateDescriptorSet(m_InstanceCullLayout);
        m_Device->UpdateDescriptorSet(m_InstanceCullSet, 0, rhi::DescriptorType::StorageBuffer, m_InstanceBuf.get());
        m_Device->UpdateDescriptorSet(m_InstanceCullSet, 1, rhi::DescriptorType::StorageBuffer, m_InstanceSphereBuf.get());
        m_Device->UpdateDescriptorSet(m_InstanceCullSet, 2, rhi::DescriptorType::StorageBuffer, m_VisibleInstanceBuf.get());
        m_Device->UpdateDescriptorSet(m_InstanceCullSet, 3, rhi::DescriptorType::StorageBuffer, m_VisibleInstanceCountBuf.get());

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
        // TransferDst：每帧开头的"计数清零"是命令缓冲里的 4B 拷贝（见 RecordClusterBVHPass）
        d.usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst;
        d.cpuAccess = true;   // dump 帧读回
        m_VisibleClusterCountBuf = m_Device->CreateBuffer(d);
        if (!m_VisibleClusterCountBuf) { HE_CORE_ERROR("NaniteCull: 可见簇计数缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32);
        d.usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst;
        d.cpuAccess = true;   // dump 帧读回（验收的"遍历访问数"）
        m_BVHVisitedCountBuf = m_Device->CreateBuffer(d);
        if (!m_BVHVisitedCountBuf) { HE_CORE_ERROR("NaniteCull: BVH 访问节点计数缓冲创建失败"); return false; }
    }
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32) * 2u;   // 前 4B 给可见簇计数、后 4B 给访问计数
        d.usage     = rhi::BufferUsage::TransferSrc;
        d.cpuAccess = true;   // 只在 Initialize 写一次 0
        m_BVHZeroClearBuf = m_Device->CreateBuffer(d);
        if (!m_BVHZeroClearBuf) { HE_CORE_ERROR("NaniteCull: BVH 计数清零源创建失败"); return false; }
    }

    // ── 3c. 【任务 14】cluster BVH 遍历：7 个显式 SSBO 绑定 + compute PSO ──
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
            { 6, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 访问节点计数（读写）
        };
        m_BVHLayout = m_Device->CreateDescriptorSetLayout(bvhLayout);
        m_BVHSet    = m_Device->AllocateDescriptorSet(m_BVHLayout);
        m_Device->UpdateDescriptorSet(m_BVHSet, 0, rhi::DescriptorType::StorageBuffer, m_BVHNodeBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 1, rhi::DescriptorType::StorageBuffer, m_BVHLeafBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 2, rhi::DescriptorType::StorageBuffer, m_BVHSphereBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 3, rhi::DescriptorType::StorageBuffer, m_InstanceBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 4, rhi::DescriptorType::StorageBuffer, m_VisibleClusterBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 5, rhi::DescriptorType::StorageBuffer, m_VisibleClusterCountBuf.get());
        m_Device->UpdateDescriptorSet(m_BVHSet, 6, rhi::DescriptorType::StorageBuffer, m_BVHVisitedCountBuf.get());

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

    // 初始清零：避免第一帧读到未初始化的显存
    ResetFrameBuffers();
    UploadFakeClusters();
    ResetInstanceCullBuffers();

    // 【任务 14】两个 BVH 计数的清零源恒为 0（只在这里写一次，之后只被 GPU 当 TRANSFER_SRC 读）
    if (void* p = m_BVHZeroClearBuf ? m_BVHZeroClearBuf->Map() : nullptr) {
        static_cast<u32*>(p)[0] = 0u;   // 可见簇计数的来源
        static_cast<u32*>(p)[1] = 0u;   // 已访问节点计数的来源
        m_BVHZeroClearBuf->Unmap();
    }

    HE_CORE_INFO("NaniteCull: 初始化完成（计数→间接绘制链；容量 {} 簇，当前 {} 簇）",
                 kNaniteMaxFakeClusters, m_FakeClusterCount);
    HE_CORE_INFO("NaniteCull: 实例剔除通道就绪（视锥 → 可见实例列表；容量 {} 实例）",
                 kNaniteMaxTestInstances);
    HE_CORE_INFO("NaniteCull: cluster BVH 通道就绪（容量 {} 节点 / {} 簇 / {} 实例域 / {} 条可见引用；"
                 "叶子容量 {}，深度上限 {}，显式栈 {}）",
                 kNaniteMaxBVHNodes, kNaniteMaxBVHClusters, kNaniteMaxBVHInstances,
                 kNaniteMaxVisibleClusterRefs, kNaniteBVHLeafCapacity, kNaniteBVHMaxDepth,
                 kNaniteBVHMaxStackDepth);
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

    m_FakeClusterBuf.reset();
    m_IndirectCmdBuf.reset();
    m_CountBuf.reset();
    m_RasterCountBuf.reset();

    // 【任务 13】实例剔除资源一并释放
    m_InstanceBuf.reset();
    m_InstanceSphereBuf.reset();
    m_VisibleInstanceBuf.reset();
    m_VisibleInstanceCountBuf.reset();
    m_VisibleCountClearBuf.reset();
    m_TestInstances.clear();
    m_TestSpheres.clear();
    m_CpuVisibleInstances.clear();
    m_TestInstanceCount = 0u;

    // 【任务 14】cluster BVH 资源与状态一并释放（`m_BVHData` 的容器也清空，避免留下悬空语义）
    m_BVHNodeBuf.reset();
    m_BVHLeafBuf.reset();
    m_BVHSphereBuf.reset();
    m_VisibleClusterBuf.reset();
    m_VisibleClusterCountBuf.reset();
    m_BVHVisitedCountBuf.reset();
    m_BVHZeroClearBuf.reset();
    m_BVHData = NaniteClusterBVH{};
    m_BVHReady = false;
    m_BVHInstanceDomain = 0u;

    m_Layout = rhi::kInvalidLayout;
    m_Set    = rhi::kInvalidSet;
    m_InstanceCullLayout = rhi::kInvalidLayout;
    m_InstanceCullSet    = rhi::kInvalidSet;
    m_BVHLayout          = rhi::kInvalidLayout;
    m_BVHSet             = rhi::kInvalidSet;
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
    // 每帧重置（无论本次是否派发）：让"上一帧的读数"不会残留到本帧的读回里
    ResetFrameBuffers();
    UploadFakeClusters();

    if (!cmd || !m_PSO) return;
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
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader,
                         rhi::PipelineStage::DrawIndirect,
                         rhi::ResourceState::UnorderedAccess,
                         rhi::ResourceState::IndirectArgument);
}

// ============================================================
// §14.8 任务 13：实例剔除（视锥 → 可见实例列表 + 计数）
//
// 数据流：`SetInstanceCullFrame`（每帧，帧图构建期）
//           → 提取 6 平面 + 生成合成实例表/包围球（世界空间）
//         `RecordInstanceCullPass`（每帧，pass 执行期）
//           → 重置计数/列表 → 上传实例与球 → CPU 参考剔除 → Dispatch → 屏障
//         `NaniteRenderer::LogInstanceCullReadback`（dump 帧）
//           → GPU 读回计数/列表 vs CPU 参考结果
// ============================================================

void NaniteCull::SetInstanceCullFrame(const float4x4& viewProj, const float3& cameraPosition,
                                      u32 testInstanceCount) {
    m_TestInstanceCount = std::min(testInstanceCount, kNaniteMaxTestInstances);
    m_FrameViewProj     = viewProj;
    m_FrameCameraPos    = cameraPosition;
    // `&viewProj[0][0]` 就是 glm::mat4 的列主序首地址（16 个 float）
    m_FrameFrustum      = NaniteExtractFrustumPlanes(&viewProj[0][0]);
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
    // 清零拷贝的**源**缓冲恒为 0（只在这里写一次，之后只被 GPU 读）
    if (void* p = m_VisibleCountClearBuf ? m_VisibleCountClearBuf->Map() : nullptr) {
        *static_cast<u32*>(p) = 0u;
        m_VisibleCountClearBuf->Unmap();
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
    //   【为什么不能靠"任务 3 已经这么写"】同一批运行里假簇链的 `count_buffer=6` 是对的 ——
    //   但那是**这台机器上 CPU/GPU 相位恰好错开**的结果，不是排序保证：本 pass 在第 4 个 pass、
    //   假簇链在第 7 个 pass，两者的主机写相对 GPU 的时间点不同。所以本 pass 不依赖相位，直接
    //   把清零放进命令缓冲；假簇链是否同样存在这一潜在竞争**不在本任务改动面内**（如实记录）。
    // 【为什么用拷贝而不是 `vkCmdFillBuffer`】RHI 目前只暴露 `CopyBuffer`（任务 12 已用它做上传），
    //   不为本任务扩 RHI 面；4B 拷贝的源缓冲常驻 0，语义与 fill 等价。
    cmd->CopyBuffer(m_VisibleCountClearBuf.get(), m_VisibleInstanceCountBuf.get(), sizeof(u32));
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
// §14.8 任务 14：per-instance cluster BVH（构建入库 + 每帧深度优先遍历 + CPU 参考）
//
// 数据流：`SetClusterBVH`（一次性，任务 12 的资产构建点）
//           → CPU 构建 BVH（`BuildNaniteClusterBVH`）+ 上传节点/叶子簇表/簇球
//         `SetInstanceCullFrame`（每帧）→ 更新视锥与合成实例表（任务 13）
//         `RecordClusterBVHPass`（每帧）
//           → 命令缓冲内清两个计数 → 上传 push constant → Dispatch → 屏障
//         `RunClusterBVHCPUReference`（dump 帧）
//           → CPU 参考遍历（同一棵 BVH、同一张簇球表、同一份实例表、同一个视锥）
//         `NaniteRenderer::LogClusterBVHReadback`（dump 帧）
//           → GPU 读回（visited / visibleCount / 可见列表）vs CPU 参考，逐项比较
// ============================================================

bool NaniteCull::SetClusterBVH(std::span<const NaniteClusterRecord> clusters) {
    if (!m_Device || !m_BVHNodeBuf || !m_BVHLeafBuf || !m_BVHSphereBuf) {
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

    // ── ② CPU 构建（RHI-free 的构建器；失败不改写出参）──
    NaniteClusterBVH built;
    if (!BuildNaniteClusterBVH(clusters.first(clusterCount), built)) {
        HE_CORE_ERROR("NaniteCull: cluster BVH 构建失败（簇数 {}）", clusterCount);
        return false;
    }

    // ── ③ 一次性上传三个只读缓冲 ──
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

    m_BVHData   = std::move(built);
    m_BVHReady  = !m_BVHData.Empty();
    m_BVHInstanceDomain = 0u;   // 每帧在 RecordClusterBVHPass 里按本帧实例数重算

    HE_CORE_INFO("NaniteCull: cluster BVH 入库完成（簇 {} → 节点 {} / 叶子 {} / 深度 {} / "
                 "最大叶子簇数 {} / 栈上界 {}；叶子容量 {}，深度上限 {}）",
                 m_BVHData.clusterCount, (u32)m_BVHData.nodes.size(), m_BVHData.leafCount,
                 m_BVHData.depth, m_BVHData.maxLeafClusterCount, m_BVHData.maxStackDepthUpperBound,
                 kNaniteBVHLeafCapacity, kNaniteBVHMaxDepth);
    return true;
}

NaniteClusterBVHTraversalStats NaniteCull::RunClusterBVHCPUReference(
        std::vector<NaniteVisibleClusterRef>& outVisible) const {
    outVisible.clear();
    if (!m_BVHReady) return NaniteClusterBVHTraversalStats{};

    // 容量 = 声明的常量（与 GPU 的 push constant `visibleCapacity` **同一个数**）；
    // 正常运行下 64 实例 × 8287 簇 = 53 万 < 105 万 ⇒ 永不截断（截断会让集合比较失去意义）。
    outVisible.resize(kNaniteMaxVisibleClusterRefs);

    NaniteClusterBVHTraversalStats stats{};
    const u32 written = NaniteTraverseClusterBVHCPU(
        m_FrameFrustum,
        m_BVHData.View(),
        m_TestInstances.empty() ? nullptr : m_TestInstances.data(),
        m_TestInstanceCount,
        kNaniteMaxBVHInstances,
        outVisible.empty() ? nullptr : outVisible.data(),
        (u32)outVisible.size(),
        &stats);
    outVisible.resize(written);
    return stats;
}

void NaniteCull::RecordClusterBVHPass(rhi::IRHICommandList* cmd) {
    if (!cmd || !m_BVHReady || !m_BVHPSO) return;

    // 本帧实例域：与 CPU 参考、与可见簇引用表的容量口径**同一个钳制**
    m_BVHInstanceDomain = (m_TestInstanceCount < kNaniteMaxBVHInstances)
                        ? m_TestInstanceCount : kNaniteMaxBVHInstances;

    // ── 【与任务 13 同一处修法】两个计数的"每帧清零"必须在命令缓冲内完成（GPU 有序）──
    // 录制期的主机写会与派发竞争（CPU 领先 GPU ⇒ 两帧原子累加叠加，任务 13 实测恰为 2 倍）。
    // 这里用同一个 8B 常驻 0 源缓冲的两半，各做一次 4B 拷贝；末尾屏障补 Transfer/CopyDst 消 WAR。
    constexpr u64 kHalfWord = sizeof(u32);
    cmd->CopyBuffer(m_BVHZeroClearBuf.get(), m_VisibleClusterCountBuf.get(), kHalfWord, 0u, 0u);
    cmd->CopyBuffer(m_BVHZeroClearBuf.get(), m_BVHVisitedCountBuf.get(),     kHalfWord, kHalfWord, 0u);
    cmd->PipelineBarrier(rhi::PipelineStage::Transfer,
                         rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::CopyDst,
                         rhi::ResourceState::UnorderedAccess | rhi::ResourceState::ShaderResource);

    if (m_BVHInstanceDomain == 0u) return;   // 0 实例 ⇒ 不派发；上面已把两个计数清 0

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
    snprintf(label, sizeof(label), "Nanite_ClusterBVH (%u nodes, %u clusters, %u instances)",
             pc.nodeCount, pc.clusterCount, pc.instanceCount);
    cmd->SetDrawDebugLabel(label);
    cmd->Dispatch((m_BVHInstanceDomain + 63u) / 64u, 1u, 1u);

    // 屏障：compute 写的可见列表/计数 → 后续 compute|transfer 读（下一帧的清零拷贝要消 WAR）
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader,
                         rhi::PipelineStage::ComputeShader | rhi::PipelineStage::Transfer,
                         rhi::ResourceState::UnorderedAccess,
                         rhi::ResourceState::ShaderResource | rhi::ResourceState::CopyDst);
}

} // namespace he::render
