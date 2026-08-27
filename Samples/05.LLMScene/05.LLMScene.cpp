// ============================================================
// 05.LLMScene — LLM 一句话生成场景（DeepSeek）
//
// 核心：用一句话 prompt 让 DeepSeek 大模型返回场景 JSON，
// 引擎经 SceneBuilder 翻译成真实 Entity/Component 并渲染出来。
//
// 用法：先设置环境变量 DEEPSEEK_API_KEY，然后：
//   05.LLMScene.exe "一个黄昏下的中世纪村庄"
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/ForwardPipeline.h"
#include "SceneRenderer.h"
#include "Pipeline/CameraController.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/PhysicalSkyComponent.h"
#include "AI/PromptToScene.h"
#include "AI/DeepSeekClient.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include <cstdlib>
#include <cstring>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace he;

int main(int argc, char** argv) {
    // --- 1. 读 DeepSeek API Key（不硬编码，从环境变量读）---
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (!key || !*key) {
        printf("[LLMScene] 请先设置环境变量 DEEPSEEK_API_KEY\n");
        return -1;
    }

    // --- 2. 用户 prompt（命令行参数，或默认值）---
    String prompt = (argc > 1) ? String(argv[1])
                               : "一个黄昏下的中世纪村庄，几间石屋、一口井和一盏温暖的篝火";

    // --- 3. 引擎 + RHI + SwapChain ---
    EngineConfig config;
    config.appName       = "HugEngine — LLM 场景生成";
    config.windowWidth   = 1280;
    config.windowHeight  = 720;
    Engine engine(config);
    engine.Initialize();

    rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = rhi::Backend::Vulkan;
    rhiDesc.enableValidation = false;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();
    auto device = rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    rhi::SetDevice(device.get());

    auto swapchain = device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width  = engine.GetWindow()->GetWidth(),
        .height = engine.GetWindow()->GetHeight(),
        .vsync  = true,
        .hdr    = false,
    });

    // --- 4. LLM 生成场景（★ 核心：替代手工搭场景）---
    World world;
    SceneGraph sceneGraph(world);

    he::ai::DeepSeekClient llm{String(key)};   // 花括号构造，避免最烦人解析
    he::ai::SceneBuildResult result = he::ai::PromptToScene(llm, world, sceneGraph, prompt);
    if (!result.success) {
        HE_CORE_ERROR("[LLMScene] 场景生成失败: {}", result.error);
        return -1;
    }
    HE_CORE_INFO("[LLMScene] LLM 生成了 {} 个实体", result.entities.size());

    // --- 5. 前向管线 + 命令列表 ---
    render::ForwardPipeline pipeline;
    pipeline.Initialize(device.get());
    pipeline.SetUseRenderGraph(false);
    pipeline.SetMultiThreadedRecording(false);
    pipeline.SetSwapChain(swapchain.get());
    pipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());
    pipeline.GetGPUCulling().enabled = false;
    pipeline.GetSceneRenderer().enableFrustumCull = false;

    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());
    cmdList->SetPipeline(pipeline.GetPipelineState());

    GLFWwindow* glfwWin = engine.GetWindow()->GetNativeHandle();
    editor::ImGuiIntegration imgui;
    imgui.Initialize(glfwWin, device.get(), swapchain.get());

    // --- 6. 自由相机 ---
    render::CameraController camCtrl;
    camCtrl.SetAspectRatio((float)swapchain->GetWidth(), (float)swapchain->GetHeight());
    camCtrl.SetPosition(float3(0.0f, 5.0f, 12.0f));
    camCtrl.SetOrientation(-1.57f, -0.2f);

    bool rightMouseDown = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;

    // 窗口调整回调（保持交换链与相机宽高比同步）
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (w == 0 || h == 0) return;
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        pipeline.OnResize(w, h);
        camCtrl.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // --- 7. 主循环 ---
    HE_CORE_INFO("[LLMScene] WASD=移动 右键拖拽=旋转 滚轮=缩放");
    f64 lastTime = glfwGetTime();
    while (!engine.GetWindow()->ShouldClose()) {
        f64 now = glfwGetTime();
        f32 dt  = (f32)(now - lastTime);
        lastTime = now;

        engine.GetWindow()->PollEvents();
        if (!swapchain->AcquireNextImage()) continue;

        // 相机控制（与 02.Cube 一致）
        bool mouseDown = glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        if (mouseDown && !rightMouseDown) {
            rightMouseDown = true;
            glfwGetCursorPos(glfwWin, &lastMouseX, &lastMouseY);
            glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        } else if (!mouseDown && rightMouseDown) {
            rightMouseDown = false;
            glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else if (mouseDown && rightMouseDown) {
            double cx, cy; glfwGetCursorPos(glfwWin, &cx, &cy);
            float dx = (float)(cx - lastMouseX), dy = (float)(cy - lastMouseY);
            lastMouseX = cx; lastMouseY = cy;
            camCtrl.Rotate(dx * 0.003f, -dy * 0.003f);
        }

        render::CameraController::MoveInput moveIn;
        moveIn.forward  = glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS;
        moveIn.backward = glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS;
        moveIn.left     = glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS;
        moveIn.right    = glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS;
        moveIn.up       = glfwGetKey(glfwWin, GLFW_KEY_E) == GLFW_PRESS;
        moveIn.down     = glfwGetKey(glfwWin, GLFW_KEY_Q) == GLFW_PRESS;
        moveIn.sprint   = glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
        camCtrl.Update(dt, moveIn);

        rhi::Format backFmt = swapchain->GetColorFormat();

        // 渲染（Forward 模式，路径与 02.Cube 一致，含阴影系统设置）
        cmdList->Begin();
        pipeline.NextFrame();

        auto* shadowSys = pipeline.GetShadowSystem();
        shadowSys->SetRenderResources(
            pipeline.GetCurrentShadowObjectBuffer(),
            pipeline.GetCurrentShadowBuffer(),
            pipeline.GetCurrentDescSet());
        render::SubsystemContext shadowCtx;
        shadowCtx.world = &world; shadowCtx.sceneGraph = &sceneGraph;
        shadowCtx.camera = &camCtrl.GetCamera();
        he::SyncPhysicalSkyToSun(world);   // 同步物理天空太阳方向到方向光
        shadowSys->Update(shadowCtx);

        pipeline.Render(cmdList.get(), world, sceneGraph, camCtrl.GetCamera());
        cmdList->BeginRenderPass(1, backFmt);
        pipeline.RenderToneMapPass(cmdList.get());

        // ImGui 叠加：FPS + 实体数 + 当前 prompt
        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("LLM 场景生成");
        ImGui::Text("FPS: %.0f", 1.0f / (dt > 0 ? dt : 0.016f));
        ImGui::Text("实体数: %zu", result.entities.size());
        ImGui::TextWrapped("Prompt: %s", prompt.c_str());
        ImGui::End();
        imgui.EndFrame(cmdList.get());
        cmdList->EndRenderPass();
        cmdList->End();

        device->Submit(cmdList.get());
        swapchain->Present(true);
    }

    imgui.Shutdown();
    device->WaitIdle();
    pipeline.Shutdown();
    HE_CORE_INFO("[LLMScene] 退出");
    return 0;
}
