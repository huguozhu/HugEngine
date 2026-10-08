// ============================================================
// TestThreadAffinityDisabled.cpp — T0.1 验收里的一条硬要求：断言必须**可被关闭**
//
// 【为什么单独一个 TU】`HE_DISABLE_THREAD_ASSERT` 必须在 `RHI/ThreadAffinity.h` 被包含**之前**
// 定义（宏展开在预处理期决定），所以不能和 TestThreadAffinity.cpp 放同一文件。
//
// 【怎么算通过】关闭开关后 `HE_ASSERT_RENDER_THREAD()` 展开为 `((void)0)`：
// 在**非拥有线程**上调用它必须正常返回，而不是 `std::abort()`。
// 这正是"关闭路径能救急"的判据——若哪天有人把关闭分支写成仍会求值归属对象，这条用例会崩。
// ============================================================
#define HE_DISABLE_THREAD_ASSERT 1
#include "RHI/ThreadAffinity.h"

#include <doctest/doctest.h>

#include <thread>

using namespace he;

TEST_CASE("HE_DISABLE_THREAD_ASSERT：断言宏在非拥有线程上是 no-op") {
    auto& aff = rhi::GetThreadAffinity();
    aff.Claim();                          // 拥有者 = 本线程
    CHECK(aff.IsClaimed());
    CHECK(aff.IsOwner());

    bool macroReturnedOnOtherThread = false;
    std::thread t([&macroReturnedOnOtherThread] {
        // 关闭开关下这一行必须**什么都不做**（否则进程在此 abort，测试整体失败）
        HE_ASSERT_RENDER_THREAD();
        macroReturnedOnOtherThread = true;
    });
    t.join();
    CHECK(macroReturnedOnOtherThread);    // 走完说明没有 abort

    aff.Release();
    CHECK(aff.IsOwner());
}
