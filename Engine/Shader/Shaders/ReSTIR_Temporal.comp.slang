// ReSTIR_Temporal.comp.slang — ReSTIR DI 时域复用
//
//   1. 用速度矢量把当前像素重投影到上一帧 → 读取历史蓄水池
//   2. 去遮挡验证（深度差 + 法线 dot，flags bit2=historyValid）
//   3. 对当前像素重新评估历史样本的目标函数 → WRS 合并
//   4. 写 TemporalReservoir + 当前帧 depth/normal 到下一帧历史槽位
//
// 历史槽位双缓冲（CPU 帧末交换）：本帧读 slot A、写 slot B。

#include "PT_Common.slang"  // Rand/SampleLight/ReconstructWorldPos/TransformWorldToUV

// === set=0 资源 ===
[[vk::binding(0, 0)]] Texture2D<float2> g_Velocity;    // ptVelocity：屏幕速度（currUV - prevUV）
[[vk::binding(1, 0)]] Texture2D<float>  g_Depth;       // ptDepth：线性视图深度
[[vk::binding(2, 0)]] Texture2D<float4> g_Normal;      // ptNormal：世界法线(0~1) + roughness.a
[[vk::binding(3, 0)]] StructuredBuffer<PTReservoir> g_Initial;       // InitialReservoir（本帧）
[[vk::binding(4, 0)]] StructuredBuffer<PTReservoir> g_History;       // 历史蓄水池（上帧）
[[vk::binding(5, 0)]] Texture2D<float>  g_HistDepth;   // 历史深度（上帧）
[[vk::binding(6, 0)]] Texture2D<float4> g_HistNormal;  // 历史法线（上帧）
[[vk::binding(7, 0)]] RWStructuredBuffer<PTReservoir> g_Temporal;    // TemporalReservoir（本帧）
[[vk::binding(8, 0)]] RWTexture2D<float>  g_CurDepth;  // 下一帧历史深度槽（= 本帧 ptDepth 拷贝）
[[vk::binding(9, 0)]] RWTexture2D<float4> g_CurNormal; // 下一帧历史法线槽（= 本帧 ptNormal 拷贝）
[[vk::binding(10, 0)]] StructuredBuffer<GPULight> g_Lights;   // 光源 SSBO

// === Push Constant ===
[[vk::push_constant]] ReSTIRPushConstant g_PC;

PTReservoir EmptyReservoir() {
    PTReservoir r;
    r.lightIndex = 0xFFFFFFFFu;
    r.weightSum  = 0.0;
    r.M          = 0u;
    r.W          = 0.0;
    r.lightPos   = float4(0, 0, 0, 0);
    return r;
}

// 目标函数（与 Init 一致）：p̂ = luminance(brdf·Li) / p_src
float EvaluateTarget(float3 P, float3 N, float3 V, float roughness,
                     uint lightIndex, out float3 lightPos) {
    uint li = min(lightIndex, g_PC.lightCount > 0 ? g_PC.lightCount - 1 : 0u);  // 越界防护
    float3 L; float3 Li; float lightPdf;
    if (!SampleLight(g_Lights[li], P, lightPos, L, Li, lightPdf)) return 0.0;
    if (dot(N, L) <= 0.0) return 0.0;
    float3 f = PBR_BRDF(float3(1, 1, 1), 0.0, roughness, N, V, L) * Li;
    return Luminance(f) / max(lightPdf, 1e-6);
}

[numthreads(8, 8, 1)]
void main(uint3 idx : SV_DispatchThreadID) {
    uint2 p = idx.xy;
    if (p.x >= g_PC.dispatchDimX || p.y >= g_PC.dispatchDimY) return;
    uint lin = p.y * g_PC.dispatchDimX + p.x;

    PTReservoir current = g_Initial[lin];

    // 先写下一帧历史槽（本帧 depth/normal 拷贝）
    g_CurDepth[p]  = g_Depth.Load(int3(p, 0)).r;
    g_CurNormal[p] = g_Normal.Load(int3(p, 0));

    // miss 像素（depth=-1000 哨兵）→ 直出（历史对天空像素无意义）
    float depth = g_Depth.Load(int3(p, 0)).r;
    if (depth >= -0.01 || current.M == 0u) {
        g_Temporal[lin] = current;
        return;
    }

    // ── 速度重投影 → 历史 UV ──
    float2 velocity = g_Velocity.Load(int3(p, 0));
    float2 uv = (float2(p) + 0.5) / float2(g_PC.dispatchDimX, g_PC.dispatchDimY);
    float2 historyUV = uv - velocity;   // prevUV = currUV - velocity
    int2 histIdx = int2(historyUV * float2(g_PC.dispatchDimX, g_PC.dispatchDimY));

    // 屏幕外 → 无历史可用
    if (histIdx.x < 0 || histIdx.x >= int(g_PC.dispatchDimX) ||
        histIdx.y < 0 || histIdx.y >= int(g_PC.dispatchDimY)) {
        g_Temporal[lin] = current;
        return;
    }

    // ── 重建当前像素几何（验证 + 目标函数共用）──
    float2 uv2 = (float2(p) + 0.5) / float2(g_PC.dispatchDimX, g_PC.dispatchDimY);
    float3 worldPos = ReconstructWorldPos(uv2, -depth, g_PC.invViewProj, g_PC.cameraPos.xyz);
    float4 nb = g_Normal.Load(int3(p, 0));
    float3 N = normalize(nb.xyz * 2.0 - 1.0);
    float  roughness = nb.a;
    float3 V = normalize(g_PC.cameraPos.xyz - worldPos);

    // ── 去遮挡验证（与 RTDenoiser 同款：当前帧深度/法线在历史 UV 处采样比较）──
    float histDepth = g_HistDepth.Load(int3(histIdx, 0)).r;
    float3 histN    = normalize(g_HistNormal.Load(int3(histIdx, 0)).xyz * 2.0 - 1.0);
    bool valid = (g_PC.flags & 4u) != 0u                  // bit2=historyValid
        && abs(depth - histDepth) < g_PC.maxDistance
        && dot(N, histN) > 0.9;
    if (!valid) {
        g_Temporal[lin] = current;
        return;
    }

    // ── 读取历史蓄水池 + 对当前像素重评估目标函数 ──
    uint histLin = uint(histIdx.y) * g_PC.dispatchDimX + uint(histIdx.x);
    PTReservoir history = g_History[histLin];
    if (history.M == 0u) {
        g_Temporal[lin] = current;
        return;
    }
    float3 histLightPos;
    float pHatHist = EvaluateTarget(worldPos, N, V, roughness, history.lightIndex, histLightPos);
    if (pHatHist <= 0.0) {
        g_Temporal[lin] = current;
        return;
    }

    // ── WRS 合并（current + history）──
    float totalW = current.weightSum + pHatHist;
    uint  totalM = current.M + history.M;
    PTReservoir r;
    if (Rand(p, g_PC.frameIndex, 7u) * totalW < pHatHist) {
        // 选中历史样本
        r.lightIndex = history.lightIndex;
        r.lightPos   = history.lightPos;
        r.W          = pHatHist;
    } else {
        // 保留当前样本（W 已在 Init 对本像素评估）
        r.lightIndex = current.lightIndex;
        r.lightPos   = current.lightPos;
        r.W          = current.W;
    }
    r.weightSum = totalW;
    r.M         = totalM;

    g_Temporal[lin] = r;
}
