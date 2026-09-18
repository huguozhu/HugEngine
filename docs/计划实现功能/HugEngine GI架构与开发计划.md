# HugEngine GI 架构与开发计划

> **本文合并自**：《HugEngine GI分层合成架构设计.md》（架构设计）+《HugEngine GI优化开发计划.md》（开发计划）。
> 原两份文档中的历史里程碑与提交级演进记录已压缩为 **附录 A**；其余历史细节可由 git 历史取回。
>
> **本次更新依据**：对当前 HEAD 的代码实读复核（而非沿用文档声明）。凡文档与代码不一致处，
> 一律以代码为准并在 §9 集中列出。

---

# 第一部 · 架构设计

## 1. 目标与问题背景

让同一场景中的**多个 GI 源共同估计同一个物理量**（间接漫反射 / 间接镜面 / 环境光遮蔽），
由层栈决定「哪些源参与、各占多少权重」，再以**归一化加权平均**合成，从而**无双重计数**。

各源之间的真实区别是**估计器的作用域与成本**，不是频率：

| 作用域 | 源 | 特点 |
|---|---|---|
| **世界空间**（屏幕外同样覆盖） | IBL · Lightmap · DDGI | 与视角无关；DDGI 受探针网格尺度限制 |
| **屏幕空间**（只有屏幕上的信息） | SSGI · SSR · SSAO · GTAO · RSM | 逐像素、便宜；屏幕外与背面无数据 |
| **光追**（全场景可见性） | RTGI · RT Reflection · RTAO | 最精确、最贵 |

> ⚠️ 早期文档把这三类写成「**远场低频用探针、中频近处用屏幕空间、高频精确用光追**」。
> 该「频段分工」说法**已被废弃**，两处独立结论各自否定了它：
> **§2.2** —— 支撑它的分类枚举混了「尺度」与「精度」两个正交维度（`RTGI` 输出 1/4 分辨率、
> 空间分辨率低于 `SSGI`，却被标成「高频」），且把**同一个物理量**按不同"频段"标注，已整体删除；
> **§3.3** —— 「低频基底 + 高频残差」的频率分离经步骤 0 实测**判定不需要**（两源本就同频段）。
> 上表的「作用域」描述的是**估计器能看见什么**，不是频谱划分。

### 1.1 改造前的四个根本局限（历史）

| 局限 | 说明 |
|---|---|
| **表达力不足** | 「选一个」无法表达「多个源共同参与、各占多少权重」 |
| **合成错误** | 多源相加是**双重计数**（同一物理量算两遍 → 能量翻倍，白炉测试失败） |
| **抽象不统一** | SSGI/DDGI/SSR 各自为管线独立成员，新增 GI 需改管线代码 + shader + 面板 |
| **无「层」概念** | 没有作用范围、置信度这些**决定权重的物理量** |

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
    IBL = 1,            // 世界空间：环境辐照度 / 预滤波（同时服务 diffuse 与 specular）
    Lightmap = 2,       // 世界空间：烘焙光照（预留，尚未实现——见 §9）
    DDGI = 3,           // 世界空间：动态漫反射探针网格
    SSGI = 4,           // 屏幕空间：间接漫反射
    SSR = 5,            // 屏幕空间：反射
    SSAO = 6,           // 屏幕空间：环境光遮蔽
    RSM = 7,            // 屏幕空间：反射阴影贴图间接光（Forward 与 Deferred 共用）
    RTGI = 8,           // 光追：间接漫反射
    RTReflection = 9,   // 光追：反射
    RTAO = 10,          // 光追：环境光遮蔽
    GTAO = 11,          // 屏幕空间：地平线切片 AO（SSAO 的高质量替代）
};
```

两点设计取向：

- **ReSTIR 不进枚举**。当前 `ReSTIRPass` 实现的是 **ReSTIR DI（直接光照重采样）**，
  不是 GI；它由 `PathTracingPipeline` 独占驱动。等到 P6 统一估计器落地再作为源接入。
- **阴影不进 `GISourceId`**。阴影是**可见性（乘法项）**而非能量（加法项），
  不参与能量加权，也没有「距离让位」语义，用独立的 `ShadowChannel{None, Raster, RT}` 表达。

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
> **对 P5（频率分离）的影响**：不能拿「源分类」当频率边界做 `base + detail`——分类说的是
> **作用域**（估计器能看见什么），不是频率。
>
> 当时的后续推测是：唯一真实的空间尺度差只有一处——**DDGI 的探针网格尺度（`cellSize` 3 m）
> vs 屏幕空间/光追的像素尺度**；故打算把 P5 落在「基底 = DDGI，残差 = SSGI/RTGI 去掉该尺度
> 均值」上。**该推测随后被 §3.3 的步骤 0 实测否定**：任何低通尺度下 `LowPass(SSGI)` 都不逼近
> DDGI，两源本就同频段。**故 P5 已整体退场，本项目不再含任何「按频段分工」的成分**——
> 本节的三个谓词只用于分类、面板标签与配置诊断，不参与合成也不参与执行决策（§3.5）。

### 2.3 层描述与层栈

```cpp
struct GISourceDesc {
    GISourceId id              = GISourceId::None;
    float      weight          = 1.0f;   // 相对权重；0 = 不参与（等价于旧的 enabled）
    float      falloffDistance = 0.0f;   // 可选「距离让位」；0 = 不启用
};

struct GIChannelStack {
    static constexpr u32 kMaxSources = 4;    // 容量按「同时启用数」而非「算法总数」定
    GISourceDesc sources[kMaxSources];       // 有序：仅决定面板展示顺序，语义上无先后
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

| | 全掩码（92.8% 像素） | 收紧掩码（86.3% 像素） |
|---|---|---|
| DDGI / SSGI 量级比 | 21.0× | 20.8× |
| `corr(SSGI, DDGI)`（不低通） | **0.9238** | **0.9251** |
| `corr(LowPass(SSGI), DDGI)` 最优 | 0.9261 @ σ=1（**≈ 不低通**） | 0.9270 @ σ=1（**≈ 不低通**） |
| 最优标度拟合后 DDGI 未被解释的方差 | 37.7% | 37.5% |
| 最低频段（0–0.042 cyc/px）能量占比 | DDGI 99.74% / SSGI **99.38%** | DDGI 99.69% / SSGI 99.34% |

> **数据说明**：上表是**修复 §11.3.1 那两处缺陷之后**重测的。此前记录的一组数字
> （`corr` 0.15/0.28、量级比 67.7×/33.3×、未解释方差 99.6%/95.9%、SSGI 最低频段仅 40%）
> 是在 **DDGI 实际不做 GI**（其"贡献"只是 `albedo × 常数`）的状态下测得的，已作废。
> 结论方向不变，但**理由完全不同**——见下方两点判定。

两点判定：

1. **低通并不能让 SSGI 更好地逼近 DDGI**。`corr(LowPass(SSGI), DDGI)` 在 σ=1 处达峰
   （0.9261），随后**随低通增强单调下降**（σ=64 时降到 0.8556）。也就是说"最优低通"
   ​就是几乎不低通。**不存在任何低通尺度使 `LowPass(SSGI)` 比 `SSGI` 本身更像 DDGI**。
   这也回答了本项原先的首选思路「用估计器自身的核做低通」——问题不在低通算子选型。
2. **两源是同一频段，且高度一致（近乎冗余）**。最低频段能量占比 DDGI 99.74% / SSGI 99.38%，
   频谱形状**几乎相同**，相关系数 **0.92**，最优标度拟合后 SSGI 能解释 DDGI 约 **62%** 的方差。
   ⇒ 它们是**同一个量的两种互相印证的近似估计**，而不是互补的两块拼图。

**结论**：DDGI 与 SSGI 同频段、且**高度相关**（近乎冗余地估同一个平滑量）。这**正是**
§3.1 归一化的适用场景，即**现状已是正确的合成方式**——而且理由比原先更强：不仅"没有可分的
边界"，两源本身就几乎在说同一件事，分离出来的"高频残差"只会是噪声。三选一的落点：

| 关系 | 判定 |
|---|---|
| 同频段 / 互为替代估计 → 归一化 | ✅ **就是这一种（现状）** |
| 完全不相交 → 加法（`GIBlendMode::Additive`） | ✗ 当前没有此情形的源对 |
| 部分重叠 → 频率分离 | ✗ **无频段可分离** |

补充：`GIBlendMode::Additive` 保留为「确有不重叠源时才用」的备用项。注意**「加法」本身
已经实现了频段叠加**——两源频段不重叠时直接相加即可，无需额外的 `LowPass` 算子；频率分离
真正的用武之地仅在「两源在重叠区必须抑制双重计数」时，而这里没有可分的边界。

> **P5 顺带暴露出的真正阻塞点（不是合成规则）**：归一化下等权重时 SSGI 只占输出值约
> **4.5%**（= 1/(1+21.0)），同时把 DDGI 自身贡献**折半**——即 SSGI 每帧付一整幅屏幕空间
> pass 的成本，却几乎看不见（§3.5「白付一份全量成本」）。叠加 §3.4 的量纲问题，
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
> 主要缺口（§3.3）：标度未统一时，归一化把 SSGI 压到约 **4.5%** 的贡献——开着却几乎看不见。
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

> 这条差异对"以后接 Lumen"的完整含义（哪些是硬冲突、哪些已有先例、怎么改造）见 **§4.3**。

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

**「零侵入」实测结论（附限定条件）**：新增一种源、**且它落在已有 pass 类别覆盖范围内**时，
改动量为——实现 Provider（新文件）+ 注册 **1 行**；帧图 / UBO / 合成循环 / 层栈结构 **0 行**。
该承诺在 GTAO 与后续 10 源 Provider 化中均获验证。

> ⚠️ **限定条件**：「0 行」只在**同类别内**成立。帧图目前有 **7 条按 source id 定制的循环**，
> 且纹理绑定是「每个源一个具名字段 + 一个 shader `case`」——**引入一个"新类别"的源
> （如 Lumen）需要新增循环与绑定，不是 0 行**。详见 §4.3。

### 4.3 执行单位与跨通道耦合

> 本节由「以后接 Lumen 会不会与现架构冲突」这一问题引出。**结论：合成端不冲突
> （单元素栈已是精确直通），冲突在 Provider 的"执行单位"与"纹理绑定"两处。**

#### 4.3.1 现状：执行单位是「Provider × 通道」，不是「Provider」

帧图里共有 **7 条按 source id 定制的循环**（`DeferredPipeline_FrameGraph.cpp`）：

| 行 | 循环守护的 id | pass 形状 |
|---|---|---|
| L315 | `RSM` | 全屏 offscreen（光源视锥光栅化） |
| L334 | `DDGI` | compute，无通道纹理输出 |
| L354 | `SSAO` 或 `GTAO` | 全屏 offscreen（AO 通道） |
| L379 | `SSR` | 全屏 offscreen（镜面通道） |
| L426 | `SSGI` | 全屏 offscreen（漫反射通道） |
| L500 | `RTEffectProvider` 四实例 | RT dispatch（一次 `dynamic_cast`，再按 `GetSourceId()` 选通道输出） |
| L582 | `IBL` | `Custom`：`rg.AddPass("IBL_Bake", {}, {}, ...)`，无 RG 资源，脏时重建 |

每条形如
`for (prov : m_GIProviders) { if (!prov->Handles(<某个 id>)) continue; ... rg.AddPass(...); prov->Render(...); }`。

**后果**：若一个 Provider 同时 `Handles(SSGI)` 与 `Handles(SSR)`（Lumen 的天然形状），
L379 与 L426 两条循环会**各注册一个同名 pass、各调一次 `Render`** ⇒ 追踪跑两遍、
两条时域历史分裂、屏幕 trace 与 radiance cache 无法复用。**那样接入 Lumen，
相对「SSGI + DDGI + RTGI 各付一份」几乎没有优势。**

而且会**在 RenderGraph 里直接撞名**：两条循环都用 `prov->GetName()` 作为
`rg.ImportTexture(...)` 的纹理名与 `rg.AddPass(...)` 的 pass 名 —— 同一个 Provider 就会被
导入两次、注册两个同名 pass。这不是性能问题，是**帧图当前表达不了这个形状**。

#### 4.3.2 另一半：纹理绑定是「每源一个具名字段 + 一个 shader case」

权重侧**已经是数组**（`GIChannelStack::sources[4]` + 合成循环里的 `blend.sources[i]`），
但纹理侧**仍是硬编码**：`LightingInputs` 每个源一个具名字段
（`ssaoTex` / `ssgiTex` / `ssrTex` / `rtGI` / `rtAO` / `rtReflection` / `rtShadowMask` /
`rsmPositionMap` / `rsmFluxMap`；IBL 另走专用的 `SetIBLTextures`），
`SampleDiffuseSource(id)` / `SampleSpecularSource(id)` 里每个源一个 `case`。

⇒ **「层栈里放哪个源」是数据，「那个源的纹理在哪」是代码。** 这正是 §4.2 那句
「帧图 0 行」需要加限定条件的原因。

**一个现成的实例（AO 通道走半条旁路）**：帧图里 AO 的输出既按 Provider 导入
（L358-360 `prov->GetAOOutput()` → `"AO_Output"`），**又在喂给合成端时绕过 Provider 直接取模块成员**
——`L661 in.ssaoTex = m_SSAO.GetAOTexture()`（而同通道的 RTAO 走的是 `L706 in.rtAO = rtAOTex`，
即 Provider 路径）。也就是说 **AO 通道目前是"直接访问 + Provider 路径"混用**：
若将来把 SSAO 换成另一个 AO Provider，合成端仍会读 `m_SSAO` 的纹理。
这也意味着合并循环时 **AO 那条不能与 specular/diffuse 同等对待**（见 §4.3.5 迁移策略）。

另：`IGIProvider::GetPassKind()` 这个本该用于"帧图据此选择注册方式"的调度声明，
**实际是死接口** —— `IGIProvider.h:63` 声明（默认 `Offscreen`）、`DDGIProvider.h:34`
覆写为 `Compute`，**全仓无任何读取点**（§9.1 第 9 行）。

#### 4.3.3 可照抄的模板就在代码里：`IBLProvider`

`IBLProvider` **已经实现了** Lumen 需要的全部三点：

| Lumen 的"不合群"之处 | IBL 的现有做法 |
|---|---|
| 一个源填**两个通道** | `GetDiffuseOutput()` → 辐照度；`GetSpecularOutput()` → 预滤波；**同一个 pass** |
| **非每帧**的摊销阶段 | `Render()` 内 `if (m_IBL->IsDirty())` 脏时重建 |
| 输出**不是通道纹理**的 pass | `rg.AddPass("IBL_Bake", {}, {}, ...)` |

而 `GISourceId::IBL` **本来就同时出现在 diffuse 与 specular 两个层栈里**（§5.2 每档预设皆然）。
**所以「同一个源 id 出现在多个通道 → 解析到同一个 Provider 实例 → 只跑一次 pass →
产出多个通道的输出」这个形状已经跑通了。** 需要做的不是发明它，而是把它从「IBL 特例」
提升为通用路径。

`RTEffectProvider`（L500）是另一个半成品：**一次 `dynamic_cast`，再按 `GetSourceId()`
选出该用哪个通道的输出**（L517-519），是通用分发的雏形。

#### 4.3.4 对照 UE：它用「同一系统出现在两个槽位」表达耦合

UE 的 `r.DynamicGlobalIlluminationMethod` 与 `r.ReflectionMethod` 是两个独立的「选一个」枚举，
但选 Lumen GI 时 `URendererSettings::PostEditChangeProperty` 会**自动把反射也设为 Lumen**
并弹框告知；Lumen 内部用
`ELumenIndirectLightingSteps = ScreenProbeGather | Reflections | StoreDepthHistory | Composite`
表示 GI 与反射是**同一系统的两个阶段**，由单一入口
`RenderDiffuseIndirectAndAmbientOcclusion` 一起产出。

**关键：UE 不是靠「一个 provider 填两个通道」，而是靠「两个槽位指向同一个系统」。**
HugEngine 的等价表达更简单——**两个通道的层栈里出现同一个源 id**。

**不学 UE 的部分**：不要改成「每槽位选一个」。单元素栈时 `Σ(c·w)/Σw ⇒ num/den = c`
已是**精确直通**，**「选一个」本就是「加权平均」的特例**；换过去是纯损失表达力，
而这个表达力正是 §1.1 声称要从那类模型里挣回来的东西。另外 UE 的方法枚举还带来锁死
（前向着色下 GI/反射/阴影三个方法**都不可编辑**；选 Lumen GI 强制 Lumen 反射），
正是本架构要避免的。

#### 4.3.5 改造分三层（代价从小到大）

| 层 | 内容 | 代价 | 性质 |
|---|---|---|---|
| **一** | 把 IBL 的形状写成**显式契约**：一个 Provider 可占多个通道；帧图保证每 Provider 每帧只注册一次 pass；「服务哪些通道」由 `GetDiffuse/Specular/AOOutput()` 是否非空表达 | **0 行代码**（注释/文档） | 立刻暴露 §4.2 的限定条件 |
| **二** | 帧图从「按通道 7 条循环」→「**按 Provider 1 条循环**」；pass 形状按已被声明却零消费的 `GetPassKind()` 选择 | 中 / 集中在帧图一个文件 | **整洁性收益，不是硬前置**（见下方"逃生口"） |
| **三** | 纹理绑定**数组化**：`LightingInputs` 具名字段 + shader per-id `case` → 每通道一组源纹理，按槽位索引取样 | 大（UBO + 描述符 + shader + 合成循环） | **真正的泛化**；唯一能同时让 Lumen 与 P6 不以特例形式落地的改动 |

> **逃生口（重要）**：**第二层并不是解锁 Lumen 的必要条件。** 因为帧图在自己手里，
> 随时可以像 IBL / RSM / RT 那样**给 Lumen 加第 8 条定制循环** —— 那条循环只认
> `Handles(Lumen)`，diffuse 循环（认 `SSGI`）与 specular 循环（认 `SSR`）都不会命中它，
> **Lumen 的 pass 因此只注册一次，不存在双跑问题**。
> 所以第二层的价值是**消掉 7 条循环的重复**（可维护性），不是"解锁"。
> 真正让 Lumen 不以特例形式落地的是第三层。

**明确不做**：
- 不加 `GetChannelMask()` —— "我服务哪些通道"已经由三个 `GetXOutput()` 是否非空表达；
- 不给 `GIChannelStack` 加 `primary` / `exclusive` 标志 —— 单元素栈已经是直通；
- 不改合成公式。

#### 4.3.6 可纯配置层做的一条（零渲染风险）

UE 用「设置间约束」表达耦合；HugEngine 的对应物是在 `GIRegistry::Analyze()` 加一条诊断：

> **某 Provider 服务多个通道，但只有部分通道的层栈里含它 ⇒ 它仍会整幅跑一次，另一半成本白付。**
> 建议两个通道要么都启用、要么都禁用。

这是 UE 那个弹框的等价物，且完全落在已有诊断框架内。

> **外部依据**：UE 的 `r.DynamicGlobalIlluminationMethod` / `r.ReflectionMethod` /
> `r.AmbientOcclusion.Method` 及其 help 原文；`RenderDiffuseIndirectAndAmbientOcclusion`、
> `ELumenIndirectLightingSteps`、`URendererSettings::PostEditChangeProperty` 的联动逻辑。

### 4.4 降噪现状与统一框架（待改造）

> 本节回答「统一降噪框架和现在的实现有什么不同」。一句话概括：
> **现在 = 两个通用滤波器类 + N 个手工拼装点；目标 = 一个按「信号类型」分派的降噪器 + N 份配置。**
>
> 它由 `IGIProvider` 的**附属 pass 机制**交付（`GetAuxPassCount/Name/Input/Output` +
> `RenderAux`），因此与 §4.3 属同一层的架构问题。

#### 4.4.1 现状：两个互不相关的类，9 个实例

| 类 | 算法 | 实例 | 数量 |
|---|---|---|---|
| `Denoiser` | 空间 5×5 双边（`PostProcess/Denoise.frag`） | `m_DenoiseSSGI` · `m_DenoiseSSR` · `m_ReflectionSpatial` · `m_GISpatial` | **4** |
| `RTDenoiser` | 时域累积（`PostProcess/RT_DenoiseTemporal.frag`） | `m_ShadowDenoiser` · `m_AODenoiser` · `m_ReflectionDenoiser` · `m_GIDenoiser` · `m_PTDenoiser` | **5** |

⇒ **9 套 PSO、9 套描述符集、9 个点采样器、14 张纹理**
（时域实例各 2 张 history/output，空间实例各 1 张；分辨率随源而不同）。

两个类的接口差异：

| | `Denoiser` | `RTDenoiser` |
|---|---|---|
| 输入 | `color, depth, normal` | `noisyColor, depth, normal, velocity` |
| 参数 | **硬编码**：`pc.dS = kDefaultDepthSigma(10)`、`pc.nS = kDefaultNormalSigma(8)`，**没有任何 setter** | `Config{ temporalBlend, depthThreshold, normalThreshold, format, width, height, debugName }` |
| 历史 | **无**（纯空间） | `m_History` + `m_Output`，`Render` 末尾 swap 角色 |
| 输出 | `m_Denoised` 单张、语义稳定 | `GetOutput()`——**swap 后语义变**，调用方必须理解该约定 |

#### 4.4.2 六处「不统一」

1. **没有「信号类型」这个概念。** UE 的 `ESignalProcessing` 含
   `AmbientOcclusion` / `Reflections` / `DiffuseAndAmbientOcclusion` / `ShadowVisibilityMask` /
   `ScreenSpaceDiffuseIndirect` / `IndirectProbeHierarchy` / `DiffuseSphericalHarmonic` ——
   **一套框架按信号类型选滤波器、核、重建与升采样策略**。这里只有两种**通用**算法，
   信号语义完全由调用方在外部拼装。佐证：**4 个 `Denoiser` 实例的参数完全相同**（无 setter），
   即同一个 σ 核被用在**漫反射间接光**与**镜面反射**这两个噪声分布与可容忍模糊度都不同的信号上。
2. **链条形状写在调用方的 if/else 里，不是数据。** `RTProvider` 手工持有 `m_Temporal` +
   `m_Spatial` 两个指针，并用**位置约定**表达顺序：

   ```cpp
   [[nodiscard]] rhi::IRHITexture* GetAuxPassInput(u32 i) {
       return IsTemporalIndex(i) ? MainOutput() : TemporalOrMain();   // 时域=0、空间=1
   }
   void RenderAux(cmd, i, ...) {
       if (IsTemporalIndex(i)) { m_Temporal->SetInputs(MainOutput(), ...);    m_Temporal->Render(cmd); }
       else if (m_Spatial)     { m_Spatial->SetInputs(TemporalOrMain(), ...); m_Spatial->Render(cmd); }
   }
   ```

   再加一级滤波就得改这段。而 `SSGIProvider` / `SSRProvider` 又各自复制了一份几乎逐行同构的
   `SetInputs + Render`。
3. **历史与资源各自管理**：9 个实例各自 `CreateTexture`、各自 `OnResize`、各自建采样器。
4. **没有批量 dispatch**：UE 的 `FScreenSpaceDenoiser` 可把多个信号打进**同一个 dispatch**
   （`DenoiseGroup` / `CommonSettings`）；这里 4 个 RT 效果就发 4 组（每组 1–2 pass），
   **降噪成本随信号数线性增长**——与 §3.5 的「N 份全量成本」是同一个病。
5. **有效性协议没有框架级契约**，每个 shader 各写一遍。§9.2-C 的协议曾在**四处**断裂
   （`SSR.frag` 写 1.0、空间降噪把 alpha 一起平均、时域降噪把符号 `lerp` 掉、合成端判定无生产者）
   正是这个原因。框架化后「有效性」应成为一个**显式的、所有降噪器统一遵守的类型**，
   而不是每个 shader 作者必须记得的约定。
6. **半分辨率时完全跳过降噪**：`SSGIProvider::AuxActive()` 在 `halfRes` 时返回 false
   ⇒ 半分辨率输出被直接采样。这是被「固定 5×5 双边 + 双线性上采样会糊」逼出来的取舍；
   根因是**缺少「需要重建升采样」这一信号属性**（UE 用 `SignalSupportsUpscaling` 表达它）。
   **这是本项唯一现在就成立、不依赖 Lumen 的画质收益。**

#### 4.4.3 目标形状

```cpp
enum class DenoiseSignal { ShadowMask, AO, DiffuseIndirect, Reflection, ProbeIrradiance };

struct DenoiseRequest {
    DenoiseSignal     signal;
    rhi::IRHITexture* noisy;
    bool              hasHistory;    // 是否需要时域
    bool              needsUpscale;  // 是否半分辨率到全分辨率重建
    // 其余参数按信号类型取默认值，可覆盖
};

// 一次提交多个请求；框架负责：统一分配历史、按时域到空间排序、批量 dispatch，
// 并把「有效性」作为契约贯穿始终
void Denoise(rhi::IRHICommandList* cmd, std::span<const DenoiseRequest> requests);
```

#### 4.4.4 切入路径：分三步，前两步不必等消费方

| 步 | 内容 | 代价 | 即时收益 / 判据 |
|---|---|---|---|
| **11.1** | 把 `SSGIProvider` / `SSRProvider` 的重复合并成一个共享实现；顺带把 `Denoiser` 的 `depthSigma` / `normalSigma` 变成**可配置** | 小 / 低 | **有**：修掉「参数硬编码」（现在两个语义不同的信号用同一个核）。判据：背靠背单源采样逐项一致 |
| **11.2** | 让 `RTProvider` 的降噪链**变成数据**（用 `std::vector<stage>` 取代两个指针 + 索引约定） | 小 / 中 | 纯去重：加第三级滤波不必改框架。判据同上 |
| **11.3** | **按信号类型分派**（`DenoiseSignal` + 统一历史分配 + 批量 dispatch + 框架级有效性契约） | 中 / 大 | **需要消费方**（Lumen / P6 / 多信号共存）才能验证抽象选型是否对 |

> **判断**：**11.1 与 11.2 值得提前做**（纯去重、判据现成，且 11.1 顺带修掉参数硬编码）；
> **11.3 应等消费方**——否则就是在猜该有哪些信号类型、每种信号要什么核。
> 「半分辨率也降噪」依赖 `needsUpscale` 这一信号属性，属 **11.3**，不是 11.1。

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
| **P1 / M·N·O** | **SSGI 采样方向与可见性**：TBN 变换方向、世界/view 空间混用、可见性判据方向；顺带修掉 SSGI 侧的 §9.2-E（改用真实相机投影） | ✅ 完成 |
| **P2 / P5** | 频率分离 —— 步骤 0 实测**判定不需要**（两源同频段，`LowPass(SSGI) ≈ DDGI` 不成立） | ⛔ 已判定退场 |
| **P1 / Q·R** | **IBL 图未烘焙（未初始化显存被采样）+ DDGI 的 `useRSM` 闩锁** —— 两者叠加使 DDGI **表面正常却完全不做 GI**；已修，并给 RSM 路径加了逐样本回退。定位过程与前后数据见 §11.3.1 | ✅ 完成 |
| **P1 / S** | **RHI 纹理「已写入」登记 + 一次性告警**：把「采样了从未写入的纹理」从静默错误变成一条带 binding/尺寸/格式的日志。覆盖 5 类写路径，补齐两处视图登记缺口，带累计阈值滤掉「同帧先采样后写入」的假阳性。首次跑出固定基线 4 条（§9.2-T，待任务 23 清零） | ✅ 完成 |
| **P1 / T** | **未产出的效果改绑中性占位纹理**（§9.2-T）：立「pass 注册结果」为单一真值 + 帧图按值捕获 + 门控通道回绑中性占位；占位改成员、RSM 占位由白改黑。告警基线 **4/4/3 → 0/0/0**，「是野指针」约 **186 → 0**；DDGI 差分贡献不变。同时发现 §9.2-U（绝对读数随二进制布局漂移） | ✅ 完成 |

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
| 8 | §4.2「帧图 0 行」 | 新增一种源，帧图 0 行 | **只在"落在已有 pass 类别内"时成立**。帧图实有 **7 条按 source id 定制的循环**（§4.3.1），引入新类别需新增循环。已在 §4.2 就地加限定条件 |
| 9 | `IGIProvider::GetPassKind()` | §4.1 把它列为「调度」：决定帧图如何注册本源 pass | **实际是死接口**：`IGIProvider.h:63` 声明（默认 `Offscreen`）、`DDGIProvider.h:34` 覆写为 `Compute`，**全仓无任何读取点**——帧图实际按 source id 硬编码选择 pass 形状（§4.3.2） |

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
| **E** | 中 → **SSGI 已修** | **屏幕空间源用硬编码默认投影矩阵**而非真实相机：`kDefaultFOV=60°/0.1/2000`；`PhysicalCamera` 会由焦距反算 fov → 非默认相机下 SSGI/SSAO/SSR 重建错位。根因是 `IGIProvider` 未把相机传给屏幕空间源（只有 DDGI 有 `SetCamera`） | SSGI 已修（见 M）；`GI_SSR.cpp:140`、`SSAO.cpp:271` **仍待修** |
| **F** | 中 | **RSM 的 pass 被嵌套在 DDGI 门控内**：单独勾选 RSM 而关闭 DDGI 时，RSM 永不注册（Forward 侧却是独立的 `ShouldRunRSM()`） | `DeferredPipeline_FrameGraph.cpp:288` |
| **G** | 中 | **层栈与子系统开关是两套真值**（不变量 1 的实际状态）：`IsValid()` 只读子系统 `enabled`，面板层栈 UI 只改层栈 → 勾选但静默失效。`halfRes` 还有第三重（需触发 `OnResize` 才重建纹理） | `SSRProvider.h:25` 等 + `06.GILab.cpp` 的通道 UI |
| **H** | 中 | **Provider 抽象只在 Deferred 落地**：`ForwardPipeline` 无 `m_GIProviders`，且 Forward 的 PBR shader **没有 `GIBlendParams` UBO** → 层栈归一化在 Forward 完全不存在，但 `PipelineCaps::Forward` 声明支持 IBL+RSM | `ForwardPipeline.h`；全仓 `GIBlendParams` 仅 DeferredLighting 使用 |
| **I** | 中 | **RTGI 用 DDGI 做 miss 回退**，破坏「源独立」前提：Ultra 档同时含 RTGI+DDGI 时，DDGI 信息被用两次再归一化 → 加权平均失去无偏性 | `RT_GI.rgen.slang:111-117` + `RTProvider.h:243` |
| **J** | 低 | 合成参数 UBO 是**单份**、非 per-frame-in-flight（`MAX_FRAMES_IN_FLIGHT=3`） | `LightingPass.cpp:155-168` |
| **K** | 低 | **DDGI 网格外查询退化为「贴边常数外推」**，无 falloff 或无效标记；探针网格为固定参数，覆盖不到的区域静默缺失低频 GI | `RT_DDGI.slang:56-63,98`；`GI_DDGI.h:57-59` |
| ~~**L**~~ | ✅ **已修复** | **SSGI 完全不生效**：`SSGIProvider` 把接口覆写写成了 `SetGBuffer`，而基类 `IGIProvider::SetInputs` 带**空实现的默认体** → 改名既不报错也不警告，静默落到空实现 → `m_Depth/m_Normal/m_Albedo` 恒为 `nullptr` → `GI_SSGI::Render` 在守卫处提前返回，SSGI 输出纹理只剩清屏值 `(0,0,0,1)`。表现为「SSGI 已启用、`IsValid()` 为真、面板一切正常，但对画面的贡献恒为 0」 | 见下方「L 的修复与实测」 |
| ~~**M**~~ | ✅ **已修复** | **SSGI 的 TBN 变换方向反了**（L 修好后暴露的主因）：`mul(TBN, 样本)` 算的是 `(T·v, B·v, N·v)`——把切线空间样本**投影到** TBN 轴上，而非变换到本空间。结果采样方向几乎与法线垂直，半球采样失效 | 见下方「M/N/O 的修复与实测」 |
| ~~**N**~~ | ✅ **已修复** | **SSGI 世界/view 空间混用**：`sDir` 由 GBuffer 的**世界空间**法线构造，却被加到 **view 空间**的 `viewPos` 上（`sPos = viewPos + sDir × radius`）。同时属 §9.2-E：SSGI 用硬编码默认 FOV/near/far 自拼投影矩阵 | 同上 |
| ~~**O**~~ | ✅ **已修复** | **SSGI 可见性判据方向相反**：view 空间朝 −Z，未被遮挡应为 `sZ <= sPos.z + bias`，原写作 `sZ >= sPos.z - 0.01`，等于只累积**被遮挡**的样本 | 同上 |
| **P** | 中 | **SSGI 的累加项不是入射辐射度**：`indirect += sAlbedo × max(0,dot(N,sDir)) × falloff` 只用命中点的**反照率**，不含任何 `L_in`；且余弦项用未归一化的 `sDir`，把 cos 项与样本长度混在一起。故 SSGI 至今不是 `E/π` 的估计（§3.4） | `GI/SSGI.frag` 累加行；归 §10.1 `SSGI-CAL` |
| ~~**Q**~~ | **高** → ✅ **已修复** | **IBL 的辐照度/预滤波图在「IBL 不在漫反射层栈」时从不烘焙**，而帧图仍把它们交给 specular 通道与 DDGI 探针使用 ⇒ **未初始化显存被当作光照数据采样**。门控用的是 `IBLProvider::NeedsPass(m_GIConfig.diffuse)`，但消费者有三个（diffuse / specular / DDGI 的辐射度回退 `GI_DDGI::SetIBL`） | 见 §11.3.1 的定位与修复 |
| ~~**R**~~ | **高** → ✅ **已修复** | **DDGI 的 `useRSM` 是只置位、永不清除的闩锁**：帧图把 RSM 的 position/flux 图交给 DDGI 是**无条件**的，而 RSM pass 的注册条件是「RSM 在漫反射层栈里」⇒ 在「DDGI 开 + 有活动阴影 + RSM 不在漫反射栈」时，DDGI 永久走**从未渲染**的 RSM 路径，全部探针样本无效，落入着色器硬编码兜底 —— **DDGI 表面正常却完全不做 GI** | 同上；判定指纹：贡献的 R:G:B 恰为 1:1.5:4（= 兜底常数 `(0.02,0.03,0.08)`） |
| ~~**S**~~ | ✅ **已修复** | **RHI 允许纹理以未初始化状态被采样**：没有任何默认零初始化或有效性标记，于是一个「消费者门控写漏」就能被放大成**静默的物理错误 + 跨构建不可复现的读数**（Q/R 两项正是这样被放大的）。修复采取**检测 + 一次性告警**而非改写纹理内容（见下方「S 的修复与实测」） | `Engine/RHI/RHI/TextureLayoutTracker.{h,cpp}`；`VulkanDevice_Descriptors.cpp`；`VulkanCommandList.cpp`；`VulkanCommandList_RenderPass.cpp`；`VulkanResources.{h,cpp}` |
| ~~**T**~~ | ✅ **已修复** | **「效果本次未产出」时描述符仍绑定真实纹理**：`LightingPass` 无条件把 RSM 位置/通量图、SSGI 输出、聚光灯阴影图等绑到 set=0，而这些 pass 的门控是「是否在层栈里 / 是否有该类光源」⇒ 这些描述符指向**从未被写入**的纹理。当前无害（着色器按权重与光源数自行守卫），但只要守卫被改动就会读到未初始化显存 —— 正是 S 所描述的危险形态。**由 S 的检测机制在 06.GILab 首次跑出**，固定 4 处。修复见「T 的修复与实测」 | `LightingPass.cpp`；`Shadow/IShadowSystem.h` + `ShadowSystem`；`DeferredPipeline_FrameGraph.cpp` |
| **U** | 中 | **绝对读数依赖二进制布局（根因未定位）**：两个只差「一处 lambda 捕获列表」的构建，`none` 层栈的绝对基线给出 **0.0327534** 与 **0.0472113**（相差 44%，`max` 5.94 与 19.68），各自在多次运行中稳定复现。已排除：门控通道、阴影、IBL 绑定、以及"把从未写入的真实纹理绑回去"（见「T 的修复与实测」末段的排除表）。**差分**量（单源做差、相关系数、频谱占比）不受影响。已确认并修掉的一个实例是帧图 Lighting lambda 的失效捕获（`BuildFrameGraph` 返回后读栈帧，实测每次运行约 186 条「是野指针」）；其余来源尚未定位 | `DeferredPipeline_FrameGraph.cpp`（已修一处）+ 待定位来源；归 §10.1 任务 24 |

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

> 注意：L 修复后 SSGI 虽已生效，但量级仍远低于 DDGI，且量纲仍是启发式（§3.4）——
> 几何与可见性缺陷见下方 M/N/O，标度与量纲归 §10 `SSGI-CAL`。

**M/N/O 的修复与实测**（SSGI 的采样方向与可见性判定）

L 修好后 SSGI 终于参与合成，但量级比 DDGI 低两个数量级。用「向 shader 注入诊断通道、
把中间量读回来」逐项排查，找到三处几何错误——**每一处都不是靠读代码能看出来的**：

| 编号 | 缺陷 | 判定依据 |
|---|---|---|
| **M** | **TBN 变换方向反了**（主因）：`mul(TBN, 样本)` 算的是 `(T·v, B·v, N·v)`，把切线空间样本**投影到** TBN 轴上；正确写法是 `mul(样本, TBN)`（HLSL 的 `float3x3(T,B,N)` 把 T/B/N 当**行**） | 诊断通道读到 `dot(N,sDir)` 均值仅 **0.012**，而正确的半球采样应在 0.5 量级 |
| **N** | **世界/view 空间混用**：`sDir` 由世界空间法线构造，却加到 view 空间的 `viewPos` 上。修法是给 UBO 补 `u_View`（world→view）把法线转到 view 空间；顺带修掉 §9.2-E（SSGI 此前用硬编码默认 FOV/near/far 自拼投影矩阵，而深度图是用真实相机渲染的） | 代码实读 + 修后产出变化 |
| **O** | **可见性判据方向相反**：view 空间朝 −Z，未被遮挡应为 `sZ ≤ sPos.z + bias`，原写作 `sZ ≥ sPos.z − 0.01`，等于只累积被遮挡的样本 | 只翻转符号即令产出 ×3.0 |

分步 A/B 实测（每步只改一项，单源层栈做差，Frame=120）：

| 状态 | SSGI 原始纹理亮度 | 合成贡献 | 正贡献像素 | 纹理非零像素 |
|---|---|---|---|---|
| 起点（仅 L 已修） | 3.935e-4 | 1.791e-4 | 53.4% | 82.6% |
| 只修 O | 1.190e-3 | 5.554e-4 | 93.1% | 99.2% |
| 修 O + N（M 未修） | 9.436e-4 | 4.450e-4 | 70.1% | 89.8% |
| **M + N + O 全修** | **2.052e-3** | **9.356e-4** | **99.5%** | **99.997%** |

> **关于原表的「DDGI/SSGI」末列**：该列已删除。这几轮测量期间 DDGI 正处于 §9.2-R 的失效状态
> （其"贡献"只是 `albedo × 常数`），因此那一列只是「常数 ÷ 本表第 1 列」的单调变换，
> **不携带任何独立信息**。修复 §11.3.1 那两处缺陷后重测，该比值为 **21.0×**（§3.3）。
> 上表前四列只涉及 SSGI 自身，是同一环境下背靠背测出的相对变化，**不受影响**。

> **一个反直觉但重要的现象**：单独修 N 反而让输出**变小**。因为 N 转到 view 空间后，
> 看向墙壁时法线的 z 分量接近 0，而缺陷 M 恰使 `dot(N,sDir)` 等于 `N.z` 乘 `s.z`。
> **M 与 N 必须同时修**——这也说明「某个改动让指标变差」不等于该改动是错的，
> 需要先判断它与其它缺陷是否耦合。本次因此把 M 与 N 放进同一条提交。

回归：单元测试 159/159、3952/3952；白炉比值与绝对值均 **1.0000**；
Vulkan 校验 46 条与改前一致。

> 遗留：三处几何缺陷修完后，SSGI 仍明显弱于 DDGI（**21.0 倍**，修复 §11.3.1 后重测），
> 且累加项用的仍是命中点 **反照率**而非入射辐射度（§9.2-P）。**这两件事不是
> 同一类问题**：前者是几何/方向错误（本次已修），后者是量纲缺失，必须补 `L_in` 并做实测
> 标定才能解决——归 §10.1 `SSGI-CAL`。在补齐之前，SSGI 无论怎么调 `intensity` 都不是
> `E/π` 的估计。

> ⚠️ **校验层条数只在同一配置内纵向比较。** 本节及后面各处出现的「46 / 49 / 51 / 54 条」
> 分别来自不同运行配置（默认配置 / 白炉 / §11.3 采样设施的三个层栈变体），绝对值**不可横向
> 比较**；有效的判据始终是「与**同配置**的改前基线**逐项相同**」。

**S 的修复与实测**（RHI 纹理「已写入」登记 + 一次性告警）

S 有两个可选修法：**默认零初始化**（改写内容）与**有效性标记**（检测 + 告警）。本次取后者：
零初始化会让问题继续静默（只是把垃圾换成 0），而告警能直接指出**是哪张图、哪个写入方漏了**——
本次 Q/R 的定位过程正缺这个信息。实现放在 RHI 的纹理布局追踪器旁边：

| 环节 | 位置 | 做法 |
|---|---|---|
| 登记 | `TextureLayoutTracker.{h,cpp}` | 新增 `MarkViewWritten` / `IsViewWritten`。按**图像**而非视图记账：立方体贴图由**逐面视图**写入、却以整张 cube 视图被采样，按视图记会误判为「从未写入」 |
| 写入方标记 | `VulkanCommandList_RenderPass.cpp`（离屏通道的颜色/深度附件）、`VulkanCommandList.cpp`（`ClearDepthStencil`、`CopyTextureToTexture`）、`VulkanResources.cpp`（带初始数据或 `UnorderedAccess` 的纹理） | 共 5 类写路径；`UnorderedAccess` 由 compute 写入、RHI 无法跟踪，故直接登记为豁免 |
| 视图登记补全 | `VulkanResources.cpp`（逐面视图）、`VulkanDevice_Descriptors.cpp`（逐 mip 逐面临时视图） | 这两类是**先前的登记缺口**：不补则 IBL 预滤波图会被误报（实测确实误报过一次，补后消失） |
| 判定 | `VulkanDevice_Descriptors.cpp`（`UpdateDescriptorSet` 单纹理版） | 只对 `COMBINED_IMAGE_SAMPLER` 判定；每张图**只告警一次** |

**判定必须带累计阈值**，否则假阳性会淹没真信号：同一帧内「先采样、后写入」（读到上一帧产物、
pass 注册顺序靠后）完全正常，首帧尤其密集。实测：不做阈值时 06.GILab 报 **15** 条，加阈值
（累计 30 次采样仍未见写入才判定）后降到 **4** 条，而真正的写漏会被每帧采到、很快越过阈值。

验证方式：**把某一条写入方标记临时注释掉，看告警是否增多**——关掉离屏通道的颜色附件标记后，
告警从 4 条升到 **11** 条（并多出 IBL 辐照度图 binding=12）。这证明检测链路（标记 → 判定 →
告警）端到端有效，而不只是「代码看起来对」。

**S 首次跑出的固定清单（4 处，全部属 §9.2-T）**——`none` 层栈下的 06.GILab：

| 描述符 | 纹理 | 归属 | 为何未写入 |
|---|---|---|---|
| binding=9 | 1024×1024 D32_FLOAT | `kGPUBinding_SpotShadow_DL` | 场景无聚光灯 ⇒ 该阴影 pass 从不注册 |
| binding=15 | 512×512 RGBA16F | `kGPUBinding_RSMPosition` | RSM 不在漫反射层栈 ⇒ 该 pass 不注册 |
| binding=16 | 512×512 RGBA16F | `kGPUBinding_RSMFlux` | 同上 |
| binding=19 | 1920×1080 RGBA16F | `kGPUBinding_SSGI` | `gi_blend_diffuse_w2=0` ⇒ SSGI pass 不注册 |

四处都**当前无害**（着色器按权重/光源数守卫，不会真的采样），但都是「描述符指向未初始化显存」
的形态。**告警只有在基线清零后才有信噪比** —— 已由 §10.1 任务 23 清零，见下。

回归：单元测试 159/159、3952/3952；白炉比值与绝对值均 **1.0000**；
Vulkan 校验 46 条与改前一致。

**T 的修复与实测**（未产出的效果改绑中性占位纹理）

思路是先立一条**单一真值**：某个源这一帧到底产出了没有，只由「它的 pass 是否注册」回答，
绑定点读同一个结果 —— 而不是各处自行猜「纹理非空就算有」。

| 环节 | 位置 | 做法 |
|---|---|---|
| 阴影 | `IShadowSystem::WasShadowMapWritten(index)` + `ShadowSystem` 实现 | 每个 Technique 的 `Render` 只遍历自己 `CollectLights` 收集到的那一段 ⇒ 「该技术本帧收集到光源数 大于 0」就是「它的图本帧被写过」的充要条件；关闭或未就绪时不重填计数，故不能拿上一帧的计数当本次已产出 |
| 帧图 | `DeferredPipeline_FrameGraph.cpp` | `rsmPassRegistered` 提到函数作用域（喂 DDGI 与绑给 Lighting 共用）；`ssgiProduced` / `ssrProduced` 在注册期一次算定，同时用于声明读取依赖与绑定纹理；未产出的传 `nullptr` |
| 绑定 | `LightingPass::bindTex` 增加 `fallback` | **只对门控通道生效**：输入为 null 表示本帧没产出，显式回绑中性占位（阴影→白=无遮挡，SSGI/SSR/RSM/RT 反射与 GI→黑=无贡献，RT 阴影遮罩与 AO→白=无遮蔽） |

**踩到的坑（值得记下来）**：一开始对所有通道一律回绑，结果把本来**有效**的 IBL 绑定也换成了
占位 —— BRDF LUT 变成 1×1 白使 `envBRDF` 恒为 1，镜面环境项被算大。原因是 `null` 在不同通道上
语义不同：**门控通道**的 null 是「本帧没产出」，而 GBuffer / 深度 / SSAO / IBL 的 null 只是
「暂时取不到」，此时保留上一次的有效绑定才接近正确值。所以 `fallback` 按通道逐个给出，
默认（不传）保持原行为。同理 RSM 的占位由白改黑：对 `u_RSMFluxMap`，白是 flux = 1.0，
即一个「全亮 VPL」，一旦门控与真实产出不一致，回落就从安全值变成偏亮的错误值。

顺带修掉两处真 bug：

- 占位纹理与占位采样器原本是 `Initialize` 里的**局部** `unique_ptr`，函数返回即销毁，
  而描述符仍指向已释放的句柄 —— 这是「是野指针」报错的来源之一。改为成员后常驻
- Lighting 的 lambda 用 `[&]` 捕获了 `ssgiFinalTex` / `rtShadowTex` 等局部量，而这些量在
  pass 执行时已随 `BuildFrameGraph` 的栈帧失效（见 §9.2-U）。改为按值捕获后，每次运行约
  **186 条「是野指针」报错归零**

验证：未写入告警 **4/4/3 → 0/0/0**；野指针报错约 **186 → 0**；Vulkan 校验 51/51/54 与改前
逐项相同；单元测试 159/159、3952/3952；白炉比值与绝对值均 1.0000；同一二进制连续 6 次运行
均正常收尾。**DDGI 的差分贡献不变**（0.0195943 → 0.0195936），故 §3.3 与 P5 的结论不受影响。

**同时发现 U（未定位）**：`none` 层栈的**绝对**基线在两个只差「一处 lambda 捕获列表」的构建之间
给出 0.0327534 与 0.0472113（相差 44%，`max` 5.94 与 19.68），各自稳定复现。排除过程：

| 线索 | 排除方式 | 结果 |
|---|---|---|
| 门控通道（SSGI/SSR/RSM/聚光阴影） | 把「从未写入的真实纹理」重新绑回去，即复现改动前的绑定 | 读数**不变**（0.0472113） |
| 阴影 | 强制把 `csmShadow0/1/2` 与 `spotShadow` 全设为 nullptr（等于关阴影） | 读数**一字不变**，说明该场景阴影本就不生效 |
| IBL 绑定 | 把 `SetIBLTextures` 恢复成改动前写法 | 读数不变 |

已确认并修掉的一个实例是帧图 Lighting lambda 的失效捕获：它按引用捕获了 `ssgiFinalTex` /
`rtShadowTex` 等局部量，而 pass 在 `BuildFrameGraph` 返回之后才执行，读到的是已弹出的栈帧 ——
表现为每次运行约 **186 条「是野指针」**（把已销毁的纹理指针交给描述符更新），改为按值捕获后归零。
但上面那张表说明**树中还有别的布局依赖来源尚未定位**（帧图其余 pass lambda 逐个审过，捕获列表
本身是干净的；嫌疑转向进程内其它未初始化内存）。

因此当前所有**绝对**读数都不可作为参照（改动前的 0.0327534 同样是布局依赖的产物），而**差分**量
不受影响。归 §10.1 任务 24。

---

## 10. 后续任务与排序

排序原则：**先让验收可信，再修架构主张的破口，然后补前提，最后才是能力提升与结构性投资。**

> 已完成 / 已退场的任务**保留其编号不再重排**：编号是稳定的引用标识（对话、提交信息、其它
> 小节都按号引用），重排会让所有下游引用失效。完成项就地标 ✅ 并加删除线，不参与"下一个
> 做什么"。
>
> **⚠️ 因此编号大小不代表优先级。** 优先级由**分组（A–F）与组内位置**表达；新任务取"下一个
> 空号"并放到它所属的分组里，所以会出现"A 组里有编号 22"这种看似跳跃的情况（第 22 项即如此）。
> 这与"组内按优先级排序"不冲突：**看组、看位置，不看号**。

> 已完成项另见 **§8.1**（完成清单）与 **附录 A**（提交索引），其详情见 **§10.2**。

### 10.1 剩余任务（编号稳定，不重排）

**A 组 · 先让验收可信**（第 1 项已完成；第 2、24 项未完成前，其余验收仍缺判据）

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| ~~**1**~~ | ~~**DDGI 绝对量级不可复现的根因**（§11.3.1）~~ —— ✅ **已完成** | 未知 / 中 | **根因是 IBL 图未被烘焙（未初始化显存被采样）+ DDGI 的 `useRSM` 闩锁**，已修（见 §9.2-Q/R 与 §11.3.1）。它原本会污染所有以 DDGI 为参照的验收——现已解除 |
| **2** | **D1 · 偶发崩溃根因获证** | 未知 / 中 | 根因未证意味着已修项可能只是其中一个实例；长跑类验收会被它污染。长跑 soak + `HE_TRACE_FB=1` |
| ~~**22**~~ | ~~**§9.2-S · RHI 纹理有效性标记**~~ —— ✅ **已完成** | 小 / 中 | **本次 §9.2-Q/R 的"放大器"**：未初始化显存把一个"消费者门控写漏"放大成了**静默的物理错误 + 跨构建不可复现的读数**。已实现为**「已写入」登记 + 一次性告警**（不改写纹理内容），并在 06.GILab 上验证：能定位这类纹理，且不产生假阳性。**编号 22 出现在 A 组是刻意的**——见下方编号政策 |
| ~~**23**~~ | ~~**§9.2-T · 未产出的效果改绑占位纹理**~~ —— ✅ **已完成** | 小 / 中 | 22 完成后，检查跑出的 4 处「描述符指向从未写入的纹理」全部属「效果本次未产出」。已立单一真值（pass 注册结果）+ 门控通道回绑中性占位，把 22 的告警基线清零：**4/4/3 → 0/0/0**，并顺带把每次运行约 186 条「是野指针」归零。详见 §9.2-T 的实测 |
| **24** | **§9.2-U · 绝对读数的二进制布局依赖（根因定位）** | 中 / 中 | **决定所有绝对读数是否可信**。两个只差一处 lambda 捕获列表的构建，`none` 绝对基线差 44%（0.0327534 / 0.0472113），各自稳定复现；门控通道、阴影、IBL 绑定、复现旧绑定四条线索均已排除。已知的一个实例（帧图 Lighting lambda 的失效捕获，约 186 条「是野指针」）已随任务 23 修掉。**验收判据（本次踩出来的）**：改动与本功能无关的代码（例如只改注释）后重编译，绝对读数必须**逐位不变**；并配合「开/关阴影」这类对照实验自洽。定位手段建议：`HE_TRACE_PASSES` 逐 pass 二分 + 单源做差看差异落在哪个通道 |

**B 组 · 修架构主张的破口与已知功能缺陷**（§9.2 的剩余各项）

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| **3** | **§9.2-I · RTGI 用 DDGI 做 miss 回退** | 中 / 中 | **直接破坏架构核心主张**——「归一化 ⇒ 无双重计数」。Ultra 档同时含 RTGI + DDGI 时 DDGI 信息被用两次，加权平均**失去无偏性**。这是合成数学层面的破口，比任何单个源的缺陷都更靠近架构本身 |
| **4** | **§9.2-G · 层栈与子系统开关是两套真值** | 中 / 中 | **不变量 1**。文档明确写着"曾因两者不一致导致画面发黑，**且在同一处复发过一次**"。复发过的根因 |
| **5** | **§9.2-D · AO 乘到了直接光上** | 中 / 中 | 每帧生效的能量错误（`color *= lerp(1, ao*aoVal, aoIntensity)` 位于直接光累加之后、间接镜面之前），且使 AO 不作用于镜面 |
| **6** | **§9.2-F · RSM 的 pass 被嵌套在 DDGI 门控内** | 中 / 中 | 单独勾选 RSM 而关闭 DDGI 时 **RSM 永不注册**——配置说谎，属功能失效 |
| **7** | **§9.2-E · SSR 与 SSAO 仍用硬编码投影** | 中 / 中 | 同一缺陷的**两个剩余实例**（SSGI 已修，见 §9.2-M）；修法与先例都已具备。非默认相机（`PhysicalCamera` 由焦距反算 fov）下空间错位 |
| **8** | **§9.2-H · Forward 无 Provider、无 `GIBlendParams` UBO** | 中 / 中 | 要么补齐 Forward 的层栈归一化，要么把 `PipelineCaps::Forward` 的声明改对——**当前是"声称支持但实际不存在"**，与 §5.1 的能力位表不符 |

**C 组 · 补齐「归一化」的前提**

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| **9** | **§3.2 置信度体系**（屏幕空间源的逐像素可信度） | 中 / 中 | §3.2 自己写着这套东西"**均未落地**"，目前只有"屏幕边缘 5% 降权"一条。缺它 ⇒ 屏幕空间源在屏幕外/背面无数据时**无法按像素降权**，只能整幅参与加权——这正是归一化在屏幕空间源上最薄弱的地方。**也是以后接 Lumen 的前置**（§4.3） |
| **10** | **SSGI-CAL · 标度与量纲标定**（= §9.2-P） | 中 / 中 | P5 退场后的接棒项。补入射辐射度项 + 余弦项归一化 + 以 PT 标定。**第 1 项已完成、依赖已解除**（DDGI 的绝对量级现已确定性可复现，参照量可信）；几何前置（M/N/O）已完成 |
| **11** | **统一降噪框架**（AO / GI / 反射 / 阴影 / 探针共用）—— 设计与现状对照见 **§4.4** | 中 / 大 | 现在每个 Provider 自带降噪（`Denoiser` / `RTDenoiser`，共 **9 个实例 / 9 套 PSO / 14 张纹理**），**降噪器之间不组合**——§9.2-C 的有效性协议四处断裂正由此而来。分三步： |
| **11.1** | 合并 `SSGIProvider` / `SSRProvider` 的重复降噪实现；把 `Denoiser` 的 `depthSigma` / `normalSigma` 变成**可配置** | 小 / 低 | **有即时收益**：4 个 `Denoiser` 实例参数完全相同且无 setter，**同一个 σ 核被用在漫反射间接光与镜面反射两种语义不同的信号上**。判据：背靠背单源采样逐项一致 |
| **11.2** | `RTProvider` 的降噪链**改为数据**（用 `std::vector<stage>` 取代 `m_Temporal`/`m_Spatial` 两指针 + 索引位置约定） | 小 / 中 | 纯去重：加第三级滤波不必再改 `RenderAux`。可与 11.1 合成一次提交 |
| **11.3** | **按信号类型分派**（`DenoiseSignal` + 统一历史分配 + 批量 dispatch + 框架级有效性契约） | 中 / 大 | **需要消费方**（Lumen / P6 / 多信号共存）来验证抽象选型；并能让**半分辨率也降噪**（现在 `AuxActive()` 在 `halfRes` 时直接跳过） |

**D 组 · 质量与性能**

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| **12** | **AMORTIZE · 时间维分摊**（DDGI 每 N 帧更新 + 时域复用） | 中 / 低 | 把 256 探针的全量更新摊到多帧。第 1 项已证明 DDGI 的时域回路**不是**问题所在（§11.3.1），该项依赖已解除 |
| **13** | **CULL · pass 级空间剔除**（tile / scissor） | 中 / 中 | `GPUCulling` **只服务 GBuffer 几何**，不服务 GI；GI pass 全是整幅执行（§3.5）。注意不能用逐像素距离代替（距离是视角相关的，会引入接缝爬行） |
| **14** | **§9.2-K · DDGI 网格外退化为贴边常数外推** | 低 / 低 | 探针网格为固定参数，覆盖不到的区域**静默缺失**低频 GI 且无 falloff / 无效标记 |
| **15** | **§9.2-J · 合成参数 UBO 只一份**（非 per-frame-in-flight） | 低 / 低 | `MAX_FRAMES_IN_FLIGHT=3` 但 UBO 是单份，目前靠"值变化小"掩盖 |
| **16** | **B3 · RSM VPL halfRes** | 小 / 低 | 位置：`SampleRSMIndirect`（16 点 Poisson 盘 VPL 求和） |
| **17** | **B4 / M5.2-A · DDGI 光追 march** | 中 / 中 | 只提升单一源质量。第 1 项已解决（§11.3.1），DDGI 的读数现已确定性可复现，可以安全改动 |
| **18** | **Lightmap 源落地** | 中 / 低 | 按需（PC 实时路线可缓）。`ToPipelineCap()` 目前无该分支 ⇒ `IsAvailable` 恒 false（§9.2 第 6 行的对应实现） |

**E 组 · 结构性投资（等消费方）**

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| **19** | **PROVIDER-EXEC · 执行单位从「Provider × 通道」改为「Provider」** | 中 / 中 | §4.3.5 第二层。**是整洁性投资、不是硬前置**——Lumen 随时可走「新增第 8 条定制循环」的逃生口（§4.3.5）。建议只先合并 specular + diffuse 两条循环。**无消费方时收益为零**，可与第 20 项合并设计 |
| **20** | **P6 · ReSTIR GI 统一估计器** + **纹理绑定数组化 / 跨通道共享**（§4.3.5 第三层） | 大 / 高 | 长期。**两者应同时做**——第三层泛化需要 P6 或 Lumen 作为验收对象；没有消费方的泛化无法验收 |

**F 组 · 文档**

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| **21** | **文档一致性修正**（可随时做） | 小 / 低 | §3.1 的合成片段与 shader 不符（`max(den,1e-4)` vs `(den>0)?num/den:0`）；§11.3 的采样目标列表缺 `radiance`；§6 不变量 2 与 6 已过期；§9.2 标题漏 M/N/O/P；§11.4 两条随 P5 退场的失效风险；§11.3 的运行示例自相矛盾 |

> **本次重排说明**：
> 1. **引入 A 组**：把两个"调查项"提到最前，理由是**它们决定其余任务的验收是否可信**——
>    第 1 项直接决定第 10 项的验收标准能否成立，第 2 项决定长跑类验收是否被污染。
> 2. **B 组把 §9.2 的剩余缺陷全部任务化**：此前 §9.2 列了 8 项未修缺陷，但 §10 的任务表里
>    **一项都没有**——缺陷与计划脱节。现按"越靠近架构主张越靠前"排列：
>    合成无偏性（I）→ 复发过的不变量（G）→ 每帧能量错误（D）→ 功能失效（F）→ 定位错位（E）
>    → 声明与实现不符（H）。
> 3. **C 组**把「置信度体系」与「统一降噪」正式立为任务：前者是归一化在屏幕空间源上的
>    前提，后者是多个源共存的前提，二者也都是接 Lumen 的前置（§4.3）。
> 4. **第 12、17 项加了"先看第 1 项结论"的依赖**：它们都作用于 DDGI 的时域行为/质量，
>    在 §11.3.1 的异常查清前动手会让归因失效。
> 5. 原来的 `P0…P5` 顺位编号与"Wave"编号混用且已完成项占号，**故整体改为从 1 起连续编号**。

### 10.2 各项详情

> 剩余任务按 §10.1 的编号排列；**已完成 / 已退场**项保留原文并以「✅ / ❌」标记、不占编号。
> B 组的多数条目证据已在 §9.2 表中，此处只记「要做什么、怎么验证」。

**1 · DDGI 绝对量级不可复现的根因** —— ✅ **已完成**（§11.3.1）

- **结果**：根因不是 DDGI 自身，而是帧图把两张**从未烘焙的纹理（未初始化显存）**交给了渲染。
  完整因果链、判定指纹、修复与前后数据见 **§11.3.1**；缺陷本身记为 **§9.2-Q** 与 **§9.2-R**。
- **过程中被否定的三个假设**（值得留着，避免以后再走一遍）：
  1. ~~DDGI 的时域回路是单位增益积分器~~ —— 把探针历史零初始化后读数几乎不变，否定；
  2. ~~「前帧 HDR 反馈环」~~ —— 读代码发现 `u_PrevHDR` 在 `DDGI.comp.slang` 里**只有声明、
     全文未用**（屏幕 HDR 回退早已被换成 IBL 辐照度），**那个环根本不存在**；
  3. ~~pass 名/顺序、`falloffDistance`、配置差异~~ —— 逐项实测排除。
- **正确的一步是"做指纹"而不是"猜机制"**：`(HDR_ddgi − HDR_none)/albedo` 的通道比恰为
  `1 : 1.5 : 4`，与着色器硬编码兜底常数 `(0.02,0.03,0.08)` 完全吻合 —— 这一步直接把范围
  从"DDGI 的动力学"缩到"探针根本没采到数据"。
- **阻塞已解除**：第 10 项（SSGI-CAL）的验收标准不再缺可信参照；第 12、17 项的
  "先看第 1 项结论"依赖也一并解除——DDGI 的时域回路已确认**不是**那个可疑机制。

**3–8 · §9.2 的剩余缺陷**（证据与位置见 §9.2 对应行，此处只列修法要点）

- **3（I · RTGI 的 miss 回退）**：要么让 RTGI 在 miss 时**返回无效**（复用 §9.2-C 已贯通的
  有效性协议 `alpha<0`），要么在 `REDUNDANCY` 层面**禁止 RTGI 与 DDGI 同时入栈**。
  前者更符合「源独立」，后者把限制推给配置。**建议前者。**
- **4（G · 两套真值）**：把 `IsValid()` 改为**由层栈派生**（与 §2.4 的 `GIConfig` 门控谓词同源），
  面板的层栈 UI 只改层栈、不再直接改子系统开关。
- **5（D · AO 乘到直接光）**：`color *= lerp(1, ao*aoVal, aoIntensity)` 要移到**只作用于间接项**
  的位置；同时明确它是否作用于镜面（现状不作用）。
- **6（F · RSM 被 DDGI 门控）**：把 RSM 的注册从 DDGI 循环里移出，成为独立门控
  （Forward 侧本就是独立的 `ShouldRunRSM()`）。
- **7（E · SSR/SSAO 硬编码投影）**：照 SSGI 已修的路径（`SetCamera` + UBO 补 `u_View`）。
  判据：改用非默认 `PhysicalCamera`（由焦距反算 fov）后空间重建不再错位。
- **8（H · Forward）**：二选一——补齐 Forward 的 Provider 与 `GIBlendParams` UBO，
  或把 `PipelineCaps::Forward` 的声明改成实际支持的范围。**不能保持"声称支持但不存在"。**

**9 · §3.2 置信度体系**

- 现状：`GIChannelBlendParams` UBO 里**没有 confidence 字段**，实际只有"屏幕边缘 5% 降权"一条。
- 要做的：给每个通道的每个源加一个**逐像素可信度乘子**（屏幕空间源：屏幕外/背面/被遮挡 ⇒ 0；
  探针源：网格覆盖外 ⇒ 0，与第 14 项合并；光追源：收敛度 / SPP）。UBO 结构与 shader 的
  `SourceWeight` 需同步扩展（已有 `static_assert` 守布局漂移）。
- 判据：屏幕边缘处屏幕空间源的权重降为 0 后，该处只剩 IBL/DDGI，且白炉仍守恒。

**11 · 统一降噪框架**（设计与现状的完整对照见 **§4.4**）

现状：`Denoiser`（空间 5×5 双边）× 4 + `RTDenoiser`（时域累积）× 5 = **9 个实例、
9 套 PSO、14 张纹理**；两个类的输入签名与参数机制互不相同；链条形状由调用方的
`if (IsTemporalIndex(i))` 位置约定表达。

分三步，**每步独立可提交、独立可回退**：

- **11.1 · 去重 + 参数可配**（小 / 低，**有即时收益**）
  - 做了什么：把 `SSGIProvider` / `SSRProvider` 里逐行同构的 `SetInputs + Render` 合并成
    一个共享实现；给 `Denoiser` 补 `SetDepthSigma/SetNormalSigma`（现在 `Render` 里直接写
    `kDefaultDepthSigma(10)` / `kDefaultNormalSigma(8)`，无任何 setter）。
  - 为什么现在值得做：**4 个实例参数完全相同**，意味着同一个滤波核被用在漫反射间接光与
    镜面反射上——这两种信号的噪声分布与可容忍模糊度不同。这是"统一"最省的第一步。
  - 判据：背靠背单源采样逐项一致（`Tools/gi/analyze_gi.py`）+ 白炉 1.0000 + 单测全绿。
- **11.2 · 链条数据化**（小 / 中）
  - 做了什么：`RTProvider` 用 `std::vector<Stage>` 取代 `m_Temporal` + `m_Spatial` 两个指针
    与 `IsTemporalIndex(i) ? MainOutput() : TemporalOrMain()` 的位置约定；
    `GetAuxPassCount/Name/Input/Output` 与 `RenderAux` 改为遍历该向量。
  - 判据：同上；并确认给 RT 效果加第三级滤波只需 push 一个 stage、不改框架代码。
- **11.3 · 按信号类型分派**（中 / 大，**需要消费方**）
  - 做了什么：引入 `DenoiseSignal`；统一分配历史纹理与采样器；支持把多个信号批量 dispatch；
    把"有效性（`alpha<0`）"提升为框架级契约（见 §4.4.3 的目标形状）。
  - 为什么必须等消费方：**要猜出该有哪些 `DenoiseSignal`、每种信号要什么核与升采样策略，
    必须有真实的多信号共存场景**（Lumen / P6）。没有消费方的泛化无法验收。
  - 顺带收益：让**半分辨率也降噪**——现在 `SSGIProvider::AuxActive()` 在 `halfRes` 时返回
    false，半分辨率输出被直接采样；根治需要 `needsUpscale`（重建升采样）这一信号属性。
  - 与其它项的协同：第 10 项（SSGI-CAL）补 `L_in` 后噪声会上升，需要更强的降噪；
    第 20 项（P6 / Lumen）会立刻撞上"两边降噪器不组合"。

**14（K）· 15（J）**：见 §9.2 对应行。两项都小，可与 B 组并行。

**18 · Lightmap 源落地**：需要 `ToPipelineCap()` 补分支 + 一个真实的 `LightmapProvider`；
在此之前 `IsAvailable` 恒 false，属"预留但不可用"（§5.1）。

**21 · 文档一致性修正**：清单见 §10.1 该行，可随时做。

**（已完成）D2 层栈归一化 CPU 单测** —— ✅ **已完成**（36 用例 / 427 断言）
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

**（已完成）REDUNDANCY 冗余源诊断 + 可证等价去重** —— ✅ **已完成**
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

**（已退场）P5 频率分离** —— ❌ **已判定不需要**，第 0 步即终止（结论与数据见 §3.3）

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

**10 · SSGI-CAL SSGI 标度与量纲标定**（P5 退场后的接棒项）

- 动机：§9.2-L 修好后 SSGI 已真正生效，但实测（同一场景、单源层栈）DDGI 仍明显强于 SSGI，
  且 `GI/SSGI.frag` 的累加式**只含命中点 albedo**：

  ```hlsl
  indirect += sAlbedo * max(0,dot(N,sDir)) * falloff;   // 没有入射辐射度项
  ```

  即它**不是 `E/π` 的估计**，而是「反照率相关性」启发式量。归一化加权平均的前提是各源估
  同一个量（§3.4），故 SSGI 在归一化里只拿到很小的权重——开着、每帧付一整幅 pass 的成本，
  却几乎看不见。

  > **数值已按修复后重测**：DDGI 约为 SSGI 的 **21.0 倍**，故归一化下 SSGI 只占输出值约
  > **4.5%**（= 1/(1+21.0)）——开着、每帧付一整幅 pass 的成本，却几乎看不见。
  > （此前记录的「22.9 倍 / 4%」是在 DDGI 失效期间（§11.3.1）测得的，数值恰巧接近，不作数。）
- **前置已完成**：原先列在此处的两处「疑似几何缺陷」已证实并修复，且实际查出**三处**
  （§9.2 的 M/N/O）：TBN 变换方向反了（主因，`dot(N,sDir)` 均值仅 0.012）、世界/view
  空间混用、可见性判据方向相反。三项合计令 SSGI 产出 ×5.2、正贡献像素 53.4% → 99.5%。
  **剩余的量级缺口不是几何问题而是量纲问题**，不能靠继续修方向解决。
- 需要做的三件事：
  1. **补入射辐射度项**：把 `sAlbedo` 换成命中点的入射辐射度 `L_in`。需要一个颜色输入
     （标准做法是上一帧的 HDR 光照结果；SSGI 在帧图里排在 Lighting 之前，此刻 HDR 目标
     仍持有上一帧内容，正好构成时域反馈）。这是本项最实质的一步，也是唯一需要新增绑定
     与帧图依赖的部分。
  2. **余弦项归一化**：现在用未归一化的 `sDir` 算 `dot(N,sDir)`，等于把 cos 项与样本长度
     混在一起（修正 TBN 后它恰好退化为切线空间样本的 z 分量，量级只有约 0.1）。应改为
     `dot(N, normalize(sDir))`，距离衰减单独用 `length(sDir)` 表达。
  3. **以 PathTracing 为参考做实测标定**，确定整体增益。
- 验收：单源亮度与 PT 参考同量级；`SSGI + DDGI` 归一化后两者贡献比接近权重比；白炉仍守恒。
- 风险：中——标定依赖 PT 参考的可比性；入射辐射度取自上一帧会引入时域反馈，需确认不发散
  （可复用 SSGI 现有的空间降噪与收敛检查）。

**19 · PROVIDER-EXEC 执行单位改为「Provider」**（§4.3.5 第二层）

- 目标：帧图的执行单位从「Provider × 通道」变成「Provider」——即**一个 Provider 每帧只注册
  一次 pass**，即使它的源 id 同时出现在多个通道的层栈里（这正是 Lumen 的天然形状）。
- 现状：7 条按 source id 定制的循环（§4.3.1）；一个 Provider 若同时 `Handles(SSGI)` 与
  `Handles(SSR)`，会被 L379 与 L426 各跑一遍。
- 做法：循环体按**已被声明却零消费的 `GetPassKind()`** 选择 pass 形状
  （`Offscreen` / `Compute` / `Custom`），输出按 `GetDiffuse/Specular/AOOutput()` 落到对应通道。
  **无需新增接口方法**——`Handles()` 已支持「一个 Provider 属于多个 id」（SSAO/GTAO 是先例）。
- **迁移策略（逐类，每步可独立验证）**：
  1. **先只合并 specular 与 diffuse 两条循环（L379 + L426）** —— 这两条形状几乎完全相同
     （都读 depth+normal+albedo、都有附属降噪链、都以 `GetFinalXOutput()` 收尾），
     而且**恰好就是 Lumen 需要共享的那一对通道**。改动最小、可独立回退。
  2. **AO 循环（L354）暂不动**：它形状不同（不读 albedo、半分辨率尺寸处理不同），
     且存在上述旁路（`in.ssaoTex = m_SSAO.GetAOTexture()` 绕过 Provider），
     需要先把旁路归位才能同等对待。
  3. `Compute`（L334 DDGI）与 `Custom`（L582 IBL）暂留原样，最后收。
  4. L500 的 RT 循环因涉及 `dynamic_cast<RTEffectProvider*>` 与 AS/SBT，**留到最后或不动**。
- **验收判据**：同一环境下**背靠背**的单源采样逐项一致（pass 集合与顺序、各 `provN_raw/final`
  纹理数值）+ 白炉 1.0000 + 单元测试全绿。§11.3.1 的异常已修复，绝对量级现在也可复现，故该判据之外还可以直接核对绝对值。
- **逃生口**：本项**不是**解锁 Lumen 的必要条件——随时可以像 IBL/RSM/RT 那样给 Lumen 加
  第 8 条定制循环，它的 pass 照样只注册一次。故本项是**可维护性投资**；
  若优先级不足，可推迟到真正接 Lumen 或 P6 之前。
- 风险：中——帧图是核心路径，对策见 §11.4。

**16 · B3 RSM VPL halfRes** — 位置：`DeferredLighting.frag.slang` 的 `SampleRSMIndirect`（16 点 Poisson 盘 VPL 求和）
**2 · D1 崩溃根因** — 长跑 soak + `HE_TRACE_FB=1`（`delay=0` 即同帧销毁的直接指标）；
现状：符号化落在 `VulkanTexture::GetImageView()` 野指针，最可疑根因（framebuffer 被同帧销毁）已修

**（已完成）§9.2 A/B/C** —— ✅ **全部完成**（详见 §9.2 的「A/B/C 的修复与实测」）
- ✅ **A · 量纲统一**——约定 `L_o = albedo × E/π`，修 4 处（DDGI 补 `albedo/π`、
  RTGI/RSM 补接收面 albedo、`RT_GI.rgen` 的 miss 回退补 `1/π`），并解耦 RSM 对
  `iblIntensity` 的依赖。**单源亮度实测验证**。
- ✅ **B · SSR 投影矩阵**——矩阵进 UBO（用 `GI_SSR.cpp` 本就声明却未使用的 binding 3），
  正/逆分开传；push constant 96B → 32B。4 处用法全部正确。
- ✅ **C · SSR 有效性协议**——贯通**四处**：SSR.frag 写 −1、空间降噪保住符号、
  时域降噪的有效性取自本帧、合成端判定终有生产者。**顺带修好了 RT 反射侧**
  （它的 rgen 早已写 −1，但时域降噪把符号 lerp 掉了）。
- 遗留：`SSGI-CAL` —— SSGI 整体标度是启发式，未按 `E/π` 校准，需以 PT 为参考实测标定。
  原述「非 P5 前置」**已不成立**：P5 判定退场后它成为接棒项，已从「遗留」升级为
  **§10.1 的第 10 项**（见 §10.1 顺位表与 §10.2 详情）。

**（已完成）§9.2-L SSGI 完全不生效** —— ✅ **已完成**（详见 §9.2 的「L 的修复与实测」）
- `SSGIProvider` 把接口覆写命名成了 `SetGBuffer`，而基类 `IGIProvider::SetInputs` 带
  空实现默认体 → 输入恒为 `nullptr` → `GI_SSGI::Render` 提前返回 → SSGI 从未参与合成。
- 修法：改名 `SetInputs` 并显式加 `override`。
- 方法论收获：这是**唯一一项无法靠读代码或写单测发现**的缺陷——开关状态、`IsValid()`、
  面板显示全部正常，只有把各源的中间纹理读回来做数值对照才暴露。

**12 · AMORTIZE 时间维分摊** — DDGI 探针更新（256 探针 × numSamples）由「每帧全量」改为
「每 N 帧 + 时域复用」。DDGI 已有 `blendAlpha` 历史混合，天生适配；需实测确认收敛速度可接受。
*（参考：UE 的 Surface Cache 明确是 "amortized over multiple frames"）*

**13 · CULL pass 级空间剔除** — 给 GI pass 加 tile / scissor 剔除（视锥外、被遮挡的 tile 不 dispatch）。
现状：`GPUCulling` **只服务 GBuffer 几何**，GI pass 全是整幅执行（§3.5）。
注意：这**不能**用逐像素距离判断代替（距离是视角相关的，会引入接缝爬行，见 §3.5 与 §3.2）。

**17 · B4 / M5.2-A DDGI 光追 march** — 方案 A（硬件光追 march）+ 按 `supportsRayTracing` 自动选择

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

**纹理「已写入」检测** —— 「采样了从未被写入的纹理」会自动报警（§9.2-S）

不需要开关：RHI 在每次 `UpdateDescriptorSet` 绑定 `COMBINED_IMAGE_SAMPLER` 时判定该纹理所属的
图像是否被写入过，未写入且**累计 30 次采样**仍未见写入就报一条 `HE_CORE_WARN`（每张图只报一次），
日志里带 `binding / 尺寸 / Format / VkFormat / 层数 / usage / VkImage`，可直接对到具体绑定：

```
[RHI] 描述符指向一张从未被写入的纹理：binding=15 512x512 Format=11 VkFormat=97 层数=1 usage=0x3 image=0x... 
```

读法：若描述符对应的效果**本次未产出**（门控关闭、无该类光源），属预期（见 §9.2-T 的基线清单）；
**若不在基线清单里，就是真的写漏了**。改判定阈值请改 `TextureLayoutTracker.cpp` 里的
`kUnwrittenWarnThreshold`（调小更灵敏但假阳性增多）。

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

#### 11.3.1 测量可靠性：一次「绝对量级跨构建不可复现」的定位与修复（已解决）

**现象**：同一次调查过程中，DDGI 对单源做差的贡献从一个稳定值变成了另一个稳定值——
早先记录 **0.01225**，后来稳定在 **0.00102**，相差约 12 倍，且两边都逐位可复现。

**结论：根因已定位并修复。** 它不是 DDGI 自身的问题，而是**帧图把两张从未烘焙的纹理
（未初始化显存）交给了渲染**；未初始化显存的内容在同一内存布局下逐位可复现、跨布局就变，
这正是「同一份代码给出两个稳定但不同的值」的来源。

##### 真正的机制

1. **IBL 烘焙的门控只看漫反射层栈**（`IBLProvider::NeedsPass(m_GIConfig.diffuse)`），
   而辐照度/预滤波 cubemap 有三个消费者：diffuse 通道、**specular 通道**（Lighting 恒定绑定）
   与 **DDGI 探针更新的辐射度回退**（`GI_DDGI::SetIBL`，无条件注入）。
   ⇒ 在「IBL 只出现在镜面栈」「漫反射栈为空」「漫反射栈只放 DDGI」这些配置下，
   **烘焙 pass 从不注册，两张 cubemap 保持未初始化被继续采样**。
2. **DDGI 的 `useRSM` 是只置位、永不清除的闩锁**：帧图把 RSM 的 position/flux 图交给 DDGI
   是**无条件**的，而 RSM pass 的注册条件却是「RSM 在漫反射栈里」。
   ⇒ 在「DDGI 开着 + 有活动阴影 + RSM 不在漫反射栈」时，RSM 本帧不渲染，空图却照样被交给
   探针更新，`useRSM` 从此恒为 1。

两条共同作用的结果：**DDGI 探针更新的全部样本都采不到有效辐射度**
（未初始化图的 NaN 让 `dot(radiance,radiance) > 0.0001` 恒为假——NaN 比较永远为假），
于是每个探针都落入 `DDGI.comp.slang` 的硬编码兜底 `sh[0] = (0.02, 0.03, 0.08)`。
**DDGI 表面上启用、无任何报错，实际完全不做 GI，其"贡献"是 `albedo × 常数`。**

##### 判定指纹（这是本次能一步定性的关键）

| 检验 | 观测 | 说明 |
|---|---|---|
| 通道比 | `(HDR_ddgi − HDR_none)/albedo` 的 R:G:B = **1 : 1.499 : 3.995** | 兜底常数 `(0.02,0.03,0.08)` 的比是 **1 : 1.5 : 4**，精确吻合 |
| 与场景的相关性 | `corr(E, albedo) = −0.033` | 除掉 albedo 后与场景无关 ⇒ 是常数而非 GI |
| 辐照度图内容 | `ibl_irr` dump = **RGB(nan,nan,0)，仅 1/1024 像素非零** | 直接指向「图未被烘焙」而不是「着色器算错」 |
| 空栈基线 | `none` HDR = 0.100791 / **max 234.16** | 而烘焙修复后是 0.0327533 / **max 5.94** ⇒ **旧基线本身也是采样未初始化显存来的假值** |

**「12 倍」由此完全解释**：早先那批测量用的是 `gi_solo=1`（关闭直接光）⇒
`HasActiveShadows()` 为假 ⇒ 整个 RSM 块被跳过 ⇒ `useRSM=0` 走 IBL 路径（恰好可用），
得 0.01225；后来改成 `gi_solo=0` ⇒ 阴影激活 ⇒ 走空 RSM 路径 ⇒ 得 0.00102。
**两个值都不是「DDGI 的正常值」**，只是分别恰好走到了一条可用/不可用的路径。

##### 修复（三处，均已提交）

| 修复 | 位置 |
|---|---|
| IBL 烘焙门控改为「任一消费者需要」（diffuse / specular / DDGI） | 帧图 `IBL_Bake` 循环 |
| `SetRSM` 与 RSM pass 的实际注册条件一致（`rsmPassRegistered`） | 帧图 RSM 段 |
| 着色器 RSM 路径**逐样本回退到 IBL 辐照度**（防止同类失效再次静默吞掉全部 GI） | `GI/DDGI.comp.slang` |

##### 修复前后（单源层栈做差，Frame=120）

| 指标 | 修复前 | 修复后 |
|---|---|---|
| `ibl_irr` 内容 | RGB(nan,nan,0)，1/1024 非零 | **0.197，1024/1024 非零** |
| `none` HDR | 0.100791 / max 234.16（假值） | **0.0327533 / max 5.94**（确定性） |
| DDGI 贡献 | 0.001022（兜底常数） | **0.019596** |
| DDGI 贡献 R:G:B | 1 : 1.500 : 4.000（常数指纹） | **1 : 1.010 : 1.016**（环境驱动） |
| DDGI 空间变化 std/mean | 0.25（仅 albedo 形状） | 0.28（真实 GI 结构） |

回归：单元测试 159/159、3952/3952；白炉 **1.0000**；Vulkan 校验 **46 条与基线一致**。

##### 方法论收获（三条，都写进约定）

1. **GI 源「吃进去」的中间量必须和「吐出来」的一样纳入纹理级对照。** 本次是靠 dump
   `ibl_irr` 一步定性的——只看 DDGI 的输出只会看到「一个暗色常数」，看不出原因。
2. **未初始化纹理会把「配置错误」放大成「静默的物理错误」。** 修复已落地为 RHI 的
   「已写入」登记 + 一次性告警（§9.2-S）；它**只检测、不改写内容**——零初始化会把垃圾换成 0
   而继续静默，告警则直接指出是哪张图、哪个写入方漏了。
3. **绝对量级的变化要先用"通道比 / 与场景的相关性"做指纹判别**，再去猜机制。
   本次前几轮都在猜「时域回路」，而真正的线索是那个 1:1.5:4 的颜色比。

> **给 S 的告警定基线**：它现在在 06.GILab 上固定报 4 条（§9.2-T，全部是「效果未产出但描述符
> 仍绑定真实纹理」）。**这 4 条是当前基线，新增的任何一条都要当场查清**——归 §10.1 任务 23 把
> 基线清零后，告警才真正具备信噪比。

> ⚠️ **曾被本文就地标注为"待重测"的数字现已重测**，§3.3 与 §9.2-M/N/O 的相关数值
> 已按修复后的数据更新；各处「不可引用」的标注已移除。


### 11.4 风险总表

| 风险 | 影响 | 对策 |
|---|---|---|
| 偶发崩溃根因未证 | 长跑 + 读回类验收被污染 | 已修最可疑项（FB 同帧销毁）；根因保留为观察项，再发即由崩溃处理器出调用栈 |
| 合成改造后「画面变了」 | 回归难判断 | 单源配置逐像素截图对比 + 白炉数值判据（已可量化） |
| C++/slang 双侧同步漏改 | 静默错值 | `static_assert(sizeof)` + 双侧常量同源 |
| **白炉测试只验归一化、不验量纲** | 源的量纲错误被掩盖 | §9.2-A；白炉需扩展为「逐源真值校验」而非短路 |
| 频率分离的 `LowPass` 选型不当 | 噪声/振铃/跳变 | 已随 P5 退场失效（§3.3 判定不需要） |
| **消费者门控写漏 → 未初始化纹理被采样** | **静默的物理错误 + 读数跨构建不可复现**（本次 §9.2-Q/R 正是如此被放大的） | 已落地 RHI「已写入」登记 + 一次性告警（§9.2-S）：任何被采样却从未写入的纹理都会报出 set/binding/尺寸/格式；告警基线已由任务 23 清零（§9.2-T）。且 GI 源**吃进去**的中间量也要纳入纹理级对照（§11.3.1） |
| **帧图 pass lambda 读失效栈帧**（§9.2-U 的一个已修实例） | **把已销毁的纹理指针交给描述符更新**：实测每次运行约 186 条「是野指针」 | 已随任务 23 修掉（Lighting lambda 改按值捕获）；帧图其余 pass lambda 逐个审过，捕获列表干净 |
| **绝对读数依赖二进制布局**（§9.2-U，未定位） | **绝对判据失真**：两个只差一处捕获列表的构建，`none` 基线差 44%（0.0327534 / 0.0472113）；差分量的可信度不受影响 | 任务 24：门控通道、阴影、IBL、复现旧绑定四条线索已排除，来源待定位。**修完前不用绝对数值下结论**；判据是「只改注释并重编译后绝对读数逐位不变」 |
| 校验层重复消息去重掩盖计数 | 误把「报告数」当真实次数（历史上两次误判） | 关闭 `duplicate_message_limit`，或用 `HE_TRACE_FB` 交叉验证 |
| P5 抽象/改造过度 | 大范围回归 | 分步提交（3.1→3.4），每步实测；保留 Additive/Normalized 作对照 |
| **帧图从「按通道」改为「按 Provider」执行** | 核心路径回归 | 逐类迁移 + 每步判据：**先只合并 specular 与 diffuse 两条循环**（形状相同、且正是 Lumen 需要共享的一对），AO 因存在旁路暂不动，`Compute`(DDGI) 与 `Custom`(IBL) 暂留；判据是**同一环境下背靠背的单源采样逐项一致**（§11.3.1 已修复，绝对量级现在也可复现） |
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
| ↑ | SSGI 采样方向与可见性三处修复（§9.2-M/N/O） | ✅ | `14708de` `c71b051` |
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
