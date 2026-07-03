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
    [[vk::location(3)]] float4 prevClipPos;   // 上一帧裁剪空间位置（无抖动）
};

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

// Push constant：双矩阵 + objectIndex
// 注意：viewProjMatrix 已含抖动偏移（CPU 端合成 jitteredProj * view）
[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;       // offset 0,  64B — 当前帧 VP（含抖动）
    float4x4 prevViewProjMatrix;   // offset 64, 64B — 上一帧 VP（不含抖动）
    uint     objectIndex;          // offset 128, 4B
    uint     _pad[15];             // 填充到 192B（push constant range 声明为 256B）
};

VSOutput vertexMain(VSInput input) {
    GPUObjectData obj = u_Objects[objectIndex];
    float4 worldPos = mul(obj.worldMatrix, float4(input.positionOS, 1.0));

    VSOutput output;
    output.position    = mul(viewProjMatrix, worldPos);      // 含抖动
    output.prevClipPos = mul(prevViewProjMatrix, worldPos);  // 不含抖动，用于速度计算
    output.worldPos    = worldPos.xyz;
    output.worldNormal = normalize(mul((float3x3)obj.worldMatrix, input.normalOS));
    output.uv          = input.uv;
    return output;
}
