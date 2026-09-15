// ============================================================
// IBL_Irradiance.vert.slang — 辐照度卷积顶点着色器
//
// 全屏三角形（SV_VertexID），用逆 VP 将 NDC 反算为世界空间方向。
// 每个 Cubemap 面使用对应的 ViewProj 矩阵。
// ============================================================

[[vk::push_constant]] cbuffer IBLPush {
    float4x4 invViewProj;  // 当前面的逆 ViewProj
};

struct VSOutput {
    float4 position            : SV_Position;
    [[vk::location(0)]] float3 worldDir;  // 世界空间采样方向
};

VSOutput vertexMain(uint vid : SV_VertexID) {
    float2 clipPos[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0),
    };
    float2 pos = clipPos[vid];

    VSOutput output;
    output.position = float4(pos.x, pos.y, 1.0, 1.0);

    // NDC → 世界空间方向
    float4 worldPos = mul(invViewProj, float4(pos.x, pos.y, 1.0, 1.0));
    output.worldDir = worldPos.xyz / worldPos.w;
    return output;
}
