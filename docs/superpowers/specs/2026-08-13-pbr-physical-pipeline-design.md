# 物理渲染管线剩余缺口设计（高优先级 4 项）

> **日期**: 2026-08-13
> **状态**: 已批准
> **目标**: 完成 `docs/未实现功能/基于物理的渲染管线剩余缺口.md` 中的高优先级 4 项（物理正确性闭环）：BRDF 多重散射补偿、亮度校准（参考白点）、正向管线物理单位适配、F0 可配置 IOR。

## 背景

规划文档列出 10 个缺口项，其中高优先级 4 项直接影响物理正确性且改动小（合计 ~125 行）。物理相机与物理光源已实现（`c12deb4` / `6c24023`），本文档补齐 BRDF 能量守恒、显示亮度锚定、正向管线物理单位与电介质反射率，形成完整物理管线闭环。

**已探明现状：**
- `pbr_common.slang`：Cook-Torrance GGX，`f0` 硬编码 `float3(0.04)`，无多重散射补偿
- `AutoExposurePass`：`m_TargetLum=0.18f`，`exposure = targetLum / avgLum`（`AutoExposurePass.cpp:89`），无尼特锚定
- `ForwardPipeline::CollectLights`（`ForwardPipeline.cpp:488`）：`gl.colorIntensity = float4(lc.color, lc.intensity)`，未读物理字段
- `Material.h PBRMaterial`：glTF 2.0 字段齐全，无 `ior`
- `GPUObjectData`（128B，`ShaderTypes.slang:144`）：有 `_pad[2]`（[120..128] 共 8 字节）可复用

---

## 一、BRDF 多重散射补偿

### 现状

`pbr_common.slang` 的 `PBR_BRDF()` 仅做单次散射（微面元单次反弹），粗糙表面（roughness>0.5）丢失多次反弹能量，White Furnace 测试输出显著小于 1.0。

### 方案

在 `PBR_BRDF()` 末尾叠加 Kulla-Conty 能量补偿项（Frostbite 拟合），方向反照率**复用现有 split-sum BRDF LUT**（`u_BRDF_LUT`）计算，**不新增 LUT**：

方向反照率 `E(f0) = f0 × envBRDF.x + envBRDF.y`——split-sum 的 scale/bias 项正好是 Schlick Fresnel 分解 `F = f0·(1-Fc) + Fc` 的两项，故 `E(f0)` 即方向反照率。

```hlsl
// 调用方（PBR.frag / DeferredLighting.frag 已采样 u_BRDF_LUT 供 IBL）：
float2 envBRDF = u_BRDF_LUT.Sample(u_IBLSampler, float2(NdotV, roughness)).rg;
float3 directionalAlbedo = f0 * envBRDF.x + envBRDF.y;   // 方向反照率

// PBR_BRDF 末尾（directionalAlbedo 由参数传入）：
float3 energyCompensation = 1.0 + f0 * (1.0 / max(directionalAlbedo, 0.001) - 1.0);
specular *= energyCompensation;
```

**收益**：粗糙金属在明亮环境亮度恢复 ~40-50%。

**改动文件**：`pbr_common.slang` + `PBR.frag.slang` + `DeferredLighting.frag.slang`（采样 LUT 传入），约 40 行。

> 说明：方向反照率 `E(f0) = f0×A + B`（A/B 为 split-sum LUT 的 scale/bias 通道），可直接由现有 `u_BRDF_LUT` 得出，无需新增 LUT。

---

## 二、亮度校准（参考白点）

### 现状

`AutoExposurePass` 目标 `m_TargetLum=0.18` 是归一化无单位值，未映射到真实显示器亮度（尼特），物理光源的 cd/lux 值无法锚定到显示输出。

### 方案

1. `AutoExposurePass` 新增 `m_DisplayWhitePoint = 80.0f`（SDR 参考白点，尼特）+ getter/setter。
2. 曝光计算改为将场景平均亮度映射到「18% 灰 = 白点 × 0.18」：

```cpp
// 当前: m_Exposure = targetLum / avgLum;  (targetLum=0.18 归一化)
// 改为: 场景平均亮度(尼特) → 白点 × 18%
m_Exposure = (m_DisplayWhitePoint * m_TargetLum) / avgLum;
m_Exposure = std::max(kMinExposure, std::min(m_Exposure, kMaxExposure));
```

3. 新增 CVar `r.AutoExposure.WhitePoint`（默认 80）运行时调参。
4. `ToneMap.frag.slang`：白点归一化（ACES 映射后除以白点，或等效地将白点折入 exposure）。

**收益**：物理光源的 cd/lux 值经 BRDF → 传感器照度 → 曝光 → 显示亮度（尼特）建立物理联系。

**改动文件**：`AutoExposurePass.h/.cpp` + `ToneMap.frag.slang` + `ToneMapPass.cpp`（透传 exposure），约 50 行。

> **公式修正**：规划文档原文 `exposure = whitePoint / (avgLum × targetLum)` 量纲有误（targetLum 在分母会反向）。正确映射为 `exposure = (whitePoint × targetLum) / avgLum`。实现时按此修正。

---

## 三、正向管线物理单位适配

### 现状

`ForwardPipeline::CollectLights` 对所有光源统一 `gl.colorIntensity = float4(lc.color, lc.intensity)`，未读取 `luminousIntensity`/`illuminance` 物理字段，也未做色温→RGB 处理。

### 方案

镜像 `DeferredPipeline::CollectLights`（`DeferredPipeline.cpp:363-433`）的物理模式：

```cpp
// 色温 → RGB（叠加到 color 滤镜色）
float3 lightColor = lc.color;
if (lc.colorTemperature > 0.0f) lightColor *= render::KelvinToRGB(lc.colorTemperature);
gl.colorIntensity = float4(lightColor, lc.intensity);

// 物理模式（positionRange.w < 0 标记，shader 用 abs() 取实际值）
// Directional: illuminance>0 → colorIntensity.w = illuminance, positionRange.w = -1
// Point/Spot:  luminousIntensity>0 → colorIntensity.w = luminousIntensity, positionRange.w = -(range)
```

**改动文件**：`ForwardPipeline.cpp`，约 20 行。

**收益**：ForwardPipeline 也支持物理光源（与 Deferred 行为一致）。

---

## 四、F0 可配置（IOR → 镜面反射率）

### 现状

所有电介质硬编码 `f0 = 0.04`（4% 垂直反射率），无法区分水（IOR 1.33→F0 0.02）、玻璃（1.5→0.04）、宝石（2.4→0.17）。

### 方案

1. `PBRMaterial`（`Material.h`）新增 `float ior = 1.5f`（电介质折射率）。
2. CPU 端 `FillObjectData()` 预计算标量 `dielectricF0 = (ior-1)²/(ior+1)²`，复用 `GPUObjectData` 的 `_pad[2]` 空隙（**128B 布局不变**）。
3. `ShaderTypes.slang`：`GPUObjectData` 的 `_pad[0]` 改名 `dielectricF0`（float），`_pad[1]` 保留。
4. `pbr_common.slang`：`float3 f0 = lerp(float3(dielectricF0), albedo, metallic)` 替代硬编码 `0.04`。

**改动文件**：`Material.h` + `ShaderTypes.slang` + `pbr_common.slang`（+ `GBuffer.frag.slang` 若单独构造 f0），约 15 行。

**收益**：水/玻璃/宝石等电介质获得正确反射率。

---

## 五、文件变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `Engine/Shader/Shaders/pbr_common.slang` | 修改 | 多重散射补偿 + DirectionalAlbedo + f0 用 dielectricF0 |
| `Engine/Render/PostProcess/AutoExposurePass.h/.cpp` | 修改 | m_DisplayWhitePoint + 曝光公式 + CVar |
| `Engine/Shader/Shaders/ToneMap.frag.slang` | 修改 | 白点归一化 |
| `Engine/Render/Pipeline/ForwardPipeline.cpp` | 修改 | CollectLights 物理模式 |
| `Engine/Render/Pipeline/Material.h` | 修改 | PBRMaterial 新增 ior + FillObjectData 预计算 F0 |
| `Engine/Shader/Shaders/ShaderTypes.slang` | 修改 | GPUObjectData padding 复用为 dielectricF0 |

---

## 六、验证

1. 编译通过（RHI + Render + 全部 shader 重新编译）。
2. 运行 04.Deferred：
   - 粗糙金属（roughness>0.7）在明亮 IBL 环境下亮度明显恢复（多重散射补偿生效）
   - `set r.AutoExposure.WhitePoint 80/120` → 整体亮度随之变化
   - ForwardPipeline（02.Cube）物理光源与 Deferred 行为一致
   - 不同 IOR 材质（如宝石 IOR 2.4）反射率明显高于默认（0.04）
3. 回归：默认 `ior=1.5`（F0=0.04）时渲染结果与改造前一致；默认白点 80 nits 时曝光行为不突变。
