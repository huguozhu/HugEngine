#include "Core/Engine.h"
#include "Core/Log.h"

namespace he {

Engine::Engine(const EngineConfig& config)
    : m_Config(config) {
}

Engine::~Engine() {
    Shutdown();
}

void Engine::Initialize() {
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
