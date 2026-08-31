// ============================================================
// 09.MaterialGen — 文生材质 + GPU 纹理生成（G2.2 本地资产生成验证）
//
// 1. TextToMaterial：LLM 输出材质规格（有 key）→ 材质参数 + 可选纹理
//    （无 key 降级内置规格：金色金属），应用到场景方块（MeshComponent PBR 字段）
// 2. GPU 纹理生成核 texture_gen：经 IAIDevice 在 GPU 上生成棋盘纹理 →
//    读回像素 → 创建 GPU 纹理（本地 GPU 后端资产生成）
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
#include "AI/Runtime/AIDevice.h"
#include "AI/Runtime/InferenceScheduler.h"
#include "AI/Runtime/Backend/GPUBackend.h"
#include "AI/AIGC/GenerativeAssetFactory.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <vector>

using namespace he;

int main() {
#ifdef _WIN32
    // 本工程以 /utf-8 编译，日志为 UTF-8 字节；控制台代码页切到 UTF-8，避免中文乱码
    SetConsoleOutputCP(CP_UTF8);
#endif

    // --- 1. 引擎 + RHI + SwapChain ---
    EngineConfig config;
    config.appName       = "HugEngine — 文生材质（G2.2）";
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

    // --- 2. AI 设备（GPU + 远程 LLM）---
    he::ai::InferenceScheduler scheduler;
    auto aiDevice = he::ai::CreateAIDevice(&scheduler, device.get());
    const he::ai::AIDeviceCaps caps = aiDevice->GetCaps();

    // --- 3. 场景：两个方块（左=默认材质，右=AI 生成材质）---
    World world;
    SceneGraph sceneGraph(world);
    MeshComponent* generatedMesh = nullptr;
    {
        // 左方块（默认白色材质，对照）
        Entity e = world.CreateEntity("Cube_Default");
        auto* xform = world.AddComponent<TransformComponent>(e);
        xform->position = float3(-1.5f, 1.0f, 0.0f);
        auto* mesh = world.AddComponent<CubeComponent>(e);
        mesh->baseColorFactor = float4(1.0f, 1.0f, 1.0f, 1.0f);
        sceneGraph.SetParent(e, Entity{kInvalidEntity});
    }
    {
        // 右方块（应用 AI 生成材质）
        Entity e = world.CreateEntity("Cube_AIGen");
        auto* xform = world.AddComponent<TransformComponent>(e);
        xform->position = float3(1.5f, 1.0f, 0.0f);
        generatedMesh = world.AddComponent<CubeComponent>(e);
        sceneGraph.SetParent(e, Entity{kInvalidEntity});
    }
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

    // --- 4. 文生材质（生成 = 标准材质，应用到右方块）---
    he::ai::aigc::MaterialGenResult matResult;
    const String kPrompt = "磨砂金色金属材质，中等粗糙度";
    if (caps.supportsRemoteLLM) {
        he::ai::aigc::GenerativeAssetFactory factory;
        matResult = factory.TextToMaterial(*aiDevice, kPrompt, "");
    } else {
        // 无 key：降级内置规格（金色金属）
        HE_CORE_WARN("[MaterialGen] 未设置 DEEPSEEK_API_KEY，用内置规格演示（LLM 规格需 key）");
        matResult.success   = true;
        matResult.baseColor = float4(1.0f, 0.72f, 0.0f, 1.0f);
        matResult.metallic  = 1.0f;
        matResult.roughness = 0.25f;
        matResult.error     = "降级模式";
    }
    if (matResult.success && generatedMesh) {
        // 应用标准材质（MeshComponent PBR 字段）
        generatedMesh->baseColorFactor = matResult.baseColor;
        generatedMesh->metallicFactor  = matResult.metallic;
        generatedMesh->roughnessFactor = matResult.roughness;
        generatedMesh->emissiveFactor  = matResult.emissive;
        HE_CORE_INFO("[MaterialGen] 已应用生成材质: 基色({:.2f},{:.2f},{:.2f}) 金属{:.2f} 粗糙{:.2f}",
                     matResult.baseColor.x, matResult.baseColor.y, matResult.baseColor.z,
                     matResult.metallic, matResult.roughness);
    }

    // --- 5. GPU 纹理生成（texture_gen 核，本地 GPU 后端）---
    constexpr u32 kGenSize = 64;
    std::vector<float> genPixels(kGenSize * kGenSize * 4, 0.0f);
    bool genOk = false;
    if (caps.supportsGPU) {
        he::ai::AITensorDesc desc;
        desc.elementCount = kGenSize * kGenSize * 4;
        desc.dtype        = he::ai::AIDataType::FP32;
        auto out = aiDevice->CreateTensor(desc);
        if (out) {
            he::ai::InferenceRequest req;
            req.kernel = he::ai::kKernelTextureGen;
            req.params = {
                {"width",  "64"},
                {"height", "64"},
                {"pattern", "2"},                        // checker
                {"colorA", "0.1,0.5,0.9"},               // 蓝
                {"colorB", "0.9,0.6,0.1"},               // 橙
            };
            req.outputs.push_back(out.get());
            auto inf = aiDevice->Submit(std::move(req));
            genOk = inf && aiDevice->ReadTensor(out.get(), Span<float>(genPixels));
        }
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
        ImGui::Begin("文生材质 G2.2");
        ImGui::Text("材质生成: %s", matResult.success ? "成功" : "失败");
        if (!matResult.error.empty())
            ImGui::TextWrapped("说明: %s", matResult.error.c_str());
        ImGui::Text("Prompt: %s", kPrompt.c_str());
        if (matResult.success) {
            ImGui::Text("基色: (%.2f, %.2f, %.2f)", matResult.baseColor.x,
                        matResult.baseColor.y, matResult.baseColor.z);
            ImGui::Text("金属度: %.2f | 粗糙度: %.2f", matResult.metallic, matResult.roughness);
            ImGui::Text("应用对象: 右方块（左方块为默认材质对照）");
        }
        ImGui::Separator();
        ImGui::Text("GPU 纹理生成(texture_gen): %s", genOk ? "成功" : "失败");
        if (genOk) {
            // 统计读回像素的平均色（验证 GPU 生成内容）
            double sr = 0, sg = 0, sb = 0;
            for (u32 i = 0; i < kGenSize * kGenSize; ++i) {
                sr += genPixels[i * 4];
                sg += genPixels[i * 4 + 1];
                sb += genPixels[i * 4 + 2];
            }
            u32 n = kGenSize * kGenSize;
            ImGui::Text("平均色: (%.2f, %.2f, %.2f) 尺寸 %ux%u",
                        (float)(sr / n), (float)(sg / n), (float)(sb / n), kGenSize, kGenSize);
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
    HE_CORE_INFO("[MaterialGen] 退出");
    return 0;
}
