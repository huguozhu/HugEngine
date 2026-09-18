// ============================================================
// LightingPass.cpp — 延迟光照 Pass 实现
// 拥有 HDR 目标纹理 + Lighting PSO + 描述符集
// 提供统一 Render 接口：输入 GBuffer + 效果纹理 → 输出 HDR
// ============================================================
#include "Pipeline/LightingPass.h"
#include "Core/Log.h"
#include "ShaderTypes.slang"  // DeferredLightingPushConstant

// SPIR-V 嵌入头文件
#include "DeferredLighting.vert.spv.h"
#include "DeferredLighting.frag.spv.h"

namespace he::render {

bool LightingPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    // ── 1. 创建 HDR 目标纹理 ──
    CreateHDRTextures(device);

    // ── 2. 创建 PSO + 描述符集 ──
    CreatePSOAndDescriptorSet(device);

    HE_CORE_INFO("LightingPass: 初始化完成 ({}x{})", width, height);
    return true;
}

void LightingPass::Shutdown() {
    if (m_Layout != rhi::kInvalidLayout && m_PSO) {
        // PSOLayout 随 PSO 隐式管理
    }
    m_PSO.reset();
    m_HDRDepth.reset();
    m_HDRTarget.reset();
    m_HDRSampler.reset();
    m_PointSampler.reset();
    m_Width = m_Height = 0;
}

void LightingPass::OnResize(rhi::IRHIDevice* device, u32 width, u32 height) {
    if (width == m_Width && height == m_Height) return;
    m_Width  = width;
    m_Height = height;

    // 重建 HDR 目标纹理
    CreateHDRTextures(device);
}

// ============================================================
// Render — 执行完整延迟光照 Pass
// ============================================================
void LightingPass::Render(rhi::IRHICommandList* cmd, const LightingInputs& in) {
    // 【每飞行帧一份描述符集】本帧只更新/绑定本槽位自己的集合：逐帧轮换的资源（光源、阴影、
    // 探针 SSBO 与合成参数 UBO）绑进各自槽位后，重绑不再跨帧（§9.2-J）。
    const u32 slot = in.frameSlot % rhi::kMaxFramesInFlight;
    rhi::DescriptorSetHandle set = m_Sets[slot];

    // 绑纹理。
    // fallback == nullptr：输入为 null 时**不更新**该绑定（与原先一致）——用于那些"每帧必有"
    //   的输入（GBuffer / 深度 / SSAO / IBL），它们的 null 只意味着"暂时取不到"，此时保留
    //   上一次的有效绑定比换成占位更接近正确值。
    // fallback != nullptr：输入为 null 表示**本帧没有产出**，必须显式回绑中性占位。不能只是
    //   "跳过更新"——描述符集是持久的，跳过会留下上一帧（或初始化时）的绑定，于是本帧没有产出
    //   的纹理仍会被采样到，读的是未初始化显存，而且描述符本身"看起来"合法，故障完全静默
    //   （§9.2-T）。白 = 1.0（无遮挡/无遮蔽），黑 = 0.0（无贡献）。
    auto bindTex = [&](u32 binding, rhi::IRHITexture* tex, rhi::IRHISampler* sampler,
                       rhi::IRHITexture* fallback = nullptr) {
        if (!m_Device) return;
        if (tex && sampler) {
            m_Device->UpdateDescriptorSet(set, binding,
                rhi::DescriptorType::CombinedImageSampler, tex, sampler);
            return;
        }
        if (!fallback || !m_PlaceholderSampler) return;
        m_Device->UpdateDescriptorSet(set, binding,
            rhi::DescriptorType::CombinedImageSampler, fallback, m_PlaceholderSampler.get());
    };

    // ── 绑定 GBuffer 纹理 ──
    rhi::IRHITexture* black = m_PlaceholderBlack.get();
    rhi::IRHITexture* white = m_PlaceholderWhite.get();
    bindTex(kGPUBinding_GBufferA, in.gbA, m_HDRSampler.get());
    bindTex(kGPUBinding_GBufferB, in.gbB, m_HDRSampler.get());
    bindTex(kGPUBinding_GBufferC, in.gbC, m_HDRSampler.get());
    bindTex(kGPUBinding_GBufferE, in.gbE, m_PointSampler.get());
    bindTex(kGPUBinding_GBufferF, in.gbDisneyA, m_HDRSampler.get());  // disneyA（anisotropic/subsurface/specular/sheen）
    bindTex(kGPUBinding_GBufferG, in.gbDisneyB, m_HDRSampler.get());  // disneyB（clearcoat/clearcoatGloss/specularTint.rg）
    bindTex(kGPUBinding_Depth, in.gbDepth, m_PointSampler.get());

    // ── 绑定阴影贴图（门控：本帧没产出就回绑白色占位 = 无遮挡）──
    // 采样深度 1.0 = 无遮挡，正是"该光源不投影"的中性值。调用方必须传 nullptr 表示
    // "本帧没有产出这张图"，不能传一张没写过的真实纹理。
    bindTex(kGPUBinding_ShadowMap0, in.csmShadow0, m_HDRSampler.get(), white);
    bindTex(kGPUBinding_ShadowMap1, in.csmShadow1, m_HDRSampler.get(), white);
    bindTex(kGPUBinding_ShadowMap2, in.csmShadow2, m_HDRSampler.get(), white);
    bindTex(kGPUBinding_SpotShadow_DL, in.spotShadow, m_HDRSampler.get(), white);

    // ── 绑定光源/阴影数据 SSBO ──
    if (in.lightBuffer && m_Device)
        m_Device->UpdateDescriptorSet(set, kGPUBinding_Lights_DL, rhi::DescriptorType::StorageBuffer, in.lightBuffer);
    if (in.shadowBuffer && m_Device)
        m_Device->UpdateDescriptorSet(set, kGPUBinding_ShadowData_DL, rhi::DescriptorType::StorageBuffer, in.shadowBuffer);

    // ── 绑定屏幕空间效果（门控：SSGI/SSR 未注册 pass 时回绑黑色占位 = 无贡献）──
    bindTex(kGPUBinding_SSGI, in.ssgiTex, in.ssgiSampler, black);
    bindTex(kGPUBinding_SSAO_DL, in.ssaoTex, m_HDRSampler.get());
    bindTex(kGPUBinding_SSR, in.ssrTex, in.ssrSampler, black);

    // ── 绑定 DDGI 探针 ──
    if (in.ddgiProbeBuffer && m_Device)
        m_Device->UpdateDescriptorSet(set, kGPUBinding_DDGIProbes, rhi::DescriptorType::StorageBuffer, in.ddgiProbeBuffer);
    // ── 绑定 DDGI 网格参数 UBO（SampleDDGI 三线性插值用）──
    if (in.ddgiGridUniform && m_Device)
        m_Device->UpdateDescriptorSet(set, kGPUBinding_DDGIGridParams, rhi::DescriptorType::UniformBuffer, in.ddgiGridUniform);

    // ── 绑定 RSM 间接光（Forward/Deferred 共用；未提供时回落到黑色占位 = 无间接光）──
    bindTex(kGPUBinding_RSMPosition, in.rsmPositionMap, m_HDRSampler.get(), black);
    bindTex(kGPUBinding_RSMFlux, in.rsmNormalMap,     m_HDRSampler.get(), black);
    // RSM 间接光 E（半分辨率）：线性采样以便升采样到全分辨率；未产出时黑色占位（无间接光）
    bindTex(kGPUBinding_RSMIndirect, in.rsmIndirectTex, m_HDRSampler.get(), black);

    // ── 绑定 Hybrid RT 效果输出纹理（未提供时回落到中性占位）──
    // 阴影/AO 遮罩用线性采样上采样到全分辨率；反射/GI HDR 结果用线性采样
    bindTex(kGPUBinding_RT_ShadowMask, in.rtShadowMask, m_HDRSampler.get(), m_PlaceholderWhite.get());  // RT 阴影遮罩
    bindTex(kGPUBinding_RT_Reflection, in.rtReflection, m_HDRSampler.get(), black);   // RT 反射
    bindTex(kGPUBinding_RT_AO, in.rtAO,         m_HDRSampler.get(), m_PlaceholderWhite.get());   // RT AO
    bindTex(kGPUBinding_RT_GI, in.rtGI,         m_HDRSampler.get(), black);   // RT GI

    // ── 聚集着色（可选）──
    u32 useClustered = 0;
    u32 clusterTilesX = 0, clusterTilesY = 0;
    float clusterNear = 0.1f, clusterFar = 2000.0f, clusterLogFactor = 1.0f;

    if (in.clusteredShading && in.clusteredShading->enabled && m_Device
        && in.lightGridBuffer && in.lightIndexListBuffer && in.cachedLights && !in.cachedLights->empty()) {
        clusterTilesX = (in.width  + 63) / 64;
        clusterTilesY = (in.height + 63) / 64;
        in.clusteredShading->CullLights(in.cachedLights->data(), (u32)in.cachedLights->size());

        // 上传 LightGrid + LightIndexList
        m_Device->UpdateDescriptorSet(set, kGPUBinding_LightGrid, rhi::DescriptorType::StorageBuffer, in.lightGridBuffer);
        m_Device->UpdateDescriptorSet(set, kGPUBinding_LightIndexList, rhi::DescriptorType::StorageBuffer, in.lightIndexListBuffer);

        clusterTilesX = in.clusteredShading->GetTileCountX();
        clusterTilesY = in.clusteredShading->GetTileCountY();
        useClustered = 1;
    }

    // ── 执行全屏三角形绘制 ──
    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, set);

    rhi::ClearValue clr{};
    cmd->BeginOffscreenPass(m_HDRTarget->GetNativeHandle(), m_HDRDepth->GetNativeHandle(),
                            in.width, in.height, &clr, false);
    cmd->SetViewport({0, (float)in.height, (float)in.width, -(float)in.height, 0, 1});
    cmd->SetScissor({0, 0, in.width, in.height});

    // Push constants
    DeferredLightingPushConstant lpc{};
    lpc.cameraPosition  = in.cameraPos;
    lpc.lightCount      = in.lightCount;
    lpc.iblIntensity    = in.iblIntensity;
    lpc.useClustered    = useClustered;
    lpc.clusterTilesX   = clusterTilesX;
    lpc.clusterTilesY   = clusterTilesY;
    lpc.clusterNear     = clusterNear;
    lpc.clusterFar      = clusterFar;
    lpc.clusterLogFactor = clusterLogFactor;
    // RT 效果输入源标志：纹理非空则 shader 侧使用 RT 输出替代屏幕空间效果
    lpc.rtShadowSource   = in.rtShadowMask ? 1u : 0u;
    lpc.atmosphere = float4(m_AtmSunDir, m_AtmTurbidity);  // 空中透视参数（太阳方向 + 浑浊度）
    lpc.giIntensity = in.giIntensity;                      // 间接漫反射 GI 总强度（默认 1.0）
    lpc.aoIntensity = in.aoIntensity;                      // AO 强度（默认 1.0）
    // ── 分层合成（Wave 1）：填充 GIBlendParams UBO（3 通道 × 源数组）──
    // C++ 侧 GIChannelBlendData 与 shader 侧 GIChannelBlendParams 布局一致，
    // 按通道 memcpy；源的具体分派由 shader 按 id 完成
    {
        static_assert(sizeof(GISourceSlotData) == sizeof(GISourceSlot),
                      "GISourceSlotData 必须与 shader 的 GISourceSlot 布局一致");
        static_assert(sizeof(GIChannelBlendData) == sizeof(GIChannelBlendParams),
                      "GIChannelBlendData 必须与 shader 的 GIChannelBlendParams 布局一致");
        if (m_BlendUBO[slot]) {
            void* mapped = m_BlendUBO[slot]->Map();
            if (mapped) {
                auto* bp = static_cast<GIBlendParams*>(mapped);
                std::memcpy(&bp->diffuse,  &in.diffuseBlend,  sizeof(GIChannelBlendData));
                std::memcpy(&bp->specular, &in.specularBlend, sizeof(GIChannelBlendData));
                std::memcpy(&bp->ao,       &in.aoBlend,       sizeof(GIChannelBlendData));
                m_BlendUBO[slot]->Unmap();
            }
        }
    }
    cmd->SetPushConstants(0, sizeof(lpc), &lpc);
    cmd->Draw(3);

    cmd->EndOffscreenPass();
}

// ============================================================
// 内部实现
// ============================================================

void LightingPass::CreateHDRTextures(rhi::IRHIDevice* device) {
    // HDR 颜色目标 (RGBA16_FLOAT)
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.width  = m_Width;
        d.height = m_Height;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_HDRTarget = device->CreateTexture(d);
    }
    // HDR 深度目标 (D32_FLOAT)
    {
        rhi::TextureDesc dd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.width  = m_Width;
        dd.height = m_Height;
        dd.usage  = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
        m_HDRDepth = device->CreateTexture(dd);
    }
    // HDR 采样器（Linear）
    {
        rhi::SamplerDesc s;
        s.minFilter = s.magFilter = rhi::FilterMode::Linear;
        s.addressU = s.addressV = rhi::AddressMode::ClampToEdge;
        m_HDRSampler = device->CreateSampler(s);
    }
    // 点采样器（Nearest — 深度/WorldPos 精确读取）
    {
        rhi::SamplerDesc ptDesc;
        ptDesc.minFilter = ptDesc.magFilter = rhi::FilterMode::Nearest;
        ptDesc.addressU  = ptDesc.addressV  = rhi::AddressMode::ClampToEdge;
        m_PointSampler = device->CreateSampler(ptDesc);
    }
}

void LightingPass::CreatePSOAndDescriptorSet(rhi::IRHIDevice* device) {
    // ── 描述符集布局 ──
    rhi::DescriptorSetLayoutDesc ll;
    ll.bindings = {
        {kGPUBinding_GBufferA, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // GBufferA
        {kGPUBinding_GBufferB, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // GBufferB
        {kGPUBinding_GBufferC, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // GBufferC
        {kGPUBinding_Depth, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // Depth
        {kGPUBinding_GBufferE, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // GBufferE (worldPos)
        {kGPUBinding_GBufferF, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // GBufferF (disneyA)
        {kGPUBinding_GBufferG, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // GBufferG (disneyB)
        {kGPUBinding_ShadowMap0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // Shadow0 (CSM0)
        {kGPUBinding_LightGrid, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskFragment},         // LightGrid (Clustered)
        {kGPUBinding_LightIndexList, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskFragment},         // LightIndexList
        {kGPUBinding_SpotShadow_DL, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // SpotShadow
        {kGPUBinding_ShadowMap1, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // Shadow1 (CSM1)
        {kGPUBinding_ShadowMap2, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // Shadow2 (CSM2)
        {kGPUBinding_IrradianceMap, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // Irradiance
        {kGPUBinding_PrefilterMap, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // Prefilter
        {kGPUBinding_BRDF_LUT, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // BRDF LUT
        {kGPUBinding_RSMPosition, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // RSM Pos
        {kGPUBinding_RSMFlux, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // RSM Normal（VPL 世界法线，任务 30 起独立附件）
        {kGPUBinding_RSMIndirect, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // RSM 间接光 E（半分辨率）
        {kGPUBinding_Lights_DL, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskFragment},         // Lights SSBO
        {kGPUBinding_ShadowData_DL, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskFragment},         // ShadowData SSBO
        {kGPUBinding_SSGI, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // SSGI
        {kGPUBinding_SSAO_DL, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // SSAO
        {kGPUBinding_SSR, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // SSR
        {kGPUBinding_DDGIProbes, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskFragment},         // DDGI Probes
        {kGPUBinding_DDGIGridParams, rhi::DescriptorType::UniformBuffer, 1, rhi::kStageMaskFragment},         // DDGI 网格参数 UBO
        {kGPUBinding_RT_ShadowMask, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // RT 阴影遮罩
        {kGPUBinding_RT_Reflection, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // RT 反射
        {kGPUBinding_RT_AO, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // RT AO
        {kGPUBinding_RT_GI, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // RT GI
        {kGPUBinding_GIBlendParams, rhi::DescriptorType::UniformBuffer, 1, rhi::kStageMaskFragment},  // GI 分层合成参数 UBO
    };
    m_Layout = device->CreateDescriptorSetLayout(ll);
    // 每飞行帧一份：逐帧轮换的资源各自绑进自己槽位的集合（§9.2-J）
    for (u32 i = 0; i < rhi::kMaxFramesInFlight; ++i) {
        m_Sets[i] = device->AllocateDescriptorSet(m_Layout);
    }
    const rhi::DescriptorSetHandle set0 = m_Sets[0];

    // ── GI 分层合成参数 UBO（3 通道 × 32B）：每飞行帧一份 ──
    // 单份时，本帧写入会覆盖仍在飞行的上一帧所读的参数（此前靠"值变化小"掩盖）。
    for (u32 i = 0; i < rhi::kMaxFramesInFlight; ++i) {
        rhi::BufferDesc bd;
        bd.size      = sizeof(GIBlendParams);
        bd.usage     = rhi::BufferUsage::Uniform;
        bd.cpuAccess = true;
        m_BlendUBO[i] = device->CreateBuffer(bd);
        device->UpdateDescriptorSet(m_Sets[i], kGPUBinding_GIBlendParams,
            rhi::DescriptorType::UniformBuffer, m_BlendUBO[i].get());
    }

    // ── 预填充所有 binding 占位纹理（避免未绑定 → Intel GPU 白屏）──
    // 产物同时保存为成员：Render 里输入为 null 时要**显式回绑**它们，仅"跳过更新"会留下
    // 上一帧的绑定（描述符集是持久的），本帧没产出的纹理会继续被采样（§9.2-T）。
    {
        u8 w4[4] = {255,255,255,255};
        rhi::TextureDesc ptd;
        ptd.format = rhi::Format::RGBA8_UNORM;
        ptd.width = 1;
        ptd.height = 1;
        ptd.mipLevels = 1;
        ptd.arrayLayers = 1;
        ptd.usage = rhi::TextureUsage::ShaderResource;
        ptd.initialData = w4;
        m_PlaceholderWhite = device->CreateTexture(ptd);

        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
        m_PlaceholderSampler = device->CreateSampler(sd);

        // 更新所有 CombinedImageSampler 绑定（0-4, 9-11, 14, 23, 28, 29 — 2D 纹理）
        // 这些通道的语义都是"1.0 = 无效果"，故白色即为中性值：
        //   阴影图采样出深度 1.0 = 无遮挡；AO/RT 阴影遮罩 = 无遮蔽
        for (u32 b : {0u,1u,2u,3u,4u,9u,10u,11u,14u,23u,28u,29u})
            for (u32 i = 0; i < rhi::kMaxFramesInFlight; ++i)
                device->UpdateDescriptorSet(m_Sets[i], b, rhi::DescriptorType::CombinedImageSampler,
                                            m_PlaceholderWhite.get(), m_PlaceholderSampler.get());

        // 占位绑定辅助：每一份集合都要写（它们各自独立，不能只写第一份）
        auto updateAllTex = [&](u32 binding, rhi::IRHITexture* tex) {
            for (u32 i = 0; i < rhi::kMaxFramesInFlight; ++i)
                device->UpdateDescriptorSet(m_Sets[i], binding, rhi::DescriptorType::CombinedImageSampler,
                                            tex, m_PlaceholderSampler.get());
        };
        auto updateAllBuf = [&](u32 binding, rhi::IRHIBuffer* buf) {
            for (u32 i = 0; i < rhi::kMaxFramesInFlight; ++i)
                device->UpdateDescriptorSet(m_Sets[i], binding, rhi::DescriptorType::StorageBuffer, buf);
        };

        // RT 效果占位纹理：
        //   24/26（RT 阴影/AO）→ 白色（无阴影/无遮蔽，语义上=1.0）
        //   25/27（RT 反射/GI）→ 黑色（无反射/无间接光，语义上=0.0）
        {
            u8 bk[4] = {0,0,0,0};
            rhi::TextureDesc btd;
            btd.format = rhi::Format::RGBA8_UNORM;
            btd.width = 1;
            btd.height = 1;
            btd.mipLevels = 1;
            btd.arrayLayers = 1;
            btd.usage = rhi::TextureUsage::ShaderResource;
            btd.initialData = bk;
            m_PlaceholderBlack = device->CreateTexture(btd);

            updateAllTex(kGPUBinding_RT_ShadowMask, m_PlaceholderWhite.get());
            updateAllTex(kGPUBinding_RT_AO, m_PlaceholderWhite.get());
            updateAllTex(kGPUBinding_RT_Reflection, m_PlaceholderBlack.get());
            updateAllTex(kGPUBinding_RT_GI, m_PlaceholderBlack.get());

            // SSGI/SSAO/SSR 占位（19/20/21）：
            // HybridRT 不计算屏幕空间效果，对应 RT 效果关闭时 shader 回退采样这些纹理。
            // 必须绑定中性占位，避免采样未初始化描述符 → 黑屏。
            //   SSGI → 黑（无间接漫反射），SSAO → 白（无遮蔽），SSR → 黑（无镜面反射）
            updateAllTex(kGPUBinding_SSGI, m_PlaceholderBlack.get());
            updateAllTex(kGPUBinding_SSAO_DL, m_PlaceholderWhite.get());
            updateAllTex(kGPUBinding_SSR, m_PlaceholderBlack.get());

            // RSM 位置/通量图（15/16）→ 黑色：
            // 这两张**不能**用白色占位。对 u_RSMPositionMap，白色是 worldPos≈(1,1,1)；
            // 对 u_RSMFluxMap，白色是 flux=1.0，即一个"全亮 VPL"——一旦门控与 RSM pass 的真实
            // 产出不一致，回落就从一个安全值变成一个偏亮的错误值。黑色才是"无间接光"的中性值。
            updateAllTex(kGPUBinding_RSMPosition, m_PlaceholderBlack.get());
            updateAllTex(kGPUBinding_RSMFlux, m_PlaceholderBlack.get());   // RSM 法线图：中性值不会让它"有效"
            // RSM 间接光 E（binding 5）→ 黑色（无间接光），理由同上
            updateAllTex(kGPUBinding_RSMIndirect, m_PlaceholderBlack.get());
        }

        // 绑定 12=Irradiance, 13=Prefilter 需要 Cubemap（Shader 声明为 TextureCube）
        // 2D 占位纹理类型不匹配 → 采样返回 0 → 间接光照全黑
        {
            // 黑色 Cubemap 占位：类型匹配 TextureCube 声明，IBL 贡献为 0
            // 不能使用白色——白色 Cubemap 会产生非零环境光贡献，导致画面异常
            u8 w4cube[6*4] = {};  // 6 面 × 4 通道，全部为 0（黑色）
            rhi::TextureDesc ctd;
            ctd.format = rhi::Format::RGBA8_UNORM;
            ctd.width = 1;
            ctd.height = 1;
            ctd.mipLevels = 1;
            ctd.arrayLayers = 6;
            ctd.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::Cubemap;
            ctd.initialData = w4cube;
            m_PlaceholderCube = device->CreateTexture(ctd);
            updateAllTex(kGPUBinding_IrradianceMap, m_PlaceholderCube.get());
            updateAllTex(kGPUBinding_PrefilterMap, m_PlaceholderCube.get());
        }

        // Cluster SSBO 占位（binding 7/8）
        rhi::BufferDesc gd;
        gd.size = 16;
        gd.usage = rhi::BufferUsage::Storage;
        auto gb = device->CreateBuffer(gd);
        updateAllBuf(kGPUBinding_LightGrid, gb.get());
        updateAllBuf(kGPUBinding_LightIndexList, gb.get());

        // DDGI 探针 SSBO 占位（binding 22）
        updateAllBuf(kGPUBinding_DDGIProbes, gb.get());
    }

    // ── 创建 PSO ──
    rhi::ShaderBytecode lVS, lFS;
    lVS.stage = rhi::ShaderStage::Vertex;
    lVS.spirv = k_DeferredLighting_vert_spv;
    lVS.entryPoint = "main";
    lFS.stage = rhi::ShaderStage::Pixel;
    lFS.spirv = k_DeferredLighting_frag_spv;
    lFS.entryPoint = "main";

    rhi::PushConstantRange lpc;
    lpc.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment;
    lpc.size = 128;

    rhi::PipelineStateDesc lDesc;
    lDesc.vertexShader = &lVS;
    lDesc.pixelShader = &lFS;
    lDesc.topology = rhi::PrimitiveTopology::TriangleList;
    lDesc.depthTest = false;
    lDesc.depthWrite = false;
    lDesc.colorAttachmentCount = 1;
    lDesc.colorFormats[0] = rhi::Format::RGBA16_FLOAT;
    lDesc.pushConstantRanges = {lpc};
    lDesc.descriptorSetLayouts = {m_Layout};
    lDesc.debugName = "DeferredLighting";

    m_PSO = device->CreatePipelineState(lDesc);
    HE_CORE_INFO("LightingPass: PSO + DescriptorSet 创建完成");
}

void LightingPass::SetIBLTextures(rhi::IRHITexture* irradiance, rhi::IRHITexture* prefilter,
                                  rhi::IRHITexture* brdfLut, rhi::IRHISampler* sampler) {
    if (!m_Device || m_Sets[0] == rhi::kInvalidSet) return;
    // 绑定 IBL 贴图到 Lighting 描述符集（12=Irradiance, 13=Prefilter, 14=BRDF LUT）
    // 【这里不做占位回落】与上面"门控"通道不同：IBL 的产物由 GI_IBL 在启动时一次性创建，
    // 传 null 只意味着"暂时取不到"，而不是"本帧没产出"。此时保留上一次的有效绑定才接近正确值；
    // 若改用占位（BRDF LUT 会变成 1×1 白 = envBRDF 恒为 1），反而会把镜面环境项算大。
    // 三份集合都要写：它们各自独立（IBL 贴图是稳定的，写三遍不会跨帧串味）。
    for (u32 i = 0; i < rhi::kMaxFramesInFlight; ++i) {
        m_Device->UpdateDescriptorSet(m_Sets[i], kGPUBinding_IrradianceMap, rhi::DescriptorType::CombinedImageSampler, irradiance, sampler);
        m_Device->UpdateDescriptorSet(m_Sets[i], kGPUBinding_PrefilterMap, rhi::DescriptorType::CombinedImageSampler, prefilter, sampler);
        m_Device->UpdateDescriptorSet(m_Sets[i], kGPUBinding_BRDF_LUT, rhi::DescriptorType::CombinedImageSampler, brdfLut, sampler);
    }
}

void LightingPass::SetAtmosphere(float3 sunDir, float turbidity) {
    m_AtmSunDir    = sunDir;
    m_AtmTurbidity = turbidity;

    // 一次性验证日志：仅在空中透视激活（turbidity>0）时打印，便于核对参数流入
    static bool s_AtmLogged = false;
    if (turbidity > 0.0f && !s_AtmLogged) {
        s_AtmLogged = true;
        HE_CORE_INFO("[AerialPerspective] sunDir=({:.3f},{:.3f},{:.3f}), turbidity={:.1f}",
                     sunDir.x, sunDir.y, sunDir.z, turbidity);
    }
}

} // namespace he::render
