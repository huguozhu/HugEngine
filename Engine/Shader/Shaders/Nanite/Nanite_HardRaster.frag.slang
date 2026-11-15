// ============================================================
// Nanite_HardRaster.frag.slang — §14.8 任务 22：硬光栅的片元（写与软光栅**逐字段一致**的 GBuffer）
//
// 【本文件的唯一职责】把 mesh shader 光栅化出来的片元写成与软光栅（任务 18/19）**完全相同**的
//   GBuffer 契约。任务 22 的验收是"混合光栅画面一致"，所以这里每一个字段都必须与
//   `Nanite_SoftRaster.comp.slang` 的第 ④ 段逐句对应：
//     · MRT0 albedo      （rgb = 反照率，a = metallic）
//     · MRT1 normal      （xyz = **世界**法线（已归一化），a = roughness）
//     · MRT4 worldPos    （xyz = 世界坐标，a = 1.0）
//     · MRT7 lightmapKey （uv.xy, z = `kNaniteObjectIndexBegin + 实例下标`, w = 0）
//   材质求值**复用同一个函数** `softRasterEvaluateMaterial`（在 `Nanite_SoftRasterCommon.slang`
//   里），不另写一份公式 —— 否则"同一批几何、两条路径"的 albedo/metallic 会出现系统性偏差，
//   验收里的 metallic 直方图相关性与 worldpos 相关性都会掉。
//
// 【为什么只写 4 张、而 PSO 却挂 8 个附件】GBuffer 是 8 张 MRT，本模块只负责其中 4 张；
//   PSO 必须按**实际帧缓冲**（8 个附件）声明，故多出来的 4 张（emissive / velocity /
//   disneyA / disneyB）用 per-MRT `writeMask = None` 关掉写入（与 `DecalPass` 的
//   "8 附件 Load + 只写 MRT0/1"完全同一手法）。颜色附件用 `LoadOp::Load`
//   ⇒ 未被硬光栅覆盖的像素原样保留软光栅写下的值。
//
// 【深度不做任何事】深度由硬件从 `SV_POSITION` 得到（PSO 上是 `LessEqual` + `depthWrite`），
//   不写 `SV_Depth`：任务 22 选择的次序（硬光栅排在软光栅**之后**）正是靠硬件深度测试
//   拿到两个方向的正确遮挡 —— 更近的硬片元通过并覆盖软颜色，更远的被丢弃。
//
// 【§14.3 依赖禁令】只依赖 `NaniteTypes.slang` 与既有 GBuffer 契约。
// ============================================================

#include "Nanite_SoftRasterCommon.slang"

// objectIndex 分区：Nanite 段起点（与 C++ 的 `kNaniteObjectIndexBegin`、以及
// `Nanite_SoftRaster.comp.slang` 的同名常量一致 —— 三处必须同为 1024，MRT7 的页号是
// "哪些像素由模块写入"的唯一标记，离线工具 `Tools/gi/lightmap_key_check.py` 与
// `build/verify/nanite_takeover_cmp.py` 都按 `[1024, 2048)` 这个段识别模块写的像素）。
static const uint kNaniteObjectIndexBegin = 1024u;

// 硬光栅读数槽位（与 mesh 侧同名常量、以及与 C++ 的 `kNaniteHardStat*` 一一对应）
static const uint kHardStatPixels         = 2u;
static const uint kHardStatFallbackPixels = 3u;   // 材质段越界/缺失而退化为中性常数的像素数（正常 0）

// ── 输入：必须与 `Nanite_HardRaster.mesh.slang` 的 `HardRasterVertexOut` **逐字段逐语义一致**
//   （mesh → fragment 的链接靠语义名，两个结构必须一一对应；`nointerpolation` 也要一致，
//     否则整形会拿到插值出来的垃圾值）。 ──
struct HardRasterFragIn {
    float4 position     : SV_POSITION;
    float3 worldPos     : TEXCOORD0;
    float3 normal       : TEXCOORD1;
    float2 uv           : TEXCOORD2;
    nointerpolation uint materialID     : TEXCOORD3;
    nointerpolation uint instanceIndex  : TEXCOORD4;
    nointerpolation uint clusterIndex   : TEXCOORD5;   // 【T8 排查】全局簇号
};

// ── 输出：只声明本模块真正写的 4 个 location（0 / 1 / 4 / 7）──
// 【为什么可以跳号】Vulkan 允许片元着色器只写部分 location；PSO 上那 4 张没被声明的附件
//   writeMask = None ⇒ 它们**根本不会被写**（不是"写入未定义值"）。若声明成连续的
//   SV_Target0..3，就会把 normal 写进 emissive、把 worldPos 写进 velocity —— 那会直接
//   打红判据 8b（"差异范围只允许 4 个 GBuffer 目标"）。
struct HardRasterFragOut {
    [[vk::location(0)]] float4 albedoMetallic  : SV_Target0;   // MRT0
    [[vk::location(1)]] float4 normalRoughness : SV_Target1;   // MRT1
    [[vk::location(4)]] float4 worldPosition   : SV_Target4;   // MRT4
    [[vk::location(2)]] float4 emissiveAO      : SV_Target2;   // MRT2（Nanite 无自发光 ⇒ 写 0；调试档用它做覆盖视图）
    [[vk::location(7)]] float4 lightmapKey     : SV_Target7;   // MRT7
};

[shader("fragment")]
HardRasterFragOut fragmentMain(HardRasterFragIn input) {
    HardRasterFragOut output;

    // ── ① 材质：**复用**软光栅的同一个函数（同一份资产材质段 + 同一批 bindless 纹理）──
    // 【为什么 uv 直接可用】mesh 侧把原始 clip.w 交给了硬件 ⇒ 硬件做的透视校正插值与软光栅的
    //   `softRasterAttribute` 是同一个权重公式；UV 因此与软光栅逐值可比。
    // 【为什么材质求值函数里的采样是 `SampleLevel(..., 0)`】compute 入口没有 quad 语义，
    //   任务 19 为软光栅显式选了 mip0；本文件复用它 ⇒ 两条路径取**同一层**贴图，
    //   不会出现"硬光栅有 mip 过渡、软光栅没有"的画面差。
    float3 matAlbedo;
    float  matMetallic;
    float  matRoughness;
    const bool materialFromAsset =
        softRasterEvaluateMaterial(input.materialID, input.uv,
                                   matAlbedo, matMetallic, matRoughness);

    // ── ② 法线：硬件插值后归一化（与软光栅 `normalize(softRasterAttribute(...))` 同一口径）──
    float3 normal = normalize(input.normal);

    // ── 【T8 排查】debugView == 1：品红平涂 ──
    //   把 albedo 强制成纯黑（过光照后仍接近黑），画面里**任何明亮的区域**就是"没有几何"的孔洞
    //   （孔洞）。这比看白模截图可靠：材质/光照/纹理的差异都不会再干扰判断。
    if (debugView == 1u) {
        matAlbedo  = float3(0.0, 0.0, 0.0);   // 纯黑：孔洞会露出明亮天空，最易辨认
        matMetallic = 0.0;
        matRoughness = 1.0;
    }
    // debugView == 2：纯黄 albedo + 世界坐标清零 —— 用来验证"本片元着色器确实在跑"：
    //   世界坐标驱动 SSGI/DDGI，若画面明显变化，说明开关已生效。
    if (debugView == 2u) {
        matAlbedo  = float3(1.0, 1.0, 0.0);
        matMetallic = 0.0;
        matRoughness = 1.0;
    }
    // debugView == 6：只把 **0 号簇** 涂成红色、其余涂黑 —— 用来把"这一簇到底画在屏幕哪里"
    //   与 CPU 侧同一个簇中心的投影结果对照（判定位移/缩放/镜像）。
    if (debugView == 6u) {
        matAlbedo   = (input.clusterIndex == debugCluster) ? float3(1.0, 0.0, 0.0)
                                                              : float3(0.0, 0.0, 0.0);
        matMetallic = 0.0;
        matRoughness = 1.0;
    }

    // debugView == 5：**直接把数组纹理采样结果当 albedo**（不做任何材质/光照调制），
    //   用来判定"采样到底有没有走、采到的是不是白色"。
    if (debugView == 5u) {
        uint layer = u_Materials[min(input.materialID, materialCount - 1u)].bindlessTextureBase;
        matAlbedo   = u_MatArray.SampleLevel(u_MatArraySamp, float3(input.uv, (float)layer), 0.0).rgb;
        matMetallic = 0.0;
        matRoughness = 1.0;
    }

    // debugView == 3：按簇号着色（相邻簇颜色差别明显）——用来判断"孔洞区域是没有任何簇，
    //   还是有簇但三角形/索引没覆盖到"。
    if (debugView == 3u) {
        uint c = input.clusterIndex;
        float r = float((c * 37u) & 255u) / 255.0;
        float g = float((c * 91u) & 255u) / 255.0;
        float b = float((c * 151u) & 255u) / 255.0;
        // 【压暗】调试色必须远低于 1，否则被光照/色调映射推到白，色相全丢
        matAlbedo   = float3(0.02 + 0.30 * r, 0.02 + 0.30 * g, 0.02 + 0.30 * b);
        matMetallic = 0.0;
        matRoughness = 1.0;
    }

    // ── ③ 写 4 张 GBuffer（字段与软光栅逐字段一致）──
    // 【T8 排查】调试色统一写在最终赋值处（分支写法会被后续代码覆盖，实测无效）：
    //   2 = 纯黄（几何覆盖）；6 = 指定簇红、其余黑（与 CPU 投影对照）；0 = 正常。
    float4 dbgAlbedo = float4(matAlbedo, matMetallic);
    if (albedoDebug == 2u) {
        dbgAlbedo = float4(1.0, 1.0, 0.0, 0.0);
    } else if (albedoDebug == 6u) {
        dbgAlbedo = (input.clusterIndex == debugCluster) ? float4(1.0, 0.0, 0.0, 0.0)
                                                         : float4(0.0, 0.0, 0.0, 0.0);
        // 把镜面也压掉：黑 albedo 仍会被环境镜面反射照白（实测画面仍是白色），
        // 这里强制粗糙度 1、法线朝上，让"黑"真的黑下来，红色才看得清。
        matRoughness = 1.0;
        normal = float3(0.0, 0.0, 1.0);
    }
    // MRT2：Nanite 几何没有自发光 ⇒ 常规档写 0（避免残留别的 pass 写入的值）；
    // albedoDebug = 2/6 时改写调试色，得到**与光照无关**的覆盖/定位视图。
    float4 emissiveOut = float4(0.0, 0.0, 0.0, 0.0);
    if (albedoDebug == 8u) {
        // 【T8 自检】用片段自身的 worldPos 反算 NDC 并编码成颜色：
        //   R = 0.5 + 0.5*ndc.x（左→右递增），G = 0.5 + 0.5*ndc.y（下→上递增）。
        //   与"像素在屏幕上的实际位置"逐点对照，即可判定投影是否一致。
        float4 hp = float4(input.worldPos, 1.0);
        float cx = dot(float4(vpRow0), hp);
        float cy = dot(float4(vpRow1), hp);
        float cw = dot(float4(vpRow3), hp);
        float2 ndc = float2(cx / max(cw, 1.0e-6), cy / max(cw, 1.0e-6));
        emissiveOut = float4(0.5 + 0.5 * ndc.x, 0.5 + 0.5 * ndc.y, 0.0, 1.0);
        dbgAlbedo = float4(0.0, 0.0, 0.0, 0.0);
    } else if (albedoDebug == 9u) {
        // 【T8 纹理自检】把材质数组的采样结果直接写进 emissive（绕过光照），
        //   UV 与材质层号都走与正常渲染完全相同的代码路径。
        uint layer9 = u_Materials[min(input.materialID, materialCount - 1u)].bindlessTextureBase;
        emissiveOut = float4(u_MatArray.SampleLevel(u_MatArraySamp,
                                                   float3(input.uv, (float)layer9), 0.0).rgb, 1.0);
        dbgAlbedo = float4(0.0, 0.0, 0.0, 0.0);
    } else if (albedoDebug == 2u) {
        emissiveOut = float4(0.0, 0.6, 0.0, 0.0);
    } else if (albedoDebug == 6u) {
        emissiveOut = (input.clusterIndex == debugCluster) ? float4(1.0, 0.0, 0.0, 0.0)
                                                          : float4(0.0, 0.0, 0.0, 0.0);
    }
    output.emissiveAO = emissiveOut;
    output.albedoMetallic  = dbgAlbedo;
    output.normalRoughness = float4(normal, matRoughness);
    // 【不要动 worldPos】它驱动 SSGI/DDGI；清零会让光照整片变黑，反而掩盖了"几何是否覆盖"。
    output.worldPosition   = float4(input.worldPos, 1.0);
    output.lightmapKey     = float4(input.uv,
                                    (float)(kNaniteObjectIndexBegin + input.instanceIndex), 0.0);

    // ── ④ 读数：硬光栅的片元数 ──
    // 【语义】PSO 是 `depthWrite = true` 且没有 discard ⇒ 主流驱动都会做 early-Z，
    //   被深度测试淘汰的片元**不会**进入本函数 ⇒ 本计数就是"真正写进 GBuffer 的像素数"。
    //   （若某个驱动不做 early-Z，本项会偏大成正比于覆盖率；这一点如实写在
    //     `NaniteRenderer` 的日志字段说明里，不假装它是精确的写入数。）
    // 【为什么用原子】每个片元都算一次。
    // 【读数分离】`pixels` = 硬光栅真正写进 GBuffer 的像素；`fallback_pixels` = 材质段缺失/越界的
    //   像素（正常必须 0，非 0 说明 `materialID` 映射坏了）。两者都进模块自持的硬光栅读数缓冲
    //   （binding 7 在本 PSO 上指向它，不是软光栅那 16 槽的缓冲）。
    InterlockedAdd(u_Stats[kHardStatPixels], 1u);
    if (!materialFromAsset) {
        InterlockedAdd(u_Stats[kHardStatFallbackPixels], 1u);
    }
    return output;
}
