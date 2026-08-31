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
#include "AI/Neural/NeuralUpscaler.h"        // 神经子系统（IRenderSubsystem 形态）
#include "AITensorAdd.comp.spv.h"            // k_AITensorAdd_comp_spv（外部核 SPIR-V）
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <vector>

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

    // --- 2.5 神经子系统（A3.2b：IRenderSubsystem 形态，内部经 IAIDevice 推理）---
    he::render::NeuralUpscaler neuralUpscaler(aiDevice.get());
    neuralUpscaler.Initialize(device.get(), swapchain->GetWidth(), swapchain->GetHeight());

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

    // ── 推理 C：零拷贝互操作（texture_sample：渲染纹理 → 张量 → 推理 → 缓冲）──
    // 创建 64×64 渐变纹理 → WrapRHITexture 包装（不拷贝）→ Submit 采样核 → 读回断言
    constexpr u32 kTexSize = 64;
    const float kBrightness = 2.0f;
    float texOut[kTexSize * kTexSize * 4] = {};
    bool  texOk = false;
    double texMs = 0.0;
    {
        // 渐变纹理（RGBA8）：r = x/63, g = y/63, b = 0.5, a = 1
        std::vector<u8> texData(kTexSize * kTexSize * 4);
        for (u32 y = 0; y < kTexSize; ++y)
            for (u32 x = 0; x < kTexSize; ++x) {
                u8* p = &texData[(y * kTexSize + x) * 4];
                p[0] = (u8)(x * 255 / (kTexSize - 1));
                p[1] = (u8)(y * 255 / (kTexSize - 1));
                p[2] = 128;
                p[3] = 255;
            }
        rhi::TextureDesc tDesc;
        tDesc.format = rhi::Format::RGBA8_UNORM;
        tDesc.width  = kTexSize;
        tDesc.height = kTexSize;
        tDesc.mipLevels = 1;
        tDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::TransferDst;
        tDesc.initialData = texData.data();
        auto tex = device->CreateTexture(tDesc);

        // 输出张量：kTexSize×kTexSize 个 float4
        he::ai::AITensorDesc desc;
        desc.elementCount = kTexSize * kTexSize * 4;
        desc.dtype        = he::ai::AIDataType::FP32;
        auto out = aiDevice->CreateTensor(desc);

        // 零拷贝包装纹理 → 提交采样核
        auto texTensor = aiDevice->WrapRHITexture(tex.get());
        if (texTensor && out) {
            he::ai::InferenceRequest req;
            req.kernel = he::ai::kKernelTextureSample;
            req.params = {{"brightness", std::to_string(kBrightness)}};
            req.textureInputs.push_back(texTensor.get());
            req.outputs.push_back(out.get());
            auto t0 = std::chrono::high_resolution_clock::now();
            auto inf = aiDevice->Submit(std::move(req));
            auto t1 = std::chrono::high_resolution_clock::now();
            texMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            texOk = inf && aiDevice->ReadTensor(out.get(), Span<float>(texOut, kTexSize * kTexSize * 4));
            // 校验（容差放宽，线性过滤下取像素中心附近）：左上角像素
            //   b 通道 = 0.5 × 2.0 = 1.0，a 通道 = 1.0 × 2.0 = 2.0，r 通道 ≈ 0
            if (texOk) {
                texOk = (std::abs(texOut[2] - 0.5f * kBrightness) < 0.05f) &&
                        (std::abs(texOut[3] - 1.0f * kBrightness) < 0.05f) &&
                        (std::abs(texOut[0] - 0.0f) < 0.05f);
            }
        }
        HE_CORE_INFO("[GPUInference] 零拷贝纹理核: {} ({:.2f} ms)",
                     texOk ? "✅ 纹理→推理→读回" : "❌ 失败", texMs);
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

        // ★ 神经子系统每帧驱动（Update 内经 IAIDevice 推理；Render 为预留 GPU 录制位）
        neuralUpscaler.Update(shadowCtx);
        neuralUpscaler.Render(cmdList.get());

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
                    scaleOk ? "通过" : "失败", scaleMs);
        if (scaleOk) {
            String inStr, outStr;
            for (u32 i = 0; i < kCount; ++i) {
                inStr  += (i ? ", " : "[") + std::to_string((int)scaleIn[i]);
                outStr += (i ? ", " : "[") + std::to_string((int)scaleOut[i]);
            }
            ImGui::TextWrapped("  %s -> %s (x%.1f)", (inStr + "]").c_str(),
                               (outStr + "]").c_str(), kScale);
        }
        ImGui::Text("外部核 customSpirv(add): %s (%.2f ms)",
                    addOk ? "通过" : "失败", addMs);
        if (addOk) {
            String inStr, outStr;
            for (u32 i = 0; i < kCount; ++i) {
                inStr  += (i ? ", " : "[") + std::to_string((int)addIn[i]);
                outStr += (i ? ", " : "[") + std::to_string((int)addOut[i]);
            }
            ImGui::TextWrapped("  %s -> %s (+%.0f)", (inStr + "]").c_str(),
                               (outStr + "]").c_str(), kAdd);
        }
        ImGui::Text("零拷贝纹理核 texture_sample: %s (%.2f ms)",
                    texOk ? "通过" : "失败", texMs);
        if (texOk) {
            ImGui::TextWrapped("  64x64 渐变纹理采样 x%.1f 后读回", kBrightness);
        }
        ImGui::Separator();
        // 神经子系统状态（IRenderSubsystem 形态，每帧经 IAIDevice 推理）
        ImGui::Text("神经子系统 NeuralUpscaler: %s",
                    neuralUpscaler.IsReady() ? "就绪" : "未初始化");
        static bool s_neuralEnabled = true;   // 开关镜像（Checkbox 需要 bool*）
        if (ImGui::Checkbox("启用", &s_neuralEnabled))
            neuralUpscaler.SetEnabled(s_neuralEnabled);
        ImGui::Text("累计推理: %u 次 | 上次用时: %.2f ms | 平均亮度: %.3f",
                    neuralUpscaler.GetInferenceCount(),
                    neuralUpscaler.GetLastInferenceMs(),
                    neuralUpscaler.GetLastAvgBrightness());
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
