// RT_Sponza.rchit.slang — Phase 3: 1D纹理材质 + cbuffer光源

struct RayPayload { float3 color; float hitT; };

// 用 1×N 纹理替代 cbuffer 数组 — 天然支持非均匀索引
[[vk::binding(0, 1)]] Texture2D<float4> u_MatTex;

struct LightUB { float4 ci; float4 dt; };
[[vk::binding(1, 1)]] cbuffer LightUB { LightUB u_Lights[8]; uint u_LightCount; uint u_Pad[3]; };

[shader("closesthit")]
void main(inout RayPayload payload) {
    uint id = InstanceID();

    // 纹理采样 — 天然支持非均匀索引
    float3 bc = u_MatTex.Load(int3(id, 0, 0)).rgb;
    if (dot(bc, float3(0.3, 0.59, 0.11)) < 0.01) {
        uint h = id * 2654435761u;
        bc = float3(
            0.52 + float((h >> 0)  & 0xFF) / 255.0 * 0.42,
            0.36 + float((h >> 8)  & 0xFF) / 255.0 * 0.38,
            0.20 + float((h >> 16) & 0xFF) / 255.0 * 0.30);
    }

    float3 N = -WorldRayDirection();
    float3 amb = float3(0.22, 0.23, 0.26);
    float hemi = (N.y * 0.5 + 0.5) * 0.12;
    float3 total = amb + hemi;

    for (uint li = 0; li < u_LightCount && li < 8; li++) {
        if (uint(u_Lights[li].dt.w) == 0)
            total += u_Lights[li].ci.rgb * u_Lights[li].ci.w
                  * max(dot(N, u_Lights[li].dt.xyz), 0.0) * 0.15;
    }
    if (u_LightCount == 0) {
        float3 L0 = normalize(float3(0.5, -0.9, 0.3));
        total += float3(1.0, 0.92, 0.78) * max(dot(N, L0), 0.0) * 0.55;
        float3 L1 = normalize(float3(-0.3, 0.4, -0.2));
        total += float3(0.45, 0.50, 0.58) * max(dot(N, L1), 0.0) * 0.35;
    }

    payload.color = bc * total;
}
