// ============================================================
// PBR.vert.slang — PBR 顶点着色器 (GPU-driven)
// per-object 数据从 Storage Buffer 读取
// ============================================================

#include "MeshVS.slang"   // GBufferVSOutput / PBRVSOutput / RSMVSOutput（接口唯一出处）
#include "ShaderTypes.slang"  // GPUObjectData 共享定义
// Explicit HLSL semantics: D3D12 matches input layout by semantic name+index,
// while Slang numbers inputs alphabetically (inJoints becomes VERTEX_IN_0).
struct PBRVertexInput {
    [[vk::location(0)]] float3 inPosition : POSITION0;
    [[vk::location(1)]] float3 inNormal   : NORMAL0;
    [[vk::location(2)]] float2 inUV       : TEXCOORD0;
    // 蒙皮属性（location 3/4）：只有蒙皮 PSO 的顶点布局提供（SkinnedVertex）；
    // 静态布局不提供时 Vulkan 读到 0，D3D12 后端从零缓冲槽位取 0，两端一致，
    // 且只有 useInstanceID==3（蒙皮 PSO）才会使用它们。
    [[vk::location(3)]] uint4  inJoints   : BLENDINDICES0;
    [[vk::location(4)]] float4 inWeights  : BLENDWEIGHT0;
};

// GPU Object（128 bytes，匹配 CPU GPUObjectData）
// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// set=0, binding=2: GPU 对象数据数组
[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_Objects : register(t2, space0);

// bindless SSBO 数组：实例化网格的实例变换（float4x4 元素缓冲，按句柄索引）
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<float4x4> u_InstanceTransforms[64] : register(t1024, space2);
// bindless SSBO 数组：逐实例剔除后的可见实例索引（u32 列表，任务 25）
// 列表是**压缩**的：列表[i] = 第 i 个可见实例在实例变换数组里的下标
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<uint> u_VisibleInstanceIndices[64] : register(t1088, space2);
// bindless SSBO 数组：骨骼蒙皮矩阵（world × inverseBind，按句柄索引）
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<float4x4> u_BoneMatrices[64] : register(t1152, space2);

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
    uint     instanceVisibleHandle; // useInstanceID=2 时的可见实例索引列表句柄（任务 25；0=未剔除）
    uint     instancePad[2];
};

PBRVSOutput vertexMain(PBRVertexInput vin, uint instanceID : SV_InstanceID) {
    const float3 inPosition = vin.inPosition;
    const float3 inNormal   = vin.inNormal;
    const float2 inUV       = vin.inUV;
    const uint4  inJoints   = vin.inJoints;
    const float4 inWeights  = vin.inWeights;
    PBRVSOutput output;
    // 【objectIndex 兜底】GPU-driven 路径下引擎不写 objectIndex（保持未初始化值），
    // 此时改用 SV_InstanceID 作为物体索引，避免越界索引 u_Objects。
    uint objIdx = (objectIndex > 1000000u) ? instanceID : objectIndex;
    GPUObjectData obj = u_Objects[objIdx];
    float4x4 worldMatrix = obj.worldMatrix;

    if (useInstanceID == 1u) {
        // GPU Driven：SV_InstanceID → objectIndex
        objIdx = instanceID;
        obj = u_Objects[objIdx];
        worldMatrix = obj.worldMatrix;
    } else if (useInstanceID == 2u) {
        // 实例化网格（B1）：实例变换 SSBO[句柄][实例下标]，材质取单个对象条目。
        // 任务 25：若开启了逐实例剔除，SV_InstanceID 先查可见列表（压缩后的下标）——
        // 于是只有通过视锥测试的实例才会被顶点着色器处理。
        uint instIdx = instanceID;
        if (instanceVisibleHandle != 0u)
            instIdx = u_VisibleInstanceIndices[instanceVisibleHandle][instanceID];
        worldMatrix = u_InstanceTransforms[instanceSSBOHandle][instIdx];
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
