// ParticleCulling.comp — 粒子视锥剔除 + 间接绘制参数写入

#include "ParticleTypes.slang"

[[vk::binding(0, 0)]] StructuredBuffer<uint>              u_AlivePostIndices;  // post_sim 存活粒子
[[vk::binding(1, 0)]] StructuredBuffer<Particle>           u_ParticleBuffer;
[[vk::binding(2, 0)]] RWStructuredBuffer<SortInfo>          u_SortIndices;       // 排序索引 (深度)
[[vk::binding(3, 0)]] RWStructuredBuffer<ParticleCounters>  u_Counters;
[[vk::binding(4, 0)]] RWStructuredBuffer<ParticleDrawArgs>  u_DrawArgs;          // 间接绘制参数

[[vk::push_constant]] cbuffer PushConstant {
    GpuCullingParam u_CullingParam;
};

// 视锥 AABB 测试
bool frustumCullSphere(float3 center, float radius) {
    [unroll]
    for (int i = 0; i < 6; i++) {
        float d = dot(u_CullingParam.frustumPlanes[i].xyz, center)
                + u_CullingParam.frustumPlanes[i].w;
        if (d < -radius) return false;
    }
    return true;
}

[shader("compute")]
[numthreads(PARTICLE_CS_X_SIZE, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    // 读取存活粒子数 (post_sim)
    uint simCount;
    InterlockedAdd(u_Counters[0].alive_count[1], 0, simCount);
    if (tid.x >= simCount) return;

    uint particleIdx = u_AlivePostIndices[tid.x];
    Particle p = u_ParticleBuffer[particleIdx];

    // 视锥剔除 (用粒子包围球)
    float size = max(1.0, p.position.w);
    if (!frustumCullSphere(p.position.xyz, size))
        return;

    // 计算视图空间深度 (用于排序)
    float4 viewPos = mul(u_CullingParam.viewProj, float4(p.position.xyz, 1.0));
    float depth = viewPos.z / max(viewPos.w, 0.0001);

    // 写入排序索引
    uint renderSlot;
    InterlockedAdd(u_Counters[0].render_count, 1, renderSlot);
    SortInfo si;
    si.particle_index = particleIdx;
    si.particle_depth = depth;
    u_SortIndices[renderSlot] = si;
}

// 注意: DrawArgs 在 ParticleTickBegin 中写入
// (instanceCount = render_count, vertexCount = 6)
