# HugEngine GI 架构与开发计划

> ## ✅ 全部完成
>
> **本文档 GI 计划内的任务已全部收口**（§10.1 任务表**没有任何未完成项**）：
> · §9.2 的缺陷 A…AG **全部已修复**（每行都有判据与实测）；
> · 未完成的工作不在这里，而是**迁到了《Lumen与Nanite完整设计规范》**——原任务 11（统一降噪框架，
> 含 11.3）、19（PROVIDER-EXEC）、20（P6 + 绑定数组化）见该文档 §5.1 / §5.2，它们的验收对象是
> Lumen / P6；原任务 31（Lightmap 真落地）**已取消**（取消记录与保留的基础设施见 §10.2）；
> · **另有两项记录在案的验证余额**（不是任务，见 §11.4）：其它示例的 Forward 观感目视确认、
> IBL/DDGI/RSM 的逐源白炉真值校验。
> · 复核手段与结果：`check_tables` 0 处不一致、文档提到的 `Tools/gi/*` 与磁盘双向一致、
> 关键实现文件逐个存在、全文无指向已迁出/已删除任务的悬空引用。
>
> **因此本文档已归档**：实现计划随代码一起完成，故从 `docs/计划实现功能/` 移到
> `docs/已实现功能/`。后续若重新开题（例如按 §10.2 的改判条件重做 Lightmap），**新开一项、
> 不要复活已迁出或已删除的编号**。

> **本文合并自**：《HugEngine GI分层合成架构设计.md》（架构设计）+《HugEngine GI优化开发计划.md》（开发计划）。
> 原两份文档中的历史里程碑与提交级演进记录已压缩为 **附录 A**；其余历史细节可由 git 历史取回。
>
> **本次更新依据**：对当前 HEAD 的代码实读复核（而非沿用文档声明）。凡文档与代码不一致处，
> 一律以代码为准并在 §9 集中列出。

> **补注（2026-09-19 复核）**：本文档的"未完成/已迁出/已取消"标注体系已完整（Lightmap 源标 **未实现**、频率分离 ❌ 已判定不需要、
> 原任务 11（含 11.3）/19/20 ➡️ 已迁至《Lumen与Nanite完整设计规范》、原任务 31 ❌ 已取消、§11.4 两项验证余额），无需另加标注。
> 唯一需要提醒的是：§4.3 等"与 UE 对照"章节里出现的 `FScreenSpaceDenoiser` / `CommonSettings` / `ESignalProcessing` / `DiffuseSphericalHarmonic` /
> `ELumenIndirectLightingSteps` / `Reblur`·`ReLAX`·`SIGMA` 等**都是 UE5 / NRD 侧的构念，用于对照说明"我们没有什么"**；本引擎**没有**集成 NRD 或 UE 的降噪/信号框架，不要把它们读成本引擎的实现。

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
    Lightmap = 2,       // 世界空间：烘焙光照（**未实现**：见 §5.1；原任务 31 已取消，见 §10.2）
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
// DeferredLighting.frag.slang —— 每个通道同构（任务 21 起与代码逐字对齐）
float3 num = float3(0.0); float den = 0.0;
for (uint i = 0u; i < bl.count; ++i) {
    GISourceSlot s = bl.sources[i];
    if (s.weight <= 0.0) continue;
    float w = SourceWeight(s, camCoverage, gridCoverage, camDist);
    num += SampleDiffuseSource(s.id, worldPos, N, uv, albedo, kD, furnace) * w;
    den += w;
}
indirectDiffuse = (bl.mode != 0u)
                ? ((den > 0.0) ? (num / den) : float3(0.0))   // 归一化：无双重计数
                : num;                                        // 相加（对照）
```

> **口径**：写的是 `(den > 0) ? num/den : 0` 而**不是** `num / max(den, 1e-4)`。二者在
> `den > 0` 时相同，但**通道里一个源都没有**（或全部被置信度判为不可信）时前者返回 0、
> 后者返回 `num/1e-4`（= 放大一万倍）。这是文档与着色器长期不一致的一处（任务 21 修正），
> 也是"空通道不该编数据"这条语义的落点。AO 通道同理，但中性值是 1（不遮蔽）：
> `aoVal = (aoDen > 0) ? aoNum/aoDen : 1.0`。

- `mode != 0`（`Normalized`）→ 除以权重和，**权重和 = 1 → 无双重计数**
- `mode == 0`（`Additive`）→ 直接相加，**仅作 A/B 对照保留**
- `GIBlendMode` 只有这两个值；设计稿中的 `Fallback`（分层回退）与
  `FrequencySplit`（频率分离）**均未实现**——`Fallback` 已确认**放弃**（可由「只用最精确的源」
  的层栈组合表达）；`FrequencySplit` 经 P5 步骤 0 实测**判定不需要**（§3.3），
  一并**放弃**。故归一化仍为唯一的生产合成模式，`Additive` 只作 A/B 对照保留。

### 3.2 权重来源

权重 = **用户权重 × 逐像素置信度 × 可选「距离让位」**。置信度判据由 C++ 逐槽写进
UBO（`GISourceSlotData::confidence`，位掩码），着色器只负责按位计算：

```hlsl
// 判据位（与 C++ GISourceConfidence 一致，见 ShaderTypes.slang 的 GICONF_*）
//   GICONF_CAMERA_COVERAGE：相机屏幕覆盖（视口外/边缘淡出 ⇒ 0）

float CameraCoverageConfidence(float2 uv, float edgeFade) {   // 带宽来自 UBO
    float2 edge = min(uv, 1.0 - uv);
    return saturate(min(edge.x, edge.y) / max(edgeFade, 1e-5));
}

float SourceWeight(GISourceSlot s, float camCoverage, float camDist) {
    float w = s.weight;
    if ((s.confidence & GICONF_CAMERA_COVERAGE) != 0u) w *= camCoverage;
    if (s.falloffDistance > 0.0) w *= saturate(1.0 - camDist / s.falloffDistance);
    return w;
}
```

**判据从哪来，为什么**：掩码由 `ToConfidenceMask(id)` 在**填 UBO 时**统一推导
（`GIChannelBlendData::Add` 内部完成，调用方无从忘记）。此前着色器里硬编码了一份
源 id 列表、C++ 侧另有一份谓词，两份真值必然漂移 —— GTAO 就曾被着色器漏掉。现在
新增源只要写对谓词，两个通道（含 AO）自动一致。

**「受相机视口限制」的源** = `IsCameraViewLimitedSource`：SSGI · SSR · SSAO · GTAO ·
RTGI · RT 反射 · RTAO。注意它**不等于**分类谓词 `IsScreenSpaceSource`：

- RSM 虽是「屏幕空间/单次反弹光栅」类，但它的产物是**光源视锥**下的 VPL 图、着色器按
  世界空间求和，与相机视口无关 ⇒ 给它乘屏幕覆盖置信度是错的，故排除在外；
- 光追三源不在 `IsScreenSpaceSource` 里，但入射方向由**本像素**出发，同样受屏幕覆盖限制
  ⇒ 必须包含。

**置信度是「相对再加权」，不是亮度缩放**：它只决定某像素信不信某个源，不信就把权重让给
同通道的其他源（`Σ(c·w)/Σw`）。两个直接推论：

1. 通道里只有**一个**源时，置信度在 `num/den` 里**精确抵消**，看不出任何效果 ——
   它的作用是「屏幕边缘只剩 IBL/DDGI」这类**多源**语义；
2. 边缘处所有源都被判不可信时，该通道权重和为 0，AO 通道取 `aoVal = 1`（不遮蔽），
   diffuse/specular 取 0（不注入）—— 这正是「屏幕外没有数据就不该编数据」。

**边缘淡出带宽 `edgeFade` 也进了 UBO**（默认 0.05）：原先它是着色器里的常量 `0.05`，
既不可配也无法验证「置信度到底有没有作用于权重」。现在它逐通道来自 `GIConfig.edgeFade`，
调大即成为可控实验（`Tools/gi/confidence_check.ps1`）。

> ⚠️ **与设计稿的差距（已收窄到两类）**：设计稿的置信度表有四类——
> 屏幕覆盖（**已落地**）、探针网格覆盖（**已落地**，任务 14：DDGI 的探针网格 AABB 之外
> 留一格淡出带、再外面归零）、RSM 光源视锥覆盖、光追收敛度/SPP。
> 后两类**刻意不声明**（掩码里没有对应的位），理由分别是：
> · **RSM 光源视锥覆盖**：着色器已按 RSM 的投影 UV 直接判无效并返回 0，无需再声明。
> · **光追收敛度/SPP**：当前没有任何逐像素收敛信息可用，声明了就是空头承诺。
>
> 探针网格覆盖为什么必须与网格拟合**一起**做：网格是固定参数时它罩不住场景，单开判据会
> 让 DDGI 在大半屏幕上归零（实测贡献 0.019594 → 1.1e-7）。所以任务 14 把两件事一起做了
> ——判据负责"没数据就别编数据"，拟合负责"让网格真的有数据"。
>
> ⚠️ **`falloffDistance` 不省性能（重要纠正）**：实测它只在两处被消费——
> `DeferredLighting.frag.slang`（合成时缩放权重，三个通道都走 `SourceWeight`）与帧图填 UBO。
> **它不改变任何 pass 是否执行**：源照旧整幅、每帧跑完，只是合成时贡献被压小。
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

| | 全掩码（92.8% 像素） | 收紧掩码（91.7% 像素） |
|---|---|---|
| DDGI / SSGI 量级比 | **2.2×** | **2.3×** |
| `corr(SSGI, DDGI)`（不低通） | **0.1480** | **0.1456** |
| `corr(LowPass(SSGI), DDGI)` 最优 | 0.1487 @ σ=1（**≈ 不低通**） | 0.1463 @ σ=1（**≈ 不低通**） |
| 最优标度拟合后 DDGI 未被解释的方差 | **98.9%** | **98.9%** |
| 最低频段（0–0.042 cyc/px）能量占比 | DDGI 99.74% / SSGI **99.42%** | DDGI 99.74% / SSGI **99.42%** |

> **数据说明（前两组数字都不可引用，务必看清）**：本表是 **SSGI-CAL（任务 10）之后**重测的。
> · `corr` 0.15/0.28、量级比 67.7×/33.3× —— 测于 **DDGI 实际不做 GI** 期间（§11.3.1）；
> · `corr` 0.9238/0.9251、量级比 21.0×、未解释方差 37.7% —— 测于 **SSGI 还不是 `E/π`
>   的估计**期间（§9.2-P）：那时它的输出近似「命中点反照率 × 常数」，与同样带接收面 albedo
>   的 DDGI 天然高度相关 —— 那个 0.92 主要来自共同乘子，而不是来自"两源在估同一件事"。
> 结论方向不变，但**理由被这次重测改写**（见下方两点判定）。
>
> ⚠️ **再更正一处（任务 14 发现、任务 17 修好）**：本节下方把 DDGI 描述成"探针网格上的平滑多次
> 弹射解"——**任务 14 实测表明当时它不是**。RSM 不在漫反射层栈时 `u_Flags.x=0`，
> `DDGI.comp.slang` 的 IBL 回退只按 `dir` 采样辐照度、根本不用 `samplePos`，而 Fibonacci 方向
> 对每个探针相同 ⇒ 整片探针的 SH 逐位一样（实测：格距 531 / 248 / 120 三种拟合的画面贡献
> 两两相差 **0.0015%**），也就是这一列 DDGI 是一个**不含场景空间信息的方向函数**。
> **任务 17 已用硬件光追 march 修好**（每条探针射线追踪真实几何）：同一条判据的读数变成
> **37.93%**，本节表格里的数字随之整体移动 —— **量级比 2.2× / 2.3× → 3.4× / 3.5×、
> `corr(SSGI, DDGI)` 0.1480 / 0.1456 → 0.6818 / 0.6815**（DDGI 现在带真实空间结构，与 SSGI
> 的相关性自然上升）。
>
> **【任务 33 之后这些数字再次整体移动】**：量级比 3.4× → **2.3×**、`corr(SSGI,DDGI)`
> 0.6818 → **0.6457**、`S_ddgi` 0.0308175 → **0.0206226**（`ddgi` HDR 0.0873536 →
> 0.0775732）。原因是**共用**的 `EvaluateHitRadiance` 量纲修正（§9.2-AC）：DDGI 探针的
> **命中**项此前漏了命中面 albedo，修后变暗 —— 方向正确（深色面反射的间接光更少）。
> **本节下面的两点判定与结论依然不变**。当前基线表见 §10.2 任务 33。
>
> **但下面的两点判定与结论不受影响**：`p5_spectrum` 的频率扫描仍然给出"不存在任何低通尺度使
> `LowPass(SSGI)` 比 `SSGI` 本身更像 DDGI" ⇒ 频率分离的前提依旧不成立，**归一化仍是正确的
> 合成方式**。表格里的具体数字请按"任务 14 之前的老 DDGI"来读。

两点判定：

1. **低通并不能让 SSGI 更好地逼近 DDGI**。`corr(LowPass(SSGI), DDGI)` 最优仅 **0.1487**
   （σ=1，≈ 不低通），σ=64 时为 0.1884 —— 低通变强时相关性略升，但始终在 0.19 以下、且
   伴随 98% 的未解释方差，谈不上"逼近"。**不存在任何低通尺度使 `LowPass(SSGI)` 比 `SSGI`
   本身更像 DDGI**。这也回答了本项原先的首选思路「用估计器自身的核做低通」——
   问题不在低通算子选型。
2. **两源同频段，但几乎不相关（不是冗余）**。最低频段能量占比 DDGI 99.74% / SSGI 99.42%
   ⇒ 频谱形状相同、都集中在最低频段；但相关系数只有 **0.148**，最优标度拟合后 SSGI 仅能
   解释 DDGI 约 **1%** 的方差。⇒ 它们是**同一个物理量的两个弱相关估计**：DDGI 是探针网格
   上的平滑多次弹射解，SSGI 是单次弹射、单半径、带屏幕空间缺失的噪声解。

**结论**：DDGI 与 SSGI 同频段，但**并非冗余**——两者弱相关、各自带不同的偏差与噪声，且按
§3.4 已在同一量纲（都是 `E/π`）。这**正是** §3.1 归一化的适用场景，即**现状已是正确的
合成方式**：频率分离的前提（`LowPass(SSGI) ≈ DDGI`）被实测否定；而"两源弱相关"还给归一化
的加权平均带来了方差下降的额外收益（改前相关 0.92 时几乎没有这个收益）。三选一的落点：

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
>
> ✅ **该阻塞点已解除（任务 10 已完成）**：SSGI 现在是真正的 `E/π` 估计，量级比由 21 倍
> 降到 **2.2 倍**，等权重下它约占输出值的 **31%**（= 1/(1+2.2)）而不是 4.5%。上面那段
> 「先标定再改合成」的判断因此已经执行完毕：合成模式**不需要改**，标定之后归一化的理由
> 反而更强（§3.3 的结论）。

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
| SSGI | 修复前 = `albedo × 命中点 albedo × cos × 假距离项`（**既不是 E/π，也没有 L_in**） | ✅ | **任务 10 已重写为 `albedo × Σ(L_in·cosθ)/Σcosθ`**（精确等于 `albedo × E/π`） |
| DDGI | **E** | ❌ | 补 `albedo/π` |
| RTGI | **E/π** | ❌（含的是命中面 albedo） | 补接收面 `albedo` |
| RSM | **≈E**（经验常数已含 1/π） | ❌ | 补接收面 `albedo`，并**解耦 `iblIntensity`** |

第 4 处（同一缺陷的另一实例）：`RayTracing/RT_GI.rgen.slang` 的 DDGI miss 回退
把**辐照度 E** 当**辐射度 L** 累加（命中路径给的是 L），已补 `× 1/π`。

**为什么白炉抓不到它**：`SampleDiffuseSource` 首行 `if (furnace) return float3(1,1,1)`
把源真值**短路**了——白炉验证的是**归一化数学**，不是源量纲。
故当时改用**单源亮度实测**验证（见 §9.2 的实测记录）。

> ✅ **该遗留已由任务 10 关闭，且顺手把白炉的盲区也补上了**：SSGI 现在返回精确的
> `E/π`（估计式是余弦加权平均，归一化常数是解析的 1，不再需要"拿 PT 标定一个增益"），
> 并且**白炉下不再被短路**——白炉条件（全白环境 + 接收面 albedo = 1）恰好是它的解析
> 真值条件，所以白炉现在**同时校验 SSGI 的标度**（实测 1.0000；把估计式临时改回
> 「除以 N」的旧形式则读数为 0.444，证明该判据不是同义反复）。
> 量级比由 21 倍降到 **2.2 倍**。其余源（IBL/DDGI/RSM/RT）在白炉下仍走短路，
> 原因见 §11.4：它们在白炉条件下的解析真值不是"估计式自然给出 1"。

### 3.5 执行模型：每帧成本结构

**执行单位是「源」。** 源分类（§2.2 的三个谓词）既不参与合成（§3.1），也不参与执行决策——
它只用于面板标签与配置诊断。

| 情况 | 成本 |
|---|---|
| 层栈里**未启用**的源 | **零**——帧图有 6 处 `NeedsPass` 门控（`DeferredPipeline_FrameGraph.cpp:318/337/357/382/429/585`），pass 根本不注册 |
| 层栈里**已启用**的源 | **每帧整幅执行**，**没有任何**分区 / 分块 / 距离剔除 |

各源每帧的实测量（**最后两列是任务 29 装上 GPU 计时后第一次拿到的真实读数**，06.GILab / Sponza / 本机）：

| 源 | pass 形态 | 规模 | 实测 GPU 耗时 |
|---|---|---|---|
| IBL | 无 pass（烘焙） | **仅脏时重建**——唯一非每帧的源 | 首次烘焙约 **5 ms**，之后不再跑（读数按"最近窗口没量到"衰减为 0） |
| DDGI | compute（探针更新）+ **光追 dispatch**（探针射线，任务 17） | 探针数**由场景包围盒自动拟合**（Sponza 实测 16×8×11 = **1408 探针**、格距 248，改前固定为 8×4×8 = 256） × `numSamples`（32）次采样。**任务 17 起这 32 条采样由 `DDGI_Trace` 用硬件光追求真实辐射度**（45056 条射线/帧）；不支持光追的设备走原来的 RSM/IBL 路径。✅ 任务 12 起可按 `updateStride` 时间维分摊 | 探针更新 **0.021 ms**（与 256 探针时的 0.019～0.025 ms 不可区分）；**另加 `DDGI_Trace` 0.039～0.041 ms**，以及**只开 DDGI 也必须付的 `AS_Build` 0.10 ms**（见下行） |
| SSGI | 全屏 offscreen | 全屏或**半分辨率** × 16–32 采样 | 16 采样 **0.436 ms**，64 采样 **1.271 ms**（随工作量线性） |
| SSR | 全屏 offscreen | 全屏或半分辨率，Hi-Z 层次 march（任务 25 修好之前**恒 miss**） | **0.254 ms**（Hi-Z）/ **0.806 ms**（线性 march）。Hi-Z 不只是更快（3.2 倍），命中的像素还更多（13.03% 对 10.22%）——修好之前它一个命中都没有，这部分成本是纯支出 |
| SSAO / GTAO | 全屏 offscreen | 全屏或半分辨率（**同一 pass 的两种模式**） | 未装读数（不属 `IGlobalIllumination`） |
| RSM | 光源视锥光栅化 + **半分辨率 VPL 求和**（任务 16 起拆成两个 pass） | 光栅化：**整个场景**从光源视角再画一遍；VPL 求和：半分辨率 × 16 点 Poisson 盘 × 2 张贴图 | 光栅化 **0.070～0.079 ms**；VPL 求和 **0.109～0.120 ms**（改前是在 Lighting 里逐**全分辨率**像素求值，**0.449 ms**；同代码放全分辨率 pass 时 **0.310 ms**，即半分辨率约为 1/2.8）。另加 Lighting 自身 +0.079 ms（多一次采样与一支源的分派） |
| RTGI | RT dispatch | **1/4 分辨率** | **0.066～0.080 ms** |
| RT 反射 / RTAO / RT 阴影 | RT dispatch | 各自 1/2 ~ 全分辨率 | RT 反射 **0.068 ms**、RT 阴影 **0.027 ms**、RTAO 未测 |
| `AS_Build` | TLAS 重建 | **整个场景每帧重建**（开了任一 RT 源、**或 DDGI 走光追 march** 时。任务 17 起后者也算，因为探针射线要打 TLAS） | **0.088～0.105 ms**（比任何单个 RT 效果都大，但绝对值仍很小） |

> **合计**：本场景（Sponza / 1920×1080 / 本机）**默认档位的全部 GI 项加起来约 0.7 ms/帧**
> （SSGI 0.44 + RT 三件 0.16 + AS_Build 0.09 + DDGI 0.02），其中 SSGI 一项占约 60%。
> 这组数字是任务 13 判定「pass 级空间剔除不值得做」的依据（见 §10.1 该行）。
> **不要把这里的逐项数字当作可比的绝对基准**：它们跨会话会整体漂移（同一份二进制连跑两次，
> `Lighting(rsm)` 读到 0.774 与 1.105 ms；不受任何改动影响的 `GB_Clear` 在 1.44 与 2.08 ms
> 之间变化）。**稳定的是比值，不是绝对值** —— 任务 16 的结论（VPL 求和搬出去后 Lighting 的
> 增量从 0.449 ms 降到约 0.1 ms）是用同一会话内的前后对照得到的。
> **只开 DDGI 的配置**（任务 17 起）还要加 `DDGI_Trace` 0.04 ms + `AS_Build` 0.10 ms；
> **RSM 在层栈里时**加 RSM 两个 pass 合计 0.18 ms —— 而它对画面的贡献目前是 0
> （§9.2-AA），这部分成本是纯支出，直到任务 30 修好量级。

> 读数口径：**只计各源的主 pass**（附属降噪不计），滚动平均约最近 10 次，且**按"最近窗口有没有量到"衰减**
> —— 所以"0"的含义是"最近没跑"（如 IBL 不脏、SSGI 不在层栈），而不是"没测"。`HE_GI_TIMING=1` 时
> 每 120 帧打一行 `[GI 耗时]` 日志，作为**面板之外**的脚本可读出口。
> **不属于任何 GI 源的 pass**（Lighting / GB_Clear / Shadow / RSM 的 VPL 求和…）不在上表口径里，
> 它们的读数由 `HE_PASS_TIMING=1` 的 `[Pass 耗时]` 给出（任务 16 加的：判断"把某一项搬出 Lighting
> 到底省了多少"必须看 Lighting 自己，只看搬出去的那一项会得出相反结论）。

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
>
> **任务归属**：第二层（原任务 19 PROVIDER-EXEC）与第三层（原任务 20 P6 + 绑定数组化）的
> **任务、迁移策略、验收判据与风险已迁到《Lumen与Nanite完整设计规范》§5.2** —— 它们的
> 验收对象只有 P6 与 Lumen。本节只保留三层改造的**设计**，不再重复任务状态。

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
| 参数 | ✅ **已可配**（任务 11.1）：`SetDepthSigma/SetNormalSigma`，默认值仍是原来的 10 / 8 | `Config{ temporalBlend, depthThreshold, normalThreshold, format, width, height, debugName }` |
| 历史 | **无**（纯空间） | `m_History` + `m_Output`，`Render` 末尾 swap 角色 |
| 输出 | `m_Denoised` 单张、语义稳定 | `GetOutput()`——**swap 后语义变**，调用方必须理解该约定 |

> **11.1 已完成的部分**：`Denoiser` 的 σ 从"着色器里的固定常量"变成可配（并集中在一处按信号
> 赋值），SSGI/SSR 两个 Provider 逐行同构的降噪附属 pass 合并为 `GI/SpatialDenoiseAux.h`
> 一份实现。**参数当前仍取默认值**，这是实测结论：把 SSGI 放宽到 2 / 2 只让高频代理再降 5%、
> 整体 std 几乎不动 ⇒ 瓶颈是 5×5 的核与缺少时域累积，不是这两个权重（详见 §10.2 第 11 项）。
> 真正的"按信号取值"要等 11.3 的框架与一条质量判据。

#### 4.4.2 六处「不统一」

1. **没有「信号类型」这个概念。** UE 的 `ESignalProcessing` 含
   `AmbientOcclusion` / `Reflections` / `DiffuseAndAmbientOcclusion` / `ShadowVisibilityMask` /
   `ScreenSpaceDiffuseIndirect` / `IndirectProbeHierarchy` / `DiffuseSphericalHarmonic` ——
   **一套框架按信号类型选滤波器、核、重建与升采样策略**。这里只有两种**通用**算法，
   信号语义完全由调用方在外部拼装。佐证：**4 个 `Denoiser` 实例的参数完全相同**，
   即同一个 σ 核被用在**漫反射间接光**与**镜面反射**这两个噪声分布与可容忍模糊度都不同的信号上。
   （11.1 已让参数**可配**并集中在一处赋值，但取值仍是同一个 —— 因为实测表明瓶颈不在这里，
   见 §4.4.1 末尾的说明。）
2. **链条形状写在调用方的 if/else 里，不是数据。** `RTProvider` 曾手工持有 `m_Temporal` +
   `m_Spatial` 两个指针，并用**位置约定**表达顺序（时域=0、空间=1，空间取"时域输出或主输出"）。

   > ✅ **11.2 已解决**：现在是一条 `std::vector<Stage>`，"顺序即执行顺序"，加一级滤波只需在
   > `SetXxxPass` 里多 push 一个。下面这段是改造前的形状，保留作对照：

   ```cpp
   [[nodiscard]] rhi::IRHITexture* GetAuxPassInput(u32 i) {
       return IsTemporalIndex(i) ? MainOutput() : TemporalOrMain();   // 时域=0、空间=1
   }
   void RenderAux(cmd, i, ...) {
       if (IsTemporalIndex(i)) { m_Temporal->SetInputs(MainOutput(), ...);    m_Temporal->Render(cmd); }
       else if (m_Spatial)     { m_Spatial->SetInputs(TemporalOrMain(), ...); m_Spatial->Render(cmd); }
   }
   ```

   而 `SSGIProvider` / `SSRProvider` 那份逐行同构的复制已在 11.1 合并为 `GI/SpatialDenoiseAux.h`。
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

#### 4.4.4 切入路径：分三步（**任务与状态已迁出**）

> **这三步的任务与状态已迁到《Lumen与Nanite完整设计规范》§5.1**（统一降噪框架）：
> 11.1（去重 + 参数可配）与 11.2（链条数据化）**已完成**，11.3（按信号类型分派）**待做**、
> 需要 Lumen 这个消费方；判据、实测读数与教训都在那一节。本节只保留**设计与现状对照**
> （即"目标形状 vs 现在的实现"），不再重复任务状态。
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
| `PipelineCaps::Forward` | 光栅阴影 + **世界空间 GI 源**：IBL（diffuse+specular）· RSM（diffuse）。**任务 26 起这三个位是真的**（Forward 已按层栈归一化合成，见下方说明） |
| `PipelineCaps::Deferred` | `Forward \| AllSources \| RT 阴影` = 光栅/光追阴影 + IBL（diffuse+specular）+ RSM + SSGI · SSR · SSAO/GTAO · DDGI · RTGI · RT 反射 · RTAO |

> **`Forward` 的能力位经历了"声明 → 撤回 → 补实现再加回"三步**（§9.2-H 及其另一半）：
> 能力位的语义是「该管线在 **GI 层栈模型**下能承载哪些源」——被声明的源会被面板放进层栈、
> 被 `Degrade` 保留，并预期由管线消费。
> · **任务 8 之前**：声明了 IBL + RSM，但 Forward 的 IBL/RSM 是管线级开关 + PBR 里硬编码相加，
>   既不读层栈也没有 `GIBlendParams` UBO ⇒ 源被放进一个没人消费的层栈，**配置说谎**。
> · **任务 8**：把声明改成"只有光栅阴影"，让声明与实现一致（零渲染风险）。
> · **任务 26**：给 `PBR.frag` 补上 `GIBlendParams` UBO 与逐通道归一化合成、
>   `ForwardPipeline` 每帧填充，于是**前向真能消费**的那三个源位被加回来。
>   仍然不声明屏幕空间源/探针/光追源 —— 前向着色没有 GBuffer，声明了就是第三次说谎。
> 判据是 `Tools/gi/forward_stack_check.ps1`（双源读数必须等于两单源的加权平均）。

`GISourceId::Lightmap` **刻意不给能力位**（任务 18 起在 `ToPipelineCap()` 里显式写成
`case GISourceId::Lightmap: return kPipelineGINone;`）⇒ `IsAvailable()` 恒为 false。
**这不是"预留"而是"明确不做"**：本文档早先把 Lightmap 描述为「预留，可用」，那是配置说谎
（§9.2 第 6 行）。真落地需要逐像素的**光照图键**（UV2 或物体 id）与一条烘焙路径，
而 GBuffer 的七个 MRT 槽位已满、拿不到新通道；改用世界坐标查表的替代方案与 DDGI 是同一个
估计量、会被 REDUNDANCY 判为冗余源。完整理由与前置条件见 §10.2 任务 18；承接它的任务 31
（Lightmap 真落地）**已取消**，取消时已落地的基础设施与两次度量教训记在 §10.2 的「31」记录里。

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
2. **权重归一化**是物理正确性的来源；距离让位只是**艺术/合成控制**（默认关闭）。
   *（校正：它**不具备性能意义**——不改任何 pass 是否执行，只缩放合成权重，见 §3.2 的纠正）*
3. **多开一个源不会变亮**；单源时行为与「二选一」时代完全一致。
4. **光追是「GI 源」而非「管线类型」**——管线能力位（架构）× 设备能力（`rtSupported`）两层判断。
5. **阴影是「可见性（乘法项）」而非「能量（加法项）」**——不进层栈。
6. **所有参与合成的源必须返回同一物理量**（`L_o = albedo × E/π`）—— ✅ **已满足**（P1·A 统一了
   四处量纲，§3.4）。*（校正与收尾：此处曾写「当前未满足」；后来发现 `EvaluateHitRadiance`
   返回的是 `albedo × E` 且直接光项连 albedo 都没有，属**同一类**问题的残留实例 ——
   RSM 那一支由任务 30 改成解析面积归一 + `L_v = albedo·lightColor·intensity·NdotL/π`；
   光追命中点那一支由任务 33 修成 `albedo/π·(E_ambient + E_direct)`，并用白炉判据锁住
   （命中与未命中都返回理想值 ⇒ 白炉恒为 1）。至此这条不变量在**实现上**也成立。）*

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
| **P1 / T** | **未产出的效果改绑中性占位纹理**（§9.2-T）：立「pass 注册结果」为单一真值 + 帧图按值捕获 + 门控通道回绑中性占位；占位改成员、RSM 占位由白改黑。告警基线 **4/4/3 → 0/0/0**，「是野指针」约 **186 → 0**；DDGI 差分贡献不变。同时发现 §9.2-U | ✅ 完成 |
| **P1 / U** | **采样脚本的陈旧转储缺陷**（§9.2-U）：崩溃/超时的运行会留下上一次的转储而分析照样出数字——一度据此虚构出"绝对读数依赖二进制布局"并追查很久。结论已推翻（只改注释重编译读数不变），缺陷改写为测量方法缺陷并修掉：运行前删除产物 + 运行后校验新鲜度 + 失败非零退出（失败路径已实测）。方法论第 4 条由此而来 | ✅ 完成 |
| **B / I·V** | **RTGI 的 miss 不再回退 DDGI**（§9.2-I）+ **时域降噪不绑管线导致启用 RTGI 即挂死**（§9.2-V）。前者恢复「源独立」：同一份 RTGI 的读数不再取决于别的源在不在层栈里（0.2034 → 0.0575，与仅 RTGI 时差 0.000%）；后者是前者的**验证前提**——不修它，任何 RT 相关测量都跑不起来。新增回归检查 `Tools/gi/rtgi_coupling_check.ps1` | ✅ 完成 |
| **B / G** | **层栈成为子系统开关的唯一真值**（§9.2-G，不变量 1）：把 `enabled` 与 halfRes 输出尺寸的对齐放进框架的 `SyncToStack`，并删掉 `06.GILab.cpp` 里那份「复发过」的手工补丁。实测 `specular={SSR}` 时 pass 列表由无 SSR 变为出现 `SSR` 与 `SSR_Denoise`；新增回归检查 `Tools/gi/stack_switch_check.ps1` | ✅ 完成 |
| **B / D** | **AO 不再压暗直接光、并开始作用于镜面**（§9.2-D）：合成顺序改为「直接光单独记下 → 间接漫反射/镜面各自累积 → 只对间接项乘 AO」。实测：纯直接光下 AO 不再改变画面（0.1007917 → 0.2021757，与关 AO 一致）；镜面 IBL 下 AO 生效（0.02649925 → 0.01478481）。§3.3 的比值/相关性**当时**不变（21.1× / 0.9234；这两个数后来被任务 10 取代为 **2.2× / 0.1480**，见 §3.3 的数据说明），白炉 1.0000 | ✅ 完成 |
| **B / F** | **RSM 的注册脱离 DDGI 门控**（§9.2-F）：门控改为两个消费方的并集（`ShouldRunDDGI() \|\| ShouldRunRSM()`），并新增 `GI_DDGI::ClearRSM` 让"本帧不注册"也成为明确结论（消除 useRSM 闩锁的残留面）。实测 `diffuse={RSM}` 且 DDGI 关时 pass 列表由无 `RSM` 变为有；新增 `Tools/gi/rsm_gate_check.ps1` 三例，A 例在改前必失败 | ✅ 完成 |
| **B / E** | **SSR 与 SSAO 的投影改用真实相机**（§9.2-E 的两个剩余实例）：Provider 把 `ctx.camera` 交给 pass，pass 用 `CameraData::GetProjMatrix()`，无相机时才退化为原来的默认投影。实测 `cam_fov=100` 时 AO 输出逐像素最大差 0.387、均值差 −0.79%；`cam_fov=60` 时与同配置重复运行的抖动同量级（0.069 对 0.078）⇒ 标准路径不变 | ✅ 完成 |
| **B / H** | **Forward 的能力位不再"声称支持但不存在"**（§9.2-H）：`PipelineCaps::Forward` 改为只含光栅阴影（GI 源位为空），新增 `AllSources` 供 Deferred 与 UI 使用；面板对无层栈 GI 源的管线显示说明并置灰层栈控件。判据为单元测试：`IsAvailable(IBL/RSM, Forward)` 为假、`Degrade(Ultra, Forward)` 后三个 GI 通道为空且兜底不发生；Forward 的 IBL/RSM 渲染不受影响（本就不读层栈） | ✅ 完成 |
| **C / §3.2** | **逐像素置信度进 UBO、判据数据驱动**（任务 9）：`GISourceSlotData::confidence` 掩码由 `ToConfidenceMask` 在 `Add` 里统一推导（着色器不再硬编码源 id 列表，GTAO 与 AO 通道一并归位），边缘带宽 `edgeFade` 从着色器常量变为 UBO 字段。实测：默认 5% 带宽下最外圈 SSGI 贡献仅为中央的 1.6%，带宽调到 50% 后 5%~15% 环带的贡献降到 32.4%（解析预测 33%）；改前二进制该检查必失败。三变体读数与白炉、各回归检查不变 | ✅ 完成 |
| **C / P** | **SSGI 成为真正的 `E/π` 估计**（任务 10 / §9.2-P）：补入射辐射度（前帧 HDR，**捕获门控改由消费者声明**）、余弦归一化、估计量改为 `Σ(L_in·cosθ)/Σcosθ`（解析归一化，无需拟合增益）、删除量纲不对的距离项；白炉下不再短路 SSGI，白炉因此同时校验它的标度。实测：白炉单源 **1.0000**（旧形式 0.444）、量级比 **21×→2.2×**、输出占比 **4.5%→31%**、与 DDGI 相关性 **0.9238→0.1480**；新增 `Tools/gi/ssgi_cal_check.ps1` 三例全过 | ✅ 完成 |
| **C / 11.1** | **降噪去重 + 参数可配**（原任务 11.1，**已迁至 Lumen 文档 §5.1**）：SSGI/SSR 的降噪附属 pass 合并为 `GI/SpatialDenoiseAux.h`；`Denoiser` 的 σ 可配并集中在一处按信号赋值。**判据**：改前/改后两个二进制、每次运行用私有 cfg 副本，`ssgi` 变体 −0.0009%、`both` 变体 +0.0002%（均在抖动内）；白炉 1.0000、单测全绿。**参数仍取默认值**是实测结论（放宽 σ 只再降 5% 高频，瓶颈是核大小与时域），并据此指出 11.3 的方向 | ✅ 完成 |
| **C / 11.2** | **降噪链条数据化**（原任务 11.2，**已迁至 Lumen 文档 §5.1**）：`RTProvider` 的 `m_Temporal` + `m_Spatial` 与位置约定改为 `std::vector<Stage>`（顺序即执行顺序），框架侧改为遍历。**判据**：三种 RT 效果的 pass 链与改造前同名同序；`rtgi_coupling_check` 0.000% PASS；三变体读数、白炉、单测不变；**临时多 push 一级即多出一个 pass、框架代码零改动**（已演示并还原） | ✅ 完成 |
| **D / AMORTIZE** | **DDGI 探针更新的时间维分摊**（任务 12）：`updateStride` + 相位轮转，未轮到的探针把历史拷进当前（ping-pong 下不拷就会倒退一代）。**判据「等量工作 ⇒ 等量结果」**：`S(4,4k)/S(1,k)` = **1.010 / 1.000 / 1.000**（k=30/60/120）；同帧下 `S(4,120)` 是 `S(1,120)` 的 **99%**（本场景约 30 次更新即收敛）。每帧样本量 8192 → 2048（**−75%**）。默认仍为 1（不 churn 基线）；新增 `Tools/gi/amortize_check.ps1`。**顺带发现 §9.2-Z**：面板的每源耗时是假的（`avgRenderTimeMs` 从未被赋值） | ✅ 完成 |
| **D / §9.2-K** | **DDGI 网格覆盖语义 + 网格自动拟合**（任务 14）：新增探针网格覆盖置信度位（网格 AABB 外一格淡出、再外面归零），并让探针网格按场景包围盒自动拟合（规则抽为纯几何 `GI/GIProbeGrid.h` + 5 例单测）。**实测**：固定网格（21×9×21 世界单位，场景却是 3720.9×1555.9×2288.2）下贡献 0.019595 → 噪声量级（2.9e-08 / −3.9e-07），即此前那个贡献**整个**来自贴边 clamp；拟合后 16×8×11 格距 248 真罩住场景，贡献回到 0.019596、既有基线逐项不变；格距 531/248/120（192/1408/9408 探针）三种拟合贡献两两相差 **0.0015%** ⇒ 探针场当前是均匀的（顺带量化了任务 17 的提升空间）；成本仍为 0.021 ms。新增 `Tools/gi/ddgi_grid_check.ps1`（4 配置 5 判定全过） | ✅ 完成 |
| **D / §9.2-Z** | **给 GI 源装真实耗时读数**（任务 29）：每源每帧一对 GPU 时间戳 → 环形查询池 → **不阻塞**读回 → 滚动平均写回源的 `GIDebugData`；`HE_GI_TIMING=1` 输出脚本可读日志。**实测**：SSGI 16→64 采样 **0.436 → 1.271 ms**（×2.92，随工作量线性）、不在层栈的源恰好为 0、重复运行离散度 **0.0%**。**第一次拿到成本结构**：IBL 首次烘焙 ~5 ms、SSGI(16) 0.44 ms、DDGI 更新 **0.019 ms**（后者解释了任务 12 为何量不出收益） | ✅ 完成 |
| **D / §9.2-J** | **跨帧重绑**（任务 15）：不止合成参数 UBO —— Lighting 的逐帧轮换资源（光源/阴影/探针 SSBO、UBO）此前全绑在**同一份**描述符集上，逐帧重绑跨帧串味。改为**每飞行帧一份描述符集 + 一份 UBO**，`LightingInputs::frameSlot` 指明本帧用哪份。**判据**：读数逐位不变、校验 56/56/56/56 不变、白炉 1.0000、单测全过。**顺带证伪**：它不是 §9.2-Y 基线分成两组的成因（改前/改后各跑 4 次，两组都照样出现） | ✅ 完成 |
| **D / CULL** | **pass 级空间剔除：实测判定不做**（任务 13）：先补齐计时覆盖（RT 源与 `AS_Build`），量出**全部 GI 项合计约 0.7 ms/帧**（SSGI 0.44 占六成，其余每项 0.02~0.09 ms）；可剔除的只有"无几何 tile"，由 albedo 判据得空像素 7.2%，且屏幕空间着色器本来就对天空像素深度早退 ⇒ **收益上限约 0.03 ms（GI 预算的 4%）**，代价是上一帧掩码造成的边界缺失条带。**不做**，并写明改判条件；新增 `Tools/gi/cost_report.ps1` | ✅ 完成 |
| **E / §9.2-AA** | **RSM 链路的量级与通道约定**（任务 30）：逐级落盘证明三张 RSM 附件此前只有清除值（`BeginOffscreenPassMRT` 的清除值长度契约被越界读破坏）、VPL 的 albedo 读错索引空间（3.18% 覆盖）；四项一起修 —— 受光项按**解析面积**归一（旧经验常数隐含"半径 60 场景"，比值 3942）、RSM 改**三个附件一个量**、辐射度带 albedo 与光源颜色、光源视锥按**场景包围盒**拟合。**判据**：三中间层 0% → **53%** 覆盖、`S_rsm` 0 → **4.28e-5**（与 `E/π×albedo` 相关 0.966）、`rsm_indirect_check` 全过 | ✅ 完成 |
| **E / §9.2-W 解析对照** | **SSR 的平面镜解析对照**（任务 32）：地面镜 + 两个已知立方体，把物体中心按镜面镜像后经同一相机投影得到"反射该出现的像素"。查出并修掉四处方向/尺度错（view/world 法线混用、y 约定两处漏翻、起点偏移小于容差导致自交、假的"仍在几何之前"命中），并按场景尺度重取 march 参数。**判据**：反射落点 **0.44 / 0.35 px**、远像素 **0.00%**、相机平移 40 后位移差 **2.39 px**、米制参数负对照 **2432 vs 30676**（`Tools/gi/ssr_mirror_check.ps1`） | ✅ 完成 |
| **E / §9.2-AC** | **光追命中点的量纲**（任务 33）：共用的 `EvaluateHitRadiance` 缺命中面 albedo、整体缺 1/π；改为返回 `albedo/π·(E_ambient + E_direct)` 并加**白炉分支**（命中与未命中都返回理想值 ⇒ 读数与几何无关）。**判据**：白炉 `diffuse={RTGI}` 中心 **1.0000**（负对照 0.0000）、SSGI 仍 1.0000、RTGI/RT 反射原始输出比 0.209 / 0.541，DDGI 绝对读数方向正确地下移 | ✅ 完成 |
| **E / §9.2-AD + §9.2-AF** | **Forward 的 RSM 真有生产者**（任务 34）：四层根因 —— 示例不驱动阴影系统（Forward 此前连阴影都没有）、RSM 用相机视锥的 CSM VP 且帧图无序、RG 路径从不绑定 RSM 纹理、以及共享结构体布局漂移（`float[3]` 在 std140 里占 48 字节 ⇒ `rsmValid` 恒读 0）。**判据**：`forward_stack_check` **10 条全过** —— `S_rsm` 恰好 0 → **8.875e-05**、三张 RSM 图覆盖 53.36%/53.36%/53.31%、换相机后逐字节相同；`GIBlendParams` 的 `sizeof` 与三个 `offsetof` 已被静态断言钉死 | ✅ 完成 |
| **E / §9.2-AE** | **SSR 的 Hi-Z 路径漏反射**（任务 35）：Hi-Z 把屏幕段参数当成射线参数用（实测在目标像素处偏差 **1251 / 1481** 世界单位 = 容差的 100 倍以上），改用透视校正的 `w(t)`/`tau(t)` 精确换算；并按实测把默认步数预算提到 256。**判据**：`ssr_mirror_check` **13 条全过**（两条路径都落在预测像素 0.44/0.35 px、远像素 0.00%、步数 256 ≤ 600 的 60%）。**同时更正**任务 25 的"Hi-Z 快 3.2 倍"（实测 4.256 对 4.505 ms） | ✅ 完成 |
| **E / §9.2-AG** | **MRT 帧缓冲写死 7 个颜色附件**（RHI，第 8 个 GBuffer MRT 接入时暴露）：附件数组与循环都写死 7，而 render pass 按 PSO 的 `colorAttachmentCount` 建（上限 8）⇒ `vkCreateFramebuffer` 与 render pass 附件数不一致，驱动在 `vkCmdBeginRenderPass` 崩溃。改为按"颜色上限 + 1（深度）"开数组、循环用 `kMaxColorAttachments` | ✅ 完成 |
| **A / D1** | **偶发崩溃根因获证 + 崩溃处理器**（任务 2）：`CrashHandler` 移到 `Engine/Core`（VEH + 分阶段报告 + minidump + 看门狗 + 线程表 + 栈扫描兜底），并修掉那条**可复现**的退出期崩溃（`JoltRuntimeGuard`：Jolt 的 `Free` 在静态析构时为空）。**判据**：自检 2 s 退出 / 14.3 MB dump / 4.7 KB 报告含 `06.GILab!main + 0x276B [06.GILab.cpp:853]`；过滤单跑 175/175 退出码 0；soak 76 次启动零崩溃 | ✅ 完成 |

### 8.2 三个关键指标（实测）

| 指标 | 当前 | 目标 |
|---|---|---|
| 每帧 Vulkan 校验违规（四类） | **0 / 0 / 0 / 0** | 0 |
| **白炉读数（绝对亮度）** | **1.0000** | 1.0（±2%） |
| 偶发崩溃 | **76 次启动零崩溃**（12 + 24 + 40 soak）；另有一条**可复现**的退出期崩溃已定位并修掉（单跑用例 161/161 崩 → 175/175 干净退出） | 持续为 0 **且根因获证** —— 可复现那条已获证（§10.2 任务 2），历史那条偶发违例按测量结案、留有重开条件 |

白炉读数由 **1.7998 → 1.0000**（修复「IBL 在归一化之外」与「RSM 旁路加法」两个缺口后达成）。

### 8.3 已明确不做 / 已划掉

| 项 | 结论 |
|---|---|
| M4.5 GBuffer 通道合并 | ⏭️ **不适用**：metallic/roughness 嵌在需 16 位精度的 MRT0/MRT1 alpha 通道，拆成独立 MRT 反增带宽 |
| M5.1 RTGI 时域累积 | ✅ **已划掉**：由 S1 的 `RTDenoiser`（velocity 重投影 + 去遮挡）覆盖；rgen 保持 SPP=1 是正确设计 |
| `GIBlendMode::Fallback`（分层回退） | ⛔ **放弃**：可由「只用最精确的源」的层栈组合表达 |
| `GIBlendMode::FrequencySplit`（频率分离） | ⛔ **放弃**：P5 步骤 0 实测**判定不需要**——不存在使 `LowPass(SSGI) ≈ DDGI` 的低通尺度，两源本就同频段（§3.3） |
| Lightmap 源（本轮） | ⛔ **本轮不落地**（任务 18）：需要给 GBuffer 加光照图 UV 通道 + 一条烘焙路径，且要与 DDGI 划清分工（否则是重复估计）。已把文档里"预留，可用"的**虚假声明**改成"明确不做 + 前置条件"，并用单测锁住"未实现的源不可能进层栈"。改判路径（原任务 31）**已取消** —— 已落地的基础设施与取消原因见 §10.2 |
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
| 6 | `Lightmap` 可用性 | 「预留，可用」 | `ToPipelineCap` 无该分支 → `IsAvailable` 恒 false，面板选不到。**已按「把声明改对」解决**（任务 18）：`ToPipelineCap` 里显式写成不给能力位（理由是它落不了地，见 §10.2 任务 18），§5.1 改写为"明确不做"；单测锁定「即使被塞进层栈，`Degrade` 也会裁掉它」（承接它的任务 31 已取消，但该声明与单测与任务无关、继续有效） |
| 7 | **`Tests` 的可测性前提** | 「`Tests` 不链接 Render 模块，故 GI 无法被单测」（依据 `Tests/CMakeLists.txt:48-54` 的直接列表） | **不成立**：直接列表虽无 Render，但 `HugEngineAI`(PUBLIC) → `HugEngineRender` + `HugEngineEditor` → `HugEngineRender`，**传递依赖早已把 Render/RHI/Vulkan 拉入**，`Engine/Render` 也已在包含路径上。GI 本就可测；抽 `GITypes.h` 的真实价值是**分层解耦**（纯数据头不再拉全量 RHI）与**显式依赖**，而非「否则测不了」 |
| 8 | §4.2「帧图 0 行」 | 新增一种源，帧图 0 行 | **只在"落在已有 pass 类别内"时成立**。帧图实有 **7 条按 source id 定制的循环**（§4.3.1），引入新类别需新增循环。已在 §4.2 就地加限定条件 |
| 9 | `IGIProvider::GetPassKind()` | §4.1 把它列为「调度」：决定帧图如何注册本源 pass | **实际是死接口**：`IGIProvider.h:63` 声明（默认 `Offscreen`）、`DDGIProvider.h:34` 覆写为 `Compute`，**全仓无任何读取点**——帧图实际按 source id 硬编码选择 pass 形状（§4.3.2） |

### 9.2 代码复核发现的缺陷（A…Z、AA…AD；逐行标注已修 / 待修）

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
| ~~**D**~~ | ✅ **已修复** | **AO 乘到了直接光上，且不作用于镜面**：`color *= lerp(1, ao*aoVal, aoIntensity)` 位于直接光累加之后、间接镜面之前 ⇒ (1) **每帧把直接光按材质 AO 压暗一次**（能量错误，不是遮蔽近似），(2) 间接镜面完全不受 AO 影响。修法：先把直接光记下（`directColor`），间接漫反射与间接镜面各自累积，最后 `color = directColor + (indirectDiffuse * giIntensity + indirectSpecular) * aoFactor`，自发光同样不受遮蔽。**实测指纹**：纯直接光 + `AO={SSAO}` 读数 **0.1007917 → 0.2021757**，与「AO 栈为空」**完全一致** ⇒ AO 不再碰直接光；只留镜面 IBL（并关掉直接光）+ `AO={SSAO}` 时 **0.02649925 → 0.01478481（×0.558）** ⇒ AO 开始遮蔽镜面。回归：DDGI 差分不变（0.0195947），§3.3 的比值与相关性不变（`p5_spectrum`：21.1× / corr 0.9234），白炉 1.0000；绝对基线随之变化（`none` 0.0472 → 0.0577，`max` 19.68 → 42.20） | `Lighting/DeferredLighting.frag.slang` |
| ~~**E**~~ | ✅ **已修复** | **屏幕空间源用硬编码默认投影矩阵**而非真实相机：`kDefaultFOV=60°/0.1/2000`；`PhysicalCamera` 会由焦距反算 fov → 非默认相机下 SSGI/SSAO/SSR 重建错位。根因是 `IGIProvider` 未把相机传给屏幕空间源（只有 DDGI 有 `SetCamera`）。**三个实例全部修完**：SSGI（见 M/N/O）、SSR 与 SSAO（本次）。修法与 SSGI 同源：Provider 把 `ctx.camera` 交给 pass，pass 用 `CameraData::GetProjMatrix()`，无相机时退化为默认投影。**实测指纹**（单源层栈、Frame=120、对 AO 输出纹理逐像素对照）：`cam_fov=60`（默认）时改前改后只在**运行间抖动**范围内（最大差 0.069，同配置重复运行对照 0.078）⇒ 标准路径不变；`cam_fov=100` 时最大差 **0.387**、均值差 **−0.79%** ⇒ 硬编码常量与真实相机不一致时确实错位 | `GI/GI_SSR.{h,cpp}`、`GI/SSRProvider.h`、`PostProcess/SSAO.{h,cpp}`、`GI/AOProvider.h`；采样设施补 `cam_fov` 配置项与镜面/AO 落盘 |
| ~~**F**~~ | ✅ **已修复** | **RSM 的 pass 被嵌套在 DDGI 门控内**：单独勾选 RSM 而关闭 DDGI 时，RSM 永不注册（Forward 侧却是独立的 `ShouldRunRSM()`）。RSM 实际有**两个独立消费方**——Lighting 的漫反射间接光（`ShouldRunRSM()`）与 DDGI 探针的世界辐射度来源——门控应取并集。**实测指纹**：`diffuse={RSM}` 且 DDGI 关时 pass 列表里**没有 `RSM`**，改后出现；同时新增 `GI_DDGI::ClearRSM`，让"没注册"也成为一个**逐帧明确结论**（`useRSM` 是由两个纹理成员推导的闩锁，此前"不注册就什么都不做"会把上一帧的绑定留在原地，是 §9.2-R 的残留面） | `DeferredPipeline_FrameGraph.cpp`；`GI/GI_DDGI.{h,cpp}`；`06.GILab.cpp`（新增 `gi_blend_diffuse_rsm` 配置键，此前 RSM 只能靠面板勾选）；回归检查 `Tools/gi/rsm_gate_check.ps1` |
| ~~**G**~~ | ✅ **已修复** | **层栈与子系统开关是两套真值**（不变量 1 的实际状态）：子系统 `enabled` 只在管线 `Initialize` 时按当时的层栈算**一次**，之后层栈再变（配置加载 / 预设 / 面板）就与子系统脱节 ⇒ `IsValid()` 为假、pass 不注册，而层栈仍以正权重把它计入归一化：**勾选却静默失效**。`halfRes` 是第三重（要等下次 `OnResize` 才重建纹理）。**实测指纹**：`specular={SSR}`（其余层栈为空）时，pass 列表里**完全没有 SSR**；把「层栈 → 开关」的对齐交给框架后，`SSR` 与 `SSR_Denoise` 都出现。**「复发过」的实体已找到**：`06.GILab.cpp` 里有一份手工补丁逐个子系统同步开关 —— 把不变量的维护放到调用方，必然有下一个忘记同步的调用方。修法：在 `IGIProvider::SyncToStack`（帧图构图前每帧调用）里对齐 `enabled` 与输出尺寸，并删除那份手工补丁 | `GI/AOProvider.h`、`SSGIProvider.h`、`SSRProvider.h`、`DDGIProvider.h`；`GI_SSGI.{h,cpp}`、`GI_SSR.{h,cpp}`；`06.GILab.cpp`；回归检查 `Tools/gi/stack_switch_check.ps1` |
| **H** | 中 → ✅ **已按「把声明改对」解决** | **Provider 抽象只在 Deferred 落地**：`ForwardPipeline` 无 `m_GIProviders`，Forward 的 PBR shader **没有 `GIBlendParams` UBO** → 层栈归一化在 Forward 完全不存在。**但 `PipelineCaps::Forward` 却声明支持 IBL + RSM** ⇒ 面板与 `Degrade` 会把这些源放进一个**没有任何代码消费**的层栈里：配置说谎。**修法取「把声明改成实际支持的范围」**：`PipelineCaps::Forward` 现在**只有光栅阴影**（GI 源位为空），新增 `PipelineCaps::AllSources` 汇总全部 GI 源位供 Deferred 与 UI 使用。判据是单元测试（`IsAvailable(IBL/RSM, Forward)` 必须为假、`Degrade` 后 Forward 的三个 GI 通道必须为空且兜底不得发生）；GILab 面板在本管线无层栈 GI 源时显示说明并**置灰**层栈控件（内容仍可查看，但不可编辑）。「让 Forward 真正走层栈归一化」拆为独立任务 26 —— **任务 26 已完成**（`GIBlendParams` UBO + 逐通道归一化，见 §10.1） | `GITypes.h`（`PipelineCaps`）；`ForwardPipeline.cpp`（注释）；`06.GILab.cpp`（面板置灰 + 候选源按能力位过滤）；`Tests/TestGITypes.cpp` |
| ~~**I**~~ | ✅ **已修复** | **RTGI 用 DDGI 做 miss 回退**，破坏「源独立」前提：Ultra 档同时含 RTGI+DDGI 时，DDGI 信息被用两次再归一化 → 加权平均失去无偏性。**实测指纹**：同一帧、同一相机，只改漫反射层栈 —— `diffuse={RTGI}` 时 RTGI 原始输出均值 **0.0575**，`diffuse={DDGI,RTGI}` 时 **0.2034**（3.5 倍）⇒ 同一份 RTGI 的读数完全由「DDGI 在不在层栈里」决定。修法：把「DDGI 是否是层栈源」作为每帧状态交给 rgen（`flags` bit1），是则 miss 贡献 0、否则保留回退（降级路径）。修后两者均值 **0.0575 对 0.0575（差 0.000%）** | `RT_GI.rgen.slang`；`RTEffectPass.h`；`RTGIPass.cpp`；`RTProvider.h`；`DeferredPipeline_FrameGraph.cpp`；回归检查 `Tools/gi/rtgi_coupling_check.ps1` |
| ~~**V**~~ | ✅ **已修复** | **时域降噪在 `BeginOffscreenPass` 之前没有绑管线** ⇒ 建出附件数与 RenderPass 不符的 Framebuffer（`VUID-VkFramebufferCreateInfo-attachmentCount-00876`：1 个附件对 2 个附件），**设备直接挂住、进程再也不推进**。表现为「只要 diffuse 层栈含 RTGI 就稳定挂死在第 1 帧」，因此**任何 RT 相关的测量都做不了**（任务 3/17/20 都被它挡住）。根因：帧图的附属 pass 先 `BeginOffscreenPass`（用它取 RenderPass 建 Framebuffer）再 `RenderAux`，而 `RTDenoiser` 自己不绑管线、由 `Render` 内部另起一个 pass。修法：`RTDenoiser` 增加 `PreBind`（绑 PSO/视口/裁剪/描述符集），`Render` 不再自起 pass，`RTEffectProvider::PreBindAux` 的时域分支改为调用它 | `PostProcess/RTDenoiser.{h,cpp}`；`GI/RTProvider.h` |
| ~~**J**~~ | 低 → ✅ **已修复**（任务 15） | 合成参数 UBO 是**单份**、非 per-frame-in-flight（`MAX_FRAMES_IN_FLIGHT=3`）。**修法比这一行记的更宽**：Lighting 的一批逐帧轮换资源（光源/阴影/探针 SSBO、合成参数 UBO）此前全绑在**同一份**描述符集上 ⇒ 改为**每飞行帧一份描述符集 + 一份 UBO**，`LightingInputs::frameSlot` 指明本帧用哪份。判据：读数逐位不变、校验条数不变、白炉与单测全过（见 §10.1 的任务 15 行） | `LightingPass.{h,cpp}`、`DeferredPipeline_FrameGraph.cpp` |
| ~~**K**~~ | ✅ **已修复**（任务 14） | **DDGI 网格外查询退化为「贴边常数外推」**，无 falloff 或无效标记；探针网格为固定参数，覆盖不到的区域静默缺失低频 GI。**修法两半一起做**：(1) 新增「探针网格覆盖」置信度位（`kGIConfProbeGrid`，网格 AABB 外一格起线性淡出、再外面归零，权重让给同通道其他源）；(2) 探针网格按场景包围盒自动拟合（每 30 帧重算；拟合规则抽为纯几何 `GI/GIProbeGrid.h` 并被单元测试覆盖）。**实测**：固定网格 8×4×8 格距 3 只覆盖 21×9×21 世界单位，而场景包围盒是 3720.9×1555.9×2288.2 ⇒ 打开覆盖语义后 DDGI 贡献 **0.019595 → 噪声量级**（两次运行 2.9e-08 / −3.9e-07），即此前那个贡献**整个**来自 clamp（`SampleDDGI` 对超界查询把 8 个采样坐标全钳到同一个边界探针）；拟合后网格 16×8×11 格距 248 真罩住场景，贡献回到 **0.019596**，与改前逐位一致。回归检查 `Tools/gi/ddgi_grid_check.ps1`（4 配置 5 判定全过） | `GI/GITypes.h`（`kGIConfProbeGrid`）；`GI/GIProbeGrid.h`（新）；`GI/GI_DDGI.{h,cpp}`；`ShaderTypes.slang` + `Lighting/DeferredLighting.frag.slang`（`ProbeGridConfidence`）；`Pipeline/DeferredPipeline{,_FrameGraph}`；`Samples/06.GILab`；`Tests/TestGIProbeGrid.cpp`（新） |
| ~~**L**~~ | ✅ **已修复** | **SSGI 完全不生效**：`SSGIProvider` 把接口覆写写成了 `SetGBuffer`，而基类 `IGIProvider::SetInputs` 带**空实现的默认体** → 改名既不报错也不警告，静默落到空实现 → `m_Depth/m_Normal/m_Albedo` 恒为 `nullptr` → `GI_SSGI::Render` 在守卫处提前返回，SSGI 输出纹理只剩清屏值 `(0,0,0,1)`。表现为「SSGI 已启用、`IsValid()` 为真、面板一切正常，但对画面的贡献恒为 0」 | 见下方「L 的修复与实测」 |
| ~~**M**~~ | ✅ **已修复** | **SSGI 的 TBN 变换方向反了**（L 修好后暴露的主因）：`mul(TBN, 样本)` 算的是 `(T·v, B·v, N·v)`——把切线空间样本**投影到** TBN 轴上，而非变换到本空间。结果采样方向几乎与法线垂直，半球采样失效 | 见下方「M/N/O 的修复与实测」 |
| ~~**N**~~ | ✅ **已修复** | **SSGI 世界/view 空间混用**：`sDir` 由 GBuffer 的**世界空间**法线构造，却被加到 **view 空间**的 `viewPos` 上（`sPos = viewPos + sDir × radius`）。同时属 §9.2-E：SSGI 用硬编码默认 FOV/near/far 自拼投影矩阵 | 同上 |
| ~~**O**~~ | ✅ **已修复** | **SSGI 可见性判据方向相反**：view 空间朝 −Z，未被遮挡应为 `sZ <= sPos.z + bias`，原写作 `sZ >= sPos.z - 0.01`，等于只累积**被遮挡**的样本 | 同上 |
| ~~**P**~~ | ✅ **已修复**（任务 10） | **SSGI 的累加项不是入射辐射度**：只累加命中点的**反照率**（不含任何 `L_in`），余弦项用未归一化的 `sDir`（把 cos 项与样本长度混在一起），距离项还写成了长度的平方。故 SSGI 长期不是 `E/π` 的估计（§3.4），在归一化里只占输出值的约 4.5%。**修法**：`L_in` 取前帧 HDR（`GIRadianceHistory`，由 Provider 声明消费者、帧图据此捕获），余弦改为 `dot(N, normalize(sDir))`，估计量改为 **`Σ(L_in·cosθ) / Σcosθ`** —— 由 `∫cosθ dω = π` 可知它精确等于 `E/π`，**归一化常数是解析值 1、无需标定**；量纲错误的那一项直接删除（固定半径采样对 L 的估计偏差改记为已知近似，不再用经验项掩盖）。**实测指纹**：白炉下 SSGI 单源读数 **1.0000**（改回「除以 N」的旧形式则为 **0.444**）；单源做差量级比 **21× → 2.2×**；SSGI 在输出中的占比 **4.5% → 31%**；与 DDGI 的相关性 **0.9238 → 0.1480**（§3.3） | `GI/SSGI.frag.slang`；`GI/GI_SSGI.{h,cpp}`；`Lighting/DeferredLighting.frag.slang`（白炉不再短路 SSGI）；`GI/IGIProvider.h`（`NeedsRadianceHistory`）；`DeferredPipeline_FrameGraph.cpp`；`GIRadianceHistory` |
| ~~**Q**~~ | **高** → ✅ **已修复** | **IBL 的辐照度/预滤波图在「IBL 不在漫反射层栈」时从不烘焙**，而帧图仍把它们交给 specular 通道与 DDGI 探针使用 ⇒ **未初始化显存被当作光照数据采样**。门控用的是 `IBLProvider::NeedsPass(m_GIConfig.diffuse)`，但消费者有三个（diffuse / specular / DDGI 的辐射度回退 `GI_DDGI::SetIBL`） | 见 §11.3.1 的定位与修复 |
| ↑ | 续（Q） | **同一族的第三、第四个实例**，都被后来的任务逮到 —— 说明"手工列举消费者"这条路走不通：任务 26 发现 **Forward 侧连 `SetIBLSkybox` 都没有人调**（烘焙的是未设置的天空盒，间接光恒为 0，见 §9.2-AB）；任务 27 发现门控**还漏了第四个消费者 —— `u_BRDF_LUT` 是 PBR 在直接光路径上无条件采样的**，镜面栈为空时那张 LUT 从未写入 ⇒ 直接光的 BRDF 读未初始化显存（见 §9.2-X）。**结论**：这类"一份产物 + 多个消费者"的东西不再按消费者清单门控 —— IBL 烘焙现在**恒注册**，由 pass 内部的 `IsDirty()` 早退（不脏时耗时读数为 0） | 见 §9.2-AB、§9.2-X 与 §10.2 任务 26/27 |
| ~~**R**~~ | **高** → ✅ **已修复** | **DDGI 的 `useRSM` 是只置位、永不清除的闩锁**：帧图把 RSM 的 position/flux 图交给 DDGI 是**无条件**的，而 RSM pass 的注册条件是「RSM 在漫反射层栈里」⇒ 在「DDGI 开 + 有活动阴影 + RSM 不在漫反射栈」时，DDGI 永久走**从未渲染**的 RSM 路径，全部探针样本无效，落入着色器硬编码兜底 —— **DDGI 表面正常却完全不做 GI** | 同上；判定指纹：贡献的 R:G:B 恰为 1:1.5:4（= 兜底常数 `(0.02,0.03,0.08)`） |
| ~~**S**~~ | ✅ **已修复** | **RHI 允许纹理以未初始化状态被采样**：没有任何默认零初始化或有效性标记，于是一个「消费者门控写漏」就能被放大成**静默的物理错误 + 跨构建不可复现的读数**（Q/R 两项正是这样被放大的）。修复采取**检测 + 一次性告警**而非改写纹理内容（见下方「S 的修复与实测」） | `Engine/RHI/RHI/TextureLayoutTracker.{h,cpp}`；`VulkanDevice_Descriptors.cpp`；`VulkanCommandList.cpp`；`VulkanCommandList_RenderPass.cpp`；`VulkanResources.{h,cpp}` |
| ~~**T**~~ | ✅ **已修复** | **「效果本次未产出」时描述符仍绑定真实纹理**：`LightingPass` 无条件把 RSM 位置/通量图、SSGI 输出、聚光灯阴影图等绑到 set=0，而这些 pass 的门控是「是否在层栈里 / 是否有该类光源」⇒ 这些描述符指向**从未被写入**的纹理。当前无害（着色器按权重与光源数自行守卫），但只要守卫被改动就会读到未初始化显存 —— 正是 S 所描述的危险形态。**由 S 的检测机制在 06.GILab 首次跑出**，固定 4 处。修复见「T 的修复与实测」 | `LightingPass.cpp`；`Shadow/IShadowSystem.h` + `ShadowSystem`；`DeferredPipeline_FrameGraph.cpp` |
| ~~**U**~~ | 中 → ✅ **已修复**（任务 24） | **测量方法缺陷：崩溃/超时的运行会留下"上一次的转储"，分析脚本无法分辨**。`HE_DUMP_GI` 只有在跑到目标帧才落盘；运行崩溃或超时时，`gi_<tag>_*.f16` 只是**上一次**运行留下的旧文件，而后续分析会把它当成本次读数照样出数字。本次即因此把一个**不存在的"绝对读数依赖二进制布局"**当成缺陷追了很久：`rep1/rep2/bis_none/ibl_none` 四次"暗读数"全部取自崩溃运行（这四次都没有落盘），而唯一两次**真实**读数（改动前 0.0327534、改动后 0.0472109，都是 crash=0/dumped=1）之间的差异另有原因（任务 23 的占位纹理存活期修复）。**已修**：采样脚本改为"运行前删除该标签产物 + 运行后校验转储存在且晚于本次启动 + 失败则不分析并以非零码退出"，并实测了失败路径 | `Tools/gi/dump_gi.ps1`；同时见 §11.3.1 的方法论条目 |
| ~~**W**~~ | **高** → ✅ **已修复**（任务 25） | **SSR 一个命中都没有**：新增的镜面通道落盘显示 SSR 输出**逐像素全为 0**，且有效性 alpha **100% 为 −1**（= 全部 miss），改前改后皆然。因此镜面层栈里"有 SSR"与"没有 SSR"在画面上完全等价 —— 与 §9.2-L（SSGI 恒为 0）同一种失效形态。**根因不止一处，三处独立叠加**：① Hi-Z 的深度判据方向反了（金字塔是 **min** 深度 = 每格最近的几何，而引擎是 **zero-to-one**；正确判据是"射线点深度**小于**该格最小深度 ⇒ 该格空 ⇒ 前进"，代码写的是大于）；② **层级步长方向也反了**（`stepT = 1/2^level` 在 level 0 一步跨完整条射线，第二轮就越界退出 —— 层级是**屏幕空间**的金字塔，步长必须折算成 `2^L / 屏幕段长(像素)`）；③ level 0 的命中阈值把 **NDC 深度差**与 `thickness*0.1`（世界单位）比，量纲不对。另外线性回退路径把"射线仍在几何之前"（`rayPos.z > rpZ`）记成命中 0.5：把它单独放回去会让有效像素从 10.22% 涨到 **73.91%**，也就是说历史文档里那个"线性 march 有 34.66% 有效命中"正是被这条假命中抬起来的。**【任务 32 的后续】任务 25 的"有效率 + 两条 march 同量级"只证明了内部一致，随后用平面镜解析真值又逮到三处它看不见的错**（世界/view 空间法线混用、y 约定两处漏翻让重建几何上下镜像、起点偏移小于命中容差导致自交），并发现**默认 Hi-Z 路径仍漏掉一个反射**（§9.2-AE / 任务 35） | 见下方「25 · §9.2-W 的修复与实测」与「32 · SSR 的解析对照（平面镜）」 |
| **X** | 中 → ✅ **已修复**（任务 27） | **镜面层栈为空时画面出现 463 量级的异常亮点**：三通道全空 `mean 0.2022 / max 463.32`；只放 `specular={IBL}` `0.0694 / 42.20`；只放 `ao={SSAO}`（specular 仍空）⇒ 回到 `0.2022 / 463.32`。**根因不在镜面通道的合成里**（空栈时那里给 0），而是 **IBL 烘焙门控漏了第四个消费者**：`u_BRDF_LUT` 由 PBR 在**直接光路径**上无条件采样，而烘焙 pass 只在「漫反射栈要它 ∨ 镜面栈要它 ∨ DDGI 要它」时注册 ⇒ 镜面栈为空且 IBL 不在别处时 LUT 从未写入，直接光的 BRDF 读未初始化显存。**所以它不是一个亮点，而是整幅画面的直接光都算错了**（均值 0.2022 → 修后 0.0429，4.7 倍）。修法：IBL 烘焙**恒注册**、由 pass 内部 `IsDirty()` 早退 | 见下方「27 · §9.2-X 的修复与实测」 |
| **Y** | 中 → ✅ **已修复**（任务 28） | **同一份「写进配置文件的键」在不同写法下落到了不同的兜底路径**：`dump_gi.ps1` 与标定脚本都要产出「漫反射层栈为空」的基线，两者写出的 `gi_blend_diffuse_w0..w3` 全为 0 的配置**逐字节等价**，但实测基线稳定地分成两组 —— 0.05720 与 0.07554（后者运行后配置文件被回写成 `gi_blend_diffuse_w0=1.000000`）。**根因（任务 28 查清）**：`GIRegistry::Degrade` 末尾有一段**静默兜底**——某个通道被裁空时补一个 IBL（AO 补 SSAO）。于是"空层栈"这个状态**取决于经过哪条路径**：配置加载路径直接重建层栈（空就是空），而任何经过 `Degrade` 的路径（预设按钮、阴影下拉）都会把它补成 `{IBL}`；示例退出时把**内存里那份**回写成文件 ⇒ 下一次运行读到被补过的配置。**修法**：`Degrade` 只裁不加（空通道是有定义的合法状态，见 §3.1），同一份输入在任何路径上同义；单测锁住"只裁不加 + 幂等"。**修后判据**：同一份配置连跑 5 次，读数离散 **0.0003%**、cfg 的层栈键逐键不变（`Tools/gi/repeatability_check.ps1`）。**另一条历史观察同时降级**：早期"同一二进制同一 cfg 分成 0.057733 / 0.05719x 两组（差 1%）"今天**未复现**，最可能是当时尚未装 §9.2-U 的陈旧转储护栏 | 见下方「28 · §9.2-Y 的修复与实测」；另见 §11.3 的采样注意事项（每次对照必须用私有 cfg 副本） |
| **AA** | 高 → ✅ **已修复**（任务 30） | **RSM 间接光整条链路实际不产出**。改前指纹：`S_rsm = lum(HDR{diffuse=RSM}) − lum(HDR{空漫反射栈})` **逐像素为 0**（连 1e-8 都取不出来），而 `RSM` / `RSM_Indirect` 两个 pass 的耗时照付（0.086 / 0.108 ms）—— 与 §9.2-L/W/X 同一种失效形态。**根因是三层叠加，前两层让"链路根本不产出"，第三层让"产出了也看不见"**：① **三张 RSM 附件里只有清除值**（逐级落盘看到 `(0,0,0,1)` = `ClearValue` 默认值 ⇒ 一个片元都没通过）。根因是 `BeginOffscreenPassMRT` 的清除值**长度契约**（`clears` 需 colorCount 个颜色项 + 末尾一个深度项），调用点写的是 `ClearValue clears[2]` 而 colorCount=2 ⇒ **越界读栈上垃圾当深度清除值**，深度清成使 `LessEqual` 全失败的值。**已修**：给足 4 项，并把契约写进 `RHI/CommandList.h` 的接口注释。② **VPL 的 albedo 读到别人的索引空间**：`RSM_Generate.frag` 从 `u_Objects[objectIndex].baseColorFactor` 取漫反射率，而对象缓冲是**相机可见性列表**（只写可见物体、索引是相机列表下标），本 pass 遍历的却是全部网格、索引用自己的计数器 ⇒ 实测只有 **3.18%** 的 texel 有非零 albedo（且与 `N·L>0` 的 53.31% 一比即知：非零辐射度恰好是两者的交）。**已修**：albedo 改走 push constant（`vplAlbedo`），本 pass 改用自己持有的 `GPUObjectData[]`。③ **量级归一与覆盖面都是场景尺度相关的**：光源视锥硬编码 `sceneCenter=(0,3,0)/sceneRadius=60`（只罩住 3720 单位宽场景的 1/60），而能量常数 `RSM_VPL_ENERGY = 0.046875` 数值上恰好等于"一个 RSM texel 在半径 60 光锥下的世界面积"（`(120/512)²=0.0549`，比 0.85）—— 把整项钉死在一个隐含的 60 单位场景上。**已修**：光锥按场景包围盒拟合（`GI/RSMFrustum.h`，纯几何 + 单测），能量归一改成解析面积 `scale = (radiusUV·2·halfExtent)²/N`（每个采样点代表的世界面积 / π），与旧常数之比 **3942**；并顺带①修掉 `flux` 的通道约定（RSM 从"两个附件塞三个量"改成**三个附件一个量**，DDGI 不再把编码法线当辐射度读，见 `ShaderTypes.slang` 的「RSM 贴图通道约定」）②把通量从灰度标量变成**带 albedo 与光源颜色的辐射度** `L_v = albedo·lightColor·intensity·NdotL/π`。**修后判据**：`rsm_pos` 覆盖 0% → **53.36%**、`rsm_rad` 非零 0% → **53.31%**（均值 0.205）、`rsm_indirect` 非零 0% → **54.62%**、`S_rsm` 0 → **4.28e-5**（与 `E/π×albedo` 逐像素相关 **0.966**）。**量级仍只占屏幕均值 0.06%，这是估计量本身的性质**（2.5D RSM 里接收点与采样到的 VPL 大多共面 ⇒ 两个余弦同时趋零，CPU 重算同一求和得中位数 0、仅 17% 接收点非零；把采样盘半径扫 10 倍均值只在 2.5 倍内波动），故判据守结构不守绝对量级 | `GI/GI_RSM.{h,cpp}`、`GI/RSMFrustum.h`（新）、`GI/RSMIndirect.{h,cpp}`、`Shader/GI/RSM_Generate.{vert,frag}.slang`、`Shader/GI/RSM_Indirect.frag.slang`、`Shader/GI/DDGI.comp.slang`、`ShaderTypes.slang`、`Pipeline/DeferredPipeline_FrameGraph.cpp`、`Pipeline/ForwardPipeline{,_FrameGraph}.cpp`、`RHI/CommandList.h`（契约注释）；复现与判据见 §10.2 任务 30 与 `Tools/gi/rsm_indirect_check.ps1` |
| **AB** | **高** → ✅ **已修复**（任务 26 顺带） | **Forward 的 IBL 从未被交给天空盒 ⇒ 烘焙出的辐照度/预滤波图近全黑，PBR 里的 IBL 漫反射与镜面恒为 0**。`ForwardPipeline` 走 RenderGraph 时，帧图直接按 `giIBL->IsDirty()` 注册烘焙 pass，而**全工程没有一处在 RG 路径上调用 `SetIBLSkybox`**（只有不走 RG 的 `PrepareGI` 里有）—— 于是烘焙的输入是"未设置的天空盒"。这与 §9.2-Q（IBL 从不烘焙、消费者照样采样）是同一类失效，只是发生在 Forward：**层栈、能力位、面板、日志全都正常，输出恒为 0**。**实测指纹**：`pipeline_mode=0` 下把漫反射层栈从 `{IBL}` 换成 `{IBL,RSM}`、甚至只放 `{RSM}`，HDR 读数**逐位相同**（0.1836214）；而把 UBO 的 `count` 直接画到颜色上又能看到 1 与 2 的差别 ⇒ 配置与 UBO 都是通的，是这两个源**本身的贡献**为 0。**修法**：RG 路径在注册烘焙 pass 之前先从 `SkyboxComponent` 调 `SetIBLSkybox`（与 Deferred / `PrepareGI` 同源），并在同一处补上 Forward RG 路径漏掉的 `m_RSM->SetLightBuffer(...)`（§9.2-AA ① 的同一个坑，Forward 有两条路径就漏了一条）。修后 Forward 的读数变成 `{IBL}` **0.1269305** / `{RSM}` **0.0865436** / `{IBL,RSM}` **0.1067366**（恰为前两者的加权平均）。**【任务 30 的更正】其中 `{RSM}` 那个数其实是"漫反射层栈为空"的读数**：Forward 的 RSM 从未产出（示例没驱动 Forward 的阴影系统 ⇒ `Shadow`/`RSM_Generate` 两个 pass 都不注册），三条判据在"源恒为 0"时全部成立 ⇒ 该检查看不出这件事。已记录为 §9.2-AD 与任务 34。**【任务 34 已修】**：示例现在照 `02.Cube` 驱动 Forward 的阴影系统（Forward 画面第一次有阴影），RSM 改用按场景包围盒拟合的固定光锥并把**同一个** VP 交给 PBR 的内联查表；`{RSM}` 现在是真读数 **0.0866324**（`S_rsm` = 8.875e-05，改前恰为 0），`forward_stack_check` 也把 `S_rsm(Forward)` 从报告项升级成了断言 | `Pipeline/ForwardPipeline_FrameGraph.cpp`；回归检查 `Tools/gi/forward_stack_check.ps1` |
| **AC** | 中 → ✅ **已修复**（任务 33） | **光追命中点的出射辐射度算错**（不变量 6 的最后一处残留）。`RT_HitCommon.slang` 的 `EvaluateHitRadiance` 被 RTGI、RT 反射、DDGI 探针三处共用，而它此前把两项直接相加就返回：`albedo·E_ambient + Σ(lightColor·intensity·N·L)` —— **两处量纲错**：① 直接光项**没有乘命中面 albedo**；② 整体**没有除以 π**（朗伯面出射辐射度是 `albedo/π·E`）。三处调用点里只有 `DDGI_Trace.rgen` 在调用后除了 π（任务 17 修探针过亮时加的补偿），RTGI 与 RT 反射直接当辐射度用 ⇒ 它们的绝对量级偏大（实测 RTGI 命中项在修正前后之比约 0.21）。**修法**：函数自己返回辐射度 `albedo/π·(E_ambient + E_direct)`，三个调用点一律不再做换算；并给该函数加**白炉分支**（全白环境 E=π ⇒ 返回 albedo），rgen 的 miss 分支在白炉下取 1 —— 命中与未命中两条路径都返回理想值，于是白炉读数**与场景几何无关、恒等于 1**，成为一条能看见绝对量级的判据。**实测**：`diffuse={RTGI}` 白炉读数 中心 **1.0000**（改前把白炉分支关掉负对照读到 **0.0000**）；SSGI 仍为 1.0000（任务 10 的判据未被破坏）。**连带影响（同一函数 → 同一修正）**：DDGI 探针的命中项也少了 albedo，故 DDGI 的绝对读数整体下移（`ddgi` 0.0873536 → **0.0775732**、`S_ddgi` 0.0308175 → **0.0206226**、`corr(SSGI,DDGI)` 0.6819 → **0.6457**），这在物理上是对的方向（深色命中面反射更少）；相关检查重跑全过 | `Shader/RT_HitCommon.slang`；`Shader/RT/RT_GI.rchit.slang`、`RT_Reflection.rchit.slang`（调用点）；`Shader/RT/DDGI_Trace.rgen.slang`（去掉重复的 /π）；`Shader/RT/RT_GI.rgen.slang`（白炉 miss）；`Shader/Lighting/DeferredLighting.frag.slang`（白炉下 RTGI 走真实路径）；`Render/RT/{RTEffectPass.h,RTGIPass.cpp,RTReflectionPass.cpp}`、`Render/GI/RTProvider.h`（furnace 位）；判据 `Tools/gi/rtgi_furnace_check.ps1` |
| **AD** | 中 → ✅ **已修复**（任务 34） | **Forward 的 RSM 源在 06.GILab 下没有生产者（连阴影也没有）**：`pipeline_mode=0` 时 Forward 管线的阴影系统**从不被驱动** —— `ShadowSystem` 要靠调用方先 `SetRenderResources` + `Update`（`02.Cube` / `03.Sponza-Forward` / `AISamples` 都这么做，**06.GILab 漏了**），于是 `HasActiveShadows()` 恒为 false ⇒ RG 里的 `Shadow` pass 与 `RSM_Generate` pass **都不注册**，`GetLightViewProj(0)` 行列式为 0，Forward 画面**没有阴影**。**实测指纹**：Forward 下 `diffuse={RSM}` 的 HDR 与**空漫反射栈**逐位相同（0.0865436），而 `forward_stack_check` 的三条判据（层栈改变画面、多源不变亮、双源等于加权平均）在"某个源恒为 0"时**全部成立** —— 一条**看不出源为 0** 的检查，任务 26 的"Forward 的 RSM 读数 0.0865436"因此是把"没有源"读成了"源很暗"。**第二层根因（补上驱动之后实测）**：即使补上 `Update`，`Shadow` 与 `RSM_Generate` 都注册了，三张 RSM 图仍只有清除值 —— 本 pass 用的是 **CSM 级联 0** 的 VP，而它由 `CSMTechnique::RenderCascade` **在 Shadow pass 执行时**才写进 `m_LightVPs`；帧图里这两个 pass 声明的是**互不相干的纹理**（阴影图 vs RSM 三张图），**没有依赖边** ⇒ 执行顺序不受保证。更深一层：CSM 的 VP 拟合**相机视锥**，用它渲染的 RSM 内容随视角变化（世界空间源的前提被破坏）。**第三层根因（补上固定视锥之后实测）**：RSM 三张图有 53% 覆盖了，`{RSM}` 的 HDR 仍与空栈逐位相同 —— RG 路径**从不调用 `UpdateRSMBindings()`**（只有非 RG 的 `PrepareGI` 调），PBR 采样的是 Initialize 时绑的 **bindless 占位纹理**。**第四层（顺着第三层查出来的另一类缺陷）**：给 `GIBlendParams` 加 `float4x4` + `float` 之后 `rsmValid` 恒读 0，根因是共享结构体里 `float _padBlend[3]` 在 C++ 占 12 字节、在 Slang 的 cbuffer（std140）里占 48 字节 ⇒ 数组之后的成员两端偏移全部错开（§9.2-AF）。**修法**：示例驱动阴影系统；`ForwardPipeline::RefreshRSMFrustum` 按场景包围盒拟合固定光锥（与 Deferred 同一份 `FitRSMFrustumToBounds`），同一个 VP 经 `GIBlendParams` 交给 PBR 的内联查表；每帧刷新时调用 `UpdateRSMBindings()`；`rsmValid` 由 C++ 统一判定。**判据**：`forward_stack_check` **10 条**全过 —— `S_rsm` 恰好 0 → **8.875e-05**；三张 RSM 图覆盖 **53.36% / 53.36% / 53.31%**（均值 0.2049）；相机沿 x 挪 300 后三张 RSM 图**逐字节相同**（视角无关，改前的相机视锥不可能满足）。**第 1 层只在 Forward 生效**：Deferred 侧本身一直由帧图驱动阴影 | `Samples/06.GILab/06.GILab.cpp`（驱动阴影系统）；`Pipeline/ForwardPipeline.cpp`（`RefreshRSMFrustum` + UBO + `UpdateRSMBindings`）；`Pipeline/ForwardPipeline_FrameGraph.cpp`（固定光锥）；`Lighting/PBR.frag.slang`（同一个 VP + `rsmValid`）；`ShaderTypes.slang`（`rsmLightViewProj` / `rsmValid`）；`Tools/gi/forward_stack_check.{ps1,py}`（断言 + 逐级判定 + 视角无关） |
| **AF** | 中 → ✅ **已修复**（任务 34 顺带） | **C++ 与 Slang 的共享结构体在「非 float4 数组」上布局不一致**：`ShaderTypes.slang` 是两端共用的单一定义，但**布局规则不同** —— Slang 的 cbuffer 按 std140，**数组元素步长固定 16 字节**，而 C++ 侧 `float[3]` 只占 12 字节 ⇒ 数组**之后**的所有成员偏移全部错开。实测（`slangc -reflection-json`）：`GIBlendParams` 里写 `float _padBlend[3]` 时 Slang 把 `rsmLightViewProj` 放在 **304**、`rsmValid` 放在 **368**、块大小 **432**，而 C++ 是 **256 / 320 / 336**；于是 PBR 读到的 `rsmValid` 恒为 0（落在 C++ 从未写入的区间），RSM 源静默不产出、且**没有任何报错**（`rsmVplScale` 在数组**之前**，所以它一直是对的 —— 这正是这个缺陷能潜伏两个任务的原因）。**修法**：填充一律用 `float4`（两端都是 16 字节、16 对齐），并在 `Pipeline/Material.h` 里把 `sizeof` 与三个字段的 `offsetof` **逐个钉死**（任何人再改这个结构都会在编译期被拦下）。**排查工具**：`slangc <shader> -reflection-json <out.json>` + `Tools/gi` 的说明（§11.3）。**同类残留检查**：`ShaderTypes.slang` 里其余数组都是 `float4x4[3]`（元素步长 64 = C++ 一致）或末尾纯填充（后面没有成员），不受影响 | `Shader/ShaderTypes.slang`；`Pipeline/Material.h`；§11.3 的排查方法 |
| **AE** | 中 → ✅ **已修复**（任务 35） | **SSR 的 Hi-Z 层次 march 会漏掉反射**（任务 32 的平面镜解析对照发现）：同一个镜面、同一帧、同一套场景尺度参数下，线性 march（`ssr_use_hiz=0`，本 pass 的正式回退路径）把红盒与绿盒的反射**都**放在解析预测像素上（偏差 0.44 px / 0.35 px），而**默认**的 Hi-Z 路径只找到红盒（0.44 px，8633 像素），**绿盒一个像素都没有**；Hi-Z 的镜面有效率反而**最高**（71%），所以"有效率 / 两条 march 同量级"这类判据看不见它。**根因（任务 35 查清）**：Hi-Z 在**屏幕空间**做 DDA（每步 2^level 像素），而代码把屏幕段的参数 `t` **直接当成射线参数**用（`rayPos = rayStart + R*(worldLen*t)` 再投影取深度）。透视投影把"世界线性"映射成"屏幕分数"的**射影**函数，两者只在射线两端深度相近时才近似一致 —— 实测（地面镜 + 解析真值）：在预测反射像素所在的屏幕分数处，这样算出的射线点比真实射线点远 **1251**（红盒）/ **1481**（绿盒）世界单位，是命中容差（11.6）的 **100 倍以上**。于是深度比较比的是射线上**另一个点**：红盒只是"蒙对"了附近的像素（最近命中 0.44 px 而整片图案是错的），绿盒一个像素都找不到。**修法**：用标准**透视校正**插值把屏幕参数换算成射线参数 —— clip 坐标沿射线线性（`cA = Proj·(rayStart,1)`、`cR = Proj·(R,0)`），`1/w` 在屏幕空间线性 ⇒ `w(t) = w0·wT / ((1−t)·wT + t·w0)`、`tau(t) = t·worldLen·w0 / ((1−t)·wT + t·w0)`，于是 `rayPos` 与 NDC 深度都能精确写出（顺带省掉一次投影）。**连带修正：步数预算**。修正后射线不再靠错误深度"蒙"到目标附近，需要更多步覆盖同一段屏幕距离（同一场景、同一套参数，参照线性 march 的 30676 / 13489 个物体色像素）：64 步 红 0 / 绿 0；128 步 17719 / 7971；**256 步 30137 / 15847**；600 步 32579 / 15902 ⇒ `autoScaleMarch` 把默认 `maxSteps` 提到 **256**（同时是线性回退的迭代上限：射程 64×11.59≈742 → 256×11.59≈2967 单位）。**判据**：`ssr_mirror_check` **13 条全过** —— Hi-Z 两条反射都落在预测像素（0.44 / 0.35 px）、远像素 **0.00%**、像素数 ≥ 线性参照的一半（30137/30676、15847/13489）、步数 256 ≤ 600 的 60%。**【测量更正】任务 25 的"Hi-Z 比线性快 3.2 倍"不成立**：那是**错误 march** + 米制参数下的读数；修正后实测 SSR pass **4.256 ms（Hi-Z 256 步）vs 4.505 ms（线性 600 步）= 0.94×**，步数比是 2.3× 而时间比只有 ~1.06× —— 每一次 Hi-Z 迭代要多付一次金字塔采样（并在 level 0 细化时付一次深度采样 + 逆投影），而地面镜场景会让层级长期停在 0（射线脚下的地面永远比射线近）⇒ 层次结构在这个几何下帮不上忙。回退手段（level-0 抖动、"只在穿越时降级"的层级策略）记在任务 35 的收尾项里 | `Shader/GI/SSR.frag.slang`（透视校正的射线参数）；`Pipeline/DeferredPipeline_FrameGraph.cpp`（步数预算 256）；`Tools/gi/ssr_mirror_check.{ps1,py}`（Hi-Z 断言 + 步数断言 + 耗时报告） |
| **AG** | 中 → ✅ **已修复**（第 8 个 GBuffer MRT 接入时顺带） | **MRT 帧缓冲的附件数写死 7 个颜色**：`VulkanCommandList::BeginOffscreenPassMRT` 里附件数组是 `VkImageView attachments[kMaxColorAttachments]`（8）且循环写成 `attachmentCount < 7`，而 render pass 是按 PSO 的 `colorAttachmentCount` 建的（`kMaxColorAttachments` = 8，render pass 侧本来用 `kMaxColorAttachments + 1` = 9 个附件）。于是给 GBuffer 加第 8 个 MRT（光照图键）时，`vkCreateFramebuffer` 收到 8 个附件而 render pass 期望 9 个 ⇒ 校验层报 `attachmentCount 8 does not match 9`，紧接着驱动在 `vkCmdBeginRenderPass` 里崩溃（`0xC0000005`，栈顶 nvoglv64）。**这解释了为什么"GBuffer 七个 MRT 已满"看起来是硬上限**：真正的上限是 8 个颜色，卡住的是这段写死的 7。**修法**：附件数组与清除值数组都按"颜色上限 + 1（深度）"开，循环用 `kMaxColorAttachments`；pass 内其余逻辑不变 | `RHI/Vulkan/VulkanCommandList_RenderPass.cpp`；复现与判据 `Tools/gi/lightmap_key_check.ps1` |
| ~~**Z**~~ | ✅ **已修复** | **面板上的「每源耗时」是假信息**：`GIDebugData::avgRenderTimeMs` 只有声明与显示两处，**全仓没有一处给它赋值**，因此 06.GILab 面板上「SSGI 耗时 / DDGI 耗时 / IBL 耗时 / SSR 耗时」**恒为 0.00 ms**。危害在于它长得像一个可用的性能读数：性能类任务（时间维分摊、pass 级剔除、march 换实现）都会自然地去读它，而它会一直回答 0 —— 与 §9.2-U（旧转储）、§9.2-Y（基线分两组）同属**测量可靠性**这一类。**修法**：每源每帧一对 GPU 时间戳 → 环形查询池 → 不阻塞地读回（`TryGetQueryResults`）→ 滚动平均写回源自己的 `GIDebugData`；`HE_GI_TIMING=1` 时每 120 帧打一行日志，便于脚本读取。**实测**：SSGI 16 采样 **0.436 ms**、64 采样 **1.271 ms**（×2.92，随工作量线性变化）；不启用 SSGI 的配置里它恒为 **0**；重复运行离散度 **0.0%**。过程中踩到三个坑（写进 §10.2）：`GetQueryResults` 带 `WAIT_BIT` 会把进程挂死；整池可用性判断因"从未写过的查询永远不可用"而恒假；pass 注册但内部直接返回（IBL 不在脏时）会留下**过期读数** | `GI/GITiming.{h,cpp}`；`RHI/CommandList.h` + `VulkanCommandList`（新增不阻塞读回）；`GI/GlobalIllumination.h`（`SetRenderTimeMs`）；`GI/IGIProvider.h`（`GetTimedPass`）；`DeferredPipeline{,_FrameGraph}`；回归检查 `Tools/gi/timing_check.ps1` |

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
> **不携带任何独立信息**。修复 §11.3.1 那两处缺陷后重测为 21.0×；**任务 10（SSGI-CAL）之后
> 再测为 2.2×**（§3.3）——注意本表第 1 列是**标定前**的 SSGI 输出，与 2.2× 不同轮，不能混用。
> 上表前四列只涉及 SSGI 自身，是同一环境下背靠背测出的相对变化，**不受影响**。

> **一个反直觉但重要的现象**：单独修 N 反而让输出**变小**。因为 N 转到 view 空间后，
> 看向墙壁时法线的 z 分量接近 0，而缺陷 M 恰使 `dot(N,sDir)` 等于 `N.z` 乘 `s.z`。
> **M 与 N 必须同时修**——这也说明「某个改动让指标变差」不等于该改动是错的，
> 需要先判断它与其它缺陷是否耦合。本次因此把 M 与 N 放进同一条提交。

回归：单元测试 159/159、3952/3952；白炉比值与绝对值均 **1.0000**；
Vulkan 校验 46 条与改前一致。

> 遗留（**已由任务 10 关闭**）：三处几何缺陷修完后，SSGI 仍明显弱于 DDGI（21.0 倍，修复
> §11.3.1 后重测），且累加项用的仍是命中点 **反照率**而非入射辐射度（§9.2-P）。**这两件事不是
> 同一类问题**：前者是几何/方向错误（本次已修），后者是量纲缺失，必须补 `L_in` 并做实测
> 标定才能解决——归 §10.1 `SSGI-CAL`。任务 10 已完成该标定：SSGI 现在是精确的 `E/π` 估计，
> 量级比 **2.2 倍**，与 DDGI 的相关性由 0.92 降到 **0.148**（§3.3、§9.2-P）。

> ⚠️ **校验层条数只在同一配置、同一去重条件、同一采样设置下纵向比较。** 各处出现的
> 「46 / 49 / 51 / 56 / 75 / 81 / 104 / 110 条」来自不同运行配置、不同去重设置与不同采样目标集
> （默认配置与白炉为 46；§11.3 采样设施的三个层栈变体在补上镜面/AO 落盘**之前**、关去重为
> 75/75/81、开去重为 49/49/51，**之后**为 104/104/110 与 56/56/56），绝对值**不可横向比较**；
> 有效的判据始终是「**同一份采样设施**下、与改前二进制的**逐类相同**」（§11.3.1 方法论第 6 条）。

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
| binding=16 | 512×512 RGBA16F | `kGPUBinding_RSMFlux` | 同上（任务 30 起这张图只存编码法线） |
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

验证：未写入告警 **4/4/3 → 0/0/0**；野指针报错约 **186 → 0**；Vulkan 校验 49/49/51 与改前
逐项相同（同一计数条件下；见 §11.3.1 方法论第 6 条）；单元测试 159/159、3952/3952；白炉比值与绝对值均 1.0000；同一二进制连续 6 次运行
均正常收尾。**DDGI 的差分贡献不变**（0.0195943 → 0.0195936），故 §3.3 与 P5 的结论不受影响。

**同时发现 U（已修）**：一度以为 `none` 层栈的**绝对**基线在两个只差「一处 lambda 捕获列表」的
构建之间变了 44%（0.0327534 与 0.0472113），并把它当成「绝对读数依赖二进制布局」。追查后
**结论被推翻**，过程本身值得记下来：

| 步骤 | 做法 | 结果 |
|---|---|---|
| 控制实验 | 只改一行注释、重编译（二进制布局必然改变） | 读数 0.0472112 → 0.0472113，**在噪声内不变** ⇒ 布局不是因素 |
| 逐一排除 | 阴影开/关、RSM 占位色、`rtShadowSource`、占位回落按通道位掩码扫描（一次编译多次运行） | 全部**无影响** |
| 回查数据来源 | 检查每次"暗读数"对应的运行日志 | `rep1/rep2/bis_none/ibl_none` **四次运行全部崩溃**（远早于第 60 帧）⇒ 那四次分析读到的都是**上一次的旧转储**，根本不是测量值 |
| 重测对照 | 在 HEAD 上重跑并确认 `crash=0 / dumped=1` | 0.04721089，与改动后一致 ⇒ 真正差异来自任务 23 的占位纹理存活期修复，与布局无关 |

所以缺陷被改写为 **§9.2-U：崩溃/超时的运行会留下旧转储而分析脚本无法分辨**，并已修掉：

- `Tools/gi/dump_gi.ps1` 在每个变体运行**前删除**该标签的全部产物
- 运行后校验 `gi_<tag>_hdr.f16` **存在且晚于本次启动**，否则报 FAILED、提示"本次数字不可用"
- 任一变体失败则脚本以**非零码**退出
- 失败路径已实测：用 `-Frame 100000 -TimeoutSec 20` 跑，三个变体全部报 FAILED 并以 exit 1 结束

教训（并进 §11.3.1）：**读数差异必须先证明"这个数字是本次跑出来的"**，再去解释它为什么变。

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

> **口径（本轮更新）**：**本表已没有未完成项**。原先未完成的任务这样处置 ——
> **11（统一降噪框架，含 11.1/11.2/11.3）与 19（PROVIDER-EXEC）、20（P6 + 绑定数组化）已迁到
> 《Lumen与Nanite完整设计规范》§5.1 / §5.2**（它们的验收对象只有 Lumen / P6）；
> **31（Lightmap 真落地）已删除**（§10.2 留有取消记录）。表里保留的已完成项是历史记录，
> 编号不重排 —— 后续新任务接着往下编号，**不要复活已迁出/已删除的编号**。

**A 组 · 先让验收可信**（第 1、2、24 项**均已完成** —— 验收设施已可信）

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| ~~**1**~~ | ~~**DDGI 绝对量级不可复现的根因**（§11.3.1）~~ —— ✅ **已完成** | 未知 / 中 | **根因是 IBL 图未被烘焙（未初始化显存被采样）+ DDGI 的 `useRSM` 闩锁**，已修（见 §9.2-Q/R 与 §11.3.1）。它原本会污染所有以 DDGI 为参照的验收——现已解除 |
| **2** | ~~**D1 · 偶发崩溃根因获证**~~ —— ✅ **已完成** | 未知 / 中 | 根因未证意味着已修项可能只是其中一个实例；长跑类验收会被它污染。**本轮结果**：先把**证据链**修好（原先 `HE_CRASH_TEST` 自检在主动崩溃后静默挂住、不产 dump、不进过滤器 —— 三个原因叠加：CRT stdio 锁、只装未处理过滤器不够（需 VEH）、报告不可诊断），随后抓到并修掉一条**可复现**的退出期崩溃（`HugEngineTests.exe`，161/161 单跑崩、整包不崩：`he::physics::s_World` 是进程级静态且含 `JPH::PhysicsSystem`，而 Jolt 的全局分配器只在 `PhysicsWorld::Initialize` 里惰性注册 ⇒ 从没初始化物理的运行在退出时调用**空的 `JPH::Free`**，`RIP = 0`；修法是把注册放进 `PhysicsWorld` 的基类 `JoltRuntimeGuard`）。历史那条偶发访问违例在 **76 次启动中零复现**，按测量结案，重开条件 = 任何一次真实崩溃（现在会自动产出 dump + 带行号的报告）。完整定位过程、证据链修法与回归检查见 §10.2 的「2」与 `Tools/gi/crash_handler_check.ps1` |
| ~~**22**~~ | ~~**§9.2-S · RHI 纹理有效性标记**~~ —— ✅ **已完成** | 小 / 中 | **本次 §9.2-Q/R 的"放大器"**：未初始化显存把一个"消费者门控写漏"放大成了**静默的物理错误 + 跨构建不可复现的读数**。已实现为**「已写入」登记 + 一次性告警**（不改写纹理内容），并在 06.GILab 上验证：能定位这类纹理，且不产生假阳性。**编号 22 出现在 A 组是刻意的**——见下方编号政策 |
| ~~**23**~~ | ~~**§9.2-T · 未产出的效果改绑占位纹理**~~ —— ✅ **已完成** | 小 / 中 | 22 完成后，检查跑出的 4 处「描述符指向从未写入的纹理」全部属「效果本次未产出」。已立单一真值（pass 注册结果）+ 门控通道回绑中性占位，把 22 的告警基线清零：**4/4/3 → 0/0/0**，并顺带把每次运行约 186 条「是野指针」归零。详见 §9.2-T 的实测 |
| ~~**24**~~ | ~~**§9.2-U · 绝对读数的二进制布局依赖**~~ —— ✅ **已完成**（结论被推翻，缺陷改写为测量方法缺陷） | 中 / 中 | 追查后确认**不存在**布局依赖：只改注释重编译读数不变；而四次"暗读数"全部取自**崩溃运行留下的旧转储**。缺陷已改写为「崩溃/超时的运行会留下上一次的转储，分析脚本无法分辨」，并修掉：运行前删除产物 + 运行后校验转储新鲜度 + 失败以非零码退出（失败路径已实测）。详见 §9.2-T 的实测小节末段 |

**B 组 · 修架构主张的破口与已知功能缺陷**（§9.2 的剩余各项）

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| ~~**3**~~ | ~~**§9.2-I · RTGI 用 DDGI 做 miss 回退**~~ —— ✅ **已完成** | 中 / 中 | **直接破坏架构核心主张**——「归一化 ⇒ 无双重计数」。已立「DDGI 是否是层栈源」为每帧状态：是则 RTGI 的 miss 不再回退 DDGI。实测同一份 RTGI 的均值为 0.0575 与 0.2034（3.5 倍）→ 修后 0.0575 对 0.0575（差 0.000%）。**顺带修掉挡住所有 RT 测量的 §9.2-V**（时域降噪不绑管线 → 启用 RTGI 稳定挂死） |
| ~~**4**~~ | ~~**§9.2-G · 层栈与子系统开关是两套真值**~~ —— ✅ **已完成** | 中 / 中 | **不变量 1**，且是**复发过**的一项。已把对齐放进框架：`IGIProvider::SyncToStack`（帧图构图前每帧调用）里让子系统的 `enabled` 与输出尺寸都跟着层栈走，并**删掉 `06.GILab.cpp` 里那份手工补丁**——「复发」的实体就是它。实测：`specular={SSR}` 时 pass 列表由「完全没有 SSR」变为出现 `SSR` 与 `SSR_Denoise` |
| ~~**5**~~ | ~~**§9.2-D · AO 乘到了直接光上**~~ —— ✅ **已完成** | 中 / 中 | 每帧生效的能量错误（AO 把**直接光**也压暗了），且 AO 不作用于镜面。修法同预期：直接光单独记下，AO 只乘间接项（漫反射 + 镜面），自发光不受遮蔽。实测：纯直接光下 AO 不再改变画面（0.1007917 → 0.2021757，与关 AO 完全一致），镜面 IBL 下 AO 开始生效（×0.558）；§3.3 的比值与相关性不受影响 |
| ~~**6**~~ | ~~**§9.2-F · RSM 的 pass 被嵌套在 DDGI 门控内**~~ —— ✅ **已完成** | 中 / 中 | 单独勾选 RSM 而关闭 DDGI 时 **RSM 永不注册**——配置说谎，属功能失效。修法：门控改为「两个消费方的并集」（`ShouldRunDDGI() \|\| ShouldRunRSM()`，后者与 Forward 侧同一个谓词），并新增 `GI_DDGI::ClearRSM` 让"本帧不注册"也成为明确结论。**实测**：`diffuse={RSM}` 且 DDGI 关时 pass 列表由**无** `RSM` 变为**有**；三例回归检查全过，「层栈无 RSM」的两例不注册（防止改过头）。三变体读数、每帧告警、白炉、单测与改前逐项一致 |
| **7** | ~~**§9.2-E · SSR 与 SSAO 仍用硬编码投影**~~ —— ✅ **已完成** | 中 / 中 | 同一缺陷的**两个剩余实例**（SSGI 已修，见 §9.2-M）；修法与先例都已具备。非默认相机（`PhysicalCamera` 由焦距反算 fov）下空间错位。**实测**：`cam_fov=100` 时 AO 输出纹理最大差 0.387、均值差 −0.79%；`cam_fov=60` 时与同配置重复运行对照量级相同（0.069 对 0.078）⇒ 标准路径不变。过程中顺带发现 **§9.2-W**（SSR 恒 miss），SSR 一半的数值验证因此要等第 25 项 |
| **8** | ~~**§9.2-H · Forward 无 Provider、无 `GIBlendParams` UBO**~~ —— ✅ **已完成**（取「把声明改对」） | 中 / 中 | 此前是"声称支持但实际不存在"：`Degrade` 会把 IBL/RSM 放进 Forward 的层栈，而 Forward 根本不读它。**实测判据**（单元测试）：`IsAvailable(IBL/RSM, Forward)` 为假、`Degrade(Ultra, Forward)` 后三个 GI 通道**全为空**且兜底不发生、属性测试（管线×设备×四档全组合）仍自洽；面板对无层栈 GI 源的管线显示说明并置灰控件。**Forward 的 IBL/RSM 渲染不受影响**（它们本来就不读层栈）。「真正走层栈归一化」拆为任务 26 |

**C 组 · 补齐「归一化」的前提**

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| ~~**9**~~ | ~~**§3.2 置信度体系**（屏幕空间源的逐像素可信度）~~ —— ✅ **已完成**（屏幕覆盖一项落地；探针网格归任务 14，其余两项已写明为何不做） | 中 / 中 | §3.2 此前只有硬编码在着色器里的「屏幕边缘 5% 降权」，UBO 里根本没有 confidence 字段，且 C++ 与着色器**各有一份源 id 列表**（GTAO 就被漏掉）。现在置信度掩码逐槽进 UBO、由 `ToConfidenceMask` 统一推导，着色器不再认识任何源 id，边缘带宽也从着色器常量变成 UBO 字段。**实测**（新增 `Tools/gi/confidence_check.ps1`）：默认 5% 带宽下屏幕最外圈 SSGI 的贡献只为中央的 **1.6%**（≈ 像素中心偏移的理论值 2%），中央仍正常贡献；带宽调到 50% 后同一环带（距边 5%~15%）的贡献降到 **32.4%**，与解析预测 `2c/(1+c) = 33%` 吻合。**改前二进制该检查必失败**（5%→50% 的比值为 **1.000**）。三变体读数、白炉、告警、各回归检查与改前逐项一致 |
| **10** | ~~**SSGI-CAL · 标度与量纲标定**（= §9.2-P）~~ —— ✅ **已完成** | 中 / 中 | P5 退场后的接棒项。**做了什么**：累加项补入射辐射度 `L_in`（**前帧 HDR**，由 Provider 声明消费者、帧图据此捕获 —— 此前捕获门控只写死 DDGI，只放 SSGI 时该纹理从未被写入，SSGI 恒 0）；余弦项改为 `dot(N, normalize(sDir))`；估计量改为 `Σ(L_in·cosθ)/Σcosθ`，由 `∫cosθ dω = π` 可知它**精确等于 `E/π`**，归一化常数是解析值 1，**不需要"以 PT 为参考标定一个增益"**；量纲不对的距离项删除。**白炉判据升级**：白炉下不再短路 SSGI（白炉条件恰是它的解析真值条件），白炉因此同时校验它的标度。**实测**：白炉单源 **1.0000**（改回旧「除以 N」形式则 **0.444**）；量级比 **21× → 2.2×**；输出占比 **4.5% → 31%**；与 DDGI 相关性 **0.9238 → 0.1480**（§3.3 的结论方向不变、理由被改写）。新增回归检查 `Tools/gi/ssgi_cal_check.ps1` 三例全过 |

**D 组 · 质量与性能**

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| **12** | ~~**AMORTIZE · 时间维分摊**（DDGI 每 N 帧更新 + 时域复用）~~ —— ✅ **已完成** | 中 / 低 | 把 256 探针的全量更新摊到多帧。实现：`GI_DDGI::updateStride`（每 N 帧更新一轮，相位逐帧轮转），未轮到的探针**把历史原样拷到当前缓冲**——两个探针缓冲是逐帧 ping-pong 的，不拷就会让跳过的探针每帧倒退一代。**判据用"等量工作 ⇒ 等量结果"**（探针场由时间混合驱动，真正决定状态的是**更新次数**而不是帧数）：`S(4,4k)` 对 `S(1,k)` 在三组 k 上分别为 **1.010 / 1.000 / 1.000**；同一帧下 `S(4,120)` 是 `S(1,120)` 的 **99%**（本场景的探针场约 30 次更新即收敛，因此 4 倍分摊的代价在这里几乎不可见）。默认仍取 1（既有行为不churn），数据与建议见 §10.2。新增回归检查 `Tools/gi/amortize_check.ps1` |
| **13** | ~~**CULL · pass 级空间剔除**（tile / scissor）~~ —— ✅ **已实测判定：不做**（理由见 §10.2） | 中 / 中 | `GPUCulling` **只服务 GBuffer 几何**，不服务 GI；GI pass 全是整幅执行（§3.5）。**先测量再决定**（任务 29 装上真实耗时读数之后才能这么做）：本场景全部 GI 项合计约 **0.7 ms/帧**，其中 SSGI 0.44 ms 占六成；而可用 tile 剔除的只有「没有几何的 tile」——由 albedo 判据得到空像素约 **7.2%**，且屏幕空间着色器**本来就对天空像素做了深度早退**，能省下的只是这部分像素的发射开销。剔除的**算术上限 ≈ 7% × 0.44 ms ≈ 0.03 ms**（占 GI 总预算约 4%），代价是"占用掩码是上一帧的"⇒ 边界处会缺一条 GI（§3.2 明确警告过不能用视角相关判据换取这点收益）。**改判条件**：几何只占屏幕一小部分（如俯视/远景）、或 GI 分辨率/采样数大幅提高、或将来出现单个 ≥2 ms 的 GI pass |
| **14** | ~~**§9.2-K · DDGI 网格外退化为贴边常数外推**~~ —— ✅ **已完成** | 低 / 低 | 探针网格为固定参数，覆盖不到的区域**静默缺失**低频 GI 且无 falloff / 无效标记。两半一起做：覆盖置信度位 + 网格按场景包围盒自动拟合。**判据**：固定网格下贡献 0.019595 → 噪声量级（2.9e-08 / −3.9e-07）；拟合后回到 **0.019596**、与改前逐位一致；拟合规则由单元测试覆盖（166 例 / 4189 断言全过）；新增回归检查 `Tools/gi/ddgi_grid_check.ps1` 4 配置 5 判定全过。**顺带量化了任务 17 的提升空间**（任务 17 已据此完成，见 §10.2：探针场从"逐位相同"变成"三种拟合分辨率相差 37.93%"） |
| **15** | ~~**§9.2-J · 合成参数 UBO 只一份**（非 per-frame-in-flight）~~ —— ✅ **已完成** | 低 / 低 | `MAX_FRAMES_IN_FLIGHT=3` 但合成参数 UBO 是单份，本帧写入会覆盖仍在飞行的上一帧所读的参数，目前靠"值变化小"掩盖。**修法不止于那一份 UBO**：Lighting 的一批逐帧轮换资源（光源/阴影/探针 SSBO、合成参数 UBO）全都绑在**同一份**描述符集上，逐帧重绑同样跨帧 ⇒ 改为**每飞行帧一份描述符集 + 一份 UBO**，`LightingInputs::frameSlot` 由帧图给出，渲染时只更新并绑定本槽位那份（占位纹理的默认绑定写进全部三份）。**判据**：三/四变体读数逐位不变（`none` 0.0577335 / `ddgi` 0.0766960 / `ssgi` 0.0662763，DDGI 差分 0.0195947）、校验条数不变（56/56/56/56）、未写入告警 0、白炉 1.0000、单元测试 161/161 与 3941/3941 全过。**同时证伪了一个假设**：这**不是** §9.2-Y 那个"同配置两次运行分成两组"的原因 —— 改前/改后两个二进制各跑 4 次，两组（0.057194 / 0.057733）在两边都照样出现，组内离散小于 0.01%、组间约 1%。改动落在 `Pipeline/LightingPass.{h,cpp}` 与 `Pipeline/DeferredPipeline_FrameGraph.cpp` |
| **16** | ~~**B3 · RSM VPL halfRes**~~ —— ✅ **已完成** | 小 / 低 | 把 16 点 Poisson VPL 求和从 Lighting 的逐**全分辨率**像素求值搬进独立的**半分辨率** pass。**判据**：`RSM_Indirect` pass 出现在 pass 计时里且非零（0.109～0.120 ms）；Lighting 在漫反射层栈含 RSM 时对不含时只涨 **+21%**（改前 **+88%**，即 0.433→0.882 ms）；单源做差 `S_rsm` 的**形状保真度**用放大信号的 A/B 量出（×1e6 后全分辨率对半分辨率：均值差 **0.60%**、逐像素相关 **0.781**、对比度降到 65%）。**顺带**：修掉 RSM 光源 VP 与查找 VP 不同源（改前 Lighting 用的是 CSM 第 0 级 VP）、修掉通量读错缓冲、把 `CollectLights` 提前到帧图开头。**并发现 §9.2-AA**（RSM 间接光整项恒为 0，成本照付），拆为任务 30。回归检查 `Tools/gi/rsm_indirect_check.ps1`（3 条判定全过）；三变体读数、白炉、单测、其余检查全部不变 |
| **17** | ~~**B4 / M5.2-A · DDGI 光追 march**~~ —— ✅ **已完成** | 中 / 中 | 只提升单一源质量。第 1 项已解决（§11.3.1），DDGI 的读数现已确定性可复现，可以安全改动。**任务 14 把这个源的"提升空间"量化了**：当前默认配置（RSM 不在漫反射层栈 ⇒ `u_Flags.x=0`）下，`DDGI.comp.slang` 的 IBL 回退路径只按 `dir` 采样 `u_IBLIrradiance`、**完全不用 `samplePos`**，而 Fibonacci 方向对每个探针都一样 ⇒ **整片探针场的 SH 逐位相同**（格距 531/248/120 三种拟合贡献相差 0.0015%）。**修法**：新增 `DDGITracePass` + `DDGI_Trace.rgen.slang`，每条探针射线用硬件光追求真实辐射度（命中复用 `RT_GI.rchit`，未命中取 IBL），`u_Flags.w` 按 `supportsRayTracing` 自动选择；帧图顺序 `AS_Build → DDGI_Trace → DDGI`。**判据**：`ddgi_grid_check` 第三条判定反转 —— 三种拟合分辨率的贡献必须显著不同，实测 **37.93%**（改前 0.0015%）；`S_ddgi` 0.0195946 → **0.0308178**；`p5_spectrum` 量级比 2.2×→**3.4×**、相关性 0.1480→**0.6818**，但"不存在使 SSGI 更像 DDGI 的低通尺度"这条结论不变；`ssgi_cal_check` 3/3、其余检查与单测全过。详见 §10.2 |
| **18** | ~~**Lightmap 源落地**~~ —— ✅ **已完成**（结论：**本轮不落地**，把声明改对并记下前置条件） | 中 / 低 | 按需（PC 实时路线可缓）。`ToPipelineCap()` 原先**没有该分支**（靠 `default` 兜成"不可用"），而文档写着「预留，可用」⇒ 配置说谎。**本轮**：把它改成**显式的"不给能力位"**并写明理由（真落地需要逐像素光照图键，而 GBuffer 七个 MRT 已满；世界坐标查表的替代方案与 DDGI 是同一估计量、会被判冗余），文档同步改成"明确不做"，新增单测锁住「即使塞进层栈也会被 `Degrade` 裁掉」。**判据**：单测 167 例 / 4194 断言全过；全文不再有"Lightmap 可用"的说法。**改判条件**：出现"静态几何需要远高于 DDGI 分辨率的烘焙细节"的真实需求时**重新开一项**（承接它的任务 31 已取消，取消时留下的基础设施与教训见 §10.2 的「31 · Lightmap 真落地」记录） |

**E 组 · 结构性投资（等消费方）—— ➡️ 已整体迁出**

> 本组原有的两项（19 PROVIDER-EXEC、20 P6 + 绑定数组化）**已迁到《Lumen与Nanite完整设计规范》
> §5.2**（验收对象只有 P6 / Lumen），故这里不再保留表。设计背景仍在本文档 **§4.3.5**
> （三层改造）与 **§4.3.2**（AO 旁路实例）。

**F 组 · 文档**

| # | 任务 | 规模/风险 | 理由 / 依赖 |
|:---:|---|---|---|
| **21** | ~~**文档一致性修正**~~ —— ✅ **已完成** | 小 / 低 | 逐条核过并改掉：① §3.1 的合成片段与着色器不符（文档写的 `num/max(den,1e-4)`，实际是 `(den>0)?num/den:0` —— 通道里没有源时前者会放大一万倍）；② §11.3 的采样目标清单缺 `radiance`/`ibl_irr`，补成与 `addTarget` 逐项一致；③ §6 不变量 2（"距离让位是性能控制"→ 更正为纯艺术控制）与不变量 6（"量纲未统一"已过期 → 已满足，并注明 `EvaluateHitRadiance` 的 π 残留见任务 30）；④ §9.2 标题漏 M/N/O/P → 改为按行标注的口径；⑤ §11.4 两条随 P5 退场的风险（`LowPass` 选型、P5 抽象过度）标为已失效；⑥ §11.3 的运行示例自相矛盾（写"运行目录必须是 `Build\bin\Debug`"，而所有工具都以仓库根目录启动 Release 版且正常工作——Content 路径是编译期绝对路径）→ 改成实际用法；⑦ 顺带把"逐 pass 耗时不可当判据"补成 §11.3.1 方法论第 7 条 |
| **25** | ~~**§9.2-W · SSR 的 Hi-Z march 恒 miss**~~ —— ✅ **已完成**（解析对照已由任务 32 补齐） | 中 / 中 | SSR 此前**一个命中都没有**（全屏 alpha=−1、RGB=0），镜面层栈里有没有它完全等价。**三处独立叠加的错**都修了：Hi-Z 深度判据方向反了（min 金字塔 + zero-to-one ⇒ 应当"射线更近才前进"）、层级步长方向反了（`1/2^level` 让 level 0 一步跨完整条射线 ⇒ 改成按屏幕像素折算的 DDA）、level 0 命中阈值量纲不对（NDC 差比世界厚度 ⇒ 改成与线性 march 同一个 view 空间判据）；线性回退路径的假命中（`rayPos.z > rpZ` 记成命中）也一并改掉。**判据**：`Tools/gi/ssr_check.ps1` 三条全过 —— Hi-Z 有效像素 **0% → 13.03%**、Hi-Z 命中最少是线性的 0.5 倍（实测 **1.28 倍**）、把 SSR 放进 `{IBL}` 镜面栈后 HDR 必须变化（**0.0577333 → 0.0596849，+3.38%**）。**顺带量出 Hi-Z 真的在加速**：SSR pass **0.254 ms**（层次）对 **0.806 ms**（线性），3.2 倍。**未做**：平面镜解析对照（场景里没有平面镜）→ 任务 32 |
| **26** | ~~**让 Forward 真正走「层栈 + 归一化合成」**（§9.2-H 的另一半）~~ —— ✅ **已完成** | 中 / 中 | 任务 8 把**声明**改对了（Forward 不再声明它消费不了的源），这一项补上另一半：`PBR.frag` 新增 `GIBlendParams` UBO（复用 Deferred 的绑定号 31 与同一结构）+ 逐通道遍历层栈做 `Σ(贡献×权重)/Σ权重`；`ForwardPipeline` 每飞行帧一份 UBO、每帧在 `Render` 开头填一次；`PipelineCaps::Forward` 加回**前向真能消费**的三个源位（漫反射 IBL/RSM、镜面 IBL）。**判据**：`Tools/gi/forward_stack_check.ps1` 三条全过 —— 改漫反射层栈会改画面（改前 cfg 键只写给 Deferred，`pipeline_mode=0` 完全忽略它们）、多开一个源**不变亮**、双源读数**恰等于**两单源的加权平均（`both 0.1067366` 对 `(0.1269305+0.0865436)/2 = 0.1067371`，相对误差 **0.000%**）。**顺带修掉 Forward 侧一个 §9.2-Q 同族的缺陷**（IBL 从未被交给天空盒 ⇒ 恒为 0，见 §9.2-AB）。单测 168 例 / 4242 断言全过。**〔任务 34 更正〕上表里 `{RSM}` 的 0.0865436 是"空层栈"读数**（Forward 的 RSM 当时没有生产者，见 §9.2-AD）；任务 34 之后 `{RSM}` = **0.0866324**、`{IBL,RSM}` = **0.1067809**，`S_rsm` = **8.875e-05** |
| **27** | ~~**§9.2-X · 镜面层栈为空时出现 463 量级亮点**~~ —— ✅ **已完成** | 中 / 低 | 复现与读数：三通道全空 ⇒ `mean 0.2022 / max 463.32`（位置固定在 (416,996)）；`specular={IBL}` ⇒ `0.0694 / 42.20`；只放 `ao={SSAO}`（specular 仍空）⇒ 回到 `0.2022 / 463.32`。**根因不在镜面通道的合成里**（那里 `specNum/max(specDen,1e-4)` 在空栈时给 0），而在 **IBL 烘焙的门控漏了第四个消费者**：`u_BRDF_LUT` 是 PBR 在**直接光路径**上无条件采样的（与任何层栈无关），而烘焙 pass 只在「漫反射栈要它 ∨ 镜面栈要它 ∨ DDGI 要它」时注册 ⇒ 镜面栈为空且 IBL 不在别处时**那张 LUT 从未被写入**，直接光的 BRDF 读到未初始化显存。**判据**：`Tools/gi/ibl_lut_gate_check.ps1`（三种配置 max 都必须落在 42 量级 + 空镜面栈下 `IBL_Bake` pass 必须仍在注册列表里）全过；修后三种配置的 `max` 都是 **42.20**，空栈均值 **0.2022 → 0.0429**（也就是说这不是"一个亮点"而是**整幅画面的直接光都算错了** 4.7 倍）。**历史读数注意**：凡以"空镜面栈"为基线的绝对值都受过这一项污染（差分对照不受影响，两侧同样被污染） |
| **28** | ~~**§9.2-Y · "空层栈"这一配置状态不可靠**~~ —— ✅ **已完成** | 中 / 中 | 根因是**两个写者 + 一处静默补源**：配置加载路径直接按 cfg 重建层栈（全 0 权重 ⇒ 空栈），而任何经过 `GIRegistry::Degrade` 的路径（预设按钮、阴影下拉）会被它的**兜底**补成 `{IBL}`；示例退出时又把**内存里那份**回写成配置文件 ⇒ 下一次运行读到被补过的配置，读数 0.05773 与 0.07554 两组、相差 32%。**修法**：`Degrade` 改成**只裁不加**（"空通道"是有定义的合法状态，见 §3.1，也是做差实验的基线），于是同一份输入在任何调用路径上同义；单测新增"只裁不加 + 幂等"三条断言。**判据**（`Tools/gi/repeatability_check.ps1`，5 次同配置）：读数离散 **0.0003%**（0.0577333～0.0577335）、cfg 里的层栈键**逐键不变**、且落在"空栈"量级而非"被补 IBL"量级 —— 三条全过。**同时把另一条历史观察降级**：文档里"同一二进制同一 cfg 连跑四次分成 0.057733 与 0.05719x 两组（差 1%）"在今天的 5 次运行里**没有复现**（离散 0.0003%），最可能的解释是当时尚未装上 §9.2-U 的陈旧转储护栏，混入了上一次运行的产物 |
| **29** | ~~**§9.2-Z · 给 GI 源装真实耗时读数**（面板现在恒显示 0.00 ms）~~ —— ✅ **已完成** | 中 / 低 | 每源每帧一对 GPU 时间戳 → 环形查询池 → **不阻塞**读回 → 滚动平均写回源自己的 `GIDebugData`；`HE_GI_TIMING=1` 时每 120 帧打一行日志供脚本读取。**判据（读数是真的）**：SSGI 16 采样 **0.436 ms** → 64 采样 **1.271 ms**（×2.92，随工作量变化）；不启用 SSGI 的配置里恒为 0；重复运行离散度 **0.0%**。**顺带第一次拿到真实成本结构**：SSGI 0.44 ms、DDGI 探针更新 **0.019 ms**、IBL 首次烘焙约 5 ms（之后不再跑）—— 这也解释了为什么任务 12 的分摊收益在这个网格尺寸下量不出来 |
| **30** | ~~**§9.2-AA · RSM 链路的量级与通道约定**（任务 16 发现）~~ —— ✅ **已完成** | 中 / 中 | 修前 RSM 间接光**整项不产出**（`S_rsm` 逐像素为 0），成本照付 0.19 ms/帧。任务 16 只看到"量级太小"，本次逐级落盘发现**三张 RSM 附件里只有清除值**（`BeginOffscreenPassMRT` 的清除值长度契约被越界读破坏 ⇒ 深度清成垃圾 ⇒ 片元全被丢弃），以及 **VPL 的 albedo 读的是相机可见性列表的索引空间**（只有 3% 的 texel 拿到非零 albedo）。四项一起过：① 受光项的量级归一改成**解析面积**（`GI/RSMFrustum.h`：`scale = (radiusUV·2·halfExtent)²/N`，旧经验常数隐含"半径 60 的场景"，比值 3942）；② RSM 改成**三个附件一个量**（位置 / 编码法线 / VPL 辐射度），`DDGI` 不再把编码法线当辐射度读；③ 辐射度带上 albedo 与光源颜色（不再只能是灰度）；④ 光源视锥按**场景包围盒**拟合（去掉硬编码 `sceneCenter/Radius`）。**判据**：三个中间层从 0% 覆盖变成 53% 且均值 0.2 量级、`S_rsm` 0 → 4.28e-5（与 `E/π×albedo` 相关 0.966）、`rsm_indirect_check` 7 条判定全过、半分辨率保真度按真实量级重测（均值差 0.25%、相关 0.941、对比度 1.11）。**量级仍只占屏幕均值 0.06%**，这是 2.5D RSM 估计量的性质（共面 VPL 的两个余弦同时趋零），判据守结构不守绝对量级。完整实测见 §10.2 任务 30 |
| **32** | ~~**SSR 的解析对照（平面镜）**（任务 25 留下的空缺）~~ —— ✅ **已完成** | 中 / 低 | 任务 25 只证明了"射线有效性恢复、两条 march 路径同量级、Hi-Z 更快"，**没有**证明反射的**位置/方向**正确 —— 平面镜的闭式答案（物体中心按镜面镜像后再经**同一个**相机投影）一上来就逮到**四处**"有效率"类判据看不见的错：① `reflect(-V, N)` 把 **view 空间**的入射方向与**世界空间**的 GBuffer 法线混用（反射只在相机与世界轴对齐时才对）；② 深度→view 重建与 view→屏幕投影**都**漏了本引擎的 y 约定（负高度视口 ⇒ `ndc.y = 1−2v`），两处漏项让**位置**自洽（shader 重建的 `viewPos.y = +70.3`，真值 **−70.7**）却把几何上下镜像 ⇒ 反射方向错；③ 起点自交偏移写死 `0.1`，而命中容差按场景尺度取到 11.6 ⇒ 每条射线**立刻命中自己**（地面镜上 62% 的像素"有效"，输出的却是镜面自己的 albedo）；④ 线性 march 把"射线仍在几何之前"记成命中 0.5（§9.2-W 的历史有效率就是这么抬起来的）。**另按场景尺度重取 march 参数**：`diag`=**4636.97** ⇒ `maxDistance=diag`、`thickness=diag×0.0025`（≈11.6）、`stepSize ≤ thickness`；历史默认（50/0.1/0.5/64）在 3720 单位宽的 Sponza 上只找到 **2432** 个反射像素（场景尺度 **30676**，差 12.6×）。**判据**（`Tools/gi/ssr_mirror_check.ps1`，7 条全过；**先在直视图上自检相机模型** —— 盒心投影到最近自身像素 0.49 / 0.18 px）：反射落点偏差 **0.44 px（红）/ 0.35 px（绿）**、远离预测的杂色 **0.00%**、相机平移 40 后反射位移与解析值差 **2.39 px**、米制参数负对照 **30676 vs 2432**。**Hi-Z 路径仍漏掉绿盒**（§9.2-AE / 任务 35），故解析断言钉在**线性回退路径**上。完整表、负对照与测量卫生见 §10.2 任务 32 |
| **33** | ~~**§9.2-AC · 光追命中点的量纲**（任务 30 期间顺带确认）~~ —— ✅ **已完成** | 小 / 中 | `EvaluateHitRadiance`（RTGI / RT 反射 / DDGI 探针三处共用）把 `albedo·E_ambient` 与**未乘 albedo** 的 `Σ lightColor·intensity·N·L` 相加后直接当辐射度返回 —— 两处量纲错（缺 albedo、缺 1/π），而三处调用点只有 DDGI 探针自己除了 π。**修法**：函数返回辐射度 `albedo/π·(E_ambient + E_direct)`，调用点不再换算；并加**白炉分支**（命中与未命中都返回理想值 ⇒ 白炉读数与几何无关、恒为 1），把一条**能看见绝对量级**的判据做出来（此前的短路与相对判据都看不见这个错）。**实测**：白炉 `diffuse={RTGI}` 中心 **1.0000**（负对照 0.0000）、SSGI 仍 1.0000；RTGI 命中项修正前后之比 ≈0.21、RT 反射原始输出 0.54（天空占比较大）；连带 DDGI 绝对读数下移（`S_ddgi` 0.0308175 → 0.0206226，方向正确：深色命中面反射更少），相关检查全部重跑通过。完整判据、负对照与当前基线表见 §10.2 任务 33 |
| **34** | ~~**§9.2-AD · 让 Forward 的 RSM 真有生产者**~~ —— ✅ **已完成** | 中 / 中 | 实际有**四层**根因，全部修掉：① 06.GILab 的 Forward 模式不驱动阴影系统（照 `02.Cube` 补 `SetRenderResources` + `Update`；此前 Forward 画面**连阴影都没有**）；② Forward 的 RSM 用 **CSM 级联 0** 的 VP（拟合相机视锥 ∧ 由 Shadow pass 执行时才写入 ∧ 帧图里与 RSM_Generate 无依赖边）⇒ 改成与 Deferred 同一份按场景包围盒拟合的**固定光锥**（`RefreshRSMFrustum`），并把**同一个** VP 经 `GIBlendParams` 交给 PBR 的内联查表（写入 UV 与查找 UV 同源）；③ RG 路径**从不调用 `UpdateRSMBindings()`**（只有非 RG 的 `PrepareGI` 调）⇒ PBR 采样的是 Initialize 时绑的 **bindless 占位纹理**，补齐到每帧刷新处；④ 顺着③查出的 **§9.2-AF**（共享结构体里 `float[3]` 在 Slang cbuffer 里步长是 16 ⇒ 数组之后的字段两端偏移错开，`rsmValid` 恒读 0）。**判据**（`Tools/gi/forward_stack_check.ps1`，**10 条**全过）：`S_rsm` = mean({RSM}) − mean(空栈) **恰好 0 → 8.875e-05**（占屏幕均值 0.102%）；三张 RSM 图覆盖 **53.36% / 53.36% / 53.31%**（VPL 辐射度均值 0.2049，与 Deferred 侧同阈值）；相机沿 x 挪 300 后三张 RSM 图**逐字节相同**（视角无关 —— 改前的相机视锥不可能满足）；原有三条（层栈改变画面 / 多源不变亮 / 双源等于加权平均，相对误差 0.000%）仍全过，`{RSM}` 读数 0.0865436 → **0.0866324**。回归：`rsm_indirect_check` 8/8（Deferred 侧 `S_rsm` 4.284e-05 不变）、单测 **172 例 / 5233 断言**全过。完整记录见 §10.2 任务 34 |
| **35** | ~~**§9.2-AE · SSR 的 Hi-Z 路径漏反射**（任务 32 发现）~~ —— ✅ **已完成** | 中 / 低 | 根因是 Hi-Z 的屏幕空间 DDA 把**屏幕段参数**当成了**射线参数**（`rayPos = rayStart + R*(worldLen*t)`），而透视投影下两者是射影关系：实测在目标像素所在的屏幕分数处，这样算出的射线点比真实射线点远 **1251 / 1481** 世界单位（容差 11.6 的 100 倍以上）⇒ 深度比较比的是射线上另一个点，绿盒整片丢掉、红盒只是"蒙对"。修法：用透视校正的 `w(t)` / `tau(t)`（`1/w` 在屏幕空间线性）精确换算出射线点与 NDC 深度。修完还要提步数预算：同一场景下 64 步两个盒子都找不到、128 步红 17719 / 绿 7971、**256 步 30137 / 15847**、600 步 32579 / 15902（参照线性 march 的 30676 / 13489）⇒ `autoScaleMarch` 把默认 `maxSteps` 提到 256。**判据**（`ssr_mirror_check`，**13 条全过**）：两条路径都必须把红/绿反射放在解析预测像素 6 px 内、远像素 ≤5%（实测 0.44 / 0.35 px、0.00%）、Hi-Z 的像素数 ≥ 线性参照的一半、以及**步数断言** 256 ≤ 600 的 60%。**【测量更正】任务 25 的"Hi-Z 快 3.2 倍"改为不成立**：那是错误 march + 米制参数下的读数，修正后 SSR pass 4.256 ms（Hi-Z 256 步）对 4.505 ms（线性 600 步）⇒ 时间比仅 ~1.06×（步数比 2.3×）—— 每次 Hi-Z 迭代多付一次金字塔采样与 level-0 细化，而地面镜会让层级长期停在 0，层次结构在这个几何下帮不上忙；level-0 抖动/层级策略优化作为**重开条件**记在 §10.2 任务 35 |

> **本次重排说明**：
> 1. **引入 A 组**：把两个"调查项"提到最前，理由是**它们决定其余任务的验收是否可信**——
>    第 1 项直接决定第 10 项的验收标准能否成立，第 2 项决定长跑类验收是否被污染。
> 2. **B 组把 §9.2 的剩余缺陷全部任务化**：此前 §9.2 列了 8 项未修缺陷，但 §10 的任务表里
>    **一项都没有**——缺陷与计划脱节。现按"越靠近架构主张越靠前"排列：
>    合成无偏性（I）→ 复发过的不变量（G）→ 每帧能量错误（D）→ 功能失效（F）→ 定位错位（E）
>    → 声明与实现不符（H）。
> 3. **C 组**把「置信度体系」立为任务（**统一降噪**部分已迁到 Lumen 文档 §5.1，见该文档）：
>    前者是归一化在屏幕空间源上的
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
  （Forward 侧本就是独立的 `ShouldRunRSM()`）。**已完成**：门控取两个消费方的并集，并补
  `GI_DDGI::ClearRSM`（"不注册"也要显式写回，否则闩锁留着上一帧的绑定）；回归检查
  `Tools/gi/rsm_gate_check.ps1` 三例，其中 A 例在改前必失败。
- **7（E · SSR/SSAO 硬编码投影）**：照 SSGI 已修的路径（`SetCamera` + UBO 补 `u_View`）。
  判据：改用非默认 `PhysicalCamera`（由焦距反算 fov）后空间重建不再错位。**已完成**：
  两个 pass 都改为从 `CameraData::GetProjMatrix()` 取投影（无相机时才退化为原来的默认投影），
  Provider 不再忽略 `ctx.camera`。实测见 §9.2-E 行与 §10.1 第 7 项。

  > 顺带发现的 **§9.2-W（SSR 恒 miss）** 曾让 SSR 这一半无法用"合成结果变化"来验证 ——
  > 它自己的输出本来就是空的。SSR 的改动与 SSAO 完全同源（同一行矩阵来源），
  > SSAO 一侧已用逐像素对照验证。**任务 25 修好 march、任务 32 补上平面镜解析对照之后**，
  > SSR 侧的位置/方向已被逐像素验证（预测像素偏差 0.44 / 0.35 px，见 §10.2 任务 32），
  > 且顺带查出"世界/view 空间法线混用"与"y 约定漏翻"这两处矩阵来源问题。
- **8（H · Forward）**：二选一——补齐 Forward 的 Provider 与 `GIBlendParams` UBO，
  或把 `PipelineCaps::Forward` 的声明改成实际支持的范围。**不能保持"声称支持但不存在"。**
  **已选后者**：`PipelineCaps::Forward` 现在只有光栅阴影，GI 源位为空。理由是这条路
  零渲染风险、可被单元测试完全覆盖，而前者要动 `PBR.frag`——它被 4 个示例与编辑器共用，
  属于必须单独安排回归面的改造，已拆为任务 26。**Forward 的 IBL/RSM 渲染不受影响**
  （它们本来就由 `iblIntensity` / `rsmIndirect` 驱动、不读层栈）。
  同时补了 UI 侧的同源问题：面板的候选源列表此前只看「有没有 Provider」，现在也按能力位过滤，
  并在本管线没有层栈 GI 源时显示说明并置灰控件——否则用户会对着一个改了也不生效的开关操作。
  （**任务 26 已完成那一半**：Forward 现在真的按层栈归一化合成，声明也随之加回了
  IBL/RSM 三个源位；见下方「26」那一节。）

**26 · 让 Forward 真正走「层栈 + 归一化合成」** —— ✅ **已完成**

- **改前的状态**（任务 8 之后）：`PipelineCaps::Forward` 只有光栅阴影，声明与实现一致、没有
  配置说谎；但 `pipeline_mode=0` 下 **cfg 里的 GI 键完全不起作用** —— 采样脚本里那一段
  "层栈恢复"只写 `deferredPipeline.GetGIConfig()`，Forward 那份配置始终是初始化时的默认值。
- **做了什么**
  1. `PBR.frag`：新增 `GIBlendParams` UBO（**复用 Deferred 的绑定号 31 与同一结构**，
     C++ 侧仍用 `GIChannelBlendData` 构造后 memcpy，`static_assert` 保证布局一致），
     把原先硬编码相加的 IBL 漫反射 / IBL 镜面 / RSM 三项改成**逐通道遍历层栈的源数组**：
     `Σ(逐源贡献 × 权重) / Σ权重`（含"通道里没有源时返回 0"这条与 Deferred 相同的语义）。
     逐源表达式一字未动 ⇒ 单源时归一化精确抵消。
  2. `ForwardPipeline`：每飞行帧一份 UBO（与 `m_LightBuffers` 同样三份），`FillGIBlendUBO()`
     每帧在 `Render` 开头填一次（RG 与非 RG 两条路径都要用）。
  3. `PipelineCaps::Forward` 加回**前向真能消费**的三个源位：漫反射 IBL/RSM、镜面 IBL。
     仍然不声明屏幕空间源、探针与光追源 —— 前向着色没有 GBuffer，声明了就是又一次
     "把源放进没人消费的层栈"（任务 8 的教训）。
  4. 示例：那段层栈恢复抽成 lambda，对两个管线各套一次；Forward 从**它自己的预设基线**出发
     套同一份键，再按 Forward 的能力位 `Degrade`（不降级就会带着跑不了的源进层栈）。
  5. 采样设施：`HE_DUMP_GI` 现在按**当前管线**取 HDR 目标 —— 此前它无条件取
     `deferredPipeline` 的 HDR，`pipeline_mode=0` 下落盘的**根本不是 Forward 的画面**，
     也就是说 Forward 侧的读数一直不可测（§11.3 早就把这写成"注意事项"，本轮修掉）。
- **判据**（`Tools/gi/forward_stack_check.ps1`，三条全过；Frame 120 / Sponza）

  | Forward 漫反射层栈 | HDR 亮度均值 |
  |---|---|
  | `{IBL}` | **0.1269305** |
  | `{RSM}` | **0.0865436** |
  | `{IBL,RSM}` | **0.1067366** |

  - **层栈真的起作用**（改前三种配置逐位相同）；
  - **多开一个源不变亮**：`{IBL,RSM}` 0.1067 < `{IBL}` 0.1269（改前是硬编码相加，只会更亮）；
  - **双源读数恰等于两单源的加权平均**：`(0.1269305 + 0.0865436)/2 = 0.1067371`，
    实测 0.1067366，相对误差 **0.000%** —— 与 Deferred 侧对 SSGI 用的那条判据（SSGI-CAL
    的"双源差分 = 加权平均"）完全同构，只是这次是在 Forward。
  - **【任务 30 的重要更正】上表里 `{RSM}` 的 0.0865436 不是"RSM 的读数"，而是"漫反射层栈
    为空"的读数** —— 实测：把 Forward 的空漫反射栈跑一遍得到**同一个** 0.0865436（逐位相同），
    即 Forward 的 RSM 在 06.GILab 下**没有生产者**（§9.2-AD / 任务 34：Forward 的阴影系统
    没被示例驱动，`Shadow` 与 `RSM_Generate` 两个 pass 都不注册）。三条判据在"某个源恒为 0"时
    **全部成立**，所以这个检查当时**看不出**这件事。
    这也意味着 §9.2-AB 修好的是 **IBL** 那一半（`{IBL}` 0.1269305 是真的），RSM 那一半要等 34。
  - **〔任务 34 之后的本节读数〕**（同一检查，现在 10 条判定全过）：`{RSM}` = **0.0866324**、
    `{IBL,RSM}` = **0.1067809**（加权平均 `(0.1269305+0.0866324)/2 = 0.1067815`，相对误差
    0.000% 不变）、`S_rsm` = **8.875e-05**（改前恰为 0）、空栈对照仍是 **0.0865436**。
    `S_rsm` 从"只报告"升级成断言，并新增①三张 RSM 图逐级非空（覆盖 53.36% / 53.36% / 53.31%）
    ②**视角无关**：相机沿 x 挪 300 后三张 RSM 图逐字节相同（详见 §10.2 任务 34）。
- **顺带修掉的 §9.2-AB**（不修这一项就没法做上面任何一条判据）：Forward 的 RG 路径从不把
  场景天空盒交给 `GI_IBL` ⇒ 烘焙出近全黑的辐照度图 ⇒ Forward 的 IBL 漫反射/镜面恒为 0。
  实测指纹：改层栈时读数逐位相同（0.1836214），而把 UBO 的 `count` 画到颜色上又能看到
  1 与 2 的差别 ⇒ 配置与 UBO 都通，是源的贡献本身为 0。修法与读数见 §9.2-AB。
- **回归**：Deferred 侧逐项不变（`none` 0.0577337 / `ddgi` 0.0873535 / `ssgi` 0.0662770、
  `S_ddgi` 0.0308173、p5 3.4× / corr 0.6817）；`confidence_check` 2/2、`ssgi_cal_check` 3/3、
  `rtgi_coupling_check` 0.000%、`rsm_gate_check` 3/3、`ssr_check` 全过、`ddgi_grid_check` 全过；
  单元测试 **168 例 / 4242 断言**全过（其中两条断言正是"任务 8 与任务 26 的分界线"：
  `IsAvailable(IBL/RSM, Forward)` 必须为真，以及 Forward 的 `Degrade` 必须留下 IBL、
  且不留下任何屏幕空间源）。
- **未做**：`PBR.frag` 被 02.Cube / 03.Sponza-Forward / 07.AISamples / Editor 共用，本轮只在
  06.GILab 上做了数值判据（其余示例只保证编译通过、未逐个跑图）。它们的 Forward 路径
  与 06.GILab 同源，风险在于"原先 IBL 恒为 0"这个缺陷被修掉之后，这几个示例的画面会**变亮**
  （那是修复而不是回归），但需要一次目视确认 —— 记在 §11.4 的风险表里。

**27 · §9.2-X 的修复与实测** —— ✅ **已完成**

- **复现（原样）**：同一场景/相机、只改层栈 —— 三通道全空 ⇒ `mean 0.2022 / max 463.32`
  （最亮像素固定在 (416,996)，rgb=(281.8, 493.0, 704.0)）；`specular={IBL}` ⇒
  `0.0694 / 42.20`（同一像素 rgb=(25.7, 44.9, 64.1)）；只放 `ao={SSAO}` 而 specular 仍空 ⇒
  回到 `0.2022 / 463.32`。
- **定位过程（两次排除法，都值得复用）**
  1. 先把直接光关掉（`gi_solo=1`）：整个画面变成 **0**，连那个亮点也没了 ⇒ 亮点**来自直接光
     路径**，与镜面通道的合成无关。这一步把"空通道 → `specNum/max(specDen,1e-4)` 放大一万倍"
     这个原推断直接否掉了（空栈时 `specNum` 恒为 0，除以 1e-4 仍是 0）。
  2. 于是去看直接光路径上**与层栈无关**的采样点：`DeferredLighting.frag.slang` 里
     `u_BRDF_LUT.Sample(...)` 是逐像素无条件执行的（BRDF 的菲涅耳/环境项），而那张 LUT
     是 **IBL 烘焙的产物之一**。帧图里烘焙 pass 的注册条件是「漫反射栈要 IBL ∨ 镜面栈要 IBL
     ∨ DDGI 要 IBL」—— **漏了第四个消费者：BRDF LUT 谁都要**。镜面栈为空且 IBL 不在别处时，
     pass 不注册 ⇒ LUT 从未被写入 ⇒ 直接光的 BRDF 读**未初始化显存**。
  3. 这解释了全部三个读数：`specular={IBL}` 触发烘焙 ⇒ 正常（42.20）；三通道全空或只放
     `ao={SSAO}` ⇒ 不触发 ⇒ 463。
- **修法**：帧图里 IBL 烘焙**恒注册**（去掉消费者清单），pass 内部仍按 `IsDirty()` 早退 ——
  不脏时它什么也不做（`[GI 耗时]` 里 IBL 恒为 0 就是证据）。这是 §9.2-Q 那一族缺陷的
  **第三个实例**（Q 修的是"只看漫反射栈"，这里发现"四个消费者只列了三个"），
  教训写进 §9.2-Q 的续行：**一份产物 + 多个消费者，不能再按手工清单门控**。
- **实测（修后）**

  | 配置 | 改前 | 改后 |
  |---|---|---|
  | 三通道全空 | mean 0.2022 / **max 463.32** | mean **0.0429** / max **42.20** |
  | 只放 `ao={SSAO}`（specular 空） | 0.2022 / 463.32 | **0.0429 / 42.20** |
  | `specular={IBL}`（对照） | 0.0694 / 42.20 | 0.0694 / 42.20（不变） |

  ⇒ 任务判据「三种配置的 `max` 都应落在 42 量级」达成，而且能看出**这不只是"一个亮点"**：
  空镜面栈时整幅画面的直接光都被算错了（均值差 4.7 倍），亮点只是它最显眼的症状。
- **判据（新增 `Tools/gi/ibl_lut_gate_check.ps1` + `.py`，全过）**：三条数字判定（每种配置的
  `max ≤ 100`）+ 一条结构判定（空镜面栈下 `IBL_Bake` **必须仍在 pass 列表里**，实测三个配置
  都是 121 行 `RG pass: IBL_Bake`）。第二条是关键：数字会随场景/相机变，而"烘焙被注册"
  这件事是这条不变量的直接表述。
- **对历史读数的影响（必须记住）**：任何以**空镜面栈**为基线的**绝对值**都受过这一项污染
  （BRDF LUT 是垃圾 ⇒ 直接光整体偏亮约 4.7 倍）。**做差对照不受影响**（两侧同样被污染），
  所以此前那些"差分/比值"结论仍然成立；但凡是引用"空栈时的绝对亮度"的地方都要按修后值重看。
  本仓库里以空镜面栈为基线的检查（如 §10.2 任务 16 的 RSM 检查）在实现时都已特意保留
  `specular={IBL}`，正是为了避开这一项 —— 这条经验现在有了解释。
- **回归**：Deferred 的三个标准变体读数逐项不变（它们本来就带 `specular={IBL}`，烘焙一直有触发）；
  `confidence_check`、`ssgi_cal_check`、`rtgi_coupling_check`、`rsm_gate_check`、`ssr_check`、
  `forward_stack_check`、`ddgi_grid_check` 全过；单元测试不变。

**28 · §9.2-Y 的修复与实测**

- **现象（原样）**：两份**逐字节等价**的"漫反射四个权重全 0"配置落到两种有效层栈 —— 一种保持
  空（`none` 基线 0.05773），另一种被补成 `{IBL}`（0.07554），相差 **32%**；而且后者的运行
  结束会把配置文件**回写**成 `gi_blend_diffuse_w0=1.000000`。以"空栈"为基线的做差实验因此
  不可信。
- **根因（两个写者 + 一处静默补源）**
  1. `GIRegistry::Degrade` 末尾有一段**兜底**：某通道被裁空时补一个 IBL（AO 补 SSAO）。
  2. 配置加载路径**不经过** `Degrade`（直接按 cfg 重建层栈）⇒ 空栈保持空。
     而任何经过 `Degrade` 的路径（预设按钮、阴影下拉、`ForwardPipeline::Initialize`）都会把它
     补成 `{IBL}`。
  3. 示例退出时保存的是**内存里的那份** `GIConfig`，于是"被补过"的状态落回文件 ⇒ 下一次运行
     读到不同的配置。**同一份文件、两种含义**，正是本仓库反复记录的"配置说谎"。
- **修法**：`Degrade` 改成**只裁不加**。理由是空通道本来就是一个**有定义的合法状态**
  （§3.1：漫反射/镜面合成返回 0、AO 取 1 不遮蔽），而且它正是采样设施做差实验的基线；
  一个语义上是"移除不可用源"的函数不该顺手往里加源。若将来某个调用方确实需要"保证非空"，
  应由它自己显式补源并在 UI 上说清楚。
  同时把**空层栈该不该被允许**这个问题定下来：**允许**，且现在它在任何路径上都同义。
- **判据（`Tools/gi/repeatability_check.ps1` + `.py`，三条全过）**

  | 判定 | 读数 |
  |---|---|
  | 同一份配置连跑 5 次，读数一致（< 0.5%） | **0.0577333 ～ 0.0577335，离散 0.0003%** |
  | cfg 的层栈键逐键不变（静默补源会改 `gi_blend_diffuse_w0`） | **无任何键变化** |
  | 读数落在"空栈"量级而非"被补 IBL"量级 | **0.0577335 ≤ 0.065**（被补组约 0.07554） |

  第二条是关键：它直接对应 §9.2-Y 的原始指纹（`gi_blend_diffuse_w0` 由 0 变 1）。
  （检查里比较的是**配置状态键**而不是整个文件：示例退出时按设计会重写配置并补上后加任务的
  新默认键，那是保存行为、不是状态变化。）
- **代码级的那一半由单测锁**（`Tests/TestGITypes.cpp`）：`Degrade` 对三通道全空的配置**保持全空**、
  对"只含 Lightmap/RTAO 这类不可用源"的配置**裁成空**、且**幂等**（再裁一次不变）。
  这条性质是"任何调用路径都同义"的可操作表述，而它测的是函数、不是画面，所以放在单测里。
- **顺带降级一条历史观察**：文档此前记录"同一个二进制、同一份字节相同的 cfg 连跑四次分成
  0.057733 与 0.05719x 两组（差 1%）"。今天同样做 5 次，离散只有 **0.0003%**、**没有**复现分组。
  这个效应没有找到机制，最可能的解释是：那批测量在 §9.2-U 的**陈旧转储护栏**装好之前做的，
  混入了上一次运行的产物（那正是 §9.2-U 记录过的坑）。结论按"当前不可复现"记，若将来再出现
  分组，`repeatability_check.ps1` 会直接把它抓出来。
- **回归**：Deferred 三变体读数逐项不变（配置加载路径本来就不经过 `Degrade`，所以基线不动）；
  `confidence_check`、`rsm_gate_check`、`ibl_lut_gate_check`、`forward_stack_check` 全过；
  单测 **168 例 / 4246 断言**全过。
- **顺带把 §11.3 的采样注意事项补一句**：**空层栈是合法的、可复现的状态**，不要再为它准备
  私有 cfg 之外的额外处理（此前"空栈不可靠"的说法随本任务作废）。

**9 · §3.2 置信度体系** —— ✅ **已完成**（屏幕覆盖项落地）

- 现状（改前）：`GIChannelBlendParams` UBO 里没有 confidence 字段，实际只有"屏幕边缘 5% 降权"
  一条，且降权对象是一份**硬编码在着色器里的 id 列表**，与 C++ 的分类谓词各说各话。
- 做了什么：`GISourceSlotData::confidence`（复用原来的 `_pad`，**UBO 尺寸不变**）承载判据位掩码，
  由 `ToConfidenceMask` 在 `GIChannelBlendData::Add` 里统一推导；着色器只按位计算，
  源 id 列表从着色器里彻底消失（GTAO 从此自动获得同等待遇，AO 通道也一并走 `SourceWeight`）；
  边缘带宽 `edgeFade` 由着色器常量改为逐通道 UBO 字段（`GIConfig.edgeFade`，默认 0.05）。
- 判据（`Tools/gi/confidence_check.ps1` + `confidence_check.py`，两例）：
  1. `diffuse={IBL}` 与 `diffuse={IBL,SSGI}` 之差：最外圈（1 像素环）只为中央的 **1.6%**，
     中央仍为 **1.7e-2**（正对照：SSGI 没有变成死源）；
  2. 带宽 5%→50% 后，距边 5%~15% 环带上的同一差值降到 **32.4%**（解析预测 `2c/(1+c)=33%`）。
  **改前二进制第 2 例必失败（比值 1.000）**，第 1 例两边数值完全相同 ⇒ 默认路径未变。
- 未做的三类判据与理由见 §3.2 的说明（探针网格归任务 14；RSM 已在着色器内判无效；
  光追收敛度无逐像素信息可用）。
- 一个必须记住的数学性质：**置信度是相对再加权**——通道里只有一个源时它在 `num/den` 里
  精确抵消，因此**不能用"单源读数变化"来验证它**，必须用同通道双源做差。

**11 · 统一降噪框架** —— ➡️ **已迁出到《Lumen与Nanite完整设计规范》§5.1**（本仓库实现未变）

> 这项工作（`Denoiser` / `RTDenoiser` 共 9 个实例、9 套 PSO、14 张纹理，降噪器之间不组合）
> 真正的消费方是 **Lumen**（多信号共存），故任务 11 / 11.1 / 11.2 / 11.3 连同它们的判据与
> 实测证据一并迁到该文档的 **§5.1 统一降噪框架**：11.1（去重 + 参数可配）与 11.2（链条
> 数据化）已完成，证据原样带过去；11.3（按信号类型分派）待做，验收判据也写在那里。
> **本仓库的实现没有变动**：`GI/SpatialDenoiseAux.h`、`RTProvider` 的 `std::vector<Stage>`
> 与那批读数都是现役代码与既有实测。这里保留的最小信息只有两条 ——
> ① 设计与现状的逐项对照仍在本文档 **§4.4**（那是这份计划的输入，不随任务迁走）；
> ② 原任务 11.1 的过程教训（**A/B 对照必须给每次运行一份私有 cfg 副本**）已并入 §11.3。

**14（K）· DDGI 网格覆盖语义 + 网格自动拟合** —— ✅ **已完成**

- **两半必须一起做，这是本项最关键的判断**。设计稿只写了"网格外 ⇒ 置信度 0"，
  但直接只做那一半会让 DDGI 在大半屏幕上归零（下面第一组实测就是如此）——因为**当时
  的网格根本罩不住场景**。所以本项同时做了"判据"与"让判据不至于把源打死"的拟合。
- **做了什么**
  1. **覆盖判据**：`GISourceConfidence::kGIConfProbeGrid`（`1u<<1`，与
     `GICONF_PROBE_GRID` 逐位一致），由 `ToConfidenceMask` 只对 DDGI 置位（单一真值仍在
     C++ 侧，着色器不认识任何源 id）。着色器端 `ProbeGridConfidence(worldPos)` 把世界坐标
     换成网格坐标、取到最近边界的格距 `d`，返回 `saturate(min(d)+1)`：凸包内为 1、
     出界一格内线性淡出、再外面为 0。`SourceWeight` 多收一个 `gridCoverage` 参数，
     三个通道的调用点一起改（DDGI 通常只在漫反射通道，但判据按位给，不靠位置约定）。
  2. **网格自动拟合**：新增 `GI_DDGI::autoFitGrid`/`fitCellsMax` 与
     `FitGridToBounds(mn, mx)`；帧图每 30 次构图重算一次场景包围盒（遍历 `MeshComponent`
     的 `GetBounds().Transform(GetLocalMatrix())`）并喂给它。拟合规则**抽成了纯几何**
     `GI/GIProbeGrid.h::FitProbeGridToBounds`，因为"网格罩没罩住场景"是纯算术，
     抽出来才能被单元测试覆盖，而单元测试不链接 Render。
  3. **重建探针缓冲时历史必须失效**：探针数变了要重建 SSBO，而新缓冲是**未初始化显存**。
     原先只有 `static bool s_FirstFrame`（进程首帧）会把 `historyValid` 置 0，重建走的是
     另一条路 ⇒ 会把垃圾按 `blendAlpha=0.85` 的权重逐帧混进 GI（静默偏色，且读数随显存
     布局变化——与 §9.2-T 同类）。改为成员 `m_HistoryValid`，重建时清零。
- **三个实现细节值得留档**（都是"看起来能跑、其实错"的那类）
  - **不能用 `m_FrameCounter` 做重算节流**：它只在启用异步计算时才自增（`DeferredPipeline.cpp`
    里 `+= 2` 与 `SetTimelineBase` 同段），普通路径上恒为 0 ⇒ `% 30 == 0` 恒真，
    实测日志**每帧**打一行"网格已拟合"。改用独立倒计时 `m_SceneBoundsCountdown`。
  - **每轴按自己的边长取探针数**，不按最长边统一取：三轴共用同一格距（否则三线性插值在
    短轴上被拉伸），但探针数取 `ceil(size_i/cell)+1`。若统一取 16，Sponza 的 Z 轴
    （2288.2 / 248 = 9.2 格）会白铺 5 根探针 —— 16×8×16 = 2048 对 16×8×11 = **1408**，
    省 31%。
  - **原点是包围盒最小角**，不再是"中心对齐"：覆盖范围 `[mn, mn+(N-1)·cell]` 直接罩住盒体，
    不必再算中心与半跨度。
- **判据（三层，全部可复跑）**
  1. **单元测试**（`Tests/TestGIProbeGrid.cpp`，5 例）：逐轴覆盖 + 紧凑性（覆盖长度
     `∈ [size, size+cell)`，缺了后半条"把格距凭空放大一倍"也能过）、三轴同格距、最长边探针数
     等于请求值、短轴不随最长边、退化包围盒（零尺寸/反向/NaN/默认 AABB）返回 `nullopt`。
     全量 **166 例 / 4189 断言全过**（改前 161 / 3941）。
  2. **端到端回归检查**（`Tools/gi/ddgi_grid_check.ps1` + `.py`，4 个配置 / 5 条判定全过）：
     | 例 | 配置（拟合后的网格） | DDGI 画面贡献 `lum(HDR_ddgi)−lum(HDR_none)` | 判定 |
     |---|---|---|---|
     | 固定网格 | `ddgi_grid_auto=0`：8×4×8 格距 3，覆盖 21×9×21 | **2.9e-08 / −3.9e-07 / 3.4e-08**（三次运行，std ~1e-4） | 覆盖语义生效：出网格的查询被整片判为不可信 |
     | 拟合 8 | 8×4×6 格距 531（192 探针） | **1.896260e-02** | 网格真罩住场景后贡献恢复 |
     | 拟合 16 | 16×8×11 格距 248（1408 探针） | **1.896233e-02** | 同上 |
     | 拟合 32 | 32×14×21 格距 120（9408 探针） | **1.896260e-02** | 同上 |
     后三例两两相差 **0.0015%** —— 这是本项最重要的读数，见下条。
  3. **既有基线逐项不变**（同一次采样内）：`none` 0.0577332 / `ddgi` 0.0766960 /
     `ssgi` 0.0662770，DDGI 差分 **0.0195949**（文档原值 0.0195946）；
     `p5_spectrum` 复现 §3.3 全部数字（量级比 2.2× / 2.3×、`corr(SSGI,DDGI)` **0.1480** /
     0.1461、最低频段能量 **99.74% / 99.42%**、未解释方差 98.9%）；
     `confidence_check` 2/2、`ssgi_cal_check` 3/3（白炉 1.0000、`S_both/mean` 1.0419）、
     `rtgi_coupling_check` 0.000%、`rsm_gate_check` 3/3、`stack_switch_check` PASS。
     即：**这次改造不移动任何既有读数**——原因见下条。
- **最重要的一条实测结论（它同时是任务 17 的前提）**：格距 531 / 248 / 120（192 / 1408 /
  9408 个探针）三种拟合给出的画面贡献两两相差 **0.0015%**。唯一解释是**探针场本身是均匀的**。

  > ⚠️ **本节的读数在任务 17 之后全部变了**（探针场不再均匀）：同一条判据从 **0.0015% → 37.93%**、
  > `S_ddgi` 从 **0.0195946 → 0.0308178**、`p5_spectrum` 的量级比 2.2× → **3.4×**、相关性
  > 0.1480 → **0.6818**（见 §10.2 任务 17）。本节保留任务 14 当时的读数，因为它们是"缺陷确实
  > 存在"的证据；**引用当前值时请用任务 17 那一节的数字**。
  读代码可确认：默认配置下 RSM 不在漫反射层栈 ⇒ `u_Flags.x=0`，`DDGI.comp.slang` 的 IBL
  回退路径 `radiance = u_IBLIrradiance.SampleLevel(dir)` **只用方向、不用 `samplePos`**，
  而 Fibonacci 方向对每个探针都相同 ⇒ 每个探针的 SH 逐位相同。**这个源当前等价于
  "IBL 辐照度的方向函数"**：网格覆盖语义修好的是"别在没数据的地方编数据"，
  但网格里也还没有真正的空间信息。⇒ 任务 14 与任务 17 的关系是：
  **14 让网格覆盖变成有意义的问题，17 才让网格内容变成有意义的数据。**
- **成本**：探针数 256 → 1408（×5.5）后 DDGI 的 GPU 耗时仍是 **0.020～0.021 ms**
  （`cost_report.ps1`），与 256 探针时的 0.019～0.025 ms 不可区分——这个 pass 本来就只占
  GI 总预算（约 0.7 ms/帧）的 3%。若将来探针数需要再涨，任务 12 的 `updateStride` 可直接分摊。
- **取默认项的落点**：`autoFitGrid = true`、`fitCellsMax = 16`（Sponza ⇒ 格距 248、
  1408 探针）、`updateStride` 仍为 1。新增配置键 `ddgi_grid_auto` / `ddgi_fit_cells`
  （`06.GILab.cpp`，读写对称）。
- **相邻但未做 → 已由任务 30 修掉**（原记录留在这里作对照）：`GI_RSM` 的 `sceneCenter = (0,3,0)` /
  `sceneRadius = 60` 也曾是硬编码的，对 Sponza 这个 3720 单位宽的场景不合适。**任务 30 已改成
  按场景包围盒拟合**（`GI/RSMFrustum.h`，帧图每 30 帧重算包围盒并喂给 RSM），见 §9.2-AA ④。

- **15（J · 跨帧重绑）** —— ✅ **已完成**。要点：问题比 §9.2-J 记的更宽 —— 不止合成参数 UBO，
  Lighting 的一批**逐帧轮换资源**（光源/阴影/探针 SSBO、合成参数 UBO）全都绑在**同一份**
  描述符集上，逐帧重绑同样会跨帧串味。修法是**每飞行帧一份描述符集 + 一份 UBO**，
  渲染时只更新/绑定本槽位那份。判据是"读数逐位不变 + 校验条数不变 + 白炉/单测全过"，
  因为这类竞态本来就是"平时看不出来"的（这也是它此前一直被"值变化小"掩盖的原因）。
  **顺带证伪**：它不是 §9.2-Y（同配置两次运行分成两组）的成因 —— 改前/改后两个二进制
  各跑 4 次，两组读数在两边都照样出现。

**18 · Lightmap 源落地** —— ✅ **已完成**（结论：**本轮不落地**，把声明改对 + 记下前置条件）

- **改前的事实**：`GISourceId::Lightmap` 在 `ToPipelineCap()` 里**没有分支**，靠 `default`
  兜到 `kPipelineGINone` ⇒ `IsAvailable` 恒 false（面板按能力位过滤，所以选不到 —— 行为是对的），
  但**文档的 §5.1 写着「预留，可用」**。这就是一处「配置说谎」：同一句话在代码里是不可用、
  在文档里是可用。它是 §9.2 第 6 行的对应实现。
- **为什么本轮不真落地**（两个前置条件，都不在现有架构里）
  1. **逐像素的光照图键**：lightmap 必须按每像素的 **UV2**（或物体 id）查表才能取到自己的纹素。
     GBuffer 的七个 MRT 槽位（A/B/C/D/E/F/G）**已全部占满**（albedo+metallic / normal+roughness /
     emissive+ao / velocity / worldPos+F0 / disneyA / disneyB），拿不到新通道；而 Deferred 的
     Lighting 只看得见 GBuffer，没有别的途径拿到"这个像素对应哪个 texel"。
  2. **一条烘焙路径**（离线工具或载入时多次弹射）以及数据的归属（纹理图集 / 序列化 / 资源导入）。
- **为什么不拿"世界空间辐照度体"顶替**：不用新通道也能做的是**按世界坐标查表**的辐照度体
  （体积/探针网格），但那与 DDGI **是同一个估计量**，会被本仓库自己的 REDUNDANCY 诊断判为
  「重复估计」（§2.2）—— 启用它就是白付一份全量成本（§3.5 的判据）。**源的价值在于它是
  不一样的估计量**，不是数字上多一个槽位。
- **做了什么**
  - `ToPipelineCap()` 里把 Lightmap 从"没有分支"改为**显式给出 `kPipelineGINone`**，并把上述
    理由写进代码注释（后来者能看出这是决定、不是遗漏）；
  - `GISourceId::Lightmap` 的枚举注释从「预留」改成「**未实现**（见 ToPipelineCap）」；
  - 本文档 §5.1、§2.1、§9.2 第 6 行同步改成"明确不做 + 前置条件"，不再出现"可用"；
  - **新增单测锁住安全性质**：把 Lightmap 手工放进层栈后，`GIRegistry::Degrade` 必须把它摘掉，
    同通道可用源一个不少。本仓库最怕的失效形态是「源在归一化里计权重、却没有任何 pass 产出它」
    （§9.2-G/L），这条单测保证未实现的源不可能以那种方式进来。
- **判据**：单元测试 **167 例 / 4194 断言全过**（新增 1 例 5 断言）；文档里**不再有把 Lightmap
  断言为可用**的句子（「预留，可用」只剩在"改前的说法"这种历史引用里，§9.2 第 6 行与本节；
  `check_tables.py` 0 mismatch）。
- **改判条件**（免得当成永久否决）：一旦出现"静态几何 + 需要远高于 DDGI 分辨率的烘焙细节"
  这个真实需求（例如室内场景的墙角漏光/软阴影细节），就**重新开一项**去做 ——
  代价是给 GBuffer 加一个光照图键通道并用它查烘焙结果，收益是省掉每帧的实时 GI 成本。
  **注意**：原承接这项改判的任务 31 **已取消**（见 §10.2 的「31 · Lightmap 真落地」记录：
  它留下了光照图键通道与箱式投影键、以及 RHI 的 8 附件修复，但真正的展开、烘焙、源与三条
  判据都没做）——所以这句改判条件是"重新开一项"的依据，不是"复活某个任务编号"的依据。

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

**10 · SSGI-CAL SSGI 标度与量纲标定** —— ✅ **已完成**

- 动机（改前）：§9.2-L 修好后 SSGI 已真正生效，但实测（同一场景、单源层栈）DDGI 仍是它的
  **21 倍**，且 `GI/SSGI.frag` 的累加式**只含命中点 albedo**，余弦项混了样本长度、距离项量纲
  也不对 —— 它不是 `E/π` 的估计，在归一化里只拿约 **4.5%** 的贡献：每帧付一整幅 pass 的成本，
  却几乎看不见。
- **前置**：原列的"两处疑似几何缺陷"实际查出**三处**（§9.2 的 M/N/O，已完成）；**剩余缺口是
  量纲问题不是几何问题**，不能靠继续修方向解决。
- 做了三件事：
  1. **补入射辐射度**：`L_in` 取 `u_Radiance` = 共享组件 `GIRadianceHistory` 的**前帧 HDR**
     （SSGI 排在 Lighting 之前，取前帧既解决因果又构成多次弹射的时域反馈）。
     ⚠️ 这里踩到一个**同类缺陷**：该组件的捕获门控此前写死为"DDGI 是否启用"，于是
     「diffuse 层栈只放 SSGI」时从不捕获，SSGI 采样到一张从未写入的纹理、**输出恒为 0**。
     已改为由消费者声明（`IGIProvider::NeedsRadianceHistory`），与 §9.2-Q 的修法同源。
  2. **余弦项归一化**：`max(0, dot(N, normalize(sDir)))`；距离项**删除**（它此前用的是
     `dot(sDir,sDir)`，长度的平方；正确的距离衰减需要采样点与几何的实际距离，而本方案只在
     固定半径处取一个样本，没有这个信息 —— 固定半径采样对 `L` 的估计偏差改记为**已知近似**，
     不再用经验项掩盖）。
  3. **标定方式改为解析而非拟合**：估计量取 `Σ(L_in·cosθ) / Σcosθ`。因为 `∫cosθ dω = π`，
     它**精确等于 `E/π`** —— 归一化常数是 1，不需要"以 PT 为参考拟合一个增益"。这比原计划的
     PT 标定更硬：无收敛噪声、无拟合自由度、与采样核方向分布无关（权重在分子分母里抵消）。
     白炉条件（全白环境 + 接收面 albedo = 1）正是它的解析真值条件，故**白炉不再短路 SSGI**，
     于是白炉同时校验它的标度。
- **实测**（`Tools/gi/ssgi_cal_check.ps1` 三例 + 采样设施）：

  | 判据 | 结果 |
  |---|---|
  | 白炉解析真值（SSGI 单源，`E/π` 应为 1） | **1.0000 / 1.0000**（中心 / 背景） |
  | 同上，把估计式临时改回「除以 N」 | **0.444** ⇒ 判据确实在校验标度，非同义反复 |
  | 双源做差 vs 两单源做差的平均 | 比值 **1.042**（+4.2% 来自边缘置信度只丢 SSGI、不丢 DDGI） |
  | 单源做差量级比 DDGI / SSGI | **2.2×**（全掩码）/ **2.3×**（收紧掩码），改前 **21×** |
  | SSGI 在等权重输出中的占比 | **31%**，改前 **4.5%** |
  | 与 DDGI 的相关性（§3.3） | **0.1480**，改前 **0.9238** |
- **回归**：三/四变体读数自洽、白炉基线（`diffuse={IBL}`）仍 **1.0000**、未写入告警 0/0/0/0、
  单元测试 161/161 与 3941/3941、`stack_switch_check` / `rsm_gate_check` / `confidence_check` /
  `rtgi_coupling_check` 全过。
- **顺带的两条新观察**（已登记为缺陷）：§9.2-X（镜面层栈为空时的 463 亮点）、
  §9.2-Y（"空层栈"这一状态在不同运行里落到不同兜底，基线差 32%）。后者是本次把双源判据
  改为"全部走同一份采样设施"的原因。
- **仍然遗留**（不属本项）：SSGI 的噪声显著高于 DDGI（`std/mean` 1.8 对 0.45），
  现有的空间降噪几乎没动它 —— 需要更合适的降噪（原任务 11，已迁至《Lumen与Nanite完整设计规范》§5.1）。
- 风险：中——标定依赖 PT 参考的可比性；入射辐射度取自上一帧会引入时域反馈，需确认不发散
  （可复用 SSGI 现有的空间降噪与收敛检查）。

**19 / 20 · PROVIDER-EXEC · P6 + 绑定数组化** —— ➡️ **已迁出到《Lumen与Nanite完整设计规范》§5.2**

> 这两项（19 = 帧图执行单位从「Provider × 通道」改为「Provider」；20 = P6 ReSTIR GI 统一
> 估计器 + 纹理绑定数组化 / 跨通道共享）的验收对象**只有 P6 与 Lumen** —— 没有消费方的
> 泛化无法验收。故任务与它们的做法、迁移策略、验收判据、逃生口与风险一并迁到该文档的
> **§5.2**；这里只保留**设计背景**：本文档 **§4.3.5 的「改造分三层」**（第二层 = 19，
> 第三层 = 20，含「逃生口」那条重要结论）与 **§4.3.2 的旁路实例**不随任务迁走。
> **本仓库实现未变**：帧图仍是 7 条按 source id 的循环。

**16 · B3 RSM VPL halfRes** —— ✅ **已完成**（改前位置：`DeferredLighting.frag.slang` 的 `SampleRSMIndirect`）

- **做了什么**：把 16 点 Poisson 盘 VPL 求和（16 VPL × 2 张 RSM 贴图 = 32 次纹理采样）从
  Lighting 的**逐全分辨率像素**求值，搬进独立 pass `GI/RSMIndirect.{h,cpp}` +
  `Shader/GI/RSM_Indirect.frag.slang`，按 **1/2 分辨率**求值，Lighting 侧改为一次线性采样
  （`kGPUBinding_RSMIndirect = 5`，本 set 里最后一个空位）。这个 pass **不是 GI 源**
  （没有通道输出、不参与归一化），与 SSAO 同类由管线持有。
- **为什么先量再搬**：新加 `HE_PASS_TIMING=1`（每 120 帧一行 `[Pass 耗时]`，逐 pass 的 GPU 耗时）
  —— 面板上的 Profiler 数字脚本读不到，而"把某一项搬出 Lighting 到底省了多少"**必须看 Lighting
  自己**。量出来的事实是：Lighting 在漫反射层栈含 RSM 时 **0.882 ms**、不含时 **0.433 ms**
  ⇒ 这一项独占 **0.449 ms**，比 SSGI（0.436 ms）还大，是当时最大的单笔 GI 成本。
- **搬完之后的读数**（同一采样设施，同机）：
  | 指标 | 改前 | 改后 |
  |---|---|---|
  | Lighting（层栈含 RSM） | 0.882 ms | **0.513 ms**（+0.079 对空栈） |
  | VPL 求和所在 pass | （在 Lighting 内，0.449 ms） | **RSM_Indirect 0.109 ms**（半分辨率） |
  | 同上，若放全分辨率 | — | 0.310 ms（同代码只改 `kDownscale`，用于标定 1/2.8） |
  | RSM 项合计（对空栈的增量） | 0.449 ms | **0.188 ms（−58%）** |
- **半分辨率的保真度怎么量**（这一步不能省）：该项当前的实际贡献只有 1e-8 量级
  （§9.2-AA），在 16 位浮点转储的噪声底之下 —— 直接 A/B 只能得到"两边都是 0"，什么也证明不了。
  办法是**把信号放大到可测区间**：把 `RSM_VPL_ENERGY` 临时乘 1e6，同一份代码分别以
  `kDownscale = 1` 与 `2` 构建，比较单源做差 `S_rsm`：
  | 量 | 全分辨率 | 半分辨率 |
  |---|---|---|
  | `S_rsm` 均值 | 2.8230e-02 | **2.8060e-02（差 0.60%）** |
  | `S_rsm` 标准差 | 0.2696 | 0.1760（**降到 65%**） |
  | 逐像素相关 | — | **0.781** |
  | 覆盖像素（绝对差 > 1e-5） | 18.8% | 20.0% |
  ⇒ **均值保真、对比度有损**：半分辨率的双线性升采样把这一项的低频之外结构抹平了约 1/3。
  在当前量级下这不可见；但**任务 30 修好量级后必须重测这条**（写进任务 30 的判据）。
- **顺带修的两处**（都在同一段代码路径上，不修则"搬"这件事无法验证）：
  1. **查找 VP 与渲染 VP 不同源**：RSM 贴图是用帧图算的**固定、覆盖场景**的光源 VP 渲染的
     （为了 DDGI 的视角无关性），而 Lighting 侧查表用的是 `u_ShadowData[0].lightViewProj[0]`
     = **CSM 第 0 级**（拟合相机视锥）⇒ 写入与读取不在同一个光源空间。现在把
     `rsmLightViewProj` 直接交给本 pass，两边天然一致。
  2. **通量读错缓冲**（§9.2-AA ①）：`GI_RSM::RenderRSMPass` 把对象缓冲同时绑到
     `binding 1 (u_Lights)` 与 `binding 2 (u_Objects)`；新增 `GI_RSM::SetLightBuffer`，
     Deferred 与 Forward 两个调用点都传真实光源缓冲。同时把 `CollectLights` 提前到帧图开头
     —— RSM 排在 Lighting 之前，不提前就只能读到上一帧（或首帧未初始化）的光源数据。
- **门控**：`rsmPassRegistered ∧ 有启用的投影方向光 ∧ pass 已就绪` 三条同时成立才注册；
  没有产出时 `LightingInputs::rsmIndirectTex = nullptr` ⇒ LightingPass 回绑**黑色占位**
  （无间接光的语义中性值），与 SSGI/SSR 同一约定（§9.2-T）。等价于旧着色器里
  `shadowParams.w >= 0.5` / `shadowParams.z <= 0` 两条提前返回，但判据在 CPU 侧、可被日志看见。
- **一个必须保留的实现细节**：`PreBind` 必须在 `BeginOffscreenPass` **之前**调用。RHI 用
  "当前已绑定的 PSO"推导 RenderPass 来建 Framebuffer；先开 pass 再绑管线会建出附件数不匹配的
  Framebuffer（`VUID-VkFramebufferCreateInfo-attachmentCount-00876`），**设备直接挂住、进程不再推进**
  —— 这正是 §9.2-V 的同一个坑，本轮第一次跑就复现了它（现象与 V 的描述逐字一致）。
- **判据/回归**：新增 `Tools/gi/rsm_indirect_check.ps1` + `.py`（3 条判定全过：半分辨率 pass 在跑且非零、
  RSM 光栅化 pass 在、Lighting 对空栈的涨幅 < 40%）。**既有读数全部不变**：
  `none` 0.0577335 / `ddgi` 0.0766957 / `ssgi` 0.0662770、`S_ddgi` 0.0195942（文档值 0.0195946）、
  `p5_spectrum` 复现 §3.3（2.2× / 2.3×、corr 0.1480 / 0.1450、99.74% / 99.42%）、
  `confidence_check` 2/2、`ssgi_cal_check` 3/3、`rtgi_coupling_check` 0.000%、
  `rsm_gate_check` 3/3、`stack_switch_check` PASS、`ddgi_grid_check` 全过。

**25 · §9.2-W SSR 的 Hi-Z march 恒 miss** —— ✅ **已完成**（解析对照那一条由任务 32 补齐）

- **改前的指纹**：`specular={SSR}` 且其余层栈为空时，SSR 输出**逐像素 RGB=0、有效性 alpha 100% 为 −1**
  ⇒ 镜面层栈里"有 SSR"与"没有 SSR"**逐像素相同**。与 §9.2-L（SSGI 恒 0）同一种失效形态：
  开关为真、pass 在跑、纹理在写，内容恒空。
- **三处独立的错，缺一不可**（都靠读代码 + 落盘实测定位）
  1. **深度判据方向反了**。Hi-Z 是 **min** 金字塔（每格存**最近的**几何），而引擎是
     **zero-to-one**（近 0 / 远 1 —— 本文件自己的天空判据 `depth >= 1.0` 就是证据）。
     两条合起来只有一个方向：射线点深度 **小于** 该格最小深度 ⇒ 射线比这一格里的一切都近
     ⇒ 这一格是空的 ⇒ 前进。代码写的是大于（注释还按 reverse-Z 写着"近=1.0"）。
  2. **层级步长方向也反了**。`stepT = 1/2^level` 在 level 0 一步就跨**完整条射线**（t=1），
     第二轮 `t=1.5 > 1` 直接退出 —— 所以**即使把判据方向修对**，整条 Hi-Z march 也只有
     1~2 个采样点。Hi-Z 的层级是**屏幕空间**的金字塔（level L 的一个 texel 覆盖 2^L × 2^L 像素），
     步长必须折算成屏幕段上的 `2^L / 屏幕段长度(像素)`。现在先把射线投影成屏幕段 s0→s1，在其上做 DDA；
     终点落到相机后方（w≤0，投影无效）时把射线长度反复减半最多四次。
  3. **命中阈值量纲不对**。level 0 的判据拿 **NDC 深度差** 与 `thickness*0.1`（世界单位）比。
     现在与线性 march 用**同一个**判据：把该处深度用逆投影还原到 view 空间，比
     `|rayPos.z − gZ| < thickness`（世界空间厚度，量纲一致）。
- **线性回退路径的假命中也一并修掉**。`if(rayPos.z > rpZ) { hit = 0.5; break; }`：view 空间朝 −Z，
  "更近"= z 更大，所以 `rayPos.z > rpZ` 表示**射线还在几何之前**，根本不是命中 —— 而射线一路往前走，
  这条分支迟早会成立，于是它把大量"前方有东西"的像素记成命中。**实测**：把它单独放回去，
  有效像素占比从 **10.22% 涨到 73.91%**。⇒ 历史文档里那个"线性 march 有 **34.66%** 有效命中"
  正是被这条假命中抬起来的，**不能**当作"真实命中"的参照（本任务改写了这条说明）。
  新逻辑按行进方向提前结束：`(rayPos.z − rpZ) * R.z > 0` ⇒ 已经越过该处几何 ⇒ 结束且**不记命中**；
  `R.z == 0`（纯水平）时不早退，交给步数上限。
- **实测**（`Tools/gi/ssr_check.ps1`，`specular={SSR}`，Frame 120；GPU 耗时来自 `HE_GI_TIMING`）

  | 量 | 改前 | 改后 |
  |---|---|---|
  | Hi-Z 有效像素（alpha > 0） | **0%** | **13.03%** |
  | 线性 march 有效像素 | 34.66%（含上面的假命中）| **10.22%**（真实命中） |
  | HDR `{IBL}` → `{IBL,SSR}` | 逐像素相同 | **0.0577333 → 0.0596849（+3.38%）** |
  | SSR pass GPU 耗时 | 有 pass、无产出 | **0.254 ms**（Hi-Z）对 **0.806 ms**（线性） |

  ⇒ **Hi-Z 现在真的在加速**：3.2 倍，而且命中的像素还比线性多（13.03% 对 10.22%，
  因为它能在同样的步数预算里跳过空区域走得更远）。
- **判据（三条自动判定，全过）**：① Hi-Z 有效像素 ≥ 5%（改前恰为 0）；② Hi-Z 的命中最少是
  线性 march 的一半（实测 **1.28 倍**，同一量级）；③ 把 SSR 放进已有 IBL 的镜面栈后 HDR 读数
  必须变化（改前逐像素相同）。
- **当时没做的一件事（已由任务 32 补上）**：任务原文还要求"反射在**平面镜**前与解析镜像一致"。
  GILab 的 Sponza 场景里没有平面镜，做这条要新增一个测试场景并把镜像几何渲染出来做像素比对。
  任务 25 本轮**没有**做，所以那条判据当时仍然空缺 —— 当时的证据只到"射线有效性恢复了、两条 march
  路径同量级、Hi-Z 更快"，**没有**到"反射的位置/方向正确"。**任务 32 已补齐**：地面镜 + 两个已知
  立方体的解析对照，反射落点偏差 0.44 / 0.35 px，并因此又查出三处方向/尺度错（详见 §10.2 任务 32）；
  顺带发现**默认 Hi-Z 路径仍漏掉一个反射**（§9.2-AE / 任务 35）。
- **上一条判据在任务 32 之后的读数（本节的数字是任务 25 当时的）**：`ssr_check.ps1` 三条仍全过 ——
  Hi-Z 有效像素 52.54% 对线性 37.73%（1.39 倍）、加 SSR 后 HDR 0.0577333 → 0.0596745（+3.362%）。
- **回归**：默认档位（`specular={IBL}`）与漫反射三个变体读数逐项不变（`none` 0.0577334 /
  `ddgi` 0.0873536 / `ssgi` 0.0662770；`S_ddgi` 0.0308177、`p5` 3.4× / corr 0.6818）；
  `confidence_check` 2/2、`ssgi_cal_check` 3/3、`rtgi_coupling_check` 0.000%、`rsm_gate_check` 3/3、
  `rsm_indirect_check` 2/2、`ddgi_grid_check` 5/5、`stack_switch_check` PASS。
- **新增配置键 `ssr_use_hiz`（默认 1，关掉即强制线性 march）**：本任务的判据就是把两条路径摆在一起
  对照做出来的。此前只能靠临时改 `pc.useHiZ` 重编，那种"改一次代码量一个数"的做法留不下可复跑的检查。


- **为什么要单列**：任务 16 把成本砍下来了，但顺着读数发现这一项**本身不产出**
  （`S_rsm` 恒在 1e-8 量级、低于 16 位浮点转储的分辨率），而成本照付 0.188 ms/帧。
  这是 §9.2-L/W 同一族（"开关为真、pass 在跑、成本在付、内容恒空"），只是这一族的第四例。
- **要做什么（四项，一起过一遍）**
  1. **量级**：受光项是 `flux·cos·cos / d²`，接收点与 VPL 相距数百到数千世界单位 ⇒
     `1/d² ≈ 1e-6`；`RSM_VPL_ENERGY = 0.046875` 只做了"25 点 → 16 点"的积分归一，
     隐含"场景约 60 单位"的假设。要么给 VPL 补上面积/能量项（物理化），要么把整项改成
     与尺度无关的形式 —— **不能再用一个经验常数去吸收场景尺度**（§9.2-K/任务 14 已经
     在 DDGI 上踩过同一个坑：硬编码的场景尺度假设在本场景里全错）。
  2. **通量的通道约定**：`RSM_Generate.frag` 写 `(N·0.5+0.5).rgb + flux.a`；新建的
     `RSM_Indirect` 按这个约定读，而 `DDGI.comp.slang` 的探针更新把 `flux.rgb`
     （其实是编码法线）当辐射度用 ⇒ 两处必须统一（建议统一到"rgb=法线、a=通量"，
     并给 DDGI 的 RSM 路径补一条与新建 pass 相同的判定）。
  3. **通量的内容**：生成端注释写的是 `albedo * lightColor * max(NdotL,0)`，代码只写了
     `intensity * NdotL`（标量、无颜色）⇒ 这一项现在只能是灰度的。补齐后 RSM 才谈得上
     "单次反弹的颜色"。
  4. **光源视锥**：`sceneCenter = (0,3,0)` / `sceneRadius = 60` 仍是硬编码；任务 14 起帧图
     已经会算场景包围盒（实测 3720.9×1555.9×2288.2），直接消费它即可。
- **判据**
  - `S_rsm` 进入可测区间（≥ 1e-3 量级），且**打开/关闭 RSM 在镜面通道非空的配置下能被
    单源做差稳定读出**；
  - 与解析对照一致：单个方向光 + 一块朗伯地板时，地板上某点的 RSM 间接光与
    "一个 VPL 的解析辐照度"在 10% 内（`1/d²` 与余弦项的符号、量纲都要对）；
  - **半分辨率保真度重测**（任务 16 用放大信号量到均值差 0.60%、对比度降到 65%）：
    量级修好后要在**真实信号**下重测这两个数，若对比度损失在画面里可见，则把本 pass 的
    `kDownscale` 改回 1（成本回到 0.31 ms，仍在 0.449 ms 之下）；
  - 回归：三变体读数、§3.3 频谱、白炉、`rsm_indirect_check`、以及 §9.2-AA ①已修的
    光源缓冲绑定不得回退。

**（历史）25 · §9.2-W 的**改前**记录 —— 已被上面的「25」取代，保留以说明当时的定位过程**

- 当时的指纹：`specular={SSR}` 且其余层栈为空时，SSR 输出**逐像素 RGB=0、alpha 100% 为 −1**，
  HDR 与"镜面层栈里没有 SSR"逐位相同。与 §9.2-L 同类：开关为真、`IsValid()` 为真、
  pass 在列表里、纹理在写，但内容恒为空。
- 当时的定位方式：把 `pc.useHiZ` 临时强制为 0（走线性 march）重编，立刻得到 **34.66% 有效像素**
  ⇒ 判定"坏的是 Hi-Z 这条默认路径"。**这一步的结论对了一半**：坏的不止 Hi-Z —— 那个 34.66%
  里绝大多数是线性分支的**假命中**（见上面「25」的第 3 点与实测 73.91%）。
- 当时对根因的判断（方向反了）是对的，但**不完整**：层级步长的方向也是反的、level 0 的命中
  阈值量纲也不对，三处缺一不可。完整修法与实测见上面的「25」。
- **教训**：拿一条"看起来能出结果的回退路径"当参照之前，必须先验证**那条路径自己是不是对的** ——
  否则会把它的错误当成基准（这里就是把假命中当成了"真实命中率"）。

**2 · D1 崩溃根因** —— ✅ **已完成**（一条**可复现**的退出期崩溃根因获证并修掉；历史偶发访问违例
按测量结案，重开条件见末尾）

一条历史进展先说清楚（本轮之前的成果，保留原文）：已查实并修掉一条**偶发卡死**的根因
（与崩溃同属 D1 这一类「偶发失败」）：

| 项 | 内容 |
|---|---|
| 现象 | 采样 soak 24 次启动中有 **1 次**在第 42 帧挂住；日志末尾是 `VUID-vkAcquireNextImageKHR-semaphore-01779`（"Semaphore must not have any pending operations"） |
| 根因 | `VulkanSwapChain::AcquireNextImage` 复用一个 acquire 槽位前，只等了**该槽位自己的 acquire 栅栏** —— 它只能证明"信号已经发出"，证明不了信号量"**已被某次提交等待消费**"。真正等待该信号量的是那次 `vkQueueSubmit`，而它的完成没有任何一处在 acquire 之前被等待。帧循环的顺序（`AcquireNextImage` → … → `cmd->Begin()` 才等提交栅栏）让这个缺口必然存在，只在负载抖动时暴露 |
| 修法 | 提交后由命令列表登记该帧的提交栅栏（`SetAcquireConsumedFence`），复用一个 acquire 槽位前先等它；只等待、不重置（栅栏归命令列表所有）。交换链重建时清空登记并把槽位归零 |
| 验证 | 同配置 soak **40 次全部正常收尾**（OK 40 / CRASH 0 / TIMEOUT 0），且 40 份日志中该 VUID 出现 **0 次**（改前 1/24）。三变体回归：VUID 49/49/51、告警 0/0/0、读数与改前一致 |

### 2.1 证据链先修好：`HE_CRASH_TEST` 自检路径

改前的指纹（本轮起点）：`HE_CRASH_TEST=1` 主动写空指针之后 **进程静默挂住、崩溃报告一行都没写、
没有 minidump**，crash log 里只有安装那一行 —— 也就是说 **D1 的证据链本身是坏的**，这正是
"根因一直拿不到"的结构性原因。查下来是**三个独立原因**叠在一起：

| # | 原因 | 证据 | 修法 |
|---|---|---|---|
| ① | 崩溃路径用 `fputs(stdout)` + `fopen/fputs/fclose` + 互斥量 —— 取 CRT 的 stdio 锁，崩溃时无从知道哪个线程正持有它 | 改成"只用 `WriteFile`"之后仍然没有报告 ⇒ 至少还有一个原因 | 日志文件在安装时打开并持有 HANDLE，崩溃路径只 `WriteFile`（不碰 CRT） |
| ② | 只装 `SetUnhandledExceptionFilter` **不够**：那次访问违例根本没走到顶层过滤器 | 装 VEH 后同一份代码立刻给出完整报告 ⇒ 决定性对照。日志里还记下了"顶层过滤器先前值 = 非空（有别人装过）" | 增加**向量化异常处理器**（`AddVectoredExceptionHandler(First=1)`）：它在 SEH 之前、对每次异常都会被调用，报告不再取决于谁抢走了过滤器。只记录、一律 `EXCEPTION_CONTINUE_SEARCH`，不吞异常 |
| ③ | 报告本身不可诊断：卡在哪一步、是谁在跑，全都没有 | —— | 分**四个阶段**打日志（异常信息 / minidump / 符号化 / 收尾，且带 ASCII 标签 `[crash phase N/4]` 供脚本检查）；未处理过滤器失败时的**兜底栈扫描**（帧指针坏了也能指出调用者）；**崩溃时的线程清单**（线程 ID + 启动地址符号化）；**看门狗**（`HE_CRASH_WATCHDOG_SEC`，默认 60 秒，超时强制结束），把"挂死"变成有界失败 |

自检判据（`Tools/gi/crash_handler_check.ps1`，A 部分 7 条全过）：进程 **2 秒内**退出（退出码 2）、
minidump **14.3 MB**、报告 4.7 KB 且**含源文件行号的符号化帧**
（`06.GILab!main + 0x276B [Samples/06.GILab/06.GILab.cpp:853]` —— 正是自检那一行）。
顺带量出一件影响所有 harness 的事：**默认路径（交还 WER）要 62 秒才退出**（WER 自己再收集一份转储），
所以自动化场景加 `HE_CRASH_NO_WER=1` 让退出有界。

### 2.2 根因获证：单元测试的**退出期崩溃**（Jolt 注册生命期）

证据链修好后的第一件事就是去抓真实崩溃 —— 而 Windows 事件日志里其实一直躺着一条**可复现**的：
`HugEngineTests.exe`，`0xc0000005`，**出错模块 unknown、错误偏移 0**（即 `RIP = 0`），每次运行都在。

- **现象与二分**：单跑**任意一个**用例都崩（实测 **161/161**，退出码 `0xC0000005`），`--no-run`
  也崩；但**整包跑全部用例不崩**（退出码 0）。于是它与"跑了哪些用例"有关，而不是某个用例本身。
- **定位**：崩溃报告（配合兜底栈扫描）给出调用链
  `he::physics::`anonymous namespace'::dynamic atexit destructor for 's_World'`
  → `he::physics::PhysicsWorld::~PhysicsWorld` → `JPH::PhysicsSystem::~PhysicsSystem` →
  `JPH::IslandBuilder::~IslandBuilder` → `...operator delete[]` → **跳到地址 0**。
- **根因**：`Engine/Physics/PhysicsSystem.cpp` 里的 `he::physics::s_World` 是一个**进程级静态
  `PhysicsWorld`**，其中含 `JPH::PhysicsSystem`。Jolt 的 `JPH::Allocate` / `JPH::Free` /
  `Factory::sInstance` 都是**全局函数指针**，而本工程原先只在 `PhysicsWorld::Initialize()`
  里惰性注册它们（`EnsureJoltRegistered`）。静态对象的析构**与 Initialize 有没有被调用无关**，
  于是"从没初始化过物理"的运行（只跑 GI 单测、`--no-run`）在退出时走到 `delete[]`，
  经 `JPH_OVERRIDE_NEW_DELETE` 宏调用**空的 `JPH::Free`** ⇒ `RIP = 0`。
  这条也解释了二分结果：凡是有用例调用 `PhysicsSystem::Update` 的运行（`*Jolt*`、`*Physics*`、
  整包）都顺带完成了注册，退出就干净；`*Collision*`、`*GI*`、`--no-run` 这类不初始化物理的运行则崩。
- **修法**：`JoltRuntimeGuard` 作为 `PhysicsWorld` 的**基类**，其构造函数调用
  `EnsureJoltRegistered()`。必须是基类 —— 成员的构造发生在构造函数体之前，只有基类能保证
  "注册先于任何 Jolt 成员构造"。
- **验证**：单跑全部用例 **175/175 退出码 0**（改前 161/161 崩）、`--no-run` 退出码 0、
  整包 **172 例 / 5233 断言全过且退出码 0**。回归检查 `Tools/gi/crash_handler_check.ps1`
  的 B 部分把这件事钉住：三种运行形态（`--no-run` / 单用例 / 整包）都必须退出码 0
  且**崩溃日志内容里没有 `0xC0000005`**（注意日志文件本身在安装处理器时就会创建，
  所以判据是内容而不是"文件不存在"）。

### 2.3 历史偶发访问违例：按测量结案

- 原始的 06.GILab 偶发访问违例（历史约 1/55）在**本轮 12 次 + 之前 64 次 = 76 次启动中零复现**，
  无法定位到具体代码。
- 结案口径：**根因获证的范围** = 上面那条可复现的退出期崩溃（同一族：空函数指针调用、
  `RIP = 0`），它已被修掉并被回归检查守住；**未获证的范围** = 那条偶发违例本身。
- **重开条件**：任何一次运行出现访问违例（soak 的 CRASH 判定，或 `HE_CRASH_TEST` 之外的真实崩溃）
  —— 现在会自动产出 minidump + 带源文件行号的报告，直接按报告定位，不需要再"等复现"。
  证据链已由 §2.1 的检查守住，这正是本任务要达到的状态。


**32 · SSR 的解析对照（平面镜）** —— ✅ **已完成**

- **为什么需要它**：任务 25 的判据（射线有效率非零、两条 march 同量级、Hi-Z 更快）全是**内部
  一致性**，反射落在哪里它一个字也没说。平面镜给了闭式答案：把物体中心按镜面**镜像**，再经
  **同一个**相机投影回来，就是反射该出现的像素 —— 于是"反射对不对"变成一个可以断言到像素的
  问题，而不是目视。
- **装置**（`HE_SSR_MIRROR=1`，`Samples/06.GILab/06.GILab.cpp`）：y=1700 的**地面镜** slab
  （位置 (0,1699.5,0)、缩放 800×1×800）+ 红色立方体（中心 (−150,1750,100)、半宽 50）+
  绿色立方体（(170,1770,−120)、半宽 45）。转储新增 `ssr` 目标（SSR 自己的输出纹理）与两个
  附属文件：`_camera.txt`（pos / forward / up / fov / near / far / aspect）与 `_mirror.txt`
  （平面 + slab + 两个盒子的中心与半宽）。**为什么是地面镜而不是竖直镜**：SSR 只能命中深度图里
  **摄像机可见**的表面，而竖直镜面上的反射光线指向物体的背面/底面 —— 那个面深度图里没有。
  实测竖直镜方案在镜面上"有效率 **99.96%**"却**一个物体颜色都没有**（命中的全是镜面自己）。
- **先自检相机模型**（否则"反射错了"可能先是参照错了）：把盒心投到屏幕，量"最近的**自身**像素"
  到它的距离（不用质心 —— 立方体可见面是斜的，质心与中心投影差约 11 px）。实测 **0.49 px（红）/
  0.18 px（绿）** ⇒ 解析参照可信，之后才允许它判反射。
- **四处方向/尺度错**（解析对照一上来就暴露，"有效率"类判据全程看不见）：
  1. **空间混用**：`reflect(-V, N)` 里 `V` 是 view 空间、`N` 是世界空间 GBuffer 法线 ⇒ 反射方向
     只在相机与世界轴对齐时才对。指纹：预测像素附近有效率 **0.1%**（射线去了别处），而全局有效率
     依旧"正常"。修法：UBO 增传 `u_View`，用 `Nview = (float3x3)u_View · N` 再反射。
  2. **y 约定漏翻**：GBuffer 用**负高度视口**渲染 ⇒ NDC 的 +y 朝上、UV 的 v 朝下 ⇒ `ndc.y = 1−2v`；
     而"深度→view 重建"与"view→屏幕投影"**两处都**漏了这次翻转。两处漏项互相抵消 ⇒ 位置看起来
     自洽（有效率 48%~66%、画面也像反射），但重建出的几何在 y 上是**镜像**的：实测 shader 的
     `viewPos.y = +70.3`，真值 **−70.7**（x、z 都对）。位置镜像 + 法线不镜像 ⇒ 反射方向错。
     修法：`UvToNdc` / `NdcToUv` 两个 helper，重建、投影与 Hi-Z 的四个采样点全部改用。
  3. **起点自交偏移小于命中容差**：偏移是写死的 `0.1`（给"1 单位≈1 米"的世界写的），而容差按场景
     尺度取到 11.6 ⇒ 第一个采样点就落在**自身**的容差带里，**每条射线立刻命中自己**：地面镜上
     62% 的像素"有效"，输出的却是镜面自己的 albedo（白）。修法：`max(0.1, thickness*2)`。
  4. **一处同族的"假命中"**（§9.2-W 的历史数据来源）：线性 march 把"射线仍在几何之前"记成命中
     0.5 —— 把它单独放回去，有效像素从 10.22% 涨到 **73.91%**。改为按行进方向早退
     （`(rayPos.z − rpZ) · R.z > 0`），两个方向都覆盖。
  另有一处 Hi-Z 的**提前退出**：level 0 未过厚度判据时原本无条件 `break`，而粗层的 min 深度可能
  属于射线路径**旁边**的几何 ⇒ 整片反射被丢掉（红盒在预测像素附近有效率仅 0.1%）。改成与线性
  march **同一条**早退判据后红盒恢复；绿盒仍丢（§9.2-AE / 任务 35）。
- **march 参数按场景尺度重取**：历史默认 `maxDistance=50 / thickness=0.1 / stepSize=0.5 / maxSteps=64`
  是给米制世界写的，而 Sponza 的对角是 **4636.97** 单位。新增 `GI_SSR::autoScaleMarch`（默认开）：
  `DeferredPipeline_FrameGraph` 拿到场景包围盒后取 `maxDistance = diag`、
  `thickness = diag×0.0025`（≈**11.6**）、`stepSize = thickness`（**步长 ≤ 容差** ⇒ 线性路径不会
  隧穿）。注意线性回退路径的射程是 `maxSteps × stepSize`（默认 64 步 ≈ 742 单位），要覆盖全场景
  须显式加大步数（解析对照用的是 600 步）。
- **判据与实测**（`Tools/gi/ssr_mirror_check.ps1` + `ssr_mirror_check.py`，7 条判定全过）：

  | 判定 | 实测 | 阈值 |
  |---|---|---|
  | 相机模型自检：盒心投影到最近自身像素（红 / 绿） | 0.49 px / 0.18 px | ≤ 8 px |
  | 红盒反射落在解析预测像素（30676 个物体色像素） | **0.44 px** | ≤ 6 px |
  | 绿盒反射落在解析预测像素（13489 个物体色像素） | **0.35 px** | ≤ 6 px |
  | 反射的**定位**：远离预测的物体色像素占比 | 0.00% | ≤ 5% |
  | 相机平移 40 后反射跟着几何走：量测位移 vs 解析位移 | (−64.3,−2.3) vs (−64.8,0.0) ⇒ **2.39 px** | ≤ 5 px |
  | 米制参数**负对照**：物体色像素数 | 场景尺度 **30676** vs 米制 **2432** | ≥ 10× |

  负对照的细节：米制参数下镜面有效率只有 **0.25%**（场景尺度 4.60%），而且那 2432 个像素只覆盖
  红盒（绿盒一个都没有）—— 也就是说"历史默认参数"下的 SSR 基本上是坏的，这与任务 25 之前"SSR
  恒为 0"的印象一脉相承。
- **[KNOWN] 默认 Hi-Z 路径漏掉绿盒**：同一装置、`ssr_use_hiz=1`（默认）+ 场景尺度参数下，Hi-Z 找到
  红盒（0.44 px，8633 像素）却**完全找不到绿盒**（0 像素），而线性回退两条都准。Hi-Z 的镜面有效率
  反而**最高**（71%）⇒ 只有解析真值能判出这件事。已记为 §9.2-AE 与任务 35；本检查的断言因此钉在
  **线性回退路径**（`ssr_use_hiz=0`，pass 的正式回退路径、有自己的开关），Hi-Z 那一路作为
  `[KNOWN]` 报告项打印。
- **测量卫生（本次踩到的第二个坑）**：**不要在重新链接 exe 的同时跑转储检查**。本轮把
  `ssr_mirror_check.ps1` 与 `cmake --build` 并行启动，jitter 那一次运行拿到的是"链接中的 exe"：
  落盘的 SSR 里 14.4 万个像素"有效"却**颜色全为 0**（射线命中采样到 albedo=0 的天空像素），
  jitter 判定直接失败；等 exe 落地后串行重跑，四个 run 逐位复现、7 条全过。诊断过程中同时确认了
  pass 参数与相机在两条 run 里只差 x（临时日志：`steps=600 step=2 maxDist=2000 thick=2 useHiZ=0
  campos=(0|40,1950,600)`），排除了"配置没生效"这一路。同一族的教训见 §9.2-U（陈旧转储）。
- **改动清单**：`GI/SSR.frag.slang`（`u_View` + `UvToNdc`/`NdcToUv` + `Nview` + 起点偏移 + 两条早退
  判据）、`GI/GI_SSR.{h,cpp}`（UBO 加 `view`、`autoScaleMarch`）、
  `Pipeline/DeferredPipeline_FrameGraph.cpp`（按包围盒取 march 参数）、
  `Samples/06.GILab/06.GILab.cpp`（镜面装置 + `ssr` 转储目标 + `_camera.txt`/`_mirror.txt` +
  `ssr_auto_scale` 等 cfg 键）、`Tools/gi/ssr_mirror_check.{ps1,py}`（新）。
- **回归**：`ssr_check.ps1` 3/3（Hi-Z 有效率 52.54% vs 线性 37.73%，加 SSR 后 HDR 相对变化 3.362%）、
  单测 172 例 / 5233 断言全过、`check_tables.py` 0 处不一致。

**33 · §9.2-AC 光追命中点的量纲** —— ✅ **已完成**

- **改前的指纹**：`RT_HitCommon.slang` 的 `EvaluateHitRadiance`（RTGI / RT 反射 / DDGI 探针
  三处共用）把两项相加后**直接当辐射度返回**：
  `albedo·E_ambient + Σ(lightColor·intensity·max(N·L,0)·atten)`。
  两处量纲错：① 直接光项**没有乘命中面 albedo**；② 整体**没有 1/π**（朗伯面出射辐射度是
  `albedo/π·E`）。三处调用点里只有 `DDGI_Trace.rgen` 在调用后除了 π（任务 17 修探针过亮时
  加的补偿，注释就写在那一行），RTGI 与 RT 反射直接当辐射度用。
- **为什么此前看不见**：白炉测试对各源的短路分支（`if (furnace && id != SSGI) return 1`）
  让 RTGI 走不到真实路径；而 `rtgi_coupling_check` 是**相对**判据（两配置互比），1/π 的缩放
  它天然看不见。所以这一项的要求是"**修它与做判据必须同时**"。
- **修法**（三件一起）：
  1. 函数返回辐射度：`albedo/π·(E_ambient + E_direct)`；`DDGI_Trace.rgen` **去掉**它自己那
     个重复的 1/π；三个调用点一律不再做换算（换算只发生一次、在函数内部）。
  2. 函数加**白炉分支**：白炉条件（全白环境辐射度 1 + albedo 1 + 关直接光）下 E_ambient = π
     ⇒ 返回 `albedo`；`RT_GI.rgen` 的 miss 分支在白炉下取 1。于是**命中与未命中两条路径都返回
     理想值**，白炉读数**与场景几何无关、恒等于 1**。
  3. `DeferredLighting.frag` 的白炉短路把 RTGI 排除（与 SSGI 一样走真实路径）——
     否则判据仍被短路掩盖。
  白炉标志经 `RTExecuteContext::furnace` → push constant 的 `flags` bit2 传到 rgen/rchit
  （bit0=半分辨率、bit1=DDGI 是层栈源，沿用既有约定）。
- **判据与实测**（新增 `Tools/gi/rtgi_furnace_check.ps1`，5 条判定全过）：

  | 量 | 改前 | 改后 |
  |---|---|---|
  | 白炉 `diffuse={RTGI}` 中心亮度（解析真值 1.0） | **0.0000**（负对照：把白炉分支关掉） | **1.0000** |
  | 白炉 `diffuse={SSGI}` 中心亮度（任务 10 的判据） | 1.0000 | **1.0000**（未被破坏） |
  | RTGI 单源原始输出（非白炉，`prov9_raw` 均值） | 5.746e-2 | **1.202e-2**（比 0.209） |
  | RT 反射单源原始输出（`prov8_spec_raw` 均值） | 7.003e-2 | **3.787e-2**（比 0.541，天空占比大） |
  | `S_rtgi` / `S_rtrefl`（HDR 单源做差均值） | 1.124e-2 / 1.219e-1 | **2.351e-3 / 2.582e-2** |

  比值不是干净的 1/π，正因为改前有**两处**错：直接光项缺 albedo（≈0.5-0.7）与整体缺 1/π，
  两者一起给出 0.2 量级的比值；RT 反射的比值更接近 1（0.541），因为它的原始输出里天空反射
  （miss 路径，量纲本来就对）占比很大。
- **连带影响（同一函数 ⇒ 同一修正）**：DDGI 探针的**命中**项此前也少了 albedo，故 DDGI 的
  绝对读数整体下移，**当前基线**（任务 33 之后，Frame 120 / Sponza，`Tools/gi/dump_gi.ps1`
  + `analyze_gi.py`）：

  | 量 | 任务 30 之后 | **任务 33 之后（当前）** |
  |---|---|---|
  | `none` HDR 均值 | 0.0577337 | **0.0577335**（不变） |
  | `ddgi` HDR 均值 | 0.0873536 | **0.0775732** |
  | `ssgi` HDR 均值 | 0.0662769 | **0.0662773**（不变） |
  | `S_ddgi` | 0.0308175 | **0.0206226** |
  | `S_ssgi` | 0.0090255 | **0.0090259**（不变） |
  | `p5_spectrum` 量级比 / `corr(SSGI,DDGI)` | 3.4× / 0.6819 | **2.3× / 0.6457** |

  方向是对的：命中面越暗，它反射出来的间接光越少（改前这条路径完全不看命中面的 albedo）。
  §3.3 的结论（不存在使 SSGI 更像 DDGI 的低通尺度、DDGI 与 SSGI 只在中低频相关）**不变**。
- **一个必须记住的构建陷阱**（本次踩到，直接导致"改了没效果"）：**增量构建不跟踪 Slang 的
  include 依赖**。编辑被 include 的 `RT_HitCommon.slang` 之后，实测只重编了 `RT_GI.rchit`，
  `RT_Reflection.rchit` 没重编；另一次构建路径甚至完全没生效（exe 时间戳没变），读数与改前
  逐位相同。现在 `rtgi_furnace_check.ps1` **先检查 `.spv` 是否比 `RT_HitCommon.slang` 新**，
  不新就直接判失败并提示"touch 源文件后重建"，避免把陈旧字节码的行为当成代码的行为。
- **回归**：`rtgi_coupling_check` 0.000%（相对不变）、`ssgi_cal_check` 3/3（量级比 2.32×，
  仍在同一量级）、`ddgi_grid_check` 4/4（三种拟合贡献 2.08e-2 / 1.98e-2 / 1.59e-2，离散
  25.88%）、三变体读数见上表、`p5_spectrum` 结论不变、单测 172 例 / 5233 断言全过。

**34 · §9.2-AD 让 Forward 的 RSM 真有生产者** —— ✅ **已完成**

- **改前指纹**：`pipeline_mode=0` 下 `diffuse={RSM}` 的 HDR 与**空漫反射栈**逐位相同
  （都是 0.0865436），而 `forward_stack_check` 的三条判据在"源恒为 0"时全部成立 —— 任务 26
  的"Forward 的 RSM 读数"其实读的是"没有源"。修复过程中一共查出**四层**独立原因，前三层
  各自都足以让源恒为 0：
  1. **示例没驱动阴影系统**：`ShadowSystem` 要调用方先 `SetRenderResources`（对象/阴影缓冲 +
     描述符集）再 `Update`（收集投影光源、拟合 CSM），02.Cube / 03.Sponza-Forward / AISamples 都这么做，
     06.GILab 漏了 ⇒ `HasActiveShadows()` 恒 false ⇒ `Shadow` 与 `RSM_Generate` 两个 pass
     都不注册。**附带后果**：Forward 画面此前**连阴影都没有**。修法：Forward 分支里照 02.Cube
     驱动（并在之前 `SyncPhysicalSkyToSun`，阴影与光照同向）。
  2. **光源 VP 来源错**：本 pass 用的是 **CSM 级联 0** 的 VP，它①拟合**相机视锥**（RSM 是当作
     世界空间源用的，内容不能随视角变）②由 Shadow pass **执行时**才写进阴影系统，而帧图里
     Shadow 与 RSM_Generate 声明的是互不相干的纹理、**没有依赖边** ⇒ 顺序不受保证。
     修法：新增 `ForwardPipeline::RefreshRSMFrustum`，用与 Deferred **同一份**
     `FitRSMFrustumToBounds`（场景包围盒 + 光源方向，不含相机）、包围盒每 30 帧重算；
     该 VP 同时喂 RSM pass 与 `GIBlendParams.rsmLightViewProj` ⇒ **写入 UV 与查找 UV 同源**。
  3. **RG 路径从不绑定 RSM 纹理**：`UpdateRSMBindings()` 只在非 RG 的 `PrepareGI` 里被调用，
     RenderGraph 路径下 PBR 采样的是 Initialize 时绑的 **bindless 占位纹理** ⇒ 三张图有 53%
     覆盖、pass 在跑、描述符却指着占位。修法：每帧刷新光锥后调用一次（在 pass 注册/执行之前）。
  4. **§9.2-AF · 共享结构体布局漂移**（顺着③查出来的独立缺陷）：给 `GIBlendParams` 加字段后
     `rsmValid` 在着色器里恒读 0。根因是**非 float4 数组**：`float _padBlend[3]` 在 C++ 占
     12 字节，在 Slang 的 cbuffer（std140，数组元素步长固定 16）里占 48 ⇒ 其后所有成员偏移
     错开。`slangc -reflection-json` 实测 Slang 把 `rsmLightViewProj`/`rsmValid` 放在
     **304/368**（块大小 432），C++ 是 **256/320/336** ⇒ 着色器读的 `rsmValid` 落在 C++ 从未
     写入的区间，恒为 0。修法：填充一律 `float4`，并在 `Pipeline/Material.h` 里把 `sizeof`
     与三个 `offsetof` 逐个钉死（编译期拦截）。**为什么能潜伏两个任务**：`rsmVplScale` 在数组
     **之前**，所以任务 30 的"尺度"那一项一直是对的。
- **判据与实测**（`Tools/gi/forward_stack_check.ps1` + `.py`，**10 条判定全过**）：

  | 判定 | 改前 | 改后 |
  |---|---|---|
  | `S_rsm` = mean({RSM}) − mean(空栈)（断言 `> 1e-6`） | 恰好 **0** | **8.875e-05**（占屏幕均值 0.102%） |
  | 三张 RSM 图覆盖（pos / normal / radiance） | 0% / 0% / 0% | **53.36% / 53.36% / 53.31%**（radiance 均值 0.2049） |
  | 视角无关：相机沿 x 挪 300 后三张图逐字节相同 | 不可能（相机视锥） | **100.0000% 相同**，max\|diff\| = 0 |
  | 层栈改变画面 / 多源不变亮 / 双源 = 加权平均 | 三条都"过"（源为 0） | 三条仍过（相对误差 0.000%） |
  | `{RSM}` / `{IBL,RSM}` 读数 | 0.0865436 / 0.1067366（=空栈） | **0.0866324 / 0.1067809** |

  逐级判定的阈值与 Deferred 侧的 `rsm_indirect_check` 同形（位置覆盖 > 20%、法线覆盖与位置
  覆盖相差 < 2%、辐射度非零 > 20% 且均值 > 0.005）—— 两侧现在用**同一套**判据看**同一件事**。
  视角无关那条是这次新加的：拟合视锥只由场景包围盒与光源方向决定，所以世界空间源在换相机后
  必须逐字节相同；用相机视锥时它必然失败，因此它直接守着 §9.2-AD 的第 2 层根因。
- **回归**：`rsm_indirect_check` 8/8（Deferred 侧读数不变：位置 53.36%、`S_rsm` 4.284e-05 ——
  共享结构体加字段后逐字段仍是两端一致）、单测 **172 例 / 5233 断言**全过（新增的 `offsetof`
  静态断言在其中）。
- **残留风险（已记录）**：02.Cube / 03.Sponza-Forward / AISamples 也走 Forward 且都驱动阴影系统，
  它们的 RSM 现在同样改用固定光锥（内容与"相机视锥"时代不同）——这是**修复**（与 Deferred 同源、
  且视角无关），但只在 06.GILab 上做了数值判据，其它示例需目视确认一次（与 §9.2-AB 修好 IBL
  时留下的那条同类注意事项一致，见 §11.4）。

**31 · Lightmap 真落地** —— ❌ **任务已取消（从 §10.1 任务表删除）**，已落地的基础设施保留

- **决定**：本任务不再排期，已从 §10.1 的剩余任务表里删除。原先承接的"任务 18 改判路径"
  随之关闭（§10.2 任务 18 的改判条件仍作为**触发条件**保留：真出现"静态几何需要远高于 DDGI
  分辨率的烘焙细节"的需求时，按下面记录的现状重新开一项，而不是复活本编号）。
- **已落地并保留的基础设施**（都在仓库里、有回归检查，取消的是"把它做成完整的 lightmap"这件事）：
  1. **GBuffer 第 8 个 MRT `gb_lightmapkey`**（RGBA16F）：`GBuffer.frag` 的 `SV_Target7`、
     `GBufferRenderer`（纹理/PSO/清除值/RenderGraph 导入/访问器）、`DeferredPipeline_FrameGraph`
     的读写依赖、`LightingPass` 的点采样绑定与 `DeferredLighting.frag` 的声明、示例的
     `gb_lightmapkey` 转储；`ShaderTypes.slang` 两个空闲 binding 常量（4 = 键、7 = 光照图）。
  2. **键 = 程序化箱式投影**：页 = `objectIndex`、tile = 主导法线轴（页内 3×2）、tile 内 uv =
     世界位置按该物体自己的世界 AABB 归一化；为此 `GPUObjectData` 增加 `boundsMin/boundsMax`
     （176 → 208 字节，静态断言同步），三个写对象缓冲的地方（`SceneRenderer`、`CSMTechnique`、
     `GI_RSM`）都填世界 AABB。
  3. **回归检查** `Tools/gi/lightmap_key_check.{ps1,py}`（5 条断言全过：覆盖面 == 几何覆盖面、
     页号是精确整数且在 `kGPUMaxObjects` 内、uv 全在 [0,1]（100.00%）、128² 下 7.33% 的 texel
     含多个表面片 ≤ 10%）。
  4. **RHI 缺陷修复 §9.2-AG**（独立于本任务的真缺陷，必须保留）：`BeginOffscreenPassMRT`
     以前把颜色附件写死 7 个，加第 8 个 MRT 时 `vkCreateFramebuffer` 与 render pass 的附件数
     不一致，驱动在 `vkCmdBeginRenderPass` 崩溃。
- **未被验证的部分（随任务取消一起搁置）**：真正的展开（箱式投影还有 ~7% 的平行面重叠）、
  烘焙路径、`GISOURCE_LIGHTMAP` 源与能力位、以及三条判据（比 DDGI 锐 / 关掉实时 GI 仍成立 /
  每帧零成本）。**不要把上面这些通道与工具当成"lightmap 可用"的证据** ——
  `GISourceId::Lightmap` 仍然刻意不给能力位（`ToPipelineCap` 返回 `kPipelineGINone`）。
- **两次被判错的度量（留作教训）**：
  1. 用"页的世界 AABB 是否小于场景对角线 30%"判"一页是不是一个物体" —— 空心壳体按定义
     就不合格（Sponza 外壳：AABB 中心 30% 内几乎没有像素），于是被误判成"合批导致一页跨
     半个场景"。**代码事实**是 `GPUScene::Collect` 按组件（每实例）收集，一页只装一个物体。
  2. 用"同一个 texel 出现两个 0.25 单位量化的世界位置"判碰撞 —— texel 本来覆盖一片面积，
     **任何**参数化都过不了这条判据；正确做法是把 texel 内世界位置的离散度与 texel 自己的
     世界足迹比（换成它之后箱式投影的重叠是 7.33%）。

**35 · §9.2-AE SSR 的 Hi-Z 路径漏反射** —— ✅ **已完成**

- **改前指纹**（任务 32 的平面镜解析对照）：同一个镜面、同一帧、同一套场景尺度参数下，
  线性 march 把红盒与绿盒的反射**都**放在解析预测像素上（0.44 / 0.35 px），而**默认**的
  Hi-Z 路径只找到红盒（0.44 px，8633 像素，且整片图案是错位的），**绿盒一个像素都没有**。
  Hi-Z 的镜面有效率反而最高（71%）⇒"有效率 / 两条 march 同量级"这类判据天然看不见它。
- **根因**：Hi-Z 在**屏幕空间**做 DDA（每步 2^level 像素），但代码把**屏幕段的参数 `t`**
  直接当成**射线参数**用：
  `rayPos = rayStart + R*(worldLen*t)` 然后投影回去取深度做比较。
  透视投影把"世界线性"映射成"屏幕分数"的**射影**函数（`1/w` 在屏幕空间线性，位置不是），
  两者只在射线两端深度相近时才近似一致。解析量化（`Build/verify/hiz_param_error.py` 的手法）：
  在预测反射像素所在的屏幕分数处，这样算出的射线点比**真实**射线点远
  **1251**（红盒）/ **1481**（绿盒）世界单位 —— 是命中容差 11.6 的 **100 倍以上**，于是
  `|rayPos.z − gZ| < thickness` 比的是射线上**另一个点**。红盒恰好有几个像素落在正确的
  投影区域内（"最近命中 0.44 px"的判据被蒙过），绿盒的投影区域更小（94.8 px 对 150.6 px）
  就一个都剩不下。
- **修法**（`Shader/GI/SSR.frag.slang`，Hi-Z 分支）：用标准**透视校正**把屏幕参数换成射线参数。
  clip 坐标沿射线是线性的（`cA = Proj·(rayStart,1)`、`cR = Proj·(R,0)`），而 `1/w` 在屏幕
  空间线性 ⇒
  ```
  w(t)   = w0·wT / ((1−t)·wT + t·w0)
  tau(t) = t·worldLen·w0 / ((1−t)·wT + t·w0)
  rayPos = rayStart + R·tau(t)
  rayZ   = (cA.z + tau·cR.z) / w(t)
  ```
  顺带省掉每步的一次投影（原来每步 `mul(u_Proj, ...)` 只为拿自己的深度）。
  `t=0 → tau=0, w=w0`；`t=1 → tau=worldLen, w=wT`，端点自洽；`denom ≤ 0`（段内穿过相机
  平面）直接当 miss。
- **连带修正：步数预算**。修正后射线不再靠错误深度"蒙"到目标附近，要覆盖同一段屏幕距离
  就需要更多步。同一场景、同一套场景尺度参数，参照线性 march 的 30676 / 13489 个物体色像素：

  | Hi-Z 步数 | 红盒像素 / 最近命中 | 绿盒像素 / 最近命中 |
  |---|---|---|
  | 64（改前默认） | 0 / 无 | 0 / 无 |
  | 128 | 17719 / 0.44 px | 7971 / 0.35 px |
  | **256（新默认）** | **30137 / 0.44 px** | **15847 / 0.35 px** |
  | 600 | 32579 / 0.44 px | 15902 / 0.35 px |

  故 `DeferredPipeline_FrameGraph` 的 `autoScaleMarch` 把 `maxSteps` 提到 **256**。注意同一个
  字段也是**线性回退**路径的迭代上限：射程从 `64×11.59 ≈ 742` 变成 `256×11.59 ≈ 2967` 单位
  （覆盖更远，代价是回退路径的循环次数）。
- **判据**（`Tools/gi/ssr_mirror_check.ps1`，**13 条判定全过**）：

  | 判定 | 实测 | 阈值 |
  |---|---|---|
  | 相机模型自检（红 / 绿） | 0.49 / 0.18 px | ≤ 8 px |
  | 线性参照：反射落在预测像素 | 0.44 / 0.35 px | ≤ 6 px |
  | **Hi-Z：红盒反射落在预测像素** | **0.44 px，30137 像素** | ≤ 6 px 且 ≥ 线性参照的一半 |
  | **Hi-Z：绿盒反射落在预测像素** | **0.35 px，15847 像素** | 同上 |
  | **Hi-Z：两条反射的定位** | **0.00%** 远像素 | ≤ 5% |
  | 抖动（相机 +40） | 2.39 px | ≤ 5 px |
  | 米制参数负对照 | 2432 vs 30676 像素 | ≥ 10× |
  | **成本：步数预算** | **Hi-Z 256 步 vs 线性 600 步** | ≤ 60% |

- **【测量更正】任务 25 的"Hi-Z 比线性快 3.2 倍"不成立**。那是**错误 march** + 米制参数下的
  读数。修正后实测（`HE_PASS_TIMING=1`，同一帧、同一配置）：SSR pass **4.256 ms（Hi-Z 256 步）
  对 4.505 ms（线性 600 步）⇒ 仅 0.94×**；步数比是 **2.3×** 而时间比只有 ~1.06×。原因是每次
  Hi-Z 迭代要多付一次金字塔采样（level 0 细化时还要一次深度采样 + 逆投影），而**地面镜**这个
  几何会让层级长期停在 0（射线脚下的地面永远比射线近 ⇒"该格不空"⇒ 降级），层次结构帮不上忙。
  两件事一起记下来：① 现在有两个口径（步数是算法的量、时间是平台的量），判据只断言步数、
  把时间作为报告项打印；② **重开条件**：如果需要真正的耗时收益，要做"只在**穿越**时降级
  层级"（用相邻两级的深度判断，而不是任何一次被遮挡就降级）+ level-0 细化的批量推进；届时
  判据补一条时间比断言（当前刻意不写，避免把平台噪声当判据）。
- **顺带记录的成本事实**：SSR 在**场景尺度参数**下的单 pass 开销是 **~4.3 ms**（1080p），
  而任务 25 在米制参数下量到的是 0.25~0.8 ms —— 那些参数下射线走 50 单位就停，等于几乎
  不工作。SSR 的真实成本随"正确的射程"上了一个台阶，这是后续做降噪/半分辨率时的基线。
- **回归**：`ssr_check` 3/3 —— Hi-Z 有效率 **26.86%** 对线性 **46.31%**（比值 **0.58**，阈值 ≥0.5），
  加 SSR 后 HDR 0.0577334 → 0.0585041（+1.335%）。**【注意这个比值的方向变了】**：改前是
  Hi-Z 1.39×线性，而那个 1.39 正是"错误 march 的假命中"抬起来的（射线用错误深度到处判到
  "命中"，有效率自然高）。现在 Hi-Z 覆盖略低于 600 步的线性参照，若要更接近可再抬步数
  （成本随之线性上升；`ssr_check` 的 0.5 阈值仍满足，但余量不大 —— 这属于已知的取舍，
  写在这里以免下次误判为回归）。单测 172 例 / 5233 断言全过、`check_tables.py` 0 处不一致。

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

**12 · AMORTIZE 时间维分摊** —— ✅ **已完成**

- 做了什么：DDGI 探针更新由「每帧全量」改为「每 `updateStride` 帧更新一轮」：
  `probeIndex % stride == phase`（相位逐帧轮转）的那一批探针**重新估计**，其余探针
  **原样继承上一次的结果**。后者必须显式实现——两个探针缓冲是逐帧 ping-pong 的，
  本帧的"当前"缓冲装的是**两帧前**的值，而"历史"缓冲才是最近一次的结果，所以未轮到的
  探针要把历史拷进当前，否则每跳过一次就倒退一代、场永远收敛不了。
  与 `blendAlpha` 天然配合：探针本来就是"每帧新估计与历史做 lerp"，降频只是把同一个
  时间常数拉长为 N 倍帧数。参数经 `ddgi_update_stride` 配置键暴露。
- **判据：等量工作 ⇒ 等量结果。** 探针场的状态由**更新次数**决定而非帧数，所以比较
  `S(stride N, frame N·k)` 与 `S(stride 1, frame k)`（`S` = `lum(HDR{DDGI}) − lum(HDR{空栈})`）：

  | k | S(1, k) | S(4, 4k) | 比值 |
  |---|---|---|---|
  | 30 | 0.018579 | 0.018768（f=120） | **1.010** |
  | 60 | 0.018960 | 0.018961（f=240） | **1.000** |
  | 120 | 0.018962 | 0.018963（f=480） | **1.000** |

  ⇒ 4 倍分摊在**相同更新次数**下给出同一个探针场（同时验证了"历史拷贝"这一步是对的：
  漏掉它会让跳过的探针倒退，比值不可能这么准）。
- **代价（同帧比较）**：`S(4,120)` = 0.018768 对 `S(1,120)` = 0.018962，即 **99%** ——
  本场景的探针场约 **30 次更新**就收敛到渐近值（S(1,30) 已达 S(1,120) 的 98%），所以
  4 倍分摊在这里几乎看不出代价；代价要到"光照突变后探针需要多久跟上"这类场景里才显现，
  而那需要动态光照的判据（本轮没有）。
- **默认仍取 1（每帧全量）**：把默认改成 4 会让所有既有基线（§3.3、§8.2）重新churn，
  而"探针对光照变化的响应速度"这一面没有实测支撑。数据已经摆在这里，要打开随时一个
  配置键/一行默认值。每帧样本量：`256×32 = 8192` → `64×32 = 2048`（**−75%**）。
- 回归：三变体读数（`none` 0.0577334 / `ddgi` 0.0766959 / `ssgi` 0.0661235，DDGI 差分
  0.0195946）、白炉 1.0000、单元测试 161/161 与 3941/3941、`ssgi_cal_check` 3/3、
  `rtgi_coupling_check` 0.000%、`stack_switch_check`、`rsm_gate_check` 3/3 全过。
- **顺带发现的测量缺口**：本项想给出"每帧省了多少时间"，但面板上那些「SSGI 耗时 / DDGI 耗时 /
  IBL 耗时 / SSR 耗时」**全都是 0.00 ms** —— `GIDebugData::avgRenderTimeMs` 只有声明和显示，
  **全仓没有一处给它赋值**。已登记为 **§9.2-Z / 任务 29**：性能类任务（12/13/17）要的
  是真实读数，这属于测量基础设施，先补它再谈优化收益。

**13 · CULL pass 级空间剔除** —— ✅ **已实测判定：不做**

- 先测后做。任务 29 装上真实 GPU 耗时读数之后，**第一次能把"哪里值得优化"量出来**
  （`Tools/gi/cost_report.ps1`，本机 / Sponza / 1920×1080 / Frame 240）：

  | 配置 | 读数 |
  |---|---|
  | `diffuse={SSGI}` | **SSGI 0.436 ms**（其余项为 0 = 没跑） |
  | `diffuse={RTGI}` | RTGI 0.080 ms + **AS_Build 0.089 ms** |
  | `diffuse={DDGI,RTGI}` + RT 反射 + RT 阴影 | DDGI 0.021 + RT 阴影 0.027 + RT 反射 0.068 + RTGI 0.066 + AS_Build 0.088 |

  ⇒ **全部 GI 项合计约 0.7 ms/帧**，SSGI 一项占约六成；单个 pass 都在 0.1 ms 以下。
- 于是 tile / scissor 剔除的收益上限可以**算出来**：能剔除的只有"没有几何的 tile"。
  由 albedo 判据（`analyze_gi.py` 的 valid 掩码）得到**空像素约 7.2%**；而且屏幕空间着色器
  **本来就对天空像素做深度早退**（`SSGI.frag` 首行 `if(depth>=1.0) return 0`），所以剔除
  省下的只是这部分像素的**发射开销**，不是它们的着色成本。
  **上限 ≈ 7% × 0.436 ms ≈ 0.03 ms**，即 GI 总预算的约 **4%**。
- 代价一侧：逐 tile 剔除需要 GPU 侧算出占用掩码、再回读到 CPU 才能设 scissor；掩码必然
  是**上一帧的**（或更旧），边界移动时会出现"一条没有 GI"的带——§3.2 已经明确警告过
  「不能用视角相关的判据换取可见收益」，同一道理。**4% 的收益换一类边界伪影，不做。**
- **改判条件（写清楚，免得以后当成"永久否决"）**：几何只占屏幕一小部分（俯视/远景/
  室内一角）、GI 分辨率或采样数大幅提高、或将来出现单个 ≥2 ms 的 GI pass。
- 附带产出：这一项把**计时覆盖补齐到了 RT 源与 `AS_Build`**（此前它们读数为 0 是因为
  根本没打时间戳，而不是"不耗时"），成本结构从此可复核。

**17 · B4 / M5.2-A DDGI 光追 march** —— ✅ **已完成**

- **问题（任务 14 量化出来的）**：DDGI 的探针更新**根本不追踪几何**。`DDGI.comp.slang` 里
  `samplePos = probePos + dir * stepDist` 算完就丢在一边，每个样本只用 `dir` 去采样 IBL 辐照度
  cubemap；而 Fibonacci 方向对**每个探针都一样** ⇒ 整片探针场的 SH **逐位相同**。指纹：格距
  531 / 248 / 120（192 / 1408 / 9408 探针）三种拟合的画面贡献两两相差 **0.0015%**
  （`Tools/gi/ddgi_grid_check.ps1`）。也就是说这个源当时等价于「IBL 辐照度的方向函数」，
  探针位置没有进入结果。
- **做了什么**
  1. 新增 `Shader/RayTracing/DDGI_Trace.rgen.slang` + `GI/DDGITracePass.{h,cpp}`：**每条探针射线**
     （探针数 × 32）用硬件光追追踪一条世界空间射线 —— 起点是探针位置、方向是 Fibonacci 方向、
     距离上限 `4 × 格距`（本场景世界单位远大于米，固定"30 m"只够探针脚下）。命中 →
     复用 `RT_GI.rchit` 的命中点出射辐射度（与 RTGI 同一套评估，保证两个源量纲一致）；
     未命中 → 该方向的 IBL 辐照度（与改造前的回退同源）。结果写进
     `RWStructuredBuffer<float4>`，布局 `[probeIndex * 32 + i]`。
  2. `DDGI.comp.slang` 新增 `u_TracedRadiance`（binding 10）：`u_Flags.w > 0.5` 走 **A 路径**
     （光追），否则保持原来的 RSM/IBL 路径 —— **按 `supportsRayTracing` 自动选择**：
     设备不支持光追时 pass 不注册、DDGI 行为与改前逐位一致（这是降级路径，不是第二套真值）。
  3. 帧图顺序 `AS_Build → DDGI_Trace → DDGI`：DDGI 的 compute 读射线缓冲，必须排在后面。
     `AS_Build` 的注册从 RT 段**提前**到 DDGI 段之前，门控改成「开了任一 RT 源 **或** DDGI 走
     march」——否则"只开 DDGI"的配置里根本不会建 TLAS。
- **一个必须写下来的坑（第一版就是这样错的）**：`RT_HitCommon.slang` 的 `EvaluateHitRadiance`
  返回 `albedo × (环境 + Σ 光源强度·N·L)`，即 **albedo × 辐照度 E**；而漫反射出射辐射度是
  `L = albedo·E/π`。探针 SH 里"命中"与"未命中"两条路径必须是**同一量纲**（后者给的是 L），
  否则**同一个探针里命中样本比未命中样本亮 π 倍**，SH 就是两个尺度的混合。第一版没除 π：
  `S_ddgi` 0.0938、DDGI/SSGI 量级比 **10.4×**，直接把 `ssgi_cal_check` 的 Test 3 打挂；
  在 rgen 里给命中项补上 `1/π` 后 `S_ddgi` **0.0308**、比值 **3.48×**，检查恢复通过。
  （RTGI 侧沿用同一函数且不除 π，属既有约定问题，记在 §9.2-AA 的相邻项；本任务只保证
  探针内部两条路径自洽。）
- **实测**（Frame 120 / Sponza / 同一次采样设施；改前列为任务 14/16 的读数）

  | 量 | 改前 | 改后 |
  |---|---|---|
  | `ddgi` HDR 均值 | 0.0766957 | **0.0873538** |
  | `S_ddgi = lum(HDR{DDGI}) − lum(HDR{空栈})` | 0.0195946 | **0.0308178**（std 0.0242） |
  | 三种拟合（格距 531/248/120）的差异 | **0.0015%** | **37.93%** |
  | DDGI/SSGI 量级比（`p5_spectrum`） | 2.2× / 2.3× | **3.4× / 3.5×** |
  | `corr(SSGI, DDGI)` | 0.1480 / 0.1456 | **0.6818 / 0.6815** |
  | `DDGI_Trace` 耗时（45056 条射线） | — | **0.039～0.041 ms** |
  | `AS_Build`（DDGI-only 配置现在也要建） | — | **0.102～0.105 ms** |
  | `DDGI` 探针更新 | 0.021 ms | 0.021～0.022 ms（未变） |

- **判据**：`Tools/gi/ddgi_grid_check.ps1` 的第三条判定**按新语义反转**——三种拟合分辨率的画面
  贡献必须**显著不同**（≥5%），实测 **37.93%**。这条判定正是任务 14 用来记录该缺陷的那一条
  （当时读数 0.0015%），同一装置、同一配置、只反了结论方向，是"探针位置真的进入了结果"的
  直接证据。前两条（覆盖语义）保持不变：固定网格仍被门控到噪声量级。
- **回归**：`ssgi_cal_check` **3/3**（白炉 1.0000、双源差分 = 加权平均 1.081、量级比 3.48× < 5×）、
  `p5_spectrum` 结论**不变**（`corr(LP(SSGI),DDGI)` 仍不超过不低通的相关性 ⇒ 频率分离仍不成立，
  只是这三个数字整体移动）、`confidence_check` 2/2、`rtgi_coupling_check` 0.000%、
  `rsm_gate_check` 3/3、`stack_switch_check` PASS、`rsm_indirect_check` 2/2、
  `amortize_check` 全过、单元测试 166/4189 全过。
- **顺带修掉一条既有校验违规**：`VulkanCommandList::SetPushConstants` 在 RT 绑定点上固定用
  **五个** RT stage 推送，而各 RT 效果 Pass 只声明 `RayGen|ClosestHit` ⇒ 每次运行 10 条
  `VUID-vkCmdPushConstants-offset-01795`（既有缺陷，任何开 RT 源的配置都有）。修法：
  `RTEffectPass::Initialize` 统一把 push constant 范围补全到五个 stage。
- **已知代价与后续**（都写清楚，免得被当成"白拿"）
  - **DDGI 从此依赖加速结构**：只开 DDGI 的配置也要付 `AS_Build`（0.10 ms）与
    `DDGI_Trace`（0.04 ms），自己仍是 0.02 ms。这是"用真 GI 换掉均匀环境"的价格。
  - **探针场变噪**：差分图的标准差/均值从 0.45 升到约 0.79（32 条射线/探针 + 格距 248 + 没有
    探针降噪）。射线方向是确定性的（Fibonacci），所以**不闪**，但空间上更粗糙。后续可加射线数、
    提探针分辨率、或给探针加一次空间滤波。
  - **新暴露出的一条既有校验违规**：`VUID-VkAccelerationStructureGeometryTrianglesDataKHR-
    vertexFormat-03797`（BLAS 顶点格式 `VK_FORMAT_UNDEFINED`）现在也会在 DDGI-only 配置出现 ——
    它是 `RTPass::BuildAS` 里的既有问题，本任务只让它变得可见，未修。
- **方法论提醒（写进 §11.3 的读法）**：本任务的所有**判据**都用图像读数（差分、相关性、
  分辨率敏感性），不用耗时阈值 —— 本机的逐 pass GPU 耗时在会话之间会整体漂移（同一份二进制
  连跑两次，`Lighting(rsm)` 读到 0.774 与 1.105 ms；`GB_Clear` 在 1.44 到 2.08 ms 之间变化），
  足以把任何绝对阈值判翻。

**29 · §9.2-Z 给 GI 源装真实耗时读数** —— ✅ **已完成**

- 动机（改前）：面板上「SSGI 耗时 / DDGI 耗时 / IBL 耗时 / SSR 耗时」四行**恒为 0.00 ms** ——
  `GIDebugData::avgRenderTimeMs` 只有声明与显示，**全仓没有一处给它赋值**。它长得像一个可用的
  性能读数，而性能类任务（12 的分摊、13 的剔除、17 的 march）都会自然地去读它。
- 做了什么：每源每帧一对 GPU 时间戳（`vkCmdWriteTimestamp`）→ **环形查询池**（6 个）→
  **不阻塞**读回 → 滚动平均写回源自己的 `GIDebugData`；`HE_GI_TIMING=1` 时每 120 帧打一行
  `[GI 耗时]` 日志。接口侧新增 `IGIProvider::GetTimedPass()`（把读数落到源的落点）与
  `IRHICommandList::TryGetQueryResults()`（不阻塞读回）。
- **三个坑（都真实踩过，写下来免得重犯）**：
  1. **`GetQueryResults` 带 `WAIT_BIT`**：对"还没执行到"的查询会**永久阻塞** —— 第一版直接把
     进程挂死在第三帧。多命令列表（图形一条、异步计算一条）下，某组时间戳是否已提交从调用方
     看不出来，所以必须用不阻塞读回，拿不到就跳过这一帧。
  2. **整池可用性判断恒假**：一次读 64 个查询（32 源 × 2）时，**从未写过的查询永远不可用**，
     于是"整池可用"永远为假、读数永远是 0。必须**逐源**判断，且只读本轮实际写过的源。
  3. **过期读数**：pass 被注册但内部直接返回（IBL 不在脏时）会写出两个相同时间戳；若把它当成
     "测到了 0"就会跳过更新，面板会把**第一次烘焙的 5 ms** 永远挂着。现在按"本轮是否量到非零
     区间"决定是否向 0 衰减，于是"0"的含义是"最近没跑"。
- **判据与实测**（`Tools/gi/timing_check.ps1`）：SSGI 16 采样 **0.436 ms** → 64 采样 **1.271 ms**
  （**×2.92**，随工作量线性）；不启用 SSGI 的配置里它恒为 **0**；同一配置重复运行离散度 **0.0%**
  （判据上限 30%）。
- **顺带第一次拿到真实成本结构**（本机 / Sponza / Frame 240）：IBL 首次烘焙约 **5 ms**、
  SSGI(16) **0.44 ms**、DDGI 探针更新 **0.019 ms**。后者解释了任务 12 为什么量不出分摊收益：
  **DDGI 的探针更新在这个网格尺寸下只占百分之几毫秒，1/4 分摊的收益低于噪声**。真正的大头是
  SSGI 与 IBL 烘焙，这两者才是任务 13（剔除）与后续优化的对象。

**30 · §9.2-AA RSM 链路的量级与通道约定** —— ✅ **已完成**

- **改前的指纹**：`diffuse={RSM}` 与空漫反射栈的 HDR **逐像素完全相同**（`S_rsm = 0`，
  16 位浮点转储下连 1e-8 都取不出来），而 `RSM` 光栅 pass **0.086 ms**、`RSM_Indirect`
  **0.108 ms** 照付 —— §9.2-L/W/X 同一族的第五例："开关为真、pass 在跑、成本在付、内容恒空"。
- **查清的第一件事：三张 RSM 附件里只有清除值**（不是"量级太小"）。把 RSM 链路逐级落盘
  （新采样目标 `rsm_pos` / `rsm_nrm` / `rsm_rad` / `rsm_indirect`）后看到的三张图分别是
  `(0,0,0,1)`——正是 `rhi::ClearValue` 的默认值，即**一个片元都没通过**。
  根因是 `BeginOffscreenPassMRT` 的**清除值长度契约**：`clears` 必须是「colorCount 个颜色项
  + 末尾一个深度项」（共 colorCount+1 项），实现读 `clears[colorCount].depth` 当深度清除值；
  而调用点当时写的是 `ClearValue clears[2]` 且 colorCount=2 ⇒ **越界读到栈上垃圾当深度
  清除值**，深度被清成一个使 `LessEqual` 全失败的值。症状与"pass 没跑"一模一样。
  修法：调用点给足 4 项（3 颜色 + 深度），并把这条契约写进 `RHI/CommandList.h` 的接口注释
  （GBuffer 的 7+1 本来就是这么用的，RSM 是唯一写错的一处）。
- **第二件事：VPL 的 albedo 读的是别人的索引空间**。`RSM_Generate.frag` 需要 VPL 的漫反射率，
  原实现从 `u_Objects[objectIndex].baseColorFactor` 取，而这个对象缓冲是**相机可见性列表**
  （`SceneRenderer` 只写可见物体、索引是相机列表下标），本 pass 遍历的却是**全部网格**、
  索引是自己的计数器。实测只有 **3.18%** 的 texel 拿到非零 albedo（其余是没填过的槽 = 0），
  而"有 albedo 的 texel 占比"与"N·L>0 的 texel 占比"（53.31%）一比就露馅：辐射度非零的
  比例恰好等于 6% × 53%。修法：albedo 是**逐物体常量**，改走 push constant（`vplAlbedo`，
  pcRange 96 B 正好放得下），本 pass 同时改用**自己持有**的 `GPUObjectData[]`
  （只写 worldMatrix），不再借用管线的缓冲 —— 索引空间从此不存在歧义。
- **第三件事（本任务的主项）：量级归一是场景尺度相关的**。① 覆盖面：
  `sceneCenter=(0,3,0) / sceneRadius=60` 是硬编码的，只罩住 3720 单位宽场景的 1/60；
  ② 能量归一：`RSM_VPL_ENERGY = 0.046875` 是个经验常数，数值上**恰好等于"一个 RSM texel
  在半径 60 的光锥下的世界面积"**（`(120/512)² = 0.0549`，比值 0.85）—— 它把整项钉死在一个
  隐含的 60 单位场景上。修法两条腿：
  - 光锥由**场景包围盒**拟合（新纯头 `GI/RSMFrustum.h` 的 `FitRSMFrustumToBounds`，与任务 14
    的探针网格共用同一份包围盒，每 30 帧刷新一次）；正交盒恒取方形（texel 必须方），
    Sponza 实测半宽 **2174.8**（旧硬编码的 36 倍）。
  - 能量归一改成**解析面积**：`E/π = scale · Σ L_v·cosθ_s·cosθ_r/d²`，
    `scale = (radiusUV·2·halfExtent)² / N`（= 每个采样点代表的世界面积 / π，π 恰好约掉）。
    旧的硬编码常数与新的解析值之比 = **3942**，即这一项此前被低估了近四千倍。
    `RHI`-free 的单测锁住两条性质：**拟合出的正交盒逐角点罩住包围盒**（含"硬编码视锥一个
    角点都罩不住"的反证），以及**尺度不变性**（场景整体缩放 k 倍时 `scale ∝ k²`、`1/d² ∝ 1/k²`，
    估计量不变 —— 辐射度是尺度不变量）。
- **通道约定一起改对（原 ②③④）**：RSM 从「两个附件塞三个量（`编码法线.rgb + 通量.a`）」
  改成**三个附件一个量**——位置 / 编码法线 / VPL 出射辐射度
  `L_v = albedo·lightColor·intensity·NdotL/π`（带颜色，不再只能是灰度）。约定集中写在
  `ShaderTypes.slang` 的「RSM 贴图通道约定」一节，四处消费者（`RSM_Indirect`、`DDGI.comp`、
  Forward 的 `PBR.frag` 内联路径、采样设施）各自读**同一个通道**，不再需要"解包知识"：
  `DDGI.comp` 此前把 `flux.rgb`（其实是编码法线）当辐射度读，现在读 `radiance.rgb`。
- **判据与实测**（`Tools/gi/rsm_indirect_check.ps1`，7 条判定全过）：

  | 量 | 改前 | 改后 |
  |---|---|---|
  | `rsm_pos` 覆盖率 | 0%（只剩清除值） | **53.36%** |
  | `rsm_rad` 非零占比 / 均值 | 0% / 0 | **53.31%** / **0.205** |
  | `rsm_indirect` 非零占比 / 均值 / 最大 | 0% / 0 / 0 | **54.62%** / **2.01e-4** / **0.374** |
  | `S_rsm`（HDR 做差均值） | 0（噪声底） | **4.28e-5**，与 `E/π × albedo` 逐像素相关 **0.966** |
  | `RSM` / `RSM_Indirect` 耗时 | 0.086 / 0.108 ms | **0.147 / 0.126 ms** |

- **量级为什么仍然只有屏幕均值的 0.06%**（这一条要写清楚，否则下一个人会以为没修好）：
  用落盘的 RSM 图在 CPU 上重算同一个求和（16 点 Poisson 盘，采样盘半径 0.0125 UV）得到
  `E/π` 均值 **1.2e-3**，但**中位数为 0、只有 17% 的接收点非零** —— 光源在上方时，RSM 里
  可见的几乎都是**同朝向（朝上）的面**，接收点与采样到的 VPL 大多**共面**，两个余弦项同时趋零。
  也就是说这是**估计量本身**的性质（2.5D RSM + 单个小采样盘），不是单位错误：把采样盘半径
  从 0.0125 扫到 0.125（6.4 → 64 texel）均值只在 0.8e-3 ~ 2.0e-3 之间波动（同样非零占比
  17%），可见"扩大采样盘"不是这里的杠杆。判据因此守**结构**（逐级有产出 + 源到达 HDR），
  不守绝对量级；`S_rsm` 的底线设成 1e-6（"不许恒为 0"），而不是"接近某个期望值"。
- **半分辨率保真度重测**（任务 16 留下的待办）。任务 16 当时只能把信号放大 1e6 才能量，
  现在有了 `rsm_indirect` 落盘目标，可以直接比半分辨率结果与全分辨率结果（同一构建只改
  `kDownscale`）：

  | 量 | 任务 16（放大 1e6 后测） | 任务 30（真实量级直测） |
  |---|---|---|
  | 均值差（半 vs 全） | 0.60% | **0.25%** |
  | 逐像素相关（半 vs 2×2 盒式降采样的全） | 0.781 | **0.941** |
  | 对比度 `std(半)/std(全↓)` | 0.65 | **1.11** |
  | 覆盖像素占比 | 20.0% vs 18.8% | **52.46% vs 52.48%** |

  ⇒ **均值保真、对比度不再有损**（旧测法的 65% 是"放大 1e6 后噪声也被放大"的产物）。
- **顺带修掉一处契约越界读**（见上）与**确定性**：同一配置重复运行，三张 RSM 图的逐通道
  统计**逐位相同**（`mean=0.214091 / 0.20339 / 0.19268`），说明修后的链路可复现。
- **回归**：`none` **0.0577337** / `ddgi` **0.0873536** / `ssgi` **0.0662769**、`S_ddgi`
  **0.0308175**、`p5_spectrum` `corr(SSGI,DDGI)=0.6819` 且"不存在使 SSGI 更像 DDGI 的低通尺度"
  不变、`confidence_check` 2/2、`ssgi_cal_check` 3/3、`rsm_gate_check` 3/3、
  `ibl_lut_gate_check`、`stack_switch_check`、`ddgi_grid_check`、`forward_stack_check`、
  `rtgi_coupling_check` 0.000%、`repeatability_check` 全过；单测新增 `TestRSMFrustum.cpp`
  （6 例 / 994 断言）。

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
# 自动化实验一律用 Release（Tools/gi/*.ps1 默认 -Config Release，可执行文件在 Build\bin\Release）
cmake --build Build --config Release --target 06.GILab -j 8
& Build\bin\Release\06.GILab.exe
```

> **运行目录不影响资源定位**（此处曾写「运行目录即 `Build\bin\Debug`，热重载的相对路径依赖它」，
> 与工具的实际用法矛盾 —— 任务 21 修正）：`06.GILab` 的 Content 路径是编译期注入的**绝对路径**
> （日志里能直接看到 `D:/Source/HugEngine/Content/gltf/...`），`HE_GILAB_CONFIG` 也是绝对路径，
> 因此 `Tools/gi/*.ps1` 一律以**仓库根目录**为工作目录启动它都能正常加载。
> `cmake --build Build --config Debug` 依然可用（`HugEngineTests` 就是 Debug），只是 GI 采样脚本
> 统一按 Release 找可执行文件。
>
> **【构建陷阱：Slang 的 include 依赖不被增量跟踪】**（任务 33 实测）编辑**被 include 的**共享
> 着色器头（`ShaderTypes.slang`、`RT_HitCommon.slang` 这类）之后，增量构建**不保证**重编所有
> 包含它的着色器：实测只重编了 `RT_GI.rchit`，`RT_Reflection.rchit` 没重编；另一次构建路径
> 甚至整个没生效（exe 时间戳未变），于是"读数与改前逐位相同"——看起来像"修法无效"。
> **做法**：改共享头之后，`touch` 一下**使用它的**着色器源文件再构建（或者清理
> `build/Engine/Shader/Shaders/*.spv`），并核对 `.spv` 时间戳；`rtgi_furnace_check.ps1`
> 已把这条检查内置（陈旧字节码直接判失败）。

| 开关 | 用途 |
|---|---|
| `HE_FURNACE=1` + `HE_FURNACE_PROBE=1` | 白炉数值测试 + 像素读回（**自动化可用**，判据：绝对亮度 = 1.0） |
| `HE_TRACE_FB=1` | framebuffer 创建/销毁追踪（`delay=0` = 同帧销毁） |
| `HE_TRACE_PASSES=1` | 打印每个 pass 开始，把 pass 名与校验层报错在时间上对齐 |
| `HE_CRASH_TEST=1` | 主动在第 3 帧写空指针，自检崩溃处理器（**任务 2 起真的可用**：分阶段报告 + minidump + 符号化栈）。配套：`HE_CRASH_NO_WER=1` 让退出有界（跳过 WER，退出码 2；默认交还 WER 时实测要 **62 秒**才退出）、`HE_CRASH_WATCHDOG_SEC=<秒>`（默认 60，超时强制结束）、`HE_CRASH_VEH=0` 关掉向量化异常处理器。回归检查 `Tools/gi/crash_handler_check.ps1` |
| `HE_FURNACE_PROBE=1` 单用 | 只开探针不开白炉 |
| `HE_DUMP_GI=<标签>`（+ `HE_DUMP_GI_FRAME=<帧号>`，默认 60） | **GI 纹理级采样**：在指定帧整幅落盘到 `Build/verify/gi_<标签>_*.f16`（RGBA16F 原始像素、无文件头、行紧密排布），并写 `_meta.txt` 记录逐目标尺寸；落盘后**自动关窗退出**，便于脚本化。**HDR 目标按当前管线取**（任务 26 起）：`pipeline_mode=1` 取 `deferredPipeline` 的 Lighting HDR，`pipeline_mode=0` 取 `forwardPipeline` 的 HDR —— 此前无条件取前者，于是 Forward 模式下转储的**根本不是 Forward 的画面**（那时 Forward 的读数一直不可测；任务 26 的判据正是修好这一点之后才做得出来的）。**完整目标清单**（`06.GILab.cpp` 的 `addTarget`，纹理为空则跳过）：`hdr`（当前管线的 HDR 目标）、`albedo`（GBuffer MRT0，仅 Deferred）、`gb_worldpos` / `gb_normal`（GBuffer 的世界坐标 / 世界法线 —— 屏幕空间 GI pass 的**输入**；任务 30 靠它们把某个 pass 的公式在 CPU 上原样重算做逐像素对照）、**`radiance`（共享的前帧 HDR 辐射度 —— DDGI 探针与 SSGI 入射辐射度的共同输入，出问题时第一个要看的中间量，仅 Deferred）**、`ibl_irr`（IBL 辐照度，仅 Deferred）、**RSM 链路的逐级中间量**：`rsm_pos` / `rsm_nrm` / `rsm_rad`（当前管线的 GI_RSM 的三张附件）与 `rsm_indirect`（半分辨率 VPL 求和结果，仅 Deferred）—— 任务 30 的缺陷"pass 在跑、附件里只有清除值"就是靠这四级落盘定位的，只看最终 HDR 无法分辨"没产出"与"产出被合成丢掉"，以及**逐 Provider** 的 `provN_raw`/`provN_final`（漫反射）、`provN_spec_raw`/`provN_spec_final`（镜面）、`provN_ao_raw`/`provN_ao_final`（AO）。**多落盘几张纹理会增加校验层中与拷贝/屏障相关的条数，因此校验计数只在同一采样设置下可比**。Forward 侧目前只有 `hdr` 与三张 RSM 图（它没有 GBuffer，也没有 Provider 输出） |
| `HE_GILAB_CONFIG=<路径>` | 覆盖示例程序的配置读写路径（读写同一路径），使自动化实验**完全不触碰**仓库内的 `Content/Config/06_GILab.cfg`——否则每次实验都会被示例程序退出时回写覆盖 |
| `HE_GI_TIMING=1` | 每 120 帧打一行 `[GI 耗时] NAME=x.xxxms`（各源主 pass 的 GPU 耗时滚动平均），作为**面板之外**的脚本可读出口；面板上那四行读数本身就是同一份数据（§9.2-Z 修好后不再是恒 0） |
| `HE_PASS_TIMING=1` | 每 120 帧打一行 `[Pass 耗时] NAME=x.xxxms …`：**逐 pass** 的 GPU 耗时（含 `Lighting` / `GB_Clear` / `Shadow` / 非 GI 源自有的 pass），末尾附帧合计。与 `HE_GI_TIMING` 的分工：后者只覆盖"注册为 GI 源的 pass"，判断"把某一项搬出 Lighting 到底省了多少"必须看 Lighting 自己（任务 16 的判据就是靠它）。两者都走 `ProfilerManager` 已有的时间戳数据，**不是**新增测量机制 |
| `Tools/gi/vk_layer_settings.txt` + `VK_LAYER_SETTINGS_PATH=Tools/gi` | 关闭校验层重复消息上限，得到违规**真实次数**。**计数只在同一采样设置、同一去重条件下可比**：任务 17 之后，`dump_gi` 的四个变体在不设该文件时是 **60 / 71 / 60 / 71**（none / ddgi / ssgi / both；distinct 15/16/15/16）。比上一次记录的 56 多出来的部分有两个来源：① **DDGI 配置现在也建 TLAS 并跑一个 RT pass**（`ddgi`/`both` 多出 AS 相关的若干条，含既有的 `VkAccelerationStructureGeometryTrianglesDataKHR-vertexFormat-03797` —— BLAS 顶点格式 UNDEFINED，属 `RTPass::BuildAS` 的既有问题，本任务只让它变得可见）；② 新增的 `DDGITracePass` 管线/SBT 在**初始化时**创建（与层栈无关，故四个变体都会多几条）。**同一次任务里还修掉 10 条**：`VUID-vkCmdPushConstants-offset-01795`（RT 绑定点用五个 RT stage 推送、而布局只声明两级，任何开 RT 源的配置每次运行 10 条）已在 `RTEffectPass::Initialize` 里统一补全 stage 掩码。见 §11.3.1 方法论第 6 条 |

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
| `Tools/gi/soak_launch.ps1` | **批量启动 soak**（D1 用）：逐次判定 OK / CRASH / TIMEOUT / NODUMP 并汇总，崩溃报告单独留存；把"偶发"变成可度量的频率。**注意该脚本刻意只用 ASCII** —— Windows PowerShell 5.1 按 ANSI 读 `.ps1`，非 ASCII 注释会破坏解析 |
| `Tools/gi/crash_handler_check.ps1` | **崩溃处理器检查**（任务 2 的回归测试），两部分共 13 条判定：**A** 用 `HE_CRASH_TEST=1` 主动崩溃，要求（1）进程**在超时内退出**（不再静默挂住）、（2）退出码非 0、（3）minidump 写出且新鲜且 ≥1 MB、（4）报告新鲜、（5）报告分四阶段完整（匹配 ASCII 标签 `[crash phase N/4]`）、（6）报告写出异常码与空目标地址、（7）报告含**带源文件行号**的符号化帧（`06.GILab!... [*.cpp:NNN]`）。**B** 退出期崩溃回归：单元测试三种运行形态（`--no-run` / 单用例 / 整包）都必须退出码 0 且**崩溃日志内容里没有 `0xC0000005`**（日志在安装处理器时就会创建，所以判据是内容而非存在性） |
| `Tools/gi/rtgi_furnace_check.ps1` | **RTGI 白炉判据**（任务 33 / §9.2-AC 的回归测试）：白炉条件（全白环境 + albedo 1 + 关直接光）下 `diffuse={RTGI}` 的探针读数必须为 1.0（解析真值；命中与未命中两条路径都返回理想值 ⇒ **与场景几何无关**），同时复核 `diffuse={SSGI}` 仍为 1.0（任务 10 的判据）。**先做字节码新鲜度检查**：`.spv` 必须比 `RT_HitCommon.slang` 新，否则直接判失败 —— 增量构建**不跟踪 Slang 的 include 依赖**，陈旧字节码会让"改了没效果"看起来像"修法无效"（本次实测踩到：编辑共享头只重编了部分使用者） |
| `Tools/gi/rtgi_coupling_check.ps1` | **RTGI 源独立性检查**（§9.2-I 的回归测试）：只改漫反射层栈跑两次，比较 RTGI 原始输出的**均值**是否一致。判据用均值而非逐字节——射线抖动种子取自帧计数器，而多一个 DDGI pass 会改变每帧的提交次数，逐像素必然不同；要保证不变的是**估计量本身**。修前是 0.0575 对 0.2034（3.5 倍），修后 0.000% |
| `Tools/gi/stack_switch_check.ps1` | **层栈与子系统开关一致性检查**（不变量 1 / §9.2-G 的回归测试）：把 SSR 放进镜面层栈（默认档位的镜面栈只有 IBL，故初始化时 SSR 开关是关的），带 `HE_TRACE_PASSES=1` 跑一帧，**要求 pass 列表里出现 `SSR`**。修前该列表里完全没有 SSR |
| `Tools/gi/rsm_gate_check.ps1` | **RSM 独立门控检查**（§9.2-F 的回归测试）：三例——关 DDGI 且 RSM 在漫反射层栈（**必须注册**，改前必失败）、关 DDGI 且层栈无 RSM（不得注册）、开 DDGI 且层栈无 RSM（不得注册，探针应回退 IBL）。第 2、3 例是防"改过头"的反向对照 |
| `Tools/gi/confidence_check.ps1` + `confidence_check.py` | **逐像素置信度检查**（§3.2 / 任务 9 的回归测试）：两例——最外圈屏幕空间源的贡献必须降到中央的很小比例（默认 5% 带宽）、把带宽调到 50% 后同一环带的贡献必须按比例下降（证明带宽来自 UBO 而非着色器常量）。**必须用同通道双源做差**：只有一个源时置信度在归一化里精确抵消，单源读数看不出任何变化。可加 `-Exe <改前的可执行文件>` 证明该检查在改前会失败 |
| `Tools/gi/ssgi_cal_check.ps1` + `ssgi_cal_check.py` | **SSGI 标定检查**（任务 10 / §9.2-P 的回归测试），三例：白炉解析真值（SSGI 单源必须读出 1.0）、双源做差必须等于两单源做差的加权平均（1.042）、DDGI 与 SSGI 量级同阶（2.2×，改前 21×）。**双源两例必须用 `dump_gi.ps1` 同一份设施产出**——同样写进配置的键在不同脚本下会落到不同兜底（§9.2-Y），不同基线做差就不再只差层栈。**白炉一例必须 `gi_solo=1`**（白炉条件含"关直接光"，否则探针读到直接光 + E/π） |
| `Tools/gi/amortize_check.ps1` + `amortize_check.py` | **DDGI 时间维分摊检查**（任务 12 的回归测试）：判据是 **"等量工作 ⇒ 等量结果"**——比较 `S(stride 4, frame 4k)` 与 `S(stride 1, frame k)`（k=30/60/120，实测比值 1.010/1.000/1.000），再确认同帧下摊销版**尚未**收敛到全量版（否则说明探针根本没被跳过）。每次运行使用**私有 cfg 副本**（示例程序退出会回写配置） |
| `Tools/gi/timing_check.ps1` + `timing_check.py` | **GI 耗时读数检查**（任务 29 / §9.2-Z 的回归测试）：读 `HE_GI_TIMING=1` 打出的 `[GI 耗时]` 行，三例——在跑的源必须有非零读数（且 > 0.05 ms）、不在层栈的源必须**恰好为 0**、把 SSGI 的采样数从 16 提到 64 读数必须明显变大（实测 ×2.92，判据 ≥1.5×），另记两次相同运行的离散度（实测 0.0%）。**这条检查的意义是"读数必须对工作量有响应"**：修好之前它恒为 0 |
| `Tools/gi/cost_report.ps1` | **GI 耗时成本表**（§3.5 的数字来源 / 任务 13 的判据）：跑四种配置（`{SSGI}`、`{RTGI}`、`{DDGI}`、`{DDGI,RTGI}+RT 反射+RT 阴影`）并打印各 GI 项与 `AS_Build` 的 GPU 耗时。读法：**只有真跑过的项才非零**（不在层栈的源没有 pass；IBL 只在脏时烘焙），所以 0 表示"没跑"而不是"没测" |
| `Tools/gi/ddgi_grid_check.ps1` + `ddgi_grid_check.py` | **DDGI 探针网格检查**（任务 14 / §9.2-K 与任务 17 的回归测试），四个配置 × 各自 `none` 基线：固定网格（`ddgi_grid_auto=0`，罩不住场景）贡献必须塌到噪声量级（**覆盖语义**）；三种拟合（8/16/32 ⇒ 192/1408/9408 探针）必须都恢复贡献，且**彼此显著不同（≥5%）**（**任务 17 的判据**：探针位置真的参与运算）。注意第三条判定在任务 17 **反转了方向** —— 任务 14 时它断言"三种分辨率一致"，因为当时探针场是均匀的（0.0015%），那正是被修的缺陷；现在断言"必须不同"（实测 37.93%）。配置里必须把 RSM 权重置 0，否则 DDGI 走 `useRSM=1` 路径，第三类判定不成立 |
| `Tools/gi/repeatability_check.ps1` + `repeatability_check.py` | **配置状态可复现性检查**（任务 28 / §9.2-Y 的回归测试）：同一份**逐字节等价**的配置连跑 N 次（默认 5），断言 ① 各次读数一致（< 0.5%）、② 配置状态键（`gi_blend_*` 与几个开关）逐键不变、③ 读数落在"空漫反射栈"量级而不是"被补 IBL"量级（9.2-Y 的两组分别是 0.05773 与 0.07554）。第二条直接对应 9.2-Y 的原始指纹（`gi_blend_diffuse_w0` 由 0 变 1）。比较的是**状态键**而不是整个文件：示例退出时按设计会重写配置并补上后加任务的新默认键 |
| `Tools/gi/ibl_lut_gate_check.ps1` + `ibl_lut_gate_check.py` | **IBL 烘焙门控检查**（任务 27 / §9.2-X 的回归测试），三例：三通道全空、只放 `ao={SSAO}`（两者都让**镜面栈为空**，也就是缺陷的触发条件）、`specular={IBL}`（对照，烘焙一向有触发）。判定：每例的 HDR `max ≤ 100`（缺陷时读 463.32，修后 42.20）、两例空镜面栈的读数一致、以及**结构判定**——`HE_TRACE_PASSES=1` 的 pass 列表里空镜面栈下 `IBL_Bake` 必须仍在（实测每例 121 行）。结构判定是关键：数字随场景/相机变，而"烘焙被注册"才是这条不变量的直接表述 |
| `Tools/gi/forward_stack_check.ps1` + `forward_stack_check.py` | **Forward 层栈归一化 + RSM 生产者检查**（任务 26 / §9.2-H 与任务 34 / §9.2-AD 的回归测试），五个 run：`pipeline_mode=0` 下漫反射层栈为 `{空}` / `{IBL}` / `{RSM}` / `{IBL,RSM}`，再加一个**只挪相机 300** 的 `{RSM}`。判定 ——① 三种层栈的 HDR 读数两两可区分（改前 cfg 键只写给 Deferred，`pipeline_mode=0` 完全忽略它们）；② 多开一个源**不变亮**（`{IBL,RSM}` < `{IBL}`）；③ 双源读数**等于**两单源的加权平均（实测相对误差 0.000%）；④ **`S_rsm` = mean({RSM}) − mean(空栈) > 1e-6**（任务 34 前恒为 **0**：这三条判据对"恒为 0 的源"全部成立）；⑤ `{RSM}` 的三张 RSM 图逐级非空（位置/编码法线/VPL 辐射度覆盖 > 20%、均值 > 0.005，与 Deferred 侧 `rsm_indirect_check` 同一套阈值）；⑥ **视角无关**：挪相机后三张 RSM 图**逐字节相同**（固定光锥不含相机）。**依赖任务 26 给 `HE_DUMP_GI` 加的"按当前管线取 HDR"**：此前它无条件转储 Deferred 的 HDR，Forward 的画面根本测不到 |
| `Tools/gi/lightmap_key_check.ps1` + `lightmap_key_check.py` | **光照图键检查**（原任务 31 的前置条件 ①；该任务**已取消**，此检查作为已落地基础设施的回归保留）：跑一帧 Deferred 并转储 `gb_lightmapkey`（第 8 个 GBuffer MRT）。**5 条断言** —— 键的覆盖面**等于**几何覆盖面（天空为零键）、页号是精确整数且在 `kGPUMaxObjects` 内、键是归一化参数化（uv 在 [0,1] 内 ≥99.9%，实测 **100%**）、键在 texel 足迹内唯一（128² 下含多个表面片的 texel ≤10%，实测 **7.33%**；256² 下 5.94%）。**报告**每页的屏幕连通块（遮挡，不是合批）与 256² 的唯一性数字。**这里记下了两次被判错的度量**：① 用"页 AABB 是否小于场景对角线 30%"判"一页是不是一个物体"（空心壳体必然不合格，于是误判成合批）；② 用"同 texel 出现两个 0.25 单位量化的世界位置"判碰撞（texel 本来覆盖一片面积，任何参数化都过不了；正确做法是与 texel 的世界足迹比离散度）。**顺带守住 §9.2-AG**：这条检查跑起来本身就要求 RHI 的 MRT 附件数不再是写死的 7（改前第 8 个附件会让驱动崩溃） |
| `slangc -reflection-json`（§9.2-AF 的排查手法） | **查 C++/Slang 共享结构体的真实布局**：`slangc <shader>.slang -target spirv -entry <entry> -stage <stage> -I Engine/Shader/Shaders -o <tmp>.spv -reflection-json <out>.json`，然后在 JSON 里按字段名找 `binding.offset`。**为什么要用它**：共享结构体是两端各自按自己的规则布局的，C++ 侧的 `sizeof`/`offsetof` 只能证明 C++ 自洽 —— 本次 `GIBlendParams` 加了 `float4x4` 之后 `rsmValid` 在着色器里恒读 0，就是靠它才看到 Slang 把它放在 368 而 C++ 写在 320（根因：std140 下**非 float4 数组**的元素步长是 16，`float[3]` 占 48 而非 12）。规则：共享结构体里的填充/数组一律用 `float4`（或 `float4x4`），并在 C++ 侧用 `static_assert(sizeof)` + `offsetof` 钉住 |
| `Tools/gi/ssr_check.ps1` + `ssr_check.py` | **SSR 检查**（任务 25 / §9.2-W 的回归测试），四例：`specular={SSR}` 走 Hi-Z、`specular={SSR}` 走线性 march（`ssr_use_hiz=0`）、`specular={IBL}`、`specular={IBL,SSR}`。三条判定——① Hi-Z 的有效像素（输出 alpha > 0）≥ 5%（改前恰为 0）；② Hi-Z 的命中最少是线性 march 的一半（任务 35 修好 Hi-Z 的透视校正之后实测 26.86% 对 46.31% ⇒ **0.58 倍**；改前是 1.39 倍，但那个数字是错误 march 的假命中抬起来的，见 §9.2-AE）；③ 把 SSR 加进已有 IBL 的镜面栈后 HDR 必须变化（改前逐像素相同，现在 +1.335%）。**它取的是 `prov4_spec_raw`**（dump 按 Provider 注册序命名：0=AO、1=IBL、2=RSM、3=SSGI、4=SSR、5=DDGI，之后是四个 RT 效果），脚本会把实际用的文件打印出来，注册序变了不会静默读错源。**注意这三条都是"内部一致性"判据**：反射的方向错、Hi-Z 漏掉一个反射，它们全都看不见（任务 32 用平面镜解析真值才发现）⇒ 位置类判据见下一条 |
| `Tools/gi/ssr_mirror_check.ps1` + `ssr_mirror_check.py` | **SSR 平面镜解析对照**（任务 32 / §9.2-W 的后续判据）：测试场景里放一块地面镜与两个已知立方体（`HE_SSR_MIRROR=1`），把物体中心按镜面**镜像**后再经**同一个**相机投影，得到反射**该出现**的像素。四个 run —— 线性 march 细步（`base`，断言在这里）、同样参数但相机平移 40（`jitter`，反射必须跟着几何走）、米制历史参数（`legacy`，负对照，必须几乎找不到反射）、默认 Hi-Z（`hiz`，只报告）。**先自检相机模型**：盒心投影到"最近的自身像素"必须 ≤ 8 px（实测 0.49 / 0.18 px），否则"反射错了"可能是参照错了。判定：反射落点 ≤ 6 px（实测 0.44 / 0.35 px）、远离预测的物体色像素 ≤ 5%（实测 0.00%）、抖动位移与解析位移差 ≤ 5 px（实测 2.39 px）、场景尺度 vs 米制的物体色像素数 ≥ 10×（实测 30676 vs 2432）。**断言两条路径都成立**（任务 35 起）：解析判据同时钉线性回退与**默认 Hi-Z** 路径 —— 两者都必须把红/绿反射放在预测像素 6 px 内、像素数不少于线性参照的一半、远像素 ≤5%（实测 Hi-Z 0.44 / 0.35 px、30137 / 15847 像素、0.00%）；另加**步数断言**（Hi-Z 的 `ssr_max_steps` 不超过线性的 60%，实测 256 对 600）与 **pass 耗时报告**（`HE_PASS_TIMING=1`，实测 4.256 ms 对 4.505 ms —— 任务 25 的"快 3.2 倍"已被更正，见 §9.2-AE）。**运行时序注意**：不要在 `cmake --build` 还在重新链接 exe 时并行启动本脚本 —— 会拿到"链接中的 exe"（实测 SSR 输出 14.4 万像素"有效"但颜色全为 0，jitter 判定假失败） |
| `Tools/gi/rsm_indirect_check.ps1` + `rsm_indirect_check.py` | **RSM 链路检查**（任务 16 建立、任务 30 扩到 7 条判定：`diffuse={RSM}` 与空漫反射栈两例，都带 `HE_PASS_TIMING=1`）——① 半分辨率 `RSM_Indirect` pass 在跑且耗时非零（任务 16 的全部要点）；② RSM 光栅化 pass 在；③~⑥ **任务 30 新增的逐级产出判定**：`rsm_pos` 覆盖率 > 20%、`rsm_nrm` 覆盖与之一致、`rsm_rad` 非零占比 > 20% 且均值 > 0.005、`rsm_indirect` 非零占比 > 20%（这四条在 9.2-AA 存续期间**全部必然失败**：三张附件里只有清除值）；⑦ `S_rsm > 1e-6`（源必须到达 HDR，不设上界——见下）。**为什么只设下界不设期望值**：本场景里 RSM 的 `E/π` 中位数为 0、只有约 17% 的接收点非零（光源在上方时 RSM 里可见的几乎都是同朝向的面，接收点与采样到的 VPL 大多共面 ⇒ 两个余弦同时趋零），量级是**估计量**的性质而不是单位错误，把某个期望值写成断言等于把取样图案的偶然性固化成判据。**第一条原本是耗时阈值，任务 17 期间被改成只报告**：本机的逐 pass GPU 耗时跨会话整体漂移（同一二进制连跑两次 `Lighting(rsm)` 读到 0.774 与 1.105 ms），任何绝对阈值都会判翻；耗时证据留在 §10.2 任务 16（改前 0.882 / 改后 0.513 ms，带 `GB_Clear` 作对照）。**镜面栈必须非空**（否则触发 §9.2-X 的 463 亮点，把要看的量淹掉） |
| `Tools/gi/vk_layer_settings.txt` | 校验层设置（配 `VK_LAYER_SETTINGS_PATH=Tools/gi`）：关闭重复消息上限，得到违规**真实次数**。**注意计数随该设置变化**——关掉去重后同一次运行为 `75/75/81`，不设该文件则为 `49/49/51`；两种都稳定，但**不可互相比较**（§11.3.1 方法论第 6 条） |

采样时有两个易踩的坑：

- **A/B 对照必须给每次运行一份私有的 cfg 副本。** 示例程序退出时会把 `HE_GILAB_CONFIG`
  指向的文件**回写**，复用同一个文件会让"后一次运行读到前一次运行写的配置"——于是配置差异
  被误读成代码差异（原任务 11.1 的第一次 A/B 就因此得到一个 −2% 的假差异）。**任务 28 起这条
  只是操作纪律、不再是语义不确定性**：`GIRegistry::Degrade` 改成只裁不加之后，"空层栈"在
  任何调用路径上都同义（同一份配置连跑 5 次读数离散 0.0003%，见 §10.2 任务 28 与
  `Tools/gi/repeatability_check.ps1`）。
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

##### 方法论收获（七条，都写进约定）

1. **GI 源「吃进去」的中间量必须和「吐出来」的一样纳入纹理级对照。** 本次是靠 dump
   `ibl_irr` 一步定性的——只看 DDGI 的输出只会看到「一个暗色常数」，看不出原因。
2. **未初始化纹理会把「配置错误」放大成「静默的物理错误」。** 修复已落地为 RHI 的
   「已写入」登记 + 一次性告警（§9.2-S）；它**只检测、不改写内容**——零初始化会把垃圾换成 0
   而继续静默，告警则直接指出是哪张图、哪个写入方漏了。
3. **绝对量级的变化要先用"通道比 / 与场景的相关性"做指纹判别**，再去猜机制。
   本次前几轮都在猜「时域回路」，而真正的线索是那个 1:1.5:4 的颜色比。
4. **解释一个读数差异之前，先证明"这个数字是本次跑出来的"。** 采样只在跑到目标帧才落盘，
   崩溃或超时的运行会留下**上一次**的旧文件，而分析照样出数字。本次因此白白追了一个
   "绝对读数依赖二进制布局"的假缺陷：四次"暗读数"全部来自崩溃运行（§9.2-U）。现在的采样
   脚本运行前删除产物、运行后校验转储新鲜度、失败以非零码退出。
5. **改着色器后要先确认 SPIR-V 真的重新生成了。** MSBuild 按时间戳判断是否重编，
   而 `Copy-Item` **保留源文件的 LastWriteTime**：用它回滚一个 `.slang` 会让源比生成的 `.spv` 旧，
   增量构建直接跳过编译，于是新代码根本没进二进制。本次即因此得到一次"改动前后完全一致"的
   假读数。**约定**：回滚或复制着色器后必须 `(Get-Item <src>).LastWriteTime = Get-Date`，
   并确认 `build` 输出里出现了该 shader 的编译行。
6. **校验层条数必须连同「是否关闭去重」一起记录，否则这个数字没有意义。** §11.3 的表格
   写着用 `vk_layer_settings.txt` 关闭 `duplicate_message_limit` 以得到"真实次数"，但**该文件
   此前并不在仓库里**，于是所有实际跑出来的计数都是在**开着去重**的条件下得到的。本次核账发现
   文档里记的 `51/51/54` 与全部历史日志不符（`Tools/gi/` 采样三变体一致为 **49/49/51**，而关闭
   去重后为 **75/75/81**）：两者都稳定可复现，差别只在去重上限截断了重复消息。
   **约定**：计数一律标注条件，只与**同条件**的改前基线比较；设置文件已入库
   （`Tools/gi/vk_layer_settings.txt`，配 `VK_LAYER_SETTINGS_PATH=Tools/gi` 使用）。
   **采样目标变化也会改变计数**（多落盘几张纹理就多几次拷贝与布局转换）：补上镜面/AO
   落盘后，三个变体在关去重下由 75/75/81 变为 **104/104/110**，开去重下由 49/49/51
   变为 **56/56/56**（去重把差值也截掉了）。所以「与改前一致」这条判据的正确做法是
   **同一份采样设施下跑两个二进制**：本次任务 7 即用改前/改后两个可执行文件（同一份
   示例代码）对照，得到 **104/104/110 对 104/104/110，逐类相同**。
7. **逐 pass 的 GPU 耗时不能当"通过/不通过"的判据，只能当同一次会话内的前后对照。**
   任务 17 期间踩到：同一份二进制连跑两次，`Lighting(rsm)` 读到 **0.774 与 1.105 ms**；
   而不受任何改动影响的 `GB_Clear` 在 **1.44 / 1.79 / 2.08 ms** 之间变化。**约定**：① 性能结论
   必须带一个**改动碰不到的对照 pass**（本次用 `GB_Clear`）；② 自动检查里不要写绝对阈值或
   相对阈值判定，能写"结构"就写结构（例如"那个专用 pass 存在且非零"），把数字留在文档里；
   ③ 引用数字时连同"同一次会话"一起引用（§3.5 的逐项成本即按此口径标注）。

> **给 S 的告警定基线**：它在 06.GILab 上曾固定报 4 条，已由任务 23 清零（§9.2-T）；
> **此后新增的任何一条都要当场查清** —— 基线为空，告警才真正具备信噪比。

> ⚠️ **曾被本文就地标注为"待重测"的数字现已重测**，§3.3 与 §9.2-M/N/O 的相关数值
> 已按修复后的数据更新；各处「不可引用」的标注已移除。


### 11.4 风险总表

| 风险 | 影响 | 对策 |
|---|---|---|
| 偶发崩溃根因未证 | 长跑 + 读回类验收被污染 | **已结项（任务 2）**：可复现的那条（单元测试退出期崩溃，Jolt 注册生命期）根因获证并修掉，回归检查 `crash_handler_check.ps1` 守住；历史偶发违例 76 次启动零复现，按测量结案。证据链现在是可信的（VEH + 分阶段报告 + minidump + 看门狗），任何新崩溃都能直接定位 |
| 合成改造后「画面变了」 | 回归难判断 | 单源配置逐像素截图对比 + 白炉数值判据（已可量化） |
| **C++/slang 双侧同步漏改** | 静默错值 | `static_assert(sizeof)` + 双侧常量同源；**结构体布局**另需 `offsetof` + `slangc -reflection-json` 复核 —— §9.2-AF：std140 下**非 float4 数组**的元素步长是 16，`float[3]` 在 Slang 里占 48 字节而 C++ 只占 12，数组之后的字段两端偏移错开、着色器静默读 0（共享结构体里的填充一律用 `float4`，`Pipeline/Material.h` 已把 `GIBlendParams` 的 `sizeof` 与三个 `offsetof` 钉死）。**往对象缓冲加字段也一样**：（已取消的）任务 31 曾给 `GPUObjectData` 加 `boundsMin/boundsMax`（176 → 208 字节），三个写对象缓冲的地方（`SceneRenderer`、`CSMTechnique`、`GI_RSM`）必须一起填，否则未初始化的字段会被其它消费者当数据读 |
| **白炉测试只验归一化、不验量纲** | 源的量纲错误被掩盖 | §9.2-A；**SSGI 已解决**（任务 10：白炉下不再短路它，白炉条件恰是它的解析真值条件，读数必须为 1.0；改回旧形式则为 0.444）；**RTGI 已解决**（任务 33：`RT_GI.rgen` 的 miss 在白炉下取 1、`DeferredLighting` 的白炉短路把它排除 ⇒ 读数恒 1.0000，负对照 0.0000）。**剩余未做**：IBL / DDGI / RSM 在白炉下仍走短路 —— 它们在白炉条件下的真值不是"估计式自然给出 1"，要逐个补的是**逐源真值校验**（记录在案，不是代码缺陷，也不是任务表里的项） |
| 频率分离的 `LowPass` 选型不当 | 噪声/振铃/跳变 | 已随 P5 退场失效（§3.3 判定不需要） |
| **消费者门控写漏 → 未初始化纹理被采样** | **静默的物理错误 + 读数跨构建不可复现**（本次 §9.2-Q/R 正是如此被放大的） | 已落地 RHI「已写入」登记 + 一次性告警（§9.2-S）：任何被采样却从未写入的纹理都会报出 set/binding/尺寸/格式；告警基线已由任务 23 清零（§9.2-T）。且 GI 源**吃进去**的中间量也要纳入纹理级对照（§11.3.1） |
| **帧图 pass lambda 读失效栈帧**（§9.2-U 的一个已修实例） | **把已销毁的纹理指针交给描述符更新**：实测每次运行约 186 条「是野指针」 | 已随任务 23 修掉（Lighting lambda 改按值捕获）；帧图其余 pass lambda 逐个审过，捕获列表干净 |
| **崩溃/超时的运行留下旧转储**（§9.2-U，已修） | **把上一次的数字当成本次读数**：本次即因此虚构出一个"绝对读数依赖二进制布局"的缺陷追查了很久 | 采样脚本改为运行前删除产物、运行后校验转储新鲜度、失败则以非零码退出（`Tools/gi/dump_gi.ps1`）；**任何读数差异先确认"这个数字是本次跑出来的"** |
| **转储检查与"重新链接 exe"并行**（任务 32 实测踩到） | **拿到链接中/半新半旧的可执行文件**：落盘的 SSR 有 14.4 万像素"有效"但颜色**全为 0**（射线命中采样到 albedo=0 的天空像素），jitter 判定假失败；险些被记成"相机平移就失效"的真实缺陷 | **先构建完、确认 exe 落地，再启动转储脚本**（本轮串行重跑后四个 run 逐位复现、7 条全过）。同一族的教训见上一行：异常读数先怀疑采样设施（构建时序、陈旧产物），再怀疑被测代码 |
| 校验层重复消息去重掩盖计数 | 误把「报告数」当真实次数（历史上两次误判） | 设置文件**已入库**：`Tools/gi/vk_layer_settings.txt` + `VK_LAYER_SETTINGS_PATH=Tools/gi` 关闭 `duplicate_message_limit`。**计数还对采样目标集敏感**，且任务 17 之后 DDGI 配置会多出 AS 相关的几条 ⇒ 当前基线以 §11.3 该行为准（不设设置文件时 `dump_gi` 四变体 60/71/60/71）。判据是「同一份采样设施下与改前二进制逐类相同」，或用 `HE_TRACE_FB` 交叉验证（§11.3.1 方法论第 6 条） |
| ~~P5 抽象/改造过度~~ | ~~大范围回归~~ | **已随 P5 退场失效**（§3.3 判定"不需要频率分离"，整波不再实施）。当年为它准备的对策（分步提交、保留 Additive 对照、同环境背靠背单源采样）已沉淀成通用做法，见 §11.3.1 |
| **帧图从「按通道」改为「按 Provider」执行**（原任务 19，**已迁至 Lumen 文档 §5.2**） | 核心路径回归 | 逐类迁移 + 每步判据：**先只合并 specular 与 diffuse 两条循环**（形状相同、且正是 Lumen 需要共享的一对），AO 因存在旁路暂不动，`Compute`(DDGI) 与 `Custom`(IBL) 暂留；判据是**同一环境下背靠背的单源采样逐项一致**（§11.3.1 已修复，绝对量级现在也可复现） |
| 文档与代码持续漂移 | 后续照文档实现出错 | 完成每个波次时同步回写本文 §2/§5 与状态表 |
| **Forward 修好 IBL 之后其它示例会变亮**（§9.2-AB） | 02.Cube / 03.Sponza-Forward / 07.AISamples / Editor 都用 `PBR.frag`，而这些示例此前也在"IBL 恒为 0"的状态下跑 | 这是**修复**而不是回归（它们的间接光本来就该有），但必须在那些示例上目视确认一遍：任务 26 只在 06.GILab 上做了数值判据。**状态：未做（记录在案的验证余额）** —— 本工作区只构建了 `06.GILab` 目标，其余示例未逐跑；若将来构建它们，按这里的说明确认"变亮是修复" |
| **Forward 的 RSM 改用固定光锥后，其它示例的 RSM 内容会变**（§9.2-AD / 任务 34） | 02.Cube / 03.Sponza-Forward / AISamples 也走 Forward 且都驱动阴影系统；它们的 RSM 此前用 CSM 级联 0 的 VP（随视角变），现在与 Deferred 同源、视角无关 | 这是**修复**（世界空间源的前提被恢复），且 06.GILab 上的数值判据已全过（`S_rsm` 非零、三图非空、换相机逐字节相同）；但那些示例的 RSM/阴影观感需目视确认一次（**状态：未做**，同上一行的原因）。另：06.GILab 的 Forward 模式现在**第一次有阴影**（此前阴影系统没被驱动），画面变暗是修复 |

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
| **P6** | **统一估计器（ReSTIR GI）** —— 任务已迁至《Lumen与Nanite完整设计规范》§5.2 | **⏳** | — |

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
| Wave 4 余项 | M5.2-A DDGI 光追 march（任务 17）· B3 RSM halfRes（任务 16）· D2 CPU 单测 | ✅ 完成 |
| **Wave 5** | **~~P6 统一估计器~~（任务已迁至《Lumen与Nanite完整设计规范》§5.2）· ~~Lightmap 源~~（任务 31 已取消）· D1 崩溃根因（任务 2）** | P6 ➡️ 已迁出；Lightmap ❌ 已取消；D1 ✅ 完成 |

### A.4 遗留任务批次

| 批次 | 内容 | 状态 |
|---|---|---|
| 第 1 批 | M4.4 · M4.5（不适用）· 面板候选派生 · 文档同步 | ✅ `42db644` |
| 第 2 批 | B4 DDGI 光追 march（任务 17）· B3 RSM halfRes（任务 16）· D2 CPU 单测 | ✅ 完成 |
| 第 3 批 | ~~Wave 3 频率分离~~ —— ❌ 已判定不需要（§3.3）；顺位由 `SSGI-CAL` 接棒 | ✅ 已判定 |
| 第 4 批 | Wave 5（~~P6~~ 已迁至 Lumen 文档 §5.2 / ~~Lightmap~~ 已取消 / D1 崩溃根因 ✅ 任务 2） | ✅ / ➡️ / ❌ |
