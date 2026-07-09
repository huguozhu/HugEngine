// RT_Background.rmiss.slang — 占位 Miss 着色器（RT Pipeline 至少需要 1 个 Miss group）
// Phase 1 的过程化三角形不调用 TraceRay，Miss 不会被触发

[shader("miss")]
void main()
{
    // 空实现 — RayGen 直接输出颜色，不使用 TraceRay
}
