// ParticleSimulate.comp — 粒子物理模拟
// 遍历粒子池中所有存活粒子，更新位置/速度/生命，死粒子回收到 DeadList

#include "ParticleTypes.slang"

[[vk::binding(0, 0)]] StructuredBuffer<uint>              u_AlivePreIndices;   // Emit 后的新粒子索引（保留兼容）
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>              u_AlivePostIndices;  // 本帧存活粒子索引
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
    // 直接以线程 ID 作为粒子索引，遍历整个粒子池
    uint idx = tid.x;
    if (idx >= u_SimParam.max_particles) return;

    // 读取粒子数据
    Particle p = u_ParticleBuffer[idx];

    // 跳过未初始化的粒子（life_time.y == 0 表示槽位从未被使用）
    if (p.life_time.y <= 0.0) return;

    // 跳过已死亡的粒子（life_time.x >= life_time.y）
    if (p.life_time.x >= p.life_time.y) return;

    float dt = u_SimParam.delta_time;

    // 更新当前生命
    p.life_time.x += dt;

    // 检查死亡
    if (p.life_time.x >= p.life_time.y) {
        // 粒子死亡 → 回收索引到 DeadList，标记槽位为空
        uint deadSlot;
        InterlockedAdd(u_Counters[0].dead_count, 1, deadSlot);
        u_DeadList[deadSlot] = idx;
        p.life_time.y = 0.0;  // 标记为未初始化，下次 emit 可重用此槽
        p.position.w = 0.0;   // 大小归零
        u_ParticleBuffer[idx] = p;
        return;  // 不写入 alivePost
    }

    // 更新速度（重力 + 风力等外力）
    p.velocity.xyz += u_SimParam.gravity * dt;

    // 更新位置（Euler 积分）
    p.position.xyz += p.velocity.xyz * dt;

    // 更新纹理帧
    p.tex_index = updateTexIndex(p.tex_index, p.life_time.x, p.life_time.y,
                                  u_SimParam.tex_rows_cols.x, u_SimParam.tex_rows_cols.y);

    // 保存更新后的粒子
    u_ParticleBuffer[idx] = p;

    // 写入 alivePost 索引（供 Culling 使用）
    uint aliveSlot;
    InterlockedAdd(u_Counters[0].alive_count[1], 1, aliveSlot);
    u_AlivePostIndices[aliveSlot] = idx;
}
