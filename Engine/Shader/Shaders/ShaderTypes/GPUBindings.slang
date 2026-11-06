#ifndef HUGENGINE_GPU_BINDINGS
#define HUGENGINE_GPU_BINDINGS
// ============================================================
// ShaderTypes/GPUBindings.slang — descriptor set 索引 + 全部绑定号（C++/Slang 共享）
//
// 只含常量、不含类型：任何 pass 都能单独包含本切片而不会拉进结构体定义。
//
// 【与 RHI 的镜像关系】Engine/RHI/RHI/Types.h 里还有一份手抄的镜像常量，那是**分层**
// 造成的：HugEngineShader 依赖 HugEngineRHI，RHI 不能反向包含 Shader。改这里的编号
// 必须同步那边（若只改一侧，描述符集布局就会与 shader 的 [[vk::binding]] 错位）。
// ============================================================

#include "ShaderTypes/GPUDefines.slang"

GPU_CONST_BEGIN

// --- Descriptor Set 索引 ---
// 【编号是两端的契约】shader 侧 `[[vk::binding(binding, set)]]` 的第二个参数、C++ 侧
// `BindDescriptorSet(set, 集)` 的第一个参数，必须逐位一致，否则布局与声明对不上。
// 【名字与用途有偏差，以行尾注释为准】set=0 是"逐帧 + 各 pass 输入"，
// 光栅路径的 bindless 三件套（5/6/30）与 TLAS（binding 0）都在这里；
// set=1 / set=2 目前**只有 RT 命中着色器**使用（3 个 rchit / 2 个 rchit）。
GPU_CONST(GPU_UINT kGPUDescSet_PerFrame = 0);   // set=0: 逐帧 + 每 pass 输入（bindless 数组 / TLAS 也在此）
GPU_CONST(GPU_UINT kGPUDescSet_Material = 1);   // set=1: RT 命中着色器的场景级数据（材质图集 / 命中点光源），非逐材质切换
GPU_CONST(GPU_UINT kGPUDescSet_Bindless = 2);   // set=2: RT 命中着色器的 bindless 纹理数组（光栅路径不用此 set）

// --- 共享 Binding 索引（set=0，Forward / Deferred / GBuffer 共用）---
GPU_CONST(GPU_UINT kGPUBinding_ObjectData       = 2);   // Object SSBO
GPU_CONST(GPU_UINT kGPUBinding_BindlessTextures = 5);   // Bindless 纹理数组
GPU_CONST(GPU_UINT kGPUBinding_BindlessSamplers = 6);   // Bindless 采样器数组
GPU_CONST(GPU_UINT kGPUBinding_BindlessSSBO     = 30);  // Bindless SSBO 数组（最高 binding，VARIABLE_COUNT）
GPU_CONST(GPU_UINT kGPUBinding_LightGrid        = 7);   // 光源网格 SSBO
GPU_CONST(GPU_UINT kGPUBinding_LightIndexList   = 8);   // 光源索引列表 SSBO

// --- Forward 管线 Binding 索引 ---
GPU_CONST(GPU_UINT kGPUBinding_Lights          = 1);   // GPULight[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_ShadowData      = 3);   // GPUShadowData[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_ShadowMap0      = 4);   // CSM cascade 0
GPU_CONST(GPU_UINT kGPUBinding_PointShadow     = 9);   // 点光源阴影 Cubemap
GPU_CONST(GPU_UINT kGPUBinding_ShadowMap1      = 10);  // CSM cascade 1
GPU_CONST(GPU_UINT kGPUBinding_ShadowMap2      = 11);  // CSM cascade 2
GPU_CONST(GPU_UINT kGPUBinding_IrradianceMap   = 12);  // IBL 辐照度图
GPU_CONST(GPU_UINT kGPUBinding_PrefilterMap    = 13);  // IBL 预滤波环境图
GPU_CONST(GPU_UINT kGPUBinding_BRDF_LUT        = 14);  // BRDF 积分 LUT
GPU_CONST(GPU_UINT kGPUBinding_RSMPosition     = 15);  // RSM 位置图
GPU_CONST(GPU_UINT kGPUBinding_RSMFlux         = 16);  // RSM 法线图（任务 30 起：只放编码法线）
// RSM VPL 辐射度图（任务 30）。绑定号只对 **Forward 的 per-frame set** 有意义：
// Deferred 的 Lighting set 里 17 是 kGPUBinding_Lights_DL，而 Deferred 的 RSM 源走共享的
// `RSM_Indirect` pass（它有自己的描述符集），故两边不会同时声明同一个号。
GPU_CONST(GPU_UINT kGPUBinding_RSMRadiance     = 17);  // RSM VPL 辐射度图（Forward 内联路径用）

// --- Deferred 管线 Binding 索引 ---
GPU_CONST(GPU_UINT kGPUBinding_GBufferA      = 0);   // GBuffer albedo+metallic
GPU_CONST(GPU_UINT kGPUBinding_GBufferB      = 1);   // GBuffer normal+roughness
GPU_CONST(GPU_UINT kGPUBinding_GBufferC      = 2);   // GBuffer emissive+ao
GPU_CONST(GPU_UINT kGPUBinding_Depth         = 3);   // 深度缓冲
GPU_CONST(GPU_UINT kGPUBinding_GBufferE      = 23);  // GBuffer worldPos（MRT4）
GPU_CONST(GPU_UINT kGPUBinding_GBufferF      = 28);  // GBuffer disneyA（anisotropic/subsurface/specular/sheen，MRT5）
GPU_CONST(GPU_UINT kGPUBinding_GBufferG      = 29);  // GBuffer disneyB（clearcoat/clearcoatGloss/specularTint.rg，MRT6）
GPU_CONST(GPU_UINT kGPUBinding_SpotShadow    = 24); // 聚光灯阴影（原 9 与点光冲突，改独立 binding 24）
GPU_CONST(GPU_UINT kGPUBinding_SpotShadow_DL = 9);  // 聚光灯阴影（Deferred Lighting set 专用：24 已被 RT 阴影遮罩占用）
GPU_CONST(GPU_UINT kGPUBinding_RectShadow    = 25); // 矩形面光阴影（独立 binding）
// 阴影专用对象数据 SSBO（26 在 Forward per-frame set 中空闲）：阴影 pass 的 `objectIndex` 按
// "全部投影物体"编号，与场景绘制用的"视锥剔除后可见列表"编号**不是同一套** ⇒ 两者必须是两个
// 同时绑定的 SSBO。靠 CPU 在录制中途把 binding 2 换来换去是无效的：GPU 执行时只看到提交前的
// 最终描述符状态，于是阴影 pass 会去读场景缓冲、索引错位（读到别的物体的矩阵）。
GPU_CONST(GPU_UINT kGPUBinding_ShadowObjects = 26); // 阴影专用 GPUObjectData[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_Lights_DL     = 17);  // GPULight[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_ShadowData_DL = 18);  // GPUShadowData[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_SSGI          = 19);  // SSGI 间接光照
GPU_CONST(GPU_UINT kGPUBinding_SSAO_DL       = 20);  // SSAO
GPU_CONST(GPU_UINT kGPUBinding_SSR           = 21);  // SSR
GPU_CONST(GPU_UINT kGPUBinding_DDGIProbes    = 22);  // DDGI 探针
GPU_CONST(GPU_UINT kGPUBinding_DDGIGridParams = 6);  // DDGI 探针网格参数 UBO（Lighting 采样 DDGI 用，binding 6 在 Deferred Lighting set 中空闲）
GPU_CONST(GPU_UINT kGPUBinding_RSMIndirect    = 5);  // RSM 间接光 E（半分辨率，任务 16；binding 5 在 Deferred Lighting set 中空闲）
// 光照图（原任务 31，该任务**已取消**；通道与常量作为已落地基础设施保留）。
// 【校验修复 §0.6.2】此前这里把键纹理放在 4、把烘焙图放在 7，理由是"两个号在 Deferred
// Lighting set 中空闲"——该前提不成立：Lighting set 的 4 是 CSM 级联 0
//（kGPUBinding_ShadowMap0，DeferredLighting.frag 真的在采样）、7 是 LightGrid，
// 于是同一个 set 里 4 出现两次，校验层报
// VUID-VkDescriptorSetLayoutCreateInfo-binding-00279；更糟的是 C++ 侧对 binding 4 的两次
// 更新会互相覆盖，光照图键会把 CSM0 的描述符顶掉。现改用该 set 里唯一空闲的 30。
// kGPUBinding_Lightmap（烘焙图本身）目前无人使用，恢复该任务时同样要另择空闲号。
GPU_CONST(GPU_UINT kGPUBinding_LightmapKey    = 30);  // GBuffer 光照图键（uv0.xy + objectIndex）
GPU_CONST(GPU_UINT kGPUBinding_Lightmap       = 7);   // 烘焙光照图（Texture2DArray，页=物体）—— 暂未使用

// --- 光追（RT）效果 Binding 索引（Lighting 采样 RT 效果输出纹理）---
GPU_CONST(GPU_UINT kGPUBinding_RT_ShadowMask = 24);  // RT 阴影遮罩（R16_FLOAT）
GPU_CONST(GPU_UINT kGPUBinding_RT_Reflection = 25);  // RT 反射（RGBA16_FLOAT）
GPU_CONST(GPU_UINT kGPUBinding_RT_AO         = 26);  // RT 环境光遮蔽（R8_UNORM）
GPU_CONST(GPU_UINT kGPUBinding_RT_GI         = 27);  // RT 间接漫反射（RGBA16_FLOAT）
GPU_CONST(GPU_UINT kGPUBinding_GIBlendParams = 31);  // GI 各通道分层合成参数 UBO（3 通道 × 源数组）
// Lumen 输出（间接漫反射/镜面共用一张，骨架阶段）。0~31 在 Deferred Lighting set 里已全部占满，
// 故取 32 —— 需要同步在 LightingPass 的描述符集布局里加同号绑定，否则采样读到未绑定描述符。
GPU_CONST(GPU_UINT kGPUBinding_Lumen         = 32);  // Lumen 输出（RGBA16_FLOAT）

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

#endif // HUGENGINE_GPU_BINDINGS
