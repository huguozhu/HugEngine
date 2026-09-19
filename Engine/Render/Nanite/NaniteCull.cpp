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

#include "Nanite_Cull.comp.spv.h"   // 由 Shader 编译管线生成（slangc → SPIR-V → spv_to_header.py）

#include <algorithm>
#include <cstring>

namespace he::render {

// 未写入槽位的哨兵值：读回时用它区分"GPU 真写过"与"还留着上一帧或初始值"。
// 取 0xFFFFFFFF 是因为合法的 indexCount/instanceCount 不可能同时为该值。
static constexpr u32 kNaniteCmdSentinel = 0xFFFFFFFFu;

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

    // 初始清零：避免第一帧读到未初始化的显存
    ResetFrameBuffers();
    UploadFakeClusters();

    HE_CORE_INFO("NaniteCull: 初始化完成（计数→间接绘制链；容量 {} 簇，当前 {} 簇）",
                 kNaniteMaxFakeClusters, m_FakeClusterCount);
    return true;
}

void NaniteCull::Shutdown() {
    m_PSO.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_Layout);

    m_FakeClusterBuf.reset();
    m_IndirectCmdBuf.reset();
    m_CountBuf.reset();
    m_RasterCountBuf.reset();

    m_Layout = rhi::kInvalidLayout;
    m_Set    = rhi::kInvalidSet;
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

} // namespace he::render
