// ============================================================
// 03.Sponza — 加载 Sponza glTF 场景，自由相机漫游
//
// 使用 glTFLoader (cgltf) 加载完整的 Sponza 场景，
// PBR 前向管线渲染，支持：
//   WASD = 移动, 右键拖拽 = 旋转视角
//   Shift = 加速, E/Q = 上升/下降
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Render/Pipeline/ForwardPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Asset/glTFLoader.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include <cmath>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace he;

int main() {
    // ============================================================
    // 1. 引擎启动
    // ============================================================
    EngineConfig config;
    config.appName      = "HugEngine — 03.Sponza";
    config.windowWidth  = 960;
    config.windowHeight = 540;
    config.enableVSync  = true;

    Engine engine(config);
    engine.Initialize();

    // ============================================================
    // 2. 创建 Vulkan RHI 设备
    // ============================================================
    rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    auto device = rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    rhi::SetDevice(device.get());

    // ============================================================
    // 3. 创建 SwapChain
    // ============================================================
    auto swapchain = device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width  = engine.GetWindow()->GetWidth(),
        .height = engine.GetWindow()->GetHeight(),
        .vsync  = true,
    });

    // ============================================================
    // 4. 初始化场景 + 加载 Sponza glTF
    // ============================================================
    World world;
    SceneGraph sceneGraph(world);

    // Content 目录由 CMake 编译定义提供（解决不同工作目录下的路径问题）
    String sponzaPath = String(HUGE_CONTENT_DIR) + "gltf/Sponza/glTF/Sponza.gltf";
    HE_CORE_INFO("加载 Sponza 场景: {}", sponzaPath);

    auto result = asset::LoadGLTF(world, sceneGraph, sponzaPath);
    if (!result.success) {
        HE_CORE_ERROR("Sponza 加载失败: {}", result.error);
        return 1;
    }
    HE_CORE_INFO("Sponza 加载完成: {} 实体, {} 网格图元",
                 result.entities.size(), result.meshCount);

    // --- 添加方向光 ---
    {
        Entity lightEntity = world.CreateEntity("DirectionalLight");
        world.AddComponent<TransformComponent>(lightEntity);
        auto* dl = world.AddComponent<DirectionalLight>(lightEntity);
        dl->direction = float3(0.4f, -1.0f, 0.6f);
        dl->color     = float3(1.0f, 0.95f, 0.85f);
        dl->intensity = 8.0f;
        sceneGraph.SetParent(lightEntity, Entity{kInvalidEntity});
    }

    // --- 添加半球环境光补光 ---
    {
        Entity lightEntity = world.CreateEntity("FillLight");
        world.AddComponent<TransformComponent>(lightEntity);
        auto* dl = world.AddComponent<DirectionalLight>(lightEntity);
        dl->direction = float3(-0.3f, -0.4f, -0.5f);
        dl->color     = float3(0.6f, 0.7f, 0.9f);
        dl->intensity = 2.0f;
        sceneGraph.SetParent(lightEntity, Entity{kInvalidEntity});
    }

    HE_CORE_INFO("场景就绪: {} 实体", world.GetEntityCount());

    // ============================================================
    // 5. 初始化前向管线
    // ============================================================
    render::ForwardPipeline pipeline;
    pipeline.Initialize(device.get());

    // ============================================================
    // 6. 创建命令列表
    // ============================================================
    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());
    cmdList->SetPipeline(pipeline.GetPipelineState());

    // ============================================================
    // 7. ImGui 初始化
    // ============================================================
    GLFWwindow* glfwWin = engine.GetWindow()->GetNativeHandle();

    editor::ImGuiIntegration imgui;
    imgui.Initialize(glfwWin, device.get(), swapchain.get());

    // ============================================================
    // 8. 相机 — 初始位置适配 Sponza 室内场景
    // ============================================================
    render::CameraData camera;
    camera.position = float3(0.0f, 3.0f, 0.0f);
    camera.forward  = float3(0.0f, -0.1f, -1.0f);
    camera.up       = float3(0.0f, 1.0f, 0.0f);
    camera.SetAspectRatio(
        static_cast<float>(swapchain->GetWidth()),
        static_cast<float>(swapchain->GetHeight()));

    float yaw   = std::atan2(camera.forward.x, -camera.forward.z);
    float pitch = std::asin(camera.forward.y);

    // 鼠标状态
    bool   rightMouseDown = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    float  moveSpeed  = 8.0f;      // Sponza 场景较大，速度稍快
    float  lookSpeed  = 0.003f;

    // ============================================================
    // 9. 窗口调整回调
    // ============================================================
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        camera.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // ============================================================
    // 10. 主渲染循环
    // ============================================================
    HE_CORE_INFO("03.Sponza 启动 — WASD=移动, 右键拖拽=旋转, Shift=加速, E/Q=升降");
    u64 frameIndex = 0;
    f64 lastTime   = glfwGetTime();

    while (!engine.GetWindow()->ShouldClose()) {
        f64 now       = glfwGetTime();
        f32 deltaTime = static_cast<f32>(now - lastTime);
        lastTime      = now;

        engine.GetWindow()->PollEvents();

        if (!swapchain->AcquireNextImage())
            continue;

        // --- 相机控制 ---
        {
            // 右键拖拽旋转
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

                yaw   += dx * lookSpeed;
                pitch -= dy * lookSpeed;
                pitch  = glm::clamp(pitch, -1.5f, 1.5f);
            }

            // 移动速度
            float speed = moveSpeed * deltaTime;
            if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
                speed *= 3.0f;

            // WASD + E/Q 移动
            float3 right = glm::normalize(glm::cross(camera.forward, camera.up));
            float3 move  = float3(0.0f);
            if (glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS) move += camera.forward;
            if (glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS) move -= camera.forward;
            if (glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS) move -= right;
            if (glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS) move += right;
            if (glfwGetKey(glfwWin, GLFW_KEY_E) == GLFW_PRESS) move += camera.up;
            if (glfwGetKey(glfwWin, GLFW_KEY_Q) == GLFW_PRESS) move -= camera.up;

            if (glm::dot(move, move) > 0.0001f) {
                move = glm::normalize(move) * speed;
                camera.position += move;
            }
        }

        // 更新相机朝向
        float3 forward;
        forward.x = cos(pitch) * sin(yaw);
        forward.y = sin(pitch);
        forward.z = -cos(pitch) * cos(yaw);
        camera.forward = glm::normalize(forward);

        // --- 渲染 ---
        cmdList->Begin();
        cmdList->BeginRenderPass(1, rhi::Format::RGBA8_UNORM);

        pipeline.BeginFrame(cmdList.get(),
            swapchain->GetWidth(), swapchain->GetHeight());
        pipeline.RenderScene(cmdList.get(), world, sceneGraph, camera);

        // ImGui（在同一渲染通道内绘制 — ImGui RP 现已与 Forward RP 兼容）
        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("03.Sponza");
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
    }

    // 清理
    imgui.Shutdown();
    device->WaitIdle();
    pipeline.Shutdown();

    HE_CORE_INFO("03.Sponza 退出 ({} 帧)", frameIndex);
    return 0;
}
