#pragma once

#include "Core/Types.h"

#include <atomic>
#include <optional>
#include <string_view>

// ============================================================
// RenderThreadingMode —— 渲染线程模式（渲染线程化方案 B §1.2 / §12，阶段 0 任务 T0.5）
//
// 三种模式一次到位（方案原文的阶段 0 只写了 `enableRenderThread` 布尔；实施时改为**三态模式参数**，
// 因为它同时决定 §12 附录 C 的三线程升级路径，一次定清口径可以避免以后再改配置语义）：
//
//   SingleThreaded      单线程渲染：收集 / 录制 / 提交 / Present 全在游戏线程（= 今天的现状）
//   RenderThread        游戏线程 + 渲染线程：渲染侧全部迁到一根独立线程（方案 B 的目标形态）
//   RenderThreadAndRHI  游戏线程 + 渲染线程 + RHI 线程：§12 的三线程形态（RHI 线程是唯一提交者）
//
// 【为什么放在 Core】EngineConfig 在 Core 层，而模式要被 RHI（T0.1 的归属检查）、渲染管线、
// 样例循环同时读到；放这里可以避免 Core 反向依赖 Render。
//
// 【阶段 0 的语义】本阶段只落地"参数 + 读取路径"：T0.3 的壳实现让 RenderThread 模式**仍在当前
// 线程消费**（行为与现状逐像素一致），阶段 2（T2.1/T2.2）才真正起线程；三线程模式要到 §12 的
// A2 才落地。因此默认值必须是 `SingleThreaded`，且它永远是回退态。
// ============================================================

namespace he {

enum class RenderThreadingMode : u8 {
    SingleThreaded     = 0,   // 单线程渲染（现状；唯一的回退态）
    RenderThread       = 1,   // 游戏线程 + 渲染线程
    RenderThreadAndRHI = 2,   // 游戏线程 + 渲染线程 + RHI 线程（§12 三线程）
};

/// 模式的规范名（日志 / 配置 / 单测共用一套口径）
[[nodiscard]] constexpr const char* RenderThreadingModeName(RenderThreadingMode mode) {
    switch (mode) {
    case RenderThreadingMode::SingleThreaded:     return "single-threaded";
    case RenderThreadingMode::RenderThread:       return "game+render";
    case RenderThreadingMode::RenderThreadAndRHI: return "game+render+rhi";
    }
    return "unknown";
}

namespace detail {

/// 大小写不敏感的相等（配置里出现 "Single"/"RENDER" 都应接受）
[[nodiscard]] inline bool ModeTextEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

} // namespace he::detail

/// 解析渲染线程模式：接受数字（"0"/"1"/"2"）与名称/别名（大小写不敏感）：
///   "0" / "single" / "single-threaded" / "off"        → SingleThreaded
///   "1" / "render" / "game+render" / "renderthread"   → RenderThread
///   "2" / "rhi"    / "game+render+rhi" / "render+rhi" → RenderThreadAndRHI
///
/// 解析失败返回 `nullopt`：调用方应当**报错并保持原模式**，不要静默退回单线程 ——
/// 静默退回会让"以为开了多线程"的实测数据失去意义。
[[nodiscard]] inline std::optional<RenderThreadingMode> ParseRenderThreadingMode(std::string_view text) {
    if (text.empty()) return std::nullopt;

    if (text == "0") return RenderThreadingMode::SingleThreaded;
    if (text == "1") return RenderThreadingMode::RenderThread;
    if (text == "2") return RenderThreadingMode::RenderThreadAndRHI;

    using detail::ModeTextEquals;
    if (ModeTextEquals(text, "single") || ModeTextEquals(text, "single-threaded") ||
        ModeTextEquals(text, "singlethreaded") || ModeTextEquals(text, "off"))
        return RenderThreadingMode::SingleThreaded;

    if (ModeTextEquals(text, "render") || ModeTextEquals(text, "renderthread") ||
        ModeTextEquals(text, "game+render") || ModeTextEquals(text, "render-thread"))
        return RenderThreadingMode::RenderThread;

    if (ModeTextEquals(text, "rhi") || ModeTextEquals(text, "render+rhi") ||
        ModeTextEquals(text, "renderthread+rhi") || ModeTextEquals(text, "game+render+rhi") ||
        ModeTextEquals(text, "renderthreadandrhi"))
        return RenderThreadingMode::RenderThreadAndRHI;

    return std::nullopt;
}

// --- 进程级当前模式 ---
// 【为什么用进程级而不是随 EngineConfig 逐层透传】模式要被 RHI（归属检查）、渲染管线、样例循环
// 同时读到，逐层传参会把 config 穿过整个渲染层；而且验收需要在运行期切换三种模式做对照，
// 也需要一个统一入口。写入用原子（可能在渲染线程上被读），读取无锁。

[[nodiscard]] inline std::atomic<RenderThreadingMode>& RenderThreadingModeStorage() {
    static std::atomic<RenderThreadingMode> s_Mode{RenderThreadingMode::SingleThreaded};
    return s_Mode;
}

[[nodiscard]] inline RenderThreadingMode GetRenderThreadingMode() {
    return RenderThreadingModeStorage().load(std::memory_order_acquire);
}

inline void SetRenderThreadingMode(RenderThreadingMode mode) {
    RenderThreadingModeStorage().store(mode, std::memory_order_release);
}

/// 便捷判据（读当前模式）：`UsesRHIThread()` 只对三线程模式为真
[[nodiscard]] inline bool UsesRenderThread() {
    return GetRenderThreadingMode() != RenderThreadingMode::SingleThreaded;
}
[[nodiscard]] inline bool UsesRHIThread() {
    return GetRenderThreadingMode() == RenderThreadingMode::RenderThreadAndRHI;
}

} // namespace he
