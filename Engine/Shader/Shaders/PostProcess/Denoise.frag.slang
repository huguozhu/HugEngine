// Denoise.frag.slang — 5×5 双边模糊（深度+法线感知，用于 SSGI/SSR 降噪）
//
// 【有效性协议】输入 alpha < 0 表示该像素**本帧无数据**（SSR miss / 天空；
// 见 GI/SSR.frag.slang 与 RT_Reflection.rgen 的 `float4(0,0,0,-1)`）。
// 双边模糊必须**保住这个符号**，否则合成端的 `a < 0` 判定会失效（§9.2-C）：
//   · 无效样本**不参与**加权平均（否则无效的黑色会污染有效结果）；
//   · 有效性**不参与**平均 —— 邻域内有任一有效贡献即输出 +1，全无则输出 -1。
// 顺带修掉了原先 `totalW>0 ? sum/totalW : center` 的兜底：它会把"本无数据"的
// 像素原样透出，而 `center` 的 alpha 也是 1.0，等于伪造有效性。
//
// 注：SSGI 的输出 alpha 恒为 +1（无负值语义），故本改动对 SSGI 行为等价。
struct VSOutput { float4 p:SV_Position; [[vk::location(0)]] float2 uv; };

[[vk::binding(0,0)]] Texture2D<float4> u_Input;      // SSGI/SSR 输出
[[vk::binding(1,0)]] Texture2D<float>  u_Depth;       // GBuffer 深度
[[vk::binding(2,0)]] Texture2D<float4> u_Normal;      // GBuffer 法线
[[vk::binding(0,0)]] SamplerState u_Sampler;

[[vk::push_constant]] cbuffer Params {
    float2 texelSize;
    float  depthSigma;    // 深度差异敏感度（越小越不跨边）
    float  normalSigma;   // 法线差异敏感度
};

float4 fragmentMain(VSOutput input):SV_Target0{
    float2 uv=input.uv;
    float centerDepth=u_Depth.Sample(u_Sampler,uv).r;
    float3 centerN=u_Normal.Sample(u_Sampler,uv).rgb*2.0-1.0;

    float4 sum=float4(0,0,0,0); float totalW=0;
    for(int y=-2;y<=2;y++){
        for(int x=-2;x<=2;x++){
            float2 suv=uv+float2(x,y)*texelSize;
            float4 s=u_Input.Sample(u_Sampler,suv);
            if(s.a<0.0)continue;                    // 无效样本：不参与（保住符号语义）
            float sd=u_Depth.Sample(u_Sampler,suv).r;
            float3 sn=u_Normal.Sample(u_Sampler,suv).rgb*2.0-1.0;

            float dW=exp(-abs(centerDepth-sd)*depthSigma);
            float nW=pow(max(0,dot(centerN,sn)),normalSigma);
            float w=dW*nW;
            sum+=s*w; totalW+=w;
        }
    }
    // 邻域内全无有效数据 → 保持无效（合成端会跳过本源的这一条）
    if(totalW<=0.0)return float4(0,0,0,-1);
    float4 outc=sum/totalW;
    outc.a=1.0;                                     // 有效性不参与平均：有有效贡献即 +1
    return outc;
}
