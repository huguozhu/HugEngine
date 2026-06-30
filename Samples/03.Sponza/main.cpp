// ============================================================
// 03.Sponza — 加载 Sponza glTF 场景，自由相机漫游
//
// 使用 glTFLoader (cgltf) 加载完整的 Sponza 场景，
// PBR 前向管线渲染，支持：
//   WASD = 移动, 右键拖拽 = 旋转视角
//   Shift = 加速, E/Q = 上升/下降
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Render/Pipeline/ForwardPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/SphereComponent.h"
#include "Scene/SkyboxComponent.h"
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
static String g_ConfigPath = String(HUGE_CONTENT_DIR) + "Config/03_Sponza.cfg";

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
        // 去除末尾 \r
        if (!val.empty() && val.back() == '\r') val.pop_back();
        map[key] = val;
    }
    return map;
}

static void SaveConfigFile(const String& path,
                            const std::unordered_map<String, String>& map) {
    // 确保目录存在
    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);

    std::ofstream f(path);
    if (!f.is_open()) return;
    for (auto& [k, v] : map) {
        f << k << "=" << v << "\n";
    }
}

// 辅助：从 map 读取 float 值
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
    config.appName      = "HugEngine — 03.Sponza";
    config.windowWidth  = 960;
    config.windowHeight = 540;
    config.enableVSync  = true;
    config.logLevel     = LogLevel::Info;   // 启动时仅显示 Info

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

    // Content 目录由 CMake 编译定义提供（解决不同工作目录下的路径问题）
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
        mainDL->intensity    = GetFloat(cfgData, "light_intensity", 15.0f); // 增强光照使纹理更可见
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

    // --- 添加点光源（测试点光阴影）---
    Entity pointLightEntity;
    Entity pointLightSphereEntity;  // 可视化球体
    {
        pointLightEntity = world.CreateEntity("PointLight");
        world.AddComponent<TransformComponent>(pointLightEntity);
        auto* pl = world.AddComponent<PointLight>(pointLightEntity);
        pl->color      = float3(1.0f, 0.85f, 0.6f);  // 暖色
        pl->intensity  = 20.0f;
        pl->range      = 600.0f;
        pl->castShadow = true;
        pl->shadowBias = 0.005f;

        // 通过 Transform 设置位置
        auto* plTransform = world.GetComponent<TransformComponent>(pointLightEntity);
        if (plTransform) {
            plTransform->position = float3(
                GetFloat(cfgData, "point_pos_x", -300.0f),
                GetFloat(cfgData, "point_pos_y", 100.0f),
                GetFloat(cfgData, "point_pos_z", 0.0f));
        }

        sceneGraph.SetParent(pointLightEntity, Entity{kInvalidEntity});

        // 创建可视化球体（在点光源位置显示一个小球，随光源一起移动）
        pointLightSphereEntity = world.CreateEntity("PointLightSphere");
        world.AddComponent<TransformComponent>(pointLightSphereEntity);
        // AddComponent 会调用 OnCreate() 生成默认半径 0.5 的几何体
        auto* sphere = world.AddComponent<SphereComponent>(pointLightSphereEntity);
        // 设置目标参数后重新生成几何体（旧缓冲区会被 unique_ptr 自动释放）
        sphere->radius       = 15.0f;   // Sponza 场景较大，球体半径 15 单位
        sphere->segmentCount = 16;      // 低面数，仅用于可视化
        sphere->ringCount    = 8;
        sphere->OnCreate();             // 用新参数重建几何体

        // 初始位置与点光源一致
        auto* sphereTransform = world.GetComponent<TransformComponent>(pointLightSphereEntity);
        if (sphereTransform && plTransform) {
            sphereTransform->position = plTransform->position;
        }

        sceneGraph.SetParent(pointLightSphereEntity, Entity{kInvalidEntity});
    }

    // --- 天空盒（从 skybox.hdr 生成 Cubemap）---
    {
        // 加载 HDR 全景图（stbi_loadf 返回 float 数据）
        String hdrPath = String(HUGE_CONTENT_DIR) + "Textures/skybox.hdr";
        int hdrW, hdrH, hdrCh;
        float* hdrData = stbi_loadf(hdrPath.c_str(), &hdrW, &hdrH, &hdrCh, 4);
        if (!hdrData) {
            HE_CORE_WARN("skybox.hdr 加载失败: {}", hdrPath);
        } else {
            HE_CORE_INFO("skybox.hdr: {}×{} ({} channels)", hdrW, hdrH, hdrCh);

            const u32 faceSize = 512;  // 每面分辨率
            const u32 faceBytes = faceSize * faceSize * 4;
            std::vector<u8> allFaces(faceBytes * 6);

            // Cubemap 6 面方向（Vulkan 标准）
            struct { float3 dir; float3 up; float3 right; } faces[6] = {
                {{ 1, 0, 0}, {0,-1, 0}, {0, 0,-1}},  // +X
                {{-1, 0, 0}, {0,-1, 0}, {0, 0, 1}},  // -X
                {{ 0, 1, 0}, {0, 0, 1}, {1, 0, 0}},  // +Y
                {{ 0,-1, 0}, {0, 0,-1}, {1, 0, 0}},  // -Y
                {{ 0, 0, 1}, {0,-1, 0}, {1, 0, 0}},  // +Z
                {{ 0, 0,-1}, {0,-1, 0}, {-1,0, 0}},  // -Z
            };

            for (u32 f = 0; f < 6; ++f) {
                u8* faceData = allFaces.data() + f * faceBytes;
                for (u32 y = 0; y < faceSize; ++y) {
                    for (u32 x = 0; x < faceSize; ++x) {
                        // 像素 → NDC [-1,1]
                        float u = (2.0f * x / faceSize) - 1.0f;
                        float v = (2.0f * y / faceSize) - 1.0f;
                        // 世界空间方向
                        float3 dir = glm::normalize(
                            faces[f].dir + faces[f].right * u + faces[f].up * v);
                        // 方向 → Equirectangular UV
                        float eqU = (std::atan2(dir.z, dir.x) / (2.0f * 3.14159265f)) + 0.5f;
                        float eqV = (std::asin(glm::clamp(dir.y, -1.0f, 1.0f)) / 3.14159265f) + 0.5f;
                        // 双线性采样 HDR（简单最近邻以避免边界计算）
                        int px = static_cast<int>(eqU * hdrW) % hdrW;
                        int py = static_cast<int>(eqV * hdrH) % hdrH;
                        if (px < 0) px += hdrW;
                        if (py < 0) py += hdrH;
                        float* src = hdrData + (py * hdrW + px) * 4;
                        // HDR float → u8（Reinhard tonemap）
                        auto tonemap = [](float c) {
                            c = c / (1.0f + c);  // Reinhard
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

    HE_CORE_INFO("场景就绪: {} 实体", world.GetEntityCount());

    // ============================================================
    // 4.5 加载 glTF 纹理（stb_image → RHI Texture → MeshComponent）
    // 纹理缓存在 main() 作用域存活，MeshComponent 使用裸指针引用
    // ============================================================
    // 纹理缓存（main 作用域存活，MeshComponent 裸指针引用）
    std::unordered_map<String,
        std::pair<std::unique_ptr<rhi::IRHITexture>,
                  std::unique_ptr<rhi::IRHISampler>>> g_TexCache;

    // 加载一张纹理（自动去重缓存）
    auto loadTexture = [&](const String& uri) -> std::pair<rhi::IRHITexture*, rhi::IRHISampler*> {
        if (uri.empty()) return {nullptr, nullptr};
        String texPath = (std::filesystem::path(sponzaPath).parent_path() / uri).string();
        auto it = g_TexCache.find(texPath);
        if (it == g_TexCache.end()) {
            int w, h, ch;
            u8* pixels = stbi_load(texPath.c_str(), &w, &h, &ch, 4);
            if (!pixels) { HE_CORE_WARN("纹理加载失败: {}", texPath); return {nullptr, nullptr}; }
            // 计算 mipmap 层级数
            u32 maxDim = static_cast<u32>(std::max(w, h));
            u32 mipLevels = 1;
            while (maxDim > 1) { maxDim >>= 1; ++mipLevels; }

            rhi::TextureDesc td; td.format=rhi::Format::RGBA8_UNORM;
            td.width=static_cast<u32>(w); td.height=static_cast<u32>(h);
            td.mipLevels = mipLevels;
            td.usage=rhi::TextureUsage::ShaderResource
                   | rhi::TextureUsage::TransferSrc
                   | rhi::TextureUsage::TransferDst;  // mipmap 生成需要 blit
            td.initialData=pixels;
            auto t = device->CreateTexture(td);
            rhi::SamplerDesc sd; sd.minFilter=rhi::FilterMode::Linear;
            sd.magFilter=rhi::FilterMode::Linear;
            sd.mipFilter=rhi::FilterMode::Linear;  // mipmap 线性过渡
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
        u32 texCount = 0;
        world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent& mesh) {
            auto [bcTex, bcSamp] = loadTexture(mesh.baseColorTexture);
            mesh.SetBaseColorTexture(bcTex, bcSamp);
            auto [nTex, nSamp] = loadTexture(mesh.normalTexture);
            mesh.SetNormalTexture(nTex, nSamp);
            auto [mrTex, mrSamp] = loadTexture(mesh.metallicRoughnessTexture);
            mesh.SetMetallicRoughnessTexture(mrTex, mrSamp);
            auto [aoTex, aoSamp] = loadTexture(mesh.occlusionTexture);
            mesh.SetOcclusionTexture(aoTex, aoSamp);
            texCount++;
        });
        HE_CORE_INFO("纹理加载完成: {} primitive, {} 张独立纹理", texCount, g_TexCache.size());
    }

    // ============================================================
    // 5. 初始化前向管线
    // ============================================================
    render::ForwardPipeline pipeline;
    pipeline.Initialize(device.get());

    // 为每个 primitive 创建独立描述符集（纹理已写入，渲染时只 bind 不 update）
    u32 descSetOk = 0, descSetFail = 0;
    world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent& mesh) {
        auto set = pipeline.CreateTextureDescriptorSet(
            mesh.GetBaseColorGPUTexture(), mesh.GetBaseColorGPUSampler(),
            mesh.GetNormalGPUTexture(), mesh.GetNormalGPUSampler(),
            mesh.GetMetallicRoughnessGPUTexture(), mesh.GetMetallicRoughnessGPUSampler(),
            mesh.GetOcclusionGPUTexture(), mesh.GetOcclusionGPUSampler());
        mesh.SetDescriptorSet(set);
        if (set != rhi::kInvalidSet) descSetOk++; else descSetFail++;
    });
    HE_CORE_INFO("描述符集分配: {} 成功, {} 失败", descSetOk, descSetFail);

    // ============================================================
    // 6. 创建命令列表
    // ============================================================
    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());
    cmdList->SetPipeline(pipeline.GetPipelineState());

    // ============================================================
    // 7. ImGui 初始化
    // ============================================================
    GLFWwindow* glfwWin = engine.GetWindow()->GetNativeHandle();

    editor::ImGuiIntegration imgui;
    imgui.Initialize(glfwWin, device.get(), swapchain.get());

    // ============================================================
    // 8. 相机 — 从配置文件加载，否则使用默认位置
    // ============================================================
    render::CameraData camera;
    camera.up = float3(0.0f, 1.0f, 0.0f);
    camera.SetAspectRatio(
        static_cast<float>(swapchain->GetWidth()),
        static_cast<float>(swapchain->GetHeight()));

    float yaw, pitch;
    if (hasConfig) {
        camera.position = float3(
            GetFloat(cfgData, "cam_pos_x", 0.0f),
            GetFloat(cfgData, "cam_pos_y", 3.0f),
            GetFloat(cfgData, "cam_pos_z", 0.0f));
        yaw   = GetFloat(cfgData, "cam_yaw", -1.57f);
        pitch = GetFloat(cfgData, "cam_pitch", -0.1f);
    } else {
        camera.position = float3(0.0f, 3.0f, 0.0f);
        yaw   = -1.57f;
        pitch = -0.1f;
    }

    // 从 yaw/pitch 计算 forward
    {
        float3 fwd;
        fwd.x = cos(pitch) * sin(yaw);
        fwd.y = sin(pitch);
        fwd.z = -cos(pitch) * cos(yaw);
        camera.forward = glm::normalize(fwd);
    }

    // 鼠标状态
    bool   rightMouseDown = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    float  moveSpeed  = 72.0f;     // Sponza 场景较大，快速移动
    float  lookSpeed  = 0.003f;

    // ============================================================
    // 9. 窗口调整回调
    // ============================================================
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        // 窗口最小化时尺寸为 0，跳过所有重建（恢复时 GLFW 会再次回调）
        if (w == 0 || h == 0) return;
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        pipeline.ResizeHDRTarget(w, h);
        camera.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // ============================================================
    // 10. 主渲染循环
    // ============================================================
    HE_CORE_INFO("03.Sponza 启动 — WASD=移动, 右键拖拽=旋转, Shift=加速, E/Q=升降");
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
            // 右键拖拽旋转
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

                yaw   += dx * lookSpeed;
                pitch -= dy * lookSpeed;
                pitch  = glm::clamp(pitch, -1.5f, 1.5f);
            }

            // 移动速度
            float speed = moveSpeed * deltaTime;
            if (glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
                speed *= 3.0f;

            // WASD + E/Q 移动
            float3 right = glm::normalize(glm::cross(camera.forward, camera.up));
            float3 move  = float3(0.0f);
            if (glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS) move += camera.forward;
            if (glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS) move -= camera.forward;
            if (glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS) move -= right;
            if (glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS) move += right;
            if (glfwGetKey(glfwWin, GLFW_KEY_E) == GLFW_PRESS) move += camera.up;
            if (glfwGetKey(glfwWin, GLFW_KEY_Q) == GLFW_PRESS) move -= camera.up;

            if (glm::dot(move, move) > 0.0001f) {
                move = glm::normalize(move) * speed;
                camera.position += move;
            }
        }

        // 更新相机朝向
        float3 forward;
        forward.x = cos(pitch) * sin(yaw);
        forward.y = sin(pitch);
        forward.z = -cos(pitch) * cos(yaw);
        camera.forward = glm::normalize(forward);

        // --- 渲染 ---
        cmdList->Begin();

        // 阴影通道（在主渲染通道之前执行，离屏渲染到 ShadowMap）
        {
            std::vector<const he::LightComponent*> shadowLights;
            std::vector<render::GPUShadowData> shadowGPUData;
            pipeline.CollectShadowLights(world, sceneGraph, shadowLights, shadowGPUData);

            // 分离方向光和点光源
            std::vector<const he::LightComponent*> dirLights, pointLights;
            std::vector<render::GPUShadowData> dirShadowData, pointShadowData;
            for (usize i = 0; i < shadowLights.size(); ++i) {
                if (shadowLights[i]->type == he::LightType::Directional) {
                    dirLights.push_back(shadowLights[i]);
                    dirShadowData.push_back(shadowGPUData[i]);
                } else if (shadowLights[i]->type == he::LightType::Point) {
                    pointLights.push_back(shadowLights[i]);
                    pointShadowData.push_back(shadowGPUData[i]);
                }
            }

            // 方向光阴影
            if (!dirShadowData.empty()) {
                pipeline.BeginShadowPass(cmdList.get());
                pipeline.RenderShadowPass(cmdList.get(), world, sceneGraph,
                                         dirLights, dirShadowData);
                pipeline.EndShadowPass(cmdList.get());
            }

            // 点光源阴影（Cubemap 6 面）
            if (!pointShadowData.empty()) {
                pipeline.RenderPointShadowPass(cmdList.get(), world, sceneGraph,
                                              pointLights, pointShadowData);
            }
        }

        // --- HDR 离屏渲染通道（RGBA16_FLOAT）---
        pipeline.BeginHDRPass(cmdList.get(),
            swapchain->GetWidth(), swapchain->GetHeight());
        pipeline.BeginFrame(cmdList.get(),
            swapchain->GetWidth(), swapchain->GetHeight());
        pipeline.RenderScene(cmdList.get(), world, sceneGraph, camera);
        // Skybox 在场景之后渲染（depth=Equal，仅在空白区域绘制）
        pipeline.RenderSkybox(cmdList.get(), world, camera);
        pipeline.EndHDRPass(cmdList.get());

        // --- ToneMap 后处理 + ImGui（输出到 SwapChain B8G8R8A8）---
        cmdList->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
        pipeline.RenderToneMapPass(cmdList.get());

        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("03.Sponza 控制面板");
        {
            // ============================================================
            // 性能监控
            // ============================================================
            float fps = 1.0f / (deltaTime > 0.001f ? deltaTime : 0.016f);
            ImGui::TextColored({0.3f, 1.0f, 0.3f, 1.0f}, "FPS: %.0f", fps);
            ImGui::SameLine(120);
            ImGui::TextColored({0.6f, 0.6f, 0.6f, 1.0f}, "(%.2f ms)", deltaTime * 1000.0f);

            // ============================================================
            // 相机
            // ============================================================
            ImGui::SeparatorText("相机");
            ImGui::DragFloat3("位置##Camera", &camera.position[0], 5.0f);
            float yawDeg   = glm::degrees(yaw);
            float pitchDeg = glm::degrees(pitch);
            if (ImGui::SliderFloat("Yaw", &yawDeg, -180.0f, 180.0f, "%.1f°"))
                yaw = glm::radians(yawDeg);
            if (ImGui::SliderFloat("Pitch", &pitchDeg, -85.0f, 85.0f, "%.1f°"))
                pitch = glm::radians(pitchDeg);
            ImGui::DragFloat("移动速度", &moveSpeed, 1.0f, 1.0f, 500.0f, "%.0f");

            // ============================================================
            // 光源
            // ============================================================
            world.ForEach<he::DirectionalLight>([&](he::Entity e, he::DirectionalLight& dl) {
                bool isMain = (e == mainLightEntity);
                ImGui::SeparatorText(isMain ? "主方向光" : "补光");

                // 启用开关
                ImGui::Checkbox(isMain ? "启用##MainDL" : "启用##FillDL", &dl.enabled);

                // 方向（滑块修改后自动归一化）
                float3 dir = dl.direction;
                if (ImGui::SliderFloat3("方向", &dir[0], -1.0f, 1.0f, "%.2f")) {
                    if (glm::dot(dir, dir) > 0.0001f)
                        dl.direction = glm::normalize(dir);
                }

                // 颜色
                ImGui::ColorEdit3("颜色", &dl.color[0]);

                // 强度
                ImGui::DragFloat("强度", &dl.intensity, 0.1f, 0.0f, 100.0f, "%.1f");

                // 阴影参数（仅主方向光显示）
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

            // ============================================================
            // 点光源
            // ============================================================
            world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
                ImGui::SeparatorText("点光源");
                ImGui::Checkbox("启用##PointEnable", &pl.enabled);
                auto* plTransform = world.GetComponent<TransformComponent>(e);
                if (plTransform) {
                    if (ImGui::DragFloat3("位置##PointLight", &plTransform->position[0], 5.0f)) {
                        // 同步球体位置
                        auto* sphereTransform = world.GetComponent<TransformComponent>(
                            pointLightSphereEntity);
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

            // 确保每次渲染前球体位置与点光源同步（处理非 ImGui 的位置更新）
            {
                auto* plTransform = world.GetComponent<TransformComponent>(pointLightEntity);
                auto* sphereTransform = world.GetComponent<TransformComponent>(pointLightSphereEntity);
                if (plTransform && sphereTransform)
                    sphereTransform->position = plTransform->position;
            }

            // ============================================================
            // 场景统计
            // ============================================================
            ImGui::SeparatorText("场景");
            u32 meshCount = 0, dirLightCount = 0;
            world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent&) {
                meshCount++;
            });
            world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight&) {
                dirLightCount++;
            });
            ImGui::Text("%u 网格  |  %u 方向光  |  %u 点光 + 可视化球体",
                meshCount, dirLightCount, 1);
        }
        ImGui::End();
        imgui.EndFrame(cmdList.get());

        cmdList->EndRenderPass();
        cmdList->End();

        device->Submit(cmdList.get());
        swapchain->Present(true);
        frameIndex++;
    }

    // 清理
    imgui.Shutdown();
    device->WaitIdle();
    pipeline.Shutdown();

    // ============================================================
    // 保存配置（相机 + 方向光参数）
    // ============================================================
    {
        std::unordered_map<String, String> out;
        // 相机
        out["cam_pos_x"]  = std::to_string(camera.position.x);
        out["cam_pos_y"]  = std::to_string(camera.position.y);
        out["cam_pos_z"]  = std::to_string(camera.position.z);
        out["cam_yaw"]    = std::to_string(yaw);
        out["cam_pitch"]  = std::to_string(pitch);

        // 主方向光
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

        // 补光也保存（方便调整）
        world.ForEach<he::DirectionalLight>([&](he::Entity e, he::DirectionalLight& l) {
            if (e == mainLightEntity) return;  // 跳过主光
            out["fill_dir_x"]     = std::to_string(l.direction.x);
            out["fill_dir_y"]     = std::to_string(l.direction.y);
            out["fill_dir_z"]     = std::to_string(l.direction.z);
            out["fill_color_r"]   = std::to_string(l.color.x);
            out["fill_color_g"]   = std::to_string(l.color.y);
            out["fill_color_b"]   = std::to_string(l.color.z);
            out["fill_intensity"] = std::to_string(l.intensity);
        });

        // 点光源位置
        auto* plTransform = world.GetComponent<TransformComponent>(pointLightEntity);
        if (plTransform) {
            out["point_pos_x"] = std::to_string(plTransform->position.x);
            out["point_pos_y"] = std::to_string(plTransform->position.y);
            out["point_pos_z"] = std::to_string(plTransform->position.z);
        }

        SaveConfigFile(g_ConfigPath, out);
        HE_CORE_INFO("配置已保存: {}", g_ConfigPath);
    }

    HE_CORE_INFO("03.Sponza 退出 ({} 帧)", frameIndex);
    return 0;
}
