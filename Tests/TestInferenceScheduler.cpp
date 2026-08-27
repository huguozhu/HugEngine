// ============================================================
// Tests/TestInferenceScheduler.cpp — 推理调度器单元测试
//
// 覆盖：优先级车道（高优先先执行）、
//       流式回调投递（PostToMain → DrainMainThread 保序）。
// ============================================================

#include "doctest.h"

#include "AI/Runtime/InferenceScheduler.h"

#include <vector>

using namespace he::ai;

TEST_CASE("InferenceScheduler 优先级车道：高优先先执行") {
    InferenceScheduler sched;
    std::vector<int> order;

    // 故意先提交后台任务、后提交帧任务
    sched.Submit(InferencePriority::Background, [&] { order.push_back(1); });
    sched.Submit(InferencePriority::Frame,      [&] { order.push_back(0); });
    sched.Submit(InferencePriority::Normal,     [&] { order.push_back(2); });

    sched.WaitIdle();
    REQUIRE(order.size() == 3);
    CHECK(order[0] == 0);   // Frame 最先
    CHECK(order[1] == 2);   // Normal 其次
    CHECK(order[2] == 1);   // Background 最后
}

TEST_CASE("InferenceScheduler 流式回调按序投递到主线程") {
    InferenceScheduler sched;
    std::vector<int> order;

    // 推理线程内投递 5 个回调（模拟流式 token 分段）
    sched.Submit(InferencePriority::Normal, [&] {
        for (int i = 0; i < 5; ++i)
            sched.PostToMain([&, i] { order.push_back(i); });
    });
    sched.WaitIdle();

    // drain 之前回调不应执行（不阻塞主线程）
    CHECK(order.empty());

    // 主线程 drain：FIFO 顺序执行
    sched.DrainMainThread();
    REQUIRE(order.size() == 5);
    for (int i = 0; i < 5; ++i)
        CHECK(order[i] == i);
}

TEST_CASE("InferenceScheduler 同优先级 FIFO 顺序") {
    InferenceScheduler sched;
    std::vector<int> order;
    for (int i = 0; i < 4; ++i)
        sched.Submit(InferencePriority::Normal, [&, i] { order.push_back(i); });
    sched.WaitIdle();
    REQUIRE(order.size() == 4);
    for (int i = 0; i < 4; ++i)
        CHECK(order[i] == i);
}
