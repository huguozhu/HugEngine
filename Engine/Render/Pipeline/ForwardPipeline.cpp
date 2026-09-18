#include "Pipeline/ForwardPipeline.h"
#include "Pipeline/PhysicalLight.h"  // render::KelvinToRGB

// 物理光照单位全局开关定义（声明见 PhysicalLight.h；EngineConfig::usePhysicalLights 启动时桥接）
namespace he::render {
he::CVar<bool> cvLightPhysicalUnits("r.Light.PhysicalUnits", false,
    "1=启用物理光照单位（光源 illuminance/luminousIntensity 生效）；0=传统 intensity 模式");
} // namespace he::render

#include "GI/GI_IBL.h"
#include "GI/GI_RSM.h"
#include "GI/RSMFrustum.h"   // RSM 光锥尺度 → VPL 采样缩放（任务 30 / §9.2-AA）
#include "GI/GITypes.h"   // GIRegistry（可用性与降级）
#include "ShaderTypes.slang"   // GIBlendParams（C++ 侧镜像，任务 26）
#include "Shadow/ShadowSystem.h"
#include "Shadow/ShadowNone.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "SceneRenderer.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/InstancedMeshComponent.h"
#include "Scene/SkeletalMeshComponent.h"
#include "Scene/SplineMeshComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Threading/JobSystem.h"
#include "PBR.vert.spv.h"
#include "PBR.frag.spv.h"
#include "GBuffer.mesh.spv.h"
#include "AntiAliasing/AA_None.h"
#include "AntiAliasing/AA_FXAA.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>  // orthoRH_ZO (Vulkan Z [0,1])
#include <unordered_set>
#include <unordered_map>
#include <cstring>

#include <chrono>
#include <mutex>
#include <cstdio>

namespace he::render {

ForwardPipeline::ForwardPipeline() {
}

ForwardPipeline::~ForwardPipeline() {
    Shutdown();
}

bool ForwardPipeline::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    HE_ASSERT(m_Device, "ForwardPipeline: device is null");

    // 采用调用方给出的初始尺寸（0 = 沿用默认），避免"先按默认尺寸建 HDR/后处理资源、
    // 随即被 OnResize 全量销毁重建"造成的启动期 churn（详见 IRenderPipeline::Initialize）
    if (width  > 0) m_HDRWidth  = width;
    if (height > 0) m_HDRHeight = height;

    // GI 通道配置：Forward 的能力位里**没有任何 GI 源**（只有光栅阴影），因此降级后三个
    // GI 通道都是空的。这不是遗漏，而是如实反映现状：本管线的 IBL 与 RSM 由**管线级开关**
    // （`iblIntensity` / `rsmIndirect`）加内部硬编码路径驱动，既不读层栈，PBR 着色器里也没有
    // `GIBlendParams` 归一化合成（§9.2-H）。此前声明 IBL/RSM 可用，实际只是把源放进一个
    // 没人消费的层栈里。要让它真正走层栈归一化是独立的改造项（文档任务 26）。
    m_GIConfig = GIRegistry::Degrade(GIConfigFromPreset(GIQualityPreset::Medium), PipelineCaps::Forward,
                                     device->GetCaps().supportsRayTracing);

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
        {kGPUBinding_GBufferB, rhi::DescriptorType::StorageBuffer,        1, 16 },  // GPULight[]
        {kGPUBinding_GBufferC, rhi::DescriptorType::StorageBuffer,        1, 17 },  // GPUObjectData[]
        {kGPUBinding_Depth, rhi::DescriptorType::StorageBuffer,        1, 16 },  // GPUShadowData[]
        {kGPUBinding_LightGrid, rhi::DescriptorType::StorageBuffer,        1, 16 },  // LightGrid（Forward+）
        {kGPUBinding_LightIndexList, rhi::DescriptorType::StorageBuffer,        1, 16 },  // LightIndexList（Forward+）
        {kGPUBinding_ShadowMap0, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // CSM cascade 0
        { 5,  rhi::DescriptorType::SampledImage,  4096, rhi::kStageMaskFragment, true },  // u_Textures[] bindless
        {kGPUBinding_DDGIGridParams, rhi::DescriptorType::Sampler,       4096, rhi::kStageMaskFragment, true },  // u_Samplers[] bindless
        {kGPUBinding_PointShadow, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Point Shadow Cubemap
        {kGPUBinding_ShadowMap1, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // CSM cascade 1
        {kGPUBinding_ShadowMap2, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // CSM cascade 2
        {kGPUBinding_IrradianceMap, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // IBL Irradiance Cubemap
        {kGPUBinding_PrefilterMap, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // IBL Prefilter Cubemap
        {kGPUBinding_BRDF_LUT, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // IBL BRDF LUT
        {kGPUBinding_RSMPosition, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // RSM Position
        {kGPUBinding_RSMFlux, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // RSM Normal（任务 30 起只存法线）
        {kGPUBinding_RSMRadiance, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // RSM VPL Radiance（任务 30）
        {kGPUBinding_SpotShadow, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Spot Shadow Map（独立 binding，避免与点光 9 冲突）
        {kGPUBinding_RectShadow, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Rect Shadow Map（矩形面光）
        { 30, rhi::DescriptorType::StorageBuffer,     4096, rhi::kStageMaskVertex | rhi::kStageMaskFragment, true },  // u_SSBO[] bindless
        // GI 分层合成参数 UBO（任务 26）：与 Deferred 侧同一绑定号与同一结构 —— Forward 的
        // IBL/RSM 从"管线级开关 + 硬编码求和"改为层栈驱动的归一化合成
        {kGPUBinding_GIBlendParams, rhi::DescriptorType::UniformBuffer, 1, rhi::kStageMaskFragment},
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

    // --- Forward+ LightGrid / LightIndexList 初始占位缓冲区 ---
    {
        rhi::BufferDesc gridDesc;
        gridDesc.size  = sizeof(ClusteredShading::LightGridCell) * 64;
        gridDesc.usage = rhi::BufferUsage::Storage;
        gridDesc.cpuAccess = true;
        m_LightGridBuffer = device->CreateBuffer(gridDesc);

        rhi::BufferDesc listDesc;
        listDesc.size  = sizeof(u32) * 64;
        listDesc.usage = rhi::BufferUsage::Storage;
        listDesc.cpuAccess = true;
        m_LightIndexListBuffer = device->CreateBuffer(listDesc);
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

        // 注册默认占位纹理到 bindless 堆（作为 null/无纹理 mesh 的回退）
        auto* heap = device->GetBindlessHeap();
        heap->SetDefaultTexture(m_BindlessPlaceholder.get(), m_BindlessSampler.get());
        // 预分配 materialID=0 的 4 个纹理槽位（BaseColor/Normal/MetallicRoughness/Occlusion），
        // 确保 materialID=0 的 mesh（如 02.Cube 中无纹理的立方体/球体）始终有有效纹理
        heap->RegisterTexture(nullptr, nullptr);
        heap->RegisterTexture(nullptr, nullptr);
        heap->RegisterTexture(nullptr, nullptr);
        heap->RegisterTexture(nullptr, nullptr);
    }

    // --- 分配三缓冲共享描述符集（set=0: per-frame + bindless）---
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::DescriptorSetHandle set = device->AllocateDescriptorSet(m_PerFrameLayout);
        device->UpdateDescriptorSet(set, kGPUBinding_GBufferB, rhi::DescriptorType::StorageBuffer,
                                    m_LightBuffers[i].get());
        device->UpdateDescriptorSet(set, rhi::kBindingObjectData, rhi::DescriptorType::StorageBuffer,
                                    m_ObjectBuffers[i].get());
        device->UpdateDescriptorSet(set, kGPUBinding_Depth, rhi::DescriptorType::StorageBuffer,
                                    m_ShadowBuffers[i].get());
        // Forward+: LightGrid / LightIndexList 初始占位
        device->UpdateDescriptorSet(set, rhi::kBindingLightGrid, rhi::DescriptorType::StorageBuffer,
                                    m_LightGridBuffer.get());
        device->UpdateDescriptorSet(set, rhi::kBindingLightIndexList, rhi::DescriptorType::StorageBuffer,
                                    m_LightIndexListBuffer.get());
        // CSM: 绑定 3 级联阴影贴图（来自 ShadowSystem）
        for (u32 c = 0; c < CASCADE_COUNT; ++c) {
            u32 binding = (c == 0) ? 4u : (c == 1 ? 10u : 11u);
            device->UpdateDescriptorSet(set, binding, rhi::DescriptorType::CombinedImageSampler,
                m_ShadowSystem->GetShadowMap(c), m_ShadowSystem->GetShadowSampler());
        }
        // 绑定 5-6: bindless 占位符（bindless 堆在渲染时更新）
        {
            rhi::IRHITexture* texPtrs[] = { m_BindlessPlaceholder.get() };
            rhi::IRHISampler* sampPtrs[] = { m_BindlessSampler.get() };
            device->UpdateDescriptorSet(set, rhi::kBindingBindlessTextures, rhi::DescriptorType::SampledImage,
                texPtrs, nullptr, 1);
            device->UpdateDescriptorSet(set, rhi::kBindingBindlessSamplers, rhi::DescriptorType::Sampler,
                nullptr, sampPtrs, 1);
        }
        // 绑定 9: 点光源阴影 Cubemap（来自 ShadowSystem）
        device->UpdateDescriptorSet(set, kGPUBinding_PointShadow, rhi::DescriptorType::CombinedImageSampler,
            m_ShadowSystem->GetPointShadowMap(), m_ShadowSystem->GetPointShadowSampler());
        // 绑定 12-14: IBL 纹理占位（GI_IBL 生成后通过 UpdateIBLBindings 替换）
        device->UpdateDescriptorSet(set, kGPUBinding_IrradianceMap, rhi::DescriptorType::CombinedImageSampler,
            m_ShadowSystem->GetPointShadowMap(), m_ShadowSystem->GetPointShadowSampler());
        device->UpdateDescriptorSet(set, kGPUBinding_PrefilterMap, rhi::DescriptorType::CombinedImageSampler,
            m_ShadowSystem->GetPointShadowMap(), m_ShadowSystem->GetPointShadowSampler());
        device->UpdateDescriptorSet(set, kGPUBinding_BRDF_LUT, rhi::DescriptorType::CombinedImageSampler,
            m_BindlessPlaceholder.get(), m_BindlessSampler.get());
        // 绑定 15-17: RSM 纹理占位（GI_RSM 渲染后替换）
        //   15=位置，16=编码法线，17=VPL 出射辐射度（任务 30 起一个附件一个量）
        device->UpdateDescriptorSet(set, kGPUBinding_RSMPosition, rhi::DescriptorType::CombinedImageSampler,
            m_BindlessPlaceholder.get(), m_BindlessSampler.get());
        device->UpdateDescriptorSet(set, kGPUBinding_RSMFlux, rhi::DescriptorType::CombinedImageSampler,
            m_BindlessPlaceholder.get(), m_BindlessSampler.get());
        device->UpdateDescriptorSet(set, kGPUBinding_RSMRadiance, rhi::DescriptorType::CombinedImageSampler,
            m_BindlessPlaceholder.get(), m_BindlessSampler.get());
        // 绑定 24: 聚光灯 2D 阴影贴图（来自 ShadowSystem，原 9 与点光冲突）
        device->UpdateDescriptorSet(set, kGPUBinding_SpotShadow, rhi::DescriptorType::CombinedImageSampler,
            m_ShadowSystem->GetSpotShadowMap(), m_ShadowSystem->GetSpotShadowSampler());
        // 绑定 25: 矩形面光 2D 阴影贴图（来自 ShadowSystem）
        device->UpdateDescriptorSet(set, kGPUBinding_RectShadow, rhi::DescriptorType::CombinedImageSampler,
            m_ShadowSystem->GetRectShadowMap(), m_ShadowSystem->GetRectShadowSampler());
        m_DescSets[i] = set;
    }
    // --- GI 分层合成参数 UBO（每飞行帧一份，任务 26）---
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::BufferDesc giDesc;
        giDesc.size      = sizeof(GIBlendParams);
        giDesc.usage     = rhi::BufferUsage::Uniform;
        giDesc.cpuAccess = true;
        m_GIBuffers[i] = device->CreateBuffer(giDesc);
        if (m_GIBuffers[i]) {
            device->UpdateDescriptorSet(m_DescSets[i], kGPUBinding_GIBlendParams,
                rhi::DescriptorType::UniformBuffer, m_GIBuffers[i].get());
        }
    }
    // 初始化时使用第一个槽位
    m_CurrentFrameSlot = 0;

    // 注册全部三缓冲描述符集到 bindless 堆
    // Flush() 会自动向全部已注册 set 推送纹理数组，无需调用方手动遍历
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        device->GetBindlessHeap()->RegisterDescriptorSet(
            m_DescSets[i], rhi::kBindingBindlessTextures,
            rhi::kBindingBindlessSamplers, rhi::kBindingBindlessSSBO);
    }

    // --- 主管线 PSO ---
    rhi::PushConstantRange pcRange;
    pcRange.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment;     // Vertex | Fragment
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

    // --- 蒙皮网格 PSO（C1b）：同着色器，扩展顶点布局（+JOINTS/WEIGHTS）---
    {
        rhi::VertexInputLayout skinnedLayout;
        skinnedLayout.stride = sizeof(he::asset::SkinnedVertex);
        skinnedLayout.attributes = {
            { 0, 0, rhi::VertexFormat::Float3, offsetof(he::asset::SkinnedVertex, position) },
            { 1, 0, rhi::VertexFormat::Float3, offsetof(he::asset::SkinnedVertex, normal) },
            { 2, 0, rhi::VertexFormat::Float2, offsetof(he::asset::SkinnedVertex, uv) },
            { 3, 0, rhi::VertexFormat::UByte4, offsetof(he::asset::SkinnedVertex, joint) },
            { 4, 0, rhi::VertexFormat::Float4, offsetof(he::asset::SkinnedVertex, weight) },
        };
        psoDesc.vertexLayout = skinnedLayout;
        psoDesc.debugName    = "ForwardPBR_Skinned";
        m_PBR_Skinned_PSO = device->CreatePipelineState(psoDesc);
        HE_ASSERT(m_PBR_Skinned_PSO, "ForwardPipeline: failed to create skinned PBR PSO");
    }

    // --- ToneMap 后处理子系统 ---
    m_ToneMap = std::make_unique<ToneMapPass>();
    m_ToneMap->Initialize(device, m_HDRWidth, m_HDRHeight);
    HE_CORE_INFO("ForwardPipeline: ToneMapPass initialized");

    // --- 天空盒子系统 ---
    m_Skybox = std::make_unique<SkyboxPass>();
    m_Skybox->Initialize(device, m_HDRWidth, m_HDRHeight);
    HE_CORE_INFO("ForwardPipeline: SkyboxPass initialized");

    // --- AA 子系统（默认 None，运行时切换）---
    m_AntiAliasing = std::make_unique<AA_None>();
    m_AntiAliasing->Initialize(device, m_HDRWidth, m_HDRHeight);
    HE_CORE_INFO("ForwardPipeline: AA initialized (None)");

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
        hdrDepthDesc.usage  = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;  // GPU Culling 需要采样深度
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
    m_GPUScene.Initialize(device);
    m_InstanceCuller.Initialize(device);   // 任务 25：逐实例剔除（可见列表 + 间接命令）
    m_Profiler.Initialize(device, rhi::kMaxProfilerPasses, MAX_FRAMES_IN_FLIGHT);  // GPU Profiler

    // --- SceneRenderer ---
    m_SceneRenderer = std::make_unique<SceneRenderer>();

    // --- Mesh Shader 支持（硬件支持时创建测试 PSO）---
    // 注：完整 Mesh Shader PSO 使用方式参见 VulkanPipeline.cpp 中的 mesh branch
    //    PipelineStateDesc::meshShader 设为 ShaderBytecode* 即可自动走 Mesh 管线路径
    //    创建后用 DrawMeshTasks(groupCount, 1, 1) 替代 DrawIndexed
    if (device->GetCaps().supportsMeshShaders) {
        HE_CORE_INFO("ForwardPipeline: Mesh Shader 硬件支持已就绪，可通过 PipelineStateDesc::meshShader 使用");
    }

    HE_CORE_INFO("ForwardPipeline initialized (with HDR + Tone Mapping + Skybox + ShadowSystem)");

    // Shader 热重载：注册 PSO 到热重载表
    {
        PSORecord rec;
        // 重建 PSO 描述符（与初始化时创建 m_PBR_PSO 的参数完全一致）
        rec.desc.debugName            = "ForwardPBR";
        rec.desc.vertexLayout         = vertexLayout;
        rec.desc.topology             = rhi::PrimitiveTopology::TriangleList;
        rec.desc.depthTest            = true;
        rec.desc.depthWrite           = true;
        rec.desc.depthCompare         = rhi::CompareFunc::LessEqual;
        rec.desc.depthFormat          = rhi::Format::D32_FLOAT;
        rec.desc.colorAttachmentCount = 1;
        rec.desc.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
        rec.desc.pushConstantRanges   = { pcRange };
        rec.desc.descriptorSetLayouts = { m_PerFrameLayout };
        // Shader 副本（自有 spirv 数据，ReloadShader 中会被替换）
        rec.vsCopy.stage      = rhi::ShaderStage::Vertex;
        rec.vsCopy.spirv      = m_VS.spirv;
        rec.vsCopy.entryPoint = "main";
        rec.fsCopy.stage      = rhi::ShaderStage::Pixel;
        rec.fsCopy.spirv      = m_FS.spirv;
        rec.fsCopy.entryPoint = "main";
        rec.desc.vertexShader = &rec.vsCopy;
        rec.desc.pixelShader  = &rec.fsCopy;
        rec.shaderNames[0]    = "PBR.vert";
        rec.shaderNames[1]    = "PBR.frag";
        rec.rawPSO            = m_PBR_PSO.get();
        m_PSORegistry.push_back(std::move(rec));
        // push_back 后重新定位指针：移动构造复制了指针值（原指向栈变量 rec），
        // 需要重定向到向量内自有副本，否则悬空
        auto& stored = m_PSORegistry.back();
        stored.desc.vertexShader = &stored.vsCopy;
        stored.desc.pixelShader  = &stored.fsCopy;

        HE_CORE_INFO("[HotReload] PSO 注册: PBR (vert + frag)");
    }

    return true;
}

// === Shader 热重载 ===

int ForwardPipeline::ReloadShader(StringView shaderName,
                                   const std::vector<u32>& newSpirv) {
    auto startTime = std::chrono::steady_clock::now();
    int count = 0;

    for (auto& rec : m_PSORegistry) {
        // 检查该 PSO 是否使用了这个 Shader
        int stageIndex = -1;
        if (rec.shaderNames[0] == shaderName) stageIndex = 0;  // 顶点shader
        if (rec.shaderNames[1] == shaderName) stageIndex = 1;  // 片元shader

        if (stageIndex < 0) continue;  // 不匹配，跳过

        // 更新 Shader 字节码
        if (stageIndex == 0) {
            rec.vsCopy.spirv = newSpirv;
        } else {
            rec.fsCopy.spirv = newSpirv;
        }

        // 重建 PSO
        auto newPSO = m_Device->CreatePipelineState(rec.desc);
        if (!newPSO) {
            HE_CORE_ERROR("[HotReload] PSO 重建失败: {}", shaderName);
            continue;
        }

        // 延迟销毁旧 PSO（GPU 可能仍在使用它的 VkRenderPass）
        // 3 帧后通过 NextFrame() 安全释放
        if (m_PBR_PSO) {
            m_RetiredPSOs.push_back({MAX_FRAMES_IN_FLIGHT, std::move(m_PBR_PSO)});
        }
        m_PBR_PSO = std::move(newPSO);
        rec.rawPSO = m_PBR_PSO.get();

        count++;
        HE_CORE_INFO("[HotReload] PSO 替换成功: {} → PBR", shaderName);
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    HE_CORE_INFO("[HotReload] {} 个 PSO 重建完成 ({}ms)", count, elapsed);

    return count > 0 ? count : 0;
}

void ForwardPipeline::Shutdown() {
    m_InstanceCuller.Shutdown();   // 任务 25：逐实例剔除（须在设备有效时释放 bindless 槽位）
    if (m_Device) {
        m_Device->DestroyDescriptorSetLayout(m_PerFrameLayout);
    }
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_LightBuffers[i].reset();
        m_ObjectBuffers[i].reset();
        m_ShadowBuffers[i].reset();
    }
    m_LightGridBuffer.reset();         // Forward+ LightGrid
    m_LightIndexListBuffer.reset();    // Forward+ LightIndexList
    m_PBR_PSO.reset();
    if (m_ToneMap) { m_ToneMap->Shutdown(); m_ToneMap.reset(); }
    if (m_Skybox)  { m_Skybox->Shutdown();  m_Skybox.reset(); }
    m_HDRTarget.reset();
    m_HDRDepth.reset();
    m_HDRSampler.reset();
    m_SecRecordLists.clear();  // Phase 5-4 sec CB 录制池

    // 阴影子系统（需在 m_Device 有效时清理）
    if (m_ShadowSystem) {
        m_ShadowSystem->Shutdown();
        m_ShadowSystem.reset();
    }

    m_GPUCulling.Shutdown(m_Device);
    m_GPUScene.Shutdown();
    m_Profiler.Shutdown();
    if (m_AntiAliasing) { m_AntiAliasing->Shutdown(); m_AntiAliasing.reset(); }

    m_Device = nullptr;
    HE_CORE_INFO("ForwardPipeline shut down");
}

void ForwardPipeline::NextFrame() {
    // 推进三缓冲槽位（帧首调用，确保 Shadow 和 Scene 使用同一帧的缓冲区）
    m_CurrentFrameSlot = (m_CurrentFrameSlot + 1) % MAX_FRAMES_IN_FLIGHT;

    // 清理延迟销毁的旧 PSO（等 GPU 完成 3 帧后安全释放）
    for (auto it = m_RetiredPSOs.begin(); it != m_RetiredPSOs.end(); ) {
        if (--it->first == 0)
            it = m_RetiredPSOs.erase(it);
        else
            ++it;
    }

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

    // 空中透视参数：从物理天空组件读取太阳方向 + 浑浊度（无物理天空时保持 0=关闭）
    float3 atmSunDir = float3(0, 1, 0);
    float atmTurbidity = 0.0f;
    he::GetPhysicalSkySun(world, atmSunDir, atmTurbidity);
    pc.atmosphere = float4(atmSunDir, atmTurbidity);

    auto collectLight = [&](he::Entity e, he::LightComponent& lc) {
        if (!lc.enabled) return;
        u32 i = pc.lightCount;
        if (i >= MAX_LIGHTS) return;

        // 色温 → RGB（叠加到 color 滤镜色）
        float3 lightColor = lc.color;
        if (lc.colorTemperature > 0.0f) lightColor *= render::KelvinToRGB(lc.colorTemperature);

        GPULight gl{};
        gl.colorIntensity  = float4(lightColor, lc.intensity);
        gl.shadowIndex     = m_ShadowSystem->GetShadowIndex(e);

        switch (lc.type) {
        case he::LightType::Directional: {
            auto* dl = static_cast<he::DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f);
            gl.positionRange = float4(0, 0, 0, 0);
            if (IsPhysicalLightEnabled(lc.illuminance)) {   // 物理模式（需全局开关）：照度 lux → 换算到渲染强度
                gl.colorIntensity.w = lc.illuminance * kPhysicalLightExposure;
                gl.positionRange.w   = -1.0f;
            }
            break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            gl.positionRange = float4(pos, pl->range);
            gl.directionType = float4(0, -1, 0, 1.0f);
            if (IsPhysicalLightEnabled(lc.luminousIntensity)) {   // 物理模式（需全局开关）：发光强度 cd → 换算到渲染强度
                gl.colorIntensity.w = lc.luminousIntensity * kPhysicalLightExposure;
                gl.positionRange.w   = -(pl->range);
            }
            break;
        }
        case he::LightType::Spot: {
            auto* sl = static_cast<he::SpotLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            float r = IsPhysicalLightEnabled(lc.luminousIntensity) ? -(sl->range) : sl->range;
            gl.positionRange = float4(pos, r);
            gl.directionType = float4(sl->direction, 2.0f);
            gl.coneAngles   = float2(sl->innerConeAngle, sl->outerConeAngle);
            if (IsPhysicalLightEnabled(lc.luminousIntensity)) gl.colorIntensity.w = lc.luminousIntensity * kPhysicalLightExposure;
            break;
        }
        case he::LightType::Rect: {
            auto* rl = static_cast<he::RectLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            gl.positionRange = float4(pos, rl->range);
            gl.directionType = float4(rl->normal, 3.0f);   // w=3 标记 Rect 类型；xyz=发光面法线
            gl.coneAngles   = float2(rl->width, rl->height); // 复用：x=宽度, y=高度
            if (IsPhysicalLightEnabled(lc.luminousIntensity)) gl.colorIntensity.w = lc.luminousIntensity * kPhysicalLightExposure;
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
    world.ForEach<he::RectLight>(collectLight);

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
// Bindless 材质 SSBO（per-material 材质数据）
// ============================================================

// 去重收集场景材质 → 写入单个 bindless 材质 SSBO 并注册
// 每材质（materialID）只写一份 GPUMaterialData；buffer 内元素索引 = materialID >> 2
void ForwardPipeline::UploadMaterialBindless(he::World& world) {
    // 按 materialID 去重（同一 materialID 的多个物体共享一份材质数据）
    std::unordered_map<u32, GPUMaterialData> uniqueMat;
    auto collect = [&](he::Entity, he::MeshComponent& m) {
        if (uniqueMat.count(m.materialID)) return;  // 已收集过该材质，跳过
        PBRMaterial mat = GetDefaultMaterial();
        mat.baseColorFactor = m.baseColorFactor;
        mat.emissiveFactor  = m.emissiveFactor;
        mat.metallicFactor  = m.metallicFactor;
        mat.roughnessFactor = m.roughnessFactor;
        mat.aoFactor        = m.aoFactor;
        mat.alphaCutoff     = m.alphaCutoff;
        mat.alphaMode       = static_cast<AlphaMode>(m.alphaMode);
        mat.doubleSided     = m.doubleSided;
        mat.unlit           = m.unlit;
        // 纹理路径 → textureMask（无纹理槽 shader 不采样，避免占位纹理污染）
        mat.baseColorTexture         = m.baseColorTexture;
        mat.normalTexture            = m.normalTexture;
        mat.metallicRoughnessTexture = m.metallicRoughnessTexture;
        mat.occlusionTexture         = m.occlusionTexture;
        mat.emissiveTexture          = m.emissiveTexture;
        GPUMaterialData g;
        FillMaterialData(g, mat);                    // 摊平成 GPUMaterialData（bindless 材质 SSBO 元素）
        uniqueMat[m.materialID] = g;
    };
    // 与 SceneRenderer 的绘制收集一致：MeshComponent + Cube/Sphere 派生组件都要收集
    world.ForEach<he::MeshComponent>([&](he::Entity e, he::MeshComponent& m) { collect(e, m); });
    world.ForEach<he::CubeComponent>([&](he::Entity e, he::CubeComponent& c) { collect(e, static_cast<he::MeshComponent&>(c)); });
    world.ForEach<he::SphereComponent>([&](he::Entity e, he::SphereComponent& s) { collect(e, static_cast<he::MeshComponent&>(s)); });
    world.ForEach<he::InstancedMeshComponent>([&](he::Entity e, he::InstancedMeshComponent& im) { collect(e, im); });
    world.ForEach<he::SkeletalMeshComponent>([&](he::Entity e, he::SkeletalMeshComponent& sm) { collect(e, sm); });
    world.ForEach<he::SplineMeshComponent>([&](he::Entity e, he::SplineMeshComponent& sm) { collect(e, sm); });

    if (uniqueMat.empty()) return;  // 场景无材质，跳过

    // materialID 是纹理基索引（每材质占 4 个纹理槽），材质数据索引 = materialID >> 2
    u32 maxSlot = 0;
    for (auto& kv : uniqueMat) maxSlot = std::max(maxSlot, kv.first >> 2);
    // 空槽位填默认 GPUMaterialData{}（值初始化 = 全 0），保证 buffer 内索引与 materialID>>2 对齐
    std::vector<GPUMaterialData> data(maxSlot + 1, GPUMaterialData{});
    for (auto& kv : uniqueMat) data[kv.first >> 2] = kv.second;

    u32 newCount = (u32)data.size();
    // 材质数变化时重建 buffer；否则复用（场景材质集通常静态，RegisterBuffer 只在首次/材质数变化时调用）
    if (!m_MaterialBuffer || newCount != m_MaterialCount) {
        rhi::BufferDesc desc;
        desc.size        = sizeof(GPUMaterialData) * newCount;
        desc.usage       = rhi::BufferUsage::Storage;
        desc.initialData = data.data();
        m_MaterialBuffer = m_Device->CreateBuffer(desc);
        m_MaterialCount  = newCount;
        // 注册到 bindless 堆（binding 30 = u_Materials[]；当前仅 1 个材质 buffer → handle=0）
        u32 handle = m_Device->GetBindlessHeap()->RegisterBuffer(m_MaterialBuffer.get());
        HE_CORE_INFO("ForwardPipeline: 上传 {} 个材质到 bindless SSBO (handle={})", newCount, handle);
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

    // pass 级调试标记：包裹整个场景渲染 pass（RenderDoc 中识别为「Forward Scene (HDR)」）
    cmd->BeginDebugLabel("Forward Scene (HDR)");
    cmd->BeginOffscreenPass(colorView, depthView, width, height, &clear, true);

    cmd->SetViewport({ 0, static_cast<float>(height),
        static_cast<float>(width), -static_cast<float>(height), 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, width, height });
}

void ForwardPipeline::EndHDRPass(rhi::IRHICommandList* cmd) {
    cmd->EndOffscreenPass();
    cmd->EndDebugLabel();   // 闭合 "Forward Scene (HDR)" pass 级标记

    // 布局转换：COLOR_ATTACHMENT → 着色器只读（ToneMap 采样）
    cmd->PipelineBarrier(
        rhi::PipelineStage::ColorAttachmentOutput,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::RenderTarget,
        rhi::ResourceState::ShaderResource,
        m_HDRTarget.get());

    // 预设 ToneMap PSO 为下一个 RenderPass 的初始管线（匹配 SwapChain 实际颜色格式：SDR=BGRA8，HDR=A2B10G10R10）
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

    // RSM 渲染（非 RG 路径）：与 RG 路径读**同一份**按场景包围盒拟合的固定光锥（本帧 Render
    // 开头的 RefreshRSMFrustum 已算好）。此前这里读 CSM 级联 0 的 VP —— 它拟合相机视锥，
    // 且由 Shadow pass 在执行时才写入 ⇒ 帧图里没有依赖边、顺序不受保证（§9.2-AD）。
    if (m_RSMFrustumValid && m_RSM) {
        m_RSM->SetLightViewProj(m_RSMLightViewProj, m_RSM->GetRSMPositionMap()->GetWidth(),
                                m_ShadowSystem->GetShadowSampler(),
                                m_DescSets[m_CurrentFrameSlot]);
        // 通量计算要读方向光的颜色/强度（§9.2-AA：不绑光源缓冲就会读到对象缓冲）
        m_RSM->SetLightBuffer(GetCurrentLightBuffer());
        // 从光源 POV 渲染几何体 → RSM 纹理（使用 RSM 自有的独立深度缓冲）
        m_RSM->RenderRSMPass(cmd, world, sg);
        UpdateRSMBindings();
    }
}

void ForwardPipeline::UpdateRSMBindings() {
    if (!m_RSM) return;
    rhi::IRHITexture* posMap  = m_RSM->GetRSMPositionMap();
    rhi::IRHITexture* nrmMap  = m_RSM->GetRSMFluxMap();      // 任务 30 起只存编码法线
    rhi::IRHITexture* radMap  = m_RSM->GetRSMRadianceMap();  // VPL 出射辐射度
    rhi::IRHISampler* sampler = m_RSM->GetRSMSampler();
    // RSM 绑定在 set=0（per-frame），只需更新共享描述符集
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_Device->UpdateDescriptorSet(m_DescSets[i], kGPUBinding_RSMPosition, rhi::DescriptorType::CombinedImageSampler, posMap, sampler);
        m_Device->UpdateDescriptorSet(m_DescSets[i], kGPUBinding_RSMFlux, rhi::DescriptorType::CombinedImageSampler, nrmMap, sampler);
        m_Device->UpdateDescriptorSet(m_DescSets[i], kGPUBinding_RSMRadiance, rhi::DescriptorType::CombinedImageSampler, radMap, sampler);
    }
}

void ForwardPipeline::UpdateIBLBindings(GI_IBL* gi) {
    rhi::IRHITexture* irr    = gi->GetIrradianceMap();
    rhi::IRHITexture* pref   = gi->GetPrefilterMap();
    rhi::IRHITexture* lut    = gi->GetBRDF_LUT();
    rhi::IRHISampler* sampler = gi->GetIBLSampler();

    // IBL 绑定在 set=0（per-frame），只需更新共享描述符集
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_Device->UpdateDescriptorSet(m_DescSets[i], kGPUBinding_IrradianceMap, rhi::DescriptorType::CombinedImageSampler, irr, sampler);
        m_Device->UpdateDescriptorSet(m_DescSets[i], kGPUBinding_PrefilterMap, rhi::DescriptorType::CombinedImageSampler, pref, sampler);
        m_Device->UpdateDescriptorSet(m_DescSets[i], kGPUBinding_BRDF_LUT, rhi::DescriptorType::CombinedImageSampler, lut, sampler);
    }
}

void ForwardPipeline::RenderSkybox(rhi::IRHICommandList* cmd, he::World& world,
                                    const CameraData& camera) {
    if (!m_Skybox) return;
    SubsystemContext ctx;
    ctx.world = &world;
    ctx.camera = &camera;
    m_Skybox->Update(ctx);
    m_Skybox->Render(cmd);
}

void ForwardPipeline::RenderToneMapPass(rhi::IRHICommandList* cmd) {
    // pass 级调试标记：RenderDoc 中识别为「色调映射输出到屏幕」
    cmd->SetDrawDebugLabel("ToneMap -> BackBuffer");
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
    hdrDepthDesc.usage  = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    m_HDRDepth = m_Device->CreateTexture(hdrDepthDesc);
    // ToneMapPass 输入会自动通过每帧 SetInput 更新，无需手动更新描述符集
}

// ---- IRenderPipeline 包装方法 ----

void ForwardPipeline::RefreshRSMFrustum(he::World& world, const CameraData& camera) {
    // 场景包围盒：与 Deferred 侧同一条做法（网格包围盒 × 世界变换），每 30 帧重算一次。
    // 【不要用帧计数器当这个计时器】它未必每帧自增，用取模判据会退化成"每帧都重算"。
    if (m_SceneBoundsCountdown == 0u) {
        he::AABB sceneBounds;
        world.ForEach<he::MeshComponent>([&](he::Entity e, he::MeshComponent& mesh) {
            if (auto* tf = world.GetComponent<TransformComponent>(e)) {
                sceneBounds.Expand(mesh.GetBounds().Transform(tf->GetLocalMatrix()));
            }
        });
        if (sceneBounds.IsValid()) m_SceneBounds = sceneBounds;
        m_SceneBoundsCountdown = kSceneBoundsRefreshFrames;
    }
    --m_SceneBoundsCountdown;

    // 先按"本帧没有 RSM"复位：下面任一条件不成立时，UBO 里的 rsmValid 就是 0，
    // PBR 侧据此直接返回 0，不去采可能没写过的 RSM 纹理（§9.2-T 的约定）。
    m_RSMFrustumValid  = false;
    m_RSMDirLightValid = false;
    m_RSMVplScale      = 0.0f;
    m_RSMLightViewProj = float4x4(1.0f);

    // 注册条件必须与 BuildFrameGraph 里那段**同源**，否则会出现"UBO 说有效但 pass 没跑"。
    if (!m_GIConfig.ShouldRunRSM() || !m_RSM || !m_ShadowSystem
        || !m_ShadowSystem->HasActiveShadows()) {
        return;
    }

    // 光源方向与"是否存在投影方向光"：无方向光时 RSM 的通量为 0（也就没有间接光可言），
    // 但 pass 仍会注册 —— 与 Deferred 侧一致；这个布尔只用于决定 PBR 要不要做内联查找。
    float3 ldir = float3(0.3f, -1.0f, 0.4f);   // 与 Deferred 同一个默认方向
    world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& l) {
        if (l.enabled && l.castShadow) {
            ldir = glm::normalize(l.direction);
            m_RSMDirLightValid = true;
        }
    });

    // 【固定视锥】RSM 是被当作**世界空间**源使用的（接收点与探针都在世界空间查表），
    // 所以光锥不能拟合相机视锥；覆盖范围由场景包围盒推出（纯几何在 GI/RSMFrustum.h，有单测）。
    // 包围盒还没算出来时（首帧 / 空场景）退回覆盖相机附近的保守视锥，保证"有产出"。
    const float3 fitMin = m_SceneBounds.IsValid() ? m_SceneBounds.min : (camera.position - float3(50.0f));
    const float3 fitMax = m_SceneBounds.IsValid() ? m_SceneBounds.max : (camera.position + float3(50.0f));
    const auto   fit    = FitRSMFrustumToBounds(fitMin, fitMax, ldir);
    if (!fit) return;   // 包围盒退化/方向为零向量 ⇒ 本帧不注册 RSM

    m_RSMLightViewProj = fit->viewProj;
    m_RSMFrustumValid  = true;
    // VPL 采样缩放：5×5 网格的采样图案覆盖 ±2 步，步长 kRSMInlineStepUV（与着色器常量同源）
    m_RSMVplScale = RSMVplScaleFromArea(
        RSMSquareArea(fit->halfExtent, fit->halfExtent, 2.0f * kRSMInlineStepUV),
        kRSMInlineSampleCount);
    // 把 RSM 三张图绑进 per-frame 描述符集（任务 34 的第二半）：**RG 路径此前从不调用它**
    // ——只有非 RG 的 PrepareGI 调。于是 RG 路径下 PBR 采样的是 Initialize 时绑的
    // **bindless 占位纹理**，RSM 项恒为 0（实测：pass 在跑、三张图有 53% 覆盖，`{RSM}` 的 HDR
    // 仍与空层栈逐位相同）。绑定必须发生在 pass 注册/执行之前，故放在这里（每帧开头一次）。
    UpdateRSMBindings();
}

void ForwardPipeline::FillGIBlendUBO() {
    if (!m_GIBuffers[m_CurrentFrameSlot]) return;
    // 与 Deferred 帧图里那段 fillSlots **同构**：逐通道把层栈的源写进 UBO 槽位，
    // 置信度掩码由 GIChannelBlendData::Add 统一推导（前向没有屏幕空间源 ⇒ 掩码全 None）。
    GIBlendParams bp{};
    auto fillSlots = [this](GIChannelBlendData& b, const GIChannelStack& st) {
        b.count = 0;
        b.mode  = (u32)st.mode;
        b.edgeFade = m_GIConfig.edgeFade;
        for (u32 i = 0; i < st.count; i++) {
            const GISourceDesc& s = st.sources[i];
            b.Add((u32)s.id, s.weight, s.falloffDistance);
        }
    };
    // CPU 侧用 GIChannelBlendData 逐槽构造（置信度掩码在那里统一推导），
    // 再按 LightingPass 的同一做法 memcpy 进 shader 结构 —— 两份布局有 static_assert 保证一致
    GIChannelBlendData d{}, sp{}, ao{};
    fillSlots(d,  m_GIConfig.diffuse);
    fillSlots(sp, m_GIConfig.specular);
    fillSlots(ao, m_GIConfig.ao);
    d.furnaceMode = m_GIConfig.furnaceMode ? 1u : 0u;
    std::memcpy(&bp.diffuse,  &d,  sizeof(GIChannelBlendData));
    std::memcpy(&bp.specular, &sp, sizeof(GIChannelBlendData));
    std::memcpy(&bp.ao,       &ao, sizeof(GIChannelBlendData));

    // RSM 内联求和的两个参数（任务 30 / 任务 34）：采样缩放与光源 VP 都由**同一个**按场景
    // 包围盒拟合的固定光锥推出 —— 与 RSM_Generate pass 用的是同一份（RefreshRSMFrustum）。
    // 此前这里的缩放由 CSM 级联 0 的 VP 反推、而着色器的查找也用那个 CSM VP：两者虽然自洽，
    // 但那个 VP 拟合相机视锥（视角一变 RSM 内容就变）且依赖 Shadow pass 的执行顺序。
    bp.rsmVplScale      = m_RSMVplScale;
    bp.rsmLightViewProj = m_RSMLightViewProj;
    // rsmValid 只在 pass 本帧真的注册（且有投影方向光）时为 1：否则着色器会去采上一帧/
    // 未初始化的 RSM 纹理 —— 那是静默的（画面只是偏暗）且随显存布局不可复现（§9.2-T）。
    bp.rsmValid = (m_RSMFrustumValid && m_RSMDirLightValid) ? 1.0f : 0.0f;

    void* mapped = m_GIBuffers[m_CurrentFrameSlot]->Map();
    if (mapped) {
        std::memcpy(mapped, &bp, sizeof(GIBlendParams));
        m_GIBuffers[m_CurrentFrameSlot]->Unmap();
    }
}

void ForwardPipeline::Render(rhi::IRHICommandList* cmd, he::World& world,
                              he::SceneGraph& sg, const CameraData& camera,
                              float deltaTime)
{
    // RSM 固定光锥必须**先**刷新（任务 34）：UBO（FillGIBlendUBO）与 frame graph 的
    // RSM pass 注册/参数两处消费者都读它，且两者都在下面几步之内。
    RefreshRSMFrustum(world, camera);
    // GI 分层合成参数：RG 路径与非 RG 路径都要用，故在分支之前填（每帧一次的小 UBO 写入）
    FillGIBlendUBO();
    if (m_UseRenderGraph) {
        RenderGraph rg;
        rg.SetProfiler(&m_Profiler);
        BuildFrameGraph(rg, world, sg, camera);
        rg.Compile();
        rg.Execute(cmd, m_Device);
        return;
    }
    // 非 RG 路径：手动渲染阴影
    // 交换链颜色格式同步（SDR=BGRA8，HDR=A2B10G10R10），非 RG 路径 ToneMap 输出需与后备缓冲一致
    rhi::Format swapFmt = m_SwapChain ? m_SwapChain->GetColorFormat() : rhi::Format::BGRA8_UNORM;
    m_ToneMap->SetOutputFormat(swapFmt);
    m_ToneMap->SetHDREnabled(swapFmt == rhi::Format::A2B10G10R10_UNORM_PACK32);
    he::SyncPhysicalSkyToSun(world);  // 物理天空太阳→方向光同步（阴影/光照收集前）
    if (m_ShadowSystem && m_ShadowSystem->HasActiveShadows()) {
        u32 slot = m_CurrentFrameSlot;
        // 切换 binding 2 到阴影 Object Buffer（仅更新 set=0 per-frame 集）
        m_Device->UpdateDescriptorSet(m_DescSets[slot], rhi::kBindingObjectData,
            rhi::DescriptorType::StorageBuffer, m_ShadowObjBuffers[slot].get());

        m_ShadowSystem->Render(cmd);

        // 恢复 binding 2
        m_Device->UpdateDescriptorSet(m_DescSets[slot], rhi::kBindingObjectData,
            rhi::DescriptorType::StorageBuffer, m_ObjectBuffers[slot].get());
    }
    PrepareGI(cmd, world, sg);
    // GPU 视锥剔除：必须在 render pass 之外（BeginHDRPass 会 Begin 本帧的 HDR pass）
    RunGPUCulling(cmd, world, sg, camera);
    BeginHDRPass(cmd, m_HDRWidth, m_HDRHeight);
    BeginFrame(cmd, m_HDRWidth, m_HDRHeight);
    RenderScene(cmd, world, sg, camera);
    RenderSkybox(cmd, world, camera);
    EndHDRPass(cmd);

    // AA Pass（FXAA 等在 HDR→ToneMap 之间或 ToneMap 之后）
    if (m_AntiAliasing && m_AntiAliasing->IsEnabled() && m_AntiAliasing->GetMode() != AAMode::None) {
        m_AntiAliasing->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
        m_AntiAliasing->Render(cmd);
    }
}


// BuildFrameGraph 实现位于 ForwardPipeline_FrameGraph.cpp

void ForwardPipeline::OnResize(u32 width, u32 height) {
    ResizeHDRTarget(width, height);
    if (m_ToneMap) m_ToneMap->OnResize(width, height);
    if (m_Skybox)  m_Skybox->OnResize(width, height);
}

// ============================================================
// GPU 视锥剔除（Compute）— 读回上帧结果 → 调度下帧
// 必须在 render pass **之外**调用（见头文件说明）。
// ============================================================
void ForwardPipeline::RunGPUCulling(
    rhi::IRHICommandList* cmd,
    he::World& world,
    he::SceneGraph& sceneGraph,
    const CameraData& camera)
{
    if (!m_GPUCulling.enabled) return;

    // 1) 读回上一帧 GPU culling 结果（该帧已 submit 执行完毕）
    m_GPUCulling.Readback(m_Device, m_GPUVisibleIndices);

    // 2) 收集场景对象 → GPUScene SSBO
    m_GPUScene.Collect(world, sceneGraph, camera);
    // FillGPUScene 必须在 Collect 之后、Upload 之前（与 Deferred 一致）
    if (!m_BatchBuilt) { m_MeshBatcher.Build(world); m_BatchBuilt = true; }
    m_MeshBatcher.FillGPUScene(m_GPUScene);
    m_GPUScene.Upload(m_Device);

    // 3) 绑定 GPUScene SSBO / HDR 深度 → Dispatch Compute
    m_GPUCulling.SetSceneBuffer(m_Device, m_GPUScene.GetObjectBuffer());
    if (m_HDRDepth) m_GPUCulling.SetDepthTexture(m_Device, m_HDRDepth.get(),
                                                 m_HDRWidth, m_HDRHeight);
    m_GPUCulling.Dispatch(cmd, camera.GetViewProjMatrix(), m_GPUScene.GetObjectCount(),
                          m_HDRWidth, m_HDRHeight);
    // Dispatch 会改绑 compute 管线：恢复后续绘制要用的 PBR 管线
    cmd->SetPipeline(m_PBR_PSO.get());
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

    // 任务 25：逐实例剔除统计（面板/日志用）
    u32 culledInstanceMeshes = 0;   // 走逐实例剔除的实例化网格数
    u32 culledInstances      = 0;   // 剔除后可见实例数
    u32 totalInstances       = 0;   // 剔除前实例总数

    // 帧级 push constant
    PushConstantData framePC{};
    framePC.viewProjMatrix = viewProj;
    framePC.cameraPosition = float4(camera.position, 0.0f);
    framePC.iblIntensity  = m_GI ? m_GI->GetSettings().intensity : 1.0f;

    // 收集光源（阴影数据由 ShadowSystem 管理，此处仅收集光照）
    CollectLights(framePC, world, sceneGraph, camera);

    // Forward+: 设置 Cluster 参数（与 ForwardPlus_LightCull pass 共享 m_ClusteredShading 状态）
    if (m_UseForwardPlus && m_ClusteredShading.enabled) {
        float n = camera.nearPlane, f = camera.farPlane;
        framePC.clusterTilesX    = m_ClusteredShading.GetTileCountX();
        framePC.clusterTilesY    = m_ClusteredShading.GetTileCountY();
        framePC.clusterNear      = n;
        framePC.clusterFar       = f;
        framePC.clusterLogFactor = std::log(f / n);
        framePC.useClustered     = 1;
    } else {
        framePC.clusterTilesX    = 0;
        framePC.clusterTilesY    = 0;
        framePC.useClustered     = 0;
    }

    // 材质参数读取开关：1=bindless（u_Materials[]），0=内联（GPUObjectData 字段）；默认内联
    framePC.useBindlessMaterial = m_UseBindlessMaterial ? 1u : 0u;

    // ============================================================
    // GPU 视锥剔除由 RunGPUCulling 完成 —— 见下方方法与本文件的 RGBuildFrameGraph
    // ============================================================
    // 【§0.6.2 校验修复】此前这段（读回 + 收集场景 + Dispatch）就在本函数里，而本函数由
    // Scene pass 在 render pass 已经 Begin 之后调用 ⇒ 每帧一条
    // VUID-vkCmdDispatch-None-10672（dispatch 出现在 render pass 内部，实测 10 条），
    // 且 dispatch 采样 hdrDepth 时它正作为该 pass 的深度附件（非法反馈）。
    // 现拆成独立的 RunGPUCulling：RG 路径由 "GPU_Cull" compute pass 调用，非 RG 路径在
    // BeginHDRPass 之前调用，两条路径都不再落在 render pass 里。

    // SceneRenderer 准备所有 draw items
    auto allDrawItems = m_SceneRenderer->Prepare(world, sceneGraph, camera,
                                                  m_ObjectBuffers[m_CurrentFrameSlot].get());

    // GPU 剔除后过滤：构建可见 draw 列表
    std::vector<DrawItem> filteredItems;
    // GPU 剔除时，传统 draw 命令也使用全部对象列表。
    // SceneRenderer 和 GPUScene 的 objectIndex 是独立索引空间，不能直接对号过滤。
    // 实际剔除由后续 DrawIndexedIndirect 根据 GPU IndirectCmdBuf 完成。
    filteredItems = std::move(allDrawItems);
    u32 totalDraws = (u32)filteredItems.size();

    // 上传去重材质到 bindless SSBO（须在 Flush 之前调用，Flush 才会把材质 buffer 推送到 binding 30）
    UploadMaterialBindless(world);

    // ============================================================
    // 实例化网格 Pass（B1）：脏标记 → 重建实例变换 SSBO → 注册 bindless
    // 须在 Flush 之前注册（Flush 才把新 SSBO 句柄推送进描述符集）；
    // useInstanceID=2 模式，单次 DrawIndexed 渲染 N 个实例。
    // MVP 限制：Forward 非 GPU-Culling 路径；Deferred/间接路径后续扩展。
    // ============================================================
    world.ForEach<he::InstancedMeshComponent>([&](he::Entity, he::InstancedMeshComponent& im) {
        const u32 count = im.GetInstanceCount();
        // 任务 23/25：实例变换上传（容量够就原地复用；退役队列有界延迟释放）
        // —— 与 Deferred 的 GBuffer 路径共用同一份逻辑（InstanceCuller::UploadInstanceTransforms）
        const u32 instHandle = m_InstanceCuller.UploadInstanceTransforms(m_Device, im);
        if (count == 0 || im.GetIndexCount() == 0 || instHandle == 0) return;

        // 定位该组件的对象条目（objectIndex → 材质数据）
        u32 objIndex = 0;
        bool found = false;
        for (auto& di : filteredItems) {
            if (di.mesh == static_cast<he::MeshComponent*>(&im)) { objIndex = di.objectIndex; found = true; break; }
        }
        if (!found) return;

        // 实例化绘制（模式 2：VS 按 SV_InstanceID 取实例变换）
        PushConstantData pc = framePC;
        pc.objectIndex       = objIndex;
        pc.useInstanceID     = 2;
        pc.instanceSSBOHandle = im.instanceSSBOHandle;

        // ── 任务 25：逐实例 GPU 视锥剔除 ──
        // 开了 enableFrustumCull 就先把"每个实例的世界 AABB"过一遍六平面测试：
        // 通过者压缩进可见列表，命令里的 instanceCount 由 GPU 原子累加；
        // 绘制改成 DrawIndexedIndirect，顶点着色器按可见列表取实例变换。
        // 关掉时保持原路径（整批实例一次 DrawIndexed），便于 A/B 对比。
        bool useCull = im.enableFrustumCull && m_InstanceCuller.GetPSO() != nullptr;
        const u32 frameSlot = m_CurrentFrameSlot % rhi::kMaxFramesInFlight;
        if (useCull) {
            if (!im.instanceCullCmd[frameSlot]) {   // 命令缓冲按飞行帧存活（每帧都要一份）
                im.instanceCullCmd[frameSlot] = m_InstanceCuller.CreateCommandBuffer(
                    im.GetIndexCount(), 0, 0);
                if (im.instanceCullCmd[frameSlot]) {
                    im.instanceCullCmdHandle[frameSlot] =
                        m_Device->GetBindlessHeap()->RegisterBuffer(im.instanceCullCmd[frameSlot].get());
                }
            }
            if (!im.instanceCullCmd[frameSlot] || im.instanceCullCmdHandle[frameSlot] == 0) {
                useCull = false;   // 命令缓冲创建失败：安全回退整批绘制
            }
        }

        if (useCull) {
            // 实例网格的局部包围盒（内置立方体 = ±0.5；glTF 资产走组件包围盒）
            const AABB lb = im.GetBounds();
            const u32 prevVisible = m_InstanceCuller.Cull(
                cmd, im.instanceBuffer.get(), im.instanceSSBOHandle,
                im.instanceCullCmd[frameSlot].get(), im.instanceCullCmdHandle[frameSlot],
                count, frameSlot, lb.min, lb.max, framePC.viewProjMatrix);

            pc.instanceVisibleHandle = m_InstanceCuller.GetVisibleIndicesHandle(frameSlot);
            cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSets[m_CurrentFrameSlot]);
            cmd->SetDrawDebugLabel("Forward InstancedMesh (逐实例剔除)");
            cmd->SetPushConstants(0, sizeof(PushConstantData), &pc);
            cmd->SetVertexBuffer(im.GetVertexBuffer().get(), 0);
            cmd->SetIndexBuffer(im.GetIndexBuffer().get());
            // 一条命令、stride 20（VkDrawIndexedIndirectCommand）：instanceCount 由 cull 写入
            cmd->DrawIndexedIndirect(im.instanceCullCmd[frameSlot].get(), 0, 1,
                                     sizeof(InstanceIndirectCommand));
            // 读回统计（上一帧 GPU 写入的值；仅用于面板/日志）
            im.visibleInstanceCount = prevVisible;
            drawCount += prevVisible;
            ++culledInstanceMeshes;
            culledInstances += prevVisible;
            totalInstances += count;
            return;
        }

        pc.instanceVisibleHandle = 0;
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSets[m_CurrentFrameSlot]);
        cmd->SetDrawDebugLabel("Forward InstancedMesh");
        cmd->SetPushConstants(0, sizeof(PushConstantData), &pc);
        cmd->SetVertexBuffer(im.GetVertexBuffer().get(), 0);
        cmd->SetIndexBuffer(im.GetIndexBuffer().get());
        cmd->DrawIndexed(im.GetIndexCount(), count);
        drawCount += count;   // 实例计入绘制统计
    });

    // ============================================================
    // 骨骼蒙皮 Pass（Phase C C1b）：骨骼矩阵 SSBO 上传（脏标记）
    // → 蒙皮 PSO + useInstanceID=3 绘制（顶点着色器按权重混合 4 骨骼矩阵）
    // MVP 限制：Forward 非 GPU-Culling 路径；Deferred/间接路径后续扩展。
    // ============================================================
    world.ForEach<he::SkeletalMeshComponent>([&](he::Entity, he::SkeletalMeshComponent& sm) {
        // 任务 23：帧边界推进退役队列（有界释放）
        sm.AdvanceRetireQueue();
        if (!sm.skeleton || sm.GetIndexCount() == 0 || sm.boneMatrices.empty()) return;

        // 骨骼矩阵上传：容量够 → Map 原地更新（句柄不变）；容量不够 → 扩建 + 旧缓冲延迟释放
        //（禁止每帧重建缓冲：SSBO 数组容量有限，重建会不断消耗 bindless 槽位）
        const u32 needCount = (u32)sm.boneMatrices.size();
        if (sm.bBonesDirty || !sm.boneBuffer) {
            if (!sm.boneBuffer || sm.boneBufferCapacity < needCount) {
                rhi::BufferDesc desc;
                desc.size        = sizeof(float4x4) * needCount;
                desc.usage       = rhi::BufferUsage::Storage;
                desc.initialData = sm.boneMatrices.data();
                desc.cpuAccess   = true;
                if (sm.boneBuffer) {
                    m_Device->GetBindlessHeap()->ReleaseBuffer(sm.boneSSBOHandle);
                    sm.RetireBoneBuffer();
                }
                sm.boneBuffer = m_Device->CreateBuffer(desc);
                sm.boneBufferCapacity = needCount;
                sm.boneSSBOHandle = m_Device->GetBindlessHeap()->RegisterBuffer(sm.boneBuffer.get());
            } else {
                // 复用缓冲：重映射写入最新骨骼矩阵（与 GPUScene::Upload 同一模式）
                void* mapped = sm.boneBuffer->Map();
                if (mapped) {
                    std::memcpy(mapped, sm.boneMatrices.data(), sizeof(float4x4) * needCount);
                    sm.boneBuffer->Unmap();
                }
            }
            sm.bBonesDirty = false;
        }

        // 定位对象条目（objectIndex → 材质数据）
        u32 objIndex = 0;
        bool found = false;
        for (auto& di : filteredItems) {
            if (di.mesh == static_cast<he::MeshComponent*>(&sm)) { objIndex = di.objectIndex; found = true; break; }
        }
        if (!found) return;

        // 蒙皮绘制（模式 3）
        PushConstantData pc = framePC;
        pc.objectIndex       = objIndex;
        pc.useInstanceID     = 3;
        pc.instanceSSBOHandle = sm.boneSSBOHandle;
        cmd->SetPipeline(m_PBR_Skinned_PSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSets[m_CurrentFrameSlot]);
        cmd->SetDrawDebugLabel("Forward SkeletalMesh");
        cmd->SetPushConstants(0, sizeof(PushConstantData), &pc);
        cmd->SetVertexBuffer(sm.GetVertexBuffer().get(), 0);
        cmd->SetIndexBuffer(sm.GetIndexBuffer().get());
        cmd->DrawIndexed(sm.GetIndexCount());
        cmd->SetPipeline(m_PBR_PSO.get());   // 恢复主 PSO
        ++drawCount;
    });

    // 推送 bindless 纹理到全部已注册描述符集（Flush 自动遍历全部 set）
    m_Device->GetBindlessHeap()->Flush();

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
                    if (di.bInstanced) continue;   // 实例化网格由专用 Pass 绘制
                    PushConstantData pc = framePC;
                    pc.objectIndex = di.objectIndex;
                    secCmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSets[m_CurrentFrameSlot]);  // set=0: per-frame + bindless
                    // 不再需要 bind set=1 — 纹理采样通过 bindless u_Textures[] 访问
                    // DrawCall 调试 marker：标记当前绘制的物体（RenderDoc 定位用，每线程独立 CB 安全）
                    char label[64];
                    snprintf(label, sizeof(label), "Forward Obj#%u", di.objectIndex);
                    secCmd->SetDrawDebugLabel(label);
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
        // ExecuteIndirect 路径（仅当 Batch 已构建且 GPU Culling 有结果）
        bool useIndirect = m_UseExecuteIndirect && m_BatchBuilt && m_GPUCulling.enabled
                        && m_MeshBatcher.GetTotalIndexCount() > 0
                        && m_GPUCulling.GetLastVisibleCount() > 0;
        if (useIndirect) {
            cmd->SetVertexBuffer(m_MeshBatcher.GetVertexBuffer(), 0);
            cmd->SetIndexBuffer(m_MeshBatcher.GetIndexBuffer(), 0);
            framePC.useInstanceID = 1;  // SV_InstanceID 模式
            cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSets[m_CurrentFrameSlot]);
            cmd->SetPushConstants(0, sizeof(PushConstantData), &framePC);
            // DrawCall 调试 marker（RenderDoc 定位用）
            char label[64];
            snprintf(label, sizeof(label), "Forward Indirect (%u)", m_GPUCulling.GetLastVisibleCount());
            cmd->SetDrawDebugLabel(label);
            cmd->DrawIndexedIndirect(m_GPUCulling.GetIndirectBuffer(), 0,
                m_GPUCulling.GetLastVisibleCount(), sizeof(IndirectDrawCommand));
            drawCount = m_GPUCulling.GetLastVisibleCount();
        } else {
            framePC.useInstanceID = 0;  // push constant 模式
            for (auto& di : filteredItems) {
                if (di.bInstanced) continue;   // 实例化网格由专用 Pass 绘制
                PushConstantData pc = framePC;
                pc.objectIndex = di.objectIndex;
                cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSets[m_CurrentFrameSlot]);
                // DrawCall 调试 marker：标记当前绘制的物体（RenderDoc 定位用）
                char label[64];
                snprintf(label, sizeof(label), "Forward Obj#%u", di.objectIndex);
                cmd->SetDrawDebugLabel(label);
                cmd->SetPushConstants(0, sizeof(PushConstantData), &pc);
                cmd->SetVertexBuffer(di.mesh->GetVertexBuffer().get(), 0);
                cmd->SetIndexBuffer(di.mesh->GetIndexBuffer().get());
                cmd->DrawIndexed(di.mesh->GetIndexCount());
                drawCount++;
            }
        }
    }

    m_LastDrawCount = drawCount;
    m_LastTriCount  = 0;
    // 三角形计数（粗略估算：每个 draw 平均 indexCount/3）
    for (auto& di : filteredItems)
        m_LastTriCount += di.mesh->GetIndexCount() / 3;

    // 任务 25：逐实例剔除统计（0 个走剔除时不覆盖，保留最后一次有效值）
    if (culledInstanceMeshes > 0) {
        m_LastCulledInstanceMeshes = culledInstanceMeshes;
        m_LastVisibleInstances     = culledInstances;
        m_LastTotalInstances       = totalInstances;
    }

    static bool s_FirstFrame = true;
    if (s_FirstFrame) {
        HE_CORE_INFO("ForwardPipeline::RenderScene: {} draws, {} tris, {} lights",
            drawCount, m_LastTriCount, framePC.lightCount);
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

    // DrawCall 调试 marker（RenderDoc 定位用）
    char label[64];
    snprintf(label, sizeof(label), "Forward Mesh Obj#%u", framePC.objectIndex);
    cmd->SetDrawDebugLabel(label);
    cmd->SetPushConstants(0, sizeof(PushConstantData), &framePC);
    cmd->SetVertexBuffer(mesh->GetVertexBuffer().get(), 0);
    cmd->SetIndexBuffer(mesh->GetIndexBuffer().get());
    cmd->DrawIndexed(mesh->GetIndexCount());
}

} // namespace he::render
