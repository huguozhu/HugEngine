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
// 【量纲：E/π 的估计（SSGI-CAL 已落地，§9.2-P）】此前累加项是
// `sAlbedo × max(0,dot(N,sDir)) × falloff`：用的是命中点的**反照率**而不是入射辐射度
// L_in，余弦项又用了未归一化的 sDir（sDir 长度在 (0,1] 随机，等于把 cosθ 与样本长度
// 混在一起），距离项还写成 1/(1+dot(sDir,sDir)·radius) —— 长度的平方，量纲也不对。
// 结果它不是 E/π 的估计，而是「反照率相关性」启发式量，在归一化合成里只拿到极小的
// 权重（实测 DDGI/SSGI ≈ 21 倍）。
//
// 现在按物理量重写：
//   · L_in   —— 命中点的入射辐射度，取自**上一帧的 HDR**（u_Radiance，帧图注入的
//               GIRadianceHistory；SSGI 排在 Lighting 之前，故取前帧既解决因果又构成
//               多次弹射的时域反馈）；
//   · cosθ   —— `max(0, dot(N, normalize(sDir)))`，只剩余弦项；
//   · 估计量 —— `Σ(L_in·cosθ) / Σcosθ`，即「余弦加权平均的入射辐射度」。因为
//               ∫cosθ dω = π，所以它精确等于 E/π：全白环境（L=1）下读数恒为 1，
//               **与被遮挡丢掉的样本、采样核分布都无关**（权重在分子分母里抵消）。
//               这比「乘一个经验增益再拿参考去标定」更硬：常数项根本不存在。
//   · 距离衰减项已删除：它此前用的是 |sDir|²，量纲不对；正确的距离项需要采样点与
//     几何的**实际**距离，而本方案只在固定半径处取一个样本，没有这个信息。固定半径
//     采样对 L 的估计偏差是本方案的已知近似，不靠一个经验衰减项来掩盖。
struct VSOutput { float4 p:SV_Position; [[vk::location(0)]] float2 uv; };

[[vk::binding(0,0)]] Texture2D<float>  u_Depth;
[[vk::binding(1,0)]] Texture2D<float4> u_Normal;
[[vk::binding(2,0)]] Texture2D<float4> u_Albedo;
[[vk::binding(4,0)]] Texture2D<float4> u_Radiance;   // 前帧 HDR 辐射度（入射辐射度 L_in）
[[vk::binding(0,0)]] SamplerState u_PointSampler;

[[vk::binding(3,0)]] cbuffer Params {
    float4 u_Samples[32];  // 半球采样方向（切线空间，z 沿法线；长度随机 = 采样距离）
    float4 u_Params;       // x=radius, y=intensity, z=sampleCount, w=furnace
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

    // 白炉条件（全白环境 + 接收面 albedo = 1）：本 pass 也必须按该条件求值，
    // 否则白炉判据只能靠 Lighting 侧的短路，SSGI 自己的标度就永远测不出来。
    if(u_Params.w > 0.5) albedo = float3(1.0, 1.0, 1.0);

    // 重建 view-space 位置（clip→view，用逆投影矩阵）
    float4 cp=float4(uv*2.0-1.0,depth,1.0);
    float4 vp4=mul(u_InvProj,cp); float3 viewPos=vp4.xyz/vp4.w;

    // TBN（N 为 view 空间法线；(N.z,0,-N.x) 与 N 点积恒为 0，故可作任意朝向的切线）
    float3 T=normalize(float3(N.z,0,-N.x)+float3(0.001,0.001,0));
    float3 B=cross(N,T); float3x3 TBN=float3x3(T,B,N);

    float radius=u_Params.x; int samples=int(u_Params.z);
    // 余弦加权平均：num/den 同为「余弦权重的入射辐射度」的分子与分母，
    // 故 den 的分母就是 ∫cosθ dω = π → 结果即 E/π（见文件头）。
    float3 num=float3(0,0,0); float den=0.0;
    for(int i=0;i<samples;i++){
        // 切线空间样本 → 本空间方向：须用 mul(向量, 矩阵)，见文件头说明。
        // 样本长度 = 采样距离（(0,1] 随机），方向必须归一化后才用于余弦项。
        float3 sDir=mul(u_Samples[i].xyz,TBN);
        float3 sDirN=normalize(sDir);
        float  cosT=max(0.0,dot(N,sDirN));
        if(cosT<=0.0) continue;                       // 下半球样本不参与（余弦权重为 0）
        float3 sPos=viewPos+sDir*radius;
        float4 off=mul(u_Proj,float4(sPos,1.0));off.xyz/=off.w;
        float2 suv=off.xy*0.5+0.5;
        float sd=u_Depth.Sample(u_PointSampler,suv).r;
        float sZ=(mul(u_InvProj,float4(suv*2.0-1.0,sd,1.0)).z)/(mul(u_InvProj,float4(suv*2.0-1.0,sd,1.0)).w);
        // 可见性：view 空间朝 −Z，越远越负。采样点未被遮挡 ⇔ 该像素保存的几何
        // **不比采样点更近**，即 sZ <= sPos.z（+bias 容忍同一表面）。
        // 原先写作 sZ >= sPos.z-0.01，方向相反，等于只采被遮挡的样本。
        if(sZ<=sPos.z+0.01){
            // 命中点的入射辐射度。真实路径取**前帧 HDR**（唯一真实亮度来源）；
            // 白炉下取解析真值 1（白炉条件本身就是「所有方向入射辐射度 = 1」，用前帧 HDR
            // 反而是循环定义：那个 HDR 正是本 pass 的输出）。注入 1 之后，读数
            // `Σ(L·cos)/Σcos` 只有在余弦归一化正确时才等于 1 —— 旧的 `Σcos/N` 形式会读出
            // 约 0.5 而不是 1，所以白炉此时**确实在校验 SSGI 的标度**，不是同义反复。
            float3 L_in=(u_Params.w>0.5)?float3(1.0,1.0,1.0)
                                        :u_Radiance.SampleLevel(u_PointSampler,suv,0).rgb;
            num+=L_in*cosT;
            den+=cosT;
        }
    }
    // den == 0（所有样本都被遮挡/落在下半球）时返回 0：没有可信估计就不编数据
    float3 indirect=(den>0.0)?(num/den):float3(0.0);
    // 返回「已乘接收面 albedo 的间接出射辐射度」：L_o = albedo × E/π（§3.4 的统一量纲）
    return float4(albedo*indirect*u_Params.y,1.0);
}
