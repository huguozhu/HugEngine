// ============================================================
// LumenScene_ProbeFilter.cpp — 步骤 35：Screen Probe 的空间 + 时域滤波
//
// 【它在 L6 里的位置】L6 = 时间混合 + 空间滤波 + 异步 Compute。本文件是第一件事：
// 把步骤 23 投出来的探针 SH（每探针只有 8~16 条均匀半球光线，方差很大）在**空间**上按
// 3×3 单元做 YCoCg AABB 截断平均、在**时域**上做重投影 EMA，结果写进一份**独立的**
// `m_ProbeFilteredBuf`：下游（逐像素辐照度、DDGI 的 Screen-Probe 输入）只读这一份。
//
// 【为什么写独立缓冲而不是就地改】3×3 邻域是**互相重叠**的：就地读写会让"邻居读到本帧已写过的值"，
// 结果取决于线程调度顺序（§附二十四 那类竞态的老毛病）。独立输出 + 乒乓历史把读写彻底分开。
//
// 【为什么历史也要乒乓】时域级要在同一个 dispatch 里"读上一帧的探针 j、写本帧的探针 i"，
// i 与 j 一般是不同的下标，但**同一块缓冲既读又写**仍然构成竞态（线程 A 读 j 时线程 B 可能正在写 j）。
// 所以历史是两份（读一份、写另一份），每帧末尾交换。
//
// 【为什么历史走统一池】步骤 34 已经把所有 GI 源的降噪历史收敛到 `DenoiseHistoryPool`；
// 探针时域历史是同一件事（"上一帧的估计量"），只是载体是缓冲而不是纹理。自己再建一套就等于
// 把 11.3 刚消灭的"各写一套"重新引入，而且显存账又会散开（`LogSummary()` 一次报全）。
//
// 【三种模式（环境变量 HE_LUMEN_PROBE_FILTER）】
//   off     = 0 直通（下游读到的值与滤波前逐位相同 ⇒ 等价于"没有这一步"）
//   spatial = 1 仅空间
//   full    = 2 空间 + 时域（默认）
// 三种模式共用同一个可执行文件与同一份配置 ⇒ 读数差就是"这一步买到了什么"。
// ============================================================

#include "Lumen/LumenScene.h"

#include "Core/Log.h"
#include "Lumen/ScreenProbe.slang"
#include "ScreenProbe_Filter.comp.spv.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace he::render {

namespace {

/// 滤波参数（push constant，96 B：64 + 16 + 16，与着色器里的 cbuffer 逐字段一致）
struct ProbeFilterPC {
    float4x4 prevViewProj;
    uint4    dims;      // x = 探针数, y = cellsX, z = cellsY, w = 模式
    float4   params;    // x = γ, y = α, z = 历史可用, w = 重投影相对容差
};

/// 历史条目（80 B）：与着色器里的 `ProbeHistory` 同布局
struct ProbeFilterHistory {
    float4 position;
    float4 normal;
    float4 shR, shG, shB;
};

} // namespace

const char* LumenScene::GetProbeFilterNote() const {
    // 日志里要能看到"这条信号用的到底是什么核"，而不是两个对它没有意义的双边参数
    return "核 3x3 单元 YCoCg AABB + 时域重投影 EMA";
}

void LumenScene::CreateProbeFilterGPUObjects() {
    if (m_ProbeFilterPSO) return;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 当帧探针（读）
        {1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 过滤结果（写）
        {2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 当帧单元 → 探针（读）
        {3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 上一帧单元 → 探针（读）
        {4, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 当帧映射写出（写）
        {5, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 上一帧探针镜像（读）
        {6, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 本帧探针镜像（写）
        {7, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 统计（写）
    };
    m_ProbeFilterLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_ProbeFilterSet    = m_Device->AllocateDescriptorSet(m_ProbeFilterLayout);

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskCompute;
    pcr.offset    = 0;
    pcr.size      = sizeof(ProbeFilterPC);

    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_ScreenProbe_Filter_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc pso;
    pso.bindPoint            = rhi::PipelineBindPoint::Compute;
    pso.computeShader        = &cs;
    pso.descriptorSetLayouts = {m_ProbeFilterLayout};
    pso.pushConstantRanges   = {pcr};
    pso.debugName            = "Lumen_ScreenProbe_Filter";
    m_ProbeFilterPSO = m_Device->CreatePipelineState(pso);
    if (!m_ProbeFilterPSO) HE_CORE_ERROR("LumenScene: 探针滤波 PSO 创建失败");
}

void LumenScene::RunScreenProbeFilter(rhi::IRHICommandList* cmd, const float4x4& viewProj) {
    if (!m_Device || !cmd || !m_ProbeBuf || !m_CellProbeBuf) return;
    if (!m_ProbeCount) return;

    const u32 cellsX = (m_Width + 15u) / 16u;
    const u32 cellsY = (m_Height + 15u) / 16u;

    if (!m_ProbeFilterBound) {
        CreateProbeFilterGPUObjects();
        if (!m_ProbeFilterPSO) return;

        // 模式与两个核参数：只在建资源时读一次环境变量（运行中改它没有意义）
        {
            const char* v = std::getenv("HE_LUMEN_PROBE_FILTER");
            m_ProbeFilterMode = 2u;
            if (v && std::strcmp(v, "off") == 0)          m_ProbeFilterMode = 0u;
            else if (v && std::strcmp(v, "spatial") == 0) m_ProbeFilterMode = 1u;
            else if (v && std::strcmp(v, "keepmean") == 0) m_ProbeFilterMode = 3u;
            if (const char* g = std::getenv("HE_LUMEN_PROBE_FILTER_GAMMA")) m_ProbeFilterGamma = (float)atof(g);
            if (const char* a = std::getenv("HE_LUMEN_PROBE_FILTER_ALPHA")) m_ProbeFilterAlpha = (float)atof(a);
        }

        rhi::BufferDesc fb;
        fb.size = (usize)kMaxScreenProbes * sizeof(ScreenProbe);
        fb.usage = rhi::BufferUsage::Storage;
        m_ProbeFilteredBuf = m_Device->CreateBuffer(fb);

        rhi::BufferDesc hb;
        hb.size = (usize)kMaxScreenProbes * sizeof(ProbeFilterHistory);
        hb.usage = rhi::BufferUsage::Storage;
        const u64 cellBytes = (u64)cellsX * cellsY * sizeof(u32);
        for (u32 k = 0; k < 2u; ++k) {
            // 历史走统一池：名字稳定，池里"同名同大小只建一次" ⇒ resize 后重建、跨源不重名
            if (m_HistoryPool) {
                char name[64];
                std::snprintf(name, sizeof(name), "Lumen_ProbeHistory%u", k);
                m_ProbeHistFromPool[k] = m_HistoryPool->AcquireBuffer(name, hb.size);
                std::snprintf(name, sizeof(name), "Lumen_CellProbeHistory%u", k);
                m_CellHistFromPool[k]  = m_HistoryPool->AcquireBuffer(name, cellBytes);
            }
            if (!m_ProbeHistFromPool[k]) m_ProbeFilterHistBuf[k] = m_Device->CreateBuffer(hb);
            if (!m_CellHistFromPool[k]) {
                rhi::BufferDesc cb;
                cb.size  = (usize)cellBytes;
                cb.usage = rhi::BufferUsage::Storage;
                m_CellProbeHistBuf[k] = m_Device->CreateBuffer(cb);
            }
        }

        rhi::BufferDesc sb;
        sb.size = sizeof(u32) * 16u; sb.usage = rhi::BufferUsage::Storage; sb.cpuAccess = true;
        m_ProbeFilterStatsBuf = m_Device->CreateBuffer(sb);
        m_ProbeFilterStatsMapped = m_ProbeFilterStatsBuf->Map();

        m_ProbeFilterBound = true;
        return;   // 本帧只建资源：新建资源当帧使用会读到未初始化的内容（§附二十）
    }

    // ── ① 先读上一帧的统计（"先读后清"：统计由 GPU 原子累加，先清就只会读到 0）──
    if (m_ProbeFilterFrame >= 1 && m_ProbeFilterStatsMapped) {
        u32 st[16] = {0};
        std::memcpy(st, m_ProbeFilterStatsMapped, sizeof(st));
        const u32 n = st[0];
        if (n) {
            const double invN = 1.0 / (double)n;
            const double meanIn  = (double)st[1] / 4096.0 * invN;
            const double meanOut = (double)st[3] / 4096.0 * invN;
            const double varIn   = (double)st[2] / 4096.0 * invN - meanIn * meanIn;
            const double varOut  = (double)st[4] / 4096.0 * invN - meanOut * meanOut;
            m_ProbeL0MeanIn  = (float)meanIn;
            m_ProbeL0MeanOut = (float)meanOut;
            m_ProbeNoiseIn   = (meanIn  > 1e-6) ? (float)(sqrt(varIn  > 0.0 ? varIn  : 0.0) / meanIn)  : 0.0f;
            m_ProbeNoiseOut  = (meanOut > 1e-6) ? (float)(sqrt(varOut > 0.0 ? varOut : 0.0) / meanOut) : 0.0f;
            m_ProbeHistoryUsed = st[6];
            m_ProbeFrameChange = (st[6] && meanOut > 1e-6)
                ? (float)((double)st[5] / 4096.0 / (double)st[6] / meanOut) : 0.0f;
        }
        if ((m_ProbeFilterFrame % 40u) == 0u) {
            HE_CORE_INFO("LumenScene 探针滤波（步骤 35）: 模式 {}（0 直通 / 1 仅空间 / 2 空间+时域）；"
                         "探针 {}；l0 亮度 mean {:.5f} → {:.5f}；噪声 std/mean {:.4f} → {:.4f}（{:+.1f}%）；"
                         "采纳历史 {}（帧间变化 {:.2f}%）；邻域样本 {}、AABB 截断 {}、候选未过门限 {}、直通 {}",
                         m_ProbeFilterMode, n,
                         (double)m_ProbeL0MeanIn, (double)m_ProbeL0MeanOut,
                         (double)m_ProbeNoiseIn, (double)m_ProbeNoiseOut,
                         m_ProbeNoiseIn > 1e-6 ? 100.0 * ((double)m_ProbeNoiseOut / (double)m_ProbeNoiseIn - 1.0) : 0.0,
                         st[6], 100.0 * (double)m_ProbeFrameChange,
                         st[7], st[9], st[8], st[10]);
        }
    }

    // ── ② 清零统计并派发 ──
    if (m_ProbeFilterStatsMapped) {
        u32 zero[16] = {0};
        std::memcpy(m_ProbeFilterStatsMapped, zero, sizeof(zero));
    }

    // 乒乓：读 r、写 w；dispatch 结束后 r = w
    const u32 r = m_ProbeFilterHistIdx;
    const u32 w = 1u - r;
    rhi::IRHIBuffer* histProbe = m_ProbeHistFromPool[r] ? m_ProbeHistFromPool[r] : m_ProbeFilterHistBuf[r].get();
    rhi::IRHIBuffer* histCell  = m_CellHistFromPool[r]  ? m_CellHistFromPool[r]  : m_CellProbeHistBuf[r].get();
    rhi::IRHIBuffer* outProbe  = m_ProbeHistFromPool[w] ? m_ProbeHistFromPool[w] : m_ProbeFilterHistBuf[w].get();
    rhi::IRHIBuffer* outCell   = m_CellHistFromPool[w]  ? m_CellHistFromPool[w]  : m_CellProbeHistBuf[w].get();
    if (!histProbe || !histCell || !outProbe || !outCell || !m_ProbeFilteredBuf || !m_ProbeFilterStatsBuf) return;

    // 描述符必须**每帧重绑**：乒乓使绑定 3/4/5/6 的目标每帧都换（步骤 27 的 fade 缓冲教训）
    m_Device->UpdateDescriptorSet(m_ProbeFilterSet, 0, rhi::DescriptorType::StorageBuffer, m_ProbeBuf.get());
    m_Device->UpdateDescriptorSet(m_ProbeFilterSet, 1, rhi::DescriptorType::StorageBuffer, m_ProbeFilteredBuf.get());
    m_Device->UpdateDescriptorSet(m_ProbeFilterSet, 2, rhi::DescriptorType::StorageBuffer, m_CellProbeBuf.get());
    m_Device->UpdateDescriptorSet(m_ProbeFilterSet, 3, rhi::DescriptorType::StorageBuffer, histCell);
    m_Device->UpdateDescriptorSet(m_ProbeFilterSet, 4, rhi::DescriptorType::StorageBuffer, outCell);
    m_Device->UpdateDescriptorSet(m_ProbeFilterSet, 5, rhi::DescriptorType::StorageBuffer, histProbe);
    m_Device->UpdateDescriptorSet(m_ProbeFilterSet, 6, rhi::DescriptorType::StorageBuffer, outProbe);
    m_Device->UpdateDescriptorSet(m_ProbeFilterSet, 7, rhi::DescriptorType::StorageBuffer, m_ProbeFilterStatsBuf.get());

    ProbeFilterPC pc{};
    pc.prevViewProj = m_ProbePrevViewProj;
    pc.dims    = uint4(m_ProbeCount, cellsX, cellsY, m_ProbeFilterMode);
    pc.params  = float4(m_ProbeFilterGamma, m_ProbeFilterAlpha,
                        (m_ProbeFilterMode >= 2u && m_ProbeFilterHistoryReady) ? 1.0f : 0.0f,
                        m_ProbeFilterDistTol);

    ComputeBarrier(cmd);   // 等"SH 投影写回探针系数"落地（Lumen 内部 pass 之间必须显式屏障）
    cmd->SetPipeline(m_ProbeFilterPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_ProbeFilterSet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    // 线程数取 max(探针数, 单元数)：每个单元一个线程负责"把当帧映射抄进历史缓冲"，
    // 一个合并探针覆盖 4 个单元，按探针抄会漏掉 3 个（见着色器里的说明）。
    const u32 threads = m_ProbeCount > cellsX * cellsY ? m_ProbeCount : cellsX * cellsY;
    cmd->Dispatch((threads + 63u) / 64u, 1, 1);

    // ── ③ 交换历史：本帧写出的那一份成为下一帧的历史 ──
    m_ProbeFilterHistIdx = w;
    m_ProbePrevViewProj  = viewProj;      // 本帧的 viewProj 是下一帧的"上一帧"
    m_ProbeFilterHistoryReady = true;     // 从下一帧起历史可用（本帧写满了 [0, probeCount)）
    ++m_ProbeFilterFrame;
}

} // namespace he::render
