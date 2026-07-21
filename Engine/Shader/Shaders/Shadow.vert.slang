// ============================================================
// Shadow.vert.slang — 阴影贴图顶点着色器
//
// 仅将顶点变换到光照裁剪空间，用于深度写入。
// 与主 PBR 管线共用相同的顶点缓冲格式。
// ============================================================

#include "ShaderTypes.slang"  // GPUObjectData / ShadowPushConstant 共享定义

layout(location = 0) in float3 inPosition;
// 注：阴影通道仅需位置，normal(loc=1) 和 uv(loc=2) 不声明，避免 Vulkan 验证警告
// 顶点缓冲 stride 仍为 sizeof(StaticVertex)=32，位置数据在偏移 0 处

// GPU Object（128 bytes，匹配 CPU GPUObjectData）
// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// set=0, binding=2: GPU 对象数据（与主管线共用）
[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_Objects;

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
