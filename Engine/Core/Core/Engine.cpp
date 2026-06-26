#include "Core/Engine.h"
#include "Core/Log.h"

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
    Logger::Initialize();
    HE_CORE_INFO("=== Initializing HugEngine v0.1.0 ===");

    // 2. Job system
    JobSystem::Initialize(m_Config.jobThreads);

    // 3. Window
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
