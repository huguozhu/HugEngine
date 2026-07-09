// ============================================================
// 01.Triangle — HugEngine RHI 渲染三角形（光栅化 + Ray Tracing）
//
// 光栅化：传统 VS→PS 管线，红色三角形
// RT：    RayGen 着色器过程化红色三角形 → StorageImage
// ImGui： 切换渲染模式
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Editor/ImGuiIntegration.h"
#include "EmbeddedShaders.h"
#include "RT_Triangle.rgen.spv.h"
#include "RT_Background.rmiss.spv.h"
#include "imgui.h"

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
    rhiDesc.backend          = he::rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    auto device = he::rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    he::rhi::SetDevice(device.get());

    bool rtSupported = device->GetCaps().supportsRayTracing;
    HE_CORE_INFO("RT 支持: {}", rtSupported ? "是" : "否");

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

    he::rhi::ShaderBytecode vertShader{he::rhi::ShaderStage::Vertex, k_Triangle_vert_spv, {}, "main"};
    he::rhi::ShaderBytecode fragShader{he::rhi::ShaderStage::Pixel,  k_Triangle_frag_spv, {}, "main"};

    auto pipeline = device->CreatePipelineState({
        .vertexShader = &vertShader, .pixelShader = &fragShader,
        .vertexLayout = {.stride = sizeof(Vertex)},
        .depthTest = false, .depthWrite = false, .debugName = "Triangle",
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
        he::rhi::ShaderBytecode rgen{he::rhi::ShaderStage::RayGen, k_RT_Triangle_rgen_spv, {}, "main"};
        he::rhi::ShaderBytecode rmiss{he::rhi::ShaderStage::Miss,  k_RT_Background_rmiss_spv, {}, "main"};

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
        HE_ASSERT(rtPSO, "RT PSO 创建失败");

        // SBT
        u32 groupCount = rtPSO->GetShaderGroupCount();
        u32 handleSize = rtPSO->GetShaderGroupHandleSize();
        auto handles   = rtPSO->GetShaderGroupHandles();
        u32 sbtSize    = groupCount * handleSize;

        rtSBTBuf = device->CreateBuffer({
            .size = sbtSize, .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Uniform,
        });
        u8* mapped = static_cast<u8*>(rtSBTBuf->Map());
        for (u32 g = 0; g < groupCount; ++g)
            std::memcpy(mapped + g * handleSize, handles.data() + g * handleSize, handleSize);
        rtSBTBuf->Unmap();

        sbt.buffer = rtSBTBuf.get();
        sbt.rayGen.handleOffset = 0;         sbt.rayGen.stride = handleSize;
        sbt.miss.handleOffset   = handleSize; sbt.miss.stride   = handleSize;

        HE_CORE_INFO("RT 管线就绪: {} groups, {}B SBT (直接渲染到 BackBuffer)", groupCount, sbtSize);
    }

    // ============================================================
    // 5. ImGui 集成
    // ============================================================
    he::editor::ImGuiIntegration imgui;
    imgui.Initialize(engine.GetWindow()->GetNativeHandle(), device.get(), swapchain.get());

    // ============================================================
    // 6. 命令列表
    // ============================================================
    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());

    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
    });

    // ============================================================
    // 7. 主渲染循环
    // ============================================================
    int renderMode = rtSupported ? 1 : 0;  // 0=光栅化, 1=RT
    HE_CORE_INFO("01.Triangle started — ImGui 切换渲染模式");
    u64 frameIndex = 0;

    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();
        if (!swapchain->AcquireNextImage()) continue;

        u32 w = swapchain->GetWidth();
        u32 h = swapchain->GetHeight();

        cmdList->Begin();

        // ── RT 调度 + 直接渲染到 BackBuffer ──
        if (renderMode == 1 && rtSupported && rtPSO) {
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
            cmdList->BeginRenderPass(1, he::rhi::Format::RGBA8_UNORM, he::rhi::Format::Unknown,
                                     nullptr, rhi::IRHICommandList::LoadOp::Load);
        } else {
            // 光栅化模式：红色三角形
            cmdList->SetPipeline(pipeline.get());
            cmdList->BeginRenderPass(1, he::rhi::Format::RGBA8_UNORM);
            cmdList->SetViewport({0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f});
            cmdList->SetScissor({0, 0, w, h});
            cmdList->SetVertexBuffer(vertexBuffer.get(), 0);
            cmdList->Draw(3);
        }

        // ── ImGui（在主 RP 内渲染，覆盖在场景之上）──
        imgui.BeginFrame();
        {
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(260, rtSupported ? 120 : 70), ImGuiCond_FirstUseEver);
            ImGui::Begin("渲染模式");

            ImGui::RadioButton("光栅化 (VS→PS)", &renderMode, 0);
            if (rtSupported) {
                ImGui::RadioButton("Ray Tracing (RayGen)", &renderMode, 1);
            } else {
                ImGui::BeginDisabled();
                ImGui::RadioButton("Ray Tracing (不支持)", &renderMode, 1);
                ImGui::EndDisabled();
            }

            ImGui::Separator();
            ImGui::Text("当前: %s", renderMode == 0 ? "光栅化" : "Ray Tracing");
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::End();
        }
        imgui.EndFrame(cmdList.get());

        cmdList->EndRenderPass();
        cmdList->End();

        device->Submit(cmdList.get());
        swapchain->Present(true);
        frameIndex++;
    }

    // ============================================================
    // 8. 清理
    // ============================================================
    device->WaitIdle();
    imgui.Shutdown();
    if (rtLayout != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(rtLayout);

    HE_CORE_INFO("Exiting after {} frames", frameIndex);
    return 0;
}
