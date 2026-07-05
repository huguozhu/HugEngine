// SSAO.frag.slang — 屏幕空间环境光遮蔽
struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float>  u_Depth;
[[vk::binding(1, 0)]] Texture2D<float4> u_Normal;
[[vk::binding(2, 0)]] Texture2D<float4> u_Noise;
[[vk::binding(0, 0)]] SamplerState      u_PointSampler;  // 点采样（用于 GBuffer）

// Uniform buffer: 64 个半球采样点 + 参数
[[vk::binding(3, 0)]] cbuffer SSAOParams {
    float4 u_Samples[64];
    float4 u_Params;  // x=radius, y=bias, z=intensity, w=sampleCount
    float4x4 u_Proj;  // 投影矩阵（用于重建 view-space Z）
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;
    float depth = u_Depth.Sample(u_PointSampler, uv).r;
    if (depth >= 1.0) return float4(1,1,1,1);  // 远平面无遮挡

    float3 N = normalize(u_Normal.Sample(u_PointSampler, uv).rgb * 2.0 - 1.0);

    // 重建 view-space 位置（利用投影矩阵）
    float4 clipP = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewP4 = mul(u_Proj, clipP);
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

        // 投影到屏幕
        float4 offset = mul(u_Proj, float4(sPos, 1.0));
        offset.xyz /= offset.w;
        float2 suv = offset.xy * 0.5 + 0.5;

        // 采样点深度
        float sd = u_Depth.Sample(u_PointSampler, suv).r;

        // 范围检查（避免深度不连续处的伪遮挡）
        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(viewPos.z - sPos.z));

        // 如果采样点的 view-space Z >= 采样深度 → 被遮挡
        float sViewZ = (mul(u_Proj, float4(suv*2.0-1.0, sd, 1.0)).z) /
                       (mul(u_Proj, float4(suv*2.0-1.0, sd, 1.0)).w);
        // 简化：直接比较 NDC Z
        float sampleClipZ = sd;
        float samplePosClipZ = (mul(u_Proj, float4(sPos, 1.0)).z) /
                                (mul(u_Proj, float4(sPos, 1.0)).w);
        occlusion += (sampleClipZ >= samplePosClipZ + bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion /= float(samples);
    float ao = 1.0 - occlusion;
    ao = pow(ao, intensity);
    return float4(ao, ao, ao, 1.0);
}
