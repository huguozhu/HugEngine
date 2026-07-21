// ============================================================
// PBR.frag.slang — PBR 片元着色器 (GPU-driven)
// 支持 Forward+（Cluster 光源剔除）和传统 Forward 两种模式
// 通过 push constant useClustered 运行时切换
// ============================================================

#include "pbr_common.slang"

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

static const float kPrefilterMips = 5.0;  // 预滤波 mip 数（匹配 GI_IBL::kPrefilterMips）
static const uint  kClusterSlices = 12;   // 与 C++ ClusteredShading::kDepthSlices 一致

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
};

float3 ACESFilm(float3 x) {
    float a = 2.51; float b = 0.03; float c = 2.43; float d = 0.59; float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// ---- 光源遍历：传统 Forward 与 Forward+ 共用 PBR 计算 ----
float3 ShadeWithLight(GPULight light, float3 worldPos, float3 N, float3 V,
                       float3 albedo, float metallic, float roughness) {
    uint ltype = uint(light.directionType.w);
    float3 L; float attenuation = 1.0;
    if (ltype == 0u) {
        L = normalize(-light.directionType.xyz);
    } else {
        float3 toLight = light.positionRange.xyz - worldPos;
        float dist = length(toLight);
        L = toLight / max(dist, 0.001);
        float d = dist / max(light.positionRange.w, 0.001);
        attenuation = 1.0 / (1.0 + 2.0 * d + d * d);
        if (ltype == 2u) {
            float spotDot = dot(-L, normalize(light.directionType.xyz));
            attenuation *= smoothstep(light.coneAngles.y, light.coneAngles.x, spotDot);
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
        } else {
            float3 lightPos = sm.pointLightData.xyz;
            float  range    = sm.pointLightData.w;
            shadow = lerp(1.0, SamplePointShadow(worldPos, lightPos, range,
                          sm, u_PointShadowCube, u_PointShadowSampler),
                          sm.shadowParams.z);
        }
    }

    return PBR_BRDF(albedo, metallic, roughness, N, V, L) * radiance * shadow;
}

float4 fragmentMain(FSInput input) : SV_Target0 {
    GPUObjectData obj = u_Objects[input.objectIndex];

    uint texBase = obj.materialID;
    float4 baseColorSample = u_Textures[texBase + 0].Sample(u_Samplers[0], input.uv);
    float3 albedo = obj.baseColorFactor.rgb * baseColorSample.rgb;
    float alpha   = obj.baseColorFactor.a * baseColorSample.a;
    if (alpha < obj.alphaCutoff) discard;

    // 法线贴图
    float3 texNormal = u_Textures[texBase + 1].Sample(u_Samplers[0], input.uv).rgb * 2.0 - 1.0;
    float3 N = normalize(input.worldNormal + texNormal * 0.5);
    float3 V = normalize(cameraPosition.xyz - input.worldPos);

    // 金属度/粗糙度贴图
    float4 mrSample = u_Textures[texBase + 2].Sample(u_Samplers[0], input.uv);
    float metallic  = obj.metallicFactor  * mrSample.b;
    float roughness = clamp(obj.roughnessFactor * mrSample.g, 0.04, 1.0);
    float ao = obj.aoFactor * u_Textures[texBase + 3].Sample(u_Samplers[0], input.uv).r;

    // ---- 光源循环：Forward+ 或传统 Forward ----
    float3 color = albedo * 0.08 * ao;  // 低环境光
    if (useClustered != 0u && clusterTilesX > 0u) {
        // --- Forward+ 路径：遍历当前 Cluster 内的光源 ---
        uint tileX = uint(input.position.x) / 64;  // kTileSize = 64
        uint tileY = uint(input.position.y) / 64;
        // 线性深度重建（Vulkan NDC: z ∈ [0,1]）
        float linearDepth = clusterNear * clusterFar / (clusterFar - input.position.z * (clusterFar - clusterNear));
        float sliceF = float(kClusterSlices) * log(linearDepth / clusterNear) / clusterLogFactor;
        uint  slice  = uint(clamp(sliceF, 0.0f, float(kClusterSlices - 1)));
        uint clusterIdx = (slice * clusterTilesY + tileY) * clusterTilesX + tileX;

        LightGridCell cell = u_LightGrid[clusterIdx];
        uint endIdx = cell.offset + min(cell.count, 64u);
        for (uint li = cell.offset; li < endIdx; li++) {
            uint i = u_LightIndexList[li];
            GPULight light = u_Lights[i];
            color += ShadeWithLight(light, input.worldPos, N, V, albedo, metallic, roughness);
        }
    } else {
        // --- 传统路径：遍历所有光源 ---
        uint count = min(lightCount, 8u);
        for (uint i = 0; i < count; i++) {
            GPULight light = u_Lights[i];
            color += ShadeWithLight(light, input.worldPos, N, V, albedo, metallic, roughness);
        }
    }
    color *= ao;

    // ---- IBL（环境光照） ----
    float3 F0_ibl = lerp(float3(0.04), albedo, metallic);
    float3 F = F_SchlickRoughness(max(dot(N, V), 0.0), F0_ibl, roughness, metallic);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 irradiance = u_IrradianceMap.Sample(u_IBLSampler, N).rgb;
    color += kD * irradiance * albedo * iblIntensity;

    // 镜面反射 IBL：预滤波环境图（roughness→mip）+ BRDF LUT
    float3 R = reflect(-V, N);
    float mipLevel = roughness * float(kPrefilterMips - 1);
    float3 prefilteredColor = u_PrefilterMap.SampleLevel(u_IBLSampler, R, mipLevel).rgb;
    float2 envBRDF = u_BRDF_LUT.Sample(u_IBLSampler, float2(max(dot(N, V), 0.0), roughness)).rg;
    color += prefilteredColor * (F * envBRDF.r + envBRDF.g) * iblIntensity;

    // ---- RSM 间接光照（Reflective Shadow Maps） ----
    if (iblIntensity > 0.0 && u_ShadowData[0].shadowParams.z > 0.0) {
        float4 lightSpacePos = mul(u_ShadowData[0].lightViewProj[0], float4(input.worldPos, 1.0));
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
                float3 dir        = samplePos - input.worldPos;
                float  dist2      = max(dot(dir, dir), 0.01);
                float  cosDir     = max(dot(sampleN, -normalize(dir)), 0.0);
                rsmIndirect += flux * max(dot(N, normalize(dir)), 0.0) * cosDir / dist2;
            }
        }
        color += rsmIndirect * 0.03 * iblIntensity;
    }

    color += obj.emissiveFactor.rgb;

    return float4(color, alpha);
}
