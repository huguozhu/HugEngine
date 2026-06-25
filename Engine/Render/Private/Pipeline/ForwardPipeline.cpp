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

    // --- 3. 先创建 DescriptorSetLayout（PSO 创建时需要）---
    rhi::DescriptorSetLayoutDesc lightLayoutDesc;
    lightLayoutDesc.bindings = {
        { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Pixel },
    };
    m_LightDescLayout = device->CreateDescriptorSetLayout(lightLayoutDesc);

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
    psoDesc.descriptorSetLayouts = { m_LightDescLayout };
    psoDesc.debugName            = "ForwardPBR";

    m_PBR_PSO = device->CreatePipelineState(psoDesc);
    HE_ASSERT(m_PBR_PSO, "ForwardPipeline: failed to create PBR PSO");

    // 创建 Storage Buffer（每帧填充）
    rhi::BufferDesc lightBufDesc;
    lightBufDesc.size  = sizeof(GPULight) * MAX_LIGHTS;
    lightBufDesc.usage = rhi::BufferUsage::Storage;
    m_LightBuffer = device->CreateBuffer(lightBufDesc);

    // 分配 DescriptorSet 并绑定 Storage Buffer
    m_LightDescSet = device->AllocateDescriptorSet(m_LightDescLayout);
    device->UpdateDescriptorSet(m_LightDescSet, 1, rhi::DescriptorType::StorageBuffer,
                                m_LightBuffer.get());

    HE_CORE_INFO("ForwardPipeline initialized");
}

void ForwardPipeline::Shutdown() {
    if (m_Device) {
        m_Device->DestroyDescriptorSetLayout(m_LightDescLayout);
    }
    m_LightBuffer.reset();
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

    // 填入 Storage Buffer
    world.ForEach<he::LightComponent>([&](he::Entity e, he::LightComponent& lc) {
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

        // 写入 Storage Buffer
        GPULight* lights = static_cast<GPULight*>(m_LightBuffer->Map());
        if (lights) {
            lights[i] = gl;
        }
        m_LightBuffer->Unmap();
        pc.lightCount++;
    });

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

    // 每帧收集光源数据
    PushConstantData lighting{};
    CollectLights(lighting, world, sceneGraph);

    // 绑定光照 DescriptorSet（set=0，binding=1 = GPULight[]）
    cmd->BindDescriptorSet(0, m_LightDescSet);

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

        DrawMesh(cmd, &mesh, worldMatrix, viewProj, mat, camera, lighting);
        drawCount++;
    };

    world.ForEach<he::MeshComponent>(renderMesh);
    world.ForEach<he::CubeComponent>([&](he::Entity e, he::CubeComponent& c) {
        renderMesh(e, static_cast<he::MeshComponent&>(c));
    });
    world.ForEach<he::SphereComponent>([&](he::Entity e, he::SphereComponent& s) {
        renderMesh(e, static_cast<he::MeshComponent&>(s));
    });

    static bool s_FirstFrame = true;
    if (s_FirstFrame) {
        HE_CORE_INFO("ForwardPipeline::RenderScene: {} draws, {} lights",
            drawCount, lighting.lightCount);
        s_FirstFrame = false;
    }
}

void ForwardPipeline::EndFrame(rhi::IRHICommandList* /*cmd*/) {
}

void ForwardPipeline::DrawMesh(
    rhi::IRHICommandList* cmd,
    he::MeshComponent* mesh,
    const float4x4& worldMatrix,
    const float4x4& viewProjMatrix,
    const PBRMaterial& material,
    const CameraData& camera,
    const PushConstantData& lighting)
{
    if (!mesh || mesh->GetIndexCount() == 0) return;

    PushConstantData pc = lighting;
    pc.modelMatrix    = worldMatrix;
    pc.viewProjMatrix = viewProjMatrix;
    FillMaterialPushConstants(pc, material);
    pc.cameraPosition = float4(camera.position, 0.0f);

    cmd->SetPushConstants(0, sizeof(PushConstantData), &pc);
    cmd->SetVertexBuffer(mesh->GetVertexBuffer().get(), 0);
    cmd->SetIndexBuffer(mesh->GetIndexBuffer().get());
    cmd->DrawIndexed(mesh->GetIndexCount());
}

} // namespace he::render
