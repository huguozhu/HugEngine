// PT_Full.rgen.slang — 全路径追踪 RayGen 着色器（迭代式路径追踪）
//
// 仅用 RayGen + ClosestHit + Miss 渲染完整帧：
//   1. 相机光线（分层抖动 + 每像素 SPP 采样）
//   2. 迭代式路径追踪（循环在 RayGen 内，每个 TraceRay 深度 1）：
//      - NEE（Next Event Estimation）：全光源循环 + 阴影射线 + MIS 幂次启发
//      - ReSTIR DI（flags bit0）：反弹 0 从蓄水池采样 1 条阴影射线替代 NEE
//      - 俄罗斯轮盘赌终止（flags bit2）
//      - BSDF 采样（漫反射余弦 + GGX 镜面混合）→ 下一根光线
//   3. 输出 4 个 UAV：HDR 颜色 / 线性视图深度 / 世界法线+粗糙度 / 屏幕速度
//
// 输出 GBuffer 供：RTDenoiser 时域累积（depth/normal/velocity）+
// ReSTIR_Init 重建世界坐标（depth+invViewProj）与粗糙度（normal.a）。
//
// 阴影射线用"哨兵 hitT"技巧：命中不执行任何着色器（payload 保持 a=0 =
// 被遮挡），miss 由 PT_Full.rmiss 写 a=-1 = 未遮挡。

#include "PT_Common.slang"  // PathPayload + 随机数 + BSDF 采样 + SampleLight

// === RayGen 资源 (set=0) ===
[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_HDR;      // RGBA16F 噪声 HDR 颜色
[[vk::binding(2, 0)]] RWTexture2D<float>  g_Depth;     // R32F 线性视图深度（-hitT, miss=-1000）
[[vk::binding(3, 0)]] RWTexture2D<float4> g_Normal;    // RGBA16F 世界法线(0~1)+roughness.a
[[vk::binding(4, 0)]] RWTexture2D<float2> g_Velocity;  // RG16F 屏幕速度（currUV - prevUV）
[[vk::binding(5, 0)]] StructuredBuffer<GPULight> g_Lights;   // 光源（RayGen 可读 SSBO）
[[vk::binding(8, 0)]] StructuredBuffer<PTReservoir> g_FinalReservoir;  // ReSTIR 蓄水池（上帧）

// === Push Constant（176B，见 ShaderTypes.slang）===
[[vk::push_constant]] PTPushConstant g_PC;

// === flags 位定义（与 C++ 一致）===
#define PT_FLAG_RESTIR   (1u << 0)   // 反弹 0 用 ReSTIR 蓄水池
#define PT_FLAG_MIS      (1u << 1)   // NEE 使用 MIS 幂次启发
#define PT_FLAG_ROULETTE (1u << 2)   // 俄罗斯轮盘赌终止
#define PT_FLAG_NEE      (1u << 3)   // NEE 直接光照

// 引擎最大光源数（kGPUMaxLights），SSBO 尺寸按此分配
#define PT_MAX_LIGHTS 8u

// ============================================================
// 发射阴影射线（哨兵 payload：a=0 被遮挡 / miss 写 a=-1 未遮挡）
// ============================================================
float TraceShadowRay(float3 P, float3 N, float3 L, float tMax) {
    RayDesc shadowRay;
    shadowRay.Origin    = P + N * 0.01;
    shadowRay.Direction = L;
    shadowRay.TMin      = 0.01;
    shadowRay.TMax      = max(tMax - 0.01, 0.01);

    PathPayload sp;
    sp.albedoMetallic = float4(0, 0, 0, 0);
    sp.normalRough    = float4(0, 0, 0, 1);
    sp.emissiveT      = float4(0, 0, 0, 0);  // 哨兵：命中不执行着色器 → a 保持 0

    TraceRay(g_TLAS,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
        0xFF, 0, 0, 0, shadowRay, sp);

    return sp.emissiveT.a < 0.0 ? 1.0 : 0.0;  // miss 写 a=-1 → 未遮挡
}

// ============================================================
// 主入口：RayGen
// ============================================================
[shader("raygeneration")]
void main() {
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    uint spp = max(1u, min(g_PC.sampleCount, 8u));
    uint maxBounces = max(1u, min(g_PC.maxBounces, 8u));

    float3 radiance = float3(0, 0, 0);

    // 主光线（反弹 0）的 GBuffer 输出数据
    float  viewZ       = -1000.0;   // miss 哨兵（ReSTIR 用 < -0.01 判定无效）
    float3 primaryN    = float3(0, 0, 0);
    float  primaryRough = 1.0;
    float3 primaryP    = float3(0, 0, 0);
    bool   primaryHit  = false;
    float2 currUV = (float2(idx) + 0.5) / float2(dim);

    for (uint s = 0; s < spp; s++) {
        // ── 相机光线（分层抖动）──
        float2 jitter = StratifiedJitter(s, spp, g_PC.frameIndex);
        float2 uv = (float2(idx) + jitter) / float2(dim);
        float2 ndc = uv * 2.0 - 1.0;
        ndc.y = -ndc.y;  // Vulkan NDC y 翻转
        float4 worldPos4 = mul(g_PC.invViewProj, float4(ndc, 1.0, 1.0));
        float3 rayDir = normalize(worldPos4.xyz / worldPos4.w - g_PC.cameraPos.xyz);

        RayDesc ray;
        ray.Origin    = g_PC.cameraPos.xyz;
        ray.Direction = rayDir;
        ray.TMin      = 0.01;
        ray.TMax      = 10000.0;

        float3 throughput = float3(1, 1, 1);

        for (uint bounce = 0; bounce < maxBounces; bounce++) {
            PathPayload payload;
            payload.albedoMetallic = float4(0, 0, 0, 0);
            payload.normalRough    = float4(0, 0, 0, 1);
            payload.emissiveT      = float4(0, 0, 0, -2);
            TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

            if (payload.emissiveT.a < 0.0) {
                // Miss：采样天空 → 终止路径
                radiance += throughput * payload.emissiveT.rgb;
                break;
            }

            float hitT = payload.emissiveT.a;
            float3 P   = ray.Origin + ray.Direction * hitT;
            float3 N   = payload.normalRough.xyz;
            float  rough = payload.normalRough.w;
            float3 albedo = payload.albedoMetallic.rgb;
            float  metallic = payload.albedoMetallic.a;

            if (bounce == 0) {
                primaryHit   = true;
                viewZ        = -hitT;
                primaryN     = N;
                primaryRough = rough;
                primaryP     = P;
            }

            // 自发光贡献（命中表面辐射）
            radiance += throughput * payload.emissiveT.rgb;

            float3 V = -ray.Direction;
            if (dot(N, V) <= 0.0) break;  // 法线背向（LoadHitNormal 已翻转，防御性检查）

            // ── 直接光照 ──
            bool restirDone = false;
            if (bounce == 0 && (g_PC.flags & PT_FLAG_RESTIR) != 0u) {
                // ReSTIR DI：从上帧蓄水池采样 1 个光源（1 条阴影射线替代 NEE 循环）
                PTReservoir res = g_FinalReservoir[idx.y * dim.x + idx.x];
                if (res.M > 0u && res.lightIndex < g_PC.lightCount) {
                    float3 lightPos2, L; float3 Li; float lightPdf;
                    if (SampleLight(g_Lights[res.lightIndex], P, lightPos2, L, Li, lightPdf)
                        && dot(N, L) > 0.0) {
                        float3 brdf = PBR_BRDF(albedo, metallic, rough, N, V, L);
                        float vis = TraceShadowRay(P, N, L, length(lightPos2 - P));
                        // RTXDI 无偏估计：f(y)·W·M / weightSum
                        // （W 为选中样本目标函数，时空复用 Pass 已对当前像素重评估）
                        float w = res.W * float(res.M) / max(res.weightSum, 1e-6);
                        radiance += throughput * brdf * Li * vis * w;
                        restirDone = true;
                    }
                }
            }
            if ((g_PC.flags & PT_FLAG_NEE) != 0u && !restirDone) {
                // NEE：遍历全部光源 + 阴影射线 + MIS
                for (uint li = 0; li < min(g_PC.lightCount, PT_MAX_LIGHTS); li++) {
                    float3 lightPos2, L; float3 Li; float lightPdf;
                    if (!SampleLight(g_Lights[li], P, lightPos2, L, Li, lightPdf)) continue;
                    float ndl = dot(N, L);
                    if (ndl <= 0.0) continue;

                    float3 brdf = PBR_BRDF(albedo, metallic, rough, N, V, L);

                    // BRDF 采样 pdf（与 SampleBSDF 相同的混合分布，MIS 用）
                    float3 H = normalize(V + L);
                    float NdotV = max(dot(N, V), 1e-4);
                    float NdotH = max(dot(N, H), 0.0);
                    float pSpec = lerp(0.35, 0.95, metallic);
                    float brdfPdf = (1.0 - pSpec) * (ndl / 3.14159265)
                                  + pSpec * (D_GGX(NdotH, rough) * NdotH / (4.0 * NdotV));

                    float weight = 1.0;
                    if ((g_PC.flags & PT_FLAG_MIS) != 0u)
                        weight = PowerHeuristic(lightPdf, brdfPdf);

                    float vis = TraceShadowRay(P, N, L, length(lightPos2 - P));
                    radiance += throughput * brdf * Li * vis * weight / lightPdf;
                }
            }

            // ── 俄罗斯轮盘赌终止（bounce > 2 后按 throughput 概率终止）──
            if ((g_PC.flags & PT_FLAG_ROULETTE) != 0u) {
                float survival = min(max(throughput.x, max(throughput.y, throughput.z)), 1.0);
                if (bounce > 2 && Rand(idx, g_PC.frameIndex, s * 97 + bounce * 13) > survival)
                    break;
                throughput /= max(survival, 1e-3);
            }

            // ── BSDF 采样 → 下一根光线 ──
            float2 uLobe = float2(Rand(idx, g_PC.frameIndex, s * 17 + bounce * 7 + 1), 0.0);
            float2 uDir  = float2(Rand(idx, g_PC.frameIndex, s * 23 + bounce * 11 + 3),
                                  Rand(idx, g_PC.frameIndex, s * 23 + bounce * 11 + 4));
            float pdf;
            float3 wi = SampleBSDF(N, V, metallic, rough, uLobe, uDir, pdf);
            if (pdf <= 0.0 || dot(N, wi) <= 0.0) break;

            float3 f = PBR_BRDF(albedo, metallic, rough, N, V, wi);
            throughput *= f * max(dot(N, wi), 0.0) / pdf;

            ray.Origin    = P + N * 0.001;
            ray.Direction = wi;
            ray.TMin      = 0.01;
            ray.TMax      = 10000.0;
        }
    }
    radiance /= float(spp);

    // ── 输出 4 个 UAV（GBuffer 仅对主光线命中像素有效）──
    float2 velocity = float2(0, 0);
    if (primaryHit) {
        float2 prevUV = TransformWorldToUV(primaryP, g_PC.prevViewProj);
        velocity = currUV - prevUV;   // 与 GBuffer.frag 约定一致（curr - prev）
    }

    g_HDR[idx]     = float4(radiance, 1.0);
    g_Depth[idx]   = viewZ;
    g_Normal[idx]  = float4(primaryN * 0.5 + 0.5, primaryRough);
    g_Velocity[idx] = velocity;
}
