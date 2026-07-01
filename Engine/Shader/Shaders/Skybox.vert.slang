// ============================================================
// Skybox.vert.slang — 天空盒全屏三角形
//
// 无需顶点缓冲，通过 SV_VertexID 生成全屏三角形。
// 用逆 ViewProj（无平移）将 NDC 反算为世界空间方向向量。
// ============================================================

// Push Constants: invViewProjMatrix（投影 × 视图旋转的逆矩阵）
[[vk::push_constant]] cbuffer SkyboxPC {
    float4x4 invViewProjMatrix;  // (Projection × ViewRotation)^-1
    float    intensity;          // 亮度
    float3   _pad;
};

struct VSOutput {
    float4 position      : SV_Position;
    [[vk::location(0)]] float3 worldDir;  // 世界空间采样方向
};

VSOutput vertexMain(uint vid : SV_VertexID) {
    // 全屏三角形（3 顶点覆盖整个屏幕）
    float2 clipPos[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0),
    };
    float2 pos = clipPos[vid];

    VSOutput output;
    // NDC 输出：xy=屏幕位置, z=1.0=远平面, w=1.0
    // 深度测试 EQUAL 配合深度缓冲清空值 1.0，仅在无几何体像素绘制
    output.position = float4(pos.x, pos.y, 1.0, 1.0);

    // NDC → 世界空间方向（逆 ViewProj 反算）
    float4 worldPos = mul(invViewProjMatrix, float4(pos.x, pos.y, 1.0, 1.0));
    output.worldDir = worldPos.xyz / worldPos.w;
    return output;
}
