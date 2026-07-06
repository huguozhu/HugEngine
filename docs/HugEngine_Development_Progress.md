# HugEngine 开发进度

> 最后更新: 2026-07-07

## 整体进度

核心渲染功能 + 子系统架构 + RenderGraph 集成 + IBL + RSM + 阴影 + 延迟渲染完成。
抗锯齿架构 + 后处理链路层已设计并实现接口层。
屏幕空间效果 (SSAO/SSGI/SSR) + DDGI 动态探针 GI 完成。

- **Phase 1-4**: 引擎模块 + PBR 前向管线 + 编辑器 + 阴影 + HDR ✅
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
- **PostProcess**: ToneMapPass + SkyboxPass ✅
- **DeferredPipeline**: GBuffer 4×MRT + 全屏 Lighting Pass + Sponza 场景 ✅
- **描述符集竞态修复**: 拆分为 set=0(per-frame) + set=1(per-mesh) ✅
- **抗锯齿架构**: IAntiAliasing 接口 + IPostProcessPass 中间层 ✅
- **AA_TAA**: 时域抗锯齿（含 Velocity Buffer GBuffer MRT4）✅
- **Clustered Shading**: 视锥空间 Cluster 划分 + 光源剔除 ✅
- **GPU Culling**: Compute Shader 视锥 + Hi-Z 遮挡剔除 ✅
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
| BeginOffscreenPassMRT (多颜色附件) | ✅ |
| PipelineBarrier (纹理 + 全局布局转换) | ✅ |

### L3 — Shader（着色器层）✅
| 特性 | 状态 |
|------|:---:|
| Slang 编译 + per-shader .spv.h 拆分 | ✅ |
| PBR / Shadow / ToneMap / Skybox | ✅ |
| IBL (Irradiance + Prefilter 5-mip + BRDF LUT) | ✅ |
| RSM_Generate (双 MRT: position + normal+flux) | ✅ |
| GBuffer (4 MRT: albedo+metallic / normal+roughness / emissive+ao / velocity + D32) | ✅ |
| DeferredLighting (全屏 PBR + IBL + RSM + Shadow + SSGI + SSR + DDGI) | ✅ |
| SSGI (屏幕空间 Ray Marching 间接漫反射) | ✅ |
| SSR (屏幕空间 Ray Marching 镜面反射) | ✅ |
| SSAO (半球采样环境光遮蔽 + 双边模糊) | ✅ |
| DDGI (Compute Shader 探针网格 + Fibonacci 球面采样 + SH 3波段投影 + 时间混合) | ✅ |
| Denoise (5×5 双边模糊降噪) | ✅ |
| GPUCull (Compute 视锥 + Hi-Z 遮挡剔除) | ✅ |

### L4 — Render（渲染层）
| 子系统 | 状态 |
|--------|:---:|
| ForwardPipeline (PBR + 多光源 + 阴影 + Skybox + IBL + RSM) | ✅ |
| DeferredPipeline (GBuffer + SSGI/SSR/DDGI + Lighting + SSAO + Denoiser) | ✅ |
| RenderGraph (声明式编排 + Barrier + 别名 + 裁剪) | ✅ |
| IRenderPipeline 基类 | ✅ |
| IRenderSubsystem 基类 | ✅ |
| IPostProcessPass 中间层（后处理链路接口） | ✅ |
| IGlobalIllumination → GI_IBL, GI_RSM, GI_SSGI, GI_SSR, GI_DDGI, GI_None | ✅ |
| IShadowSystem → ShadowSystem (CSM + Point + Spot) | ✅ |
| ToneMapPass + SkyboxPass + SSAO + Denoiser | ✅ |
| IAntiAliasing → AA_None, AA_TAA (AA_FXAA 待实现) | ✅ |
| SceneRenderer (实体收集 → 视锥剔除 → 上传) | ✅ |
| ClusteredShading (视锥 Cluster + 光源剔除 + LightGrid) | ✅ |
| GPUCulling (Compute 视锥 + Hi-Z 遮挡剔除) | ✅ |
| ImGui LoadOp::Load (UI 叠加 + GI 参数面板) | ✅ |

### L5 — Scene（场景层）✅
- Entity/Component/Transform/World/SceneGraph, Mesh/Light/Skybox

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

## 架构设计

### DeferredPipeline 渲染流程

```
DeferredPipeline::BuildFrameGraph
  ├── "GPU_Cull" Pass (GPU 剔除启用时)
  │     └── Compute Shader: 读上帧深度 → GPUScene 视锥+Hi-Z 剔除 → IndirectCmdBuf
  ├── "GB_Clear" Pass
  │     └── 4 MRT GBuffer (albedo+metallic / normal+roughness / emissive+ao / velocity) + D32
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
  ├── "TAA_Resolve" Pass (TAA 启用时)
  │     └── 时域抗锯齿 (HDR 空间, 复用 GBuffer 深度/法线/velocity)
  ├── "ToneMap" Pass → LDR Target / BackBuffer
  │     └── HDR→LDR ACES Tonemapping (FXAA 启用时输出到 LDR 中间纹理)
  └── "FXAA" Pass (FXAA 启用时) → BackBuffer
        └── LDR 空间快速近似抗锯齿 (edge-detect + blend)
```

### 后处理链架构

```
Lighting 输出 (HDR)
  │
  ├─ [TAA]   ← SetInput → Render → GetOutput=HDR_AA
  ├─ ToneMap ← SetInput → BeginRP → Render → GetOutput=LDR
  ├─ [FXAA]  ← SetInput → BeginRP → Render → GetOutput=LDR_AA (→ Present)
  └─ Present
```

### 子系统继承树

```
IRenderSubsystem
  ├── IShadowSystem → ShadowSystem, ShadowNone
  ├── IGlobalIllumination → GI_IBL, GI_RSM, GI_SSGI, GI_SSR, GI_DDGI, GI_None
  ├── SkyboxPass（场景 Pass）
  └── IPostProcessPass（后处理链路层）
        ├── ToneMapPass
        └── IAntiAliasing → AA_None, AA_TAA (AA_FXAA 待实现)
```

### 目录结构

```
Engine/Render/
├── Pipeline/       (IRenderPipeline, ForwardPipeline, DeferredPipeline, CameraController, Material)
├── Subsystem/      (IRenderSubsystem)
├── GI/             (IGlobalIllumination, GI_None, GI_IBL, GI_RSM, GI_SSGI, GI_SSR, GI_DDGI)
├── Shadow/         (IShadowSystem, ShadowSystem, ShadowNone, CSMTechnique, PointShadowTechnique, SpotShadowTechnique)
├── PostProcess/    (IPostProcessPass, ToneMapPass, SkyboxPass, SSAO, Denoiser)
├── AntiAliasing/   (IAntiAliasing, AA_None, AA_TAA, AA_FXAA)
├── SceneRenderer.h/.cpp
└── RenderGraph.h/.cpp

Engine/RHI/Vulkan/  (拆分为 5 个文件)
├── VulkanInternal.h
├── VulkanDevice.cpp
├── VulkanSwapChain.cpp
├── VulkanCommandList.cpp
├── VulkanResources.cpp
└── VulkanPipeline.cpp

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
└── common, pbr_common                       (公共头文件)
```

### AA 技术分配

| AA 技术 | ForwardPipeline | DeferredPipeline | 空间 | 原因 |
|---------|:---:|:---:|------|------|
| None | ✓ | ✓ | — | 空操作 |
| MSAA | ✓ | ✗ | HDR | 延迟 GBuffer MRT 多采样代价过高 |
| TAA | ✗ | ✓ | HDR | 复用 GBuffer 深度/法线/velocity 做邻域裁剪 |
| FXAA | ✓ | ✓ | LDR | 纯 LDR 后处理，无管线依赖 |

## 已知限制

| 问题 | 影响 |
|------|------|
| FullScene Pass 仍为老代码打包 | 未拆分为独立 Shadow/IBL/HDR Pass |
| 点光阴影无 PCF 软滤波 | 硬边缘锯齿 |
| 点光阴影无视锥剔除 | 6 面渲染全场景 |
| GPU Culling 仅 forward 管线使用 | Deferred 已集成 ✅ |
| GPUCulling::Dispatch 在 RenderPass 内执行 | Vulkan 校验警告 |
| DDGI 探针更新仅采样 albedo (无真实 radiance) | 光照估计粗糙，需前帧 HDR 或 Light Probe |
| DDGI 探针可见性测试使用简单深度比较 | 可能出现漏光/遮挡错误 |
| 后处理链无 DOF/MotionBlur | Bloom + DOF + MotionBlur 已实现 ✅ |

## 待办任务

| # | 任务 | 状态 |
|---|------|:---:|
| 9 | GI_RSM (RSM 完善) | ✅ |
| 10 | DeferredPipeline (GBuffer + LightPass + Sponza) | ✅ |
| 11 | IPostProcessPass + IAntiAliasing 接口架构 | ✅ |
| — | AA_TAA 实现（含 Velocity Buffer 4 MRT GBuffer） | ✅ |
| — | VMA 集成（Vulkan Memory Allocator 替换裸 vkAllocateMemory） | ✅ |
| — | SSAO / SSGI / SSR 屏幕空间效果 | ✅ |
| — | DDGI 动态探针 GI（Compute Shader + SH + 时间混合） | ✅ |
| — | Denoiser（5×5 双边模糊，SSGI/SSR 共用） | ✅ |
| — | Clustered Shading（视锥空间 Cluster 划分 + 光源剔除） | ✅ |
| — | GPU Culling (Compute Shader 视锥 + Hi-Z 剔除) | ✅ |
| — | Shader Hot Reload（Editor 模式 .slang 文件监控 + 自动重编译 + PSO 热替换） | 📋 |
| — | DDGI 探针真实 radiance 采样（前帧 HDR 输入） | ⬜ |
| — | DDGI 探针可见性优化（多步 march + 深度偏移） | ⬜ |
| — | GI_VXGI 体素锥追踪 | ⬜ |
| — | AA_FXAA 实现 | ✅ |
| — | FullScene 拆分为独立 Shadow/IBL/HDR Pass | ⬜ |
| — | GPUCulling 集成到 DeferredPipeline | ✅ |
| — | Bloom 后处理（DOF / MotionBlur 待实现） | ✅ |
| — | DOF 景深（CoC + 模糊 + 合成） | ✅ |
| — | MotionBlur 运动模糊（velocity 方向采样） | ✅ |
| — | ExecuteIndirect + GPU Driven (Deferred) | ✅ |
| — | GPU Profiling（时间戳查询 + ImGui 面板） | ✅ |
| — | GBuffer 描述符集绑定修复（Compute PSO） | ✅ |

## 架构文档对比分析 (vs HugEngine_Architecture_And_Tasks.md)

### 各 Phase 完成度

| Phase | 主题 | 完成度 | 完成项 | 缺失项 |
|-------|------|:---:|--------|--------|
| P1 | 核心骨架 | ~85% | RHI Vulkan, Slang→SPIR-V, RenderGraph, ECS, glTF, Forward+Deferred, TAA, SSAO, Bloom, DOF, MotionBlur, FXAA, GPU Profiling, Editor基础 | D3D12/Metal后端, AsyncCompute, Shader HotReload, AutoExposure, MSAA, Undo/Redo |
| P2 | GPU Driven | ~50% | Bindless, GPU Culling, GPU Scene, CSM+Shadow, IBL, Clustered Shading, ExecuteIndirect+DGC (Deferred) | ExecuteIndirect (Forward), VSM, VRS, Decal/ReflProbe, Prefab, Forward+, TAAU |
| P3 | 高级几何 | ~5% | — | Nanite, Mesh Shader, Virtual Texturing, OIT, Impostor |
| P4 | GI + RT | ~15% | DDGI (GBuffer), Denoiser, SSGI, SSR | HW RT, Lumen GI, VXGI, ReSTIR, NRD, NRC, RT管线 |
| P5 | 神经渲染 | 0% | — | DLSS/FSR/XeSS, FrameGen, RayRecon, Neural Materials |
| P6 | 大气+后处理+动画 | ~30% | Bloom, DOF, MotionBlur | Atmosphere, Volumetrics, 骨骼动画, 地形植被 |
| P7 | 高斯泼溅+焦散 | 0% | — | 3DGS, 4DGS, 焦散 |
| P8 | 打磨发布 | 0% | — | WebGPU, PSO Cache, Full PT, VR/XR |

### 按优先级缺失功能 Top 20

| # | 功能 | Phase | 重要性 | 说明 |
|---|------|:---:|:---:|------|
| — | GPUCulling 集成到 Deferred | P2 | 🔴 高 | ✅ 独立 GPU_Cull Pass + 修复冗余 Collect/Upload |
| — | FXAA 实现 | P1 | 🔴 高 | ✅ AA_FXAA 类 + LDR 纹理 + ToneMap→FXAA→Present |
| — | Bloom + DOF + MotionBlur | P1/P6 | 🔴 高 | ✅ BrightPass + GaussianBlur + CoC + Velocity MB |
| — | ExecuteIndirect + GPU Driven (Deferred) | P2 | 🔴 高 | ✅ MeshBatcher + DrawIndexedIndirect + CPU 回退 |
| — | GPU Profiling | P1 | 🟡 中 | ✅ 时间戳查询 + RenderGraph + ImGui 面板 |
| 1 | Shader Hot Reload | P1 | 🔴 高 | 开发效率倍增器（.slang 监控 + 自动重编译 + PSO 热替换） |
| 2 | DDGI 前帧 HDR radiance 采样 | P4 | 🟡 中 | 探针光照精度从粗糙→准确 |
| 3 | DDGI 探针可见性优化 | P4 | 🟡 中 | 多步 march + 深度偏移减少漏光 |
| 4 | PostProcess AutoExposure + ColorGrading | P1 | 🟡 中 | HDR→LDR 视觉品质 |
| 5 | ExecuteIndirect + GPU Driven (Forward) | P2 | 🟡 中 | ForwardPipeline 同 Deferred 架构改造 |
| 6 | FullScene 拆分为独立 Pass | P1 | 🟡 中 | Shadow / IBL / HDR 解耦 |
| 7 | RHI D3D12 后端 | P1 | 🟢 低 | 跨平台 Windows |
| 8 | Virtual Shadow Maps | P3 | 🟢 低 | 大规模高质量阴影 |
| 9 | HW Ray Tracing | P4 | 🟢 低 | TLAS/BLAS + RT PSO |
| 10 | Prefab 系统 | P2 | 🟢 低 | 编辑器工作流 |
| 11 | Temporal Upsampling (TAAU) | P2 | 🟢 低 | 低分辨率渲染→超采样 |
| 12 | RHI AsyncCompute | P1 | 🟢 低 | 并行 compute + graphics |
| 13 | Decal + ReflectionProbe | P2 | 🟢 低 | 场景丰富度 |
| 14 | Atmosphere + Volumetrics | P6 | 🟢 低 | 天空/雾/云 |
| 15 | Skeletal Animation | P6 | 🟢 低 | 角色动画 |
| 16 | GI_VXGI 体素锥追踪 | P4 | 🟢 低 | 3D Clipmap Cone Trace |
| 17 | 3DGS (Gaussian Splatting) | P7 | 🟢 低 | 新图元类型 |
| 18 | Editor Undo/Redo | P1 | 🟢 低 | 编辑器体验 |
| 19 | Forward+ (Tile-based Light Culling) | P2 | 🟢 低 | 前向管线多光源优化 |
| 20 | SMAA 抗锯齿 | P1 | 🟢 低 | 比 FXAA 更高质量的 LDR 后处理 |
