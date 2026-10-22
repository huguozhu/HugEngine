#pragma once

// ============================================================
// CameraInput —— 样例共用的"鼠标 + 键盘控制相机"逻辑
//
// 【为什么要有它】02/03/04/05/06/07 六个样例各自抄了同一段 ~12 行：右键拖拽 → `Rotate`、
// WASD/QE/Shift → `MoveInput` → `Update`。抄多份的代价不只是行数：灵敏度、Y 轴方向、
// 光标捕获/恢复、切换键去抖这些细节一旦某个样例改了就各不相同（用户在不同样例里手感不一样）。
// 本模块把**轮询与状态机**收在一处，各样例只保留自己的**策略**（是否启用升降、切换键是什么、
// 按下切换键后做什么）。
//
// 【放在 Samples/Common 而不是引擎】它直接依赖 GLFW（`GLFWwindow*`、`glfwGetKey`…），
// 而引擎的 `render::CameraController` 刻意不依赖任何窗口库（`MoveInput` 是纯数据）。
// 把 GLFW 轮询放进引擎会让引擎反向依赖窗口后端；放在样例共用目录即可。
//
// 【用法（每帧一次）】
//     static he::samples::CameraInput camInput;                       // 跨帧保存状态
//     if (camInput.Update(glfwWin, camCtrl, deltaTime, options)) {    // 返回 true = 切换键刚按下
//         animCameraMode = !animCameraMode;                           // 样例自己的策略
//     }
// ============================================================

#include "Pipeline/CameraController.h"

#include <GLFW/glfw3.h>   // 样例侧直接轮询输入（引擎层不依赖窗口库）

namespace he::samples {

/// 相机输入策略（各样例按需覆盖；字段都有合理默认值）
struct CameraInputOptions {
    float lookSensitivity = 0.003f;   ///< 鼠标灵敏度（弧度/像素）
    bool  invertY         = false;    ///< 是否反转俯仰方向
    bool  enableVertical  = true;     ///< 是否响应 Q/E 升降
    bool  enableSprint    = true;     ///< 是否响应 Shift 加速
    int   toggleKey       = 0;        ///< 切换键（GLFW_KEY_*；0 = 不检测）
};

/// 相机输入状态机：跨帧保存"右键是否按下 / 上次光标位置 / 切换键去抖"
class CameraInput {
public:
    /// 清空内部状态（例如从动画相机切回手动相机时调用，避免用到过期的光标坐标）
    void Reset() {
        m_RightDown    = false;
        m_ToggleWasDown = false;
        m_LastX = m_LastY = 0.0;
    }

    /// 每帧调用一次：轮询鼠标/键盘并驱动 `camCtrl`。
    /// 返回 true 表示 `opt.toggleKey` 本帧**刚按下**（调用方可据此切换动画相机等）。
    bool Update(GLFWwindow* win, render::CameraController& camCtrl, float deltaTime,
                const CameraInputOptions& opt = {}) {
        if (!win) return false;

        bool toggled = false;

        // ── 右键拖拽旋转 ──
        const bool down = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        if (down && !m_RightDown) {
            // 刚按下：记录起点并捕获光标（否则拖出窗口会断）
            glfwGetCursorPos(win, &m_LastX, &m_LastY);
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        } else if (!down && m_RightDown) {
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else if (down && m_RightDown) {
            double cx = 0.0, cy = 0.0;
            glfwGetCursorPos(win, &cx, &cy);
            const float dx = static_cast<float>(cx - m_LastX);
            const float dy = static_cast<float>(cy - m_LastY);
            m_LastX = cx;
            m_LastY = cy;
            camCtrl.Rotate(dx * opt.lookSensitivity,
                           (opt.invertY ? dy : -dy) * opt.lookSensitivity);
        }
        m_RightDown = down;

        // ── 切换键（边沿触发，去抖）──
        if (opt.toggleKey != 0) {
            const bool k = glfwGetKey(win, opt.toggleKey) == GLFW_PRESS;
            if (k && !m_ToggleWasDown) toggled = true;
            m_ToggleWasDown = k;
        }

        // ── WASD / QE / Shift ──
        render::CameraController::MoveInput in;
        in.forward  = glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS;
        in.backward = glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS;
        in.left     = glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS;
        in.right    = glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS;
        in.up       = opt.enableVertical && glfwGetKey(win, GLFW_KEY_E) == GLFW_PRESS;
        in.down     = opt.enableVertical && glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS;
        in.sprint   = opt.enableSprint   && glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
        camCtrl.Update(deltaTime, in);

        return toggled;
    }

    /// 右键当前是否按住（样例可据此隐藏/显示提示文字）
    [[nodiscard]] bool IsRightMouseDown() const { return m_RightDown; }

private:
    bool   m_RightDown     = false;   ///< 上一帧右键状态（用于边沿检测与光标捕获/恢复）
    bool   m_ToggleWasDown = false;   ///< 上一帧切换键状态（去抖）
    double m_LastX = 0.0;             ///< 上次光标 X（拖动增量）
    double m_LastY = 0.0;             ///< 上次光标 Y
};

} // namespace he::samples
