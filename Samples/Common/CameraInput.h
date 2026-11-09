#pragma once

// ============================================================
// CameraInput —— 样例共用的"鼠标 + 键盘控制相机"逻辑
//
// 【为什么要有它】02/03/04/05/06/07 六个样例各自抄了同一段 ~12 行：右键拖拽 → `Rotate`、
// WASD/QE/Shift → `MoveInput` → `Update`。抄多份的代价不只是行数：灵敏度、Y 轴方向、
// 光标捕获/恢复、切换键去抖这些细节一旦某个样例改了就各不相同（用户在不同样例里手感不一样）。
// 本模块把**轮询与状态机**收在一处，各样例只保留自己的**策略**。
//
// 【P0-3 起不再依赖 GLFW】输入由引擎的 `he::PollInput(window)` 提供一份平台无关快照
// （Windows 用 GLFW 轮询填充，鸿蒙用触摸事件填充）。这样同一份相机逻辑在两端都能用，
// 鸿蒙上"右键拖拽"由触摸拖动映射而来（见 InputOHOS.cpp）。
//
// 【用法（每帧一次）】
//     static he::samples::CameraInput camInput;
//     auto& in = he::PollInput(engine.GetWindow());
//     if (camInput.Update(in, camCtrl, deltaTime, options)) { ... }
// ============================================================

#include "Platform/IInput.h"
#include "Pipeline/CameraController.h"

namespace he::samples {

/// 相机输入策略（各样例按需覆盖；字段都有合理默认值）
struct CameraInputOptions {
    float lookSensitivity = 0.003f;   ///< 鼠标灵敏度（弧度/像素）
    bool  invertY         = false;    ///< 是否反转俯仰方向
    bool  enableVertical  = true;     ///< 是否响应 Q/E 升降
    bool  enableSprint    = true;     ///< 是否响应 Shift 加速
    Key   toggleKey       = Key::Count;   ///< 切换键（Key::Count = 不检测）
};

/// 相机输入状态机：跨帧保存"右键是否按下 / 上次光标位置 / 切换键去抖"
class CameraInput {
public:
    /// 清空内部状态（例如从动画相机切回手动相机时调用，避免用到过期的光标坐标）
    void Reset() {
        m_RightDown     = false;
        m_ToggleWasDown = false;
    }

    /// 每帧调用一次：消费平台无关的输入快照并驱动 `camCtrl`。
    /// 返回 true 表示 `opt.toggleKey` 本帧**刚按下**（调用方可据此切换动画相机等）。
    /// 【为什么用重载而不是默认实参】`const CameraInputOptions& = {}` 依赖聚合体的
    // default member initializer；clang 在类定义内解析默认实参时会报
    // "default member initializer needed within definition of enclosing class"。
    // 加构造函数又会破坏指定初始化（`.toggleKey = ...`），所以用重载。
    bool Update(const InputState& in, render::CameraController& camCtrl, float deltaTime) {
        return Update(in, camCtrl, deltaTime, CameraInputOptions{});
    }

    bool Update(const InputState& in, render::CameraController& camCtrl, float deltaTime,
                const CameraInputOptions& opt) {
        bool toggled = false;

        // ── 鼠标右键拖拽旋转（桌面）；移动端由 InputOHOS 把单指拖动映射成 mouseDelta ──
        const bool down = in.mouseDown[1];   // 1 = 右键
        if (down && m_RightDown) {
            // 按住期间用相对位移旋转（光标已被隐藏/锁定，不会跳变）
            camCtrl.Rotate(in.mouseDelta.x * opt.lookSensitivity,
                           (opt.invertY ? in.mouseDelta.y : -in.mouseDelta.y) * opt.lookSensitivity);
        }
        m_RightDown = down;

        // ── 切换键（边沿触发，去抖）──
        if (opt.toggleKey != Key::Count) {
            const bool k = in.IsDown(opt.toggleKey);
            if (k && !m_ToggleWasDown) toggled = true;
            m_ToggleWasDown = k;
        }

        // ── WASD / QE / Shift ──
        render::CameraController::MoveInput move;
        move.forward  = in.IsDown(Key::W);
        move.backward = in.IsDown(Key::S);
        move.left     = in.IsDown(Key::A);
        move.right    = in.IsDown(Key::D);
        move.up       = opt.enableVertical && in.IsDown(Key::E);
        move.down     = opt.enableVertical && in.IsDown(Key::Q);
        move.sprint   = opt.enableSprint   && in.IsDown(Key::LeftShift);
        camCtrl.Update(deltaTime, move);

        return toggled;
    }

    /// 右键当前是否按住（样例可据此隐藏/显示提示文字）
    [[nodiscard]] bool IsRightMouseDown() const { return m_RightDown; }

private:
    bool m_RightDown     = false;   ///< 上一帧右键状态（边沿检测）
    bool m_ToggleWasDown = false;   ///< 上一帧切换键状态（去抖）
};

} // namespace he::samples
