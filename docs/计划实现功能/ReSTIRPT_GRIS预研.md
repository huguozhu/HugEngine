# ReSTIR PT / GRIS 预研（§12 B7 的落点方案与代价评估）

> **本文件的边界（2026-09-19）**：只做 **设计细化 + 代价评估**，供"是否把 PT 转为实时主路径"这个决策用。
> 它**不改变** `全路径追踪管线规划.md` §0.1 的定位（PT = 参考渲染器），**不动** §10/§11，
> 也**不改变** §12 B 组的触发条件（B7 状态仍是"未做，条件未到"）。本文件不含任何代码改动。
>
> 所有数字都标了来源（代码位置 / 实测命令 / 日志），可复现；推算部分明确写"推算"。

---

## 1. 结论摘要

| 问题 | 结论 |
|---|---|
| 技术上能不能做？ | **能**，且缺口比预期小：PT 的随机数是**无状态确定性**的（`Rand(idx, frame, s)`），随机重放式 shift 只需存 `(源像素, 源帧)` 两个 uint，不需要逐顶点 RNG 状态 |
| 主要工作量在哪？ | 引擎**完全没有 shift / 雅可比 / 重连机件**（全库检索 0 命中）。这不是"多算几个 bounce"，而是**新增一套路径复用的数学与数据结构** |
| 主要门槛是什么？ | **显存**（完整路径蓄水池在 1080p 推算 1.2~2.8 GB，本机 8 GB 笔记本 GPU 上不可行；GRIS-lite ≈ 0.12~0.5 GB）+ **shift 写错即引入偏差** |
| 时间增量 | 复用 pass 本身很便宜（实测 ReSTIR DI 三个 compute 只 +0.35~0.60 ms @960×540）；成本几乎全在光线（1 个额外路径样本 ≈ 2.1 ms @b=1 / 9.4 ms @b=4） |
| 在"参考渲染器"定位下值得做吗？ | **现在不值得**。同样质量可用多 SPP 换（本文件给了换算表），而"标准答案"最怕的是**偏差**：写错 shift 的 GRIS 比慢的 PT 更糟 |
| 触发后怎么做？ | 三步：**M1 = ReSTIR GI（1 顶点重连）** → **M2 = 完整 GRIS（随机重放 + 时域/空间复用）** → **M3 = 与 DI/P6 统一**；无偏性判据用**现有迭代式 PT 当 oracle** 对照（本项目独有的便利） |
| 必须先解决的前置问题 | ReSTIR GI 的落点已归到 `Lumen…` P6（§12 C16）。**先定落点**（PT 侧 vs P6），否则会做出两套估计器 |

---

## 2. 现状盘点：B7 能复用什么、缺什么

| 项 | 现状 | 出处 |
|---|---|---|
| ReSTIR DI | ✅ 三个 compute（Init/Temporal/Spatial）单 RG Pass 顺序执行；蓄水池双缓冲 + 历史 depth/normal 双缓冲 | `Engine/Render/RT/ReSTIRPass.{h,cpp}`、`PathTracingPipeline.cpp` 的 `ReSTIR_DI` pass |
| 蓄水池结构 | `PTReservoir` **32 B**：`lightIndex / weightSum / M / W / lightPos(float4)` —— 只存**一个光源样本** | `ShaderTypes.slang` 的 `GPU_STRUCT PTReservoir` |
| 目标函数 | 复用 PT 第 5 UAV 的真实 albedo/metallic，`PBR_BRDF(albedo, metallic, roughness, N, V, L)·Li` | `ReSTIR_Init.comp.slang` |
| PT 路径结构 | **单 RayGen 迭代循环**（NEE + MIS + 轮盘赌 + 天空），命中信息经 112B `PathPayload` 回传；**没有逐 bounce 的 shader 分离** | `PT_Full.rgen.slang` / `PT_Full.rchit.slang` / `RT/PathPayload.h` |
| 随机数 | **无状态确定性**：`Rand(idx, frame, s)` / `RandInt(...)` / `StratifiedJitter(sampleIdx, sampleCount, frame)`，全部由 `(像素, 帧, 维度槽 s)` 决定 | `PT_Common.slang` L43~76 |
| 材质/贴图数据 | 材质纹理 **11 行**（含 `materialID`/`textureMask`/因子）、三角形法线 + UV 纹理、`PBR_BRDF` 求值端共用 | `RTPass::BuildSceneMaterialTexture`、§0.6 缺陷 3 |
| 对照/验证设施 | `HE_DUMP_PT` 落盘、`Tools/pt/dump_pt.ps1`、`analyze_pt.py`（`--diff/--compare/--converge`）、`HE_DUMP_MODE=deferred` | §12 任务 5/6 完成记录 |
| **shift / 雅可比 / 重连** | ❌ **0 命中**：`Jacobian` / `shiftMapping` / `ShiftMapping` / `reconnection` / `Reconnection` / `GRIS` / `PathReservoir` 全库（排除 `External/`）均 0 | 见附录 A 的检索命令 |
| **路径蓄水池** | ❌ 不存在（现有蓄水池只够 DI：光源索引 + 位置 + W） | 同上 |
| **重放/重连的 RayGen 入口** | ❌ 不存在（PT 只有 `FullPT` 一条 RT 管线） | `PTPass`/`RTPass::CreateEffectPipeline` |

**一句话**：DI 那套"蓄水池 + 时域/空间复用 + 帧图接线"可以照搬；缺的是 **shift 映射（含雅可比与可见性校验）**、**路径数据的存储与压缩**、**一个能"按给定路径重放/重连"的 RayGen**。

---

## 3. 目标形态（一个具体设计）

### 3.1 两种候选形态

| | 形态 A：GRIS-lite（= ReSTIR GI 的一般化） | 形态 B：完整 GRIS（路径重采样） |
|---|---|---|
| 复用什么 | 把 bounce0 的**间接光**换成一个"首个间接顶点"样本：`(x1 位置, x0→x1 的吞吐, x1 处的 NEE 辐射度)`，复用时要**重连**（重算 x0→y1 边 + 可见性） | 复用**整条后缀路径** `[x1..xR]`；首边重连 + 更深的段用**随机重放**（同随机数重放）或存路径顶点 |
| 需要的 shift | 重连 shift（闭式雅可比） | 重连 + 重放 shift（重放需 `(源像素, 源帧)`） |
| 显存/像素 | ≈ 80 B × 3 槽（推算） | ≈ 200~450 B × 3 槽（推算，见 4.3） |
| 能治的病 | 间接漫反射噪声（含大光源/环境光） | 焦散、多次弹射的间接光、难采样路径 |
| 实现风险 | 中（= 现有 DI 的 1 次泛化） | 高（路径存储 + 两种 shift + 雅可比 + 可见性 + MIS 权重） |
| **建议** | **M1 做这个** | M2 再做 |

> 形态 A 与 `全路径追踪管线规划.md` §6.1「ReSTIR GI」是同一件事。**先做 A 再谈 B**：A 能把 shift 机件、数据布局、无偏性判据全部跑通，且是 B 的子集。

### 3.2 数据结构（形态 A，逐字段）

```
// 每像素一份（累积到 FinalReservoir，供下帧 PT 使用）
struct PTIndirectReservoir {   // 推算 80 B（可压到 48 B：位置/方向用 fp16、W 用 fp32）
    uint   valid;              // 0 = 无效（天空/无命中/被拒）
    uint   M;                  // 累计候选数
    float  W;                  // 选中样本的（重连后）目标函数值
    uint2  srcPixel;           // 样本来源像素（随机重放 shift 用）
    uint   srcFrame;           // 样本来源帧（随机重放 shift 用）
    uint   rngSlot;            // 该样本对应的维度槽编号（重放的维度对齐）
    float3 posX1;              // 首个间接顶点位置（重连用）
    float3 throughput;         // x0→x1 的吞吐（含 BSDF/pdf/几何项，按来源像素的 BSDF 算好？否——见下）
    float  pdfX1;              // 采样 pdf（重连需换算）
    float3 radiance;           // x1 处的入射辐射度（NEE 或 1 条 shadow ray 的结果）
    float3 normalX1;           // x1 法线（重连的几何项 + 目标函数判据）
};
```

**关键实现要点（决定 reservoir 能不能只存这些）**：

1. **吞吐不能预先乘来源像素的 BSDF**：shift 后的一阶边属于*当前*像素，`f(y0)·G(y0,x1)/pdf_shift` 必须在**目标像素**重算。因此 reservoir 只存"材质量（`normalX1`、`posX1`、`radiance`）"，与"来源像素的 BSDF 无关"。
2. **重放式的随机数**：因为 `Rand` 是 `(idx, frame, s)` 的纯函数，深段重放只需 `(srcPixel, srcFrame, rngSlot)`；`rngSlot` 用现有维度槽编号约定即可，**无需逐顶点存 RNG**（这是本引擎独有的便利，见 §2）。
3. **重连的可见性**：`x0→x1` 一条 shadow ray；失败即丢弃该候选（GRIS 的标准做法）。
4. **雅可比**：重连 shift 的 `|∂T/∂x|` 用闭式解（GRIS 论文给出；实现时写成 `Tools/check_*` 式的可单测函数，配 doctest）。

### 3.3 Pass 划分与帧图插入点

现有链（`PathTracingPipeline::BuildFrameGraph`）：
```
AS_Build → PT_Render(PT_Full: RayGen 迭代) → [ParticleRender] → ReSTIR_DI → PT_Denoise → PT_Atrous → ToneMap → FXAA
```
B7 之后的链（形态 A）：
```
AS_Build → PT_Indirect_Init（新，1 条间接光线/像素 → 首顶点样本 → PTIndirectReservoir）
        → PT_Render（bounce0 的间接光改为读 FinalReservoir；直接光仍走 NEE）
        → ReSTIR_DI（不变，负责直接光）
        → PT_Indirect_Temporal（新：速度重投影 + 历史合并 + 重连）
        → PT_Indirect_Spatial（新：邻域复用 + 重连）
        → PT_Denoise → PT_Atrous → ToneMap → FXAA
```
- **新增 1 条 RT 管线**（`PT_Indirect_Init` 的 RayGen，或复用 `PT_Full` 加 flag 分支）+ **2 个 compute**（Temporal/Spatial，可复用现有 DI 的 PSO 骨架）。
- `PT_Indirect_Init` 与 `PT_Render` 都需要"从像素发射间接光线并求交"的能力，现有 `PT_Full.rgen` 已有全部代码，**加一个 flag 走不同出口即可**（避免多一条 RT 管线的编译与 SBT 维护成本）。
- 帧图资源：`PTIndirectReservoir` 双缓冲（跨帧，SSBO，不进 RG，与现有 `FinalReservoir` 同款约定）。

### 3.4 无偏性要点（写给实现者）

- 复用必须满足：`W_shifted = p̂(y0)·... / (M · p_shift(x))`，其中 `p_shift` 是**shift 后的 pdf**（含雅可比）；漏掉雅可比 ⇒ 系统性偏差，且在"参考渲染器"定位下是**致命**的（比慢更糟）。
- MIS/权重：时域与空间合并用 pairwise MIS（与现有 DI 的加权和保持同一套写法，避免两套约定）。
- 可见性拒绝是一种**合法的零贡献**（不引入偏差），但要在 `M` 的记账上保持一致（与 DI 的 `weightSum/M` 同样处理）。
- 目标函数必须用**当前像素**的 albedo/metallic（现有 `albedoMetallic` UAV 已经在做这件事，直接沿用）。

---

## 4. 代价评估（实测优先）

### 4.1 现有 PT 成本曲线（实机实测）

环境：`Samples/05.Sponza-PathTracing`，**960×540**（示例内置 `config.windowWidth/Height`），NVIDIA **RTX 4060 Laptop 8 GB**，驱动 595.79，
时域降噪 + A-Trous 开、ReSTIR DI 关，Sponza（105 实例）。

| `pt_spp` × `pt_bounces` | ms/帧 | FPS | 说明 |
|---:|---:|---:|---|
| 1 × 1 | **2.1** | 468~477 | 当前 `Content/Config/05_...cfg` 的取值 |
| 1 × 4 | **9.4** | 107~108 | |
| 4 × 4 | **38.4** | 26.0 | |
| 6 × 7 | **61.1** | 16.4 | 与一次 25 s 冒烟实测的 16.5 FPS 完全吻合 |

- **边际成本**（1 spp 下）：bounce 1→4 每多一个 bounce ≈ **+2.4 ms**；4 bounces 下每多一个 spp ≈ **+9.2 ms**；即"每 spp ≈ 一条 b=4 的路径链"。
- 简单线性模型：`ms ≈ spp × (2.1 + 2.4 × (b−1))`，在 4×4 上误差 <3%；`6×7` 上高估（轮盘赌在深弹射处截断）——**报读数时必须同时声明 spp/bounces**。
- 测量口径（重要，避免复现歧义）：`Content/Config/05_Sponza-PathTracing.cfg` 会被示例**回写**（实测该文件在 2026-09-19 10:59 被写过一次，内容从 `6 spp / 7 bounce` 变成 `1 spp / 1 bounce`）；因此
  **测量必须走 `HE_CFG` 私有副本**（`Tools/pt/set_cfg.py` 覆盖），脚本见 `build/verify/measure_pt_cost.ps1` / `recheck_pt_fps.ps1`。
  表里 `6×7 = 16.4 FPS` 与那次 25 s 冒烟实测的 16.5 FPS 吻合，说明**冒烟当时**的基础 cfg 是 6×7，而不是现在的 1×1。

### 4.2 复用 pass 的开销（实测，GRIS 复用成本的下界代理）

| 配置 | 无 ReSTIR | 有 ReSTIR DI | 增量 |
|---|---:|---:|---:|
| 1 spp × 1 bounce | 2.14 ms | 2.49 ms | **+0.35 ms** |
| 1 spp × 4 bounces | 9.37 ms | 9.97 ms | **+0.60 ms** |

⇒ **3 个 compute dispatch（含 M=16 候选的 shadow ray 与 5 邻居空间复用）在 0.52 MP 上只要 0.35~0.60 ms**。结论：**复用的调度与邻域开销可忽略，成本全在光线数量上**。GRIS 的增量因此主要看"每帧多打几条路径光线"：
- 形态 A：bounce0 的间接光从"NEE 1 条 shadow ray"变成"1 条间接光线 + 复用时的 1 条重连 ray/候选" ⇒ 粗估 **+2~5 ms @0.52 MP**（≈ +1 个 spp 当量的一部分）。
- 形态 B：每帧多一整个路径链 + 复用候选的重放 ⇒ 粗估 **+10~25 ms @0.52 MP**（把 468 FPS 基线压到 ~30~60 FPS），且随 bounce 数线性增长。

### 4.3 显存（推算，公式给出便于复算）

**现状 @960×540（518,400 px）**：

| 项 | 计算 | 大小 |
|---|---|---:|
| ReSTIR DI 蓄水池 ×4 | `4 × W·H × 32 B` | **66.4 MB** |
| ReSTIR 历史 depth/normal ×2 | `2 × W·H × (4+8) B` | 12.4 MB |
| PT 5 个 UAV | `W·H × 32 B`（8+4+8+4+8） | 16.6 MB |
| 材质/法线/UV 纹理（Sponza） | `n×11×16 B` + `3·tris×(16+8) B` | ≈ 12 MB |
| 小计（PT 链路） | | **≈ 107 MB** |

**B7 新增（推算）**：

| 形态 | 每像素每槽 | ×3 槽 | @0.52 MP | @1080p（4× 像素） |
|---|---:|---:|---:|---:|
| A：GRIS-lite | 80 B | 240 B | **≈ 124 MB** | ≈ 500 MB |
| B：完整 GRIS（R=4~7，48~64 B/顶点） | 192~448 B | 576~1344 B | **≈ 0.30~0.70 GB** | **≈ 1.2~2.8 GB** |

⇒ **形态 B 在 1080p 上超出 8 GB 笔记本 GPU 的可行区**（还要与 HDR/GBuffer/降噪历史共存）；形态 A 是唯一现实起点。压缩手段（fp16 位置/法线、`radiance` 用 RGB9E5、路径顶点按需存）能把 B 压到约一半，但**压缩本身又是一个独立的正确性风险源**（要在无偏性判据下验证）。

### 4.4 管线数量与编译时间（实测，含与文档数字的差异）

- 现有 RT 管线：`05` 只建 **1 条**（FullPT）；`04` 建 **5 条**（RTShadow/RTAO/RTReflection/RTGI/DDGITrace）。
- 实测创建耗时（**磁盘 PSO 缓存热**）：`05` 的 FullPT `27.405 → 27.406` ≈ **1 ms**；`04` 的每个效果管线 ≈ **2 ms**。
- ⚠ `全路径追踪管线规划.md` §11 记的"**本机 Intel Arc 上 RT 管线编译约 20 s/个**"指的是另一台机器。B7 若在**冷缓存**首次运行要多编译 1 条 RT + 2~3 条 compute，**本机冷编译时间未测**（列为未知项，触发时要补测；`build/Samples/05.Sponza-PathTracing/pipeline_cache.bin` 638 KB 是热缓存证据）。

### 4.5 工程面清单（形态 A 的最小集合）

| 类别 | 内容 | 规模（估算） |
|---|---|---|
| Slang | 新 `PT_Indirect_Init`（可复用 `PT_Full.rgen` 加 flag 分支）+ `PT_Indirect_Temporal/Spatial.comp` + `PTIndirectReservoir` 结构 + 重连/雅可比函数 | 3~4 个文件，~600 行 |
| C++ | 新 `ReSTIRIndirectPass`（照 `ReSTIRPass` 骨架：布局/PSO/SSBO/历史纹理/`Execute`）+ 帧图 3 个 pass + CVar 6~8 个 + `PT_Render` 读蓄水池的分支 | ~700 行 |
| RHI | **无需新能力**：只用现有 SSBO + UAV + compute + RT 管线（`descriptorBindingStorageBufferUpdateAfterBind` 等已启用）；**不需要 SER**（那是 B8） | 0 |
| 工具/文档 | 无偏性对照脚本（复用 `analyze_pt.py --compare/--diff`）、收敛判据沿用任务 6 | 小 |
| 单测 | 雅可比闭式解、shift 的可见性/退化处理、reservoir 记账（`M`/`W`） | 3~5 个 doctest |

---

## 5. 风险与未知

1. **偏差（最高风险）**：漏雅可比、shift 后未重算几何项、可见性条件写错，都会让"标准答案"悄悄变偏。**必须**把"GRIS vs 迭代式 PT 的逐像素对照"作为准入门槛，而不是事后检查。
2. **两套估计器的语义分裂**：`ReSTIR GI` 的落点已在 §12 C16 归到 `Lumen…` P6。若 PT 侧另起一套，会出现"bounce0 的直接光由 DI 负责、间接光由 PT 版 GI 负责、Lumen 侧还有 P6"的三方重叠。**触发前先定落点**。
3. **显存**：形态 B 在 1080p ≈ 1.2~2.8 GB（推算）——本机 8 GB 上大概率不可行；即使形态 A 也要与 5 个 PT UAV + 降噪历史 + 场景纹理争用。
4. **RT 着色器发散**：GRIS 让相邻像素走不同深度的路径，发散更严重；本机**未启用 SER**（B8 未做），这部分收益拿不到，实时化路线存在连带依赖。
5. **未知**：冷缓存的 RT 管线编译时间（本机）、1080p/1440p 的实测显存与帧时（本文件只有 960×540 实测）、`Rand` 在"跨帧重放"时的相关性（源帧与当前帧同层会否出现时空相关，需要一次专门的相关性检查）。
6. **收益边界**：GRIS 的价值在**实时**预算下最大。作为离线/交互式参考渲染器，现有 1 spp×b=4 只需 9.4 ms，配合时域累积 30 帧即收敛（§12 任务 6：p50 在帧 30 变化 0.000%），**已经够用**。

---

## 6. 里程碑与验收判据（触发"PT 转实时主路径"之后再执行）

| 里程碑 | 内容 | 验收判据（可执行） |
|---|---|---|
| **M0 落点决策** | 定 ReSTIR GI/GRIS 归 PT 侧还是 P6（§12 C16）；若归 P6，本预研的 M1/M2 转为其子任务 | 决策记录进 `全路径追踪管线规划.md` §0.1/§0.3 与 Lumen 规划 §5.2 |
| **M1 GRIS-lite（形态 A）** | shift 机件 + 1 顶点重连 + 时域/空间复用；`PT_Render` 读新蓄水池 | ① 与迭代式 PT 逐像素对照（`analyze_pt.py --compare`）：SPP 递增时偏差收敛到噪声内（无系统性偏移）；② p50 达收敛所需帧数 ≤ 现有方案；③ 帧时增量 ≤ +5 ms @0.52 MP |
| **M2 完整 GRIS（形态 B）** | 随机重放 shift + 路径存储（含压缩）+ 两种 shift 的 MIS | ① 同上无偏性判据；② 显存 ≤ 1 GB @1080p；③ 焦散/多弹射场景的噪声方差显著优于 M1（给出对照读数） |
| **M3 与 DI / P6 统一** | 明确 bounce0 直接光/间接光的责任划分；必要时并入 P6 的估计器 | 两套估计器只留一套交接语义；文档更新 |

**M1 的最小可跑集合**（若只想验证可行性）：跑通"1 像素 1 间接光线 + 时域复用 + 重连可见性"，**不做**空间复用与压缩；用 `HE_DUMP_PT` 的 hdr/albedo 两个目标做逐像素对照即可判定无偏性。

---

## 7. 本轮明确不做（非目标）

- 不改 `全路径追踪管线规划.md` §0.1 的定位、§10/§11 的目标；
- 不实现任何 B7 代码；
- 不让 §12 B 组的触发条件"被默认满足"（B7 状态保持未做）；
- 不动 §12 的编号与既有 ✅ 记录（只在 B7 行加一条指向本文件的指针）。

---

## 附录 A：数据来源与复现命令

```powershell
# 1) 成本曲线（每档 18~22 s，数 "RG pass: PT_Render" 行算 FPS；HE_CFG 私有副本避免写回基础 cfg）
powershell -File build\verify\measure_pt_cost.ps1
powershell -File build\verify\recheck_pt_fps.ps1     # 基础 cfg vs 私有副本的一致性复核
powershell -File build\verify\measure_restir_cost.ps1 # ReSTIR DI 复用开销

# 2) 机件缺口检索（全引擎，排除 External/；预期全为 0）
#    Jacobian / shiftMapping / ShiftMapping / reconnection / Reconnection / GRIS / PathReservoir
# 3) RT 管线创建耗时（磁盘 PSO 缓存热）：05 日志里 CreateRTPipelineState → CreateEffectPipeline 的时间差
# 4) 收敛与对照（标准答案的现状）
python Tools\pt\analyze_pt.py --converge <tag30> <tag60> <tag120> <tag240> --conv-tol 0.01
python Tools\pt\analyze_pt.py --compare <pt_tag> <deferred_tag> --target hdr
```

## 附录 B：与 §12 C16 / P6 的关系

`ReSTIR GI` 的实现在本预研里被当作 GRIS 的 M1（形态 A），而它在 `全路径追踪管线规划.md` §6.1 的落点已归 `Lumen与Nanite完整设计规范` §5.2 任务 20（P6：ReSTIR GI 统一估计器 + 纹理绑定数组化）。因此：
- 若 P6 先落地 → 本文件的 M1/M2 应作为 P6 的 PT 侧适配（复用其估计器与绑定数组），而不是另写一套；
- 若决定 PT 侧先做 → 需要在 §0.3 增补一条"落点从 P6 移回 PT"的修订记录（本轮不做该修订）。
