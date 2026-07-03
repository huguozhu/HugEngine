#include "AntiAliasing/AA_TAA.h"
#include "TAA_Resolve.vert.spv.h"
#include "TAA_Resolve.frag.spv.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <glm/gtc/matrix_transform.hpp>

namespace he::render {

// ── Halton(2, 3) 序列，8 样本循环 ──
float2 AA_TAA::HaltonSample(u32 index) {
    // Halton(2): 0.5, -0.5, 0.25, -0.75, 0.875, -0.875, -0.125, 0.125
    // Halton(3): 0.333, -0.333, -0.111, 0.778, -0.556, -0.222, 0.444, -0.444
    static const float kHaltonX[8] = { 0.5f, -0.5f,  0.25f, -0.75f, 0.875f, -0.875f, -0.125f, 0.125f };
    static const float kHaltonY[8] = { 0.333f, -0.333f, -0.111f, 0.778f, -0.556f, -0.222f, 0.444f, -0.444f };
    u32 i = index % 8;
    return {kHaltonX[i], kHaltonY[i]};
}

bool AA_TAA::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    HE_ASSERT(m_Device, "AA_TAA: null device");

    // 描述符布局：bindings 0-4 为输入纹理，binding 5 为 uniform buffer
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // u_CurrentColor
        {1, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // u_HistoryColor
        {2, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // u_Depth
        {3, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // u_Normal
        {4, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // u_Velocity
        {5, rhi::DescriptorType::UniformBuffer, 1, 17},         // u_TAAUniforms
    };
    m_DescLayout = device->CreateDescriptorSetLayout(layout);
    m_DescSet    = device->AllocateDescriptorSet(m_DescLayout);

    // Uniform buffer（144 bytes，每帧更新 prevViewProj + invCurrViewProj + resolution）
    m_UniformBuffer = device->CreateBuffer({144, rhi::BufferUsage::Uniform});

    // 绑定 uniform buffer 到描述符（内容每帧 Map/Unmap 更新）
    device->UpdateDescriptorSet(m_DescSet, 5, rhi::DescriptorType::UniformBuffer,
                                m_UniformBuffer.get());

    // 历史缓冲
    CreateHistoryTextures(width, height);

    // PSO
    CreatePSO();

    m_Ready = true;
    HE_CORE_INFO("AA_TAA initialized ({}, {})", width, height);
    return true;
}

void AA_TAA::CreateHistoryTextures(u32 w, u32 h) {
    for (int i = 0; i < 2; ++i) {
        rhi::TextureDesc d;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.width  = w;
        d.height = h;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_HistoryColor[i] = m_Device->CreateTexture(d);
    }
    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU  = sd.addressV  = rhi::AddressMode::ClampToEdge;
    m_HistorySampler = m_Device->CreateSampler(sd);

    // 点采样器：用于 current color、depth、normal、velocity 等不需要插值的纹理
    rhi::SamplerDesc psd;
    psd.minFilter = psd.magFilter = rhi::FilterMode::Nearest;
    psd.addressU  = psd.addressV  = rhi::AddressMode::ClampToEdge;
    m_PointSampler = m_Device->CreateSampler(psd);
}

void AA_TAA::CreatePSO() {
    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_TAA_Resolve_vert_spv;
    vs.entryPoint = "main";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_TAA_Resolve_frag_spv;
    fs.entryPoint = "main";

    rhi::PushConstantRange pc;
    pc.stageMask = 1 | 16;  // Vertex | Fragment
    pc.size      = 16;       // float2 jitterOffset

    rhi::PipelineStateDesc desc;
    desc.vertexShader        = &vs;
    desc.pixelShader         = &fs;
    desc.topology            = rhi::PrimitiveTopology::TriangleList;
    desc.depthTest           = false;
    desc.depthWrite          = false;
    desc.colorAttachmentCount = 1;
    desc.colorFormats[0]     = rhi::Format::RGBA16_FLOAT;  // HDR 输出
    desc.pushConstantRanges  = {pc};
    desc.descriptorSetLayouts = {m_DescLayout};
    desc.debugName            = "AA_TAA";
    m_PSO = m_Device->CreatePipelineState(desc);
    HE_ASSERT(m_PSO, "AA_TAA: PSO creation failed");
}

void AA_TAA::Shutdown() {
    // 释放描述符集（RHI 使用 pool 自动回收，此处重置句柄保证安全）
    if (m_Device && m_DescSet != rhi::kInvalidSet) {
        m_DescSet = rhi::kInvalidSet;
    }
    if (m_Device && m_DescLayout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    }
    m_PSO.reset();
    m_UniformBuffer.reset();
    m_HistoryColor[0].reset();
    m_HistoryColor[1].reset();
    m_HistorySampler.reset();
    m_PointSampler.reset();
    m_Device = nullptr;
    m_Ready  = false;
    HE_CORE_INFO("AA_TAA shutdown");
}

void AA_TAA::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width  = w;
    m_Height = h;
    m_HistoryColor[0].reset();
    m_HistoryColor[1].reset();
    CreateHistoryTextures(w, h);
    // 首帧使用当前帧 → 重设 read/write
    m_HistoryRead  = 0;
    m_HistoryWrite = 1;
    m_JitterIndex = 0;  // 重置抖动序列，与新历史缓冲对齐
}

void AA_TAA::SetInput(rhi::IRHITexture* color, rhi::IRHISampler* /*sampler*/) {
    m_InputColor = color;
    if (m_InputColor) {
        // CurrentColor 使用点采样（最近邻），避免对已抖动 HDR 颜色做线性插值模糊
        m_Device->UpdateDescriptorSet(m_DescSet, 0, rhi::DescriptorType::CombinedImageSampler,
                                      m_InputColor, m_PointSampler.get());
    }
}

void AA_TAA::SetGBufferInputs(rhi::IRHITexture* depth,
                               rhi::IRHITexture* normal,
                               rhi::IRHITexture* velocity) {
    m_DepthTexture    = depth;
    m_NormalTexture   = normal;
    m_VelocityTexture = velocity;

    // Depth/Normal/Velocity 使用点采样（最近邻），避免深度边界或法线方向因线性滤波失真
    auto bind = [&](u32 b, rhi::IRHITexture* t) {
        if (t) m_Device->UpdateDescriptorSet(m_DescSet, b, rhi::DescriptorType::CombinedImageSampler,
                                             t, m_PointSampler.get());
    };
    bind(2, depth);
    bind(3, normal);
    bind(4, velocity);
}

void AA_TAA::UpdateUniforms(const float4x4& prevViewProj,
                             const float4x4& invCurrViewProj,
                             u32 width, u32 height) {
    // TAAUniforms 布局（144B）：
    //   float4x4 prevViewProj      [0, 64)
    //   float4x4 invCurrViewProj   [64, 128)
    //   float2   resolution        [128, 136)
    //   float    blendFactor       [136, 140)
    //   float    unused            [140, 144)

    struct {
        float4x4 prevViewProj;
        float4x4 invCurrViewProj;
        float2   resolution;
        float    blendFactor;
        float    unused;
    } uniforms;

    uniforms.prevViewProj    = prevViewProj;
    uniforms.invCurrViewProj = invCurrViewProj;
    uniforms.resolution      = {(float)width, (float)height};
    uniforms.blendFactor     = 0.05f;
    uniforms.unused          = 0.0f;

    void* mapped = m_UniformBuffer->Map();
    if (mapped) {
        memcpy(mapped, &uniforms, sizeof(uniforms));
        m_UniformBuffer->Unmap();
    }
}

float2 AA_TAA::GetJitterOffset() const {
    return m_CurrentJitter;
}

void AA_TAA::OnBeginFrame() {
    // 推进抖动序列
    float2 rawJitter = HaltonSample(m_JitterIndex);
    m_JitterIndex = (m_JitterIndex + 1) % 8;

    // 缩放为 pixel → NDC offset
    m_CurrentJitter.x = rawJitter.x * 2.0f / (float)m_Width;
    m_CurrentJitter.y = rawJitter.y * 2.0f / (float)m_Height;

    // 交换历史缓冲 read/write（本次 TAA resolve 读取上一帧的历史，写入当前帧）
    m_HistoryWrite = m_HistoryRead;
    m_HistoryRead  = 1 - m_HistoryRead;

    ++m_FrameIndex;
}

rhi::IRHITexture* AA_TAA::GetOutputTexture() const {
    return m_HistoryColor[m_HistoryRead].get();
}

rhi::IRHISampler* AA_TAA::GetOutputSampler() const {
    return m_HistorySampler.get();
}

void AA_TAA::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled) return;

    // 更新 history 纹理绑定到当前 read buffer
    m_Device->UpdateDescriptorSet(m_DescSet, 1, rhi::DescriptorType::CombinedImageSampler,
                                  m_HistoryColor[m_HistoryRead].get(), m_HistorySampler.get());

    cmd->SetPipeline(m_PSO.get());
    cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
    cmd->SetScissor({0, 0, m_Width, m_Height});
    cmd->BindDescriptorSet(0, m_DescSet);

    // Push constant: 全 16B 范围（jitterOffset 8B + zero padding 8B）
    struct { float2 jitter; float2 _pad; } pc = { m_CurrentJitter, {0.0f, 0.0f} };
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    // 在 render pass 内执行 TAA resolve 绘制（自拥有输出纹理，无深度）
    cmd->BeginOffscreenPass(m_HistoryColor[m_HistoryWrite]->GetNativeHandle(),
                            nullptr, m_Width, m_Height, nullptr, false);
    // 全屏三角形
    cmd->Draw(3);
    cmd->EndOffscreenPass();
}

} // namespace he::render
