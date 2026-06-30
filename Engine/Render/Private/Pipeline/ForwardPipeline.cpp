#include "Render/Pipeline/ForwardPipeline.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Threading/JobSystem.h"
#include "EmbeddedShaders.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>  // orthoRH_ZO (Vulkan Z [0,1])

#include <mutex>

namespace he::render {

ForwardPipeline::ForwardPipeline() {
}

ForwardPipeline::~ForwardPipeline() {
    Shutdown();
}

// 计算方向光的正交投影矩阵（固定世界中心，不跟相机移动）
static float4x4 ComputeDirectionalLightViewProj(
    const float3& lightDir, const CameraData& camera)
{
    // 固定场景中心
    float3 sceneCenter(0.0f, 400.0f, 0.0f);
    float sceneSize = 4000.0f;

    // 灯光视图矩阵
    float3 lightUp = (abs(lightDir.y) < 0.999f) ? float3(0, 1, 0) : float3(1, 0, 0);
    float4x4 lightView = glm::lookAtRH(
        sceneCenter - lightDir * sceneSize * 0.5f,
        sceneCenter,
        lightUp);

    // 正交投影
    float halfSize  = sceneSize * 0.5f;
    float nearPlane = 0.1f;
    float farPlane  = sceneSize * 2.0f;
    float4x4 lightProj = glm::orthoRH_ZO(-halfSize, halfSize, -halfSize, halfSize,
                                         nearPlane, farPlane);

    return lightProj * lightView;
}

void ForwardPipeline::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    HE_ASSERT(m_Device, "ForwardPipeline: device is null");

    // --- PBR 着色器 ---
    m_VS.stage      = rhi::ShaderStage::Vertex;
    m_VS.spirv      = k_PBR_vert_spv;
    m_VS.entryPoint = "main";

    m_FS.stage      = rhi::ShaderStage::Pixel;
    m_FS.spirv      = k_PBR_frag_spv;
    m_FS.entryPoint = "main";

    // --- 阴影着色器 ---
    m_ShadowVS.stage      = rhi::ShaderStage::Vertex;
    m_ShadowVS.spirv      = k_Shadow_vert_spv;
    m_ShadowVS.entryPoint = "main";

    m_ShadowFS.stage      = rhi::ShaderStage::Pixel;
    m_ShadowFS.spirv      = k_Shadow_frag_spv;
    m_ShadowFS.entryPoint = "main";

    // --- ToneMap 着色器 ---
    m_ToneMapVS.stage      = rhi::ShaderStage::Vertex;
    m_ToneMapVS.spirv      = k_ToneMap_vert_spv;
    m_ToneMapVS.entryPoint = "main";
    m_ToneMapFS.stage      = rhi::ShaderStage::Pixel;
    m_ToneMapFS.spirv      = k_ToneMap_frag_spv;
    m_ToneMapFS.entryPoint = "main";

    rhi::VertexInputLayout vertexLayout;
    vertexLayout.stride = sizeof(he::StaticVertex);
    vertexLayout.attributes = {
        { 0, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, position) },
        { 1, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, normal) },
        { 2, 0, rhi::VertexFormat::Float2, offsetof(he::StaticVertex, uv) },
    };

    // --- 主管线 DescriptorSetLayout ---
    rhi::DescriptorSetLayoutDesc combinedLayoutDesc;
    combinedLayoutDesc.bindings = {
        { 1, rhi::DescriptorType::StorageBuffer,        1, 16 },  // Fragment — 灯光
        { 2, rhi::DescriptorType::StorageBuffer,        1, 17 },  // Vertex|Fragment — 对象
        { 3, rhi::DescriptorType::StorageBuffer,        1, 16 },  // Fragment — 阴影数据
        { 4, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — 方向光阴影贴图
        { 5, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — 基础色
        { 6, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — 法线
        { 7, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — 金属度/粗糙度
        { 8, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — AO
        { 9, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — 点光源阴影 Cubemap
    };
    m_DescLayout = device->CreateDescriptorSetLayout(combinedLayoutDesc);

    // --- 创建三缓冲 Storage Buffers（Phase 1 多线程渲染）---
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::BufferDesc objBufDesc;
        objBufDesc.size  = sizeof(GPUObjectData) * MAX_OBJECTS;
        objBufDesc.usage = rhi::BufferUsage::Storage;
        m_ObjectBuffers[i] = device->CreateBuffer(objBufDesc);

        rhi::BufferDesc lightBufDesc;
        lightBufDesc.size  = sizeof(GPULight) * MAX_LIGHTS;
        lightBufDesc.usage = rhi::BufferUsage::Storage;
        m_LightBuffers[i] = device->CreateBuffer(lightBufDesc);

        rhi::BufferDesc shadowBufDesc;
        shadowBufDesc.size  = sizeof(GPUShadowData) * MAX_SHADOWS;
        shadowBufDesc.usage = rhi::BufferUsage::Storage;
        m_ShadowBuffers[i] = device->CreateBuffer(shadowBufDesc);
    }

    // --- 创建阴影贴图纹理（深度纹理，将被着色器采样）---
    rhi::TextureDesc shadowTexDesc;
    shadowTexDesc.format      = rhi::Format::D32_FLOAT;
    shadowTexDesc.width       = m_ShadowMapSize;
    shadowTexDesc.height      = m_ShadowMapSize;
    shadowTexDesc.depth       = 1;
    shadowTexDesc.mipLevels   = 1;
    shadowTexDesc.arrayLayers = 1;
    shadowTexDesc.usage       = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    m_ShadowMap = device->CreateTexture(shadowTexDesc);

    // --- 创建阴影贴图采样器（非比较模式 — PCF 手动做深度比较）---
    {
        rhi::SamplerDesc shadowSampDesc;
        shadowSampDesc.minFilter = rhi::FilterMode::Linear;
        shadowSampDesc.magFilter = rhi::FilterMode::Linear;
        shadowSampDesc.addressU  = rhi::AddressMode::ClampToEdge;
        shadowSampDesc.addressV  = rhi::AddressMode::ClampToEdge;
        shadowSampDesc.addressW  = rhi::AddressMode::ClampToEdge;
        m_ShadowSampler = device->CreateSampler(shadowSampDesc);
    }

    // --- 创建点光源阴影 Cubemap（6 面深度纹理）---
    {
        rhi::TextureDesc pointShadowDesc;
        pointShadowDesc.format      = rhi::Format::D32_FLOAT;
        pointShadowDesc.width       = m_PointShadowMapSize;
        pointShadowDesc.height      = m_PointShadowMapSize;
        pointShadowDesc.depth       = 1;
        pointShadowDesc.mipLevels   = 1;
        pointShadowDesc.arrayLayers = 1;  // Cubemap 标志自动设 6 层
        pointShadowDesc.usage       = rhi::TextureUsage::DepthStencil
                                    | rhi::TextureUsage::ShaderResource
                                    | rhi::TextureUsage::Cubemap;
        m_PointShadowMap = device->CreateTexture(pointShadowDesc);

        rhi::SamplerDesc pointSampDesc;
        pointSampDesc.minFilter = rhi::FilterMode::Linear;
        pointSampDesc.magFilter = rhi::FilterMode::Linear;
        pointSampDesc.addressU  = rhi::AddressMode::ClampToEdge;
        pointSampDesc.addressV  = rhi::AddressMode::ClampToEdge;
        pointSampDesc.addressW  = rhi::AddressMode::ClampToEdge;
        m_PointShadowSampler = device->CreateSampler(pointSampDesc);
    }

    // --- HDR 离屏渲染目标（RGBA16_FLOAT 颜色 + D32 深度）---
    {
        rhi::TextureDesc hdrColorDesc;
        hdrColorDesc.format = rhi::Format::RGBA16_FLOAT;
        hdrColorDesc.width  = m_HDRWidth;
        hdrColorDesc.height = m_HDRHeight;
        hdrColorDesc.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_HDRTarget = device->CreateTexture(hdrColorDesc);

        rhi::TextureDesc hdrDepthDesc;
        hdrDepthDesc.format = rhi::Format::D32_FLOAT;
        hdrDepthDesc.width  = m_HDRWidth;
        hdrDepthDesc.height = m_HDRHeight;
        hdrDepthDesc.usage  = rhi::TextureUsage::DepthStencil;
        m_HDRDepth = device->CreateTexture(hdrDepthDesc);

        rhi::SamplerDesc hdrSampDesc;
        hdrSampDesc.minFilter = rhi::FilterMode::Linear;
        hdrSampDesc.magFilter = rhi::FilterMode::Linear;
        hdrSampDesc.addressU  = rhi::AddressMode::ClampToEdge;
        hdrSampDesc.addressV  = rhi::AddressMode::ClampToEdge;
        m_HDRSampler = device->CreateSampler(hdrSampDesc);
    }

    // --- 创建占位纹理 + 采样器 ---
    {
        u8 white4[4] = { 255, 255, 255, 255 };
        rhi::TextureDesc whiteTexDesc;
        whiteTexDesc.format      = rhi::Format::RGBA8_UNORM;
        whiteTexDesc.width       = 1;
        whiteTexDesc.height      = 1;
        whiteTexDesc.usage       = rhi::TextureUsage::ShaderResource;
        whiteTexDesc.initialData = white4;
        m_DefaultBaseColorTex = device->CreateTexture(whiteTexDesc);

        rhi::SamplerDesc defaultSampDesc;
        defaultSampDesc.minFilter = rhi::FilterMode::Linear;
        defaultSampDesc.magFilter = rhi::FilterMode::Linear;
        defaultSampDesc.addressU  = rhi::AddressMode::Repeat;
        defaultSampDesc.addressV  = rhi::AddressMode::Repeat;
        m_DefaultBaseColorSampler = device->CreateSampler(defaultSampDesc);
    }

    // --- 分配三缓冲共享描述符集（Phase 1：每帧槽位独立绑定）---
    // 每帧槽位对应一组 Buffer，避免 GPU 正在读取时 CPU 覆盖
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::DescriptorSetHandle set = device->AllocateDescriptorSet(m_DescLayout);
        device->UpdateDescriptorSet(set, 1, rhi::DescriptorType::StorageBuffer,
                                    m_LightBuffers[i].get());
        device->UpdateDescriptorSet(set, 2, rhi::DescriptorType::StorageBuffer,
                                    m_ObjectBuffers[i].get());
        device->UpdateDescriptorSet(set, 3, rhi::DescriptorType::StorageBuffer,
                                    m_ShadowBuffers[i].get());
        // binding 4-9: 静态资源（纹理/采样器），三帧共用
        device->UpdateDescriptorSet(set, 4,
                                    rhi::DescriptorType::CombinedImageSampler,
                                    m_ShadowMap.get(), m_ShadowSampler.get());
        device->UpdateDescriptorSet(set, 5,
                                    rhi::DescriptorType::CombinedImageSampler,
                                    m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());
        device->UpdateDescriptorSet(set, 6,
                                    rhi::DescriptorType::CombinedImageSampler,
                                    m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());
        device->UpdateDescriptorSet(set, 7,
                                    rhi::DescriptorType::CombinedImageSampler,
                                    m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());
        device->UpdateDescriptorSet(set, 8,
                                    rhi::DescriptorType::CombinedImageSampler,
                                    m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());
        device->UpdateDescriptorSet(set, 9,
                                    rhi::DescriptorType::CombinedImageSampler,
                                    m_PointShadowMap.get(), m_PointShadowSampler.get());
        m_DescSets[i] = set;
    }
    // 初始化时使用第一个槽位
    m_CurrentFrameSlot = 0;

    // --- 主管线 PSO ---
    rhi::PushConstantRange pcRange;
    pcRange.stageMask = 1 | 16;     // Vertex | Fragment
    pcRange.offset    = 0;
    pcRange.size      = sizeof(PushConstantData);

    rhi::PipelineStateDesc psoDesc;
    psoDesc.vertexShader         = &m_VS;
    psoDesc.pixelShader          = &m_FS;
    psoDesc.vertexLayout         = vertexLayout;
    psoDesc.topology             = rhi::PrimitiveTopology::TriangleList;
    psoDesc.depthTest            = true;
    psoDesc.depthWrite           = true;
    psoDesc.depthCompare         = rhi::CompareFunc::LessEqual;
    psoDesc.depthFormat          = rhi::Format::D32_FLOAT;      // 匹配 HDR/Shadow 深度附件
    psoDesc.colorAttachmentCount = 1;
    psoDesc.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;  // HDR 离屏目标
    psoDesc.pushConstantRanges   = { pcRange };
    psoDesc.descriptorSetLayouts = { m_DescLayout };
    psoDesc.debugName            = "ForwardPBR";

    m_PBR_PSO = device->CreatePipelineState(psoDesc);
    HE_ASSERT(m_PBR_PSO, "ForwardPipeline: failed to create PBR PSO");

    // --- 阴影 PSO（深度专用：无颜色附件，仅深度写入）---
    // 阴影 VS 仅使用位置属性，声明精简顶点布局避免 Vulkan 验证警告
    rhi::VertexInputLayout shadowVertexLayout;
    shadowVertexLayout.stride = sizeof(he::StaticVertex);
    shadowVertexLayout.attributes = {
        { 0, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, position) },
    };

    rhi::PushConstantRange shadowPCRange;
    shadowPCRange.stageMask = 1 | 16;  // Vertex | Fragment（与 PBR PSO 一致，共享 SetPushConstants 实现）
    shadowPCRange.offset    = 0;
    shadowPCRange.size      = sizeof(ShadowPushConstant);

    rhi::PipelineStateDesc shadowPSODesc;
    shadowPSODesc.vertexShader         = &m_ShadowVS;
    shadowPSODesc.pixelShader          = &m_ShadowFS;
    shadowPSODesc.vertexLayout         = shadowVertexLayout;
    shadowPSODesc.topology             = rhi::PrimitiveTopology::TriangleList;
    shadowPSODesc.depthTest            = true;
    shadowPSODesc.depthWrite           = true;
    shadowPSODesc.depthCompare         = rhi::CompareFunc::LessEqual;
    shadowPSODesc.depthFormat          = rhi::Format::D32_FLOAT;
    shadowPSODesc.colorAttachmentCount = 0;  // 深度专用：无颜色输出
    shadowPSODesc.pushConstantRanges   = { shadowPCRange };
    shadowPSODesc.descriptorSetLayouts = { m_DescLayout };
    shadowPSODesc.debugName            = "ShadowDepth";

    m_ShadowPSO = device->CreatePipelineState(shadowPSODesc);
    HE_ASSERT(m_ShadowPSO, "ForwardPipeline: failed to create Shadow PSO");

    // --- ToneMap 全屏后处理 PSO ---
    {
        rhi::DescriptorSetLayoutDesc tmLayout;
        tmLayout.bindings = {
            { 0, rhi::DescriptorType::CombinedImageSampler, 1, 16 },  // Fragment — HDR 纹理
        };
        m_ToneMapLayout = device->CreateDescriptorSetLayout(tmLayout);

        m_ToneMapSet = device->AllocateDescriptorSet(m_ToneMapLayout);
        device->UpdateDescriptorSet(m_ToneMapSet, 0,
            rhi::DescriptorType::CombinedImageSampler,
            m_HDRTarget.get(), m_HDRSampler.get());

        rhi::PipelineStateDesc tmDesc;
        tmDesc.vertexShader         = &m_ToneMapVS;
        tmDesc.pixelShader          = &m_ToneMapFS;
        tmDesc.topology             = rhi::PrimitiveTopology::TriangleList;
        tmDesc.depthTest            = false;
        tmDesc.depthWrite           = false;
        tmDesc.depthFormat          = rhi::Format::D32_FLOAT;  // 匹配 SwapChain RP 深度附件
        tmDesc.colorAttachmentCount = 1;
        tmDesc.colorFormats[0]      = rhi::Format::BGRA8_UNORM; // 输出到 SwapChain
        tmDesc.descriptorSetLayouts = { m_ToneMapLayout };
        tmDesc.debugName            = "ToneMap";

        m_ToneMapPSO = device->CreatePipelineState(tmDesc);
        HE_ASSERT(m_ToneMapPSO, "ForwardPipeline: failed to create ToneMap PSO");
    }

    // --- 天空盒 PSO（全屏三角形，SV_VertexID，无需 VB/IB）---
    {
        m_SkyboxVS.stage      = rhi::ShaderStage::Vertex;
        m_SkyboxVS.spirv      = k_Skybox_vert_spv;
        m_SkyboxVS.entryPoint = "main";

        m_SkyboxFS.stage      = rhi::ShaderStage::Pixel;
        m_SkyboxFS.spirv      = k_Skybox_frag_spv;
        m_SkyboxFS.entryPoint = "main";

        rhi::DescriptorSetLayoutDesc skyLayout;
        skyLayout.bindings = {
            { 10, rhi::DescriptorType::CombinedImageSampler, 1, 16 },
        };
        m_SkyboxDescLayout = device->CreateDescriptorSetLayout(skyLayout);

        // 天空盒专用 Push Constant：float4x4 + float = 80 bytes
        rhi::PushConstantRange skyPCRange;
        skyPCRange.stageMask = 1 | 16;  // Vertex | Fragment
        skyPCRange.offset    = 0;
        skyPCRange.size      = 96;  // float4x4(64) + float(4) + 16B对齐

        rhi::PipelineStateDesc skyDesc;
        skyDesc.vertexShader         = &m_SkyboxVS;
        skyDesc.pixelShader          = &m_SkyboxFS;
        skyDesc.topology             = rhi::PrimitiveTopology::TriangleList;
        skyDesc.depthTest            = true;
        skyDesc.depthWrite           = false;   // 不写深度
        skyDesc.depthCompare         = rhi::CompareFunc::Equal;  // 仅远平面空白处
        skyDesc.depthFormat          = rhi::Format::D32_FLOAT;
        skyDesc.colorAttachmentCount = 1;
        skyDesc.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
        skyDesc.pushConstantRanges   = { skyPCRange };
        skyDesc.descriptorSetLayouts = { m_SkyboxDescLayout };
        skyDesc.debugName            = "Skybox";

        m_SkyboxPSO = device->CreatePipelineState(skyDesc);
        HE_ASSERT(m_SkyboxPSO, "ForwardPipeline: failed to create Skybox PSO");
    }

    // --- Phase 5-4: 预分配 sec CB 录制池（每线程一个独立 sec CL）---
    if (m_MultiThreadRecord) {
        u32 threadCount = JobSystem::Instance().GetThreadCount();
        u32 secCount = std::min(kMaxSecRecordLists, std::max(threadCount, 1u));
        for (u32 i = 0; i < secCount; ++i) {
            auto secCL = device->CreateSecondaryCommandList();
            if (secCL) m_SecRecordLists.push_back(std::move(secCL));
        }
        HE_CORE_INFO("  Sec record pool: {} lists", m_SecRecordLists.size());
    }

    HE_CORE_INFO("ForwardPipeline initialized (with HDR + Tone Mapping + Skybox)");
}

void ForwardPipeline::Shutdown() {
    if (m_Device) {
        m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    }
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_LightBuffers[i].reset();
        m_ObjectBuffers[i].reset();
        m_ShadowBuffers[i].reset();
    }
    m_AllPerMeshDescSets.clear();
    m_ShadowMap.reset();
    m_ShadowSampler.reset();
    m_PBR_PSO.reset();
    m_ShadowPSO.reset();
    m_ToneMapPSO.reset();
    m_HDRTarget.reset();
    m_HDRDepth.reset();
    m_HDRSampler.reset();
    if (m_Device) m_Device->DestroyDescriptorSetLayout(m_ToneMapLayout);
    if (m_Device) m_Device->DestroyDescriptorSetLayout(m_SkyboxDescLayout);
    m_SkyboxPSO.reset();
    m_SecRecordLists.clear();  // Phase 5-4 sec CB 录制池
    m_Device = nullptr;
    HE_CORE_INFO("ForwardPipeline shut down");
}

rhi::DescriptorSetHandle ForwardPipeline::CreateTextureDescriptorSet(
    rhi::IRHITexture* baseColor, rhi::IRHISampler* bcSampler,
    rhi::IRHITexture* normal,   rhi::IRHISampler* nSampler,
    rhi::IRHITexture* metallicRoughness, rhi::IRHISampler* mrSampler,
    rhi::IRHITexture* occlusion, rhi::IRHISampler* ocSampler)
{
    if (!m_Device) return rhi::kInvalidSet;
    auto set = m_Device->AllocateDescriptorSet(m_DescLayout);
    if (set == rhi::kInvalidSet) return set;

    // 复制共享绑定 1-3（使用第一个槽位的缓冲区，每帧 UpdatePerFrameBindings 会切换）
    m_Device->UpdateDescriptorSet(set, 1, rhi::DescriptorType::StorageBuffer, m_LightBuffers[0].get());
    m_Device->UpdateDescriptorSet(set, 2, rhi::DescriptorType::StorageBuffer, m_ObjectBuffers[0].get());
    m_Device->UpdateDescriptorSet(set, 3, rhi::DescriptorType::StorageBuffer, m_ShadowBuffers[0].get());
    m_Device->UpdateDescriptorSet(set, 4, rhi::DescriptorType::CombinedImageSampler,
        m_ShadowMap.get(), m_ShadowSampler.get());

    // 纹理绑定 5-8（使用默认纹理作为回退）
    auto use = [&](u32 b, rhi::IRHITexture* t, rhi::IRHISampler* s) {
        m_Device->UpdateDescriptorSet(set, b, rhi::DescriptorType::CombinedImageSampler,
            t ? t : m_DefaultBaseColorTex.get(),
            s ? s : m_DefaultBaseColorSampler.get());
    };
    use(5, baseColor, bcSampler);
    use(6, normal, nSampler);
    use(7, metallicRoughness, mrSampler);
    use(8, occlusion, ocSampler);

    // binding 9: 点光源阴影 Cubemap（共享）
    m_Device->UpdateDescriptorSet(set, 9, rhi::DescriptorType::CombinedImageSampler,
        m_PointShadowMap.get(), m_PointShadowSampler.get());

    // 跟踪所有 per-mesh 描述符集，用于每帧更新动态绑定（Phase 1 三缓冲）
    m_AllPerMeshDescSets.push_back(set);

    return set;
}

void ForwardPipeline::NextFrame() {
    // 推进三缓冲槽位（帧首调用，确保 Shadow 和 Scene 使用同一帧的缓冲区）
    m_CurrentFrameSlot = (m_CurrentFrameSlot + 1) % MAX_FRAMES_IN_FLIGHT;

    // 更新所有 per-mesh 描述符集的动态绑定 1-3（指向当前帧的缓冲区）
    rhi::IRHIBuffer* curLightBuf  = m_LightBuffers[m_CurrentFrameSlot].get();
    rhi::IRHIBuffer* curObjectBuf = m_ObjectBuffers[m_CurrentFrameSlot].get();
    rhi::IRHIBuffer* curShadowBuf = m_ShadowBuffers[m_CurrentFrameSlot].get();
    for (auto& set : m_AllPerMeshDescSets) {
        m_Device->UpdateDescriptorSet(set, 1, rhi::DescriptorType::StorageBuffer, curLightBuf);
        m_Device->UpdateDescriptorSet(set, 2, rhi::DescriptorType::StorageBuffer, curObjectBuf);
        m_Device->UpdateDescriptorSet(set, 3, rhi::DescriptorType::StorageBuffer, curShadowBuf);
    }
}

void ForwardPipeline::BeginFrame(rhi::IRHICommandList* cmd, u32 width, u32 height) {
    cmd->SetViewport({
        0, static_cast<float>(height),
        static_cast<float>(width), -static_cast<float>(height),
        0.0f, 1.0f
    });
    cmd->SetScissor({ 0, 0, width, height });
}

void ForwardPipeline::CollectLights(
    PushConstantData& pc,
    std::vector<GPUShadowData>& shadowData,
    he::World& world,
    he::SceneGraph& sg,
    const CameraData& camera)
{
    pc.lightCount = 0;
    shadowData.clear();

    auto collectLight = [&](he::Entity e, he::LightComponent& lc) {
        if (!lc.enabled) return;  // 禁用光源：不参与光照
        if (pc.lightCount >= MAX_LIGHTS) return;
        u32 i = pc.lightCount;

        GPULight gl{};
        gl.colorIntensity  = float4(lc.color, lc.intensity);
        gl.shadowIndex     = -1;  // 默认无阴影

        // 分配阴影槽（如果光源投射阴影）
        if (lc.castShadow && shadowData.size() < MAX_SHADOWS) {
            gl.shadowIndex = static_cast<i32>(shadowData.size());
        }

        switch (lc.type) {
        case he::LightType::Directional: {
            auto* dl = static_cast<he::DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f);
            gl.positionRange = float4(0, 0, 0, 0);

            if (gl.shadowIndex >= 0) {
                float3 lightDir = glm::normalize(dl->direction);

                GPUShadowData sd{};
                sd.lightViewProj = ComputeDirectionalLightViewProj(
                    lightDir, camera);
                sd.shadowParams = float4(lc.shadowBias, lc.shadowNormalBias,
                                         lc.shadowStrength, 0.0f);
                shadowData.push_back(sd);
            }
            break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            gl.positionRange = float4(pos, pl->range);
            gl.directionType = float4(0, -1, 0, 1.0f);

            if (gl.shadowIndex >= 0) {
                // 点光源阴影暂不支持，清除 shadowIndex
                gl.shadowIndex = -1;
            }
            break;
        }
        case he::LightType::Spot: {
            auto* sl = static_cast<he::SpotLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            gl.positionRange = float4(pos, sl->range);
            gl.directionType = float4(sl->direction, 2.0f);
            gl.coneAngles   = float2(sl->innerConeAngle, sl->outerConeAngle);

            if (gl.shadowIndex >= 0) {
                // 聚光灯阴影暂不支持，清除 shadowIndex
                gl.shadowIndex = -1;
            }
            break;
        }
        }

        GPULight* lights = static_cast<GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
        if (lights) lights[i] = gl;
        m_LightBuffers[m_CurrentFrameSlot]->Unmap();
        pc.lightCount++;
    };

    world.ForEach<he::DirectionalLight>(collectLight);
    world.ForEach<he::PointLight>(collectLight);
    world.ForEach<he::SpotLight>(collectLight);

    // 无光源时提供默认方向光
    if (pc.lightCount == 0) {
        pc.lightCount = 1;
        GPULight gl{};
        gl.colorIntensity = float4(1.0f, 0.95f, 0.85f, 5.0f);
        gl.directionType  = float4(0.5f, -1.0f, 1.0f, 0.0f);
        gl.shadowIndex    = -1;
        GPULight* lights = static_cast<GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
        if (lights) lights[0] = gl;
        m_LightBuffers[m_CurrentFrameSlot]->Unmap();
    }

    // 上传阴影 GPU 数据到 SSBO
    if (!shadowData.empty()) {
        GPUShadowData* sd = static_cast<GPUShadowData*>(m_ShadowBuffers[m_CurrentFrameSlot]->Map());
        for (usize j = 0; j < shadowData.size(); ++j) {
            sd[j] = shadowData[j];
        }
        m_ShadowBuffers[m_CurrentFrameSlot]->Unmap();
    }
}

// 公开方法：仅收集阴影投射光源（供外部渲染循环在 BeginRenderPass 前调用）
void ForwardPipeline::CollectShadowLights(
    he::World& world, he::SceneGraph& sg,
    std::vector<const he::LightComponent*>& shadowLights,
    std::vector<GPUShadowData>& shadowGPUData,
    const CameraData& camera)
{
    shadowLights.clear();
    shadowGPUData.clear();

    world.ForEach<he::DirectionalLight>([&](he::Entity e, he::DirectionalLight& lc) {
        if (!lc.enabled) return;
        if (!lc.castShadow) return;
        if (shadowGPUData.size() >= MAX_SHADOWS) return;

        float3 lightDir = glm::normalize(lc.direction);

        GPUShadowData sd{};
        sd.lightViewProj = ComputeDirectionalLightViewProj(lightDir, camera);
        sd.shadowParams  = float4(lc.shadowBias, lc.shadowNormalBias, lc.shadowStrength, 0.0f);
        shadowGPUData.push_back(sd);
        shadowLights.push_back(&lc);
    });

    // 收集点光源阴影
    world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& lc) {
        if (!lc.enabled) return;    // 禁用光源不投射阴影
        if (!lc.castShadow) return;
        if (shadowGPUData.size() >= MAX_SHADOWS) return;

        float3 lightPos = sg.GetWorldPosition(e);

        GPUShadowData sd{};
        // 复用 lightViewProj 存储点光数据：col0.xyz=位置, col0.w=范围
        sd.lightViewProj[0] = float4(lightPos, lc.range);
        sd.shadowParams  = float4(lc.shadowBias, lc.shadowNormalBias, lc.shadowStrength, 1.0f);  // w=1 表示点光
        shadowGPUData.push_back(sd);
        shadowLights.push_back(&lc);
    });

    // 上传阴影 GPU 数据到 SSBO（供阴影 VS 和主 PS 读取）
    if (!shadowGPUData.empty()) {
        GPUShadowData* sd = static_cast<GPUShadowData*>(m_ShadowBuffers[m_CurrentFrameSlot]->Map());
        for (usize j = 0; j < shadowGPUData.size(); ++j)
            sd[j] = shadowGPUData[j];
        m_ShadowBuffers[m_CurrentFrameSlot]->Unmap();
    }
}

// 开始离屏阴影渲染通道（在主管线 BeginRenderPass 之前调用）
void ForwardPipeline::BeginShadowPass(rhi::IRHICommandList* cmd) {
    // 设置 Shadow PSO（depth-only render pass）
    cmd->SetPipeline(m_ShadowPSO.get());

    // 获取阴影贴图的原生 ImageView
    void* depthView = m_ShadowMap->GetNativeHandle();
    if (!depthView) {
        HE_CORE_ERROR("BeginShadowPass: ShadowMap ImageView 无效");
        return;
    }

    rhi::ClearValue clearVal{};
    clearVal.depth = 1.0f;  // 远平面 = 无遮挡

    cmd->BeginOffscreenPass(nullptr, depthView, m_ShadowMapSize, m_ShadowMapSize, &clearVal);
}

// 结束离屏阴影渲染通道并执行布局转换
void ForwardPipeline::EndShadowPass(rhi::IRHICommandList* cmd) {
    cmd->EndOffscreenPass();

    // 布局转换：DEPTH_STENCIL_READ_ONLY_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
    // 注意：srcState 必须与 RenderPass 的 finalLayout 一致
    cmd->PipelineBarrier(
        rhi::PipelineStage::LateFragmentTests,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::DepthStencilRead,  // 匹配 RP finalLayout
        rhi::ResourceState::ShaderResource,
        m_ShadowMap.get());

    // 恢复 PBR PSO（确保后续 BeginRenderPass 使用正确的 render pass）
    cmd->SetPipeline(m_PBR_PSO.get());
}

// ============================================================
// HDR 离屏渲染 + ToneMap 后处理
// ============================================================

void ForwardPipeline::BeginHDRPass(rhi::IRHICommandList* cmd, u32 width, u32 height) {
    // 首次使用：确保点光 Cubemap 布局从 Undefined 转换到 ShaderResource
    if (!m_PointShadowInitDone) {
        m_PointShadowInitDone = true;
        cmd->PipelineBarrier(
            rhi::PipelineStage::TopOfPipe,
            rhi::PipelineStage::FragmentShader,
            rhi::ResourceState::Undefined,
            rhi::ResourceState::ShaderResource,
            m_PointShadowMap.get());
    }

    cmd->SetPipeline(m_PBR_PSO.get());

    void* colorView = m_HDRTarget->GetNativeHandle();
    void* depthView = m_HDRDepth->GetNativeHandle();

    rhi::ClearValue clear{};
    clear.depth = 1.0f;

    cmd->BeginOffscreenPass(colorView, depthView, width, height, &clear);

    cmd->SetViewport({ 0, static_cast<float>(height),
        static_cast<float>(width), -static_cast<float>(height), 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, width, height });
}

void ForwardPipeline::EndHDRPass(rhi::IRHICommandList* cmd) {
    cmd->EndOffscreenPass();

    // 布局转换：Present（RP finalLayout）→ 着色器只读（ToneMap 采样）
    // 注意：当前 RP 颜色附件 finalLayout 固定为 PRESENT_SRC_KHR
    cmd->PipelineBarrier(
        rhi::PipelineStage::ColorAttachmentOutput,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::Present,
        rhi::ResourceState::ShaderResource,
        m_HDRTarget.get());

    // 预置 ToneMap PSO，确保后续 BeginRenderPass 使用正确的 SwapChain RP（B8G8R8A8）
    cmd->SetPipeline(m_ToneMapPSO.get());
}

void ForwardPipeline::RenderSkybox(rhi::IRHICommandList* cmd, he::World& world,
                                    const CameraData& camera) {
    he::SkyboxComponent* skybox = nullptr;
    world.ForEach<he::SkyboxComponent>([&](he::Entity, he::SkyboxComponent& sc) {
        if (sc.enabled && sc.GetCubemap()) skybox = &sc;
    });
    if (!skybox) return;

    // 描述符集只分配一次，复用（cubemap/sampler 不变）
    static rhi::DescriptorSetHandle s_CachedSkySet = rhi::kInvalidSet;
    static he::SkyboxComponent* s_CachedSkybox = nullptr;
    if (s_CachedSkySet == rhi::kInvalidSet || s_CachedSkybox != skybox) {
        s_CachedSkySet = m_Device->AllocateDescriptorSet(m_SkyboxDescLayout);
        m_Device->UpdateDescriptorSet(s_CachedSkySet, 10,
            rhi::DescriptorType::CombinedImageSampler,
            skybox->GetCubemap(), skybox->GetCubemapSampler());
        s_CachedSkybox = skybox;
    }
    auto skySet = s_CachedSkySet;

    // 计算旋转视图的逆 ViewProj（相机置于原点，去除平移影响）
    // 直接清零 viewProj[3] 是错误的——V 的平移已被 P 乘积分布到多列
    float4x4 viewRotOnly     = glm::lookAtRH(float3(0.0f), camera.forward, camera.up);
    float4x4 viewProjNoTrans = camera.GetProjMatrix() * viewRotOnly;
    float4x4 invViewProj     = glm::inverse(viewProjNoTrans);

    // Push Constants: float4x4(64) + float(4) + 28B对齐 = 96B
    struct alignas(16) SkyboxPC { float4x4 invVP; float intensity; float _pad[7]; } pc{};
    pc.invVP     = invViewProj;
    pc.intensity = skybox->intensity;

    cmd->SetPipeline(m_SkyboxPSO.get());
    cmd->BindDescriptorSet(0, skySet);
    cmd->SetPushConstants(0, sizeof(SkyboxPC), &pc);
    cmd->Draw(3);  // 全屏三角形，3 顶点（SV_VertexID 驱动）
}

void ForwardPipeline::RenderToneMapPass(rhi::IRHICommandList* cmd) {
    cmd->SetPipeline(m_ToneMapPSO.get());

    // 显式设置视口 + 裁剪（BeginRenderPass 不会自动设置，需匹配 SwapChain 尺寸）
    // 当前 SwapChain 尺寸由 m_HDRWidth/Height 近似（resize 时同步更新）
    cmd->SetViewport({ 0, static_cast<float>(m_HDRHeight),
        static_cast<float>(m_HDRWidth), -static_cast<float>(m_HDRHeight), 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, m_HDRWidth, m_HDRHeight });

    cmd->BindDescriptorSet(0, m_ToneMapSet);
    cmd->Draw(3);  // 全屏三角形
}

void ForwardPipeline::ResizeHDRTarget(u32 width, u32 height) {
    if (width == m_HDRWidth && height == m_HDRHeight) return;
    m_HDRWidth  = width;
    m_HDRHeight = height;

    // 重建 HDR 颜色纹理
    rhi::TextureDesc hdrColorDesc;
    hdrColorDesc.format = rhi::Format::RGBA16_FLOAT;
    hdrColorDesc.width  = m_HDRWidth;
    hdrColorDesc.height = m_HDRHeight;
    hdrColorDesc.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_HDRTarget = m_Device->CreateTexture(hdrColorDesc);

    // 重建 HDR 深度纹理
    rhi::TextureDesc hdrDepthDesc;
    hdrDepthDesc.format = rhi::Format::D32_FLOAT;
    hdrDepthDesc.width  = m_HDRWidth;
    hdrDepthDesc.height = m_HDRHeight;
    hdrDepthDesc.usage  = rhi::TextureUsage::DepthStencil;
    m_HDRDepth = m_Device->CreateTexture(hdrDepthDesc);

    // 更新 ToneMap 描述符集指向新纹理
    m_Device->UpdateDescriptorSet(m_ToneMapSet, 0,
        rhi::DescriptorType::CombinedImageSampler,
        m_HDRTarget.get(), m_HDRSampler.get());
}

// ============================================================
// 点光源阴影 — Cubemap 6 面透视渲染
// ============================================================

// Cubemap 6 面方向 + 上向量（Vulkan 右手坐标系）
struct CubemapFace {
    float3 dir;
    float3 up;
};
static const CubemapFace kCubeFaces[6] = {
    { float3( 1, 0, 0), float3(0,-1, 0) },  // +X (right)
    { float3(-1, 0, 0), float3(0,-1, 0) },  // -X (left)
    { float3( 0, 1, 0), float3(0, 0, 1) },  // +Y (top)
    { float3( 0,-1, 0), float3(0, 0,-1) },  // -Y (bottom)
    { float3( 0, 0, 1), float3(0,-1, 0) },  // +Z (front)
    { float3( 0, 0,-1), float3(0,-1, 0) },  // -Z (back)
};

void ForwardPipeline::RenderPointShadowPass(
    rhi::IRHICommandList* cmd,
    he::World& world,
    he::SceneGraph& sceneGraph,
    const std::vector<const he::LightComponent*>& pointLights,
    const std::vector<GPUShadowData>& pointShadowData)
{
    if (pointShadowData.empty() || !m_PointShadowMap) return;

    // 对每个投射阴影的点光源
    for (usize li = 0; li < pointShadowData.size() && li < pointLights.size(); ++li) {
        const GPUShadowData& sd = pointShadowData[li];
        float3 lightPos = float3(sd.lightViewProj[0]);
        float  range    = sd.lightViewProj[0].w;

        // 6 面透视投影（90° FOV，宽高比 1:1）
        float4x4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, range);

        for (u32 face = 0; face < 6; ++face) {
            // 视图矩阵：从光源位置看向该面方向
            float4x4 view = glm::lookAtRH(lightPos,
                lightPos + kCubeFaces[face].dir,
                kCubeFaces[face].up);
            float4x4 viewProj = proj * view;

            // 设置 Shadow PSO → BeginOffscreenPass（每面独立 depth-only RP）
            cmd->SetPipeline(m_ShadowPSO.get());

            void* faceView = m_PointShadowMap->GetNativeHandle(face);
            if (!faceView) continue;

            rhi::ClearValue clearVal{};
            clearVal.depth = 1.0f;
            cmd->BeginOffscreenPass(nullptr, faceView,
                m_PointShadowMapSize, m_PointShadowMapSize, &clearVal);

            // 视口 + 裁剪
            cmd->SetViewport({ 0, static_cast<float>(m_PointShadowMapSize),
                static_cast<float>(m_PointShadowMapSize),
                -static_cast<float>(m_PointShadowMapSize), 0.0f, 1.0f });
            cmd->SetScissor({ 0, 0, m_PointShadowMapSize, m_PointShadowMapSize });

            // 绑定描述符集
            cmd->BindDescriptorSet(0, m_DescSets[m_CurrentFrameSlot]);

            // 绘制所有几何体
            ShadowPushConstant shadowPC{};
            shadowPC.lightViewProj = viewProj;

            auto renderMeshForPointShadow = [&](he::Entity e, he::MeshComponent& mesh) {
                if (mesh.GetIndexCount() == 0) return;
                shadowPC.objectIndex = 0;

                float4x4 worldMatrix = sceneGraph.GetWorldMatrix(e);
                GPUObjectData* objData = static_cast<GPUObjectData*>(m_ObjectBuffers[m_CurrentFrameSlot]->Map());
                objData[0].worldMatrix = worldMatrix;
                m_ObjectBuffers[m_CurrentFrameSlot]->Unmap();

                cmd->SetPushConstants(0, sizeof(ShadowPushConstant), &shadowPC);
                cmd->SetVertexBuffer(mesh.GetVertexBuffer().get(), 0);
                cmd->SetIndexBuffer(mesh.GetIndexBuffer().get());
                cmd->DrawIndexed(mesh.GetIndexCount());
            };

            world.ForEach<he::MeshComponent>(renderMeshForPointShadow);
            world.ForEach<he::CubeComponent>([&](he::Entity e, he::CubeComponent& c) {
                renderMeshForPointShadow(e, static_cast<he::MeshComponent&>(c));
            });
            world.ForEach<he::SphereComponent>([&](he::Entity e, he::SphereComponent& s) {
                renderMeshForPointShadow(e, static_cast<he::MeshComponent&>(s));
            });

            cmd->EndOffscreenPass();

            // 每面渲染后执行布局转换（深度写入 → 着色器资源）
            cmd->PipelineBarrier(
                rhi::PipelineStage::LateFragmentTests,
                rhi::PipelineStage::FragmentShader,
                rhi::ResourceState::DepthStencilRead,
                rhi::ResourceState::ShaderResource,
                m_PointShadowMap.get());
        }
    }

    // 恢复 PBR PSO
    cmd->SetPipeline(m_PBR_PSO.get());
}

void ForwardPipeline::RenderShadowPass(
    rhi::IRHICommandList* cmd,
    he::World& world,
    he::SceneGraph& sceneGraph,
    const std::vector<const he::LightComponent*>& /*shadowLights*/,
    const std::vector<GPUShadowData>& shadowGPUData)
{
    if (shadowGPUData.empty()) return;

    // 阴影通道目前每个阴影光源一个深度贴图，但先实现单光源简化版
    // 对于每个投射阴影的方向光，渲染场景到其深度贴图
    // 注：当前简化实现 — 仅渲染到单个阴影贴图（最后处理的光源覆盖前面的）

    // 绑定阴影 PSO
    cmd->SetPipeline(m_ShadowPSO.get());

    // 绑定描述符集（阴影着色器使用与主管线相同的对象数据）
    cmd->BindDescriptorSet(0, m_DescSets[m_CurrentFrameSlot]);

    // 阴影通道视口 + 裁剪
    cmd->SetViewport({ 0, static_cast<float>(m_ShadowMapSize),
                        static_cast<float>(m_ShadowMapSize),
                        -static_cast<float>(m_ShadowMapSize), 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, m_ShadowMapSize, m_ShadowMapSize });

    // 为每个阴影投射光源渲染场景深度
    // 当前版本：仅处理第一个阴影投射方向光
    const GPUShadowData& sm = shadowGPUData[0];

    ShadowPushConstant shadowPC{};
    shadowPC.lightViewProj = sm.lightViewProj;

    // 一次性映射对象缓冲区，逐实体分配独立 objectIndex（避免槽位覆盖冲突）
    GPUObjectData* objData = static_cast<GPUObjectData*>(
        m_ObjectBuffers[m_CurrentFrameSlot]->Map());
    u32 objectIndex = 0;

    auto renderMeshForShadow = [&](he::Entity e, he::MeshComponent& mesh) {
        if (mesh.GetIndexCount() == 0) return;
        if (objectIndex >= MAX_OBJECTS) return;

        float4x4 worldMatrix = sceneGraph.GetWorldMatrix(e);

        // 写入独立槽位，不再所有实体共用 objData[0]
        objData[objectIndex].worldMatrix = worldMatrix;
        shadowPC.objectIndex = objectIndex;
        objectIndex++;

        cmd->SetPushConstants(0, sizeof(ShadowPushConstant), &shadowPC);
        cmd->SetVertexBuffer(mesh.GetVertexBuffer().get(), 0);
        cmd->SetIndexBuffer(mesh.GetIndexBuffer().get());
        cmd->DrawIndexed(mesh.GetIndexCount());
    };

    world.ForEach<he::MeshComponent>(renderMeshForShadow);
    world.ForEach<he::CubeComponent>([&](he::Entity e, he::CubeComponent& c) {
        renderMeshForShadow(e, static_cast<he::MeshComponent&>(c));
    });
    world.ForEach<he::SphereComponent>([&](he::Entity e, he::SphereComponent& s) {
        renderMeshForShadow(e, static_cast<he::MeshComponent&>(s));
    });

    m_ObjectBuffers[m_CurrentFrameSlot]->Unmap();
}

void ForwardPipeline::RenderScene(
    rhi::IRHICommandList* cmd,
    he::World& world,
    he::SceneGraph& sceneGraph,
    const CameraData& camera)
{
    sceneGraph.UpdateTransforms();

    float4x4 viewProj = camera.GetViewProjMatrix();
    u32 drawCount = 0;

    // 帧级 push constant
    PushConstantData framePC{};
    framePC.viewProjMatrix = viewProj;
    framePC.cameraPosition = float4(camera.position, 0.0f);

    // 收集光源 + 阴影数据
    std::vector<GPUShadowData> shadowGPUData;
    std::vector<const he::LightComponent*> shadowLights;
    CollectLights(framePC, shadowGPUData, world, sceneGraph, camera);

    // ============================================================
    // Phase 5-3: 多线程视锥剔除
    // ============================================================

    // Step 1: 单线程收集所有可绘制实体 + 预计算世界空间包围盒
    struct DrawableEntity {
        he::Entity        entity;
        he::MeshComponent* mesh;
        float4x4          worldMatrix;  // 缓存，避免 GPU 上传时重复计算
        AABB              worldBounds;
    };
    std::vector<DrawableEntity> drawables;

    auto gatherEntity = [&](he::Entity e, he::MeshComponent& m) {
        if (m.GetIndexCount() == 0) return;
        float4x4 wm = sceneGraph.GetWorldMatrix(e);
        drawables.push_back({e, &m, wm, m.GetBounds().Transform(wm)});
    };
    world.ForEach<he::MeshComponent>(gatherEntity);
    world.ForEach<he::CubeComponent>([&](he::Entity e, he::CubeComponent& c) {
        gatherEntity(e, static_cast<he::MeshComponent&>(c));
    });
    world.ForEach<he::SphereComponent>([&](he::Entity e, he::SphereComponent& s) {
        gatherEntity(e, static_cast<he::MeshComponent&>(s));
    });

    u32 totalEntities = static_cast<u32>(drawables.size());

    // Step 2: 并行视锥剔除（chunk=64，每线程独立收集后合并）
    Frustum frustum = camera.GetFrustum();
    std::mutex visibleMutex;
    std::vector<DrawableEntity> visible;
    visible.reserve(totalEntities);

    if (totalEntities > 0) {
        JobSystem::Instance().ParallelForChunked(
            totalEntities, 64,
            [&](u32 start, u32 end) {
                std::vector<DrawableEntity> local;
                local.reserve(end - start);
                for (u32 i = start; i < end; ++i) {
                    if (frustum.Intersects(drawables[i].worldBounds))
                        local.push_back(drawables[i]);
                }
                if (!local.empty()) {
                    std::lock_guard<std::mutex> lock(visibleMutex);
                    visible.insert(visible.end(), local.begin(), local.end());
                }
            });
    }

    // Step 3: 上传 GPU 数据（单线程）+ 录制绘制命令（单/多线程可选）
    GPUObjectData* objData = static_cast<GPUObjectData*>(
        m_ObjectBuffers[m_CurrentFrameSlot]->Map());
    u32 objectIndex = 0;

    // --- 3a: 单线程上传所有可见实体的材质到 Object Buffer ---
    for (auto& de : visible) {
        if (objectIndex >= MAX_OBJECTS) break;

        PBRMaterial mat = GetDefaultMaterial();
        mat.baseColorFactor = de.mesh->baseColorFactor;
        mat.emissiveFactor  = de.mesh->emissiveFactor;
        mat.metallicFactor  = de.mesh->metallicFactor;
        mat.roughnessFactor = de.mesh->roughnessFactor;
        mat.aoFactor        = de.mesh->aoFactor;
        mat.alphaCutoff     = de.mesh->alphaCutoff;
        mat.alphaMode       = static_cast<AlphaMode>(de.mesh->alphaMode);
        mat.doubleSided     = de.mesh->doubleSided;
        mat.unlit           = de.mesh->unlit;

        GPUObjectData& obj = objData[objectIndex];
        obj.worldMatrix = de.worldMatrix;
        FillObjectData(obj, mat);
        objectIndex++;
    }

    u32 totalDraws = objectIndex;
    m_ObjectBuffers[m_CurrentFrameSlot]->Unmap();

    // --- 3b: 录制绘制命令 ---
    if (m_MultiThreadRecord && !m_SecRecordLists.empty() && totalDraws > 0) {
        // Phase 5-4: 多线程并行录制 sec CB，主 CB 统一执行
        u32 numThreads = std::min((u32)m_SecRecordLists.size(), totalDraws);
        u32 chunkSize  = (totalDraws + numThreads - 1) / numThreads;

        std::vector<std::function<void()>> tasks;
        tasks.reserve(numThreads);
        for (u32 t = 0; t < numThreads; ++t) {
            tasks.push_back([&, t]() {
                u32 start = t * chunkSize;
                u32 end   = std::min(start + chunkSize, totalDraws);
                if (start >= end) return;

                auto& secCmd = m_SecRecordLists[t];
                secCmd->BeginSecondary(m_PBR_PSO.get());

                for (u32 i = start; i < end; ++i) {
                    auto& de = visible[i];
                    PushConstantData pc = framePC;
                    pc.objectIndex = i;

                    if (de.mesh->GetDescriptorSet() != rhi::kInvalidSet)
                        secCmd->BindDescriptorSet(0, de.mesh->GetDescriptorSet());
                    else
                        secCmd->BindDescriptorSet(0, m_DescSets[m_CurrentFrameSlot]);

                    secCmd->SetPushConstants(0, sizeof(PushConstantData), &pc);
                    secCmd->SetVertexBuffer(de.mesh->GetVertexBuffer().get(), 0);
                    secCmd->SetIndexBuffer(de.mesh->GetIndexBuffer().get());
                    secCmd->DrawIndexed(de.mesh->GetIndexCount());
                }

                secCmd->End();
            });
        }

        JobSystem::Instance().ParallelInvoke(tasks);

        // 主 CB 统一执行所有 sec CB
        for (u32 t = 0; t < numThreads; ++t) {
            u32 start = t * chunkSize;
            if (start >= totalDraws) continue;
            cmd->ExecuteSecondary(m_SecRecordLists[t].get());
        }

        drawCount = totalDraws;
    } else {
        // 单线程录制（回退路径）
        objectIndex = 0;
        for (auto& de : visible) {
            if (objectIndex >= MAX_OBJECTS) break;

            framePC.objectIndex = objectIndex;
            objectIndex++;

            if (de.mesh->GetDescriptorSet() != rhi::kInvalidSet)
                cmd->BindDescriptorSet(0, de.mesh->GetDescriptorSet());
            else
                cmd->BindDescriptorSet(0, m_DescSets[m_CurrentFrameSlot]);

            DrawMesh(cmd, de.mesh, de.worldMatrix, viewProj, GetDefaultMaterial(), camera, framePC);
            drawCount++;
        }
    }

    static bool s_FirstFrame = true;
    if (s_FirstFrame) {
        HE_CORE_INFO("ForwardPipeline::RenderScene: {} draws, {} lights, {} shadows (Phase 5-3 frustum cull)",
            drawCount, framePC.lightCount, shadowGPUData.size());
        s_FirstFrame = false;
    }
}

void ForwardPipeline::EndFrame(rhi::IRHICommandList* /*cmd*/) {
}

void ForwardPipeline::DrawMesh(
    rhi::IRHICommandList* cmd,
    he::MeshComponent* mesh,
    const float4x4& /*worldMatrix*/,
    const float4x4& /*viewProjMatrix*/,
    const PBRMaterial& /*material*/,
    const CameraData& /*camera*/,
    const PushConstantData& framePC)
{
    if (!mesh || mesh->GetIndexCount() == 0) return;

    cmd->SetPushConstants(0, sizeof(PushConstantData), &framePC);
    cmd->SetVertexBuffer(mesh->GetVertexBuffer().get(), 0);
    cmd->SetIndexBuffer(mesh->GetIndexBuffer().get());
    cmd->DrawIndexed(mesh->GetIndexCount());
}

} // namespace he::render
