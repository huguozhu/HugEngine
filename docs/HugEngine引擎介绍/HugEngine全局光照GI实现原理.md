# HugEngine 全局光照（GI）实现原理

> 本文档逐项分析 HugEngine 中每种全局光照（Global Illumination, GI）技术的实现细节与底层原理，涵盖光栅化路径的 GI 子系统与硬件光线追踪路径。

---

## 目录

1. [GI 架构总览](#1-gi-架构总览)
2. [GI_IBL —— 基于图像的光照](#2-gi_ibl--基于图像的光照)
3. [GI_SSGI —— 屏幕空间全局光照](#3-gi_ssgi--屏幕空间全局光照)
4. [GI_SSR —— 屏幕空间反射](#4-gi_ssr--屏幕空间反射)
5. [GI_RSM —— 反射阴影贴图](#5-gi_rsm--反射阴影贴图)
6. [GI_DDGI —— 动态漫反射全局光照](#6-gi_ddgi--动态漫反射全局光照)
7. [RTGIPass —— 硬件光线追踪 GI](#7-rtgipass--硬件光线追踪-gi)
8. [间接光在延迟光照中的集成](#8-间接光在延迟光照中的集成)
9. [技术对比总结](#9-技术对比总结)
10. [待实现技术](#10-待实现技术)

---

## 1. GI 架构总览

HugEngine 的 GI 分两套并行的体系：

### 1.1 光栅化路径 —— 统一 GI 子系统

所有光栅化 GI 技术都继承自统一接口 `IGlobalIllumination`（`Engine/Render/GI/GlobalIllumination.h`），该接口继承 `IRenderSubsystem`，在子系统生命周期（`Initialize` / `Update` / `Render` / `Bind` / `OnResize`）之上扩展了 GI 特有能力：

- **模式/质量切换**：`GIMode` + `GIQuality` 枚举；
- **间接光纹理暴露**：`GetIndirectDiffuseTexture()` / `GetIndirectSpecularTexture()` 供 PBR Shader 采样；
- **调试统计**：`GIDebugData` 供 ImGui 面板展示。

```cpp
// GlobalIllumination.h:14 —— GI 模式枚举（覆盖长期路线图）
enum class GIMode : u8 {
    None    = 0,   // 无 GI（仅直接光照）
    IBL     = 1,   // 基于图像的光照
    SSGI    = 2,   // 屏幕空间 GI
    VXGI    = 3,   // 体素锥追踪 GI（仅占位，未实现）
    DDGI    = 4,   // 动态漫反射 GI（探针网格 + SH）
    ReSTIR  = 5,   // 重采样 GI（仅占位，未实现）
    RSM     = 6,   // 反射阴影贴图 GI
};
```

**实际落地为 `.cpp` 实现的类**（`Engine/Render/GI/`）：

| 类 | GIMode | 状态 |
|---|---|---|
| `GI_None` | None | 空实现（Null Object，默认关闭）|
| `GI_IBL` | IBL | ✅ 已实现 |
| `GI_SSGI` | SSGI | ✅ 已实现 |
| `GI_SSR` | SSGI | ✅ 已实现（`GetMode()` 暂复用 SSGI）|
| `GI_RSM` | RSM | ✅ 已实现 |
| `GI_DDGI` | DDGI | ✅ 已实现 |

### 1.2 硬件光线追踪路径

独立于上述接口，位于 `Engine/Render/RT/`，继承 `RTEffectPass`：

- `RTGIPass` —— RT 间接漫反射（本文第 7 节）；
- `RTReflectionPass` / `RTAOPass` —— RT 反射 / RT 环境光遮蔽（间接光照相关效果）。

> 注意：`GIMode` 枚举中预留的 `VXGI`（体素锥追踪）与 `ReSTIR` **尚无实现类**，`Engine/Render/RT/ReSTIRPass` 实现的是 ReSTIR **直接光照（DI）** 重采样，非 GI。

---

## 2. GI_IBL —— 基于图像的光照

**实现文件**：`Engine/Render/GI/GI_IBL.cpp`、`Engine/Render/GI/GI_IBL.h`
**着色器**：`Engine/Shader/Shaders/GI/IBL_Irradiance.frag.slang`、`IBL_Prefilter.frag.slang`、`IBL_BRDF_LUT.frag.slang`

### 2.1 原理：Split-Sum 近似

PBR 的镜面 IBL 积分：

```
L_spec = ∫_Ω  L_i(L) · D·F·G / (4·NdotL·NdotV) · NdotL  dω
```

直接实时计算不可行，HugEngine 采用 UE4 的 **Split-Sum（分裂求和）近似**，把积分拆成两个可预计算的独立部分相乘：

```
L_spec ≈ (预滤波环境贴图) × (BRDF 积分 LUT)
```

其中漫反射部分用一张低分辨率的辐照度图（`Irradiance Map`）近似。因此 `GI_IBL` 从天空盒 Cubemap 预生成三张图：

| 纹理 | 分辨率 | 用途 |
|---|---|---|
| Irradiance Map | 32×32 Cubemap（1 mip）| 漫反射辐照度 |
| Prefilter Map | 128×128 Cubemap（5 mip）| 各 roughness 下的预滤波镜面反射 |
| BRDF LUT | 512×512 RG16F 2D | Split-Sum BRDF 积分查找表 |

### 2.2 生成流程

`GI_IBL::Render()` 用**光栅化全屏三角形 + 逐面 offscreen pass**（无需 Compute Shader），仅在天空盒变化（脏标记 `m_Dirty`）时重生成：

1. **辐照度图**：遍历 6 个面，每面渲染一次全屏三角形；
2. **预滤波图**：遍历 5 个 mip × 6 个面，`roughness = mip / 4`；
3. **BRDF LUT**：渲染一次 2D 全屏三角形。

```cpp
// GI_IBL.cpp —— 预滤波逐 mip 渲染，roughness 与 mip 线性映射
for (u32 mip = 0; mip < kPrefilterMips; ++mip) {
    u32 mipRes   = kPrefilterRes >> mip;  // 128, 64, 32, 16, 8
    pc.roughness = static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);
    for (u32 face = 0; face < rhi::kCubemapFaceCount; ++face) {
        void* mipView = m_Device->CreateTextureMipStorageView(m_PrefilterMap.get(), mip, face);
        // ... BeginOffscreenPass → Draw(3) 全屏三角形
    }
}
```

### 2.3 Shader 算法细节

**（1）辐照度卷积** —— `IBL_Irradiance.frag.slang`

对天空盒做半球方向黎曼和采样，累加 `cos(theta)` 加权（Lambertian），最后归一化：

```hlsl
// 球面坐标 → 切线空间 → 世界空间采样，cos(theta)·sin(theta) 为球面积分元
irradiance += color * cos(theta) * sin(theta);
irradiance = irradiance * 3.14159265 / float(sampleCount);  // π/N 归一化
```

采样步长 `SAMPLE_DELTA = 0.05`，对 32×32 的低频辐照度图足够。

**（2）预滤波环境贴图** —— `IBL_Prefilter.frag.slang`

用 **GGX 重要性采样**（256 samples）+ **Hammersley 低差异序列**减少方差：

```hlsl
// 低差异序列第二分量：Van der Corput 基数逆
float2 Hammersley(uint i, uint N) {
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}
// GGX 重要性采样（a = roughness²），把采样方向集中到 specular 波瓣内
float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness) {
    float a = roughness * roughness;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    // ... 由微面半程向量 H 推出采样方向 L
}
```

每个 mip 用更大的 roughness（采样方向更分散），从而实现对模糊反射的分级表示。

**（3）BRDF LUT** —— `IBL_BRDF_LUT.frag.slang`

横轴 `NdotV`、纵轴 `roughness`，预计算 Split-Sum 的 BRDF 部分，输出 `R=scale(DFG1)`、`G=bias(DFG2)`：

```hlsl
// IBL 用简化的 Smith 几何项（k = a²/2）
float G_Smith(float NdotV, float NdotL, float roughness) {
    float a = roughness * roughness;
    float k = a * a * 0.5;
    // ...
}
// 积分：A += (1-Fc)·G_Vis, B += Fc·G_Vis, Fc = (1-VdotH)^5（Fresnel-Schlick）
```

### 2.4 消费方式

在延迟光照 shader 中，`envBRDF` 采样一次同时服务直接光的多重散射能量补偿和 IBL 镜面项。IBL 的漫反射与镜面分别由 `SampleDiffuseSource(GISOURCE_IBL, …)` 与 `SampleSpecularSource(GISOURCE_IBL, …)` 产出，作为层栈里的普通源参与归一化合成：

```hlsl
// DeferredLighting.frag.slang —— IBL 漫反射 + 镜面反射
// 漫反射：辐照度图存 E/π，已含 kD 与接收面 albedo
return kD * u_IrradianceMap.Sample(u_IBLSampler, N).rgb * albedo * iblIntensity;
// 镜面：预滤波图按 roughness 选 mip，再乘 Split-Sum 的环境 BRDF
return u_PrefilterMap.SampleLevel(u_IBLSampler, R,
           roughness * (kGPUPrefilterMips - 1.0)).rgb
     * (F * envBRDF.r + envBRDF.g) * iblIntensity;
```

---

## 3. GI_SSGI —— 屏幕空间全局光照

**实现文件**：`Engine/Render/GI/GI_SSGI.cpp`
**着色器**：`Engine/Shader/Shaders/GI/SSGI.frag.slang`

### 3.1 原理

屏幕空间间接漫反射：对每个像素在**法线半球内采样 N 个方向**，沿方向在深度缓冲中做可见性检测，把可见采样点处的**入射辐射度** `L_in` 按余弦加权平均累加，得到 `Σ(L_in·cosθ)/Σcosθ`。因为 `∫cosθdω = π`，该估计量**精确等于 `E/π`**（归一化常数是解析值，不需要经验增益标定）。`L_in` 取自**前帧 HDR**（`GIRadianceHistory`），既解决因果（本 pass 排在 Lighting 之前）又构成多次弹射的时域反馈。这是一种廉价的单次反弹近似，只能利用屏幕内可见信息。

### 3.2 实现细节

**采样核**：CPU 侧用固定种子（`gen(42)`）预生成 32 个半球方向（`kSSGIKernelSize = 32`），并按索引距离分级（`scale = mix(0.1, 1.0, i²/N²)`），实现近密远疏；实际使用的采样数由 `sampleCount` 决定（默认 **16**）：

```cpp
// GI_SSGI.cpp —— 半球采样核生成（半径按索引平方分级）
static constexpr u32 kSSGIKernelSize = 32;
static void GenSSGISamples(std::vector<float4>& kernel, int count) {
    std::default_random_engine gen(42);   // 固定种子：采样核跨帧稳定，避免闪烁
    // s = normalize(随机半球方向) * rnd；scale 使远端采样更分散
    float scale = float(i)/float(count); scale = glm::mix(0.1f, 1.0f, scale*scale);
    kernel[i] = float4(s*scale, 0);
}
```

**参数传递**：采样核（32×float4）+ 参数 + 逆投影 + 正投影 + 视图矩阵共 **720 字节**，超出 Vulkan push constant 的 256 字节上限，改用 **Uniform Buffer**（binding 3）传递。

**着色器算法**（`SSGI.frag.slang`）：

```hlsl
// 1. 深度重建 view-space 位置（用逆投影矩阵）
float4 cp = float4(uv*2.0-1.0, depth, 1.0);
float3 viewPos = mul(u_InvProj, cp).xyz / mul(u_InvProj, cp).w;

// 2. GBuffer 法线是世界空间 → 用 u_View 转到 view 空间；再构造 TBN
// 3. 每个采样方向：切线空间样本经 mul(向量, TBN) 变到本空间，深度可见性检测
float3 sDir = mul(u_Samples[i].xyz, TBN);
float3 sDirN = normalize(sDir);
float  cosT  = max(0.0, dot(N, sDirN));
float3 sPos  = viewPos + sDir*radius;
float4 off   = mul(u_Proj, float4(sPos, 1.0)); off.xyz /= off.w;   // 正投影求屏幕 UV
float2 suv   = off.xy*0.5 + 0.5;
float  sd    = u_Depth.Sample(u_PointSampler, suv).r;              // 采样点深度
float  sZ    = /* sd 经逆投影反算的 view-space z */;
if (sZ <= sPos.z + 0.01) {                                         // 可见（未被遮挡）
    float3 L_in = u_Radiance.SampleLevel(u_PointSampler, suv, 0).rgb;  // 前帧 HDR 辐射度
    num += L_in * cosT;  den += cosT;                              // 余弦加权平均
}
float3 indirect = (den > 0.0) ? (num/den) : float3(0.0);           // = E/π
return float4(albedo * indirect * u_Params.y, 1.0);                // 乘自身 albedo 与强度
```

**关键点**：可见性判据是 `sZ <= sPos.z + 0.01`（view 空间朝 −Z，采样点未被遮挡 ⇔ 该像素保存的几何不比采样点更近）；`den == 0`（样本全被遮挡或落在下半球）时返回 0，不编数据。

---

## 4. GI_SSR —— 屏幕空间反射

**实现文件**：`Engine/Render/GI/GI_SSR.cpp`
**着色器**：`Engine/Shader/Shaders/GI/SSR.frag.slang`

### 4.1 原理

屏幕空间反射（Screen Space Reflection）：对每个像素计算反射向量，在深度缓冲中做 **Hi-Z 层次 march（屏幕空间 DDA，默认）**，命中处返回其 albedo 作为间接镜面反射；`useHiZ = false` 时回退到**线性 Ray Marching**（正式支持的回退路径）。

### 4.2 实现细节

默认参数（`GI_SSR.h`）：`maxSteps=64`、`stepSize=0.5`、`maxDistance=50`、`thickness=0.1`；`autoScaleMarch = true` 时由帧图按场景包围盒对角线推导（`maxDistance = 对角线`、`thickness = 对角线×0.0025`、`stepSize = thickness`、`maxSteps ≥ 256`），米制默认值在 3720 单位宽的场景里几乎什么都找不到。矩阵（逆投影 + 正投影 + 视图）经 Uniform Buffer（binding 3）传递，push constant 只剩标量（32 B，含 `useHiZ` flag）。

**着色器算法**（`SSR.frag.slang`）：

```hlsl
// Hi-Z 路径（u_UseHiZ>0）：先投影成屏幕段 s0→s1，再在段上做 DDA
//   屏幕参数 t 不是射线参数 → 必须做透视校正：
//   w(t) = w0·wT/((1−t)·wT + t·w0)，tau(t) = t·worldLen·w0/((1−t)·wT + t·w0)
//   rayPos = rayStart + R·tau，rayZ = (cA.z + tau·cR.z)/wq
//   射线点比该层最小深度更近 ⇒ 升到更粗层（大步）；被遮挡 ⇒ 回退更细层
float hiZ = u_HiZ.SampleLevel(u_HiZSampler, rpUV, level).r;
if (rayZ < hiZ) level = min(level + 1, kHiZMaxLevel);
else if (level == 0) { if (abs(rayPos.z - gZ) < thickness) { hit = 1; break; } }
else level--;

// 线性回退路径（u_UseHiZ==0）：逐步步进 + 同样的 view 空间厚度判据
if (abs(rayPos.z - rpZ) < thickness) { hitUV = rpUV; hit = 1; break; }
if ((rayPos.z - rpZ) * R.z > 0.0) break;   // 已越过该处几何 → 早退（不能记成命中）

// 命中：返回 albedo · 命中点 albedo · |N·R|
// 未命中：返回 float4(0,0,0,-1)
```

**关键点**：`thickness` 厚度带用于容忍浮点误差（世界空间量纲）；NR 与深度比较都在 **view 空间**，故 GBuffer 的**世界空间**法线必须先经 `u_View` 转成 view 空间再 `reflect`；命中返回 `alpha = +1`、未命中返回 `alpha = -1`，合成端据此跳过无效源。

---

## 5. GI_RSM —— 反射阴影贴图

**实现文件**：`Engine/Render/GI/GI_RSM.cpp`
**着色器**：`Engine/Shader/Shaders/GI/RSM_Generate.vert.slang`、`RSM_Generate.frag.slang`

### 5.1 原理

Reflective Shadow Maps：从**光源视角**渲染一次场景，把每个 texel 视为一个小的虚拟点光源（VPL），存储其**世界位置、法线、通量**。主渲染时，在 light space 采样这些 VPL，累加它们对当前像素的单次反弹漫反射贡献。本质是"光栅化单次反弹"的近似。

### 5.2 生成流程

从方向光的 `lightViewProj` 渲染场景，使用**三 MRT** 输出三张纹理（`GI_RSM.cpp` 的 `psoDesc.colorAttachmentCount = 3`，一个附件一个量）：

| MRT | 格式 | 内容 |
|---|---|---|
| 0 (`RSM_Position`) | RGBA16F | `worldPos.xyz` |
| 1 (`RSM_Normal`) | RGBA16F | `worldNormal.xyz`（编码 [0,1]）|
| 2 (`RSM_Radiance`) | RGBA16F | 该 VPL 的**出射辐射度** `L_v` |

```hlsl
// RSM_Generate.frag.slang —— VPL 出射辐射度 = albedo · 光源颜色 · 强度 · cos / π
float NdotL = max(dot(N, L), 0.0);
float3 radiance = vplAlbedo.rgb * light.colorIntensity.xyz
                * light.colorIntensity.w * NdotL * (1.0 / HE_PI);
output.vplRadiance = float4(radiance, 1.0);
```

**独立深度缓冲**：`GI_RSM` 使用自己的 D32 深度缓冲（`m_RSMDepth`），不再复用 CSM ShadowMap，避免布局冲突导致的白屏问题。

### 5.3 消费方式

RSM 间接光由**共享的半分辨率独立 pass** `RSMIndirect`（`GI/RSM_Indirect.frag.slang`）求值：16 点 Poisson 盘 VPL 求和（16×3 = 48 次纹理采样），光源 VP 与 `RSM_Generate` **同一个**按场景包围盒拟合的固定光锥；VPL 采样缩放由该光锥的半宽推出（`GI/RSMFrustum.h` 的 `RSMVplScale`），不再是经验常数。延迟光照 shader 只对该半分辨率结果做一次线性采样（binding 5）：

```hlsl
// DeferredLighting.frag.slang —— RSM 作为 diffuse 层栈里的普通源
case GISOURCE_RSM:
    // RSMIndirect 已乘 rsmVplScale（E，经验常数由光锥推出），此处补接收面 albedo
    return u_RSMIndirect.Sample(u_IBLSampler, uv).rgb * albedo;
```

> 原因见 `GI/RSMIndirect.h`：此前这段 32 次采样的求和写在 Lighting 里逐全分辨率像素执行，实测独占约 0.45 ms（Lighting 含 RSM 0.882 ms 对不含 0.433 ms）。

---

## 6. GI_DDGI —— 动态漫反射全局光照

**实现文件**：`Engine/Render/GI/GI_DDGI.cpp`、`Engine/Render/GI/GI_DDGI.h`
**着色器**：`Engine/Shader/Shaders/GI/DDGI.comp.slang`（探针更新）、`Engine/Shader/Shaders/RT_DDGI.slang`（探针查询共享库）

### 6.1 原理

Dynamic Diffuse GI：在场景中放置一个 **3D 探针网格**（默认按场景包围盒自动拟合），每个探针存储**二阶球谐（SH，band 0/1 共 4 系数）**表示的辐照度。每帧用 Compute Shader 对每个探针沿 Fibonacci 球面方向取辐射度 → SH 投影，并用时间混合收敛；主渲染时对包围点的 8 个最近探针做**三线性插值 + SH 评估（含 Lambert 卷积）**，得到平滑的间接漫反射。

### 6.2 探针数据结构

```cpp
// GI_DDGI.h —— 每探针 4 个 float4（步骤 30 的 L5 升级：原 16 个）
static constexpr u32 kFloats4PerProbe = 4;
// [0..3]  SH 系数 band 0/1（4 个系数，各占一个 float4 的 .rgb）
```

网格字段默认 `gridX=8, gridY=4, gridZ=8`（共 256 探针）、`cellSize=3.0`、`origin=(-10,-2,-10)`；但 `autoFitGrid = true`（默认）时由 `FitGridToBounds` 按场景包围盒覆盖：三轴共用同一格距，每轴探针数按自己的边长取 `ceil(size/cell)+1`，原点为包围盒最小角。`blendAlpha=0.85`；`updateStride=1`（每帧全量更新，>1 时按相位分摊）。

**双缓冲**：`m_ProbeBuffer`（当前帧写入）+ `m_ProbeHistory`（上一帧历史），每帧 swap 用于时间混合。

### 6.3 探针更新算法（DDGI.comp.slang）

每个 Compute 线程处理一个探针（`[numthreads(64, 1, 1)]`）：

1. **球面采样**：用 **Fibonacci 球面**生成 `numSamples` 个均匀分布方向（`GI_DDGI::kSamplesPerProbe = 32`，即 32 采样/探针），采样步长 `stepDist = cellSize * 0.4`；

```hlsl
// DDGI.comp.slang —— Fibonacci 球面：单位球上均匀分布的第 i 个方向
float3 FibonacciSphere(int i, int n) {
    float phi   = acos(1.0 - 2.0 * (float(i) + 0.5) / float(n));
    float theta = 3.14159265359 * (1.0 + sqrt(5.0)) * float(i);
    return float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
}
```

2. **辐射度来源（按优先级）**：Screen Probe（Lumen 的探针辐射度 SH，步骤 31）→ **光追 march**（`DDGI_Trace` 写入的 `u_TracedRadiance`，命中取命中点出射辐射度、未命中取 IBL 辐照度）→ **RSM 世界辐射度**（`u_RSMRadianceMap`，视角无关的单次反弹 VPL）→ **IBL 辐照度**（按方向采样，兜底）。**不再采样前帧 HDR**：屏幕空间来源受视锥限制（视锥外采样点被跳过 ⇒ 探针随相机变化），已改为世界空间、视角无关的来源；

```hlsl
// 兜底路径：按采样方向取 IBL 辐照度（世界空间、视角无关）
radiance = u_IBLIrradiance.SampleLevel(u_LinearSampler, dir, 0).rgb;
```

3. **SH 投影**：把辐射度投影到 **4 个 SH 系数（band 0/1）**，蒙特卡洛缩放因子 `4π/N × intensity`；

```hlsl
// DDGI.comp.slang —— 方向投影为 4 个 SH 系数（bands 0/1）
void SHBasis(float3 dir, out float sh[4]) {
    sh[0] = 0.28209479177387814;                       // l=0: sqrt(1/4π)
    float3 b1 = dir * 0.4886025119029199;              // l=1: sqrt(3/4π)
    sh[1] = b1.y; sh[2] = b1.z; sh[3] = b1.x;          // l=1: m=-1,0,1
}
// 蒙特卡洛积分归一化 4π/N × 强度
float scale = (4.0 * 3.14159265359) / validSamples * u_Params.x;
```

4. **时间混合**：与上一帧历史 SH 系数 lerp（`blendAlpha=0.85`），抑制闪烁、加速收敛：

```hlsl
// DDGI.comp.slang —— 时间混合（历史保留 85%）
sh[j].rgb = lerp(sh[j].rgb, hist.rgb, blendAlpha);
```

### 6.4 消费方式：三线性插值 + SH 评估

`RT_DDGI.slang` 提供共享查询库，被延迟光照与 RT GI 复用。网格参数**不再硬编码**，由 includer 声明 `u_DDGIGridOrigin` / `u_DDGIGridSize` cbuffer（与 `GI_DDGI::ProbeGridUniform` 前两个 float4 一致）：

```hlsl
// RT_DDGI.slang —— 世界坐标 → 网格坐标 → 8 个最近探针三线性插值
float3 gridCoord = (worldPos - u_DDGIGridOrigin.xyz) / u_DDGIGridSize.w;   // .w = cellSize
// 遍历 8 个包围探针，权重 = (1-|dx-frac.x|)·(1-|dy-frac.y|)·(1-|dz-frac.z|)
// 每个探针读取 4 个 SH 系数，用 EvalDDGI_SH(sh, normal) 评估辐照度（A_l 卷积系数在评估端施加）
```

消费点（`DeferredLighting.frag.slang` 的 `SampleDiffuseSource(GISOURCE_DDGI, …)`）：

```hlsl
// EvalDDGI_SH 已施加 Lambert 卷积 A_l → 返回辐照度 E → 需补 albedo/π
return SampleDDGI(worldPos, N) * albedo * (1.0 / HE_PI);
```

**前帧 HDR 辐射度**：已收敛为共享组件 `GIRadianceHistory`（`Engine/Render/GI/GIRadianceHistory.{h,cpp}`），由帧图的 `CaptureRadiance` pass 在 Lighting 之后捕获一次，只供声明了 `NeedsRadianceHistory()` 的源使用（当前为 SSGI 的 `L_in` 与 DDGI 的声明）；`GI_DDGI` 不再自持 `m_PrevHDR` 与下采样 pass。

---

## 7. RTGIPass —— 硬件光线追踪 GI

**实现文件**：`Engine/Render/RT/RTGIPass.cpp`、`Engine/Render/RT/RTGIPass.h`
**着色器**：`Engine/Shader/Shaders/RayTracing/RT_GI.rgen.slang`、`RT_GI.rchit.slang`、`RT_GI.rmiss.slang`

### 7.1 原理

硬件光追间接漫反射：对 GBuffer 有效像素（默认**每轴四分之一分辨率**，GI 为低频效果）在法线半球**余弦采样 N 条射线**，做**一次反弹**。ClosestHit 评估命中表面的出射辐射度；Miss 在 DDGI **不在漫反射层栈**时回退到 DDGI 探针查询（降级路径），否则只报告自己追踪到的部分。

**与 DDGI 的分工**（`RTGIPass.h`）：RT GI 覆盖**中距离（5–30m）**的精确间接光，DDGI 覆盖**远距离低频 GI**；二者都是漫反射层栈里的源，由合成端归一化加权（不再是无条件叠加）。

### 7.2 资源绑定（set0）

```cpp
// RTGIPass.cpp —— b0=TLAS, b1=输出, b2=GBDepth, b3=GBNormal,
// b4=材质纹理, b5=光源UB, b6=三角法线纹理, b7=DDGI探针SSBO(miss回退),
// b8=DDGI 探针网格参数 UBO（与 DeferredLighting 的 binding 6 同源）
```

**运行时 CVar 热更新**（`RTGIPass::Execute`）：

```cpp
pc.maxDistance  = std::max(cvRTGIMaxDist.Get(), 0.01f);  // GI 追踪范围（m）
pc.sampleCount  = std::clamp(cvRTGISPP.Get(), 1, 16);    // SPP（CPU 侧上限 16）
```

### 7.3 Shader 算法

**（1）RayGen** —— `RT_GI.rgen.slang`

```hlsl
// 余弦权重半球采样（绕法线 N），SPP 由 shader 再夹到 1–8
uint n = max(1u, min(g_PC.sampleCount, 8u));
float3 dir = CosineSampleHemisphere(N, u);
RayDesc ray; ray.Origin = worldPos + N * 0.02; ray.TMax = g_PC.maxDistance;
TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

if (payload.radianceT.a < 0.0) {
    // 源独立：DDGI 自己也在漫反射层栈里时 miss 贡献 0（否则 DDGI 信息被用两次）
    if ((g_PC.flags & 2u) == 0u)
        radiance += SampleDDGI(worldPos, dir) * (1.0 / 3.14159265);  // E → L 换算
} else {
    radiance += payload.radianceT.rgb;       // hit → 命中点出射辐射度
}
radiance /= float(n);
g_Output[idx] = float4(radiance, valid > 0 ? 1.0 : 0.0);
```

**（2）ClosestHit** —— `RT_GI.rchit.slang`

命中点评估出射辐射度（作为间接光来源）：

```hlsl
float3 N = LoadHitNormal(instanceID, primId, attr.barycentrics);      // 平滑法线
LoadHitMaterial(instanceID, albedo, metallic, roughness, emissive);   // 材质
float3 radiance = EvaluateHitRadiance(P, N, albedo, g_PC.lightCount); // 环境光 + 直接光源
payload.radianceT = float4(radiance, RayTCurrent());
```

**（3）Miss** —— `RT_GI.rmiss.slang`

未命中回退到程序化天空色（天光作为远距离间接漫反射来源），`a=-1` 标记未命中。

---

## 8. 间接光在延迟光照中的集成

`Engine/Shader/Shaders/Lighting/DeferredLighting.frag.slang` 是间接光（以及直接光）的最终消费点。**源的选择不再由 push constant 开关决定**：`rtDiffuseSource` / `rtSpecularSource` / `rtAOSource` 三个字段已随 Wave 1「按源 id 分派」移除，改为遍历 `GIBlendParams` UBO（binding 31）里的**通道层栈源数组**，按 `GISourceId` 分派采样。合成顺序：

```hlsl
// DeferredLighting.frag.slang —— 集成顺序（Wave 1 层栈合成）
1. 直接光照（Clustered Shading 或线性回退）+ CSM/聚光阴影
2. color *= rtShadowFactor;                      // RT 阴影整体因子（rtShadowSource != 0 时）
3. const float3 directColor = color;             // 记下直接光：AO 不作用于它
4. 间接漫反射：遍历 diffuse 通道层栈（IBL / DDGI / SSGI / Lumen / RSM / RTGI）
   num += SampleDiffuseSource(id,…) * w;  den += w;   // 权重来自 SourceWeight（用户权重 × 置信度）
   indirectDiffuse = (mode != 0) ? num/den : num;      // 归一化加权，无双重计数
   白炉：到此即 return directColor + indirectDiffuse
5. AO：遍历 ao 通道层栈（SSAO/GTAO/RTAO），只算遮蔽因子 → aoFactor
6. 间接镜面：遍历 specular 通道层栈（IBL / SSR / RT 反射 / Lumen），同构归一化
   SSR / RT 反射 / Lumen 先按 alpha 协议筛掉"本条无效"（alpha < 0）
7. color = directColor + (indirectDiffuse * giIntensity + indirectSpecular) * aoFactor;
   color += emissive;                              // 自发光最后加
8. 空中透视（大气消光 + 入射散射）
```

**AO 只作用于间接项**：直接光与自发光不乘 AO（`aoFactor = lerp(1, materialAO * aoVal, aoIntensity)` 只乘在 (间接漫反射 + 间接镜面) 上）。

**有效性协议（`alpha < 0`）**：屏幕空间与光追类源在"本条无效"（天空像素 / miss / 高粗糙度跳过）时返回 `float4(0,0,0,-1)`，合成端据此跳过该源（值不计入分子、**权重也不计入分母**）；缺了它，miss 的黑色会以全权重进入归一化、把环境反射压暗约一半。

**置信度**：`SourceWeight(s, camCoverage, gridCoverage, camDist)` = 用户权重 × 逐像素置信度（由 UBO 的 `confidence` 掩码按位给出）× 可选「距离让位」。屏幕覆盖位适用于 7 个源（SSGI/SSR/SSAO/GTAO/RTGI/RT 反射/RTAO），探针网格覆盖位只对 DDGI 置位。

---

## 9. 技术对比总结

| 技术 | 路径 | 反弹 | 覆盖范围 | 动态性 | 主要局限 |
|---|---|---|---|---|---|
| IBL | 光栅化 | 环境光 | 无限远（天空盒）| 仅天空盒变化时重生成 | 不响应局部动态物体 |
| SSGI | 光栅化 | 1 次 | 屏幕内 | 每帧实时 | 屏幕外信息缺失、有噪声 |
| SSR | 光栅化 | 1 次 | 屏幕内 | 每帧实时 | 屏幕外反射缺失、Ray March 有噪 |
| RSM | 光栅化 | 1 次 | 光源 POV | 每帧实时 | 无遮挡、VPL 密度受限 |
| DDGI | Compute | 多帧累积 | 探针网格体积 | 时间混合收敛 | 低频、光照变化有滞后 |
| RTGI | 硬件光追 | 1 次 | 场景全局 | 每帧实时 | 需硬件光追、SPP 低时有噪 |

---

## 10. 待实现技术

根据 `GIMode` 枚举与路线图，以下 GI 技术**尚未实现**：

- **VXGI（体素锥追踪）**：3D Clipmap → Cone Tracing，`GIMode::VXGI` 仅占位；
- **ReSTIR GI**：`GIMode::ReSTIR` 仅占位；当前 `ReSTIRPass` 实现的是直接光照（DI）重采样。

> 相关规划见 `docs/已实现功能/全路径追踪管线规划.md`、`docs/未实现功能/全流程RayTracing渲染实施规划.md`。
