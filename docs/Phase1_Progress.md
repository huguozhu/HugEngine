# HugEngine 实施进度

> 最后更新: 2026-06-26

## 整体进度

Phase 1（核心骨架）+ Phase 2（基础设施）+ Phase 3（编辑器完整功能）已完成。

- **Phase 1**: 8 个引擎模块落地（Core/Reflect/RHI/Shader/Render/Scene/Asset/Editor）
- **Phase 2**: PBR 前向管线 + 反射序列化 + 多光源 + Bindless 基础设施
- **Phase 3**: 独立编辑器应用（Viewport/Outliner/Details/ContentBrowser/ProjectSettings/PlayStop）
- **示例**: Triangle + Sponza + HugEditor

## 模块完成度

### L0 — Core（平台层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Types | `Core/Types.h` | ✅ |
| Platform | `Core/Platform.h`, `Platform/Window.h/.cpp` (GLFW) | ✅ |
| Assert | `Core/Assert.h` | ✅ |
| Log | `Core/Log.h/.cpp` (spdlog) | ✅ |
| Engine | `Core/Engine.h/.cpp` | ✅ |
| Math | `Math/Math.h` (GLM), `Math/Geometry.h` | ✅ |
| Containers | `Containers/Array.h` (TArray, TMap, TInlineVec) | ✅ |
| Memory | `Memory/Allocator.h` (IAllocator, MallocAllocator) | ✅ |
| Threading | `Threading/JobSystem.h/.cpp` (Taskflow) | ✅ |

### L1 — Reflect（反射层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| ReflectionAPI | `Public/Reflect/ReflectionAPI.h` (ClassInfo/PropertyInfo/TypeRegistry/ForEachProperty) | ✅ |
| ReflectionMacros | `Public/Reflect/ReflectionMacros.h` (HE_CLASS/HE_BEGIN_REGISTER 等后端宏) | ✅ |
| TypeInfo | `Public/Reflect/TypeInfo.h` (兼容入口) | ✅ |
| TypeRegistry | `Private/TypeRegistry.cpp` | ✅ |
| Attribute | `Public/Reflect/Attribute.h` | ✅ |
| Serialize | `Serialize/Public/Serialize/Archive.h`, `BinaryArchive.h`, `Private/BinaryArchive.cpp` | ✅ |

### L2 — RHI（渲染硬件接口层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| 公共接口 | `Public/RHI/RHI.h`, `Types.h`, `Buffer.h`, `Shader.h`, `SwapChain.h`, `CommandList.h` | ✅ |
| Descriptor Sets | CreateDescriptorSetLayout / AllocateDescriptorSet / UpdateDescriptorSet / BindDescriptorSet | ✅ |
| Push Constants / Barrier | SetPushConstants / PipelineBarrier / CopyBuffer | ✅ |
| Vulkan 后端 | `Vulkan/VulkanDevice.cpp`, `VulkanResources.cpp`, `VulkanInternal.h` | ✅ |
| Vulkan 功能 | Device/SwapChain/CommandList/Buffer/Texture/Sampler/Pipeline/Depth/DescriptorPool/Bindless | ✅ |
| D3D12 后端 | — | ❌ 已删除 |

### L3 — Shader（着色器层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Slang 编译器 | `CMakeLists.txt` (slangc, Vulkan SDK 自带) | ✅ |
| SPIR-V 嵌入 | `spv_to_header.py` | ✅ |
| PBR Shader | `Shaders/PBR.vert.slang`, `PBR.frag.slang`, `pbr_common.slang` | ✅ |
| ToneMap Shader | `Shaders/ToneMap.vert.slang`, `ToneMap.frag.slang` | ✅ |
| 示例 Shader | `Shaders/Triangle.vert.slang`, `Triangle.frag.slang` | ✅ |
| 公共头文件 | `Shaders/common.slang` | ✅ |

### L4 — Render（渲染层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| RenderGraph | `Public/Render/RenderGraph.h`, `Private/RenderGraph.cpp` | ✅ |
| Forward 管线 | `Pipeline/ForwardPipeline.h/.cpp` (PBR + 多光源 + GPU Scene Upload) | ✅ |
| Material 系统 | `Pipeline/Material.h` (glTF 2.0 PBR + GPUObjectData + GPULight) | ✅ |
| Camera 系统 | `Pipeline/Camera.h` (视图/投影/视锥体) | ✅ |
| BindlessManager | 基础设施就绪（descriptor indexing + shader 声明） | ✅ |
| Deferred / 后处理 | — | ❌ Phase 3+ |

### L5 — Scene（场景层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Entity | `Public/Scene/Entity.h` | ✅ |
| Component | `Public/Scene/Component.h` (基类+生命周期) | ✅ |
| Transform | `Public/Scene/Transform.h` | ✅ |
| World | `Public/Scene/World.h`, `Private/World.cpp` (ECS 容器) | ✅ |
| SceneGraph | `Public/Scene/SceneGraph.h`, `Private/SceneGraph.cpp` (层级变换) | ✅ |
| MeshComponent | `Public/Scene/MeshComponent.h`, `Private/MeshComponent.cpp` (glTF PBR 材质) | ✅ |
| CubeComponent | `Public/Scene/CubeComponent.h`, `Private/CubeComponent.cpp` | ✅ |
| SphereComponent | `Public/Scene/SphereComponent.h`, `Private/SphereComponent.cpp` | ✅ |
| LightComponent | `Public/Scene/LightComponent.h`, `Private/LightComponent.cpp` (Directional/Point/Spot) | ✅ |
| Prefab | — | ❌ Phase 3+ |

### Asset（资源层）
| 子系统 | 文件 | 状态 |
|--------|------|------|
| glTF 加载器 | `Public/Asset/glTFLoader.h`, `Private/glTFLoader.cpp` (GLB 2.0) | ✅ |
| Registry | — | ❌ Phase 3+ |

### L8 — Editor（编辑器层）
| 子系统 | 文件 | 状态 |
|--------|------|------|
| CVar | `Public/Editor/CVar.h`, `Private/CVar.cpp` | ✅ |
| Command | `Public/Editor/Command.h`, `Private/Command.cpp` (Undo/Redo) | ✅ |
| ImGui 集成 | `Public/Editor/ImGuiIntegration.h`, `Private/ImGuiIntegration.cpp` (v1.91) | ✅ |
| EditorContext | `Public/Editor/EditorContext.h`, `Private/EditorContext.cpp` | ✅ |
| PropertyChangeCommand | `Public/Editor/Command.h` (lambda Undo/Redo，任意属性类型) | ✅ |
| EditorApp | `Samples/Editor/EditorApp.h/.cpp`, `main.cpp` | ✅ |
| SceneSerializer | `Public/Editor/SceneSerializer.h`, `Private/SceneSerializer.cpp` (.hescene 二进制格式) | ✅ |
| Play/Stop | EditorContext::Play()/Stop() + ESC 退出 + 游戏覆盖层 | ✅ |
| ViewportPanel | `Panels/ViewportPanel` (编辑器相机 + WASD 飞行控制) | ✅ |
| OutlinerPanel | `Panels/OutlinerPanel` (层级树 + 搜索 + 右键菜单 + O(N) 优化) | ✅ |
| DetailsPanel | `Panels/DetailsPanel` (Transform/Mesh/三光源/材质扩展，全部 Undo/Redo) | ✅ |
| ContentBrowser | `Panels/ContentBrowserPanel` (目录树 + 文件平铺 + glTF/.hescene 导入) | ✅ |
| ProjectSettings | `Panels/ProjectSettingsPanel` (CVar 浏览/搜索/编辑) | ✅ |
| 光照编辑器 | DirectionalLight/PointLight/SpotLight 完整属性编辑 | ✅ |
| Material 编辑器 | AlphaMode/DoubleSided/Unlit/AO 等 PBR 参数 | ✅ |

## 示例项目

| 项目 | 说明 | 状态 |
|------|------|------|
| Samples/Triangle | 使用 IRHI 接口渲染三角形 | ✅ |
| Samples/Sponza | PBR 材质立方体+球体，WASD 自由相机，ImGui 覆盖层 | ✅ |

## 工程配置

| 项目 | 说明 |
|------|------|
| 构建系统 | CMake 4.3.1 + MSVC 2026 |
| 第三方库 | git submodule + CDN (ImGui) |
| 着色器语言 | Slang (slangc, Vulkan SDK 自带) |
| C++ 标准 | C++20 (Engine) / C++17 (External) |

## Phase 2 关键技术成果

| 子系统 | 实现 |
|--------|------|
| 前向 PBR 管线 | Cook-Torrance BRDF + ACES ToneMapping, Slang Shader |
| 反射拆分 | ReflectionAPI (稳定接口) + ReflectionMacros (宏后端，可替换为 C++26) |
| 序列化 | BinaryArchive，基于 ReflectionAPI，SerializeObject<T>() 自动遍历属性 |
| 光照组件 | LightComponent 基类 + DirectionalLight/PointLight/SpotLight 子类 |
| Descriptor Sets | IRHI 接口 + Vulkan 后端 (DescriptorPool + SetLayout + Set + Update) |
| 多光源 | GPULight[8] Storage Buffer, PBR Shader 循环遍历 + 衰减 |
| ImGui 集成 | v1.91 + GLFW + Vulkan 后端, Sponza 覆盖面板 |
| Bindless 基础设施 | VK_EXT_descriptor_indexing + Shader Texture2D[] 声明 |
| GPU Scene Upload | GPUObjectData[1024] Storage Buffer + objectIndex Push Constant |

## Phase 3 关键技术成果

| 子系统 | 实现 |
|--------|------|
| 编辑器应用 | 独立 HugEditor.exe，GLFW+Vulkan 窗口，ImGui 面板系统 |
| EditorContext | 共享状态管理 + 选中回调 + EditorState (Edit/Play) |
| 属性编辑 | PropertyChangeCommand (lambda Undo/Redo) + IsItemDeactivatedAfterEdit |
| 场景序列化 | .hescene 二进制格式，反射驱动 Save/Load，Entity ID 重映射 |
| Content Browser | std::filesystem 文件浏览 + glTF 导入 + .hescene 加载 |
| Play/Stop | Edit/Play 模式切换，World::Update 驱动，ESC 退出 |
| 光照编辑器 | DirectionalLight/PointLight/SpotLight 完整编辑，锥角度数显示 |
| Material 编辑器 | AlphaMode 下拉/AlphaCutoff/DoubleSided/Unlit/AO |
| Project Settings | CVar 浏览/搜索/类型分派编辑 |
| 视口渲染 | 透明子窗口 + 不透明面板遮盖，3D 仅视口内可见 |
| 光源收集修复 | ForEach 遍历三种光源子类（type_index 分桶） |
| EditorMain 布局 | NoBackground 透明主窗口 + Viewport 透明/Details&Outliner 不透明 |

## 已知限制

| 问题 | 影响 | 计划 |
|------|------|------|
| 视口 3D 渲染到全屏 backbuffer | 3D 仅在 ImGui 透明区域可见 | Phase 4 离屏渲染 (ImGui::Image) |
| glTFLoader 使用字符串解析 JSON | 复杂模型可能失败 | Phase 4 引入 json/nlohmann |
| 无 Gizmo 操作 | 无法拖拽移动/旋转/缩放 | Phase 4 编辑器增强 |
| 无鼠标拾取选择 | 只能通过 Outliner 选中 | Phase 4 编辑器增强 |
| Outliner 无实体名称 | 显示 Entity #N | Phase 4 NameComponent |

## 待实施 (Phase 4+)

- 离屏视口渲染 (RHI Texture → ImGui::Image)
- Gizmo 操作 (平移/旋转/缩放) + 鼠标拾取选择
- Shadow Maps (CSM + PCF) + IBL (Split-Sum)
- Bindless 纹理实际采样 (stb_image → RHI Texture → bindless index)
- GPU Driven 渲染 (视锥剔除 / Hi-Z / ExecuteIndirect)
- Prefab 系统 + Asset Registry + NameComponent
- 骨骼动画 + 粒子系统
- Nanite / VSM / Virtual Texturing
- Ray Tracing + GI + 神经网络渲染 (DLSS/FSR/XeSS)

- GPU Driven 渲染 (视锥剔除 / Hi-Z / ExecuteIndirect)
- Bindless 纹理实际采样 (stb_image → RHI Texture → bindless index)
- Shadow Maps (CSM + PCF) + IBL (Split-Sum)
- Nanite / VSM / Virtual Texturing
- Ray Tracing + GI + 神经网络渲染 (DLSS/FSR/XeSS)
