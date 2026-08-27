// ============================================================
// Tests/TestAIModule.cpp — AI 运行时模块单例测试
//
// 验证 Initialize/GetDevice/GetScheduler/Shutdown 生命周期。
// 注：测试环境通常无 DEEPSEEK_API_KEY，
// 此时设备仍创建（仅 RemoteLLM 能力为 false）。
// ============================================================

#include "doctest.h"

#include "AI/Runtime/AIModule.h"

using namespace he::ai;

TEST_CASE("AIModule 生命周期：初始化 → 访问 → 关闭") {
    AIModule::Initialize();

    IAIDevice* dev = AIModule::GetDevice();
    REQUIRE(dev != nullptr);
    InferenceScheduler* sched = AIModule::GetScheduler();
    REQUIRE(sched != nullptr);

    // 当前 A1 阶段：RemoteLLM 能力依环境变量，但设备对象必须存在
    (void)dev->GetCaps().supportsRemoteLLM;

    AIModule::Shutdown();
    CHECK(AIModule::GetDevice() == nullptr);
}

TEST_CASE("AIModule 幂等：重复初始化/关闭不崩溃") {
    AIModule::Initialize();
    AIModule::Initialize();   // 幂等
    REQUIRE(AIModule::GetDevice() != nullptr);

    AIModule::Shutdown();
    AIModule::Shutdown();     // 幂等
    CHECK(AIModule::GetDevice() == nullptr);

    AIModule::Initialize();   // 可再次初始化
    REQUIRE(AIModule::GetDevice() != nullptr);
    AIModule::Shutdown();
}
