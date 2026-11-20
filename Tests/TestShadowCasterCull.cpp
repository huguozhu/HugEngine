// ============================================================
// TestShadowCasterCull.cpp — T10：阴影投射者逐级联剔除的单测
//
// 【为什么先钉这个】T10 的验收是"阴影与 IA 版本截图对比在阈值内"，但截图对比
//   需要一个能跑出画面的环境。剔除是纯几何判定，可以先用单测把口径固定下来：
//   只要"哪些物体进哪个级联"完全一致，后面换 MS 变体时视觉差异就只可能来自光栅化，
//   不会来自"漏画/多画物体"。
// ============================================================
#include "doctest.h"

#include "Shadow/ShadowCasterCull.h"

using namespace he;
using namespace he::render;

namespace {

// 盒子视锥：半边长 half（与 Nanite 侧单测同一种构造，口径一致）
NaniteFrustumPlanes MakeBoxFrustum(float half) {
    NaniteFrustumPlanes f{};
    for (int axis = 0; axis < 3; ++axis) {
        for (int sign = 0; sign < 2; ++sign) {
            const float s = (sign == 0) ? 1.0f : -1.0f;
            float* p = f.planes[axis * 2 + sign];
            p[0] = (axis == 0) ? s : 0.0f;
            p[1] = (axis == 1) ? s : 0.0f;
            p[2] = (axis == 2) ? s : 0.0f;
            p[3] = half;
        }
    }
    return f;
}

ShadowCasterBounds MakeBox(float cx, float cy, float cz, float half) {
    ShadowCasterBounds b{};
    b.min[0] = cx - half; b.min[1] = cy - half; b.min[2] = cz - half;
    b.max[0] = cx + half; b.max[1] = cy + half; b.max[2] = cz + half;
    return b;
}

}  // namespace

TEST_CASE("ShadowCasterCull(T10): 盒内/盒外/跨边界三种情形") {
    const NaniteFrustumPlanes frustum = MakeBoxFrustum(10.0f);

    CHECK(ShadowAabbVisibleInFrustum(frustum, MakeBox(0.0f, 0.0f, 0.0f, 1.0f)));      // 完全在里
    CHECK_FALSE(ShadowAabbVisibleInFrustum(frustum, MakeBox(30.0f, 0.0f, 0.0f, 1.0f))); // 完全在外
    // 跨边界：中心 10.5、半径 1 ⇒ 与 x=10 平面相交 ⇒ 必须判可见（保守要求）
    CHECK(ShadowAabbVisibleInFrustum(frustum, MakeBox(10.5f, 0.0f, 0.0f, 1.0f)));
    // 刚好贴住边界外侧：中心 11.01、半径 1 ⇒ 最小 x = 10.01 > 10 ⇒ 不可见
    CHECK_FALSE(ShadowAabbVisibleInFrustum(frustum, MakeBox(11.01f, 0.0f, 0.0f, 1.0f)));
}

TEST_CASE("ShadowCasterCull(T10): 逐级联剔除的下标与截断语义") {
    const NaniteFrustumPlanes narrow = MakeBoxFrustum(5.0f);

    ShadowCasterBounds casters[4];
    casters[0] = MakeBox(0.0f, 0.0f, 0.0f, 1.0f);    // 可见
    casters[1] = MakeBox(100.0f, 0.0f, 0.0f, 1.0f);  // 不可见
    casters[2] = MakeBox(0.0f, 4.9f, 0.0f, 1.0f);    // 跨边界（4.9±1 与 y=5 相交）⇒ 可见
    casters[3] = MakeBox(0.0f, 0.0f, -100.0f, 1.0f); // 不可见

    u32 idx[8] = {};
    const u32 visible = CullShadowCasters(narrow, casters, 4u, idx, 8u);
    REQUIRE(visible == 2u);
    CHECK(idx[0] == 0u);
    CHECK(idx[1] == 2u);

    // 容量不足：返回值仍是"可见总数"，但只写前 cap 个
    u32 small[1] = { 0xFFFFFFFFu };
    const u32 visible2 = CullShadowCasters(narrow, casters, 4u, small, 1u);
    CHECK(visible2 == 2u);
    CHECK(small[0] == 0u);

    // 退化输入不崩
    CHECK(CullShadowCasters(narrow, nullptr, 4u, idx, 8u) == 0u);
    CHECK(CullShadowCasters(narrow, casters, 0u, idx, 8u) == 0u);
}
