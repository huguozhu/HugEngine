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
#include "Lumen_FarFieldCompare.comp.spv.h"
#include "Lumen_FarFieldMerge.comp.spv.h"
#include "Lumen_Irradiance.frag.spv.h"
#include "SSAO.vert.spv.h"

#include <cstring>

namespace he::render {

void LumenScene::CreateIrradianceTexture() {
    if (!m_Device || m_Width == 0 || m_Height == 0) return;
    // 【步骤 37 诊断】这张纹理每重建一次，存储图像描述符就要重绑一次；重建次数不应该是"每帧"。
    static u32 s_irrTexCreateCount = 0;
    ++s_irrTexCreateCount;
    if (std::getenv("HE_LUMEN_TRACE_OUT")) {
        HE_CORE_INFO("CreateIrradianceTexture: 第 {} 次创建（{}x{}）", s_irrTexCreateCount, m_Width, m_Height);
    }
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
        // 【步骤 35】绑定**过滤后**的探针缓冲：辐照度是探针 SH 的逐像素重建，
        // 读未过滤的一份就等于把刚做的滤波丢掉（滤波 pass 每帧都会写出这一份）。
        m_Device->UpdateDescriptorSet(m_IrrSet, 0, rhi::DescriptorType::StorageBuffer,
                                      m_ProbeFilteredBuf ? m_ProbeFilteredBuf.get() : m_ProbeBuf.get());
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

    // 【步骤 37】辐照度纹理是自持存储图像（compute 以 RWTexture2D 写），创建点没有命令列表，
    // 故在首次写入之前转换一次（理由见 LumenSDF::BakeOne 的同一处说明）。
    if (!m_IrradianceTransitioned && m_IrradianceTex) {
        m_IrradianceTransitioned = true;
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::Undefined, rhi::ResourceState::UnorderedAccess,
                             m_IrradianceTex.get());
    }

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

// ============================================================
// 步骤 26：远场硬件光追 —— 与 SDF 追踪逐光线对照
// ============================================================
// ============================================================
// 步骤 29（L4 退出判据）：显存增量清单
//
// 【为什么要一行行报出来】§15.3 是"按设计量级做预算"的测算表，而实现里每一块的真实尺寸只能从
// 代码里读到（缓冲区按容量分配、atlas 按物理页数、SDF 按 mesh 数与分辨率）。把"实测占用"
// 逐条打出来，才能和预算表对照，也才能在加功能时立刻看见"这一版又多了多少 MB"。
// ============================================================
void LumenScene::LogMemoryUsage() const {
    struct Item { const char* name; double mb; };
    std::vector<Item> items;
    const auto addBuf = [&](const char* name, const rhi::IRHIBuffer* b) {
        if (b) items.push_back({name, (double)b->GetSize() / (1024.0 * 1024.0)});
    };

    // 世界空间（与视口无关）
    items.push_back({"Mesh SDF（逐 mesh 128³ R16F）", m_SDF.GetMeshFieldBytes() / (1024.0 * 1024.0)});
    // 全局 clipmap 每层是 R32U + R32F 两张 128³（引擎初始化日志里的"16.00 MB/层"就是它们）
    items.push_back({"Global SDF clipmap（2 层 × (R32U + R32F) 128³）",
                     2.0 * (double)m_SDF.GetGlobalResolution() * m_SDF.GetGlobalResolution() *
                     m_SDF.GetGlobalResolution() * 4.0 * 2.0 / (1024.0 * 1024.0)});
    items.push_back({"Surface Cache atlas（3 × 512² RGBA16F）", 3.0 * 512.0 * 512.0 * 8.0 / (1024.0 * 1024.0)});

    // 屏幕尺寸（与视口相关）
    const double px = (double)m_Width * (double)m_Height;
    items.push_back({"Lumen 输出 + 辐照度（2 × 屏幕 RGBA16F）", 2.0 * px * 8.0 / (1024.0 * 1024.0)});
    addBuf("卡片清单 / 页表镜像", m_ShadeCardsBuf.get());
    addBuf("页表镜像", m_PageTableBuf.get());
    addBuf("命中点缓冲", m_RayHitPosBuf.get());
    addBuf("光线结果缓冲", m_RayResultBuf.get());
    addBuf("着色输出（atlas / GBuffer / 最优候选）", m_ShadeOutBuf.get());
    addBuf("着色输出（GBuffer 对照）", m_ShadeOutGbBuf.get());
    addBuf("着色输出（最优候选）", m_ShadeOutBestBuf.get());
    addBuf("远场光追结果（2 × 光线）", m_FarField ? m_FarField->GetResultBuffer() : nullptr);
    addBuf("远场切换 fade（2 × 光线）", m_RayFadeBuf.get());
    addBuf("探针缓冲（65536 × 96 B）", m_ProbeBuf.get());
    addBuf("单元 → 探针映射", m_CellProbeBuf.get());
    addBuf("SH 重建/参考辐照度（2 × 探针）", m_IrradShBuf.get());

    double total = 0.0;
    for (const auto& it : items) total += it.mb;
    HE_CORE_INFO("Lumen 显存清单（步骤 29 / 对照 §15.3 的预算表）—— 视口 {}×{}", m_Width, m_Height);
    for (const auto& it : items) {
        if (it.mb > 0.0) HE_CORE_INFO("   {:<44} {:8.2f} MB", it.name, it.mb);
    }
    HE_CORE_INFO("   {:<44} {:8.2f} MB", "合计（Lumen 自持资源）", total);
}
void LumenScene::CreateFarFieldCompareGPUObjects() {
    if (m_FarCmpPSO && m_FarMergePSO) return;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // SDF 光线结果
        {1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 光追光线结果
        {2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 分类统计
        {3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 相对误差直方图
    };
    m_FarCmpLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_FarCmpSet    = m_Device->AllocateDescriptorSet(m_FarCmpLayout);

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskCompute;
    pcr.offset    = 0;
    pcr.size      = 16u;

    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_Lumen_FarFieldCompare_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc pso;
    pso.bindPoint            = rhi::PipelineBindPoint::Compute;
    pso.computeShader        = &cs;
    pso.descriptorSetLayouts = {m_FarCmpLayout};
    pso.pushConstantRanges   = {pcr};
    pso.debugName            = "Lumen_FarFieldCompare";
    m_FarCmpPSO = m_Device->CreatePipelineState(pso);

    rhi::BufferDesc sb;
    sb.size = sizeof(u32) * 24u; sb.usage = rhi::BufferUsage::Storage; sb.cpuAccess = true;
    m_FarCmpStatsBuf = m_Device->CreateBuffer(sb);
    rhi::BufferDesc msb;
    msb.size = sizeof(u32) * 8u; msb.usage = rhi::BufferUsage::Storage; msb.cpuAccess = true;
    m_FarMergeStatsBuf = m_Device->CreateBuffer(msb);
    m_FarMergeStatsMapped = m_FarMergeStatsBuf->Map();
    // 距离分桶缓冲在 CreateShadeGPUObjects 里已建（着色 pass 的绑定 11）
    m_FarCmpStatsMapped = m_FarCmpStatsBuf->Map();
    rhi::BufferDesc hb;
    hb.size = sizeof(u32) * 16u; hb.usage = rhi::BufferUsage::Storage; hb.cpuAccess = true;
    m_FarCmpHistBuf = m_Device->CreateBuffer(hb);
    m_FarCmpHistMapped = m_FarCmpHistBuf->Map();
    m_FarFieldHist.assign(16u, 0u);

    m_Device->UpdateDescriptorSet(m_FarCmpSet, 0, rhi::DescriptorType::StorageBuffer, m_RayResultBuf.get());
    m_Device->UpdateDescriptorSet(m_FarCmpSet, 2, rhi::DescriptorType::StorageBuffer, m_FarCmpStatsBuf.get());
    m_Device->UpdateDescriptorSet(m_FarCmpSet, 3, rhi::DescriptorType::StorageBuffer, m_FarCmpHistBuf.get());
    if (!m_FarCmpPSO) HE_CORE_ERROR("LumenScene: 远场对照 PSO 创建失败");

    // ── 合并 pass：把远场光追的命中并进探针光线结果（见 shader 头的理由）──
    rhi::DescriptorSetLayoutDesc ml;
    ml.bindings = {
        {0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // SDF 结果（就地改写）
        {1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 光追结果
        {2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 命中点
        {3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 步骤 27：副命中点 + 混合权重
        {4, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 合并分类统计
    };
    m_FarMergeLayout = m_Device->CreateDescriptorSetLayout(ml);
    m_FarMergeSet    = m_Device->AllocateDescriptorSet(m_FarMergeLayout);
    rhi::PushConstantRange mpc;
    mpc.stageMask = rhi::kStageMaskCompute; mpc.offset = 0; mpc.size = 16u;
    rhi::ShaderBytecode mcs;
    mcs.stage = rhi::ShaderStage::Compute;
    mcs.spirv = k_Lumen_FarFieldMerge_comp_spv;
    mcs.entryPoint = "main";
    rhi::PipelineStateDesc mpso;
    mpso.bindPoint            = rhi::PipelineBindPoint::Compute;
    mpso.computeShader        = &mcs;
    mpso.descriptorSetLayouts = {m_FarMergeLayout};
    mpso.pushConstantRanges   = {mpc};
    mpso.debugName            = "Lumen_FarFieldMerge";
    m_FarMergePSO = m_Device->CreatePipelineState(mpso);
    m_Device->UpdateDescriptorSet(m_FarMergeSet, 0, rhi::DescriptorType::StorageBuffer, m_RayResultBuf.get());
    m_Device->UpdateDescriptorSet(m_FarMergeSet, 2, rhi::DescriptorType::StorageBuffer, m_RayHitPosBuf.get());
    m_Device->UpdateDescriptorSet(m_FarMergeSet, 4, rhi::DescriptorType::StorageBuffer, m_FarMergeStatsBuf.get());
    if (m_RayFadeBuf)   // 在着色 pass 的资源创建里已经建好（见 CreateShadeGPUObjects）
        m_Device->UpdateDescriptorSet(m_FarMergeSet, 3, rhi::DescriptorType::StorageBuffer, m_RayFadeBuf.get());
    // 绑定 1（光追结果）此时还不存在（它由第一次 Trace 创建）⇒ 在派发合并之前才绑。
    // 这是本轮第三次踩同一个坑："把结果缓冲当前置条件"在任何一条路径上都会变成死锁或空指针。
}

void LumenScene::RunFarFieldRT(rhi::IRHICommandList* cmd) {
    // 桶均值：亮度以 ×4096 定点累加（桶内条数有限，不会溢出 u32）
    auto BinMean = [this](u32 i) {
        return (m_DistBinCount[i] && i < m_DistBinLum.size())
             ? (double)m_DistBinLum[i] / 4096.0 / (double)m_DistBinCount[i] : 0.0;
    };
    // 【一次性诊断】步骤 26 首次落地时，任何一个前置为空的静默返回都会让"远场根本没跑"变成
    // 一条不打印任何东西的日志 —— 这里显式报一次原因，避免又走一遍"猜为什么没输出"的老路。
    static bool s_farLogged = false;
    if (!s_farLogged) {
        s_farLogged = true;
        HE_CORE_INFO("LumenScene 远场光追前置检查: device={} cmd={} probe={} tlas={} rayResult={} traceFrame={}",
                     (void*)m_Device, (void*)cmd, (void*)m_ProbeBuf.get(), (void*)m_TLAS,
                     (void*)m_RayResultBuf.get(), m_TraceFrame);
    }
    if (!m_Device || !cmd || !m_ProbeBuf || !m_TLAS || !m_RayResultBuf) return;
    if (m_TraceFrame == 0u) return;   // 还没追踪过 ⇒ 没有可比的光线

    if (!m_FarField) {
        m_FarField = std::make_unique<LumenFarFieldPass>();
        if (!m_FarField->Initialize(m_Device)) { m_FarField.reset(); return; }
        CreateFarFieldCompareGPUObjects();
        // 【不能在这里绑结果缓冲】它要到第一次 Trace（EnsureCapacity）才存在；绑 nullptr 会崩
        //（第一次就是这么崩的）。绑定改在 Trace 之后按需重绑。
        return;
    }
    // 【别用 IsValid() 当这里的门】它包含 `结果缓冲非空`，而结果缓冲正是**这次 Trace** 才建的
    // ⇒ 用它当门会永远提前返回（第一次就是这么写的，表现是"初始化成功但一条光线都没发"）。
    if (!m_FarField || !m_FarCmpPSO) return;

    // 步骤 29：资源都建齐之后报一次显存清单（对照 §15.3 的预算表）。
    // 【注意】m_FarFieldFrame 在函数尾部才自增 ⇒ 这里读到的是"上一帧"的计数，== 20 即第 21 帧。
    if (m_FarFieldFrame == 60u) LogMemoryUsage();   // 等卡片/mesh 场都建齐再报

    // ── ① 先读**上一帧**的对照统计（同"先读后清"；本帧紧接着会把统计清零再派发）──
    if (m_FarFieldFrame >= 1u && m_FarCmpStatsMapped) {
        u32 st[24] = {0};
        std::memcpy(st, m_FarCmpStatsMapped, sizeof(st));
        const u32 rays = m_FarFieldRays;   // 上一次派发的光线数（与本批统计同批）
        m_FarFieldBothHit = st[0];
        m_FarFieldSdfOnly = st[1];
        m_FarFieldRtOnly  = st[2];
        const u32 both = st[0];
        // 累加倍数是 1024（不是 1e6）：4.5 万 × 0.5 × 1e6 会超过 u32 上限并把读数冲掉
        m_FarFieldMeanRel = both ? (float)((double)st[4] / 1024.0 / (double)both) : 0.0f;
        m_FarFieldMaxRel  = (float)((double)st[5] / 1048576.0);
        m_FarFieldAgree5  = both ? (float)((double)st[6] / (double)both) : 0.0f;
        m_FarFieldAgree20 = both ? (float)((double)st[7] / (double)both) : 0.0f;
        const u32 sdfHits = st[12];
        m_FarFieldSelfHits = st[22];
        m_FarFieldSdfMeanT = sdfHits ? (float)((double)st[23] / 16.0 / (double)sdfHits) : 0.0f;
        const u32 nearHit = st[14], farHit = st[17];
        m_FarFieldNear20 = nearHit ? (float)((double)st[15] / (double)nearHit) : 0.0f;
        m_FarFieldNearMeanRel = nearHit ? (float)((double)st[20] / 1024.0 / (double)nearHit) : 0.0f;
        m_FarFieldFar20 = farHit ? (float)((double)st[18] / (double)farHit) : 0.0f;
        m_FarFieldFarMeanRel = farHit ? (float)((double)st[21] / 1024.0 / (double)farHit) : 0.0f;
        if (m_FarCmpHistMapped) std::memcpy(m_FarFieldHist.data(), m_FarCmpHistMapped, 16u * sizeof(u32));
        if ((m_FarFieldFrame % 40u) == 0u) {
            HE_CORE_INFO("LumenScene 远场光追（步骤 26）: 光线 {}；两侧都命中 {}（SDF 命中率 {:.1f}%、RT 命中率 {:.1f}%）；"
                         "只有 SDF 命中 {} / 只有三角形命中 {}；两者都未命中 {}；全部命中光线 相对差 均值 {:.4f} 最大 {:.4f}，"
                         "≤5% 占 {:.1f}%、≤20% 占 {:.1f}%",
                         rays, both,
                         rays ? 100.0 * (double)st[12] / (double)rays : 0.0,
                         rays ? 100.0 * (double)st[11] / (double)rays : 0.0,
                         st[1], st[2], st[3],
                         (double)m_FarFieldMeanRel, (double)m_FarFieldMaxRel,
                         100.0 * (double)m_FarFieldAgree5, 100.0 * (double)m_FarFieldAgree20);
            HE_CORE_INFO("   分带对照（分界 {:.0f} 单位）: 近带 {} 条 —— 相对差 均值 {:.4f}、≤20% 占 {:.1f}%；"
                         "远带 {} 条 —— 相对差 均值 {:.4f}、≤20% 占 {:.1f}%；相对差直方图(0..1 分 16 桶) {} {} {} {} "
                         "{} {} {} {} {} {} {} {} {} {} {} {}",
                         (double)m_FarFieldNearBand, nearHit, (double)m_FarFieldNearMeanRel, 100.0 * (double)m_FarFieldNear20,
                         farHit, (double)m_FarFieldFarMeanRel, 100.0 * (double)m_FarFieldFar20,
                         m_FarFieldHist[0], m_FarFieldHist[1], m_FarFieldHist[2], m_FarFieldHist[3],
                         m_FarFieldHist[4], m_FarFieldHist[5], m_FarFieldHist[6], m_FarFieldHist[7],
                         m_FarFieldHist[8], m_FarFieldHist[9], m_FarFieldHist[10], m_FarFieldHist[11],
                         m_FarFieldHist[12], m_FarFieldHist[13], m_FarFieldHist[14], m_FarFieldHist[15]);
            HE_CORE_INFO("   SDF 命中距离: 均值 {:.2f}（定点 1/16）；其中 t < 1 的**自交命中** {} 条（占 SDF 命中 {:.1f}%）",
                         (double)m_FarFieldSdfMeanT, m_FarFieldSelfHits,
                         sdfHits ? 100.0 * (double)m_FarFieldSelfHits / (double)sdfHits : 0.0);
            // 步骤 27：切换分类 + 按命中距离分桶的最终材质亮度（10 单位/桶，阈值 50 在第 5 桶）
            HE_CORE_INFO("   步骤 27 切换: 纯 SDF {} / 纯光追 {} / **重叠带混合 {}**（重叠带 [{:.0f},{:.0f}]，半宽比例 {:.2f}）；"
                         "带内实际混合的光线 {}",
                         m_FadeSdfOnly, m_FadeRtOnly, m_FadeBlend,
                         (double)(m_FarFieldThreshold * (1.0f - m_FarFieldOverlap)),
                         (double)(m_FarFieldThreshold * (1.0f + m_FarFieldOverlap)),
                         (double)m_FarFieldOverlap, m_FadeBlendedRays);
            HE_CORE_INFO("   带内细分: 进入混合分支 {} / 副点材质可取 {} / 副点缺页（朝远场兜底值混合）{}；"
                         "副点失败拆分: 不在任何卡内 {} / 卡内但页无内容 {}",
                         m_FadeBandRays, m_FadeBlendedRays, m_FadeNoAltRays,
                         m_FadeAltNoCard, m_FadeAltNoPage);
            HE_CORE_INFO("   分类不变量自检（步骤 29，应恒为 0）: 违例 {}（近场越界 {} / 远场未到上界 {} / 该分支 SDF 却命中 {}）",
                         m_FadeInvariantViolations, m_FadeInvNearViol, m_FadeInvFarViol, m_FadeInvBranchViol);
            HE_CORE_INFO("   按距离分桶的平均材质亮度（桶宽 5 单位，覆盖 20..100；阈值 50 = 第 6 桶）: "
                         "20-25 {} | 25-30 {} | 30-35 {} | 35-40 {} | 40-45 {} | 45-50 {} | 50-55 {} | 55-60 {} | "
                         "60-65 {} | 65-70 {} | 70-75 {} | 75-80 {} | 80-85 {} | 85-90 {} | 90-95 {} | 95-100 {}",
                         BinMean(0), BinMean(1), BinMean(2), BinMean(3), BinMean(4), BinMean(5),
                         BinMean(6), BinMean(7), BinMean(8), BinMean(9), BinMean(10), BinMean(11),
                         BinMean(12), BinMean(13), BinMean(14), BinMean(15));
        }
    }

    // ── ② 步骤 27 的读数：合并分类 + "按距离分桶的最终材质亮度"曲线 ──
    if (m_FarMergeStatsMapped) {
        u32 ms[8] = {0};
        std::memcpy(ms, m_FarMergeStatsMapped, sizeof(ms));
        m_FadeSdfOnly = ms[0]; m_FadeRtOnly = ms[1]; m_FadeBlend = ms[2];
        m_FadeInvNearViol = ms[3]; m_FadeInvFarViol = ms[4]; m_FadeInvBranchViol = ms[5];
        m_FadeInvariantViolations = ms[3] + ms[4] + ms[5];
    }
    if (m_DistBinMapped) {
        u32 bins[32] = {0};
        std::memcpy(bins, m_DistBinMapped, sizeof(bins));
        for (u32 i = 0; i < 16u; ++i) {
            m_DistBinCount[i] = bins[i * 2u];
            m_DistBinLum[i]   = bins[i * 2u + 1u];
        }
    }

    // ── ③ 本帧：先光追，再与 SDF 结果逐光线比较 ──
    ComputeBarrier(cmd);   // 等"追踪 pass 写 SDF 光线结果"落地

    LumenFarFieldPass::FrameParams fp;
    fp.probeBuffer   = m_ProbeBuf.get();
    fp.probeCount    = m_ProbeCount;
    fp.traceRep      = m_TraceConfig.traceRep;
    // 种子必须与 SDF 追踪那一帧一致（追踪 pass 用自己的帧计数当种子再自增）
    fp.seed          = m_TraceFrame - 1u;
    fp.sampleMode    = kSampleModeUniformHemisphere;
    fp.tMin          = 0.0f;
    fp.tMax          = m_FarFieldTMax;
    fp.farThreshold  = m_FarFieldThreshold;
    fp.materialTex   = m_RTMaterialTex;
    fp.triangleNorms = m_RTTriangleNormals;
    fp.lightBuffer   = m_RTLightBuffer;
    fp.lightCount    = m_RTLightCount;
    const u32 rays = m_FarField->Trace(cmd, m_TLAS, fp);
    if (rays == 0u) return;
    m_FarFieldRays = rays;
    ++m_FarFieldFrame;

    // 【必须在 RT 缓冲被下一帧覆盖之前比较】两个缓冲本帧都写完了 ⇒ 紧接着比
    ComputeBarrier(cmd);
    if (m_FarCmpSet == rhi::kInvalidSet) return;
    // RT 结果缓冲可能在扩容时被重建 ⇒ 这里每次重绑（一帧一次，代价可忽略）
    m_Device->UpdateDescriptorSet(m_FarCmpSet, 1, rhi::DescriptorType::StorageBuffer,
                                  m_FarField->GetResultBuffer());
    struct { u32 rays, thr, nearBand, pad; } pc{};
    pc.rays     = rays;
    pc.thr      = (u32)m_FarFieldThreshold;
    pc.nearBand = (u32)m_FarFieldNearBand;   // 近带/远带的分界（近带里 SDF 是准的，可作对照）
    if (m_FarCmpStatsMapped) { u32 zero[24] = {0}; std::memcpy(m_FarCmpStatsMapped, zero, sizeof(zero)); }
    if (m_FarCmpHistMapped)  { u32 zero[16] = {0}; std::memcpy(m_FarCmpHistMapped, zero, sizeof(zero)); }
    cmd->SetPipeline(m_FarCmpPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_FarCmpSet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch((rays + 63u) / 64u, 1, 1);

    // ── ③ 合并：近场留 SDF、远场用光追、重叠带两侧材质混合（步骤 26/27）──
    if (m_FarMergePSO && m_FarField->GetResultBuffer()) {
        ComputeBarrier(cmd);
        // 光追结果缓冲可能在扩容时被重建 ⇒ 每次重绑
        m_Device->UpdateDescriptorSet(m_FarMergeSet, 1, rhi::DescriptorType::StorageBuffer,
                                      m_FarField->GetResultBuffer());
        // 【绑定 3 必须每帧绑】它指向着色 pass 建的 fade 缓冲，而远场 pass 的 set 是在**更早的一帧**
        // 创建的 —— 那一帧着色还没跑过，`if (m_RayFadeBuf)` 直接跳过 ⇒ 描述符一直是空的，
        // 合并 pass 的写入落进"未绑定描述符"（表现为"合并统计说 445 条在带内，着色侧一条也读不到"）。
        if (m_RayFadeBuf)
            m_Device->UpdateDescriptorSet(m_FarMergeSet, 3, rhi::DescriptorType::StorageBuffer, m_RayFadeBuf.get());
        // 重叠带 [t0, t1]：t1 == t0 时退化成硬切换（步骤 27 的 A/B 对照就靠它）
        const float halfBand = m_FarFieldThreshold * std::max(0.0f, m_FarFieldOverlap);
        const u32 t0 = (u32)std::max(0.0f, m_FarFieldThreshold - halfBand);
        const u32 t1 = (u32)(m_FarFieldThreshold + halfBand);
        if (m_FarMergeStatsMapped) { u32 zero[8] = {0}; std::memcpy(m_FarMergeStatsMapped, zero, sizeof(zero)); }
        if (m_DistBinMapped)       { u32 zero[32] = {0}; std::memcpy(m_DistBinMapped, zero, sizeof(zero)); }
        struct { u32 rays, t0, t1, pad0; } mpc2{};
        mpc2.rays = rays;
        mpc2.t0   = t0;
        mpc2.t1   = t1;
        cmd->SetPipeline(m_FarMergePSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_FarMergeSet);
        cmd->SetPushConstants(0, sizeof(mpc2), &mpc2);
        cmd->Dispatch((rays + 63u) / 64u, 1, 1);
    }
}
} // namespace he::render
