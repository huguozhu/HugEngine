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

### 2.2 源分类（谓词，不是「频段」）

```cpp
inline bool IsWorldSpaceSource(GISourceId);   // 环境（世界空间）：IBL · Lightmap · DDGI
inline bool IsScreenSpaceSource(GISourceId);  // 屏幕空间/单次光栅：SSGI · SSR · SSAO · RSM · GTAO
inline bool IsRayTracingSource(GISourceId);   // 光追：RTGI · RTReflection · RTAO
inline const char* GISourceClassName(GISourceId);   // 面板标签，由上述三谓词推导
```

三个谓词**互斥且完备**（对 11 个源构成一个无歧义的划分：3 + 5 + 3 = 11），
`GISourceClassName` 由它们推导，保证「名字与语义同源」。

> **⚠️ 为什么删掉了原来的 `GIBand {Low, Mid, High}`（P0·REDUNDANCY 期间的结论）**
>
> 该枚举显示为「低频 / 中频 / 高频」，但它实际表达的不是空间频段，而且**一个枚举混了
> 两个正交维度**：
>
> | 分界 | 实际区别 | 维度 |
> |---|---|---|
> | `Low` ↔ `Mid` | 世界空间大范围（环境图 / 3 m 探针网格）vs 屏幕空间像素级 | **尺度** |
> | `Mid` ↔ `High` | 屏幕空间近似 vs 光追精确 | **精度** |
>
> **反证**：`RTGI` 的输出是 **1/4 分辨率**，其**空间分辨率低于**全分辨率 `SSGI`，
> 却被标为「高**频**」。可见 `High` 表达的是**精度高**，不是空间频率高。
>
> **直接后果**：`GIBandOf(SSGI)=Mid` 而 `GIBandOf(RTGI)=High`——二者估的是
> **同一个物理量**（间接漫反射），却分属不同"频段"。这也使该枚举**无法充当 P5 的频率边界**。
>
> 它当时的全部实际用途只有**面板上显示一个标签**（`GIBandName(GIBandOf(id))`），
> 而 `IGIProvider::GetBand()` 是**零调用点的死接口**（实测全仓无任何调用）。
> 故连同 `GetBand()` 一并删除，改为上述谓词。
>
> **对 P5（频率分离）的影响**：不能拿「源分类」当频率边界做 `base + detail`——
> 真正的空间频率差只有一处：**DDGI 的探针网格尺度（`cellSize` 3 m）vs 屏幕空间/光追的
> 像素尺度**。故 P5 的落点应为「低频基底 = DDGI，高频残差 = SSGI/RTGI 去其网格尺度均值」。

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
  `FrequencySplit`（频率分离）**均未实现**——`Fallback` 已确认**放弃**（可由「只用最精确的源」
  的层栈组合表达）；`FrequencySplit` 经 P5 步骤 0 实测**判定不需要**（§3.3），
  一并**放弃**。故归一化仍为唯一的生产合成模式，`Additive` 只作 A/B 对照保留。

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
>
> ⚠️ **`falloffDistance` 不省性能（重要纠正）**：实测它只在两处被消费——
> `DeferredLighting.frag.slang:110`（合成时缩放权重）与帧图填 UBO
> （`DeferredPipeline_FrameGraph.cpp:685,694`）。**它不改变任何 pass 是否执行**：
> 源照旧整幅、每帧跑完，只是合成时贡献被压小。
> 故本节的「距离让位」是**纯艺术/合成控制，不具备性能意义**——
> 早期文档把它写成「性能/艺术控制」，性能那半是错的，会误导优化方向。

### 3.3 频率分离 —— ❌ 已判定**不需要**（P5 步骤 0 实测结论）

原设计意图：

```hlsl
float3 base   = SampleDDGI(...);                  // 低频基底
float3 detail = SampleSSGI(...) - LowPass(SSGI);  // 去低频后的高频残差
float3 gi     = base + detail;                    // 频段不重叠 → 数学合法的叠加
```

物理依据：**归一化**解决「多源估同一量」的双重计数；**频率分离**解决「低频基底 + 高频细节」
的不重叠叠加。两者是并列的合法合成方式——但频率分离**只在两源部分重叠时才有价值**：
它需要 `LowPass(SSGI) ≈ DDGI` 成立，`SSGI − LowPass(SSGI)` 才真的表示「DDGI 未覆盖的部分」。
否则频率分离只是把某个源的偏差当成另一个源的补充。

**步骤 0 实测否定了该前提。** 方法：单源层栈做差 `S_x = lum(HDR_x − HDR_none)`（直接光/天空/
镜面在差中精确抵消），再做径向功率谱与频率域高斯低通扫描。采样设施见 §11.3。

| | 全掩码（92.8% 像素） | 收紧掩码（24.4% 像素，仅 SSGI 覆盖内部） |
|---|---|---|
| DDGI / SSGI 量级比 | 67.7× | 33.3× |
| `corr(SSGI, DDGI)`（不低通） | 0.1454 | **0.2779** |
| `corr(LowPass(SSGI), DDGI)` 最优 | 0.0912 @ σ=1（**低于不低通**） | 0.2826 @ σ=1（**约等于不低通**） |
| 最优标度拟合后 DDGI 未被解释的方差 | 99.6% | **95.9%** |
| 最低频段（0–0.042 cyc/px）能量占比 | DDGI 99.82% / SSGI 40.13% | DDGI **97.90%** / SSGI **96.00%** |

两点判定：

1. **低通并不能让 SSGI 逼近 DDGI**。`corr(LowPass(SSGI), DDGI)` 随低通增强**单调下降**
   （收紧掩码下 0.2826 → 0.2318），最优即几乎不低通；即使做最优标度拟合，低通后的 SSGI
   仍解释不了 DDGI 96% 的方差。**不存在任何低通尺度使 `LowPass(SSGI) ≈ DDGI`**。
   这也回答了本项原先的首选思路「用估计器自身的核做低通」——问题不在低通算子选型，
   而在被低通的那个信号本身就不逼近 DDGI。
2. **「两源频段不重叠」是假象**。全掩码下 SSGI 看似宽带（仅 40% 能量在最低频段），但那是
   它自身 **0 / 非 0 覆盖边界**（屏幕空间射线打不到东西时恰好返回 0）制造的硬边；收紧到
   SSGI 真正有数据的内部后，两源频谱形状**几乎一致**（96.00% vs 97.90%，逐段比值 0.4–1.0）。

**结论**：DDGI 与 SSGI **同频段、且是同一个量的两种互相不服的近似估计**——两者都极度平滑
（DDGI 被 SH l≤2 与 3.0 单位探针网格平滑，SSGI 被 16 次采样平均 + 5×5 双边降噪平滑），
却在同一位置分歧很大（corr 仅 0.28）。这正是 §3.1 归一化的适用场景，即**现状已是正确的
合成方式**。三选一的落点：

| 关系 | 判定 |
|---|---|
| 同频段 / 互为替代估计 → 归一化 | ✅ **就是这一种（现状）** |
| 完全不相交 → 加法（`GIBlendMode::Additive`） | ✗ 当前没有此情形的源对 |
| 部分重叠 → 频率分离 | ✗ **无频段可分离** |

补充：`GIBlendMode::Additive` 保留为「确有不重叠源时才用」的备用项。注意**「加法」本身
已经实现了频段叠加**——两源频段不重叠时直接相加即可，无需额外的 `LowPass` 算子；频率分离
真正的用武之地仅在「两源在重叠区必须抑制双重计数」时，而这里没有可分的边界。

> **P5 顺带暴露出的真正阻塞点（不是合成规则）**：归一化下等权重时 SSGI 只占输出值约
> **1.5%**（= 1/(1+67.7)），同时把 DDGI 自身贡献**折半**——即 SSGI 每帧付一整幅屏幕空间
> pass 的成本，却只带来 1.5% 的扰动（§3.5「白付一份全量成本」）。叠加 §3.4 的量纲问题，
> **应先做 §10 `SSGI-CAL` 标定，而不是改合成模式**：在标定完成前调整混合规则，
> 等于给一个未标定的启发式量加权。

### 3.4 源量纲一致性 —— ✅ 已统一（P1·A）

归一化加权的前提是**所有源返回同一个物理量**。

**统一约定（基准由 IBL 确立）**：

```
L_o = albedo × E/π          ← 已乘接收面 albedo 的间接出射辐射度
```

基准之所以是 IBL：其辐照度图在卷积时**已归一化为 E/π**（见 `GI/IBL_Irradiance.frag`
末尾的 `× π / sampleCount`），故 `kD × irradianceMap × albedo` 本身就是出射辐射度。

修复前后各源的量纲（`Lighting/DeferredLighting.frag.slang` 的 `SampleDiffuseSource`）：

| 源 | 修复前返回 | 含接收面 albedo | 修复动作 |
|---|---|---|---|
| IBL | `kD × (E/π) × albedo` | ✅ | —（基准） |
| SSGI | `albedo × indirect` | ✅ | —（标度仍是启发式，见下） |
| DDGI | **E** | ❌ | 补 `albedo/π` |
| RTGI | **E/π** | ❌（含的是命中面 albedo） | 补接收面 `albedo` |
| RSM | **≈E**（经验常数已含 1/π） | ❌ | 补接收面 `albedo`，并**解耦 `iblIntensity`** |

第 4 处（同一缺陷的另一实例）：`RayTracing/RT_GI.rgen.slang` 的 DDGI miss 回退
把**辐照度 E** 当**辐射度 L** 累加（命中路径给的是 L），已补 `× 1/π`。

**为什么白炉抓不到它**：`SampleDiffuseSource` 首行 `if (furnace) return float3(1,1,1)`
把源真值**短路**了——白炉验证的是**归一化数学**，不是源量纲。
故本次改用**单源亮度实测**验证（见 §9.2 的实测记录）。

> **遗留（现已成为首要后续项）**：SSGI 的整体标度是启发式（`falloff = 1/(1+|sDir|²·radius)`，
> 无量纲），未按 `E/π` 校准，靠用户 `intensity` 调节。P5 的实测已确认这是当前 GI 合成链上的
> 主要缺口（§3.3）：标度未统一时，归一化把 SSGI 压到约 **1.5%** 的贡献——开着却几乎看不见。
> 严格统一标度需以 PT 为参考做实测校准，列为独立任务（§10 `SSGI-CAL`）。
>
> 另需一并处理：`GI/SSGI.frag` 只累加**命中点的 albedo**
> （`indirect += sAlbedo * max(0,dot(N,sDir)) * falloff`），**不含任何入射辐射度项**，
> 因此它现在并不是 `E/π` 的估计，而是「反照率相关性」启发式量。仅靠调 `intensity`
> 无法补上缺失的物理量。

### 3.5 执行模型：每帧成本结构

**执行单位是「源」。** 源分类（§2.2 的三个谓词）既不参与合成（§3.1），也不参与执行决策——
它只用于面板标签与配置诊断。

| 情况 | 成本 |
|---|---|
| 层栈里**未启用**的源 | **零**——帧图有 6 处 `NeedsPass` 门控（`DeferredPipeline_FrameGraph.cpp:318/337/357/382/429/585`），pass 根本不注册 |
| 层栈里**已启用**的源 | **每帧整幅执行**，**没有任何**分区 / 分块 / 距离剔除 |

各源每帧的实测量：

| 源 | pass 形态 | 规模 |
|---|---|---|
| IBL | 无 pass（烘焙） | **仅脏时重建**——唯一非每帧的源 |
| DDGI | compute | **256 探针**（8×4×8）× `numSamples` 次单步采样 |
| SSGI | 全屏 offscreen | 全屏或**半分辨率** × 16–32 采样 |
| SSR | 全屏 offscreen | 全屏或半分辨率，Hi-Z march |
| SSAO / GTAO | 全屏 offscreen | 全屏或半分辨率（**同一 pass 的两种模式**） |
| RSM | 光源视锥光栅化 | **整个场景**从光源视角再画一遍 |
| RTGI | RT dispatch | **1/4 分辨率** |
| RT 反射 / RTAO / RT 阴影 | RT dispatch | 各自 1/2 ~ 全分辨率 |
| `AS_Build` | TLAS 重建 | **整个场景每帧重建**（只要开了任一 RT 源） |

```
每帧 GI 成本 ≈ Σ(所有启用的源) + AS_Build + Lighting 的逐像素采样
```

**线性随启用源数增长，没有亚线性项。**

与主流引擎的结构差异值得记住：

| | 组合发生在 | 每帧需要跑几套 |
|---|---|---|
| UE Lumen | 一个估计器**内部**，逐射线续接 | **1 套追踪**同时服务 GI + 反射 |
| CryEngine SVOGI | 一个体素结构 | **1 次体素化**同时服务 GI + 大尺度 AO(+specular) |
| **HugEngine** | **合成端**混合 N 个独立缓冲 | **N 个源各跑一遍** |

**根因**：合成端做加权平均，就要求每个源都产出**完整的通道缓冲**（合成时要逐像素取它的值）。
Lumen 不需要「SSGI 缓冲」与「DDGI 缓冲」——它只有一条射线。
**本架构的组合方式决定了它必须付 N 份全量成本。**

由此可识别的三类浪费（§10 有对应任务）：

1. **严格冗余**：`SSAO + GTAO` 同时启用——二者共用**同一 pass、同一输出纹理**，
   归一化平均会把同一纹理值平均回它自己：**零增益、纯浪费**。
2. **重复估计**：`SSGI + RTGI`、`SSR + RTReflection`、`SSAO + RTAO`——同为逐屏幕像素的
   同一物理量估计，成本翻倍且归一化会互相稀释。
   *注：这是 S1.5 的**有意设计**（「两类源同时参与」），故只能提示、不宜强制剔除。*
3. **相关性**：`RTGI + DDGI`——RTGI 的 miss 回退即 DDGI（§9.2-I），二者非独立估计，
   归一化平均失去无偏性。

---

## 4. 统一抽象：`IGIProvider`

> 实现位置：`Engine/Render/GI/IGIProvider.h`

设计承诺：**新增一种 GI 只需实现本接口 + 注册**；帧图按注册表遍历构建 pass，
不再手写 `ShouldRunXXX` 门控，也不改 UBO / 合成循环 / 面板候选列表。

### 4.1 接口构成（以实际实现为准）

| 分组 | 方法 |
|---|---|
| **身份** | `GetSourceId()` / `GetName()` / `Handles(id)`（源分类由 `IsWorldSpace/ScreenSpace/RayTracingSource` 三谓词给出，见 §2.2） |
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
| **P0 / D2** | GI 数据模型下沉为 RHI-free `GI/GITypes.h`（断开旧 `GIConfig.h → LightingPass.h → RHI` 传导链，并移除 `GIConfig.h`/`GIRegistry.h` 两个转发头） | ✅ 完成 |
| **P0 / REDUNDANCY** | 冗余源诊断（严格冗余 / 重复估计 / 相关估计 / 成本提示）+ 可证等价去重；删除语义混淆的 `GIBand` 与死接口 `GetBand()`，改为三个分类谓词；06.GILab 诊断面板 | ✅ 完成 |
| **P1 / A·B·C** | 源量纲统一（`L_o = albedo × E/π`）· SSR 投影矩阵进 UBO · SSR 有效性协议贯通四处 | ✅ 完成 |
| **P1 / L** | **SSGI 完全不生效**（Provider 覆写改名 → 输入恒空）；顺带确立「纹理级数值对照」判定手段 | ✅ 完成 |
| **P2 / P5** | 频率分离 —— 步骤 0 实测**判定不需要**（两源同频段，`LowPass(SSGI) ≈ DDGI` 不成立） | ⛔ 已判定退场 |

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
| `GIBlendMode::FrequencySplit`（频率分离） | ⛔ **放弃**：P5 步骤 0 实测**判定不需要**——不存在使 `LowPass(SSGI) ≈ DDGI` 的低通尺度，两源本就同频段（§3.3） |
| NRC / VXGI / LPV / SVOGI | ⛔ 不在近期路线 |

---

## 9. 已知问题

### 9.1 文档与代码不一致（本次复核发现，已按代码修正本文）

| # | 项 | 文档原述 | 代码实际 |
|---|---|---|---|
| 1 | ~~`GIBandOf` 频段映射~~ | 「RT*→High」 | 曾有两层问题：先是 `High` **永不可达**（RT* 与 `default` 共用 fallthrough → 全部 `Mid`，已在 P0/D2 修复）；随后发现**该枚举本身语义有误**——它混了「尺度」与「精度」两个正交维度，且 SSGI=Mid / RTGI=High 把同一物理量标成不同"频段"。**已整体删除**，改为 §2.2 的三个分类谓词 |
| 2 | 四档档位内容 | Low = 仅 DDGI | 每档均含 IBL/SSAO/光栅阴影基线；Low = IBL+SSGI 且 `halfRes=true` |
| 3 | 降级行为 | 「RTGI→SSGI 同频段替代」 | 实际只做**移除 + 通道兜底**（diffuse/specular→IBL，ao→SSAO），无同频段替换 |
| 4 | 置信度体系 | 5 源各自的置信度依据表 | UBO 无 confidence 字段；实际只有「屏幕边缘 5% 降权」一条 |
| 5 | `IGIProvider` 签名 | `GetBand`/`GetRange`/`IsValid`/输出/生命周期 | 多出 `Handles`/`NeedsPass`/`SyncToStack`/`GetPassKind`/`HasTextureOutput`/附属 pass 一组；`GetRange` 已移除 |
| 6 | `Lightmap` 可用性 | 「预留，可用」 | `ToPipelineCap` 无该分支 → `IsAvailable` 恒 false，面板选不到 |
| 7 | **`Tests` 的可测性前提** | 「`Tests` 不链接 Render 模块，故 GI 无法被单测」（依据 `Tests/CMakeLists.txt:48-54` 的直接列表） | **不成立**：直接列表虽无 Render，但 `HugEngineAI`(PUBLIC) → `HugEngineRender` + `HugEngineEditor` → `HugEngineRender`，**传递依赖早已把 Render/RHI/Vulkan 拉入**，`Engine/Render` 也已在包含路径上。GI 本就可测；抽 `GITypes.h` 的真实价值是**分层解耦**（纯数据头不再拉全量 RHI）与**显式依赖**，而非「否则测不了」 |

### 9.2 代码复核发现的缺陷（A/B/C/L 已实测修复，其余待验证）

> 以下由代码实读得出，与 §9.1 的「文档漂移」性质不同——这些是**实现本身的问题**。
> 标 ✅ 的项已按「最小复现 → 修改 → 实测对照 → 回归」闭环；其余各项仍需先用 06.GILab
> 的对应开关做最小复现后再改。
>
> **重要教训**：这类缺陷**不是都能靠读代码或写单测发现**。L 项（SSGI 完全不生效）在代码上
> 完全自洽、编译无警告、`IsValid()` 也为真、面板上一切正常——只有把**各源的中间纹理读回来
> 做数值对照**（§11.3 的 `HE_DUMP_GI`）才暴露出来。凡涉及「某源是否真的参与了合成」，
> 都应以纹理级数值对照为准，不以开关状态为准。

| # | 严重度 | 问题 | 证据 |
|---|---|---|---|
| ~~**A**~~ | ✅ **已修复** | **归一化混合了不同量纲的源**（§3.4）：IBL/SSGI 含接收面 albedo，DDGI/RTGI 不含 | 见下方「A 的修复与实测」 |
| ~~**B**~~ | ✅ **已修复** | **SSR 屏幕投影用错矩阵**：`GI_SSR.cpp` 传的是 `inverse(proj)`，而 shader 用它做 view→clip 投影（Hi-Z 与线性 march 两条路径都错） | 见下方「B/C 的修复与实测」 |
| ~~**C**~~ | ✅ **已修复** | **SSR 有效性协议未实现**：合成端判 `u_SSR.Sample().a < 0` 表示无效，但 SSR 所有分支 alpha 都是 1.0，空间/时域降噪还会把 alpha 平均 → 判定永不成立，SSR miss 的黑色以全权重进入 `(IBL+0)/2` | 见下方「B/C 的修复与实测」 |
| **D** | 中 | **AO 乘到了直接光上**，且不作用于镜面：`color *= lerp(1, ao*aoVal, aoIntensity)` 位于直接光累加之后、间接镜面之前 | `Lighting/DeferredLighting.frag.slang:436` |
| **E** | 中 | **屏幕空间源用硬编码默认投影矩阵**而非真实相机：`kDefaultFOV=60°/0.1/2000`；`PhysicalCamera` 会由焦距反算 fov → 非默认相机下 SSGI/SSAO/SSR 重建错位。根因是 `IGIProvider` 未把相机传给屏幕空间源（只有 DDGI 有 `SetCamera`） | `GI_SSGI.cpp:186`、`GI_SSR.cpp:140`、`SSAO.cpp:271` |
| **F** | 中 | **RSM 的 pass 被嵌套在 DDGI 门控内**：单独勾选 RSM 而关闭 DDGI 时，RSM 永不注册（Forward 侧却是独立的 `ShouldRunRSM()`） | `DeferredPipeline_FrameGraph.cpp:288` |
| **G** | 中 | **层栈与子系统开关是两套真值**（不变量 1 的实际状态）：`IsValid()` 只读子系统 `enabled`，面板层栈 UI 只改层栈 → 勾选但静默失效。`halfRes` 还有第三重（需触发 `OnResize` 才重建纹理） | `SSRProvider.h:25` 等 + `06.GILab.cpp` 的通道 UI |
| **H** | 中 | **Provider 抽象只在 Deferred 落地**：`ForwardPipeline` 无 `m_GIProviders`，且 Forward 的 PBR shader **没有 `GIBlendParams` UBO** → 层栈归一化在 Forward 完全不存在，但 `PipelineCaps::Forward` 声明支持 IBL+RSM | `ForwardPipeline.h`；全仓 `GIBlendParams` 仅 DeferredLighting 使用 |
| **I** | 中 | **RTGI 用 DDGI 做 miss 回退**，破坏「源独立」前提：Ultra 档同时含 RTGI+DDGI 时，DDGI 信息被用两次再归一化 → 加权平均失去无偏性 | `RT_GI.rgen.slang:111-117` + `RTProvider.h:243` |
| **J** | 低 | 合成参数 UBO 是**单份**、非 per-frame-in-flight（`MAX_FRAMES_IN_FLIGHT=3`） | `LightingPass.cpp:155-168` |
| **K** | 低 | **DDGI 网格外查询退化为「贴边常数外推」**，无 falloff 或无效标记；探针网格为固定参数，覆盖不到的区域静默缺失低频 GI | `RT_DDGI.slang:56-63,98`；`GI_DDGI.h:57-59` |
| ~~**L**~~ | ✅ **已修复** | **SSGI 完全不生效**：`SSGIProvider` 把接口覆写写成了 `SetGBuffer`，而基类 `IGIProvider::SetInputs` 带**空实现的默认体** → 改名既不报错也不警告，静默落到空实现 → `m_Depth/m_Normal/m_Albedo` 恒为 `nullptr` → `GI_SSGI::Render` 在守卫处提前返回，SSGI 输出纹理只剩清屏值 `(0,0,0,1)`。表现为「SSGI 已启用、`IsValid()` 为真、面板一切正常，但对画面的贡献恒为 0」 | 见下方「L 的修复与实测」 |

**A 的修复与实测**（4 处量纲修正 + 单源亮度验证）

改动：
1. `Lighting/DeferredLighting.frag.slang` · `SampleDiffuseSource`：DDGI 补 `albedo/π`；RTGI 补接收面 `albedo`；RSM 补接收面 `albedo`
2. 同上 · `SampleRSMIndirect`：移除对 `iblIntensity` 的**门控与缩放**（此前「把 IBL 调暗」会连带关掉 RSM）
3. `RayTracing/RT_GI.rgen.slang`：DDGI miss 回退补 `× 1/π`（E → L，与命中路径量纲对齐）

实测（`HE_FURNACE_PROBE=1`，不启白炉；cfg 设为 **diffuse 层栈 = 仅 DDGI**、无镜面/AO/阴影、关闭直接光）：

| | 修复前 | 修复后 |
|---|---|---|
| 中心亮度 | 0.0279 | **0.0034** |
| 中心 RGB | (0.0177, 0.0266, 0.0709) | (0.0024, 0.0033, 0.0075) |
| **中心 vs 背景** | **完全相同（比值 1.0000）** | 0.0034 vs 0.0029（比值 1.16） |

两条特征分别印证了缺陷与修复：
- **修复前中心亮度与接收面 albedo 无关** → 中心与背景读数一模一样（都是 0.0279）——
  这正是「DDGI 返回 E、不含 albedo」的直接指纹；
- **修复后逐通道比值 = π/albedo_c**：整体 8.21× 反推 albedo ≈ 0.38；逐通道反推得
  R = 0.43 / G = 0.39 / B = 0.33 —— Sponza 中心像素的**暖色** albedo，与 `albedo/π` 的预测关系一致。

回归：白炉读数仍为 **1.0000**（预期——白炉短路源真值，本就不覆盖量纲）；VUID 46 = 改前 46。
*（`build/verify/` 下保留了 before/after 两份运行日志可供查阅）*

**B/C 的修复与实测**（SSR 的空间正确性 + 有效性协议）

**B · 投影矩阵正/逆分离**
- 根因确认：引擎保证的 push constant 范围只有 **128B**，而 `GI_SSR.cpp` 原先只往它里面
  传了**一个**矩阵（`inverse(proj)`）——**正投影无处安放**，于是 shader 拿逆矩阵当正投影用。
- 修法（照 **SSAO 早已采用**的惯例）：矩阵进 UBO。`GI_SSR.cpp:30` 本就声明了
  binding 3 是 `UniformBuffer` 却从未创建/绑定过——正好用它放 `invProj + proj`（128B）。
  push constant 从 96B **缩到 32B**（只剩标量）。
- shader 侧按用途命名：`u_InvProj`（clip→view）与 `u_Proj`（view→clip）。改完后 4 处用法
  全部正确，其中 2 处（`mul(u_Proj, rayPos)` × 两条 march 路径）是**自动变正确**的
  ——因为 `u_Proj` 现在真的持有正投影。

**C · 有效性协议贯通**（此前断在**四处**，不止文档写的两处）

| 环节 | 修复前 | 修复后 |
|---|---|---|
| `GI/SSR.frag.slang` | 天空/miss 分支返回 alpha **1.0** | 返回 **−1**（与 `RT_Reflection.rgen` 早已采用的协议一致） |
| `PostProcess/Denoise.frag.slang`（空间降噪） | 把 alpha 一起加权平均 → **符号被抹掉** | **无效样本不参与**平均；有效性不参与平均，有有效贡献即 +1、全无则 −1 |
| `PostProcess/RT_DenoiseTemporal.frag.slang`（时域降噪） | `lerp` 连 alpha 一起混 → 符号被抹掉，且"本帧无数据"的像素会**继承历史而永不失效** | 有效性**取自本帧**，只让 RGB 走时域累积 |
| 合成端 `Lighting/DeferredLighting.frag.slang` | 判 `a < 0` —— **无生产者** | 有生产者了（链路贯通） |

> 补充发现：**RT 反射侧其实早就写了 −1**（`RT_Reflection.rgen.slang:80,89`），
> 但它的时域降噪同样会把符号 lerp 掉 —— 也就是说 RT 反射的有效性协议此前也**不生效**，
> 本次一并修好。

**实测**（cfg 设为 **specular = IBL + SSR**、diffuse 只留 IBL、无 AO/阴影、关直接光）

| | 修复前 | 修复后 |
|---|---|---|
| 中心亮度 | 0.0833 | **0.0869**（+4.3%） |
| 背景亮度 | 0.0490 | **0.0557**（+13.7%） |
| 中心/背景 | 1.6988 | **1.5593**（更接近 1） |

方向与预测一致：**整体变亮，且背景受惠（+13.7%）明显大于中心（+4.3%）**——
这正是「SSR 无效像素不再以全权重把 IBL 压暗」的特征（背景像素更可能是 SSR 无效的）。
幅度小于朴素的 2× 预测，原因是空间降噪会把无效像素**从有效邻居填补**回来，
因此真正被排除的像素少于"所有 miss 像素"。比值从 1.70 降到 1.56 也说明屏幕上的反射更均匀了。

回归：白炉 **1.0000**；VUID 46 = 改前 46；单元测试 159/159、3952/3952。
*（`build/verify/ssr-before.log` 与 `ssr-after.log` 保留了这次 A/B）*

**L 的修复与实测**（SSGI 从未参与合成）

- 发现路径：为 P5 步骤 0 采集「单源」数据时，SSGI 独开的那一组与空栈基线**读数完全相同**
  （差值均值 −2.3e-6、正负像素各半——即纯噪声；而正确的 SSGI 输出**必然非负**）。
- 定位链（每一环都留下实测判据，避免停在猜测）：

  | 观测 | 据此排除的假设 |
  |---|---|
  | 合成端仅约 1% 像素有信号且正负对称 | 不是「贡献很弱」，是**结构性为零** |
  | 着色器累加项全部非负 ⇒ 正确输出必 ≥0 且随 `intensity` 线性；而 `ssgi_intensity` 放大 **1000 倍毫无变化** | 不是正常路径的输出 |
  | 向 shader 注入诊断通道后读到 `samples=0` 而 `intensity=1000` | 参数本身没送达（UBO 绑定、相机矩阵、采样核均已逐个排除） |
  | 改为**无条件常量输出**后纹理**仍是** `(0,0,0,1)` | 该 pass 只清屏，**draw 从未执行** |
  | 全仓库搜索：`SSGIProvider::SetGBuffer` 定义后**无任何调用点** | **根因确认** |

- 根因：帧图统一调用接口方法 `IGIProvider::SetInputs`
  （`DeferredPipeline_FrameGraph.cpp:430`），而 `SSGIProvider` 把该覆写命名成了 `SetGBuffer`。
  基类 `SetInputs` 带**空实现的默认体**，改名既不报错也不警告，只是静默落到空实现。
  其余需要 GBuffer 输入的 Provider（`SSRProvider` / `AOProvider` / `DDGIProvider` / `RTProvider`）
  都正确写了 `SetInputs ... override`——`SSGIProvider` 是**唯一**的漏网者，这也解释了为何
  同样走 Provider 抽象的 SSR 一直有效。
- 修法：改名为 `SetInputs` 并**显式加 `override`**（缺失 `override` 正是让改名静默通过的
  安全网缺口）。

实测（漫反射层栈分别为 空 / [DDGI] / [SSGI]，其余配置完全一致；读数用 §11.3 的采样设施）：

| | 修复前 | 修复后 |
|---|---|---|
| SSGI 原始输出纹理亮度 | 恒 0 | **3.94e-4** |
| `ssgi_intensity` 放大 1000 倍 | **无变化** | **精确 ×1000**（0.3935） |
| 对合成结果的正贡献像素占比 | 0.9%（噪声） | **53.5%** |

回归：单元测试 159/159、3952/3952；白炉比值与绝对值均 **1.0000**；
Vulkan 校验 46 条与改前**逐类一致**。

> 注意：L 修复后 SSGI 的量级仍比 DDGI 低 **34–68 倍**，且量纲仍是启发式（§3.4）——
> 本次只修「完全不生效」，标度问题归 §10 `SSGI-CAL`。

---

## 10. 后续任务与排序

排序原则：**先补验证能力，再动高风险主线；小项与调查项并行，不打断主线。**

| 顺位 | 任务 | 规模/风险 | 理由 |
|:---:|---|---|---|
| **P0** | ✅ **D2 · 抽 `GITypes.h` + 层栈/降级 CPU 单测**（**已完成**） | 小 / 低 | 守的是**已经咬过两次**的不变量 1；同时把 RHI 依赖从 GI 数据模型中剥离 |
| **P0** | **REDUNDANCY · 冗余源诊断 + 可证等价去重**（**已完成**） | 小 / 低 | 同时命中**性能**与**正确性**（§3.5）；静态可测，复用 P0 的单测设施 |
| **P1** | **§9.2 的 A/B/C/L 四项正确性缺陷** —— ✅ **全部已完成** | 中 / 中 | A（量纲统一）当时是 P5 的硬前置（§3.4），不过 **P5 随后被实测判定不需要**；B/C 修好了 SSR 的空间正确性与有效性协议；L 修好了 SSGI 完全不生效 |
| ~~**P2**~~ | ~~**P5 · 频率分离**（Wave 3）~~ —— ❌ **已判定不需要** | — | **步骤 0 实测否定了它的前提**（§3.3）：不存在使 `LowPass(SSGI) ≈ DDGI` 的低通尺度，两源本就同频段。**省下整个 Wave 3** |
| **P2** | **SSGI-CAL · SSGI 标度与量纲标定**（由 §9.2-L 与 §3.3 暴露） | 中 / 中 | L 修复后 SSGI 虽已生效，但仍比 DDGI 弱 **34–68 倍**，且 shader 不含入射辐射度项 ⇒ 归一化只给它约 **1.5%** 权重。**这是当前 GI 合成链上的主要缺口，也是 P5 退场后的接棒项** |
| **P2** | ~~**§9.2-L · SSGI 完全不生效**~~ —— ✅ **已修复** | 小 / 低 | Provider 覆写改名导致输入恒空；顺带说明「开关状态」不足以证明某源真的参与合成 |
| **P2** | **B3 · RSM VPL halfRes** | 小 / 低 | 与 P2 并行 |
| **P2** | **D1 · 偶发崩溃根因获证** | 未知 / 中 | 并行；根因未证意味着已修项可能只是其中一个实例 |
| **P3** | **AMORTIZE · 时间维分摊**（DDGI 每 N 帧更新 + 时域复用） | 中 / 低 | DDGI 已有 `blendAlpha` 历史混合，天然适配；把 256 探针的全量更新摊到多帧 |
| **P3** | **CULL · pass 级空间剔除**（tile / scissor） | 中 / 中 | 目前 `GPUCulling` **只服务 GBuffer 几何**，不服务 GI；GI pass 全是整幅执行（§3.5） |
| **P4** | **B4 / M5.2-A · DDGI 光追 march** | 中 / 中 | 只提升单一源质量；DDGI 低频兜底已由 M5.3 修正，紧迫性下降 |
| **P5** | **P6 · ReSTIR GI 统一估计器** | 大 / 高 | 长期 |
| **P5** | **Lightmap 源落地** | 中 / 低 | 按需（PC 实时路线可缓） |

> **顺位调整说明**：A/B/C 由原 P3 提前到 **P1**（因为它含 P5 的硬前置 A）；
> 新增三项——`REDUNDANCY`（P0，本次实现）、`AMORTIZE` 与 `CULL`（P3，性能）；
> 其余任务 ID 保持不变，仅顺位变化。
>
> **本次再调整**：**P5 频率分离经步骤 0 实测判定为「不需要」，整体退场**（§3.3），
> 空出的 P2 顺位由 **`SSGI-CAL`** 接棒——它原本被记为「独立任务、非 P5 前置」，
> 但 P5 的实测把结论反转了：真正的问题不是合成模式，而是 SSGI 自身的标度与量纲。
> 另新增 `§9.2-L`（已修复）作为「运行时数值对照」方法论的来源记入。

### 10.1 各项详情

**P0 · D2 层栈归一化 CPU 单测** —— ✅ **已完成**（36 用例 / 427 断言）
- 产出：`Engine/Render/GI/GITypes.h`（RHI-free，只依赖 `Core/Types.h`）+ `Tests/TestGITypes.cpp`
- **前置已澄清**：原述「`Tests` 不链接 Render 故 GI 无法单测」**不成立**（见 §9.1 第 7 行）——
  `HugEngineAI`(PUBLIC) 已传递引入 `HugEngineRender`。故本次的价值是
  **分层解耦**（断开旧 `GIConfig.h → LightingPass.h → RHI/RHI.h` 这条传导链）
  与**显式依赖**（Tests 显式声明 `Engine/Render` 包含路径，不再依赖传递隐式可得）
- 覆盖目标：源分类谓词的**互斥性与完备性**（3+5+3=11 无重叠无遗漏）、能力位与可用性、
  `GIChannelStack` 的增删改语义与容量上限、`GIConfig` 门控谓词**严格由层栈派生**（防影子开关）、
  四档预设的基线与精度、`GIRegistry::Degrade` 的逐源裁剪与每通道兜底、
  shader UBO 镜像结构的布局不漂移
- 边界说明：测的是 **C++ 侧配置/注册表/降级**逻辑，**不是 shader 里的合成数学**——对 P5 的保护有限

**P0 · REDUNDANCY 冗余源诊断 + 可证等价去重** —— ✅ **已完成**
- 位置：`Engine/Render/GI/GITypes.h`（`GIRegistry`）+ `Tests/TestGITypes.cpp` + 06.GILab 面板
- 动机（§3.5）：本架构要求每个启用的源都**整幅、每帧**产出完整通道缓冲，
  于是"启用了一个没有增益的源"等于**白付一份全量成本**。当前层栈**允许**这类配置且无任何提示。
- 检测三类问题（**只诊断，不强制改渲染行为**）：
  | 类别 | 判定 | 处置 |
  |---|---|---|
  | **严格冗余** | `SSAO + GTAO` 同通道——共用同一 pass、同一输出纹理 | 可**安全去重**：归一化平均把同一纹理值平均回自身，去重后逐像素等价 |
  | **重复估计** | `SSGI+RTGI` / `SSR+RTReflection` / `SSAO+RTAO`——同一物理量的多个逐屏幕像素估计 | **仅提示**：这是 S1.5 的有意设计，强制剔除会回退该决策 |
  | **相关性** | `RTGI + DDGI`——RTGI 的 miss 回退即 DDGI（§9.2-I） | **仅提示**：归一化失去无偏性 |
- 新增 API：`GIRegistry::Analyze()` / `IsStrictlyRedundant()` / `RTCounterpartOf()` /
  `IsEffectiveSource()` / `DeduplicateRedundant()`
- **顺带清理**：删除语义混淆的 `GIBand` 枚举与零调用的死接口 `IGIProvider::GetBand()`，
  改为 §2.2 的三个分类谓词
- 验收：诊断结果有单测覆盖；去重前后**逐像素等价**（配置层等价断言）；
  06.GILab 面板显示诊断行并带一键去重按钮
- 风险：低——纯静态分析，改动限于配置层

**P2 · P5 频率分离** —— ❌ **已判定不需要**，第 0 步即终止（结论与数据见 §3.3）

第 0 步（LowPass 选型小实验）本就是本项的**前置判定**，其结果直接终结了本项：

- 原计划的首选思路「用估计器自身的核做低通」没有走通——问题**不在低通算子选型**，
  而在被低通的那个信号（SSGI）本身就不逼近 DDGI。实测 `corr(LowPass(SSGI), DDGI)`
  随低通增强**单调下降**，最优即几乎不低通（0.2826 @ σ=1，与不低通的 0.2779 基本持平）。
- 两源频谱形状**几乎一致**（最低频段能量占比 96.00% vs 97.90%），属**同频段**；
  全掩码下看到的「SSGI 宽带」是它自身 0/非 0 覆盖边界造成的假象。
- 因此原第 1–4 步（`GIBlendMode::FrequencySplit`、`base + detail`、时域平滑、面板三模式 A/B）
  **全部不再实施**；`GIBlendMode::Additive` 继续作为「确有不重叠源」时的备用项保留。
- 落地方式：**不新增任何合成模式，保持现状的 `Normalized`**。
- 产出（可复用的验证设施）：单源做差 + 径向功率谱 + 频率域低通扫描，见 §11.3。

**P2 · SSGI-CAL SSGI 标度与量纲标定**（P5 退场后的接棒项）

- 动机：§9.2-L 修好后 SSGI 已真正生效，但实测（同一场景、单源层栈）**DDGI 比 SSGI 强
  34–68 倍**，且 `GI/SSGI.frag` 的累加式**只含命中点 albedo**：

  ```hlsl
  indirect += sAlbedo * max(0,dot(N,sDir)) * falloff;   // 没有入射辐射度项
  ```

  即它**不是 `E/π` 的估计**，而是「反照率相关性」启发式量。归一化加权平均的前提是各源估
  同一个量（§3.4），故现在 SSGI 只占输出值约 **1.5%**——开着、每帧付一整幅 pass 的成本，
  却几乎看不见。
- 需要做的两件事：
  1. 补入射辐射度项，使 SSGI 的输出语义成为 `albedo × E/π`；
  2. 以 PathTracing 为参考做实测标定，确定整体增益。
- 顺带应一并核查的两处**疑似**几何缺陷（本次未验证，属 L 的邻域）：
  - `sDir` 由**世界空间**法线构造（`TBN` 取自 `u_Normal`），却加到 **view 空间**的
    `viewPos` 上——两者不同空间却直接相加；
  - 可见性判据 `sZ >= sPos.z - 0.01` 在 `perspectiveRH_ZO`（view 朝 −Z）下方向相反，
    未遮挡样本的条件应为 `sZ <= sPos.z + eps`。实测 SSGI 的量级比其自身算术估算低约 30 倍，
    与该判定大量拒绝样本相符。
- 验收：单源亮度与 PT 参考同量级；`SSGI + DDGI` 归一化后两者贡献比接近权重比；白炉仍守恒。
- 风险：中——标定依赖 PT 参考的可比性。

**P2 · B3 RSM VPL halfRes** — 位置：`DeferredLighting.frag.slang` 的 `SampleRSMIndirect`（16 点 Poisson 盘 VPL 求和）

**P2 · D1 崩溃根因** — 长跑 soak + `HE_TRACE_FB=1`（`delay=0` 即同帧销毁的直接指标）；
现状：符号化落在 `VulkanTexture::GetImageView()` 野指针，最可疑根因（framebuffer 被同帧销毁）已修

**P1 · §9.2 A/B/C** —— ✅ **全部完成**（详见 §9.2 的「A/B/C 的修复与实测」）
- ✅ **A · 量纲统一**——约定 `L_o = albedo × E/π`，修 4 处（DDGI 补 `albedo/π`、
  RTGI/RSM 补接收面 albedo、`RT_GI.rgen` 的 miss 回退补 `1/π`），并解耦 RSM 对
  `iblIntensity` 的依赖。**单源亮度实测验证**。
- ✅ **B · SSR 投影矩阵**——矩阵进 UBO（用 `GI_SSR.cpp` 本就声明却未使用的 binding 3），
  正/逆分开传；push constant 96B → 32B。4 处用法全部正确。
- ✅ **C · SSR 有效性协议**——贯通**四处**：SSR.frag 写 −1、空间降噪保住符号、
  时域降噪的有效性取自本帧、合成端判定终有生产者。**顺带修好了 RT 反射侧**
  （它的 rgen 早已写 −1，但时域降噪把符号 lerp 掉了）。
- 遗留：`SSGI-CAL` —— SSGI 整体标度是启发式，未按 `E/π` 校准，需以 PT 为参考实测标定。
  原述「非 P5 前置」**已不成立**：P5 判定退场后它成为接棒项，已从「遗留」升级为 **P2**
  （见 §10 顺位表与 §10.1）。

**P1 · §9.2-L SSGI 完全不生效** —— ✅ **已完成**（详见 §9.2 的「L 的修复与实测」）
- `SSGIProvider` 把接口覆写命名成了 `SetGBuffer`，而基类 `IGIProvider::SetInputs` 带
  空实现默认体 → 输入恒为 `nullptr` → `GI_SSGI::Render` 提前返回 → SSGI 从未参与合成。
- 修法：改名 `SetInputs` 并显式加 `override`。
- 方法论收获：这是**唯一一项无法靠读代码或写单测发现**的缺陷——开关状态、`IsValid()`、
  面板显示全部正常，只有把各源的中间纹理读回来做数值对照才暴露。

**P3 · AMORTIZE 时间维分摊** — DDGI 探针更新（256 探针 × numSamples）由「每帧全量」改为
「每 N 帧 + 时域复用」。DDGI 已有 `blendAlpha` 历史混合，天生适配；需实测确认收敛速度可接受。
*（参考：UE 的 Surface Cache 明确是 "amortized over multiple frames"）*

**P3 · CULL pass 级空间剔除** — 给 GI pass 加 tile / scissor 剔除（视锥外、被遮挡的 tile 不 dispatch）。
现状：`GPUCulling` **只服务 GBuffer 几何**，GI pass 全是整幅执行（§3.5）。
注意：这**不能**用逐像素距离判断代替（距离是视角相关的，会引入接缝爬行，见 §3.5 与 §3.2）。

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
| `HE_DUMP_GI=<标签>`（+ `HE_DUMP_GI_FRAME=<帧号>`，默认 60） | **GI 纹理级采样**：在指定帧整幅落盘 HDR / GBuffer albedo / **各有效 Provider 的原始与降噪后输出**到 `Build/verify/gi_<标签>_*.f16`（RGBA16F 原始像素、无文件头、行紧密排布），并写 `_meta.txt` 记录逐目标尺寸；落盘后**自动关窗退出**，便于脚本化 |
| `HE_GILAB_CONFIG=<路径>` | 覆盖示例程序的配置读写路径（读写同一路径），使自动化实验**完全不触碰**仓库内的 `Content/Config/06_GILab.cfg`——否则每次实验都会被示例程序退出时回写覆盖 |
| `vk_layer_settings.txt` + `VK_LAYER_SETTINGS_PATH` | 关闭校验层重复消息上限，得到违规**真实次数** |

**GI 纹理级采样** —— 「某源是否**真的**参与合成」的唯一可靠判定手段

`HE_DUMP_GI` 会把每个有效 Provider 的中间纹理一起落盘。这是必要的：§9.2-L（SSGI 完全不生效）
在开关状态、`IsValid()`、面板显示上**全部正常**，只有把纹理读回来做数值对照才暴露出来。
**凡涉及"某源是否真的参与合成"，都以纹理级数值对照为准，不以开关状态为准。**

配套脚本（**已入库**，`Tools/gi/`）：

| 脚本 | 用途 |
|---|---|
| `Tools/gi/dump_gi.ps1` | 按漫反射层栈变体批量采样（`-Frame` 预热帧、`-Config` 构建配置） |
| `Tools/gi/analyze_gi.py` | 可比性校验（albedo 必须逐像素相同）+ 单源做差 + 量级与相关性 |
| `Tools/gi/p5_spectrum.py` | 径向功率谱 + 频率域低通扫描 + 互补高通相关性（P5 判定所用） |

采样时有两个易踩的坑：

- 用于做差的层栈配置**必须保留直接光**（`gi_solo=0`）。SSGI 是屏幕空间估计器，需要屏幕上有
  内容作为输入；把直接光全关、又不把 IBL 入栈会让屏幕全黑，此时 SSGI 与空栈基线的**读数都是 0**，
  实测上无法区分二者（这一度掩盖了 L 的真实成因）。
- 单源做差 `S_x = HDR(层栈=[x]) − HDR(层栈=空)` 能让直接光/天空/镜面**精确抵消**，得到该源在
  合成中真正被叠加的那一份；再除掉接收面 albedo 即得其自身估计量（做频谱比较时，全掩码会被
  源自身 0/非 0 的覆盖边界污染，需收紧到其覆盖内部，见 §3.3）。

**运行期产物**：`Content/Config/06_GILab_crash.log`（目录已 gitignore）、
`Build/bin/Debug/06.GILab_crash.dmp`、`Content/Config/06_GILab.cfg` 与 `06_GILab_imgui.ini`；
自动化实验的采样产物在 `Build/verify/`（目录已 gitignore）。

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
| P1 | 数据模型：`GIConfig` 层栈 + ~~`GIBand`~~ / `weight` / `falloffDistance` | ✅ | `105911b` |
| ↑ | *（`GIBand` 后于 P0·REDUNDANCY 删除——语义混淆了「尺度」与「精度」两个维度，详见 §2.2）* | | |
| P2 | 合成改归一化（保留 Additive 对照）+ 参数经 UBO 传递 | ✅ | `105911b` |
| P3 | 4 个单值枚举 → `GIChannelStack`；帧图门控与降级逐源化 | ✅ | `105911b` |
| S1 / S1.5 | 光追归入 Deferred（作为 GI 源）/ 两类源同时参与 | ✅ | `90649ba` `aac5690` `5c2b84b` |
| S2 / S3 | 管线维度收敛 / 移除 `HybridRTPipeline`（-1245 行） | ✅ | `06c8580` `0f8aca8` |
| PT | 参考渲染器定位 + 与 Deferred 共享加速结构 | ✅ | `3301040` |
| P4 | Provider 抽象，10 源全接入 | ✅ | `3a4075b` `8fc6091` `2b6b51a` `fd2510d` `48984e9` `51b8c98` `ea4620d` |
| ~~**P5**~~ | ~~**频率分离**~~ —— ❌ **已判定不需要** | ✅ **结论** | `bb55cd6` |
| ↑ | *（步骤 0 实测否定其前提「`LowPass(SSGI) ≈ DDGI`」——两源同频段、corr 仅 0.28，详见 §3.3）* | | |
| ↑ | SSGI 完全不生效修复（Provider 覆写改名，§9.2-L） | ✅ | `3266657` |
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
| ~~**Wave 3**~~ | ~~**频率分离**~~ —— ❌ **已判定不需要**，整波退场（§3.3） | ✅ 已判定 |
| Wave 4 余项 | M5.2-A DDGI 光追 march · B3 RSM halfRes · D2 CPU 单测 | ⏳ |
| **Wave 5** | **P6 统一估计器 · Lightmap 源 · D1 崩溃根因** | **⏳** |

### A.4 遗留任务批次

| 批次 | 内容 | 状态 |
|---|---|---|
| 第 1 批 | M4.4 · M4.5（不适用）· 面板候选派生 · 文档同步 | ✅ `42db644` |
| 第 2 批 | B4 DDGI 光追 march · B3 RSM halfRes · D2 CPU 单测 | ⏳ |
| 第 3 批 | ~~Wave 3 频率分离~~ —— ❌ 已判定不需要（§3.3）；顺位由 `SSGI-CAL` 接棒 | ✅ 已判定 |
| 第 4 批 | Wave 5（P6 / Lightmap / D1） | ⏳ |
