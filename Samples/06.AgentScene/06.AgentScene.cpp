// ============================================================
// 06.AgentScene — AI 智能体演示（AgentSystem + MockBrain）
//
// 场景中央的实体挂载 AgentComponent（Mock 大脑，默认每 2 秒
// 思考一次），每次思考经 AgentSystem 驱动产生一个新方块，
// 动作走 CommandHistory 可撤销。
//
// ImGui 控制：
//   「立即思考」 强制下一帧触发思考
//   「撤销」     撤销最近一次智能体动作
//   brainType / thinkInterval / enabled 实时可调
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
#include "AI/Agent/AgentComponent.h"
#include "AI/Agent/AgentSystem.h"
#include "AI/Runtime/AIModule.h"
#include "Editor/ImGuiIntegration.h"
#include "Editor/Command.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace he;

int main() {
    // --- 1. 引擎 + RHI + SwapChain ---
    EngineConfig config;
    config.appName       = "HugEngine — AI 智能体演示";
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

    // --- 2. AI 运行时（Mock 大脑不需要设备；初始化以支持 LLM 模式）---
    he::ai::AIModule::Initialize();
    he::ai::IAIDevice* aiDevice = he::ai::AIModule::GetDevice();

    // --- 3. 场景：地面 + 方向光 + 物理天空 + 智能体 ---
    World world;
    SceneGraph sceneGraph(world);
    he::CommandHistory history;   // 智能体动作的历史（可撤销）

    // 地面
    {
        Entity e = world.CreateEntity("Ground");
        auto* xform = world.AddComponent<TransformComponent>(e);
        xform->position = float3(0.0f, -1.0f, 0.0f);
        xform->scale    = float3(20.0f, 0.2f, 20.0f);
        auto* cube = world.AddComponent<CubeComponent>(e);
        cube->baseColorFactor = float4(0.3f, 0.3f, 0.35f, 1.0f);
        cube->roughnessFactor = 0.9f;
        sceneGraph.SetParent(e, Entity{kInvalidEntity});
    }
    // 方向光
    {
        Entity e = world.CreateEntity("Sun");
        world.AddComponent<TransformComponent>(e);
        auto* dl = world.AddComponent<DirectionalLight>(e);
        dl->direction = float3(0.5f, -1.0f, 0.5f);
        dl->color     = float3(1.0f, 0.95f, 0.85f);
        dl->intensity = 5.0f;
        dl->castShadow = true;
        sceneGraph.SetParent(e, Entity{kInvalidEntity});
    }
    // 物理天空
    {
        Entity e = world.CreateEntity("Sky");
        world.AddComponent<TransformComponent>(e);
        auto* ps = world.AddComponent<PhysicalSkyComponent>(e);
        ps->sunDirection = float3(0.4f, 0.6f, 0.2f);
        sceneGraph.SetParent(e, Entity{kInvalidEntity});
    }
    // ★ 智能体实体（Mock 大脑，每 2 秒思考一次，每次生成一个新方块）
    Entity agentEntity;
    {
        Entity e = world.CreateEntity("Agent");
        agentEntity = e;   // 记录 Agent 实体（UI 控制用）
        world.AddComponent<TransformComponent>(e);
        auto* agent = world.AddComponent<he::ai::AgentComponent>(e);
        agent->brainType     = "Mock";        // 可切 "LLM"（需 DEEPSEEK_API_KEY）
        agent->thinkInterval = 2.0f;          // 思考间隔（秒）
        agent->systemPrompt  = "你是演示智能体：每隔一段时间生成一个彩色方块。";
        sceneGraph.SetParent(e, Entity{kInvalidEntity});
        HE_CORE_INFO("[AgentScene] 智能体已挂载（Mock 大脑，每 {:.1f}s 思考一次）", agent->thinkInterval);
    }

    // --- 4. 前向管线 + 命令列表 ---
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

    // --- 5. 自由相机 ---
    render::CameraController camCtrl;
    camCtrl.SetAspectRatio((float)swapchain->GetWidth(), (float)swapchain->GetHeight());
    camCtrl.SetPosition(float3(0.0f, 4.0f, 12.0f));
    camCtrl.SetOrientation(-1.57f, -0.2f);

    bool rightMouseDown = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;

    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (w == 0 || h == 0) return;
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        pipeline.OnResize(w, h);
        camCtrl.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // --- 6. 主循环 ---
    HE_CORE_INFO("[AgentScene] WASD=移动 右键拖拽=旋转；智能体会自动思考生成方块");
    f64 lastTime = glfwGetTime();
    int thinkCount = 0;   // 累计思考次数（UI 显示）
    int lastEntityCount = (int)world.GetEntityCount();

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

        // ★ 智能体驱动：计时 → 决策 → 动作执行（可撤销）
        {
            he::ai::AgentSystem::Update(world, sceneGraph, history, aiDevice, dt);
            if ((int)world.GetEntityCount() != lastEntityCount) {
                lastEntityCount = (int)world.GetEntityCount();
                ++thinkCount;
                HE_CORE_INFO("[AgentScene] 智能体思考完成，当前实体数: {}", lastEntityCount);
            }
        }

        // --- 渲染（Forward 模式，与 02.Cube 一致）---
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

        // ImGui 面板：智能体状态与控制
        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("AI 智能体");
        ImGui::Text("FPS: %.0f", 1.0f / (dt > 0 ? dt : 0.016f));
        ImGui::Text("实体数: %d", lastEntityCount);
        ImGui::Text("思考次数: %d", thinkCount);
        ImGui::Separator();

        // 实时调整 Agent 参数
        auto* agent = world.GetComponent<he::ai::AgentComponent>(agentEntity);
        if (agent) {
            ImGui::Checkbox("enabled", &agent->enabled);
            ImGui::SliderFloat("thinkInterval(s)", &agent->thinkInterval, 0.2f, 10.0f);
            // brainType 切换（Mock / LLM）
            const char* brains[] = {"Mock", "LLM"};
            int cur = (agent->brainType == "LLM") ? 1 : 0;
            if (ImGui::Combo("brainType", &cur, brains, 2))
                agent->brainType = (cur == 1) ? "LLM" : "Mock";

            // 手动触发：把计时器置满，下一帧 Update 即思考
            if (ImGui::Button("立即思考")) {
                agent->m_ThinkTimer = agent->thinkInterval;
                HE_CORE_INFO("[AgentScene] 手动触发一次思考");
            }
            ImGui::SameLine();
            if (ImGui::Button("撤销上一步")) {
                history.Undo();
                lastEntityCount = (int)world.GetEntityCount();
                HE_CORE_INFO("[AgentScene] 已撤销，实体数: {}", lastEntityCount);
            }
        }
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
    he::ai::AIModule::Shutdown();
    HE_CORE_INFO("[AgentScene] 退出");
    return 0;
}
