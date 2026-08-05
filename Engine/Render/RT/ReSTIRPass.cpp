// ============================================================
// ReSTIRPass.cpp — ReSTIR DI 时空重采样 Pass 实现
// Init → Temporal → Spatial 三个 compute dispatch 顺序执行
// ============================================================
#include "RT/ReSTIRPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
// 通过 Material.h 引入 ShaderTypes.slang（ReSTIRPushConstant / PTReservoir / GPULight）
#include "Pipeline/Material.h"

// ReSTIR 着色器 SPIR-V
#include "ReSTIR_Init.comp.spv.h"
#include "ReSTIR_Temporal.comp.spv.h"
#include "ReSTIR_Spatial.comp.spv.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <algorithm>

namespace he::render {

// ============================================================
// CreatePipeline — 创建 compute PSO + set0 布局 + 描述符集
// ============================================================
bool ReSTIRPass::CreatePipeline(ComputePipe& pipe,
                                const std::vector<rhi::DescriptorSetLayoutBinding>& bindings,
                                const std::vector<u32>& spirv, StringView name) {
    // set0 布局
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = bindings;
    pipe.layout = m_Device->CreateDescriptorSetLayout(layout);
    if (pipe.layout == rhi::kInvalidLayout) {
        HE_CORE_ERROR("ReSTIRPass[{}]: set0 布局创建失败", name);
        return false;
    }
    pipe.set = m_Device->AllocateDescriptorSet(pipe.layout);
    if (pipe.set == rhi::kInvalidSet) {
        HE_CORE_ERROR("ReSTIRPass[{}]: set0 描述符集分配失败", name);
        return false;
    }

    // compute PSO（128B push constant）
    rhi::ShaderBytecode cs;
    cs.stage = rhi::ShaderStage::Compute; cs.spirv = spirv; cs.entryPoint = "main";
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskCompute;
    pc.size      = sizeof(ReSTIRPushConstant);
    rhi::PipelineStateDesc d;
    d.bindPoint = rhi::PipelineBindPoint::Compute;
    d.computeShader = &cs;
    d.pushConstantRanges = {pc};
    d.descriptorSetLayouts = {pipe.layout};
    d.debugName = String(name);
    pipe.pso = m_Device->CreatePipelineState(d);
    if (!pipe.pso) {
        HE_CORE_ERROR("ReSTIRPass[{}]: compute PSO 创建失败", name);
        return false;
    }
    return true;
}

void ReSTIRPass::DestroyPipeline(ComputePipe& pipe) {
    pipe.pso.reset();
    if (m_Device && pipe.layout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(pipe.layout);
    pipe.layout = rhi::kInvalidLayout;
    pipe.set = rhi::kInvalidSet;
}

// ============================================================
// Initialize — 创建 3 个 compute PSO + 蓄水池 SSBO + 历史纹理
// ============================================================
bool ReSTIRPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    u32 total = width * height;

    // ── Init：b0=ptDepth, b1=ptNormal, b2=GPULight[] SSBO, b3=Initial RW SSBO,
    //    b4=ptAlbedo（PT 第 5 UAV：albedo(rgb)+metallic(a)）──
    {
        std::vector<rhi::DescriptorSetLayoutBinding> bindings = {
            {0, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
            {1, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
            {2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
            {3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
            {4, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
        };
        if (!CreatePipeline(m_Init, bindings, k_ReSTIR_Init_comp_spv, "ReSTIR_Init")) return false;
    }
    // ── Temporal：b0=ptVel, b1=ptDepth, b2=ptNormal, b3=Initial, b4=History,
    //    b5=HistDepth, b6=HistNormal, b7=Temporal RW, b8=CurDepth RW,
    //    b9=CurNormal RW, b10=GPULight[], b11=ptAlbedo ──
    {
        std::vector<rhi::DescriptorSetLayoutBinding> bindings = {
            {0, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
            {1, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
            {2, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
            {3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
            {4, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
            {5, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
            {6, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
            {7, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
            {8, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute},
            {9, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute},
            {10, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
            {11, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
        };
        if (!CreatePipeline(m_Temporal, bindings, k_ReSTIR_Temporal_comp_spv, "ReSTIR_Temporal")) return false;
    }
    // ── Spatial：b0=ptDepth, b1=ptNormal, b2=Temporal, b3=Final RW,
    //    b4=GPULight[], b5=ptAlbedo ──
    {
        std::vector<rhi::DescriptorSetLayoutBinding> bindings = {
            {0, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
            {1, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
            {2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
            {3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
            {4, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
            {5, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
        };
        if (!CreatePipeline(m_Spatial, bindings, k_ReSTIR_Spatial_comp_spv, "ReSTIR_Spatial")) return false;
    }

    // ── 蓄水池 SSBO（PTReservoir × W×H，32B/个）──
    rhi::BufferDesc bd;
    bd.size = sizeof(PTReservoir) * total;
    bd.usage = rhi::BufferUsage::Storage;
    m_Initial = device->CreateBuffer(bd);
    m_TemporalBuf[0] = device->CreateBuffer(bd);
    m_TemporalBuf[1] = device->CreateBuffer(bd);
    m_Final = device->CreateBuffer(bd);
    if (!m_Initial || !m_TemporalBuf[0] || !m_TemporalBuf[1] || !m_Final) {
        HE_CORE_ERROR("ReSTIRPass: 蓄水池 SSBO 创建失败 ({}B/份)", bd.size);
        return false;
    }

    // ── 历史纹理（双缓冲）──
    rhi::TextureDesc td;
    td.width = width; td.height = height; td.mipLevels = 1;
    td.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;
    td.format = rhi::Format::R32_FLOAT;
    m_HistDepth[0] = device->CreateTexture(td);
    m_HistDepth[1] = device->CreateTexture(td);
    td.format = rhi::Format::RGBA16_FLOAT;
    m_HistNormal[0] = device->CreateTexture(td);
    m_HistNormal[1] = device->CreateTexture(td);
    if (!m_HistDepth[0] || !m_HistDepth[1] || !m_HistNormal[0] || !m_HistNormal[1]) {
        HE_CORE_ERROR("ReSTIRPass: 历史纹理创建失败");
        return false;
    }

    m_HistorySlot = 0;
    m_Ready = true;
    HE_CORE_INFO("ReSTIRPass: 初始化完成 ({}x{}, 蓄水池 {}B/份×4, 历史纹理双缓冲)",
                 width, height, bd.size);
    return true;
}

void ReSTIRPass::Shutdown() {
    m_HistNormal[1].reset(); m_HistNormal[0].reset();
    m_HistDepth[1].reset();  m_HistDepth[0].reset();
    m_Final.reset(); m_TemporalBuf[1].reset(); m_TemporalBuf[0].reset(); m_Initial.reset();
    DestroyPipeline(m_Spatial);
    DestroyPipeline(m_Temporal);
    DestroyPipeline(m_Init);
    m_Device = nullptr;
    m_Width = m_Height = 0;
    m_Ready = false;
}

// ============================================================
// Execute — Init → Temporal → Spatial 顺序 dispatch
// ============================================================
void ReSTIRPass::Execute(rhi::IRHICommandList* cmd, const ReSTIRDispatchContext& ctx) {
    if (!m_Ready || !ctx.lightBuffer || !ctx.ptDepth || !ctx.ptNormal || !ctx.ptVelocity
        || !ctx.ptAlbedo) return;

    const u32 readSlot  = m_HistorySlot;         // 读取槽（上帧历史）
    const u32 writeSlot = m_HistorySlot ^ 1;     // 写入槽（本帧 → 下帧历史）

    // ── 填充 push constant（三个 Pass 共享）──
    ReSTIRPushConstant pc{};
    memcpy(&pc.invViewProj, glm::value_ptr(ctx.invViewProj), sizeof(float) * 16);
    pc.cameraPos       = float4(ctx.cameraPos, 0.0f);
    pc.dispatchDimX    = m_Width;
    pc.dispatchDimY    = m_Height;
    pc.frameIndex      = ctx.frameIndex;
    pc.lightCount      = ctx.lightCount;
    pc.candidateCount  = std::clamp(ctx.candidateCount, 1u, 64u);
    pc.spatialRadius   = std::clamp(ctx.spatialRadius, 1u, 8u);
    pc.spatialSamples  = std::clamp(ctx.spatialSamples, 1u, 16u);
    pc.maxDistance     = ctx.maxDistance;
    pc.flags           = ctx.historyValid ? 4u : 0u;  // bit2=historyValid

    const u32 groupsX = (m_Width + 7) / 8;
    const u32 groupsY = (m_Height + 7) / 8;

    // ── Pass 1: Init ──
    {
        m_Device->UpdateDescriptorSet(m_Init.set, 0,
            rhi::DescriptorType::SampledImage, ctx.ptDepth, nullptr);
        m_Device->UpdateDescriptorSet(m_Init.set, 1,
            rhi::DescriptorType::SampledImage, ctx.ptNormal, nullptr);
        m_Device->UpdateDescriptorSet(m_Init.set, 2,
            rhi::DescriptorType::StorageBuffer, ctx.lightBuffer);
        m_Device->UpdateDescriptorSet(m_Init.set, 3,
            rhi::DescriptorType::StorageBuffer, m_Initial.get());
        m_Device->UpdateDescriptorSet(m_Init.set, 4,
            rhi::DescriptorType::SampledImage, ctx.ptAlbedo, nullptr);
        cmd->SetPipeline(m_Init.pso.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Init.set);
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->Dispatch(groupsX, groupsY, 1);
    }

    // ── Pass 2: Temporal（读历史槽，写 TemporalReservoir[writeSlot] + 下一帧历史）──
    {
        m_Device->UpdateDescriptorSet(m_Temporal.set, 0,
            rhi::DescriptorType::SampledImage, ctx.ptVelocity, nullptr);
        m_Device->UpdateDescriptorSet(m_Temporal.set, 1,
            rhi::DescriptorType::SampledImage, ctx.ptDepth, nullptr);
        m_Device->UpdateDescriptorSet(m_Temporal.set, 2,
            rhi::DescriptorType::SampledImage, ctx.ptNormal, nullptr);
        m_Device->UpdateDescriptorSet(m_Temporal.set, 3,
            rhi::DescriptorType::StorageBuffer, m_Initial.get());
        m_Device->UpdateDescriptorSet(m_Temporal.set, 4,
            rhi::DescriptorType::StorageBuffer, m_TemporalBuf[readSlot].get());
        m_Device->UpdateDescriptorSet(m_Temporal.set, 5,
            rhi::DescriptorType::SampledImage, m_HistDepth[readSlot].get(), nullptr);
        m_Device->UpdateDescriptorSet(m_Temporal.set, 6,
            rhi::DescriptorType::SampledImage, m_HistNormal[readSlot].get(), nullptr);
        m_Device->UpdateDescriptorSet(m_Temporal.set, 7,
            rhi::DescriptorType::StorageBuffer, m_TemporalBuf[writeSlot].get());
        m_Device->UpdateDescriptorSetWithImageView(m_Temporal.set, 8,
            rhi::DescriptorType::StorageImage, m_HistDepth[writeSlot]->GetNativeHandle());
        m_Device->UpdateDescriptorSetWithImageView(m_Temporal.set, 9,
            rhi::DescriptorType::StorageImage, m_HistNormal[writeSlot]->GetNativeHandle());
        m_Device->UpdateDescriptorSet(m_Temporal.set, 10,
            rhi::DescriptorType::StorageBuffer, ctx.lightBuffer);
        m_Device->UpdateDescriptorSet(m_Temporal.set, 11,
            rhi::DescriptorType::SampledImage, ctx.ptAlbedo, nullptr);
        cmd->SetPipeline(m_Temporal.pso.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Temporal.set);
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->Dispatch(groupsX, groupsY, 1);
    }

    // ── Pass 3: Spatial（读 TemporalReservoir[writeSlot]，写 FinalReservoir）──
    {
        m_Device->UpdateDescriptorSet(m_Spatial.set, 0,
            rhi::DescriptorType::SampledImage, ctx.ptDepth, nullptr);
        m_Device->UpdateDescriptorSet(m_Spatial.set, 1,
            rhi::DescriptorType::SampledImage, ctx.ptNormal, nullptr);
        m_Device->UpdateDescriptorSet(m_Spatial.set, 2,
            rhi::DescriptorType::StorageBuffer, m_TemporalBuf[writeSlot].get());
        m_Device->UpdateDescriptorSet(m_Spatial.set, 3,
            rhi::DescriptorType::StorageBuffer, m_Final.get());
        m_Device->UpdateDescriptorSet(m_Spatial.set, 4,
            rhi::DescriptorType::StorageBuffer, ctx.lightBuffer);
        m_Device->UpdateDescriptorSet(m_Spatial.set, 5,
            rhi::DescriptorType::SampledImage, ctx.ptAlbedo, nullptr);
        cmd->SetPipeline(m_Spatial.pso.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Spatial.set);
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->Dispatch(groupsX, groupsY, 1);
    }
}

} // namespace he::render
