// ============================================================
// GBuffer.mesh.slang — Mesh Shader 版 GBuffer 几何输出（P9）
//
// 【在管线里的位置】替代「IA + GBuffer.vert」的几何装配段：mesh shader 直接从
//   Storage Buffer 按索引读顶点（没有输入装配阶段），做与 GBuffer.vert **逐字一致**的
//   世界变换 / 法线变换，把顶点与图元写进 `verts[]` / `prims[]`，再由硬件光栅器
//   交给同一个 `GBuffer.frag`（片元着色器一行未改）。CPU 侧用
//   `DrawMeshTasks(groupCount,1,1)` 取代 `DrawIndexed(indexCount)`。
//
// 【几何从哪来】只读 `u_MeshVertices` / `u_MeshIndices` 两张 SSBO：
//   它们就是 `MeshBatcher` 每帧合批出来的**合并 VB/IB**（索引已是绝对顶点索引，
//   `firstIndex` 指向该物体的索引起点）。逐绘制只要推送 (firstIndex, triangleCount)，
//   故一次绑定服务整帧所有物体，不需要"每物体一次描述符集改写"。
//
// 【一个工作组画多少】64 个三角形（逐角点展开 = 192 输出顶点 / 64 图元）。
//   D3D12 的 `maxMeshOutputVertices` 典型值 256，`maxMeshOutputPrimitives` 典型值 256；
//   Vulkan 同一组上限。192/64 在两者之内（与 `Nanite_HardRaster.mesh.slang` 同一数量级，
//   那条管线已实测通过）。三角形更多时由 CPU 多派发几个工作组（groupCount = ceil(tri/64)）。
//
// 【输出顶点语义】直接复用 `MeshVS.slang` 的 `GBufferVSOutput`（与 GBuffer.vert / GBuffer.frag
//   是**同一个结构体**）⇒ MS 输出与 PS 输入逐字段同源，不会各自漂移；`objectIndex` 上的
//   `nointerpolation` 也随结构体一起带过来。
//
// 【已知边界】mesh 着色器没有 `SV_InstanceID`：本路径只支持**逐物体绘制**
//   （push constant `useInstanceID == 0`）。GPU 驱动路径把物体下标写进 push constant
//   `objectIndex`（= 合批序号），实例化网格（useInstanceID == 2）与 GPU 剔除的
//   "firstInstance 传对象号"语义没有对应物，仍走原来的 IA 路径。
// ============================================================

#include "ShaderTypes.slang"
#include "MeshVS.slang"   // GBufferVSOutput（与 GBuffer.vert / GBuffer.frag 同一份）

// ── 与 C++ `he::StaticVertex`（Scene/MeshComponent.h）**逐字节一致**：48 字节 ──
// 【为什么带显式填充】引擎的 Math.h 打开了 `GLM_FORCE_DEFAULT_ALIGNED_GENTYPES`，
//   glm::vec3 是 16 字节对齐（sizeof=16）、glm::vec2 是 8 字节对齐，于是
//   StaticVertex = { vec3 position@0, vec3 normal@16, vec2 uv@32 }，总大小 48 字节
//   （实测 MeshBatcher 日志：192694 顶点 = 9032 KB ⇒ 48 B/顶点）。
//   Slang 默认按 4 字节对齐（float3 紧排 12B、float2 紧排 8B = 32B），直接用会**整体错位**
//   ⇒ 顶点读出垃圾坐标（实测画面撕裂成大片三角形）。这里显式补齐。
struct GBufferMeshVertex {
    float3 position;   // offset 0,  实际占 16B（含 4B 对齐填充）
    float  _padPos;
    float3 normal;     // offset 16, 实际占 16B
    float  _padNrm;
    float2 uv;         // offset 32, 实际占 16B（8B 数据 + 8B 尾填充）
    float2 _padUV;
};

// 合批几何（MeshBatcher 的合并 VB / IB），一次绑定服务整帧：逐绘制用 push constant 偏移
[[vk::binding(kGPUBinding_MeshVertices, kGPUDescSet_PerFrame)]] StructuredBuffer<GBufferMeshVertex> u_MeshVertices : register(t21, space0);
[[vk::binding(kGPUBinding_MeshIndices,  kGPUDescSet_PerFrame)]] StructuredBuffer<uint>                 u_MeshIndices  : register(t22, space0);

// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）；与 GBuffer.vert 同一个 binding
[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_Objects : register(t2, space0);

// ── Push constant：前 4 个字段与 GBuffer.vert 的 FrameConstants **逐字节一致**（192B）──
//   offset 0/64  : 本帧 / 上帧 VP（都含各自的 TAA 抖动，velocity 口径与顶点路径相同）
//   offset 128   : objectIndex（= 物体缓冲下标；mesh 路径由 CPU 直接给，不取实例号）
//   offset 132   : useInstanceID（mesh 路径恒 0；1/2 的语义见上面的"已知边界"）
//   offset 136/140: 实例 SSBO / 可见列表句柄（mesh 路径不用，保留占位以对齐布局）
//   offset 144   : firstIndex（本次绘制在合并 IB 里的索引起点）
//   offset 148   : triangleCount（本次绘制的三角形数 = indexCount / 3）
//   其余填到 192B：与 GBufferRenderer 里 192 字节的 push constant 结构体对齐。
[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;         // offset 0,   64B
    float4x4 prevViewProjMatrix;     // offset 64,  64B
    uint     objectIndex;            // offset 128, 4B
    uint     useInstanceID;          // offset 132, 4B
    uint     instanceSSBOHandle;     // offset 136, 4B
    uint     instanceVisibleHandle;  // offset 140, 4B
    uint     firstIndex;             // offset 144, 4B
    uint     triangleCount;          // offset 148, 4B
    uint     _pad[10];               // 填充到 192B
};

// 每工作组的容量（改这里必须同步 CPU 侧的 kGBufferMeshTrianglesPerGroup）
static const uint kMeshGroupMaxTriangles  = 64u;   // 每工作组处理的三角形数
static const uint kMeshGroupMaxVertices   = 192u;  // 3 × 64：逐角点展开，不做索引去重
static const uint kMeshGroupMaxPrimitives = 64u;   // 与三角形数一一对应

[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void main(
    uint  tid : SV_GroupIndex,                        // 0..63：组内三角形槽位
    uint3 gid : SV_GroupID,
    out vertices GBufferVSOutput verts[kMeshGroupMaxVertices],
    out indices uint3 prims[kMeshGroupMaxPrimitives])
{
    // ── 本组负责的三角形区间（只依赖 gid 与 push constant ⇒ 组内所有线程取值相同）──
    const uint firstTri = gid.x * kMeshGroupMaxTriangles;
    const uint triCount = (triangleCount > firstTri)
                        ? min(kMeshGroupMaxTriangles, triangleCount - firstTri)
                        : 0u;

    // ── 整个入口**只调用这一次** `SetMeshOutputCounts`，且它支配所有输出写入 ──
    // 【不要在这之后紧跟 `if (triCount == 0u) return;`】dxc 优化会按该分支把调用复制成
    //   "0 / 非 0" 两份，DXIL 校验报 "cannot be called multiple times"（Nanite 硬光栅踩过）。
    //   这里把"没有图元"表达为 triCount == 0：下面的 `tid >= triCount` 对所有线程成立。
    SetMeshOutputCounts(3u * triCount, triCount);

    if (tid >= triCount) return;   // 末组的多余线程 / 多派发的组

    // ── 世界矩阵：逐物体语义（objectIndex 由 CPU 推送），与 GBuffer.vert 同源 ──
    // GBuffer.vert 在 useInstanceID == 2 时会换用实例 SSBO 的矩阵；mesh 路径不支持实例化
    //（没有 SV_InstanceID），只用 u_Objects 的 worldMatrix，与 useInstanceID == 0 完全一致。
    const uint objIdx = objectIndex;
    GPUObjectData obj = u_Objects[objIdx];
    const float4x4 worldMatrix = obj.worldMatrix;
    const float3x3 worldNormal  = (float3x3)worldMatrix;

    // ── 三个角点：按索引取顶点 → 世界变换 → 裁剪空间，属性与 GBuffer.vert 逐项对齐 ──
    const uint triBase = (firstIndex + (firstTri + tid) * 3u);
    [unroll]
    for (uint k = 0u; k < 3u; ++k) {
        const uint vi = u_MeshIndices[triBase + k];
        const GBufferMeshVertex v = u_MeshVertices[vi];

        const float4 worldPos = mul(worldMatrix, float4(v.position, 1.0));

        GBufferVSOutput outVert;
        outVert.position    = mul(viewProjMatrix, worldPos);      // 含当前帧抖动（与顶点路径一致）
        outVert.prevClipPos = mul(prevViewProjMatrix, worldPos);  // 含上帧抖动
        outVert.worldPos    = worldPos.xyz;
        outVert.worldNormal = normalize(mul(worldNormal, v.normal));
        outVert.uv          = v.uv;
        // 片元用它查材质 / 包围盒：必须是"本物体"的下标（同 GBuffer.vert 的 objIdx 语义）
        outVert.objectIndex = objIdx;

        verts[3u * tid + k] = outVert;
    }

    // 图元索引：第 tid 个三角形用第 3tid..3tid+2 个输出顶点（每个三角形只被一个线程写 ⇒ 无竞态）
    prims[tid] = uint3(3u * tid + 0u, 3u * tid + 1u, 3u * tid + 2u);
}
