#include "GI/GI_IBL.h"
#include "RHI/RHI.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Vulkan/VulkanResources.h"  // 创建逐 mip 面视图需要访问 VkImage
// 按需包含 IBL Shader SPV（修改 IBL shader 只重编译 GI_IBL.cpp）
#include "IBL_Irradiance.vert.spv.h"
#include "IBL_Irradiance.frag.spv.h"
#include "IBL_Prefilter.vert.spv.h"
#include "IBL_Prefilter.frag.spv.h"
#include "IBL_BRDF_LUT.vert.spv.h"
#include "IBL_BRDF_LUT.frag.spv.h"

using namespace he;
using namespace he::rhi;

namespace {

// 为 Cubemap 的指定 face + mip 创建临时 VkImageView（用于逐 mip 离屏渲染）
VkImageView CreateFaceMipView(IRHITexture* tex, u32 face, u32 mip) {
    auto* vkTex = static_cast<VulkanTexture*>(tex);
    VkImageViewCreateInfo info{};
    info.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image      = vkTex->GetImage();
    info.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    info.format     = vkTex->GetVkFormat();
    info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.baseMipLevel   = mip;
    info.subresourceRange.levelCount     = 1;
    info.subresourceRange.baseArrayLayer = face;
    info.subresourceRange.layerCount     = 1;

    VkImageView view = VK_NULL_HANDLE;
    VkDevice device = vkTex->GetDevice();
    vkCreateImageView(device, &info, nullptr, &view);
    return view;
}

} // anonymous namespace

#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>

namespace he::render {

// ============================================================
// Cubemap 6 面方向 + 对应的上向量（Vulkan Cubemap 约定）
// Face 0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z
// ============================================================
static const float3 kCubeFaceDirs[6] = {
    float3( 1,  0,  0),  // +X
    float3(-1,  0,  0),  // -X
    float3( 0,  1,  0),  // +Y (Vulkan: up)
    float3( 0, -1,  0),  // -Y
    float3( 0,  0,  1),  // +Z
    float3( 0,  0, -1),  // -Z
};
static const float3 kCubeFaceUps[6] = {
    float3(0, -1,  0),   // +X → down
    float3(0, -1,  0),   // -X → down
    float3(0,  0,  1),   // +Y → forward
    float3(0,  0, -1),   // -Y → backward
    float3(0, -1,  0),   // +Z → down
    float3(0, -1,  0),   // -Z → down
};

float4x4 GI_IBL::CubeFaceViewMatrix(u32 face) {
    float3 eye(0.0f);  // 相机在原点
    float3 center = kCubeFaceDirs[face];
    float3 up     = kCubeFaceUps[face];
    return glm::lookAtRH(eye, center, up);
}

bool GI_IBL::Initialize(rhi::IRHIDevice* device, u32, u32) {
    m_Device = device;
    HE_CORE_INFO("GI_IBL::Initialize — 开始创建 IBL 资源");

    // --- 1. 创建输出纹理 ---

    // Irradiance Map: 32×32 RGBA16_FLOAT Cubemap（6 面，1 mip）
    {
        rhi::TextureDesc desc;
        desc.format      = rhi::Format::RGBA16_FLOAT;
        desc.width       = kIrradianceRes;
        desc.height      = kIrradianceRes;
        desc.mipLevels   = 1;
        desc.arrayLayers = 6;
        desc.usage       = rhi::TextureUsage::RenderTarget
                         | rhi::TextureUsage::ShaderResource
                         | rhi::TextureUsage::Cubemap;
        m_IrradianceMap = device->CreateTexture(desc);
        HE_CORE_INFO("  Irradiance Map: {}x{} Cubemap", kIrradianceRes, kIrradianceRes);
    }

    // Prefilter Map: 128×128 RGBA16_FLOAT Cubemap（6 面，5 mip 对应 roughness 0/0.25/0.5/0.75/1.0）
    {
        rhi::TextureDesc desc;
        desc.format      = rhi::Format::RGBA16_FLOAT;
        desc.width       = kPrefilterRes;
        desc.height      = kPrefilterRes;
        desc.mipLevels   = kPrefilterMips;
        desc.arrayLayers = 6;
        desc.usage       = rhi::TextureUsage::RenderTarget
                         | rhi::TextureUsage::ShaderResource
                         | rhi::TextureUsage::Cubemap;
        m_PrefilterMap = device->CreateTexture(desc);
        HE_CORE_INFO("  Prefilter Map: {}x{} Cubemap ({} mips)", kPrefilterRes, kPrefilterRes, kPrefilterMips);
    }

    // BRDF LUT: 512×512 RG16_FLOAT 2D
    {
        rhi::TextureDesc desc;
        desc.format    = rhi::Format::RG16_FLOAT;
        desc.width     = kBRDF_LUT_Res;
        desc.height    = kBRDF_LUT_Res;
        desc.mipLevels = 1;
        desc.usage     = rhi::TextureUsage::RenderTarget
                       | rhi::TextureUsage::ShaderResource;
        m_BRDF_LUT = device->CreateTexture(desc);
        HE_CORE_INFO("  BRDF LUT: {}x{}", kBRDF_LUT_Res, kBRDF_LUT_Res);
    }

    // --- 2. 创建公共采样器（三线性插值 + Clamp 边缘，支持 mip 过滤）---
    {
        rhi::SamplerDesc sampDesc;
        sampDesc.minFilter = rhi::FilterMode::Linear;
        sampDesc.magFilter = rhi::FilterMode::Linear;
        sampDesc.mipFilter = rhi::FilterMode::Linear;
        sampDesc.minLod    = 0.0f;
        sampDesc.maxLod    = static_cast<float>(kPrefilterMips - 1);  // mip index max = 4
        sampDesc.addressU  = rhi::AddressMode::ClampToEdge;
        sampDesc.addressV  = rhi::AddressMode::ClampToEdge;
        sampDesc.addressW  = rhi::AddressMode::ClampToEdge;
        m_IBLSampler = device->CreateSampler(sampDesc);
    }

    // --- 3. 创建 PSO ---

    rhi::PushConstantRange pcRange;
    pcRange.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment;  // Vertex | Fragment
    pcRange.offset    = 0;
    pcRange.size      = 96;  // float4x4(64) + float(4) + padding → 96B (align 16)

    // 辐照度卷积 PSO
    {
        rhi::DescriptorSetLayoutDesc iblLayout;
        iblLayout.bindings = {
            { 0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment },
        };
        m_IrradianceLayout = device->CreateDescriptorSetLayout(iblLayout);

        rhi::ShaderBytecode vs, fs;
        vs.stage      = rhi::ShaderStage::Vertex;
        vs.spirv      = k_IBL_Irradiance_vert_spv;
        vs.entryPoint = "main";
        fs.stage      = rhi::ShaderStage::Pixel;
        fs.spirv      = k_IBL_Irradiance_frag_spv;
        fs.entryPoint = "main";

        rhi::PipelineStateDesc psoDesc;
        psoDesc.vertexShader         = &vs;
        psoDesc.pixelShader          = &fs;
        psoDesc.topology             = rhi::PrimitiveTopology::TriangleList;
        psoDesc.depthTest            = false;
        psoDesc.depthWrite           = false;
        psoDesc.colorAttachmentCount = 1;
        psoDesc.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
        psoDesc.depthFormat          = rhi::Format::Unknown;  // 无需深度
        psoDesc.pushConstantRanges   = { pcRange };
        psoDesc.descriptorSetLayouts = { m_IrradianceLayout };
        psoDesc.debugName            = "IBL_Irradiance";

        m_IrradiancePSO = device->CreatePipelineState(psoDesc);
        HE_ASSERT(m_IrradiancePSO, "GI_IBL: failed to create Irradiance PSO");

        // 绑定 Skybox Cubemap 到描述符集（初始占位，SetSkyboxCubemap 时更新）
        m_IrradianceSet = device->AllocateDescriptorSet(m_IrradianceLayout);
    }

    // 预滤波 PSO
    {
        rhi::DescriptorSetLayoutDesc pfLayout;
        pfLayout.bindings = {
            { 0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment },
        };
        m_PrefilterLayout = device->CreateDescriptorSetLayout(pfLayout);

        rhi::ShaderBytecode vs, fs;
        vs.stage      = rhi::ShaderStage::Vertex;
        vs.spirv      = k_IBL_Prefilter_vert_spv;
        vs.entryPoint = "main";
        fs.stage      = rhi::ShaderStage::Pixel;
        fs.spirv      = k_IBL_Prefilter_frag_spv;
        fs.entryPoint = "main";

        rhi::PipelineStateDesc psoDesc;
        psoDesc.vertexShader         = &vs;
        psoDesc.pixelShader          = &fs;
        psoDesc.topology             = rhi::PrimitiveTopology::TriangleList;
        psoDesc.depthTest            = false;
        psoDesc.depthWrite           = false;
        psoDesc.colorAttachmentCount = 1;
        psoDesc.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
        psoDesc.depthFormat          = rhi::Format::Unknown;  // 无需深度
        psoDesc.pushConstantRanges   = { pcRange };
        psoDesc.descriptorSetLayouts = { m_PrefilterLayout };
        psoDesc.debugName            = "IBL_Prefilter";

        m_PrefilterPSO = device->CreatePipelineState(psoDesc);
        HE_ASSERT(m_PrefilterPSO, "GI_IBL: failed to create Prefilter PSO");

        m_PrefilterSet = device->AllocateDescriptorSet(m_PrefilterLayout);
    }

    // BRDF LUT PSO
    {
        rhi::ShaderBytecode vs, fs;
        vs.stage      = rhi::ShaderStage::Vertex;
        vs.spirv      = k_IBL_BRDF_LUT_vert_spv;
        vs.entryPoint = "main";
        fs.stage      = rhi::ShaderStage::Pixel;
        fs.spirv      = k_IBL_BRDF_LUT_frag_spv;
        fs.entryPoint = "main";

        rhi::PipelineStateDesc psoDesc;
        psoDesc.vertexShader         = &vs;
        psoDesc.pixelShader          = &fs;
        psoDesc.topology             = rhi::PrimitiveTopology::TriangleList;
        psoDesc.depthTest            = false;
        psoDesc.depthWrite           = false;
        psoDesc.colorAttachmentCount = 1;
        psoDesc.colorFormats[0]      = rhi::Format::RG16_FLOAT;
        psoDesc.depthFormat          = rhi::Format::Unknown;  // 无需深度
        psoDesc.debugName            = "IBL_BRDF_LUT";

        m_BRDF_LUT_PSO = device->CreatePipelineState(psoDesc);
        HE_ASSERT(m_BRDF_LUT_PSO, "GI_IBL: failed to create BRDF LUT PSO");
    }

    m_Ready = true;
    HE_CORE_INFO("GI_IBL::Initialize — 完成");
    return true;
}

void GI_IBL::Shutdown() {
    // 清理缓存的逐 mip 面视图
    if (m_PrefilterMap && !m_CachedMipViews.empty()) {
        auto* vkTex = static_cast<VulkanTexture*>(m_PrefilterMap.get());
        for (auto& v : m_CachedMipViews) vkDestroyImageView(vkTex->GetDevice(), v, nullptr);
        m_CachedMipViews.clear();
    }
    m_IrradianceMap.reset();
    m_PrefilterMap.reset();
    m_BRDF_LUT.reset();
    m_IBLSampler.reset();
    m_IrradiancePSO.reset();
    m_PrefilterPSO.reset();
    m_BRDF_LUT_PSO.reset();
    m_Device = nullptr;
    m_Ready  = false;
    HE_CORE_INFO("GI_IBL::Shutdown");
}

void GI_IBL::Update(const SubsystemContext& ctx) {
    (void)ctx;
    // Skybox Cubemap 检测由 ForwardPipeline 在 RenderSkybox 中完成，
    // 通过 SetIBLSkybox() 传入。此处保留空实现以支持未来扩展。
}

void GI_IBL::SetIBLSkybox(rhi::IRHITexture* cubemap, rhi::IRHISampler* sampler) {
    if (!cubemap || !sampler || !m_Device) return;
    // 仅当 skybox 实际变化时才标记脏（避免每帧重生成）
    if (m_SkyboxCubemap == cubemap && m_SkyboxSampler == sampler) return;
    m_SkyboxCubemap = cubemap;
    m_SkyboxSampler = sampler;
    m_Dirty         = true;

    // 更新描述符集绑定
    if (m_IrradianceSet != rhi::kInvalidSet)
        m_Device->UpdateDescriptorSet(m_IrradianceSet, 0,
            rhi::DescriptorType::CombinedImageSampler,
            m_SkyboxCubemap, m_SkyboxSampler);
    if (m_PrefilterSet != rhi::kInvalidSet)
        m_Device->UpdateDescriptorSet(m_PrefilterSet, 0,
            rhi::DescriptorType::CombinedImageSampler,
            m_SkyboxCubemap, m_SkyboxSampler);
}

void GI_IBL::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_SkyboxCubemap || !m_Dirty) return;
    HE_CORE_INFO("GI_IBL::Render — 生成 IBL 贴图...");

    // 90° 透视投影（Cubemap 面渲染用）
    float4x4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);

    // --- Pass 1: 辐照度图 ---
    {
        // Push constants: invViewProj 每个面不同
        struct alignas(16) IBLPush { float4x4 invVP; float roughnessPad; float _pad[3]; } pc{};

        cmd->SetPipeline(m_IrradiancePSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_IrradianceSet);

        for (u32 face = 0; face < rhi::kCubemapFaceCount; ++face) {
            void* faceView = m_IrradianceMap->GetNativeHandle(face);
            if (!faceView) continue;

            float4x4 view  = CubeFaceViewMatrix(face);
            pc.invVP       = glm::inverse(proj * view);

            cmd->BeginOffscreenPass(faceView, nullptr, kIrradianceRes, kIrradianceRes, nullptr);
            cmd->SetViewport({ 0, static_cast<float>(kIrradianceRes),
                static_cast<float>(kIrradianceRes), -static_cast<float>(kIrradianceRes),
                0.0f, 1.0f });
            cmd->SetScissor({ 0, 0, kIrradianceRes, kIrradianceRes });
            cmd->SetPushConstants(0, sizeof(IBLPush), &pc);
            cmd->Draw(3);  // 全屏三角形
            cmd->EndOffscreenPass();
        }

        // 布局转换：COLOR_ATTACHMENT → SHADER_READ（PBR Shader 采样）
        cmd->PipelineBarrier(
            rhi::PipelineStage::ColorAttachmentOutput,
            rhi::PipelineStage::FragmentShader,
            rhi::ResourceState::RenderTarget,
            rhi::ResourceState::ShaderResource,
            m_IrradianceMap.get());
    }

    // --- Pass 2: 预滤波环境图（5 mip，roughness = mip / 4）---
    {
        // 先销毁旧缓存视图（prefilter 重建时）
        auto* vkTex = static_cast<VulkanTexture*>(m_PrefilterMap.get());
        for (auto& v : m_CachedMipViews) vkDestroyImageView(vkTex->GetDevice(), v, nullptr);
        m_CachedMipViews.clear();

        struct alignas(16) IBLPush { float4x4 invVP; float roughness; float _pad[3]; } pc{};
        cmd->SetPipeline(m_PrefilterPSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_PrefilterSet);

        for (u32 mip = 0; mip < kPrefilterMips; ++mip) {
            u32 mipRes   = kPrefilterRes >> mip;  // 128, 64, rhi::kStageMaskCompute, rhi::kStageMaskFragment, 8
            pc.roughness = static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);

            for (u32 face = 0; face < rhi::kCubemapFaceCount; ++face) {
                VkImageView mipView = CreateFaceMipView(m_PrefilterMap.get(), face, mip);
                m_CachedMipViews.push_back(mipView);  // 缓存：framebuffer 延迟销毁需要有效视图

                float4x4 view  = CubeFaceViewMatrix(face);
                pc.invVP       = glm::inverse(proj * view);

                cmd->BeginOffscreenPass(reinterpret_cast<void*>(mipView), nullptr,
                                        mipRes, mipRes, nullptr);
                cmd->SetViewport({ 0, static_cast<float>(mipRes),
                    static_cast<float>(mipRes), -static_cast<float>(mipRes), 0.0f, 1.0f });
                cmd->SetScissor({ 0, 0, mipRes, mipRes });
                cmd->SetPushConstants(0, sizeof(IBLPush), &pc);
                cmd->Draw(3);
                cmd->EndOffscreenPass();
            }
        }

        // 布局转换：COLOR_ATTACHMENT → SHADER_READ
        cmd->PipelineBarrier(
            rhi::PipelineStage::ColorAttachmentOutput,
            rhi::PipelineStage::FragmentShader,
            rhi::ResourceState::RenderTarget,
            rhi::ResourceState::ShaderResource,
            m_PrefilterMap.get());
    }

    // --- Pass 3: BRDF LUT ---
    {
        cmd->SetPipeline(m_BRDF_LUT_PSO.get());
        void* colorView = m_BRDF_LUT->GetNativeHandle();
        cmd->BeginOffscreenPass(colorView, nullptr, kBRDF_LUT_Res, kBRDF_LUT_Res, nullptr);
        cmd->SetViewport({ 0, static_cast<float>(kBRDF_LUT_Res),
            static_cast<float>(kBRDF_LUT_Res), -static_cast<float>(kBRDF_LUT_Res),
            0.0f, 1.0f });
        cmd->SetScissor({ 0, 0, kBRDF_LUT_Res, kBRDF_LUT_Res });
        cmd->Draw(3);
        cmd->EndOffscreenPass();

        // 布局转换：COLOR_ATTACHMENT → SHADER_READ
        cmd->PipelineBarrier(
            rhi::PipelineStage::ColorAttachmentOutput,
            rhi::PipelineStage::FragmentShader,
            rhi::ResourceState::RenderTarget,
            rhi::ResourceState::ShaderResource,
            m_BRDF_LUT.get());
    }

    m_Dirty = false;
    HE_CORE_INFO("GI_IBL::Render — IBL 贴图生成完成");
}

void GI_IBL::Bind(rhi::IRHICommandList* cmd) const {
    // IBL 纹理在 PBR Shader 中通过扩展的 descriptor set 绑定
    // 此处绑定 IBL 输出到 command list（实际绑定由 ForwardPipeline 管理）
    (void)cmd;
}

void GI_IBL::OnResize(u32, u32) {
    // IBL 贴图分辨率独立于视口，无需重建
}

} // namespace he::render
