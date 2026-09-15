// PT_Atrous.comp.slang — SVGF 风格 A-Trous 空间滤波
//
// 输入：时域降噪后的 HDR + 深度/法线 GBuffer
// 输出：空间滤波后的 HDR
// 4 迭代（step=1,2,4,8），每迭代 5-tap 十字核；
// 边权重 = depth(σ) × normal(dot^power) × luminance(σ 由 3×3 局部亮度方差自适应)；
// 可选火萤钳制（样本亮度显著高于中心 → 压低权重）。
// 全部用 Load() 整数取像素（无采样器）。

#include "ShaderTypes.slang"

[[vk::binding(0, 0)]] Texture2D<float4> g_Color;    // 时域降噪后 HDR
[[vk::binding(1, 0)]] Texture2D<float>  g_Depth;    // 线性视图深度（米）
[[vk::binding(2, 0)]] Texture2D<float4> g_Normal;   // 世界法线(0~1)+roughness.a
[[vk::binding(3, 0)]] RWTexture2D<float4> g_Output; // 空间滤波结果

struct AtrousPC {
    uint2 dispatchDim;    // 分辨率
    uint  iterations;     // 1-5
    float sigmaDepth;     // 深度边权重 σ（米）
    float normalPower;    // 法线边权重指数（如 128）
    float sigmaColor;     // 颜色边权重 σ 系数（× 局部亮度标准差）
    float clampThreshold; // 火萤钳制阈值（0=关）
    float pad1, pad2;
};
[[vk::push_constant]] AtrousPC g_PC;

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(8, 8, 1)]
void main(uint3 idx : SV_DispatchThreadID) {
    uint2 p = idx.xy;
    if (p.x >= g_PC.dispatchDim.x || p.y >= g_PC.dispatchDim.y) return;

    float3 centerC = g_Color.Load(int3(p, 0)).rgb;
    float  centerZ = g_Depth.Load(int3(p, 0)).r;
    float3 centerN = normalize(g_Normal.Load(int3(p, 0)).xyz * 2.0 - 1.0);
    float  centerLum = Luminance(centerC);

    // 3×3 局部亮度方差 → 自适应颜色边 σ
    float mean = 0.0, meanSq = 0.0;
    [unroll]
    for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
        float l = Luminance(g_Color.Load(int3(int2(p) + int2(dx, dy), 0)).rgb);
        mean += l; meanSq += l * l;
    }
    mean /= 9.0; meanSq /= 9.0;
    float sigmaColor = g_PC.sigmaColor * sqrt(max(meanSq - mean * mean, 1e-6));

    float3 sum = centerC;
    float  wSum = 1.0;
    uint   iters = clamp(g_PC.iterations, 1u, 5u);
    float  step = 1.0;

    for (uint i = 0; i < iters; i++) {
        int2 s = int2(step);
        int2 offs[4] = { int2( s.x, 0), int2(-s.x, 0), int2(0,  s.y), int2(0, -s.y) };
        for (uint k = 0; k < 4; k++) {
            int2 np = int2(p) + offs[k];
            if (np.x < 0 || np.y < 0 ||
                np.x >= int(g_PC.dispatchDim.x) || np.y >= int(g_PC.dispatchDim.y)) continue;

            float3 c = g_Color.Load(int3(np, 0)).rgb;
            float  z = g_Depth.Load(int3(np, 0)).r;
            float3 n = normalize(g_Normal.Load(int3(np, 0)).xyz * 2.0 - 1.0);

            float wDepth = exp(-abs(z - centerZ) / max(g_PC.sigmaDepth, 1e-4));
            float wNorm  = pow(max(dot(n, centerN), 0.0), g_PC.normalPower);
            float wCol   = exp(-abs(Luminance(c) - centerLum) / max(sigmaColor, 1e-4));

            float w = wDepth * wNorm * wCol;

            // 火萤钳制：样本远亮于中心 → 视为异常高光，压低权重
            if (g_PC.clampThreshold > 0.0) {
                if (Luminance(c) > centerLum * g_PC.clampThreshold + 1e-4)
                    w *= 0.05;
            }

            sum += c * w;
            wSum += w;
        }
        step *= 2.0;
    }

    g_Output[p] = float4(sum / max(wSum, 1e-4), 1.0);
}
