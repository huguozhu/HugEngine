// Denoise.frag.slang — 5×5 双边模糊（深度+法线感知，用于 SSGI/SSR 降噪）
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
    float4 center=u_Input.Sample(u_Sampler,uv);
    float centerDepth=u_Depth.Sample(u_Sampler,uv).r;
    float3 centerN=u_Normal.Sample(u_Sampler,uv).rgb*2.0-1.0;

    float4 sum=float4(0,0,0,0); float totalW=0;
    for(int y=-2;y<=2;y++){
        for(int x=-2;x<=2;x++){
            float2 suv=uv+float2(x,y)*texelSize;
            float4 s=u_Input.Sample(u_Sampler,suv);
            float sd=u_Depth.Sample(u_Sampler,suv).r;
            float3 sn=u_Normal.Sample(u_Sampler,suv).rgb*2.0-1.0;

            float dW=exp(-abs(centerDepth-sd)*depthSigma);
            float nW=pow(max(0,dot(centerN,sn)),normalSigma);
            float w=dW*nW;
            sum+=s*w; totalW+=w;
        }
    }
    return totalW>0?sum/totalW:center;
}
