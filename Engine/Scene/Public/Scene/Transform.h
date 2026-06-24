#pragma once

#include "Scene/Component.h"
#include "Math/Math.h"

// ============================================================
// TransformComponent — 位置 / 旋转 / 缩放
// ============================================================

namespace he {

/// 变换组件（每个可放置在场景中的 Entity 必备）
class TransformComponent : public Component {
    HE_COMPONENT()
public:
    float3 position    = float3(0.0f);
    quat   rotation    = glm::identity<quat>();
    float3 scale       = float3(1.0f);

    /// 获取局部空间变换矩阵
    float4x4 GetLocalMatrix() const {
        float4x4 T = glm::translate(float4x4(1.0f), position);
        float4x4 R = glm::mat4_cast(rotation);
        float4x4 S = glm::scale(float4x4(1.0f), scale);
        return T * R * S;
    }
};

} // namespace he
