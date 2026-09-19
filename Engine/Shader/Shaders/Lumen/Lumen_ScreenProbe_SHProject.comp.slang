// Lumen_ScreenProbe_SHProject.comp.slang — 探针 SH 投影（步骤 23）
//
// 【做什么】把步骤 21 追踪、步骤 22 着色的每个 (探针, 光线) 结果投成**二阶球谐**：
// 每探针 4 系数 × RGB = 12 floats，直接写回 `ScreenProbe` 的 shR/shG/shB 字段（步骤 20 已定好布局）。
//
// 【估计量】光线是**均匀半球采样**（pdf = 1/(2π)），于是辐射度的 SH 系数
//   l_i ≈ (1/N) Σ_j L_j·Y_i(d_j) / pdf = (2π/N) Σ_j L_j·Y_i(d_j)
// 白炉（L ≡ 1）下 0 阶项**解析可算**：l0 = 2π·Y00 = √π —— 与采样方向、采样数无关。
// 这正是本步骤验收"白炉下误差为 0"能被数值钉死的原因。
//
// 【辐照度重建】E(n) = π·l0·Y00 + (2π/3)·(l1·Y1(n) + l2·Y2(n) + l3·Y3(n))（clamped cosine 卷积系数）。
// 同一帧里再用"逐光线求和"独立算一遍参考值 E_ref(n) = (2π/N) Σ_j L_j·max(0, d_j·n·) 写进另一个缓冲：
// 两者在**白炉**下的差就是"SH 带限 + 有限采样"的误差，交给 CPU 侧统计（不是猜的）。
//
// 【光线方向从哪来】与追踪 pass **共用** `ScreenProbeSampling.slang`：同一 (探针, 光线, 帧) 必然
// 得到同一方向。不另开方向缓冲（省 16 MB/帧，也避免两份采样代码漂移）。
#include "Lumen/ScreenProbe.slang"
#include "Lumen/ScreenProbeSampling.slang"

[[vk::binding(0, 0)]] RWStructuredBuffer<ScreenProbe> u_Probes;   // 位置/法线来自步骤 20，SH 由本 pass 填
[[vk::binding(1, 0)]] StructuredBuffer<float4>        u_Radiance; // 每 (探针, 光线) 的辐射度（= 步骤 22 的命中点 albedo / 缺页中性值）
[[vk::binding(5, 0)]] StructuredBuffer<float4>        u_RayResult; // x=t, y=是否命中几何（未命中的光线贡献 0）
[[vk::binding(2, 0)]] RWStructuredBuffer<float4>      u_IrradSh;  // xyz = SH 重建的辐照度 E_sh(n)
[[vk::binding(3, 0)]] RWStructuredBuffer<float4>      u_IrradRef; // xyz = 逐光线求和的参考辐照度 E_ref(n)
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>        u_Stats;    // 0=有效探针 1=有效光线 2=l0 定点(|l0|*4096) 3=白炉 l0 偏差定点(|l0-√π|*65536)

[[vk::push_constant]] cbuffer ShPC {
    uint4 dims;        // x = 探针数, y = 每探针光线数, z = 随机种子（帧号）, w = 白炉标志（1 = 强制 L=1）
    uint4 mode;        // x = 采样模式（0=GGX, 1=均匀半球）, yzw = 未用
};

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= dims.x) return;

    ScreenProbe probe = u_Probes[tid.x];
    if (probe.position.w < 0.5) {   // 无几何的探针：SH 清零并标记无效
        probe.shR = float4(0.0, 0.0, 0.0, 0.0);
        probe.shG = float4(0.0, 0.0, 0.0, 0.0);
        probe.shB = float4(0.0, 0.0, 0.0, 0.0);
        u_Probes[tid.x] = probe;
        u_IrradSh[tid.x]  = float4(0.0, 0.0, 0.0, 0.0);
        u_IrradRef[tid.x] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const float3 n  = normalize(probe.normal.xyz);
    const float  roughness = clamp(probe.normal.w, 0.05, 1.0);
    const uint   N  = dims.y;
    const bool   furnace = (dims.w != 0u);

    // 4 系数 × RGB + 逐光线求和的参考辐照度（沿探针法线 n 求 E，与 SH 重建同方向比较）
    float3 l0 = float3(0.0, 0.0, 0.0), l1 = float3(0.0, 0.0, 0.0);
    float3 l2 = float3(0.0, 0.0, 0.0), l3 = float3(0.0, 0.0, 0.0), eRef = float3(0.0, 0.0, 0.0);
    uint   validRays = 0;

    // 与追踪 pass 完全相同地由**探针身份**导出随机流（不能跟槽位走，见 ScreenProbe_Trace 的说明）
    const uint probeKey = probe.meta.y * 4u + probe.meta.w;
    for (uint r = 0u; r < N; ++r) {
        const uint flat = tid.x * N + r;
        const float2 u = ProbeRayRandom(probeKey * N + r, dims.z);
        const float3 d = (mode.x == 0u) ? SampleGgxDirection(n, roughness, u)
                                        : SampleUniformHemisphere(n, u);
        // 白炉：辐射度恒为 1（不看 atlas）⇒ l0 必须等于 √π，与方向/采样数无关
        // 非白炉：只剩"这条光线有没有打到几何"这一道门。**不**再按"页是否命中"过滤 ——
        // 步骤 22 对缺页已经返回中性值 0.18（而不是黑），那正是它要继续往下的语义；
        // 若在这里再滤掉，实测每条探针只剩 0.9 条有效光线（8 条里 0.9 条），SH 等于没积。
        const float4 s = u_Radiance[flat];
        const bool   rayHit = u_RayResult[flat].y > 0.5f;
        const float3 L = furnace ? float3(1.0, 1.0, 1.0)
                                 : (rayHit ? s.xyz : float3(0.0, 0.0, 0.0));
        if (rayHit || furnace) ++validRays;

        l0 += L * SHBasis(0u, d);
        l1 += L * SHBasis(1u, d);
        l2 += L * SHBasis(2u, d);
        l3 += L * SHBasis(3u, d);
        eRef += L * max(0.0, dot(d, n));         // 参考：逐光线 cos 加权求和
    }

    // 均匀半球 pdf = 1/(2π) ⇒ 系数 = (2π/N)·Σ；参考辐照度 = (2π/N)·Σ L·cos
    const float k = 6.28318530718 / (float)N;
    l0 *= k; l1 *= k; l2 *= k; l3 *= k;
    eRef *= k;

    probe.shR = float4(l0.r, l1.r, l2.r, l3.r);
    probe.shG = float4(l0.g, l1.g, l2.g, l3.g);
    probe.shB = float4(l0.b, l1.b, l2.b, l3.b);
    u_Probes[tid.x] = probe;

    const float3 eSh = SHRadianceToIrradiance(l0, l1, l2, l3, n);
    u_IrradSh[tid.x]  = float4(eSh, 1.0);
    u_IrradRef[tid.x] = float4(eRef, 1.0);

    InterlockedAdd(u_Stats[0], 1u);
    InterlockedAdd(u_Stats[1], validRays);
    // 自检：l0 的 R 通道定点写进统计。均匀半球 + 白炉下 l0 必须等于 √π（与方向、采样数无关），
    // 所以这里同时累计"偏离 √π 的量"——CPU 侧除以 65536×探针数就是平均绝对偏差。
    InterlockedAdd(u_Stats[2], (uint)(abs(l0.r) * 4096.0 + 0.5));
    if (furnace) InterlockedAdd(u_Stats[3], (uint)(abs(l0.r - 1.7724539) * 65536.0 + 0.5));
}
