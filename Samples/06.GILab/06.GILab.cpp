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
#include "Pipeline/DeferredPipeline.h"
#include "GI/GIConfig.h"
#include "GI/GIRegistry.h"
#include "Pipeline/CameraController.h"
#include "Pipeline/PhysicalCamera.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/AnimationComponent.h"
#include "Asset/glTFLoader.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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
static String g_ConfigPath = String(HUGE_CONTENT_DIR) + "Config/06_GILab.cfg";

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
    // 1. 引擎启动
    // ============================================================
    EngineConfig config;
    config.appName      = "HugEngine — 06.GILab (Cornell Box GI 对比)";
    config.windowWidth  = 1920;   // 窗口宽（960×2）
    config.windowHeight = 1080;   // 窗口高（540×2）
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
    // 6. 初始化延迟管线
    // ============================================================
    render::DeferredPipeline pipeline;
    pipeline.Initialize(device.get());
    pipeline.SetSwapChain(swapchain.get());
    pipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());

    // ── 从配置文件恢复管线 / GI / 后处理设置 ──
    if (hasConfig) {
        pipeline.GetClusteredShading().enabled = GetInt(cfgData, "clustered", 1) != 0;
        pipeline.GetGPUCulling().enabled       = GetInt(cfgData, "gpu_cull", 1) != 0;
        pipeline.SetGBufferMode((render::GBufferRenderer::Mode)GetInt(cfgData, "gbuffer_mode", 0));

        auto& ae = pipeline.GetAutoExposure();
        ae.SetEnabled(GetInt(cfgData, "ae_enabled", 0) != 0);
        ae.SetAdaptSpeed(GetFloat(cfgData, "ae_adapt_speed", 2.0f));
        ae.SetTargetLum(GetFloat(cfgData, "ae_target_lum", 0.18f));

        auto& bloom = pipeline.GetBloom();
        bloom.SetEnabled(GetInt(cfgData, "bloom_enabled", 0) != 0);
        bloom.SetThreshold(GetFloat(cfgData, "bloom_threshold", 1.0f));
        bloom.SetIntensity(GetFloat(cfgData, "bloom_intensity", 0.5f));

        auto& dof = pipeline.GetDOF();
        dof.SetEnabled(GetInt(cfgData, "dof_enabled", 0) != 0);
        dof.SetFocusDepth(GetFloat(cfgData, "dof_focus", 0.5f));
        dof.SetFocusRange(GetFloat(cfgData, "dof_range", 0.1f));
        dof.SetIntensity(GetFloat(cfgData, "dof_intensity", 1.0f));

        auto& mb = pipeline.GetMotionBlur();
        mb.SetEnabled(GetInt(cfgData, "mb_enabled", 0) != 0);
        mb.SetIntensity(GetFloat(cfgData, "mb_intensity", 0.5f));

        pipeline.GetSSAO().enabled = GetInt(cfgData, "ssao_enabled", 0) != 0;

        if (auto* gi = pipeline.GetGI()) {
            auto s = gi->GetSettings();
            s.intensity = GetFloat(cfgData, "ibl_intensity", 1.0f);
            gi->SetSettings(s);
        }
        if (auto* ssgi = pipeline.GetSSGI()) {
            ssgi->SetEnabled(GetInt(cfgData, "ssgi_enabled", 0) != 0);
            ssgi->radius      = GetFloat(cfgData, "ssgi_radius", 1.0f);
            ssgi->sampleCount = GetInt(cfgData, "ssgi_samples", 16);
            auto s = ssgi->GetSettings();
            s.intensity = GetFloat(cfgData, "ssgi_intensity", 1.0f);
            ssgi->SetSettings(s);
        }
        if (auto* ddgi = pipeline.GetDDGI()) {
            ddgi->SetEnabled(GetInt(cfgData, "ddgi_enabled", 0) != 0);
            ddgi->blendAlpha = GetFloat(cfgData, "ddgi_blend", 0.9f);
            ddgi->debugScale = GetFloat(cfgData, "ddgi_scale", 1.0f);
            auto s = ddgi->GetSettings();
            s.intensity = GetFloat(cfgData, "ddgi_intensity", 1.0f);
            ddgi->SetSettings(s);
        }
        if (auto* ssr = pipeline.GetSSR()) {
            ssr->SetEnabled(GetInt(cfgData, "ssr_enabled", 0) != 0);
            ssr->maxSteps = GetFloat(cfgData, "ssr_max_steps", 64.0f);
            ssr->stepSize = GetFloat(cfgData, "ssr_step_size", 0.5f);
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
    cmdList->SetPipeline(pipeline.GetToneMap()->GetPSO());

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
    camCtrl.SetMoveSpeed(200.0f);   // 移动速度 200（快速浏览 Sponza 场景）

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
        pipeline.OnResize(w, h);
        camCtrl.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // ============================================================
    // 11. 主渲染循环
    // ============================================================
    HE_CORE_INFO("06.GILab (Cornell Box) 启动 — WASD=移动, 右键拖拽=旋转, Shift=加速, E/Q=升降");
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

        // --- 渲染（DeferredPipeline 通过 RenderGraph 全自动编排）---
        cmdList->Begin();
        pipeline.NextFrame();
        pipeline.Render(cmdList.get(), world, sceneGraph, camCtrl.GetCamera());

        // --- ImGui（LOAD 保留 ToneMap 输出）---
        cmdList->BeginRenderPass(1, rhi::Format::BGRA8_UNORM,
            rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);

        imgui.BeginFrame();
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
            bool clustered = pipeline.GetClusteredShading().enabled;
            if (ImGui::Checkbox("Clustered Shading", &clustered))
                pipeline.GetClusteredShading().enabled = clustered;
            if (clustered) {
                ImGui::SameLine();
                ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "%u clusters",
                    pipeline.GetClusteredShading().GetClusterCount());
            }
            bool gpuCull = pipeline.GetGPUCulling().enabled;
            if (ImGui::Checkbox("GPU 视锥剔除", &gpuCull))
                pipeline.GetGPUCulling().enabled = gpuCull;
            if (gpuCull) {
                ImGui::SameLine();
                ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "%u 可见",
                    pipeline.GetGPUCulling().GetLastVisibleCount());
            }

            // ── GI 实验室：只看 GI（关闭直接光）→ 逐个开启 GI 看间接光贡献 ──
            ImGui::SeparatorText("GI 实验室");
            static bool s_gISolo = true;   // 默认只看 GI（关闭直接光）
            if (ImGui::Checkbox("只看 GI（关闭直接光）", &s_gISolo)) {
                // 关闭/恢复所有直接光源：画面只剩 GI（IBL/SSGI/DDGI/RSM 等）的间接光
                world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& l){ l.enabled = !s_gISolo; });
                world.ForEach<he::PointLight>([&](he::Entity, he::PointLight& l){ l.enabled = !s_gISolo; });
                world.ForEach<he::SpotLight>([&](he::Entity, he::SpotLight& l){ l.enabled = !s_gISolo; });
                world.ForEach<he::RectLight>([&](he::Entity, he::RectLight& l){ l.enabled = !s_gISolo; });
            }
            ImGui::TextWrapped("开启后画面只剩 GI 间接光——\n逐个启用 SSGI/DDGI 看各自贡献；IBL 强度即环境 GI 强度。");

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

            // ── GI 质量档位（M2 数据驱动：档位/通道/强度）──
            ImGui::SeparatorText("GI 质量 (M2)");
            static int giPreset = -1;
            const char* presetNames[] = {"Low", "Medium", "High", "Ultra"};
            if (ImGui::Combo("质量档位", &giPreset, presetNames, 4)) {
                auto& gc = pipeline.GetGIConfig();
                // M3：应用预设并经 GIRegistry 自动降级（RT 源按设备光追能力决定，不可用则降级）
                gc = render::GIRegistry::Degrade(render::GIConfigFromPreset((render::GIQualityPreset)giPreset),
                                                 device->GetCaps().supportsRayTracing);
                // 应用档位到 GI 子系统开关（帧图按 config 条件注册）
                pipeline.GetSSGI()->SetEnabled(gc.ShouldRunSSGI());
                pipeline.GetDDGI()->SetEnabled(gc.ShouldRunDDGI());
                pipeline.GetSSR()->SetEnabled(gc.ShouldRunSpecular());
                pipeline.GetSSAO().enabled = gc.ShouldRunAO();
                // M4.1 halfRes 应用：档位的 halfRes 同步到 GI 子系统并重建输出纹理
                auto sgiSettings = pipeline.GetSSGI()->GetSettings();
                sgiSettings.halfRes = gc.halfRes;
                pipeline.GetSSGI()->SetSettings(sgiSettings);
                auto ssrSettings = pipeline.GetSSR()->GetSettings();
                ssrSettings.halfRes = gc.halfRes;
                pipeline.GetSSR()->SetSettings(ssrSettings);
                pipeline.GetSSAO().halfRes = gc.halfRes;
                pipeline.GetSSGI()->OnResize(config.windowWidth, config.windowHeight);
                pipeline.GetSSR()->OnResize(config.windowWidth, config.windowHeight);
                pipeline.GetSSAO().OnResize(config.windowWidth, config.windowHeight);
            }
            auto& gc2 = pipeline.GetGIConfig();
            ImGui::SliderFloat("GI 强度", &gc2.giIntensity, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("AO 强度", &gc2.aoIntensity, 0.0f, 1.5f, "%.2f");

            // GI — IBL
            auto* gi = pipeline.GetGI();
            if (gi) {
                ImGui::SeparatorText("GI — IBL");
                auto settings = gi->GetSettings();
                float intensity = settings.intensity;
                if (ImGui::SliderFloat("IBL 强度", &intensity, 0.0f, 3.0f, "%.2f")) {
                    settings.intensity = intensity;
                    gi->SetSettings(settings);
                }
                auto gdbg = gi->GetDebugData();
                ImGui::Text("耗时 %.2f ms", gdbg.avgRenderTimeMs);
            }

            // GI — SSGI（屏幕空间间接漫反射）
            if (auto* ssgi = pipeline.GetSSGI()) {
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
                ImGui::Text("耗时 %.2f ms", ssgi->GetDebugData().avgRenderTimeMs);
            }

            // GI — DDGI（探针网格动态 GI）
            if (auto* ddgi = pipeline.GetDDGI()) {
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
                ImGui::Text("耗时 %.2f ms", ddgi->GetDebugData().avgRenderTimeMs);
            }

            // GI — SSR（屏幕空间反射）
            if (auto* ssr = pipeline.GetSSR()) {
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
                ImGui::Text("耗时 %.2f ms", ssr->GetDebugData().avgRenderTimeMs);
            }

            // ── AutoExposure ──
            ImGui::SeparatorText("AutoExposure");
            {
                auto& ae = pipeline.GetAutoExposure();
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

            // ── 后处理（仅保留与 GI/AO 相关的 SSAO）──
            ImGui::SeparatorText("后处理");
            {
                bool ssaoOn = pipeline.GetSSAO().enabled;
                if (ImGui::Checkbox("SSAO（环境光遮蔽）", &ssaoOn))
                    pipeline.GetSSAO().enabled = ssaoOn;
            }

            // ── GPU Profiler ──
            ImGui::SeparatorText("GPU Profiler");
            auto& pdata = pipeline.GetProfiler().GetLastFrameData();
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
            world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent&) { meshCount++; });
            world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight&) { dirLightCount++; });
            world.ForEach<he::SpotLight>([&](he::Entity, he::SpotLight&) { spotCount++; });
            ImGui::Text("%u 网格  |  %u 方向光  |  %u 点光  |  %u 聚光", meshCount, dirLightCount, 1, spotCount);
        }
        ImGui::End();

        // GPU Profiler 面板（按 F1 切换）
        if (ImGui::IsKeyPressed(ImGuiKey_F1))
            pipeline.GetProfilerPanel().Toggle();
        pipeline.GetProfilerPanel().Draw();

        imgui.EndFrame(cmdList.get());
        cmdList->EndRenderPass();
        cmdList->End();

        device->Submit(cmdList.get());
        pipeline.FlushComputeWork();  // AsyncCompute: Graphics Submit 之后提交 Compute 工作
        swapchain->Present(true);
        frameIndex++;
    }

    // 清理
    imgui.Shutdown();
    device->WaitIdle();
    pipeline.Shutdown();

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
        out["clustered"]    = std::to_string(pipeline.GetClusteredShading().enabled ? 1 : 0);
        out["gpu_cull"]     = std::to_string(pipeline.GetGPUCulling().enabled ? 1 : 0);
        out["gbuffer_mode"] = std::to_string((int)pipeline.GetGBufferMode());

        // ── AutoExposure ──
        auto& ae = pipeline.GetAutoExposure();
        out["ae_enabled"]     = std::to_string(ae.IsEnabled() ? 1 : 0);
        out["ae_adapt_speed"] = std::to_string(ae.GetAdaptSpeed());
        out["ae_target_lum"]  = std::to_string(ae.GetTargetLum());


        // ── GI ──
        if (auto* gi = pipeline.GetGI()) {
            out["ibl_intensity"] = std::to_string(gi->GetSettings().intensity);
        }
        if (auto* ssgi = pipeline.GetSSGI()) {
            out["ssgi_enabled"]   = std::to_string(ssgi->IsEnabled() ? 1 : 0);
            out["ssgi_radius"]    = std::to_string(ssgi->radius);
            out["ssgi_samples"]   = std::to_string(ssgi->sampleCount);
            out["ssgi_intensity"] = std::to_string(ssgi->GetSettings().intensity);
        }
        if (auto* ddgi = pipeline.GetDDGI()) {
            out["ddgi_enabled"]   = std::to_string(ddgi->IsEnabled() ? 1 : 0);
            out["ddgi_blend"]     = std::to_string(ddgi->blendAlpha);
            out["ddgi_scale"]     = std::to_string(ddgi->debugScale);
            out["ddgi_intensity"] = std::to_string(ddgi->GetSettings().intensity);
        }
        if (auto* ssr = pipeline.GetSSR()) {
            out["ssr_enabled"]   = std::to_string(ssr->IsEnabled() ? 1 : 0);
            out["ssr_max_steps"] = std::to_string(ssr->maxSteps);
            out["ssr_step_size"] = std::to_string(ssr->stepSize);
        }

        // ── SSAO ──
        out["ssao_enabled"] = std::to_string(pipeline.GetSSAO().enabled ? 1 : 0);

        SaveConfigFile(g_ConfigPath, out);
        HE_CORE_INFO("配置已保存: {}", g_ConfigPath);
    }

    HE_CORE_INFO("04.Deferred 退出 ({} 帧)", frameIndex);
    return 0;
}
