#include "Core/Engine.h"
#include "Core/Log.h"
#include "Core/CVar.h"

#include <cstdlib>   // std::getenv（HE_RENDER_THREADING_MODE 覆盖）

#ifdef _WIN32
#include <windows.h>
#endif

namespace he {

Engine::Engine(const EngineConfig& config)
    : m_Config(config) {
}

Engine::~Engine() {
    Shutdown();
}

void Engine::Initialize() {
    // Windows 控制台默认使用 GBK 编码，切换为 UTF-8 避免中文日志乱码
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // 日志系统必须最先初始化
    Logger::Initialize(m_Config.logLevel);
    HE_CORE_INFO("=== Initializing HugEngine v0.1.0 ===");

    // 2. 调试功能开关：EngineConfig → CVar 桥接（启动时一次性应用）
    // 配置开关写入对应 CVar，RHI 后端读取 CVar 决策；运行时仍可用控制台覆盖
    if (auto* cvar = FindCVar("r.Debug.DrawMarker"))
        cvar->SetFromString(m_Config.enableDrawMarkers ? "true" : "false");

    // 物理光照单位开关（默认关闭：所有光源走传统 intensity 模式）
    if (auto* cvar = FindCVar("r.Light.PhysicalUnits"))
        cvar->SetFromString(m_Config.usePhysicalLights ? "true" : "false");

    // 2.6 渲染线程模式（阶段 0 T0.5）：先按 EngineConfig 落地到进程级，再允许环境变量覆盖 ——
    //     验收需要在同一台机器上反复切换三种模式做对照，而样例到目前为止没有统一的配置解析入口
    //     （T2.4 会把 cfg 键接上）。**解析失败只告警、保持原模式**，绝不静默退回，否则
    //     "以为开了多线程"的实测数据会失去意义。
    {
        RenderThreadingMode mode = m_Config.renderThreadingMode;
        if (const char* env = std::getenv("HE_RENDER_THREADING_MODE")) {
            if (auto parsed = ParseRenderThreadingMode(env)) {
                mode = *parsed;
            } else {
                HE_CORE_WARN("HE_RENDER_THREADING_MODE=\"{}\" 不是合法模式，保持 {}（合法值：0/1/2 "
                             "或 single-threaded / game+render / game+render+rhi）",
                             env, RenderThreadingModeName(mode));
            }
        }
        SetRenderThreadingMode(mode);
        HE_CORE_INFO("渲染线程模式 = {}（{}），自旋等待 {} us", RenderThreadingModeName(mode),
                     static_cast<int>(mode), m_Config.renderThreadSpinWaitUs);
    }

    // 3. Job system
    JobSystem::Initialize(m_Config.jobThreads);

    // 4. Window
    WindowDesc wdesc;
    wdesc.title  = m_Config.appName;
    wdesc.width  = m_Config.windowWidth;
    wdesc.height = m_Config.windowHeight;
    wdesc.vsync  = m_Config.enableVSync;

    m_Window = std::make_unique<Window>(wdesc);

    HE_CORE_INFO("Engine initialized successfully");
}

void Engine::Shutdown() {
    HE_CORE_INFO("Shutting down engine...");
    m_Window.reset();
    JobSystem::Shutdown();
    Logger::Shutdown();
}

} // namespace he
