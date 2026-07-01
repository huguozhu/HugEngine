#include "Pipeline/CameraController.h"

namespace he::render {

CameraController::CameraController() {
    m_Camera.position = float3(0.0f, 2.0f, 8.0f);
    m_Camera.up       = float3(0.0f, 1.0f, 0.0f);
    SetOrientationFromForward(float3(0.0f, -0.2f, -1.0f));
}

void CameraController::SetPosition(const float3& pos) {
    m_Camera.position = pos;
}

void CameraController::SetOrientation(float yawRad, float pitchRad) {
    m_Yaw   = yawRad;
    m_Pitch = pitchRad;
    UpdateForward();
}

void CameraController::SetOrientationFromForward(const float3& forward) {
    float3 f = glm::normalize(forward);
    m_Yaw   = std::atan2(f.x, -f.z);
    m_Pitch = std::asin(f.y);
    m_Camera.forward = f;
}

void CameraController::Rotate(float deltaYaw, float deltaPitch) {
    m_Yaw   += deltaYaw;
    m_Pitch += deltaPitch;       // deltaPitch = dy * sensitivity，其中 dy = cy - lastY（上推鼠标 dy<0）
    m_Pitch  = glm::clamp(m_Pitch, -1.5f, 1.5f);  // 限制 ±86° 避免万向锁
    UpdateForward();
}

void CameraController::Update(float deltaTime, const MoveInput& input) {
    float speed = m_MoveSpeed * deltaTime;
    if (input.sprint) speed *= 3.0f;

    float3 move(0.0f);

    if (m_MoveMode == MoveMode::Ground) {
        // 水平面移动：forward/right 投影到 XZ 平面，避免抬头时附带垂直分量
        float3 groundForward = glm::normalize(
            float3(m_Camera.forward.x, 0.0f, m_Camera.forward.z));
        float3 groundRight = glm::normalize(
            glm::cross(groundForward, float3(0.0f, 1.0f, 0.0f)));

        if (input.forward)  move += groundForward;
        if (input.backward) move -= groundForward;
        if (input.left)     move -= groundRight;
        if (input.right)    move += groundRight;
        if (input.up)       move += float3(0.0f, 1.0f, 0.0f);
        if (input.down)     move -= float3(0.0f, 1.0f, 0.0f);
    } else {
        // 完全 3D 飞行模式
        float3 right = glm::normalize(glm::cross(m_Camera.forward, m_Camera.up));

        if (input.forward)  move += m_Camera.forward;
        if (input.backward) move -= m_Camera.forward;
        if (input.left)     move -= right;
        if (input.right)    move += right;
        if (input.up)       move += m_Camera.up;
        if (input.down)     move -= m_Camera.up;
    }

    if (glm::dot(move, move) > 0.0001f) {
        m_Camera.position += glm::normalize(move) * speed;
    }
}

void CameraController::UpdateForward() {
    float3 fwd;
    fwd.x =  cos(m_Pitch) * sin(m_Yaw);
    fwd.y =  sin(m_Pitch);
    fwd.z = -cos(m_Pitch) * cos(m_Yaw);
    m_Camera.forward = glm::normalize(fwd);
}

} // namespace he::render
