# HybridRT CVar 质量体系设计

> **日期**: 2026-08-03
> **状态**: 已批准
> **目标**: 将 HybridRTPipeline 的全部硬编码质量参数暴露为运行时可调的 CVar（控制台变量），并实现运行时热更新。

## 背景

HybridRTPipeline 的 RT 质量参数（开关、分辨率、SPP、追踪距离、降噪参数）目前全部硬编码：

- 4 个效果开关：`m_RTShadowEnabled` 等（`HybridRTPipeline.h:137-140`），仅 ImGui 可调
- 分辨率：`RTShadowPass.h:50` 等 `m_HalfRes`/`m_QuarterRes`，Initialize 时固化
- 追踪距离 / SPP：各 RT Pass `Execute()` 内硬编码进 push constant（如 `RTShadowPass.cpp:156` maxShadowDist=200）
- 降噪参数：`RTDenoiser::Config` 在 `HybridRTPipeline.cpp:112-188` 初始化时烘焙，运行时不可改

引擎**已有完整 CVar 系统**（`Engine/Editor/Editor/CVar.h/cpp`），支持 bool/i32/f32/String，构造时自注册，ConsolePanel（`~` 键）支持 `set/get/list`，ProjectSettingsPanel 自动显示。**但 CVar 位于 Editor 模块，Render 模块（HybridRTPipeline 等）无法访问。**

## 方案

采用已批准的**方案 A**：把 CVar 系统从 Editor 迁移到 Core 模块，使 Render 模块内部能直接声明和读取 CVar。全部参数运行时热更新。

## 一、CVar 系统迁移（Editor → Core）

| 文件 | 操作 | 说明 |
|------|------|------|
| `Engine/Core/Core/CVar.h/cpp` | 新建 | 从 `Engine/Editor/Editor/CVar.h/cpp` 迁移，内容不变 |
| `Engine/Editor/Editor/CVar.h/cpp` | 删除 | 原位置移除 |
| `Engine/Core/CMakeLists.txt` | 修改 | `CORE_SOURCES` 添加 `Core/CVar.cpp` |
| `Engine/Editor/CMakeLists.txt` | 修改 | 移除 `Editor/CVar.h` 和 `Editor/CVar.cpp` 两行 |
| `Samples/Editor/EditorApp.cpp` | 修改 | include `Editor/CVar.h` → `Core/CVar.h` |
| `Samples/Editor/Panels/ConsolePanel.cpp` | 修改 | 同上 |
| `Samples/Editor/Panels/ProjectSettingsPanel.cpp` | 修改 | 同上 |
| `Samples/Editor/Panels/ViewportPanel.cpp` | 修改 | 同上 |

CVar.h 仅依赖 `Core/Types.h`、`Containers/Array.h`、`std::variant`，均可在 Core 层满足。`CVarBase::GetAll()` 全局注册表逻辑在 `CVar.cpp` 中，随文件迁移。

## 二、CVar 集中定义

新建 `Engine/Render/Pipeline/RTQualityCVars.h/.cpp`，集中声明 + 定义所有 RT 质量 CVar。RT Passes 与 HybridRTPipeline 统一 include 该头访问，避免 extern 散落各文件。

### CVar 清单

**管线模式**（定义在 `Samples/02.Cube/02.Cube.cpp`，应用层切换管线）：

| CVar | 类型 | 默认 | 说明 |
|------|------|------|------|
| `r.Pipeline.Mode` | i32 | 2 | 0=Forward, 1=Deferred, 2=HybridRT |

**RT 质量**（定义在 `RTQualityCVars.cpp`）：

| CVar | 类型 | 默认 | 说明 |
|------|------|------|------|
| `r.RT.Shadow` | bool | true | RT 阴影开关 |
| `r.RT.Shadow.HalfRes` | bool | true | 阴影半分辨率 |
| `r.RT.Shadow.MaxDistance` | f32 | 200 | 阴影最大追踪距离 |
| `r.RT.AO` | bool | true | RT AO 开关 |
| `r.RT.AO.HalfRes` | bool | true | AO 半分辨率 |
| `r.RT.AO.SPP` | i32 | 2 | AO 每像素射线数 |
| `r.RT.AO.MaxDistance` | f32 | 2.0 | AO 遮蔽半径 |
| `r.RT.Reflection` | bool | true | RT 反射开关 |
| `r.RT.Reflection.HalfRes` | bool | true | 反射半分辨率 |
| `r.RT.Reflection.SPP` | i32 | 1 | 反射每像素采样数 |
| `r.RT.Reflection.MaxDistance` | f32 | 500 | 反射最大追踪距离 |
| `r.RT.GI` | bool | true | RT GI 开关 |
| `r.RT.GI.QuarterRes` | bool | true | GI 四分之一分辨率 |
| `r.RT.GI.SPP` | i32 | 1 | GI 每像素采样数 |
| `r.RT.GI.MaxDistance` | f32 | 30 | GI 追踪范围 |
| `r.RT.Denoise.Temporal` | bool | true | 时域降噪开关 |
| `r.RT.Denoise.Spatial` | bool | true | 空间滤波开关 |
| `r.RT.Denoise.Shadow.Blend` | f32 | 0.05 | 阴影时域混合因子 |
| `r.RT.Denoise.AO.Blend` | f32 | 0.05 | AO 时域混合因子 |
| `r.RT.Denoise.Reflection.Blend` | f32 | 0.10 | 反射时域混合因子 |
| `r.RT.Denoise.GI.Blend` | f32 | 0.15 | GI 时域混合因子 |

## 三、读取机制（运行时热更新）

### 开关（`r.RT.Shadow` 等）
`BuildFrameGraph()` 每帧读 CVar 决定是否加入对应 Pass。`m_RTShadowEnabled` 等成员**保留为 CVar 薄封装**：
- `SetRTShadowEnabled(bool e)` → `cvRTShadow.Set(e)`
- `IsRTShadowEnabled()` → `cvRTShadow.Get()`

02.Cube 现有 ImGui Checkbox 代码因此零改动。

### SPP / MaxDistance
各 RT Pass `Execute()` 每帧读 CVar 写入 push constant：
- `RTShadowPass::Execute()` → `pc.maxShadowDist = cvShadowMaxDist.Get()`
- `RTAOPass::Execute()` → `pc.sampleCount = cvAOSPP.Get()`，`pc.maxDistance = cvAOMaxDist.Get()`
- `RTReflectionPass::Execute()` → `pc.sampleCount / maxDistance`
- `RTGIPass::Execute()` → 同理

`CVar<T>::Get()` 直接读模板自身值，无查表开销，每帧调用安全。

### 分辨率（`HalfRes` / `QuarterRes`）
`HybridRTPipeline::Render()` 每帧读 CVar，与各 RT Pass 当前分辨率模式比较，**仅值变化时触发一次重建**（`Shutdown()` + `Initialize()`，复用 OnResize 的重建逻辑）。需在类中缓存上次应用的分辨率标志，避免每帧重复重建。

### 降噪参数（Blend）
`RTDenoiser` 新增 `SetTemporalBlend(float)` 方法（配置存储在成员，Render 时写入 push constant）。管线在渲染前每帧从对应 CVar 应用 blend 值。

### 降噪开关（Temporal / Spatial）
- `r.RT.Denoise.Temporal` = false → 跳过时域累积 Pass，Spatial（若开）直接以 **RT 原始输出** 为输入
- `r.RT.Denoise.Spatial` = false → 跳过空间滤波 Pass（反射/GI 用时域输出）
- 两个开关都关 → 完全禁用降噪，回退到无降噪的原始 RT 输出

数据流：
```
Temporal=on:  RT原始 → [时域] → 时域输出 → [空间] → 降噪输出
Temporal=off: RT原始 → [空间] → 降噪输出（空间直接滤波原始）
Spatial=off:  RT原始 → [时域] → 时域输出 → Lighting
两者都关:     RT原始 → Lighting
```

## 四、ImGui / 控制台集成

- ConsolePanel（`~` 键）与 ProjectSettingsPanel **自动显示所有新 CVar**，零额外代码
- 02.Cube ImGui 面板：现有 getter/setter 保留（薄封装）；`renderMode` 局部变量删除，改为读 `r.Pipeline.Mode` CVar；`RadioButton` 绑定该 CVar
- 02.Cube 主循环的 `if (renderMode == N)` 分支改为读 `r.Pipeline.Mode.Get()`

## 五、校验与边界

- **SPP**：读取处 `clamp(Get(), 1, 16)`
- **Blend**：读取处 `clamp(Get(), 0.0f, 1.0f)`
- **MaxDistance**：读取处 `max(Get(), 0.01f)`
- **RT 设备不支持**（`m_RTEnabled=false`）：`r.RT.*` 无效果，管线回退光栅化（现有逻辑不变）
- **分辨率重建**：仅值变化时触发，缓存上次应用值防止每帧重建
- **降噪器重建**：分辨率 CVar 变化触发 RT Pass 重建时，同步重建对应降噪器（复用现有 OnResize 中降噪器重建逻辑）

## 六、验证

1. 编译通过（Render 模块 + 02.Cube + Editor）
2. 启动 02.Cube，确认默认 HybridRT 渲染无回归
3. 控制台验证热更新：
   - `set r.RT.Shadow 0` → 阴影立即消失
   - `set r.RT.AO.SPP 4` → AO 质量提升
   - `set r.RT.GI.QuarterRes 0` → GI 触发一次重建，变全分辨率
   - `set r.RT.Denoise.Temporal 0` → 降噪关闭，RT 噪声出现
4. 确认 ImGui 面板的 RT 开关仍正常（薄封装）
5. 切换 Deferred/Forward 模式无回归

## 关键文件变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `Engine/Core/Core/CVar.h/cpp` | 新建 | CVar 系统迁移 |
| `Engine/Editor/Editor/CVar.h/cpp` | 删除 | 迁移源 |
| `Engine/Core/CMakeLists.txt` | 修改 | 注册 CVar.cpp |
| `Engine/Editor/CMakeLists.txt` | 修改 | 移除 CVar |
| 4 个 Editor 引用文件 | 修改 | include 路径 |
| `Engine/Render/Pipeline/RTQualityCVars.h/cpp` | 新建 | RT 质量 CVar 集中定义 |
| `Engine/Render/Pipeline/HybridRTPipeline.h/cpp` | 修改 | 开关薄封装、分辨率热更新、降噪参数应用 |
| `Engine/Render/RT/RTShadowPass.cpp` | 修改 | maxDistance 读 CVar |
| `Engine/Render/RT/RTAOPass.cpp` | 修改 | SPP/maxDistance 读 CVar |
| `Engine/Render/RT/RTReflectionPass.cpp` | 修改 | SPP/maxDistance 读 CVar |
| `Engine/Render/RT/RTGIPass.cpp` | 修改 | SPP/maxDistance 读 CVar |
| `Engine/Render/PostProcess/RTDenoiser.h/cpp` | 修改 | 加 SetTemporalBlend() |
| `Samples/02.Cube/02.Cube.cpp` | 修改 | r.Pipeline.Mode 替换 renderMode |
