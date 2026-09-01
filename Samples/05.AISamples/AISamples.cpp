// ============================================================
// 05.AISamples — AI 综合示例（原 05~10 六个 AI Sample 合并）
//
// 共享引擎/RHI/AI 设备/渲染管线/相机，6 个功能模块独立文件：
//   LLM 场景生成 / 智能体 / GPU 推理 / 文生纹理 / 文生材质 / 文生网格动画
// 顶部 TabBar 切换功能，各功能的 World/场景完全隔离。
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/ForwardPipeline.h"
#include "SceneRenderer.h"
#include "Pipeline/CameraController.h"
#include "Scene/PhysicalSkyComponent.h"
#include "AI/Runtime/AIDevice.h"
#include "AI/Runtime/InferenceScheduler.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include "Features/IFeature.h"
#include "Features/FeatureLLMScene.h"
#include "Features/FeatureAgentScene.h"
#include "Features/FeatureGPUInference.h"
#include "Features/FeatureTextureGen.h"
#include "Features/FeatureMaterialGen.h"
#include "Features/FeatureMeshAnimGen.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <memory>
#include <vector>

using namespace he;

int main() {
#ifdef _WIN32
    // 本工程以 /utf-8 编译，日志为 UTF-8 字节；控制台代码页切到 UTF-8，避免中文乱码
    SetConsoleOutputCP(CP_UTF8);
#endif

    // --- 1. 引擎 + RHI + SwapChain ---
    EngineConfig config;
    config.appName       = "HugEngine — AI Samples";
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

    // --- 2. AI 设备（GPU + 远程 LLM，全局共享）---
    he::ai::InferenceScheduler scheduler;
    auto aiDevice = he::ai::CreateAIDevice(&scheduler, device.get());

    // --- 3. 共享渲染管线 + 相机 + ImGui ---
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

    render::CameraController camCtrl;
    camCtrl.SetAspectRatio((float)swapchain->GetWidth(), (float)swapchain->GetHeight());
    camCtrl.SetPosition(float3(0, 3, 8));
    camCtrl.SetOrientation(-1.57f, -0.1f);

    // --- 4. 功能模块注册（每个原 Sample 一个 Feature，独立文件）---
    std::vector<std::unique_ptr<IFeature>> features;
    features.push_back(std::make_unique<FeatureLLMScene>());
    features.push_back(std::make_unique<FeatureAgentScene>());
    features.push_back(std::make_unique<FeatureGPUInference>());
    features.push_back(std::make_unique<FeatureTextureGen>());
    features.push_back(std::make_unique<FeatureMaterialGen>());
    features.push_back(std::make_unique<FeatureMeshAnimGen>());

    for (auto& f : features) {
        if (!f->Initialize(device.get(), swapchain.get(), aiDevice.get()))
            HE_CORE_WARN("[AISamples] 功能 '{}' 初始化失败", f->GetName());
    }
    int currentFeature = 0;
    features[currentFeature]->Update(0.0f);   // 触发首次逻辑（如一次性推理）

    // --- 5. 输入状态 ---
    bool rightMouseDown = false;
    double lastMouseX = 0, lastMouseY = 0;

    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (w == 0 || h == 0) return;
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        pipeline.OnResize(w, h);
        camCtrl.SetAspectRatio((float)w, (float)h);
    });

    // --- 6. 主循环 ---
    f64 lastTime = glfwGetTime();
    while (!engine.GetWindow()->ShouldClose()) {
        f64 now = glfwGetTime();
        f32 dt  = (f32)(now - lastTime);
        lastTime = now;

        engine.GetWindow()->PollEvents();
        if (!swapchain->AcquireNextImage()) continue;

        // 相机控制（WASD + 右键）
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

        // 当前功能 CPU 逻辑
        IFeature* cur = features[currentFeature].get();
        cur->Update(dt);

        // 渲染（当前功能的 World，经共享管线）
        rhi::Format backFmt = swapchain->GetColorFormat();
        cmdList->Begin();
        pipeline.NextFrame();

        World* fWorld = cur->GetWorld();
        SceneGraph* fSG = cur->GetSceneGraph();
        if (cur->NeedsRender3D() && fWorld && fSG) {
            auto* shadowSys = pipeline.GetShadowSystem();
            shadowSys->SetRenderResources(
                pipeline.GetCurrentShadowObjectBuffer(),
                pipeline.GetCurrentShadowBuffer(),
                pipeline.GetCurrentDescSet());
            render::SubsystemContext shadowCtx;
            shadowCtx.world = fWorld; shadowCtx.sceneGraph = fSG;
            shadowCtx.camera = &camCtrl.GetCamera();
            he::SyncPhysicalSkyToSun(*fWorld);
            shadowSys->Update(shadowCtx);
            pipeline.Render(cmdList.get(), *fWorld, *fSG, camCtrl.GetCamera());
            cmdList->BeginRenderPass(1, backFmt);
            pipeline.RenderToneMapPass(cmdList.get());
        } else {
            // 无 3D 场景：仅 ImGui 面板（用 LoadOp::Load 保留背景色）
            cmdList->BeginRenderPass(1, backFmt, rhi::Format::Unknown, nullptr, rhi::LoadOp::Clear);
        }

        // ImGui：顶部功能切换 TabBar + 当前功能面板
        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("AI Samples");
        ImGui::Text("后端: GPU=%s LLM=%s",
                    aiDevice->GetCaps().supportsGPU ? "可用" : "不可用",
                    aiDevice->GetCaps().supportsRemoteLLM ? "可用" : "不可用");
        ImGui::Separator();

        // TabBar 切换功能
        if (ImGui::BeginTabBar("FeatureTab")) {
            for (int i = 0; i < (int)features.size(); ++i) {
                bool selected = (i == currentFeature);
                if (ImGui::TabItemButton(features[i]->GetName(), selected
                        ? ImGuiTabItemFlags_None : ImGuiTabItemFlags_None)) {
                    if (i != currentFeature) {
                        currentFeature = i;
                        camCtrl.SetPosition(features[i]->GetDefaultCameraPos());
                    }
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::End();

        cur->RenderUI();   // 当前功能面板

        imgui.EndFrame(cmdList.get());
        cmdList->EndRenderPass();
        cmdList->End();

        device->Submit(cmdList.get());
        swapchain->Present(true);
    }

    imgui.Shutdown();
    device->WaitIdle();
    for (auto& f : features) f->Shutdown();
    pipeline.Shutdown();
    HE_CORE_INFO("[AISamples] 退出");
    return 0;
}
