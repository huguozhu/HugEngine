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
};

// GPU Object（128 bytes，匹配 CPU GPUObjectData）
// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// set=0, binding=2: GPU 对象数据数组
[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_Objects : register(t2, space0);

// bindless SSBO 数组：实例化网格的实例变换（float4x4 元素缓冲，按句柄索引）
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<float4x4> u_InstanceTransforms[64] : register(t64, space2);
// bindless SSBO 数组：逐实例剔除后的可见实例索引（u32 列表，任务 25）
// 列表是**压缩**的：列表[i] = 第 i 个可见实例在实例变换数组里的下标
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<uint> u_VisibleInstanceIndices[64] : register(t128, space2);
// bindless SSBO 数组：骨骼蒙皮矩阵（world × inverseBind，按句柄索引）
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<float4x4> u_BoneMatrices[64] : register(t192, space2);

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
    // engine StaticVertex has no skin attributes; use Vulkan-equivalent defaults
    const uint4  inJoints  = uint4(0u, 0u, 0u, 0u);
    const float4 inWeights = float4(0.0, 0.0, 0.0, 1.0);
    PBRVSOutput output;
    // 【objectIndex 兜底】GPU-driven 路径下引擎不写 objectIndex（保持 0xCCCCCCCC 未初始化值），
    // 此时应改用 SV_InstanceID 作为物体索引；否则会越界索引 u_Objects 读到 0，顶点全部塌缩。
    uint objIdx = (objectIndex > 1000000u) ? instanceID : objectIndex;
    GPUObjectData obj = u_Objects[objIdx];
#if defined(HE_BYPASS_MATRIX)
    float4x4 worldMatrix = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
#else
    float4x4 worldMatrix = obj.worldMatrix;
#endif

    if (false && useInstanceID == 1u) {
        // GPU Driven：SV_InstanceID → objectIndex
        objIdx = instanceID;
        obj = u_Objects[objIdx];
        worldMatrix = obj.worldMatrix;
    } else if (false && useInstanceID == 2u) {
        // 实例化网格（B1）：实例变换 SSBO[句柄][实例下标]，材质取单个对象条目。
        // 任务 25：若开启了逐实例剔除，SV_InstanceID 先查可见列表（压缩后的下标）——
        // 于是只有通过视锥测试的实例才会被顶点着色器处理。
        uint instIdx = instanceID;
        if (instanceVisibleHandle != 0u)
            instIdx = u_VisibleInstanceIndices[instanceVisibleHandle][instanceID];
        worldMatrix = u_InstanceTransforms[instanceSSBOHandle][instIdx];
    } else if (false && useInstanceID == 3u) {
        // 骨骼蒙皮（C1b）：4 骨骼矩阵按权重加权；
        // 再乘实体世界矩阵（glTF 骨骼定义在网格节点局部空间）
        // Engine vertices carry no skin attributes (StaticVertex = pos/normal/uv),
        // so joints/weights read as zero. Zero weights would produce a zero
        // skin matrix and collapse the mesh; fall back to "bone 0, weight 1".
        float4 wW = inWeights;
        if (wW.x + wW.y + wW.z + wW.w <= 0.0) wW = float4(0.0, 0.0, 0.0, 1.0);
        float4x4 skinMat  = u_BoneMatrices[instanceSSBOHandle][inJoints.x] * wW.x;
        skinMat += u_BoneMatrices[instanceSSBOHandle][inJoints.y] * wW.y;
        skinMat += u_BoneMatrices[instanceSSBOHandle][inJoints.z] * wW.z;
        skinMat += u_BoneMatrices[instanceSSBOHandle][inJoints.w] * wW.w;
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
