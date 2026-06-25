// ============================================================
// PBR.vert.slang — PBR 渲染顶点着色器
// ============================================================

layout(location = 0) in float3 inPosition;
layout(location = 1) in float3 inNormal;
layout(location = 2) in float2 inUV;

// Push constants（光照数据已移至 Storage Buffer）
[[vk::push_constant]] cbuffer PushConstants {
    float4x4 modelMatrix;           // [0..64]
    float4x4 viewProjMatrix;        // [64..128]
    float4   baseColorFactor;       // [128..144]
    float    metallicFactor;        // [144]
    float    roughnessFactor;       // [148]
    float    aoFactor;              // [152]
    float    alphaCutoff;           // [156]
    float4   emissiveFactor;        // [160..176]
    float4   cameraPosition;        // [176..192]
    uint     lightCount;            // [192]
    uint     materialFlags;         // [196]
    uint     materialID;            // [200] bindless index
    uint     _pad[13];              // [204..256]
};

struct VSOutput {
    float4 position      : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
};

VSOutput vertexMain() {
    VSOutput output;
    float4 worldPos = mul(modelMatrix, float4(inPosition, 1.0));
    output.worldPos = worldPos.xyz;
    output.worldNormal = normalize(mul((float3x3)modelMatrix, inNormal));
    output.uv = inUV;
    output.position = mul(viewProjMatrix, worldPos);
    return output;
}
