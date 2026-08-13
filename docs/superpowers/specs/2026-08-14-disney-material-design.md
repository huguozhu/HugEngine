# Disney BSDF 材质扩展设计（7 参数）

> **日期**: 2026-08-14
> **状态**: 已批准
> **目标**: 为 `PBRMaterial` 增加 7 个 Disney principled BSDF 参数（anisotropic / subsurface / specular / specularTint / sheen / clearcoat / clearcoatGloss），并在 `pbr_common.slang` 的 `PBR_BRDF` 中实现对应 BRDF 项，默认值还原为当前 glTF metallic/roughness 行为。

## 背景

当前仅支持 glTF 2.0 金属/粗糙度模型（`PBRMaterial` 的 baseColor/metallic/roughness/ao/emissive/alpha + `ior`）。规划文档列出 7 个缺失的 Disney BSDF 参数，用于支持拉丝金属、皮肤、天鹅绒、车漆等材质。

## 参数定义与默认值

| 参数 | 类型 | 默认 | 语义 |
|------|------|------|------|
| `anisotropic` | float | 0.0 | 各向异性强度（0=各向同性 GGX） |
| `subsurface` | float | 0.0 | 次表面散射混合（0=纯 Lambert） |
| `specular` | float | 0.5 | 镜面强度（F0 = 0.16 × specular²，Disney 约定） |
| `specularTint` | float3 | (1,1,1) | 镜面色调（彩色反射） |
| `sheen` | float | 0.0 | 光泽（天鹅绒边缘柔光） |
| `clearcoat` | float | 0.0 | 清漆层强度 |
| `clearcoatGloss` | float | 1.0 | 清漆粗糙度 |

**默认还原**：默认值下 `F0 = 0.16×0.5² = 0.04`（与现有 `ior=1.5` 默认一致），各扩展项为 0，BRDF 退化为当前 glTF metallic/roughness。

**specular 与 ior 的关系**：二者都控制电介质 F0。实现时 `specular` 优先——当 `specular != 0.5` 时用 `F0 = 0.16×specular²` 覆盖 `ior` 派生的 `dielectricF0`；否则沿用 `dielectricF0`。

## 存储布局

`GPUObjectData`（`ShaderTypes.slang`）由 128 字节扩展到 176 字节（float4 16 字节对齐），新增 2 个 `float4` + 1 个 `float` 打包 7 参数：

```cpp
GPU_STRUCT GPUObjectData {
    // ... 原 128 字节字段不变 ...
    float4   disneyA;   // x=anisotropic, y=subsurface, z=specular, w=sheen
    float4   disneyB;   // x=clearcoat, y=clearcoatGloss, z=specularTint.r, w=specularTint.g
    float    disneyC;   // specularTint.b
    GPU_UINT _pad;      // 对齐到 160
};
```

同步更新：`Material.h` 的 `static_assert(sizeof(GPUObjectData)==176)`、`PBRMaterial` 新增 7 字段、`FillObjectData` 填充。

## BRDF 扩展（`pbr_common.slang` `PBR_BRDF`）

在现有 Cook-Torrance 基础上叠加（`PBR_BRDF` 新增 `GPUObjectData` 的迪士尼参数或打包的 `float4/float` 形参）：

1. **specular/specularTint**：`f0 = lerp(float3(0.16×specular²), albedo, metallic) × specularTint`（电介质）
2. **clearcoat + clearcoatGloss**：第二层镜面（固定 IOR 1.5 的 GGX 层），`F_cc = 0.04 + 0.96×F_Schlick`，层叠在基底上（能量按 `clearcoat` 混合）
3. **sheen**：`sheenColor = specularTint`，边缘项 `F_sheen = sheen × (1-HdotV)^5 × albedo`，加到漫反射
4. **anisotropic**：各向异性 GGX 的 D 项（`aspect = sqrt(1 - 0.9×anisotropic)`，切线/副切线两方向粗糙度）
5. **subsurface**：`diffuse = mix(Lambert, wrapDiffuse(NdotL→NdotL_wrapped), subsurface)`（次表面近似）

**改动文件**：`Material.h` + `ShaderTypes.slang` + `pbr_common.slang`（+ 调用点透传迪士尼参数），约 200 行。

## 文件变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `Engine/Render/Pipeline/Material.h` | 修改 | `PBRMaterial` 加 7 字段 + `FillObjectData` 填充 + static_assert 改 160 |
| `Engine/Shader/Shaders/ShaderTypes.slang` | 修改 | `GPUObjectData` 加 disneyA/disneyB/disneyC |
| `Engine/Shader/Shaders/pbr_common.slang` | 修改 | `PBR_BRDF` 迪士尼 BRDF 项 |
| `Engine/Shader/Shaders/PBR.frag.slang` | 修改 | 透传迪士尼参数 |
| `Engine/Shader/Shaders/DeferredLighting.frag.slang` | 修改 | 透传迪士尼参数（GBuffer 扩展） |

## 验证

1. 编译通过（Slang shader + C++）。
2. 默认值（7 参数全默认）渲染与改造前一致（无回归）。
3. 手动设 clearcoat>0 显示清漆高光；sheen>0 显示天鹅绒边缘；anisotropic>0 显示拉丝金属。
4. 视觉确认由用户完成。
