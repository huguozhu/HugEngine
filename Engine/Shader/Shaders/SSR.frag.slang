// SSR.frag.slang — 屏幕空间反射（深度缓冲 Ray Marching）
struct VSOutput { float4 p:SV_Position; [[vk::location(0)]] float2 uv; };

[[vk::binding(0,0)]] Texture2D<float>  u_Depth;
[[vk::binding(1,0)]] Texture2D<float4> u_Normal;
[[vk::binding(2,0)]] Texture2D<float4> u_Albedo;
[[vk::binding(0,0)]] SamplerState u_PointSampler;

[[vk::binding(3,0)]] cbuffer Params {
    float4x4 u_Proj;       // 投影矩阵逆（重建 view-space）
    float4   u_Params;     // x=maxSteps, y=stepSize, z=maxDistance, w=thickness
};

float4 fragmentMain(VSOutput input):SV_Target0{
    float2 uv=input.uv; float depth=u_Depth.Sample(u_PointSampler,uv).r;
    if(depth>=1.0)return float4(0,0,0,1);
    float3 N=normalize(u_Normal.Sample(u_PointSampler,uv).rgb*2.0-1.0);
    float3 albedo=u_Albedo.Sample(u_PointSampler,uv).rgb;

    // 重建 view-space 位置和视线方向
    float4 cp=float4(uv*2.0-1.0,depth,1.0);
    float4 vp4=mul(u_Proj,cp); float3 viewPos=vp4.xyz/vp4.w;
    float3 V=normalize(-viewPos);
    float3 R=reflect(-V,N);

    float maxSteps=u_Params.x, stepSz=u_Params.y, maxDist=u_Params.z, thickness=u_Params.w;

    // Ray march 起点
    float3 rayPos=viewPos+R*0.1; // 起点偏移避免自交
    float3 rayStep=R*stepSz;
    float2 hitUV=uv; float hit=0;

    for(float i=0;i<maxSteps;i+=1.0){
        rayPos+=rayStep;
        if(length(rayPos-viewPos)>maxDist)break;

        float4 rpClip=mul(u_Proj,float4(rayPos,1.0));
        float3 rpNDC=rpClip.xyz/rpClip.w;
        float2 rpUV=rpNDC.xy*0.5+0.5;
        if(rpUV.x<0||rpUV.x>1||rpUV.y<0||rpUV.y>1)break;

        float rpDepth=u_Depth.Sample(u_PointSampler,rpUV).r;
        float4 rpVP=mul(u_Proj,float4(rpUV*2.0-1.0,rpDepth,1.0));
        float rpZ=rpVP.z/rpVP.w;

        if(rpZ>rayPos.z-thickness&&rpZ<rayPos.z+thickness){hitUV=rpUV;hit=1;break;}
        // 穿透检测
        if(rayPos.z>rpZ){hitUV=rpUV;hit=0.5;break;}
    }

    if(hit>0){
        float3 hitAlbedo=u_Albedo.Sample(u_PointSampler,hitUV).rgb;
        float3 hitNormal=normalize(u_Normal.Sample(u_PointSampler,hitUV).rgb*2.0-1.0);
        float NdotR=abs(dot(hitNormal,R));
        return float4(albedo*hitAlbedo*NdotR*hit,1.0);
    }
    return float4(0,0,0,1);
}
