// ============================================================
// 04.Deferred — Sponza 场景 + 延迟渲染管线
//
// 与 03.Sponza 相同的 Sponza 场景（glTF 加载 + 纹理 + Skybox），
// 但使用 DeferredPipeline（GBuffer + 全屏 Lighting Pass）替代前向管线。
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/DeferredPipeline.h"
#include "Pipeline/CameraController.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/SphereComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/AnimationComponent.h"
#include "Asset/glTFLoader.h"
#include "Asset/BindlessTextureManager.h"
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
static String g_ConfigPath = String(HUGE_CONTENT_DIR) + "Config/04_Deferred.cfg";

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
    // 1. 引擎启动
    // ============================================================
    EngineConfig config;
    config.appName      = "HugEngine — 04.Deferred (Sponza)";
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
        mainDL->castShadow   = GetInt(cfgData, "shadow_enabled", 1) != 0;
        mainDL->shadowBias   = GetFloat(cfgData, "shadow_bias", 0.003f);
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
        sceneGraph.SetParent(lightEntity, Entity{kInvalidEntity});
    }

    // --- 添加点光源 + 可视化球体 ---
    Entity pointLightEntity, pointLightSphereEntity;
    {
        pointLightEntity = world.CreateEntity("PointLight");
        world.AddComponent<TransformComponent>(pointLightEntity);
        auto* pl = world.AddComponent<PointLight>(pointLightEntity);
        pl->color      = float3(1.0f, 0.85f, 0.6f);
        pl->intensity  = 20.0f;
        pl->range      = 600.0f;
        pl->castShadow = true;
        pl->shadowBias = 0.005f;

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
        sl->color           = float3(1.0f, 0.9f, 0.7f);
        sl->intensity       = 80.0f;
        sl->range           = 1200.0f;
        sl->innerConeAngle  = 0.25f;  // ~14°
        sl->outerConeAngle  = 0.50f;  // ~29°
        sl->direction       = float3(0.0f, -1.0f, 0.3f);  // 向下倾斜
        sl->castShadow      = true;
        sl->shadowBias      = 0.005f;

        auto* slTransform = world.GetComponent<TransformComponent>(spotLightEntity);
        if (slTransform) {
            slTransform->position = float3(0.0f, 200.0f, -200.0f);
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

            rhi::TextureDesc td; td.format=rhi::Format::RGBA8_UNORM;
            td.width=static_cast<u32>(w); td.height=static_cast<u32>(h);
            td.mipLevels = mipLevels;
            td.usage=rhi::TextureUsage::ShaderResource
                   | rhi::TextureUsage::TransferSrc
                   | rhi::TextureUsage::TransferDst;
            td.initialData=pixels;
            auto t = device->CreateTexture(td);
            rhi::SamplerDesc sd; sd.minFilter=rhi::FilterMode::Linear;
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
        rhi::TextureDesc td; td.format=rhi::Format::RGBA8_UNORM;
        td.width=1; td.height=1; td.mipLevels=1;
        td.usage=rhi::TextureUsage::ShaderResource; td.initialData=white;
        auto defaultTex = device->CreateTexture(td);
        rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
        sd.addressU=sd.addressV=rhi::AddressMode::Repeat;
        auto defaultSamp = device->CreateSampler(sd);
        he::asset::BindlessTextureManager::Instance().SetDefaultTexture(
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
            u32 matID = he::asset::BindlessTextureManager::Instance().RegisterMaterial(
                bcTex, bcSamp, nTex, nSamp, mrTex, mrSamp, aoTex, aoSamp);
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
        pipeline.OnResize(w, h);
        camCtrl.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // ============================================================
    // 11. 主渲染循环
    // ============================================================
    HE_CORE_INFO("04.Deferred (Sponza) 启动 — WASD=移动, 右键拖拽=旋转, Shift=加速, E/Q=升降");
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
            rhi::Format::Unknown, nullptr, rhi::IRHICommandList::LoadOp::Load);

        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("04.Deferred (Sponza)");
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
            }

            // ── GBuffer 模式 ──
            ImGui::SeparatorText("GBuffer 渲染模式");
            int gbMode = (int)pipeline.GetGBufferMode();
            ImGui::RadioButton("CPU Driven", &gbMode, 0); ImGui::SameLine();
            ImGui::RadioButton("GPU Driven (ExecuteIndirect)", &gbMode, 1);
            if (gbMode != (int)pipeline.GetGBufferMode())
                pipeline.SetGBufferMode((render::DeferredPipeline::GBufferMode)gbMode);

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

            // ── ColorGrading ──
            ImGui::SeparatorText("色彩分级");
            {
                auto& cg = pipeline.GetColorGrading();
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
                bool ssaoOn = pipeline.GetSSAO().enabled;
                if (ImGui::Checkbox("SSAO", &ssaoOn))
                    pipeline.GetSSAO().enabled = ssaoOn;

                // Bloom
                auto& bloom = pipeline.GetBloom();
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
                auto& dof = pipeline.GetDOF();
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
                auto& mb = pipeline.GetMotionBlur();
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
                bool msaaOn = pipeline.IsMSAAEnabled();
                if (ImGui::Checkbox("MSAA 4x (硬件)", &msaaOn)) {
                    pipeline.EnableMSAA(msaaOn);
                }
                if (msaaOn) {
                    ImGui::SameLine();
                    ImGui::TextColored({0.5f, 1.0f, 0.5f, 1.0f}, "(重启生效)");
                }

                // LDR 抗锯齿（SMAA 与 FXAA 互斥二选一，与 MSAA 可叠加）
                bool smaaOn = pipeline.IsSMAAEnabled();
                bool fxaaOn = pipeline.IsFXAAEnabled();
                int ldrAA = smaaOn ? 1 : (fxaaOn ? 2 : 0);
                if (ImGui::Combo("LDR 抗锯齿", &ldrAA, "关闭\0SMAA\0FXAA\0")) {
                    pipeline.EnableSMAA(ldrAA == 1);
                    pipeline.EnableFXAA(ldrAA == 2);
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
            world.ForEach<he::DirectionalLight>([&](he::Entity e, he::DirectionalLight& dl) {
                bool isMain = (e == mainLightEntity);
                ImGui::SeparatorText(isMain ? "主方向光" : "补光");
                ImGui::Checkbox(isMain ? "启用##MainDL" : "启用##FillDL", &dl.enabled);

                float3 dir = dl.direction;
                if (ImGui::SliderFloat3(isMain ? "方向##MainDL" : "方向##FillDL", &dir[0], -1.0f, 1.0f, "%.2f")) {
                    if (glm::dot(dir, dir) > 0.0001f)
                        dl.direction = glm::normalize(dir);
                }
                ImGui::ColorEdit3(isMain ? "颜色##MainDL" : "颜色##FillDL", &dl.color[0]);
                ImGui::DragFloat(isMain ? "强度##MainDL" : "强度##FillDL", &dl.intensity, 0.1f, 0.0f, 100.0f, "%.1f");

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
            world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
                ImGui::SeparatorText("点光源");
                ImGui::Checkbox("启用##PointEnable", &pl.enabled);
                auto* plTransform = world.GetComponent<TransformComponent>(e);
                if (plTransform) {
                    if (ImGui::DragFloat3("位置##PointLight", &plTransform->position[0], 5.0f)) {
                        auto* sphereTransform = world.GetComponent<TransformComponent>(pointLightSphereEntity);
                        if (sphereTransform)
                            sphereTransform->position = plTransform->position;
                    }
                }
                ImGui::ColorEdit3("颜色", &pl.color[0]);
                ImGui::DragFloat("强度", &pl.intensity, 0.1f, 0.0f, 200.0f, "%.1f");
                ImGui::DragFloat("范围", &pl.range, 10.0f, 10.0f, 5000.0f, "%.0f");
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

            // 同步球体位置
            {
                auto* plTransform = world.GetComponent<TransformComponent>(pointLightEntity);
                auto* sphereTransform = world.GetComponent<TransformComponent>(pointLightSphereEntity);
                if (plTransform && sphereTransform)
                    sphereTransform->position = plTransform->position;
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
            u32 meshCount = 0, dirLightCount = 0;
            world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent&) { meshCount++; });
            world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight&) { dirLightCount++; });
            ImGui::Text("%u 网格  |  %u 方向光  |  %u 点光", meshCount, dirLightCount, 1);
        }
        ImGui::End();
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
    // 保存配置
    // ============================================================
    {
        std::unordered_map<String, String> out;
        out["cam_pos_x"]  = std::to_string(camCtrl.GetCamera().position.x);
        out["cam_pos_y"]  = std::to_string(camCtrl.GetCamera().position.y);
        out["cam_pos_z"]  = std::to_string(camCtrl.GetCamera().position.z);
        out["cam_yaw"]   = std::to_string(camCtrl.GetYaw());
        out["cam_pitch"] = std::to_string(camCtrl.GetPitch());
        out["cam_near"]  = std::to_string(camCtrl.GetCamera().nearPlane);
        out["cam_far"]   = std::to_string(camCtrl.GetCamera().farPlane);

        if (mainDL) {
            out["light_dir_x"]     = std::to_string(mainDL->direction.x);
            out["light_dir_y"]     = std::to_string(mainDL->direction.y);
            out["light_dir_z"]     = std::to_string(mainDL->direction.z);
            out["light_color_r"]   = std::to_string(mainDL->color.x);
            out["light_color_g"]   = std::to_string(mainDL->color.y);
            out["light_color_b"]   = std::to_string(mainDL->color.z);
            out["light_intensity"] = std::to_string(mainDL->intensity);
            out["shadow_enabled"]  = std::to_string(mainDL->castShadow ? 1 : 0);
            out["shadow_bias"]     = std::to_string(mainDL->shadowBias);
        }

        world.ForEach<he::DirectionalLight>([&](he::Entity e, he::DirectionalLight& l) {
            if (e == mainLightEntity) return;
            out["fill_dir_x"]     = std::to_string(l.direction.x);
            out["fill_dir_y"]     = std::to_string(l.direction.y);
            out["fill_dir_z"]     = std::to_string(l.direction.z);
            out["fill_color_r"]   = std::to_string(l.color.x);
            out["fill_color_g"]   = std::to_string(l.color.y);
            out["fill_color_b"]   = std::to_string(l.color.z);
            out["fill_intensity"] = std::to_string(l.intensity);
        });

        auto* plTransform = world.GetComponent<TransformComponent>(pointLightEntity);
        if (plTransform) {
            out["point_pos_x"] = std::to_string(plTransform->position.x);
            out["point_pos_y"] = std::to_string(plTransform->position.y);
            out["point_pos_z"] = std::to_string(plTransform->position.z);
        }

        SaveConfigFile(g_ConfigPath, out);
        HE_CORE_INFO("配置已保存: {}", g_ConfigPath);
    }

    HE_CORE_INFO("04.Deferred 退出 ({} 帧)", frameIndex);
    return 0;
}
