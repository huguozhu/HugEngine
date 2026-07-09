// RT_Shadow.rgen.slang — 硬阴影光线生成着色器
// 对每个像素发射 shadow ray 采样光源可见性，输出屏幕空间阴影遮罩

[shader("raygeneration")]
void main()
{
    // 从 DispatchRaysIndex 获取像素坐标
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    // TODO: 从常量缓冲读取相机参数 + 光源方向
    // float3 rayOrigin = worldPosFromDepth(idx, depthBuffer);
    // float3 rayDir = normalize(lightDir);

    // 初始化光线 payload
    // RayDesc ray;
    // ray.Origin    = rayOrigin + normal * bias;
    // ray.Direction = rayDir;
    // ray.TMin      = 0.001;
    // ray.TMax      = lightDistance;

    // 调用 TraceRay
    // uint payload = 1;  // 1 = 未命中(可见), 0 = 命中(被遮挡)
    // TraceRay(sceneTLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 0, 0, 0, ray, payload);

    // 写入输出纹理
    // float visibility = float(payload);
    // outShadowMask[idx] = visibility;
}
