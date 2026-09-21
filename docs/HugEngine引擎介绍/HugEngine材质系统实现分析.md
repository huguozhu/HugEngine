# HugEngine 材质系统实现分析

> 分析日期：2026-09-21 | 基线提交：`1ee4c1e`
> 范围：**延迟渲染路径**（`DeferredPipeline` → `GBuffer.frag` → `DeferredLighting.frag`）的材质参数、
> 各参数的意义与数学原理、以及与材质系统相关的已知边界与改进清单。
> 相关文档：[HugEngine渲染管线实现分析.md](HugEngine渲染管线实现分析.md)（管线与 GBuffer 通道）、
> [HugEngine全局光照GI实现分析与架构优化方案.md](HugEngine全局光照GI实现分析与架构优化方案.md)（IBL 烘焙与 split-sum）、
> [HugEngine Entity-Component 架构与组件功能.md](HugEngine%20Entity-Component%20架构与组件功能.md)（组件字段）、
> [HugEngine架构UML文档.md](HugEngine架构UML文档.md)（类关系）。
>
> **本文怎么读**：第 2~4 节是"现在支持什么"（事实），第 5 节是"这些参数在数学上是什么"（原理），
> 第 6 节是"哪些声明了但没生效"（坑），第 7~9 节是"该怎么改"（改进清单 + 验证 + 落地顺序）。
> 所有事实性断言均带 `文件:行` 证据；未读过的文件不作论断。

---

## 目录

1. [结论速览](#1-结论速览)
2. [材质数据流与三条解析路径](#2-材质数据流与三条解析路径)
3. [延迟渲染 GBuffer 通道与材质参数](#3-延迟渲染-gbuffer-通道与材质参数)
4. [延迟路径实际生效的材质参数](#4-延迟路径实际生效的材质参数)
5. [参数的意义与数学原理](#5-参数的意义与数学原理)
6. [已知边界与不生效的材质参数](#6-已知边界与不生效的材质参数)
7. [需要改进的方面](#7-需要改进的方面)
8. [验证与回归](#8-验证与回归)
9. [落地顺序与提交分组](#9-落地顺序与提交分组)
10. [附录 A：材质相关符号索引](#附录-a材质相关符号索引)
11. [附录 B：通道预算与承载能力](#附录-b通道预算与承载能力)

---

## 1. 结论速览

| 维度 | 现状 |
|---|---|
| 材质参数来源 | `MeshComponent` 的裸字段（**不是资产**，`MaterialEditor` 节点图为游离存根） |
| 核心层 | glTF 2.0 metallic-roughness 全套：baseColor / metallic / roughness / normal / occlusion / emissive / alphaCutoff / ior |
| 扩展层 | Disney 参数 9 个标量：anisotropic / subsurface / specular / sheen / clearcoat / clearcoatGloss / specularTint.rg（**specularTint.b 无通道**） |
| 纹理 | **4 个 bindless 槽**（BaseColor / Normal / MetallicRoughness / Occlusion），`materialID` 为基索引 |
| 承载 | GBuffer 8 张 MRT，材质占 MRT0/1/2/4/5/6 = 48 B/像素 |
| **最严重的三处事实** | ① 材质**全部不入存档**（`MeshComponent` 零反射属性）；② 贴图**无 sRGB→线性解码**；③ `SceneRenderer` 少拷贝 Disney/ior ⇒ 延迟画面里 MRT5/6 与 F0 **恒为中性默认值** |
| 改进项 | 19 项，分 P0（正确性/数据完整性，6 项）、P1（表达能力，5 项）、P2（架构与工作流，7 项）、P3（数值精度，3 项） |

---

## 2. 材质数据流与三条解析路径

```
MeshComponent(CPU 字段)
   │
   ├─(光栅路径) SceneRenderer::Prepare ─→ PBRMaterial ─→ GPUObjectData(SSBO)
   │                                                        │
   │                                          GBuffer.frag ──┴─→ 8×MRT ─→ DeferredLighting.frag ─→ PBR_BRDF
   │
   ├─(RT/PT 路径) RTPass::BuildSceneMaterialTexture ─→ 材质纹理 row0~row7 ─→ RT/PT 着色器
   │              （直接读 MeshComponent，不经 PBRMaterial）
   │
   └─(Forward 路径) ForwardPipeline::UploadMaterialBindless ─→ GPUMaterialData ─→ PBR.frag
```

三条路径各自解析材质，**字段来源与拷贝范围不一致**（见 §6.1 与 §7 P0-4）：

| 路径 | 材质载体 | 读取方 | 备注 |
|---|---|---|---|
| 光栅（Forward/Deferred 共用 GBuffer/PBR） | `GPUObjectData`（208 B，内联材质） | `GBuffer.frag.slang:54`、`PBR.frag.slang:290` | 每物体一份，未去重 |
| 光栅（Forward bindless 可选） | `GPUMaterialData`（112 B，按 `materialID>>2` 索引） | `PBR.frag.slang:281`（`useBindlessMaterial`） | **延迟路径不用**这条 |
| RT/PT | 材质纹理 row0~row7 + `PathPayload` | `RT_Bindless.rcall.slang`、`PT_Full.rchit.slang` | `u_Materials` 在 RT 侧**未绑定**（`RT_Bindless.rcall.slang:4` 自述） |

---

## 3. 延迟渲染 GBuffer 通道与材质参数

通道定义：`GBuffer.frag.slang:41-51`（写入）、`GBufferRenderer.h:16-26`（槽位常量）、
`GBufferRenderer.cpp:248-255`（格式）、`GBufferRenderer_CPU.cpp:38-54`（清屏值）。

| MRT | 格式 | 内容 | 材质参数来源 | 清屏值 |
|---|---|---|---|---|
| 0 | RGBA16F | albedo.rgb + metallic | `baseColorFactor × BaseColor` 贴图；`metallicFactor × MR.b` | a=1.0 |
| 1 | RGBA16F | normal×0.5+0.5 + roughness | 几何法线 + Normal 贴图（`N + t×0.5`）；`roughnessFactor × MR.g`，clamp[0.04,1] | a=1.0 |
| 2 | RGBA16F | emissive.rgb + ao | `emissiveFactor.rgb`；`aoFactor × Occlusion.r` | a=1.0 |
| 3 | RG16F | velocity = currUV−prevUV | **非材质**（TAA / 运动模糊） | 0 |
| 4 | RGBA16F | worldPos.xyz + **dielectricF0** | `ior` → `(ior−1)²/(ior+1)²` | 0 |
| 5 | RGBA16F | **disneyA** = aniso, subsurface, specular, sheen | Disney 扩展 | x=0, z=0.5 |
| 6 | RGBA16F | **disneyB** = clearcoat, clearcoatGloss, specularTint.rg | Disney 扩展 | y=1, z=1, w=1 |
| 7 | RGBA16F | lightmapKey（按物体 AABB 的箱式投影 + objectIndex），**`.a` 空闲**（写 0） | 物体级，非材质 | 0 |

写入与取值（`GBuffer.frag.slang:56-135`）：

```hlsl
albedo    = obj.baseColorFactor.rgb * Sample(BaseColor, uv).rgb            // :61  （无 sRGB 解码，见 §6.2）
metallic  = obj.metallicFactor * mrSample.b                              // :66  （无 MR 贴图时 b=0）
N         = normalize(worldNormal + (2·Sample(Normal,uv).rgb-1) * 0.5)   // :71-76 （非 TBN，见 §6.5）
roughness = clamp(obj.roughnessFactor * mrSample.g, 0.04, 1.0)           // :78
ao        = obj.aoFactor * (有 Occlusion 贴图 ? Sample(...).r : 1.0)     // :79-81
if (alpha < obj.alphaCutoff) discard;                                   // :68  （无条件，不看 alphaMode）
worldPos.a = obj.dielectricF0                                            // :97
disneyA/B  = obj.disneyA / obj.disneyB                                   // :98-99
lightmapKey= boxProject(worldPos, N, obj.boundsMin/Max) + objectIndex    // :112-134
```

---

## 4. 延迟路径实际生效的材质参数

「生效」= 从 `MeshComponent` 一路到达 `PBR_BRDF` 并被使用。

| 参数 | 默认 | 在延迟里的作用 | 代码位置 |
|---|---|---|---|
| `baseColorFactor` (rgb) | (1,1,1,1) | ×BaseColor 贴图 → albedo，参与漫反射与金属 F0 | `GBuffer.frag.slang:61` |
| `metallicFactor` | 0.0 | ×MR 贴图.b → MRT0.a，进 `lerp(dielectric, albedo, metallic)` | `:66` |
| `roughnessFactor` | 0.8 | ×MR 贴图.g，clamp 0.04…1 → GGX α、clearcoat、IBL mip、RT 反射锥角 | `:78` |
| `emissiveFactor` (rgb) | (0,0,0) | MRT2.rgb，光照末尾**原样相加**（不吃 AO、不吃直接光） | `:95`、`DeferredLighting.frag.slang:538` |
| `aoFactor` | 1.0 | ×Occlusion 贴图.r → MRT2.a，只作用于**间接**漫反射/镜面（×`aoIntensity`） | `:79`、`DeferredLighting.frag.slang:484-505` |
| `alphaCutoff` | 0.5 | `alpha(=factor.a×tex.a) < cutoff` → discard | `:68` |
| `ior` | 1.5 | 预计算 `dielectricF0` 存 MRT4.a，用于 Fresnel | `Material.h:136-137` |
| `anisotropic` | 0.0 | MRT5.x → 各向异性 GGX | `pbr_common.slang:184-189` |
| `subsurface` | 0.0 | MRT5.y → wrap 漫反射 | `:205-207` |
| `specular` | 0.5 | MRT5.z → 覆盖 F0（`0.16·specular²`） | `:178` |
| `sheen` | 0.0 | MRT5.w → 边缘项 `sheen·(1−h·v)⁵·albedo` | `:209-210` |
| `clearcoat` | 0.0 | MRT6.x → 第二层镜面混合 | `:215-220` |
| `clearcoatGloss` | 1.0 | MRT6.y → 清漆层 GGX α | `:217-218` |
| `specularTint.r/g` | (1,1) | MRT6.z/w → f0 着色 | `:180` |
| 4 张贴图槽 + `textureMask` | — | BaseColor/Normal/MetallicRough/Occlusion，`materialID` 为基索引 | `ShaderTypes.slang:225-229` |

> ⚠ **前置条件**：上表第 8~14 行（Disney 层）与 `ior` **在当前代码里恒为默认值** ——
> `SceneRenderer.cpp:110-125` 没有把 `MeshComponent` 的 `ior/anisotropic/subsurface/specular/
> specularTint/sheen/clearcoat/clearcoatGloss` 拷进 `PBRMaterial`，见 §6.1。

---

## 5. 参数的意义与数学原理

### 5.1 总纲：渲染方程与存储判据

```
L_o(x, ω_o) = ∫_Ω  f_r(x, ω_i, ω_o) · L_i(x, ω_i) · (n·ω_i) dω_i  +  L_e(x, ω_o)
```

延迟渲染把积分拆成两半：几何阶段算 `f_r` 的**参数**写进 GBuffer，光照阶段对每个光源/GI 源求乘积与累加
（`DeferredLighting.frag.slang:366` 的 `color += PBR_BRDF(...) * radiance * shadow`）。

| 类别 | 例 | 是否进 GBuffer | 原因 |
|---|---|---|---|
| 与 ω_i 无关、与 ω_o 近似无关的材质常量 | albedo、F0、roughness、Disney 参数 | **必须** | 光照阶段每光源都要用，重算 = 重采样贴图 |
| 视角相关但可解析重建 | V、NdotV、H | **不进** | 光照阶段有 worldPos（MRT4）+ cameraPosition，一次减法即得（`:276`） |
| 光源相关量 | L、辐照度、阴影 | **不进** | 光照阶段直接取 |
| 逐像素高频几何量 | N、worldPos、velocity | 进（N 在 MRT1） | 无法从深度稳定重建，AO/反射/降噪都要 |

### 5.2 glTF 核心层

#### 5.2.1 albedo — MRT0.rgb

**意义**：表面固有色；金属的 albedo 同时兼作镜面颜色。

**数学**：Lambert 漫反射的反射率 ρ。

```
f_d = k_D · albedo / π                                    // pbr_common.slang:203
∫_hemisphere (ρ/π) cosθ dω = ρ                            // 1/π 的来历：半球积分 ∫cosθ dω = π
```

`1/π` 是归一化因子而非调参：只有除以 π，漫反射的方向积分才等于 ρ。这也是所有 GI 源必须返回
`L_o = albedo·E/π` 而非 `E` 的原因（`DeferredLighting.frag.slang:165-171` 的"统一量纲约定"）。
贴图必须按 sRGB 解码成线性（**当前缺失**，§6.2）。

#### 5.2.2 metallic — MRT0.a

**意义**：0 = 塑料/木头/石头，1 = 纯金属。

**数学**：在电介质与导体两套光学参数间插值，并关闭导体的漫反射。

```
f0 = lerp(float3(0.04), albedo, metallic)                 // :180
k_D = (1 - F) · (1 - metallic)                            // :202
```

导体内部无透射（自由电子气重新辐射），没有次表面散射 ⇒ 没有漫反射项。这是 metallic-roughness
工作流唯一的"开关"。贴图缺省时 MR 采样返回 `(0,1,0,1)`（`GBuffer.frag.slang:65`），即 b=0 不污染 factor。

#### 5.2.3 roughness — MRT1.a

**意义**：微观粗糙度（与法线贴图的介观起伏是两件事）。

**数学**：控制 GGX / Trowbridge-Reitz 法线分布的宽度。

```
α = roughness²                                            // :186
D(n,h) = α² / (π · ((n·h)²(α²-1) + 1)²)                   // :28-33
G_Smith = G_SchlickGGX(n·v)·G_SchlickGGX(n·l), k=(r+1)²/8 // :52-58  （直接光形式）
```

- **为什么平方**：Disney 的感知线性映射，物理宽度是 α；所以 `D_GGX(NdotH, roughness)` 内部自己再平方一次。
- **为什么 clamp 0.04**（`GBuffer.frag.slang:78`）：α→0 时 D 趋于 δ，`D·G·F/(4 n·v n·l)` 数值爆炸。
- 间接影响：IBL 预滤波 mip = `roughness·(mips-1)`（`DeferredLighting.frag.slang:236`）、RT 反射锥角、SSR 淡出。

#### 5.2.4 法线 N — MRT1.xyz（编码 ×0.5+0.5）

**意义**：介观起伏；BRDF 里**所有**方向量的基准（`n·l`、`n·v`、`n·h`）。

**数学**：正确形式是切线基变换 `N = normalize(T·t.x + B·t.y + N_geom·t.z)`，`t = (2·tex−1)·scale`。
当前实现（`GBuffer.frag.slang:71-76`）退化为世界空间直接相加 + 固定 0.5 强度，且顶点布局无 tangent
（`MeshComponent.h:19-23`）、`normalTexture.scale` 未读取 ⇒ 见 §6.5。

#### 5.2.5 emissive — MRT2.rgb

**意义**：自发光表面（灯管、屏幕、岩浆），HDR 光源。

**数学**：渲染方程的 `L_e`，**加法项**，不经 BRDF、不乘 `n·l`、不受阴影影响：

```
color = direct + (indirectDiffuse·giIntensity + indirectSpecular)·aoFactor
color += gbC.rgb;                                          // :536-538
```

注释明确记录 AO 不应作用于自发光（`:442-446`）。

#### 5.2.6 ao — MRT2.a

**意义**：暗角/缝隙变暗；**不是 BRDF 参数**，是遮蔽近似。

**数学**：把方向相关的可见性压成标量常数因子：

```
L_o ≈ ∫ f_r·L_i·cosθ·V(ω_i) dω   →   (1−occlusion) · ∫ f_r·L_i·cosθ dω
aoFactor = lerp(1.0, ao·aoVal, aoIntensity)                // :505
color = directColor + (indirectDiffuse·gi + indirectSpecular)·aoFactor   // :536
```

只在间接项上施加是**正确**的；历史上曾乘到直接光上（`:445-446` 记录的 4.7 倍亮度事故）。

#### 5.2.7 alphaCutoff — 非 BRDF

**意义**：镂空（树叶、铁网）。

**数学**：随机几何的确定性二值化近似——把"覆盖概率"变成 discard。代价是 GBuffer 无 per-pixel coverage，
MSAA 对 GBuffer 失效（`AA_MSAA.h:9`），边缘只能靠 TAA/SMAA；半透明（Blend）无通道承载（§6.8）。

#### 5.2.8 ior → dielectricF0 — MRT4.a

**意义**：电介质折射率（水 1.33 / 玻璃 1.5 / 钻石 2.4）。

**数学**：

```
F(θ) = F0 + (1−F0)(1−cosθ)⁵          （Schlick 近似）       // :15-17
F0   = ((n1−n2)/(n1+n2))², n1=1, n2=ior                    // Material.h:137
```

存 F0 而非 ior：BRDF 只需 F0（每像素省一次除法，通道也紧张）。**丢失**：标量 F0 无色散、无导体复折射率
k 项、Schlick 在掠射角误差最大。

### 5.3 Disney 扩展层（MRT5 / MRT6）

存在理由：metallic-roughness 只有"一个漫反射叶 + 一个 GGX 镜面叶"，无法表达清漆、布料、蜡、拉丝金属。
Disney principled BSDF 的做法是在基础层上增加**正交的叶**，每叶对应一个可解释参数。

| 参数 | 数学形式 | 物理/美术意义 | 代码 |
|---|---|---|---|
| `specular` | `dielectric = 0.16·specular²`（≠0.5 时覆盖 ior 派生 F0） | 电介质 F0 覆盖（KHR_materials_specular） | `:178` |
| `specularTint` | `f0 *= tint`（RGB 三分量） | 金属/有色镜面的色调 | `:180` |
| `anisotropic` | `aspect=√(1−0.9a)`，`ax=α/aspect`，`ay=α·aspect`，`D_GGX_aniso`（Burley 2012） | 拉丝金属、头发、唱片；`ax=ay` 时精确退化为等向 | `:41-47, 184-189` |
| `subsurface` | wrap 漫反射 `NdotL_wrap=(n·l+ss)/(1+ss)` | 蜡/玉/皮肤的明暗交界线扩散（Hanrahan-Krueger 廉价版） | `:205-207` |
| `sheen` | `diffuse += albedo·sheen·(1−h·v)⁵` | 布料微纤维的掠射"银边" | `:210` |
| `clearcoat` | `Fcc=0.04+0.96(1−h·v)⁵`；`base = lerp(base, (1−Fcc)·base + Fcc·lobe_cc, cc)` | 车漆/釉面：外层无色镜面 + 内层有色 | `:215-220` |
| `clearcoatGloss` | `D_GGX(n·h, ccGloss)`（内部再平方） | 清漆层的光滑度；glTF 存 roughness ⇒ 加载时取反 | `:217`、`glTFLoader.cpp:185` |

**要点**：
- `clearcoat` 的 `(1−Fcc)·base` 是**分层能量守恒**的标准写法（外层反射走 Fcc，透过的按 1−Fcc 衰减到内层），
  比"再叠一个高光"正确。
- `sheen` 是**加法**、不减 `f_d`，严格意义上不守恒（真实模型见 Charlie 分布 / Zeltner 2022）——已知近似。
- 多重散射能量补偿（Kulla-Conty / Frostbite，`:197-199`）由 roughness+F0 共同决定：
  `E(f0)=f0·A+B`，`f_ms = 1 + f0(1/E−1)`，`specularBRDF *= f_ms`。**只补偿镜面**。

### 5.4 光照阶段：参数如何进入积分

**直接光**（`:366`）：

```
color += PBR_BRDF(albedo, metallic, roughness, N, V, L, F0, envBRDF, disneyA, disneyB, disneyC)
         · radiance · shadow
```

`PBR_BRDF` 的返回已含 `(n·l)`（`:222` 的 `baseBRDF * NdotL`），所以外面只乘 `L_i` 与可见性。

**间接光（split-sum）**：

```
∫ f(l,v)·L_i·cosθ dω ≈ (∫ f·cosθ dω) × (∫ L_i·D·cosθ dω / ∫ D·cosθ dω)
                         ↑ BRDF LUT (A,B)     ↑ 预滤波 cubemap（按 roughness 分 mip）
```

- 镜面：`(F0·A + B) · prefiltered(R, roughness·(mips−1))`（`:235-237`）；A/B 的来历是 Schlick 拆成
  常数项与掠射项后的两个积分 `∫(1−Fc)·G_vis`、`∫Fc·G_vis`，Hammersley + GGX 重要性采样 256 样本
  （`IBL_BRDF_LUT.frag.slang:56-86`，512² RG16F）。
- 漫反射：辐照度图存 `E/π`（`IBL_Irradiance.frag.slang:44-49`），故只需 `k_D·irradiance·albedo`（`:211`）。
- GI 层栈：各源统一到 `L_o = albedo·E/π` 后按权重归一化（`:456-470`），权重和 = 1 保证不双重计数；
  白炉测试（`:293-296`、`:472-478`）用"全白环境真值 = 1"验证归一化。

---

## 6. 已知边界与不生效的材质参数

### 6.1 延迟链路上游断供（Disney 与 ior 恒为默认值）

`SceneRenderer.cpp:110-125` 与 `ForwardPipeline.cpp:650-665` 各写一份 `MeshComponent → PBRMaterial` 拷贝，
**两份都漏了** `ior/anisotropic/subsurface/specular/specularTint/sheen/clearcoat/clearcoatGloss`。
⇒ 延迟画面里 MRT5/6 与 F0 恒等于 `PBRMaterial` 的默认值（与 GBuffer 清屏值逐位相同），
`KHR_materials_clearcoat / specular / sheen / ior / anisotropy` 在延迟路径**全部无效**。
RT/PT 侧因直读 `MeshComponent`（`RTPass.cpp:601-605`）而是有效的。

### 6.2 贴图无 sRGB→线性解码

样本以 `RGBA8_UNORM` 创建贴图（`04.Sponza-Deferred.cpp:404`），PNG/JPEG 字节是 sRGB 编码，
shader 直接采样后乘 factor。`common.slang:27` 定义了标准分段 `SRGBToLinear`，但**全引擎只有
ColorGrading 用自己那份**（`ColorGrading.frag.slang:31`）。数学后果：sRGB 0.5 ≈ 线性 0.21，
光照积分全线性 ⇒ 中间调亮度/对比系统性偏差。

### 6.3 材质全部不入存档

`SceneReflect.cpp:57-58` 是空注册块，而 `SceneSerializer::Save` 只遍历 `PF_Serializable` 反射属性
（`SceneSerializer.cpp:64-66`）⇒ **MeshComponent 的所有材质字段与贴图路径存盘即丢**。

### 6.4 序列化分派表缺类型 = 静默丢数据

`SerializeProperty`（`SceneSerializer.cpp:25-39`）支持 `bool/i32/u32/i64/u64/f32/f64/String/float3/float4/quat`，
**无 `float2` 与 `u8`**，且 `else` 分支静默跳过。已注册的 `float2 BillboardComponent::size`
（`SceneReflect.cpp:253`）与 `u8 DecalComponent::blendMode`（`:294`）**现在就在丢**。

### 6.5 法线贴图不是 TBN

`GBuffer.frag.slang:71-76` 把切线空间的 `t` 直接加到世界空间法线上（`N + t·0.5`），
顶点无 tangent（`MeshComponent.h:19-23`、`GBufferRenderer.cpp:224-227`），`normalTexture.scale` 未读取。
⇒ 方向错误（仅在起伏极小时近似）、强度不可调、各向异性无方向（§6.10）。
另：占位纹理是全白，法线槽若 `textureMask` 位误置位，`(2·1−1)` 会把 N 偏 45°。

### 6.6 specularTint.b 无通道

`GPUObjectData.disneyC` 存在（`ShaderTypes.slang:280`），但 GBuffer 未携带，
`DeferredLighting.frag.slang:273-274` 硬编码 `disneyC = 1.0`。⇒ 镜面色调只能偏黄/青，不能偏蓝。
**MRT7.a 是空闲的**（`GBuffer.frag.slang:134` 写 0）。

### 6.7 纹理槽只有 4 个

`kGPUMaterialTexSlot_Count = 4`，`u_Materials` 索引 = `materialID>>2`（`ShaderTypes.slang:229`、`common.slang:72`）。
⇒ `emissiveTexture` 字段存在且有拷贝（`SceneRenderer.cpp:125`）但**无槽**，自发光只能是常量因子；
clearcoat/sheen/transmission 贴图同样无处放。

### 6.8 材质标志不上 GBuffer

`materialFlags`（`MF_DoubleSided/AlphaMask/Unlit`）上传到 `GPUObjectData`，但 `GBuffer.frag`/`DeferredLighting.frag`
**一个字都不读**（仅 `PBR.frag.slang:284,290` 读）。具体后果：

| 标志 | 实际行为 | 原因 |
|---|---|---|
| `MF_Unlit` | 延迟路径无效（编辑器勾了没反应） | 标志未进 GBuffer |
| `MF_AlphaMask` | 位无作用；discard **无条件**执行 | `GBuffer.frag.slang:68` |
| `MF_DoubleSided` | 无意义（从不背面剔除） | GBuffer PSO 未设 cullMode，RHI 默认 `CullMode::None`（`RHI/Shader.h:72` → `VK_CULL_MODE_NONE`，`VulkanConverters.cpp:86-92`） |
| `AlphaMode::Blend` | 无半透明；alpha 不写 GBuffer | 延迟无透明通道 |

### 6.9 transmission / volume 不在 GPUObjectData

`transmission/attenuationColor/attenuationDistance` 只在 `MeshComponent.h:80-83` 与 RT 材质纹理
（`RTPass.cpp:620-624` 的 σ_t）中存在 ⇒ 光栅路径下玻璃完全不透明。

### 6.10 各向异性方向不可控

`glTFLoader.cpp:206-207` 只取 `anisotropy_strength`（方向未读），shader 里 `T/B` 由
`cross(float3(0,1,0), N)` 现凑（`pbr_common.slang:167-173`）⇒ 拉丝方向随法线游走。

### 6.11 Nanite 软光栅只写 4 张 MRT

`Nanite_SoftRaster.comp.slang:123-126` 只写 albedo/normal/worldPos/lightmapKey
⇒ 其覆盖像素上 emissive、disneyA/B、velocity 保持清屏值（Disney 与自发光无效）。

### 6.12 MRT7 光照图键尚未被消费

`DeferredLighting.frag.slang:30` 声明了 `u_LightmapKey`，全文件**再无引用**。

### 6.13 IBL BRDF LUT 的 `k` 多平方一次

`IBL_BRDF_LUT.frag.slang:49-50`：`a = roughness²; k = a²·0.5` ⇒ `k = roughness⁴/2`，而 Karis/UE4 的
IBL 惯例是 `k = a/2 = roughness²/2` ⇒ 高粗糙下 Smith 遮蔽偏小 ⇒ **间接高光偏亮**。
（直接光路径的 `k=(r+1)²/8`，`pbr_common.slang:53`，是正确的，两者不可混用。）
该疑点此前已记录于 [GI 分析文档](HugEngine全局光照GI实现分析与架构优化方案.md) 第 43 行。

### 6.14 注释与文档漂移

| 位置 | 漂移 |
|---|---|
| `ShaderTypes.slang:265` | 写"GPUObjectData：176 字节"，实际 `sizeof == 208`（`Material.h:52` 断言） |
| `GBufferRenderer.h:24-26`、`GBuffer.frag.slang:9` | 写"lightmapKey = uv0.xy"，实际是**按物体 AABB 的箱式投影**（`GBuffer.frag.slang:112-134`） |
| `HugEngine渲染管线实现分析.md:456` | "头部注释过时：GBuffer 5×MRT 实际 7 MRT"（现为 8 张） |
| `GBuffer.vert.slang:1`、`GBufferRenderer.h:110` | "5 个 MRT 颜色纹理"、"4 MRT + velocity" 等旧计数 |
| `MaterialEditor.h` | 节点图 UI 为**游离存根**（无序列化、无编译、无消费者），UI 上无任何标注 |

### 6.15 其它近似（标注，不急于修改）

- `sheen` 加法不守恒（`:210`）；能量补偿只作用于镜面（`:197-199`）。
- `k_D = (1−F)(1−metallic)` 里的 F 是单点采样（行业通行简化）。
- 硬编码魔法数：`clamp(roughness, 0.04)`、法线强度 `0.5`、PCF 核 `1/2048`（`pbr_common.slang:94`）。

---

## 7. 需要改进的方面

### 7.0 改进项总表

| # | 优先级 | 问题 | 证据 | 后果 |
|---|---|---|---|---|
| 1 | P0 | 材质全部不入存档 | `SceneReflect.cpp:57-58`、`SceneSerializer.cpp:64-66` | `.hescene` 存盘后材质归零 |
| 2 | P0 | 分派表缺 `float2`/`u8` → 静默丢数据 | `SceneSerializer.cpp:25-39` vs `SceneReflect.cpp:253,294` | Billboard `size`、Decal `blendMode` 已丢失 |
| 3 | P0 | 缺 sRGB→线性解码 | `04.Sponza-Deferred.cpp:404`、`common.slang:27` 无人调用 | 所有贴图材质颜色/亮度偏差 |
| 4 | P0 | 延迟链路 Disney/ior 断供 | `SceneRenderer.cpp:110-125` | MRT5/6 与 F0 恒为默认值 |
| 5 | P0 | 法线贴图非 TBN + 无 strength | `GBuffer.frag.slang:71-76` | 法线方向错误、强度不可调 |
| 6 | P1 | `specularTint.b` 无通道 | `GBuffer.frag.slang:134`（.a 空闲） | 镜面色调不能偏蓝 |
| 7 | P1 | 纹理槽仅 4 个 | `ShaderTypes.slang:229`、`common.slang:72` | 自发光等贴图无处放 |
| 8 | P1 | 材质标志不上 GBuffer | `GBuffer.frag.slang` 全文 | unlit 无效、Blend 无半透明 |
| 9 | P1 | transmission/volume 不在 GPUObjectData | `MeshComponent.h:80-83` | 玻璃不透明 |
| 10 | P1 | 各向异性方向无处存 | `glTFLoader.cpp:206-207` | 拉丝方向不可控 |
| 11 | P2 | 材质不是资产 | `MaterialEditor.h`、`Engine/Asset/` 仅 glTF | 无法共享/继承/复用 |
| 12 | P2 | 三条解析路径不同源 | `SceneRenderer.cpp` vs `RTPass.cpp:601` | 同一材质三处语义不一致 |
| 13 | P2 | 延迟不用 `GPUMaterialData` | `GBuffer.frag.slang:54` 读内联 | 同材质多物体重复 208 B/物体 |
| 14 | P2 | `materialID` 靠样本手工注册 | `04.Sponza-Deferred.cpp:423-434` 等 | 引擎无材质→纹理绑定流程 |
| 15 | P2 | 材质上传无脏标记 | `ForwardPipeline.cpp:1108` | 每帧 O(物体数) 哈希 + 全量上传 |
| 16 | P2 | 编辑器材质能力缺口 | `DetailsPanel.cpp:251-261` | 纹理只读、无 ior/Disney 编辑 |
| 17 | P2 | 文档/注释漂移 | 见 §6.14 | 误导后续改动 |
| 18 | P3 | IBL LUT `k = roughness⁴/2` | `IBL_BRDF_LUT.frag.slang:49-50` | 高粗糙间接高光偏亮 |
| 19 | P3 | sheen 加法不守恒 / 魔法数散落 | `pbr_common.slang:210,94` | 已知近似，可标注 |

### 7.1 P0：正确性与数据完整性

#### P0-1 材质进反射与存档

**改法**：
1. 在 `SceneReflect.cpp:57` 的块内补注册全部材质字段（`HE_ATTR_CATEGORY("Material")` 便于编辑器分组）：
   `float4 baseColorFactor` / `float3 emissiveFactor` / `float metallicFactor` / `float roughnessFactor` /
   `float aoFactor` / `float alphaCutoff` / `u32 alphaMode` / `bool doubleSided` / `bool unlit` /
   `bool castShadow` / 5 条 `String` 贴图路径 / `float ior` / `anisotropic` / `subsurface` / `specular` /
   `sheen` / `clearcoat` / `clearcoatGloss` / `float3 specularTint` / `float transmission` /
   `float3 attenuationColor` / `float attenuationDistance`。
   注意 `alphaMode` 是 `u8`：**注册成 `u32`**（或按 P0-2 补 `u8` 分派）。
2. **不要注册**运行期派生量：`materialID`、`baseColorAvg/metallicAvg/roughnessAvg/hasMaterialAvg`
   （由 TextureRegistry / 均值计算填充，序列化会引入失效句柄）。
3. `.hescene` 语义变化 ⇒ 递增 `kVersion`（`SceneSerializer.cpp:103` 是硬失败校验，安全）。

**风险**：旧档不兼容（可接受，反序列化会明确报版本错误）。

#### P0-2 序列化分派表补类型（消除静默丢数据）

**改法**：`SerializeProperty` 补 `float2`、`u8`；把"跳过未知类型"从静默改为**一次性告警**，
并让 `HE_REGISTER_PROPERTY` 在编译期 `static_assert` 类型受支持（否则下一个新类型还会重演）。

**风险**：低，纯增益。

#### P0-3 sRGB 输入解码

**改法（二选一，推荐 A）**：

- **A. 格式层**：材质纹理按用途选格式 —— baseColor/emissive 用 `R8G8B8A8_SRGB`（RHI 已支持，
  `RHI/Types.h:104`），normal/metallicRoughness/occlusion 保持 UNORM。需要给纹理注册
  `TextureUsage { SRGB, Linear }` 语义位（材质系统应有的元数据）。
- **B. shader 层**：`GBuffer.frag` / `PBR.frag` / `PT_Full.rchit` 三处在 baseColor、emissive 采样后调
  `SRGBToLinear`。代价是每像素几次 pow，且与 A 不能同时启用（会双重转换）。

**风险**：**会让所有已调过的画面变亮**。建议带 cvar（如 `r.Material.SRGBInput`）先做 A/B 对照，
并在同一提交里更新样例观感基线。注意只作用于 RGB，alpha 保持线性。

#### P0-4 单源材质填充（消除 Disney/ior 断供）

**改法**：在 `Material.h` 增加唯一入口，三处共用：

```cpp
inline PBRMaterial MakeMaterialFrom(const he::MeshComponent& mc);   // 全字段
```

- `SceneRenderer::Prepare`（`:110-125`）、`ForwardPipeline::UploadMaterialBindless`（`:650-665`）改调它；
- `RTPass`（`:579-624`）也走同一来源（若需要 `transmission/attenuation*`，给 `PBRMaterial` 补这两个字段，
  或保留 `PackDisneyParams` 重载）；
- 顺手消除两处重复拷贝代码。

**验证**：新增单测断言 `FillObjectData(MakeMaterialFrom(mc))` 的 `disneyA/disneyB/disneyC/dielectricF0`
等于 `PackDisneyParams(mc.*)` 的结果。现有 `Tests/TestPTMaterialParams.cpp` **只测了 `PackDisneyParams`
本身，没测这条链路** —— 这正是断供能长期存活的土壤。

**风险**：带 `KHR_materials_clearcoat/specular/sheen/ior` 的资产会**立刻出现可见变化**（清漆/镜面色调），
属预期；需在提交信息与对照读数里写明。

#### P0-5 法线贴图 TBN 与强度

**改法（两档，建议先 A）**：

- **A. 屏幕空间导数 cotangent frame**（Schüler）：`GBuffer.frag` 用 `ddx/ddy(worldPos, uv)` 构造 T/B，
  零顶点改动，顺手支持 `scale` 与正确强度语义；UV 退化处（`det≈0`）回退几何法线。
- **B. 加 tangent 属性**：`StaticVertex` 加 `float4 tangent`，`glTFLoader` 读 `TANGENT` 或 MikkTSpace 生成，
  同步 `GBufferRenderer.cpp:224-227`、`GBuffer.vert.slang`、`GBuffer.mesh.slang`、Nanite 顶点解码
  （`NaniteTypes.slang`）。成本高，但各向异性（P1-10）必需。

**附带**：法线槽的默认占位纹理改为常量 `(0.5,0.5,1.0)`，从根上消除"占位白偏 45°"的污染。

### 7.2 P1：表达能力

#### P1-6 把 `specularTint.b` 放回 GBuffer（零成本）

`GBuffer.frag.slang:134` 的 `lightmapKey.a` 空闲 ⇒ `output.lightmapKey.a = obj.disneyC;`，
光照侧用**点采样**读 `.a`（键是索引，线性过滤会插值出无意义值，理由同 `:28-29` 对页号的说明）。
同步 `GBufferRenderer.h:24-26` 的语义注释与 `Material.h` 的 `offsetof` 断言。

#### P1-7 纹理槽 4 → 8

1. `kGPUMaterialTexSlot_Count = 8`（`>>3`），新增 `Emissive=4, Clearcoat=5, Sheen=6, Transmission=7`。
2. **同步修改点（必须一次搜全）**：`ShaderTypes.slang:229`、`common.slang:72-73` 注释、
   `PBR.frag.slang:281`、`GBuffer.frag.slang`（新增 emissive 采样 + sRGB 解码）、
   `ForwardPipeline.cpp:680-696`、`Nanite_SoftRasterCommon.slang:87,145`、`RT_Bindless.rcall.slang`、
   `PT_Full.rchit.slang:139-161`。
3. 注册入口集中在 P2-14 的 `RegisterMaterial`，避免 N 处手写。
4. emissive 贴图接入后，`emissiveFactor` 变为乘性（`emissiveFactor.rgb * Sample(Emissive,uv).rgb`）。

**风险**：bindless 堆容量；`>>2 → >>3` 使已存的 `materialID` 失效（运行期派生不持久化，只影响当帧）。

#### P1-8 材质标志与 unlit / 半透明

1. **立刻可做**：discard 用 `MF_AlphaMask` 门控；按 `doubleSided` 建两个 PSO（cull None / cull Back）。
2. **unlit 与 Blend**：走"光照之后的前向混合通道"——管线里已有同位置先例：Skybox pass
   （`DeferredPipeline_FrameGraph.cpp:1487-1503`，depth=Equal）与 ParticleRender（`:1534-1548`）。
   unlit/半透明网格在那里用 `PBR.frag` 的 unlit 分支绘制。
3. **透明排序**：`SceneRenderer::Prepare` 需分拣 `alphaMode==Blend` 的物体（前向通道按距离排序、关深度写）。

**风险**：新增 pass（帧图连接、深度 LoadOp、与 AA 的先后），建议单独立项。

#### P1-9 通道预算：建议新增材质标志附件

待接入的扩展至少有：unlit/mask 标志、各向异性方向、transmission、thickness、iridescence、dispersion。
现有 8 张 MRT 每通道都有语义 ⇒ 建议新增 `MRT8 = RGBA8_UNORM`（4 B/像素）承载标志位 + 两个量化参数。

**必须先查的硬约束**：`VkPhysicalDeviceLimits::maxColorAttachments` —— Vulkan 规范最小保证仅 **4**，
常见桌面为 8，**本引擎已在用 8 张**。→ 落地前先加设备能力查询（RHI 需提供
`GetCaps().maxColorAttachments`），超限设备回退到"标志打包进 MRT7.a 高位"或"分两趟 GBuffer"。

#### P1-10 各向异性方向

与 P0-5B 绑定（有 tangent 才有物理上有意义的 T/B）；补读 `anisotropyRotation` 并为其分配通道
（P1-9 的量化位，或 `GPUObjectData` + 空位）。**若不做 tangent**，应在文档中明确声明该参数
仅支持"强度"、方向为任意轴。

### 7.3 P2：架构与工作流

| # | 问题 | 改法 |
|---|---|---|
| P2-11 | 材质不是资产 | 定义 `MaterialAsset`（`.hemat`）：`PBRMaterial` + 名字 + 纹理相对路径；`MeshComponent` 增加材质引用（字段保留为内联覆盖以兼容现有样本）；运行时 `MaterialTable` 解析并去重成 `GPUMaterialData`。编辑器先做"资产 + DetailsPanel 指派"，节点图暂留为长期目标 |
| P2-12 | 三条路径不同源 | `MaterialTable` 落地后，光栅 / RT/PT / Forward 三处**只读同一份** `GPUMaterialData`；`RTPass` 的 row4~row7 直接由它序列化 |
| P2-13 | 延迟不用 per-material 表 | `GBuffer.frag.slang` 增加 `u_Materials[0][materialID>>shift]` 分支（复用 `common.slang:73`），并把 `useBindlessMaterial` 默认改为 true。**注意**：必须在 GBuffer 的 per-frame set 里完成绑定与 Flush —— 历史上 `u_BRDF_LUT` 因漏绑一度让整幅画面直接光算错 4.7 倍 |
| P2-14 | 注册流程缺失 | 新增 `RegisterMaterial(MeshComponent&, BindlessHeap*)`：按 `ComputeMaterialTextureMask` 逐槽注册（缺贴图注册**中性默认纹理**：法线 `(0.5,0.5,1)`、MR `(1,1,0,1)`）、校验基索引连续、按路径缓存去重。样本（`04/05/06/07`、`EditorApp.cpp:332-341`）统一改调 |
| P2-15 | 上传无脏标记 | `MaterialTable` 持版本号，材质/路径变化时 bump；`UploadMaterialBindless`（`ForwardPipeline.cpp:1108`）版本未变则早退 |
| P2-16 | 编辑器缺口 | `DetailsPanel.cpp:251-261` 纹理路径改为可指派 + 立即重注册；补 ior/Disney/transmission 控件；加材质预览球（可复用 GBuffer+Lighting 迷你尺寸，或用 PathTracingPipeline 做参考预览）；`MaterialEditor.h` 在 UI 上标注为存根 |
| P2-17 | 注释漂移 | 修 §6.14 各处；并把 `Material.h:50-60` 的 `static_assert` 扩展为**逐字段 `offsetof` 断言**（`GIBlendParams` 已是现成样板，`Material.h:69-72`） |

### 7.4 P3：数值与精度

| # | 项 | 改法 |
|---|---|---|
| P3-18 | IBL LUT 的 `k` | `IBL_BRDF_LUT.frag.slang:50` 改为 `k = a * 0.5`（`a = roughness²`）；用 PT 作参考对拍或与解析真值对照 |
| P3-19 | 近似与魔法数 | sheen 不守恒、镜面-only 能量补偿：在代码注释与本文档标注；把 `0.04`、`0.5`（法线强度）、`1/2048`（PCF）等集中成常量表，便于对照调参 |

---

## 8. 验证与回归

| 手段 | 内容 | 针对项 |
|---|---|---|
| 单测 `TestMaterialPacking` | `offsetof` 逐字段断言 + `MakeMaterialFrom` → `disneyA/B/C`、`dielectricF0` 数值断言 | P0-4, P2-17 |
| 存档往返测试 | `Save → Load` 后 MeshComponent 每个材质字段相等（**当前必然失败**） | P0-1, P0-2 |
| 分派表负测试 | 注册一个未支持类型必须**报错**而非静默 | P0-2 |
| PT 对拍 | 同场景 raster vs PT 的 albedo/roughness/F0/clearcoat 响应对比（项目已有 PT 为参考渲染器） | P0-3, P0-4, P3-18 |
| 白炉扩展 | 现有 `furnaceMode` 加"材质参数矩阵"扫描（roughness 0.04→1 × metallic 0/1 × clearcoat 0/1） | P3-18, P3-19 |
| 设备能力查询 | `maxColorAttachments`（第 9 张 MRT 的前置条件） | P1-9 |
| 画面对照 | 每项视觉变更前后各存一次读数/截图（`Tools/gi/*.ps1` 已有校验脚本范式） | P0-3, P0-4, P0-5 |

---

## 9. 落地顺序与提交分组

按"一条提交只做一件事"组织（与项目 Git 规则一致）：

| 序号 | 分组 | 内容 | 风险 |
|---|---|---|---|
| 1 | 数据完整性 | `SceneReflect` 注册材质属性 + 分派表补 `float2/u8` + `.hescene` 版本号（P0-1、P0-2、P2-17 的一部分） | 最低、立刻见效 |
| 2 | 单源填充 | `MakeMaterialFrom` + 三处改调 + 新单测（P0-4、P2-12 一半） | 有可见材质变化，需写明 |
| 3 | 色彩正确性 | sRGB 输入（P0-3），带 cvar 支持 A/B | 画面整体变化 |
| 4 | 零成本通道修复 | `specularTint.b` → MRT7.a（P1-6）；法线导数 TBN + `scale`（P0-5A） | 低 |
| 5 | 注册集中化 | `RegisterMaterial` + 样本改造（P2-14），为 P1-7 铺路 | 低 |
| 6 | 纹理槽扩展 | 4 → 8 槽 + emissive 贴图（P1-7） | 中（需一次改全） |
| 7 | 材质资产化 | `MaterialAsset` + `MaterialTable` + 延迟走 bindless 表 + 脏标记（P2-11、P2-13、P2-15） | 高（架构级） |
| 8 | 前向通道与标志 | 透明/unlit 前向通道 + 标志位与 MRT8 裁决（P1-8、P1-9、P1-10） | 中高（依赖设备查询） |
| 9 | 精度修正 | IBL LUT `k`（P3-18）+ 常量收敛（P3-19） | 低 |

---

## 附录 A：材质相关符号索引

| 符号 | 位置 | 用途 |
|---|---|---|
| `MeshComponent` 材质字段 | `Engine/Scene/Scene/MeshComponent.h:44-90` | CPU 侧材质来源（含 Disney / transmission / volume） |
| `PBRMaterial` / `MaterialFlags` | `Engine/Render/Pipeline/Material.h:25-107` | 光栅侧中间结构 + 标志位定义 |
| `FillObjectData` / `FillMaterialData` | `Material.h:129-183` | 打包成 `GPUObjectData`（内联）/ `GPUMaterialData`（bindless） |
| `PTMaterialParams` / `PackDisneyParams` | `Engine/Render/RT/PTMaterialParams.h:44-62` | Disney 参数三元组打包（三路径同源约定） |
| `GPUObjectData` / `GPUMaterialData` | `Engine/Shader/Shaders/ShaderTypes.slang:265-307` | GPU 结构（208 B / 112 B） |
| 纹理槽常量 | `ShaderTypes.slang:225-229` | 4 槽 + `materialID>>2` 约定 |
| 材质标志枚举 | `Material.h:25-30`（`MF_*`） | DoubleSided / AlphaMask / Unlit |
| `PBR_BRDF` | `Engine/Shader/Shaders/pbr_common.slang:149-223` | 唯一 BRDF 求值点（直接光 / IBL / PT 共用） |
| `D_GGX` / `D_GGX_aniso` / `F_Schlick` / `G_Smith` | `pbr_common.slang:15-59` | BRDF 分项 |
| `IntegrateBRDF` / `G_Smith`（IBL 版） | `IBL_BRDF_LUT.frag.slang:48-86` | split-sum 的 A/B 预计算 |
| `IBL_Irradiance` | `IBL_Irradiance.frag.slang:27-50` | 辐照度烘焙（归一化为 `E/π`） |
| GBuffer 通道常量 | `GBufferRenderer.h:16-26` | MRT 语义 |
| `SceneRenderer::Prepare` | `Engine/Render/SceneRenderer.cpp:105-147` | 材质 → `GPUObjectData` 填充（**当前漏 Disney**） |
| `UploadMaterialBindless` | `ForwardPipeline.cpp:645-696`、调用点 `:1108` | per-material 表去重上传（仅 Forward 使用） |
| `RTPass::BuildSceneMaterialTexture` | `RTPass.cpp:575-624` | RT/PT 材质纹理（直读 `MeshComponent`） |
| 清屏值 | `GBufferRenderer_CPU.cpp:38-54`、`GBufferRenderer_GPU.cpp:47-55` | GBuffer 中性默认值 |
| 序列化 | `SceneSerializer.cpp:25-39`、`SceneReflect.cpp:57-58` | 反射驱动的 `.hescene`（**MeshComponent 零注册**） |

## 附录 B：通道预算与承载能力

**GBuffer 总带宽**：8 张附件（7×RGBA16F + 1×RG16F）= 60 B/像素，其中**材质占 48 B**
（MRT0/1/2/4/5/6，各 RGBA16F = 8 B），几何占 4 B（MRT3 velocity，RG16F），物体级占 8 B（MRT7 key）。

| 待接入能力 | 需要的通道/槽 | 当前是否够 | 建议落点 |
|---|---|---|---|
| specularTint.b | 1 通道 | ✅ 够 | MRT7.a（空闲） |
| unlit / mask / doubleSided 标志 | 若干位 | ❌ | MRT8（RGBA8）或 MRT7.a 高位打包 |
| 各向异性方向（切线角） | 8~16 位 | ❌ | MRT8 量化位 |
| transmission / thickness | 8~16 位 | ❌ | MRT8，或仅前向透明通道 |
| emissive 贴图 | 1 个纹理槽（x3 通道带宽） | ❌（槽满） | 扩到 8 槽 |
| clearcoat / sheen / transmission 贴图 | 各 1 槽 | ❌ | 同上 |
| iridescence / dispersion | 多通道 + 槽 | ❌ | 长期：材质图（node graph）编译期解决 |

> **结论**：材质系统的表达能力当前被 **4 个纹理槽 + 8 张 MRT 的通道预算**双重约束。
> 短期靠"回收空闲通道 + 扩槽"能解决 1~2 个参数；一旦要支持 iridescence/transmission 这类
> 扩展族，必须走"材质资产 + 材质节点图编译"的路线（P2-11），靠通道挤位不可持续。
