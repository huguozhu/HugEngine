#pragma once

// ============================================================
// Time.h —— 平台无关的单调时钟
//
// 样例原来直接调用 glfwGetTime()（GLFW 专属）。鸿蒙没有 GLFW，改用
// std::chrono::steady_clock：两端都是单调时钟，且不受系统时间调整影响。
// ============================================================

#include "Core/Types.h"

namespace he {

class Time {
public:
    /// 当前时间（秒，单调递增）
    static f64 Now();
};

} // namespace he
