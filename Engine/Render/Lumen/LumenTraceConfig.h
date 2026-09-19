#pragma once

// ============================================================
// Lumen/LumenTraceConfig.h — 追踪/着色的二维配置（步骤 21，对应《Lumen设计与实现》§6）
//
// 【为什么是"二维"】Lumen 的一次探针射线有两个独立选择：
//   · **追踪（trace）**：用什么求交 —— SDF / 硬件光追（HWRT）/ 屏幕空间（Screen）
//   · **着色（shade）**：命中后拿什么材质 —— Surface Cache / 命中点光照（Hit Lighting）/ 中性值
// 两者是正交的，但**并非所有组合都成立**（见 Validate）。把"非法组合"在配置加载期就拒掉，
// 比在渲染时得到一片黑要好得多 —— 这正是步骤 21 的验收之一。
//
// 【首版配置】`SDF(+HW 远场) × SurfaceCache`、`screenTrace = false`
// （与计划一致：远场用硬件光追是步骤 26–29 的事，这里先把 SDF 追踪的分布做对）。
// ============================================================

#include "Core/Types.h"   // u32（枚举的底层类型在 namespace 之外用到）
#include <string>

namespace he::render {

enum class LumenTraceSource : u32 {
    SDF = 0,        // 全局 SDF 的 sphere tracing（步骤 11 的 march）
    HardwareRT = 1, // 硬件光追（步骤 26+）
    Screen = 2,     // 屏幕空间追踪（可选，首版关闭）
};

enum class LumenShadeSource : u32 {
    SurfaceCache = 0,   // 命中点查 Surface Cache atlas 的材质（L2 的产物）
    HitLighting = 1,    // 命中点直接做光照（需要命中点的材质/法线/光源列表）
    Neutral = 2,        // 中性辐射度（调试/降级）
};

struct LumenTraceConfig {
    LumenTraceSource trace = LumenTraceSource::SDF;
    LumenShadeSource shade = LumenShadeSource::SurfaceCache;
    u32   traceRep  = 8;      // 每探针追踪的光线数（§6：8–16）
    u32   shadeRep  = 1;      // 每条命中光线做几次着色（§6 的 traceRep × shadeRep）
    bool  screenTrace = false;
    bool  hwFarField  = false;   // 远场是否交给硬件光追（步骤 26–29）

    /// 配置校验：返回空串表示合法，否则返回**中文**原因（配置加载期直接报错并拒绝启动该源）
    [[nodiscard]] std::string Validate() const;
};

[[nodiscard]] const char* LumenTraceSourceName(LumenTraceSource s);
[[nodiscard]] const char* LumenShadeSourceName(LumenShadeSource s);

} // namespace he::render
