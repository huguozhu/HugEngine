// PT_Full.rmiss.slang — 全路径追踪 Miss 着色器
//
// 写入天空辐射度 + hitT=-1 哨兵（a<0 = 未命中）。
// 该哨兵同时服务 shadow ray：shadow ray 加
//   RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
// 后，命中不执行任何着色器（payload 保持调用前的 a=0 = 被遮挡），
// miss 则本着色器写 a=-1 = 未遮挡。
//
// PathPayload 定义必须与 PT_Full.rchit 逐字节一致（SPIR-V 校验）。
// 背景：程序渐变天空（曾尝试绑定天空盒 Cubemap，但 Slang 组合采样器
// 声明会破坏描述符集使材质绑定读到 0，故回退程序天空）。

#include "PT_Common.slang"  // PathPayload + SampleSky

// STBN 时空蓝噪声（PT_Common 随机函数体引用，本 stage 未用但需声明以通过编译）
[[vk::binding(10, 0)]] Texture3D<float4> g_BlueNoise;

[[vk::push_constant]] PTPushConstant g_PC;

[shader("miss")]
void main(inout PathPayload payload) {
    payload.albedoMetallic = float4(0, 0, 0, 0);
    payload.normalRough    = float4(0, 0, 0, 1);
    payload.emissiveT      = float4(SampleSky(WorldRayDirection(), g_PC.skyIntensity), -1.0);
}
