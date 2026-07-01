#include "Pipeline/ForwardPipeline.h"
#include "GI/GI_IBL.h"
#include "Shadow/ShadowSystem.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Threading/JobSystem.h"
// 按需包含 Shader SPV（修改单个 shader 只重编译 ForwardPipeline.cpp）
#include "PBR.vert.spv.h"
#include "PBR.frag.spv.h"
#include "ToneMap.vert.spv.h"
#include "ToneMap.frag.spv.h"
#include "Skybox.vert.spv.h"
#include "Skybox.frag.spv.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>  // orthoRH_ZO (Vulkan Z [0,1])

#include <mutex>

namespace he::render {

ForwardPipeline::ForwardPipeline() {
}

ForwardPipeline::~ForwardPipeline() {
    Shutdown();
}

bool ForwardPipeline::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    HE_ASSERT(m_Device, "ForwardPipeline: device is null");

    // --- PBR 着色器 ---
    m_VS.stage      = rhi::ShaderStage::Vertex;
    m_VS.spirv      = k_PBR_vert_spv;
    m_VS.entryPoint = "main";

    m_FS.stage      = rhi::ShaderStage::Pixel;
    m_FS.spirv      = k_PBR_frag_spv;
    m_FS.entryPoint = "main";

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

    // --- 阴影子系统初始化（必须在描述符集之前，因为需要纹理/缓冲区访问器）---
    m_ShadowSystem = std::make_unique<ShadowSystem>();
    m_ShadowSystem->Initialize(device, 0, 0);
    HE_CORE_INFO("ForwardPipeline: ShadowSystem initialized");

    // --- 主管线 DescriptorSetLayout ---
    rhi::DescriptorSetLayoutDesc combinedLayoutDesc;
    combinedLayoutDesc.bindings = {
        { 1, rhi::DescriptorType::StorageBuffer,        1, 16 },
        { 2, rhi::DescriptorType::StorageBuffer,        1, 17 },
        { 3, rhi::DescriptorType::StorageBuffer,        1, 16 },
        { 4, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // CSM cascade 0
        { 5, rhi::DescriptorType::CombinedImageSampler,  1, 16 },
        { 6, rhi::DescriptorType::CombinedImageSampler,  1, 16 },
        { 7, rhi::DescriptorType::CombinedImageSampler,  1, 16 },
        { 8, rhi::DescriptorType::CombinedImageSampler,  1, 16 },
        { 9, rhi::DescriptorType::CombinedImageSampler,  1, 16 },
        { 10, rhi::DescriptorType::CombinedImageSampler, 1, 16 },  // CSM cascade 1
        { 11, rhi::DescriptorType::CombinedImageSampler, 1, 16 },  // CSM cascade 2
        { 12, rhi::DescriptorType::CombinedImageSampler, 1, 16 },  // IBL Irradiance Cubemap
        { 13, rhi::DescriptorType::CombinedImageSampler, 1, 16 },  // IBL Prefilter Cubemap
        { 14, rhi::DescriptorType::CombinedImageSampler, 1, 16 },  // IBL BRDF LUT
    };
    m_DescLayout = device->CreateDescriptorSetLayout(combinedLayoutDesc);

    // 用管线的描述符集布局创建 Shadow PSO（复用同一布局，与 PBR Shader 兼容）
    m_ShadowSystem->CreateShadowPSO(m_DescLayout);

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

    // --- 创建纹理采样器（通用）---
    // 基础色纹理 + 采样器（默认 1×1 白色）
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
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::DescriptorSetHandle set = device->AllocateDescriptorSet(m_DescLayout);
        device->UpdateDescriptorSet(set, 1, rhi::DescriptorType::StorageBuffer,
                                    m_LightBuffers[i].get());
        device->UpdateDescriptorSet(set, 2, rhi::DescriptorType::StorageBuffer,
                                    m_ObjectBuffers[i].get());
        // 绑定 3: 阴影数据 SSBO（ForwardPipeline 自有缓冲，每帧独立槽位）
        device->UpdateDescriptorSet(set, 3, rhi::DescriptorType::StorageBuffer,
                                    m_ShadowBuffers[i].get());
        // CSM: 绑定 3 级联阴影贴图（来自 ShadowSystem）
        for (u32 c = 0; c < CASCADE_COUNT; ++c) {
            u32 binding = (c == 0) ? 4u : (c == 1 ? 10u : 11u);
            device->UpdateDescriptorSet(set, binding, rhi::DescriptorType::CombinedImageSampler,
                m_ShadowSystem->GetCSMShadowMap(c), m_ShadowSystem->GetShadowSampler());
        }
        // 绑定 5-8: 材质纹理占位（运行时通过 CreateTextureDescriptorSet 替换）
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
        // 绑定 9: 点光源阴影 Cubemap（来自 ShadowSystem）
        device->UpdateDescriptorSet(set, 9,
            rhi::DescriptorType::CombinedImageSampler,
            m_ShadowSystem->GetPointShadowMap(), m_ShadowSystem->GetPointShadowSampler());
        // 绑定 12-14: IBL 纹理占位（GI_IBL 生成后通过 UpdateIBLBindings 替换）
        device->UpdateDescriptorSet(set, 12,
            rhi::DescriptorType::CombinedImageSampler,
            m_ShadowSystem->GetPointShadowMap(), m_ShadowSystem->GetPointShadowSampler());
        device->UpdateDescriptorSet(set, 13,
            rhi::DescriptorType::CombinedImageSampler,
            m_ShadowSystem->GetPointShadowMap(), m_ShadowSystem->GetPointShadowSampler());
        device->UpdateDescriptorSet(set, 14,
            rhi::DescriptorType::CombinedImageSampler,
            m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());
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

    // --- GI 子系统（IBL）---
    {
        auto gi = std::make_unique<GI_IBL>();
        gi->Initialize(device, 0, 0);  // IBL 分辨率独立于视口
        m_GI = std::move(gi);
        HE_CORE_INFO("ForwardPipeline: GI_IBL initialized");
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

    HE_CORE_INFO("ForwardPipeline initialized (with HDR + Tone Mapping + Skybox + ShadowSystem)");
    return true;
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
    m_PBR_PSO.reset();
    m_ToneMapPSO.reset();
    m_HDRTarget.reset();
    m_HDRDepth.reset();
    m_HDRSampler.reset();
    if (m_Device) m_Device->DestroyDescriptorSetLayout(m_ToneMapLayout);
    if (m_Device) m_Device->DestroyDescriptorSetLayout(m_SkyboxDescLayout);
    m_SkyboxPSO.reset();
    m_SecRecordLists.clear();  // Phase 5-4 sec CB 录制池

    // 阴影子系统
    if (m_ShadowSystem) {
        m_ShadowSystem->Shutdown();
        m_ShadowSystem.reset();
    }

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

    // 复制共享绑定 1-2（使用第一个槽位的缓冲区，每帧 NextFrame 会切换）
    m_Device->UpdateDescriptorSet(set, 1, rhi::DescriptorType::StorageBuffer, m_LightBuffers[0].get());
    m_Device->UpdateDescriptorSet(set, 2, rhi::DescriptorType::StorageBuffer, m_ObjectBuffers[0].get());
    // 绑定 3: 阴影数据 SSBO（ForwardPipeline 自有缓冲）
    m_Device->UpdateDescriptorSet(set, 3, rhi::DescriptorType::StorageBuffer,
        m_ShadowBuffers[0].get());
    // CSM: per-mesh set 绑定 3 级联（来自 ShadowSystem）
    for (u32 c = 0; c < CASCADE_COUNT; ++c) {
        u32 binding = (c == 0) ? 4u : (c == 1 ? 10u : 11u);
        m_Device->UpdateDescriptorSet(set, binding, rhi::DescriptorType::CombinedImageSampler,
            m_ShadowSystem->GetCSMShadowMap(c), m_ShadowSystem->GetShadowSampler());
    }

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

    // 绑定 9: 点光源阴影 Cubemap（来自 ShadowSystem）
    m_Device->UpdateDescriptorSet(set, 9, rhi::DescriptorType::CombinedImageSampler,
        m_ShadowSystem->GetPointShadowMap(), m_ShadowSystem->GetPointShadowSampler());

    // 跟踪所有 per-mesh 描述符集，用于每帧更新动态绑定（Phase 1 三缓冲）
    m_AllPerMeshDescSets.push_back(set);

    return set;
}

void ForwardPipeline::NextFrame() {
    // 推进三缓冲槽位（帧首调用，确保 Shadow 和 Scene 使用同一帧的缓冲区）
    m_CurrentFrameSlot = (m_CurrentFrameSlot + 1) % MAX_FRAMES_IN_FLIGHT;

    // 同步阴影子系统帧槽位
    m_ShadowSystem->NextFrame();

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
    he::World& world,
    he::SceneGraph& sg,
    const CameraData& camera)
{
    pc.lightCount = 0;

    auto collectLight = [&](he::Entity e, he::LightComponent& lc) {
        if (!lc.enabled) return;  // 禁用光源：不参与光照
        u32 i = pc.lightCount;
        if (i >= MAX_LIGHTS) return;

        GPULight gl{};
        gl.colorIntensity  = float4(lc.color, lc.intensity);
        gl.shadowIndex     = m_ShadowSystem->GetShadowIndex(e);  // 从 ShadowSystem 获取阴影索引

        switch (lc.type) {
        case he::LightType::Directional: {
            auto* dl = static_cast<he::DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f);
            gl.positionRange = float4(0, 0, 0, 0);
            break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            gl.positionRange = float4(pos, pl->range);
            gl.directionType = float4(0, -1, 0, 1.0f);
            break;
        }
        case he::LightType::Spot: {
            auto* sl = static_cast<he::SpotLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            gl.positionRange = float4(pos, sl->range);
            gl.directionType = float4(sl->direction, 2.0f);
            gl.coneAngles   = float2(sl->innerConeAngle, sl->outerConeAngle);
            break;
        }
        }

        GPULight* lights = static_cast<GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
        if (lights) lights[i] = gl;
        m_LightBuffers[m_CurrentFrameSlot]->Unmap();
        pc.lightCount++;  // lightCount++
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
}

// ============================================================
// HDR 离屏渲染 + ToneMap 后处理
// ============================================================

void ForwardPipeline::BeginHDRPass(rhi::IRHICommandList* cmd, u32 width, u32 height) {
    // 同步当前渲染尺寸（多线程路径 / ToneMap 视口需要）
    if (width != m_HDRWidth || height != m_HDRHeight) {
        ResizeHDRTarget(width, height);
    }

    cmd->SetPipeline(m_PBR_PSO.get());

    void* colorView = m_HDRTarget->GetNativeHandle();
    void* depthView = m_HDRDepth->GetNativeHandle();

    rhi::ClearValue clear{};
    clear.depth = 1.0f;

    cmd->BeginOffscreenPass(colorView, depthView, width, height, &clear, true);

    cmd->SetViewport({ 0, static_cast<float>(height),
        static_cast<float>(width), -static_cast<float>(height), 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, width, height });
}

void ForwardPipeline::EndHDRPass(rhi::IRHICommandList* cmd) {
    cmd->EndOffscreenPass();

    // 布局转换：COLOR_ATTACHMENT → 着色器只读（ToneMap 采样）
    cmd->PipelineBarrier(
        rhi::PipelineStage::ColorAttachmentOutput,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::RenderTarget,
        rhi::ResourceState::ShaderResource,
        m_HDRTarget.get());

    // 预置 ToneMap PSO，确保后续 BeginRenderPass 使用正确的 SwapChain RP（B8G8R8A8）
    cmd->SetPipeline(m_ToneMapPSO.get());
}

void ForwardPipeline::PrepareGI(rhi::IRHICommandList* cmd, he::World& world) {
    if (!m_GI || !m_GI->IsEnabled()) return;

    // 查找启用的 SkyboxComponent → 设置 Skybox Cubemap
    world.ForEach<he::SkyboxComponent>([&](he::Entity, he::SkyboxComponent& sc) {
        if (sc.enabled && sc.GetCubemap()) {
            auto* giIBL = dynamic_cast<GI_IBL*>(m_GI.get());
            if (giIBL) {
                giIBL->SetIBLSkybox(sc.GetCubemap(), sc.GetCubemapSampler());
                // 若 IBL 脏 → 生成辐照度/预滤波/BRDF LUT
                giIBL->Render(cmd);
                // 更新 PBR 描述符集绑定到新生成的 IBL 纹理
                UpdateIBLBindings(giIBL);
            }
        }
    });
}

void ForwardPipeline::UpdateIBLBindings(GI_IBL* gi) {
    rhi::IRHITexture* irr    = gi->GetIrradianceMap();
    rhi::IRHITexture* pref   = gi->GetPrefilterMap();
    rhi::IRHITexture* lut    = gi->GetBRDF_LUT();
    rhi::IRHISampler* sampler = gi->GetIBLSampler();

    // 更新共享描述符集（三帧全部更新，确保一致性）
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_Device->UpdateDescriptorSet(m_DescSets[i], 12,
            rhi::DescriptorType::CombinedImageSampler, irr, sampler);
        m_Device->UpdateDescriptorSet(m_DescSets[i], 13,
            rhi::DescriptorType::CombinedImageSampler, pref, sampler);
        m_Device->UpdateDescriptorSet(m_DescSets[i], 14,
            rhi::DescriptorType::CombinedImageSampler, lut, sampler);
    }
    // 更新 per-mesh 描述符集
    for (auto& set : m_AllPerMeshDescSets) {
        m_Device->UpdateDescriptorSet(set, 12,
            rhi::DescriptorType::CombinedImageSampler, irr, sampler);
        m_Device->UpdateDescriptorSet(set, 13,
            rhi::DescriptorType::CombinedImageSampler, pref, sampler);
        m_Device->UpdateDescriptorSet(set, 14,
            rhi::DescriptorType::CombinedImageSampler, lut, sampler);
    }
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

// ---- IRenderPipeline 包装方法 ----

void ForwardPipeline::Render(rhi::IRHICommandList* cmd, he::World& world,
                              he::SceneGraph& sg, const CameraData& camera)
{
    // GI 准备（检测 Skybox → 更新 IBL）
    PrepareGI(cmd, world);
    // HDR 离屏通道
    BeginHDRPass(cmd, m_HDRWidth, m_HDRHeight);
    BeginFrame(cmd, m_HDRWidth, m_HDRHeight);
    RenderScene(cmd, world, sg, camera);
    RenderSkybox(cmd, world, camera);
    EndHDRPass(cmd);
}

void ForwardPipeline::OnResize(u32 width, u32 height) {
    ResizeHDRTarget(width, height);
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
    framePC.iblIntensity  = m_GI ? m_GI->GetSettings().intensity : 1.0f;

    // 收集光源（阴影数据由 ShadowSystem 管理，此处仅收集光照）
    CollectLights(framePC, world, sceneGraph, camera);

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
                // Secondary CB 不继承 Primary 的动态状态，必须显式设置视口/裁剪区
                secCmd->SetViewport({ 0, static_cast<float>(m_HDRHeight),
                    static_cast<float>(m_HDRWidth), -static_cast<float>(m_HDRHeight),
                    0.0f, 1.0f });
                secCmd->SetScissor({ 0, 0, m_HDRWidth, m_HDRHeight });

                for (u32 i = start; i < end; ++i) {
                    auto& de = visible[i];
                    PushConstantData pc = framePC;
                    pc.objectIndex = i;  // objectIndex

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
        HE_CORE_INFO("ForwardPipeline::RenderScene: {} draws, {} lights (Phase 5-3 frustum cull)",
            drawCount, framePC.lightCount);
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
