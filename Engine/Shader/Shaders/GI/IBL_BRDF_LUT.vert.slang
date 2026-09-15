// ============================================================
// IBL_BRDF_LUT.vert.slang — BRDF 积分查找表顶点着色器
//
// 全屏三角形，输出 UV 坐标（Vulkan NDC → [0,1] UV）。
// ============================================================

struct VSOutput {
    float4 position          : SV_Position;
    [[vk::location(0)]] float2 uv;
};

VSOutput vertexMain(uint vid : SV_VertexID) {
    float2 clipPos[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0),
    };
    float2 pos = clipPos[vid];

    VSOutput output;
    output.position = float4(pos.x, pos.y, 0.0, 1.0);
    // NDC [-1,1] → UV [0,1]
    output.uv = float2(pos.x * 0.5 + 0.5, pos.y * 0.5 + 0.5);
    return output;
}
