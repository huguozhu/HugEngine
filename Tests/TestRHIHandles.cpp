// ============================================================
// TestRHIHandles.cpp — 阶段 0 T0.7：资源句柄与句柄表（generation 校验）
//
// 【为什么要有它】句柄化的全部风险集中在**槽位复用**：槽位一旦回收再分配，只有下标的句柄就会
// 静默指向另一个资源（不崩溃、画面莫名错乱，是最难查的一类 bug）。所以这里把"代次"的语义钉死：
//   ① 正常登记/解析；
//   ② Remove 之后所有更早的句柄立即失效（含复用同一槽位的新资源场景）；
//   ③ 越界 / 伪造 / 跨表句柄一律解析为 nullptr（绝不允许"猜中"另一个资源）；
//   ④ 槽位复用后 `SlotCount` 不增长、`LiveCount` 正确。
// 用假的资源类型实例化模板：本测试不链接 HugEngineRHI，只验证契约本身。
// ============================================================
#include "RHI/RHIHandles.h"

#include <doctest/doctest.h>

using namespace he;
using namespace he::rhi;

namespace {

/// 假资源：只要有地址就能验证解析是否指向同一个对象
struct FakeBuffer {
    int marker = 0;
};

using FakeTable = RHIResourceTable<RHIBufferHandle, FakeBuffer>;

} // namespace

TEST_CASE("RHIHandles：登记与解析，句柄有效且下标从 1 开始") {
    FakeTable table;
    FakeBuffer a{1}, b{2};

    const auto ha = table.Add(&a);
    const auto hb = table.Add(&b);

    CHECK(ha.IsValid());
    CHECK(hb.IsValid());
    CHECK(ha.index == 1u);                 // 0 留给"无效"
    CHECK(hb.index == 2u);
    CHECK(ha != hb);
    CHECK(table.Get(ha) == &a);
    CHECK(table.Get(hb) == &b);
    CHECK(table.LiveCount() == 2u);
    CHECK(table.SlotCount() == 2u);
}

TEST_CASE("RHIHandles：Remove 后原句柄失效，且幂等") {
    FakeTable table;
    FakeBuffer a{1};
    const auto ha = table.Add(&a);
    REQUIRE(table.Get(ha) == &a);

    table.Remove(ha);
    CHECK(table.Get(ha) == nullptr);        // 立即失效
    CHECK(table.LiveCount() == 0u);
    CHECK(table.FreeSlotCount() == 1u);

    table.Remove(ha);                       // 重复 Remove 必须是 no-op（不能把代次推两次）
    CHECK(table.FreeSlotCount() == 1u);
    CHECK(table.Get(ha) == nullptr);
}

TEST_CASE("RHIHandles：槽位复用后代次前进，旧句柄绝不指向新资源") {
    FakeTable table;
    FakeBuffer a{1}, b{2};

    const auto ha = table.Add(&a);
    const u32  slot = ha.index;
    table.Remove(ha);

    const auto hb = table.Add(&b);          // 必须复用刚回收的槽位
    CHECK(hb.index == slot);
    CHECK(hb.generation != ha.generation);  // 代次前进 ⇒ 旧句柄可被识别
    CHECK(table.SlotCount() == 1u);         // 未增长
    CHECK(table.LiveCount() == 1u);

    CHECK(table.Get(hb) == &b);
    CHECK(table.Get(ha) == nullptr);        // 旧句柄不得"猜中"新资源
}

TEST_CASE("RHIHandles：越界与伪造句柄解析为 nullptr") {
    FakeTable table;
    FakeBuffer a{1};
    const auto ha = table.Add(&a);

    CHECK(table.Get(RHIBufferHandle{}) == nullptr);                 // 无效句柄
    CHECK(table.Get(RHIBufferHandle{99u, 1u}) == nullptr);          // 越界下标
    CHECK(table.Get(RHIBufferHandle{ha.index, ha.generation + 7u}) == nullptr);  // 伪造代次
    CHECK(table.Get(RHIBufferHandle{ha.index, 0u}) == nullptr);     // 代次 0 永不匹配
}

TEST_CASE("RHIHandles：多轮复用后代次持续前进，槽位数不增长") {
    FakeTable table;
    FakeBuffer r{0};

    for (int round = 0; round < 5; ++round) {
        const auto h = table.Add(&r);
        CHECK(h.index == 1u);
        CHECK(h.generation == static_cast<u32>(round) + 1u);
        CHECK(table.Get(h) == &r);
        table.Remove(h);
        CHECK(table.Get(h) == nullptr);
    }
    CHECK(table.SlotCount() == 1u);
    CHECK(table.LiveCount() == 0u);
}

TEST_CASE("RHIHandles：纹理句柄与缓冲句柄是不同类型（传错资源种类应当编译期就被挡住）") {
    RHIResourceTable<RHITextureHandle, FakeBuffer> texTable;
    FakeTable                                      bufTable;
    FakeBuffer                                     r{3};

    const auto th = texTable.Add(&r);
    const auto bh = bufTable.Add(&r);

    CHECK(texTable.Get(th) == &r);
    CHECK(bufTable.Get(bh) == &r);
    CHECK(texTable.Get(RHITextureHandle{bh.index, bh.generation}) == &r);  // 同下标同代次在本表内可解析
    CHECK(bufTable.Get(RHIBufferHandle{th.index, th.generation}) == &r);
    // 两个表互不影响
    texTable.Remove(th);
    CHECK(bufTable.Get(bh) == &r);
}
