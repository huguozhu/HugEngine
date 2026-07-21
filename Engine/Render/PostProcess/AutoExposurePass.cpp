// PostProcess/AutoExposurePass.cpp — 自动曝光（Compute reduction + CPU 平均）
#include "AutoExposurePass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "AutoExposure.comp.spv.h"
#include <algorithm>

namespace he::render {

bool AutoExposurePass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device; m_Width = width; m_Height = height;

    // 描述符集：HDR(0) + Result SSBO(1)
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},
        {1, rhi::DescriptorType::StorageBuffer,        1, rhi::kStageMaskCompute},
    };
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    // Compute PSO
    m_CS.stage = rhi::ShaderStage::Compute; m_CS.spirv = k_AutoExposure_comp_spv; m_CS.entryPoint = "main";
    rhi::PushConstantRange pc; pc.stageMask = rhi::kStageMaskCompute; pc.size = 16;
    rhi::PipelineStateDesc d;
    d.bindPoint = rhi::PipelineBindPoint::Compute; d.computeShader = &m_CS;
    d.pushConstantRanges = {pc}; d.descriptorSetLayouts = {m_Layout}; d.debugName = "AutoExposure";
    m_PSO = device->CreatePipelineState(d);
    HE_ASSERT(m_PSO, "AutoExposurePass: PSO failed");

    // SSBO: 256 个 partial sum（16×16 组 × 256 线程/组 → 256 组）
    {
        rhi::BufferDesc bd; bd.size = kNumGroups * sizeof(float);
        bd.usage = rhi::BufferUsage::Storage; bd.cpuAccess = true;
        m_ResultBuf = device->CreateBuffer(bd);
    }

    m_Ready = true;
    HE_CORE_INFO("AutoExposurePass 初始化完成");
    return true;
}

void AutoExposurePass::Shutdown() {
    m_PSO.reset(); m_ResultBuf.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_Layout);
    m_Device = nullptr; m_Ready = false;
}

void AutoExposurePass::OnResize(u32 w, u32 h) { m_Width = w; m_Height = h; }

void AutoExposurePass::SetInput(rhi::IRHITexture* hdr, rhi::IRHISampler* sampler) {
    m_HDRInput = hdr; m_HDRSampler = sampler;
    if (m_HDRInput && m_HDRSampler)
        m_Device->UpdateDescriptorSet(m_Set, 0, rhi::DescriptorType::CombinedImageSampler, m_HDRInput, m_HDRSampler);
}

void AutoExposurePass::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_HDRInput) return;

    // 绑定 SSBO
    m_Device->UpdateDescriptorSet(m_Set, 1, rhi::DescriptorType::StorageBuffer, m_ResultBuf.get());

    // 调度：16×16 = 256 组
    struct { float adaptSpeed, targetLogLum, deltaTime; u32 totalPixels; } pc;
    pc.adaptSpeed   = m_AdaptSpeed;
    pc.targetLogLum = log2(m_TargetLum);
    pc.deltaTime    = 0.016f;
    pc.totalPixels  = m_Width * m_Height;

    cmd->SetPipeline(m_PSO.get()); cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch(16, rhi::kStageMaskFragment, 1);  // 256 组

    // CPU 读回并平均
    void* data = m_ResultBuf->Map();
    if (data) {
        auto* vals = static_cast<float*>(data);
        float sum = 0; u32 valid = 0;
        for (u32 i = 0; i < kNumGroups; ++i) {
            float v = vals[i];
            if (v > -20.0f && v < 20.0f) { sum += v; valid++; }  // 剔除异常值
        }
        m_ResultBuf->Unmap();

        if (valid > 0) {
            float curAvg = sum / float(valid);
            float t = std::min(m_AdaptSpeed * 0.016f, 1.0f);
            m_PrevLogLum = m_PrevLogLum + (curAvg - m_PrevLogLum) * t;  // 时间混合
            m_Exposure = exp2(log2(m_TargetLum) - m_PrevLogLum);
            m_Exposure = std::max(0.005f, std::min(m_Exposure, 200.0f));
        }
    }
}

} // namespace he::render
