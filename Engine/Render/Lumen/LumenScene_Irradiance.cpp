// ============================================================
// LumenScene_Irradiance.cpp — 步骤 24：由探针 SH 采样出逐像素辐照度，并贴到 Provider 输出
//
// 【两段分工】
//   ① compute pass（`Lumen_ProbeIrradiance.comp.slang`）：每个屏幕像素按"16×16 单元 → 探针"映射
//      找到归属探针，用该探针的 SH 沿**像素法线**重建入射辐照度，乘 `albedo/π` 得到出射辐亮度，
//      写进屏幕尺寸的 RGBA16F 纹理；
//   ② 全屏 pass（`Lumen_Irradiance.frag.slang`）：帧图的 offscreen pass 只能把输出当作**颜色附件**
//      写入，所以用一条全屏三角形把这张纹理采样出来写进 Provider 的输出（Lighting 照旧采样它）。
//
// 【为什么不让 compute 直接写 Provider 输出】输出纹理在本帧被 offscreen pass 用作颜色附件，
// 又在其它 pass 里被当 storage image 写，会踩到 layout/依赖冲突（RHI 只保证"自持资源自管 barrier"）。
// 分成"compute 写中间纹理 + 全屏贴图"两段，边界清晰，也不动 Lighting 侧任何代码。
// ============================================================

#include "Lumen/LumenScene.h"

#include "Core/Log.h"
#include "Lumen_ScreenProbe_SHProject.comp.spv.h"
#include "Lumen_ProbeIrradiance.comp.spv.h"
#include "Lumen_Irradiance.frag.spv.h"
#include "SSAO.vert.spv.h"

#include <cstring>

namespace he::render {

void LumenScene::CreateIrradianceTexture() {
    if (!m_Device || m_Width == 0 || m_Height == 0) return;
    rhi::TextureDesc td;
    td.width  = m_Width;
    td.height = m_Height;
    td.depth  = 1;
    td.format = rhi::Format::RGBA16_FLOAT;
    // UnorderedAccess：compute 写入（走 UpdateDescriptorSetWithImageView，见 RHI 的存储图像约束）
    // ShaderResource：全屏 pass 采样
    td.usage  = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
    m_IrradianceTex = m_Device->CreateTexture(td);
    m_IrrBound = false;   // 纹理换了 ⇒ 描述符要重绑

    if (!m_IrradianceStatsBuf) {
        rhi::BufferDesc sb;
        sb.size = sizeof(u32) * 4u; sb.usage = rhi::BufferUsage::Storage; sb.cpuAccess = true;
        m_IrradianceStatsBuf = m_Device->CreateBuffer(sb);
        m_IrrStatsMapped = m_IrradianceStatsBuf->Map();
    }
}

void LumenScene::CreateIrradianceGPUObjects() {
    if (m_IrrPSO) return;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::StorageBuffer,        1, rhi::kStageMaskCompute},   // 探针
        {1, rhi::DescriptorType::StorageBuffer,        1, rhi::kStageMaskCompute},   // 单元 → 探针
        {2, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // GBuffer 法线
        {3, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // GBuffer albedo
        {4, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // GBuffer 世界坐标
        {5, rhi::DescriptorType::StorageImage,         1, rhi::kStageMaskCompute},   // 输出辐照度纹理
        {6, rhi::DescriptorType::StorageBuffer,        1, rhi::kStageMaskCompute},   // 统计
    };
    m_IrrLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_IrrSet    = m_Device->AllocateDescriptorSet(m_IrrLayout);

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskCompute;
    pcr.offset    = 0;
    pcr.size      = 2u * 16u;   // uint4 dims + float4 params

    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_Lumen_ProbeIrradiance_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc pso;
    pso.bindPoint            = rhi::PipelineBindPoint::Compute;
    pso.computeShader        = &cs;
    pso.descriptorSetLayouts = {m_IrrLayout};
    pso.pushConstantRanges   = {pcr};
    pso.debugName            = "Lumen_ProbeIrradiance";
    m_IrrPSO = m_Device->CreatePipelineState(pso);
    if (!m_IrrPSO) HE_CORE_ERROR("LumenScene: 逐像素辐照度 PSO 创建失败");
}

void LumenScene::RunProbeIrradiance(rhi::IRHICommandList* cmd, rhi::IRHITexture* gbNormal,
                                    rhi::IRHITexture* gbAlbedo, rhi::IRHITexture* gbWorldPos) {
    if (!m_Device || !cmd || !m_ProbeBuf || !m_CellProbeBuf || !m_IrradianceTex) return;
    if (!gbNormal || !gbAlbedo || !gbWorldPos || !m_ProbeCount) return;

    if (!m_IrrBound) {
        CreateIrradianceGPUObjects();
        if (!m_IrrPSO) return;
        m_Device->UpdateDescriptorSet(m_IrrSet, 0, rhi::DescriptorType::StorageBuffer, m_ProbeBuf.get());
        m_Device->UpdateDescriptorSet(m_IrrSet, 1, rhi::DescriptorType::StorageBuffer, m_CellProbeBuf.get());
        m_Device->UpdateDescriptorSetWithImageView(m_IrrSet, 5,
            rhi::DescriptorType::StorageImage, m_IrradianceTex->GetNativeHandle());
        m_Device->UpdateDescriptorSet(m_IrrSet, 6, rhi::DescriptorType::StorageBuffer, m_IrradianceStatsBuf.get());
        m_IrrBound = true;
        return;   // 本帧只建资源；下一帧开始派发（"新建资源当帧使用"的教训见 §附二十）
    }
    // GBuffer 随 resize 换纹理 ⇒ 每帧重绑这三张
    m_Device->UpdateDescriptorSet(m_IrrSet, 2, rhi::DescriptorType::CombinedImageSampler,
                                  gbNormal, m_SDF.GetLinearSampler());
    m_Device->UpdateDescriptorSet(m_IrrSet, 3, rhi::DescriptorType::CombinedImageSampler,
                                  gbAlbedo, m_SDF.GetLinearSampler());
    m_Device->UpdateDescriptorSet(m_IrrSet, 4, rhi::DescriptorType::CombinedImageSampler,
                                  gbWorldPos, m_SDF.GetLinearSampler());

    // ① 先读上一帧的统计（同"先读后清"）
    if (m_IrrFrame >= 1 && m_IrrStatsMapped) {
        u32 st[4] = {0, 0, 0, 0};
        std::memcpy(st, m_IrrStatsMapped, sizeof(st));
        m_IrradianceCovered = st[0];
        m_IrradianceMean = st[0] ? (float)((double)st[1] / 1024.0 / (double)st[0]) : 0.0f;
        m_IrradianceMax  = (float)((double)st[2] / 65536.0);
        if ((m_IrrFrame % 40u) == 0u) {
            HE_CORE_INFO("LumenScene 逐像素辐照度（步骤 24）: 覆盖像素 {}（{:.1f}% 屏幕）；入射辐照度亮度 "
                         "均值 {:.5f}、最大 {:.5f}（辐照度，未乘 albedo/π）；帧计数 trace {} / sh {} / irr {} / probe {}",
                         m_IrradianceCovered,
                         (m_Width * m_Height) ? 100.0 * (double)m_IrradianceCovered / (double)(m_Width * m_Height) : 0.0,
                         (double)m_IrradianceMean, (double)m_IrradianceMax,
                         m_TraceFrame, m_SHFrame, m_IrrFrame, m_ProbeFrame);
        }
    }

    // ② 清零统计并派发
    if (m_IrrStatsMapped) { u32 zero[4] = {0, 0, 0, 0}; std::memcpy(m_IrrStatsMapped, zero, sizeof(zero)); }
    struct { u32 x, y, z, w; float4 params; } pc{};
    const u32 cellsX = (m_Width + 15u) / 16u;
    pc.x = m_Width; pc.y = m_Height; pc.z = cellsX; pc.w = 0u;
    pc.params = float4(0.0f, 0.0f, 0.0f, 0.0f);   // x = 1 时只输出辐照度（调试）
    ComputeBarrier(cmd);   // 等"SH pass 写回探针系数"落地
    cmd->SetPipeline(m_IrrPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_IrrSet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch((m_Width + 7u) / 8u, (m_Height + 7u) / 8u, 1);
    ++m_IrrFrame;

    // 【验收模式：可复现】
    // Lumen 内部多处要把 GPU 原子计数/缓冲回读到 CPU（探针数、请求列表、页表状态），再据此决定
    // 下一帧的行为。映射内存的**读取时机**取决于 CPU 相对 GPU 领先多少 —— 领先量随每帧的墙钟
    // 时间抖动 ⇒ 同一配置两次运行的中间状态会差一点点（实测：追踪命中 42393 vs 42391，
    // 逐像素辐照度均值 0.54495 vs 0.53749，转储逐像素不同）。这与"背靠背读数逐项一致"的验收
    // 口径冲突，所以给一个**显式**的确定性模式：每帧在这里把 GPU 等干净，让回读位置固定。
    // 只由环境变量开启（默认关，避免把每帧同步带进常规运行）。
    if (m_Deterministic) m_Device->WaitIdle();
}

void LumenScene::CreateIrradianceCopyPipeline() {
    if (!m_Device || m_IrrCopyPSO) return;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},   // 辐照度纹理
    };
    m_IrrCopyLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_IrrCopySet    = m_Device->AllocateDescriptorSet(m_IrrCopyLayout);

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskFragment;
    pcr.offset    = 0;
    pcr.size      = sizeof(float4);   // params

    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_Lumen_Irradiance_frag_spv;
    fs.entryPoint = "fragmentMain";

    rhi::PipelineStateDesc d;
    d.vertexShader         = &vs;
    d.pixelShader          = &fs;
    d.topology             = rhi::PrimitiveTopology::TriangleList;
    d.depthTest            = false;
    d.depthWrite           = false;
    d.depthFormat          = rhi::Format::Unknown;   // 与 BeginOffscreenPass(nullptr 深度) 一致
    d.colorAttachmentCount = 1;
    d.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
    d.descriptorSetLayouts = {m_IrrCopyLayout};
    d.pushConstantRanges   = {pcr};
    d.debugName            = "Lumen_Irradiance";
    m_IrrCopyPSO = m_Device->CreatePipelineState(d);
    if (!m_IrrCopyPSO) HE_CORE_ERROR("LumenScene: 辐照度输出 pass 的 PSO 创建失败");

    if (m_IrradianceTex) {
        m_Device->UpdateDescriptorSet(m_IrrCopySet, 0, rhi::DescriptorType::CombinedImageSampler,
                                      m_IrradianceTex.get(), GetOutputSampler());
    }
}

void LumenScene::DrawIrradiance(rhi::IRHICommandList* cmd, float gain) {
    if (!cmd) return;
    CreateIrradianceCopyPipeline();
    if (!m_IrrCopyPSO || !m_IrradianceTex) return;
    // 纹理可能刚随 resize 重建 ⇒ 每次绑定（一帧一次，代价可忽略）
    m_Device->UpdateDescriptorSet(m_IrrCopySet, 0, rhi::DescriptorType::CombinedImageSampler,
                                  m_IrradianceTex.get(), GetOutputSampler());
    const float4 pc(gain, 1.0f, 0.0f, 0.0f);
    cmd->SetPipeline(m_IrrCopyPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_IrrCopySet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Draw(3);
}

} // namespace he::render
