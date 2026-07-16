// ParticleRender.frag — Billboard 粒子片元着色器 + 软粒子

// 场景深度纹理（用于软粒子边缘混合）
[[vk::binding(3, 0)]] Texture2D<float> u_SceneDepth;

struct PSInput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float  lifeRatio : TEXCOORD1;
    uint   particleIdx : TEXCOORD2;
    float3 worldPos : TEXCOORD3;
};

// HSV → RGB
float3 hsv2rgb(float h, float s, float v) {
    float3 c = float3(h * 6.0, 0.0, 0.0);
    float i = floor(c.x);
    float f = c.x - i;
    float p = v * (1.0 - s);
    float q = v * (1.0 - s * f);
    float t = v * (1.0 - s * (1.0 - f));
    if (i == 0.0) return float3(v, t, p);
    if (i == 1.0) return float3(q, v, p);
    if (i == 2.0) return float3(p, v, t);
    if (i == 3.0) return float3(p, q, v);
    if (i == 4.0) return float3(t, p, v);
    return float3(v, p, q);
}

// 简易 hash → 色相
float hashToHue(uint seed) {
    uint h = (seed * 2654435761u) >> 24;
    return float(h) / 256.0;
}

float4 fragmentMain(PSInput input) : SV_Target0 {
    // 1. 圆形软边缘
    float2 center = input.uv - 0.5;
    float dist = length(center);
    float edgeAlpha = 1.0 - smoothstep(0.2, 0.5, dist);

    // 2. 基于粒子索引的鲜艳颜色
    float hue = hashToHue(input.particleIdx);
    float3 rgb = hsv2rgb(hue, 0.9, 1.0);

    // 3. 中心高亮发光
    float glow = exp(-dist * 3.0);
    rgb = lerp(rgb * 0.7, rgb * 1.5, glow);

    // 4. 生命周期衰减
    float lifeAlpha = 1.0 - input.lifeRatio * input.lifeRatio;
    float alpha = edgeAlpha * lifeAlpha * 0.85;

    // 5. 软粒子：读取场景深度，靠近几何体时淡出
    {
        // 用像素坐标直接 Load 场景深度（不需要 sampler）
        float sceneDepth = u_SceneDepth.Load(int3(input.position.xy, 0));
        float particleDepth = input.position.z;
        // 粒子在场景前方时 diff>0，靠近时 diff→0 则淡出
        float depthDiff = sceneDepth - particleDepth;
        float softFactor = saturate(depthDiff * 50.0);  // 50=过渡比例（越小越柔和）
        alpha *= softFactor;
    }

    if (alpha < 0.02) discard;
    return float4(rgb, alpha);
}
