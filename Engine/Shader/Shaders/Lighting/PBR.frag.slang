// ============================================================
// PBR.frag.slang — PBR 片元着色器 (GPU-driven)
// 支持 Forward+（Cluster 光源剔除）和传统 Forward 两种模式
// 通过 push constant useClustered 运行时切换
// ============================================================

#include "pbr_common.slang"
#include "Atmosphere.slang"  // 空中透视（大气消光 + 入射散射）

struct FSInput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] nointerpolation uint objectIndex;  // 从 VS 传入
};

// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// GPULight 定义来自 ShaderTypes.slang（C++/Slang 共享）

// GPUShadowData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// Descriptor Sets:
//   set=0: per-frame 动态数据
//     binding=1: GPULight[]
//     binding=2: GPUObjectData[]
//     binding=3: GPUShadowData[]
//     binding=4: ShadowMap CSM cascade 0 (CombinedImageSampler)
//     binding=7: LightGrid（Forward+ 光源列表头）
//     binding=8: LightIndexList（Forward+ 光源索引数组）
//     binding=9: PointShadow Cubemap
//     binding=10-11: CSM cascade 1-2
//     binding=12-14: IBL (Irradiance/Prefilter/BRDF LUT)
//     binding=15-16: RSM (Position/Flux)
//   set=1: per-mesh 静态纹理（永不更新，避免帧间竞态）
//     binding=5: BaseColor + Sampler
//     binding=6: Normal + Sampler
//     binding=7: MetallicRoughness + Sampler
//     binding=8: Occlusion + Sampler
[[vk::binding(kGPUBinding_Lights, kGPUDescSet_PerFrame)]] StructuredBuffer<GPULight> u_Lights;
[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_Objects;
[[vk::binding(kGPUBinding_ShadowData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUShadowData> u_ShadowData;
[[vk::binding(kGPUBinding_ShadowMap0, kGPUDescSet_PerFrame)]] Texture2D<float> u_ShadowMap0;   // CSM cascade 0
[[vk::binding(kGPUBinding_ShadowMap0, kGPUDescSet_PerFrame)]] SamplerState u_ShadowSampler;
[[vk::binding(kGPUBinding_ShadowMap1, kGPUDescSet_PerFrame)]] Texture2D<float> u_ShadowMap1;  // CSM cascade 1
[[vk::binding(kGPUBinding_ShadowMap2, kGPUDescSet_PerFrame)]] Texture2D<float> u_ShadowMap2;  // CSM cascade 2
// Bindless 纹理通过 u_Textures[] / u_Samplers[] 访问（common.slang 声明）
// 偏移: materialID + 0 = BaseColor, +1 = Normal, +2 = MetallicRoughness, +3 = Occlusion
[[vk::binding(kGPUBinding_PointShadow, kGPUDescSet_PerFrame)]] TextureCube<float> u_PointShadowCube;
[[vk::binding(kGPUBinding_PointShadow, kGPUDescSet_PerFrame)]] SamplerState u_PointShadowSampler;
[[vk::binding(kGPUBinding_SpotShadow, kGPUDescSet_PerFrame)]] Texture2D<float> u_SpotShadowMap;   // 聚光灯阴影贴图（独立 binding）
[[vk::binding(kGPUBinding_SpotShadow, kGPUDescSet_PerFrame)]] SamplerState u_SpotShadowSampler;
[[vk::binding(kGPUBinding_RectShadow, kGPUDescSet_PerFrame)]] Texture2D<float> u_RectShadowMap;   // 矩形面光阴影贴图（独立 binding）
[[vk::binding(kGPUBinding_RectShadow, kGPUDescSet_PerFrame)]] SamplerState u_RectShadowSampler;

// Forward+ LightGrid 和 LightIndexList（Cluster 光源剔除结果）
struct LightGridCell { uint offset; uint count; };
[[vk::binding(kGPUBinding_LightGrid, kGPUDescSet_PerFrame)]]  StructuredBuffer<LightGridCell> u_LightGrid;
[[vk::binding(kGPUBinding_LightIndexList, kGPUDescSet_PerFrame)]]  StructuredBuffer<uint>          u_LightIndexList;

// IBL 纹理
[[vk::binding(kGPUBinding_IrradianceMap, kGPUDescSet_PerFrame)]] TextureCube<float4> u_IrradianceMap;
[[vk::binding(kGPUBinding_IrradianceMap, kGPUDescSet_PerFrame)]] SamplerState       u_IBLSampler;
[[vk::binding(kGPUBinding_PrefilterMap, kGPUDescSet_PerFrame)]] TextureCube<float4> u_PrefilterMap;
[[vk::binding(kGPUBinding_BRDF_LUT, kGPUDescSet_PerFrame)]] Texture2D<float2>   u_BRDF_LUT;

// RSM 纹理（Reflective Shadow Maps）
[[vk::binding(kGPUBinding_RSMPosition, kGPUDescSet_PerFrame)]] Texture2D<float4> u_RSMPositionMap;   // worldPos.xyz
[[vk::binding(kGPUBinding_RSMPosition, kGPUDescSet_PerFrame)]] SamplerState      u_RSMSampler;
[[vk::binding(kGPUBinding_RSMFlux, kGPUDescSet_PerFrame)]] Texture2D<float4> u_RSMFluxMap;       // normal.xy + flux

// GI 分层合成参数（任务 26）：与 Deferred 侧**同一份布局与同一份语义** —— Forward 的 IBL/RSM
// 不再是"管线级开关 + 硬编码求和"，而是和 Deferred 一样：逐通道、按源数组、归一化加权。
// 绑定号复用 kGPUBinding_GIBlendParams（Forward 的 per-frame set 里它是空的）。
[[vk::binding(kGPUBinding_GIBlendParams, kGPUDescSet_PerFrame)]] cbuffer GIBlendParamsCB {
    GIBlendParams u_GIBlendParams;
};

[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;
    float4   cameraPosition;
    uint     lightCount;
    uint     objectIndex;
    uint     useInstanceID;
    float    iblIntensity;
    uint     clusterTilesX;     // Cluster X 方向 tile 数（0=传统模式）
    uint     clusterTilesY;     // Cluster Y 方向 tile 数
    float    clusterNear;       // 近裁剪面
    float    clusterFar;        // 远裁剪面
    float    clusterLogFactor;  // log(far/near) 用于 Z slice 计算
    uint     useClustered;      // 1=Forward+，0=传统 Forward
    uint     useBindlessMaterial;  // 1=从 u_Materials[] 读材质参数（bindless），0=从 GPUObjectData 内联读
    float4   atmosphere;        // sunDir.xyz（指向太阳）+ turbidity（空中透视）
    uint     instanceSSBOHandle;  // B1 实例化网格：FS 不使用，仅占位对齐 VS
    uint     instancePad[3];
};

// ACESFilm 定义在 common.slang 中，此处直接使用

// ---- 光源遍历：传统 Forward 与 Forward+ 共用 PBR 计算 ----
float3 ShadeWithLight(GPULight light, float3 worldPos, float3 N, float3 V,
                       float3 albedo, float metallic, float roughness,
                       float dielectricF0, float2 envBRDF,
                       float4 disneyA, float4 disneyB, float disneyC) {  // 电介质 F0（由 IOR 预计算）
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
            // 锥内 spotDot ∈ [cos(outer), 1]；用 cos 阈值做平滑衰减（锥内亮、锥外无光）。
            // 原用弧度角(outer,inner)当 edge 与 cos 值比较且 edge0>edge1，导致锥内映射为 0（光照反了）。
            attenuation *= smoothstep(cos(light.coneAngles.y), cos(light.coneAngles.x), spotDot);
        }
        if (ltype == 3u) {
            // 矩形面光：朝向（面法线）敏感 + 面积因子 + 距离衰减
            float3 rectN = normalize(light.directionType.xyz);   // 发光面法线
            float2 rectWH = light.coneAngles;                     // x=宽, y=高
            float facing = dot(rectN, -L);   // 面法线 vs 光传播方向（面光→表面）；正面发光、背面不发光
            // 面积因子：矩形面积 / 距离²（面光展宽，越靠近越亮）
            float areaF = (rectWH.x * rectWH.y) / max(dist * dist, 0.001);
            attenuation *= areaF * clamp(facing, 0.0, 1.0);
        }
    }
    float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;

    // 阴影采样
    float shadow = 1.0;
    if (light.shadowIndex >= 0) {
        GPUShadowData sm = u_ShadowData[light.shadowIndex];
        if (sm.shadowParams.w < 0.5f) {
            shadow = lerp(1.0, SampleShadowPCF(worldPos, N, L, sm,
                          cameraPosition.xyz,
                          u_ShadowMap0, u_ShadowMap1, u_ShadowMap2,
                          u_ShadowSampler),
                          sm.shadowParams.z);
        } else if (sm.shadowParams.w > 1.5f) {
            // 聚光灯阴影：透视投影到 2D 阴影贴图
            float4 spotClip = mul(sm.lightViewProj[0], float4(worldPos + N * sm.shadowParams.y, 1.0));
            float3 spotNdc = spotClip.xyz / spotClip.w;
            float2 spotUv = float2(spotNdc.x * 0.5 + 0.5, 0.5 - spotNdc.y * 0.5);
            float  spotZ  = spotNdc.z - sm.shadowParams.x;
            float2 ts = 1.0 / 1024.0;
            float spotShadow = 0.0;
            if (spotUv.x > 0.0 && spotUv.x < 1.0 && spotUv.y > 0.0 && spotUv.y < 1.0) {
                for (int sx = -1; sx <= 1; sx++)
                    for (int sy = -1; sy <= 1; sy++)
                        spotShadow += (spotZ <= u_SpotShadowMap.Sample(u_SpotShadowSampler, spotUv + float2(sx, sy) * ts)) ? 1.0 : 0.0;
                shadow = spotShadow / 9.0;
            }
            shadow = lerp(1.0, shadow, sm.shadowParams.z);
        } else if (sm.shadowParams.w > 2.5f) {
            // 矩形面光阴影：透视投影 + 大半径 PCF 软阴影（softness 控制半影宽度）
            float4 rectClip = mul(sm.lightViewProj[0], float4(worldPos + N * sm.shadowParams.y, 1.0));
            float3 rectNdc = rectClip.xyz / rectClip.w;
            float2 rectUv = float2(rectNdc.x * 0.5 + 0.5, 0.5 - rectNdc.y * 0.5);
            float  rectZ  = rectNdc.z - sm.shadowParams.x;
            float softness = sm.splitDistances.w;              // 软阴影系数 [0,1]
            float2 ts = (1.0 / 1024.0) * (1.0 + softness * 4.0); // PCF 采样半径随 softness 放大
            float rectShadow = 0.0;
            if (rectUv.x > 0.0 && rectUv.x < 1.0 && rectUv.y > 0.0 && rectUv.y < 1.0) {
                for (int sx = -2; sx <= 2; sx++)
                    for (int sy = -2; sy <= 2; sy++)
                        rectShadow += (rectZ <= u_RectShadowMap.Sample(u_RectShadowSampler, rectUv + float2(sx, sy) * ts)) ? 1.0 : 0.0;
                shadow = rectShadow / 25.0;
            }
            shadow = lerp(1.0, shadow, sm.shadowParams.z);
        } else {
            float3 lightPos = sm.pointLightData.xyz;
            float  range    = sm.pointLightData.w;
            shadow = lerp(1.0, SamplePointShadow(worldPos, lightPos, range,
                          sm, u_PointShadowCube, u_PointShadowSampler),
                          sm.shadowParams.z);
        }
    }

    return PBR_BRDF(albedo, metallic, roughness, N, V, L, dielectricF0, envBRDF,
                    disneyA, disneyB, disneyC) * radiance * shadow;
}

// ============================================================
// 间接光源分派（任务 26）：与 DeferredLighting.frag 的 SampleDiffuseSource /
// SampleSpecularSource 一一对应 —— 同名、同签名形状、同量纲（都返回"该源估计的间接出射
// 辐射度"）。Forward 目前只支持世界空间源，故这里只有 IBL 与 RSM 两个分支；
// 将来往 Forward 加源，照着加一个 case 即可，合成循环不必改。
// ============================================================

/// RSM 间接光（反射阴影贴图，5×5 VPL 求和）—— 从 fragmentMain 里提出来供分派调用
///
/// 【与改前的差别】不再由 `iblIntensity` 门控：RSM 与 IBL 是两个独立的源，"把 IBL 调暗会
/// 连带关掉 RSM" 是 §9.2-A 修过的耦合，Forward 侧这里是它的残留，一并去掉。
/// 【保留的有效性判据】必须是有阴影的方向光：点光/聚光场景的 `u_ShadowData[0]` 没有
/// `lightViewProj`（零矩阵），照做会得到垃圾坐标 → 阴影区出现"钱币"伪影。
float3 FwdSampleRSMIndirect(float3 worldPos, float3 N) {
    if (u_ShadowData[0].shadowParams.z <= 0.0 || u_ShadowData[0].shadowParams.w >= 0.5) {
        return float3(0.0);
    }
    float4 lightSpacePos = mul(u_ShadowData[0].lightViewProj[0], float4(worldPos, 1.0));
    float2 rsmCoord = lightSpacePos.xy / lightSpacePos.w * 0.5 + 0.5;
    float3 rsmIndirect = float3(0.0);
    const float rsmRadius = 0.005;
    for (int dx = -2; dx <= 2; dx++) {
        for (int dy = -2; dy <= 2; dy++) {
            float2 uv = saturate(rsmCoord + float2(dx, dy) * rsmRadius);
            float4 posSample  = u_RSMPositionMap.Sample(u_RSMSampler, uv);
            float4 fluxSample = u_RSMFluxMap.Sample(u_RSMSampler, uv);
            float3 samplePos  = posSample.xyz;
            float3 sampleN    = normalize(fluxSample.rgb * 2.0 - 1.0);
            float  flux       = fluxSample.a;
            float3 dir        = samplePos - worldPos;
            float  dist2      = max(dot(dir, dir), 0.01);
            float  cosDir     = max(dot(sampleN, -normalize(dir)), 0.0);
            rsmIndirect += flux * max(dot(N, normalize(dir)), 0.0) * cosDir / dist2;
        }
    }
    return rsmIndirect * 0.03;   // 25 点离散化能量归一（与改前同一个常数）
}

/// 漫反射源分派：返回"已乘接收面 albedo 的间接出射辐射度"（与 Deferred 侧同约定）
float3 FwdDiffuseSource(uint id, float3 N, float3 albedo, float3 kD, float3 worldPos,
                        float iblIntensity) {
    if (id == GISOURCE_IBL) {
        return kD * u_IrradianceMap.Sample(u_IBLSampler, N).rgb * albedo * iblIntensity;
    }
    if (id == GISOURCE_RSM) {
        // 量纲：FwdSampleRSMIndirect 返回 E（经验常数已含 1/π）→ 补接收面 albedo，
        // 与 Deferred 的 `SampleRSMIndirect(...) * albedo` 完全一致
        return FwdSampleRSMIndirect(worldPos, N) * albedo;
    }
    return float3(0.0);
}

/// 镜面源分派：返回"已含菲涅耳与环境 BRDF 的间接镜面辐射度"
float3 FwdSpecularSource(uint id, float3 F, float2 envBRDF, float3 prefilteredColor,
                         float iblIntensity) {
    if (id == GISOURCE_IBL) {
        return prefilteredColor * (F * envBRDF.r + envBRDF.g) * iblIntensity;
    }
    return float3(0.0);
}

float4 fragmentMain(FSInput input) : SV_Target0 {
    GPUObjectData obj = u_Objects[input.objectIndex];

    uint texBase = obj.materialID;

    // 材质参数：bindless（u_Materials[materialID>>2]）或内联（GPUObjectData 字段）二选一
    float4 baseColorF; float4 emissiveF;
    float  metallicF, roughnessF, aoF, alphaCutoffF, dielectricF0F;
    uint   matFlags;
    uint   texMask;                    // 纹理存在位掩码（bit=槽有真实纹理）
    float4 disneyA, disneyB; float disneyC;
    if (useBindlessMaterial != 0u) {
        GPUMaterialData m = u_Materials[0][obj.materialID >> 2];  // [0]=材质 buffer，[materialID>>2]=buffer 内元素
        baseColorF = m.baseColorFactor; emissiveF = m.emissiveFactor;
        metallicF = m.metallicFactor; roughnessF = m.roughnessFactor; aoF = m.aoFactor;
        alphaCutoffF = m.alphaCutoff; dielectricF0F = m.dielectricF0; matFlags = m.materialFlags;
        texMask  = m.textureMask;
        disneyA = m.disneyA; disneyB = m.disneyB; disneyC = m.disneyC;
    } else {
        baseColorF = obj.baseColorFactor; emissiveF = obj.emissiveFactor;
        metallicF = obj.metallicFactor; roughnessF = obj.roughnessFactor; aoF = obj.aoFactor;
        alphaCutoffF = obj.alphaCutoff; dielectricF0F = obj.dielectricF0; matFlags = obj.materialFlags;
        texMask  = obj.textureMask;
        disneyA = obj.disneyA; disneyB = obj.disneyB; disneyC = obj.disneyC;
    }

    // 各纹理槽：有真实纹理才采样（无纹理用中性默认，避免占位纹理污染）
    float4 baseColorSample = (texMask & kGPUMaterialTexMask_BaseColor) != 0u
        ? u_Textures[texBase + kGPUMaterialTexSlot_BaseColor].Sample(u_Samplers[texBase + kGPUMaterialTexSlot_BaseColor], input.uv)
        : float4(1.0);
    float3 albedo = baseColorF.rgb * baseColorSample.rgb;
    float alpha   = baseColorF.a * baseColorSample.a;
    if (alpha < alphaCutoffF) discard;

    // 法线：按纹理存在掩码判断——无法线贴图（占位白会错误偏转 N，
    // 导致 +Y 表面被下方光源照亮的穿透问题）时直接用几何法线
    float3 N = normalize(input.worldNormal);
    if ((texMask & kGPUMaterialTexMask_Normal) != 0u) {
        float3 texNormal = u_Textures[texBase + kGPUMaterialTexSlot_Normal]
            .Sample(u_Samplers[texBase + kGPUMaterialTexSlot_Normal], input.uv).rgb * 2.0 - 1.0;
        N = normalize(N + texNormal * 0.5);
    }
    float3 V = normalize(cameraPosition.xyz - input.worldPos);

    // 金属度/粗糙度/环境光遮蔽：无纹理时用因子本身
    float4 mrSample = (texMask & kGPUMaterialTexMask_MetallicRough) != 0u
        ? u_Textures[texBase + kGPUMaterialTexSlot_MetallicRough].Sample(u_Samplers[texBase + kGPUMaterialTexSlot_MetallicRough], input.uv)
        : float4(0.0, 1.0, 0.0, 1.0);   // 中性：g=1(rough 不动) b=0(metallic 不动)
    float metallic  = metallicF  * mrSample.b;
    float roughness = clamp(roughnessF * mrSample.g, 0.04, 1.0);
    float ao = aoF * (((texMask & kGPUMaterialTexMask_Occlusion) != 0u)
        ? u_Textures[texBase + kGPUMaterialTexSlot_Occlusion].Sample(u_Samplers[texBase + kGPUMaterialTexSlot_Occlusion], input.uv).r
        : 1.0);

    // BRDF LUT：用于直接光照的多重散射能量补偿，IBL 镜面项复用同一采样
    float2 envBRDF = u_BRDF_LUT.Sample(u_IBLSampler, float2(max(dot(N, V), 0.0), roughness)).rg;

    // ---- 光源循环：Forward+ 或传统 Forward ----
    float3 color = albedo * 0.08 * ao;  // 低环境光
    if (useClustered != 0u && clusterTilesX > 0u) {
        // --- Forward+ 路径：遍历当前 Cluster 内的光源 ---
        uint tileX = uint(input.position.x) / kGPUClusterTileSize;
        uint tileY = uint(input.position.y) / kGPUClusterTileSize;
        // 线性深度重建（Vulkan NDC: z ∈ [0,1]）
        float linearDepth = clusterNear * clusterFar / (clusterFar - input.position.z * (clusterFar - clusterNear));
        float sliceF = float(kGPUClusterSlices) * log(linearDepth / clusterNear) / clusterLogFactor;
        uint  slice  = uint(clamp(sliceF, 0.0f, float(kGPUClusterSlices - 1)));
        uint clusterIdx = (slice * clusterTilesY + tileY) * clusterTilesX + tileX;

        LightGridCell cell = u_LightGrid[clusterIdx];
        uint endIdx = cell.offset + min(cell.count, 64u);
        for (uint li = cell.offset; li < endIdx; li++) {
            uint i = u_LightIndexList[li];
            GPULight light = u_Lights[i];
            color += ShadeWithLight(light, input.worldPos, N, V, albedo, metallic, roughness,
                                    dielectricF0F, envBRDF, disneyA, disneyB, disneyC);
        }
    } else {
        // --- 传统路径：遍历所有光源 ---
        uint count = min(lightCount, 8u);
        for (uint i = 0; i < count; i++) {
            GPULight light = u_Lights[i];
            color += ShadeWithLight(light, input.worldPos, N, V, albedo, metallic, roughness,
                                    dielectricF0F, envBRDF, disneyA, disneyB, disneyC);
        }
    }
    color *= ao;

    // ---- 间接光：**层栈驱动的归一化合成**（任务 26 / §9.2-H 的另一半）----
    //
    // 改前这里是把 IBL 漫反射、IBL 镜面、RSM 三项**硬编码相加**，而 RSM 还被
    // `iblIntensity > 0` 门控（"把 IBL 调暗会连带关掉 RSM" —— 正是 §9.2-A 修过的那个耦合在
    // Forward 侧的残留）。现在与 Deferred 走同一套语义：逐通道遍历层栈的源数组，
    // 算 `Σ(贡献 × 权重) / Σ权重`。
    //
    // 【为什么单源读数与改前一致】逐源表达式一字未动（IBL 漫反射/镜面仍是原来那两行），
    // 归一化在只有一个源时精确抵消 ⇒ {IBL} 的读数与改前逐位相同。
    // 【为什么多开一个源不会变亮】加权平均 ≤ 两者之和，且权重和归一到 1。
    // 【Forward 支持哪些源】只有世界空间源能用（前向着色没有 GBuffer，也没有屏幕空间缓冲）：
    // 漫反射 IBL/RSM、镜面 IBL。能力位由 PipelineCaps::Forward 声明，面板与 Degrade 都读它。
    float3 F0_ibl = lerp(float3(dielectricF0F), albedo, metallic);
    float3 F = F_SchlickRoughness(max(dot(N, V), 0.0), F0_ibl, roughness, metallic);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 R = reflect(-V, N);
    float  mipLevel = roughness * float(kGPUPrefilterMips - 1);
    float3 prefilteredColor = u_PrefilterMap.SampleLevel(u_IBLSampler, R, mipLevel).rgb;
    float  camDist = length(cameraPosition.xyz - input.worldPos);

    // 通道合成：与 Deferred 完全同构（含"通道里没有源时返回 0"这条语义）
    {
        GIChannelBlendParams bl = u_GIBlendParams.diffuse;
        float3 num = float3(0.0);
        float  den = 0.0;
        for (uint i = 0u; i < bl.count; ++i) {
            GISourceSlot s = bl.sources[i];
            if (s.weight <= 0.0) continue;
            // 前向没有屏幕空间源 ⇒ 置信度掩码全是 None ⇒ 权重就是用户权重（与 Deferred 一致：
            // 世界空间源不带 CAMERA_COVERAGE/PROBE_GRID 位）。falloff 仍按 UBO 生效。
            float w = s.weight;
            if (s.falloffDistance > 0.0) w *= saturate(1.0 - camDist / s.falloffDistance);
            num += FwdDiffuseSource(s.id, N, albedo, kD, input.worldPos, iblIntensity) * w;
            den += w;
        }
        color += (bl.mode != 0u) ? ((den > 0.0) ? (num / den) : float3(0.0)) : num;
    }
    {
        GIChannelBlendParams bl = u_GIBlendParams.specular;
        float3 num = float3(0.0);
        float  den = 0.0;
        for (uint i = 0u; i < bl.count; ++i) {
            GISourceSlot s = bl.sources[i];
            if (s.weight <= 0.0) continue;
            float w = s.weight;
            if (s.falloffDistance > 0.0) w *= saturate(1.0 - camDist / s.falloffDistance);
            num += FwdSpecularSource(s.id, F, envBRDF, prefilteredColor, iblIntensity) * w;
            den += w;
        }
        color += (bl.mode != 0u) ? ((den > 0.0) ? (num / den) : float3(0.0)) : num;
    }

    color += emissiveF.rgb;

    // 空中透视：对场景几何应用距离/高度相关大气消光 + 入射散射
    ApplyAerialPerspective(color, cameraPosition.xyz, input.worldPos, atmosphere.xyz, atmosphere.w);
    return float4(color, alpha);
}
