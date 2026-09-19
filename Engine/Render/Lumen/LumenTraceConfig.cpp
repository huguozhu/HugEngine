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

// ============================================================
// 步骤 28：§6 的组合表写成**代码**（而不是只写在文档里）
//
// §6 的表格逐行对应：
//   SDF × SurfaceCache      ✅ 首版默认            → Implemented
//   HW(RT) × SurfaceCache   ✅                    → Implemented（步骤 26 起远场就是这条）
//   HW(RT) × HitLighting    ✅（第二版）           → LegalNotImplemented（必须显式报"未实现"）
//   SDF × HitLighting       ❌ 语义不成立          → Illegal
//   Screen × 任意           ✅（作为**优先层**）    → LegalNotImplemented（screenTrace 首版关闭）
// 另外 `Neutral` 是调试/降级用的着色源，任何追踪源都合法且已实现（就是写中性值）。
// ============================================================
LumenCombinationStatus LumenClassifyCombination(LumenTraceSource trace, LumenShadeSource shade,
                                                bool screenTrace) {
    // 非法：SDF 拿不到命中点的三角形信息，做不了命中点光照
    if (trace == LumenTraceSource::SDF && shade == LumenShadeSource::HitLighting) {
        return LumenCombinationStatus::Illegal;
    }
    // 屏幕空间追踪是"优先层"：首版没有实现，显式报未实现（不许静默退化成 SDF 追踪）
    if (trace == LumenTraceSource::Screen || screenTrace) {
        return LumenCombinationStatus::LegalNotImplemented;
    }
    // 命中点光照是第二版的内容（需要材质求值 + NEE 直接光）
    if (shade == LumenShadeSource::HitLighting) {
        return LumenCombinationStatus::LegalNotImplemented;
    }
    // 其余（SDF/HWRT × SurfaceCache/Neutral）都是首版已实现的路径
    return LumenCombinationStatus::Implemented;
}

const char* LumenCombinationStatusName(LumenCombinationStatus s) {
    switch (s) {
        case LumenCombinationStatus::Implemented:         return "已实现";
        case LumenCombinationStatus::LegalNotImplemented: return "合法但首版未实现";
        case LumenCombinationStatus::Illegal:             return "非法组合";
        default:                                          return "未知";
    }
}

} // namespace he::render
