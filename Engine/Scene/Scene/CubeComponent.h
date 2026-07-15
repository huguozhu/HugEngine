#pragma once

#include "Scene/MeshComponent.h"

// ============================================================
// CubeComponent — 立方体形状组件
//
// 继承 MeshComponent，在 OnCreate 时自动生成单位立方体几何。
// 用法: world.AddComponent<CubeComponent>(entity);
// ============================================================

namespace he {

class CubeComponent : public MeshComponent {
    HE_COMPONENT()
public:
    void OnCreate() override;

    // 配置（须在 AddComponent 之前设置）
    float halfExtent = 0.5f;  // 半边长，控制立方体大小
};

} // namespace he
