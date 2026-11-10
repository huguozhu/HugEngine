#include "Core/Platform.h"   // HE_PLATFORM_*：鸿蒙分支要按平台切换 sink

#include "Core/Log.h"

#include <string>
#include <vector>

#if HE_PLATFORM_OHOS
    // 鸿蒙：spdlog 的 stdout 在应用进程里不一定能进 hilog，额外挂一个 hilog sink，
    // 这样 `hdc shell hilog` 能直接看到引擎日志（tag = HugEngine / App）。
    #include <hilog/log.h>
    #include <spdlog/sinks/base_sink.h>
#elif HE_PLATFORM_ANDROID
    // Android：同理挂一个 logcat sink（`adb logcat -s HugEngine`）
    #include <android/log.h>
    #include <spdlog/sinks/base_sink.h>
#endif

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

#if HE_PLATFORM_OHOS
namespace {

/// 把 spdlog 的每条日志转发到鸿蒙 hilog（tag 用于 `hdc shell hilog -T <tag>` 过滤）
class HilogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit HilogSink(const char* tag) : m_Tag(tag) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        // 只用 payload（已格式化好的正文），避免再走一遍 pattern
        const std::string text(msg.payload.data(), msg.payload.size());
        ::LogLevel level = LOG_INFO;   // 【必须全局限定】he::LogLevel 会遮蔽 hilog 的同名枚举
        switch (msg.level) {
            case spdlog::level::trace:
            case spdlog::level::debug:    level = LOG_DEBUG; break;
            case spdlog::level::info:     level = LOG_INFO;  break;
            case spdlog::level::warn:     level = LOG_WARN;  break;
            case spdlog::level::err:
            case spdlog::level::critical: level = LOG_ERROR; break;
            default:                      level = LOG_INFO;  break;
        }
        // %{public}s：鸿蒙默认把 %s 当私有参数打码，必须显式标 public 才看得见内容
        OH_LOG_Print(LOG_APP, level, 0x0000, m_Tag, "%{public}s", text.c_str());
    }
    void flush_() override {}

private:
    const char* m_Tag;
};

} // namespace
#endif // HE_PLATFORM_OHOS

#if HE_PLATFORM_ANDROID
namespace {

/// 把 spdlog 的每条日志转发到 Android logcat（tag 用于 `adb logcat -s <tag>` 过滤）
class LogcatSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit LogcatSink(const char* tag) : m_Tag(tag) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        const std::string text(msg.payload.data(), msg.payload.size());
        int prio = ANDROID_LOG_INFO;
        switch (msg.level) {
            case spdlog::level::trace:
            case spdlog::level::debug:    prio = ANDROID_LOG_DEBUG; break;
            case spdlog::level::info:     prio = ANDROID_LOG_INFO;  break;
            case spdlog::level::warn:     prio = ANDROID_LOG_WARN;  break;
            case spdlog::level::err:
            case spdlog::level::critical: prio = ANDROID_LOG_ERROR; break;
            default:                      prio = ANDROID_LOG_INFO;  break;
        }
        __android_log_print(prio, m_Tag, "%s", text.c_str());
    }
    void flush_() override {}

private:
    const char* m_Tag;
};

} // namespace
#endif // HE_PLATFORM_ANDROID

void Logger::Initialize(LogLevel level) {
    // 幂等：鸿蒙 NativeEntry 可能先初始化一次，Engine 构造时又会调用一次
    if (s_CoreLogger && s_ClientLogger) {
        return;
    }

    const auto spdLevel = ToSpdlogLevel(level);

    // Core logger — engine internals
    {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#if HE_PLATFORM_OHOS
        sinks.push_back(std::make_shared<HilogSink>("HugEngine"));
#elif HE_PLATFORM_ANDROID
        sinks.push_back(std::make_shared<LogcatSink>("HugEngine"));
#endif
        s_CoreLogger = std::make_shared<spdlog::logger>("HugEngine", sinks.begin(), sinks.end());
        s_CoreLogger->set_level(spdLevel);
        s_CoreLogger->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");
        s_CoreLogger->flush_on(spdlog::level::warn);
    }

    // Client logger — game code
    {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#if HE_PLATFORM_OHOS
        sinks.push_back(std::make_shared<HilogSink>("App"));
#elif HE_PLATFORM_ANDROID
        sinks.push_back(std::make_shared<LogcatSink>("App"));
#endif
        s_ClientLogger = std::make_shared<spdlog::logger>("App", sinks.begin(), sinks.end());
        s_ClientLogger->set_level(spdLevel);
        s_ClientLogger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        s_ClientLogger->flush_on(spdlog::level::warn);
    }
}

void Logger::Shutdown() {
    s_CoreLogger.reset();
    s_ClientLogger.reset();
    spdlog::shutdown();
}

} // namespace he
