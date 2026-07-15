# HugEngine 开发进度

> 最后更新: 2026-07-15（今日更新: GPU Driven 全链路完成 — AsyncCompute + 两阶段剔除 + PTG + DGC + WorkGraph + Forward+）

## 整体进度

核心渲染功能 + 子系统架构 + RenderGraph 集成 + IBL + RSM + 阴影 + 延迟渲染完成。
抗锯齿架构 + 后处理链路层已设计并实现接口层。
屏幕空间效果 (SSAO/SSGI/SSR) + DDGI 动态探针 GI 完成。
后处理链完整: Bloom + DOF + MotionBlur + AutoExposure + ColorGrading。
基础 Transform 动画系统 + Shader 热重载 + GPU Profiling + AsyncCompute 基础设施。

- **Phase 1**: 引擎模块 + PBR 前向管线 + 编辑器 + 阴影 + HDR + 后处理链 + TAA/FXAA + 动画基础 ✅
- **Phase 1-4**: 全面完成后处理链 (Bloom/DOF/MotionBlur/AutoExposure/ColorGrading) ✅
- **Phase 2**: GPU Driven — Bindless + GPU Culling + CSM + IBL + Clustered Shading + ExecuteIndirect (Deferred+Forward) ✅
- **Phase 2a (GPU Driven 升级)**: 两阶段遮挡剔除 + Hi-Z 金字塔 + PTG 剔除 + AsyncCompute 开启 ✅
- **DGC (Device Generated Commands)**: VK_EXT_device_generated_commands + GPU 自主命令生成 ✅
- **GPU Work Graph**: 软件模拟框架 (Entry/Compute/Draw 节点链 + 原子计数器 Record 传递) ✅
- **Forward+ Pipeline**: ClusteredShading 集成到 ForwardPipeline ✅
- **Phase 5**: 三缓冲帧环 + 辅助命令缓冲 + 多线程视锥剔除 + 录制 ✅
- **GI_IBL**: 环境光照（辐照度 32² + 预滤波 5-mip + BRDF LUT） ✅
- **GI_RSM**: Reflective Shadow Maps（独立深度缓冲 + 光栅化生成 + PBR 采样） ✅
- **GI_SSGI**: 屏幕空间全局光照（深度缓冲 Ray Marching + 降噪） ✅
- **GI_SSR**: 屏幕空间反射（深度缓冲 Ray Marching + 降噪） ✅
- **GI_DDGI**: 动态漫反射 GI（Compute Shader 3D 探针网格 + SH 投影 + 时间混合） ✅
- **SSAO**: 屏幕空间环境光遮蔽（半球采样 + 双边模糊） ✅
- **Denoiser**: 5×5 双边模糊降噪（SSGI/SSR 双实例共用） ✅
- **CameraController**: 可复用相机控制（Free/Ground + 配置持久化） ✅
- **RHI Compute Shader**: Dispatch + Compute PSO + .comp.slang ✅
- **RenderGraph**: 声明式 Pass 编排 + Barrier 推导 + 别名 + 裁剪 ✅
- **前向渲染 RG 集成**: FullScene(旧打包) + ToneMap + ImGui LoadOp ✅
- **ShadowSystem**: CSM + Point Cubemap + Spot + 阴影专用 Object Buffer ✅
- **PostProcess**: ToneMapPass + SkyboxPass + BloomPass + DOFPass + MotionBlurPass + AutoExposurePass + ColorGradingPass ✅
- **DeferredPipeline**: GBuffer 5×MRT (含 worldPos) + 全屏 Lighting Pass (直接读世界坐标) + ShadowSystem 集成 + Sponza 场景 ✅
- **描述符集竞态修复**: 拆分为 set=0(per-frame) + set=1(per-mesh) ✅
- **抗锯齿架构**: IAntiAliasing 接口 + IPostProcessPass 中间层 ✅
- **AA_TAA**: 时域抗锯齿（含 Velocity Buffer GBuffer MRT4）✅
- **AA_FXAA**: LDR 空间快速近似抗锯齿 ✅
- **AA_SMAA**: LDR 空间子像素形态学抗锯齿（3 Pass，与 FXAA 互斥）✅
- **AA_MSAA**: 硬件多重采样（IAntiAliasing 接口 + TextureDesc.sampleCount）✅
- **Clustered Shading**: 视锥空间 Cluster 划分 + 光源剔除 ✅
- **GPU Culling**: Compute Shader 视锥 + Hi-Z 遮挡剔除 ✅
- **ExecuteIndirect**: DeferredPipeline (MeshBatcher + DrawIndexedIndirect + CPU 回退) ✅ + ForwardPipeline ✅
- **AsyncCompute**: 双队列 + Timeline Semaphore + RenderGraph 异步调度 ✅
- **Shader Hot Reload**: FileWatcher + slangc 重编译 + PSO 热替换 ✅
- **GPU Profiling**: 时间戳查询 + ProfilerManager + RenderGraph 集成 + ImGui 面板 ✅
- **VMA 集成**: Vulkan Memory Allocator 替换裸 vkAllocateMemory ✅
- **Animation**: Transform 关键帧动画组件 (Translation/Rotation/Scale) ✅
- **Samples**: 02.Cube, 03.Sponza (Forward), 04.Deferred (Sponza+延迟) ✅

## 模块完成度

### L2 — RHI（渲染硬件接口层）✅
| 特性 | 状态 |
|------|:---:|
| Vulkan 后端（拆分为 5 个 .cpp 文件） | ✅ |
| 三缓冲帧环 + 持久映射 | ✅ |
| 辅助命令缓冲 + 多线程录制 | ✅ |
| Compute Shader (Dispatch + Compute PSO) | ✅ |
| BeginRenderPass LoadOp::Load (ImGui 叠加) | ✅ |
| BeginOffscreenPassMRT (最多 8 颜色附件, Vulkan 标准上限) | ✅ |
| PipelineBarrier (纹理 + 全局布局转换) | ✅ |
| VMA 集成 (VulkanMemoryAllocator) | ✅ |
| AsyncCompute (双队列 + Timeline Semaphore + 跨队列同步) | ✅ |
| 时间戳查询 (QueryPool + GPU Profiler) | ✅ |
| Ray Tracing (BLAS/TLAS + RT PSO + SBT + TraceRays) | ✅ |
| AccelerationStructure 描述符类型 + UpdateDescriptorSet(AS*) | ✅ |
| BufferUsage::AccelerationStruct → AS build input flag | ✅ |

### L3 — Shader（着色器层）✅
| 特性 | 状态 |
|------|:---:|
| Slang 编译 + per-shader .spv.h 拆分 | ✅ |
| PBR / Shadow / ToneMap / Skybox | ✅ |
| IBL (Irradiance + Prefilter 5-mip + BRDF LUT) | ✅ |
| RSM_Generate (双 MRT: position + normal+flux) | ✅ |
| GBuffer (5 MRT: albedo+metallic / normal+roughness / emissive+ao / velocity + worldPos + D32) | ✅ |
| ShaderTypes.slang (C++/Slang Push Constant 结构体统一, GPU_STRUCT 宏) | ✅ |
| DeferredLighting (全屏 PBR + IBL + RSM + Shadow + SSGI + SSR + DDGI) | ✅ |
| SSGI (屏幕空间 Ray Marching 间接漫反射) | ✅ |
| SSR (屏幕空间 Ray Marching 镜面反射) | ✅ |
| SSAO (半球采样环境光遮蔽 + 双边模糊) | ✅ |
| DDGI (Compute Shader 探针网格 + Fibonacci 球面采样 + SH 3波段投影 + 时间混合) | ✅ |
| Denoise (5×5 双边模糊降噪) | ✅ |
| GPUCull (Compute 视锥 + Hi-Z 遮挡剔除) | ✅ |
| HiZDownsample (深度金字塔生成) | ✅ |
| TAA_Resolve (时域抗锯齿) | ✅ |
| FXAA (LDR 快速近似抗锯齿) | ✅ |
| Bloom (BrightPass + GaussianBlur 双 Pass + Composite) | ✅ |
| DOF (CoC 计算 + 模糊 + 合成) | ✅ |
| MotionBlur (velocity 方向采样模糊) | ✅ |
| AutoExposure (Compute 降采样 256 partial sum + 时间混合) | ✅ |
| ColorGrading (LDR 饱和度/对比度/振铃调节) | ✅ |
| SMAA (EdgeDetect + BlendWeight + Neighborhood 3 Pass) | ✅ |
| Shader Hot Reload (FileWatcher + 自动重编译 + PSO 热替换) | ✅ |
| RT_Triangle.rgen/rmiss (Phase 1: 过程化三角形) | ✅ |
| RT_Sponza.rgen/rmiss/rchit (Phase 2-3: 几何体 RayGen/Miss/ClosestHit) | ✅ |
| RT_Shadow.rgen + RT_Common.rmiss/rchit (Phase 3+ 存根) | ✅ |

### L4 — Render（渲染层）
| 子系统 | 状态 |
|--------|:---:|
| ForwardPipeline (PBR + 多光源 + 阴影 + Skybox + IBL + RSM) | ✅ |
| DeferredPipeline (GBuffer + SSGI/SSR/DDGI + Lighting + SSAO + Denoiser + 完整后处理链) | ✅ |
| RenderGraph (声明式编排 + Barrier + 别名 + 裁剪) | ✅ |
| IRenderPipeline 基类 | ✅ |
| IRenderSubsystem 基类 | ✅ |
| IPostProcessPass 中间层（后处理链路接口） | ✅ |
| IGlobalIllumination → GI_IBL, GI_RSM, GI_SSGI, GI_SSR, GI_DDGI, GI_None | ✅ |
| IShadowSystem → ShadowSystem (CSM + Point + Spot) | ✅ |
| ToneMapPass + SkyboxPass + SSAO + Denoiser | ✅ |
| BloomPass + DOFPass + MotionBlurPass | ✅ |
| AutoExposurePass + ColorGradingPass | ✅ |
| IAntiAliasing → AA_None, AA_TAA, AA_FXAA, AA_SMAA, AA_MSAA | ✅ |
| SceneRenderer (实体收集 → 视锥剔除 → 上传) | ✅ |
| ClusteredShading (视锥 Cluster + 光源剔除 + LightGrid) | ✅ |
| GPUCulling (Compute 视锥 + Hi-Z 遮挡剔除) | ✅ |
| MeshBatcher (GPU Driven 批量绘制 + DrawIndexedIndirect) | ✅ |
| ProfilerManager (GPU 时间戳查询 + RenderGraph 集成 + ImGui 面板) | ✅ |
| ShaderHotReload (FileWatcher + slangc 编译 + PSO 热替换) | ✅ |
| RTPass (BLAS/TLAS 管理 + SBT + 描述符集 + 材质/光源 UB/纹理) | ✅ |
| ImGui LoadOp::Load (UI 叠加 + GI/后处理参数面板) | ✅ |

### L5 — Scene（场景层）✅
| 特性 | 状态 |
|------|:---:|
| Entity/Component/Transform/World/SceneGraph | ✅ |
| Mesh/Light/Skybox 组件 | ✅ |
| AnimationComponent (Transform 关键帧: Translation/Rotation/Scale) | ✅ |
| CameraController (Free/Ground 模式 + 配置持久化) | ✅ |
| ECS 反射系统 (Component/Tag/System 注册) | ✅ |
| SceneSerializer (关卡序列化/反序列化) | ✅ |

### Editor ✅
| 特性 | 状态 |
|------|:---:|
| EditorContext + ImGui 集成 | ✅ |
| ViewportPanel (3D 视口 + 相机控制) | ✅ |
| OutlinerPanel (实体层级浏览器) | ✅ |
| DetailsPanel (组件属性编辑 + Transform/Light/Mesh) | ✅ |
| ContentBrowserPanel (资源浏览器) | ✅ |
| ConsolePanel (日志/命令控制台) | ✅ |
| StatsPanel (性能统计面板) | ✅ |
| ProjectSettingsPanel (项目设置编辑器) | ✅ |
| MaterialEditor (材质属性编辑器) | ✅ |
| Gizmo (编辑器 Transform 小控件) | ✅ |
| LevelLoader (关卡加载器) | ✅ |
| CVar 控制台变量系统 | ✅ |
| Command 命令系统 | ✅ |

## GI 技术对比

| 技术 | 类型 | 空间 | 采样方式 | 反弹 | 性能 | 状态 |
|------|------|------|---------|------|------|:---:|
| GI_IBL | 环境光照 | 世界 | Cubemap 预卷积 | ∞ | 极高 | ✅ |
| GI_RSM | 反射阴影贴图 | 世界 | 光源视角光栅化 | 1 | 高 | ✅ |
| GI_SSGI | 屏幕空间 GI | 屏幕 | 深度 Ray Marching | 1 | 中 | ✅ |
| GI_SSR | 屏幕空间反射 | 屏幕 | 深度 Ray Marching | 1 | 中 | ✅ |
| GI_DDGI | 动态探针 GI | 世界 | Compute SH 投影 | ∞ | 中 | ✅ |
| GI_VXGI | 体素锥追踪 | 世界 | 3D Clipmap Cone Trace | ∞ | — | ⬜ |
| GI_ReSTIR | 时空重采样 | 世界 | 重要性采样 + 光追 | ∞ | — | ⬜ |

## 后处理技术对比

| 技术 | 类型 | 管线 | 空间 | 说明 |
|------|------|------|------|------|
| Bloom | HDR 后处理 | Deferred | HDR | BrightPass + GaussianBlur ×2 + Composite |
| DOF (景深) | HDR 后处理 | Deferred | HDR | CoC 计算 + 模糊 + 合成 (GBuffer 深度) |
| MotionBlur | HDR 后处理 | Deferred | HDR | velocity 方向采样 (GBuffer velocity buffer) |
| AutoExposure | HDR 后处理 | Deferred | HDR | Compute 256 partial sum + CPU 时间混合 |
| ColorGrading | LDR 后处理 | Deferred | LDR | 饱和度/对比度/振铃 (ToneMap→FXAA 之间) |
| TAA | HDR 抗锯齿 | Deferred | HDR | 时域累积 + 邻域裁剪 (GBuffer 深度/法线/velocity) |
| FXAA | LDR 抗锯齿 | Both | LDR | 边缘检测 + 混合 (ColorGrading 之后) |
| SSAO | 屏幕空间 AO | Both | HDR | 半球采样 + 双边模糊 |
| SMAA | LDR 抗锯齿 | Both | LDR | 3 Pass 形态学 (EdgeDetect+BlendWeight+Neighborhood)，与 FXAA 互斥 |

## 架构设计

### DeferredPipeline 渲染流程

```
DeferredPipeline::BuildFrameGraph
  ├── "GPU_Cull" Pass (GPU 剔除启用时)
  │     └── Compute Shader: 读上帧深度 → GPUScene 视锥+Hi-Z 剔除 → IndirectCmdBuf
  ├── "Shadow" Pass (阴影投射光源存在时)
  │     └── ShadowSystem::Update → 收集光源 GPUShadowData（光源 VP 矩阵）
  │         ShadowSystem::Render → CSM(3 cascade) + Spot 阴影贴图渲染
  ├── "GB_Clear" Pass
  │     └── 5 MRT GBuffer (albedo+metallic / normal+roughness / emissive+ao / velocity / worldPos) + D32
  │         绑定 set=0(per-frame ObjectBuffer) + bindless 纹理数组
  │         使用上帧 GPU Culling Readback 结果过滤可见物体
  ├── [DDGI_Update] Pass (DDGI 启用时)
  │     └── Compute Shader: 3D 探针网格 Fibonacci 球面采样 GBuffer → SH 投影 + 时间混合
  ├── "SSAO" Pass → SSAO Output
  │     └── 半球采样深度+法线 → 环境光遮蔽
  ├── "SSR" Pass → SSR Output
  │     └── 屏幕空间深度 Ray Marching → 间接镜面反射
  ├── "SSR_Denoise" Pass → SSR Denoised
  │     └── 5×5 双边模糊
  ├── "SSGI" Pass → SSGI Output
  │     └── 屏幕空间半球采样 → 间接漫反射 (Uniform Buffer 传参)
  ├── "SSGI_Denoise" Pass → SSGI Denoised
  │     └── 5×5 双边模糊
  ├── "Lighting" Pass → HDR Target
  │     └── 全屏 PBR + GBuffer 采样 + IBL + RSM + Shadow
  │         + SSGI (降噪后) + SSR (降噪后) + DDGI (三线性插值 SH 评估) + SSAO
  │         + Clustered Shading (LightGrid 多光源剔除)
  ├── "DDGI_CaptureHDR" Pass (DDGI 启用时, Noop if disabled)
  │     └── 将当前帧 HDR Lighting 输出拷贝到 DDGI radiance 纹理
  │         供下帧探针采样真实辐射度（替代旧的 albedo*0.3 近似）
  ├── "AutoExposure" Pass (Compute)
  │     └── Compute Shader: 256 组并行降采样 HDR 亮度 → SSBO → CPU 平均 + 时间混合
  ├── "Bloom" Pass (可选) → Bloom Output
  │     └── BrightPass → GaussianBlur H → GaussianBlur V → Composite
  ├── "DOF" Pass (可选) → DOF Output
  │     └── CoC 计算 → 散景模糊 → 合成 (读上一步输出或 HDR)
  ├── "MotionBlur" Pass (可选) → MB Output
  │     └── velocity 方向采样模糊 (读上一步输出或 HDR)
  ├── "TAA_Resolve" Pass (TAA 启用时)
  │     └── 时域抗锯齿 (HDR 空间, 复用 GBuffer 深度/法线/velocity, 读后处理链输出)
  ├── "ToneMap" Pass → LDR Target / BackBuffer
  │     └── HDR→LDR ACES Tonemapping (含 AutoExposure 曝光值)
  │         输出到 LDR 中间纹理（有下游 Pass 时）或直接输出到 BackBuffer
  ├── "ColorGrading" Pass (可选) → CG Output
  │     └── LDR 色彩分级: Saturation + Contrast + Vibrance (ToneMap 之后、AA 之前)
  ├── "SMAA" Pass (可选) → BackBuffer (与 FXAA 互斥)
  │     └── LDR 空间形态学抗锯齿 3 Pass: EdgeDetect→BlendWeight→Neighborhood
  └── "FXAA" Pass (可选) → BackBuffer
        └── LDR 空间快速近似抗锯齿 (edge-detect + blend, SMAA 未启用时执行)
```

### 后处理链架构

```
Lighting 输出 (HDR)
  │
  ├─ DDGI_CaptureHDR ──→ DDGI Radiance 纹理（下帧探针采样用）
  │
  ├─ AutoExposure ──→ exposure 值（Compute reduction + 时间混合）
  │
  ├─ [Bloom]      ← SetInput → BrightPass → BlurH → BlurV → Composite → GetOutput=HDR
  ├─ [DOF]        ← SetInput (读 Bloom/HDR) → CoC → Blur → Comp → GetOutput=HDR
  ├─ [MotionBlur] ← SetInput (读 DOF/Bloom/HDR) + velocity → 采样模糊 → GetOutput=HDR
  ├─ [TAA]        ← SetInput (读 MB/DOF/Bloom/HDR) + GBuffer → Render → GetOutput=HDR_AA
  ├─ ToneMap      ← SetInput → BeginRP → Render → GetOutput=LDR (含 exposure 参数)
  ├─ [ColorGrading] ← SetInput → BeginRP → Render → GetOutput=LDR (饱和度/对比度/振铃)
  ├─ [FXAA]       ← SetInput → BeginRP → Render → GetOutput=LDR_AA (→ Present)
  └─ Present
```

后处理责任链（按序串联，每个 Pass 可选启用，输出级联到下一个）：
```
HDR → Bloom → DOF → MotionBlur → TAA → ToneMap → ColorGrading → [SMAA|FXAA] → Present
```

### 子系统继承树

```
IRenderSubsystem
  ├── IShadowSystem → ShadowSystem, ShadowNone
  ├── IGlobalIllumination → GI_IBL, GI_RSM, GI_SSGI, GI_SSR, GI_DDGI, GI_None
  ├── SkyboxPass（场景 Pass）
  └── IPostProcessPass（后处理链路层）
        ├── ToneMapPass
        ├── AutoExposurePass (Compute, HDR 亮度统计)
        ├── BloomPass (BrightPass + GaussianBlur + Composite)
        ├── DOFPass (CoC + 模糊 + 合成)
        ├── MotionBlurPass (velocity 方向采样)
        ├── ColorGradingPass (LDR 色彩分级)
        └── IAntiAliasing → AA_None, AA_TAA, AA_FXAA, AA_SMAA, AA_MSAA
```

### 目录结构

```
Engine/Render/
├── Pipeline/       (IRenderPipeline, ForwardPipeline, DeferredPipeline, CameraController, Material,
│                    ClusteredShading, GPUCulling, GPUScene, MeshBatcher, GBufferRenderer)
├── Subsystem/      (IRenderSubsystem)
├── GI/             (IGlobalIllumination, GI_None, GI_IBL, GI_RSM, GI_SSGI, GI_SSR, GI_DDGI)
├── Shadow/         (IShadowSystem, ShadowSystem, ShadowNone, CSMTechnique, PointShadowTechnique, SpotShadowTechnique)
├── PostProcess/    (IPostProcessPass, ToneMapPass, SkyboxPass, SSAO, Denoiser, BloomPass, DOFPass,
│                    MotionBlurPass, AutoExposurePass, ColorGradingPass, GaussianBlurPass)
├── AntiAliasing/   (IAntiAliasing, AA_None, AA_TAA, AA_FXAA, AA_SMAA, AA_MSAA)
├── Profiler/       (ProfilerManager)
├── SceneRenderer.h/.cpp
├── RenderGraph.h/.cpp
└── ShaderHotReload.h/.cpp

Engine/RHI/Vulkan/  (拆分为 5 个文件)
├── VulkanInternal.h
├── VulkanDevice.cpp
├── VulkanSwapChain.cpp
├── VulkanCommandList.cpp
├── VulkanResources.cpp
├── VulkanPipeline.cpp
└── VulkanQueryPool.h

Engine/Shader/Shaders/  (Slang → SPIR-V → .spv.h)
├── GBuffer, DeferredLighting, PBR            (核心着色器)
├── SSGI, SSR, SSAO, SSAO_Blur               (屏幕空间效果)
├── Denoise                                   (降噪)
├── DDGI                                      (Compute 探针 GI)
├── GPUCull, HiZDownsample                   (Compute 剔除)
├── IBL_Irradiance, IBL_Prefilter, IBL_BRDF_LUT (IBL 生成)
├── RSM_Generate                             (RSM 生成)
├── TAA_Resolve, FXAA                        (抗锯齿)
├── ToneMap, Skybox, Shadow                  (后处理 + 阴影)
├── Bloom (BrightPass + GaussianBlur + Composite) (Bloom 后处理)
├── DOF_CoC + DOF_Composite                  (景深后处理)
├── MotionBlur                               (运动模糊后处理)
├── AutoExposure.comp                        (自动曝光 Compute)
├── ColorGrading                             (LDR 色彩分级)
├── SMAA_EdgeDetect + SMAA_BlendWeight + SMAA_Neighborhood  (SMAA 3 Pass)
├── RT_Triangle, RT_Background, RT_Shadow     (RT 着色器: Phase 1-2 存根)
├── RT_Sponza.rgen/rmiss/rchit               (RT 着色器: Phase 2-3 几何体渲染)
├── RT_Common.rmiss/rchit                    (RT 着色器: Phase 3+ 通用存根)
├── ShaderTypes.slang                         (C++/Slang 共享 GPU 结构体定义)
└── common, pbr_common                       (公共头文件)
```

### AA 技术分配

| AA 技术 | ForwardPipeline | DeferredPipeline | 空间 | 原因 |
|---------|:---:|:---:|------|------|
| None | ✓ | ✓ | — | 空操作 |
| MSAA | ✓ | ✗ | HDR | 延迟 GBuffer MRT 多采样代价过高 |
| TAA | ✗ | ✓ | HDR | 复用 GBuffer 深度/法线/velocity 做邻域裁剪 |
| FXAA | ✓ | ✓ | LDR | 纯 LDR 后处理，无管线依赖 |
| SMAA | ✓ | ✓ | LDR | 3 Pass 形态学（边缘检测+权重计算+邻域混合），与 FXAA 互斥 |
| MSAA | ✓ | ⚠️ | HDR | 硬件多采样（仅 HDR 目标，GBuffer 保持 1x，需重启生效） |

### Ray Tracing 渲染流程（Phase 2-3）

```
RT 模式帧循环:
  pipeline.Render()                              ← 填充 GPUObjectData + 光栅化输出
  rtPass.BuildAS(world, sg)                      ← BLAS(几何变更时) + TLAS(每帧)
  rtPass.UpdateLightBuffer(lightBuf)             ← 光源 UB 填充
  PipelineBarrier(Undefined → UAV)               ← BackBuffer 准备 RT 写入
  BindRTPipeline → BindDescriptorSets
  SetPushConstants(invViewProj, camPos)          ← 相机数据
  TraceRays(w, h)
  PipelineBarrier(UAV → RenderTarget)            ← BackBuffer 准备 ImGui
  SetPipeline(rasterPSO)
  BeginRenderPass(Load) → ImGui → EndRenderPass  ← ImGui 叠加
  Present
```

**数据流**：
```
MeshComponent.baseColorFactor → Texture2D(1×256 RGBA32F) → ClosestHit::Load(id)
GPULight SSBO → Uniform Buffer(cbuffer) → ClosestHit::u_Lights[li]
Camera → PushConstant → RayGen::invViewProj → world ray
```

**已知 RT 限制**：
| 限制 | 原因 | 方案 |
|------|------|------|
| SSBO 在 ClosestHit 中 GPU fault | slangc SPIR-V 兼容性 | ✅ UB/纹理替代 |
| 无纹理采样 | bindless 纹理未接入 RT | Phase 4 |
| 法线近似 (-WorldRayDirection) | 无顶点缓冲 SSBO | Phase 4 |
| 无阴影射线 | 需 GPUShadowData SSBO | Phase 4 |

---

## 已知限制

| 问题 | 影响 | 状态 |
|------|------|:---:|
| FullScene Pass 仍为老代码打包 | 未拆分为独立 Shadow/IBL/HDR Pass | ⬜ |
| DeferredPipeline: ShadowSystem 已集成 Update/Render + Shadow pass | 光源 VP 矩阵正确传入 Lighting | ✅ |
| DeferredPipeline: IBL iblIntensity 此前硬编码为 0 (已修复) | 环境光始终禁用 | ✅ |
| DeferredPipeline: 深度 Linear→Nearest 采样修复 | 边缘混合导致 worldPos 错误 | ✅ |
| 点光阴影无 PCF 软滤波 | 硬边缘锯齿 | ⬜ |
| 点光阴影无视锥剔除 | 6 面渲染全场景 | ⬜ |
| GPUCulling::Dispatch 在 RenderPass 内执行 | Vulkan 校验警告 | ⬜ |
| DDGI 探针可见性测试使用简单深度比较 | 可能出现漏光/遮挡错误 | ⬜ |
| Forward+ (Tile-based Light Culling) | 前向管线仍用传统多光源遍历 | ⬜ |
| AsyncCompute 默认关闭 | 等待多阶段提交架构完善 | ⬜ |

## 待办任务

| # | 任务 | 状态 |
|---|------|:---:|
| 1 | DDGI 探针可见性优化（多步 march + 深度偏移减少漏光） | ⬜ |
| 2 | FullScene 拆分为独立 Shadow/IBL/HDR Pass | ⬜ |
| 3 | 点光阴影 PCF 软滤波 | ⬜ |
| 4 | 点光阴影视锥剔除 | ⬜ |
| 5 | GPUCulling Dispatch 移出 RenderPass | ✅ 2026-07-14 (确认无需修复) |
| 6 | AsyncCompute 多阶段提交架构完善 + 默认开启 | ✅ 2026-07-14 |
| 7 | 两阶段遮挡剔除 (Phase 1 粗筛 + Phase 2 精筛 + Hi-Z 金字塔) | ✅ 2026-07-14 |
| 8 | 持久化线程组剔除 (PTG — per-frame dispatch 模式) | ✅ 2026-07-15 |
| 9 | Device Generated Commands (VK_EXT_dgc) | ✅ 2026-07-15 |
| 10 | GPU Work Graph 软件模拟框架 | ✅ 2026-07-15 |
| 11 | Forward+ (Tile-based Light Culling) | ✅ 2026-07-15 |
| 12 | GI_VXGI 体素锥追踪 | ⬜ |
| 13 | HW Ray Tracing (TLAS/BLAS + RT PSO + TraceRays + 材质+光照) | ✅ Phase 1-3 完成 |
| 14 | Virtual Shadow Maps | ⬜ |
| 15 | GI_ReSTIR 时空重采样 | ⬜ |
| 16 | Prefab 系统 | ⬜ |
| 17 | Decal + ReflectionProbe | ⬜ |
| 18 | Atmosphere + Volumetrics | ⬜ |
| 19 | Skeletal Animation (骨骼动画) | ⬜ |
| 20 | Editor Undo/Redo | ⬜ |
| 21 | Nanite / Mesh Shader / Virtual Texturing | ⬜ |
| 22 | 3DGS (Gaussian Splatting) | ⬜ |

## 架构文档对比分析 (vs HugEngine_Architecture_And_Tasks.md)

### 各 Phase 完成度

| Phase | 主题 | 完成度 | 完成项 | 缺失项 |
|-------|------|:---:|--------|--------|
| P1 | 核心骨架 | ~97% | RHI Vulkan+VMA, Slang→SPIR-V, RenderGraph, ECS, glTF, Forward+Deferred, TAA, SSAO, Bloom, DOF, MotionBlur, FXAA, SMAA, MSAA(基础), AutoExposure, ColorGrading, GPU Profiling, Editor基础, Shader HotReload, AsyncCompute(基础), Animation(关键帧) | Undo/Redo |
| P2 | GPU Driven | ~95% | Bindless, GPU Culling, GPU Scene, CSM+Shadow, IBL, Clustered Shading, ExecuteIndirect+DGC (Deferred+Forward), VMA, 两阶段剔除, Hi-Z 金字塔, PTG, AsyncCompute, Forward+, WorkGraph 框架 | VSM, VRS, Decal/ReflProbe, Prefab |
| P3 | 高级几何 | ~5% | — | Nanite, Mesh Shader, Virtual Texturing, OIT, Impostor |
| P4 | GI + RT | ~30% | DDGI (HDR radiance), Denoiser, SSGI, SSR, **HW RT Phase 1-3** (BLAS/TLAS + TraceRays + 材质+光照) | Lumen GI, VXGI, ReSTIR, NRD, NRC, RT 顶点法线/纹理/阴影 |
| P5 | 神经渲染 | 0% | — | DLSS/FSR/XeSS, FrameGen, RayRecon, Neural Materials |
| P6 | 大气+后处理+动画 | ~40% | Bloom, DOF, MotionBlur, AutoExposure, ColorGrading, 关键帧动画 | Atmosphere, Volumetrics, 骨骼动画, 地形植被 |
| P7 | 高斯泼溅+焦散 | 0% | — | 3DGS, 4DGS, 焦散 |
| P8 | 打磨发布 | 0% | — | WebGPU, PSO Cache, Full PT, VR/XR |

### 按优先级缺失功能 Top 20

| # | 功能 | Phase | 重要性 | 说明 |
|---|------|:---:|:---:|------|
| 1 | DDGI 探针可见性优化 | P4 | 🟡 中 | 多步 march + 深度偏移减少漏光 |
| 2 | FullScene 拆分为独立 Pass | P1 | 🟡 中 | Shadow / IBL / HDR 解耦 |
| 3 | 点光阴影 PCF 软滤波 + 视锥剔除 | P1 | 🟡 中 | 阴影质量与性能优化 |
| 4 | AsyncCompute 完善 + 默认开启 | P1 | 🟡 中 | 多阶段提交架构 |
| 5 | GPUCulling Dispatch 移出 RenderPass | P2 | 🟢 低 | Vulkan 校验警告修复 |
| 6 | Forward+ (ForwardPipeline + ClusteredShading 模式) | P2 | 🟡 中 | 前向管线 Tile-based 光源剔除 |
| 7 | HW Ray Tracing (纹理采样 + 顶点法线 + 阴影) | P4 | 🟢 低 | Phase 1-3 ✅, Phase 4 待规划 |
| 8 | GI_VXGI 体素锥追踪 | P4 | 🟢 低 | 3D Clipmap Cone Trace |
| 9 | Virtual Shadow Maps | P3 | 🟢 低 | 大规模高质量阴影 |
| 10 | Prefab 系统 | P2 | 🟢 低 | 编辑器工作流 |
| 11 | Decal + ReflectionProbe | P2 | 🟢 低 | 场景丰富度 |
| 12 | Skeletal Animation | P6 | 🟡 中 | 角色动画 |
| 13 | Atmosphere + Volumetrics | P6 | 🟢 低 | 天空/雾/云 |
| 14 | Editor Undo/Redo | P1 | 🟢 低 | 编辑器体验 |
| 15 | Nanite / Mesh Shader | P3 | 🟢 低 | 虚拟几何 |
| 16 | Virtual Texturing | P3 | 🟢 低 | 大地形纹理流式加载 |
| 17 | GI_ReSTIR 时空重采样 | P4 | 🟢 低 | 重要性采样 + 光线追踪 |
| 18 | 3DGS (Gaussian Splatting) | P7 | 🟢 低 | 新图元类型 + 4DGS |
