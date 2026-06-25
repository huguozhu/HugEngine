#pragma once

#include "Scene/MeshComponent.h"

// ============================================================
// SphereComponent — 球体形状组件
//
// 继承 MeshComponent，在 OnCreate 时自动生成 UV 球几何。
// 用法: world.AddComponent<SphereComponent>(entity);
// ============================================================

namespace he {

class SphereComponent : public MeshComponent {
    HE_COMPONENT()
public:
    void OnCreate() override;

    // 配置（须在 AddComponent 之前设置）
    float radius       = 0.5f;     // 球体半径
    u32   segmentCount = 32;       // 经线段数（越高越光滑）
    u32   ringCount    = 16;       // 纬线环数
};

} // namespace he
