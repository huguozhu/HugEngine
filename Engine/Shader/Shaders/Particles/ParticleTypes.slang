// ParticleTypes.slang — GPU 粒子系统共享类型
// 与 C++ Engine/Scene/Scene/ParticleComponent.h 保持一致
#pragma once

// ── 常量 ──
static const uint PARTICLE_CS_X_SIZE    = 32;
static const uint BITONIC_BLOCK_SIZE    = 512;
static const uint TRANSPOSE_BLOCK_SIZE  = 16;
static const uint RANDOM_FLOAT_NUM      = 512;

// ── 发射方向类型 ──
static const uint Emit_Direction_Type_Directional    = 0;
static const uint Emit_Direction_Type_BiDirectional  = 1;
static const uint Emit_Direction_Type_Uniform_2D     = 2;
static const uint Emit_Direction_Type_Uniform_3D     = 3;

// ── 发射形状类型 ──
static const uint Emit_Shape_Type_Sphere = 0;
static const uint Emit_Shape_Type_Box    = 1;

// ── 纹理时间采样类型 ──
static const uint Tex_Time_Sampling_Type_Play_Once          = 0;
static const uint Tex_Time_Sampling_Type_Play_Loop          = 1;
static const uint Tex_Time_Sampling_Type_Random_Still_Frame = 2;
static const uint Tex_Time_Sampling_Type_Random_Loop        = 3;

// ── 粒子数据 (48 bytes) ──
struct Particle {
    float3 life_time;       // x: 当前生命, y: 总生命, z: 纹理时间累积
    uint   tex_index;       // 纹理子帧索引
    float4 velocity;        // xyz: 速度方向+大小, w: 阻尼
    float4 position;        // xyz: 世界位置, w: 当前大小
};

// ── 全局计数器 (原子操作) ──
struct ParticleCounters {
    uint   dead_count;          // DeadList 中可用数量
    uint   alive_count[2];      // [0]=pre_sim, [1]=post_sim
    uint   emit_count;          // 本帧发射数
    uint   simulate_count;      // 本帧模拟数
    uint   render_count;        // 本帧渲染数
    uint2  pad;
};

// ── 排序条目 (8 bytes) ──
struct SortInfo {
    uint   particle_index;
    float  particle_depth;
};

// ── GPU 参数结构体 ──

struct GpuEmitParam {
    float3  position;
    uint    max_particles;
    uint    emit_direction_type;
    float3  direction;
    float   direction_spread_percent;
    float   min_init_speed;
    float   max_init_speed;
    float   min_life_time;
    float   max_life_time;
    float3  box_size;
    int     emit_shape;
    float   sphere_radius;
    uint2   tex_rows_cols;
    uint    tex_time_sampling_type;
    float   min_size;          // 初始最小大小
    float   max_size;          // 初始最大大小
    uint    _pad3a;            // 手动对齐
    uint    _pad3b;
};

struct GpuSimulateParam {
    float   delta_time;
    float3  gravity;
    uint    tex_time_sampling_type;
    uint2   tex_rows_cols;
    float   tex_frames_per_sec;
    uint    max_particles;      // 粒子池总容量，用于遍历所有粒子
};

struct GpuCullingParam {
    float4x4 viewProj;
    float4   frustumPlanes[6];
};

struct GpuRenderParam {
    float4x4 viewMatrix;       // offset 0  (64B)
    float4x4 projMatrix;       // offset 64 (64B)
    uint2    texRowsCols;      // offset 128 (8B)
    float4   startColor;       // offset 136 (16B) — 出生颜色（lifeRatio=0）
    float4   endColor;         // offset 152 (16B) — 死亡颜色（lifeRatio=1）
};

struct GpuSortParam {
    uint sortLevel;
    uint descendMask;
    uint matrixWidth;
    uint matrixHeight;
};

// ── 间接参数 ──

struct DispatchArgs {
    uint dispatchX;
    uint dispatchY;
    uint dispatchZ;
};

struct ParticleDrawArgs {
    uint vertexCount;       // 6 (billboard 2 triangles)
    uint instanceCount;     // 渲染粒子数
    uint firstVertex;
    uint firstInstance;
};
