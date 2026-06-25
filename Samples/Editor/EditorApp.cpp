// Samples/Editor/EditorApp.cpp

#include "EditorApp.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Render/Pipeline/ForwardPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/LightComponent.h"
#include "Scene/CubeComponent.h"
#include "Editor/ImGuiIntegration.h"
#include "Editor/EditorContext.h"
#include "Editor/Command.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace he;

// 面板头文件
#include "Panels/OutlinerPanel.h"
#include "Panels/DetailsPanel.h"
#include "Panels/ViewportPanel.h"

EditorApp::EditorApp()  = default;
EditorApp::~EditorApp() = default;

int EditorApp::Run() {
    InitEngine();
    InitScene();
    InitPipeline();
    InitEditor();
    MainLoop();
    Shutdown();
    return 0;
}

void EditorApp::InitEngine() {
    // 创建引擎和窗口
    EngineConfig config;
    config.appName      = "HugEditor";
    config.windowWidth  = 1920;
    config.windowHeight = 1080;
    config.enableVSync  = true;

    m_Engine = new Engine(config);
    m_Engine->Initialize();
    m_Window = m_Engine->GetWindow()->GetNativeHandle();

    // 创建 RHI 设备
    rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = m_Engine->GetWindow()->GetNativeHandleRaw();

    m_Device = rhi::CreateDevice(rhiDesc.backend);
    m_Device->Initialize(rhiDesc);
    rhi::SetDevice(m_Device.get());

    // 创建 SwapChain
    m_SwapChain = m_Device->CreateSwapChain({
        .windowHandle = m_Engine->GetWindow()->GetNativeHandleRaw(),
        .width  = m_Engine->GetWindow()->GetWidth(),
        .height = m_Engine->GetWindow()->GetHeight(),
        .vsync  = true,
    });
}

void EditorApp::InitScene() {
    // 创建默认场景：地面 + 方向光
    m_World      = std::make_unique<World>();
    m_SceneGraph = std::make_unique<SceneGraph>(*m_World);

    // 地面
    Entity ground = m_World->CreateEntity("Ground");
    auto* gt = m_World->AddComponent<TransformComponent>(ground);
    gt->position = float3(0.0f, -1.5f, 0.0f);
    gt->scale    = float3(5.0f, 0.2f, 5.0f);
    auto* gc = m_World->AddComponent<CubeComponent>(ground);
    gc->halfExtent = 0.5f;
    gc->baseColorFactor  = float4(0.3f, 0.3f, 0.35f, 1.0f);
    gc->metallicFactor   = 0.0f;
    gc->roughnessFactor  = 0.9f;
    m_SceneGraph->SetParent(ground, {kInvalidEntity});

    // 方向光
    Entity lightEnt = m_World->CreateEntity("DirectionalLight");
    m_World->AddComponent<TransformComponent>(lightEnt);
    auto* dl = m_World->AddComponent<DirectionalLight>(lightEnt);
    dl->direction = float3(0.5f, -1.0f, 1.0f);
    dl->color     = float3(1.0f, 0.95f, 0.85f);
    dl->intensity = 5.0f;
    m_SceneGraph->SetParent(lightEnt, {kInvalidEntity});

    HE_CORE_INFO("Default scene created: {} entities", m_World->GetEntityCount());
}

void EditorApp::InitPipeline() {
    m_CmdList = m_Device->CreateCommandList();
    m_CmdList->SetSwapChain(m_SwapChain.get());

    m_Pipeline = std::make_unique<render::ForwardPipeline>();
    m_Pipeline->Initialize(m_Device.get());
    m_CmdList->SetPipeline(m_Pipeline->GetPipelineState());
}

void EditorApp::InitEditor() {
    m_CmdHistory = std::make_unique<CommandHistory>();

    m_EditorCtx = std::make_unique<editor::EditorContext>();
    m_EditorCtx->Initialize(m_World.get(), m_SceneGraph.get(), m_CmdHistory.get());

    m_ImGui = std::make_unique<editor::ImGuiIntegration>();
    m_ImGui->Initialize(m_Window, m_Device.get(), m_SwapChain.get());

    // 创建面板（当前为占位实现）
    m_Viewport  = std::make_unique<editor::ViewportPanel>();
    m_Outliner  = std::make_unique<editor::OutlinerPanel>();
    m_Details   = std::make_unique<editor::DetailsPanel>();

    m_Viewport->Initialize(m_EditorCtx.get(), m_Pipeline.get(), m_Window);
    m_Outliner->Initialize(m_EditorCtx.get());
    m_Details->Initialize(m_EditorCtx.get());

    m_LastTime = glfwGetTime();
}

void EditorApp::MainLoop() {
    while (!m_Engine->GetWindow()->ShouldClose()) {
        f64 now = glfwGetTime();
        f32 dt  = static_cast<f32>(now - m_LastTime);
        m_LastTime = now;

        m_Engine->GetWindow()->PollEvents();

        if (!m_SwapChain->AcquireNextImage())
            continue;

        // 渲染
        m_CmdList->Begin();
        m_CmdList->BeginRenderPass(1, rhi::Format::RGBA8_UNORM);

        m_Pipeline->BeginFrame(m_CmdList.get(),
            m_SwapChain->GetWidth(), m_SwapChain->GetHeight());

        // 场景渲染（由 ViewportPanel 驱动）
        m_Viewport->Render(m_CmdList.get());

        // UI
        m_ImGui->BeginFrame();

        // --- 菜单栏 ---
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                ImGui::MenuItem("New Scene", nullptr);
                ImGui::MenuItem("Open...", nullptr);
                ImGui::MenuItem("Save", nullptr);
                ImGui::Separator();
                ImGui::MenuItem("Exit", nullptr);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", nullptr,
                    m_CmdHistory->CanUndo())) {
                    m_CmdHistory->Undo();
                }
                if (ImGui::MenuItem("Redo", "Ctrl+Y", nullptr,
                    m_CmdHistory->CanRedo())) {
                    m_CmdHistory->Redo();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // 主布局：Viewport (左) | Details (右)
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float detailsWidth = 300.0f;
        float viewportWidth = avail.x - detailsWidth - 4.0f;

        // Viewport
        ImGui::BeginChild("ViewportRegion", {viewportWidth, avail.y * 0.75f}, true);
        // 3D 场景已由 ViewportPanel::Render 渲染到 BackBuffer
        // Phase 3-2: 改为渲染到 off-screen texture → ImGui::Image
        ImGui::EndChild();

        ImGui::SameLine();

        // Details
        ImGui::BeginChild("DetailsRegion", {detailsWidth, avail.y * 0.75f}, true);
        m_Details->Render();
        ImGui::EndChild();

        // Outliner（底部区域）
        ImGui::BeginChild("OutlinerRegion", {avail.x, avail.y * 0.25f - 4.0f}, true);
        m_Outliner->Render();
        ImGui::EndChild();

        // 状态栏
        ImGui::Begin("StatusBar");
        ImGui::Text("Selected: %d | FPS: %.0f",
            (int)m_EditorCtx->GetSelection().size(),
            1.0f / (dt > 0 ? dt : 0.016f));
        ImGui::End();

        m_ImGui->EndFrame(m_CmdList.get());

        m_CmdList->EndRenderPass();
        m_CmdList->End();

        m_Device->Submit(m_CmdList.get());
        m_SwapChain->Present(true);
    }
}

void EditorApp::Shutdown() {
    m_Device->WaitIdle();
    m_ImGui->Shutdown();
    m_Pipeline->Shutdown();
    // Engine 在析构时清理
    delete m_Engine;
}
