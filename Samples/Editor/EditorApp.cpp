// Samples/Editor/EditorApp.cpp

#include "EditorApp.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/ForwardPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/LightComponent.h"
#include "Scene/CubeComponent.h"
#include "Editor/ImGuiIntegration.h"
#include "Editor/EditorContext.h"
#include "Editor/Command.h"
#include "imgui.h"
#include "Editor/SceneSerializer.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace he;

// ���ͷ�ļ�
#include "Panels/OutlinerPanel.h"
#include "Panels/DetailsPanel.h"
#include "Panels/ViewportPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/ProjectSettingsPanel.h"

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
    // ��������ʹ���
    EngineConfig config;
    config.appName      = "HugEditor";
    config.windowWidth  = 1920;
    config.windowHeight = 1080;
    config.enableVSync  = true;

    m_Engine = std::make_unique<Engine>(config);
    m_Engine->Initialize();
    m_Window = m_Engine->GetWindow()->GetNativeHandle();

    // ���ڵ�����С�ص� �� �Զ��ؽ� SwapChain
    m_Engine->GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        m_SwapChain->Resize(w, h);
        m_CmdList->SetSwapChain(m_SwapChain.get());
    });

    // ���� RHI �豸
    rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = m_Engine->GetWindow()->GetNativeHandleRaw();

    m_Device = rhi::CreateDevice(rhiDesc.backend);
    m_Device->Initialize(rhiDesc);
    rhi::SetDevice(m_Device.get());

    // ���� SwapChain
    m_SwapChain = m_Device->CreateSwapChain({
        .windowHandle = m_Engine->GetWindow()->GetNativeHandleRaw(),
        .width  = m_Engine->GetWindow()->GetWidth(),
        .height = m_Engine->GetWindow()->GetHeight(),
        .vsync  = true,
    });
}

void EditorApp::InitScene() {
    // ����Ĭ�ϳ��������� + �����
    m_World      = std::make_unique<World>();
    m_SceneGraph = std::make_unique<SceneGraph>(*m_World);

    // ����
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

    // �����
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

    // ������壨��ǰΪռλʵ�֣�
    m_Viewport  = std::make_unique<editor::ViewportPanel>();
    m_Outliner  = std::make_unique<editor::OutlinerPanel>();
    m_Details   = std::make_unique<editor::DetailsPanel>();
    m_ContentBrowser = std::make_unique<editor::ContentBrowserPanel>();
    m_ProjectSettings = std::make_unique<editor::ProjectSettingsPanel>();

    m_Viewport->Initialize(m_EditorCtx.get(), m_Pipeline.get(), m_Window);
    m_Outliner->Initialize(m_EditorCtx.get());
    m_Details->Initialize(m_EditorCtx.get());
    m_ContentBrowser->Initialize(m_EditorCtx.get());
    m_ProjectSettings->Initialize(m_EditorCtx.get());

    m_LastTime = glfwGetTime();
}

void EditorApp::MainLoop() {
    while (!m_Engine->GetWindow()->ShouldClose()) {
        f64 now = glfwGetTime();
        f32 dt  = static_cast<f32>(now - m_LastTime);
        m_LastTime = now;

        m_Engine->GetWindow()->PollEvents();

        // Ctrl+Z / Ctrl+Y Undo/Redo ���̿�ݼ�
        // �� ImGui �ı�����򼤻�ʱ���������������ı��༭��ݼ���ͻ
        if (!ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z) && ImGui::GetIO().KeyCtrl) {
                m_CmdHistory->Undo();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Y) && ImGui::GetIO().KeyCtrl) {
                m_CmdHistory->Redo();
            }
        }

        if (!m_SwapChain->AcquireNextImage())
            continue;

        // --- 帧逻辑分叉：Play 模式用游戏渲染路径，Edit 模式用编辑器渲染路径 ---
        m_CmdList->Begin();
        m_CmdList->BeginRenderPass(1, rhi::Format::RGBA8_UNORM);

        // 每帧通用操作：设置视口尺寸（提升至分支外，避免重复调用）
        m_Pipeline->BeginFrame(m_CmdList.get(),
            m_SwapChain->GetWidth(), m_SwapChain->GetHeight());
        m_Viewport->SetViewportSize(m_SwapChain->GetWidth(), m_SwapChain->GetHeight());

        if (m_EditorCtx->IsPlaying()) {
            // Play 模式：更新世界 + 渲染游戏视图
            m_World->Update(dt);
            m_Viewport->RenderGameView(m_CmdList.get());
        } else {
            // Edit 模式：场景渲染由 ViewportPanel 管理
            m_Viewport->Render(m_CmdList.get());
        }

        // UI
        m_ImGui->BeginFrame();

        // --- �˵��� ---
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Scene")) {
                    // 清空场景（重置 World 和 SceneGraph）
                }
                if (ImGui::MenuItem("Open Scene...")) {
                    editor::SceneSerializer::Load("Content/Scenes/scene.hescene", *m_World, *m_SceneGraph);
                }
                if (ImGui::MenuItem("Save Scene As...")) {
                    editor::SceneSerializer::Save("Content/Scenes/scene.hescene", *m_World, *m_SceneGraph);
                }
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
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Content Browser", nullptr, &m_ContentBrowser->m_Visible);
                ImGui::MenuItem("Project Settings", nullptr, &m_ProjectSettings->m_Visible);
                ImGui::EndMenu();
            }

            // Play/Stop 按钮（菜单栏右侧）
            ImGui::SameLine(ImGui::GetWindowWidth() - 100);
            if (m_EditorCtx->IsPlaying()) {
                if (ImGui::Button("Stop")) m_EditorCtx->Stop();
            } else {
                if (ImGui::Button("Play")) m_EditorCtx->Play();
            }
            ImGui::EndMainMenuBar();
        }

        // --- Play 模式覆盖层 / Edit 模式编辑器 UI ---
        if (m_EditorCtx->IsPlaying()) {
            // 游戏覆盖层：FPS + 退出提示
            ImGui::SetNextWindowBgAlpha(0.3f);
            ImGui::Begin("GameOverlay", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("FPS: %.0f | Press ESC to Stop", 1.0f / dt);
            ImGui::End();
            // ESC 键退出 Play 模式
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                m_EditorCtx->Stop();
        } else {
            // 主面板窗口（填充菜单栏下方全部空间，无标题栏/无边框）
            ImGuiViewport* mainVP = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(mainVP->WorkPos);
            ImGui::SetNextWindowSize(mainVP->WorkSize);
            ImGui::Begin("EditorMain", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);

            // 整体布局：Viewport (左) | Details (右)
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float detailsWidth = 300.0f;
            float viewportWidth = avail.x - detailsWidth - 4.0f;

            // Viewport（透明背景，3D 场景仅在此区域可见）
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0,0,0,0));
            ImGui::BeginChild("ViewportRegion", {viewportWidth, avail.y * 0.75f}, true);
            ImGui::PopStyleColor();
            // 3D 场景由 ViewportPanel::Render 渲染到 BackBuffer
            // Phase 3-2: 改为渲染到 off-screen texture + ImGui::Image
            ImGui::EndChild();

            ImGui::SameLine();

            // Details（不透明白色背景，遮盖后方 3D 场景）
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(30,30,34,255));
            ImGui::BeginChild("DetailsRegion", {detailsWidth, avail.y * 0.75f}, true);
            m_Details->Render();
            ImGui::EndChild();
            ImGui::PopStyleColor();

            // Outliner（不透明白色背景，遮盖后方 3D 场景）
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(30,30,34,255));
            ImGui::BeginChild("OutlinerRegion", {avail.x, avail.y * 0.25f - 4.0f}, true);
            m_Outliner->Render();
            ImGui::EndChild();
            ImGui::PopStyleColor();

            // 状态栏
            ImGui::Text("Selected: %d | FPS: %.0f",
                (int)m_EditorCtx->GetSelection().size(),
                1.0f / (dt > 0 ? dt : 0.016f));

            ImGui::End(); // EditorMain

            // 浮动面板（自持窗口，不嵌入 EditorMain 布局）
            m_ContentBrowser->Render();
            m_ProjectSettings->Render();
        }

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
    // �?Engine 销毁前释放所有子系统（避免析构时 Logger 已失效）
    m_Pipeline.reset();
    m_SceneGraph.reset();
    m_World.reset();
    m_CmdList.reset();
    m_SwapChain.reset();
    m_Device.reset();
    // Engine ������ʱ����
}
