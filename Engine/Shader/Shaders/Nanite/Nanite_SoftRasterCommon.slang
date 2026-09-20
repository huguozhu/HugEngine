// ============================================================
// Nanite/Nanite_SoftRasterCommon.slang — 软光栅的**共享**绑定 / 参数 / 光栅化内联函数（仅供 include）
//
// 【这不是 shader 入口】被下面两个入口 include（都已登记进 COMP_SLANG）：
//   · Nanite_SoftRasterDepth.comp.slang —— 第 1 趟：只做**原子深度键**（不碰任何 GBuffer 颜色）
//   · Nanite_SoftRaster.comp.slang      —— 第 2 趟：重跑光栅化 + 等值复检 ⇒ 写 GBuffer 颜色
// 本文件自身不进任何编译列表，只进 `Engine/Shader/CMakeLists.txt` 的 SLANG_INCLUDES。
//
// 【为什么是"两趟原子深度 + 等值复检"，而不是一趟 + interlock（重要，有实测证据）】
//   设计 §5.2 的原文是"每个 cluster 一个 wave，interlock 写 GBuffer"，即用 Slang/HLSL 的
//   `RasterizerOrderedTexture2D`（ROV）拿到逐像素互锁。**本仓库的 Slang 版本做不到**：
//   Slang 2026.13 对 compute 入口里的 `RasterizerOrderedTexture2D` **静默降级**成普通
//   `RWTexture2D`（exit 0、`-warnings-as-errors all` 下零诊断、SPIR-V 里既没有
//   `OpBeginInvocationInterlockEXT` 也没有任何 `OpExtension`），而且 SPIR-V 规范本身规定
//   interlock 的 execution mode 只对 **Fragment** 入口合法（手工汇编后 `spirv-val` 直接报
//   "Execution mode can only be used with the Fragment execution model."）。
//   ⇒ 单趟写法（"InterlockedMin 深度 → 若自己更近就写 GBuffer"）**有竞态**：
//     更近的三角形原子胜出后写颜色，更远的那个的**颜色写入可能后落地**，最终颜色属于更远的三角形。
//   本实现改成**两趟**，用零额外设备特性拿到正确的最终像素：
//     第 1 趟：`InterlockedMin(u_DepthKey[p], key)`，只写深度键，**不碰 GBuffer**；
//     第 2 趟：每个三角形重算自己的 `key`，与 `u_DepthKey[p]` **等值复检**，相等才写 GBuffer
//              ⇒ 每个像素恰好一个三角形写颜色（平局由低 8 位的三角形下标给出全序）。
//   两趟之间由命令缓冲里的一条 storage-image 屏障定序（见 `NaniteRaster::RecordSoftRasterPass`）。
//
// 【深度键（R32_UINT）的编码】`key = (asuint(ndcZ) & 0xFFFFFF00u) | (triLocal & 0xFFu)`
//   · 高 24 位 = NDC 深度（Vulkan `[0,1]`，近=0）的**浮点位模式**：非负浮点的位模式与无符号
//     整数**同序** ⇒ 直接 `InterlockedMin` 就是"取最近"（与 Lumen 的 `InterlockedMin(asuint())`
//     同一惯用法，见 SDF_MeshScatter.comp.slang 的注释）；截掉低 8 位只损失 256 ulp 的深度精度。
//   · 低 8 位 = 簇内三角形下标（0..63，占 6 位）：把"深度恰好相同"的平局变成**全序**，
//     于是第 2 趟的等值复检是确定性的。
//   · 哨兵 `0xFFFFFFFF` = 该像素没有几何（第 1 趟前由模块清零）。真实键恒 < 0x3F800000|0xFF
//     （NDC 深度 ≤ 1.0）⇒ 与哨兵不可能撞上。
//
// 【§14.3 依赖禁令】只依赖本模块自己的 POD（`NaniteTypes.slang`）与既有 GBuffer 契约，
//   不引用 GI/Lumen/GPUCulling。
// ============================================================

#pragma once

#include "NaniteTypes.slang"

// 一条可见簇引用（8B；与 `NaniteVisibleClusterRef` 逐字段一致）
struct ClusterRef {
    uint instance;
    uint cluster;
};

// 实例表：GPUSceneObject（128B）契约的镜像。软光栅只用 `ltw3.xyz`（列主序平移）。
struct InstanceObject {
    float4 ltw0;
    float4 ltw1;
    float4 ltw2;
    float4 ltw3;              // 偏移 48：xyz = 平移（列主序矩阵的第 4 列）
    float4 boundsMin;
    float4 boundsMax;
    uint   meshIndex;
    uint   materialIndex;
    uint   objectID;
    uint   visibilityFlags;
    uint   indexCount;
    uint   firstIndex;
    int    vertexOffset;
    uint   _pad;
};

// ── 描述符绑定（set=0；顺序与 `NaniteRaster::EnsureSoftRasterResources` 的布局逐条对应）──
//   0 = 簇记录（只读；`NaniteCluster` 64B）
//   1 = 量化顶点（只读；`NaniteVertex` 16B）
//   2 = 打包三角形（只读；`NanitePackedTriangle` 8B）
//   3 = 可见簇引用（只读；`ClusterRef` 8B，(instance, cluster)）
//   4 = 可见簇计数（只读；单个 uint，由剔除链在同一帧写出）
//   5 = 实例表（只读；128B 契约，只用平移列）
//   6 = 深度键（读写；`RWStructuredBuffer<uint>`，长度 = W×H，行主序：index = y*W + x）
//   7 = 读数（读写；扁平 u32，槽位见下面的 kSoftStat*）
[[vk::binding(0, 0)]] StructuredBuffer<NaniteCluster>        u_Clusters;
[[vk::binding(1, 0)]] StructuredBuffer<NaniteVertex>         u_Vertices;
[[vk::binding(2, 0)]] StructuredBuffer<NanitePackedTriangle> u_Triangles;
[[vk::binding(3, 0)]] StructuredBuffer<ClusterRef>           u_VisibleClusters;
[[vk::binding(4, 0)]] StructuredBuffer<uint>                 u_VisibleClusterCount;
[[vk::binding(5, 0)]] StructuredBuffer<InstanceObject>       u_Instances;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint>               u_DepthKey;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint>               u_Stats;

// ── 读数槽位（与 C++ 侧 `NaniteSoftRasterStats` 的 static_assert 逐条对应）──
static const uint kSoftStatRasterClusters  = 0u;   // 本帧真正走软光栅的簇数（triangleCount <= maxTriangles）
static const uint kSoftStatSkippedClusters = 1u;   // 因 triangleCount > maxTriangles 跳过的簇数（留给任务 22）
static const uint kSoftStatTriangles       = 2u;   // 参与光栅化的**非退化**三角形数（第 1 趟）
static const uint kSoftStatDegenerate      = 3u;   // 被丢弃的三角形数（在相机后 / 面积过小 / 越界下标）
static const uint kSoftStatPixels          = 4u;   // 通过深度等值复检、**真正写进 GBuffer** 的像素数（第 2 趟）
static const uint kSoftStatNeutralPixels   = 5u;   // 其中用**中性材质常数**写入的像素数（资产材质段为空）
static const uint kSoftStatCapacity        = 16u;  // 缓冲条数（与 C++ 侧一致）
// ── 诊断槽位（第 1 趟的工作组 0 写入；把"shader 实际收到的 push constant"读回来）──
static const uint kSoftStatCoveredPixels   = 6u;   // 第 1 趟通过重心覆盖测试的像素数（未做原子）
static const uint kSoftStatDiagScreenW     = 7u;   // push constant 的 screenWidth（原样回读）
static const uint kSoftStatDiagScreenH     = 8u;   // push constant 的 screenHeight
static const uint kSoftStatDiagMaxTri      = 9u;   // push constant 的 maxTriangles
static const uint kSoftStatDiagExtent      = 10u;  // meshMaxExtent × 1000（定点回读，验证非 0）
static const uint kSoftStatTestedPixels    = 11u;  // 第 1 趟包围盒内的像素测试数（诊断）

// ── push constant（96B；与 C++ 的 `NaniteSoftRasterParams` 逐字段一致）──
//   vpRows  = 列主序 viewProj 的 4 个**行**（`vpRow{r}` = viewProj[c][r]），显式点积 ⇒ 与 CPU 同式
//             （与任务 15 的 `CullChainParams` 同一份比特、同一手法）
//   maxTriangles = 只对"triangleCount ≤ 该值"的簇走软光栅（§5.2 的 16；可配 `nanite_soft_max_triangles`）
[[vk::push_constant]] cbuffer NaniteSoftRasterParams {
    float4 vpRow0;            // 偏移 0
    float4 vpRow1;            // 偏移 16
    float4 vpRow2;            // 偏移 32
    float4 vpRow3;            // 偏移 48
    uint   screenWidth;       // 偏移 64
    uint   screenHeight;      // 偏移 68
    uint   maxTriangles;      // 偏移 72
    uint   instanceCount;     // 偏移 76：实例域上界（越界引用直接跳过，不读实例表）
    float  meshMaxExtent;     // 偏移 80：位置量化尺度（整网格最大轴长）
    float  depthKeyEpsilon;   // 偏移 84：第 2 趟等值复检的容差（0 = 严格逐位相等；实测严格即可）
    float  _pad0;             // 偏移 88
    float  _pad1;             // 偏移 92
};

// 世界坐标 → 裁剪空间（与 CPU 侧 `NaniteProjectPoint` / 任务 15 的 Hi-Z 投影同一表达式）
float4 softRasterProjectWorld(float3 worldPos) {
    float4 hp = float4(worldPos, 1.0);
    return float4(dot(vpRow0, hp), dot(vpRow1, hp), dot(vpRow2, hp), dot(vpRow3, hp));
}

// NDC → 帧缓冲像素坐标。
// 【y 的方向】**本引擎用 `glm::perspectiveRH_ZO`（NDC y 向上）**，而 Vulkan 的帧缓冲 y 向下；
//   引擎的离屏通道统一用**负高度视口**抵消（见 `NaniteRaster::RecordRasterPass` 的
//   `SetViewport({..., -height, ...})`）⇒ NDC y = +1 落在**帧缓冲第 0 行**。故：
//     row = (0.5 - 0.5 × ndc.y) × H，col = (0.5 + 0.5 × ndc.x) × W
//   （与剔除链的 `s = ndc.xy*0.5+0.5` 相比，y 是镜像的 —— 那一处是采样 Hi-Z 纹理的 UV，
//    属任务 15 的口径，不在本次改动面内，已在实施记录里作为观察项记下。）
float2 softRasterNdcToPixel(float2 ndc) {
    return float2((0.5 + 0.5 * ndc.x) * (float)screenWidth,
                  (0.5 - 0.5 * ndc.y) * (float)screenHeight);
}

// 深度键：高 24 位 = NDC 深度位模式、低 8 位 = 三角形下标（平局全序）
uint softRasterDepthKey(float ndcZ, uint triLocal) {
    return (asuint(ndcZ) & 0xFFFFFF00u) | (triLocal & 0xFFu);
}

// 一个三角形的屏幕空间形态（第 1/2 趟共用；两趟必须逐位一致才谈得上等值复检）
struct SoftRasterTriangle {
    float2 p0; float2 p1; float2 p2;   // 帧缓冲像素坐标
    float  z0; float  z1; float  z2;   // NDC 深度（Vulkan [0,1]，近 = 0）
    float  invW0;                      // 透视校正用的 1/w（下面三个字段按顶点展开）
    float  invW1;
    float  invW2;
    bool   valid;
};

// 取簇内第 `triLocal` 个三角形的屏幕形态。
// 【有效性判据（两趟一致）】三个顶点的 clip.w 都必须 > 0（跨越相机平面的三角形直接丢弃 ——
//   简单、可复现，不做近平面裁剪）；屏幕面积过小（共线/退化）也丢弃，避免除零与无意义的遍历。
SoftRasterTriangle softRasterFetchTriangle(NaniteCluster cluster, uint triLocal, float3 instanceTranslation) {
    SoftRasterTriangle tri;
    tri.valid = false;
    tri.p0 = tri.p1 = tri.p2 = float2(0.0, 0.0);
    tri.z0 = tri.z1 = tri.z2 = 0.0;
    tri.invW0 = tri.invW1 = tri.invW2 = 0.0;

    NanitePackedTriangle packed = u_Triangles[cluster.triangleOffset + triLocal];
    uint3 local = naniteDecodeTriangle(packed);
    uint base = cluster.vertexOffset;

    float3 world[3];
    float4 clip[3];
    for (uint k = 0; k < 3u; ++k) {
        uint li = (k == 0u) ? local.x : ((k == 1u) ? local.y : local.z);
        if (!naniteIsValidClusterLocalVertexIndex(li)) return tri;
        NaniteVertex v = u_Vertices[base + li];
        // 位置：簇内局部量化 + **本簇球心** + 网格最大范围（§14.19 硬约束①）+ 实例平移
        float3 meshPos = naniteDecodeVertexPosition(v, cluster, meshMaxExtent);
        world[k] = meshPos + instanceTranslation;
        clip[k] = softRasterProjectWorld(world[k]);
        if (!(clip[k].w > 1.0e-6)) return tri;          // 跨越/落在相机平面后 ⇒ 丢弃
    }

    float3 ndc0 = clip[0].xyz / clip[0].w;
    float3 ndc1 = clip[1].xyz / clip[1].w;
    float3 ndc2 = clip[2].xyz / clip[2].w;
    if (any(isnan(ndc0)) || any(isnan(ndc1)) || any(isnan(ndc2))) return tri;

    tri.p0 = softRasterNdcToPixel(ndc0.xy);
    tri.p1 = softRasterNdcToPixel(ndc1.xy);
    tri.p2 = softRasterNdcToPixel(ndc2.xy);
    tri.z0 = ndc0.z; tri.z1 = ndc1.z; tri.z2 = ndc2.z;
    tri.invW0 = 1.0 / clip[0].w;
    tri.invW1 = 1.0 / clip[1].w;
    tri.invW2 = 1.0 / clip[2].w;

    // 有向面积（像素²）：|area| 太小的三角形在屏幕上是退化的 ⇒ 丢弃（与两趟一致）
    float area2 = (tri.p1.x - tri.p0.x) * (tri.p2.y - tri.p0.y)
                - (tri.p2.x - tri.p0.x) * (tri.p1.y - tri.p0.y);
    if (!(abs(area2) > 1.0e-8)) return tri;

    tri.valid = true;
    return tri;
}

// 某个像素是否落在三角形内（重心坐标的三个边函数都 ≥ 0，**顺序无关** ⇒ 双面光栅化，
// 与既有 GBuffer 路径默认的双面行为一致；`RasterizerState.cullMode` 在模块内的软光栅里不适用）。
// 【为什么加一个极小容差】浮点边函数在像素中心恰好落在线上的情形会因舍入而抖动；
//   同一个容差在两趟里是同一常量 ⇒ 覆盖集合逐位一致（等值复检的前提）。
bool softRasterCovered(SoftRasterTriangle tri, float2 p, out float3 bary) {
    float2 e0 = tri.p1 - tri.p0;
    float2 e1 = tri.p2 - tri.p1;
    float2 e2 = tri.p0 - tri.p2;
    // 【符号约定必须与下面的 area2 一致】area2 = cross(p1-p0, p2-p0) = e.x*b.y - e.y*b.x；
    //   子面积于是写成 `e.x*(p.y-pk.y) - e.y*(p.x-pk.x)`（**顺序不能反**：写反会让子面积与总面积
    //   差一个负号，结果"三角形内部的三个边函数全为负"⇒ 一个像素都覆盖不到。实测踩过：
    //   61 个三角形、9767 次像素测试、0 次覆盖）。
    float a0 = e0.x * (p.y - tri.p0.y) - e0.y * (p.x - tri.p0.x);
    float a1 = e1.x * (p.y - tri.p1.y) - e1.y * (p.x - tri.p1.x);
    float a2 = e2.x * (p.y - tri.p2.y) - e2.y * (p.x - tri.p2.x);
    // 【双面光栅化：必须按面积符号翻转】边函数带**手性** —— 屏幕空间里顺时针绕序的三角形，
    //   三个边函数在三角形内部**全为负**；不翻转就会一个像素都不覆盖（实测：61 个三角形、
    //   0 个像素）。统一乘上 `sign(有向面积)` 后，判据只依赖"点在不在三角形里"，与绕序无关。
    float area2 = (tri.p1.x - tri.p0.x) * (tri.p2.y - tri.p0.y)
                - (tri.p2.x - tri.p0.x) * (tri.p1.y - tri.p0.y);
    float flip = (area2 < 0.0) ? -1.0 : 1.0;
    a0 *= flip;
    a1 *= flip;
    a2 *= flip;
    const float kEdgeTolerance = -1.0e-4;
    if (a0 < kEdgeTolerance || a1 < kEdgeTolerance || a2 < kEdgeTolerance) {
        bary = float3(0.0, 0.0, 0.0);
        return false;
    }
    float sum = a0 + a1 + a2;
    if (!(abs(sum) > 1.0e-12)) { bary = float3(0.0, 0.0, 0.0); return false; }
    // 面积权重：a1 对顶点 0 的权重、a2 对顶点 1、a0 对顶点 2（边函数与对角顶点的对应关系）
    bary = float3(a1 / sum, a2 / sum, a0 / sum);
    return true;
}

// 透视校正插值：属性 = Σ(w_k × attr_k) / Σ(w_k)，其中 w_k = bary_k / clipW_k
// （平面深度不需要透视校正 —— NDC z 是屏幕坐标的线性函数，直接 Σ(bary × z) 即可）
float3 softRasterAttribute(float3 bary, SoftRasterTriangle tri, float3 a0, float3 a1, float3 a2) {
    float w0 = bary.x * tri.invW0;
    float w1 = bary.y * tri.invW1;
    float w2 = bary.z * tri.invW2;
    float sum = w0 + w1 + w2;
    if (!(abs(sum) > 1.0e-20)) return a0;
    return (a0 * w0 + a1 * w1 + a2 * w2) / sum;
}
