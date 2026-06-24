// ============================================================
// Triangle — 使用 HugEngine RHI 渲染三角形
//
// 本示例演示 HugEngine 的最简渲染流程：
//   Engine 启动 → Vulkan 设备 → SwapChain → Buffer/Shader
//   → Pipeline 状态 → CommandList → 主循环绘制
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Vulkan/VulkanInternal.h"  // Phase 1 桥接：直接管理 Framebuffer
#include "EmbeddedShaders.h"

#include <cstring>
#include <vector>

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
    // 2. 创建 Vulkan RHI 设备
    // ============================================================
    he::rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = he::rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    auto device = he::rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    he::rhi::SetDevice(device.get());

    // ============================================================
    // 3. 创建 SwapChain（与窗口关联）
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
    struct Vertex {
        float x, y;  // NDC 坐标
    };

    const Vertex kVertices[] = {
        { 0.0f, -0.5f },  // 底端中点
        { 0.5f,  0.5f },  // 右上角
        {-0.5f,  0.5f },  // 左上角
    };

    auto vertexBuffer = device->CreateBuffer({
        .size        = sizeof(kVertices),
        .usage       = he::rhi::BufferUsage::Vertex,
        .initialData = kVertices,
        .stride      = sizeof(Vertex),
    });

    // ============================================================
    // 5. 加载着色器（由 HugEngineShader 模块编译嵌入）
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
    // 7. 创建命令列表
    // ============================================================
    auto cmdList = device->CreateCommandList();
    cmdList->SetPipeline(pipeline.get());

    // ============================================================
    // 8. Vulkan Framebuffer 设置（Phase 1 桥接）
    //    后续版本将通过 RenderGraph 自动管理
    // ============================================================
    auto* vkCmd       = static_cast<he::rhi::VulkanCommandList*>(cmdList.get());
    auto* vkSwapchain = static_cast<he::rhi::VulkanSwapChain*>(swapchain.get());

    std::vector<VkImageView> g_SwapchainViews(3);
    auto setupFramebuffers = [&]() {
        for (int i = 0; i < 3; ++i)
            g_SwapchainViews[i] = vkSwapchain->GetImageView(i);
        vkCmd->SetSwapchainViews(g_SwapchainViews, {vkSwapchain->GetWidth(), vkSwapchain->GetHeight()});
    };
    setupFramebuffers();

    // ============================================================
    // 9. 窗口大小调整回调
    // ============================================================
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        swapchain->Resize(w, h);
        setupFramebuffers();
    });

    // ============================================================
    // 10. 主渲染循环
    // ============================================================
    HE_CORE_INFO("Triangle sample started — rendering...");
    u64 frameIndex = 0;

    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();

        // 获取下一帧图像
        if (!swapchain->AcquireNextImage())
            continue;

        vkCmd->SetCurrentImageIndex(swapchain->GetCurrentBackBufferIndex());

        // 录制渲染命令
        cmdList->Begin();
        cmdList->BeginRenderPass(1, he::rhi::Format::RGBA8_UNORM);
        cmdList->SetViewport({
            0, 0,
            static_cast<float>(swapchain->GetWidth()),
            static_cast<float>(swapchain->GetHeight()),
            0.0f, 1.0f
        });
        cmdList->SetScissor({
            0, 0,
            swapchain->GetWidth(),
            swapchain->GetHeight()
        });
        cmdList->SetVertexBuffer(vertexBuffer.get(), 0);
        cmdList->Draw(3);  // 3 个顶点 = 1 个三角形
        cmdList->EndRenderPass();
        cmdList->End();

        // 提交并呈现
        device->Submit(cmdList.get());
        swapchain->Present(true);
        frameIndex++;
    }

    // ============================================================
    // 11. 清理退出
    // ============================================================
    device->WaitIdle();
    HE_CORE_INFO("Triangle sample exited after {} frames", frameIndex);
    engine.Shutdown();
    return 0;
}
