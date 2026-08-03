// RT_DenoiseTemporal.frag.slang — RT 效果时域累积降噪
// 用途：对所有 RT 效果输出（阴影/反射/AO/GI）做时域重投影累积，抑制单帧噪声。
// 算法：GBuffer Velocity 重投影历史帧 → 深度/法线去遮挡检测 → 自适应混合。
// 支持任意通道格式（R16/R8 取 .r 通道，RGBA16 全通道），通过 float4 统一处理。
struct VSOutput {
    float4 position          : SV_Position;
    [[vk::location(0)]] float2 uv;
};

// ── 输入纹理（set=0，CombinedImageSampler）──
[[vk::binding(0, 0)]] Texture2D<float4> u_NoisyColor;  // 当前帧噪声 RT 输出
[[vk::binding(0, 0)]] SamplerState      u_PointSampler; // 最近邻采样（全部输入）
[[vk::binding(1, 0)]] Texture2D<float4> u_History;      // 上一帧降噪历史
[[vk::binding(2, 0)]] Texture2D<float>  u_Depth;        // GBuffer 深度
[[vk::binding(3, 0)]] Texture2D<float4> u_Normal;       // GBuffer 法线（编码 0~1）
[[vk::binding(4, 0)]] Texture2D<float2> u_Velocity;     // GBuffer 速度（currUV - prevUV）

// ── Push Constants（32B）──
[[vk::push_constant]] cbuffer Params {
    float2 texelSize;       // 1.0 / 纹理分辨率
    float  temporalBlend;   // 当前帧混合权重（0=纯历史, 1=纯当前帧）
    float  depthThreshold;  // 去遮挡深度容差
    float  normalThreshold; // 去遮挡法线 dot 阈值
    uint   isFirstFrame;    // 首帧标志（无历史数据，直接输出当前帧）
    float  _pad0;
    float  _pad1;
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;

    // 1. 当前帧数据
    float4 noisyColor = u_NoisyColor.SampleLevel(u_PointSampler, uv, 0);
    float  depth      = u_Depth.SampleLevel(u_PointSampler, uv, 0).r;
    float3 N          = normalize(u_Normal.SampleLevel(u_PointSampler, uv, 0).rgb * 2.0 - 1.0);

    // 2. 首帧：无历史数据，直接输出当前帧（同时写入历史完成初始化）
    if (isFirstFrame != 0u) {
        return noisyColor;
    }

    // 3. 速度重投影 → 历史 UV
    float2 velocity  = u_Velocity.SampleLevel(u_PointSampler, uv, 0).rg;
    float2 historyUV = uv - velocity;

    // 4. 屏幕外 → 无历史可用，使用当前帧
    if (historyUV.x < 0.0 || historyUV.x > 1.0 ||
        historyUV.y < 0.0 || historyUV.y > 1.0) {
        return noisyColor;
    }

    // 5. 去遮挡检测：历史 UV 处的深度/法线与当前差异过大 → 丢弃历史
    float  histDepth = u_Depth.SampleLevel(u_PointSampler, historyUV, 0).r;
    float3 histN     = normalize(u_Normal.SampleLevel(u_PointSampler, historyUV, 0).rgb * 2.0 - 1.0);
    float  depthDiff = abs(depth - histDepth);
    float  normalDot = dot(N, histN);

    float historyWeight = 1.0 - temporalBlend;
    if (depthDiff > depthThreshold || normalDot < normalThreshold) {
        historyWeight = 0.0;  // 去遮挡 → 完全使用当前帧
    }

    // 6. 混合当前帧 + 历史（lerp(a, b, w) = a*(1-w) + b*w）
    float4 histColor = u_History.SampleLevel(u_PointSampler, historyUV, 0);
    return lerp(noisyColor, histColor, historyWeight);
}
