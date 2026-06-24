// ============================================================
// HelloWorld — HugEngine Phase 1: Triangle on screen
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Vulkan/VulkanInternal.h"  // Phase 1 bridge
#include "EmbeddedShaders.h"

#include <cstring>

int main() {
    // --- Engine bootstrap ---
    he::EngineConfig config;
    config.appName      = "HugEngine — Triangle";
    config.windowWidth  = 1920;
    config.windowHeight = 1080;

    he::Engine engine(config);
    engine.Initialize();

    // --- RHI device ---
    he::rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = he::rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    auto device = he::rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    he::rhi::SetDevice(device.get());

    // --- SwapChain ---
    auto swapchain = device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width  = engine.GetWindow()->GetWidth(),
        .height = engine.GetWindow()->GetHeight(),
        .vsync  = true,
    });

    // --- Triangle vertices ---
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

    // --- Shaders ---
    he::rhi::ShaderBytecode vertShader;
    vertShader.stage      = he::rhi::ShaderStage::Vertex;
    vertShader.spirv      = k_Triangle_vert_spv;
    vertShader.entryPoint = "main";

    he::rhi::ShaderBytecode fragShader;
    fragShader.stage      = he::rhi::ShaderStage::Pixel;
    fragShader.spirv      = k_Triangle_frag_spv;
    fragShader.entryPoint = "main";

    // --- Pipeline ---
    auto pipeline = device->CreatePipelineState({
        .debugName           = "Triangle",
        .vertexShader        = &vertShader,
        .pixelShader         = &fragShader,
        .vertexLayout        = {.stride = sizeof(Vertex)},
        .depthTest           = false,
        .depthWrite          = false,
    });

    // --- Command list ---
    auto cmdList = device->CreateCommandList();
    cmdList->SetPipeline(pipeline.get());

    // --- Vulkan framebuffer setup (Phase 1 bridge) ---
    auto* vkCmd       = static_cast<he::rhi::VulkanCommandList*>(cmdList.get());
    auto* vkSwapchain = static_cast<he::rhi::VulkanSwapChain*>(swapchain.get());

    auto setupFramebuffers = [&]() {
        u32 count = 3;
        std::vector<VkImageView> views(count);
        for (u32 i = 0; i < count; ++i)
            views[i] = vkSwapchain->GetImageView(i);
        vkCmd->SetSwapchainViews(views, {vkSwapchain->GetWidth(), vkSwapchain->GetHeight()});
    };
    setupFramebuffers();

    // --- Resize ---
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        swapchain->Resize(w, h);
        setupFramebuffers();
    });

    // --- Main loop ---
    HE_CORE_INFO("Rendering triangle...");
    u64 frameIndex = 0;

    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();

        if (!swapchain->AcquireNextImage())
            continue;

        vkCmd->SetCurrentImageIndex(swapchain->GetCurrentBackBufferIndex());

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
