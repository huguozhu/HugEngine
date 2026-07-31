// ============================================================
// RTEffectPass.cpp — RT 效果 Pass 基类实现
// 公共能力：set0 布局/描述符集、效果 RT 管线 + SBT、输出 UAV 纹理、TraceRays
// ============================================================
#include "RT/RTEffectPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
// 通过 Material.h 引入 ShaderTypes.slang（GPULight 等共享结构）
#include "Pipeline/Material.h"

namespace he::render {

bool RTEffectPass::Initialize(rhi::IRHIDevice* device,
                              u32 width, u32 height,
                              std::vector<rhi::DescriptorSetLayoutBinding> rayGenBindings,
                              std::vector<rhi::ShaderBytecode> rtShaders,
                              std::vector<rhi::RTShaderGroup> shaderGroups,
                              rhi::PushConstantRange pcRange,
                              rhi::Format outFormat,
                              rhi::TextureUsage outUsage,
                              u32 maxPayloadSize,
                              u32 maxRecursionDepth,
                              StringView debugName) {
    m_Device   = device;
    m_Width    = width;
    m_Height   = height;
    m_PCRange  = pcRange;
    m_DebugName = String(debugName);

    // ── 创建 set0 RayGen 描述符集布局 + 分配描述符集 ──
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = std::move(rayGenBindings);
    m_RayGenLayout = device->CreateDescriptorSetLayout(layout);
    if (m_RayGenLayout == rhi::kInvalidLayout) {
        HE_CORE_ERROR("RTEffectPass[{}]: set0 布局创建失败", debugName);
        return false;
    }
    m_RayGenSet = device->AllocateDescriptorSet(m_RayGenLayout);
    if (m_RayGenSet == rhi::kInvalidSet) {
        HE_CORE_ERROR("RTEffectPass[{}]: set0 描述符集分配失败", debugName);
        return false;
    }

    // ── 创建效果 RT 管线 + SBT（set0 仅此效果自己的布局）──
    m_Pipeline = std::make_unique<RTPass::RTEffectPipeline>(
        RTPass::CreateEffectPipeline(device, rtShaders, shaderGroups,
                                     {m_RayGenLayout}, pcRange,
                                     maxPayloadSize, maxRecursionDepth, debugName));
    if (!m_Pipeline->pipeline) {
        HE_CORE_ERROR("RTEffectPass[{}]: 效果管线创建失败", debugName);
        return false;
    }

    // ── 创建输出 UAV 纹理 ──
    rhi::TextureDesc d;
    d.format = outFormat;
    d.width  = m_Width; d.height = m_Height;
    d.usage  = outUsage;
    m_Output = device->CreateTexture(d);
    if (!m_Output) {
        HE_CORE_ERROR("RTEffectPass[{}]: 输出纹理创建失败", debugName);
        return false;
    }

    HE_CORE_INFO("RTEffectPass[{}]: 初始化完成 ({}x{})", debugName, m_Width, m_Height);
    return true;
}

void RTEffectPass::Shutdown() {
    m_Output.reset();
    m_LightUB.reset();
    m_Pipeline.reset();
    m_RayGenSet  = rhi::kInvalidSet;
    m_RayGenLayout = rhi::kInvalidLayout;
    m_Device = nullptr;
    m_Width = m_Height = 0;
    m_OutputWritten = false;
}

// ============================================================
// PrepareOutputUAV — 输出纹理 → UnorderedAccess（RT 写）
// 首帧从 Undefined 过渡（无历史访问）；后续帧从 ShaderResource 过渡
//（上一帧由 Lighting 采样结束）。UAV→ShaderResource 的反向转换由
// RenderGraph 依据 Lighting 的读取依赖自动生成。
// ============================================================
void RTEffectPass::PrepareOutputUAV(rhi::IRHICommandList* cmd) {
    if (!m_Output) return;
    if (m_OutputWritten) {
        cmd->PipelineBarrier(rhi::PipelineStage::FragmentShader, rhi::PipelineStage::RayTracingShader,
            rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess, m_Output.get());
    } else {
        cmd->PipelineBarrier(rhi::PipelineStage::TopOfPipe, rhi::PipelineStage::RayTracingShader,
            rhi::ResourceState::Undefined, rhi::ResourceState::UnorderedAccess, m_Output.get());
    }
    m_OutputWritten = true;
}

// ============================================================
// CreateHitLightUB — ClosestHit 光源 UBO（48B × 16 = 768B）
// ============================================================
bool RTEffectPass::CreateHitLightUB(rhi::IRHIDevice* device) {
    rhi::BufferDesc ub;
    ub.size  = sizeof(RTHitLightGPU) * 16;
    ub.usage = rhi::BufferUsage::Uniform;
    m_LightUB = device->CreateBuffer(ub);
    if (!m_LightUB) {
        HE_CORE_ERROR("RTEffectPass[{}]: ClosestHit 光源 UBO 创建失败", m_DebugName);
        return false;
    }
    return true;
}

// ============================================================
// FillHitLightUB — 从 GPULight[] SSBO 显式抽取命中点光源数据
// GPULight 为 64B/个，RTHitLightGPU 为 48B/个（紧密布局）
// ============================================================
void RTEffectPass::FillHitLightUB(const RTExecuteContext& ctx) {
    if (!m_LightUB || !ctx.lightBuffer) return;

    RTHitLightGPU* dst = static_cast<RTHitLightGPU*>(m_LightUB->Map());
    const GPULight* src = static_cast<const GPULight*>(ctx.lightBuffer->Map());
    if (!dst || !src) { if (dst) m_LightUB->Unmap(); if (src) ctx.lightBuffer->Unmap(); return; }

    u32 count = 0;
    const u32 maxCount = std::min(ctx.lightCount, 16u);
    for (u32 i = 0; i < maxCount; ++i) {
        if (src[i].colorIntensity.w <= 0.0f) continue;  // 跳过无效光源
        RTHitLightGPU& l = dst[count++];
        l.colorIntensity = src[i].colorIntensity;
        l.positionRange  = src[i].positionRange;
        l.directionType  = src[i].directionType;
    }
    // 剩余位清零（避免残留光源影响 ClosestHit 的 lightCount 循环边界外读取）
    for (u32 i = count; i < 16; ++i)
        dst[i] = RTHitLightGPU{};

    m_LightUB->Unmap();
    ctx.lightBuffer->Unmap();
}

// ============================================================
// BindAndTrace — 绑定效果管线 + 描述符集 + 发射光线
// ============================================================
void RTEffectPass::BindAndTrace(rhi::IRHICommandList* cmd, u32 w, u32 h,
                                rhi::DescriptorSetHandle set1, rhi::DescriptorSetHandle set2) {
    if (!m_Pipeline || !m_Pipeline->pipeline) return;

    // 绑定 RT 管线
    cmd->BindRTPipeline(m_Pipeline->pipeline.get());

    // 绑定描述符集（set0 效果必绑；set1/set2 场景资源可选）
    if (m_RayGenSet != rhi::kInvalidSet)
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_RayGenSet);
    if (set1 != rhi::kInvalidSet)
        cmd->BindDescriptorSet(rhi::kDescSetMaterial, set1);
    if (set2 != rhi::kInvalidSet)
        cmd->BindDescriptorSet(rhi::kDescSetBindless, set2);

    // 发射光线
    cmd->TraceRays(m_Pipeline->sbt, w, h, 1);
}

} // namespace he::render
