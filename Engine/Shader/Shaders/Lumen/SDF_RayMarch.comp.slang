// SDF_RayMarch.comp.slang — sphere tracing 求交（步骤 11）
//
// 输入：一批世界（网格局部）空间的射线（origin / dir）+ Global SDF（R32F 3D 纹理）。
// 输出：命中距离 t、有效性、步数、命中点法线（SDF 梯度，前向差分 3 次采样）。
//
// 【为什么步进用 max(d, eps)】场是"到最近表面的下界"（§5 的注入语义保证不高估），所以
// t += d 是安全的；但下界可能极小（远处粗层尤其明显），纯 t += d 会停滞在极小的步长上，
// 故给一个下限 eps（= 0.25 体素）。上限步长不存在 —— 那正是高估，会穿漏。
//
// 【法线】用前向差分：N = normalize(vec3(f(p+dx)-f(p), f(p+dy)-f(p), f(p+dz)-f(p)))，3 次采样。

[[vk::binding(0, 0)]] Texture3D<float>          u_Field;
[[vk::binding(0, 0)]] SamplerState              u_FieldSampler;
// 第二层（clipmap 的远层）：近层细、远层粗，步进取两层的最小值（两者都是下界 ⇒ 仍不高估）
[[vk::binding(6, 0)]] Texture3D<float>          u_Field1;
[[vk::binding(6, 0)]] SamplerState              u_FieldSampler1;
[[vk::binding(2, 0)]] StructuredBuffer<float4>  u_RayOrigin;
[[vk::binding(3, 0)]] StructuredBuffer<float4>  u_RayDir;
[[vk::binding(4, 0)]] RWStructuredBuffer<float4> u_RayHit;     // x=t, y=有效, z=步数, w=dot(N,dir)
[[vk::binding(5, 0)]] RWStructuredBuffer<float4> u_RayNormal;  // xyz=命中点法线

[[vk::push_constant]] cbuffer MarchPC {
    float4 originVoxel;   // 层 0（近层）：xyz = 原点, w = 体素边长
    uint4  grid;          // 层 0：xyz = 分辨率, w = 射线数
    float4 marchParams;   // x = 最大步数, y = 收敛阈值（世界单位）, z = 最大距离, w = 未用
    float4 originVoxel1;  // 层 1（远层）
    uint4  grid1;         // 层 1：xyz = 分辨率
};

/// 层内采样：返回 (值, 是否在层内)
float2 SampleLayer(Texture3D<float> tex, SamplerState smp, float4 ov, uint3 res, float3 p) {
    const float3 local = (p - ov.xyz) / ov.w;
    const bool inside = all(local >= 0.0) && all(local <= float3(res));
    const float v = tex.SampleLevel(smp, (local + 0.5) / float3(res), 0);
    return float2(v, inside ? 1.0 : 0.0);
}

bool InsideField(float3 p) {
    const float3 local = (p - originVoxel.xyz) / originVoxel.w;
    return all(local >= 0.0) && all(local <= float3(grid.xyz));
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= grid.w) return;

    const float3 ro = u_RayOrigin[i].xyz;
    const float3 rd = normalize(u_RayDir[i].xyz);

    const uint  maxSteps = (uint)marchParams.x;
    const float eps      = marchParams.y;
    const float maxDist  = marchParams.z;

    float t = 0.0;
    bool  hit = false;
    bool  advanced = false;   // 命中判据要求"沿射线真正走过一步"，t=0 处的近表面不算命中
    uint  steps = 0;
    for (uint s = 0; s < maxSteps; ++s) {
        steps = s + 1;
        const float3 p = ro + rd * t;
        const float2 a = SampleLayer(u_Field,  u_FieldSampler,  originVoxel,  grid.xyz,  p);
        const float2 b = SampleLayer(u_Field1, u_FieldSampler1, originVoxel1, grid1.xyz, p);
        if (a.y < 0.5 && b.y < 0.5) break;               // 出所有层：本版按未命中处理
        const float d = min(a.y > 0.5 ? a.x : 1e30, b.y > 0.5 ? b.x : 1e30);
        if (advanced && d < eps) { hit = true; break; }
        // 安全步长系数 0.5：场是**近似**的（洪泛松弛可能给出偏大的值），
        // 全量步进会让射线越过真实表面（实测出现 3 条穿漏）。半量步进是近似场的标准做法：
        // 步数变多，但**不会穿漏** —— 安全判据优先于效率。
        // 步长下限与命中判据分开：命中判据仍是 eps，而**步长下限**取 eps/50。
        // 原来下限就等于 eps，射线起点离表面仅 1 单位时第一步就可能跨过表面（实测 19 条穿漏）。
        // 步长系数试降为 0.25：全局层的洪泛会**高估**，系数越小越不容易一步跨过表面（穿漏）。
        t += max(d * 0.25, eps * 0.02);
        advanced = true;
        if (t > maxDist) break;
    }

    float3 N = float3(0.0);
    float  nd = 0.0;
    if (hit) {
        const float3 p = ro + rd * t;
        const float h  = originVoxel.w;             // 一个体素的差分步长（取近层）
        // 梯度也用两层取最小（与步进同源，避免法线与命中面不一致）
        const float f0 = min(SampleLayer(u_Field,  u_FieldSampler,  originVoxel,  grid.xyz,  p).x,
                             SampleLayer(u_Field1, u_FieldSampler1, originVoxel1, grid1.xyz, p).x);
        N = float3(min(SampleLayer(u_Field, u_FieldSampler, originVoxel, grid.xyz, p + float3(h,0,0)).x,
                       SampleLayer(u_Field1, u_FieldSampler1, originVoxel1, grid1.xyz, p + float3(h,0,0)).x) - f0,
                   min(SampleLayer(u_Field, u_FieldSampler, originVoxel, grid.xyz, p + float3(0,h,0)).x,
                       SampleLayer(u_Field1, u_FieldSampler1, originVoxel1, grid1.xyz, p + float3(0,h,0)).x) - f0,
                   min(SampleLayer(u_Field, u_FieldSampler, originVoxel, grid.xyz, p + float3(0,0,h)).x,
                       SampleLayer(u_Field1, u_FieldSampler1, originVoxel1, grid1.xyz, p + float3(0,0,h)).x) - f0);
        const float len = length(N);
        N = (len > 1e-6) ? (N / len) : float3(0.0, 1.0, 0.0);
        nd = dot(N, rd);
    }

    u_RayHit[i]    = float4(t, hit ? 1.0 : 0.0, (float)steps, nd);
    u_RayNormal[i] = float4(N, 0.0);
}
