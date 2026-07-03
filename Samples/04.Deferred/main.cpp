// 04.Deferred — DeferredPipeline 延迟渲染演示
#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/DeferredPipeline.h"
#include "Pipeline/CameraController.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/Transform.h"
#include "Scene/LightComponent.h"
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

Entity CreateShape(World& w, SceneGraph& sg, const float3& pos, const float3& scale,
                   const float4& color, float metallic, float roughness, bool sphere = false) {
    Entity e = w.CreateEntity(sphere ? "Sphere" : "Cube");
    auto* xf = w.AddComponent<TransformComponent>(e);
    xf->position = pos; xf->scale = scale;
    MeshComponent* mesh;
    if (sphere) { auto* sc = w.AddComponent<SphereComponent>(e); sc->radius = 0.5f; mesh = sc; }
    else { auto* cc = w.AddComponent<CubeComponent>(e); cc->halfExtent = 0.5f; mesh = cc; }
    mesh->baseColorFactor = color; mesh->metallicFactor = metallic; mesh->roughnessFactor = roughness;
    sg.SetParent(e, Entity{kInvalidEntity});
    return e;
}

int main() {
    EngineConfig cfg; cfg.appName = "HugEngine — 04.Deferred"; cfg.windowWidth = 960; cfg.windowHeight = 500;
    Engine engine(cfg); engine.Initialize();

    rhi::DeviceInitDesc rd; rd.backend = rhi::Backend::Vulkan; rd.enableValidation = true;
    rd.windowHandle = engine.GetWindow()->GetNativeHandleRaw();
    auto device = rhi::CreateDevice(rd.backend); device->Initialize(rd); rhi::SetDevice(device.get());

    auto swapchain = device->CreateSwapChain({.windowHandle=rd.windowHandle,
        .width=engine.GetWindow()->GetWidth(), .height=engine.GetWindow()->GetHeight(), .vsync=true});

    World world; SceneGraph sg(world);
    // 场景几何
    CreateShape(world, sg, float3(0,-1.5f,0), float3(5,0.2f,5), float4(0.3f,0.3f,0.35f,1), 0, 0.9f);
    CreateShape(world, sg, float3(-1.5f,0,0), float3(0.8f), float4(1,0.72f,0,1), 1.0f, 0.15f, true);
    CreateShape(world, sg, float3(1.5f,0,0), float3(0.8f), float4(0.2f,0.5f,1,1), 0, 0.2f);
    CreateShape(world, sg, float3(0,0,1.5f), float3(0.7f), float4(0.9f,0.15f,0.1f,1), 0, 0.85f);
    CreateShape(world, sg, float3(0,0.2f,-1.5f), float3(0.6f), float4(0.95f,0.93f,0.88f,1), 0, 0.35f, true);
    // 光照
    Entity le = world.CreateEntity("DirLight");
    world.AddComponent<TransformComponent>(le);
    auto* dl = world.AddComponent<DirectionalLight>(le);
    dl->direction = {0.5,-1,1}; dl->color = {1,0.95,0.85}; dl->intensity = 5.0f;
    sg.SetParent(le, Entity{kInvalidEntity});

    render::DeferredPipeline pipeline;
    pipeline.Initialize(device.get());
    pipeline.SetSwapChain(swapchain.get());
    pipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());

    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());
    cmdList->SetPipeline(pipeline.GetToneMap()->GetPSO());  // 预设 BGRA8 RP，ImGui LoadOp 需要

    GLFWwindow* glfwWin = engine.GetWindow()->GetNativeHandle();
    editor::ImGuiIntegration imgui; imgui.Initialize(glfwWin, device.get(), swapchain.get());

    render::CameraController cam; cam.SetAspectRatio((float)swapchain->GetWidth(), (float)swapchain->GetHeight());
    bool rmb = false; double mx = 0, my = 0;

    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (w==0||h==0) return; swapchain->Resize(w, h); cmdList->SetSwapChain(swapchain.get());
        pipeline.OnResize(w, h); cam.SetAspectRatio((float)w, (float)h);
    });

    HE_CORE_INFO("04.Deferred started");
    f64 lt = glfwGetTime();
    while (!engine.GetWindow()->ShouldClose()) {
        f64 now = glfwGetTime(); f32 dt = (f32)(now - lt); lt = now;
        engine.GetWindow()->PollEvents();
        if (!swapchain->AcquireNextImage()) continue;

        bool md = glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        if (md && !rmb) { rmb = true; glfwGetCursorPos(glfwWin, &mx, &my); glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED); }
        else if (!md && rmb) { rmb = false; glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_NORMAL); }
        else if (md && rmb) { double cx, cy; glfwGetCursorPos(glfwWin, &cx, &cy); cam.Rotate((float)(cx-mx)*0.003f, -(float)(cy-my)*0.003f); mx=cx; my=cy; }
        render::CameraController::MoveInput mi;
        mi.forward=glfwGetKey(glfwWin,GLFW_KEY_W)==GLFW_PRESS; mi.backward=glfwGetKey(glfwWin,GLFW_KEY_S)==GLFW_PRESS;
        mi.left=glfwGetKey(glfwWin,GLFW_KEY_A)==GLFW_PRESS; mi.right=glfwGetKey(glfwWin,GLFW_KEY_D)==GLFW_PRESS;
        mi.up=glfwGetKey(glfwWin,GLFW_KEY_E)==GLFW_PRESS; mi.down=glfwGetKey(glfwWin,GLFW_KEY_Q)==GLFW_PRESS;
        mi.sprint=glfwGetKey(glfwWin,GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS;
        cam.Update(dt, mi);

        cmdList->Begin(); pipeline.NextFrame();
        pipeline.Render(cmdList.get(), world, sg, cam.GetCamera());

        cmdList->BeginRenderPass(1, rhi::Format::BGRA8_UNORM, rhi::Format::Unknown, nullptr,
            rhi::IRHICommandList::LoadOp::Load);
        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10,10}, ImGuiCond_Once); ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("04.Deferred"); ImGui::Text("FPS: %.0f", 1.0f/(dt>0?dt:0.016f)); ImGui::End();
        imgui.EndFrame(cmdList.get());
        cmdList->EndRenderPass(); cmdList->End();
        device->Submit(cmdList.get()); swapchain->Present(true);
    }
    imgui.Shutdown(); device->WaitIdle(); pipeline.Shutdown();
    return 0;
}
