// ============================================================
// PBR.vert.slang — PBR 顶点着色器 (GPU-driven)
// per-object 数据从 Storage Buffer 读取
// ============================================================

#include "ShaderTypes.slang"  // GPUObjectData 共享定义
layout(location = 0) in float3 inPosition;
layout(location = 1) in float3 inNormal;
layout(location = 2) in float2 inUV;

// GPU Object（128 bytes，匹配 CPU GPUObjectData）
// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// set=0, binding=2: GPU 对象数据数组
[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;

// 帧级 Push Constants
[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;
    float4   cameraPosition;
    uint     lightCount;
    uint     objectIndex;     // 传统模式：push constant 传入
    uint     useInstanceID;   // 1=从 SV_InstanceID 读 objectIndex
    float    iblIntensity;    // IBL 强度（VS 不使用，仅占位对齐 FS）
    uint     _pad;
};

struct VSOutput {
    float4 position      : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] nointerpolation uint objectIndex;  // 传递到 FS
};

VSOutput vertexMain(uint instanceID : SV_InstanceID) {
    VSOutput output;
    uint objIdx = (useInstanceID != 0u) ? instanceID : objectIndex;
    GPUObjectData obj = u_Objects[objIdx];

    float4 worldPos = mul(obj.worldMatrix, float4(inPosition, 1.0));
    output.worldPos = worldPos.xyz;
    output.worldNormal = normalize(mul((float3x3)obj.worldMatrix, inNormal));
    output.uv = inUV;
    output.objectIndex = objIdx;  // 传递 objectIndex 到 FS
    output.position = mul(viewProjMatrix, worldPos);
    return output;
}
