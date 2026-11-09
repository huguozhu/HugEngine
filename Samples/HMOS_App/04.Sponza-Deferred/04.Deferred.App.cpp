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
#include "Platform/IWindow.h"
#include "Platform/IVfs.h"
#include "Platform/IInput.h"
#include "RHI/FramePresenter.h"
#include "Pipeline/DeferredPipeline.h"
#include "Pipeline/CameraController.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/SkyboxComponent.h"
#include "Asset/glTFLoader.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include "CameraInput.h"

// STB_IMAGE_STATIC：实现留在本 TU，避免与其它 App 的 stb 实现重复定义
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

namespace he::samples::sponza04 {

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

    /// 天空盒组件（面板里可开关；鸿蒙上默认关，用于排查过曝）
    SkyboxComponent* skybox = nullptr;

    /// HDR 全景图 → 6 面立方体贴图（与 03 同一套算法）
    void CreateSkybox() {
        const String hdrPath = vfs::Resolve("Textures/skybox.hdr");
        int hdrW = 0, hdrH = 0, hdrCh = 0;
        float* hdrData = stbi_loadf(hdrPath.c_str(), &hdrW, &hdrH, &hdrCh, 4);
        if (!hdrData) {
            HE_CORE_WARN("04.App: skybox.hdr 加载失败: {}", hdrPath);
            return;
        }
        HE_CORE_INFO("04.App: skybox.hdr {}x{}（{} 通道）", hdrW, hdrH, hdrCh);

        const u32 faceSize  = 512;
        const u32 faceBytes = faceSize * faceSize * 4;
        std::vector<u8> allFaces(faceBytes * 6);

        struct Face { float3 dir, up, right; };
        const Face faces[6] = {
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
                    const float u = (2.0f * x / faceSize) - 1.0f;
                    const float v = (2.0f * y / faceSize) - 1.0f;
                    const float3 dir = glm::normalize(faces[f].dir + faces[f].right * u + faces[f].up * v);
                    const float eqU = (std::atan2(dir.z, dir.x) / (2.0f * 3.14159265f)) + 0.5f;
                    const float eqV = (std::asin(glm::clamp(dir.y, -1.0f, 1.0f)) / 3.14159265f) + 0.5f;
                    int px = static_cast<int>(eqU * hdrW) % hdrW;
                    int py = static_cast<int>(eqV * hdrH) % hdrH;
                    if (px < 0) px += hdrW;
                    if (py < 0) py += hdrH;
                    const float* src = hdrData + (py * hdrW + px) * 4;
                    auto tonemap = [](float c) {
                        c = c / (1.0f + c);   // Reinhard
                        return static_cast<u8>(glm::clamp(c, 0.0f, 1.0f) * 255.0f);
                    };
                    const usize idx = (y * faceSize + x) * 4;
                    faceData[idx + 0] = tonemap(src[0]);
                    faceData[idx + 1] = tonemap(src[1]);
                    faceData[idx + 2] = tonemap(src[2]);
                    faceData[idx + 3] = 255;
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
        cmSamp.minFilter = rhi::FilterMode::Linear;
        cmSamp.magFilter = rhi::FilterMode::Linear;
        cmSamp.addressU = cmSamp.addressV = cmSamp.addressW = rhi::AddressMode::ClampToEdge;
        auto cmSampler = device->CreateSampler(cmSamp);

        Entity skyEntity = world.CreateEntity("Skybox");
        world.AddComponent<TransformComponent>(skyEntity);
        auto* skyComp = world.AddComponent<SkyboxComponent>(skyEntity);
        skybox = skyComp;
        skyComp->SetCubemap(std::move(cubemap), std::move(cmSampler));

        sceneGraph->SetParent(skyEntity, Entity{kInvalidEntity});
    }

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
    const String sponzaPath = vfs::Resolve("gltf/Sponza/glTF/Sponza.gltf");
    HE_CORE_INFO("04.App: 加载 Sponza: {}", sponzaPath);
    auto loadResult = asset::LoadGLTF(g.world, *g.sceneGraph, sponzaPath);
    if (!loadResult.success) {
        HE_CORE_ERROR("04.App: Sponza 加载失败: {}", loadResult.error);
        return false;
    }
    HE_CORE_INFO("04.App: Sponza 加载完成: {} 实体, {} 网格图元",
                 loadResult.entities.size(), loadResult.meshCount);

    // ---- 灯光 ----
    {
        Entity e = g.world.CreateEntity("DirectionalLight");
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
        g.world.AddComponent<TransformComponent>(e);
        auto* dl = g.world.AddComponent<DirectionalLight>(e);
        dl->direction = float3(-0.3f, -0.4f, -0.5f);
        dl->color     = float3(0.6f, 0.7f, 0.9f);
        dl->intensity = 2.0f;
        g.sceneGraph->SetParent(e, Entity{kInvalidEntity});
    }

    g.CreateSkybox();
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

    if (opt.enableImGui) {
        g.imgui = std::make_unique<editor::ImGuiIntegration>();
        g.imgui->Initialize(engine.GetWindow(), g.device.get(), g.swapchain.get());
    }

    g.ready = true;
    HE_CORE_INFO("04.App 初始化完成（{}x{}）", g.swapchain->GetWidth(), g.swapchain->GetHeight());
    return true;
}

bool App::Frame(f32 deltaTime, const InputState& input) {
    Impl& g = *m_Impl;
    if (!g.ready || !g.presenter) return false;

    g.camInput.Update(input, g.camera, deltaTime);

    if (!g.presenter->BeginFrame()) return false;
    g.presenter->BeginRecording(g.device.get(), g.cmdList.get());

    // 【与 04 完整样例一致】延迟管线自己负责阴影资源的逐帧绑定与 pass 顺序，
    // 样例侧只需要：NextFrame → AssembleScene → Render
    g.pipeline->NextFrame();
    g.pipeline->GetFrameAssembler().AssembleScene(g.world, *g.sceneGraph, g.camera.GetCamera());

    // GBuffer → Lighting → ToneMap（ToneMap 写后缓冲）
    g.pipeline->Render(g.cmdList.get(), g.pipeline->GetFrameSnapshot(), g.camera.GetCamera());

    // ImGui：LOAD 保留 ToneMap 输出；颜色格式取交换链实际格式（手机上是 RGBA8）
    g.cmdList->BeginRenderPass(1, g.swapchain->GetColorFormat(),
                               rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);

    if (g.imgui && g.imgui->IsInitialized()) {
        g.imgui->BeginFrame();
        const float ui = ImGui::GetFontSize() / 16.0f;
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(300.0f * ui, 140.0f * ui), ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("04.Sponza-Deferred (App)");
        ImGui::Text("FPS: %.0f", 1.0f / (deltaTime > 0.0f ? deltaTime : 0.016f));
        ImGui::Text("GBuffer + Lighting");
        ImGui::End();
        g.imgui->EndFrame(g.cmdList.get());
    }

    g.cmdList->EndRenderPass();
    g.presenter->EndRecording();
    g.presenter->EndFrame(true);
    return true;
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
    g.texCache.clear();
    g.sceneGraph.reset();
    g.swapchain.reset();
    g.device.reset();
    rhi::SetDevice(nullptr);
    g.ready = false;
    HE_CORE_INFO("04.App 已关闭");
}

} // namespace he::samples::sponza04
