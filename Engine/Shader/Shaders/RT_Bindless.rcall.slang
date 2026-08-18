// RT_Bindless.rcall.slang — Callable: bindless PBR 纹理采样

#include "common.slang"

struct BindlessCallData {
    uint  materialID;
    float2 uv;
    float3 albedo;
    float3 normal;
    float  metallic;
    float  roughness;
    float  ao;
};

[shader("callable")]
void main(inout BindlessCallData data) {
    uint texBase = data.materialID;
    float2 uv = data.uv;

    data.albedo    = u_Textures[texBase + kGPUMaterialTexSlot_BaseColor].SampleLevel(u_Samplers[texBase + kGPUMaterialTexSlot_BaseColor], uv, 0.0).rgb;
    float3 tn      = u_Textures[texBase + kGPUMaterialTexSlot_Normal].SampleLevel(u_Samplers[texBase + kGPUMaterialTexSlot_Normal], uv, 0.0).rgb * 2.0 - 1.0;
    data.normal    = tn;
    float4 mr      = u_Textures[texBase + kGPUMaterialTexSlot_MetallicRough].SampleLevel(u_Samplers[texBase + kGPUMaterialTexSlot_MetallicRough], uv, 0.0);
    data.metallic  = mr.b;
    data.roughness = mr.g;
    data.ao        = u_Textures[texBase + kGPUMaterialTexSlot_Occlusion].SampleLevel(u_Samplers[texBase + kGPUMaterialTexSlot_Occlusion], uv, 0.0).r;
}
