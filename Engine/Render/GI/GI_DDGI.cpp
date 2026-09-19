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
static constexpr u32 kDDGIBindScreenProbes = 11;   // 步骤 31：Lumen 的 Screen Probe（StructuredBuffer<ScreenProbe>）
static constexpr u32 kDDGIBindScreenCells  = 12;   // 步骤 31：16×16 单元 → 探针下标
static constexpr u32 kDDGIBindStats        = 13;   // 步骤 31：收敛读数（整数定点）

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
        // 步骤 31：Screen Probe 输入（Lumen 的探针缓冲 + 单元映射）。两者都由 Lumen 持有，
        // 本 pass 只借句柄；未启用时 u_ScreenProbeFlags.x=0，着色器不采样它们。
        {kDDGIBindScreenProbes, rhi::DescriptorType::StorageBuffer,       1, rhi::kStageMaskCompute},
        {kDDGIBindScreenCells,  rhi::DescriptorType::StorageBuffer,       1, rhi::kStageMaskCompute},
        {kDDGIBindStats,        rhi::DescriptorType::StorageBuffer,       1, rhi::kStageMaskCompute},
    };
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    // 步骤 31：收敛读数缓冲（CPU 可映射；每帧由 compute 累加，CPU 侧"先读后清"）
    {
        rhi::BufferDesc sbd;
        sbd.size = sizeof(u32) * 8u; sbd.usage = rhi::BufferUsage::Storage; sbd.cpuAccess = true;
        m_StatsBuffer = device->CreateBuffer(sbd);
        m_StatsMapped = m_StatsBuffer ? m_StatsBuffer->Map() : nullptr;
        if (m_StatsBuffer) {
            device->UpdateDescriptorSet(m_Set, kDDGIBindStats, rhi::DescriptorType::StorageBuffer,
                                        m_StatsBuffer.get());
        }
    }

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

    // 步骤 31：先读**上一帧**的收敛统计（同"先读后清"；本帧紧接着清零再派发）
    if (m_StatsFrame >= 1u && m_StatsMapped) {
        u32 st[8] = {0};
        memcpy(st, m_StatsMapped, sizeof(st));
        m_ProbesScreenSampled = st[0];
        m_ProbesCarried       = st[2];
        const u32 updated = st[1];
        m_ProbeRelChange = updated ? (float)((double)st[3] / 65536.0 / (double)updated) : 0.0f;
        // 前 40 帧逐帧打（"30 帧内收敛"的读数），之后每 60 帧打一次
        if (m_StatsFrame <= 40u || (m_StatsFrame % 60u) == 0u) {
            HE_CORE_INFO("DDGI 收敛（步骤 31）: 帧 {} —— Screen Probe 采纳 {} / 继承历史 {} / 更新 {}；"
                         "探针 SH 相对变化均值 {:.5f}；抓取分步: 进入 {} / 屏内 {} / 单元有探针 {} / 过深度门限 {}",
                         m_StatsFrame, m_ProbesScreenSampled, m_ProbesCarried, updated,
                         (double)m_ProbeRelChange, st[4], st[5], st[6], st[7]);
        }
    }
    ++m_StatsFrame;
    if (m_StatsMapped) { u32 zero[8] = {0}; memcpy(m_StatsMapped, zero, sizeof(zero)); }

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
    // 步骤 31：Screen Probe 输入（把 DDGI 探针投到屏幕 → 16×16 单元 → 探针）。
    // `kScreenProbeSymmetricScale` 把"单侧（半球）估计"外推成"整球估计"（见 DDGI.comp 的说明）。
    uniforms.screenProbeDims  = float4(float(m_ScreenCellsX), float(m_ScreenCellsY),
                                       float(m_ScreenW), float(m_ScreenH));
    uniforms.screenProbeFlags = float4(m_ScreenProbeEnabled ? 1.0f : 0.0f,
                                       m_ScreenMaxMatchDistance,
                                       kScreenProbeSymmetricScale, 0.0f);
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

// ============================================================
// 步骤 31（L5）：把"当前估计"的来源从自追踪改为 Screen Probe 的结果
//
// 【为什么可行】DDGI 探针与 Lumen 的 Screen Probe 现在都是"二阶 4 系数、RGB 独立"的辐射度 SH
//（步骤 30 刚把两边收敛到同一个表示），所以"换输入"只是换一个求值来源，投影流程一行不用动。
//
// 【一帧延迟】帧图里 DDGI 段在 Lumen 段**之前**注册，所以本 pass 读到的是**上一帧**的探针缓冲 ——
// 这既避开了循环依赖，也是标准的"上一帧屏幕信息"用法（与本仓库其它 GI 源一致）。
// ============================================================
void GI_DDGI::SetScreenProbeInput(const ScreenProbeInput& in) {
    m_ScreenProbeEnabled = (in.probeBuffer != nullptr && in.cellProbeMap != nullptr &&
                            in.cellsX > 0 && in.cellsY > 0);
    m_ScreenProbeBuffer   = in.probeBuffer;
    m_ScreenCellProbeMap  = in.cellProbeMap;
    m_ScreenViewProj      = in.viewProj;
    m_ScreenCellsX        = in.cellsX;
    m_ScreenCellsY        = in.cellsY;
    m_ScreenW             = in.width;
    m_ScreenH             = in.height;
    m_ScreenMaxMatchDistance = in.maxMatchDistance;
    if (m_Device && m_ScreenProbeEnabled) {
        m_Device->UpdateDescriptorSet(m_Set, kDDGIBindScreenProbes, rhi::DescriptorType::StorageBuffer,
                                      m_ScreenProbeBuffer);
        m_Device->UpdateDescriptorSet(m_Set, kDDGIBindScreenCells, rhi::DescriptorType::StorageBuffer,
                                      m_ScreenCellProbeMap);
    }
}

void GI_DDGI::ClearScreenProbeInput() {
    // 只清成员（是否启用由它们推导）：描述符留着不重绑 —— 着色器在 screenProbeFlags.x=0 时
    // 根本不采样这两个绑定（与 ClearRSM 同一约定）。
    m_ScreenProbeEnabled  = false;
    m_ScreenProbeBuffer   = nullptr;
    m_ScreenCellProbeMap  = nullptr;
    m_ScreenCellsX = m_ScreenCellsY = m_ScreenW = m_ScreenH = 0;
    m_ScreenMaxMatchDistance = 0.0f;
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

    // 步骤 32：把**原点**也打出来 —— 网格对齐伪影的分析（`build/verify/grid_artifact.py`）需要它，
    // 而"拟合参数复核"也需要肉眼能对：最后一颗探针 = 原点 + (count-1)×格距 应当覆盖到包围盒的 max。
    HE_CORE_INFO("DDGI 网格已按场景拟合：场景 {}x{}x{} -> 探针 {}x{}x{} 格距 {:.2f}（共 {} 个）；"
                 "原点 ({:.2f}, {:.2f}, {:.2f})，末探针 ({:.2f}, {:.2f}, {:.2f}) vs 包围盒 max ({:.2f}, {:.2f}, {:.2f})",
                 mx.x - mn.x, mx.y - mn.y, mx.z - mn.z,
                 gridX, gridY, gridZ, cellSize, gridX * gridY * gridZ,
                 gridOrigin.x, gridOrigin.y, gridOrigin.z,
                 gridOrigin.x + float(gridX - 1) * cellSize,
                 gridOrigin.y + float(gridY - 1) * cellSize,
                 gridOrigin.z + float(gridZ - 1) * cellSize,
                 mx.x, mx.y, mx.z);
}

void GI_DDGI::SetIBL(rhi::IRHITexture* irradiance, rhi::IRHISampler* sampler) {
    m_IBLIrradiance = irradiance;
    if (m_Device && irradiance && sampler) {
        m_Device->UpdateDescriptorSet(m_Set, kDDGIBindIBL, rhi::DescriptorType::CombinedImageSampler,
            irradiance, sampler);
    }
}

} // namespace he::render
