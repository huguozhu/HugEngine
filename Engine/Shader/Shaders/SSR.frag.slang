// SSR.frag.slang — 屏幕空间反射（Hi-Z 层次追踪 + 线性 march 回退）
//
// Hi-Z 层次 march：用深度金字塔从粗到细跳过低空区域，
// 相比线性 64 步 march 大幅减少采样次数（大步推进 + 命中细化）。
// Hi-Z 为 min 深度（Reverse-Z：近=1.0 / 远=0.0），
// 射线屏幕深度 > Hi-Z（射线在几何之前）→ 推进到更粗层级；
// 否则回退细化，直到 level 0 判定命中。
//
// u_UseHiZ=0 时回退线性 march（Hi-Z 金字塔不可用时）。
struct VSOutput { float4 p:SV_Position; [[vk::location(0)]] float2 uv; };

[[vk::binding(0,0)]] Texture2D<float>  u_Depth;
[[vk::binding(1,0)]] Texture2D<float4> u_Normal;
[[vk::binding(2,0)]] Texture2D<float4> u_Albedo;
[[vk::binding(4,0)]] Texture2D<float>  u_HiZ;          // Hi-Z 深度金字塔（min 深度，多级 mip）
[[vk::binding(0,0)]] SamplerState u_PointSampler;
[[vk::binding(4,0)]] SamplerState u_HiZSampler;

// 与 GI_SSR::Render 的 SetPushConstants 结构严格对应
[[vk::push_constant]] cbuffer Params {
    float4x4 u_Proj;       // 投影矩阵逆（重建 view-space）
    float4   u_Params;     // x=maxSteps, y=stepSize, z=maxDistance, w=thickness
    float    u_UseHiZ;     // 1=Hi-Z 层次 march，0=线性 march
    float3   _pad;
};

static const int kHiZMaxLevel = 5;   // 最多 5 级（32 像素大步）

float4 fragmentMain(VSOutput input):SV_Target0{
    float2 uv=input.uv;
    float depth=u_Depth.Sample(u_PointSampler,uv).r;
    if(depth>=1.0)return float4(0,0,0,1);
    float3 N=normalize(u_Normal.Sample(u_PointSampler,uv).rgb*2.0-1.0);
    float3 albedo=u_Albedo.Sample(u_PointSampler,uv).rgb;

    // 重建 view-space 位置和视线方向
    float4 cp=float4(uv*2.0-1.0,depth,1.0);
    float4 vp4=mul(u_Proj,cp);
    float3 viewPos=vp4.xyz/vp4.w;
    float3 V=normalize(-viewPos);
    float3 R=reflect(-V,N);

    float maxSteps=u_Params.x, stepSz=u_Params.y, maxDist=u_Params.z, thickness=u_Params.w;

    float3 rayStart=viewPos+R*0.1;   // 起点偏移避免自交
    float2 hitUV=uv;
    float  hit=0.0;

    if(u_UseHiZ>0.5){
        // ---- Hi-Z 层次 march ----
        int   level = 0;
        float t     = 0.0;              // 射线参数 [0,1]
        for(int iter=0; iter<int(maxSteps); ++iter){
            float stepT = 1.0 / float(1 << level);   // 层步长（level 越大步越大）
            float tNext = t + stepT;
            if(tNext > 1.0) break;

            float3 rayPos = rayStart + R*(maxDist*tNext);
            float4 rpClip = mul(u_Proj, float4(rayPos,1.0));
            if(rpClip.w <= 0.0001) break;
            float3 rpNDC = rpClip.xyz / rpClip.w;
            float2 rpUV  = rpNDC.xy*0.5+0.5;
            if(rpUV.x<0.0||rpUV.x>1.0||rpUV.y<0.0||rpUV.y>1.0) break;

            float hiZ = u_HiZ.SampleLevel(u_HiZSampler, rpUV, level).r;

            // Reverse-Z：射线屏幕深度 > Hi-Z → 射线在几何之前 → 大步推进
            if(rpNDC.z > hiZ){
                t = tNext;
                level = min(level + 1, kHiZMaxLevel);
            } else {
                // 射线进入几何后方 → 回退细化
                if(level == 0){
                    float geomDepth = u_Depth.Sample(u_PointSampler, rpUV).r;
                    if(abs(rpNDC.z - geomDepth) < thickness*0.1){
                        hit = 1.0;
                        hitUV = rpUV;
                    }
                    break;
                }
                level--;
            }
        }
    } else {
        // ---- 线性 march（回退）----
        float3 rayPos=rayStart;
        float3 rayStep=R*stepSz;
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
            if(rayPos.z>rpZ){hitUV=rpUV;hit=0.5;break;}
        }
    }

    if(hit>0){
        float3 hitAlbedo=u_Albedo.Sample(u_PointSampler,hitUV).rgb;
        float3 hitNormal=normalize(u_Normal.Sample(u_PointSampler,hitUV).rgb*2.0-1.0);
        float NdotR=abs(dot(hitNormal,R));
        return float4(albedo*hitAlbedo*NdotR*hit,1.0);
    }
    return float4(0,0,0,1);
}
