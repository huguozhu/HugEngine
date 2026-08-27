#include "AI/Runtime/AIModule.h"

#include "Core/Log.h"

namespace he::ai {

std::unique_ptr<InferenceScheduler> AIModule::s_Scheduler;
std::unique_ptr<IAIDevice> AIModule::s_Device;

void AIModule::Initialize() {
    if (s_Device) return;   // 幂等：已初始化

    // 1. 调度器（流式回调投递 + 优先级车道）
    s_Scheduler = std::make_unique<InferenceScheduler>();
    // 2. AI 设备（按环境变量建后端；当前注册 RemoteBackend）
    s_Device = CreateAIDevice(s_Scheduler.get());

    HE_CORE_INFO("[AIModule] AI 运行时初始化完成（RemoteLLM={}）",
                 s_Device->GetCaps().supportsRemoteLLM);
}

void AIModule::Shutdown() {
    if (!s_Device) return;   // 幂等：未初始化或已关闭
    s_Device.reset();
    s_Scheduler.reset();
    HE_CORE_INFO("[AIModule] AI 运行时已关闭");
}

IAIDevice* AIModule::GetDevice() {
    return s_Device.get();
}

InferenceScheduler* AIModule::GetScheduler() {
    return s_Scheduler.get();
}

} // namespace he::ai
