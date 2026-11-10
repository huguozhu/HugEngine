// ============================================================
// 03.Sponza.App —— Sponza 前向渲染子集（桌面 / 鸿蒙共用）
//
// 场景装配与纹理注册照搬 `03.Sponza-Forward`，但去掉 RT 路径与配置读写，
// 目的是让手机也能跑同一套场景代码，同时保持桌面 `03.Sponza.Subset` 可回归。
// ============================================================

#include "03.Sponza.App.h"

#include "Core/Platform.h"
#include "Core/Log.h"
#include "Core/Engine.h"
#include "Platform/IWindow.h"
#include "Platform/IVfs.h"
#include "Platform/IInput.h"
#include "RHI/FramePresenter.h"
#include "Pipeline/ForwardPipeline.h"
#include "Pipeline/CameraController.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/SkyboxComponent.h"
#include "Asset/glTFLoader.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include "CameraInput.h"
#include "SkyboxLoader.h"  // HMOS_App/Common：HDR 全景 → 6 面 Cubemap

// STB_IMAGE_STATIC：让实现留在本 TU 内部，避免与 02.App 等其它 TU 的实现重复定义
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

namespace he::samples::sponza03 {

struct App::Impl {
    World           world;
    std::unique_ptr<SceneGraph> sceneGraph;

    std::unique_ptr<rhi::IRHIDevice>        device;
    std::unique_ptr<rhi::IRHISwapChain>     swapchain;
    std::unique_ptr<rhi::IRHICommandList>   cmdList;
    std::unique_ptr<render::ForwardPipeline> pipeline;
    std::unique_ptr<editor::ImGuiIntegration> imgui;
    std::unique_ptr<rhi::FramePresenter>    presenter;

    render::CameraController camera;
    samples::CameraInput     camInput;

    /// glTF 纹理缓存：MeshComponent 只存裸指针，缓存必须比场景活得久
    std::unordered_map<String, std::pair<std::unique_ptr<rhi::IRHITexture>,
                                         std::unique_ptr<rhi::IRHISampler>>> texCache;

    rhi::Backend backend = rhi::Backend::Vulkan;
    bool         ready   = false;

    /// 天空盒：HDR 全景图 → 6 面立方体贴图（与桌面 03 同一套算法）
    
    /// 加载一张 glTF 纹理并缓存（同路径只加载一次）
    std::pair<rhi::IRHITexture*, rhi::IRHISampler*> LoadTexture(const String& uri,
                                                                const String& gltfPath) {
        if (uri.empty()) return {nullptr, nullptr};
        const String texPath = (std::filesystem::path(gltfPath).parent_path() / uri).string();
        auto it = texCache.find(texPath);
        if (it == texCache.end()) {
            int w = 0, h = 0, ch = 0;
            u8* pixels = stbi_load(texPath.c_str(), &w, &h, &ch, 4);
            if (!pixels) {
                HE_CORE_WARN("03.App: 纹理加载失败: {}", texPath);
                return {nullptr, nullptr};
            }
            u32 maxDim = static_cast<u32>(std::max(w, h));
            u32 mipLevels = 1;
            while (maxDim > 1) { maxDim >>= 1; ++mipLevels; }

            rhi::TextureDesc td;
            td.format      = rhi::Format::RGBA8_UNORM;
            td.width       = static_cast<u32>(w);
            td.height      = static_cast<u32>(h);
            td.mipLevels   = mipLevels;
            td.usage       = rhi::TextureUsage::ShaderResource
                           | rhi::TextureUsage::TransferSrc
                           | rhi::TextureUsage::TransferDst;
            td.initialData = pixels;
            auto tex = device->CreateTexture(td);

            rhi::SamplerDesc sd;
            sd.minFilter = rhi::FilterMode::Linear;
            sd.magFilter = rhi::FilterMode::Linear;
            sd.mipFilter = rhi::FilterMode::Linear;
            sd.maxLod    = static_cast<float>(mipLevels);
            sd.addressU = sd.addressV = rhi::AddressMode::Repeat;
            auto samp = device->CreateSampler(sd);
            stbi_image_free(pixels);

            it = texCache.emplace(texPath, std::make_pair(std::move(tex), std::move(samp))).first;
            HE_CORE_INFO("03.App: GPU 纹理 {}x{}: {}", w, h, texPath);
        }
        return {it->second.first.get(), it->second.second.get()};
    }
};

App::App() : m_Impl(std::make_unique<Impl>()) {}
App::~App() { Shutdown(); }

bool App::IsReady() const { return m_Impl->ready; }

bool App::Initialize(Engine& engine, rhi::Backend backend, const Options& opt) {
    Impl& g = *m_Impl;
    Shutdown();

    g.backend = backend;
    HE_CORE_INFO("=== 03.App 初始化渲染后端：Vulkan ===");

    rhi::DeviceInitDesc desc;
    desc.backend          = backend;
    desc.enableValidation = false;   // 桌面/手机统一关（手机没有校验层）
    desc.windowHandle     = engine.GetWindow()->GetNativeWindow();

    g.device = rhi::CreateDevice(desc.backend);
    g.device->Initialize(desc);
    rhi::SetDevice(g.device.get());

    const u32 swW = engine.GetWindow()->GetWidth();
    const u32 swH = engine.GetWindow()->GetHeight();
    g.swapchain = g.device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeWindow(),
        .width = swW, .height = swH, .vsync = true,
    });
    if (!g.swapchain) {
        HE_CORE_ERROR("03.App: 交换链创建失败");
        return false;
    }

    // ---- 场景 ----
    g.sceneGraph = std::make_unique<SceneGraph>(g.world);
    const String sponzaPath = vfs::Resolve("gltf/Sponza/glTF/Sponza.gltf");
    HE_CORE_INFO("03.App: 加载 Sponza: {}", sponzaPath);
    auto loadResult = asset::LoadGLTF(g.world, *g.sceneGraph, sponzaPath);
    if (!loadResult.success) {
        HE_CORE_ERROR("03.App: Sponza 加载失败: {}", loadResult.error);
        return false;
    }
    HE_CORE_INFO("03.App: Sponza 加载完成: {} 实体, {} 网格图元",
                 loadResult.entities.size(), loadResult.meshCount);

    // ---- 灯光 ----
    {
        Entity e = g.world.CreateEntity("DirectionalLight");
        g.world.AddComponent<TransformComponent>(e);
        auto* dl = g.world.AddComponent<DirectionalLight>(e);
        dl->direction      = float3(0.4f, -1.0f, 0.6f);
        dl->color          = float3(1.0f, 0.95f, 0.85f);
        dl->intensity      = 15.0f;
        dl->castShadow     = true;
        dl->shadowBias     = 0.003f;
        g.sceneGraph->SetParent(e, Entity{kInvalidEntity});
    }
    {
        Entity e = g.world.CreateEntity("FillLight");
        g.world.AddComponent<TransformComponent>(e);
        auto* dl = g.world.AddComponent<DirectionalLight>(e);
        dl->direction = float3(-0.3f, -0.4f, -0.5f);
        dl->color     = float3(0.6f, 0.7f, 0.9f);
        dl->intensity = 2.0f;
        g.sceneGraph->SetParent(e, Entity{kInvalidEntity});
    }

    // ---- 天空盒 ----
    CreateSkyboxFromHdr(g.world, *g.sceneGraph, g.device.get());
    HE_CORE_INFO("03.App: 场景就绪: {} 实体", g.world.GetEntityCount());

    // ---- 纹理 → bindless（4 个连续槽位构成一个材质）----
    {
        // 默认占位纹理：必须在注册材质纹理之前设置
        u8 white[4] = {255, 255, 255, 255};
        rhi::TextureDesc td;
        td.format      = rhi::Format::RGBA8_UNORM;
        td.width       = 1;
        td.height      = 1;
        td.mipLevels   = 1;
        td.usage       = rhi::TextureUsage::ShaderResource;
        td.initialData = white;
        auto defaultTex = g.device->CreateTexture(td);
        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU = sd.addressV = rhi::AddressMode::Repeat;
        auto defaultSamp = g.device->CreateSampler(sd);
        g.device->GetBindlessHeap()->SetDefaultTexture(defaultTex.get(), defaultSamp.get());
        g.texCache["__default__"] = {std::move(defaultTex), std::move(defaultSamp)};
    }
    g.world.ForEach<MeshComponent>([&](Entity, MeshComponent& mesh) {
        auto [bcTex, bcSamp] = g.LoadTexture(mesh.baseColorTexture, sponzaPath);
        auto [nTex,  nSamp]  = g.LoadTexture(mesh.normalTexture, sponzaPath);
        auto [mrTex, mrSamp] = g.LoadTexture(mesh.metallicRoughnessTexture, sponzaPath);
        auto [aoTex, aoSamp] = g.LoadTexture(mesh.occlusionTexture, sponzaPath);
        auto* heap = g.device->GetBindlessHeap();
        const u32 matID = heap->RegisterTexture(bcTex, bcSamp);
        heap->RegisterTexture(nTex, nSamp);
        heap->RegisterTexture(mrTex, mrSamp);
        heap->RegisterTexture(aoTex, aoSamp);
        mesh.materialID = matID;
    });
    HE_CORE_INFO("03.App: 纹理注册完成: {} 张", g.texCache.size());

    // ---- 前向管线 ----
    g.pipeline = std::make_unique<render::ForwardPipeline>();
    g.pipeline->Initialize(g.device.get());
    g.pipeline->SetUseRenderGraph(false);
    g.pipeline->SetSwapChain(g.swapchain.get());
    g.pipeline->OnResize(g.swapchain->GetWidth(), g.swapchain->GetHeight());

    g.cmdList = g.device->CreateCommandList();
    g.cmdList->SetSwapChain(g.swapchain.get());
    g.cmdList->SetPipeline(g.pipeline->GetPipelineState());

    g.presenter = std::make_unique<rhi::FramePresenter>(g.swapchain.get());

    // ---- 相机（Sponza 尺度较大：移动速度 72，远裁剪 2000）----
    g.camera.SetAspectRatio(static_cast<float>(g.swapchain->GetWidth()),
                            static_cast<float>(g.swapchain->GetHeight()));
    g.camera.SetMoveSpeed(72.0f);
    g.camera.SetPosition(float3(0.0f, 2.5f, -10.0f));
    g.camera.SetOrientationFromForward(float3(0.0f, 0.0f, 1.0f));
    g.camera.GetCamera().nearPlane = 0.1f;
    g.camera.GetCamera().farPlane  = 2000.0f;

    if (opt.enableImGui) {
        g.imgui = std::make_unique<editor::ImGuiIntegration>();
        g.imgui->Initialize(engine.GetWindow(), g.device.get(), g.swapchain.get());
    }

    g.ready = true;
    HE_CORE_INFO("03.App 初始化完成（{}x{}）", g.swapchain->GetWidth(), g.swapchain->GetHeight());
    return true;
}

bool App::Frame(f32 deltaTime, const InputState& input) {
    Impl& g = *m_Impl;
    if (!g.ready || !g.presenter) return false;

    // 相机输入（桌面：右键拖拽 + WASD；鸿蒙：单指拖动映射成右键拖拽）
    g.camInput.Update(input, g.camera, deltaTime);

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

    // BackBuffer 合成：ToneMap + ImGui
    g.cmdList->BeginDebugLabel("ToneMap + ImGui (BackBuffer)");
    g.cmdList->BeginRenderPass(1, g.swapchain->GetColorFormat());
    g.pipeline->RenderToneMapPass(g.cmdList.get());

    if (g.imgui && g.imgui->IsInitialized()) {
        g.imgui->BeginFrame();
        const float ui = ImGui::GetFontSize() / 16.0f;   // 高 DPI 缩放
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(300.0f * ui, 140.0f * ui), ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("03.Sponza (App)");
        ImGui::Text("FPS: %.0f", 1.0f / (deltaTime > 0.0f ? deltaTime : 0.016f));
        ImGui::Text("后端: %d", static_cast<int>(g.backend));
        ImGui::End();
        g.imgui->EndFrame(g.cmdList.get());
    }

    g.cmdList->EndRenderPass();
    g.presenter->EndRecording();
    g.presenter->EndFrame(true);
    return true;
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
    if (!g.device) {
        g.ready = false;
        return;
    }
    g.device->WaitIdle();

    if (g.imgui) { g.imgui->Shutdown(); g.imgui.reset(); }
    g.presenter.reset();
    g.cmdList.reset();
    if (g.pipeline) { g.pipeline->Shutdown(); g.pipeline.reset(); }

    // 天空盒 Cubemap 是设备资源：必须在设备销毁前释放
    if (g.sceneGraph) {
        g.world.ForEach<SkyboxComponent>([&](Entity, SkyboxComponent& sky) {
            sky.SetCubemap(nullptr, nullptr);
        });
    }
    // 【必须在销毁设备之前把场景里的 GPU 缓冲清掉】World 是 Impl 的成员，比 device 活得久；
    // 设备销毁后再析构 MeshComponent 会对已释放的 device 调 vkDestroyBuffer（真机实测：
    // 从 03 切回 01 时进程崩溃，日志停在 "03.App 已关闭"）。
    g.world.ForEach<MeshComponent>([](Entity, MeshComponent& mesh) {
        mesh.GetVertexBuffer().reset();
        mesh.GetIndexBuffer().reset();
    });
    g.texCache.clear();
    g.sceneGraph.reset();
    g.swapchain.reset();
    g.device.reset();
    rhi::SetDevice(nullptr);
    g.ready = false;
    HE_CORE_INFO("03.App 已关闭");
}

} // namespace he::samples::sponza03


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
    cfg.title  = "HugEngine — 03.Sponza-Forward";
    cfg.width  = 960;
    cfg.height = 540;
    return he::samples::desktop::RunApp<he::samples::sponza03::App>(cfg);
}
#endif // HE_DESKTOP_ENTRY
