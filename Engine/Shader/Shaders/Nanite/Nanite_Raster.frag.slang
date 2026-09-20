// ============================================================
// Nanite_Raster.frag.slang — 绘制端最小片元着色器（§14.8 任务 3；任务 16 改为"每绘制一次"）
//
// 【唯一职责】把"被光栅化的**绘制条数**"原子加一：`primID == 0` 的片元恰好每个绘制一个。
//
// 【任务 16 为什么要改计数语义】任务 3 的假簇链每簇 1 个三角形 ⇒ "片元数 == 绘制数"，
//   于是片元里直接原子加一就够。任务 16 起 `indexCount` 是簇的**真实**索引个数
//   （`triangleCount × 3`，最多 192）⇒ 一条命令会光栅化出**多个**片元（全部落在同一个
//   1×1 像素上），片元数不再等于绘制数。判据改成"每个绘制恰好一次"：
//     · `SV_PrimitiveID` 是**绘制内**的图元序号（每次绘制都从 0 重新开始，与命令里
//       firstInstance/簇号无关）⇒ `primID == 0` 在且仅在"本条命令的第一个三角形"上成立；
//     · 因此该计数恒等于"**产生了片元的绘制条数**"——既不会把一条命令数多次，也不会漏掉
//       （每条命令的第一个三角形只要被光栅化就一定会跑一次片元）。
//   【为什么不用"按簇号去重位图"】可见簇引用是 (实例, 簇) 二元组：同一个簇会被同一份资产的
//   多个实例各引用一次，而命令的 `firstInstance` 只带簇号 ⇒ 按簇号去重会把多个实例的绘制
//   错误地折叠成一条。`primID` 是**逐次执行**的量，天然不受这条影响。
//   【为什么不能用 `vid` 推"绘制内序号"】簇的 `firstIndex/vertexOffset` 是真实值，顶点着色器
//   拿不到"起点的 vid"；而 `SV_PrimitiveID` 是硬件直接给的绘制内图元序号，无需任何推算。
//   【设备依赖（如实记录）】用 `SV_PrimitiveID` 会让 SPIR-V 声明 `OpCapability Geometry`
//   （实测：slangc 对 HLSL 拼写的 `SV_PrimitiveID` 会正确补上该 capability），而 Vulkan 的
//   SPIR-V 环境要求"Geometry ⇒ `VkPhysicalDeviceFeatures::geometryShader` 已启用"
//   （见 `VulkanDevice.cpp` 的启用处）。本引擎不建任何几何着色器管线，开启该特性只为满足
//   这条能力要求，对渲染结果零影响。
//
// 【为什么必须保留可见输出】模块**绝不能**改动可见画面：本 pass 渲染到模块自建的
//   1×1 R8 小目标（不是 GBuffer 的任何一张），返回值只写进那张小目标。
//   颜色值本身无意义，写红色只是为了在 RenderDoc 里能看出"这个 pass 确实画了"。
//
// 【§14.3 依赖禁令】只依赖模块自持的计数缓冲，不引用 GI/Lumen/GPUCulling。
// ============================================================

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> u_RasterCount;   // [0] = 被光栅化的绘制条数

struct RasterVSOut {
    float4 position  : SV_Position;
    uint   clusterId : TEXCOORD0;   // 间接命令的 firstInstance（簇号；本任务只用于调试观察）
};

[shader("fragment")]
float4 fragmentMain(RasterVSOut input, uint primId : SV_PrimitiveID) : SV_Target {
    // 每个绘制恰好加一：`primId` 是绘制内的图元序号（每条命令都从 0 开始）。
    if (primId == 0u) {
        InterlockedAdd(u_RasterCount[0], 1u);
    }
    return float4(1.0, 0.0, 0.0, 1.0);
}
