// ============================================================
// Nanite_Raster.vert.slang — 绘制端最小顶点着色器（§14.8 任务 3；任务 16 改一处取模）
//
// 【为什么"不读顶点属性"】本通道只验证「可见簇 → 间接绘制」链的条数正确性（真实软光栅是
//   任务 18），不做真实簇光栅化。因此这里只用 SV_VertexID 生成一个覆盖整个 NDC 的三角形，
//   用 SV_InstanceID 接收间接命令里的 firstInstance（簇号）传给片元着色器。
//
// 【任务 16 的改动：`vid` → `vid % 3`（唯一一处，且是必须的）】
//   任务 3 的假簇链每簇只有 1 个三角形、`firstIndex = 0`、`vertexOffset = 0` ⇒ `vid ∈ {0,1,2}`，
//   所以直接比 `vid == 1 / vid == 2` 就够了。任务 16 起 indexCount / firstIndex / vertexOffset
//   都是**簇的真实值**（最多 192 个索引、任意的簇起始位置与顶点偏移），`vid` 不再从 0 开始。
//   光栅端绑定的索引缓冲是模块自持的**占位**缓冲（内容 = 0,1,2 周而复始，见 `NaniteRaster.cpp`），
//   因此对任意 `firstIndex`（3 的倍数）与任意 `vertexOffset`：
//       · 每个三角形的三个 `vid` 恰好是 `{v, v+1, v+2}`（v = vertexOffset + 3k）
//       · `vid % 3` 因此是 `{0,1,2}` 的一个**排列** ⇒ 三个顶点位置互不相同 ⇒ 三角形非退化
//   这正是"每条命令恰好产生片元"的前提：退化三角形不会产生任何片元，计数会失真。
//   【仍然不读任何顶点属性】位置只由 `vid % 3` 决定，与索引值、顶点数据无关。
//
// 【为什么恰好是 1 个片元/三角形】模块把目标建成 1×1、视口/剪裁也设成 1×1：
//   覆盖整个 NDC 的三角形在这个目标上只会产生 1 个片元。
//   【注意：任务 16 起"每个绘制"可能有多个三角形（= 簇内三角形数）】所以片元里的原子加**不能**
//   再当作"绘制次数"：计数改为"每个绘制恰好一次"（片元里用簇号位图判定首次），见
//   `Nanite_Raster.frag.slang`。
//
// 【§14.3 依赖禁令】只做绘制端的最小通道，不采样任何 GI/Lumen/GPUCulling 资源。
// ============================================================

struct RasterVSOut {
    float4 position  : SV_Position;
    uint   clusterId : TEXCOORD0;   // 间接命令的 firstInstance（簇号）
};

[shader("vertex")]
RasterVSOut vertexMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    RasterVSOut o;
    // 顶点 (-1,-1) / (3,-1) / (-1,3)：三角形覆盖整个 NDC 方块（含 [-1,1]²）。
    // 【为什么用 `vid % 3` 而不是 `vid`】见文件头"任务 16 的改动"：真实的 `vid` 起点与步长
    // 由簇的 firstIndex/vertexOffset 决定，取模后才是"三角形内的第几个顶点"，且对任意
    // vertexOffset 都给出 {0,1,2} 的一个排列。
    uint corner = vid % 3u;
    float2 p = float2((corner == 1u) ? 3.0 : -1.0,
                      (corner == 2u) ? 3.0 : -1.0);
    o.position  = float4(p, 0.5, 1.0);
    o.clusterId = iid;
    return o;
}
