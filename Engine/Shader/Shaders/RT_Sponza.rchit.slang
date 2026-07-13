// RT_Sponza.rchit.slang — Phase 4: 2×N 扩展纹理 + 完整 PBR 材质数据
//
// 方案：用 2×N RGBA32F 纹理替代 StructuredBuffer，规避 slangc StorageBuffer 在 ClosestHit 中的 bug
//   Row 0 (y=0): baseColorFactor (RGBA)
//   Row 1 (y=1): metallic, roughness, ao, alphaCutoff (RGBA)
//
// slangc bug: StructuredBuffer / ByteAddressBuffer（均映射到 SPIR-V StorageBuffer）
//   在 ClosestHit 着色器中导致 GPU 静默失败（黑屏），slangc 2026.1 ~ 2026.13 均未修复
//   变通：Texture2D::Load() 天然支持非均匀索引，在 ClosestHit 中稳定工作

struct RayPayload { float3 color; float hitT; };

// 2×N 材质纹理：行0=baseColor, 行1=PBR参数
[[vk::binding(0, 1)]] Texture2D<float4> u_MatTex;

// 光源 Uniform Buffer
struct LightUB { float4 ci; float4 dt; };
[[vk::binding(1, 1)]] cbuffer LightUB { LightUB u_Lights[8]; uint u_LightCount; uint u_Pad[3]; };

[shader("closesthit")]
void main(inout RayPayload payload) {
    uint id = InstanceID();

    // ── 1. 从 2×N 纹理读取完整 PBR 材质数据 ──
    // Row 0 (y=0): baseColorFactor
    float4 baseColor = u_MatTex.Load(int3(id, 0, 0));
    float3 bc = baseColor.rgb;

    // Row 1 (y=1): metallic, roughness, ao, alphaCutoff
    float4 pbrParams = u_MatTex.Load(int3(id, 1, 0));
    float metallic  = pbrParams.x;
    float roughness = pbrParams.y;
    float ao        = pbrParams.z;

    // 若 baseColorFactor 接近黑色，用随机颜色染色（调试可见性）
    if (dot(bc, float3(0.3, 0.59, 0.11)) < 0.01) {
        uint h = id * 2654435761u;
        bc = float3(
            0.52 + float((h >> 0)  & 0xFF) / 255.0 * 0.42,
            0.36 + float((h >> 8)  & 0xFF) / 255.0 * 0.38,
            0.20 + float((h >> 16) & 0xFF) / 255.0 * 0.30);
    }

    // ── 2. 法线计算 ──
    float3 N = -WorldRayDirection();
    // HitKind 修正朝向（HIT_KIND_TRIANGLE_FRONT_FACE=0xFE, BACK_FACE=0xFF）
    if (HitKind() == 0xFF) N = -N;

    // ── 3. 光照计算 ──
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

    // ── 4. 材质属性影响 ──
    float roughnessAtten = 1.0 - roughness * 0.3;
    float aoAtten = lerp(0.7, 1.0, ao);

    payload.color = bc * total * roughnessAtten * aoAtten;
}
