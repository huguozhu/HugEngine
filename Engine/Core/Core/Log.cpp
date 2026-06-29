#include "Core/Log.h"

namespace he {

std::shared_ptr<spdlog::logger> Logger::s_CoreLogger;
std::shared_ptr<spdlog::logger> Logger::s_ClientLogger;

// LogLevel → spdlog::level 映射
static spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return spdlog::level::trace;
        case LogLevel::Debug:    return spdlog::level::debug;
        case LogLevel::Info:     return spdlog::level::info;
        case LogLevel::Warn:     return spdlog::level::warn;
        case LogLevel::Error:    return spdlog::level::err;
        case LogLevel::Critical: return spdlog::level::critical;
        case LogLevel::Off:      return spdlog::level::off;
        default:                 return spdlog::level::info;
    }
}

void Logger::Initialize(LogLevel level) {
    auto spdLevel = ToSpdlogLevel(level);

    // Core logger — engine internals
    s_CoreLogger = spdlog::stdout_color_mt("HugEngine");
    s_CoreLogger->set_level(spdLevel);
    s_CoreLogger->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");

    // Client logger — game code
    s_ClientLogger = spdlog::stdout_color_mt("App");
    s_ClientLogger->set_level(spdLevel);
    s_ClientLogger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
}

void Logger::Shutdown() {
    s_CoreLogger.reset();
    s_ClientLogger.reset();
    spdlog::shutdown();
}

} // namespace he
