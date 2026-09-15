// RT_Reflection.rgen.slang — 硬件 Ray Tracing 反射射线生成着色器
//
// 数据流：
//   GBuffer 深度 + 法线 + 粗糙度 → 重建世界坐标 → 反射方向（GGX 重要性采样）
//   → TraceRay → ClosestHit 评估命中表面出射辐射度 / Miss 回退天空色
//   → 输出 RGBA16_FLOAT 反射颜色（rgb + 命中距离 a，-1=未命中）
//
// 输出由 LightingPass 在 rtSpecularSource=1 时采样（u_RT_Reflection）。

#include "ShaderTypes.slang"  // RTRayEffectPushConstant（C++/Slang 共享布局）

// === RayGen 资源 (set=0) ===
[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_Output;   // 反射颜色输出
[[vk::binding(2, 0)]] Texture2D<float>  g_Depth;       // GBuffer 深度
[[vk::binding(3, 0)]] Texture2D<float4> g_NormalRough; // GBufferB: worldNormal.xyz + roughness.w

// === Push Constant（共享结构，见 ShaderTypes.slang）===
[[vk::push_constant]] RTRayEffectPushConstant g_PC;

// === Payload（与 ClosestHit/Miss 共享）===
struct ReflectionPayload {
    float4 colorT;  // rgb=反射颜色, a=命中距离（-1=未命中）
};

// ============================================================
// 工具函数
// ============================================================

// 简单整数哈希（PCG 风格）
uint Hash(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x ^= x >> 4u;
    x *= 0x27d4eb2du;
    x ^= x >> 15u;
    return x;
}

// 确定性随机数（像素坐标 + 帧索引 + 采样号 → [0,1)）
float Rand(uint2 idx, uint frame, uint s) {
    uint seed = idx.x * 1973u + idx.y * 9277u + frame * 26699u + s * 479u;
    return float(Hash(seed) & 0xFFFFu) / 65536.0;
}

// GGX 微表面法线重要性采样（半向量 H，用于粗糙反射）
float3 SampleGGX(float3 N, float roughness, float2 u) {
    float a = max(roughness * roughness, 1e-4);
    float phi = 2.0 * 3.14159265 * u.x;
    float cosTheta = sqrt((1.0 - u.y) / (1.0 + (a * a - 1.0) * u.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float3 H = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    // 变换到 N 的正切空间
    float3 Up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(Up, N));
    float3 B = cross(N, T);
    return normalize(H.x * T + H.y * B + H.z * N);
}

// 从深度和 UV 重建世界坐标（Vulkan NDC y 翻转，与 RT_Sponza 一致）
float3 ReconstructWorldPos(uint2 idx, float depth) {
    float2 uv = (float2(idx) + 0.5) / float2(g_PC.dispatchDimX, g_PC.dispatchDimY);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = mul(g_PC.invViewProj, clipPos);
    return worldPos4.xyz / worldPos4.w;
}

// ============================================================
// 主入口：RayGen 着色器
// ============================================================
[shader("raygeneration")]
void main() {
    uint2 idx = DispatchRaysIndex().xy;

    // ── 读取 GBuffer ──
    float depth = g_Depth.Load(int3(idx, 0)).r;
    if (depth >= 1.0 || depth <= 0.0) {
        g_Output[idx] = float4(0, 0, 0, -1);  // 天空像素无反射
        return;
    }
    float4 gbB = g_NormalRough.Load(int3(idx, 0));
    float3 N = normalize(gbB.xyz * 2.0 - 1.0);
    float roughness = gbB.w;

    // 粗糙度超过阈值：不发射 RT 反射线，回退 IBL prefilter（a=-1 标记无效）
    if (roughness > g_PC.maxRoughness) {
        g_Output[idx] = float4(0, 0, 0, -1);
        return;
    }

    // ── 重建世界坐标 + 视线方向 ──
    float3 worldPos = ReconstructWorldPos(idx, depth);
    float3 V = normalize(g_PC.cameraPos.xyz - worldPos);

    // ── 反射方向：粗糙度 > 阈值时 GGX 重要性采样，否则镜面 ──
    float3 R = reflect(-V, N);
    if (roughness > 0.02) {
        float2 u = float2(Rand(idx, g_PC.frameIndex, 0), Rand(idx, g_PC.frameIndex, 1));
        float3 H = SampleGGX(N, roughness, u);
        R = reflect(-V, H);
        if (dot(R, N) < 0.0) R = reflect(-V, N);  // 回退镜面，避免射入表面内
    }

    // ── 发射反射光线 ──
    RayDesc ray;
    ray.Origin    = worldPos + N * 0.01;
    ray.Direction = R;
    ray.TMin      = 0.01;
    ray.TMax      = g_PC.maxDistance;

    ReflectionPayload payload;
    payload.colorT = float4(0, 0, 0, -1);
    TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    g_Output[idx] = payload.colorT;
}
