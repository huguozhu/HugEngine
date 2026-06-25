// ============================================================
// ToneMap.vert.slang — 全屏三角形（无需顶点缓冲）
//
// 使用 SV_VertexID 生成覆盖整个屏幕的三角形
// ============================================================

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOutput vertexMain(uint vid : SV_VertexID) {
    VSOutput output;
    // 全屏三角形：3 个顶点覆盖整个 NDC 空间
    // vid=0 → (-1,-1), vid=1 → (3,-1), vid=2 → (-1,3)
    output.uv = float2((vid << 1) & 2, vid & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}
