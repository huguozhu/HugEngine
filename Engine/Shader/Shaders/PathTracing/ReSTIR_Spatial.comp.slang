// ReSTIR_Spatial.comp.slang — ReSTIR DI 空间复用
//
//   1. 从 TemporalReservoir 读取中心像素蓄水池
//   2. 邻域随机采样（radius 像素内 spatialSamples 个）→ 几何验证
//   3. 对中心像素重新评估邻域样本的目标函数 → WRS 合并
//   4. 输出 FinalReservoir（PT_RayGen 下帧读取）

#include "PT_Common.slang"  // Rand/SampleLight/ReconstructWorldPos

// === set=0 资源 ===
[[vk::binding(0, 0)]] Texture2D<float>  g_Depth;       // ptDepth：线性视图深度
[[vk::binding(1, 0)]] Texture2D<float4> g_Normal;      // ptNormal：世界法线(0~1) + roughness.a
[[vk::binding(2, 0)]] StructuredBuffer<PTReservoir> g_Temporal;   // TemporalReservoir（本帧）
[[vk::binding(3, 0)]] RWStructuredBuffer<PTReservoir> g_Final;    // FinalReservoir（本帧）
[[vk::binding(4, 0)]] StructuredBuffer<GPULight> g_Lights;        // 光源 SSBO
[[vk::binding(5, 0)]] Texture2D<float4> g_AlbedoMetallic;  // PT 第 5 UAV: albedo(rgb)+metallic(a)
[[vk::binding(6, 0)]] Texture3D<float4> g_BlueNoise;       // STBN 时空蓝噪声（Load 采样，Rand 用）

// === Push Constant ===
[[vk::push_constant]] ReSTIRPushConstant g_PC;


#include "PathTracing/ReSTIR_Common.slang"   // EmptyReservoir / EvaluateTarget（唯一出处）
[numthreads(8, 8, 1)]
void main(uint3 idx : SV_DispatchThreadID) {
    uint2 p = idx.xy;
    if (p.x >= g_PC.dispatchDimX || p.y >= g_PC.dispatchDimY) return;
    uint lin = p.y * g_PC.dispatchDimX + p.x;

    // 读取 PT 第 5 UAV：真实 albedo(rgb) + metallic(a)（中心像素材质）
    float4 am = g_AlbedoMetallic.Load(int3(p, 0));

    PTReservoir center = g_Temporal[lin];
    if (center.M == 0u) {
        g_Final[lin] = center;
        return;
    }

    // ── 中心像素几何 ──
    float depth = g_Depth.Load(int3(p, 0)).r;
    if (depth < -0.01) {
        // 有效像素：重建世界坐标（天空像素在 Temporal 已直出空蓄水池）
        float2 uv = (float2(p) + 0.5) / float2(g_PC.dispatchDimX, g_PC.dispatchDimY);
        float3 centerPos = ReconstructWorldPos(uv, -depth, g_PC.invViewProj, g_PC.cameraPos.xyz);
        float4 nb = g_Normal.Load(int3(p, 0));
        float3 centerN = normalize(nb.xyz * 2.0 - 1.0);
        float  roughness = nb.a;
        float3 V = normalize(g_PC.cameraPos.xyz - centerPos);

        // ── 邻域采样 + WRS 合并 ──
        uint samples = max(1u, min(g_PC.spatialSamples, 16u));
        int radius = max(1, int(g_PC.spatialRadius));

        for (uint i = 0; i < samples; i++) {
            float2 off = float2(Rand(p, g_PC.frameIndex, i * 2 + 5),
                                Rand(p, g_PC.frameIndex, i * 2 + 6)) * 2.0 - 1.0;
            int2 nbrIdx = int2(p) + int2(off * float(radius));
            if (nbrIdx.x < 0 || nbrIdx.x >= int(g_PC.dispatchDimX) ||
                nbrIdx.y < 0 || nbrIdx.y >= int(g_PC.dispatchDimY))
                continue;

            PTReservoir nbr = g_Temporal[uint(nbrIdx.y) * g_PC.dispatchDimX + uint(nbrIdx.x)];
            if (nbr.M == 0u) continue;

            // 几何验证（拒绝不相似邻域）
            float nbrDepth = g_Depth.Load(int3(nbrIdx, 0)).r;
            if (nbrDepth < -0.01) continue;
            float2 nbrUv = (float2(nbrIdx) + 0.5) / float2(g_PC.dispatchDimX, g_PC.dispatchDimY);
            float3 nbrPos = ReconstructWorldPos(nbrUv, -nbrDepth, g_PC.invViewProj, g_PC.cameraPos.xyz);
            float3 nbrN = normalize(g_Normal.Load(int3(nbrIdx, 0)).xyz * 2.0 - 1.0);
            if (distance(centerPos, nbrPos) > 1.0) continue;   // 1m 位置容差
            if (dot(centerN, nbrN) < 0.9) continue;            // 法线朝向容差

            // 对中心像素重新评估邻域样本的目标函数
            float3 nbrLightPos;
            float pHat = EvaluateTarget(centerPos, centerN, V, roughness, am.rgb, am.a,
                                        nbr.lightIndex, nbrLightPos);
            if (pHat <= 0.0) continue;

            // WRS 合并
            float totalW = center.weightSum + pHat;
            if (Rand(p, g_PC.frameIndex, i + 50u) * totalW < pHat) {
                // 选中邻域样本
                center.lightIndex = nbr.lightIndex;
                center.lightPos   = nbr.lightPos;
                center.W          = pHat;
            }
            center.weightSum = totalW;
            center.M += nbr.M;
        }
    }

    g_Final[lin] = center;
}
