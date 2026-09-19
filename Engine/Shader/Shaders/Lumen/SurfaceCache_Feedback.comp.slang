// SurfaceCache_Feedback.comp.slang — 缺失页检测（步骤 16）
//
// 【目标】每帧产出"接下来需要捕获哪些页"的请求列表 `(pageIndex, weight)`：
// 屏幕按 **16×16 像素**分块，每块取中心像素的世界坐标，找到覆盖它的那张卡（页），
// weight = 该块的**屏幕覆盖面积**（= 块像素数，等权时即块数）× 卡的**重要性** × **距离衰减**。
// C++ 侧对请求做小规模排序，取前 `maxCapturesPerFrame` 个去请求/捕获（步骤 17 的预算摊销在这里落地）。
//
// 【为什么用"卡 AABB 包含测试"当页映射】步骤 13 的卡片清单（每卡：mesh AABB + 轴向）本身就是
// 一个足够好的空间索引：任意世界点落在哪张卡的 AABB 里，那张卡就是"这块屏幕需要的页"。
// 加一条"取包含它的最小 AABB"的规则，避免大网格的粗卡把细卡盖掉。真正上 GPU 的页表散列
// （按 mesh + 面 + 2D 块）留到 18/19 做碎片整理时再上。
//
// 【稳定性验收】请求列表本身是"屏幕覆盖 × 重要性"的排序结果：相机微动时同一批卡仍占主导，
// 所以 Jaccard（前后两帧 top-N 的交集/并集）应当高且平滑；这一步的数值由 C++ 侧统计并打印。
struct FeedbackCard {
    float4 aabbLo;        // xyz = mesh AABB 最小角, w = AABB 边长（立方）
    float4 axisDirPage;   // x = 轴向(0/1/2), y = 方向(±1), z = 页号, w = 重要性
};

[[vk::binding(0, 0)]] Texture2D<float4>            u_WorldPos;    // GBuffer 世界坐标（MRT4.xyz；**alpha 是 dielectricF0，不是有效位**）
[[vk::binding(1, 0)]] StructuredBuffer<FeedbackCard> u_Cards;     // 卡片清单
[[vk::binding(2, 0)]] RWStructuredBuffer<uint>     u_ReqCount;    // [0] = 写入了多少条
[[vk::binding(3, 0)]] RWStructuredBuffer<uint2>    u_Requests;    // x = 页号, y = 权重（定点：weight*1024）

[[vk::push_constant]] cbuffer FeedbackPC {
    uint4 dims;           // xy = 屏幕尺寸, z = 卡片数, w = 槽位总数（tilesX*tilesY）
    uint4 pages;          // x = 页表页数（只统计"有物理页"的卡，避免请求指向不存在的页）
    float4 camPosDist;    // xyz = 相机位置, w = 距离衰减尺度
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint tilesX = (dims.x + 15u) / 16u;
    const uint tilesY = (dims.y + 15u) / 16u;
    if (tid.x >= tilesX || tid.y >= tilesY) return;

    const uint2 px = uint2(tid.x * 16u + 8u, tid.y * 16u + 8u);   // 块中心
    const float4 wp = u_WorldPos.Load(int3((int2)px, 0));
    // 【不能用 alpha 判有效】MRT4 的 alpha 是 dielectricF0（金属/介电各不相同，可以是 0）；
    // 用"世界坐标是否为零"判背景（天空/无几何像素的 worldpos 为 0）。此前用 wp.w <= 0 把整屏都丢掉了 ⇒ 请求恒 0。
    if (all(abs(wp.xyz) < 1e-4)) return;

    const float3 p = wp.xyz;
    // 找"包含 p 的最小 AABB"对应的卡 ⇒ 该页就是这块屏幕需要的页
    uint  bestPage = 0xFFFFFFFFu;
    float bestSide = 1e30;
    float bestImportance = 0.0;
    for (uint i = 0; i < dims.z; ++i) {
        const FeedbackCard c = u_Cards[i];
        const float3 lo = c.aabbLo.xyz;
        const float3 hi = lo + c.aabbLo.w;
        if (any(p < lo) || any(p > hi)) continue;
        if ((uint)c.axisDirPage.z >= pages.x) continue;   // 该卡还没有页 ⇒ 本步不请求
        if (c.aabbLo.w < bestSide) {
            bestSide = c.aabbLo.w;
            bestPage = (uint)c.axisDirPage.z;
            bestImportance = c.axisDirPage.w;
        }
    }
    if (bestPage == 0xFFFFFFFFu) return;

    // weight = 屏幕覆盖面积（16×16 = 256 像素，等权） × 重要性 × 距离衰减
    const float dist = length(p - camPosDist.xyz);
    const float falloff = 1.0 / (1.0 + dist / max(1.0, camPosDist.w));
    const float weight = 256.0 * max(0.01, bestImportance) * falloff;

    // 【每个块写自己的槽位，不用计数器】此前用"原子加取槽位 + 容量上限"有两个问题：
    // ① 非原子版会互相覆盖（实测整屏只剩 1 条）；② 即使改成原子，容量截断让"哪些块进得了列表"
    // 取决于线程完成顺序 ⇒ 请求集合逐帧乱跳，稳定性（步骤 16 的验收）无从谈起。
    // 现在按块号索引：完整、确定、可复现；没有几何的块权重写 0，由 C++ 侧过滤。
    const uint tileIndex = tid.y * tilesX + tid.x;
    u_Requests[tileIndex] = uint2(bestPage, (uint)(weight * 1024.0 + 0.5));
}
