// GTAO.frag.slang — Ground Truth Ambient Occlusion（地平线切片 + 解析积分）
//
// 与 SSAO 的区别：
//   SSAO   在半球内随机撒点，判断"是否被遮挡"（0/1），再平均 → 噪点多、边缘偏软
//   GTAO   沿方位角切片搜索"地平线仰角"，再用解析式积分 → 同采样数下更准、无噪点
//
// 算法（Jimenez et al. 2016 "Practical Realtime Strategies for Accurate IBL"）：
//   1. 每像素取 SLICE_COUNT 个方位角方向（配合噪声抖动，避免条带）
//   2. 每个方向在切片平面内沿 ±t 步进，屏幕空间采样深度并重建几何位置，
//      求该点的"仰角余弦" cos(γ) = dot(S - P, N) / |S - P|，取两侧最大者作为地平线
//   3. 用地平线角解析积分得到该切片的可见性（含余弦权重与视线夹角）
//   4. 所有切片平均 → 可见性 → AO
//
// 输入：深度 + 法线（GBuffer）；输出：单通道 AO

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float>  u_Depth;
[[vk::binding(1, 0)]] Texture2D<float4> u_Normal;
[[vk::binding(2, 0)]] Texture2D<float4> u_Noise;
[[vk::binding(0, 0)]] SamplerState      u_PointSampler;

// Uniform buffer：参数 + 正/逆投影矩阵（与 SSAO 布局一致，便于共用 C++ 侧写法）
//   u_Params.x = radius（世界尺度，按视距缩放）
//   u_Params.y = bias（深度偏移）
//   u_Params.z = intensity（强度指数）
//   u_Params.w = sliceCount（方位角切片数）
[[vk::binding(3, 0)]] cbuffer GTAOParams {
    float4   u_Samples[64];
    float4   u_Params;
    float4x4 u_InvProj;
    float4x4 u_Proj;
};

#define GTAO_PI 3.14159265359

// 由屏幕 uv + 深度重建 view-space 位置
float3 ReconstructViewPos(float2 uv, float depth) {
    float4 clipP = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewP4 = mul(u_InvProj, clipP);
    return viewP4.xyz / viewP4.w;
}

// 由 view-space 位置投影到屏幕 uv（越界返回 false）
bool ProjectToUV(float3 viewPos, out float2 uv) {
    float4 clipS = mul(u_Proj, float4(viewPos, 1.0));
    if (clipS.w <= 0.0001) return false;
    float3 ndcS = clipS.xyz / clipS.w;
    uv = ndcS.xy * 0.5 + 0.5;
    return (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0);
}

// 地平线角解析积分（Jimenez 2016）
//   h1/h2 = 两侧地平线的仰角余弦（-1 表示该侧无遮挡）
//   n     = 视线方向与法线夹角的余弦
// 返回该切片贡献的"被遮蔽量"（0 = 完全开阔）
float IntegrateArc(float h1, float h2, float n) {
    float h1n = (-cos(2.0 * h1 - n) + cos(n) + 2.0 * h1 * sin(n)) * 0.25;
    float h2n = (-cos(2.0 * h2 - n) + cos(n) + 2.0 * h2 * sin(n)) * 0.25;
    return abs(h1 - h2) * (h1n + h2n);
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    const float2 uv = input.uv;
    const float depth = u_Depth.Sample(u_PointSampler, uv).r;
    if (depth >= 1.0) return float4(1.0, 1.0, 1.0, 1.0);   // 天空/背景：不遮蔽

    const float3 N = normalize(u_Normal.Sample(u_PointSampler, uv).rgb * 2.0 - 1.0);
    const float3 P = ReconstructViewPos(uv, depth);

    const float radius    = u_Params.x;
    const float bias      = u_Params.y;
    const float intensity = u_Params.z;
    const int   slices    = max(int(u_Params.w), 1);

    // 视距缩放：远处像素在屏幕上覆盖更大世界范围，半径需按视距放大
    const float viewDist     = max(abs(P.z), 0.1);
    const float scaledRadius = radius * viewDist * 0.5;

    // 视线方向与法线夹角余弦（用于积分中的余弦权重；clamp 避免退化）
    const float cosN = clamp(dot(normalize(-P), N), 0.0, 1.0);

    // 方位角抖动（时间/空间噪声，抑制切片条带）
    const float rand = u_Noise.Sample(u_PointSampler, uv * float2(320.0, 180.0)).r;

    const int kSteps = 6;   // 每侧步数
    float occlusion = 0.0;

    for (int s = 0; s < slices; s++) {
        const float phi = (float(s) + rand) * GTAO_PI / float(slices);
        const float3 sliceDir = float3(cos(phi), sin(phi), 0.0);

        // 切片平面内的切向（垂直于法线）
        float3 T = sliceDir - N * dot(sliceDir, N);
        const float tlen = length(T);
        if (tlen < 1e-5) continue;      // 退化方向（法线几乎平行切片）→ 跳过
        T /= tlen;

        // 两侧地平线搜索
        float cosHorizon[2] = { -1.0, -1.0 };
        for (int side = 0; side < 2; side++) {
            const float sgn = (side == 0) ? 1.0 : -1.0;
            for (int step = 1; step <= kSteps; step++) {
                // 步长按步数递增（近处密、远处疏）
                const float t = sgn * (float(step) / float(kSteps)) * scaledRadius;
                const float3 sp = P + T * t;

                float2 suv;
                if (!ProjectToUV(sp, suv)) break;

                const float gDepth = u_Depth.Sample(u_PointSampler, suv).r;
                if (gDepth >= 1.0) continue;                  // 采样到天空 → 该方向开阔
                const float3 geoPos = ReconstructViewPos(suv, gDepth);

                const float3 D = geoPos - P;
                const float dlen = length(D);
                if (dlen < max(bias, 1e-4)) continue;         // 自相交/过近 → 忽略
                // 仰角余弦：几何点相对法线的抬升程度
                const float cosH = clamp(dot(D, N) / dlen, -1.0, 1.0);
                cosHorizon[side] = max(cosHorizon[side], cosH);
            }
        }

        occlusion += IntegrateArc(cosHorizon[0], cosHorizon[1], cosN);
    }

    // 平均切片 → 归一化到 [0,1]（每切片最多贡献 π）
    occlusion /= max(float(slices), 1.0);
    float visibility = saturate(1.0 - occlusion / GTAO_PI);
    visibility = pow(visibility, max(intensity, 0.001));
    return float4(visibility, visibility, visibility, 1.0);
}
