// ============================================================
// 01.Triangle — HugEngine RHI 渲染三角形（光栅化 + Ray Tracing + Mesh Shader）
//
// 光栅化：   传统 VS→PS 管线，纯红色三角形
// RT：       RayGen 着色器过程化红色三角形 → StorageImage
// Mesh Shader：无 VB/IB，MS→PS 硬编码三色渐变三角形
// ImGui：    切换渲染模式；**下拉选择渲染后端（Vulkan / D3D12 / D3D11）并热切换**
//
// 【后端热切换怎么做的】RHI 只允许同时存在一个设备（全局 `rhi::SetDevice`），所以切换 =
//   在帧末销毁整份后端状态（设备 / 交换链 / PSO / ImGui / 命令列表 / 帧外壳），再用同一套
//   初始化代码换一个 `Backend` 重建一遍。窗口（Engine/Window）与 RHI 无关，不需要重建。
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "RHI/FramePresenter.h"   // L4-1：帧外壳（acquire/present）收口
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
#include "imgui.h"

#include <cstdlib>
#include <cstring>
#include <iterator>   // std::begin / std::end（填 dxil 字节码用）
#include <memory>
#include <vector>

using namespace he;

namespace {

// ============================================================
// 后端列表与名字
// ============================================================
/// 本平台可选的渲染后端。D3D 只在 Windows 构建里出现（对应 BACKEND 宏由 CMake 打开）。
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

/// 在可用列表里的下标（下拉菜单用），找不到返回 0
int BackendIndex(const std::vector<rhi::Backend>& list, rhi::Backend b) {
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        if (list[i] == b) return i;
    }
    return 0;
}

// ============================================================
// 一份后端状态：切换后端时整体销毁重建
// ============================================================
struct Gfx {
    rhi::Backend backend = rhi::Backend::Vulkan;

    std::unique_ptr<rhi::IRHIDevice>          device;
    std::unique_ptr<rhi::IRHISwapChain>       swapchain;
    std::unique_ptr<rhi::IRHICommandList>     cmdList;
    std::unique_ptr<rhi::IRHIPipelineState>   pipeline;       // 光栅化
    std::unique_ptr<rhi::IRHIBuffer>          vertexBuffer;

    // ---- 能力（每次重建都要重新取：同一份代码在不同后端上能力不同）----
    bool rtSupported   = false;   // 硬件有光追
    bool rtRenderable  = false;   // 且后缓冲能当存储图像（D3D12 的 FLIP 后缓冲不行）
    bool meshSupported = false;   // 硬件有网格着色器

    // ---- RT 路径（直接写后缓冲，无中间纹理）----
    std::unique_ptr<rhi::IRHIRayTracingPipelineState> rtPSO;
    std::unique_ptr<rhi::IRHIBuffer>                  rtSBTBuf;
    rhi::SBTDesc                    sbt{};
    rhi::DescriptorSetLayoutHandle  rtLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle        rtSet    = rhi::kInvalidSet;

    // ---- Mesh Shader 路径 ----
    std::unique_ptr<rhi::IRHIPipelineState> meshPSO;

    // ---- UI 与帧外壳（都持有 device / swapchain 指针，必须随后端重建）----
    std::unique_ptr<editor::ImGuiIntegration> imgui;
    std::unique_ptr<rhi::FramePresenter>      framePresenter;
};

// ============================================================
// 初始化一份后端状态
//
// 所有对象都从 `backend` 新建；失败返回 false（调用方决定是退出还是保留旧状态）。
// 注意：`rhi::CreateDevice` 在设备创建失败时会直接退出进程（见 RHI.h 的说明），
//       所以这里只兜「设备创建成功之后」的失败（交换链 / PSO 等）。
// ============================================================
bool InitGfx(Gfx& g, Engine& engine, rhi::Backend backend) {
    g.backend = backend;
    HE_CORE_INFO("=== 初始化渲染后端：{} ===", BackendName(backend));

    rhi::DeviceInitDesc desc;
    desc.backend          = backend;
    desc.enableValidation = true;
    desc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    g.device = rhi::CreateDevice(desc.backend);
    g.device->Initialize(desc);
    rhi::SetDevice(g.device.get());

    g.rtSupported   = g.device->GetCaps().supportsRayTracing;
    g.meshSupported = g.device->GetCaps().supportsMeshShaders;
    // 【P2】本样例的 RT 模式是**直接把后缓冲当存储图像（UAV）写**：
    //   · Vulkan 交换链图像带 VK_IMAGE_USAGE_STORAGE_BIT ⇒ 可以；
    //   · D3D12 的 FLIP 后缓冲由 DXGI 创建，资源标志只有 ALLOW_RENDER_TARGET，转 UNORDERED_ACCESS
    //     会被调试层判为 id=524（缺 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS，且该标志无法补）。
    // 故"能进 RT 模式"要同时满足硬件支持光追 ∧ 后缓冲可作存储图像。
    g.rtRenderable = g.rtSupported && g.device->GetCaps().supportsBackBufferStorageImage;
    HE_CORE_INFO("RT 支持: {}, Mesh Shader 支持: {}（RT 写后缓冲={}）",
                 g.rtSupported ? "是" : "否", g.meshSupported ? "是" : "否",
                 g.rtRenderable ? "可" : "不可");

    // ---- 交换链 ----
    const u32 swW = engine.GetWindow()->GetWidth();
    const u32 swH = engine.GetWindow()->GetHeight();
    g.swapchain = g.device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width = swW, .height = swH, .vsync = true,
    });
    if (!g.swapchain) {
        HE_CORE_ERROR("交换链创建失败（后端 {}）", BackendName(backend));
        return false;
    }

    // ---- 光栅化路径 ----
    struct Vertex { float x, y; };
    // 【Y 方向】引擎的裁剪空间约定是 **Y 向上（D3D 风格）**：Vulkan 后端的
    // `SetFullscreenViewport` 用负高度视口把 Y 翻过来，D3D 用正高度、不翻。
    // 因此这里的顶点按 "y = +0.5 是屏幕上方" 写：顶点 0 在上、底边在下。
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
        // 颜色附件必须是后缓冲的真实格式（D3D12 交换链为 BGRA8，默认值 RGBA8 会让绘制被丢弃）
        .colorFormats = { g.swapchain->GetColorFormat() },
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
        rtpDesc.shaders      = { rgen, rmiss };
        rtpDesc.shaderGroups = groups;
        rtpDesc.maxRecursionDepth = 1;
        rtpDesc.descriptorSetLayouts = { g.rtLayout };
        rtpDesc.debugName = "RT_Triangle";

        g.rtPSO = g.device->CreateRTPipelineState(rtpDesc);
        // 光追是**可选**路径：某个后端没实现 RT 时只跳过，不中断样例
        if (!g.rtPSO) {
            HE_CORE_WARN("01.Triangle: RT PSO 不可用（当前后端未实现光追），跳过光追路径");
        }

        // SBT —— 三条硬性要求（与引擎 RTPass::BuildSBT 的写法一致）：
        //   1) 承载缓冲必须带 SHADER_BINDING_TABLE 用途，否则 deviceAddress 找不到有效缓冲；
        //   2) 每个区域的起始地址必须按 shaderGroupBaseAlignment 对齐；
        //   3) 组与组之间的步长同样要按该对齐向上取整，不能直接用 handleSize。
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
        u8* mapped = static_cast<u8*>(g.rtSBTBuf->Map());
        for (u32 gi = 0; gi < groupCount; ++gi) {
            std::memcpy(mapped + gi * stride, handles.data() + gi * handleSize, handleSize);
        }
        g.rtSBTBuf->Unmap();

        g.sbt.buffer = g.rtSBTBuf.get();
        g.sbt.rayGen.handleOffset = 0;
        g.sbt.rayGen.stride = stride;
        g.sbt.miss.handleOffset = stride;
        g.sbt.miss.stride = stride;

        HE_CORE_INFO("RT 管线就绪: {} groups, {}B SBT (直接渲染到 BackBuffer)", groupCount, sbtSize);
    }

    // ---- Mesh Shader 路径（无 VB/IB，硬编码三色三角形）----
    if (g.meshSupported) {
        // 网格着色器只有 SPIR-V / DXIL（D3D11 不支持网格着色器，故无 DXBC）；
        // 片元着色器三套都给：D3D11 走 DXBC，Vulkan 走 SPIR-V，D3D12 走 DXIL
        rhi::ShaderBytecode meshShader = rhi::MakeShaderBytecode(
            rhi::ShaderStage::Mesh, k_Triangle_mesh_spv, k_Triangle_mesh_dxil, "main");
        rhi::ShaderBytecode meshFrag = rhi::MakeShaderBytecode(
            rhi::ShaderStage::Pixel, k_TriangleMesh_frag_spv, k_TriangleMesh_frag_dxil, k_TriangleMesh_frag_dxbc, "main");

        g.meshPSO = g.device->CreatePipelineState({
            .pixelShader = &meshFrag,
            .meshShader  = &meshShader,
            .depthTest   = false,
            .depthWrite  = false,
            .colorFormats = { g.swapchain->GetColorFormat() },
            .debugName   = "TriangleMesh",
        });
        HE_CORE_INFO("Mesh Shader 管线就绪: DrawMeshTasks(1,1,1)");
    }

    // ---- ImGui（每个后端一套实现，必须重建）----
    g.imgui = std::make_unique<editor::ImGuiIntegration>();
    g.imgui->Initialize(engine.GetWindow()->GetNativeHandle(), g.device.get(), g.swapchain.get());

    // ---- 命令列表与帧外壳 ----
    g.cmdList = g.device->CreateCommandList();
    g.cmdList->SetSwapChain(g.swapchain.get());
    g.framePresenter = std::make_unique<rhi::FramePresenter>(g.swapchain.get());
    return true;
}

// ============================================================
// 销毁一份后端状态（顺序与创建相反）
// ============================================================
void ShutdownGfx(Gfx& g) {
    if (!g.device) return;
    HE_CORE_INFO("=== 关闭渲染后端：{} ===", BackendName(g.backend));
    g.device->WaitIdle();

    // 帧外壳持有交换链指针；ImGui 持有 device / swapchain 指针 ⇒ 都必须先于它们释放
    g.framePresenter.reset();
    if (g.imgui) {
        g.imgui->Shutdown();
        g.imgui.reset();
    }
    if (g.rtLayout != rhi::kInvalidLayout) {
        g.device->DestroyDescriptorSetLayout(g.rtLayout);
        g.rtLayout = rhi::kInvalidLayout;
    }
    g.rtSet = rhi::kInvalidSet;

    HE_CORE_INFO("[关闭] 释放 meshPSO");
    g.meshPSO.reset();
    g.rtPSO.reset();
    g.rtSBTBuf.reset();
    HE_CORE_INFO("[关闭] 释放顶点缓冲 / 光栅化 PSO");
    g.vertexBuffer.reset();
    g.pipeline.reset();
    HE_CORE_INFO("[关闭] 释放命令列表 / 交换链 / 设备");
    g.cmdList.reset();
    g.swapchain.reset();
    g.device.reset();
    rhi::SetDevice(nullptr);   // 全局设备指针指向已销毁对象会变成悬垂指针

    g.rtSupported = g.rtRenderable = g.meshSupported = false;
    g.sbt = {};
}

/// 切换后端后，把用户选的渲染模式夹到新后端真正可用的档位：
///   1（RT）需要 rtRenderable，2（Mesh）需要 meshSupported
int ClampRenderMode(int mode, const Gfx& g) {
    if (mode == 1 && !(g.rtRenderable && g.rtPSO)) return 0;
    if (mode == 2 && !(g.meshSupported && g.meshPSO)) return 0;
    return mode;
}

}  // namespace

int main() {
    // ============================================================
    // 1. 引擎启动（窗口与 RHI 无关，切换后端时窗口不重建）
    // ============================================================
    EngineConfig config;
    config.appName      = "HugEngine — 01.Triangle";
    config.windowWidth  = 960;
    config.windowHeight = 540;

    Engine engine(config);
    engine.Initialize();

    // 初始后端由 HE_RHI_BACKEND 决定（下拉切换后不再读它）
    Gfx gfx;
    if (!InitGfx(gfx, engine, rhi::ResolveBackendFromEnv())) {
        HE_CORE_ERROR("01.Triangle: 初始后端初始化失败");
        return 1;
    }

    // 窗口缩放：交换链与命令列表都要跟着换（回调里引用的是 gfx，切换后端后依然有效）
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (!gfx.swapchain || !gfx.cmdList) return;
        gfx.swapchain->Resize(w, h);
        gfx.cmdList->SetSwapChain(gfx.swapchain.get());
    });

    // ============================================================
    // 2. 主渲染循环
    // ============================================================
    int renderMode = (gfx.rtRenderable && gfx.rtPSO) ? 1 : 0;  // 0=光栅化, 1=RT, 2=Mesh Shader
    // 【自动化用】HE_TRIANGLE_MODE=0/1/2 可强制渲染模式：跨后端截图对比时不必手点面板
    // （Vulkan 默认走 RT、D3D 因后缓冲不可写只能走光栅化，不带这个开关就无法做同路径对比）
    if (const char* modeEnv = std::getenv("HE_TRIANGLE_MODE")) renderMode = std::atoi(modeEnv);
    renderMode = ClampRenderMode(renderMode, gfx);

    const std::vector<rhi::Backend> backends = AvailableBackends();
    rhi::Backend pendingBackend = gfx.backend;   // 下拉里选中的后端（帧末真正切换）

    HE_CORE_INFO("01.Triangle started — ImGui 切换渲染模式 / 渲染后端（当前 {}）", BackendName(gfx.backend));
    u64 frameIndex = 0;

    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();
        if (!gfx.framePresenter->BeginFrame()) continue;

        const u32 w = gfx.swapchain->GetWidth();
        const u32 h = gfx.swapchain->GetHeight();

        gfx.framePresenter->BeginRecording(gfx.device.get(), gfx.cmdList.get());   // L4-2

        // ── RT 调度 + 直接渲染到 BackBuffer ──
        if (renderMode == 1 && gfx.rtRenderable && gfx.rtPSO) {
            // 将 BackBuffer ImageView 绑定为 RT 输出（StorageImage）
            void* backBufferView = gfx.swapchain->GetCurrentBackBufferView();
            gfx.device->UpdateDescriptorSetWithImageView(gfx.rtSet, 0,
                rhi::DescriptorType::StorageImage, backBufferView);

            // 全局屏障：确保 BackBuffer 可被 RT 写入
            gfx.cmdList->PipelineBarrier(rhi::PipelineStage::BottomOfPipe,
                                         rhi::PipelineStage::RayTracingShader,
                                         rhi::ResourceState::Undefined,
                                         rhi::ResourceState::UnorderedAccess);

            gfx.cmdList->BindRTPipeline(gfx.rtPSO.get());
            gfx.cmdList->BindDescriptorSet(0, gfx.rtSet);
            gfx.cmdList->TraceRays(gfx.sbt, w, h, 1);

            // 全局屏障：等待 RT 写入完成
            gfx.cmdList->PipelineBarrier(rhi::PipelineStage::RayTracingShader,
                                         rhi::PipelineStage::ColorAttachmentOutput,
                                         rhi::ResourceState::UnorderedAccess,
                                         rhi::ResourceState::RenderTarget);

            // SetPipeline 设置 m_CurrentRenderPass（BeginRenderPass 必需）
            gfx.cmdList->SetPipeline(gfx.pipeline.get());

            // RenderPass (LoadOp::Load 保留 BackBuffer 上的 RT 输出)
            gfx.cmdList->BeginRenderPass(1, gfx.swapchain->GetColorFormat(), rhi::Format::Unknown,
                                         nullptr, rhi::LoadOp::Load);
        } else if (renderMode == 2 && gfx.meshSupported && gfx.meshPSO) {
            // ── Mesh Shader 模式：三色渐变三角形，无 VB/IB ──
            gfx.cmdList->SetPipeline(gfx.meshPSO.get());
            gfx.cmdList->BeginRenderPass(1, gfx.swapchain->GetColorFormat());
            // 全屏视口：走引擎统一入口（Vulkan 会设负高度翻转 Y，与 D3D 的 Y 向上约定对齐）
            gfx.cmdList->SetFullscreenViewport(w, h);
            gfx.cmdList->DrawMeshTasks(1, 1, 1);  // 1 个工作组，3 个线程输出 1 个三角形
        } else {
            // 光栅化模式：红色三角形
            gfx.cmdList->SetPipeline(gfx.pipeline.get());
            gfx.cmdList->BeginRenderPass(1, gfx.swapchain->GetColorFormat());
            // 同上：用 SetFullscreenViewport 而不是裸 SetViewport（裸调用会绕过后端的 Y 约定）
            gfx.cmdList->SetFullscreenViewport(w, h);
            gfx.cmdList->SetVertexBuffer(gfx.vertexBuffer.get(), 0);
            gfx.cmdList->Draw(3);
        }

        // ── ImGui（在主 RP 内渲染，覆盖在场景之上）──
        if (gfx.imgui && gfx.imgui->IsInitialized()) {
            gfx.imgui->BeginFrame();
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(300, 240), ImGuiCond_FirstUseEver);
            ImGui::Begin("01.Triangle");

            // ---- 渲染后端下拉（选中后在本帧末重建后端）----
            ImGui::TextUnformatted("渲染后端");
            ImGui::SetNextItemWidth(170.0f);
            if (ImGui::BeginCombo("##backend", BackendName(pendingBackend))) {
                for (const rhi::Backend b : backends) {
                    const bool selected = (b == pendingBackend);
                    if (ImGui::Selectable(BackendName(b), selected)) pendingBackend = b;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (pendingBackend != gfx.backend) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "本帧末切换到 %s（重建设备，约 1 秒）",
                                   BackendName(pendingBackend));
            } else {
                ImGui::TextDisabled("当前后端: %s（切换会重建设备）", BackendName(gfx.backend));
            }

            // ---- 渲染模式 ----
            ImGui::Separator();
            ImGui::RadioButton("光栅化 (VS→PS)", &renderMode, 0);
            if (gfx.rtRenderable) {
                ImGui::RadioButton("Ray Tracing (RayGen)", &renderMode, 1);
            } else {
                ImGui::BeginDisabled();
                // 区分「硬件没有光追」与「本后端后缓冲不能当 UAV 写」两种情况，避免误导
                ImGui::RadioButton(gfx.rtSupported ? "Ray Tracing (后缓冲不可写)" : "Ray Tracing (不支持)",
                                   &renderMode, 1);
                ImGui::EndDisabled();
            }
            if (gfx.meshSupported) {
                ImGui::RadioButton("Mesh Shader (MS→PS)", &renderMode, 2);
            } else {
                ImGui::BeginDisabled();
                ImGui::RadioButton("Mesh Shader (不支持)", &renderMode, 2);
                ImGui::EndDisabled();
            }

            ImGui::Separator();
            const char* modeNames[] = {"光栅化", "Ray Tracing", "Mesh Shader"};
            ImGui::Text("当前模式: %s", modeNames[renderMode]);
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::End();
            gfx.imgui->EndFrame(gfx.cmdList.get());
        }

        gfx.cmdList->EndRenderPass();
        gfx.framePresenter->EndRecording();   // L4-2：End + Submit 收口
        gfx.framePresenter->EndFrame(true);
        ++frameIndex;

        // ============================================================
        // 帧末：如果下拉选了别的后端，就在这里整体重建
        // ============================================================
        if (pendingBackend != gfx.backend) {
            const rhi::Backend want = pendingBackend;
            ShutdownGfx(gfx);
            if (!InitGfx(gfx, engine, want)) {
                HE_CORE_ERROR("01.Triangle: 切换到 {} 失败，退出", BackendName(want));
                break;
            }
            // 新后端未必支持原来的渲染模式（D3D 没有可写后缓冲的 RT、D3D11 没有 mesh shader）
            renderMode = ClampRenderMode(renderMode, gfx);
            HE_CORE_INFO("01.Triangle: 已切换到 {}（渲染模式 {}）", BackendName(gfx.backend), renderMode);
        }
    }

    // ============================================================
    // 3. 清理
    // ============================================================
    ShutdownGfx(gfx);
    HE_CORE_INFO("Exiting after {} frames", frameIndex);
    return 0;
}
