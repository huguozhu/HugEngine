// ============================================================
// Triangle.vert.slang — 三角形示例顶点着色器
// ============================================================

#include "MeshVS.slang"   // PositionOnlyVSOutput（同形状深度类 pass 接口唯一出处）
// explicit semantics (D3D12) + vk::location (Vulkan)
struct TriangleVertexInput {
    [[vk::location(0)]] float2 inPosition : POSITION0;
};

PositionOnlyVSOutput vertexMain(TriangleVertexInput vin) {
    const float2 inPosition = vin.inPosition;
    PositionOnlyVSOutput output;
    output.position = float4(inPosition, 0.0, 1.0);
    return output;
}
