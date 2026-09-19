#include "Lumen/LumenScene.h"

#include "Core/Log.h"
#include "Core/Assert.h"              // HE_ASSERT（PSO 创建失败要立刻可见）
#include <cstdlib>
#include <string>                    // std::getenv（确定性验收模式的开关）

#include "SSAO.vert.spv.h"            // 全屏三角顶点着色（与 SSGI/AO 等 pass 共用）
#include "Lumen_Skeleton.frag.spv.h"  // 骨架阶段的常量输出

namespace he::render {

bool LumenScene::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    if (!device) {
        HE_CORE_ERROR("LumenScene: 设备为空，初始化失败");
        return false;
    }
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    // 确定性验收模式（见 RunProbeIrradiance 末尾的说明）：默认关，只在需要"背靠背逐位一致"时开
    m_Deterministic = (std::getenv("HE_LUMEN_DETERMINISTIC") != nullptr);
    if (m_Deterministic) HE_CORE_INFO("LumenScene: 确定性模式已开启（每帧末等待 GPU）");
    // 步骤 27 的 A/B 开关：远场阈值与重叠带半宽（比例）。用环境变量而不是 cfg 键，是为了让
    // "沿阈值扫一遍看有没有阶跃"的对照实验不必改配置文件（HE_LUMEN_FARFIELD_OVERLAP=0 即硬切换）。
    if (const char* ov = std::getenv("HE_LUMEN_FARFIELD_OVERLAP")) {
        m_FarFieldOverlap = std::max(0.0f, (float)std::atof(ov));
        HE_CORE_INFO("LumenScene: 远场重叠带半宽 = {:.3f}（0 = 硬切换）", (double)m_FarFieldOverlap);
    }
    // 步骤 28：追踪/着色源的运行期开关（用于演示"合法但未实现"的组合会被**显式**报出来，
    // 而不是静默回落到 SurfaceCache）。cfg 里没有这两个键，故用环境变量。
    if (const char* sd = std::getenv("HE_LUMEN_SHADE")) {
        const std::string v(sd);
        if (v == "hitlighting") m_TraceConfig.shade = LumenShadeSource::HitLighting;
        else if (v == "neutral") m_TraceConfig.shade = LumenShadeSource::Neutral;
        else                     m_TraceConfig.shade = LumenShadeSource::SurfaceCache;
        HE_CORE_INFO("LumenScene: 着色源 = {}", LumenShadeSourceName(m_TraceConfig.shade));
    }
    if (const char* tr = std::getenv("HE_LUMEN_TRACE")) {
        const std::string v(tr);
        if (v == "hwrt")        m_TraceConfig.trace = LumenTraceSource::HardwareRT;
        else if (v == "screen") m_TraceConfig.trace = LumenTraceSource::Screen;
        else                    m_TraceConfig.trace = LumenTraceSource::SDF;
        HE_CORE_INFO("LumenScene: 追踪源 = {}", LumenTraceSourceName(m_TraceConfig.trace));
    }
    if (std::getenv("HE_LUMEN_SCREEN_TRACE")) {
        m_TraceConfig.screenTrace = true;
        HE_CORE_INFO("LumenScene: screenTrace = true");
    }
    if (const char* th = std::getenv("HE_LUMEN_FARFIELD_THRESHOLD")) {
        m_FarFieldThreshold = std::max(1.0f, (float)std::atof(th));
        HE_CORE_INFO("LumenScene: 远场阈值 = {:.1f} 世界单位", (double)m_FarFieldThreshold);
    }
    CreateSkeletonPipeline();
    CreatePageCheckGPUObjects();   // 步骤 14：描述符集/PSO 提前建好（帧中途分配实测拿不到有效集合）
    CreateOutput();
    m_SDF.Initialize(device);   // 步骤 8：逐 mesh 距离场（构建由 StepSDF 逐帧推进）
    m_SDF.SetViewport(width, height);   // 步骤 12：调试视图按视口分辨率逐像素发射主射线
    HE_CORE_INFO("LumenScene: 初始化持久资源宿主（视口 {}x{}）", width, height);
    return true;
}

void LumenScene::Shutdown() {
    // 步骤 8/10/14/23 在这里释放 Mesh SDF 缓存、Global SDF clipmap、页表与探针缓冲。
    m_SDF.Shutdown();
    DestroySkeletonPipeline();
    m_OutputSampler.reset();
    m_Output.reset();
    if (m_Device) {
        HE_CORE_INFO("LumenScene: 释放持久资源宿主");
    }
    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
}

void LumenScene::OnResize(u32 width, u32 height) {
    if (!m_Device) return;
    if (width == m_Width && height == m_Height) return;
    m_Width  = width;
    m_Height = height;
    // 屏幕尺寸相关的资源按新尺寸重建；Surface Cache atlas 与 Global SDF clipmap 是
    // **世界空间**尺寸，与视口无关，不在此重建。
    CreateOutput();
    m_SDF.SetViewport(width, height);   // 调试视图纹理随之按新尺寸重建（步骤 12）

    // 【步骤 35】探针滤波的状态一律作废：单元网格（cellsX/cellsY）变了 ⇒ 单元映射缓冲与
    // 其历史都要按新尺寸重建，历史内容也失去意义（探针布局变了，重投影对不上）。
    // 这里把"池里借来的指针"与"自建缓冲"一起清掉，下一帧的 `!m_ProbeFilterBound` 分支重新取；
    // 历史标记与上一帧 viewProj 也复位，避免拿旧尺寸的历史去混合（那会得到错位的拖影）。
    m_ProbeHistFromPool[0] = m_ProbeHistFromPool[1] = nullptr;
    m_CellHistFromPool[0]  = m_CellHistFromPool[1]  = nullptr;
    for (u32 k = 0; k < 2u; ++k) {
        m_ProbeFilterHistBuf[k].reset();
        m_CellProbeHistBuf[k].reset();
    }
    m_ProbeFilteredBuf.reset();
    m_ProbeFilterStatsBuf.reset();
    m_ProbeFilterStatsMapped = nullptr;
    m_ProbeFilterBound = false;
    m_ProbeFilterHistoryReady = false;
    m_ProbeFilterHistIdx = 0;
    m_ProbePrevViewProj = float4x4(1.0f);
    m_IrrBound = false;   // 辐照度 pass 绑的是过滤后探针缓冲 ⇒ 必须重绑
}

void LumenScene::CreateSkeletonPipeline() {
    if (!m_Device || m_SkeletonPSO) return;

    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_Lumen_Skeleton_frag_spv;
    fs.entryPoint = "fragmentMain";

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskFragment;
    pcr.offset    = 0;
    pcr.size      = sizeof(float) * 2;   // u_ValueAlpha = (值, alpha)

    rhi::PipelineStateDesc d;
    d.vertexShader         = &vs;
    d.pixelShader          = &fs;
    d.topology             = rhi::PrimitiveTopology::TriangleList;
    d.depthTest            = false;                 // 全屏占位 pass，不读写深度
    d.depthWrite           = false;
    d.depthFormat          = rhi::Format::Unknown;  // 与 BeginOffscreenPass(nullptr 深度) 保持一致
    d.colorAttachmentCount = 1;
    d.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
    d.pushConstantRanges   = {pcr};
    d.debugName            = "Lumen_Skeleton";
    m_SkeletonPSO = m_Device->CreatePipelineState(d);
    HE_ASSERT(m_SkeletonPSO, "LumenScene: 骨架 pass 的 PSO 创建失败");
}

void LumenScene::DestroySkeletonPipeline() {
    m_SkeletonPSO.reset();
}

void LumenScene::PreBind(rhi::IRHICommandList* cmd) {
    if (cmd && m_SkeletonPSO) cmd->SetPipeline(m_SkeletonPSO.get());
}

void LumenScene::DrawSkeleton(rhi::IRHICommandList* cmd, float value, float alpha) {
    if (!cmd || !m_SkeletonPSO) return;
    const float pc[2] = { value, alpha };
    cmd->SetPushConstants(0, sizeof(pc), pc);
    cmd->Draw(3);
}

void LumenScene::CreateOutput() {
    if (!m_Device || m_Width == 0 || m_Height == 0) return;

    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA16_FLOAT;
    td.width  = m_Width;
    td.height = m_Height;
    // RenderTarget：帧图把它当作 offscreen 颜色附件清写；ShaderResource：Lighting 采样它
    td.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output  = m_Device->CreateTexture(td);

    // 步骤 24：屏幕尺寸的辐照度中间纹理（compute 写、全屏 pass 采样）
    CreateIrradianceTexture();

    // 线性 + Clamp：Lumen 输出是低频间接光，采样器与 SSGI/SSR 的输出采样器同规格
    if (!m_OutputSampler) {
        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU  = sd.addressV  = rhi::AddressMode::ClampToEdge;
        m_OutputSampler = m_Device->CreateSampler(sd);
    }
}

} // namespace he::render
