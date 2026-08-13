# Disney BSDF 材质扩展实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 `PBRMaterial` 增加 7 个 Disney BSDF 参数并在 `PBR_BRDF` 实现对应 BRDF 项，默认值还原 glTF 行为。

**Architecture:** `GPUObjectData` 128B→160B 装 7 参数；`pbr_common.slang` 的 `PBR_BRDF` 叠加 clearcoat/sheen/anisotropy/subsurface/specular-specularTint；Forward 直接透传，Deferred 经 GBuffer 扩展透传。

**Tech Stack:** C++17 / Slang / Vulkan。

## Global Constraints

- **逐功能验证门控**：每任务编译+运行验证通过后才进入下一个。
- **Commit 规则**：不自动 commit，提交前征得用户确认。中文 log，无 AI 信息。
- **代码注释**：新增代码附中文注释。
- **构建**：`cmake --build build --config Debug`（Slang 编译期报 shader 错）。
- **运行**：`02.Cube.exe`（Forward）/ `04.Deferred.exe`（Deferred）。视觉确认由用户完成。
- **默认还原**：7 参数默认值下 BRDF 与当前 glTF metallic/roughness 一致（specular=0.5→F0=0.04）。

---

### Task 1: 材质字段 + GPUObjectData 扩展

**Files:**
- Modify: `Engine/Render/Pipeline/Material.h`（`PBRMaterial` 加 7 字段 + `FillObjectData` 填充 + static_assert 改 160）
- Modify: `Engine/Shader/Shaders/ShaderTypes.slang`（`GPUObjectData` 加 disneyA/disneyB/disneyC）

**Interfaces:**
- Consumes: `PBRMaterial`、`GPUObjectData`（当前 128B，`Material.h:51` static_assert）。
- Produces: 7 个 `float` 字段 + `GPUObjectData.disneyA/disneyB/disneyC`（Task 2/3 依赖）。

- [ ] **Step 1: `Material.h` `PBRMaterial` 新增 7 字段**

在 `PBRMaterial`（`Material.h:63`）的 `ior` 之后新增：

```cpp
    // ── Disney principled BSDF 扩展参数（默认值还原 glTF metallic/roughness）──
    float    anisotropic       = 0.0f;   // 各向异性强度（0=各向同性）
    float    subsurface        = 0.0f;   // 次表面散射混合（0=纯 Lambert）
    float    specular          = 0.5f;   // 镜面强度（F0 = 0.16 * specular²，0.5→0.04）
    float3   specularTint      = float3(1.0f);  // 镜面色调
    float    sheen             = 0.0f;   // 光泽（天鹅绒边缘）
    float    clearcoat         = 0.0f;   // 清漆层强度
    float    clearcoatGloss    = 1.0f;   // 清漆粗糙度
```

- [ ] **Step 2: `Material.h` `FillObjectData` 填充**

在 `FillObjectData`（`Material.h:96`）末尾（`obj.materialFlags = flags;` 之后）新增：

```cpp
    // Disney 参数打包（与 ShaderTypes.slang GPUObjectData 布局一致）
    obj.disneyA = float4(mat.anisotropic, mat.subsurface, mat.specular, mat.sheen);
    obj.disneyB = float4(mat.clearcoat, mat.clearcoatGloss, mat.specularTint.x, mat.specularTint.y);
    obj.disneyC = mat.specularTint.z;
```

- [ ] **Step 3: `ShaderTypes.slang` 扩展 `GPUObjectData`**

在 `GPUObjectData`（`ShaderTypes.slang:144`）末尾（`_pad[2]` 处）改为：

```cpp
    GPU_UINT materialFlags;         // [112]
    GPU_UINT materialID;            // [116]     bindless 纹理基索引
    float    dielectricF0;          // [120]     电介质 F0（由 IOR 预计算）
    float4   disneyA;               // [128]     x=anisotropic, y=subsurface, z=specular, w=sheen
    float4   disneyB;               // [144]     x=clearcoat, y=clearcoatGloss, z=specularTint.r, w=specularTint.g
    float    disneyC;               // [160]     specularTint.b
    GPU_UINT _pad;                  // [164..168] 对齐到 168 → 取整 176（std430 float4 对齐）
```

（注：std430 下 `float4` 需 16 字节对齐，`disneyA` 从 [128] 起已对齐；最终结构尺寸按 `std430` 实际对齐为准，更新 `Material.h:51` 的 static_assert 为实际 `sizeof(GPUObjectData)`。）

- [ ] **Step 4: 更新 static_assert**

将 `Material.h:51` 的 `static_assert(sizeof(GPUObjectData) == 128, ...)` 改为实际新尺寸（编译后按报错提示填写，预期 176）：

```cpp
static_assert(sizeof(GPUObjectData)   == 176, "GPUObjectData must be 176 bytes");
```

- [ ] **Step 5: 编译验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（struct 尺寸对齐无误）。运行 02.Cube/04.Deferred 无回归（默认参数下行为不变）。

- [ ] **Step 6: Commit（先向用户确认）**

```bash
git add Engine/Render/Pipeline/Material.h Engine/Shader/Shaders/ShaderTypes.slang
git commit -m "Render: Disney 材质参数字段与 GPUObjectData 扩展"
```

---

### Task 2: Disney BRDF 实现 + Forward 透传

**Files:**
- Modify: `Engine/Shader/Shaders/pbr_common.slang`（`PBR_BRDF` 迪士尼 BRDF 项）
- Modify: `Engine/Shader/Shaders/PBR.frag.slang`（透传迪士尼参数）

**Interfaces:**
- Consumes: `GPUObjectData.disneyA/B/C`（Task 1）、`PBR_BRDF`（现签名含 dielectricF0/envBRDF）。
- Produces: `PBR_BRDF` 新增迪士尼参数（打包 `float4 disneyA, float4 disneyB, float disneyC`，默认值还原）。

- [ ] **Step 1: `pbr_common.slang` 扩展 `PBR_BRDF`**

`PBR_BRDF` 签名追加迪士尼参数（默认值还原 glTF）：

```cpp
float3 PBR_BRDF(float3 albedo, float metallic, float roughness,
                float3 N, float3 V, float3 L,
                float dielectricF0 = 0.04, float2 envBRDF = float2(0.0, 1.0),
                float4 disneyA = float4(0, 0, 0.5, 0),   // anisotropic, subsurface, specular, sheen
                float4 disneyB = float4(0, 1, 1, 1),      // clearcoat, clearcoatGloss, specularTint.rg
                float  disneyC = 1.0)                     // specularTint.b
{
    float anisotropic = disneyA.x, subsurface = disneyA.y, specular = disneyA.z, sheen = disneyA.w;
    float clearcoat = disneyB.x, clearcoatGloss = disneyB.y;
    float3 specularTint = float3(disneyB.z, disneyB.w, disneyC);

    // specular/specularTint：电介质 F0 = 0.16*specular²（specular!=0.5 时覆盖 ior 派生 F0）
    float dielectric = (abs(specular - 0.5f) > 1e-4f) ? (0.16f * specular * specular) : dielectricF0;
    float3 f0 = lerp(float3(dielectric), albedo, metallic) * specularTint;

    // ... 原 F/D/G 计算，D 项改用各向异性 GGX（anisotropic>0 时）...
    // clearcoat：第二层镜面（IOR 1.5，GGX），按 clearcoat 混合
    // sheen：边缘项 F_sheen = sheen * pow(1-HdotV,5) * albedo 加到漫反射
    // subsurface：diffuse = mix(Lambert, wrapDiffuse, subsurface)
    // ...（多重散射补偿 envBRDF 逻辑保留）...
}
```

实现要点（参考 Burley 2012 "Physically Based Shading at Disney"）：
- 各向异性 D 项：`aspect = sqrt(1 - 0.9*anisotropic)`，`ax = roughness²/aspect, ay = roughness²*aspect`，用 `D_GGX_aniso(NdotH, HdotT, HdotB, ax, ay)`
- clearcoat：`F_cc = 0.04 + 0.96 * F_Schlick(0.04, HdotV)`，`D_GGX(NdotH, clearcoatGloss²)`，`G_Smith(...)`，层叠后 `f = (1 - F_cc) * base + F_cc * clearcoat_lobe`（近似，按 clearcoat 强度混合）
- sheen：`sheenColor = specularTint`，`F_sheen = sheen * pow(1 - HdotV, 5)`，`diffuse += albedo * F_sheen`
- subsurface：`NdotL_wrap = (NdotL + subsurface) / (1 + subsurface)`，`diffuse = mix(albedo/PI * NdotL, albedo/PI * NdotL_wrap, subsurface)`

- [ ] **Step 2: `PBR.frag.slang` 透传**

`ShadeWithLight` 调用 `PBR_BRDF` 处传入 `obj.disneyA, obj.disneyB, obj.disneyC`（`obj` 为 GPUObjectData）。

- [ ] **Step 3: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过。默认参数下 02.Cube 渲染与改造前一致（无回归）。

- [ ] **Step 4: Commit（先向用户确认）**

```bash
git add Engine/Shader/Shaders/pbr_common.slang Engine/Shader/Shaders/PBR.frag.slang
git commit -m "Render: Disney BSDF BRDF 实现（Forward）"
```

---

### Task 3: Deferred 透传（GBuffer 扩展）

**Files:**
- Modify: `Engine/Shader/Shaders/GBuffer.frag.slang`（写入迪士尼参数到 GBuffer）
- Modify: `Engine/Shader/Shaders/DeferredLighting.frag.slang`（读取并透传）
- 可能：`Engine/Render/Pipeline/GBufferRenderer.*`（GBuffer 纹理格式/数量，若需新增通道）

**Interfaces:**
- Consumes: `GPUObjectData.disneyA/B/C`（Task 1）、`PBR_BRDF` 迪士尼参数（Task 2）。

- [ ] **Step 1: 探查 GBuffer 现状**

读 `GBuffer.frag.slang` + `DeferredLighting.frag.slang`，确认 GBuffer 是否还有空闲通道可承载迪士尼参数（9 floats）。若无空闲，需新增一张 GBuffer 纹理（如 RGBA16_FLOAT ×3 或类似）——此为本任务主要工作量，实现时按实际 GBuffer 布局决定。

- [ ] **Step 2: GBuffer 写入 + Deferred 读取**

将迪士尼参数（至少 clearcoat/sheen/specular/specularTint 等高频项）写入 GBuffer，`DeferredLighting.frag` 读取并传入 `PBR_BRDF`。若通道不足以承载全部 7 参数，优先承载 clearcoat/clearcoatGloss/sheen/specular/specularTint（各向异性/次表面可后续补）。

- [ ] **Step 3: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过。04.Deferred 默认参数下渲染与改造前一致。

- [ ] **Step 4: Commit（先向用户确认）**

```bash
git add Engine/Shader/Shaders/GBuffer.frag.slang Engine/Shader/Shaders/DeferredLighting.frag.slang
git commit -m "Render: Disney BSDF Deferred 透传（GBuffer 扩展）"
```

---

## 验证清单

1. 全量编译零错误。
2. 默认参数下 02.Cube / 04.Deferred 渲染与改造前一致（无回归）。
3. 手动设 clearcoat/sheen/anisotropic 可见对应材质效果（用户视觉确认）。
