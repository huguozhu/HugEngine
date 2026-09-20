// ============================================================
// Nanite_ClusterBVH.comp.slang — per-instance cluster BVH 的三阶段簇剔除
//   （§14.8 任务 14 的 DFS 遍历 + 任务 15 的 Phase 2 Hi-Z 遮挡 / Phase 3 LOD 选择）
//
// 【与设计 §5.1 的对应】
//   Phase 2 "Persistent Cluster Culling (per-instance)":
//       For each visible instance: BVH traverse (cluster tree, depth-first)
//       → Frustum cull cluster bounds → Hi-Z occlusion cull (sample Hi-Z pyramid)
//   Phase 3 "LOD Selection (per surviving cluster)":
//       projectedError = cluster.maxError / distance → selectLevel(…, threshold = 1 pixel)
//
// 【本文件在任务 15 里的改动（三处，逐条）】
//   ① 实例域改为 **Phase 1 的可见实例**：任务 13 的 `Nanite_InstanceCull` 每帧写一张
//      `u_VisibleMask`（按实例下标寻址），本 pass 只处理 `mask[i] != 0` 的实例。
//      【为什么用掩码而不是"可见列表"】掩码与实例域 `[0, min(instanceCount, maxInstances))`
//      天然对齐 ⇒ 钳制后的子集**确定**；压缩列表的槽位顺序由 GPU 原子决定，"取前 k 个"
//      在可见数超过上限时是不确定的子集，会让 CPU/GPU 逐项比较失去意义。
//   ② Phase 2 后半：簇球通过视锥后做 Hi-Z 遮挡测试（`hizOccluded`，4 角最小深度比较）。
//   ③ Phase 3：DAG 割（`lodSelected`，用 `u_LodInfo` 的 own/parent 误差 + 相机距离 + 像素焦距）。
//   另加三组读数（`u_Stats`）：通过视锥的簇数、被遮挡的簇数、**选中级别分布**直方图 + 访问节点数。
//
// 【任务 16 的改动（本文件现在也产出"光栅端要用的东西"）】Phase 3 接受一个簇时，在**同一个原子
//   槽位**上除可见簇引用外再写一条 `IndirectCmd`（= `VkDrawIndexedIndirectCommand`）：
//     indexCount    = 簇内索引个数（`triangleCount × 3`）
//     firstIndex    = 簇在索引段里的首个索引位置（`triangleOffset × 3`）
//     vertexOffset  = 簇的顶点段起始记录下标
//     instanceCount = 1
//     firstInstance = **簇号**（光栅端用 `SV_InstanceID` 收它）
//   并把"已接受的绘制条数"原子累加进 `u_DrawCount`（= 绘制端 `DrawIndexedIndirectCount` 的计数）。
//   【为什么用**第二个**计数器而不是复用 `u_VisibleClusterCount`】后者是任务 14/15 的验收读数
//   （"可见簇总数"），必须**不截断**；而绘制计数必须满足"≤ 间接缓冲容量"这条硬约束
//   （`vkCmdDrawIndexedIndirectCount` 的 count 超过 maxDrawCount 不是定义行为）。两者分开后：
//   容量足够时它们相等（正常路径），容量不足时可见计数保持真值、绘制计数被钳住 + 截断计数入
//   `u_Stats[kStatDrawTruncated]`。
//   【无空转】计数与命令由**同一次派发**写出，绘制端只画 `[0, count)` ⇒ 不存在"没写就画"。
//   【顺序】绘制端由 `NaniteRenderer` 录在**同一个帧图 pass** 内（本 pass 之后一个屏障），
//   不依赖帧图对两个零资源 pass 的排序（帧图对 inDegree=0 的 pass 按 LIFO 处理，注册顺序 ≠
//   执行顺序 —— 任务 15 的教训）。
//
// 【与 C++ 侧的布局契约】本文件的每个结构体都必须与
//   `Engine/Render/Nanite/NaniteTypes.h` 的 "§14.8 任务 14/15" 小节逐字段一致（那边用
//   sizeof + offsetof 的 static_assert 钉住）：
//     · `BVHNode`        —— 32B：float4 centerRadius + uint4 link(left,right,count,flags)
//     · `ClusterSphere`  —— 16B：float4(xyz = 网格空间球心，w = 半径)
//     · `ClusterRef`     —— 8B：(instance, cluster)
//     · `LODInfo`        —— 16B：ownError / parentError / lodLevel / flags(bit0 = 根)
//     · `ClusterDrawRange`—— 16B：【任务 16】每簇的绘制参数（firstIndex/indexCount/vertexOffset）
//     · `IndirectCmd`    —— 20B：【任务 16】= VkDrawIndexedIndirectCommand（5×u32）
//     · `InstanceObject` —— GPUSceneObject（128B）契约的镜像（只用 `ltw3.xyz` 与 `indexCount`）
//     · `CullChainParams`—— 112B：4 个 vp 行 + cameraPos + lodParams + misc
//     · `u_Stats`        —— 扁平 u32：0=通过视锥、1=遮挡、2..9=级直方图、10=访问节点数、
//                           11=【任务 16】因绘制容量不足被截断的簇数
//     · push constant    —— 6 个世界空间视锥平面 + 4 个 u32（112B，与任务 14 完全一致）
//
// 【为什么把 localToWorld 拆成 4 个 float4】Slang 的矩阵布局约定（行/列主序）与"内存里的
//   16 个 float"之间的对应关系依赖编译选项 ⇒ 拆成 `ltw0..ltw3` 后 `ltw3.xyz` **无条件**是
//   内存里的第 12/13/14 个 float（列主序矩阵的平移列，与 C++ 侧 `localToWorld[12..14]` 同一份
//   比特），风险归零。**任务 15 的 view-proj 沿用同一手法**（4 个 `vpRow` + 显式点积）：
//   `vpRow{r}` = 列主序 glm 矩阵的 **row r**（即 `viewProj[c][r]`），CPU 侧按同一口径填。
//
// 【判据（必须与 CPU 参考逐字一致）】
//   · 视锥：平面 `planes[i] = (n.xyz, d)`，`dot(n, p) + d >= 0` 在内侧；任一平面满足
//     `dot(n, c) + d < -radius` ⇒ 不可见（**不加 epsilon**）。顺序 `[左, 右, 下, 上, 近, 远]`。
//   · LOD：`error × focal > threshold × distance` ⇒ "该误差在该距离下投影超过 threshold 像素"。
//   · Hi-Z：世界球 → 屏幕 UV 包围盒 + 最近 ndc.z；完全在屏幕外 ⇒ 不剔除；按盒子的最长边像素
//     选层（`ceil(log2(max(1, 最长边)))`，**下限钳到 1**：金字塔 mip0 从未被写入）；
//     取 4 角的**最小深度**的最大值，`zNear > 该值` ⇒ 被遮挡。
//
// 【§14.3 依赖禁令】只依赖本模块自己的 POD，不引用 GI/Lumen/GPUCulling。
// ============================================================

// ── 描述符绑定（set=0；顺序与 `NaniteCull::Initialize` 的布局逐条对应）──
//   0 = BVH 节点表（只读）
//   1 = 叶子簇表（只读；叶子用 [link.x, link.x + link.z)）
//   2 = 簇包围球表（只读，网格空间）
//   3 = 实例表（128B 契约，只读）
//   4 = 可见簇列表（读写；原子取槽位压缩写入）
//   5 = 可见簇计数（读写；单个 uint，每帧由命令缓冲内的拷贝清 0）
//   6 = 【任务 15】可见实例掩码（只读；Phase 1 的输出，按实例下标寻址）
//   7 = 【任务 15】每簇 LOD 元数据（只读；own/parent 误差 + LOD 级 + 根标志）
//   8 = 【任务 15】三阶段参数（只读；vp 的 4 行 + 相机 + LOD/Hi-Z 参数）
//   9 = 【任务 15】三阶段读数（读写；每帧由命令缓冲内的拷贝清 0）
//  10 = 【任务 15】Hi-Z 金字塔（R32_FLOAT，采样器点采样 + 层间线性；见下面的选层说明）
//  11 = 【任务 16】每簇绘制参数（只读；firstIndex / indexCount / vertexOffset）
//  12 = 【任务 16】间接绘制命令（读写；与可见簇列表**同一个槽位**）
//  13 = 【任务 16】绘制计数（读写；单个 uint，每帧由命令缓冲内的拷贝清 0）

// BVH 节点（32B）：与 NaniteBVHNode 逐字段一致
//   link.x = 内部节点左孩子下标 / 叶子簇表首下标
//   link.y = 内部节点右孩子下标 / 叶子为 kNoChild
//   link.z = 内部节点 2 / 叶子簇数（≥1）
//   link.w = 位标志：bit0 = 1 表示叶子
struct BVHNode {
    float4 centerRadius;   // 偏移 0：xyz = 网格空间包围球心；w = 半径
    uint4  link;           // 偏移 16：(left, right, count, flags)
};

// 每簇包围球（网格空间；与 NaniteClusterSphere 一致：3×float + radius = 16B）
struct ClusterSphere {
    float4 centerRadius;   // xyz = 网格空间球心；w = 半径（≥ 0）
};

// 一条可见簇引用（8B；与 NaniteVisibleClusterRef 一致）
struct ClusterRef {
    uint instance;
    uint cluster;
};

// 【任务 15】每簇 LOD 元数据（16B；与 NaniteClusterLODInfo 逐字段一致）
//   ownError    = 用本簇替代其孩子渲染的绝对误差（叶子 = 0）
//   parentError = 用本簇的父簇替代本簇渲染的绝对误差（根 = 0，用 flags 区分）
//   flags bit0  = 根簇（没有任何簇以它为子）
struct LODInfo {
    float ownError;
    float parentError;
    uint  lodLevel;
    uint  flags;
};

// 【任务 16】每簇的绘制参数（16B；与 `NaniteClusterDrawRange` 逐字段一致）
//   firstIndex   = 簇在索引段里的首个**索引位置**（= triangleOffset × 3）
//   indexCount   = 簇内索引个数（= triangleCount × 3；3 的倍数）
//   vertexOffset = 簇的顶点段起始**记录下标**（原样搬运进命令）
struct ClusterDrawRange {
    uint firstIndex;
    uint indexCount;
    int  vertexOffset;
    uint _pad;
};

// 【任务 16】间接绘制命令（20B；必须与 VkDrawIndexedIndirectCommand 二进制兼容：
//   5×u32 = 20B，顺序 indexCount/instanceCount/firstIndex/vertexOffset/firstInstance）
struct IndirectCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

// GPUSceneObject（128B）的 Slang 镜像：字段顺序/类型必须与 NaniteInstanceGpuObject 一致。
// 【本文件只用两个字段】`ltw3.xyz`（实例平移）与 `indexCount`（空实例跳过）；
//   其余字段原样保留，仅为把 128B 的步长钉对。
struct InstanceObject {
    float4 ltw0;              // 偏移 0  ：localToWorld 第 1 列
    float4 ltw1;              // 偏移 16 ：localToWorld 第 2 列
    float4 ltw2;              // 偏移 32 ：localToWorld 第 3 列
    float4 ltw3;              // 偏移 48 ：localToWorld 第 4 列（xyz = 平移，列主序）
    float4 boundsMin;         // 偏移 64 ：世界空间 AABB 下界（w 不用）
    float4 boundsMax;         // 偏移 80 ：世界空间 AABB 上界（w 不用）
    uint   meshIndex;         // 偏移 96
    uint   materialIndex;     // 偏移 100
    uint   objectID;          // 偏移 104
    uint   visibilityFlags;   // 偏移 108
    uint   indexCount;        // 偏移 112：0 ⇒ 空实例，跳过（与 CPU 参考同规则）
    uint   firstIndex;        // 偏移 116
    int    vertexOffset;      // 偏移 120
    uint   _pad;              // 偏移 124
};

// 【任务 15】三阶段参数（112B；与 `NaniteCullChainParams` 逐字段一致）
//   vpRow{r}  = 列主序 viewProj 的 row r（= viewProj[c][r]），显式点积 ⇒ 与 CPU 同式
//   cameraPos = 相机世界坐标（xyz；w 未用）
//   lodParams = (focalPixels, thresholdPixels, screenW, screenH)
//   misc      = (hizMipCount, lodEnabled, 保留, 保留)
struct CullChainParams {
    float4 vpRow0;
    float4 vpRow1;
    float4 vpRow2;
    float4 vpRow3;
    float4 cameraPos;
    float4 lodParams;
    uint4  misc;          // (hizMipCount, lodEnabled, drawCapacity, 保留)
};

[[vk::binding(0, 0)]] StructuredBuffer<BVHNode>       u_Nodes;
[[vk::binding(1, 0)]] StructuredBuffer<uint>          u_LeafClusters;
[[vk::binding(2, 0)]] StructuredBuffer<ClusterSphere> u_ClusterSpheres;
[[vk::binding(3, 0)]] StructuredBuffer<InstanceObject> u_Instances;
[[vk::binding(4, 0)]] RWStructuredBuffer<ClusterRef>  u_VisibleClusters;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint>        u_VisibleClusterCount;
[[vk::binding(6, 0)]] StructuredBuffer<uint>          u_VisibleMask;
[[vk::binding(7, 0)]] StructuredBuffer<LODInfo>       u_LodInfo;
[[vk::binding(8, 0)]] StructuredBuffer<CullChainParams> u_Params;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint>        u_Stats;
[[vk::binding(10, 0)]] Texture2D<float>               u_HiZ;
[[vk::binding(10, 0)]] SamplerState                   u_HiZSampler;
[[vk::binding(11, 0)]] StructuredBuffer<ClusterDrawRange> u_ClusterDrawRanges;  // 【任务 16】只读
[[vk::binding(12, 0)]] RWStructuredBuffer<IndirectCmd> u_IndirectCommands;      // 【任务 16】读写
[[vk::binding(13, 0)]] RWStructuredBuffer<uint>        u_DrawCount;             // 【任务 16】读写

[[vk::push_constant]] cbuffer NaniteClusterBVHParams {
    float4 planes[6];         // 偏移 0 ：世界空间视锥六平面（[左,右,下,上,近,远]，n 已归一化）
    uint   instanceCount;     // 偏移 96：本帧实例数（= cfg 键 nanite_instance_test_count，已钳制）
    uint   clusterCount;      // 偏移 100：簇数（= 簇球表条数）
    uint   nodeCount;         // 偏移 104：BVH 节点数
    uint   visibleCapacity;   // 偏移 108：可见簇列表容量（超出容量的槽位不写；计数照常累加，
                              //             读回时按容量截断 —— 与 CPU 参考的写入口径一致）
                              //             【任务 16】间接命令同槽位写入，但按 params.misc.z
                              //             的绘制容量单独设门（≤ 本容量）
};

// 显式栈容量：必须与 C++ 侧 kNaniteBVHMaxStackDepth（= 32）一致。
static const uint kBVHMaxStackDepth = 32u;

// ── u_Stats 的槽位约定（与 C++ 侧 `NaniteCullStats` 的 static_assert 逐条对应）──
static const uint kStatFrustumPass = 0u;    // Phase 2 前半：通过簇球视锥测试的引用数
static const uint kStatOccluded    = 1u;    // Phase 2 后半：被 Hi-Z 判为遮挡的引用数
static const uint kStatLodBase     = 2u;    // Phase 3：直方图起点（8 个槽：级 0..7）
static const uint kStatVisited     = 10u;   // 本 pass 访问过的 BVH 节点数（全部实例求和）
static const uint kStatDrawTruncated = 11u; // 【任务 16】因绘制容量不足而未写间接命令的簇数

// Hi-Z 的最小可采样层：**1**（金字塔贴图的 mip0 从未被写入 —— `BuildHiZPyramid` 从深度输入
//   直接下采样进 mip1；既有 `GPUCull_TwoPhase.comp.slang:34` 把下限钳到 0 是一处既有缺陷）。
static const float kHiZMinMip = 1.0f;

// 球-视锥可见性：任一面外侧即不可见（与 NaniteSphereVisibleInFrustum 同判据、同比较符）
bool sphereVisibleInFrustum(float3 center, float radius) {
    // 与 CPU 侧一致：半径 < 0 按 0 处理（NaN 半径走"可见"分支，生成侧不产生 NaN）
    float r = (radius > 0.0f) ? radius : 0.0f;
    for (int i = 0; i < 6; ++i) {
        float distance = dot(planes[i].xyz, center) + planes[i].w;
        if (distance < -r) return false;
    }
    return true;
}

// 【任务 15】Phase 2 后半：Hi-Z 遮挡测试
//   ① 世界 AABB 的 8 个角 → 屏幕 UV 包围盒 + 最近 ndc.z；
//   ② 任一角跨越相机平面（clip.w <= 0）或完全在屏幕外 ⇒ **保守不剔除**；
//   ③ 选层：`ceil(log2(max(1, 盒子最长边像素)))` 钳到 [1, mipCount-1]；
//   ④ 4 角的**最小深度**取最大；`zNear > 该值` ⇒ 被遮挡。
// 【为什么 4 角而不是整盒】层 L 的每个 texel 覆盖 2^L×2^L 足迹；取 4 个角的采样 ⇒ 覆盖盒子的
//   4 个象限，对"盒子不比 texel 足迹大"的情形不会漏掉遮挡物（保守方向）。
bool hizOccluded(float3 center, float radius) {
    CullChainParams p = u_Params[0];
    uint mipCount = p.misc.x;
    // mipCount < 2 ⇒ 没有可采样的层（mip0 未写入）⇒ 关闭遮挡测试（= cfg 键 nanite_hiz=0 的退化口径）
    if (mipCount < 2u) return false;

    float2 rMin = float2(1.0e30f, 1.0e30f);
    float2 rMax = float2(-1.0e30f, -1.0e30f);
    float zNear = 1.0e30f;

    for (uint c = 0u; c < 8u; ++c) {
        float3 corner = center + radius * float3((c & 1u) != 0u ? 1.0f : -1.0f,
                                                 (c & 2u) != 0u ? 1.0f : -1.0f,
                                                 (c & 4u) != 0u ? 1.0f : -1.0f);
        float4 hp = float4(corner, 1.0f);
        float4 clip = float4(dot(p.vpRow0, hp), dot(p.vpRow1, hp),
                             dot(p.vpRow2, hp), dot(p.vpRow3, hp));
        if (!(clip.w > 1.0e-6f)) return false;   // 跨越相机平面 ⇒ 保守不剔除

        float3 ndc = clip.xyz / clip.w;
        if (ndc.x != ndc.x || ndc.y != ndc.y || ndc.z != ndc.z) return false;   // NaN 兜底
        float2 s = ndc.xy * 0.5f + 0.5f;
        rMin = min(rMin, s);
        rMax = max(rMax, s);
        zNear = min(zNear, ndc.z);
    }

    // 完全在屏幕外（与 [0,1]² 无交）⇒ Hi-Z 覆盖不到 ⇒ 保守不剔除
    if (rMax.x < 0.0f || rMax.y < 0.0f || rMin.x > 1.0f || rMin.y > 1.0f) return false;
    rMin = clamp(rMin, 0.0f, 1.0f);
    rMax = clamp(rMax, 0.0f, 1.0f);

    float2 sizePixels = (rMax - rMin) * p.lodParams.zw;   // zw = (screenW, screenH)
    float largest = max(1.0f, max(sizePixels.x, sizePixels.y));
    float mip = clamp(ceil(log2(largest)), kHiZMinMip, float(mipCount - 1u));

    float d0 = u_HiZ.SampleLevel(u_HiZSampler, float2(rMin.x, rMin.y), mip);
    float d1 = u_HiZ.SampleLevel(u_HiZSampler, float2(rMax.x, rMin.y), mip);
    float d2 = u_HiZ.SampleLevel(u_HiZSampler, float2(rMin.x, rMax.y), mip);
    float d3 = u_HiZ.SampleLevel(u_HiZSampler, float2(rMax.x, rMax.y), mip);
    float deepest = max(max(d0, d1), max(d2, d3));
    return zNear > deepest;
}

// 【任务 15】Phase 3：DAG 割（LOD 选择）
//   ① `ownError × focal > threshold × distance` ⇒ 本簇还不够好（应交给更细的孩子）⇒ 不选；
//   ② 父簇"够好"（`parentError × focal <= threshold × distance`）⇒ 父簇会被选中 ⇒ 本簇不选；
//      根簇没有父簇 ⇒ ② 自动成立（用 flags 的根位，不能用 parentError == 0：根的它也是 0）。
// 【与 C++ 侧】`NaniteLODClusterSelected` 是逐字同构的实现（单测直接覆盖它）。
bool lodSelected(uint cluster, float3 worldCenter, float focalPixels, float thresholdPixels) {
    LODInfo info = u_LodInfo[cluster];
    float3 delta = worldCenter - u_Params[0].cameraPos.xyz;
    float distance = length(delta);
    if (!(distance > 1.0e-4f)) distance = 1.0e-4f;   // 与 kNaniteLODMinDistance 同一个常量

    if (info.ownError * focalPixels > thresholdPixels * distance) return false;
    if ((info.flags & 1u) != 0u) return true;        // 根簇
    return info.parentError * focalPixels > thresholdPixels * distance;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= instanceCount) return;

    // ① 空实例（无索引可画）不参与 —— 与 CPU 参考的 indexCount == 0 跳过逐条对应
    InstanceObject instance = u_Instances[id.x];
    if (instance.indexCount == 0u) return;

    // ② 【任务 15】Phase 1 的可见实例掩码：不可见 ⇒ 本实例一个节点都不遍历
    if (u_VisibleMask[id.x] == 0u) return;

    CullChainParams params = u_Params[0];
    float focalPixels     = params.lodParams.x;
    float thresholdPixels = params.lodParams.y;
    bool  lodEnabled      = (params.misc.y != 0u) && (focalPixels > 0.0f);
    // 【任务 16】间接命令缓冲的容量上界（CPU 侧的 cfg 键 `nanite_draw_capacity`，已钳到
    //   可见簇引用表容量与缓冲容量；0/缺省时 = 容量上界）。绘制计数恒 ≤ 它 ⇒ 恒 ≤ maxDrawCount。
    uint  drawCapacity    = params.misc.z;

    // ③ 实例变换的平移列（列主序的第 4 列）；本任务的合成实例是纯平移，半径不变
    float3 origin = instance.ltw3.xyz;

    // ④ 每实例一次深度优先遍历（显式栈；先压右、再压左 ⇒ 左子树先访问）
    uint visited = 0u;
    uint stack[kBVHMaxStackDepth];
    uint stackSize = 0u;
    stack[stackSize++] = 0u;   // 根节点恒为 0（CPU 构建器保证）

    while (stackSize > 0u) {
        uint nodeIndex = stack[--stackSize];
        if (nodeIndex >= nodeCount) continue;   // 防御：非法孩子下标不越界
        ++visited;                              // 先计数、后判可见（与 CPU 参考同位置）

        BVHNode node = u_Nodes[nodeIndex];
        if (!sphereVisibleInFrustum(origin + node.centerRadius.xyz, node.centerRadius.w)) {
            continue;   // 节点球不可见 ⇒ 整棵子树跳过（节点球是后代簇球的保守并集）
        }

        if ((node.link.w & 1u) != 0u) {
            // 叶子：对 [link.x, link.x + link.z) 的每个簇做三阶段判据
            uint first = node.link.x;
            uint count = node.link.z;
            for (uint k = 0u; k < count; ++k) {
                uint cluster = u_LeafClusters[first + k];
                if (cluster >= clusterCount) continue;   // 防御：越界簇下标不读球表
                ClusterSphere sphere = u_ClusterSpheres[cluster];
                float3 worldCenter = origin + sphere.centerRadius.xyz;
                if (!sphereVisibleInFrustum(worldCenter, sphere.centerRadius.w)) {
                    continue;
                }
                // ── Phase 2 前半：通过视锥（计数位置与 CPU 参考的 ++ 同位置）──
                InterlockedAdd(u_Stats[kStatFrustumPass], 1u);

                // ── Phase 2 后半：Hi-Z 遮挡（mipCount < 2 ⇒ 恒不遮挡）──
                if (hizOccluded(worldCenter, sphere.centerRadius.w)) {
                    InterlockedAdd(u_Stats[kStatOccluded], 1u);
                    continue;
                }

                // ── Phase 3：DAG 割（LOD 选择；关闭时不筛任何簇）──
                if (lodEnabled) {
                    if (!lodSelected(cluster, worldCenter, focalPixels, thresholdPixels)) continue;
                    LODInfo info = u_LodInfo[cluster];
                    uint level = (info.lodLevel < 8u) ? info.lodLevel : 7u;
                    InterlockedAdd(u_Stats[kStatLodBase + level], 1u);
                }

                // 原子取槽位：计数缓冲的最终值 = 被接受的簇引用总数（与 CPU 的 written 同义）
                uint slot;
                InterlockedAdd(u_VisibleClusterCount[0], 1u, slot);
                if (slot < visibleCapacity) {
                    ClusterRef ref;
                    ref.instance = id.x;
                    ref.cluster  = cluster;
                    u_VisibleClusters[slot] = ref;
                }

                // ── 【任务 16】把这条可见簇直接翻译成光栅端要用的间接绘制命令 ──
                // 【同槽位】命令写在与引用**完全相同的槽位**上 ⇒ 槽位 k 的引用与命令永远是同一条簇，
                //   读回逐条比对不需要任何映射（容量也刻意取成同一个数）。
                // 【容量截断】超过 drawCapacity（≤ 缓冲容量，CPU 侧钳过）的槽位不写命令、只记数
                //   ⇒ 绘制计数恒 ≤ maxDrawCount（`vkCmdDrawIndexedIndirectCount` 的硬约束）。
                // 【绘制计数】只在**真的写了命令**时 +1 ⇒ count 恰好 = 命令缓冲里 [0, count) 的有效条数，
                //   绘制端按 [0, count) 画 ⇒ 一条空转都不会有。
                if (slot < drawCapacity) {
                    ClusterDrawRange range = u_ClusterDrawRanges[cluster];
                    IndirectCmd cmd;
                    cmd.indexCount    = range.indexCount;      // 簇内索引个数（triangleCount × 3）
                    cmd.instanceCount = 1u;                    // 一个簇 = 一次绘制
                    cmd.firstIndex    = range.firstIndex;       // 簇在索引段里的首个索引位置
                    cmd.vertexOffset  = range.vertexOffset;     // 簇的顶点段起始记录下标
                    cmd.firstInstance = cluster;                // 簇号（光栅端用 SV_InstanceID 收）
                    u_IndirectCommands[slot] = cmd;
                    InterlockedAdd(u_DrawCount[0], 1u);
                } else {
                    InterlockedAdd(u_Stats[kStatDrawTruncated], 1u);
                }
            }
        } else {
            // 内部：先压右、再压左 ⇒ 下一次弹出的是左孩子（深度优先、顺序与 CPU 一致）
            // 溢出不静默：构建端把树高硬限制在 kBVHMaxStackDepth 以内（单测断言）。
            if (stackSize + 2u <= kBVHMaxStackDepth) {
                stack[stackSize++] = node.link.y;
                stack[stackSize++] = node.link.x;
            }
        }
    }

    // ⑤ 把本线程（本实例）访问过的节点数一次性累加：整数加法可交换 ⇒ 与线程顺序无关，
    //    同输入两次运行的读数逐位相同（这正是"遍历访问数可复现"的实现前提）。
    InterlockedAdd(u_Stats[kStatVisited], visited);
}
