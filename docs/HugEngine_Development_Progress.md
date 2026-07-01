# HugEngine 开发进度

> 最后更新: 2026-07-01

## 整体进度

核心渲染功能 + IBL 环境光照 + 多线程渲染完成。架构重构为 `IRenderPipeline + IRenderSubsystem` 双层模式已规划。

- **Phase 1-4**: 引擎模块 + PBR 前向管线 + 编辑器 + 阴影 + HDR ✅
- **Phase 5**: 三缓冲帧环 + 辅助命令缓冲 + 多线程视锥剔除 + 录制 ✅
- **GI_IBL**: 环境光照（辐照度 32² + 预滤波 5-mip + BRDF LUT） ✅
- **CameraController**: 可复用相机控制模块（Free/Ground 双模式） ✅
- **RHI Compute Shader**: Dispatch + Compute PSO + .comp.slang 编译 ✅

## 模块完成度

### L0 — Core（平台层）✅
- Types/Platform/Assert/Log/Engine, Math (GLM), Containers, Threading (Taskflow)

### L1 — Reflect（反射层）✅
- ReflectionAPI/Macros/TypeInfo, TypeRegistry, Serialize (BinaryArchive)

### L2 — RHI（渲染硬件接口层）✅
| 子系统 | 状态 |
|--------|------|
| 公共接口: RHI.h, Types.h, Buffer.h, Shader.h, SwapChain.h, CommandList.h | ✅ |
| Descriptor Sets / Push Constants / PipelineBarrier | ✅ |
| 离屏渲染: BeginOffscreenPass / EndOffscreenPass | ✅ |
| Vulkan 后端: Device/Resources/CommandList | ✅ |
| 三缓冲帧环 (kMaxFramesInFlight=3) | ✅ |
| 持久映射 (VulkanBuffer Map/Unmap no-op) | ✅ |
| 辅助命令缓冲 + 多线程录制 (Phase 5-2/5-4) | ✅ |
| **Compute Shader**: Dispatch + Compute PSO + .comp.slang | ✅ |

### L3 — Shader（着色器层）✅
| 子系统 | 状态 |
|--------|------|
| Slang 编译器 + SPIR-V 嵌入 (per-shader .spv.h) | ✅ |
| PBR / Shadow / ToneMap / Skybox | ✅ |
| **IBL**: Irradiance / Prefilter (5-mip) / BRDF LUT | ✅ |
| **TestClear.comp** (Compute Shader 参考) | ✅ |

### L4 — Render（渲染层）✅
| 子系统 | 状态 |
|--------|------|
| ForwardPipeline (PBR + 多光源 + 阴影 + Skybox + IBL) | ✅ |
| Material 系统 (GPUObjectData + GPULight + GPUShadowData) | ✅ |
| Camera 系统 + CameraController | ✅ |
| HDR 离屏管线 → ToneMap(ACES) → SwapChain | ✅ |
| ImGui GI 面板 (强度滑条 0~3) | ✅ |
| IRenderSubsystem / IGlobalIllumination 基类 | ✅ |
| GI_IBL / GI_None (Null Object) | ✅ |
| RenderGraph (Phase 1 骨架) | ✅ |
| Deferred / 后处理 | ❌ |

### L5 — Scene（场景层）✅
- Entity/Component/Transform/World/SceneGraph, Mesh/Light/Skybox 组件

## 架构设计

### 渲染架构（规划）

```
IRenderPipeline (编排层)
├── IRenderSubsystem (子系统接口)
│   ├── ShadowSystem (待抽取, CSM + Point)
│   ├── IGlobalIllumination (GI 基类)
│   │   ├── GI_None (空实现) ✅
│   │   ├── GI_IBL (环境光照) ✅
│   │   └── GI_RSM (待实现)
│   └── ToneMapPass (待抽取)
└── RenderGraph (Barrier + 别名 + 调度)
```

### 目录结构

```
Engine/Render/
├── Pipeline/    (Camera, CameraController, ForwardPipeline, Material)
├── Subsystem/   (IRenderSubsystem)
├── GI/          (IGlobalIllumination, GI_None, GI_IBL, GI_RSM)
├── Shadow/      (占位 — Shadow 抽取后填充)
├── PostProcess/ (占位)
└── RenderGraph.h/.cpp
```

## 已知限制

| 问题 | 影响 | 计划 |
|------|------|------|
| Shadow 未抽取为 IRenderSubsystem | 无法复用给其他管线 | 任务 #11 |
| IRenderPipeline 基类未实现 | 无法添加 Deferred/Forward+ | 任务 #12 |
| 点光阴影无 PCF / 无视锥剔除 | 硬边 + 高 draw count | |
| 无 Bindless 纹理数组 | 无法降低 draw call | |
| 离屏视口渲染 (ImGui::Image) | 编辑器 Viewport | |
| Gizmo 操作 + 鼠标拾取 | 编辑器可用性 | |

## 待办任务

| # | 任务 | 状态 |
|---|------|:---:|
| 9 | GI_RSM（Reflective Shadow Maps） | ⬜ |
| 10 | DeferredPipeline（延迟渲染） | ⬜ |
| 11 | Shadow 子系统抽取（IRenderSubsystem） | ⬜ |
| 12 | IRenderPipeline 基类 | ⬜ |
