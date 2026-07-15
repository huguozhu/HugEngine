// ParticleEmit.comp — 粒子发射
// 从 DeadList 取空闲槽位，初始化粒子属性

#include "ParticleTypes.slang"

[[vk::binding(0, 0)]] StructuredBuffer<uint>              u_DeadList;       // 空闲粒子索引
[[vk::binding(1, 0)]] RWStructuredBuffer<ParticleCounters>  u_Counters;       // 全局计数器
[[vk::binding(2, 0)]] RWStructuredBuffer<uint>              u_AliveIndices;   // 发射后存活粒子索引 (pre_sim)
[[vk::binding(3, 0)]] RWStructuredBuffer<Particle>          u_ParticleBuffer; // 粒子数据池
[[vk::binding(4, 0)]] StructuredBuffer<float>               u_RandomFloats;   // 预生成随机数

[[vk::push_constant]] cbuffer PushConstant {
    GpuEmitParam u_EmitParam;
};

// 在球体内生成随机方向
float3 randomDirectionSphere(uint seed) {
    float u = u_RandomFloats[(seed * 3 + 0) % RANDOM_FLOAT_NUM] * 2.0 - 1.0;
    float v = u_RandomFloats[(seed * 3 + 1) % RANDOM_FLOAT_NUM] * 2.0 - 1.0;
    float w = u_RandomFloats[(seed * 3 + 2) % RANDOM_FLOAT_NUM];
    float theta = 6.283185 * u;     // 2*PI
    float phi = acos(2.0 * v - 1.0);
    return float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
}

// 在盒子内生成随机位置
float3 randomPositionBox(float3 boxSize, uint seed) {
    float x = (u_RandomFloats[(seed * 3 + 0) % RANDOM_FLOAT_NUM] - 0.5) * boxSize.x;
    float y = (u_RandomFloats[(seed * 3 + 1) % RANDOM_FLOAT_NUM] - 0.5) * boxSize.y;
    float z = (u_RandomFloats[(seed * 3 + 2) % RANDOM_FLOAT_NUM] - 0.5) * boxSize.z;
    return float3(x, y, z);
}

// 发射方向计算
float3 computeEmitDirection(uint seed) {
    float3 dir;
    float spread = u_EmitParam.direction_spread_percent;
    float3 baseDir = normalize(u_EmitParam.direction);

    switch (u_EmitParam.emit_direction_type) {
        case Emit_Direction_Type_Directional: {
            // 锥形扩散
            float coneAngle = spread * 3.14159 / 180.0;  // 百分比 → 弧度
            float u1 = u_RandomFloats[(seed * 4 + 0) % RANDOM_FLOAT_NUM];
            float u2 = u_RandomFloats[(seed * 4 + 1) % RANDOM_FLOAT_NUM];
            float theta = 6.283185 * u1;
            float phi = acos(1.0 - u2 * (1.0 - cos(coneAngle)));
            // 构建 TBN 基向量
            float3 tangent, bitangent;
            if (abs(baseDir.y) < 0.999) {
                tangent = normalize(cross(float3(0, 1, 0), baseDir));
            } else {
                tangent = normalize(cross(float3(1, 0, 0), baseDir));
            }
            bitangent = cross(baseDir, tangent);
            dir = baseDir * cos(phi)
                + tangent * sin(phi) * cos(theta)
                + bitangent * sin(phi) * sin(theta);
            break;
        }
        case Emit_Direction_Type_Uniform_2D: {
            float angle = u_RandomFloats[(seed * 4 + 0) % RANDOM_FLOAT_NUM] * 6.283185;
            dir = float3(cos(angle), 0, sin(angle));
            break;
        }
        case Emit_Direction_Type_Uniform_3D:
        default:
            dir = randomDirectionSphere(seed * 4 + 3);
            break;
    }
    return normalize(dir);
}

[shader("compute")]
[numthreads(PARTICLE_CS_X_SIZE, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    // 仅发射 emit_count 个粒子
    uint emitCount;
    InterlockedAdd(u_Counters[0].emit_count, 0, emitCount);
    if (tid.x >= emitCount) return;

    // 1. 从 DeadList 获取空闲粒子索引
    uint deadSlot;  // DeadList 中的槽位
    InterlockedAdd(u_Counters[0].dead_count, uint(-1), deadSlot);
    if (deadSlot == 0) return;  // 无可用粒子

    uint particleIdx = u_DeadList[deadSlot - 1];

    // 2. 初始化粒子属性
    Particle p;
    uint seed = tid.x * 7 + 13;

    // 随机生命
    float lifeRange = u_EmitParam.max_life_time - u_EmitParam.min_life_time;
    float life = u_EmitParam.min_life_time
        + u_RandomFloats[(seed + 0) % RANDOM_FLOAT_NUM] * lifeRange;
    p.life_time = float3(0, life, 0);  // cur=0, total=life

    // 纹理子帧索引
    p.tex_index = uint(u_RandomFloats[(seed + 1) % RANDOM_FLOAT_NUM]
        * float(u_EmitParam.tex_rows_cols.x * u_EmitParam.tex_rows_cols.y));

    // 发射位置
    float3 emitPos = u_EmitParam.position;
    if (u_EmitParam.emit_shape == Emit_Shape_Type_Sphere) {
        float r = u_RandomFloats[(seed + 2) % RANDOM_FLOAT_NUM] * u_EmitParam.sphere_radius;
        emitPos += randomDirectionSphere(seed + 5) * r;
    } else if (u_EmitParam.emit_shape == Emit_Shape_Type_Box) {
        emitPos += randomPositionBox(u_EmitParam.box_size, seed + 6);
    }
    p.position = float4(emitPos, 1.0);  // w=1.0 初始大小

    // 速度
    float speed = u_EmitParam.min_init_speed
        + u_RandomFloats[(seed + 3) % RANDOM_FLOAT_NUM]
        * (u_EmitParam.max_init_speed - u_EmitParam.min_init_speed);
    float3 dir = computeEmitDirection(seed);
    p.velocity = float4(dir * speed, 0.0);  // w=阻尼系数

    // 3. 写入粒子缓冲
    u_ParticleBuffer[particleIdx] = p;

    // 4. 写入存活索引 (pre_sim)
    uint aliveSlot;
    InterlockedAdd(u_Counters[0].alive_count[0], 1, aliveSlot);
    u_AliveIndices[aliveSlot] = particleIdx;
}
