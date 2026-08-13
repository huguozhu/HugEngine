# 物理渲染管线剩余缺口（高优先级 4 项）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 完成物理渲染管线高优先级 4 项：F0 可配置 IOR、BRDF 多重散射补偿、正向管线物理单位适配、亮度校准（参考白点）。

**Architecture:** 电介质 F0 从 IOR 预计算并塞入 `GPUObjectData` 现有 padding；多重散射补偿复用现有 split-sum BRDF LUT（`E(f0)=f0×A+B`）；正向管线镜像 Deferred 的物理光源编码；曝光锚定到 SDR 参考白点 80 nits。

**Tech Stack:** C++17 / Slang shader / Vulkan（CMake MSVC 2026）。

## Global Constraints

- **逐功能验证门控**：每个任务只实现一个功能，编译 + 运行验证通过后才进入下一个。任何任务未通过验证不得继续。
- **Commit 规则（CLAUDE.md）**：不自动 `git commit`；提交前征得用户确认。中文 commit log，不含 AI 信息。
- **代码注释**：新增代码附中文注释。
- **构建命令**：`cmake --build build --config Debug`（含 Slang→SPIR-V 编译；shader 语法错误在编译期暴露）。
- **运行验证**：`build/bin/Debug/04.Deferred.exe`（Deferred）与 `build/bin/Debug/02.Cube.exe`（Forward）。视觉确认由用户完成，自动化验证 = 编译通过 + 运行无崩溃/无 shader 报错。
- **shader 改动注意**：`pbr_common.slang` 的 `PBR_BRDF` 被多处调用（PBR.frag / DeferredLighting.frag / ReSTIR_*.comp / PT_Full.rgen），签名改动必须用**默认参数**向后兼容，避免破坏 ReSTIR/PT 调用点。

---

### Task 1: F0 可配置（IOR → 镜面反射率）

**Files:**
- Modify: `Engine/Render/Pipeline/Material.h`（`PBRMaterial` 新增 `ior` + `FillObjectData` 预计算 F0）
- Modify: `Engine/Shader/Shaders/ShaderTypes.slang`（`GPUObjectData._pad[0]` 改名 `dielectricF0`）
- Modify: `Engine/Shader/Shaders/pbr_common.slang`（`PBR_BRDF` 新增 `dielectricF0` 默认参数）
- Modify: `Engine/Shader/Shaders/PBR.frag.slang`（传入 `obj.dielectricF0` + IBL F0）
- Modify: `Engine/Shader/Shaders/DeferredLighting.frag.slang`（同上）

**Interfaces:**
- Consumes: `PBRMaterial`（`Material.h:63`）、`GPUObjectData`（`ShaderTypes.slang:144`，`_pad[2]` 在 [120..128]）。
- Produces: `float PBRMaterial::ior = 1.5f`、`GPUObjectData.dielectricF0`、`PBR_BRDF(..., float dielectricF0 = 0.04)`（Task 2 依赖）。

- [ ] **Step 1: `Material.h` 新增 `ior` 字段**

在 `PBRMaterial`（`Material.h:63`）的 `aoFactor` 之后新增：

```cpp
    float    ior                  = 1.5f;   // 电介质折射率（F0 = (ior-1)^2/(ior+1)^2）
```

- [ ] **Step 2: `Material.h` 的 `FillObjectData` 预计算 F0**

在 `FillObjectData`（`Material.h:96`）中，`obj.aoFactor = mat.aoFactor;` 之后新增：

```cpp
    // 电介质 F0（标量）：由 IOR 预计算，替代硬编码 0.04
    float ior = mat.ior;
    obj.dielectricF0 = (ior - 1.0f) * (ior - 1.0f) / ((ior + 1.0f) * (ior + 1.0f));
```

- [ ] **Step 3: `ShaderTypes.slang` 复用 padding**

将 `GPUObjectData`（`ShaderTypes.slang:144-155`）的 `_pad[0]` 改为 `dielectricF0`：

```cpp
    GPU_UINT materialFlags;         // [112]
    GPU_UINT materialID;            // [116]     bindless 纹理基索引
    float    dielectricF0;          // [120]     电介质 F0（由 IOR 预计算，默认 0.04）
    GPU_UINT _pad;                  // [124..128] 对齐
```

- [ ] **Step 4: `pbr_common.slang` 用 `dielectricF0`**

`PBR_BRDF`（`pbr_common.slang:126`）签名新增默认参数，`f0` 计算改用之：

```cpp
float3 PBR_BRDF(float3 albedo, float metallic, float roughness,
                float3 N, float3 V, float3 L,
                float dielectricF0 = 0.04)  // 默认 0.04 保持 ReSTIR/PT 调用点不变
{
    ...
    // 菲涅尔反射率：电介质 = dielectricF0（由 IOR 预计算），金属 = albedo
    float3 f0 = lerp(float3(dielectricF0), albedo, metallic);
    ...
}
```

- [ ] **Step 5: `PBR.frag.slang` 与 `DeferredLighting.frag.slang` 传入 F0**

将两处 `PBR_BRDF(albedo, metallic, roughness, N, V, L)` 调用改为传入 `obj.dielectricF0`（`PBR.frag.slang:124`、`DeferredLighting.frag.slang:164,209`）：

```cpp
PBR_BRDF(albedo, metallic, roughness, N, V, L, obj.dielectricF0)
```

并将两处 IBL 的 F0 硬编码改为材质 F0（`PBR.frag.slang:177`、`DeferredLighting.frag.slang:217`）：

```cpp
float3 F0_ibl = lerp(float3(obj.dielectricF0), albedo, metallic);
```

- [ ] **Step 6: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（Slang shader + C++ 零错误）。

Run: `build/bin/Debug/04.Deferred.exe` + `build/bin/Debug/02.Cube.exe`（各运行数秒后退出）
Expected: 默认 `ior=1.5`（F0=0.04）时渲染与改造前一致；无崩溃、无 shader 报错。

- [ ] **Step 7: Commit（先向用户确认）**

```bash
git add Engine/Render/Pipeline/Material.h Engine/Shader/Shaders/ShaderTypes.slang Engine/Shader/Shaders/pbr_common.slang Engine/Shader/Shaders/PBR.frag.slang Engine/Shader/Shaders/DeferredLighting.frag.slang
git commit -m "Render: 电介质 F0 可配置（IOR）"
```

---

### Task 2: BRDF 多重散射补偿

**Files:**
- Modify: `Engine/Shader/Shaders/pbr_common.slang`（`PBR_BRDF` 新增 `envBRDF` 参数 + 能量补偿）
- Modify: `Engine/Shader/Shaders/PBR.frag.slang`（采样 LUT 传入）
- Modify: `Engine/Shader/Shaders/DeferredLighting.frag.slang`（采样 LUT 传入）

**Interfaces:**
- Consumes: `u_BRDF_LUT`（`kGPUBinding_BRDF_LUT`，两 shader 已绑定）、`PBR_BRDF(..., float dielectricF0)`（Task 1）。
- Produces: `PBR_BRDF(..., float dielectricF0 = 0.04, float2 envBRDF = float2(0,1))`。

- [ ] **Step 1: `pbr_common.slang` 新增能量补偿**

`PBR_BRDF` 签名追加 `float2 envBRDF = float2(0.0, 1.0)` 参数，在 specular 计算后叠加补偿：

```cpp
float3 PBR_BRDF(float3 albedo, float metallic, float roughness,
                float3 N, float3 V, float3 L,
                float dielectricF0 = 0.04, float2 envBRDF = float2(0.0, 1.0))
{
    ...
    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, HE_EPSILON);

    // ── 多重散射能量补偿（Kulla-Conty / Frostbite）──
    // 方向反照率 E(f0) = f0 * A + B（split-sum LUT 的 scale/bias 即 Schlick Fresnel 分解的两项）
    // envBRDF 由调用方采样 u_BRDF_LUT(NdotV, roughness) 传入；默认 (0,1) 表示无补偿
    float3 directionalAlbedo = f0 * envBRDF.x + envBRDF.y;
    float3 energyCompensation = 1.0 + f0 * (1.0 / max(directionalAlbedo, 0.001) - 1.0);
    specular *= energyCompensation;
    ...
}
```

- [ ] **Step 2: `PBR.frag.slang` 采样并传入**

在调用 `PBR_BRDF` 前采样 LUT（若尚未有 `envBRDF` 局部量；`PBR.frag.slang:187` 已有 `float2 envBRDF = u_BRDF_LUT.Sample(...)`），将其传入：

```cpp
float3 brdf = PBR_BRDF(albedo, metallic, roughness, N, V, L, obj.dielectricF0, envBRDF);
```

- [ ] **Step 3: `DeferredLighting.frag.slang` 采样并传入**

`DeferredLighting.frag.slang:223` 已有 `float2 envBRDF = u_BRDF_LUT.Sample(...)`。两处 `PBR_BRDF(...)` 调用（`:164, :209`）传入：

```cpp
color += PBR_BRDF(albedo, metallic, roughness, N, V, L, gbuffer.dielectricF0, envBRDF) * radiance * shadow;
```

（`gbuffer.dielectricF0` 为 GBuffer 中采样到的材质 F0；若 GBuffer 未存 F0，则回退为固定 0.04，需与 Task 1 的 GBuffer 写入对齐——见下方注意。）

- [ ] **Step 4: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过。

Run: `build/bin/Debug/04.Deferred.exe`
Expected: 粗糙金属（roughness>0.7）在明亮 IBL 下亮度明显恢复；无崩溃。

- [ ] **Step 5: Commit（先向用户确认）**

```bash
git add Engine/Shader/Shaders/pbr_common.slang Engine/Shader/Shaders/PBR.frag.slang Engine/Shader/Shaders/DeferredLighting.frag.slang
git commit -m "Render: BRDF 多重散射能量补偿"
```

> **注意**：Deferred 管线的 GBuffer 是否存储 dielectricF0 需在 Task 1/2 实现时确认。若 GBuffer 未存，则 Task 1 需同步在 GBuffer 写入 F0 或 IOR（改动扩展到 `GBuffer.frag.slang` / `GBufferPushConstant` / `GBufferRenderer`），实现时以「Deferred 材质 F0 能正确传入 `DeferredLighting.frag`」为准。

---

### Task 3: 正向管线物理单位适配

**Files:**
- Modify: `Engine/Render/Pipeline/ForwardPipeline.cpp`（`CollectLights` 物理模式）

**Interfaces:**
- Consumes: `render::KelvinToRGB`（`PhysicalLight.h:18`）、`LightComponent::illuminance/luminousIntensity`（`LightComponent.h:37-38`）。
- Produces: ForwardPipeline 物理光源编码（与 Deferred 一致）。

- [ ] **Step 1: 引入 PhysicalLight 头**

在 `ForwardPipeline.cpp` 顶部 include 区新增（若未包含）：

```cpp
#include "Pipeline/PhysicalLight.h"  // render::KelvinToRGB
```

- [ ] **Step 2: `CollectLights` 镜像 Deferred 物理模式**

将 `ForwardPipeline.cpp:482-513` 的 `collectLight` lambda 改为（对齐 `DeferredPipeline.cpp:363-433`）：

```cpp
    auto collectLight = [&](he::Entity e, he::LightComponent& lc) {
        if (!lc.enabled) return;
        u32 i = pc.lightCount;
        if (i >= MAX_LIGHTS) return;

        // 色温 → RGB（叠加到 color 滤镜色）
        float3 lightColor = lc.color;
        if (lc.colorTemperature > 0.0f) lightColor *= render::KelvinToRGB(lc.colorTemperature);

        GPULight gl{};
        gl.colorIntensity  = float4(lightColor, lc.intensity);
        gl.shadowIndex     = m_ShadowSystem->GetShadowIndex(e);

        switch (lc.type) {
        case he::LightType::Directional: {
            auto* dl = static_cast<he::DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f);
            gl.positionRange = float4(0, 0, 0, 0);
            if (lc.illuminance > 0.0f) {          // 物理模式：照度 lux
                gl.colorIntensity.w = lc.illuminance;
                gl.positionRange.w   = -1.0f;
            }
            break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            gl.positionRange = float4(pos, pl->range);
            gl.directionType = float4(0, -1, 0, 1.0f);
            if (lc.luminousIntensity > 0.0f) {    // 物理模式：发光强度 cd
                gl.colorIntensity.w = lc.luminousIntensity;
                gl.positionRange.w   = -(pl->range);
            }
            break;
        }
        case he::LightType::Spot: {
            auto* sl = static_cast<he::SpotLight*>(&lc);
            float3 pos = sg.GetWorldPosition(e);
            float r = lc.luminousIntensity > 0.0f ? -(sl->range) : sl->range;
            gl.positionRange = float4(pos, r);
            gl.directionType = float4(sl->direction, 2.0f);
            gl.coneAngles   = float2(sl->innerConeAngle, sl->outerConeAngle);
            if (lc.luminousIntensity > 0.0f) gl.colorIntensity.w = lc.luminousIntensity;
            break;
        }
        }

        GPULight* lights = static_cast<GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
        if (lights) lights[i] = gl;
        m_LightBuffers[m_CurrentFrameSlot]->Unmap();
        pc.lightCount++;
    };
```

- [ ] **Step 3: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过。

Run: `build/bin/Debug/02.Cube.exe`
Expected: 物理光源模式下 Forward 与 Deferred 行为一致；无崩溃。

- [ ] **Step 4: Commit（先向用户确认）**

```bash
git add Engine/Render/Pipeline/ForwardPipeline.cpp
git commit -m "Render: ForwardPipeline 物理光源单位适配"
```

---

### Task 4: 亮度校准（参考白点）

**Files:**
- Modify: `Engine/Render/PostProcess/AutoExposurePass.h`（`m_DisplayWhitePoint` + getter/setter）
- Modify: `Engine/Render/PostProcess/AutoExposurePass.cpp`（曝光公式 + CVar）
- Modify: `Engine/Shader/Shaders/ToneMap.frag.slang`（若需白点归一化）

**Interfaces:**
- Consumes: `AutoExposurePass`（`m_TargetLum=0.18f`，`AutoExposurePass.cpp:89` 曝光公式）。
- Produces: `float m_DisplayWhitePoint = 80.0f` + `GetDisplayWhitePoint/SetDisplayWhitePoint`。

- [ ] **Step 1: `AutoExposurePass.h` 新增白点参数**

在 `m_TargetLum` 之后新增：

```cpp
    float GetDisplayWhitePoint() const { return m_DisplayWhitePoint; }
    void  SetDisplayWhitePoint(float w) { m_DisplayWhitePoint = w; }
```

private 成员区新增：

```cpp
    float m_DisplayWhitePoint = 80.0f;  // SDR 参考白点（尼特），默认 80
```

- [ ] **Step 2: `AutoExposurePass.cpp` 曝光公式 + CVar**

将曝光计算（`AutoExposurePass.cpp:89`）：

```cpp
m_Exposure = exp2(log2(m_TargetLum) - m_PrevLogLum);   // 旧: targetLum / avgLum
```

改为（场景平均亮度映射到白点 × 18%）：

```cpp
// 场景平均亮度(尼特) → 白点 × 18% 灰；avgLum = exp2(m_PrevLogLum)
float avgLum = exp2(m_PrevLogLum);
m_Exposure = (m_DisplayWhitePoint * m_TargetLum) / avgLum;
m_Exposure = std::max(kMinExposure, std::min(m_Exposure, kMaxExposure));
```

新增 CVar（文件顶部静态量 + 每帧读取）：

```cpp
static float cvAutoExposureWhitePoint = 80.0f;  // r.AutoExposure.WhitePoint
```

在 `Render()` 开头读取：`m_DisplayWhitePoint = cvAutoExposureWhitePoint;`

- [ ] **Step 3: `ToneMap.frag.slang` 白点归一化（若需要）**

若曝光已把白点折入（avgLum → 14.4 nits），`ToneMap.frag.slang` 的 `ACESFilm` 后需除以白点归一化（或等效地将白点传入 push constant 在 shader 内处理）。实现时确认：默认白点 80 下画面亮度不突变。若曝光公式已保证亮度不变，则本步可跳过。

- [ ] **Step 4: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过。

Run: `build/bin/Debug/04.Deferred.exe`
Expected: 默认白点 80 下亮度与改造前接近（无突变）；`set r.AutoExposure.WhitePoint 120` 后整体变亮。最终视觉校准由用户确认。

- [ ] **Step 5: Commit（先向用户确认）**

```bash
git add Engine/Render/PostProcess/AutoExposurePass.h Engine/Render/PostProcess/AutoExposurePass.cpp Engine/Shader/Shaders/ToneMap.frag.slang
git commit -m "Render: 自动曝光参考白点校准"
```

---

## 验证清单（全部完成后复验）

1. 全量编译 `cmake --build build --config Debug` 零错误。
2. 默认参数（ior=1.5、白点 80）下 04.Deferred / 02.Cube 渲染与改造前一致（无回归）。
3. 不同 IOR 材质反射率可区分；粗糙金属多重散射亮度恢复；Forward 物理光源与 Deferred 一致；白点 CVar 调参生效。
