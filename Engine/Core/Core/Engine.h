#pragma once

#include "Core/Core.h"
#include "Core/Log.h"
#include "Core/RenderThreadingMode.h"   // 三态渲染线程模式（阶段 0 T0.5）
#include "Platform/Window.h"
#include "Threading/JobSystem.h"

// ============================================================
// Engine initialization — bootstraps all core systems
// ============================================================

namespace he {

struct EngineConfig {
    String      appName     = "HugEngine";
    u32         windowWidth  = kDefaultWindowWidth;
    u32         windowHeight = kDefaultWindowHeight;
    bool        enableVSync  = true;
    u32         jobThreads   = 0;    // 0 = auto-detect
    bool        enableValidation = true;
    bool        enableMultiThreadRecord = true;  // Phase 5-4: 多线程命令录制
    bool        enableDrawMarkers = true;   // DrawCall 级调试 marker（RenderDoc 定位用，启动时写入 r.Debug.DrawMarker）
    bool        usePhysicalLights = false;  // 1=启用物理光照单位（illuminance/luminousIntensity 生效，默认关）；0=传统 intensity 模式
    LogLevel    logLevel     = LogLevel::Info;  // 默认日志等级

    // --- 渲染线程模式（阶段 0 T0.5）---
    // 三态：单线程渲染（默认 = 现状）/ 游戏线程+渲染线程 / 游戏线程+渲染线程+RHI 线程。
    // 阶段 0 只落地"参数 + 读取路径"：多线程路径由 T0.3 的壳（仍在本线程消费，行为等价）
    // 与阶段 2 的真线程接管；默认值永远是可直接回退的单线程。
    RenderThreadingMode renderThreadingMode = RenderThreadingMode::SingleThreaded;
    // 渲染线程空闲时"先自旋再睡"的窗口（微秒）。0 = 直接条件变量等待（不烧核，默认）；
    // 低延迟场景可调大（T2.1 的休眠策略读它；空闲 CPU 占用必须 < 1%）。
    u32 renderThreadSpinWaitUs = 0;

    [[nodiscard]] bool UsesRenderThread() const {
        return renderThreadingMode != RenderThreadingMode::SingleThreaded;
    }
    [[nodiscard]] bool UsesRHIThread() const {
        return renderThreadingMode == RenderThreadingMode::RenderThreadAndRHI;
    }
};

class Engine {
    HE_DECLARE_NON_COPYABLE(Engine);
    HE_DECLARE_NON_MOVABLE(Engine);

public:
    Engine(const EngineConfig& config);
    ~Engine();

    void Initialize();
    void Shutdown();

    Window*    GetWindow()    { return m_Window.get(); }
    JobSystem* GetJobSystem() { return m_JobSystem.get(); }

private:
    EngineConfig                    m_Config;
    std::unique_ptr<Window>         m_Window;
    std::unique_ptr<JobSystem>      m_JobSystem;
};

} // namespace he
