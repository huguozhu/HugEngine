// Engine/Render/ShaderHotReload.h
// Shader 热重载 — 监控 .slang 文件变化 → 自动重编译 → PSO 热替换
#pragma once

#include "Core/Types.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace he::render {

class ShaderHotReload {
public:
    // 回调: (shader文件名, SPIR-V字节码)
    using ReloadCallback = std::function<void(const String& shaderName,
                                               const std::vector<u32>& spirv)>;

    ShaderHotReload();
    ~ShaderHotReload();

    // 启动文件监控 + 设置重载回调（主线程调用）
    // shaderDir: .slang 文件所在目录 (如 "Engine/Shader/Shaders")
    // slangcPath: slangc 可执行文件路径
    void Start(StringView shaderDir, StringView slangcPath,
               ReloadCallback onReload);

    // 每帧调用，处理待重载的 Shader（主线程调用，安全）
    void Poll();

    // 停止监控
    void Stop();

private:
    // 文件监控线程
    void WatchThread(StringView shaderDir);

    // 调用 slangc 编译单个 .slang → SPIR-V
    static bool CompileShader(StringView slangcPath, StringView slFile,
                              StringView stage, StringView entryPoint,
                              StringView includeDir,
                              std::vector<u32>& outSpirv);

    // 从 .spv 二进制文件读取 SPIR-V 字码
    static bool LoadSpirv(StringView spvPath, std::vector<u32>& outSpirv);

    // 根据扩展名推断 shader stage 和 entry point
    static bool InferShaderType(StringView filename,
                                String& outStage, String& outEntry);

    std::thread          m_WatchThread;
    std::atomic<bool>    m_Running{false};

    // 主线程回调（WatchThread → m_Pending 队列入队，Poll 中消费）
    std::mutex           m_Mutex;
    std::vector<std::pair<String, std::vector<u32>>> m_Pending;
    ReloadCallback       m_OnReload;
    String              m_SlangcPath;
    String              m_IncludeDir;
};

} // namespace he::render
