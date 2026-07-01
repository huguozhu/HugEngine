#pragma once

#include "Pipeline/Camera.h"
#include "Math/Math.h"

namespace he::render {

// ============================================================================
// CameraController — 可复用的自由相机控制器
//
// 功能：
//   - 鼠标右键拖拽旋转（yaw/pitch 跟踪）
//   - WASD 水平面移动 + E/Q 垂直升降
//   - 支持 Ground（地面限定）和 Free（完全 3D）两种移动模式
//   - 视线方向从 yaw/pitch 计算，forward.y 跟随 pitch
//
// 用法（每帧）：
//   1. 从 GLFW/输入系统填充 MoveInput
//   2. 从鼠标 delta 调用 Rotate(dx, dy)
//   3. 调用 Update(deltaTime, input)
//   4. 通过 GetCamera() 获取 CameraData 供渲染
// ============================================================================
class CameraController {
public:
    CameraController();

    // ---- 初始设置 ----

    void SetPosition(const float3& pos);
    void SetOrientation(float yawRad, float pitchRad);
    void SetOrientationFromForward(const float3& forward);

    // ---- 鼠标旋转（每帧鼠标 delta） ----

    void Rotate(float deltaYaw, float deltaPitch);  // dx * sensitivity, dy * sensitivity

    // ---- 移动输入 ----

    struct MoveInput {
        bool forward  = false;  // W
        bool backward = false;  // S
        bool left     = false;  // A
        bool right    = false;  // D
        bool up       = false;  // E
        bool down     = false;  // Q
        bool sprint   = false;  // Shift — 3× 加速
    };

    // ---- 移动模式 ----

    enum class MoveMode : u8 {
        Ground,   // 水平面移动：forward/right 投影到 XZ 平面，E/Q 垂直升降
        Free,     // 完全 3D：forward 沿视线方向移动
    };

    // ---- 每帧更新 ----

    void Update(float deltaTime, const MoveInput& input);

    // ---- 查询 ----

    const CameraData& GetCamera() const { return m_Camera; }
    CameraData& GetCamera() { return m_Camera; }

    float GetYaw()   const { return m_Yaw; }
    float GetPitch() const { return m_Pitch; }

    // ---- 参数 ----

    void SetMoveSpeed(float speed)         { m_MoveSpeed = speed; }
    void SetLookSensitivity(float sens)    { m_LookSensitivity = sens; }
    float GetMoveSpeed() const             { return m_MoveSpeed; }
    void SetMoveMode(MoveMode mode)        { m_MoveMode = mode; }
    void SetAspectRatio(float w, float h)  { m_Camera.SetAspectRatio(w, h); }

private:
    void UpdateForward();  // 从 yaw/pitch 重新计算 camera.forward

    CameraData m_Camera;
    float m_Yaw              = 0.0f;
    float m_Pitch            = 0.0f;
    float m_MoveSpeed        = 5.0f;     // 基础移动速度（单位/秒）
    float m_LookSensitivity  = 0.003f;   // 旋转灵敏度
    MoveMode m_MoveMode      = MoveMode::Ground;
};

} // namespace he::render
