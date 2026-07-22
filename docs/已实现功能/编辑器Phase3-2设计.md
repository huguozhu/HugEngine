# HugEngine Phase 3-2 编辑器设计

> 日期: 2026-06-25 | 状态: 待实施

## 概述

Phase 3-2 在已完成 3-1（Viewport + Outliner + Details）之上，实现场景持久化、资源浏览和运行模式切换。

| 子系统 | 说明 |
|--------|------|
| 场景保存/加载 | 反射驱动的 Entity→Component→Property 序列化，自定义二进制格式 `.hescene` |
| Content Browser | 文件树+平铺视图，双击 glTF 加载到场景 |
| Play/Stop 模式 | 编辑/运行状态切换，隐藏 UI + 驱动 World::Update |

## §1 — 场景保存/加载

### 文件格式（`.hescene` 二进制）

```
Header:     "HESC" (4 bytes magic) + version (u32, 当前=1)
Entities:   entity_count (u32)
  [entity]:   entity_id (u64) + component_count (u32)
    [comp]:     typeHash (u64, FNV-1a 组件类型) + data_size (u32) + data (raw bytes)
Hierarchy:  pair_count (u32)
  [pair]:     child_id (u64) + parent_id (u64)
```

### 保存流程 (SceneSerializer)

```
遍历 World 所有 Entity → 对每个:
  遍历组件 bucket → 对每个 component:
    通过 component->GetClass() 获取 ClassInfo → typeHash
    创建 BinaryArchive(Write) → SerializeObject<T>(ar, component*)
    将 ar.GetBuffer() 写入 entity 段
遍历 SceneGraph → 写入 hierarchy 段
```

### 加载流程

```
读取 header 验证 magic+version
对每个 entity segment:
  World::CreateEntity()
  对每个 component segment:
    typeHash → TypeRegistry::FindClassByHash() → factory()
    创建 BinaryArchive(Read, data) → SerializeObject<T>(ar, component*)
    World::AddComponent<T>(entity, component*)
对每个 hierarchy pair:
  SceneGraph::SetParent(child, parent)
```

### 局限

- GPU 资源（VBO/IBO/纹理）不序列化，glTF 重新加载时重建
- SerializeObject 不认识的类型（AABB 等）自动跳过
- 首次打开场景时需同时有原始 glTF 文件

### 接口

```cpp
// Engine/Editor/Public/Editor/SceneSerializer.h
class SceneSerializer {
public:
    bool Save(const String& filePath, World& world, SceneGraph& sg);
    bool Load(const String& filePath, World& world, SceneGraph& sg);
};
```

## §2 — Content Browser

### 布局

```
┌─────────────────────────────────┐
│ Content Browser                 │
├──────────────┬──────────────────┤
│ 目录树        │  文件列表（平铺）   │
│ Content/     │  sponza.glb      │
│ ├─ Models/   │  helmet.glb      │
│ ├─ Textures/ │  floor.png       │
│ └─ Scenes/   │  test.hescene    │
└──────────────┴──────────────────┘
```

### 功能

- **左侧目录树**：默认扫描 `Content/` 目录，`std::filesystem::directory_iterator`
- **右侧文件列表**：显示文件名 + 大小，双击 glTF → `glTFLoader::LoadGLB()`
- **GLB header 预览**：读取前 12 bytes 显示 magic+version
- **搜索过滤**：按文件名
- **右键菜单**：Import、Show in Explorer
- 可通过 `View → Content Browser` 开关

### 面板结构

```cpp
class ContentBrowserPanel {
    void Initialize(EditorContext* ctx);
    void Render();
private:
    String m_CurrentPath;  // 当前浏览目录
    void RenderDirectoryTree(const String& path);
    void RenderFileGrid(const String& path);
    void ImportGLB(const String& filePath);
};
```

## §3 — Play/Stop 模式

### 状态模型

```
EditMode  ←──→  PlayMode
(完整UI)        (全屏渲染+World::Update)
```

### 行为变化

| 系统 | EditMode | PlayMode |
|------|----------|----------|
| 编辑面板 | 显示 | 全部隐藏 |
| 菜单栏 | 完整 | 仅 Stop 按钮 |
| 相机 | EditorCamera (WASD) | 场景 Camera Component |
| 选中/编辑 | 允许 | 禁止 |
| 组件 Update | 不调用 | 每帧 World::Update(dt) |
| 退出方式 | 关闭窗口 | P 键 / ESC / Stop 按钮 |

### EditorContext 扩增

```cpp
enum class EditorState : u8 { Edit, Play };

class EditorContext {
    // ... 现有接口 ...
    void Play();
    void Stop();
    EditorState GetState() const;
    bool IsPlaying() const;
};
```

### EditorApp::MainLoop 分叉

```cpp
if (m_EditorCtx->IsPlaying()) {
    m_World->Update(dt);
    m_Viewport->RenderGameView(cmdList);  // 游戏相机
} else {
    // 现有编辑 UI 渲染流程
}
m_ImGui->BeginFrame();
if (m_EditorCtx->IsPlaying()) {
    // 覆盖层：FPS + "Press ESC to Stop"
} else {
    // 现有面板渲染
}
```

### 非目标

- 物理模拟
- 脚本运行时
- 多 Camera 选择（取场景中第一个，无则报错）

## 文件结构

```
Engine/Editor/
├── Public/Editor/SceneSerializer.h    (新增)
├── Private/SceneSerializer.cpp         (新增)
Samples/Editor/Panels/
├── ContentBrowserPanel.h/cpp           (新增)
```

修改文件：
- `EditorContext.h/cpp` — 添加 EditorState
- `EditorApp.cpp` — MainLoop 分叉 + 菜单栏 Play/Stop 按钮
- `ViewportPanel.h/cpp` — 支持游戏相机渲染
- `Engine/Editor/CMakeLists.txt` — 添加 SceneSerializer.cpp
