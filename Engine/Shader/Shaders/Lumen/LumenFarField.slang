// LumenFarField.slang — Lumen 远场光追的共享布局（步骤 26）
//
// 【用途】RayGen / ClosestHit / Miss 三个着色器必须对同一份 payload 与参数有完全一致的理解，
// 所以这两块放共享头里定义一次（与 `Lumen/ScreenProbeSampling.slang` 同一个理由：
// 两侧各写一份就会悄悄漂移，而载荷错位的症状是"结果看起来像随机数"）。
//
// 【payload 必须与 RT_GI.rchit / RT_GI.rmiss 逐字段一致】远场 pass **复用** RTGI 的命中/未命中
// 着色器（见 `LumenFarFieldPass::Initialize`）：它返回 `radianceT` = (命中点出射辐射度, 命中距离,
// 未命中时 a = -1)。复用而不是另写一份，是为了让"远场命中的辐射度"与 RTGI 同源、量纲一致。
#ifndef HUGENGINE_LUMEN_FAR_FIELD
#define HUGENGINE_LUMEN_FAR_FIELD

struct LumenFarFieldPayload {
    float4 radianceT;   // rgb = 命中点出射辐射度（ClosestHit 写入）, a = 命中距离（<0 = 未命中）
};

// 参数 cbuffer（set0 / binding 7）：本 pass 的每帧参数
struct LumenFarFieldParams {
    uint4  dims;      // x = 探针数, y = 每探针光线数, z = 随机种子（帧号）, w = 采样模式
    float4 range;     // x = tMin, y = tMax, z = 远场阈值（记录用）, w = 未用
};

#endif // HUGENGINE_LUMEN_FAR_FIELD
