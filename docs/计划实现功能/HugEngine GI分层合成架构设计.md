# HugEngine GI 分层合成架构设计（研究）

> 目标：让同一场景中的多种 GI 算法**按频段/尺度分工协作**——
> 远场低频用探针（DDGI/Lightmap）、中频近处用屏幕空间（SSGI）、高频精确用光追（RTGI），
> 并以**物理正确的方式合成**（无双重计数）。

---

## 一、现状与局限

### 1.1 当前架构

| 组件 | 现状 |
|---|---|
| `GIMode`（GlobalIllumination.h） | None / IBL / SSGI / VXGI / DDGI / ReSTIR / RSM——**每技术一个枚举** |
| `GISettings` | mode + quality + intensity + halfRes——**单模式语义** |
| `IGlobalIllumination` | 一个实例 = 一个技术；`GetIndirectDiffuseTexture()` 单纹理输出 |
| 管线持有 | `m_GI`（IBL）+ **独立成员** `m_SSGI` / `m_SSR` / `m_DDGI` / `m_RSM`——**接口不统一** |
| 通道选择 | `DiffuseChannel`（None/SSGI/RTGI）——**单值枚举，选一个** |
| 多源 | 仅 `ddgiOverlay` 一个特例（DDGI 叠加） |
| 合成 | `DeferredLighting.frag` 内**硬编码相加**：`ssgi + ddgi*ddgiScale` |
| 可用性 | `GIRegistry::IsAvailable`（管线能力 ∧ 设备能力）+ `Degrade` 降级链 |

### 1.2 四个根本局限

1. **表达力不足**——"选一个"无法表达"DDGI 管低频 + SSGI 管中频 + RTGI 管高频"的分工
2. **合成错误**——多源相加是**双重计数**（同一物理量算两遍 → 能量翻倍，白炉测试失败）
3. **抽象不统一**——SSGI/DDGI/SSR 各自 Initialize/Render/GetOutput，新增 GI 需改管线代码 + shader + 面板
4. **无"层"概念**——没有频段（低频/中频/高频）、有效作用范围、置信度这些**决定权重的物理量**

---

## 二、设计目标

| 目标 | 说明 |
|---|---|
| **多源并存** | Diffuse 可同时启用 DDGI(低频) + SSGI(中频) + RTGI(高频) |
| **正确合成** | 权重归一化 / 频率分离——**权重和 ≤ 1**，白炉测试通过 |
| **物理权重** | 权重来自**置信度**（可见性 / 有效作用范围 / 采样质量），不是拍脑袋的距离线性插值 |
| **统一抽象** | 每个 GI 实现 `IGIProvider`——新增 GI 不改管线/shader 结构 |
| **渐进兼容** | 单源时行为与现有**完全一致**；档位语义不变（只改"谁上场 + 多精细"） |
| **同构扩展** | Diffuse / Specular / AO 三个通道共用同一套层栈模型 |

---

## 三、架构设计

### 3.1 数据模型：从"单枚举"到"层栈"

> **⚠️ 本节已于 2026-09-14 按实现回写**（原设计稿与实现存在漂移：`GILayerDesc`→`GISourceDesc`、
> `GIBand` 不进层描述、`range`→`falloffDistance`、`enabled`→`weight>0`、
> `kMaxLayers`→`kMaxSources`、`blend`→`mode`、`GIBlendMode::Fallback` 未实现）。
> 以下为**当前实现**（`Engine/Render/GI/GIConfig.h`）。

```cpp
// ---- GI 源标识（与具体实现解耦；共 11 项，含 1 项预留）----
// 频段不写进层描述，而是由 id 推导（GIBandOf）——避免"同一算法被填成不同频段"的不一致
enum class GISourceId : u8 {
    None = 0,
    // 低频（远场 / 环境）
    IBL = 1,        // 环境辐照度 / 预滤波（同时服务 diffuse 与 specular）
    Lightmap = 2,   // 烘焙光照（预留，尚未实现）
    DDGI = 3,       // 动态漫反射探针网格
    // 中频（近处细节）
    SSGI = 4,       // 屏幕空间间接漫反射
    SSR = 5,        // 屏幕空间反射
    SSAO = 6,       // 屏幕空间环境光遮蔽
    RSM = 7,        // 反射阴影贴图间接光
    GTAO = 11,      // 地平线切片 AO（SSAO 的高质量替代）
    // 高频（精确光追）
    RTGI = 8,       // 硬件光追间接漫反射
    RTReflection = 9,   // 硬件光追反射
    RTAO = 10,      // 硬件光追环境光遮蔽
};
// 注：ReSTIR 未列入枚举——它属于 P6 统一估计器（Wave 5），届时再作为源接入。
// 注：阴影不进 GISourceId——它是「可见性（乘法项）」而非能量（加法项），
//     用独立 ShadowChannel 枚举表达（见 §3.x 与不变量 5）。

// ---- 频段（决定该源在合成中的角色；由 id 推导，不存储）----
enum class GIBand : u8 { Low, Mid, High };
GIBand GIBandOf(GISourceId id);   // IBL/Lightmap/DDGI→Low；RT*→High；其余→Mid

// ---- 单个 GI 源描述 ----
struct GISourceDesc {
    GISourceId id              = GISourceId::None;
    float      weight          = 1.0f;   // 相对权重（0 = 不参与，等价于旧设计的 enabled）
    float      falloffDistance = 0.0f;   // 可选「距离让位」（0 = 不启用）
};
// 注：原设计的 `range`（有效作用范围）更名并改为可选的 `falloffDistance`——
//     因为物理正确性来自权重归一化，距离衰减只是性能/艺术控制，默认关闭。

// ---- 一个通道的源层栈（有序：低频 → 高频）----
struct GIChannelStack {
    static constexpr u32 kMaxSources = 4;
    GISourceDesc sources[kMaxSources];
    u32          count = 0;
    GIBlendMode  mode  = GIBlendMode::Normalized;   // 通道级合成模式
};

// ---- 合成模式（当前实现）----
enum class GIBlendMode : u8 {
    Additive,    // 直接相加（旧行为——双重计数，仅作 A/B 对照）
    Normalized,  // 归一化加权 Σ(源×w)/Σw（推荐，权重和=1 → 无双重计数）
};
// 注：原设计的 Fallback（分层回退）与 FrequencySplit（频率分离）尚未实现——
//     前者可由"只用最精确的源"的层栈组合表达；后者是 P5 / Wave 3 的目标。

// ---- 扩展后的 GIConfig ----
struct GIConfig {
    GIChannelStack diffuse;    // 间接漫反射层栈
    GIChannelStack specular;   // 间接镜面层栈
    GIChannelStack ao;         // 环境光遮蔽层栈
    ShadowChannel  shadow = ShadowChannel::Raster;   // 阴影：独立枚举，不进层栈
    float giIntensity = 1.0f;
    float aoIntensity = 1.0f;
    bool  halfRes = false;
};
```

**兼容性**：单源时 `diffuse.count == 1` —— 合成退化为"直接取用"，与现有行为一致。

**已实现的配套机制**（原设计未展开、实现时补充）：
1. **按源 id 分派采样**：合成的 UBO 用**源数组**（`GISourceSlot{id, weight, falloffDistance} × 4`
   + `count` + `mode`），shader 遍历数组并调 `SampleDiffuse/SpecularSource(id)` ——
   新增算法只需加 `GISOURCE_XXX` 常量 + 一个 `case`，**不改 UBO 结构/合成循环**
2. **Provider 注册表**：每个源由 `IGIProvider` 实现自报身份/门控/输出/附属 pass，
   帧图的 pass 与面板候选均由注册表生成（P4 已落地）
3. **逐源降级**：`GIRegistry::Degrade(stack, pipelineCaps, rtSupported)` 按
   「管线能力 × 设备能力」逐源裁剪，并保证通道不空

> 注：原设计提到的「`DiffuseChannel` 枚举作为便捷构造器保留」**未采用**——
> P4 完成后层栈成为唯一表达方式，这些枚举已无使用者并已删除（见不变量 4）。

### 3.2 权重来源（关键——不是"相机距离"）

每个源的权重 = **置信度 × 范围衰减 × 用户权重**：

| 源 | 置信度依据 | 范围（默认） |
|---|---|---|
| **IBL / Lightmap** | 恒定 1.0（全方向、无限范围） | ∞ |
| **DDGI** | 探针可见性（chevron / relocation）、**与最近探针的距离 / cellSize** | ∞（低频兜底） |
| **SSGI** | **屏幕内可见性**（屏幕边缘/遮挡 → 0）、march 有效距离 | 5-10 m |
| **RSM** | 光源视锥覆盖、VPL 密度 | 10-20 m |
| **RTGI** | **SPP / 时域收敛度**、射线最大距离 | 20-50 m |

```hlsl
// 权重 = 置信度 × 范围衰减 × 用户权重
float w = confidence * saturate(1.0 - dist / range) * userWeight;
```

**注意**：`dist` 是**相机到着色点的距离**（近似"该源能否覆盖这个尺度"），
但真正的判据是 **confidence**（可见性）——距离只是让位机制的近似。

### 3.3 合成模型（shader）

```hlsl
// 归一化加权（推荐默认）
float3 num = 0; float den = 0;
[unroll] for (uint i = 0; i < stack.count; ++i) {
    float3 c   = SampleSource(stack.layers[i].id, worldPos, N);   // 各源输出
    float  w   = ComputeWeight(stack.layers[i]);                   // 3.2 节公式
    num += c * w;  den += w;
}
float3 gi = num / max(den, 1e-4);     // ← 归一化：无双重计数
```

```hlsl
// 频率分离（进阶）：DDGI 提供低频基底，SSGI/RTGI 提供高频细节
float3 base   = SampleDDGI(...);                     // 低频
float3 detail = SampleSSGI(...) - LowPass(SSGI);     // 去低频后的细节
float3 gi     = base + detail;                       // 频段不重叠，数学合法
```

### 3.4 统一抽象：`IGIProvider`

把 SSGI / DDGI / SSR / RTGI 从"管线独立成员"重构为统一接口：

```cpp
class IGIProvider {
public:
    virtual ~IGIProvider() = default;

    [[nodiscard]] virtual GISourceId GetSourceId() const = 0;
    [[nodiscard]] virtual GIBand     GetBand()     const = 0;   // 天然频段
    [[nodiscard]] virtual float      GetRange()    const = 0;   // 有效作用范围

    /// 本帧是否产出有效数据（如 SSGI 全屏外 → false；RTGI 未收敛 → 低置信度）
    [[nodiscard]] virtual bool  IsValid() const = 0;

    /// 通道输出（按需覆写；无输出的通道返回 nullptr）
    [[nodiscard]] virtual rhi::IRHITexture* GetDiffuseOutput()  const { return nullptr; }
    [[nodiscard]] virtual rhi::IRHITexture* GetSpecularOutput() const { return nullptr; }
    [[nodiscard]] virtual rhi::IRHITexture* GetAOOutput()       const { return nullptr; }

    /// 生命周期 / 帧内执行（沿用 IRenderSubsystem 风格）
    virtual bool Initialize(rhi::IRHIDevice*, u32 w, u32 h) = 0;
    virtual void Render(rhi::IRHICommandList*) = 0;
};
```

**收益**：新增一种 GI（如 VXGI / Lightmap）只需实现 `IGIProvider` + 在 `GIRegistry` 注册，
**管线帧图 / Lighting shader / 面板 均无需改动**。

### 3.5 档位映射（与现有 4 档共存）

档位决定"**候选集合 + 各层精度**"，合成数学（B 方案）**不随档位变化**：

| 档位 | Diffuse 层栈 | 各层精度 |
|---|---|---|
| **Low** | `{DDGI(Low)}`（或 IBL） | DDGI 8×4×8 / halfRes |
| **Medium** | `{DDGI(Low), SSGI(Mid)}` | SSGI 16 采样 / halfRes |
| **High** | `{DDGI(Low), SSGI(Mid)}` | SSGI 32 采样 / fullRes |
| **Ultra** | `{DDGI(Low), RTGI(High)}` | RTGI 高 SPP + 时域累积 |

**切档 → 只改层栈内容与精度 → 合成公式不变 → 无亮度跳变**。

### 3.6 可用性/降级（沿用并扩展）

```
档位 → GIConfigFromPreset（层栈 + 精度）
     → GIRegistry::Degrade(stacks, pipelineCaps, rtSupported)
         · 逐层裁剪：管线不提供的源移除（Forward 无 SSGI/DDGI）
         · RT 系列在无光追设备上替换为同频段替代（RTGI→SSGI，RT 反射→SSR）
         · 层栈清空 → 退回 IBL（保证至少有一个兜底）
     → push constant（各层 useXXX + range + weight + blendMode）
     → shader 合成
```

---

## 四、关键问题与对策

| 问题 | 对策 |
|---|---|
| **双重计数** | 权重**归一化**（`Σw=1`）；或频率分离（频段不重叠） |
| **权重依据** | 用**置信度**（可见性/收敛度），距离仅作范围让位 |
| **时序稳定** | 权重做时域平滑（避免权重突变导致闪烁）；DDGI 时间累积与 SSGI 噪声需分别处理 |
| **性能** | 多源并存 → 层栈可被档位裁剪；低频源（DDGI/IBL）便宜，高频源（RTGI）按需；半分辨率 + 时域累积 |
| **过渡突变** | 权重连续（`saturate` 衰减）+ 时域平滑；层启用/禁用用淡入淡出 |
| **白炉测试** | 必须通过（全白环境 + 反射率 1 → 球消失）；作为 CI 校验项 |
| **调试** | 面板可单层显示（每层独立可视化）+ 合成前后对比 |

---

## 五、演进路线（渐进、每步可回退）

> **实施状态（2026-09-14 更新）**：**P1–P4 已全部落地**，并额外完成了 S1（光追归入 GI 源）、
> S2（管线维度收敛）、S3（移除 HybridRTPipeline）、PT 定位与加速结构共享、
> S1.5（两类源可同时参与）、M6.3（GTAO）。**余下 P5 频率分离与 P6 统一估计器。**

| Phase | 内容 | 风险 | 状态 |
|---|---|---|---|
| **P1 · 数据模型** | `GIConfig` 引入层栈 + `GIBand` / `weight` / `falloffDistance` / `blendMode` | 低 | ✅ `105911b` |
| **P2 · 合成改造** | `DeferredLighting.frag` 合成改**归一化**（保留 Additive 分支作对照）；参数经 UBO 传递 | 低 | ✅ `105911b` |
| **P3 · 源层栈** | 4 个单值枚举 → `GIChannelStack`；帧图门控与降级全部逐源；06 源列表 UI | 中 | ✅ `105911b` |
| **S1 · 光追归入 Deferred** | 光追作为 GI 源（RTGI/RT 反射/RTAO/RT 阴影）接入 Deferred，层栈条件门控 | 中 | ✅ `90649ba` `aac5690` |
| **S2 · 管线维度收敛** | 管线下拉收敛为 Forward / Deferred；HybridRT 不再是管线 | 低 | ✅ `06c8580` |
| **S3 · 移除 HybridRT 管线** | 删除 `HybridRTPipeline`（-1245 行），02.Cube 迁移 | 中 | ✅ `0f8aca8` |
| **PT · 参考渲染器定位** | 明确 PT 为 ground truth + 与 Deferred 共享加速结构 | 低 | ✅ `3301040` |
| **S1.5 · 两类源同时参与** | SSGI+RTGI（specular/AO 同理）由二选一改为各自独立采样、归一化合成 | 低 | ✅ `5c2b84b` |
| **P4 · Provider 抽象** | **全部 10 个 GI 源接入注册表**；帧图 pass 与面板候选由注册表生成 | 中 | ✅ `3a4075b` `8fc6091` `2b6b51a` `fd2510d` `48984e9` `51b8c98` `ea4620d` |
| **Wave 0 缺口修复** | IBL/RSM 归位归一化（白炉 1.7998→**1.0000**）、3 槽位数组化、M5.3 DDGI SH 卷积、ddgiScale 收敛 | 中 | ✅ `2211e4c` `e862931` |
| **M6.3 · GTAO** | 地平线切片 AO，作为 AO 通道独立源（同时验证「新增源零侵入」） | 低 | ✅ `c487109` |
| **P5 · 频率分离** | DDGI 低频 + SSGI/RTGI 高频细节（去低频叠加） | 高 | ⏳ 待做 |
| **P6 · 统一估计器** | 长期：ReSTIR GI 把各源统一为「重采样 + 回退」框架 | 高 | ⏳ 待做 |

**P4 已兑现的承诺**（新增一种 GI 源的实际改动）：
实现 Provider（算法本体）+ **注册 1 行**；**帧图 / UBO / 合成循环 / 层栈结构 0 行**；
AO 通道的面板候选已由注册表自动派生。该承诺在 **GTAO**（先于 P4 落地，需改帧图 1 行 +
面板 1 行）与随后的 **10 源 Provider 化**中均获验证。

**下一步优先级建议**：① **P5 频率分离**（前提已全部就绪：Wave 0 白炉判据 + Wave 1 按源合成 +
Wave 2 Provider 注册表） ② Wave 4 遗留小项（M4.4 RSM VPL 降采样 / M4.5 GBuffer 合并 /
M5.2-A DDGI 光追 march） ③ P6。

---

## 五之二、已落地的终态架构（2026-09-14 更新）

```
渲染管线（架构差异，不可合并）            GI 源（效果差异，自由组合 + 权重）
├─ ForwardPipeline                        低频： IBL · Lightmap(预留) · DDGI
├─ DeferredPipeline                       中频： SSGI · SSR · SSAO · GTAO · RSM
│   └─ 内含全部 10 个 GI Provider          高频： RTGI · RT 反射 · RTAO
└─ PathTracingPipeline（参考渲染器）       （可见性：光栅阴影 / RT 阴影 —— 独立枚举）
         ↓                                          ↓
    GBuffer / Lighting / 后处理            归一化加权合成（权重和 = 1，物理正确）
```

**每帧 pass 由 Provider 注册表生成**（帧图不再手写任何源的 pass 与门控）：

| Provider | 源 | pass 链 |
|---|---|---|
| `IBLProvider` | IBL | `IBL_Bake`（脏时重建） |
| `DDGIProvider` | DDGI | `DDGI`（compute） |
| `ScreenAOProvider` | SSAO / GTAO | `AO`（同 pass 两种模式） |
| `SSGIProvider` | SSGI | `SSGI` → `SSGI_Denoise` |
| `SSRProvider` | SSR | `SSR` → `SSR_Denoise` |
| `RSMProvider` | RSM | `RSM` |
| `RTEffectProvider`×4 | RT 阴影 / RTAO / RT 反射 / RTGI | 主 pass → 时域（→ 空间滤波），共享 `AS_Build` |

**关键不变量（已实现并需长期保持）**：
1. 通道的「层栈」与「子系统开关」**同源**——层栈说参与，子系统就必须真的启用
   （曾因两者不一致导致画面发黑；后又在 06 被旧 cfg 开关覆盖，由 Provider 诊断暴露并修复）
2. **权重归一化**是物理正确性的来源；距离让位只是性能/艺术控制，默认关闭
3. **多开一个源不会变亮**（归一化保证）；**单源时行为与二选一时代完全一致**
4. **光追是「GI 源」而非「管线类型」**——管线能力位（架构）× 设备能力（`rtSupported`）
   两层判断，Deferred 的位已包含全部光追源
5. **阴影是「可见性（乘法项）」而非「能量（加法项）」**——不进层栈，用独立 `ShadowChannel`

---

## 六、与现有系统的共存关系

| 现有机制 | 扩展后 |
|---|---|
| `GIConfig`（4 通道 + ddgiOverlay） | 层栈化；`ddgiOverlay` 语义升级为"DDGI 作为一层参与合成" |
| `DiffuseChannel`/`SpecularChannel`/`AOChannel` | **保留**（便捷构造单层栈），不破坏现有代码/配置 |
| `GIQualityPreset` 4 档 | **保留**——改的是"层栈内容 + 精度"，不改档位体系 |
| `PipelineCaps` / `GIRegistry::Degrade` | **扩展**为逐层裁剪（现有可用性判断逻辑复用） |
| `IGlobalIllumination` | **并存**（IBL 等继续用它）；新增 `IGIProvider` 服务于多源层栈 |
| `06.GILab` 面板 | GI 控制台 → Diffuse 通道内加"层列表"（每层：源/频段/范围/权重/开关）+ 合成模式下拉 |
| `06_GILab.cfg` | 层栈序列化（源 id / band / range / weight / enabled） |

---

## 七、结论

1. **需要扩展**——现有"单枚举 + 相加"模型无法表达分层分工，且相加在物理上是错的
2. **扩展的核心**是把 **"选一个 GI"** 变成 **"一组带频段/范围/权重的 GI 层 + 归一化合成"**
3. **不需要推翻现有体系**——4 档档位、PipelineCaps 降级、通道枚举都可复用/升级
4. **落地优先级**：P1（数据）+ P2（合成归一化）→ 先修正现有过亮问题 → 再逐步多源化
5. **验证基线**：白炉测试（能量守恒）+ 06.GILab 分档对比
