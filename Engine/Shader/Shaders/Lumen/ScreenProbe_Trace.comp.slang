// ScreenProbe_Trace.comp.slang — 探针半球追踪（步骤 21）
//
// 【做什么】每个探针按 `traceRep` 条**重要性采样**方向发射光线，用步骤 11 的同一套 sphere tracing
// 语义（两层全局场取 min、`max(d*0.25, eps*0.02)` 安全步长、`advanced && d < eps` 命中）求交，
// 把 (命中距离 t, 是否命中, dot(dir, n)) 写进结果缓冲。着色留给步骤 22（消费 L2 的 atlas）。
//
// 【采样模式】步骤 21 用的是 GGX 半向量采样（mode 0）；步骤 23 起默认切到**均匀半球采样**（mode 1,
// 见 `ScreenProbeSampling.slang` 的解释：均匀 pdf 让白炉下 SH 的 0 阶项有解析值 √π，验收可钉死）。
// 方向与随机数由共享头提供 ⇒ 追踪与投影**必然**用同一组方向（否则投影积的是另一组方向，最难查）。
#include "Lumen/ScreenProbe.slang"
#include "Lumen/ScreenProbeSampling.slang"

[[vk::binding(0, 0)]] Texture3D<float> u_Field;        // 近层
[[vk::binding(0, 0)]] SamplerState     u_FieldSampler;
[[vk::binding(6, 0)]] Texture3D<float> u_Field1;       // 远层
[[vk::binding(6, 0)]] SamplerState     u_FieldSampler1;
[[vk::binding(1, 0)]] StructuredBuffer<ScreenProbe> u_Probes;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4>    u_RayResult;   // 每 (探针, 光线) 一条：x=t, y=命中, z=dot(dir,n), w=未用
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>      u_Stats;       // 0=命中, 1=未命中, 2=总光线, 3=半球内(点积>0)光线
[[vk::binding(4, 0)]] RWStructuredBuffer<float4>    u_RayHitPos;   // xyz = 命中点（步骤 22 的着色输入）, w = 是否命中

[[vk::push_constant]] cbuffer TracePC {
    uint4  dims;          // x = 探针数, y = 每探针光线数, z = 随机种子（帧号）, w = 采样模式（0=GGX, 1=均匀半球）
    float4 originVoxel0;  // 近层：xyz = 原点, w = 体素
    uint4  grid0;         // 近层分辨率
    float4 originVoxel1;  // 远层
    uint4  grid1;         // 远层分辨率
    float4 marchParams;   // x = 最大步数, y = eps, z = 最大距离, w = 未用
};

float2 SampleLayer(Texture3D<float> tex, SamplerState smp, float4 ov, uint3 res, float3 p) {
    if (ov.w <= 0.0) return float2(0.0, 0.0);
    const float3 local = (p - ov.xyz) / ov.w;
    const bool inside = all(local >= 0.0) && all(local <= float3(res));
    return float2(tex.SampleLevel(smp, (local + 0.5) / float3(res), 0), inside ? 1.0 : 0.0);
}

float FieldValue(float3 p, out bool insideAny) {
    const float2 a = SampleLayer(u_Field,  u_FieldSampler,  originVoxel0, grid0.xyz, p);
    const float2 b = SampleLayer(u_Field1, u_FieldSampler1, originVoxel1, grid1.xyz, p);
    insideAny = (a.y > 0.5) || (b.y > 0.5);
    return min(a.y > 0.5 ? a.x : 1e30, b.y > 0.5 ? b.x : 1e30);
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint total = dims.x * dims.y;      // 探针数 × 光线数
    if (tid.x >= total) return;

    const uint probeIndex = tid.x / dims.y;
    const uint rayIndex   = tid.x % dims.y;

    const ScreenProbe probe = u_Probes[probeIndex];
    if (probe.position.w < 0.5) { u_RayResult[tid.x] = float4(0.0, 0.0, 0.0, 0.0); return; }
    const float3 ro = probe.position.xyz;
    const float3 n  = normalize(probe.normal.xyz);
    const float  roughness = clamp(probe.normal.w, 0.05, 1.0);   // w 里放粗糙度（步骤 20 先填了 0 ⇒ 用下限）

    // 每帧换种子（dims.z = 帧号）：时序累积才有意义；同一帧内完全可复现。
    // 【随机流的编号必须用"探针身份"而不是"槽位下标"】布置 pass 用 `InterlockedAdd` 分配槽位，
    // 线程完成顺序不同 ⇒ 同一个探针两次运行可能落在不同 slot。若随机流跟着 slot 走，那么
    // "位置 ↔ 方向"的配对每次运行都会变，命中数/辐照度随之逐位不同（实测背靠背 98.6% 像素不同）。
    // 用探针身份（tileIndex×4 + 单元号）编号后，随机流与槽位解耦 ⇒ 结果可复现。
    const uint  probeKey = probe.meta.y * 4u + probe.meta.w;
    const float2 u = ProbeRayRandom(probeKey * dims.y + rayIndex, dims.z);
    // 采样模式：w = 0 → GGX（步骤 21 原样）；w = 1 → 均匀半球（步骤 23 起的默认，SH 0 阶项有解析值）
    const float3 rd = (dims.w == 0u) ? SampleGgxDirection(n, roughness, u)
                                     : SampleUniformHemisphere(n, u);
    const float  nd = dot(rd, n);

    const uint maxSteps = (uint)marchParams.x;
    const float eps     = marchParams.y;
    const float maxDist = marchParams.z;

    float t = 0.0;
    bool  hit = false;
    bool  advanced = false;
    for (uint s = 0; s < maxSteps; ++s) {
        const float3 p = ro + rd * t;
        bool inside = false;
        const float d = FieldValue(p, inside);
        if (!inside) break;
        if (advanced && d < eps) { hit = true; break; }
        t += max(d * 0.25, eps * 0.02);
        advanced = true;
        if (t > maxDist) break;
    }

    u_RayResult[tid.x] = float4(t, hit ? 1.0 : 0.0, nd, 0.0);
    u_RayHitPos[tid.x] = float4(ro + rd * t, hit ? 1.0 : 0.0);   // 命中点位置（未命中时无意义）
    InterlockedAdd(u_Stats[2], 1u);
    if (nd > 0.0) InterlockedAdd(u_Stats[3], 1u);   // 半球性检查：采样必须全部落在法线半球内
    if (hit) InterlockedAdd(u_Stats[0], 1u); else InterlockedAdd(u_Stats[1], 1u);
}
