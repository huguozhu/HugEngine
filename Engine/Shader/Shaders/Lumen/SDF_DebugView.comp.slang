// SDF_DebugView.comp.slang — SDF 追踪结果可视化（步骤 12，L1 退出判据）
//
// 与 `SDF_RayMarch.comp.slang` 的关系：那个是**自检**（256 条测试射线，结果回读到 CPU 逐条比对），
// 本 shader 是**可视化**（逐像素主射线，结果写成一张 RGBA16F 图像）。两者共用同一份步进语义
// （两层取 min、`max(d*0.25, eps*0.02)` 安全步长、`advanced && d < eps` 命中判据），
// 这样"看得见的画面"与"算得出的指标"不会各说各话。
//
// 【为什么用相机基向量而不是逆 VP 矩阵】push constant 里放 4×4 矩阵要额外小心 slang 与 C++
// 的对齐；直接传 forward/right/up + tan(半视场角) + aspect，逐像素只需要两次乘加，
// 且与引擎 `CameraData` 的约定一一对应，不存在"矩阵转置搞反"这类静默错误。
//
// 输出编码（4 通道都带信息，便于 CPU 侧统计与离线着色）：
//   R = 命中层（0 = 层0，0.5 = 层1）；未命中写 1.0
//   G = 命中距离 t / 最大距离（未命中为 0）
//   B = 步数 / 最大步数（未命中为 0）
//   A = 是否命中（1 / 0）
// 统计走 `u_Stats`（原子计数），供 CPU 侧打印"命中率 / 分层命中 / 平均步数"，
// 使可视化本身也能进回归基线（不依赖看图）。

[[vk::binding(0, 0)]] Texture3D<float>      u_Field;
[[vk::binding(0, 0)]] SamplerState          u_FieldSampler;
[[vk::binding(6, 0)]] Texture3D<float>      u_Field1;
[[vk::binding(6, 0)]] SamplerState          u_FieldSampler1;
[[vk::binding(7, 0)]] RWTexture2D<float4>   u_Out;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> u_Stats;   // 0=近层命中 1=远层命中 2=未命中 3=总步数

[[vk::push_constant]] cbuffer DebugPC {
    float4 camPos;        // xyz = 相机世界坐标, w = tan(垂直半视场角)
    float4 camForward;    // xyz = 前向, w = aspect
    float4 camRight;      // xyz = 右向, w = 未用
    float4 camUp;         // xyz = 上向, w = 未用
    uint4  grid0;         // xyz = 层0 分辨率, w = 输出宽
    float4 originVoxel0;  // xyz = 层0 原点, w = 体素边长
    uint4  grid1;         // xyz = 层1 分辨率, w = 输出高
    float4 originVoxel1;  // xyz = 层1 原点, w = 体素边长
    float4 marchParams;   // x = 最大步数, y = eps, z = 最大距离, w = 未用
};

/// 层内采样：返回 (值, 是否在层内)。与 SDF_RayMarch.comp.slang 同源，保证两处语义一致。
float2 SampleLayer(Texture3D<float> tex, SamplerState smp, float4 ov, uint3 res, float3 p) {
    if (ov.w <= 0.0) return float2(0.0, 0.0);   // 该层不存在（只开一层）：直接判"不在域内"
    const float3 local = (p - ov.xyz) / ov.w;
    const bool inside = all(local >= 0.0) && all(local <= float3(res));
    const float v = tex.SampleLevel(smp, (local + 0.5) / float3(res), 0);
    return float2(v, inside ? 1.0 : 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint w = grid0.w;
    const uint h = grid1.w;
    if (tid.x >= w || tid.y >= h) return;

    // ── 逐像素主射线 ──
    const float2 ndc = float2(2.0 * ((float)tid.x + 0.5) / (float)w - 1.0,
                              2.0 * ((float)tid.y + 0.5) / (float)h - 1.0);
    const float tanHalf = camPos.w;
    const float aspect  = camForward.w;
    const float3 rd = normalize(camForward.xyz
                              + camRight.xyz * (ndc.x * tanHalf * aspect)
                              - camUp.xyz    * (ndc.y * tanHalf));   // 图像 y 向下 → 取负，使"上"朝上

    const float3 ro = camPos.xyz;
    const uint   maxSteps = (uint)marchParams.x;
    const float  eps      = marchParams.y;
    const float  maxDist  = marchParams.z;

    float t = 0.0;
    bool  hit = false;
    bool  advanced = false;
    uint  steps = 0;
    float hitLayer = 0.0;

    for (uint s = 0; s < maxSteps; ++s) {
        steps = s + 1;
        const float3 p = ro + rd * t;
        const float2 a = SampleLayer(u_Field,  u_FieldSampler,  originVoxel0, grid0.xyz, p);
        const float2 b = SampleLayer(u_Field1, u_FieldSampler1, originVoxel1, grid1.xyz, p);
        if (a.y < 0.5 && b.y < 0.5) break;                      // 出所有层：按未命中处理
        const float d = min(a.y > 0.5 ? a.x : 1e30, b.y > 0.5 ? b.x : 1e30);
        if (advanced && d < eps) { hit = true; hitLayer = (a.y > 0.5 && a.x <= d) ? 0.0 : 0.5; break; }
        t += max(d * 0.25, eps * 0.02);
        advanced = true;
        if (t > maxDist) break;
    }

    u_Out[tid.xy] = hit
        ? float4(hitLayer, saturate(t / max(1e-6, maxDist)), (float)steps / max(1.0, marchParams.x), 1.0)
        : float4(1.0, 0.0, 0.0, 0.0);

    // 原子统计：让"看得见的画面"同时给出可回归的数字
    if (hit) {
        InterlockedAdd(u_Stats[hitLayer < 0.25 ? 0u : 1u], 1u);
    } else {
        InterlockedAdd(u_Stats[2], 1u);
    }
    InterlockedAdd(u_Stats[3], steps);
}
