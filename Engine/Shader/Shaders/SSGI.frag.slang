// SSGI.frag.slang — 屏幕空间全局光照（间接漫反射）
struct VSOutput { float4 p:SV_Position; [[vk::location(0)]] float2 uv; };

[[vk::binding(0,0)]] Texture2D<float>  u_Depth;
[[vk::binding(1,0)]] Texture2D<float4> u_Normal;
[[vk::binding(2,0)]] Texture2D<float4> u_Albedo;
[[vk::binding(0,0)]] SamplerState u_PointSampler;

[[vk::binding(3,0)]] cbuffer Params {
    float4 u_Samples[32];  // 半球采样方向
    float4 u_Params;       // x=radius, y=intensity, z=sampleCount, w=unused
    float4x4 u_Proj;       // 投影矩阵（重建 view-space）
};

float4 fragmentMain(VSOutput input):SV_Target0{
    float2 uv=input.uv; float depth=u_Depth.Sample(u_PointSampler,uv).r;
    if(depth>=1.0)return float4(0,0,0,1);
    float3 N=normalize(u_Normal.Sample(u_PointSampler,uv).rgb*2.0-1.0);
    float3 albedo=u_Albedo.Sample(u_PointSampler,uv).rgb;

    // 重建 view-space 位置
    float4 cp=float4(uv*2.0-1.0,depth,1.0);
    float4 vp4=mul(u_Proj,cp); float3 viewPos=vp4.xyz/vp4.w;

    // TBN
    float3 T=normalize(float3(N.z,0,-N.x)+float3(0.001,0.001,0));
    float3 B=cross(N,T); float3x3 TBN=float3x3(T,B,N);

    float radius=u_Params.x; int samples=int(u_Params.z);
    float3 indirect=float3(0,0,0);
    for(int i=0;i<samples;i++){
        float3 sDir=mul(TBN,u_Samples[i].xyz);
        float3 sPos=viewPos+sDir*radius;
        float4 off=mul(u_Proj,float4(sPos,1.0));off.xyz/=off.w;
        float2 suv=off.xy*0.5+0.5;
        float sd=u_Depth.Sample(u_PointSampler,suv).r;
        float sZ=(mul(u_Proj,float4(suv*2.0-1.0,sd,1.0)).z)/(mul(u_Proj,float4(suv*2.0-1.0,sd,1.0)).w);
        if(sZ>=sPos.z-0.01){  // 采样点可见 → 累积间接光
            float3 sAlbedo=u_Albedo.Sample(u_PointSampler,suv).rgb;
            float falloff=1.0/(1.0+dot(sDir,sDir)*radius);  // 距离衰减
            indirect+=sAlbedo*max(0,dot(N,sDir))*falloff;
        }
    }
    indirect/=float(samples);
    return float4(albedo*indirect*u_Params.y,1.0);
}
