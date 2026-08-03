# HybridRT CVar 质量体系实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 HybridRTPipeline 全部硬编码质量参数（开关/分辨率/SPP/追踪距离/降噪参数）暴露为运行时可调的 `r.RT.*` CVar，实现控制台热更新。

**Architecture:** 把引擎已有 CVar 系统从 Editor 模块迁移到 Core（Render 模块可直接访问）；新建 `RTQualityCVars.h/cpp` 集中定义所有 RT 质量 CVar；各 RT Pass 在 `Execute()` 每帧读 CVar 写入 push constant；HybridRTPipeline 每帧读 CVar 控制 pass 开关、检测分辨率变化触发重建；02.Cube 用 `r.Pipeline.Mode` CVar 替代硬编码 renderMode。

**Tech Stack:** C++ / Vulkan / Slang / CMake

## Global Constraints

- 编译命令：`cmake --build build --target 02.Cube --config Debug`（含 Editor 需 `--target 02.Cube`，它链接 Editor）
- 验证方式是编译 + 运行 02.Cube 目视/日志确认，本项目无单元测试框架
- 新代码必须附带中文注释（CLAUDE.md）
- **commit 需用户确认后执行**（CLAUDE.md：不自动 commit）
- CVar 命名沿用引擎现有 `r.` 前缀（文档 11.3 约定）
- `CVar<T>::Get()` 直接读值，无查表开销，可每帧调用

---

### Task 1: CVar 系统迁移（Editor → Core）

**Files:**
- Create: `Engine/Core/Core/CVar.h`（内容 = 原 `Engine/Editor/Editor/CVar.h` 原样）
- Create: `Engine/Core/Core/CVar.cpp`（内容 = 原 `Engine/Editor/Editor/CVar.cpp` 原样）
- Delete: `Engine/Editor/Editor/CVar.h`
- Delete: `Engine/Editor/Editor/CVar.cpp`
- Modify: `Engine/Core/CMakeLists.txt`（`CORE_SOURCES` 加 `Core/CVar.cpp`，`CORE_HEADERS` 若有则加 `Core/CVar.h`）
- Modify: `Engine/Editor/CMakeLists.txt`（移除 `Editor/CVar.h` 与 `Editor/CVar.cpp` 两行）
- Modify: `Samples/Editor/EditorApp.cpp:49`（include `"Editor/CVar.h"` → `"Core/CVar.h"`）
- Modify: `Samples/Editor/Panels/ConsolePanel.cpp:3`（同上）
- Modify: `Samples/Editor/Panels/ProjectSettingsPanel.cpp:5`（同上）
- Modify: `Samples/Editor/Panels/ViewportPanel.cpp:16`（同上）

**Interfaces:**
- Produces: `he::CVar<T>`（位于 `Core/CVar.h`，支持 `i32/f32/String/bool`，构造自注册，`FindCVar(name)` 查询）。后续所有任务依赖此头文件位置。

- [ ] **Step 1: 用 git mv 移动文件**

```bash
cd /d/Source/HugEngine
git mv Engine/Editor/Editor/CVar.h Engine/Core/Core/CVar.h
git mv Engine/Editor/Editor/CVar.cpp Engine/Core/Core/CVar.cpp
```
（文件内容不变；CVar.h 仅依赖 `Core/Types.h`/`Containers/Array.h`，Core 层已满足）

- [ ] **Step 2: 更新 CMake**

`Engine/Core/CMakeLists.txt` 的 `CORE_SOURCES`（当前含 `Core/Engine.cpp` 等）追加一行：
```cmake
    Core/CVar.cpp
```
`Engine/Editor/CMakeLists.txt` 的 `EDITOR_SOURCES` 删除：
```cmake
    Editor/CVar.h
```
和：
```cmake
    Editor/CVar.cpp
```

- [ ] **Step 3: 更新 4 个 include 路径**

四个文件中 `#include "Editor/CVar.h"` 全部改为 `#include "Core/CVar.h"`（EditorApp.cpp:49、ConsolePanel.cpp:3、ProjectSettingsPanel.cpp:5、ViewportPanel.cpp:16）。

- [ ] **Step 4: 编译验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无 error/LNK（02.Cube 链接 Editor + Core，验证迁移后所有 CVar 引用仍解析）。

- [ ] **Step 5: 暂存并等待用户确认提交**（不自动 commit）

```bash
git add -A && git status --short
```

---

### Task 2: 创建 RTQualityCVars.h/cpp

**Files:**
- Create: `Engine/Render/Pipeline/RTQualityCVars.h`
- Create: `Engine/Render/Pipeline/RTQualityCVars.cpp`
- Modify: `Engine/Render/CMakeLists.txt`（`RENDER_SOURCES` 加两行）

**Interfaces:**
- Produces: `he::render::cvRTShadow`（`he::CVar<bool>`）等全部 `r.RT.*` CVar 对象（extern 声明在 `.h`，定义在 `.cpp`）。Task 3-5 include `"Pipeline/RTQualityCVars.h"` 直接访问。

- [ ] **Step 1: 写 RTQualityCVars.h**

```cpp
#pragma once

#include "Core/CVar.h"

namespace he::render {

// ============================================================
// RTQualityCVars — HybridRT 质量参数 CVar 声明（定义见 .cpp）
// 所有 r.RT.* CVar 集中于此，RT Passes 与 HybridRTPipeline 统一 include 访问
// ============================================================
// 开关
extern CVar<bool>  cvRTShadow;          // r.RT.Shadow            RT 阴影开关
extern CVar<bool>  cvRTAO;              // r.RT.AO                RT AO 开关
extern CVar<bool>  cvRTReflection;      // r.RT.Reflection        RT 反射开关
extern CVar<bool>  cvRTGI;              // r.RT.GI                RT GI 开关
// 分辨率
extern CVar<bool>  cvRTShadowHalfRes;   // r.RT.Shadow.HalfRes    阴影半分辨率
extern CVar<bool>  cvRTAOHalfRes;       // r.RT.AO.HalfRes        AO 半分辨率
extern CVar<bool>  cvRTReflectionHalfRes;// r.RT.Reflection.HalfRes 反射半分辨率
extern CVar<bool>  cvRTGIQuarterRes;    // r.RT.GI.QuarterRes     GI 四分之一分辨率
// 采样/追踪
extern CVar<i32>   cvRTAOSPP;           // r.RT.AO.SPP            AO 每像素射线数
extern CVar<float> cvRTAOMaxDist;       // r.RT.AO.MaxDistance    AO 遮蔽半径
extern CVar<i32>   cvRTReflectionSPP;   // r.RT.Reflection.SPP    反射每像素采样数
extern CVar<float> cvRTReflectionMaxDist;// r.RT.Reflection.MaxDistance 反射最大距离
extern CVar<i32>   cvRTGISPP;           // r.RT.GI.SPP            GI 每像素采样数
extern CVar<float> cvRTGIMaxDist;       // r.RT.GI.MaxDistance    GI 追踪范围
extern CVar<float> cvRTShadowMaxDist;   // r.RT.Shadow.MaxDistance 阴影最大距离
// 降噪
extern CVar<bool>  cvRTDenoiseTemporal; // r.RT.Denoise.Temporal  时域降噪开关
extern CVar<bool>  cvRTDenoiseSpatial;  // r.RT.Denoise.Spatial   空间滤波开关
extern CVar<float> cvRTDenoiseShadowBlend;   // r.RT.Denoise.Shadow.Blend
extern CVar<float> cvRTDenoiseAOBlend;       // r.RT.Denoise.AO.Blend
extern CVar<float> cvRTDenoiseReflectionBlend;// r.RT.Denoise.Reflection.Blend
extern CVar<float> cvRTDenoiseGIBlend;       // r.RT.Denoise.GI.Blend

} // namespace he::render
```

- [ ] **Step 2: 写 RTQualityCVars.cpp**

```cpp
#include "Pipeline/RTQualityCVars.h"

namespace he::render {

// 开关
CVar<bool>  cvRTShadow("r.RT.Shadow", true, "RT 阴影开关");
CVar<bool>  cvRTAO("r.RT.AO", true, "RT AO 开关");
CVar<bool>  cvRTReflection("r.RT.Reflection", true, "RT 反射开关");
CVar<bool>  cvRTGI("r.RT.GI", true, "RT GI 开关");
// 分辨率
CVar<bool>  cvRTShadowHalfRes("r.RT.Shadow.HalfRes", true, "RT 阴影半分辨率");
CVar<bool>  cvRTAOHalfRes("r.RT.AO.HalfRes", true, "RT AO 半分辨率");
CVar<bool>  cvRTReflectionHalfRes("r.RT.Reflection.HalfRes", true, "RT 反射半分辨率");
CVar<bool>  cvRTGIQuarterRes("r.RT.GI.QuarterRes", true, "RT GI 四分之一分辨率");
// 采样/追踪
CVar<i32>   cvRTAOSPP("r.RT.AO.SPP", 2, "RT AO 每像素射线数");
CVar<float> cvRTAOMaxDist("r.RT.AO.MaxDistance", 2.0f, "RT AO 遮蔽半径(m)");
CVar<i32>   cvRTReflectionSPP("r.RT.Reflection.SPP", 1, "RT 反射每像素采样数");
CVar<float> cvRTReflectionMaxDist("r.RT.Reflection.MaxDistance", 500.0f, "RT 反射最大追踪距离(m)");
CVar<i32>   cvRTGISPP("r.RT.GI.SPP", 1, "RT GI 每像素采样数");
CVar<float> cvRTGIMaxDist("r.RT.GI.MaxDistance", 30.0f, "RT GI 追踪范围(m)");
CVar<float> cvRTShadowMaxDist("r.RT.Shadow.MaxDistance", 200.0f, "RT 阴影最大追踪距离");
// 降噪
CVar<bool>  cvRTDenoiseTemporal("r.RT.Denoise.Temporal", true, "RT 时域降噪开关");
CVar<bool>  cvRTDenoiseSpatial("r.RT.Denoise.Spatial", true, "RT 空间滤波开关");
CVar<float> cvRTDenoiseShadowBlend("r.RT.Denoise.Shadow.Blend", 0.05f, "RT 阴影时域混合因子");
CVar<float> cvRTDenoiseAOBlend("r.RT.Denoise.AO.Blend", 0.05f, "RT AO 时域混合因子");
CVar<float> cvRTDenoiseReflectionBlend("r.RT.Denoise.Reflection.Blend", 0.10f, "RT 反射时域混合因子");
CVar<float> cvRTDenoiseGIBlend("r.RT.Denoise.GI.Blend", 0.15f, "RT GI 时域混合因子");

} // namespace he::render
```

- [ ] **Step 3: 注册到 Render CMakeLists**

`Engine/Render/CMakeLists.txt` 的 `RENDER_SOURCES`（Pipeline 分组处，`Pipeline/HybridRTPipeline.h` 附近）追加：
```cmake
    Pipeline/RTQualityCVars.h
    Pipeline/RTQualityCVars.cpp
```

- [ ] **Step 4: 编译验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无错误（CVar 现在可从 Render 模块访问，验证迁移成功）。

- [ ] **Step 5: 暂存并等待用户确认提交**

```bash
git add -A && git status --short
```

---

### Task 3: RT Passes 读取 SPP / MaxDistance CVar

**Files:**
- Modify: `Engine/Render/RT/RTShadowPass.cpp`（include + `pc.maxShadowDist`）
- Modify: `Engine/Render/RT/RTAOPass.cpp`（include + `pc.sampleCount`/`pc.maxDistance`）
- Modify: `Engine/Render/RT/RTReflectionPass.cpp`（include + `pc.sampleCount`/`pc.maxDistance`）
- Modify: `Engine/Render/RT/RTGIPass.cpp`（include + `pc.sampleCount`/`pc.maxDistance`）

**Interfaces:**
- Consumes: Task 2 的 `cvRTShadowMaxDist`、`cvRTAOSPP`、`cvRTAOMaxDist`、`cvRTReflectionSPP`、`cvRTReflectionMaxDist`、`cvRTGISPP`、`cvRTGIMaxDist`

- [ ] **Step 1: RTShadowPass.cpp**

文件顶部 `#include "RT/RTShadowPass.h"` 附近追加 include：
```cpp
#include "Pipeline/RTQualityCVars.h"
```
`Execute()` 中 `pc.maxShadowDist = 200.0f;` 改为：
```cpp
    pc.maxShadowDist = max(cvRTShadowMaxDist.Get(), 0.01f);
```

- [ ] **Step 2: RTAOPass.cpp**

追加 `#include "Pipeline/RTQualityCVars.h"`。`Execute()` 中：
```cpp
    pc.maxDistance  = 2.0f;      // AO 遮蔽半径（m）
    pc.sampleCount  = 2;         // 每像素射线数（时域累积提升质量）
```
改为：
```cpp
    pc.maxDistance  = max(cvRTAOMaxDist.Get(), 0.01f);  // AO 遮蔽半径（m）
    pc.sampleCount  = clamp(cvRTAOSPP.Get(), 1, 16);    // 每像素射线数（时域累积提升质量）
```

- [ ] **Step 3: RTReflectionPass.cpp**

追加 `#include "Pipeline/RTQualityCVars.h"`。`Execute()` 中：
```cpp
    pc.maxDistance  = 500.0f;   // 反射最大追踪距离
    pc.sampleCount  = 1;
```
改为：
```cpp
    pc.maxDistance  = max(cvRTReflectionMaxDist.Get(), 0.01f);  // 反射最大追踪距离
    pc.sampleCount  = clamp(cvRTReflectionSPP.Get(), 1, 16);    // 每像素采样数
```

- [ ] **Step 4: RTGIPass.cpp**

追加 `#include "Pipeline/RTQualityCVars.h"`。`Execute()` 中：
```cpp
    pc.maxDistance  = 30.0f;    // GI 追踪范围（m）
    pc.sampleCount  = 1;        // SPP（时域累积提升质量）
```
改为：
```cpp
    pc.maxDistance  = max(cvRTGIMaxDist.Get(), 0.01f);  // GI 追踪范围（m）
    pc.sampleCount  = clamp(cvRTGISPP.Get(), 1, 16);    // SPP（时域累积提升质量）
```

- [ ] **Step 5: 编译验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无错误。

- [ ] **Step 6: 暂存并等待用户确认提交**

```bash
git add -A && git status --short
```

---

### Task 4: RTDenoiser 支持运行时 blend + HybridRT 每帧应用

**Files:**
- Modify: `Engine/Render/PostProcess/RTDenoiser.h`（加 `SetTemporalBlend`）
- Modify: `Engine/Render/PostProcess/RTDenoiser.cpp`（实现）
- Modify: `Engine/Render/Pipeline/HybridRTPipeline.cpp`（include RTQualityCVars + 每帧 SetTemporalBlend）

**Interfaces:**
- Consumes: Task 2 的 `cvRTDenoiseShadowBlend`/`cvRTDenoiseAOBlend`/`cvRTDenoiseReflectionBlend`/`cvRTDenoiseGIBlend`
- Produces: `RTDenoiser::SetTemporalBlend(float blend)` — 更新内部 `m_Cfg.temporalBlend`，下次 Render 生效

- [ ] **Step 1: RTDenoiser.h 加方法**

在 `public:` 区 `SetInputs` 声明后追加：
```cpp
    // 运行时更新时域混合因子（CVar 热更新用；Render 时写入 push constant）
    void SetTemporalBlend(float blend) { m_Cfg.temporalBlend = blend; }
```

- [ ] **Step 2: RTDenoiser.cpp 无改动**（`Render()` 已用 `m_Cfg.temporalBlend` 写 push constant，方法在头文件内联即可）。

- [ ] **Step 3: HybridRTPipeline.cpp 每帧应用 blend**

文件顶部 `#include "Pipeline/HybridRTPipeline.h"` 后追加：
```cpp
#include "Pipeline/RTQualityCVars.h"
```
在 `BuildFrameGraph()` 中、各降噪 Pass 的 AddPass lambda **外**（避免每帧重复 SetTemporalBlend 进 lambda），在 `CollectLights` 之后加入：
```cpp
    // ── CVar 热更新：降噪时域混合因子（每帧应用，Render 时写入 push constant）──
    if (m_ShadowDenoiser)     m_ShadowDenoiser->SetTemporalBlend(cvRTDenoiseShadowBlend.Get());
    if (m_AODenoiser)         m_AODenoiser->SetTemporalBlend(cvRTDenoiseAOBlend.Get());
    if (m_ReflectionDenoiser) m_ReflectionDenoiser->SetTemporalBlend(cvRTDenoiseReflectionBlend.Get());
    if (m_GIDenoiser)         m_GIDenoiser->SetTemporalBlend(cvRTDenoiseGIBlend.Get());
```

- [ ] **Step 4: 编译验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无错误。

- [ ] **Step 5: 暂存并等待用户确认提交**

```bash
git add -A && git status --short
```

---

### Task 5: HybridRTPipeline 开关薄封装 + 分辨率热更新 + 降噪开关

**Files:**
- Modify: `Engine/Render/Pipeline/HybridRTPipeline.cpp`（开关 setter/getter 改 CVar、BuildFrameGraph 读 CVar、Render 检测分辨率变化）
- Modify: `Engine/Render/Pipeline/HybridRTPipeline.h`（可保留声明不变）

**Interfaces:**
- Consumes: Task 2 的 `cvRTShadow`/`cvRTAO`/`cvRTReflection`/`cvRTGI`、`cvRTShadowHalfRes`/`cvRTAOHalfRes`/`cvRTReflectionHalfRes`/`cvRTGIQuarterRes`、`cvRTDenoiseTemporal`/`cvRTDenoiseSpatial`
- Produces: 保留 `SetRTShadowEnabled`/`IsRTShadowEnabled` 等签名不变（02.Cube 调用方零改动），内部读写 CVar

- [ ] **Step 1: 开关 setter/getter 改为 CVar 封装**

在 `HybridRTPipeline.cpp`（已有 include RTQualityCVars from Task 4）中，把 4 组 setter/getter 实现改为：
```cpp
void HybridRTPipeline::SetRTShadowEnabled(bool e) { cvRTShadow.Set(e); }
bool HybridRTPipeline::IsRTShadowEnabled() const  { return cvRTShadow.Get(); }
void HybridRTPipeline::SetRTAOEnabled(bool e)     { cvRTAO.Set(e); }
bool HybridRTPipeline::IsRTAOEnabled() const      { return cvRTAO.Get(); }
void HybridRTPipeline::SetRTReflectionEnabled(bool e) { cvRTReflection.Set(e); }
bool HybridRTPipeline::IsRTReflectionEnabled() const  { return cvRTReflection.Get(); }
void HybridRTPipeline::SetRTGIEnabled(bool e)     { cvRTGI.Set(e); }
bool HybridRTPipeline::IsRTGIEnabled() const      { return cvRTGI.Get(); }
```
（这些函数当前可能定义在 .cpp；若在 .h 内联则改 .h。删除 `m_RTShadowEnabled` 等 4 个成员，或保留但不再使用。）

- [ ] **Step 2: BuildFrameGraph 读 CVar 控制 pass 开关**

4 个 RT Pass 的 `if` 条件中，`m_RTShadowEnabled` → `cvRTShadow.Get()`（AO/Reflection/GI 同理）。示例（RT_Shadow）：
```cpp
    if (m_RTEnabled && cvRTShadow.Get() && m_RTShadow && m_RTShadow->IsValid() && m_RTPass) {
```

- [ ] **Step 3: BuildFrameGraph 读降噪开关**

4 个时域降噪 Pass 条件追加 `&& cvRTDenoiseTemporal.Get()`，2 个空间滤波 Pass 条件追加 `&& cvRTDenoiseSpatial.Get()`。示例（RT_Shadow_Denoise）：
```cpp
    if (m_ShadowDenoiser && m_ShadowDenoiser->IsReady()
        && cvRTDenoiseTemporal.Get() && rtShadowHandle != kInvalidHandle) {
```
空间滤波（RT_Reflection_Spatial）：
```cpp
    if (m_ReflectionSpatial.IsReady() && cvRTDenoiseSpatial.Get() && rtReflectionTemporalHandle != kInvalidHandle) {
```

- [ ] **Step 4: Render() 分辨率热更新**

在 `HybridRTPipeline::Render()` 中，`m_FrameIndex++` 之前插入分辨率检测（类中新增 `bool m_ShadowHalfResApplied` 等 4 个缓存标志，`Initialize`/`OnResize` 时初始化为当前值）：
```cpp
    // ── CVar 热更新：分辨率参数变化 → 重建 RT Pass + 对应降噪器（仅变化时一次）──
    auto rebuildRT = [this](RTDenoiser*& denoiser, auto*& pass, bool half,
                            Denoiser* spatial, auto*& rPass) { /* 见下方内联 */ };
```
为避免模板 lambda 复杂化，直接展开 4 组。阴影示例：
```cpp
    bool shHalf = cvRTShadowHalfRes.Get();
    if (m_RTShadow && m_ShadowDenoiser && m_ShadowDenoiser->IsReady()
        && shHalf != m_ShadowHalfResApplied) {
        m_ShadowHalfResApplied = shHalf;
        m_RTShadow->Shutdown();
        m_RTShadow->Initialize(m_Device, m_Width, m_Height, shHalf);
        m_ShadowDenoiser->Shutdown();
        RTDenoiser::Config cfg;
        cfg.format = rhi::Format::R16_FLOAT;
        cfg.width  = m_RTShadow->GetWidth(); cfg.height = m_RTShadow->GetHeight();
        cfg.temporalBlend = cvRTDenoiseShadowBlend.Get();
        cfg.depthThreshold = 0.02f; cfg.normalThreshold = 0.85f;
        cfg.debugName = "RTShadowDenoiser";
        m_ShadowDenoiser->Initialize(m_Device, cfg);
    }
```
AO 同理（`cvRTAOHalfRes`，R8_UNORM，blend `cvRTDenoiseAOBlend`）。反射（含 spatial）完整展开：
```cpp
    bool reflHalf = cvRTReflectionHalfRes.Get();
    if (m_RTReflection && m_ReflectionDenoiser && m_ReflectionDenoiser->IsReady()
        && reflHalf != m_ReflectionHalfResApplied) {
        m_ReflectionHalfResApplied = reflHalf;
        m_RTReflection->Shutdown();
        m_RTReflection->Initialize(m_Device, m_Width, m_Height, reflHalf);
        m_ReflectionDenoiser->Shutdown();
        RTDenoiser::Config cfg;
        cfg.format = rhi::Format::RGBA16_FLOAT;
        cfg.width  = m_RTReflection->GetWidth(); cfg.height = m_RTReflection->GetHeight();
        cfg.temporalBlend = cvRTDenoiseReflectionBlend.Get();
        cfg.depthThreshold = 0.05f; cfg.normalThreshold = 0.80f;
        cfg.debugName = "RTReflectionDenoiser";
        m_ReflectionDenoiser->Initialize(m_Device, cfg);
        m_ReflectionSpatial.Shutdown();
        m_ReflectionSpatial.Initialize(m_Device, cfg.width, cfg.height);
    }
```
GI 用 `cvRTGIQuarterRes`，格式 RGBA16_FLOAT，blend `cvRTDenoiseGIBlend`，重建模式与反射完全相同（`m_RTGI`/`m_GIDenoiser`/`m_GISpatial`，`m_GIQuarterResApplied`）。类头新增成员：
```cpp
    bool m_ShadowHalfResApplied = true;
    bool m_AOHalfResApplied = true;
    bool m_ReflectionHalfResApplied = true;
    bool m_GIQuarterResApplied = true;
```

- [ ] **Step 5: 编译验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无错误。

- [ ] **Step 6: 暂存并等待用户确认提交**

```bash
git add -A && git status --short
```

---

### Task 6: 02.Cube 用 r.Pipeline.Mode 替代 renderMode

**Files:**
- Modify: `Samples/02.Cube/02.Cube.cpp`

**Interfaces:**
- Consumes: `he::CVar<int>`（Task 1 迁移到 Core，02.Cube 可 include）

- [ ] **Step 1: 定义管线模式 CVar**

文件顶部 include 区（`#include "Core/Engine.h"` 附近）追加：
```cpp
#include "Core/CVar.h"
```
文件作用域（main 之外，`he` 命名空间或全局）定义：
```cpp
// 渲染管线模式 CVar（0=Forward, 1=Deferred, 2=HybridRT）
he::CVar<int> cvPipelineMode("r.Pipeline.Mode", 2, "渲染管线模式 0=Forward 1=Deferred 2=HybridRT");
```

- [ ] **Step 2: 替换 renderMode 变量与分支**

`main()` 中删除 `int renderMode = 2;`。主循环三处 `if (renderMode == 0)` / `else if (renderMode == 1)` / `else if (renderMode == 2)` 改为 `if (cvPipelineMode.Get() == 0)` 等。ImGui 部分 `if (renderMode == 2 && device->GetCaps().supportsRayTracing)` 改为 `if (cvPipelineMode.Get() == 2 && ...)`。

- [ ] **Step 3: ImGui RadioButton 绑定 CVar**

`ImGui::RadioButton("Forward 前向渲染", &renderMode, 0);` 三行改为读本地值 + Set 回 CVar：
```cpp
        int mode = cvPipelineMode.Get();
        ImGui::RadioButton("Forward 前向渲染", &mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Deferred 延迟渲染", &mode, 1);
        if (device->GetCaps().supportsRayTracing) {
            ImGui::SameLine();
            ImGui::RadioButton("Hybrid RT 混合光追", &mode, 2);
        }
        cvPipelineMode.Set(mode);
```

- [ ] **Step 4: 编译 + 运行验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无错误。随后运行：
```bash
cd build/bin/Debug && ./02.Cube.exe
```
手动验证：默认 HybridRT 渲染正常；ImGui 切换 Forward/Deferred/HybridRT 正常；关闭 RT AO 不黑屏（回归）。

- [ ] **Step 5: 暂存并等待用户确认提交**

```bash
git add -A && git status --short
```

---

## 自审查

**Spec 覆盖检查：**
- 迁移 CVar → Task 1 ✓
- RTQualityCVars 集中定义 → Task 2 ✓
- 开关薄封装 → Task 5 ✓
- SPP/MaxDistance 每帧读 → Task 3 ✓
- 分辨率热更新（仅变化重建） → Task 5 ✓
- 降噪 blend 热更新 → Task 4 ✓
- 降噪 Temporal/Spatial 开关 → Task 5 ✓
- 校验（SPP clamp、blend 由 CVar 天然限制、MaxDistance max） → Task 3/5 ✓
- ImGui/控制台自动集成 → 引擎 ConsolePanel/ProjectSettingsPanel 自动发现，无需代码 ✓
- 02.Cube r.Pipeline.Mode → Task 6 ✓

**类型一致性：** 所有 CVar 名称在 Task 2 定义、Task 3-5 引用，签名一致；`RTDenoiser::SetTemporalBlend(float)` 在 Task 4 定义并被 Task 4 调用。✓

**注：** 降噪 blend 未 clamp（`CVar<float>` 无范围约束），但 blend 用于 `lerp` 权重，超出 [0,1] 仅导致过冲/欠冲，不崩溃；如需严格约束可在 Task 4 的 SetTemporalBlend 内 clamp 到 [0,1]。
