// ============================================================
// PBR.vert.slang — PBR 顶点着色器 (GPU-driven)
// per-object 数据从 Storage Buffer 读取
// ============================================================

#include "ShaderTypes.slang"  // GPUObjectData 共享定义
layout(location = 0) in float3 inPosition;
layout(location = 1) in float3 inNormal;
layout(location = 2) in float2 inUV;
// 蒙皮顶点属性（location 3/4；静态布局缺失时为 0 → 模式 3 才启用）
layout(location = 3) in uint8_t4 inJoints;  // 关节索引 ×4
layout(location = 4) in float4 inWeights;   // 权重 ×4

// GPU Object（128 bytes，匹配 CPU GPUObjectData）
// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// set=0, binding=2: GPU 对象数据数组
[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_Objects;

// bindless SSBO 数组：实例化网格的实例变换（float4x4 元素缓冲，按句柄索引）
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<float4x4> u_InstanceTransforms[];
// bindless SSBO 数组：骨骼蒙皮矩阵（world × inverseBind，按句柄索引）
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<float4x4> u_BoneMatrices[];

// 帧级 Push Constants（Forward+ 扩展，与 FS 对齐）
[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;
    float4   cameraPosition;
    uint     lightCount;
    uint     objectIndex;     // 传统模式：push constant 传入
    uint     useInstanceID;   // 1=从 SV_InstanceID 读 objectIndex；2=实例化网格（实例变换 SSBO）
    float    iblIntensity;    // IBL 强度（VS 不使用，仅占位对齐 FS）
    uint     clusterTilesX;   // Forward+ 参数（VS 不使用，仅占位对齐）
    uint     clusterTilesY;
    float    clusterNear;
    float    clusterFar;
    float    clusterLogFactor;
    uint     useClustered;
    uint     useBindlessMaterial;  // VS 不使用，仅占位对齐 FS
    float4   atmosphere;           // VS 不使用，仅占位对齐 FS
    uint     instanceSSBOHandle;   // useInstanceID=2 时的实例变换 SSBO 句柄
    uint     instancePad[3];
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
    uint objIdx = objectIndex;
    GPUObjectData obj = u_Objects[objIdx];
    float4x4 worldMatrix = obj.worldMatrix;

    if (useInstanceID == 1u) {
        // GPU Driven：SV_InstanceID → objectIndex
        objIdx = instanceID;
        obj = u_Objects[objIdx];
        worldMatrix = obj.worldMatrix;
    } else if (useInstanceID == 2u) {
        // 实例化网格（B1）：实例变换 SSBO[句柄][instanceID]，材质取单个对象条目
        worldMatrix = u_InstanceTransforms[instanceSSBOHandle][instanceID];
    } else if (useInstanceID == 3u) {
        // 骨骼蒙皮（C1b）：4 骨骼矩阵按权重加权；
        // 再乘实体世界矩阵（glTF 骨骼定义在网格节点局部空间）
        float4x4 skinMat  = u_BoneMatrices[instanceSSBOHandle][inJoints.x] * inWeights.x;
        skinMat += u_BoneMatrices[instanceSSBOHandle][inJoints.y] * inWeights.y;
        skinMat += u_BoneMatrices[instanceSSBOHandle][inJoints.z] * inWeights.z;
        skinMat += u_BoneMatrices[instanceSSBOHandle][inJoints.w] * inWeights.w;
        worldMatrix = mul(obj.worldMatrix, skinMat);
    }

    float4 worldPos = mul(worldMatrix, float4(inPosition, 1.0));
    output.worldPos = worldPos.xyz;
    output.worldNormal = normalize(mul((float3x3)worldMatrix, inNormal));
    output.uv = inUV;
    output.objectIndex = objIdx;  // 传递 objectIndex 到 FS
    output.position = mul(viewProjMatrix, worldPos);
    return output;
}
