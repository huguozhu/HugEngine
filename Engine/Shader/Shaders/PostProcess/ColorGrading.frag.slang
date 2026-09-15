// ColorGrading.frag.slang — LDR 色彩分级（饱和度 / 对比度 / 色彩平衡 / Vibrance）
// 输入 LDR BGRA8 → 输出 LDR BGRA8

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_Input;
[[vk::binding(0, 0)]] SamplerState      u_Sampler;

[[vk::push_constant]] cbuffer CGParams {
    float saturation;     // 1.0=正常
    float contrast;       // 1.0=正常, >1=增强对比
    float vibrance;       // 0=关闭
    float shadowsR;       // 暗部色彩平衡 R
    float shadowsG;
    float shadowsB;
    float midtonesR;
    float midtonesG;
    float midtonesB;
    float highlightsR;
    float highlightsG;
    float highlightsB;
    float _pad;
};

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// 标准分段 sRGB 传递函数（与 common.slang 保持一致）
float3 SRGBToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow((max(c, 0.0) + 0.055) / 1.055, float3(2.4));
    return c <= 0.04045 ? lo : hi;
}
float3 LinearToSRGB(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), float3(1.0 / 2.4)) - 0.055;
    return c <= 0.0031308 ? lo : hi;
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float4 color = u_Input.Sample(u_Sampler, input.uv);

    // sRGB → Linear（准确计算用 pow，简单场景用近似）
    float3 c = SRGBToLinear(color.rgb);

    // 1. 饱和度
    float lum = Luminance(c);
    c = lerp(float3(lum, lum, lum), c, saturation);

    // 2. Vibrance（智能饱和度）
    float avgSat = (max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b)) / max(max(max(c.r, c.g), c.b), 0.001);
    float vibFactor = 1.0 + vibrance * (1.0 - avgSat);
    c = lerp(float3(lum, lum, lum), c, vibFactor);

    // 3. 对比度
    float midGray = 0.5;
    c = (c - midGray) * contrast + midGray;
    c = clamp(c, 0.0, 1.0);

    // 4. 色彩平衡（暗部/中间/亮部分别调色）
    float3 shadows   = float3(shadowsR, shadowsG, shadowsB);
    float3 midtones  = float3(midtonesR, midtonesG, midtonesB);
    float3 highlights = float3(highlightsR, highlightsG, highlightsB);
    float tShadow    = smoothstep(0.3, 0.0, lum);
    float tHighlight = smoothstep(0.6, 1.0, lum);
    float tMid       = 1.0 - tShadow - tHighlight;
    c = c + shadows * tShadow * 0.5 + midtones * tMid * 0.3 + highlights * tHighlight * 0.5;

    // Linear → sRGB
    c = clamp(c, 0.0, 1.0);
    color.rgb = LinearToSRGB(c);
    return color;
}
