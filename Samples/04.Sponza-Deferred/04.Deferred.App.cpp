// ============================================================
// 04.Deferred.App —— Sponza 延迟渲染子集（桌面 / 鸿蒙共用）
//
// 场景装配与 03.Sponza.App 同源（glTF + 纹理 bindless + HDR 天空盒），
// 管线换成 DeferredPipeline（GBuffer → 全屏 Lighting → ToneMap）。
// 移动端默认关掉 SSGI/DDGI/SSR/SSAO/Clustered，保证还能交互。
// ============================================================

#include "04.Deferred.App.h"

#include "Core/Platform.h"
#include "Core/Log.h"
#include "Core/Engine.h"
#include "Platform/IWindow.h"
#include "Platform/IVfs.h"
#include "Platform/IInput.h"
#include "RHI/FramePresenter.h"
#include "Pipeline/DeferredPipeline.h"
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
#include "SkyboxLoader.h"  // Samples/Common：HDR 全景 → 6 面 Cubemap

// STB_IMAGE_STATIC：实现留在本 TU，避免与其它 App 的 stb 实现重复定义
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace he::samples::sponza04 {

namespace {

/// 配置文件：相机 + 两盏方向光 + 管线开关（key=value；路径走 vfs 的可写目录）
String ConfigPath() {
    return vfs::ResolveWritable("Config/04_Sponza-Deferred.cfg");
}

std::unordered_map<String, String> LoadConfigFile(const String& path) {
    std::unordered_map<String, String> map;
    std::ifstream f(path);
    if (!f.is_open()) return map;
    String line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == String::npos) continue;
        String key = line.substr(0, eq);
        String val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        map[key] = val;
    }
    return map;
}

void SaveConfigFile(const String& path, const std::unordered_map<String, String>& map) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path);
    if (!f.is_open()) return;
    for (const auto& [k, v] : map) f << k << "=" << v << "\n";
}

f32 GetFloat(const std::unordered_map<String, String>& m, const String& key, f32 def) {
    const auto it = m.find(key);
    return it != m.end() ? std::stof(it->second) : def;
}

bool GetBool(const std::unordered_map<String, String>& m, const String& key, bool def) {
    return GetFloat(m, key, def ? 1.0f : 0.0f) != 0.0f;
}

} // namespace

struct App::Impl {
    World           world;
    std::unique_ptr<SceneGraph> sceneGraph;

    std::unique_ptr<rhi::IRHIDevice>         device;
    std::unique_ptr<rhi::IRHISwapChain>      swapchain;
    std::unique_ptr<rhi::IRHICommandList>    cmdList;
    std::unique_ptr<render::DeferredPipeline> pipeline;
    std::unique_ptr<editor::ImGuiIntegration> imgui;
    std::unique_ptr<rhi::FramePresenter>     presenter;

    render::CameraController camera;
    samples::CameraInput     camInput;

    std::unordered_map<String, std::pair<std::unique_ptr<rhi::IRHITexture>,
                                         std::unique_ptr<rhi::IRHISampler>>> texCache;

    rhi::Backend backend = rhi::Backend::Vulkan;
    bool         ready   = false;
    f32          frameDt = 0.016f;   // 本帧 deltaTime（面板 FPS 用）

    /// 天空盒组件（面板里可开关；鸿蒙上默认关，用于排查过曝）
    SkyboxComponent* skybox = nullptr;

    // ---- 管线开关（面板 + 配置）----
    bool clustered = true;
    bool gpuCull   = false;
    bool skyboxOn  = true;
    bool ssgi = false, ddgi = false, ssr = false, ssao = false;

    Entity mainLightEntity{};
    Entity fillLightEntity{};

    /// HDR 全景图 → 6 面立方体贴图（与 03 同一套算法）
    
    std::pair<rhi::IRHITexture*, rhi::IRHISampler*> LoadTexture(const String& uri,
                                                                const String& gltfPath) {
        if (uri.empty()) return {nullptr, nullptr};
        const String texPath = (std::filesystem::path(gltfPath).parent_path() / uri).string();
        auto it = texCache.find(texPath);
        if (it == texCache.end()) {
            int w = 0, h = 0, ch = 0;
            u8* pixels = stbi_load(texPath.c_str(), &w, &h, &ch, 4);
            if (!pixels) {
                HE_CORE_WARN("04.App: 纹理加载失败: {}", texPath);
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
            HE_CORE_INFO("04.App: GPU 纹理 {}x{}: {}", w, h, texPath);
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
    HE_CORE_INFO("=== 04.App 初始化渲染后端：Vulkan ===");

    rhi::DeviceInitDesc desc;
    desc.backend          = backend;
    desc.enableValidation = false;
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
        HE_CORE_ERROR("04.App: 交换链创建失败");
        return false;
    }

    // ---- 场景 ----
    g.sceneGraph = std::make_unique<SceneGraph>(g.world);
    const String sponzaPath = vfs::Resolve("gltf/Sponza/glTF/Sponza.gltf");
    HE_CORE_INFO("04.App: 加载 Sponza: {}", sponzaPath);
    auto loadResult = asset::LoadGLTF(g.world, *g.sceneGraph, sponzaPath);
    if (!loadResult.success) {
        HE_CORE_ERROR("04.App: Sponza 加载失败: {}", loadResult.error);
        return false;
    }
    HE_CORE_INFO("04.App: Sponza 加载完成: {} 实体, {} 网格图元",
                 loadResult.entities.size(), loadResult.meshCount);

    // ---- 灯光 ----
    {
        Entity e = g.world.CreateEntity("DirectionalLight");
        g.mainLightEntity = e;
        g.world.AddComponent<TransformComponent>(e);
        auto* dl = g.world.AddComponent<DirectionalLight>(e);
        dl->direction  = float3(0.4f, -1.0f, 0.6f);
        dl->color      = float3(1.0f, 0.95f, 0.85f);
        dl->intensity  = 15.0f;
        dl->castShadow = true;
        dl->shadowBias = 0.003f;
        g.sceneGraph->SetParent(e, Entity{kInvalidEntity});
    }
    {
        Entity e = g.world.CreateEntity("FillLight");
        g.fillLightEntity = e;
        g.world.AddComponent<TransformComponent>(e);
        auto* dl = g.world.AddComponent<DirectionalLight>(e);
        dl->direction = float3(-0.3f, -0.4f, -0.5f);
        dl->color     = float3(0.6f, 0.7f, 0.9f);
        dl->intensity = 2.0f;
        g.sceneGraph->SetParent(e, Entity{kInvalidEntity});
    }

    CreateSkyboxFromHdr(g.world, *g.sceneGraph, g.device.get());
    g.world.ForEach<SkyboxComponent>([&](Entity, SkyboxComponent& sky) { g.skybox = &sky; });
    HE_CORE_INFO("04.App: 场景就绪: {} 实体", g.world.GetEntityCount());

    // ---- 纹理 → bindless ----
    {
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
    HE_CORE_INFO("04.App: 纹理注册完成: {} 张", g.texCache.size());

    // ---- 延迟管线 ----
    g.pipeline = std::make_unique<render::DeferredPipeline>();
    g.pipeline->Initialize(g.device.get());
    g.pipeline->SetSwapChain(g.swapchain.get());
    g.pipeline->OnResize(g.swapchain->GetWidth(), g.swapchain->GetHeight());

    // 【移动端性能】关掉开销大的 GI/后处理；CSM 阴影与 GBuffer 保留
    if (auto* ssgi = g.pipeline->GetSSGI()) ssgi->SetEnabled(false);
    if (auto* ddgi = g.pipeline->GetDDGI()) ddgi->SetEnabled(false);
    if (auto* ssr = g.pipeline->GetSSR())   ssr->SetEnabled(false);
    g.pipeline->GetSSAO().enabled              = false;
    // 【排查】Clustered 关闭后 Lighting 可能走另一条（未验证的）路径：真机上过曝。
    // 先保持开启，验证它是否是过曝的根源。
    g.pipeline->GetClusteredShading().enabled  = true;
    g.pipeline->GetGPUCulling().enabled        = false;
    HE_CORE_INFO("04.App: 延迟管线初始化完成（SSGI/DDGI/SSR/SSAO/Clustered 已关）");

    g.cmdList = g.device->CreateCommandList();
    g.cmdList->SetSwapChain(g.swapchain.get());
    // 预设 ToneMap PSO：与后缓冲 RP 匹配（ImGui 的 LoadOp 兼容）
    g.cmdList->SetPipeline(g.pipeline->GetToneMap()->GetPSO());

    g.presenter = std::make_unique<rhi::FramePresenter>(g.swapchain.get());

    // ---- 相机 ----
    g.camera.SetAspectRatio(static_cast<float>(g.swapchain->GetWidth()),
                            static_cast<float>(g.swapchain->GetHeight()));
    g.camera.SetMoveSpeed(72.0f);
    g.camera.SetPosition(float3(0.0f, 2.5f, -10.0f));
    g.camera.SetOrientationFromForward(float3(0.0f, 0.0f, 1.0f));
    g.camera.GetCamera().nearPlane = 0.1f;
    g.camera.GetCamera().farPlane  = 2000.0f;

    // ---- 配置：相机 / 灯光 / 管线开关（没有配置文件就用默认值）----
    const String cfgPath = ConfigPath();
    const auto cfg = LoadConfigFile(cfgPath);
    if (!cfg.empty()) {
        HE_CORE_INFO("04.App: 加载配置文件 {}", cfgPath);
        g.camera.SetPosition(float3(GetFloat(cfg, "cam_pos_x", 0.0f),
                                    GetFloat(cfg, "cam_pos_y", 2.5f),
                                    GetFloat(cfg, "cam_pos_z", -10.0f)));
        g.camera.SetOrientation(GetFloat(cfg, "cam_yaw", 0.0f), GetFloat(cfg, "cam_pitch", 0.0f));
        g.camera.GetCamera().nearPlane = GetFloat(cfg, "cam_near", 0.1f);
        g.camera.GetCamera().farPlane  = GetFloat(cfg, "cam_far", 2000.0f);

        g.clustered = GetBool(cfg, "clustered", true);
        g.gpuCull   = GetBool(cfg, "gpu_cull", false);
        g.skyboxOn  = GetBool(cfg, "skybox", true);
        g.ssgi      = GetBool(cfg, "ssgi_enabled", false);
        g.ddgi      = GetBool(cfg, "ddgi_enabled", false);
        g.ssr       = GetBool(cfg, "ssr_enabled", false);
        g.ssao      = GetBool(cfg, "ssao_enabled", false);

        g.pipeline->GetClusteredShading().enabled = g.clustered;
        g.pipeline->GetGPUCulling().enabled       = g.gpuCull;
        if (g.pipeline->GetSSGI()) g.pipeline->GetSSGI()->SetEnabled(g.ssgi);
        if (g.pipeline->GetDDGI()) g.pipeline->GetDDGI()->SetEnabled(g.ddgi);
        if (g.pipeline->GetSSR())  g.pipeline->GetSSR()->SetEnabled(g.ssr);
        g.pipeline->GetSSAO().enabled = g.ssao;
        if (g.skybox) g.skybox->enabled = g.skyboxOn;

        if (auto* dl = g.world.GetComponent<DirectionalLight>(g.mainLightEntity)) {
            dl->direction = float3(GetFloat(cfg, "light_dir_x", 0.4f),
                                   GetFloat(cfg, "light_dir_y", -1.0f),
                                   GetFloat(cfg, "light_dir_z", 0.6f));
            dl->color     = float3(GetFloat(cfg, "light_color_r", 1.0f),
                                   GetFloat(cfg, "light_color_g", 0.95f),
                                   GetFloat(cfg, "light_color_b", 0.85f));
            dl->intensity = GetFloat(cfg, "light_intensity", 15.0f);
            dl->castShadow = GetBool(cfg, "shadow_enabled", true);
            dl->shadowBias = GetFloat(cfg, "shadow_bias", 0.003f);
        }
    }

    if (opt.enableImGui) {
        g.imgui = std::make_unique<editor::ImGuiIntegration>();
        g.imgui->Initialize(engine.GetWindow(), g.device.get(), g.swapchain.get());
    }

    g.ready = true;
    HE_CORE_INFO("04.App 初始化完成（{}x{}）", g.swapchain->GetWidth(), g.swapchain->GetHeight());
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
    g.camInput.Update(input, g.camera, deltaTime);
    g.frameDt = deltaTime;
    return true;
}

bool App::RecordScene() {
    Impl& g = *m_Impl;
    if (!g.ready || !g.presenter) return false;

    // Acquire 失败时返回 false，宿主会跳过本帧的第二条命令（UI + Present）
    if (!g.presenter->BeginFrame()) return false;
    g.presenter->BeginRecording(g.device.get(), g.cmdList.get());

    // 延迟管线自己负责阴影资源的逐帧绑定与 pass 顺序，样例侧只需：
    // NextFrame → AssembleScene → Render
    g.pipeline->NextFrame();
    g.pipeline->GetFrameAssembler().AssembleScene(g.world, *g.sceneGraph, g.camera.GetCamera());
    g.pipeline->Render(g.cmdList.get(), g.pipeline->GetFrameSnapshot(), g.camera.GetCamera());

    // ImGui：LOAD 保留 ToneMap 输出；颜色格式取交换链实际格式（手机上是 RGBA8）
    g.cmdList->BeginRenderPass(1, g.swapchain->GetColorFormat(),
                               rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);
    return true;
}

void App::RecordUIAndPresent() {
    Impl& g = *m_Impl;
    if (g.imgui && g.imgui->IsInitialized()) {
        g.imgui->BeginFrame();
        const float ui = ImGui::GetFontSize() / 16.0f;
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(300.0f * ui, 140.0f * ui), ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("04.Sponza-Deferred (App)");
        ImGui::Text("FPS: %.0f", 1.0f / (g.frameDt > 0.0f ? g.frameDt : 0.016f));
        ImGui::Text("GBuffer + Lighting");

        ImGui::SeparatorText("延迟渲染管线");
        if (ImGui::Checkbox("Clustered Shading", &g.clustered))
            g.pipeline->GetClusteredShading().enabled = g.clustered;
        if (ImGui::Checkbox("GPU 视锥剔除", &g.gpuCull))
            g.pipeline->GetGPUCulling().enabled = g.gpuCull;
        if (g.skybox && ImGui::Checkbox("天空盒", &g.skyboxOn))
            g.skybox->enabled = g.skyboxOn;

        ImGui::SeparatorText("GI");
        if (ImGui::Checkbox("SSGI", &g.ssgi) && g.pipeline->GetSSGI()) g.pipeline->GetSSGI()->SetEnabled(g.ssgi);
        if (ImGui::Checkbox("DDGI", &g.ddgi) && g.pipeline->GetDDGI()) g.pipeline->GetDDGI()->SetEnabled(g.ddgi);
        if (ImGui::Checkbox("SSR",  &g.ssr)  && g.pipeline->GetSSR())  g.pipeline->GetSSR()->SetEnabled(g.ssr);

        ImGui::SeparatorText("后处理");
        if (ImGui::Checkbox("SSAO", &g.ssao)) g.pipeline->GetSSAO().enabled = g.ssao;
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
    if (!g.device) {
        g.ready = false;
        return;
    }
    g.device->WaitIdle();

    // ---- 保存配置（相机 + 灯光 + 管线开关）----
    {
        std::unordered_map<String, String> out;
        out["cam_pos_x"] = std::to_string(g.camera.GetCamera().position.x);
        out["cam_pos_y"] = std::to_string(g.camera.GetCamera().position.y);
        out["cam_pos_z"] = std::to_string(g.camera.GetCamera().position.z);
        out["cam_yaw"]   = std::to_string(g.camera.GetYaw());
        out["cam_pitch"] = std::to_string(g.camera.GetPitch());
        out["cam_near"]  = std::to_string(g.camera.GetCamera().nearPlane);
        out["cam_far"]   = std::to_string(g.camera.GetCamera().farPlane);
        out["clustered"] = std::to_string(g.clustered ? 1 : 0);
        out["gpu_cull"]  = std::to_string(g.gpuCull ? 1 : 0);
        out["skybox"]    = std::to_string(g.skyboxOn ? 1 : 0);
        out["ssgi_enabled"] = std::to_string(g.ssgi ? 1 : 0);
        out["ddgi_enabled"] = std::to_string(g.ddgi ? 1 : 0);
        out["ssr_enabled"]  = std::to_string(g.ssr ? 1 : 0);
        out["ssao_enabled"] = std::to_string(g.ssao ? 1 : 0);
        if (auto* dl = g.world.GetComponent<DirectionalLight>(g.mainLightEntity)) {
            out["light_dir_x"]     = std::to_string(dl->direction.x);
            out["light_dir_y"]     = std::to_string(dl->direction.y);
            out["light_dir_z"]     = std::to_string(dl->direction.z);
            out["light_color_r"]   = std::to_string(dl->color.x);
            out["light_color_g"]   = std::to_string(dl->color.y);
            out["light_color_b"]   = std::to_string(dl->color.z);
            out["light_intensity"] = std::to_string(dl->intensity);
            out["shadow_enabled"]  = std::to_string(dl->castShadow ? 1 : 0);
            out["shadow_bias"]     = std::to_string(dl->shadowBias);
        }
        SaveConfigFile(ConfigPath(), out);
        HE_CORE_INFO("04.App: 配置已保存 {}", ConfigPath());
    }

    if (g.imgui) { g.imgui->Shutdown(); g.imgui.reset(); }
    g.presenter.reset();
    g.cmdList.reset();
    if (g.pipeline) { g.pipeline->Shutdown(); g.pipeline.reset(); }

    if (g.sceneGraph) {
        g.world.ForEach<SkyboxComponent>([&](Entity, SkyboxComponent& sky) {
            sky.SetCubemap(nullptr, nullptr);
        });
    }
    // 设备销毁前清空网格 GPU 缓冲（World 比 device 活得久，见 03.App 的说明）
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
    HE_CORE_INFO("04.App 已关闭");
}

} // namespace he::samples::sponza04


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
    cfg.title  = "HugEngine — 04.Sponza-Deferred";
    cfg.width  = 960;
    cfg.height = 540;
    return he::samples::desktop::RunApp<he::samples::sponza04::App>(cfg);
}
#endif // HE_DESKTOP_ENTRY
