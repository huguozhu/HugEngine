# HugEngine Phase 3-3 编辑器设计

> 日期: 2026-06-26 | 状态: 待实施

## 概述

Phase 3-3 完善编辑器面板：补全光照组件编辑、扩展材质编辑、新增项目设置面板。

| 子系统 | 说明 | 方式 |
|--------|------|------|
| 光照编辑器完善 | PointLight/SpotLight 属性编辑，补齐三光源 | DetailsPanel 扩增 |
| Material Editor | AlphaMode/DoubleSided/Unlit 等 PBR 参数 | DetailsPanel 内扩展 |
| Project Settings | CVar 浏览+编辑面板 | 新独立面板 |

## §1 — 光照编辑器完善

### 当前问题
`RenderLight()` 只调用 `GetComponent<DirectionalLight>()`，其他光源类型选中后无属性显示。

### 设计

按子类优先级检测：DirectionalLight → PointLight → SpotLight → 基类兜底。

### 属性编辑矩阵

| 属性 | DirectionalLight | PointLight | SpotLight |
|------|:---:|:---:|:---:|
| Color (ColorEdit3) | ✅ | ✅ | ✅ |
| Intensity (DragFloat) | ✅ | ✅ | ✅ |
| Cast Shadow (Checkbox) | ✅ | ✅ | ✅ |
| Direction (DragFloat3, normalize) | ✅ | — | ✅ |
| Range (DragFloat) | — | ✅ | ✅ |
| Inner Cone Angle (Slider, rad→deg 显示) | — | — | ✅ |
| Outer Cone Angle (Slider, rad→deg 显示) | — | — | ✅ |

- 基类公共属性（Color/Intensity/CastShadow）提取为 `RenderLightBase()` 复用
- 全部走 `PropertyChangeCommand` Undo/Redo（`IsItemDeactivatedAfterEdit` 模式）

## §2 — Project Settings 面板

### 设计

```
┌─────────────────────────────────┐
│ Project Settings                │
├─────────────────────────────────┤
│ [Search...]                     │
├─────────────────────────────────┤
│ Rendering                       │
│   r.width       1920    [int]   │
│   r.vsync       true    [bool]  │
│ Editor                          │
│   ed.grid.size  1.0     [float] │
└─────────────────────────────────┘
```

### 数据源
`CVarBase::GetAll()` → 遍历所有注册的 CVar

### 按类型渲染编辑器
- `i32` → `ImGui::DragInt`
- `f32` → `ImGui::DragFloat`
- `bool` → `ImGui::Checkbox`
- `String` → `ImGui::InputText`

### 功能
- 搜索栏按名称过滤
- 按 `Category` 属性分组折叠（如 `"Rendering"`, `"Editor"`）
- 通过菜单 `View → Project Settings` 开关

### 文件
```
Samples/Editor/Panels/ProjectSettingsPanel.h/cpp  (新增)
EditorApp.h/cpp (集成)
```

## §3 — Material Editor（DetailsPanel 内扩展）

在现有 `RenderMesh()` 中增加：

| 控件 | 属性 | 备注 |
|------|------|------|
| `ImGui::Combo` | `alphaMode` | Opaque(0) / Mask(1) / Blend(2) |
| `ImGui::DragFloat` | `alphaCutoff` | 仅 alphaMode==Mask 时显示 |
| `ImGui::Checkbox` | `doubleSided` | 双面渲染 |
| `ImGui::Checkbox` | `unlit` | 无光照模式 |
| `ImGui::DragFloat` | `aoFactor` | 环境光遮蔽强度 [0,1] |

全部走 `PropertyChangeCommand` Undo/Redo。

### 非目标
- 纹理缩略图预览（ImGui::Image）——延后至 Bindless 纹理就绪

## 文件结构

```
Samples/Editor/Panels/ProjectSettingsPanel.h  (新增)
Samples/Editor/Panels/ProjectSettingsPanel.cpp (新增)
Samples/Editor/Panels/DetailsPanel.cpp          (修改)
Samples/Editor/EditorApp.h                      (修改: 添加成员)
Samples/Editor/EditorApp.cpp                    (修改: 集成面板)
Samples/Editor/CMakeLists.txt                   (修改: 添加源文件)
```
