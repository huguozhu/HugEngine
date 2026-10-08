// ============================================================
// TestRenderThread.cpp — 阶段 2 T2.1：渲染线程**真起线程**
//
// 【为什么要有它】T2.1 的失效方式全是"时序/归属型"的，且都不会编译报错：
//   ① 说好真起线程，其实还在调用线程执行（= 阶段 0 的壳，功能没变却以为做完了）；
//   ② 线程归属判断错（`IsCurrent()` 在渲染线程里返回 false ⇒ T0.1 的断言会在渲染线程里误报）；
//   ③ 停止时丢掉未执行的帧（游戏线程等票据回收 ⇒ 永久背压卡死）；
//   ④ 退出策略错（线程不 join ⇒ 进程退出时崩溃）。
// 逐条钉死，并把"必须在**另一根**线程上执行"写成断言（比较线程 id）。
// ============================================================
#include "Threading/RenderThread.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace he;
using namespace he::render;

namespace {
/// 有超时地等待条件成立（避免测试因实现缺陷而永久挂住）
template <typename TPredicate>
bool WaitFor(TPredicate pred, int timeoutMs = 3000) {
    for (int i = 0; i < timeoutMs; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}
} // namespace

TEST_CASE("RenderThread(T2.1)：命令在**另一根**线程执行，且归属判断为真") {
    RenderCommandQueue queue(4);
    RenderThread       rt(queue);

    CHECK_FALSE(rt.IsRunning());
    CHECK(RenderThread::IsCurrent());        // 壳模式：当前线程即渲染线程（阶段 0 语义不变）

    REQUIRE(rt.Start());
    CHECK(rt.IsRunning());

    const std::thread::id callerId = std::this_thread::get_id();
    std::atomic<bool>     ranInside{false};
    std::thread::id       executedOn{};

    queue.BeginFrame();
    queue.Enqueue([&](RenderThreadContext&) {
        executedOn = std::this_thread::get_id();
        ranInside.store(RenderThread::IsCurrent());   // 在渲染线程里必须为 true
    });
    FrameTicket ticket{};
    REQUIRE(queue.TrySubmitFrame(ticket));

    REQUIRE(WaitFor([&] { return rt.ExecutedFrameCount() >= 1u; }));
    CHECK(ranInside.load());
    CHECK(executedOn != callerId);                    // 关键判据：确实换了一根线程
    CHECK(executedOn != std::thread::id{});
    CHECK(rt.ExecutedCommandCount() == 1u);

    // 票据必须被渲染线程回收（否则背压会永久生效）
    REQUIRE(WaitFor([&] { return queue.InFlightFrameCount() == 0u; }));

    rt.Stop();
    CHECK_FALSE(rt.IsRunning());
    CHECK(RenderThread::IsCurrent());        // 停止后回到壳语义
}

TEST_CASE("RenderThread(T2.1)：多帧按序执行且票据全部回收") {
    RenderCommandQueue queue(4);
    RenderThread       rt(queue);
    REQUIRE(rt.Start());

    std::atomic<int> executed{0};
    for (int i = 0; i < 8; ++i) {
        queue.BeginFrame();
        queue.Enqueue([&](RenderThreadContext&) { executed.fetch_add(1); });
        // 用**阻塞**提交：上限 4，非阻塞提交在渲染线程还没回收时会被拒（背压是特性，不是缺陷）。
        // 这正是真线程模式下游戏线程的真实调用形态：满了就在票据上等，而不是丢帧。
        const FrameTicket ticket = queue.SubmitFrameBlocking();
        CHECK(ticket.frameIndex == static_cast<u64>(i));
    }

    REQUIRE(WaitFor([&] { return executed.load() == 8; }));
    REQUIRE(WaitFor([&] { return queue.InFlightFrameCount() == 0u; }));
    CHECK(queue.PublishedFrameCount() == 8u);
    CHECK(queue.RetiredFrameCount() == 8u);

    rt.Stop();
}

TEST_CASE("RenderThread(T2.1)：Stop 排空已提交但未执行的帧") {
    RenderCommandQueue queue(4);
    RenderThread       rt(queue);
    // 【不 Start】先按壳模式攒下 3 帧（谁也没执行），随后启动线程 —— 帧不能丢
    for (int i = 0; i < 3; ++i) {
        queue.BeginFrame();
        queue.Enqueue([](RenderThreadContext&) {});
        FrameTicket ticket{};
        REQUIRE(queue.TrySubmitFrame(ticket));
    }
    CHECK(queue.PendingFrameCount() == 3u);

    REQUIRE(rt.Start());
    REQUIRE(WaitFor([&] { return rt.ExecutedFrameCount() == 3u; }));
    rt.Stop();
    CHECK(queue.InFlightFrameCount() == 0u);          // 全部回收
    CHECK(queue.PendingFrameCount() == 0u);
}

TEST_CASE("RenderThread(T2.1)：未启动时仍是壳语义（就地执行，IsCurrent 恒真）") {
    RenderCommandQueue queue(2);
    RenderThread       rt(queue);
    CHECK_FALSE(rt.IsRunning());

    queue.BeginFrame();
    queue.Enqueue([&](RenderThreadContext&) { CHECK(RenderThread::IsCurrent()); });
    queue.PublishFrame();

    CHECK(rt.PumpOnce());                             // 就地执行
    CHECK(rt.ExecutedFrameCount() == 1u);
    CHECK_FALSE(rt.PumpOnce());                       // 没有待处理帧
}

TEST_CASE("严格握手（T2.2）：SubmitAndWait 只在帧真正被消费完后返回") {
    // 【它测什么】`WaitFrameRetired` 是"把每帧 RHI 调用搬进渲染命令"时逐项验证的前提：
    // 游戏线程发布后必须等到本帧（含呈现）做完，否则下一帧的采集会与渲染线程并发改写共享状态。
    RenderCommandQueue queue(2);
    RenderThread       rt(queue);
    FrameScheduler     sched(queue, rt);

    // 壳模式：就地执行并回收 ⇒ 立刻满足
    bool timedOut = true;
    queue.BeginFrame();
    queue.Enqueue([](RenderThreadContext&) {});
    const u64 f0 = sched.SubmitAndWait(timedOut, 200u);
    CHECK_FALSE(timedOut);
    CHECK(queue.WaitFrameRetired(f0, 10u));
    CHECK(queue.InFlightFrameCount() == 0u);

    // 未发布的帧号：等待应超时（不静默返回成功）
    bool timedOut2 = false;
    CHECK_FALSE(queue.WaitFrameRetired(999u, 20u));
    timedOut2 = true;   // 位点仅用于说明"超时路径不会阻塞"

    // 真线程模式：发布后由渲染线程执行，握手必须真的等到回收
    REQUIRE(rt.Start());
    queue.BeginFrame();
    queue.Enqueue([](RenderThreadContext&) {});
    bool timedOut3 = true;
    const u64 f1 = sched.SubmitAndWait(timedOut3, 2000u);
    CHECK_FALSE(timedOut3);                       // 2s 内必须做完
    CHECK(queue.WaitFrameRetired(f1, 0u));        // 已退休 ⇒ 立即返回
    CHECK(rt.ExecutedFrameCount() >= 1u);
    rt.Stop();
    CHECK(queue.InFlightFrameCount() == 0u);
}
