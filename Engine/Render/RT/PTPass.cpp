// ============================================================
// PTPass.cpp — 全路径追踪 Pass 实现
// 迭代式路径追踪 RayGen 直接渲染整帧，输出 HDR + GBuffer（深度/法线/速度）
// ============================================================
#include "RT/PTPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
// 通过 Material.h 引入 ShaderTypes.slang（PTPushConstant / GPULight / PTReservoir）
#include "Pipeline/Material.h"

// RT 着色器 SPIR-V
#include "PT_Full.rgen.spv.h"
#include "PT_Full.rchit.spv.h"
#include "PT_Full.rmiss.spv.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <algorithm>

namespace he::render {

// ============================================================
// Initialize — 创建 set0 + 效果管线 + 4 张输出纹理
// ============================================================
bool PTPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    // ── set0 资源绑定（10 项）──
    // b0=TLAS(RG), b1..b4=四输出 UAV(RG), b5=光源 SSBO(RG),
    // b6=材质纹理(CH), b7=三角形法线(CH), b8=FinalReservoir SSBO(RG), b9=albedoMetallic UAV(RG)
    std::vector<rhi::DescriptorSetLayoutBinding> bindings = {
        {0, rhi::DescriptorType::AccelerationStructure, 1, rhi::kStageMaskRayGen},
        {1, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskRayGen},
        {2, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskRayGen},
        {3, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskRayGen},
        {4, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskRayGen},
        {5, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskRayGen},      // GPULight[]
        {6, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskClosestHit},   // 场景材质纹理
        {7, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskClosestHit},   // 三角形法线纹理
        {8, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskRayGen},      // FinalReservoir
        {9, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskRayGen},       // 第 5 输出 UAV: albedoMetallic
    };

    // ── push constant 范围（RayGen + ClosestHit + Miss 共用，176B）──
    // Miss 需要 g_PC 检查天空盒 flag / skyIntensity（背景用）
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskRayGen | rhi::kStageMaskClosestHit | rhi::kStageMaskMiss;
    pc.size      = sizeof(PTPushConstant);

    // ── 创建 set0 布局 + 描述符集 ──
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = std::move(bindings);
    m_Layout = device->CreateDescriptorSetLayout(layout);
    if (m_Layout == rhi::kInvalidLayout) {
        HE_CORE_ERROR("PTPass: set0 布局创建失败");
        return false;
    }
    m_Set = device->AllocateDescriptorSet(m_Layout);
    if (m_Set == rhi::kInvalidSet) {
        HE_CORE_ERROR("PTPass: set0 描述符集分配失败");
        return false;
    }
    m_PCRange = pc;

    // ── 着色器：RayGen + ClosestHit + Miss ──
    std::vector<rhi::ShaderBytecode> shaders;
    {
        rhi::ShaderBytecode bc;
        bc.stage = rhi::ShaderStage::RayGen;
        bc.spirv = k_PT_Full_rgen_spv; bc.entryPoint = "main";
        shaders.push_back(bc);
    }
    {
        rhi::ShaderBytecode bc;
        bc.stage = rhi::ShaderStage::ClosestHit;
        bc.spirv = k_PT_Full_rchit_spv; bc.entryPoint = "main";
        shaders.push_back(bc);
    }
    {
        rhi::ShaderBytecode bc;
        bc.stage = rhi::ShaderStage::Miss;
        bc.spirv = k_PT_Full_rmiss_spv; bc.entryPoint = "main";
        shaders.push_back(bc);
    }

    // ── shader groups：RayGen(0) + Hit(ClosestHit=1) + Miss(2) ──
    // 相机光线与阴影光线共用 Hit 记录 0 / Miss 记录 0（哨兵 payload 区分）
    std::vector<rhi::RTShaderGroup> groups;
    {
        rhi::RTShaderGroup rg;
        rg.type = rhi::RTShaderGroupType::RayGen;
        rg.generalShader = 0; rg.name = "PTRayGen";
        groups.push_back(rg);
    }
    {
        rhi::RTShaderGroup hg;
        hg.type = rhi::RTShaderGroupType::Hit;
        hg.closestHitShader = 1; hg.name = "PTClosestHit";
        groups.push_back(hg);
    }
    {
        rhi::RTShaderGroup mg;
        mg.type = rhi::RTShaderGroupType::Miss;
        mg.generalShader = 2; mg.name = "PTMiss";
        groups.push_back(mg);
    }

    // ── 创建独立 RT 管线 + SBT（48B payload，深度 2 够用——循环在 RayGen 内）──
    m_Pipeline = std::make_unique<RTPass::RTEffectPipeline>(
        RTPass::CreateEffectPipeline(device, shaders, groups, {m_Layout}, m_PCRange,
                                     48, rhi::kRTMaxRecursionDepth, "FullPT"));
    if (!m_Pipeline->pipeline) {
        HE_CORE_ERROR("PTPass: 全路径追踪管线创建失败（设备 maxPayloadSize 可能 < 48B）");
        return false;
    }

    // ── 5 张输出纹理（RT 写 UAV，降噪/ReSTIR 读 SRV）──
    rhi::TextureDesc d;
    d.width = m_Width; d.height = m_Height; d.mipLevels = 1;
    d.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
    d.format = rhi::Format::RGBA16_FLOAT;
    m_HDR = device->CreateTexture(d);
    d.format = rhi::Format::R32_FLOAT;
    m_Depth = device->CreateTexture(d);
    d.format = rhi::Format::RGBA16_FLOAT;
    m_Normal = device->CreateTexture(d);
    d.format = rhi::Format::RG16_FLOAT;
    m_Velocity = device->CreateTexture(d);
    d.format = rhi::Format::RGBA16_FLOAT;
    m_AlbedoMetallic = device->CreateTexture(d);
    if (!m_HDR || !m_Depth || !m_Normal || !m_Velocity || !m_AlbedoMetallic) {
        HE_CORE_ERROR("PTPass: 输出纹理创建失败");
        return false;
    }

    HE_CORE_INFO("PTPass: 初始化完成 ({}x{}, payload=48B)", m_Width, m_Height);
    return true;
}

void PTPass::Shutdown() {
    m_Velocity.reset(); m_AlbedoMetallic.reset();
    m_Normal.reset(); m_Depth.reset(); m_HDR.reset();
    m_Pipeline.reset();
    m_Set = rhi::kInvalidSet; m_Layout = rhi::kInvalidLayout;
    m_Device = nullptr; m_Width = m_Height = 0;
    m_OutputWritten = false;
}

// ============================================================
// PrepareOutputUAV — 5 张输出纹理 → UnorderedAccess（RT 写）
// 首帧从 Undefined 过渡（无历史访问）；后续帧从 ShaderResource 过渡
//（上帧由 RTDenoiser 以 SRV 采样结束）。UAV→ShaderResource 的反向转换
// 由 RenderGraph 依据降噪/ReSTIR 的读取依赖自动生成。
// ============================================================
void PTPass::PrepareOutputUAV(rhi::IRHICommandList* cmd) {
    if (!m_HDR) return;
    auto barrier = [&](rhi::IRHITexture* tex) {
        if (m_OutputWritten) {
            cmd->PipelineBarrier(rhi::PipelineStage::FragmentShader, rhi::PipelineStage::RayTracingShader,
                rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess, tex);
        } else {
            cmd->PipelineBarrier(rhi::PipelineStage::TopOfPipe, rhi::PipelineStage::RayTracingShader,
                rhi::ResourceState::Undefined, rhi::ResourceState::UnorderedAccess, tex);
        }
    };
    barrier(m_HDR.get());
    barrier(m_Depth.get());
    barrier(m_Normal.get());
    barrier(m_Velocity.get());
    barrier(m_AlbedoMetallic.get());
    m_OutputWritten = true;
}

// ============================================================
// Execute — 每帧执行 PT Pass
// ============================================================
void PTPass::Execute(rhi::IRHICommandList* cmd, rhi::IRHIAccelerationStructure* tlas,
                     const PTRenderContext& ctx) {
    if (!IsValid() || !tlas || !m_HDR || !m_Device) return;

    // ── 预屏障：5 输出 → UnorderedAccess（RT 写）──
    PrepareOutputUAV(cmd);

    // ── 更新 set0 描述符 ──
    m_Device->UpdateDescriptorSet(m_Set, 0,
        rhi::DescriptorType::AccelerationStructure, tlas);
    m_Device->UpdateDescriptorSetWithImageView(m_Set, 1,
        rhi::DescriptorType::StorageImage, m_HDR->GetNativeHandle());
    m_Device->UpdateDescriptorSetWithImageView(m_Set, 2,
        rhi::DescriptorType::StorageImage, m_Depth->GetNativeHandle());
    m_Device->UpdateDescriptorSetWithImageView(m_Set, 3,
        rhi::DescriptorType::StorageImage, m_Normal->GetNativeHandle());
    m_Device->UpdateDescriptorSetWithImageView(m_Set, 4,
        rhi::DescriptorType::StorageImage, m_Velocity->GetNativeHandle());
    m_Device->UpdateDescriptorSetWithImageView(m_Set, 9,
        rhi::DescriptorType::StorageImage, m_AlbedoMetallic->GetNativeHandle());
    if (ctx.lightBuffer)
        m_Device->UpdateDescriptorSet(m_Set, 5,
            rhi::DescriptorType::StorageBuffer, ctx.lightBuffer);
    if (ctx.sceneMaterialTex)
        m_Device->UpdateDescriptorSet(m_Set, 6,
            rhi::DescriptorType::SampledImage, ctx.sceneMaterialTex, nullptr);
    if (ctx.sceneTriangleNormals)
        m_Device->UpdateDescriptorSet(m_Set, 7,
            rhi::DescriptorType::SampledImage, ctx.sceneTriangleNormals, nullptr);
    if (ctx.finalReservoir)
        m_Device->UpdateDescriptorSet(m_Set, 8,
            rhi::DescriptorType::StorageBuffer, ctx.finalReservoir);

    // ── 设置 push constants（PTPushConstant，176B）──
    PTPushConstant pc{};
    memcpy(&pc.invViewProj, glm::value_ptr(ctx.invViewProj), sizeof(float) * 16);
    memcpy(&pc.prevViewProj, glm::value_ptr(ctx.prevViewProj), sizeof(float) * 16);
    pc.cameraPos    = float4(ctx.cameraPos, 0.0f);
    pc.maxBounces   = std::clamp(ctx.maxBounces, 1u, 8u);
    pc.sampleCount  = std::clamp(ctx.sampleCount, 1u, 8u);
    pc.frameIndex   = ctx.frameIndex;
    pc.lightCount   = ctx.lightCount;
    pc.skyIntensity = ctx.skyIntensity;
    pc.dispatchDimX = m_Width;
    pc.dispatchDimY = m_Height;
    pc.flags        = ctx.flags;

    // ── 先绑 RT 管线（设置正确 push constant 布局），再推常量，最后发射光线 ──
    //（若先推常量，会应用到上一 Pass 的布局——降噪等图形 Pass 范围不匹配 → 写入失败）
    cmd->BindRTPipeline(m_Pipeline->pipeline.get());
    if (m_Set != rhi::kInvalidSet)
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->TraceRays(m_Pipeline->sbt, m_Width, m_Height, 1);
}

} // namespace he::render
