// SSAO.frag.slang — 屏幕空间环境光遮蔽
struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float>  u_Depth;
[[vk::binding(1, 0)]] Texture2D<float4> u_Normal;
[[vk::binding(2, 0)]] Texture2D<float4> u_Noise;
[[vk::binding(0, 0)]] SamplerState      u_PointSampler;

// Uniform buffer: 采样点 + 参数 + 正/逆投影矩阵
[[vk::binding(3, 0)]] cbuffer SSAOParams {
    float4 u_Samples[64];
    float4 u_Params;    // x=radius, y=bias, z=intensity, w=sampleCount
    float4x4 u_InvProj; // 逆投影（clip→view）
    float4x4 u_Proj;    // 正投影（view→clip）
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;
    float depth = u_Depth.Sample(u_PointSampler, uv).r;
    if (depth >= 1.0) return float4(1,1,1,1);

    float3 N = normalize(u_Normal.Sample(u_PointSampler, uv).rgb * 2.0 - 1.0);

    // 重建 view-space 位置
    float4 clipP = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewP4 = mul(u_InvProj, clipP);
    float3 viewPos = viewP4.xyz / viewP4.w;

    // 随机旋转 TBN
    float2 noiseScale = float2(320.0, 180.0);
    float3 randVec = normalize(u_Noise.Sample(u_PointSampler, uv * noiseScale).rgb * 2.0 - 1.0);
    float3 tangent   = normalize(randVec - N * dot(randVec, N));
    float3 bitangent = cross(N, tangent);
    float3x3 TBN     = float3x3(tangent, bitangent, N);

    float radius  = u_Params.x;
    float bias    = u_Params.y;
    int   samples = int(u_Params.w);
    float intensity = u_Params.z;

    // 按 view-space Z 缩放半径
    float viewDist = max(abs(viewPos.z), 0.1);
    float scaledRadius = radius * viewDist * 0.5;

    float occlusion = 0.0;
    for (int i = 0; i < samples; i++) {
        float3 sDir = mul(TBN, u_Samples[i].xyz);     // tangent → view
        float3 sPos = viewPos + sDir * scaledRadius;

        // 投影到屏幕 UV
        float4 clipS = mul(u_Proj, float4(sPos, 1.0));
        float3 ndcS = clipS.xyz / max(clipS.w, 0.0001);
        float2 suv = ndcS.xy * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        // 读取该位置的深度，重建 view-space 位置
        float geoDepth = u_Depth.Sample(u_PointSampler, suv).r;
        if (geoDepth >= 1.0) continue;
        float4 clipG = float4(suv * 2.0 - 1.0, geoDepth, 1.0);
        float4 viewG4 = mul(u_InvProj, clipG);
        float3 geoPos = viewG4.xyz / viewG4.w;

        // View-space 比较: |geoPos.z| < |sPos.z| → 几何体更近 → 采样点被遮挡
        float d1 = abs(geoPos.z);
        float d2 = abs(sPos.z);
        if (d1 < d2 - bias) {
            float dist = length(geoPos - viewPos);
            float rangeCheck = smoothstep(scaledRadius, 0.0, dist);
            occlusion += rangeCheck;
        }
    }

    occlusion /= float(samples);
    float ao = 1.0 - occlusion;
    ao = pow(ao, intensity);
    return float4(ao, ao, ao, 1.0);
}
