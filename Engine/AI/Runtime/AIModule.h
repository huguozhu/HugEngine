#pragma once

#include "AI/Runtime/AIDevice.h"
#include "AI/Runtime/InferenceScheduler.h"

#include <memory>

// ============================================================
// AIModule — AI 运行时模块单例（A1 基座的进程级入口）
//
// 生命周期：Initialize() 创建 InferenceScheduler + IAIDevice（含后端），
// GetDevice()/GetScheduler() 供引擎与编辑器访问，Shutdown() 逆序释放。
//
// 注：文档 3 提出由 Core 的 Engine 直接持有 m_AIDevice，
// 但 Core 是 L0 基础层、AI 依赖 Scene/Reflect，反向引用会形成循环依赖，
// 故 A1 以模块单例实现（解耦），Engine 直接持有的集成留待后续评估。
// ============================================================

namespace he::ai {

class AIModule {
public:
    // 初始化 AI 运行时（幂等）：调度器 + AI 设备 + 后端
    static void Initialize();
    // 关闭 AI 运行时（幂等）：逆序释放设备与调度器
    static void Shutdown();

    static IAIDevice* GetDevice();
    static InferenceScheduler* GetScheduler();

private:
    static std::unique_ptr<InferenceScheduler> s_Scheduler;
    static std::unique_ptr<IAIDevice> s_Device;
};

} // namespace he::ai
