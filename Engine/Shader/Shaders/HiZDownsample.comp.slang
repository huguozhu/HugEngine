// HiZDownsample.comp.slang — 深度金字塔下采样
// 从 level N 读取 2×2 区域，取最小值写入 level N+1

[[vk::binding(0, 0)]] Texture2D<float>  u_SrcDepth;  // 上一级深度
[[vk::binding(0, 0)]] SamplerState      u_PointSampler;
[[vk::binding(1, 0)]] RWTexture2D<float> u_DstDepth;  // 下一级深度（1/2 分辨率）

[[vk::push_constant]] cbuffer Params {
    uint2 srcSize;    // 上一级尺寸
    uint2 dstSize;    // 下一级尺寸
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= dstSize.x || id.y >= dstSize.y) return;

    // 源纹理中对应 2×2 区域的中心
    float2 uv = (float2(float(id.x), float(id.y)) + 0.5) / float2(float(dstSize.x), float(dstSize.y));

    // 采样 2×2 区域，取最小深度（最接近相机）
    float d00 = u_SrcDepth.SampleLevel(u_PointSampler, uv, 0, int2(0,0));
    float d10 = u_SrcDepth.SampleLevel(u_PointSampler, uv, 0, int2(1,0));
    float d01 = u_SrcDepth.SampleLevel(u_PointSampler, uv, 0, int2(0,1));
    float d11 = u_SrcDepth.SampleLevel(u_PointSampler, uv, 0, int2(1,1));

    float minDepth = min(min(d00, d10), min(d01, d11));
    u_DstDepth[id.xy] = minDepth;
}
