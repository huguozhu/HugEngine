// RT_Bindless.rcall.slang — Callable Shader: bindless PBR 纹理采样
// 由 ClosestHit 通过 CallShader 调用，绕过 slangc ClosestHitKHR 动态数组索引 bug
// CallableKHR 执行模型不受此 bug 影响

#include "common.slang"  // u_Textures[], u_Samplers[]

struct BindlessCallData {
    uint  materialID;    // [in]  bindless 纹理基索引
    float2 uv;           // [in]  纹理坐标（triplanar）
    float3 albedo;       // [out] baseColor × texture
    float3 normal;       // [out] 切线空间法线 (RGB)
    float  metallic;     // [out]
    float  roughness;    // [out]
    float  ao;           // [out]
};

[shader("callable")]
void main(inout BindlessCallData data) {
    uint texBase = data.materialID;
    float2 uv = data.uv;

    // SampleLevel: CallableKHR 需要显式 LOD
    data.albedo    = u_Textures[texBase + 0].SampleLevel(u_Samplers[0], uv, 0.0).rgb;
    float3 tn      = u_Textures[texBase + 1].SampleLevel(u_Samplers[0], uv, 0.0).rgb * 2.0 - 1.0;
    data.normal    = tn;
    float4 mr      = u_Textures[texBase + 2].SampleLevel(u_Samplers[0], uv, 0.0);
    data.metallic  = mr.b;
    data.roughness = mr.g;
    data.ao        = u_Textures[texBase + 3].SampleLevel(u_Samplers[0], uv, 0.0).r;
}
