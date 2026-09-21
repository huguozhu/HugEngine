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
//   8..11 = 第 2 趟专属：4 张 GBuffer 颜色目标（albedo/normal/worldPos/lightmapKey，见入口文件）
//  12 = 【任务 19】资产材质段（只读；`NaniteMaterial` 32B/条，与 §8 的任务 19 定稿一致）
//  13 = 【任务 19】bindless 纹理数组（`Texture2D<float4>[]`；与既有 GBuffer 路径**同一个堆**）
//  14 = 【任务 19】bindless 采样器数组（与纹理槽位一一对应）
[[vk::binding(0, 0)]] StructuredBuffer<NaniteCluster>        u_Clusters;
[[vk::binding(1, 0)]] StructuredBuffer<NaniteVertex>         u_Vertices;
[[vk::binding(2, 0)]] StructuredBuffer<NanitePackedTriangle> u_Triangles;
[[vk::binding(3, 0)]] StructuredBuffer<ClusterRef>           u_VisibleClusters;
[[vk::binding(4, 0)]] StructuredBuffer<uint>                 u_VisibleClusterCount;
[[vk::binding(5, 0)]] StructuredBuffer<InstanceObject>       u_Instances;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint>               u_DepthKey;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint>               u_Stats;
[[vk::binding(12, 0)]] StructuredBuffer<NaniteMaterialRecord> u_Materials;
[[vk::binding(13, 0)]] Texture2D<float4>                     u_MaterialTextures[];
[[vk::binding(14, 0)]] SamplerState                          u_MaterialSamplers[];

// ── 【任务 24】LOD 流式的五 + 一个视图（bindings 15..20）──
// 【口径：0/1/2 号槽一槽两用】流式开启时，binding 0/1/2 绑的是**页池**的簇段/顶点段/三角形段，
//   关闭时绑的仍是资产段。两种档位共用下面**同一段取值表达式**（`softRasterResolveCluster`
//   只在开启档多做一次页表查表 + 偏移减法）⇒ "间接层算错没算错"这件事有一条**逐位可比**的
//   判据：页池足够大时两档的四个 GBuffer 必须一致。
// 【为什么 15..20 在关闭档也要绑】描述符集布局是建 PSO 时固定的；关闭档把它们绑到**既有**
//   资产缓冲上作占位（着色器在该档一个字节都不读），因此不新建任何 GPU 资源（§14.2 不变式 1）。
[[vk::binding(15, 0)]] StructuredBuffer<NaniteClusterPageRef> u_ClusterPage;
[[vk::binding(16, 0)]] StructuredBuffer<NanitePageEntry>      u_PageTable;
[[vk::binding(17, 0)]] StructuredBuffer<NaniteCluster>        u_PoolClusters;
[[vk::binding(18, 0)]] StructuredBuffer<NaniteVertex>         u_PoolVertices;
[[vk::binding(19, 0)]] StructuredBuffer<NanitePackedTriangle> u_PoolTriangles;
// 反馈环（读写）：[0] = 本帧写入的请求条数（原子取号）、[1] = 环满丢弃数（原子）、
// [2, 2 + kNanitePageRequestSlots) = 环形缓冲本体。**GPU 只追加，去重在 CPU 读回侧做**。
[[vk::binding(20, 0)]] RWStructuredBuffer<uint>               u_PageFeedback;

// ── 读数槽位（与 C++ 侧 `NaniteSoftRasterStats` 的 static_assert 逐条对应）──
static const uint kSoftStatRasterClusters  = 0u;   // 本帧真正走软光栅的簇数（triangleCount <= maxTriangles）
static const uint kSoftStatSkippedClusters = 1u;   // 因 triangleCount > maxTriangles 跳过的簇数（留给任务 22）
static const uint kSoftStatTriangles       = 2u;   // 参与光栅化的**非退化**三角形数（第 1 趟）
static const uint kSoftStatDegenerate      = 3u;   // 被丢弃的三角形数（在相机后 / 面积过小 / 越界下标）
static const uint kSoftStatPixels          = 4u;   // 通过深度等值复检、**真正写进 GBuffer** 的像素数（第 2 趟）
static const uint kSoftStatNeutralPixels   = 5u;   // 【任务 19 起恒 0】用中性材质常数写入的像素数
// 【缓冲条数 kSoftStatCapacity 的唯一权威定义在下面「任务 24」小节（= 22）】
//   —— 它的位置必须跟着**最后一个槽**走（任务 23 时是 20、任务 24 起是 22）；
//   本行原来是任务 23 的定义，已上移合并，避免同作用域重定义。
// ── 【任务 19】材质相关读数 ──
static const uint kSoftStatMaterialPixels  = 12u;  // 用**资产材质段**的真实材质写入的像素数
static const uint kSoftStatFallbackPixels  = 13u;  // 材质段越界/缺失而退化为中性常数的像素数（正常 0）
// ── 【P0 修复（§14.30）】深度解析读数 ──
// 【为什么加这一项】过去 `depth_written` 在 C++ 侧是**硬编码的 1**（"深度解析恒执行"），
//   于是"深度解析其实一列都没写进去"这个真 bug 被一个恒真读数掩盖了整整一个任务。
//   现在由深度解析通道**自己原子计数**：只有"读到非哨兵深度键"的像素才 +1
//   ⇒ 它是真实 GPU 读回。语义是**去重后的像素数**（全屏片元对每个像素只访问一次），
//   与第 2 趟的 `kSoftStatPixels` 不是同一个量：后者是"通过等值复检的
//   （簇×三角形×像素）写次数"，会因**键完全相同的平局**（任务 18 已记录的已知口径）
//   大于去重像素数。可核对的不变式是 `depthResolvedPixels <= pixels`。
static const uint kSoftStatDepthResolvedPixels = 14u;   // 深度解析真正写进非远平面深度的**像素数**（去重）
// ── 诊断槽位（第 1 趟的工作组 0 写入；把"shader 实际收到的 push constant"读回来）──
static const uint kSoftStatCoveredPixels   = 6u;   // 第 1 趟通过重心覆盖测试的像素数（未做原子）
static const uint kSoftStatDiagScreenW     = 7u;   // push constant 的 screenWidth（原样回读）
static const uint kSoftStatDiagScreenH     = 8u;   // push constant 的 screenHeight
static const uint kSoftStatDiagMaxTri      = 9u;   // push constant 的 maxTriangles
static const uint kSoftStatDiagExtent      = 10u;  // meshMaxExtent × 1000（定点回读，验证非 0）
static const uint kSoftStatTestedPixels    = 11u;  // 第 1 趟包围盒内的像素测试数（诊断）

// ── 【任务 23】可见簇的「簇大小分布」五桶（占任务 23 之前**唯一空闲**的槽 15 起连续 5 槽）──
// 【与 C++ 的互锁】下面三个常量必须与 `NaniteTypes.h` 的
//   `kNaniteSoftStatSizeBucket0 / kNaniteSoftStatSizeBucketCount / kNaniteSoftStatsCapacity`
//   一一对应（C++ 侧有 static_assert 钉住"紧跟 14 号槽 + 连续 + 吃满容量"；改一处必须同步另一处）。
// 【口径】统计**本帧全部可见簇**（通过合法性检查且 `triangleCount != 0`），按 `triangleCount`
//   落入 1-4 / 5-8 / 9-16 / 17-32 / 33-64 五个闭区间桶；计数发生在分流判据**之前**
//   ⇒ 五桶之和 == `kSoftStatRasterClusters + kSoftStatSkippedClusters`（可核对的不变式）。
static const uint kSoftStatSizeBucket0     = 15u;  // 五桶的第一个槽（= kSoftStatDepthResolvedPixels + 1）
static const uint kSoftStatSizeBucketCount = 5u;   // 桶数（1-4 / 5-8 / 9-16 / 17-32 / 33-64）

// ── 【任务 24】LOD 流式的两个读数槽（占 20/21；容量 20 → 22）──
// 【必须真的写，否则判据空洞】读数缓冲每帧由 C++ 用 `CopyBuffer` 清 0，若这里不写，C++ 读回就
//   恒为 0 ⇒ `page_misses == 0` 会在"流式完全没工作"时也成立（**空洞通过**）。
//   与 C++ 的 `kNaniteSoftStatPageMissClusters / kNaniteSoftStatPageRequests /
//   kNaniteSoftStatsCapacity` 一一对应（改一处必须同步另一处）。
static const uint kSoftStatPageMissClusters = 20u;  // 因所在页未驻留而被跳过的**可见簇数**
static const uint kSoftStatPageRequests     = 21u;  // 本帧写入反馈环的请求**条数**（含重复页）
static const uint kSoftStatCapacity         = 22u;  // 缓冲条数（与 C++ 的 kNaniteSoftStatsCapacity 必须相等）

// 反馈环的常量（与 C++ 的 `kNanitePageFeedbackRingOffset / kNanitePageRequestSlots /
//   kNanitePageFeedbackStampOffset` 一致）
static const uint kPageFeedbackRingOffset = 2u;     // 环从第 2 个 u32 开始（前两个是原子计数）
static const uint kPageRequestSlots       = 1024u;  // 环槽数（满则计入 [1] 号溢出计数，不静默）
static const uint kPageFeedbackStampOffset = kPageFeedbackRingOffset + kPageRequestSlots;  // 戳记数组起点

// 簇大小 → 桶号（调用方必须先滤掉 `triangleCount == 0`）。
// 【为什么写成 5 级阶梯而不是查表】Slang 侧没有编译期数组常量（`static const uint[kN]` 的
//   下标不能用于常量表达式），写成显式比较既零成本又与 C++ 侧的 `NaniteSizeBucketOf()`
//   逐分支等价 —— 单测对 1..64 全枚举比对两者。
static uint softRasterSizeBucket(uint triangleCount) {
    if (triangleCount <= 4u)  return 0u;   // 1-4
    if (triangleCount <= 8u)  return 1u;   // 5-8
    if (triangleCount <= 16u) return 2u;   // 9-16
    if (triangleCount <= 32u) return 3u;   // 17-32
    return 4u;                             // 33-64（> 64 只可能来自损坏资产：归入最后一桶，不越界）
}

// ── push constant（112B；与 C++ 的 `NaniteSoftRasterParams` 逐字段一致）──
//   任务 24 从 96B 扩到 112B：原偏移 92 的 `_pad1` 空洞改为 `pagesEnabled`，后面追加四个 u32
//   （三个池槽步长 + 资产簇数），既有 7 个字段的偏移与语义一个都没动。
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
    uint   materialCount;     // 偏移 88：【任务 19】资产材质段条数（0 ⇒ 退化为中性常数并计数）
    // ── 【任务 24】流式四个数（原偏移 92 是 `_pad1` 空洞，既有字段偏移一个都没动）──
    uint   pagesEnabled;      // 偏移 92：1 = 经页表从页池取几何；0 = 直读资产段（默认档）
    uint   clusterStride;     // 偏移 96：池内一个槽占多少条簇记录（= 每页最大簇数）
    uint   vertexStride;      // 偏移 100：池内一个槽占多少条量化顶点
    uint   triangleStride;    // 偏移 104：池内一个槽占多少条打包三角形
    uint   assetClusterCount; // 偏移 108：资产的簇出现总数（页映射的索引空间上界）
};

// ── 【任务 19】按**资产材质段** + bindless 纹理算一个像素的 albedo/metallic/roughness ──
// 【与既有 GBuffer 路径逐句同源】（`GBuffer.frag.slang:57-81` 的表达式）：
//     albedo    = baseColorFactor.rgb × Sample(BaseColor, uv).rgb               （无纹理槽 ⇒ ×1.0）
//     metallic  = metallicFactor      × Sample(MetallicRough, uv).b             （无纹理槽 ⇒ ×0.0）
//     roughness = clamp(roughnessFactor × Sample(MetallicRough, uv).g, 0.04, 1) （无纹理槽 ⇒ ×1.0）
// 这正是任务 19 验收"材质字段与既有 GBuffer 路径逐项可比"的实现口径：
//   字段来自同一批来源（MeshComponent 的 PBR 参数 + 同一个 bindless 纹理堆），公式逐句相同。
// 【材质段越界】`materialIndex >= materialCount` ⇒ 返回 false（调用方用中性常数兜底并单独计数，
//   不静默；正常路径下 `materialIndex` 由任务 19 的"三角形多数票"映射保证在界内）。
bool softRasterEvaluateMaterial(uint materialIndex, float2 uv,
                                out float3 outAlbedo, out float metallic, out float roughness) {
    outAlbedo = float3(0.8, 0.8, 0.8);
    metallic  = 0.0;
    roughness = 0.5;
    if (materialIndex >= materialCount) return false;   // 兜底：越界不读缓冲（防越界读）

    NaniteMaterialRecord material = u_Materials[materialIndex];
    outAlbedo = material.baseColorFactor.rgb;
    float mrG = 1.0;   // 无 MetallicRough 纹理：g（粗糙度）不调制
    float mrB = 0.0;   // 无 MetallicRough 纹理：b（金属度）为 0（与 GBuffer 路径的中性值一致）
    if ((material.textureMask & NANITE_TEXMASK_BASECOLOR) != 0u) {
        uint slot = material.bindlessTextureBase + NANITE_TEXSLOT_BASECOLOR;
        // 【为什么是 `SampleLevel(..., 0)` 而不是 `Sample`】compute 入口没有导数（quad）语义：
        //   `Sample` 需要 2×2 线程的派生组，而本入口是 `[numthreads(16,1,1)]`（Y=1 不是 2 的倍数）
        //   ⇒ Slang 直接报 E31210。改用显式 LOD 0（= 只采 mip0）。
        //   【如实偏差】既有 GBuffer 路径是片元里的 `Sample`（有隐式 LOD）⇒ 双方在"贴图有 mip 且
        //   像素覆盖率大于一个 texel"时可能取到不同 mip。本任务用 mip0 是为了可复现与零额外特性，
        //   已在实施记录里列为偏差（对 albedo 均值/相关系数的影响可测）。
        outAlbedo *= u_MaterialTextures[slot].SampleLevel(u_MaterialSamplers[slot], uv, 0.0).rgb;
    }
    if ((material.textureMask & NANITE_TEXMASK_METALLICROUGH) != 0u) {
        uint slot = material.bindlessTextureBase + NANITE_TEXSLOT_METALLICROUGH;
        float4 mr = u_MaterialTextures[slot].SampleLevel(u_MaterialSamplers[slot], uv, 0.0);
        mrG = mr.g;
        mrB = mr.b;
    }
    metallic  = material.metallicFactor * mrB;
    roughness = clamp(material.roughnessFactor * mrG, 0.04, 1.0);
    return true;
}

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
//   （剔除链的 Hi-Z 采样在 P0 修复后用的是同一条约定 `v = 0.5 - 0.5*ndc.y`，
//     见 `Nanite_ClusterBVH.comp.slang` 的 `hizOccluded` 与实施记录。）
float2 softRasterNdcToPixel(float2 ndc) {
    return float2((0.5 + 0.5 * ndc.x) * (float)screenWidth,
                  (0.5 - 0.5 * ndc.y) * (float)screenHeight);
}

// ============================================================
// 【任务 24】页池取址：一个"已解析"的簇视图
//
// 【为什么要有这一层】流式的间接层只有一个职责：把"共享数组的绝对偏移"换算成"页池内的偏移"。
//   软光栅第 1/2 趟与硬光栅都经过**同一个**函数拿地址 ⇒ 三条路径不可能各算一套
//   （§14.32 第二处追加点名的 8 处侵入点，在这里被收成 1 处 + 3 个调用点）。
// 【关闭档逐位不变】`pagesEnabled == 0` 时本函数返回的 `vertexBase / triangleBase` 就是簇记录
//   里的原始偏移，后续取值表达式与任务 23 的实现**逐字相同**。
// ============================================================
struct SoftRasterClusterView {
    NaniteCluster cluster;   // 簇记录（来自资产段或页池，取决于档位）
    uint vertexBase;         // 顶点取址基（页池内的**绝对**元素下标；关闭档 = 簇记录里的偏移）
    uint triangleBase;       // 三角形取址基
    uint page;               // 该簇所在页（0xFFFFFFFF = 关闭档，无页语义）
    bool resident;           // false ⇒ 页未驻留（调用方必须跳过这一簇并计缺页）
    bool valid;              // false ⇒ 引用非法或页未驻留，调用方一律跳过
};

SoftRasterClusterView softRasterResolveCluster(uint clusterIndex) {
    SoftRasterClusterView v;
    v.vertexBase = 0u;
    v.triangleBase = 0u;
    v.page = 0xFFFFFFFFu;
    v.resident = false;
    v.valid = false;
    if (pagesEnabled == 0u) {
        // 关闭档：直读资产段（与任务 23 的代码路径逐字相同）
        v.cluster = u_Clusters[clusterIndex];
        v.vertexBase = v.cluster.vertexOffset;
        v.triangleBase = v.cluster.triangleOffset;
        v.resident = true;
        v.valid = true;
        return v;
    }
    // 开启档：两次查表 —— 簇→页、页→（槽，两段起点）
    // 【为什么这里要按资产簇数收口，而不是 `GetDimensions`】页映射的下标空间是**资产**簇下标，
    //   而 binding 0 这时绑的是**页池**（元素个数 = 槽数 × clusterStride，池小时会**小于**簇数）
    //   ⇒ 用 GetDimensions 做越界判据会误杀大量合法簇（静默少画）。所以开启档必须按资产簇数查。
    //   （入口文件里保留了关闭档原来的 GetDimensions 判据，一字未改。）
    if (clusterIndex >= assetClusterCount) return v;
    NaniteClusterPageRef cp = u_ClusterPage[clusterIndex];
    NanitePageEntry e = u_PageTable[cp.page];
    v.page = cp.page;
    if (e.resident == 0u) return v;                 // 缺页：valid=false，但 page 有效（供上报用）
    v.cluster = u_Clusters[e.slot * clusterStride + cp.local];
    v.vertexBase   = e.slot * vertexStride   + (v.cluster.vertexOffset   - e.vertexBegin);
    v.triangleBase = e.slot * triangleStride + (v.cluster.triangleOffset - e.triangleBegin);
    v.resident = true;
    v.valid = true;
    return v;
}

// 【任务 24】缺页上报：把页号追加进反馈环（**按页去重**，每帧每页至多一条）。
// 【为什么不在 GPU 侧用页表去重】设计 ④ 原写"去重由页表的 `requestedFrame == 当前帧` 判定"，
//   那要求 **GPU 写页表**，而页表同时被 CPU 每帧重写 ⇒ 同一块内存无同步地双边写。
//   这里改用一个**独立的戳记数组**（反馈缓冲的尾部区域，CPU 只在 `WaitIdle` 之后清 0、
//   GPU 只在帧内写）⇒ 页表仍然**只由 CPU 写**，去重却回到了 GPU 侧。
// 【为什么必须去重（实测踩过）】不去重时，一个缺页会被引用它的**每一个可见簇**各请求一次：
//   首次实测 31648 个可见簇 → 单帧 31648 条请求 vs 1024 槽的环 ⇒ `overflow_total=119476`，
//   且"某一页的请求恰好全落在环外"时该页**永远传不上来**（画面与关流式档不同，且极难定位）。
void softRasterRequestPage(uint page) {
    // 交换取旧值：只有"本帧第一个发现这一页缺失"的调用者拿到 0 ⇒ 由它追加
    uint prev;
    InterlockedExchange(u_PageFeedback[kPageFeedbackStampOffset + page], 1u, prev);
    if (prev != 0u) return;

    uint slot;
    InterlockedAdd(u_PageFeedback[0], 1u, slot);
    if (slot < kPageRequestSlots) {
        u_PageFeedback[kPageFeedbackRingOffset + slot] = page;
        InterlockedAdd(u_Stats[kSoftStatPageRequests], 1u);   // 真 GPU 计数（判据 (d1) 的非空洞守卫）
    } else {
        InterlockedAdd(u_PageFeedback[1], 1u);   // 环满：单独计数，不静默丢
    }
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
// 【任务 24】取址经 `SoftRasterClusterView`：关闭档 = 资产段（与任务 23 逐字相同），
//   开启档 = 页池段（基址已由页表换算）。**函数体只有一份** ⇒ 两档不可能分叉。
SoftRasterTriangle softRasterFetchTriangle(SoftRasterClusterView view, uint triLocal,
                                           float3 instanceTranslation) {
    SoftRasterTriangle tri;
    tri.valid = false;
    tri.p0 = tri.p1 = tri.p2 = float2(0.0, 0.0);
    tri.z0 = tri.z1 = tri.z2 = 0.0;
    tri.invW0 = tri.invW1 = tri.invW2 = 0.0;

    NaniteCluster cluster = view.cluster;
    NanitePackedTriangle packed = u_Triangles[view.triangleBase + triLocal];
    uint3 local = naniteDecodeTriangle(packed);
    uint base = view.vertexBase;

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
