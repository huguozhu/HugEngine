// ParticleRender.vert — Billboard 粒子顶点着色器
// 在 View Space 中构建始终面向相机的四边形

#include "ParticleTypes.slang"

[[vk::binding(0, 0)]] StructuredBuffer<float4> u_Vertices;        // 6×float4 (pos.xy, uv)
[[vk::binding(1, 0)]] StructuredBuffer<SortInfo> u_SortIndices;    // 排序后的粒子索引
[[vk::binding(2, 0)]] StructuredBuffer<Particle> u_ParticleBuffer;

[[vk::push_constant]] cbuffer PushConstant {
    GpuRenderParam u_RenderParam;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float  lifeRatio : TEXCOORD1;
    uint   particleIdx : TEXCOORD2;
    float3 worldPos : TEXCOORD3;
};

VSOutput vertexMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    // 1. 获取粒子数据
    SortInfo si = u_SortIndices[instanceID];
    uint particleIdx = si.particle_index;
    Particle p = u_ParticleBuffer[particleIdx];

    // 2. 获取 Billboard 顶点角偏移和 UV
    float4 vertexData = u_Vertices[vertexID];
    float2 corner = vertexData.xy;   // (-1,-1), (1,-1), (-1,1), (1,1)...
    float2 uv     = vertexData.zw;   // (0,0), (1,0), (0,1), (1,1)...

    float4x4 view = u_RenderParam.viewMatrix;
    float4x4 proj = u_RenderParam.projMatrix;

    // 3. 粒子位置变换到 View Space
    float4 viewPos = mul(view, float4(p.position.xyz, 1.0));

    // 4. 在 View Space 的 XY 平面扩展角偏移（始终面向相机）
    float size = p.position.w;
    viewPos.xy += corner * size;

    // 5. 投影到 Clip Space
    float4 clipPos = mul(proj, viewPos);

    // 6. 计算世界空间角偏移（近似：粒子世界位置 + 相机轴偏移）
    // 从 View 矩阵提取相机右和上向量
    float3 camRight = float3(view[0].x, view[1].x, view[2].x);
    float3 camUp    = float3(view[0].y, view[1].y, view[2].y);
    float3 worldPos = p.position.xyz
        + camRight * corner.x * size
        + camUp    * corner.y * size;

    VSOutput o;
    o.position    = clipPos;
    o.uv          = uv;
    o.lifeRatio   = p.life_time.x / max(p.life_time.y, 0.001);
    o.particleIdx = particleIdx;
    o.worldPos    = worldPos;
    return o;
}
