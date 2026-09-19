// SurfaceCache_Capture.comp.slang — Card 捕获：把卡片表面写进 atlas（步骤 15）
//
// 【做法】一次 dispatch 捕获**一张卡**（参数走 push constant），组内 256 个线程遍历该卡的
// pageRes×pageRes 个 texel：
//   1. texel 中心的世界坐标 = planeOrigin + planeStepU*lx + planeStepV*ly（卡平面），
//      再沿 axis 方向做**定步长 march**，用全局 SDF（近层/远层取 min）找该轴上的第一个表面点；
//   2. 命中点投影到屏幕 → 采 **GBuffer 的 albedo / normal**（这就是"场景材质"，无需另做材质系统）；
//   3. 写进 3 张 atlas（albedo / normal / 自发光，RGBA16F）的对应页；未命中、相机后方、
//      GBuffer 上无几何的 texel 一律写 alpha=0（= 该 texel 缺内容）。
//
// 【为什么用 GBuffer 当材质源】步骤 15 的验收是"atlas 可视化与场景材质一致"，GBuffer 里就是本帧的
// 真实材质；卡片捕获本质是"对每条轴做一次可见性查询"，命中点在本帧可见就能取到正确材质。
// 完整版（L2 收尾）会把材质换成 bindless 逐材质求值，使 atlas 独立于屏幕。
//
// 【定步长而非 sphere tracing】这里只要"这条轴上的第一个表面"，固定步长实现简单、步数天然有界
// （AABB 厚度 / 步长）；命中判据仍用 |d| < eps。
[[vk::binding(0, 0)]] Texture3D<float>        u_Field;      // 近层
[[vk::binding(0, 0)]] SamplerState            u_FieldSampler;
[[vk::binding(6, 0)]] Texture3D<float>        u_Field1;     // 远层
[[vk::binding(6, 0)]] SamplerState            u_FieldSampler1;
[[vk::binding(2, 0)]] Texture2D<float4>       u_GBufferAlbedo;
[[vk::binding(3, 0)]] Texture2D<float4>       u_GBufferNormal;
[[vk::binding(4, 0)]] Texture2D<float>        u_GBufferDepth;
[[vk::binding(1, 0)]] RWTexture2D<float4>     u_AtlasAlbedo;
[[vk::binding(7, 0)]] RWTexture2D<float4>     u_AtlasNormal;
[[vk::binding(8, 0)]] RWTexture2D<float4>     u_AtlasEmissive;
struct CaptureFrame {       // 每帧常量（与 C++ 的 CaptureFrame 逐字段一致）
    float4 vp0, vp1, vp2, vp3;
    uint4  screen;
    float4 originVoxel0;
    float4 originVoxel1;
    uint4  grid0;
    uint4  grid1;
};
[[vk::binding(10, 0)]] StructuredBuffer<CaptureFrame> u_Frame;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint> u_Stats;     // 0=命中写入 1=未命中 2=march 命中 3=未用 4..7=push constant 回报（诊断）

[[vk::push_constant]] cbuffer CapturePC {
    float4 pageOriginRes;   // xy = 该页在 atlas 的像素原点, z = 页边长(texel), w = texel 世界边长
    float4 planeOrigin;     // xyz = texel(0,0) 角点在起始平面上的世界坐标
    float4 planeStepU;      // xyz = 沿 u 每前进 1 texel 的世界位移
    float4 planeStepV;      // xyz = 沿 v 每前进 1 texel 的世界位移
    float4 axis;            // xyz = march 方向（单位向量）, w = march 起点距离
    float4 marchParams;     // x = 步数, y = eps（世界单位）, z = 步长, w = 未用
};

float2 SampleLayer(Texture3D<float> tex, SamplerState smp, float4 ov, uint3 res, float3 p) {
    if (ov.w <= 0.0) return float2(0.0, 0.0);
    const float3 local = (p - ov.xyz) / ov.w;
    const bool inside = all(local >= 0.0) && all(local <= float3(res));
    return float2(tex.SampleLevel(smp, (local + 0.5) / float3(res), 0), inside ? 1.0 : 0.0);
}

float FieldValue(float3 p, out bool insideAny) {
    const float2 a = SampleLayer(u_Field,  u_FieldSampler,  u_Frame[0].originVoxel0, u_Frame[0].grid0.xyz, p);
    const float2 b = SampleLayer(u_Field1, u_FieldSampler1, u_Frame[0].originVoxel1, u_Frame[0].grid1.xyz, p);
    insideAny = (a.y > 0.5) || (b.y > 0.5);
    return min(a.y > 0.5 ? a.x : 1e30, b.y > 0.5 ? b.x : 1e30);
}

float4 WorldToClip(float3 p) {
    return float4(dot(u_Frame[0].vp0.xyz, p) + u_Frame[0].vp0.w,
                  dot(u_Frame[0].vp1.xyz, p) + u_Frame[0].vp1.w,
                  dot(u_Frame[0].vp2.xyz, p) + u_Frame[0].vp2.w,
                  dot(u_Frame[0].vp3.xyz, p) + u_Frame[0].vp3.w);
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    // 【诊断】把 push constant 实际收到的值回报给 CPU（4..7 号槽）：验证"常量是否到达"
    if (tid.x == 0u && tid.y == 0u) {
        u_Stats[4] = asuint(planeOrigin.x);
        u_Stats[5] = asuint(u_Frame[0].originVoxel0.x);
        u_Stats[6] = asuint(u_Frame[0].originVoxel0.w);
        u_Stats[7] = asuint(planeStepU.x);
    }
    const int   pageRes = (int)pageOriginRes.z;
    const int   tiles   = (pageRes + 15) / 16;
    const uint  steps   = (uint)marchParams.x;

    for (int ty = 0; ty < tiles; ++ty) {
        for (int tx = 0; tx < tiles; ++tx) {
            const int lx = tx * 16 + (int)tid.x;
            const int ly = ty * 16 + (int)tid.y;
            if (lx >= pageRes || ly >= pageRes) continue;

            const float3  planeP = planeOrigin.xyz + planeStepU.xyz * (float)lx + planeStepV.xyz * (float)ly;
            float t = axis.w;
            bool  hit = false;
            float3 hitP = float3(0.0, 0.0, 0.0);
            for (uint s = 0; s < steps; ++s) {
                const float3 q = planeP + axis.xyz * t;
                bool inside = false;
                const float d = FieldValue(q, inside);
                if (!inside) { t += marchParams.z; continue; }
                if (d < marchParams.y) { hitP = q; hit = true; break; }
                t += marchParams.z;
            }

            const uint2 dst = uint2((uint)pageOriginRes.x + (uint)lx, (uint)pageOriginRes.y + (uint)ly);
            float4 albedo = float4(0.0, 0.0, 0.0, 0.0);
            float4 normal = float4(0.0, 0.0, 0.0, 0.0);
            bool written = false;
            if (hit) {
                const float4 clip = WorldToClip(hitP);
                if (clip.w > 0.0) {
                    const float2 ndc = clip.xy / clip.w;
                    const float2 uv  = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
                        const uint2 px = uint2((uint)clamp(uv.x * (float)u_Frame[0].screen.x, 0.0, (float)u_Frame[0].screen.x - 1.0),
                                               (uint)clamp(uv.y * (float)u_Frame[0].screen.y, 0.0, (float)u_Frame[0].screen.y - 1.0));
                        const float gbDepth = u_GBufferDepth.Load(int3((int2)px, 0));
                        // 存在性判据：GBuffer 在该像素上确实有几何（深度落在 (0,1) 内）
                        if (gbDepth > 0.0 && gbDepth < 1.0) {
                            albedo = float4(u_GBufferAlbedo.Load(int3((int2)px, 0)).rgb, 1.0);
                            normal = float4(u_GBufferNormal.Load(int3((int2)px, 0)).rgb, 1.0);
                            written = true;
                        }
                    }
                }
            }
            u_AtlasAlbedo[dst]   = albedo;
            u_AtlasNormal[dst]   = normal;
            u_AtlasEmissive[dst] = written ? float4(0.0, 0.0, 0.0, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
            if (hit)      InterlockedAdd(u_Stats[2], 1u);   // 诊断：SDF march 命中（不管 GBuffer 是否有效）
            if (written) InterlockedAdd(u_Stats[0], 1u); else InterlockedAdd(u_Stats[1], 1u);
        }
    }
}
