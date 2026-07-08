// ============================================================
// RSM_Generate.vert.slang — RSM 生成顶点着色器
//
// 从 lightViewProj 渲染几何体，输出世界空间位置和法线。
// 与 Shadow.vert 共用同一顶点格式（StaticVertex: pos+normal+uv）。
#include "ShaderTypes.slang"  // GPUObjectData / GPULight 共享定义
// ============================================================

layout(location = 0) in float3 inPosition;
layout(location = 1) in float3 inNormal;

// GPU Object（128 bytes，匹配 CPU GPUObjectData）
// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;

[[vk::push_constant]] cbuffer RSMConstants {
    float4x4 lightViewProj;
    uint     objectIndex;
    uint     _pad[3];
};

struct VSOutput {
    float4 position              : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
};

VSOutput vertexMain() {
    GPUObjectData obj = u_Objects[objectIndex];
    float4 worldPos4 = mul(obj.worldMatrix, float4(inPosition, 1.0));

    VSOutput output;
    output.position    = mul(lightViewProj, worldPos4);
    output.worldPos    = worldPos4.xyz;
    output.worldNormal = normalize(mul((float3x3)obj.worldMatrix, inNormal));
    return output;
}
