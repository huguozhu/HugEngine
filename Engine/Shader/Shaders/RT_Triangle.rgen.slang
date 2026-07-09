// RT_Triangle.rgen.slang — RayGen 着色器：过程化红色三角形 + 深蓝背景
// 不依赖任何几何体/BLAS/TLAS，直接向 Storage Image 输出颜色

[[vk::binding(0, 0)]] RWTexture2D<float4> g_Output;

[shader("raygeneration")]
void main()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float2 uv = (float2(idx) + 0.5) / float2(dim);

    // 过程化三角形：用重心坐标判断像素是否在三角形内
    float2 p0 = float2(0.5, 0.2);
    float2 p1 = float2(0.8, 0.8);
    float2 p2 = float2(0.2, 0.8);

    float2 v0 = p1 - p0;
    float2 v1 = p2 - p0;
    float2 v2 = uv - p0;

    float d00 = dot(v0, v0);
    float d01 = dot(v0, v1);
    float d11 = dot(v1, v1);
    float d20 = dot(v2, v0);
    float d21 = dot(v2, v1);

    float invDenom = 1.0 / (d00 * d11 - d01 * d01);
    float b1 = (d11 * d20 - d01 * d21) * invDenom;
    float b2 = (d00 * d21 - d01 * d20) * invDenom;
    float b0 = 1.0 - b1 - b2;

    // 判断是否在三角形内
    if (b0 >= 0.0 && b1 >= 0.0 && b2 >= 0.0)
    {
        g_Output[idx] = float4(1.0, 0.0, 0.0, 1.0);  // 红色三角形
    }
    else
    {
        g_Output[idx] = float4(0.1, 0.15, 0.3, 1.0);  // 深蓝背景
    }
}
