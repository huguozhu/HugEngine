// ============================================================
// Nanite_GBufferClear.comp.slang — §14.8 任务 18：清屏 8 张 GBuffer 颜色目标（模块接管写入）
//
// 【为什么不用渲染通道清屏】任务 18 起模块是 GBuffer 几何段的写入者，第一步就是把 8 张颜色
//   附件清成**既有路径的同一组清除值**（`GBufferRenderer_CPU.cpp:38-54`，一个数都不改），
//   这样"模块接管"与"既有路径"的起点完全一致，两档转储的差异只可能来自几何写入本身。
//   渲染通道清屏在这条链上**不可靠**：`BeginOffscreenPassMRT` 的 loadOp 取自 PSO 的
//   `colorLoadOp`，而 render pass 在 RHI 里是按"格式组合"复用的（Decal 通道用的是同一组
//   8 张格式 + `Load`）⇒ 实测模块清屏后未覆盖像素读出的是 (0,0,0,0) 而不是清除值
//   （albedo.a 均值 = 0.0000，而清除值是 1.0）。改用 compute 直接写 UAV：所有 8 张颜色目标
//   在任务 4 的 A1 裁决里都已经带 `UnorderedAccess`，写 UAV 完全在模块自己手里，
//   不依赖任何 render pass 的缓存行为。深度由 RHI 的 `ClearDepthStencil` 单独清（同样在模块手里）。
//
// 【清除值来自 push constant】8 × float4 = 128B（正好是引擎的 push constant 上限），
//   由 C++ 侧（`NaniteRaster::RecordGBufferClearPass`）填与既有路径逐位相同的常数。
//
// 【§14.3 依赖禁令】只依赖既有 GBuffer 契约（8 张颜色附件的格式与语义）。
// ============================================================

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_GBuffer0;  // MRT0 albedo+metallic
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_GBuffer1;  // MRT1 normal+roughness
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_GBuffer2;  // MRT2 emissive+ao
[[vk::binding(3, 0)]] [[vk::image_format("rg16f")]]   RWTexture2D<float4> u_GBuffer3;  // MRT3 velocity
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_GBuffer4;  // MRT4 worldPos
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_GBuffer5;  // MRT5 disneyA
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_GBuffer6;  // MRT6 disneyB
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_GBuffer7;  // MRT7 lightmapKey

[[vk::push_constant]] cbuffer NaniteGBufferClearParams {
    float4 clear0;
    float4 clear1;
    float4 clear2;
    float4 clear3;
    float4 clear4;
    float4 clear5;
    float4 clear6;
    float4 clear7;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w = 0, h = 0;
    u_GBuffer0.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    u_GBuffer0[id.xy] = clear0;
    u_GBuffer1[id.xy] = clear1;
    u_GBuffer2[id.xy] = clear2;
    u_GBuffer3[id.xy] = clear3;
    u_GBuffer4[id.xy] = clear4;
    u_GBuffer5[id.xy] = clear5;
    u_GBuffer6[id.xy] = clear6;
    u_GBuffer7[id.xy] = clear7;
}
