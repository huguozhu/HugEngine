// ============================================================
// Shadow.vert.slang — 阴影贴图顶点着色器
//
// 仅将顶点变换到光照裁剪空间，用于深度写入。
// 与主 PBR 管线共用相同的顶点缓冲格式。
// ============================================================

layout(location = 0) in float3 inPosition;
layout(location = 1) in float3 inNormal;
layout(location = 2) in float2 inUV;

// GPU Object（128 bytes，匹配 CPU GPUObjectData）
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

// set=0, binding=2: GPU 对象数据（与主管线共用）
[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;

// Push Constants（阴影通道专用：lightViewProj + objectIndex）
[[vk::push_constant]] cbuffer ShadowConstants {
    float4x4 lightViewProj;
    uint     objectIndex;
    uint     _pad[3];
};

struct VSOutput {
    float4 position : SV_Position;
};

VSOutput vertexMain() {
    VSOutput output;
    GPUObjectData obj = u_Objects[objectIndex];
    float4 worldPos = mul(obj.worldMatrix, float4(inPosition, 1.0));
    output.position = mul(lightViewProj, worldPos);
    return output;
}
