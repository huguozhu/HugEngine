#include "Pipeline/ForwardPipeline.h"
#include "GI/GI_IBL.h"
#include "GI/GI_RSM.h"
#include "Shadow/ShadowSystem.h"
#include "Shadow/ShadowNone.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "SceneRenderer.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Threading/JobSystem.h"
#include "PBR.vert.spv.h"
#include "PBR.frag.spv.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>  // orthoRH_ZO (Vulkan Z [0,1])
#include <unordered_set>

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

    // --- 主管线 DescriptorSetLayout（set=0: per-frame + bindless 纹理/采样器）---
    // set=0: per-frame 动态数据 + 全局 bindless 纹理数组
    rhi::DescriptorSetLayoutDesc perFrameLayoutDesc;
    perFrameLayoutDesc.bindings = {
        { 1,  rhi::DescriptorType::StorageBuffer,        1, 16 },  // GPULight[]
        { 2,  rhi::DescriptorType::StorageBuffer,        1, 17 },  // GPUObjectData[]
        { 3,  rhi::DescriptorType::StorageBuffer,        1, 16 },  // GPUShadowData[]
        { 4,  rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // CSM cascade 0
        { 5,  rhi::DescriptorType::SampledImage,  4096, 16, true },  // u_Textures[] bindless
        { 6,  rhi::DescriptorType::Sampler,       4096, 16, true },  // u_Samplers[] bindless
        { 9,  rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Point Shadow Cubemap
        { 10, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // CSM cascade 1
        { 11, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // CSM cascade 2
        { 12, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // IBL Irradiance Cubemap
        { 13, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // IBL Prefilter Cubemap
        { 14, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // IBL BRDF LUT
        { 15, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // RSM Position
        { 16, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // RSM Normal+Flux
    };
    m_PerFrameLayout = device->CreateDescriptorSetLayout(perFrameLayoutDesc);

    // 用 per-frame 布局创建 Shadow PSO（阴影通道不需要 per-mesh 纹理绑定）
    m_ShadowSystem->CreateShadowPSO(m_PerFrameLayout);

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

        // 阴影专用 Object Buffer（独立于场景，避免 CPU 录制覆盖）
        rhi::BufferDesc shadowObjDesc;
        shadowObjDesc.size  = sizeof(GPUObjectData) * MAX_OBJECTS;
        shadowObjDesc.usage = rhi::BufferUsage::Storage;
        m_ShadowObjBuffers[i] = device->CreateBuffer(shadowObjDesc);
    }

    // --- 创建 bindless 占位纹理 + 采样器 ---
    {
        u8 white4[4] = { 255, 255, 255, 255 };
        rhi::TextureDesc texDesc;
        texDesc.format      = rhi::Format::RGBA8_UNORM;
        texDesc.width       = 1;
        texDesc.height      = 1;
        texDesc.usage       = rhi::TextureUsage::ShaderResource;
        texDesc.initialData = white4;
        m_BindlessPlaceholder = device->CreateTexture(texDesc);

        rhi::SamplerDesc sampDesc;
        sampDesc.minFilter = rhi::FilterMode::Linear;
        sampDesc.magFilter = rhi::FilterMode::Linear;
        sampDesc.addressU  = rhi::AddressMode::Repeat;
        sampDesc.addressV  = rhi::AddressMode::Repeat;
        m_BindlessSampler = device->CreateSampler(sampDesc);
    }

    // --- 分配三缓冲共享描述符集（set=0: per-frame + bindless）---
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::DescriptorSetHandle set = device->AllocateDescriptorSet(m_PerFrameLayout);
        device->UpdateDescriptorSet(set, 1, rhi::DescriptorType::StorageBuffer,
                                    m_LightBuffers[i].get());
        device->UpdateDescriptorSet(set, 2, rhi::DescriptorType::StorageBuffer,
                                    m_ObjectBuffers[i].get());
        device->UpdateDescriptorSet(set, 3, rhi::DescriptorType::StorageBuffer,
                                    m_ShadowBuffers[i].get());
        // CSM: 绑定 3 级联阴影贴图（来自 ShadowSystem）
        for (u32 c = 0; c < CASCADE_COUNT; ++c) {
            u32 binding = (c == 0) ? 4u : (c == 1 ? 10u : 11u);
            device->UpdateDescriptorSet(set, binding, rhi::DescriptorType::CombinedImageSampler,
                m_ShadowSystem->GetShadowMap(c), m_ShadowSystem->GetShadowSampler());
        }
        // 绑定 5-6: bindless 占位符（BindlessTextureManager 在渲染时更新）
        {
            rhi::IRHITexture* texPtrs[] = { m_BindlessPlaceholder.get() };
            rhi::IRHISampler* sampPtrs[] = { m_BindlessSampler.get() };
            device->UpdateDescriptorSet(set, 5, rhi::DescriptorType::SampledImage,
                texPtrs, nullptr, 1);
            device->UpdateDescriptorSet(set, 6, rhi::DescriptorType::Sampler,
                nullptr, sampPtrs, 1);
        }
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
            m_BindlessPlaceholder.get(), m_BindlessSampler.get());
        // 绑定 15-16: RSM 纹理占位（GI_RSM 渲染后替换）
        device->UpdateDescriptorSet(set, 15,
            rhi::DescriptorType::CombinedImageSampler,
            m_BindlessPlaceholder.get(), m_BindlessSampler.get());
        device->UpdateDescriptorSet(set, 16,
            rhi::DescriptorType::CombinedImageSampler,
            m_BindlessPlaceholder.get(), m_BindlessSampler.get());
        m_DescSets[i] = set;
    }
    // 初始化时使用第一个槽位
    m_CurrentFrameSlot = 0;

    // 注册全部三缓冲描述符集到 BindlessTextureManager
    // FlushPending() 会自动向全部已注册 set 推送纹理数组，无需调用方手动遍历
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        he::asset::BindlessTextureManager::Instance().RegisterDescriptorSet(
            device, m_DescSets[i], 5, 6);
    }

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
    psoDesc.descriptorSetLayouts = { m_PerFrameLayout };  // 仅 set=0，bindless 统一管理
    psoDesc.debugName            = "ForwardPBR";

    m_PBR_PSO = device->CreatePipelineState(psoDesc);
    HE_ASSERT(m_PBR_PSO, "ForwardPipeline: failed to create PBR PSO");

    // --- ToneMap 后处理子系统 ---
    m_ToneMap = std::make_unique<ToneMapPass>();
    m_ToneMap->Initialize(device, m_HDRWidth, m_HDRHeight);
    HE_CORE_INFO("ForwardPipeline: ToneMapPass initialized");

    // --- 天空盒子系统 ---
    m_Skybox = std::make_unique<SkyboxPass>();
    m_Skybox->Initialize(device, m_HDRWidth, m_HDRHeight);
    HE_CORE_INFO("ForwardPipeline: SkyboxPass initialized");

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

    // --- GI 子系统（IBL + RSM）---
    {
        auto gi = std::make_unique<GI_IBL>();
        gi->Initialize(device, 0, 0);  // IBL 分辨率独立于视口
        m_GI = std::move(gi);
        HE_CORE_INFO("ForwardPipeline: GI_IBL initialized");
    }
    {
        m_RSM = std::make_unique<GI_RSM>();
        m_RSM->Initialize(device, 0, 0);
        HE_CORE_INFO("ForwardPipeline: GI_RSM initialized");
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

    // --- GPU Culling ---
    m_GPUCulling.Initialize(device);

    // --- SceneRenderer ---
    m_SceneRenderer = std::make_unique<SceneRenderer>();

    HE_CORE_INFO("ForwardPipeline initialized (with HDR + Tone Mapping + Skybox + ShadowSystem)");
    return true;
}

void ForwardPipeline::Shutdown() {
    if (m_Device) {
        m_Device->DestroyDescriptorSetLayout(m_PerFrameLayout);
    }
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_LightBuffers[i].reset();
        m_ObjectBuffers[i].reset();
        m_ShadowBuffers[i].reset();
    }
    m_PBR_PSO.reset();
    if (m_ToneMap) { m_ToneMap->Shutdown(); m_ToneMap.reset(); }
    if (m_Skybox)  { m_Skybox->Shutdown();  m_Skybox.reset(); }
    m_HDRTarget.reset();
    m_HDRDepth.reset();
    m_HDRSampler.reset();
    m_Device = nullptr;
    m_SecRecordLists.clear();  // Phase 5-4 sec CB 录制池

    // 阴影子系统
    if (m_ShadowSystem) {
        m_ShadowSystem->Shutdown();
        m_ShadowSystem.reset();
    }

    m_Device = nullptr;
    HE_CORE_INFO("ForwardPipeline shut down");
}

void ForwardPipeline::NextFrame() {
    // 推进三缓冲槽位（帧首调用，确保 Shadow 和 Scene 使用同一帧的缓冲区）
    m_CurrentFrameSlot = (m_CurrentFrameSlot + 1) % MAX_FRAMES_IN_FLIGHT;

    // 同步阴影子系统帧槽位
    m_ShadowSystem->NextFrame();
    // per-mesh 描述符集 (set=1) 是静态纹理绑定，不需要每帧更新
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

    // 预设 ToneMap PSO 为下一个 RenderPass 的初始管线（匹配 SwapChain BGRA8_UNORM 格式）
    if (m_ToneMap) m_ToneMap->PreBind(cmd);
}

void ForwardPipeline::PrepareGI(rhi::IRHICommandList* cmd, he::World& world, he::SceneGraph& sg) {
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

    // RSM 渲染（使用独立深度缓冲，不再复用 CSM ShadowMap 避免布局冲突）
    if (m_RSM && m_ShadowSystem && m_ShadowSystem->HasActiveShadows()) {
        float4x4 lightVP = m_ShadowSystem->GetLightViewProj(0);
        if (glm::determinant(lightVP) != 0.0f) {  // 有效光源 VP
            m_RSM->SetLightViewProj(lightVP, m_RSM->GetRSMPositionMap()->GetWidth(),
                                    m_ObjectBuffers[m_CurrentFrameSlot].get(),
                                    m_ShadowSystem->GetShadowSampler(),
                                    m_DescSets[m_CurrentFrameSlot]);
            // 从光源 POV 渲染几何体 → RSM 纹理（使用 RSM 自有的独立深度缓冲）
            m_RSM->RenderRSMPass(cmd, world, sg);
            UpdateRSMBindings();
        }
    }
}

void ForwardPipeline::UpdateRSMBindings() {
    if (!m_RSM) return;
    rhi::IRHITexture* posMap  = m_RSM->GetRSMPositionMap();
    rhi::IRHITexture* fluxMap = m_RSM->GetRSMFluxMap();
    rhi::IRHISampler* sampler = m_RSM->GetRSMSampler();
    // RSM 绑定在 set=0（per-frame），只需更新共享描述符集
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_Device->UpdateDescriptorSet(m_DescSets[i], 15,
            rhi::DescriptorType::CombinedImageSampler, posMap, sampler);
        m_Device->UpdateDescriptorSet(m_DescSets[i], 16,
            rhi::DescriptorType::CombinedImageSampler, fluxMap, sampler);
    }
}

void ForwardPipeline::UpdateIBLBindings(GI_IBL* gi) {
    rhi::IRHITexture* irr    = gi->GetIrradianceMap();
    rhi::IRHITexture* pref   = gi->GetPrefilterMap();
    rhi::IRHITexture* lut    = gi->GetBRDF_LUT();
    rhi::IRHISampler* sampler = gi->GetIBLSampler();

    // IBL 绑定在 set=0（per-frame），只需更新共享描述符集
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_Device->UpdateDescriptorSet(m_DescSets[i], 12,
            rhi::DescriptorType::CombinedImageSampler, irr, sampler);
        m_Device->UpdateDescriptorSet(m_DescSets[i], 13,
            rhi::DescriptorType::CombinedImageSampler, pref, sampler);
        m_Device->UpdateDescriptorSet(m_DescSets[i], 14,
            rhi::DescriptorType::CombinedImageSampler, lut, sampler);
    }
}

void ForwardPipeline::RenderSkybox(rhi::IRHICommandList* cmd, he::World& world,
                                    const CameraData& camera) {
    if (!m_Skybox) return;
    SubsystemContext ctx; ctx.world = &world; ctx.camera = &camera;
    m_Skybox->Update(ctx);
    m_Skybox->Render(cmd);
}

void ForwardPipeline::RenderToneMapPass(rhi::IRHICommandList* cmd) {
    if (m_ToneMap) {
        m_ToneMap->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
        m_ToneMap->Render(cmd);
    }
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

    // ToneMapPass 输入会自动通过每帧 SetInput 更新，无需手动更新描述符集
}

// ---- IRenderPipeline 包装方法 ----

void ForwardPipeline::Render(rhi::IRHICommandList* cmd, he::World& world,
                              he::SceneGraph& sg, const CameraData& camera)
{
    if (m_UseRenderGraph) {
        RenderGraph rg;
        BuildFrameGraph(rg, world, sg, camera);
        rg.Compile();
        rg.Execute(cmd, m_Device);
        return;
    }
    // 非 RG 路径：手动渲染阴影
    if (m_ShadowSystem && m_ShadowSystem->HasActiveShadows()) {
        u32 slot = m_CurrentFrameSlot;
        // 切换 binding 2 到阴影 Object Buffer（仅更新 set=0 per-frame 集）
        m_Device->UpdateDescriptorSet(m_DescSets[slot], 2,
            rhi::DescriptorType::StorageBuffer, m_ShadowObjBuffers[slot].get());

        m_ShadowSystem->Render(cmd);

        // 恢复 binding 2
        m_Device->UpdateDescriptorSet(m_DescSets[slot], 2,
            rhi::DescriptorType::StorageBuffer, m_ObjectBuffers[slot].get());
    }
    PrepareGI(cmd, world, sg);
    BeginHDRPass(cmd, m_HDRWidth, m_HDRHeight);
    BeginFrame(cmd, m_HDRWidth, m_HDRHeight);
    RenderScene(cmd, world, sg, camera);
    RenderSkybox(cmd, world, camera);
    EndHDRPass(cmd);
}

void ForwardPipeline::BuildFrameGraph(RenderGraph& rg, he::World& world,
                                       he::SceneGraph& sg, const CameraData& camera)
{
    if (m_SwapChain) rg.SetSwapChain(m_SwapChain);
    u32 sw = m_SwapChain->GetWidth(), sh = m_SwapChain->GetHeight();
    if (m_ToneMap) m_ToneMap->OnResize(sw, sh);
    if (m_Skybox)  m_Skybox->OnResize(sw, sh);
    u32 w = m_HDRWidth, h = m_HDRHeight;
    auto hdrColor = rg.ImportTexture("HDR_Color", m_HDRTarget.get());
    auto hdrDepth = rg.ImportTexture("HDR_Depth", m_HDRDepth.get());
    auto backBuf  = rg.ImportBackBuffer();

    // --- Pass 0: Shadow — CSM 级联 + Point Cubemap 阴影贴图渲染 ---
    // 声明写入所有阴影贴图 + hdrDepth（WAW 确保 Shadow 先于 FullScene）
    if (m_ShadowSystem && m_ShadowSystem->HasActiveShadows()) {
        ResourceHandle csmMaps[CASCADE_COUNT];
        for (u32 c = 0; c < CASCADE_COUNT; ++c) {
            auto* tex = m_ShadowSystem->GetShadowMap(c);
            if (tex) {
                char name[32];
                snprintf(name, sizeof(name), "CSM_ShadowMap_C%u", c);
                csmMaps[c] = rg.ImportTexture(name, tex);
            } else {
                csmMaps[c] = kInvalidHandle;
            }
        }

        std::vector<PassResource> shadowWrites;
        for (u32 c = 0; c < CASCADE_COUNT; ++c)
            if (csmMaps[c] != kInvalidHandle)
                shadowWrites.push_back(RG_WRITE(csmMaps[c]));

        // Point Shadow Cubemap
        if (auto* ptTex = m_ShadowSystem->GetPointShadowMap()) {
            auto ptHandle = rg.ImportTexture("PointShadow_CubeMap", ptTex);
            shadowWrites.push_back(RG_WRITE(ptHandle));
        }

        // WAW 依赖：声明写入 hdrDepth 确保 Shadow → FullScene 的执行顺序
        shadowWrites.push_back(RG_WRITE(hdrDepth));

        rg.AddPass("Shadow", {}, std::move(shadowWrites),
            [this](rhi::IRHICommandList* c) {
                // 切换描述符集 binding 2 到阴影专用 Object Buffer
                // （仅更新 set=0 per-frame 集，per-mesh set=1 不包含 buffer 绑定）
                u32 slot = m_CurrentFrameSlot;
                m_Device->UpdateDescriptorSet(m_DescSets[slot], 2,
                    rhi::DescriptorType::StorageBuffer,
                    m_ShadowObjBuffers[slot].get());

                m_ShadowSystem->Render(c);

                // 恢复 binding 2 到场景 Object Buffer（FullScene 使用）
                m_Device->UpdateDescriptorSet(m_DescSets[slot], 2,
                    rhi::DescriptorType::StorageBuffer,
                    m_ObjectBuffers[slot].get());
            });
    }

    // --- Pass 1: IBL 生成（仅在天空盒脏时执行）---
    auto* giIBL = dynamic_cast<GI_IBL*>(m_GI.get());
    bool iblNeedsUpdate = false;
    if (giIBL && giIBL->IsDirty()) {
        auto iblIrr  = rg.ImportTexture("IBL_Irradiance", giIBL->GetIrradianceMap());
        auto iblPref = rg.ImportTexture("IBL_Prefilter",  giIBL->GetPrefilterMap());
        auto iblLUT  = rg.ImportTexture("IBL_BRDF_LUT",   giIBL->GetBRDF_LUT());
        rg.AddPass("IBL_Generate", {},
            {{iblIrr, ResourceAccess::Write}, {iblPref, ResourceAccess::Write}, {iblLUT, ResourceAccess::Write}},
            [giIBL](rhi::IRHICommandList* c) { giIBL->Render(c); });
        iblNeedsUpdate = true;
    }

    // --- Pass 2: RSM 生成（Reflective Shadow Maps）---
    if (m_RSM && m_ShadowSystem && m_ShadowSystem->HasActiveShadows()) {
        float4x4 lightVP = m_ShadowSystem->GetLightViewProj(0);
        if (glm::determinant(lightVP) != 0.0f) {
            auto rsmPos  = rg.ImportTexture("RSM_Position",  m_RSM->GetRSMPositionMap());
            auto rsmFlux = rg.ImportTexture("RSM_Flux",      m_RSM->GetRSMFluxMap());
            rg.AddPass("RSM_Generate", {},
                {{rsmPos, ResourceAccess::Write}, {rsmFlux, ResourceAccess::Write}},
                [this, &world, &sg](rhi::IRHICommandList* c) {
                    m_RSM->SetLightViewProj(m_ShadowSystem->GetLightViewProj(0),
                        m_RSM->GetRSMPositionMap()->GetWidth(),
                        m_ObjectBuffers[m_CurrentFrameSlot].get(),
                        m_ShadowSystem->GetShadowSampler(),
                        m_DescSets[m_CurrentFrameSlot]);
                    m_RSM->RenderRSMPass(c, world, sg);
                });
        }
    }

    // --- Pass 3: Scene — HDR 几何 + 天空盒渲染 ---
    rg.AddPass("Scene",
        {{hdrDepth, ResourceAccess::Read}},  // 读深度确保在 Shadow 之后
        {{hdrColor, ResourceAccess::Write}, {hdrDepth, ResourceAccess::Write}},
        [this, w, h, &world, &sg, &camera, iblNeedsUpdate](rhi::IRHICommandList* c) {
            // IBL bindings 更新（IBL pass 之后纹理已变化）
            if (iblNeedsUpdate && m_GI) {
                auto* gi = dynamic_cast<GI_IBL*>(m_GI.get());
                if (gi) UpdateIBLBindings(gi);
            }
            BeginHDRPass(c, w, h);
            BeginFrame(c, w, h);
            RenderScene(c, world, sg, camera);
            RenderSkybox(c, world, camera);
            EndHDRPass(c);
        });

    // --- Pass 4: ToneMap — HDR → LDR 色调映射 ---
    rg.AddPass("ToneMap", {{hdrColor, ResourceAccess::Read}}, {{backBuf, ResourceAccess::Write}},
        [this](rhi::IRHICommandList* c) {
            m_ToneMap->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
            m_ToneMap->PreBind(c);
            c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
            m_ToneMap->Render(c);
            c->EndRenderPass();
        });
}
void ForwardPipeline::OnResize(u32 width, u32 height) {
    ResizeHDRTarget(width, height);
    if (m_ToneMap) m_ToneMap->OnResize(width, height);
    if (m_Skybox)  m_Skybox->OnResize(width, height);
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
    // GPU 视锥剔除（Compute Shader）— 读回上帧结果 → 调度下帧
    // ============================================================
    // 1) 读回上一帧 GPU culling 结果（已 submit 执行完毕）
    bool useGPUVisible = false;
    if (m_GPUCulling.enabled) {
        m_GPUCulling.Readback(m_Device, m_GPUVisibleIndices);
        useGPUVisible = !m_GPUVisibleIndices.empty();
    }

    // 2) SceneRenderer 准备所有 draw items
    auto allDrawItems = m_SceneRenderer->Prepare(world, sceneGraph, camera,
                                                  m_ObjectBuffers[m_CurrentFrameSlot].get());

    // 3) 上传当前帧 bounds + 调度 GPU culling（结果下帧读回）
    if (m_GPUCulling.enabled) {
        std::vector<CullObjectBounds> allBounds;
        world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent& mc) {
            if (mc.GetIndexCount() == 0) return;
            CullObjectBounds cb;
            const auto& bb = mc.GetBounds();
            cb.minPoint    = float4(bb.min, 0);
            cb.maxPoint    = float4(bb.max, 0);
            cb.objectIndex = static_cast<u32>(allBounds.size());
            allBounds.push_back(cb);
        });
        if (!allBounds.empty()) {
            m_GPUCulling.UploadBounds(m_Device, allBounds);
            m_GPUCulling.Dispatch(cmd, viewProj, (u32)allBounds.size());
            // Dispatch 切换到了 Compute PSO，恢复 Graphics PSO
            cmd->SetPipeline(m_PBR_PSO.get());
        }
    }

    // GPU 剔除后过滤：构建可见 draw 列表
    std::vector<DrawItem> filteredItems;
    if (useGPUVisible) {
        // 构建 GPU 可见集合（objectIndex → true）
        std::unordered_set<u32> visibleSet(m_GPUVisibleIndices.begin(), m_GPUVisibleIndices.end());
        for (auto& di : allDrawItems) {
            if (visibleSet.count(di.objectIndex))
                filteredItems.push_back(di);
        }
    } else {
        filteredItems = std::move(allDrawItems);
    }
    u32 totalDraws = (u32)filteredItems.size();

    // 推送 bindless 纹理到全部已注册描述符集（FlushPending 自动遍历全部 set）
    he::asset::BindlessTextureManager::Instance().FlushPending();

    // ============================================================
    // 录制绘制命令（ForwardPipeline 特有：PBR PSO + bindless 描述符集）
    // ============================================================
    if (m_MultiThreadRecord && !m_SecRecordLists.empty() && totalDraws > 0) {
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
                secCmd->SetViewport({0, (float)m_HDRHeight, (float)m_HDRWidth, -(float)m_HDRHeight, 0, 1});
                secCmd->SetScissor({0, 0, m_HDRWidth, m_HDRHeight});

                for (u32 i = start; i < end; ++i) {
                    auto& di = filteredItems[i];
                    PushConstantData pc = framePC;
                    pc.objectIndex = di.objectIndex;
                    secCmd->BindDescriptorSet(0, m_DescSets[m_CurrentFrameSlot]);  // set=0: per-frame + bindless
                    // 不再需要 bind set=1 — 纹理采样通过 bindless u_Textures[] 访问
                    secCmd->SetPushConstants(0, sizeof(PushConstantData), &pc);
                    secCmd->SetVertexBuffer(di.mesh->GetVertexBuffer().get(), 0);
                    secCmd->SetIndexBuffer(di.mesh->GetIndexBuffer().get());
                    secCmd->DrawIndexed(di.mesh->GetIndexCount());
                }
                secCmd->End();
            });
        }
        JobSystem::Instance().ParallelInvoke(tasks);
        for (u32 t = 0; t < numThreads; ++t) {
            if (t * chunkSize >= totalDraws) continue;
            cmd->ExecuteSecondary(m_SecRecordLists[t].get());
        }
        drawCount = totalDraws;
    } else {
        for (auto& di : filteredItems) {
            PushConstantData pc = framePC;
            pc.objectIndex = di.objectIndex;
            cmd->BindDescriptorSet(0, m_DescSets[m_CurrentFrameSlot]);  // set=0: per-frame + bindless
            // 不再需要 bind set=1
            cmd->SetPushConstants(0, sizeof(PushConstantData), &pc);
            cmd->SetVertexBuffer(di.mesh->GetVertexBuffer().get(), 0);
            cmd->SetIndexBuffer(di.mesh->GetIndexBuffer().get());
            cmd->DrawIndexed(di.mesh->GetIndexCount());
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
