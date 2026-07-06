// SSAO.frag.slang — 屏幕空间环境光遮蔽
struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float>  u_Depth;
[[vk::binding(1, 0)]] Texture2D<float4> u_Normal;
[[vk::binding(2, 0)]] Texture2D<float4> u_Noise;
[[vk::binding(0, 0)]] SamplerState      u_PointSampler;  // 点采样（用于 GBuffer）

// Uniform buffer: 采样点 + 参数 + 正/逆投影矩阵
[[vk::binding(3, 0)]] cbuffer SSAOParams {
    float4 u_Samples[64];
    float4 u_Params;    // x=radius, y=bias, z=intensity, w=sampleCount
    float4x4 u_InvProj; // 逆投影矩阵（clip→view，用于从深度重建 view-space 位置）
    float4x4 u_Proj;    // 正投影矩阵（view→clip，用于将采样点投影到屏幕）
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;
    float depth = u_Depth.Sample(u_PointSampler, uv).r;
    if (depth >= 1.0) return float4(1,1,1,1);  // 远平面无遮挡

    float3 N = normalize(u_Normal.Sample(u_PointSampler, uv).rgb * 2.0 - 1.0);

    // 重建 view-space 位置（逆投影: clip→view）
    float4 clipP = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewP4 = mul(u_InvProj, clipP);
    float3 viewPos = viewP4.xyz / viewP4.w;

    // TBN 矩阵（随机旋转）
    float2 noiseScale = float2(320.0, 180.0);  // 视口尺寸 / noiseTex尺寸
    float3 randVec = normalize(u_Noise.Sample(u_PointSampler, uv * noiseScale).rgb * 2.0 - 1.0);
    float3 tangent   = normalize(randVec - N * dot(randVec, N));
    float3 bitangent = cross(N, tangent);
    float3x3 TBN     = float3x3(tangent, bitangent, N);

    float radius  = u_Params.x;
    float bias    = u_Params.y;
    int   samples = int(u_Params.w);
    float intensity = u_Params.z;

    float occlusion = 0.0;
    for (int i = 0; i < samples; i++) {
        float3 sDir = mul(TBN, u_Samples[i].xyz);     // tangent → view space
        float3 sPos = viewPos + sDir * radius;

        // 正向投影到屏幕（view→clip→NDC→UV）
        float4 clipS = mul(u_Proj, float4(sPos, 1.0));
        float3 ndcS = clipS.xyz / max(clipS.w, 0.0001);
        float2 suv = ndcS.xy * 0.5 + 0.5;

        // 屏幕外跳过
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        // 采样点深度（投影后的 NDC Z，已透视除法）
        float sampleDepth = ndcS.z;

        // 几何体深度（从深度缓冲读取）
        float geoDepth = u_Depth.Sample(u_PointSampler, suv).r;

        // 范围检查（避免深度不连续处的伪遮挡）
        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(viewPos.z - sPos.z));

        // 遮挡判定: Vulkan NDC [0,1]，引擎使用 Reverse-Z（近=1, 远=0）
        // sampleDepth < geoDepth 表示采样点比几何体更远 → 被遮挡
        occlusion += (sampleDepth < geoDepth - bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion /= float(samples);
    float ao = 1.0 - occlusion;
    ao = pow(ao, intensity);
    return float4(ao, ao, ao, 1.0);
}
