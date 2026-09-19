// Denoise_Upscale.frag.slang — 降噪后的「重建升采样」（半分辨率信号的低分辨率还原）
//
// 【为什么需要它】半分辨率信号此前是**直接采样**交给合成端的：降噪整个被跳过，而半分辨率
// 恰恰最需要降噪（噪声随像素数下降而上升，5×5 的核在半分辨率上才够用）。统一降噪框架
// （§10 的 11.3）把这件事写成信号属性 `needsUpscale`：信号分辨率低于消费端时，
// **先在信号分辨率上降噪，再重建升采样**——本着色器就是后一半。
//
// 【为什么不是普通双线性放大】普通双线性会把低分辨率噪声糊成块，且会跨过深度/法线
// 不连续处（物体边缘出现"光晕"）。这里做的是**引导重建**：空间权重用高斯（σ = 1 个目标
// 纹素，等价于双线性的三角形核的更平滑版本），再乘上全分辨率深度/法线的双边权重，
// 于是重建只在同一表面内插值 —— 这也是深度/法线引导在升采样上的标准用法。
//
// 【有效性协议】必须与 `Denoise.frag.slang` 完全一致（框架级契约，11.3）：
//   · 源 alpha < 0 ⇒ 无效样本，不参与加权平均；
//   · 邻域内全无有效样本 ⇒ 输出 alpha = -1（合成端跳过本源的这一条，且不计入归一化分母）。
// 少了任何一半，半分辨率路径上的"本条无数据"就会在升采样这一步丢失。
struct VSOutput { float4 p:SV_Position; [[vk::location(0)]] float2 uv; };

[[vk::binding(0,0)]] Texture2D<float4> u_Input;      // 信号分辨率的降噪结果
[[vk::binding(1,0)]] Texture2D<float>  u_Depth;       // GBuffer 深度（目标/全分辨率）
[[vk::binding(2,0)]] Texture2D<float4> u_Normal;      // GBuffer 法线（目标/全分辨率）
[[vk::binding(0,0)]] SamplerState u_Sampler;

[[vk::push_constant]] cbuffer Params {
    float2 srcTexelSize;    // 源（信号分辨率）纹素尺寸
    float2 dstTexelSize;    // 目标（全分辨率）纹素尺寸
    float  depthSigma;
    float  normalSigma;
    float2 _pad;
};

float4 fragmentMain(VSOutput input):SV_Target0{
    float2 uv = input.uv;
    float centerDepth = u_Depth.SampleLevel(u_Sampler, uv, 0).r;
    float3 centerN    = u_Normal.SampleLevel(u_Sampler, uv, 0).rgb * 2.0 - 1.0;

    float4 sum = float4(0,0,0,0); float totalW = 0;
    // 源纹素 3×3 邻域：2× 升采样时这覆盖了目标像素周围的全部候选低分辨率样本
    for (int y=-1; y<=1; y++) {
        for (int x=-1; x<=1; x++) {
            float2 suv = uv + float2(x,y) * srcTexelSize;
            float4 s = u_Input.SampleLevel(u_Sampler, suv, 0);
            if (s.a < 0.0) continue;                      // 无效样本：不参与（保住符号语义）
            float  sd = u_Depth.SampleLevel(u_Sampler, suv, 0).r;
            float3 sn = u_Normal.SampleLevel(u_Sampler, suv, 0).rgb * 2.0 - 1.0;

            float  dW = exp(-abs(centerDepth - sd) * depthSigma);
            float  nW = pow(max(0.0, dot(centerN, sn)), normalSigma);
            // 空间权重：以**目标**纹素为单位的高斯（σ=1），只问"这个低分辨率样本离本像素多近"
            float2 d  = (suv - uv) / dstTexelSize;
            float  sW = exp(-0.5 * dot(d, d));
            float  w  = dW * nW * sW;
            sum += s * w; totalW += w;
        }
    }
    if (totalW <= 0.0) return float4(0,0,0,-1);            // 邻域内全无有效数据 ⇒ 保持无效
    float4 outc = sum / totalW;
    outc.a = 1.0;                                          // 有效性不参与平均
    return outc;
}
