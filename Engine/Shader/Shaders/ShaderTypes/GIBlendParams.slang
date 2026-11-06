#ifndef HUGENGINE_GPU_GI_BLEND_PARAMS
#define HUGENGINE_GPU_GI_BLEND_PARAMS
// ============================================================
// ShaderTypes/GIBlendParams.slang — 分层合成参数 UBO 布局（C++/Slang 共享）
//
// 本切片**含 float4 / float4x4**，C++ 侧包含它之前必须先 include Math/Math.h（he::float4）。
// 纯 GI 数据模型（GI/GITypes.h）不要包含本切片 —— 那会把 GLM 拉进 RHI-free 的单测。
//
// 【填充规则】_padBlend / _padRsm 必须是 float4：写成 float[3] 时 Slang 的 std140 数组
// 步长是 16、C++ 是 12，数组之后的 rsmLightViewProj / rsmValid 两端偏移全部错开，
// 表现为"RSM 源静默不产出且没有任何报错"（任务 34 / §9.2-AF）。
// ============================================================

#include "ShaderTypes/GPUDefines.slang"

GPU_STRUCT GIBlendParams {
    GIChannelBlendParams diffuse;        // 间接漫反射
    GIChannelBlendParams specular;       // 间接镜面
    GIChannelBlendParams ao;             // 环境光遮蔽
    // 本帧 RSM VPL 采样缩放（任务 30 / §9.2-AA 的量级归一）：
    //   `E/π = rsmVplScale · Σ (L_v · cosθ_s · cosθ_r / d²)`
    // 由**该管线自己的** RSM 光源视锥与采样图案推出（C++ 侧 `GI/RSMFrustum.h` 的
    // `RSMVplScale`，单测锁住"场景均匀缩放时估计量不变"）。
    // 【谁读它】只有 `PBR.frag`（Forward）的**内联** RSM 求和读 —— Forward 没有 GBuffer，
    // 无法使用共享的半分辨率 `RSM_Indirect` pass。Deferred 的 RSM 源走那个 pass，
    // 其缩放由该 pass 自己的参数 UBO 传入，故 Deferred 侧这里填 0（**不要**误认为它生效）。
    float        rsmVplScale;
    // 【填充必须用 float4，不能用 float[3]】共享结构体是 C++/Slang **两端各自**按自己的规则
    // 布局的：Slang 的 cbuffer（std140）里**数组元素步长固定 16 字节**，而 C++ 侧 `float[3]`
    // 只占 12 字节 ⇒ 数组之后的所有成员在两端的偏移都不一致。实测（`slangc -reflection-json`）：
    // 写成 `float _padBlend[3]` 时 Slang 把 `rsmLightViewProj` 放在 304、`rsmValid` 放在 368、
    // 块大小 432，而 C++ 分别是 256 / 320 / 336 —— 于是 PBR 读到的 `rsmValid` 恒为 0
    // （落在 C++ 未写入的区间），RSM 源静默不产出。改用 float4 后两端逐字段一致（见 §9.2-AF）。
    float4       _padBlend;
    // RSM 查找用的光源 VP（任务 34 / §9.2-AD）：**必须与 RSM_Generate pass 用的那个是同一个**
    // ——即按场景包围盒拟合的固定光锥（`GI/RSMFrustum.h`），不是 CSM 级联 0 的 VP。
    // 【为什么不能用 CSM 的】① CSM 的 VP 拟合**相机视锥** ⇒ RSM 内容随视角变化，而它是被当作
    // 世界空间源使用的；② 那个 VP 由 Shadow pass 在**执行时**才写进阴影系统，而帧图里
    // Shadow 与 RSM_Generate 声明的是互不相干的纹理、没有依赖边 ⇒ 顺序不受保证（实测：
    // 执行顺序一变 RSM 三张图就只剩清除值）。
    // 【谁读它】只有 `PBR.frag`（Forward）的内联 5×5 求和；Deferred 的 RSM 源走共享的
    // `RSM_Indirect` pass，那个 pass 有自己的一套参数，故 Deferred 侧这里填单位矩阵。
    float4x4     rsmLightViewProj;
    // 本帧「RSM 内联查找可用」：RSM 在层栈 ∧ 有投影方向光 ∧ RSM pass 本帧真的注册了
    // （三个条件都在 C++ 侧判定，见 `ForwardPipeline::RefreshRSMFrustum`）。
    // 不用层栈里有没有 RSM 当判据：那样在 pass 没跑时会采到上一帧/未初始化的纹理（§9.2-T）。
    float        rsmValid;
    float4       _padRsm;                // 同 _padBlend：填充一律用 float4
};

#ifdef __cplusplus
// C++ 分支自清理：允许 C++ 头直接 include 本切片而不泄漏宏；Slang 分支由总入口收尾
#undef GPU_UINT
#undef GPU_INT
#undef GPU_STRUCT
#undef GPU_CONST
#undef GPU_CONST_BEGIN
#undef GPU_CONST_END
#endif

#endif // HUGENGINE_GPU_GI_BLEND_PARAMS
