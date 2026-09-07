#pragma once

// ============================================================
// Scene/NavMeshComponent.h — 导航网格组件（C4 NavMesh 寻路，简化 Grid + A*）
//
// 纯数据：均匀格子导航网格（cellSize / 宽高 / origin）+ 阻挡标记 blocked。
// 世界坐标 <-> 格子坐标 换算 + 阻挡查询。A* 寻路由 NavMeshSystem 提供。
// 范围外：Recast 三角形网格生成、路径平滑（漏斗）、动态避让（用 blocked 标记替代）。
// ============================================================

#include "Scene/Component.h"
#include "Math/Math.h"

#include <vector>

namespace he {

class NavMeshComponent : public he::Component {
    HE_COMPONENT()
public:
    float cellSize = 1.0f;            // 每格世界尺寸（米）
    int   width    = 0;               // 格子列数
    int   height   = 0;               // 格子行数
    float3 origin  = float3(0.0f);    // 网格原点（世界，格子 (0,0) 中心）
    std::vector<bool> blocked;        // width*height；true=阻挡（不可通行）

    /// 用宽高/格尺寸分配 blocked 数组（默认全部可通行）
    void Resize(int w, int h, float cell) {
        width = w; height = h; cellSize = cell;
        blocked.assign((usize)(w * h), false);
    }

    /// 设置某格阻挡
    void SetBlocked(int col, int row, bool b) {
        if (col < 0 || row < 0 || col >= width || row >= height) return;
        blocked[(usize)(row * width + col)] = b;
    }

    /// shift位世界坐标 → 格子（col,row）；越界返回 false
    bool WorldToCell(const float3& world, int& col, int& row) const {
        if (cellSize <= 0.0f) return false;
        float dx = world.x - origin.x;
        float dz = world.z - origin.z;
        col = (int)std::floor(dx / cellSize);
        row = (int)std::floor(dz / cellSize);
        return col >= 0 && row >= 0 && col < width && row < height;
    }

    /// 格子中心 → 世界坐标
    float3 CellToWorld(int col, int row) const {
        return float3(origin.x + (col + 0.5f) * cellSize, 0.0f, origin.z + (row + 0.5f) * cellSize);
    }

    /// 某格是否阻挡（越界视为阻挡）
    bool IsBlocked(int col, int row) const {
        if (col < 0 || row < 0 || col >= width || row >= height) return true;
        return blocked[(usize)(row * width + col)];
    }
};

} // namespace he
