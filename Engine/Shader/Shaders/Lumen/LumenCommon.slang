// LumenCommon.slang — Lumen 探针/表面缓存 pass 的公共小工具（唯一出处）
//
// 【为什么收敛】以下两个函数在 Lumen 的两个 pass 里各有一份**同名但捕获不同**的实现：
//   · `WorldToClip` —— 一个读 push constant 的 `vp0..vp3`，一个读 `u_Frame[0].vp*`
//   · `FieldValue`  —— 一个读 push constant 的 `originVoxel0/grid0/...`，一个读 `u_Frame[0].*`
// 逻辑完全相同，只是"每帧常量从哪来"不同 ⇒ 改为把这几项作为**参数**传入即可共享
// （调用点各自传自己的来源，数值完全不变）。
//
// 【零绑定】本文件不声明任何资源、也不依赖调用方声明特定名字：体素场纹理/采样器与每帧常量
// 全部作为**参数**传入（Slang 允许把资源数组作参数，这里用到的是单张 Texture3D）。
// 这样"只用 WorldToClip"的 pass 不会因为 FieldValue 引用了未声明的 u_Field 而编译失败。
#ifndef HUGENGINE_LUMEN_COMMON_SLANG
#define HUGENGINE_LUMEN_COMMON_SLANG

// 体素场单层采样：返回 (距离, 是否命中)；`ov.w` 是体素边长，<=0 视为层未启用。
// 复用自 Lumen/ScreenProbe_Trace.comp 与 SurfaceCache_Capture.comp（两处逐字相同）。
// ⚠ SDF 调试/射线步进两个 shader 里的同名变体是**刻意不同**的（无 `ov.w` 守卫），
//   已改名为 `SampleLayerSDF`，不要与本函数混用。
float2 SampleLayer(Texture3D<float> tex, SamplerState smp, float4 ov, uint3 res, float3 p) {
    if (ov.w <= 0.0) return float2(0.0, 0.0);
    const float3 local = (p - ov.xyz) / ov.w;
    const bool inside = all(local >= 0.0) && all(local <= float3(res));
    if (!inside) return float2(0.0, 0.0);
    return tex.SampleLevel(smp, local / float3(res), 0.0).xx;
}

// 世界坐标 → 裁剪空间：用显式 4 行而非 mat4（与各 pass 的 push constant 约定一致，
// 避免行列主序歧义）。复用自 Lumen_SurfaceCacheSample.comp / SurfaceCache_Capture.comp。
float4 WorldToClip(float4 vp0, float4 vp1, float4 vp2, float4 vp3, float3 p) {
    return float4(dot(vp0.xyz, p) + vp0.w, dot(vp1.xyz, p) + vp1.w,
                  dot(vp2.xyz, p) + vp2.w, dot(vp3.xyz, p) + vp3.w);
}

// 双层级联体素场查询：返回最近命中距离（未命中为 1e30），并回出是否落在任一层内。
// 复用自 Lumen/ScreenProbe_Trace.comp / SurfaceCache_Capture.comp（此前各自实现）。
float FieldValue(Texture3D<float> field0, SamplerState sampler0,
                 Texture3D<float> field1, SamplerState sampler1,
                 float3 p, float4 originVoxel0, uint3 grid0,
                 float4 originVoxel1, uint3 grid1, out bool insideAny) {
    const float2 a = SampleLayer(field0, sampler0, originVoxel0, grid0, p);
    const float2 b = SampleLayer(field1, sampler1, originVoxel1, grid1, p);
    insideAny = (a.y > 0.5) || (b.y > 0.5);
    return min(a.y > 0.5 ? a.x : 1e30, b.y > 0.5 ? b.x : 1e30);
}

#endif // HUGENGINE_LUMEN_COMMON_SLANG
