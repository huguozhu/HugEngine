// ============================================================
// Nanite_SoftRaster.comp.slang — 软光栅**第 2 趟**：等值复检后写 GBuffer（任务 18 的几何写入者）
//
// 【本文件就是设计 §5.2 说的那一趟】逐三角形投影到屏幕 → 2D 包围盒 → 重心坐标 → 深度测试
//   → 写 GBuffer。**唯一的差别**是"深度测试 + 互锁"的落地方式：
//     · 设计原文：`InterlockedMin(深度)` 之后**紧接着**写 GBuffer（单趟 + ROV/interlock）；
//     · 本实现：深度已在**第 1 趟**（`Nanite_SoftRasterDepth.comp.slang`）原子定型，本趟对每个
//       像素做 `u_DepthKey[p] == 自己的 key` 的**等值复检**，相等才写 —— 于是每个像素恰好被
//       一个三角形写一次，多张 GBuffer 之间天然一致（不需要任何互锁）。
//   为什么不能用单趟 + ROV：见 `Nanite_SoftRasterCommon.slang` 顶部的实测证据（本仓库的
//   Slang 在 compute 里静默丢弃 ROV 语义；SPIR-V 的 interlock 只对 Fragment 入口合法）。
//
// 【写哪些通道】albedo（MRT0：rgb = 反照率、a = metallic）、normal（MRT1：xyz = 世界法线、
//   a = roughness）、worldPos（MRT4：xyz = 世界坐标）、lightmapKey（MRT7：uv.xy + 页号 + 0）。
//   · 材质：资产的材质段**为空**（任务 12/19 才解析材质）⇒ 用**中性材质常数**
//     `albedo = (0.8,0.8,0.8)`、`metallic = 0`、`roughness = 0.5`，并把"用中性常数的像素数"
//     记进 `kSoftStatNeutralPixels`（读数里如实标出，不假装是真材质）。
//   · 页号：`kbNaniteObjectIndexBegin + 实例下标`（objectIndex 分区契约的 Nanite 段
//     `[1024, 2048)`，见 `NaniteTypes.h`）—— 这是"混排场景运行时校验"的第一个真实生产者，
//     离线工具 `Tools/gi/lightmap_key_check.py` 的按段分类正为它而改。
//   · **不写** depth/velocity/emissive/disneyA/disneyB：深度走既有附件路径（由
//     `Nanite_DepthResolve.frag.slang` 从深度键还原后写 `SV_Depth`），其余通道由 GB_Clear
//     的清屏值承担（本任务的明确边界，见实施记录）。
//
// 【§14.3 依赖禁令】只依赖 `NaniteTypes.slang` 与既有 GBuffer 契约。
// ============================================================

#include "Nanite_SoftRasterCommon.slang"

// objectIndex 分区：Nanite 段起点（与 `NaniteTypes.h` 的 `kNaniteObjectIndexBegin` 一致）
static const uint kNaniteObjectIndexBegin = 1024u;

// GBuffer 颜色目标（RGBA16F；绑定顺序与 `NaniteRaster::EnsureSoftRasterResources` 一致）
[[vk::binding(8, 0)]]  [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_Albedo;      // MRT0
[[vk::binding(9, 0)]]  [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_Normal;      // MRT1
[[vk::binding(10, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_WorldPos;    // MRT4
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_LightmapKey; // MRT7

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    // ── ① 与第 1 趟**逐字相同**的取簇逻辑（这是等值复检成立的前提）──
    uint visibleCount = u_VisibleClusterCount[0];
    uint slot = gid.x;
    if (slot >= visibleCount) return;

    ClusterRef ref = u_VisibleClusters[slot];
    if (ref.instance >= instanceCount) return;
    if (pagesEnabled != 0u) {
        // 【任务 24】与第 1 趟逐字相同的收口口径（见 `Nanite_SoftRasterDepth.comp.slang`）
        if (ref.cluster >= assetClusterCount) return;
    } else {
        uint clusterCapacity = 0u;
        uint clusterStride = 0u;
        u_Clusters.GetDimensions(clusterCapacity, clusterStride);
        if (ref.cluster >= clusterCapacity) return;
    }

    InstanceObject instance = u_Instances[ref.instance];
    if (instance.indexCount == 0u) return;
    float3 translation = instance.ltw3.xyz;

    // ── 【任务 24】与第 1 趟**逐字相同**的页解析（这是等值复检成立的前提）──
    //   缺页的簇在第 1 趟没有写过深度键 ⇒ 本趟也必须跳过它，否则会写出"没有任何深度键与之匹配"
    //   的颜色（等值复检会拦住它，但白跑一遍包围盒）。两趟口径一致 ⇒ 结果与关闭档逐位可比。
    SoftRasterClusterView view = softRasterResolveCluster(ref.cluster);
    if (!view.valid) return;
    NaniteCluster cluster = view.cluster;
    uint triangleCount = cluster.triangleCount;
    if (triangleCount == 0u) return;
    if (triangleCount > maxTriangles) return;    // 与第 1 趟同一条跳过规则（计数只在第 1 趟做）

    // ── 【任务 19】真实材质：从**资产材质段**取（`cluster.materialID` 是任务 19 的
    //    "三角形多数票"映射写下的材质下标）。材质段为空 / 下标越界 ⇒ 中性常数兜底并单独计数。
    //    albedo/metallic/roughness 的公式与既有 GBuffer 路径逐句相同（见 softRasterEvaluateMaterial）。

    // ── ② 逐三角形：重跑光栅化 → 等值复检 → 写 GBuffer ──
    for (uint triLocal = tid.x; triLocal < triangleCount; triLocal += 16u) {
        SoftRasterTriangle tri = softRasterFetchTriangle(view, triLocal, translation);
        if (!tri.valid) continue;

        // 三个顶点的属性（法线 / UV 按**簇内局部下标**取 —— 任务 18 的 P0 修复保证了
        // "共享内容 ⇒ 逐下标属性逐位一致"，因此这里按局部下标取法线/UV 是安全的）
        // 【任务 24】取址同样经 `view` 的两段基址（关闭档 = 资产段，开启档 = 页池段）。
        NanitePackedTriangle packed = u_Triangles[view.triangleBase + triLocal];
        uint3 local = naniteDecodeTriangle(packed);
        NaniteVertex v0 = u_Vertices[view.vertexBase + local.x];
        NaniteVertex v1 = u_Vertices[view.vertexBase + local.y];
        NaniteVertex v2 = u_Vertices[view.vertexBase + local.z];

        float3 n0 = naniteDecodeNormal(v0.packedNormal);
        float3 n1 = naniteDecodeNormal(v1.packedNormal);
        float3 n2 = naniteDecodeNormal(v2.packedNormal);
        float2 t0 = naniteDecodeUV(v0.packedUV);
        float2 t1 = naniteDecodeUV(v1.packedUV);
        float2 t2 = naniteDecodeUV(v2.packedUV);

        float2 lo = min(min(tri.p0, tri.p1), tri.p2);
        float2 hi = max(max(tri.p0, tri.p1), tri.p2);
        int2 bmin = int2(clamp(floor(lo), float2(0.0, 0.0),
                               float2((float)screenWidth - 1.0, (float)screenHeight - 1.0)));
        int2 bmax = int2(clamp(ceil(hi), float2(0.0, 0.0),
                               float2((float)screenWidth - 1.0, (float)screenHeight - 1.0)));

        for (int py = bmin.y; py <= bmax.y; ++py) {
            for (int px = bmin.x; px <= bmax.x; ++px) {
                float2 pixelCenter = float2((float)px + 0.5, (float)py + 0.5);
                float3 bary;
                if (!softRasterCovered(tri, pixelCenter, bary)) continue;

                float ndcZ = bary.x * tri.z0 + bary.y * tri.z1 + bary.z * tri.z2;
                uint key = softRasterDepthKey(ndcZ, triLocal);

                // ── ③ 深度等值复检（两趟之间唯一的同步点，替代 ROV/interlock）──
                uint pixelIndex = (uint)py * screenWidth + (uint)px;
                if (u_DepthKey[pixelIndex] != key) continue;

                // ── ④ 透视校正插值 → 写 GBuffer ──
                float3 worldPos = softRasterAttribute(bary, tri,
                    naniteDecodeVertexPosition(v0, cluster, meshMaxExtent) + translation,
                    naniteDecodeVertexPosition(v1, cluster, meshMaxExtent) + translation,
                    naniteDecodeVertexPosition(v2, cluster, meshMaxExtent) + translation);
                float3 normal = normalize(softRasterAttribute(bary, tri, n0, n1, n2));
                float2 uv = softRasterAttribute(bary, tri, float3(t0, 0.0), float3(t1, 0.0),
                                                float3(t2, 0.0)).xy;

                // ── 【任务 19】真实材质（与既有 GBuffer 路径同一批来源 + 同一组公式）──
                float3 matAlbedo;
                float  matMetallic;
                float  matRoughness;
                const bool materialFromAsset =
                    softRasterEvaluateMaterial(cluster.materialID, uv,
                                               matAlbedo, matMetallic, matRoughness);

                u_Albedo[uint2((uint)px, (uint)py)]      = float4(matAlbedo, matMetallic);
                u_Normal[uint2((uint)px, (uint)py)]      = float4(normal, matRoughness);
                u_WorldPos[uint2((uint)px, (uint)py)]    = float4(worldPos, 1.0);
                u_LightmapKey[uint2((uint)px, (uint)py)] = float4(uv, (float)(kNaniteObjectIndexBegin + ref.instance), 0.0);
                InterlockedAdd(u_Stats[kSoftStatPixels], 1u);
                // 【读数分离】`material_pixels` = 真的从资产材质段取到材质的像素；
                //   `fallback_pixels` = 材质段缺失/越界的像素（正常必须 0）；
                //   `neutral_material_pixels`（任务 18 的中性常数计数）**恒为 0** —— 不再写中性常数。
                if (materialFromAsset) {
                    InterlockedAdd(u_Stats[kSoftStatMaterialPixels], 1u);
                } else {
                    InterlockedAdd(u_Stats[kSoftStatFallbackPixels], 1u);
                }
            }
        }
    }
}
