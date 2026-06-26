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
#include "Render/Pipeline/ForwardPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/Transform.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include <cmath>
#include <cstring>

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
    config.windowWidth  = 1920;
    config.windowHeight = 1080;
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

    HE_CORE_INFO("Scene created: {} entities", world.GetEntityCount());

    // --- 5. 初始化前向管线 ---
    render::ForwardPipeline pipeline;
    pipeline.Initialize(device.get());

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
    render::CameraData camera;
    camera.position = float3(0.0f, 2.0f, 8.0f);
    camera.forward  = float3(0.0f, -0.2f, -1.0f);
    camera.up       = float3(0.0f, 1.0f, 0.0f);
    camera.SetAspectRatio(
        static_cast<float>(swapchain->GetWidth()),
        static_cast<float>(swapchain->GetHeight()));

    // 从初始朝向反算 yaw / pitch
    float yaw   = std::atan2(camera.forward.x, -camera.forward.z);
    float pitch = std::asin(camera.forward.y);

    // 鼠标状态
    bool   rightMouseDown = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    float  moveSpeed  = 5.0f;      // 基础移动速度（单位/秒）
    float  lookSpeed  = 0.003f;    // 旋转灵敏度

    // --- 8. 窗口调整回调 ---
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        camera.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
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
                // 开始拖拽：记录初始位置，锁定光标
                rightMouseDown = true;
                glfwGetCursorPos(glfwWin, &lastMouseX, &lastMouseY);
                glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            } else if (!mouseDown && rightMouseDown) {
                // 释放拖拽：还原光标
                rightMouseDown = false;
                glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            } else if (mouseDown && rightMouseDown) {
                double cx, cy;
                glfwGetCursorPos(glfwWin, &cx, &cy);
                float dx = static_cast<float>(cx - lastMouseX);
                float dy = static_cast<float>(cy - lastMouseY);
                lastMouseX = cx;
                lastMouseY = cy;

                yaw   += dx * lookSpeed;   // 向右拖拽 → 视角右转
                pitch -= dy * lookSpeed;   // 向上拖拽 → 视角上抬
                // 限制俯仰角，避免万向锁
                pitch = glm::clamp(pitch, -1.5f, 1.5f);
            }

            // --- 计算移动速度 ---
            float speed = moveSpeed * deltaTime;
            if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
                speed *= 3.0f;  // Shift 加速
            }

            // --- WASD 移动 ---
            float3 right = glm::normalize(glm::cross(camera.forward, camera.up));
            float3 move  = float3(0.0f);
            if (glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS) move += camera.forward;
            if (glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS) move -= camera.forward;
            if (glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS) move -= right;
            if (glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS) move += right;
            if (glfwGetKey(glfwWin, GLFW_KEY_E) == GLFW_PRESS) move += camera.up;     // E = 上升
            if (glfwGetKey(glfwWin, GLFW_KEY_Q) == GLFW_PRESS) move -= camera.up;     // Q = 下降

            if (glm::dot(move, move) > 0.0001f) {
                move = glm::normalize(move) * speed;
                camera.position += move;
            }

            // --- 滚轮缩放 ---
            // （GLFW 滚轮通过回调获取，这里用简化方式：alt+W/S）
            // 留空，后续通过 ImGui / 输入系统完善
        }

        // 更新相机朝向
        float3 forward;
        forward.x = cos(pitch) * sin(yaw);
        forward.y = sin(pitch);
        forward.z = -cos(pitch) * cos(yaw);
        camera.forward = glm::normalize(forward);

        // 渲染一帧（顺序必须严格：Begin → BeginRenderPass → Viewport/Scissor → Draw → ImGui → EndRenderPass → End）
        cmdList->Begin();

        // 注意：BeginRenderPass 必须在 SetViewport/SetScissor 之前
        cmdList->BeginRenderPass(1, rhi::Format::RGBA8_UNORM);

        // Viewport/Scissor 必须在 RenderPass 内部设置（Vulkan 规范）
        pipeline.BeginFrame(cmdList.get(),
            swapchain->GetWidth(), swapchain->GetHeight());

        // 渲染场景
        pipeline.RenderScene(cmdList.get(), world, sceneGraph, camera);

        // --- ImGui（在同一渲染通道内绘制 — ImGui RP 现已与 Forward RP 兼容）---
        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("HugEngine");
        ImGui::Text("FPS: %.0f", 1.0f / (deltaTime > 0 ? deltaTime : 0.016f));
        ImGui::Text("Pos: (%.1f, %.1f, %.1f)",
            camera.position.x, camera.position.y, camera.position.z);
        ImGui::End();
        imgui.EndFrame(cmdList.get());

        cmdList->EndRenderPass();
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
                camera.position.x, camera.position.y, camera.position.z);
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
