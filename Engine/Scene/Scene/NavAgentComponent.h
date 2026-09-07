#pragma once

// ============================================================
// Scene/NavAgentComponent.h — 寻路代理组件（C4）
//
// 挂在实体上，沿 NavMeshSystem::FindPath 得到的路径移动（更新 Transform.position）。
// 目标点变化时置 needsRepath；到达目标置 bReached。
// 范围外：动态避让（用 NavMesh blocked 标记替代）、路径平滑（漏斗）。
// ============================================================

#include "Scene/Component.h"
#include "Math/Math.h"

#include <vector>

namespace he {

class NavAgentComponent : public he::Component {
    HE_COMPONENT()
public:
    Entity navMeshEntity;              // 引用的 NavMeshComponent 实体
    float3 target   = float3(0.0f);    // 世界目标点
    bool   hasTarget = false;
    bool   bReached = false;
    bool   needsRepath = true;         // target 变化/初始 → 重新寻路
    float  speed    = 3.0f;            // 移动速度（米/秒）
    float  arriveRadius = 0.5f;        // 到达判定半径

    void SetTarget(const float3& t) {
        target = t; hasTarget = true; bReached = false; needsRepath = true;
    }
    void ClearTarget() { hasTarget = false; bReached = true; }

    // 运行时路径缓存（由 NavAgentSystem 填充）
    std::vector<float3> path;
    int   pathIndex   = 0;
};

} // namespace he
