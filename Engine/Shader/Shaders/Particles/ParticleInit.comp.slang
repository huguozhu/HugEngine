// ParticleInit.comp — 粒子系统初始化
// 填充 DeadList + 清零 ParticleCounters

#include "ParticleTypes.slang"

// PushConstant
[[vk::push_constant]] cbuffer PushConstant {
    uint u_MaxParticles;
};

// 输出
[[vk::binding(0, 0)]] RWStructuredBuffer<uint>              u_DeadList;       // 可用粒子索引列表
[[vk::binding(1, 0)]] RWStructuredBuffer<ParticleCounters>   u_Counters;       // 全局计数器

// 将线程 ID 写入 DeadList，初始化所有粒子槽位
[shader("compute")]
[numthreads(PARTICLE_CS_X_SIZE, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= u_MaxParticles) return;

    // DeadList[tid.x] = tid.x (所有槽位初始都可用)
    u_DeadList[tid.x] = tid.x;
}

// 注意: Counters 清零在 ParticleTickBegin 中完成
// (因为 Begin 在 Init 之后调用，保证顺序)
