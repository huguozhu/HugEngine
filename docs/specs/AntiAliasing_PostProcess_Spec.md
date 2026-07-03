# 抗锯齿 & 后处理架构 Spec

> 版本: 1.0 | 日期: 2026-07-03 | 基于 HugEngine Phase 5 架构

## 1. 目标

建立统一的抗锯齿（AA）和后处理（PostProcess）子系统架构。AA 支持 MSAA/TAA/FXAA 多种技术，通过 `IAntiAliasing` 接口和 `IPostProcessPass` 链路层与管线解耦。

## 2. 已完成文件清单

| 文件 | 说明 |
|------|------|
| `Engine/Render/PostProcess/PostProcessPass.h` | IPostProcessPass 中间层接口 |
| `Engine/Render/AntiAliasing/AntiAliasing.h` | IAntiAliasing 抽象接口 + AAMode/AASettings |
| `Engine/Render/AntiAliasing/AA_None.h` | AA 空对象（Null Object） |
| `Engine/Render/PostProcess/ToneMapPass.h` | 重构为继承 IPostProcessPass |
| `Engine/Render/PostProcess/SkyboxPass.h` | 不变（场景 Pass，非后处理链） |
| `Engine/Render/CMakeLists.txt` | 添加新文件 |

## 3. 类继承树

```
IRenderSubsystem                         ← 基础生命周期
  │
  ├── IShadowSystem                      ← 阴影（抽象）
  │     ├── ShadowSystem                 ← CSM + Point Cubemap
  │     └── ShadowNone                   ← 空对象
  │
  ├── IGlobalIllumination                ← GI（抽象）
  │     ├── GI_IBL                       ← 基于图像的光照
  │     ├── GI_RSM                       ← 反射阴影贴图
  │     └── GI_None                      ← 空对象
  │
  ├── SkyboxPass                         ← 天空盒（场景 Pass）
  │
  └── IPostProcessPass                   ← 后处理链路层（抽象）
        │ SetInput(tex, sampler)
        │ GetOutputFormat()
        │ OwnsOutput() / GetOutputTexture() / GetOutputSampler()
        │
        ├── ToneMapPass                  ← HDR→LDR ACES 色调映射
        │     OwnsOutput=false
        │     GetOutputFormat() = BGRA8_UNORM
        │
        └── IAntiAliasing                ← 抗锯齿（抽象）
              │ GetMode() / AASettings
              │ RequiresMultisampling() / GetSampleCount()
              │ OverrideTextureDesc() / OverridePSODesc()
              │ SupportsForward() / SupportsDeferred()
              │ GetJitterOffset() / OnBeginFrame()
              │
              └── AA_None                ← 空对象
                  (AA_MSAA / AA_TAA / AA_FXAA 待实现)
```

## 4. AA 技术分配

| AA 技术 | ForwardPipeline | DeferredPipeline | 空间 | ToneMap 位置 | 原因 |
|---------|:---:|:---:|------|:---:|------|
| None | ✓ | ✓ | — | — | 空操作 |
| MSAA | ✓ | ✗ | HDR | 前（Resolve） | 延迟 GBuffer MRT 多采样代价过高 |
| TAA | ✗ | ✓ | HDR | 前 | 复用 GBuffer 深度/法线做邻域裁剪 |
| FXAA | ✓ | ✓ | LDR | 后 | 纯 LDR 后处理，无管线依赖 |

## 5. 后处理链编排

```
Lighting/Scene 输出 (HDR)
  │
  ├─ [TAA]        ← IsHDRSpace=true, OwnsOutput=true
  │    SetInput(HDR_Color) → Render() → GetOutputTexture()=HDR_AA
  │
  ├─ ToneMap      ← OwnsOutput=false
  │    SetInput(HDR_AA) → BeginRP(LDR_Intermediate) → Render()
  │
  ├─ [FXAA]       ← IsHDRSpace=false, OwnsOutput=false
  │    SetInput(LDR_Intermediate) → BeginRP(BackBuffer) → Render()
  │
  └─ Present
```

### RenderGraph Pass 编排（以 DeferredPipeline 为例）

```
Pass "Lighting"   [写] HDR_Color
  ↓ (if TAA)
Pass "TAA"        [读] HDR_Color  [写] HDR_AA
  ↓
Pass "ToneMap"    [读] HDR_AA     [写] LDR_Intermediate (or BackBuffer)
  ↓ (if FXAA)
Pass "FXAA"       [读] LDR_Intermediate  [写] BackBuffer
```

## 6. IAntiAliasing 接口核心方法

| 方法 | 用途 |
|------|------|
| `GetMode()` | 返回 AA 技术类型 |
| `SupportsForward()` / `SupportsDeferred()` | 管线兼容性查询 |
| `RequiresMultisampling()` / `GetSampleCount()` | MSAA 覆盖 RT 采样数 |
| `OverrideTextureDesc()` / `OverridePSODesc()` | MSAA 覆盖创建参数 |
| `GetJitterOffset()` / `OnBeginFrame()` | TAA 抖动投影 |
| `SetInput()` / `GetOutput*()` | 继承自 IPostProcessPass |

## 7. 待实现

| 任务 | 文件 |
|------|------|
| AA_MSAA | `AntiAliasing/AA_MSAA.h/.cpp` |
| AA_TAA | `AntiAliasing/AA_TAA.h/.cpp`（含 TAA_Resolve 着色器） |
| AA_FXAA | `AntiAliasing/AA_FXAA.h/.cpp`（含 FXAA 着色器） |
| 管线集成 | ForwardPipeline/DeferredPipeline 添加 AA 子系统创建 + Pass 编排 |
| RHI 扩展 | TextureDesc/PipelineStateDesc 添加 sampleCount；vkCmdResolveImage 封装 |

## 8. 设计原则

1. **策略模式**: AA 技术作为 IAntiAliasing 的可替换实现
2. **空对象模式**: AA_None 作为默认禁用状态，避免 nullptr 检查
3. **链路模式**: IPostProcessPass 统一 SetInput→Render→GetOutput 链路
4. **管线无关**: 管线通过 SupportsForward/SupportsDeferred 查询兼容性，通过 GetMode 决定 Pass 插入位置
