// SDF_LayerProbe.comp.slang — 从某一层的距离场里取样到探针缓冲（自检用）
//
// 【为什么单独一个 pass】原先把探针写出塞在"u32 → R32F 转换"那个 mode 里（靠 `id % stride`
// 条件），实测近层（层 0）的探针缓冲**从未被写过**（哨兵残留 64/64），而层 1 正常。
// 与其去猜那段条件与状态顺序，不如把"取样"这件事做成一个显然正确的独立 dispatch：
// 每个线程负责一个探针，按坐标采样该层的场、直接写结果。**先证明数据是被写出来的**。

[[vk::binding(2, 0)]] Texture3D<float>          u_Field;     // 该层的距离场（与采样器同 binding）
[[vk::binding(2, 0)]] SamplerState              u_FieldSampler;
[[vk::binding(4, 0)]] RWStructuredBuffer<float> u_ProbeDist;

[[vk::push_constant]] cbuffer ProbePC {
    float4 originVoxel;   // xyz = 该层原点, w = 体素边长
    uint4  dims;          // xyz = 该层分辨率, w = 探针采样步长
    uint4  ranges;        // x = 探针总数
};

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= ranges.x) return;

    const uint stride = dims.w;
    const uint nx = dims.x / stride, ny = dims.y / stride, nz = dims.z / stride;
    const uint x = i % nx;
    const uint y = (i / nx) % ny;
    const uint z = i / (nx * ny);
    if (z >= nz) return;

    // 探针位于体素中心（与 CPU 参考的取样点一致）
    const float3 local = float3((float)(x * stride) + 0.5f, (float)(y * stride) + 0.5f,
                                (float)(z * stride) + 0.5f);
    const float3 uv = local / float3(dims.xyz);
    u_ProbeDist[i] = u_Field.SampleLevel(u_FieldSampler, uv, 0);
}
