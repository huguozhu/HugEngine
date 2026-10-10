// ============================================================
// RT_Common.slang — RT pass 共用的小工具（此前 4 个 rgen 各抄一份）
//
// 提供：Hash / RandHash（哈希随机流）、ReconstructWorldPosFromIndex（深度 → 世界坐标）
// 另 include Sampling.slang，故使用方同时拿到 CosineSampleHemisphere。
//
// 【包含者必须声明】本文件的 `ReconstructWorldPosFromIndex` 依赖一个 **push constant `g_PC`**，
// 它需要含 `dispatchDimX` / `dispatchDimY` / `invViewProj` 字段（`RTRayEffectPushConstant`
// 或 `RTShadowPushConstant` 都满足）。因此 **include 本文件要放在 `g_PC` 声明之后** ——
// 与 `RT_DDGI.slang`（要求先声明网格参数 cbuffer）是同一约定。
//
// 【与 PT 的区别】`PT_Common.slang` 另有一个**参数化**的 `ReconstructWorldPosFromIndex(uv, depth,
// invViewProj, cameraPos)`：接口不同（不依赖 g_PC）、且它对近平面做了相机方向校正。
// 两者语义相近但不逐字等价，**合并需要先定语义**（见待办 P4），本文件只负责消除 RT 四份。
// ============================================================
#ifndef HUGENGINE_RT_COMMON_SLANG
#define HUGENGINE_RT_COMMON_SLANG

#include "Sampling.slang"


uint Hash(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x ^= x >> 4u;
    x *= 0x27d4eb2du;
    x ^= x >> 15u;
    return x;
}


float RandHash(uint2 idx, uint frame, uint s) {
    uint seed = idx.x * 1973u + idx.y * 9277u + frame * 26699u + s * 479u;
    return float(Hash(seed) & 0xFFFFu) / 65536.0;
}

// 深度 → 世界坐标：用 push constant 的派发尺寸还原 uv，再做 
// Vulkan NDC y 翻转（与全项目约定一致）。
// 从深度和 UV 重建世界坐标（Vulkan NDC y 翻转）
float3 ReconstructWorldPosFromIndex(uint2 idx, float depth) {
    float2 uv = (float2(idx) + 0.5) / float2(g_PC.dispatchDimX, g_PC.dispatchDimY);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = mul(g_PC.invViewProj, clipPos);
    return worldPos4.xyz / worldPos4.w;
}

#endif // HUGENGINE_RT_COMMON_SLANG
