// PostProcess/RTDenoiser.cpp — RT 效果时域累积降噪器实现
#include "PostProcess/RTDenoiser.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "SSAO.vert.spv.h"             // 全屏三角形 VS（k_SSAO_vert_spv）
#include "RT_DenoiseTemporal.frag.spv.h" // 时域累积 FS（k_RT_DenoiseTemporal_frag_spv）

namespace he::render {

// RT Denoiser 鎻忚堪绗﹂泦缁戝畾鍙穈r
static constexpr u32 kRTDenoiseBindColor = 0;
static constexpr u32 kRTDenoiseBindHistory = 1;
static constexpr u32 kRTDenoiseBindDepth = 2;
static constexpr u32 kRTDenoiseBindNormal = 3;
static constexpr u32 kRTDenoiseBindVelocity = 4;

// ============================================================
// Initialize — 创建历史/输出纹理 + PSO + 描述符集
// ============================================================
bool RTDenoiser::Initialize(rhi::IRHIDevice* device, const Config& cfg,
                              DenoiseHistoryPool* historyPool) {
    m_Device = device;
    m_Cfg    = cfg;
    m_Width  = cfg.width;
    m_Height = cfg.height;
    HE_ASSERT(m_Device, "RTDenoiser: null device");

    // ── 描述符布局：5 个 CombinedImageSampler（噪声/历史/深度/法线/速度）──
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // u_NoisyColor + u_PointSampler
        {1, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // u_History
        {2, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // u_Depth
        {3, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // u_Normal
        {4, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // u_Velocity
    };
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    // ── 点采样器（所有输入均用最近邻，避免插值模糊信号）──
    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Nearest;
    sd.addressU  = sd.addressV  = rhi::AddressMode::ClampToEdge;
    m_PointSampler = device->CreateSampler(sd);

    // ── 历史 + 输出纹理（格式与 RT Pass 输出一致）──
    m_HistoryPool = historyPool;   // 步骤 34：非空 ⇒ 历史从统一池里取
    CreateTextures(m_Width, m_Height);

    // ── PSO ──
    CreatePSO();

    m_Ready = true;
    HE_CORE_INFO("RTDenoiser[{}] initialized ({}x{}, format={}, blend={})",
                 m_Cfg.debugName, m_Width, m_Height,
                 static_cast<u32>(m_Cfg.format), m_Cfg.temporalBlend);
    return true;
}

// ============================================================
// CreateTextures — 创建历史（上一帧）+ 输出（当前帧）纹理
// ============================================================
void RTDenoiser::CreateTextures(u32 w, u32 h) {
    rhi::TextureDesc d;
    d.format     = m_Cfg.format;
    d.width      = w;
    d.height     = h;
    d.mipLevels  = 1;
    // RenderTarget：累积 Pass 写入；ShaderResource：下帧作为历史被采样
    d.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_OwnedOutput = m_Device->CreateTexture(d);
    m_Output      = m_OwnedOutput.get();
    if (m_HistoryPool) {
        // 步骤 34（11.3）：历史由**统一池**分配 —— 同名同尺寸同格式只建一次，
        // 池自己持有所有权，本类只用裸指针（swap 角色时不动所有权）。
        m_History = m_HistoryPool->Acquire(m_Cfg.debugName, w, h, m_Cfg.format);
    } else {
        m_OwnedHistory = m_Device->CreateTexture(d);
        m_History      = m_OwnedHistory.get();
    }
}

// ============================================================
// CreatePSO — 全屏三角形图形管线
// ============================================================
void RTDenoiser::CreatePSO() {
    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_RT_DenoiseTemporal_frag_spv;
    fs.entryPoint = "fragmentMain";

    // Push constant：32B（float2 texelSize + 3×float + uint + 2×float pad）
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment;
    pc.size      = 32;

    rhi::PipelineStateDesc desc;
    desc.vertexShader        = &vs;
    desc.pixelShader         = &fs;
    desc.topology            = rhi::PrimitiveTopology::TriangleList;
    desc.depthTest           = false;
    desc.depthWrite          = false;
    desc.depthFormat         = rhi::Format::Unknown;  // 无深度附件
    desc.colorAttachmentCount = 1;
    desc.colorFormats[0]     = m_Cfg.format;          // 与历史/输出纹理格式一致
    desc.pushConstantRanges  = {pc};
    desc.descriptorSetLayouts = {m_Layout};
    desc.debugName           = m_Cfg.debugName;
    m_PSO = m_Device->CreatePipelineState(desc);
    HE_ASSERT(m_PSO, "RTDenoiser PSO 创建失败");
}

// ============================================================
// Shutdown — 释放资源
// ============================================================
void RTDenoiser::Shutdown() {
    if (m_Device && m_Set != rhi::kInvalidSet) m_Set = rhi::kInvalidSet;
    if (m_Device && m_Layout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_Layout);
        m_Layout = rhi::kInvalidLayout;
    }
    m_PSO.reset();
    m_OwnedHistory.reset();
    m_OwnedOutput.reset();
    m_History = nullptr;   // 池中的纹理由池持有，这里只断开引用
    m_Output  = nullptr;
    m_PointSampler.reset();
    m_Device = nullptr;
    m_Ready  = false;
    HE_CORE_INFO("RTDenoiser[%s] shutdown", m_Cfg.debugName);
}

// ============================================================
// OnResize — 重建纹理（分辨率变化时）
// ============================================================
void RTDenoiser::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width  = w;
    m_Height = h;
    m_OwnedHistory.reset();
    m_OwnedOutput.reset();
    m_History = nullptr;
    m_Output  = nullptr;
    CreateTextures(w, h);
    m_FrameIndex = 0;  // 重置历史：首帧使用当前帧初始化
}

// ============================================================
// SetInputs — 绑定当前帧输入纹理（Render 前调用）
// ============================================================
void RTDenoiser::SetInputs(rhi::IRHITexture* noisyColor, rhi::IRHITexture* depth,
                           rhi::IRHITexture* normal, rhi::IRHITexture* velocity) {
    m_NoisyColor = noisyColor;
    m_Depth      = depth;
    m_Normal     = normal;
    m_Velocity   = velocity;

    // 全部使用点采样，避免深度边界/法线方向因线性滤波失真
    auto bind = [&](u32 b, rhi::IRHITexture* t) {
        if (t) m_Device->UpdateDescriptorSet(m_Set, b,
            rhi::DescriptorType::CombinedImageSampler, t, m_PointSampler.get());
    };
    bind(0, noisyColor);
    bind(2, depth);
    bind(3, normal);
    bind(4, velocity);
}

// ============================================================
// PreBind — 绑定管线与动态状态（必须早于调用方的 BeginOffscreenPass）
//
// BeginOffscreenPass 用「当前绑定的 PSO」取 RenderPass 建 Framebuffer；此刻若绑着的是别的
// PSO（上一 Pass 的 RT 管线或带深度附件的图形 PSO），附件数就会不匹配：
// 实测报 VUID-VkFramebufferCreateInfo-attachmentCount-00876（1 个附件的 Framebuffer 对上
// 2 个附件的 RenderPass），随后设备挂住、整个进程再也不会推进（启用 RTGI 时稳定复现）。
// ============================================================
void RTDenoiser::PreBind(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !cmd) return;
    cmd->SetPipeline(m_PSO.get());
    cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
    cmd->SetScissor({0, 0, m_Width, m_Height});
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
}

// ============================================================
// Render — 执行时域累积降噪（调用方已 BeginOffscreenPass）+ 历史角色交换
// ============================================================
void RTDenoiser::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready) return;

    // 历史绑定：上一帧累积结果作为当前帧采样输入
    m_Device->UpdateDescriptorSet(m_Set, kRTDenoiseBindHistory,
        rhi::DescriptorType::CombinedImageSampler,
        m_History, m_PointSampler.get());

    // 首帧无历史数据 → shader 直接输出当前帧（初始化历史）
    const u32 isFirstFrame = (m_FrameIndex <= 1) ? 1u : 0u;

    // Push constant：texelSize + blend/thresholds + firstFrame + motionBlend（32B）
    struct {
        float2 texelSize;
        float  temporalBlend;
        float  depthThreshold;
        float  normalThreshold;
        u32    isFirstFrame;
        float  motionBlend;
        float  pad1;
    } pc;
    pc.texelSize      = float2(1.0f / (float)m_Width, 1.0f / (float)m_Height);
    pc.temporalBlend  = m_Cfg.temporalBlend;
    pc.depthThreshold = m_Cfg.depthThreshold;
    pc.normalThreshold = m_Cfg.normalThreshold;
    pc.isFirstFrame   = isFirstFrame;
    pc.motionBlend    = m_MotionBlend;   // 相机运动自适应（0=关闭，静止时沿用 temporalBlend）
    pc.pad1           = 0.0f;
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    // 离屏 Pass 由调用方（帧图的附属 pass 包装）负责 Begin/End：这里只负责画。
    // 管线在 PreBind 里绑定 —— 它必须早于 BeginOffscreenPass（见该函数的说明）。
    cmd->Draw(3);

    // 刚写入的输出成为下帧历史；原历史纹理成为下帧写入目标
    std::swap(m_History, m_Output);   // 裸指针交换（历史可能来自统一池，不可 swap 所有权）
    ++m_FrameIndex;
}

} // namespace he::render
