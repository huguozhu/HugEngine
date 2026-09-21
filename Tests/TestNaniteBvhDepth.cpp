// ============================================================
// Tests/TestNaniteBvhDepth.cpp — 每簇 BVH 节点深度的正确性用例（§14.8 任务 26）
//
// 【本文件钉住什么】任务 26 的"BVH 深度"可视化（档位 4）需要一个每簇的**节点深度**镜像。
//   它是从"资产 → 加速结构"派生出来的**只读**量（不参与剔除、不影响画面），
//   所以它的正确性**只能**靠不变量来钉，而不是靠"看起来对"：
//     ① 口径：深度按**节点数**计，**根 = 1**（与 `NaniteClusterBVH::depth` 同一口径）——
//        这条如果错了，可视化会把整棵树画成偏 1 的一层，而"看图"是发现不了的；
//     ② 长度恒等于**簇数**（不是节数、不是叶子数）：调用方按簇下标直接索引；
//     ③ **不在任何叶子里的簇保持 0**：这是"这个簇不在 BVH 里"的可读信息，
//        绝不能顺手填个 1 把它伪装成"根簇"；
//     ④ **失败语义**：空 BVH ⇒ 返回 false 且清空出参（不编造深度）；
//     ⑤ **防御**：损坏镜像（节点下标越界、叶子簇表越界、簇下标越界）不越界读写、不崩 ——
//        它是 dump 期跑的诊断路径，输入可能来自"正在排查的那个错误镜像"；
//     ⑥ **不递归**：显式栈实现必须能吃下退化的深链（本文件用 400 层，远超构建期的深度上限），
//        递归实现会在这里爆栈；
//     ⑦ 与真实构建的**交叉核对**：`BuildNaniteClusterBVH` 报出的 `depth` 必须等于
//        逐簇深度的最大值 —— 两个独立实现（构建期记深度 vs 事后从镜像推深度）互为对照，
//        任一侧口径漂了都会在这里变红。
//
// 【为什么这些用例只能在这里（而不是运行期读数）】它是纯函数、无 RHI 依赖
//   （`NaniteUpload.cpp` 是 RHI-free 的，见 Tests/CMakeLists.txt 的纪律钉子），
//   因此单测可以**直接编译它**，不必链接 HugEngineRender。
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteUpload.h"

#include <vector>

// 【两个 using 都要】`u32` 等基础别名在 `he` 里，`NaniteClusterBVH` 等在 `he::render` 里
//   —— 与 TestNaniteStream.cpp / TestNaniteTypes.cpp 同一写法。
using namespace he;
using namespace he::render;

namespace {

/// 造一个叶子节点：`left` = 叶子簇表首下标，`count` = 该叶子的簇数（构建契约要求 ≥ 1）
NaniteBVHNode MakeLeaf(u32 left, u32 count) {
    NaniteBVHNode node{};
    node.center[0] = 0.0f; node.center[1] = 0.0f; node.center[2] = 0.0f;
    node.radius    = 1.0f;
    node.left      = left;
    node.right     = kNaniteBVHNoChild;
    node.count     = count;
    node.flags     = kNaniteBVHNodeFlagLeaf;
    return node;
}

/// 造一个内部节点：`left`/`right` = 两个孩子下标，`count` 固定 2、`flags` 不带 leaf 位
NaniteBVHNode MakeInternal(u32 left, u32 right) {
    NaniteBVHNode node{};
    node.center[0] = 0.0f; node.center[1] = 0.0f; node.center[2] = 0.0f;
    node.radius    = 2.0f;
    node.left      = left;
    node.right     = right;
    node.count     = 2u;
    node.flags     = 0u;
    return node;
}

/// 确定性伪随机（线性同余）—— 不用 `<random>`，保证"同一份输入逐位可复现"
float NextUnit(u32& state) {
    state = state * 1664525u + 1013904223u;
    return (float)(state >> 8) * (1.0f / 16777216.0f);
}

} // namespace

// ── ① 失败语义：空 BVH ⇒ false + 清空出参 ──────────────────────────────────
TEST_CASE("NaniteBvhDepth(合成): 空 BVH 返回 false 且清空出参（不编造深度）") {
    NaniteClusterBVH bvh;                     // nodes 为空
    bvh.clusterCount = 7u;                    // 即使簇数非 0，也不该编造出 7 个深度
    std::vector<u32> depths = { 9u, 9u, 9u }; // 预置脏值，用来验证"被清空"而不是"没动"

    const bool ok = ComputeNaniteClusterBVHDepths(bvh, depths);
    CHECK(ok == false);
    CHECK(depths.empty());
}

// ── ② 单叶根：根 = 1，所以该叶子里所有簇的深度都是 1 ──────────────────────
TEST_CASE("NaniteBvhDepth(合成): 单叶根 —— 所有簇深度为 1（根 = 1 的口径）") {
    NaniteClusterBVH bvh;
    bvh.nodes              = { MakeLeaf(0u, 3u) };
    bvh.leafClusterIndices = { 0u, 1u, 2u };
    bvh.clusterCount       = 3u;

    std::vector<u32> depths;
    CHECK(ComputeNaniteClusterBVHDepths(bvh, depths) == true);
    REQUIRE(depths.size() == 3u);
    CHECK(depths[0] == 1u);
    CHECK(depths[1] == 1u);
    CHECK(depths[2] == 1u);
}

// ── ③ 不均衡树：每簇的深度 = 它所在叶子的**节点深度** ─────────────────────
TEST_CASE("NaniteBvhDepth(合成): 不均衡树 —— 每簇深度等于其叶子的节点深度") {
    // 树形（下标即节点下标）：
    //   0 internal ── 1 leaf{cluster 0,1}        （深度 2）
    //              └─ 2 internal ── 3 leaf{cluster 2}   （深度 3）
    //                            └─ 4 leaf{cluster 3}   （深度 3）
    NaniteClusterBVH bvh;
    bvh.nodes = {
        MakeInternal(1u, 2u),   // 0：根，深度 1
        MakeLeaf(0u, 2u),       // 1：左叶，深度 2 → 簇 0、1
        MakeInternal(3u, 4u),   // 2：右子树，深度 2
        MakeLeaf(2u, 1u),       // 3：深度 3 → 簇 2
        MakeLeaf(3u, 1u),       // 4：深度 3 → 簇 3
    };
    bvh.leafClusterIndices = { 0u, 1u, 2u, 3u };
    bvh.clusterCount       = 4u;

    std::vector<u32> depths;
    CHECK(ComputeNaniteClusterBVHDepths(bvh, depths) == true);
    REQUIRE(depths.size() == 4u);
    CHECK(depths[0] == 2u);
    CHECK(depths[1] == 2u);
    CHECK(depths[2] == 3u);
    CHECK(depths[3] == 3u);
}

// ── ④ 长度与"不在 BVH 里"的簇 ─────────────────────────────────────────────
TEST_CASE("NaniteBvhDepth(合成): 长度恒等于簇数；不在任何叶里的簇保持 0") {
    NaniteClusterBVH bvh;
    bvh.nodes              = { MakeLeaf(0u, 2u) };   // 只覆盖簇 0、1
    bvh.leafClusterIndices = { 0u, 1u };
    bvh.clusterCount       = 5u;                     // 簇 2、3、4 不在任何叶里

    std::vector<u32> depths;
    CHECK(ComputeNaniteClusterBVHDepths(bvh, depths) == true);
    REQUIRE(depths.size() == 5u);                    // 长度按**簇数**，不是按叶子簇表条数
    CHECK(depths[0] == 1u);
    CHECK(depths[1] == 1u);
    CHECK(depths[2] == 0u);                          // "不在 BVH 里"是可读信息，不是 1
    CHECK(depths[3] == 0u);
    CHECK(depths[4] == 0u);

    SUBCASE("簇数为 0 但节点表非空 ⇒ 成功且出参为空") {
        NaniteClusterBVH two;
        two.nodes              = { MakeLeaf(0u, 1u) };
        two.leafClusterIndices = { 0u };
        two.clusterCount       = 0u;                 // 自相矛盾的镜像
        std::vector<u32> d;
        CHECK(ComputeNaniteClusterBVHDepths(two, d) == true);
        CHECK(d.empty());
    }
}

// ── ⑤ 防御：损坏镜像不越界读写、不崩 ──────────────────────────────────────
TEST_CASE("NaniteBvhDepth(合成): 损坏镜像的下标越界既不越界读也不越界写") {
    SUBCASE("节点下标越界与叶子簇表越界都被丢弃") {
        //   0 internal(left=1, right=3)         深度 1
        //   1 leaf{cluster 0,1}                 深度 2
        //   2 leaf(left=50, count=4)            深度 3 —— 叶子簇表只有 4 条，slot 50 越界
        //   3 internal(left=2, right=77)        深度 2 —— 77 越界（节点表只有 4 条）
        NaniteClusterBVH bvh;
        bvh.nodes = {
            MakeInternal(1u, 3u),
            MakeLeaf(0u, 2u),
            MakeLeaf(50u, 4u),
            MakeInternal(2u, 77u),
        };
        bvh.leafClusterIndices = { 0u, 1u, 2u, 3u };
        bvh.clusterCount       = 4u;

        std::vector<u32> depths;
        CHECK(ComputeNaniteClusterBVHDepths(bvh, depths) == true);   // 非空节点表 ⇒ 成功
        REQUIRE(depths.size() == 4u);
        CHECK(depths[0] == 2u);      // 来自节点 1
        CHECK(depths[1] == 2u);
        CHECK(depths[2] == 0u);      // 节点 2 的 slot 50 越界 ⇒ 整片跳过，没有越界写
        CHECK(depths[3] == 0u);
    }

    SUBCASE("簇下标越界被丢弃（叶子簇表里的脏值不会写到界外）") {
        NaniteClusterBVH bvh;
        bvh.nodes              = { MakeLeaf(0u, 4u) };
        bvh.leafClusterIndices = { 0u, 1u, 2u, 9u };   // 9 越界（clusterCount = 4）
        bvh.clusterCount       = 4u;

        std::vector<u32> depths;
        CHECK(ComputeNaniteClusterBVHDepths(bvh, depths) == true);
        REQUIRE(depths.size() == 4u);
        CHECK(depths[0] == 1u);
        CHECK(depths[1] == 1u);
        CHECK(depths[2] == 1u);
        CHECK(depths[3] == 0u);      // 越界簇下标既不写、也不崩
    }
}

// ── ⑥ 不递归：400 层退化深链必须能吃下（递归实现会爆栈）───────────────────
TEST_CASE("NaniteBvhDepth(合成): 400 层深链不爆栈且逐簇深度正确") {
    // 结构（每个叶子只被引用一次，因此写入无歧义）：
    //   内部节点 0..N-2 组成左链；内部 i（0<=i<=N-3）的右孩子是一个叶子；
    //   最深的内部节点 N-2 的两个孩子都是叶子。
    //   ⇒ 深度：簇 0 与簇 1 = N；簇 k（2<=k<=N-1）= k。
    constexpr u32 kChain = 400u;
    const u32 n = kChain;                    // 簇数（= 叶子数）

    NaniteClusterBVH bvh;
    bvh.nodes.assign((size_t)(2u * n - 1u), NaniteBVHNode{});
    bvh.leafClusterIndices.assign((size_t)n, 0u);
    bvh.clusterCount = n;

    // 叶子节点下标从 n-1 开始：leaf0 = n-1（簇 0）、leaf1 = n（簇 1）、
    // 其余叶子 n+1+i（簇 2+i）挂在内部节点 i 的右孩子上。
    // 【边界】只有内部节点 0..n-3 带"右孩子是普通叶子"这一形状（最深那个内部节点的两个孩子
    //   都是叶子），所以 i 只到 n-3 —— 写满 2n-1 个槽恰好不越界。
    for (u32 i = 0u; i + 1u < n; ++i) {
        bvh.nodes[i] = (i + 1u < n - 1u) ? MakeInternal(i + 1u, n + 1u + i)
                                         : MakeInternal(n - 1u, n);
    }
    bvh.nodes[n - 1u] = MakeLeaf(0u, 1u);                    // 簇 0
    bvh.nodes[n]      = MakeLeaf(1u, 1u);                    // 簇 1
    for (u32 i = 0u; i + 3u <= n; ++i) {
        bvh.nodes[n + 1u + i] = MakeLeaf(2u + i, 1u);        // 簇 2+i
    }
    for (u32 k = 0u; k < n; ++k) bvh.leafClusterIndices[k] = k;

    std::vector<u32> depths;
    CHECK(ComputeNaniteClusterBVHDepths(bvh, depths) == true);
    REQUIRE(depths.size() == (size_t)n);

    CHECK(depths[0] == n);                                   // 最深处的左叶
    CHECK(depths[1] == n);                                   // 最深处的右叶
    bool chainOk = true;
    for (u32 k = 2u; k < n; ++k) {
        if (depths[k] != k) { chainOk = false; break; }
    }
    CHECK(chainOk);

    u32 maxDepth = 0u;
    bool allReached = true;
    for (u32 d : depths) {
        if (d == 0u) allReached = false;
        if (d > maxDepth) maxDepth = d;
    }
    CHECK(allReached);                                       // 每个簇都落在某个叶子里
    CHECK(maxDepth == n);                                    // 深度口径 = 节点数
}

// ── ⑦ 与真实构建交叉核对 ──────────────────────────────────────────────────
TEST_CASE("NaniteBvhDepth(真实构建): 逐簇深度的最大值等于构建期报出的 depth") {
    // 用确定性伪随机造一批簇球，走**真实的** `BuildNaniteClusterBVH`：
    //   · `depth` 由构建期自己记（另一条代码路径），与事后从镜像推出来的逐簇深度互为对照；
    //   · 两者口径都是"节点数、根 = 1"，所以**必须相等**。
    std::vector<NaniteClusterRecord> clusters;
    clusters.resize(256u);
    u32 state = 20260921u;
    for (size_t i = 0; i < clusters.size(); ++i) {
        clusters[i].boundsCenterRadius[0] = NextUnit(state) * 200.0f - 100.0f;
        clusters[i].boundsCenterRadius[1] = NextUnit(state) * 200.0f - 100.0f;
        clusters[i].boundsCenterRadius[2] = NextUnit(state) * 200.0f - 100.0f;
        clusters[i].boundsCenterRadius[3] = 2.0f + NextUnit(state) * 8.0f;
    }

    NaniteClusterBVH bvh;
    REQUIRE(BuildNaniteClusterBVH(clusters, bvh) == true);
    REQUIRE(bvh.Empty() == false);
    CHECK(bvh.clusterCount == (u32)clusters.size());

    std::vector<u32> depths;
    CHECK(ComputeNaniteClusterBVHDepths(bvh, depths) == true);
    REQUIRE(depths.size() == (size_t)bvh.clusterCount);

    u32 maxDepth   = 0u;
    u32 zeroCount  = 0u;
    for (u32 d : depths) {
        if (d == 0u) ++zeroCount;
        if (d > maxDepth) maxDepth = d;
    }
    CHECK(zeroCount == 0u);                 // 构建期把每个簇都放进了某个叶子
    CHECK(maxDepth == bvh.depth);           // ★ 两条独立路径的口径必须一致
    CHECK(bvh.maxStackDepthUpperBound <= kNaniteBVHMaxStackDepth);   // 既有不变式，顺带钉一下

    SUBCASE("同一输入两次调用逐位一致（纯函数、无隐藏状态）") {
        std::vector<u32> again;
        CHECK(ComputeNaniteClusterBVHDepths(bvh, again) == true);
        CHECK(again == depths);
    }
}
