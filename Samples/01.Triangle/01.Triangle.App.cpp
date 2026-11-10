// ============================================================
// 01.Triangle.App —— `01.Triangle` 的可移植渲染子集
//
// 从桌面 main 抽出：设备/交换链/PSO/RT/Mesh/ImGui/帧外壳都放这里，
// 桌面 main 只负责引擎循环与后端热切换，鸿蒙入口直接驱动 App。
// ============================================================

#include "01.Triangle.App.h"

#include "Core/Platform.h"
#include "Core/Log.h"
#include "Core/Engine.h"
#include "Platform/IInput.h"   // InputState 完整类型（Frame 的形参）
#include "Platform/IWindow.h"
#include "RHI/FramePresenter.h"
#include "Editor/ImGuiIntegration.h"

#include "Triangle.vert.spv.h"
#include "Triangle.vert.dxil.h"
#include "Triangle.vert.dxbc.h"
#include "Triangle.frag.spv.h"
#include "Triangle.frag.dxil.h"
#include "Triangle.frag.dxbc.h"
#include "RT_Triangle.rgen.spv.h"
#include "RT_Triangle.rgen.dxil.h"
#include "RT_Background.rmiss.spv.h"
#include "RT_Background.rmiss.dxil.h"
#include "Triangle.mesh.spv.h"
#include "Triangle.mesh.dxil.h"
#include "TriangleMesh.frag.spv.h"
#include "TriangleMesh.frag.dxil.h"
#include "TriangleMesh.frag.dxbc.h"
// RT 写离屏纹理后，用全屏拷贝着色器贴回后缓冲（D3D12 后缓冲不能当 UAV）
#include "Fullscreen.vert.spv.h"
#include "Fullscreen.vert.dxil.h"
#include "Fullscreen.vert.dxbc.h"
#include "FullscreenCopy.frag.spv.h"
#include "FullscreenCopy.frag.dxil.h"
#include "FullscreenCopy.frag.dxbc.h"
#include "imgui.h"

#include <cstdlib>
#include <iterator>
#include <vector>

namespace he::samples::triangle01 {
namespace {

/// 推送常量：三条渲染路径共用的旋转角（与 shader 的 TrianglePushConstant 一致）
struct RotationPushConstant {
    f32 angle = 0.0f;
};

std::vector<rhi::Backend> AvailableBackends() {
    std::vector<rhi::Backend> list;
    list.push_back(rhi::Backend::Vulkan);
#if defined(_WIN32)
    list.push_back(rhi::Backend::D3D12);
    list.push_back(rhi::Backend::D3D11);
#endif
    return list;
}

const char* BackendName(rhi::Backend b) {
    switch (b) {
        case rhi::Backend::Vulkan: return "Vulkan";
        case rhi::Backend::D3D12:  return "D3D12";
        case rhi::Backend::D3D11:  return "D3D11";
        default:                   return "Unknown";
    }
}

} // namespace

struct App::Impl {
    rhi::Backend backend = rhi::Backend::Vulkan;

    std::unique_ptr<rhi::IRHIDevice>        device;
    std::unique_ptr<rhi::IRHISwapChain>     swapchain;
    std::unique_ptr<rhi::IRHICommandList>   cmdList;
    std::unique_ptr<rhi::IRHIPipelineState> pipeline;
    std::unique_ptr<rhi::IRHIBuffer>        vertexBuffer;

    // ---- 能力（每次重建都重新取）----
    bool rtSupported   = false;
    bool rtRenderable  = false;
    bool meshSupported = false;

    // ---- RT 路径 ----
    std::unique_ptr<rhi::IRHIRayTracingPipelineState> rtPSO;
    std::unique_ptr<rhi::IRHIBuffer>                  rtSBTBuf;
    rhi::SBTDesc                     sbt{};
    rhi::DescriptorSetLayoutHandle   rtLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle         rtSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHITexture> rtTarget;
    u32                              rtTargetW = 0, rtTargetH = 0;
    std::unique_ptr<rhi::IRHISampler> blitSampler;
    rhi::DescriptorSetLayoutHandle    blitLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle          blitSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> blitPSO;

    // ---- Mesh Shader 路径 ----
    std::unique_ptr<rhi::IRHIPipelineState> meshPSO;

    // ---- UI 与帧外壳 ----
    std::unique_ptr<editor::ImGuiIntegration> imgui;
    std::unique_ptr<rhi::FramePresenter>      framePresenter;

    // ---- 帧状态 ----
    bool  ready         = false;
    bool  enableImGui   = true;
    int   renderMode    = 0;        // 0=光栅化 1=RT 2=Mesh
    f32   rotationAngle = 0.0f;
    f32   rotationSpeed = 1.0f;
    rhi::Backend pendingBackend = rhi::Backend::Vulkan;
    bool  hasBackendRequest = false;

    bool EnsureRtTarget(u32 w, u32 h) {
        if (w == 0 || h == 0) return false;
        if (rtTarget && rtTargetW == w && rtTargetH == h) return true;

        rtTarget.reset();
        rhi::TextureDesc td;
        td.width       = w;
        td.height      = h;
        td.mipLevels   = 1;
        td.arrayLayers = 1;
        td.sampleCount = 1;
        // 离屏纹理要能当 UAV：用 RGBA8_UNORM（sRGB/BGRA 存储格式在部分后端不可写）
        td.format = rhi::Format::RGBA8_UNORM;
        td.usage  = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
        rtTarget  = device->CreateTexture(td);
        if (!rtTarget) {
            HE_CORE_ERROR("01.App: 离屏 RT 目标创建失败（{}x{}）", w, h);
            return false;
        }
        rtTargetW = w;
        rtTargetH = h;

        void* view = rtTarget->GetNativeHandle();
        device->UpdateDescriptorSetWithImageView(rtSet, 0, rhi::DescriptorType::StorageImage, view);
        device->UpdateDescriptorSet(blitSet, 0, rhi::DescriptorType::CombinedImageSampler,
                                    rtTarget.get(), blitSampler.get());
        return true;
    }

    int ClampRenderMode(int mode) const {
        if (mode == 1 && !(rtRenderable && rtPSO)) return 0;
        if (mode == 2 && !(meshSupported && meshPSO)) return 0;
        return mode;
    }
};

App::App() : m_Impl(std::make_unique<Impl>()) {}
App::~App() { Shutdown(); }

bool App::IsReady() const { return m_Impl->ready; }
rhi::Backend App::CurrentBackend() const { return m_Impl->backend; }

bool App::TakeBackendRequest(rhi::Backend& out) {
    Impl& g = *m_Impl;
    if (!g.hasBackendRequest || g.pendingBackend == g.backend) return false;
    out = g.pendingBackend;
    g.hasBackendRequest = false;
    return true;
}

void App::SetRenderMode(int mode) {
    m_Impl->renderMode = m_Impl->ClampRenderMode(mode);
}

bool App::Initialize(Engine& engine, rhi::Backend backend, const Options& opt) {
    Impl& g = *m_Impl;
    Shutdown();   // 可重复调用

    g.backend     = backend;
    g.enableImGui = opt.enableImGui;
    HE_CORE_INFO("=== 初始化渲染后端：{} ===", BackendName(backend));

    rhi::DeviceInitDesc desc;
    desc.backend = backend;
    // 切换后端时不重建验证层（每切一次都重建 instance 会明显变慢）
    static bool s_ValidationConsumed = false;
    desc.enableValidation = !s_ValidationConsumed;
    s_ValidationConsumed  = true;
    if (const char* v = std::getenv("HE_TRIANGLE_VALIDATION")) desc.enableValidation = (std::atoi(v) != 0);
#if HE_PLATFORM_OHOS
    desc.enableValidation = false;   // 手机上没有校验层（与 02 的 App 保持一致）
#endif
    desc.windowHandle = engine.GetWindow()->GetNativeWindow();

    g.device = rhi::CreateDevice(desc.backend);
    g.device->Initialize(desc);
    rhi::SetDevice(g.device.get());

    g.rtSupported   = g.device->GetCaps().supportsRayTracing;
    g.meshSupported = g.device->GetCaps().supportsMeshShaders;
    g.rtRenderable  = g.rtSupported;   // RT 写离屏纹理 ⇒ 不再要求后缓冲可写

    const u32 swW = engine.GetWindow()->GetWidth();
    const u32 swH = engine.GetWindow()->GetHeight();
    g.swapchain = g.device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeWindow(),
        .width = swW, .height = swH, .vsync = true,
    });
    if (!g.swapchain) {
        HE_CORE_ERROR("交换链创建失败（后端 {}）", BackendName(backend));
        return false;
    }

    // ---- 光栅化路径 ----
    struct Vertex { float x, y; };
    const Vertex kVertices[] = {
        { 0.0f, 0.5f }, { 0.5f, -0.5f }, {-0.5f, -0.5f },
    };
    g.vertexBuffer = g.device->CreateBuffer({
        .size = sizeof(kVertices), .usage = rhi::BufferUsage::Vertex,
        .initialData = kVertices, .stride = sizeof(Vertex),
    });

    rhi::ShaderBytecode vertShader = rhi::MakeShaderBytecode(
        rhi::ShaderStage::Vertex, k_Triangle_vert_spv, k_Triangle_vert_dxil, k_Triangle_vert_dxbc, "main");
    rhi::ShaderBytecode fragShader = rhi::MakeShaderBytecode(
        rhi::ShaderStage::Pixel, k_Triangle_frag_spv, k_Triangle_frag_dxil, k_Triangle_frag_dxbc, "main");

    g.pipeline = g.device->CreatePipelineState({
        .vertexShader = &vertShader, .pixelShader = &fragShader,
        .vertexLayout = {.stride = sizeof(Vertex)},
        .depthTest = false, .depthWrite = false,
        .colorFormats = { g.swapchain->GetColorFormat() },
        .pushConstantRanges = {{rhi::kStageMaskVertex, 0u, sizeof(RotationPushConstant)}},
        .debugName = "Triangle",
    });
    if (!g.pipeline) {
        HE_CORE_ERROR("光栅化 PSO 创建失败（后端 {}）", BackendName(backend));
        return false;
    }

    // ---- RT 路径 ----
    if (g.rtSupported) {
        rhi::ShaderBytecode rgen = rhi::MakeShaderBytecode(
            rhi::ShaderStage::RayGen, k_RT_Triangle_rgen_spv, k_RT_Triangle_rgen_dxil, "main");
        rhi::ShaderBytecode rmiss = rhi::MakeShaderBytecode(
            rhi::ShaderStage::Miss, k_RT_Background_rmiss_spv, k_RT_Background_rmiss_dxil, "main");

        rhi::DescriptorSetLayoutDesc ldesc;
        ldesc.bindings = {{ 0, rhi::DescriptorType::StorageImage, 1, 0x100 }};
        g.rtLayout = g.device->CreateDescriptorSetLayout(ldesc);
        g.rtSet    = g.device->AllocateDescriptorSet(g.rtLayout);

        std::vector<rhi::RTShaderGroup> groups = {
            {rhi::RTShaderGroupType::RayGen, 0, ~0u, ~0u, ~0u, "RayGen"},
            {rhi::RTShaderGroupType::Miss,   1, ~0u, ~0u, ~0u, "Miss"},
        };
        rhi::RTPipelineStateDesc rtpDesc;
        rtpDesc.shaders           = { rgen, rmiss };
        rtpDesc.shaderGroups     = groups;
        rtpDesc.maxRecursionDepth = 1;
        rtpDesc.descriptorSetLayouts = { g.rtLayout };
        rtpDesc.pushConstantRanges = {{rhi::kStageMaskRayGen, 0u, sizeof(RotationPushConstant)}};
        rtpDesc.debugName = "RT_Triangle";
        g.rtPSO = g.device->CreateRTPipelineState(rtpDesc);
        // 光追是**可选**路径：某后端没实现 RT 时只跳过，不中断样例
        if (!g.rtPSO) {
            HE_CORE_WARN("01.App: RT PSO 不可用（当前后端未实现光追），跳过光追路径");
            g.rtSupported  = false;
            g.rtRenderable = false;
        }

        // SBT —— 三条硬性要求（与引擎 RTPass::BuildSBT 一致）：
        //   1) 承载缓冲必须带 SHADER_BINDING_TABLE 用途；
        //   2) 每个区域起始地址按 shaderGroupBaseAlignment 对齐；
        //   3) 组间步长同样按该对齐向上取整，不能直接用 handleSize。
        if (g.rtPSO) {
            const u32 groupCount = g.rtPSO->GetShaderGroupCount();
            const u32 handleSize = g.rtPSO->GetShaderGroupHandleSize();
            auto handles = g.rtPSO->GetShaderGroupHandles();
            const u32 align = g.device->GetCaps().shaderGroupBaseAlignment
                            ? g.device->GetCaps().shaderGroupBaseAlignment : 1u;
            const u32 stride = (handleSize + align - 1u) & ~(align - 1u);
            const u32 sbtSize = stride * groupCount;

            g.rtSBTBuf = g.device->CreateBuffer({
                .size = sbtSize,
                .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Uniform
                       | rhi::BufferUsage::ShaderBindingTable,
            });
            if (!g.rtSBTBuf) {
                HE_CORE_WARN("01.App: SBT 缓冲创建失败，回落到光栅化");
                g.rtSupported  = false;
                g.rtRenderable = false;
            } else {
                u8* mapped = static_cast<u8*>(g.rtSBTBuf->Map());
                for (u32 gi = 0; gi < groupCount; ++gi) {
                    std::memcpy(mapped + gi * stride, handles.data() + gi * handleSize, handleSize);
                }
                g.rtSBTBuf->Unmap();

                g.sbt.buffer = g.rtSBTBuf.get();
                g.sbt.rayGen.handleOffset = 0;
                g.sbt.rayGen.stride       = stride;
                g.sbt.miss.handleOffset   = stride;
                g.sbt.miss.stride         = stride;
            }
        }

        // ---- 全屏拷贝（RT 离屏目标 → 后缓冲）----
        if (g.rtRenderable) {
            rhi::ShaderBytecode fsVert = rhi::MakeShaderBytecode(
                rhi::ShaderStage::Vertex, k_Fullscreen_vert_spv, k_Fullscreen_vert_dxil, k_Fullscreen_vert_dxbc, "main");
            rhi::ShaderBytecode fsFrag = rhi::MakeShaderBytecode(
                rhi::ShaderStage::Pixel, k_FullscreenCopy_frag_spv, k_FullscreenCopy_frag_dxil,
                k_FullscreenCopy_frag_dxbc, "main");

            rhi::DescriptorSetLayoutDesc bdesc;
            bdesc.bindings = {{ 0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment }};
            g.blitLayout = g.device->CreateDescriptorSetLayout(bdesc);
            g.blitSet    = g.device->AllocateDescriptorSet(g.blitLayout);

            rhi::SamplerDesc sd;
            sd.minFilter = rhi::FilterMode::Linear;
            sd.magFilter = rhi::FilterMode::Linear;
            sd.mipFilter = rhi::FilterMode::Linear;
            sd.addressU = sd.addressV = sd.addressW = rhi::AddressMode::ClampToEdge;
            g.blitSampler = g.device->CreateSampler(sd);

            g.blitPSO = g.device->CreatePipelineState({
                .vertexShader = &fsVert, .pixelShader = &fsFrag,
                .vertexLayout = {},
                .depthTest = false, .depthWrite = false,
                .colorFormats = { g.swapchain->GetColorFormat() },
                .pushConstantRanges = {},
                .descriptorSetLayouts = { g.blitLayout },
                .debugName = "TriangleBlit",
            });
            if (!g.blitPSO) {
                HE_CORE_WARN("01.App: 全屏拷贝 PSO 创建失败，回落到光栅化");
                g.rtRenderable = false;
            } else if (!g.EnsureRtTarget(g.swapchain->GetWidth(), g.swapchain->GetHeight())) {
                HE_CORE_WARN("01.App: 离屏 RT 目标不可用，只提供光栅化 / Mesh 路径");
                g.rtRenderable = false;
            }
        }
    }

    // ---- Mesh Shader 路径 ----
    if (g.meshSupported) {
        // 网格着色器只有 SPIR-V / DXIL（D3D11 不支持网格着色器，故无 DXBC）
        rhi::ShaderBytecode msShader = rhi::MakeShaderBytecode(
            rhi::ShaderStage::Mesh, k_Triangle_mesh_spv, k_Triangle_mesh_dxil, "main");
        rhi::ShaderBytecode msFrag = rhi::MakeShaderBytecode(
            rhi::ShaderStage::Pixel, k_TriangleMesh_frag_spv, k_TriangleMesh_frag_dxil,
            k_TriangleMesh_frag_dxbc, "main");
        // 【必须用 .meshShader，不能塞进 .vertexShader】RHI 的 mesh 管线分支按
        // `desc.meshShader` 判定；塞进 vertexShader 会走传统 IA 管线，
        // vkCreateGraphicsPipelines 报 "pName not found for VK_SHADER_STAGE_VERTEX_BIT"，
        // 得到一个空管线句柄，随后绑定即崩（实测：Vulkan + Mesh Shader 直接 crash）。
        g.meshPSO = g.device->CreatePipelineState({
            .pixelShader = &msFrag,
            .meshShader = &msShader,
            .vertexLayout = {},
            .depthTest = false, .depthWrite = false,
            .colorFormats = { g.swapchain->GetColorFormat() },
            .pushConstantRanges = {{rhi::kStageMaskMesh, 0u, sizeof(RotationPushConstant)}},
            .debugName = "TriangleMesh",
        });
        if (!g.meshPSO) g.meshSupported = false;
    }

    // ---- 命令列表 + 帧外壳 + ImGui ----
    g.cmdList = g.device->CreateCommandList();
    g.cmdList->SetSwapChain(g.swapchain.get());
    g.framePresenter = std::make_unique<rhi::FramePresenter>(g.swapchain.get());

    if (g.enableImGui) {
        g.imgui = std::make_unique<editor::ImGuiIntegration>();
        g.imgui->Initialize(engine.GetWindow(), g.device.get(), g.swapchain.get());
    }

    g.renderMode = g.ClampRenderMode(g.rtRenderable ? 1 : 0);
    if (const char* modeEnv = std::getenv("HE_TRIANGLE_MODE")) {
        g.renderMode = g.ClampRenderMode(std::atoi(modeEnv));
    }
    g.pendingBackend    = backend;
    g.hasBackendRequest = false;
    g.ready             = true;

    HE_CORE_INFO("01.App 初始化完成（后端 {}，{}x{}，模式 {}，ImGui {}）",
                 BackendName(backend), g.swapchain->GetWidth(), g.swapchain->GetHeight(),
                 g.renderMode, g.imgui && g.imgui->IsInitialized() ? "开" : "关");
    return true;
}

void App::Resize(u32 width, u32 height) {
    Impl& g = *m_Impl;
    if (!g.swapchain || !g.cmdList) return;
    g.swapchain->Resize(width, height);
    g.cmdList->SetSwapChain(g.swapchain.get());
    g.rtTargetW = g.rtTargetH = 0;   // 下一帧重建离屏目标
}

bool App::Frame(f32 deltaTime, const InputState& /*input*/) {
    Impl& g = *m_Impl;
    if (!g.ready || !g.framePresenter) return false;
    if (!g.framePresenter->BeginFrame()) return false;

    // 旋转角：2π 取模，长时间运行也不丢精度
    g.rotationAngle += g.rotationSpeed * deltaTime;
    if (g.rotationAngle > 6.28318530718f) g.rotationAngle -= 6.28318530718f;
    const RotationPushConstant rotationPC{ g.rotationAngle };

    const u32 w = g.swapchain->GetWidth();
    const u32 h = g.swapchain->GetHeight();

    g.framePresenter->BeginRecording(g.device.get(), g.cmdList.get());

    // ── RT：写离屏纹理 → 全屏拷贝到后缓冲 ──
    if (g.renderMode == 1 && g.rtRenderable && g.rtPSO && g.blitPSO && g.EnsureRtTarget(w, h)) {
        g.cmdList->PipelineBarrier(rhi::PipelineStage::BottomOfPipe,
                                   rhi::PipelineStage::RayTracingShader,
                                   rhi::ResourceState::Undefined,
                                   rhi::ResourceState::UnorderedAccess);
        g.cmdList->BindRTPipeline(g.rtPSO.get());
        g.cmdList->BindDescriptorSet(0, g.rtSet);
        g.cmdList->SetPushConstants(0, sizeof(rotationPC), &rotationPC);
        g.cmdList->TraceRays(g.sbt, w, h, 1);

        g.cmdList->PipelineBarrier(rhi::PipelineStage::RayTracingShader,
                                   rhi::PipelineStage::FragmentShader,
                                   rhi::ResourceState::UnorderedAccess,
                                   rhi::ResourceState::ShaderResource);

        g.cmdList->SetPipeline(g.blitPSO.get());
        g.cmdList->BeginRenderPass(1, g.swapchain->GetColorFormat());
        g.cmdList->SetFullscreenViewport(w, h);
        g.cmdList->BindDescriptorSet(0, g.blitSet);
        g.cmdList->Draw(3);
    } else if (g.renderMode == 2 && g.meshSupported && g.meshPSO) {
        g.cmdList->SetPipeline(g.meshPSO.get());
        g.cmdList->BeginRenderPass(1, g.swapchain->GetColorFormat());
        g.cmdList->SetFullscreenViewport(w, h);
        g.cmdList->SetPushConstants(0, sizeof(rotationPC), &rotationPC);
        g.cmdList->DrawMeshTasks(1, 1, 1);
    } else {
        g.cmdList->SetPipeline(g.pipeline.get());
        g.cmdList->BeginRenderPass(1, g.swapchain->GetColorFormat());
        g.cmdList->SetFullscreenViewport(w, h);
        g.cmdList->SetPushConstants(0, sizeof(rotationPC), &rotationPC);
        g.cmdList->SetVertexBuffer(g.vertexBuffer.get(), 0);
        g.cmdList->Draw(3);
    }

    // ── ImGui（在主 RP 内渲染）──
    if (g.imgui && g.imgui->IsInitialized()) {
        g.imgui->BeginFrame();
        const float ui = ImGui::GetFontSize() / 16.0f;   // 高 DPI：按字号比例给尺寸
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330.0f * ui, 300.0f * ui), ImGuiCond_FirstUseEver);
        ImGui::Begin("01.Triangle (App)");

        // 渲染后端（鸿蒙只有 Vulkan，列表里就一项）
        const std::vector<rhi::Backend> backends = AvailableBackends();
        ImGui::TextUnformatted("渲染后端");
        ImGui::SetNextItemWidth(170.0f * ui);
        if (ImGui::BeginCombo("##backend", BackendName(g.pendingBackend))) {
            for (const rhi::Backend b : backends) {
                const bool selected = (b == g.pendingBackend);
                if (ImGui::Selectable(BackendName(b), selected)) {
                    g.pendingBackend    = b;
                    g.hasBackendRequest = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Separator();
        ImGui::RadioButton("光栅化 (VS/PS)", &g.renderMode, 0);
        if (g.rtRenderable) {
            ImGui::RadioButton("Ray Tracing (RayGen)", &g.renderMode, 1);
        } else {
            ImGui::BeginDisabled();
            ImGui::RadioButton(g.rtSupported ? "Ray Tracing (不可用)" : "Ray Tracing (不支持)", &g.renderMode, 1);
            ImGui::EndDisabled();
        }
        if (g.meshSupported) {
            ImGui::RadioButton("Mesh Shader (MS/PS)", &g.renderMode, 2);
        } else {
            ImGui::BeginDisabled();
            ImGui::RadioButton("Mesh Shader (不支持)", &g.renderMode, 2);
            ImGui::EndDisabled();
        }

        ImGui::Separator();
        const char* modeNames[] = {"光栅化", "Ray Tracing", "Mesh Shader"};
        ImGui::Text("当前模式: %s", modeNames[g.renderMode]);
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::SetNextItemWidth(120.0f * ui);
        ImGui::SliderFloat("转速", &g.rotationSpeed, 0.0f, 5.0f, "%.2f rad/s");
        ImGui::SameLine();
        if (ImGui::SmallButton("停/转")) g.rotationSpeed = (g.rotationSpeed > 0.0f) ? 0.0f : 1.0f;
        ImGui::End();

        g.imgui->EndFrame(g.cmdList.get());
    }

    g.cmdList->EndRenderPass();
    g.framePresenter->EndRecording();
    g.framePresenter->EndFrame(true);
    return true;
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
    HE_CORE_INFO("=== 关闭渲染后端：{} ===", BackendName(g.backend));
    g.device->WaitIdle();

    g.framePresenter.reset();
    if (g.imgui) {
        g.imgui->Shutdown();
        g.imgui.reset();
    }
    if (g.rtLayout != rhi::kInvalidLayout) {
        g.device->DestroyDescriptorSetLayout(g.rtLayout);
        g.rtLayout = rhi::kInvalidLayout;
    }
    if (g.blitLayout != rhi::kInvalidLayout) {
        g.device->DestroyDescriptorSetLayout(g.blitLayout);
        g.blitLayout = rhi::kInvalidLayout;
    }
    g.rtSet = rhi::kInvalidSet;
    g.blitSet = rhi::kInvalidSet;

    g.meshPSO.reset();
    g.blitPSO.reset();
    g.rtPSO.reset();
    g.rtSBTBuf.reset();
    g.blitSampler.reset();
    g.rtTarget.reset();
    g.rtTargetW = g.rtTargetH = 0;
    g.vertexBuffer.reset();
    g.pipeline.reset();
    g.cmdList.reset();
    g.swapchain.reset();
    g.device.reset();
    rhi::SetDevice(nullptr);

    g.rtSupported = g.rtRenderable = g.meshSupported = false;
    g.sbt   = {};
    g.ready = false;
    g.hasBackendRequest = false;
}

} // namespace he::samples::triangle01


// ============================================================
// 桌面入口
//
// 只有桌面 target 定义 HE_DESKTOP_ENTRY 时才编这一段：
//   · 桌面 exe（01.Triangle）→ CMake 定义 HE_DESKTOP_ENTRY=1，这里产出 main；
//   · 鸿蒙 entry（libentry.so）→ 同一份 App.cpp，但没有这个宏 ⇒ 不含 main，
//     入口在 Platforms/OHOS/entry/src/main/cpp/AppMain.cpp。
// 这样样例目录里只剩 App 一个 .cpp，不需要单独的 Main.Win32.cpp。
// ============================================================
#if defined(HE_DESKTOP_ENTRY)
#include "DesktopHost.h"

int main() {
    he::samples::desktop::HostConfig cfg;
    cfg.title  = "HugEngine — 01.Triangle";
    cfg.width  = 960;
    cfg.height = 540;

    // 01 是唯一支持面板/环境变量热切换后端的样例
    cfg.backendSwitch = true;
    cfg.switchToEnv   = "HE_TRIANGLE_SWITCH_TO";
    cfg.switchBackEnv = "HE_TRIANGLE_SWITCH_BACK";

    return he::samples::desktop::RunApp<he::samples::triangle01::App>(cfg);
}
#endif // HE_DESKTOP_ENTRY
