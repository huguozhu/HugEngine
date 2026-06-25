#include "Render/Pipeline/ForwardPipeline.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "EmbeddedShaders.h"  // 编译后生成的 SPIR-V 数组

namespace he::render {

ForwardPipeline::ForwardPipeline() {
}

ForwardPipeline::~ForwardPipeline() {
    Shutdown();
}

void ForwardPipeline::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    HE_ASSERT(m_Device, "ForwardPipeline: device is null");

    // --- 1. 设置着色器字节码（来自编译生成的 .spv 嵌入文件）---
    m_VS.stage      = rhi::ShaderStage::Vertex;
    m_VS.spirv      = k_PBR_vert_spv;
    m_VS.entryPoint = "main";

    m_FS.stage      = rhi::ShaderStage::Pixel;
    m_FS.spirv      = k_PBR_frag_spv;
    m_FS.entryPoint = "main";

    // --- 2. 顶点布局（匹配 StaticVertex: position + normal + uv）---
    rhi::VertexInputLayout vertexLayout;
    vertexLayout.stride = sizeof(he::StaticVertex);
    vertexLayout.attributes = {
        { 0, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, position) },
        { 1, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, normal) },
        { 2, 0, rhi::VertexFormat::Float2, offsetof(he::StaticVertex, uv) },
    };

    // --- 3. Push constant range（Vertex + Fragment 均可见，使用 Vulkan 位掩码）---
    // VK_SHADER_STAGE_VERTEX_BIT = 1, VK_SHADER_STAGE_FRAGMENT_BIT = 16
    rhi::PushConstantRange pcRange;
    pcRange.stageMask = 1 | 16;  // Vertex (1) + Fragment (16)
    pcRange.offset    = 0;
    pcRange.size      = sizeof(PushConstantData);

    // --- 4. 创建管线状态 ---
    rhi::PipelineStateDesc psoDesc;
    psoDesc.vertexShader        = &m_VS;
    psoDesc.pixelShader         = &m_FS;
    psoDesc.vertexLayout        = vertexLayout;
    psoDesc.topology            = rhi::PrimitiveTopology::TriangleList;
    psoDesc.depthTest           = true;
    psoDesc.depthWrite          = true;
    psoDesc.depthCompare        = rhi::CompareFunc::LessEqual;
    psoDesc.pushConstantRanges  = { pcRange };
    psoDesc.debugName           = "ForwardPBR";

    m_PBR_PSO = device->CreatePipelineState(psoDesc);
    HE_ASSERT(m_PBR_PSO, "ForwardPipeline: failed to create PBR PSO");

    HE_CORE_INFO("ForwardPipeline initialized");
}

void ForwardPipeline::Shutdown() {
    m_PBR_PSO.reset();
    m_Device = nullptr;
    HE_CORE_INFO("ForwardPipeline shut down");
}

void ForwardPipeline::BeginFrame(rhi::IRHICommandList* cmd, u32 width, u32 height) {
    // Vulkan 视口：使用负高度翻转 Y 轴
    // GLM 输出 Y-up NDC，Vulkan 要求 Y-down → 通过负高度翻转
    cmd->SetViewport({
        0, static_cast<float>(height),              // y = height（屏幕底部）
        static_cast<float>(width), -static_cast<float>(height),  // 负高度翻转 Y
        0.0f, 1.0f
    });
    cmd->SetScissor({ 0, 0, width, height });
}

void ForwardPipeline::RenderScene(
    rhi::IRHICommandList* cmd,
    he::World& world,
    he::SceneGraph& sceneGraph,
    const CameraData& camera,
    const PushConstantData& lighting)
{
    // 更新场景图中所有脏变换
    sceneGraph.UpdateTransforms();

    float4x4 viewProj = camera.GetViewProjMatrix();
    u32 drawCount = 0;

    // 遍历所有 MeshComponent，渲染每个网格
    // 渲染单个 MeshComponent 的公共逻辑
    auto renderMesh = [&](he::Entity e, he::MeshComponent& mesh) {
        if (mesh.GetIndexCount() == 0) return;
        float4x4 worldMatrix = sceneGraph.GetWorldMatrix(e);
        PBRMaterial mat = GetDefaultMaterial();
        mat.baseColorFactor = mesh.baseColorFactor;
        mat.metallicFactor  = mesh.metallicFactor;
        mat.roughnessFactor = mesh.roughnessFactor;
        DrawMesh(cmd, &mesh, worldMatrix, viewProj, mat, camera, lighting);
        drawCount++;
    };

    // 遍历 MeshComponent 及其子类（World 按 typeid 分桶，需分别查询）
    world.ForEach<he::MeshComponent>(renderMesh);
    world.ForEach<he::CubeComponent>([&](he::Entity e, he::CubeComponent& c) {
        renderMesh(e, static_cast<he::MeshComponent&>(c));
    });
    world.ForEach<he::SphereComponent>([&](he::Entity e, he::SphereComponent& s) {
        renderMesh(e, static_cast<he::MeshComponent&>(s));
    });

    // 首帧打印场景统计
    static bool s_FirstFrame = true;
    if (s_FirstFrame) {
        HE_CORE_INFO("ForwardPipeline::RenderScene: {} draws, camera at ({:.1f},{:.1f},{:.1f})",
            drawCount, camera.position.x, camera.position.y, camera.position.z);
        HE_CORE_INFO("  viewProj col0: ({:.3f},{:.3f},{:.3f},{:.3f})",
            viewProj[0][0], viewProj[0][1], viewProj[0][2], viewProj[0][3]);
        s_FirstFrame = false;
    }
}

void ForwardPipeline::EndFrame(rhi::IRHICommandList* /*cmd*/) {
    // Phase 2 B1: 暂无需特殊操作（后续添加 GUI / 后处理）
}

PushConstantData ForwardPipeline::MakeDefaultLighting() {
    PushConstantData pc;
    // 默认方向光：从右上方照射，暖白色
    pc.lightDirection   = float4(0.5f, -1.0f, 1.0f, 5.0f);  // xyz=方向, w=强度
    pc.lightColor       = float4(1.0f, 0.95f, 0.85f, 0.0f); // 暖白色
    return pc;
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

    // --- 构建 push constant 数据 ---
    PushConstantData pc = lighting;
    pc.modelMatrix    = worldMatrix;
    pc.viewProjMatrix = viewProjMatrix;
    pc.baseColorFactor  = material.baseColorFactor;
    pc.metallicFactor   = material.metallicFactor;
    pc.roughnessFactor  = material.roughnessFactor;
    pc.aoFactor         = material.aoFactor;
    pc.alphaCutoff      = material.alphaCutoff;
    pc.cameraPosition   = float4(camera.position, 0.0f);

    // 推送常量
    cmd->SetPushConstants(0, sizeof(PushConstantData), &pc);

    // 绑定顶点/索引缓冲（从 unique_ptr 取裸指针）
    cmd->SetVertexBuffer(mesh->GetVertexBuffer().get(), 0);
    cmd->SetIndexBuffer(mesh->GetIndexBuffer().get());

    // 绘制索引几何
    cmd->DrawIndexed(mesh->GetIndexCount());
}

} // namespace he::render
