// ============================================================
// RSM_Generate.frag.slang — RSM 数据生成（双 MRT）
//
// MRT 0: RGBA16_FLOAT — worldPos.xyz + 未使用
// MRT 1: RGBA16_FLOAT — worldNormal.xyz + flux
// 通量 = albedo * lightColor * max(NdotL, 0)（Lambertian 一次反射）
// ============================================================

struct VSOutput {
    float4 position              : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
};

struct FSOutput {
    [[vk::location(0)]] float4 posAndUnused : SV_Target0;
    [[vk::location(1)]] float4 normalAndFlux : SV_Target1;
};

// 灯光数据（与 PBR 共用 set=0 binding=1）
struct GPULight {
    float4 colorIntensity;
    float4 directionType;
    float4 positionRange;
    float2 coneAngles;
    int    shadowIndex;
    int    _padLight;
};

[[vk::binding(1, 0)]] StructuredBuffer<GPULight> u_Lights;

[[vk::push_constant]] cbuffer RSMConstants {
    float4x4 lightViewProj;
    uint     objectIndex;
    uint     lightIndex;   // RSM 对应的光源在 u_Lights 中的索引
    uint     _pad[2];
};

FSOutput fragmentMain(VSOutput input) {
    FSOutput output;

    // MRT 0: 世界空间位置
    output.posAndUnused = float4(input.worldPos, 1.0);

    // MRT 1: 世界空间法线 + 通量
    float3 N = normalize(input.worldNormal);
    GPULight light = u_Lights[lightIndex];
    float3 L = normalize(-light.directionType.xyz);  // 方向光
    float NdotL = max(dot(N, L), 0.0);
    float flux = light.colorIntensity.w * NdotL;     // intensity * cos(theta)

    output.normalAndFlux = float4(N * 0.5 + 0.5, flux);  // 法线编码 [0,1]

    return output;
}
