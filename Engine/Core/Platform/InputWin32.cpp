// ============================================================
// InputWin32.cpp —— Windows 输入采样（GLFW 轮询）
//
// 把 GLFW 的键鼠状态填进平台无关的 InputState；GLFW 只出现在本文件里。
// 相对鼠标模式（右键拖拽旋转相机）也在这里统一处理：右键按下时隐藏并锁定光标。
// ============================================================

#include "Platform/IInput.h"
#include "Platform/IWindow.h"
#include "Platform/WindowWin32.h"

#include "Core/Log.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace he {
namespace {

// GLFW 键码 ↔ 引擎 Key
int ToGlfwKey(Key k) {
    switch (k) {
        case Key::A: return GLFW_KEY_A;  case Key::B: return GLFW_KEY_B;
        case Key::C: return GLFW_KEY_C;  case Key::D: return GLFW_KEY_D;
        case Key::E: return GLFW_KEY_E;  case Key::F: return GLFW_KEY_F;
        case Key::G: return GLFW_KEY_G;  case Key::H: return GLFW_KEY_H;
        case Key::I: return GLFW_KEY_I;  case Key::J: return GLFW_KEY_J;
        case Key::K: return GLFW_KEY_K;  case Key::L: return GLFW_KEY_L;
        case Key::M: return GLFW_KEY_M;  case Key::N: return GLFW_KEY_N;
        case Key::O: return GLFW_KEY_O;  case Key::P: return GLFW_KEY_P;
        case Key::Q: return GLFW_KEY_Q;  case Key::R: return GLFW_KEY_R;
        case Key::S: return GLFW_KEY_S;  case Key::T: return GLFW_KEY_T;
        case Key::U: return GLFW_KEY_U;  case Key::V: return GLFW_KEY_V;
        case Key::W: return GLFW_KEY_W;  case Key::X: return GLFW_KEY_X;
        case Key::Y: return GLFW_KEY_Y;  case Key::Z: return GLFW_KEY_Z;
        case Key::Num0: return GLFW_KEY_0; case Key::Num1: return GLFW_KEY_1;
        case Key::Num2: return GLFW_KEY_2; case Key::Num3: return GLFW_KEY_3;
        case Key::Num4: return GLFW_KEY_4; case Key::Num5: return GLFW_KEY_5;
        case Key::Num6: return GLFW_KEY_6; case Key::Num7: return GLFW_KEY_7;
        case Key::Num8: return GLFW_KEY_8; case Key::Num9: return GLFW_KEY_9;
        case Key::Escape:    return GLFW_KEY_ESCAPE;
        case Key::Space:     return GLFW_KEY_SPACE;
        case Key::LeftShift: return GLFW_KEY_LEFT_SHIFT;
        case Key::LeftCtrl:  return GLFW_KEY_LEFT_CONTROL;
        case Key::Tab:       return GLFW_KEY_TAB;
        case Key::Left:      return GLFW_KEY_LEFT;
        case Key::Right:     return GLFW_KEY_RIGHT;
        case Key::Up:        return GLFW_KEY_UP;
        case Key::Down:      return GLFW_KEY_DOWN;
        case Key::F1:        return GLFW_KEY_F1;
        case Key::F2:        return GLFW_KEY_F2;
        case Key::F3:        return GLFW_KEY_F3;
        case Key::F4:        return GLFW_KEY_F4;
        default:             return GLFW_KEY_UNKNOWN;
    }
}

struct Win32InputState {
    InputState state;
    float2     lastMouse{ 0.0f, 0.0f };
    bool       hasLastMouse = false;
    bool       relativeMode = false;
    f64        lastPollTime = -1.0;
};

Win32InputState& State() {
    static Win32InputState s;
    return s;
}

} // namespace

InputState& PollInput(IWindow* window) {
    Win32InputState& s = State();
    GLFWwindow* win = GetGlfwWindow(window);
    if (!win) return s.state;

    // 键
    for (u32 i = 0; i < kKeyCount; ++i) {
        const int k = ToGlfwKey(static_cast<Key>(i));
        if (k == GLFW_KEY_UNKNOWN) continue;
        const bool down = glfwGetKey(win, k) == GLFW_PRESS;
        s.state.keysPressed[i] = down && !s.state.keys[i];
        s.state.keys[i] = down;
    }

    // 鼠标位置 / 位移
    double cx = 0.0, cy = 0.0;
    glfwGetCursorPos(win, &cx, &cy);
    const float2 pos{ static_cast<float>(cx), static_cast<float>(cy) };
    if (!s.hasLastMouse) {
        s.lastMouse = pos;
        s.hasLastMouse = true;
    }
    s.state.mousePos   = pos;
    s.state.mouseDelta = pos - s.lastMouse;
    s.lastMouse        = pos;

    // 三键
    const bool left   = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT)   == GLFW_PRESS;
    const bool right  = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT)  == GLFW_PRESS;
    const bool middle = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    const bool now[3] = { left, right, middle };
    for (u32 i = 0; i < 3; ++i) {
        s.state.mousePressed[i] = now[i] && !s.state.mouseDown[i];
        s.state.mouseDown[i] = now[i];
    }

    // 相对鼠标模式：右键按下时隐藏光标（样例的相机拖拽手感依赖它）
    if (right != s.relativeMode) {
        s.relativeMode = right;
        window->SetCursorVisible(!right);
        s.lastMouse = pos;   // 隐藏/恢复光标时丢弃跳变
    }

    // 桌面端没有触摸
    s.state.touchCount = 0;
    return s.state;
}

} // namespace he
