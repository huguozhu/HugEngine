// ParticleRender.vert — Billboard 粒子顶点着色器
// 从 StructuredBuffer 读取粒子数据，扩展为始终面向相机的四边形

#include "ParticleTypes.slang"

// 输入: 预定义 Billboard 顶点 (6 vertices, 2 triangles)
// 存储在 u_Vertices buffer 中，通过 SV_VertexID 索引

[[vk::binding(0, 0)]] StructuredBuffer<float4> u_Vertices;        // 6×float4 (pos.xy, uv)
[[vk::binding(1, 0)]] StructuredBuffer<SortInfo> u_SortIndices;    // 排序后的粒子索引
[[vk::binding(2, 0)]] StructuredBuffer<Particle> u_ParticleBuffer;

[[vk::push_constant]] cbuffer PushConstant {
    GpuRenderParam u_RenderParam;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float  lifeRatio : TEXCOORD1;  // 生命比例 (current/total)
    uint   particleIdx : TEXCOORD2; // 粒子索引（用于颜色 hash）
    float3 worldPos : TEXCOORD3;   // 世界位置 (for soft particles)
};

VSOutput vertexMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    // 1. 从排序索引获取粒子索引
    uint renderCount;
    // count 从 DrawArgs 传入 (通过 Indirect Draw)
    SortInfo si = u_SortIndices[instanceID];
    uint particleIdx = si.particle_index;
    Particle p = u_ParticleBuffer[particleIdx];

    // 2. 获取 Billboard 顶点数据 (预定义 6 顶点)
    float4 vertexData = u_Vertices[vertexID];
    float2 corner = vertexData.xy;   // (-1,-1), (1,-1), (-1,1), (1,1)...
    float2 uv     = vertexData.zw;   // (0,0), (1,0), (0,1), (1,1)...

    // 3. 相机矩阵分解
    float4x4 view = u_RenderParam.viewMatrix;
    float4x4 proj = u_RenderParam.projMatrix;

    // 提取相机右/上向量 (用于 Billboard 对齐)
    float3 camRight   = float3(view[0].x, view[1].x, view[2].x);
    float3 camUp      = float3(view[0].y, view[1].y, view[2].y);

    // 4. 计算 Billboard 世界位置
    float  particleSize = p.position.w;
    float3 worldPos = p.position.xyz
        + camRight * corner.x * particleSize
        + camUp    * corner.y * particleSize;

    // 5. 投影
    float4 worldPos4 = float4(worldPos, 1.0);
    float4 clipPos   = mul(proj, mul(view, worldPos4));

    VSOutput o;
    o.position   = clipPos;
    o.uv         = uv;
    o.lifeRatio  = p.life_time.x / max(p.life_time.y, 0.001);
    o.particleIdx = particleIdx;  // 唯一粒子索引 → 片元着色器颜色 hash
    o.worldPos   = worldPos;
    return o;
}
