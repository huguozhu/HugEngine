// ============================================================
// Tests/TestAIPipeline.cpp — 异步生成管线单元测试
//
// 覆盖：提交与完成回调派发、相同请求去重、取消不派发 onDone。
// ============================================================

#include "doctest.h"

#include "AI/AIGC/AIPipeline.h"
#include "AI/Runtime/InferenceScheduler.h"

#include <atomic>

using namespace he;
using namespace he::ai::aigc;

namespace {

// 假 Provider：立即同步成功，记录调用次数
struct FakeProvider : IAIGCProvider {
    std::atomic<int> calls{0};
    bool Supports(GenKind k) const override { return k == GenKind::Scene; }
    void Generate(const GenRequest&, std::function<void(GenResult&&)> onDone) override {
        ++calls;
        GenResult r;
        r.success = true;
        if (onDone) onDone(std::move(r));
    }
};

} // namespace

TEST_CASE("AIPipeline 提交并派发完成回调") {
    he::ai::InferenceScheduler sched;
    FakeProvider provider;
    AIPipeline pipe(&provider, &sched);

    std::atomic<int> done{0};
    u64 id = pipe.Enqueue({GenKind::Scene, "村庄"}, [&](GenResult&&) { ++done; });
    REQUIRE(id != 0);

    sched.WaitIdle();   // 等后台生成完成
    pipe.Poll();        // 主线程派发 onDone
    CHECK(done == 1);
    CHECK(provider.calls == 1);
}

TEST_CASE("AIPipeline 相同请求去重合并") {
    he::ai::InferenceScheduler sched;
    FakeProvider provider;
    AIPipeline pipe(&provider, &sched);

    std::atomic<int> done{0};
    u64 id1 = pipe.Enqueue({GenKind::Scene, "村庄"}, [&](GenResult&&) { ++done; });
    u64 id2 = pipe.Enqueue({GenKind::Scene, "村庄"}, [&](GenResult&&) { ++done; });
    CHECK(id2 == id1);   // 相同请求返回已有任务 id

    sched.WaitIdle();
    pipe.Poll();
    CHECK(provider.calls == 1);   // 只执行一次
    CHECK(done == 1);
}

TEST_CASE("AIPipeline 取消后不派发 onDone") {
    he::ai::InferenceScheduler sched;
    FakeProvider provider;
    AIPipeline pipe(&provider, &sched);

    std::atomic<int> done{0};
    u64 id = pipe.Enqueue({GenKind::Scene, "村庄"}, [&](GenResult&&) { ++done; });
    pipe.Cancel(id);

    sched.WaitIdle();
    pipe.Poll();
    CHECK(done == 0);   // 取消的任务不派发完成回调
}
