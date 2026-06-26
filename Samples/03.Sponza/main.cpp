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
#include "Asset/glTFLoader.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

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
            rhi::TextureDesc td; td.format=rhi::Format::RGBA8_UNORM;
            td.width=static_cast<u32>(w); td.height=static_cast<u32>(h);
            td.usage=rhi::TextureUsage::ShaderResource; td.initialData=pixels;
            auto t = device->CreateTexture(td);
            rhi::SamplerDesc sd; sd.minFilter=rhi::FilterMode::Linear;
            sd.magFilter=rhi::FilterMode::Linear;
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
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
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
        cmdList->BeginRenderPass(1, rhi::Format::RGBA8_UNORM);

        pipeline.BeginFrame(cmdList.get(),
            swapchain->GetWidth(), swapchain->GetHeight());
        pipeline.RenderScene(cmdList.get(), world, sceneGraph, camera);

        // ImGui（在同一渲染通道内绘制 — ImGui RP 现已与 Forward RP 兼容）
        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("03.Sponza — 性能监控");
        {
            // --- FPS + 帧时间 ---
            float fps = 1.0f / (deltaTime > 0.001f ? deltaTime : 0.016f);
            ImGui::TextColored({0.3f, 1.0f, 0.3f, 1.0f}, "FPS: %.0f", fps);
            ImGui::SameLine(120);
            ImGui::TextColored({0.6f, 0.6f, 0.6f, 1.0f}, "(%.2f ms)", deltaTime * 1000.0f);

            // --- 相机 ---
            ImGui::SeparatorText("相机");
            ImGui::Text("位置: (%.1f, %.1f, %.1f)",
                camera.position.x, camera.position.y, camera.position.z);
            ImGui::Text("朝向: (%.2f, %.2f, %.2f)",
                camera.forward.x, camera.forward.y, camera.forward.z);
            ImGui::Text("Yaw: %.1f°  Pitch: %.1f°",
                glm::degrees(yaw), glm::degrees(pitch));

            // --- 方向光 ---
            ImGui::SeparatorText("方向光");
            world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& dl) {
                ImGui::Text("方向: (%.2f, %.2f, %.2f)",
                    dl.direction.x, dl.direction.y, dl.direction.z);
                ImGui::Text("颜色: (%.2f, %.2f, %.2f)",
                    dl.color.x, dl.color.y, dl.color.z);
                ImGui::Text("强度: %.1f", dl.intensity);

                // 阴影状态
                if (dl.castShadow) {
                    ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f},
                        "阴影: ON  (%ux%u, bias=%.4f)",
                        dl.shadowMapSize, dl.shadowMapSize, dl.shadowBias);
                } else {
                    ImGui::TextColored({0.5f, 0.5f, 0.5f, 1.0f}, "阴影: OFF");
                }
            });

            // --- 场景统计 ---
            ImGui::SeparatorText("场景");
            u32 meshCount = 0, dirLightCount = 0;
            world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent&) {
                meshCount++;
            });
            world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight&) {
                dirLightCount++;
            });
            ImGui::Text("%u 网格  |  %u 方向光  |  1 阴影投射",
                meshCount, dirLightCount);
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

        SaveConfigFile(g_ConfigPath, out);
        HE_CORE_INFO("配置已保存: {}", g_ConfigPath);
    }

    HE_CORE_INFO("03.Sponza 退出 ({} 帧)", frameIndex);
    return 0;
}
