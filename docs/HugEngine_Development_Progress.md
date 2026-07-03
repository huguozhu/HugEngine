# HugEngine 开发进度

> 最后更新: 2026-07-03

## 整体进度

核心渲染功能 + 子系统架构 + RenderGraph 集成 + IBL + RSM + 阴影 + 延迟渲染完成。
抗锯齿架构 + 后处理链路层已设计并实现接口层。

- **Phase 1-4**: 引擎模块 + PBR 前向管线 + 编辑器 + 阴影 + HDR ✅
- **Phase 5**: 三缓冲帧环 + 辅助命令缓冲 + 多线程视锥剔除 + 录制 ✅
- **GI_IBL**: 环境光照（辐照度 32² + 预滤波 5-mip + BRDF LUT） ✅
- **GI_RSM**: Reflective Shadow Maps（独立深度缓冲 + 光栅化生成 + PBR 采样） ✅
- **CameraController**: 可复用相机控制（Free/Ground + 配置持久化） ✅
- **RHI Compute Shader**: Dispatch + Compute PSO + .comp.slang ✅
- **RenderGraph**: 声明式 Pass 编排 + Barrier 推导 + 别名 + 裁剪 ✅
- **前向渲染 RG 集成**: FullScene(旧打包) + ToneMap + ImGui LoadOp ✅
- **ShadowSystem**: CSM + Point Cubemap + 阴影专用 Object Buffer ✅
- **PostProcess**: ToneMapPass + SkyboxPass ✅
- **DeferredPipeline**: GBuffer MRT + 全屏 Lighting Pass + Sponza 场景 ✅
- **描述符集竞态修复**: 拆分为 set=0(per-frame) + set=1(per-mesh) ✅
- **抗锯齿架构**: IAntiAliasing 接口 + IPostProcessPass 中间层 ✅
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
| PipelineBarrier (纹理布局转换) | ✅ |

### L3 — Shader（着色器层）✅
| 特性 | 状态 |
|------|:---:|
| Slang 编译 + per-shader .spv.h 拆分 | ✅ |
| PBR / Shadow / ToneMap / Skybox | ✅ |
| IBL (Irradiance + Prefilter 5-mip + BRDF LUT) | ✅ |
| RSM_Generate (双 MRT: position + normal+flux) | ✅ |
| GBuffer (3 MRT + D32) | ✅ |
| DeferredLighting (全屏 PBR + IBL + RSM + Shadow) | ✅ |

### L4 — Render（渲染层）
| 子系统 | 状态 |
|--------|:---:|
| ForwardPipeline (PBR + 多光源 + 阴影 + Skybox + IBL + RSM) | ✅ |
| DeferredPipeline (GBuffer + Lighting Pass + Sponza) | ✅ |
| RenderGraph (声明式编排 + Barrier + 别名 + 裁剪) | ✅ |
| IRenderPipeline 基类 | ✅ |
| IRenderSubsystem 基类 | ✅ |
| IPostProcessPass 中间层（后处理链路接口） | ✅ |
| IGlobalIllumination → GI_IBL, GI_RSM, GI_None | ✅ |
| IShadowSystem → ShadowSystem (CSM + Point) | ✅ |
| ToneMapPass + SkyboxPass | ✅ |
| IAntiAliasing → AA_None (AA_MSAA/AA_TAA/AA_FXAA 待实现) | ✅ |
| SceneRenderer (实体收集 → 视锥剔除 → 上传) | ✅ |
| ImGui LoadOp::Load (UI 叠加) | ✅ |

### L5 — Scene（场景层）✅
- Entity/Component/Transform/World/SceneGraph, Mesh/Light/Skybox

## 架构设计

### 渲染流程

```
ForwardPipeline::BuildFrameGraph (m_UseRenderGraph = true)
  ├── "Shadow" Pass
  │     └── ShadowSystem::Update + Render (CSM + Point)
  ├── "FullScene" Pass
  │     ├── PrepareGI (IBL / RSM)
  │     ├── BeginHDRPass → RenderScene (per-mesh set=0+set=1) → Skybox
  │     └── EndHDRPass (Barrier + SetPipeline)
  └── "ToneMap" Pass (PreBind + BeginRenderPass + Draw + EndRenderPass)

DeferredPipeline::BuildFrameGraph
  ├── "GB_Clear" Pass
  │     └── 3 MRT GBuffer (albedo+metallic / normal+roughness / emissive+ao) + D32
  │         绑定 set=0(per-frame ObjectBuffer) + set=1(per-mesh 纹理)
  ├── "Lighting" Pass
  │     └── 全屏 PBR + GBuffer 采样 + IBL + RSM + Shadow
  └── "ToneMap" Pass → BackBuffer
```

### 后处理链架构

```
Lighting/Scene 输出 (HDR)
  │
  ├─ [TAA]   ← IPostProcessPass::SetInput → Render → GetOutput=HDR_AA
  ├─ ToneMap ← IPostProcessPass::SetInput → BeginRP → Render
  ├─ [FXAA]  ← IPostProcessPass::SetInput → BeginRP(BackBuffer) → Render
  └─ Present
```

### 子系统继承树

```
IRenderSubsystem
  ├── IShadowSystem → ShadowSystem, ShadowNone
  ├── IGlobalIllumination → GI_IBL, GI_RSM, GI_None
  ├── SkyboxPass（场景 Pass）
  └── IPostProcessPass（后处理链路层）
        ├── ToneMapPass
        └── IAntiAliasing → AA_None（AA_MSAA/AA_TAA/AA_FXAA 待实现）
```

### 目录结构

```
Engine/Render/
├── Pipeline/       (IRenderPipeline, ForwardPipeline, DeferredPipeline, CameraController, Material)
├── Subsystem/      (IRenderSubsystem)
├── GI/             (IGlobalIllumination, GI_None, GI_IBL, GI_RSM)
├── Shadow/         (IShadowSystem, ShadowSystem, ShadowNone, CSMTechnique, PointShadowTechnique)
├── PostProcess/    (IPostProcessPass, ToneMapPass, SkyboxPass)
├── AntiAliasing/   (IAntiAliasing, AA_None)          ← 新增
├── SceneRenderer.h/.cpp
└── RenderGraph.h/.cpp

Engine/RHI/Vulkan/  (拆分为 5 个文件)
├── VulkanInternal.h
├── VulkanDevice.cpp
├── VulkanSwapChain.cpp
├── VulkanCommandList.cpp
├── VulkanResources.cpp
└── VulkanPipeline.cpp
```

### AA 技术分配

| AA 技术 | ForwardPipeline | DeferredPipeline | 空间 | 原因 |
|---------|:---:|:---:|------|------|
| None | ✓ | ✓ | — | 空操作 |
| MSAA | ✓ | ✗ | HDR | 延迟 GBuffer MRT 多采样代价过高 |
| TAA | ✗ | ✓ | HDR | 复用 GBuffer 深度/法线做邻域裁剪 |
| FXAA | ✓ | ✓ | LDR | 纯 LDR 后处理，无管线依赖 |

## 已知限制

| 问题 | 影响 |
|------|------|
| FullScene Pass 仍为老代码打包 | 未拆分为独立 Shadow/IBL/HDR Pass |
| 点光阴影无 PCF 软滤波 | 硬边缘锯齿 |
| 点光阴影无视锥剔除 | 6 面渲染全场景 |
| 无 Bindless 纹理 | 无法降低 draw call |
| AA 具体技术未实现 | MSAA/TAA/FXAA 仅完成接口层 |
| 后处理链无 Bloom/DOF/MotionBlur | 仅 ToneMap + Skybox |

## 待办任务

| # | 任务 | 状态 |
|---|------|:---:|
| 9 | GI_RSM (RSM 完善) | ✅ |
| 10 | DeferredPipeline (GBuffer + LightPass + Sponza) | ✅ |
| 11 | IPostProcessPass + IAntiAliasing 接口架构 | ✅ |
| — | AA_MSAA 实现 | ⬜ |
| — | AA_TAA 实现（含 TAA_Resolve 着色器） | ⬜ |
| — | AA_FXAA 实现 | ⬜ |
| — | 管线 AA 集成（ForwardPipeline + DeferredPipeline） | ⬜ |
| — | FullScene 拆分为独立 Shadow/IBL/HDR Pass | ⬜ |
| — | Bindless 纹理数组 | ⬜ |
| — | GPU Culling (Compute Shader 视锥剔除) | ⬜ |
| — | Bloom / DOF / MotionBlur 后处理 | ⬜ |
