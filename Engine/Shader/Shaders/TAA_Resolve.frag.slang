// TAA_Resolve.frag — TAA 时域抗锯齿 resolve 片段着色器
// 算法：重投影 → disocclusion 检测 → 邻域 AABB 裁剪 → 混合
#include "common.slang"

struct VSOutput {
    float4 position          : SV_Position;
    [[vk::location(0)]] float2 uv;
};

// ── 输入纹理（set=0） ──
[[vk::binding(0, 0)]] Texture2D<float4> u_CurrentColor;
[[vk::binding(0, 0)]] SamplerState      u_PointSampler;
[[vk::binding(1, 0)]] Texture2D<float4> u_HistoryColor;
[[vk::binding(2, 0)]] Texture2D<float>  u_Depth;
[[vk::binding(3, 0)]] Texture2D<float4> u_Normal;      // GBuffer B: normal*0.5+0.5
[[vk::binding(4, 0)]] Texture2D<float2> u_Velocity;    // GBuffer D: currUV - prevUV

// ── Uniform Buffer ──
struct TAAUniforms {
    float4x4 prevViewProj;
    float4x4 invCurrViewProj;
    float2   resolution;
    float    blendFactor;      // 基础混合系数 (0.05)
    float    unused;
};
[[vk::binding(5, 0)]] cbuffer u_TAA : register(b5) { TAAUniforms u_TAA; }

// ── Push Constants ──
[[vk::push_constant]] cbuffer TAA_PC {
    float2 jitterOffset;  // 当前帧子像素抖动（NDC 空间）
};

// ── YCoCg 颜色空间转换（用于邻域 AABB 裁剪） ──
float3 rgb_to_ycocg(float3 c) {
    float y  =  0.25 * c.r + 0.5  * c.g + 0.25 * c.b;
    float co =  0.5  * c.r               - 0.5  * c.b;
    float cg = -0.25 * c.r + 0.5  * c.g - 0.25 * c.b;
    return float3(y, co, cg);
}
float3 ycocg_to_rgb(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// ── 当前帧 3×3 邻域 YCoCg AABB ──
void current_color_aabb(float2 uv, out float3 aabbMin, out float3 aabbMax) {
    float2 ts = 1.0 / u_TAA.resolution;
    float3 c00 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2(-ts.x, -ts.y)).rgb);
    float3 c10 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2( 0.0,  -ts.y)).rgb);
    float3 c20 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2( ts.x, -ts.y)).rgb);
    float3 c01 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2(-ts.x,  0.0)).rgb);
    float3 c11 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv).rgb);
    float3 c21 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2( ts.x,  0.0)).rgb);
    float3 c02 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2(-ts.x,  ts.y)).rgb);
    float3 c12 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2( 0.0,   ts.y)).rgb);
    float3 c22 = rgb_to_ycocg(u_CurrentColor.Sample(u_PointSampler, uv + float2( ts.x,  ts.y)).rgb);

    aabbMin = min(min(min(c00,c10),min(c20,c01)),min(min(c11,c21),min(c02,c12)));
    aabbMin = min(aabbMin, c22);
    aabbMax = max(max(max(c00,c10),max(c20,c01)),max(max(c11,c21),max(c02,c12)));
    aabbMax = max(aabbMax, c22);

    // 膨胀 AABB 放宽裁剪范围，避免阻止 TAA 时域收敛
    float3 ext = (aabbMax - aabbMin) * 0.05 + 0.001;
    aabbMin -= ext;
    aabbMax += ext;
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;

    // 1. 当前帧颜色
    float3 currColor = u_CurrentColor.Sample(u_PointSampler, uv).rgb;

    // 2. Velocity → history UV
    float2 vel = u_Velocity.Sample(u_PointSampler, uv).rg;
    float2 historyUV = uv - vel;

    // 3. Disocclusion 检测
    float depth = u_Depth.Sample(u_PointSampler, uv).r;
    float3 N    = normalize(u_Normal.Sample(u_PointSampler, uv).rgb * 2.0 - 1.0);

    bool offScreen = historyUV.x < 0.0 || historyUV.x > 1.0 ||
                     historyUV.y < 0.0 || historyUV.y > 1.0;
    float disocclusion = 0.0;

    if (!offScreen) {
        float depthHist = u_Depth.Sample(u_PointSampler, historyUV).r;
        float3 N_hist   = normalize(u_Normal.Sample(u_PointSampler, historyUV).rgb * 2.0 - 1.0);

        // 深度差异
        float depthDiff = abs(depth - depthHist) / max(depth, 0.001);
        disocclusion = max(disocclusion, smoothstep(0.001, 0.01, depthDiff));

        // 法线差异
        float normalDiff = 1.0 - dot(N, N_hist);
        disocclusion = max(disocclusion, smoothstep(0.001, 0.05, normalDiff));

        // 速度过大
        float speed = length(vel);
        disocclusion = max(disocclusion, smoothstep(0.01, 0.05, speed));
    } else {
        disocclusion = 1.0;
    }

    // 4. 混合
    float4 output;
    if (disocclusion > 0.95 || offScreen) {
        output = float4(currColor, 1.0);
    } else {
        float3 histColor = u_HistoryColor.Sample(u_PointSampler, historyUV).rgb;

        // YCoCg 邻域 AABB 裁剪
        float3 aabbMin, aabbMax;
        current_color_aabb(uv, aabbMin, aabbMax);
        float3 histYC = rgb_to_ycocg(histColor);
        histYC = clamp(histYC, aabbMin, aabbMax);
        float3 clampedHist = ycocg_to_rgb(histYC);

        float blend = lerp(u_TAA.blendFactor, 0.3, disocclusion);
        float3 resolved = lerp(clampedHist, currColor, blend);
        output = float4(resolved, 1.0);
    }
    return output;
}
