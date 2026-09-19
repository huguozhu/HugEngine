// ============================================================
// Nanite_Raster.vert.slang — 绘制端最小顶点着色器（§14.8 任务 3）
//
// 【为什么"不读顶点属性"】任务 3 只验证「计数 → 间接绘制」链的条数正确性，
//   不做真实簇光栅化。因此这里只用 SV_VertexID 生成一个覆盖整个 NDC 的三角形，
//   用 SV_InstanceID 接收间接命令里的 firstInstance（簇号）传给片元着色器。
//
// 【为什么恰好是 1 个片元/簇】模块把目标建成 1×1、视口/剪裁也设成 1×1：
//   覆盖整个 NDC 的三角形在这个目标上只会产生 1 个片元 ⇒ 片元里的原子加就等于
//   "被光栅化的簇数"，可用来逐位验证"计数为 k ⇒ 恰好画 k 次"。
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
    // 顶点 (-1,-1) / (3,-1) / (-1,3)：三角形覆盖整个 NDC 方块（含 [-1,1]²）
    float2 p = float2((vid == 1u) ? 3.0 : -1.0,
                      (vid == 2u) ? 3.0 : -1.0);
    o.position  = float4(p, 0.5, 1.0);
    o.clusterId = iid;
    return o;
}
