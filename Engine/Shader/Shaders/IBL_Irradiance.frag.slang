// ============================================================
// IBL_Irradiance.frag.slang — 辐照度图卷积
//
// 对 Skybox Cubemap 做半球方向采样，计算漫反射辐照度。
// 每个 pixel 输出当前面方向上的半球积分结果。
// ============================================================

struct VSOutput {
    float4 position            : SV_Position;
    [[vk::location(0)]] float3 worldDir;
};

[[vk::binding(0, 0)]] TextureCube<float4> u_SkyboxTexture;
[[vk::binding(0, 0)]] SamplerState       u_SkyboxSampler;

// 半球均匀采样步长（32×32 辐照度图，16 采样点足矣）
static const float SAMPLE_DELTA = 0.05;

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float3 N = normalize(input.worldDir);  // 当前面像素对应的世界方向（法线）

    // 构建切线空间基（绕 N 的半球采样）
    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);

    float3 irradiance = float3(0.0);
    uint sampleCount = 0;

    // 黎曼和：半球均匀采样
    for (float phi = 0.0; phi < 2.0 * 3.14159265; phi += SAMPLE_DELTA) {
        for (float theta = 0.0; theta < 0.5 * 3.14159265; theta += SAMPLE_DELTA) {
            // 球面坐标 → 切线空间方向
            float3 tangentDir = float3(
                sin(theta) * cos(phi),
                sin(theta) * sin(phi),
                cos(theta));

            // 切线空间 → 世界空间
            float3 worldSample = tangentDir.x * T + tangentDir.y * B + tangentDir.z * N;

            // 采样天空盒 + cos(theta) 权重（Lambertian）
            float3 color = u_SkyboxTexture.Sample(u_SkyboxSampler, worldSample).rgb;
            irradiance += color * cos(theta) * sin(theta);  // sin(theta) 是球面积分元
            sampleCount++;
        }
    }

    irradiance = irradiance * 3.14159265 / float(sampleCount);  // 归一化
    return float4(irradiance, 1.0);
}
