// ============================================================
// 05.Sponza-PathTracing — Sponza 场景 + 全路径追踪管线
//
// 与 03.Sponza-Forward / 04.Sponza-Deferred 相同的 Sponza 场景
// （glTF 加载 + 纹理 + Skybox + 光照），但渲染改用 PathTracingPipeline：
// 不做任何光栅化，RayGen 直接求解渲染方程。
//
// 管线定位：参考渲染器 / Ground Truth（不是第四种实时管线）
//   [AS 构建] → [PT_Render：NEE + MIS + 俄罗斯轮盘赌] → [ReSTIR DI（可选）]
//   → [时域降噪] → [A-Trous 空间滤波] → [ToneMap] → [BackBuffer]
//
// 本示例同时是《全路径追踪管线规划.md》各项 PT 功能的验证载体：
// 每个新功能都在这里冒烟通过后才提交。
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/PathTracingPipeline.h"
#include "Pipeline/PTQualityCVars.h"
#include "Pipeline/DeferredPipeline.h"   // 对照模式：PT 当标准答案 vs GI 层栈（HE_DUMP_MODE=deferred）
#include "Pipeline/CameraController.h"
#include "Pipeline/PhysicalCamera.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/SphereComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/AnimationComponent.h"
#include "Scene/ParticleComponent.h"
#include "Asset/glTFLoader.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include <algorithm>
#include <cstdlib>      // std::getenv / std::atoi（HE_DUMP_PT / HE_CFG）
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// stb_image — 纹理解码
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

using namespace he;

// ============================================================
// 配置读写（简易 key=value 格式）
// ============================================================
// 配置文件路径（示例退出时会回写该文件；对照流程用 HE_CFG 指定私有副本，见 main）
static String g_ConfigPath = String(HUGE_CONTENT_DIR) + "Config/05_Sponza-PathTracing.cfg";

static std::unordered_map<String, String> LoadConfigFile(const String& path) {
    std::unordered_map<String, String> map;
    std::ifstream f(path);
    if (!f.is_open()) return map;
    String line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == String::npos) continue;
        String key = line.substr(0, eq);
        String val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        map[key] = val;
    }
    return map;
}

static void SaveConfigFile(const String& path,
                            const std::unordered_map<String, String>& map) {
    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);

    std::ofstream f(path);
    if (!f.is_open()) return;
    for (auto& [k, v] : map) {
        f << k << "=" << v << "\n";
    }
}

static float GetFloat(const std::unordered_map<String, String>& m,
                      const String& key, float def = 0.0f) {
    auto it = m.find(key);
    return (it != m.end()) ? std::stof(it->second) : def;
}
static int GetInt(const std::unordered_map<String, String>& m,
                  const String& key, int def = 0) {
    auto it = m.find(key);
    return (it != m.end()) ? std::stoi(it->second) : def;
}

int main() {
    // ============================================================
    // 0. 环境变量：cfg 路径覆盖（对照流程的私有副本）
    // ============================================================
    // 本示例退出时会把面板参数回写 g_ConfigPath，多次运行会互相覆盖。
    // 对照 / 复现实验因此支持 HE_CFG=<路径> 指定本次运行的私有 cfg 副本
    // （Tools/pt/dump_pt.ps1 就是这么用的），避免污染基准配置。
    if (const char* cfgEnv = std::getenv("HE_CFG")) {
        if (*cfgEnv) g_ConfigPath = cfgEnv;
    }

    // ============================================================
    // 1. 引擎启动
    // ============================================================
    EngineConfig config;
    config.appName      = "HugEngine — 05.Sponza-PathTracing";
    config.windowWidth  = 960;
    config.windowHeight = 540;
    config.enableVSync  = true;
    config.logLevel     = LogLevel::Info;

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

    String sponzaPath = String(HUGE_CONTENT_DIR) + "gltf/Sponza/glTF/Sponza.gltf";
    HE_CORE_INFO("加载 Sponza 场景: {}", sponzaPath);

    auto result = asset::LoadGLTF(world, sceneGraph, sponzaPath);
    if (!result.success) {
        HE_CORE_ERROR("Sponza 加载失败: {}", result.error);
        return 1;
    }
    HE_CORE_INFO("Sponza 加载完成: {} 实体, {} 网格图元",
                 result.entities.size(), result.meshCount);

    // --- 加载配置文件 ---
    auto cfgData = LoadConfigFile(g_ConfigPath);
    bool hasConfig = !cfgData.empty();
    if (hasConfig) {
        HE_CORE_INFO("加载配置文件: {}", g_ConfigPath);
    }

    // --- 添加方向光 ---
    Entity mainLightEntity;
    DirectionalLight* mainDL = nullptr;
    {
        mainLightEntity = world.CreateEntity("DirectionalLight");
        world.AddComponent<TransformComponent>(mainLightEntity);
        mainDL = world.AddComponent<DirectionalLight>(mainLightEntity);
        mainDL->direction    = float3(
            GetFloat(cfgData, "light_dir_x", 0.4f),
            GetFloat(cfgData, "light_dir_y", -1.0f),
            GetFloat(cfgData, "light_dir_z", 0.6f));
        mainDL->color        = float3(
            GetFloat(cfgData, "light_color_r", 1.0f),
            GetFloat(cfgData, "light_color_g", 0.95f),
            GetFloat(cfgData, "light_color_b", 0.85f));
        mainDL->intensity    = GetFloat(cfgData, "light_intensity", 15.0f);
        mainDL->castShadow       = GetInt(cfgData, "shadow_enabled", 1) != 0;
        mainDL->shadowBias       = GetFloat(cfgData, "shadow_bias", 0.003f);
        mainDL->shadowNormalBias = GetFloat(cfgData, "shadow_normal_bias", 0.02f);
        mainDL->shadowStrength   = GetFloat(cfgData, "shadow_strength", 1.0f);
        mainDL->enabled          = GetInt(cfgData, "light_enabled", 1) != 0;
        sceneGraph.SetParent(mainLightEntity, Entity{kInvalidEntity});
    }

    // --- 添加半球环境光补光 ---
    {
        Entity lightEntity = world.CreateEntity("FillLight");
        world.AddComponent<TransformComponent>(lightEntity);
        auto* dl = world.AddComponent<DirectionalLight>(lightEntity);
        dl->direction = float3(
            GetFloat(cfgData, "fill_dir_x", -0.3f),
            GetFloat(cfgData, "fill_dir_y", -0.4f),
            GetFloat(cfgData, "fill_dir_z", -0.5f));
        dl->color     = float3(
            GetFloat(cfgData, "fill_color_r", 0.6f),
            GetFloat(cfgData, "fill_color_g", 0.7f),
            GetFloat(cfgData, "fill_color_b", 0.9f));
        dl->intensity = GetFloat(cfgData, "fill_intensity", 2.0f);
        dl->enabled   = GetInt(cfgData, "fill_enabled", 1) != 0;
        sceneGraph.SetParent(lightEntity, Entity{kInvalidEntity});
    }

    // --- 添加点光源 + 可视化球体 ---
    Entity pointLightEntity, pointLightSphereEntity;
    {
        pointLightEntity = world.CreateEntity("PointLight");
        world.AddComponent<TransformComponent>(pointLightEntity);
        auto* pl = world.AddComponent<PointLight>(pointLightEntity);
        pl->color      = float3(
            GetFloat(cfgData, "point_color_r", 1.0f),
            GetFloat(cfgData, "point_color_g", 0.85f),
            GetFloat(cfgData, "point_color_b", 0.6f));
        pl->intensity  = GetFloat(cfgData, "point_intensity", 20.0f);
        pl->range      = GetFloat(cfgData, "point_range", 600.0f);
        pl->castShadow = GetInt(cfgData, "point_shadow", 1) != 0;
        pl->shadowBias = GetFloat(cfgData, "point_bias", 0.005f);
        pl->enabled    = GetInt(cfgData, "point_enabled", 1) != 0;

        auto* plTransform = world.GetComponent<TransformComponent>(pointLightEntity);
        if (plTransform) {
            plTransform->position = float3(
                GetFloat(cfgData, "point_pos_x", -300.0f),
                GetFloat(cfgData, "point_pos_y", 100.0f),
                GetFloat(cfgData, "point_pos_z", 0.0f));
        }
        sceneGraph.SetParent(pointLightEntity, Entity{kInvalidEntity});

        // 可视化球体
        pointLightSphereEntity = world.CreateEntity("PointLightSphere");
        world.AddComponent<TransformComponent>(pointLightSphereEntity);
        auto* sphere = world.AddComponent<SphereComponent>(pointLightSphereEntity);
        sphere->radius       = 15.0f;
        sphere->segmentCount = 16;
        sphere->ringCount    = 8;
        sphere->OnCreate();

        auto* sphereTransform = world.GetComponent<TransformComponent>(pointLightSphereEntity);
        if (sphereTransform && plTransform)
            sphereTransform->position = plTransform->position;
        sceneGraph.SetParent(pointLightSphereEntity, Entity{kInvalidEntity});
    }

    // --- 聚光灯 ---
    Entity spotLightEntity, spotLightConeEntity;
    {
        spotLightEntity = world.CreateEntity("SpotLight");
        world.AddComponent<TransformComponent>(spotLightEntity);
        auto* sl = world.AddComponent<SpotLight>(spotLightEntity);
        sl->color           = float3(
            GetFloat(cfgData, "spot_color_r", 1.0f),
            GetFloat(cfgData, "spot_color_g", 0.9f),
            GetFloat(cfgData, "spot_color_b", 0.7f));
        sl->intensity       = GetFloat(cfgData, "spot_intensity", 80.0f);
        sl->range           = GetFloat(cfgData, "spot_range", 1200.0f);
        sl->innerConeAngle  = GetFloat(cfgData, "spot_inner_angle", 0.25f);
        sl->outerConeAngle  = GetFloat(cfgData, "spot_outer_angle", 0.50f);
        sl->direction       = float3(
            GetFloat(cfgData, "spot_dir_x", 0.0f),
            GetFloat(cfgData, "spot_dir_y", -1.0f),
            GetFloat(cfgData, "spot_dir_z", 0.3f));
        sl->castShadow      = GetInt(cfgData, "spot_shadow", 1) != 0;
        sl->shadowBias      = GetFloat(cfgData, "spot_bias", 0.005f);
        sl->enabled         = GetInt(cfgData, "spot_enabled", 1) != 0;

        auto* slTransform = world.GetComponent<TransformComponent>(spotLightEntity);
        if (slTransform) {
            slTransform->position = float3(
                GetFloat(cfgData, "spot_pos_x", 0.0f),
                GetFloat(cfgData, "spot_pos_y", 200.0f),
                GetFloat(cfgData, "spot_pos_z", -200.0f));
        }
        sceneGraph.SetParent(spotLightEntity, Entity{kInvalidEntity});

        // 可视化锥体（小圆锥表示光源位置）
        spotLightConeEntity = world.CreateEntity("SpotLightCone");
        world.AddComponent<TransformComponent>(spotLightConeEntity);
        auto* coneSphere = world.AddComponent<SphereComponent>(spotLightConeEntity);
        coneSphere->radius       = 10.0f;
        coneSphere->segmentCount = 8;
        coneSphere->ringCount    = 4;
        coneSphere->OnCreate();

        auto* coneTransform = world.GetComponent<TransformComponent>(spotLightConeEntity);
        if (coneTransform && slTransform)
            coneTransform->position = slTransform->position;
        sceneGraph.SetParent(spotLightConeEntity, Entity{kInvalidEntity});
    }

    // --- 天空盒 ---
    {
        String hdrPath = String(HUGE_CONTENT_DIR) + "Textures/skybox.hdr";
        int hdrW, hdrH, hdrCh;
        float* hdrData = stbi_loadf(hdrPath.c_str(), &hdrW, &hdrH, &hdrCh, 4);
        if (!hdrData) {
            HE_CORE_WARN("skybox.hdr 加载失败: {}", hdrPath);
        } else {
            HE_CORE_INFO("skybox.hdr: {}×{} ({} channels)", hdrW, hdrH, hdrCh);

            const u32 faceSize = 512;
            const u32 faceBytes = faceSize * faceSize * 4;
            std::vector<u8> allFaces(faceBytes * 6);

            struct { float3 dir; float3 up; float3 right; } faces[6] = {
                {{ 1, 0, 0}, {0,-1, 0}, {0, 0,-1}},
                {{-1, 0, 0}, {0,-1, 0}, {0, 0, 1}},
                {{ 0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
                {{ 0,-1, 0}, {0, 0,-1}, {1, 0, 0}},
                {{ 0, 0, 1}, {0,-1, 0}, {1, 0, 0}},
                {{ 0, 0,-1}, {0,-1, 0}, {-1,0, 0}},
            };

            for (u32 f = 0; f < 6; ++f) {
                u8* faceData = allFaces.data() + f * faceBytes;
                for (u32 y = 0; y < faceSize; ++y) {
                    for (u32 x = 0; x < faceSize; ++x) {
                        float u = (2.0f * x / faceSize) - 1.0f;
                        float v = (2.0f * y / faceSize) - 1.0f;
                        float3 dir = glm::normalize(
                            faces[f].dir + faces[f].right * u + faces[f].up * v);
                        float eqU = (std::atan2(dir.z, dir.x) / (2.0f * 3.14159265f)) + 0.5f;
                        float eqV = (std::asin(glm::clamp(dir.y, -1.0f, 1.0f)) / 3.14159265f) + 0.5f;
                        int px = static_cast<int>(eqU * hdrW) % hdrW;
                        int py = static_cast<int>(eqV * hdrH) % hdrH;
                        if (px < 0) px += hdrW;
                        if (py < 0) py += hdrH;
                        float* src = hdrData + (py * hdrW + px) * 4;
                        auto tonemap = [](float c) {
                            c = c / (1.0f + c);
                            return static_cast<u8>(glm::clamp(c, 0.0f, 1.0f) * 255.0f);
                        };
                        usize idx = (y * faceSize + x) * 4;
                        faceData[idx+0] = tonemap(src[0]);
                        faceData[idx+1] = tonemap(src[1]);
                        faceData[idx+2] = tonemap(src[2]);
                        faceData[idx+3] = 255;
                    }
                }
            }
            stbi_image_free(hdrData);

            rhi::TextureDesc cmDesc;
            cmDesc.format      = rhi::Format::RGBA8_UNORM;
            cmDesc.width       = faceSize;
            cmDesc.height      = faceSize;
            cmDesc.mipLevels   = 1;
            cmDesc.arrayLayers = 6;
            cmDesc.usage       = rhi::TextureUsage::ShaderResource
                               | rhi::TextureUsage::Cubemap
                               | rhi::TextureUsage::TransferDst;
            cmDesc.initialData = allFaces.data();
            auto cubemap = device->CreateTexture(cmDesc);

            rhi::SamplerDesc cmSamp;
            cmSamp.minFilter  = rhi::FilterMode::Linear;
            cmSamp.magFilter  = rhi::FilterMode::Linear;
            cmSamp.addressU   = rhi::AddressMode::ClampToEdge;
            cmSamp.addressV   = rhi::AddressMode::ClampToEdge;
            cmSamp.addressW   = rhi::AddressMode::ClampToEdge;
            auto cmSampler = device->CreateSampler(cmSamp);

            Entity skyEntity = world.CreateEntity("Skybox");
            world.AddComponent<TransformComponent>(skyEntity);
            auto* skyComp = world.AddComponent<SkyboxComponent>(skyEntity);
            skyComp->SetCubemap(std::move(cubemap), std::move(cmSampler));
            sceneGraph.SetParent(skyEntity, Entity{kInvalidEntity});
        }
    }

    // ============================================================
    // 5. 加载 glTF 纹理 → RHI Texture → MeshComponent
    // ============================================================
    std::unordered_map<String,
        std::pair<std::unique_ptr<rhi::IRHITexture>,
                  std::unique_ptr<rhi::IRHISampler>>> g_TexCache;

    auto loadTexture = [&](const String& uri) -> std::pair<rhi::IRHITexture*, rhi::IRHISampler*> {
        if (uri.empty()) return {nullptr, nullptr};
        String texPath = (std::filesystem::path(sponzaPath).parent_path() / uri).string();
        auto it = g_TexCache.find(texPath);
        if (it == g_TexCache.end()) {
            int w, h, ch;
            u8* pixels = stbi_load(texPath.c_str(), &w, &h, &ch, 4);
            if (!pixels) { HE_CORE_WARN("纹理加载失败: {}", texPath); return {nullptr, nullptr}; }
            u32 maxDim = static_cast<u32>(std::max(w, h));
            u32 mipLevels = 1;
            while (maxDim > 1) { maxDim >>= 1; ++mipLevels; }

            rhi::TextureDesc td;
            td.format=rhi::Format::RGBA8_UNORM;
            td.width=static_cast<u32>(w);
            td.height=static_cast<u32>(h);
            td.mipLevels = mipLevels;
            td.usage=rhi::TextureUsage::ShaderResource
                   | rhi::TextureUsage::TransferSrc
                   | rhi::TextureUsage::TransferDst;
            td.initialData=pixels;
            auto t = device->CreateTexture(td);
            rhi::SamplerDesc sd;
            sd.minFilter=rhi::FilterMode::Linear;
            sd.magFilter=rhi::FilterMode::Linear;
            sd.mipFilter=rhi::FilterMode::Linear;
            sd.maxLod = static_cast<float>(mipLevels);
            sd.addressU=sd.addressV=rhi::AddressMode::Repeat;
            auto s = device->CreateSampler(sd);
            stbi_image_free(pixels);
            it = g_TexCache.emplace(texPath, std::make_pair(std::move(t), std::move(s))).first;
            HE_CORE_INFO("GPU 纹理: {} ({}×{})", texPath, w, h);
        }
        return {it->second.first.get(), it->second.second.get()};
    };

    {
        // 创建 bindless 默认占位纹理（必须在 RegisterMaterial 之前设置）
        u8 white[4]={255,255,255,255};
        rhi::TextureDesc td;
        td.format=rhi::Format::RGBA8_UNORM;
        td.width=1;
        td.height=1;
        td.mipLevels=1;
        td.usage=rhi::TextureUsage::ShaderResource;
        td.initialData=white;
        auto defaultTex = device->CreateTexture(td);
        rhi::SamplerDesc sd;
        sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
        sd.addressU=sd.addressV=rhi::AddressMode::Repeat;
        auto defaultSamp = device->CreateSampler(sd);
        device->GetBindlessHeap()->SetDefaultTexture(
            defaultTex.get(), defaultSamp.get());
        g_TexCache["__default__"] = {std::move(defaultTex), std::move(defaultSamp)};
    }

    {
        u32 texCount = 0;
        world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent& mesh) {
            auto [bcTex, bcSamp] = loadTexture(mesh.baseColorTexture);
            auto [nTex, nSamp] = loadTexture(mesh.normalTexture);
            auto [mrTex, mrSamp] = loadTexture(mesh.metallicRoughnessTexture);
            auto [aoTex, aoSamp] = loadTexture(mesh.occlusionTexture);
            // 材质 = 4 个连续 bindless 纹理槽（BaseColor/Normal/MetallicRoughness/Occlusion），
            // 首调用返回值即 materialID（基索引），shader 用 texBase+0/1/2/3 采样
            auto* heap = device->GetBindlessHeap();
            u32 matID = heap->RegisterTexture(bcTex, bcSamp);
            heap->RegisterTexture(nTex, nSamp);
            heap->RegisterTexture(mrTex, mrSamp);
            heap->RegisterTexture(aoTex, aoSamp);
            mesh.materialID = matID;
            texCount++;
        });
        HE_CORE_INFO("纹理加载 + bindless 注册完成: {} primitive, {} 张独立纹理", texCount, g_TexCache.size());
    }

    // ============================================================
    // 6. 初始化全路径追踪管线
    // ============================================================
    // PT 自带 RTPass（BLAS/TLAS 构建），无需其它管线提供加速结构。
    // 注意：PT 对比测试（白炉 / 参考图）要求场景材质纹理与光源先于首帧就绪，
    //       场景纹理已在上一节注册进 bindless 堆，光照由管线的 CollectLights 收集。
    render::PathTracingPipeline pathTracingPipeline;
    pathTracingPipeline.Initialize(device.get());
    pathTracingPipeline.SetSwapChain(swapchain.get());
    pathTracingPipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());

    if (!pathTracingPipeline.IsRTEnabled()) {
        HE_CORE_ERROR("设备不支持硬件光追（RT），05.Sponza-PathTracing 无法运行");
        pathTracingPipeline.Shutdown();
        return 1;
    }

    // ── 从配置文件恢复 PT 质量参数（默认值即管线内 CVar 默认值）──
    if (hasConfig) {
        pathTracingPipeline.SetPTSampleCount(GetInt(cfgData, "pt_spp", pathTracingPipeline.GetPTSampleCount()));
        pathTracingPipeline.SetPTMaxBounces(GetInt(cfgData, "pt_bounces", pathTracingPipeline.GetPTMaxBounces()));
        pathTracingPipeline.SetPTDenoise(GetInt(cfgData, "pt_denoise", 1) != 0);
        pathTracingPipeline.SetPTAtrous(GetInt(cfgData, "pt_atrous", 1) != 0);
        pathTracingPipeline.SetPTReSTIR(GetInt(cfgData, "pt_restir", 1) != 0);
        pathTracingPipeline.SetPTMIS(GetInt(cfgData, "pt_mis", 1) != 0);
        pathTracingPipeline.SetPTRoulette(GetInt(cfgData, "pt_roulette", 1) != 0);

        render::cvPTSkyIntensity.Set(
            GetFloat(cfgData, "pt_sky_intensity", render::cvPTSkyIntensity.Get()));
        render::cvPTDenoiseBlend.Set(
            GetFloat(cfgData, "pt_denoise_blend", render::cvPTDenoiseBlend.Get()));

        render::cvPTAtrousIterations.Set(
            GetInt(cfgData, "pt_atrous_iterations", render::cvPTAtrousIterations.Get()));
        render::cvPTAtrousSigmaDepth.Set(
            GetFloat(cfgData, "pt_atrous_sigma_depth", render::cvPTAtrousSigmaDepth.Get()));
        render::cvPTAtrousSigmaNormal.Set(
            GetFloat(cfgData, "pt_atrous_sigma_normal", render::cvPTAtrousSigmaNormal.Get()));
        render::cvPTAtrousSigmaColor.Set(
            GetFloat(cfgData, "pt_atrous_sigma_color", render::cvPTAtrousSigmaColor.Get()));
        render::cvPTAtrousClamp.Set(
            GetFloat(cfgData, "pt_atrous_clamp", render::cvPTAtrousClamp.Get()));

        render::cvPTRestirCandidates.Set(
            GetInt(cfgData, "pt_restir_candidates", render::cvPTRestirCandidates.Get()));
        render::cvPTRestirRadius.Set(
            GetInt(cfgData, "pt_restir_radius", render::cvPTRestirRadius.Get()));
        render::cvPTRestirSamples.Set(
            GetInt(cfgData, "pt_restir_samples", render::cvPTRestirSamples.Get()));

        HE_CORE_INFO("PT 质量参数已从配置文件恢复: {}", g_ConfigPath);
    }

    HE_CORE_INFO("PathTracingPipeline 初始化完成 (RT={})", pathTracingPipeline.IsRTEnabled());

    // ============================================================
    // 6.5 对照模式：延迟管线（PT = 标准答案 vs GI 层栈）
    // ============================================================
    // 常态下只跑 PT（本示例定位就是参考渲染器）。做「PT vs 近似 GI」的对照实验时，
    // 用 HE_DUMP_MODE=deferred 让**同一场景、同一相机、同一帧号**改走 DeferredPipeline
    // （IBL / RSM / SSGI / DDGI / SSR 的 GI 层栈齐全），两次运行各自落盘，
    // 再用 Tools/pt/analyze_pt.py --compare 出偏差读数。
    // 两条管线各自持有加速结构：只有对照模式才创建 Deferred，避免常态下重复占内存。
    const String g_DumpMode    = std::getenv("HE_DUMP_MODE") ? String(std::getenv("HE_DUMP_MODE")) : String("pt");
    const bool   g_UseDeferred = (g_DumpMode == "deferred");

    render::DeferredPipeline deferredPipeline;
    if (g_UseDeferred) {
        deferredPipeline.Initialize(device.get());
        deferredPipeline.SetSwapChain(swapchain.get());
        deferredPipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());
        HE_CORE_INFO("对照模式：本次运行改走 DeferredPipeline（GI 层栈），其余参数与 PT 模式一致");
    }

    // ============================================================
    // 7. 创建命令列表
    // ============================================================
    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());
    // 预设 ToneMap PSO → 匹配 BGRA8_UNORM RP（ImGui LoadOp 兼容）
    cmdList->SetPipeline(g_UseDeferred
        ? deferredPipeline.GetToneMap()->GetPSO()
        : pathTracingPipeline.GetPostProcess()->GetToneMap()->GetPSO());

    // ============================================================
    // 8. ImGui 初始化
    // ============================================================
    GLFWwindow* glfwWin = engine.GetWindow()->GetNativeHandle();
    editor::ImGuiIntegration imgui;
    imgui.Initialize(glfwWin, device.get(), swapchain.get());

    // ============================================================
    // 9. 相机 — 从配置文件加载，否则使用默认位置
    // ============================================================
    render::CameraController camCtrl;
    camCtrl.SetAspectRatio(
        static_cast<float>(swapchain->GetWidth()),
        static_cast<float>(swapchain->GetHeight()));
    camCtrl.SetMoveSpeed(72.0f);

    if (hasConfig) {
        camCtrl.SetPosition(float3(
            GetFloat(cfgData, "cam_pos_x", 0.0f),
            GetFloat(cfgData, "cam_pos_y", 3.0f),
            GetFloat(cfgData, "cam_pos_z", 0.0f)));
        camCtrl.SetOrientation(
            GetFloat(cfgData, "cam_yaw", -1.57f),
            GetFloat(cfgData, "cam_pitch", -0.1f));
        camCtrl.GetCamera().nearPlane = GetFloat(cfgData, "cam_near", 0.1f);
        camCtrl.GetCamera().farPlane  = GetFloat(cfgData, "cam_far", 2000.0f);
    } else {
        camCtrl.SetPosition(float3(0.0f, 3.0f, 0.0f));
        camCtrl.SetOrientation(-1.57f, -0.1f);
    }
    // ============================================================
    // 8.5 相机平移动画（AnimationComponent 演示）
    // ============================================================
    Entity camAnimEntity = world.CreateEntity("CameraAnimation");
    world.AddComponent<TransformComponent>(camAnimEntity);
    auto* camAnim = world.AddComponent<AnimationComponent>(camAnimEntity);
    {
        auto& clips = camAnim->clips;
        clips.push_back({});
        camAnim->currentClip = 0;
        auto& clip = clips.back();
        clip.name    = "CameraOrbit";
        clip.looping = true;
        const float radius   = 600.0f;
        const float height   = 200.0f;
        const int   steps    = 60;
        const float duration = 6.0f;
        for (int i = 0; i <= steps; ++i) {
            float t = static_cast<float>(i) / steps * duration;
            float angle = (static_cast<float>(i) / steps) * glm::radians(360.0f);
            camAnim->AddTranslationKey(t, float3(
                std::cos(angle) * radius, height, std::sin(angle) * radius));
        }
        camAnim->FinalizeClip();
        camAnim->playing = false;  // 默认关闭启动动画
    }
    sceneGraph.SetParent(camAnimEntity, Entity{kInvalidEntity});
    HE_CORE_INFO("相机动画已创建: {} 秒圆形路径", 6.0f);

    bool   rightMouseDown = false;
    bool   animCameraMode = true;
    double lastMouseX = 0.0, lastMouseY = 0.0;

    // ============================================================
    // 10. 窗口调整回调
    // ============================================================
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (w == 0 || h == 0) return;
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        pathTracingPipeline.OnResize(w, h);
        if (g_UseDeferred) deferredPipeline.OnResize(w, h);
        camCtrl.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });
    // ============================================================
    // 10.5 PT 参考图落盘（对照流程，PT 任务 5）
    // ============================================================
    // 用法：HE_DUMP_PT=<标签>  [HE_DUMP_PT_FRAME=<帧号，默认 60>]  [HE_CFG=<私有 cfg>]
    //   输出 build/verify/pt_<标签>_<目标>.f16（原始像素、无文件头、行紧密排布）
    //       + _meta.txt（逐目标一行：名称 宽 高 格式）+ _camera.txt（渲染该帧时的相机参数）
    //   目标：hdr / depth / normal / albedo（PT 的四张输出；velocity 未落盘）
    //   落盘后自动请求退出窗口，便于脚本化（脚本见 Tools/pt/dump_pt.ps1）。
    //
    // 为什么需要它：PT 是"标准答案"，对照结论必须建立在**可复现的读数**上。
    // 落盘的是未 ToneMap 的线性 HDR（降噪 + A-Trous 之后的最终辐射度），
    // 离线脚本据此算均值/分位数/两版差值，避免用截图或目测下结论。
    const char* dumpTagEnv   = std::getenv("HE_DUMP_PT");
    const char* dumpFrameEnv = std::getenv("HE_DUMP_PT_FRAME");
    const String g_DumpTag   = dumpTagEnv ? dumpTagEnv : "";
    bool         g_DumpPT    = !g_DumpTag.empty();   // 非 const：失败时关闭以免每帧重试
    const u64    g_DumpFrame = dumpFrameEnv ? (u64)std::max(1, std::atoi(dumpFrameEnv)) : 60ull;
    bool g_DumpDone    = false;   // 已录制拷贝（防止重复录制）
    bool g_DumpWritten = false;   // 已落盘（防止每帧重复写文件）
    struct DumpTarget {
        String                           name;
        rhi::IRHITexture*                tex = nullptr;
        std::unique_ptr<rhi::IRHIBuffer> buf;
        u32                              w = 0, h = 0;
        u32                              bytesPerPixel = 8;   // RGBA16F=8，R32F=4
    };
    std::vector<DumpTarget> g_DumpTargets;
    if (g_DumpPT)
        HE_CORE_INFO("[PT采样] 已启用：标签={} 目标帧={} 输出 build/verify/pt_{}_*.f16",
                     g_DumpTag, g_DumpFrame, g_DumpTag);

    // ============================================================
    // 11. 主渲染循环
    // ============================================================
    HE_CORE_INFO("05.Sponza-PathTracing 启动 — WASD=移动, 右键拖拽=旋转, Shift=加速, E/Q=升降");
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
                camCtrl.Rotate(dx * 0.003f, -dy * 0.003f);
            }

            // T 键切换动画/手动相机模式
            static bool tWasDown = false;
            bool tDown = glfwGetKey(glfwWin, GLFW_KEY_T) == GLFW_PRESS;
            if (tDown && !tWasDown) animCameraMode = !animCameraMode;
            tWasDown = tDown;

            // 动画相机模式：动画播放时同步 AnimationComponent 的位置
            if (animCameraMode && camAnim->playing) {
                auto* camTf = world.GetComponent<TransformComponent>(camAnimEntity);
                if (camTf) {
                    camCtrl.SetPosition(camTf->position);
                    float3 toOrigin = glm::normalize(float3(0, 200, 0) - camTf->position);
                    camCtrl.SetOrientationFromForward(toOrigin);
                }
            }

            render::CameraController::MoveInput moveIn;
            moveIn.forward  = glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS;
            moveIn.backward = glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS;
            moveIn.left     = glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS;
            moveIn.right    = glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS;
            moveIn.up       = glfwGetKey(glfwWin, GLFW_KEY_E) == GLFW_PRESS;
            moveIn.down     = glfwGetKey(glfwWin, GLFW_KEY_Q) == GLFW_PRESS;
            moveIn.sprint   = glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
            camCtrl.Update(deltaTime, moveIn);
        }

        // Transform 动画更新（驱动 AnimationComponent → TransformComponent）
        world.ForEach<he::AnimationComponent>([&](he::Entity e, he::AnimationComponent& anim) {
            auto* tf = world.GetComponent<TransformComponent>(e);
            if (tf) anim.Update(deltaTime, tf);
        });

        // --- 渲染（PathTracingPipeline / 对照模式下的 DeferredPipeline，均通过 RenderGraph 编排）---
        cmdList->Begin();
        if (g_UseDeferred) {
            deferredPipeline.NextFrame();
            deferredPipeline.Render(cmdList.get(), world, sceneGraph, camCtrl.GetCamera(), deltaTime);
        } else {
            pathTracingPipeline.NextFrame();
            pathTracingPipeline.Render(cmdList.get(), world, sceneGraph, camCtrl.GetCamera(), deltaTime);
        }

        // ── PT 参考图落盘：整幅 CopyTextureToBuffer 到 host 可见缓冲（仅对照路径）──
        // 必须在 render pass 之外录制；缓冲在采样帧才创建，尺寸取自纹理本身。
        if (g_DumpPT && !g_DumpDone && frameIndex >= g_DumpFrame) {
            auto addTarget = [&](const String& name, rhi::IRHITexture* tex, u32 bytesPerPixel) {
                if (!tex) return;
                DumpTarget t;
                t.name = name;
                t.tex  = tex;
                t.w    = tex->GetWidth();
                t.h    = tex->GetHeight();
                t.bytesPerPixel = bytesPerPixel;
                rhi::BufferDesc dd;
                dd.size      = (usize)t.w * t.h * bytesPerPixel;
                dd.usage     = rhi::BufferUsage::Storage;   // 该路径恒定带 TRANSFER_DST，可作拷贝目标
                dd.cpuAccess = true;                        // 需要 Map 读回
                t.buf = device->CreateBuffer(dd);
                if (!t.buf) {
                    HE_CORE_ERROR("[PT采样] 读回缓冲创建失败: {}（{}x{}）", name, t.w, t.h);
                    return;
                }
                // x=y=0 且取满宽高 ⇒ bufferRowLength=0 的紧密排布正好等于线性落盘布局
                cmdList->CopyTextureToBuffer(tex, t.buf.get(), 0, 0, t.w, t.h, 0);
                g_DumpTargets.push_back(std::move(t));
            };
            if (g_UseDeferred) {
                // 对照模式：落盘延迟管线的 HDR（ToneMap 前的线性光照结果）+ GBuffer 的关键输入
                addTarget("hdr", deferredPipeline.GetLighting().GetHDRTarget(), 8);
                if (auto* gb = deferredPipeline.GetGBuffer()) {
                    addTarget("albedo", gb->GetAlbedo(), 8);
                    addTarget("normal", gb->GetNormal(), 8);
                }
            } else if (auto* pt = pathTracingPipeline.GetPT()) {
                addTarget("hdr",    pt->GetHDR(),            8);   // RGBA16F 最终辐射度
                addTarget("depth",  pt->GetDepth(),          4);   // R32F 线性视图深度
                addTarget("normal", pt->GetNormal(),         8);   // RGBA16F 世界法线 + roughness
                addTarget("albedo", pt->GetAlbedoMetallic(), 8);   // RGBA16F albedo + metallic
            }
            if (!g_DumpTargets.empty()) {
                g_DumpDone = true;   // 已录制；实际读取放在 Submit 之后
            } else {
                HE_CORE_WARN("[PT采样] 无可用目标，关闭落盘");
                g_DumpPT = false;
            }
        }

        // --- ImGui（LOAD 保留 ToneMap 输出）---
        cmdList->BeginRenderPass(1, rhi::Format::BGRA8_UNORM,
            rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);

        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("05.Sponza-PathTracing");
        {
            float fps = 1.0f / (deltaTime > 0.001f ? deltaTime : 0.016f);
            ImGui::TextColored({0.3f, 1.0f, 0.3f, 1.0f}, "FPS: %.0f", fps);
            ImGui::SameLine(120);
            ImGui::TextColored({0.6f, 0.6f, 0.6f, 1.0f}, "(%.2f ms)", deltaTime * 1000.0f);
            // 当前渲染模式（HE_DUMP_MODE=deferred 时为对照模式，走 GI 层栈）
            ImGui::SameLine(300);
            if (g_UseDeferred)
                ImGui::TextColored({1.0f, 0.7f, 0.3f, 1.0f}, "对照模式：Deferred（GI 层栈）");
            else
                ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "PathTracing（参考渲染器）");


                    // 相机
            ImGui::SeparatorText("相机");
            if (animCameraMode) {
                ImGui::TextColored({0.3f, 1.0f, 0.5f, 1.0f}, "动画相机 (按 T 切换手动)");
                bool playing = camAnim->playing;
                if (ImGui::Checkbox("播放动画", &playing))
                    camAnim->playing = playing;
                ImGui::SameLine();
                float s = camAnim->speed;
                if (ImGui::SliderFloat("动画速度", &s, 0.1f, 3.0f, "%.1f"))
                    camAnim->speed = s;
            } else {
                ImGui::TextColored({1.0f, 1.0f, 0.3f, 1.0f}, "手动相机 (按 T 切换动画)");
            }
            ImGui::DragFloat3("位置##Camera", &camCtrl.GetCamera().position[0], 5.0f);
            float yawDeg   = glm::degrees(camCtrl.GetYaw());
            float pitchDeg = glm::degrees(camCtrl.GetPitch());
            if (ImGui::SliderFloat("Yaw", &yawDeg, -180.0f, 180.0f, "%.1f°"))
                camCtrl.SetOrientation(glm::radians(yawDeg), camCtrl.GetPitch());
            if (ImGui::SliderFloat("Pitch", &pitchDeg, -85.0f, 85.0f, "%.1f°"))
                camCtrl.SetOrientation(camCtrl.GetYaw(), glm::radians(pitchDeg));
            float moveSpeed = camCtrl.GetMoveSpeed();
            if (ImGui::DragFloat("移动速度", &moveSpeed, 1.0f, 1.0f, 500.0f, "%.0f"))
                camCtrl.SetMoveSpeed(moveSpeed);
            float nearP = camCtrl.GetCamera().nearPlane;
            float farP  = camCtrl.GetCamera().farPlane;
            if (ImGui::DragFloat("近裁剪面", &nearP, 0.01f, 0.001f, 10.0f, "%.3f"))
                camCtrl.GetCamera().nearPlane = nearP;
            if (ImGui::DragFloat("远裁剪面", &farP, 10.0f, 10.0f, 50000.0f, "%.0f"))
                camCtrl.GetCamera().farPlane = farP;

            // ── 路径追踪质量（全部为 r.PT.* CVar 的薄封装）──
            ImGui::SeparatorText("路径追踪（参考渲染器 / Ground Truth）");
            ImGui::Text("NEE + MIS + 俄罗斯轮盘赌 → ReSTIR DI → 时域降噪 → A-Trous");
            ImGui::Text("5×UAV: HDR + 深度 + 法线 + 速度 + albedoMetallic");

            int spp = pathTracingPipeline.GetPTSampleCount();
            if (ImGui::SliderInt("SPP", &spp, 1, 8))
                pathTracingPipeline.SetPTSampleCount(spp);
            int bounces = pathTracingPipeline.GetPTMaxBounces();
            if (ImGui::SliderInt("弹射次数", &bounces, 1, 8))
                pathTracingPipeline.SetPTMaxBounces(bounces);
            float skyIntensity = render::cvPTSkyIntensity.Get();
            if (ImGui::DragFloat("天空强度", &skyIntensity, 0.05f, 0.0f, 10.0f, "%.2f"))
                render::cvPTSkyIntensity.Set(skyIntensity);

            bool denoiseOn = pathTracingPipeline.IsPTDenoise();
            if (ImGui::Checkbox("时域降噪", &denoiseOn))
                pathTracingPipeline.SetPTDenoise(denoiseOn);
            ImGui::SameLine();
            bool atrousOn = pathTracingPipeline.IsPTAtrous();
            if (ImGui::Checkbox("A-Trous 空间滤波", &atrousOn))
                pathTracingPipeline.SetPTAtrous(atrousOn);
            ImGui::SameLine();
            bool restirOn = pathTracingPipeline.IsPTReSTIR();
            if (ImGui::Checkbox("ReSTIR DI", &restirOn))
                pathTracingPipeline.SetPTReSTIR(restirOn);
            ImGui::SameLine();
            bool misOn = pathTracingPipeline.IsPTMIS();
            if (ImGui::Checkbox("NEE MIS", &misOn))
                pathTracingPipeline.SetPTMIS(misOn);
            ImGui::SameLine();
            bool rouletteOn = pathTracingPipeline.IsPTRoulette();
            if (ImGui::Checkbox("俄罗斯轮盘赌", &rouletteOn))
                pathTracingPipeline.SetPTRoulette(rouletteOn);

            ImGui::Text("蓄水池: %s", pathTracingPipeline.IsReservoirReady() ? "有效" : "预热中/无效");

            if (ImGui::TreeNode("降噪参数")) {
                float blend = render::cvPTDenoiseBlend.Get();
                if (ImGui::SliderFloat("时域混合", &blend, 0.0f, 1.0f, "%.2f"))
                    render::cvPTDenoiseBlend.Set(blend);

                int atrousIter = render::cvPTAtrousIterations.Get();
                if (ImGui::SliderInt("A-Trous 迭代", &atrousIter, 1, 5))
                    render::cvPTAtrousIterations.Set(atrousIter);
                float sigmaDepth = render::cvPTAtrousSigmaDepth.Get();
                if (ImGui::DragFloat("深度 sigma", &sigmaDepth, 0.01f, 0.001f, 5.0f, "%.3f"))
                    render::cvPTAtrousSigmaDepth.Set(sigmaDepth);
                float sigmaNormal = render::cvPTAtrousSigmaNormal.Get();
                if (ImGui::DragFloat("法线 sigma", &sigmaNormal, 1.0f, 1.0f, 256.0f, "%.0f"))
                    render::cvPTAtrousSigmaNormal.Set(sigmaNormal);
                float sigmaColor = render::cvPTAtrousSigmaColor.Get();
                if (ImGui::DragFloat("颜色 sigma", &sigmaColor, 0.01f, 0.01f, 8.0f, "%.2f"))
                    render::cvPTAtrousSigmaColor.Set(sigmaColor);
                float atrousClamp = render::cvPTAtrousClamp.Get();
                if (ImGui::DragFloat("方差钳制", &atrousClamp, 0.1f, 0.1f, 64.0f, "%.1f"))
                    render::cvPTAtrousClamp.Set(atrousClamp);

                int candidates = render::cvPTRestirCandidates.Get();
                if (ImGui::SliderInt("ReSTIR 候选 M", &candidates, 1, 64))
                    render::cvPTRestirCandidates.Set(candidates);
                int radius = render::cvPTRestirRadius.Get();
                if (ImGui::SliderInt("ReSTIR 复用半径", &radius, 1, 8))
                    render::cvPTRestirRadius.Set(radius);
                int restirSamples = render::cvPTRestirSamples.Get();
                if (ImGui::SliderInt("ReSTIR 复用采样", &restirSamples, 1, 16))
                    render::cvPTRestirSamples.Set(restirSamples);
                ImGui::TreePop();
            }

            // ── 主方向光 ──
            if (mainDL) {
                ImGui::SeparatorText("主方向光");
                bool enabled = mainDL->enabled;
                if (ImGui::Checkbox("启用##DirLight", &enabled)) mainDL->enabled = enabled;
                ImGui::DragFloat3("方向##DirLight", &mainDL->direction[0], 0.01f, -1.0f, 1.0f);
                ImGui::ColorEdit3("颜色##DirLight", &mainDL->color[0]);
                ImGui::DragFloat("强度##DirLight", &mainDL->intensity, 0.1f, 0.0f, 200.0f, "%.2f");
            }

            // ── 点光源 ──
            ImGui::SeparatorText("点光源");
            world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
                bool enabled = pl.enabled;
                if (ImGui::Checkbox("启用##PointLight", &enabled)) pl.enabled = enabled;
                ImGui::ColorEdit3("颜色##PointLight", &pl.color[0]);
                ImGui::DragFloat("强度##PointLight", &pl.intensity, 1.0f, 0.0f, 10000.0f, "%.1f");
                ImGui::DragFloat("半径##PointLight", &pl.range, 1.0f, 1.0f, 5000.0f, "%.0f");
                if (auto* t = world.GetComponent<TransformComponent>(e)) {
                    if (ImGui::DragFloat3("位置##PointLight", &t->position[0], 5.0f)) {
                        // 同步可视化球体位置
                        if (auto* sphereTf =
                                world.GetComponent<TransformComponent>(pointLightSphereEntity))
                            sphereTf->position = t->position;
                    }
                }
            });

            // ── 场景统计 ──
            ImGui::SeparatorText("场景");
            u32 meshCount = 0, dirLightCount = 0, spotCount = 0;
            world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent&) { meshCount++; });
            world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight&) { dirLightCount++; });
            world.ForEach<he::SpotLight>([&](he::Entity, he::SpotLight&) { spotCount++; });
            ImGui::Text("%u 网格  |  %u 方向光  |  1 点光  |  %u 聚光",
                        meshCount, dirLightCount, spotCount);
            ImGui::Text("分辨率: %ux%u", swapchain->GetWidth(), swapchain->GetHeight());
        }
        ImGui::End();

        imgui.EndFrame(cmdList.get());
        cmdList->EndRenderPass();
        cmdList->End();

        device->Submit(cmdList.get());

        // ── PT 参考图落盘：等 GPU 完成后原样写文件（原始像素、无文件头）──
        if (g_DumpDone && !g_DumpWritten) {
            device->WaitIdle();   // 对照用途，允许停顿
            const String dir  = "build/verify/";
            const String base = dir + "pt_" + g_DumpTag;
            std::filesystem::create_directories(dir);
            std::ofstream meta(base + "_meta.txt");
            for (auto& t : g_DumpTargets) {
                const usize bytes = (usize)t.w * t.h * t.bytesPerPixel;
                const void* p = t.buf ? t.buf->Map() : nullptr;
                if (p) {
                    std::ofstream f(base + "_" + t.name + ".f16", std::ios::binary);
                    f.write(static_cast<const char*>(p), (std::streamsize)bytes);
                    t.buf->Unmap();
                    meta << t.name << " " << t.w << " " << t.h << " "
                         << (t.bytesPerPixel == 4 ? "R32F" : "RGBA16F") << "\n";
                    HE_CORE_INFO("[PT采样] {}_{}.f16  {}x{}  {} B", base, t.name, t.w, t.h, bytes);
                } else {
                    HE_CORE_ERROR("[PT采样] 映射失败: {}_{}", base, t.name);
                }
            }
            // 相机参数一并落盘：离线对照需要**渲染这一帧时**的相机参数，
            // 否则判据只能靠硬编码，而硬编码一旦被 cfg 改动就失效
            {
                const render::CameraData& cam = camCtrl.GetCamera();
                std::ofstream cm(base + "_camera.txt");
                cm << "pos "     << cam.position.x << " " << cam.position.y << " " << cam.position.z << "\n";
                cm << "forward " << cam.forward.x  << " " << cam.forward.y  << " " << cam.forward.z  << "\n";
                cm << "up "      << cam.up.x       << " " << cam.up.y       << " " << cam.up.z       << "\n";
                cm << "fov "     << cam.fov        << "\n";
                cm << "near "    << cam.nearPlane  << "\n";
                cm << "far "     << cam.farPlane   << "\n";
                cm << "aspect "  << cam.aspectRatio << "\n";
                cm << "frame "   << frameIndex << "\n";
            }
            HE_CORE_INFO("[PT采样] 共落盘 {} 个目标，请求退出", g_DumpTargets.size());
            g_DumpWritten = true;
            // 采样完成即请求关窗：脚本无需超时等待，也保证退出前正常走完清理与保存流程
            glfwSetWindowShouldClose(glfwWin, GLFW_TRUE);
        }

        swapchain->Present(true);
        frameIndex++;
    }

    // 清理
    imgui.Shutdown();
    device->WaitIdle();
    if (g_UseDeferred) deferredPipeline.Shutdown();
    pathTracingPipeline.Shutdown();

    // ============================================================
    // 保存配置（所有 ImGui 可控参数）
    // ============================================================
    {
        std::unordered_map<String, String> out;

        // ── 相机 ──
        out["cam_pos_x"] = std::to_string(camCtrl.GetCamera().position.x);
        out["cam_pos_y"] = std::to_string(camCtrl.GetCamera().position.y);
        out["cam_pos_z"] = std::to_string(camCtrl.GetCamera().position.z);
        out["cam_yaw"]   = std::to_string(camCtrl.GetYaw());
        out["cam_pitch"] = std::to_string(camCtrl.GetPitch());
        out["cam_near"]  = std::to_string(camCtrl.GetCamera().nearPlane);
        out["cam_far"]   = std::to_string(camCtrl.GetCamera().farPlane);

        // ── 主方向光 ──
        if (mainDL) {
            out["light_enabled"]  = std::to_string(mainDL->enabled ? 1 : 0);
            out["light_dir_x"]    = std::to_string(mainDL->direction.x);
            out["light_dir_y"]    = std::to_string(mainDL->direction.y);
            out["light_dir_z"]    = std::to_string(mainDL->direction.z);
            out["light_color_r"]  = std::to_string(mainDL->color.x);
            out["light_color_g"]  = std::to_string(mainDL->color.y);
            out["light_color_b"]  = std::to_string(mainDL->color.z);
            out["light_intensity"] = std::to_string(mainDL->intensity);
        }

        // ── 补光 ──
        world.ForEach<he::DirectionalLight>([&](he::Entity e, he::DirectionalLight& l) {
            if (e == mainLightEntity) return;
            out["fill_enabled"]   = std::to_string(l.enabled ? 1 : 0);
            out["fill_dir_x"]     = std::to_string(l.direction.x);
            out["fill_dir_y"]     = std::to_string(l.direction.y);
            out["fill_dir_z"]     = std::to_string(l.direction.z);
            out["fill_color_r"]   = std::to_string(l.color.x);
            out["fill_color_g"]   = std::to_string(l.color.y);
            out["fill_color_b"]   = std::to_string(l.color.z);
            out["fill_intensity"] = std::to_string(l.intensity);
        });

        // ── 点光源 ──
        world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
            out["point_enabled"]   = std::to_string(pl.enabled ? 1 : 0);
            out["point_color_r"]   = std::to_string(pl.color.x);
            out["point_color_g"]   = std::to_string(pl.color.y);
            out["point_color_b"]   = std::to_string(pl.color.z);
            out["point_intensity"] = std::to_string(pl.intensity);
            out["point_range"]     = std::to_string(pl.range);
            auto* t = world.GetComponent<TransformComponent>(e);
            if (t) {
                out["point_pos_x"] = std::to_string(t->position.x);
                out["point_pos_y"] = std::to_string(t->position.y);
                out["point_pos_z"] = std::to_string(t->position.z);
            }
        });

        // ── 聚光灯 ──
        world.ForEach<he::SpotLight>([&](he::Entity e, he::SpotLight& sl) {
            out["spot_enabled"]     = std::to_string(sl.enabled ? 1 : 0);
            out["spot_dir_x"]       = std::to_string(sl.direction.x);
            out["spot_dir_y"]       = std::to_string(sl.direction.y);
            out["spot_dir_z"]       = std::to_string(sl.direction.z);
            out["spot_color_r"]     = std::to_string(sl.color.x);
            out["spot_color_g"]     = std::to_string(sl.color.y);
            out["spot_color_b"]     = std::to_string(sl.color.z);
            out["spot_intensity"]   = std::to_string(sl.intensity);
            out["spot_range"]       = std::to_string(sl.range);
            out["spot_inner_angle"] = std::to_string(sl.innerConeAngle);
            out["spot_outer_angle"] = std::to_string(sl.outerConeAngle);
            auto* t = world.GetComponent<TransformComponent>(e);
            if (t) {
                out["spot_pos_x"] = std::to_string(t->position.x);
                out["spot_pos_y"] = std::to_string(t->position.y);
                out["spot_pos_z"] = std::to_string(t->position.z);
            }
        });

        // ── PT 质量参数 ──
        out["pt_spp"]        = std::to_string(pathTracingPipeline.GetPTSampleCount());
        out["pt_bounces"]    = std::to_string(pathTracingPipeline.GetPTMaxBounces());
        out["pt_denoise"]    = std::to_string(pathTracingPipeline.IsPTDenoise() ? 1 : 0);
        out["pt_atrous"]     = std::to_string(pathTracingPipeline.IsPTAtrous() ? 1 : 0);
        out["pt_restir"]     = std::to_string(pathTracingPipeline.IsPTReSTIR() ? 1 : 0);
        out["pt_mis"]        = std::to_string(pathTracingPipeline.IsPTMIS() ? 1 : 0);
        out["pt_roulette"]   = std::to_string(pathTracingPipeline.IsPTRoulette() ? 1 : 0);
        out["pt_sky_intensity"] = std::to_string(render::cvPTSkyIntensity.Get());
        out["pt_denoise_blend"] = std::to_string(render::cvPTDenoiseBlend.Get());

        out["pt_atrous_iterations"]  = std::to_string(render::cvPTAtrousIterations.Get());
        out["pt_atrous_sigma_depth"] = std::to_string(render::cvPTAtrousSigmaDepth.Get());
        out["pt_atrous_sigma_normal"] = std::to_string(render::cvPTAtrousSigmaNormal.Get());
        out["pt_atrous_sigma_color"] = std::to_string(render::cvPTAtrousSigmaColor.Get());
        out["pt_atrous_clamp"]       = std::to_string(render::cvPTAtrousClamp.Get());

        out["pt_restir_candidates"] = std::to_string(render::cvPTRestirCandidates.Get());
        out["pt_restir_radius"]     = std::to_string(render::cvPTRestirRadius.Get());
        out["pt_restir_samples"]    = std::to_string(render::cvPTRestirSamples.Get());

        SaveConfigFile(g_ConfigPath, out);
        HE_CORE_INFO("配置已保存: {}", g_ConfigPath);
    }

    HE_CORE_INFO("05.Sponza-PathTracing 退出 ({} 帧)", frameIndex);
    return 0;
}
