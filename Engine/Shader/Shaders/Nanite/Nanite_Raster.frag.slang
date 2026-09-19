// ============================================================
// Nanite_Raster.frag.slang — 绘制端最小片元着色器（§14.8 任务 3）
//
// 【唯一职责】每被光栅化一个簇，就把"已光栅化簇数"原子加一。
//   配合 1×1 目标 / 1×1 视口（见 Nanite_Raster.vert.slang 的说明），
//   每个被 `DrawIndexedIndirectCount` 真正绘制的簇恰好产生 1 个片元，
//   因此该计数就是"实际绘制次数"，用于验收 count_buffer == rasterized_clusters。
//
// 【为什么必须 discard 掉可见输出】模块**绝不能**改动可见画面：本 pass 渲染到模块
//   自建的 1×1 R8 小目标（不是 GBuffer 的任何一张），返回值只写进那张小目标。
//   颜色值本身无意义，写红色只是为了在 RenderDoc 里能看出"这个 pass 确实画了"。
//
// 【§14.3 依赖禁令】只依赖模块自持的计数缓冲，不引用 GI/Lumen/GPUCulling。
// ============================================================

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> u_RasterCount;   // [0] = 已光栅化簇数

struct RasterVSOut {
    float4 position  : SV_Position;
    uint   clusterId : TEXCOORD0;
};

[shader("fragment")]
float4 fragmentMain(RasterVSOut input) : SV_Target {
    // 每个片元 = 一个被光栅化的簇（1×1 目标保证每簇只有一个片元）
    InterlockedAdd(u_RasterCount[0], 1u);
    return float4(1.0, 0.0, 0.0, 1.0);
}
