#include "Core/Log.h"

namespace he {

std::shared_ptr<spdlog::logger> Logger::s_CoreLogger;
std::shared_ptr<spdlog::logger> Logger::s_ClientLogger;

void Logger::Initialize() {
    // Core logger — engine internals
    s_CoreLogger = spdlog::stdout_color_mt("HugEngine");
    s_CoreLogger->set_level(spdlog::level::trace);
    s_CoreLogger->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");

    // Client logger — game code
    s_ClientLogger = spdlog::stdout_color_mt("App");
    s_ClientLogger->set_level(spdlog::level::trace);
    s_ClientLogger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
}

void Logger::Shutdown() {
    s_CoreLogger.reset();
    s_ClientLogger.reset();
    spdlog::shutdown();
}

} // namespace he
