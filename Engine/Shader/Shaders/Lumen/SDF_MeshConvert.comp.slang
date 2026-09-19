// SDF_MeshConvert.comp.slang — scatter 的 u32 距离场 → R32F 输出 + 自检探针
//
// scatter 用 u32 场做原子最小（浮点位模式同序），消费者要的是浮点场：本 pass 做转换，
// 并顺带把按步长采样的距离写进探针缓冲，供 CPU 侧用独立实现复核（网格映射/偏移/描述符）。

[[vk::binding(0, 0)]] RWTexture3D<uint>          u_FieldU32;
[[vk::binding(1, 0)]] RWTexture3D<float>         u_FieldOut;
[[vk::binding(2, 0)]] RWStructuredBuffer<float>  u_ProbeDist;

[[vk::push_constant]] cbuffer ConvertPC {
    float4 originVoxel;   // xyz = 网格原点, w = 体素边长（本 pass 未用）
    uint4  dims;          // xyz = 分辨率, w = mesh 序号（0 = 写探针）
    uint4  ranges;        // w = 探针采样步长
};

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id >= dims.xyz)) return;
    const float d = asfloat(u_FieldU32[id]);
    u_FieldOut[id] = d;

    const uint stride = ranges.w;
    if (dims.w == 0u && stride > 0u) {
        if ((id.x % stride) == 0u && (id.y % stride) == 0u && (id.z % stride) == 0u) {
            const uint nx = dims.x / stride, ny = dims.y / stride, nz = dims.z / stride;
            const uint idx = (id.z / stride) * nx * ny + (id.y / stride) * nx + (id.x / stride);
            if (idx < nx * ny * nz) u_ProbeDist[idx] = d;
        }
    }
}
