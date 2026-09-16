# HugEngine GI 架构与开发计划

> **本文合并自**：《HugEngine GI分层合成架构设计.md》（架构设计）+《HugEngine GI优化开发计划.md》（开发计划）。
> 原两份文档中的历史里程碑与提交级演进记录已压缩为 **附录 A**；其余历史细节可由 git 历史取回。
>
> **本次更新依据**：对当前 HEAD 的代码实读复核（而非沿用文档声明）。凡文档与代码不一致处，
> 一律以代码为准并在 §9 集中列出。

---

# 第一部 · 架构设计

## 1. 目标与问题背景

让同一场景中的多种 GI 算法**按频段/尺度分工协作**——远场低频用探针（IBL/DDGI/Lightmap）、
中频近处用屏幕空间（SSGI/SSR/SSAO/RSM）、高频精确用光追（RTGI/RT 反射/RTAO），
并以**物理正确的方式合成**（无双重计数）。

### 1.1 改造前的四个根本局限（历史）

| 局限 | 说明 |
|---|---|
| **表达力不足** | 「选一个」无法表达「DDGI 管低频 + SSGI 管中频 + RTGI 管高频」的分工 |
| **合成错误** | 多源相加是**双重计数**（同一物理量算两遍 → 能量翻倍，白炉测试失败） |
| **抽象不统一** | SSGI/DDGI/SSR 各自为管线独立成员，新增 GI 需改管线代码 + shader + 面板 |
| **无「层」概念** | 没有频段、作用范围、置信度这些**决定权重的物理量** |

---

## 2. 数据模型

> 实现位置：`Engine/Render/GI/GITypes.h`（**RHI-free**，只依赖 `Core/Types.h`，可被 `Tests/` 直接包含）
>
> 注：原先的 `GI/GIConfig.h` 与 `GI/GIRegistry.h` 两个转发头已在 P0/D2 一并移除——
> 数据模型只保留 `GITypes.h` 一个头文件，避免「GIConfig 到底在哪」的歧义。

### 2.1 GI 源标识

```cpp
enum class GISourceId : u8 {
    None = 0,
    IBL = 1,            // 低频：环境辐照度 / 预滤波（同时服务 diffuse 与 specular）
    Lightmap = 2,       // 低频：烘焙光照（预留，尚未实现——见 §9）
    DDGI = 3,           // 低频：动态漫反射探针网格
    SSGI = 4,           // 中频：屏幕空间间接漫反射
    SSR = 5,            // 中频：屏幕空间反射
    SSAO = 6,           // 中频：屏幕空间环境光遮蔽
    RSM = 7,            // 中频：反射阴影贴图间接光（Forward 与 Deferred 共用）
    RTGI = 8,           // 高频：硬件光追间接漫反射
    RTReflection = 9,   // 高频：硬件光追反射
    RTAO = 10,          // 高频：硬件光追环境光遮蔽
    GTAO = 11,          // 中频：地平线切片 AO（SSAO 的高质量替代）
};
```

两点设计取向：

- **ReSTIR 不进枚举**。当前 `ReSTIRPass` 实现的是 **ReSTIR DI（直接光照重采样）**，
  不是 GI；它由 `PathTracingPipeline` 独占驱动。等到 P6 统一估计器落地再作为源接入。
- **阴影不进 `GISourceId`**。阴影是**可见性（乘法项）**而非能量（加法项），
  没有频段、没有「距离让位」语义，用独立的 `ShadowChannel{None, Raster, RT}` 表达。

### 2.2 频段

```cpp
enum class GIBand : u8 { Low = 0, Mid = 1, High = 2 };
GIBand GIBandOf(GISourceId id);
```

频段**不写进层描述**，而是由 id 推导——避免「同一算法被填成不同频段」的不一致。

> 注：`GIBandOf` 曾有一处缺陷——`RTGI / RTReflection / RTAO` 与 `default:` 共用同一条
> fallthrough，导致 `GIBand::High` **永不可达**，06.GILab 的面板把光追源显示成「中频」
> （`GIBandName(GIBandOf(id))`，`06.GILab.cpp:1049-1050`）。
> 已在 **P0/D2 修复**为显式 `return GIBand::High`，并在 `Tests/TestGITypes.cpp` 中补了
> 「`High` 可达」的回归断言（详见 §9.1 第 1 行）。

### 2.3 层描述与层栈

```cpp
struct GISourceDesc {
    GISourceId id              = GISourceId::None;
    float      weight          = 1.0f;   // 相对权重；0 = 不参与（等价于旧的 enabled）
    float      falloffDistance = 0.0f;   // 可选「距离让位」；0 = 不启用
};

struct GIChannelStack {
    static constexpr u32 kMaxSources = 4;    // 容量按「同时启用数」而非「算法总数」定
    GISourceDesc sources[kMaxSources];       // 有序：低频 → 高频
    u32          count = 0;
    GIBlendMode  mode  = GIBlendMode::Normalized;
};
```

`falloffDistance` 相对早期设计稿的 `range`（有效作用范围）改了名并改成**可选**——
因为物理正确性来自权重归一化，距离衰减只是性能/艺术控制，**默认关闭**。

### 2.4 顶层配置

```cpp
struct GIConfig {
    GIChannelStack diffuse;    // 间接漫反射（IBL/Lightmap/DDGI/SSGI/RSM/RTGI）
    GIChannelStack specular;   // 间接镜面（IBL/SSR/RTReflection）
    GIChannelStack ao;         // 环境光遮蔽（SSAO/GTAO/RTAO）
    ShadowChannel  shadow = ShadowChannel::Raster;   // 独立枚举，不进层栈
    float giIntensity = 1.0f;
    float aoIntensity = 1.0f;
    bool  rsmIndirect = true;
    bool  halfRes     = false;
    bool  furnaceMode = false;   // 白炉数值测试（§11.3）
};
```

`GIConfig` 是**面板与帧图共用的单一数据源**：帧图的 pass 注册、Lighting 的合成参数、
面板的候选列表全部从它派生。

---

## 3. 合成数学

### 3.1 归一化加权（已实现，当前唯一的生产合成模式）

```hlsl
// DeferredLighting.frag.slang —— 每个通道同构
float3 num = 0; float den = 0;
for (uint i = 0; i < bl.count; ++i) {
    GISourceSlot s = bl.sources[i];
    if (s.weight <= 0.0) continue;
    float w = SourceWeight(s, confEdge, camDist);
    num += SampleDiffuseSource(s.id, worldPos, N, uv, albedo, kD, furnace) * w;
    den += w;
}
float3 indirect = (bl.mode != 0u) ? (num / max(den, 1e-4)) : num;
```

- `mode != 0`（`Normalized`）→ 除以权重和，**权重和 = 1 → 无双重计数**
- `mode == 0`（`Additive`）→ 直接相加，**仅作 A/B 对照保留**
- `GIBlendMode` 只有这两个值；设计稿中的 `Fallback`（分层回退）与
  `FrequencySplit`（频率分离）**均未实现**——前者已确认**放弃**（可由「只用最精确的源」的
  层栈组合表达），后者是 P5 的目标（§10）。

### 3.2 权重来源

实现里只有一条规则（`DeferredLighting.frag.slang` 的 `SourceWeight`）：

```hlsl
float SourceWeight(GISourceSlot s, float confEdge, float camDist) {
    bool screenSpace = (id ∈ {SSGI, SSR, SSAO, RTGI, RT_REFLECTION, RTAO});
    float w = s.weight;
    if (screenSpace) w *= confEdge;                                   // 屏幕内可见性
    if (s.falloffDistance > 0.0) w *= saturate(1.0 - camDist / s.falloffDistance);
    return w;
}
```

其中 `confEdge` = 屏幕边缘 5% 内线性降权（屏幕外无数据）。

> ⚠️ **与设计稿的差距**：早期设计的置信度表（DDGI 探针可见性、SSGI march 有效距离、
> RSM 光源视锥覆盖、RTGI SPP/时域收敛度）**均未落地**——`GIChannelBlendParams` UBO 里
> 根本没有 confidence 字段。当前实际只有「屏幕边缘可见性」一条 + 可选的相机距离让位。
> 距离衰减是**近似**而非物理判据：屏幕内清晰可见的远处物体，屏幕空间 GI 依然可信。

### 3.3 频率分离（设计目标，未实现）

```hlsl
float3 base   = SampleDDGI(...);                  // 低频基底
float3 detail = SampleSSGI(...) - LowPass(SSGI);  // 去低频后的高频残差
float3 gi     = base + detail;                    // 频段不重叠 → 数学合法的叠加
```

物理依据：**归一化**解决「多源估同一量」的双重计数；**频率分离**解决「低频基底 + 高频细节」
的不重叠叠加。两者是**并列的合法合成方式**，不是替代关系。

### 3.4 源量纲一致性（待修，详见 §9.2-A）

归一化加权的前提是**所有源返回同一个物理量**。当前实现不满足：

| 源 | 返回表达式 | 是否含**接收面** albedo |
|---|---|---|
| IBL | `kD × irradiance × albedo × iblIntensity` | ✅ |
| SSGI | `albedo × indirect × intensity` | ✅ |
| DDGI | `SampleDDGI()` 直接返回**辐照度 E** | ❌ |
| RTGI | 命中点出射辐射度按余弦半球平均（≈ E/π） | ❌（含的是**命中面** albedo） |

Medium/High 档默认同时启用 SSGI+DDGI，Ultra 档启用 RTGI+DDGI，
于是 `(SSGI×albedo + E_ddgi) / 2` 在混合不同量纲，结果随 albedo 偏移。

白炉测试**抓不到它**——`SampleDiffuseSource` 首行 `if (furnace) return float3(1,1,1)`
把源真值短路了：白炉验证的是**归一化数学**，不是源量纲。

---

## 4. 统一抽象：`IGIProvider`

> 实现位置：`Engine/Render/GI/IGIProvider.h`

设计承诺：**新增一种 GI 只需实现本接口 + 注册**；帧图按注册表遍历构建 pass，
不再手写 `ShouldRunXXX` 门控，也不改 UBO / 合成循环 / 面板候选列表。

### 4.1 接口构成（以实际实现为准）

| 分组 | 方法 |
|---|---|
| **身份** | `GetSourceId()` / `GetBand()`（默认由 `GIBandOf` 推导）/ `GetName()` / `Handles(id)` |
| **调度** | `GetPassKind()`（`Offscreen` / `Compute` / `Custom`）、`HasTextureOutput()`、`IsValid()`、`NeedsPass(stack)`、`SyncToStack(stack)` |
| **通道输出** | `GetDiffuse/Specular/AOOutput()` + `GetFinalDiffuse/Specular/AOOutput()`（后者含降噪/半分辨率选择） |
| **附属 pass** | `GetAuxPassCount/Name/Output/Input()` + `PreBindAux()` / `RenderAux()` |
| **生命周期** | `Initialize()` / `Shutdown()` / `OnResize()` / `Render(cmd, ctx)` / `PreBind()` / `SetInputs()` |

`Handles(id)` 用于表达「**同 pass 多模式**」：`ScreenAOProvider` 同时代表 SSAO 与 GTAO，
由 `SyncToStack` 在二者间切换片段着色器。

`GIProviderContext` 由帧图注入：`world` / `sceneGraph` / `camera` / `frameIndex`，
以及光追类源所需的 `lightBuffer` / `lightCount` / `tlas`。

### 4.2 已注册的 10 个源

| Provider | 覆盖的源 | pass 链 |
|---|---|---|
| `IBLProvider` | IBL | `IBL_Bake`（脏时重建，无 GBuffer 依赖） |
| `DDGIProvider` | DDGI | `DDGI`（compute，无通道纹理输出） |
| `ScreenAOProvider` | SSAO / GTAO | `AO`（同 pass 两种模式） |
| `SSGIProvider` | SSGI | `SSGI` → `SSGI_Denoise` |
| `SSRProvider` | SSR | `SSR` → `SSR_Denoise` |
| `RSMProvider` | RSM | `RSM`（需场景数据） |
| `RTEffectProvider` ×4 | RT 阴影 / RTAO / RT 反射 / RTGI | 主 pass → 时域累积（→ 空间滤波），共享 `AS_Build` |

**「零侵入」实测结论**：新增一种源的改动量为——实现 Provider（新文件）+ 注册 **1 行**；
帧图 / UBO / 合成循环 / 层栈结构 **0 行**。该承诺在 GTAO 与后续 10 源 Provider 化中均获验证。

---

## 5. 可用性与降级

可用性 = **管线能力 ∧ 设备能力 ∧ 层栈选择**。

```
GIConfigFromPreset(档位)                  → 层栈 + 精度
  → GIRegistry::Degrade(stacks, pipelineCaps, rtSupported)
        · 逐源裁剪：weight<=0 或不可用的源移除
        · 阴影单独降级：RT → Raster → None
        · 兜底：通道被裁空时补 IBL（diffuse/specular）或 SSAO（ao）
  → 帧图遍历 Provider 注册表，按 NeedsPass(stack) 注册 pass
  → LightingPass 把层栈直传为 UBO 源数组 → shader 按 id 分派合成
```

### 5.1 管线能力位

| 管线 | 能力位 |
|---|---|
| `PipelineCaps::Forward` | 光栅阴影 · IBL（diffuse+specular）· RSM — **无 GBuffer，故无屏幕空间源与探针** |
| `PipelineCaps::Deferred` | Forward 的全部 + SSGI · SSR · SSAO · DDGI · 光追阴影 · RTGI · RT 反射 · RTAO |

`GISourceId::Lightmap` **在 `ToPipelineCap()` 中没有分支**，落到 `default` 返回
`kPipelineGINone` → `IsAvailable()` 恒为 false。这是「预留但不可用」的准确语义（§9.2-F）。

### 5.2 四档预设（实际内容）

| 档位 | diffuse 层栈 | 其他通道 | 精度 / 强度 |
|---|---|---|---|
| **Low** | IBL + **SSGI** | specular: IBL；ao: SSAO | `halfRes=true`，`giIntensity=0.6` |
| **Medium**（默认） | IBL + SSGI + **DDGI** | 同上 | `giIntensity=0.8` |
| **High** | IBL + SSGI + DDGI | 同上 | 全分辨率，`giIntensity=1.0` |
| **Ultra** | IBL + **RTGI** + DDGI | specular: IBL + **RTReflection**；ao: SSAO + **RTAO**；shadow: **RT** | `giIntensity=1.2` |

> ⚠️ 与早期设计稿的档位表不同（设计稿写 Low = 仅 DDGI、且未含 IBL 基线与 specular/ao 通道）。
> 实际**每个档位都以 IBL/SSAO/光栅阴影为基线**，档位只在其上叠加。
> 切档只改层栈内容与精度，**合成公式不变 → 无亮度跳变**。

---

## 6. 关键不变量（需长期保持）

1. **层栈与子系统开关同源**——层栈说参与，子系统就必须真的启用。
   *（曾因两者不一致导致画面发黑，且在同一处复发过一次）*
2. **权重归一化**是物理正确性的来源；距离让位只是性能/艺术控制，默认关闭。
3. **多开一个源不会变亮**；单源时行为与「二选一」时代完全一致。
4. **光追是「GI 源」而非「管线类型」**——管线能力位（架构）× 设备能力（`rtSupported`）两层判断。
5. **阴影是「可见性（乘法项）」而非「能量（加法项）」**——不进层栈。
6. **所有参与合成的源必须返回同一物理量**（当前**未满足**，见 §3.4 / §9.2-A）。

---

## 7. 与既有系统的关系

| 既有机制 | 关系 |
|---|---|
| `GIConfig` 4 档档位 | 保留——改的是「层栈内容 + 精度」，不改档位体系 |
| `PipelineCaps` / `GIRegistry::Degrade` | 扩展为逐源裁剪 + 每通道兜底 |
| `ShadowChannel` | 保留为独立枚举（阴影不是能量项） |
| `IGlobalIllumination`（`GlobalIllumination.h`） | **并存**：仅 `GI_IBL` 仍以此身份被帧图使用；其余源改由 `IGIProvider` 包装。`GIMode` 中的 `VXGI` / `ReSTIR` 为无实现占位 |
| `06.GILab` | GI 对比实验室：四通道层栈 UI + 画质档位 + 白炉探针 + GPU Profiler |
| `06_GILab.cfg` | 层栈序列化（源 id / weight / falloff） |

---

# 第二部 · 开发计划

## 8. 当前落地状态

### 8.1 已完成

| 波次 | 内容 | 状态 |
|---|---|---|
| **Wave 0** | 验证基线（工具链/编译/运行）、白炉判据、四类每帧校验违规归零、崩溃处理器 | ✅ 完成 |
| **Wave 1** | UBO 按源数组化（`GISourceSlot[4]` + count + mode）、shader 按 id 分派、删除 `useScreenGI` 隐式耦合 | ✅ 完成 |
| **Wave 2 / P4** | `IGIProvider` 抽象，**10 个源全部接入注册表** | ✅ 完成 |
| **P1/P2/P3** | 数据模型 / 归一化合成 / 源层栈 | ✅ 完成 |
| **S1 / S1.5 / S2 / S3** | 光追归入 Deferred / 两类源同时参与 / 管线维度收敛 / 删除 `HybridRTPipeline` | ✅ 完成 |
| **PT** | 参考渲染器定位 + 与 Deferred 共享加速结构 | ✅ 完成 |
| **第 1 批遗留** | M4.4 RSM VPL 25→16（Poisson 盘 + 能量常数按 1/N 重标定）· M4.5（经核查**不适用**）· 06 面板候选由注册表派生 · 文档同步 | ✅ 完成 |
| **P0 / D2** | GI 数据模型下沉为 RHI-free `GI/GITypes.h`（断开旧 `GIConfig.h → LightingPass.h → RHI` 传导链，并移除 `GIConfig.h`/`GIRegistry.h` 两个转发头）；`Tests/TestGITypes.cpp` 36 用例 / 427 断言 | ✅ 完成 |

### 8.2 三个关键指标（实测）

| 指标 | 当前 | 目标 |
|---|---|---|
| 每帧 Vulkan 校验违规（四类） | **0 / 0 / 0 / 0** | 0 |
| **白炉读数（绝对亮度）** | **1.0000** | 1.0（±2%） |
| 偶发崩溃 | 约 100 次启动零崩溃（40/40 soak） | 持续为 0 **且根因获证** |

白炉读数由 **1.7998 → 1.0000**（修复「IBL 在归一化之外」与「RSM 旁路加法」两个缺口后达成）。

### 8.3 已明确不做 / 已划掉

| 项 | 结论 |
|---|---|
| M4.5 GBuffer 通道合并 | ⏭️ **不适用**：metallic/roughness 嵌在需 16 位精度的 MRT0/MRT1 alpha 通道，拆成独立 MRT 反增带宽 |
| M5.1 RTGI 时域累积 | ✅ **已划掉**：由 S1 的 `RTDenoiser`（velocity 重投影 + 去遮挡）覆盖；rgen 保持 SPP=1 是正确设计 |
| `GIBlendMode::Fallback`（分层回退） | ⛔ **放弃**：可由「只用最精确的源」的层栈组合表达 |
| NRC / VXGI / LPV / SVOGI | ⛔ 不在近期路线 |

---

## 9. 已知问题

### 9.1 文档与代码不一致（本次复核发现，已按代码修正本文）

| # | 项 | 文档原述 | 代码实际 |
|---|---|---|---|
| 1 | `GIBandOf` 频段映射 | 「RT*→High」 | 实现曾与文档不符：`High` **永不可达**（RT* 与 `default` 共用 fallthrough → 全部 `Mid`）。**已在 P0/D2 修复为显式 `High`，并加回归断言** |
| 2 | 四档档位内容 | Low = 仅 DDGI | 每档均含 IBL/SSAO/光栅阴影基线；Low = IBL+SSGI 且 `halfRes=true` |
| 3 | 降级行为 | 「RTGI→SSGI 同频段替代」 | 实际只做**移除 + 通道兜底**（diffuse/specular→IBL，ao→SSAO），无同频段替换 |
| 4 | 置信度体系 | 5 源各自的置信度依据表 | UBO 无 confidence 字段；实际只有「屏幕边缘 5% 降权」一条 |
| 5 | `IGIProvider` 签名 | `GetBand`/`GetRange`/`IsValid`/输出/生命周期 | 多出 `Handles`/`NeedsPass`/`SyncToStack`/`GetPassKind`/`HasTextureOutput`/附属 pass 一组；`GetRange` 已移除 |
| 6 | `Lightmap` 可用性 | 「预留，可用」 | `ToPipelineCap` 无该分支 → `IsAvailable` 恒 false，面板选不到 |
| 7 | **`Tests` 的可测性前提** | 「`Tests` 不链接 Render 模块，故 GI 无法被单测」（依据 `Tests/CMakeLists.txt:48-54` 的直接列表） | **不成立**：直接列表虽无 Render，但 `HugEngineAI`(PUBLIC) → `HugEngineRender` + `HugEngineEditor` → `HugEngineRender`，**传递依赖早已把 Render/RHI/Vulkan 拉入**，`Engine/Render` 也已在包含路径上。GI 本就可测；抽 `GITypes.h` 的真实价值是**分层解耦**（纯数据头不再拉全量 RHI）与**显式依赖**，而非「否则测不了」 |

### 9.2 代码复核发现的缺陷（**未经运行时验证**，需实测确认后修复）

> 以下由本次代码实读得出，与 §9.1 的「文档漂移」性质不同——这些是**实现本身的问题**。
> 建议逐项用 06.GILab 的对应开关做最小复现后再改。

| # | 严重度 | 问题 | 证据 |
|---|---|---|---|
| **A** | 严重 | **归一化混合了不同量纲的源**（§3.4）：IBL/SSGI 含接收面 albedo，DDGI/RTGI 不含 | `DeferredLighting.frag.slang` 的 `SampleDiffuseSource`；`RT_DDGI.slang` 返回辐照度 |
| **B** | 严重 | **SSR 屏幕投影用错矩阵**：`GI_SSR.cpp` 传的是 `inverse(proj)`，而 shader 用它做 view→clip 投影（`SSR.frag.slang` 的 Hi-Z 与线性 march 两条路径） | `GI_SSR.cpp:140` vs `SSR.frag.slang:59,92` |
| **C** | 严重 | **SSR 有效性协议未实现**：合成端判 `u_SSR.Sample().a < 0` 表示无效，但 `SSR.frag` 所有分支 alpha 都是 1.0，`Denoise.frag` 还会把 alpha 平均 → 判定永不成立，SSR miss 的黑色以全权重进入 `(IBL+0)/2` | `DeferredLighting.frag.slang:450` vs `SSR.frag.slang:32,110,112` |
| **D** | 中 | **AO 乘到了直接光上**，且不作用于镜面：`color *= lerp(1, ao*aoVal, aoIntensity)` 位于直接光累加之后、间接镜面之前 | `DeferredLighting.frag.slang:436` |
| **E** | 中 | **屏幕空间源用硬编码默认投影矩阵**而非真实相机：`kDefaultFOV=60°/0.1/2000`；`PhysicalCamera` 会由焦距反算 fov → 非默认相机下 SSGI/SSAO/SSR 重建错位。根因是 `IGIProvider` 未把相机传给屏幕空间源（只有 DDGI 有 `SetCamera`） | `GI_SSGI.cpp:186`、`GI_SSR.cpp:140`、`SSAO.cpp:271` |
| **F** | 中 | **RSM 的 pass 被嵌套在 DDGI 门控内**：单独勾选 RSM 而关闭 DDGI 时，RSM 永不注册（Forward 侧却是独立的 `ShouldRunRSM()`） | `DeferredPipeline_FrameGraph.cpp:288` |
| **G** | 中 | **层栈与子系统开关是两套真值**（不变量 1 的实际状态）：`IsValid()` 只读子系统 `enabled`，面板层栈 UI 只改层栈 → 勾选但静默失效。`halfRes` 还有第三重（需触发 `OnResize` 才重建纹理） | `SSRProvider.h:25` 等 + `06.GILab.cpp` 的通道 UI |
| **H** | 中 | **Provider 抽象只在 Deferred 落地**：`ForwardPipeline` 无 `m_GIProviders`，且 Forward 的 PBR shader **没有 `GIBlendParams` UBO** → 层栈归一化在 Forward 完全不存在，但 `PipelineCaps::Forward` 声明支持 IBL+RSM | `ForwardPipeline.h`；全仓 `GIBlendParams` 仅 `DeferredLighting.frag` 使用 |
| **I** | 中 | **RTGI 用 DDGI 做 miss 回退**，破坏「源独立」前提：Ultra 档同时含 RTGI+DDGI 时，DDGI 信息被用两次再归一化 → 加权平均失去无偏性 | `RT_GI.rgen.slang:111-113` + `RTProvider.h:243` |
| **J** | 低 | 合成参数 UBO 是**单份**、非 per-frame-in-flight（`MAX_FRAMES_IN_FLIGHT=3`） | `LightingPass.cpp:155-168` |
| **K** | 低 | **DDGI 网格外查询退化为「贴边常数外推」**，无 falloff 或无效标记；探针网格为固定参数，覆盖不到的区域静默缺失低频 GI | `RT_DDGI.slang:56-63,98`；`GI_DDGI.h:57-59` |

---

## 10. 后续任务与排序

排序原则：**先补验证能力，再动高风险主线；小项与调查项并行，不打断主线。**

| 顺位 | 任务 | 规模/风险 | 理由 |
|:---:|---|---|---|
| **P0** | ✅ **D2 · 抽 `GITypes.h` + 层栈/降级 CPU 单测**（**已完成**） | 小 / 低 | 守的是**已经咬过两次**的不变量 1；同时把 RHI 依赖从 GI 数据模型中剥离 |
| **P1** | **P5 · 频率分离**（Wave 3） | 大 / **高** | 合成正确性主题的收尾；前提（Wave 0 判据 + Wave 1 按源合成 + Wave 2 Provider）已全部就绪 |
| **P2** | **B3 · RSM VPL halfRes** | 小 / 低 | 与 P1 并行 |
| **P2** | **D1 · 偶发崩溃根因获证** | 未知 / 中 | 与 P1 并行；根因未证意味着已修项可能只是其中一个实例 |
| **P3** | **§9.2 的 A/B/C 三项正确性缺陷** | 中 / 中 | 直接影响画面正确性，建议插在 P5 之前或紧随 |
| **P4** | **B4 / M5.2-A · DDGI 光追 march** | 中 / 中 | 只提升单一源质量；DDGI 低频兜底已由 M5.3 修正，紧迫性下降 |
| **P5** | **P6 · ReSTIR GI 统一估计器** | 大 / 高 | 长期 |
| **P5** | **Lightmap 源落地** | 中 / 低 | 按需（PC 实时路线可缓） |

### 10.1 各项详情

**P0 · D2 层栈归一化 CPU 单测** —— ✅ **已完成**（36 用例 / 427 断言）
- 产出：`Engine/Render/GI/GITypes.h`（RHI-free，只依赖 `Core/Types.h`）+ `Tests/TestGITypes.cpp`
- **前置已澄清**：原述「`Tests` 不链接 Render 故 GI 无法单测」**不成立**（见 §9.1 第 7 行）——
  `HugEngineAI`(PUBLIC) 已传递引入 `HugEngineRender`。故本次的价值是
  **分层解耦**（断开旧 `GIConfig.h → LightingPass.h → RHI/RHI.h` 这条传导链）
  与**显式依赖**（Tests 显式声明 `Engine/Render` 包含路径，不再依赖传递隐式可得）
- 覆盖目标：频段映射（含 `High` 可达的回归断言）、能力位与可用性、
  `GIChannelStack` 的增删改语义与容量上限、`GIConfig` 门控谓词**严格由层栈派生**（防影子开关）、
  四档预设的基线与精度、`GIRegistry::Degrade` 的逐源裁剪与每通道兜底、
  shader UBO 镜像结构的布局不漂移
- 边界说明：测的是 **C++ 侧配置/注册表/降级**逻辑，**不是 shader 里的合成数学**——对 P5 的保护有限

**P1 · P5 频率分离**（内部顺序，**不要跳过第 0 步**）
```
0. LowPass 选型小实验（降采样 vs SH 低阶；保留 Normalized 作对照）
1. GIBlendMode 增 FrequencySplit
2. 低频基底 + 高频残差（detail = c - LowPass(c)，gi = base + detail）
3. 过渡与时序稳定（权重/层启用变化做时域平滑，避免模式切换跳变）
4. 面板三模式 A/B（Additive 对照 / Normalized / FrequencySplit）
```
- 验收：FrequencySplit 下**白炉仍守恒**；Sponza 对比**无过亮、细节保留优于纯归一化**；切换无跳变
- 风险：高——高频提取本身会引入噪声/振铃

**P2 · B3 RSM VPL halfRes** — 位置：`DeferredLighting.frag.slang` 的 `SampleRSMIndirect`（16 点 Poisson 盘 VPL 求和）

**P2 · D1 崩溃根因** — 长跑 soak + `HE_TRACE_FB=1`（`delay=0` 即同帧销毁的直接指标）；
现状：符号化落在 `VulkanTexture::GetImageView()` 野指针，最可疑根因（framebuffer 被同帧销毁）已修

**P3 · §9.2 A/B/C** — 量纲统一（约定「所有 diffuse 源返回已乘接收面 albedo 的间接出射辐射度」）、
SSR 投影矩阵正/逆分离、SSR 有效性协议落地

**P4 · B4 / M5.2-A DDGI 光追 march** — 方案 A（硬件光追 march）+ 按 `supportsRayTracing` 自动选择

---

## 11. 执行约定与验证方法

### 11.1 状态标记口径

| 标记 | 含义 |
|---|---|
| `✅ 已落地` | 代码已写 **且** 在当前 HEAD 编译通过 **且** `06.GILab` 实测确认 |
| `🟡 代码已写，验证待补` | 代码存在但缺编译/运行验证 |
| `⏳ 未做` | 尚无实现 |

### 11.2 执行约定

1. **编译通过 ≠ 完成**——必须在 `06.GILab` 跑 exe 冒烟 + 用户实测确认；**跑的时间要够长**
   （偶发崩溃曾 5 次出现 1 次，短测会漏）。
2. 每个里程碑**停下等用户确认**再进下一个。
3. 不自动 commit；合入点由用户确认；commit log 中文、无 AI 相关字样、无格式垃圾字符。
4. 开工前**复核文档引用的行号**（历史上已多次发生行号漂移）。
5. 添加代码**必须附中文注释**。

### 11.3 构建 / 运行 / 诊断

```powershell
# 关键：把 anaconda 加进 PATH，否则 Slang 之后的 SPV→头文件 步骤会以 MSB8066/9009 失败
$env:PATH = "C:\anaconda3;C:\anaconda3\Scripts;C:\anaconda3\Library\bin;$env:PATH"
cmake --preset default
cmake --build Build --config Debug --target 06.GILab -j 8
& Build\bin\Debug\06.GILab.exe          # 运行目录即 Build\bin\Debug（热重载的相对路径依赖它）
```

| 开关 | 用途 |
|---|---|
| `HE_FURNACE=1` + `HE_FURNACE_PROBE=1` | 白炉数值测试 + 像素读回（**自动化可用**，判据：绝对亮度 = 1.0） |
| `HE_TRACE_FB=1` | framebuffer 创建/销毁追踪（`delay=0` = 同帧销毁） |
| `HE_TRACE_PASSES=1` | 打印每个 pass 开始，把 pass 名与校验层报错在时间上对齐 |
| `HE_CRASH_TEST=1` | 主动崩溃，自检崩溃处理器 |
| `HE_FURNACE_PROBE=1` 单用 | 只开探针不开白炉 |
| `vk_layer_settings.txt` + `VK_LAYER_SETTINGS_PATH` | 关闭校验层重复消息上限，得到违规**真实次数** |

**运行期产物**：`Content/Config/06_GILab_crash.log`（目录已 gitignore）、
`Build/bin/Debug/06.GILab_crash.dmp`、`Content/Config/06_GILab.cfg` 与 `06_GILab_imgui.ini`。

### 11.4 风险总表

| 风险 | 影响 | 对策 |
|---|---|---|
| 偶发崩溃根因未证 | 长跑 + 读回类验收被污染 | 已修最可疑项（FB 同帧销毁）；根因保留为观察项，再发即由崩溃处理器出调用栈 |
| 合成改造后「画面变了」 | 回归难判断 | 单源配置逐像素截图对比 + 白炉数值判据（已可量化） |
| C++/slang 双侧同步漏改 | 静默错值 | `static_assert(sizeof)` + 双侧常量同源 |
| **白炉测试只验归一化、不验量纲** | 源的量纲错误被掩盖 | §9.2-A；白炉需扩展为「逐源真值校验」而非短路 |
| 频率分离的 `LowPass` 选型不当 | 噪声/振铃/跳变 | 先做离线小实验，保留 Normalized 作对照 |
| 校验层重复消息去重掩盖计数 | 误把「报告数」当真实次数（历史上两次误判） | 关闭 `duplicate_message_limit`，或用 `HE_TRACE_FB` 交叉验证 |
| P5 抽象/改造过度 | 大范围回归 | 分步提交（3.1→3.4），每步实测；保留 Additive/Normalized 作对照 |
| 文档与代码持续漂移 | 后续照文档实现出错 | 完成每个波次时同步回写本文 §2/§5 与状态表 |

---

## 附录 A · 里程碑与提交索引

> 本节为原两份文档中历史里程碑与提交级记录的**压缩索引**；完整原文可由 git 历史取回。

### A.1 技术里程碑（M 系列）

| 里程碑 | 内容 | 状态 | 提交 |
|---|---|---|---|
| M0 | 正确性 Bug 修复（SSGI 投影、DDGI 参数、RSM 守卫） | ✅ | `5712721` `55d1038` |
| M1 | 接口收敛（Render 33→6 参数）+ shader 通道化 | ✅ | `59c3382` `65d9cf5` |
| M2 | 数据驱动 `GIConfig` + 帧图自动编排 | ✅ | `2b06045` |
| M3 | Provider 注册表 + 自动降级 | ✅ | `66faf5c` |
| M4 | 性能：halfRes / SSR Hi-Z / DDGI 1/4 HDR | ✅ | `52d8280` `5441d2e` `afb7587` |
| M4.4 | RSM VPL 降采样 25→16 | ✅ | `42db644` |
| M5.2-B | DDGI 探针辐射度视角无关 | ✅ | `afb7587` |
| M5.3 | DDGI SH 修正（评估端补 Lambert 卷积 A_l） | ✅ | `e862931` |
| M6.3 | SSAO→GTAO（地平线切片 + 解析积分） | ✅ | `c487109` |

### A.2 架构演进（P / S 系列）

| Phase | 内容 | 状态 | 提交 |
|---|---|---|---|
| P1 | 数据模型：`GIConfig` 层栈 + `GIBand` / `weight` / `falloffDistance` | ✅ | `105911b` |
| P2 | 合成改归一化（保留 Additive 对照）+ 参数经 UBO 传递 | ✅ | `105911b` |
| P3 | 4 个单值枚举 → `GIChannelStack`；帧图门控与降级逐源化 | ✅ | `105911b` |
| S1 / S1.5 | 光追归入 Deferred（作为 GI 源）/ 两类源同时参与 | ✅ | `90649ba` `aac5690` `5c2b84b` |
| S2 / S3 | 管线维度收敛 / 移除 `HybridRTPipeline`（-1245 行） | ✅ | `06c8580` `0f8aca8` |
| PT | 参考渲染器定位 + 与 Deferred 共享加速结构 | ✅ | `3301040` |
| P4 | Provider 抽象，10 源全接入 | ✅ | `3a4075b` `8fc6091` `2b6b51a` `fd2510d` `48984e9` `51b8c98` `ea4620d` |
| **P5** | **频率分离** | **⏳** | — |
| **P6** | **统一估计器（ReSTIR GI）** | **⏳** | — |

### A.3 波次（Wave 系列）

| Wave | 内容 | 状态 |
|---|---|---|
| Wave 0.0 | 工具链 + 验证基线 | ✅ |
| Wave 0.1 | 偶发崩溃按死 | 🔄 判据满足、**根因未证** |
| Wave 0.2 | 白炉判据（读数 1.7998 → **1.0000**） | ✅ |
| Wave 0.7 | 四类每帧校验违规 → 0/0/0/0 | ✅ |
| Wave 0.8 | 崩溃处理器（自检通过） | ✅ |
| Wave 0 缺口 A/B/C | IBL 归位归一化 / RSM 归入层栈 / 3 槽位数组化 | ✅ |
| Wave 1 | UBO 按源数组化 + shader 按源分派 | ✅ |
| Wave 2 | `IGIProvider` 抽象（10 源） | ✅ |
| **Wave 3** | **频率分离** | **⏳ 未开始** |
| Wave 4 余项 | M5.2-A DDGI 光追 march · B3 RSM halfRes · D2 CPU 单测 | ⏳ |
| **Wave 5** | **P6 统一估计器 · Lightmap 源 · D1 崩溃根因** | **⏳** |

### A.4 遗留任务批次

| 批次 | 内容 | 状态 |
|---|---|---|
| 第 1 批 | M4.4 · M4.5（不适用）· 面板候选派生 · 文档同步 | ✅ `42db644` |
| 第 2 批 | B4 DDGI 光追 march · B3 RSM halfRes · D2 CPU 单测 | ⏳ |
| 第 3 批 | Wave 3 频率分离 | ⏳ |
| 第 4 批 | Wave 5（P6 / Lightmap / D1） | ⏳ |
