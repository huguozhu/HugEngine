// DeferredLighting.frag — 延迟光照全屏片段着色器
// 读取 GBuffer MRT + Shadow/IBL/RSM 纹理，逐光源 PBR + 间接光
#include "pbr_common.slang"
// DDGI 探针网格参数（由 GI_DDGI 每帧上传，替代 RT_DDGI.slang 硬编码常量；
// 布局与 GI_DDGI.h 的 ProbeGridUniform 前两个 float4 一致）
[[vk::binding(kGPUBinding_DDGIGridParams, kGPUDescSet_PerFrame)]] cbuffer DDGIGridParams {
    float4 u_DDGIGridOrigin;  // xyz=世界空间原点
    float4 u_DDGIGridSize;    // xyz=探针数量(gridX/gridY/gridZ), w=cellSize
};
#include "RT_DDGI.slang"  // DDGI 探针查询共享库（EvalDDGI_SH + SampleDDGI）
#include "Atmosphere.slang"  // 空中透视（大气消光 + 入射散射）

struct VSOutput {
    float4 position          : SV_Position;
    [[vk::location(0)]] float2 uv;
};

// GBuffer 纹理
[[vk::binding(kGPUBinding_GBufferA, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferA;  // albedo.rgb + metallic
[[vk::binding(kGPUBinding_GBufferA, kGPUDescSet_PerFrame)]] SamplerState      u_GBufferSampler;
[[vk::binding(kGPUBinding_GBufferB, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferB;  // normal*0.5+0.5 + roughness
[[vk::binding(kGPUBinding_GBufferC, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferC;  // emissive.rgb + ao
[[vk::binding(kGPUBinding_Depth, kGPUDescSet_PerFrame)]] Texture2D<float>  u_Depth;
[[vk::binding(kGPUBinding_GBufferE, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferE;  // worldPos.xyz（GBuffer MRT4 直接读取）
[[vk::binding(kGPUBinding_GBufferF, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferF;  // disneyA：anisotropic, subsurface, specular, sheen（MRT5）
[[vk::binding(kGPUBinding_GBufferG, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferG;  // disneyB：clearcoat, clearcoatGloss, specularTint.r, specularTint.g（MRT6）
// 光照图键（任务 31）：GBuffer MRT7 = (uv0.x, uv0.y, objectIndex, 0)。
// 【为什么用点采样】键是**索引**：线性过滤会把相邻像素的页号与 UV 混成无意义的中间值
// （页号 3 与 4 之间插值出 3.5 = 另一个物体）。查表必须是无过滤的最近邻。
[[vk::binding(kGPUBinding_LightmapKey, kGPUDescSet_PerFrame)]] Texture2D<float4> u_LightmapKey;

// Shadow + IBL + RSM (复用 Forward PBR 绑定)
[[vk::binding(kGPUBinding_ShadowMap0, kGPUDescSet_PerFrame)]] Texture2D<float>  u_ShadowMap0;
[[vk::binding(kGPUBinding_ShadowMap0, kGPUDescSet_PerFrame)]] SamplerState      u_ShadowSampler;
[[vk::binding(kGPUBinding_SpotShadow_DL, kGPUDescSet_PerFrame)]] Texture2D<float>  u_SpotShadowMap;  // 聚光灯阴影贴图（Deferred 专用 binding 9）
[[vk::binding(kGPUBinding_ShadowMap1, kGPUDescSet_PerFrame)]] Texture2D<float> u_ShadowMap1;
[[vk::binding(kGPUBinding_ShadowMap2, kGPUDescSet_PerFrame)]] Texture2D<float> u_ShadowMap2;
[[vk::binding(kGPUBinding_IrradianceMap, kGPUDescSet_PerFrame)]] TextureCube<float4> u_IrradianceMap;
[[vk::binding(kGPUBinding_IrradianceMap, kGPUDescSet_PerFrame)]] SamplerState        u_IBLSampler;
[[vk::binding(kGPUBinding_PrefilterMap, kGPUDescSet_PerFrame)]] TextureCube<float4> u_PrefilterMap;
[[vk::binding(kGPUBinding_BRDF_LUT, kGPUDescSet_PerFrame)]] Texture2D<float2>   u_BRDF_LUT;
// RSM 贴图（15/16）：**本着色器不采样它们**。Deferred 的 RSM 源由共享的半分辨率 pass
// （GI/RSM_Indirect.frag.slang）求值，这里只采下面的 u_RSMIndirect（L_o/albedo = E/π）。
// 声明保留是为了与 Forward 的 PBR.frag（内联 RSM 求和，同样用 15/16）共用同一套 binding 号，
// 两套 per-frame set 的布局各自独立。任务 30 起 16 号只存编码法线（此前 .a 兼存通量）。
[[vk::binding(kGPUBinding_RSMPosition, kGPUDescSet_PerFrame)]] Texture2D<float4>   u_RSMPositionMap;
[[vk::binding(kGPUBinding_RSMFlux, kGPUDescSet_PerFrame)]] Texture2D<float4>   u_RSMNormalMap;
// RSM 间接光辐照度 E（半分辨率，由 GI/RSM_Indirect.frag.slang 产出；任务 16）
[[vk::binding(kGPUBinding_RSMIndirect, kGPUDescSet_PerFrame)]] Texture2D<float4> u_RSMIndirect;

// GPULight / GPUShadowData 定义来自 ShaderTypes.slang（C++/Slang 共享）

[[vk::binding(kGPUBinding_Lights_DL, kGPUDescSet_PerFrame)]] StructuredBuffer<GPULight>      u_Lights;
[[vk::binding(kGPUBinding_ShadowData_DL, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUShadowData> u_ShadowData;
[[vk::binding(kGPUBinding_SSGI, kGPUDescSet_PerFrame)]] Texture2D<float4> u_SSGI;  // SSGI 间接光照
[[vk::binding(kGPUBinding_SSAO_DL, kGPUDescSet_PerFrame)]] Texture2D<float4> u_SSAO;  // SSAO 环境光遮蔽
[[vk::binding(kGPUBinding_SSR, kGPUDescSet_PerFrame)]] Texture2D<float4> u_SSR;   // SSR 屏幕空间反射
[[vk::binding(kGPUBinding_Lumen, kGPUDescSet_PerFrame)]] Texture2D<float4> u_Lumen;  // Lumen 输出（间接漫反射/镜面共用）
[[vk::binding(kGPUBinding_DDGIProbes, kGPUDescSet_PerFrame)]] StructuredBuffer<float4> u_DDGIProbes;  // DDGI 探针数据（步骤 30：每探针 4 float4 的 SH）

// RT 效果输出纹理（由 DeferredPipeline 通过 LightingPass 绑定）
// 通过 push constant 的 rt*Source 字段在 shader 内选择 RT 或屏幕空间源
[[vk::binding(kGPUBinding_RT_ShadowMask, kGPUDescSet_PerFrame)]] Texture2D<float>  u_RT_ShadowMask;  // RT 阴影遮罩（0=全影, 1=无影）
[[vk::binding(kGPUBinding_RT_Reflection, kGPUDescSet_PerFrame)]] Texture2D<float4> u_RT_Reflection;  // RT 反射（rgb=反射色, a=有效标记）
[[vk::binding(kGPUBinding_RT_AO, kGPUDescSet_PerFrame)]] Texture2D<float>  u_RT_AO;          // RT 环境光遮蔽（0=全遮蔽, 1=无遮蔽）
[[vk::binding(kGPUBinding_RT_GI, kGPUDescSet_PerFrame)]] Texture2D<float4> u_RT_GI;          // RT 间接漫反射（rgb=间接光, a=有效标记）

// Clustered Shading: LightGrid + LightIndexList（每 cluster 只迭代影响它的光源）
struct LightGridCell { uint offset; uint count; };
[[vk::binding(kGPUBinding_LightGrid, kGPUDescSet_PerFrame)]]  StructuredBuffer<LightGridCell> u_LightGrid;       // 每 cluster 的光源列表头
[[vk::binding(kGPUBinding_LightIndexList, kGPUDescSet_PerFrame)]]  StructuredBuffer<uint>          u_LightIndexList;  // 扁平光源索引数组

// Cluster 网格参数（push constant 传入）
// worldPos 由 GBuffer MRT4 直接读取，无需 Camera invViewProj
[[vk::push_constant]] cbuffer LightingPC {
    float4   cameraPosition;
    uint     lightCount;
    float    iblIntensity;
    uint     clusterTilesX;     // screen 宽度方向 tile 数（0=禁用 clustered）
    uint     clusterTilesY;     // screen 高度方向 tile 数
    float    clusterNear;       // 近裁剪面
    float    clusterFar;        // 远裁剪面
    float    clusterLogFactor;  // log(far/near) 用于计算 Z slice
    uint     useClustered;      // 是否启用 clustered shading
    uint     rtShadowSource;    // 0=CSM, 1=RT 阴影（配合 u_RT_ShadowMask）
    // 注：rtAOSource/rtSpecularSource/rtDiffuseSource/useScreenGI/ddgiScale 已随
    // Wave 1「按源 id 分派」移除——源的选择与模式由层栈（UBO 源数组）表达
    float    giIntensity;       // 间接漫反射 GI 总强度（层栈合成总倍率）
    float    aoIntensity;       // AO 强度（0=无 AO，1=完整 AO）
    uint     _pad0;             // 对齐到 16 字节边界（atmosphere 需 16B 对齐）
    float4   atmosphere;        // sunDir.xyz（指向太阳）+ turbidity（空中透视）
    // GI 分层合成参数经 GIBlendParams UBO 传递（见 kGPUBinding_GIBlendParams）
};

// GI 各通道分层合成参数（UBO：3 通道 × 32B，避免 push constant 超限且便于层栈扩展）
[[vk::binding(kGPUBinding_GIBlendParams, kGPUDescSet_PerFrame)]] cbuffer GIBlendParamsCB {
    GIBlendParams u_GIBlendParams;
};

// kGPUPrefilterMips / kGPUClusterSlices 统一在 ShaderTypes.slang（kGPU*）

// ============================================================
// GI 分层合成（Wave 1）：源采样与权重
//
// 合成按「源 id」分派而不是固定语义槽 —— 新增一种 GI 算法只需：
//   ① ShaderTypes.slang 加 GISOURCE_XXX 常量（与 C++ GISourceId 同步）
//   ② 在下面的 SampleXxxSource 加一个 case
//   ③ 实现算法本体（Provider）
// 无需改动 UBO 结构、帧图映射或合成循环。
// ============================================================

/// 屏幕覆盖置信度：视口外为 0，靠近边缘 `edgeFade` 带宽内线性升到 1
///
/// 「屏幕外无数据」不是一个近似，而是硬事实：屏幕空间 march 走出视口就没有采样点，
/// 光追源的入射方向也是从本像素出发的。带宽由 UBO 的 `edgeFade` 给出（默认 5%），
/// 不再是着色器常量。注意它**不改变归一化总量**：置信度只决定「这个像素信不信这个源」，
/// 不信就把权重让给同通道的其他源（全部不信时该通道为 0）。
float CameraCoverageConfidence(float2 uv, float edgeFade) {
    float2 edge = min(uv, 1.0 - uv);
    return saturate(min(edge.x, edge.y) / max(edgeFade, 1e-5));
}

/// 探针网格覆盖置信度（DDGI 的**有效域**，§9.2-K / 任务 14）
///
/// 探针网格只在 AABB 内插值才有意义：网格外 `SampleDDGI` 只能把网格坐标 clamp 到边界探针
/// （贴边常数外推），于是"开着 DDGI"与"该处有一个常数 GI"变得不可区分，而且毫无标记。
/// 这里给网格外留**一格**的淡出带，再外面直接归零；置信度为 0 时归一化合成会把这份权重
/// 让给同通道的其他源（IBL / SSGI / RSM…），正是"没数据就别编数据"。
float ProbeGridConfidence(float3 worldPos) {
    float cell = max(u_DDGIGridSize.w, 1e-4);
    float3 rel = (worldPos - u_DDGIGridOrigin.xyz) / cell;      // 世界 → 网格坐标
    float3 hi  = u_DDGIGridSize.xyz - 1.0;                      // 探针索引范围 [0, N-1]
    float3 d   = min(rel, hi - rel);                            // 到最近边界的距离（单位：格）
    // d >= 0：在探针凸包内；d ∈ [-1,0)：刚出边界一格内，线性淡出；d < -1：完全在外
    return saturate(min(min(d.x, d.y), d.z) + 1.0);
}

/// 源权重 = 用户权重 × 置信度 × 可选「距离让位」
///
/// 置信度是**逐像素**的，判据由 UBO 的 `confidence` 掩码按位给出（见 ShaderTypes.slang
/// 的 GICONF_*）——着色器里不再硬编码任何源 id 列表，新增源无需改这里。
/// 世界空间源（IBL/DDGI/RSM）不依赖相机屏幕覆盖，其掩码里没有 CAMERA_COVERAGE 位；
/// DDGI 另有探针网格覆盖位（网格外无数据）。
float SourceWeight(GISourceSlot s, float camCoverage, float gridCoverage, float camDist) {
    float w = s.weight;
    if ((s.confidence & GICONF_CAMERA_COVERAGE) != 0u) w *= camCoverage;
    if ((s.confidence & GICONF_PROBE_GRID) != 0u)      w *= gridCoverage;
    if (s.falloffDistance > 0.0) w *= saturate(1.0 - camDist / s.falloffDistance);
    return w;
}

/// RSM 间接光（反射阴影贴图，单次反弹 VPL 求和）
///
/// 【任务 16 起这里是**采样**而不是求值】16 点 Poisson 盘 VPL 求和（16×2 = 32 次纹理采样）
/// 已搬到 `GI/RSM_Indirect.frag.slang` 的**半分辨率**独立 pass：
///   实测（HE_PASS_TIMING=1）Lighting 在漫反射层栈含 RSM 时 0.882 ms、不含时 0.433 ms，
///   这一项独占约 0.45 ms，是全部 GI 里最大的一笔。搬走后 Lighting 侧只做一次线性采样
///   （半分辨率 → 全分辨率的升采样），而该项本身是低频量（见该 shader 的文件头）。
/// 【门控】未产出时由 LightingPass 回绑黑色占位（= 无间接光），与 SSGI/SSR 同一约定。
///
/// 描述符索引 5 是本 set 里最后一个空位（见 ShaderTypes.slang 的 kGPUBinding_RSMIndirect）。

/// 漫反射源采样（按 GISourceId 分派）
///
/// 【统一量纲约定】所有 diffuse 源必须返回**已乘接收面 albedo 的间接出射辐射度**：
///     L_o = albedo × E/π
/// 基准来自 IBL：其辐照度图在卷积时已归一化为 E/π（见 IBL_Irradiance.frag 末尾的
/// `× π / sampleCount`），故 `kD × irradianceMap × albedo` 本身就是出射辐射度。
///
/// 为什么必须统一：归一化加权平均 `Σ(c·w)/Σw` 的前提是各源在估**同一个量**。
/// 量纲不一致会让结果随 albedo 偏移（§9.2-A），而白炉测试因源真值被短路而抓不到。
// ============================================================
// 【框架级有效性契约】（《Lumen设计与实现》§10 的 11.3）
//
// 任何 GI 源的输出统一用 **alpha 通道**表达"本条有没有数据"：
//     alpha >= 0 ⇒ 有效；alpha < 0 ⇒ **本条无数据**，合成端必须**跳过该源**
//（值不计入分子，**权重也不计入分母** —— 把无数据的源按 0 计入分母会无谓稀释同通道其它源，
//  表现为画面变暗且无法归因，GI 计划 §9.2-C 就是这么断出来的）。
//
// 【为什么收敛成一个函数】此前是逐源硬编码：diffuse 循环里只查了 Lumen，specular 循环里
// 查 SSR / RT_Reflection / Lumen —— 新增一个源就得记得再加一处分支，漏掉的那处就是"协议断裂"。
// 现在合成端**只读这一个约定**：三处循环都调 `SourceIsValid()`，新增源只需在这里加一行
// （或按约定返回 true），而不是在三处循环里各加一次。
// ============================================================
bool SourceIsValid(uint id, float2 uv) {
    switch (id) {
        case GISOURCE_LUMEN:         return u_Lumen.Sample(u_GBufferSampler, uv).a >= 0.0;
        case GISOURCE_SSR:           return u_SSR.Sample(u_GBufferSampler, uv).a >= 0.0;
        case GISOURCE_RT_REFLECTION: return u_RT_Reflection.Sample(u_GBufferSampler, uv).a >= 0.0;
        // 其余源（IBL / DDGI / RSM / SSGI / RTGI / AO）在整屏上恒有效：
        // 它们是"处处给估计"的源，没有逐像素无效的概念（网格/屏幕覆盖由置信度掩码表达）。
        default:                     return true;
    }
}

float3 SampleDiffuseSource(uint id, float3 worldPos, float3 N, float2 uv,
                           float3 albedo, float3 kD, bool furnace) {
    // 白炉：各源真值 = 1（只验合成数学，不验量纲）。
    // **例外是 SSGI**：白炉条件（全白环境 + 接收面 albedo = 1）恰好就是它的解析真值条件
    // （E/π = 1），所以白炉下让它走**真实路径** —— 这样白炉同时校验它的标度，而不是像
    // 以前那样被短路掩盖（§11.4 的「白炉只验归一化、不验量纲」正是这一项）。
    // SSGI 的 pass 自己也带白炉标志：把接收面 albedo 取 1 后，其输出即「估计的 E/π」。
    // 白炉：各源真值 = 1（只验合成数学，不验量纲）。
    // **例外是 SSGI 与 RTGI**：它们自己带白炉路径（把入射辐射度取真实的白炉条件），
    // 所以白炉下让它们走**真实路径** —— 这样白炉同时校验它们的标度与量纲，而不是被短路掩盖
    // （SSGI 是任务 10 做的，RTGI 是任务 33 做的：它此前经 `EvaluateHitRadiance` 返回
    //   `albedo × 辐照度`，比辐射度大 π 倍，而短路让它一直读不出这个错）。
    if (furnace && id != GISOURCE_SSGI && id != GISOURCE_RTGI) return float3(1.0, 1.0, 1.0);
    switch (id) {
    case GISOURCE_IBL:   // 基准：辐照度图存 E/π，已含 kD 与接收面 albedo
        return kD * u_IrradianceMap.Sample(u_IBLSampler, N).rgb * albedo * iblIntensity;
    case GISOURCE_DDGI:  // 低频：动态漫反射探针（世界空间）
        // 合成路径恒用真实辐照度——ddgiScale（GI_DDGI::debugScale）只用于调试可视化，
        // 若在此相乘会破坏能量守恒（归一化之内引入非物理缩放，Wave 0.6）
        // EvalDDGI_SH 已施加 Lambert 卷积 A_l → 返回**辐照度 E** → 需补 albedo/π
        return SampleDDGI(worldPos, N) * albedo * (1.0 / HE_PI);
    case GISOURCE_SSGI:  // 中频：屏幕空间（SSGI.frag 输出已含接收面 albedo）
        return u_SSGI.Sample(u_GBufferSampler, uv).rgb;
    case GISOURCE_LUMEN: // 虚拟化几何 GI：输出约定与 SSGI 相同（已含接收面 albedo 的 L_o = albedo·E/π）
        return u_Lumen.Sample(u_GBufferSampler, uv).rgb;
    case GISOURCE_RSM:   // 中频：反射阴影贴图（半分辨率 VPL 求和的结果，返回 E，经验常数已含 1/π）→ 补 albedo
        return u_RSMIndirect.Sample(u_IBLSampler, uv).rgb * albedo;
    case GISOURCE_RTGI:  // 高频：硬件光追（rgen 的余弦加权平均 ≈ E/π）→ 补接收面 albedo
        return u_RT_GI.Sample(u_GBufferSampler, uv).rgb * albedo;
    default:
        return float3(0.0);
    }
}

/// 镜面源采样（按 GISourceId 分派）
float3 SampleSpecularSource(uint id, float2 uv, float3 R, float roughness,
                            float3 F, float2 envBRDF) {
    switch (id) {
    case GISOURCE_IBL:   // 低频：环境预滤波（含菲涅耳与环境 BRDF）
        return u_PrefilterMap.SampleLevel(u_IBLSampler, R,
                   roughness * (kGPUPrefilterMips - 1.0)).rgb
             * (F * envBRDF.r + envBRDF.g) * iblIntensity;
    case GISOURCE_SSR:           return u_SSR.Sample(u_GBufferSampler, uv).rgb;
    case GISOURCE_RT_REFLECTION: return u_RT_Reflection.Sample(u_GBufferSampler, uv).rgb;
    case GISOURCE_LUMEN:         return u_Lumen.Sample(u_GBufferSampler, uv).rgb;   // Lumen 镜面（与漫反射共用一张输出）
    default:                     return float3(0.0);
    }
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;

    // 1. 反算世界坐标
    // 1. 世界坐标直接从 GBuffer MRT4 读取（无需 Camera 矩阵，避免视角耦合）
    float4 gbE = u_GBufferE.Sample(u_GBufferSampler, uv);
    float3 worldPos = gbE.xyz;
    // 电介质 F0（由 IOR 预计算，存于 GBuffer worldPos.a）
    float  dielectricF0 = gbE.a;
    // 深度仅用于 Clustered Shading 的 Z-slice 计算（非世界坐标重建）
    float  depth    = u_Depth.Sample(u_GBufferSampler, uv).r;

    // 2. 读取 GBuffer
    float4 gbA = u_GBufferA.Sample(u_GBufferSampler, uv);
    float4 gbB = u_GBufferB.Sample(u_GBufferSampler, uv);
    float4 gbC = u_GBufferC.Sample(u_GBufferSampler, uv);
    float4 gbF = u_GBufferF.Sample(u_GBufferSampler, uv);  // disneyA（MRT5）
    float4 gbG = u_GBufferG.Sample(u_GBufferSampler, uv);  // disneyB（MRT6）

    float3 albedo    = gbA.rgb;
    float  metallic  = gbA.a;
    float3 N         = normalize(gbB.rgb * 2.0 - 1.0);
    float  roughness = gbB.a;
    float  ao        = gbC.a;

    // Disney BSDF 参数（GBuffer 读取，透传给 PBR_BRDF）
    float4 disneyA = gbF;  // anisotropic, subsurface, specular, sheen
    float4 disneyB = gbG;  // clearcoat, clearcoatGloss, specularTint.r, specularTint.g
    // specularTint.b 未随 GBuffer 携带（通道不足舍弃），Deferred 端固定为 1.0（默认中性色调）
    float  disneyC = 1.0;

    float3 V = normalize(cameraPosition.xyz - worldPos);

    // BRDF LUT：用于直接光照的多重散射能量补偿，IBL 镜面项复用同一采样
    float2 envBRDF = u_BRDF_LUT.Sample(u_IBLSampler, float2(max(dot(N,V),0.0), roughness)).rg;

    float3 color = float3(0.0);

    // ============================================================
    // 白炉数值测试（Wave 0.2）——能量守恒的硬判据
    //
    // 白炉条件：全白环境（各方向入射辐射度 = 1）+ 表面 albedo = 1 + 关闭直接光 + 无自发光。
    // 此时**每个间接光源的真值都是 1**，因此正确的分层合成（权重归一化、权重和 = 1）
    // 必须恰好得到 1.0。任何 > 1 的结果都说明有源被加在归一化之外（双重计数）。
    //
    // 实现方式：不是绕过真实路径另写一份，而是把"白炉下的源真值"代入**同一条**真实合成
    // 代码，这样测的就是真正的合成数学（也能暴露 IBL 环境项、RSM 之类在归一化之外累加的项）。
    // ============================================================
    const bool furnace = (u_GIBlendParams.diffuse.furnaceMode != 0u);
    if (furnace) {
        albedo = float3(1.0, 1.0, 1.0);   // 全白反照率
    }

    // RT 阴影因子：RT 阴影遮罩覆盖所有阴影光源（全屏像素级乘积），
    // 在直接光照结算后整体乘一次；rtShadowSource=0 时使用传统逐光源 CSM
    float rtShadowFactor = (rtShadowSource != 0u)
        ? u_RT_ShadowMask.Sample(u_GBufferSampler, uv).r : 1.0;

    // 3. 直接光照（Clustered 或线性回退）
    if (useClustered != 0u && clusterTilesX > 0u) {
        // --- Clustered Shading ---
        uint tileX = uint(input.position.x) / kGPUClusterTileSize;
        uint tileY = uint(input.position.y) / kGPUClusterTileSize;
        float linearDepth = clusterNear * clusterFar / (clusterFar - depth * (clusterFar - clusterNear));
        float sliceF = float(kGPUClusterSlices) * log(linearDepth / clusterNear) / clusterLogFactor;
        uint  slice  = uint(clamp(sliceF, 0.0f, float(kGPUClusterSlices - 1)));
        uint clusterIdx = (slice * clusterTilesY + tileY) * clusterTilesX + tileX;

        LightGridCell cell = u_LightGrid[clusterIdx];
        uint endIdx = cell.offset + min(cell.count, 64u);
        for (uint li = cell.offset; li < endIdx; li++) {
            uint i = u_LightIndexList[li];
            GPULight light = u_Lights[i];
        uint ltype = uint(light.directionType.w);
        float3 L; float attenuation = 1.0;
        if (ltype == 0u) {
            L = normalize(-light.directionType.xyz);
        } else {
            float3 toLight = light.positionRange.xyz - worldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.001);
            float d = dist / max(abs(light.positionRange.w), 0.001);
            // 物理模式：positionRange.w < 0 → 平方反比衰减 + 范围软截止
            // 传统模式：positionRange.w > 0 → 多项式衰减
            if (light.positionRange.w < 0.0) {
                attenuation = 1.0 / max(d * d, 0.001);
                float fade = smoothstep(1.0, 0.8, d);
                attenuation *= fade * fade;  // 范围边缘软过渡
            } else {
                attenuation = 1.0 / (1.0 + 2.0 * d + d * d);
            }
            if (ltype == 2u) {
                float spotDot = dot(-L, normalize(light.directionType.xyz));
                attenuation *= smoothstep(light.coneAngles.y, light.coneAngles.x, spotDot);
            }
        }
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;

        float shadow = 1.0;
        if (light.shadowIndex >= 0 && rtShadowSource == 0u) {
            GPUShadowData sm = u_ShadowData[light.shadowIndex];
            if (sm.shadowParams.w < 0.5f) {
                // 方向光 CSM
                shadow = lerp(1.0, SampleShadowPCF(worldPos, N, L, sm, cameraPosition.xyz,
                    u_ShadowMap0, u_ShadowMap1, u_ShadowMap2, u_ShadowSampler), sm.shadowParams.z);
            } else if (sm.shadowParams.w > 1.5f) {
                // 聚光灯透视阴影（单采样 + PCF 3×3）
                float4 spotClip = mul(sm.lightViewProj[0], float4(worldPos + N * sm.shadowParams.y, 1.0));
                float3 spotNdc = spotClip.xyz / spotClip.w;
                float2 spotUv = float2(spotNdc.x * 0.5 + 0.5, 0.5 - spotNdc.y * 0.5);
                float  spotZ  = spotNdc.z - sm.shadowParams.x;
                float2 ts = 1.0 / 1024.0;
                float spotShadow = 0.0;
                if (spotUv.x > 0.0 && spotUv.x < 1.0 && spotUv.y > 0.0 && spotUv.y < 1.0) {
                    for (int sx = -1; sx <= 1; sx++)
                        for (int sy = -1; sy <= 1; sy++)
                            spotShadow += (spotZ <= u_SpotShadowMap.Sample(u_ShadowSampler, spotUv + float2(sx, sy) * ts)) ? 1.0 : 0.0;
                    shadow = spotShadow / 9.0;
                }
            }
        }
        color += PBR_BRDF(albedo, metallic, roughness, N, V, L, dielectricF0, envBRDF, disneyA, disneyB, disneyC) * radiance * shadow;
        }  // end clustered for
    } else {
        // --- 线性回退（Clustered 禁用时）---
        uint count = min(lightCount, 8u);
        for (uint i = 0u; i < count; i++) {
            GPULight light = u_Lights[i];
        uint ltype = uint(light.directionType.w);
        float3 L; float attenuation = 1.0;
        if (ltype == 0u) { L = normalize(-light.directionType.xyz); }
        else {
            float3 toLight = light.positionRange.xyz - worldPos;
            float dist = length(toLight); L = toLight / max(dist, 0.001);
            float d = dist / max(abs(light.positionRange.w), 0.001);
            if (light.positionRange.w < 0.0) {
                attenuation = 1.0 / max(d * d, 0.001);
                float fade = smoothstep(1.0, 0.8, d);
                attenuation *= fade * fade;
            } else {
                attenuation = 1.0 / (1.0 + 2.0 * d + d * d);
            }
            if (ltype == 2u) { float spotDot = dot(-L, normalize(light.directionType.xyz));
                attenuation *= smoothstep(cos(light.coneAngles.y), cos(light.coneAngles.x), spotDot); }
        }
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
        float shadow = 1.0;
        if (light.shadowIndex >= 0 && rtShadowSource == 0u) {
            GPUShadowData sm = u_ShadowData[light.shadowIndex];
            if (sm.shadowParams.w < 0.5f) {
                shadow = lerp(1.0, SampleShadowPCF(worldPos, N, L, sm, cameraPosition.xyz,
                    u_ShadowMap0, u_ShadowMap1, u_ShadowMap2, u_ShadowSampler), sm.shadowParams.z);
            } else if (sm.shadowParams.w > 1.5f) {
                float4 spotClip = mul(sm.lightViewProj[0], float4(worldPos + N * sm.shadowParams.y, 1.0));
                float3 spotNdc = spotClip.xyz / spotClip.w;
                float2 spotUv = float2(spotNdc.x * 0.5 + 0.5, 0.5 - spotNdc.y * 0.5);
                float  spotZ  = spotNdc.z - sm.shadowParams.x;
                float2 ts = 1.0 / 1024.0; float spotShadow = 0.0;
                if (spotUv.x > 0.0 && spotUv.x < 1.0 && spotUv.y > 0.0 && spotUv.y < 1.0) {
                    for (int sx = -1; sx <= 1; sx++)
                        for (int sy = -1; sy <= 1; sy++)
                            spotShadow += (spotZ <= u_SpotShadowMap.Sample(u_ShadowSampler, spotUv + float2(sx, sy) * ts)) ? 1.0 : 0.0;
                    shadow = spotShadow / 9.0;
                }
            }
        }
        color += PBR_BRDF(albedo, metallic, roughness, N, V, L, dielectricF0, envBRDF, disneyA, disneyB, disneyC) * radiance * shadow;
        }
    }

    // RT 阴影整体因子（覆盖所有阴影光源；rtShadowSource=0 时恒为 1.0）
    color *= rtShadowFactor;

    // 4. IBL / BRDF 公共项（供源分派使用）
    float3 F0_ibl = lerp(float3(dielectricF0), albedo, metallic);
    float3 F = F_SchlickRoughness(max(dot(N, V), 0.0), F0_ibl, roughness, metallic);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 R = reflect(-V, N);
    if (furnace) {
        // 白炉测试聚焦「合成数学」，不掺 BRDF 的菲涅耳分布：kD 取 1
        kD = float3(1.0, 1.0, 1.0);
    }

    // 相机距离（供「距离让位」用）。屏幕覆盖置信度**逐通道**计算：每个通道的边缘淡出
    // 带宽来自自己的 UBO 字段（`edgeFade`），不再是一个全局硬编码的 5%（§3.2）。
    float  camDist  = length(cameraPosition.xyz - worldPos);
    // 探针网格覆盖：只有带 GICONF_PROBE_GRID 位的源（当前仅 DDGI）会用到它
    const float gridCoverage = ProbeGridConfidence(worldPos);

    // ═══════════════════════════════════════════════════════════════
    // GI 分层合成（Wave 1：按源 id 分派，遍历通道的源数组）
    //
    // 每个通道的多个源描述的是**同一个物理量** → 归一化加权（权重和 = 1）避免双重计数。
    // IBL 与 RSM 也作为**普通源**参与（Wave 0.4 / 0.5）——不再有归一化之外的加法项。
    // 采样函数定义在文件作用域（见下方 SampleXxxSource / SourceWeight）。
    // ═══════════════════════════════════════════════════════════════

    // 直接光到此为止。AO 是**环境光遮蔽**：它近似的是「周围环境光被遮住多少」，
    // 不应作用于直接光（太阳/灯是明确方向的光源，与半球环境光无关），也不应作用于自发光。
    // 先把直接光结果记下来，最后只对间接项施加 AO（§9.2-D）。
    // 此前 `color *= lerp(1, ao*aoVal, aoIntensity)` 紧跟在直接光累加之后，于是每一帧都
    // 把直接光按材质 AO 压暗了一次 —— 那是实打实的能量错误，而不是遮蔽近似。
    const float3 directColor = color;

    // ── 间接漫反射合成 ──
    float3 indirectDiffuse = float3(0.0);
    {
        GIChannelBlendParams bl = u_GIBlendParams.diffuse;
        const float camCoverage = CameraCoverageConfidence(uv, bl.edgeFade);
        float3 num = float3(0.0);
        float  den = 0.0;
        for (uint i = 0u; i < bl.count; ++i) {
            GISourceSlot s = bl.sources[i];
            if (s.weight <= 0.0) continue;
            // 有效性契约（Lumen 骨架阶段的最小形态）：`alpha < 0` = 本条无数据，跳过而不计入
            // 权重。归一化的分母 den 若把它算进去，同通道其它源会被无端稀释（画面变暗且无法
            // 归因）。把这条契约推广到**所有**信号是《Lumen设计与实现》§10 的 11.3 的工作。
            if (s.id == GISOURCE_LUMEN && u_Lumen.Sample(u_GBufferSampler, uv).a < 0.0) continue;
            float w = SourceWeight(s, camCoverage, gridCoverage, camDist);
            num += SampleDiffuseSource(s.id, worldPos, N, uv, albedo, kD, furnace) * w;
            den += w;
        }
        indirectDiffuse = (bl.mode != 0u)
                        ? ((den > 0.0) ? (num / den) : float3(0.0))   // 归一化：无双重计数
                        : num;                                        // 相加（对照）
    }

    // ── 白炉数值测试：到此即返回 ──
    // 白炉下 diffuse 通道的间接光真值 = 1；此时**所有**源都已进入归一化，
    // 若仍读到 > 1，说明存在归一化之外的加法项（双重计数）。
    // giIntensity 是用户强度调节，判据下取 1 以便读回值即合成结果。
    if (furnace) {
        return float4(directColor + indirectDiffuse, 1.0);
    }

    // ── AO 合成（归一化加权：SSAO / RTAO 可同时参与）──
    // 只算出遮蔽因子，施加对象留给最后的合成：**间接漫反射与间接镜面**。
    // 原先把 AO 乘在「直接光 + 间接漫反射」上、且在间接镜面之前，于是既压暗了直接光，
    // 又完全没作用到镜面 —— 两处都是错的（§9.2-D）。
    float aoFactor = 1.0;
    {
        GIChannelBlendParams ba = u_GIBlendParams.ao;
        float aoNum = 0.0;
        float aoDen = 0.0;
        // 同样走 SourceWeight：AO 源也是「逐屏幕像素」的估计（SSAO/GTAO/RTAO 都是
        // CAMERA_COVERAGE 源），屏幕边缘处其数据同样不可信 —— 与 diffuse/specular 同源处理。
        // 边缘处所有权重都归零时 aoVal 取 1（不遮蔽），正是「该处只剩世界空间源」的语义。
        const float camCoverageAO = CameraCoverageConfidence(uv, ba.edgeFade);
        for (uint i = 0u; i < ba.count; ++i) {
            GISourceSlot s = ba.sources[i];
            if (s.weight <= 0.0) continue;
            // SSAO 与 GTAO 共用同一张 AO 输出（二者互为替代，不会同时启用）
            float v = (s.id == GISOURCE_RTAO)
                    ? u_RT_AO.Sample(u_GBufferSampler, uv).r
                    : u_SSAO.Sample(u_GBufferSampler, uv).r;
            float w = SourceWeight(s, camCoverageAO, gridCoverage, camDist);
            aoNum += v * w;
            aoDen += w;
        }
        float aoVal = (aoDen > 0.0) ? (aoNum / aoDen) : 1.0;   // 无 AO 源 → 不遮蔽
        aoFactor = lerp(1.0, ao * aoVal, aoIntensity);   // aoIntensity 控制 AO 强度（0=无 AO）
    }

    // ── 间接镜面合成（与 diffuse 同构）──
    // 环境镜面（IBL prefilter）与屏幕空间/光追反射描述同一物理量 → 归一化合成；
    // a<0 表示该源本条光线无效（高粗糙度跳过或 miss）→ 该源权重记 0，由其他源兜底
    float3 indirectSpecular = float3(0.0);
    {
        GIChannelBlendParams bs = u_GIBlendParams.specular;
        const float camCoverageSpec = CameraCoverageConfidence(uv, bs.edgeFade);
        float3 specNum = float3(0.0);
        float  specDen = 0.0;
        for (uint i = 0u; i < bs.count; ++i) {
            GISourceSlot s = bs.sources[i];
            if (s.weight <= 0.0) continue;
            if (s.id == GISOURCE_SSR) {
                if (u_SSR.Sample(u_GBufferSampler, uv).a < 0.0) continue;              // 本条无效
            } else if (s.id == GISOURCE_RT_REFLECTION) {
                if (u_RT_Reflection.Sample(u_GBufferSampler, uv).a < 0.0) continue;
            } else if (s.id == GISOURCE_LUMEN) {
                if (u_Lumen.Sample(u_GBufferSampler, uv).a < 0.0) continue;   // 骨架阶段无数据
            }
            float w = SourceWeight(s, camCoverageSpec, gridCoverage, camDist);
            specNum += SampleSpecularSource(s.id, uv, R, roughness, F, envBRDF) * w;
            specDen += w;
        }
        indirectSpecular = (bs.mode != 0u) ? (specNum / max(specDen, 1e-4)) : specNum;
    }

    // ── 合成：AO 只作用于间接项；直接光与自发光原样保留 ──
    // giIntensity 仍只作用于间接漫反射（与改动前一致）
    color = directColor + (indirectDiffuse * giIntensity + indirectSpecular) * aoFactor;

    color += gbC.rgb;  // emissive

    // 空中透视：对场景几何应用距离/高度相关大气消光 + 入射散射
    ApplyAerialPerspective(color, cameraPosition.xyz, worldPos, atmosphere.xyz, atmosphere.w);
    return float4(color, 1.0);
}
