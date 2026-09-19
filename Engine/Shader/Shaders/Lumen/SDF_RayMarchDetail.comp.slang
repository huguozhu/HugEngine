// SDF_RayMarchDetail.comp.slang — 逐 mesh 的"细节追踪"（步骤 11 的精度补齐）
//
// 【为什么需要它】Global SDF 是单层粗场（本场景 128³ → 24.66 单位体素），三线性插值把表面抹平
// 到数个体素，命中距离不可能优于这个尺度（实测平均误差 3.15 体素）。UE 的 Lumen 用的是同一套
// 思路的补救：**Detail Tracing** —— 近处（前 2 m）直接对每个 mesh 自己的距离场求交，只有远处
// 才用合并后的 Global DF。
//
// 【调用方式】每个 mesh 一次 dispatch（由 C++ 侧循环），光线跑完该 mesh 的 AABB 就停；
// 结果按 **min 归约**写到共享的 u_RayT（位模式存 +inf = 未命中），最后与全局追踪的结果取 min，
// 得到"最近命中"。每一次单独追踪都是**下界**（场本身是下界、且步进无上限步长），所以 min 合并
// 之后仍然不会高估 —— 安全性质不被破坏。
//
// 【与全局版的两点差异】① 收敛阈值 eps 取**该 mesh 的体素**（小网格可到 0.1 单位量级）；
// ② 出 AABB 即停（AABB 外谈"到该 mesh 的距离"对本 mesh 的求交没有意义）。

[[vk::binding(0, 0)]] Texture3D<float>          u_Field;
[[vk::binding(0, 0)]] SamplerState              u_FieldSampler;
[[vk::binding(2, 0)]] StructuredBuffer<float4>  u_RayOrigin;
[[vk::binding(3, 0)]] StructuredBuffer<float4>  u_RayDir;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>  u_RayT;   // 位模式：min 归约（+inf = 未命中）

[[vk::push_constant]] cbuffer DetailPC {
    float4 originVoxel;   // xyz = 该 mesh 距离场原点, w = 该 mesh 体素边长
    uint4  grid;          // xyz = 该 mesh 分辨率, w = 射线数
    float4 marchParams;   // x = 最大步数, y = eps（世界单位）, z = 最大距离
};

float SampleField(float3 p) {
    const float3 local = (p - originVoxel.xyz) / originVoxel.w;
    const float3 uv    = (local + 0.5) / float3(grid.xyz);
    return u_Field.SampleLevel(u_FieldSampler, uv, 0);
}

bool InsideField(float3 p) {
    const float3 local = (p - originVoxel.xyz) / originVoxel.w;
    return all(local >= 0.0) && all(local <= float3(grid.xyz));
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= grid.w) return;

    const float3 ro = u_RayOrigin[i].xyz;
    const float3 rd = normalize(u_RayDir[i].xyz);
    const uint   maxSteps = (uint)marchParams.x;
    const float  eps      = marchParams.y;
    const float  maxDist  = marchParams.z;

    float t = 0.0;
    bool  advanced = false;   // 命中判据要求"沿射线真正走过一步"
    for (uint s = 0; s < maxSteps; ++s) {
        const float3 p = ro + rd * t;
        if (!InsideField(p)) return;            // 出该 mesh 的 AABB：本 mesh 不再有贡献
        const float d = SampleField(p);         // **带符号**：内部为负
        // 带符号场的步进用 |d|（到表面的距离），命中判定也用 |d|：这样"起点在几何内部"的射线会
        // 一路走到最近的表面，而不是在 t=0 就自认为命中（那正是无符号场时的假命中来源）。
        const float ad = abs(d);
        if (advanced && ad < eps) {
            // 命中：以 min 归约写入（t ≥ 0，浮点位模式与无符号整数同序）
            InterlockedMin(u_RayT[i], asuint(t));
            return;
        }
        // 安全步长系数 0.5（理由同全局版：近似场全量步进会穿漏）
        // 步长下限与命中判据分开（理由同全局版）
        t += max(ad * 0.5, eps * 0.02);
        advanced = true;
        if (t > maxDist) return;
    }
}
