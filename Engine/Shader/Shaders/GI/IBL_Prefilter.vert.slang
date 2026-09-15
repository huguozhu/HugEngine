// ============================================================
// IBL_Prefilter.vert.slang — 预滤波环境贴图顶点着色器
//
// 与 Irradiance 相同：全屏三角形 + 逆 VP 反算世界方向。
// ============================================================

[[vk::push_constant]] cbuffer IBLPush {
    float4x4 invViewProj;
};

struct VSOutput {
    float4 position            : SV_Position;
    [[vk::location(0)]] float3 worldDir;
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

    float4 worldPos = mul(invViewProj, float4(pos.x, pos.y, 1.0, 1.0));
    output.worldDir = worldPos.xyz / worldPos.w;
    return output;
}
