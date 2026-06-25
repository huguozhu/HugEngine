# Phase 3-1 编辑器实现计划

> **For agentic workers:** 使用 superpowers:subagent-driven-development 逐步实施。步骤使用 checkbox (`- [ ]`) 跟踪。

**目标：** 实现 HugEngine 独立编辑器应用的最小可用版本——Viewport + Outliner + Details 三个核心面板。

**架构：** 新增 EditorContext 作为面板间共享状态管理器；三个面板通过 ImGui 渲染，通过 EditorContext 间接通信；EditorApp 管理生命周期和初始化顺序。

**技术栈：** C++20, CMake, MSVC 2026, Vulkan RHI, ImGui v1.91, GLFW, GLM

## 全局约束

- 所有新增代码必须附带中文注释
- 引擎模块新增文件放 `Engine/Editor/`，应用代码放 `Samples/Editor/`
- 复用不修改：现有 Engine 模块最小化修改（World 需加 `ForEachEntity`）
- C++ 命名空间：引擎代码 `he` / `he::editor`，应用代码 `he::editor`
- 遵循现有 ImGui GLFW+Vulkan 集成模式（参考 Sponza main.cpp）

---

### Task 1: World::ForEachEntity 迭代器

**文件：**
- 修改: `Engine/Scene/Public/Scene/World.h`

**接口：**
- 产生: `void World::ForEachEntity(std::function<void(Entity)> callback) const` — 遍历所有存活的实体

- [ ] **Step 1: 添加 ForEachEntity 方法声明**

在 `Engine/Scene/Public/Scene/World.h` 的 `World` 类中，`GetEntityCount()` 方法下方添加声明：

```cpp
/// 遍历所有存活实体（供编辑器等遍历使用）
void ForEachEntity(std::function<void(Entity)> callback) const {
    for (auto& e : m_Entities)
        callback(e);
}
```

- [ ] **Step 2: 编译验证**

```bash
cd D:/Source/HugEngine/build && cmake --build . --target HugEngineScene --config Debug
```

预期: 编译通过

- [ ] **Step 3: 提交**

```bash
git add Engine/Scene/Public/Scene/World.h
git commit -m "World 新增 ForEachEntity 迭代器，供编辑器遍历实体"
```

---

### Task 2: EditorContext — 编辑器共享状态

**文件：**
- 创建: `Engine/Editor/Public/Editor/EditorContext.h`
- 创建: `Engine/Editor/Private/EditorContext.cpp`
- 修改: `Engine/Editor/CMakeLists.txt`

**接口：**
- 消费: `World*`, `SceneGraph*`, `CommandHistory*`
- 产生:
  - `void Initialize(World*, SceneGraph*, CommandHistory*)` — 绑定引擎对象
  - `void SelectEntity(Entity e)` — 单选
  - `void DeselectAll()` — 清空选中
  - `const TArray<Entity>& GetSelection() const` — 获取选中列表
  - `bool IsSelected(Entity e) const` — 判断是否选中
  - `World* GetWorld() const`
  - `SceneGraph* GetSceneGraph() const`
  - `CommandHistory* GetCommandHistory() const`
  - `void OnSelectionChanged(std::function<void()> cb)` — 注册选中变化回调
  - `template<typename T> void SetProperty(Entity, const char* propName, const T& value)` — 属性编辑（走 Command）

- [ ] **Step 1: 编写 EditorContext.h 头文件**

```cpp
// Engine/Editor/Public/Editor/EditorContext.h
#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "Scene/Entity.h"

#include <functional>

namespace he {
    class World;
    class SceneGraph;
    class CommandHistory;
}

namespace he::editor {

// ============================================================
// EditorContext — 编辑器运行时共享上下文
//
// 所有面板通过 EditorContext 读写编辑器状态，
// 避免面板间直接耦合。选中变化通过回调通知面板刷新。
// ============================================================
class EditorContext {
public:
    EditorContext() = default;

    /// 绑定引擎对象（World/SceneGraph 不由此类拥有）
    void Initialize(he::World* world, he::SceneGraph* sg, he::CommandHistory* cmdHistory);

    // --- 选中管理 ---
    void SelectEntity(he::Entity e);
    void DeselectAll();
    const TArray<he::Entity>& GetSelection() const { return m_Selection; }
    bool IsSelected(he::Entity e) const;

    // --- 属性编辑（自动包装为 Command，支持 Undo/Redo）---
    template<typename T>
    void SetProperty(he::Entity entity, const char* propName, const T& value);

    // --- 引擎引用 ---
    he::World*        GetWorld()          const { return m_World; }
    he::SceneGraph*   GetSceneGraph()     const { return m_SceneGraph; }
    he::CommandHistory* GetCommandHistory() const { return m_CmdHistory; }

    // --- 回调 ---
    void OnSelectionChanged(std::function<void()> callback) {
        m_SelectionCallbacks.push_back(std::move(callback));
    }

private:
    void NotifySelectionChanged();

    he::World*          m_World      = nullptr;
    he::SceneGraph*     m_SceneGraph = nullptr;
    he::CommandHistory* m_CmdHistory = nullptr;
    TArray<he::Entity>  m_Selection;
    TArray<std::function<void()>> m_SelectionCallbacks;
};

} // namespace he::editor
```

- [ ] **Step 2: 编写 EditorContext.cpp 实现文件**

```cpp
// Engine/Editor/Private/EditorContext.cpp
#include "Editor/EditorContext.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Editor/Command.h"

namespace he::editor {

void EditorContext::Initialize(World* world, SceneGraph* sg, CommandHistory* cmdHistory) {
    m_World      = world;
    m_SceneGraph = sg;
    m_CmdHistory = cmdHistory;
}

void EditorContext::SelectEntity(Entity e) {
    m_Selection.clear();
    if (e.IsValid() && m_World && m_World->IsValid(e)) {
        m_Selection.push_back(e);
    }
    NotifySelectionChanged();
}

void EditorContext::DeselectAll() {
    m_Selection.clear();
    NotifySelectionChanged();
}

bool EditorContext::IsSelected(Entity e) const {
    for (auto& sel : m_Selection) {
        if (sel == e) return true;
    }
    return false;
}

void EditorContext::NotifySelectionChanged() {
    for (auto& cb : m_SelectionCallbacks) {
        cb();
    }
}

// 属性编辑模板方法 — 构建 PropertyChangeCommand 并推入 Undo 栈
// 注：Phase 3-1 不做完整反射路径的自动属性编辑，
// 具体编辑逻辑在 DetailsPanel 中按已知类型处理。
// SetProperty 模板为 3-2 扩展预留接口。

} // namespace he::editor
```

- [ ] **Step 3: 更新 Editor CMakeLists.txt**

在 `Engine/Editor/CMakeLists.txt` 的 `target_sources` 中添加 `Private/EditorContext.cpp`：

```cmake
# Editor — Editor framework (Phase 1-3)
add_library(HugEngineEditor STATIC)

target_sources(HugEngineEditor PRIVATE
    Private/CVar.cpp
    Private/Command.cpp
    Private/ImGuiIntegration.cpp
    Private/EditorContext.cpp
)
# ... 其余不变
```

- [ ] **Step 4: 编译验证**

```bash
cd D:/Source/HugEngine/build && cmake --build . --target HugEngineEditor --config Debug
```

预期: 编译通过

- [ ] **Step 5: 提交**

```bash
git add Engine/Editor/Public/Editor/EditorContext.h Engine/Editor/Private/EditorContext.cpp Engine/Editor/CMakeLists.txt
git commit -m "新增 EditorContext 编辑器共享上下文（选中管理+回调通知）"
```

---

### Task 3: 项目脚手架 — CMake + 目录结构

**文件：**
- 创建: `Samples/Editor/CMakeLists.txt`
- 修改: `Samples/CMakeLists.txt`
- 创建: `Samples/Editor/Panels/.gitkeep`（后续任务填入面板代码后删除）

- [ ] **Step 1: 创建 Editor CMakeLists.txt**

```cmake
# HugEditor — 独立编辑器应用
add_executable(HugEditor
    main.cpp
    EditorApp.cpp
    Panels/ViewportPanel.cpp
    Panels/OutlinerPanel.cpp
    Panels/DetailsPanel.cpp
)

target_include_directories(HugEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(HugEditor PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineRender
    HugEngineScene
    HugEngineEditor
)

add_dependencies(HugEditor CompileShaders)
```

- [ ] **Step 2: 更新 Samples CMakeLists.txt**

在 `Samples/CMakeLists.txt` 末尾添加：

```cmake
add_subdirectory(Editor)
```

- [ ] **Step 3: 创建 Panels 目录**

```bash
mkdir -p D:/Source/HugEngine/Samples/Editor/Panels
```

- [ ] **Step 4: 编译验证（预期失败 — 缺少源文件）**

```bash
cd D:/Source/HugEngine/build && cmake .. && cmake --build . --target HugEditor --config Debug 2>&1 | head -20
```

预期: CMake 配置通过，编译失败（main.cpp 等文件尚不存在）

- [ ] **Step 5: 提交**

```bash
git add Samples/Editor/CMakeLists.txt Samples/CMakeLists.txt
git commit -m "新增 HugEditor 项目脚手架（CMake 配置 + Panels 目录）"
```

---

### Task 4: EditorApp 主类 + main.cpp

**文件：**
- 创建: `Samples/Editor/EditorApp.h`
- 创建: `Samples/Editor/EditorApp.cpp`
- 创建: `Samples/Editor/main.cpp`

**接口：**
- 消费: GLFW, RHI, ImGuiIntegration, World, SceneGraph, ForwardPipeline, CommandHistory, EditorContext
- 产生: `int main()` — 可运行的编辑器窗口（三个面板为空占位）

- [ ] **Step 1: 编写 EditorApp.h**

```cpp
// Samples/Editor/EditorApp.h
#pragma once

// ============================================================
// EditorApp — HugEditor 应用主类
//
// 管理编辑器生命周期：初始化引擎 → 创建面板 → 主循环 → 清理
// ============================================================

#include "Core/Types.h"
#include <memory>

struct GLFWwindow;

namespace he {
    class World;
    class SceneGraph;
    class CommandHistory;
    class Engine;
namespace rhi {
    class IRHIDevice;
    class IRHISwapChain;
    class IRHICommandList;
}
namespace render {
    class ForwardPipeline;
}
namespace editor {
    class ImGuiIntegration;
    class EditorContext;
}
}

// 前向声明面板类
namespace he::editor {
    class ViewportPanel;
    class OutlinerPanel;
    class DetailsPanel;
}

class EditorApp {
public:
    EditorApp();
    ~EditorApp();

    int Run();

private:
    void InitEngine();      // GLFW 窗口 + RHI 设备 + SwapChain
    void InitScene();       // 默认场景（地面 + 光源）
    void InitEditor();      // ImGui + EditorContext + 面板
    void InitPipeline();    // ForwardPipeline
    void MainLoop();        // 帧循环
    void RenderUI();        // 面板渲染
    void Shutdown();

    // --- 底层 ---
    he::Engine*                     m_Engine    = nullptr;
    GLFWwindow*                     m_Window    = nullptr;
    std::unique_ptr<he::rhi::IRHIDevice>       m_Device;
    std::unique_ptr<he::rhi::IRHISwapChain>    m_SwapChain;
    std::unique_ptr<he::rhi::IRHICommandList>  m_CmdList;

    // --- 引擎系统 ---
    std::unique_ptr<he::World>                  m_World;
    std::unique_ptr<he::SceneGraph>             m_SceneGraph;
    std::unique_ptr<he::render::ForwardPipeline> m_Pipeline;
    std::unique_ptr<he::CommandHistory>          m_CmdHistory;
    std::unique_ptr<he::editor::ImGuiIntegration> m_ImGui;
    std::unique_ptr<he::editor::EditorContext>    m_EditorCtx;

    // --- 面板 ---
    std::unique_ptr<he::editor::ViewportPanel> m_Viewport;
    std::unique_ptr<he::editor::OutlinerPanel> m_Outliner;
    std::unique_ptr<he::editor::DetailsPanel>  m_Details;

    f64  m_LastTime  = 0.0;
    bool m_Running   = true;
};
```

- [ ] **Step 2: 编写 main.cpp**

```cpp
// Samples/Editor/main.cpp

// ============================================================
// HugEditor — HugEngine 独立编辑器入口
// ============================================================

#include "EditorApp.h"

int main() {
    EditorApp app;
    return app.Run();
}
```

- [ ] **Step 3: 编写 EditorApp.cpp — Init 和 Shutdown**

```cpp
// Samples/Editor/EditorApp.cpp

#include "EditorApp.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Render/Pipeline/ForwardPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/LightComponent.h"
#include "Scene/CubeComponent.h"
#include "Editor/ImGuiIntegration.h"
#include "Editor/EditorContext.h"
#include "Editor/Command.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace he;

// 前向声明面板（后续 Task 中实现）
namespace he::editor {

// 占位面板 — 后续 Task 替换
class ViewportPanel {
public:
    void Initialize(EditorContext*) {}
    void Render(rhi::IRHICommandList*) {}
};

class OutlinerPanel {
public:
    void Initialize(EditorContext*) {}
    void Render() {}
};

class DetailsPanel {
public:
    void Initialize(EditorContext*) {}
    void Render() {}
};

} // namespace he::editor

EditorApp::EditorApp()  = default;
EditorApp::~EditorApp() = default;

int EditorApp::Run() {
    InitEngine();
    InitScene();
    InitPipeline();
    InitEditor();
    MainLoop();
    Shutdown();
    return 0;
}

void EditorApp::InitEngine() {
    // 创建引擎和窗口
    EngineConfig config;
    config.appName      = "HugEditor";
    config.windowWidth  = 1920;
    config.windowHeight = 1080;
    config.enableVSync  = true;

    m_Engine = new Engine(config);
    m_Engine->Initialize();
    m_Window = m_Engine->GetWindow()->GetNativeHandle();

    // 创建 RHI 设备
    rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = rhi::Backend::Vulkan;
    rhiDesc.enableValidation = true;
    rhiDesc.windowHandle     = m_Engine->GetWindow()->GetNativeHandleRaw();

    m_Device = rhi::CreateDevice(rhiDesc.backend);
    m_Device->Initialize(rhiDesc);
    rhi::SetDevice(m_Device.get());

    // 创建 SwapChain
    m_SwapChain = m_Device->CreateSwapChain({
        .windowHandle = m_Engine->GetWindow()->GetNativeHandleRaw(),
        .width  = m_Engine->GetWindow()->GetWidth(),
        .height = m_Engine->GetWindow()->GetHeight(),
        .vsync  = true,
    });
}

void EditorApp::InitScene() {
    // 创建默认场景：地面 + 方向光
    m_World      = std::make_unique<World>();
    m_SceneGraph = std::make_unique<SceneGraph>(*m_World);

    // 地面
    Entity ground = m_World->CreateEntity("Ground");
    auto* gt = m_World->AddComponent<TransformComponent>(ground);
    gt->position = float3(0.0f, -1.5f, 0.0f);
    gt->scale    = float3(5.0f, 0.2f, 5.0f);
    auto* gc = m_World->AddComponent<CubeComponent>(ground);
    gc->halfExtent = 0.5f;
    gc->baseColorFactor  = float4(0.3f, 0.3f, 0.35f, 1.0f);
    gc->metallicFactor   = 0.0f;
    gc->roughnessFactor  = 0.9f;
    m_SceneGraph->SetParent(ground, {kInvalidEntity});

    // 方向光
    Entity lightEnt = m_World->CreateEntity("DirectionalLight");
    m_World->AddComponent<TransformComponent>(lightEnt);
    auto* dl = m_World->AddComponent<DirectionalLight>(lightEnt);
    dl->direction = float3(0.5f, -1.0f, 1.0f);
    dl->color     = float3(1.0f, 0.95f, 0.85f);
    dl->intensity = 5.0f;
    m_SceneGraph->SetParent(lightEnt, {kInvalidEntity});

    HE_CORE_INFO("Default scene created: {} entities", m_World->GetEntityCount());
}

void EditorApp::InitPipeline() {
    m_CmdList = m_Device->CreateCommandList();
    m_CmdList->SetSwapChain(m_SwapChain.get());

    m_Pipeline = std::make_unique<render::ForwardPipeline>();
    m_Pipeline->Initialize(m_Device.get());
    m_CmdList->SetPipeline(m_Pipeline->GetPipelineState());
}

void EditorApp::InitEditor() {
    m_CmdHistory = std::make_unique<CommandHistory>();

    m_EditorCtx = std::make_unique<editor::EditorContext>();
    m_EditorCtx->Initialize(m_World.get(), m_SceneGraph.get(), m_CmdHistory.get());

    m_ImGui = std::make_unique<editor::ImGuiIntegration>();
    m_ImGui->Initialize(m_Window, m_Device.get(), m_SwapChain.get());

    // 创建面板（当前为占位实现）
    m_Viewport  = std::make_unique<editor::ViewportPanel>();
    m_Outliner  = std::make_unique<editor::OutlinerPanel>();
    m_Details   = std::make_unique<editor::DetailsPanel>();

    m_Viewport->Initialize(m_EditorCtx.get());
    m_Outliner->Initialize(m_EditorCtx.get());
    m_Details->Initialize(m_EditorCtx.get());

    m_LastTime = glfwGetTime();
}

void EditorApp::MainLoop() {
    // 相机（临时内置，ViewportPanel 完成后移入面板）
    render::CameraData camera;
    camera.position = float3(0.0f, 2.0f, 8.0f);
    camera.forward  = float3(0.0f, 0.0f, -1.0f);
    camera.up       = float3(0.0f, 1.0f, 0.0f);
    camera.SetAspectRatio(
        static_cast<f32>(m_SwapChain->GetWidth()),
        static_cast<f32>(m_SwapChain->GetHeight()));

    while (!m_Engine->GetWindow()->ShouldClose()) {
        f64 now = glfwGetTime();
        f32 dt  = static_cast<f32>(now - m_LastTime);
        m_LastTime = now;

        m_Engine->GetWindow()->PollEvents();

        if (!m_SwapChain->AcquireNextImage())
            continue;

        // 渲染
        m_CmdList->Begin();
        m_CmdList->BeginRenderPass(1, rhi::Format::RGBA8_UNORM);

        m_Pipeline->BeginFrame(m_CmdList.get(),
            m_SwapChain->GetWidth(), m_SwapChain->GetHeight());

        // 场景渲染
        m_Pipeline->RenderScene(m_CmdList.get(), *m_World, *m_SceneGraph, camera);

        // UI
        m_ImGui->BeginFrame();

        // --- 菜单栏 ---
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                ImGui::MenuItem("New Scene", nullptr);
                ImGui::MenuItem("Open...", nullptr);
                ImGui::MenuItem("Save", nullptr);
                ImGui::Separator();
                ImGui::MenuItem("Exit", nullptr);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", nullptr,
                    m_CmdHistory->CanUndo())) {
                    m_CmdHistory->Undo();
                }
                if (ImGui::MenuItem("Redo", "Ctrl+Y", nullptr,
                    m_CmdHistory->CanRedo())) {
                    m_CmdHistory->Redo();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // 主布局：Viewport (左) | Details (右)
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float detailsWidth = 300.0f;
        float viewportWidth = avail.x - detailsWidth - 4.0f;

        // Viewport
        ImGui::BeginChild("ViewportRegion", {viewportWidth, avail.y * 0.75f}, true);
        ImGui::Text("Viewport (placeholder)");
        ImGui::EndChild();

        ImGui::SameLine();

        // Details
        ImGui::BeginChild("DetailsRegion", {detailsWidth, avail.y * 0.75f}, true);
        ImGui::Text("Details (placeholder)");
        ImGui::EndChild();

        // Outliner（底部区域）
        ImGui::BeginChild("OutlinerRegion", {avail.x, avail.y * 0.25f - 4.0f}, true);
        ImGui::Text("Outliner (placeholder)");
        ImGui::EndChild();

        m_ImGui->EndFrame(m_CmdList.get());

        m_CmdList->EndRenderPass();
        m_CmdList->End();

        m_Device->Submit(m_CmdList.get());
        m_SwapChain->Present(true);
    }
}

void EditorApp::Shutdown() {
    m_Device->WaitIdle();
    m_ImGui->Shutdown();
    m_Pipeline->Shutdown();
    // Engine 在析构时清理
    delete m_Engine;
}
```

- [ ] **Step 4: 编译验证**

```bash
cd D:/Source/HugEngine/build && cmake .. && cmake --build . --target HugEditor --config Debug
```

预期: 编译通过，生成 `build/Samples/Editor/Debug/HugEditor.exe`

- [ ] **Step 5: 运行验证**

```bash
cd D:/Source/HugEngine/build/Samples/Editor/Debug && timeout 3 ./HugEditor.exe 2>&1 || true
```

预期: 窗口启动，显示三个占位区域和菜单栏，3秒后退出无崩溃

- [ ] **Step 6: 提交**

```bash
git add Samples/Editor/EditorApp.h Samples/Editor/EditorApp.cpp Samples/Editor/main.cpp
git commit -m "新增 EditorApp 主框架（菜单栏 + 三面板占位布局 + 默认场景）"
```

---

### Task 5: OutlinerPanel — 场景层级树

**文件：**
- 创建: `Samples/Editor/Panels/OutlinerPanel.h`
- 创建: `Samples/Editor/Panels/OutlinerPanel.cpp`
- 修改: `Samples/Editor/EditorApp.cpp`（替换占位 OutlinerPanel）

**接口：**
- 消费: `EditorContext*` — GetWorld(), SelectEntity(), OnSelectionChanged()
- 产生: `void Initialize(EditorContext*)`, `void Render()` — 绘制层级树

- [ ] **Step 1: 编写 OutlinerPanel.h**

```cpp
// Samples/Editor/Panels/OutlinerPanel.h
#pragma once

// ============================================================
// OutlinerPanel — 场景层级树面板
//
// 显示 World 中所有实体的层级结构。
// 点击实体 → EditorContext::SelectEntity()。
// 支持搜索过滤和右键菜单。
// ============================================================

#include "Core/Types.h"

namespace he::editor {
    class EditorContext;
}

namespace he::editor {

class OutlinerPanel {
public:
    void Initialize(EditorContext* ctx) { m_Ctx = ctx; }

    /// 渲染场景层级树（每帧由 EditorApp 调用）
    void Render();

    /// 设置搜索过滤文本
    void SetFilter(const char* filter) { m_Filter = filter; }

private:
    /// 递归渲染实体及其子节点
    void RenderEntity(he::Entity entity, int depth);

    EditorContext* m_Ctx    = nullptr;
    String         m_Filter;  // 搜索过滤文本
};

} // namespace he::editor
```

- [ ] **Step 2: 编写 OutlinerPanel.cpp**

```cpp
// Samples/Editor/Panels/OutlinerPanel.cpp

#include "OutlinerPanel.h"
#include "Editor/EditorContext.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/LightComponent.h"
#include "imgui.h"

namespace he::editor {

void OutlinerPanel::Render() {
    if (!m_Ctx || !m_Ctx->GetWorld()) return;

    // 标题栏
    ImGui::Text("Outliner");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    char filterBuf[128] = {};
    if (!m_Filter.empty()) {
        std::strncpy(filterBuf, m_Filter.c_str(), sizeof(filterBuf) - 1);
    }
    if (ImGui::InputText("##Filter", filterBuf, sizeof(filterBuf))) {
        m_Filter = filterBuf;
    }
    ImGui::Separator();

    // 遍历所有根实体（无父节点的实体视为顶层）
    auto* world = m_Ctx->GetWorld();
    auto* sg    = m_Ctx->GetSceneGraph();

    world->ForEachEntity([&](Entity e) {
        Entity parent = sg ? sg->GetParent(e) : Entity{kInvalidEntity};
        if (!parent.IsValid()) {
            // 根实体
            RenderEntity(e, 0);
        }
    });
}

void OutlinerPanel::RenderEntity(Entity entity, int depth) {
    auto* world = m_Ctx->GetWorld();
    auto* sg    = m_Ctx->GetSceneGraph();
    if (!world || !sg) return;

    // 获取实体名称
    auto* t = world->GetComponent<TransformComponent>(entity);
    String name = "Entity#" + std::to_string(entity.id);
    if (t) {
        // 尝试从组件获取名称（后续可扩展为 NameComponent）
        name = "Entity #" + std::to_string(entity.id);
    }

    // 搜索过滤
    if (!m_Filter.empty()) {
        if (name.find(m_Filter) == String::npos) {
            // 不匹配，仍渲染子节点（可能有匹配的）
            // 简化处理：不匹配就不渲染子树
            // 完整实现需要检查子树中是否有匹配项
        }
    }

    // --- 实体类型图标 ---
    const char* icon = "  ";
    if (world->HasComponent<DirectionalLight>(entity) ||
        world->HasComponent<LightComponent>(entity)) {
        icon = "(L)";  // Light
    } else if (world->HasComponent<MeshComponent>(entity)) {
        icon = "(M)";  // Mesh
    }

    // 缩进
    String label = String(depth * 2, ' ') + icon + " " + name;

    // 选中高亮
    bool isSelected = m_Ctx->IsSelected(entity);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_Leaf;  // Leaf: 无箭头
    if (isSelected)
        flags |= ImGuiTreeNodeFlags_Selected;

    // 作为 Selectable 节点渲染
    bool open = ImGui::TreeNodeEx((void*)(uintptr_t)entity.id, flags, "%s", label.c_str());

    // 点击选中
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_Ctx->SelectEntity(entity);
    }

    // 右键菜单
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Delete")) {
            // Phase 3-1: 简单设为 DestroyEntity
            world->DestroyEntity(entity);
            if (isSelected) m_Ctx->DeselectAll();
        }
        if (ImGui::MenuItem("Rename")) {
            // TODO: 3-2 实现重命名弹窗
        }
        ImGui::EndPopup();
    }

    if (open) {
        // 渲染子实体（遍历所有实体，找 parent == entity 的子节点）
        world->ForEachEntity([&](Entity child) {
            Entity parent = sg->GetParent(child);
            if (parent == entity) {
                RenderEntity(child, depth + 1);
            }
        });
        ImGui::TreePop();
    }
}

} // namespace he::editor
```

- [ ] **Step 3: 集成到 EditorApp.cpp**

替换 `EditorApp.cpp` 中占位的 `OutlinerPanel` 类定义：

```cpp
// 删除占位定义，改为 #include：
#include "Panels/OutlinerPanel.h"

// InitEditor() 中：
m_Outliner->Initialize(m_EditorCtx.get());

// MainLoop() 中 Outliner 区域替换为：
m_Outliner->Render();
```

- [ ] **Step 4: 编译验证**

```bash
cd D:/Source/HugEngine/build && cmake --build . --target HugEditor --config Debug
```

预期: 编译通过

- [ ] **Step 5: 提交**

```bash
git add Samples/Editor/Panels/OutlinerPanel.h Samples/Editor/Panels/OutlinerPanel.cpp Samples/Editor/EditorApp.cpp
git commit -m "新增 OutlinerPanel 场景层级树（实体列表+选中+右键菜单）"
```

---

### Task 6: DetailsPanel — 属性面板

**文件：**
- 创建: `Samples/Editor/Panels/DetailsPanel.h`
- 创建: `Samples/Editor/Panels/DetailsPanel.cpp`
- 修改: `Samples/Editor/EditorApp.cpp`（替换占位 DetailsPanel）

**接口：**
- 消费: `EditorContext*` — GetSelection(), GetWorld(), OnSelectionChanged()
- 产生: `void Initialize(EditorContext*)`, `void Render()` — 绘制属性编辑器

- [ ] **Step 1: 编写 DetailsPanel.h**

```cpp
// Samples/Editor/Panels/DetailsPanel.h
#pragma once

// ============================================================
// DetailsPanel — 选中对象属性检查器
//
// 显示 EditorContext 当前选中实体的所有组件及其属性。
// 通过 ReflectionAPI 枚举属性，按类型渲染编辑器控件。
// ============================================================

#include "Core/Types.h"

namespace he {
    class World;
    class Entity;
}
namespace he::editor {
    class EditorContext;
}

namespace he::editor {

class DetailsPanel {
public:
    void Initialize(EditorContext* ctx);

    /// 渲染属性面板（每帧调用）
    void Render();

private:
    /// 渲染一个组件的所有属性
    void RenderComponent(he::World* world, he::Entity entity,
                         const char* componentName);

    /// 渲染单个 Transform 属性（手动处理，性能优于纯反射路径）
    void RenderTransform(he::World* world, he::Entity entity);

    /// 渲染单个 Mesh 属性
    void RenderMesh(he::World* world, he::Entity entity);

    /// 渲染单个 Light 属性
    void RenderLight(he::World* world, he::Entity entity);

    EditorContext* m_Ctx = nullptr;
};

} // namespace he::editor
```

- [ ] **Step 2: 编写 DetailsPanel.cpp — 框架和 Transform 编辑**

```cpp
// Samples/Editor/Panels/DetailsPanel.cpp

#include "DetailsPanel.h"
#include "Editor/EditorContext.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/LightComponent.h"
#include "Reflect/ReflectionAPI.h"
#include "imgui.h"

namespace he::editor {

void DetailsPanel::Initialize(EditorContext* ctx) {
    m_Ctx = ctx;
}

void DetailsPanel::Render() {
    if (!m_Ctx) return;

    ImGui::Text("Details");
    ImGui::Separator();

    auto& selection = m_Ctx->GetSelection();
    if (selection.empty()) {
        ImGui::TextDisabled("No entity selected");
        return;
    }

    Entity entity = selection[0];
    auto* world = m_Ctx->GetWorld();
    if (!world || !world->IsValid(entity)) {
        ImGui::TextDisabled("Invalid entity");
        return;
    }

    // 实体名称 / ID
    ImGui::Text("Entity: #%llu", static_cast<unsigned long long>(entity.id));
    ImGui::Separator();

    // 按组件类型渲染属性
    if (world->HasComponent<TransformComponent>(entity))
        RenderTransform(world, entity);

    if (world->HasComponent<MeshComponent>(entity))
        RenderMesh(world, entity);

    if (world->HasComponent<DirectionalLight>(entity))
        RenderLight(world, entity);
}

void DetailsPanel::RenderTransform(World* world, Entity entity) {
    if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    auto* t = world->GetComponent<TransformComponent>(entity);
    if (!t) return;

    // 位置
    float pos[3] = { t->position.x, t->position.y, t->position.z };
    if (ImGui::DragFloat3("Position", pos, 0.1f)) {
        t->position = float3(pos[0], pos[1], pos[2]);
        if (auto* sg = m_Ctx->GetSceneGraph())
            sg->MarkDirty(entity);
    }

    // 缩放
    float scl[3] = { t->scale.x, t->scale.y, t->scale.z };
    if (ImGui::DragFloat3("Scale", scl, 0.1f, 0.01f, 100.0f)) {
        t->scale = float3(scl[0], scl[1], scl[2]);
    }

    // 旋转（显示四元数分量，简化编辑）
    ImGui::Text("Rotation: (%.2f, %.2f, %.2f, %.2f)",
        t->rotation.w, t->rotation.x, t->rotation.y, t->rotation.z);
    // Phase 3-2: 改为欧拉角 DragFloat3
}

void DetailsPanel::RenderMesh(World* world, Entity entity) {
    if (!ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    auto* mesh = world->GetComponent<MeshComponent>(entity);
    if (!mesh) return;

    // Base Color
    float color[4] = {
        mesh->baseColorFactor.x,
        mesh->baseColorFactor.y,
        mesh->baseColorFactor.z,
        mesh->baseColorFactor.w
    };
    if (ImGui::ColorEdit4("Base Color", color)) {
        mesh->baseColorFactor = float4(color[0], color[1], color[2], color[3]);
    }

    // Metallic
    ImGui::DragFloat("Metallic", &mesh->metallicFactor, 0.01f, 0.0f, 1.0f);

    // Roughness
    ImGui::DragFloat("Roughness", &mesh->roughnessFactor, 0.01f, 0.0f, 1.0f);

    // Emissive (glTF 2.0)
    float emissive[3] = {
        mesh->emissiveFactor.x,
        mesh->emissiveFactor.y,
        mesh->emissiveFactor.z
    };
    if (ImGui::ColorEdit3("Emissive", emissive)) {
        mesh->emissiveFactor = float3(emissive[0], emissive[1], emissive[2]);
    }

    // Metallic-Roughness 纹理索引（只读显示）
    ImGui::Text("MetallicRoughness Tex: %u", mesh->metallicRoughnessTexIndex);
    ImGui::Text("BaseColor Tex: %u", mesh->baseColorTexIndex);
    ImGui::Text("Normal Tex: %u", mesh->normalTexIndex);
}

void DetailsPanel::RenderLight(World* world, Entity entity) {
    if (!ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    auto* light = world->GetComponent<DirectionalLight>(entity);
    if (!light) return;

    // 方向
    float dir[3] = { light->direction.x, light->direction.y, light->direction.z };
    if (ImGui::DragFloat3("Direction", dir, 0.05f, -1.0f, 1.0f)) {
        light->direction = glm::normalize(float3(dir[0], dir[1], dir[2]));
    }

    // 颜色
    float col[3] = { light->color.x, light->color.y, light->color.z };
    if (ImGui::ColorEdit3("Color", col)) {
        light->color = float3(col[0], col[1], col[2]);
    }

    // 强度
    ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 100.0f);
}

} // namespace he::editor
```

- [ ] **Step 3: 集成到 EditorApp.cpp**

```cpp
// 删除占位 DetailsPanel，替换为：
#include "Panels/DetailsPanel.h"

// MainLoop() 中 Details 区域替换为：
m_Details->Render();
```

- [ ] **Step 4: 编译验证**

```bash
cd D:/Source/HugEngine/build && cmake --build . --target HugEditor --config Debug
```

预期: 编译通过

- [ ] **Step 5: 提交**

```bash
git add Samples/Editor/Panels/DetailsPanel.h Samples/Editor/Panels/DetailsPanel.cpp Samples/Editor/EditorApp.cpp
git commit -m "新增 DetailsPanel 属性检查器（Transform/Mesh/Light 组件编辑）"
```

---

### Task 7: ViewportPanel — 3D 视口

**文件：**
- 创建: `Samples/Editor/Panels/ViewportPanel.h`
- 创建: `Samples/Editor/Panels/ViewportPanel.cpp`
- 修改: `Samples/Editor/EditorApp.cpp`（替换占位 ViewportPanel，移入相机逻辑）

**接口：**
- 消费: `EditorContext*`, `ForwardPipeline*`, `IRHIDevice*`
- 产生: `void Initialize(...)`, `void Render(IRHICommandList*)` — 渲染 3D 视口

- [ ] **Step 1: 编写 ViewportPanel.h**

```cpp
// Samples/Editor/Panels/ViewportPanel.h
#pragma once

// ============================================================
// ViewportPanel — 3D 场景视口
//
// 在 ImGui 窗口中渲染 3D 场景。
// 独立 Editor Camera（WASD + 鼠标右键旋转）。
// ============================================================

#include "Core/Types.h"
#include "Render/Pipeline/Camera.h"
#include "Math/Math.h"

struct GLFWwindow;

namespace he::rhi {
    class IRHIDevice;
    class IRHICommandList;
}
namespace he::render {
    class ForwardPipeline;
}
namespace he {
    class World;
    class SceneGraph;
}
namespace he::editor {
    class EditorContext;
}

namespace he::editor {

class ViewportPanel {
public:
    /// @param window GLFW 窗口句柄（用于捕获输入）
    void Initialize(EditorContext* ctx, he::render::ForwardPipeline* pipeline,
                    GLFWwindow* window);

    /// 渲染视口（每帧调用，在 RenderPass 内部）
    void Render(he::rhi::IRHICommandList* cmdList);

    /// 获取编辑器相机引用（供外部读取相机状态）
    const he::render::CameraData& GetCamera() const { return m_Camera; }

private:
    void UpdateCamera(float deltaTime);

    EditorContext*              m_Ctx      = nullptr;
    he::render::ForwardPipeline* m_Pipeline = nullptr;
    GLFWwindow*                  m_Window   = nullptr;

    he::render::CameraData m_Camera;

    // 鼠标状态
    bool   m_RightMouseDown = false;
    double m_LastMouseX     = 0.0;
    double m_LastMouseY     = 0.0;
    float  m_Yaw            = 0.0f;
    float  m_Pitch          = 0.0f;
    float  m_MoveSpeed      = 5.0f;
    float  m_LookSpeed      = 0.003f;
};

} // namespace he::editor
```

- [ ] **Step 2: 编写 ViewportPanel.cpp — 初始化和相机控制**

```cpp
// Samples/Editor/Panels/ViewportPanel.cpp

#include "ViewportPanel.h"
#include "Editor/EditorContext.h"
#include "Render/Pipeline/ForwardPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace he::editor {

void ViewportPanel::Initialize(EditorContext* ctx,
                                render::ForwardPipeline* pipeline,
                                GLFWwindow* window) {
    m_Ctx      = ctx;
    m_Pipeline = pipeline;
    m_Window   = window;

    // 初始化编辑器相机
    m_Camera.position = float3(0.0f, 2.0f, 8.0f);
    m_Camera.forward  = float3(0.0f, -0.2f, -1.0f);
    m_Camera.up       = float3(0.0f, 1.0f, 0.0f);
    m_Camera.SetAspectRatio(16.0f / 9.0f);

    // 从初始朝向反算 yaw/pitch
    m_Yaw   = std::atan2(m_Camera.forward.x, -m_Camera.forward.z);
    m_Pitch = std::asin(m_Camera.forward.y);
}

void ViewportPanel::Render(rhi::IRHICommandList* cmdList) {
    if (!m_Ctx || !m_Pipeline) return;

    // 计算帧时间
    static f64 lastTime = glfwGetTime();
    f64 now = glfwGetTime();
    float dt = static_cast<float>(now - lastTime);
    lastTime = now;

    // 面板视口区域
    ImGui::Text("Viewport");
    ImGui::Separator();

    ImVec2 viewportSize = ImGui::GetContentRegionAvail();
    if (viewportSize.x <= 0 || viewportSize.y <= 0)
        return;

    // 更新编辑器相机
    UpdateCamera(dt);

    // 渲染场景到视口区域
    m_Pipeline->RenderScene(cmdList,
        *m_Ctx->GetWorld(),
        *m_Ctx->GetSceneGraph(),
        m_Camera);

    // MVP 阶段：直接使用 ForwardPipeline 渲染到 BackBuffer
    // Phase 3-2: 改为渲染到 off-screen texture → ImGui::Image
}

void ViewportPanel::UpdateCamera(float deltaTime) {
    if (!m_Window) return;

    // --- 焦点检查：仅当视口区域被 hovered/focused 时才捕获输入 ---
    // MVP 简化：始终捕获（后续改为焦点敏感）

    // --- 鼠标旋转（右键拖拽）---
    bool mouseDown = glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    if (mouseDown && !m_RightMouseDown) {
        m_RightMouseDown = true;
        glfwGetCursorPos(m_Window, &m_LastMouseX, &m_LastMouseY);
        glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    } else if (!mouseDown && m_RightMouseDown) {
        m_RightMouseDown = false;
        glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    } else if (mouseDown && m_RightMouseDown) {
        double cx, cy;
        glfwGetCursorPos(m_Window, &cx, &cy);
        float dx = static_cast<float>(cx - m_LastMouseX);
        float dy = static_cast<float>(cy - m_LastMouseY);
        m_LastMouseX = cx;
        m_LastMouseY = cy;

        m_Yaw   += dx * m_LookSpeed;
        m_Pitch -= dy * m_LookSpeed;
        m_Pitch  = glm::clamp(m_Pitch, -1.5f, 1.5f);
    }

    // --- WASD 移动 ---
    float speed = m_MoveSpeed * deltaTime;
    if (glfwGetKey(m_Window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        speed *= 3.0f;

    float3 right = glm::normalize(glm::cross(m_Camera.forward, m_Camera.up));
    float3 move(0.0f);

    if (glfwGetKey(m_Window, GLFW_KEY_W) == GLFW_PRESS) move += m_Camera.forward;
    if (glfwGetKey(m_Window, GLFW_KEY_S) == GLFW_PRESS) move -= m_Camera.forward;
    if (glfwGetKey(m_Window, GLFW_KEY_A) == GLFW_PRESS) move -= right;
    if (glfwGetKey(m_Window, GLFW_KEY_D) == GLFW_PRESS) move += right;
    if (glfwGetKey(m_Window, GLFW_KEY_E) == GLFW_PRESS) move += m_Camera.up;
    if (glfwGetKey(m_Window, GLFW_KEY_Q) == GLFW_PRESS) move -= m_Camera.up;

    if (glm::dot(move, move) > 0.0001f)
        m_Camera.position += glm::normalize(move) * speed;

    // 更新朝向
    float3 forward;
    forward.x = cos(m_Pitch) * sin(m_Yaw);
    forward.y = sin(m_Pitch);
    forward.z = -cos(m_Pitch) * cos(m_Yaw);
    m_Camera.forward = glm::normalize(forward);
}

} // namespace he::editor
```

- [ ] **Step 3: 集成到 EditorApp.cpp**

修改 `EditorApp.cpp`：
- 删除占位 ViewportPanel 定义
- 添加: `#include "Panels/ViewportPanel.h"`
- 删除 MainLoop 中的内嵌 Camera 变量
- Viewport 区域改为: `m_Viewport->Render(m_CmdList.get());`
- `InitEditor()` 中改为: `m_Viewport->Initialize(m_EditorCtx.get(), m_Pipeline.get(), m_Window);`

完整改动后的 MainLoop 核心渲染段：

```cpp
// 场景渲染（由 ViewportPanel 驱动）
m_Viewport->Render(m_CmdList.get());

// UI
m_ImGui->BeginFrame();

// --- 菜单栏（同前）---
// ... 不变 ...

// Outliner
m_Outliner->Render();

// Details
m_Details->Render();

// 状态栏
ImGui::Begin("StatusBar");
ImGui::Text("Selected: %d | FPS: %.0f",
    (int)m_EditorCtx->GetSelection().size(),
    1.0f / (dt > 0 ? dt : 0.016f));
ImGui::End();

m_ImGui->EndFrame(m_CmdList.get());
```

- [ ] **Step 4: 编译验证**

```bash
cd D:/Source/HugEngine/build && cmake --build . --target HugEditor --config Debug
```

预期: 编译通过

- [ ] **Step 5: 运行验证**

```bash
# 手动运行 3-5 秒验证：窗口 + 3D 视口 + 面板交互
```

预期: 
- 窗口标题 "HugEditor"
- Viewport 区域渲染 3D 场景
- Outliner 显示实体列表（Ground, DirectionalLight）
- 点击 Outliner 中实体 → Details 显示属性
- WASD + 右键旋转编辑器相机
- Ctrl+Z 菜单项可用

- [ ] **Step 6: 提交**

```bash
git add Samples/Editor/Panels/ViewportPanel.h Samples/Editor/Panels/ViewportPanel.cpp Samples/Editor/EditorApp.cpp
git commit -m "新增 ViewportPanel 3D 视口（独立编辑器相机+WASD飞行控制）"
```

---

### Task 8: 完善与集成测试

**文件：**
- 修改: `Samples/Editor/EditorApp.cpp`（布局调整、状态栏、Undo/Redo 快捷键）
- 修改: `docs/Phase1_Progress.md`（更新进度）

- [ ] **Step 1: Undo/Redo 键盘快捷键**

在 `MainLoop()` 中添加快捷键处理：

```cpp
// Ctrl+Z / Ctrl+Y 快捷键
if (ImGui::IsKeyPressed(ImGuiKey_Z) && ImGui::GetIO().KeyCtrl) {
    m_CmdHistory->Undo();
}
if (ImGui::IsKeyPressed(ImGuiKey_Y) && ImGui::GetIO().KeyCtrl) {
    m_CmdHistory->Redo();
}
```

- [ ] **Step 2: 窗口调整大小回调**

在 `InitEngine()` 后添加：

```cpp
m_Engine->GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
    m_SwapChain->Resize(w, h);
    m_CmdList->SetSwapChain(m_SwapChain.get());
});
```

- [ ] **Step 3: 最终编译与运行验证**

```bash
cd D:/Source/HugEngine/build && cmake --build . --target HugEditor --config Debug
```

预期: 编译通过，运行功能正常

- [ ] **Step 4: 更新进度文档**

在 `docs/Phase1_Progress.md` 的 "L8 — Editor" 表格中添加：

```markdown
| EditorContext | `Public/Editor/EditorContext.h`, `Private/EditorContext.cpp` | ✅ |
| EditorApp | `Samples/Editor/EditorApp.h/.cpp`, `main.cpp` | ✅ |
| Panels | `Panels/ViewportPanel`, `OutlinerPanel`, `DetailsPanel` | ✅ |
```

- [ ] **Step 5: 最终提交**

```bash
git add Samples/Editor/EditorApp.cpp docs/Phase1_Progress.md
git commit -m "完善 HugEditor：Undo/Redo 快捷键 + 窗口调整 + 进度文档更新"
```

---

## 依赖关系

```
Task 1 (World::ForEachEntity)
  ↓
Task 2 (EditorContext)
  ↓
Task 3 (CMake 脚手架)
  ↓
Task 4 (EditorApp + main) ←── 可运行的窗口（占位面板）
  ↓
Task 5 (OutlinerPanel) ──┐
Task 6 (DetailsPanel)  ──┤  可并行实现
Task 7 (ViewportPanel) ──┘
  ↓
Task 8 (完善/集成/文档)
```
