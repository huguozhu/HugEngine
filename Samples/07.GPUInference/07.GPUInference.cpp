// ============================================================
// 07.GPUInference — GPU 推理后端最小验证（A3.1）
//
// 验证 IAIDevice → GPUBackend → RHI compute 全链路，两种核来源：
//   A. 内置核：kernel="tensor_scale" + 参数 scale=2.5
//   B. 外部核：customSpirv=运行时传入的 SPIR-V（AITensorAdd，out=in+10）
// 结果与用时显示在 ImGui 面板。
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
#include "AI/Runtime/Backend/GPUBackend.h"   // kKernelTensorScale（内置核名）
#include "AITensorAdd.comp.spv.h"            // k_AITensorAdd_comp_spv（外部核 SPIR-V）
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>

using namespace he;

// 张量规模（每个推理用例的元素个数）
constexpr u32 kTensorCount = 4;

int main() {
    // --- 1. 引擎 + RHI + SwapChain ---
    EngineConfig config;
    config.appName       = "HugEngine — GPU 推理后端验证";
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

    // --- 2. AI 设备（带 GPU 后端：张量 + compute 推理）---
    he::ai::InferenceScheduler scheduler;
    auto aiDevice = he::ai::CreateAIDevice(&scheduler, device.get());
    const he::ai::AIDeviceCaps caps = aiDevice->GetCaps();

    // --- 3. GPU 推理全链路（一次性，结果供 UI 展示）---
    // 演示两种计算核来源：
    //   A. 内置核：kernel="tensor_scale" + 参数 {scale:2.5}
    //   B. 外部核：customSpirv=自定义 SPIR-V（AITensorAdd）+ 原始推参字节 {count, add=10}
    constexpr u32 kCount = 4;
    const float kScale = 2.5f, kAdd = 10.0f;

    // ── 推理 A：内置核（tensor_scale，out = in × 2.5）──
    float scaleIn[kCount]  = {1, 2, 3, 4};
    float scaleOut[kCount] = {0};
    bool  scaleOk = false;
    double scaleMs = 0.0;
    {
        he::ai::AITensorDesc desc;
        desc.elementCount = kCount;
        desc.dtype        = he::ai::AIDataType::FP32;
        auto in  = aiDevice->CreateTensor(desc);
        auto out = aiDevice->CreateTensor(desc);
        if (in && out && aiDevice->WriteTensor(in.get(), Span<const float>(scaleIn, kCount))) {
            he::ai::InferenceRequest req;
            req.kernel = he::ai::kKernelTensorScale;
            req.params = {{"scale", std::to_string(kScale)}};
            req.inputs.push_back(in.get());
            req.outputs.push_back(out.get());
            auto t0 = std::chrono::high_resolution_clock::now();
            auto inf = aiDevice->Submit(std::move(req));
            auto t1 = std::chrono::high_resolution_clock::now();
            scaleMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            scaleOk = inf && aiDevice->ReadTensor(out.get(), Span<float>(scaleOut, kCount));
        }
        // 校验：out == in × kScale
        for (u32 i = 0; i < kCount; ++i)
            if (scaleOut[i] != scaleIn[i] * kScale) { scaleOk = false; break; }
        HE_CORE_INFO("[GPUInference] 内置核: {} ({:.2f} ms)",
                     scaleOk ? "✅ [1..4] ×2.5 → [2.5,5,7.5,10]" : "❌ 失败", scaleMs);
    }

    // ── 推理 B：外部核（customSpirv = AITensorAdd 的 SPIR-V，out = in + 10）──
    float addIn[kCount]  = {1, 2, 3, 4};
    float addOut[kCount] = {0};
    bool  addOk = false;
    double addMs = 0.0;
    {
        he::ai::AITensorDesc desc;
        desc.elementCount = kCount;
        desc.dtype        = he::ai::AIDataType::FP32;
        auto in  = aiDevice->CreateTensor(desc);
        auto out = aiDevice->CreateTensor(desc);
        if (in && out && aiDevice->WriteTensor(in.get(), Span<const float>(addIn, kCount))) {
            // 推参原始字节：{u32 count, float add}（与 AITensorAdd.comp.slang 布局对齐）
            struct AddPC { u32 count; float add; } pc = { kCount, kAdd };
            he::ai::InferenceRequest req;
            req.customSpirv      = Span<const u32>(k_AITensorAdd_comp_spv);
            req.pushConstantSize = sizeof(pc);
            req.pushConstants.assign(reinterpret_cast<u8*>(&pc),
                                     reinterpret_cast<u8*>(&pc) + sizeof(pc));
            req.inputs.push_back(in.get());
            req.outputs.push_back(out.get());
            auto t0 = std::chrono::high_resolution_clock::now();
            auto inf = aiDevice->Submit(std::move(req));
            auto t1 = std::chrono::high_resolution_clock::now();
            addMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            addOk = inf && aiDevice->ReadTensor(out.get(), Span<float>(addOut, kCount));
        }
        // 校验：out == in + kAdd
        for (u32 i = 0; i < kCount; ++i)
            if (addOut[i] != addIn[i] + kAdd) { addOk = false; break; }
        HE_CORE_INFO("[GPUInference] 外部核: {} ({:.2f} ms)",
                     addOk ? "✅ [1..4] +10 → [11,12,13,14]" : "❌ 失败", addMs);
    }

    // --- 4. 简单场景（地面+光源+天空）便于观察 ---
    World world;
    SceneGraph sceneGraph(world);
    {
        Entity e = world.CreateEntity("Ground");
        auto* xform = world.AddComponent<TransformComponent>(e);
        xform->position = float3(0, -1, 0);
        xform->scale    = float3(20, 0.2f, 20);
        world.AddComponent<CubeComponent>(e);
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

    // --- 5. 前向管线 + 相机 + ImGui ---
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
    camCtrl.SetPosition(float3(0, 4, 12));
    camCtrl.SetOrientation(-1.57f, -0.2f);

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

        // 相机控制
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

        // 渲染（Forward）
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

        // ImGui：推理结果面板
        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("GPU 推理验证");
        ImGui::Text("后端能力: GPU=%s LLM=%s",
                    caps.supportsGPU ? "可用" : "不可用",
                    caps.supportsRemoteLLM ? "可用" : "不可用");
        ImGui::Separator();
        ImGui::Text("内置核 tensor_scale: %s (%.2f ms)",
                    scaleOk ? "✅ 通过" : "❌ 失败", scaleMs);
        if (scaleOk) {
            String inStr, outStr;
            for (u32 i = 0; i < kCount; ++i) {
                inStr  += (i ? ", " : "[") + std::to_string((int)scaleIn[i]);
                outStr += (i ? ", " : "[") + std::to_string((int)scaleOut[i]);
            }
            ImGui::TextWrapped("  %s → %s (×%.1f)", (inStr + "]").c_str(),
                               (outStr + "]").c_str(), kScale);
        }
        ImGui::Text("外部核 customSpirv(add): %s (%.2f ms)",
                    addOk ? "✅ 通过" : "❌ 失败", addMs);
        if (addOk) {
            String inStr, outStr;
            for (u32 i = 0; i < kCount; ++i) {
                inStr  += (i ? ", " : "[") + std::to_string((int)addIn[i]);
                outStr += (i ? ", " : "[") + std::to_string((int)addOut[i]);
            }
            ImGui::TextWrapped("  %s → %s (+%.0f)", (inStr + "]").c_str(),
                               (outStr + "]").c_str(), kAdd);
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
    HE_CORE_INFO("[GPUInference] 退出");
    return 0;
}
