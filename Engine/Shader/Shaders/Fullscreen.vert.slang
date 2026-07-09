// Fullscreen.vert.slang — 全屏三角形顶点着色器（无顶点缓冲，用 SV_VertexID 生成）

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput vertexMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    // 2 个三角形覆盖全屏（无需顶点缓冲）
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
