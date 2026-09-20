// ============================================================
// Nanite_MeshTest.mesh.slang — §14.8 任务 6 的最小 Mesh Shader
//
// 【与 GBuffer.mesh.slang 的区别（务必不要混淆）】
//   `GBuffer/GBuffer.mesh.slang:30-31` 是**桩**：`SetMeshOutputCounts(0, 0)`，一个图元都不输出，
//   而且它属于 GBuffer 路径，接进可见画面是**任务 22**（硬光栅分流）的事。
//   本文件是 Nanite 模块**自己**的 mesh 通道：真正调用 `SetMeshOutputCounts(4, 2)` 输出
//   4 个顶点 + 2 个三角形，并且只画进模块自建的 1×1 R8 小目标 —— 对可见画面零影响。
//
// 【为什么输出「全屏四边形 = 2 个三角形」】
//   目标是 1×1（`kNaniteMeshTestTargetSize`），四边形恰好完整覆盖这唯一一个像素：
//   两个三角形各产生 1 个片元 ⇒ 片元的原子计数 = 2 = **mesh shader 真正输出并被光栅化的图元数**。
//   于是任务 6 的"非空输出"自证可以同时给出两个互相独立的 GPU 读数：
//     · `meshlet_outputs`（片元原子计数缓冲）= 2（>= 1 即非空）
//     · `target_max`（1×1 R8 目标读回）= 255（写入了 1.0 ⇒ 非零）
//   若这里沿用 GBuffer.mesh.slang 的 (0,0)，两个读数都会是 0，验收直接失败。
//
// 【为什么不用 DrawMeshTasksIndirect】任务 6 只要证明"mesh PSO 能被建出来并画出一帧"，
//   任务数由 CPU 直接给出（1 个工作组）就够了；间接/计数驱动属于任务 22 的分流。
//
// 【§14.3 依赖禁令】不引用 GI_* / Lumen* / GPUCulling 的任何结构。
// ============================================================

// 顶点输出：只带 SV_POSITION（内置语义，片元侧必然匹配）。
// 刻意不声明任何用户 varying —— mesh → fragment 的 SPIR-V 链接不需要额外的 location 约定，
// 少一处可能对不上的地方（任务 6 只关心"画出来了"，不关心插值数据）。
struct MeshTestVertexOut
{
    float4 position : SV_POSITION;
};

// 工作组 4 个线程：每个线程负责一个顶点（tid == 顶点号）。
[numthreads(4, 1, 1)]
[outputtopology("triangle")]
void main(
    uint tid : SV_GroupIndex,                  // 线程索引 0..3 = 顶点索引
    out vertices MeshTestVertexOut verts[4],   // 最多 4 个顶点
    out indices uint3 prims[2])                // 最多 2 个三角形
{
    // 覆盖整个 NDC 的四边形（顺序：左下 → 右下 → 右上 → 左上）
    const float2 positions[4] = {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0,  1.0),
    };

    // Vulkan 要求：所有线程都必须调用，且参数一致。
    // 【这就是本任务的"非空"所在】4 个顶点、2 个图元（不是桩的 0, 0）。
    SetMeshOutputCounts(4, 2);

    verts[tid].position = float4(positions[tid], 0.0, 1.0);

    // 图元索引只需输出一次（约定由 0 号线程写）
    if (tid == 0)
    {
        prims[0] = uint3(0, 1, 2);
        prims[1] = uint3(0, 2, 3);
    }
}
