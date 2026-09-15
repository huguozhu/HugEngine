// AITextureSample.comp.slang — 纹理→张量 采样核（A3.2a 零拷贝互操作验证）
//
// 用途：验证「渲染纹理 → WrapRHITexture(张量) → 推理 → 输出缓冲」链路。
// 语义：采样输入纹理（线性过滤），乘亮度系数写入输出缓冲。
// 描述符布局：binding 0 = 输入纹理+采样器（CombinedImageSampler），
//             binding 1 = 输出缓冲（RWStructuredBuffer<float4>）。

[[vk::binding(0, 0)]] Texture2D<float4> u_InputTex;
[[vk::binding(0, 0)]] SamplerState      u_InputSampler;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> u_Output;

// 推参（16 字节，含 pad 对齐 vec4）
[[vk::push_constant]] cbuffer SampleParams {
    uint  width;        // 纹理宽（像素）
    uint  height;       // 纹理高（像素）
    float brightness;   // 亮度系数
    float _pad;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    // 像素中心 UV
    float2 uv = (float2(id.xy) + 0.5f) / float2(float(width), float(height));
    float4 c = u_InputTex.SampleLevel(u_InputSampler, uv, 0);
    u_Output[id.y * width + id.x] = c * brightness;
}
