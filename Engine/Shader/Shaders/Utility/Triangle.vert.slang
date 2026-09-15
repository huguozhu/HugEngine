// ============================================================
// Triangle.vert.slang — 三角形示例顶点着色器
// ============================================================

layout(location = 0) in float2 inPosition;

struct VSOutput {
    float4 position : SV_Position;
};

VSOutput vertexMain() {
    VSOutput output;
    output.position = float4(inPosition, 0.0, 1.0);
    return output;
}
