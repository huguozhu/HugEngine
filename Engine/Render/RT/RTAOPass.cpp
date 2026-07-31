// ============================================================
// RTAOPass.cpp — 硬件 Ray Tracing 环境光遮蔽 Pass 实现
// ============================================================
#include "RT/RTAOPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
// 通过 Material.h 引入 ShaderTypes.slang（RTRayEffectPushConstant 等共享结构）
#include "Pipeline/Material.h"

// RT 着色器 SPIR-V
#include "RT_AO.rgen.spv.h"
#include "RT_Shadow.rahit.spv.h"
#include "RT_Common.rmiss.spv.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstring>

namespace he::render {

bool RTAOPass::Initialize(rhi::IRHIDevice* device, u32 fullWidth, u32 fullHeight,
                          bool halfRes) {
    m_FullWidth  = fullWidth;
    m_FullHeight = fullHeight;
    m_HalfRes    = halfRes;
    u32 w = halfRes ? (fullWidth + 1) / 2 : fullWidth;
    u32 h = halfRes ? (fullHeight + 1) / 2 : fullHeight;

    // ── set0 RayGen 资源绑定 ──
    // b0=TLAS, b1=AOMask(StorageImage), b2=GBDepth, b3=GBNormal
    std::vector<rhi::DescriptorSetLayoutBinding> bindings = {
        {0, rhi::DescriptorType::AccelerationStructure, 1, rhi::kStageMaskRayGen},
        {1, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskRayGen},
        {2, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskRayGen},
        {3, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskRayGen},
    };

    // ── push constant 范围（RayGen 阶段，128B）──
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskRayGen;
    pc.size      = 128;

    // ── 着色器：RayGen + AnyHit（复用阴影）+ Miss（复用通用）──
    std::vector<rhi::ShaderBytecode> shaders;
    {
        rhi::ShaderBytecode bc;
        bc.stage = rhi::ShaderStage::RayGen;
        bc.spirv = k_RT_AO_rgen_spv; bc.entryPoint = "main";
        shaders.push_back(bc);
    }
    {
        rhi::ShaderBytecode bc;
        bc.stage = rhi::ShaderStage::AnyHit;
        bc.spirv = k_RT_Shadow_rahit_spv; bc.entryPoint = "main";
        shaders.push_back(bc);
    }
    {
        rhi::ShaderBytecode bc;
        bc.stage = rhi::ShaderStage::Miss;
        bc.spirv = k_RT_Common_rmiss_spv; bc.entryPoint = "main";
        shaders.push_back(bc);
    }

    // ── shader groups：RayGen(0) + Hit(AnyHit=1) + Miss(2) ──
    std::vector<rhi::RTShaderGroup> groups;
    {
        rhi::RTShaderGroup rg;
        rg.type = rhi::RTShaderGroupType::RayGen;
        rg.generalShader = 0; rg.name = "AORayGen";
        groups.push_back(rg);
    }
    {
        rhi::RTShaderGroup hg;
        hg.type = rhi::RTShaderGroupType::Hit;
        hg.anyHitShader = 1; hg.name = "AOAnyHit";
        groups.push_back(hg);
    }
    {
        rhi::RTShaderGroup mg;
        mg.type = rhi::RTShaderGroupType::Miss;
        mg.generalShader = 2; mg.name = "AOMiss";
        groups.push_back(mg);
    }

    // ── 调用基类：创建 set0 + 效果管线 + 输出纹理 ──
    bool ok = RTEffectPass::Initialize(device, w, h,
        std::move(bindings), std::move(shaders), std::move(groups), pc,
        rhi::Format::R8_UNORM,
        rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource,
        rhi::kRTMaxPayloadSize, rhi::kRTMaxRecursionDepth, "RTAO");
    if (!ok) return false;

    HE_CORE_INFO("RTAOPass: 初始化完成 ({}x{}, halfRes={})", w, h, halfRes);
    return true;
}

// ============================================================
// Execute — 每帧执行 AO Pass
// ============================================================
void RTAOPass::Execute(rhi::IRHICommandList* cmd,
                       rhi::IRHIAccelerationStructure* tlas,
                       const RTExecuteContext& ctx) {
    if (!IsValid() || !tlas || !m_Output || !m_Device) return;

    // ── 预屏障：输出纹理 → UnorderedAccess（RT 写）──
    PrepareOutputUAV(cmd);

    // ── 更新 set0 描述符 ──
    m_Device->UpdateDescriptorSet(m_RayGenSet, 0,
        rhi::DescriptorType::AccelerationStructure, tlas);
    m_Device->UpdateDescriptorSetWithImageView(m_RayGenSet, 1,
        rhi::DescriptorType::StorageImage, m_Output->GetNativeHandle());
    if (ctx.gbDepth)
        m_Device->UpdateDescriptorSet(m_RayGenSet, 2,
            rhi::DescriptorType::SampledImage, ctx.gbDepth, nullptr);
    if (ctx.gbNormal)
        m_Device->UpdateDescriptorSet(m_RayGenSet, 3,
            rhi::DescriptorType::SampledImage, ctx.gbNormal, nullptr);

    // ── 设置 push constants（RTRayEffectPushConstant 共享结构）──
    RTRayEffectPushConstant pc{};
    memcpy(&pc.invViewProj, glm::value_ptr(ctx.invViewProj), sizeof(float) * 16);
    pc.cameraPos    = float4(ctx.cameraPos, 0.0f);
    pc.dispatchDimX = m_Width;
    pc.dispatchDimY = m_Height;
    pc.frameIndex   = ctx.frameIndex;
    pc.maxDistance  = 2.0f;      // AO 遮蔽半径（m）
    pc.sampleCount  = 2;         // 每像素射线数（时域累积提升质量）
    pc.flags        = m_HalfRes ? 1u : 0u;  // bit0=半分辨率
    pc.lightCount   = 0;
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    // ── 绑定管线 + set0 + TraceRays ──
    BindAndTrace(cmd, m_Width, m_Height);
}

} // namespace he::render
