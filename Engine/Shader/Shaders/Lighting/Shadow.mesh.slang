// ============================================================
// Shadow.mesh.slang — T10：阴影贴图的 mesh shader 变体（MS-2）
//
// 【为什么要有它】IA 版本的阴影通道（`Shadow.vert.slang`）依赖"顶点缓冲 + 输入装配"：
//   每画一个物体要 CPU 设一次顶点/索引缓冲并 `DrawIndexed`。T10 要求阴影 pass 也走 MS，
//   这样：
//     · 几何入口与 GBuffer 一致（同一条 mesh 管线族），T11 才能把两条 IA 管线一起移除；
//     · 每个工作组自己决定要发射多少图元（按物体三角形数夹取），不受 IA 的"整批顶点"限制。
//
// 【工作组织】一个工作组 = 一个物体的**64 个三角形**：
//   `SV_GroupID.x` = 物体在 push constant 里的编号（当前 CPU 逐个物体派发），
//   `SV_GroupID.y` = 三角形分块序号（`firstTriangle + 64 * y`）。
//   输出上限 192 顶点 / 64 图元，与 `Nanite_HardRaster.mesh.slang` 取同一组上限值
//   （本机 `maxMeshOutputVertices = 256`），超限时 `EnsureShadowMeshReady` 会拒绝建 PSO。
//
// 【§14.3 依赖禁令】只依赖 `ShaderTypes.slang`（`GPUObjectData` / `ShadowPushConstant`）。
// ============================================================

#include "ShaderTypes.slang"

// ── 与 C++ 侧常量一一对应（改一处必须同步）──
//   · `kShadowMeshGroupThreads`  = 64  （= 分块三角形数，一个线程一个三角形）
//   · `kShadowMeshMaxVertices`   = 192 （3 × 64）
//   · `kShadowMeshMaxPrimitives` = 64
static const uint kShadowMeshGroupThreads  = 64u;
static const uint kShadowMeshMaxVertices   = 192u;
static const uint kShadowMeshMaxPrimitives = 64u;

// ── 绑定：与 IA 版阴影共用"阴影专用对象数据"（binding 26），另加 VB/IB 的 SRV ──
[[vk::binding(kGPUBinding_ShadowObjects, kGPUDescSet_PerFrame)]]
StructuredBuffer<GPUObjectData> u_Objects : register(t26, space0);

/// `StaticVertex`（32B：position + normal + uv）——**必须整条结构读**：
///   IA 版靠顶点缓冲的 stride 跳步，MS 版没有 IA，若声明成 `StructuredBuffer<float3>`
///   stride 会变成 12B ⇒ 读到的是"每 12 字节取 3 个 float"，位置全错（实测与 IA 版差异 5.9%）。
///   下面的布局与 `he::StaticVertex` 逐字段一致（改一处必须同步 C++ 侧）。
struct ShadowVertex {
    float3 position;
    float3 normal;
    float2 uv;
};
[[vk::binding(27, kGPUDescSet_PerFrame)]] StructuredBuffer<ShadowVertex> u_Vertices : register(t27, space0);
// 索引缓冲按 32 位索引读取
[[vk::binding(28, kGPUDescSet_PerFrame)]] StructuredBuffer<uint>   u_Indices   : register(t28, space0);

// Push constants：与 IA 版同名同序，额外带"本物体的三角形区间"
[[vk::push_constant]] cbuffer ShadowConstants {
    float4x4 lightViewProj;   // 与 IA 版一致：光源裁剪矩阵
    uint     objectIndex;     // 与 IA 版一致：`u_Objects` 下标
    uint     firstTriangle;   // T10：本组起点（三角形下标）
    uint     triangleCount;   // T10：本物体三角形总数（用于夹取）
    uint     _pad0;
};

struct ShadowMeshPayload {
    float4 position : SV_Position;
};

[outputtopology("triangle")]
[numthreads(kShadowMeshGroupThreads, 1, 1)]
void main(uint3 gid : SV_GroupID,
                    uint  tid : SV_GroupThreadID,
                    out vertices ShadowMeshPayload vout[kShadowMeshMaxVertices],
                    out indices uint3            pout[kShadowMeshMaxPrimitives]) {
    // ① 本组覆盖的三角形区间（跨物体边界不外溢）
    const uint begin = firstTriangle + gid.y * kShadowMeshGroupThreads;
    const uint end   = min(triangleCount, begin + kShadowMeshGroupThreads);
    const uint count = (begin < end) ? (end - begin) : 0u;

    // ② 统一报告一次输出数量（D3D12 要求：整入口只调一次，且支配所有输出写）
    // 【不要紧跟 `if (count == 0) return;`】dxc 会按该分支把本调用复制成两份，
    //   DXIL 校验直接报 "SetMeshOutputCounts cannot be called multiple times"
    //   （NaNite 硬光栅踩过同一个坑，见 `Nanite_HardRaster.mesh.slang` 的说明）。
    //   这里只保留 `tid >= count` 的早退：count 为 0 时它对所有线程成立，等价于"没图元"。
    SetMeshOutputCounts(3u * count, count);
    if (tid >= count) return;

    // ③ 每个线程负责一个三角形：读 3 个索引 → 3 个位置 → 变换到光源裁剪空间
    GPUObjectData obj = u_Objects[objectIndex];
    const uint tri = begin + tid;
    uint3 idx = uint3(u_Indices[tri * 3u], u_Indices[tri * 3u + 1u], u_Indices[tri * 3u + 2u]);
    for (uint k = 0u; k < 3u; ++k) {
        const uint li = (k == 0u) ? idx.x : ((k == 1u) ? idx.y : idx.z);
        const float3 local = u_Vertices[li].position;
        const float4 world = mul(obj.worldMatrix, float4(local, 1.0f));
        vout[tid * 3u + k].position = mul(lightViewProj, world);
        if (k == 0u) pout[tid] = uint3(tid * 3u, tid * 3u + 1u, tid * 3u + 2u);
    }
}

// 空的片元着色器：阴影只写深度（与 IA 版的 `Shadow.frag.slang` 同职责）

// 【片元复用】`Shadow.frag.slang`（无输入、只写深度的空片元）与本文件的输出兼容，
//   因此 MS 管线直接复用它，不再另写一份。
