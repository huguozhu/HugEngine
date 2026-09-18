// SSR.frag.slang — 屏幕空间反射（Hi-Z 层次追踪 + 线性 march 回退）
//
// Hi-Z 层次 march：用深度金字塔从粗到细跳过低空区域，
// 相比线性 64 步 march 大幅减少采样次数（大步推进 + 命中细化）。
//
// 【深度约定：zero-to-one（近=0 / 远=1），**不是** reverse-Z】（任务 25 / §9.2-W）
// 引擎用 `glm::perspectiveRH_ZO`，近平面映射到 0、远平面映射到 1 —— 本文件自己的天空判据
// `depth >= 1.0` 就是这条约定的直接证据。而且金字塔是 **min** 深度（`HiZDownsample.comp`
// 取 2×2 最小值）= 每格**最近的**几何。两条合起来，判据只有一个方向：
//     射线点的深度 **小于** 该格最小深度 ⇒ 射线比这一格里的一切都近 ⇒ 这一格是空的 ⇒ 前进
// 此前这里写的是 `rpNDC.z > hiZ ⇒ 前进`（注释按 reverse-Z 写的），方向正好相反 ——
// 于是射线"一路前进"直到 t>1 直接退出，**永远走不到命中判定**：实测 `specular={SSR}`
// 时输出逐像素 RGB=0、有效性 alpha 100% 为 −1（§9.2-W）。
//
// 【层级步长必须按屏幕像素算】Hi-Z 的层级是**屏幕空间**的金字塔：level L 的一个 texel 覆盖
// 2^L × 2^L 像素。所以"走一格"必须折算成屏幕段上的 `2^L / 屏幕段长度(像素)`。此前用
// `1 / 2^level` 当步长，方向同样是反的（level 0 一步就跨完整条射线，第二轮 t=1.5 越界退出）。
// 现在先把射线投影成屏幕段 s0→s1，再在其上做 DDA。
//
// u_UseHiZ=0 时回退线性 march（Hi-Z 金字塔不可用时）。
struct VSOutput { float4 p:SV_Position; [[vk::location(0)]] float2 uv; };

[[vk::binding(0,0)]] Texture2D<float>  u_Depth;
[[vk::binding(1,0)]] Texture2D<float4> u_Normal;
[[vk::binding(2,0)]] Texture2D<float4> u_Albedo;
[[vk::binding(4,0)]] Texture2D<float>  u_HiZ;          // Hi-Z 深度金字塔（min 深度，多级 mip）
[[vk::binding(0,0)]] SamplerState u_PointSampler;
[[vk::binding(4,0)]] SamplerState u_HiZSampler;

// ── 矩阵 UBO（binding 3，与 GI_SSR::Render 的 SSRMatrices 逐字段一致）──
//
// 为什么矩阵在 UBO 而不在 push constant：引擎保证的 push constant 范围只有 128B，
// 而两个 mat4 恰好用满。此前只往 push constant 传了**逆**矩阵，可 shader 还拿它当
// **正**投影用 —— 正投影无处安放正是 §9.2-B 的成因。
// 现按用途各给一个矩阵，互不顶替：
[[vk::binding(3,0)]] cbuffer SSRMatrices {
    float4x4 u_InvProj;    // clip → view：重建 view-space 位置
    float4x4 u_Proj;       // view → clip：把射线采样点投影到屏幕
};

// 与 GI_SSR::Render 的 SetPushConstants 结构严格对应（仅标量，32B）
[[vk::push_constant]] cbuffer Params {
    float4   u_Params;     // x=maxSteps, y=stepSize, z=maxDistance, w=thickness
    float    u_UseHiZ;     // 1=Hi-Z 层次 march，0=线性 march
    float3   _pad;
};

static const int kHiZMaxLevel = 5;   // 最多 5 级（32 像素大步）

float4 fragmentMain(VSOutput input):SV_Target0{
    float2 uv=input.uv;
    float depth=u_Depth.Sample(u_PointSampler,uv).r;
    if(depth>=1.0)return float4(0,0,0,-1);   // 天空像素：无反射数据 → 无效（负数 alpha）
    float3 N=normalize(u_Normal.Sample(u_PointSampler,uv).rgb*2.0-1.0);
    float3 albedo=u_Albedo.Sample(u_PointSampler,uv).rgb;

    // 重建 view-space 位置和视线方向（clip → view：用**逆**投影）
    float4 cp=float4(uv*2.0-1.0,depth,1.0);
    float4 vp4=mul(u_InvProj,cp);
    float3 viewPos=vp4.xyz/vp4.w;
    float3 V=normalize(-viewPos);
    float3 R=reflect(-V,N);

    float maxSteps=u_Params.x, stepSz=u_Params.y, maxDist=u_Params.z, thickness=u_Params.w;

    float3 rayStart=viewPos+R*0.1;   // 起点偏移避免自交
    float2 hitUV=uv;
    float  hit=0.0;

    if(u_UseHiZ>0.5){
        // ---- Hi-Z 层次 march（屏幕空间 DDA）----
        // 1) 把射线投影成屏幕段 s0→s1：层级 texel 的大小只有换算到像素才有意义
        float4 c0=mul(u_Proj,float4(rayStart,1.0));
        if(c0.w<=0.0001){
            // 起点在相机后方（不该出现：rayStart 由深度重建）→ 当作 miss
            return float4(0,0,0,-1);
        }
        float2 s0=(c0.xy/c0.w)*0.5+0.5;
        // 终点可能落到相机后方（w<=0，投影无效）→ 反复缩短，最多四次
        float2 s1=s0;
        float  worldLen=0.0;
        for(int k=0;k<4;++k){
            float tEnd=exp2(-float(k));
            float4 c1=mul(u_Proj,float4(rayStart+R*(maxDist*tEnd),1.0));
            if(c1.w>0.0001){ s1=(c1.xy/c1.w)*0.5+0.5; worldLen=maxDist*tEnd; break; }
        }
        if(worldLen<=0.0){
            return float4(0,0,0,-1);   // 整条射线都在相机后方：无屏幕空间信息
        }
        // 2) 屏幕段长度（像素）：用 Hi-Z 第 0 级的尺寸（= 全分辨率深度图尺寸）
        uint hizW=1u, hizH=1u;
        u_HiZ.GetDimensions(hizW,hizH);
        float2 dPix=(s1-s0)*float2(float(hizW),float(hizH));
        float  lenPix=max(abs(dPix.x),abs(dPix.y));
        if(lenPix<1.0){
            return float4(0,0,0,-1);   // 屏幕位移不足一个像素：SSR 无从谈起
        }

        int   level=0;
        float t=0.0;                       // 屏幕段参数 [0,1]
        for(int iter=0;iter<int(maxSteps);++iter){
            float stepPix=float(1<<level); // level L 一格 = 2^L 像素
            t+=stepPix/lenPix;
            if(t>1.0)break;

            float2 rpUV=s0+(s1-s0)*t;
            if(rpUV.x<0.0||rpUV.x>1.0||rpUV.y<0.0||rpUV.y>1.0)break;

            // 射线点自身（世界 = view 空间的 z 由 R 与 worldLen 决定）→ 投影回 clip 取自己的深度
            float3 rayPos=rayStart+R*(worldLen*t);
            float4 rpClip=mul(u_Proj,float4(rayPos,1.0));
            if(rpClip.w<=0.0001)break;
            float  rayZ=rpClip.z/rpClip.w;     // NDC 深度：zero-to-one（近 0 / 远 1）

            float hiZ=u_HiZ.SampleLevel(u_HiZSampler,rpUV,level).r;

            if(rayZ<hiZ){
                // 射线比该层**最近的**几何还近 ⇒ 这一格是空的 ⇒ 升到更粗的层（步子更大）
                level=min(level+1,kHiZMaxLevel);
            }else{
                if(level==0){
                    // 细化到底：命中判据与线性 march **完全一致**（世界空间厚度）。
                    // 此前这里拿 NDC 深度差去比 `thickness*0.1`（世界单位），量纲不对，
                    // 即使方向修对了也会因为阈值没有物理含义而判不出命中。
                    float geomDepth=u_Depth.Sample(u_PointSampler,rpUV).r;
                    float4 gVP=mul(u_InvProj,float4(rpUV*2.0-1.0,geomDepth,1.0));
                    float  gZ=gVP.z/gVP.w;
                    if(abs(rayPos.z-gZ)<thickness){ hit=1.0; hitUV=rpUV; }
                    break;
                }
                level--;   // 被遮挡 → 回退到更细的层（不前进）
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
            // 采样点深度由 clip → view：用**逆**投影（此处此前误用同一矩阵做两个方向）
            float4 rpVP=mul(u_InvProj,float4(rpUV*2.0-1.0,rpDepth,1.0));
            float rpZ=rpVP.z/rpVP.w;

            // 命中：射线点与该处几何的 view 深度差在厚度内（世界空间厚度，量纲一致）
            if(abs(rayPos.z-rpZ)<thickness){hitUV=rpUV;hit=1;break;}
            // 【此处此前把"射线仍在几何之前"当成命中 0.5】view 空间朝 −Z，"更近"= z 更大：
            //   · 射线朝远处走（R.z<0）时，rayPos.z < rpZ 表示它已经**越过**该处的几何；
            //   · 射线朝相机走（R.z>0）时相反。
            // 两种情况都只需**提前结束**（穿过去了，不可能再命中）；绝不能记成命中。
            // 用行进方向点乘即可覆盖两个方向；R.z==0（纯水平）时不早退，交给步数上限。
            if((rayPos.z-rpZ)*R.z>0.0)break;
        }
    }

    if(hit>0){
        float3 hitAlbedo=u_Albedo.Sample(u_PointSampler,hitUV).rgb;
        float3 hitNormal=normalize(u_Normal.Sample(u_PointSampler,hitUV).rgb*2.0-1.0);
        float NdotR=abs(dot(hitNormal,R));
        // 有效：alpha = +1（合成端据此把本源的权重计入归一化）
        return float4(albedo*hitAlbedo*NdotR*hit,1.0);
    }
    // 无效：alpha = **-1**。合成端以 `u_SSR.Sample(...).a < 0` 判定"该源本条无效"并
    // 跳过它（§9.2-C）。此前这里也返回 1.0，导致判定永不成立 —— SSR miss 的黑色
    // 会以全权重进入归一化，把环境反射压暗约一半。
    // 该协议与 RT 反射一致（见 RT_Reflection.rgen 的 `float4(0,0,0,-1)`）。
    return float4(0,0,0,-1);
}
