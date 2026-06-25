# HugEngine Phase 3 编辑器设计

> 日期: 2026-06-25 | 状态: 待实施

## 概述

Phase 3 在已完成 Phase 1+2 引擎核心（L0-L5 + Asset + Editor 基础）之上，实现独立编辑器应用。分三个子任务按顺序推进：

| 阶段 | 内容 | 目标 |
|------|------|------|
| 3-1 | Viewport + Outliner + Details | 最小可用编辑器 |
| 3-2 | Content Browser + 场景保存/加载 + Play/Stop | 完整编辑器 |
| 3-3 | Material Editor + 光照编辑器 + Project Settings | 全功能编辑器 |

本文档聚焦 3-1 的设计，3-2 和 3-3 留待后续补充。

## 架构

### 编辑器分层

```
┌─────────────────────────────────────────┐
│  Samples/Editor/                        │
│  EditorApp (主循环, 生命周期)             │
│  ├─ ViewportPanel  (3D 视口)             │
│  ├─ OutlinerPanel  (场景层级树)           │
│  └─ DetailsPanel   (属性面板)             │
├─────────────────────────────────────────┤
│  Engine/Editor/                         │
│  EditorContext (选中状态, 属性编辑)        │
│  ImGuiIntegration (渲染后端)              │
│  Command/CommandHistory (Undo/Redo)      │
│  CVar (控制台变量)                        │
├─────────────────────────────────────────┤
│  Engine 模块 (只读)                       │
│  World / SceneGraph / Component /        │
│  ReflectionAPI / ForwardPipeline / RHI   │
└─────────────────────────────────────────┘
```

### EditorContext — 编辑器运行时上下文

```cpp
class EditorContext {
public:
    void Initialize(World* world, SceneGraph* sceneGraph, CommandHistory* cmdHistory);

    // 选中管理
    void SelectEntity(Entity e);
    void AddToSelection(Entity e);
    void ToggleSelection(Entity e);
    void DeselectAll();
    const TArray<Entity>& GetSelection() const;
    bool IsSelected(Entity e) const;

    // 属性编辑（自动包装 Command，支持 Undo/Redo）
    template<typename T>
    void SetProperty(Entity entity, const char* propName, const T& value);

    // 引擎引用
    World* GetWorld() const;
    SceneGraph* GetSceneGraph() const;
    CommandHistory* GetCommandHistory() const;

    // 选中变化回调（面板通过此回调刷UI）
    void OnSelectionChanged(std::function<void()> cb);

private:
    World*          m_World = nullptr;
    SceneGraph*     m_SceneGraph = nullptr;
    CommandHistory* m_CmdHistory = nullptr;
    TArray<Entity>  m_Selection;
    TArray<std::function<void()>> m_SelectionCallbacks;
};
```

关键设计决策：
- **不拥有 World** — EditorContext 是操作者/观察者，World 由 EditorApp 拥有
- **选中是 Entity 列表** — 支持多选（Ctrl+Click），3-1 阶段 UI 只暴露单选
- **SetProperty 自动走 Command** — 旧值 → PropertyChangeCommand → CommandHistory → Ctrl+Z 可用

### 面板间通信

```
Outliner 点击
  → EditorContext::SelectEntity()
    → 触发 OnSelectionChanged 回调
      → Details 刷新显示被选中对象属性
      → Viewport 高亮被选中对象（后续）
```

所有面板只依赖 EditorContext，不互调。这保证面板可独立开发和测试。

## 面板设计

### 布局

```
┌──────────────────────────────────────────────────┐
│  Menu Bar: File | Edit | View                    │
├──────────────────────┬───────────────────────────┤
│   Viewport           │   Details Panel           │
│   (3D 渲染视口)      │   - 组件列表               │
│   - ForwardPipeline  │   - 每属性行               │
│   - Editor Camera    │   - 即时编辑+Ctrl+Z        │
│   - 网格辅助线        │                           │
├──────────────────────┴───────────────────────────┤
│  Status Bar                                       │
├──────────────────────────────────────────────────┤
│  Outliner (场景层级树)                            │
└──────────────────────────────────────────────────┘
```

### Viewport Panel

- 通过 `ForwardPipeline::RenderScene()` 渲染 World 到 off-screen texture
- ImGui `Image` 控件显示纹理
- 独立的 Editor Camera（WASD + 鼠标右键旋转）
- 绘制网格辅助线（ImGui 或简单线条渲染）
- 鼠标拾取留到后续阶段

### Outliner Panel

- 数据源：`World::GetEntities()` + `SceneGraph` 层级
- 点击 = 选中（→ EditorContext::SelectEntity）
- 右键菜单：删除、重命名
- 搜索过滤文本框
- 实体类型图标（灯泡=Light, 立方体=Mesh, 球=Sphere）

### Details Panel

- 数据源：选中实体 → World 组件查询 → ReflectionAPI::ForEachProperty
- 每个属性根据类型选编辑器：
  - `float` → DragFloat
  - `vec3` (位置) → InputFloat3
  - `vec3` (颜色) → ColorEdit3
  - `string` → InputText
- 值变化时 → `EditorContext::SetProperty()` → 自动 Undoable

## 编辑器应用 (EditorApp)

### 文件结构

```
Samples/Editor/
├── CMakeLists.txt
├── EditorApp.h          # 应用主类
├── EditorApp.cpp
└── Panels/
    ├── ViewportPanel.h
    ├── ViewportPanel.cpp
    ├── OutlinerPanel.h
    ├── OutlinerPanel.cpp
    ├── DetailsPanel.h
    └── DetailsPanel.cpp
```

### 生命周期

```cpp
class EditorApp {
public:
    int Run();

private:
    void Init();      // 窗口 + RHI + ImGui + EditorContext + 默认场景
    void Loop();      // BeginFrame → 渲染面板 → EndFrame
    void Shutdown();

    GLFWwindow*              m_Window;
    std::unique_ptr<IRHIDevice>    m_Device;
    std::unique_ptr<ImGuiIntegration> m_ImGui;

    World            m_World;
    SceneGraph       m_SceneGraph;
    ForwardPipeline  m_Pipeline;
    CommandHistory   m_CmdHistory;
    EditorContext    m_EditorCtx;

    ViewportPanel    m_Viewport;
    OutlinerPanel    m_Outliner;
    DetailsPanel     m_Details;
};
```

### 默认场景

编辑器启动时创建默认场景：地面 Plane（带网格纹理方向）+ DirectionalLight + Camera。

### 与 Sponza 的关系

Sponza 保持独立不修改。编辑器通过 `glTFLoader` 加载 glTF 文件，不依赖 Sponza 代码。

## 依赖与约束

- **复用不修改**：所有 Engine 模块只读使用，不在 Phase 3-1 中修改
- **RHI**：通过现有 Vulkan 后端 + ImGuiIntegration 渲染
- **反射**：Details 面板依赖 ReflectionAPI 枚举属性
- **序列化**：场景保存/加载延后到 3-2，3-1 不做磁盘持久化

## 非目标（3-1 不做）

- 场景保存/加载（→ 3-2）
- Content Browser / Asset 管理（→ 3-2）
- Play/Stop 模式切换（→ 3-2）
- Gizmo 操作（平移/旋转/缩放拖拽）
- 鼠标拾取选择
- Material Editor（→ 3-3）
- 多窗口/可停靠面板布局
