// RT_PBR.rchit.slang — Phase 4.2: 顶点拉取 + 重心插值法线 + Bindless 纹理 + 光源
//
// 数据流：
//   PrimitiveIndex → ByteAddressBuffer 读索引 → StructuredBuffer<GPUVertex> 读顶点
//   → 重心插值法线 + UV → bindless 纹理采样(可选) / 材质色 → 光源 → 输出
//
// 使用 -force-glsl-scalar-layout 确保 GPUVertex 布局为紧密打包 (32 字节)
// 对应 C++ 端的 RTVertexPacked 结构体

struct RayPayload { float3 color; float hitT; };

// GPU 端顶点结构（标量布局：position=12B, normal=12B, uv=8B，共 32B）
struct GPUVertex {
    float3 position;    // offset 0,  12 字节
    float3 normal;      // offset 12, 12 字节
    float2 uv;          // offset 24, 8 字节
};

// === set=1 (ClosestHit 资源) ===
// binding 0: 材质纹理 (1×N)
//   RGB = 基础色 (无 bindless 时使用)
//   A   = bindless 纹理索引 + 1 (0 = 无纹理, >=1 = 纹理索引-1)
[[vk::binding(0, 1)]] Texture2D<float4> u_MatTex;

// binding 1: 光源 Uniform Buffer
struct LightUB { float4 ci; float4 dt; };
[[vk::binding(1, 1)]] cbuffer LightCB { LightUB u_Lights[8]; uint u_LightCount; uint u_Pad[3]; };

// binding 2: 顶点缓冲 → StructuredBuffer<GPUVertex>
[[vk::binding(2, 1)]] StructuredBuffer<GPUVertex> u_Vertices;

// binding 3: 索引缓冲 → ByteAddressBuffer
[[vk::binding(3, 1)]] ByteAddressBuffer u_Indices;

// === set=2 (Bindless 纹理, Phase 4.2) ===
// binding 0: 纹理数组 (CombinedImageSampler)
[[vk::binding(0, 2)]] Texture2D u_Textures[];
[[vk::binding(0, 2)]] SamplerState u_Sampler;

// ============================================================
// 辅助函数
// ============================================================

void GetTriangleIndices(uint primId, out uint i0, out uint i1, out uint i2) {
    uint base = primId * 3;
    i0 = u_Indices.Load<int>((base + 0) * 4);
    i1 = u_Indices.Load<int>((base + 1) * 4);
    i2 = u_Indices.Load<int>((base + 2) * 4);
}

float3 ComputeBarycentrics(float3 P, float3 v0, float3 v1, float3 v2) {
    float3 e0 = v2 - v1;
    float3 e1 = v0 - v2;
    float3 e2 = v1 - v0;
    float3 d0 = P - v0;
    float3 d1 = P - v1;
    float3 d2 = P - v2;

    float area = length(cross(e0, e1));
    if (area < 1e-6) return float3(1.0/3.0, 1.0/3.0, 1.0/3.0);

    float w0 = length(cross(d1, d2)) / area;
    float w1 = length(cross(d2, d0)) / area;
    float w2 = 1.0 - w0 - w1;
    return float3(w0, w1, w2);
}

// 从材料数据解析基础色：RGB=固有色, A=纹理索引+1
float3 SampleMaterialColor(float4 matData, float2 uv) {
    uint texId = uint(matData.w + 0.5);  // 四舍五入
    if (texId > 0) {
        // 有 bindless 纹理：使用 UV 采样
        uint idx = NonUniformResourceIndex(texId - 1);
        return u_Textures[idx].Sample(u_Sampler, uv).rgb;
    }
    // 无 bindless 纹理：使用材质固有色
    return matData.rgb;
}

// ============================================================
// 主入口：ClosestHit 着色器
// ============================================================
[shader("closesthit")]
void main(inout RayPayload payload) {
    // ── 步骤 1: 获取命中信息 ──
    uint instanceID = NonUniformResourceIndex(InstanceID());
    uint primId     = NonUniformResourceIndex(PrimitiveIndex());
    float3 P = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    // ── 步骤 2: 顶点拉取 ──
    uint i0, i1, i2;
    GetTriangleIndices(primId, i0, i1, i2);

    GPUVertex v0 = u_Vertices[NonUniformResourceIndex(i0)];
    GPUVertex v1 = u_Vertices[NonUniformResourceIndex(i1)];
    GPUVertex v2 = u_Vertices[NonUniformResourceIndex(i2)];

    // ── 步骤 3: 重心插值（法线 + UV）──
    float3 bary = ComputeBarycentrics(P, v0.position, v1.position, v2.position);
    float3 N = normalize(v0.normal * bary.x + v1.normal * bary.y + v2.normal * bary.z);
    float2 uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;

    // ── 步骤 4: 材质色（bindless 纹理优先，回落固有色）──
    float4 matData = u_MatTex.Load(int3(instanceID, 0, 0));
    float3 bc = SampleMaterialColor(matData, uv);

    // 兜底：材质色接近黑色（未设置）时使用伪随机色
    if (dot(bc, float3(0.3, 0.59, 0.11)) < 0.01) {
        uint h = instanceID * 2654435761u;
        bc = float3(
            0.52 + float((h >> 0)  & 0xFF) / 255.0 * 0.42,
            0.36 + float((h >> 8)  & 0xFF) / 255.0 * 0.38,
            0.20 + float((h >> 16) & 0xFF) / 255.0 * 0.30);
    }

    // ── 步骤 5: 光照计算 ──
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

    // ── 步骤 6: 输出 ──
    payload.color = bc * total;
    payload.hitT  = RayTCurrent();
}
