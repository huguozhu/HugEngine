// ============================================================
// Nanite_DepthResolve.vert.slang — 深度解析通道的顶点着色器（全屏大三角形）
//
// 【为什么需要这个通道】任务 18 的软光栅是 compute（写 GBuffer 颜色 UAV），而
//   `D32_SFLOAT` 的 GBuffer 深度**没有** `UnorderedAccess` usage（任务 4 的 A1 裁决只给 8 张
//   颜色目标加了 UAV，且本次改动面禁止改 `GBufferRenderer`）⇒ compute 写不了深度。
//   裁决给出的另一条路正是"深度继续走既有附件路径（或片元 `SV_Depth`）"，本通道就是后半句：
//   从软光栅第 1 趟定下的**深度键**（R32_UINT）还原 NDC 深度，用 `SV_Depth` 写进既有深度附件。
//
// 【全屏大三角形】不加顶点缓冲、不读任何输入：3 个顶点覆盖整个 NDC 方块
//   （`SV_VertexID` 0/1/2 → (-1,-1) / (3,-1) / (-1,3)）。每个像素恰好一个片元
//   ⇒ 深度写入没有竞态（不需要 interlock），天空像素也会被写成远平面。
// ============================================================

struct DepthResolveVSOutput {
    float4 position : SV_Position;
};

DepthResolveVSOutput vertexMain(uint vertexId : SV_VertexID) {
    // 0 → (0,0)、1 → (2,0)、2 → (0,2)，再映到 NDC 的 (-1,-1) / (3,-1) / (-1,3)
    float2 corner = float2((float)((vertexId << 1) & 2u), (float)(vertexId & 2u));
    DepthResolveVSOutput output;
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}
