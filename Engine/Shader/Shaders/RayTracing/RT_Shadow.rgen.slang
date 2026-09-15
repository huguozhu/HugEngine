// RT_Shadow.rgen.slang — 硬件 Ray Tracing 阴影射线生成着色器
// 对每个像素发射 shadow ray，输出屏幕空间阴影遮罩
//
// 数据流：
//   GBuffer 深度 + 法线 → 重建世界坐标 → 对每个光源发射 TraceRay
//   → AnyHit 标记遮挡 → 输出 float 阴影遮罩（0=全影, 1=无影）
//
// 阴影语义（修复旧版恒无阴影 bug）：
//   使用 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
//     + RAY_FLAG_SKIP_CLOSEST_HIT_SHADER，
//   命中检测由 RT_Shadow.rahit（AnyHit）完成——置 payload.blocked=true。
//   旧版错误地在 ClosestHit 置 blocked 却用 SKIP_CLOSEST_HIT 跳过它，
//   导致 payload 恒为 false → 阴影恒为 1.0（无阴影）。

#include "ShaderTypes.slang"  // RTShadowPushConstant（C++/Slang 共享布局）

// === RayGen 资源 (set=0) ===
[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;

// 输出：单通道阴影遮罩纹理（R16_FLOAT）
[[vk::binding(1, 0)]] RWTexture2D<float> g_ShadowMask;

// GBuffer 输入
[[vk::binding(2, 0)]] Texture2D<float>  g_DepthBuffer;      // 深度缓冲
[[vk::binding(3, 0)]] Texture2D<float4> g_GBufferB;          // worldNormal.xyz + roughness.w

// 光源 Uniform Buffer（由 RTShadowPass 每帧从 GPULight[] 显式抽取，
// 避免与 GPULight SSBO 的 64B 布局不匹配——旧版 bug 根源）
struct ShadowLight {
    float4 pos_type;      // xyz=位置/方向, w=类型(0=方向光, 1=点光源, 2=聚光灯)
    float4 color_radius;  // rgb=颜色, w=强度（用于剔除无效光源）
    float4 spotDir_angle; // xyz=聚光方向, w=cos(内锥角)（聚光灯专用）
    float4 radius;        // x=光源半径（软阴影），与 C++ ShadowLightGPU 布局一致（16×64B）
};
[[vk::binding(4, 0)]] cbuffer ShadowLightCB {
    ShadowLight g_Lights[16];
};

// === Push Constant（共享结构，见 ShaderTypes.slang）===
[[vk::push_constant]] RTShadowPushConstant g_PC;

// === Payload（与 AnyHit/Miss 共享）===
struct ShadowPayload {
    bool blocked;  // true=被遮挡, false=可见
};

// === 随机数工具（与 RT_AO/RT_GI 等共享同款实现，供软阴影面光源采样使用）===
uint Hash(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x ^= x >> 4u;
    x *= 0x27d4eb2du;
    x ^= x >> 15u;
    return x;
}

// 基于像素索引 + 帧号 + 样本号的确定性随机数（[0,1)）
float Rand(uint2 idx, uint frame, uint s) {
    uint seed = idx.x * 1973u + idx.y * 9277u + frame * 26699u + s * 479u;
    return float(Hash(seed) & 0xFFFFu) / 65536.0;
}

// 从深度和 UV 重建世界坐标
// 注意：Vulkan 像素坐标 y 向下，而相机 ViewProj 为 GLM 约定（y 向上），
// 必须先翻转 NDC y（与 RT_Sponza.rgen 一致），否则重建结果上下颠倒。
float3 ReconstructWorldPos(uint2 idx, float depth) {
    float2 uv = (float2(idx) + 0.5) / float2(g_PC.dispatchDimX, g_PC.dispatchDimY);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Vulkan NDC y 翻转
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = mul(g_PC.invViewProj, clipPos);
    return worldPos4.xyz / worldPos4.w;
}

// 软阴影：面光源采样（在光源面上采样 spp 个发射点，发射 spp 条射线取平均）。
// spp<=1 且 radius.x=0 时退化为硬阴影（单条精确射线，与旧版行为一致）。
float TraceShadowRaySoft(float3 worldPos, float3 N, ShadowLight light, uint spp) {
    // 射线生成阶段：取当前像素索引作为随机种子（与 main() 一致）
    uint2 idx = DispatchRaysIndex().xy;

    // ── 确定基础光线方向和最大距离 ──
    float3 L;
    float  tMax;

    if (light.pos_type.w == 0.0) {
        // 方向光：光线方向 = 光源方向，最大距离 = 场景包围盒
        L = normalize(light.pos_type.xyz);
        tMax = 10000.0;
    } else {
        // 点光源/聚光灯：光线方向 = 指向光源，最大距离 = 到光源距离
        float3 lightPos = light.pos_type.xyz;
        float3 toLight  = lightPos - worldPos;
        float  dist     = length(toLight);
        L     = toLight / dist;
        tMax  = dist;
    }

    float shadow = 0.0;
    for (uint s = 0; s < spp; s++) {
        float3 dir = L;
        if (light.pos_type.w == 0.0) {
            // 方向光：光锥内按角度半径抖动（radius.x 为弧度）
            float3 up = abs(L.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
            float3 T  = normalize(cross(up, L));
            float3 B  = cross(L, T);
            float  ang = light.radius.x * sqrt(Rand(idx, g_PC.frameIdx, s * 2));
            float  phi = 6.2831853 * Rand(idx, g_PC.frameIdx, s * 2 + 1);
            dir = normalize(L + (T * cos(phi) + B * sin(phi)) * tan(ang));
        } else {
            // 点/聚光：面向 shade point 的圆盘上采样发射点（半径 radius.x）
            float3 toL = normalize(light.pos_type.xyz - worldPos);
            float3 up  = abs(toL.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
            float3 T   = normalize(cross(up, toL));
            float3 B   = cross(toL, T);
            float  r   = light.radius.x * sqrt(Rand(idx, g_PC.frameIdx, s * 2));
            float  phi = 6.2831853 * Rand(idx, g_PC.frameIdx, s * 2 + 1);
            float3 offset = (T * cos(phi) + B * sin(phi)) * r;
            float3 lp     = light.pos_type.xyz + offset;  // 采样后的光源发射点
            float3 toP    = lp - worldPos;
            float  dist   = length(toP);
            dir  = toP / dist;
            tMax = dist;
        }

        // ── 发射阴影射线（AnyHit 检测遮挡，见 RT_Shadow.rahit.slang）──
        RayDesc ray;
        ray.Origin    = worldPos + N * 0.01;  // 沿法线偏移避免自交
        ray.Direction = dir;
        ray.TMin      = 0.01;
        ray.TMax      = tMax - 0.01;  // 减去微小偏移避免光线刚好命中光源位置

        ShadowPayload payload;
        payload.blocked = false;

        TraceRay(
            g_TLAS,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,  // 跳过 ClosestHit，仅用 AnyHit 检测遮挡
            0xFF,   // instanceMask：追踪所有实例
            0,      // sbtRecordOffset：阴影 Hit Group
            0,      // sbtRecordStride
            0,      // missIndex：阴影 Miss Shader
            ray,
            payload
        );
        shadow += payload.blocked ? 0.0 : 1.0;
    }
    return shadow / float(spp);
}

// ============================================================
// 主入口：RayGen 着色器
// ============================================================
[shader("raygeneration")]
void main() {
    uint2 idx = DispatchRaysIndex().xy;

    // ── 读取 GBuffer 数据 ──
    float  depth = g_DepthBuffer.Load(int3(idx, 0));
    float4 gbB   = g_GBufferB.Load(int3(idx, 0));
    float3 N     = normalize(gbB.xyz * 2.0 - 1.0);

    // ── 无效像素（天空等）→ 完全照亮 ──
    if (depth >= 1.0 || depth <= 0.0) {
        g_ShadowMask[idx] = 1.0;
        return;
    }

    // ── 重建世界坐标 ──
    float3 worldPos = ReconstructWorldPos(idx, depth);

    // ── 超出最大阴影距离 → 完全照亮 ──
    if (distance(worldPos, g_PC.cameraPos.xyz) > g_PC.maxShadowDist) {
        g_ShadowMask[idx] = 1.0;
        return;
    }

    // ── 对每个光源计算阴影 ──
    float shadow = 1.0;

    for (uint li = 0; li < g_PC.lightCount; li++) {
        ShadowLight light = g_Lights[li];

        // 跳过无贡献光源（方向光 dot(N,L) < 0 → 背对光源 → 无需阴影计算）
        float3 L_test;
        if (light.pos_type.w == 0.0) {
            L_test = normalize(light.pos_type.xyz);
        } else {
            L_test = normalize(light.pos_type.xyz - worldPos);
        }
        float NdotL = dot(N, L_test);
        if (NdotL <= 0.0) continue;

        // 软阴影：shadowFlags bit2 开启时按 softSPP 采样面光源；关闭时 spp=1 退化为硬阴影
        bool  soft = (g_PC.shadowFlags & 4u) != 0u;   // bit2=软阴影
        uint  spp  = soft ? g_PC.softSPP : 1u;
        shadow *= TraceShadowRaySoft(worldPos, N, light, spp);
        if (shadow <= 0.0) break;  // 任何光源已完全遮挡 → 提前退出
    }

    g_ShadowMask[idx] = shadow;
}
