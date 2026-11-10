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
#include "Pipeline/RTPass.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/AnimationComponent.h"   // 相机平移动画演示（面板可播放/调速）
#include "Scene/SphereComponent.h"      // 点光源的可视化小球
#include "Asset/glTFLoader.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include "CameraInput.h"     // Samples/Common：消费 InputState 的相机控制

// RT 路径的着色器字节码（CompileShaders / CompileShadersDxil 生成）
#include "RT_Sponza.rgen.spv.h"
#include "RT_Sponza.rgen.dxil.h"
#include "RT_Sponza.rmiss.spv.h"
#include "RT_Sponza.rmiss.dxil.h"
#include "RT_Sponza.rchit.spv.h"
#include "RT_Sponza.rchit.dxil.h"
#include "RT_Bindless.rcall.spv.h"
#include "RT_Bindless.rcall.dxil.h"
#include "SkyboxLoader.h"  // Samples/Common：HDR 全景 → 6 面 Cubemap

// STB_IMAGE_STATIC：让实现留在本 TU 内部，避免与 02.App 等其它 TU 的实现重复定义
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace he::samples::sponza03 {

namespace {

/// 配置文件：相机与两盏方向光（key=value；路径走 vfs 的可写目录）
String ConfigPath() {
    return vfs::ResolveWritable("Config/03_Sponza-Forward.cfg");
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

} // namespace

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
    f32          frameDt = 0.016f;   // 本帧 deltaTime（面板 FPS 用）

    // ---- RT 路径 ----
    bool    rtSupported = false;
    int     renderMode  = 0;          // 0=光栅化 1=RT
    int     rtSampleCount = 1;        // RT 每像素采样数 1..16
    u64     frameIndex    = 0;
    render::RTPass rtPass;
    rhi::DescriptorSetLayoutHandle rtLayout0 = rhi::kInvalidLayout;
    rhi::DescriptorSetLayoutHandle rtLayout1 = rhi::kInvalidLayout;

    // ---- 配置保存要用的句柄 ----
    Entity mainLightEntity{};
    Entity fillLightEntity{};
    Entity pointLightEntity{};        // 点光源（面板可调位置/颜色/强度/范围/阴影）
    Entity pointLightSphereEntity{};  // 点光源位置上的可视化小球

    // ---- 相机平移动画（AnimationComponent 演示）----
    Entity               camAnimEntity{};
    AnimationComponent*  camAnim = nullptr;
    bool                 animCameraMode = false;   // T 键切换动画相机/手动相机

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
        g.mainLightEntity = e;
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
        g.fillLightEntity = e;
        g.world.AddComponent<TransformComponent>(e);
        auto* dl = g.world.AddComponent<DirectionalLight>(e);
        dl->direction = float3(-0.3f, -0.4f, -0.5f);
        dl->color     = float3(0.6f, 0.7f, 0.9f);
        dl->intensity = 2.0f;
        g.sceneGraph->SetParent(e, Entity{kInvalidEntity});
    }

    // ---- 点光源（测试点光阴影；面板可调位置/颜色/强度/范围）----
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
        sphere->radius       = 15.0f;   // Sponza 尺度较大
        sphere->segmentCount = 16;
        sphere->ringCount    = 8;
        sphere->OnCreate();             // 用新参数重建几何体
        if (auto* tf = g.world.GetComponent<TransformComponent>(s)) {
            tf->position = float3(-300.0f, 100.0f, 0.0f);
        }
        g.sceneGraph->SetParent(s, Entity{kInvalidEntity});
    }

    // ---- 相机平移动画（AnimationComponent 演示：绕 Y 轴 6 秒一圈）----
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
        g.camAnim->playing = false;   // 默认关闭，按 T 或在面板里勾"播放动画"
        g.sceneGraph->SetParent(e, Entity{kInvalidEntity});
        HE_CORE_INFO("03.App: 相机动画已创建（6 秒圆形路径，默认不播放）");
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

    // ---- RT 路径初始化（仅设备支持时）----
    g.rtSupported = g.device->GetCaps().supportsRayTracing;
    if (g.rtSupported) {
        // set=0：TLAS + BackBuffer + Callable 用的 bindless 纹理/采样器
        rhi::DescriptorSetLayoutDesc rtSet0Desc;
        rtSet0Desc.bindings = {
            { 0, rhi::DescriptorType::AccelerationStructure, 1,    0x100 },
            { 1, rhi::DescriptorType::StorageImage,          1,    0x100 },
            { 5, rhi::DescriptorType::SampledImage,          4096, 0x2000, true },
            { 6, rhi::DescriptorType::Sampler,               4096, 0x2000, true },
        };
        g.rtLayout0 = g.device->CreateDescriptorSetLayout(rtSet0Desc);

        // set=1：材质纹理 + 光源 UB（ClosestHit 使用）
        rhi::DescriptorSetLayoutDesc rtSet1Desc;
        rtSet1Desc.bindings = {
            { 0, rhi::DescriptorType::SampledImage,  1, rhi::kStageMaskClosestHit },
            { 1, rhi::DescriptorType::UniformBuffer, 1, rhi::kStageMaskClosestHit },
        };
        g.rtLayout1 = g.device->CreateDescriptorSetLayout(rtSet1Desc);

        rhi::ShaderBytecode rgen  = he::rhi::MakeShaderBytecode(rhi::ShaderStage::RayGen,     k_RT_Sponza_rgen_spv,  k_RT_Sponza_rgen_dxil,  "main");
        rhi::ShaderBytecode rmiss = he::rhi::MakeShaderBytecode(rhi::ShaderStage::Miss,       k_RT_Sponza_rmiss_spv, k_RT_Sponza_rmiss_dxil, "main");
        rhi::ShaderBytecode rchit = he::rhi::MakeShaderBytecode(rhi::ShaderStage::ClosestHit, k_RT_Sponza_rchit_spv, k_RT_Sponza_rchit_dxil, "main");
        rhi::ShaderBytecode rcall = he::rhi::MakeShaderBytecode(rhi::ShaderStage::Callable,   k_RT_Bindless_rcall_spv, k_RT_Bindless_rcall_dxil, "main");

        std::vector<rhi::RTShaderGroup> rtGroups = {
            { rhi::RTShaderGroupType::RayGen,   0, ~0u, ~0u, ~0u, "RayGen" },
            { rhi::RTShaderGroupType::Miss,     1, ~0u, ~0u, ~0u, "Miss"   },
            { rhi::RTShaderGroupType::Hit,     ~0u, 2,   ~0u, ~0u, "Hit"    },
            { rhi::RTShaderGroupType::Callable, 3, ~0u, ~0u, ~0u, "BindlessCall" },
        };

        rhi::PushConstantRange pcRange;
        pcRange.stageMask = 0x100;
        pcRange.offset    = 0;
        pcRange.size      = 96;   // float4x4(64) + float4(16) + uint4(16)

        std::vector<rhi::DescriptorSetLayoutHandle> rtLayouts = { g.rtLayout0, g.rtLayout1 };
        g.rtPass.Initialize(g.device.get(), { rgen, rmiss, rchit, rcall }, rtGroups, rtLayouts, pcRange);
        g.rtPass.CreateMaterialTexture(g.device.get(), 256, g.pipeline->GetFrameSnapshot());
        g.rtPass.CreateLightBuffer(g.device.get(), 8);
        g.device->GetBindlessHeap()->RegisterDescriptorSet(
            g.rtPass.GetDescriptorSet0(), rhi::kBindingBindlessTextures,
            rhi::kBindingBindlessSamplers, 0);
        HE_CORE_INFO("03.App: RT 路径初始化完成（{}）", g.rtPass.IsValid() ? "就绪" : "不可用");
    } else {
        HE_CORE_WARN("03.App: 设备不支持 Ray Tracing，RT 模式不可用");
    }

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

    // ---- 配置：相机与灯光（没有配置文件就用上面的默认值）----
    const String cfgPath = ConfigPath();
    const auto cfg = LoadConfigFile(cfgPath);
    if (!cfg.empty()) {
        HE_CORE_INFO("03.App: 加载配置文件 {}", cfgPath);
        g.camera.SetPosition(float3(GetFloat(cfg, "cam_pos_x", 0.0f),
                                    GetFloat(cfg, "cam_pos_y", 2.5f),
                                    GetFloat(cfg, "cam_pos_z", -10.0f)));
        g.camera.SetOrientation(GetFloat(cfg, "cam_yaw", 0.0f), GetFloat(cfg, "cam_pitch", 0.0f));
        g.camera.GetCamera().nearPlane = GetFloat(cfg, "cam_near", 0.1f);
        g.camera.GetCamera().farPlane  = GetFloat(cfg, "cam_far", 2000.0f);
        g.renderMode = static_cast<int>(GetFloat(cfg, "render_mode", 0.0f));
        g.rtSampleCount = static_cast<int>(GetFloat(cfg, "rt_spp", 1.0f));
        // 点光源（与面板一致：退出时保存 → 启动时恢复）
        if (auto* pl = g.world.GetComponent<PointLight>(g.pointLightEntity)) {
            pl->color      = float3(GetFloat(cfg, "point_color_r", 1.0f),
                                    GetFloat(cfg, "point_color_g", 0.85f),
                                    GetFloat(cfg, "point_color_b", 0.6f));
            pl->intensity  = GetFloat(cfg, "point_intensity", 20.0f);
            pl->range      = GetFloat(cfg, "point_range", 600.0f);
            pl->castShadow = GetFloat(cfg, "point_shadow", 1.0f) != 0.0f;
            pl->shadowBias = GetFloat(cfg, "point_shadow_bias", 0.005f);
            if (auto* tf = g.world.GetComponent<TransformComponent>(g.pointLightEntity)) {
                tf->position = float3(GetFloat(cfg, "point_pos_x", -300.0f),
                                      GetFloat(cfg, "point_pos_y", 100.0f),
                                      GetFloat(cfg, "point_pos_z", 0.0f));
            }
        }


        if (auto* dl = g.world.GetComponent<DirectionalLight>(g.mainLightEntity)) {
            dl->direction = float3(GetFloat(cfg, "light_dir_x", 0.4f),
                                   GetFloat(cfg, "light_dir_y", -1.0f),
                                   GetFloat(cfg, "light_dir_z", 0.6f));
            dl->color     = float3(GetFloat(cfg, "light_color_r", 1.0f),
                                   GetFloat(cfg, "light_color_g", 0.95f),
                                   GetFloat(cfg, "light_color_b", 0.85f));
            dl->intensity = GetFloat(cfg, "light_intensity", 15.0f);
            dl->castShadow = GetFloat(cfg, "shadow_enabled", 1.0f) != 0.0f;
            dl->shadowBias = GetFloat(cfg, "shadow_bias", 0.003f);
        }
    }

    if (opt.enableImGui) {
        g.imgui = std::make_unique<editor::ImGuiIntegration>();
        g.imgui->Initialize(engine.GetWindow(), g.device.get(), g.swapchain.get());
    }

    g.ready = true;
    HE_CORE_INFO("03.App 初始化完成（{}x{}）", g.swapchain->GetWidth(), g.swapchain->GetHeight());
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
    // T 键切换"动画相机 / 手动相机"（与旧 03 main 一致）
    if (g.camInput.Update(input, g.camera, deltaTime, {.toggleKey = Key::T})) {
        g.animCameraMode = !g.animCameraMode;
        g.camInput.Reset();   // 切回手动时丢弃过期光标坐标，避免视角跳一帧
    }
    g.frameDt = deltaTime;   // 面板 FPS 用
    return true;
}

bool App::RecordScene() {
    Impl& g = *m_Impl;
    if (!g.ready || !g.presenter) return false;

    // Acquire 失败时返回 false，宿主会跳过本帧的第二条命令（UI + Present）
    if (!g.presenter->BeginFrame()) return false;
    g.presenter->BeginRecording(g.device.get(), g.cmdList.get());

    g.pipeline->NextFrame();

    // Transform 动画更新（驱动 AnimationComponent → TransformComponent）
    // 与旧 03 main 一致：必须在装配快照之前推进，相机动画才会当帧生效。
    g.world.ForEach<AnimationComponent>([&](Entity e, AnimationComponent& anim) {
        if (auto* tf = g.world.GetComponent<TransformComponent>(e)) {
            anim.Update(g.frameDt, tf);
        }
    });
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

    if (g.renderMode == 1 && g.rtPass.IsValid()) {
        // ---- RT 路径：光追直接写后缓冲，覆盖上面光栅化的输出 ----
        g.rtPass.BuildAS(g.cmdList.get(), g.pipeline->GetFrameSnapshot(), g.pipeline->GetMeshRegistry());
        g.rtPass.UpdateLightBuffer(g.pipeline->GetCurrentLightBuffer());
        g.rtPass.UpdateRTDescriptorSet(g.device.get(),
            g.swapchain->GetCurrentBackBufferView(),
            g.pipeline->GetCurrentObjectBuffer());

        struct RTPushConstant {
            float4x4 invViewProj;
            float4   camPosNearFar;
            u32      sampleCount;
            u32      frameIndex;
            u32      _pad0;
            u32      _pad1;
        };
        RTPushConstant rtPC;
        const auto& camData = g.camera.GetCamera();
        rtPC.invViewProj   = glm::inverse(camData.GetViewProjMatrix());
        rtPC.camPosNearFar = float4(camData.position.x, camData.position.y,
                                    camData.position.z, camData.nearPlane);
        rtPC.sampleCount   = static_cast<u32>(g.rtSampleCount);
        rtPC.frameIndex    = static_cast<u32>(g.frameIndex % 1000000);

        g.cmdList->PipelineBarrier(rhi::PipelineStage::BottomOfPipe,
                                   rhi::PipelineStage::RayTracingShader,
                                   rhi::ResourceState::Undefined,
                                   rhi::ResourceState::UnorderedAccess);
        g.rtPass.BindPipeline(g.cmdList.get());
        g.rtPass.BindDescriptorSets(g.cmdList.get());
        g.cmdList->SetPushConstants(0, sizeof(RTPushConstant), &rtPC);
        g.rtPass.TraceRays(g.cmdList.get(), g.swapchain->GetWidth(), g.swapchain->GetHeight());
        g.cmdList->PipelineBarrier(rhi::PipelineStage::RayTracingShader,
                                   rhi::PipelineStage::ColorAttachmentOutput,
                                   rhi::ResourceState::UnorderedAccess,
                                   rhi::ResourceState::RenderTarget);
        g.cmdList->SetPipeline(g.pipeline->GetPipelineState());
        g.cmdList->BeginRenderPass(1, g.swapchain->GetColorFormat(),
                                   rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);
    } else {
        // BackBuffer 合成：ToneMap + ImGui（ImGui 的录制在 RecordUIAndPresent 里）
        g.cmdList->BeginDebugLabel("ToneMap + ImGui (BackBuffer)");
        g.cmdList->BeginRenderPass(1, g.swapchain->GetColorFormat());
        g.pipeline->RenderToneMapPass(g.cmdList.get());
    }
    ++g.frameIndex;
    return true;
}

void App::RecordUIAndPresent() {
    Impl& g = *m_Impl;
    if (g.imgui && g.imgui->IsInitialized()) {
        g.imgui->BeginFrame();
        const float ui = ImGui::GetFontSize() / 16.0f;   // 高 DPI 缩放
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
        // 宽度交给 ImGui 自适应（旧版没有强制宽度，固定 340 会把 "Ray Tracing" 裁掉）
        ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f), ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("03.Sponza-Forward 控制面板");
        {
            // ===== 性能监控 =====
            const float fps = 1.0f / (g.frameDt > 0.001f ? g.frameDt : 0.016f);
            ImGui::TextColored({0.3f, 1.0f, 0.3f, 1.0f}, "FPS: %.0f", fps);
            ImGui::SameLine(120);
            ImGui::TextColored({0.6f, 0.6f, 0.6f, 1.0f}, "(%.2f ms)", g.frameDt * 1000.0f);

            // ===== 渲染模式 =====
            ImGui::SeparatorText("渲染模式");
            if (g.rtSupported) {
                ImGui::RadioButton("光栅化 (ForwardPipeline)", &g.renderMode, 0);
                ImGui::SameLine();
                ImGui::RadioButton("Ray Tracing", &g.renderMode, 1);
            } else {
                ImGui::TextColored({0.5f, 0.5f, 0.5f, 1.0f}, "RT 不可用（设备不支持）");
            }
            if (g.renderMode == 1) {
                ImGui::SeparatorText("RT 采样");
                ImGui::SliderInt("SPP", &g.rtSampleCount, 1, 16);
                ImGui::TextColored({0.5f, 0.5f, 0.5f, 1.0f},
                    "每像素 %d 条光线 (%dx 性能开销)", g.rtSampleCount, g.rtSampleCount);
            }

            // ===== 相机 =====
            ImGui::SeparatorText("相机");
            if (g.animCameraMode) {
                ImGui::TextColored({0.3f, 1.0f, 0.5f, 1.0f}, "动画相机 (按 T 切换手动)");
                bool playing = g.camAnim->playing;
                if (ImGui::Checkbox("播放动画", &playing)) g.camAnim->playing = playing;
                ImGui::SameLine();
                float animSpeed = g.camAnim->speed;
                if (ImGui::SliderFloat("动画速度", &animSpeed, 0.1f, 3.0f, "%.1f")) g.camAnim->speed = animSpeed;
            } else {
                ImGui::TextColored({1.0f, 1.0f, 0.3f, 1.0f}, "手动相机 (按 T 切换动画)");
            }
            ImGui::DragFloat3("位置##Camera", &g.camera.GetCamera().position[0], 5.0f);
            float yawDeg   = glm::degrees(g.camera.GetYaw());
            float pitchDeg = glm::degrees(g.camera.GetPitch());
            if (ImGui::SliderFloat("Yaw", &yawDeg, -180.0f, 180.0f, "%.1f"))
                g.camera.SetOrientation(glm::radians(yawDeg), g.camera.GetPitch());
            if (ImGui::SliderFloat("Pitch", &pitchDeg, -85.0f, 85.0f, "%.1f"))
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

            bool multiThread = g.pipeline->IsMultiThreadedRecording();
            if (ImGui::Checkbox("多线程录制", &multiThread))
                g.pipeline->SetMultiThreadedRecording(multiThread);
            bool gpuCull = g.pipeline->GetGPUCulling().enabled;
            if (ImGui::Checkbox("GPU 视锥剔除", &gpuCull))
                g.pipeline->GetGPUCulling().enabled = gpuCull;
            if (gpuCull) {
                ImGui::SameLine();
                ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "可见 %u",
                    g.pipeline->GetGPUCulling().GetLastVisibleCount());
            }

            // ===== GI =====
            if (auto* gi = g.pipeline->GetGI()) {
                ImGui::SeparatorText("GI");
                auto settings   = gi->GetSettings();
                float intensity = settings.intensity;
                if (ImGui::SliderFloat("IBL Intensity", &intensity, 0.0f, 3.0f, "%.2f")) {
                    settings.intensity = intensity;
                    gi->SetSettings(settings);
                }
            }

            // ===== GPU Profiler =====
            ImGui::SeparatorText("GPU Profiler");
            {
                auto& pdata   = g.pipeline->GetProfiler().GetLastFrameData();
                float totalMs = 0.0f;
                for (auto& pf : pdata) {
                    if (pf.gpuMs < 0.0f) continue;
                    totalMs += pf.gpuMs;
                    ImGui::Text("%-20s %6.2fms", pf.name.c_str(), pf.gpuMs);
                }
                ImGui::Separator();
                ImGui::Text("Total GPU: %.2fms (%.0f FPS)", totalMs,
                            totalMs > 0.0f ? 1000.0f / totalMs : 0.0f);
            }

            // ===== 渲染设置 =====
            ImGui::SeparatorText("渲染设置");
            bool useIndirect = g.pipeline->GetUseExecuteIndirect();
            if (ImGui::Checkbox("GPU Driven (ExecuteIndirect)", &useIndirect))
                g.pipeline->SetUseExecuteIndirect(useIndirect);

            // ===== 光源 =====
            g.world.ForEach<DirectionalLight>([&](Entity e, DirectionalLight& dl) {
                const bool isMain = (e == g.mainLightEntity);
                ImGui::SeparatorText(isMain ? "主方向光" : "补光");
                ImGui::Checkbox(isMain ? "启用##MainDL" : "启用##FillDL", &dl.enabled);
                float3 dir = dl.direction;
                if (ImGui::SliderFloat3(isMain ? "方向##MainDL" : "方向##FillDL", &dir[0], -1.0f, 1.0f, "%.2f")) {
                    if (glm::dot(dir, dir) > 0.0001f) dl.direction = glm::normalize(dir);
                }
                ImGui::ColorEdit3(isMain ? "颜色##MainDL" : "颜色##FillDL", &dl.color[0]);
                ImGui::DragFloat(isMain ? "强度##MainDL" : "强度##FillDL", &dl.intensity, 0.1f, 0.0f, 100.0f, "%.1f");
                if (isMain) {
                    bool shadowOn = dl.castShadow;
                    if (ImGui::Checkbox("投射阴影", &shadowOn)) dl.castShadow = shadowOn;
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

            // ===== 点光源 =====
            g.world.ForEach<PointLight>([&](Entity e, PointLight& pl) {
                ImGui::SeparatorText("点光源");
                ImGui::Checkbox("启用##PointEnable", &pl.enabled);
                if (auto* plTransform = g.world.GetComponent<TransformComponent>(e)) {
                    if (ImGui::DragFloat3("位置##PointLight", &plTransform->position[0], 5.0f)) {
                        if (auto* sphereTransform = g.world.GetComponent<TransformComponent>(g.pointLightSphereEntity))
                            sphereTransform->position = plTransform->position;
                    }
                }
                ImGui::ColorEdit3("颜色##PointLight", &pl.color[0]);
                ImGui::DragFloat("强度##PointLight", &pl.intensity, 0.1f, 0.0f, 200.0f, "%.1f");
                ImGui::DragFloat("范围##PointLight", &pl.range, 10.0f, 10.0f, 5000.0f, "%.0f");
                bool shadowOn = pl.castShadow;
                if (ImGui::Checkbox("投射阴影##PointShadow", &shadowOn)) pl.castShadow = shadowOn;
                if (pl.castShadow) {
                    ImGui::Indent(12.0f);
                    ImGui::DragFloat("深度偏移##PointBias", &pl.shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f",
                                     ImGuiSliderFlags_Logarithmic);
                    ImGui::Unindent(12.0f);
                }
            });

            // 每帧确保球体与点光源位置一致（非面板路径改了位置时也能同步）
            {
                auto* plTransform     = g.world.GetComponent<TransformComponent>(g.pointLightEntity);
                auto* sphereTransform = g.world.GetComponent<TransformComponent>(g.pointLightSphereEntity);
                if (plTransform && sphereTransform) sphereTransform->position = plTransform->position;
            }

            // ===== 场景统计 =====
            ImGui::SeparatorText("场景");
            u32 meshCount = 0, dirLightCount = 0, pointLightCount = 0;
            g.world.ForEach<MeshComponent>([&](Entity, MeshComponent&) { meshCount++; });
            g.world.ForEach<DirectionalLight>([&](Entity, DirectionalLight&) { dirLightCount++; });
            g.world.ForEach<PointLight>([&](Entity, PointLight&) { pointLightCount++; });
            ImGui::Text("%u 网格  |  %u 方向光  |  %u 点光 + 可视化球体",
                        meshCount, dirLightCount, pointLightCount);
        }
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

    // ---- 保存配置（相机 + 灯光；退出前保存，崩溃时最多丢最后一次改动）----
    {
        std::unordered_map<String, String> out;
        out["cam_pos_x"] = std::to_string(g.camera.GetCamera().position.x);
        out["cam_pos_y"] = std::to_string(g.camera.GetCamera().position.y);
        out["cam_pos_z"] = std::to_string(g.camera.GetCamera().position.z);
        out["cam_yaw"]   = std::to_string(g.camera.GetYaw());
        out["cam_pitch"] = std::to_string(g.camera.GetPitch());
        out["cam_near"]  = std::to_string(g.camera.GetCamera().nearPlane);
        out["cam_far"]   = std::to_string(g.camera.GetCamera().farPlane);
        out["render_mode"] = std::to_string(g.renderMode);
        out["rt_spp"]      = std::to_string(g.rtSampleCount);
        if (auto* pl = g.world.GetComponent<PointLight>(g.pointLightEntity)) {
            out["point_color_r"]     = std::to_string(pl->color.x);
            out["point_color_g"]     = std::to_string(pl->color.y);
            out["point_color_b"]     = std::to_string(pl->color.z);
            out["point_intensity"]   = std::to_string(pl->intensity);
            out["point_range"]       = std::to_string(pl->range);
            out["point_shadow"]      = std::to_string(pl->castShadow ? 1 : 0);
            out["point_shadow_bias"] = std::to_string(pl->shadowBias);
            if (auto* tf = g.world.GetComponent<TransformComponent>(g.pointLightEntity)) {
                out["point_pos_x"] = std::to_string(tf->position.x);
                out["point_pos_y"] = std::to_string(tf->position.y);
                out["point_pos_z"] = std::to_string(tf->position.z);
            }
        }

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
        if (auto* pl = g.world.GetComponent<PointLight>(g.pointLightEntity)) {
            out["point_color_r"]     = std::to_string(pl->color.x);
            out["point_color_g"]     = std::to_string(pl->color.y);
            out["point_color_b"]     = std::to_string(pl->color.z);
            out["point_intensity"]   = std::to_string(pl->intensity);
            out["point_range"]       = std::to_string(pl->range);
            out["point_shadow"]      = std::to_string(pl->castShadow ? 1 : 0);
            out["point_shadow_bias"] = std::to_string(pl->shadowBias);
            if (auto* tf = g.world.GetComponent<TransformComponent>(g.pointLightEntity)) {
                out["point_pos_x"] = std::to_string(tf->position.x);
                out["point_pos_y"] = std::to_string(tf->position.y);
                out["point_pos_z"] = std::to_string(tf->position.z);
            }
        }
        SaveConfigFile(ConfigPath(), out);
        HE_CORE_INFO("03.App: 配置已保存 {}", ConfigPath());
    }

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
