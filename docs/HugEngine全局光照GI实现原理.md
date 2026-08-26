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
**着色器**：`Engine/Shader/Shaders/IBL_Irradiance.frag.slang`、`IBL_Prefilter.frag.slang`、`IBL_BRDF_LUT.frag.slang`

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
// GI_IBL.cpp:316 —— 预滤波逐 mip 渲染，roughness 与 mip 线性映射
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

在延迟光照 shader 中，`envBRDF` 采样一次同时服务直接光的多重散射能量补偿和 IBL 镜面项：

```hlsl
// DeferredLighting.frag.slang:239 —— IBL 漫反射 + 镜面反射
color += kD * u_IrradianceMap.Sample(u_IBLSampler, N).rgb * albedo * iblIntensity;
float3 R = reflect(-V, N);
float3 prefiltered = u_PrefilterMap.SampleLevel(u_IBLSampler, R, roughness * (kGPUPrefilterMips - 1.0)).rgb;
color += prefiltered * (F * envBRDF.r + envBRDF.g) * iblIntensity;
```

---

## 3. GI_SSGI —— 屏幕空间全局光照

**实现文件**：`Engine/Render/GI/GI_SSGI.cpp`
**着色器**：`Engine/Shader/Shaders/SSGI.frag.slang`

### 3.1 原理

屏幕空间间接漫反射：对每个像素在**法线半球内采样 N 个方向**，沿方向在深度缓冲中做可见性检测，把可见采样点处的 albedo 累积为间接光。是一种廉价的单次反弹近似，只能利用屏幕内可见信息。

### 3.2 实现细节

**采样核**：CPU 侧用固定种子（`gen(42)`）预生成 32 个半球方向，并按索引距离分级（`scale = mix(0.1, 1.0, i²/N²)`），实现近密远疏：

```cpp
// GI_SSGI.cpp:14 —— 半球采样核生成（半径按索引平方分级）
static void GenSSGISamples(std::vector<float4>& kernel, int count) {
    // s = normalize(随机半球方向) * rnd；scale 使远端采样更分散
    float scale = float(i)/float(count); scale = glm::mix(0.1f, 1.0f, scale*scale);
    kernel[i] = float4(s*scale, 0);
}
```

**参数传递**：因采样核 + 参数 + 逆投影矩阵共 592 字节，超出 Vulkan push constant 的 256 字节上限，改用 **Uniform Buffer**（binding 3）传递。

**着色器算法**（`SSGI.frag.slang`）：

```hlsl
// 1. 深度重建 view-space 位置
float4 cp = float4(uv*2.0-1.0, depth, 1.0);
float3 viewPos = mul(u_Proj, cp).xyz / mul(u_Proj, cp).w;  // u_Proj 为逆投影矩阵

// 2. 构造 TBN，将采样方向变换到世界/view 空间
// 3. 每个采样方向做深度可见性检测
float sd = u_Depth.Sample(u_PointSampler, suv).r;         // 采样点深度
float sZ = /* 采样点 view-space z */;
if (sZ >= sPos.z - 0.01) {                                // 可见（未遮挡）
    float3 sAlbedo = u_Albedo.Sample(u_PointSampler, suv).rgb;
    float falloff = 1.0 / (1.0 + dot(sDir, sDir) * radius); // 距离衰减
    indirect += sAlbedo * max(0, dot(N, sDir)) * falloff;   // 余弦加权 + albedo
}
indirect /= float(samples);
return float4(albedo * indirect * u_Params.y, 1.0);         // 乘自身 albedo 与强度
```

**关键点**：可见性判断用深度缓冲比较（`sZ >= sPos.z - 0.01`），若采样点深度在场景表面之后则视为被遮挡、跳过；否则累加其 albedo。

---

## 4. GI_SSR —— 屏幕空间反射

**实现文件**：`Engine/Render/GI/GI_SSR.cpp`
**着色器**：`Engine/Shader/Shaders/SSR.frag.slang`

### 4.1 原理

屏幕空间反射（Screen Space Reflection）：对每个像素计算反射向量，在深度缓冲中做**线性 Ray Marching**，命中处返回其 albedo 作为间接镜面反射。

### 4.2 实现细节

默认参数（`GI_SSR.h:40`）：`maxSteps=64`、`stepSize=0.5`、`maxDistance=50`、`thickness=0.1`，通过 push constant 传入。

**着色器算法**（`SSR.frag.slang`）：

```hlsl
float3 R = reflect(-V, N);                    // 反射方向
float3 rayPos = viewPos + R * 0.1;            // 起点偏移避免自交
float3 rayStep = R * stepSz;

for (float i = 0; i < maxSteps; i += 1.0) {
    rayPos += rayStep;
    if (length(rayPos - viewPos) > maxDist) break;
    // 步进点投影到屏幕 → 采样深度
    float rpDepth = u_Depth.Sample(u_PointSampler, rpUV).r;
    float rpZ = /* rpDepth 反算的 view-space z */;

    // 厚度带内命中（ray 深度 ≈ 表面深度）
    if (rpZ > rayPos.z - thickness && rpZ < rayPos.z + thickness) { hitUV = rpUV; hit = 1; break; }
    // 穿透检测（ray 已越过表面）
    if (rayPos.z > rpZ) { hitUV = rpUV; hit = 0.5; break; }
}
// 命中：返回 albedo * 命中点 albedo * |N·R|
float3 hitAlbedo = u_Albedo.Sample(u_PointSampler, hitUV).rgb;
return float4(albedo * hitAlbedo * NdotR * hit, 1.0);
```

**关键点**：`thickness` 厚度带用于容忍浮点误差；`hit=0.5` 的穿透情形给予半强度；`hit=1` 为精确命中。

---

## 5. GI_RSM —— 反射阴影贴图

**实现文件**：`Engine/Render/GI/GI_RSM.cpp`
**着色器**：`Engine/Shader/Shaders/RSM_Generate.vert.slang`、`RSM_Generate.frag.slang`

### 5.1 原理

Reflective Shadow Maps：从**光源视角**渲染一次场景，把每个 texel 视为一个小的虚拟点光源（VPL），存储其**世界位置、法线、通量**。主渲染时，在 light space 采样这些 VPL，累加它们对当前像素的单次反弹漫反射贡献。本质是"光栅化单次反弹"的近似。

### 5.2 生成流程

从方向光的 `lightViewProj` 渲染场景，使用**双 MRT** 输出两张纹理（`GI_RSM.cpp:94`）：

| MRT | 格式 | 内容 |
|---|---|---|
| 0 (`RSM_Position`) | RGBA16F | `worldPos.xyz` |
| 1 (`RSM_Flux`) | RGBA16F | `worldNormal.xyz`（编码 [0,1]）+ 通量 `flux` |

```hlsl
// RSM_Generate.frag.slang:43 —— 通量 = 光强 × cos(theta)（Lambert 一次反射）
float NdotL = max(dot(N, L), 0.0);
float flux = light.colorIntensity.w * NdotL;
output.normalAndFlux = float4(N * 0.5 + 0.5, flux);
```

**独立深度缓冲**：`GI_RSM.cpp:39` 注释指出 RSM 使用自己的 D32 深度缓冲，不再复用 CSM ShadowMap，避免布局冲突导致的白屏问题。

### 5.3 消费方式

延迟光照 shader 中，把当前像素投影到 light space，采样 5×5 邻域 VPL 并累加：

```hlsl
// DeferredLighting.frag.slang:249 —— RSM 5×5 邻域采样，几何衰减 1/d²
for (int dx = -2; dx <= 2; dx++) {
    for (int dy = -2; dy <= 2; dy++) {
        float4 ps = u_RSMPositionMap.Sample(u_IBLSampler, suv);  // VPL 位置
        float4 fs = u_RSMFluxMap.Sample(u_IBLSampler, suv);      // VPL 法线 + 通量
        float3 sN = normalize(fs.rgb * 2.0 - 1.0);
        float3 dir = ps.xyz - worldPos;
        float d2 = max(dot(dir, dir), 0.01);
        // 通量 × 接收面余弦 × VPL 出射余弦 × 距离平方反比
        rsmIndirect += fs.a * max(dot(N, normalize(dir)), 0.0) * max(dot(sN, -normalize(dir)), 0.0) / d2;
    }
}
color += rsmIndirect * 0.03 * iblIntensity;  // 强度缩放
```

---

## 6. GI_DDGI —— 动态漫反射全局光照

**实现文件**：`Engine/Render/GI/GI_DDGI.cpp`、`Engine/Render/GI/GI_DDGI.h`
**着色器**：`Engine/Shader/Shaders/DDGI.comp.slang`（探针更新）、`Engine/Shader/Shaders/RT_DDGI.slang`（探针查询共享库）

### 6.1 原理

Dynamic Diffuse GI：在场景中放置一个 **3D 探针网格**，每个探针存储**球谐函数（SH）系数**表示的辐照度。每帧用 Compute Shader 对每个探针做球面采样 GBuffer → SH 投影，并用时间混合收敛；主渲染时对包围点的 8 个最近探针做**三线性插值 + SH 评估**，得到平滑的间接漫反射。

### 6.2 探针数据结构

```cpp
// GI_DDGI.h:53 —— 每探针 16 个 float4
static constexpr u32 kFloats4PerProbe = 16;
// [0..8]  SH 系数 (band 0/1/2, 9×float4)
// [9..15] 保留（深度/可见性/偏移等，暂未使用）
```

默认网格：`gridX=8, gridY=4, gridZ=8`（共 256 探针），`cellSize=3.0`，`blendAlpha=0.85`。

**双缓冲**：`m_ProbeBuffer`（当前帧写入）+ `m_ProbeHistory`（上一帧历史），每帧 swap 用于时间混合。

### 6.3 探针更新算法（DDGI.comp.slang）

每个 Compute 线程处理一个探针（64 线程/组）：

1. **球面采样**：用 **Fibonacci 球面**生成 32 个均匀分布方向，采样步长 `stepDist = cellSize * 0.4`；

```hlsl
// DDGI.comp.slang:71 —— Fibonacci 球面：单位球上均匀分布的第 i 个方向
float3 FibonacciSphere(int i, int n) {
    float phi   = acos(1.0 - 2.0 * (float(i) + 0.5) / float(n));
    float theta = 3.14159265359 * (1.0 + sqrt(5.0)) * float(i);
    return float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
}
```

2. **世界 → 屏幕投影 + 可见性测试**：把采样点投影到屏幕，与 GBuffer 深度比较（Reverse-Z 约定：近=1，远=0），`ndc.z > gbufDepth + 0.003` 表示被遮挡则跳过；

```hlsl
// DDGI.comp.slang:138 —— Reverse-Z 深度可见性测试
if (ndc.z > gbufDepth + 0.003) continue;  // 采样点被遮挡
```

3. **辐射度读取**：采样**前帧 HDR 纹理**（`u_PrevHDR`）获取真实辐射度（包含完整 PBR+IBL+阴影），避免单纯 albedo 近似的失真；首帧 HDR 全零时回退到 `albedo * 0.03` 兜底；

```hlsl
// DDGI.comp.slang:147 —— 前帧 HDR 作为真实辐射度，首帧回退 albedo 近似
float3 radiance = u_PrevHDR.SampleLevel(u_LinearSampler, uv, 0).rgb;
if (dot(radiance, radiance) < 0.0001) {
    float3 albedo = u_GBufferA.SampleLevel(u_PointSampler, uv, 0).rgb;
    radiance = albedo * 0.03;  // 首帧兜底
}
```

4. **SH 投影**：把辐射度投影到 9 个 SH 系数（band 0/1/2），蒙特卡洛缩放因子 `4π/N`；

```hlsl
// DDGI.comp.slang:59 —— 方向投影为 9 个 SH 系数（bands 0/1/2）
void SHBasis(float3 dir, out float sh[9]) {
    sh[0] = 0.28209479177387814;           // l=0: sqrt(1/4π)
    // l=1: sqrt(3/4π) * dir，l=2: 5 个二次项系数
}
// DDGI.comp.slang:165 —— 蒙特卡洛积分归一化 4π/N
float scale = (4.0 * 3.14159265359) / validSamples;
```

5. **时间混合**：与上一帧历史 SH 系数 lerp（`blendAlpha=0.85`），抑制闪烁、加速收敛：

```hlsl
// DDGI.comp.slang:176 —— 时间混合（历史保留 85%）
sh[j].rgb = lerp(sh[j].rgb, hist.rgb, blendAlpha);
```

### 6.4 消费方式：三线性插值 + SH 评估

`RT_DDGI.slang` 提供共享查询库，被延迟光照与 RT GI 复用：

```hlsl
// RT_DDGI.slang:37 —— 世界坐标 → 网格坐标 → 8 个最近探针三线性插值
float3 SampleDDGI(float3 worldPos, float3 normal) {
    float3 gridCoord = (worldPos - kDDGI_Origin) / kDDGI_CellSize;
    int3   baseCoord = int3(floor(gridCoord));
    float3 frac      = frac(gridCoord);
    // 遍历 8 个包围探针，权重 = (1-|dx-frac.x|)·(1-|dy-frac.y|)·(1-|dz-frac.z|)
    // 每个探针读取 9 个 SH 系数，用 EvalDDGI_SH(sh, normal) 评估辐照度
}
```

消费点（`DeferredLighting.frag.slang:274`）：

```hlsl
float3 ddgi = SampleDDGI(worldPos, N);
color += ddgi * 0.5;  // 与 SSGI 叠加，强度缩放避免过度照亮
```

**`CaptureHDR` 机制**（`GI_DDGI.cpp:212`）：每帧把 Lighting 输出 `CopyTextureToTexture` 到 `m_PrevHDR`，供下一帧探针采样真实辐射度，形成反馈回路。

---

## 7. RTGIPass —— 硬件光线追踪 GI

**实现文件**：`Engine/Render/RT/RTGIPass.cpp`、`Engine/Render/RT/RTGIPass.h`
**着色器**：`Engine/Shader/Shaders/RT_GI.rgen.slang`、`RT_GI.rchit.slang`、`RT_GI.rmiss.slang`

### 7.1 原理

硬件光追间接漫反射：对 GBuffer 有效像素（默认**四分之一分辨率**，GI 为低频效果）在法线半球**余弦采样 N 条射线**，做**一次反弹**。ClosestHit 评估命中表面的出射辐射度，Miss 回退到 DDGI 探针查询（而非纯天空色）。

**与 DDGI 的分工**（`RTGIPass.h:16`）：RT GI 覆盖**中距离（5–30m）**的精确间接光，DDGI 覆盖**远距离低频 GI**，二者在 LightingPass 中叠加。

### 7.2 资源绑定（set0）

```cpp
// RTGIPass.cpp:35 —— b0=TLAS, b1=输出, b2=GBDepth, b3=GBNormal,
// b4=材质纹理, b5=光源UB, b6=三角法线纹理, b7=DDGI探针SSBO(miss回退)
```

**运行时 CVar 热更新**（`RTGIPass.cpp:153`）：

```cpp
pc.maxDistance  = std::max(cvRTGIMaxDist.Get(), 0.01f);  // GI 追踪范围（m）
pc.sampleCount  = std::clamp(cvRTGISPP.Get(), 1, 16);    // SPP（时域累积提升质量）
```

### 7.3 Shader 算法

**（1）RayGen** —— `RT_GI.rgen.slang`

```hlsl
// 余弦权重半球采样（绕法线 N）
float3 dir = CosineSampleHemisphere(N, u);
RayDesc ray; ray.Origin = worldPos + N * 0.02; ray.TMax = g_PC.maxDistance;
TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

if (payload.radianceT.a < 0.0) {
    radiance += SampleDDGI(worldPos, dir);   // miss → DDGI 探针回退
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

`Engine/Shader/Shaders/DeferredLighting.frag.slang` 是间接光（以及直接光）的最终消费点。间接光累加顺序：

```hlsl
// DeferredLighting.frag.slang —— 间接光集成顺序
1. 直接光照（Clustered Shading 或线性回退）+ CSM/RT 阴影
2. color *= rtShadowFactor;                                    // RT 阴影整体因子
3. IBL   —— kD·irradiance·albedo + prefiltered·(F·envBRDF.r+envBRDF.g)  // 第 2 节
4. RSM   —— 5×5 邻域 VPL 累加 ·0.03                             // 第 5 节
5. AO    —— GBuffer AO × (SSAO 或 RT AO)
6. SSGI / RTGI —— rtDiffuseSource 选择屏幕空间或 RT 输出        // 第 3/7 节
7. DDGI  —— SampleDDGI(worldPos, N) ·0.5                       // 第 6 节
8. SSR / RT反射 —— rtSpecularSource 选择                        // 第 4 节
9. emissive + 空中透视（大气消光）
```

**运行时源切换**：通过 push constant 的 `rtDiffuseSource` / `rtSpecularSource` / `rtAOSource` 字段在 shader 内选择 RT 或屏幕空间实现，实现混合管线下的动态降级。

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

> 相关规划见 `docs/未实现功能/全路径追踪管线规划.md`、`docs/未实现功能/全流程RayTracing渲染实施规划.md`。
