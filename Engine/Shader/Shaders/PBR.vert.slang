// ============================================================
// PBR.vert.slang — PBR 渲染顶点着色器
//
// 输入:  StaticVertex (position + normal + uv)
// Push:  modelMatrix, viewProjMatrix
// ============================================================

// --- 顶点输入（匹配 StaticVertex 布局）---
layout(location = 0) in float3 inPosition;
layout(location = 1) in float3 inNormal;
layout(location = 2) in float2 inUV;

// --- Push constants（与 CPU 端 PushConstantData 对齐）---
[[vk::push_constant]] cbuffer PushConstants {
    float4x4 modelMatrix;       // [0..64]
    float4x4 viewProjMatrix;    // [64..128]
    float4   baseColorFactor;   // [128..144]
    float    metallicFactor;    // [144]
    float    roughnessFactor;   // [148]
    float    aoFactor;          // [152]
    float    alphaCutoff;       // [156]
    float4   emissiveFactor;    // [160..176]
    float4   cameraPosition;    // [176..192]
    float4   lightDirection;    // [192..208]
    float4   lightColor;        // [208..224]
    uint     materialFlags;     // [224]
    uint     _pad[3];           // [228..240]
};

// --- 顶点输出结构体 ---
struct VSOutput {
    float4 position      : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
};

VSOutput vertexMain() {
    VSOutput output;

    // 世界空间位置
    float4 worldPos = mul(modelMatrix, float4(inPosition, 1.0));
    output.worldPos = worldPos.xyz;

    // 世界空间法线（假设 uniform scale）
    output.worldNormal = normalize(mul((float3x3)modelMatrix, inNormal));

    // UV 直传
    output.uv = inUV;

    // 裁剪空间位置
    output.position = mul(viewProjMatrix, worldPos);
    return output;
}
