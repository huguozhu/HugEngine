// ============================================================
// TestMeshRegistry.cpp — 阶段 1（附录 E / E-1）：mesh 注册表
//
// 【为什么要有它】注册表的失效方式全是"索引语义"问题：注销后旧索引还解析出条目（指向错资源）、
// 索引复用导致旧索引指向新网格、越界索引不返回空 —— 这些都不会崩溃，只会让画面莫名错乱。
// 这里把语义钉死，并把"索引复用会让旧索指引到新条目"这条**已知限制**显式写成用例
// （它是"将来换成带代次的 `RHIBufferHandle`"的理由，不是被忽略的 bug）。
//
// 本测试只用到裸指针值（不解引用），因此不需要链接 HugEngineRHI。
// ============================================================
#include "Threading/MeshRegistry.h"

#include <doctest/doctest.h>

using namespace he;
using namespace he::render;

namespace {
/// 造一个非空但不解引用的"缓冲指针"（注册表只借不拥有）
rhi::IRHIBuffer* FakeBuffer(usize addr) {
    return reinterpret_cast<rhi::IRHIBuffer*>(addr);
}
} // namespace

TEST_CASE("MeshRegistry：注册返回从 1 起的索引，Find 能取回条目") {
    MeshRegistry reg;
    int keyA = 0, keyB = 0;

    MeshRegistryEntry a;
    a.vertexBuffer = FakeBuffer(0x1000);
    a.indexBuffer  = FakeBuffer(0x2000);
    a.indexCount   = 36;
    a.materialID   = 12;
    const u32 ia = reg.Register(&keyA, a);

    MeshRegistryEntry b;
    b.indexCount = 6;
    const u32 ib = reg.Register(&keyB, b);

    CHECK(ia == 1u);                       // 0 是哨兵
    CHECK(ib == 2u);
    CHECK(reg.Count() == 2u);

    const auto* ra = reg.Find(ia);
    REQUIRE(ra != nullptr);
    CHECK(ra->vertexBuffer == a.vertexBuffer);
    CHECK(ra->indexCount == 36u);
    CHECK(ra->materialID == 12u);
    const auto* rb = reg.Find(ib);
    REQUIRE(rb != nullptr);
    CHECK(rb->indexCount == 6u);

    CHECK(reg.Find(0u) == nullptr);        // 哨兵永不解析
    CHECK(reg.Find(99u) == nullptr);       // 越界
}

TEST_CASE("MeshRegistry：同 key 重复注册 = 更新，索引不变") {
    MeshRegistry reg;
    int key = 0;
    MeshRegistryEntry e;
    e.indexCount = 3;
    const u32 first = reg.Register(&key, e);

    e.indexCount = 9;
    e.materialID = 5;
    const u32 second = reg.Register(&key, e);

    CHECK(second == first);                // 更新不换索引（调用方缓存的 meshIndex 继续有效）
    CHECK(reg.Count() == 1u);
    const auto* r = reg.Find(first);
    REQUIRE(r != nullptr);
    CHECK(r->indexCount == 9u);
    CHECK(r->materialID == 5u);
}

TEST_CASE("MeshRegistry：注销后旧索引解析为空，且索引可被复用") {
    MeshRegistry reg;
    int keyA = 0, keyB = 0;
    MeshRegistryEntry e;
    e.indexCount = 1;

    const u32 ia = reg.Register(&keyA, e);
    reg.Unregister(&keyA);
    CHECK(reg.Find(ia) == nullptr);        // 已注销 ⇒ 空（而不是指到空记录里的垃圾）
    CHECK(reg.Count() == 0u);
    reg.Unregister(&keyA);                 // 幂等
    CHECK(reg.Count() == 0u);

    const u32 ib = reg.Register(&keyB, e);
    CHECK(ib == ia);                       // 复用刚释放的索引
    CHECK(reg.Find(ib) != nullptr);
    // 【已知限制】复用后，旧索引值会指到**新条目** —— 这正是将来换 `RHIBufferHandle`（带代次）的理由
    CHECK(reg.Find(ia) != nullptr);
}

TEST_CASE("MeshRegistry：Clear 复位（保留哨兵）") {
    MeshRegistry reg;
    int key = 0;
    MeshRegistryEntry e;
    reg.Register(&key, e);
    reg.Register(&key, e);
    REQUIRE(reg.Count() == 1u);

    reg.Clear();
    CHECK(reg.Count() == 0u);
    CHECK(reg.Find(1u) == nullptr);
    const u32 again = reg.Register(&key, e);   // 清空后重新注册仍从 1 开始
    CHECK(again == 1u);
}
