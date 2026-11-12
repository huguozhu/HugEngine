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
#include "Core/CrashHandler.h"
#include "Platform/IWindow.h"
#include "Platform/IVfs.h"
#include "Platform/IInput.h"
#include "RHI/FramePresenter.h"
#include "Pipeline/DeferredPipeline.h"
#include "Pipeline/CameraController.h"
#include "Pipeline/PhysicalCamera.h"   // 面板「物理相机」分组
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/AnimationComponent.h"   // 相机平移动画（面板可播放/调速）
#include "Scene/SphereComponent.h"   // 点光源/聚光灯位置上的可视化小球
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
#include "Scene/CubeComponent.h"   // T8：多网格变换验收用的立方体

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

// T8 ④-b：整数型 cfg 值（多资产份数）
static int GetInt(const std::unordered_map<String, String>& m,
                  const String& key, int def = 0) {
    auto it = m.find(key);
    return (it != m.end()) ? std::stoi(it->second) : def;
}

// T9：字符串型 cfg 值（Nanite 离线资产路径）
static String GetString(const std::unordered_map<String, String>& m,
                        const String& key, const String& def = String{}) {
    auto it = m.find(key);
    return (it != m.end()) ? it->second : def;
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
    bool transformOnly = false;   // T8：只画变换验收件（cfg `nanite_transform_only`）
    u32  multiAssetCount = 8;     // T8：多资产档注册份数（cfg `nanite_multi_asset_count`）
    bool transformTest   = false; // T8：生成三个变换验收件（cfg `nanite_transform_test`）
    bool clustered = true;
    bool gpuCull   = true;    // 与旧完整版一致：默认开启 GPU 视锥剔除
    bool skyboxOn  = true;
    bool ssgi = false, ddgi = false, ssr = false, ssao = false;

    Entity mainLightEntity{};
    Entity fillLightEntity{};

    // ---- 点光源 / 聚光灯（旧完整版面板要能调它们，故一并建出来）----
    Entity pointLightEntity{};        // 点光源（面板可调位置/颜色/强度/范围/阴影）
    Entity pointLightSphereEntity{};  // 点光源位置上的可视化小球
    Entity spotLightEntity{};         // 聚光灯
    Entity spotLightConeEntity{};     // 聚光灯位置上的可视化小球

    // ---- 相机平移动画（AnimationComponent 演示；面板可播放/调速，T 键切换模式）----
    Entity               camAnimEntity{};
    AnimationComponent*  camAnim = nullptr;
    bool                 animCameraMode = true;

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
    // 【T8 验收开关】`nanite_transform_only=1` 时**不加载 Sponza**，场景只剩 3 个变换验收件，
    //   截图才能干净地看出"平移 / 旋转 / 非均匀缩放"是否真的作用到了 Nanite 资产上。
    const String sponzaPath = vfs::Resolve("gltf/Sponza/glTF/Sponza.gltf");
    // 【为什么用环境变量而不是 cfg】cfg 在**场景加载之后**才解析（`LoadConfig` 在
    //   `Init` 的后段），而 Sponza 的加载在这里 ⇒ 用 cfg 开关永远是 false。
    const bool transformOnly = std::getenv("HE_NANITE_TRANSFORM_ONLY") != nullptr;
    g.transformOnly = transformOnly;
    if (transformOnly) {
        HE_CORE_INFO("04.App: nanite_transform_only=1 —— 跳过 Sponza，仅保留变换验收件");
    } else {
        HE_CORE_INFO("04.App: 加载 Sponza: {}", sponzaPath);
        auto loadResult = asset::LoadGLTF(g.world, *g.sceneGraph, sponzaPath);
        if (!loadResult.success) {
            HE_CORE_ERROR("04.App: Sponza 加载失败: {}", loadResult.error);
            return false;
        }
        HE_CORE_INFO("04.App: Sponza 加载完成: {} 实体, {} 网格图元",
                     loadResult.entities.size(), loadResult.meshCount);
    }

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

    // ---- 点光源 + 可视化球体（旧完整版面板可调位置/颜色/强度/范围/阴影）----
    {
        Entity e = g.world.CreateEntity("PointLight");
        g.pointLightEntity = e;
        g.world.AddComponent<TransformComponent>(e);
        auto* pl = g.world.AddComponent<PointLight>(e);
        pl->color      = float3(1.0f, 0.85f, 0.6f);   // 暖色
        pl->intensity  = 20.0f;
        pl->range      = 600.0f;
        pl->castShadow = true;
        pl->shadowBias = 0.005f;
        if (auto* tf = g.world.GetComponent<TransformComponent>(e)) {
            tf->position = float3(-300.0f, 100.0f, 0.0f);
        }
        g.sceneGraph->SetParent(e, Entity{kInvalidEntity});

        // 可视化小球：跟着点光源走，便于在 Sponza 大场景里定位
        Entity s = g.world.CreateEntity("PointLightSphere");
        g.pointLightSphereEntity = s;
        g.world.AddComponent<TransformComponent>(s);
        auto* sphere = g.world.AddComponent<SphereComponent>(s);
        sphere->radius       = 15.0f;
        sphere->segmentCount = 16;
        sphere->ringCount    = 8;
        sphere->OnCreate();
        if (auto* tf = g.world.GetComponent<TransformComponent>(s)) {
            tf->position = float3(-300.0f, 100.0f, 0.0f);
        }
        g.sceneGraph->SetParent(s, Entity{kInvalidEntity});
    }

    // ---- 聚光灯 + 可视化球体（面板同样可调）----
    {
        Entity e = g.world.CreateEntity("SpotLight");
        g.spotLightEntity = e;
        g.world.AddComponent<TransformComponent>(e);
        auto* sl = g.world.AddComponent<SpotLight>(e);
        sl->color          = float3(1.0f, 0.9f, 0.7f);
        sl->intensity      = 80.0f;
        sl->range          = 1200.0f;
        sl->innerConeAngle = 0.25f;
        sl->outerConeAngle = 0.50f;
        sl->direction      = float3(0.0f, -1.0f, 0.3f);
        sl->castShadow     = true;
        sl->shadowBias     = 0.005f;
        if (auto* tf = g.world.GetComponent<TransformComponent>(e)) {
            tf->position = float3(0.0f, 200.0f, -200.0f);
        }
        g.sceneGraph->SetParent(e, Entity{kInvalidEntity});

        Entity s = g.world.CreateEntity("SpotLightCone");
        g.spotLightConeEntity = s;
        g.world.AddComponent<TransformComponent>(s);
        auto* cone = g.world.AddComponent<SphereComponent>(s);
        cone->radius       = 10.0f;
        cone->segmentCount = 8;
        cone->ringCount    = 4;
        cone->OnCreate();
        if (auto* tf = g.world.GetComponent<TransformComponent>(s)) {
            tf->position = float3(0.0f, 200.0f, -200.0f);
        }
        g.sceneGraph->SetParent(s, Entity{kInvalidEntity});
    }

    // ---- 相机平移动画（AnimationComponent 演示：绕 Y 轴 6 秒一圈，默认不播放）----
    {
        Entity e = g.world.CreateEntity("CameraAnimation");
        g.camAnimEntity = e;
        g.world.AddComponent<TransformComponent>(e);
        g.camAnim = g.world.AddComponent<AnimationComponent>(e);
        auto& clips = g.camAnim->clips;
        clips.push_back({});
        g.camAnim->currentClip = 0;
        auto& clip = clips.back();
        clip.name    = "CameraOrbit";
        clip.looping = true;
        const f32 radius   = 600.0f;
        const f32 height   = 200.0f;
        const int steps    = 60;
        const f32 duration = 6.0f;
        for (int i = 0; i <= steps; ++i) {
            const f32 t     = static_cast<f32>(i) / steps * duration;
            const f32 angle = (static_cast<f32>(i) / steps) * glm::radians(360.0f);
            g.camAnim->AddTranslationKey(t, float3(std::cos(angle) * radius, height,
                                                   std::sin(angle) * radius));
        }
        g.camAnim->FinalizeClip();
        g.camAnim->playing = false;   // 默认关闭，面板里勾"播放动画"或在面板里勾
        g.sceneGraph->SetParent(e, Entity{kInvalidEntity});
        HE_CORE_INFO("04.App: 相机动画已创建（6 秒圆形路径，默认不播放）");
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
        g.transformOnly = GetBool(cfg, "nanite_transform_only", false);
        g.transformTest = GetBool(cfg, "nanite_transform_test", false);
        g.gpuCull   = GetBool(cfg, "gpu_cull", true);
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

        // ── T8 ④-b：多资产验收档（cfg `nanite_multi_asset=1`）──
        // 把 `Content/Processed/primitives/*.nanite`（按 primitive 产出的离线资产）逐个注册进
        // `NaniteScene` 的资产注册表，并按**网格创建顺序**把资产槽位绑到各网格上。
        // 默认关闭 ⇒ 单资产路径与 T9 完全一致。
        // 【只做前 N 份】注册会整体重建聚合缓冲（O(资产体积)），103 份全量在调试档下太慢；
        //   `nanite_multi_asset_count` 控制份数（默认 8），够验证"多资产剔除/实例表"。
        if (GetBool(cfg, "nanite_multi_asset", false)) {
            const String dir = vfs::Resolve("Processed/primitives");
            std::error_code listEc;
            std::vector<String> files;
            for (const auto& entry : std::filesystem::directory_iterator(dir, listEc)) {
                if (entry.is_regular_file() && entry.path().extension() == ".nanite") {
                    files.push_back(entry.path().string());
                }
            }
            std::sort(files.begin(), files.end());
            const int limit = std::max(1, GetInt(cfg, "nanite_multi_asset_count", 8));
        g.multiAssetCount = (u32)limit;
            if ((int)files.size() > limit) files.resize((usize)limit);

            u32 bound = 0u;
            for (const String& file : files) {
                std::ifstream in(file, std::ios::binary | std::ios::ate);
                if (!in) continue;
                const std::streamsize size = in.tellg();
                in.seekg(0, std::ios::beg);
                std::vector<u8> bytes((usize)size);
                if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) continue;

                render::NanitePackedAsset asset;
                render::NaniteAssetLoadInfo info;
                if (render::ParseNaniteAsset(bytes.data(), bytes.size(),
                                             render::kNaniteBuilderVersion, asset, &info) !=
                    render::NaniteAssetLoadStatus::Ok) {
                    continue;
                }
                // 【T8】每份资产的材质记录由 MaterialDesc 扩展段翻译（否则多资产档全部回退）
                render::NaniteFillMaterialsFromDesc(asset, info.materialDescs);
                const u32 slot = g.pipeline->GetNanite().GetScene().RegisterAsset(asset);
                if (slot == render::NaniteScene::kInvalidAssetIndex) continue;

                // 按"网格创建顺序"绑定：第 bound 个 MeshComponent 用第 bound 个资产
                u32 seen = 0u;
                g.world.ForEach<MeshComponent>([&](Entity, MeshComponent& mesh) {
                    if (seen++ != bound) return;
                    mesh.SetNaniteAssetIndex(slot);
                });
                ++bound;
            }
            HE_CORE_INFO("04.App: 多资产验收档启用 —— 已注册并绑定 {} 份 per-primitive 资产", bound);

            // ── T8 ④-b：多网格变换验收（cfg `nanite_transform_test=1`）──
            // 生成 3 个立方体，分别带：纯平移 / 旋转 + 平移 / 非均匀缩放 + 平移，
            // 各绑一份离线资产。它们覆盖第 4 步"完整 localToWorld + 最大轴缩放半径"的三条分支，
            // 也覆盖第 5 步"多资产同帧绘制"。
            // 【T8 验收】三件改用**单位立方体资产**（`Processed/cube_0.nanite`）：
            //   Sponza 的 primitive 资产顶点在建筑坐标系里（离原点数百单位），缩放后仍会跑到
            //   画面外，截图无法验收；立方体资产以原点为中心，三种变换一眼可辨。
            u32 cubeAssetSlot = render::NaniteScene::kInvalidAssetIndex;
            if (GetBool(cfg, "nanite_transform_test", false)) {
                const String cubePath = vfs::Resolve("Processed/cube_0.nanite");
                std::ifstream cubeFile(std::filesystem::path(cubePath), std::ios::binary);
                if (cubeFile) {
                    // 【为什么要单独取文件大小】`tellg()` 在刚打开的流上不保证返回文件长度
                    //   （实测返回 0 ⇒ 读到 0 字节、解析失败、验收件根本没被创建）。
                    std::error_code sizeEc;
                    const u64 n = (u64)std::filesystem::file_size(std::filesystem::path(cubePath), sizeEc);
                    cubeFile.seekg(0, std::ios::beg);
                    std::vector<u8> bytes((size_t)n);
                    if (cubeFile.read(reinterpret_cast<char*>(bytes.data()), n)) {
                        render::NanitePackedAsset cubeAsset;
                        render::NaniteAssetLoadInfo cubeInfo;
                        if (render::ParseNaniteAsset(bytes.data(), bytes.size(),
                                                     render::kNaniteBuilderVersion,
                                                     cubeAsset, &cubeInfo) ==
                            render::NaniteAssetLoadStatus::Ok) {
                            render::NaniteFillMaterialsFromDesc(cubeAsset, cubeInfo.materialDescs);
                            cubeAssetSlot = g.pipeline->GetNanite().GetScene().RegisterAsset(cubeAsset);
                            HE_CORE_INFO("04.App: 变换验收件资产已注册（cube_0.nanite → 槽位 {}）",
                                         cubeAssetSlot);
                        } else {
                            HE_CORE_WARN("04.App: cube_0.nanite 解析失败（{} 字节）", (u64)bytes.size());
                        }
                    } else {
                        HE_CORE_WARN("04.App: cube_0.nanite 读取失败");
                    }
                } else {
                    HE_CORE_WARN("04.App: cube_0.nanite 打开失败：{}", cubePath);
                }
            }

            if (GetBool(cfg, "nanite_transform_test", false) &&
                cubeAssetSlot != render::NaniteScene::kInvalidAssetIndex) {
                struct CubeSpec { const char* name; float3 pos; float3 eulerDeg; float3 scale; };
                // 【T8 验收】三件放在 Sponza 建筑**之外**（y=300、z=-2600 一线）：在建筑内部会被
                //   墙体遮住，截图看不出变换是否正确。资产是 Sponza 的原始尺寸（数百单位），
                //   用 0.02 缩放缩成"小块"；第三件用 (0.06, 0.01, 0.02) 覆盖"包围球半径按
                //   最大轴放大"这条分支。
                const CubeSpec specs[3] = {
                    { "NaniteCube_Translate", float3(-150.0f, 200.0f, -2600.0f), float3(0.0f,  0.0f, 0.0f), float3(40.0f, 40.0f, 40.0f) },
                    { "NaniteCube_Rotate",    float3(   0.0f, 200.0f, -2600.0f), float3(0.0f, 45.0f, 0.0f), float3(40.0f, 40.0f, 40.0f) },
                    { "NaniteCube_NonUniform",float3( 150.0f, 200.0f, -2600.0f), float3(30.0f,20.0f, 0.0f), float3(60.0f, 15.0f, 40.0f) },
                };
                for (u32 i = 0u; i < 3u; ++i) {
                    Entity e = g.world.CreateEntity(specs[i].name);
                    g.world.AddComponent<TransformComponent>(e);
                    auto* cube = g.world.AddComponent<CubeComponent>(e);
                    cube->halfExtent = 40.0f;
                    cube->OnCreate();
                    cube->SetNaniteAssetIndex(cubeAssetSlot);   // 同一份立方体资产，靠变换区分
                    if (auto* tf = g.world.GetComponent<TransformComponent>(e)) {
                        tf->position = specs[i].pos;
                        tf->rotation = float3(glm::radians(specs[i].eulerDeg.x),
                                              glm::radians(specs[i].eulerDeg.y),
                                              glm::radians(specs[i].eulerDeg.z));
                        tf->scale    = specs[i].scale;
                    }
                    g.sceneGraph->SetParent(e, Entity{kInvalidEntity});
                }
                HE_CORE_INFO("04.App: 多网格变换验收 —— 已生成 3 个立方体（平移 / 旋转 / 非均匀缩放），"
                             "共用立方体资产槽位 {}", cubeAssetSlot);

            }
            u32 boundCheck = 0u;
            g.world.ForEach<MeshComponent>([&](Entity, MeshComponent& mesh) {
                if (mesh.GetNaniteAssetIndex() != 0xFFFFFFFFu) ++boundCheck;
            });
            HE_CORE_INFO("04.App: 多资产绑定自检 —— 已写入资产槽位的网格 {} 个", boundCheck);
        }

        // ── T9：Nanite 开关（cfg → 真值）──
        // 验收要求能在 04 里用 nanite_enable=1 验证「GBuffer 让位给 Nanite」，
        // 因此这三个键与 07 同口径（硬件不支持 / 资产缺失时模块自己回退，见 NaniteRenderer）。
        {
            auto naniteSettings = g.pipeline->GetNaniteSettings();
            naniteSettings.enabled = GetBool(cfg, "nanite_enable", naniteSettings.enabled);
            naniteSettings.offline = GetBool(cfg, "nanite_offline", naniteSettings.offline);
            naniteSettings.multiAsset = GetBool(cfg, "nanite_multi_asset", naniteSettings.multiAsset);
            // 【T8 验收】只画变换件时：**关掉离线 Sponza 资产**（否则 8287 簇的 Sponza 仍会
            //   被单资产路径画出来，验收件被淹没），并强制走多资产路径（立方体资产经注册表绘制）。
            if (g.transformOnly) {
                naniteSettings.offline = false;
                naniteSettings.multiAsset = true;
            }
            // 【T8 验收】多资产档可单独关软光栅：软光栅的资源创建路径与多资产偶发冲突，
            // 硬光栅（T9 默认）已能覆盖多资产绘制验收。
            naniteSettings.softRaster = GetBool(cfg, "nanite_soft_raster", naniteSettings.softRaster);
            // 【T8 排查】假簇链：绕开剔除，直接用 6 个假簇走同一条光栅路径。
            // 若假簇能出画面 ⇒ 光栅/呈现正常，问题在剔除到绘制的数据衔接。
            naniteSettings.fakeChain = GetBool(cfg, "nanite_fake_chain", naniteSettings.fakeChain);
            // 【T11】移除 GBuffer 的 IA 管线（几何只走 Nanite 硬光栅；无 MS 设备自动忽略）
            naniteSettings.removeIaPso = GetBool(cfg, "nanite_remove_ia_pso", naniteSettings.removeIaPso);
            // 【T8】LOD 阈值（像素）：调大 ⇒ 细分到叶子（全细节，慢）
            naniteSettings.lodThresholdPixels = (float)GetInt(cfg, "nanite_lod_threshold", (int)naniteSettings.lodThresholdPixels);
            naniteSettings.hardRasterRefBudget = (u32)GetInt(cfg, "nanite_hard_raster_budget", (int)naniteSettings.hardRasterRefBudget);
            // 【T8】合成实例数：硬光栅的派发上界 = 簇数 × 实例域，太大就会被预算判为超限
            naniteSettings.instanceTestCount = (u32)GetInt(cfg, "nanite_instance_test_count", (int)naniteSettings.instanceTestCount);
            // 【T8】Hi-Z 遮挡剔除（Phase 2 后半）：关掉会让过绘制飙升
            naniteSettings.hiz = GetBool(cfg, "nanite_hiz", naniteSettings.hiz);
            if (const String nanitePath = GetString(cfg, "nanite_offline_path", naniteSettings.offlinePath);
                !nanitePath.empty()) {
                naniteSettings.offlinePath = nanitePath;
            }
            g.pipeline->SetNaniteSettings(naniteSettings);
        // 【T10】阴影绘制路径（cfg `shadow_ms`，默认 IA）
        g.pipeline->SetShadowMeshEnabled(GetBool(cfg, "shadow_ms", false));
            if (naniteSettings.enabled) {
                HE_CORE_INFO("04.App: Nanite 已启用（hardRaster={} offline={} path={}）",
                             naniteSettings.hardRaster ? 1 : 0, naniteSettings.offline ? 1 : 0,
                             naniteSettings.offlinePath);
            }
        }
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

        // 点光源 / 聚光灯：与旧完整版面板同一批配置键（面板改动可跨启动保留）
        if (auto* pl = g.world.GetComponent<PointLight>(g.pointLightEntity)) {
            pl->color      = float3(GetFloat(cfg, "point_color_r", 1.0f),
                                   GetFloat(cfg, "point_color_g", 0.85f),
                                   GetFloat(cfg, "point_color_b", 0.6f));
            pl->intensity  = GetFloat(cfg, "point_intensity", 20.0f);
            pl->range      = GetFloat(cfg, "point_range", 600.0f);
            pl->castShadow = GetBool(cfg, "point_shadow", true);
            pl->shadowBias = GetFloat(cfg, "point_bias", 0.005f);
            if (auto* tf = g.world.GetComponent<TransformComponent>(g.pointLightEntity)) {
                tf->position = float3(GetFloat(cfg, "point_pos_x", -300.0f),
                                      GetFloat(cfg, "point_pos_y", 100.0f),
                                      GetFloat(cfg, "point_pos_z", 0.0f));
            }
        }
        if (auto* sl = g.world.GetComponent<SpotLight>(g.spotLightEntity)) {
            sl->color          = float3(GetFloat(cfg, "spot_color_r", 1.0f),
                                       GetFloat(cfg, "spot_color_g", 0.9f),
                                       GetFloat(cfg, "spot_color_b", 0.7f));
            sl->intensity      = GetFloat(cfg, "spot_intensity", 80.0f);
            sl->range          = GetFloat(cfg, "spot_range", 1200.0f);
            sl->innerConeAngle = GetFloat(cfg, "spot_inner_angle", 0.25f);
            sl->outerConeAngle = GetFloat(cfg, "spot_outer_angle", 0.50f);
            sl->castShadow     = GetBool(cfg, "spot_shadow", true);
            sl->shadowBias     = GetFloat(cfg, "spot_bias", 0.005f);
            if (auto* tf = g.world.GetComponent<TransformComponent>(g.spotLightEntity)) {
                tf->position = float3(GetFloat(cfg, "spot_pos_x", 0.0f),
                                      GetFloat(cfg, "spot_pos_y", 200.0f),
                                      GetFloat(cfg, "spot_pos_z", -200.0f));
            }
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
    // 动画相机模式：先按 AnimationComponent 的位姿覆盖相机，再让输入叠加（与旧 main 顺序一致）
    if (g.animCameraMode && g.camAnim && g.camAnim->playing) {
        if (auto* tf = g.world.GetComponent<TransformComponent>(g.camAnimEntity)) {
            g.camera.SetPosition(tf->position);
            const float3 toOrigin = glm::normalize(float3(0.0f, 200.0f, 0.0f) - tf->position);
            g.camera.SetOrientationFromForward(toOrigin);
        }
    }

    // 相机输入（桌面：右键拖拽 + WASD；鸿蒙：单指拖动映射成右键拖拽）
    // T 键切换"动画相机 / 手动相机"（与旧 04 main 一致）
    if (g.camInput.Update(input, g.camera, deltaTime, {.toggleKey = Key::T})) {
        g.animCameraMode = !g.animCameraMode;
        g.camInput.Reset();   // 切换时丢弃过期光标坐标，避免视角跳一帧
    }
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

    // Transform 动画更新（驱动 AnimationComponent → TransformComponent）
    // 必须在装配快照之前推进，相机动画才会当帧生效（与旧 04 main 一致）。
    g.world.ForEach<AnimationComponent>([&](Entity e, AnimationComponent& anim) {
        if (auto* tf = g.world.GetComponent<TransformComponent>(e)) {
            anim.Update(g.frameDt, tf);
        }
    });

    g.pipeline->GetFrameAssembler().AssembleScene(g.world, *g.sceneGraph, g.camera.GetCamera());
    g.pipeline->Render(g.cmdList.get(), g.pipeline->GetFrameSnapshot(), g.camera.GetCamera());

    // ImGui：LOAD 保留 ToneMap 输出；颜色格式取交换链实际格式（手机上是 RGBA8）
    g.cmdList->BeginRenderPass(1, g.swapchain->GetColorFormat(),
                               rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);
    return true;
}

void App::RecordUIAndPresent() {
    Impl& g = *m_Impl;

    // 【T8 验收钩子】`HE_CAPTURE_FRAME=N` 时在第 N 帧抓一张 PNG（默认关闭；与 01 的口径一致）。
    // 路径写进当前工作目录，便于脚本对比"单资产 vs 多资产"两张图。
    if (const char* captureEnv = std::getenv("HE_CAPTURE_FRAME")) {
        static u32 s_Frame = 0u;
        const int target = std::atoi(captureEnv);
        if (++s_Frame == (u32)(target > 0 ? target : 0)) {
            const char* outName = std::getenv("HE_CAPTURE_PATH");
            RequestScreenshot(outName != nullptr ? outName : "capture_04.png");
            HE_CORE_INFO("04.App: 已请求截图（第 {} 帧 → {}）", s_Frame,
                         outName != nullptr ? outName : "capture_04.png");
        }
    }
    if (g.imgui && g.imgui->IsInitialized()) {
        g.imgui->BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("04.Sponza-Deferred");
        {
            float fps = 1.0f / (g.frameDt > 0.001f ? g.frameDt : 0.016f);
            ImGui::TextColored({0.3f, 1.0f, 0.3f, 1.0f}, "FPS: %.0f", fps);
            ImGui::SameLine(120);
            ImGui::TextColored({0.6f, 0.6f, 0.6f, 1.0f}, "(%.2f ms)", g.frameDt * 1000.0f);

            ImGui::SeparatorText("延迟渲染管线");
            ImGui::Text("GBuffer + Lighting Pass (全屏 PBR)");
            ImGui::Text("3×MRT (albedo+metallic | normal+roughness | emissive+ao) + D32");
            bool clustered = g.pipeline->GetClusteredShading().enabled;
            if (ImGui::Checkbox("Clustered Shading", &clustered))
                g.pipeline->GetClusteredShading().enabled = clustered;
            if (clustered) {
                ImGui::SameLine();
                ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "%u clusters",
                    g.pipeline->GetClusteredShading().GetClusterCount());
            }
            bool gpuCull = g.pipeline->GetGPUCulling().enabled;
            if (ImGui::Checkbox("GPU 视锥剔除", &gpuCull))
                g.pipeline->GetGPUCulling().enabled = gpuCull;
            if (gpuCull) {
                ImGui::SameLine();
                ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "%u 可见",
                    g.pipeline->GetGPUCulling().GetLastVisibleCount());
            }

            // 相机
            ImGui::SeparatorText("相机");
            if (g.animCameraMode) {
                ImGui::TextColored({0.3f, 1.0f, 0.5f, 1.0f}, "动画相机 (按 T 切换手动)");
                bool playing = g.camAnim->playing;
                if (ImGui::Checkbox("播放动画", &playing))
                    g.camAnim->playing = playing;
                ImGui::SameLine();
                float s = g.camAnim->speed;
                if (ImGui::SliderFloat("动画速度", &s, 0.1f, 3.0f, "%.1f"))
                    g.camAnim->speed = s;
            } else {
                ImGui::TextColored({1.0f, 1.0f, 0.3f, 1.0f}, "手动相机 (按 T 切换动画)");
            }
            ImGui::DragFloat3("位置##Camera", &g.camera.GetCamera().position[0], 5.0f);
            float yawDeg   = glm::degrees(g.camera.GetYaw());
            float pitchDeg = glm::degrees(g.camera.GetPitch());
            if (ImGui::SliderFloat("Yaw", &yawDeg, -180.0f, 180.0f, "%.1f°"))
                g.camera.SetOrientation(glm::radians(yawDeg), g.camera.GetPitch());
            if (ImGui::SliderFloat("Pitch", &pitchDeg, -85.0f, 85.0f, "%.1f°"))
                g.camera.SetOrientation(g.camera.GetYaw(), glm::radians(pitchDeg));
            float moveSpeed = g.camera.GetMoveSpeed();
            if (ImGui::DragFloat("移动速度", &moveSpeed, 1.0f, 1.0f, 500.0f, "%.0f"))
                g.camera.SetMoveSpeed(moveSpeed);
            float nearP = g.camera.GetCamera().nearPlane;
            float farP  = g.camera.GetCamera().farPlane;
            if (ImGui::DragFloat("近裁剪面", &nearP, 0.01f, 0.001f, 10.0f, "%.3f"))
                g.camera.GetCamera().nearPlane = nearP;
            if (ImGui::DragFloat("远裁剪面", &farP, 10.0f, 10.0f, 50000.0f, "%.0f"))
                g.camera.GetCamera().farPlane = farP;

            // ── 物理相机参数 ──
            ImGui::SeparatorText("物理相机");
            static render::PhysicalCameraParams physCam;
            static bool usePhysicalCamera = false;
            ImGui::Checkbox("启用物理相机", &usePhysicalCamera);
            if (usePhysicalCamera) {
                ImGui::Indent(12.0f);
                // 镜头
                ImGui::DragFloat("焦距 (mm)", &physCam.focalLength, 1.0f, 10.0f, 200.0f, "%.0f");
                ImGui::DragFloat("光圈 f/", &physCam.fStop, 0.1f, 1.0f, 22.0f, "%.1f");
                // 快门 + ISO
                float shutterRecip = 1.0f / physCam.shutterSpeed;
                ImGui::DragFloat("快门 1/", &shutterRecip, 1.0f, 1.0f, 8000.0f, "%.0f");
                physCam.shutterSpeed = 1.0f / std::max(shutterRecip, 1.0f);
                ImGui::DragFloat("ISO", &physCam.iso, 10.0f, 50.0f, 25600.0f, "%.0f");
                // 传感器预设
                static int sensorPreset = 0;
                const char* sensorNames[] = {"全画幅 36×24", "APS-C 23.6×15.6", "M4/3 17.3×13.0"};
                if (ImGui::Combo("传感器", &sensorPreset, sensorNames, 3)) {
                    if (sensorPreset == 0) physCam.sensor = render::SensorSize::FullFrame();
                    else if (sensorPreset == 1) physCam.sensor = render::SensorSize::APSC();
                    else physCam.sensor = render::SensorSize::Micro43();
                }
                // 对焦距离
                ImGui::DragFloat("对焦距离 (m)", &physCam.focusDistance, 0.1f, 0.1f, 500.0f, "%.1f");

                // 应用物理相机参数到 CameraData
                auto derived = render::DerivePhysicalCamera(physCam,
                    g.camera.GetCamera().aspectRatio);
                g.camera.GetCamera().fov               = derived.fov;
                g.camera.GetCamera().apertureDiameter   = derived.apertureDiameter;
                g.camera.GetCamera().exposureBias       = derived.exposureBias;
                g.camera.GetCamera().motionBlurIntensity = derived.motionBlurIntensity;
                g.camera.GetCamera().focusDistance      = derived.focusDistance;
                g.camera.GetCamera().maxCoC             = derived.maxCoC;
                g.camera.GetCamera().nearPlane          = physCam.nearPlane;
                g.camera.GetCamera().farPlane           = physCam.farPlane;

                // 显示推导值
                ImGui::Text("推导 FOV: %.1f°  |  光圈孔径: %.2f mm", derived.fov, derived.apertureDiameter);
                ImGui::Text("EV 偏置: %+.2f  |  运动模糊: %.2f×", derived.exposureBias, derived.motionBlurIntensity);
                ImGui::Text("弥散圆 max: %.3f", derived.maxCoC);
                ImGui::Unindent(12.0f);
            }

            // GI — IBL
            auto* gi = g.pipeline->GetGI();
            if (gi) {
                ImGui::SeparatorText("GI — IBL");
                auto settings = gi->GetSettings();
                float intensity = settings.intensity;
                if (ImGui::SliderFloat("IBL 强度", &intensity, 0.0f, 3.0f, "%.2f")) {
                    settings.intensity = intensity;
                    gi->SetSettings(settings);
                }
            }

            // GI — SSGI（屏幕空间间接漫反射）
            if (auto* ssgi = g.pipeline->GetSSGI()) {
                ImGui::SeparatorText("GI — SSGI");
                bool ssgiOn = ssgi->IsEnabled();
                if (ImGui::Checkbox("启用 SSGI", &ssgiOn))
                    ssgi->SetEnabled(ssgiOn);
                if (ssgiOn) {
                    ImGui::Indent(12.0f);
                    ImGui::DragFloat("采样半径##ssgi", &ssgi->radius, 0.1f, 0.1f, 5.0f, "%.1f");
                    ImGui::SliderInt("采样数##ssgi", &ssgi->sampleCount, 4, 64);
                    auto ssgiSettings = ssgi->GetSettings();
                    if (ImGui::SliderFloat("强度##ssgi", &ssgiSettings.intensity, 0.0f, 2.0f, "%.2f"))
                        ssgi->SetSettings(ssgiSettings);
                    ImGui::Unindent(12.0f);
                }
            }

            // GI — DDGI（探针网格动态 GI）
            if (auto* ddgi = g.pipeline->GetDDGI()) {
                ImGui::SeparatorText("GI — DDGI");
                bool ddgiOn = ddgi->IsEnabled();
                if (ImGui::Checkbox("启用 DDGI", &ddgiOn))
                    ddgi->SetEnabled(ddgiOn);
                if (ddgiOn) {
                    ImGui::Indent(12.0f);
                    ImGui::SliderFloat("时间混合##ddgi", &ddgi->blendAlpha, 0.0f, 0.98f, "%.2f");
                    ImGui::SliderFloat("贡献缩放##ddgi", &ddgi->debugScale, 0.0f, 2.0f, "%.2f");
                    auto ddgiSettings = ddgi->GetSettings();
                    if (ImGui::SliderFloat("强度##ddgi", &ddgiSettings.intensity, 0.0f, 2.0f, "%.2f"))
                        ddgi->SetSettings(ddgiSettings);
                    if (ImGui::Button("重建探针网格##ddgi")) {
                        // 重新创建探针缓冲以响应参数变化
                    }
                    u32 pc = ddgi->gridX * ddgi->gridY * ddgi->gridZ;
                    ImGui::Text("%u 探针 (%u×%u×%u)", pc, ddgi->gridX, ddgi->gridY, ddgi->gridZ);
                    ImGui::Unindent(12.0f);
                }
            }

            // GI — SSR（屏幕空间反射）
            if (auto* ssr = g.pipeline->GetSSR()) {
                ImGui::SeparatorText("GI — SSR");
                bool ssrOn = ssr->IsEnabled();
                if (ImGui::Checkbox("启用 SSR", &ssrOn))
                    ssr->SetEnabled(ssrOn);
                if (ssrOn) {
                    ImGui::Indent(12.0f);
                    ImGui::SliderFloat("最大步数##ssr", &ssr->maxSteps, 16.0f, 256.0f, "%.0f");
                    ImGui::SliderFloat("步长##ssr", &ssr->stepSize, 0.1f, 2.0f, "%.1f");
                    ImGui::Unindent(12.0f);
                }
            }

            // ── GBuffer 模式 ──
            ImGui::SeparatorText("GBuffer 渲染模式");
            int gbMode = (int)g.pipeline->GetGBufferMode();
            ImGui::RadioButton("CPU Driven", &gbMode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("GPU Driven (ExecuteIndirect)", &gbMode, 1);
            if (gbMode != (int)g.pipeline->GetGBufferMode())
                g.pipeline->SetGBufferMode((render::GBufferRenderer::Mode)gbMode);

            // ── AutoExposure ──
            ImGui::SeparatorText("AutoExposure");
            {
                auto& ae = g.pipeline->GetAutoExposure();
                bool aeOn = ae.IsEnabled();
                if (ImGui::Checkbox("启用自动曝光", &aeOn)) ae.SetEnabled(aeOn);
                if (aeOn) {
                    ImGui::Indent(12.0f);
                    float s = ae.GetAdaptSpeed();
                    if (ImGui::SliderFloat("适应速度", &s, 0.1f, 10.0f, "%.1f")) ae.SetAdaptSpeed(s);
                    float t = ae.GetTargetLum();
                    if (ImGui::SliderFloat("目标亮度", &t, 0.01f, 1.0f, "%.2f")) ae.SetTargetLum(t);
                    ImGui::Text("当前曝光: %.2f", ae.GetExposure());
                    ImGui::Unindent(12.0f);
                }
            }

            // ── ColorGrading ──
            ImGui::SeparatorText("色彩分级");
            {
                auto& cg = g.pipeline->GetColorGrading();
                bool cgOn = cg.IsEnabled();
                if (ImGui::Checkbox("启用色彩分级", &cgOn)) cg.SetEnabled(cgOn);
                if (cgOn) {
                    ImGui::Indent(12.0f);
                    float s = cg.GetSaturation();
                    if (ImGui::SliderFloat("饱和度", &s, 0.0f, 2.0f, "%.2f")) cg.SetSaturation(s);
                    float c = cg.GetContrast();
                    if (ImGui::SliderFloat("对比度", &c, 0.5f, 2.0f, "%.2f")) cg.SetContrast(c);
                    float v = cg.GetVibrance();
                    if (ImGui::SliderFloat("Vibrance", &v, 0.0f, 2.0f, "%.2f")) cg.SetVibrance(v);
                    ImGui::Unindent(12.0f);
                }
            }

            // ── 后处理 ──
            ImGui::SeparatorText("后处理");
            {
                // SSAO
                bool ssaoOn = g.pipeline->GetSSAO().enabled;
                if (ImGui::Checkbox("SSAO", &ssaoOn))
                    g.pipeline->GetSSAO().enabled = ssaoOn;

                // Bloom
                auto& bloom = g.pipeline->GetBloom();
                bool bloomOn = bloom.IsEnabled();
                if (ImGui::Checkbox("Bloom", &bloomOn)) bloom.SetEnabled(bloomOn);
                if (bloomOn) {
                    ImGui::Indent(12.0f);
                    float t = bloom.GetThreshold();
                    if (ImGui::SliderFloat("阈值##bloom", &t, 0.1f, 10.0f, "%.1f")) bloom.SetThreshold(t);
                    float i = bloom.GetIntensity();
                    if (ImGui::SliderFloat("强度##bloom", &i, 0.0f, 2.0f, "%.2f")) bloom.SetIntensity(i);
                    ImGui::Unindent(12.0f);
                }

                // DOF
                auto& dof = g.pipeline->GetDOF();
                bool dofOn = dof.IsEnabled();
                if (ImGui::Checkbox("景深 (DOF)", &dofOn)) dof.SetEnabled(dofOn);
                if (dofOn) {
                    ImGui::Indent(12.0f);
                    float fd = dof.GetFocusDepth();
                    if (ImGui::SliderFloat("对焦深度##dof", &fd, 0.0f, 1.0f, "%.3f")) dof.SetFocusDepth(fd);
                    float fr = dof.GetFocusRange();
                    if (ImGui::SliderFloat("过渡范围##dof", &fr, 0.001f, 0.5f, "%.3f")) dof.SetFocusRange(fr);
                    float di = dof.GetIntensity();
                    if (ImGui::SliderFloat("强度##dof", &di, 0.0f, 2.0f, "%.2f")) dof.SetIntensity(di);
                    ImGui::Unindent(12.0f);
                }

                // MotionBlur
                auto& mb = g.pipeline->GetMotionBlur();
                bool mbOn = mb.IsEnabled();
                if (ImGui::Checkbox("运动模糊 (MB)", &mbOn)) mb.SetEnabled(mbOn);
                if (mbOn) {
                    ImGui::Indent(12.0f);
                    float mi = mb.GetIntensity();
                    if (ImGui::SliderFloat("强度##mb", &mi, 0.0f, 2.0f, "%.2f")) mb.SetIntensity(mi);
                    ImGui::Unindent(12.0f);
                }

                // 硬件 MSAA（HDR 目标多采样，需重启应用生效）
                ImGui::Spacing();
                bool msaaOn = g.pipeline->IsMSAAEnabled();
                if (ImGui::Checkbox("MSAA 4x (硬件)", &msaaOn)) {
                    g.pipeline->EnableMSAA(msaaOn);
                }
                if (msaaOn) {
                    ImGui::SameLine();
                    ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "(重启生效)");
                }

                // LDR 抗锯齿（SMAA 与 FXAA 互斥二选一，与 MSAA 可叠加）
                bool smaaOn = g.pipeline->IsSMAAEnabled();
                bool fxaaOn = g.pipeline->IsFXAAEnabled();
                int ldrAA = smaaOn ? 1 : (fxaaOn ? 2 : 0);
                if (ImGui::Combo("LDR 抗锯齿", &ldrAA, "关闭\0SMAA\0FXAA\0")) {
                    g.pipeline->EnableSMAA(ldrAA == 1);
                    g.pipeline->EnableFXAA(ldrAA == 2);
                }
                if (ldrAA == 1) {
                    ImGui::SameLine();
                    ImGui::TextColored({0.5f, 0.8f, 1.0f, 1.0f}, "(形态学)");
                } else if (ldrAA == 2) {
                    ImGui::SameLine();
                    ImGui::TextColored({1.0f, 0.8f, 0.5f, 1.0f}, "(快速近似)");
                }
            }

            // 光源
            g.world.ForEach<he::DirectionalLight>([&](he::Entity e, he::DirectionalLight& dl) {
                bool isMain = (e == g.mainLightEntity);
                ImGui::SeparatorText(isMain ? "主方向光" : "补光");
                ImGui::Checkbox(isMain ? "启用##MainDL" : "启用##FillDL", &dl.enabled);

                float3 dir = dl.direction;
                if (ImGui::SliderFloat3(isMain ? "方向##MainDL" : "方向##FillDL", &dir[0], -1.0f, 1.0f, "%.2f")) {
                    if (glm::dot(dir, dir) > 0.0001f)
                        dl.direction = glm::normalize(dir);
                }
                ImGui::ColorEdit3(isMain ? "颜色##MainDL" : "颜色##FillDL", &dl.color[0]);
                ImGui::DragFloat(isMain ? "强度##MainDL" : "强度##FillDL", &dl.intensity, 0.1f, 0.0f, 100.0f, "%.1f");

                // 物理光源参数（方向光）
                static bool physDirLight = false;
                ImGui::Checkbox(isMain ? "物理模式##PhysDL" : "物理模式##PhysFillDL", &physDirLight);
                if (physDirLight) {
                    ImGui::Indent(12.0f);
                    ImGui::DragFloat(isMain ? "照度 lux##MainDL" : "照度 lux##FillDL",
                        &dl.illuminance, 1000.0f, 0.0f, 200000.0f, "%.0f");
                    if (dl.illuminance > 0.0f) {
                        ImGui::TextDisabled(isMain ? "(!) 强度将被忽略，使用物理模式" : "(!) intensity ignored, using physical");
                    }
                    ImGui::DragFloat(isMain ? "色温 K##MainDL" : "色温 K##FillDL",
                        &dl.colorTemperature, 50.0f, 0.0f, 12000.0f, "%.0f");
                    ImGui::Unindent(12.0f);
                }

                if (isMain) {
                    bool shadowOn = dl.castShadow;
                    if (ImGui::Checkbox("投射阴影", &shadowOn))
                        dl.castShadow = shadowOn;
                    if (dl.castShadow) {
                        ImGui::Indent(12.0f);
                        ImGui::DragFloat("深度偏移", &dl.shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f",
                            ImGuiSliderFlags_Logarithmic);
                        ImGui::DragFloat("法线偏移", &dl.shadowNormalBias, 0.001f, 0.0f, 0.5f, "%.3f");
                        ImGui::SliderFloat("阴影强度", &dl.shadowStrength, 0.0f, 1.0f, "%.2f");
                        ImGui::Unindent(12.0f);
                    }
                }
            });

            // 点光源
            g.world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
                ImGui::SeparatorText("点光源");
                ImGui::Checkbox("启用##PointEnable", &pl.enabled);
                auto* plTransform = g.world.GetComponent<TransformComponent>(e);
                if (plTransform) {
                    if (ImGui::DragFloat3("位置##PointLight", &plTransform->position[0], 5.0f)) {
                        auto* sphereTransform = g.world.GetComponent<TransformComponent>(g.pointLightSphereEntity);
                        if (sphereTransform)
                            sphereTransform->position = plTransform->position;
                    }
                }
                ImGui::ColorEdit3("颜色", &pl.color[0]);
                ImGui::DragFloat("强度", &pl.intensity, 0.1f, 0.0f, 200.0f, "%.1f");
                ImGui::DragFloat("范围", &pl.range, 10.0f, 10.0f, 5000.0f, "%.0f");

                // 物理光源参数（点光源）
                static bool physPointLight = false;
                ImGui::Checkbox("物理模式##PhysPoint", &physPointLight);
                if (physPointLight) {
                    ImGui::Indent(12.0f);
                    ImGui::DragFloat("发光强度 cd##Point", &pl.luminousIntensity, 1.0f, 0.0f, 100000.0f, "%.0f");
                    if (pl.luminousIntensity > 0.0f) {
                        ImGui::TextDisabled("(!) 强度将被忽略，使用物理模式");
                    }
                    ImGui::DragFloat("色温 K##Point", &pl.colorTemperature, 50.0f, 0.0f, 12000.0f, "%.0f");
                    ImGui::Unindent(12.0f);
                }

                bool shadowOn = pl.castShadow;
                if (ImGui::Checkbox("投射阴影##PointShadow", &shadowOn))
                    pl.castShadow = shadowOn;
                if (pl.castShadow) {
                    ImGui::Indent(12.0f);
                    ImGui::DragFloat("深度偏移##PointBias", &pl.shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f",
                        ImGuiSliderFlags_Logarithmic);
                    ImGui::Unindent(12.0f);
                }
            });

            // 聚光灯
            g.world.ForEach<he::SpotLight>([&](he::Entity e, he::SpotLight& sl) {
                ImGui::SeparatorText("聚光灯");
                ImGui::Checkbox("启用##SpotEnable", &sl.enabled);
                auto* slTransform = g.world.GetComponent<TransformComponent>(e);
                if (slTransform) {
                    if (ImGui::DragFloat3("位置##SpotLight", &slTransform->position[0], 5.0f)) {
                        auto* coneTransform = g.world.GetComponent<TransformComponent>(g.spotLightConeEntity);
                        if (coneTransform)
                            coneTransform->position = slTransform->position;
                    }
                }
                float3 dir = sl.direction;
                if (ImGui::SliderFloat3("方向##SpotDir", &dir[0], -1.0f, 1.0f, "%.2f")) {
                    if (glm::dot(dir, dir) > 0.0001f)
                        sl.direction = glm::normalize(dir);
                }
                ImGui::ColorEdit3("颜色##SpotColor", &sl.color[0]);
                ImGui::DragFloat("强度##SpotIntensity", &sl.intensity, 0.1f, 0.0f, 200.0f, "%.1f");
                ImGui::DragFloat("范围##SpotRange", &sl.range, 10.0f, 10.0f, 5000.0f, "%.0f");
                ImGui::SliderFloat("内锥角##SpotInner", &sl.innerConeAngle, 0.01f, 1.5f, "%.2f rad");
                ImGui::SliderFloat("外锥角##SpotOuter", &sl.outerConeAngle, 0.01f, 1.5f, "%.2f rad");
                bool shadowOn = sl.castShadow;
                if (ImGui::Checkbox("投射阴影##SpotShadow", &shadowOn))
                    sl.castShadow = shadowOn;
                if (sl.castShadow) {
                    ImGui::Indent(12.0f);
                    ImGui::DragFloat("深度偏移##SpotBias", &sl.shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f",
                        ImGuiSliderFlags_Logarithmic);
                    ImGui::Unindent(12.0f);
                }
            });

            // 同步球体位置
            {
                auto* plTransform = g.world.GetComponent<TransformComponent>(g.pointLightEntity);
                auto* sphereTransform = g.world.GetComponent<TransformComponent>(g.pointLightSphereEntity);
                if (plTransform && sphereTransform)
                    sphereTransform->position = plTransform->position;
            }

            // ── GPU Profiler ──
            ImGui::SeparatorText("GPU Profiler");
            auto& pdata = g.pipeline->GetProfiler().GetLastFrameData();
            float totalMs = 0;
            for (auto& p : pdata) {
                if (p.gpuMs < 0) continue;  // 未使用
                totalMs += p.gpuMs;
                ImGui::Text("%-20s %6.2fms", p.name.c_str(), p.gpuMs);
            }
            ImGui::Separator();
            ImGui::Text("Total GPU: %.2fms (%.0f FPS)", totalMs, totalMs > 0 ? 1000.0f / totalMs : 0);

            // 场景统计
            ImGui::SeparatorText("场景");
            u32 meshCount = 0, dirLightCount = 0, spotCount = 0;
            g.world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent&) { meshCount++; });
            g.world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight&) { dirLightCount++; });
            g.world.ForEach<he::SpotLight>([&](he::Entity, he::SpotLight&) { spotCount++; });
            ImGui::Text("%u 网格  |  %u 方向光  |  %u 点光  |  %u 聚光", meshCount, dirLightCount, 1, spotCount);
        }
        ImGui::End();

        // GPU Profiler 面板（按 F1 切换；与旧完整版一致）
        if (ImGui::IsKeyPressed(ImGuiKey_F1))
            g.pipeline->GetProfilerPanel().Toggle();
        g.pipeline->GetProfilerPanel().Draw();

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
        // T8：Nanite 开关与离线资产路径（必须回写，否则下一次运行丢配置）
        {
            const auto& nanite = g.pipeline->GetNaniteSettings();
            out["nanite_enable"]      = std::to_string(nanite.enabled ? 1 : 0);
            out["nanite_offline"]     = std::to_string(nanite.offline ? 1 : 0);
            out["nanite_offline_path"] = nanite.offlinePath;
            out["nanite_soft_raster"] = std::to_string(nanite.softRaster ? 1 : 0);
            out["nanite_fake_chain"] = std::to_string(nanite.fakeChain ? 1 : 0);
            out["nanite_remove_ia_pso"] = std::to_string(nanite.removeIaPso ? 1 : 0);
            out["shadow_ms"] = std::to_string(g.pipeline->GetShadowMeshEnabled() ? 1 : 0);
            out["nanite_multi_asset"] = std::to_string(nanite.multiAsset ? 1 : 0);
            // 【修正】这几个键此前只读不写 ⇒ 样例退出时回写 cfg 会把它们**抹掉**，
            //   下次启动就回到默认值（实例域 64 会让 Sponza 的硬光栅上界超预算而被自动关闭）。
            out["nanite_multi_asset_count"] = std::to_string(g.multiAssetCount);
            out["nanite_transform_test"] = std::to_string(g.transformTest ? 1 : 0);
            out["nanite_transform_only"] = std::to_string(g.transformOnly ? 1 : 0);
            out["nanite_hiz"] = std::to_string(nanite.hiz ? 1 : 0);
            out["nanite_lod_threshold"] = std::to_string((int)nanite.lodThresholdPixels);
            out["nanite_hard_raster_budget"] = std::to_string(nanite.hardRasterRefBudget);
            out["nanite_instance_test_count"] = std::to_string(nanite.instanceTestCount);
        }
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

        // 点光源 / 聚光灯的配置写回（与旧完整版一致：面板改动跨启动保留）
        if (auto* pl = g.world.GetComponent<PointLight>(g.pointLightEntity)) {
            out["point_color_r"] = std::to_string(pl->color.x);
            out["point_color_g"] = std::to_string(pl->color.y);
            out["point_color_b"] = std::to_string(pl->color.z);
            out["point_intensity"] = std::to_string(pl->intensity);
            out["point_range"]     = std::to_string(pl->range);
            out["point_shadow"]    = std::to_string(pl->castShadow ? 1 : 0);
            out["point_bias"]      = std::to_string(pl->shadowBias);
            out["point_enabled"]   = std::to_string(pl->enabled ? 1 : 0);
            if (auto* tf = g.world.GetComponent<TransformComponent>(g.pointLightEntity)) {
                out["point_pos_x"] = std::to_string(tf->position.x);
                out["point_pos_y"] = std::to_string(tf->position.y);
                out["point_pos_z"] = std::to_string(tf->position.z);
            }
        }
        if (auto* sl = g.world.GetComponent<SpotLight>(g.spotLightEntity)) {
            out["spot_color_r"] = std::to_string(sl->color.x);
            out["spot_color_g"] = std::to_string(sl->color.y);
            out["spot_color_b"] = std::to_string(sl->color.z);
            out["spot_intensity"]    = std::to_string(sl->intensity);
            out["spot_range"]        = std::to_string(sl->range);
            out["spot_inner_angle"]  = std::to_string(sl->innerConeAngle);
            out["spot_outer_angle"]  = std::to_string(sl->outerConeAngle);
            out["spot_shadow"]       = std::to_string(sl->castShadow ? 1 : 0);
            out["spot_bias"]         = std::to_string(sl->shadowBias);
            out["spot_enabled"]      = std::to_string(sl->enabled ? 1 : 0);
            if (auto* tf = g.world.GetComponent<TransformComponent>(g.spotLightEntity)) {
                out["spot_pos_x"] = std::to_string(tf->position.x);
                out["spot_pos_y"] = std::to_string(tf->position.y);
                out["spot_pos_z"] = std::to_string(tf->position.z);
            }
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
    // 【派生类型要单独清】ECS 的 ForEach<MeshComponent> 不命中派生组件（SphereComponent 等），
    // 点光源/聚光灯的可视化小球正是 SphereComponent，漏掉会在设备销毁时触发 VMA 断言。
    g.world.ForEach<SphereComponent>([](Entity, SphereComponent& sphere) {
        sphere.GetVertexBuffer().reset();
        sphere.GetIndexBuffer().reset();
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
    // 【T8 诊断】装崩溃处理器：多资产档异常时把模块/偏移/调用栈落到 Content/Config 下，
    // 比"窗口只是说 stopped working"更容易定位（06/07 已有同款做法）。
    he::InstallCrashHandler("Content/Config/04_Sponza-Deferred_crash.log", nullptr);
    he::samples::desktop::HostConfig cfg;
    cfg.title  = "HugEngine — 04.Sponza-Deferred";
    cfg.width  = 960;
    cfg.height = 540;
    return he::samples::desktop::RunApp<he::samples::sponza04::App>(cfg);
}
#endif // HE_DESKTOP_ENTRY
