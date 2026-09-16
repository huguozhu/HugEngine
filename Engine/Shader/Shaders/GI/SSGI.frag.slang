// SSGI.frag.slang — 屏幕空间全局光照（间接漫反射）
//
// 【空间一致性】本 pass 全程在 **view 空间** 计算：viewPos 由深度经逆投影重建，
// 采样点 sPos = viewPos + sDir*radius 也在 view 空间，因此 sDir 必须同样是 view 空间
// 方向。GBuffer 存的是**世界空间**法线，故先用 u_View 变换到 view 空间。
//
// 【已知缺陷 ③ 的方向约定】切线→本空间的变换必须用 mul(向量, 矩阵)：HLSL 的
// float3x3(T,B,N) 把 T/B/N 当**行**，故 mul(v,TBN) = v.x*T+v.y*B+v.z*N。
// 原先写作 mul(TBN,v)，算的是 (T·v, B·v, N·v)，得到的方向几乎与法线垂直，
// 半球采样实际失效（实测 dot(N,sDir) 均值仅 0.012，正确应在 0.5 量级）。
//
// 【仍待 SSGI-CAL 处理】累加项用的是命中点的**反照率** sAlbedo，而非入射辐射度
// L_in，且余弦项用了未归一化的 sDir（把 cosθ 与样本长度混在一起）。因此本 pass
// 目前仍不是 E/π 的估计，而是「反照率相关性」启发式量，标度需连同入射辐射度项
// 一并补上（见 docs 的 §10.1 SSGI-CAL）。
struct VSOutput { float4 p:SV_Position; [[vk::location(0)]] float2 uv; };

[[vk::binding(0,0)]] Texture2D<float>  u_Depth;
[[vk::binding(1,0)]] Texture2D<float4> u_Normal;
[[vk::binding(2,0)]] Texture2D<float4> u_Albedo;
[[vk::binding(0,0)]] SamplerState u_PointSampler;

[[vk::binding(3,0)]] cbuffer Params {
    float4 u_Samples[32];  // 半球采样方向（切线空间，z 沿法线）
    float4 u_Params;       // x=radius, y=intensity, z=sampleCount, w=unused
    float4x4 u_InvProj;    // 逆投影矩阵（clip→view，重建 view-space 位置）
    float4x4 u_Proj;       // 正投影矩阵（view→clip，采样点投影到屏幕）
    float4x4 u_View;       // 视图矩阵（world→view，把世界空间法线转到 view 空间）
};

float4 fragmentMain(VSOutput input):SV_Target0{
    float2 uv=input.uv; float depth=u_Depth.Sample(u_PointSampler,uv).r;
    if(depth>=1.0)return float4(0,0,0,1);
    // GBuffer 法线是世界空间 → 转到 view 空间，与 viewPos/sPos 同空间
    float3 Nw=normalize(u_Normal.Sample(u_PointSampler,uv).rgb*2.0-1.0);
    float3 N=normalize(mul((float3x3)u_View,Nw));
    float3 albedo=u_Albedo.Sample(u_PointSampler,uv).rgb;

    // 重建 view-space 位置（clip→view，用逆投影矩阵）
    float4 cp=float4(uv*2.0-1.0,depth,1.0);
    float4 vp4=mul(u_InvProj,cp); float3 viewPos=vp4.xyz/vp4.w;

    // TBN（N 为 view 空间法线；(N.z,0,-N.x) 与 N 点积恒为 0，故可作任意朝向的切线）
    float3 T=normalize(float3(N.z,0,-N.x)+float3(0.001,0.001,0));
    float3 B=cross(N,T); float3x3 TBN=float3x3(T,B,N);

    float radius=u_Params.x; int samples=int(u_Params.z);
    float3 indirect=float3(0,0,0);
    for(int i=0;i<samples;i++){
        // 切线空间样本 → 本空间方向：须用 mul(向量, 矩阵)，见文件头说明
        float3 sDir=mul(u_Samples[i].xyz,TBN);
        float3 sPos=viewPos+sDir*radius;
        float4 off=mul(u_Proj,float4(sPos,1.0));off.xyz/=off.w;
        float2 suv=off.xy*0.5+0.5;
        float sd=u_Depth.Sample(u_PointSampler,suv).r;
        float sZ=(mul(u_InvProj,float4(suv*2.0-1.0,sd,1.0)).z)/(mul(u_InvProj,float4(suv*2.0-1.0,sd,1.0)).w);
        // 可见性：view 空间朝 −Z，越远越负。采样点未被遮挡 ⇔ 该像素保存的几何
        // **不比采样点更近**，即 sZ <= sPos.z（+bias 容忍同一表面）。
        // 原先写作 sZ >= sPos.z-0.01，方向相反，等于只采被遮挡的样本。
        if(sZ<=sPos.z+0.01){
            float3 sAlbedo=u_Albedo.Sample(u_PointSampler,suv).rgb;
            float falloff=1.0/(1.0+dot(sDir,sDir)*radius);  // 距离衰减
            indirect+=sAlbedo*max(0,dot(N,sDir))*falloff;
        }
    }
    indirect/=float(samples);
    return float4(albedo*indirect*u_Params.y,1.0);
}
