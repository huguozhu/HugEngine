// RTPayloads.slang — 光线追踪 payload 结构体（rgen / rchit / rahit / rmiss 之间传递）
//
// 【为什么收敛】这 4 个 payload 此前在 6 / 5 / 4 / 3 个文件里各声明一份：同一个结构体在 rgen
// 里写一次、在 rchit 里写一次、在 rmiss 里再写一次。它们**必须逐字段一致**（TraceRay 的内联
// payload 就是按交出的内存解释），手抄多份是"改一处忘一处"的温床 —— 这类不一致编译期查不出，
// 只在运行期表现为读到垃圾。集中到这里后所有 pass 引同一份定义。
//
// 用法：`#include "RTPayloads.slang"`，然后 `GIPayload p; p.radianceT = ...; TraceRay(..., p);`
//
// 【约定】带 `a`/`w` 分量的 payload 用它表示**命中距离**，各 rchit 写入 `-1` 表示未命中
//（rgen 侧据此区分"命中"与"未命中"）。
#ifndef HUGENGINE_RT_PAYLOADS_SLANG
#define HUGENGINE_RT_PAYLOADS_SLANG

// 阴影射线：只需要"是否被遮挡"。
// 原先 6 份：RT_Shadow.rgen / .rahit / .rmiss、RT_AO.rgen、RT_Common.rchit / .rmiss
struct ShadowPayload {
    bool blocked;  // true=被遮挡, false=可见
};

// 通用着色射线载荷（Sponza / 顶点拉取路径）：颜色 + 命中距离。
// 原先 5 份：RT_Sponza.rgen / .rchit / .rmiss、RT_PBR.rchit、RT_PositionFetch.rchit
struct RayPayload {
    float3 color;
    float  hitT;
};

// GI 射线载荷：rgb=辐射度, a=命中距离（-1=未命中）。
// 原先 4 份：DDGI_Trace.rgen、RT_GI.rgen / .rchit / .rmiss（注释原有两种写法，此处统一）
struct GIPayload {
    float4 radianceT;  // rgb=辐射度, a=命中距离（-1=未命中）
};

// 反射射线载荷：rgb=反射颜色, a=命中距离（-1=未命中）。
// 原先 3 份：RT_Reflection.rgen / .rchit / .rmiss
struct ReflectionPayload {
    float4 colorT;  // rgb=反射颜色, a=命中距离（-1=未命中）
};

#endif // HUGENGINE_RT_PAYLOADS_SLANG
