# Shader Hot Reload 设计文档

> **日期**: 2026-07-06
> **状态**: 设计完成，待实现
> **关联**: [HugEngine_Architecture_And_Tasks.md](../HugEngine_Architecture_And_Tasks.md)

---

## 1. 概述

### 1.1 目标

在 Editor 中修改 `.slang` Shader 文件后，保存即自动：重编译为 SPIR-V → 重建受影响的 PSO → 替换到渲染管线，**无需重启应用**。

### 1.2 与非热重载的区别

```
当前:  修改 .slang → 关闭应用 → cmake --build → 重启应用 → 看到效果  (30-60s)
目标:  修改 .slang → 保存 → 立即看到效果  (<500ms)
```

---

## 2. 架构

```
┌─────────────────────────────────────────────────────────┐
│  EditorApp                                              │
│    └─ ShaderHotReload (新增)                            │
│         ├─ FileWatcher     ← 监控 .slang 文件变化        │
│         ├─ ShaderCompiler  ← 调用 slangc 重编译          │
│         └─ 主线程回调      ← 线程安全的事件投递           │
│                                                          │
│  IRenderPipeline (改动)                                  │
│    └─ ReloadShader(name, bytecode)  ← PSO 热替换接口      │
│         └─ ForwardPipeline 实现                           │
│                                                          │
│  RHI (不改动)                                            │
│    └─ CreatePipelineState(desc)    ← 复用现有接口          │
└─────────────────────────────────────────────────────────┘
```

### 2.1 工作流程

```
.slang 变化 → FileWatcher 检测 → 200ms debounce
  → ShaderCompiler 调用 slangc → 生成临时 .spv
  → 读取 SPIR-V 二进制 → 主线程回调
  → pipeline->ReloadShader(name, spirv)
    → 查找受影响 PSO → 更新 desc → CreatePipelineState → 替换 PSO
  → 日志输出耗时
```

### 2.2 新增文件

| 文件 | 职责 |
|------|------|
| `Engine/Render/ShaderHotReload.h` | ShaderHotReload 类声明 |
| `Engine/Render/ShaderHotReload.cpp` | FileWatcher + ShaderCompiler + 主逻辑 |

### 2.3 改动文件

| 文件 | 改动 |
|------|------|
| `IRenderPipeline.h` | 新增 `virtual int ReloadShader(StringView name, const std::vector<u32>& spirv)` |
| `ForwardPipeline.h/.cpp` | 实现 ReloadShader；新增 PSO→Shader 映射表 |
| `EditorApp.h/.cpp` | 创建 ShaderHotReload 实例 + 每帧 Poll |

---

## 3. FileWatcher — 文件监控

**实现**: Windows `ReadDirectoryChangesW`，后台线程 + 200ms debounce

```
FileWatcher
  ├─ Watch(path, callback)     ← 启动后台线程
  ├─ Stop()                    ← 停止监控
  └─ 内部:
       ├─ CreateFile + ReadDirectoryChangesW (阻塞等待)
       ├─ 200ms debounce 合并多次保存事件
       └─ 仅监控 .slang 扩展名
```

**依赖追踪**: `.slang` 公共头文件变更时，需要重编译所有 include 它的 Shader。策略：
- 如果变更是 `*.slang`（公共 include 文件）→ 重编译所有 Shader
- 如果变更是 `*.vert.slang` / `*.frag.slang` / `*.comp.slang` → 仅重编译该文件

---

## 4. ShaderCompiler — 运行时重编译

通过系统调用 `slangc` 命令行工具（不在引擎内链接 Slang 库）。

### 4.1 命令参数

| Shader 类型 | 示例 | stage | entryPoint |
|-------------|------|-------|------------|
| `.vert.slang` | PBR.vert.slang | vertex | vertexMain |
| `.frag.slang` | PBR.frag.slang | fragment | fragmentMain |
| `.comp.slang` | DDGI.comp.slang | compute | main |

```
slangc <file>.slang -target spirv -entry <entryPoint> -stage <stage>
       -I "<Shader/Shaders>" -o <temp.spv> -Wno-39001
```

### 4.2 错误处理

- `slangc` 编译失败 → 捕获 stderr → 输出到 Editor Console → 保留旧 PSO（不替换）
- `slangc` 未找到 → 日志警告 → 禁用热重载

---

## 5. PSO Registry — 管线集成

不做全局 Registry。每个管线在 `Initialize()` 末尾注册 PSO→Shader 映射。

### 5.1 ForwardPipeline 数据结构

```cpp
struct PSOEntry {
    rhi::IRHIPipelineState* pso;       // 指向现有 m_PBR_PSO 等
    String                  shaderName; // "PBR.frag"
    rhi::PipelineStateDesc  desc;       // 完整 PSO 创建参数
};
std::vector<PSOEntry> m_ShaderPSOs;
```

### 5.2 ReloadShader 流程

```
1. 遍历 m_ShaderPSOs，找到所有 shaderName 匹配的 entry
2. 对每个 entry:
   a. 根据 ShaderStage 更新 desc 中对应的 ShaderBytecode::spirv (新字节码)
   b. device->CreatePipelineState(desc) → 新 PSO
   c. 替换旧 unique_ptr
   d. 如果是当前绑定的 PSO(与 m_CurrentBindPSO 比较)，调用 cmdList->SetPipeline(新PSO)
3. 输出日志: "Reloaded PBR.frag → N PSOs rebuilt in Xms"
4. 返回受影响的 PSO 数量
```

### 5.3 IRenderPipeline 接口

```cpp
// 热重载单个 Shader
// shaderName: "PBR.frag" (不含路径和扩展)
// newSpirv: 新编译的 SPIR-V 字节码
// 返回受影响的 PSO 数量，0 表示未找到，-1 表示失败
virtual int ReloadShader(StringView shaderName,
                         const std::vector<u32>& newSpirv) { return -1; }
```

---

## 6. 验证方案

### 6.1 测试步骤

1. 启动 HugEditor
2. 打开 `Engine/Shader/Shaders/PBR.frag.slang`
3. 找到 `float4(1,0,0,1)` → 保存
4. **预期**: Editor 视口中 PBR 材质立即变红
5. Console 输出: `[HotReload] PBR.frag changed → recompiled (12ms) → 1 PSO rebuilt in 3ms`

### 6.2 故障排查

| 症状 | 检查点 |
|------|--------|
| 没有任何日志 | FileWatcher 是否启动 → 监控路径是否正确 → 线程是否正常运行 |
| 有变更日志但无重载 | ShaderCompiler 是否找到 slangc → 编译是否成功 → shaderName 是否匹配 |
| PSO 替换了但无视觉变化 | 新 PSO 是否正确 → SetPipeline 是否调用 → RenderPass 是否使用了缓存的旧 PSO |
| 编译失败 | Console 中查看 slangc 错误输出 → 语法问题 → include 路径问题 |

---

## 7. 实现步骤

| 步骤 | 内容 | 预估 |
|------|------|:---:|
| Step 1 | FileWatcher 类 (ReadDirectoryChangesW + 200ms debounce + 线程) | 0.5d |
| Step 2 | ShaderCompiler 类 (CreateProcess 调用 slangc → 解析 SPIR-V) | 0.5d |
| Step 3 | ShaderHotReload 类 (组合 FileWatcher + ShaderCompiler + 主线程回调) | 0.5d |
| Step 4 | IRenderPipeline::ReloadShader 接口 + ForwardPipeline 实现 | 0.5d |
| Step 5 | EditorApp 集成 (创建 + Poll + 销毁) | 0.5d |
| Step 6 | 端到端测试验证 | 0.5d |

**总计约 3 个工作日**。

---

> **文档版本**: v1.0
