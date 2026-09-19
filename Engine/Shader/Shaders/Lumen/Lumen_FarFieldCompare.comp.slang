// Lumen_FarFieldCompare.comp.slang — SDF 命中 vs 三角形命中（步骤 26 的验收口径）
//
// 【做什么】逐光线比较两条结果：
//   · 步骤 21 的 SDF sphere tracing：`(t_sdf, hit_sdf)`
//   · 步骤 26 的硬件光追：`(t_rt, hit_rt)`
// 把"两者都命中时的相对距离差""只有一边命中""远场子集"分类计数，写进统计缓冲。
//
// 【为什么按距离分带】步骤 26 的验收是"**远场**命中与三角形场景一致"。SDF 的精度随距离急剧下降
// （近层体素 21.3 单位、远层 42.7 单位，且远层是洪泛近似，自检已报"远场仍有超差"），
// 所以"SDF 与三角形不一致"在远场是**预期**；能证伪 HW RT 的只有**近场**——那里 SDF 是准的，
// 两者应当高度一致。于是按 `min(t_sdf, t_rt) ≤ 阈值` 切成"近带/远带"分别统计：
//   · 近带一致率高 ⇒ HW RT 打在真实三角形上（对照成立）；
//   · 远带一致率低 ⇒ 量化了"SDF 远场不可用"，正是本步骤存在的理由。
//
// 【统计全部用整数定点】GPU 上没有确定性的浮点原子加；整数原子加与顺序无关。
// 累加倍数取 1024（不是 1e6）：4.5 万条 × 相对误差 0.5 × 1e6 ≈ 2.2e10 会超过 u32 上限 ——
// 第一版就是 ×1e6，和值回绕后"平均相对差"读成 0.0776（真值远大于此，见 §附三十三）。
#include "ShaderTypes.slang"

[[vk::binding(0, 0)]] StructuredBuffer<float4> u_SdfResult;   // x=t, y=hit, z=dot(dir,n)
[[vk::binding(1, 0)]] StructuredBuffer<float4> u_RtResult;    // **每光线 2 条**：[2i]=(t, hit, 亮度), [2i+1]=(命中点,1)
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> u_Stats;       // 槽位见文件末尾注释
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> u_RelHist;     // 相对误差直方图（16 桶，0..100%）

[[vk::push_constant]] cbuffer CmpPC {
    uint4 dims;      // x = 光线数, y = 远场阈值（世界单位）, z = 近带上限（世界单位）, w = 未用
};

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= dims.x) return;
    const float4 sdf = u_SdfResult[tid.x];
    const float4 rt  = u_RtResult[tid.x * 2u];   // 【必须 ×2】光追结果每条光线占两个 float4

    const bool hs = sdf.y > 0.5 && sdf.x > 0.0;
    const bool hr = rt.y  > 0.5 && rt.x  > 0.0;
    InterlockedAdd(u_Stats[13], 1u);

    const float thr = (float)dims.y;
    const float near = (float)dims.z;
    const bool far = (hs && sdf.x >= thr) || (hr && rt.x >= thr);
    if (far) InterlockedAdd(u_Stats[8], 1u);

    if (hs) InterlockedAdd(u_Stats[12], 1u);
    if (hr) InterlockedAdd(u_Stats[11], 1u);
    // 【自交诊断】探针就贴在表面上，若 SDF march 的起点不跳过自身，第一条"命中"会落在 t≈0 —— 
    // 那样"间接光"其实是**本地反照率**，整个 GI 都是假的。这里把"t < 1 的命中"单独计数，
    // 并累计命中距离的均值，用来判定这件事（判据见 §附三十三）。
    if (hs) {
        if (sdf.x < 1.0) InterlockedAdd(u_Stats[22], 1u);
        InterlockedAdd(u_Stats[23], (uint)min(sdf.x * 16.0, 65535.0));
    }

    if (hs && hr) {
        InterlockedAdd(u_Stats[0], 1u);
        if (far) InterlockedAdd(u_Stats[9], 1u);
        const float denom = max(max(sdf.x, rt.x), 1e-3);
        const float rel   = abs(sdf.x - rt.x) / denom;
        const uint  sumFx = (uint)(rel * 1024.0 + 0.5);
        const uint  maxFx = (uint)(rel * 1048576.0 + 0.5);
        InterlockedAdd(u_Stats[4], sumFx);
        InterlockedMax(u_Stats[5], maxFx);
        if (far) InterlockedAdd(u_Stats[10], sumFx);
        if (rel <= 0.05) InterlockedAdd(u_Stats[6], 1u);
        if (rel <= 0.20) InterlockedAdd(u_Stats[7], 1u);

        // 近带 / 远带分开统计（判据在文件头）
        const bool nearBand = (min(sdf.x, rt.x) <= near);
        if (nearBand) {
            InterlockedAdd(u_Stats[14], 1u);
            InterlockedAdd(u_Stats[20], sumFx);
            if (rel <= 0.05) InterlockedAdd(u_Stats[16], 1u);
            if (rel <= 0.20) InterlockedAdd(u_Stats[15], 1u);
        } else {
            InterlockedAdd(u_Stats[17], 1u);
            InterlockedAdd(u_Stats[21], sumFx);
            if (rel <= 0.05) InterlockedAdd(u_Stats[19], 1u);
            if (rel <= 0.20) InterlockedAdd(u_Stats[18], 1u);
        }
        const uint bucket = (uint)clamp((int)(rel * 16.0), 0, 15);
        InterlockedAdd(u_RelHist[bucket], 1u);
    } else if (hs && !hr) {
        InterlockedAdd(u_Stats[1], 1u);   // SDF 命中而三角形未命中（SDF 的"假命中"或超出光追范围）
    } else if (!hs && hr) {
        InterlockedAdd(u_Stats[2], 1u);   // 只有三角形命中（SDF 漏掉的几何）
    } else {
        InterlockedAdd(u_Stats[3], 1u);   // 都未命中
    }
}

// 统计槽位（与 `LumenScene::RunFarFieldRT` 的读取逐条对应）
//   0  两侧都命中            1 只有 SDF 命中        2 只有三角形命中      3 都未命中
//   4  Σ相对差 ×1024        5  max 相对差 ×1048576  6  ≤5% 计数           7  ≤20% 计数
//   8  远场光线数            9 远场两侧都命中        10 远场 Σ相对差 ×1024
//   11 RT 命中数            12  SDF 命中数           13  光线总数
//   14 近带两侧都命中        15 近带 ≤20%           16  近带 ≤5%           20 近带 Σ相对差 ×1024
//   17 远带两侧都命中        18 远带 ≤20%           19  远带 ≤5%           21 远带 Σ相对差 ×1024
