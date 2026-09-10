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

```cpp
// ---- GI 源标识（与具体实现解耦）----
enum class GISourceId : u8 {
    None = 0,
    IBL,        // 环境辐照度（远场、全方向、低频）
    Lightmap,   // 烘焙（远场、静态、低频）
    DDGI,       // 探针网格（远场/中距、低频）
    SSGI,       // 屏幕空间（近处、中频）
    RSM,        // 反射阴影贴图（中距、中频）
    RTGI,       // 硬件光追（近处、高频）
    ReSTIR,     // 时空重采样（近处、高频，RTGI 的进阶）
};

// ---- 频段（决定该源在合成中的角色）----
enum class GIBand : u8 { Low, Mid, High };

// ---- 单个 GI 层描述 ----
struct GILayerDesc {
    GISourceId id      = GISourceId::None;
    GIBand     band    = GIBand::Low;    // 该源天然的频段
    float      range   = 30.0f;          // 有效作用范围（米；超出 → 权重衰减让位）
    float      weight  = 1.0f;           // 用户/档位给的相对权重（0..1）
    bool       enabled = false;
};

// ---- 一个通道的层栈（有序：低频 → 高频）----
struct GIChannelStack {
    static constexpr u32 kMaxLayers = 4;
    GILayerDesc layers[kMaxLayers];
    u32         count  = 0;
    GIBlendMode blend  = GIBlendMode::Normalized;
};

// ---- 合成模式 ----
enum class GIBlendMode : u8 {
    Fallback,     // 分层回退：RTGI → SSGI → DDGI/IBL（最简，绝对无双重计数）
    Normalized,   // 归一化加权（推荐；权重来自置信度）
    FrequencySplit, // 频率分离（低频+高频细节；进阶）
};

// ---- 扩展后的 GIConfig ----
struct GIConfig {
    GIChannelStack diffuse;    // 间接漫反射层栈
    GIChannelStack specular;   // 间接镜面层栈
    GIChannelStack ao;         // 环境光遮蔽层栈
    ShadowChannel  shadow = ShadowChannel::Raster;
    float giIntensity = 1.0f;
    float aoIntensity = 1.0f;
    bool  halfRes = false;
};
```

**兼容性**：单源时 `diffuse.count == 1` —— 合成退化为"直接取用"，与现有行为一致。
现有的 `DiffuseChannel` 枚举可作为**便捷构造器**保留（`DiffuseChannel::SSGI` → 生成单层栈）。

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

| Phase | 内容 | 风险 |
|---|---|---|
| **P1 · 数据模型** | `GIConfig` 引入 `GIChannelStack`（层栈）+ `GIBand` / `range` / `weight` / `blendMode`；保留 `DiffuseChannel` 便捷构造 | 低（纯数据） |
| **P2 · 合成改造** | `DeferredLighting.frag` 合成改**归一化**（保留 Additive 分支作对照）；push constant 传层参数 | 低（单源等价） |
| **P3 · 多源启用** | Medium/High 档启用 `{DDGI, SSGI}` 分层；面板可调每层 range/weight | 中（画面变化，需对比） |
| **P4 · Provider 抽象** | 重构 SSGI/DDGI/SSR/RTGI 为 `IGIProvider`；帧图按注册表遍历构建 pass | 中（结构性重构） |
| **P5 · 频率分离** | DDGI 低频 + SSGI/RTGI 高频细节（去低频叠加）；Specular 同理（IBL prefilter 低频 + SSR/RT 高频） | 高（需低频分解） |
| **P6 · 统一估计器** | 长期：ReSTIR GI 把各源统一为"重采样 + 回退"框架（探针作为 miss 回退） | 高 |

**建议从 P1 + P2 开始**——低风险、可对照、立即修正"多源相加过亮"的现有问题。

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
