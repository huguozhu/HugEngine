// ============================================================
// TestNaniteMultiAsset.cpp — T8：多资产剔除口径的 CPU 单测
//
// 【为什么需要这组用例】第 5 步 ③ 把"共享剔除缓冲 + 每资产基址表"接进了
//   `NaniteTraverseClusterBVHCPU`，第 4 步又把实例变换从"纯平移"扩到"完整 localToWorld
//   （旋转 + 非均匀缩放，半径按最大轴缩放）"。这两处都是纯数学，**不需要 GPU** 就能判定：
//   · 基址表若漏加/多加一次，第二个实例会读到第一个资产的簇（画面会缺块、很难从截图看出）；
//   · 非均匀缩放的半径若忘了乘 `maxScale`，缩小/放大的实例会被错误剔除。
//   两个用例各自钉住一条，CI 里直接跑。
// ============================================================
#include "doctest.h"

#include "Nanite/NaniteTypes.h"

#include <cstring>
#include <vector>

using namespace he;
using namespace he::render;

namespace {

// 造一个轴对齐的"盒子视锥"：半边长 half（6 个平面法线朝外、d = half）
// 【口径】`NaniteSphereVisibleInFrustum` 判据是 `dot(n,c) + d < -r ⇒ 不可见`，
//   所以 `dot(n,c) + d >= -r` 等价于"球还没整个跑到平面外侧"。
NaniteFrustumPlanes MakeBoxFrustum(float half) {
    NaniteFrustumPlanes f{};
    const float axis[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int i = 0; i < 3; ++i) {
        for (int sign = 0; sign < 2; ++sign) {
            const float s = (sign == 0) ? 1.0f : -1.0f;
            float* p = f.planes[i * 2 + sign];
            p[0] = axis[i][0] * s;
            p[1] = axis[i][1] * s;
            p[2] = axis[i][2] * s;
            p[3] = half;
        }
    }
    return f;
}

// 造一个"纯平移 + 各轴缩放"的列主序 localToWorld
void MakeTrs(float* out16, const float scale[3], const float translate[3]) {
    std::memset(out16, 0, sizeof(float) * 16);
    out16[0]  = scale[0];        // 第 1 列
    out16[5]  = scale[1];        // 第 2 列
    out16[10] = scale[2];        // 第 3 列
    out16[15] = 1.0f;
    out16[12] = translate[0];    // 平移列
    out16[13] = translate[1];
    out16[14] = translate[2];
}

// 一份"单叶单簇"的资产区间（节点/叶子/簇都只有 1 条）
NaniteAssetCullRangeGpu MakeRange(u32 base) {
    NaniteAssetCullRangeGpu r{};
    r.nodeBase      = base;
    r.nodeCount     = 1u;
    r.leafBase      = base;
    r.leafCount     = 1u;
    r.clusterBase   = base;
    r.clusterCount  = 1u;
    r.lodInfoBase   = base;
    r.drawRangeBase = base;
    return r;
}

}  // namespace

TEST_CASE("NaniteMultiAsset(合成): 两份资产各自按基址遍历，簇号不串味") {
    // 资产 0 的簇球在原点（视野内）；资产 1 的簇球在 x=100（视野外）
    NaniteBVHNode nodes[2]{};
    for (int i = 0; i < 2; ++i) {
        nodes[i].radius = 1.0f;
        nodes[i].left   = 0u;      // 叶子：叶子簇表首下标（资产局部）
        nodes[i].count  = 1u;
        nodes[i].flags  = 1u;      // bit0 = 叶子
    }
    nodes[1].center[0] = 100.0f;

    const u32 leafLocal[2] = { 0u, 0u };   // 两份资产的叶子簇表项都是"第 0 个局部簇"
    NaniteClusterSphere spheres[2]{};
    spheres[0].center[0] = 0.0f;   spheres[0].center[1] = 0.0f; spheres[0].center[2] = 0.0f;
    spheres[0].radius = 1.0f;
    spheres[1].center[0] = 100.0f; spheres[1].center[1] = 0.0f; spheres[1].center[2] = 0.0f;
    spheres[1].radius = 1.0f;

    NaniteClusterBVHView bvh{};
    bvh.nodes              = nodes;
    bvh.nodeCount          = 2u;
    bvh.leafClusterIndices = leafLocal;
    bvh.clusterSpheres     = spheres;
    bvh.clusterCount       = 2u;

    const NaniteAssetCullRangeGpu ranges[2] = { MakeRange(0u), MakeRange(1u) };

    // 两个实例：0 号用资产 0，1 号用资产 1（`meshIndex` 承载资产槽位）
    NaniteInstanceGpuObject instances[2]{};
    const float unit[3]  = { 1.0f, 1.0f, 1.0f };
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 2; ++i) {
        instances[i].indexCount = 3u;
        instances[i].meshIndex  = (u32)i;
        MakeTrs(instances[i].localToWorld, unit, origin);
    }

    const NaniteFrustumPlanes frustum = MakeBoxFrustum(10.0f);
    NaniteVisibleClusterRef refs[8]{};
    NaniteClusterBVHTraversalStats stats{};

    const u32 written = NaniteTraverseClusterBVHCPU(
        frustum, bvh, instances, 2u, 2u, refs, 8u, &stats, NaniteCullChainDesc{},
        ranges, 2u);

    // 资产 1 的簇球在视野外 ⇒ 只有资产 0 的簇可见
    REQUIRE(written == 1u);
    CHECK(refs[0].instance == 0u);
    CHECK(refs[0].cluster  == 0u);   // 全局簇号 = clusterBase(0) + 局部 0
    CHECK(stats.visitedNodes == 2u); // 两个实例各访问一个根节点

    // 反过来：把 1 号实例移到"资产 0 的位置"，它必须仍然读资产 1 的簇（基址 1）
    instances[1].localToWorld[12] = -100.0f;
    const u32 written2 = NaniteTraverseClusterBVHCPU(
        frustum, bvh, instances, 2u, 2u, refs, 8u, nullptr, NaniteCullChainDesc{},
        ranges, 2u);
    REQUIRE(written2 == 2u);
    // 可见引用里必须出现"资产 1 的全局簇号 1"，而不是两份都指向 0
    bool sawCluster0 = false, sawCluster1 = false;
    for (u32 i = 0; i < written2; ++i) {
        if (refs[i].cluster == 0u) sawCluster0 = true;
        if (refs[i].cluster == 1u) sawCluster1 = true;
    }
    CHECK(sawCluster0);
    CHECK(sawCluster1);
}

TEST_CASE("NaniteMultiAsset(合成): 非均匀缩放用最大轴放大半径（否则错误剔除）") {
    NaniteBVHNode node{};
    node.radius    = 1.0f;
    node.left      = 0u;
    node.count     = 1u;
    node.flags     = 1u;

    const u32 leafLocal[1] = { 0u };
    NaniteClusterSphere sphere{};
    sphere.center[0] = 0.0f; sphere.center[1] = 0.0f; sphere.center[2] = 0.0f;
    sphere.radius    = 1.0f;

    NaniteClusterBVHView bvh{};
    bvh.nodes              = &node;
    bvh.nodeCount          = 1u;
    bvh.leafClusterIndices = leafLocal;
    bvh.clusterSpheres     = &sphere;
    bvh.clusterCount       = 1u;

    const NaniteAssetCullRangeGpu range = MakeRange(0u);
    const NaniteFrustumPlanes frustum   = MakeBoxFrustum(1.5f);   // 半边长 1.5 的盒子

    NaniteInstanceGpuObject instance{};
    instance.indexCount = 3u;
    instance.meshIndex  = 0u;

    // ① 平移到 z=3（球心 z=3、半径 1 ⇒ 球完全在盒子 [-1.5,1.5] 外侧）⇒ 不可见
    {
        const float unit[3]      = { 1.0f, 1.0f, 1.0f };
        const float translate[3] = { 0.0f, 0.0f, 3.0f };
        MakeTrs(instance.localToWorld, unit, translate);
        NaniteVisibleClusterRef refs[4]{};
        const u32 written = NaniteTraverseClusterBVHCPU(
            frustum, bvh, &instance, 1u, 1u, refs, 4u, nullptr, NaniteCullChainDesc{},
            &range, 1u);
        CHECK(written == 0u);
    }

    // ② 同样的平移，但 x 轴放大 10 倍 ⇒ 半径 ×10 = 10，球覆盖到盒子内 ⇒ 可见
    //    【关键】若实现只用平移（半径不变），这里会错误返回 0。
    {
        const float scale[3]     = { 10.0f, 1.0f, 1.0f };
        const float translate[3] = { 0.0f, 0.0f, 3.0f };
        MakeTrs(instance.localToWorld, scale, translate);
        NaniteVisibleClusterRef refs[4]{};
        const u32 written = NaniteTraverseClusterBVHCPU(
            frustum, bvh, &instance, 1u, 1u, refs, 4u, nullptr, NaniteCullChainDesc{},
            &range, 1u);
        CHECK(written == 1u);
    }
}
