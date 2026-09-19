// Lumen_Irradiance.frag.slang — 把探针辐照度纹理贴到 Lumen 的 Provider 输出上（步骤 24）
//
// 【为什么还要一个全屏 pass】帧图的 `BeginOffscreenPass` 用当前管线推导 RenderPass，
// 输出纹理只能作为**颜色附件**写入（不能直接拿一张 compute 写好的纹理当输出）。
// 所以步骤 24 的分工是：compute pass 算逐像素辐照度 → 这个全屏 pass 把它采样并写出。
// 这样 Provider 的输出仍然是"本帧真正算出来的内容"，Lighting 侧完全不用改。
//
// 【白炉怎么办】白炉验收要求 Lumen 输出恒为 1.0（读"源自身的标度"）。白炉下 Provider 直接走
// 常量骨架 pass（`u_ValueAlpha = (1,1)`），不经过这里 —— 这样白炉读数与步骤 24 的内容解耦，
// 不会因为辐照度算错就悄悄改变白炉结果。
struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_Irradiance;   // 屏幕尺寸 RGBA16F（w = 有效）
[[vk::binding(0, 0)]] SamplerState      u_IrradianceSampler;

[[vk::push_constant]] cbuffer CopyPC {
    float4 flags;   // x = 增益（验收时可放大观察）, y = uv 缩放, zw 保留
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    const float4 c = u_Irradiance.SampleLevel(u_IrradianceSampler, input.uv * flags.y, 0);
    // alpha = -1 表示"本条无数据"（沿用框架既有约定，合成端会 skip）——
    // 没有探针覆盖的像素返回无效，而不是"有效的零"，否则会平白稀释同通道其它源。
    if (c.w < 0.5) return float4(0.0, 0.0, 0.0, -1.0);
    return float4(c.rgb * flags.x, 1.0);
}
