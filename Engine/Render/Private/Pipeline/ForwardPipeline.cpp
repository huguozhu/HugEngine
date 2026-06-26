#include "Render/Pipeline/ForwardPipeline.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "EmbeddedShaders.h"

#include <glm/gtc/matrix_transform.hpp>

namespace he::render {

ForwardPipeline::ForwardPipeline() {
}

ForwardPipeline::~ForwardPipeline() {
    Shutdown();
}

// 计算方向光的正交投影矩阵（用于阴影贴图）
static float4x4 ComputeDirectionalLightViewProj(
    const float3& lightDir, const CameraData& camera, float size)
{
    // 将灯光视为无穷远，构建正交投影
    // 使用相机视锥体的中心作为阴影投影的参考点
    float3 camForward = camera.forward;
    float3 camCenter = camera.position + camForward * 20.0f; // 阴影中心在相机前方

    // 灯光上方向量（避免与 lightDir 共线）
    float3 up = (abs(lightDir.y) < 0.999f) ? float3(0, 1, 0) : float3(1, 0, 0);

    float4x4 lightView = glm::lookAt(
        camCenter - lightDir * size * 0.5f,  // 灯光位置（远离场景中心）
        camCenter,
        up);

    // 正交投影包围盒
    float halfSize = size * 0.5f;
    float nearPlane = 0.1f;
    float farPlane  = size * 2.0f;
    float4x4 lightProj = glm::ortho(-halfSize, halfSize, -halfSize, halfSize, nearPlane, farPlane);

    // Vulkan NDC: 反转 Y 轴
    lightProj[1][1] *= -1.0f;

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
        { 4, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — 阴影贴图
        { 5, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — 基础色
        { 6, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — 法线
        { 7, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — 金属度/粗糙度
        { 8, rhi::DescriptorType::CombinedImageSampler,  1, 16 },  // Fragment — AO
    };
    m_DescLayout = device->CreateDescriptorSetLayout(combinedLayoutDesc);

    // --- 创建 Storage Buffers ---
    rhi::BufferDesc objBufDesc;
    objBufDesc.size  = sizeof(GPUObjectData) * MAX_OBJECTS;
    objBufDesc.usage = rhi::BufferUsage::Storage;
    m_ObjectBuffer = device->CreateBuffer(objBufDesc);

    rhi::BufferDesc lightBufDesc;
    lightBufDesc.size  = sizeof(GPULight) * MAX_LIGHTS;
    lightBufDesc.usage = rhi::BufferUsage::Storage;
    m_LightBuffer = device->CreateBuffer(lightBufDesc);

    rhi::BufferDesc shadowBufDesc;
    shadowBufDesc.size  = sizeof(GPUShadowData) * MAX_SHADOWS;
    shadowBufDesc.usage = rhi::BufferUsage::Storage;
    m_ShadowBuffer = device->CreateBuffer(shadowBufDesc);

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

    // --- 创建 PCF 比较采样器 ---
    rhi::SamplerDesc shadowSamplerDesc;
    shadowSamplerDesc.minFilter     = rhi::FilterMode::Linear;
    shadowSamplerDesc.magFilter     = rhi::FilterMode::Linear;
    shadowSamplerDesc.addressU      = rhi::AddressMode::ClampToEdge;
    shadowSamplerDesc.addressV      = rhi::AddressMode::ClampToEdge;
    shadowSamplerDesc.addressW      = rhi::AddressMode::ClampToEdge;
    shadowSamplerDesc.enableCompare = true;               // 启用深度比较
    shadowSamplerDesc.compareFunc   = rhi::CompareFunc::LessEqual;
    m_ShadowSampler = device->CreateSampler(shadowSamplerDesc);

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

        // 阴影 PCF 比较采样器（后续阴影贴图就绪后启用）
        rhi::SamplerDesc shadowSampDesc;
        shadowSampDesc.minFilter     = rhi::FilterMode::Linear;
        shadowSampDesc.magFilter     = rhi::FilterMode::Linear;
        shadowSampDesc.addressU      = rhi::AddressMode::ClampToEdge;
        shadowSampDesc.addressV      = rhi::AddressMode::ClampToEdge;
        shadowSampDesc.enableCompare = true;
        shadowSampDesc.compareFunc   = rhi::CompareFunc::LessEqual;
        m_ShadowSampler = device->CreateSampler(shadowSampDesc);
    }

    // --- 分配描述符集并绑定所有资源 ---
    // 注：binding 4 复用 DefaultBaseColorTex（阴影贴图渲染通道就绪后替换为深度纹理）
    m_DescSet = device->AllocateDescriptorSet(m_DescLayout);
    device->UpdateDescriptorSet(m_DescSet, 1, rhi::DescriptorType::StorageBuffer,
                                m_LightBuffer.get());
    device->UpdateDescriptorSet(m_DescSet, 2, rhi::DescriptorType::StorageBuffer,
                                m_ObjectBuffer.get());
    device->UpdateDescriptorSet(m_DescSet, 3, rhi::DescriptorType::StorageBuffer,
                                m_ShadowBuffer.get());
    device->UpdateDescriptorSet(m_DescSet, 4,
                                rhi::DescriptorType::CombinedImageSampler,
                                m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());
    device->UpdateDescriptorSet(m_DescSet, 5,
                                rhi::DescriptorType::CombinedImageSampler,
                                m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());
    // binding 6-8: 复用默认纹理（运行时通过 SetXxxTexture 替换）
    device->UpdateDescriptorSet(m_DescSet, 6,
                                rhi::DescriptorType::CombinedImageSampler,
                                m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());
    device->UpdateDescriptorSet(m_DescSet, 7,
                                rhi::DescriptorType::CombinedImageSampler,
                                m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());
    device->UpdateDescriptorSet(m_DescSet, 8,
                                rhi::DescriptorType::CombinedImageSampler,
                                m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());

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
    psoDesc.pushConstantRanges   = { pcRange };
    psoDesc.descriptorSetLayouts = { m_DescLayout };
    psoDesc.debugName            = "ForwardPBR";

    m_PBR_PSO = device->CreatePipelineState(psoDesc);
    HE_ASSERT(m_PBR_PSO, "ForwardPipeline: failed to create PBR PSO");

    // --- 阴影 PSO（深度专用：无颜色附件，仅深度写入）---
    rhi::PushConstantRange shadowPCRange;
    shadowPCRange.stageMask = 1;    // Vertex only
    shadowPCRange.offset    = 0;
    shadowPCRange.size      = sizeof(ShadowPushConstant);

    rhi::PipelineStateDesc shadowPSODesc;
    shadowPSODesc.vertexShader         = &m_ShadowVS;
    shadowPSODesc.pixelShader          = &m_ShadowFS;
    shadowPSODesc.vertexLayout         = vertexLayout;
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

    // TODO Phase 4A-2: 阴影贴图布局转换（UNDEFINED → SHADER_READ_ONLY_OPTIMAL）
    // 需在 RHI 层添加 Image Barrier 支持后再实现

    HE_CORE_INFO("ForwardPipeline initialized (with Shadow Mapping)");
}

void ForwardPipeline::Shutdown() {
    if (m_Device) {
        m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    }
    m_LightBuffer.reset();
    m_ObjectBuffer.reset();
    m_ShadowBuffer.reset();
    m_ShadowMap.reset();
    m_ShadowSampler.reset();
    m_PBR_PSO.reset();
    m_ShadowPSO.reset();
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

    // 复制共享绑定 1-4（灯光/对象/阴影数据/阴影贴图）
    m_Device->UpdateDescriptorSet(set, 1, rhi::DescriptorType::StorageBuffer, m_LightBuffer.get());
    m_Device->UpdateDescriptorSet(set, 2, rhi::DescriptorType::StorageBuffer, m_ObjectBuffer.get());
    m_Device->UpdateDescriptorSet(set, 3, rhi::DescriptorType::StorageBuffer, m_ShadowBuffer.get());
    m_Device->UpdateDescriptorSet(set, 4, rhi::DescriptorType::CombinedImageSampler,
        m_DefaultBaseColorTex.get(), m_DefaultBaseColorSampler.get());

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

    return set;
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
    he::SceneGraph& sg)
{
    pc.lightCount = 0;
    shadowData.clear();

    auto collectLight = [&](he::Entity e, he::LightComponent& lc) {
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
                // 使用一个虚拟相机来计算方向光阴影的包围区域
                // 简化版：基于光源方向计算正交投影
                float3 lightDir = glm::normalize(dl->direction);
                float sceneSize = 30.0f; // 固定场景大小（后续可根据实际场景调整）

                GPUShadowData sd{};
                sd.lightViewProj = ComputeDirectionalLightViewProj(
                    lightDir, CameraData{}, sceneSize);
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

        GPULight* lights = static_cast<GPULight*>(m_LightBuffer->Map());
        if (lights) lights[i] = gl;
        m_LightBuffer->Unmap();
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
        GPULight* lights = static_cast<GPULight*>(m_LightBuffer->Map());
        if (lights) lights[0] = gl;
        m_LightBuffer->Unmap();
    }

    // 上传阴影 GPU 数据到 SSBO
    if (!shadowData.empty()) {
        GPUShadowData* sd = static_cast<GPUShadowData*>(m_ShadowBuffer->Map());
        for (usize j = 0; j < shadowData.size(); ++j) {
            sd[j] = shadowData[j];
        }
        m_ShadowBuffer->Unmap();
    }
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
    cmd->BindDescriptorSet(0, m_DescSet);

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

    auto renderMeshForShadow = [&](he::Entity e, he::MeshComponent& mesh) {
        if (mesh.GetIndexCount() == 0) return;
        shadowPC.objectIndex = 0;  // 简化：每帧为阴影通道单独上传对象
        // 获取世界矩阵
        float4x4 worldMatrix = sceneGraph.GetWorldMatrix(e);

        // 临时上传此对象到 Object Buffer（阴影通道需读取世界矩阵）
        // 注意：阴影着色器从 u_Objects[objectIndex].worldMatrix 读取
        GPUObjectData* objData = static_cast<GPUObjectData*>(m_ObjectBuffer->Map());
        objData[0].worldMatrix = worldMatrix;
        m_ObjectBuffer->Unmap();

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
    CollectLights(framePC, shadowGPUData, world, sceneGraph);

    // ============================================================
    // 阴影通道
    // ============================================================
    // 注意：阴影通道需要在一个渲染通道内执行，此处利用 VulkanCommandList 的
    // BeginRenderPass 机制。但是阴影 PSO 有自己独立的渲染通道（无颜色附件），
    // 因此阴影通道需要在 BeginRenderPass 之前通过 ShadowPSO 的 renderPass 渲染。

    // 当前简化实现：阴影通道使用 ShadowPSO 的 renderPass
    // 需要先结束主管道的 renderPass（由调用方保证已在 BeginRenderPass 内）

    // 上传 per-object 数据到 Storage Buffer
    GPUObjectData* objData = static_cast<GPUObjectData*>(m_ObjectBuffer->Map());
    u32 objectIndex = 0;

    auto uploadObject = [&](he::Entity e, he::MeshComponent& mesh, float4x4& wm, PBRMaterial& mat) {
        if (objectIndex >= MAX_OBJECTS) return;
        GPUObjectData& obj = objData[objectIndex];
        obj.worldMatrix = wm;
        FillObjectData(obj, mat);
        objectIndex++;
    };

    auto renderMesh = [&](he::Entity e, he::MeshComponent& mesh) {
        if (mesh.GetIndexCount() == 0) return;
        float4x4 worldMatrix = sceneGraph.GetWorldMatrix(e);

        PBRMaterial mat = GetDefaultMaterial();
        mat.baseColorFactor = mesh.baseColorFactor;
        mat.emissiveFactor  = mesh.emissiveFactor;
        mat.metallicFactor  = mesh.metallicFactor;
        mat.roughnessFactor = mesh.roughnessFactor;
        mat.aoFactor        = mesh.aoFactor;
        mat.alphaCutoff     = mesh.alphaCutoff;
        mat.alphaMode       = static_cast<AlphaMode>(mesh.alphaMode);
        mat.doubleSided     = mesh.doubleSided;
        mat.unlit           = mesh.unlit;

        uploadObject(e, mesh, worldMatrix, mat);
        framePC.objectIndex = objectIndex - 1;

        // 绑定该 mesh 的独立描述符集（纹理已在加载时写入，渲染时只 bind 不 update）
        if (mesh.GetDescriptorSet() != rhi::kInvalidSet)
            cmd->BindDescriptorSet(0, mesh.GetDescriptorSet());
        else
            cmd->BindDescriptorSet(0, m_DescSet);

        DrawMesh(cmd, &mesh, worldMatrix, viewProj, mat, camera, framePC);
        drawCount++;
    };

    world.ForEach<he::MeshComponent>(renderMesh);
    world.ForEach<he::CubeComponent>([&](he::Entity e, he::CubeComponent& c) {
        renderMesh(e, static_cast<he::MeshComponent&>(c));
    });
    world.ForEach<he::SphereComponent>([&](he::Entity e, he::SphereComponent& s) {
        renderMesh(e, static_cast<he::MeshComponent&>(s));
    });

    m_ObjectBuffer->Unmap();

    static bool s_FirstFrame = true;
    if (s_FirstFrame) {
        HE_CORE_INFO("ForwardPipeline::RenderScene: {} draws, {} lights, {} shadows",
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
