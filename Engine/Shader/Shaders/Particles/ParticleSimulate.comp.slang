// ParticleSimulate.comp — 粒子物理模拟
// 更新位置/速度/生命，死粒子回收到 DeadList

#include "ParticleTypes.slang"

[[vk::binding(0, 0)]] StructuredBuffer<uint>              u_AlivePreIndices;   // pre_sim 存活索引
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>              u_AlivePostIndices;  // post_sim 存活索引
[[vk::binding(2, 0)]] RWStructuredBuffer<ParticleCounters>  u_Counters;
[[vk::binding(3, 0)]] RWStructuredBuffer<Particle>          u_ParticleBuffer;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>              u_DeadList;
[[vk::binding(5, 0)]] StructuredBuffer<float>               u_RandomFloats;

[[vk::push_constant]] cbuffer PushConstant {
    GpuSimulateParam u_SimParam;
};

// 更新纹理帧索引
uint updateTexIndex(uint curIndex, float elapsed, float totalLife, uint rows, uint cols) {
    uint totalFrames = rows * cols;
    if (totalFrames <= 1) return curIndex;

    switch (u_SimParam.tex_time_sampling_type) {
        case Tex_Time_Sampling_Type_Play_Once: {
            float progress = elapsed / totalLife;
            return min(uint(progress * float(totalFrames)), totalFrames - 1);
        }
        case Tex_Time_Sampling_Type_Play_Loop: {
            float cycle = elapsed * u_SimParam.tex_frames_per_sec;
            return uint(cycle) % totalFrames;
        }
        case Tex_Time_Sampling_Type_Random_Still_Frame:
            return curIndex;  // 发射时已随机选择，保持不变
        case Tex_Time_Sampling_Type_Random_Loop: {
            float cycle = elapsed * u_SimParam.tex_frames_per_sec;
            float frac = frac(cycle);
            if (frac < 0.01) {  // 帧切换时重新随机
                uint seed = uint(elapsed * 1000.0);
                return uint(u_RandomFloats[seed % RANDOM_FLOAT_NUM] * float(totalFrames));
            }
            return curIndex;
        }
        default:
            return curIndex;
    }
}

[shader("compute")]
[numthreads(PARTICLE_CS_X_SIZE, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    // 读取 alive_pre_count
    uint simCount;
    InterlockedAdd(u_Counters[0].alive_count[0], 0, simCount);
    if (tid.x >= simCount) return;

    // 1. 获取粒子索引
    uint particleIdx = u_AlivePreIndices[tid.x];
    Particle p = u_ParticleBuffer[particleIdx];

    // 2. 更新生命
    float dt = u_SimParam.delta_time;
    p.life_time.x += dt;  // cur_life += dt

    // 3. 检查死亡
    if (p.life_time.x >= p.life_time.y) {
        // 粒子死亡 → 回收索引到 DeadList
        uint deadSlot;
        InterlockedAdd(u_Counters[0].dead_count, 1, deadSlot);
        u_DeadList[deadSlot] = particleIdx;
        return;  // 不写入 alive_post
    }

    // 4. 更新位置 (Euler 积分)
    float3 gravity = u_SimParam.gravity;
    p.velocity.xyz += gravity * dt;
    p.position.xyz += p.velocity.xyz * dt;

    // 5. 更新纹理帧
    p.tex_index = updateTexIndex(p.tex_index, p.life_time.x, p.life_time.y,
                                  u_SimParam.tex_rows_cols.x, u_SimParam.tex_rows_cols.y);

    // 6. 保存更新后的粒子
    u_ParticleBuffer[particleIdx] = p;

    // 7. 写入 alive_post 索引
    uint aliveSlot;
    InterlockedAdd(u_Counters[0].alive_count[1], 1, aliveSlot);
    u_AlivePostIndices[aliveSlot] = particleIdx;
}
