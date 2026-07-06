// Engine/Render/ShaderHotReload.cpp
// Shader 热重载实现 — FileWatcher + ShaderCompiler
#include "ShaderHotReload.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#include <cstdio>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iterator>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace he::render {

ShaderHotReload::ShaderHotReload() = default;
ShaderHotReload::~ShaderHotReload() { Stop(); }

void ShaderHotReload::Start(StringView shaderDir, StringView slangcPath,
                             ReloadCallback onReload) {
    m_SlangcPath = String(slangcPath);
    m_IncludeDir = String(shaderDir);
    m_OnReload   = std::move(onReload);

    // 校验目录是否存在（避免线程静默失败）
    namespace fs = std::filesystem;
    String absPath = fs::absolute(fs::path(String(shaderDir))).string();
    if (!fs::exists(absPath)) {
        HE_CORE_ERROR("[HotReload] Shader 目录不存在: {} (解析为: {})", shaderDir, absPath);
        return;
    }
    // 使用解析后的绝对路径，确保线程中 CreateFileA 成功
    HE_CORE_INFO("[HotReload] 启动文件监控: {} → {}", shaderDir, absPath);

    m_Running    = true;
    m_WatchThread = std::thread(&ShaderHotReload::WatchThread, this, absPath);
}

void ShaderHotReload::Stop() {
    m_Running = false;
    if (m_WatchThread.joinable()) {
        m_WatchThread.join();
    }
}

void ShaderHotReload::Poll() {
    std::vector<std::pair<String, std::vector<u32>>> pending;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        pending.swap(m_Pending);
    }

    for (auto& [shaderName, spirv] : pending) {
        if (m_OnReload && !spirv.empty()) {
            HE_CORE_INFO("[HotReload] 重载 Shader: {}", shaderName);
            m_OnReload(shaderName, spirv);
        }
    }
}

// 根据文件扩展名推断 shader 类型
bool ShaderHotReload::InferShaderType(StringView filename,
                                       String& outStage, String& outEntry) {
    // 例如 "PBR.frag.slang" → stage=fragment, entry=fragmentMain
    if (filename.find(".vert.slang") != StringView::npos) {
        outStage = "vertex"; outEntry = "vertexMain"; return true;
    }
    if (filename.find(".frag.slang") != StringView::npos) {
        outStage = "fragment"; outEntry = "fragmentMain"; return true;
    }
    if (filename.find(".comp.slang") != StringView::npos) {
        outStage = "compute"; outEntry = "main"; return true;
    }
    return false;
}

// 文件监控线程 — Windows ReadDirectoryChangesW + 200ms debounce
void ShaderHotReload::WatchThread(StringView shaderDir) {
    namespace fs = std::filesystem;
    String dir(shaderDir);

    // 打开目录句柄
    HANDLE hDir = CreateFileA(
        dir.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (hDir == INVALID_HANDLE_VALUE) {
        HE_CORE_ERROR("[HotReload] 无法打开监控目录: {}", dir);
        return;
    }

    // 创建同步事件（用于 WaitForSingleObject 等待异步 IO 完成）
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hEvent) {
        HE_CORE_ERROR("[HotReload] 无法创建监控事件");
        CloseHandle(hDir);
        return;
    }

    static const u32 kBufSize = 4096;
    u8 buffer[kBufSize];

    // 记录上次变更时间，用于 200ms debounce
    std::unordered_map<String, std::chrono::steady_clock::time_point> lastChange;

    while (m_Running) {
        // 每次循环前重置事件（手动重置事件在 ReadDirectoryChangesW 完成后保持 signaled，
        // 必须重置才能安全复用）
        ResetEvent(hEvent);

        DWORD bytesReturned = 0;
        OVERLAPPED overlapped{};
        overlapped.hEvent = hEvent;

        BOOL ok = ReadDirectoryChangesW(
            hDir, buffer, kBufSize, FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
            &bytesReturned, &overlapped, nullptr);

        if (!ok) {
            HE_CORE_ERROR("[HotReload] ReadDirectoryChangesW 失败");
            break;
        }

        // 最多等待 500ms（便于检查 m_Running 退出标志）
        DWORD waitResult = WaitForSingleObject(hEvent, 500);
        auto now = std::chrono::steady_clock::now();

        // 解析文件变更通知（可能包含多个文件）
        if (waitResult == WAIT_OBJECT_0) {
            DWORD cb = 0;
            if (!GetOverlappedResult(hDir, &overlapped, &cb, FALSE)) break;
            if (cb > 0) {
                auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
                for (;;) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, info->FileName,
                        info->FileNameLength / 2, nullptr, 0, nullptr, nullptr);
                    String filename(len, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, info->FileName,
                        info->FileNameLength / 2, &filename[0], len, nullptr, nullptr);
                    if (filename.find(".slang") != String::npos) {
                        lastChange[filename] = now;
                    }
                    if (info->NextEntryOffset == 0) break;
                    info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                        reinterpret_cast<u8*>(info) + info->NextEntryOffset);
                }
            }
        } else if (waitResult != WAIT_TIMEOUT) {
            break;
        }

        // 200ms debounce: 每次循环都检查（超时或变更通知都触发），确保累积变更最终被处理
        for (auto it = lastChange.begin(); it != lastChange.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second).count();
            if (elapsed >= 200) {
                String filename = it->first;
                it = lastChange.erase(it);

                String stage, entry;
                if (!InferShaderType(filename, stage, entry)) continue;

                HE_CORE_INFO("[HotReload] 检测到变更: {}", filename);
                auto slPath = fs::path(dir) / filename;
                std::vector<u32> spirv;
                if (CompileShader(m_SlangcPath, slPath.string(),
                                  stage, entry, m_IncludeDir, spirv)) {
                    String shaderName = fs::path(filename).stem().string();
                    {
                        std::lock_guard<std::mutex> lock(m_Mutex);
                        m_Pending.push_back({shaderName, std::move(spirv)});
                    }
                }
            } else {
                ++it;
            }
        }
    }

    CloseHandle(hEvent);
    CloseHandle(hDir);
    HE_CORE_INFO("[HotReload] 文件监控线程退出");
}

// 调用 slangc 编译单个 Shader
bool ShaderHotReload::CompileShader(StringView slangcPath, StringView slFile,
                                     StringView stage, StringView entryPoint,
                                     StringView includeDir,
                                     std::vector<u32>& outSpirv) {
    auto startTime = std::chrono::steady_clock::now();

    // 临时输出文件
    auto tmpSpv = std::filesystem::temp_directory_path() / "hug_shader_temp.spv";

    // 构建命令行（Windows: 第一个参数是程序名，不需要引号包裹；其余参数正常传递）
    // slangc 使用 "-" 前缀的参数格式，不需要 shell 特殊处理
    String cmdLine;
    cmdLine += "\"" + String(slangcPath) + "\"";
    cmdLine += " \"" + String(slFile) + "\"";
    cmdLine += " -target spirv";
    cmdLine += " -entry " + String(entryPoint);
    cmdLine += " -stage " + String(stage);
    cmdLine += " -I \"" + String(includeDir) + "\"";
    cmdLine += " -o \"" + tmpSpv.string() + "\"";
    cmdLine += " -Wno-39001";

    // 使用 CreateProcess 直接调用，避免 cmd.exe 引号转义问题
    // 创建临时 stderr 文件捕获错误输出
    auto tmpErr = std::filesystem::temp_directory_path() / "hug_shader_temp.err";
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hErrFile = CreateFileA(tmpErr.string().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdError = hErrFile;

    PROCESS_INFORMATION pi{};
    // 使用可修改的缓冲区（CreateProcessA 的命令行参数需要非 const）
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    BOOL created = CreateProcessA(
        nullptr,                // lpApplicationName（从命令行第一个 token 解析）
        cmdBuf.data(),          // lpCommandLine（可修改的缓冲区）
        nullptr, nullptr, TRUE, // 继承句柄
        0, nullptr, nullptr, &si, &pi);

    if (hErrFile) CloseHandle(hErrFile);

    if (!created) {
        HE_CORE_ERROR("[HotReload] 无法启动 slangc (错误码: {}):\n  cmd: {}",
                      GetLastError(), cmdLine);
        return false;
    }

    // 等待编译完成（最多 30 秒）
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();

    // 检查输出文件是否存在
    if (!std::filesystem::exists(tmpSpv) || std::filesystem::file_size(tmpSpv) == 0) {
        // 读取 stderr 获取错误信息
        String errOutput;
        std::ifstream errFile(tmpErr.string());
        if (errFile) {
            errOutput.assign(std::istreambuf_iterator<char>(errFile),
                            std::istreambuf_iterator<char>());
        }
        HE_CORE_ERROR("[HotReload] 编译失败 ({}ms):\n{}", elapsed,
                      errOutput.empty() ? "(无错误输出)" : errOutput);
        std::filesystem::remove(tmpErr);
        return false;
    }

    // 清理错误文件（编译成功时为空）
    std::filesystem::remove(tmpErr);

    // 从临时 .spv 加载 SPIR-V
    if (!LoadSpirv(tmpSpv.string(), outSpirv)) {
        HE_CORE_ERROR("[HotReload] 无法读取编译产物: {}", tmpSpv.string());
        return false;
    }

    HE_CORE_INFO("[HotReload] 编译成功: {} ({}ms, {} bytes)",
                 slFile, elapsed, outSpirv.size() * 4);
    std::filesystem::remove(tmpSpv);
    return true;
}

// 从 .spv 二进制文件读取 SPIR-V 字节码
bool ShaderHotReload::LoadSpirv(StringView spvPath, std::vector<u32>& outSpirv) {
    std::ifstream file(String(spvPath), std::ios::binary | std::ios::ate);
    if (!file) return false;

    usize fileSize = static_cast<usize>(file.tellg());
    file.seekg(0, std::ios::beg);

    outSpirv.resize(fileSize / sizeof(u32));
    file.read(reinterpret_cast<char*>(outSpirv.data()), fileSize);
    return file.good();
}

} // namespace he::render
