// ============================================================
// 08.TextureGen — 文生纹理（G2.1 资产生成验证）
//
// 验证「生成 = 标准资产」闭环：
//   1. TextToTexture：LLM 输出纹理规格（有 key）→ 程序化生成像素 → 写盘 PNG
//      （无 key 时降级：用内置规格直接生成，演示资产管线）
//   2. stbi_load 读回生成的 PNG（与 glTF 导入纹理同一条加载管线）
//   3. 创建 GPU 纹理 → WrapRHITexture → texture_sample 推理 → 读回平均色
// 结果与状态显示在 ImGui 面板。
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
#include "AI/AIGC/TextureGenerator.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <filesystem>
#include <vector>

using namespace he;

int main() {
#ifdef _WIN32
    // 本工程以 /utf-8 编译，日志为 UTF-8 字节；控制台代码页切到 UTF-8，避免中文乱码
    SetConsoleOutputCP(CP_UTF8);
#endif

    // --- 1. 引擎 + RHI + SwapChain ---
    EngineConfig config;
    config.appName       = "HugEngine — 文生纹理（G2.1）";
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

    // --- 3. 文生纹理（生成 = 标准资产）---
    // 输出路径：Content/Generated/tex_gen_checker.png（与 glTF 纹理同目录）
    String outPath = String(HUGE_CONTENT_DIR) + "Generated/tex_gen_checker.png";
    std::filesystem::path outP(outPath);
    std::error_code ec;
    std::filesystem::create_directories(outP.parent_path(), ec);

    he::ai::aigc::TextureGenResult genResult;
    const String kPrompt = "棋盘纹理：蓝色与红色相间，64 像素";
    if (caps.supportsRemoteLLM) {
        // 有 key：LLM 输出规格
        he::ai::aigc::GenerativeAssetFactory factory;
        genResult = factory.TextToTexture(*aiDevice, kPrompt, outPath);
    } else {
        // 无 key：降级用内置规格（演示资产管线本身）
        HE_CORE_WARN("[TextureGen] 未设置 DEEPSEEK_API_KEY，用内置规格演示（LLM 规格需 key）");
        he::ai::aigc::TextureSpec spec;
        spec.pattern = "checker";
        spec.size    = 64;
        spec.colorA  = float3(0.2f, 0.4f, 0.9f);   // 蓝
        spec.colorB  = float3(0.9f, 0.2f, 0.2f);   // 红
        genResult.success = true;
        genResult.width = genResult.height = spec.size;
        genResult.error = "降级模式";
        he::ai::aigc::TextureGenerator::Generate(spec, genResult.pixels);
        if (he::ai::aigc::TextureGenerator::WritePNG(outPath, spec.size, spec.size,
                                                     genResult.pixels.data()))
            genResult.path = outPath;
    }

    // --- 4. 标准加载管线读回生成的 PNG → GPU 纹理 ---
    std::unique_ptr<rhi::IRHITexture> generatedTex;
    bool loadedOk = false;
    if (genResult.success && !genResult.path.empty()) {
        int w = 0, h = 0, ch = 0;
        u8* pixels = stbi_load(genResult.path.c_str(), &w, &h, &ch, 4);
        if (pixels) {
            rhi::TextureDesc tDesc;
            tDesc.format = rhi::Format::RGBA8_UNORM;
            tDesc.width  = (u32)w;
            tDesc.height = (u32)h;
            tDesc.mipLevels = 1;
            tDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::TransferDst;
            tDesc.initialData = pixels;
            generatedTex = device->CreateTexture(tDesc);
            stbi_image_free(pixels);
            loadedOk = generatedTex != nullptr;
        }
    }

    // --- 5. 生成纹理 → AI 推理消费（texture_sample）→ 读回平均色 ---
    float avgR = 0, avgG = 0, avgB = 0;
    bool inferOk = false;
    if (loadedOk && caps.supportsGPU) {
        auto texTensor = aiDevice->WrapRHITexture(generatedTex.get());
        he::ai::AITensorDesc desc;
        desc.elementCount = genResult.width * genResult.height * 4;
        desc.dtype        = he::ai::AIDataType::FP32;
        auto out = aiDevice->CreateTensor(desc);
        if (texTensor && out) {
            he::ai::InferenceRequest req;
            req.kernel = he::ai::kKernelTextureSample;
            req.params = {{"brightness", "1.0"}};
            req.textureInputs.push_back(texTensor.get());
            req.outputs.push_back(out.get());
            auto inf = aiDevice->Submit(std::move(req));
            if (inf) {
                std::vector<float> data(genResult.width * genResult.height * 4);
                if (aiDevice->ReadTensor(out.get(), Span<float>(data))) {
                    double sr = 0, sg = 0, sb = 0;
                    for (u32 i = 0; i < genResult.width * genResult.height; ++i) {
                        sr += data[i * 4];
                        sg += data[i * 4 + 1];
                        sb += data[i * 4 + 2];
                    }
                    u32 n = genResult.width * genResult.height;
                    avgR = (float)(sr / n);
                    avgG = (float)(sg / n);
                    avgB = (float)(sb / n);
                    inferOk = true;
                }
            }
        }
    }

    // --- 6. 简单场景 ---
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

    // --- 7. 渲染管线 + 相机 + ImGui ---
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

    // --- 8. 主循环 ---
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
        ImGui::Begin("文生纹理 G2.1");
        ImGui::Text("生成: %s", genResult.success ? "成功" : "失败");
        if (!genResult.error.empty())
            ImGui::TextWrapped("说明: %s", genResult.error.c_str());
        ImGui::Text("Prompt: %s", kPrompt.c_str());
        ImGui::Text("资产文件: %s", genResult.path.empty() ? "(未写盘)" : genResult.path.c_str());
        ImGui::Text("尺寸: %u x %u", genResult.width, genResult.height);
        ImGui::Text("标准加载(stbi): %s", loadedOk ? "通过" : "失败");
        ImGui::Text("AI 推理消费: %s", inferOk ? "通过" : "失败");
        if (inferOk) {
            ImGui::Text("平均色: (%.2f, %.2f, %.2f)", avgR, avgG, avgB);
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
    HE_CORE_INFO("[TextureGen] 退出");
    return 0;
}
