# HugEngine 混合 Ray Tracing 管线 — 完整规划

> **定位**: HybridRTPipeline 作为独立 Pipeline 类，与 DeferredPipeline 共用 GBuffer/Lighting/PostProcess 三大组件。通过 `r.Pipeline.Mode` 一键切换。
>
> **核心决策**: 提取 GBufferRenderer / LightingPass / PostProcessChain 为共享组件，避免代码重复。两个 Pipeline 各自独立、各自干净。
>
> **最后更新**: 2026-07-27

---

## 目录

1. [架构决策：独立 Pipeline + 共享组件](#1-架构决策独立-pipeline--共享组件)
2. [共享组件设计](#2-共享组件设计)
3. [P0 前置：SSBO 兼容性修复](#3-p0-前置ssbo-兼容性修复)
4. [Pass 1: RT Shadow](#4-pass-1-rt-shadow)
5. [Pass 2: RT Reflection](#5-pass-2-rt-reflection)
6. [Pass 3: RT AO](#6-pass-3-rt-ao)
7. [Pass 4: RT GI](#7-pass-4-rt-gi)
8. [降噪方案](#8-降噪方案)
9. [类层次结构](#9-类层次结构)
10. [实施时间线](#10-实施时间线)
11. [性能预算与 CVar 参考值](#11-性能预算与-cvar-参考值)
12. [风险与缓解](#12-风险与缓解)

---

## 1. 架构决策：独立 Pipeline + 共享组件

### 1.1 为什么不用"在 DeferredPipeline 内加分支"方案

如果把 RT 效果作为 if/else 嵌入 `DeferredPipeline::BuildFrameGraph`：

```
问题:
  1. BuildFrameGraph 从 700 行膨胀到 1000+
  2. 每个 Pass 位置一个 if (rtEnabled) { ... } else { ... }，条件遍布全函数
  3. Shadow/SSAO/SSR/SSGI 全有双份逻辑，改一个容易坏另一个
  4. 测试困难 — 改 Deferred 可能不小心破坏 RT 路径
  5. 将来 PathTracingPipeline 无法复用任何东西
```

### 1.2 本方案：提取共享组件，两个独立 Pipeline

```
现状 DeferredPipeline 包含:
  ├── GBuffer 5 MRT (纹理 + 渲染逻辑)
  ├── Shadow Maps (CSM + Spot)
  ├── SSAO / SSR / SSGI (屏幕空间效果)
  ├── DDGI (探针 GI)
  ├── Lighting Pass (全屏 PBR 着色器)
  └── PostProcess Chain (ToneMap → Bloom → DOF → MB → TAA → LDR)

重构后:

  GBufferRenderer         ← 独立组件（纹理所有权 + 渲染）
  LightingPass            ← 独立组件（全屏 PBR + 输入源抽象）
  PostProcessChain        ← 独立组件（后处理责任链）

       ┌──────────────────┼──────────────────┐
       ▼                  ▼                  ▼
  DeferredPipeline   HybridRTPipeline   PathTracingPipeline
  (光栅化效果)        (GBuffer + RT)      (全 PT, 远期)

  共用 GBufferRenderer / LightingPass / PostProcessChain
  各自拥有的效果模块独立管理
```

### 1.3 切换方式

```cpp
// 引擎主循环:
static int32_t cvPipelineMode = 0;  // r.Pipeline.Mode

void Engine::Tick(float dt) {
    switch (cvPipelineMode) {
        case 0: m_ActivePipeline = m_DeferredPipeline.get();  break;
        case 1: m_ActivePipeline = m_HybridRTPipeline.get();  break;
    }
    // 统一接口，调用方不感知内部实现
    m_ActivePipeline->NextFrame();
    m_ActivePipeline->Render(cmd, world, sg, camera, dt);
}
```

### 1.4 当前 RT 基础设施状态

| 模块 | 文件 | 状态 |
|------|------|:---:|
| RHI RT 接口 | `Engine/RHI/RHI/RayTracing.h` | ✅ 完整 |
| Vulkan RT 后端 | `Engine/RHI/Vulkan/VulkanRT.cpp/h` | ✅ 完整 |
| RTPass 管理器 | `Engine/Render/Pipeline/RTPass.h/cpp` | ⚠️ 独立运行（需重构为 RTManager + 各 RT Pass 类） |
| RT 着色器 | `Engine/Shader/Shaders/RT_*.slang` | ⚠️ 基础可用（缺阴影/间接光） |
| SBT 管理 | `RTPass::CreateSBT()` | ✅ 完整 |
| 材质纹理 | `RTPass::CreateMaterialTexture()` | ✅ 3×N RGBA32F |
| Bindless 纹理 | `RTPass::RegisterBindlessTexture()` | ✅ CallableKHR 绕行 |

---

## 2. 共享组件设计

### 2.1 GBufferRenderer — 纹理所有权 + 渲染

当前 DeferredPipeline 的 `m_GBufferA/B/C/D/E + m_GBufferDepth` 分散在类内部，无法被其他 Pipeline 使用。

```cpp
// 新文件: Engine/Render/Pipeline/GBufferRenderer.h
// 从 DeferredPipeline 中提取，成为独立共享组件

class GBufferRenderer {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 width, u32 height);

    // 渲染（CPU 或 GPU 模式）
    void Render(rhi::IRHICommandList* cmd, he::World& world,
                he::SceneGraph& sg, const CameraData& camera,
                GPUScene& gpuScene, MeshBatcher& batcher,
                rhi::IRHIBuffer* objectBuffer,
                const float4x4& prevViewProj,
                GBufferMode mode);

    // 导入到任意 RenderGraph（所有 Pipeline 统一调用）
    struct Handles {
        ResourceHandle albedo;    // RGBA16_FLOAT  (baseColor.rgb + metallic)
        ResourceHandle normal;    // RGBA16_FLOAT  (worldNormal.xyz + roughness)
        ResourceHandle emissive;  // RGBA16_FLOAT  (emissive.rgb + ao)
        ResourceHandle velocity;  // RG16_FLOAT    (screen-space motion vector)
        ResourceHandle worldPos;  // RGBA16_FLOAT  (worldPos.xyz)
        ResourceHandle depth;     // D32_FLOAT
    };
    Handles ImportToRenderGraph(RenderGraph& rg);

    // 原始纹理访问（用于描述符集绑定）
    rhi::IRHITexture* GetAlbedo()   const { return m_A.get(); }
    rhi::IRHITexture* GetNormal()   const { return m_B.get(); }
    rhi::IRHITexture* GetEmissive() const { return m_C.get(); }
    rhi::IRHITexture* GetVelocity() const { return m_D.get(); }
    rhi::IRHITexture* GetWorldPos() const { return m_E.get(); }
    rhi::IRHITexture* GetDepth()    const { return m_Depth.get(); }

private:
    // 5 MRT + Depth — 纹理所有权在此，不归任何 Pipeline 所有
    std::unique_ptr<rhi::IRHITexture> m_A, m_B, m_C, m_D, m_E, m_Depth;
    std::unique_ptr<IGBufferRenderer>  m_Renderer;  // CPU 或 GPU 实现
    u32 m_Width, m_Height;
};
```

### 2.2 LightingPass — 输入源抽象

当前 `DeferredLighting.frag.slang` 硬编码了 CSM、SSAO、SSR、SSGI 的 binding slot。重构为输入源可配置：

```cpp
// 新文件: Engine/Render/Pipeline/LightingPass.h

enum class LightingSource : u8 {
    None = 0,
    // 阴影
    Shadow_CSM, Shadow_RT,
    // AO
    AO_SSAO, AO_RTAO,
    // 镜面反射
    Specular_SSR, Specular_RT,
    // 间接漫反射
    Diffuse_SSGI, Diffuse_RTGI, Diffuse_DDGI,
};

struct LightingInputSources {
    LightingSource shadow   = LightingSource::Shadow_CSM;
    LightingSource ao       = LightingSource::AO_SSAO;
    LightingSource specular = LightingSource::Specular_SSR;
    LightingSource diffuse  = LightingSource::Diffuse_SSGI;
    bool  useDDGI = true;  // DDGI 可与任意 diffuse 叠加
};

class LightingPass {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();

    // 统一接口：调用方声明输入来源，内部自动绑定正确纹理并设置 push constant
    void Render(RenderGraph& rg,
                const GBufferRenderer::Handles& gb,
                const LightingInputSources& sources,
                rhi::IRHIBuffer* lightBuffer,
                rhi::IRHIBuffer* shadowBuffer,
                ClusteredShading* clusteredShading,     // 可选, Deferred 用
                // RT 纹理（仅 HybridRTPipeline 传）:
                rhi::IRHITexture* rtShadowMask = nullptr,
                rhi::IRHITexture* rtReflection = nullptr,
                rhi::IRHITexture* rtAO = nullptr,
                rhi::IRHITexture* rtGI = nullptr,
                ResourceHandle hdrOutput);

    rhi::IRHITexture* GetHDRTarget() const { return m_HDRTarget.get(); }

private:
    std::unique_ptr<rhi::IRHITexture> m_HDRTarget, m_HDRDepth;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_Layout;
    rhi::DescriptorSetHandle       m_DescSet;
};
```

shader 侧通过 push constant 选择输入源（而非大量纹理切换）：

```hlsl
// DeferredLighting.frag.slang — 统一版本

// 所有可能的输入纹理都声明（未使用的由 1×1 dummy 占位）
[[vk::binding(4)]]  Texture2D g_CSM_Shadow;      // CSM shadow maps
[[vk::binding(24)]] Texture2D g_RT_ShadowMask;   // RT shadow mask (R16_FLOAT)
[[vk::binding(20)]] Texture2D g_SSAO;            // SSAO
[[vk::binding(26)]] Texture2D g_RT_AO;           // RT AO (R8_UNORM)
[[vk::binding(21)]] Texture2D g_SSR;             // SSR reflection
[[vk::binding(25)]] Texture2D g_RT_Reflection;   // RT reflection (RGBA16_FLOAT)
[[vk::binding(19)]] Texture2D g_SSGI;            // SSGI diffuse
[[vk::binding(27)]] Texture2D g_RT_GI;           // RT GI diffuse (RGBA16_FLOAT)

// Push constant 控制输入源
struct LightingPushConstant {
    // ... 现有字段 (cameraPosition, iblIntensity, lightCount, ...)
    uint  shadowSource;    // 0=CSM, 1=RT
    uint  aoSource;        // 0=SSAO, 1=RT
    uint  specularSource;  // 0=SSR, 1=RT
    uint  diffuseSource;   // 0=SSGI, 1=RTGI
};

float3 SampleShadow(float2 uv) {
    if (pc.shadowSource == 1) return texture(g_RT_ShadowMask, uv).r;
    return SampleCSM(uv);  // 现有逻辑完全保留
}
float3 SampleAO(float2 uv) { /* 同理 */ }
float3 SampleSpecular(float2 uv) { /* 同理 */ }
float3 SampleDiffuse(float2 uv) { /* 同理 */ }
```

### 2.3 PostProcessChain — 后处理责任链

```cpp
// 新文件: Engine/Render/PostProcess/PostProcessChain.h

class PostProcessChain {
public:
    void Initialize(rhi::IRHIDevice* device, u32 w, u32 h);
    void Render(RenderGraph& rg,
                ResourceHandle hdrInput,    // HDR 输入纹理
                GBufferRenderer& gb,        // velocity, depth 等辅助输入
                ResourceHandle backBuffer); // 最终输出
    void OnResize(u32 w, u32 h);

    // 公开各环节供独立配置
    BloomPass&      GetBloom()      { return m_Bloom; }
    DOFPass&        GetDOF()        { return m_DOF; }
    MotionBlurPass& GetMotionBlur() { return m_MotionBlur; }
    ToneMapPass&    GetToneMap()    { return m_ToneMap; }
    AutoExposurePass& GetAutoExposure() { return m_AutoExposure; }
    ColorGradingPass& GetColorGrading() { return m_ColorGrading; }

private:
    BloomPass      m_Bloom;
    DOFPass        m_DOF;
    MotionBlurPass m_MotionBlur;
    ToneMapPass    m_ToneMap;
    AutoExposurePass m_AutoExposure;
    ColorGradingPass m_ColorGrading;
    // TAA / FXAA / SMAA 同样在此管理
};
```

### 2.4 DeferredPipeline 瘦身后

```cpp
class DeferredPipeline : public IRenderPipeline {
    // === 共享组件（组合，非继承） ===
    GBufferRenderer   m_GBuffer;       // 从 m_GBufferA/B/C/D/E 迁移至此
    LightingPass      m_Lighting;      // 从 DeferredLighting.frag 逻辑迁移至此
    PostProcessChain  m_PostProcess;   // 从 MakePostProcess() lambda 迁移至此

    // === DeferredPipeline 独有效果 ===
    ShadowSystem      m_Shadow;        // CSM + Spot shadow maps
    SSAO              m_SSAO;
    SSR               m_SSR;
    SSGI              m_SSGI;
    DDGI              m_DDGI;

    void BuildFrameGraph(RenderGraph& rg, ...) {
        auto gb = m_GBuffer.ImportToRenderGraph(rg);

        // 阴影（光栅化 Shadow Maps）
        rg.AddPass("Shadow", ..., [&] { m_Shadow.Render(cmd); });

        // GBuffer（光栅化）
        rg.AddPass("GB_Clear", {gb writes}, [&] { m_GBuffer.Render(cmd, ...); });

        // 屏幕空间效果（Deferred 独有）
        m_SSAO.Render(rg, gb);
        m_SSR.Render(rg, gb);
        m_SSGI.Render(rg, gb);
        m_DDGI.Update(rg, gb);

        // 光照（共享组件）
        m_Lighting.Render(rg, gb, {
            .shadow = LightingSource::Shadow_CSM,
            .ao = LightingSource::AO_SSAO,
            .specular = LightingSource::Specular_SSR,
            .diffuse = LightingSource::Diffuse_SSGI,
        }, ...);

        // 后处理（共享组件）
        m_PostProcess.Render(rg, m_Lighting.GetHDRTarget(), gb, backBuf);
    }
};
```

### 2.5 HybridRTPipeline — 新类

```cpp
class HybridRTPipeline : public IRenderPipeline {
    // === 共享组件 — 和 DeferredPipeline 完全相同的类型! ===
    GBufferRenderer   m_GBuffer;
    LightingPass      m_Lighting;
    PostProcessChain  m_PostProcess;

    // === RT 基础设施 ===
    RTManager         m_RTManager;      // AS + SBT

    // === RT 效果（替代 Deferred 的屏幕空间效果） ===
    RTShadowPass      m_RTShadow;
    RTReflectionPass  m_RTReflection;
    RTAOPass          m_RTAO;
    RTGIPass          m_RTGI;

    // DDGI 保留（远距离低频 GI 仍由探针覆盖）
    DDGI              m_DDGI;

    void BuildFrameGraph(RenderGraph& rg, ...) {
        auto gb = m_GBuffer.ImportToRenderGraph(rg);

        // TLAS/BLAS（RT 管线必须）
        m_RTManager.BuildAS(rg);

        // GBuffer（光栅化 — 和 DeferredPipeline 完全一样的调用）
        rg.AddPass("GB_Clear", {gb writes}, [&] { m_GBuffer.Render(cmd, ...); });

        // RT 效果（全新 Pass，替代屏幕空间效果）
        m_RTShadow.Render(rg, gb, m_RTManager.GetTLAS());
        m_RTReflection.Render(rg, gb, m_RTManager.GetTLAS());
        m_RTAO.Render(rg, gb, m_RTManager.GetTLAS());
        m_RTGI.Render(rg, gb, m_RTManager.GetTLAS());
        m_DDGI.Update(rg, gb);  // 远距离 GI

        // 光照（共享组件 — 同样调用，不同输入源）
        m_Lighting.Render(rg, gb, {
            .shadow   = LightingSource::Shadow_RT,
            .ao       = LightingSource::AO_RTAO,
            .specular = LightingSource::Specular_RT,
            .diffuse  = LightingSource::Diffuse_RTGI,
        }, rtShadowMask, rtReflection, rtAO, rtGI);

        // 后处理（共享组件 — 完全相同的调用）
        m_PostProcess.Render(rg, m_Lighting.GetHDRTarget(), gb, backBuf);
    }
};
```

### 2.6 两个 Pipeline 的 BuildFrameGraph 对比

```
DeferredPipeline                    HybridRTPipeline
─────────────────────────           ─────────────────────────
GPU_Cull (AsyncCompute)            GPU_Cull (AsyncCompute)
  ↓                                  ↓
Shadow Maps (CSM)                  BLAS_Update (仅脏几何)
  ↓                                  ↓
GB_Clear (光栅化)                  TLAS_Build (每帧)
  ↓                                  ↓
DDGI_Update (AsyncCompute)         GB_Clear (光栅化) ← 同一行代码
  ↓                                  ↓
SSAO                               RT_Shadow ← 新 Pass
  ↓                                  ↓
SSR → SSR_Denoise                  DDGI_Update (AsyncCompute)
  ↓                                  ↓
SSGI → SSGI_Denoise                RT_AO ← 新 Pass
  ↓                                  ↓
Lighting (CSM+SSAO+SSR+SSGI)      RT_Reflection → Denoise ← 新 Pass
  ↓                                  ↓
PostProcess (Bloom/DOF/MB/TAA/...) RT_GI → Denoise ← 新 Pass
  ↓                                  ↓
Present                            Lighting (RT+DDGI) ← 同一行代码，不同参数
                                     ↓
                                   PostProcess ← 同一行代码
                                     ↓
                                   Present

各自独立，无 if/else 交织
```

---

## 3. P0 前置：SSBO 兼容性修复

### 3.1 问题现状

```
StorageBuffer (StructuredBuffer/ByteAddressBuffer) 在 ClosestHitKHR 中:
  - slangc (all versions tested): GPU fault → 黑屏
  - glslangValidator: GPU fault → 黑屏
  - 但 CallableKHR 中完全正常
```

### 3.2 影响范围

| 数据 | 当前存放 | 在 ClosestHit 中需要? | 修复前替代方案 |
|------|----------|:---:|------|
| GPUObjectData (worldMatrix/materialID) | SSBO | ✅ 是 | Texture2D (3×N) — 已实现 |
| 顶点位置/法线/切线/UV | 顶点缓冲(VB/IB) | ✅ 是 | 无（当前使用 Triplanar + -WorldRayDirection 近似） |
| 光源数据 | CB (Uniform) | ✅ 是 | Uniform Buffer — 已实现 (但受 64KB 限制) |
| 阴影数据 (VP 矩阵) | SSBO | ❌ RayGen 中可用 | RayGen 中直接访问 |
| 场景 Mesh 列表 | SSBO | ❌ RayGen 中可用 | RayGen 中直接访问 |

### 3.3 修复方案

#### 方案 1: 升级 slangc + 使用正确 SPIR-V 标志（推荐优先尝试）

```
目标: 确认问题是否已在新版 slangc 中修复
方法:
  1. 升级 slangc 到最新 master 分支
  2. 尝试新的编译标志:
     -fvk-use-storage-buffer-class
     -fvk-use-entry-point-interface
     -emit-spirv-via-glsl (在 ClosestHit 中)
  3. 在多个 GPU 上测试 (NVIDIA/AMD/Intel)
```

#### 方案 2: VK_KHR_ray_tracing_position_fetch（已加载扩展）

```
收益: 直接在 ClosestHit 中获取顶点位置，无需 SSBO
状态: VK_KHR_ray_tracing_position_fetch 已在 VulkanDevice.cpp 中启用
用法:
  [shader("closesthit")]
  void main(inout Payload p) {
      float3 v0 = HitTriangleVertexPosition(0);
      float3 v1 = HitTriangleVertexPosition(1);
      float3 v2 = HitTriangleVertexPosition(2);
      float3 bary = HitTriangleBarycentrics();
      float3 position = v0 * bary.x + v1 * bary.y + v2 * bary.z;
      float3 normal = normalize(cross(v1 - v0, v2 - v0));
  }
```

#### 方案 3: 大号 Uniform Buffer（短期限 workaround）

```
限制: VkPhysicalDeviceLimits::maxUniformBufferRange (通常 64KB)
     64KB / sizeof(GPUObjectData) ≈ 256 个物体
     对于 RT Shadow/Reflection（仅需判断遮挡），不需要所有物体数据
```

#### 方案 4: Texture2D 化所有场景数据

```
将 GPUObjectData 用 Texture2D::Load() 读取:
  objectDataTex: 4×N RGBA32F (每物体 4 行 × 4 组件 = 16 floats)
顶点数据同理: vertexTex: 3×M RGBA32F
优点: 绕过 SSBO 兼容性问题
缺点: 需要每帧上传大量纹理数据 (但对数万顶点场景可接受)
```

### 3.4 推荐执行顺序

```
W1:  尝试方案 1 (slangc 升级)
W1:  并行: 方案 2 接入 (position_fetch 已在引擎中启用，仅需 shader 侧实现)
     ↓ 如果 W1 结束时方案 1 和方案 2 都不可行
W2:  实施方案 4 (Texture2D 化) — 作为最终 fallback
同时: 在多 GPU 上交叉验证以确定根因
```

---

## 4. Pass 1: RT Shadow

### 4.1 目标

用硬件 RT 阴影替代 CSM + Spot Shadow Maps。RT 阴影提供：
- 无级联过渡伪影
- 无 shadow acne / peter panning
- 天然支持软阴影（面光源扩展）
- 点光源/聚光灯同样适用

### 4.2 着色器设计

```hlsl
// RT_Shadow.rgen.slang
struct ShadowPushConstant {
    float4x4 viewProj;
    float3   cameraPos;
    uint     lightCount;
    uint     shadowFlags;   // bit0=硬阴影, bit1=半分辨率, bit2=透明阴影(未实现)
    uint     sampleIdx;     // 时间抖动索引
};
[[vk::push_constant]] ShadowPushConstant g_PC;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
[[vk::binding(1, 0)]] RWTexture2D<float> g_ShadowMask;

// GBuffer 输入 (从 RenderGraph 导入)
[[vk::binding(2, 0)]] Texture2D<float>  g_Depth;
[[vk::binding(3, 0)]] Texture2D<float4> g_GBufferB; // normal.xyz + roughness

struct ShadowLight {
    float4 pos_type;      // xyz=位置(方向光时是方向), w=类型(0=方向,1=点,2=聚光)
    float4 color_radius;  // rgb=颜色, w=光源半径(软阴影用)
    float4 spotDir_angle; // xyz=聚光方向, w=cos(内角)
};
[[vk::binding(4, 0)]] cbuffer ShadowLights {
    ShadowLight g_Lights[16];
    uint g_LightCount;
};

[shader("raygeneration")]
void main() {
    uint2 idx = DispatchRaysIndex().xy;

    float depth = g_Depth.Load(int3(idx, 0));
    float3 worldPos = ReconstructWorldPos(idx, depth, g_PC.viewProj);
    float3 N = g_GBufferB.Load(int3(idx, 0)).xyz * 2.0 - 1.0;

    float shadow = 1.0;

    for (uint li = 0; li < g_LightCount; li++) {
        ShadowLight light = g_Lights[li];

        float3 L; float tMax;
        if (light.pos_type.w == 0.0) { L = light.pos_type.xyz; tMax = 10000.0; }
        else { float3 lp = light.pos_type.xyz; L = lp - worldPos;
               tMax = length(L); L /= tMax; }

        RayDesc ray;
        ray.Origin = worldPos + N * 0.001;
        ray.Direction = L;
        ray.TMin = 0.01;
        ray.TMax = tMax;

        ShadowPayload payload; payload.blocked = false;
        TraceRay(g_TLAS,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
            0xFF, 0, 0, 0, ray, payload);
        if (payload.blocked) shadow *= 0.0;
    }

    g_ShadowMask[idx] = shadow;
}
```

### 4.3 Miss Shader

```hlsl
// RT_Shadow.rmiss.slang
struct ShadowPayload { bool blocked; };
[shader("miss")]
void main(inout ShadowPayload payload) { payload.blocked = false; }
```

### 4.4 软阴影扩展

```
面光源随机采样: 每个光源取 N 个在光源球面/圆盘上的采样点
SPP=1 (单采样+时域累积) 可接受
SPP=4 可获得高质量软阴影（使用 NRD SIGMA 降噪）
```

### 4.5 在 HybridRTPipeline::BuildFrameGraph 中的位置

```cpp
// GB_Clear 之后:
auto rtShadowOut = rg.ImportTexture("RT_Shadow", m_RTShadow.GetShadowMask());
rg.AddPass("RT_Shadow",
    {{gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
    {{rtShadowOut, ResourceAccess::Write}},
    [&, w, h](rhi::IRHICommandList* c) {
        u32 rw = m_RTShadowHalfRes ? w/2 : w;
        u32 rh = m_RTShadowHalfRes ? h/2 : h;
        m_RTShadow.Execute(c, rw, rh);
    });
```

---

## 5. Pass 2: RT Reflection

### 5.1 目标

用硬件 RT 反射替代 SSR（屏幕空间反射）。核心优势：
- 可反射屏幕外物体（SSR 的最大限制）
- 可处理多 bounce 反射
- 与粗糙度关联的 LOD

### 5.2 着色器设计

```hlsl
// RT_Reflection.rgen.slang
[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_Reflection;  // RGB=颜色, A=命中距离
[[vk::binding(2, 0)]] Texture2D<float>  g_Depth;
[[vk::binding(3, 0)]] Texture2D<float4> g_GBufferA;       // albedo + metallic
[[vk::binding(4, 0)]] Texture2D<float4> g_GBufferB;       // normal + roughness
[[vk::binding(5, 0)]] Texture2D<float4> g_GBufferE;       // worldPos
[[vk::binding(6, 0)]] TextureCube<float4> g_PrefilterEnv; // IBL 回退
[[vk::binding(7, 0)]] SamplerState g_Sampler;

struct ReflectionPayload { float3 color; float hitT; float roughness; };

[shader("raygeneration")]
void main() {
    uint2 idx = DispatchRaysIndex().xy;
    float roughness = g_GBufferB.Load(int3(idx, 0)).w;

    if (roughness > g_ReflectionRoughnessMax) {
        g_Reflection[idx] = float4(0, 0, 0, -1); return;
    }

    float3 N = g_GBufferB.Load(int3(idx, 0)).xyz * 2.0 - 1.0;
    float3 worldPos = g_GBufferE.Load(int3(idx, 0)).xyz;
    float3 V = normalize(g_CameraPos - worldPos);

    float3 rayDir = roughness < 0.01
        ? reflect(-V, N)
        : SampleGGX_VNDF(V, N, roughness, BlueNoise2D(idx, g_FrameIndex));

    RayDesc ray;
    ray.Origin = worldPos + N * 0.01;
    ray.Direction = rayDir;
    ray.TMin = 0.1;
    ray.TMax = 500.0;

    ReflectionPayload payload;
    payload.color = float3(0, 0, 0); payload.hitT = -1;
    payload.roughness = roughness;

    TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    g_Reflection[idx] = float4(payload.color, payload.hitT);
}
```

### 5.3 分辨率策略

```
粗糙度 < 0.2:  全分辨率 (镜面反射需要高精度)
粗糙度 0.2-0.4: 半分辨率
粗糙度 0.4-0.6: 四分之一分辨率
粗糙度 > 0.6:   不发射 RT 光线 (直接用 IBL prefilter)
```

### 5.4 Miss 回退

```hlsl
// RT_Reflection.rmiss.slang
[shader("miss")]
void main(inout ReflectionPayload payload) {
    // 回退到 IBL 预滤波环境贴图
    float mipLevel = payload.roughness * MAX_REFLECTION_LOD;
    float3 R = reflect(-WorldRayDirection(), ...);
    payload.color = g_PrefilterEnv.SampleLevel(g_Sampler, R, mipLevel).rgb;
    payload.hitT = -1;
}
```

---

## 6. Pass 3: RT AO

### 6.1 目标

用硬件 RT 环境光遮蔽替代 SSAO。优势：不限于屏幕空间，更物理准确。

### 6.2 配置

| 参数 | 值 | 说明 |
|------|----|------|
| 分辨率 | 半分辨率 | AO 是低频效果 |
| 光线数 | 2-4 条 | 半球余弦分布 |
| 最大距离 | 2.0m | 控制遮蔽半径 |
| Ray Flags | `ACCEPT_FIRST_HIT_AND_END_SEARCH` + `SKIP_CLOSEST_HIT` | 仅需命中测试 |
| 输出格式 | R8_UNORM | 单通道遮蔽值 |

### 6.3 与 LightingPass 的交互

RT_AO 输出绑定到 `g_RT_AO` binding slot (26)。LightingPass 通过 `aoSource=RT` 选择从该 slot 读取。与 DeferredPipeline 的 SSAO 走相同的数据路径，LightingPass shader 不变。

---

## 7. Pass 4: RT GI

### 7.1 目标

用硬件 RT 实现间接漫反射，补充 DDGI 探针系统（不是替代）。

### 7.2 三层 GI 覆盖

```
DDGI:   全局范围，低频，探针网格 (64×) → 每帧 2ms
SSGI:   屏幕空间，中频，Ray Marching → 每帧 1ms  (仅 DeferredPipeline)
RT GI:  屏幕空间 + 屏幕外，高频 → 每帧 3-5ms    (仅 HybridRTPipeline)

优先级（距离升序）: SSGI → RT GI → DDGI
  - SSGI 处理最近（< 5m）的屏幕空间间接光
  - RT GI 处理中距离（5-30m）的精确 RT 间接光
  - DDGI 处理远距离（> 30m）的低频环境光

全部可通过 CVar 独立开关：r.SSGI.Enable, r.RT.GI, r.DDGI.Enable
```

### 7.3 配置

| 参数 | 值 | 说明 |
|------|----|------|
| 分辨率 | 四分之一 (~480×270@1080p) | GI 是极低频信号 |
| SPP | 1-2 | 依赖时域累积 |
| 反弹次数 | 1 | 间接漫反射（更高 bounce 由 DDGI 覆盖） |
| 追踪范围 | 30m | 超出范围回退到 DDGI 探针查询 |
| 输出格式 | RGBA16_FLOAT | HDR 间接漫反射颜色 |

### 7.4 着色器关键逻辑

```hlsl
// RT_GI.rgen.slang
[shader("raygeneration")]
void main() {
    // ... 读取 GBuffer worldPos + normal ...

    float3 sampleDir = SampleCosineHemisphere(N, Hash2D(idx, g_FrameIndex));

    RayDesc ray;
    ray.Origin = worldPos + N * 0.01;
    ray.Direction = sampleDir;
    ray.TMin = 0.1;
    ray.TMax = g_MaxTraceDistance;  // 30m

    GIPayload payload; payload.radiance = float3(0, 0, 0); payload.hitT = -1;
    TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    // w=0 表示 miss，LightingPass 中回退到 DDGI 探针查询
    g_GIIndirectDiffuse[idx] = float4(payload.radiance, payload.hitT > 0 ? 1 : 0);
}
```

---

## 8. 降噪方案

### 8.1 分阶段降噪策略

**Phase A: 时域累积（2w）— 所有 RT Pass 共用**

```
核心: Temporal Reprojection
  1. 读取 Motion Vector (GBuffer velocity buffer)
  2. historyUV = currentUV - motionVector
  3. 验证: |depth - historyDepth| < threshold → accept
           dot(normal, historyNormal) > 0.9 → accept
  4. 混合: result = lerp(history, current, alpha)
     alpha 动态调整: scenes static → 0.05, scenes dynamic → 0.5
```

**Phase B: 空间滤波（1w）— RT GI 专用**

```
核心: Edge-Aware Spatial Filter (5×5)
  w_d = exp(-|depth - center| / sigma_d)
  w_n = max(0, dot(normal, centerNormal)^power)
  w_c = exp(-luminance_diff / sigma_c)
  filterColor = sum(w_d*w_n*w_c * color) / sum(weights)
```

**Phase C: NRD 集成（3w）— 高质量降噪（远期）**

```
NRD 组件匹配:
  RT Shadow     → SIGMA Shadow Denoiser
  RT Reflection → ReBLUR Specular
  RT GI         → ReBLUR Diffuse
  RT AO         → 轻量空间滤波即可

由 r.NRD.Enable CVar 控制（默认自研方案）
```

### 8.2 降噪在 BuildFrameGraph 中的位置

```
RT_Reflection Pass → RT_Refl_Temporal → RT_Refl_Spatial → ReflectionDenoised
RT_GI Pass         → RT_GI_Temporal    → RT_GI_Spatial    → GIDenoised
RT_AO Pass         → RT_AO_Temporal                       → AODenoised

GBuffer velocity 必须在所有 RT Pass 之前写入完成
  → 确保 GB_Clear 在 RT passes 之前
```

---

## 9. 类层次结构

### 9.1 完整类图

```
IRenderPipeline
  ├── ForwardPipeline
  ├── DeferredPipeline        ← 瘦身后（仅持有独有效果 + 共享组件引用）
  └── HybridRTPipeline        ← 新（与 Deferred 平行，共享三大组件）

共享组件（组合关系，非继承）:
  GBufferRenderer             ← 纹理所有权 + CPU/GPU 渲染
  LightingPass                ← 全屏 PBR + 输入源抽象
  PostProcessChain            ← Bloom/DOF/MotionBlur/ToneMap/TAA/ColorGrading
  
RT 基础设施:
  RTManager                   ← AS(BLAS/TLAS) + RT PSO + SBT（场景级单例）
  RTShadowPass                ← 使用 RTManager 的 AS
  RTReflectionPass            ← 使用 RTManager 的 AS
  RTAOPass                    ← 使用 RTManager 的 AS
  RTGIPass                    ← 使用 RTManager 的 AS
```

### 9.2 组件所有权

```
Engine (顶层)
  │
  ├── GBufferRenderer* ─────────── 共享（1 个实例）
  ├── LightingPass* ────────────── 共享（1 个实例）
  ├── PostProcessChain* ────────── 共享（1 个实例）
  ├── RTManager* ───────────────── 共享（HybridRTPipeline + 远期 PTPipeline 共用）
  │
  ├── DeferredPipeline (owning):
  │     ShadowSystem, SSAO, SSR, SSGI, DDGI
  │     ├── 使用 m_GBuffer 引用
  │     ├── 使用 m_Lighting 引用
  │     └── 使用 m_PostProcess 引用
  │
  └── HybridRTPipeline (owning):
        RTShadowPass, RTReflectionPass, RTAOPass, RTGIPass, DDGI
        ├── 使用 m_GBuffer 引用
        ├── 使用 m_Lighting 引用
        ├── 使用 m_RTManager 引用
        └── 使用 m_PostProcess 引用
```

---

## 10. 实施时间线

```
Phase RT-1: 混合 RT 管线 — 总计 14 周

Week 1-2:  SSBO 兼容性修复
  ├── 并行: slangc 升级测试 (NVIDIA/AMD/Intel)
  ├── 并行: VK_KHR_ray_tracing_position_fetch 接入
  ├── 保底: Texture2D 化场景数据方案
  └── 在多 GPU 上交叉验证以确定根因

Week 3-4:  提取共享组件 ← 新增步骤
  ├── 从 DeferredPipeline 提取 GBufferRenderer（纹理所有权迁移）
  ├── 从 DeferredPipeline 提取 LightingPass（输入源抽象）
  ├── 从 DeferredPipeline 提取 PostProcessChain（后处理责任链）
  └── 验证: DeferredPipeline 功能完整无回归（光栅化 A/B 对比）

Week 5-6:  RTManager + HybridRTPipeline 骨架
  ├── RTPass 拆分为 RTManager (AS+SBT) + 各 RT Pass 类
  ├── HybridRTPipeline 类创建（空 BuildFrameGraph）
  ├── RenderGraph 集成: BLAS Update + TLAS Build Pass
  └── 验证: 空 HybridRTPipeline 输出全屏清除色 → r.Pipeline.Mode 可切换

Week 7-8:  RT Shadow Pass
  ├── RT_Shadow.rgen/rmiss 着色器
  ├── 方向光 + 点光源阴影
  ├── 集成到 HybridRTPipeline::BuildFrameGraph
  └── 验证: 与 DeferredPipeline CSM 阴影 A/B 对比

Week 9-10: RT Reflection Pass
  ├── RT_Reflection.rgen/rchit/rmiss 着色器
  ├── GGX 重要性采样 + IBL miss 回退
  ├── 粗糙度分级分辨率
  └── 验证: 与 DeferredPipeline SSR 反射 A/B 对比

Week 11:   RT AO + RT GI Pass
  ├── RT_AO.rgen (简单, 1 天)
  ├── RT_GI.rgen/rchit/rmiss (2-3 天)
  ├── DDGI 保留配置
  └── 验证: AO 与 SSAO 对比，GI 与 SSGI+DDGI 对比

Week 12-13: 降噪 Pass
  ├── 时域累积（所有 RT Pass 共用）
  ├── 空间滤波（RT GI + RT Reflection）
  └── 集成到 BuildFrameGraph

Week 14:   集成测试 + 文档
  ├── r.Pipeline.Mode 0/1 切换全测试
  ├── Sponza 场景: RT Shadow + Reflection + AO + GI 全开性能基准
  ├── 光栅化 vs RT 视觉对比 (PSNR)
  └── 开发者文档
```

---

## 11. 性能预算与 CVar 参考值

### 11.1 目标性能 (RTX 4070 @ 1440p, 60fps)

| Pass | 预算 | 分辨率 | SPP | 说明 |
|------|:---:|------|:---:|------|
| BLAS Update | 0.2ms | N/A | N/A | 仅几何变更时，per mesh |
| TLAS Build | 0.5ms | N/A | N/A | 每帧 (数千实例) |
| RT Shadow | 1.5ms | 全分辨率 | 1 | 硬阴影 |
| RT Reflection | 2.0ms | 半分辨率 | 1 | roughness < 0.4 才发射 |
| RT AO | 1.0ms | 半分辨率 | 2 | 半球采样 |
| RT GI | 2.0ms | 四分之一 | 1 | 1 bounce |
| RT Denoise | 1.5ms | N/A | N/A | 时域+空间 |
| **RT 总计** | **8.7ms** | | | 在 16.6ms 帧预算内 |

### 11.2 低端设备适配 (GTX 1060 @ 1080p, 30fps)

| Pass | 预算 |
|------|:---:|
| RT Shadow | 3.0ms (半分辨率) |
| RT Reflection | 不启用 (回退到 DeferredPipeline SSR) |
| RT AO | 2.0ms (四分之一分辨率) |
| RT GI | 不启用 (回退到 DeferredPipeline DDGI+SSGI) |
| **RT 总计** | **5.0ms** (在 33.3ms 帧预算内) |

### 11.3 CVar 参考

```cpp
// 管线选择
r.Pipeline.Mode          0   // 0=Deferred, 1=HybridRT

// RT 质量（HybridRTPipeline 内）
r.RT.Shadow              1   // RT 阴影开关
r.RT.Shadow.HalfRes      0   // 半分辨率阴影
r.RT.Shadow.Soft         0   // 软阴影（面光源采样）
r.RT.Reflection          1   // RT 反射开关
r.RT.Reflection.HalfRes  1   // 半分辨率反射
r.RT.Reflection.SPP      1   // 反射采样数
r.RT.AO                  1   // RT AO 开关
r.RT.AO.HalfRes          1
r.RT.AO.SPP              2
r.RT.GI                  1   // RT GI 开关
r.RT.GI.QuarterRes       1   // 四分之一分辨率
r.RT.GI.MaxDistance      30  // 追踪最大距离(m)
r.RT.GI.SPP              1

// 降噪
r.RT.Denoise.Temporal    1   // 时域累积
r.RT.Denoise.Spatial     1   // 空间滤波
```

---

## 12. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| SSBO 兼容性无法修复 | RT Shadow/GI 需要 SSBO 传光源和场景数据 → 不可用 | Texture2D 化 + Uniform Buffer。30 个光源以内不成问题 |
| RT 性能不达标 | 全部效果开启超过 16.6ms | 逐效果分辨率缩放 + 自适应 SPP + DLSS/FSR 降低渲染分辨率 |
| 提取共享组件引入回归 | DeferredPipeline 功能损坏 | 提取时保留 DeferredPipeline 的 BuildFrameGraph 无需改动，仅改成员所有权，通过 screenshot 对比 CI guard |
| 光栅化 vs RT 视觉差异 | 切换时 pop | 共用 `pbr_common.slang` BRDF，CI 中 PSNR 对比 |
| slangc RT 着色器兼容性 | ClosestHit 中某些特性不可用 | 准备 glslangValidator 回退路径，复杂逻辑放 RayGen |

---

## 附录 A: 与 Full Path Tracing 规划的边界

本规划覆盖 **Level 1: 混合 RT**。以下内容属于 [全路径追踪管线规划](全路径追踪管线规划.md)：

- Path Tracing 参考模式 (替代 GBuffer 光栅化)
- ReSTIR DI / ReSTIR GI / ReSTIR PT
- SER (Shader Execution Reordering)
- OMM (Opacity Micromaps)
- Neural Radiance Cache (NRC)
- DLSS Ray Reconstruction
- 完整 glTF PBR 材质扩展 (transmission/volume/clearcoat/sheen/anisotropy/iridescence)

这些不在此文档范围内，它们要求 Level 1 的混合管线已经验证稳定，且架构足以承载更激进的 RT 管线。

---

## 附录 B: 关键文件变更清单

| 文件 | 变更 | 说明 |
|------|:---:|------|
| **共享组件（提取自 DeferredPipeline）** | | |
| `Engine/Render/Pipeline/GBufferRenderer.h/cpp` | 重构 | 纹理所有权从 DeferredPipeline 迁移至此 |
| `Engine/Render/Pipeline/LightingPass.h/cpp` | 重构 | 全屏 PBR + LightSource 枚举 + 输入源抽象 |
| `Engine/Render/PostProcess/PostProcessChain.h/cpp` | 新建 | 后处理责任链封装 |
| **RT 基础设施** | | |
| `Engine/Render/RT/RTManager.h/cpp` | 重构 | 从 RTPass 拆分，管理 AS + RT PSO + SBT |
| `Engine/Render/RT/RTShadowPass.h/cpp` | 新建 | RT 阴影 Pass |
| `Engine/Render/RT/RTReflectionPass.h/cpp` | 新建 | RT 反射 Pass |
| `Engine/Render/RT/RTAOPass.h/cpp` | 新建 | RT AO Pass |
| `Engine/Render/RT/RTGIPass.h/cpp` | 新建 | RT GI Pass |
| **着色器** | | |
| `Engine/Shader/Shaders/RT_Shadow.rgen.slang` | 新建 | 阴影 RayGen |
| `Engine/Shader/Shaders/RT_Shadow.rmiss.slang` | 新建 | 阴影 Miss |
| `Engine/Shader/Shaders/RT_Reflection.rgen.slang` | 新建 | 反射 RayGen |
| `Engine/Shader/Shaders/RT_Reflection.rchit.slang` | 新建 | 反射 ClosestHit |
| `Engine/Shader/Shaders/RT_Reflection.rmiss.slang` | 新建 | 反射 Miss |
| `Engine/Shader/Shaders/RT_GI.rgen.slang` | 新建 | GI RayGen |
| `Engine/Shader/Shaders/RT_GI.rchit.slang` | 新建 | GI ClosestHit |
| `Engine/Shader/Shaders/RT_AO.rgen.slang` | 新建 | AO RayGen |
| `Engine/Shader/Shaders/pbr_common.slang` | 重构 | 光栅化 + RT 共用 BRDF |
| `Engine/Shader/Shaders/DeferredLighting.frag.slang` | 修改 | 新增 RT 纹理绑定 + inputSource 分支 |
| `Engine/Shader/Shaders/ShaderTypes.slang` | 修改 | 新增 RT 相关 Push Constant 结构体 |
| **管线类** | | |
| `Engine/Render/Pipeline/HybridRTPipeline.h/cpp` | **新建** | 混合 RT 管线类 |
| `Engine/Render/Pipeline/DeferredPipeline.h` | 修改 | 瘦身：移除 GBuffer/Lighting/PostProcess 成员，改为组件引用 |
| `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp` | 修改 | 简化为纯光栅化路径（移除 RT 条件分支） |

---

> **文档版本**: v2.1
> **创建日期**: 2026-07-27
> **更新**: 2026-07-28 — 实施进度更新：Phase 1-4.1 完成，HybridRTPipeline 已知问题记录
> **工期**: 14 周（含 2 周共享组件提取）
> **前置依赖**: 现有 DeferredPipeline / RenderGraph / RHI RT 接口全部就绪

---

## 附录 C: 实施进度（2026-07-28）

### 已完成

| 阶段 | 状态 | 提交 | 内容 |
|------|:---:|------|------|
| Phase 1 | ✅ | `a4e5e89` | SSBO 兼容性修复：RT_Shadow.rgen 重写 + position_fetch POC |
| Phase 2-1 | ✅ | `72a7680` | GBufferRenderer 提取（纹理所有权迁移）+ ForwardPipeline RT 清理 |
| Phase 2-2 | ✅ | `47296a8` | LightingPass 提取（HDR 目标 + Lighting PSO + 输入源抽象） |
| Phase 2-3 | ✅ | `c36e830` | PostProcessChain 提取（Bloom/DOF/MotionBlur/TAA/ToneMap/AA + LDR） |
| Phase 3 | ✅ | `54b6200` | HybridRTPipeline 骨架：GBuffer + DDGI + Lighting + 后处理 |
| Phase 4-1 | ✅ | `bc91488` | RTShadowPass 集成：阴影遮罩纹理 + 描述符集 + Execute |
| 02.Cube | ✅ | `94ef8ff` `d08237c` | 添加 HybridRTPipeline 支持（renderMode=2），移除独立 RTPass 原型 |
| Phase 4-2 (P2) | ✅ | 工作区 | RTReflectionPass：RT_Reflection.rgen/rchit/rmiss + GGX 反射 + 半分辨率 |
| Phase 4-3 (P3) | ✅ | 工作区 | RTAOPass：RT_AO.rgen + 余弦半球采样 + 半分辨率 R8 遮罩 |
| Phase 4-4 (P4) | ✅ | 工作区 | RTGIPass：RT_GI.rgen/rchit/rmiss + 一次反弹间接漫反射 + 四分之一分辨率 |

**当前已知限制（2026-08-01，不影响正确性）**：
- GTX 1070 等 Pascal 设备不支持 `VK_KHR_ray_tracing_position_fetch` → ClosestHit 用「材质纹理 + 三角形法线纹理」查询几何（而非 position_fetch / SSBO，SSBO 在 ClosestHit 中已知 slangc GPU fault）。
- RenderGraph 对导入纹理逐帧重置为 Undefined，深度等跨帧状态可能产生 `VUID-oldLayout-01197` 验证警告（cosmetic，访问掩码同步仍正确）。

### 关键文件变更（实际）

| 文件 | 操作 | 说明 |
|------|:---:|------|
| `Engine/Render/Pipeline/GBufferRenderer.h` | 重写 | 新增 `GBufferRenderer` 拥有类 + `Handles` + `ImportToRenderGraph` |
| `Engine/Render/Pipeline/GBufferRenderer.cpp` | 新建 | 纹理创建 + PSO + 描述符集实现 |
| `Engine/Render/Pipeline/LightingPass.h/cpp` | 新建 | HDR 目标 + Lighting PSO + `LightingSource` 枚举 |
| `Engine/Render/PostProcess/PostProcessChain.h/cpp` | 新建 | 后处理 Pass 所有权 + LDR 纹理管理 |
| `Engine/Render/Pipeline/HybridRTPipeline.h/cpp` | 新建 | 混合 RT 管线完整实现 |
| `Engine/Render/RT/RTShadowPass.h/cpp` | 新建 | RT 阴影 Pass |
| `Engine/Render/Pipeline/DeferredPipeline.h/cpp` | 修改 | 移除 ~38 个成员变量，委托给共享组件 |
| `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp` | 修改 | ImportToRenderGraph + getter 替换 |
| `Engine/Render/Pipeline/ForwardPipeline.h/cpp` | 修改 | 移除 RTPass 死代码 (~70 行) |
| `Samples/02.Cube/02.Cube.cpp` | 修改 | HybridRTPipeline + 简化渲染模式 |

### 已知问题

**HybridRTPipeline 运行时崩溃**（2026-07-28，待修复）

- **症状**: `VulkanCommandList::EndRenderPass()` 崩溃
- **日志**: `BeginRenderPass: no swapchain views or render pass set`
- **定位**: HybridRTPipeline 的最后一个 Pass（ToneMap → LDR → FXAA → BackBuffer）中调用 `BeginRenderPass` 时，`m_CurrentRenderPass` 与 SwapChain 兼容性问题
- **分析**: 
  - DeferredPipeline 通过 TAA/SMAA/FXAA 等中间 Pass 的 `SetPipeline` 自然设置正确的 RP
  - HybridRTPipeline 缺少这些中间 Pass（TAA/DOF/MotionBlur 均未启用）
  - FXAA PSO 的 depthFormat=Unknown 理论上应生成 1-attachment RP 与 SwapChain 兼容，但仍失败
  - 可能与 RenderGraph 的 swapchain 管理有关（DeferredPipeline 不调用 `rg.SetSwapChain()` 却能正常工作）
- **绕过方案**: 暂使用 DeferredPipeline（renderMode=1）
- **下一步**: 深入调试 VulkanCommandList 的 render pass 状态管理，或参考 DeferredPipeline 的完整后处理链
