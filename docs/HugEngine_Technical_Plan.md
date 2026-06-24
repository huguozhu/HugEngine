# HugEngine — 现代实时渲染引擎技术全景与实施计划

> **目标定位**: 对标 UE5.5+ 及 2026 年最新渲染研究，覆盖从底层 RHI 到神经网络渲染的完整技术栈。
>
> **总计特性**: 340+ 项（原设计 294 项 + v2 新增 20 项 + v3 组件架构 26 项）
>
> **最后更新**: 2026-06-24

---

## 目录

1. [项目概要](#1-项目概要)
2. [架构全景图](#2-架构全景图)
3. [技术清单](#3-技术清单)
   - [0. 架构基础](#0-架构基础)
   - [1. GPU Driven 管线](#1-gpu-driven-管线)
   - [2. 场景管理与组件化渲染架构 🆕](#2-场景管理与组件化渲染架构-)
   - [3. 虚拟几何系统](#3-虚拟几何系统)
   - [4. 虚拟阴影](#4-虚拟阴影)
   - [5. 虚拟纹理](#5-虚拟纹理)
   - [6. 全局光照](#6-全局光照)
   - [7. AO 与屏幕空间效果](#7-ao-与屏幕空间效果)
   - [8. 光线追踪](#8-光线追踪)
   - [9. Mesh Shader](#9-mesh-shader)
   - [10. 神经网络渲染](#10-神经网络渲染)
   - [11. 光照与着色](#11-光照与着色)
   - [12. 大气与体积渲染](#12-大气与体积渲染)
   - [13. 后处理管线](#13-后处理管线)
   - [14. 抗锯齿与超分](#14-抗锯齿与超分)
   - [15. 渲染管线](#15-渲染管线)
   - [16. 降噪](#16-降噪)
   - [17. 反射与折射](#17-反射与折射)
   - [18. 动画系统](#18-动画系统)
   - [19. 性能优化](#19-性能优化)
   - [20. glTF v2 资源](#20-gltf-v2-资源)
   - [21. 3D Gaussian Splatting 🆕](#21-3d-gaussian-splatting-)
   - [22. 实时焦散 🆕](#22-实时焦散-)
   - [23. 工具与工作流](#23-工具与工作流)
   - [24. 编辑器](#24-编辑器)
   - [25. C++26 反射系统](#25-c26-反射系统)
4. [分阶段实施计划](#4-分阶段实施计划)
5. [关键技术选型参考](#5-关键技术选型参考)
6. [🆕 新增技术对照表](#6-新增技术对照表)

---

## 1. 项目概要

实现一个对标 UE5 的现代实时渲染引擎，覆盖最新的渲染技术栈。

| 维度 | 选型 |
|------|------|
| 构建系统 | CMake + vcpkg |
| 数学库 | GLM + 自研扩展 |
| 着色器语言 | Slang (glslang + dxc + metal 后端) |
| RHI 后端 | Vulkan 1.3+, D3D12 SM 6.10+, Metal 3, WebGPU 🆕 |
| 起步策略 | 核心架构优先（RHI + Slang 编译 + RenderGraph + 基础管线） |

---

## 2. 架构全景图

```
┌─────────────────────────────────────────────────────────────┐
│                        编辑器层                               │
│  World Editor │ Material Editor │ Visual Script │ Terrain    │
├─────────────────────────────────────────────────────────────┤
│                      C++26 反射系统                           │
│  ^T 反射 │ 属性系统 │ 序列化 │ 类型注册表 │ 脚本绑定          │
├─────────────────────────────────────────────────────────────┤
│                      渲染特性层                               │
│  GI · RT · VSM · VT · Nanite · Neural · Caustics · 3DGS     │
├─────────────────────────────────────────────────────────────┤
│                       Render Graph                           │
│  Barrier 自动推导 · 资源别名 · Pass 重排 · Async Compute     │
├─────────────────────────────────────────────────────────────┤
│                      RHI 抽象层                               │
│  Vulkan 1.3+ │ D3D12 SM 6.10+ │ Metal 3 │ WebGPU 🆕         │
├─────────────────────────────────────────────────────────────┤
│                   平台层 (GLFW/SDL3)                          │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 技术清单

> 图例: 🔴 = 核心 (Phase 1-2) · 🟡 = 重要 (Phase 3-4) · 🟢 = 进阶 (Phase 5-6) · 🆕 = 本次新增

---

### 0. 架构基础

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 0.1 | 多线程渲染 | 并行 Command List 录制 + Async Compute | 🔴 核心 |
| 0.2 | RHI 抽象层 | Vulkan / D3D12 / Metal / WebGPU 🆕 统一接口 | 🔴 核心 |
| 0.3 | Render Graph | 基于图的帧资源管理，自动 Barrier 插入、资源别名、Pass 重排 | 🔴 核心 |
| 0.4 | PSO Caching | Pipeline State Object 预编译与缓存 | 🔴 核心 |
| 0.5 | Pipeline Library | 管线库缓存，加速 PSO 创建 | 🔴 核心 |
| 0.6 | Bindless Resources | Descriptor Indexing，无绑定资源模型 | 🟡 重要 |
| 0.7 | Parallel Command List | 多线程并行录制 Command List | 🔴 核心 |
| 0.8 | Shader Hot Reloading | 着色器热重载，迭代加速 | 🟡 重要 |
| 0.9 | Async Compute Queue | 独立异步计算队列（剔除、后处理、GI） | 🟡 重要 |
| 0.10 | Resource Transitions | D3D12 Enhanced Barriers / Vulkan Layout Transitions 自动管理 | 🔴 核心 |
| 0.11 | GPU Profiling Markers | PIX / RenderDoc / Nsight 集成标记 | 🟡 重要 |
| 0.12 | Slang 着色器编译管线 | Slang → SPIR-V / DXIL / MSL 多后端编译 | 🔴 核心 |
| 0.13 | **Advanced Shader Delivery** 🆕 | 预编译着色器分发，消除 shader compilation stutter | 🟡 重要 |
| 0.14 | **D3D12 Enhanced Barriers v2** 🆕 | 新的增强 Barrier 模型（SM 6.10 更新） | 🟡 重要 |

---

### 1. GPU Driven 管线

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 1.1 | GPU Frustum Culling | Compute Shader 视锥剔除 | 🔴 核心 |
| 1.2 | GPU Occlusion Culling | Hi-Z 层次化遮挡剔除 | 🔴 核心 |
| 1.3 | Two-Phase Occlusion Culling | 上一帧遮挡结果复用，两阶段剔除 | 🟡 重要 |
| 1.4 | ExecuteIndirect | Multi-Draw Indirect，GPU 端批处理 | 🔴 核心 |
| 1.5 | GPU Scene Data Upload | GPU 端场景数据上传和变换 | 🔴 核心 |
| 1.6 | GPU Work Graphs | D3D12 Work Graphs / VK_AMDX_shader_enqueue，GPU 自主生成工作 | 🟡 重要 |
| 1.7 | Persistent Thread Group | 持久化线程组，持续处理剔除任务 | 🟡 重要 |
| 1.8 | **VK_EXT_device_generated_commands** 🆕 | Vulkan 标准化 GPU 驱动命令（AMD/NV/Intel 全覆盖） | 🟡 重要 |
| 1.9 | **Mesh Nodes (Work Graph)** 🆕 | GPU Work Graph 直接触发 Mesh Shader 渲染 | 🟢 进阶 |
| 1.10 | **Wave Matrix Operations** 🆕 | SM 6.9+ Wave 级矩阵运算，用于 shader 内 ML 推理 | 🟢 进阶 |

---

### 2. 场景管理与组件化渲染架构 🆕

> HugEngine 采用 **Actor-Component** 架构作为场景组织与渲染调度的核心范式：
> - **Entity（实体）**: 场景中的原子对象，自身不承载逻辑，仅作为 Component 容器和空间锚点
> - **Component（组件）**: 可复用的功能单元，挂载到 Entity 上提供渲染、物理、音频等行为
> - **System（系统）**: 按 Component 类型遍历并执行逻辑的调度器（RenderSystem、AnimationSystem 等）
>
> 与 UE5 的 Actor-Component 模型对齐，但通过 C++26 静态反射实现零宏、零代码生成的组件注册。

#### 2A. 核心架构

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 2A.1 | Entity 模型 | 轻量实体：UUID + Name + Component 列表 + Transform 引用，无虚函数 | 🔴 核心 |
| 2A.2 | Component 基类 | `[[engine::component]]` 属性标记，编译期生成类型 ID、工厂函数、属性反射 | 🔴 核心 |
| 2A.3 | Component 生命周期 | `OnCreate()` → `OnStart()` → `OnUpdate(tick)` → `OnDestroy()`，纯虚接口 | 🔴 核心 |
| 2A.4 | **Entity-Component 查询** 🆕 | `World::Query<MeshComponent, Transform>()` 按需遍历，借助反射自动建立加速索引 | 🔴 核心 |
| 2A.5 | Component 依赖声明 | `[[engine::require<Transform>]]` / `[[engine::optional<Light>]]`，编译期解析依赖图 | 🟡 重要 |
| 2A.6 | Component 执行排序 | `[[engine::update_order(100)]]` 控制 System 遍历顺序（Transform → Animation → Render） | 🟡 重要 |
| 2A.7 | 运行时 Component 增删 | Entity 运行时动态 AddComponent / RemoveComponent，热重载兼容 | 🟡 重要 |
| 2A.8 | 多线程 Component 更新 | Job System 并行遍历独立 Entity，Read-Write 依赖检测避免 data race | 🟡 重要 |
| 2A.9 | **Prefab 系统** 🆕 | Entity 模板，支持 Component 覆盖、嵌套 Prefab、继承变体 | 🟡 重要 |
| 2A.10 | **Scene Graph** 🆕 | 树形空间层级：父 Entity Transform 级联传播，`Transform::GetWorldMatrix()` 延迟计算 | 🔴 核心 |

#### 2B. 渲染组件族

> 每种渲染图元以 Component 形式挂载到 Entity，RenderSystem 按类型收集并提交 RenderGraph Pass。

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 2B.1 | **TransformComponent** 🆕 | 位置/旋转/缩放，支持局部→世界矩阵级联，Dirty Flag 传播，空间哈希加速查询 | 🔴 核心 |
| 2B.2 | **StaticMeshComponent** 🆕 | 引用 MeshAsset + MaterialAsset，GPU Scene Upload 时自动 Batch 到对应 Pass | 🔴 核心 |
| 2B.3 | **SkeletalMeshComponent** 🆕 | 骨骼动画网格，持有 AnimationInstance 引用，GPU Skinning 输出 | 🟡 重要 |
| 2B.4 | **LightComponent** 🆕 | 统一光照基类 → PointLight / SpotLight / DirectionalLight / AreaLight / IESLight 子类 | 🔴 核心 |
| 2B.5 | **CameraComponent** 🆕 | 视口参数（FOV/Near/Far/Projection），多 Camera 切换，后处理 Volume 绑定 | 🔴 核心 |
| 2B.6 | **ReflectionProbeComponent** 🆕 | Cubemap / Parallax Corrected 探针放置，自动捕获或 Baking | 🟡 重要 |
| 2B.7 | **VolumeComponent** 🆕 | 后处理 Volume (Color Grading/Bloom/DOF), 触发区域 (Level Streaming Trigger) | 🟡 重要 |
| 2B.8 | **DecalComponent** 🆕 | DBuffer / Screen Space Decal，投影材质到场景几何 | 🟡 重要 |
| 2B.9 | **ParticleSystemComponent** 🆕 | GPU 粒子发射器，Compute Shader 模拟 + 光栅化输出 | 🟢 进阶 |
| 2B.10 | **3DGaussianSplatComponent** 🆕 | 3DGS/4DGS 高斯泼溅渲染组件（对应 Section 21） | 🟢 进阶 |

#### 2C. 场景空间管理

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 2C.1 | 八叉树场景划分 | 空间分割，加速查询与剔除 | 🔴 核心 |
| 2C.2 | BVH | 包围体层次结构，RT 查询 | 🔴 核心 |
| 2C.3 | TLAS/BLAS 管理 | 光追加速结构动态更新 | 🟡 重要 |
| 2C.4 | 场景流式加载 | World Partition 风格的大世界加载 | 🟡 重要 |
| 2C.5 | LOD 系统 | 离散/连续 LOD 管理（HLOD + Meshlet LOD） | 🟡 重要 |
| 2C.6 | **Spatial Hash Grid** 🆕 | 空间哈希网格加速邻近查询（替代八叉树的轻量方案），配合 GPU Driven 剔除 | 🟢 进阶 |

#### 2D. 序列化与网络

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 2D.1 | Component 序列化 | 基于 C++26 反射的编译期序列化展开（Binary + JSON），零手写 | 🔴 核心 |
| 2D.2 | Entity 序列化 | 递归序列化所有 Component，World → File 双向 | 🔴 核心 |
| 2D.3 | Prefab Diff 序列化 | 仅序列化与 Prefab 默认值的差异属性（Override），压缩存储 | 🟡 重要 |
| 2D.4 | **Network Replication** 🆕 | `[[engine::replicated]]` 属性自动网络同步，支持 RPC 调用 | 🟢 进阶 |
| 2D.5 | **Level Streaming** 🆕 | Component 标记 `[[engine::streaming_source]]`，异步加载/卸载关卡 Entity 集合 | 🟡 重要 |

---

### 3. 虚拟几何系统（对标 Nanite）

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 3.1 | Virtualized Geometry | 虚拟微多边形几何，支持数十亿面片 | 🟡 重要 |
| 3.2 | Meshlet Pipeline | 基于 Mesh Shader 的两级剔除管线 | 🔴 核心 |
| 3.3 | Cluster-based Rendering | GPU 端 Cluster 生成、LOD 选择和渲染 | 🟡 重要 |
| 3.4 | Software VRS | 软件光栅化 + Variable Rate Shading 混合 | 🟢 进阶 |
| 3.5 | Visibility Buffer | 只存几何 ID + 重心坐标，材质延迟查询 | 🟡 重要 |
| 3.6 | RTX Mega Geometry | 分区 TLAS + Wide BVH，巨量几何 RT 加速 | 🟡 重要 |
| 3.7 | **Displacement Micromaps (DMM)** 🆕 | RT 管线中硬件加速亚三角形位移，不增加 BVH 构建成本 | 🟢 进阶 |

---

### 4. 虚拟阴影

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 4.1 | Virtual Shadow Maps (VSM) | 虚拟阴影贴图，按需分配 Shadow Page | 🟡 重要 |
| 4.2 | Ray Traced Shadows | 硬件光追精确阴影 | 🟡 重要 |
| 4.3 | Contact Shadows | 屏幕空间接触阴影 | 🟡 重要 |
| 4.4 | Shadow Map Caching | 静态物体阴影缓存 | 🟢 进阶 |
| 4.5 | CSM (Cascaded Shadow Maps) | 级联阴影（远距离/地形用） | 🔴 核心 |
| 4.6 | PCF / PCSS 软阴影 | 百分比渐近软阴影 | 🔴 核心 |
| 4.7 | **SMRT Soft Shadows** 🆕 | 基于 RTX Kit 的物理精确半影算法 | 🟢 进阶 |

---

### 5. 虚拟纹理

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 5.1 | Sparse Virtual Texturing (SVT) | D3D12 Tiled Resources / Vulkan Sparse | 🟡 重要 |
| 5.2 | Sampler Feedback | 采样器反馈，精确追踪纹理页使用情况 | 🟡 重要 |
| 5.3 | Streaming Texture Pool | GPU 端纹理池 + 流式加载 | 🟡 重要 |
| 5.4 | Feedback-based Page Table | 基于反馈的页表更新 | 🟢 进阶 |
| 5.5 | **RTX Texture Streaming (RTXTS)** 🆕 | NVIDIA Tile-based 纹理流式加载 SDK | 🟢 进阶 |
| 5.6 | **RTX Texture Filtering (RTXTF)** 🆕 | 随机采样纹理后过滤，减少体积/半透明伪影 | 🟢 进阶 |

---

### 6. 全局光照（GI）

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 6.1 | IBL (Image Based Lighting) | 基于图像的环境光照 | 🔴 核心 |
| 6.2 | RSM (Reflective Shadow Maps) | 反射阴影贴图，手电筒式 GI | 🟢 进阶 |
| 6.3 | LPV (Light Propagation Volumes) | 光传播体积 | 🟢 进阶 |
| 6.4 | VXGI (Voxel Global Illumination) | 体素全局光照 | 🟡 重要 |
| 6.5 | SDFGI (Signed Distance Field GI) | 有向距离场 GI | 🟡 重要 |
| 6.6 | Lumen (Surface Cache + Radiance Cache) | UE5 实时 GI 方案 | 🟡 重要 |
| 6.7 | DDGI (Dynamic Diffuse GI) | RTXGI 动态漫反射 GI | 🟡 重要 |
| 6.8 | SH Probe Grid | 球谐探针网格（配合 DDGI） | 🟡 重要 |
| 6.9 | Light Probe Based GI | 光照探针系统（漫反射+镜面） | 🟡 重要 |
| 6.10 | RTXGI (NVIDIA RTX Global Illumination) | NVIDIA RTX 全局光照 SDK | 🟡 重要 |
| 6.11 | SHaRC (Spatial Hash Radiance Cache) | 空间哈希辐射缓存 | 🟡 重要 |
| 6.12 | NDGI (Neural Dynamic GI) 🆕 | 神经动态 GI（基于 Neural Radiance Cache 推广） | 🟢 进阶 |
| 6.13 | PRT (Precomputed Radiance Transfer) | 预计算辐射传输 | 🟢 进阶 |

---

### 7. AO 与屏幕空间效果

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 7.1 | SSAO | 屏幕空间环境光遮蔽 | 🔴 核心 |
| 7.2 | GTAO | Ground Truth 环境光遮蔽 | 🟡 重要 |
| 7.3 | RTAO | 光线追踪环境光遮蔽 | 🟡 重要 |
| 7.4 | SSGI | 屏幕空间全局光照 | 🟡 重要 |
| 7.5 | GTGI | Ground Truth 全局光照（屏幕空间） | 🟢 进阶 |
| 7.6 | SSR | 屏幕空间反射 | 🔴 核心 |
| 7.7 | HBAO+ | NVIDIA 高精度 AO | 🟡 重要 |
| 7.8 | SSDO | 屏幕空间方向遮蔽 | 🟢 进阶 |

---

### 8. 光线追踪

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 8.1 | Ray Traced Reflections | 光追反射 | 🟡 重要 |
| 8.2 | Ray Traced Shadows | 光追阴影 (硬/软) | 🟡 重要 |
| 8.3 | Ray Traced GI | 光追全局光照 | 🟡 重要 |
| 8.4 | Ray Traced AO | 光追环境光遮蔽 | 🟡 重要 |
| 8.5 | Ray Traced Translucency | 光追半透明 | 🟢 进阶 |
| 8.6 | Path Tracing (Reference) | 路径追踪参考模式 | 🟢 进阶 |
| 8.7 | ReSTIR DI | 时空重采样直接光照 | 🟡 重要 |
| 8.8 | ReSTIR GI | 时空重采样全局光照 | 🟡 重要 |
| 8.9 | **ReSTIR PT (Path Tracing)** 🆕 | 时空重采样路径追踪（GRIS + 互惠邻居选择） | 🟡 重要 |
| 8.10 | **Reservoir Splatting** 🆕 | 前向投影时空复用（SIGGRAPH 2025），运动场景 30-50% 更高复用率 | 🟢 进阶 |
| 8.11 | **Shader Execution Reordering (SER)** 🆕 | SM 6.9 RT 着色器执行重排，RT 性能 2-3× 提升 | 🟡 重要 |
| 8.12 | **Opacity Micromaps (OMM)** 🆕 | SM 6.9 透明几何微图编码，大幅减少 RT 无效着色 | 🟡 重要 |

---

### 9. Mesh Shader

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 9.1 | Meshlet Generation Pipeline | 预计算 Meshlet 划分 | 🔴 核心 |
| 9.2 | GPU Primitive Assembly | Mesh Shader 图元装配 | 🔴 核心 |
| 9.3 | Amplification Shader Culling | AS 级剔除 + LOD 选择 | 🔴 核心 |
| 9.4 | Meshlet Cone Culling | 背面锥体剔除 | 🟡 重要 |

---

### 10. 神经网络渲染

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 10.1 | **DLSS Super Resolution 4.5+** 🆕 | NVIDIA Transformer 超分辨率 (FP8 + 全局自注意力) | 🟡 重要 |
| 10.2 | **FSR 4.1** 🆕 | AMD ML-based 超分辨率 | 🟡 重要 |
| 10.3 | XeSS | Intel 超分辨率 | 🟡 重要 |
| 10.4 | **PSSR 2.0** 🆕 | PlayStation Spectral Super Resolution (Sony+AMD Project Amethyst) | 🟢 进阶 |
| 10.5 | **DLSS Multi Frame Generation 6×** 🆕 | 每渲染帧生成 5 AI 帧，动态 MFG 适配 GPU 负载 | 🟡 重要 |
| 10.6 | **Ray Reconstruction (DLSS 3.5/RX Transformer)** | Transformer 自注意力光线重建去噪，效率 2.8× | 🟡 重要 |
| 10.7 | Neural Radiance Cache (NRC) | NVIDIA 神经网络辐射缓存 | 🟡 重要 |
| 10.8 | **RTX Neural Shaders (RTXNS)** 🆕 | Slang + Cooperative Vectors 着色器内神经网络推理框架 | 🟡 重要 |
| 10.9 | **Cooperative Vectors / DirectX LinAlg** 🆕 | SM 6.9 → SM 6.10 LinAlg (`linalg::Matrix`)，统一向量-矩阵 ML 加速 | 🟡 重要 |
| 10.10 | Neural Materials + NTC | 神经网络材质 + 纹理压缩（8× VRAM 节省） | 🟡 重要 |
| 10.11 | **RTX Neural Faces** | NVIDIA 神经网络面部生成 | 🟢 进阶 |
| 10.12 | Neural BRDF | 用神经网络拟合复杂 BRDF | 🟢 进阶 |
| 10.13 | **RTX Character Rendering (RTXCR)** 🆕 | Linear Swept Spheres 毛发 (Blackwell 加速 2×)，SSS 皮肤 | 🟢 进阶 |
| 10.14 | **DirectX Compute Graph Compiler** 🆕 | 全模型图编译器，GPU 原生 ML 图执行（超分/降噪/LLM） | 🟢 进阶 |
| 10.15 | **SpatioTemporal Blue Noise (STBN)** 🆕 | 预生成时空蓝噪声纹理，提升蒙特卡洛采样质量 | 🟡 重要 |

---

### 11. 光照与着色

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 11.1 | Clustered Shading | Cluster-based 光照剔除 (优于 Tiled) | 🔴 核心 |
| 11.2 | Clustered Deferred | Clustered + Deferred 结合 | 🔴 核心 |
| 11.3 | Clustered Forward | Clustered + Forward 结合 | 🔴 核心 |
| 11.4 | Area Lights | 面光源（矩形、圆盘、球体） | 🟡 重要 |
| 11.5 | IES Light Profiles | IES 光域网 | 🟡 重要 |
| 11.6 | Subsurface Scattering (SSS) | 次表面散射（屏幕空间 + Burley） | 🟡 重要 |
| 11.7 | Hair / Fur Shading | Marschner 模型 + 各向异性高光 | 🟢 进阶 |
| 11.8 | Thin-film Interference | 薄膜干涉效果 | 🟢 进阶 |
| 11.9 | Clear Coat | 清漆材质 (KHR_materials_clearcoat) | 🟡 重要 |
| 11.10 | Sheen | 织物光泽 (KHR_materials_sheen) | 🟡 重要 |
| 11.11 | Anisotropy | 各向异性高光 (KHR_materials_anisotropy) | 🟡 重要 |
| 11.12 | Iridescence | 彩虹色 (KHR_materials_iridescence) | 🟢 进阶 |
| 11.13 | Transmission | 薄壁透射 (KHR_materials_transmission) | 🟡 重要 |
| 11.14 | Volume / Thickness | 体积厚度 (KHR_materials_volume) | 🟢 进阶 |
| 11.15 | Specular / IOR | 光泽度/IOR (KHR_materials_specular, KHR_materials_ior) | 🟡 重要 |
| 11.16 | Dispersion | 色散 (KHR_materials_dispersion) | 🟢 进阶 |
| 11.17 | Emissive Strength | 强自发光 | 🟡 重要 |
| 11.18 | Physically Based Sky | Bruneton 大气散射天空 | 🟡 重要 |
| 11.19 | **Spectral Rendering** 🆕 | 基于波长的渲染（替代 RGB），用于色散/焦散/薄膜 | 🟢 进阶 |
| 11.20 | **Neural Appearance Models** 🆕 | 神经辐照度体积 (NIV)，1-5MB 替代传统探针网格，1ms 推理 | 🟢 进阶 |

---

### 12. 大气与体积渲染

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 12.1 | Atmospheric Scattering | Rayleigh / Mie 大气散射 | 🟡 重要 |
| 12.2 | Volumetric Clouds | 体积云 | 🟢 进阶 |
| 12.3 | Volumetric Fog | 体积雾 + 散射光照 | 🟡 重要 |
| 12.4 | Volumetric Light Shafts | 体积光柱 (God Rays) | 🟡 重要 |
| 12.5 | Sky Atmosphere | Bruneton / Hosek-Wilkie 天空模型 | 🟡 重要 |
| 12.6 | Aerial Perspective | 大气透视（远距离大气散射） | 🟡 重要 |
| 12.7 | Cloud Shadow | 云层阴影系统 | 🟢 进阶 |
| 12.8 | **Volumetric Caustics** 🆕 | 水下体积焦散（Markov Chain 路径引导，30+ FPS） | 🟢 进阶 |

---

### 13. 后处理管线

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 13.1 | Bloom | 泛光效果 | 🔴 核心 |
| 13.2 | Depth of Field (DOF) | 景深 (Circle of Confusion) | 🟡 重要 |
| 13.3 | Motion Blur | 运动模糊（基于速度缓冲） | 🔴 核心 |
| 13.4 | Auto Exposure (Eye Adaptation) | 自动曝光 / 人眼亮度适应 | 🔴 核心 |
| 13.5 | Color Grading | 颜色分级 (LUT + ACES Tone Mapping) | 🔴 核心 |
| 13.6 | Lens Flare | 镜头光晕 | 🟢 进阶 |
| 13.7 | Chromatic Aberration | 色差 | 🟡 重要 |
| 13.8 | Film Grain | 胶片颗粒 | 🟡 重要 |
| 13.9 | Vignette | 暗角 | 🟡 重要 |
| 13.10 | HDR Display Output | HDR10 / scRGB 输出 | 🟡 重要 |
| 13.11 | Tone Mapping | ACES / Filmic / Reinhard / PBR Neutral | 🔴 核心 |
| 13.12 | Temporal Upsampling | 时域超采样（独立于 DLSS/FSR） | 🟡 重要 |

---

### 14. 抗锯齿与超分

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 14.1 | MSAA | 前向渲染多重采样 | 🔴 核心 |
| 14.2 | TAA | 时域抗锯齿 | 🔴 核心 |
| 14.3 | TAAU | 时域抗锯齿 + 超采样 | 🟡 重要 |
| 14.4 | SMAA | 子像素形态学抗锯齿 | 🟢 进阶 |
| 14.5 | FXAA | 快速近似抗锯齿 | 🟢 进阶 |
| 14.6 | DLAA | NVIDIA 深度学习抗锯齿 | 🟢 进阶 |
| 14.7 | CMAA | 保守形态学抗锯齿 | 🟢 进阶 |
| 14.8 | **Streamline SDK** 🆕 | NVIDIA 开源跨厂商超分/AA 集成框架 (DLSS/FSR/XeSS) | 🟡 重要 |

---

### 15. 渲染管线

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 15.1 | Forward Rendering (HDR) | 前向渲染管线 | 🔴 核心 |
| 15.2 | Deferred Rendering (HDR) | 延迟渲染管线 | 🔴 核心 |
| 15.3 | Forward+ (Tiled Forward) | Tiled 光照前向 | 🔴 核心 |
| 15.4 | Visibility Buffer Rendering | 可视性缓冲渲染 | 🟡 重要 |
| 15.5 | OIT (Order Independent Transparency) | 顺序无关透明 (Weighted Blended OIT) | 🟡 重要 |
| 15.6 | Multi-View / Stereo Rendering | VR 立体渲染 | 🟢 进阶 |
| 15.7 | VRS Tier 1 / Tier 2 | 可变速率着色 | 🟡 重要 |
| 15.8 | **Full Path Tracing Pipeline** 🆕 | 完整路径追踪渲染管线（Raster + PT 混合 → 全 PT） | 🟢 进阶 |

---

### 16. 降噪

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 16.1 | Spatial-Temporal Denoising | 时空调制降噪器 | 🟡 重要 |
| 16.2 | NRD (NVIDIA Real-time Denoisers) | NVIDIA 降噪库 (Shadow, Reflection, AO, GI) | 🟡 重要 |
| 16.3 | SVGF | 空时方差引导滤波 | 🟢 进阶 |
| 16.4 | A-SVGF | 自适应 SVGF | 🟢 进阶 |
| 16.5 | ReBLUR | 模糊重投影降噪 | 🟡 重要 |
| 16.6 | ReLAX | 松弛重投影降噪 | 🟡 重要 |
| 16.7 | **SIGMA** 🆕 | NRD 阴影降噪器（RTX Kit 组件） | 🟡 重要 |

---

### 17. 反射与折射

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 17.1 | SSR | 屏幕空间反射 | 🔴 核心 |
| 17.2 | Planar Reflections | 平面反射（水面、镜面） | 🟡 重要 |
| 17.3 | Ray Traced Reflections | 光追反射 | 🟡 重要 |
| 17.4 | Reflection Probes | 反射探针系统 (Cubemap / Parallax Corrected) | 🔴 核心 |
| 17.5 | Specular IBL | 镜面反射 IBL (Split-Sum Approximation) | 🔴 核心 |
| 17.6 | Screen Space Refraction | 屏幕空间折射 | 🟢 进阶 |
| 17.7 | **Newton's Method Refraction** 🆕 | 牛顿法屏幕空间折射/焦散（3-5 迭代收敛替代 Ray Marching） | 🟢 进阶 |

---

### 18. 动画系统

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 18.1 | Skeletal Animation | 骨骼动画 + GPU Skinning | 🟡 重要 |
| 18.2 | Blend Shapes / Morph Targets | 混合变形（表情） | 🟡 重要 |
| 18.3 | Vertex Animation Texture (VAT) | 顶点动画贴图 | 🟢 进阶 |
| 18.4 | Procedural Animation | GPU 程序化动画 | 🟢 进阶 |
| 18.5 | **GPU Procedural Generation** 🆕 | Work Graphs + Mesh Nodes 驱动 GPU 程序化几何（52KB→34GB） | 🟢 进阶 |

---

### 19. 性能优化

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 19.1 | Hi-Z Occlusion Culling | 层次化 Z 遮挡剔除 | 🔴 核心 |
| 19.2 | Variable Rate Shading (VRS) | 可变速率着色 | 🟡 重要 |
| 19.3 | LOD 系统 | 离散 + 连续 LOD | 🟡 重要 |
| 19.4 | Impostor System | 远景 Impostor 替代完整几何 | 🟢 进阶 |
| 19.5 | Draw Call Merging | GPU Instance + Indirect 合批 | 🔴 核心 |
| 19.6 | Resource Streaming | 几何/纹理异步流式加载 | 🟡 重要 |
| 19.7 | D3D12 Enhanced Barriers | 增强 Barrier 模型 | 🟡 重要 |
| 19.8 | **RTX Memory Utility (RTXMU)** 🆕 | 加速结构内存压缩与子分配 | 🟢 进阶 |
| 19.9 | **Variable Group Shared Memory** 🆕 | SM 6.10 突破 32KB 共享内存限制 | 🟢 进阶 |
| 19.10 | **Reflex 2 Frame Warp** 🆕 | 显示前基于最新输入更新帧，大幅降低延迟 | 🟡 重要 |

---

### 20. glTF v2 资源

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 20.1 | glTF 2.0 Binary (GLB) | GLB 格式加载 | 🔴 核心 |
| 20.2 | glTF 2.0 JSON + External | JSON + 外部资源加载 | 🔴 核心 |
| 20.3 | PBR Metallic-Roughness | 核心 PBR 材质 | 🔴 核心 |
| 20.4 | PBR Specular-Glossiness | 镜面光泽材质 (KHR) | 🔴 核心 |
| 20.5 | Clear Coat | KHR_materials_clearcoat | 🟡 重要 |
| 20.6 | Sheen | KHR_materials_sheen | 🟡 重要 |
| 20.7 | Transmission | KHR_materials_transmission | 🟡 重要 |
| 20.8 | Volume / Thickness | KHR_materials_volume | 🟢 进阶 |
| 20.9 | Anisotropy | KHR_materials_anisotropy | 🟡 重要 |
| 20.10 | Iridescence | KHR_materials_iridescence | 🟢 进阶 |
| 20.11 | Specular / IOR | KHR_materials_specular + KHR_materials_ior | 🟡 重要 |
| 20.12 | Dispersion | KHR_materials_dispersion | 🟢 进阶 |
| 20.13 | Emissive Strength | KHR_materials_emissive_strength | 🟡 重要 |
| 20.14 | Unlit | KHR_materials_unlit | 🔴 核心 |
| 20.15 | Skeletal Skinning | 蒙皮骨骼 | 🟡 重要 |
| 20.16 | Morph Targets | 混合变形 | 🟡 重要 |
| 20.17 | Draco Compression | KHR_draco_mesh_compression | 🟡 重要 |
| 20.18 | Basis Universal | KTX2 / BasisU 纹理压缩 | 🟡 重要 |
| 20.19 | Meshopt Compression | EXT_meshopt_compression | 🟡 重要 |
| 20.20 | Texture Transform | KHR_texture_transform | 🟡 重要 |
| 20.21 | KHR_lights_punctual | 点光源、聚光灯、方向光 | 🟡 重要 |
| 20.22 | Animation | 节点动画 | 🟡 重要 |
| 20.23 | Quantized Accessors | KHR_mesh_quantization | 🟢 进阶 |
| 20.24 | **KHR_gaussian_splatting** 🆕 | glTF 3DGS 扩展，标准化高斯泼溅场景交换 | 🟢 进阶 |

---

### 21. 3D Gaussian Splatting 🆕

> **全新模块**: 将 3DGS 作为一等渲染图元集成到引擎管线中

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 21.1 | 3DGS Rasterization Pipeline | 原生 GPU 高斯泼溅光栅化管线 | 🟡 重要 |
| 21.2 | Hybrid Mesh + 3DGS Compositing | 深度感知混合：传统 Mesh + 高斯泼溅合成 | 🟡 重要 |
| 21.3 | Nanite-style LOD for GS | 类 Nanite 的屏幕空间误差 LOD + Splat Compaction | 🟢 进阶 |
| 21.4 | 4DGS (Dynamic/Volumetric Video) | 动态体积视频高斯泼溅（时间维度扩展） | 🟢 进阶 |
| 21.5 | Relightable 3DGS | 可重新照明的 3DGS（NVOL/PLY + SH + ACES/OCIO） | 🟢 进阶 |
| 21.6 | Deformable Beta Splatting | 频率自适应 Beta 核替代高斯核（参数 -55%，速度 1.5×） | 🟢 进阶 |
| 21.7 | VR/XR 3DGS Support | 双目立体渲染 + OpenXR 支持 | 🟢 进阶 |
| 21.8 | Compressed GS Format (.sog) | 压缩高斯泼溅格式支持 | 🟢 进阶 |

---

### 22. 实时焦散 🆕

> **全新模块**: 2025-2026 年突破使实时无偏焦散成为可能

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 22.1 | Screen-Space Caustics (Newton) | 牛顿法屏幕空间焦散（3-5 次迭代，替代 Ray Marching） | 🟡 重要 |
| 22.2 | SMS + ReSTIR Caustics | Specular Manifold Sampling + 时空重采样（SIGGRAPH Asia 2025） | 🟢 进阶 |
| 22.3 | Volumetric Caustics | 参与介质体积焦散（Markov Chain 路径引导，30+ FPS） | 🟢 进阶 |
| 22.4 | Markov Chain Path Guiding | 无偏路径引导（von Mises-Fisher 混合 + 空间哈希） | 🟢 进阶 |

---

### 23. 工具与工作流

| # | 特性 | 说明 | 优先级 |
|---|------|------|--------|
| 23.1 | Shader Hot Reload | Slang 着色器热重载 | 🔴 核心 |
| 23.2 | RenderDoc / PIX / Nsight | 调试器集成（GPU 标记） | 🟡 重要 |
| 23.3 | Asset Pipeline | 资源导入/处理管线（烘焙、压缩、Cook） | 🟡 重要 |
| 23.4 | Shader Variant Management | 着色器变体管理 + 预编译 | 🟡 重要 |
| 23.5 | Debug Views | GBuffer、深度、法线、Overdraw、Shader Complexity 等可视化 | 🟡 重要 |
| 23.6 | Content Browser | 资源浏览器 | 🟡 重要 |
| 23.7 | **Streamline SDK Integration** 🆕 | NVIDIA 跨厂商超分/AA 集成框架 v2.7.2+ | 🟡 重要 |

---

### 24. 编辑器

| # | 分类 | 特性 | 说明 | 优先级 |
|---|------|------|------|--------|
| 24.1 | 架构 | Editor/Engine 分离 | 编辑器独立模块，引擎可独立运行 | 🔴 核心 |
| 24.2 | 架构 | Viewport 渲染 | 多视口（透视/正交/Wireframe/Lit/Unlit） | 🔴 核心 |
| 24.3 | 架构 | Editor-only RHI | 编辑器 UI 资源与引擎渲染资源隔离 | 🔴 核心 |
| 24.4 | 架构 | Undo/Redo 系统 | 基于 Command 模式，覆盖 Transform/属性/删除 | 🔴 核心 |
| 24.5 | 架构 | Transaction 系统 | 批量操作原子化，嵌套事务 | 🟡 重要 |
| 24.6 | 架构 | Dirty/Autosave | 修改标记 + 自动保存 | 🟡 重要 |
| 24.7 | 架构 | Docking Panel | 可停靠、拖拽、分屏的编辑器面板布局 | 🟡 重要 |
| 24.8 | 架构 | Editor Settings | 编辑器偏好设置持久化 | 🟡 重要 |
| 24.9 | 场景 | World Outliner | 层级场景树，展示所有 Actor/Entity | 🔴 核心 |
| 24.10 | 场景 | Gizmo 系统 | 位移/旋转/缩放 Gizmo（世界/局部坐标） | 🔴 核心 |
| 24.11 | 场景 | Snapping | 网格吸附、顶点吸附、角度吸附 | 🟡 重要 |
| 24.12 | 场景 | 多选 + 批量操作 | Ctrl+点击多选，批量移动/复制/删除 | 🟡 重要 |
| 24.13 | 场景 | Copy/Paste/Duplicate | Actor 复制粘贴，支持跨关卡 | 🔴 核心 |
| 24.14 | 场景 | Align / Distribute | 对齐与分布工具 | 🟢 进阶 |
| 24.15 | 场景 | 编辑模式切换 | 选择/地形编辑/Foliage 笔刷模式 | 🟡 重要 |
| 24.16 | 资产 | Content Browser | 树形 + 缩略图/列表视图 | 🔴 核心 |
| 24.17 | 资产 | Asset Thumbnail | 资源缩略图实时渲染（材质球、模型预览） | 🟡 重要 |
| 24.18 | 资产 | Drag & Drop | 资源拖拽到场景 | 🔴 核心 |
| 24.19 | 资产 | Asset Import | 拖入 glTF/图片等自动导入 | 🔴 核心 |
| 24.20 | 资产 | Asset Dependency | 资源依赖追踪（引用图），重定向/替换 | 🟡 重要 |
| 24.21 | 资产 | Asset Picker | 属性中资源槽位弹出选择器 | 🟡 重要 |
| 24.22 | 资产 | Asset Registry | 资源注册表 + 异步扫描 + 元数据 | 🔴 核心 |
| 24.23 | 资产 | Asset Migration | 资源迁移/重命名自动更新引用 | 🟢 进阶 |
| 24.24 | 属性 | Details Panel | 属性面板，显示选中对象可编辑属性 | 🔴 核心 |
| 24.25 | 属性 | Property Binding | C++ 反射属性自动绑定 UI | 🔴 核心 |
| 24.26 | 属性 | Category/Foldout | 属性按 Category 分组折叠 | 🔴 核心 |
| 24.27 | 属性 | 类型适配 Widget | bool→复选框, enum→下拉, float3→颜色/向量编辑 | 🔴 核心 |
| 24.28 | 属性 | Color Picker | RGBA/HSV 拾色器 + 色轮 | 🟡 重要 |
| 24.29 | 属性 | Curve Editor | 曲线编辑器（Float Curve, Color Curve） | 🟡 重要 |
| 24.30 | 属性 | Multi-Edit | 多选对象公共属性批量修改 | 🟡 重要 |
| 24.31 | 属性 | Search / Filter | 属性面板搜索过滤 | 🟡 重要 |
| 24.32 | 属性 | Property Meta | Range, Clamp, Tooltip, ReadOnly, HideInEditor 等反射标记 | 🟡 重要 |
| 24.33 | 属性 | Custom Property Widget | 为特定类型注册自定义 Widget | 🟢 进阶 |
| 24.34 | 关卡 | Level / World 概念 | Level = 空间容器，World = 运行时场景聚合 | 🔴 核心 |
| 24.35 | 关卡 | Level Streaming | 运行时动态加载/卸载关卡 | 🟡 重要 |
| 24.36 | 关卡 | Sub-Level | 子关卡叠加，World Composition | 🟡 重要 |
| 24.37 | 关卡 | Level Blueprint Actor | 关卡级别配置 Actor | 🟢 进阶 |
| 24.38 | 材质 | Node-based Material Editor | 节点材质编辑器（类似 UE / Blender Shader Nodes） | 🟡 重要 |
| 24.39 | 材质 | Material Compilation | 材质图 → Slang 代码生成 → 编译 | 🟡 重要 |
| 24.40 | 材质 | Material Preview | 实时预览材质球 | 🟡 重要 |
| 24.41 | 材质 | Material Instance | 材质实例（参数覆盖，不重编译） | 🟡 重要 |
| 24.42 | 材质 | Material Function | 可复用的材质函数节点 | 🟢 进阶 |
| 24.43 | 脚本 | Visual Scripting | 通用可视化脚本（Blueprint 式游戏逻辑/编辑器自动化） | 🟢 进阶 |
| 24.44 | 预览 | Play In Editor (PIE) | 编辑器内运行游戏（模拟/独立进程） | 🟡 重要 |
| 24.45 | 预览 | Console | 内置控制台，CVar 实时调参 | 🔴 核心 |
| 24.46 | 预览 | Stats / Profiler | 帧率、Draw Call、GPU 时间显示 | 🟡 重要 |
| 24.47 | 预览 | Log Panel | 日志输出面板 | 🟡 重要 |
| 24.48 | 预览 | GPU Capture | 一键触发 RenderDoc/PIX 截帧 | 🟡 重要 |
| 24.49 | 预览 | Visualization Modes | Lit/Unlit/Wireframe/Overdraw/Shader Complexity | 🟡 重要 |
| 24.50 | 地形 | Heightmap Terrain | 高度图地形系统 | 🟢 进阶 |
| 24.51 | 地形 | Terrain Sculpting | 地形雕刻（升高/降低/平滑/侵蚀笔刷） | 🟢 进阶 |
| 24.52 | 地形 | Terrain Material Layers | 地形多层材质（自动混合权重） | 🟢 进阶 |
| 24.53 | 地形 | Foliage System | GPU 植被实例化 + 笔刷布景 | 🟢 进阶 |

---

### 25. C++26 反射系统

基于 C++26 标准静态反射（P2996 / P3294），零宏、零外部代码生成器。

| # | 分类 | 特性 | 说明 | 优先级 |
|---|------|------|------|--------|
| 25.1 | 核心 | 类型注册 | `^T` 获取 `meta::info`，编译期自动生成类型 ID | 🔴 核心 |
| 25.2 | 核心 | 属性反射 | `meta::members_of(^T)` 枚举成员，获取类型/名称/偏移 | 🔴 核心 |
| 25.3 | 核心 | 函数反射 | `meta::members_of(^T)` 过滤 `meta::is_function`，获取参数/返回值 | 🟡 重要 |
| 25.4 | 核心 | 构造工厂 | `[:meta::type_of(^T):]` 拼接回类型 | 🔴 核心 |
| 25.5 | 核心 | 基类反射 | `meta::bases_of(^T)` 遍历继承链 | 🔴 核心 |
| 25.6 | 核心 | 枚举反射 | `meta::enumerators_of(^E)` 获取枚举值 | 🔴 核心 |
| 25.7 | 核心 | 类型 Traits | 自定义 `[[engine::component]]`/`[[engine::resource]]` 属性 | 🟡 重要 |
| 25.8 | 核心 | Placeholder / Deleted | `TReflectedPtr<T>` + `meta::is_valid` | 🟡 重要 |
| 25.9 | 元数据 | Category | `[[engine::category("...")]]` | 🔴 核心 |
| 25.10 | 元数据 | Display Name | `[[engine::display_name("...")]]` | 🟡 重要 |
| 25.11 | 元数据 | Tooltip | `[[engine::tooltip("...")]]` | 🟡 重要 |
| 25.12 | 元数据 | Range | `[[engine::range(min, max)]]` | 🟡 重要 |
| 25.13 | 元数据 | Clamp | `[[engine::clamp(min, max)]]` | 🟡 重要 |
| 25.14 | 元数据 | Slider / Step | `[[engine::slider]]` / `[[engine::step(s)]]` | 🟡 重要 |
| 25.15 | 元数据 | Read Only / Edit Condition | `[[engine::read_only]]` / `[[engine::edit_condition("prop")]]` | 🟡 重要 |
| 25.16 | 元数据 | Hide In Editor | `[[engine::hide_in_editor]]` | 🟡 重要 |
| 25.17 | 元数据 | Asset Picker | `[[engine::asset_picker(".gltf")]]` | 🟡 重要 |
| 25.18 | 元数据 | Color Widget | `[[engine::color_widget]]` | 🟢 进阶 |
| 25.19 | 元数据 | Unit | `[[engine::unit("cm")]]` | 🟢 进阶 |
| 25.20 | 元数据 | Sort Priority | `[[engine::sort_priority(N)]]` | 🟢 进阶 |
| 25.21 | 元数据 | Deprecated | `[[engine::deprecated("use X instead")]]` | 🟢 进阶 |
| 25.22 | 访问 | 属性读写 | 编译期生成类型安全 Getter/Setter | 🔴 核心 |
| 25.23 | 访问 | 运行时遍历 | `meta::expand` + 编译期循环展开 | 🔴 核心 |
| 25.24 | 访问 | 成员指针获取 | `[:member:]` 直接访问 | 🔴 核心 |
| 25.25 | 访问 | Sub-property | 递归反射（如 `float3.x`） | 🟡 重要 |
| 25.26 | 序列化 | 二进制序列化 | 编译期生成 `ar &` 展开 | 🔴 核心 |
| 25.27 | 序列化 | JSON 序列化 | `meta::name_of(member)` 自动生成 key | 🔴 核心 |
| 25.28 | 序列化 | 增量序列化 | Default 对象 + Diff 比较 | 🟡 重要 |
| 25.29 | 序列化 | 版本兼容 | `[[engine::renamed_from("old")]]` 向下兼容 | 🟡 重要 |
| 25.30 | 序列化 | Asset 引用序列化 | Path/ID 序列化，反序列化时解析 | 🔴 核心 |
| 25.31 | 序列化 | Binary Diff/Merge | `meta::members_of` 生成逐成员 diff | 🟢 进阶 |
| 25.32 | 注册表 | 类型注册表 | 编译期 `TypeDescriptor` → 运行时全局 Registry | 🔴 核心 |
| 25.33 | 注册表 | 属性描述符 | `PropertyDescriptor{name, type, offset, attributes}` | 🔴 核心 |
| 25.34 | 注册表 | 类型 Factory | `HashMap<uint64, std::function<void*()>>` | 🔴 核心 |
| 25.35 | 注册表 | Namespace 支持 | `meta::name_of` 自动包含 namespace | 🟡 重要 |
| 25.36 | 注册表 | Template 容器反射 | `TArray<T>`, `TMap<K,V>` 等 | 🟡 重要 |
| 25.37 | 注册表 | 属性提取 | `meta::attributes_of(member)` 提取 `[[engine::...]]` | 🔴 核心 |
| 25.38 | 注册表 | Editor-only 过滤 | `if constexpr` 剔除 Release 构建的属性注册 | 🟡 重要 |
| 25.39 | 集成 | Details Panel | `meta::members_of` → Widget 自动生成 | 🔴 核心 |
| 25.40 | 集成 | 序列化/反序列化 | 编译期 `serialize()` → 关卡保存/加载 | 🔴 核心 |
| 25.41 | 集成 | CVar 绑定 | `[[engine::cvar]]` → 控制台变量 | 🟡 重要 |
| 25.42 | 集成 | 脚本绑定 | `meta::members_of` → Lua/C# 暴露 | 🟢 进阶 |
| 25.43 | 集成 | ImGui Debug | `for_each_property` → 自动 Debug 面板 | 🟡 重要 |
| 25.44 | 集成 | 资源热重载 | `[[engine::depends_on_asset]]` → 自动依赖追踪 | 🟡 重要 |
| 25.45 | 集成 | 动画轨道 | 反射属性路径作为动画曲线绑定目标 | 🟢 进阶 |
| 25.46 | 集成 | Prefab Override | 序列化 Diff vs Prefab，仅保存覆盖属性 | 🟢 进阶 |
| 25.47 | 集成 | Network Replication | `[[engine::replicated]]` → 自动网络同步 | 🟢 进阶 |
| 25.48 | 集成 | Undo/Redo | `PropertyChangeCommand` 自动入栈 | 🔴 核心 |

---

## 4. 分阶段实施计划

### 总览

```
Phase 1: 核心骨架    (21 周)  ── 能跑起来的基础引擎 + Actor-Component 架构
Phase 2: GPU Driven   (14 周)  ── GPU 驱动的现代化管线
Phase 3: 高级几何     (14 周)  ── Nanite + VSM + VT
Phase 4: GI + RT      (18 周)  ── 实时光照系统完整
Phase 5: 神经渲染     (14 周)  ── AI 驱动的渲染
Phase 6: 大气+后处理  (14 周)  ── 视觉效果完整
Phase 7: 高斯+焦散    (12 周)  ── 🆕 新一代渲染图元
Phase 8: 打磨+发布    (10 周)  ── 优化+文档+示例
─────────────────────────────────────────
总计: 约 115 周 (约 2.2 年)
```

---

### 第一阶段 · 核心骨架 (21 周)

**目标**: 可运行的基础渲染引擎 + 简单编辑器（Actor-Component 架构就位）

| 周 | 任务 |
|----|------|
| 1-2 | 项目脚手架: CMake + vcpkg + GLFW/SDL3 + spdlog + 目录结构 |
| 3-4 | 数学库扩展: GLM 封装 + SIMD 优化 + 几何工具 |
| 5-8 | **RHI 抽象层**: Vulkan 1.3+ + D3D12 SM6.6+ 双后端, Device/SwapChain/Buffer/Texture 封装 |
| 9-10 | **Slang 着色器编译管线**: Slang → SPIR-V / DXIL 编译, Include 系统, 预编译缓存 |
| 11-13 | **Render Graph**: 帧资源管理, Pass 依赖图, 自动 Barrier (D3D12 Enhanced Barriers / Vulkan Layout Transitions), 资源别名 |
| 14-15 | **C++26 反射系统**: ^T + [:...:] + [[engine::]] 属性 + 类型注册表 + 序列化 |
| 16-17 | **Actor-Component 架构 🆕**: Entity/Component 基类, 生命周期, Component 查询, Scene Graph + Transform 层级, 基础渲染组件 (StaticMesh/Light/Camera) |
| 18 | **基础前向渲染**: HDR + PBR (Metallic-Roughness) + 基础 Tone Mapping (ACES) |
| 19 | **基础延迟渲染**: GBuffer (Albedo+Normal+Roughness+Metallic+Depth) + 基础光照 Pass |
| 20 | **glTF 2.0 加载器 + Component 集成 🆕**: GLB + JSON → Entity+StaticMeshComponent 自动生成, PBR 材质, 基础 Mesh/Texture 导入 |
| 21 | **基础编辑器**: Editor/Engine 分离, Viewport, World Outliner (Entity 树), Details Panel (Component 属性), Console |

**交付物**:
- 双 RHI 后端可切换渲染
- 前向/延迟双管线
- glTF 模型 → Entity+Component 自动装配
- 基础编辑器可运行，World Outliner 浏览 Entity-Component 层级
- 100% C++26 反射驱动属性编辑（零宏、零代码生成）

---

### 第二阶段 · GPU Driven (14 周)

**目标**: GPU 驱动管线，消除 CPU 瓶颈；完善组件生态

| 周 | 任务 |
|----|------|
| 1-2 | **GPU Frustum Culling**: Compute Shader 视锥剔除 + 间接绘制 |
| 3-4 | **Hi-Z Occlusion Culling**: 深度金字塔构建 + 遮挡查询 |
| 5-6 | **ExecuteIndirect + DGC**: Multi-Draw Indirect, VK_EXT_device_generated_commands |
| 7-8 | **GPU Scene Upload 🆕**: GPU 端 Transform 更新 (Component Dirty Flag → GPU Upload), Persistent Buffer, Ring Buffer |
| 9-10 | **Bindless Resources + Component 扩展 🆕**: Descriptor Indexing, 无绑定纹理/缓冲区/Sampler, 新增 Decal/Particle/ReflectionProbe Component |
| 11-12 | **Shadow Maps**: CSM + PCF 软阴影 |
| 13 | **VRS Tier 1/2**: 可变速率着色集成 |
| 14 | **IBL + Reflection Probes**: Split-Sum Approximation 镜面 IBL, Parallax Corrected Cubemap, ReflectionProbeComponent 自动捕获 |

**交付物**:
- 全 GPU Driven 管线，CPU 不碰绘制命令
- 数万 Draw Call 场景 60+ FPS
- 基础阴影和 IBL 光照

---

### 第三阶段 · 高级几何 (14 周)

**目标**: Nanite 风格虚拟几何 + 虚拟阴影/纹理

| 周 | 任务 |
|----|------|
| 1-3 | **Mesh Shader + Meshlet Pipeline**: Meshlet 预计算划分, AS 级剔除, GPU 图元装配 |
| 4-6 | **Virtualized Geometry**: Cluster 生成, 软件光栅化, LOD 选择 (类 Nanite) |
| 7-8 | **Visibility Buffer**: 几何 ID + 重心坐标, 材质延迟查询 |
| 9-10 | **Virtual Shadow Maps**: 页表管理, 按需 SMRT 软阴影分配 |
| 11-12 | **Virtual Texturing**: SVT + Sampler Feedback, 纹理页池 |
| 13 | **RTX Mega Geometry**: 分区 TLAS + Wide BVH, 植被密集场景加速 |
| 14 | **GPU Work Graphs**: D3D12 Work Graphs 初步集成 |

**交付物**:
- 数十亿面片场景渲染能力
- 全虚拟化阴影/纹理系统
- Mega Geometry 加速植被场景

---

### 第四阶段 · GI + RT (18 周)

**目标**: 完整实时光照系统

| 周 | 任务 |
|----|------|
| 1-3 | **Lumen 式 GI**: Surface Cache (Card-based), Radiance Cache, Final Gather |
| 4-5 | **DDGI + RTXGI**: 动态漫反射 GI, 探针系统 |
| 6-7 | **SH Probe Grid + SHaRC**: 球谐探针 + 空间哈希辐射缓存 |
| 8-10 | **Ray Tracing Pipeline**: RT Reflections + RT Shadows + RTAO + RT Translucency |
| 11-12 | **NRD 降噪器集成**: ReBLUR (Diffuse/Specular), SIGMA (Shadows), ReLAX (RTXDI) |
| 13-14 | **Path Tracing Reference**: 完整路径追踪参考模式 |
| 15-16 | **ReSTIR DI + GI**: 时空重采样直接光照 + 全局光照 |
| 17-18 | **ReSTIR PT + SER + OMM**: 🆕 路径追踪重采样 + 着色器执行重排 + 透明微图 |

**交付物**:
- 全动态 GI（Lumen + DDGI 双方案可选）
- 硬件 RT 全套效果
- ReSTIR 全家桶（DI/GI/PT）
- SER + OMM 性能优化

---

### 第五阶段 · 神经网络渲染 (14 周)

**目标**: AI 驱动的渲染管线

| 周 | 任务 |
|----|------|
| 1-2 | **Streamline SDK 集成**: DLSS/FSR/XeSS 统一框架 |
| 3-4 | **DLSS Super Resolution + Frame Generation**: Transformer 超分 + 多帧生成 |
| 5-6 | **Ray Reconstruction**: DLSS 3.5/RX Transformer 光线重建 |
| 7-8 | **Neural Radiance Cache**: NRC 训练与推理管线 |
| 9-10 | **Neural Materials + NTC**: 神经材质 + 纹理压缩 (Cooperative Vectors 加速) |
| 11-12 | **DirectX LinAlg + SM 6.10**: linalg::Matrix API, Wave Matrix, Variable Group Shared Memory |
| 13-14 | **RTX Neural Shaders + Character Rendering**: Slang + CV 推理, RTXCR (LSS 毛发 + SSS 皮肤) |

**交付物**:
- DLSS/FSR/XeSS 三厂商超分可用
- 神经辐射缓存加速 GI
- NTC 纹理压缩 8× VRAM 节省
- SM 6.10 DirectX LinAlg 支持

---

### 第六阶段 · 大气 + 后处理 + 动画 (14 周)

**目标**: 视觉效果完整 + 动画系统

| 周 | 任务 |
|----|------|
| 1-2 | **Atmospheric Scattering + Sky**: Rayleigh/Mie, Bruneton/Hosek-Wilkie 天空 |
| 3-4 | **Volumetric Fog + Clouds + Light Shafts**: 体积雾 + 云 + God Rays |
| 5-7 | **完整后处理栈**: Bloom, DOF, Motion Blur, Auto Exposure, Color Grading, Lens Flare, CA, Film Grain |
| 8 | **HDR Display Output**: HDR10 / scRGB |
| 9-10 | **动画系统**: GPU Skinning + Morph Targets + VAT |
| 11-12 | **Terrain + Foliage 编辑器**: 高度图地形 + 雕刻 + 植被 |
| 13-14 | **编辑器完善**: PIE, Profiler, GPU Capture, Prefab |

**交付物**:
- 完整大气/天空/体积渲染
- 电影级后处理栈
- 动画系统 + 地形系统
- 编辑器功能完整

---

### 第七阶段 · 高斯泼溅 + 焦散 🆕 (12 周)

**目标**: 新一代渲染图元 + 物理精确焦散

| 周 | 任务 |
|----|------|
| 1-2 | **3DGS 光栅化管线**: 原生 GPU 高斯泼溅排序+光栅化 |
| 3-4 | **Hybrid Mesh + 3DGS 合成**: 深度感知混合 + SplatBus 风格 IPC |
| 5-6 | **Nanite-style LOD for GS**: 屏幕空间误差 LOD, GPU Radix Sort + Compaction |
| 7-8 | **4DGS (动态体积视频)**: 时间维度扩展, 4D 高斯 → 条件切片 3D 高斯 |
| 9 | **KHR_gaussian_splatting**: glTF 扩展支持 |
| 10-11 | **Screen-Space Caustics**: 牛顿法 (JCGT 2026), 3-5 迭代收敛 |
| 12 | **SMS + ReSTIR Caustics + 体积焦散**: 无偏焦散 + 水下体积焦散 |

**交付物**:
- 3DGS 作为一等图元集成
- 4DGS 动态体积视频播放
- 实时水体焦散效果

---

### 第八阶段 · 打磨与发布 (10 周)

**目标**: 稳定、优化、文档、示例

| 周 | 任务 |
|----|------|
| 1-3 | **性能优化**: PSO 预编译, Advanced Shader Delivery, GPU Profiling, 内存优化 |
| 4-5 | **WebGPU 后端**: 🆕 RHI 扩展, 浏览器部署路径 |
| 6-7 | **文档**: API 文档, 架构文档, Shader 指南, 教程 |
| 8-9 | **示例项目**: 室内场景, 室外场景, 材质库, 效果展示 |
| 10 | **Reflex 2 Frame Warp + 发布准备** 🆕 |

**交付物**:
- 多平台 (Windows + Linux + Web)
- 完整文档
- 示例项目
- 0.1.0 版本发布

---

## 5. 关键技术选型参考

| 模块 | 推荐方案 | 备选 |
|------|----------|------|
| RHI 后端 | **Vulkan 1.3+, D3D12 SM 6.10+, Metal 3, WebGPU** 🆕 | — |
| 着色器编译 | **Slang** (glslang + dxc + metal + WebGPU 后端) | HLSL/GLSL 直接编译 |
| 资源格式 | **glTF 2.0 + KTX2/BasisU + .sog (3DGS)** 🆕 | FBX (可选) |
| 窗口系统 | **GLFW 3.4+** 或 **SDL3** | 自研 |
| 数学库 | **GLM 1.0+** + 自研 SIMD 扩展 | Eigen (仅编辑器) |
| 线程库 | **Taskflow** 或 **Fiber-based Job System** | TBB |
| 日志 | **spdlog** | fmt |
| 内存分配 | **Vulkan Memory Allocator (VMA) + D3D12MA** | jemalloc |
| 纹理压缩 | **Basis Universal** (ETC2/BC7/ASTC) + **NTC** (神经) 🆕 | — |
| 图片加载 | **stb_image + tinyexr** | OpenImageIO |
| 网格优化 | **meshoptimizer** | — |
| 调试 | **RenderDoc + PIX + Nsight** | — |
| 编辑器 UI | **Dear ImGui** (核心) + **自研 Docking 框架** | Qt (备选) |
| C++ 反射 | **C++26 静态反射** (P2996): `^T` + `[:...:]` + `[[engine::]]` | ClangTool 代码生成 (回退) |
| 序列化 | **编译期生成 Binary + JSON** | Protobuf/FlatBuffers |
| 材质编辑器 | **自研 Node Graph Editor** (基于 ImGui) | — |
| 可视化脚本 | **自研 Visual Scripting** (Blueprint 式) | Lua 绑定 |
| 超分框架 | **NVIDIA Streamline** (DLSS/FSR/XeSS 统一) 🆕 | 各 SDK 独立集成 |
| 3DGS 格式 | **PLY + .splat + .sog + KHR_gaussian_splatting** 🆕 | — |
| 神经推理 | **DirectX LinAlg (SM 6.10) + RTX Neural Shaders (Slang+CV)** 🆕 | ONNX Runtime |
| 降噪 | **NRD** (ReBLUR + SIGMA + ReLAX) | 自研 |
| GI | **Lumen 式** (默认) + **DDGI/RTXGI** (备选) | — |
| GPU Driven | **ExecuteIndirect + DGC + Work Graphs** 🆕 | 传统 Draw/Dispatch |

---

## 6. 🆕 新增技术对照表

以下是与原始设计文档 (v1) 相比，本次头脑风暴新增的全部技术点：

| # | 新增技术 | 所属模块 | 优先级 | 来源 |
|----|----------|----------|--------|------|
| 1 | Advanced Shader Delivery (ASD) | 0.架构基础 | 🟡 | GDC 2026 / Agility SDK 1.619 |
| 2 | D3D12 Enhanced Barriers v2 | 0.架构基础 | 🟡 | SM 6.10 SDK 1.720 |
| 3 | VK_EXT_device_generated_commands | 1.GPU Driven | 🟡 | Vulkan 1.3.296, 三厂商全覆盖 |
| 4 | Mesh Nodes (Work Graph) | 1.GPU Driven | 🟢 | VK_AMDX_shader_enqueue, GDC 2025 |
| 5 | Wave Matrix Operations | 1.GPU Driven | 🟢 | SM 6.9 |
| 6 | RTX Mega Geometry | 2.场景管理 | 🟡 | NVIDIA RTX Kit |
| 7 | Displacement Micromaps (DMM) | 3.虚拟几何 | 🟢 | VK_NV_displacement_micromap |
| 8 | SMRT Soft Shadows | 4.虚拟阴影 | 🟢 | NVIDIA RTX Kit |
| 9 | RTX Texture Streaming (RTXTS) | 5.虚拟纹理 | 🟢 | NVIDIA RTX Kit |
| 10 | RTX Texture Filtering (RTXTF) | 5.虚拟纹理 | 🟢 | NVIDIA RTX Kit |
| 11 | NDGI (Neural Dynamic GI) | 6.GI | 🟢 | NRC 推广 |
| 12 | ReSTIR PT (Path Tracing) | 8.光线追踪 | 🟡 | SIGGRAPH 2025, GDC 2026 |
| 13 | Reservoir Splatting | 8.光线追踪 | 🟢 | SIGGRAPH 2025 |
| 14 | Shader Execution Reordering (SER) | 8.光线追踪 | 🟡 | SM 6.9 |
| 15 | Opacity Micromaps (OMM) | 8.光线追踪 | 🟡 | SM 6.9 / DXR 1.2 |
| 17 | DLSS 4.5+ (Transformer FP8) | 10.神经渲染 | 🟡 | NVIDIA RTX 50 Series / GTC 2025 |
| 18 | FSR 4.1 (ML-based) | 10.神经渲染 | 🟡 | AMD |
| 19 | PSSR 2.0 (Project Amethyst) | 10.神经渲染 | 🟢 | Sony + AMD |
| 20 | DLSS Multi Frame Generation 6× | 10.神经渲染 | 🟡 | GTC 2026 |
| 21 | RTX Neural Shaders (RTXNS) | 10.神经渲染 | 🟡 | NVIDIA RTX Kit |
| 22 | DirectX LinAlg (SM 6.10) | 10.神经渲染 | 🟡 | GDC 2026, SDK 1.720 |
| 23 | DirectX Compute Graph Compiler | 10.神经渲染 | 🟢 | GDC 2026 |
| 24 | RTX Character Rendering (RTXCR) | 10.神经渲染 | 🟢 | NVIDIA RTX Kit |
| 25 | SpatioTemporal Blue Noise (STBN) | 10.神经渲染 | 🟡 | NVIDIA RTX Kit |
| 26 | Spectral Rendering | 11.光照着色 | 🟢 | 2025-2026 研究趋势 |
| 27 | Neural Appearance Models (NIV) | 11.光照着色 | 🟢 | Eurographics 2026 |
| 28 | Volumetric Caustics | 12.大气体积 | 🟢 | KIT 2025, ACM CGIT |
| 29 | Streamline SDK (跨厂商超分) | 14.抗锯齿 | 🟡 | NVIDIA v2.7.2+ |
| 30 | Full Path Tracing Pipeline | 15.渲染管线 | 🟢 | 行业趋势 |
| 31 | SIGMA Shadow Denoiser | 16.降噪 | 🟡 | NVIDIA NRD |
| 32 | Newton's Method Refraction | 17.反射折射 | 🟢 | JCGT 2026 |
| 33 | GPU Procedural Generation | 18.动画 | 🟢 | AMD 52KB Demo |
| 34 | RTX Memory Utility (RTXMU) | 19.性能优化 | 🟢 | NVIDIA RTX Kit |
| 35 | Variable Group Shared Memory | 19.性能优化 | 🟢 | SM 6.10 |
| 36 | Reflex 2 Frame Warp | 19.性能优化 | 🟡 | NVIDIA |
| 37 | KHR_gaussian_splatting | 20.glTF | 🟢 | glTF 扩展提案 |
| 38 | 3DGS Rasterization Pipeline | 21.3DGS 🆕 | 🟡 | 2025-2026 主流引擎集成 |
| 39 | Hybrid Mesh + 3DGS Compositing | 21.3DGS 🆕 | 🟡 | SplatBus / GDGS |
| 40 | Nanite-style LOD for GS | 21.3DGS 🆕 | 🟢 | NanoGS (2026) |
| 41 | 4DGS (Dynamic/Volumetric) | 21.3DGS 🆕 | 🟢 | MLSLabs, Bevy |
| 42 | Relightable 3DGS | 21.3DGS 🆕 | 🟢 | Volinga / Pixotope |
| 43 | Deformable Beta Splatting | 21.3DGS 🆕 | 🟢 | SIGGRAPH 2025 (USC ICT) |
| 44 | VR/XR 3DGS Support | 21.3DGS 🆕 | 🟢 | Bevy OpenXR, MLSLabs Pro |
| 45 | Screen-Space Caustics (Newton) | 22.焦散 🆕 | 🟡 | JCGT 2026 |
| 46 | SMS + ReSTIR Caustics | 22.焦散 🆕 | 🟢 | SIGGRAPH Asia 2025 |
| 47 | Markov Chain Path Guiding | 22.焦散 🆕 | 🟢 | KIT 2025 |
| 48 | WebGPU RHI Backend | 架构 | 🟢 | W3C Standard 2025 |
| 49 | **Actor-Component 渲染架构** 🆕 | 2.场景管理 | 🔴 | UE5 Actor-Component 模型 + C++26 反射 |
| 50 | **Scene Graph + Transform 层级** 🆕 | 2.场景管理 | 🔴 | 树形空间层级，Dirty Flag 传播 |
| 51 | **渲染组件族 (StaticMesh/Light/Camera/SkeletalMesh/Decal/Particle/3DGS/Volume)** 🆕 | 2.场景管理 | 🔴 | 以 Component 形式挂载渲染图元 |
| 52 | **Component 生命周期与依赖** 🆕 | 2.场景管理 | 🔴 | OnCreate→Start→Update→Destroy + [[engine::require<T>]] |
| 53 | **Prefab + Diff 序列化** 🆕 | 2.场景管理 | 🟡 | Entity 模板，Override 存储，继承变体 |
| 54 | **Network Replication (Component 级)** 🆕 | 2.场景管理 | 🟢 | `[[engine::replicated]]` 属性自动网络同步 + RPC |
| 55 | **Spatial Hash Grid** 🆕 | 2.场景管理 | 🟢 | 空间哈希网格加速邻近查询 |
| 56 | **Level Streaming + Component 标记** 🆕 | 2.场景管理 | 🟡 | `[[engine::streaming_source]]` 异步关卡加载 |

---

> **文档版本**: v3.1  
> **基于**: 原始 NewEngine_Feature_Plan.docx (v1) + 2025-2026 最新渲染技术研究 + Actor-Component 架构  
> **特性变更**: 294 → 320+ (+26 个全新特性，+56 个增强/新增子项)  
> **v3 核心新增**: Actor-Component 渲染架构 (26 项)  
> **实施周期**: 约 115 周 (约 2.2 年)，8 个阶段  
> 
> 关键参考文献:
> - GDC 2025/2026 Advanced Graphics Summit
> - SIGGRAPH 2025/2026 Conference Papers
> - GTC 2025/2026 NVIDIA Keynotes
> - Eurographics 2026
> - DirectX Shader Model 6.9/6.10 Specifications
> - Vulkan 1.3.296+ Extensions
> - NVIDIA RTX Kit Documentation
> - JCGT 2026
