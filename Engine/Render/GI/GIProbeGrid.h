#pragma once

// ============================================================
// GI/GIProbeGrid.h — DDGI 探针网格的**纯几何**拟合（RHI-free）
//
// 为什么单独一个头：探针网格参数（探针数 / 格距 / 原点）正是 §9.2-K 的判据所在，
// 而它本身只是一段纯算术。抽到这里就能被单元测试直接覆盖——不必为了验证
// 「网格到底罩没罩住场景」去跑一次 GPU 采样。GI_DDGI 只负责应用结果。
//
// 约束：只依赖 Core/Types.h 与 Math/Math.h，不得引入任何 RHI 类型。
// ============================================================

#include "Core/Types.h"
#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace he::render {

/// DDGI 探针网格参数（纯数据）
struct GIProbeGridFit {
    u32    countX = 8, countY = 4, countZ = 8;   // 各轴探针数
    float  cellSize = 3.0f;                      // 三轴共用格距（世界单位）
    float3 origin = float3(0.0f);                // 网格原点 = 最小角探针的世界位置

    /// 探针总数（= 每帧 dispatch 的线程数）
    u32 ProbeCount() const { return countX * countY * countZ; }

    /// 覆盖范围是否真的罩住给定包围盒（逐轴比较，含边界）
    bool Covers(const float3& mn, const float3& mx) const {
        const float3 last = origin + float3(float(countX - 1u), float(countY - 1u), float(countZ - 1u)) * cellSize;
        return origin.x <= mn.x && origin.y <= mn.y && origin.z <= mn.z &&
               last.x >= mx.x && last.y >= mx.y && last.z >= mx.z;
    }
};

/// 用场景包围盒拟合探针网格（任务 14 / §9.2-K）。
///
/// 规则（每一条都有理由，改动前请先读）：
///   · 格距由**最长边**推出：`cellSize = maxExtent / (cellsAlongLongestAxis - 1)`。
///     三轴共用同一个格距——若各轴各按自己的边长取格距，三线性插值会在短轴上被拉伸
///     （同一世界距离在不同轴上对应不同的探针间距）。
///   · 每轴探针数取"**刚好罩住该轴**"：`N_i = ceil(size_i / cellSize) + 1`，至少 2。
///     若三轴统一取最长边的探针数，短轴会白铺大量探针：Sponza 场景
///     （3720.9 × 1555.9 × 2288.2）三轴同取 15 格 ⇒ Z 轴浪费 63% 的探针。
///   · 原点取包围盒最小角，于是网格覆盖 `[mn, mn + (N-1)·cellSize] ⊇ [mn, mx]`。
///   · 退化包围盒（任一轴 `size <= 0`，含 NaN）返回 `std::nullopt`，调用方**保持原参数**。
inline std::optional<GIProbeGridFit> FitProbeGridToBounds(const float3& mn, const float3& mx,
                                                          u32 cellsAlongLongestAxis) {
    const float3 size = mx - mn;
    // `!(x > 0)` 而非 `x <= 0`：NaN 与 0 都走同一条"退化"分支
    if (!(size.x > 0.0f) || !(size.y > 0.0f) || !(size.z > 0.0f)) return std::nullopt;

    const float maxExtent = std::max(size.x, std::max(size.y, size.z));
    const u32   cells     = std::max(2u, cellsAlongLongestAxis);

    GIProbeGridFit fit;
    fit.cellSize = maxExtent / float(cells - 1u);
    auto axisCells = [&](float s) {
        return (u32)std::max(2.0f, std::ceil(s / fit.cellSize) + 1.0f);
    };
    fit.countX = axisCells(size.x);
    fit.countY = axisCells(size.y);
    fit.countZ = axisCells(size.z);
    fit.origin = mn;
    return fit;
}

} // namespace he::render
