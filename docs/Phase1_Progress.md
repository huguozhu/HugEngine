# HugEngine 实施进度

> 最后更新: 2026-07-01

## 整体进度

Phase 1-4 核心渲染功能完成，多线程渲染 Phase 5 部分完成，Skybox 系统完成。

- **Phase 1**: 8 个引擎模块落地（Core/Reflect/RHI/Shader/Render/Scene/Asset/Editor）
- **Phase 2**: PBR 前向管线 + 反射序列化 + 多光源 + Bindless 基础设施
- **Phase 3**: 独立编辑器应用（Viewport/Outliner/Details/ContentBrowser/ProjectSettings/PlayStop）
- **Phase 4**: 阴影系统 + HDR 管线 + 点光源阴影 + ImGui 控制面板
- **Skybox**: 天空盒系统（VS 全屏三角形 + 逆VP + Cubemap 采样 + ECS 组件）
- **多线程 Phase 5-1**: 三缓冲帧环 + 非阻塞提交 + 持久映射（FPS 20→74）
- **多线程 Phase 5-2**: RHI 辅助命令缓冲（ForwardPipeline 集成受阻）
- **示例**: 01.Triangle + 02.Cube + 03.Sponza + HugEditor

## 模块完成度

### L0 — Core（平台层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Types/Platform/Assert/Log/Engine | 基础平台层 | ✅ |
| Math | `Math/Math.h` (GLM), `Math/Geometry.h` | ✅ |
| Containers/Memory | TArray, TMap, Allocator | ✅ |
| Threading | `Threading/JobSystem.h/.cpp` (Taskflow) | ✅ |

### L1 — Reflect（反射层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| ReflectionAPI/Macros/TypeInfo | 反射基础 | ✅ |
| TypeRegistry/Attribute | 类型注册 | ✅ |
| Serialize | Archive, BinaryArchive | ✅ |

### L2 — RHI（渲染硬件接口层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| 公共接口 | RHI.h, Types.h, Buffer.h, Shader.h, SwapChain.h, CommandList.h | ✅ |
| Descriptor Sets / Push Constants | CreateDescriptorSetLayout / SetPushConstants / PipelineBarrier | ✅ |
| 离屏渲染 | BeginOffscreenPass / EndOffscreenPass | ✅ |
| Vulkan 后端 | VulkanDevice.cpp, VulkanResources.cpp, VulkanInternal.h | ✅ |
| 深度专用管线 / Cubemap / 描述符池 | colorAttachmentCount=0, CUBE_COMPATIBLE, maxSets=256 | ✅ |
| 三缓冲帧环 | kMaxFramesInFlight=3 cmd pools/buffers/fences（Phase 5-1） | ✅ |
| 持久映射 | VulkanBuffer 构造时 vkMapMemory，Map/Unmap 变 no-op（Phase 5-1） | ✅ |
| 辅助命令缓冲 | BeginSecondary/ExecuteSecondary/CreateSecondaryCommandList（Phase 5-2） | ✅ |
| FB 生命周期 | 延迟销毁队列（离屏）+ 标记重建（SwapChain） | ✅ |
| 空顶点输入 | vertexBindingDescriptionCount=0（SV_VertexID 全屏三角形） | ✅ |
| SetPipeline 绑定 | vkCmdBindPipeline（关键修复） | ✅ |

### L3 — Shader（着色器层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Slang 编译器 / SPIR-V 嵌入 | CMakeLists + spv_to_header.py | ✅ |
| PBR Shader | PBR.vert.slang, PBR.frag.slang, pbr_common.slang | ✅ |
| Shadow Shader | Shadow.vert.slang, Shadow.frag.slang | ✅ |
| ToneMap Shader | ToneMap.vert.slang, ToneMap.frag.slang | ✅ |
| **Skybox Shader** | **Skybox.vert.slang, Skybox.frag.slang** | ✅ |
| 示例 Shader | Triangle.vert.slang, Triangle.frag.slang | ✅ |
| PCF / Cubemap 阴影采样 | 3x3 kernel + SamplePointShadow() | ✅ |

### L4 — Render（渲染层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Forward 管线 | ForwardPipeline.h/.cpp (PBR + 多光源 + 纹理 + 阴影 + 天空盒) | ✅ |
| Material 系统 | Material.h (GPUObjectData + GPULight + GPUShadowData) | ✅ |
| Camera 系统 | Camera.h (视图/投影/视锥体) | ✅ |
| 方向光阴影 | BeginShadowPass → RenderShadowPass → EndShadowPass + Barrier | ✅ |
| 点光源阴影 | RenderPointShadowPass：Cubemap 6 面 90 度透视渲染 | ✅ |
| HDR 离屏管线 | BeginHDRPass → EndHDRPass → ToneMap(ACES+sRGB) → SwapChain | ✅ |
| Per-primitive 纹理 | 独立描述符集 → 渲染时直接 Bind | ✅ |
| **天空盒** | **RenderSkybox()：全屏三角形 + Equal 深度测试 + Cubemap** | ✅ |
| ImGui 控制面板 | 相机/光源/阴影参数可编辑 | ✅ |
| Deferred / 后处理 | — | ❌ |

### L5 — Scene（场景层）✅
| 子系统 | 文件 | 状态 |
|--------|------|------|
| Entity/Component/Transform/World/SceneGraph | 核心场景系统 | ✅ |
| MeshComponent | GPU 纹理字段 + 描述符集字段 | ✅ |
| CubeComponent / SphereComponent | 形状组件 | ✅ |
| LightComponent | Point/Directional/Spot + enabled 开关 + 阴影参数 | ✅ |
| **SkyboxComponent** | **Cubemap 纹理 + 采样器 + intensity + enabled** | ✅ |

## Skybox 系统关键技术细节

| 细节 | 实现 |
|------|------|
| 几何体 | 全屏三角形（3 顶点，SV_VertexID，无 Vertex Buffer） |
| 顶点着色器 | NDC 坐标 → 逆 ViewProj（去平移）反算世界空间方向 |
| 片元着色器 | TextureCube.Sample(方向) × intensity |
| 深度测试 | Equal（仅远平面 depth=1.0 空白区域绘制） |
| 渲染顺序 | 场景不透明物体 → 天空盒（场景覆盖天空盒） |
| HDR 全景 | stbi_loadf 加载 skybox.hdr → Equirectangular → Cubemap 转换 |
| 面纹理 | stbi_load 加载 6 面 daylight*.png |
| 描述符集 | 缓存复用（避免 frame-in-flight UAF 纹理错乱） |
| vkCmdBindPipeline | SetPipeline 必须调用（天空盒不渲染根因） |

## 已知限制

| 问题 | 影响 | 计划 |
|------|------|------|
| 视口 3D 渲染到全屏 backbuffer | 3D 仅在 ImGui 透明区域可见 | 离屏渲染 (ImGui::Image) |
| 无 Gizmo 操作 | 无法拖拽移动/旋转/缩放 | 编辑器增强 |
| 无鼠标拾取选择 | 只能通过 Outliner 选中 | 编辑器增强 |
| 点光阴影无视锥剔除 | 6 面 × 103 mesh ≈ 618 draw/帧 | Per-face 视锥剔除 |
| 无点光阴影 PCF | 点光阴影为单采样，边缘硬 | 多采样软阴影 |
| sec CB 多面执行崩溃 | ≥2 面 primary CB invalid | **✅ 已修复** (Phase 5-2) |
| 方向光阴影跳变 | 阴影跟随相机移动 | **✅ 已修复**（固定世界中心） |
| 部分视角阴影消失 | 极端视角阴影贴图覆盖不足 | CSM |

## 待实施

- Cascaded Shadow Maps (CSM)
- 面积光源（Area Light）：矩形/碟形光源、LTC 线性变换余弦近似
- IBL（Image-Based Lighting）+ 环境贴图
- Bindless 纹理数组（Texture2D[] + materialID 索引）
- 离屏视口渲染（RHI Texture → ImGui::Image）
- Gizmo 操作 + 鼠标拾取选择
- 多线程视锥剔除（Phase 5-3）**✅**
- 多线程命令缓冲录制（Phase 5-4）**✅**
- 方向光阴影跳变修复：固定世界中心 + 动态覆盖尺寸 **✅**
- sec CB 多面执行崩溃修复（Phase 5-2 续）**✅**
- GPU Driven 渲染（视锥剔除 / Hi-Z / ExecuteIndirect）
- Prefab 系统 + Asset Registry + NameComponent
- 骨骼动画 + 粒子系统
- Nanite / VSM / Virtual Texturing
- Ray Tracing + GI
