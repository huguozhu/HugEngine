// GBuffer.vert — Deferred 渲染 GBuffer 顶点着色器
// 输出 worldPos + worldNormal + uv，供 GBuffer.frag 使用
// 与 PBR.vert 逻辑一致，但不输出 SV_Position (next stage depth)

layout(location = 0) in float3 inPosition;
layout(location = 1) in float3 inNormal;
layout(location = 2) in float2 inUV;

struct GPUObjectData {
    float4x4 worldMatrix;
    float4   baseColorFactor;
    float4   emissiveFactor;
    float    metallicFactor;
    float    roughnessFactor;
    float    aoFactor;
    float    alphaCutoff;
    uint     materialFlags;
    uint     materialID;
    uint     _pad[2];
};

[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;

[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;
    uint     objectIndex;
    uint     _pad[12];
};

struct VSOutput {
    float4 position              : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
};

VSOutput vertexMain() {
    GPUObjectData obj = u_Objects[objectIndex];
    float4 worldPos4 = mul(obj.worldMatrix, float4(inPosition, 1.0));

    VSOutput output;
    output.position    = mul(viewProjMatrix, worldPos4);
    output.worldPos    = worldPos4.xyz;
    output.worldNormal = normalize(mul((float3x3)obj.worldMatrix, inNormal));
    output.uv          = inUV;
    return output;
}
