// ============================================================
// TestThreadAffinity.cpp — 渲染线程化方案 T0.1：线程归属断言的单元测试
//
// 【为什么要有它】`HE_ASSERT_RENDER_THREAD()` 是"RHI 只允许渲染线程调用"这条铁律
// （方案 §3 铁律 1）的唯一强制手段，而它的失效方式很隐蔽：**未 Claim 时 `IsOwner()` 恒为真**
// ⇒ 任何违规调用都会静默通过。这里把三种状态钉死：
//   ① 未 Claim（设备未创建/已销毁）→ 对任何线程都不设限；
//   ② Claim 后 → 只有认领线程是拥有者，其它线程被拦截；
//   ③ Release 后 → 重新回到不设限。
//
// 【为什么不直接测"断言触发"】拦截路径会走 `he::AssertFailure` → `std::abort()`，
// 一旦触发整个测试进程就死了。因此跨线程只求值 `IsOwner()`（它正是断言的判据），
// 而 `CHECK` 一律放在 join 之后的主线程上做（doctest 的断言上下文是每线程的）。
// ============================================================
#include "RHI/ThreadAffinity.h"

#include <doctest/doctest.h>

#include <thread>

using namespace he;

TEST_CASE("ThreadAffinity：未 Claim 时对任何线程都不设限") {
    auto& aff = rhi::GetThreadAffinity();
    aff.Release();                       // 复位到"未认领"（设备尚未创建的语义）

    CHECK_FALSE(aff.IsClaimed());
    CHECK(aff.IsOwner());                // 当前线程放行

    bool otherThreadOwner = false;
    std::thread t([&aff, &otherThreadOwner] { otherThreadOwner = aff.IsOwner(); });
    t.join();
    CHECK(otherThreadOwner);             // 其它线程同样放行：加载前/销毁后不设限
}

TEST_CASE("ThreadAffinity：Claim 后只有认领线程是拥有者") {
    auto& aff = rhi::GetThreadAffinity();
    aff.Claim();
    CHECK(aff.IsClaimed());
    CHECK(aff.IsOwner());                // 本线程 = 认领线程

    bool otherThreadOwner = true;
    std::thread t([&aff, &otherThreadOwner] { otherThreadOwner = aff.IsOwner(); });
    t.join();
    CHECK_FALSE(otherThreadOwner);       // 其它线程被拦截（真实路径上会断言失败）

    aff.Release();
    CHECK_FALSE(aff.IsClaimed());
    CHECK(aff.IsOwner());                // 释放后重新不设限
}

TEST_CASE("ThreadAffinity：在新线程上 Claim 会更新拥有者") {
    auto& aff = rhi::GetThreadAffinity();
    aff.Claim();
    CHECK(aff.IsOwner());

    bool claimedOnOtherThread = false;   // 在子线程认领（模拟"设备在渲染线程创建"）
    std::thread t([&aff, &claimedOnOtherThread] {
        aff.Claim();
        claimedOnOtherThread = aff.IsOwner();
    });
    t.join();

    CHECK(claimedOnOtherThread);
    CHECK_FALSE(aff.IsOwner());          // 拥有者已变成刚结束的那个线程

    aff.Release();                       // 复位，避免影响同进程内的其它用例
    CHECK(aff.IsOwner());
}
