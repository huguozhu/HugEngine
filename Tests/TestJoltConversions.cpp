// ============================================================
// Tests/TestJoltConversions.cpp — JoltConversions 转换层测试（T2）
//
// 目的：验证 glm（float3/quat）↔ Jolt（RVec3/Quat/Vec3）往返转换一致，
// 以及位置/四元数/矩阵关键分量（坐标/单位收口正确）。
// 不创建物理世界，仅验证转换纯函数。
// ============================================================

#include "doctest.h"

#include "Physics/Physics/JoltConversions.h"

using namespace he;
using namespace he::physics;

TEST_CASE("JoltConversions 位置往返一致") {
    float3 a(1.5f, -2.0f, 3.25f);
    auto j = ToJoltPos(a);
    auto back = ToGlmPos(j);
    CHECK(back.x == doctest::Approx(a.x));
    CHECK(back.y == doctest::Approx(a.y));
    CHECK(back.z == doctest::Approx(a.z));
}

TEST_CASE("JoltConversions 向量往返一致") {
    float3 a(0.1f, -0.2f, 12.5f);
    auto j = ToJolt(a);
    auto back = ToGlm(j);
    CHECK(back.x == doctest::Approx(a.x));
    CHECK(back.y == doctest::Approx(a.y));
    CHECK(back.z == doctest::Approx(a.z));
}

TEST_CASE("JoltConversions 四元数往返一致") {
    he::quat a;   // 直接设分量（避免 glm 构造序歧义）
    a.x = 0.1f; a.y = -0.3f; a.z = 0.5f; a.w = 0.8f;   // 不必归一化，仅验证往返
    auto j = ToJolt(a);
    auto back = ToGlm(j);
    CHECK(back.x == doctest::Approx(a.x));
    CHECK(back.y == doctest::Approx(a.y));
    CHECK(back.z == doctest::Approx(a.z));
    CHECK(back.w == doctest::Approx(a.w));
}
