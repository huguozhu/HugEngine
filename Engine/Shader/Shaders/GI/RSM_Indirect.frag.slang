// RSM_Indirect.frag.slang — RSM 间接光（16 点 Poisson 盘 VPL 求和）的**半分辨率**求值
//
// 【为什么要把这段从 DeferredLighting.frag.slang 搬出来】它此前是逐**全分辨率像素**跑的：
// 16 个 VPL × 2 张 RSM 贴图 = 32 次纹理采样，还要做两次 normalize 与一次除法。
// 实测（HE_PASS_TIMING=1 / 06.GILab / Sponza / 本机）：Lighting 在 diffuse 层栈含 RSM 时
// **0.882 ms**，不含时 **0.433 ms** —— 这一项独占约 **0.45 ms**，是全部 GI 里最大的一笔
// （比 SSGI 的 0.436 ms 还大）。搬到半分辨率后这项成本降到 1/4 量级。
//
// 【为什么可以半分辨率】这一项的空间变化来自接收点位置与法线，而 16 个 VPL 分布在光源
// 空间的一个 Poisson 盘上、`1/d²` 与两个余弦项在场景尺度上都变得很慢（最近与最远的 VPL
// 距离比远小于像素尺度），所以它本身是低频量，与 SSGI 同类。判定不靠这句话，靠 A/B：
// 搬走前后的单源做差 `S_rsm` 必须一致到噪声量级（见文档 §10.2 任务 16）。
//
// 【与 Lighting 的分工】本 pass 输出**辐照度 E**（`RSM_VPL_ENERGY` 里已经含 1/π 的经验
// 归一），Lighting 侧只再乘接收面 albedo，与搬走之前逐项一致。
//
// 【天空像素写 0】这是与 SSGI 相同的约定：天空像素没有接收面，写 0 之后双线性升采样会在
// 物体轮廓处混入一点 0（一圈很细的偏暗）。若改成"不写"，插值到的就是上一步的垃圾值。
// 这个代价与收益一起由 A/B 读数判定（轮廓一圈的占比是可见的）。
struct VSOutput { float4 p:SV_Position; [[vk::location(0)]] float2 uv; };

[[vk::binding(0,0)]] Texture2D<float>   u_Depth;          // GBuffer 深度
[[vk::binding(1,0)]] Texture2D<float4>  u_WorldPos;       // GBuffer MRT4：世界坐标
[[vk::binding(2,0)]] Texture2D<float4>  u_Normal;         // GBuffer MRT1：世界空间法线
[[vk::binding(3,0)]] Texture2D<float4>  u_RSMPositionMap; // RSM：世界位置
[[vk::binding(4,0)]] Texture2D<float4>  u_RSMFluxMap;     // RSM：法线 + 通量（a = flux 权重）
[[vk::binding(0,0)]] SamplerState       u_PointSampler;   // GBuffer 精确读取
[[vk::binding(1,0)]] SamplerState       u_IBLSampler;     // 线性 clamp（沿用 Lighting 侧对该贴图的既有采样器名）
[[vk::binding(5,0)]] cbuffer Params {
    float4x4 u_LightViewProj;   // 方向光的 RSM 光源 VP（与 RSM pass 同源，帧图传入）
    float4   u_Params;          // x=lightCount, y=shadowParams.w(光源类型), z=shadowParams.z(阴影强度)
};

// VPL 采样点数与能量归一（M4.4）：
//   原先用 5×5 规则网格（25 点）——规则网格会在斜面上产生条纹状伪影；
//   改为 16 点 Poisson 盘（径向分布更均匀），采样数省 36%，伪影更少。
//   离散化能量归一常数与 VPL 数量成反比：E ≈ (1/N)·Σ VPL_i，
//   故系数由 25 点的 0.03 调整为 16 点的 0.03 × 25/16 ≈ 0.0469（保持总能量不变）。
#define RSM_VPL_COUNT   16
#define RSM_VPL_ENERGY  0.046875   // 0.03 × 25/16：按 VPL 数量反比归一

// 16 点 Poisson 盘（单位半径内近似均匀分布）
static const float2 kRSMPoisson16[RSM_VPL_COUNT] = {
    float2(-0.94201624, -0.39906216), float2( 0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2( 0.34495938,  0.29387760),
    float2(-0.91588581,  0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543,  0.27676845), float2( 0.97484398,  0.75648379),
    float2( 0.44323325, -0.97511554), float2( 0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2( 0.79197514,  0.19090188),
    float2(-0.24188840,  0.99706507), float2(-0.81409955,  0.91437590),
    float2( 0.19984126,  0.78641367), float2( 0.14383161, -0.14100790),
};

float4 fragmentMain(VSOutput input):SV_Target0{
    // 天空像素：没有接收面，直接写 0（见文件头说明）
    if (u_Depth.Sample(u_PointSampler, input.uv).r >= 1.0) return float4(0, 0, 0, 1);

    uint  lightCount     = uint(u_Params.x + 0.5);
    float shadowType     = u_Params.y;   // shadowParams.w：0=方向光 / 1=点光 / 2=聚光
    float shadowStrength = u_Params.z;   // shadowParams.z：<=0 表示该光源不投影

    // 仅当存在方向光阴影时才求值（与搬走之前逐条一致）：
    // shadowType >= 0.5 表示不是方向光；shadowStrength <= 0 表示没有阴影数据
    // ——此时 u_ShadowData[0] 全零，按方向光编码恰好匹配，会注入 NaN。
    if (lightCount == 0u) return float4(0, 0, 0, 1);
    if (shadowType >= 0.5f) return float4(0, 0, 0, 1);
    if (shadowStrength <= 0.0f) return float4(0, 0, 0, 1);

    // GBuffer 按本 pass 的 uv（半分辨率像素中心）**点采样**：几何量大尺度上一致，
    // 与 SSGI 的 halfRes 路径同一约定。
    float3 worldPos = u_WorldPos.Sample(u_PointSampler, input.uv).xyz;
    float3 N = normalize(u_Normal.Sample(u_PointSampler, input.uv).rgb * 2.0 - 1.0);

    float4 lsp = mul(u_LightViewProj, float4(worldPos, 1.0));
    float2 rsmUV = saturate(lsp.xy / lsp.w * 0.5 + 0.5);
    float3 rsmIndirect = float3(0.0);
    for (int i = 0; i < RSM_VPL_COUNT; i++) {
        float2 suv = saturate(rsmUV + kRSMPoisson16[i] * 0.0125);   // 半径按 Poisson 盘归一后放大
        float4 ps = u_RSMPositionMap.Sample(u_IBLSampler, suv);
        float4 fs = u_RSMFluxMap.Sample(u_IBLSampler, suv);
        float3 sN  = normalize(fs.rgb * 2.0 - 1.0);
        float3 dir = ps.xyz - worldPos;
        float  d2  = max(dot(dir, dir), 0.01);
        rsmIndirect += fs.a * max(dot(N, normalize(dir)), 0.0)
                     * max(dot(sN, -normalize(dir)), 0.0) / d2;
    }
    // VPL 离散化能量归一（E ≈ (1/N)·Σ VPL），返回量纲为**辐照度 E**
    // （1/π 已并入该经验常数）→ 调用方（Lighting）再乘接收面 albedo。
    return float4(rsmIndirect * RSM_VPL_ENERGY, 1.0);
}
