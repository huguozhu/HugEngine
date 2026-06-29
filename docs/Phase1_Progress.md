# HugEngine 实施进度

> 最后更新: 2026-06-30

## 整体进度

Phase 1（核心骨架）+ Phase 2（基础设施）+ Phase 3（编辑器完整功能）已完成。
Phase 4（渲染增强）进行中。

- **Phase 1**: 8 个引擎模块落地（Core/Reflect/RHI/Shader/Render/Scene/Asset/Editor）
- **Phase 2**: PBR 前向管线 + 反射序列化 + 多光源 + Bindless 基础设施
- **Phase 3**: 独立编辑器应用（Viewport/Outliner/Details/ContentBrowser/ProjectSettings/PlayStop）
- **Phase 4**: 阴影系统 + HDR 管线 + 点光源阴影 + ImGui 控制面板
- **示例**: 01.Triangle + 02.Cube + 03.Sponza + HugEditor

## 模块完成度

### L0 — Core（平台层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Types | `Core/Types.h` | ✅ |
| Platform | `Core/Platform.h`, `Platform/Window.h/.cpp` (GLFW) | ✅ |
| Assert | `Core/Assert.h` | ✅ |
| Log | `Core/Log.h/.cpp` (spdlog) + LogLevel 枚举 | ✅ |
| Engine | `Core/Engine.h/.cpp` (+ EngineConfig::logLevel) | ✅ |
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
| TextureUsage | Cubemap 标志 + IRHITexture::GetNativeHandle(face) | ✅ |
| Descriptor Sets | CreateDescriptorSetLayout / AllocateDescriptorSet / UpdateDescriptorSet(tex+sampler) / BindDescriptorSet | ✅ |
| Push Constants / Barrier | SetPushConstants / PipelineBarrier（全局 + 图像布局转换） | ✅ |
| 离屏渲染 | BeginOffscreenPass / EndOffscreenPass（非 SwapChain 渲染目标） | ✅ |
| Vulkan 后端 | `Vulkan/VulkanDevice.cpp`, `VulkanResources.cpp`, `VulkanInternal.h` | ✅ |
| 深度专用管线 | colorAttachmentCount=0 → depth-only render pass | ✅ |
| Cubemap 支持 | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT + 6 面独立 ImageView | ✅ |
| 描述符池 | StorageBuffer=512, CombinedImageSampler=1024, maxSets=256 | ✅ |

### L3 — Shader（着色器层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Slang 编译器 | `CMakeLists.txt` (slangc, Vulkan SDK 自带) | ✅ |
| SPIR-V 嵌入 | `spv_to_header.py` | ✅ |
| PBR Shader | `Shaders/PBR.vert.slang`, `PBR.frag.slang`, `pbr_common.slang` | ✅ |
| Shadow Shader | `Shaders/Shadow.vert.slang`, `Shadow.frag.slang` | ✅ |
| ToneMap Shader | `Shaders/ToneMap.vert.slang`, `ToneMap.frag.slang`（ACES + LinearToSRGB） | ✅ |
| 示例 Shader | `Shaders/Triangle.vert.slang`, `Triangle.frag.slang` | ✅ |
| PCF 阴影采样 | 3×3 kernel 手动深度比较（pbr_common.slang） | ✅ |
| Cubemap 阴影采样 | SamplePointShadow() 方向向量采样 + 距离比较（pbr_common.slang） | ✅ |
| 多纹理采样 | BaseColor/Normal/MetallicRoughness/Occlusion + ShadowMap + PointShadowCube | ✅ |

### L4 — Render（渲染层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Forward 管线 | `Pipeline/ForwardPipeline.h/.cpp` (PBR + 多光源 + Per-primitive 纹理) | ✅ |
| Material 系统 | `Pipeline/Material.h` (glTF 2.0 PBR + GPUObjectData + GPULight + GPUShadowData) | ✅ |
| Camera 系统 | `Pipeline/Camera.h` (视图/投影/视锥体) | ✅ |
| 方向光阴影 | BeginShadowPass → RenderShadowPass → EndShadowPass + Barrier | ✅ |
| 点光源阴影 | RenderPointShadowPass：Cubemap 6 面 90° 透视渲染 | ✅ |
| HDR 离屏管线 | BeginHDRPass(RGBA16_FLOAT+D32) → EndHDRPass → ToneMap(ACES+sRGB) → SwapChain | ✅ |
| Per-primitive 纹理 | CreateTextureDescriptorSet → 独立描述符集 → 渲染时直接 Bind | ✅ |
| ImGui 控制面板 | 相机位置/朝向/速度 + 光源方向/颜色/强度 + 阴影参数可编辑 | ✅ |
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
| LightComponent | `Public/Scene/LightComponent.h` (Point/Directional/Spot + 阴影参数) | ✅ |

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
| ImGui 中文字体 | simhei.ttf 黑体 + CJK 字形范围（简体中文） | ✅ |
| ImGui RP 兼容 | 深度附件 + subpass dependency 匹配 Forward RP | ✅ |
| EditorApp | `Samples/Editor/EditorApp.h/.cpp`, `main.cpp` | ✅ |
| 其他面板 | Viewport/Outliner/Details/ContentBrowser/ProjectSettings | ✅ |

## 示例项目

| 项目 | 说明 | 状态 |
|------|------|------|
| Samples/01.Triangle | 使用 IRHI 接口渲染三角形 | ✅ |
| Samples/02.Cube | PBR 材质立方体+球体，WASD 自由相机，HDR + ToneMap | ✅ |
| Samples/03.Sponza | Sponza glTF + PBR + 方向光阴影 + 点光阴影 + HDR + ImGui 控制面板 | ✅ |

## 工程配置

| 项目 | 说明 |
|------|------|
| 构建系统 | CMake 4.3.1 + MSVC 2026 |
| 第三方库 | git submodule + CDN (ImGui) + stb_image（纹理解码） |
| 着色器语言 | Slang (slangc, Vulkan SDK 自带) |
| C++ 标准 | C++20 (Engine) / C++17 (External) |
| 资源下载 | cmake/DownloadAssets.cmake（Sponza glTF 自动下载） |
| 验证层 | Vulkan Validation Layers，03.Sponza 和 02.Cube 均 0 VUID 错误 |

## Phase 4 关键技术成果

| 子系统 | 实现 |
|--------|------|
| glTF 加载器重写 | cgltf v1.14 替代手工字符串解析，支持 .glb/.gltf |
| 纹理加载管线 | stb_image 解码 → RHI Texture 上传 → 纹理缓存去重 |
| 着色器纹理采样 | PBR.frag 采样 4 种 PBR 贴图（BC/Normal/MR/AO） |
| Per-primitive 纹理 | 独立描述符集 → 渲染时 BindDescriptorSet 直接切换 |
| 方向光阴影 | Shadow PSO + ShadowMap + PCF 3×3 + Barrier 布局转换 |
| 点光源阴影 | Cubemap 6 面 90° 透视渲染 + SamplePointShadow() |
| HDR 离屏管线 | RGBA16_FLOAT 离屏目标 → ACES ToneMap → sRGB SwapChain |
| ToneMap 后处理 | 全屏三角形 + CombinedImageSampler + ACES + LinearToSRGB |
| ImGui 中文字体 | simhei.ttf 黑体 + CJK Unified Ideographs |
| ImGui 控制面板 | 相机 DragFloat3/Slider、光源 SliderFloat3/ColorEdit3、阴影 Checkbox/DragFloat |
| Vulkan RP 兼容 | ImGui RP 与 Forward RP 匹配深度附件和 subpass 依赖 |
| 中文日志 | SetConsoleOutputCP(CP_UTF8) 解决 Windows 控制台乱码 |
| LogLevel 控制 | LogLevel 枚举 + EngineConfig::logLevel（Sponza 默认 Error） |
| 配置持久化 | Content/Config/03_Sponza.cfg 保存/加载相机+灯光参数 |

## 已知限制

| 问题 | 影响 | 计划 |
|------|------|------|
| 视口 3D 渲染到全屏 backbuffer | 3D 仅在 ImGui 透明区域可见 | 离屏渲染 (ImGui::Image) |
| 无 Gizmo 操作 | 无法拖拽移动/旋转/缩放 | 编辑器增强 |
| 无鼠标拾取选择 | 只能通过 Outliner 选中 | 编辑器增强 |
| 点光阴影无视锥剔除 | 6 面 × 103 mesh ≈ 618 draw/帧，FPS ~20 | Per-face 视锥剔除 |
| 无点光阴影 PCF | 点光阴影为单采样，边缘硬 | 多采样软阴影 |

## 待实施 (Phase 4+)

- ~~阴影通道渲染（ShadowMap 深度写入 + 布局转换）~~ ✅
- ~~点光源阴影~~ ✅
- ~~HDR 离屏渲染管线~~ ✅
- ~~ImGui 可编辑控制面板~~ ✅
- ~~ImGui 中文字体~~ ✅
- ~~LogLevel 日志等级控制~~ ✅
- Cascaded Shadow Maps (CSM)
- 面积光源（Area Light）：矩形/碟形光源、LTC 线性变换余弦近似
- IBL（Image-Based Lighting）+ 环境贴图
- Bindless 纹理数组（Texture2D[] + materialID 索引）
- 离屏视口渲染（RHI Texture → ImGui::Image）
- Gizmo 操作 + 鼠标拾取选择
- GPU Driven 渲染（视锥剔除 / Hi-Z / ExecuteIndirect）
- Prefab 系统 + Asset Registry + NameComponent
- 骨骼动画 + 粒子系统
- Nanite / VSM / Virtual Texturing
- Ray Tracing + GI
