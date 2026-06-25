# Phase 3-2 编辑器实现计划

> **目标：** 实现场景保存/加载 + Content Browser + Play/Stop 模式

**架构：** SceneSerializer（反射驱动二进制序列化）+ ContentBrowserPanel（文件浏览+glTF导入）+ EditorState（编辑/运行切换）。World 新增 ForEachComponent 迭代器支持序列化遍历。

**技术栈：** C++20, CMake, MSVC 2026, Vulkan, ImGui v1.91, std::filesystem

## 全局约束

- 所有新增代码附带中文注释
- 引擎模块代码放 `Engine/Editor/`，应用代码放 `Samples/Editor/Panels/`
- 复用不修改：现有引擎模块最小化修改（World 需加 `ForEachComponent`）
- 序列化不处理 GPU 资源（VBO/IBO/纹理），glTF 重新加载
- Commit 中文，无 AI 署名

---

### Task 1: World::ForEachComponent — 遍历实体所有组件

**文件：**
- 修改: `Engine/Scene/Public/Scene/World.h`

**接口：**
- 产生: `void World::ForEachComponent(Entity entity, std::function<void(Component*)> callback)` — 遍历指定实体的所有组件

- [ ] **Step 1: 添加 ForEachComponent 方法**

在 `World.h` 的 `ForEachEntity` 方法下方添加：

```cpp
/// 遍历指定实体的所有组件（供序列化等遍历使用）
/// 回调参数为 Component* 基类指针，可通过 GetClass() 获取反射信息
void ForEachComponent(Entity entity, std::function<void(Component*)> callback) {
    if (!IsValid(entity)) return;
    for (auto& [typeIndex, bucket] : m_Store) {
        for (auto& entry : bucket) {
            if (entry.entityID == entity.id) {
                callback(entry.ptr.get());
            }
        }
    }
}
```

- [ ] **Step 2: 编译验证**

```bash
cd D:/Source/HugEngine/build && cmake --build . --target HugEngineScene --config Debug
```

- [ ] **Step 3: 提交**

---

### Task 2: SceneSerializer — 场景保存/加载

**文件：**
- 创建: `Engine/Editor/Public/Editor/SceneSerializer.h`
- 创建: `Engine/Editor/Private/SceneSerializer.cpp`
- 修改: `Engine/Editor/CMakeLists.txt`

**接口：**
- 消费: `World::ForEachEntity`, `World::ForEachComponent`, `Component::GetClass()`, `TypeRegistry::FindClassByHash()`, `BinaryArchive`, `SerializeObject<T>()`, `SceneGraph::GetParent()`, `SceneGraph::SetParent()`
- 产生: `bool Save(path, World&, SceneGraph&)`, `bool Load(path, World&, SceneGraph&)`

- [ ] **Step 1: 编写 SceneSerializer.h**

```cpp
// Engine/Editor/Public/Editor/SceneSerializer.h
#pragma once

#include "Core/Types.h"

namespace he {
    class World;
    class SceneGraph;
}

namespace he::editor {

// ============================================================
// SceneSerializer — 场景二进制序列化
//
// .hescene 格式: Header "HESC" + version + entities[] + hierarchy[]
// 每个 entity 包含 component 列表，通过反射 SerializeObject 序列化
// ============================================================
class SceneSerializer {
public:
    /// 保存场景到文件
    static bool Save(StringView filePath, World& world, SceneGraph& sg);

    /// 从文件加载场景（会清空现有 World/SceneGraph 内容）
    static bool Load(StringView filePath, World& world, SceneGraph& sg);

private:
    static constexpr u32 kMagic   = 0x43534548; // "HESC" (little-endian)
    static constexpr u32 kVersion = 1;
};

} // namespace he::editor
```

- [ ] **Step 2: 编写 SceneSerializer.cpp — Save 实现**

```cpp
// Engine/Editor/Private/SceneSerializer.cpp
#include "Editor/SceneSerializer.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Component.h"
#include "Scene/Transform.h"
#include "Serialize/BinaryArchive.h"
#include "Reflect/ReflectionAPI.h"
#include "Core/Log.h"

#include <fstream>
#include <vector>

namespace he::editor {

bool SceneSerializer::Save(StringView filePath, World& world, SceneGraph& sg) {
    std::vector<u8> buffer;

    // --- 写入 Header ---
    auto writeU32 = [&](u32 v) {
        buffer.push_back((u8)(v));
        buffer.push_back((u8)(v >> 8));
        buffer.push_back((u8)(v >> 16));
        buffer.push_back((u8)(v >> 24));
    };
    auto writeU64 = [&](u64 v) {
        for (int i = 0; i < 8; ++i)
            buffer.push_back((u8)(v >> (i * 8)));
    };

    writeU32(kMagic);
    writeU32(kVersion);

    // --- 写入 Entities ---
    // 第一遍：计算 entity 数量
    u32 entityCount = 0;
    world.ForEachEntity([&](Entity) { entityCount++; });
    writeU32(entityCount);

    world.ForEachEntity([&](Entity e) {
        writeU64(e.id);

        // 收集该 entity 的所有 component 序列化数据
        struct CompData { u64 typeHash; std::vector<u8> data; };
        std::vector<CompData> comps;

        world.ForEachComponent(e, [&](Component* comp) {
            const reflect::ClassInfo* cls = comp->GetClass();
            if (!cls) return;

            serialize::BinaryArchive ar(serialize::ArchiveMode::Write);
            ar.BeginObject("comp");

            // 通过反射遍历属性 → SerializeObject 派发
            reflect::ForEachProperty<void>(comp, [&](const reflect::PropertyInfo& prop, void* ptr) {
                if (!(prop.flags & reflect::PF_Serializable)) return;
                StringView tn = prop.typeName;
                if      (tn == "bool")   { auto* v = (bool*)ptr;  ar.Serialize(prop.name, *v); }
                else if (tn == "i32")    { auto* v = (i32*)ptr;   ar.Serialize(prop.name, *v); }
                else if (tn == "u32")    { auto* v = (u32*)ptr;   ar.Serialize(prop.name, *v); }
                else if (tn == "i64")    { auto* v = (i64*)ptr;   ar.Serialize(prop.name, *v); }
                else if (tn == "u64")    { auto* v = (u64*)ptr;   ar.Serialize(prop.name, *v); }
                else if (tn == "f32")    { auto* v = (f32*)ptr;   ar.Serialize(prop.name, *v); }
                else if (tn == "f64")    { auto* v = (f64*)ptr;   ar.Serialize(prop.name, *v); }
                else if (tn == "String") { auto* v = (String*)ptr;ar.Serialize(prop.name, *v); }
                else if (tn == "float3") { auto* v = (float3*)ptr;ar.Serialize(prop.name, *v); }
                else if (tn == "float4") { auto* v = (float4*)ptr;ar.Serialize(prop.name, *v); }
                else if (tn == "float2") { auto* v = (float2*)ptr;ar.Serialize(prop.name, *v); }
                else if (tn == "quat")   { auto* v = (quat*)ptr;  ar.Serialize(prop.name, *v); }
            });

            ar.EndObject();
            comps.push_back({cls->typeHash, ar.GetBuffer()});
        });

        writeU32((u32)comps.size());
        for (auto& [hash, data] : comps) {
            writeU64(hash);
            writeU32((u32)data.size());
            buffer.insert(buffer.end(), data.begin(), data.end());
        }
    });

    // --- 写入 Hierarchy ---
    // 收集所有 parent-child 关系
    struct Pair { u64 child; u64 parent; };
    std::vector<Pair> pairs;
    world.ForEachEntity([&](Entity e) {
        Entity parent = sg.GetParent(e);
        if (parent.IsValid()) {
            pairs.push_back({e.id, parent.id});
        }
    });
    writeU32((u32)pairs.size());
    for (auto& [child, parent] : pairs) {
        writeU64(child);
        writeU64(parent);
    }

    // --- 写入文件 ---
    std::ofstream file(String(filePath), std::ios::binary);
    if (!file) {
        HE_CORE_ERROR("SceneSerializer: cannot open file for write: {}", filePath);
        return false;
    }
    file.write((const char*)buffer.data(), buffer.size());
    HE_CORE_INFO("Scene saved: {} entities, {} hierarchy pairs, {} bytes",
        entityCount, pairs.size(), buffer.size());
    return true;
}
```

- [ ] **Step 3: 编写 SceneSerializer.cpp — 属性类型分派辅助**

在 Save 之前添加一个共享的类型分派 lambda：

```cpp
// 属性类型分派序列化（Save 和 Load 共用）
static void SerializeProperty(serialize::IArchive& ar, StringView typeName,
                               const reflect::PropertyInfo& prop, void* ptr) {
    if      (typeName == "bool")   { ar.Serialize(prop.name, *(bool*)ptr); }
    else if (typeName == "i32")    { ar.Serialize(prop.name, *(i32*)ptr); }
    else if (typeName == "u32")    { ar.Serialize(prop.name, *(u32*)ptr); }
    else if (typeName == "i64")    { ar.Serialize(prop.name, *(i64*)ptr); }
    else if (typeName == "u64")    { ar.Serialize(prop.name, *(u64*)ptr); }
    else if (typeName == "f32")    { ar.Serialize(prop.name, *(f32*)ptr); }
    else if (typeName == "f64")    { ar.Serialize(prop.name, *(f64*)ptr); }
    else if (typeName == "String") { ar.Serialize(prop.name, *(String*)ptr); }
    else if (typeName == "float3") { ar.Serialize(prop.name, *(float3*)ptr); }
    else if (typeName == "float4") { ar.Serialize(prop.name, *(float4*)ptr); }
    else if (typeName == "float2") { ar.Serialize(prop.name, *(float2*)ptr); }
    else if (typeName == "quat")   { ar.Serialize(prop.name, *(quat*)ptr); }
    // else: 跳过未知类型
}
```

- [ ] **Step 4: 编写 SceneSerializer.cpp — Save 实现**

```cpp
bool SceneSerializer::Save(StringView filePath, World& world, SceneGraph& sg) {
    std::vector<u8> buffer;
    auto w32 = [&](u32 v) { for(int i=0;i<4;++i) buffer.push_back((u8)(v>>(i*8))); };
    auto w64 = [&](u64 v) { for(int i=0;i<8;++i) buffer.push_back((u8)(v>>(i*8))); };
    auto wStr = [&](StringView s) { w32((u32)s.size()); for(char c:s)buffer.push_back((u8)c); };

    w32(kMagic); w32(kVersion);

    // Entities
    u32 ec = 0; world.ForEachEntity([&](Entity){ec++;}); w32(ec);
    world.ForEachEntity([&](Entity e) {
        w64(e.id);
        // 收集组件数据
        struct CD { u64 hash; std::vector<u8> data; };
        std::vector<CD> comps;
        world.ForEachComponent(e, [&](Component* comp) {
            auto* cls = comp->GetClass(); if(!cls) return;
            serialize::BinaryArchive ar(serialize::ArchiveMode::Write);
            ar.BeginObject("comp");
            reflect::ForEachProperty<void>(comp, [&](const reflect::PropertyInfo& p, void* ptr) {
                if(p.flags & reflect::PF_Serializable) SerializeProperty(ar, p.typeName, p, ptr);
            });
            ar.EndObject();
            comps.push_back({cls->typeHash, ar.GetBuffer()});
        });
        w32((u32)comps.size());
        for(auto& [h,d] : comps) { w64(h); w32((u32)d.size()); buffer.insert(buffer.end(),d.begin(),d.end()); }
    });

    // Hierarchy
    struct P { u64 c,p; }; std::vector<P> prs;
    world.ForEachEntity([&](Entity e) { Entity p = sg.GetParent(e);
        if(p.IsValid()) prs.push_back({e.id,p.id}); });
    w32((u32)prs.size());
    for(auto& [c,p] : prs) { w64(c); w64(p); }

    std::ofstream f(String(filePath),std::ios::binary);
    if(!f) { HE_CORE_ERROR("SceneSerializer: write failed: {}", filePath); return false; }
    f.write((const char*)buffer.data(), buffer.size());
    HE_CORE_INFO("Scene saved: {} entities, {} pairs, {} bytes", ec, prs.size(), buffer.size());
    return true;
}
```

- [ ] **Step 5: 编写 SceneSerializer.cpp — Load 实现**

```cpp
bool SceneSerializer::Load(StringView filePath, World& world, SceneGraph& sg) {
    std::ifstream f(String(filePath),std::ios::binary|std::ios::ate);
    if(!f) { HE_CORE_ERROR("SceneSerializer: open failed: {}", filePath); return false; }
    std::vector<u8> buf(f.tellg()); f.seekg(0); f.read((char*)buf.data(),buf.size());
    usize p=0;
    auto r32=[&](){u32 v=0;for(int i=0;i<4;++i)v|=(u32)buf[p++]<<(i*8);return v;};
    auto r64=[&](){u64 v=0;for(int i=0;i<8;++i)v|=(u64)buf[p++]<<(i*8);return v;};

    if(r32()!=kMagic){ HE_CORE_ERROR("SceneSerializer: bad magic"); return false; }
    if(r32()!=kVersion){ HE_CORE_ERROR("SceneSerializer: bad version"); return false; }

    u32 ec=r32();
    for(u32 ei=0;ei<ec;++ei){
        u64 eid=r64();
        Entity ent=world.CreateEntity("Entity");

        u32 cc=r32();
        for(u32 ci=0;ci<cc;++ci){
            u64 hash=r64(); u32 ds=r32();

            auto* cls=reflect::TypeRegistry::Instance().FindClassByHash(hash);
            if(!cls){ p+=ds; continue; }

            // 按类型名创建组件（通过 AddComponent）
            StringView cn=cls->name;
            Component* added=nullptr;
            if      (cn=="TransformComponent")  added=world.AddComponent<TransformComponent>(ent);
            else if (cn=="MeshComponent")       added=world.AddComponent<MeshComponent>(ent);
            else if (cn=="CubeComponent")       added=world.AddComponent<CubeComponent>(ent);
            else if (cn=="SphereComponent")     added=world.AddComponent<SphereComponent>(ent);
            else if (cn=="DirectionalLight")    added=world.AddComponent<DirectionalLight>(ent);
            else if (cn=="PointLight")          added=world.AddComponent<PointLight>(ent);
            else if (cn=="SpotLight")           added=world.AddComponent<SpotLight>(ent);
            else if (cn=="LightComponent")      added=world.AddComponent<LightComponent>(ent);
            else { p+=ds; continue; }

            // 反序列化属性到已创建的组件
            serialize::BinaryArchive ar(serialize::ArchiveMode::Read);
            ar.SetBuffer(std::vector<u8>(buf.begin()+p,buf.begin()+p+ds));
            ar.BeginObject("comp");
            reflect::ForEachProperty<void>(added, [&](const reflect::PropertyInfo& pr, void* ptr) {
                if(pr.flags & reflect::PF_Serializable) SerializeProperty(ar, pr.typeName, pr, ptr);
            });
            ar.EndObject();
            p+=ds;
        }
    }

    // Hierarchy
    u32 pc=r32();
    for(u32 pi=0;pi<pc;++pi){ u64 c=r64(),p=r64(); sg.SetParent({c},{p}); }

    HE_CORE_INFO("Scene loaded: {} entities", ec);
    return true;
}
```

- [ ] **Step 6: 更新 CMakeLists.txt 并编译验证**

```bash
# 添加 Private/SceneSerializer.cpp 到 Engine/Editor/CMakeLists.txt target_sources
cd D:/Source/HugEngine/build && cmake --build . --target HugEngineEditor --config Debug
```

- [ ] **Step 7: 提交**

---

### Task 3: ContentBrowserPanel — 资源浏览器

**文件：**
- 创建: `Samples/Editor/Panels/ContentBrowserPanel.h`
- 创建: `Samples/Editor/Panels/ContentBrowserPanel.cpp`
- 修改: `Samples/Editor/EditorApp.cpp` (集成面板)
- 修改: `Samples/Editor/EditorApp.h` (添加成员)

**接口：**
- 消费: `EditorContext*`
- 产生: `void Initialize(EditorContext*)`, `void Render()`

- [ ] **Step 1: 编写 ContentBrowserPanel.h**

```cpp
// Samples/Editor/Panels/ContentBrowserPanel.h
#pragma once

#include "Core/Types.h"

namespace he::editor {
    class EditorContext;
}

namespace he::editor {

// ============================================================
// ContentBrowserPanel — 资源浏览器
//
// 左侧目录树 + 右侧文件平铺视图。
// 双击 glTF 文件 → glTFLoader 加载到场景。
// ============================================================
class ContentBrowserPanel {
public:
    void Initialize(EditorContext* ctx) { m_Ctx = ctx; }
    void Render();

private:
    void RenderDirectoryTree();
    void RenderFileGrid();
    void ImportGLB(const String& filePath);

    EditorContext* m_Ctx       = nullptr;
    String         m_CurrentPath = "Content";  // 默认浏览目录
    String         m_SelectedFile;
    bool           m_Visible     = false;       // 由菜单控制
};

} // namespace he::editor
```

- [ ] **Step 2: 编写 ContentBrowserPanel.cpp**

```cpp
// Samples/Editor/Panels/ContentBrowserPanel.cpp
#include "ContentBrowserPanel.h"
#include "Editor/EditorContext.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Asset/glTFLoader.h"
#include "imgui.h"

#include <filesystem>

namespace he::editor {

void ContentBrowserPanel::Render() {
    if (!m_Visible) return;

    ImGui::Begin("Content Browser", &m_Visible);

    // 左右分栏
    ImGui::Columns(2, "ContentBrowserColumns", true);
    ImGui::SetColumnWidth(0, 200);

    RenderDirectoryTree();

    ImGui::NextColumn();

    // 路径导航
    ImGui::Text("Path: %s", m_CurrentPath.c_str());
    ImGui::SameLine();
    if (ImGui::Button("..")) {
        auto parent = std::filesystem::path(m_CurrentPath).parent_path();
        if (!parent.empty()) m_CurrentPath = parent.string();
    }
    ImGui::Separator();

    RenderFileGrid();

    ImGui::Columns(1);
    ImGui::End();
}

void ContentBrowserPanel::RenderDirectoryTree() {
    ImGui::Text("Directories");
    ImGui::Separator();

    // 列出 Content 目录下的子目录
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(m_CurrentPath, ec)) {
        if (entry.is_directory()) {
            String name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str(), false)) {
                m_CurrentPath = entry.path().string();
            }
        }
    }
}

void ContentBrowserPanel::RenderFileGrid() {
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(m_CurrentPath, ec)) {
        if (entry.is_regular_file()) {
            String name = entry.path().filename().string();
            String ext  = entry.path().extension().string();

            // 仅显示支持的文件类型
            if (ext != ".glb" && ext != ".gltf" && ext != ".png" && ext != ".jpg" && ext != ".hescene")
                continue;

            bool isGLB = (ext == ".glb" || ext == ".gltf");

            // 平铺视图：每行 4 个
            ImGui::BeginGroup();
            ImGui::PushID(name.c_str());

            // 图标占位
            ImGui::Button(isGLB ? "3D" : "IMG", {60, 60});

            // 文件名（截断过长名称）
            String displayName = name;
            if (displayName.size() > 12)
                displayName = displayName.substr(0, 10) + "..";
            ImGui::Text("%s", displayName.c_str());

            // 双击导入
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (isGLB) {
                    ImportGLB(entry.path().string());
                } else if (ext == ".hescene") {
                    // 场景文件加载
                    // Phase 3-2: 通过 SceneSerializer::Load
                }
            }

            // 右键菜单
            if (ImGui::BeginPopupContextItem(name.c_str())) {
                if (isGLB && ImGui::MenuItem("Import to Scene")) {
                    ImportGLB(entry.path().string());
                }
                ImGui::MenuItem("Show in Explorer");
                ImGui::EndPopup();
            }

            ImGui::PopID();
            ImGui::EndGroup();

            // 每行 4 个，自动换行
            static int itemCount = 0;
            itemCount++;
            if (itemCount % 4 != 0)
                ImGui::SameLine();
        }
    }
}

void ContentBrowserPanel::ImportGLB(const String& filePath) {
    if (!m_Ctx) return;
    auto* world = m_Ctx->GetWorld();
    auto* sg    = m_Ctx->GetSceneGraph();
    if (!world || !sg) return;

    HE_CORE_INFO("Importing GLB: {}", filePath);
    auto result = asset::LoadGLB(*world, filePath);
    if (result.success) {
        // 将所有导入实体设为根节点
        for (auto& e : result.entities) {
            sg->SetParent(e, {kInvalidEntity});
        }
        HE_CORE_INFO("Imported {} entities from {}", result.meshCount, filePath);
    } else {
        HE_CORE_ERROR("Failed to import GLB: {}", result.error);
    }
}

} // namespace he::editor
```

- [ ] **Step 3: 集成到 EditorApp**

在 `EditorApp.h` 添加：
```cpp
std::unique_ptr<he::editor::ContentBrowserPanel> m_ContentBrowser;
#include "Panels/ContentBrowserPanel.h"  // in cpp
```

在 `InitEditor()` 中添加：
```cpp
m_ContentBrowser = std::make_unique<editor::ContentBrowserPanel>();
m_ContentBrowser->Initialize(m_EditorCtx.get());
```

在 `MainLoop()` 的 View 菜单添加开关，并在面板区域渲染：
```cpp
if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Content Browser", nullptr, &m_ContentBrowser->m_Visible);
    ImGui::EndMenu();
}
// ... 在 ImGui 区域内：
m_ContentBrowser->Render();
```

- [ ] **Step 4: 编译验证**

```bash
cd D:/Source/HugEngine/build && cmake --build . --target HugEditor --config Debug
```

- [ ] **Step 5: 提交**

---

### Task 4: EditorState — Play/Stop 模式 (EditorContext 扩增)

**文件：**
- 修改: `Engine/Editor/Public/Editor/EditorContext.h`
- 修改: `Engine/Editor/Private/EditorContext.cpp`

**接口：**
- 产生: `enum class EditorState : u8 { Edit, Play }`, `EditorState GetState()`, `bool IsPlaying()`, `void Play()`, `void Stop()`

- [ ] **Step 1: 扩增 EditorContext.h**

在现有 EditorContext 类中添加：

```cpp
// --- 编辑器状态（头文件 Enum，类内 public 区域）---
enum class EditorState : u8 { Edit, Play };

// --- 状态管理 ---
void Play()           { m_State = EditorState::Play; }
void Stop()           { m_State = EditorState::Edit; }
EditorState GetState() const { return m_State; }
bool IsPlaying()     const { return m_State == EditorState::Play; }

// private 成员新增：
EditorState m_State = EditorState::Edit;
```

- [ ] **Step 2: 编译验证 + 提交**

---

### Task 5: EditorApp Play/Stop 集成

**文件：**
- 修改: `Samples/Editor/EditorApp.cpp` (MainLoop 分叉 + 菜单按钮)

- [ ] **Step 1: MainLoop 分叉**

在 `MainLoop()` 中，用 `IsPlaying()` 包裹不同的渲染路径：

```cpp
// --- 帧逻辑分叉 ---
if (m_EditorCtx->IsPlaying()) {
    // Play 模式：仅更新世界 + 渲染场景（游戏相机）
    m_World->Update(dt);
    m_Viewport->RenderGameView(m_CmdList.get());
} else {
    // Edit 模式：现有完整渲染流程
    // ... (现有 m_Pipeline->BeginFrame + m_Viewport->Render + UI)
}
```

- [ ] **Step 2: 菜单栏 Play/Stop 按钮**

在菜单栏右侧添加：

```cpp
// 在 EndMainMenuBar 之前
ImGui::SameLine(ImGui::GetWindowWidth() - 100);
if (m_EditorCtx->IsPlaying()) {
    if (ImGui::Button("Stop")) m_EditorCtx->Stop();
} else {
    if (ImGui::Button("Play")) m_EditorCtx->Play();
}
```

Play 模式时简化 UI：
```cpp
m_ImGui->BeginFrame();
if (m_EditorCtx->IsPlaying()) {
    // 游戏覆盖层：FPS + 退出提示
    ImGui::SetNextWindowBgAlpha(0.3f);
    ImGui::Begin("GameOverlay", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("FPS: %.0f | Press ESC to Stop", 1.0f / dt);
    ImGui::End();
    // ESC 键退出
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        m_EditorCtx->Stop();
} else {
    // 现有完整编辑 UI
}
```

- [ ] **Step 3: ViewportPanel 游戏相机支持**

在 `ViewportPanel.h` 添加：
```cpp
void RenderGameView(rhi::IRHICommandList* cmdList);
```

实现：遍历 World 查找 CameraComponent（当前简化：使用默认相机 + 场景中心）：
```cpp
void ViewportPanel::RenderGameView(rhi::IRHICommandList* cmdList) {
    if (!m_Ctx || !m_Pipeline) return;
    // 简化：使用编辑器相机，后续扩展为场景 Camera
    m_Pipeline->RenderScene(cmdList,
        *m_Ctx->GetWorld(), *m_Ctx->GetSceneGraph(), m_Camera);
}
```

- [ ] **Step 4: 编译验证 + 提交**

---

### Task 6: 完善与集成

**文件：**
- 修改: `Samples/Editor/EditorApp.cpp` (布局调整)
- 修改: `docs/Phase1_Progress.md` (更新进度)

- [ ] **Step 1: File 菜单添加 Save/Load 场景入口**

```cpp
if (ImGui::MenuItem("Save Scene As...")) {
    editor::SceneSerializer::Save("scene.hescene", *m_World, *m_SceneGraph);
}
if (ImGui::MenuItem("Open Scene...")) {
    editor::SceneSerializer::Load("scene.hescene", *m_World, *m_SceneGraph);
}
```

- [ ] **Step 2: 更新进度文档**

在 `Phase1_Progress.md` 的 Editor 表格中添加新条目。

- [ ] **Step 3: Content 默认目录**

创建 `Content/` 目录并添加 `.gitkeep`：
```bash
mkdir -p Content/Models Content/Textures Content/Scenes
```

- [ ] **Step 4: 编译验证 + 提交**

---

## 依赖关系

```
Task 1 (ForEachComponent)
  ↓
Task 2 (SceneSerializer) ──┐
                            ├── Task 6 (完善/集成)
Task 3 (ContentBrowser) ────┤
                            │
Task 4 (EditorState) ──┐   │
  ↓                     ├───┘
Task 5 (EditorApp集成) ─┘
```

Task 2、3、4 可并行实现（各自独立文件，互不依赖）。
