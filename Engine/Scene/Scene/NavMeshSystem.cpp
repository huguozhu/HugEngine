// ============================================================
// Scene/NavMeshSystem.cpp — 导航网格 A* 寻路实现（C4）
//
// Grid A*：启发式欧氏距离，8 向邻居（对角不允许穿角）。
// world 预留参数（当前未用；未来支持动态阻挡/多网格查询）。
// ============================================================

#include "Scene/NavMeshSystem.h"
#include "Scene/NavMeshComponent.h"
#include "Scene/World.h"

#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

namespace he {

namespace {
    // 8 向邻居：dx, dz, 代价（对角 √2）
    constexpr int kDirs[8][2] = {
        { 1, 0}, {-1, 0}, { 0, 1}, { 0,-1},
        { 1, 1}, { 1,-1}, {-1, 1}, {-1,-1},
    };
    constexpr float kDiagonal = 1.41421356f;

    struct Node {
        int col, row;
        float g = 0.0f;   // 起点到当前代价
        float f = 0.0f;   // g + 启发式
        int parent = -1;  // 父节点索引
    };
} // namespace

bool NavMeshSystem::IsWalkable(World&, NavMeshComponent& nav, const float3& point) {
    int c, r;
    return nav.WorldToCell(point, c, r) && !nav.IsBlocked(c, r);
}

bool NavMeshSystem::FindPath(World&, NavMeshComponent& nav,
                             const float3& from, const float3& to,
                             std::vector<float3>& outPath) {
    outPath.clear();
    if (nav.width <= 0 || nav.height <= 0 || nav.cellSize <= 0.0f) return false;

    int sc, sr, tc, tr;
    if (!nav.WorldToCell(from, sc, sr) || !nav.WorldToCell(to, tc, tr)) return false;
    if (nav.IsBlocked(sc, sr) || nav.IsBlocked(tc, tr)) return false;

    const int total = nav.width * nav.height;
    std::vector<Node> nodes((usize)total);
    std::vector<bool> closed((usize)total, false);
    // 节点索引 = row*width+col

    auto cellToNodeIdx = [&](int c, int r) { return r * nav.width + c; };
    auto idxToCell = [&](int idx, int& c, int& r) { c = idx % nav.width; r = idx / nav.width; };

    auto heuristic = [&](int c, int r) {
        float dc = (float)(c - tc), dr = (float)(r - tr);
        return std::sqrt(dc * dc + dr * dr);
    };

    // A* 开放集：按 f 值最小优先
    struct QItem { int idx; float f; };
    auto cmp = [](const QItem& a, const QItem& b) { return a.f > b.f; };
    std::priority_queue<QItem, std::vector<QItem>, decltype(cmp)> open(cmp);

    int startIdx = cellToNodeIdx(sc, sr);
    nodes[startIdx].g = 0.0f;
    nodes[startIdx].f = heuristic(sc, sr);
    open.push({startIdx, nodes[startIdx].f});

    while (!open.empty()) {
        QItem cur = open.top(); open.pop();
        int curIdx = cur.idx;
        if (closed[curIdx]) continue;
        closed[curIdx] = true;

        int cc, cr; idxToCell(curIdx, cc, cr);
        if (cc == tc && cr == tr) {   // 到达目标 → 回溯路径
            std::vector<int> pathIdx;
            int n = curIdx;
            while (n != -1) {
                pathIdx.push_back(n);
                n = nodes[n].parent;
            }
            std::reverse(pathIdx.begin(), pathIdx.end());
            for (size_t i = 0; i < pathIdx.size(); ++i) {
                int c2, r2; idxToCell((int)pathIdx[i], c2, r2);
                outPath.push_back(nav.CellToWorld(c2, r2));
            }
            return true;
        }

        for (int d = 0; d < 8; ++d) {
            int nc = cc + kDirs[d][0];
            int nr = cr + kDirs[d][1];
            if (nav.IsBlocked(nc, nr)) continue;
            // 对角不允许穿角：两个正交邻格都阻挡则阻断
            if (kDirs[d][0] != 0 && kDirs[d][1] != 0) {
                if (nav.IsBlocked(cc + kDirs[d][0], cr) || nav.IsBlocked(cc, cr + kDirs[d][1]))
                    continue;
            }
            int nIdx = cellToNodeIdx(nc, nr);
            if (closed[nIdx]) continue;
            float step = (kDirs[d][0] != 0 && kDirs[d][1] != 0) ? kDiagonal : 1.0f;
            float ng = nodes[curIdx].g + step;
            if (ng < nodes[nIdx].g || nodes[nIdx].parent == -1) {
                nodes[nIdx].g = ng;
                nodes[nIdx].f = ng + heuristic(nc, nr);
                nodes[nIdx].parent = curIdx;
                open.push({nIdx, nodes[nIdx].f});
            }
        }
    }

    return false;   // 无路径
}

} // namespace he
