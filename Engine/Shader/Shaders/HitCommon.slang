// HitCommon.slang — 命中点查询辅助（**零绑定**：所需的纹理/bindless 数组由调用方作为参数传入）
//
// 【为什么收敛】以下两个函数此前在两组文件里各有一份**逐字相同**的实现：
//   · `LoadHitNormal`       —— `RT_HitCommon.slang` 与 `PathTracing/PT_Full.rchit.slang`
//   · `SampleMaterialColor` —— `RayTracing/RT_PBR.rchit.slang` 与 `RT_PositionFetch.rchit.slang`
// 它们的资源本来分散在各自文件里（RT 命中路径 binding 4/6、PT 命中路径 set1、顶点拉取路径
// set1），所以无法共用一个"声明了绑定"的头。改为把资源作为**参数**传入即可零绑定共享
// （Slang 允许把 `Texture2D` 甚至无界数组 `Texture2D<...>[]` 作为函数参数 —— 实测通过）。
#ifndef HUGENGINE_HIT_COMMON_SLANG
#define HUGENGINE_HIT_COMMON_SLANG

// 平滑法线：材质纹理 row2 取出该实例的三角形法线线性起始与纹理宽度，
// 再按 3 个顶点法线做重心插值，最后按入射方向翻正。
float3 LoadHitNormal(Texture2D<float4> sceneMaterials,   // 场景材质纹理（原先靠各文件自己的绑定）
                     Texture2D<float4> triNormals,       // 三角形顶点法线纹理
                     uint instanceID, uint primId, float2 bary) {
    float4 row2 = sceneMaterials.Load(int3(instanceID, 2, 0));
    uint linBase = uint(row2.x);      // 本实例法线数组线性起始
    uint texW    = uint(row2.z);      // 法线纹理宽度（lin%W → 列）

    // 读取三角形 3 个顶点法线
    float3 n0 = triNormals.Load(int3((linBase + primId * 3 + 0) % texW,
                                             (linBase + primId * 3 + 0) / texW, 0)).xyz;
    float3 n1 = triNormals.Load(int3((linBase + primId * 3 + 1) % texW,
                                             (linBase + primId * 3 + 1) / texW, 0)).xyz;
    float3 n2 = triNormals.Load(int3((linBase + primId * 3 + 2) % texW,
                                             (linBase + primId * 3 + 2) / texW, 0)).xyz;

    // 重心插值（顶点 0 权重 = 1 - x - y）
    float3 N = normalize(n0 * (1.0 - bary.x - bary.y) + n1 * bary.x + n2 * bary.y);

    // 确保法线朝向光线入射方向（背面命中 → 翻转）
    if (dot(N, -WorldRayDirection()) < 0.0) N = -N;
    return N;
}

// 材质颜色：`matData.w` 存 bindless 纹理索引（0 = 无纹理，用 matData.rgb 作为常量色）。
float3 SampleMaterialColor(Texture2D<float4> textures[],   // bindless 纹理数组（调用方传入）
                           SamplerState sampler, float4 matData, float2 uv) {
    uint texId = uint(matData.w + 0.5);  // 四舍五入
    if (texId > 0) {
        // 有 bindless 纹理：使用 UV 采样
        uint idx = NonUniformResourceIndex(texId - 1);
        return textures[idx].Sample(sampler, uv).rgb;
    }
    // 无 bindless 纹理：使用材质固有色
    return matData.rgb;
}

#endif // HUGENGINE_HIT_COMMON_SLANG
