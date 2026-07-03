# HugEngine 开发进度

> 最后更新: 2026-07-03

## 整体进度

核心渲染功能 + 子系统架构 + RenderGraph 集成 + IBL + RSM + 阴影完成。

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
- **Samples**: 02.Cube (阴影+点光+ImGui+RG切换), 03.Sponza (完整场景+阴影) ✅

## 模块完成度

### L2 — RHI（渲染硬件接口层）✅
| 特性 | 状态 |
|------|:---:|
| Vulkan 后端 (Device/SwapChain/CommandList) | ✅ |
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

### L4 — Render（渲染层）
| 子系统 | 状态 |
|--------|:---:|
| ForwardPipeline (PBR + 多光源 + 阴影 + Skybox + IBL + RSM) | ✅ |
| RenderGraph (声明式编排 + Barrier + 别名 + 裁剪) | ✅ |
| IRenderPipeline 基类 | ✅ |
| IRenderSubsystem 基类 | ✅ |
| IGlobalIllumination → GI_IBL, GI_RSM, GI_None | ✅ |
| IShadowSystem → ShadowSystem (CSM + Point) | ✅ |
| ToneMapPass + SkyboxPass | ✅ |
| SceneRenderer (实体收集 → 视锥剔除 → 上传) | ✅ |
| ImGui LoadOp::Load (UI 叠加) | ✅ |
| DeferredPipeline | ❌ |

### L5 — Scene（场景层）✅
- Entity/Component/Transform/World/SceneGraph, Mesh/Light/Skybox

## 架构设计

### 渲染流程

```
ForwardPipeline::Render (m_UseRenderGraph = true)
  └── BuildFrameGraph
        ├── "FullScene" Pass (老命令式打包)
        │     ├── ShadowSystem::Update + Render (阴影贴图)
        │     ├── PrepareGI (IBL / RSM 生成)
        │     ├── BeginHDRPass (ResizeHDRTarget + offscreen)
        │     ├── RenderScene (几何绘制)
        │     ├── RenderSkybox (天空盒)
        │     └── EndHDRPass (Barrier + SetPipeline)
        └── "ToneMap" Pass (PreBind + BeginRenderPass + Draw + EndRenderPass)
```

### 目录结构

```
Engine/Render/
├── Pipeline/     (IRenderPipeline, ForwardPipeline, Camera, CameraController, Material)
├── Subsystem/    (IRenderSubsystem)
├── GI/           (IGlobalIllumination, GI_None, GI_IBL, GI_RSM)
├── Shadow/       (IShadowSystem, ShadowSystem, ShadowNone, CSMTechnique, PointShadowTechnique)
├── PostProcess/  (ToneMapPass, SkyboxPass)
├── SceneRenderer.h/.cpp
└── RenderGraph.h/.cpp
```

## 已知限制

| 问题 | 影响 |
|------|------|
| FullScene Pass 仍为老代码打包 | 未拆分为独立 Shadow/IBL/HDR Pass |
| 点光阴影无 PCF 软滤波 | 硬边缘锯齿 |
| 点光阴影无视锥剔除 | 6 面渲染全场景 |
| 无 Bindless 纹理 | 无法降低 draw call |
| DeferredPipeline 未实现 | 仅 Forward 可用 |

## 待办任务

| # | 任务 | 状态 |
|---|------|:---:|
| 9 | GI_RSM (RSM 完善) | ✅ |
| 10 | DeferredPipeline (GBuffer + LightPass) | ⬜ |
| — | FullScene 拆分为独立 Shadow/IBL/HDR Pass | ⬜ |
| — | Bindless 纹理数组 | ⬜ |
| — | GPU Culling (Compute Shader 视锥剔除) | ⬜ |
