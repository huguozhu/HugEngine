#include "Render/Pipeline/ForwardPipeline.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "EmbeddedShaders.h"

namespace he::render {

ForwardPipeline::ForwardPipeline() {
}

ForwardPipeline::~ForwardPipeline() {
    Shutdown();
}

void ForwardPipeline::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    HE_ASSERT(m_Device, "ForwardPipeline: device is null");

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

    // --- 3. 创建 DescriptorSetLayout ---
    // Descriptor set layout: 只包含 shader 实际使用的 binding
    // binding=1 (Light SSBO, Fragment)
    // binding=2 (Object SSBO, Vertex+Fragment)
    rhi::DescriptorSetLayoutDesc combinedLayoutDesc;
    combinedLayoutDesc.bindings = {
        { 1, rhi::DescriptorType::StorageBuffer, 1, 16 },     // stageMask=16 (Fragment)
        { 2, rhi::DescriptorType::StorageBuffer, 1, 17 },     // stageMask=17 (Vertex|Fragment)
    };
    m_DescLayout = device->CreateDescriptorSetLayout(combinedLayoutDesc);

    // 创建对象 Storage Buffer（每帧填充）
    rhi::BufferDesc objBufDesc;
    objBufDesc.size  = sizeof(GPUObjectData) * MAX_OBJECTS;
    objBufDesc.usage = rhi::BufferUsage::Storage;
    m_ObjectBuffer = device->CreateBuffer(objBufDesc);

    // --- 4. Push constant + PSO ---
    rhi::PushConstantRange pcRange;
    pcRange.stageMask = 1 | 16;
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

    // 创建 Light Storage Buffer
    rhi::BufferDesc lightBufDesc;
    lightBufDesc.size  = sizeof(GPULight) * MAX_LIGHTS;
    lightBufDesc.usage = rhi::BufferUsage::Storage;
    m_LightBuffer = device->CreateBuffer(lightBufDesc);

    // 分配 DescriptorSet 并绑定两个 Storage Buffer
    m_DescSet = device->AllocateDescriptorSet(m_DescLayout);
    device->UpdateDescriptorSet(m_DescSet, 1, rhi::DescriptorType::StorageBuffer,
                                m_LightBuffer.get());
    device->UpdateDescriptorSet(m_DescSet, 2, rhi::DescriptorType::StorageBuffer,
                                m_ObjectBuffer.get());

    HE_CORE_INFO("ForwardPipeline initialized");
}

void ForwardPipeline::Shutdown() {
    if (m_Device) {
        m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    }
    m_LightBuffer.reset();
    m_ObjectBuffer.reset();
    m_PBR_PSO.reset();
    m_Device = nullptr;
    HE_CORE_INFO("ForwardPipeline shut down");
}

void ForwardPipeline::BeginFrame(rhi::IRHICommandList* cmd, u32 width, u32 height) {
    cmd->SetViewport({
        0, static_cast<float>(height),
        static_cast<float>(width), -static_cast<float>(height),
        0.0f, 1.0f
    });
    cmd->SetScissor({ 0, 0, width, height });
}

void ForwardPipeline::CollectLights(PushConstantData& pc, he::World& world, he::SceneGraph& sg) {
    pc.lightCount = 0;

    // 填入 Storage Buffer（需遍历三种光源子类，World 按 type_index 分桶存储）
    auto collectLight = [&](he::Entity e, he::LightComponent& lc) {
        if (pc.lightCount >= MAX_LIGHTS) return;
        u32 i = pc.lightCount;

        GPULight gl{};
        gl.colorIntensity  = float4(lc.color, lc.intensity);

        switch (lc.type) {
        case he::LightType::Directional: {
            auto* dl = static_cast<he::DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f);  // type=0
            gl.positionRange = float4(0, 0, 0, 0);
            break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            gl.positionRange = float4(pos, pl->range);
            gl.directionType = float4(0, -1, 0, 1.0f);  // type=1
            break;
        }
        case he::LightType::Spot: {
            auto* sl = static_cast<he::SpotLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            gl.positionRange = float4(pos, sl->range);
            gl.directionType = float4(sl->direction, 2.0f);  // type=2
            gl.coneAngles   = float2(sl->innerConeAngle, sl->outerConeAngle);
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
        GPULight* lights = static_cast<GPULight*>(m_LightBuffer->Map());
        if (lights) lights[0] = gl;
        m_LightBuffer->Unmap();
    }
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

    // 帧级 push constant（viewProj + camera + lightCount）
    PushConstantData framePC{};
    framePC.viewProjMatrix = viewProj;
    framePC.cameraPosition = float4(camera.position, 0.0f);

    // 每帧收集光源数据
    CollectLights(framePC, world, sceneGraph);

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

    // 绑定 DescriptorSet（光照 + 对象数据）
    cmd->BindDescriptorSet(0, m_DescSet);

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
        HE_CORE_INFO("ForwardPipeline::RenderScene: {} draws, {} lights",
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

    // 帧级 push constant: viewProj + camera + lightCount + objectIndex
    cmd->SetPushConstants(0, sizeof(PushConstantData), &framePC);
    cmd->SetVertexBuffer(mesh->GetVertexBuffer().get(), 0);
    cmd->SetIndexBuffer(mesh->GetIndexBuffer().get());
    cmd->DrawIndexed(mesh->GetIndexCount());
}

} // namespace he::render
