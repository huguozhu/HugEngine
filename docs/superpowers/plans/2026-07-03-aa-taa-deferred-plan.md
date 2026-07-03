# AA_TAA：延迟渲染时域抗锯齿（含 Velocity Buffer）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 DeferredPipeline 中实现完整 TAA，包含 Velocity Buffer（GBuffer MRT3），消除几何锯齿并为后续 MotionBlur/TAAU/DLSS 铺路。

**Architecture:** 修改 GBuffer shader 输出 velocity（双 MVP 投影），新增 AA_TAA 类继承 IAntiAliasing + IPostProcessPass，TAA 内部管理 double-buffered history，RenderGraph 在 Lighting 和 ToneMap 之间插入 TAA_Resolve pass。

**Tech Stack:** Slang → SPIR-V, Vulkan 1.3+, C++17, GLM

**Spec:** `docs/superpowers/specs/2026-07-03-aa-taa-deferred-design.md`

## Global Constraints

- Vulkan push constant 上限 256B（引擎已设定）
- GBuffer PSO 从 3 MRT 扩到 4 MRT（新增 RG16_FLOAT 格式）
- 不引入新的第三方依赖
- 代码注释使用中文
- Commit 前手动确认，不自动提交

---

### Task 1: RHI — BeginOffscreenPassMRT 附件数组扩容

**Files:**
- Modify: `Engine/RHI/Vulkan/VulkanCommandList.cpp:457-459`

**Interfaces:**
- Consumes: 无（独立前置修复）
- Produces: `BeginOffscreenPassMRT` 支持最多 5 个总附件（4 颜色 + 1 深度）

- [ ] **Step 1: 修改附件数组大小和循环上限**

`Engine/RHI/Vulkan/VulkanCommandList.cpp` 中：

```cpp
// 将 VkImageView attachments[4] 改为 [5]
// Line ~457: VkImageView attachments[4] = {};
// 改为：
VkImageView attachments[5] = {};

// Line ~459: for (u32 i = 0; i < colorCount && attachmentCount < 4; ++i)
// 改为：
for (u32 i = 0; i < colorCount && attachmentCount < 5; ++i)
```

- [ ] **Step 2: 编译验证**

```bash
cd D:\Source\HugEngine && cmake --build build --target HugEngine --config Debug 2>&1 | tail -20
```

期望：`Build succeeded`。

---

### Task 2: GBuffer 顶点着色器 — 双矩阵投影

**Files:**
- Modify: `Engine/Shader/Shaders/GBuffer.vert.slang`

**Interfaces:**
- Produces: VSOutput 新增 `[[vk::location(3)]] float4 prevClipPos`
- Push constant 新增 `prevViewProjMatrix`（offset 64，64B）

- [ ] **Step 1: 重写 GBuffer.vert.slang**

将文件内容替换为：

```hlsl
// GBuffer.vert — 延迟渲染 GBuffer 顶点着色器（4 MRT + velocity）
// 双 MVP 投影：current + previous，支持 TAA velocity 计算
// 抖动：viewProjMatrix 已在 CPU 端加 jitter，prevViewProjMatrix 不抖动
#include "common.slang"

struct VSInput {
    [[vk::location(0)]] float3 positionOS;
    [[vk::location(1)]] float3 normalOS;
    [[vk::location(2)]] float2 uv;
};

struct VSOutput {
    float4 position              : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] float4 prevClipPos;   // 上一帧裁剪空间位置（无抖动）
};

struct GPUObjectData {
    float4x4 worldMatrix;
    float4   baseColorFactor;
    float4   emissiveFactor;
    float    metallicFactor;
    float    roughnessFactor;
    float    aoFactor;
    float    alphaCutoff;
    uint     materialFlags;
    uint     materialID;
    uint     _pad[2];
};

[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;

// Push constant：双矩阵 + objectIndex
// 注意：viewProjMatrix 已含抖动偏移（CPU 端合成 jitteredProj * view）
[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;       // offset 0,  64B — 当前帧 VP（含抖动）
    float4x4 prevViewProjMatrix;   // offset 64, 64B — 上一帧 VP（不含抖动）
    uint     objectIndex;          // offset 128, 4B
    uint     _pad[15];             // 填充到 192B（push constant range 声明为 256B）
};

VSOutput main(VSInput input) {
    GPUObjectData obj = u_Objects[objectIndex];
    float4 worldPos = mul(obj.worldMatrix, float4(input.positionOS, 1.0));

    VSOutput output;
    output.position    = mul(viewProjMatrix, worldPos);      // 含抖动
    output.prevClipPos = mul(prevViewProjMatrix, worldPos);  // 不含抖动，用于速度计算
    output.worldPos    = worldPos.xyz;
    output.worldNormal = mul((float3x3)obj.worldMatrix, input.normalOS);
    output.uv          = input.uv;
    return output;
}
```

---

### Task 3: GBuffer 片段着色器 — MRT3 velocity 输出

**Files:**
- Modify: `Engine/Shader/Shaders/GBuffer.frag.slang`

**Interfaces:**
- Consumes: `[[vk::location(3)]] float4 prevClipPos`（来自 Task 2 VSOutput）
- Produces: FSOutput 新增 `[[vk::location(3)]] float2 velocity`

- [ ] **Step 1: 重写 GBuffer.frag.slang**

将文件内容替换为：

```hlsl
// GBuffer.frag — 延迟渲染 GBuffer 片段着色器（4 MRT + velocity）
// MRT 0: RGBA16  albedo.rgb + metallic
// MRT 1: RGBA16  worldNormal.xyz * 0.5 + 0.5 + roughness
// MRT 2: RGBA16  emissive.rgb + ao
// MRT 3: RG16    velocity.xy = currUV - prevUV（UV 空间运动矢量）

struct VSOutput {
    float4 position              : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] float4 prevClipPos;
};

struct GPUObjectData {
    float4x4 worldMatrix;
    float4   baseColorFactor;
    float4   emissiveFactor;
    float    metallicFactor;
    float    roughnessFactor;
    float    aoFactor;
    float    alphaCutoff;
    uint     materialFlags;
    uint     materialID;
    uint     _pad[2];
};

// set=0: per-frame GPUObjectData[]
[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;
// set=1: per-mesh 静态纹理
[[vk::binding(5, 1)]] Texture2D<float4> u_BaseColorTex;
[[vk::binding(5, 1)]] SamplerState       u_BaseColorSampler;
[[vk::binding(6, 1)]] Texture2D<float4> u_NormalTex;
[[vk::binding(6, 1)]] SamplerState       u_NormalSampler;
[[vk::binding(7, 1)]] Texture2D<float4> u_MetallicRoughnessTex;
[[vk::binding(7, 1)]] SamplerState       u_MetallicRoughnessSampler;
[[vk::binding(8, 1)]] Texture2D<float4> u_OcclusionTex;
[[vk::binding(8, 1)]] SamplerState       u_OcclusionSampler;

[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;
    float4x4 prevViewProjMatrix;
    uint     objectIndex;
    uint     _pad[15];
};

struct FSOutput {
    [[vk::location(0)]] float4 albedoMetallic  : SV_Target0;
    [[vk::location(1)]] float4 normalRoughness : SV_Target1;
    [[vk::location(2)]] float4 emissiveAO      : SV_Target2;
    [[vk::location(3)]] float2 velocity         : SV_Target3;  // UV 空间运动矢量
};

FSOutput fragmentMain(VSOutput input) {
    GPUObjectData obj = u_Objects[objectIndex];

    // ── MRT 0-2：PBR 材质（逻辑不变） ──
    float4 baseColorSample = u_BaseColorTex.Sample(u_BaseColorSampler, input.uv);
    float3 albedo   = obj.baseColorFactor.rgb * baseColorSample.rgb;
    float  alpha    = obj.baseColorFactor.a * baseColorSample.a;
    float  metallic = obj.metallicFactor * u_MetallicRoughnessTex.Sample(u_MetallicRoughnessSampler, input.uv).b;

    if (alpha < obj.alphaCutoff) discard;

    float3 texNormal = u_NormalTex.Sample(u_NormalSampler, input.uv).rgb * 2.0 - 1.0;
    float3 N = normalize(input.worldNormal + texNormal * 0.5);

    float roughness = clamp(obj.roughnessFactor *
        u_MetallicRoughnessTex.Sample(u_MetallicRoughnessSampler, input.uv).g, 0.04, 1.0);
    float ao = obj.aoFactor * u_OcclusionTex.Sample(u_OcclusionSampler, input.uv).r;

    // ── MRT 3：Velocity（UV 空间运动矢量 = currUV - prevUV） ──
    float3 currNDC = input.position.xyz / input.position.w;
    float2 currUV  = currNDC.xy * 0.5 + 0.5;

    float3 prevNDC = input.prevClipPos.xyz / input.prevClipPos.w;
    float2 prevUV  = prevNDC.xy * 0.5 + 0.5;

    float2 motion = currUV - prevUV;

    FSOutput output;
    output.albedoMetallic  = float4(albedo, metallic);
    output.normalRoughness = float4(N * 0.5 + 0.5, roughness);
    output.emissiveAO      = float4(obj.emissiveFactor.rgb, ao);
    output.velocity        = motion;
    return output;
}
```

---

### Task 4: TAA_Resolve 顶点着色器

**Files:**
- Create: `Engine/Shader/Shaders/TAA_Resolve.vert.slang`

**Interfaces:**
- Produces: VSOutput.position (SV_Position), VSOutput.uv (location 0)

- [ ] **Step 1: 创建 TAA_Resolve.vert.slang**

```hlsl
// TAA_Resolve.vert — TAA 全屏三角形顶点着色器（无 VB/IB）
// 3 顶点通过 SV_VertexID 生成，覆盖 [0,1] UV 全屏

struct VSOutput {
    float4 position          : SV_Position;
    [[vk::location(0)]] float2 uv;
};

VSOutput main(uint vid : SV_VertexID) {
    VSOutput o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.position = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv.y = 1.0 - o.uv.y;  // Vulkan 上下翻转
    return o;
}
```

---

### Task 5: TAA_Resolve 片段着色器

**Files:**
- Create: `Engine/Shader/Shaders/TAA_Resolve.frag.slang`

**Interfaces:**
- Consumes: set=0 binding 0-5（CurrentColor, HistoryColor, Depth, Normal, Velocity, TAAUniforms）
- Push constant: float2 jitterOffset
- Produces: float4 SV_Target0（HDR 颜色）

- [ ] **Step 1: 创建 TAA_Resolve.frag.slang**

```hlsl
// TAA_Resolve.frag — TAA 时域抗锯齿 resolve 片段着色器
// 算法：重投影 → disocclusion 检测 → 邻域 AABB 裁剪 → 混合
#include "common.slang"

struct VSOutput {
    float4 position          : SV_Position;
    [[vk::location(0)]] float2 uv;
};

// ── 输入纹理（set=0） ──
[[vk::binding(0, 0)]] Texture2D<float4> u_CurrentColor;
[[vk::binding(0, 0)]] SamplerState      u_PointSampler;
[[vk::binding(1, 0)]] Texture2D<float4> u_HistoryColor;
[[vk::binding(2, 0)]] Texture2D<float>  u_Depth;
[[vk::binding(3, 0)]] Texture2D<float4> u_Normal;      // GBuffer B: normal*0.5+0.5
[[vk::binding(4, 0)]] Texture2D<float2> u_Velocity;    // GBuffer D: currUV - prevUV

// ── Uniform Buffer ──
struct TAAUniforms {
    float4x4 prevViewProj;
    float4x4 invCurrViewProj;
    float2   resolution;
    float    blendFactor;      // 基础混合系数 (0.05)
    float    unused;
};
[[vk::binding(5, 0)]] cbuffer u_TAA : register(b5) { TAAUniforms u_TAA; }

// ── Push Constants ──
[[vk::push_constant]] cbuffer TAA_PC {
    float2 jitterOffset;  // 当前帧子像素抖动（NDC 空间）
};

// ── YCoCg 颜色空间转换（用于邻域 AABB 裁剪） ──
float3 rgb_to_ycocg(float3 c) {
    float y  =  0.25 * c.r + 0.5  * c.g + 0.25 * c.b;
    float co =  0.5  * c.r               - 0.5  * c.b;
    float cg = -0.25 * c.r + 0.5  * c.g - 0.25 * c.b;
    return float3(y, co, cg);
}
float3 ycocg_to_rgb(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// ── 当前帧 3×3 邻域 YCoCg AABB ──
void current_color_aabb(float2 uv, out float3 aabbMin, out float3 aabbMax) {
    float2 ts = 1.0 / u_TAA.resolution;
    float3 c00 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2(-ts.x, -ts.y)).rgb);
    float3 c10 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2( 0.0,  -ts.y)).rgb);
    float3 c20 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2( ts.x, -ts.y)).rgb);
    float3 c01 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2(-ts.x,  0.0)).rgb);
    float3 c11 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv).rgb);
    float3 c21 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2( ts.x,  0.0)).rgb);
    float3 c02 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2(-ts.x,  ts.y)).rgb);
    float3 c12 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2( 0.0,   ts.y)).rgb);
    float3 c22 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2( ts.x,  ts.y)).rgb);

    aabbMin = min(min(min(c00,c10),min(c20,c01)),min(min(c11,c21),min(c02,c12)));
    aabbMin = min(aabbMin, c22);
    aabbMax = max(max(max(c00,c10),max(c20,c01)),max(max(c11,c21),max(c02,c12)));
    aabbMax = max(aabbMax, c22);

    // 轻微膨胀 AABB，容忍微小颜色变化
    float3 ext = (aabbMax - aabbMin) * 0.01;
    aabbMin -= ext;
    aabbMax += ext;
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;

    // 1. 当前帧颜色
    float3 currColor = u_CurrentColor.Sample(u_PointSampler, uv).rgb;

    // 2. Velocity → history UV
    float2 vel = u_Velocity.Sample(u_PointSampler, uv).rg;
    float2 historyUV = uv - vel;

    // 3. Disocclusion 检测
    float depth = u_Depth.Sample(u_PointSampler, uv).r;
    float3 N    = normalize(u_Normal.Sample(u_PointSampler, uv).rgb * 2.0 - 1.0);

    bool offScreen = historyUV.x < 0.0 || historyUV.x > 1.0 ||
                     historyUV.y < 0.0 || historyUV.y > 1.0;
    float disocclusion = 0.0;

    if (!offScreen) {
        float depthHist = u_Depth.Sample(u_PointSampler, historyUV).r;
        float3 N_hist   = normalize(u_Normal.Sample(u_PointSampler, historyUV).rgb * 2.0 - 1.0);

        // 深度差异
        float depthDiff = abs(depth - depthHist) / max(depth, 0.001);
        disocclusion = max(disocclusion, smoothstep(0.001, 0.01, depthDiff));

        // 法线差异
        float normalDiff = 1.0 - dot(N, N_hist);
        disocclusion = max(disocclusion, smoothstep(0.001, 0.05, normalDiff));

        // 速度过大
        float speed = length(vel);
        disocclusion = max(disocclusion, smoothstep(0.01, 0.05, speed));
    } else {
        disocclusion = 1.0;
    }

    // 4. 混合
    float4 output;
    if (disocclusion > 0.95 || offScreen) {
        output = float4(currColor, 1.0);
    } else {
        float3 histColor = u_HistoryColor.Sample(u_PointSampler, historyUV).rgb;

        // YCoCg 邻域 AABB 裁剪
        float3 aabbMin, aabbMax;
        current_color_aabb(uv, aabbMin, aabbMax);
        float3 histYC = rgb_to_ycocg(histColor);
        histYC = clamp(histYC, aabbMin, aabbMax);
        float3 clampedHist = ycocg_to_rgb(histYC);

        float blend = lerp(u_TAA.blendFactor, 0.3, disocclusion);
        float3 resolved = lerp(clampedHist, currColor, blend);
        output = float4(resolved, 1.0);
    }
    return output;
}
```

---

### Task 6: AA_TAA 头文件

**Files:**
- Create: `Engine/Render/AntiAliasing/AA_TAA.h`

**Interfaces:**
- Produces: `class AA_TAA final : public IAntiAliasing` — 全部方法签名

- [ ] **Step 1: 创建 AA_TAA.h**

```cpp
#pragma once

#include "AntiAliasing.h"
#include "RHI/RHI.h"
#include <memory>

namespace he::render {

// ============================================================================
// AA_TAA — 时域抗锯齿（Temporal Anti-Aliasing）
//
// 依赖 Velocity Buffer（GBuffer MRT3）做精确重投影。
// 仅支持延迟渲染（SupportsForward=false）。
//
// 管线位置：Lighting(HDR) → TAA_Resolve → ToneMap → BackBuffer
// 自拥有输出：double-buffered history（OwnsOutput=true）
// ============================================================================
class AA_TAA final : public IAntiAliasing {
    HE_DECLARE_NON_COPYABLE(AA_TAA);

public:
    AA_TAA()  = default;
    ~AA_TAA() override = default;

    // ── IRenderSubsystem ──
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList* cmd) const override {}
    void OnResize(u32 width, u32 height) override;

    [[nodiscard]] const char* GetName()  const override { return "AA_TAA"; }
    [[nodiscard]] bool        IsReady()  const override { return m_Ready; }
    [[nodiscard]] bool        IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool e) override { m_Enabled = e; }

    // ── IPostProcessPass ──

    /// 绑定当前帧 HDR 颜色输入
    void SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) override;

    /// TAA 输出 HDR 格式（ToneMap 前）
    [[nodiscard]] rhi::Format GetOutputFormat() const override { return rhi::Format::RGBA16_FLOAT; }

    /// TAA 自拥有历史缓冲
    [[nodiscard]] bool OwnsOutput() const override { return true; }
    [[nodiscard]] rhi::IRHITexture* GetOutputTexture() const override;
    [[nodiscard]] rhi::IRHISampler* GetOutputSampler() const override;

    // ── IAntiAliasing ──

    [[nodiscard]] AAMode GetMode() const override { return AAMode::TAA; }

    /// 当前帧 Halton 抖动偏移（NDC 空间，调用方写入投影矩阵）
    [[nodiscard]] float2 GetJitterOffset() const override;

    /// 推进抖动序列 + 交换历史 read/write index
    void OnBeginFrame() override;

    [[nodiscard]] bool SupportsForward()  const override { return false; }
    [[nodiscard]] bool SupportsDeferred() const override { return true; }

    // ── TAA 特有：绑定 GBuffer 辅助输入 ──

    /// 设置 GBuffer 深度 / 法线 / velocity 纹理（Render 前调用）
    /// @param depth     GBuffer Depth (D32_FLOAT)
    /// @param normal    GBuffer B (normal*0.5+0.5 + roughness)
    /// @param velocity  GBuffer D (RG16_FLOAT, UV 空间运动矢量)
    void SetGBufferInputs(rhi::IRHITexture* depth,
                          rhi::IRHITexture* normal,
                          rhi::IRHITexture* velocity);

    // ── Uniform 更新（管线每帧调用） ──

    /// 更新 TAA uniform buffer（prevViewProj、invCurrViewProj、resolution）
    /// @param prevViewProj    上一帧 ViewProj 矩阵
    /// @param invCurrViewProj 当前帧 InvViewProj（用于深度反算世界坐标）
    /// @param width, height   视口尺寸
    void UpdateUniforms(const float4x4& prevViewProj,
                        const float4x4& invCurrViewProj,
                        u32 width, u32 height);

private:
    void CreateHistoryTextures(u32 w, u32 h);
    void CreatePSO();
    [[nodiscard]] static float2 HaltonSample(u32 index);

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    // PSO + 描述符
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIBuffer> m_UniformBuffer;

    // 输入纹理（外部注入，不持有所有权）
    rhi::IRHITexture* m_InputColor      = nullptr;
    rhi::IRHISampler* m_InputSampler    = nullptr;
    rhi::IRHITexture* m_DepthTexture    = nullptr;
    rhi::IRHITexture* m_NormalTexture   = nullptr;
    rhi::IRHITexture* m_VelocityTexture = nullptr;

    // 历史缓冲（double-buffered，自拥有）
    std::unique_ptr<rhi::IRHITexture> m_HistoryColor[2];
    std::unique_ptr<rhi::IRHISampler> m_HistorySampler;
    u32 m_HistoryRead  = 0;
    u32 m_HistoryWrite = 1;

    // 抖动序列
    u32 m_FrameIndex   = 0;
    u32 m_JitterIndex  = 0;
    float2 m_CurrentJitter = {0.0f, 0.0f};

    // 前一帧 VP 矩阵（TAA 内部缓存，用于 uniform buffer）
    float4x4 m_PrevViewProj = float4x4(1.0f);
};

} // namespace he::render
```

---

### Task 7: AA_TAA 实现文件

**Files:**
- Create: `Engine/Render/AntiAliasing/AA_TAA.cpp`

**Interfaces:**
- Consumes: `AA_TAA.h`（Task 6）、`TAA_Resolve.vert.spv.h` / `TAA_Resolve.frag.spv.h`（CMake 编译产出）

- [ ] **Step 1: 创建 AA_TAA.cpp**

```cpp
#include "AntiAliasing/AA_TAA.h"
#include "TAA_Resolve.vert.spv.h"
#include "TAA_Resolve.frag.spv.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <glm/gtc/matrix_transform.hpp>

namespace he::render {

// ── Halton(2, 3) 序列，8 样本循环 ──
float2 AA_TAA::HaltonSample(u32 index) {
    // Halton(2): 0.5, -0.5, 0.25, -0.75, 0.875, -0.875, -0.125, 0.125
    // Halton(3): 0.333, -0.333, -0.111, 0.778, -0.556, -0.222, 0.444, -0.444
    static const float kHaltonX[8] = { 0.5f, -0.5f,  0.25f, -0.75f, 0.875f, -0.875f, -0.125f, 0.125f };
    static const float kHaltonY[8] = { 0.333f, -0.333f, -0.111f, 0.778f, -0.556f, -0.222f, 0.444f, -0.444f };
    u32 i = index % 8;
    return {kHaltonX[i], kHaltonY[i]};
}

bool AA_TAA::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    HE_ASSERT(m_Device, "AA_TAA: null device");

    // 描述符布局：bindings 0-4 为输入纹理，binding 5 为 uniform buffer
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // u_CurrentColor
        {1, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // u_HistoryColor
        {2, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // u_Depth
        {3, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // u_Normal
        {4, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // u_Velocity
        {5, rhi::DescriptorType::UniformBuffer, 1, 17},         // u_TAAUniforms
    };
    m_DescLayout = device->CreateDescriptorSetLayout(layout);
    m_DescSet    = device->AllocateDescriptorSet(m_DescLayout);

    // Uniform buffer（144 bytes，每帧更新 prevViewProj + invCurrViewProj + resolution）
    m_UniformBuffer = device->CreateBuffer({144, rhi::BufferUsage::Uniform});

    // 绑定 uniform buffer 到描述符（内容每帧 Map/Unmap 更新）
    device->UpdateDescriptorSet(m_DescSet, 5, rhi::DescriptorType::UniformBuffer,
                                m_UniformBuffer.get());

    // 历史缓冲
    CreateHistoryTextures(width, height);

    // PSO
    CreatePSO();

    m_Ready = true;
    HE_CORE_INFO("AA_TAA initialized ({}, {})", width, height);
    return true;
}

void AA_TAA::CreateHistoryTextures(u32 w, u32 h) {
    for (int i = 0; i < 2; ++i) {
        rhi::TextureDesc d;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.width  = w;
        d.height = h;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_HistoryColor[i] = m_Device->CreateTexture(d);
    }
    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU  = sd.addressV  = rhi::AddressMode::ClampToEdge;
    m_HistorySampler = m_Device->CreateSampler(sd);
}

void AA_TAA::CreatePSO() {
    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_TAA_Resolve_vert_spv;
    vs.entryPoint = "main";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_TAA_Resolve_frag_spv;
    fs.entryPoint = "main";

    rhi::PushConstantRange pc;
    pc.stageMask = 1 | 16;  // Vertex | Fragment
    pc.size      = 16;       // float2 jitterOffset

    rhi::PipelineStateDesc desc;
    desc.vertexShader        = &vs;
    desc.pixelShader         = &fs;
    desc.topology            = rhi::PrimitiveTopology::TriangleList;
    desc.depthTest           = false;
    desc.depthWrite          = false;
    desc.colorAttachmentCount = 1;
    desc.colorFormats[0]     = rhi::Format::RGBA16_FLOAT;  // HDR 输出
    desc.pushConstantRanges  = {pc};
    desc.descriptorSetLayouts = {m_DescLayout};
    desc.debugName            = "AA_TAA";
    m_PSO = m_Device->CreatePipelineState(desc);
    HE_ASSERT(m_PSO, "AA_TAA: PSO creation failed");
}

void AA_TAA::Shutdown() {
    if (m_Device && m_DescLayout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    }
    m_PSO.reset();
    m_UniformBuffer.reset();
    m_HistoryColor[0].reset();
    m_HistoryColor[1].reset();
    m_HistorySampler.reset();
    m_Device = nullptr;
    m_Ready  = false;
    HE_CORE_INFO("AA_TAA shutdown");
}

void AA_TAA::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width  = w;
    m_Height = h;
    m_HistoryColor[0].reset();
    m_HistoryColor[1].reset();
    CreateHistoryTextures(w, h);
    // 首帧使用当前帧 → 重设 read/write
    m_HistoryRead  = 0;
    m_HistoryWrite = 1;
}

void AA_TAA::SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) {
    m_InputColor   = color;
    m_InputSampler = sampler;
    if (m_InputColor && m_InputSampler) {
        m_Device->UpdateDescriptorSet(m_DescSet, 0, rhi::DescriptorType::CombinedImageSampler,
                                      m_InputColor, m_InputSampler);
    }
}

void AA_TAA::SetGBufferInputs(rhi::IRHITexture* depth,
                               rhi::IRHITexture* normal,
                               rhi::IRHITexture* velocity) {
    m_DepthTexture    = depth;
    m_NormalTexture   = normal;
    m_VelocityTexture = velocity;

    auto bind = [&](u32 b, rhi::IRHITexture* t) {
        if (t) m_Device->UpdateDescriptorSet(m_DescSet, b, rhi::DescriptorType::CombinedImageSampler,
                                             t, m_HistorySampler.get());
    };
    bind(2, depth);
    bind(3, normal);
    bind(4, velocity);
}

void AA_TAA::UpdateUniforms(const float4x4& prevViewProj,
                             const float4x4& invCurrViewProj,
                             u32 width, u32 height) {
    // TAAUniforms 布局（144B）：
    //   float4x4 prevViewProj      [0, 64)
    //   float4x4 invCurrViewProj   [64, 128)
    //   float2   resolution        [128, 136)
    //   float    blendFactor       [136, 140)
    //   float    unused            [140, 144)

    struct {
        float4x4 prevViewProj;
        float4x4 invCurrViewProj;
        float2   resolution;
        float    blendFactor;
        float    unused;
    } uniforms;

    uniforms.prevViewProj    = prevViewProj;
    uniforms.invCurrViewProj = invCurrViewProj;
    uniforms.resolution      = {(float)width, (float)height};
    uniforms.blendFactor     = 0.05f;
    uniforms.unused          = 0.0f;

    void* mapped = m_UniformBuffer->Map();
    if (mapped) {
        memcpy(mapped, &uniforms, sizeof(uniforms));
        m_UniformBuffer->Unmap();
    }
}

float2 AA_TAA::GetJitterOffset() const {
    return m_CurrentJitter;
}

void AA_TAA::OnBeginFrame() {
    // 推进抖动序列
    float2 rawJitter = HaltonSample(m_JitterIndex);
    m_JitterIndex = (m_JitterIndex + 1) % 8;

    // 缩放为 pixel → NDC offset
    m_CurrentJitter.x = rawJitter.x * 2.0f / (float)m_Width;
    m_CurrentJitter.y = rawJitter.y * 2.0f / (float)m_Height;

    // 交换历史缓冲 read/write（本次 TAA resolve 读取上一帧的历史，写入当前帧）
    m_HistoryWrite = m_HistoryRead;
    m_HistoryRead  = 1 - m_HistoryRead;

    ++m_FrameIndex;
}

rhi::IRHITexture* AA_TAA::GetOutputTexture() const {
    return m_HistoryColor[m_HistoryRead].get();
}

rhi::IRHISampler* AA_TAA::GetOutputSampler() const {
    return m_HistorySampler.get();
}

void AA_TAA::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled) return;

    // 更新 history 纹理绑定到当前 read buffer
    m_Device->UpdateDescriptorSet(m_DescSet, 1, rhi::DescriptorType::CombinedImageSampler,
                                  m_HistoryColor[m_HistoryRead].get(), m_HistorySampler.get());

    cmd->SetPipeline(m_PSO.get());
    cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
    cmd->SetScissor({0, 0, m_Width, m_Height});
    cmd->BindDescriptorSet(0, m_DescSet);

    // Push constant: jitterOffset（16 bytes）
    cmd->SetPushConstants(0, sizeof(float2), &m_CurrentJitter);

    // 全屏三角形
    cmd->Draw(3);
}

} // namespace he::render
```

---

### Task 8: Shader CMakeLists — 注册 TAA shader 编译

**Files:**
- Modify: `Engine/Shader/CMakeLists.txt`

**Interfaces:**
- Produces: `TAA_Resolve.vert.spv.h` / `TAA_Resolve.frag.spv.h` 编译产物

- [ ] **Step 1: 查看现有 DeferredLighting shader 编译规则作为参考**

打开 `Engine/Shader/CMakeLists.txt`，找到 `DeferredLighting.vert` / `DeferredLighting.frag` 的编译规则。在其附近添加 TAA 的编译规则。当前引擎使用 `compile_shader_to_spv` 函数（或类似的 cmake 自定义函数）。

- [ ] **Step 2: 添加 TAA_Resolve shader 编译规则**

在 `Engine/Shader/CMakeLists.txt` 中，DeferredLighting 的编译规则旁添加：

```cmake
# TAA Resolve shader（时域抗锯齿 resolve pass）
compile_shader_to_spv(Shaders/TAA_Resolve.vert.slang TAA_Resolve.vert.spv)
compile_shader_to_spv(Shaders/TAA_Resolve.frag.slang TAA_Resolve.frag.spv)
```

---

### Task 9: DeferredPipeline 头文件 — 新增成员

**Files:**
- Modify: `Engine/Render/Pipeline/DeferredPipeline.h`

**Interfaces:**
- Consumes: `AntiAliasing/AntiAliasing.h`（include 已存在）
- Produces: `m_GBufferD`, `m_AntiAliasing`, `m_PrevViewProj`, `m_CurrViewProj`, `m_GBufferSampler`

- [ ] **Step 1: 修改 DeferredPipeline.h**

在 `DeferredPipeline.h` 中添加以下成员变量（约 line 78，`m_GBufferC` 之后）：

```cpp
    // GBuffer 第 4 张 MRT：velocity（RG16_FLOAT，UV 空间运动矢量）
    std::unique_ptr<rhi::IRHITexture> m_GBufferD;
    std::unique_ptr<rhi::IRHISampler> m_GBufferSampler;  // GBuffer 读取通用采样器

    // 时域抗锯齿
    std::unique_ptr<IAntiAliasing> m_AntiAliasing;

    // 相机矩阵缓存（当前帧 + 上一帧，用于 velocity 计算和 TAA）
    float4x4 m_PrevViewProj = float4x4(1.0f);
    float4x4 m_CurrViewProj = float4x4(1.0f);
```

在 include 区域添加（若尚未存在）：

```cpp
#include "AntiAliasing/AntiAliasing.h"
```

---

### Task 10: DeferredPipeline 实现 — 4 MRT + TAA 集成

**Files:**
- Modify: `Engine/Render/Pipeline/DeferredPipeline.cpp`

**Interfaces:**
- Consumes: All previous tasks
- Produces: 完整 TAA 集成管线

- [ ] **Step 1: 添加 include**

在 `DeferredPipeline.cpp` 顶部添加：

```cpp
#include "AntiAliasing/AA_TAA.h"
```

- [ ] **Step 2: Initialize — 新增 m_GBufferD 创建**

在 `m_GBufferC` 创建代码后添加（约 line 41）：

```cpp
    // GBuffer D: velocity (RG16_FLOAT)
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::RG16_FLOAT;
        d.width  = m_Width; d.height = m_Height;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_GBufferD = device->CreateTexture(d);
    }
```

- [ ] **Step 3: Initialize — 新增 GBuffer 通用采样器**

在 `m_HDRSampler` 创建之后添加：

```cpp
    // GBuffer 读取采样器（线性 + Clamp to Edge，用于 TAA 采样 Depth/Normal/Velocity）
    {
        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU  = sd.addressV  = rhi::AddressMode::ClampToEdge;
        m_GBufferSampler = device->CreateSampler(sd);
    }
```

- [ ] **Step 4: Initialize — 修改 GBuffer PSO 为 4 MRT**

将 PSO 描述中的 3 → 4：

```cpp
    // 原来：gbDesc.colorAttachmentCount = 3;
    gbDesc.colorAttachmentCount = 4;
    gbDesc.colorFormats[0] = gbDesc.colorFormats[1] = gbDesc.colorFormats[2] = rhi::Format::RGBA16_FLOAT;
    gbDesc.colorFormats[3] = rhi::Format::RG16_FLOAT;  // velocity ← 新增
```

- [ ] **Step 5: Initialize — push constant 扩容**

```cpp
    // 原来：pc.size = 128;
    rhi::PushConstantRange pc; pc.stageMask = 1|16; pc.size = 256; // 双矩阵 128B + objectIndex + padding
```

- [ ] **Step 6: Initialize — 创建 AA_TAA 实例**

在子系统初始化区域（`m_SceneRenderer` 之后）添加：

```cpp
    // AA_TAA
    m_AntiAliasing = std::make_unique<AA_TAA>();
    if (!m_AntiAliasing->Initialize(device, m_Width, m_Height)) {
        HE_CORE_WARN("DeferredPipeline: AA_TAA init failed, anti-aliasing disabled");
        m_AntiAliasing.reset();
    }
```

- [ ] **Step 7: Shutdown — 清理新增资源**

在 `Shutdown()` 方法中，`m_GBufferC.reset()` 之后添加：

```cpp
    m_GBufferD.reset();
    m_GBufferSampler.reset();
    if (m_AntiAliasing) m_AntiAliasing->Shutdown();
    m_AntiAliasing.reset();
```

- [ ] **Step 8: OnResize — 重建 m_GBufferD**

在 `OnResize` 中，`m_GBufferC` 重建的 `r()` 调用之后添加：

```cpp
    r(m_GBufferD, rhi::Format::RG16_FLOAT, rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource);
```

以及在 `m_Skybox` resize 之后：

```cpp
    if (m_AntiAliasing) m_AntiAliasing->OnResize(w, h);
```

- [ ] **Step 9: Render — 帧首矩阵 + 抖动 + 帧末矩阵保存**

在 `BuildFrameGraph()` 开头（创建 RenderGraph 之前），添加：

```cpp
    // ── 帧首：记录当前帧 ViewProj + 合成抖动 ViewProj ──
    float4x4 viewMat = camera.GetViewMatrix();
    float4x4 projMat = camera.GetProjMatrix();
    m_CurrViewProj = projMat * viewMat;

    // 首帧：prev = curr（velocity 为零）
    static bool firstFrame = true;
    if (firstFrame) { m_PrevViewProj = m_CurrViewProj; firstFrame = false; }

    // 带抖动的 ViewProj（TAA 子像素抖动，对投影矩阵第 3 列的 xy 做偏移）
    float4x4 jitteredProj = projMat;
    if (m_AntiAliasing && m_AntiAliasing->IsEnabled()) {
        float2 jitter = m_AntiAliasing->GetJitterOffset();
        jitteredProj[2][0] += jitter.x;  // proj[2][0] 映射 NDC x
        jitteredProj[2][1] += jitter.y;  // proj[2][1] 映射 NDC y
    }
    float4x4 jitteredVP = jitteredProj * viewMat;

    // TAA 帧首：推进抖动序列 + 交换历史缓冲
    if (m_AntiAliasing) m_AntiAliasing->OnBeginFrame();
```

然后在 GB_Clear pass 中，将 `jitteredVP` 和 `m_PrevViewProj` 写入 push constant：

```cpp
    // 原有：pc.vp = camera.GetViewProjMatrix();
    // 改为：
    pc.vp = jitteredVP;                    // 含抖动的当前帧 VP
    pc.prevViewProjMatrix = m_PrevViewProj; // 上一帧 VP（无抖动）
```

在 BuildFrameGraph 末尾（所有 Pass 声明之后、Compile 之前），保存上一帧矩阵：

```cpp
    // ── 帧末：保存当前帧 VP 供下一帧使用 ──
    m_PrevViewProj = m_CurrViewProj;
```

- [ ] **Step 10: GB_Clear pass — 改用新 push constant + 4 MRT + prevVP**

修改 build frame 中 GB_Clear pass 的内部 lambda。原 push constant 结构体：

```cpp
// 旧 pc 结构体：
// struct { float4x4 vp; u32 oi; u32 _pad[12]; } pc;

// 新 pc 结构体（与 GBuffer.vert.slang push constant 布局对齐）：
struct {
    float4x4 viewProjMatrix;       // offset 0
    float4x4 prevViewProjMatrix;   // offset 64
    u32      objectIndex;          // offset 128
    u32      _pad[15];             // offset 132 → sizeof = 192
} pc;
pc.viewProjMatrix     = jitteredVP;
pc.prevViewProjMatrix = m_PrevViewProj;
```

Clear values 从 4 个扩到 5 个（4 颜色 + 深度）：

```cpp
rhi::ClearValue clears[5]{};
clears[0].color[3] = 1.0f; clears[1].color[3] = 1.0f;
clears[2].color[3] = 1.0f; clears[3].color[0] = 0.0f; // velocity: 初始化为 0
clears[3].color[1] = 0.0f;
clears[4].depth     = 1.0f;
```

BeginOffscreenPassMRT 改为 4 颜色附件：

```cpp
void* cv[4] = {m_GBufferA->GetNativeHandle(), m_GBufferB->GetNativeHandle(),
               m_GBufferC->GetNativeHandle(), m_GBufferD->GetNativeHandle()};
c->BeginOffscreenPassMRT(cv, 4, m_GBufferDepth->GetNativeHandle(), w, h, clears, false);
```

- [ ] **Step 11: 插入 TAA Resolve Pass（Lighting 和 ToneMap 之间）**

在 Lighting pass 和 ToneMap pass 之间，新增 TAA resolve pass：

```cpp
    // TAA Resolve Pass — 在 HDR 空间做时域抗锯齿
    rg.AddPass("TAA_Resolve",
        {{hdrC, ResourceAccess::Read}},
        {}, // TAA 写自己的 HistoryColor（自拥有），不写入 graph 管理的外部 RT
        [&, h = m_Height, w = m_Width](rhi::IRHICommandList* c) {
            if (!m_AntiAliasing || !m_AntiAliasing->IsEnabled()) return;

            // 设置输入：HDR 颜色 + GBuffer 辅助纹理
            m_AntiAliasing->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
            m_AntiAliasing->SetGBufferInputs(m_GBufferDepth.get(),
                                              m_GBufferB.get(),
                                              m_GBufferD.get());

            // 更新 TAA uniform buffer
            float4x4 invCurrVP = glm::inverse(m_CurrViewProj);
            static_cast<AA_TAA*>(m_AntiAliasing.get())->UpdateUniforms(
                m_PrevViewProj, invCurrVP, m_Width, m_Height);

            // Barrier：HDR 从 RenderTarget 转为 ShaderResource
            c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                               rhi::PipelineStage::FragmentShader,
                               rhi::ResourceState::RenderTarget,
                               rhi::ResourceState::ShaderResource,
                               m_HDRTarget.get());

            m_AntiAliasing->Render(c);
        });
```

- [ ] **Step 12: ToneMap pass — 采样 TAA 输出而非 HDR 直接**

修改 ToneMap pass 的 SetInput 调用：

```cpp
    // ToneMap pass 采样 TAA 输出（而非直接采样 HDR）：
    // 旧代码：m_ToneMap->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
    // 改为：
    if (m_AntiAliasing && m_AntiAliasing->IsEnabled()) {
        m_ToneMap->SetInput(m_AntiAliasing->GetOutputTexture(),
                            m_AntiAliasing->GetOutputSampler());
    } else {
        m_ToneMap->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
    }
```

### Task 11: Build Verification — 编译 + 运行

- [ ] **Step 1: 重新生成 CMake（shader 文件列表变更）**

```bash
cd D:\Source\HugEngine && cmake -B build 2>&1 | tail -10
```

期望：CMake 配置成功，无错误。

- [ ] **Step 2: 编译所有 target**

```bash
cmake --build build --config Debug 2>&1 | tail -30
```

期望：所有 target 编译成功（GBuffer shader recompile, TAA_Resolve shader compile, AA_TAA.cpp, DeferredPipeline.cpp）。

- [ ] **Step 3: 运行 04.Deferred 示例**

```bash
# 通过 Visual Studio 或直接运行
# 检查输出日志中是否有 "AA_TAA initialized" 日志
```

期望：
- 引擎启动无崩溃
- 控制台输出 `AA_TAA initialized (W, H)`
- Sponza 场景正常渲染
- 画面几何边缘有可见的抗锯齿平滑效果
- 相机旋转/平移停止后无 ghosting 拖影

---

## 自检清单

- [ ] **Spec coverage** — 对照 spec 验证：
  - ✅ 4 MRT GBuffer（含 velocity）→ Task 2, 3, 10
  - ✅ AA_TAA 类 → Task 6, 7
  - ✅ TAA_Resolve shader → Task 4, 5
  - ✅ Halton 抖动序列 → Task 7 (HaltonSample)
  - ✅ YCoCg 邻域 AABB 裁剪 → Task 5
  - ✅ Disocclusion 检测（深度+法线+速度） → Task 5
  - ✅ Double-buffered history → Task 6, 7
  - ✅ DeferredPipeline 集成 → Task 9, 10
  - ✅ RHI 附件数组修复 → Task 1
  - ✅ 后续扩展（MotionBlur/TAAU 共享 velocity）→ 隐式完成（velocity 纹理已暴露）

- [ ] **Placeholder scan** — 无 TBD/TODO/待实现标记

- [ ] **Type consistency** — 所有接口名匹配：
  - `SetGBufferInputs(depth, normal, velocity)` — AA_TAA.h + DeferredPipeline.cpp 一致
  - `UpdateUniforms(prevVP, invCurrVP, w, h)` — AA_TAA.h + DeferredPipeline.cpp 一致
  - `m_HistoryColor[2]` — 声明和实现一致
  - Push constant `float2 jitterOffset` — shader 和 C++ 一致
