// ============================================================
// Triangle.mesh.slang — Mesh Shader 三角形示例
//
// 替代传统 VS+IA 管线：Mesh Shader 直接输出三角形顶点和图元
// 无 VertexBuffer / IndexBuffer 依赖，硬编码三个顶点位置
// 用法: DrawMeshTasks(1, 1, 1)
//
// 语法说明:
//   - SV_GroupIndex 需作为函数参数传入（Slang 不支持在函数体内直接访问 SV_*）
//   - out vertices / out indices 参数声明输出顶点和图元数组
//   - [outputtopology("triangle")] 声明输出拓扑为三角形
// ============================================================

// 输出顶点结构：与 Pixel Shader 输入匹配
struct VertexOut
{
    float4 position : SV_POSITION;
    float3 color    : COLOR0;
};

// 工作组：3 个线程（每个顶点一个），输出最多 3 个顶点和 1 个三角形
[numthreads(3, 1, 1)]
[outputtopology("triangle")]

void main(
    // 线程索引（0 / 1 / 2）—— 通过参数传入
    uint tid : SV_GroupIndex,
    // 顶点输出数组：最多 3 个顶点
    out vertices VertexOut verts[3],
    // 图元索引数组：最多 1 个三角形
    out indices uint3 prims[1])
{
    // 三个顶点位置（与光栅化模式使用相同的三角形坐标）
    const float2 positions[3] = {
        float2( 0.0, -0.5),   // 顶点 0：顶部
        float2( 0.5,  0.5),   // 顶点 1：右下
        float2(-0.5,  0.5),   // 顶点 2：左下
    };

    // 各顶点颜色（三色渐变：红/绿/蓝，便于在屏幕上区分三个顶点）
    const float3 colors[3] = {
        float3(1.0, 0.2, 0.2),  // 顶点 0：红色
        float3(0.2, 1.0, 0.2),  // 顶点 1：绿色
        float3(0.2, 0.2, 1.0),  // 顶点 2：蓝色
    };

    // 每个线程输出其对应的顶点
    verts[tid].position = float4(positions[tid], 0.0, 1.0);
    verts[tid].color    = colors[tid];

    // 仅线程 0 输出图元索引（只需输出一次）
    if (tid == 0)
    {
        prims[0] = uint3(0, 1, 2);  // 三角形由顶点 0→1→2 组成（顺时针）
    }

    // 设置输出数量（Vulkan 要求所有线程调用且参数一致）
    SetMeshOutputCounts(3, 1);  // 3 个顶点，1 个三角形
}
