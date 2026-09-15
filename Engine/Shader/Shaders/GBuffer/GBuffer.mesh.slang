// GBuffer.mesh.slang — Mesh Shader 版 GBuffer 几何输出
// 替代传统 VS+IA 管线：Mesh Shader 直接从 Storage Buffer 读取顶点数据并输出三角形
// 用法: DrawMeshTasks(groupCount, 1, 1) 替代 DrawIndexed
//
// 注意: slangc 2026.13+ 要求 mesh shader 入口声明 OutputVertices + OutputIndices 输出参数

#include "ShaderTypes.slang"

// Mesh Shader 输出：与 Pixel Shader GBuffer 输入匹配
struct MeshOutput
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

// 工作组大小：每个线程处理一个顶点
[numthreads(64, 1, 1)]
[outputtopology("triangle")]
// TODO: 实现完整的 Mesh Shader 路径
//   1. 从 Storage Buffer 读取顶点数据（替代 SetVertexBuffer）
//   2. 计算世界空间变换
//   3. 输出顶点 + 图元索引
//   4. SetMeshOutputsEXT(vertexCount, primitiveCount);
void main(
    out vertices MeshOutput verts[256],
    out indices uint3 tris[512]
) {
    // 桩实现：输出 0 个顶点/图元（暂不使用 Mesh Shader 路径）
    SetMeshOutputCounts(0, 0);
}
