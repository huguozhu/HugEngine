// Samples/Editor/EditorApp.cpp

#include "EditorApp.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/ForwardPipeline.h"
#include "ShaderHotReload.h"
#include "AntiAliasing/AA_None.h"
#include "AntiAliasing/AA_FXAA.h"
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
#include "Asset/glTFLoader.h"
#include "Asset/BindlessTextureManager.h"
#include "Core/Log.h"
#include <cctype>
#include <unordered_map>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace he;

// 拖放文件队列（GLFW 回调写入，主循环消费）
static std::vector<std::string> s_DroppedFiles;

// ���ͷ�ļ�
#include "Panels/OutlinerPanel.h"
#include "Panels/DetailsPanel.h"
#include "Panels/ViewportPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/ProjectSettingsPanel.h"
#include "Panels/StatsPanel.h"
#include "Panels/ConsolePanel.h"
#include "Panels/MaterialEditor.h"
#include "Panels/LevelLoader.h"
#include "Editor/CVar.h"

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

    m_Engine->GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        m_SwapChain->Resize(w, h);
        m_CmdList->SetSwapChain(m_SwapChain.get());
    });

    // 拖放导入回调
    glfwSetDropCallback(m_Window, [](GLFWwindow*, int count, const char** paths) {
        for (int i = 0; i < count; ++i) s_DroppedFiles.push_back(paths[i]);
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
    m_World->SetSceneGraph(m_SceneGraph.get());

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
// === 全局 CVar 注册（Project Settings 面板自动发现）===
he::CVar<float> cvCamSpeed  ("editor.camera.speed", 5.0f, "编辑器相机移速");
he::CVar<float> cvSnapGrid  ("editor.gizmo.snap_grid", 1.0f, "Gizmo 平移吸附网格");
he::CVar<float> cvSnapAngle ("editor.gizmo.snap_angle", 15.0f, "Gizmo 旋转吸附角度");
he::CVar<float> cvSnapScale ("editor.gizmo.snap_scale", 0.1f, "Gizmo 缩放吸附步长");
he::CVar<bool>  cvVSync     ("render.vsync", true, "垂直同步");
he::CVar<bool>  cvShadows   ("render.shadow_enabled", true, "阴影开关");
he::CVar<bool>  cvDebugVis  ("editor.debug.frustum", false, "Debug 视锥体显示");
he::CVar<String> cvPipeline ("render.pipeline", "forward", "渲染管线: forward / deferred (需重启)");
he::CVar<String> cvAAMode   ("render.aa_mode", "none", "抗锯齿: none / fxaa / taa");
he::CVar<bool>   cvExecIndirect("render.execute_indirect", true, "ExecuteIndirect: GPU Driven绘制");

void EditorApp::InitPipeline() {
    m_CmdList = m_Device->CreateCommandList();
    m_CmdList->SetSwapChain(m_SwapChain.get());

    m_Pipeline = std::make_unique<render::ForwardPipeline>();
    m_Pipeline->Initialize(m_Device.get());
    m_Pipeline->SetUseRenderGraph(false);
    m_Pipeline->SetSwapChain(m_SwapChain.get());
    m_Pipeline->OnResize(m_SwapChain->GetWidth(), m_SwapChain->GetHeight());
    m_CmdList->SetPipeline(m_Pipeline->GetPipelineState());
    HE_CORE_INFO("Pipeline: Forward (change via Project Settings CVar 'render.pipeline', requires restart)");

    // --- Shader 热重载 ---
    m_ShaderHotReload = std::make_unique<he::render::ShaderHotReload>();
    // 获取 shader 源文件目录和 slangc 路径
    String shaderDir  = "../../Engine/Shader/Shaders";     // 相对于 build/bin/Debug
    String slangcPath = "slangc";                           // 依赖 PATH 环境变量
    m_ShaderHotReload->Start(shaderDir, slangcPath,
        [this](const String& shaderName, const std::vector<u32>& spirv) {
            if (m_Pipeline) {
                int n = m_Pipeline->ReloadShader(shaderName, spirv);
                if (n > 0) {
                    HE_CORE_INFO("[HotReload] {} → 成功重载 {} 个 PSO", shaderName, n);
                }
            }
        });
    HE_CORE_INFO("Shader Hot Reload 已启动");
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
    m_Stats = std::make_unique<editor::StatsPanel>();
    m_Console = std::make_unique<editor::ConsolePanel>();
    m_MaterialEditor = std::make_unique<editor::MaterialEditor>();

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

        // Shader 热重载 — 每帧处理待重载队列
        if (m_ShaderHotReload) {
            m_ShaderHotReload->Poll();
        }

        // AA 模式切换（根据 CVar 动态创建/替换）
        static String s_LastAAMode = "none";
        String aaMode = cvAAMode.Get();
        if (aaMode != s_LastAAMode) {
            s_LastAAMode = aaMode;
            if (aaMode == "fxaa") {
                auto fxaa = std::make_unique<render::AA_FXAA>();
                fxaa->Initialize(m_Device.get(), m_SwapChain->GetWidth(), m_SwapChain->GetHeight());
                m_Pipeline->SetAntiAliasing(std::move(fxaa));
                HE_CORE_INFO("AA switched to FXAA");
            } else if (aaMode == "taa") {
                // TAA 仅 DeferredPipeline 支持（当前 Editor 用 ForwardPipeline）
                HE_CORE_WARN("TAA requires DeferredPipeline — falling back to None");
                m_Pipeline->SetAntiAliasing(std::make_unique<render::AA_None>());
                cvAAMode.Set("none");
                s_LastAAMode = "none";
            } else {
                auto none = std::make_unique<render::AA_None>();
                none->Initialize(m_Device.get(), m_SwapChain->GetWidth(), m_SwapChain->GetHeight());
                m_Pipeline->SetAntiAliasing(std::move(none));
                HE_CORE_INFO("AA switched to None");
            }
        }

        // ExecuteIndirect 开关
        m_Pipeline->SetUseExecuteIndirect(cvExecIndirect.Get());

        // 同步 LevelComponent（加载新的，无需每帧）
        he::editor::LevelLoader::SyncAll(*m_World);

        // 处理拖放导入队列
        if (!s_DroppedFiles.empty()) {
            for (auto& path : s_DroppedFiles) {
                String ext = path.substr(path.find_last_of('.') + 1);
                // 转小写比较
                for (auto& c : ext) c = static_cast<char>(std::tolower(c));
                if (ext == "glb" || ext == "gltf") {
                    HE_CORE_INFO("拖放导入 glTF: {}", path);
                    auto result = asset::LoadGLTF(*m_World, *m_SceneGraph, String(path));
                    if (result.success) {
                        HE_CORE_INFO("  导入成功: {} 实体", result.entities.size());
                        // 纹理基路径
                        String baseDir = (std::filesystem::path(path).parent_path() / "").string();
                        // 占位纹理（持久化）
                        if (!m_DefaultTex.get()) {
                            u8 w4[4]={255,255,255,255};
                            rhi::TextureDesc td; td.format=rhi::Format::RGBA8_UNORM;
                            td.width=td.height=1; td.mipLevels=1;
                            td.usage=rhi::TextureUsage::ShaderResource; td.initialData=w4;
                            m_DefaultTex = m_Device->CreateTexture(td);
                            rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
                            sd.addressU=sd.addressV=rhi::AddressMode::Repeat;
                            m_DefaultSampler = m_Device->CreateSampler(sd);
                            asset::BindlessTextureManager::Instance().SetDefaultTexture(
                                m_DefaultTex.get(), m_DefaultSampler.get());
                        }
                        m_World->ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent& mc) {
                            auto loadTex = [&](const String& uriStr) -> std::pair<rhi::IRHITexture*, rhi::IRHISampler*> {
                                if (uriStr.empty()) return {nullptr, nullptr};
                                String fullPath = (std::filesystem::path(baseDir) / uriStr).string();
                                auto it = m_TexCache.find(fullPath);
                                if (it != m_TexCache.end()) return {it->second.first.get(), it->second.second.get()};
                                int w, h, c;
                                u8* px = stbi_load(fullPath.c_str(), &w, &h, &c, 4);
                                if (!px) { HE_CORE_WARN("纹理加载失败: {}", fullPath); return {nullptr, nullptr}; }
                                rhi::TextureDesc td; td.format=rhi::Format::RGBA8_UNORM;
                                td.width=u32(w); td.height=u32(h); td.mipLevels=1;
                                td.usage=rhi::TextureUsage::ShaderResource; td.initialData=px;
                                auto tex = m_Device->CreateTexture(td);
                                rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
                                sd.addressU=sd.addressV=rhi::AddressMode::Repeat;
                                auto samp = m_Device->CreateSampler(sd);
                                stbi_image_free(px);
                                auto* tp = tex.get(); auto* sp = samp.get();
                                m_TexCache[fullPath] = {std::move(tex), std::move(samp)};
                                return {tp, sp};
                            };
                            auto [bc, bcs] = loadTex(mc.baseColorTexture);
                            auto [n, ns]   = loadTex(mc.normalTexture);
                            auto [mr, mrs] = loadTex(mc.metallicRoughnessTexture);
                            auto [ao, aos] = loadTex(mc.occlusionTexture);
                            mc.materialID = asset::BindlessTextureManager::Instance().RegisterMaterial(
                                bc, bcs, n, ns, mr, mrs, ao, aos);
                        });
                        // 选中并聚焦第一个导入实体
                        if (!result.entities.empty()) {
                            m_EditorCtx->SelectEntity(result.entities[0]);
                            // 聚焦相机到选中物体
                            auto* tf = m_World->GetComponent<TransformComponent>(result.entities[0]);
                            if (tf) m_Viewport->FocusOn(tf->position);
                        }
                    } else {
                        HE_CORE_WARN("  导入失败: {}", result.error);
                    }
                } else {
                    HE_CORE_INFO("拖放文件: {} (未处理的格式)", path);
                }
            }
            s_DroppedFiles.clear();
        }

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

        m_Pipeline->NextFrame();
        m_Pipeline->BeginFrame(m_CmdList.get(),
            m_SwapChain->GetWidth(), m_SwapChain->GetHeight());
        m_Viewport->SetViewportSize(m_SwapChain->GetWidth(), m_SwapChain->GetHeight());

        if (m_EditorCtx->IsPlaying() || m_EditorCtx->IsPaused()) {
            // Play/Paused 模式渲染
            if (m_EditorCtx->IsPlaying()) {
                m_World->Update(dt);
            }
            if (m_StepFrame) {
                m_World->Update(0.016f);
                m_EditorCtx->Pause();
                m_StepFrame = false;
            }
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
                ImGui::MenuItem("Material Editor", nullptr, &m_MaterialEditor->m_Visible);
                ImGui::EndMenu();
            }

            // Play/Pause/Stop 按钮（菜单栏右侧）
            ImGui::SameLine(ImGui::GetWindowWidth() - 150);
            if (m_EditorCtx->GetState() == he::editor::EditorContext::EditorState::Edit) {
                if (ImGui::Button("Play")) m_EditorCtx->Play();
            } else {
                bool playing = m_EditorCtx->IsPlaying();
                if (ImGui::Button(playing ? "Pause" : "Resume"))
                    playing ? m_EditorCtx->Pause() : m_EditorCtx->Play();
                ImGui::SameLine();
                if (m_EditorCtx->IsPaused()) {
                    if (ImGui::Button("Step")) { m_EditorCtx->Play(); m_StepFrame = true; }
                }
                ImGui::SameLine();
                if (ImGui::Button("Stop")) { m_EditorCtx->Stop(); m_StepFrame = false; }
            }
            ImGui::EndMainMenuBar();
        }

        // --- Play/Paused 覆盖层 / Edit 模式编辑器 UI ---
        if (m_EditorCtx->IsPlaying() || m_EditorCtx->IsPaused()) {
            ImGui::SetNextWindowBgAlpha(0.3f);
            ImGui::Begin("GameOverlay", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
            if (m_EditorCtx->IsPaused())
                ImGui::TextColored({1,1,0,1}, "PAUSED | FPS: %.0f | ESC: Stop | Step: advance 1 frame", 1.0f/dt);
            else
                ImGui::Text("FPS: %.0f | Press ESC to Stop", 1.0f / dt);
            ImGui::End();
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

            // Viewport（透明背景，3D 场景渲染到全窗口 backbuffer）
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0,0,0,0));
            ImGui::BeginChild("ViewportRegion", {viewportWidth, avail.y * 0.75f}, true);
            ImGui::PopStyleColor();
            // Gizmo 投影用全窗口坐标（3D 场景渲染到完整 backbuffer）
            m_Viewport->m_VP_Pos  = float2(0, 0);
            m_Viewport->m_VP_Size = float2(m_SwapChain->GetWidth(), m_SwapChain->GetHeight());
            // 点击检测用视口 child 的窗口区域
            ImVec2 wPos = ImGui::GetWindowPos();
            ImVec2 wSize = ImGui::GetWindowSize();
            m_Viewport->m_VP_ChildMin = float2(wPos.x, wPos.y);
            m_Viewport->m_VP_ChildMax = float2(wPos.x + wSize.x, wPos.y + wSize.y);
            m_Viewport->RenderGizmoOverlay();  // Gizmo 先处理，标记 hover/drag
            m_Viewport->HandleClickSelect();    // 再点击选中（跳过 gizmo 交互中的点击）
            m_Viewport->RenderDebugOverlay();
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
            u32 drawCount = m_Pipeline->GetLastDrawCount();
            u32 triCount  = m_Pipeline->GetLastTriCount();
            m_Stats->Render(dt, drawCount, triCount);
            m_Console->Render();
            m_MaterialEditor->Render();
        }

        m_ImGui->EndFrame(m_CmdList.get());

        m_CmdList->EndRenderPass();
        m_CmdList->End();

        m_Device->Submit(m_CmdList.get());
        m_SwapChain->Present(true);
    }
}

void EditorApp::Shutdown() {
    // 停止 Shader 热重载（必须在设备释放前停止）
    if (m_ShaderHotReload) {
        m_ShaderHotReload->Stop();
        m_ShaderHotReload.reset();
    }

    m_Device->WaitIdle();
    m_ImGui->Shutdown();
    // �?Engine 销毁前释放所有子系统（避免析构时 Logger 已失效）
    m_Pipeline.reset();
    m_SceneGraph.reset();
    m_World.reset();
    // 纹理缓存必须在 Device 之前释放（VMA 在 Device 内）
    m_TexCache.clear();
    m_DefaultTex.reset();
    m_DefaultSampler.reset();
    m_CmdList.reset();
    m_SwapChain.reset();
    m_Device.reset();
    // Engine ������ʱ����
}
