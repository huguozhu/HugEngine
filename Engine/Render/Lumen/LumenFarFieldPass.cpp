// ============================================================
// LumenFarFieldPass.cpp — Lumen 远场硬件光追（步骤 26）
// ============================================================
#include "Lumen/LumenFarFieldPass.h"

#include "Core/Log.h"
#include "Pipeline/Material.h"   // RTRayEffectPushConstant（与 rchit 共享）

#include "Lumen_FarField.rgen.spv.h"
#include "RT_GI.rchit.spv.h"
#include "RT_GI.rmiss.spv.h"

#include <cstring>

namespace he::render {

bool LumenFarFieldPass::Initialize(rhi::IRHIDevice* device) {
    if (!device) return false;
    m_Device = device;

    // b0 = TLAS, b1 = 命中结果 SSBO, b2 = 探针缓冲（rgen 用）
    // b4/b5/b6 由 RT_HitCommon.slang（rchit）固定占用：材质纹理 / 命中点光源 / 三角形法线
    // b7 = 本 pass 参数 UBO
    std::vector<rhi::DescriptorSetLayoutBinding> bindings = {
        {kBindTLAS,     rhi::DescriptorType::AccelerationStructure, 1, rhi::kStageMaskRayGen},
        {kBindResult,   rhi::DescriptorType::StorageBuffer,         1, rhi::kStageMaskRayGen},
        {kBindProbes,   rhi::DescriptorType::StorageBuffer,         1, rhi::kStageMaskRayGen},
        {kBindMaterial, rhi::DescriptorType::SampledImage,          1, rhi::kStageMaskClosestHit},
        {kBindLights,   rhi::DescriptorType::UniformBuffer,         1, rhi::kStageMaskClosestHit},
        {kBindNormals,  rhi::DescriptorType::SampledImage,          1, rhi::kStageMaskClosestHit},
        {kBindParams,   rhi::DescriptorType::UniformBuffer,         1, rhi::kStageMaskRayGen},
    };

    // push constant 与 RT_GI.rchit 共享（命中着色要读 lightCount / flags）
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskRayGen | rhi::kStageMaskClosestHit;
    pc.size      = 128;

    std::vector<rhi::ShaderBytecode> shaders(3);
    shaders[0].stage = rhi::ShaderStage::RayGen;     shaders[0].spirv = k_Lumen_FarField_rgen_spv; shaders[0].entryPoint = "main";
    shaders[1].stage = rhi::ShaderStage::ClosestHit; shaders[1].spirv = k_RT_GI_rchit_spv;         shaders[1].entryPoint = "main";
    shaders[2].stage = rhi::ShaderStage::Miss;       shaders[2].spirv = k_RT_GI_rmiss_spv;         shaders[2].entryPoint = "main";

    std::vector<rhi::RTShaderGroup> groups(3);
    groups[0].type = rhi::RTShaderGroupType::RayGen; groups[0].generalShader = 0;    groups[0].name = "LumenFarFieldRayGen";
    groups[1].type = rhi::RTShaderGroupType::Hit;    groups[1].closestHitShader = 1; groups[1].name = "LumenFarFieldClosestHit";
    groups[2].type = rhi::RTShaderGroupType::Miss;   groups[2].generalShader = 2;    groups[2].name = "LumenFarFieldMiss";

    if (!CreateHitLightUB(device)) {
        HE_CORE_WARN("LumenFarFieldPass: 命中点光源 UBO 创建失败，远场光追不可用");
        return false;
    }

    rhi::BufferDesc ubDesc;
    ubDesc.size      = sizeof(FarFieldParams);
    ubDesc.usage     = rhi::BufferUsage::Uniform;
    ubDesc.cpuAccess = true;
    m_ParamsUBO = device->CreateBuffer(ubDesc);
    if (!m_ParamsUBO) {
        HE_CORE_WARN("LumenFarFieldPass: 参数 UBO 创建失败");
        return false;
    }

    // 基类：1×1 占位输出纹理（本 pass 的输出是 SSBO），只要 ShaderResource 用途 ⇒ 不需要 UAV 屏障
    const bool ok = RTEffectPass::Initialize(device, 1, 1,
        std::move(bindings), std::move(shaders), std::move(groups), pc,
        rhi::Format::RGBA16_FLOAT, rhi::TextureUsage::ShaderResource,
        rhi::kRTMaxPayloadSize, rhi::kRTMaxRecursionDepth, "LumenFarField");
    if (!ok) {
        HE_CORE_WARN("LumenFarFieldPass: 管线/SBT 创建失败，远场光追不可用（近场 SDF 不受影响）");
        return false;
    }

    device->UpdateDescriptorSet(m_RayGenSet, kBindParams, rhi::DescriptorType::UniformBuffer, m_ParamsUBO.get());
    HE_CORE_INFO("LumenFarFieldPass: 初始化完成（Lumen 远场硬件光追，复用 RT_GI 命中着色）");
    return true;
}

bool LumenFarFieldPass::EnsureCapacity(u32 rayCount) {
    if (rayCount == 0u || !m_Device) return false;
    if (rayCount <= m_Capacity && m_Result) { m_RayCount = rayCount; return true; }

    rhi::BufferDesc bd;
    // **每条光线两个 float4**（[2i] = 命中距离/标志/亮度，[2i+1] = 命中点）——rgen 就是这么写的。
    // 第一版只按 1 个 float4/光线分配，于是 rgen 写出界、踩坏相邻显存：症状是"RT 命中率从 71% 掉到 44%"
    // 这类完全说不通的变化（不是确定性差异，是越界写）。
    bd.size      = (u64)rayCount * 2u * sizeof(float4);
    bd.usage     = rhi::BufferUsage::Storage;
    // 直接映射读回：验收要做"SDF 命中 vs 三角形命中"的逐光线对照，回读是最短路径
    bd.cpuAccess = true;
    m_Result = m_Device->CreateBuffer(bd);
    if (!m_Result) {
        HE_CORE_WARN("LumenFarFieldPass: 结果缓冲创建失败（{} 条射线）", rayCount);
        return false;
    }
    m_ResultMapped = m_Result->Map();
    m_Capacity     = rayCount;
    m_RayCount     = rayCount;
    m_Device->UpdateDescriptorSet(m_RayGenSet, kBindResult, rhi::DescriptorType::StorageBuffer, m_Result.get());
    return true;
}

u32 LumenFarFieldPass::Trace(rhi::IRHICommandList* cmd, rhi::IRHIAccelerationStructure* tlas,
                             const FrameParams& p) {
    // 【别用 IsValid()】它要求 `结果缓冲非空`，而结果缓冲正是下面 EnsureCapacity 才建的 ——
    // 用 IsValid() 当门会让第一次 Trace 直接返回，于是缓冲永远建不出来（死锁）。
    if (!cmd || !tlas || !RTEffectPass::IsValid() || !p.probeBuffer || p.probeCount == 0u) return 0u;

    const u32 rayCount = p.probeCount * p.traceRep;
    if (!EnsureCapacity(rayCount)) return 0u;

    m_Device->UpdateDescriptorSet(m_RayGenSet, kBindTLAS, rhi::DescriptorType::AccelerationStructure, tlas);
    m_Device->UpdateDescriptorSet(m_RayGenSet, kBindProbes, rhi::DescriptorType::StorageBuffer, p.probeBuffer);
    if (p.materialTex)
        m_Device->UpdateDescriptorSet(m_RayGenSet, kBindMaterial, rhi::DescriptorType::SampledImage,
                                      p.materialTex, nullptr);
    if (p.triangleNorms)
        m_Device->UpdateDescriptorSet(m_RayGenSet, kBindNormals, rhi::DescriptorType::SampledImage,
                                      p.triangleNorms, nullptr);
    if (m_LightUB)
        m_Device->UpdateDescriptorSet(m_RayGenSet, kBindLights, rhi::DescriptorType::UniformBuffer,
                                      m_LightUB.get());

    // 参数 UBO（内容每帧变，绑定不变）
    FarFieldParams fp{};
    fp.probeCount   = p.probeCount;
    fp.traceRep     = p.traceRep;
    fp.seed         = p.seed;
    fp.sampleMode   = p.sampleMode;
    fp.tMin         = p.tMin;
    fp.tMax         = p.tMax;
    fp.farThreshold = p.farThreshold;
    if (void* mapped = m_ParamsUBO->Map()) {
        std::memcpy(mapped, &fp, sizeof(fp));
        m_ParamsUBO->Unmap();
    }

    // 命中点光源 UBO（rchit 直接光）：从 GPULight[] 抽取，与 RTGI 同一套
    {
        RTExecuteContext lightCtx;
        lightCtx.lightBuffer = p.lightBuffer;
        lightCtx.lightCount  = p.lightCount;
        FillHitLightUB(lightCtx);
    }

    RTRayEffectPushConstant pc{};
    pc.frameIndex  = p.seed;
    pc.maxDistance = p.farThreshold;   // rchit 不读它；这里只保留语义
    pc.sampleCount = p.traceRep;
    pc.flags       = (p.lightCount == 0u) ? 4u : 0u;   // bit2 = 白炉/无光源时走理想值（与 RTGI 约定一致）
    pc.lightCount  = p.lightCount;

    BindRT(cmd);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    TraceRays(cmd, rayCount, 1);
    return rayCount;
}

void LumenFarFieldPass::Execute(rhi::IRHICommandList* /*cmd*/, rhi::IRHIAccelerationStructure* /*tlas*/,
                                const RTExecuteContext& /*ctx*/) {
    // 本 pass 不挂 GI Provider：由 `LumenScene::RunFarFieldRT` 直接驱动（它的输入是探针缓冲，
    // 而不是 GBuffer 像素）。若有人把它当 RT 效果源注册进来，这里显式告警而不是静默不干活。
    if (!m_WarnedExecute) {
        HE_CORE_WARN("LumenFarFieldPass::Execute 被调用：本 pass 不挂 Provider，请用 Trace() 驱动");
        m_WarnedExecute = true;
    }
}

} // namespace he::render
