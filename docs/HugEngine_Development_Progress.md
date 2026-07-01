# HugEngine 开发进度

> 最后更新: 2026-07-02

## 整体进度

核心渲染功能 + 子系统架构抽取 + 多线程渲染完成。

- **Phase 1-4**: 引擎模块 + PBR 前向管线 + 编辑器 + 阴影 + HDR ✅
- **Phase 5**: 三缓冲帧环 + 辅助命令缓冲 + 多线程视锥剔除 + 录制 ✅
- **GI_IBL**: 环境光照（辐照度 32² + 预滤波 5-mip + BRDF LUT） ✅
- **CameraController**: 可复用相机控制模块（Free/Ground 双模式） ✅
- **RHI Compute Shader**: Dispatch + Compute PSO + .comp.slang 编译 ✅
- **ShadowSystem**: CSM + Point 阴影子系统，Technique 组合模式 ✅
- **IRenderPipeline**: 渲染管线抽象基类 ✅
- **ToneMapPass + SkyboxPass**: 抽取为 IRenderSubsystem ✅
- **SceneRenderer**: 通用几何体数据准备器 ✅
- **描述符集**: UPDATE_AFTER_BIND 修复（warning 180→75） ✅

## 模块完成度

### L0 — Core（平台层）✅
- Types/Platform/Assert/Log/Engine, Math (GLM), Containers, Threading (Taskflow)

### L1 — Reflect（反射层）✅
- ReflectionAPI/Macros/TypeInfo, TypeRegistry, Serialize (BinaryArchive)

### L2 — RHI（渲染硬件接口层）✅
| 子系统 | 状态 |
|--------|:---:|
| 公共接口: RHI.h, Types.h, Buffer.h, Shader.h, SwapChain.h, CommandList.h | ✅ |
| Descriptor Sets / Push Constants / PipelineBarrier | ✅ |
| 离屏渲染: BeginOffscreenPass / EndOffscreenPass | ✅ |
| Vulkan 后端: Device/Resources/CommandList | ✅ |
| 三缓冲帧环 (kMaxFramesInFlight=3) | ✅ |
| 持久映射 + 辅助命令缓冲 + 多线程录制 | ✅ |
| Compute Shader: Dispatch + Compute PSO + .comp.slang | ✅ |
| UPDATE_AFTER_BIND 描述符集更新 | ✅ |

### L3 — Shader（着色器层）✅
- Slang 编译器 + SPIR-V 嵌入, PBR / Shadow / ToneMap / Skybox / IBL

### L4 — Render（渲染层）✅
| 子系统 | 状态 |
|--------|:---:|
| ForwardPipeline (PBR + 多光源 + 阴影 + Skybox + IBL) | ✅ |
| Material 系统 (GPUObjectData + GPULight + GPUShadowData) | ✅ |
| Camera 系统 + CameraController | ✅ |
| HDR 离屏管线 → ToneMap(ACES) → SwapChain | ✅ |
| IRenderPipeline 基类 | ✅ |
| IRenderSubsystem 基类 | ✅ |

#### 子系统（IRenderSubsystem）
| 模块 | 状态 |
|------|:---:|
| IGlobalIllumination → GI_IBL, GI_None | ✅ |
| IShadowSystem → ShadowSystem (CSMTechnique + PointShadowTechnique), ShadowNone | ✅ |
| ToneMapPass (ACES ToneMap) | ✅ |
| SkyboxPass (天空盒) | ✅ |

#### 通用工具
| 模块 | 状态 |
|------|:---:|
| SceneRenderer (实体收集 → 视锥剔除 → GPU 上传) | ✅ |
| RenderGraph | 🟡 骨架 |

### L5 — Scene（场景层）✅
- Entity/Component/Transform/World/SceneGraph, Mesh/Light/Skybox 组件

## 架构设计

### 当前架构

```
IRenderPipeline (编排层)
  └── ForwardPipeline
        ├── IRenderSubsystem (子系统)
        │     ├── IShadowSystem → ShadowSystem (CSMTechnique + PointShadowTechnique)
        │     ├── IGlobalIllumination → GI_IBL
        │     ├── ToneMapPass
        │     └── SkyboxPass
        └── SceneRenderer (数据准备，非 IRenderSubsystem)

IRenderSubsystem
  ├── IGlobalIllumination
  │     ├── GI_None
  │     └── GI_IBL
  ├── IShadowSystem
  │     ├── ShadowNone
  │     └── ShadowSystem
  │           ├── CSMTechnique
  │           └── PointShadowTechnique
  ├── ToneMapPass
  └── SkyboxPass
```

### 目录结构

```
Engine/Render/
├── Pipeline/     (IRenderPipeline, ForwardPipeline, Camera, Material)
├── Subsystem/    (IRenderSubsystem)
├── GI/           (IGlobalIllumination, GI_None, GI_IBL)
├── Shadow/       (IShadowSystem, ShadowSystem, ShadowNone, IShadowTechnique,
│                  CSMTechnique, PointShadowTechnique)
├── PostProcess/  (ToneMapPass, SkyboxPass)
├── SceneRenderer.h/.cpp
└── RenderGraph.h/.cpp
```

## 已知限制

| 问题 | 影响 |
|------|------|
| 点光阴影无 PCF 软滤波 | 硬边缘锯齿 |
| 点光阴影无视锥剔除 | 6 面渲染全场景实体 |
| 无 Bindless 纹理数组 | 无法降低 draw call |
| DeferredPipeline 未实现 | 仅 Forward 可用 |
| RenderGraph 仅骨架 | 无 Barrier 自动推导 |

## 待办任务

| # | 任务 | 状态 |
|---|------|:---:|
| 9 | GI_RSM（Reflective Shadow Maps） | ⬜ |
| 10 | DeferredPipeline（延迟渲染） | ⬜ |
| 11 | Shadow 子系统抽取（IRenderSubsystem） | ✅ |
| 12 | IRenderPipeline 基类 | ✅ |
| — | IShadowSystem 抽象接口 + ShadowNone | ✅ |
| — | ToneMapPass + SkyboxPass 抽取 | ✅ |
| — | SceneRenderer 通用数据准备器 | ✅ |
| — | UPDATE_AFTER_BIND 描述符集修复 | ✅ |
