#pragma once

// ============================================================
// IInput.h —— 平台无关的输入状态 + 每帧采样入口
//
// 【为什么这样设计】样例原来直接调用 glfwGetKey / glfwGetCursorPos，把输入与窗口库
// 绑死。鸿蒙上没有 GLFW（触摸事件来自 XComponent），因此把"设备事件"先收敛成
// 一份每帧快照 InputState：
//   Windows: InputWin32.cpp 用 GLFW 轮询填充（键鼠 + 相对鼠标模式）
//   鸿蒙:    InputOHOS.cpp 把 XComponent 的触摸回调入队，PollInput 时换算成状态
// 样例（CameraInput / UI）只读 InputState，不再认识 GLFW。
// ============================================================

#include "Core/Types.h"
#include "Math/Math.h"

namespace he {

class IWindow;

/// 用到的按键（只列引擎/样例实际用到的，避免维护一张完整键表）
enum class Key : u16 {
    A = 0, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Escape, Space, LeftShift, LeftCtrl, Tab,
    Left, Right, Up, Down,
    F1, F2, F3, F4,
    Count
};

constexpr u32 kKeyCount = static_cast<u32>(Key::Count);
constexpr u32 kMaxTouchPoints = 8;

struct TouchPoint {
    u32    id    = 0;
    float2 pos{ 0.0f, 0.0f };      // 归一化屏幕坐标 [0,1]
    float2 delta{ 0.0f, 0.0f };    // 本帧位移（归一化）
    bool   began = false;          // 本帧按下
    bool   ended = false;          // 本帧抬起
};

/// 一帧的输入快照
struct InputState {
    // 键鼠（桌面）
    bool   keys[kKeyCount]        = {};
    bool   keysPressed[kKeyCount] = {};   // 本帧刚按下
    float2 mousePos{ 0.0f, 0.0f };
    float2 mouseDelta{ 0.0f, 0.0f };
    bool   mouseDown[3]    = {};          // 左 / 右 / 中
    bool   mousePressed[3] = {};

    // 触摸（移动端）
    u32        touchCount = 0;
    TouchPoint touches[kMaxTouchPoints] = {};

    bool IsDown(Key k) const    { return keys[static_cast<u32>(k)]; }
    bool IsPressed(Key k) const { return keysPressed[static_cast<u32>(k)]; }
};

/// 每帧调用一次：采样当前窗口输入并返回快照（由各平台实现）。
/// 同一帧内重复调用返回同一份状态（内部按时间戳去重）。
InputState& PollInput(IWindow* window);

} // namespace he
