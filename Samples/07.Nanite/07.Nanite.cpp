// ============================================================
// 07.Nanite — Cornell Box 场景 + 延迟渲染管线；Nanite 虚拟几何（cluster 光栅化）实验台
//
// 基线自 06.GILab 整体拷贝：先保证与既有验收口径（白炉 / 逐帧 dump / 开关不变性）完全一致，
//      再在此基础上按 docs/计划实现功能/Nanite设计与实现.md §14 逐项接入 Nanite 模块。
// 用途：检验与对比各种 GI（IBL/RSM/SSGI/DDGI/SSR）在同一 Cornell Box 场景下的效果，
//      并作为 Nanite 虚拟几何与后续新 GI 的接入试验场。
// 使用 DeferredPipeline（GBuffer + 全屏 Lighting Pass）。
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include <chrono>   // 步骤 37：CPU 侧帧时（判定 CPU 受限还是 GPU 受限）
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/DeferredPipeline.h"
#include "Pipeline/ForwardPipeline.h"
#include "Pipeline/IRenderPipeline.h"
#include "GI/GITypes.h"   // GI 数据模型 + GIRegistry（RHI-free）
#include "Nanite/NaniteSettings.h"   // Nanite 开关/档位（§14.4 真值；RHI-free）
#include "GI/LumenProvider.h"   // 步骤 12：取 SDF 追踪可视化纹理做转储
#include "Pipeline/CameraController.h"
#include "Pipeline/PhysicalCamera.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/PhysicalSkyComponent.h"   // SyncPhysicalSkyToSun（Forward 的阴影/光照同向，任务 34）
#include "Scene/AnimationComponent.h"
#include "Asset/glTFLoader.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"
#include "Core/CrashHandler.h"   // 崩溃处理器（Wave 0.8）：崩溃时打印完整调用栈 + minidump

#include <glm/gtc/packing.hpp>   // 白炉探针：half float 解码（RGBA16F 读回）

#include <algorithm>
#include <cmath>
#include <cstdlib>   // std::getenv（HE_CRASH_TEST 自检开关）
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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
// 默认读写内容目录下的正式配置；自动化实验可用 HE_NANITE_CONFIG=<路径> 指向临时文件
// （读写同一路径，因此指向临时文件即完全不触碰仓库内的正式配置）
static String g_ConfigPath = []{
    if (const char* p = std::getenv("HE_NANITE_CONFIG")) return String(p);
    return String(HUGE_CONTENT_DIR) + "Config/07_Nanite.cfg";
}();

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

// ============================================================
// 创建形状实体（立方体/球）——Cornell Box 场景构件
// ============================================================
// 平面镜测试台的几何（任务 32）：判据要在 CPU 侧做解析计算，所以参数必须**从同一处**导出，
// 不能一边写在场景搭建里、一边抄进检查脚本（那种"两份真值"迟早漂移）。
struct MirrorRigSpec {
    bool   valid = false;
    float3 planeNormal   = float3(0.0f, 1.0f, 0.0f);   // 镜面平面法线（朝向相机）
    float  planeOffset   = -1700.0f;                    // 平面方程 n·P + d = 0 的 d
    float3 slabCenter    = float3(0.0f, 1699.5f, 0.0f); // 镜面板中心
    float3 slabHalf      = float3(400.0f, 0.5f, 400.0f);// 镜面板半尺寸
    // 两个标记物（**立方体**，half* 是半边长；默认值与实际创建保持一致，避免两份真值漂移）
    float3 boxRed        = float3(-150.0f, 1750.0f, 100.0f);
    float  halfRed       = 50.0f;
    float3 boxGreen      = float3(170.0f, 1770.0f, -120.0f);
    float  halfGreen     = 45.0f;
};
static MirrorRigSpec g_MirrorRig;

Entity CreateShapeEntity(World& world, SceneGraph& sg,
                         const float3& position, const float3& scale,
                         const float4& baseColor, float metallic, float roughness,
                         bool sphere = false)
{
    Entity e = world.CreateEntity(sphere ? "Sphere" : "Cube");
    auto* xform = world.AddComponent<TransformComponent>(e);
    xform->position = position;
    xform->scale    = scale;

    MeshComponent* mesh;
    if (sphere) {
        auto* sc = world.AddComponent<SphereComponent>(e);
        sc->radius = 0.5f;
        mesh = static_cast<MeshComponent*>(sc);
    } else {
        auto* cc = world.AddComponent<CubeComponent>(e);
        cc->halfExtent = 0.5f;
        mesh = static_cast<MeshComponent*>(cc);
    }
    mesh->baseColorFactor  = baseColor;
    mesh->metallicFactor   = metallic;
    mesh->roughnessFactor  = roughness;
    sg.SetParent(e, Entity{kInvalidEntity});
    return e;
}

int main() {
    // ============================================================
    // 0. 安装崩溃处理器（Wave 0.8 诊断基建）
    //    崩溃时打印"函数名 + 源文件:行号"的完整调用栈，并写出 minidump。
    //    之所以放在最前面：初始化阶段（设备/交换链/资源创建）同样是崩溃高发区。
    // ============================================================
    {
        const std::string crashLog = std::string(HUGE_CONTENT_DIR) + "Config/07_Nanite_crash.log";
        he::InstallCrashHandler(crashLog.c_str(), nullptr);
    }

    // ============================================================
    // 1. 引擎启动
    // ============================================================
    EngineConfig config;
    config.appName      = "HugEngine — 07.Nanite (Cornell Box GI 对比)";
    config.windowWidth  = 1920;   // 窗口宽（960×2）
    config.windowHeight = 1080;   // 窗口高（540×2）
    // 【步骤 37 / L6 帧时判据】vsync 打开时墙钟帧率被锁在刷新率（60Hz），"有没有 60fps"
    // 这件事就没法从帧率上判定。`HE_NO_VSYNC=1` 关掉垂直同步，让墙钟帧率反映真实 GPU 吞吐。
    const bool noVsync = (std::getenv("HE_NO_VSYNC") != nullptr);
    config.enableVSync  = !noVsync;
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
        .vsync  = !noVsync,   // 与 EngineConfig 同源（HE_NO_VSYNC=1 时同时关掉，否则帧率仍被锁 60）
    });

    // ============================================================
    // 4. 初始化场景（Sponza glTF——检验各种 GI 的间接光照/反射）
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

    // 场景默认无直接光源（纯 GI）；DDGI 探针需首次照明输入（直接从 Lighting HDR 学习会自反馈发散），
    // 故加一个弱主方向光提供 DDGI 首次照明（强度低，画面仍以 GI 为主导）
    {
        Entity mainLightEntity = world.CreateEntity("DirectionalLight");
        world.AddComponent<TransformComponent>(mainLightEntity);
        auto* mainDL = world.AddComponent<DirectionalLight>(mainLightEntity);
        mainDL->direction = float3(0.3f, -1.0f, 0.4f);
        mainDL->color     = float3(1.0f, 0.95f, 0.9f);
        mainDL->intensity = 3.0f;   // 弱光：DDGI 输入，不喧宾夺主
        mainDL->castShadow = true;
        mainDL->enabled    = false; // 默认只看 GI（直接光关闭，勾选「只看 GI」可切回）
        sceneGraph.SetParent(mainLightEntity, Entity{kInvalidEntity});
    }

    // --- 点光源（2 个：Sponza 走廊两端，用于测试点光 GI/阴影/DDGI 探针）---
    // 位置/颜色/强度/范围可由面板编辑，并随 07_Nanite.cfg 序列化
    Entity pointLightEntities[2];
    {
        struct PointLightInit { float3 pos; float3 color; float intensity; float range; };
        const PointLightInit inits[2] = {
            { float3(-20.0f, 5.0f,  0.0f), float3(1.0f, 0.7f, 0.4f), 30.0f, 18.0f },  // 暖色，走廊左端（X 负侧）
            { float3( 20.0f, 5.0f,  0.0f), float3(0.4f, 0.7f, 1.0f), 30.0f, 18.0f },  // 冷色，走廊右端（X 正侧）
        };
        for (int i = 0; i < 2; i++) {
            Entity e = world.CreateEntity("PointLight");
            auto* tf = world.AddComponent<TransformComponent>(e);
            tf->position = inits[i].pos;
            auto* pl = world.AddComponent<PointLight>(e);
            pl->color     = inits[i].color;
            pl->intensity = inits[i].intensity;
            pl->range     = inits[i].range;
            pl->castShadow = false;       // 点光阴影开销大，默认关闭（面板可开）
            pl->enabled    = false;       // 默认只看 GI，与方向光一致
            sceneGraph.SetParent(e, Entity{kInvalidEntity});
            pointLightEntities[i] = e;
        }
    }

    // --- 平面镜测试台（任务 32 / §9.2-W 的解析对照用；HE_SSR_MIRROR=1 时创建）---
    // 为什么要有它：任务 25 只证明了"射线有效性恢复、两条 march 同量级、Hi-Z 更快"，
    // **没有**证明反射的位置/方向正确。平面镜恰好有一个闭式解析真值：物体中心 C 关于平面
    // (n, d) 的镜像点 C' = C − 2(n·C + d)n，相机 E 与 C' 的连线与镜面的交点 Q 就是"镜面上
    // 出现该物体反射"的那一点 —— 于是反射是否落在正确的像素上可以逐像素判定，而不靠肉眼。
    // 台子架在 Sponza 建筑上方（y≈500）的开阔处，背景是天空，避免建筑几何干扰判据。
    if (std::getenv("HE_SSR_MIRROR")) {
        // 【为什么是**地面镜**而不是竖镜】SSR 的命中判据只能命中**深度缓冲里存着的那一面**
        // （相机看到的那一面）。物体若夹在相机与竖镜之间，反射线打到的是物体的**背面**，
        // 深度图上却是它的正面 ⇒ 永远判不出命中（实测：竖镜下 99.96% 的镜面像素"有效"但
        // 一个物体颜色都没有）。地面镜不存在这个问题：反射线从镜面向上，命中的正是物体
        // 朝相机的那一面。
        // 【为什么架在 y=1700】Sponza 建筑高约 1556（y∈[-57,1499]），放进楼里会被墙挡住
        // （实测：相机在楼内时整屏只剩一面 25 单位外的墙）。架在楼顶之上，背景是天空，
        // 判据里"出现物体颜色"才唯一对应**反射**。
        // 镜面 = y=1700 平面、法线 +Y。用一块薄板实现：+Y 面即镜面。
        CreateShapeEntity(world, sceneGraph, float3(0.0f, 1699.5f, 0.0f), float3(800.0f, 1.0f, 800.0f),
                          float4(0.95f, 0.95f, 0.95f, 1.0f), /*metallic=*/1.0f, /*roughness=*/0.0f);
        // 两个已知物体（红/绿）。**必须用立方体而不是球**：地面镜里球体只有朝下的那一面
        // 会被反射到，而相机看不到那一面（深度图里存的是朝上的面）⇒ SSR 永远判不出命中
        // （实测：球体时预测像素附近 0.1% 有效）。立方体的**正面**朝相机，反射线从正面一侧
        // 打到它，深度图里存的就是那一面 ✓。
        CreateShapeEntity(world, sceneGraph, float3(-150.0f, 1750.0f, 100.0f), float3(100.0f, 100.0f, 100.0f),
                          float4(1.0f, 0.05f, 0.05f, 1.0f), 0.0f, 0.7f, /*sphere=*/false);
        CreateShapeEntity(world, sceneGraph, float3(170.0f, 1770.0f, -120.0f), float3(90.0f, 90.0f, 90.0f),
                          float4(0.05f, 1.0f, 0.05f, 1.0f), 0.0f, 0.7f, /*sphere=*/false);
        HE_CORE_INFO("[平面镜测试台] 已创建：地面镜 y=1700（法线 +Y）、红立方 (-150,1750,100) 半长 50、"
                     "绿立方 (170,1770,-120) 半长 45");
        g_MirrorRig.valid = true;   // 参数落盘给检查脚本用（见上方的 MirrorRigSpec 注释）
        g_MirrorRig.planeNormal = float3(0.0f, 1.0f, 0.0f);
        g_MirrorRig.planeOffset = -1700.0f;
        g_MirrorRig.slabCenter  = float3(0.0f, 1699.5f, 0.0f);
        g_MirrorRig.slabHalf    = float3(400.0f, 0.5f, 400.0f);
        g_MirrorRig.boxRed     = float3(-150.0f, 1750.0f, 100.0f);
        g_MirrorRig.halfRed    = 50.0f;
        g_MirrorRig.boxGreen   = float3(170.0f, 1770.0f, -120.0f);
        g_MirrorRig.halfGreen  = 45.0f;
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
        // 创建 bindless 默认占位纹理（必须在注册材质纹理之前设置）
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
    // 6. 初始化渲染管线（Forward / Deferred；光追经 GI 层栈的 RT 源启用）
    // ============================================================
    render::DeferredPipeline   deferredPipeline;   // 延迟管线（默认，GI 对比主用）
    render::ForwardPipeline    forwardPipeline;    // 前向管线

    // 初始尺寸必须用**交换链的真实尺寸**：请求的窗口是 1920x1080，但客户区实际是
    // 1920x1061；若 Initialize 用默认尺寸建资源，紧随其后的 OnResize 会把 GBuffer /
    // HDR / 后处理 / GI 半分辨率纹理 / RT 输出**整套销毁重建**，在启动期制造纹理与
    // framebuffer churn（校验层大量 "command buffer ... were invalidated"，
    // 并给"已销毁纹理仍被引用"留下窗口）。尺寸一致时 OnResize 会提前返回，等于空操作。
    deferredPipeline.Initialize(device.get(), swapchain->GetWidth(), swapchain->GetHeight());
    deferredPipeline.SetSwapChain(swapchain.get());
    deferredPipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());

    forwardPipeline.Initialize(device.get(), swapchain->GetWidth(), swapchain->GetHeight());
    forwardPipeline.SetSwapChain(swapchain.get());
    forwardPipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());


    int  g_PipelineMode = 1;                       // 0=Forward 1=Deferred 2=HybridRT
    bool g_PendingHalfResApply = false;            // 档位切换后延迟到帧边界重建半分辨率纹理（ImGui 回调内重建会死锁）
    bool g_GISolo = true;                          // 只看 GI（关闭直接光）
    int  g_GIPreset = 1;                           // GI 质量档位（默认 Medium=1）
    render::IRenderPipeline* curPipeline = &deferredPipeline;

    // ============================================================
    // 白炉探针（Wave 0.2）—— 能量守恒判据的读数设施
    //
    // 目的：白炉测试要求"全白环境 + albedo=1 + 关闭直接光"时物体应消失，即
    //       任意两点亮度应相等。这里读回 HDR 目标（**色调映射之前**，线性值）
    //       上两个像素：画面中心（物体所在）与背景点，输出亮度比供数值断言。
    // 实现：RHI 的 CopyTextureToBuffer 把 1×1 像素拷进 host 可见缓冲，等 GPU 完成后
    //       映射读取（RGBA16_FLOAT → half 解码）。仅在启用探针时每 N 帧执行一次，
    //       且会 WaitIdle（测试用途，不追求性能）。
    // ============================================================
    bool  g_ProbeEnabled   = (std::getenv("HE_FURNACE_PROBE") != nullptr);   // 也可用 HE_FURNACE_PROBE=1 直接开启（便于自动化验证）
    int   g_ProbeInterval  = 30;      // 每 N 帧采一次
    float g_ProbeCenter[3] = {0, 0, 0};   // 中心像素 RGB（线性）
    float g_ProbeBg[3]     = {0, 0, 0};   // 背景像素 RGB（线性）
    float g_ProbeRatio     = 0.0f;        // 中心亮度 / 背景亮度（白炉正确时应 ≈ 1）
    float g_ProbeLumCenter = 0.0f;        // 中心亮度绝对值（白炉正确时应 = 1.0）
    // 白炉数值测试（Wave 0.2）：源真值取白炉条件（全白环境 + albedo=1 + 关直接光）
    bool  g_FurnaceMode    = (std::getenv("HE_FURNACE") != nullptr);   // HE_FURNACE=1 直接开启
    std::unique_ptr<rhi::IRHIBuffer> probeBuffer;   // 2 个 RGBA16F 像素 = 32 B
    {
        rhi::BufferDesc pd;
        pd.size      = 32;
        pd.usage     = rhi::BufferUsage::Storage;   // 该路径恒定带 TRANSFER_DST，可作拷贝目标
        pd.cpuAccess = true;                        // 需要 Map 读回
        probeBuffer  = device->CreateBuffer(pd);
    }

    // ============================================================
    // GI 频谱采样（P5 步骤 0）—— 判定 DDGI 与 SSGI 是否需要"频率分离"
    //
    // 目的：把"单个漫反射源独开"时的 HDR 结果与 GBuffer albedo 原样落盘，供离线做
    //       频谱分析，回答那个决定性问题——"低通(SSGI) 是否 ≈ DDGI"。
    //       只有两者近似相等，SSGI − LowPass(SSGI) 才真正代表"DDGI 未覆盖的部分"；
    //       若不相等，频率分离就只是把一个源的偏差当成另一个源的补充，不如直接用加法。
    // 用法：HE_DUMP_GI=<标签>   [HE_DUMP_GI_FRAME=<帧号，默认 60>]
    //       输出 build/verify/gi_<标签>_<目标>.f16（RGBA16F 原始像素、无文件头、行紧密排布）
    //       与 _meta.txt（逐目标一行：名称 宽 高 格式）。目标含：
    //         hdr / albedo      —— 合成结果与接收端反照率
    //         provN_raw/final   —— 第 N 个有效 Provider 的原始输出 / 降噪后输出
    //       落盘后自动请求退出窗口，便于脚本化。
    // albedo 必须一并落盘：A 修复后各源都带接收端 albedo，albedo 纹理自身的高频会淹没
    //       要观察的 GI 频谱，离线分析必须先把 albedo 除掉。
    // 采样三组配置以便做差：漫反射栈=空（基线：天空+自发光+空气透视+镜面）、=[DDGI]、=[SSGI]；
    //       后两者减去基线即得各源自身的贡献（且两者带同一 albedo，比值即频谱之比）。
    // 实现：整幅 CopyTextureToBuffer 到 host 可见缓冲。仅测试路径使用，会 WaitIdle，不追求性能。
    // ============================================================
    const char*  dumpTagEnv   = std::getenv("HE_DUMP_GI");
    const char*  dumpFrameEnv = std::getenv("HE_DUMP_GI_FRAME");
    const String g_DumpTag    = dumpTagEnv ? dumpTagEnv : "";
    bool         g_DumpGI     = !g_DumpTag.empty();   // 非 const：失败时关闭以免每帧重试
    const u64    g_DumpFrame  = dumpFrameEnv ? (u64)std::max(1, std::atoi(dumpFrameEnv)) : 60ull;
    bool g_DumpDone    = false;   // 已录制拷贝（防止重复录制）
    bool g_DumpWritten = false;   // 已落盘（防止每帧重复写文件）
    // 采样目标列表：每项一张纹理 + 一条读回缓冲（尺寸各自取自纹理本身，故可混放不同分辨率）
    struct DumpTarget {
        String                          name;
        rhi::IRHITexture*               tex = nullptr;
        std::unique_ptr<rhi::IRHIBuffer> buf;
        u32                             w = 0, h = 0;
    };
    std::vector<DumpTarget> g_DumpTargets;
    if (g_DumpGI)
        HE_CORE_INFO("[GI采样] 已启用：标签={} 目标帧={} 输出 build/verify/gi_{}_*.f16",
                     g_DumpTag, g_DumpFrame, g_DumpTag);

    // 启动即应用白炉条件（HE_FURNACE=1 路径；面板开关在 GI 控制台里）
    if (g_FurnaceMode) {
        deferredPipeline.GetGIConfig()->furnaceMode = true;
        g_ProbeEnabled = true;
        g_GISolo       = true;
        world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& l){ l.enabled = false; });
        world.ForEach<he::PointLight>([&](he::Entity, he::PointLight& l){ l.enabled = false; });
        world.ForEach<he::SpotLight>([&](he::Entity, he::SpotLight& l){ l.enabled = false; });
        world.ForEach<he::RectLight>([&](he::Entity, he::RectLight& l){ l.enabled = false; });
        if (auto* gi = deferredPipeline.GetGI()) {   // 环境强度取 1（白炉真值）
            auto s = gi->GetSettings();               // GetSettings 返回 const& → 取副本改用 SetSettings
            s.intensity = 1.0f;
            gi->SetSettings(s);
        }
        HE_CORE_INFO("[白炉] 已启用白炉数值测试：关闭直接光、环境强度=1、探针已开");
    }

    // ── 从配置文件恢复管线 / GI / 后处理设置 ──
    if (hasConfig) {
        deferredPipeline.GetClusteredShading().enabled = GetInt(cfgData, "clustered", 1) != 0;
        deferredPipeline.GetGPUCulling().enabled       = GetInt(cfgData, "gpu_cull", 1) != 0;
        deferredPipeline.SetGBufferMode((render::GBufferRenderer::Mode)GetInt(cfgData, "gbuffer_mode", 0));

        // ── Nanite（§14.8 任务 1 / N0 开关；任务 3 假簇数量）：独立开关的"配置"层 ──
        // 真值只有一处：`NaniteSettings`（由 NaniteRenderer 持有），
        // 这里只做 cfg → 真值的单向恢复，与 `gi_half_res` 的往返写法同构。
        // 【键缺失时保留当前值】当前值 = NaniteRenderer::Initialize 从 CVar
        // `r.Nanite.Enable` / `r.Nanite.FakeClusters` 读到的启动默认；若这里硬写默认值，
        // 那份"控制台默认"就会被一份没有该键的 cfg 静默覆盖。
        {
            auto naniteSettings = deferredPipeline.GetNaniteSettings();
            naniteSettings.enabled = GetInt(cfgData, "nanite_enable",
                                            naniteSettings.enabled ? 1 : 0) != 0;
            // 假簇数量：钳制到 [0, kNaniteMaxFakeClusters]，与模块内的钳制口径一致
            naniteSettings.fakeClusters = (u32)std::max(0, std::min(
                GetInt(cfgData, "nanite_fake_clusters", (int)naniteSettings.fakeClusters),
                (int)1024));
            // 任务 4 的 UAV 自证开关（默认 0）：cfg → 真值，写法与上面两个键完全同构。
            // 它只决定模块是否在 GBuffer 之后追加 `Nanite_TestWrite`（往 albedo 写棋盘图案）。
            naniteSettings.testWrite = GetInt(cfgData, "nanite_test_write",
                                              naniteSettings.testWrite ? 1 : 0) != 0;
            // 任务 6 的 mesh PSO 自证开关（默认 0）：cfg → 真值，写法与上面三个键完全同构。
            // 它只决定模块是否追加 `Nanite_MeshTest`（最小 mesh PSO，只画模块自建的 1×1 小目标）。
            naniteSettings.meshTest = GetInt(cfgData, "nanite_mesh_test",
                                             naniteSettings.meshTest ? 1 : 0) != 0;
            // 任务 13 的合成实例网格条数（默认 64）：钳制到 [0, kNaniteMaxTestInstances]，
            // 与模块内的钳制口径一致。它是实例剔除验收（GPU vs CPU 逐项对照）的样本规模。
            naniteSettings.instanceTestCount = (u32)std::max(0, std::min(
                GetInt(cfgData, "nanite_instance_test_count", (int)naniteSettings.instanceTestCount),
                (int)render::kNaniteMaxTestInstances));
            deferredPipeline.SetNaniteSettings(naniteSettings);
            HE_CORE_INFO("[Nanite] 配置恢复: nanite_enable={} nanite_fake_clusters={} "
                         "nanite_test_write={} nanite_mesh_test={} nanite_instance_test_count={}",
                         naniteSettings.enabled ? 1 : 0, naniteSettings.fakeClusters,
                         naniteSettings.testWrite ? 1 : 0, naniteSettings.meshTest ? 1 : 0,
                         naniteSettings.instanceTestCount);
        }

        auto& ae = deferredPipeline.GetAutoExposure();
        ae.SetEnabled(GetInt(cfgData, "ae_enabled", 0) != 0);
        ae.SetAdaptSpeed(GetFloat(cfgData, "ae_adapt_speed", 2.0f));
        ae.SetTargetLum(GetFloat(cfgData, "ae_target_lum", 0.18f));

        auto& bloom = deferredPipeline.GetBloom();
        bloom.SetEnabled(GetInt(cfgData, "bloom_enabled", 0) != 0);
        bloom.SetThreshold(GetFloat(cfgData, "bloom_threshold", 1.0f));
        bloom.SetIntensity(GetFloat(cfgData, "bloom_intensity", 0.5f));

        auto& dof = deferredPipeline.GetDOF();
        dof.SetEnabled(GetInt(cfgData, "dof_enabled", 0) != 0);
        dof.SetFocusDepth(GetFloat(cfgData, "dof_focus", 0.5f));
        dof.SetFocusRange(GetFloat(cfgData, "dof_range", 0.1f));
        dof.SetIntensity(GetFloat(cfgData, "dof_intensity", 1.0f));

        auto& mb = deferredPipeline.GetMotionBlur();
        mb.SetEnabled(GetInt(cfgData, "mb_enabled", 0) != 0);
        mb.SetIntensity(GetFloat(cfgData, "mb_intensity", 0.5f));

        // 注：AO/SSGI/DDGI/SSR 的「启用」不再独立配置——统一由 GI 层栈派生
        //（见下方层栈恢复后的同步），避免出现「层栈说参与、子系统却关着」的不一致。
        // 这里只恢复各源的算法参数。

        if (auto* gi = deferredPipeline.GetGI()) {
            auto s = gi->GetSettings();
            s.intensity = GetFloat(cfgData, "ibl_intensity", 1.0f);
            gi->SetSettings(s);
        }
        if (auto* ssgi = deferredPipeline.GetSSGI()) {
            ssgi->radius      = GetFloat(cfgData, "ssgi_radius", 1.0f);
            ssgi->sampleCount = GetInt(cfgData, "ssgi_samples", 16);
            auto s = ssgi->GetSettings();
            s.intensity = GetFloat(cfgData, "ssgi_intensity", 1.0f);
            ssgi->SetSettings(s);
        }
        if (auto* ddgi = deferredPipeline.GetDDGI()) {
            ddgi->blendAlpha = GetFloat(cfgData, "ddgi_blend", 0.9f);
            ddgi->debugScale = GetFloat(cfgData, "ddgi_scale", 1.0f);
            // 时间维分摊（任务 12）：每 N 帧更新一轮探针；1 = 每帧全量（默认）
            ddgi->updateStride = (u32)std::max(1, GetInt(cfgData, "ddgi_update_stride", 1));
            // 网格自动拟合（任务 14 / §9.2-K）：默认开启；关闭后沿用固定网格参数，
            // 覆盖不到的区域会被 clamp 成贴边常数外推（实测贡献几乎全部来自该外推）。
            ddgi->autoFitGrid = GetInt(cfgData, "ddgi_grid_auto", 1) != 0;
            ddgi->fitCellsMax = (u32)std::max(2, GetInt(cfgData, "ddgi_fit_cells", 16));
            auto s = ddgi->GetSettings();
            s.intensity = GetFloat(cfgData, "ddgi_intensity", 1.0f);
            ddgi->SetSettings(s);
        }
        if (auto* ssr = deferredPipeline.GetSSR()) {
            ssr->maxSteps = GetFloat(cfgData, "ssr_max_steps", 64.0f);
            ssr->stepSize = GetFloat(cfgData, "ssr_step_size", 0.5f);
            // Hi-Z 层次 march 开关（任务 25）：默认 1；置 0 强制线性 march，用于两条路径对照
            ssr->useHiZ   = GetInt(cfgData, "ssr_use_hiz", 1) != 0;
            // march 参数按场景尺度自动推导（任务 32）：默认开。置 0 时用下面两个显式键，
            // 把"场景尺度假设"单独摆出来做 A/B（判据见 Tools/gi/ssr_mirror_check.ps1）。
            ssr->autoScaleMarch = GetInt(cfgData, "ssr_auto_scale", 1) != 0;
            if (!ssr->autoScaleMarch) {
                ssr->maxDistance = GetFloat(cfgData, "ssr_max_distance", 50.0f);
                ssr->thickness   = GetFloat(cfgData, "ssr_thickness", 0.1f);
            }
        }

        // ── 面板状态：管线 / GI 档位 / 只看 GI / GI 通道配置 ──
        g_PipelineMode = GetInt(cfgData, "pipeline_mode", 1);
        if (g_PipelineMode > 1) g_PipelineMode = 1;   // 兼容旧配置：HybridRT(2) 已并入 Deferred + RT 层栈
        g_GISolo       = GetInt(cfgData, "gi_solo", 1) != 0;
        // 应用「只看 GI」到场景光源（cfg 加载发生在场景创建之后）
        world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& l){ l.enabled = !g_GISolo; });
        world.ForEach<he::PointLight>([&](he::Entity, he::PointLight& l){ l.enabled = !g_GISolo; });
        world.ForEach<he::SpotLight>([&](he::Entity, he::SpotLight& l){ l.enabled = !g_GISolo; });
        world.ForEach<he::RectLight>([&](he::Entity, he::RectLight& l){ l.enabled = !g_GISolo; });
        g_GIPreset     = GetInt(cfgData, "gi_preset", -1);
        {
            // 层栈恢复：从 cfg 的「每通道源权重」重建（键缺失时保留默认预设值）
            //
            // 【任务 26 起这段是**两个管线共用**的】原先它只写 `deferredPipeline.GetGIConfig()`，
            // 于是 `pipeline_mode=0`（Forward）下这些 GI 键对画面**没有任何影响** ——
            // 任务 26 的判据（把 diffuse 从 {IBL} 改成 {IBL,RSM} 看读数）根本做不了。
            // 现在抽成 lambda 对两个管线各套一次，各自再按**自己的能力位**降级。
            auto applyGIConfig = [&](render::GIConfig& gc) {
            gc.giIntensity = GetFloat(cfgData, "gi_intensity", 1.0f);
            gc.aoIntensity = GetFloat(cfgData, "ao_intensity", 1.0f);
            // 屏幕覆盖置信度的边缘带宽（§3.2）：默认 5% 只影响贴边的一条，调大即可把它
            // 变成「整屏可见」的实验——用于验证置信度确实作用于权重，而不是死代码。
            gc.edgeFade    = GetFloat(cfgData, "gi_edge_fade", 0.05f);
            gc.rsmIndirect = GetInt(cfgData, "gi_rsm_indirect", 1) != 0;
            gc.halfRes     = GetInt(cfgData, "gi_half_res", 0) != 0;

            auto loadStack = [&](render::GIChannelStack& st, const char* key, int cap0, int cap1, int cap2, int cap3) {
                const float w[4] = {
                    GetFloat(cfgData, (String(key) + "_w0").c_str(), st.WeightOf((render::GISourceId)cap0)),
                    GetFloat(cfgData, (String(key) + "_w1").c_str(), st.WeightOf((render::GISourceId)cap1)),
                    GetFloat(cfgData, (String(key) + "_w2").c_str(), st.WeightOf((render::GISourceId)cap2)),
                    GetFloat(cfgData, (String(key) + "_w3").c_str(), st.WeightOf((render::GISourceId)cap3)),
                };
                const int ids[4] = { cap0, cap1, cap2, cap3 };
                st.Clear();
                for (int i = 0; i < 4; i++) {
                    if (ids[i] >= 0 && w[i] > 0.0f) st.Set((render::GISourceId)ids[i], w[i]);
                }
            };
            // 通道源顺序与面板一致（低频 → 高频）
            loadStack(gc.diffuse, "gi_blend_diffuse",
                      (int)render::GISourceId::IBL, (int)render::GISourceId::DDGI,
                      (int)render::GISourceId::SSGI, (int)render::GISourceId::RTGI);
            loadStack(gc.specular, "gi_blend_specular",
                      (int)render::GISourceId::IBL, (int)render::GISourceId::SSR,
                      (int)render::GISourceId::RTReflection, (int)-1);
            loadStack(gc.ao, "gi_blend_ao",
                      (int)render::GISourceId::SSAO, (int)render::GISourceId::RTAO, (int)-1, (int)-1);
            // RSM 不在上面三个通道的 4 个固定槽位里（槽位是「低频 → 高频」的固定顺序），
            // 但它有**独立门控**（§9.2-F：RSM 与 DDGI 各自判定），面板也能勾选它，
            // 因此单列一个键，保证「配置 → 层栈」这条路上 RSM 不丢（否则配置往返有损，
            // RSM 只能靠面板手工勾选，回归检查无从复现）。
            gc.diffuse.Set(render::GISourceId::RSM,
                           GetFloat(cfgData, "gi_blend_diffuse_rsm", gc.diffuse.WeightOf(render::GISourceId::RSM)));
            // Lumen 与 RSM 同理：它同时属于漫反射与镜面两个通道，塞进 4 个固定槽位会改变
            // 其它源的槽位语义（旧 cfg 的 _w2 会被重新解释成别的源）→ 单列键，配置往返无损。
            gc.diffuse.Set(render::GISourceId::Lumen,
                           GetFloat(cfgData, "gi_blend_diffuse_lumen", gc.diffuse.WeightOf(render::GISourceId::Lumen)));
            gc.specular.Set(render::GISourceId::Lumen,
                            GetFloat(cfgData, "gi_blend_specular_lumen", gc.specular.WeightOf(render::GISourceId::Lumen)));
            // 阴影通道独立于层栈（可见性乘法项，非能量源）→ 用枚举恢复
            gc.shadow = (render::ShadowChannel)GetInt(cfgData, "gi_shadow", (int)gc.shadow);

            // 层栈恢复后**不需要**在这里同步子系统开关：框架的 IGIProvider::SyncToStack
            // 每帧按层栈对齐子系统的 enabled（不变量 1：层栈与子系统开关同源）。
            // 这里曾有一份手工补丁，正是「复发过一次」的那一处 —— 把不变量的维护交给调用方，
            // 就必然会有下一个忘记同步的调用方（§9.2-G）。
            };
            applyGIConfig(*deferredPipeline.GetGIConfig());

            // 【步骤 34 发现的连通性缺口：`gi_half_res` 到不了渲染】
            // 上面只写了 `GIConfig::halfRes`，而 SSGI/SSR 读的是**各自的** `GISettings::halfRes`；
            // 启动路径上没有任何一处把两者连起来（只有 ImGui 的档位切换会同步它们），于是
            // 「半分辨率」这一档**从配置文件根本到不了 SSGI/SSR** —— 而 cfg 写回里却有
            // `gi_half_res`（配置往返有损，半分辨率路径也无法用 cfg 回归）。
            // 这里补上同步：输出纹理的重建由帧图的 `Provider::SyncToStack → SyncOutputSize`
            // 每帧核对完成，故只需要写设置。（SSAO 有自己的 `ssao_half_res` 键，不在此列。）
            if (auto* gi = deferredPipeline.GetSSGI()) {
                auto s = gi->GetSettings();   // GetSettings 返回 const& → 取副本再 SetSettings
                s.halfRes = deferredPipeline.GetGIConfig()->halfRes;
                gi->SetSettings(s);
            }
            if (auto* gi = deferredPipeline.GetSSR()) {
                auto s = gi->GetSettings();
                s.halfRes = deferredPipeline.GetGIConfig()->halfRes;
                gi->SetSettings(s);
            }
            // Forward：从**它自己的预设基线**出发套同一份键，再按 Forward 的能力位降级 ——
            // 不降级的话 Forward 会带着它跑不了的源（SSGI/DDGI/光追）进层栈，
            // 正是 §9.2-G 那个"归一化里计权重、却没人产出"的失效形态。
            {
                render::GIConfig fwd = render::GIConfigFromPreset((render::GIQualityPreset)g_GIPreset);
                applyGIConfig(fwd);
                *forwardPipeline.GetGIConfig() = render::GIRegistry::Degrade(
                    fwd, render::PipelineCaps::Forward, device->GetCaps().supportsRayTracing);
            }
        }
        {
            auto& ssao = deferredPipeline.GetSSAO();
            ssao.radius      = GetFloat(cfgData, "ssao_radius", 1.0f);
            ssao.sampleCount = GetInt(cfgData, "ssao_samples", 16);
            ssao.halfRes     = GetInt(cfgData, "ssao_half_res", 0) != 0;
        }

        HE_CORE_INFO("管道设置已从配置文件恢复");
    }

    HE_CORE_INFO("DeferredPipeline 初始化完成");

    // ============================================================
    // 7. 创建命令列表
    // ============================================================
    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());
    // 预设 ToneMap PSO → 匹配 BGRA8_UNORM RP（ImGui LoadOp 兼容）
    cmdList->SetPipeline(deferredPipeline.GetToneMap()->GetPSO());

    // ============================================================
    // 8. ImGui 初始化
    // ============================================================
    GLFWwindow* glfwWin = engine.GetWindow()->GetNativeHandle();
    editor::ImGuiIntegration imgui;
    imgui.Initialize(glfwWin, device.get(), swapchain.get());
    // 各面板的窗口位置/大小/折叠状态由 ImGui 自动序列化——保存到配置目录（与 07_Nanite.cfg 同处）
    // 用绝对路径（HUGE_CONTENT_DIR）避免依赖当前工作目录
    static String g_ImGuiIniPath = String(HUGE_CONTENT_DIR) + "Config/07_Nanite_imgui.ini";
    ImGui::GetIO().IniFilename = g_ImGuiIniPath.c_str();

    // ============================================================
    // 9. 相机 — 从配置文件加载，否则使用默认位置
    // ============================================================
    render::CameraController camCtrl;
    camCtrl.SetAspectRatio(
        static_cast<float>(swapchain->GetWidth()),
        static_cast<float>(swapchain->GetHeight()));
    camCtrl.SetMoveSpeed(hasConfig ? GetFloat(cfgData, "cam_move_speed", 200.0f) : 200.0f);

    if (hasConfig) {
        camCtrl.SetPosition(float3(
            GetFloat(cfgData, "cam_pos_x", 0.0f),
            GetFloat(cfgData, "cam_pos_y", 3.0f),
            GetFloat(cfgData, "cam_pos_z", 0.0f)));
        camCtrl.SetOrientation(
            GetFloat(cfgData, "cam_yaw", -1.57f),
            GetFloat(cfgData, "cam_pitch", -0.1f));
        camCtrl.GetCamera().nearPlane = GetFloat(cfgData, "cam_near", 0.1f);
        camCtrl.GetCamera().farPlane  = GetFloat(cfgData, "cam_far", 3000.0f);
        // 视场角：屏幕空间源（SSGI/SSR/SSAO）的空间重建必须用渲染深度图时的那套投影参数，
        // 而这条路径曾经用硬编码的 60°/0.1/2000 自拼矩阵。留一个配置入口才能把
        // 「非默认相机」这一条判据做成可复现的回归检查（§9.2-E）。
        camCtrl.GetCamera().fov        = GetFloat(cfgData, "cam_fov", kDefaultFOV);
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
        // 【为什么留一个环境变量开关】步骤 31（L5 Radiance Cache）的验收有一条是"相机移动时
        // 无拖影累积"—— 静态相机测不出来。`HE_CAMERA_ORBIT=1` 让这条圆形路径直接跑起来。
        // 注意它按**墙钟** deltaTime 推进 ⇒ 该模式下的画面不保证逐位可复现（静态模式不受影响）。
        if (std::getenv("HE_CAMERA_ORBIT")) {
            camAnim->playing = true;
            HE_CORE_INFO("相机动画已启用（HE_CAMERA_ORBIT=1）：用于验证时间混合无拖影累积");
        }
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
        deferredPipeline.OnResize(w, h);
        forwardPipeline.OnResize(w, h);
        camCtrl.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // ============================================================
    // 11. 主渲染循环
    // ============================================================
    HE_CORE_INFO("07.Nanite (Cornell Box) 启动 — WASD=移动, 右键拖拽=旋转, Shift=加速, E/Q=升降");
    u64 frameIndex = 0;
    f64 lastTime   = glfwGetTime();

    while (!engine.GetWindow()->ShouldClose()) {
        f64 now       = glfwGetTime();
        f32 deltaTime = static_cast<f32>(now - lastTime);
        lastTime      = now;
        // 【步骤 37】CPU 侧耗时：从帧首到 Present 之前。与墙钟帧时一起看才能判定"CPU 受限还是
        // GPU 受限"—— 只看墙钟帧率会把"CPU 在重建帧图"误读成"GPU 太慢"，从而去优化错的对象。
        const auto cpuT0 = std::chrono::steady_clock::now();

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

        // --- 渲染（按选择的管线执行；各管线均通过 RenderGraph 自动编排）---
        cmdList->Begin();
        switch (g_PipelineMode) {
        case 0:  // Forward
            curPipeline = &forwardPipeline;
            break;
        default: // Deferred（含光追源：RT 已归入 GI 层栈，无需独立管线）
            curPipeline = &deferredPipeline;
            break;
        }
        curPipeline->NextFrame();
        // --- Forward 的阴影系统必须由**调用方**驱动（任务 34 / §9.2-AD）---
        // `ShadowSystem` 不像 GI 子系统那样自己从帧图拿数据：它要靠调用方先
        // `SetRenderResources`（对象/阴影缓冲 + 描述符集）再 `Update`（收集投影光源、拟合 CSM），
        // 之后 `HasActiveShadows()` 才为真。02.Cube / 03.Sponza-Forward / AISamples 都这么做，
        // **07.Nanite 此前漏了** ⇒ Forward 模式下 `Shadow` 与 `RSM_Generate` 两个 pass 都不注册：
        // 画面**没有阴影**，RSM 源恒为 0（而"层栈改变画面 / 多源不变亮 / 双源等于加权平均"
        // 三条判据在"某个源恒为 0"时全部成立，看不出这件事）。
        // 位置：必须在 NextFrame 之后（阴影缓冲按飞行帧轮换）且在本帧 Render 之前（帧图按
        // `HasActiveShadows()` 门控）。Deferred 侧由管线内部自己驱动，这里只处理 Forward。
        if (g_PipelineMode == 0) {
            if (auto* shadowSys = forwardPipeline.GetShadowSystem()) {
                shadowSys->SetRenderResources(forwardPipeline.GetCurrentShadowObjectBuffer(),
                                              forwardPipeline.GetCurrentShadowBuffer(),
                                              forwardPipeline.GetCurrentDescSet());
                render::SubsystemContext shadowCtx;
                shadowCtx.world      = &world;
                shadowCtx.sceneGraph = &sceneGraph;
                shadowCtx.camera     = &camCtrl.GetCamera();
                // 物理天空的太阳方向先同步到方向光：阴影与光照必须同向（02.Cube 同款做法）
                he::SyncPhysicalSkyToSun(world);
                shadowSys->Update(shadowCtx);
            }
        }
        // 帧边界应用延迟的半分辨率纹理重建（先等待 GPU 空闲，避免销毁正在使用的纹理）
        if (g_PendingHalfResApply) {
            device->WaitIdle();
            if (auto* dpApply = dynamic_cast<render::DeferredPipeline*>(curPipeline)) {
                dpApply->GetSSGI()->OnResize(swapchain->GetWidth(), swapchain->GetHeight());
                dpApply->GetSSR()->OnResize(swapchain->GetWidth(), swapchain->GetHeight());
                dpApply->GetSSAO().OnResize(swapchain->GetWidth(), swapchain->GetHeight());
            }
            g_PendingHalfResApply = false;
        }
        // 【步骤 37】管线 CPU 侧耗时（重建帧图 + 录制 + 提交）与"其余 CPU"分开计：
        // 整帧 CPU 受限时，先要知道这 50 ms 是花在管线里还是花在样例/ImGui 里，否则优化对象会选错。
        static double s_accPipelineMs = 0.0;
        {
            const auto t0 = std::chrono::steady_clock::now();
            curPipeline->Render(cmdList.get(), world, sceneGraph, camCtrl.GetCamera());
            s_accPipelineMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        }

        // --- 崩溃处理器自检（Wave 0.8）---
        // 设置环境变量 HE_CRASH_TEST=1 启动，会在第 3 帧主动解引用空指针，
        // 用于验证崩溃处理器能否打出完整调用栈（默认关闭，不影响正常使用）。
        // 放在 Render 之后是为了让调用栈具备真实深度。
        if (std::getenv("HE_CRASH_TEST")) {
            static int s_CrashTestFrame = 0;
            if (++s_CrashTestFrame == 3) {
                HE_CORE_ERROR("HE_CRASH_TEST=1：主动触发崩溃，用于验证崩溃处理器");
                volatile int* nullPtr = nullptr;   // volatile 保证编译器不优化掉这次写入
                *nullPtr = 1;
            }
        }

        // --- ImGui（LOAD 保留 ToneMap 输出）---
        cmdList->BeginRenderPass(1, rhi::Format::BGRA8_UNORM,
            rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);

        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("07.Nanite (Cornell Box)");
        {
            float fps = 1.0f / (deltaTime > 0.001f ? deltaTime : 0.016f);
            ImGui::TextColored({0.3f, 1.0f, 0.3f, 1.0f}, "FPS: %.0f", fps);
            ImGui::SameLine(120);
            ImGui::TextColored({0.6f, 0.6f, 0.6f, 1.0f}, "(%.2f ms)", deltaTime * 1000.0f);

            ImGui::SeparatorText("延迟渲染管线");
            ImGui::Text("GBuffer + Lighting Pass (全屏 PBR)");
            ImGui::Text("3×MRT (albedo+metallic | normal+roughness | emissive+ao) + D32");
            bool clustered = deferredPipeline.GetClusteredShading().enabled;
            if (ImGui::Checkbox("Clustered Shading", &clustered))
                deferredPipeline.GetClusteredShading().enabled = clustered;
            if (clustered) {
                ImGui::SameLine();
                ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "%u clusters",
                    deferredPipeline.GetClusteredShading().GetClusterCount());
            }
            bool gpuCull = deferredPipeline.GetGPUCulling().enabled;
            if (ImGui::Checkbox("GPU 视锥剔除", &gpuCull))
                deferredPipeline.GetGPUCulling().enabled = gpuCull;
            if (gpuCull) {
                ImGui::SameLine();
                ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "%u 可见",
                    deferredPipeline.GetGPUCulling().GetLastVisibleCount());
            }

            // 相机
            ImGui::SeparatorText("相机");
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


            // ── AutoExposure ──
            ImGui::SeparatorText("AutoExposure");
            {
                auto& ae = deferredPipeline.GetAutoExposure();
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

            // 场景统计
            ImGui::SeparatorText("场景");
            u32 meshCount = 0, dirLightCount = 0, spotCount = 0;
            world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent&) { meshCount++; });
            world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight&) { dirLightCount++; });
            world.ForEach<he::SpotLight>([&](he::Entity, he::SpotLight&) { spotCount++; });
            ImGui::Text("%u 网格  |  %u 方向光  |  %u 点光  |  %u 聚光", meshCount, dirLightCount, 1, spotCount);

            // ── 光源列表：类型 / 开关 / 颜色 / 强度 / 方向（可直接编辑）──
            ImGui::SeparatorText("光源");
            int lightId = 0;
            world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& l) {
                ImGui::PushID(lightId++);
                ImGui::TextUnformatted("方向光"); ImGui::SameLine();
                ImGui::Checkbox("启用##dl", &l.enabled); ImGui::SameLine();
                ImGui::ColorEdit3("颜色##dl", &l.color.x, ImGuiColorEditFlags_NoInputs); ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::DragFloat("强度##dl", &l.intensity, 0.05f, 0.0f, 100.0f, "%.2f");
                ImGui::SetNextItemWidth(220.0f);
                ImGui::DragFloat3("方向##dl", &l.direction.x, 0.01f, -1.0f, 1.0f, "%.2f");
                ImGui::Checkbox("投射阴影##dl", &l.castShadow);
                ImGui::PopID();
            });
            world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& l) {
                ImGui::PushID(lightId++);
                ImGui::TextUnformatted("点光  "); ImGui::SameLine();
                ImGui::Checkbox("启用##pl", &l.enabled); ImGui::SameLine();
                ImGui::ColorEdit3("颜色##pl", &l.color.x, ImGuiColorEditFlags_NoInputs); ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::DragFloat("强度##pl", &l.intensity, 0.05f, 0.0f, 100.0f, "%.2f");
                ImGui::SetNextItemWidth(90.0f);
                ImGui::DragFloat("范围##pl", &l.range, 0.1f, 0.1f, 500.0f, "%.1f");
                // 位置编辑：修改本地变换后标记 SceneGraph 脏，GetWorldPosition 即时反映
                if (auto* tf = world.GetComponent<he::TransformComponent>(e)) {
                    float3 p = tf->position;
                    if (ImGui::DragFloat3("位置##pl", &p.x, 0.1f, -500.0f, 500.0f, "%.1f")) {
                        tf->position = p;
                        sceneGraph.MarkDirty(e);
                    }
                }
                ImGui::PopID();
            });
            world.ForEach<he::SpotLight>([&](he::Entity, he::SpotLight& l) {
                ImGui::PushID(lightId++);
                ImGui::TextUnformatted("聚光  "); ImGui::SameLine();
                ImGui::Checkbox("启用##sl", &l.enabled); ImGui::SameLine();
                ImGui::ColorEdit3("颜色##sl", &l.color.x, ImGuiColorEditFlags_NoInputs); ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::DragFloat("强度##sl", &l.intensity, 0.05f, 0.0f, 100.0f, "%.2f");
                ImGui::SetNextItemWidth(90.0f);
                ImGui::DragFloat("范围##sl", &l.range, 0.1f, 0.1f, 500.0f, "%.1f");
                ImGui::SetNextItemWidth(160.0f);
                ImGui::DragFloat3("方向##sl", &l.direction.x, 0.01f, -1.0f, 1.0f, "%.2f");
                ImGui::PopID();
            });
            world.ForEach<he::RectLight>([&](he::Entity, he::RectLight& l) {
                ImGui::PushID(lightId++);
                ImGui::TextUnformatted("矩形光"); ImGui::SameLine();
                ImGui::Checkbox("启用##rl", &l.enabled); ImGui::SameLine();
                ImGui::ColorEdit3("颜色##rl", &l.color.x, ImGuiColorEditFlags_NoInputs); ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::DragFloat("强度##rl", &l.intensity, 0.05f, 0.0f, 100.0f, "%.2f");
                ImGui::SetNextItemWidth(90.0f);
                ImGui::DragFloat("范围##rl", &l.range, 0.1f, 0.1f, 500.0f, "%.1f");
                ImGui::PopID();
            });
            if (lightId == 0) {
                ImGui::TextUnformatted("（场景中无光源）");
            }
        }
        ImGui::End();

        // ============================================================
        // 独立 GI 控制面板：渲染管线 → GI 质量档位 → GI 四通道（可用性 + 参数）
        // 与主面板分离，便于独立摆放/查看
        // ============================================================
        // 首次使用时默认摆放在主面板右侧（之后由 imgui.ini 恢复用户位置/大小）
        ImGui::SetNextWindowPos(ImVec2(560.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430.0f, 700.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("GI 控制台");
        {
            // ── 渲染管线选择（Forward / Deferred）──
            // 注：光追已归入 GI 源（层栈中的 RTGI/RT 反射/RTAO/RT 阴影），
            //     Deferred 管线勾选 RT 源即等价于此前的 HybridRT，无需独立管线
            ImGui::SeparatorText("渲染管线");
            {
                const char* pipelineNames[] = {"Forward", "Deferred"};
                int prevMode = g_PipelineMode;
                ImGui::Combo("管线##pipeline", &g_PipelineMode, pipelineNames, 2);
                if (g_PipelineMode != prevMode) {
                    // 切换管线：确保交换链与视口尺寸同步
                    curPipeline = (g_PipelineMode == 0) ? static_cast<render::IRenderPipeline*>(&forwardPipeline)
                                                        : static_cast<render::IRenderPipeline*>(&deferredPipeline);
                    curPipeline->SetSwapChain(swapchain.get());
                    curPipeline->OnResize(swapchain->GetWidth(), swapchain->GetHeight());
                }
            }

            // ── GI 实验室：只看 GI（关闭直接光）→ 逐个开启 GI 看间接光贡献 ──
            ImGui::SeparatorText("GI 实验室");
            if (ImGui::Checkbox("只看 GI（关闭直接光）", &g_GISolo)) {
                // 关闭/恢复所有直接光源：画面只剩 GI（IBL/SSGI/DDGI/RSM 等）的间接光
                world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& l){ l.enabled = !g_GISolo; });
                world.ForEach<he::PointLight>([&](he::Entity, he::PointLight& l){ l.enabled = !g_GISolo; });
                world.ForEach<he::SpotLight>([&](he::Entity, he::SpotLight& l){ l.enabled = !g_GISolo; });
                world.ForEach<he::RectLight>([&](he::Entity, he::RectLight& l){ l.enabled = !g_GISolo; });
            }
            ImGui::TextWrapped("开启后画面只剩 GI 间接光——\n逐个启用 SSGI/DDGI 看各自贡献；IBL 强度即环境 GI 强度。");

            // ── 白炉探针（Wave 0.2）：能量守恒的数值读数 ──
            ImGui::SeparatorText("白炉探针（能量守恒）");
            ImGui::Checkbox("启用探针（读回 HDR 目标像素）", &g_ProbeEnabled);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderInt("采样间隔(帧)", &g_ProbeInterval, 1, 120);
            if (ImGui::Checkbox("白炉数值测试（源真值=1）", &g_FurnaceMode)) {
                // 白炉条件：全白环境 + albedo=1 + 关闭直接光（源真值由 shader 代入，这里保证场景端一致）
                if (auto* gcPtr = curPipeline->GetGIConfig()) gcPtr->furnaceMode = g_FurnaceMode;
                if (g_FurnaceMode) g_ProbeEnabled = true;   // 打开白炉即自动开探针
                if (g_FurnaceMode) {
                    g_GISolo = true;   // 关闭全部直接光
                    world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& l){ l.enabled = false; });
                    world.ForEach<he::PointLight>([&](he::Entity, he::PointLight& l){ l.enabled = false; });
                    world.ForEach<he::SpotLight>([&](he::Entity, he::SpotLight& l){ l.enabled = false; });
                    world.ForEach<he::RectLight>([&](he::Entity, he::RectLight& l){ l.enabled = false; });
                    if (auto* gi = curPipeline->GetGI()) {   // 环境强度取 1（白炉真值）
                        auto s = gi->GetSettings();
                        s.intensity = 1.0f;
                        gi->SetSettings(s);
                    }
                }
            }
            ImGui::TextWrapped("白炉条件：全白环境 + albedo=1 + 关闭直接光 → 各源真值均为 1，\n"
                               "正确的分层合成应恰好读回 1.0；>1 即存在归一化之外的双重计数。");
            if (g_ProbeEnabled) {
                ImGui::Text("中心像素 (线性 RGB): %.4f %.4f %.4f", g_ProbeCenter[0], g_ProbeCenter[1], g_ProbeCenter[2]);
                ImGui::Text("背景像素 (线性 RGB): %.4f %.4f %.4f", g_ProbeBg[0], g_ProbeBg[1], g_ProbeBg[2]);
                // 白炉正确时：物体消失 → 中心与背景都应等于环境真值 1.0
                const bool ok = (g_ProbeRatio > 0.98f && g_ProbeRatio < 1.02f);
                const ImVec4 col = ok ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
                ImGui::TextColored(col, "亮度比 中心/背景 = %.4f   (白炉判据 0.98 ~ 1.02)", g_ProbeRatio);
                ImGui::TextColored(col, "中心亮度 = %.4f   (白炉真值 1.0)", g_ProbeLumCenter);
            }

            // ── GI 质量档位（紧随管线选择）──
            ImGui::SeparatorText("GI 质量档位");
            const char* presetNames[] = {"Low", "Medium", "High", "Ultra"};
            render::GIConfig& gc = *curPipeline->GetGIConfig();
            const u32  giCaps = curPipeline->GetGIPipelineCaps();
            const bool rtOk   = device->GetCaps().supportsRayTracing;
            if (ImGui::Combo("档位##preset", &g_GIPreset, presetNames, 4)) {
                // 应用预设 + 按「管线能力 ∧ 设备能力」自动降级
                gc = render::GIRegistry::Degrade(render::GIConfigFromPreset((render::GIQualityPreset)g_GIPreset),
                                                 giCaps, rtOk);
                if (auto* dp = dynamic_cast<render::DeferredPipeline*>(curPipeline)) {
                    dp->GetSSGI()->SetEnabled(gc.ShouldRunSSGI());
                    dp->GetDDGI()->SetEnabled(gc.ShouldRunDDGI());
                    dp->GetSSR()->SetEnabled(gc.ShouldRunSpecular());
                    dp->GetSSAO().enabled = gc.ShouldRunAO();
                    auto sg = dp->GetSSGI()->GetSettings();
                    sg.halfRes = gc.halfRes;
                    dp->GetSSGI()->SetSettings(sg);
                    auto sr = dp->GetSSR()->GetSettings();
                    sr.halfRes = gc.halfRes;
                    dp->GetSSR()->SetSettings(sr);
                    dp->GetSSAO().halfRes = gc.halfRes;
                    // 纹理重建延迟到帧边界（NextFrame 之后），避免在 ImGui 回调内销毁/创建正在使用的纹理
                    g_PendingHalfResApply = true;
                }
            }

            // 当前管线可用的 GI 子系统（按管线类型获取，Forward 无屏幕空间/探针 GI）
            auto* dp = dynamic_cast<render::DeferredPipeline*>(curPipeline);
            auto* giSSGI = dp ? dp->GetSSGI() : nullptr;
            auto* giSSR  = dp ? dp->GetSSR()  : nullptr;
            auto* giDDGI = dp ? dp->GetDDGI() : nullptr;

            // ── Nanite 模块（§14.8 任务 1 开关 / 任务 3 假簇链）：独立开关 + 光栅档位 ──
            // 面板是 §14.4 三层的第三层：改动即写回 `NaniteSettings`（唯一真值），
            // 下一帧的帧图门控就会读到新值。任务 3 下开启的效果是帧图里多 `Nanite_Cull`
            // 与 `Nanite_Raster` 两个 pass（渲染到模块自建的 1×1 目标 ⇒ 可见画面不变）。
            if (dp) {
                ImGui::SeparatorText("Nanite（虚拟几何）");
                auto naniteSettings = dp->GetNaniteSettings();
                bool naniteOn = naniteSettings.enabled;
                if (ImGui::Checkbox("启用 Nanite##nanite", &naniteOn)) {
                    naniteSettings.enabled = naniteOn;
                    dp->SetNaniteSettings(naniteSettings);
                    HE_CORE_INFO("[Nanite] 面板开关: enabled={}", naniteSettings.enabled ? 1 : 0);
                }
                static const char* naniteRasterNames[] = {"软光栅", "混合光栅"};
                int naniteRasterMode = (int)naniteSettings.rasterMode;
                if (ImGui::Combo("光栅档位##nanite", &naniteRasterMode, naniteRasterNames, 2)) {
                    naniteSettings.rasterMode = (render::NaniteRasterMode)naniteRasterMode;
                    dp->SetNaniteSettings(naniteSettings);
                    HE_CORE_INFO("[Nanite] 面板档位: rasterMode={}", naniteRasterMode);
                }
                // 假簇数量（任务 3 的验收输入）：面板只读显示，自动化用 cfg 键
                // `nanite_fake_clusters`（或 CVar `r.Nanite.FakeClusters`）设置。
                ImGui::TextDisabled("任务 3 假簇数 N=%u（每帧恰好应画 N 次）",
                                    naniteSettings.fakeClusters);
                // 任务 4 的 UAV 自证开关（默认关）：勾上后模块在 GBuffer 之后追加
                // `Nanite_TestWrite`，用 compute 往 albedo 写 8×8 棋盘 —— 这是 A1 路线
                // （"模块直接写既有 GBuffer"）的自证通道，会**改变画面**，故默认关闭。
                bool naniteTestWrite = naniteSettings.testWrite;
                if (ImGui::Checkbox("UAV 自证：往 GBuffer albedo 写棋盘（任务 4）##nanite_tw",
                                    &naniteTestWrite)) {
                    naniteSettings.testWrite = naniteTestWrite;
                    dp->SetNaniteSettings(naniteSettings);
                    HE_CORE_INFO("[Nanite] 面板 UAV 自证开关: test_write={}",
                                 naniteSettings.testWrite ? 1 : 0);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("任务 4：compute 直接写既有 GBuffer albedo 的 UAV；\n"
                                      "默认关（会改变画面，仅供 A1 裁决取证）");
                ImGui::TextDisabled("模块就绪=%s（任务 3：Nanite_Cull + Nanite_Raster，画面不变）",
                                    dp->GetNanite().IsReady() ? "是" : "否");
                // ── 任务 6 的 mesh PSO 自证开关（默认关）──
                // 勾上后模块追加 `Nanite_MeshTest`：用 `PipelineStateDesc::meshShader` 建一条
                // 最小 mesh 管线（真正输出 4 顶点 / 2 图元），只画模块自建的 1×1 R8 小目标
                // ⇒ 即使打开也**不改动可见画面**，dump 帧会多打印一行 mesh_pso/meshlet_outputs/target_max。
                bool naniteMeshTest = naniteSettings.meshTest;
                if (ImGui::Checkbox("mesh PSO 自证：任务 6 mesh PSO 通道##nanite_mt", &naniteMeshTest)) {
                    naniteSettings.meshTest = naniteMeshTest;
                    dp->SetNaniteSettings(naniteSettings);
                    HE_CORE_INFO("[Nanite] 面板 mesh PSO 自证开关: mesh_test={}",
                                 naniteSettings.meshTest ? 1 : 0);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("任务 6 mesh PSO 自证开关，默认关；\n"
                                      "开启后多一个 Nanite_MeshTest pass（写模块自建 1×1 目标，画面不变）");
            }

            // ── GI 通道：Diffuse / Specular / AO / Shadow ──
            ImGui::SeparatorText("GI 通道");
            const ImVec4 colOk  = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
            const ImVec4 colBad = ImVec4(1.0f, 0.55f, 0.2f, 1.0f);

            // 本管线的能力位里一个 GI 源都没有（当前只有 Forward 是这样）：它的 IBL/RSM 由
            // 管线级开关驱动、不读层栈，因此下面整段层栈 UI 对它是**死开关**。与其把死开关
            // 摆出来，不如直说（§9.2-H）。阴影通道独立于层栈，仍然有效。
            const bool anyGISource = (giCaps & render::PipelineCaps::AllSources) != 0u;
            if (!anyGISource) {
                ImGui::TextColored(colBad,
                    "本管线的 GI 不由层栈驱动（IBL/RSM 是管线级开关），层栈内容对它没有影响");
                // 层栈 UI 全部置灰但不隐藏：内容仍可查看（便于对照诊断），但不可编辑，
                // 免得用户以为改了这里就能改变 Forward 的画面。
                ImGui::BeginDisabled();
            }

            // ── 通道 UI 辅助：显示该通道的「源层栈」（源 / 频段 / 权重 / 让位距离）──
            // 勾选 = 该源参与合成（weight>0）；多个源同时勾选即为「融合」
            auto channelUI = [&](const char* label, render::GIChannelStack& st,
                                 const render::GISourceId* candidates, int candCount) {
                ImGui::TextUnformatted(label);
                ImGui::Indent(12.0f);
                for (int i = 0; i < candCount; i++) {
                    const render::GISourceId id = candidates[i];
                    const bool  avail  = render::GIRegistry::IsAvailable(id, giCaps, rtOk);
                    const float weight = st.WeightOf(id);
                    bool active = weight > 0.0f;
                    ImGui::PushID((int)id);
                    if (ImGui::Checkbox(render::GISourceName(id), &active)) {
                        st.Set(id, active ? 1.0f : 0.0f, st.FalloffOf(id));
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(avail ? colOk : colBad, "[%s]",
                        avail ? render::GISourceClassName(id) : "不可用");
                    if (active) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(80.0f);
                        float w = st.WeightOf(id);
                        if (ImGui::DragFloat("权重", &w, 0.05f, 0.0f, 2.0f, "%.2f")) {
                            st.Set(id, w, st.FalloffOf(id));
                        }
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(80.0f);
                        float fo = st.FalloffOf(id);
                        if (ImGui::DragFloat("让位", &fo, 0.5f, 0.0f, 200.0f, "%.0f")) {
                            st.Set(id, w, fo);
                        }
                    }
                    ImGui::PopID();
                }
                // 该通道的合成方式（相加仅作对照；归一化保证无双重计数）
                int mode = (int)st.mode;
                const char* modes[] = {"相加(对照)", "归一化加权"};
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::Combo("合成", &mode, modes, 2)) {
                    st.mode = (render::GIBlendMode)mode;
                }
                ImGui::Unindent(12.0f);
            };

            // ---- Diffuse（间接漫反射）----
            {
                // P4：候选源从已注册的 Provider 派生（低频 → 高频排列）。
                // 频段顺序用于面板可读性；未接入 Provider 的源（如预留的 Lightmap）不出现。
                static const render::GISourceId kAllDiffuse[] = {
                    render::GISourceId::IBL, render::GISourceId::Lightmap, render::GISourceId::DDGI,
                    render::GISourceId::SSGI, render::GISourceId::RSM, render::GISourceId::RTGI,
                    render::GISourceId::Lumen };
                std::vector<render::GISourceId> diffuseSources;
                if (dp) {
                    for (auto id : kAllDiffuse) {
                        // 只列出「本管线能力位允许」的源：能力位就是层栈模型下管线能承载的范围，
                        // 列出一个它根本不消费的源，等于让面板把一个假开关摆给用户（§9.2-H）。
                        if (!render::GIRegistry::IsAvailable(id, giCaps, rtOk)) continue;
                        for (auto& p : dp->GetGIProviders()) {
                            if (p->Handles(id)) { diffuseSources.push_back(id); break; }
                        }
                    }
                }
                if (diffuseSources.empty()) diffuseSources.push_back(render::GISourceId::IBL);
                channelUI("Diffuse — 间接漫反射（低频 → 高频）", gc.diffuse,
                          diffuseSources.data(), (int)diffuseSources.size());
                ImGui::Indent(12.0f);
                ImGui::SliderFloat("GI 强度", &gc.giIntensity, 0.0f, 2.0f, "%.2f");
                ImGui::Checkbox("半分辨率", &gc.halfRes);
                if (giSSGI) {
                    bool on = giSSGI->IsEnabled();
                    if (ImGui::Checkbox("启用 SSGI", &on)) giSSGI->SetEnabled(on);
                    if (on) {
                        ImGui::Indent(12.0f);
                        ImGui::DragFloat("采样半径##ssgi", &giSSGI->radius, 0.1f, 0.1f, 5.0f, "%.1f");
                        ImGui::SliderInt("采样数##ssgi", &giSSGI->sampleCount, 4, 64);
                        ImGui::Unindent(12.0f);
                    }
                    ImGui::Text("SSGI 耗时 %.2f ms", giSSGI->GetDebugData().avgRenderTimeMs);
                }
                if (giDDGI) {
                    bool on = giDDGI->IsEnabled();
                    if (ImGui::Checkbox("启用 DDGI", &on)) giDDGI->SetEnabled(on);
                    if (on) {
                        ImGui::Indent(12.0f);
                        ImGui::SliderFloat("时间混合##ddgi", &giDDGI->blendAlpha, 0.0f, 0.98f, "%.2f");
                        ImGui::SliderFloat("贡献缩放##ddgi", &giDDGI->debugScale, 0.0f, 2.0f, "%.2f");
                        u32 pc = giDDGI->gridX * giDDGI->gridY * giDDGI->gridZ;
                        ImGui::Text("%u 探针 (%u×%u×%u)", pc, giDDGI->gridX, giDDGI->gridY, giDDGI->gridZ);
                        ImGui::Unindent(12.0f);
                    }
                    ImGui::Text("DDGI 耗时 %.2f ms", giDDGI->GetDebugData().avgRenderTimeMs);
                }
                // IBL 环境光（所有管线共用）
                if (auto* gi = curPipeline->GetGI()) {
                    auto s = gi->GetSettings();
                    float inten = s.intensity;
                    if (ImGui::SliderFloat("IBL 强度", &inten, 0.0f, 3.0f, "%.2f")) {
                        s.intensity = inten;
                        gi->SetSettings(s);
                    }
                    ImGui::Text("IBL 耗时 %.2f ms", gi->GetDebugData().avgRenderTimeMs);
                }
                ImGui::Unindent(12.0f);
            }

            // ---- Specular（镜面反射）----
            {
                // P4：候选源从已注册的 Provider 派生
                static const render::GISourceId kAllSpecular[] = {
                    render::GISourceId::IBL, render::GISourceId::SSR,
                    render::GISourceId::RTReflection, render::GISourceId::Lumen };
                std::vector<render::GISourceId> specSources;
                if (dp) {
                    for (auto id : kAllSpecular) {
                        for (auto& p : dp->GetGIProviders()) {
                            if (p->Handles(id)) { specSources.push_back(id); break; }
                        }
                    }
                }
                if (specSources.empty()) specSources.push_back(render::GISourceId::IBL);
                channelUI("Specular — 镜面反射（低频 → 高频）", gc.specular,
                          specSources.data(), (int)specSources.size());
                ImGui::Indent(12.0f);
                if (giSSR) {
                    bool on = giSSR->IsEnabled();
                    if (ImGui::Checkbox("启用 SSR", &on)) giSSR->SetEnabled(on);
                    if (on) {
                        ImGui::Indent(12.0f);
                        ImGui::SliderFloat("最大步数##ssr", &giSSR->maxSteps, 16.0f, 256.0f, "%.0f");
                        ImGui::SliderFloat("步长##ssr", &giSSR->stepSize, 0.1f, 2.0f, "%.1f");
                        ImGui::Unindent(12.0f);
                    }
                    ImGui::Text("SSR 耗时 %.2f ms", giSSR->GetDebugData().avgRenderTimeMs);
                }
                ImGui::Unindent(12.0f);
            }

            // ---- AO（环境光遮蔽）----
            {
                // P4：候选源从已注册的 GI Provider 派生（不再硬编码源列表）。
                // 目前 SSAO/GTAO 已 Provider 化；RTAO 尚未（留待下一波次推广）。
                std::vector<render::GISourceId> aoSources;
                if (dp) {
                    for (auto& p : dp->GetGIProviders()) {
                        if (p->Handles(render::GISourceId::SSAO)) {
                            aoSources.push_back(render::GISourceId::SSAO);
                            aoSources.push_back(render::GISourceId::GTAO);
                        }
                    }
                }
                if (aoSources.empty()) aoSources.push_back(render::GISourceId::SSAO);
                aoSources.push_back(render::GISourceId::RTAO);   // 尚未 Provider 化
                channelUI("AO — 环境光遮蔽", gc.ao, aoSources.data(), (int)aoSources.size());
                ImGui::Indent(12.0f);
                ImGui::SliderFloat("AO 强度##gc", &gc.aoIntensity, 0.0f, 1.5f, "%.2f");
                if (dp) {
                    auto& ssao = dp->GetSSAO();
                    bool on = ssao.enabled;
                    if (ImGui::Checkbox("启用 SSAO", &on)) ssao.enabled = on;
                    if (on) {
                        ImGui::Indent(12.0f);
                        ImGui::DragFloat("半径##ssao", &ssao.radius, 0.05f, 0.1f, 5.0f, "%.2f");
                        ImGui::SliderInt("采样数##ssao", &ssao.sampleCount, 4, 32);
                        ImGui::Checkbox("半分辨率##ssao", &ssao.halfRes);
                        ImGui::Unindent(12.0f);
                    }
                }
                ImGui::Unindent(12.0f);
            }

            // 结束「层栈 UI 置灰」（见上：本管线没有层栈 GI 源时）
            if (!anyGISource) ImGui::EndDisabled();

            // ---- Shadow（阴影）----
            // 阴影是「可见性（乘法项）」而非「能量（加法项）」——不适用层栈的
            // 频段/权重/距离让位语义，故用独立枚举：None / Raster / RT
            {
                const char* shadowNames[] = {"None", "Raster（光栅 CSM 等）", "RT（光追阴影）"};
                int sh = (int)gc.shadow;
                ImGui::TextUnformatted("Shadow — 阴影（可见性，不参与能量合成）");
                ImGui::Indent(12.0f);
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::Combo("##shadow", &sh, shadowNames, 3)) {
                    gc.shadow = (render::ShadowChannel)sh;
                    gc = render::GIRegistry::Degrade(gc, giCaps, rtOk);   // 不可用则回退
                }
                const bool shOk = render::GIRegistry::IsAvailable(gc.shadow, giCaps, rtOk)
                               || gc.shadow == render::ShadowChannel::None;
                ImGui::SameLine();
                ImGui::TextColored(shOk ? colOk : colBad, shOk ? "[可用]" : "[不可用]");
                ImGui::Unindent(12.0f);
            }

            // ---- 配置诊断（P0 · REDUNDANCY）----
            // 本架构要求每个已启用的源都**整幅、每帧**产出完整通道缓冲（文档 §3.5），
            // 故「启用一个没有增益的源」= 白付一份全量成本。这里把这类配置显式列出来。
            // 只做提示与可证等价的去重，不改变渲染语义。
            {
                const std::vector<render::GIDiagnostic> diags = render::GIRegistry::Analyze(gc);

                // 诊断配色（本面板此前只定义了 colOk / colBad）
                const ImVec4 colWarn = ImVec4(1.0f, 0.75f, 0.25f, 1.0f);   // 提示：可优化但不影响正确性
                const ImVec4 colDim  = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);  // 信息：性能提示

                // 先统计各类别次数（连按钮的可见性判断也要用，故放在 if/else 之外）
                u32 nRedundant = 0, nDupEst = 0, nCorr = 0, nCost = 0;
                for (const auto& e : diags) {
                    switch (e.kind) {
                    case render::GIDiagnosticKind::RedundantDuplicate:  nRedundant++; break;
                    case render::GIDiagnosticKind::DuplicateEstimate:   nDupEst++;    break;
                    case render::GIDiagnosticKind::CorrelatedEstimates: nCorr++;      break;
                    case render::GIDiagnosticKind::MultiSourceCost:     nCost++;      break;
                    default: break;
                    }
                }

                ImGui::SeparatorText("配置诊断");
                if (diags.empty()) {
                    ImGui::TextColored(colOk, "无问题：当前层栈无冗余或重复估计");
                } else {
                    if (nRedundant > 0) {
                        ImGui::TextColored(colBad, "严格冗余 ×%u（零增益，可安全去重）", nRedundant);
                    }
                    if (nDupEst > 0) {
                        ImGui::TextColored(colWarn, "重复估计 ×%u（成本翻倍、归一化互相稀释）", nDupEst);
                    }
                    if (nCorr > 0) {
                        ImGui::TextColored(colWarn, "相关估计 ×%u（非独立，归一化失去无偏性）", nCorr);
                    }
                    if (nCost > 0) {
                        ImGui::TextColored(colDim, "多源通道 ×%u（每源各跑一遍整幅 pass）", nCost);
                    }

                    ImGui::Indent(12.0f);
                    for (const auto& e : diags) {
                        const bool bad  = (e.kind == render::GIDiagnosticKind::RedundantDuplicate);
                        const bool warn = (e.kind == render::GIDiagnosticKind::DuplicateEstimate ||
                                           e.kind == render::GIDiagnosticKind::CorrelatedEstimates);
                        const ImVec4 col = bad ? colBad : (warn ? colWarn : colDim);
                        ImGui::TextColored(col, "[%s] %s: %s%s%s",
                            render::GIDiagnosticKindName(e.kind),
                            render::GIChannelName(e.channel),
                            e.a != render::GISourceId::None ? render::GISourceName(e.a) : "",
                            e.b != render::GISourceId::None ? " + " : "",
                            e.b != render::GISourceId::None ? render::GISourceName(e.b) : "");
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.detail);
                    }
                    ImGui::Unindent(12.0f);
                }

                // 只在确有严格冗余时才提供一键去重
                if (nRedundant > 0) {
                    if (ImGui::Button("去除严格冗余（保留 GTAO）")) {
                        render::GIRegistry::DeduplicateRedundant(gc);
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(colDim, "可证与去重前逐像素等价");
                }
            }
        }
        ImGui::End();

        // ============================================================
        // 独立 GPU Profiler 面板：各 pass 的 GPU 耗时统计
        // ============================================================
        // 首次使用时默认摆放在 GI 面板下方（之后由 imgui.ini 恢复）
        ImGui::SetNextWindowPos(ImVec2(560.0f, 720.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430.0f, 260.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("GPU Profiler");
        {
            auto& pdata = deferredPipeline.GetProfiler().GetLastFrameData();
            float totalMs = 0;
            for (auto& p : pdata) {
                if (p.gpuMs < 0) continue;  // 未使用
                totalMs += p.gpuMs;
                ImGui::Text("%-20s %6.2fms", p.name.c_str(), p.gpuMs);
            }
            ImGui::Separator();
            ImGui::Text("Total GPU: %.2fms (%.0f FPS)", totalMs, totalMs > 0 ? 1000.0f / totalMs : 0);
        }
        ImGui::End();

        // GPU Profiler 面板（按 F1 切换）
        if (ImGui::IsKeyPressed(ImGuiKey_F1))
            deferredPipeline.GetProfilerPanel().Toggle();
        deferredPipeline.GetProfilerPanel().Draw();

        imgui.EndFrame(cmdList.get());
        cmdList->EndRenderPass();

        // ── 白炉探针：把 HDR 目标上的两个像素拷进 host 可见缓冲 ──
        // 必须在 render pass 之外录制（拷贝不能在 pass 内），因此放在 EndRenderPass 之后、End 之前
        const bool probeThisFrame = g_ProbeEnabled && (frameIndex % (u64)std::max(1, g_ProbeInterval) == 0);
        if (probeThisFrame && probeBuffer) {
            // 白炉探针也要按**当前管线**取 HDR：Forward 有自己的一张（任务 26 起层栈归一化
            // 也在 Forward 生效，白炉判据必须能覆盖它）
            rhi::IRHITexture* hdr = (g_PipelineMode == 0)
                ? forwardPipeline.GetHDRTarget()
                : deferredPipeline.GetLighting().GetHDRTarget();
            if (hdr) {
                const u32 pw = swapchain->GetWidth(), ph = swapchain->GetHeight();
                const u32 cx = pw / 2,           cy = ph / 2;            // 中心（物体所在）
                const u32 bx = pw / 10,          by = ph / 10;           // 背景取样点
                // RGBA16_FLOAT：每像素 8 字节 → 第二个像素偏移 16 字节
                cmdList->CopyTextureToBuffer(hdr, probeBuffer.get(), cx, cy, 1, 1, 0);
                cmdList->CopyTextureToBuffer(hdr, probeBuffer.get(), bx, by, 1, 1, 16);
            }
        }

        // ── GI 频谱采样：把若干张纹理整幅拷进 host 可见缓冲（仅测试路径）──
        // 同样必须在 render pass 之外录制；缓冲在采样帧才创建，尺寸取自纹理本身
        // 目标分三类：
        //   hdr / albedo  —— 合成结果与接收端反照率（做差 + 除 albedo 后即得 E/π）
        //   provN_raw/final —— 各有效 Provider 的原始输出 / 降噪后输出（定位"某源为 0"发生在哪一级）
        if (g_DumpGI && !g_DumpDone && frameIndex >= g_DumpFrame) {
            auto addTarget = [&](const String& name, rhi::IRHITexture* tex) {
                if (!tex) return;
                DumpTarget t;
                t.name = name;
                t.tex  = tex;
                t.w    = tex->GetWidth();
                t.h    = tex->GetHeight();
                rhi::BufferDesc dd;                             // 宽×高×8 B（RGBA16F）
                dd.size      = (usize)t.w * t.h * 8;
                dd.usage     = rhi::BufferUsage::Storage;       // 该路径恒定带 TRANSFER_DST，可作拷贝目标
                dd.cpuAccess = true;                            // 需要 Map 读回
                t.buf = device->CreateBuffer(dd);
                if (!t.buf) { HE_CORE_ERROR("[GI采样] 读回缓冲创建失败: {}（{}x{}）", name, t.w, t.h); return; }
                // x=y=0 且取满宽高 ⇒ bufferRowLength=0 的紧密排布正好等于线性落盘布局
                cmdList->CopyTextureToBuffer(tex, t.buf.get(), 0, 0, t.w, t.h, 0);
                g_DumpTargets.push_back(std::move(t));
            };
            // HDR 取**当前管线**的那张（任务 26）：Forward 没有 GBuffer / Provider 输出，
            // 但它的 HDR 目标就是层栈归一化的产物 —— 这正是 Forward 侧唯一可读的判据出口。
            // 此前这里无条件取 deferredPipeline 的 HDR，于是 `pipeline_mode=0` 下落盘的
            // 根本不是 Forward 的画面（文档 §11.3 早就把这点写成了注意事项，任务 26 修掉）。
            const bool forwardMode = (g_PipelineMode == 0);
            addTarget("hdr", forwardMode ? forwardPipeline.GetHDRTarget()
                                         : deferredPipeline.GetLighting().GetHDRTarget());
            if (!forwardMode) {
            if (auto* gb = deferredPipeline.GetGBuffer()) {
                addTarget("albedo", gb->GetAlbedo());
                // GBuffer 的世界坐标/法线：屏幕空间 GI pass 的**输入**。少了它们，
                // "某个源的输出纹理对不对"就只能靠形状相关性猜；有了它们才能把那个 pass
                // 的公式在 CPU 上原样重算一遍做逐像素对照（任务 30 就是这么定位 RSM 链路的）。
                addTarget("gb_worldpos", gb->GetWorldPos());
                addTarget("gb_normal",   gb->GetNormal());
                // 光照图键（任务 31）：MRT7 = (uv0.x, uv0.y, objectIndex, 0)。烘焙光照图的
                // 前置条件就是"逐像素能反查页号与页内坐标"，这个转储是那条性质的唯一直接证据。
                addTarget("gb_lightmapkey", gb->GetLightmapKey());
            }
            // 共享的前帧 HDR 辐射度（DDGI 探针 / SSGI 入射辐射度的共同输入）：
            // 它是 GI 源吃进去的东西，出问题时第一个要看的中间量
            addTarget("radiance", deferredPipeline.GetRadianceHistory().GetTexture());
            // IBL 辐照度：DDGI 探针更新的**唯一**辐射度回退来源（GI_DDGI::SetIBL）。
            // 若它为空/未绑定，DDGI 会静默退化为 DDGI.comp.slang 的硬编码兜底常数。
            if (auto* giIBL = dynamic_cast<render::GI_IBL*>(deferredPipeline.GetGI()))
                addTarget("ibl_irr", giIBL->GetIrradianceMap());
            const auto& providers = deferredPipeline.GetGIProviders();
            for (size_t i = 0; i < providers.size(); ++i) {
                auto* p = providers[i].get();
                // 【步骤 35】判据从 `IsValid()`（"pass 对象在"）换成 `ProducedThisFrame()`（"本帧真的跑了"）：
                // Lumen/RTAO 这些源即使没进任何层栈，`IsValid()` 也为真，于是 `provN_*` 会落到
                // 上一帧或从未使用的纹理上 —— 转储看起来"有内容"，实际是假读数。
                if (!p || !p->ProducedThisFrame()) continue;
                const String pre = "prov" + std::to_string(i) + "_";
                addTarget(pre + "raw",   p->GetDiffuseOutput());
                addTarget(pre + "final", p->GetFinalDiffuseOutput());
                // 镜面通道与 AO 通道的输出也必须能落盘：只看得见漫反射输出的话，
                // 「SSR/SSAO 是否真的产出了东西」就无从做纹理级对照（§9.2-B/C/E 都属这一类）。
                // 名称带通道后缀，避免与漫反射的 raw/final 混淆；不存在该通道输出时自动跳过。
                addTarget(pre + "spec_raw",   p->GetSpecularOutput());
                addTarget(pre + "spec_final", p->GetFinalSpecularOutput());
                addTarget(pre + "ao_raw",     p->GetAOOutput());
                addTarget(pre + "ao_final",   p->GetFinalAOOutput());
                // 步骤 12（L1 退出判据）：逐像素 SDF 追踪可视化。名字**稳定**（不依赖注册顺序），
                // 因为它是后面所有阶段（Screen Probe / Surface Cache）的公共"几何是否靠谱"凭据。
                if (auto* lp = dynamic_cast<render::LumenProvider*>(p)) {
                    addTarget("lumen_sdf_trace", lp->GetSDFDebugTexture());
                    // 步骤 37：把"逐像素入射辐照度"也落盘。它是 Lumen 的中间层，此前只有 CPU 侧
                    // 统计（均值/覆盖）可看；当"统计正常但输出为黑"时，必须能直接看到这张纹理本身
                    // 到底有没有内容，否则只能在"没画"和"画了但采样到空"之间猜。
                    addTarget("lumen_irradiance", lp->GetIrradianceTexture());
                    // 步骤 13（L2 的输入）：卡片覆盖率可视化（上半 = 代表 mesh 的 6 个投影面，下半 = 逐 mesh 覆盖条）
                    addTarget("lumen_card_coverage", lp->GetCardCoverageTexture());
                    addTarget("lumen_sc_atlas_albedo", lp->GetCardAtlasAlbedo());   // 步骤 15：Card 捕获的 albedo atlas
                }
            }
            }   // if (!forwardMode)
            // RSM 链路的逐级中间量（任务 30）：位置 / 编码法线 / VPL 辐射度 / 间接光输出。
            // 该链路的典型失效是"pass 在跑、成本在付、画面里什么都没有"（§9.2-AA），
            // 只看最终 HDR 无法分辨是"没产出"还是"产出被合成丢掉"——所以四级都落盘。
            // 两条管线各有自己的 GI_RSM 实例（同样是"同一个着色器、各自的光源视锥"），
            // 故这里按当前管线取，才能对照两个视锥各自的产出。
            if (auto* rsm = forwardMode ? forwardPipeline.GetRSM() : deferredPipeline.GetRSM()) {
                addTarget("rsm_pos", rsm->GetRSMPositionMap());
                addTarget("rsm_nrm", rsm->GetRSMFluxMap());
                addTarget("rsm_rad", rsm->GetRSMRadianceMap());
            }
            if (!forwardMode) addTarget("rsm_indirect", deferredPipeline.GetRSMIndirect().GetOutput());
            // SSR 的输出单独给一个稳定名字（任务 32）：它的 provider 输出名是 `provN_spec_raw`，
            // 而 N 取决于哪些 provider 有效 —— 判据不该依赖注册顺序。
            if (!forwardMode) {
                if (auto* ssr = deferredPipeline.GetSSR())
                    addTarget("ssr", ssr->GetIndirectSpecularTexture());
            }
            if (!g_DumpTargets.empty()) {
                g_DumpDone = true;   // 已录制；实际读取放在 Submit 之后
            } else {
                g_DumpGI = false;
                HE_CORE_WARN("[GI采样] 没有可用目标（HDR 未创建？），本次跳过");
            }
        }

        cmdList->End();

        device->Submit(cmdList.get());
        deferredPipeline.FlushComputeWork();  // AsyncCompute: Graphics Submit 之后提交 Compute 工作

        // ── 白炉探针：等 GPU 完成后读回（half → float），算亮度比并打日志 ──
        if (probeThisFrame && probeBuffer) {
            device->WaitIdle();   // 测试用途，允许停顿
            const auto* texels = static_cast<const uint16_t*>(probeBuffer->Map());
            if (texels) {
                for (int i = 0; i < 3; ++i) {
                    g_ProbeCenter[i] = glm::unpackHalf1x16(texels[i]);          // 像素 0：中心
                    g_ProbeBg[i]     = glm::unpackHalf1x16(texels[8 + i]);      // 像素 1：背景（16B 偏移 = 8 个 half）
                }
                const float lumCenter = 0.2126f * g_ProbeCenter[0] + 0.7152f * g_ProbeCenter[1] + 0.0722f * g_ProbeCenter[2];
                const float lumBg     = 0.2126f * g_ProbeBg[0]     + 0.7152f * g_ProbeBg[1]     + 0.0722f * g_ProbeBg[2];
                g_ProbeLumCenter = lumCenter;
                g_ProbeRatio = (lumBg > 1e-6f) ? (lumCenter / lumBg) : 0.0f;
                HE_CORE_INFO("[白炉探针] 中心 RGB=({:.4f},{:.4f},{:.4f}) 背景 RGB=({:.4f},{:.4f},{:.4f}) "
                             "亮度 中心={:.4f} 背景={:.4f} 比值={:.4f} 白炉={}",
                             g_ProbeCenter[0], g_ProbeCenter[1], g_ProbeCenter[2],
                             g_ProbeBg[0], g_ProbeBg[1], g_ProbeBg[2],
                             lumCenter, lumBg, g_ProbeRatio,
                             g_FurnaceMode ? "ON" : "off");
                probeBuffer->Unmap();
            }
        }

        // ── GI 频谱采样：等 GPU 完成后原样落盘（RGBA16F 原始像素、无文件头、行紧密排布）──
        if (g_DumpDone && !g_DumpWritten) {
            device->WaitIdle();   // 测试用途，允许停顿

            // ── Nanite（§14.8 任务 3）：dump 帧打印**恰好一行**真实 GPU 读回 ──
            // 同步已在上一行的 `WaitIdle()` 完成（与白炉探针/落盘同一套做法，不新造同步机制）；
            // 关闭档下模块自身会直接返回（不打印），保证关闭档日志与基线一致。
            if (auto* dpNanite = dynamic_cast<render::DeferredPipeline*>(curPipeline))
                dpNanite->GetNanite().LogFakePipelineReadback();
            // ── Nanite（§14.8 任务 13）：dump 帧打印**恰好一行**实例剔除的 GPU/CPU 逐项对照 ──
            // 同步同样依赖上一行的 `WaitIdle()`；关闭档下模块内部直接返回、不打印。
            if (auto* dpNanite = dynamic_cast<render::DeferredPipeline*>(curPipeline))
                dpNanite->GetNanite().LogInstanceCullReadback();
            // ── Nanite（§14.8 任务 14）：dump 帧打印**恰好一行** cluster BVH 的
            //    节点数/深度 + GPU 与 CPU 参考的"访问节点数 / 可见簇数 / 逐项差异" ──
            // 同步同样依赖上面的 `WaitIdle()`；关闭档下模块内部直接返回、不打印。
            if (auto* dpNanite = dynamic_cast<render::DeferredPipeline*>(curPipeline))
                dpNanite->GetNanite().LogClusterBVHReadback();
            // ── Nanite（§14.8 任务 6）：mesh PSO 通道的**恰好一行**真实 GPU 读回 ──
            // 同步同样已在上一行的 `WaitIdle()` 完成；`nanite_mesh_test=0`（默认）时模块内部
            // 直接返回、不打印，因此不改变任何既有档位的日志。
            if (auto* dpNanite = dynamic_cast<render::DeferredPipeline*>(curPipeline))
                dpNanite->GetNanite().LogMeshTestReadback();

            const String dir  = "build/verify/";
            const String base = dir + "gi_" + g_DumpTag;
            std::filesystem::create_directories(dir);
            std::ofstream meta(base + "_meta.txt");
            for (auto& t : g_DumpTargets) {                        // 逐目标写：像素直落，无头
                const usize bytes = (usize)t.w * t.h * 8;
                const void* p = t.buf ? t.buf->Map() : nullptr;
                if (p) {
                    std::ofstream f(base + "_" + t.name + ".f16", std::ios::binary);
                    f.write(static_cast<const char*>(p), (std::streamsize)bytes);
                    t.buf->Unmap();
                    meta << t.name << " " << t.w << " " << t.h << " RGBA16F\n";
                    HE_CORE_INFO("[GI采样] {}_{}.f16  {}x{}  {} B", base, t.name, t.w, t.h, bytes);
                } else {
                    HE_CORE_ERROR("[GI采样] 映射失败: {}_{}", base, t.name);
                }
            }
            HE_CORE_INFO("[GI采样] 共落盘 {} 个目标，请求退出", g_DumpTargets.size());
            // 相机参数一并落盘：解析对照（平面镜的镜像点投影到屏幕）需要那套**渲染这一帧时**
            // 的相机参数，否则判据只能靠硬编码 —— 而硬编码的相机参数一旦被 cfg 改动就失效。
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
            }
            // 平面镜测试台的几何（任务 32）：与场景搭建同源，判据据此做解析镜像计算
            if (g_MirrorRig.valid) {
                std::ofstream mr(base + "_mirror.txt");
                mr << "plane "  << g_MirrorRig.planeNormal.x << " " << g_MirrorRig.planeNormal.y << " "
                                << g_MirrorRig.planeNormal.z << " " << g_MirrorRig.planeOffset << "\n";
                mr << "slab "   << g_MirrorRig.slabCenter.x << " " << g_MirrorRig.slabCenter.y << " "
                                << g_MirrorRig.slabCenter.z << " " << g_MirrorRig.slabHalf.x << " "
                                << g_MirrorRig.slabHalf.y << " " << g_MirrorRig.slabHalf.z << "\n";
                mr << "red "    << g_MirrorRig.boxRed.x << " " << g_MirrorRig.boxRed.y << " "
                                << g_MirrorRig.boxRed.z << " " << g_MirrorRig.halfRed << "\n";
                mr << "green "  << g_MirrorRig.boxGreen.x << " " << g_MirrorRig.boxGreen.y << " "
                                << g_MirrorRig.boxGreen.z << " " << g_MirrorRig.halfGreen << "\n";
            }
            g_DumpWritten = true;
            // 采样完成即请求关窗：让脚本无需超时等待，也保证退出前正常走完清理与保存流程
            glfwSetWindowShouldClose(engine.GetWindow()->GetNativeHandle(), GLFW_TRUE);
        }

        // 垂直同步必须与创建时一致：只改 SwapChainDesc 而这里仍传 true，帧率照样被锁在刷新率
        // （步骤 37 第一次测就是这么被误导的：pass 合计 14 ms 却只有 19 fps）。
        swapchain->Present(!noVsync);
        frameIndex++;

        // 【步骤 37 / L6 帧时判据】周期性打印**真实墙钟帧率**与 CPU 侧耗时。
        // 只在 `HE_NO_VSYNC=1` 时帧率才有判据意义（vsync 打开时它恒等于刷新率，会被误读成"达标"）。
        {
            const double cpuMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cpuT0).count();
            static double s_accMs = 0.0;
            static double s_accCpuMs = 0.0;
            static u32    s_n = 0;
            s_accMs += (double)deltaTime * 1000.0;
            s_accCpuMs += cpuMs;
            ++s_n;
            if (g_DumpGI && s_n >= 120u) {
                const double wallMs = s_accMs / (double)s_n;
                const double cm     = s_accCpuMs / (double)s_n;
                const double pm     = s_accPipelineMs / (double)s_n;
                // 步骤 37：管线 CPU 侧的三段分解（重建帧图 / 编译 / 执行）——判断该修哪一段
                double bMs = 0.0, cMs = 0.0, eMs = 0.0;
                if (auto* dp = dynamic_cast<render::DeferredPipeline*>(curPipeline)) {
                    bMs = dp->GetCpuBuildMs(); cMs = dp->GetCpuCompileMs(); eMs = dp->GetCpuExecMs();
                }
                HE_CORE_INFO("帧率读数（步骤 37）: 最近 {} 帧 墙钟 {:.3f} ms ⇒ {:.1f} fps；CPU 侧 {:.3f} ms"
                             "（{:.0f}% 的帧时，其中管线 Render {:.3f} ms / 其余 {:.3f} ms）⇒ 受限方 = {}"
                             "（vsync {}；pass 合计见【帧预算】行）",
                             s_n, wallMs, wallMs > 0.0 ? 1000.0 / wallMs : 0.0, cm,
                             wallMs > 0.0 ? 100.0 * cm / wallMs : 0.0, pm, cm - pm,
                             cm > wallMs * 0.8 ? "CPU" : "GPU/呈现",
                             noVsync ? "关" : "开（帧率被锁刷新率，不作判据）");
                HE_CORE_INFO("   管线 CPU 分解（上一帧）: 重建帧图 {:.3f} ms / 编译 {:.3f} ms / 执行(录制+提交) {:.3f} ms",
                             bMs, cMs, eMs);
                s_accMs = 0.0; s_accCpuMs = 0.0; s_accPipelineMs = 0.0;
                s_n = 0;
            }
        }
    }

    // 清理
    // 面板几何（位置/大小/折叠状态）显式落盘：ImGui 自动保存有 5 秒节流，
    // 拖动窗口后立即退出会丢失；必须在 imgui.Shutdown()（销毁上下文）之前调用
    if (ImGui::GetIO().IniFilename) {
        ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
        HE_CORE_INFO("面板布局已保存: {}", ImGui::GetIO().IniFilename);
    }
    imgui.Shutdown();
    device->WaitIdle();
    deferredPipeline.Shutdown();

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


        // ── 渲染设置 ──
        out["clustered"]    = std::to_string(deferredPipeline.GetClusteredShading().enabled ? 1 : 0);
        out["gpu_cull"]     = std::to_string(deferredPipeline.GetGPUCulling().enabled ? 1 : 0);
        out["gbuffer_mode"] = std::to_string((int)deferredPipeline.GetGBufferMode());
        // ── Nanite（§14.8 任务 1 开关 / 任务 3 假簇数）：cfg 回写（与 gi_half_res 同写法）──
        // 【为什么 Shutdown() 之后还能读】NaniteRenderer::Shutdown() 只释放资源、
        // 不重置开关真值（本段代码确实在 deferredPipeline.Shutdown() 之后执行）。
        out["nanite_enable"]        = std::to_string(deferredPipeline.GetNaniteSettings().enabled ? 1 : 0);
        out["nanite_fake_clusters"] = std::to_string(deferredPipeline.GetNaniteSettings().fakeClusters);
        // 任务 4：UAV 自证开关（默认 0）——同样在 Shutdown() 之后回写，故真值必须保留
        out["nanite_test_write"]    = std::to_string(deferredPipeline.GetNaniteSettings().testWrite ? 1 : 0);
        // 任务 6：mesh PSO 自证开关（默认 0）——同写法、同在 Shutdown() 之后回写
        out["nanite_mesh_test"]     = std::to_string(deferredPipeline.GetNaniteSettings().meshTest ? 1 : 0);
        // 任务 13：合成实例网格条数（默认 64）——同写法、同在 Shutdown() 之后回写
        out["nanite_instance_test_count"] =
            std::to_string(deferredPipeline.GetNaniteSettings().instanceTestCount);

        // ── AutoExposure ──
        auto& ae = deferredPipeline.GetAutoExposure();
        out["ae_enabled"]     = std::to_string(ae.IsEnabled() ? 1 : 0);
        out["ae_adapt_speed"] = std::to_string(ae.GetAdaptSpeed());
        out["ae_target_lum"]  = std::to_string(ae.GetTargetLum());


        // ── GI ──
        if (auto* gi = deferredPipeline.GetGI()) {
            out["ibl_intensity"] = std::to_string(gi->GetSettings().intensity);
        }
        if (auto* ssgi = deferredPipeline.GetSSGI()) {
            out["ssgi_radius"]    = std::to_string(ssgi->radius);
            out["ssgi_samples"]   = std::to_string(ssgi->sampleCount);
            out["ssgi_intensity"] = std::to_string(ssgi->GetSettings().intensity);
        }
        if (auto* ddgi = deferredPipeline.GetDDGI()) {
            out["ddgi_blend"]     = std::to_string(ddgi->blendAlpha);
            out["ddgi_scale"]     = std::to_string(ddgi->debugScale);
            out["ddgi_intensity"] = std::to_string(ddgi->GetSettings().intensity);
            out["ddgi_update_stride"] = std::to_string(ddgi->updateStride);
            out["ddgi_grid_auto"]     = std::to_string(ddgi->autoFitGrid ? 1 : 0);
            out["ddgi_fit_cells"]     = std::to_string(ddgi->fitCellsMax);
        }
        if (auto* ssr = deferredPipeline.GetSSR()) {
            out["ssr_max_steps"] = std::to_string(ssr->maxSteps);
            out["ssr_step_size"] = std::to_string(ssr->stepSize);
            out["ssr_use_hiz"]   = std::to_string(ssr->useHiZ ? 1 : 0);
        }

        // ── SSAO ──

        // ── 面板状态：管线 / GI 档位 / 只看 GI / GI 通道配置 ──
        out["pipeline_mode"] = std::to_string(g_PipelineMode);
        out["gi_solo"]       = std::to_string(g_GISolo ? 1 : 0);
        out["gi_preset"]     = std::to_string(g_GIPreset);
        {
            auto& gc = *deferredPipeline.GetGIConfig();
            out["gi_intensity"]    = std::to_string(gc.giIntensity);
            out["ao_intensity"]    = std::to_string(gc.aoIntensity);
            out["gi_edge_fade"]    = std::to_string(gc.edgeFade);
            out["gi_rsm_indirect"] = std::to_string(gc.rsmIndirect ? 1 : 0);
            out["gi_half_res"]     = std::to_string(gc.halfRes ? 1 : 0);

            // 层栈序列化：每通道按「源顺序」写出各源权重（0 = 不参与）
            auto saveStack = [&](const render::GIChannelStack& st, const char* key,
                                 int cap0, int cap1, int cap2, int cap3) {
                const int ids[4] = { cap0, cap1, cap2, cap3 };
                for (int i = 0; i < 4; i++) {
                    if (ids[i] < 0) continue;
                    out[String(key) + "_w" + std::to_string(i)] =
                        std::to_string(st.WeightOf((render::GISourceId)ids[i]));
                }
            };
            saveStack(gc.diffuse, "gi_blend_diffuse",
                      (int)render::GISourceId::IBL, (int)render::GISourceId::DDGI,
                      (int)render::GISourceId::SSGI, (int)render::GISourceId::RTGI);
            saveStack(gc.specular, "gi_blend_specular",
                      (int)render::GISourceId::IBL, (int)render::GISourceId::SSR,
                      (int)render::GISourceId::RTReflection, (int)-1);
            saveStack(gc.ao, "gi_blend_ao",
                      (int)render::GISourceId::SSAO, (int)render::GISourceId::RTAO, (int)-1, (int)-1);
            // RSM 权重单独序列化（与上面的加载对应；它不在 4 个固定槽位里）
            out["gi_blend_diffuse_rsm"] =
                std::to_string(gc.diffuse.WeightOf(render::GISourceId::RSM));
            // Lumen 同理（同时服务漫反射与镜面，故两个通道各一个键）
            out["gi_blend_diffuse_lumen"] =
                std::to_string(gc.diffuse.WeightOf(render::GISourceId::Lumen));
            out["gi_blend_specular_lumen"] =
                std::to_string(gc.specular.WeightOf(render::GISourceId::Lumen));
            // 阴影通道独立于层栈 → 按枚举序列化
            out["gi_shadow"] = std::to_string((int)gc.shadow);
        }
        {
            auto& ssao = deferredPipeline.GetSSAO();
            out["ssao_radius"]   = std::to_string(ssao.radius);
            out["ssao_samples"]  = std::to_string(ssao.sampleCount);
            out["ssao_half_res"] = std::to_string(ssao.halfRes ? 1 : 0);
        }
        out["cam_move_speed"] = std::to_string(camCtrl.GetMoveSpeed());

        SaveConfigFile(g_ConfigPath, out);
        HE_CORE_INFO("配置已保存: {}", g_ConfigPath);
    }

    HE_CORE_INFO("07.Nanite 退出 ({} 帧)", frameIndex);
    return 0;
}
