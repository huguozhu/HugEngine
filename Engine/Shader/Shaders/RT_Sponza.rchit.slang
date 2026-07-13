// RT_Sponza.rchit.slang — Phase 5: Callable bindless PBR + 平滑 Triplanar + 天空光照

struct RayPayload { float3 color; float hitT; };

struct BindlessCallData {
    uint  materialID;
    float2 uv;
    float3 albedo;
    float3 normal;
    float  metallic;
    float  roughness;
    float  ao;
};

[[vk::binding(0, 1)]] Texture2D<float4> u_MatTex;

struct LightUB { float4 ci; float4 dt; };
[[vk::binding(1, 1)]] cbuffer LightUB { LightUB u_Lights[8]; uint u_LightCount; uint u_Pad[3]; };

// Triplanar UV — 平滑轴混合
float2 TriplanarUV(float3 P, float3 N) {
    float s = 0.01;
    float3 w = abs(N) / (abs(N).x + abs(N).y + abs(N).z + 0.0001);
    return (P.zy * w.x + P.xz * w.y + P.xy * w.z) * s;
}

[shader("closesthit")]
void main(inout RayPayload payload) {
    uint id = InstanceID();

    float3 bc = u_MatTex.Load(int3(id, 0, 0)).rgb;
    float4 pp = u_MatTex.Load(int3(id, 1, 0));
    float roughnessFac = pp.y;
    float aoFac = pp.z;
    uint  matID = uint(u_MatTex.Load(int3(id, 2, 0)).r);

    if (dot(bc, float3(0.3, 0.59, 0.11)) < 0.01) {
        uint h = id * 2654435761u;
        bc = float3(
            0.52 + float((h >> 0)  & 0xFF) / 255.0 * 0.42,
            0.36 + float((h >> 8)  & 0xFF) / 255.0 * 0.38,
            0.20 + float((h >> 16) & 0xFF) / 255.0 * 0.30);
    }

    float3 W = WorldRayDirection();
    float3 N = -W;
    if (HitKind() == 0xFF) N = -N;
    float3 P = WorldRayOrigin() + W * RayTCurrent();

    BindlessCallData callData;
    callData.materialID = matID;
    callData.uv = TriplanarUV(P, N);
    callData.albedo    = bc;
    callData.normal    = float3(0, 0, 1);
    callData.metallic  = 0.0;
    callData.roughness = roughnessFac;
    callData.ao        = aoFac;
    CallShader(0, callData);

    float3 albedo    = bc * callData.albedo;
    float  roughness = clamp(roughnessFac * callData.roughness, 0.04, 1.0);
    float  ao        = aoFac * callData.ao;
    float3 Nf        = normalize(N + callData.normal * 0.15);

    float3 skyAmb = float3(0.55, 0.58, 0.62);
    float3 total  = lerp(float3(0.15, 0.12, 0.08), skyAmb, (Nf.y * 0.5 + 0.5));

    for (uint li = 0; li < u_LightCount && li < 8; li++) {
        if (uint(u_Lights[li].dt.w) == 0)
            total += u_Lights[li].ci.rgb * u_Lights[li].ci.w * max(dot(Nf, u_Lights[li].dt.xyz), 0.0);
    }
    if (u_LightCount == 0) {
        float3 sunDir = normalize(float3(0.5, -0.8, 0.4));
        total += float3(1.0, 0.93, 0.78) * max(dot(Nf, sunDir), 0.0) * 4.0;
        float3 fillDir = normalize(float3(-0.3, 0.3, -0.2));
        total += float3(0.3, 0.35, 0.45) * max(dot(Nf, fillDir), 0.0) * 1.0;
    }

    float roughnessAtten = 1.0 - roughness * 0.15;
    float aoAtten = lerp(0.5, 1.0, ao);

    payload.color = albedo * total * roughnessAtten * aoAtten;
}
