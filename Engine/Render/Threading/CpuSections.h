#pragma once

#include "Core/Types.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

// ============================================================
// CpuSections —— 主线程 CPU 分段计时（诊断）
//
// 【为什么要它】`docs/计划实现功能/多线程渲染优化方案` §4 的判据要求"逐项（剔除 / 快照构建 /
// 上传 / MTCR / 录制）前后对比"，而仓库既有的 `HE_CPU_PASSES=1` 只覆盖 RenderGraph 的逐 pass ——
// 03.Sponza-Forward 走的是**非 RG 路径**，那些阶段此前没有任何读数。
//
// 【口径】沿用同一个开关（默认关 ⇒ 只判一个 `static bool`，零可观测开销）。
//   累加只在**主线程**发生：并行段由调用方包在 `ParallelInvoke` 外层计总时长，
//   因此这里不需要原子、也不引入额外同步。
//   每 `kReportEvery` 帧打印一次 avg/max 并清零（avg 看占比，max 看尖刺）。
// ============================================================

namespace he::render {

enum class CpuSection : u32 {
    SnapshotBuild = 0,   // 快照构建（`SceneSnapshotBuilder::BuildObjects`）
    Cull,                // 并行视锥剔除（`SceneRenderer::Prepare` Step 1）
    Upload,              // Map + 对象数据写回（`SceneRenderer::Prepare` Step 2）
    Mtcr,                // MTCR 并行录制 + 次级命令缓冲执行（`ForwardPipeline::RenderScene`）
    RecordTotal,         // 整帧 CPU 录制总时长（`ForwardPipeline::Render`）
    Count
};

class CpuSections {
public:
    static constexpr u32 kSectionCount = static_cast<u32>(CpuSection::Count);

    /// 开关：`HE_CPU_PASSES=1`（与 RenderGraph / Nanite / Lumen 的分步读数同一个开关）
    static bool Enabled() {
        static const bool s_Enabled = (std::getenv("HE_CPU_PASSES") != nullptr);
        return s_Enabled;
    }

    static void Add(CpuSection s, double ms) {
        if (!Enabled()) return;
        Accum& a = Acc();
        const u32 i = static_cast<u32>(s);
        a.sum[i] += ms;
        if (ms > a.max[i]) a.max[i] = ms;
    }

    /// 帧边界调用一次：攒够 `kReportEvery` 帧就打一行 avg/max 并清零
    static void Frame() {
        if (!Enabled()) return;
        Accum& a = Acc();
        if (++a.frames < kReportEvery) return;

        char line[512];
        int n = std::snprintf(line, sizeof(line), "[CPU] 前 %u 帧 CPU 分解（avg / max，ms）:",
                              a.frames);
        static const char* kNames[kSectionCount] = {
            "BuildObjects", "Cull", "Upload", "MTCR", "RecordTotal"
        };
        for (u32 i = 0; i < kSectionCount && n > 0 && n < static_cast<int>(sizeof(line)); ++i) {
            const int w = std::snprintf(line + n, sizeof(line) - static_cast<usize>(n),
                                        " %s=%.3f/%.3f", kNames[i],
                                        a.sum[i] / static_cast<double>(a.frames), a.max[i]);
            if (w > 0) n += w;
        }
        std::fprintf(stdout, "%s\n", line);
        std::fflush(stdout);

        for (u32 i = 0; i < kSectionCount; ++i) { a.sum[i] = 0.0; a.max[i] = 0.0; }
        a.frames = 0;
    }

private:
    static constexpr u32 kReportEvery = 120;

    struct Accum {
        double sum[kSectionCount] = {};
        double max[kSectionCount] = {};
        u32    frames = 0;
    };
    static Accum& Acc() {
        static Accum s_Acc;
        return s_Acc;
    }
};

/// RAII：测一段（离开作用域时累加）。开关关闭时不取时间戳（零开销）。
class CpuScope {
public:
    explicit CpuScope(CpuSection section)
        : m_Section(section), m_Enabled(CpuSections::Enabled()) {
        if (m_Enabled) m_T0 = Clock::now();
    }
    ~CpuScope() {
        if (!m_Enabled) return;
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - m_T0).count();
        CpuSections::Add(m_Section, ms);
    }
    CpuScope(const CpuScope&) = delete;
    CpuScope& operator=(const CpuScope&) = delete;

private:
    using Clock = std::chrono::steady_clock;
    CpuSection m_Section;
    bool       m_Enabled;
    Clock::time_point m_T0{};
};

} // namespace he::render
