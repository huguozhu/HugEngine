// GBuffer.mesh.slang — Mesh Shader 版 GBuffer 几何输出
// 替代传统 VS+IA 管线：Mesh Shader 直接从 Storage Buffer 读取顶点数据并输出三角形
// 用法: DrawMeshTasks(groupCount, 1, 1) 替代 DrawIndexed

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
// TODO: 设置 max_vertices 和 max_primitives
void main()
{
    // 从 DispatchMesh 的 group ID 计算该工作组处理的几何体
    // uint groupID = DispatchMesh().x;
    //
    // 1. 从 Storage Buffer 读取顶点数据（替代 SetVertexBuffer）
    // 2. 计算世界空间变换
    // 3. 输出顶点 + 图元索引
    //
    // SetMeshOutputsEXT(vertexCount, primitiveCount);
}
