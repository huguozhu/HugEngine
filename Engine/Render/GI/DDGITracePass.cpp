// ============================================================
// DDGITracePass.cpp — DDGI 探针射线的硬件光追 march（任务 17 / B4 · M5.2-A）
// ============================================================
#include "GI/DDGITracePass.h"
#include "Core/Log.h"
#include "Pipeline/Material.h"   // ShaderTypes.slang（RTRayEffectPushConstant）

#include "DDGI_Trace.rgen.spv.h"
#include "RT_GI.rchit.spv.h"   // 命中点出射辐射度：与 RTGI 共用，保证两源量纲一致
#include "RT_GI.rmiss.spv.h"   // 未命中只置 a=-1，回退在 rgen 里做（它绑着 IBL）

#include <cstring>

namespace he::render {

bool DDGITracePass::Initialize(rhi::IRHIDevice* device) {
    if (!device) return false;
    m_Device = device;

    // ── set0 布局：前四个是 rgen 自己的，4/5/6 由 RT_HitCommon.slang（rchit）固定占用 ──
    std::vector<rhi::DescriptorSetLayoutBinding> bindings = {
        {kBindTLAS,     rhi::DescriptorType::AccelerationStructure, 1, rhi::kStageMaskRayGen},
        {kBindRadiance, rhi::DescriptorType::StorageBuffer,         1, rhi::kStageMaskRayGen},
        {kBindMaterial, rhi::DescriptorType::SampledImage,          1, rhi::kStageMaskClosestHit},
        {kBindLights,   rhi::DescriptorType::UniformBuffer,         1, rhi::kStageMaskClosestHit},
        {kBindNormals,  rhi::DescriptorType::SampledImage,          1, rhi::kStageMaskClosestHit},
        {kBindParams,   rhi::DescriptorType::UniformBuffer,         1, rhi::kStageMaskRayGen},
        {kBindIBL,      rhi::DescriptorType::CombinedImageSampler,  1, rhi::kStageMaskRayGen},
    };

    // push constant：与 RT_GI.rchit 共享的 RTRayEffectPushConstant（rchit 要读 lightCount）
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskRayGen | rhi::kStageMaskClosestHit;
    pc.size      = 128;

    std::vector<rhi::ShaderBytecode> shaders(3);
    shaders[0].stage = rhi::ShaderStage::RayGen;      shaders[0].spirv = k_DDGI_Trace_rgen_spv; shaders[0].entryPoint = "main";
    shaders[1].stage = rhi::ShaderStage::ClosestHit;  shaders[1].spirv = k_RT_GI_rchit_spv;     shaders[1].entryPoint = "main";
    shaders[2].stage = rhi::ShaderStage::Miss;        shaders[2].spirv = k_RT_GI_rmiss_spv;     shaders[2].entryPoint = "main";

    std::vector<rhi::RTShaderGroup> groups(3);
    groups[0].type = rhi::RTShaderGroupType::RayGen; groups[0].generalShader = 0;   groups[0].name = "DDGITraceRayGen";
    groups[1].type = rhi::RTShaderGroupType::Hit;    groups[1].closestHitShader = 1; groups[1].name = "DDGITraceClosestHit";
    groups[2].type = rhi::RTShaderGroupType::Miss;   groups[2].generalShader = 2;   groups[2].name = "DDGITraceMiss";

    // 命中点光源 UBO（rchit 用）
    if (!CreateHitLightUB(device)) {
        HE_CORE_WARN("DDGITracePass: ClosestHit 光源 UBO 创建失败");
        return false;
    }

    // 参数 UBO：每帧写入（网格/采样参数）
    rhi::BufferDesc ubDesc;
    ubDesc.size      = sizeof(TraceParams);
    ubDesc.usage     = rhi::BufferUsage::Uniform;
    ubDesc.cpuAccess = true;
    m_ParamsUBO = device->CreateBuffer(ubDesc);
    if (!m_ParamsUBO) {
        HE_CORE_WARN("DDGITracePass: 参数 UBO 创建失败");
        return false;
    }

    // 基类：创建 set0 + 效果管线 + SBT。输出纹理是 1x1 占位（本 pass 的输出是 SSBO，
    // 见头文件说明），因此不请求 UnorderedAccess —— 不需要 UAV 屏障。
    bool ok = RTEffectPass::Initialize(device, 1, 1,
        std::move(bindings), std::move(shaders), std::move(groups), pc,
        rhi::Format::RGBA16_FLOAT,
        rhi::TextureUsage::ShaderResource,
        rhi::kRTMaxPayloadSize, rhi::kRTMaxRecursionDepth, "DDGITrace");
    if (!ok) {
        HE_CORE_WARN("DDGITracePass: 管线/描述符集创建失败，DDGI 回退到 IBL 路径");
        return false;
    }

    // 参数 UBO 固定绑定（内容每帧变，绑定不变）
    device->UpdateDescriptorSet(m_RayGenSet, kBindParams, rhi::DescriptorType::UniformBuffer, m_ParamsUBO.get());
    HE_CORE_INFO("DDGITracePass: 初始化完成（探针射线光追 march）");
    return true;
}

bool DDGITracePass::EnsureCapacity(u32 probeCount, u32 samplesPerProbe) {
    m_SamplesPerProbe = (samplesPerProbe > 0u) ? samplesPerProbe : 1u;
    if (probeCount == 0u || !m_Device) return false;
    m_RayCount = probeCount * m_SamplesPerProbe;

    // 探针数变了才重建（网格拟合会把 8x4x8 换成 16x8x11，只发生一次）
    if (probeCount != m_ProbeCapacity) {
        rhi::BufferDesc bd;
        bd.size  = (u64)m_RayCount * sizeof(float4);
        bd.usage = rhi::BufferUsage::Storage;
        m_Radiance = m_Device->CreateBuffer(bd);
        if (!m_Radiance) {
            HE_CORE_WARN("DDGITracePass: 辐射度缓冲创建失败（{} 条射线）", m_RayCount);
            m_ProbeCapacity = 0;
            return false;
        }
        m_ProbeCapacity = probeCount;
        HE_CORE_INFO("DDGITracePass: 辐射度缓冲重建（{} 探针 × {} 采样 = {} 条射线，{} KB）",
                     probeCount, m_SamplesPerProbe, m_RayCount,
                     (m_RayCount * 16ull) / 1024ull);
    }
    if (m_Radiance) {
        m_Device->UpdateDescriptorSet(m_RayGenSet, kBindRadiance,
            rhi::DescriptorType::StorageBuffer, m_Radiance.get());
    }
    return m_Radiance != nullptr;
}

void DDGITracePass::SetGrid(const float3& origin, u32 countX, u32 countY, u32 countZ, float cellSize) {
    m_Params.gridOrigin = float4(origin, 0.0f);
    m_Params.gridSize   = float4(float(countX), float(countY), float(countZ), cellSize);
    m_ParamsDirty = true;
}

void DDGITracePass::SetSampling(u32 samplesPerProbe, float maxDistance, float stepRatio) {
    m_SamplesPerProbe = (samplesPerProbe > 0u) ? samplesPerProbe : 1u;
    m_MaxDistance = maxDistance;
    m_StepRatio   = stepRatio;
    m_ParamsDirty = true;
}

void DDGITracePass::SetIBL(rhi::IRHITexture* irradiance, rhi::IRHISampler* sampler) {
    m_IBLIrradiance = irradiance;
    m_IBLSampler    = sampler;
    if (m_Device && irradiance && sampler) {
        m_Device->UpdateDescriptorSet(m_RayGenSet, kBindIBL,
            rhi::DescriptorType::CombinedImageSampler, irradiance, sampler);
    }
}

void DDGITracePass::Execute(rhi::IRHICommandList* cmd,
                            rhi::IRHIAccelerationStructure* tlas,
                            const RTExecuteContext& ctx) {
    if (!IsValid() || !tlas || !cmd || m_RayCount == 0u) return;

    m_Device->UpdateDescriptorSet(m_RayGenSet, kBindTLAS,
        rhi::DescriptorType::AccelerationStructure, tlas);
    if (ctx.sceneMaterialTex)
        m_Device->UpdateDescriptorSet(m_RayGenSet, kBindMaterial,
            rhi::DescriptorType::SampledImage, ctx.sceneMaterialTex, nullptr);
    if (m_LightUB)
        m_Device->UpdateDescriptorSet(m_RayGenSet, kBindLights,
            rhi::DescriptorType::UniformBuffer, m_LightUB.get());
    if (ctx.sceneTriangleNormals)
        m_Device->UpdateDescriptorSet(m_RayGenSet, kBindNormals,
            rhi::DescriptorType::SampledImage, ctx.sceneTriangleNormals, nullptr);

    // 参数 UBO（网格/采样）
    if (m_ParamsDirty) {
        m_Params.params    = float4(float(m_SamplesPerProbe), m_MaxDistance, m_StepRatio, 0.0f);
        m_Params.probeStep = float4(m_StepRatio, 0.0f, 0.0f, 0.0f);
        void* mapped = m_ParamsUBO->Map();
        if (mapped) {
            memcpy(mapped, &m_Params, sizeof(TraceParams));
            m_ParamsUBO->Unmap();
        }
        m_ParamsDirty = false;
    }

    FillHitLightUB(ctx);

    RTRayEffectPushConstant pc{};
    pc.cameraPos    = float4(ctx.cameraPos, 0.0f);
    pc.dispatchDimX = m_RayCount;
    pc.dispatchDimY = 1;
    pc.frameIndex   = ctx.frameIndex;
    pc.maxDistance  = m_MaxDistance;
    pc.sampleCount  = m_SamplesPerProbe;
    pc.lightCount   = ctx.lightCount;

    // 先绑管线再推常量（push constant 用当前绑定布局，见 RTEffectPass 的说明）
    BindRT(cmd);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    TraceRays(cmd, m_RayCount, 1);

    // 射线写入 → DDGI 的 compute 读取：缓冲区不在帧图资源表里，屏障必须显式给
    cmd->PipelineBarrier(
        rhi::PipelineStage::RayTracingShader,
        rhi::PipelineStage::ComputeShader,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::ShaderResource);
}

} // namespace he::render
