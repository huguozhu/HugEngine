# 物理渲染管线剩余缺口（5 项）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 完成物理渲染管线剩余 5 项缺口：sRGB 精确传递函数、后处理效果（胶片颗粒/晕影/色差/镜头畸变）、物理天空/太阳模型（Preetham）、HDR 输出（HDR10 PQ）。

**Architecture:** sRGB 精确分段函数替换 `pow(x,1/2.2)` 近似；镜头后处理合并为单一 `CameraEffectsPass`（LDR 空间，ToneMap/ColorGrading 之后、AA 之前）；物理天空用 Preetham 解析模型直接替换 `SkyboxPass` 的静态 Cubemap 采样（复用 `Skybox.vert.slang`，新增 `PhysicalSky.frag.slang`）；HDR 输出新增 `A2B10G10R10_UNORM_PACK32` 格式 + `VK_EXT_swapchain_colorspace` 的 HDR10 色彩空间 + ToneMap PQ（ST.2084）编码 + BT.709→BT.2020 色域映射。

**Tech Stack:** C++17 / Slang shader / Vulkan（CMake MSVC 2026）。

**Spec:** `docs/已实现功能/基于物理的渲染管线实现.md`（第 5/7/8/9/10 项）。

## Global Constraints

- **逐功能验证门控**：每个任务只实现一个功能，编译 + 运行验证通过后才进入下一个。
- **Commit 规则（CLAUDE.md）**：不自动 `git commit`；提交前征得用户确认。中文 commit log，不含 AI 信息。
- **代码注释**：新增代码附中文注释。
- **构建命令**：`cmake --build build --config Debug`（含 Slang→SPIR-V 编译；shader 语法错误在编译期暴露）。新增 `.slang` 必须同时加入 `Engine/Shader/CMakeLists.txt` 的显式列表（`FRAG_SLANG` 等），否则不会被编译。
- **运行验证**：`build/bin/Debug/04.Deferred.exe`（Deferred）与 `build/bin/Debug/02.Cube.exe`（Forward）。视觉确认由用户完成，自动化验证 = 编译通过 + 运行无崩溃/无 shader 报错。
- **shader entry point 注意**：新增 Pass 的 `ShaderBytecode.entryPoint` 以相邻 Pass 为准——`CameraEffectsPass` 对齐 `ColorGradingPass`（`vertexMain`/`fragmentMain`），物理天空对齐 `SkyboxPass`（`main`）。若 slangc 编译或管线创建报入口名不匹配，按实际 SPIR-V 入口名修正一行即可（`Engine/RHI/RHI/Shader.h:26` 的 `kDefaultShaderEntryPoint="main"` 说明主管线路径以 `main` 为默认入口）。
- **HDR 任务风险最高**：需改动 RHI 深层格式硬编码，务必逐处核对（本计划已用行号标注全部硬编码点）。

---

### Task 1: sRGB 精确传递函数

**Files:**
- Modify: `Engine/Shader/Shaders/common.slang:19-26`（`LinearToSRGB` / `SRGBToLinear`）
- Modify: `Engine/Shader/Shaders/ColorGrading.frag.slang:31-32`（替换局部近似 `c*c` / `sqrt`）

**Interfaces:**
- Consumes: `LinearToSRGB`/`SRGBToLinear` 被 `ToneMap.frag.slang`、`PBR.frag.slang` 等调用（保持函数名与签名不变，仅改内部实现）。
- Produces: 标准分段 sRGB 传递函数（误差 <0.5%），Task 4 的 PQ 编码复用 `common.slang` 常量风格。

- [ ] **Step 1: `common.slang` 替换为精确分段实现**

将 `common.slang:19-26` 的两处 `pow` 近似改为标准 IEC 61966-2-1 分段函数：

```hlsl
// 线性空间 → sRGB（标准分段传递函数，误差 < 0.5%）
// 阈值 0.0031308 为线性段与幂函数段的分界
float3 LinearToSRGB(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), float3(1.0 / 2.4)) - 0.055;
    return c <= 0.0031308 ? lo : hi;
}

// sRGB → 线性空间（标准分段传递函数）
float3 SRGBToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow((max(c, 0.0) + 0.055) / 1.055, float3(2.4));
    return c <= 0.04045 ? lo : hi;
}
```

- [ ] **Step 2: `ColorGrading.frag.slang` 替换局部近似**

将 `ColorGrading.frag.slang:31-32` 的局部函数改为标准实现（此文件未 include `common.slang`，故内联同款公式）：

```hlsl
// 标准分段 sRGB 传递函数（与 common.slang 保持一致）
float3 SRGBToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow((max(c, 0.0) + 0.055) / 1.055, float3(2.4));
    return c <= 0.04045 ? lo : hi;
}
float3 LinearToSRGB(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), float3(1.0 / 2.4)) - 0.055;
    return c <= 0.0031308 ? lo : hi;
}
```

- [ ] **Step 3: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（含 `ToneMap.frag` / `PBR.frag` / `ColorGrading.frag` 等引用 `common.slang` 的 shader）。

Run: `build/bin/Debug/04.Deferred.exe`
Expected: 画面与改造前几乎一致（sRGB 精确化只影响 <0.5% 的中间调，肉眼难辨）；无崩溃、无 shader 报错。

- [ ] **Step 4: Commit（先向用户确认）**

```bash
git add Engine/Shader/Shaders/common.slang Engine/Shader/Shaders/ColorGrading.frag.slang
git commit -m "Render: sRGB 精确分段传递函数"
```

---

### Task 2: 后处理效果（胶片颗粒 / 晕影 / 色差 / 镜头畸变）

> 将文档第 8 项（后处理效果）与第 10 项（镜头畸变）合并为一个 `CameraEffectsPass`——镜头畸变本就是第 8 项表格中的一项，统一在一个 LDR 全屏 Pass 中实现，避免碎片化。

**Files:**
- Create: `Engine/Shader/Shaders/CameraEffects.frag.slang`
- Create: `Engine/Render/PostProcess/CameraEffectsPass.h`
- Create: `Engine/Render/PostProcess/CameraEffectsPass.cpp`
- Modify: `Engine/Shader/CMakeLists.txt`（`FRAG_SLANG` 列表追加 `CameraEffects.frag.slang`）
- Modify: `Engine/Render/CMakeLists.txt`（`RENDER_SOURCES` 追加 `.h/.cpp`）
- Modify: `Engine/Render/PostProcess/PostProcessChain.h`（成员 + getter）
- Modify: `Engine/Render/PostProcess/PostProcessChain.cpp`（Shutdown/OnResize）
- Modify: `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp`（LDR 链插入 `CameraEffects`）

**Interfaces:**
- Consumes: `rhi::IRHITexture`/`IRHISampler`（上游 LDR 输出）、`SSAO.vert.spv.h`（`k_SSAO_vert_spv`，全屏 VS，复用 ColorGrading 同款）。
- Produces: `CameraEffectsPass::GetOutput()/GetOutputSampler()/SetInput()/Render()/SetEnabled()`（下游 AA Pass 消费）。

- [ ] **Step 1: 新建 `CameraEffects.frag.slang`**

```hlsl
// CameraEffects.frag.slang — 镜头后处理（胶片颗粒/晕影/色差/镜头畸变）
// 输入 LDR BGRA8 → 输出 LDR BGRA8

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_Input;
[[vk::binding(0, 0)]] SamplerState      u_Sampler;

[[vk::push_constant]] cbuffer PostFXParams {
    float filmGrain;      // 胶片颗粒强度（0=关闭）
    float vignette;       // 晕影强度（0=关闭）
    float caStrength;     // 色差强度（0=关闭）
    float distortion;     // 镜头畸变系数（正=桶形，负=枕形，0=关闭）
    float time;           // 时间（颗粒动画种子）
    float _pad[3];
};

// 哈希噪声（胶片颗粒）
float hashNoise(float2 p) {
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 center = float2(0.5, 0.5);
    float2 duv = input.uv - center;
    float r2 = dot(duv, duv);

    // 1. 镜头畸变：径向缩放 UV（桶形/枕形）
    float2 dUv = duv * (1.0 + distortion * r2);

    // 2. 色差：R/B 通道在畸变基础上向内外偏移
    float2 caR = dUv * (1.0 + caStrength);
    float2 caB = dUv * (1.0 - caStrength);
    float3 color;
    color.r = u_Input.Sample(u_Sampler, center + caR).r;
    color.g = u_Input.Sample(u_Sampler, center + dUv).g;
    color.b = u_Input.Sample(u_Sampler, center + caB).b;

    // 3. 晕影：边缘径向暗化（smoothstep 内径→外径）
    float d = length(duv) * 2.0;  // 中心 0 → 边缘 ~1.4
    float vig = 1.0 - vignette * smoothstep(0.4, 1.4, d);
    color *= vig;

    // 4. 胶片颗粒：时变哈希噪声
    color += (hashNoise(input.uv * 1000.0 + time) - 0.5) * filmGrain;

    return float4(clamp(color, 0.0, 1.0), 1.0);
}
```

- [ ] **Step 2: 新建 `CameraEffectsPass.h`**

```cpp
#pragma once
#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// CameraEffectsPass — LDR 镜头后处理（胶片颗粒/晕影/色差/镜头畸变）
// ToneMap/ColorGrading 之后、AA 之前执行
// ============================================================
class CameraEffectsPass {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    void SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler);
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput() const { return m_Output.get(); }
    rhi::IRHISampler* GetOutputSampler() const { return m_OutSampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd) const { if (m_Ready) cmd->SetPipeline(m_PSO.get()); }
    bool IsEnabled() const { return m_Enabled; }
    void SetEnabled(bool e) { m_Enabled = e; if (e && !m_Ready) EnsureInitialized(); }

    // 参数（0=关闭对应效果）
    float GetFilmGrain()           const { return m_FilmGrain; }
    void  SetFilmGrain(float g)          { m_FilmGrain = g; }
    float GetVignette()            const { return m_Vignette; }
    void  SetVignette(float v)           { m_Vignette = v; }
    float GetChromaticAberration() const { return m_CA; }
    void  SetChromaticAberration(float c){ m_CA = c; }
    float GetLensDistortion()      const { return m_Distortion; }
    void  SetLensDistortion(float d)     { m_Distortion = d; }

private:
    void EnsureInitialized() { if (!m_Ready && m_Device) Initialize(m_Device, m_Width, m_Height); }

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false, m_Enabled = false;
    float m_FilmGrain = 0.0f;
    float m_Vignette  = 0.0f;
    float m_CA        = 0.0f;
    float m_Distortion = 0.0f;
    float m_Time = 0.0f;  // 累计时间（颗粒动画）

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_OutSampler;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    rhi::IRHITexture* m_Input = nullptr;
    rhi::IRHISampler* m_InputSampler = nullptr;
};

} // namespace he::render
```

- [ ] **Step 3: 新建 `CameraEffectsPass.cpp`**

```cpp
// PostProcess/CameraEffectsPass.cpp — LDR 镜头后处理实现
#include "CameraEffectsPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "SSAO.vert.spv.h"
#include "CameraEffects.frag.spv.h"

namespace he::render {

bool CameraEffectsPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device; m_Width = width; m_Height = height;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {{0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment}};
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    rhi::ShaderBytecode vs, fs;
    vs.stage = rhi::ShaderStage::Vertex; vs.spirv = k_SSAO_vert_spv; vs.entryPoint = "vertexMain";
    fs.stage = rhi::ShaderStage::Pixel;  fs.spirv = k_CameraEffects_frag_spv; fs.entryPoint = "fragmentMain";

    rhi::PushConstantRange pc; pc.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment; pc.size = 32;  // 8 floats
    rhi::PipelineStateDesc d;
    d.vertexShader = &vs; d.pixelShader = &fs;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.depthTest = false; d.depthWrite = false; d.depthFormat = rhi::Format::Unknown;
    d.colorAttachmentCount = 1; d.colorFormats[0] = rhi::Format::BGRA8_UNORM;
    d.pushConstantRanges = {pc}; d.descriptorSetLayouts = {m_Layout}; d.debugName = "CameraEffects";
    m_PSO = device->CreatePipelineState(d);
    HE_ASSERT(m_PSO, "CameraEffectsPass: PSO failed");

    rhi::TextureDesc td; td.format = rhi::Format::BGRA8_UNORM;
    td.width = width; td.height = height;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = device->CreateTexture(td);
    rhi::SamplerDesc sd; sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
    m_OutSampler = device->CreateSampler(sd);

    m_Ready = true;
    HE_CORE_INFO("CameraEffectsPass 初始化完成");
    return true;
}

void CameraEffectsPass::Shutdown() {
    m_PSO.reset(); m_Output.reset(); m_OutSampler.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_Layout);
    m_Device = nullptr; m_Ready = false;
}

void CameraEffectsPass::OnResize(u32 w, u32 h) {
    m_Width = w; m_Height = h;
    rhi::TextureDesc td; td.format = rhi::Format::BGRA8_UNORM;
    td.width = w; td.height = h; td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = m_Device->CreateTexture(td);
}

void CameraEffectsPass::SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) {
    m_Input = color; m_InputSampler = sampler;
    if (m_Input && m_InputSampler)
        m_Device->UpdateDescriptorSet(m_Set, 0, rhi::DescriptorType::CombinedImageSampler, m_Input, m_InputSampler);
}

void CameraEffectsPass::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_Input) return;

    m_Time += 0.016f;  // 帧间隔（颗粒动画），与 AutoExposure kDefaultDeltaTime 一致

    struct { float filmGrain, vignette, ca, distortion, time, _pad[3]; } pc;
    pc.filmGrain = m_FilmGrain; pc.vignette = m_Vignette;
    pc.ca = m_CA; pc.distortion = m_Distortion; pc.time = m_Time;

    cmd->SetPipeline(m_PSO.get()); cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
    cmd->SetScissor({0, 0, m_Width, m_Height});
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Draw(3);
}

} // namespace he::render
```

- [ ] **Step 4: CMake 注册新文件**

`Engine/Shader/CMakeLists.txt` 的 `FRAG_SLANG` 列表（`:76` 后）追加：

```cmake
        "${SHADER_DIR}/CameraEffects.frag.slang"
```

`Engine/Render/CMakeLists.txt` 的 `RENDER_SOURCES`（`:126` 后）追加：

```cmake
    PostProcess/CameraEffectsPass.h
    PostProcess/CameraEffectsPass.cpp
```

- [ ] **Step 5: `PostProcessChain` 接入**

`PostProcessChain.h`：`ColorGradingPass m_ColorGrading;` 之后新增成员，并在 getter 区新增：

```cpp
    ColorGradingPass m_ColorGrading;
    CameraEffectsPass m_CameraEffects;   // LDR 镜头后处理
```

```cpp
    ColorGradingPass& GetColorGrading() { return m_ColorGrading; }
    CameraEffectsPass& GetCameraEffects() { return m_CameraEffects; }  // 新增 getter
```

`PostProcessChain.cpp`：`Shutdown()`（`:64` 后）追加 `m_CameraEffects.Shutdown();`；`OnResize()`（`:108` 后）追加 `m_CameraEffects.OnResize(width, height);`。

- [ ] **Step 6: `DeferredPipeline_FrameGraph.cpp` 编排**

在 `useColor`（`:547`）之后新增：

```cpp
    bool useFX = m_PostProcess.GetCameraEffects().IsEnabled()
                 && m_PostProcess.GetCameraEffects().GetOutput() != nullptr;
```

在 `ColorGrading` Pass（`:606` 的 `}` 之后）新增 `CameraEffects` Pass：

```cpp
    // CameraEffects Pass（LDR 镜头后处理，ColorGrading 之后、AA 之前）
    if (useFX) {
        auto fxOut = rg.ImportTexture("FX_Out", m_PostProcess.GetCameraEffects().GetOutput());
        rg.AddPass("CameraEffects",
            {{ldrTarget, ResourceAccess::Read}},
            {{fxOut, ResourceAccess::Write}},
            [this, useColor, w, h](rhi::IRHICommandList* c) {
                auto* fxIn = useColor ? m_PostProcess.GetColorGrading().GetOutput() : m_PostProcess.GetLDRTarget();
                auto* fxSp = useColor ? m_PostProcess.GetColorGrading().GetOutputSampler() : m_PostProcess.GetLDRSampler();
                m_PostProcess.GetCameraEffects().SetInput(fxIn, fxSp);
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, fxIn);
                m_PostProcess.GetCameraEffects().PreBind(c);
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_PostProcess.GetCameraEffects().GetOutput()->GetNativeHandle(), nullptr, w, h, &clr, false);
                m_PostProcess.GetCameraEffects().Render(c);
                c->EndOffscreenPass();
            });
    }
```

更新 SMAA（`:611`）与 FXAA（`:633`）的输入选择，使其优先读取 `CameraEffects` 输出：

```cpp
    if (useSMAA) {
        auto* smaaInput = useFX   ? m_PostProcess.GetCameraEffects().GetOutput()
                        : useColor ? m_PostProcess.GetColorGrading().GetOutput()
                        :           m_PostProcess.GetLDRTarget();
        auto* smaaSamp  = useFX   ? m_PostProcess.GetCameraEffects().GetOutputSampler()
                        : useColor ? m_PostProcess.GetColorGrading().GetOutputSampler()
                        :           m_PostProcess.GetLDRSampler();
        ...
    }
    else if (useFXAA) {
        auto* fxaaInput = useFX   ? m_PostProcess.GetCameraEffects().GetOutput()
                        : useColor ? m_PostProcess.GetColorGrading().GetOutput()
                        :           m_PostProcess.GetLDRTarget();
        auto* fxaaSamp  = useFX   ? m_PostProcess.GetCameraEffects().GetOutputSampler()
                        : useColor ? m_PostProcess.GetColorGrading().GetOutputSampler()
                        :           m_PostProcess.GetLDRSampler();
        ...
    }
```

- [ ] **Step 7: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（`CameraEffects.frag` → SPIR-V 成功）。

Run: `build/bin/Debug/04.Deferred.exe`
Expected: 默认参数（全 0）下画面与改造前一致（Pass 关闭无副作用）。临时在 `DeferredPipeline` 构造处 `SetVignette(0.6f); SetFilmGrain(0.03f); SetEnabled(true);` 验证暗角与颗粒可见，验证后还原为 0。视觉确认由用户完成。

- [ ] **Step 8: Commit（先向用户确认）**

```bash
git add Engine/Shader/Shaders/CameraEffects.frag.slang Engine/Shader/CMakeLists.txt Engine/Render/PostProcess/CameraEffectsPass.h Engine/Render/PostProcess/CameraEffectsPass.cpp Engine/Render/PostProcess/PostProcessChain.h Engine/Render/PostProcess/PostProcessChain.cpp Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp Engine/Render/CMakeLists.txt
git commit -m "Render: 镜头后处理（颗粒/晕影/色差/畸变）"
```

---

### Task 3: 物理天空/太阳模型（Preetham）

> 采用 Preetham 解析大气散射（瑞利 + 米氏 + 天顶亮度），纯解析自包含，无 LUT 预计算，贴合文档「解析式计算」与 300 行预算。空中透视（大气消光应用到场景对象）涉及光照 shader，本轮暂缓，见文末「暂缓项」。

**Files:**
- Create: `Engine/Scene/Scene/PhysicalSkyComponent.h`
- Create: `Engine/Scene/Scene/PhysicalSkyComponent.cpp`
- Create: `Engine/Shader/Shaders/PhysicalSky.frag.slang`
- Modify: `Engine/Scene/Scene/SceneReflect.cpp`（注册组件）
- Modify: `Engine/Scene/CMakeLists.txt`（`SCENE_SOURCES` 追加）
- Modify: `Engine/Shader/CMakeLists.txt`（`FRAG_SLANG` 追加 `PhysicalSky.frag.slang`）
- Modify: `Engine/Render/PostProcess/SkyboxPass.h`（第二个 PSO + 缓存物理天空组件）
- Modify: `Engine/Render/PostProcess/SkyboxPass.cpp`（物理天空路径）

**Interfaces:**
- Consumes: `Skybox.vert.slang`（`k_Skybox_vert_spv`，输出 `worldDir`）、`World::ForEach`（遍历组件）。
- Produces: `he::PhysicalSkyComponent`（`sunDirection/turbidity/groundAlbedo/intensity/sunIntensity/enabled`）、`SkyboxPass` 物理天空渲染路径（物理天空优先于 Cubemap）。

- [ ] **Step 1: 新建 `PhysicalSkyComponent.h`**

```cpp
#pragma once
#include "Scene/Component.h"
#include "Math/Math.h"

// ============================================================
// PhysicalSkyComponent — 物理天空组件（Preetham 大气散射模型）
//
// 提供太阳方向、浑浊度、地面反照率等参数，由 SkyboxPass 渲染解析天空。
// 用法: world.AddComponent<PhysicalSkyComponent>(entity);
// ============================================================

namespace he {

class PhysicalSkyComponent : public Component {
    HE_COMPONENT()
public:
    void OnCreate() override;

    float3 sunDirection = float3(0.0f, 0.6f, 0.4f);  // 太阳方向（世界空间，OnCreate 归一化）
    float  turbidity    = 4.0f;   // 大气浑浊度（1=极清，2=清，5=霾，10=浓霾）
    float  groundAlbedo = 0.1f;   // 地面反照率（0~1，影响天空亮度）
    float  intensity    = 1.0f;   // 天空整体亮度倍率
    float  sunIntensity = 1.0f;   // 太阳盘亮度倍率
    bool   enabled      = true;   // 是否渲染
};

} // namespace he
```

- [ ] **Step 2: 新建 `PhysicalSkyComponent.cpp`**

```cpp
// ============================================================
// PhysicalSkyComponent.cpp — 物理天空组件实现
// ============================================================

#include "Scene/PhysicalSkyComponent.h"

namespace he {

void PhysicalSkyComponent::OnCreate() {
    sunDirection = glm::normalize(sunDirection);
}

} // namespace he
```

- [ ] **Step 3: 新建 `PhysicalSky.frag.slang`（Preetham 模型）**

```hlsl
// PhysicalSky.frag.slang — Preetham 解析大气散射天空
// 输入 worldDir（Skybox.vert 反算），输出线性 HDR 天空辐射度

static const float SKY_PI = 3.14159265359;

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldDir;
};

[[vk::push_constant]] cbuffer PhysicalSkyPC {
    float4x4 invViewProjMatrix;  // [0..64]（VS 使用，与 Skybox.vert 对齐）
    float    intensity;          // [64..68]
    float3   sunDirection;       // [68..80]
    float    turbidity;          // [80..84]
    float    groundAlbedo;       // [84..88]
    float    sunIntensity;       // [88..92]
    float    _pad;               // [92..96]
};

// ── Perez 天空分布函数 ──
// A/B/C/D/E 为系数，cosTheta=视线天顶角余弦，gamma=视线与太阳夹角
float Perez(float A, float B, float C, float D, float E, float cosTheta, float gamma, float cosGamma) {
    float t = 1.0 + A * exp(B / max(cosTheta, 0.0001));
    return t * (1.0 + C * exp(D * gamma) + E * cosGamma * cosGamma);
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float3 dir = normalize(input.worldDir);
    float3 sun = normalize(sunDirection);

    // 仅渲染上半球；dir.y<=0 为地面，输出简化地面色（SkyboxPass 深度 EQUAL 只填无几何处）
    if (dir.y <= 0.0) {
        return float4(float3(groundAlbedo) * 0.5 * intensity, 1.0);
    }

    float cosTheta = clamp(dir.y, 0.0, 1.0);         // 视线天顶角余弦
    float thetaSun = acos(clamp(sun.y, 0.0, 1.0));   // 太阳天顶角
    float cosSun   = clamp(sun.y, 0.0, 1.0);
    float cosGamma = clamp(dot(dir, sun), -1.0, 1.0);
    float gamma    = acos(cosGamma);

    float T = turbidity;
    float T2 = T * T;

    // ── 天顶 xyY（Preetham 系数拟合）──
    float chi  = (4.0 / 9.0 - T / 120.0) * SKY_PI;
    float chi2 = chi * chi, chi3 = chi2 * chi;

    float Yz = (4.0453 * T - 4.9710) * tan(chi) - 0.2155 * T + 2.4192;

    float xz = T2 * (0.00166 * chi3 - 0.00375 * chi2 + 0.00209 * chi)
             + T  * (-0.02903 * chi3 + 0.06377 * chi2 - 0.03202 * chi + 0.00394)
             + (0.11693 * chi3 - 0.21196 * chi2 + 0.06052 * chi + 0.25886);
    float yz = T2 * (0.00275 * chi3 - 0.00610 * chi2 + 0.00317 * chi)
             + T  * (-0.04214 * chi3 + 0.08970 * chi2 - 0.04153 * chi + 0.00516)
             + (0.15346 * chi3 - 0.26756 * chi2 + 0.06670 * chi + 0.26688);

    // ── 亮度（luminance）分布系数 ──
    float Al =  0.1787 * T - 1.4630;
    float Bl = -0.3554 * T + 0.4275;
    float Cl = -0.0227 * T + 5.3251;
    float Dl =  0.1206 * T - 2.5771;
    float El = -0.0670 * T + 0.3703;

    // ── x 色度分布系数 ──
    float Ax = -0.0193 * T - 0.2592, Bx = -0.0665 * T + 0.0008,
          Cx = -0.0004 * T + 0.2125, Dx = -0.0641 * T - 0.8989, Ex = -0.0033 * T + 0.0452;
    // ── y 色度分布系数 ──
    float Ayc = -0.0167 * T - 0.2608, Byc = -0.0950 * T + 0.0092,
          Cyc = -0.0079 * T + 0.2102, Dyc = -0.0441 * T - 1.6537, Eyc = -0.0109 * T + 0.0529;

    // ── 天顶处 Perez 分布（分母归一化）──
    float fYz = Perez(Al, Bl, Cl, Dl, El, 1.0, thetaSun, cosSun);
    float fxz = Perez(Ax, Bx, Cx, Dx, Ex, 1.0, thetaSun, cosSun);
    float fyz = Perez(Ayc, Byc, Cyc, Dyc, Eyc, 1.0, thetaSun, cosSun);

    // ── 视线方向 Perez 分布 ──
    float fY = Perez(Al, Bl, Cl, Dl, El, cosTheta, gamma, cosGamma);
    float fx = Perez(Ax, Bx, Cx, Dx, Ex, cosTheta, gamma, cosGamma);
    float fy = Perez(Ayc, Byc, Cyc, Dyc, Eyc, cosTheta, gamma, cosGamma);

    // ── 视线方向 xyY ──
    float Y = Yz * fY / fYz;
    float x = xz * fx / fxz;
    float y = yz * fy / fyz;

    // ── xyY → XYZ → 线性 RGB ──
    float X = (x * Y) / max(y, 0.0001);
    float Z = ((1.0 - x - y) * Y) / max(y, 0.0001);

    // Rec.709 色彩匹配矩阵（XYZ → linear RGB）
    float3 linearRGB;
    linearRGB.r =  3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    linearRGB.g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    linearRGB.b =  0.0557 * X - 0.2040 * Y + 1.0570 * Z;

    linearRGB *= intensity;

    // ── 太阳盘：视线接近太阳方向时叠加（~2° 视直径）──
    float sunDot  = dot(dir, sun);
    float sunDisc = smoothstep(0.99935, 0.99990, sunDot);
    linearRGB += float3(1.0, 0.95, 0.85) * sunDisc * sunIntensity * 2.0;

    return float4(linearRGB, 1.0);
}
```

- [ ] **Step 4: CMake 注册新文件**

`Engine/Scene/CMakeLists.txt` 的 `SCENE_SOURCES`（`:18` 后）追加：

```cmake
    Scene/PhysicalSkyComponent.h
    Scene/PhysicalSkyComponent.cpp
```

`Engine/Shader/CMakeLists.txt` 的 `FRAG_SLANG`（`:76` 后）追加：

```cmake
        "${SHADER_DIR}/PhysicalSky.frag.slang"
```

`Engine/Scene/Scene/SceneReflect.cpp`：include（`:13` 后）追加 `#include "Scene/PhysicalSkyComponent.h"`；注册区（`:55` 后）追加：

```cpp
HE_BEGIN_REGISTER(he::PhysicalSkyComponent)
HE_END_REGISTER()
```

- [ ] **Step 5: `SkyboxPass.h` 新增物理天空 PSO 与缓存**

include 区新增 `class PhysicalSkyComponent;` 前置声明；成员区（`:40` 后）新增：

```cpp
    // 物理天空（Preetham 解析模型，优先级高于 Cubemap）
    rhi::ShaderBytecode m_PS_FS;   // PhysicalSky 片段着色器
    std::unique_ptr<rhi::IRHIPipelineState> m_PS_PSO;
    const he::PhysicalSkyComponent* m_CachedPhysSky = nullptr;
```

- [ ] **Step 6: `SkyboxPass.cpp` 物理天空路径**

`Initialize()` 在创建 Cubemap PSO（`:32`）之后，追加物理天空 PSO（无纹理绑定，空描述符布局）：

```cpp
    // 物理天空 PSO：解析 Preetham 模型，无纹理采样（空描述符集）
    m_PS_FS.stage=rhi::ShaderStage::Pixel;m_PS_FS.spirv=k_PhysicalSky_frag_spv;m_PS_FS.entryPoint="fragmentMain";
    {
        rhi::PipelineStateDesc d;d.vertexShader=&m_VS;d.pixelShader=&m_PS_FS;  // 复用 Skybox.vert（m_VS）
        d.topology=rhi::PrimitiveTopology::TriangleList;
        d.depthTest=true;d.depthWrite=false;d.depthCompare=rhi::CompareFunc::Equal;
        d.depthFormat=rhi::Format::D32_FLOAT;
        d.colorAttachmentCount=1;d.colorFormats[0]=rhi::Format::RGBA16_FLOAT;
        d.pushConstantRanges={rhi::PushConstantRange{rhi::kStageMaskVertex|rhi::kStageMaskFragment,0,96}};
        d.descriptorSetLayouts={};d.debugName="PhysicalSky";
        m_PS_PSO=device->CreatePipelineState(d);
        HE_ASSERT(m_PS_PSO,"SkyboxPass: PhysicalSky PSO failed");
    }
```

`Update()` 开头（缓存相机之后）新增物理天空组件查找：

```cpp
    // 查找物理天空组件（优先级高于 Cubemap）
    if(ctx.world){
        const he::PhysicalSkyComponent* foundSky=nullptr;
        ctx.world->ForEach<he::PhysicalSkyComponent>([&](he::Entity,he::PhysicalSkyComponent& ps){
            if(ps.enabled)foundSky=&ps;
        });
        m_CachedPhysSky=foundSky;
    }
```

`Render()` 改为物理天空优先（`:66` 的守卫与 `:79` 的绘制之间插入）：

```cpp
    // 物理天空：解析 Preetham 模型（无纹理绑定，推入天空参数）
    if(m_CachedPhysSky){
        struct alignas(16){float4x4 invVP;float intensity;float3 sunDir;float turbidity;float groundAlbedo;float sunIntensity;float _pad;}pc;
        pc.invVP=invVP;pc.intensity=m_CachedPhysSky->intensity;
        pc.sunDir=m_CachedPhysSky->sunDirection;
        pc.turbidity=m_CachedPhysSky->turbidity;
        pc.groundAlbedo=m_CachedPhysSky->groundAlbedo;
        pc.sunIntensity=m_CachedPhysSky->sunIntensity;
        cmd->SetPipeline(m_PS_PSO.get());
        cmd->SetPushConstants(0,sizeof(pc),&pc);
        cmd->Draw(3);
        return;
    }
```

（`m_PSO.get()` 的 Cubemap 分支保持不变，作为无物理天空时的回退。）

- [ ] **Step 7: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（`PhysicalSky.frag` → SPIR-V 成功）。

Run: `build/bin/Debug/04.Deferred.exe`
Expected: 未添加 `PhysicalSkyComponent` 时走原 Cubemap 路径，画面不变（无回归）。在示例场景临时 `world.AddComponent<PhysicalSkyComponent>(e);` 后，天空从 Cubemap 变为解析蓝天 + 太阳盘；不同浑浊度（`turbidity` 1 vs 10）色调可区分。视觉确认由用户完成。

- [ ] **Step 8: Commit（先向用户确认）**

```bash
git add Engine/Scene/Scene/PhysicalSkyComponent.h Engine/Scene/Scene/PhysicalSkyComponent.cpp Engine/Scene/Scene/SceneReflect.cpp Engine/Scene/CMakeLists.txt Engine/Shader/Shaders/PhysicalSky.frag.slang Engine/Shader/CMakeLists.txt Engine/Render/PostProcess/SkyboxPass.h Engine/Render/PostProcess/SkyboxPass.cpp
git commit -m "Render: 物理天空（Preetham 解析模型）"
```

---

### Task 4: HDR 输出（HDR10 PQ）

> 全链路：RHI 格式枚举 → 格式转换 → SwapChain HDR10 色彩空间 → ToneMap PQ 编码 + BT.2020 色域映射。风险最高，务必逐处核对硬编码。

**Files:**
- Modify: `Engine/RHI/RHI/Types.h`（`Format` 枚举新增 `A2B10G10R10_UNORM_PACK32`）
- Modify: `Engine/RHI/RHI/SwapChain.h`（`SwapChainDesc` 新增 `bool hdr`；`IRHISwapChain` 新增 `GetColorFormat()`）
- Modify: `Engine/RHI/Vulkan/VulkanConverters.cpp`（`ToVkFormat` 映射）
- Modify: `Engine/RHI/Vulkan/VulkanResources.cpp`（`GetFormatByteSize`）
- Modify: `Engine/RHI/Vulkan/PSOPrecompileManager.cpp`（`ToVkFormat`）
- Modify: `Engine/RHI/Vulkan/VulkanPipeline.cpp`（`:183`/`:501` finalLayout 条件）
- Modify: `Engine/RHI/Vulkan/VulkanCommandList_RenderPass.cpp`（`:37` 硬编码 → swapchain 格式）
- Modify: `Engine/RHI/Vulkan/VulkanSwapChain.h/.cpp`（HDR10 色彩空间选择 + `GetColorFormat`）
- Modify: `Engine/Shader/Shaders/common.slang`（PQ 编码 + BT.2020 矩阵）
- Modify: `Engine/Shader/Shaders/ToneMap.frag.slang`（PQ 输出路径 + 开关 push constant）
- Modify: `Engine/Render/PostProcess/ToneMapPass.h/.cpp`（动态输出格式 + HDR 开关）
- Modify: `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp`（`:584/626/644` 硬编码 BGRA8 → 交换链格式）

**Interfaces:**
- Consumes: `ToVkFormat`（`VulkanConverters.cpp:15`）、`IRHISwapChain::GetBackendFormat`（`SwapChain.h:36`）、`ToneMapPass::GetOutputFormat`（`ToneMapPass.h:39`）。
- Produces: `rhi::Format::A2B10G10R10_UNORM_PACK32`、`IRHISwapChain::GetColorFormat()`、`ToneMapPass` 动态输出格式 + HDR 开关。

- [ ] **Step 1: `Format` 枚举 + 后端格式映射**

`Types.h:119`（`R11G11B10_FLOAT` 之后）新增：

```cpp
    R11G11B10_FLOAT,

    // HDR10（10-bit RGB + 2-bit alpha）
    A2B10G10R10_UNORM_PACK32,
```

`VulkanConverters.cpp:35` 之后新增映射：

```cpp
        case Format::A2B10G10R10_UNORM_PACK32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
```

`PSOPrecompileManager.cpp:118`（`BGRA8_UNORM` case 后）新增：

```cpp
    case Format::A2B10G10R10_UNORM_PACK32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
```

`VulkanResources.cpp:143`（`BGRA8_UNORM` 行）追加新格式到 4 字节分组：

```cpp
        case Format::BGRA8_UNORM:    case Format::BGRA8_SRGB:
        case Format::A2B10G10R10_UNORM_PACK32:                 return 4;
```

- [ ] **Step 2: `SwapChain.h` 新增 HDR 描述与格式查询**

`SwapChainDesc`（`:10`）新增字段：

```cpp
    bool    hdr             = false;   // HDR10 输出（A2B10G10R10 + ST.2084）
```

`IRHISwapChain`（`:36` 后）新增：

```cpp
    // RHI 颜色格式（供 ToneMap 等查询实际交换链格式，HDR 时为 A2B10G10R10）
    virtual Format GetColorFormat() const = 0;
```

- [ ] **Step 3: `VulkanSwapChain` HDR10 色彩空间选择**

`VulkanSwapChain.h`：新增 `m_ColorSpace` 成员 + `GetColorFormat()` 实现（返回 `m_Format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? Format::A2B10G10R10_UNORM_PACK32 : Format::BGRA8_UNORM`）。

`VulkanSwapChain.cpp` 的 `CreateSwapchain()`（`:37-44` 格式选择处）改为支持 HDR10（`desc.hdr` 优先选 `VK_FORMAT_A2B10G10R10_UNORM_PACK32` + `VK_COLOR_SPACE_HDR10_ST2084_EXT`，需 `VK_EXT_swapchain_colorspace` 扩展，不可用则回退 SDR）：

```cpp
    // 优先选择格式：HDR10（A2B10G10R10 + ST.2084）否则 BGRA8 sRGB
    m_Format = VK_FORMAT_B8G8R8A8_UNORM;
    m_ColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (m_HDR) {  // m_HDR = desc.hdr，构造时记录
        for (auto& f : formats) {
            if (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32
                && f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
                m_Format = f.format; m_ColorSpace = f.colorSpace; break;
            }
        }
    }
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
            && m_Format == VK_FORMAT_B8G8R8A8_UNORM) {  // 未命中 HDR10 时回退 SDR
            m_Format = f.format; m_ColorSpace = f.colorSpace; break;
        }
    }
```

并将 `swapInfo.imageColorSpace`（`:73`）改为 `m_ColorSpace`。

- [ ] **Step 4: 修正渲染通道/管线的格式硬编码**

`VulkanPipeline.cpp:183` 与 `:501` 的 `finalLayout` 条件加入新格式：

```cpp
        out.attachments[c].finalLayout = (desc.colorFormats[c] == Format::BGRA8_UNORM ||
                                           desc.colorFormats[c] == Format::BGRA8_SRGB  ||
                                           desc.colorFormats[c] == Format::A2B10G10R10_UNORM_PACK32)
                                          ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                          : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
```

`VulkanCommandList_RenderPass.cpp:37` 的硬编码改为从交换链取实际格式：

```cpp
            att[0].format = m_pSwapChain ? static_cast<VkFormat>(m_pSwapChain->GetBackendFormat()) : VK_FORMAT_B8G8R8A8_UNORM; // SwapChain 实际格式
```

- [ ] **Step 5: `common.slang` 新增 PQ 编码 + BT.2020 矩阵**

`common.slang` 末尾（`:43` 前）追加：

```hlsl
// ST.2084 (PQ) 编码：线性 HDR 亮度 → PQ 非线性（HDR10 传输函数）
// linNits 为绝对亮度（尼特），峰值 10000
float3 LinearToPQ(float3 linNits) {
    const float m1 = 0.1593017578125;  // 2610/16384
    const float m2 = 78.84375;          // 2523/32
    const float c1 = 0.8359375;         // 3424/4096
    const float c2 = 18.8515625;        // 2413/128
    const float c3 = 18.6875;           // 2392/128
    float3 y = max(linNits, 0.0) / 10000.0;
    float3 yPow = pow(y, float3(m1));
    return pow((c1 + c2 * yPow) / (1.0 + c3 * yPow), float3(m2));
}

// BT.709 → BT.2020 色域映射（HDR10 广色域）
float3 BT709ToBT2020(float3 c) {
    float3x3 m = float3x3(
        0.6274, 0.3293, 0.0433,
        0.0691, 0.9195, 0.0114,
        0.0164, 0.0880, 0.8956);
    return mul(m, c);
}
```

- [ ] **Step 6: `ToneMap.frag.slang` PQ 输出路径**

`ToneMap.frag.slang` 的 push constant（`:19`）改为含 HDR 开关与白点：

```hlsl
[[vk::push_constant]] cbuffer ToneMapPC {
    float exposure;      // 自动曝光值（默认 1.0 = 无调整）
    float hdrEnabled;    // 1=HDR10（PQ 输出），0=SDR（sRGB 输出）
    float whitePointNits;// HDR 参考白点（尼特），把相对线性亮度换算为绝对亮度
    float _pad;
};
```

`fragmentMain`（`:24-34`）改为双路径：

```hlsl
float4 fragmentMain(FSInput input) : SV_Target0 {
    float3 hdrColor = u_HDRTexture.Sample(u_HDRSampler, input.uv).rgb * exposure;

    if (hdrEnabled > 0.5) {
        // HDR10：ACES → BT.2020 广色域 → PQ 编码（10-bit 非线性）
        float3 mapped = ACESFilm(hdrColor);
        float3 rec2020 = BT709ToBT2020(mapped);
        float3 nits = rec2020 * whitePointNits;  // 相对亮度 → 绝对尼特
        return float4(LinearToPQ(nits), 1.0);
    }

    // SDR：ACES → sRGB（保持原有行为）
    float3 mapped = ACESFilm(hdrColor);
    return float4(LinearToSRGB(mapped), 1.0);
}
```

- [ ] **Step 7: `ToneMapPass` 动态输出格式 + HDR 开关**

`ToneMapPass.h`：`GetOutputFormat()`（`:39`）改为返回 `m_OutputFormat`；新增 `SetOutputFormat(rhi::Format)` 与 `SetHDREnabled(bool)`、`SetWhitePointNits(float)`；成员区新增 `rhi::Format m_OutputFormat = rhi::Format::BGRA8_UNORM; float m_HDR = 0.0f; float m_WhitePointNits = 80.0f;`。

`ToneMapPass.cpp`：PSO 创建（`:26`）的 `colorFormats[0]` 改为 `m_OutputFormat`；`Render()`（`:57`）的 push constant 结构改为含 `hdrEnabled/whitePointNits`，并从成员填充。

- [ ] **Step 8: `DeferredPipeline_FrameGraph.cpp` 解除 BGRA8 硬编码**

`:584`、`:626`、`:644` 的 `c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM)` 改为使用交换链格式。在 `BuildFrameGraph` 入口处取一次：

```cpp
    rhi::Format swapFmt = m_SwapChain ? m_SwapChain->GetColorFormat() : rhi::Format::BGRA8_UNORM;
```

三处改为 `c->BeginRenderPass(1, swapFmt);`。同时在 ToneMap Pass 处（`:552` 的 lambda 前）设置：

```cpp
            m_PostProcess.GetToneMap()->SetOutputFormat(swapFmt);
            m_PostProcess.GetToneMap()->SetHDREnabled(swapFmt == rhi::Format::A2B10G10R10_UNORM_PACK32 ? 1.0f : 0.0f);
```

- [ ] **Step 9: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（含新增格式映射与 PQ shader）。

Run: `build/bin/Debug/04.Deferred.exe`
Expected: 默认 SDR（`hdr=false`）下画面与改造前一致（无回归）。切换 HDR（构造 `SwapChainDesc` 处 `desc.hdr = true`）需显示器支持 HDR10，此时 10-bit 输出 + PQ 编码；不支持时回退 SDR 无崩溃。视觉确认由用户完成。

- [ ] **Step 10: Commit（先向用户确认）**

```bash
git add Engine/RHI/RHI/Types.h Engine/RHI/RHI/SwapChain.h Engine/RHI/Vulkan/VulkanConverters.cpp Engine/RHI/Vulkan/VulkanResources.cpp Engine/RHI/Vulkan/PSOPrecompileManager.cpp Engine/RHI/Vulkan/VulkanPipeline.cpp Engine/RHI/Vulkan/VulkanCommandList_RenderPass.cpp Engine/RHI/Vulkan/VulkanSwapChain.h Engine/RHI/Vulkan/VulkanSwapChain.cpp Engine/Shader/Shaders/common.slang Engine/Shader/Shaders/ToneMap.frag.slang Engine/Render/PostProcess/ToneMapPass.h Engine/Render/PostProcess/ToneMapPass.cpp Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp
git commit -m "Render: HDR10 输出（PQ 编码 + BT.2020）"
```

---

## 暂缓项（后续单独规划）

- **空中透视（大气消光）**：文档第 5 项第 4 条「基于距离的大气消光应用到场景对象」涉及 `pbr_common.slang` / `DeferredLighting.frag.slang` / `LightingPass`，与物理天空正交且改动面较大，建议物理天空落地后单独一轮规划。
- **太阳盘 → 方向光 illuminance 自动同步**：`PhysicalSkyComponent.sunDirection/sunIntensity` 已提供字段，但「自动设置 `DirectionalLight::illuminance`」需在世界层建立组件间同步，本轮以手动赋值 + 文档说明为准。
- **ForwardPipeline 的 `BeginRenderPass` 硬编码**（`ForwardPipeline_FrameGraph.cpp:216`、`HybridRTPipeline.cpp:913/930`、`PathTracingPipeline.cpp:562/578`）：本计划 Task 4 只改 Deferred 管线；其余管线在启用 HDR 前需同样替换为交换链格式。

---

## 验证清单（全部完成后复验）

1. 全量编译 `cmake --build build --config Debug` 零错误。
2. 默认参数（全效果关闭、SDR、无物理天空）下 04.Deferred / 02.Cube 渲染与改造前一致（无回归）。
3. sRGB 精确化后中间调无可见突变；镜头后处理四项效果开关均生效；物理天空蓝天/太阳盘/浑浊度可区分；HDR 开启后 PQ 输出无崩溃（视觉由用户确认）。
