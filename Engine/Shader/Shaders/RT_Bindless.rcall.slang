// RT_Bindless.rcall.slang — Callable: bindless PBR 纹理采样
//
// ⚠️ TODO（结构性问题）：本 Callable 引用的 bindless 材质（u_Textures /
// u_Materials，set 0）在 RT 渲染器（HybridRT/PathTracing）中尚未上传绑定
// （RT 场景走 u_MatTex 简化材质）。当前直接采样会命中未绑定/占位纹理，
// 覆盖调用方（rchit）已填好的正确初值（白占位会把法线等污染为错误值）。
// 修复方向：RT 渲染器接入 bindless 材质上传（UploadMaterialBindless +
// u_Materials 绑定）后，按 textureMask 判断各槽有无真实纹理再采样
// （与 PBR.frag/GBuffer.frag 的 kGPUMaterialTexMask_* 一致）。

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
