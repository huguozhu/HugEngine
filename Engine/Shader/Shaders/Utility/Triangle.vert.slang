// ============================================================
// Triangle.vert.slang — 三角形示例顶点着色器
// ============================================================

#include "MeshVS.slang"   // PositionOnlyVSOutput（同形状深度类 pass 接口唯一出处）
layout(location = 0) in float2 inPosition;

PositionOnlyVSOutput vertexMain() {
    PositionOnlyVSOutput output;
    output.position = float4(inPosition, 0.0, 1.0);
    return output;
}
