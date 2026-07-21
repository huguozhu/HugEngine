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
#include "GBuffer.mesh.spv.h"
#include "RT_Shadow.rgen.spv.h"
#include "RT_Common.rmiss.spv.h"
#include "RT_Common.rchit.spv.h"
#include "AntiAliasing/AA_None.h"
#include "AntiAliasing/AA_FXAA.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>  // orthoRH_ZO (Vulkan Z [0,1])
#include <unordered_set>

#include <chrono>
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
        { 7,  rhi::DescriptorType::StorageBuffer,        1, 16 },  // LightGrid（Forward+）
        { 8,  rhi::DescriptorType::StorageBuffer,        1, 16 },  // LightIndexList（Forward+）
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

        // 注册默认占位纹理到 BindlessTextureManager（作为 null/无纹理 mesh 的回退）
        // 预分配 materialID=0 的 4 个纹理槽位（BaseColor, Normal, MetallicRoughness, Occlusion），
        // 确保 materialID=0 的 mesh（如 02.Cube 中无纹理的立方体/球体）始终有有效纹理
        he::asset::BindlessTextureManager::Instance().SetDefaultTexture(
            m_BindlessPlaceholder.get(), m_BindlessSampler.get());
        he::asset::BindlessTextureManager::Instance().RegisterMaterial(
            nullptr, nullptr,   // BaseColor → 默认白色纹理
            nullptr, nullptr,   // Normal → 默认白色纹理(法线=[0.5,0.5,1.0]=无扰动)
            nullptr, nullptr,   // MetallicRoughness → 默认白色纹理
            nullptr, nullptr);  // Occlusion → 默认白色纹理(AO=1.0)
    }

    // --- 分配三缓冲共享描述符集（set=0: per-frame + bindless）---
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::DescriptorSetHandle set = device->AllocateDescriptorSet(m_PerFrameLayout);
        device->UpdateDescriptorSet(set, 1, rhi::DescriptorType::StorageBuffer,
                                    m_LightBuffers[i].get());
        device->UpdateDescriptorSet(set, rhi::kBindingObjectData, rhi::DescriptorType::StorageBuffer,
                                    m_ObjectBuffers[i].get());
        device->UpdateDescriptorSet(set, 3, rhi::DescriptorType::StorageBuffer,
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
        // 绑定 5-6: bindless 占位符（BindlessTextureManager 在渲染时更新）
        {
            rhi::IRHITexture* texPtrs[] = { m_BindlessPlaceholder.get() };
            rhi::IRHISampler* sampPtrs[] = { m_BindlessSampler.get() };
            device->UpdateDescriptorSet(set, rhi::kBindingBindlessTextures, rhi::DescriptorType::SampledImage,
                texPtrs, nullptr, 1);
            device->UpdateDescriptorSet(set, rhi::kBindingBindlessSamplers, rhi::DescriptorType::Sampler,
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
    m_Profiler.Initialize(device, rhi::kMaxProfilerPasses, MAX_FRAMES_IN_FLIGHT);  // GPU Profiler

    // --- SceneRenderer ---
    m_SceneRenderer = std::make_unique<SceneRenderer>();

    // --- Ray Tracing 子系统（可选，硬件不支持时跳过）---
    m_RTEnabled = device->GetCaps().supportsRayTracing;
    if (m_RTEnabled) {
        m_RTPass = std::make_unique<RTPass>();
        // 构建 RT shader 字节码（从嵌入的 SPIR-V 数据加载）
        std::vector<rhi::ShaderBytecode> rtShaders;
        // RT_Shadow.rgen — RayGen
        {
            rhi::ShaderBytecode bc;
            bc.stage      = rhi::ShaderStage::RayGen;
            bc.spirv      = k_RT_Shadow_rgen_spv;
            bc.entryPoint = "main";
            rtShaders.push_back(bc);
        }
        // RT_Common.rmiss — Miss
        {
            rhi::ShaderBytecode bc;
            bc.stage      = rhi::ShaderStage::Miss;
            bc.spirv      = k_RT_Common_rmiss_spv;
            bc.entryPoint = "main";
            rtShaders.push_back(bc);
        }
        // RT_Common.rchit — ClosestHit (index 2 = hit group 0)
        {
            rhi::ShaderBytecode bc;
            bc.stage      = rhi::ShaderStage::ClosestHit;
            bc.spirv      = k_RT_Common_rchit_spv;
            bc.entryPoint = "main";
            rtShaders.push_back(bc);
        }

        // 着色器组: [0]=RayGen, [1]=Miss, [2]=Hit
        std::vector<rhi::RTShaderGroup> groups;
        {
            rhi::RTShaderGroup rg;
            rg.type = rhi::RTShaderGroupType::RayGen;
            rg.generalShader = 0;
            rg.name = "RayGen";
            groups.push_back(rg);
        }
        {
            rhi::RTShaderGroup mg;
            mg.type = rhi::RTShaderGroupType::Miss;
            mg.generalShader = 1;
            mg.name = "Miss";
            groups.push_back(mg);
        }
        {
            rhi::RTShaderGroup hg;
            hg.type = rhi::RTShaderGroupType::Hit;
            hg.closestHitShader = 2;
            hg.name = "Hit";
            groups.push_back(hg);
        }

        if (m_RTPass->Initialize(device, rtShaders, groups)) {
            HE_CORE_INFO("ForwardPipeline: RTPass initialized — RT Shadow enabled");
        } else {
            m_RTEnabled = false;
            HE_CORE_WARN("ForwardPipeline: RTPass init failed, RT disabled");
        }
    }

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
    if (m_RTPass) {
        m_RTPass->Shutdown();
        m_RTPass.reset();
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
    hdrDepthDesc.usage  = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    m_HDRDepth = m_Device->CreateTexture(hdrDepthDesc);
    // ToneMapPass 输入会自动通过每帧 SetInput 更新，无需手动更新描述符集
}

// ---- IRenderPipeline 包装方法 ----

void ForwardPipeline::Render(rhi::IRHICommandList* cmd, he::World& world,
                              he::SceneGraph& sg, const CameraData& camera,
                              float deltaTime)
{
    if (m_UseRenderGraph) {
        RenderGraph rg;
        rg.SetProfiler(&m_Profiler);
        BuildFrameGraph(rg, world, sg, camera);
        rg.Compile();
        rg.Execute(cmd, m_Device);
        return;
    }
    // 非 RG 路径：手动渲染阴影
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

    // ============================================================
    // GPU 视锥剔除（Compute Shader）— 读回上帧结果 → 调度下帧
    // ============================================================
    // 1) 读回上一帧 GPU culling 结果（已 submit 执行完毕）
    if (m_GPUCulling.enabled) {
        m_GPUCulling.Readback(m_Device, m_GPUVisibleIndices);
    }

    // 2) SceneRenderer 准备所有 draw items
    auto allDrawItems = m_SceneRenderer->Prepare(world, sceneGraph, camera,
                                                  m_ObjectBuffers[m_CurrentFrameSlot].get());

    // 3) GPU 剔除：绑定 GPUScene SSBO → Dispatch Compute → 恢复 Graphics PSO
    if (m_GPUCulling.enabled) {
        m_GPUScene.Collect(world, sceneGraph);
        // FillGPUScene 必须在 Collect 之后、Upload 之前（与 Deferred 一致）
        if (!m_BatchBuilt) { m_MeshBatcher.Build(world); m_BatchBuilt = true; }
        m_MeshBatcher.FillGPUScene(m_GPUScene);
        m_GPUScene.Upload(m_Device);
        m_GPUCulling.SetSceneBuffer(m_Device, m_GPUScene.GetObjectBuffer());
        if (m_HDRDepth) m_GPUCulling.SetDepthTexture(m_Device, m_HDRDepth.get(),
                                                       m_HDRWidth, m_HDRHeight);
        m_GPUCulling.Dispatch(cmd, viewProj, m_GPUScene.GetObjectCount(),
                              m_HDRWidth, m_HDRHeight);
        cmd->SetPipeline(m_PBR_PSO.get());
    }

    // GPU 剔除后过滤：构建可见 draw 列表
    std::vector<DrawItem> filteredItems;
    // GPU 剔除时，传统 draw 命令也使用全部对象列表。
    // SceneRenderer 和 GPUScene 的 objectIndex 是独立索引空间，不能直接对号过滤。
    // 实际剔除由后续 DrawIndexedIndirect 根据 GPU IndirectCmdBuf 完成。
    filteredItems = std::move(allDrawItems);
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
                    secCmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSets[m_CurrentFrameSlot]);  // set=0: per-frame + bindless
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
            cmd->DrawIndexedIndirect(m_GPUCulling.GetIndirectBuffer(), 0,
                m_GPUCulling.GetLastVisibleCount(), sizeof(IndirectDrawCommand));
            drawCount = m_GPUCulling.GetLastVisibleCount();
        } else {
            framePC.useInstanceID = 0;  // push constant 模式
            for (auto& di : filteredItems) {
                PushConstantData pc = framePC;
                pc.objectIndex = di.objectIndex;
                cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSets[m_CurrentFrameSlot]);
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

    cmd->SetPushConstants(0, sizeof(PushConstantData), &framePC);
    cmd->SetVertexBuffer(mesh->GetVertexBuffer().get(), 0);
    cmd->SetIndexBuffer(mesh->GetIndexBuffer().get());
    cmd->DrawIndexed(mesh->GetIndexCount());
}

} // namespace he::render
