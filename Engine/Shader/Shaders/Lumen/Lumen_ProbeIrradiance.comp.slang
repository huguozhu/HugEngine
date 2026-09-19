// Lumen_ProbeIrradiance.comp.slang — 由探针 SH 采样出逐像素入射辐照度（步骤 24）
//
// 【做什么】每个屏幕像素：
//   1. 查它所在的 16×16 单元对应的探针（步骤 20 顺手写出的单元→探针映射，见 `ScreenProbe_Gather`）；
//   2. 用该探针的 4 系数 SH 沿**该像素自己的 GBuffer 法线**重建入射辐照度 E(n)
//      （这就把"探针的低频辐照度"插值到了每个像素上 —— 比直接沿探针法线取值细腻得多）；
//   3. 兰伯特出射：`L_o = albedo/π · E(n)`，写进屏幕尺寸的辐照度纹理，交给 Lumen 的输出 pass 贴到
//      Provider 的输出上（Lighting 再采样它）。
//
// 【为什么按像素法线重建而不是直接用探针的 E】SH 是**方向函数**：同一个探针的 4 个系数可以重建出
// 任意方向的辐照度。用像素法线求值等于免费拿到了"法线插值"，代价只是每像素 4 次乘加。
// 首版不做空间插值（探针间双线性）—— 那属于 §10 的降噪/滤波框架。
//
// 【通道不能弄混】SH 系数是"逐通道各 4 个"：`shR = (l0,l1,l2,l3)` 属于 R 通道。
// 重建时必须逐通道各取自己的 4 个系数，把 R 的线性项用到 G/B 上会让画面偏色。
#include "Lumen/ScreenProbe.slang"
#include "Lumen/ScreenProbeSampling.slang"

[[vk::binding(0, 0)]] StructuredBuffer<ScreenProbe> u_Probes;
[[vk::binding(1, 0)]] StructuredBuffer<uint>        u_CellProbe;      // 16×16 单元 → 探针下标（0xFFFFFFFF = 无）
[[vk::binding(2, 0)]] Texture2D<float4>             u_GBufferNormal;  // xyz = n*0.5+0.5
[[vk::binding(3, 0)]] Texture2D<float4>             u_GBufferAlbedo;
[[vk::binding(4, 0)]] Texture2D<float4>             u_GBufferWorldPos;
[[vk::binding(5, 0)]] RWTexture2D<float4>           u_Irradiance;     // RGBA16F：xyz = 出射辐亮度, w = 有效
// 统计（避免回读整张纹理）：0 = 有效像素数, 1 = 亮度定点累加（*1024）, 2 = 亮度定点最大值（*65536）
// 【为什么累加只用 1024 倍】2073600 个像素 × 亮度 0.2 × 65536 ≈ 2.7e10，早已超过 u32 上限 ——
// 第一版就是这么写的，和值回绕后平均亮度读成了 0.0018（真值约 0.2）。1024 倍下和约 4e8，安全。
[[vk::binding(6, 0)]] RWStructuredBuffer<uint>      u_Stats;

[[vk::push_constant]] cbuffer IrrPC {
    uint4  dims;    // xy = 屏幕尺寸, z = cellsX, w = 未用
    float4 flags;  // x = 1 时只输出**辐照度**（不乘 albedo/π，调试用）, yzw 保留
};

float3 DecodeNormal(float4 n) { return normalize(n.xyz * 2.0 - 1.0); }

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= dims.x || tid.y >= dims.y) return;
    const uint2 px = tid.xy;

    const float4 wp = u_GBufferWorldPos.Load(int3((int2)px, 0));
    if (all(abs(wp.xyz) < 1e-4)) { u_Irradiance[px] = float4(0.0, 0.0, 0.0, 0.0); return; }

    const uint cell = (tid.y / 16u) * dims.z + (tid.x / 16u);
    const uint probeIndex = u_CellProbe[cell];
    if (probeIndex == 0xFFFFFFFFu) { u_Irradiance[px] = float4(0.0, 0.0, 0.0, 0.0); return; }

    const ScreenProbe probe = u_Probes[probeIndex];
    if (probe.position.w < 0.5) { u_Irradiance[px] = float4(0.0, 0.0, 0.0, 0.0); return; }

    // 逐通道各取自己的 4 个系数（shR 的 xyzw = R 通道的 l0..l3）
    const float3 l0 = float3(probe.shR.x, probe.shG.x, probe.shB.x);
    const float3 l1 = float3(probe.shR.y, probe.shG.y, probe.shB.y);
    const float3 l2 = float3(probe.shR.z, probe.shG.z, probe.shB.z);
    const float3 l3 = float3(probe.shR.w, probe.shG.w, probe.shB.w);

    const float3 n = DecodeNormal(u_GBufferNormal.Load(int3((int2)px, 0)));
    const float3 eIrradiance = SHRadianceToIrradiance(l0, l1, l2, l3, n);

    const float3 albedo = u_GBufferAlbedo.Load(int3((int2)px, 0)).rgb;
    const float3 outRadiance = (flags.x > 0.5) ? eIrradiance : (albedo * eIrradiance / 3.14159265359);
    u_Irradiance[px] = float4(outRadiance, 1.0);

    // 统计用**辐照度**而不是出射辐亮度：它不随 albedo 变化，便于跨帧/跨配置对照
    const float lum = dot(eIrradiance, float3(0.2126, 0.7152, 0.0722));
    const uint  fixedSum = (uint)(lum * 1024.0 + 0.5);
    const uint  fixedMax = (uint)(lum * 65536.0 + 0.5);
    InterlockedAdd(u_Stats[0], 1u);
    InterlockedAdd(u_Stats[1], fixedSum);
    // 定点最大值：CAS 循环。**注意 HLSL/Slang 的参数顺序是 (dest, compare, value, out original)** ——
    // 第一版把 compare 与 value 写反了，于是永远只会把 dest 往小里改，读数恒为 0。
    uint prev = u_Stats[2];
    while (fixedMax > prev) {
        uint observed;
        InterlockedCompareExchange(u_Stats[2], prev, fixedMax, observed);
        if (observed == prev) break;
        prev = observed;
    }
}
