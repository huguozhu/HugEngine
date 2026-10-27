// ============================================================
// RSM_Generate.vert.slang — RSM 生成顶点着色器
//
// 从 lightViewProj 渲染几何体，输出世界空间位置和法线。
// 与 Shadow.vert 共用同一顶点格式（StaticVertex: pos+normal+uv）。
#include "MeshVS.slang"   // GBufferVSOutput / PBRVSOutput / RSMVSOutput（接口唯一出处）
#include "ShaderTypes.slang"  // GPUObjectData / GPULight 共享定义
// ============================================================

// explicit semantics (D3D12) + vk::location (Vulkan)
struct RSMVertexInput {
    [[vk::location(0)]] float3 inPosition : POSITION0;
    [[vk::location(1)]] float3 inNormal   : NORMAL0;
};

// GPU Object（128 bytes，匹配 CPU GPUObjectData）
// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_Objects : register(t2, space0);

[[vk::push_constant]] cbuffer RSMConstants {
    float4x4 lightViewProj;
    uint     objectIndex;
    uint     lightIndex;   // Fragment 侧用（光源颜色/强度），VS 不读但必须保持布局一致
    uint     _pad[2];
    float4   vplAlbedo;    // Fragment 侧用（VPL 漫反射率），同样只为布局一致
};

RSMVSOutput vertexMain(RSMVertexInput vin) {
    const float3 inPosition = vin.inPosition;
    const float3 inNormal   = vin.inNormal;
    GPUObjectData obj = u_Objects[objectIndex];
    float4 worldPos4 = mul(obj.worldMatrix, float4(inPosition, 1.0));

    RSMVSOutput output;
    output.position    = mul(lightViewProj, worldPos4);
    output.worldPos    = worldPos4.xyz;
    output.worldNormal = normalize(mul((float3x3)obj.worldMatrix, inNormal));
    return output;
}
