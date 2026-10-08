// ============================================================
// 07.AISamples — AI 综合示例（原 05~10 六个 AI Sample 合并）
//
// 共享引擎/RHI/AI 设备/渲染管线/相机，6 个功能模块独立文件：
//   LLM 场景生成 / 智能体 / GPU 推理 / 文生纹理 / 文生材质 / 文生网格动画
// 顶部 TabBar 切换功能，各功能的 World/场景完全隔离。
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "RHI/ThreadAffinity.h"      // T2.4：渲染线程启动/停止钩子里认领与撤销 RHI 归属
#include "Threading/RenderThread.h"  // T2.4：命令队列 + 渲染线程 + 帧票据（严格握手）
#include "Pipeline/ForwardPipeline.h"
#include "SceneRenderer.h"
#include "Pipeline/CameraController.h"
#include "Pipeline/Camera.h"
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

    // --- 6. 渲染线程化（T2.4）：整帧 RHI 交给渲染线程，游戏线程只做输入/相机/装配快照/UI ---
    // 【交接形态】一帧两条命令：① Acquire + 录制（管线 + 后处理 + BackBuffer pass）；
    // ② ImGui 的**录制**（draw data）+ End + Submit + Present。控件（CPU 侧）留在游戏线程，
    // 仍位于两条命令之间 —— 与改动前的帧内顺序一致。
    render::RenderCommandQueue renderQueue(rhi::kMaxFramesInFlight);
    render::RenderThread       renderThread(renderQueue);
    render::FrameScheduler     frameScheduler(renderQueue, renderThread);
    const bool                 useRenderQueue = he::UsesRenderThread();
    const bool                 forceShell = std::getenv("HE_RENDER_THREAD_FORCE_SHELL") != nullptr;
    if (useRenderQueue && !forceShell) {
        renderThread.SetSpinWaitUs(50);
        // RHI 归属：启动时认领给渲染线程、停止时撤销（认领必须在新线程里做 ⇒ 走启动钩子）
        renderThread.SetThreadStartHook([] { he::rhi::GetThreadAffinity().Claim(); });
        renderThread.SetThreadStopHook([] { he::rhi::GetThreadAffinity().Release(); });
        HE_CORE_INFO("[AISamples] 已起真渲染线程（整帧 RHI 归它；游戏线程按帧严格握手等待）");
        renderThread.Start();
    }
    // 模式 0 直接在本线程执行（此时本线程就是渲染线程）；模式 1/2 经队列 + 严格握手
    auto submitRender = [&](auto&& fn) {
        if (useRenderQueue) {
            renderQueue.BeginFrame();
            renderQueue.Enqueue([&](render::RenderThreadContext&) { fn(); });
            bool timedOut = false;
            frameScheduler.SubmitAndWait(timedOut);
            if (timedOut) HE_CORE_WARN("[AISamples] 等待本帧渲染命令完成超时");
        } else {
            fn();
        }
    };

    // --- 7. 主循环 ---
    f64 lastTime = glfwGetTime();
    while (!engine.GetWindow()->ShouldClose()) {
        f64 now = glfwGetTime();
        f32 dt  = (f32)(now - lastTime);
        lastTime = now;

        engine.GetWindow()->PollEvents();
        // 【T2.4】AcquireNextImage 已移入渲染命令（命令 1）—— 交换链归属在渲染线程

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
            double cx, cy;
            glfwGetCursorPos(glfwWin, &cx, &cy);
            float dx = (float)(cx - lastMouseX), dy = (float)(cy - lastMouseY);
            lastMouseX = cx;
            lastMouseY = cy;
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

        // ---- 游戏线程：帧前准备（相机/装配快照/阴影收集；都不碰 RHI）----
        rhi::Format backFmt = swapchain->GetColorFormat();   // 只读查询，不涉及归属
        pipeline.NextFrame();
        World* fWorld = cur->GetWorld();
        SceneGraph* fSG = cur->GetSceneGraph();
        const bool needs3D = cur->NeedsRender3D() && fWorld && fSG;
        render::CameraData frameCamera = camCtrl.GetCamera();
        if (needs3D) {
            auto* shadowSys = pipeline.GetShadowSystem();
            shadowSys->SetRenderResources(
                pipeline.GetCurrentShadowObjectBuffer(),
                pipeline.GetCurrentShadowBuffer(),
                pipeline.GetCurrentDescSet());
            // 帧相机解析：LLM 生成的场景含主相机实体（Camera 组件 isMain=true）时优先使用，
            // 否则回退自由相机 CameraController（S0.4 主相机接入）
            he::CameraComponent* mainCam = fWorld->GetPrimaryCamera();
            he::TransformComponent* mainCamXform =
                mainCam ? fWorld->GetComponent<he::TransformComponent>(mainCam->GetEntity()) : nullptr;
            frameCamera = render::ResolveFrameCamera(mainCam, mainCamXform, camCtrl.GetCamera());
            render::SubsystemContext shadowCtx;
            shadowCtx.world = fWorld;
            shadowCtx.sceneGraph = fSG;
            shadowCtx.camera = &frameCamera;
            he::SyncPhysicalSkyToSun(*fWorld);
            // 【阶段 1 §15.1 第③段第 4 批】快照由样例装配（口径由管线在 Initialize 配置）；
            // 顺序：世界同步 → 装配场景 → 阴影收集 → 解析光源 shadowIndex → Render(快照)
            pipeline.GetFrameAssembler().AssembleScene(*fWorld, *fSG, frameCamera);
            shadowCtx.snapshot     = &pipeline.GetFrameSnapshot();
            shadowCtx.meshRegistry = &pipeline.GetMeshRegistry();
            shadowSys->Update(shadowCtx);
            pipeline.GetFrameAssembler().ResolveLightShadowIndices(
                [&](he::Entity le) { return shadowSys->GetShadowIndex(le); });
            pipeline.GetFrameAssembler().ReserveOnce();
        }

        // ---- 命令 1：Acquire + 录制（管线 → 后处理 → BackBuffer pass 开始）----
        bool frameAborted = false;
        auto recordScene = [&]() {
            if (!swapchain->AcquireNextImage()) {
                frameAborted = true;   // 命令可能在渲染线程跑 ⇒ 用标志代替 while 的 continue
                return;
            }
            cmdList->Begin();
            if (needs3D) {
                pipeline.Render(cmdList.get(), pipeline.GetFrameSnapshot(), frameCamera);
                // pass 级调试标记：BackBuffer 合成（ToneMap + ImGui），RenderDoc 可识别
                cmdList->BeginDebugLabel("ToneMap + ImGui (BackBuffer)");
                cmdList->BeginRenderPass(1, backFmt);
                pipeline.RenderToneMapPass(cmdList.get());
            } else {
                // 无 3D 场景：仅 ImGui 面板（用 LoadOp::Load 保留背景色）
                cmdList->BeginDebugLabel("ImGui Only (BackBuffer)");
                cmdList->BeginRenderPass(1, backFmt, rhi::Format::Unknown, nullptr, rhi::LoadOp::Clear);
            }
        };
        submitRender(recordScene);
        if (frameAborted) continue;   // Acquire 失败：跳过本帧剩余部分

        // ImGui：顶部功能切换 TabBar + 当前功能面板（CPU 侧，游戏线程）
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

        // ---- 命令 2：ImGui 录制 + End + Submit + Present ----
        auto recordUiAndPresent = [&]() {
            imgui.EndFrame(cmdList.get());
            cmdList->EndDebugLabel();   // 闭合 BackBuffer pass 级标记
            cmdList->EndRenderPass();
            cmdList->End();
            device->Submit(cmdList.get());
            swapchain->Present(true);
        };
        submitRender(recordUiAndPresent);
    }

    // 【T2.4】退出前停渲染线程并撤销 RHI 归属认领：收尾期的 WaitIdle/设备销毁仍在游戏线程
    renderThread.Stop();
    he::rhi::GetThreadAffinity().Release();

    imgui.Shutdown();
    device->WaitIdle();
    for (auto& f : features) f->Shutdown();
    pipeline.Shutdown();
    HE_CORE_INFO("[AISamples] 退出");
    return 0;
}
