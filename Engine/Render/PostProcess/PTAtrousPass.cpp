#include "PostProcess/PTAtrousPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "PT_Atrous.comp.spv.h"   // k_PT_Atrous_comp_spv

#include <algorithm>   // std::clamp

namespace he::render {

bool PTAtrousPass::Initialize(rhi::IRHIDevice* device, const Config& cfg) {
    m_Device = device;
    m_Cfg    = cfg;
    m_Width  = cfg.width;
    m_Height = cfg.height;
    HE_ASSERT(m_Device, "PTAtrousPass: null device");

    // set0：b0=color(SampledImage), b1=depth, b2=normal, b3=output(StorageImage)
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
        {1, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
        {2, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
        {3, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute},
    };
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);
    if (m_Layout == rhi::kInvalidLayout || m_Set == rhi::kInvalidSet) {
        HE_CORE_ERROR("PTAtrousPass: set0 布局/描述符集创建失败");
        return false;
    }

    // compute PSO（40B push constant）
    rhi::ShaderBytecode cs;
    cs.stage = rhi::ShaderStage::Compute; cs.spirv = k_PT_Atrous_comp_spv; cs.entryPoint = "main";
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskCompute;
    pc.size      = 40;   // AtrousPC：uint2(8)+uint(4)+float×5(20)+float×2 pad(8)
    rhi::PipelineStateDesc d;
    d.bindPoint = rhi::PipelineBindPoint::Compute;
    d.computeShader = &cs;
    d.pushConstantRanges = {pc};
    d.descriptorSetLayouts = {m_Layout};
    d.debugName = "PTAtrous";
    m_PSO = device->CreatePipelineState(d);
    if (!m_PSO) { HE_CORE_ERROR("PTAtrousPass: compute PSO 创建失败"); return false; }

    CreateTextures(m_Width, m_Height);
    m_Ready = true;
    HE_CORE_INFO("PTAtrousPass: 初始化完成 ({}x{}, iters={})", m_Width, m_Height, m_Cfg.iterations);
    return true;
}

void PTAtrousPass::CreateTextures(u32 w, u32 h) {
    rhi::TextureDesc d;
    d.format = rhi::Format::RGBA16_FLOAT;
    d.width = w; d.height = h; d.mipLevels = 1;
    d.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
    m_Output = m_Device->CreateTexture(d);
}

void PTAtrousPass::Shutdown() {
    m_Output.reset();
    m_PSO.reset();
    if (m_Device && m_Set != rhi::kInvalidSet) m_Set = rhi::kInvalidSet;
    if (m_Device && m_Layout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_Layout);
        m_Layout = rhi::kInvalidLayout;
    }
    m_Device = nullptr; m_Ready = false;
}

void PTAtrousPass::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width = w; m_Height = h;
    m_Output.reset();
    CreateTextures(w, h);
}

void PTAtrousPass::SetInputs(rhi::IRHITexture* color, rhi::IRHITexture* depth, rhi::IRHITexture* normal) {
    m_Color = color; m_Depth = depth; m_Normal = normal;
    if (!m_Device) return;
    m_Device->UpdateDescriptorSet(m_Set, 0, rhi::DescriptorType::SampledImage, color, nullptr);
    m_Device->UpdateDescriptorSet(m_Set, 1, rhi::DescriptorType::SampledImage, depth, nullptr);
    m_Device->UpdateDescriptorSet(m_Set, 2, rhi::DescriptorType::SampledImage, normal, nullptr);
}

void PTAtrousPass::SetParams(u32 iterations, float sigmaDepth, float normalPower,
                             float sigmaColor, float clampThreshold) {
    m_Cfg.iterations     = std::clamp(iterations, 1u, 5u);
    m_Cfg.sigmaDepth     = sigmaDepth;
    m_Cfg.normalPower    = normalPower;
    m_Cfg.sigmaColor     = sigmaColor;
    m_Cfg.clampThreshold = clampThreshold;
}

void PTAtrousPass::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Color || !m_Depth || !m_Normal) return;

    m_Device->UpdateDescriptorSetWithImageView(m_Set, 3,
        rhi::DescriptorType::StorageImage, m_Output->GetNativeHandle());

    struct {
        uint2 dispatchDim;
        u32   iterations;
        float sigmaDepth;
        float normalPower;
        float sigmaColor;
        float clampThreshold;
        float pad1, pad2;
    } pc;
    pc.dispatchDim     = uint2(m_Width, m_Height);
    pc.iterations      = m_Cfg.iterations;
    pc.sigmaDepth      = m_Cfg.sigmaDepth;
    pc.normalPower     = m_Cfg.normalPower;
    pc.sigmaColor      = m_Cfg.sigmaColor;
    pc.clampThreshold  = m_Cfg.clampThreshold;
    pc.pad1 = pc.pad2 = 0.0f;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch((m_Width + 7) / 8, (m_Height + 7) / 8, 1);
}

} // namespace he::render
