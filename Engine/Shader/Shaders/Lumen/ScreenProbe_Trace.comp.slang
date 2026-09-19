// ScreenProbe_Trace.comp.slang — 探针半球追踪（步骤 21）
//
// 【做什么】每个探针按 `traceRep` 条 **GGX 重要性采样**方向发射光线，用步骤 11 的同一套 sphere tracing
// 语义（两层全局场取 min、`max(d*0.25, eps*0.02)` 安全步长、`advanced && d < eps` 命中）求交，
// 把 (命中距离 t, 是否命中, dot(dir, n)) 写进结果缓冲。着色留给步骤 22（消费 L2 的 atlas）。
//
// 【为什么 V 取探针法线】探针没有"视线"概念，用 `V = N` 做 GGX 半向量采样得到的分布天然以法线为中心，
// 正是"漫反射探针 + 粗糙度"该有的形态；步骤 22/23 会把它按 cos/BRDF 权重投影成 SH。
#include "Lumen/ScreenProbe.slang"

[[vk::binding(0, 0)]] Texture3D<float> u_Field;        // 近层
[[vk::binding(0, 0)]] SamplerState     u_FieldSampler;
[[vk::binding(6, 0)]] Texture3D<float> u_Field1;       // 远层
[[vk::binding(6, 0)]] SamplerState     u_FieldSampler1;
[[vk::binding(1, 0)]] StructuredBuffer<ScreenProbe> u_Probes;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4>    u_RayResult;   // 每 (探针, 光线) 一条：x=t, y=命中, z=dot(dir,n), w=未用
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>      u_Stats;       // 0=命中, 1=未命中, 2=总光线, 3=半球内(点积>0)光线

[[vk::push_constant]] cbuffer TracePC {
    uint4  dims;          // x = 探针数, y = 每探针光线数, z = 随机种子, w = 未用
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

// 确定性的低差异随机（PCG 变体）：同一 (探针, 光线, 帧) 恒得同一方向 ⇒ 可复现（背靠背读数一致的前提）
uint Pcg(uint v) {
    v = v * 747796405u + 2891336453u;
    const uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}
float U01(uint seed) { return (float)(Pcg(seed) & 0x00FFFFFFu) / 16777216.0; }

// 以 n 为 Z 轴的 GGX 半向量采样，再镜面反射得到出射方向（V = n）
float3 SampleGgxDirection(float3 n, float roughness, float2 u) {
    const float a = max(0.002, roughness * roughness);
    const float phi = 6.28318530718 * u.x;
    const float cosTheta = sqrt((1.0 - u.y) / max(1e-6, 1.0 + (a * a - 1.0) * u.y));
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float3 t = normalize(abs(n.z) < 0.999 ? cross(float3(0.0, 0.0, 1.0), n) : cross(float3(1.0, 0.0, 0.0), n));
    const float3 b = cross(n, t);
    const float3 h = t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + n * cosTheta;
    return normalize(reflect(-n, h));
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

    // 每帧换种子（dims.z = 帧号）：时序累积才有意义；同一帧内完全可复现
    const float2 u = float2(U01(tid.x * 3u + dims.z * 7919u + 1u),
                            U01(tid.x * 7u + dims.z * 6271u + 5u));
    const float3 rd = SampleGgxDirection(n, roughness, u);
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
    InterlockedAdd(u_Stats[2], 1u);
    if (nd > 0.0) InterlockedAdd(u_Stats[3], 1u);   // 半球性检查：GGX 采样必须全部落在法线半球内
    if (hit) InterlockedAdd(u_Stats[0], 1u); else InterlockedAdd(u_Stats[1], 1u);
}
