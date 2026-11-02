// RT_Triangle.rgen.slang — RayGen 着色器：过程化红色三角形 + 深蓝背景
// 不依赖任何几何体/BLAS/TLAS，直接向 Storage Image 输出颜色

[[vk::binding(0, 0)]] RWTexture2D<float4> g_Output;

// 推送常量：与 Triangle.vert.slang / 01.Triangle.cpp 的 RotationPushConstant 逐字段一致
struct TrianglePushConstant {
    float angle;   // 绕画面中心的旋转角（弧度）
};

[[vk::push_constant]] TrianglePushConstant g_PC;

[shader("raygeneration")]
void main()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float2 uv = (float2(idx) + 0.5) / float2(dim);

    // 【旋转】RT 路径的三角形是在**UV 空间**里定义的（p0/p1/p2 是归一化坐标），
    // 所以绕画面中心 (0.5,0.5) 旋转采样坐标即可，效果与光栅化/Mesh 路径一致。
    const float c = cos(g_PC.angle);
    const float s = sin(g_PC.angle);
    const float2 d = uv - float2(0.5, 0.5);
    uv = float2(0.5, 0.5) + float2(d.x * c - d.y * s, d.x * s + d.y * c);

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
