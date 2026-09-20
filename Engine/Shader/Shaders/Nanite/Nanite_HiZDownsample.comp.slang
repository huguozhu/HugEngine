// ============================================================
// Nanite_HiZDownsample.comp.slang — 模块自持的 Hi-Z 金字塔下采样（§14.8 任务 15）
//
// 【口径】与仓库既有 `Culling/HiZDownsample.comp.slang:31` **逐字相同**：从上一级取 2×2 四个
//   纹素的**最小深度**写进下一级（深度是 Vulkan `[0,1]`：近 = 0 ⇒ 最小值 = 最近表面）。
//   层级/格式沿用既有的 `GPUCulling` Hi-Z 金字塔：`R32_FLOAT`、最多 8 层、层 L 覆盖 2^L×2^L 足迹，
//   **mip0 不写**（只写 mip1..N-1；遮挡测试的层下限因此钳到 1，见 `kNaniteHiZMinMip`）。
//
// 【为什么不直接复用 Culling/HiZDownsample.comp.slang（如实记录，两条硬原因）】
//   ① **布局冲突**：既有那份把"源"当**可采样图像**（`Texture2D` + `SamplerState`）。整条链的源与
//      目标都是**同一张** Hi-Z 纹理的不同 mip，而"写目标"要求该图处于 `GENERAL`、"采样源"要求
//      `SHADER_READ_ONLY` —— 一张图不可能同时是两种布局（RHI 只暴露**整图**屏障，无法按 mip 单独
//      转换）。本文件把 mip>0 的源改成**存储图像读取**（`RWTexture2D`，与写目标同布局），
//      于是整条链只需要在**开始/结束**各一次整图布局转换。
//   ② **描述符更新次序**：既有构建器 `GPUCulling::BuildHiZPyramid`（`GPUCulling.cpp:478-543`）
//      在循环里**逐 mip 更新同一个描述符集**；而本引擎的 GPU 在**执行期**读取描述符、
//      最后一次主机写对整段命令缓冲生效（实测：把同一个绑定先指向 A、派发后再指向 B，
//      派发读到的是 B ⇒ 那 7 次派发全部用最后一个状态）⇒ 它写入的是"2×2 的 mip6"（当时为空）
//      而不是金字塔，结果**全 0**。本文件配合"每个目标 mip 一个专属描述符集"（见 `NaniteCull`）
//      从根上避开这条次序依赖。
//   【既有路径为什么一直没被发现】`BuildHiZPyramid` 只被 `HiZ_Build` pass 调用，而该 pass 的注册
//   条件是 `useTwoPhase || ShouldRunSpecular()`；`useTwoPhase` 在所有配置里恒为 false，
//   07.Nanite 的默认 GI 预设也没有 SSR ⇒ 这条路径在本仓库**从未被真正执行过**。
//
// 【推常量】与既有 shader 的 `Params` 同形（24B）：源/目标尺寸 + 源层号。
// ============================================================

[[vk::binding(0, 0)]] Texture2D<float>   u_Depth;        // srcMip == 0 时的源：本帧深度纹理
[[vk::binding(0, 0)]] SamplerState       u_PointSampler; // 点采样（层间用显式 LOD 0）
[[vk::binding(1, 0)]] RWTexture2D<float> u_SrcMip;       // srcMip > 0 时的源：上一级金字塔（存储视图）
[[vk::binding(2, 0)]] RWTexture2D<float> u_DstMip;       // 目标：下一级金字塔（存储视图）

[[vk::push_constant]] cbuffer Params {
    uint2 srcSize;   // 偏移 0 ：源级尺寸（像素；srcMip == 0 时 = 屏幕尺寸）
    uint2 dstSize;   // 偏移 8 ：目标级尺寸（像素）
    uint  srcMip;    // 偏移 16：源层号（0 = 深度纹理，>0 = 金字塔的该层）
    uint  _pad;      // 偏移 20：填充到 24B（与既有 shader 的布局一致）
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= dstSize.x || id.y >= dstSize.y) return;

    float d00;
    float d10;
    float d01;
    float d11;

    if (srcMip == 0u) {
        // 深度纹理：目标纹素 (x,y) 对应源的 2×2 块。用目标纹素中心求 UV 再取四个偏移 —— 与既有
        // `HiZDownsample.comp.slang` 的采样点**完全一致**（点采样 + int2 偏移，不引入插值差异）。
        float2 uv = (float2(float(id.x), float(id.y)) + 0.5) / float2(float(dstSize.x), float(dstSize.y));
        d00 = u_Depth.SampleLevel(u_PointSampler, uv, 0, int2(0, 0));
        d10 = u_Depth.SampleLevel(u_PointSampler, uv, 0, int2(1, 0));
        d01 = u_Depth.SampleLevel(u_PointSampler, uv, 0, int2(0, 1));
        d11 = u_Depth.SampleLevel(u_PointSampler, uv, 0, int2(1, 1));
    } else {
        // 上一级金字塔：显式 2×2 纹素读取（存储图像，无采样器 ⇒ 与写目标同布局，不需要按 mip 转换）
        uint2 s = id.xy * 2u;
        d00 = u_SrcMip[int2(int(s.x),      int(s.y))];
        d10 = u_SrcMip[int2(int(s.x) + 1,  int(s.y))];
        d01 = u_SrcMip[int2(int(s.x),      int(s.y) + 1)];
        d11 = u_SrcMip[int2(int(s.x) + 1,  int(s.y) + 1)];
    }

    u_DstMip[id.xy] = min(min(d00, d10), min(d01, d11));
}
