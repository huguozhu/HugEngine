// RT_Common.rmiss.slang — 通用 Miss 着色器
// 用于 Shadow/Reflection/AO 的未命中处理

[shader("miss")]
void main()
{
    // 未命中表示光线未击中任何几何体
    // Shadow:  返回 1.0（无遮挡，完全可见）
    // Reflection: 采样天空盒/IBL
    // AO:      返回 0.0（无遮蔽）
    // 具体行为由 payload 区分（由 RayGen 预设）

    // 默认行为：写入 payload = 1（可见/天空）
    // 具体 payload 语义由 RayGen shader 定义
}
