// ============================================================
// Triangle.vert.slang — 三角形示例顶点着色器
// ============================================================

#include "MeshVS.slang"   // PositionOnlyVSOutput（同形状深度类 pass 接口唯一出处）
// explicit semantics (D3D12) + vk::location (Vulkan)
struct TriangleVertexInput {
    [[vk::location(0)]] float2 inPosition : POSITION0;
};

// 推送常量：由样例每帧写入（与 01.Triangle.cpp 的 RotationPushConstant 逐字段一致：1 个 float）
struct TrianglePushConstant {
    float angle;   // 绕 NDC 原点的旋转角（弧度）
};

[[vk::push_constant]] TrianglePushConstant g_PC;

PositionOnlyVSOutput vertexMain(TriangleVertexInput vin) {
    const float2 inPosition = vin.inPosition;
    // 【旋转】三角形是纯 2D 图形，直接在裁剪空间绕原点旋转即可：
    // 不需要视图/投影矩阵，也与后端的 Y 约定无关（旋转与 Y 翻转可交换）。
    const float c = cos(g_PC.angle);
    const float s = sin(g_PC.angle);
    const float2 p = float2(inPosition.x * c - inPosition.y * s,
                            inPosition.x * s + inPosition.y * c);
    PositionOnlyVSOutput output;
    output.position = float4(p, 0.0, 1.0);
    return output;
}
