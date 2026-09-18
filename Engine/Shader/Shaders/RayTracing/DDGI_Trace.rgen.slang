// DDGI_Trace.rgen.slang — DDGI 探针射线的硬件光追 march（任务 17 / B4 · M5.2-A）
//
// 【为什么需要它】DDGI.comp.slang 的探针更新此前**根本不追踪**任何几何：它把采样位置
// 算出来（`samplePos = probePos + dir * stepDist`）之后，只用 `dir` 去采样 IBL 辐照度
// cubemap —— `samplePos` 从未参与运算，而 Fibonacci 方向对每个探针都一样。结果是**整片
// 探针场的 SH 逐位相同**，该源在画面上等价于「IBL 辐照度的方向函数」。实测指纹（任务 14 /
// `Tools/gi/ddgi_grid_check.ps1`）：格距 531 / 248 / 120（192 / 1408 / 9408 个探针）三种
// 拟合给出的画面贡献两两相差 **0.0015%** —— 探针位置没有进入结果。
//
// 本 pass 用硬件光追为每条探针射线求**真实辐射度**：命中 → ClosestHit 评估命中点出射
// 辐射度（复用 RT_GI.rchit，与 RTGI 同一套命中着色，保证两个源的量纲一致）；未命中 →
// 取 IBL 辐照度图按方向的值（与改造前 DDGI 的回退同源，即"天空方向的平均入射辐射度"）。
// 结果写进 SSBO，供 DDGI.comp 的 SH 投影读取。
//
// 【视角无关】射线从探针位置向世界空间方向发射，与相机无关 —— 与 DDGI 的世界空间语义一致。
//
// 【回退】设备不支持光追时本 pass 不注册，DDGI.comp 走原来的 IBL/RSM 路径（见 u_Flags.w）。
//
// set0 绑定与 RT_GI.rchit 的要求对齐（b4/b5/b6，见 RT_HitCommon.slang）：
//   b0=TLAS  b1=辐射度输出 SSBO  b4=场景材质纹理  b5=命中点光源 UBO  b6=三角形法线纹理
//   b7=本 pass 参数 cbuffer  b8=IBL 辐照度 cubemap  b9=线性采样器

#include "ShaderTypes.slang"   // RTRayEffectPushConstant
#include "RT_HitCommon.slang"  // 命中点材质/光源查询（rchit 用）

// === 绑定（set0）===
[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> g_RadianceOut;   // [probeIndex*numSamples + i]
[[vk::binding(7, 0)]] cbuffer DDGITraceParams {
    float4   u_GridOrigin;   // xyz=网格原点
    float4   u_GridSize;     // xyz=探针数量, w=格距
    float4   u_Params;       // x=每探针采样数, y=最大追踪距离, z=起始偏移系数, w=未使用
    float4   u_ProbeStep;    // x=探针步长占格距的比例（与 DDGI.comp 的 stepDist 同源）
};
[[vk::binding(8, 0)]] TextureCube<float4> u_IBLIrradiance;   // 未命中回退（按方向采样）
[[vk::binding(8, 0)]] SamplerState        u_LinearSampler;   // 与上面共用 binding（combined image sampler）

// === Push Constant（与 RT_GI.rchit 共享布局：它要读 lightCount）===
[[vk::push_constant]] RTRayEffectPushConstant g_PC;

// === Payload（与 RT_GI.rchit / RT_GI.rmiss 共享）===
struct GIPayload {
    float4 radianceT;   // rgb=辐射度, a=命中距离（-1=未命中）
};

// Fibonacci 球面方向：必须与 DDGI.comp.slang 的 FibonacciSphere 逐位一致，
// 否则本 pass 算出的方向与被替换掉的那条路径不是同一个采样点。
float3 FibonacciSphere(int i, int n) {
    float phi   = acos(1.0 - 2.0 * (float(i) + 0.5) / float(n));
    float theta = 3.14159265359 * (1.0 + sqrt(5.0)) * float(i);
    return float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
}

[shader("raygeneration")]
void main() {
    uint rayIndex = DispatchRaysIndex().x;

    uint  numSamples = uint(u_Params.x + 0.5);
    uint  gridX = uint(u_GridSize.x + 0.5);
    uint  gridY = uint(u_GridSize.y + 0.5);
    uint  gridZ = uint(u_GridSize.z + 0.5);
    float cellSize = u_GridSize.w;
    uint  probeCount = gridX * gridY * gridZ;
    if (numSamples == 0u) { g_RadianceOut[rayIndex] = float4(0, 0, 0, 0); return; }

    uint probeIndex = rayIndex / numSamples;
    uint s          = rayIndex % numSamples;
    if (probeIndex >= probeCount) { g_RadianceOut[rayIndex] = float4(0, 0, 0, 0); return; }

    // 探针世界位置：与 DDGI.comp 完全相同的索引分解与格距
    uint gx = probeIndex % gridX;
    uint gy = (probeIndex / gridX) % gridY;
    uint gz = probeIndex / (gridX * gridY);
    float3 probePos = u_GridOrigin.xyz + float3(gx, gy, gz) * cellSize;

    float3 dir = FibonacciSphere(int(s), int(numSamples));

    // 起始偏移：与 DDGI.comp 的 stepDist 同一约定（探针落在几何内部时避免自交）
    float  stepDist = cellSize * u_ProbeStep.x;
    RayDesc ray;
    ray.Origin    = probePos + dir * stepDist;
    ray.Direction = dir;
    ray.TMin      = 0.0;
    ray.TMax      = u_Params.y;

    GIPayload payload;
    payload.radianceT = float4(0, 0, 0, -1);
    TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    float3 radiance;
    if (payload.radianceT.a < 0.0) {
        // 未命中：取该方向的 IBL 辐照度图（改造前 DDGI 的回退路径，数值一致）。
        // 该图存的是 **E/π**（见 §3.4 与 IBL_Irradiance.frag 的归一），对均匀天空即等于
        // 该方向的入射**辐射度 L** —— 与本 pass 要累积的量纲一致。
        radiance = u_IBLIrradiance.SampleLevel(u_LinearSampler, dir, 0).rgb;
    } else {
        // 命中：ClosestHit 给出的命中点出射辐射度（与 RTGI 同一套评估）。
        //
        // 【为什么这里要除以 π】RT_HitCommon.slang 的 `EvaluateHitRadiance` 返回的是
        // `albedo × (环境 + Σ 光源强度·N·L)`，即 **albedo × 辐照度 E**；而漫反射出射
        // 辐射度是 `L_o = albedo × E/π`。探针的 SH 累积必须与"未命中"那条路径同量纲
        // （那里给的是 L），否则**同一个探针里命中样本比未命中样本亮 π 倍**，
        // SH 就是两个尺度的混合 —— 这正是本次改造最容易静默搞错的地方。
        // （RTGI 侧沿用同一函数且不除 π，属既有约定问题，§9.2-AA 的相邻项；本任务只保证
        //   探针内部两条路径一致。）
        radiance = payload.radianceT.rgb * (1.0 / 3.14159265);
    }
    g_RadianceOut[rayIndex] = float4(radiance, 1.0);
}
