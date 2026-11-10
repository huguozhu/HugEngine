// ============================================================
// 02.Cube.App.cpp —— 立方体 + 球体 + 天空盒（Forward 管线的可移植子集）
//
// 渲染顺序与 02 的 Forward 模式一致（阴影收集 → 快照装配 → Render → ToneMap → ImGui）：
//   NextFrame → ShadowSystem::Update → AssembleScene → ResolveLightShadowIndices
//   → ReserveOnce → ForwardPipeline::Render → BeginRenderPass + RenderToneMapPass
// 平台相关的东西只出现在 Engine / IWindow / InputState 三个入口里。
// ============================================================

#include "02.Cube.App.h"

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/IWindow.h"
#include "Platform/IVfs.h"
#include "RHI/RHI.h"
#include "RHI/FramePresenter.h"
#include "Pipeline/ForwardPipeline.h"
#include "Pipeline/CameraController.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Subsystem/RenderSubsystem.h"
#include "Shadow/IShadowSystem.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include "CameraInput.h"   // Samples/Common：消费 InputState 的相机控制
#include "SkyboxLoader.h"  // HMOS_App/Common：6 面 PNG 与 HDR 两套天空盒加载

#include <vector>


namespace he::samples::cube02 {
namespace {

Entity CreateShape(World& world, SceneGraph& sg, const char* name,
                   const float3& position, const float3& scale,
                   const float4& baseColor, float metallic, float roughness,
                   bool sphere) {
    Entity e = world.CreateEntity(name);
    auto* xform = world.AddComponent<TransformComponent>(e);
    xform->position = position;
    xform->scale    = scale;

    MeshComponent* mesh = nullptr;
    if (sphere) {
        auto* sc = world.AddComponent<SphereComponent>(e);
        sc->radius = 0.5f;
        mesh = static_cast<MeshComponent*>(sc);
    } else {
        auto* cc = world.AddComponent<CubeComponent>(e);
        cc->halfExtent = 0.5f;
        mesh = static_cast<MeshComponent*>(cc);
    }
    mesh->baseColorFactor = baseColor;
    mesh->metallicFactor  = metallic;
    mesh->roughnessFactor = roughness;
    sg.SetParent(e, Entity{ kInvalidEntity });
    return e;
}


} // namespace

struct App::Impl {
    Engine*                engine = nullptr;
    rhi::Backend           backend = rhi::Backend::Vulkan;
    Options                opt{};

    std::unique_ptr<rhi::IRHIDevice>        device;
    std::unique_ptr<rhi::IRHISwapChain>     swapchain;
    std::unique_ptr<rhi::IRHICommandList>   cmdList;
    std::unique_ptr<rhi::FramePresenter>    presenter;
    std::unique_ptr<render::ForwardPipeline> pipeline;
    std::unique_ptr<editor::ImGuiIntegration> imgui;

    World                          world;
    std::unique_ptr<SceneGraph>    sceneGraph;
    render::CameraController       camera;
    samples::CameraInput           camInput;

    bool ready = false;
    f32  frameDt = 0.016f;   // 本帧 deltaTime（面板 FPS 用）

    void BuildScene() {
        sceneGraph = std::make_unique<SceneGraph>(world);

        // 地面（压扁的立方体）+ 立方体 + 球体：足够验证 PBR / 阴影 / 天空盒
        CreateShape(world, *sceneGraph, "Ground", float3(0.0f, 0.0f, 0.0f), float3(12.0f, 0.1f, 12.0f),
                    float4(0.35f, 0.35f, 0.38f, 1.0f), 0.0f, 0.9f, /*sphere=*/false);
        CreateShape(world, *sceneGraph, "Cube", float3(-1.2f, 0.6f, 0.0f), float3(1.2f, 1.2f, 1.2f),
                    float4(0.85f, 0.25f, 0.2f, 1.0f), 0.1f, 0.35f, false);
        CreateShape(world, *sceneGraph, "Sphere", float3(1.2f, 0.7f, 0.0f), float3(1.4f, 1.4f, 1.4f),
                    float4(0.95f, 0.75f, 0.25f, 1.0f), 1.0f, 0.15f, true);

        // 方向光（开阴影，与 02 同参数）
        Entity lightEntity = world.CreateEntity("DirectionalLight");
        world.AddComponent<TransformComponent>(lightEntity);
        auto* dl = world.AddComponent<DirectionalLight>(lightEntity);
        dl->direction          = float3(0.5f, -1.0f, 1.0f);
        dl->color              = float3(1.0f, 0.95f, 0.85f);
        dl->intensity          = 5.0f;
        dl->castShadow         = true;
        dl->syncWithPhysicalSky = false;
        dl->shadowBias         = 0.0015f;
        dl->shadowNormalBias   = 0.05f;
        sceneGraph->SetParent(lightEntity, Entity{ kInvalidEntity });

        CreateSkyboxFromPngFaces(world, *sceneGraph, device.get());
        HE_CORE_INFO("[02.App] 场景就绪: {} 实体", world.GetEntityCount());
    }

    void BuildCamera() {
        camera.SetAspectRatio(static_cast<float>(swapchain->GetWidth()),
                              static_cast<float>(swapchain->GetHeight()));
        camera.SetMoveSpeed(6.0f);
        // 放在 -Z 方向看向原点（用"看向目标"避免依赖 yaw 正负号约定）
        const float3 camPos(0.0f, 2.0f, -6.0f);
        camera.SetPosition(camPos);
        camera.SetOrientationFromForward(glm::normalize(float3(0.0f, 1.2f, 0.0f) - camPos));
        camera.GetCamera().nearPlane = 0.1f;
        camera.GetCamera().farPlane  = 500.0f;
    }
};

App::App() : m_Impl(std::make_unique<Impl>()) {}
App::~App() { Shutdown(); }

bool App::Initialize(Engine& engine, rhi::Backend backend, const Options& opt) {
    Shutdown();
    Impl& g = *m_Impl;
    g.engine  = &engine;
    g.backend = backend;
    g.opt     = opt;

    // ---- 设备 + 交换链 ----
    rhi::DeviceInitDesc desc;
    desc.backend          = backend;
    desc.enableValidation = false;   // 手机/正式运行都不开验证层
    desc.windowHandle     = engine.GetWindow()->GetNativeWindow();
    g.device = rhi::CreateDevice(backend);
    g.device->Initialize(desc);
    rhi::SetDevice(g.device.get());

    g.swapchain = g.device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeWindow(),
        .width        = engine.GetWindow()->GetWidth(),
        .height       = engine.GetWindow()->GetHeight(),
        .vsync        = true,
    });

    // ---- 场景 + 相机 ----
    g.BuildScene();
    g.BuildCamera();

    // ---- Forward 管线（与 02 相同的配置）----
    g.pipeline = std::make_unique<render::ForwardPipeline>();
    g.pipeline->Initialize(g.device.get());
    g.pipeline->SetUseRenderGraph(false);
    g.pipeline->SetMultiThreadedRecording(false);
    g.pipeline->SetSwapChain(g.swapchain.get());
    g.pipeline->OnResize(g.swapchain->GetWidth(), g.swapchain->GetHeight());
    g.pipeline->GetGPUCulling().enabled = false;
    g.pipeline->GetSceneRenderer().enableFrustumCull = false;

    // ---- 命令列表 + 帧外壳 ----
    g.cmdList = g.device->CreateCommandList();
    g.cmdList->SetSwapChain(g.swapchain.get());
    g.cmdList->SetPipeline(g.pipeline->GetPipelineState());
    g.presenter = std::make_unique<rhi::FramePresenter>(g.swapchain.get());

    // ---- ImGui（鸿蒙 P2 阶段先关：imgui_impl_ohos 在 P3）----
    if (opt.enableImGui) {
        g.imgui = std::make_unique<editor::ImGuiIntegration>();
        g.imgui->Initialize(engine.GetWindow(), g.device.get(), g.swapchain.get());
    }

    g.ready = true;
    HE_CORE_INFO("[02.App] 初始化完成（后端 {}，{}x{}）",
                 static_cast<int>(backend), g.swapchain->GetWidth(), g.swapchain->GetHeight());
    // 【真机诊断】顶点布局真值：手机上若与 Windows 不一致，几何会"被打碎"
    HE_CORE_INFO("[02.App] StaticVertex sizeof={} pos={} normal={} uv={} maxPush={}",
                 sizeof(he::StaticVertex), offsetof(he::StaticVertex, position),
                 offsetof(he::StaticVertex, normal), offsetof(he::StaticVertex, uv),
                 static_cast<unsigned>(g.device->GetCaps().maxPushConstantsSize));
    return true;
}

bool App::Frame(f32 deltaTime, const InputState& input) {
    if (!BeginFrame(deltaTime, input)) return false;
    if (!RecordScene()) return false;
    RecordUIAndPresent();
    return true;
}

bool App::BeginFrame(f32 deltaTime, const InputState& input) {
    Impl& g = *m_Impl;
    if (!g.ready || !g.presenter) return false;
    g.frameDt = deltaTime;
    g.camInput.Update(input, g.camera, deltaTime);
    return true;
}

bool App::RecordScene() {
    Impl& g = *m_Impl;
    if (!g.ready || !g.presenter) return false;

    // Acquire 失败时返回 false，宿主会跳过本帧的第二条命令（UI + Present）
    if (!g.presenter->BeginFrame()) return false;
    g.presenter->BeginRecording(g.device.get(), g.cmdList.get());

    g.pipeline->NextFrame();
    auto* shadowSys = g.pipeline->GetShadowSystem();
    shadowSys->SetRenderResources(
        g.pipeline->GetCurrentShadowObjectBuffer(),
        g.pipeline->GetCurrentShadowBuffer(),
        g.pipeline->GetCurrentDescSet());

    render::SubsystemContext ctx;
    ctx.world          = &g.world;
    ctx.sceneGraph     = g.sceneGraph.get();
    ctx.camera         = &g.camera.GetCamera();
    ctx.viewportWidth  = g.swapchain->GetWidth();
    ctx.viewportHeight = g.swapchain->GetHeight();

    g.pipeline->GetFrameAssembler().AssembleScene(g.world, *g.sceneGraph, g.camera.GetCamera());
    ctx.snapshot     = &g.pipeline->GetFrameSnapshot();
    ctx.meshRegistry = &g.pipeline->GetMeshRegistry();
    shadowSys->Update(ctx);
    g.pipeline->GetFrameAssembler().ResolveLightShadowIndices(
        [&](Entity le) { return shadowSys->GetShadowIndex(le); });
    g.pipeline->GetFrameAssembler().ReserveOnce();

    g.pipeline->Render(g.cmdList.get(), g.pipeline->GetFrameSnapshot(), g.camera.GetCamera());

    // BackBuffer 合成：ToneMap + ImGui（ImGui 的录制在 RecordUIAndPresent 里）
    g.cmdList->BeginDebugLabel("ToneMap + ImGui (BackBuffer)");
    g.cmdList->BeginRenderPass(1, g.swapchain->GetColorFormat());
    g.pipeline->RenderToneMapPass(g.cmdList.get());
    return true;
}

void App::RecordUIAndPresent() {
    Impl& g = *m_Impl;
    if (g.imgui && g.imgui->IsInitialized()) {
        g.imgui->BeginFrame();
        ImGui::SetNextWindowPos({ 10, 10 }, ImGuiCond_Once);
        {
            const float ui = ImGui::GetFontSize() / 16.0f;
            ImGui::SetNextWindowSize(ImVec2(300.0f * ui, 140.0f * ui), ImGuiCond_Once);
        }
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("02.Cube (App)");
        ImGui::Text("FPS: %.0f", 1.0f / (g.frameDt > 0.0f ? g.frameDt : 0.016f));
        ImGui::Text("后端: %d", static_cast<int>(g.backend));
        ImGui::End();
        g.imgui->EndFrame(g.cmdList.get());
    }

    g.cmdList->EndRenderPass();
    g.presenter->EndRecording();
    g.presenter->EndFrame(true);
}

void App::Resize(u32 width, u32 height) {
    Impl& g = *m_Impl;
    if (!g.ready || width == 0 || height == 0) return;
    g.swapchain->Resize(width, height);
    g.cmdList->SetSwapChain(g.swapchain.get());
    g.pipeline->OnResize(width, height);
    g.camera.SetAspectRatio(static_cast<float>(width), static_cast<float>(height));
}

void App::RequestScreenshot(const char* path) {
    Impl& g = *m_Impl;
    if (g.swapchain) g.swapchain->RequestCaptureNextFrame(path);
}

void App::Shutdown() {
    Impl& g = *m_Impl;
    if (!g.device) return;

    g.device->WaitIdle();
    if (g.imgui) { g.imgui->Shutdown(); g.imgui.reset(); }
    g.presenter.reset();
    g.cmdList.reset();
    if (g.pipeline) { g.pipeline->Shutdown(); g.pipeline.reset(); }
    // 天空盒 Cubemap 属于设备资源：必须在设备销毁前释放（组件析构会销毁纹理）
    if (g.sceneGraph) {
        g.world.ForEach<SkyboxComponent>([&](Entity, SkyboxComponent& sky) {
            sky.SetCubemap(nullptr, nullptr);
        });
    }
    g.sceneGraph.reset();
    g.swapchain.reset();
    g.device.reset();
    rhi::SetDevice(nullptr);
    g.ready = false;
}

bool App::IsReady() const { return m_Impl->ready; }

} // namespace he::samples::cube02


// ============================================================
// 桌面入口
//
// 桌面 target 定义 HE_DESKTOP_ENTRY ⇒ 这里产出 main；鸿蒙 entry 编同一份 .cpp
// 但没有该宏，入口在 Platforms/OHOS/entry/src/main/cpp/AppMain.cpp。
// ============================================================
#if defined(HE_DESKTOP_ENTRY)
#include "DesktopHost.h"

int main() {
    he::samples::desktop::HostConfig cfg;
    cfg.title  = "HugEngine — 02.Cube";
    cfg.width  = 960;
    cfg.height = 540;
    return he::samples::desktop::RunApp<he::samples::cube02::App>(cfg);
}
#endif // HE_DESKTOP_ENTRY
