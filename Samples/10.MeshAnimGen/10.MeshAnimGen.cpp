// ============================================================
// 10.MeshAnimGen — 文生网格 + 文生动画（G2.3 资产生成验证）
//
// 1. TextToMesh：LLM 输出形状规格（有 key）→ 程序化生成顶点/索引
//    （无 key 降级内置规格：金字塔），SetMeshData 渲染
// 2. TextToAnimation：LLM 输出关键帧规格 → 生成动画轨道
//    （无 key 降级内置规格：左右摆动），AnimationComponent 播放
// 结果显示在 ImGui 面板。
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
#include "Scene/CubeComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Scene/AnimationComponent.h"
#include "AI/Runtime/AIDevice.h"
#include "AI/Runtime/InferenceScheduler.h"
#include "AI/AIGC/GenerativeAssetFactory.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace he;

int main() {
#ifdef _WIN32
    // 本工程以 /utf-8 编译，日志为 UTF-8 字节；控制台代码页切到 UTF-8，避免中文乱码
    SetConsoleOutputCP(CP_UTF8);
#endif

    // --- 1. 引擎 + RHI + SwapChain ---
    EngineConfig config;
    config.appName       = "HugEngine — 文生网格与动画（G2.3）";
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

    // --- 2. AI 设备 ---
    he::ai::InferenceScheduler scheduler;
    auto aiDevice = he::ai::CreateAIDevice(&scheduler, device.get());
    const he::ai::AIDeviceCaps caps = aiDevice->GetCaps();

    // --- 3. 场景 ---
    World world;
    SceneGraph sceneGraph(world);

    // 生成网格实体（应用 TextToMesh 结果）
    Entity genEntity;
    MeshComponent* genMesh = nullptr;
    AnimationComponent* genAnim = nullptr;
    {
        Entity e = world.CreateEntity("GenObject");
        genEntity = e;
        world.AddComponent<TransformComponent>(e);
        genMesh = world.AddComponent<CubeComponent>(e);   // 先用默认几何，下面覆盖
        genMesh->baseColorFactor = float4(0.8f, 0.5f, 0.2f, 1.0f);
        sceneGraph.SetParent(e, Entity{kInvalidEntity});
    }
    // 地面 / 光源 / 天空
    {
        Entity e = world.CreateEntity("Ground");
        auto* xform = world.AddComponent<TransformComponent>(e);
        xform->position = float3(0, -1, 0);
        xform->scale    = float3(20, 0.2f, 20);
        auto* mesh = world.AddComponent<CubeComponent>(e);
        mesh->baseColorFactor = float4(0.3f, 0.3f, 0.35f, 1.0f);
        sceneGraph.SetParent(e, Entity{kInvalidEntity});
    }
    {
        Entity e = world.CreateEntity("Sun");
        world.AddComponent<TransformComponent>(e);
        auto* dl = world.AddComponent<DirectionalLight>(e);
        dl->direction = float3(0.5f, -1, 0.5f);
        dl->intensity = 5.0f;
        sceneGraph.SetParent(e, Entity{kInvalidEntity});
    }
    {
        Entity e = world.CreateEntity("Sky");
        world.AddComponent<TransformComponent>(e);
        world.AddComponent<PhysicalSkyComponent>(e);
        sceneGraph.SetParent(e, Entity{kInvalidEntity});
    }

    // --- 4. 文生网格（生成 = 标准网格，SetMeshData 渲染）---
    he::ai::aigc::MeshGenResult meshResult;
    const String kMeshPrompt = "一个金字塔，尺寸 1.2";
    if (caps.supportsRemoteLLM) {
        he::ai::aigc::GenerativeAssetFactory factory;
        meshResult = factory.TextToMesh(*aiDevice, kMeshPrompt);
    } else {
        // 无 key：降级内置规格（金字塔）
        HE_CORE_WARN("[MeshAnimGen] 未设置 DEEPSEEK_API_KEY，用内置规格演示（LLM 规格需 key）");
        meshResult.name = "pyramid";
        he::ai::aigc::MeshGenerator::Generate("pyramid", 1.2f, 16, meshResult);
        meshResult.error = "降级模式";
    }
    if (meshResult.success && genMesh) {
        genMesh->SetMeshData(meshResult.vertices, meshResult.indices);
        HE_CORE_INFO("[MeshAnimGen] 已应用生成网格: {} ({} 顶点 / {} 索引)",
                     meshResult.name, meshResult.vertices.size(), meshResult.indices.size());
    }

    // --- 5. 文生动画（生成 = 标准动画，AnimationComponent 播放）---
    he::ai::aigc::AnimGenResult animResult;
    const String kAnimPrompt = "左右摆动动画：0 秒在原点，1 秒移到右侧，2 秒回到原点";
    if (caps.supportsRemoteLLM) {
        he::ai::aigc::GenerativeAssetFactory factory;
        animResult = factory.TextToAnimation(*aiDevice, kAnimPrompt);
    } else {
        // 无 key：降级内置规格（左右摆动）
        HE_CORE_WARN("[MeshAnimGen] 未设置 DEEPSEEK_API_KEY，用内置规格演示（LLM 规格需 key）");
        animResult.success = true;
        animResult.name    = "swing";
        animResult.duration = 2.0f;
        animResult.loop    = true;
        animResult.translations = {{0.0f, float3(0, 0.5f, 0)},
                                   {1.0f, float3(1.5f, 0.5f, 0)},
                                   {2.0f, float3(0, 0.5f, 0)}};
        animResult.error = "降级模式";
    }
    if (animResult.success) {
        // 标准动画 → AnimationComponent（clip 轨道）
        genAnim = world.AddComponent<AnimationComponent>(genEntity);
        genAnim->clips.resize(1);
        auto& clip = genAnim->clips[0];
        clip.name = animResult.name;
        clip.duration = animResult.duration;
        clip.looping  = animResult.loop;
        for (auto& k : animResult.translations) clip.translations.push_back(k);
        for (auto& k : animResult.scales)       clip.scales.push_back(k);
        genAnim->currentClip = 0;
        genAnim->playing = true;
        HE_CORE_INFO("[MeshAnimGen] 已应用生成动画: {}（{} 关键帧，{}s）",
                     animResult.name, animResult.translations.size(), animResult.duration);
    }

    // --- 6. 渲染管线 + 相机 + ImGui ---
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

    bool rightMouseDown = false;
    double lastMouseX = 0, lastMouseY = 0;

    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (w == 0 || h == 0) return;
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        pipeline.OnResize(w, h);
        camCtrl.SetAspectRatio((float)w, (float)h);
    });

    // --- 7. 主循环 ---
    f64 lastTime = glfwGetTime();
    while (!engine.GetWindow()->ShouldClose()) {
        f64 now = glfwGetTime();
        f32 dt  = (f32)(now - lastTime);
        lastTime = now;

        engine.GetWindow()->PollEvents();
        if (!swapchain->AcquireNextImage()) continue;

        // ★ 播放生成动画（驱动目标 Transform）
        if (genAnim && genAnim->playing) {
            genAnim->Update(dt, world.GetComponent<TransformComponent>(genEntity));
        }

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
        he::SyncPhysicalSkyToSun(world);
        shadowSys->Update(shadowCtx);
        pipeline.Render(cmdList.get(), world, sceneGraph, camCtrl.GetCamera());
        cmdList->BeginRenderPass(1, backFmt);
        pipeline.RenderToneMapPass(cmdList.get());

        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("文生网格与动画 G2.3");
        ImGui::Text("网格生成: %s", meshResult.success ? "成功" : "失败");
        if (meshResult.success)
            ImGui::Text("形状: %s | 顶点: %zu | 索引: %zu",
                        meshResult.name.c_str(),
                        (size_t)meshResult.vertices.size(), (size_t)meshResult.indices.size());
        if (!meshResult.error.empty())
            ImGui::TextWrapped("说明: %s", meshResult.error.c_str());
        ImGui::Separator();
        ImGui::Text("动画生成: %s", animResult.success ? "成功" : "失败");
        if (animResult.success)
            ImGui::Text("名称: %s | 关键帧: %zu | 时长: %.1fs | 循环: %s",
                        animResult.name.c_str(), (size_t)animResult.translations.size(),
                        animResult.duration, animResult.loop ? "是" : "否");
        if (!animResult.error.empty())
            ImGui::TextWrapped("说明: %s", animResult.error.c_str());
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
    HE_CORE_INFO("[MeshAnimGen] 退出");
    return 0;
}
