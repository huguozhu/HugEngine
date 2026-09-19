// ============================================================
// Lumen/LumenTraceConfig.cpp — 追踪/着色二维配置的校验（步骤 21）
// ============================================================

#include "Lumen/LumenTraceConfig.h"

#include "Core/Log.h"

namespace he::render {

const char* LumenTraceSourceName(LumenTraceSource s) {
    switch (s) {
        case LumenTraceSource::SDF:        return "SDF";
        case LumenTraceSource::HardwareRT: return "HardwareRT";
        case LumenTraceSource::Screen:     return "Screen";
        default:                           return "Unknown";
    }
}

const char* LumenShadeSourceName(LumenShadeSource s) {
    switch (s) {
        case LumenShadeSource::SurfaceCache: return "SurfaceCache";
        case LumenShadeSource::HitLighting:  return "HitLighting";
        case LumenShadeSource::Neutral:      return "Neutral";
        default:                             return "Unknown";
    }
}

std::string LumenTraceConfig::Validate() const {
    // ① SDF × HitLighting：**必须显式拒绝**（计划里点名的非法组合）
    //    原因：SDF 的 march 只给出"命中距离"，拿不到命中点的三角形/重心坐标/材质 UV ——
    //    要做命中点光照必须先有这些信息（HWRT 的 closest-hit 才有）。放行只会得到一片黑。
    if (trace == LumenTraceSource::SDF && shade == LumenShadeSource::HitLighting) {
        return "非法组合：SDF 追踪 × HitLighting —— SDF 只能给命中距离，拿不到重心坐标/材质，"
               "要做命中点光照必须用 HardwareRT 追踪";
    }
    // ② 光线数：§6 规定 8–16
    if (traceRep < 8u || traceRep > 16u) {
        return "traceRep 必须在 8–16 之间（§6 的二维配置）";
    }
    // ③ 着色重复次数至少 1
    if (shadeRep == 0u) return "shadeRep 至少为 1";
    // ④ 屏幕空间追踪首版关闭：打开但没有屏幕空间追踪的实现会静默退化成"什么都不追"
    if (trace == LumenTraceSource::Screen && !screenTrace) {
        return "trace = Screen 但 screenTrace = false（自相矛盾：要么打开 screenTrace，要么换追踪源）";
    }
    // ⑤ 硬件光追要求加速结构：本仓库总是有 TLAS，但若 hwFarField 打开而 trace 仍是 SDF，
    //    只是"远场用光追"的混合模式，合法 —— 这里不拦。
    return {};
}

} // namespace he::render
