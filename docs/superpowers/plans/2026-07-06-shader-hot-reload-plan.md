# Shader Hot Reload 实现计划

> **for agentic workers:** 使用 superpowers:subagent-driven-development 按任务逐步实现。步骤使用 checkbox (`- [ ]`) 语法追踪。

**目标**: 在 HugEditor 中修改 `.slang` 文件后保存→自动重编译→PSO 热替换，无需重启

**架构**: Editor 层 `ShaderHotReload` 类 (FileWatcher + ShaderCompiler) + `IRenderPipeline::ReloadShader` 接口 + ForwardPipeline 实现

**技术栈**: Windows `ReadDirectoryChangesW`, slangc 命令行, SPIR-V, Vulkan PSO 重建

## 全局约束

- C++17，命名空间 `he::render`
- Vulkan 1.3 后端
- 仅 Windows 平台（FileWatcher 使用 ReadDirectoryChangesW）
- Shader 源文件路径: `Engine/Shader/Shaders/`
- 遵守项目规范：中文注释、不自动 commit
- 每个 Task commit 一次

---

### Task 1: ShaderHotReload 类骨架 + FileWatcher

**文件:**
- 创建: `Engine/Render/ShaderHotReload.h`
- 创建: `Engine/Render/ShaderHotReload.cpp`
- 改动: `Engine/Render/CMakeLists.txt` — 添加新文件

**接口:**
- 产出: `class ShaderHotReload { void Start(...); void Poll(); void Stop(); }`

- [ ] **Step 1: 创建 ShaderHotReload.h 头文件**

```cpp
// Engine/Render/ShaderHotReload.h
// Shader 热重载 — 监控 .slang 文件变化 → 自动重编译 → PSO 热替换
#pragma once

#include "Core/Types.h"
#include <atomic>
#include <functional>
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
```

- [ ] **Step 2: 创建 ShaderHotReload.cpp — FileWatcher + ShaderCompiler**

```cpp
// Engine/Render/ShaderHotReload.cpp
#include "ShaderHotReload.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#include <filesystem>
#include <chrono>

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
    m_Running    = true;
    m_WatchThread = std::thread(&ShaderHotReload::WatchThread, this, String(shaderDir));
    HE_CORE_INFO("[HotReload] 启动文件监控: {}", shaderDir);
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

    static const u32 kBufSize = 4096;
    u8 buffer[kBufSize];
    OVERLAPPED overlapped{};

    // 记录上次变更时间，用于 200ms debounce
    std::unordered_map<String, std::chrono::steady_clock::time_point> lastChange;

    while (m_Running) {
        overlapped = OVERLAPPED{};
        DWORD bytesReturned = 0;

        BOOL ok = ReadDirectoryChangesW(
            hDir, buffer, kBufSize, FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
            &bytesReturned, &overlapped, nullptr);

        if (!ok) {
            HE_CORE_ERROR("[HotReload] ReadDirectoryChangesW 失败");
            break;
        }

        // 最多等待 500ms（便于检查 m_Running 退出标志）
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 500);
        if (waitResult == WAIT_TIMEOUT) continue;  // 检查退出标志
        if (waitResult != WAIT_OBJECT_0) break;

        DWORD cb = 0;
        if (!GetOverlappedResult(hDir, &overlapped, &cb, FALSE)) break;
        if (cb == 0) continue;

        auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
        auto now = std::chrono::steady_clock::now();

        for (;;) {
            // 宽字符 → UTF-8
            int len = WideCharToMultiByte(CP_UTF8, 0, info->FileName,
                                          info->FileNameLength / 2, nullptr, 0, nullptr, nullptr);
            String filename(len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, info->FileName,
                                info->FileNameLength / 2, &filename[0], len, nullptr, nullptr);

            // 只处理 .slang 文件
            if (filename.find(".slang") != String::npos) {
                lastChange[filename] = now;
            }

            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<u8*>(info) + info->NextEntryOffset);
        }

        // 200ms debounce: 检查哪些文件已稳定
        for (auto it = lastChange.begin(); it != lastChange.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second).count();
            if (elapsed >= 200) {
                String filename = it->first;
                it = lastChange.erase(it);

                String stage, entry;
                if (!InferShaderType(filename, stage, entry)) continue;

                HE_CORE_INFO("[HotReload] 检测到变更: {}", filename);

                // 调用 slangc 重编译
                auto slPath = fs::path(dir) / filename;
                std::vector<u32> spirv;
                if (CompileShader(m_SlangcPath, slPath.string(),
                                  stage, entry, m_IncludeDir, spirv)) {
                    // 提取 shader 名（如 "PBR.frag"）
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

    CloseHandle(hDir);
    HE_CORE_INFO("[HotReload] 文件监控线程退出");
}

// 调用 slangc 编译单个 Shader
bool ShaderHotReload::CompileShader(StringView slangcPath, StringView slFile,
                                     StringView stage, StringView entryPoint,
                                     StringView includeDir,
                                     std::vector<u32>& outSpirv) {
    auto startTime = std::chrono::steady_clock::now();

    // 构建命令行: slangc file.slang -target spirv -entry main -stage compute -I dir -o out.spv
    // 输出到临时文件
    auto tmpSpv = std::filesystem::temp_directory_path() / "hug_shader_temp.spv";

    String cmd;
    cmd += "\"" + String(slangcPath) + "\"";
    cmd += " \"" + String(slFile) + "\"";
    cmd += " -target spirv";
    cmd += " -entry " + String(entryPoint);
    cmd += " -stage " + String(stage);
    cmd += " -I \"" + String(includeDir) + "\"";
    cmd += " -o \"" + tmpSpv.string() + "\"";
    cmd += " -Wno-39001 2>&1";

    // 执行编译
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        HE_CORE_ERROR("[HotReload] 无法启动 slangc: {}", cmd);
        return false;
    }

    // 读取输出
    String output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    int ret = _pclose(pipe);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();

    if (ret != 0) {
        HE_CORE_ERROR("[HotReload] 编译失败 ({}ms):\n{}", elapsed, output);
        return false;
    }

    // 从临时 .spv 加载 SPIR-V
    if (!LoadSpirv(tmpSpv.string(), outSpirv)) {
        HE_CORE_ERROR("[HotReload] 无法读取编译产物: {}", tmpSpv.string());
        return false;
    }

    HE_CORE_INFO("[HotReload] 编译成功: {} ({}ms, {} bytes)",
                 slFile, elapsed, outSpirv.size() * 4);
    // 清理临时文件
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
```

- [ ] **Step 3: 更新 CMakeLists.txt**

在 `Engine/Render/CMakeLists.txt` 中添加 (在 `# ShaderHotReload` 注释后):

```cmake
    # ShaderHotReload
    ShaderHotReload.h
    ShaderHotReload.cpp
```

- [ ] **Step 4: 编译验证**

```bash
cd build && cmake --build . --config Debug
```
预期: 编译通过，HugEngineRender 库链接成功。

- [ ] **Step 5: Commit**

```bash
git add Engine/Render/ShaderHotReload.h Engine/Render/ShaderHotReload.cpp Engine/Render/CMakeLists.txt
git commit -m "Shader Hot Reload: FileWatcher + ShaderCompiler 实现

- FileWatcher: Windows ReadDirectoryChangesW + 200ms debounce
- ShaderCompiler: _popen 调用 slangc 运行时重编译 → 读取 .spv
- 线程安全: 监控线程入队 → 主线程 Poll 消费"
```

---

### Task 2: IRenderPipeline::ReloadShader 接口 + ForwardPipeline 实现

**文件:**
- 改动: `Engine/Render/Pipeline/IRenderPipeline.h` — 新增虚方法
- 改动: `Engine/Render/Pipeline/ForwardPipeline.h` — PSO 注册表 + ReloadShader 声明
- 改动: `Engine/Render/Pipeline/ForwardPipeline.cpp` — 实现 ReloadShader

**接口:**
- 消费: ShaderHotReload::ReloadCallback (来自 Task 1)
- 产出: `virtual int ReloadShader(StringView, const std::vector<u32>&)`

- [ ] **Step 1: 在 IRenderPipeline.h 添加 ReloadShader 虚方法**

在 `Engine/Render/Pipeline/IRenderPipeline.h` 的 `GetGI()` 方法后面添加:

```cpp
    // ---- Shader 热重载 ----

    /// 热重载单个 Shader（传入新编译的 SPIR-V 字节码）
    /// @param shaderName 文件名（不含路径和扩展名，如 "PBR.frag"）
    /// @param newSpirv   新编译的 SPIR-V 字节码
    /// @return 受影响并已替换的 PSO 数量，0=未找到匹配, -1=失败
    virtual int ReloadShader(StringView shaderName,
                             const std::vector<u32>& newSpirv) { return -1; }
```

- [ ] **Step 2: 在 ForwardPipeline.h 添加 PSO 注册表**

在 `ForwardPipeline.h` 的 private 区域末尾 (在 `m_LastTriCount` 之后) 添加:

```cpp
    // Shader 热重载 — PSO 注册表
    struct PSORecord {
        rhi::PipelineStateDesc  desc;              // 完整 PSO 创建参数（含 ShaderBytecode 副本）
        rhi::ShaderBytecode     vsCopy;            // 顶点着色器字节码副本（自有 spirv 数据）
        rhi::ShaderBytecode     fsCopy;            // 片元着色器字节码副本
        String                  shaderNames[2];    // [0]=顶点shader名, [1]=片元shader名（如 "PBR.vert", "PBR.frag"）
        rhi::IRHIPipelineState* rawPSO = nullptr;  // 指向 m_PBR_PSO.get()
    };
    std::vector<PSORecord> m_PSORegistry;
```

- [ ] **Step 3: 在 ForwardPipeline::Initialize() 末尾注册 PSO**

在 `Engine/Render/Pipeline/ForwardPipeline.cpp` 的 `Initialize()` 函数末尾 (`m_Ready = true;` 之后) 添加:

```cpp
    // 注册 PSO 到热重载表
    {
        PSORecord rec;
        // 复制完整的 PSO 创建参数（含 ShaderBytecode 副本）
        rec.desc = psoDesc;  // psoDesc 来自第191行
        // ShaderBytecode 的 spirv 字段是 const std::vector<u32>&，
        // 需要复制到自有向量中以便后续替换
        rec.vsCopy.stage      = m_VS.stage;
        rec.vsCopy.spirv      = m_VS.spirv;  // 初始值，ReloadShader 中会被替换
        rec.vsCopy.entryPoint = m_VS.entryPoint;
        rec.fsCopy.stage      = m_FS.stage;
        rec.fsCopy.spirv      = m_FS.spirv;
        rec.fsCopy.entryPoint = m_FS.entryPoint;
        // 让 desc 指向副本（后续可以修改副本中的 spirv 数据）
        rec.desc.vertexShader = &rec.vsCopy;
        rec.desc.pixelShader  = &rec.fsCopy;
        rec.shaderNames[0]    = "PBR.vert";   // 顶点shader名称
        rec.shaderNames[1]    = "PBR.frag";   // 片元shader名称
        rec.rawPSO            = m_PBR_PSO.get();
        m_PSORegistry.push_back(std::move(rec));
        HE_CORE_INFO("[HotReload] PSO 注册: PBR (vert + frag)");
    }
```

注意: `psoDesc` 需要从局部变量提升或在此处重新构建。由于 `psoDesc` 定义在 Initialize 中间位置，这里需要捕获。最简单方式：将 `psoDesc` 的定义移到函数顶部，或在此处重建它。**推荐重新构建**以确保完整性:

```cpp
    // 注册 PBR PSO 到热重载表
    {
        PSORecord rec;
        // 重建 PSO 描述符（与初始化时创建 m_PBR_PSO 的参数完全一致）
        rec.desc.debugName            = "ForwardPBR";
        rec.desc.vertexLayout         = vertexLayout;
        rec.desc.topology             = rhi::PrimitiveTopology::TriangleList;
        rec.desc.depthTest            = true;
        rec.desc.depthWrite           = true;
        rec.desc.depthCompare         = rhi::CompareFunc::LessEqual;
        rec.desc.depthFormat          = rhi::Format::D32_FLOAT;
        rec.desc.colorAttachmentCount = 1;
        rec.desc.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
        rec.desc.pushConstantRanges   = { pcRange };
        rec.desc.descriptorSetLayouts = { m_PerFrameLayout };
        // Shader 副本（自有 spirv 数据，ReloadShader 中会被替换）
        rec.vsCopy.stage      = rhi::ShaderStage::Vertex;
        rec.vsCopy.spirv      = m_VS.spirv;
        rec.vsCopy.entryPoint = "main";
        rec.fsCopy.stage      = rhi::ShaderStage::Pixel;
        rec.fsCopy.spirv      = m_FS.spirv;
        rec.fsCopy.entryPoint = "main";
        rec.desc.vertexShader = &rec.vsCopy;
        rec.desc.pixelShader  = &rec.fsCopy;
        rec.shaderNames[0]    = "PBR.vert";
        rec.shaderNames[1]    = "PBR.frag";
        rec.rawPSO            = m_PBR_PSO.get();
        m_PSORegistry.push_back(std::move(rec));
        HE_CORE_INFO("[HotReload] PSO 注册: PBR (vert + frag)");
    }
```

- [ ] **Step 4: 实现 ForwardPipeline::ReloadShader**

在 `Engine/Render/Pipeline/ForwardPipeline.cpp` 中添加:

```cpp
// === Shader 热重载 ===

int ForwardPipeline::ReloadShader(StringView shaderName,
                                   const std::vector<u32>& newSpirv) {
    auto startTime = std::chrono::steady_clock::now();
    int count = 0;

    for (auto& rec : m_PSORegistry) {
        // 检查该 PSO 是否使用了这个 Shader
        int stageIndex = -1;
        if (rec.shaderNames[0] == shaderName) stageIndex = 0;  // 顶点shader
        if (rec.shaderNames[1] == shaderName) stageIndex = 1;  // 片元shader

        if (stageIndex < 0) continue;  // 不匹配，跳过

        // 更新 Shader 字节码
        if (stageIndex == 0) {
            rec.vsCopy.spirv = newSpirv;
        } else {
            rec.fsCopy.spirv = newSpirv;
        }

        // 重建 PSO
        auto newPSO = m_Device->CreatePipelineState(rec.desc);
        if (!newPSO) {
            HE_CORE_ERROR("[HotReload] PSO 重建失败: {}", shaderName);
            continue;
        }

        // 替换旧 PSO
        // rec.rawPSO 指向的原 unique_ptr 会被自动释放
        // 需要找到对应的 unique_ptr 并替换
        // （当前只有 m_PBR_PSO，直接替换）
        m_PBR_PSO = std::move(newPSO);
        rec.rawPSO = m_PBR_PSO.get();

        count++;
        HE_CORE_INFO("[HotReload] PSO 替换成功: {} → PBR", shaderName);
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    HE_CORE_INFO("[HotReload] {} 个 PSO 重建完成 ({}ms)", count, elapsed);

    return count > 0 ? count : 0;
}
```

- [ ] **Step 5: 编译验证**

```bash
cd build && cmake --build . --config Debug
```
预期: 编译通过，无错误。

- [ ] **Step 6: Commit**

```bash
git add Engine/Render/Pipeline/IRenderPipeline.h Engine/Render/Pipeline/ForwardPipeline.h Engine/Render/Pipeline/ForwardPipeline.cpp
git commit -m "Shader Hot Reload: IRenderPipeline::ReloadShader 接口 + ForwardPipeline PSO 注册表实现

- IRenderPipeline 新增虚方法 ReloadShader(shaderName, spirv)
- ForwardPipeline 维护 PSORecord 表（desc副本 + ShaderBytecode副本）
- ReloadShader: 匹配 shaderName → 更新 spirv → 重建 PSO → 替换 m_PBR_PSO"
```

---

### Task 3: EditorApp 集成

**文件:**
- 改动: `Samples/Editor/EditorApp.h` — 新增 ShaderHotReload 成员
- 改动: `Samples/Editor/EditorApp.cpp` — 初始化/销毁/每帧Poll

**接口:**
- 消费: `ShaderHotReload::Start()`, `Poll()`, `Stop()` (来自 Task 1)
- 消费: `IRenderPipeline::ReloadShader()` (来自 Task 2)

- [ ] **Step 1: 在 EditorApp.h 添加成员**

在 `EditorApp.h` 顶部 include 区域添加前向声明，在 private 区域添加成员:

```cpp
// 前向声明区域新增:
namespace he::render { class ShaderHotReload; }

// private 区域 (m_StatsPanel 之后，m_LastTime 之前) 新增:
    std::unique_ptr<he::render::ShaderHotReload> m_ShaderHotReload;  // Shader 热重载
```

- [ ] **Step 2: 在 EditorApp.cpp 添加 include**

```cpp
#include "Render/ShaderHotReload.h"
```

- [ ] **Step 3: 在 InitPipeline() 末尾初始化 ShaderHotReload**

在 `EditorApp::InitPipeline()` 函数末尾 (`m_CmdList->SetPipeline(m_Pipeline->GetPipelineState());` 之后) 添加:

```cpp
    // --- Shader 热重载 ---
    m_ShaderHotReload = std::make_unique<he::render::ShaderHotReload>();
    // 获取 shader 源文件目录和 slangc 路径
    String shaderDir  = "../../Engine/Shader/Shaders";     // 相对于 build/bin/Debug
    String slangcPath = "slangc";                           // 依赖 PATH 环境变量
    m_ShaderHotReload->Start(shaderDir, slangcPath,
        [this](const String& shaderName, const std::vector<u32>& spirv) {
            if (m_Pipeline) {
                int n = m_Pipeline->ReloadShader(shaderName, spirv);
                if (n > 0) {
                    HE_CORE_INFO("[HotReload] {} → 成功重载 {} 个 PSO", shaderName, n);
                }
            }
        });
    HE_CORE_INFO("Shader Hot Reload 已启动");
```

- [ ] **Step 4: 在 MainLoop() 每帧调用 Poll**

在 `EditorApp::MainLoop()` 中，`PollEvents()` 之后添加:

```cpp
        // Shader 热重载 — 每帧处理待重载队列
        if (m_ShaderHotReload) {
            m_ShaderHotReload->Poll();
        }
```

> 放在 `PollEvents()` 之后、任何渲染之前，确保新 PSO 在渲染前生效。

- [ ] **Step 5: 在 Shutdown() 中停止热重载**

在 `EditorApp::Shutdown()` 函数开头 (在任何设备清理之前) 添加:

```cpp
    // 停止 Shader 热重载（必须在设备释放前停止）
    if (m_ShaderHotReload) {
        m_ShaderHotReload->Stop();
        m_ShaderHotReload.reset();
    }
```

- [ ] **Step 6: 更新 Editor CMakeLists.txt**

在 `Samples/Editor/CMakeLists.txt` 中添加 `HugEngineRender` 依赖（如果还没有）:

检查 `target_link_libraries(HugEditor ...)` 是否已包含 `HugEngineRender`。如果已包含，无需改动。`ShaderHotReload` 定义在 `HugEngineRender` 库中。

- [ ] **Step 7: 编译验证**

```bash
cd build && cmake --build . --config Debug --target HugEditor
```
预期: 编译通过，HugEditor.exe 链接成功。

- [ ] **Step 8: Commit**

```bash
git add Samples/Editor/EditorApp.h Samples/Editor/EditorApp.cpp
git commit -m "Shader Hot Reload: EditorApp 集成 — 启动监控 + 每帧 Poll + 停止

- InitPipeline: 创建 ShaderHotReload，监控 ../../Engine/Shader/Shaders
- MainLoop: 每帧 Poll() 处理待重载 Shader
- Shutdown: Stop() 释放监控线程"
```

---

### Task 4: 端到端验证

- [ ] **Step 1: 构建 HugEditor**

```bash
cd build && cmake --build . --config Debug --target HugEditor
```
预期: 编译成功。

- [ ] **Step 2: 启动 Editor，检查初始化日志**

```bash
cd build/bin/Debug && ./HugEditor.exe
```
预期 Console 输出:
```
[HotReload] 启动文件监控: ../../Engine/Shader/Shaders
[HugEngine] Shader Hot Reload 已启动
```

- [ ] **Step 3: 修改 PBR.frag.slang**

打开 `Engine/Shader/Shaders/PBR.frag.slang`，找到 fragmentMain 函数中最后的颜色输出，在 return 之前添加:

```hlsl
// 热重载测试: 屏幕变红
float3 testColor = float3(1.0, 0.0, 0.0);
finalColor.rgb = lerp(finalColor.rgb, testColor, 0.7);
```

保存文件。

- [ ] **Step 4: 观察 Console 和视口**

预期 Console 输出:
```
[HotReload] 检测到变更: PBR.frag.slang
[HotReload] 编译成功: .../PBR.frag.slang (XXms, XXXX bytes)
[HotReload] 重载 Shader: PBR.frag
[HotReload] PSO 替换成功: PBR.frag → PBR
[HotReload] 1 个 PSO 重建完成 (Xms)
```

预期视觉: Editor 视口中 PBR 材质呈现明显偏红色调。

- [ ] **Step 5: 撤销测试改动**

将 `PBR.frag.slang` 中的测试代码移除，保存。预期视口恢复正常。

- [ ] **Step 6: Commit (验证通过后更新文档)**

```bash
git add docs/superpowers/specs/2026-07-06-shader-hot-reload-design.md
git commit -m "Shader Hot Reload 设计文档: 更新验证结果"
```

---

### Task 5 (可选): DeferredPipeline + 多个 PSO 扩展

> 仅当 ForwardPipeline 验证通过后才实施。

- 在 `DeferredPipeline` 实现 `ReloadShader()`
- 注册 GBuffer、Lighting、SSAO、SSGI、SSR、DDGI、Denoiser、Bloom、DOF 等 PSO
- 扩展 DeferredPipeline PSORecord 到多个 unique_ptr 的映射

---

### 故障排查指南

| 症状 | 检查点 | 调试命令 |
|------|--------|---------|
| "无法打开监控目录" | shaderDir 路径是否正确 | 检查 `../../Engine/Shader/Shaders` 相对路径 |
| 无检测日志 | FileWatcher 线程是否启动 | 查看 `[HotReload] 启动文件监控` 日志 |
| 编译失败 | slangc 是否在 PATH 中 | 运行 `slangc --version` |
| 编译失败 + stderr 乱码 | include 路径是否正确 | 检查 `-I` 参数指向的目录 |
| PSO 重建失败 | SPIR-V 是否与 PSO 兼容 | 检查 shader stage / entry point 是否匹配 |
| 视口无变化 | 新 PSO 是否被正确绑定 | 检查 `m_PBR_PSO` 是否被替换 |
| 崩溃 | PSO 替换时机的线程安全 | 确保 Poll() 在主线程调用 |
