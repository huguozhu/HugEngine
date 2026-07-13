// ============================================================
// 02.Cube — HugEngine PBR 前向渲染管线演示
//
// 使用逐物体 Push Constants 的 PBR 渲染：
//   Engine → RHI Vulkan → ForwardPipeline → 场景遍历
//
// 创建多个带不同材质的立方体，展示 PBR 效果。
// 自由相机：WASD + 鼠标右键旋转
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/ForwardPipeline.h"
#include "Pipeline/CameraController.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/Transform.h"
#include "Scene/SkyboxComponent.h"
#include "Editor/ImGuiIntegration.h"
#include "Asset/BindlessTextureManager.h"
#include "imgui.h"

// RT 着色器 SPIR-V（Phase 2：复用 Sponza RT 着色器）
#include "RT_Sponza.rgen.spv.h"
#include "RT_Sponza.rmiss.spv.h"
#include "RT_Sponza.rchit.spv.h"
#include "RT_Bindless.rcall.spv.h"

#include <cmath>
#include <cstring>
#include <vector>
#include <fstream>
#include <filesystem>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace he;

// ============================================================
// 相机配置读写（简易 key=value 格式）
// ============================================================
static String g_ConfigPath = String(HUGE_CONTENT_DIR) + "Config/02_Cube.cfg";

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
    for (auto& [k, v] : map)
        f << k << "=" << v << "\n";
}

static float GetFloat(const std::unordered_map<String, String>& m,
                      const String& key, float def = 0.0f) {
    auto it = m.find(key);
    return (it != m.end()) ? std::stof(it->second) : def;
}

// ============================================================
// 辅助：创建带材质的形状实体
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

// ============================================================
// 主函数
// ============================================================
int main() {
    // --- 1. 引擎启动 ---
    EngineConfig config;
    config.appName      = "HugEngine — PBR Forward Pipeline";
    config.windowWidth  = 960;
    config.windowHeight = 500;
    config.enableVSync  = true;

    Engine engine(config);
    engine.Initialize();

    // --- 2. 创建 RHI 设备 ---
    rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    auto device = rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    rhi::SetDevice(device.get());

    // --- 3. 创建 SwapChain ---
    auto swapchain = device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width  = engine.GetWindow()->GetWidth(),
        .height = engine.GetWindow()->GetHeight(),
        .vsync  = true,
    });

    // --- 4. 初始化场景 ---
    World world;
    SceneGraph sceneGraph(world);

    // 地板（深灰色立方体，粗糙，长宽 2x）
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, -1.5f, 0.0f), float3(20.0f, 0.2f, 20.0f),
        float4(0.3f, 0.3f, 0.35f, 1.0f), 0.0f, 0.9f);

    // 金球（金属，光滑）
    CreateShapeEntity(world, sceneGraph,
        float3(-1.5f, 2.0f, 0.0f), float3(0.8f),
        float4(1.0f, 0.72f, 0.0f, 1.0f), 1.0f, 0.15f, true);

    // 铜球（金属，中度粗糙）
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, 3.0f, 0.0f), float3(0.8f),
        float4(0.85f, 0.45f, 0.2f, 1.0f), 0.95f, 0.4f, true);

    // 蓝色塑料立方体（非金属，光滑）
    CreateShapeEntity(world, sceneGraph,
        float3(1.5f, 3.0f, 0.0f), float3(0.8f),
        float4(0.2f, 0.5f, 1.0f, 1.0f), 0.0f, 0.2f);

    // 红色橡胶立方体（非金属，粗糙）
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, 2.0f, 1.5f), float3(0.7f),
        float4(0.9f, 0.15f, 0.1f, 1.0f), 0.0f, 0.85f);

    // 白色陶瓷球
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, 1.2f, -1.5f), float3(0.6f),
        float4(0.95f, 0.93f, 0.88f, 1.0f), 0.0f, 0.35f, true);

    // --- 方向光 ---
    Entity mainLightEntity;
    DirectionalLight* mainDL = nullptr;
    {
        mainLightEntity = world.CreateEntity("DirectionalLight");
        world.AddComponent<TransformComponent>(mainLightEntity);
        mainDL = world.AddComponent<DirectionalLight>(mainLightEntity);
        mainDL->direction = float3(0.5f, -1.0f, 1.0f);
        mainDL->color     = float3(1.0f, 0.95f, 0.85f);
        mainDL->intensity = 5.0f;
        mainDL->castShadow = true;
        mainDL->shadowBias = 0.003f;
        sceneGraph.SetParent(mainLightEntity, Entity{kInvalidEntity});
    }

    // --- 点光源（投射阴影）---
    Entity pointLightEntity;
    Entity pointLightSphere;  // 可视化球体
    {
        pointLightEntity = world.CreateEntity("PointLight");
        world.AddComponent<TransformComponent>(pointLightEntity);
        auto* pl = world.AddComponent<PointLight>(pointLightEntity);
        pl->color      = float3(1.0f, 0.6f, 0.3f);  // 暖橙色
        pl->intensity  = 30.0f;
        pl->range      = 15.0f;
        pl->castShadow = true;
        pl->shadowBias = 0.005f;

        auto* plTransform = world.GetComponent<TransformComponent>(pointLightEntity);
        if (plTransform) plTransform->position = float3(2.0f, 4.0f, 3.0f);
        sceneGraph.SetParent(pointLightEntity, Entity{kInvalidEntity});

        // 可视化球体
        pointLightSphere = world.CreateEntity("PointLightSphere");
        world.AddComponent<TransformComponent>(pointLightSphere);
        auto* sphere = world.AddComponent<SphereComponent>(pointLightSphere);
        sphere->radius = 0.15f;
        sphere->segmentCount = 12;
        sphere->ringCount = 6;
        sphere->OnCreate();
        auto* sphereTransform = world.GetComponent<TransformComponent>(pointLightSphere);
        if (sphereTransform) sphereTransform->position = float3(2.0f, 2.0f, 3.0f);
        sceneGraph.SetParent(pointLightSphere, Entity{kInvalidEntity});
    }

    // --- 天空盒（从 skybox 目录加载 6 面纹理）---
    {
        String skyDir = String(HUGE_CONTENT_DIR) + "Textures/skybox/";
        const char* faceFiles[6] = {
            "daylight0.png", "daylight1.png", "daylight2.png",
            "daylight3.png", "daylight4.png", "daylight5.png",
        };
        std::vector<u8> allFaces;
        u32 faceW = 0, faceH = 0;

        for (u32 f = 0; f < 6; ++f) {
            String path = skyDir + faceFiles[f];
            int w, h, ch;
            u8* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
            if (!pixels) { HE_CORE_WARN("Skybox face {} 加载失败: {}", f, path); break; }
            if (f == 0) { faceW = (u32)w; faceH = (u32)h; }
            usize byteSize = faceW * faceH * 4;
            allFaces.insert(allFaces.end(), pixels, pixels + byteSize);
            stbi_image_free(pixels);
        }

        if (!allFaces.empty()) {
            rhi::TextureDesc cmDesc;
            cmDesc.format=rhi::Format::RGBA8_UNORM; cmDesc.width=faceW; cmDesc.height=faceH;
            cmDesc.mipLevels=1; cmDesc.arrayLayers=6;
            cmDesc.usage=rhi::TextureUsage::ShaderResource|rhi::TextureUsage::Cubemap|rhi::TextureUsage::TransferDst;
            cmDesc.initialData=allFaces.data();
            auto cm = device->CreateTexture(cmDesc);
            rhi::SamplerDesc s; s.minFilter=s.magFilter=rhi::FilterMode::Linear;
            s.addressU=s.addressV=s.addressW=rhi::AddressMode::ClampToEdge;
            auto cs = device->CreateSampler(s);
            Entity e = world.CreateEntity("Skybox");
            world.AddComponent<TransformComponent>(e);
            auto* sc = world.AddComponent<SkyboxComponent>(e);
            sc->SetCubemap(std::move(cm), std::move(cs));
            sceneGraph.SetParent(e, Entity{kInvalidEntity});
        }
    }

    HE_CORE_INFO("Scene created: {} entities", world.GetEntityCount());

    // --- 5. 初始化前向管线 ---
    render::ForwardPipeline pipeline;
    pipeline.Initialize(device.get());
    pipeline.SetUseRenderGraph(false);
    pipeline.SetSwapChain(swapchain.get());
    pipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());

    // ============================================================
    // 5.5 RT 路径初始化
    // ============================================================
    bool rtSupported = device->GetCaps().supportsRayTracing;
    he::render::RTPass rtPass;
    rhi::DescriptorSetLayoutHandle rtLayout0 = rhi::kInvalidLayout;

    if (rtSupported) {
        // set=0: TLAS + BackBuffer + Bindless（Callable shader 需要）
        rhi::DescriptorSetLayoutDesc rtSet0Desc;
        rtSet0Desc.bindings = {
            { 0, rhi::DescriptorType::AccelerationStructure, 1,    0x100 },
            { 1, rhi::DescriptorType::StorageImage,          1,    0x100 },
            { 5, rhi::DescriptorType::SampledImage,          4096, 0x2000, true },  // Callable bindless
            { 6, rhi::DescriptorType::Sampler,               4096, 0x2000, true },  // Callable bindless
        };
        rtLayout0 = device->CreateDescriptorSetLayout(rtSet0Desc);

        // set=1: 材质纹理 + 光源 UB（ClosestHit）
        rhi::DescriptorSetLayoutHandle rtLayout1;
        rhi::DescriptorSetLayoutDesc rtSet1Desc;
        rtSet1Desc.bindings = {
            { 0, rhi::DescriptorType::SampledImage,  1, 0x40 },  // 材质纹理 (3×N)
            { 1, rhi::DescriptorType::UniformBuffer, 1, 0x40 },  // 光源 UB
        };
        rtLayout1 = device->CreateDescriptorSetLayout(rtSet1Desc);

        rhi::ShaderBytecode rgen{ rhi::ShaderStage::RayGen,
            k_RT_Sponza_rgen_spv, {}, "main" };
        rhi::ShaderBytecode rmiss{ rhi::ShaderStage::Miss,
            k_RT_Sponza_rmiss_spv, {}, "main" };
        rhi::ShaderBytecode rchit{ rhi::ShaderStage::ClosestHit,
            k_RT_Sponza_rchit_spv, {}, "main" };
        rhi::ShaderBytecode rcall{ rhi::ShaderStage::Callable,
            k_RT_Bindless_rcall_spv, {}, "main" };

        std::vector<rhi::RTShaderGroup> rtGroups = {
            { rhi::RTShaderGroupType::RayGen,   0, ~0u, ~0u, ~0u, "RayGen" },
            { rhi::RTShaderGroupType::Miss,     1, ~0u, ~0u, ~0u, "Miss"   },
            { rhi::RTShaderGroupType::Hit,     ~0u, 2,   ~0u, ~0u, "Hit"    },
            { rhi::RTShaderGroupType::Callable, 3, ~0u, ~0u, ~0u, "BindlessCall" },
        };

        rhi::PushConstantRange pcRange;
        pcRange.stageMask = 0x100;
        pcRange.offset    = 0;
        pcRange.size      = 96;

        std::vector<rhi::DescriptorSetLayoutHandle> rtLayouts = { rtLayout0, rtLayout1 };
        rtPass.Initialize(device.get(), { rgen, rmiss, rchit, rcall }, rtGroups,
                         rtLayouts, pcRange);

        // 创建 RT 资源（材质纹理 + 光源 UB）
        rtPass.CreateMaterialTexture(device.get(), 256, world);
        rtPass.CreateLightBuffer(device.get(), 8);

        // 注册到 BindlessTextureManager（Callable shader 需要 bindless 纹理）
        he::asset::BindlessTextureManager::Instance().RegisterDescriptorSet(
            device.get(), rtPass.GetDescriptorSet0(), 5, 6);

        HE_CORE_INFO("02.Cube RT: {}", rtPass.IsValid() ? "就绪" : "不可用");
    }

    // --- 6. 创建命令列表 ---
    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());
    cmdList->SetPipeline(pipeline.GetPipelineState());

    // --- 6.5. 获取 GLFW 窗口句柄 ---
    GLFWwindow* glfwWin = engine.GetWindow()->GetNativeHandle();

    // --- 6.6. ImGui ---
    editor::ImGuiIntegration imgui;
    imgui.Initialize(glfwWin, device.get(), swapchain.get());

    // --- 7. 相机 ---
    render::CameraController camCtrl;
    camCtrl.SetAspectRatio(
        static_cast<float>(swapchain->GetWidth()),
        static_cast<float>(swapchain->GetHeight()));

    // 从配置文件加载相机状态
    auto cfgData = LoadConfigFile(g_ConfigPath);
    if (!cfgData.empty()) {
        camCtrl.SetPosition(float3(
            GetFloat(cfgData, "cam_pos_x", 0.0f),
            GetFloat(cfgData, "cam_pos_y", 3.0f),
            GetFloat(cfgData, "cam_pos_z", 0.0f)));
        camCtrl.SetOrientation(
            GetFloat(cfgData, "cam_yaw", -1.57f),
            GetFloat(cfgData, "cam_pitch", -0.1f));
        HE_CORE_INFO("加载相机配置: {}", g_ConfigPath);
    }

    // 鼠标状态
    bool   rightMouseDown = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;

    // --- 8. 窗口调整回调 ---
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (w == 0 || h == 0) return;
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        pipeline.OnResize(w, h);
        camCtrl.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // --- 9. 主循环 ---
    HE_CORE_INFO("02.Cube demo started — WASD=移动, 右键拖拽=旋转, 滚轮=缩放, Shift=加速");
    u64  frameIndex = 0;
    f64  lastTime   = glfwGetTime();
    int  renderMode  = 0;  // 0=光栅化, 1=RT

    while (!engine.GetWindow()->ShouldClose()) {
        // 计算帧时间
        f64 now       = glfwGetTime();
        f32 deltaTime = static_cast<f32>(now - lastTime);
        lastTime      = now;

        engine.GetWindow()->PollEvents();

        if (!swapchain->AcquireNextImage())
            continue;

        // ============================================================
        // 相机控制
        // ============================================================
        {
            // --- 鼠标旋转（右键拖拽）---
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

                camCtrl.Rotate(dx * 0.003f, -dy * 0.003f);  // 上推鼠标(dy<0) → pitch增大
            }

            // --- 键盘移动 ---
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

        // 渲染一帧
        cmdList->Begin();
        pipeline.NextFrame();

        // 阴影子系统：CPU 端数据收集（GPU 渲染在 RenderGraph 的 Shadow Pass）
        {
            auto* shadowSys = pipeline.GetShadowSystem();
            shadowSys->SetRenderResources(
                pipeline.GetCurrentShadowObjectBuffer(),  // 阴影专用 Object Buffer
                pipeline.GetCurrentShadowBuffer(),
                pipeline.GetCurrentDescSet());

            render::SubsystemContext shadowCtx;
            shadowCtx.world       = &world;
            shadowCtx.sceneGraph  = &sceneGraph;
            shadowCtx.camera      = &camCtrl.GetCamera();
            shadowSys->Update(shadowCtx);
        }

        // 统一使用 pipeline.Render()（RG/非RG 内部均包含 Shadow + HDR + 场景 + 天空盒）
        pipeline.Render(cmdList.get(), world, sceneGraph, camCtrl.GetCamera());

        // --- ToneMap + ImGui / RT 路径分支 ---
        if (renderMode == 1 && rtPass.IsValid()) {
            // RT 路径：光追直写 BackBuffer（覆盖光栅化输出）
            rtPass.BuildAS(cmdList.get(), world, sceneGraph);
            rtPass.UpdateLightBuffer(pipeline.GetCurrentLightBuffer());
            rtPass.UpdateRTDescriptorSet(device.get(),
                swapchain->GetCurrentBackBufferView(),
                pipeline.GetCurrentObjectBuffer());

            // Push Constants：相机数据
            struct RTPushConstant {
                float4x4 invViewProj;
                float4   camPosNearFar;
                u32      sampleCount;
                u32      frameIndex;
                u32      _pad0;
                u32      _pad1;
            };
            RTPushConstant rtPC;
            const auto& camData = camCtrl.GetCamera();
            rtPC.invViewProj = glm::inverse(camData.GetViewProjMatrix());
            rtPC.camPosNearFar = float4(camData.position.x, camData.position.y,
                                        camData.position.z, camData.nearPlane);
            rtPC.sampleCount = 1;
            rtPC.frameIndex  = 0;

            // BackBuffer 屏障 → RT 可写
            cmdList->PipelineBarrier(
                rhi::PipelineStage::BottomOfPipe,
                rhi::PipelineStage::RayTracingShader,
                rhi::ResourceState::Undefined,
                rhi::ResourceState::UnorderedAccess);

            rtPass.BindPipeline(cmdList.get());
            rtPass.BindDescriptorSets(cmdList.get());
            cmdList->SetPushConstants(0, sizeof(RTPushConstant), &rtPC);
            rtPass.TraceRays(cmdList.get(),
                swapchain->GetWidth(), swapchain->GetHeight());

            // BackBuffer 屏障 → RenderTarget（准备 ImGui）
            cmdList->PipelineBarrier(
                rhi::PipelineStage::RayTracingShader,
                rhi::PipelineStage::ColorAttachmentOutput,
                rhi::ResourceState::UnorderedAccess,
                rhi::ResourceState::RenderTarget);

            cmdList->SetPipeline(pipeline.GetPipelineState());
            cmdList->BeginRenderPass(1, rhi::Format::BGRA8_UNORM,
                rhi::Format::Unknown, nullptr,
                rhi::IRHICommandList::LoadOp::Load);
        } else {
            // 光栅化路径（不变）
            cmdList->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
            pipeline.RenderToneMapPass(cmdList.get());
        }

        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("HugEngine");
        ImGui::Text("FPS: %.0f", 1.0f / (deltaTime > 0 ? deltaTime : 0.016f));
        // ============================================================
        // 相机信息（可编辑）
        // ============================================================
        ImGui::SeparatorText("相机");
        auto& cam = camCtrl.GetCamera();

        // 位置
        ImGui::DragFloat3("位置", &cam.position[0], 1.0f);

        // 旋转（Yaw / Pitch 角度制）
        float yawDeg   = glm::degrees(camCtrl.GetYaw());
        float pitchDeg = glm::degrees(camCtrl.GetPitch());
        bool yawChanged   = ImGui::SliderFloat("Yaw",   &yawDeg,   -180.0f, 180.0f, "%.1f°");
        bool pitchChanged = ImGui::SliderFloat("Pitch", &pitchDeg, -89.0f,  89.0f,  "%.1f°");
        if (yawChanged || pitchChanged)
            camCtrl.SetOrientation(glm::radians(yawDeg), glm::radians(pitchDeg));

        // 朝向 / 上方向（只读，由 Yaw/Pitch 导出）
        ImGui::Text("朝向: (%.2f, %.2f, %.2f)", cam.forward.x, cam.forward.y, cam.forward.z);

        // FOV、裁剪面
        ImGui::SliderFloat("FOV", &cam.fov, 10.0f, 120.0f, "%.0f°");
        ImGui::DragFloat("近裁剪面", &cam.nearPlane, 0.01f, 0.001f, 10.0f, "%.3f",
            ImGuiSliderFlags_Logarithmic);
        ImGui::DragFloat("远裁剪面", &cam.farPlane, 10.0f, 10.0f, 50000.0f, "%.0f");

        // 宽高比 / 移动速度
        ImGui::Text("宽高比: %.2f", cam.aspectRatio);
        float speed = camCtrl.GetMoveSpeed();
        if (ImGui::DragFloat("移动速度", &speed, 1.0f, 0.1f, 500.0f, "%.1f"))
            camCtrl.SetMoveSpeed(speed);

        // 渲染模式切换
        ImGui::SeparatorText("渲染模式");
        if (rtSupported) {
            ImGui::RadioButton("光栅化 (ForwardPipeline)", &renderMode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Ray Tracing", &renderMode, 1);
        } else {
            ImGui::TextColored({0.5f, 0.5f, 0.5f, 1.0f}, "RT 不可用（设备不支持）");
        }

        // 渲染选项
        bool useRG = pipeline.UseRenderGraph();
        if (ImGui::Checkbox("RenderGraph", &useRG))
            pipeline.SetUseRenderGraph(useRG);

        // GI 控制
        auto* gi = pipeline.GetGI();
        if (gi) {
            ImGui::SeparatorText("GI");
            auto settings = gi->GetSettings();
            ImGui::Text("Mode: %s", settings.mode == render::GIMode::IBL ? "IBL" : "None");
            float intensity = settings.intensity;
            if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 3.0f, "%.2f")) {
                settings.intensity = intensity;
                gi->SetSettings(settings);
            }
        }

        // 方向光控制
        if (mainDL) {
            ImGui::SeparatorText("方向光");

            ImGui::Checkbox("启用##DLEnabled", &mainDL->enabled);

            float3 dir = mainDL->direction;
            if (ImGui::SliderFloat3("方向##DLDir", &dir[0], -1.0f, 1.0f, "%.2f")) {
                if (glm::dot(dir, dir) > 0.0001f)
                    mainDL->direction = glm::normalize(dir);
            }

            ImGui::ColorEdit3("颜色##DLColor", &mainDL->color[0]);

            ImGui::DragFloat("强度##DLIntensity", &mainDL->intensity, 0.1f, 0.0f, 50.0f, "%.1f");

            bool shadowOn = mainDL->castShadow;
            if (ImGui::Checkbox("投射阴影##DLShadow", &shadowOn))
                mainDL->castShadow = shadowOn;

            if (mainDL->castShadow) {
                ImGui::Indent(12.0f);
                ImGui::DragFloat("深度偏移##DLBias", &mainDL->shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::DragFloat("法线偏移##DLNormalBias", &mainDL->shadowNormalBias, 0.001f, 0.0f, 0.5f, "%.3f");
                ImGui::SliderFloat("阴影强度##DLShadowStr", &mainDL->shadowStrength, 0.0f, 1.0f, "%.2f");
                ImGui::Unindent(12.0f);
            }
        }

        // 点光源控制
        world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
            ImGui::SeparatorText("点光源");

            ImGui::Checkbox("启用##PTEnabled", &pl.enabled);

            auto* plTransform = world.GetComponent<TransformComponent>(e);
            if (plTransform) {
                if (ImGui::DragFloat3("位置##PTPos", &plTransform->position[0], 0.1f)) {
                    // 同步可视化球体位置
                    auto* sphereTransform = world.GetComponent<TransformComponent>(pointLightSphere);
                    if (sphereTransform)
                        sphereTransform->position = plTransform->position;
                }
            }

            ImGui::ColorEdit3("颜色##PTColor", &pl.color[0]);
            ImGui::DragFloat("强度##PTIntensity", &pl.intensity, 0.5f, 0.0f, 200.0f, "%.1f");
            ImGui::DragFloat("范围##PTRange", &pl.range, 0.5f, 0.5f, 50.0f, "%.1f");

            bool ptShadow = pl.castShadow;
            if (ImGui::Checkbox("投射阴影##PTShadow", &ptShadow))
                pl.castShadow = ptShadow;

            if (pl.castShadow) {
                ImGui::Indent(12.0f);
                ImGui::DragFloat("深度偏移##PTBias", &pl.shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::Unindent(12.0f);
            }
        });

        ImGui::End();
        imgui.EndFrame(cmdList.get());
        cmdList->EndRenderPass();  // 关闭 ImGui RP（RG 和 non-RG 都需要）
        cmdList->End();

        device->Submit(cmdList.get());
        swapchain->Present(true);
        frameIndex++;

        // 每秒更新窗口标题，显示 FPS
        static f64  titleTimer  = 0.0;
        static u64  titleFrame  = 0;
        titleTimer += deltaTime;
        titleFrame++;
        if (titleTimer >= 0.5) {
            f64 fps = static_cast<f64>(titleFrame) / titleTimer;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "HugEngine — PBR | FPS: %.0f | Pos: (%.1f, %.1f, %.1f) "
                "| 右键拖拽旋转 WASD移动",
                fps,
                camCtrl.GetCamera().position.x, camCtrl.GetCamera().position.y, camCtrl.GetCamera().position.z);
            glfwSetWindowTitle(glfwWin, buf);
            titleTimer = 0.0;
            titleFrame = 0;
        }
    }

    // 保存相机配置
    {
        std::unordered_map<String, String> out;
        out["cam_pos_x"] = std::to_string(camCtrl.GetCamera().position.x);
        out["cam_pos_y"] = std::to_string(camCtrl.GetCamera().position.y);
        out["cam_pos_z"] = std::to_string(camCtrl.GetCamera().position.z);
        out["cam_yaw"]   = std::to_string(camCtrl.GetYaw());
        out["cam_pitch"] = std::to_string(camCtrl.GetPitch());
        SaveConfigFile(g_ConfigPath, out);
        HE_CORE_INFO("相机配置已保存: {}", g_ConfigPath);
    }

    // 清理
    imgui.Shutdown();
    device->WaitIdle();

    // RT 资源清理
    rtPass.Shutdown();
    if (rtLayout0 != rhi::kInvalidLayout)
        device->DestroyDescriptorSetLayout(rtLayout0);

    pipeline.Shutdown();

    HE_CORE_INFO("Exiting after {} frames", frameIndex);
    return 0;
}
