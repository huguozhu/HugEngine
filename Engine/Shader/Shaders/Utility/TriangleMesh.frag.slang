// ============================================================
// TriangleMesh.frag.slang — Mesh Shader 三角形配套片元着色器
//
// 接收 Mesh Shader 输出的逐顶点颜色，经光栅化插值后输出
// 颜色为 RGB 三色渐变，与光栅化模式的纯红色三角形区分
// ============================================================

// 从 Mesh Shader 传来的插值颜色
float4 fragmentMain(float3 color : COLOR0) : SV_Target0
{
    return float4(color, 1.0);  // 输出 RGB 三色渐变的三角形
}
