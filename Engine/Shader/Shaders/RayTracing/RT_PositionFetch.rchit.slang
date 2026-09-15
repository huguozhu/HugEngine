// RT_PositionFetch.rchit.slang — 使用 VK_KHR_ray_tracing_position_fetch 的 ClosestHit 着色器
//
// 目的：验证 VK_KHR_ray_tracing_position_fetch 扩展在引擎中正常工作
//       消除对 SSBO 顶点拉取的依赖（SSBO 在 ClosestHit 中有兼容性问题）
//
// 使用 gl_HitTriangleVertexPositionsEXT 直接获取三角形顶点位置
// 无需通过 PrimitiveIndex → ByteAddressBuffer → StructuredBuffer<GPUVertex>
//
// 参考：VK_KHR_ray_tracing_position_fetch specification
//   gl_HitTriangleVertexPositionsEXT[3] — 世界空间顶点位置
//   HitTriangleVertexPosition(0/1/2)    — Slang 包装函数
//   HitTriangleBarycentrics()           — 重心坐标

struct RayPayload { float3 color; float hitT; };

// === set=1 (ClosestHit 资源) ===
[[vk::binding(0, 1)]] Texture2D<float4> u_MatTex;  // 材质纹理 (3×N RGBA32F)

// 光源 Uniform Buffer
struct LightUB { float4 ci; float4 dt; };
[[vk::binding(1, 1)]] cbuffer LightCB {
    LightUB u_Lights[8];
    uint    u_LightCount;
    uint    u_Pad[3];
};

// === set=2 (Bindless 纹理) ===
[[vk::binding(0, 2)]] Texture2D    u_Textures[];
[[vk::binding(0, 2)]] SamplerState u_Sampler;

// ============================================================
// 辅助函数
// ============================================================

// 从材质数据解析基础色：RGB=固有色, A=纹理索引+1
float3 SampleMaterialColor(float4 matData, float2 uv) {
    uint texId = uint(matData.w + 0.5);
    if (texId > 0) {
        uint idx = NonUniformResourceIndex(texId - 1);
        return u_Textures[idx].Sample(u_Sampler, uv).rgb;
    }
    return matData.rgb;
}

// ============================================================
// 主入口：使用 position_fetch 的 ClosestHit
// ============================================================
[shader("closesthit")]
void main(inout RayPayload payload) {
    uint instanceID = NonUniformResourceIndex(InstanceID());
    float3 P = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    // ── 步骤 1: 通过 position_fetch 直接获取顶点位置（无需 SSBO 拉取）──
    // VK_KHR_ray_tracing_position_fetch 提供的 GLSL 内建变量：
    //   gl_HitTriangleVertexPositionsEXT[0/1/2] = 三个顶点的世界空间位置
    float3 v0 = HitTriangleVertexPosition(0);  // = gl_HitTriangleVertexPositionsEXT[0]
    float3 v1 = HitTriangleVertexPosition(1);  // = gl_HitTriangleVertexPositionsEXT[1]
    float3 v2 = HitTriangleVertexPosition(2);  // = gl_HitTriangleVertexPositionsEXT[2]

    // ── 步骤 2: 从顶点位置计算面法线 ──
    float3 edge1 = v1 - v0;
    float3 edge2 = v2 - v0;
    float3 N = normalize(cross(edge1, edge2));

    // ── 步骤 3: 从重心坐标插值 UV（如果有顶点属性）──
    // 注意：position_fetch 只提供位置，不提供法线/UV
    // 对于阴影/AO 这类只需判断遮挡的 Pass，面法线 + 材质色已足够
    // 对于反射：使用面法线（而不是逐像素法线贴图）做第一次反弹
    // 对于 GI：面法线对间接漫反射的近似足够

    // ── 步骤 4: 获取材质色 ──
    float4 matData = u_MatTex.Load(int3(instanceID, 0, 0));
    float3 bc = matData.rgb;

    // 兜底：材质色接近黑色时使用实例 ID 生成伪随机色
    if (dot(bc, float3(0.3, 0.59, 0.11)) < 0.01) {
        uint h = instanceID * 2654435761u;
        bc = float3(
            0.52 + float((h >> 0)  & 0xFF) / 255.0 * 0.42,
            0.36 + float((h >> 8)  & 0xFF) / 255.0 * 0.38,
            0.20 + float((h >> 16) & 0xFF) / 255.0 * 0.30);
    }

    // ── 步骤 5: 光照计算（简化环境光 + 半球光 + 方向光）──
    float3 amb  = float3(0.22, 0.23, 0.26);
    float  hemi = (N.y * 0.5 + 0.5) * 0.12;
    float3 total = amb + hemi;

    for (uint li = 0; li < u_LightCount && li < 8; li++) {
        if (uint(u_Lights[li].dt.w) == 0) {
            total += u_Lights[li].ci.rgb * u_Lights[li].ci.w
                  * max(dot(N, u_Lights[li].dt.xyz), 0.0) * 0.15;
        }
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
