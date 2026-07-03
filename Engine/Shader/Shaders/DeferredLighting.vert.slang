// DeferredLighting.vert — 全屏三角形延迟光照顶点着色器
// SV_VertexID 驱动，无需 VB/IB

struct VSOutput {
    float4 position          : SV_Position;
    [[vk::location(0)]] float2 uv;
};

VSOutput vertexMain(uint vid : SV_VertexID) {
    float2 clipPos = float2(
        float(vid == 2u ? 3 : -1),
        float(vid == 1u ? 3 : -1));

    VSOutput output;
    output.position = float4(clipPos, 0.0, 1.0);
    output.uv       = clipPos * 0.5 + 0.5;
    return output;
}
