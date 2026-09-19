// GI/GI_DDGI.cpp — 动态探针 GI（Compute Shader 更新 + 时间混合 + 探针网格）
#include "GI/GI_DDGI.h"
#include "GI/GIProbeGrid.h"   // 探针网格拟合规则（纯几何、可单测）
#include "Core/Log.h"
#include "Subsystem/RenderSubsystem.h"
#include "DDGI.comp.spv.h"
#include <cstring>
#include <cstdio>

namespace he::render {

// DDGI 探针更新描述符集绑定号（与 DDGI.comp.slang 一致）
static constexpr u32 kDDGIBindAlbedo      = 0;   // GBuffer 反照率
static constexpr u32 kDDGIBindNormal      = 1;   // GBuffer 法线
static constexpr u32 kDDGIBindDepth       = 2;   // GBuffer 深度
static constexpr u32 kDDGIBindProbes      = 3;   // 探针输出（RW）
static constexpr u32 kDDGIBindGridParams  = 4;   // 探针网格参数 UBO
static constexpr u32 kDDGIBindHistory     = 5;   // 上一帧探针历史
static constexpr u32 kDDGIBindPrevHDR     = 6;   // 前帧 HDR（屏幕回退）
static constexpr u32 kDDGIBindRSMPosition = 7;   // RSM 位置图
static constexpr u32 kDDGIBindRSMRadiance     = 8;   // RSM 辐射度图（任务 30）
static constexpr u32 kDDGIBindIBL         = 9;   // IBL 辐照度（Cubemap）
static constexpr u32 kDDGIBindTracedRadiance = 10;  // 光追 march 的探针射线辐射度（任务 17）

// 计算调度所需的 Dispatch 组数（每线程处理一个探针，64 线程/组）
static u32 DispatchGroupCount(u32 probeCount) {
    return (probeCount + 63) / 64;
}

bool GI_DDGI::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width = width;
    m_Height = height;
    m_Settings.enabled   = false;
    m_Settings.intensity = 1.0f;
    m_Settings.mode      = GIMode::DDGI;

    u32 probeCount = gridX * gridY * gridZ;
    u64 bufferSize = probeCount * kFloats4PerProbe * sizeof(float4);  // 每探针 4×16 B（步骤 30：原 256 B）

    // ---- 探针数据存储（两帧：当前 + 历史，每帧交换） ----
    rhi::BufferDesc probeDesc;
    probeDesc.size  = bufferSize;
    probeDesc.usage = rhi::BufferUsage::Storage;
    m_ProbeBuffer  = device->CreateBuffer(probeDesc);
    m_ProbeHistory = device->CreateBuffer(probeDesc);
    m_ProbeBufferCount = probeCount;   // 网格拟合改变探针数时按此判断要不要重建

    // ---- 探针网格参数 Uniform Buffer（CPU 可写，每帧更新） ----
    rhi::BufferDesc uniformDesc;
    uniformDesc.size      = sizeof(ProbeGridUniform);
    uniformDesc.usage     = rhi::BufferUsage::Uniform;
    uniformDesc.cpuAccess = true;  // 每帧 Map/Unmap 更新
    m_GridUniform = device->CreateBuffer(uniformDesc);

    // 一次性写入默认网格参数，确保 DDGI 未启用时 Lighting Pass 采样到的 UBO 内容有效
    // （避免 gridSize.w=0 导致 SampleDDGI 中 (worldPos-origin)/cellSize 除零）
    {
        ProbeGridUniform init{};
        init.gridOrigin = float4(gridOrigin, 0.0f);
        init.gridSize   = float4(float(gridX), float(gridY), float(gridZ), cellSize);
        init.cameraPos  = float4(0, 0, 0, 0);
        init.params     = float4(1.0f, 32.0f, 0.0f, 0.0f);
        init.viewProj   = float4x4(1.0f);
        void* mapped = m_GridUniform->Map();
        if (mapped) { memcpy(mapped, &init, sizeof(ProbeGridUniform)); m_GridUniform->Unmap(); }
    }

    // ---- 采样器 ----
    rhi::SamplerDesc psd;
    psd.minFilter  = psd.magFilter = rhi::FilterMode::Nearest;
    psd.addressU   = psd.addressV   = rhi::AddressMode::ClampToEdge;
    m_PointSampler = device->CreateSampler(psd);

    rhi::SamplerDesc lsd;
    lsd.minFilter  = lsd.magFilter = rhi::FilterMode::Linear;
    lsd.addressU   = lsd.addressV   = rhi::AddressMode::ClampToEdge;
    m_LinearSampler = device->CreateSampler(lsd);

    // ---- 前帧 HDR 纹理与下采样 PSO 已抽到共享组件 GIRadianceHistory ----
    // （原先此处创建 m_PrevHDR + m_DownsamplePSO；多个 GI 源共用一份，避免重复下采样）

    // ---- DescriptorSet 布局 ----
    // binding 0-2: GBuffer CombinedImageSampler
    // binding 3:   ProbeBuffer  StorageBuffer（RW, 当前帧输出）
    // binding 4:   GridUniform  UniformBuffer（探针网格参数）
    // binding 5:   HistoryBuffer StorageBuffer（只读, 上一帧历史）
    // binding 6:   PrevHDR CombinedImageSampler（前帧 HDR 辐射度，屏幕回退）
    // binding 7/8: RSM Position/Radiance CombinedImageSampler（B 路径世界辐射度）
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {kDDGIBindAlbedo,      rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},
        {kDDGIBindNormal,      rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},
        {kDDGIBindDepth,       rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},
        {kDDGIBindProbes,      rhi::DescriptorType::StorageBuffer,         1, rhi::kStageMaskCompute},
        {kDDGIBindGridParams,  rhi::DescriptorType::UniformBuffer,         1, rhi::kStageMaskCompute},
        {kDDGIBindHistory,     rhi::DescriptorType::StorageBuffer,         1, rhi::kStageMaskCompute},
        {kDDGIBindPrevHDR,     rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // 前帧 HDR
        {kDDGIBindRSMPosition, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // RSM Position
        {kDDGIBindRSMRadiance,     rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // RSM Radiance（VPL 出射辐射度）
        {kDDGIBindIBL,         rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // IBL Irradiance (Cubemap)
        // 光追 march 的探针射线辐射度（任务 17）：A 路径的输入，未启用时 u_Flags.w=0 不采样
        {kDDGIBindTracedRadiance, rhi::DescriptorType::StorageBuffer,     1, rhi::kStageMaskCompute},
    };
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    // 预绑定前帧 HDR：纹理由共享组件持有，可能因 resize 被重建，故按代次判断是否重绑
    BindRadianceHistory();

    // 预绑定不变的 binding：uniform buffer（每帧只需 Map/Unmap 更新内容）
    device->UpdateDescriptorSet(m_Set, kDDGIBindGridParams, rhi::DescriptorType::UniformBuffer, m_GridUniform.get());

    // ---- Compute PSO ----
    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_DDGI_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc psoDesc;
    psoDesc.bindPoint            = rhi::PipelineBindPoint::Compute;
    psoDesc.computeShader        = &cs;
    psoDesc.descriptorSetLayouts = {m_Layout};
    psoDesc.debugName            = "DDGI";
    m_PSO = device->CreatePipelineState(psoDesc);

    if (!m_PSO) {
        HE_CORE_ERROR("GI_DDGI: Compute PSO creation failed");
        return false;
    }

    m_Ready = true;
    HE_CORE_INFO("GI_DDGI initialized ({}×{}×{} = {} probes, {} KB, temporal={})",
        gridX, gridY, gridZ, probeCount,
        (bufferSize * 2) / 1024, blendAlpha);
    return true;
}

void GI_DDGI::Shutdown() {
    if (m_Device && m_Layout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_Layout);
    }
    m_PSO.reset();
    m_ProbeBuffer.reset();
    m_ProbeHistory.reset();
    m_GridUniform.reset();
    m_PointSampler.reset();
    m_LinearSampler.reset();
    m_Radiance = nullptr;              // 非拥有，仅清引用
    m_RadianceGeneration = 0;
    m_Device = nullptr;
    m_Ready  = false;
    HE_CORE_INFO("GI_DDGI shutdown");
}

void GI_DDGI::OnResize(u32 w, u32 h) {
    m_Width = w;
    m_Height = h;
}

void GI_DDGI::SetGBufferInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo) {
    m_Depth  = depth;
    m_Normal = normal;
    m_Albedo = albedo;

    // 更新描述符集中 GBuffer 绑定（每帧纹理可能变化）
    if (m_Albedo && m_Device) {
        m_Device->UpdateDescriptorSet(m_Set, kDDGIBindAlbedo, rhi::DescriptorType::CombinedImageSampler,
            m_Albedo, m_PointSampler.get());
    }
    if (m_Normal && m_Device) {
        m_Device->UpdateDescriptorSet(m_Set, kDDGIBindNormal, rhi::DescriptorType::CombinedImageSampler,
            m_Normal, m_PointSampler.get());
    }
    if (m_Depth && m_Device) {
        m_Device->UpdateDescriptorSet(m_Set, kDDGIBindDepth, rhi::DescriptorType::CombinedImageSampler,
            m_Depth, m_PointSampler.get());
    }
}

void GI_DDGI::Update(const SubsystemContext& ctx) {
    // 从 SubsystemContext 获取相机位置和投影矩阵（用于探针→屏幕投影）
    if (ctx.camera) {
        m_CameraPos   = ctx.camera->position;
        m_ViewProj    = ctx.camera->GetViewProjMatrix();
        m_CameraReady = true;
    }
    // 共享辐射度纹理可能因 resize 被重建 → 按代次补绑（未变化时是零开销的早退）
    BindRadianceHistory();
}

void GI_DDGI::BindRadianceHistory() {
    if (!m_Device || !m_Radiance || m_Set == rhi::kInvalidSet) return;
    // 纹理未变（代次相同）则无需重绑，避免每帧无谓的 UpdateDescriptorSet
    if (m_RadianceGeneration == m_Radiance->GetGeneration()) return;
    rhi::IRHITexture* tex = m_Radiance->GetTexture();
    if (!tex) return;
    m_Device->UpdateDescriptorSet(m_Set, kDDGIBindPrevHDR, rhi::DescriptorType::CombinedImageSampler,
                                  tex, m_Radiance->GetSampler());
    m_RadianceGeneration = m_Radiance->GetGeneration();
}

void GI_DDGI::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Settings.enabled) return;
    if (!m_Depth || !m_Normal || !m_Albedo) return;

    u32 probeCount = gridX * gridY * gridZ;

    // 网格探针数变了就重建探针缓冲：自动拟合按场景包围盒定网格，而它只能发生在 Initialize
    // 之后（那时拿不到场景）。重建只在包围盒首次稳定时发生一次。
    if (probeCount != m_ProbeBufferCount) {
        rhi::BufferDesc probeDesc;
        probeDesc.size  = (u64)probeCount * kFloats4PerProbe * sizeof(float4);
        probeDesc.usage = rhi::BufferUsage::Storage;
        m_ProbeBuffer  = m_Device->CreateBuffer(probeDesc);
        m_ProbeHistory = m_Device->CreateBuffer(probeDesc);
        m_ProbeBufferCount = probeCount;
        m_HistoryValid = false;   // 新缓冲是未初始化显存：本帧必须按"无历史"更新（见成员注释）
        HE_CORE_INFO("DDGI 探针缓冲已按新网格重建：{} 个探针", probeCount);
    }

    // 球面采样数
    static const u32 kNumSamples = kSamplesPerProbe;

    // 更新 descriptor set：绑定当前输出缓冲和历史缓冲（每帧因 swap 而变化）
    m_Device->UpdateDescriptorSet(m_Set, kDDGIBindProbes, rhi::DescriptorType::StorageBuffer, m_ProbeBuffer.get());
    m_Device->UpdateDescriptorSet(m_Set, kDDGIBindHistory, rhi::DescriptorType::StorageBuffer, m_ProbeHistory.get());
    // 光追 march 的射线辐射度（任务 17）：由帧图在本帧的 DDGI_Trace pass 之后注入；
    // 未注入时保持上一次绑定，但 u_Flags.w=0 ⇒ 着色器根本不采样它（与 §9.2-T 同一约定：
    // "没用到的绑定"不会读，只是不能让"用了却没绑"发生）。
    if (m_TracedRadiance) {
        m_Device->UpdateDescriptorSet(m_Set, kDDGIBindTracedRadiance,
            rhi::DescriptorType::StorageBuffer, m_TracedRadiance);
    }

    // ---- 上传探针网格 Uniform（含时间混合参数） ----
    ProbeGridUniform uniforms;
    uniforms.gridOrigin = float4(gridOrigin, 0.0f);
    uniforms.gridSize   = float4(float(gridX), float(gridY), float(gridZ), cellSize);
    uniforms.cameraPos  = float4(m_CameraPos, 0.0f);
    uniforms.params     = float4(m_Settings.intensity,
                                 float(kNumSamples),
                                 blendAlpha,
                                 m_HistoryValid ? 1.0f : 0.0f);  // w=historyValid
    uniforms.viewProj   = m_ViewProj;
    uniforms.rsmLightViewProj = m_RSMLightViewProj;   // B 路径：RSM 光源 VP
    // x=useRSM；y/z=时间维分摊的步长与相位（任务 12）；w=光追 march 是否可用（任务 17）
    // 每帧只更新 probeIndex % stride == phase 的那一批探针，未轮到的探针原样继承上一次结果
    // （见 DDGI.comp.slang 的早退分支）。
    const u32 stride = std::max(1u, updateStride);
    const u32 phase  = updatePhase % stride;
    // 【A 路径优先】光追 march 可用时走真实可见性；RSM 是给不支持光追的设备留的 B 路径
    const bool tracedReady = (m_TracedRadiance != nullptr && m_TracedSamples == kNumSamples);
    uniforms.flags = float4((m_RSMPositionMap && m_RSMRadianceMap) ? 1.0f : 0.0f,
                            float(stride), float(phase), tracedReady ? 1.0f : 0.0f);
    ++updatePhase;

    void* mapped = m_GridUniform->Map();
    if (mapped) {
        memcpy(mapped, &uniforms, sizeof(ProbeGridUniform));
        m_GridUniform->Unmap();
    }

    // ---- Compute Dispatch ----
    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    // Dispatch 调试 marker（RenderDoc 定位用）
    char label[64];
    snprintf(label, sizeof(label), "DDGI ProbeUpdate (%u probes)", probeCount);
    cmd->SetDrawDebugLabel(label);
    cmd->Dispatch(DispatchGroupCount(probeCount), 1, 1);

    // 交换当前帧与历史帧缓冲（下一帧用本次结果作为历史）
    m_ProbeBuffer.swap(m_ProbeHistory);
    m_HistoryValid = true;

    // 全局 Barrier：确保 Compute Shader 所有 UAV 写入完成后，后续 Fragment Shader 可读取
    cmd->PipelineBarrier(
        rhi::PipelineStage::ComputeShader,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::ShaderResource);
}

void GI_DDGI::SetRSM(rhi::IRHITexture* pos, rhi::IRHITexture* radiance, const float4x4& lightViewProj) {
    m_RSMPositionMap   = pos;
    m_RSMRadianceMap   = radiance;
    m_RSMLightViewProj = lightViewProj;
    if (m_Device && pos && radiance) {
        m_Device->UpdateDescriptorSet(m_Set, kDDGIBindRSMPosition, rhi::DescriptorType::CombinedImageSampler,
            pos, m_LinearSampler.get());
        m_Device->UpdateDescriptorSet(m_Set, kDDGIBindRSMRadiance, rhi::DescriptorType::CombinedImageSampler,
            radiance, m_LinearSampler.get());
    }
}

void GI_DDGI::ClearRSM() {
    // 只清成员（useRSM 由它们推导）；描述符留着不重绑——着色器在 useRSM=0 时
    // 根本不会采样 RSM 纹理（见 DDGI.comp.slang 的 u_Flags.x 分支），重绑反而多一次写。
    m_RSMPositionMap = nullptr;
    m_RSMRadianceMap     = nullptr;
}

void GI_DDGI::FitGridToBounds(const float3& mn, const float3& mx) {
    // 拟合规则在 GI/GIProbeGrid.h（纯几何、有单元测试）；这里只负责应用与记账
    const auto fit = FitProbeGridToBounds(mn, mx, fitCellsMax);
    if (!fit) return;   // 退化包围盒（无几何 / NaN）：保持原参数不动
    gridX = fit->countX;
    gridY = fit->countY;
    gridZ = fit->countZ;
    cellSize   = fit->cellSize;
    gridOrigin = fit->origin;

    HE_CORE_INFO("DDGI 网格已按场景拟合：场景 {}x{}x{} -> 探针 {}x{}x{} 格距 {:.2f}（共 {} 个）",
                 mx.x - mn.x, mx.y - mn.y, mx.z - mn.z,
                 gridX, gridY, gridZ, cellSize, gridX * gridY * gridZ);
}

void GI_DDGI::SetIBL(rhi::IRHITexture* irradiance, rhi::IRHISampler* sampler) {
    m_IBLIrradiance = irradiance;
    if (m_Device && irradiance && sampler) {
        m_Device->UpdateDescriptorSet(m_Set, kDDGIBindIBL, rhi::DescriptorType::CombinedImageSampler,
            irradiance, sampler);
    }
}

} // namespace he::render
