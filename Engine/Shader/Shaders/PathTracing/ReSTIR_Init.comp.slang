// ReSTIR_Init.comp.slang — ReSTIR DI 初始采样（WRS 蓄水池构建）
//
// 对每个有效像素：
//   1. 从 PT 输出的深度重建世界坐标，读取法线/粗糙度
//   2. 随机采样 candidateCount 个候选光源 → 评估目标函数 p̂ = f/p_src
//   3. WRS（加权蓄水池采样）更新：以 p̂/Σp̂ 概率替换选中样本
//   4. 输出 InitialReservoir
//
// 目标函数使用 PT 输出的真实 albedo/metallic（第 5 UAV），
// 使 WRS 的 p̂ 与最终着色权重一致 → 降低有色物体的直接光噪声。

#include "PT_Common.slang"  // Rand/RandInt/SampleLight/ReconstructWorldPos

// === set=0 资源 ===
[[vk::binding(0, 0)]] Texture2D<float>  g_Depth;    // ptDepth：线性视图深度（miss=-1000）
[[vk::binding(1, 0)]] Texture2D<float4> g_Normal;   // ptNormal：世界法线(0~1) + roughness.a
[[vk::binding(2, 0)]] StructuredBuffer<GPULight> g_Lights;   // 光源 SSBO
[[vk::binding(3, 0)]] RWStructuredBuffer<PTReservoir> g_Initial;  // InitialReservoir
[[vk::binding(4, 0)]] Texture2D<float4> g_AlbedoMetallic;  // PT 第 5 UAV: albedo(rgb)+metallic(a)
[[vk::binding(5, 0)]] Texture3D<float4> g_BlueNoise;       // STBN 时空蓝噪声（Load 采样，Rand/RandInt 用）

// === Push Constant（128B，见 ShaderTypes.slang）===
[[vk::push_constant]] ReSTIRPushConstant g_PC;

// 引擎最大光源数（kGPUMaxLights）
#define RESTIR_MAX_LIGHTS 8u

// 空蓄水池（miss 像素 / 无有效候选）
PTReservoir EmptyReservoir() {
    PTReservoir r;
    r.lightIndex = 0xFFFFFFFFu;
    r.weightSum  = 0.0;
    r.M          = 0u;
    r.W          = 0.0;
    r.lightPos   = float4(0, 0, 0, 0);
    return r;
}

// 目标函数：p̂ = luminance(brdf·Li) / p_src（含真实 albedo/metallic）
float EvaluateTarget(float3 P, float3 N, float3 V, float roughness,
                     float3 albedo, float metallic,
                     uint lightIndex, out float3 lightPos) {
    uint li = min(lightIndex, g_PC.lightCount > 0 ? g_PC.lightCount - 1u : 0u);
    float3 L; float3 Li; float lightPdf;
    if (!SampleLight(g_Lights[li], P, lightPos, L, Li, lightPdf)) return 0.0;
    if (dot(N, L) <= 0.0) return 0.0;
    float3 f = PBR_BRDF(albedo, metallic, roughness, N, V, L) * Li;
    return Luminance(f) / max(lightPdf, 1e-6);
}

[numthreads(8, 8, 1)]
void main(uint3 idx : SV_DispatchThreadID) {
    uint2 p = idx.xy;
    if (p.x >= g_PC.dispatchDimX || p.y >= g_PC.dispatchDimY) return;

    float depth = g_Depth.Load(int3(p, 0)).r;
    if (depth >= -0.01) {
        // miss 像素（哨兵 -1000）→ 空蓄水池
        g_Initial[p.y * g_PC.dispatchDimX + p.x] = EmptyReservoir();
        return;
    }

    // ── 重建世界坐标 + 材质数据 ──
    float2 uv = (float2(p) + 0.5) / float2(g_PC.dispatchDimX, g_PC.dispatchDimY);
    float3 worldPos = ReconstructWorldPos(uv, -depth, g_PC.invViewProj, g_PC.cameraPos.xyz);
    float4 nb = g_Normal.Load(int3(p, 0));
    float3 N = normalize(nb.xyz * 2.0 - 1.0);
    float  roughness = nb.a;
    float3 V = normalize(g_PC.cameraPos.xyz - worldPos);

    // 读取 PT 第 5 UAV：真实 albedo(rgb) + metallic(a)
    float4 am = g_AlbedoMetallic.Load(int3(p, 0));
    float3 albedo = am.rgb;
    float  metallic = am.a;

    // ── WRS 初始采样 ──
    PTReservoir r = EmptyReservoir();
    uint candidates = max(1u, min(g_PC.candidateCount, 64u));
    uint lightCount = min(g_PC.lightCount, RESTIR_MAX_LIGHTS);
    if (lightCount == 0u) {
        g_Initial[p.y * g_PC.dispatchDimX + p.x] = r;
        return;
    }

    for (uint i = 0; i < candidates; i++) {
        uint li = RandInt(p, g_PC.frameIndex, i, lightCount);
        float3 lightPos, L; float3 Li; float lightPdf;
        if (!SampleLight(g_Lights[li], worldPos, lightPos, L, Li, lightPdf)) continue;
        if (dot(N, L) <= 0.0) continue;

        float3 f = PBR_BRDF(albedo, metallic, roughness, N, V, L) * Li;
        float pHat = Luminance(f) / max(lightPdf, 1e-6);
        if (pHat <= 0.0) continue;

        r.weightSum += pHat;
        r.M++;
        // WRS：以 p̂/weightSum 概率替换当前选中样本
        if (Rand(p, g_PC.frameIndex, i + 1000u) * r.weightSum < pHat) {
            r.lightIndex = li;
            r.W = pHat;
            r.lightPos = float4(lightPos, 0.0);
        }
    }

    g_Initial[p.y * g_PC.dispatchDimX + p.x] = r;
}
