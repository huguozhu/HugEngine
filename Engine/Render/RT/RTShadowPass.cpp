// ============================================================
// RTShadowPass.cpp — 硬件 Ray Tracing 阴影 Pass 实现
// ============================================================
#include "RT/RTShadowPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
// 通过 Material.h 引入 ShaderTypes.slang（其内部先包含 Math.h，保证 float4 等类型可用）
#include "Pipeline/Material.h"  // RTShadowPushConstant / GPULight（C++/Slang 共享）
// RT 质量参数 CVar（r.RT.*，运行时热更新 SPP / 追踪距离）
#include "Pipeline/RTQualityCVars.h"

// RT 着色器 SPIR-V
#include "RT_Shadow.rgen.spv.h"
#include "RT_Shadow.rahit.spv.h"
#include "RT_Common.rmiss.spv.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <algorithm>  // std::max / std::clamp

namespace he::render {

// 阴影光源结构（GPU 侧，64B/个，与 RT_Shadow.rgen.slang 的 ShadowLight 一致）
struct ShadowLightGPU {
    float4 pos_type;      // xyz=位置/方向, w=类型(0=Dir,1=Point,2=Spot)
    float4 color_radius;  // rgb=颜色, w=光源半径（软阴影预留）
    float4 spotDir_angle; // xyz=聚光方向, w=cos(内锥角)
    float4 radius;        // x=光源半径（软阴影）
};
static_assert(sizeof(ShadowLightGPU) == 64, "ShadowLightGPU must be 64 bytes");

static constexpr u32 kShadowMaxLights = 16;

bool RTShadowPass::Initialize(rhi::IRHIDevice* device, u32 fullWidth, u32 fullHeight,
                              bool halfRes) {
    m_FullWidth  = fullWidth;
    m_FullHeight = fullHeight;
    m_HalfRes    = halfRes;
    u32 w = halfRes ? (fullWidth + 1) / 2 : fullWidth;
    u32 h = halfRes ? (fullHeight + 1) / 2 : fullHeight;

    // ── set0 RayGen 资源绑定 ──
    // b0=TLAS, b1=ShadowMask(StorageImage), b2=GBDepth, b3=GBNormal, b4=LightUB
    std::vector<rhi::DescriptorSetLayoutBinding> bindings = {
        {0, rhi::DescriptorType::AccelerationStructure, 1, rhi::kStageMaskRayGen},
        {1, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskRayGen},
        {2, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskRayGen},
        {3, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskRayGen},
        {4, rhi::DescriptorType::UniformBuffer, 1, rhi::kStageMaskRayGen},
    };

    // ── push constant 范围（RayGen 阶段，128B）──
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskRayGen;
    pc.size      = 128;

    // ── 着色器：RayGen + AnyHit + Miss ──
    std::vector<rhi::ShaderBytecode> shaders;
    {
        rhi::ShaderBytecode bc;
        bc.stage = rhi::ShaderStage::RayGen;
        bc.spirv = k_RT_Shadow_rgen_spv; bc.entryPoint = "main";
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
        rg.generalShader = 0; rg.name = "ShadowRayGen";
        groups.push_back(rg);
    }
    {
        rhi::RTShaderGroup hg;
        hg.type = rhi::RTShaderGroupType::Hit;
        hg.anyHitShader = 1; hg.name = "ShadowAnyHit";
        groups.push_back(hg);
    }
    {
        rhi::RTShaderGroup mg;
        mg.type = rhi::RTShaderGroupType::Miss;
        mg.generalShader = 2; mg.name = "ShadowMiss";
        groups.push_back(mg);
    }

    // ── 创建阴影光源 Uniform Buffer（48B × 16 = 768B）──
    {
        rhi::BufferDesc ub;
        ub.size  = sizeof(ShadowLightGPU) * kShadowMaxLights;
        ub.usage = rhi::BufferUsage::Uniform;
        m_LightUB = device->CreateBuffer(ub);
        if (!m_LightUB) {
            HE_CORE_ERROR("RTShadowPass: 光源 UB 创建失败");
            return false;
        }
    }

    // ── 调用基类：创建 set0 + 效果管线 + 输出纹理 ──
    bool ok = RTEffectPass::Initialize(device, w, h,
        std::move(bindings), std::move(shaders), std::move(groups), pc,
        rhi::Format::R16_FLOAT,
        rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource,
        rhi::kRTMaxPayloadSize, rhi::kRTMaxRecursionDepth, "RTShadow");
    if (!ok) return false;

    HE_CORE_INFO("RTShadowPass: 初始化完成 ({}x{}, halfRes={})", w, h, halfRes);
    return true;
}

// ============================================================
// Execute — 每帧执行阴影 Pass
// ============================================================
void RTShadowPass::Execute(rhi::IRHICommandList* cmd,
                           rhi::IRHIAccelerationStructure* tlas,
                           const RTExecuteContext& ctx) {
    if (!IsValid() || !tlas || !m_Output || !m_Device) return;

    // ── 预屏障：输出纹理 → UnorderedAccess（RT 写，首帧 Undefined → UAV）──
    PrepareOutputUAV(cmd);

    // ── 更新 set0 描述符 ──
    m_Device->UpdateDescriptorSet(m_RayGenSet, 0,
        rhi::DescriptorType::AccelerationStructure, tlas);
    // StorageImage 用原生 ImageView 绑定（与 RTPass 的 backbuffer 模式一致）
    m_Device->UpdateDescriptorSetWithImageView(m_RayGenSet, 1,
        rhi::DescriptorType::StorageImage, m_Output->GetNativeHandle());
    if (ctx.gbDepth)
        m_Device->UpdateDescriptorSet(m_RayGenSet, 2,
            rhi::DescriptorType::SampledImage, ctx.gbDepth, nullptr);
    if (ctx.gbNormal)
        m_Device->UpdateDescriptorSet(m_RayGenSet, 3,
            rhi::DescriptorType::SampledImage, ctx.gbNormal, nullptr);
    if (m_LightUB)
        m_Device->UpdateDescriptorSet(m_RayGenSet, 4,
            rhi::DescriptorType::UniformBuffer, m_LightUB.get());

    // ── 填充阴影光源数据（显式抽取 GPULight[] → ShadowLight[16]）──
    FillLightBuffer(ctx);

    // ── 设置 push constants（RTShadowPushConstant 共享结构）──
    RTShadowPushConstant pc{};
    memcpy(&pc.invViewProj, glm::value_ptr(ctx.invViewProj), sizeof(float) * 16);
    pc.cameraPos    = float4(ctx.cameraPos, 0.0f);
    pc.dispatchDimX = m_Width;
    pc.dispatchDimY = m_Height;
    pc.frameIdx     = ctx.frameIndex;
    pc.maxShadowDist = std::max(cvRTShadowMaxDist.Get(), 0.01f);  // 阴影最大追踪距离（CVar 热更新）
    pc.softSPP      = std::clamp(cvRTShadowSPP.Get(), 1, 16);         // 软阴影每光源采样数
    pc.shadowFlags  = (m_HalfRes ? 2u : 0u)
                    | (cvRTShadowSoft.Get() ? 4u : 0u);               // bit1=半分辨率, bit2=软阴影
    pc.lightCount   = ctx.lightCount;

    // ── 先绑 RT 管线（设置正确 push constant 布局），再推常量，最后发射光线 ──
    //（若先推常量，会应用到上一 Pass 的布局——降噪等图形 Pass 范围不匹配 → 写入失败）
    BindRT(cmd);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    TraceRays(cmd, m_Width, m_Height);
    // 输出纹理的 UAV → ShaderResource 转换由 RenderGraph 依据 Lighting 对
    // "RT_ShadowMask" 的读取依赖自动生成屏障，此处不再重复转换——否则双重
    // 屏障会因 oldLayout 不匹配触发 Validation 报错（VUID-VkImageMemoryBarrier-oldLayout-01197）。
}

// ============================================================
// FillLightBuffer — 从 GPULight[] SSBO 抽取阴影光源数据
// ============================================================
void RTShadowPass::FillLightBuffer(const RTExecuteContext& ctx) {
    if (!m_LightUB || !ctx.lightBuffer) return;

    ShadowLightGPU* dst = static_cast<ShadowLightGPU*>(m_LightUB->Map());
    const GPULight* src = static_cast<const GPULight*>(ctx.lightBuffer->Map());
    if (!dst || !src) { if (dst) m_LightUB->Unmap(); if (src) ctx.lightBuffer->Unmap(); return; }

    u32 count = 0;
    const u32 maxCount = std::min(ctx.lightCount, kShadowMaxLights);
    for (u32 i = 0; i < maxCount; ++i) {
        if (src[i].colorIntensity.w <= 0.0f) continue;  // 跳过无效光源
        ShadowLightGPU& sl = dst[count++];
        sl.pos_type     = src[i].directionType;
        sl.color_radius = src[i].colorIntensity;
        // 避免 GLM swizzle（.xyz），用 vec3(vec4) 构造提取前三分量
        sl.spotDir_angle = float4(glm::vec3(src[i].directionType), src[i].coneAngles.x);
        sl.radius.x = src[i].shadowRadius;  // 光源半径（软阴影）
    }

    m_LightUB->Unmap();
    ctx.lightBuffer->Unmap();
}

} // namespace he::render
