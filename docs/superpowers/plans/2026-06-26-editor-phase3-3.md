# Phase 3-3 编辑器实施计划

> **目标：** 补全光照编辑 + Material 扩展 + Project Settings 面板

**架构：** DetailsPanel 扩增光源/材质编辑；ProjectSettingsPanel 新独立面板（CVar 驱动）。三个任务互不依赖可并行。

**技术栈：** C++20, CMake, MSVC 2026, ImGui v1.91

## 全局约束

- 中文注释、Commit 中文无 AI 署名
- 属性编辑走 `PropertyChangeCommand` + `IsItemDeactivatedAfterEdit` Undo/Redo
- 面板集成在 `EditorApp` 中，`m_Visible` 公开供菜单引用

---

### Task 1: 光照编辑器完善

**文件：** 修改 `Samples/Editor/Panels/DetailsPanel.cpp`

- `Render()` 中增加 PointLight/SpotLight 检测
- 提取 `RenderLightBase()` 渲染 Color/Intensity/CastShadow 公共属性
- `RenderDirectionalLight()` 渲染 Direction + 基类属性
- `RenderPointLight()` 渲染 Range + 基类属性
- `RenderSpotLight()` 渲染 Direction/Range/InnerAngle/OuterAngle + 基类属性
- 锥角以度数显示（内部存储弧度），全部 Undo/Redo

- [ ] 编译 + 提交

---

### Task 2: Material 编辑器扩展

**文件：** 修改 `Samples/Editor/Panels/DetailsPanel.cpp`

在 `RenderMesh()` 中增加：
- AlphaMode Combo（Opaque/Mask/Blend）
- AlphaCutoff DragFloat（仅 Mask 时显示）
- DoubleSided Checkbox
- Unlit Checkbox
- AO Strength DragFloat [0,1]

全部 Undo/Redo。

- [ ] 编译 + 提交

---

### Task 3: Project Settings 面板

**文件：**
- 创建 `Samples/Editor/Panels/ProjectSettingsPanel.h`
- 创建 `Samples/Editor/Panels/ProjectSettingsPanel.cpp`
- 修改 `Samples/Editor/EditorApp.h`（成员 + 前向声明）
- 修改 `Samples/Editor/EditorApp.cpp`（Init + View 菜单 + Render）
- 修改 `Samples/Editor/CMakeLists.txt`（源文件）

数据源 `CVarBase::GetAll()`，按类型渲染 DragInt/DragFloat/Checkbox/InputText。搜索栏 + View 菜单开关。

- [ ] 编译 + 提交
