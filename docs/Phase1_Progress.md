# HugEngine 实施进度

> 最后更新: 2026-06-26

## 整体进度

Phase 1（核心骨架）+ Phase 2（基础设施）+ Phase 3（编辑器完整功能）已完成。
Phase 4（渲染增强）进行中。

- **Phase 1**: 8 个引擎模块落地（Core/Reflect/RHI/Shader/Render/Scene/Asset/Editor）
- **Phase 2**: PBR 前向管线 + 反射序列化 + 多光源 + Bindless 基础设施
- **Phase 3**: 独立编辑器应用（Viewport/Outliner/Details/ContentBrowser/ProjectSettings/PlayStop）
- **Phase 4**: glTF 纹理加载 + 阴影基础设施 + Per-primitive 纹理绑定
- **示例**: 01.Triangle + 02.Cube + 03.Sponza + HugEditor

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
| ReflectionAPI | `Public/Reflect/ReflectionAPI.h` | ✅ |
| ReflectionMacros | `Public/Reflect/ReflectionMacros.h` | ✅ |
| TypeInfo | `Public/Reflect/TypeInfo.h` | ✅ |
| TypeRegistry | `Private/TypeRegistry.cpp` | ✅ |
| Attribute | `Public/Reflect/Attribute.h` | ✅ |
| Serialize | `Serialize/Public/Serialize/Archive.h`, `BinaryArchive.h`, `Private/BinaryArchive.cpp` | ✅ |

### L2 — RHI（渲染硬件接口层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| 公共接口 | `Public/RHI/RHI.h`, `Types.h`, `Buffer.h`, `Shader.h`, `SwapChain.h`, `CommandList.h` | ✅ |
| Descriptor Sets | CreateDescriptorSetLayout / AllocateDescriptorSet / UpdateDescriptorSet(tex+sampler) / BindDescriptorSet | ✅ |
| Push Constants / Barrier | SetPushConstants / PipelineBarrier | ✅ |
| Vulkan 后端 | `Vulkan/VulkanDevice.cpp`, `VulkanResources.cpp`, `VulkanInternal.h` | ✅ |
| 深度专用管线 | colorAttachmentCount=0 → depth-only render pass | ✅ |
| 描述符池 | StorageBuffer=512, CombinedImageSampler=1024, maxSets=256 | ✅ |

### L3 — Shader（着色器层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Slang 编译器 | `CMakeLists.txt` (slangc, Vulkan SDK 自带) | ✅ |
| SPIR-V 嵌入 | `spv_to_header.py` | ✅ |
| PBR Shader | `Shaders/PBR.vert.slang`, `PBR.frag.slang`, `pbr_common.slang` | ✅ |
| Shadow Shader | `Shaders/Shadow.vert.slang`, `Shadow.frag.slang` | ✅ |
| 示例 Shader | `Shaders/Triangle.vert.slang`, `Triangle.frag.slang` | ✅ |
| PCF 阴影采样 | 3×3 kernel 手动深度比较（pbr_common.slang） | ✅ |
| 多纹理采样 | BaseColor/Normal/MetallicRoughness/Occlusion + ShadowMap | ✅ |

### L4 — Render（渲染层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Forward 管线 | `Pipeline/ForwardPipeline.h/.cpp` (PBR + 多光源 + Per-primitive 纹理) | ✅ |
| Material 系统 | `Pipeline/Material.h` (glTF 2.0 PBR + GPUObjectData + GPULight + GPUShadowData) | ✅ |
| Camera 系统 | `Pipeline/Camera.h` (视图/投影/视锥体) | ✅ |
| Shadow 基础设施 | Shadow PSO + 阴影贴图纹理 + PCF 采样器 + 描述符绑定 | ✅ |
| Per-primitive 纹理 | CreateTextureDescriptorSet → 独立描述符集 → 渲染时直接 Bind | ✅ |
| 阴影通道渲染 | ShadowPass 深度渲染到 ShadowMap | ⬜ 待实施 |
| Deferred / 后处理 | — | ❌ |

### L5 — Scene（场景层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Entity | `Public/Scene/Entity.h` | ✅ |
| Component | `Public/Scene/Component.h` | ✅ |
| Transform | `Public/Scene/Transform.h` | ✅ |
| World | `Public/Scene/World.h`, `Private/World.cpp` | ✅ |
| SceneGraph | `Public/Scene/SceneGraph.h`, `Private/SceneGraph.cpp` | ✅ |
| MeshComponent | `Public/Scene/MeshComponent.h` (+ GPU 纹理字段 + 描述符集字段) | ✅ |
| CubeComponent | `Public/Scene/CubeComponent.h` | ✅ |
| SphereComponent | `Public/Scene/SphereComponent.h` | ✅ |
| LightComponent | `Public/Scene/LightComponent.h` (+ 阴影参数: shadowBias/normalBias/strength) | ✅ |

### Asset（资源层）
| 子系统 | 文件 | 状态 |
|--------|------|------|
| glTF 加载器 | `Public/Asset/glTFLoader.h`, `Private/glTFLoader.cpp` (cgltf 驱动) | ✅ |
| 纹理路径提取 | ApplyMaterial → 5 种纹理 URI (BC/Normal/MR/Occlusion/Emissive) | ✅ |
| 自动下载测试资源 | `cmake/DownloadAssets.cmake` (git sparse-checkout Sponza) | ✅ |

### L8 — Editor（编辑器层）
| 子系统 | 文件 | 状态 |
|--------|------|------|
| ImGui 集成 | `Public/Editor/ImGuiIntegration.h`, `Private/ImGuiIntegration.cpp` | ✅ |
| ImGui RP 兼容 | 深度附件 + subpass dependency 匹配 Forward RP | ✅ |
| EditorApp | `Samples/Editor/EditorApp.h/.cpp`, `main.cpp` | ✅ |
| 其他面板 | Viewport/Outliner/Details/ContentBrowser/ProjectSettings | ✅ |

## 示例项目

| 项目 | 说明 | 状态 |
|------|------|------|
| Samples/01.Triangle | 使用 IRHI 接口渲染三角形 | ✅ |
| Samples/02.Cube | PBR 材质立方体+球体，WASD 自由相机 | ✅ |
| Samples/03.Sponza | 加载 Sponza glTF 场景，PBR + 纹理 + 阴影基础 | ✅ |

## 工程配置

| 项目 | 说明 |
|------|------|
| 构建系统 | CMake 4.3.1 + MSVC 2026 |
| 第三方库 | git submodule + CDN (ImGui) + stb_image（纹理解码） |
| 着色器语言 | Slang (slangc, Vulkan SDK 自带) |
| C++ 标准 | C++20 (Engine) / C++17 (External) |
| 资源下载 | cmake/DownloadAssets.cmake（Sponza glTF 自动下载） |

## Phase 4 关键技术成果（进行中）

| 子系统 | 实现 |
|--------|------|
| glTF 加载器重写 | cgltf v1.14 替代手工字符串解析，支持 .glb/.gltf |
| 纹理加载管线 | stb_image 解码 → RHI Texture 上传 → 纹理缓存去重 |
| 着色器纹理采样 | PBR.frag 采样 4 种 PBR 贴图（BC/Normal/MR/AO） |
| Per-primitive 纹理 | 独立描述符集 → 渲染时 BindDescriptorSet 直接切换 |
| 阴影基础设施 | Shadow PSO + 阴影贴图 + PCF 采样器 + GPUShadowData SSBO |
| Vulkan RP 兼容 | ImGui RP 与 Forward RP 匹配深度附件和 subpass 依赖 |
| 中文日志 | SetConsoleOutputCP(CP_UTF8) 解决 Windows 控制台乱码 |
| 配置持久化 | Content/Config/03_Sponza.cfg 保存/加载相机+灯光参数 |
| ImGui 性能面板 | FPS/帧时间/相机位置/灯光参数/场景统计 |

## 已知限制

| 问题 | 影响 | 计划 |
|------|------|------|
| 视口 3D 渲染到全屏 backbuffer | 3D 仅在 ImGui 透明区域可见 | 离屏渲染 (ImGui::Image) |
| 无 Gizmo 操作 | 无法拖拽移动/旋转/缩放 | 编辑器增强 |
| 无鼠标拾取选择 | 只能通过 Outliner 选中 | 编辑器增强 |
| 阴影通道未执行 | ShadowMap 为占位纹理，无实际阴影 | 实现 ShadowPass 渲染 |
| 所有 primitive 共用全局纹理 | 多材质场景纹理错误 | Per-primitive 描述符集（已完成） |

## 待实施 (Phase 4+)

- 阴影通道渲染（ShadowMap 深度写入 + 布局转换）
- Cascaded Shadow Maps (CSM) + 点光/聚光阴影
- IBL（Image-Based Lighting）+ 环境贴图
- Bindless 纹理数组（Texture2D[] + materialID 索引）
- 离屏视口渲染（RHI Texture → ImGui::Image）
- Gizmo 操作 + 鼠标拾取选择
- GPU Driven 渲染（视锥剔除 / Hi-Z / ExecuteIndirect）
- Prefab 系统 + Asset Registry + NameComponent
- 骨骼动画 + 粒子系统
- Nanite / VSM / Virtual Texturing
- Ray Tracing + GI
