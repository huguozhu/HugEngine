// GBuffer.vert — 延迟渲染 GBuffer 顶点着色器（4 MRT + velocity）
// 双 MVP 投影：current + previous，支持 TAA velocity 计算
// 【抖动（2026-09 画质阶段 0 第②项接线）】两帧的 VP **都含各自的 TAA 抖动**（CPU 端由
//   `CameraData::jitterNdc` 施加）。这样速度矢量 = (本帧含抖动 NDC) − (上帧含抖动 NDC)，
//   TAA resolve 用 `historyUV = uv − velocity` 正好落到"同一世界点在上帧被采样到的那个
//   亚像素位置"，无需在 resolve 里再补一个 jitter 项（见 GBuffer.frag.slang 的说明）。
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
    [[vk::location(3)]] float4 prevClipPos;   // 上一帧裁剪空间位置（含上帧抖动）→ velocity 计算
};

// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_Objects;
// bindless SSBO 数组：实例化网格的实例变换 + 逐实例剔除的可见索引列表（任务 25）
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<float4x4> u_InstanceTransforms[];
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<uint>     u_VisibleInstanceIndices[];

// offset 0:  viewProjMatrix     (64B) — 当前帧 VP（含当前帧抖动）
// offset 64: prevViewProjMatrix (64B) — 上一帧 VP（含**上帧**抖动，velocity 因此自带抖动差）
// offset 128:objectIndex / useInstanceID / 实例句柄
[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;         // offset 0,  64B
    float4x4 prevViewProjMatrix;     // offset 64, 64B
    uint     objectIndex;            // offset 128, 4B
    uint     useInstanceID;          // offset 132, 4B  [1=GPU Driven；2=实例化网格（任务 25）]
    uint     instanceSSBOHandle;     // offset 136, 4B  实例变换 SSBO 句柄
    uint     instanceVisibleHandle;  // offset 140, 4B  可见实例索引列表句柄（0=未剔除）
    uint     _pad[12];               // 填充到 192B
};

VSOutput vertexMain(VSInput input, uint instanceID : SV_InstanceID) {
    // 对象/材质条目：GPU Driven 模式（1）用 SV_InstanceID 当对象下标；
    // 实例化网格（2）是"一个对象条目 + N 个实例"——对象下标仍取 push constant 的 objectIndex，
    // 只有世界矩阵来自实例 SSBO（这一点与 PBR.vert 的语义必须一致，否则会越界读到别的物体）。
    uint objIdx = (useInstanceID == 1u) ? instanceID : objectIndex;
    GPUObjectData obj = u_Objects[objIdx];
    float4x4 worldMatrix = obj.worldMatrix;
    if (useInstanceID == 2u) {
        // 任务 25：可见列表非空时先查压缩后的实例下标（逐实例剔除的结果）
        uint instIdx = instanceID;
        if (instanceVisibleHandle != 0u)
            instIdx = u_VisibleInstanceIndices[instanceVisibleHandle][instanceID];
        worldMatrix = u_InstanceTransforms[instanceSSBOHandle][instIdx];
    }
    float4 worldPos = mul(worldMatrix, float4(input.positionOS, 1.0));

    VSOutput output;
    output.position    = mul(viewProjMatrix, worldPos);      // 含当前帧抖动
    output.prevClipPos = mul(prevViewProjMatrix, worldPos);  // 含上帧抖动；两者相减即"含抖动的真实位移"
    output.worldPos    = worldPos.xyz;
    output.worldNormal = normalize(mul((float3x3)worldMatrix, input.normalOS));
    output.uv          = input.uv;
    return output;
}
