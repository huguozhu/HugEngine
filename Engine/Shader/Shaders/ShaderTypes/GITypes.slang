#ifndef HUGENGINE_GPU_GI_TYPES
#define HUGENGINE_GPU_GI_TYPES
// ============================================================
// ShaderTypes/GITypes.slang — GI 源槽与通道合成参数（C++/Slang 共享）
//
// 【为什么独立成片】这块结构只有 uint / float，**不含任何向量类型** ⇒ C++ 侧包含它不会
// 牵入 Math/GLM。`Engine/Render/GI/GITypes.h`（RHI-free 数据模型 + 单元测试）只需要这一片，
// 于是从"展开 549 行"降到"展开约 110 行"。带 float4x4 的 GIBlendParams 单独在
// GIBlendParams.slang —— 要合成参数的 pass 本来就都在 GPU 侧，没理由让纯数据模型付这份代价。
// ============================================================

#include "ShaderTypes/GPUDefines.slang"

GPU_CONST_BEGIN

// ============================================================
// GI 分层合成参数（UBO）——通道通用，按「源数组」表达（Wave 1）
//
// 为什么是数组而不是三个固定语义槽（screenSpace/rayTracing/probe）：
//   · 固定槽位等价于"每个算法预留一个专属席位"——每加一种 GI 都要改 UBO 结构 +
//     帧图映射 + shader 分支，且第 4 个源无处安放（RSM/Lightmap 曾因此进不了 shader）
//   · 数组化后，「槽位」是通用容器，语义由 id 决定 → 新增算法只需
//     GISourceId 加一项 + SampleSource 加一个 case
//   · count 与 GIChannelStack::kMaxSources 一致，容量按「同时启用数」而非「算法总数」定
// ============================================================
GPU_CONST(GPU_UINT kGIMaxSourcesPerChannel = 4);   // 每通道最多同时参与的源数

// ── GISourceId（必须与 C++ 的 he::render::GISourceId 数值一致）──
// 源的语义（属于哪个频段、采样什么数据）由 id 决定；新增算法在此加一项 +
// 在 shader 的 SampleXxxSource 加一个 case 即可，无需改动 UBO 结构。
GPU_CONST(GPU_UINT GISOURCE_NONE          = 0);
GPU_CONST(GPU_UINT GISOURCE_IBL           = 1);   // 低频：环境辐照度 / 预滤波
GPU_CONST(GPU_UINT GISOURCE_LIGHTMAP      = 2);   // 低频：烘焙光照（预留）
GPU_CONST(GPU_UINT GISOURCE_DDGI          = 3);   // 低频：动态漫反射探针
GPU_CONST(GPU_UINT GISOURCE_SSGI          = 4);   // 中频：屏幕空间间接漫反射
GPU_CONST(GPU_UINT GISOURCE_SSR           = 5);   // 中频：屏幕空间反射
GPU_CONST(GPU_UINT GISOURCE_SSAO          = 6);   // 中频：屏幕空间环境光遮蔽
GPU_CONST(GPU_UINT GISOURCE_RSM           = 7);   // 中频：反射阴影贴图间接光
GPU_CONST(GPU_UINT GISOURCE_RTGI          = 8);   // 高频：光追间接漫反射
GPU_CONST(GPU_UINT GISOURCE_RT_REFLECTION = 9);   // 高频：光追反射
GPU_CONST(GPU_UINT GISOURCE_RTAO          = 10);  // 高频：光追环境光遮蔽
GPU_CONST(GPU_UINT GISOURCE_GTAO          = 11);  // 中频：地平线切片 AO（SSAO 的高质量替代）
GPU_CONST(GPU_UINT GISOURCE_LUMEN         = 12);  // 虚拟化几何 GI（Surface Cache + SDF + 屏幕探针 + Radiance Cache）

GPU_STRUCT GISourceSlot {
    GPU_UINT id;                 // GISourceId（决定 SampleSource 走哪条分支）
    float    weight;             // 相对权重（0 = 不参与合成）
    float    falloffDistance;    // 「距离让位」（0 = 不启用）
    GPU_UINT confidence;         // GISourceConfidence 位掩码（逐像素可信度判据，见下）
};

// ── 逐像素置信度判据位（与 C++ GISourceConfidence 逐位一致，由 UBO 逐槽传入）──
// 放在 UBO 里而不是在着色器里按 id 判断：否则 C++ 与着色器各有一份 id 列表，必然漂移。
GPU_CONST(GPU_UINT GICONF_CAMERA_COVERAGE = 1);  // 相机屏幕覆盖：视口外/边缘淡出 ⇒ 置信度 0
GPU_CONST(GPU_UINT GICONF_PROBE_GRID      = 2);  // 探针网格覆盖：网格外 ⇒ 置信度 0（DDGI，§9.2-K）

// ── 每通道的合成参数（UBO 元素）──
// 【C++ 侧默认值】Slang 结构不支持默认成员初始化器，故默认值整块放在 `__cplusplus` 分支里：
//   · 布局完全不受影响（默认成员初始化器不改变 sizeof / offsetof）
//   · 有了它，C++ 侧直接 `GIChannelBlendParams b{}` 就得到"归一化加权 + 5% 边缘带宽"，
//     不必再维护一个带 `Add()` 和默认值的包装结构（原先的 `GIChannelBlendData` 已删除）
//   · 数组必须值初始化：未使用的槽位若留垃圾，整块 memcpy 进 UBO 会把垃圾带给 shader
GPU_STRUCT GIChannelBlendParams {
#ifdef __cplusplus
    GISourceSlot sources[kGIMaxSourcesPerChannel]{};
    GPU_UINT     count       = 0;
    GPU_UINT     mode        = 1;      // 0=相加（对照）, 1=归一化加权
    GPU_UINT     furnaceMode = 0;      // 白炉数值测试（见 frag）
    float        edgeFade    = 0.05f;  // 屏幕覆盖置信度的边缘淡出带宽（占短边比例）
#else
    GISourceSlot sources[kGIMaxSourcesPerChannel];   // 参与合成的源（低频 → 高频）
    GPU_UINT     count;                              // 实际源数
    GPU_UINT     mode;                               // 0=相加（对照）, 1=归一化加权
    GPU_UINT     furnaceMode;                        // 白炉数值测试（见 frag）
    float        edgeFade;                           // 屏幕覆盖置信度的边缘淡出带宽（占短边比例）
#endif
};

GPU_CONST_END

#ifdef __cplusplus
// C++ 分支自清理：允许 C++ 头直接 include 本切片而不泄漏宏；Slang 分支由总入口收尾
#undef GPU_UINT
#undef GPU_INT
#undef GPU_STRUCT
#undef GPU_CONST
#undef GPU_CONST_BEGIN
#undef GPU_CONST_END
#endif

#endif // HUGENGINE_GPU_GI_TYPES
