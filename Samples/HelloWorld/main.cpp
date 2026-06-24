// ============================================================
// HelloWorld — HugEngine Phase 1: Triangle on screen
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "EmbeddedShaders.h"

#include <cstring>

using namespace he;

int main() {
    // ============================================================
    // 1. 引擎启动
    // ============================================================
    he::EngineConfig config;
    config.appName      = "HugEngine — Triangle";
    config.windowWidth  = 1920;
    config.windowHeight = 1080;

    he::Engine engine(config);
    engine.Initialize();

    // ============================================================
    // 2. 创建 RHI 设备
    // ============================================================
    he::rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = he::rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    auto device = he::rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    he::rhi::SetDevice(device.get());

    // ============================================================
    // 3. 创建 SwapChain
    // ============================================================
    auto swapchain = device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width  = engine.GetWindow()->GetWidth(),
        .height = engine.GetWindow()->GetHeight(),
        .vsync  = true,
    });

    // ============================================================
    // 4. 创建三角形顶点缓冲
    // ============================================================
    struct Vertex { float x, y; };
    const Vertex kVertices[] = {
        { 0.0f, -0.5f},
        { 0.5f,  0.5f},
        {-0.5f,  0.5f},
    };

    auto vertexBuffer = device->CreateBuffer({
        .size        = sizeof(kVertices),
        .usage       = he::rhi::BufferUsage::Vertex,
        .initialData = kVertices,
        .stride      = sizeof(Vertex),
    });

    // ============================================================
    // 5. 加载着色器
    // ============================================================
    he::rhi::ShaderBytecode vertShader;
    vertShader.stage      = he::rhi::ShaderStage::Vertex;
    vertShader.spirv      = k_Triangle_vert_spv;
    vertShader.entryPoint = "main";

    he::rhi::ShaderBytecode fragShader;
    fragShader.stage      = he::rhi::ShaderStage::Pixel;
    fragShader.spirv      = k_Triangle_frag_spv;
    fragShader.entryPoint = "main";

    // ============================================================
    // 6. 创建管线状态（PSO）
    // ============================================================
    auto pipeline = device->CreatePipelineState({
        .vertexShader        = &vertShader,
        .pixelShader         = &fragShader,
        .vertexLayout        = {.stride = sizeof(Vertex)},
        .depthTest           = false,
        .depthWrite          = false,
        .debugName           = "Triangle",
    });

    // ============================================================
    // 7. 创建命令列表，关联 SwapChain（自动管理 Framebuffer + 同步）
    // ============================================================
    auto cmdList = device->CreateCommandList();
    cmdList->SetPipeline(pipeline.get());
    cmdList->SetSwapChain(swapchain.get());

    // ============================================================
    // 8. 窗口大小调整回调
    // ============================================================
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());  // 重建 Framebuffer
    });

    // ============================================================
    // 9. 主渲染循环
    // ============================================================
    HE_CORE_INFO("Rendering triangle...");
    u64 frameIndex = 0;

    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();

        if (!swapchain->AcquireNextImage())
            continue;

        cmdList->Begin();
        cmdList->BeginRenderPass(1, he::rhi::Format::RGBA8_UNORM);
        cmdList->SetViewport({0, 0,
            (float)swapchain->GetWidth(), (float)swapchain->GetHeight(), 0, 1});
        cmdList->SetScissor({0, 0, swapchain->GetWidth(), swapchain->GetHeight()});
        cmdList->SetVertexBuffer(vertexBuffer.get(), 0);
        cmdList->Draw(3);
        cmdList->EndRenderPass();
        cmdList->End();

        device->Submit(cmdList.get());
        swapchain->Present(true);
        frameIndex++;
    }

    device->WaitIdle();
    HE_CORE_INFO("Exited after {} frames", frameIndex);
    engine.Shutdown();
    return 0;
}
