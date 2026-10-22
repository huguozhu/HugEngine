// ============================================================
// 06.GILab — Cornell Box 场景 + 延迟渲染管线（GI 对比 / 开发实验室）
//
// 用途：检验与对比各种 GI（IBL/RSM/SSGI/DDGI/SSR）在同一 Cornell Box 场景下的效果，
//      并作为新 GI（实现 IGlobalIllumination 接口）的开发/接入试验场。
// 使用 DeferredPipeline（GBuffer + 全屏 Lighting Pass）。
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "RHI/FramePresenter.h"   // L4-1：帧外壳（acquire/present）收口
#include "Debug/FrameDumpRecorder.h"
#include "Debug/PixelProbe.h"	// 白炉探针（像素数值读回）
#include "CameraInput.h"   		// 样例共用：鼠标/键盘控制相机
#include "RHI/ThreadAffinity.h"   // T2.2：渲染线程启动钩子里认领 RHI 归属（GetThreadAffinity）
#include "Pipeline/DeferredPipeline.h"
#include "Pipeline/ForwardPipeline.h"
#include "Pipeline/IRenderPipeline.h"
#include "GI/GITypes.h"   // GI 数据模型 + GIRegistry（RHI-free）
#include "GI/LumenProvider.h"   // 步骤 12：取 SDF 追踪可视化纹理做转储
// 渲染线程化（方案 B 阶段 0 T0.3）：命令队列 + 渲染线程壳 + 帧票据/背压。
// 阶段 0 的壳在**调用线程**上执行（帧序与结果不变），阶段 2 起同一段代码由真线程消费。
#include "Threading/RenderCommandQueue.h"
#include "Threading/RenderThread.h"
#include "Threading/RHIThread.h"      // 模式 2：RHI 线程（唯一提交者）
#include "Lumen/ScreenProbe.slang"   // 2026-09-24 诊断：探针缓冲落盘需要 sizeof(ScreenProbe)
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


#include <algorithm>
#include <cstdlib>   // std::getenv / std::atoi（环境变量开关）
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// stb_image — 纹理解码
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

using namespace he;

// ---- 配置读写（简易 key=value 格式） ----
// 默认读写内容目录下的正式配置；自动化实验可用 HE_GILAB_CONFIG=<路径> 指向临时文件
// （读写同一路径，因此指向临时文件即完全不触碰仓库内的正式配置）
static String g_ConfigPath = []{
    if (const char* p = std::getenv("HE_GILAB_CONFIG")) return String(p);
    return String(HUGE_CONTENT_DIR) + "Config/06_GILab.cfg";
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

// ---- 创建形状实体（立方体/球）——Cornell Box 场景构件 ----
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

// ============================================================
// 天空盒：从等距柱状 HDR 生成 6 面立方体贴图，并注册为场景实体
// 原先内联在 main() 里（79 行）—— 抽出来让 main 只保留流程骨架，也便于其它样例复用。
// ============================================================
static void CreateSkybox(World& world, SceneGraph& sceneGraph, rhi::IRHIDevice* device,
                         const String& hdrPath) {
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
}

// ============================================================
// TextureCache —— glTF 纹理 → RHI Texture/Sampler 的加载、去重、默认占位
//
// 原先内联在 main() 里：一个 g_TexCache 表 + 一个 loadTexture lambda（39 行）+ 一段 1x1 默认
// 占位纹理的创建（19 行）。抽出来后 main 只剩"取纹理"的调用，纹理参数（RGBA8、mip 数、
// Linear/Repeat、TransferSrc|TransferDst）也只有一处定义。
//
// 【去重】按**纹理全路径**缓存：同一张图被多个材质引用时只创建一次。
// 【默认占位】bindless heap 必须在注册材质纹理**之前**拿到默认纹理，否则未贴图的材质槽无效。
// ============================================================
class TextureCache {
public:
    using Entry = std::pair<std::unique_ptr<rhi::IRHITexture>, std::unique_ptr<rhi::IRHISampler>>;

    explicit TextureCache(rhi::IRHIDevice* device) : m_Device(device) {}

    /// 创建 1x1 白色默认纹理并登记到 bindless heap；必须在注册材质纹理之前调用。
    void CreateAndBindDefault();

    /// 已缓存条目数（含默认占位纹理），仅用于日志。
    [[nodiscard]] usize Count() const { return m_Cache.size(); }

    /// 加载（或命中缓存返回）纹理与采样器；baseDir 为 glTF 所在目录，uri 为相对路径。
    std::pair<rhi::IRHITexture*, rhi::IRHISampler*> Load(const String& baseDir, const String& uri);

private:
    rhi::IRHIDevice*                  m_Device = nullptr;
    std::unordered_map<String, Entry> m_Cache;
};

void TextureCache::CreateAndBindDefault() {
    u8 white[4] = {255, 255, 255, 255};
    rhi::TextureDesc td;
    td.format      = rhi::Format::RGBA8_UNORM;
    td.width       = 1;
    td.height      = 1;
    td.mipLevels   = 1;
    td.usage       = rhi::TextureUsage::ShaderResource;
    td.initialData = white;
    auto defaultTex = m_Device->CreateTexture(td);

    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU  = sd.addressV  = rhi::AddressMode::Repeat;
    auto defaultSamp = m_Device->CreateSampler(sd);

    m_Device->GetBindlessHeap()->SetDefaultTexture(defaultTex.get(), defaultSamp.get());
    m_Cache["__default__"] = {std::move(defaultTex), std::move(defaultSamp)};
}

std::pair<rhi::IRHITexture*, rhi::IRHISampler*> TextureCache::Load(const String& baseDir,
                                                                  const String& uri) {
    if (uri.empty()) return {nullptr, nullptr};

    const String texPath = (std::filesystem::path(baseDir) / uri).string();
    auto it = m_Cache.find(texPath);
    if (it != m_Cache.end()) return {it->second.first.get(), it->second.second.get()};

    int w = 0, h = 0, ch = 0;
    u8* pixels = stbi_load(texPath.c_str(), &w, &h, &ch, 4);
    if (!pixels) {
        HE_CORE_WARN("纹理加载失败: {}", texPath);
        return {nullptr, nullptr};
    }

    // mip 数：按最大边长逐次减半
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
    auto tex = m_Device->CreateTexture(td);

    rhi::SamplerDesc sd;
    sd.minFilter = rhi::FilterMode::Linear;
    sd.magFilter = rhi::FilterMode::Linear;
    sd.mipFilter = rhi::FilterMode::Linear;
    sd.maxLod    = static_cast<float>(mipLevels);
    sd.addressU  = sd.addressV = rhi::AddressMode::Repeat;
    auto samp = m_Device->CreateSampler(sd);

    stbi_image_free(pixels);
    auto inserted = m_Cache.emplace(texPath, std::make_pair(std::move(tex), std::move(samp))).first;
    HE_CORE_INFO("GPU 纹理: {} ({}×{})", texPath, w, h);
    return {inserted->second.first.get(), inserted->second.second.get()};
}

// ============================================================
// 场景搭建：Sponza glTF + 主方向光 + 两盏点光 + 天空盒 + 纹理与 bindless 材质注册
//
// 原先这 ~89 行内联在 main() 里。抽出来后 main 只保留"启动参数"性质的工作（窗口/RHI/配置读写），
// 场景内容集中在这里 —— 想换场景或加灯只需改这一个函数。
//
// 【点光源句柄】样例后续要按索引访问两盏点光（面板编辑 → 配置序列化），故用出参回传。
// 【顺序】纹理注册必须在天空盒之后、渲染管线初始化之前：bindless 默认占位纹理要先就位。
// ============================================================
static bool BuildScene(World& world, SceneGraph& sceneGraph, rhi::IRHIDevice* device,
                       const String& sponzaPath, Entity (&pointLightEntities)[2],
                       TextureCache& texCache) {
    HE_CORE_INFO("加载 Sponza 场景: {}", sponzaPath);
    auto result = asset::LoadGLTF(world, sceneGraph, sponzaPath);
    if (!result.success) {
        HE_CORE_ERROR("Sponza 加载失败: {}", result.error);
        return false;
    }
    HE_CORE_INFO("Sponza 加载完成: {} 实体, {} 网格图元",
                 result.entities.size(), result.meshCount);

    // 场景默认无直接光源（纯 GI）；DDGI 探针需首次照明输入（直接从 Lighting HDR 学习会自反馈发散），
    // 故加一个弱主方向光提供 DDGI 首次照明（强度低，画面仍以 GI 为主导）。
    {
        Entity mainLightEntity = world.CreateEntity("DirectionalLight");
        world.AddComponent<TransformComponent>(mainLightEntity);
        auto* mainDL = world.AddComponent<DirectionalLight>(mainLightEntity);
        mainDL->direction = float3(0.3f, -1.0f, 0.4f);
        mainDL->color     = float3(1.0f, 0.95f, 0.9f);
        mainDL->intensity = 3.0f;    // 弱光：DDGI 输入，不喧宾夺主
        mainDL->castShadow = true;
        mainDL->enabled    = false;  // 默认只看 GI（直接光关闭，勾选「只看 GI」可切回）
        sceneGraph.SetParent(mainLightEntity, Entity{kInvalidEntity});
    }

    // 点光源（2 个：Sponza 走廊两端，用于测试点光 GI/阴影/DDGI 探针）；
    // 位置/颜色/强度/范围可由面板编辑，并随 06_GILab.cfg 序列化。
    {
        struct PointLightInit { float3 pos; float3 color; float intensity; float range; };
        const PointLightInit inits[2] = {
            { float3(-20.0f, 5.0f, 0.0f), float3(1.0f, 0.7f, 0.4f), 30.0f, 18.0f },  // 暖色，走廊左端
            { float3( 20.0f, 5.0f, 0.0f), float3(0.4f, 0.7f, 1.0f), 30.0f, 18.0f },  // 冷色，走廊右端
        };
        for (int i = 0; i < 2; i++) {
            Entity e = world.CreateEntity("PointLight");
            auto* tf = world.AddComponent<TransformComponent>(e);
            tf->position = inits[i].pos;
            auto* pl = world.AddComponent<PointLight>(e);
            pl->color      = inits[i].color;
            pl->intensity  = inits[i].intensity;
            pl->range      = inits[i].range;
            pl->castShadow = false;   // 点光阴影开销大，默认关闭（面板可开）
            pl->enabled    = false;   // 默认只看 GI，与方向光一致
            sceneGraph.SetParent(e, Entity{kInvalidEntity});
            pointLightEntities[i] = e;
        }
    }

    // 天空盒：HDR 等距柱状图 → 立方体贴图 → 场景实体
    CreateSkybox(world, sceneGraph, device, String(HUGE_CONTENT_DIR) + "Textures/skybox.hdr");

    // 纹理：创建/去重/默认占位在 TextureCache（由调用方持有 —— 它必须活得比本函数久，
    // 因为 bindless 堆持有这些纹理/采样器指针）；这里只按 glTF 的 uri 取并注册 bindless 槽
    texCache.CreateAndBindDefault();   // bindless 默认占位纹理（必须在注册材质纹理之前）
    const String sponzaDir = std::filesystem::path(sponzaPath).parent_path().string();
    auto loadTexture = [&](const String& uri) { return texCache.Load(sponzaDir, uri); };

    u32 texCount = 0;
    world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent& mesh) {
        auto [bcTex, bcSamp] = loadTexture(mesh.baseColorTexture);
        auto [nTex, nSamp]   = loadTexture(mesh.normalTexture);
        auto [mrTex, mrSamp] = loadTexture(mesh.metallicRoughnessTexture);
        auto [aoTex, aoSamp] = loadTexture(mesh.occlusionTexture);
        // 材质 = 4 个连续 bindless 纹理槽（BaseColor/Normal/MetallicRoughness/Occlusion），
        // 首调用返回值即 materialID（基索引），shader 用 texBase+0/1/2/3 采样
        auto* heap = device->GetBindlessHeap();
        const u32 matID = heap->RegisterTexture(bcTex, bcSamp);
        heap->RegisterTexture(nTex, nSamp);
        heap->RegisterTexture(mrTex, mrSamp);
        heap->RegisterTexture(aoTex, aoSamp);
        mesh.materialID = matID;
        texCount++;
    });
    HE_CORE_INFO("纹理加载 + bindless 注册完成: {} primitive, {} 张独立纹理", texCount, texCache.Count());
    return true;
}

// ============================================================
// GILabUI —— 06.GILab 的两个 ImGui 面板（主窗口 / GI 控制台）
//
// 原先这两个面板共 ~460 行内联在 main() 的帧循环里。抽成成员函数后 main 只保留
// "BeginFrame -> DrawMainPanel -> DrawGIPanel"，读 UI 逻辑不必在 1700 行里找。
//
// 【为什么用"全引用成员"】面板要读写 main 里的一批状态（相机、管线、模式开关、探针…）。
// 把这些状态以**同名引用**成员交给本结构后，面板代码可以**逐字不动**（裸名字直接解析到成员），
// 既没有 lambda 捕获，也不必给几百行代码逐个改写成 ui.xxx。
// ============================================================
struct GILabUI {
    render::DeferredPipeline&            deferredPipeline;
    render::ForwardPipeline&             forwardPipeline;
    render::IRenderPipeline*&            curPipeline;
    render::CameraController&            camCtrl;
    World&                               world;
    SceneGraph&                          sceneGraph;
    std::unique_ptr<rhi::IRHIDevice>&    device;
    std::unique_ptr<rhi::IRHISwapChain>& swapchain;
    render::PixelProbe&                  probe;
    f32&                                 deltaTime;
    int&                                 g_PipelineMode;
    bool&                                g_GISolo;
    int&                                 g_GIPreset;
    int&                                 g_UiThreadingMode;
    bool&                                g_ProbeEnabled;
    int&                                 g_ProbeInterval;
    bool&                                g_FurnaceMode;
    bool&                                g_PendingPipelineSwitch;
    bool&                                g_PendingHalfResApply;

    void DrawMainPanel();   // 主窗口：FPS / 管线信息 / 相机与曝光 / 场景与光源列表
    void DrawGIPanel();     // GI 控制台：管线与档位 / 层栈四通道 / 配置诊断已删 / Shadow
    void DrawGIChannels();  // 层栈四通道（Diffuse/Specular/AO）+ Shadow
};

void GILabUI::DrawMainPanel() {
    ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("06.GILab (Cornell Box)");
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
        // ---- 渲染线程模式（T2.4/T2.2）----
    {
        static const char* kModeItems[] = {
            "0  单线程（收集/录制/提交/呈现都在游戏线程）",
            "1  游戏线程 + 渲染线程（整帧 RHI 归渲染线程）",
            "2  游戏线程 + 渲染线程 + RHI 线程（RHI 线程 = 唯一提交者）",
        };
        int sel = g_UiThreadingMode;
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::Combo("渲染线程模式##threading", &sel, kModeItems, IM_ARRAYSIZE(kModeItems))) {
            g_UiThreadingMode = sel;   // 帧首统一生效（切换需要排空并停/起线程）
        }
        ImGui::TextDisabled("当前生效：%s；三种模式可任意切换，切换在下一帧帧首生效",
                            he::RenderThreadingModeName(he::GetRenderThreadingMode()));
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
}

void GILabUI::DrawGIPanel() {
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
                // 切换管线：确保交换链与视口尺寸同步。
                // 【T2.2】`SetSwapChain`/`OnResize` 都是 RHI 调用 ⇒ 不能在这里做（控件跑在游戏
                // 线程），改为置标志、由**下一条渲染命令**在渲染线程上执行。
                g_PendingPipelineSwitch = true;
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
            const auto& pv = probe.Values();
            ImGui::Text("中心像素 (线性 RGB): %.4f %.4f %.4f", pv[0].rgb[0], pv[0].rgb[1], pv[0].rgb[2]);
            ImGui::Text("背景像素 (线性 RGB): %.4f %.4f %.4f", pv[1].rgb[0], pv[1].rgb[1], pv[1].rgb[2]);
            // 白炉正确时：物体消失 → 中心与背景都应等于环境真值 1.0
            const float ratio = probe.LuminanceRatio(0, 1);
            const bool ok = (ratio > 0.98f && ratio < 1.02f);
            const ImVec4 col = ok ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
            ImGui::TextColored(col, "亮度比 中心/背景 = %.4f   (白炉判据 0.98 ~ 1.02)", ratio);
            ImGui::TextColored(col, "中心亮度 = %.4f   (白炉真值 1.0)", pv[0].luminance);
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

        DrawGIChannels();   // 层栈四通道 + Shadow（实现见本函数之后）

    }
    ImGui::End();
}

// GI 通道 UI：层栈四通道（Diffuse/Specular/AO，含源勾选 + 权重 + 让位 + 合成方式）与 Shadow。
// 依赖从 curPipeline / device 现算，因此可从 DrawGIPanel 里独立出来（原先 216 行内联在其中）。
void GILabUI::DrawGIChannels() {
    const u32  giCaps = curPipeline->GetGIPipelineCaps();
    const bool rtOk   = device->GetCaps().supportsRayTracing;
    render::GIConfig& gc = *curPipeline->GetGIConfig();

        // 当前管线可用的 GI 子系统（按管线类型获取，Forward 无屏幕空间/探针 GI）
        auto* dp = dynamic_cast<render::DeferredPipeline*>(curPipeline);
        auto* giSSGI = dp ? dp->GetSSGI() : nullptr;
        auto* giSSR  = dp ? dp->GetSSR()  : nullptr;
        auto* giDDGI = dp ? dp->GetDDGI() : nullptr;

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
}



// ============================================================
// GILabFrameRecorder —— 每帧两条渲染命令的录制体（命令 1 / 命令 2）
//
// 原先两条命令是 main() 里两个 [&] lambda（74 + 112 行）。抽成成员函数后主循环只剩
// "submitRender(...)" 两处调用，录制逻辑集中在这里。
//
// 【为什么用"同名引用成员"】两条命令要读写 main 里的一批对象（设备/命令列表/管线/相机/
// 转储器/探针/各开关）。以同名引用成员交给本结构后，命令体可以**逐字不动**。
// 【哪些走参数】每帧会变的值（frameAborted、deltaTime、renderFrameIndex、dumpThisFrame）
// 一律走参数 —— 避免引用成员绑到每帧新建的局部量上而悬空。
// ============================================================
struct GILabFrameRecorder {
    editor::ImGuiIntegration&             imgui;
    rhi::FramePresenter&               framePresenter;
    std::unique_ptr<rhi::IRHIDevice>&     device;
    std::unique_ptr<rhi::IRHICommandList>& cmdList;
    std::unique_ptr<rhi::IRHISwapChain>&  swapchain;
    Engine&                               engine;
    render::DeferredPipeline&             deferredPipeline;
    render::ForwardPipeline&              forwardPipeline;
    render::IRenderPipeline*&             curPipeline;
    render::CameraController&             camCtrl;
    render::FrameDumpRecorder&            giDump;
    render::PixelProbe&                   probe;
    World&                                world;
    SceneGraph&                           sceneGraph;
    u64&                                  frameIndex;
    int&                                  g_PipelineMode;
    int&                                  g_ProbeInterval;
    bool&                                 g_ProbeEnabled;
    bool&                                 g_FurnaceMode;
    bool&                                 g_PendingPipelineSwitch;
    bool&                                 g_PendingHalfResApply;
    const bool&                           noVsync;   // main 里是 const bool（开机决定一次）

    /// 命令 1：Acquire + 切管线 + 装配快照 + 阴影 + 各 Pass 录制 + End/Submit
    void RecordScene(bool& frameAborted, f32 deltaTime);

    /// 命令 2：ImGui 录制 + 白炉探针 + GI 采样落盘 + Present
    void RecordUiAndPresent(u64 renderFrameIndex, bool dumpThisFrame);
};

void GILabFrameRecorder::RecordScene(bool& frameAborted, f32 deltaTime) {
    if (!framePresenter.BeginFrame()) {
        HE_CORE_WARN("06.GILab：AcquireNextImage 失败，跳过本帧剩余部分");
        frameAborted = true;   // 命令可能在渲染线程跑 ⇒ 用标志代替 while 的 continue
        return;
    }

    // 控件里请求的管线切换（含 RHI）在这里（渲染线程）落地
    if (g_PendingPipelineSwitch) {
        g_PendingPipelineSwitch = false;
        curPipeline->SetSwapChain(swapchain.get());
        curPipeline->OnResize(swapchain->GetWidth(), swapchain->GetHeight());
    }

    framePresenter.BeginRecording(device.get(), cmdList.get());   // L4-2
    switch (g_PipelineMode) {
    case 0:  // Forward
        curPipeline = &forwardPipeline;
        break;
    default: // Deferred（含光追源：RT 已归入 GI 层栈，无需独立管线）
        curPipeline = &deferredPipeline;
        break;
    }
    curPipeline->NextFrame();
    // 【阶段 1 §15.1 第③段：帧入口收快照】装配在**命令 1（recordScene）内部**统一经接口完成
    // （模式 0 就地 ⇒ 游戏线程；模式 1/2 ⇒ 渲染线程，由命令 1 的严格握手保证此刻无人改世界；
    //  三条管线同构；
    // `AssembleScene` 每帧幂等 ⇒ Forward 分支下面再调一次也无副作用）。必须在任何阴影收集之前，
    // 因为阴影技术现在从快照取几何与光源。
    curPipeline->GetFrameAssembler().AssembleScene(world, sceneGraph, camCtrl.GetCamera());
    // --- Forward 的阴影系统必须由**调用方**驱动（任务 34 / §9.2-AD）---
    // `ShadowSystem` 不像 GI 子系统那样自己从帧图拿数据：它要靠调用方先
    // `SetRenderResources`（对象/阴影缓冲 + 描述符集）再 `Update`（收集投影光源、拟合 CSM），
    // 之后 `HasActiveShadows()` 才为真。02.Cube / 03.Sponza-Forward / AISamples 都这么做，
    // **06.GILab 此前漏了** ⇒ Forward 模式下 `Shadow` 与 `RSM_Generate` 两个 pass 都不注册：
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
            shadowCtx.camera     = &camCtrl.GetCamera();
            // 物理天空的太阳方向先同步到方向光：阴影与光照必须同向（02.Cube 同款做法）
            he::SyncPhysicalSkyToSun(world);
            // 快照已在上面统一装配（`AssembleScene` 幂等）；阴影收集吃的就是它
            shadowCtx.snapshot     = &forwardPipeline.GetFrameSnapshot();
            shadowCtx.meshRegistry = &forwardPipeline.GetMeshRegistry();
            shadowSys->Update(shadowCtx);
            forwardPipeline.GetFrameAssembler().ResolveLightShadowIndices(
                [&](he::Entity le) { return shadowSys->GetShadowIndex(le); });
            forwardPipeline.GetFrameAssembler().ReserveOnce();
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
    // 【T2.2 步骤 (b)】管线渲染与其余 RHI 调用一起放进本帧的**第一条命令**，
    // 不再单独经队列（一帧两条命令，见下面 submitRender 的两处调用）。
    curPipeline->Render(cmdList.get(), curPipeline->GetFrameSnapshot(),
                         camCtrl.GetCamera(), deltaTime);


}

void GILabFrameRecorder::RecordUiAndPresent(u64 renderFrameIndex, bool dumpThisFrame) {
    // --- ImGui（LOAD 保留 ToneMap 输出）---
    cmdList->BeginRenderPass(1, rhi::Format::BGRA8_UNORM,
        rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);

    imgui.EndFrame(cmdList.get());
    cmdList->EndRenderPass();

    // ── 白炉探针：把 HDR 目标上的两个像素拷进 host 可见缓冲 ──
    // 必须在 render pass 之外录制（拷贝不能在 pass 内），因此放在 EndRenderPass 之后、End 之前
    // 白炉探针：把 HDR 上的中心/背景两个像素读回（机制在 PixelProbe）
    const bool probeThisFrame = g_ProbeEnabled &&
        probe.WantsFrame(frameIndex, (u32)std::max(1, g_ProbeInterval));
    if (probeThisFrame) {
        // HDR 要按**当前管线**取：Forward 有自己的那张（层栈归一化在 Forward 也生效）
        rhi::IRHITexture* hdr = (g_PipelineMode == 0) ? forwardPipeline.GetHDRTarget()
                                                      : deferredPipeline.GetLighting().GetHDRTarget();
        if (hdr) {
            const u32 pw = swapchain->GetWidth(), ph = swapchain->GetHeight();
            const render::PixelProbe::Sample pts[2] = {
                {pw / 2,  ph / 2},    // 中心（物体所在）
                {pw / 10, ph / 10},   // 背景取样点
            };
            probe.RecordReadback(cmdList.get(), hdr, pts, 2);
            probe.FinishRecording(device.get(), cmdList.get());   // 挂本帧 timeline
        }
    }

    // ── GI 频谱采样：把若干张纹理整幅拷进 host 可见缓冲（仅测试路径）──
    // 同样必须在 render pass 之外录制；缓冲在采样帧才创建，尺寸取自纹理本身
    // 目标分三类：
    //   hdr / albedo  —— 合成结果与接收端反照率（做差 + 除 albedo 后即得 E/π）
    //   provN_raw/final —— 各有效 Provider 的原始输出 / 降噪后输出（定位"某源为 0"发生在哪一级）
    if (dumpThisFrame) {
        // 登记纹理目标：机制（读回缓冲 / 格式推断 / 拷贝录制）在 FrameDumpRecorder 里
        auto addTarget = [&](const String& name, rhi::IRHITexture* tex) {
            giDump.AddTexture(device.get(), cmdList.get(), name.c_str(), tex);
        };
        // 【诊断用】缓冲落盘（只在 HE_DUMP_LUMEN_PROBES=1 时调用）
        // 登记缓冲目标（探针/单元图等非纹理中间量，落盘为 .bin）
        auto addBufferTarget = [&](const String& name, rhi::IRHIBuffer* src, usize bytes) {
            giDump.AddBuffer(device.get(), cmdList.get(), name.c_str(), src, bytes);
        };
        // HDR 取**当前管线**的那张（任务 26）：Forward 没有 GBuffer / Provider 输出，
        // 但它的 HDR 目标就是层栈归一化的产物 —— 这正是 Forward 侧唯一可读的判据出口。
        // 此前这里无条件取 deferredPipeline 的 HDR，于是 `pipeline_mode=0` 下落盘的
        // 根本不是 Forward 的画面（文档 §11.3 早就把这点写成了注意事项，任务 26 修掉）。
        const bool forwardMode = (g_PipelineMode == 0);
        addTarget("hdr", forwardMode ? forwardPipeline.GetHDRTarget()
                                     : deferredPipeline.GetLighting().GetHDRTarget());
                          // RSM 链路的逐级中间量（任务 30）：按当前管线取（两管线各有自己的光源视锥）
        if (auto* rsm = forwardMode ? forwardPipeline.GetRSM() : deferredPipeline.GetRSM()) {
            addTarget("rsm_pos", rsm->GetRSMPositionMap());
            addTarget("rsm_nrm", rsm->GetRSMFluxMap());
            addTarget("rsm_rad", rsm->GetRSMRadianceMap());
        }
        // Deferred 侧清单（GBuffer / 前帧辐射度 / 各 Provider / Lumen / RSM 间接光 / SSR）
        // 已下沉到管线 CollectDumpTargets()：判据定义的"真值"只有一份。
        if (auto* dpDump = dynamic_cast<render::DeferredPipeline*>(curPipeline))
            dpDump->CollectDumpTargets(giDump, device.get(), cmdList.get());
        // 探针级**缓冲**目标：需要 ScreenProbe 的尺寸（该类型只对本样例可见），故留在样例侧。
        if (giDump.IncludeBuffers() && !forwardMode) {
            constexpr usize kDumpProbes = 8192;
            for (auto& prov : deferredPipeline.GetGIProviders()) {
                auto* lp = dynamic_cast<render::LumenProvider*>(prov.get());
                if (!lp || !lp->ProducedThisFrame()) continue;
                addBufferTarget("lumen_probes",    lp->GetProbeBuffer(),    kDumpProbes * sizeof(ScreenProbe));
                addBufferTarget("lumen_rawprobes", lp->GetRawProbeBuffer(), kDumpProbes * sizeof(ScreenProbe));
                if (auto* cells = lp->GetCellProbeBuffer()) {
                    addBufferTarget("lumen_cellmap", cells,
                                    (usize)lp->GetScreenCellsX() * lp->GetScreenCellsY() * sizeof(u32));
                }
                break;
            }
        }
        giDump.FinishRecording(device.get(), cmdList.get());   // 挂本帧 timeline 信号
    }

    framePresenter.EndRecording();   // L4-2：End + Submit 收口
    deferredPipeline.FlushComputeWork();  // AsyncCompute: Graphics Submit 之后提交 Compute 工作

    // ── 白炉探针：等本帧 timeline 后读回数值（机制在 PixelProbe）──
    if (probeThisFrame && probe.TryReadBack(device.get())) {
        const auto& v = probe.Values();
        HE_CORE_INFO("[白炉探针] 中心 RGB=({:.4f},{:.4f},{:.4f}) 背景 RGB=({:.4f},{:.4f},{:.4f}) "
                     "亮度 中心={:.4f} 背景={:.4f} 比值={:.4f} 白炉={}",
                     v[0].rgb[0], v[0].rgb[1], v[0].rgb[2],
                     v[1].rgb[0], v[1].rgb[1], v[1].rgb[2],
                     v[0].luminance, v[1].luminance, probe.LuminanceRatio(0, 1),
                     g_FurnaceMode ? "ON" : "off");
    }

    // ── GI 频谱采样：等本帧 timeline 后原样落盘（机制在 FrameDumpRecorder）──
    {
        const render::CameraData& cam = camCtrl.GetCamera();
        render::FrameDumpRecorder::CameraSnapshot camSnap;
        camSnap.pos[0] = cam.position.x; camSnap.pos[1] = cam.position.y; camSnap.pos[2] = cam.position.z;
        camSnap.forward[0] = cam.forward.x; camSnap.forward[1] = cam.forward.y; camSnap.forward[2] = cam.forward.z;
        camSnap.up[0] = cam.up.x; camSnap.up[1] = cam.up.y; camSnap.up[2] = cam.up.z;
        camSnap.fov = cam.fov; camSnap.nearPlane = cam.nearPlane;
        camSnap.farPlane = cam.farPlane; camSnap.aspect = cam.aspectRatio;
        if (giDump.TryWriteBack(device.get(), camSnap)) {
            // 采样完成即请求关窗：让脚本无需超时等待，也保证退出前正常走完清理与保存流程
            glfwSetWindowShouldClose(engine.GetWindow()->GetNativeHandle(), GLFW_TRUE);
        }
    }


    // 垂直同步必须与创建时一致：只改 SwapChainDesc 而这里仍传 true，帧率照样被锁在刷新率
    // （步骤 37 第一次测就是这么被误导的：pass 合计 14 ms 却只有 19 fps）。
    framePresenter.EndFrame(!noVsync);
}


int main() {
    // ---- 0. 安装崩溃处理器（Wave 0.8 诊断基建） ----
    //    崩溃时打印"函数名 + 源文件:行号"的完整调用栈，并写出 minidump。
    //    之所以放在最前面：初始化阶段（设备/交换链/资源创建）同样是崩溃高发区。
    {
        const std::string crashLog = std::string(HUGE_CONTENT_DIR) + "Config/06_GILab_crash.log";
        he::InstallCrashHandler(crashLog.c_str(), nullptr);
    }

    // ---- 1. 引擎启动 ----
    EngineConfig config;
    config.appName      = "HugEngine — 06.GILab (Cornell Box GI 对比)";
    config.windowWidth  = 1920;   // 窗口宽（960×2）
    config.windowHeight = 1080;   // 窗口高（540×2）

    // 【默认模式 2：三线程】（游戏线程 + 渲染线程 + RHI 线程）
    //   · 渲染线程只**录制**；`Submit` / `Present` 经二级队列交给 **RHI 线程**（唯一提交者）；
    //   · 游戏线程只做输入 / 相机 / 打包命令与 UI —— **装配快照在命令 1（recordScene）里**，
    //     因此模式 1/2 下它在**渲染线程**执行（详见下方 applyThreadingMode 附近的说明）。
    // 【如何覆盖】环境变量优先于本配置（见 `Engine::Initialize`）：
    //   · `HE_RENDER_THREADING_MODE=0` ⇒ 单线程；`=1` ⇒ 游戏线程 + 渲染线程（两线程对照）；
    config.renderThreadingMode = he::RenderThreadingMode::RenderThreadAndRHI;
    // vsync 打开时墙钟帧率被锁在刷新率，`HE_NO_VSYNC=1` 可关掉垂直同步（用于观察真实 GPU 吞吐）
    const bool noVsync = (std::getenv("HE_NO_VSYNC") != nullptr);
    config.enableVSync  = !noVsync;
    config.logLevel     = LogLevel::Info;

    Engine engine(config);
    engine.Initialize();

    // ---- 2. 创建 Vulkan RHI 设备 ----
    rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    auto device = rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    rhi::SetDevice(device.get());

    // ---- 3. 创建 SwapChain ----
    auto swapchain = device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width  = engine.GetWindow()->GetWidth(),
        .height = engine.GetWindow()->GetHeight(),
        .vsync  = !noVsync,   // 与 EngineConfig 同源（HE_NO_VSYNC=1 时同时关掉，否则帧率仍被锁 60）
    });

    // ---- 4. 初始化场景（Sponza glTF + 光源 + 天空盒 + 纹理注册，实现见 BuildScene）----
    World world;
    SceneGraph sceneGraph(world);

    const String sponzaPath = String(HUGE_CONTENT_DIR) + "gltf/Sponza/glTF/Sponza.gltf";
    Entity pointLightEntities[2];   // 面板编辑/配置序列化要按索引访问，故由 BuildScene 回传
    // 纹理缓存必须由 main 持有：bindless 堆会长期持有这些纹理/采样器指针，
    // 若让缓存随 BuildScene 作用域析构，下一帧 Flush() 更新描述符集就是野指针（实测崩溃）。
    TextureCache texCache(device.get());
    if (!BuildScene(world, sceneGraph, device.get(), sponzaPath, pointLightEntities, texCache)) {
        return 1;
    }

    // --- 加载配置文件 ---
    auto cfgData = LoadConfigFile(g_ConfigPath);
    const bool hasConfig = !cfgData.empty();
    if (hasConfig) {
        HE_CORE_INFO("加载配置文件: {}", g_ConfigPath);
    }

    // ---- 6. 初始化渲染管线（Forward / Deferred；光追经 GI 层栈的 RT 源启用） ----
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


    int  g_PipelineMode = 1;                // 0=Forward / 1=Deferred
    bool g_PendingHalfResApply = false;     // 档位切换后延迟到帧边界重建半分辨率纹理
    bool g_PendingPipelineSwitch = false;   // 控件请求切换管线（含 RHI）⇒ 延迟到渲染命令里做
    bool g_GISolo = true;                   // 只看 GI（关闭直接光）
    int  g_GIPreset = 1;                    // GI 质量档位（默认 Medium=1）
    render::IRenderPipeline* curPipeline = &deferredPipeline;

    // ---- 白炉探针（Wave 0.2）—— 能量守恒判据的读数设施 ----
    //
    // 目的：白炉测试要求"全白环境 + albedo=1 + 关闭直接光"时物体应消失，即
    //       任意两点亮度应相等。这里读回 HDR 目标（**色调映射之前**，线性值）
    //       上两个像素：画面中心（物体所在）与背景点，输出亮度比供数值断言。
    // 实现：RHI 的 CopyTextureToBuffer 把 1×1 像素拷进 host 可见缓冲，等 GPU 完成后
    //       映射读取（RGBA16_FLOAT → half 解码）。仅在启用探针时每 N 帧执行一次，
    //       且会 WaitIdle（测试用途，不追求性能）。
    bool  g_ProbeEnabled   = (std::getenv("HE_FURNACE_PROBE") != nullptr);   // 也可用 HE_FURNACE_PROBE=1 直接开启（便于自动化验证）
    int   g_ProbeInterval  = 30;      // 每 N 帧采一次
    // 白炉模式（HE_FURNACE=1 直接开启；面板开关在 GI 控制台里）
    bool  g_FurnaceMode    = (std::getenv("HE_FURNACE") != nullptr);
    // 像素数值探针（机制在 Engine/Render/Debug/PixelProbe；这里只保留开关/间隔与判据阈值）
    render::PixelProbe probe;
    probe.Initialize(device.get(), 2);   // 两个采样点：中心 / 背景

    // ---- GI 频谱采样（P5 步骤 0）—— 判定 DDGI 与 SSGI 是否需要"频率分离" ----
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
    // GI 采样落盘：**机制**在 Engine/Render/Debug/FrameDumpRecorder（录制拷贝 / timeline 等待 /
    // _meta.txt 与 _camera.txt 写法都收在那里）；这里只给"哪些目标值得看"的清单（策略）。
    auto giDump = render::FrameDumpRecorder::FromEnv();


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

    // ---- 7. 创建命令列表 ----
    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());
    // 预设 ToneMap PSO → 匹配 BGRA8_UNORM RP（ImGui LoadOp 兼容）
    cmdList->SetPipeline(deferredPipeline.GetToneMap()->GetPSO());

    // ---- 8. ImGui 初始化 ----
    GLFWwindow* glfwWin = engine.GetWindow()->GetNativeHandle();
    editor::ImGuiIntegration imgui;
    imgui.Initialize(glfwWin, device.get(), swapchain.get());
    // 各面板的窗口位置/大小/折叠状态由 ImGui 自动序列化——保存到配置目录（与 06_GILab.cfg 同处）
    // 用绝对路径（HUGE_CONTENT_DIR）避免依赖当前工作目录
    static String g_ImGuiIniPath = String(HUGE_CONTENT_DIR) + "Config/06_GILab_imgui.ini";
    ImGui::GetIO().IniFilename = g_ImGuiIniPath.c_str();

    // ---- 9. 相机 — 从配置文件加载，否则使用默认位置 ----
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

    // ---- 8.5 相机平移动画（AnimationComponent 演示） ----
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

    // ---- 10. 窗口调整回调 ----
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (w == 0 || h == 0) return;
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        deferredPipeline.OnResize(w, h);
        forwardPipeline.OnResize(w, h);
        camCtrl.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // ---- 11. 主渲染循环 ----
    HE_CORE_INFO("06.GILab (Cornell Box) 启动 — WASD=移动, 右键拖拽=旋转, Shift=加速, E/Q=升降");
    u64 frameIndex = 0;
    f64 lastTime   = glfwGetTime();

    // ── 渲染线程化（方案 B 阶段 0 T0.3/T0.4）：帧渲染经命令队列交给"渲染线程" ──
    // 【阶段 0 的语义】`RenderThread` 是**壳**：`SubmitAndPump` 在调用线程上立即执行命令，
    // 因此帧序、录制顺序与结果与直接调用逐像素一致（判据即 `cmp_dumps` 前后相同）。
    // 三态模式（T0.5）决定是否走队列：`SingleThreaded` 走旧路径，另外两种模式走队列。
    // 【T2.4 已落地：载荷按值捕获】命令载荷现在只携带"本帧渲染输入"的**值**（管线指针 + 本帧槽位
    // 快照指针 + 命令缓冲 + 相机/时长的值拷贝），不再按引用捕获游戏线程的栈变量 —— 铁律 2 由此满足。
    // 剩下的"真正异步"（渲染线程自己持有命令缓冲、自己 Submit/Present）是 T2.2，它把
    // `SubmitAndPump` 换成"只发布"、由渲染线程的循环 Pump。
    render::RenderCommandQueue renderQueue(rhi::kMaxFramesInFlight);
    render::RenderThread       renderThread(renderQueue);
    render::FrameScheduler     frameScheduler(renderQueue, renderThread);
    // 【运行时可切换】不再是启动时锁存的常量：下拉选择模式后，在**帧首**统一生效
    // （切到 0 要排空并停线程、切到 ≥1 要起线程并认领 RHI 归属）。
    bool useRenderQueue = he::UsesRenderThread();
    // 下拉菜单绑定的目标模式（初值 = 实际生效的模式，取自配置/环境变量）
    int  g_UiThreadingMode = static_cast<int>(he::GetRenderThreadingMode());
    // 【T2.2 读数开关，默认关闭】`HE_RENDER_THREAD_STRICT=1` 时**真的起渲染线程**。
    // 现在样例里仍有大量游戏线程直接调 RHI 的地方（acquire / 录制 / Submit / Present / WaitIdle），
    // 而 RHI 侧对"拥有线程"有断言（`HE_ASSERT_RENDER_THREAD`）⇒ 打开本开关就能**实测**出
    // 还差哪些调用点没搬。默认关闭 ⇒ 正常路径（壳模式）与既有判据完全不变。
    // 【T2.2】模式 1/2 **起真渲染线程**：整帧 RHI（Acquire → 录制 → ImGui 录制 → 调试绘制 →
    // Submit → 探测读回 → Present）都在它上面执行；游戏线程只做输入/相机/装配快照/ImGui 的 CPU 侧，
    // 并按帧用**严格握手**（`SubmitAndWait`）等待本帧做完才继续下一帧。
    // 判据：模式 0 与模式 1 的转储逐位一致（实测 0 像素）、无归属断言、帧时间不退化。
    // 【模式 2】RHI 线程（三线程形态）：渲染线程的 Submit/Present 被路由到它，由它真正提交/呈现
    he::render::RHIThread rhiThread;
    // 【统一切换入口】`mode` 为下拉选择的目标模式（壳模式请直接用 RenderThread 的未启动形态）
    // 【为什么按"先停后起、停 RHI 先于渲染、起渲染先于 RHI"】任意两种模式之间切换都必须走这条路：
    //   · 停：RHI 线程先排空（它可能持有未提交的命令），否则渲染线程退出时还有路由在飞；
    //   · 起：渲染线程先起来（录制者），RHI 线程随后装路由（提交者）—— 反过来会让首帧的提交
    //     先路由到一个还没起来的线程（`RouteSubmit` 会退回就地执行，虽不丢命令但语义不清）。
    auto applyThreadingMode = [&](he::RenderThreadingMode mode) {
        const bool wantRender = (mode != he::RenderThreadingMode::SingleThreaded);
        const bool wantRhi    = (mode == he::RenderThreadingMode::RenderThreadAndRHI);

        // ① 先改模式（线程启停期间日志/`UsesRenderThread()` 与目标一致）
        he::SetRenderThreadingMode(mode);
        useRenderQueue = he::UsesRenderThread();

        // ② 【切模式前必须等渲染队列排空】命令 2 是"只发布不等待"（`submitRender(..., false)`）⇒
        // 上一帧可能还在渲染线程上执行（正在 Acquire/Present）。此时停线程或换路由会让呈现落到
        // 错误的线程上（实测切换瞬间命中 VUID-vkAcquireNextImageKHR-semaphore-01286 / -surface-07783）。
        while (renderQueue.InFlightFrameCount() > 0u) {
            std::this_thread::yield();   // 帧在飞数极小（≤ kMaxFramesInFlight），短自旋足够
        }

        // ③ 停：RHI 线程 → 渲染线程
        if (rhiThread.IsRunning() && !wantRhi) rhiThread.Stop();
        if (renderThread.IsRunning() && !wantRender) {
            renderThread.Stop();   // Stop 会排空队列并触发停止钩子（撤销 RHI 归属）
            HE_CORE_INFO("06.GILab：已停渲染线程（模式 = {}；帧渲染回到游戏线程）",
                         he::RenderThreadingModeName(mode));
        }

        // ③ 起：渲染线程 → RHI 线程
        if (wantRender && !renderThread.IsRunning()) {
            renderThread.SetSpinWaitUs(50);
            // RHI 归属：启动时认领给渲染线程、停止时撤销（认领必须在新线程里做 ⇒ 走启动钩子）
            renderThread.SetThreadStartHook([] { he::rhi::GetThreadAffinity().Claim(); });
            renderThread.SetThreadStopHook([] { he::rhi::GetThreadAffinity().Release(); });
            renderThread.Start();
            HE_CORE_INFO("06.GILab：已起真渲染线程（模式 = {}；录制归它）", he::RenderThreadingModeName(mode));
        }
        if (wantRhi && !rhiThread.IsRunning()) {
            rhiThread.Start(device.get());   // 内部装路由：此后 Submit/Present 走 RHI 线程
        }
        HE_CORE_INFO("06.GILab：线程模式已生效 = {}（渲染线程 {} / RHI 线程 {}）",
                     he::RenderThreadingModeName(mode),
                     renderThread.IsRunning() ? "on" : "off",
                     rhiThread.IsRunning() ? "on" : "off");
    };
    applyThreadingMode(he::GetRenderThreadingMode());   // 启动时按配置/环境变量生效一次

    // L4-1：帧外壳的 acquire/present 收口（L4-3 把整帧变成命令流时只改这里）
    rhi::FramePresenter framePresenter(swapchain.get());

    while (!engine.GetWindow()->ShouldClose()) {
        f64 now       = glfwGetTime();
        f32 deltaTime = static_cast<f32>(now - lastTime);
        lastTime      = now;

        engine.GetWindow()->PollEvents();

        // 【帧首落地】下拉选择与当前生效模式不一致时，在这里统一切换（下一帧命令即按新模式走）
        if (g_UiThreadingMode != static_cast<int>(he::GetRenderThreadingMode())) {
            applyThreadingMode(static_cast<he::RenderThreadingMode>(g_UiThreadingMode));
        }

        bool frameAborted = false;   // 渲染命令里 Acquire 失败时置位（命令可能在别的线程跑）

        // 【T2.2 步骤 (b)】一帧两条命令的统一交接：模式 0 直接在本线程执行（此时本线程就是渲染
        // 线程）；模式 1/2 经队列 + **严格握手**（`SubmitAndWait`）⇒ 游戏线程在渲染线程工作期间
        // 阻塞，所以命令里 `[&]` 捕获的帧内局部量不会被并发改写（T2.6 会换成类型化载荷 + cv 唤醒）。
        // 【T2.4 步骤 (b)：去掉每帧的第二次握手】`waitForCompletion = false` 时**只发布不等待** ——
        // 真线程模式下渲染线程按 FIFO 消费，所以"下一帧的命令 1"必然在"本帧的命令 2"之后执行；
        // 于是游戏线程**不必**在命令 2 之后等待，可以直接去准备下一帧（输入/相机/装配快照），
        // 与渲染线程执行本帧命令 2（ImGui 录制 + Submit + Present）**重叠**。
        // 帧内资源仍安全：命令 2 不读快照，而同一份 `cmdList` 由 FIFO 保证了先来后到。
        u64 lastAsyncFrame = UINT64_MAX;
        auto submitRender = [&](auto&& fn, bool waitForCompletion = true) {
            if (useRenderQueue) {
                renderQueue.BeginFrame();
                renderQueue.Enqueue([&](render::RenderThreadContext&) { fn(); });
                if (waitForCompletion) {
                    bool timedOut = false;
                    frameScheduler.SubmitAndWait(timedOut);
                    if (timedOut) HE_CORE_WARN("06.GILab：等待本帧渲染命令完成超时");
                } else {
                    // 只发布：真线程模式下由渲染线程执行；壳模式下 SubmitAndPump 会就地执行并回收
                    lastAsyncFrame = frameScheduler.SubmitAndPump();
                }
            } else {
                fn();
            }
        };


        // --- 相机控制（鼠标右键拖拽旋转 / WASD+EQ 移动 / T 切换动画相机）---
        // 轮询与状态机统一在 Samples/Common/CameraInput.h（六个样例共用，手感一致）；
        // 这里只保留样例自己的策略：T 键在"动画相机 / 手动相机"之间切换。
        // 动画相机模式：先让 AnimationComponent 覆盖位置/朝向，再让输入叠加（与旧顺序一致）。
        if (animCameraMode && camAnim->playing) {
            if (auto* camTf = world.GetComponent<TransformComponent>(camAnimEntity)) {
                camCtrl.SetPosition(camTf->position);
                const float3 toOrigin = glm::normalize(float3(0, 200, 0) - camTf->position);
                camCtrl.SetOrientationFromForward(toOrigin);
            }
        }
        static he::samples::CameraInput camInput;   // 跨帧保存右键/光标/切换键状态
        if (camInput.Update(glfwWin, camCtrl, deltaTime, {.toggleKey = GLFW_KEY_T})) {
            animCameraMode = !animCameraMode;
            camInput.Reset();   // 切回手动时丢弃过期的光标坐标，避免视角跳一下
        }

        // Transform 动画更新（驱动 AnimationComponent → TransformComponent）
        world.ForEach<he::AnimationComponent>([&](he::Entity e, he::AnimationComponent& anim) {
            auto* tf = world.GetComponent<TransformComponent>(e);
            if (tf) anim.Update(deltaTime, tf);
        });

        // --- 渲染（按选择的管线执行；各管线均通过 RenderGraph 自动编排）---
        // ---- 命令 1：Acquire + 录制（管线）----
        // 两条渲染命令的录制体（全引用成员；见 main 之前的 GILabFrameRecorder）
        GILabFrameRecorder frameRec{imgui, framePresenter, device, cmdList, swapchain, engine,
                                    deferredPipeline, forwardPipeline, curPipeline, camCtrl,
                                    giDump, probe, world, sceneGraph, frameIndex,
                                    g_PipelineMode, g_ProbeInterval, g_ProbeEnabled, g_FurnaceMode,
                                    g_PendingPipelineSwitch, g_PendingHalfResApply, noVsync};

        submitRender([&] { frameRec.RecordScene(frameAborted, deltaTime); });
        if (frameAborted) continue;   // Acquire 失败：跳过本帧剩余部分

        // UI 依赖（全引用成员；见 main 之前的 GILabUI）
        GILabUI uiDeps{deferredPipeline, forwardPipeline, curPipeline, camCtrl, world, sceneGraph,
                       device, swapchain, probe, deltaTime,
                       g_PipelineMode, g_GISolo, g_GIPreset, g_UiThreadingMode,
                       g_ProbeEnabled, g_ProbeInterval, g_FurnaceMode,
                       g_PendingPipelineSwitch, g_PendingHalfResApply};

        imgui.BeginFrame();
        uiDeps.DrawMainPanel();   // 主窗口（实现见 main 之前）

        // ---- 独立 GI 控制面板：渲染管线 → GI 质量档位 → GI 四通道（可用性 + 参数） ----
        // 与主面板分离，便于独立摆放/查看
        // 首次使用时默认摆放在主面板右侧（之后由 imgui.ini 恢复用户位置/大小）
        uiDeps.DrawGIPanel();     // GI 控制台（实现见 main 之前；Begin/End 都在函数内）

        // ---- 命令 2：ImGui 录制 + 调试绘制 + Submit + 探测读回 + Present ----
        // 【按值携带】帧号与转储判定由游戏线程算好后**按值**传进来（绝不读外层可变变量 ——
        // 命令可能在渲染线程执行，读外层变量会拿到下一帧已改写的值）。
        // 命令 2 只发布：游戏线程随即进入下一帧的准备（与渲染线程重叠）
        // ---- 游戏线程记账（这些是游戏线程自己的循环状态，**不能**放进渲染命令）----
        // 【顺序很关键】先冻结本帧命令该看到的帧号（原判定发生在命令里、`frameIndex++` **之前**），
        // 再自增；写反会让转储/读数早一帧。
        const u64  renderFrameIndex = frameIndex;
        const bool dumpNow = giDump.WantsFrame(renderFrameIndex);
        frameIndex++;

        // 【测量确定性】转储/读数之前先把在飞的"命令 2"等掉：流水线化后它可能还没做完，
        // 否则读回的是"少一帧"的状态（表现为转储差异与 pass/VUID 计数偏移）。
        if (useRenderQueue && lastAsyncFrame != UINT64_MAX) {
            // 【必须有超时】这是诊断/转储路径，不是背压点：无界等待会在"该帧因故未被回收"时
            // 永久挂住（实测出现过一次 300 秒未退出）。超时只告警、继续转储。
            if (!renderQueue.WaitFrameRetired(lastAsyncFrame, 5000u))
                HE_CORE_WARN("06.GILab：转储前等待在飞渲染命令超时（帧 {}）", lastAsyncFrame);
            lastAsyncFrame = UINT64_MAX;
        }

        // ---- 入队命令 2（**按值捕获**帧号与转储判定：命令可能在渲染线程执行）----
        submitRender([&, renderFrameIndex, dumpNow]() {
            frameRec.RecordUiAndPresent(renderFrameIndex, dumpNow);
        }, /*waitForCompletion=*/false);


    }

    // 【T2.2】退出前先停渲染线程并撤销 RHI 归属认领：收尾期的 `WaitIdle`/设备销毁仍在游戏线程，
    // 而归属已（在严格模式下）认领给渲染线程 ⇒ 不撤销就会撞归属断言。
    // 【模式 2】**先停 RHI 线程**再停渲染线程：否则渲染线程在收尾期仍可能提交新帧，而 RHI 线程
    // 还在队列里持有提交 ⇒ 设备销毁时会命中 vkDestroyBuffer/vkDestroyPipeline「资源仍在使用」类 VUID。
    if (rhiThread.IsRunning()) rhiThread.Stop();
    if (renderThread.IsRunning()) renderThread.Stop();   // 可能已被下拉切回单线程
    he::rhi::GetThreadAffinity().Release();

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

    // ---- 保存配置（所有 ImGui 可控参数） ----
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

    HE_CORE_INFO("06.GILab 退出 ({} 帧)", frameIndex);
    return 0;
}
