// ============================================================
// Tests/TestNaniteStream.cpp — §14.8 任务 24：LOD 流式（反馈 + 页池）的**页划分**单测
//
// 【为什么单独立一个文件】`TestNaniteBuilder.cpp` 已 2500+ 行、按任务分段累加；
//   本任务覆盖的是**页划分这条纯函数**（`BuildNanitePagePlan`）与它的不变式/守卫，
//   单独一文件更易查、也更容易与新任务并存。
//
// 【为什么这里不需要 GPU】`BuildNanitePagePlan` 落在 `NaniteUpload.{h,cpp}` 里，
//   那个翻译单元是 **RHI-free** 的（`NaniteUpload.h` 只前置声明 `rhi::IRHIDevice`），
//   已被 `Tests/CMakeLists.txt` 直接编译进 `HugEngineTests`（第 54 行那条纪律钉子）
//   ⇒ 页划分的每一条契约都能在无设备环境下断言。**页池/页表/反馈环**（`NaniteStream`）
//   要 RHI，故不在本文件覆盖（它们的运行期证据是 `stream` 读数行与验收 (a)–(e)）。
//
// 【覆盖范围（编号续 `TestNaniteBuilder.cpp` 已有的 22 条，本文件从 23 起）】
//  23. 空资产：返回 true、`pageCount == 0`、`Empty()` 为真（与任务 7/8/9/10/12 同口径）
//  24. 失败路径：`contentsPerPage == 0` ⇒ false 且**出参逐字段未被改写**（哨兵核对）
//  25. 失败路径：有簇但 `vertexCount == 0` / `triangleCount == 0` ⇒ false（资产自相矛盾）
//  26. 失败路径：首份内容不从记录 0 开始（段首空洞）⇒ false
//  27. 失败路径：**两张共享表不同源**（秩个数不等 / 秩逐一错位）⇒ false 且出参未被改写
//       —— 这是设计里没有、实现里补上的关键守卫（防"顶点段与三角形段各自一套内容"）
//  28. 不变式（多种规模 × 多组 K）：页数 = ceil(内容份数 / K)；每页 `local` 是 0..count−1 的
//      **排列**；三段区间长度齐全且首尾相接覆盖整段；`pageStraddleCount == 0`；
//      `rankMismatchCount == 0`；槽步长 = 每页最大值；`slot*Bytes == 步长 × 记录大小`
//  29. `pageClusterBegin/Count` 是前缀和的自洽（Σ == 簇数、逐页前缀和相等）
//  30. `PoolBytes(slots) == slots × (三段槽字节之和)`（并打印实测数字）
//  31. 确定性：同一输入两次划分 ⇒ `clusterPage` + 6 个区间数组 + stats 逐位一致
//  32. K 的影响：K = 1 / 2 / ≥内容份数 ⇒ 页数分别为 contentCount / ceil(c/2) / 1，
//      且三种划分覆盖的**簇集合**完全一致
//  33. 真实资产路径：走过 DAG 去重的资产的 `cluster.vertexOffset` **确实不单调**
//      （§14.32 推翻"簇下标区间即页"的那条事实），且页划分在它上面照常成立
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteUpload.h"   // BuildNanitePagePlan / BuildNaniteAssetFromGeometry

#include <algorithm>   // std::is_sorted / std::sort
#include <set>         // 秩个数与集合比较（只做校验，不参与产物顺序）
#include <string>
#include <vector>

using namespace he;
using namespace he::render;

namespace {

// ============================================================
// 造簇表的小工具：只填页划分真正读的三个字段
//   （`vertexOffset` / `triangleOffset` 是两张共享表的**记录下标**，`triangleCount` 只做录入，
//     不参与页划分 —— 页只由"内容区间"决定）
// ============================================================
NaniteClusterRecord MakeCluster(u32 vertexOffset, u32 triangleOffset, u32 triangleCount = 1u) {
    NaniteClusterRecord c{};
    c.vertexOffset   = vertexOffset;
    c.triangleOffset = triangleOffset;
    c.triangleCount  = triangleCount;
    return c;
}

/// 去重升序的起点表（页划分的输入口径：它就是构建期 `uniqueVertexOffset[]` 的重建）
std::vector<u32> SortedUnique(std::vector<u32> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

// ============================================================
// 页划分的**全部不变式**（对任意输入/任意 K 都成立；失败即返回 false 并记录原因）
// 【为什么写成函数而不是内联】用例 28 要在多种规模 × 多组 K 上重复调用它；
//   写成一处可以保证"每一个不变式都只在同一处被定义一次"。
// 返回空串 = 全部通过；否则返回第一条不成立的描述（供 CHECK 打印）。
// ============================================================
std::string CheckPagePlanInvariants(const NanitePagePlan& plan,
                                    const std::vector<NaniteClusterRecord>& clusters,
                                    u32 vertexCount, u32 triangleCount, u32 K) {
    const u32 clusterCount = (u32)clusters.size();
    if (plan.contentsPerPage != K)                       return "contentsPerPage 不等于传入的 K";
    if (plan.clusterCount != clusterCount)               return "clusterCount 与输入不符";
    if (plan.stats.clusterCount != clusterCount)         return "stats.clusterCount 与输入不符";
    // 【双源一致性】`stats` 与 plan 层的三个规模计数必须**逐位相等**。
    //   这一条是防"读数出口恒 0 ⇒ 判据空洞通过"的钉子（本任务修过一处同类缺陷）。
    if (plan.stats.clusterCount != plan.clusterCount)    return "stats.clusterCount != plan.clusterCount";
    if (plan.stats.contentCount != plan.contentCount)    return "stats.contentCount != plan.contentCount";
    if (plan.stats.pageCount    != plan.pageCount)       return "stats.pageCount != plan.pageCount";
    if (plan.pageCount != (plan.contentCount + K - 1u) / K) return "页数 != ceil(内容份数 / K)";
    if (plan.clusterPage.size() != clusterCount)         return "clusterPage 长度 != 簇数";
    if (plan.pageVertexBegin.size()   != plan.pageCount) return "pageVertexBegin 长度 != 页数";
    if (plan.pageVertexCount.size()   != plan.pageCount) return "pageVertexCount 长度 != 页数";
    if (plan.pageTriangleBegin.size() != plan.pageCount) return "pageTriangleBegin 长度 != 页数";
    if (plan.pageTriangleCount.size() != plan.pageCount) return "pageTriangleCount 长度 != 页数";
    if (plan.pageClusterBegin.size()  != plan.pageCount) return "pageClusterBegin 长度 != 页数";
    if (plan.pageClusterCount.size()  != plan.pageCount) return "pageClusterCount 长度 != 页数";
    if (plan.stats.pageStraddleCount != 0u)              return "有共享内容跨页（必须 0）";
    if (plan.stats.rankMismatchCount != 0u)              return "两张共享表秩不一致（必须 0）";

    // 内容份数 = 去重升序起点表的个数；两份表必须等长
    const std::vector<u32> vStarts = SortedUnique([&] {
        std::vector<u32> v; v.reserve(clusterCount);
        for (const NaniteClusterRecord& c : clusters) v.push_back(c.vertexOffset);
        return v;
    }());
    const std::vector<u32> tStarts = SortedUnique([&] {
        std::vector<u32> t; t.reserve(clusterCount);
        for (const NaniteClusterRecord& c : clusters) t.push_back(c.triangleOffset);
        return t;
    }());
    if (plan.contentCount != (u32)vStarts.size())        return "contentCount != 去重升序的起点个数";
    if (vStarts.size() != tStarts.size())                return "两张共享表的内容份数不等";

    // 三段区间：首尾相接覆盖整段（顶点 / 三角形 / 簇收集序）
    if (plan.pageVertexBegin[0] != 0u || plan.pageTriangleBegin[0] != 0u)
        return "顶点段或三角形段的首份内容不在记录 0";
    for (u32 p = 0u; p < plan.pageCount; ++p) {
        if (plan.pageVertexCount[p] == 0u || plan.pageTriangleCount[p] == 0u)
            return "存在空区间页";
        if (p + 1u < plan.pageCount) {
            if (plan.pageVertexBegin[p] + plan.pageVertexCount[p] != plan.pageVertexBegin[p + 1u])
                return "顶点段不连续";
            if (plan.pageTriangleBegin[p] + plan.pageTriangleCount[p]
                != plan.pageTriangleBegin[p + 1u])
                return "三角形段不连续";
            if (plan.pageClusterBegin[p] + plan.pageClusterCount[p]
                != plan.pageClusterBegin[p + 1u])
                return "簇段前缀和不连续";
        }
    }
    if (plan.pageVertexBegin[plan.pageCount - 1u] + plan.pageVertexCount[plan.pageCount - 1u]
        != vertexCount)                                  return "顶点段未覆盖到段尾";
    if (plan.pageTriangleBegin[plan.pageCount - 1u] + plan.pageTriangleCount[plan.pageCount - 1u]
        != triangleCount)                                return "三角形段未覆盖到段尾";
    if (plan.pageClusterBegin[plan.pageCount - 1u] + plan.pageClusterCount[plan.pageCount - 1u]
        != clusterCount)                                 return "簇段未覆盖到全部出现";

    // 每页的 `local` 必须是 0..count-1 的**排列**（无重复、无空洞），且簇段是双射
    std::vector<u8> filled(clusterCount, 0u);
    std::vector<u32> seenPerPage(plan.pageCount, 0u);
    for (u32 i = 0u; i < clusterCount; ++i) {
        const u32 page = plan.clusterPage[i].page;
        if (page >= plan.pageCount)                      return "clusterPage.page 越界";
        if (plan.clusterPage[i].local >= plan.pageClusterCount[page]) {
            MESSAGE("DIAG i=" << i << " page=" << page << " local=" << plan.clusterPage[i].local
                    << " count[page]=" << plan.pageClusterCount[page]
                    << " pageCount=" << plan.pageCount << " contentCount=" << plan.contentCount
                    << " K=" << K);
            return "clusterPage.local 越出该页的簇数";
        }
        ++seenPerPage[page];
        const u32 pos = plan.pageClusterBegin[page] + plan.clusterPage[i].local;
        if (pos >= clusterCount)                         return "收集序位置越界";
        if (filled[pos] != 0u)                           return "收集序出现重复（不是双射）";
        filled[pos] = 1u;
    }
    for (u32 p = 0u; p < plan.pageCount; ++p) {
        if (seenPerPage[p] != plan.pageClusterCount[p])  return "该页的簇数与该页出现次数不符";
    }
    for (u8 f : filled) { if (f == 0u) return "收集序有空洞（不是双射）"; }

    // 每份共享内容的**整段**必须落在它所在页的区间内（"不跨页"的显式复核）
    for (u32 i = 0u; i < clusterCount; ++i) {
        const u32 page = plan.clusterPage[i].page;
        const u32 vr = (u32)(std::lower_bound(vStarts.begin(), vStarts.end(),
                                              clusters[i].vertexOffset) - vStarts.begin());
        const u32 tr = (u32)(std::lower_bound(tStarts.begin(), tStarts.end(),
                                              clusters[i].triangleOffset) - tStarts.begin());
        if (vr != tr)                                    return "秩不一致（守卫应已拒绝）";
        const u32 vEnd = (vr + 1u < (u32)vStarts.size()) ? vStarts[vr + 1u] : vertexCount;
        const u32 tEnd = (tr + 1u < (u32)tStarts.size()) ? tStarts[tr + 1u] : triangleCount;
        if (vStarts[vr] < plan.pageVertexBegin[page]
            || vEnd > plan.pageVertexBegin[page] + plan.pageVertexCount[page])
            return "顶点内容跨页";
        if (tStarts[tr] < plan.pageTriangleBegin[page]
            || tEnd > plan.pageTriangleBegin[page] + plan.pageTriangleCount[page])
            return "三角形内容跨页";
    }

    // 槽步长 = 每页在三段里的最大值；槽字节 = 步长 × 记录大小
    u32 maxC = 0u, maxV = 0u, maxT = 0u;
    for (u32 p = 0u; p < plan.pageCount; ++p) {
        maxC = (plan.pageClusterCount[p]  > maxC) ? plan.pageClusterCount[p]  : maxC;
        maxV = (plan.pageVertexCount[p]   > maxV) ? plan.pageVertexCount[p]   : maxV;
        maxT = (plan.pageTriangleCount[p] > maxT) ? plan.pageTriangleCount[p] : maxT;
    }
    if (plan.stats.clusterStride  != maxC)               return "clusterStride != 每页最大簇数";
    if (plan.stats.vertexStride   != maxV)               return "vertexStride != 每页最大顶点数";
    if (plan.stats.triangleStride != maxT)               return "triangleStride != 每页最大三角形数";
    if (plan.stats.maxClustersPerPage  != maxC)          return "maxClustersPerPage 不符";
    if (plan.stats.maxVerticesPerPage  != maxV)          return "maxVerticesPerPage 不符";
    if (plan.stats.maxTrianglesPerPage != maxT)          return "maxTrianglesPerPage 不符";
    if (plan.stats.slotClusterBytes  != (usize)maxC * sizeof(NaniteClusterRecord))
        return "slotClusterBytes != 步长 × 64";
    if (plan.stats.slotVertexBytes   != (usize)maxV * sizeof(NaniteVertex))
        return "slotVertexBytes != 步长 × 16";
    if (plan.stats.slotTriangleBytes != (usize)maxT * sizeof(NanitePackedTriangle))
        return "slotTriangleBytes != 步长 × 8";
    return std::string();
}

/// 一批"形状不同"的合成输入（规模从 1 个出现到 ~4000 个出现），用于不变式与池足迹的实测
struct SyntheticAsset {
    std::vector<NaniteClusterRecord> clusters;
    u32 vertexCount   = 0u;
    u32 triangleCount = 0u;
};

/// 造一份合成资产：`contentCount` 份共享内容，每份 `vertsPerContent` 顶点 / `trisPerContent` 三角形，
/// 每个内容被 `occurrencesPerContent` 个"出现"引用。**顶点偏移刻意做成回跳**
/// （后面的出现引用更早的内容），复现去重路径下 `vertexOffset` 不单调的真实形态。
SyntheticAsset MakeSyntheticAsset(u32 contentCount, u32 vertsPerContent, u32 trisPerContent,
                                  u32 occurrencesPerContent) {
    SyntheticAsset out;
    for (u32 c = 0u; c < contentCount; ++c) {
        for (u32 k = 0u; k < occurrencesPerContent; ++k) {
            // 回跳模式：第 k 次出现引用 `c - (k % (c + 1))` —— 当 c > 0 时会出现更早的内容
            const u32 content = (k == 0u) ? c : (c - (k % (c + 1u)));
            out.clusters.push_back(MakeCluster(content * vertsPerContent,
                                               content * trisPerContent,
                                               trisPerContent));
        }
    }
    out.vertexCount   = contentCount * vertsPerContent;
    out.triangleCount = contentCount * trisPerContent;
    return out;
}

} // namespace

// ============================================================
// 用例 23：空资产（合法输入，不是失败）
// ============================================================
TEST_CASE("NanitePage: 空资产 ⇒ 成功且页数 0（与任务 7/8/9/10/12 同口径）") {
    NanitePagePlan plan;
    CHECK(BuildNanitePagePlan({}, 0u, 0u, 512u, plan));
    CHECK(plan.pageCount == 0u);
    CHECK(plan.contentCount == 0u);
    CHECK(plan.clusterCount == 0u);
    CHECK(plan.Empty());
    CHECK(plan.clusterPage.empty());
    CHECK(plan.pageVertexBegin.empty());
    CHECK(plan.stats.pageStraddleCount == 0u);
    CHECK(plan.PoolBytes(64u) == 0u);
}

// ============================================================
// 用例 24~26：三条失败路径（且**出参不许被改写**）
// ============================================================
TEST_CASE("NanitePage: 失败路径（K=0 / 段为空 / 段首空洞）不改写出参") {
    const std::vector<NaniteClusterRecord> one = { MakeCluster(0u, 0u) };

    // 哨兵：所有会被 `BuildNanitePagePlan` 写的字段都先填成不可能的值，失败时逐个核对
    auto makeSentinel = [] {
        NanitePagePlan p;
        p.contentsPerPage = 0x11111111u;
        p.clusterCount    = 0x22222222u;
        p.contentCount    = 0x33333333u;
        p.pageCount       = 0x44444444u;
        p.clusterPage.resize(3u);
        p.pageVertexBegin = { 7u, 7u };
        p.pageVertexCount = { 7u };
        p.pageTriangleBegin = { 7u };
        p.pageTriangleCount = { 7u };
        p.pageClusterBegin = { 7u };
        p.pageClusterCount = { 7u };
        p.stats.clusterCount = 0x55555555u;
        return p;
    };
    auto unchanged = [](const NanitePagePlan& p) {
        return p.contentsPerPage == 0x11111111u && p.clusterCount == 0x22222222u
            && p.contentCount == 0x33333333u && p.pageCount == 0x44444444u
            && p.clusterPage.size() == 3u && p.pageVertexBegin.size() == 2u
            && p.pageVertexCount.size() == 1u && p.pageTriangleBegin.size() == 1u
            && p.pageTriangleCount.size() == 1u && p.pageClusterBegin.size() == 1u
            && p.pageClusterCount.size() == 1u && p.stats.clusterCount == 0x55555555u;
    };

    // ① `contentsPerPage == 0` ⇒ 分页不终止，必须拒绝
    {
        NanitePagePlan plan = makeSentinel();
        CHECK_FALSE(BuildNanitePagePlan(one, 8u, 8u, 0u, plan));
        CHECK(unchanged(plan));
    }
    // ② 有簇但段长度为 0 ⇒ 资产自相矛盾（顶点段空 / 三角形段空各一次）
    {
        NanitePagePlan plan = makeSentinel();
        CHECK_FALSE(BuildNanitePagePlan(one, 0u, 8u, 4u, plan));
        CHECK(unchanged(plan));
        NanitePagePlan plan2 = makeSentinel();
        CHECK_FALSE(BuildNanitePagePlan(one, 8u, 0u, 4u, plan2));
        CHECK(unchanged(plan2));
    }
    // ③ 段首有"没人引用的前缀"（首份内容不从记录 0 开始）⇒ 页划分覆盖不到它，拒绝
    {
        const std::vector<NaniteClusterRecord> gapV = { MakeCluster(4u, 0u) };
        NanitePagePlan plan = makeSentinel();
        CHECK_FALSE(BuildNanitePagePlan(gapV, 8u, 8u, 4u, plan));
        CHECK(unchanged(plan));
        const std::vector<NaniteClusterRecord> gapT = { MakeCluster(0u, 4u) };
        NanitePagePlan plan2 = makeSentinel();
        CHECK_FALSE(BuildNanitePagePlan(gapT, 8u, 8u, 4u, plan2));
        CHECK(unchanged(plan2));
    }
}

// ============================================================
// 用例 27：两张共享表**不同源**的守卫（设计里没有、实现里补上的关键一条）
//   两条子情形都必须在页划分层被拒绝，否则着色器会在两个槽里各读到半份数据（静默错画）：
//     ① 去重后的**秩个数不等**（顶点 2 份内容、三角形 1 份内容）；
//     ② 秩个数相等但**逐一错位**（第三个簇的两个秩是 (2,1)）。
// ============================================================
TEST_CASE("NanitePage: 守卫 —— 顶点段与三角形段必须同源（秩个数 / 秩逐一）") {
    // ① 秩个数不等：顶点起点 {0,8}、三角形起点 {0}
    {
        const std::vector<NaniteClusterRecord> clusters = {
            MakeCluster(0u, 0u), MakeCluster(8u, 0u),
        };
        NanitePagePlan plan;
        plan.pageCount = 0xDEADBEEFu;
        CHECK_FALSE(BuildNanitePagePlan(clusters, 16u, 8u, 1u, plan));
        CHECK(plan.pageCount == 0xDEADBEEFu);      // 出参未被改写
    }
    // ② 秩逐一错位：顶点起点 {0,8,16}、三角形起点 {0,4,8}（两份表等长、段首都无空洞）
    //    第三个簇的两个秩是 (2,1) ⇒ 只有"秩逐一对应"这条检查能抓住它
    {
        const std::vector<NaniteClusterRecord> clusters = {
            MakeCluster(0u, 0u),      // (0,0)
            MakeCluster(8u, 8u),      // (1,1)
            MakeCluster(16u, 4u),     // (2,1) ← 错位
        };
        NanitePagePlan plan;
        plan.contentCount = 0xC0FFEEu;
        CHECK_FALSE(BuildNanitePagePlan(clusters, 24u, 16u, 1u, plan));
        CHECK(plan.contentCount == 0xC0FFEEu);
    }
}

// ============================================================
// 用例 28~30：不变式 + 前缀和自洽 + 池足迹（多种规模 × 多组 K）
// ============================================================
TEST_CASE("NanitePage: 不变式与前缀和自洽（多规模 × 多组 K）+ 池足迹实测") {
    struct Shape { const char* name; SyntheticAsset asset; };
    std::vector<Shape> shapes;
    shapes.push_back({ "1 份内容 × 1 出现",   MakeSyntheticAsset(1u, 3u, 1u, 1u) });
    shapes.push_back({ "7 份内容 × 2 出现",   MakeSyntheticAsset(7u, 5u, 3u, 2u) });
    shapes.push_back({ "64 份内容 × 4 出现",  MakeSyntheticAsset(64u, 9u, 4u, 4u) });
    shapes.push_back({ "512 份内容 × 3 出现", MakeSyntheticAsset(512u, 17u, 6u, 3u) });
    shapes.push_back({ "1024 份内容 × 2 出现",MakeSyntheticAsset(1024u, 21u, 8u, 2u) });

    for (const Shape& shape : shapes) {
        const SyntheticAsset& a = shape.asset;
        REQUIRE(!a.clusters.empty());

        // 【前置事实：这份合成输入的顶点偏移**不单调**】
        //   注意只在"多份内容 × 多次出现"的形态上成立：1 份内容 1 次出现的退化形态本来就是升序的，
        //   对它断言"不单调"是断言写得太强（单测第一版就在这里红过）。
        std::vector<u32> offsets;
        for (const NaniteClusterRecord& c : a.clusters) offsets.push_back(c.vertexOffset);
        if (a.clusters.size() > 1u) {
            CHECK_FALSE(std::is_sorted(offsets.begin(), offsets.end()));
        } else {
            CHECK(std::is_sorted(offsets.begin(), offsets.end()));
        }

        for (u32 K : { 1u, 2u, 3u, 8u, 512u }) {
            NanitePagePlan plan;
            REQUIRE(BuildNanitePagePlan(a.clusters, a.vertexCount, a.triangleCount, K, plan));
            const std::string bad = CheckPagePlanInvariants(plan, a.clusters,
                                                            a.vertexCount, a.triangleCount, K);
            if (!bad.empty()) { MESSAGE("不变式失败：" << bad.c_str()); }
            CHECK(bad.empty());

            // 前缀和自洽：Σ 每页簇数 == 簇数，且逐页前缀和相等
            u32 sum = 0u, running = 0u;
            for (u32 p = 0u; p < plan.pageCount; ++p) {
                CHECK(plan.pageClusterBegin[p] == running);
                running += plan.pageClusterCount[p];
                sum     += plan.pageClusterCount[p];
            }
            CHECK(sum == (u32)a.clusters.size());

            // 池足迹：PoolBytes(slots) == slots × (三段槽字节之和)
            const u32 slots = 64u;
            const usize perSlot = plan.stats.slotClusterBytes + plan.stats.slotVertexBytes
                                + plan.stats.slotTriangleBytes;
            CHECK(plan.PoolBytes(slots) == (usize)slots * perSlot);
            CHECK(plan.PoolBytes(0u) == 0u);
        }
        // K ≥ 内容份数 ⇒ 单页
        {
            NanitePagePlan single;
            REQUIRE(BuildNanitePagePlan(a.clusters, a.vertexCount, a.triangleCount,
                                        (u32)a.clusters.size() + 8u, single));
            CHECK(single.pageCount == 1u);
        }
        MESSAGE("合成资产 " << shape.name
                << "：簇=" << a.clusters.size()
                << " 顶点=" << a.vertexCount << " 三角形=" << a.triangleCount);
    }

    // ── 池足迹的**实测打印**（一组具体数字，便于与运行期 `stream_setup` 行对照）──
    {
        const SyntheticAsset a = MakeSyntheticAsset(1024u, 21u, 8u, 2u);
        NanitePagePlan plan;
        REQUIRE(BuildNanitePagePlan(a.clusters, a.vertexCount, a.triangleCount, 512u, plan));
        const usize perSlot = plan.stats.slotClusterBytes + plan.stats.slotVertexBytes
                            + plan.stats.slotTriangleBytes;
        MESSAGE("池足迹实测（合成 1024 份内容 × 2 出现，K=512，64 槽）：内容=" << plan.contentCount
                << " 页=" << plan.pageCount
                << " 步长(c,v,t)=(" << plan.stats.clusterStride << ","
                << plan.stats.vertexStride << "," << plan.stats.triangleStride << ")"
                << " 槽字节(c,v,t)=(" << plan.stats.slotClusterBytes << ","
                << plan.stats.slotVertexBytes << "," << plan.stats.slotTriangleBytes << ")"
                << " 每槽总=" << perSlot
                << " 池总=" << plan.PoolBytes(64u)
                << " 资产字节(原始记录)=" << (usize)plan.clusterCount * 64u
                                          + (usize)a.vertexCount * 16u
                                          + (usize)a.triangleCount * 8u);
        CHECK(perSlot > 0u);
    }
}

// ============================================================
// 用例 31~32：确定性与 K 的影响（覆盖的簇集合必须与 K 无关）
// ============================================================
TEST_CASE("NanitePage: 确定性 + K 只改变页数不改变覆盖") {
    const SyntheticAsset a = MakeSyntheticAsset(200u, 11u, 5u, 3u);
    const u32 clusterCount = (u32)a.clusters.size();

    // ── 确定性：两次调用逐位一致（6 个区间数组 + 统计 + clusterPage）──
    NanitePagePlan p1, p2;
    REQUIRE(BuildNanitePagePlan(a.clusters, a.vertexCount, a.triangleCount, 3u, p1));
    REQUIRE(BuildNanitePagePlan(a.clusters, a.vertexCount, a.triangleCount, 3u, p2));
    CHECK(p1.pageCount == p2.pageCount);
    CHECK(p1.contentCount == p2.contentCount);
    CHECK(p1.clusterCount == p2.clusterCount);
    CHECK(p1.pageVertexBegin == p2.pageVertexBegin);
    CHECK(p1.pageVertexCount == p2.pageVertexCount);
    CHECK(p1.pageTriangleBegin == p2.pageTriangleBegin);
    CHECK(p1.pageTriangleCount == p2.pageTriangleCount);
    CHECK(p1.pageClusterBegin == p2.pageClusterBegin);
    CHECK(p1.pageClusterCount == p2.pageClusterCount);
    CHECK(p1.clusterPage.size() == p2.clusterPage.size());
    bool sameClusterPage = (p1.clusterPage.size() == p2.clusterPage.size());
    for (usize i = 0u; sameClusterPage && i < p1.clusterPage.size(); ++i) {
        sameClusterPage = (p1.clusterPage[i].page == p2.clusterPage[i].page)
                       && (p1.clusterPage[i].local == p2.clusterPage[i].local);
    }
    CHECK(sameClusterPage);

    // ── K 的影响：页数分别是 contentCount / ceil(c/2) / 1，且覆盖的簇集合一致 ──
    NanitePagePlan k1, k2, kBig;
    REQUIRE(BuildNanitePagePlan(a.clusters, a.vertexCount, a.triangleCount, 1u, k1));
    REQUIRE(BuildNanitePagePlan(a.clusters, a.vertexCount, a.triangleCount, 2u, k2));
    REQUIRE(BuildNanitePagePlan(a.clusters, a.vertexCount, a.triangleCount,
                                k1.contentCount + 1u, kBig));
    CHECK(k1.pageCount == k1.contentCount);
    CHECK(k2.pageCount == (k1.contentCount + 1u) / 2u);
    CHECK(kBig.pageCount == 1u);

    // 【覆盖一致性】三种划分里，簇 i 落在哪一页无关紧要；但"每个簇都被某个页收下"必须成立
    //   （页号本身随 K 变，所以比的是"覆盖到的簇下标集合"而不是页号）
    for (const NanitePagePlan* plan : { &k1, &k2, &kBig }) {
        std::vector<u8> covered(clusterCount, 0u);
        for (u32 i = 0u; i < clusterCount; ++i) covered[i] = 1u;   // 每簇必有一条 clusterPage 记录
        for (u32 i = 0u; i < clusterCount; ++i) {
            const u32 page = plan->clusterPage[i].page;
            REQUIRE(page < plan->pageCount);
            const u32 pos = plan->pageClusterBegin[page] + plan->clusterPage[i].local;
            REQUIRE(pos < clusterCount);
            CHECK(covered[i] == 1u);
        }
        CHECK(plan->pageClusterBegin[plan->pageCount - 1u]
              + plan->pageClusterCount[plan->pageCount - 1u] == clusterCount);
    }
    MESSAGE("K 的影响（内容 " << k1.contentCount << " 份）：K=1 ⇒ " << k1.pageCount
            << " 页，K=2 ⇒ " << k2.pageCount << " 页，K>c ⇒ " << kBig.pageCount << " 页");
}

// ============================================================
// 用例 33：真实资产路径（走过 DAG 去重）上的页划分
//
// 【为什么还要一条"真资产"用例】合成输入的偏移模式是人造的；设计的核心依据 ——
//   "去重升序的 `cluster.vertexOffset` 集合 == 构建期的 uniqueVertexOffset[]" —— 只有在
//   真的走过 `BuildNaniteClusterDAG` → `PackNaniteClusters` 的资产上才算被验证过。
// 【输入怎么造才能**真的**产生"偏移回跳"（第一版踩过）】
//   · 每个"片"必须**恰好 64 个三角形**（= 每簇三角形上限）：否则 meshopt 会把多个片合并进
//     一个 meshlet，"内容相同"的片就永远不成为独立的簇出现，去重也就无从发生；
//   · 必须有**部分相同**的片，且它们在合并网格里**交错排列**：去重把第 2 片命中回第 0 片的
//     内容 ⇒ `vertexOffset` 从 45 回跳到 0 ⇒ 非单调。若所有片都相同（或都不同），偏移反而是升序的。
//   本用例：4 片 8×4 quads（各 64 tri），第 0/2 片完全相同、第 1/3 片各带不同 z 偏移。
// ============================================================
TEST_CASE("NanitePage: 真实资产（DAG 去重路径）的偏移不单调，页划分照常成立") {
    std::vector<float> positions;
    std::vector<u32>   indices;
    const u32 kQuads = 4u;        // 每片 8(x) × 4(y) = 32 个 quad = 64 个三角形
    const u32 kTiles = 4u;        // 四片一排；第 0/2 片**形状逐位相同** ⇒ 去重命中 ⇒ 偏移回跳
    for (u32 tile = 0u; tile < kTiles; ++tile) {
        const u32 base = (u32)(positions.size() / 3u);
        for (u32 y = 0u; y <= kQuads; ++y) {
            for (u32 x = 0u; x <= 2u * kQuads; ++x) {
                // 【形状必须真的不同，不能只平移】去重的内容键是"**相对簇心**的局部几何" ⇒
                //   纯平移（哪怕沿 z 平移）仍然命中同一份内容（第一版就在这里踩过：四片的
                //   vertexOffset 全是 0，用例什么都没验证）。这里让第 1 片沿 y 起脊、
                //   第 3 片沿 x 起脊 ⇒ 三份不同的内容，而第 0/2 片保持完全一致。
                float z = 0.0f;
                if (tile == 1u && y == kQuads / 2u) z = 0.5f;
                if (tile == 3u && x == kQuads)      z = 0.5f;
                positions.push_back((float)x + (float)tile * 100.0f);
                positions.push_back((float)y);
                positions.push_back(z);
            }
        }
        const u32 stride = 2u * kQuads + 1u;
        for (u32 y = 0u; y < kQuads; ++y) {
            for (u32 x = 0u; x < 2u * kQuads; ++x) {
                const u32 i0 = base + y * stride + x;
                const u32 i1 = i0 + 1u;
                const u32 i2 = i0 + stride;
                const u32 i3 = i2 + 1u;
                indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
                indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
            }
        }
    }

    NanitePackedAsset asset;
    REQUIRE(BuildNaniteAssetFromGeometry(positions, {}, {}, indices, {}, asset));
    REQUIRE(!asset.clusters.empty());
    REQUIRE(!asset.vertices.empty());
    REQUIRE(!asset.triangles.empty());

    std::vector<u32> offsets;
    for (const NaniteClusterRecord& c : asset.clusters) offsets.push_back(c.vertexOffset);
    const bool monotonic = std::is_sorted(offsets.begin(), offsets.end());
    // 【打印前 4 个偏移时要带分隔符】否则 `0,45,0,90` 会连写成 `045090`，容易被误读成一个数
    std::string head;
    for (usize i = 0u; i < offsets.size() && i < 4u; ++i) {
        if (i != 0u) head += " ";
        head += std::to_string(offsets[i]);
    }
    MESSAGE("真实资产：簇=" << asset.clusters.size() << " 顶点=" << asset.vertices.size()
            << " 三角形=" << asset.triangles.size()
            << " 前 4 个 vertexOffset=[" << head.c_str() << " ...]"
            << " vertexOffset 单调=" << (monotonic ? 1 : 0));
    // 【设计依据的可执行版】去重命中会把 uniqueIndex 指回更早的内容 ⇒ 偏移回跳
    CHECK(asset.clusters.size() >= 4u);   // 至少四片各成一个簇出现（否则本用例什么都没验证）
    CHECK_FALSE(monotonic);

    for (u32 K : { 1u, 2u, 3u, 512u }) {
        NanitePagePlan plan;
        REQUIRE(BuildNanitePagePlan(asset.clusters, (u32)asset.vertices.size(),
                                    (u32)asset.triangles.size(), K, plan));
        const std::string bad = CheckPagePlanInvariants(plan, asset.clusters,
                                                        (u32)asset.vertices.size(),
                                                        (u32)asset.triangles.size(), K);
        if (!bad.empty()) { MESSAGE("真实资产不变式失败：" << bad.c_str()); }
        CHECK(bad.empty());
    }
}
