// ============================================================
// Time.cpp —— 单调时钟实现（跨平台：Windows / 鸿蒙都用 steady_clock）
// ============================================================

#include "Platform/Time.h"

#include <chrono>

namespace he {

f64 Time::Now() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point s_Start = Clock::now();
    const auto elapsed = Clock::now() - s_Start;
    return std::chrono::duration<f64>(elapsed).count();
}

} // namespace he
