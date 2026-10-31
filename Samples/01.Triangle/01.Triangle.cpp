// ============================================================
// 01.Triangle — HugEngine RHI 渲染三角形（光栅化 + Ray Tracing + Mesh Shader）
//
// 光栅化：   传统 VS→PS 管线，纯红色三角形
// RT：       RayGen 着色器过程化红色三角形 → StorageImage
// Mesh Shader：无 VB/IB，MS→PS 硬编码三色渐变三角形
// ImGui：    切换渲染模式
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "RHI/FramePresenter.h"   // L4-1：帧外壳（acquire/present）收口
#include "Editor/ImGuiIntegration.h"
#include "Triangle.vert.spv.h"
#include "Triangle.vert.dxil.h"
#include "Triangle.frag.spv.h"
#include "Triangle.frag.dxil.h"
#include "RT_Triangle.rgen.spv.h"
#include "RT_Triangle.rgen.dxil.h"
#include "RT_Background.rmiss.spv.h"
#include "RT_Background.rmiss.dxil.h"
#include "Triangle.mesh.spv.h"
#include "Triangle.mesh.dxil.h"
#include "TriangleMesh.frag.spv.h"
#include "TriangleMesh.frag.dxil.h"
#include "imgui.h"

#include <iterator>   // std::begin / std::end（填 dxil 字节码用）
#include <cstring>

using namespace he;

int main() {
    // ============================================================
    // 1. 引擎启动 + 设备创建
    // ============================================================
    he::EngineConfig config;
    config.appName      = "HugEngine — 01.Triangle";
    config.windowWidth  = 960;
    config.windowHeight = 540;

    he::Engine engine(config);
    engine.Initialize();

    he::rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = he::rhi::ResolveBackendFromEnv();
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    auto device = he::rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    he::rhi::SetDevice(device.get());

    bool rtSupported   = device->GetCaps().supportsRayTracing;
    bool meshSupported = device->GetCaps().supportsMeshShaders;
    // 【P2】光追能力现在按硬件默认上报。但本样例的 RT 模式是**直接把后缓冲当存储图像（UAV）写**：
    //   · Vulkan 交换链图像带 VK_IMAGE_USAGE_STORAGE_BIT ⇒ 可以；
    //   · D3D12 的 FLIP 后缓冲由 DXGI 创建，资源标志只有 ALLOW_RENDER_TARGET，转 UNORDERED_ACCESS
    //     会被调试层判为 id=524（缺 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS，且该标志无法补）。
    // 故"能进 RT 模式"要同时满足硬件支持光追 ∧ 后缓冲可作存储图像，二者缺一就走光栅化路径，
    // 不放任它去撞资源标志检查（改前不设 HE_D3D12_RT 时 rtSupported 恒假，这条路径根本没暴露过）。
    const bool rtRenderable = rtSupported && device->GetCaps().supportsBackBufferStorageImage;
    HE_CORE_INFO("RT 支持: {}, Mesh Shader 支持: {}（RT 写后缓冲={}）", rtSupported ? "是" : "否",
                 meshSupported ? "是" : "否", rtRenderable ? "可" : "不可");

    // ============================================================
    // 2. SwapChain
    // ============================================================
    u32 swWidth  = engine.GetWindow()->GetWidth();
    u32 swHeight = engine.GetWindow()->GetHeight();
    auto swapchain = device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width  = swWidth, .height = swHeight, .vsync = true,
    });

    // ============================================================
    // 3. 光栅化路径
    // ============================================================
    struct Vertex { float x, y; };
    const Vertex kVertices[] = {
        { 0.0f, -0.5f }, { 0.5f, 0.5f }, {-0.5f, 0.5f },
    };
    auto vertexBuffer = device->CreateBuffer({
        .size = sizeof(kVertices), .usage = he::rhi::BufferUsage::Vertex,
        .initialData = kVertices, .stride = sizeof(Vertex),
    });

    he::rhi::ShaderBytecode vertShader = he::rhi::MakeShaderBytecode(he::rhi::ShaderStage::Vertex, k_Triangle_vert_spv, k_Triangle_vert_dxil, "main");
    he::rhi::ShaderBytecode fragShader = he::rhi::MakeShaderBytecode(he::rhi::ShaderStage::Pixel, k_Triangle_frag_spv, k_Triangle_frag_dxil, "main");

    auto pipeline = device->CreatePipelineState({
        .vertexShader = &vertShader, .pixelShader = &fragShader,
        .vertexLayout = {.stride = sizeof(Vertex)},
        .depthTest = false, .depthWrite = false,
        // 颜色附件必须是后缓冲的真实格式（D3D12 交换链为 BGRA8，默认值 RGBA8 会让绘制被丢弃）
        .colorFormats = { swapchain->GetColorFormat() },
        .debugName = "Triangle",
    });

    // ============================================================
    // 4. RT 路径（直接渲染到 SwapChain BackBuffer，无中间纹理）
    // ============================================================
    std::unique_ptr<he::rhi::IRHIRayTracingPipelineState> rtPSO;
    std::unique_ptr<he::rhi::IRHIBuffer> rtSBTBuf;
    he::rhi::SBTDesc sbt;
    rhi::DescriptorSetLayoutHandle rtLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       rtSet    = rhi::kInvalidSet;

    if (rtSupported) {
        he::rhi::ShaderBytecode rgen = he::rhi::MakeShaderBytecode(he::rhi::ShaderStage::RayGen, k_RT_Triangle_rgen_spv, k_RT_Triangle_rgen_dxil, "main");
        he::rhi::ShaderBytecode rmiss = he::rhi::MakeShaderBytecode(he::rhi::ShaderStage::Miss, k_RT_Background_rmiss_spv, k_RT_Background_rmiss_dxil, "main");

        rhi::DescriptorSetLayoutDesc ldesc;
        ldesc.bindings = {{ 0, rhi::DescriptorType::StorageImage, 1, 0x100 }};
        rtLayout = device->CreateDescriptorSetLayout(ldesc);
        rtSet = device->AllocateDescriptorSet(rtLayout);

        std::vector<he::rhi::RTShaderGroup> groups = {
            {he::rhi::RTShaderGroupType::RayGen, 0, ~0u, ~0u, ~0u, "RayGen"},
            {he::rhi::RTShaderGroupType::Miss,   1, ~0u, ~0u, ~0u, "Miss"},
        };
        he::rhi::RTPipelineStateDesc rtpDesc;
        rtpDesc.shaders      = { rgen, rmiss };
        rtpDesc.shaderGroups = groups;
        rtpDesc.maxRecursionDepth = 1;
        rtpDesc.descriptorSetLayouts = { rtLayout };
        rtpDesc.debugName = "RT_Triangle";

        rtPSO = device->CreateRTPipelineState(rtpDesc);
        // 光追是**可选**路径：D3D12 后端的 RTPipelineState 属阶段 4，尚未实现。
        // 这里失败不应当中断整个样例 —— 图形路径（CreatePipelineState + Draw(3)）
        // 才是阶段 0/1 的验收目标。
        if (!rtPSO) {
            HE_CORE_WARN("01.Triangle: RT PSO 不可用（当前后端未实现光追），跳过光追路径");
        }

        // SBT —— 三条硬性要求（与引擎 RTPass::BuildSBT 的写法保持一致）：
        //   1) 承载缓冲必须带 SHADER_BINDING_TABLE 用途，否则 deviceAddress 找不到有效缓冲
        //      （VUID-vkCmdTraceRaysKHR-pRayGenShaderBindingTable-03681 / -pMissShaderBindingTable-03684）；
        //   2) 每个区域的起始地址必须按 shaderGroupBaseAlignment 对齐（本机 64B），
        //      否则报 VUID-...-pMissShaderBindingTable-03685；
        //   3) 组与组之间的步长同样要按该对齐向上取整，不能直接用 handleSize（本机 32B）。
        u32 groupCount = rtPSO->GetShaderGroupCount();
        u32 handleSize = rtPSO->GetShaderGroupHandleSize();
        auto handles   = rtPSO->GetShaderGroupHandles();
        const u32 align  = device->GetCaps().shaderGroupBaseAlignment
                         ? device->GetCaps().shaderGroupBaseAlignment : 1u;
        const u32 stride = (handleSize + align - 1u) & ~(align - 1u);
        const u32 sbtSize = stride * groupCount;

        rtSBTBuf = device->CreateBuffer({
            .size = sbtSize,
            .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Uniform
                   | rhi::BufferUsage::ShaderBindingTable,
        });
        u8* mapped = static_cast<u8*>(rtSBTBuf->Map());
        for (u32 g = 0; g < groupCount; ++g)
            std::memcpy(mapped + g * stride, handles.data() + g * handleSize, handleSize);
        rtSBTBuf->Unmap();

        sbt.buffer = rtSBTBuf.get();
        sbt.rayGen.handleOffset = 0;
        sbt.rayGen.stride = stride;
        sbt.miss.handleOffset   = stride;
        sbt.miss.stride   = stride;

        HE_CORE_INFO("RT 管线就绪: {} groups, {}B SBT (直接渲染到 BackBuffer)", groupCount, sbtSize);
    }

    // ============================================================
    // 5. Mesh Shader 路径（无 VB/IB，硬编码三色三角形）
    // ============================================================
    std::unique_ptr<he::rhi::IRHIPipelineState> meshPSO;

    if (meshSupported) {
        he::rhi::ShaderBytecode meshShader = he::rhi::MakeShaderBytecode(he::rhi::ShaderStage::Mesh, k_Triangle_mesh_spv, k_Triangle_mesh_dxil, "main");
        he::rhi::ShaderBytecode meshFrag = he::rhi::MakeShaderBytecode(he::rhi::ShaderStage::Pixel, k_TriangleMesh_frag_spv, k_TriangleMesh_frag_dxil, "main");

        meshPSO = device->CreatePipelineState({
            .pixelShader = &meshFrag,
            .meshShader  = &meshShader,
            .depthTest   = false,
            .depthWrite  = false,
            .colorFormats = { swapchain->GetColorFormat() },   // 同上：与后缓冲格式一致
            .debugName   = "TriangleMesh",
        });

        HE_CORE_INFO("Mesh Shader 管线就绪: DrawMeshTasks(1,1,1)");
    }

    // ============================================================
    // 6. ImGui 集成
    // ============================================================
    he::editor::ImGuiIntegration imgui;
    imgui.Initialize(engine.GetWindow()->GetNativeHandle(), device.get(), swapchain.get());

    // ============================================================
    // 7. 命令列表
    // ============================================================
    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());

    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
    });

    // ============================================================
    // 8. 主渲染循环
    // ============================================================
    int renderMode = rtRenderable ? 1 : 0;  // 0=光栅化, 1=RT, 2=Mesh Shader
    HE_CORE_INFO("01.Triangle started — ImGui 切换渲染模式");
    u64 frameIndex = 0;

    // L4-1：帧外壳的 acquire/present 收口（L4-3 把整帧变成命令流时只改这里）
    rhi::FramePresenter framePresenter(swapchain.get());

    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();
        if (!framePresenter.BeginFrame()) continue;

        u32 w = swapchain->GetWidth();
        u32 h = swapchain->GetHeight();

        framePresenter.BeginRecording(device.get(), cmdList.get());   // L4-2

        // ── RT 调度 + 直接渲染到 BackBuffer ──
        if (renderMode == 1 && rtRenderable && rtPSO) {
            // 将 BackBuffer ImageView 绑定为 RT 输出（StorageImage）
            void* backBufferView = swapchain->GetCurrentBackBufferView();
            device->UpdateDescriptorSetWithImageView(rtSet, 0,
                rhi::DescriptorType::StorageImage, backBufferView);

            // 全局屏障：确保 BackBuffer 可被 RT 写入
            cmdList->PipelineBarrier(rhi::PipelineStage::BottomOfPipe,
                                     rhi::PipelineStage::RayTracingShader,
                                     rhi::ResourceState::Undefined,
                                     rhi::ResourceState::UnorderedAccess);

            cmdList->BindRTPipeline(rtPSO.get());
            cmdList->BindDescriptorSet(0, rtSet);
            cmdList->TraceRays(sbt, w, h, 1);

            // 全局屏障：等待 RT 写入完成
            cmdList->PipelineBarrier(rhi::PipelineStage::RayTracingShader,
                                     rhi::PipelineStage::ColorAttachmentOutput,
                                     rhi::ResourceState::UnorderedAccess,
                                     rhi::ResourceState::RenderTarget);

            // SetPipeline 设置 m_CurrentRenderPass（BeginRenderPass 必需）
            cmdList->SetPipeline(pipeline.get());

            // RenderPass (LoadOp::Load 保留 BackBuffer 上的 RT 输出)
            cmdList->BeginRenderPass(1, swapchain->GetColorFormat(), he::rhi::Format::Unknown,
                                     nullptr, rhi::LoadOp::Load);
        } else if (renderMode == 2 && meshSupported && meshPSO) {
            // ── Mesh Shader 模式：三色渐变三角形，无 VB/IB ──
            cmdList->SetPipeline(meshPSO.get());
            cmdList->BeginRenderPass(1, swapchain->GetColorFormat());
            cmdList->SetViewport({0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f});
            cmdList->SetScissor({0, 0, w, h});
            cmdList->DrawMeshTasks(1, 1, 1);  // 1 个工作组，3 个线程输出 1 个三角形
        } else {
            // 光栅化模式：红色三角形
            cmdList->SetPipeline(pipeline.get());
            cmdList->BeginRenderPass(1, swapchain->GetColorFormat());
            cmdList->SetViewport({0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f});
            cmdList->SetScissor({0, 0, w, h});
            cmdList->SetVertexBuffer(vertexBuffer.get(), 0);
            cmdList->Draw(3);
        }

        // ── ImGui（在主 RP 内渲染，覆盖在场景之上）──
        if (imgui.IsInitialized()) imgui.BeginFrame();   // 非 Vulkan 后端没有 UI
        if (imgui.IsInitialized()) {
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(260, 160), ImGuiCond_FirstUseEver);
            ImGui::Begin("渲染模式");

            ImGui::RadioButton("光栅化 (VS→PS)", &renderMode, 0);
            if (rtRenderable) {
                ImGui::RadioButton("Ray Tracing (RayGen)", &renderMode, 1);
            } else {
                ImGui::BeginDisabled();
                // 区分「硬件没有光追」与「本后端后缓冲不能当 UAV 写」两种情况，避免误导
                ImGui::RadioButton(rtSupported ? "Ray Tracing (后缓冲不可写)" : "Ray Tracing (不支持)",
                                   &renderMode, 1);
                ImGui::EndDisabled();
            }
            if (meshSupported) {
                ImGui::RadioButton("Mesh Shader (MS→PS)", &renderMode, 2);
            } else {
                ImGui::BeginDisabled();
                ImGui::RadioButton("Mesh Shader (不支持)", &renderMode, 2);
                ImGui::EndDisabled();
            }

            ImGui::Separator();
            const char* modeNames[] = {"光栅化", "Ray Tracing", "Mesh Shader"};
            ImGui::Text("当前: %s", modeNames[renderMode]);
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::End();
        }
        if (imgui.IsInitialized()) imgui.EndFrame(cmdList.get());

        cmdList->EndRenderPass();
        framePresenter.EndRecording();   // L4-2：End + Submit 收口
        framePresenter.EndFrame(true);
        frameIndex++;
    }

    // ============================================================
    // 9. 清理
    // ============================================================
    device->WaitIdle();
    imgui.Shutdown();
    if (rtLayout != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(rtLayout);

    HE_CORE_INFO("Exiting after {} frames", frameIndex);
    return 0;
}
