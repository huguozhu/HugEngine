# HugEngine 全局光照（GI）系统的结构与实现细节

> **本文定位**：描述**当前代码**里 GI 系统的结构、数据模型、执行流程与各源的实现细节，
> 面向"要改 GI / 要接新 GI 源 / 要排查 GI 不生效"的开发者。
>
> **与其它文档的分工**：
> · 计划、判据、缺陷史与实测数据在 `docs/已实现功能/HugEngine GI架构与开发计划.md`（**已归档**，
>   该计划内任务全部完成）；
> · 后续框架类工作（统一降噪框架、Provider 执行单位收敛、P6）在
>   `docs/计划实现功能/Lumen与Nanite完整设计规范.md` §5.1 / §5.2；
> · 本文只讲**现在是怎么实现的**，并把"实现细节背后的理由"与"已知边界"写清 —— 因为这些理由
>   大多是踩坑换来的，删掉它们就会重踩。
>
> 文中所有数字与文件名都对着当前 HEAD 的代码；凡"未实现/未做"的地方都显式标注。

---

## 1. 一分钟总览

GI 在这个引擎里不是"一条管线"，而是**一组估计量 + 一套统一的合成规则**：

```
                    ┌──────────────── 场景与相机 ────────────────┐
GBuffer(8 MRT) ──┬─▶ Shadow(CSM/点/聚/矩形) ──▶ AS_Build(TLAS) ─┐ │
（albedo/normal/  │                                            │ │
  worldpos/…/     ├─▶ RSM_Generate(512²×3 附件) ─▶ RSM_Indirect │ │
  光照图键/深度） │      （光源视锥按场景包围盒拟合）  （半分辨率）│ │
                 │                                            ▼ │
                 ├─▶ 各 Provider 主 pass（帧图按注册表遍历）   │ │
                 │     · 屏幕空间：SSGI / SSR / SSAO·GTAO      │ │
                 │     · 探针：DDGI（可选 DDGI_Trace 硬件光追）│ │
                 │     · 光追：RTGI / RT 反射 / RTAO / RT 阴影 │ │
                 │     · 环境：IBL 烘焙（脏时）                │ │
                 │            ↓ 每个源各自附带降噪链（Aux pass）│ │
                 └──────────────▶ Lighting（全屏 PBR + 分层合成）◀┘
                                     ↓
                          HDR → 后处理 → ToneMap → 屏幕
```

三条设计主线：

1. **源（Source）= 一个估计量**：每个源只负责"我这份估计的间接光是多大"，不关心别人。
2. **通道（Channel）× 层栈（Stack）= 用户意图**：`漫反射 / 镜面 / AO` 三个通道各有一个
   源列表 + 权重；**归一化加权合成**保证"多开一个源不会变亮"。
3. **Provider = 执行单位**：帧图不写 `ShouldRunXXX`，而是遍历 Provider 注册表，问
   `NeedsPass(层栈)` 决定是否注册 pass；源自己声明输出到哪个通道、还需要哪些附属 pass。

---

## 2. 目录结构与关键文件

| 位置 | 内容 |
|---|---|
| `Engine/Render/GI/GITypes.h` | **GI 数据模型**（RHI-free）：`GISourceId`、分类谓词、能力位、置信度位、`GISourceDesc` / `GIChannelStack`、`GIConfig`、`GIRegistry`（可用性判定 / `Degrade`）、REDUNDANCY 诊断、`ToPipelineCap` / `ToConfidenceMask` |
| `Engine/Render/GI/IGIProvider.h` | **Provider 接口**：`GetSourceId` / `Handles` / `GetPassKind` / `IsValid` / `NeedsPass` / `SyncToStack` / `NeedsRadianceHistory` / 通道输出 / 附属 pass 链 / `GetTimedPass` / `Render` / `PreBind(Aux)` |
| `Engine/Render/GI/GlobalIllumination.h` | `IGlobalIllumination` 旧接口（仅 `GI_IBL` 仍以它被帧图使用）+ `GIDebugData`（面板与耗时读数） |
| `Engine/Render/GI/GI_IBL.{h,cpp}` | 环境光烘焙：辐照度 32²、预滤波 128²×5 mips、BRDF LUT 512² |
| `Engine/Render/GI/GI_RSM.{h,cpp}` | RSM 光栅（512²×3 附件：世界位置 / 编码法线 / VPL 辐射度） |
| `Engine/Render/GI/RSMFrustum.h` | RSM 光源正交视锥的**纯几何**拟合 + VPL 采样面积（可单测） |
| `Engine/Render/GI/RSMIndirect.{h,cpp}` | RSM 间接光的**半分辨率** VPL 求和（16 点 Poisson 盘） |
| `Engine/Render/GI/GI_SSGI.{h,cpp}` | 屏幕空间漫反射（默认 16 采样、半径 1.0、可半分辨率） |
| `Engine/Render/GI/GI_SSR.{h,cpp}` | 屏幕空间反射（Hi-Z 层次 march + 线性回退） |
| `Engine/Render/GI/GI_DDGI.{h,cpp}` | 探针网格 + 二阶 SH（9×float4/探针，32 采样/探针） |
| `Engine/Render/GI/DDGITracePass.{h,cpp}` + `GIProbeGrid.h` | 探针射线的硬件光追 march（可关）+ 网格拟合的纯几何 |
| `Engine/Render/GI/GITiming.{h,cpp}` | 每源 GPU 时间戳（环形查询池，不阻塞读回） |
| `Engine/Render/GI/GIRadianceHistory.{h,cpp}` | 前帧 HDR 辐射度（DDGI 回退 / SSGI 的 `L_in` 共同输入） |
| `Engine/Render/GI/SpatialDenoiseAux.h` | SSGI/SSR 共用的"主输出 → 空间降噪"附属 pass 适配器 |
| `Engine/Render/GI/*Provider.h` | **6 个常驻 Provider 类**：`ScreenAOProvider`（SSAO/GTAO 同 pass 双模式）、`IBLProvider`、`RSMProvider`、`SSGIProvider`、`SSRProvider`、`DDGIProvider`；**光追四效果**（阴影/RTAO/反射/RTGI）共用一个 `RTProvider.h` 里的 `RTEffectProvider`（4 个实例） |
| `Engine/Render/RT/` | RT 效果实现：`RTShadowPass` / `RTAOPass` / `RTReflectionPass` / `RTGIPass` + `RTEffectPass`（RT 管线 + SBT + set0 生命周期）+ `STBN` |
| `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp` | **帧图**：pass 顺序、Provider 遍历、各源依赖声明、Lighting 输入装配 |
| `Engine/Render/Pipeline/LightingPass.{h,cpp}` | Lighting 的 PSO / 描述符集（每飞行帧一份）/ 合成参数 UBO / 中性占位纹理 |
| `Engine/Shader/Shaders/Lighting/DeferredLighting.frag.slang` | **合成端**：源分派 `SampleDiffuseSource` / `SampleSpecularSource` / `SourceWeight` + 三通道归一化 + AO 只作用于间接项 + 白炉 |
| `Engine/Shader/Shaders/Lighting/PBR.frag.slang` | Forward 的 GI（IBL / RSM 内联），复用同一份 `GIBlendParams` |
| `Engine/Shader/Shaders/GI/*` | 各源的 pass 着色器（SSGI/SSR/SSAO/SSAO_Blur/GTAO/RSM_* /IBL_*/DDGI.comp） |
| `Engine/Shader/Shaders/RayTracing/*` | RT 源：`RT_GI.*`、`RT_Reflection.*`、`RT_AO.rgen`、`RT_Shadow.*`、`DDGI_Trace.rgen`、`RT_HitCommon.slang`（命中点辐射度共用） |
| `Engine/Shader/Shaders/ShaderTypes.slang` | C++/Slang **共享**的 GPU 结构体与 binding 常量（GI 源 id、`GIBlendParams`、`GPUObjectData`…） |
| `Tools/gi/*` | 采样与判据脚本（见 §11） |

---

## 3. 数据模型（`GITypes.h`）

### 3.1 源与分类

`GISourceId`（11 个），按**估计器类别**分三类（三个互斥且完备的谓词，不再有"低频/中频/高频"枚举）：

| 类别 | 谓词 | 源 | 语义 |
|---|---|---|---|
| 世界空间 / 环境 | `IsWorldSpaceSource` | `IBL`、`Lightmap`（**未实现**）、`DDGI` | 与视角无关，屏外仍有效 |
| 屏幕空间 / 单次反弹光栅 | `IsScreenSpaceSource` | `SSGI`、`SSR`、`SSAO`、`GTAO`、`RSM` | 逐屏幕像素估计，受屏幕覆盖限制 |
| 硬件光追 | `IsRayTracingSource` | `RTGI`、`RTReflection`、`RTAO` | 需要 TLAS + RT PSO |

> 历史上这里有一个 `GIBand {Low,Mid,High}`，被删除的原因是**它混了两个正交维度**（尺度与精度）：
> 反例是 RTGI 输出 1/4 分辨率、空间分辨率低于全分辨率 SSGI，却被标成"高频"。三个谓词取代它，
> 面板标签由谓词推导。

### 3.2 能力位与降级

- `kPipelineGI*` 位：`ToPipelineCap(GISourceId)` 把源映射到管线能力位。
  `PipelineCaps::Forward` = 光栅阴影 + IBL（漫反射/镜面）+ RSM；`Deferred` = 全部源 + RT 阴影；
  `Lightmap` **显式返回 `kPipelineGINone`**（刻意不给位，不是"忘了写分支"）。
- `GIRegistry::Degrade(cfg, pipelineCaps, rtSupported)`：逐通道裁掉"该管线承载不了"或"设备不支持"
  （无光追硬件裁 RT 源）的源；**只裁不加**、幂等。阴影通道另行按设备能力在 RT/光栅之间回退。
- 单元测试锁住两条安全性质：`Degrade` 只裁不加且幂等；未实现的源（如手工塞进层栈的 Lightmap）
  一定被裁掉 —— 避免出现"在归一化里计权重、却没人产出"的源。

### 3.3 层栈与配置

```cpp
struct GISourceDesc { GISourceId id; float weight; float falloffDistance; };
struct GIChannelStack { GISourceDesc sources[kGIMaxSourcesPerChannel /*4*/]; u32 count; u32 mode; };
struct GIConfig {
    GIChannelStack diffuse, specular, ao;   // 三个通道各自的源列表
    ShadowChannel  shadow;                  // None / Raster / RT（阴影不进层栈）
    float giIntensity, aoIntensity, edgeFade;
    bool  halfRes;                          // 屏幕空间源半分辨率
    bool  furnaceMode;                      // 白炉数值测试
    // 预设：Low / Medium / High / Ultra（Ultra 用光追源，无 RT 设备经 Degrade 回退）
};
```

### 3.4 逐像素置信度

置信度**位掩码**逐槽进 UBO（`GISourceSlotData::confidence`，由 `ToConfidenceMask` 统一推导），
着色器**不认识任何源 id 列表**：

- `kGIConfCameraCoverage`：屏幕覆盖 —— 视口外/边缘淡出 ⇒ 权重 0（`edgeFade` 是带宽，占短边比例）。
  适用源由 `IsCameraViewLimitedSource` 给出：**SSGI · SSR · SSAO · GTAO · RTGI · RT 反射 · RTAO**
  七个。注意它**不含 RSM**：RSM 的产物是光源视锥下的 VPL 图、按世界空间求和，与相机视口无关，
  给它乘屏幕覆盖置信度是错的（两个谓词各有用途，不能合并）；
- `kGIConfProbeGrid`：探针网格覆盖 —— 网格外一格线性淡出、再外面归零（只对 DDGI 置位）。

`SourceWeight(s, camCoverage, gridCoverage, camDist)` = `用户权重 × 置信度 × 距离让位`；
**距离让位默认关闭**，它是艺术控制而不是物理量（不变量 2）。

### 3.5 REDUNDANCY 诊断

`GIDiagnosticKind` 四类，跑在配置层（不依赖 GPU）：

| 诊断 | 判定 |
|---|---|
| 严格冗余 | 两个源在合成端解析到**同一张纹理**（当前仅 SSAO/GTAO） |
| 重复估计 | 同一物理量的"屏幕空间"与"光追"两份估计同时启用（如 SSGI + RTGI） |
| 相关估计 | 两个源的估计量高度相关（提示可能白付成本） |
| 成本提示 | 通道含多个源 ⇒ 每帧多份整幅 pass |

`GIRegistry::DedupStrict` 可对"严格冗余"做**可证等价**去重（同组取高质量者，GTAO 优先于 SSAO）。

---

## 4. Provider 抽象（`IGIProvider.h`）

| 成员 | 作用 |
|---|---|
| `GetSourceId()` / `GetName()` | 身份；`Handles(id)` 支持"一个 Provider 代表多个源"（SSAO/GTAO 同 pass 双模式） |
| `GetPassKind()` | `Offscreen` / `Compute` / `Custom` —— 帧图据此选择注册方式 |
| `HasTextureOutput()` | 产物是否需要导入帧图当纹理（探针/RSM 类由 shader 直接读内部资源 ⇒ false） |
| `IsValid()` / `NeedsPass(stack)` | 本帧是否注册：`stack.Has(id) && IsValid()` |
| `SyncToStack(stack)` | 层栈选择与 pass 模式不一致时的同步钩子（如选 GTAO ⇒ 切 pass 模式） |
| `NeedsRadianceHistory()` | 是否消费**前帧 HDR**（DDGI 的探针辐射度回退、SSGI 的 `L_in`）。**捕获门控与消费者同源**，避免"捕获写漏 ⇒ 采样未写入纹理、输出恒 0" |
| `GetTimedPass()` | 耗时读数落点（回写 `IGlobalIllumination::SetRenderTimeMs`） |
| `GetDiffuse/Specular/AOOutput()` + `GetFinal…` | 输出到哪些通道；`Final` 是跑完附属（降噪）链之后的纹理 |
| `GetAuxPassCount/Name/Input/Output` + `PreBindAux` / `RenderAux` | **附属 pass 链**（降噪/累积）也纳入注册表驱动，帧图不为每种源手写 |
| `Initialize` / `Shutdown` / `OnResize` / `Render` / `PreBind` / `SetInputs` | 生命周期与执行 |

`GIProviderContext`（帧图注入）：`world` / `sceneGraph` / `camera` / `frameIndex` / `furnace`，
光追类另有 `lightBuffer` / `lightCount` / `tlas`。

**注册**（`DeferredPipeline::Initialize`，顺序即帧图遍历顺序）：
`ScreenAOProvider` → `IBLProvider` → `RSMProvider` → `SSGIProvider` → `SSRProvider` → `DDGIProvider`
→（RT 基础设施就绪后）`RTEffectProvider` × 4（Shadow / AO / Reflection / GI）。

---

## 5. 帧内执行流程

`DeferredPipeline::Render` → `BuildFrameGraph`（声明式，自动 barrier）→ `Execute`。pass 顺序：

```
[Compute] GPU_Cull_Phase1 / GPU_Cull（上一帧深度 Hi-Z 遮挡剔除）
          Shadow（CSM + 点/聚/矩形；写阴影图 + hdrDepth 以建立 WAW 顺序）
          GB_Clear（8 MRT：albedo+metallic | normal+roughness | emissive+ao | velocity |
                    worldPos+F0 | disneyA | disneyB | 光照图键）
          HiZ_Build（下采样深度金字塔，供 SSR 与剔除）
          GPU_Cull_Phase2
          RSM_Indirect（半分辨率 VPL 求和；必须排在 RSM 之后、Lighting 之前）
          AS_Build（TLAS）
          DDGI_Trace（探针射线的硬件光追；排在 AS_Build 之后、DDGI 之前）
          ── Provider 主 pass（按注册顺序；每个源后面紧跟它的附属降噪 pass）──
          IBL_Bake（仅脏时，且恒注册、内部早退）
[Graphics] Lighting（全屏 PBR + 三通道归一化合成）
          Skybox / CaptureRadiance（捕获本帧 HDR，作下一帧的入射辐射度）
          AutoExposure / Bloom / DOF / MotionBlur / TAA_Resolve / ToneMap /
          ColorGrading / CameraEffects / SMAA / FXAA
```

GI 相关的几个顺序理由（都不能随便调）：

- `RSM_Indirect` 与 `Lighting` **同队列且按注册顺序**对齐：RSM 的三张图由 `GI_RSM` 自己持有、
  不在帧图资源表里，因此顺序只能靠注册顺序保证；
- `DDGI_Trace` 必须在 `AS_Build` 之后、`DDGI.comp` 之前（它写的辐射度缓冲就是探针更新的输入）；
- `CaptureRadiance` 必须在 Lighting 之后（它捕获的就是本帧 Lighting 结果）；
- **每个源是否注册由 `NeedsPass(层栈)` 决定**，帧图据此决定"要不要声明读写依赖"与"要不要把纹理
  绑给 Lighting" —— 二者用**同一个**布尔，避免"pass 没跑但描述符仍绑着真实纹理"（会采样到上一帧
  或未初始化显存）。

---

## 6. 合成细节（`DeferredLighting.frag.slang`）

### 6.1 量纲约定（所有源必须遵守）

```
间接漫反射源：返回 L_o = albedo_接收面 × (E/π)     —— 已含接收面 albedo 与 1/π
间接镜面源：  返回完整镜面辐射度（含菲涅耳与环境 BRDF）
AO 源：       返回遮蔽因子（0..1），不是能量
```

合成端对每个源只做"乘权重、加进 num/den"，**不再做任何量纲换算**。这条约定是任务 10 / 33 换来的：
此前各源量纲不一（有的返回 E、有的返回 L、有的漏 albedo），归一化混合它们等于混合不同物理量。

### 6.2 三个通道的合成

```hlsl
// 直接光先单独记下（AO 不应作用于直接光与自发光）
const float3 directColor = color;

// ① 间接漫反射：Σ(源 × 权重) / Σ权重（mode==0 时退化为直接相加，仅作对照）
w   = SourceWeight(slot, camCoverage, gridCoverage, camDist);
num += SampleDiffuseSource(id, …) * w;   den += w;
indirectDiffuse = (mode != 0) ? (den > 0 ? num/den : 0) : num;

// 白炉：到此即返回（所有源都在归一化里，读数必须 = 1）
if (furnace) return float4(directColor + indirectDiffuse, 1.0);

// ② AO：只算遮蔽因子，归一化加权（SSAO/GTAO/RTAO 可同时参与）；
//     没有 AO 源时 aoVal = 1（不遮蔽）；aoFactor = lerp(1, materialAO*aoVal, aoIntensity)
// ③ 间接镜面：与 diffuse 同构；SSR / RT 反射先用 alpha 协议筛掉"本条无效"
if (u_SSR.Sample(uv).a < 0.0) continue;              // 该源本条光线无效 → 权重 0
if (u_RT_Reflection.Sample(uv).a < 0.0) continue;
indirectSpecular = (mode != 0) ? specNum / max(specDen, 1e-4) : specNum;

// ④ 最终合成：AO 只作用于间接项；自发光最后加；再叠空中透视
color = directColor + (indirectDiffuse * giIntensity + indirectSpecular) * aoFactor;
color += emissive;
```

**有效性协议（`alpha < 0`）**：屏幕空间与光追反射源在"本条光线无效"（高粗糙度跳过 / miss）时返回
`float4(0,0,0,-1)`；合成端读 alpha 决定是否让这份权重参与归一化。缺了它，"miss 的黑色"会以全权重
进入归一化、把环境反射压暗约一半（§9.2-C 的四处断裂）。

**白炉（furnace）**：全白环境 + `albedo = 1` + 关直接光的解析真值条件下，每个源的贡献都必须是 1。
它专门用来抓"归一化正确但量纲错"的缺陷：SSGI 与 RTGI 现在走真实路径并在白炉下返回理想值
（读数恒 1.0000，负对照 0.0000）；IBL / DDGI / RSM 仍走短路，它们的逐源真值校验**未做**。

---

## 7. 各源实现细节

> 成本数字来自本机 1080p 的 `HE_GI_TIMING` / `HE_PASS_TIMING` 读数，用于量级比较，不作为跨机器结论。

| 源 | 估计量 | 输出 | 分辨率 | 每帧成本 | 备注 |
|---|---|---|---|---|---|
| **IBL** | 环境辐照度（32² cubemap）+ 预滤波（128²×5 mips）+ BRDF LUT（512²） | diffuse + specular | 图集 | 仅**脏时**烘焙（首次约 5 ms，之后 0） | 恒注册、内部 `IsDirty()` 早退 |
| **RSM** | 单次反弹 VPL（16 点 Poisson 盘） | diffuse | RSM 512²×3；间接光半分辨率 | RSM 光栅 ~0.14 ms + 间接 ~0.14 ms | 光源视锥按**场景包围盒**拟合（固定、视角无关） |
| **SSGI** | `Σ(L_in·cosθ)/Σcosθ`，解析上等于 `E/π` | diffuse | 全/半分辨率（`halfRes`） | ~0.44 ms @16 采样（64 采样 1.27 ms） | 需前帧 HDR 作 `L_in`；噪声较高 |
| **SSR** | Hi-Z 层次 march + 线性回退 | specular | 全/半分辨率 | ~4.3 ms（场景尺度参数、256 步） | 只能命中深度图里的**可见面**；`alpha<0` 协议 |
| **SSAO / GTAO** | 屏幕空间遮蔽（GTAO：地平线切片 + 解析积分） | AO | 全/半分辨率 | — | 同一 Provider 双模式；半分辨率时**不降噪**（已知边界） |
| **DDGI** | 探针网格二阶 SH（9×float4/探针，32 采样） | diffuse | 探针场（拟合后 192/1408/9408 探针） | 探针更新 ~0.019 ms + 可选光追 march | 网格按场景包围盒拟合；网格覆盖置信度；可 `updateStride` 时间分摊 |
| **RTGI** | 余弦加权半球追踪 | diffuse | 1/4 分辨率（默认） | — | 命中点辐射度走共用 `EvaluateHitRadiance` |
| **RT 反射** | 镜面方向追踪 + GGX | specular | 1/2 分辨率 | — | 同 `alpha<0` 协议 |
| **RTAO** | 半球遮蔽 | AO | — | — | 与 SSAO/GTAO 归一化共存 |
| **RT 阴影** | any-hit 可见性 | 乘性（不进层栈） | — | — | 阴影是可见性项（不变量 5） |

### 7.1 IBL（环境光照）

- 三个产物：辐照度 cubemap 32²、预滤波 128²（5 mips，粗糙度分级）、BRDF LUT 512²（RG16F）。
- 走**光栅化**路径（全屏三角形 + 逐面 offscreen pass），不需要 compute。
- **只在脏时烘焙**：`GI_IBL::IsDirty()` 由天空盒变更置位；帧图**恒注册**该 pass、由内部早退
  （不脏时耗时读数为 0）。恒注册的理由：它有三个消费者（漫反射通道、镜面通道、DDGI 的辐射度
  回退），按消费者清单门控必然漏（§9.2-Q 就是这么漏的）。
- 天空盒必须在烘焙**之前**交给 `GI_IBL`（Forward 的 RG 路径曾漏掉这一步，导致烘焙的是"未设置的
  天空盒"、IBL 恒为 0）。

### 7.2 RSM（反射阴影贴图）

- 三张 512² 附件：**世界位置** / **编码法线** / **VPL 出射辐射度**（"一个附件一个量"，此前两个量
  挤在一张图里导致 DDGI 把编码法线当辐射度读）。
- 辐射度 `L_v = albedo·lightColor·intensity·NdotL/π`，不再是灰度标量。
- **光源视锥按场景包围盒拟合**（`GI/RSMFrustum.h`，纯几何 + 单测）：固定、不随相机；帧图每 30 帧
  重算包围盒。旧实现硬编码 `sceneCenter=(0,3,0)/radius=60`，在一个 3720 单位宽的场景里只覆盖 1/60。
- **VPL 采样面积由光锥推出**：`scale = (radiusUV·2·halfExtent)²/N`，替代旧经验常数（两者差 3942 倍）。
- **间接光在半分辨率独立 pass**（`RSM_Indirect`，`kDownscale = 2`）：此前它在 Lighting 里逐全分辨率
  像素做 16×2 次采样，独占约 0.45 ms（Lighting 含 RSM 0.882 ms 对不含 0.433 ms）。搬走后 Lighting
  只做一次升采样。
- 门控：`ShouldRunDDGI() || ShouldRunRSM()` ∧ `HasActiveShadows()`（两个消费者独立，任一需要就渲染）。
- `BeginOffscreenPassMRT` 的**清除值长度契约**：`clears` 需 `colorCount` 个颜色项 + 末尾一个深度项。
  写少一项会越界读栈上垃圾当深度清除值 ⇒ 三张图只剩清除值（这是任务 30 定位到的坑，契约已写进
  `RHI/CommandList.h` 的注释）。

### 7.3 SSGI（屏幕空间间接漫反射）

- 估计量：`Σ(L_in·cosθ)/Σcosθ`，由 `∫cosθdω = π` 可知它**精确等于 `E/π`** —— 归一化常数是解析值 1，
  不需要"以 PT 为参考标定一个增益"。
- `L_in` 取**前帧 HDR**（`GIRadianceHistory`）；消费者通过 `NeedsRadianceHistory()` 声明，帧图据此
  门控 `CaptureRadiance`。
- 采样方向由 GBuffer 法线构造 TBN 后变换（世界/view 空间混用曾是主因缺陷）；可见性判据是
  `sZ ≤ sPos.z + bias`（view 空间朝 −Z）。
- 默认 16 采样、半径 1.0，可半分辨率（半分辨率时**附属降噪被跳过**，是已知边界）。
- 与 DDGI 的关系：两者估计同一物理量但**不同频段**——实测不存在使 `LowPass(SSGI) ≈ DDGI` 的
  低通尺度（这是 P5"频率分离"整波退场的依据）。两者同时启用会被 REDUNDANCY 判为"重复估计"。

### 7.4 SSR（屏幕空间反射）

- 两条 march：**Hi-Z 层次**（屏幕空间 DDA，默认）与**线性回退**（`ssr_use_hiz=0`，正式支持的回退路径）。
- **屏幕参数必须做透视校正**：屏幕段的参数 `t` 不是射线参数，`1/w` 才在屏幕空间线性。修法：
  `w(t) = w0·wT/((1−t)·wT + t·w0)`、`tau(t) = t·worldLen·w0/((1−t)·wT + t·w0)`，由此精确得到射线点
  与其 NDC 深度。不做校正时实测偏差可达命中容差的 100 倍以上（反射整片丢失）。
- 空间约定：重建（clip → view）与投影（view → clip）都必须用**负高度视口**的 y 约定
  （`ndc.y = 1 − 2·uv.y`，见 `UvToNdc` / `NdcToUv`）；法线是**世界空间**，反射前必须转到 view 空间。
- **场景尺度参数**（`GI_SSR::autoScaleMarch`，帧图按场景包围盒推导）：`maxDistance = diag`、
  `thickness = diag×0.0025`、`stepSize = thickness`、`maxSteps ≥ 256`。米制默认值（50/0.1/0.5/64）
  在 3720 单位宽的场景里几乎什么都找不到。
- 命中判据：`|rayPos.z − gZ| < thickness`（世界空间厚度）；早退用行进方向点乘
  （`(rayPos.z − gZ)·R.z > 0`），**不能**把"仍在几何之前"记成命中。
- 限制：SSR 只能命中深度缓冲里**相机可见**的那一面（背面/底面永远命中不了）；单 pass 约 4.3 ms
  （场景尺度参数下），是 GI 里最贵的一项。

### 7.5 DDGI（动态漫反射探针）

- 结构：3D 探针网格 → compute 每帧采样/追踪更新二阶 SH（band 0/1/2 = 9×float4/探针，32 采样/探针）
  → Lighting 三线性插值采样。
- **网格按场景包围盒自动拟合**：纯几何在 `GIProbeGrid.h`（有单测）。规则：三轴共用同一格距，但每轴
  探针数按自己的边长取 `ceil(size/cell)+1`；原点是包围盒最小角。
- **网格覆盖置信度**：网格外一格线性淡出、再外面归零 —— 网格外的三线性插值只会 clamp 到边界探针
  （"贴边常数外推"），置信度让权重让给同通道的其他源。
- **探针射线的硬件光追**（`DDGITracePass` + `DDGI_Trace.rgen`）：每条探针射线追真实辐射度，命中复用
  `RT_GI.rchit`，未命中取 IBL 辐照度。没有它时 IBL 回退路径只按方向采样、Fibonacci 方向对每个探针
  相同 ⇒ 整片探针场 SH 逐位相同（内容与探针位置无关）。
- **时间维分摊**（`updateStride`）：每 N 帧只更新一轮探针，未轮到的把**历史拷进当前**（两个探针缓冲
  逐帧 ping-pong，不拷会倒退一代）。判据是"等量工作 ⇒ 等量结果"。
- **重建探针缓冲必须让历史失效**（新缓冲是未初始化显存，否则垃圾会按混合权重混进 GI）。
- 采样约定：`u_DDGIGridOrigin` / `u_DDGIGridSize`（网格参数 UBO，binding 6）供 Lighting 与 RTGI 共用。

### 7.6 光追四源（RTGI / RT 反射 / RTAO / RT 阴影）

- 共用 `RTEffectPass` 基类（RT 管线 + SBT + set0 生命周期）与 `RTEffectProvider`（参数化四种效果）。
- **命中点辐射度共用一份**（`RT_HitCommon.slang::EvaluateHitRadiance`）：
  `L_o = albedo/π·(E_ambient + E_direct)` —— 两处量纲（命中面 albedo、1/π）在三个调用点必须一致。
  白炉下返回理想值（命中与未命中两条路径都返回），使白炉读数**与场景几何无关、恒等于 1**。
- 有效性：反射类写 `alpha = -1` 表示本条无效；RTGI 输出 1/4 分辨率（默认）。
- CT 常量（`GPUShadowData.lightViewProj[3]` / `GPULight`）与着色器逐字段一致（有 `static_assert` 尺寸）。

### 7.7 屏幕空间 AO（SSAO / GTAO）

- 一个 Provider 两种模式（`Handles(SSAO)` 与 `Handles(GTAO)` 都为真），层栈选哪个就切哪个模式；
  二者互为替代，同时启用会被 REDUNDANCY 判为**严格冗余**并可去重。
- AO 只作用于**间接项**（直接光与自发光不乘 AO）；`aoIntensity` 是强度控制。
- SSAO 的模糊（`SSAO_Blur`）在 SSAO pass **内部**完成，因此本 Provider 不暴露附属 pass 链；
  半分辨率的"不降噪"问题只出现在 SSGI / SSR（它们有独立的 `SpatialDenoiseAux` 链）。

---

## 8. Forward 管线的 GI

- Forward 只支持**世界空间源**：IBL（漫反射 + 镜面）与 RSM（漫反射）。能力位由
  `PipelineCaps::Forward` 声明，面板与 `Degrade` 都读它。
- 合成走**同一份** `GIBlendParams`（同结构、同 binding 31）：`PBR.frag` 逐通道遍历层栈、
  `Σ(贡献×权重)/Σ权重`，行为与 Deferred 的归一化一致（判据：双源读数 = 两单源加权平均）。
- RSM 是**内联 5×5 网格求和**（没有 GBuffer，用不了共享的半分辨率 pass）：
  · 光源 VP 来自 `GIBlendParams.rsmLightViewProj` —— 与 `RSM_Generate` pass **同一个**按场景包围盒
    拟合的固定光锥（写入 UV 与查找 UV 同源）；
  · `rsmValid` 由 C++ 统一判定（层栈含 RSM ∧ 有投影方向光 ∧ 本帧 pass 真的注册）⇒ pass 没跑时
    着色器不会去采未初始化的 RSM 纹理；
  · `rsmVplScale` 由同一光锥的采样面积推出。
- **阴影系统要由调用方驱动**：`ShadowSystem` 不像 GI 子系统那样自己从帧图取数据，调用方必须先
  `SetRenderResources` + `Update`，否则 `HasActiveShadows()` 恒假 ⇒ `Shadow` 与 `RSM_Generate`
  都不注册（06.GILab 曾因此既没有阴影、RSM 也恒为 0）。

---

## 9. 横切机制

### 9.1 场景尺度拟合（同一个教训的三个实例）

不要给"场景尺度"写常数。三处都按场景包围盒推导（遍历 `MeshComponent::GetBounds()` × 世界矩阵，
每 30 帧重算；**不要**用 `m_FrameCounter` 做节流，它只在异步计算路径自增）：

| 用途 | 推导 |
|---|---|
| DDGI 探针网格 | `FitProbeGridToBounds`：格距 + 每轴探针数 + 原点 |
| RSM 光源视锥 | `FitRSMFrustumToBounds`：固定光锥 + 正交半宽（→ VPL 采样面积） |
| SSR march 参数 | `maxDistance = 对角线`、`thickness = 对角线 × 0.0025`、`stepSize ≤ thickness` |

### 9.2 半分辨率

`GIConfig::halfRes` 打开后，屏幕空间源（SSGI/SSR/SSAO/GTAO）输出纹理降半（省约 3/4 像素着色），
Lighting 侧线性升采样。**已知边界**：半分辨率下 SSGI/SSR 的附属降噪被跳过（`AuxActive()` 直接
返回 false），根治需要"重建升采样"这一信号属性（属统一降噪框架）。

### 9.3 降噪现状

| 类别 | 现状 |
|---|---|
| 屏幕空间源 | `Denoiser`（空间 5×5 双边；σ 可配 `SetDepthSigma/SetNormalSigma`，SSGI/SSR 共用 `SpatialDenoiseAux.h`） |
| 光追效果 | `RTDenoiser`（时域累积 + 空间滤波），链路是 `std::vector<Stage>`（顺序即执行顺序，加一级只需 push） |
| 统一框架 | **未做**（`DenoiseSignal` + 统一历史分配 + 批量 dispatch + 框架级有效性契约）。任务已迁到 `Lumen与Nanite完整设计规范` §5.1，因为它的验收对象是 Lumen 的多信号共存 |

### 9.4 读数与告警

- **每源 GPU 耗时**：`GITiming` 环形查询池 + 不阻塞读回 + 滚动平均（`HE_GI_TIMING=1` 打印）。
  写回 `IGlobalIllumination::SetRenderTimeMs`，面板那几行才有真数。
- **纹理"已写入"登记**：RHI 记录每个纹理是否被写过，一次性告警报出 set/binding/尺寸/格式 ——
  把"采样未初始化显存"从静默错误变成日志（历史告警基线已清零）。
- **帧图 lambda 按值捕获**：不要把栈上局部（如 `float4x4 lightVP`）以引用交给延迟执行的 pass。

### 9.5 共享结构体的纪律（C++ / Slang 双端）

`ShaderTypes.slang` 是两端**同一个定义**，但布局规则不同：

- std140 下**数组元素步长固定 16 字节** —— 所以共享结构体里的填充一律用 `float4`。曾经用
  `float _padBlend[3]`（C++ 12 字节、Slang 48 字节）导致其后所有字段偏移错开、着色器静默读 0。
- `Pipeline/Material.h` 把 `GIBlendParams` 的 `sizeof` 与关键字段 `offsetof` **逐个钉死**，并在
  `slangc -reflection-json` 下核对过。
- **往对象缓冲加字段要填所有写者**：`GPUObjectData` 现有三个写入点（`SceneRenderer`、`CSMTechnique`、
  `GI_RSM`），漏填会出现"读 AABB 读成垃圾"。

---

## 10. 调试与验证设施（`Tools/gi/`）

| 工具 | 用途 |
|---|---|
| `dump_gi.ps1` + `analyze_gi.py` | 四变体采样（none/ddgi/ssgi/both）+ 单源做差 + 量级/相关性；**陈旧转储护栏**（运行前删产物、运行后校验新鲜度） |
| `p5_spectrum.py` | 径向功率谱 + 低通扫描（频率维判据） |
| `ssgi_cal_check` / `confidence_check` / `rsm_gate_check` / `ibl_lut_gate_check` / `stack_switch_check` | 各源的“是否真的生效/门控正确”判据 |
| `ddgi_grid_check` / `amortize_check` / `rsm_indirect_check` | DDGI 网格拟合、时间分摊、RSM 链路逐级非空 |
| `ssr_check` / `ssr_mirror_check` | SSR 有效性 + **平面镜解析对照**（反射落点/定位/抖动/负对照/Hi-Z 断言/步数断言） |
| `forward_stack_check` | Forward 层栈归一化 + RSM 生产者（含"换相机后 RSM 三图逐字节相同"的视角无关性） |
| `rtgi_coupling_check` / `rtgi_furnace_check` | RTGI 源独立性 + 白炉真值（命中/未命中都返回理想值） |
| `repeatability_check` / `timing_check` / `cost_report` / `crash_handler_check` / `soak_launch` | 配置可复现性、耗时读数、成本表、崩溃处理器、长跑 |
| `vk_layer_settings.txt` | 关闭校验层重复消息上限（计数只在同一设置下可比） |

**三条判据纪律**（写下来是因为都踩过）：

1. **A/B 对照必须给每次运行一份私有 cfg 副本** —— 示例退出时会回写 `HE_GILAB_CONFIG` 指向的文件，
   复用同一文件会把配置差异误读成代码差异。
2. **读数差异先证明"这个数字是本次跑出来的"**，再去解释它为什么变（陈旧转储曾让一个不存在的
   "绝对读数依赖二进制布局"被追查很久）。
3. **"有效率/同量级"这类内部一致性判据看不见方向错与量级错** —— 位置类问题要用解析真值
   （平面镜），量级类问题要用白炉。

---

## 11. 已知边界与未做项

| 项 | 状态 |
|---|---|
| SSR 只能反射相机可见面；单 pass ~4.3 ms | 固有性质 + 已知成本，半分辨率/降噪是后续方向 |
| SSR Hi-Z 在"射线脚下的地面永远比射线近"的几何里层级长期停在 0 | 时间收益远小于步数收益（实测 ~1.06×）；优化（只在穿越时降级）已记为重开条件 |
| Lightmap 源 | **未实现**（`ToPipelineCap` 刻意不给能力位）。原任务已取消；已落地的基础设施（GBuffer 第 8 MRT 的光照图键、程序化箱式投影、检查脚本）保留 |
| 统一降噪框架 / Provider 执行单位收敛 / P6 | **未做**，任务在 `Lumen与Nanite完整设计规范` §5.1 / §5.2 |
| IBL / DDGI / RSM 的逐源白炉真值校验 | 未做（RTGI 与 SSGI 已补齐） |
| 其它示例（02.Cube / 03.Sponza-Forward / AISamples）的 Forward 观感 | 未逐个跑图（工作区只构建 06.GILab）；IBL 修好、RSM 换固定光锥后画面变亮/变阴影是修复 |
| 半分辨率下不降噪 | 同上（需 `needsUpscale` 信号属性） |

---

## 12. 扩展指南：新增一个 GI 源

1. **枚举与分类**：`GITypes.h` 的 `GISourceId` 加一项；把它放进三个分类谓词之一；`ToPipelineCap`
   给出能力位；需要置信度则给 `ToConfidenceMask` 加位。
2. **ShaderTypes 同步**：`ShaderTypes.slang` 加 `GISOURCE_XXX`（与 C++ 数值一致）。
3. **Provider**：实现 `IGIProvider`（最省是可复用某个现有 Provider 的形状），在
   `DeferredPipeline::Initialize` 里注册；声明 `GetDiffuse/Specular/AOOutput()`、
   `NeedsRadianceHistory()`、附属降噪链。
4. **合成端**：`DeferredLighting.frag.slang` 的 `SampleDiffuseSource` / `SampleSpecularSource`
   加一个 `case`（**必须返回已乘接收面 albedo 的 `E/π`**，或完整镜面辐射度）；
   需要"本条无效"就写 `alpha < 0` 协议。
5. **帧图**：只需在 Provider 遍历里被注册（`Offscreen`/`Compute`/`Custom` 三种形状各有一处循环），
   无需改 UBO 结构或合成循环。
6. **面板与降级**：候选列表按能力位自动派生；`Degrade` 自动处理设备能力。
7. **判据**：照 §10 的纪律写一个检查脚本（生效性 + 门控 + 量级/位置/白炉），并登记进文档。

---

## 13. 术语与常量速查

| 名称 | 说明 |
|---|---|
| 通道 stack | `diffuse` / `specular` / `ao` 三个源列表（每通道最多 4 个源） |
| `mode` | `0` = 相加（对照），`1` = 归一化加权（默认） |
| `furnace` | 白炉数值测试：全白环境 + albedo 1 + 关直接光，各源真值 = 1 |
| `alpha < 0` | 该源"本条光线无效"（SSR / RT 反射 / RSM 未产出等） |
| 置信度位 | `CAMERA_COVERAGE`（屏幕覆盖）/ `PROBE_GRID`（DDGI 网格覆盖） |
| 能力位 | `kPipelineGI*`：管线"架构上能否承载"，与设备能力（`rtSupported`）两层判断 |
| 关键 binding | 0/1/2 GBuffer A/B/C、3 Depth、4 光照图键、5 RSM 间接光、6 DDGI 网格参数、9 聚光阴影、17 光源 SSBO、18 阴影 SSBO、19 SSGI、20 SSAO、21 SSR、22 DDGI 探针、23 GBuffer worldPos、24–27 RT 效果、28/29 disneyA/B、31 `GIBlendParams` |
| 关键常量 | IBL：32²/128²×5/512²；RSM：512²×3 + 16 VPL；SSGI：16 采样/半径 1.0；DDGI：32 采样/探针、二阶 SH；RTGI：1/4 分辨率；RSM 间接光 `kDownscale = 2` |
