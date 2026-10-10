#include "pbr_common.slang"   // G_Smith_IBL（IBL 端 k=alpha/2 的 Smith；直接光用 G_Smith_Direct）
#include "Sampling.slang"   // Hammersley / RadicalInverse_VdC（唯一出处）
// ============================================================
// IBL_BRDF_LUT.frag.slang — Split-Sum BRDF 积分查找表
//
// 基于 UE4 方法：预计算 BRDF 的镜面反射部分，
// 2D LUT：横轴=NdotV，纵轴=roughness
// 输出 R=scale(DFG1), G=bias(DFG2), B=0, A=1
// ============================================================

#include "FullscreenVS.slang"   // VSOutput / VSOutputWorldDir（接口唯一出处）

static const uint SAMPLE_COUNT = 256;

float2 IntegrateBRDF(float NdotV, float roughness) {
    float3 V;
    V.x = sqrt(1.0 - NdotV * NdotV);
    V.y = 0.0;
    V.z = NdotV;

    float  A = 0.0;
    float  B = 0.0;
    float3 N = float3(0.0, 0.0, 1.0);

    for (uint i = 0; i < SAMPLE_COUNT; i++) {
        float2 Xi   = Hammersley(i, SAMPLE_COUNT);
        // GGX 半向量：共享采样器（原本地 ImportanceSampleGGX）；IBL 端 alpha 无下限 ⇒ minAlpha = 0
        float NdotH_raw;
        float3 H    = SampleGgxHalf(N, roughness, Xi, 0.0, NdotH_raw);
        float3 L    = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(NdotH_raw, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0) {
            float G = G_Smith_IBL(NdotV, NdotL, roughness);
            float G_Vis = (G * VdotH) / max(NdotH * NdotV, 0.001);
            float Fc = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    return float2(A, B) / float(SAMPLE_COUNT);
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 brdf = IntegrateBRDF(input.uv.x, input.uv.y);
    return float4(brdf, 0.0, 1.0);
}
