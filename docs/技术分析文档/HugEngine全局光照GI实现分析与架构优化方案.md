# HugEngine 全局光照（GI）实现分析与架构优化方案

> 日期：2026-09-08
>
> 说明：本文由三份文档按逻辑顺序（**现状分析 → 架构选型 → 工程落地**）合并而成：
> 1. `HugEngine全局光照GI实现分析与优化建议.md`（现状盘点 + 正确性缺陷 + 优化建议 + 工业界缺口）
> 2. `HugEngine GI架构优化与选型方案.md`（三层模型与 5 个 Phase 概要设计 + 第八章修订）
> 3. `HugEngine GI架构优化-详细落地方案.md`（工程落地细化）

## 目录

- 第一部分 · GI 实现分析（现状盘点 + 正确性缺陷 + 优化建议 + 工业界缺口）
- 第二部分 · 架构优化与选型方案
- 第三部分 · 详细落地方案

---

# 第一部分 · GI 实现分析（现状盘点 + 正确性缺陷 + 优化建议 + 工业界缺口）

> 范围：`Engine/Render/GI/` 光栅化 GI 子系统 + `Engine/Render/RT/` 硬件光追路径 + `Engine/Render/PostProcess/SSAO`
> 说明：本部分在 `docs/HugEngine全局光照GI实现原理.md`（实现细节）之上，聚焦"现有实现的正确性缺陷与优化空间"。

---

## 一、现有 GI 技术清单

工程里 GI 分两套并行体系：**光栅化 GI 子系统**（`Engine/Render/GI/`，统一继承 `IGlobalIllumination`）和**硬件光追路径**（`Engine/Render/RT/`，继承 `RTEffectPass`）。

| # | 技术 | 文件 | 原理 | 反弹 | 动态性 | 状态 |
|---|---|---|---|---|---|---|
| 1 | **GI_IBL** | `GI/GI_IBL.cpp` | Split-Sum 近似：辐照度图 + 预滤波图 + BRDF LUT | 环境光 | 天空盒变化时 | ✅ 完整 |
| 2 | **GI_SSGI** | `GI/GI_SSGI.cpp` | 屏幕空间半球采样 + 深度可见性 | 1 次 | 每帧 | ✅（有 bug）|
| 3 | **GI_SSR** | `GI/GI_SSR.cpp` | 屏幕空间线性 Ray March 镜面反射 | 1 次 | 每帧 | ✅ |
| 4 | **GI_RSM** | `GI/GI_RSM.cpp` | 光源 POV 渲染 VPL + 5×5 邻域累积 | 1 次 | 每帧 | ✅ |
| 5 | **GI_DDGI** | `GI/GI_DDGI.cpp` | 探针网格 + SH 辐照度 + 时间混合 | 多帧累积 | 每帧 | ✅ |
| 6 | **RTGIPass** | `RT/RTGIPass.cpp` | 硬件光追余弦半球采样 1 次反弹 | 1 次 | 每帧 | ✅ |
| 7 | **SSAO** | `PostProcess/SSAO.cpp` | 半球采样环境光遮蔽（属 AO 而非 GI，但同属间接光）| — | 每帧 | ✅ |

`GIMode` 枚举中 **VXGI（体素锥追踪）和 ReSTIR GI 仍是占位，无实现类**（`ReSTIRPass` 实现的是直接光照 DI 重采样）。

## 二、各技术现状要点

- **IBL**：辐照度 32²、预滤波 128²×5 mip、BRDF LUT 512²；用光栅化全屏三角形逐面 offscreen pass 生成（共 37 个 pass = 辐照度 6 面 + 预滤波 5 mip×6 面 + BRDF LUT 1），仅在 `m_Dirty` 时重建。⚠️ **BRDF LUT 的 Smith `k` 值存疑**：`IBL_BRDF_LUT.frag.slang:49-50` 是 `a = roughness*roughness; k = a*a*0.5;`，实际展开为 `k = roughness⁴/2`，比 Karis/UE4 IBL 标准的 `k = roughness²/2`（`a=roughness²` 时 `k=a/2`）**多平方了一次**。高粗糙度下几何遮蔽项偏小、间接高光偏亮，建议对照参考实现核查是否为笔误。
- **SSGI**：32 半球采样、全分辨率、无时域累积、无降噪；参数走 UBO（592 字节超 push constant 256 上限）。
- **SSR**：64 步线性 march、`stepSize=0.5`、全分辨率、无 Hi-Z、无时域重投影。
- **RSM**：512² 光源 POV 双 MRT，消费端每像素 5×5=25 个 VPL 采样，硬编码 `*0.03`。
- **DDGI**：8×4×8=256 探针、32 Fibonacci 球面采样、SH band 0/1/2、`blendAlpha=0.85`、前帧 HDR 反馈；三线性插值消费。
- **RTGI**：**每轴 /4（= 总 1/16 像素）**分辨率、SPP 默认 **1**（shader 端 clamp 1–8、cpp 端 clamp 1–16，`RTGIPass.cpp:154`/`RT_GI.rgen.slang:88`）、miss 回退 DDGI 探针、CVar 热更新。仅在 `HybridRTPipeline` 路径激活（`DeferredPipeline` 里 RT 纹理指针恒 `nullptr`）。
- **SSAO**：64 采样核 + 4² 噪声旋转 + blur。

---

## 三、优化建议（按优先级）

### 🔴 第一优先级：正确性 Bug

**1. SSGI 采样点投影用了错误的矩阵** — `SSGI.frag.slang:34`

```hlsl
float4 off = mul(u_Proj, float4(sPos, 1.0)); off.xyz /= off.w;  // ❌ 用逆投影矩阵投影 view-space 点
```

`u_Proj` 在 `GI_SSGI.cpp:93` 被赋值为 `glm::inverse(perspective...)`（**逆**投影，clip→view）。第 22–23 行用它重建 view 位置是对的，但第 34 行把 view-space 采样点 `sPos` 再乘**逆**投影矩阵去求屏幕 UV 是数学错误的——应该乘**正向**投影矩阵。这导致可见性检测采样的屏幕位置 `suv` 是错的，SSGI 大概率处于"半残"状态。需把正向投影矩阵一并传入 UBO（参考 SSAO 同时传 `u_InvProj` + `u_Proj` 两个矩阵的正确做法，见 `SSAO.cpp:198-205`）。

**2. DDGI 网格参数在 C++ 与 Shader 里硬编码重复，且不同步** — `RT_DDGI.slang:5-11`

```hlsl
// TODO: 改为 Uniform Buffer，当前与 GI_DDGI 默认值保持同步
static const uint kDDGI_GridX = 8; ... static const float3 kDDGI_Origin = float3(-10,-2,-10);
```

而 `GI_DDGI.h:47` 的 `gridX/gridY/gridZ/gridOrigin/cellSize` 是可变的运行时字段。一旦 C++ 侧改网格，`SampleDDGI` 的三线性插值索引和钳位边界就全部错位（越界读取、辐照度错位），而 `DDGI.comp` 里又用 UBO 传的 `u_GridSize`。**三处参数源不一致是隐患**，应统一改为 UBO/SSBO 传递，`RT_DDGI.slang` 不再自持常量。

**3. RSM 无条件索引 `u_ShadowData[0]` 且守卫字段选错** — `DeferredLighting.frag.slang:245`

```hlsl
if (iblIntensity > 0.0 && u_ShadowData[0].shadowParams.z > 0.0) {
```

`u_ShadowData` 是 `StructuredBuffer<GPUShadowData>`，C++ 侧恒按 `MAX_SHADOWS`（>0，`DeferredPipeline.cpp:67`）分配，故 `[0]` **始终在界内，并非内存越界**。真正的缺陷是**读脏数据 + 守卫字段选错（逻辑 UB）**：

- `lightCount==0` 时 `ShadowSystem` 不写入任何元素（`ShadowSystem.cpp` 仅 `m_ActiveCount>0` 才写），`u_ShadowData[0]` 保留上一帧残留/未初始化内容，`shadowParams.z` 为脏值，RSM 分支可能被误触发；
- 首个 shadow 非方向光（Spot/Point/Rect）时，`shadowParams.z` 对所有光源类型都是 shadowStrength、恒 `>0`，导致用 spot/point 的 VP 矩阵去采样为方向光 RSM 渲染的 Position/Flux 图 → **结果错位**。

**正确修复**：不能只加 `lightCount > 0`，还应校验光源类型字段 `shadowParams.w`（区分方向光/CSM，参考同文件 210-211 行的用法），确保只有存在方向光 shadow 时才走 RSM 分支。

### 🟠 第二优先级：性能

**4. 光栅化 GI 全部跑在全分辨率，`GISettings.halfRes` 从未接线**

`GlobalIllumination.h:43` 定义了 `halfRes`，但 SSGI/SSR/RSM/DDGI/SSAO 的 `Render` 都用 `m_Width/m_Height`（全分辨率）。GI 是低频信号，SSGI/SSAO/DDGI 探针采样、SSR 均可在半分辨率计算后 bilinear 上采样，性能可省约 3/4 像素着色开销。RT 路径已各自支持 `halfRes/quarterRes`，光栅化侧反而没有——这是性价比最高的一处改动。

**5. DDGI 探针用全分辨率 HDR 采样低频辐照度** — `DDGI.comp.slang:147`

```hlsl
float3 radiance = u_PrevHDR.SampleLevel(u_LinearSampler, uv, 0).rgb;
```

256 探针 × 32 方向 = 8192 次全分辨率 HDR 纹理采样。DDGI 是极低频信息，`CaptureHDR` 里（`GI_DDGI.cpp:229`）应额外维护一张 1/4 分辨率的 HDR 副本供探针采样，带宽立减。

**6. SSR 用 64 步线性 march，无 Hi-Z 层级追踪** — `GI_SSR.cpp:59` + `SSR.frag.slang`

改 Hi-Z（深度 mip 链）层级步进可将步数从 64 降到 ~log2 级别，且命中后用二分细化，质量反而更高。是 SSR 标准优化。

**7. RSM 每像素 25 次 VPL 采样** — `DeferredLighting.frag.slang:249-251`

5×5 全分辨率邻域累积，无重要性采样。可降到 4×4 或用固定 Poisson 盘 16 点 + 随机旋转，配合半分辨率，代价可忽略。

**8. DeferredLighting 一次采样 7 张 GBuffer MRT + 阴影/IBL/RSM/SSGI/SSR/DDGI/AO 全套**

`DeferredLighting.frag.slang:82-94` 每像素采样 GBuffer A/B/C/E/F/G 七张，加阴影 3 张 + IBL 3 张 + RSM 2 张 + SSGI/SSR/AO/DDGI。其中 `u_GBufferE`（worldPos）和 `u_Depth` 在阴影、SSGI、clustered 中重复读取。可考虑合并 GBuffer 通道（如把 metallic/roughness 打包进一个 R8G8B8A8），并让 SSGI/SSR 输出降采样纹理减少后续采样带宽。

**9. 两处 push constant 采样核可下沉为静态常量**

SSGI 的采样核在 CPU 每帧 `Map/Unmap` 重传（`GI_SSGI.cpp:88-95`），实际核是固定种子生成的常量，只需初始化时传一次（与 SSAO 的 kernel 一样是一次性的）。

### 🟡 第三优先级：质量与架构

**10. 多种 GI 同时叠加 + 魔法系数，能量不守恒**

`DeferredLighting.frag.slang` 里 IBL + RSM + SSGI/RTGI + DDGI + SSR **全部累加**，靠 `*0.5`（DDGI，line 275）、`*0.03`（RSM，line 260）这些硬编码系数压亮度。没有"一份漫反射 GI + 一份镜面 GI + 一个预算"的统一策略。建议：按质量档位显式选型（例如 IBL 打底 + DDGI 或 RTGI 二选一作为漫反射，SSR 或 RT 反射二选一作为镜面），用 `GISettings.enabled/intensity` 统一控制，去掉散落的魔法系数。

**11. RTGI 声称"时域累积"但实际没有历史缓冲**

`RT_GI.rgen.slang:93` 传了 `frameIndex` 做随机抖动，但**没有 history buffer、没有重投影、没有累积/降噪**（RT 目录里只有 ReSTIR/PT 有时域处理）。默认 SPP=1（`RT_GI.rgen.slang:115` 的 `radiance /= n` 只是当帧多 SPP 平均，非跨帧累积）直接输出，噪点严重。要么接一个真正的时域累积 + 降噪 pass，要么靠 DDGI 兜底时把 SPP 压到 1，避免浪费。

**12. DDGI 与 RTGI 双重计入（当前潜在，RT 路径接线后触发）**

`RT_GI.rgen.slang:108` miss 时每条射线回退 `SampleDDGI`，而 `DeferredLighting.frag.slang:275` 又无条件 `color += ddgi * 0.5`。当 RTGI 作为漫反射源时 DDGI 被算了两次。

⚠️ **注意触发条件**：此缺陷目前**尚未在运行路径激活**——`DeferredPipeline_FrameGraph.cpp:418` 传给 Lighting 的 4 个 RT 指针恒为 `nullptr`（注释"RT 纹理暂未使用"），`rtDiffuseSource` 恒为 0，RTGI 分支不生效。它只在 **`HybridRTPipeline` 路径**（RT 纹理真正接线）下暴露。修复方向不变：在启用 RTGI 时用 push constant 关掉 DDGI 的直接叠加，或把 miss 回退改为纯天空色。

**13. DDGI 探针"屏幕空间采样"导致屏幕外探针永不更新**

`DDGI.comp.slang:133` 采样点投影到屏幕外就 `continue`，探针 SH 只靠历史混合维持。转身后 GI 是过期的（时滞/鬼影）。若要真正确保动态，需改用探针射线 march（RT 路径下）或至少用上一帧 HDR 的宽范围采样 + 更低的 `blendAlpha` 兜底。这是当前 DDGI 方案的根本局限，值得在文档里明确。

**14. DDGI SH 投影缺余弦加权 + 评估端 `max(result,0)` 破坏重建**

`DDGI.comp.slang:155-159` 直接投影裸 radiance（无 `cos` 权重），`RT_DDGI.slang:33` 评估端又 `max(result,0)` 逐通道截断。二者都会让辐照度偏暗/畸变。标准 DDGI 应在投影时乘 `cos`（或选辐照度 SH），评估端不做截断（负值来自 band-2 振铃，应靠提高 SH 阶数或 blum 滤波缓解）。

**15. IBL 生成是光栅化全屏三角形，可用 Compute 一步替代**

37 个 offscreen pass 属一次性开销，问题不大；但若未来做**运行时动态天空盒**（日夜切换），这 37 pass 会每帧跑。届时建议换 Compute Shader（辐照度/预滤波各一个 dispatch，共享内存分块）。

---

## 四、建议的落地顺序

1. 先修 3 个正确性 bug（SSGI 投影矩阵、DDGI 参数同步、RSM 越界）——低成本、消除隐性错误。
2. 接上半分辨率管线（`halfRes` 落地到 SSGI/SSR/RSM/DDGI/SSAO）——最大性能杠杆。
3. SSR 上 Hi-Z + DDGI 用降采样 HDR——次大的带宽收益。
4. 统一 GI 叠加策略，去掉魔法系数，解决 DDGI/RTGI 双重计入。
5. 视需求再补 RTGI 时域累积/降噪与 DDGI 探针射线 march。

---

## 五、工业界 GI 缺口对比（尚未实现的技术）

> 说明：上文聚焦"现有实现的修正与优化"。本章补充一个更广的视角——对照工业界主流 GI 技术，列出 HugEngine 尚未实现的部分，作为路线图参考。
> 注意：除 `Engine/Render/GI/` 光栅化子系统外，工程另有 `PathTracingPipeline`（全路径追踪，maxBounces=4）+ `ReSTIRPass`（ReSTIR DI）+ `RTDenoiser`（时域降噪）+ `PTAtrousPass`（A-Trous）+ STBN 蓝噪声，因此"多反弹 GI 与降噪栈"已有雏形，不作为缺口。

### 5.1 工业界有、HugEngine 完全空白

#### 预计算 / 烘焙类（离线）
| 技术 | 说明 | 代表 |
|---|---|---|
| Lightmap 光照贴图烘焙 | 离线烘间接光进纹理，运行时零成本 | UE Lightmass、Unity、Bakery |
| 静态 Light / Reflection Probe | 手工放置探针 + 烘焙 SH/cubemap | Unity Light Probe、UE Reflection Capture |
| PRT 预计算辐射传输 | 静态几何 + 动态光照 | 老牌技术 |

#### 体素类实时 GI
| 技术 | 说明 | 代表 |
|---|---|---|
| VXGI 体素锥追踪 | 3D Clipmap 体素化 + 锥追踪 | NVIDIA VXGI（`GIMode::VXGI` 占位）|
| LPV 光传播体积 | RSM 注入 SH + 3D 网格迭代传播 | CryEngine |
| SVOGI 稀疏体素八叉树 | VXGI 稀疏变体 | UE4 |

#### 光追 GI 进阶
| 技术 | 说明 | 差距 |
|---|---|---|
| ReSTIR GI | 时空重采样用于间接光 | 现有 `ReSTIRPass` 是 DI，`GIMode::ReSTIR` 占位 |
| RTXGI | DDGI 商业版：relocation / classification / 可见性 / infinite scroll | 现有 DDGI 缺进阶特性 |
| 多反弹 RT GI | 现有 RTGI 仅 1 次反弹 | PT 管线 4 反弹属离线参考模式 |
| Photon Mapping / PPM | 渐进光子映射 | 经典离线技术 |

#### 全动态混合标杆
| 技术 | 说明 | 状态 |
|---|---|---|
| Lumen（UE5） | Surface Cache + 屏幕追踪 + HW/软光追 + Final Gather | 已有设计规范文档，未实现 |

#### 神经 / 学习类（前沿）
| 技术 | 说明 | 代表 |
|---|---|---|
| Neural Radiance Cache (NRC) | 神经网络缓存/预测低采样 GI | NVIDIA 2023 |
| 神经降噪 | transformer 降噪 | DLSS Ray Reconstruction、NRD ReBLUR |
| RTX Neural Shading / Materials | 神经材质/着色压缩 | NVIDIA 2025（实验性）|

### 5.2 已有雏形、缺进阶升级

| 现有 | 升级方向 | 对标 |
|---|---|---|
| DDGI | 探针 relocation / 可见性 / 无限滚动体积 | → RTXGI |
| RTGI（1 bounce）| 时空重采样 + 多反弹 | → ReSTIR GI |
| SSAO | 地平线 AO、方向遮蔽 | → HBAO / GTAO / SSDO |
| SSR | Hi-Z 层级追踪 + 时域重投影 | → UE / 寒霜 SSR |
| 降噪栈（RTDenoiser + A-Trous）| SVGF / NRD | 实时光追 GI 配套 |

### 5.3 落地建议（按性价比）

1. **ReSTIR GI**：工程已有 ReSTIR DI 完整基础设施（蓄水池、时域/空间复用、STBN），推广到间接光即实现 `GIMode::ReSTIR`，性价比最高。
2. **DDGI → RTXGI 进阶**：修探针缺陷（可见性、屏幕外更新、relocation），不动现有架构。
3. **SSAO → GTAO**：成本低、画面提升明显（UE 默认）。
4. **Lightmap 烘焙**：仅进军移动端/主机静态场景时需要，PC 实时路线可缓。
5. **体素类（VXGI/LPV/SVOGI）与神经类（NRC 等）**：作为长期/研究方向，不在近期工程路线内。

---

# 第二部分 · 架构优化与选型方案

> 本文聚焦：在算法已经足够多的前提下，如何通过**架构层改造**，让多种 GI 算法可以被方便地选取/组合进渲染管线。
> 现状盘点、正确性缺陷与工业界缺口见**第一部分**。

---

## 一、当前架构的根本问题

不是"算法不够"，而是**"选型"散落在各处，没有统一编排层**：

- `DeferredLighting.frag.slang` 把所有 GI **硬编码累加**，靠 `*0.5`、`*0.03` 压亮度；
- 选型靠零散的 push constant（`rtDiffuseSource` / `rtSpecularSource` / `rtAOSource`），每加一种算法就要改 shader + 改 `LightingPass` 绑定；
- 光栅化 GI（`Engine/Render/GI/`）、Hybrid RT（`RTGIPass`）、PT 三条路径**各自为政**，没有一个"挑算法"的统一入口；
- `GISettings.halfRes`、`maxBounces` 定义了但没接线，`GIMode` 枚举里 `VXGI/ReSTIR` 是空占位。

## 二、核心设计原则

1. **按"光照通道"分，而不是按"算法"分** —— 用户选的是"漫反射用谁、镜面用谁"，不是"我要开 DDGI"。
2. **Provider 输出统一契约** —— LightingPass 只消费通道结果，不关心是谁算出来的。
3. **选型 = 数据，不是代码** —— 组合方式放进一个配置结构，CVar / ImGui / 质量档位驱动。
4. **统一能量预算** —— 每个通道一个 `intensity`，消灭散落的魔法系数。
5. **帧图自动编排** —— 每种 GI 是 RenderGraph 的可选节点，配置决定哪些启用。

## 三、目标架构：三层模型

> ⚠️ **修订提示**：本章的"4 个正交通道"模型经代码核对后发现 3 处建模缺陷（见第八章），推荐读者结合第八章的**改良模型**理解本章。本章保留原始设计以呈现演进脉络。

```
┌─────────────────────────────────────────────────────┐
│  第 1 层  GI 通道（GILayer）—— 4 个正交语义槽        │
│  Ambient  │  Diffuse  │  Specular  │  AO             │
├─────────────────────────────────────────────────────┤
│  第 2 层  Provider 实现（统一接口）                    │
│  IBL  SSGI  RSM  DDGI  RTGI  ReSTIRGI  SSR  SSAO ... │
├─────────────────────────────────────────────────────┤
│  第 3 层  组合配置（GIConfig / 质量档位预设）          │
│  Low / Medium / High / Ultra + 运行时 CVar 覆盖       │
└─────────────────────────────────────────────────────┘
```

### 第 1 层：通道抽象

把 GI 拆成 4 个正交通道，每个通道独立选 provider：

```cpp
// 通道语义：对应 LightingPass 里"间接光"的几个正交加项
enum class GILayer : u8 {
    Ambient,     // 无限远环境光（IBL 天空盒）
    Diffuse,     // 间接漫反射
    Specular,    // 间接镜面反射
    AO,          // 环境光遮蔽
};
```

### 第 2 层：统一 Provider 接口（在现有 `IGlobalIllumination` 上收窄）

现有接口已经不错，只需把"输出"收敛成一个标准契约，并声明自己属于哪个通道：

```cpp
// 现有 IGlobalIllumination 已具备 GetMode/GetQuality/GetIndirect*Texture
// 需要补的只是两点：声明通道归属 + 统一强度
class IGlobalIllumination : public IRenderSubsystem {
public:
    [[nodiscard]] virtual GILayer GetLayer() const = 0;      // 本算法属于哪个通道
    [[nodiscard]] virtual float   GetIntensity() const;      // 由 GIConfig 统一注入
    // 输出：统一为"通道纹理"（diffuse/specular/ao），空表示该通道由别人填充
    [[nodiscard]] virtual rhi::IRHITexture* GetChannelTexture() const;
};
```

> `GI_SSR` 现在的 `GetMode()` 复用 `SSGI` 这种 hack 也可以借机理顺：SSR 归 `Specular` 通道，SSGI 归 `Diffuse` 通道。

### 第 3 层：数据驱动的组合配置

这是"方便选取"的核心。选型变成填一个 struct：

```cpp
// 每个通道的 provider 选择（多选一 + 可降级）
struct GILayerConfig {
    GIMode provider  = GIMode::None;   // 主 provider
    GIMode fallback  = GIMode::None;   // 主 provider 不可用（屏幕外/未初始化）时的降级
    float  intensity = 1.0f;           // 通道统一强度（归一化预算）
    bool   enabled   = true;
};

// 全局 GI 配置 = 4 通道 + 质量档位 + 全局预算
struct GIConfig {
    GILayerConfig layers[4];           // Ambient/Diffuse/Specular/AO
    GIQuality     quality = GIQuality::Medium;
    float         globalBudget = 1.0f; // 整帧 GI 能量预算（超了按比例压）
};
```

**质量档位 = 预设表**（一组 `GIConfig` 静态默认值 + 每档算法参数）：

| 档位 | Ambient | Diffuse | Specular | AO |
|---|---|---|---|---|
| Low | IBL | SSGI(半分辨率) | 关 | SSAO |
| Medium | IBL | DDGI | SSR(Hi-Z) | SSAO |
| High | IBL | RTGI + DDGI 兜底 | RT 反射 | GTAO |
| Ultra | IBL | ReSTIR GI | RT 反射 | RT AO |

运行时切换只改一个 `GIConfig`，ImGui 面板直接暴露 4 个下拉框即可，不必碰代码。

## 四、Shader 消费端收敛

`DeferredLighting.frag` 不再写死每个算法，只读 4 个通道输入：

```hlsl
// 之前：硬编码累加 7 种算法 + 魔法系数
// 之后：每个通道一个统一输入，provider 已由 CPU 侧配置选好并填充
float3 color = 直接光照;
color += u_GI_Ambient;                     // provider 已填充（IBL）
color += u_GI_Diffuse;                     // SSGI/DDGI/RTGI/ReSTIRGI 之一已填充
color += u_GI_Specular;                    // SSR/RT反射/IBL prefilter 之一
color *= u_GI_AO;                          // SSAO/GTAO/RT AO 之一
```

`rtDiffuseSource/rtSpecularSource/rtAOSource` 三个零散开关，统一收敛成"通道绑定"——由 CPU 侧 `GIConfig` 决定把哪个 provider 的纹理绑定到 `u_GI_Diffuse` 等 slot，shader 里不再有分支判断。

## 五、帧图自动编排

工程已有 RenderGraph。把每种 provider 的 pass 注册成**可选节点**，`GIConfig` 决定 enable 谁：

```
DeferredFrameGraph::BuildGI(cmd, giConfig):
    if giConfig.layers[Diffuse].provider == DDGI:   add(DDGIProbeUpdate)
    if giConfig.layers[Diffuse].provider == SSGI:   add(SSGIPass)
    if giConfig.layers[Specular].provider == SSR:   add(SSRPass)
    ...
```

选型自动反映到"跑哪些 pass"，未选中的 pass 连资源都不分配（省显存/带宽）。

## 六、结合现有代码的演进路径（不推倒重来）

现有 `IGlobalIllumination` / `GIMode` / `GISettings` / `GIMode::None`（Null Object）已经是对的骨架，只需增量改造：

1. **Phase 1 — 收窄接口**：给 `IGlobalIllumination` 加 `GetLayer()` + `GetIntensity()`，把 SSR 的 mode hack 理顺。改动集中在 `GI/` 目录，风险低。
2. **Phase 2 — 收敛消费端**：`DeferredLighting.frag` 改成 4 通道采样，`LightingPass.cpp` 按 `GIConfig` 绑定通道纹理，替换掉 `rt*Source` 三开关。**这一步就解决了"双重计入"和"魔法系数"两个老问题**。
3. **Phase 3 — 数据驱动**：引入 `GIConfig` + 质量档位预设表，ImGui 暴露 4 通道下拉框 + 档位切换。
4. **Phase 4 — 帧图编排**：把各 provider pass 改为 RenderGraph 可选节点，按配置启用。
5. **Phase 5（可选）— 分层混合**：同通道支持"主 provider + 距离/屏幕外降级"（如 RTGI 中距离 + DDGI 远距离），用 `fallback` 字段表达。

## 七、总结

把"选 GI"从"改 shader 和管线代码"变成"填一个 `GIConfig` 配置"：

- 用 **4 个正交通道**（Ambient / Diffuse / Specular / AO）承载任意算法组合；
- **质量档位预设**绑定默认组合；
- **帧图自动编排**让选型反映到实际执行的 pass；
- 加一种新算法（比如 ReSTIR GI）只需三步：实现 provider、注册到 `GIMode`、加一行档位预设，其余自动生效。

---

## 八、方案修订：从"4 正交通道"到"2 加性间接项 + 单一装配点"

> 结论先行：前七章的**方向正确**（数据驱动选型、Provider 统一接口、帧图条件编排应保留），但核心抽象"4 个正交通道"经代码核对存在 3 处建模缺陷，且**没有击中真正的病根**。本章给出改良模型，不推翻增量精神。

### 8.1 原模型的 3 处建模缺陷（代码佐证）

#### 缺陷 1：4 个通道并不正交，"Ambient=IBL"是错误建模。
`DeferredLighting.frag.slang` 里 IBL **同时**贡献两项：
- `:239` `color += kD * u_IrradianceMap... * albedo`（间接**漫反射**）
- `:242` `color += prefiltered * (F*envBRDF.r + envBRDF.g)`（间接**镜面**）

而第三章预设表把 IBL 只放进 `Ambient` 一列。实际上 IBL **不是一个通道**，而是 Diffuse 和 Specular 两项的**默认 provider / 兜底**（`:280` 注释自陈"specular IBL prefilter 已提供回退"）。同理 **AO 不是加性通道**，它是对间接项的**乘法调制**——把相加与相乘两种运算并列为"正交通道"是概念混淆。

#### 缺陷 2：`GetChannelTexture()` 单纹理契约对非屏幕空间技术不成立。
IBL 输出 cubemap（irradiance + prefilter），DDGI 输出探针 SSBO（`SampleDDGI` 用 worldPos+normal 采样），只有 SSGI/SSR/SSAO/RTGI 是屏幕空间纹理。**恰恰是非屏幕空间的 IBL/DDGI 不适配"统一纹理"契约**——第二章 `LightingInputs` 已不得不把 `ddgiProbeBuffer` 单列，即是此漏抽象的证据。

#### 缺陷 3（最关键）：真正的病根是"光照方程的装配顺序随意"，而非"选谁"。
`DeferredLighting.frag.slang:266` 的 AO：
```hlsl
color *= ao * aoVal;   // 位于 IBL/RSM 之后、SSGI/DDGI/SSR 之前
```
当前累加顺序为：直接光 → `*rtShadow` → +IBL(diffuse+spec) → +RSM → **`*AO`** → +SSGI/RTGI → +DDGI → +SSR → +emissive。结果是 **AO 乘到了直接光上（物理错误，AO 只该衰减间接光），又完全漏掉了 SSGI/DDGI/SSR 三个间接项**。这是真实的正确性 bug，而"4 slot 通道"模型**并不能防止它**——它只管"哪个 provider 填哪个 slot"，不管"这些 slot 按什么方程、什么顺序装配"。

> **原模型抽象了"选型"，却没有抽象"装配（composite）"——而装配才是双重计入、魔法系数、AO 错位这些问题的共同来源。**

### 8.2 改良模型

**目标架构（数据流）**：

```mermaid
flowchart TD
  Config["GIConfig 数据<br/>质量档位 + 每项 intensity"]
  Sel{"通道选型<br/>Diffuse / Specular / AO"}
  Config -->|"ToInputSources()"| Sel

  subgraph PROV["Providers（声明 GetSupplies + GetConsumeMode，provider-&gt;Build(rg) 自注册 pass）"]
    direction LR
    IBL["IBL<br/>供 Diffuse + Specular<br/>Cubemap（默认 / 兜底）"]
    DIFF["SSGI / DDGI / RTGI / ReSTIR GI<br/>供 Diffuse<br/>屏幕纹理 / 探针 SSBO"]
    SPEC["SSR / RT 反射<br/>供 Specular<br/>屏幕纹理"]
    AOP["SSAO / GTAO / RT AO<br/>供 AO<br/>屏幕纹理"]
  end
  Sel --> PROV

  subgraph ASM["AssembleIndirect() — 单一装配点（Deferred / HybridRT / PT 三管线复用）"]
    direction TB
    A1["out = direct × shadow"]
    A2["indDiffuse&nbsp;&nbsp;= PickDiffuse() × diffuseIntensity<br/>indSpecular = PickSpecular() × specularIntensity<br/>（主 provider + 可选低频补光，内部完成，不双重计入）"]
    A3["out += (indDiffuse + indSpecular) × AO<br/>⚠️ AO 只调制间接项，绝不乘直接光"]
    A4["out += emissive"]
    A1 --> A2 --> A3 --> A4
  end
  PROV --> ASM

  ASM --> Out["最终颜色"]
```

> 三条纵向要点：**选型是数据**（`GIConfig` 驱动，不改代码）→ **Provider 声明能力而非固定通道**（IBL 一个 provider 同时供两项）→ **装配在单一 `AssembleIndirect()` 收口**（方程只此一处定义，结构性杜绝双重计入 / 魔法系数 / AO 错位）。

**① 通道模型：2 个加性间接项 + 1 个只调制间接的 AO**

```
IndirectDiffuse    加性；单一主 provider + 可选低频补光（如 DDGI）
IndirectSpecular   加性；单一 provider
AO                 乘法调制，只作用于上面两项，绝不乘直接光
```
- **去掉虚构的 Ambient 通道**；IBL 建模成"Diffuse 与 Specular 两项的默认 provider / fallback"，自然承担二者兜底 → 修正缺陷 1 与预设表错位。
- AO 明确为"间接项调制器" → 修正 `:266` 的 AO 错位。

**② Provider 声明"能力位掩码 + 消费方式"，而非单一 channel**

```cpp
[[nodiscard]] virtual GITermMask   GetSupplies()    const = 0; // Diffuse | Specular（IBL 返回两者）
[[nodiscard]] virtual GIConsumeMode GetConsumeMode() const = 0; // ScreenTexture | ProbeBuffer | Cubemap
```
装配端按消费方式绑定正确的资源类型 → 修正缺陷 2。UI 也能据能力位**只显示合法组合**（避免"SSAO 填进 Diffuse"这类无效项）。

**③ 单一 `AssembleIndirect()` —— 一处定义方程，三条管线复用（本方案的核心）**

把光照方程装配收敛成一个函数，取代散在 `DeferredLighting.frag` 里逐行的 `color +=`：
```
outColor  = directLight * shadow;
float3 indDiffuse  = PickDiffuse()  * diffuseIntensity;   // provider 内部已处理"主+兜底"
float3 indSpecular = PickSpecular() * specularIntensity;
outColor += (indDiffuse + indSpecular) * ao;              // AO 只调制间接项
outColor += emissive;
```
- **结构性消灭双重计入**：RTGI 在 miss 时回退 DDGI 属 `PickDiffuse()` 内部实现细节，装配端只拿到"一份 diffuse"，不可能再加第二次。
- **消灭魔法系数**：`*0.03`/`*0.5` 全变成 provider 归一化输出 + 通道 intensity。
- **三条管线（Deferred / HybridRT / PT）共用同一装配函数**，不再各写一遍——这是原方案未提及、但真正解耦的关键。

**④ 用 `CompositePolicy` 表达空间混合，取代单一 `fallback` 枚举**

原第五章 Phase 5 想要"RTGI 中距离 + DDGI 远距离"，但数据模型只有一个 `fallback` 枚举，**只能表达"A 不可用则用 B"（可用性降级），表达不了"按距离/置信度逐像素混合"**。真实 Lumen 式混合需要：
```cpp
enum class CompositePolicy : u8 { Add, DistanceBlend };
struct BlendParams { float nearDist, farDist, transition; };
```
这是唯一值得比原方案"多花一点"之处。

**⑤ 去掉 `globalBudget`**

"所有通道强度之和超 1 就按比例压"（第三章 `GIConfig::globalBudget`）**不是能量守恒**，只是全局亮度旋钮，且会误导使用者以为它保证了物理正确。真正守能量靠"每项单一来源（不重复计）+ 正确 kD/kS 拆分"（`:238` 已有 `kD=(1-F)(1-metallic)`）。删除此假承诺。

**⑥ 帧图：provider 自注册 `Build(rg)`，而非管线里 if/else 枚举**

原第五章 Phase 4 仍是管线文件里 `if (cfg.diffuse==DDGI) add(...)` 逐个枚举。更彻底、也更贴合"加算法只改三处"的目标：
```cpp
for (auto* p : enabledProviders) p->Build(rg, giResources); // 新增技术完全不碰管线文件
```

### 8.3 altitude 判断与采纳优先级

对 HugEngine 这种 **GI 对比实验室 / 学习型引擎**，原方案是合理的低风险增量，**不需要**做成 Lumen 那种"单一 GI 系统内部按距离自选技术"的黑盒——那会牺牲本引擎"并排对比各算法"的核心价值。故建议**不推翻、只修正建模**，按性价比采纳：

| 优先级 | 改动 | 解决的问题 |
|---|---|---|
| 🔴 最高 | 单一 `AssembleIndirect()` 装配点（②③） | 结构性根治双重计入 + AO 错位 + 魔法系数 + 三管线重复 |
| 🔴 高 | IBL 建模为"双项 provider/fallback"，去掉 Ambient 伪通道（①） | 修正预设表与光照方程对不上 |
| 🟠 中 | 能力位掩码 + 消费方式（②）、删 `globalBudget`（⑤） | 补漏抽象、去假承诺 |
| 🟡 按需 | `CompositePolicy` 空间混合（④）、provider 自注册（⑥） | 为 Lumen 式 hybrid 留路 |

> **更激进的替代范式（备选）**：不分固定通道，用单一 `IndirectComposer` + 贡献列表 `{term, resource, sampleMode, weight}`，composer 按 term 求和。比 4 个固定 slot 更灵活、代码更少，能自然容纳体积 GI / 透射 GI 等第 5、6 类；代价是"菜单式选型"的直观性略降。追求扩展性可选此范式；当前"4 下拉框"易用性目标下，上面的改良模型更合适。

---

# 第三部分 · 详细落地方案

> 本部分是前述架构方案的**工程落地细化**：精确到文件、接口签名、结构体、shader 改动与验收标准，可直接据此编码。
> 现状基线见**第一部分**，架构设计见**第二部分**。

---

## 0. 现状基线（方案的前提）

通过代码核对，当前有 4 个关键事实决定方案走向：

| # | 事实 | 位置 |
|---|---|---|
| F1 | **`LightingSource` 枚举 + `LightingInputSources` 结构体已定义但未接线**——`Render` 仍用 33 个位置参数（含 cmd）+ 裸指针推断 | `LightingPass.h:19-45`、`LightingPass.cpp:55-76`（签名）、`167-170`（推断） |
| F2 | **GI 是分散成员**：`m_GI`(IBL, `unique_ptr<IGlobalIllumination>`) + `m_RSM` + `m_SSGI`/`m_SSR`/`m_DDGI`(值成员)，`GetGI()` 只返回 IBL，无统一注册表 | `DeferredPipeline.h:141-167` |
| F3 | **帧图硬编码顺序**：DDGI → SSAO → SSR → DenoiseSSR → SSGI → DenoiseSSGI → IBL → Lighting → DDGI_CaptureHDR，无选型分支 | `DeferredPipeline_FrameGraph.cpp:262-446` |
| F4 | **shader 端魔法系数叠加**：IBL + RSM(`*0.03`) + SSGI/RTGI + DDGI(`*0.5`) + SSR 全部累加；DDGI 双重计入为**潜在缺陷**（当前 `DeferredPipeline` 里 RT 指针恒 `nullptr`、`rtDiffuseSource` 恒 0，仅 `HybridRTPipeline` 接线后触发） | `DeferredLighting.frag.slang:235-281`、`DeferredPipeline_FrameGraph.cpp:418` |

结论：**骨架已经存在（F1），问题在"接线"**。本方案基于现有骨架增量改造，不重写管线。

---

## 1. 总体路线（依赖图）

```
Phase 1（接口收敛）──► Phase 2（shader 通道化）──► Phase 3（配置驱动）
                                                    │
Phase 4（帧图编排）──────────────────────────────────┘
        │
Phase 5（注册表 + fallback，可独立、最后做）
```

- **Phase 1–2** 是低风险的"接线 + 去魔法系数"，解决 F1/F4，可单独合入。
- **Phase 3–4** 是真正的"方便选取"，落地数据驱动选型与自动编排。
- **Phase 5** 是可选增强（支持主 provider 不可用时的自动降级）。

---

## 2. Phase 1 —— 激活 `LightingInputSources`，收敛 `Render` 签名

### 目标
把 `LightingPass::Render` 的 33 个位置参数打包成结构体，选型信息显式进入 `LightingInputSources`，替换"裸指针推断 rt*Source"。

### 改动文件
- `Engine/Render/Pipeline/LightingPass.h`
- `Engine/Render/Pipeline/LightingPass.cpp`

### 2.1 扩展 `LightingSource` 枚举（补 ambient 通道 + 预留未来算法）

```cpp
// LightingPass.h —— 现状基础上增量扩展
enum class LightingSource : u8 {
    None = 0,
    // 环境光（无限远）—— 新增，把 IBL 纳入统一选型
    Ambient_IBL,
    // 阴影
    Shadow_CSM, Shadow_RT,
    // 环境光遮蔽
    AO_SSAO, AO_RTAO, AO_GTAO,        // GTAO 预留
    // 镜面反射
    Specular_SSR, Specular_RT,
    // 间接漫反射
    Diffuse_SSGI, Diffuse_RTGI, Diffuse_DDGI,
    Diffuse_RSM, Diffuse_ReSTIRGI,     // RSM / ReSTIR GI 预留
};
```

### 2.2 扩展 `LightingInputSources`（补 ambient、强度、fallback）

```cpp
struct LightingInputSources {
    // ── 4 个正交通道的 provider 选择 ──
    LightingSource ambient  = LightingSource::Ambient_IBL;
    LightingSource shadow   = LightingSource::Shadow_CSM;
    LightingSource ao       = LightingSource::AO_SSAO;
    LightingSource specular = LightingSource::Specular_SSR;
    LightingSource diffuse  = LightingSource::Diffuse_SSGI;

    // ── 低频补光 / 兜底（显式化原 useDDGI 语义）──
    // diffuse 主 provider 之外，可选叠加一个低频 provider（如 DDGI）
    // None = 不叠加。这取代旧的 `bool useDDGI`，并解决"DDGI 无条件 *0.5"问题。
    LightingSource diffuseFallback = LightingSource::Diffuse_DDGI;

    // ── 每通道统一强度预算（取代 shader 魔法系数）──
    float ambientIntensity        = 1.0f;
    float diffuseIntensity        = 1.0f;
    float diffuseFallbackIntensity = 0.5f;   // 对应原 *0.5
    float specularIntensity       = 1.0f;
    float aoIntensity             = 1.0f;
};
```

### 2.3 打包输入结构 `LightingInputs`（取代 33 参数）

```cpp
// 实际纹理/缓冲输入（"是什么"），与 LightingInputSources（"选哪个"）解耦
struct LightingInputs {
    // GBuffer（7 张）
    rhi::IRHITexture* gbA=nullptr, *gbB=nullptr, *gbC=nullptr, *gbDepth=nullptr,
                     *gbE=nullptr, *gbF=nullptr, *gbG=nullptr;
    // 阴影（CSM×3 + Spot）
    rhi::IRHITexture* shadowMap0=nullptr, *shadowMap1=nullptr,
                     *shadowMap2=nullptr, *spotShadow=nullptr;
    // 光源 / 阴影数据 SSBO
    rhi::IRHIBuffer* lightBuffer=nullptr, *shadowBuffer=nullptr;
    // 屏幕空间效果（由 provider 填充，空则用占位）
    rhi::IRHITexture* ssaoTex=nullptr, *ssgiTex=nullptr, *ssrTex=nullptr;
    rhi::IRHISampler* ssgiSampler=nullptr, *ssrSampler=nullptr;
    // DDGI 探针缓冲（低频补光用）
    rhi::IRHIBuffer* ddgiProbeBuffer=nullptr;
    // RT 效果纹理（可选）
    rhi::IRHITexture* rtShadowMask=nullptr, *rtReflection=nullptr,
                     *rtAO=nullptr, *rtGI=nullptr;
    // 聚集着色
    ClusteredShading* clusteredShading=nullptr;
    rhi::IRHIBuffer* lightGridBuffer=nullptr, *lightIndexListBuffer=nullptr;
    std::vector<GPULight>* cachedLights=nullptr;
};
```

### 2.4 收敛 `Render` 签名

```cpp
// 之前：Render(cmd, gbA, gbB, ..., 33 个位置参数)
// 之后：
void Render(rhi::IRHICommandList* cmd,
            const LightingInputs& in,
            const LightingInputSources& src,
            const float4& cameraPos, float iblIntensity,
            u32 lightCount, u32 width, u32 height);
```

### 2.5 内部：由 `src` 派生 rt*Source push constant

```cpp
// LightingPass.cpp —— 替换掉 `rtShadowMask ? 1u : 0u` 这种裸指针推断
lpc.rtShadowSource   = (src.shadow   == LightingSource::Shadow_RT)   ? 1u : 0u;
lpc.rtAOSource       = (src.ao       == LightingSource::AO_RTAO)     ? 1u : 0u;
lpc.rtSpecularSource = (src.specular == LightingSource::Specular_RT) ? 1u : 0u;
lpc.rtDiffuseSource  = (src.diffuse  == LightingSource::Diffuse_RTGI)? 1u : 0u;
```

### 验收标准
- [ ] `LightingPass::Render` 参数从 33 个降到 6 个（`in`/`src` 两结构 + 标量）。
- [ ] `DeferredPipeline_FrameGraph.cpp` 与 `HybridRTPipeline.cpp` 调用点编译通过，行为不变（仅重构）。

---

## 3. Phase 2 —— shader 通道化 + 强度预算

### 目标
`DeferredLighting.frag` 去掉散落魔法系数，改为每通道显式强度；DDGI 从"无条件叠加"变成"由 `diffuseFallback` 控制"。

### 改动文件
- `Engine/Shader/Shaders/DeferredLighting.frag.slang`
- `Engine/Shader/Shaders/ShaderTypes.slang`（`DeferredLightingPushConstant` 结构）

### 3.1 push constant 增加每通道强度字段

```hlsl
// ShaderTypes.slang —— DeferredLightingPushConstant 追加
struct DeferredLightingPushConstant {
    // ... 现有字段 ...
    // 新增：每通道统一强度预算
    float4 giIntensity;   // x=ambient, y=diffuse, z=diffuseFallback, w=specular
    float  aoIntensity;   // AO 通道强度
};
```

### 3.2 消费端去魔法系数

```hlsl
// 之前：
//   color += rsmIndirect * 0.03 * iblIntensity;      // 魔法系数
//   color += ddgi * 0.5;                              // 魔法系数

// 之后：
// RSM 作为 diffuse 可选 provider（src.diffuse == Diffuse_RSM 时生效）
if (g_PC.diffuseSource == DIFFUSE_RSM) {
    color += rsmIndirect * g_PC.giIntensity.y;
}
// DDGI 作为 diffuseFallback（默认叠加，但强度可配、可关）
if (g_PC.diffuseFallbackEnabled) {
    color += ddgi * g_PC.giIntensity.z;
}
// SSGI/RTGI 主 diffuse
color += rtGI.rgb * g_PC.giIntensity.y;
// 镜面
color += rtRefl.rgb * g_PC.giIntensity.w;
// AO
color *= ao * aoVal * g_PC.aoIntensity;
```

### 3.3 用枚举常量替代裸 `rt*Source` 判断

把 `rtDiffuseSource`（0/1 二选一）升级为枚举编码的 `diffuseSource`（可表达 SSGI/RTGI/RSM/DDGI/None 多值），shader 里用 `switch` 或 `if` 分支选择，而非布尔。

### 验收标准
- [ ] 魔法系数 `*0.03`、`*0.5` 从 shader 移除，全部由 push constant 强度驱动。
- [ ] `useDDGI` 语义由 `diffuseFallback` + `diffuseFallbackEnabled` 表达，关掉后 DDGI 贡献严格为 0（消除双重计入）。

---

## 4. Phase 3 —— `GIConfig` 数据驱动 + 质量档位 + ImGui

### 目标
选型彻底数据化：一个 `GIConfig` 承载全部组合，质量档位预设绑定默认组合，ImGui 暴露 4 个通道下拉框。

### 改动文件（新增）
- `Engine/Render/GI/GIConfig.h`（新增）
- `Engine/Render/GI/GIConfig.cpp`（新增：质量档位预设表）

### 4.1 定义 `GIConfig`

```cpp
// GIConfig.h —— GI 组合配置（数据驱动选型的核心）
struct GIConfig {
    // 4 通道 provider（直接复用 LightingPass 的 LightingSource，避免双份枚举）
    LightingSource ambient  = LightingSource::Ambient_IBL;
    LightingSource diffuse  = LightingSource::Diffuse_SSGI;
    LightingSource diffuseFallback = LightingSource::Diffuse_DDGI;
    LightingSource specular = LightingSource::Specular_SSR;
    LightingSource ao       = LightingSource::AO_SSAO;

    // 强度预算
    float ambientIntensity  = 1.0f;
    float diffuseIntensity  = 1.0f;
    float diffuseFallbackIntensity = 0.5f;
    float specularIntensity = 1.0f;
    float aoIntensity       = 1.0f;

    // 全局预算（所有通道强度之和超 1 时按比例压）
    float globalBudget = 1.0f;

    GIQuality quality = GIQuality::Medium;

    // 便捷转换：→ LightingInputSources（供 LightingPass 消费）
    LightingInputSources ToInputSources() const;
};
```

### 4.2 质量档位预设表

```cpp
// GIConfig.cpp —— 静态预设，ImGui / CVar 切换只需改一个 GIConfig
static const GIConfig kGIPresets[4] = {
    // Low   —— 移动/入门：IBL + 半分辨率 SSGI + SSAO
    { Ambient_IBL, Diffuse_SSGI, None, None, AO_SSAO,
      1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, GIQuality::Low },
    // Medium —— IBL + DDGI + SSR + SSAO
    { Ambient_IBL, Diffuse_None, Diffuse_DDGI, Specular_SSR, AO_SSAO,
      1.0f, 0.0f, 0.8f, 1.0f, 1.0f, 1.0f, GIQuality::Medium },
    // High   —— IBL + RTGI(主) + DDGI(兜底) + RT 反射 + RT AO
    { Ambient_IBL, Diffuse_RTGI, Diffuse_DDGI, Specular_RT, AO_RTAO,
      1.0f, 1.0f, 0.4f, 1.0f, 1.0f, 1.0f, GIQuality::High },
    // Ultra  —— IBL + ReSTIR GI + RT 反射 + RT AO
    { Ambient_IBL, Diffuse_ReSTIRGI, None, Specular_RT, AO_RTAO,
      1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, GIQuality::Ultra },
};

GIConfig GIConfig::FromQuality(GIQuality q) { return kGIPresets[(int)q]; }
```

### 4.3 管线持有 `GIConfig` 而非零散 bool

`DeferredPipeline` / `HybridRTPipeline` 各持有一个 `GIConfig m_GIConfig`，每帧从 `m_GIConfig.ToInputSources()` 得到选型，再据选型 enable/disable 各 GI 子系统。

### 4.4 ImGui 面板

暴露 4 个通道 `Combo`（枚举下拉）+ 质量档位 `Combo` + 每通道强度 `SliderFloat`。改动即改 `m_GIConfig`，下帧生效。

### 验收标准
- [ ] 切换质量档位只改一个 `GIConfig`，无需重建 PSO。
- [ ] ImGui 可任意组合 4 通道 provider 并实时生效。

---

## 5. Phase 4 —— 帧图自动编排

### 目标
`DeferredPipeline_FrameGraph.cpp` 里硬编码的 GI pass 顺序，改为按 `GIConfig` 条件注册。

### 改动文件
- `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp`

### 5.1 用 `shouldRun` 判定包装每个 GI pass

```cpp
// 之前：无条件依次 add（DDGI → SSAO → SSR → SSGI → ...）
// 之后：按配置决定是否注册节点

const auto& cfg = m_GIConfig;

// DDGI 探针更新：仅当 diffuse 或 diffuseFallback 用到了 DDGI
if (cfg.diffuse == LightingSource::Diffuse_DDGI
    || cfg.diffuseFallback == LightingSource::Diffuse_DDGI) {
    rg.AddPass("DDGI_Update", ...,
        [&](auto c) { m_DDGI.Update(...); m_DDGI.Render(c); });
}

// SSGI：仅当 diffuse == Diffuse_SSGI
if (cfg.diffuse == LightingSource::Diffuse_SSGI) {
    rg.AddPass("SSGI", ..., [&](auto c) { m_SSGI.SetInputs(...); m_SSGI.Render(c); });
    rg.AddPass("SSGI_Denoise", ...);
}

// SSR：仅当 specular == Specular_SSR
if (cfg.specular == LightingSource::Specular_SSR) { ... }

// RSM：仅当 diffuse == Diffuse_RSM
if (cfg.diffuse == LightingSource::Diffuse_RSM) { ... }
```

### 5.2 Lighting 节点消费 `GIConfig`

```cpp
rg.AddPass("Lighting", ..., [&, w, h](auto c) {
    // IBL：ambient 通道为 IBL 时才生成 + 绑定
    if (cfg.ambient == LightingSource::Ambient_IBL && giIBL) {
        if (giIBL->IsDirty()) giIBL->Render(c);
        m_Lighting.SetIBLTextures(...);
    }
    LightingInputs in = BuildInputs();          // 汇总各 provider 输出
    m_Lighting.Render(c, in, cfg.ToInputSources(), ...);
});
```

### 验收标准
- [ ] 未选中的 GI pass 不注册、不分配资源（RenderDoc 里看不到对应 pass）。
- [ ] 切换 provider 后帧图节点数量随之变化。

---

## 6. Phase 5（可选）—— Provider 注册表 + 自动降级

### 目标
支持"主 provider 不可用（屏幕外/未初始化/设备不支持）时自动降级到 fallback"，为分层混合打基础。

### 改动文件
- `Engine/Render/GI/GIRegistry.h`（新增）

### 6.1 注册表 + 工厂

```cpp
// 按 GIMode 注册 provider 工厂，运行时按配置实例化/查询
class GIRegistry {
public:
    using Factory = std::function<std::unique_ptr<IGlobalIllumination>()>;
    void Register(GIMode m, Factory f);
    std::unique_ptr<IGlobalIllumination> Create(GIMode m);
    bool IsAvailable(GIMode m, rhi::IRHIDevice* dev);  // 设备能力查询
};
```

### 6.2 降级链

```cpp
// 每帧解析：主 provider 不可用 → 沿 fallback 链降级
LightingSource ResolveSource(LightingSource primary, const GIRegistry& reg, rhi::IRHIDevice* dev) {
    if (reg.IsAvailable(primary, dev)) return primary;
    return FallbackOf(primary);   // 预定义降级表，如 RTGI→DDGI→SSGI→None
}
```

### 验收标准
- [ ] 不支持的设备上，High 档自动落到 Medium 档的 provider 组合。

---

## 7. 数据流总览（改造前后）

**改造前**：管线代码硬编码"跑哪些 pass + 传哪些纹理"，shader 硬编码"叠加哪些 + 系数多少"。

**改造后**：

```
GIConfig（数据）
   │  ToInputSources()
   ▼
LightingInputSources（4 通道选型 + 强度）
   │
   ├──► 帧图编排：按选型 add/跳过 GI pass（Phase 4）
   │
   └──► LightingPass::Render(in, src)（Phase 1）
            │  push constant：通道强度 + 枚举 source（Phase 2/3）
            ▼
        DeferredLighting.frag：读通道、按枚举选 provider、乘通道强度（无魔法系数）
```

---

## 8. 验收标准汇总（跨 Phase）

| Phase | 验收要点 |
|---|---|
| 1 | `Render` 33 参数 → 6 参数；调用点行为不变 |
| 2 | 魔法系数移除；DDGI 可显式关闭；diffuse 多 provider 枚举化 |
| 3 | `GIConfig` 驱动；质量档位切换只改一份配置；ImGui 实时组合 |
| 4 | 未选中的 GI pass 不注册不分配；帧图节点数随选型变化 |
| 5 | 主 provider 不可用自动降级 |

---

## 9. 建议的合入顺序与风险

1. **Phase 1 + 2 一起合**（重构 + 去魔法系数）：纯内部改动，行为等价，风险低，先打地基。
2. **Phase 3 + 4 一起合**（数据驱动 + 自动编排）：对外可见的行为变化（ImGui 选型），需在 `06.GILab` sample 里验证各档位组合。
3. **Phase 5 独立合**：纯增量，不影响前四项。
