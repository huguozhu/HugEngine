// RT_Background.rmiss.slang — 占位 Miss 着色器（RT Pipeline 至少需要 1 个 Miss group）
// Phase 1 的过程化三角形不调用 TraceRay，Miss 不会被触发

// D3D12/DXR 专属：miss 入口必须带 payload 参数。SPIR-V 路径不定义 HE_SHADER_D3D12，
// 因此 Vulkan 侧仍然是空参数入口（行为与之前完全一致）。
#if defined(HE_SHADER_D3D12)
struct RTBackgroundPayload { float3 color; };
#endif

[shader("miss")]
void main(
#if defined(HE_SHADER_D3D12)
    // DXR 的 miss 入口必须带 payload 参数（SPIR-V 允许空参数；本着色器从不被调用，仅为满足签名）
    inout RTBackgroundPayload payload
#endif
)
{
    // 空实现 — RayGen 直接输出颜色，不使用 TraceRay
}
