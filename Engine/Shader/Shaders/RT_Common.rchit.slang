// RT_Common.rchit.slang — 通用 ClosestHit 着色器
// 用于 Shadow/Reflection/AO 的命中处理

[shader("closesthit")]
void main()
{
    // 命中几何体后的处理：
    // Shadow:  返回 0.0（被遮挡）
    // Reflection: 采样命中点材质 → 递归或终止 → 返回颜色
    // AO:      返回 1.0（被遮蔽）

    // 默认行为：写入 payload = 0（被遮挡/遮蔽）
    // 具体 payload 语义由 RayGen shader 定义
}
