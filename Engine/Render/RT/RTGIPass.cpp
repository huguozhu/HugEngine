// ============================================================
// RTGIPass.cpp — 硬件 Ray Tracing 间接漫反射 Pass 实现
// ============================================================
#include "RT/RTGIPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
// 通过 Material.h 引入 ShaderTypes.slang（RTRayEffectPushConstant 等共享结构）
#include "Pipeline/Material.h"

// RT 着色器 SPIR-V
#include "RT_GI.rgen.spv.h"
#include "RT_GI.rchit.spv.h"
#include "RT_GI.rmiss.spv.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstring>

namespace he::render {

bool RTGIPass::Initialize(rhi::IRHIDevice* device, u32 fullWidth, u32 fullHeight,
                          bool quarterRes) {
    m_FullWidth  = fullWidth;
    m_FullHeight = fullHeight;
    m_QuarterRes = quarterRes;
    u32 w = quarterRes ? (fullWidth + 3) / 4 : fullWidth;
    u32 h = quarterRes ? (fullHeight + 3) / 4 : fullHeight;

    // ── set0 资源绑定（与 RTReflectionPass 相同的场景数据布局）──
    // b0=TLAS(RG), b1=Output(RG), b2=GBDepth(RG), b3=GBNormal(RG),
    // b4=材质纹理(CH), b5=光源UB(CH), b6=三角形法线纹理(CH)
    std::vector<rhi::DescriptorSetLayoutBinding> bindings = {
        {0, rhi::DescriptorType::AccelerationStructure, 1, rhi::kStageMaskRayGen},
        {1, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskRayGen},
        {2, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskRayGen},
        {3, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskRayGen},
        {4, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskClosestHit},  // 场景材质纹理
        {5, rhi::DescriptorType::UniformBuffer, 1, rhi::kStageMaskClosestHit}, // 命中点光源
        {6, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskClosestHit},  // 三角形顶点法线纹理
    };

    // ── push constant 范围（RayGen 深度重建 + ClosestHit 光照计数）──
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskRayGen | rhi::kStageMaskClosestHit;
    pc.size      = 128;

    // ── 着色器：RayGen + ClosestHit + Miss ──
    std::vector<rhi::ShaderBytecode> shaders;
    {
        rhi::ShaderBytecode bc;
        bc.stage = rhi::ShaderStage::RayGen;
        bc.spirv = k_RT_GI_rgen_spv; bc.entryPoint = "main";
        shaders.push_back(bc);
    }
    {
        rhi::ShaderBytecode bc;
        bc.stage = rhi::ShaderStage::ClosestHit;
        bc.spirv = k_RT_GI_rchit_spv; bc.entryPoint = "main";
        shaders.push_back(bc);
    }
    {
        rhi::ShaderBytecode bc;
        bc.stage = rhi::ShaderStage::Miss;
        bc.spirv = k_RT_GI_rmiss_spv; bc.entryPoint = "main";
        shaders.push_back(bc);
    }

    // ── shader groups：RayGen(0) + Hit(ClosestHit=1) + Miss(2) ──
    std::vector<rhi::RTShaderGroup> groups;
    {
        rhi::RTShaderGroup rg;
        rg.type = rhi::RTShaderGroupType::RayGen;
        rg.generalShader = 0; rg.name = "GIRayGen";
        groups.push_back(rg);
    }
    {
        rhi::RTShaderGroup hg;
        hg.type = rhi::RTShaderGroupType::Hit;
        hg.closestHitShader = 1; hg.name = "GIClosestHit";
        groups.push_back(hg);
    }
    {
        rhi::RTShaderGroup mg;
        mg.type = rhi::RTShaderGroupType::Miss;
        mg.generalShader = 2; mg.name = "GIMiss";
        groups.push_back(mg);
    }

    // ── ClosestHit 光源 UBO ──
    if (!CreateHitLightUB(device)) return false;

    // ── 调用基类：创建 set0 + 效果管线 + 输出纹理 ──
    bool ok = RTEffectPass::Initialize(device, w, h,
        std::move(bindings), std::move(shaders), std::move(groups), pc,
        rhi::Format::RGBA16_FLOAT,
        rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource,
        rhi::kRTMaxPayloadSize, rhi::kRTMaxRecursionDepth, "RTGI");
    if (!ok) return false;

    HE_CORE_INFO("RTGIPass: 初始化完成 ({}x{}, quarterRes={})", w, h, quarterRes);
    return true;
}

// ============================================================
// Execute — 每帧执行 GI Pass
// ============================================================
void RTGIPass::Execute(rhi::IRHICommandList* cmd,
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
    if (ctx.sceneMaterialTex)
        m_Device->UpdateDescriptorSet(m_RayGenSet, 4,
            rhi::DescriptorType::SampledImage, ctx.sceneMaterialTex, nullptr);
    if (m_LightUB)
        m_Device->UpdateDescriptorSet(m_RayGenSet, 5,
            rhi::DescriptorType::UniformBuffer, m_LightUB.get());
    if (ctx.sceneTriangleNormals)
        m_Device->UpdateDescriptorSet(m_RayGenSet, 6,
            rhi::DescriptorType::SampledImage, ctx.sceneTriangleNormals, nullptr);

    // ── 填充 ClosestHit 光源数据 ──
    FillHitLightUB(ctx);

    // ── 设置 push constants（RTRayEffectPushConstant 共享结构）──
    RTRayEffectPushConstant pc{};
    memcpy(&pc.invViewProj, glm::value_ptr(ctx.invViewProj), sizeof(float) * 16);
    pc.cameraPos    = float4(ctx.cameraPos, 0.0f);
    pc.dispatchDimX = m_Width;
    pc.dispatchDimY = m_Height;
    pc.frameIndex   = ctx.frameIndex;
    pc.maxDistance  = 30.0f;    // GI 追踪范围（m）
    pc.sampleCount  = 1;        // SPP（时域累积提升质量）
    pc.flags        = m_QuarterRes ? 1u : 0u;  // bit0=四分之一分辨率
    pc.lightCount   = ctx.lightCount;
    // ── 先绑 RT 管线（设置正确 push constant 布局），再推常量，最后发射光线 ──
    //（若先推常量，会应用到上一 Pass 的布局——降噪等图形 Pass 范围不匹配 → 写入失败）
    BindRT(cmd);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    TraceRays(cmd, m_Width, m_Height);
}

} // namespace he::render
