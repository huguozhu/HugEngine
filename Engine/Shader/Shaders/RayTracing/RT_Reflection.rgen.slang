// RT_Reflection.rgen.slang — 硬件 Ray Tracing 反射射线生成着色器
//
// 数据流：
//   GBuffer 深度 + 法线 + 粗糙度 → 重建世界坐标 → 反射方向（GGX 重要性采样）
//   → TraceRay → ClosestHit 评估命中表面出射辐射度 / Miss 回退天空色
//   → 输出 RGBA16_FLOAT 反射颜色（rgb + 命中距离 a，-1=未命中）
//
// 输出由 LightingPass 在 rtSpecularSource=1 时采样（u_RT_Reflection）。

#include "Sampling.slang"   // SampleGgxHalf（GGX 半向量，唯一出处）
#include "ShaderTypes.slang"  // RTRayEffectPushConstant（C++/Slang 共享布局）

// === RayGen 资源 (set=0) ===
[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_Output;   // 反射颜色输出
[[vk::binding(2, 0)]] Texture2D<float>  g_Depth;       // GBuffer 深度
[[vk::binding(3, 0)]] Texture2D<float4> g_NormalRough; // GBufferB: worldNormal.xyz + roughness.w

// === Push Constant（共享结构，见 ShaderTypes.slang）===
[[vk::push_constant]] RTRayEffectPushConstant g_PC;

#include "RT_Common.slang"   // Hash / RandHash / ReconstructWorldPosFromIndex（唯一出处）
// === Payload（与 ClosestHit/Miss 共享）===
#include "RTPayloads.slang"   // ShadowPayload / RayPayload / GIPayload / ReflectionPayload（唯一出处）

// ============================================================
// 工具函数
// ============================================================

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
    float3 worldPos = ReconstructWorldPosFromIndex(idx, depth);
    float3 V = normalize(g_PC.cameraPos.xyz - worldPos);

    // ── 反射方向：粗糙度 > 阈值时 GGX 重要性采样，否则镜面 ──
    float3 R = reflect(-V, N);
    if (roughness > 0.02) {
        float2 u = float2(RandHash(idx, g_PC.frameIndex, 0), RandHash(idx, g_PC.frameIndex, 1));
        float3 H = SampleGgxHalf(N, roughness, u, 1e-4);   // 沿用原 alpha 下限 1e-4
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
