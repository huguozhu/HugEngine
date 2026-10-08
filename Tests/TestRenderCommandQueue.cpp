// ============================================================
// TestRenderCommandQueue.cpp — 阶段 0 T0.3 / T0.4：命令队列、渲染线程壳、帧票据与背压
//
// 【为什么要有它】这套机制的失效方式全是"时序型"的：帧序错乱、丢失回收（在飞数只增不减 ⇒
// 游戏线程被永久背压卡死）、壳模式却没在调用线程执行（等于偷偷起了线程）。这些都不会编译报错，
// 所以逐条钉死：
//   ① 整帧批量交接 + 命令顺序 + 帧号/槽位轮转；
//   ② 票据回收与在飞计数（发布 N 帧未回收 ⇒ 在飞 = N）；
//   ③ 背压：上限 1 时非阻塞提交必须被拒（可观测），阻塞提交必须真的等到回收才返回；
//   ④ 壳模式：命令在**调用线程**上执行（`IsCurrent()` 恒真，阶段 0 不算违规）。
//
// 本翻译单元直接编译 `Engine/Render/Threading/*.cpp`（它们 RHI-free），不链接 HugEngineRender ——
// 这条约束本身是钉子：一旦有人把 RHI 依赖塞进队列/线程文件，单测目标会立刻编译失败。
// ============================================================
#include "Threading/RenderCommandQueue.h"
#include "Threading/RenderThread.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace he;
using namespace he::render;

TEST_CASE("RenderCommandQueue：整帧批量交接与命令顺序") {
    RenderCommandQueue queue(3);
    std::vector<int>   order;

    for (int frame = 0; frame < 3; ++frame) {
        queue.BeginFrame();
        for (int i = 0; i < 3; ++i) queue.Enqueue([&order, frame, i](RenderThreadContext&) {
            order.push_back(frame * 10 + i);
        });
        queue.PublishFrame();
    }

    RenderThread rt(queue);
    CHECK(rt.PumpAll() == 3u);
    CHECK(order == std::vector<int>({0, 1, 2, 10, 11, 12, 20, 21, 22}));   // 帧内有序、帧间 FIFO
}

TEST_CASE("RenderCommandQueue：帧号单调、帧槽位按上限轮转") {
    RenderCommandQueue queue(3);
    std::vector<u64>   frameIndex;
    std::vector<u32>   frameSlot;

    for (int i = 0; i < 5; ++i) {                    // 5 帧 > 上限 3 ⇒ 槽位必须回绕
        queue.BeginFrame();
        queue.Enqueue([&](RenderThreadContext& ctx) {
            frameIndex.push_back(ctx.frameIndex);
            frameSlot.push_back(ctx.frameSlot);
        });
        queue.PublishFrame();
        queue.RetireFrame(FrameTicket{0, 0});        // 立即回收，避免触发背压
    }
    RenderThread(queue).PumpAll();

    CHECK(frameIndex == std::vector<u64>({0, 1, 2, 3, 4}));
    CHECK(frameSlot == std::vector<u32>({0, 1, 2, 0, 1}));
}

TEST_CASE("RenderCommandQueue：票据与在飞计数") {
    RenderCommandQueue queue(3);
    CHECK(queue.InFlightFrameCount() == 0u);

    FrameTicket t0{}, t1{};
    for (int i = 0; i < 2; ++i) {
        queue.BeginFrame();
        queue.Enqueue([](RenderThreadContext&) {});
        REQUIRE(queue.TrySubmitFrame(i == 0 ? t0 : t1));
    }
    CHECK(queue.InFlightFrameCount() == 2u);         // 未回收 ⇒ 在飞 = 2
    CHECK(queue.PublishedFrameCount() == 2u);

    queue.RetireFrame(t0);
    CHECK(queue.InFlightFrameCount() == 1u);
    queue.RetireFrame(t1);
    CHECK(queue.InFlightFrameCount() == 0u);
    CHECK(queue.RetiredFrameCount() == 2u);
}

TEST_CASE("RenderCommandQueue：上限 1 时非阻塞提交被拒（背压可观测）") {
    RenderCommandQueue queue(1);
    FrameTicket t{};

    queue.BeginFrame();
    queue.Enqueue([](RenderThreadContext&) {});
    CHECK(queue.TrySubmitFrame(t));                  // 第 1 帧：在飞 0 → 1，成功
    CHECK(queue.InFlightFrameCount() == 1u);

    queue.BeginFrame();
    queue.Enqueue([](RenderThreadContext&) {});
    CHECK_FALSE(queue.TrySubmitFrame(t));            // 第 2 帧：在飞已满 ⇒ 拒绝
    CHECK(queue.DroppedFrameCount() == 1u);          // 且必须记入丢弃数（不静默）

    queue.RetireFrame(FrameTicket{0, 0});
    queue.BeginFrame();
    queue.Enqueue([](RenderThreadContext&) {});
    CHECK(queue.TrySubmitFrame(t));                  // 回收后又能提交
}

TEST_CASE("RenderCommandQueue：阻塞提交必须等到回收才返回") {
    RenderCommandQueue queue(1);
    FrameTicket t{};
    queue.BeginFrame();
    queue.Enqueue([](RenderThreadContext&) {});
    REQUIRE(queue.TrySubmitFrame(t));                 // 占掉唯一名额

    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        queue.BeginFrame();                           // 注意：BeginFrame 会丢弃上一帧的残留收集态
        queue.Enqueue([](RenderThreadContext&) {});
        queue.SubmitFrameBlocking();                  // 背压：应阻塞
        returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_FALSE(returned.load());                     // 仍在阻塞（这就是"游戏线程被背压"的可测形态）
    queue.RetireFrame(t);                             // 回收 ⇒ 唤醒
    waiter.join();
    CHECK(returned.load());
    CHECK(queue.BackpressureWaitCount() == 1u);
}

TEST_CASE("RenderThread：壳模式在调用线程执行（必须）") {
    RenderCommandQueue queue(3);
    const auto callerId = std::this_thread::get_id();
    std::thread::id execId{};

    queue.BeginFrame();
    queue.Enqueue([&execId](RenderThreadContext&) { execId = std::this_thread::get_id(); });
    queue.PublishFrame();

    RenderThread rt(queue);
    CHECK(RenderThread::IsCurrent());                 // 阶段 0：当前线程即渲染线程
    CHECK(rt.PumpOnce());
    CHECK(execId == callerId);                        // 没有偷偷起线程
    CHECK_FALSE(rt.PumpOnce());                       // 无待处理帧
    CHECK(rt.ExecutedFrameCount() == 1u);
    CHECK(rt.ExecutedCommandCount() == 1u);
}

TEST_CASE("FrameScheduler：提交→执行→回收，在飞数归零") {
    RenderCommandQueue queue(3);
    RenderThread       rt(queue);
    FrameScheduler     sched(queue, rt);

    int runs = 0;
    for (int i = 0; i < 4; ++i) {
        queue.BeginFrame();
        queue.Enqueue([&runs](RenderThreadContext&) { ++runs; });
        sched.SubmitAndPump();
        CHECK(queue.InFlightFrameCount() == 0u);      // 壳模式每帧执行后立即回收
    }
    CHECK(runs == 4);
    CHECK(queue.PublishedFrameCount() == 4u);
    CHECK(rt.ExecutedFrameCount() == 4u);
}

TEST_CASE("RenderCommandQueue：未发布就再次 BeginFrame 必须计入丢弃") {
    RenderCommandQueue queue(2);
    queue.BeginFrame();
    queue.Enqueue([](RenderThreadContext&) {});
    queue.BeginFrame();                               // 半成品被丢弃
    CHECK(queue.DroppedFrameCount() == 1u);

    queue.Enqueue([](RenderThreadContext&) {});
    queue.PublishFrame();
    RenderFrameCommands frame;
    REQUIRE(queue.TryConsumeFrame(frame));
    CHECK(frame.commands.size() == 1u);               // 只带走了第二次 BeginFrame 之后的那条
    CHECK(frame.frameIndex == 1u);                    // 帧号已前进（第一次的号被丢弃）
}
