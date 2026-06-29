#pragma once

#include "Core/Types.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>

// ============================================================
// Logging system — thin wrapper over spdlog
// ============================================================

namespace he {

// 日志等级（与 spdlog 对齐）
enum class LogLevel : u8 {
    Trace    = 0,
    Debug    = 1,
    Info     = 2,
    Warn     = 3,
    Error    = 4,
    Critical = 5,
    Off      = 6,
};

class Logger {
public:
    static void Initialize(LogLevel level = LogLevel::Info);
    static void Shutdown();

    static spdlog::logger* GetCoreLogger()   { return s_CoreLogger.get(); }
    static spdlog::logger* GetClientLogger() { return s_ClientLogger.get(); }

private:
    static std::shared_ptr<spdlog::logger> s_CoreLogger;
    static std::shared_ptr<spdlog::logger> s_ClientLogger;
};

} // namespace he

// --- Log macros ---
#define HE_CORE_TRACE(...)    ::he::Logger::GetCoreLogger()->trace(__VA_ARGS__)
#define HE_CORE_INFO(...)     ::he::Logger::GetCoreLogger()->info(__VA_ARGS__)
#define HE_CORE_WARN(...)     ::he::Logger::GetCoreLogger()->warn(__VA_ARGS__)
#define HE_CORE_ERROR(...)    ::he::Logger::GetCoreLogger()->error(__VA_ARGS__)
#define HE_CORE_CRITICAL(...) ::he::Logger::GetCoreLogger()->critical(__VA_ARGS__)

#define HE_TRACE(...)         ::he::Logger::GetClientLogger()->trace(__VA_ARGS__)
#define HE_INFO(...)          ::he::Logger::GetClientLogger()->info(__VA_ARGS__)
#define HE_WARN(...)          ::he::Logger::GetClientLogger()->warn(__VA_ARGS__)
#define HE_ERROR(...)         ::he::Logger::GetClientLogger()->error(__VA_ARGS__)
#define HE_CRITICAL(...)      ::he::Logger::GetClientLogger()->critical(__VA_ARGS__)
