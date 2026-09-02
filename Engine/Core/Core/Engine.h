#pragma once

#include "Core/Core.h"
#include "Core/Log.h"
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
