// PT_Full.rmiss.slang — 全路径追踪 Miss 着色器
//
// 写入天空辐射度 + hitT=-1 哨兵（a<0 = 未命中）。
// 该哨兵同时服务 shadow ray：shadow ray 加
//   RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
// 后，命中不执行任何着色器（payload 保持调用前的 a=0 = 被遮挡），
// miss 则本着色器写 a=-1 = 未遮挡。
//
// PathPayload 定义必须与 PT_Full.rchit 逐字节一致（SPIR-V 校验）。

#include "PT_Common.slang"  // PathPayload + SampleSky

[shader("miss")]
void main(inout PathPayload payload) {
    payload.albedoMetallic = float4(0, 0, 0, 0);
    payload.normalRough    = float4(0, 0, 0, 1);
    payload.emissiveT      = float4(SampleSky(WorldRayDirection(), 1.0), -1.0);
}
