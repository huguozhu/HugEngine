// ============================================================
// 02.Cube — HugEngine PBR 前向渲染管线演示
//
// 使用逐物体 Push Constants 的 PBR 渲染：
//   Engine → RHI Vulkan → ForwardPipeline → 场景遍历
//
// 创建多个带不同材质的立方体，展示 PBR 效果。
// 自由相机：WASD + 鼠标右键旋转
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/ForwardPipeline.h"
#include "Pipeline/CameraController.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/Transform.h"
#include "Scene/SkyboxComponent.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include <cmath>
#include <cstring>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace he;

// ============================================================
// 辅助：创建带材质的形状实体
// ============================================================
Entity CreateShapeEntity(World& world, SceneGraph& sg,
                         const float3& position, const float3& scale,
                         const float4& baseColor, float metallic, float roughness,
                         bool sphere = false)
{
    Entity e = world.CreateEntity(sphere ? "Sphere" : "Cube");

    auto* xform = world.AddComponent<TransformComponent>(e);
    xform->position = position;
    xform->scale    = scale;

    MeshComponent* mesh;
    if (sphere) {
        auto* sc = world.AddComponent<SphereComponent>(e);
        sc->radius = 0.5f;
        mesh = static_cast<MeshComponent*>(sc);
    } else {
        auto* cc = world.AddComponent<CubeComponent>(e);
        cc->halfExtent = 0.5f;
        mesh = static_cast<MeshComponent*>(cc);
    }

    mesh->baseColorFactor  = baseColor;
    mesh->metallicFactor   = metallic;
    mesh->roughnessFactor  = roughness;

    sg.SetParent(e, Entity{kInvalidEntity});
    return e;
}

// ============================================================
// 主函数
// ============================================================
int main() {
    // --- 1. 引擎启动 ---
    EngineConfig config;
    config.appName      = "HugEngine — PBR Forward Pipeline";
    config.windowWidth  = 960;
    config.windowHeight = 500;
    config.enableVSync  = true;

    Engine engine(config);
    engine.Initialize();

    // --- 2. 创建 RHI 设备 ---
    rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    auto device = rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    rhi::SetDevice(device.get());

    // --- 3. 创建 SwapChain ---
    auto swapchain = device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width  = engine.GetWindow()->GetWidth(),
        .height = engine.GetWindow()->GetHeight(),
        .vsync  = true,
    });

    // --- 4. 初始化场景 ---
    World world;
    SceneGraph sceneGraph(world);

    // 地板（深灰色立方体，粗糙）
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, -1.5f, 0.0f), float3(5.0f, 0.2f, 5.0f),
        float4(0.3f, 0.3f, 0.35f, 1.0f), 0.0f, 0.9f);

    // 金球（金属，光滑）
    CreateShapeEntity(world, sceneGraph,
        float3(-1.5f, 0.0f, 0.0f), float3(0.8f),
        float4(1.0f, 0.72f, 0.0f, 1.0f), 1.0f, 0.15f, true);

    // 铜球（金属，中度粗糙）
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, 0.0f, 0.0f), float3(0.8f),
        float4(0.85f, 0.45f, 0.2f, 1.0f), 0.95f, 0.4f, true);

    // 蓝色塑料立方体（非金属，光滑）
    CreateShapeEntity(world, sceneGraph,
        float3(1.5f, 0.0f, 0.0f), float3(0.8f),
        float4(0.2f, 0.5f, 1.0f, 1.0f), 0.0f, 0.2f);

    // 红色橡胶立方体（非金属，粗糙）
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, 0.0f, 1.5f), float3(0.7f),
        float4(0.9f, 0.15f, 0.1f, 1.0f), 0.0f, 0.85f);

    // 白色陶瓷球
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, 0.2f, -1.5f), float3(0.6f),
        float4(0.95f, 0.93f, 0.88f, 1.0f), 0.0f, 0.35f, true);

    // --- 创建光照 ---
    {
        Entity lightEntity = world.CreateEntity("DirectionalLight");
        world.AddComponent<TransformComponent>(lightEntity);
        auto* dl = world.AddComponent<DirectionalLight>(lightEntity);
        dl->direction = float3(0.5f, -1.0f, 1.0f);
        dl->color     = float3(1.0f, 0.95f, 0.85f);
        dl->intensity = 5.0f;
        sceneGraph.SetParent(lightEntity, Entity{kInvalidEntity});
    }

    // --- 天空盒（从 skybox 目录加载 6 面纹理）---
    {
        String skyDir = String(HUGE_CONTENT_DIR) + "Textures/skybox/";
        const char* faceFiles[6] = {
            "daylight0.png", "daylight1.png", "daylight2.png",
            "daylight3.png", "daylight4.png", "daylight5.png",
        };
        std::vector<u8> allFaces;
        u32 faceW = 0, faceH = 0;

        for (u32 f = 0; f < 6; ++f) {
            String path = skyDir + faceFiles[f];
            int w, h, ch;
            u8* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
            if (!pixels) { HE_CORE_WARN("Skybox face {} 加载失败: {}", f, path); break; }
            if (f == 0) { faceW = (u32)w; faceH = (u32)h; }
            usize byteSize = faceW * faceH * 4;
            allFaces.insert(allFaces.end(), pixels, pixels + byteSize);
            stbi_image_free(pixels);
        }

        if (!allFaces.empty()) {
            rhi::TextureDesc cmDesc;
            cmDesc.format=rhi::Format::RGBA8_UNORM; cmDesc.width=faceW; cmDesc.height=faceH;
            cmDesc.mipLevels=1; cmDesc.arrayLayers=6;
            cmDesc.usage=rhi::TextureUsage::ShaderResource|rhi::TextureUsage::Cubemap|rhi::TextureUsage::TransferDst;
            cmDesc.initialData=allFaces.data();
            auto cm = device->CreateTexture(cmDesc);
            rhi::SamplerDesc s; s.minFilter=s.magFilter=rhi::FilterMode::Linear;
            s.addressU=s.addressV=s.addressW=rhi::AddressMode::ClampToEdge;
            auto cs = device->CreateSampler(s);
            Entity e = world.CreateEntity("Skybox");
            world.AddComponent<TransformComponent>(e);
            auto* sc = world.AddComponent<SkyboxComponent>(e);
            sc->SetCubemap(std::move(cm), std::move(cs));
            sceneGraph.SetParent(e, Entity{kInvalidEntity});
        }
    }

    HE_CORE_INFO("Scene created: {} entities", world.GetEntityCount());

    // --- 5. 初始化前向管线 ---
    render::ForwardPipeline pipeline;
    pipeline.Initialize(device.get());
    pipeline.SetUseRenderGraph(true);
    pipeline.SetSwapChain(swapchain.get());

    // --- 6. 创建命令列表 ---
    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());
    cmdList->SetPipeline(pipeline.GetPipelineState());

    // --- 6.5. 获取 GLFW 窗口句柄 ---
    GLFWwindow* glfwWin = engine.GetWindow()->GetNativeHandle();

    // --- 6.6. ImGui ---
    editor::ImGuiIntegration imgui;
    imgui.Initialize(glfwWin, device.get(), swapchain.get());

    // --- 7. 相机 ---
    render::CameraController camCtrl;
    camCtrl.SetAspectRatio(
        static_cast<float>(swapchain->GetWidth()),
        static_cast<float>(swapchain->GetHeight()));

    // 鼠标状态
    bool   rightMouseDown = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;

    // --- 8. 窗口调整回调 ---
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (w == 0 || h == 0) return;
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        pipeline.ResizeHDRTarget(w, h);
        camCtrl.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // --- 9. 主循环 ---
    HE_CORE_INFO("02.Cube demo started — WASD=移动, 右键拖拽=旋转, 滚轮=缩放, Shift=加速");
    u64  frameIndex = 0;
    f64  lastTime   = glfwGetTime();

    while (!engine.GetWindow()->ShouldClose()) {
        // 计算帧时间
        f64 now       = glfwGetTime();
        f32 deltaTime = static_cast<f32>(now - lastTime);
        lastTime      = now;

        engine.GetWindow()->PollEvents();

        if (!swapchain->AcquireNextImage())
            continue;

        // ============================================================
        // 相机控制
        // ============================================================
        {
            // --- 鼠标旋转（右键拖拽）---
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
                float dx = static_cast<float>(cx - lastMouseX);
                float dy = static_cast<float>(cy - lastMouseY);
                lastMouseX = cx;
                lastMouseY = cy;

                camCtrl.Rotate(dx * 0.003f, -dy * 0.003f);  // 上推鼠标(dy<0) → pitch增大
            }

            // --- 键盘移动 ---
            render::CameraController::MoveInput moveIn;
            moveIn.forward  = glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS;
            moveIn.backward = glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS;
            moveIn.left     = glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS;
            moveIn.right    = glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS;
            moveIn.up       = glfwGetKey(glfwWin, GLFW_KEY_E) == GLFW_PRESS;
            moveIn.down     = glfwGetKey(glfwWin, GLFW_KEY_Q) == GLFW_PRESS;
            moveIn.sprint   = glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;

            camCtrl.Update(deltaTime, moveIn);
        }

        // 渲染一帧
        cmdList->Begin();
        pipeline.NextFrame();

        if (pipeline.UseRenderGraph()) {
            // RenderGraph 声明式路径：HDR → ToneMap 全部在 Graph 内完成
            pipeline.PrepareGI(cmdList.get(), world, sceneGraph);
            pipeline.Render(cmdList.get(), world, sceneGraph, camCtrl.GetCamera());
        } else {
            // 命令式路径（兼容）
            pipeline.PrepareGI(cmdList.get(), world, sceneGraph);
            pipeline.BeginHDRPass(cmdList.get(),
                swapchain->GetWidth(), swapchain->GetHeight());
            pipeline.BeginFrame(cmdList.get(),
                swapchain->GetWidth(), swapchain->GetHeight());
            pipeline.RenderScene(cmdList.get(), world, sceneGraph, camCtrl.GetCamera());
            pipeline.RenderSkybox(cmdList.get(), world, camCtrl.GetCamera());
            pipeline.EndHDRPass(cmdList.get());
            cmdList->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
            pipeline.RenderToneMapPass(cmdList.get());
        }

        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("HugEngine");
        ImGui::Text("FPS: %.0f", 1.0f / (deltaTime > 0 ? deltaTime : 0.016f));
        ImGui::Text("Pos: (%.1f, %.1f, %.1f)",
            camCtrl.GetCamera().position.x, camCtrl.GetCamera().position.y, camCtrl.GetCamera().position.z);

        // GI 控制
        auto* gi = pipeline.GetGI();
        if (gi) {
            ImGui::SeparatorText("GI");
            auto settings = gi->GetSettings();
            ImGui::Text("Mode: %s", settings.mode == render::GIMode::IBL ? "IBL" : "None");
            float intensity = settings.intensity;
            if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 3.0f, "%.2f")) {
                settings.intensity = intensity;
                gi->SetSettings(settings);
            }
        }

        ImGui::End();
        imgui.EndFrame(cmdList.get());

        if (!pipeline.UseRenderGraph()) {
            cmdList->EndRenderPass();  // 命令式：关闭 swapchain RP
        }
        cmdList->End();

        device->Submit(cmdList.get());
        swapchain->Present(true);
        frameIndex++;

        // 每秒更新窗口标题，显示 FPS
        static f64  titleTimer  = 0.0;
        static u64  titleFrame  = 0;
        titleTimer += deltaTime;
        titleFrame++;
        if (titleTimer >= 0.5) {
            f64 fps = static_cast<f64>(titleFrame) / titleTimer;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "HugEngine — PBR | FPS: %.0f | Pos: (%.1f, %.1f, %.1f) "
                "| 右键拖拽旋转 WASD移动",
                fps,
                camCtrl.GetCamera().position.x, camCtrl.GetCamera().position.y, camCtrl.GetCamera().position.z);
            glfwSetWindowTitle(glfwWin, buf);
            titleTimer = 0.0;
            titleFrame = 0;
        }
    }

    // 清理
    imgui.Shutdown();
    device->WaitIdle();
    pipeline.Shutdown();

    HE_CORE_INFO("Exiting after {} frames", frameIndex);
    return 0;
}
