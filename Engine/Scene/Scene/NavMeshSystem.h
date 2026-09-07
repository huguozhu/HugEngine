#pragma once

// ============================================================
// Scene/NavMeshSystem.h — 导航网格 A* 寻路（C4）
//
// 在 NavMeshComponent 的 grid 上做 8 向 A*，返回世界坐标路径点数组。
// 供 NavAgentSystem（沿路径移动）与 AI/Agent 决策调用。
// ============================================================

#include "Math/Math.h"
#include <vector>

namespace he {

class NavMeshComponent;
class World;

class NavMeshSystem {
public:
    /// A* 8 向寻路：from → to（世界坐标）。成功返回 true 并填充 outPath（世界点序列，含 to）。
    /// 对角移动不允许穿角（两正交格都阻挡则阻断）。
    static bool FindPath(World& world, NavMeshComponent& nav,
                         const float3& from, const float3& to,
                         std::vector<float3>& outPath);

    /// 世界坐标点是否可通行（在网格内且非阻挡）
    static bool IsWalkable(World& world, NavMeshComponent& nav, const float3& point);
};

} // namespace he
