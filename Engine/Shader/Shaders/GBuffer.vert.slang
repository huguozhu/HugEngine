// GBuffer.vert — 延迟渲染 GBuffer 顶点着色器（4 MRT + velocity）
// 双 MVP 投影：current + previous，支持 TAA velocity 计算
// 抖动：viewProjMatrix 已在 CPU 端加 jitter，prevViewProjMatrix 不抖动
#include "common.slang"

struct VSInput {
    [[vk::location(0)]] float3 positionOS;
    [[vk::location(1)]] float3 normalOS;
    [[vk::location(2)]] float2 uv;
};

struct VSOutput {
    float4 position              : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] float4 prevClipPos;   // 上一帧裁剪空间位置（无抖动）→ velocity 计算
};

// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_Objects;

// offset 0:  viewProjMatrix     (64B) — 当前帧 VP（含抖动）
// offset 64: prevViewProjMatrix (64B) — 上一帧 VP（不含抖动）
// offset 128:objectIndex / useInstanceID
[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;       // offset 0,  64B
    float4x4 prevViewProjMatrix;   // offset 64, 64B
    uint     objectIndex;          // offset 128, 4B
    uint     useInstanceID;        // offset 132, 4B  [GPU Driven 模式：1=从 SV_InstanceID 读取]
    uint     _pad[14];             // 填充到 192B
};

VSOutput vertexMain(VSInput input, uint instanceID : SV_InstanceID) {
    uint objIdx = (useInstanceID != 0u) ? instanceID : objectIndex;
    GPUObjectData obj = u_Objects[objIdx];
    float4 worldPos = mul(obj.worldMatrix, float4(input.positionOS, 1.0));

    VSOutput output;
    output.position    = mul(viewProjMatrix, worldPos);      // 含抖动
    output.prevClipPos = mul(prevViewProjMatrix, worldPos);  // 不含抖动，用于速度计算
    output.worldPos    = worldPos.xyz;
    output.worldNormal = normalize(mul((float3x3)obj.worldMatrix, input.normalOS));
    output.uv          = input.uv;
    return output;
}
