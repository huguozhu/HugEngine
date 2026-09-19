# Lumen 设计与实现

> 最后更新: 2026-09-19
> 状态: 设计规范已定稿；实现分阶段推进（部分基础设施已落地）

本文件由两份源文档合并重写而成：《Lumen 与 Nanite 完整设计规范》（只取其 Lumen / 全局光照
部分，Nanite 部分归 `Nanite设计与实现.md`）与《ReSTIR PT / GRIS 预研》（全文并入附录 A）。
合并只做重排与连接，源文档中的表格、实测数字、决策记录、里程碑与任务编号均逐条保留。

> **2026-09-19 补充（架构对照后的修正）**：与 UE5 Lumen 做了一次架构对照（见 §15–§17），
> 结论是两处差异属于"架构分叉"而非"规模简化"，**本轮就补进设计**：
> · §4 新增 **Surface Cache 页状态机 + 每帧预算**；
> · §6 把原文一维的"距离分支"改为 **追踪表示 × 着色表示二维解耦**（为 Hit Lighting 留出入口）；
> · §12 增列与之配套的两项实现前置（Provider 生命周期遍历、RG 显式依赖边）；
> · §17 记录**明确不抄**的清单与"已知降级"，避免后续对齐 UE 时范围失控。

---

## 0. 本文件怎么读

- **第一 ~ 八章是"设计"**（§1–§8）：Lumen 的目标与定位、可复用的现有基础设施、数据流、
  Surface Cache、SDF 体系、Screen Probe Gather、Radiance Cache、以及与现有管线的集成方式。
  这部分是**规范性描述**：它说明"要做成什么样"，不代表已经做完。
- **第九 ~ 十四章是"实现"**（§9–§14）：§9 是**基于代码实证**的已落地现状（每一项都给
  `文件:行号` 或符号名，未落地的给出检索关键词与 0 命中说明）；§10、§11 是两项**待落地**的
  框架前置工作；§12 是 Lumen 里程碑总表（含与 Nanite 的推进顺序关系）；§13 是 Lumen 相关的
  关键数据结构；§14 是 Lumen 相关的已知风险。
- **哪些已落地、哪些待落地**，以 §9 为准。要点：GI Provider 统一抽象（`IGIProvider`）、DDGI
  探针 GI、RT GI（`RTGIPass`）、RT 降噪链数据化（`std::vector<Stage>`）、降噪去重
  （`SpatialDenoiseAux`）**已落地**；Lumen 本体的 Surface Cache / SDF / Screen Probe /
  Radiance Cache **全部未落地**（设计在 §4–§7，里程碑 L1–L5）；统一降噪框架的第三步
  （11.3，§10）与 Provider 执行单位收敛 / 绑定数组化（§11）**待落地**。
- **附录 A 是 ReSTIR PT / GRIS 预研**：它是 Lumen GI 的落点方案与代价评估（含全部实测数字、
  成本表、显存推算、里程碑 M0–M3、非目标、通用性分析与复现命令）。它是**决策依据与执行
  留档**，不是已排期的实现任务。
- **第十五 ~ 十七章是"与 UE5 Lumen 的架构对照"**（§15–§17，2026-09-19 补充）：§15 是能力与
  显存对照（含本设计自己的显存测算），§16 是架构判断（哪些地方本设计更好、哪四处处属于
  "架构分叉"），§17 是决策记录与"明确不抄"清单。它不是设计规范，但**实现前应当读**：
  §16 的四处分叉里有两条已经落回 §4 / §6 的设计，另两条是"已知降级"，写清了代价。
- **Nanite 相关的内容不在本文件**：源文档中与 Nanite 同时出现的表述（共享基础设施、帧图
  插入点、推进顺序）予以保留，但**逐处注明"属 Nanite"**，其设计与里程碑见
  `Nanite设计与实现.md`。

---

## 一、设计

### 1. 目标与定位

在 HugEngine 的**延迟渲染管线**上集成**动态全局光照（Lumen）**，目标形态为：

- 近场用 **Mesh SDF + Global SDF Clipmap 的软件光线步进（SDF Ray Marching）**做追踪；
- 远场切换到 **硬件光线追踪（HW RT，VK 1.3 的 AS + RT PSO + SBT）**；
- 命中点材质通过 **Surface Cache**（Card Capture + Page Table）提供，避免为每条光线重建材质；
- 屏幕空间以 **Screen Probe Gather** 组织半球追踪，输出二阶球谐（SH）供 Lighting 采样；
- 低频 GI 由 **Radiance Cache**（在现有 DDGI 基础上升级）承担，做跨帧稳定；
- 命中点的**直接光照**复用现有 **ClusteredShading LightGrid**。

定位上的三点约束（来自源设计的验证标准）：

1. Lumen 是**延迟管线的一个源**，而不是另起一条管线：产物最终由 Lighting Pass 采样
   （见 §8 的帧图插入点）。
2. Lumen 是**多信号共存**的消费方（Screen Probe Gather / Radiance Cache / 反射 / 阴影 / 探针
   同帧），因此它同时是"统一降噪框架"（§10）与"Provider 执行单位收敛 + 绑定数组化"（§11）
   这两项框架工作的**真正验收对象**。
3. 最终性能目标是 **L6 的 60fps @ 1080p**（见 §12 里程碑总表）。

> Nanite 是 Lumen 的 GBuffer 产出来源之一（Nanite Phase 1 直接写现有 5×MRT GBuffer），但
> Nanite 的设计与实现由 `Nanite设计与实现.md` 负责，本文件不展开。

### 2. 现有基础设施（可复用部分）

下表取自源文档第 1 节。"属 Nanite"一列标注该能力的**主要**服务对象；标注为 Nanite 的行
仍是 Lumen 可复用的共享基础设施，但它的设计与实现归 `Nanite设计与实现.md`。

| 能力 | 状态 | 用途 | 归属 |
|------|:---:|------|------|
| VK 1.3 + RT (AS + RT PSO + SBT) | ✅ | Lumen 远场 HW RT 追踪、Nanite BVH 遍历 | Lumen + 属 Nanite |
| VK_EXT_mesh_shader | ✅ | Nanite Cluster 硬光栅 | 属 Nanite |
| GPU Culling (Hi-Z + Two-Phase + PTG) | ✅ | Nanite Instance/Cluster 剔除 | 属 Nanite |
| VK_EXT_device_generated_commands | ✅ | Nanite 间接绘制生成 | 属 Nanite |
| GPU WorkGraph (软件模拟) | ✅ | Nanite 剔除链 → Draw 链 | 属 Nanite |
| Bindless Textures | ✅ | Surface Cache Atalas、Nanite 材质 | Lumen + 属 Nanite |
| AsyncCompute | ✅ | SDF 更新、Surface Cache 更新 | Lumen |
| DDGI (探针 GI) | ✅ | 升级为 Radiance Cache | Lumen |
| GBuffer DeferredPipeline | ✅ | Nanite Phase 1 写入目标 | Lumen + 属 Nanite |
| ClusteredShading LightGrid | ✅ | Lumen 命中点直接光照 | Lumen |
| Denoiser (5×5 双边) | ✅ | Screen Probe Gather 空间滤波；**统一降噪框架**见 §10 | Lumen |
| meshoptimizer | ✅ | Nanite 预处理 Cluster/LOD | 属 Nanite |
| VMA | ✅ | GPU 内存管理 | 共享 |

> 注：表中 `Denoiser` 一行声明它"可用于 Screen Probe Gather 的空间滤波"，这是**设计意图**；
> 降噪器类本身已落地（§9），但 Lumen 尚未接入，且其多信号共存的框架（§10 的 11.3）仍待做。
> 两者不矛盾：能力在，消费方与框架未到。

### 3. 数据流

```
GBuffer (Albedo/Normal/Emissive/Depth)
    │
    ├──→ Surface Cache ──→ Atalas (Albedo|Normal|Emissive)
    │         │
    ├──→ SDF Tracing ←── Mesh SDF + Global SDF Clipmap
    │         │
    └──→ Screen Probe Gather
              │
              ├── 近场 (dist < MaxSDF): SDF Ray Marching → Surface Cache 读材质
              ├── 远场 (dist >= MaxSDF): HW RT TraceRay → ClosestHit 读材质
              │
              ├── Spatial Filter (3×3 YCoCg AABB 裁剪)
              ├── Temporal Filter (混合 Radiance Cache 历史)
              └── SH Project (二阶 4 系数) → Radiance Cache
```

### 4. Surface Cache

| 配置项 | 值 |
|--------|----|
| 页面大小 | 128×128 texels |
| Atalas 总页数 | 1024 (初始，可扩展到 4096) |
| 虚拟分辨率 | 128² × 1024 = 4096² (→ 8192²) |
| 每页通道 | RGBA16F × 3 (Albedo|Normal|Emissive) |
| 页面组织 | 3D Clipmap (世界空间对齐) |
| 管理方式 | Page Table (虚拟→物理映射) + LRU 淘汰 |
| Card Capture | Compute Shader 软件光栅化，逐 Card 一个线程组 |
| 更新触发 | Feedback Pass 检测缺失页 → Request → Allocate → Capture |
| 失效 | 物体移动/材质变更时标记脏页 |

**页状态机（2026-09-19 补充，原设计缺）**

原设计只写了"LRU 淘汰 + dirty 标记"，缺了**跨帧请求队列**这一层。而捕获不可能一帧做完：
atlas 4096² ≈ 402 MB（8192² ≈ 1.61 GB，见 §15.3），一页 128² × 3 通道 RGBA16F ≈ 393 KB，
软件光栅一页还要遍历该页覆盖的卡片几何。所以页的生命周期必须显式建模：

| 状态 | 含义 | 迁移 |
|------|------|------|
| `Invalid` | 无数据（从未捕获，或已被淘汰） | Feedback 命中 → `Requested` |
| `Requested` | 已排队，等待本帧预算 | 拿到预算 → `Allocating` |
| `Allocating` | 页表已分配物理位置，等待捕获 | 发出 Capture → `Capturing` |
| `Capturing` | 本帧正在被 Card Capture 写入 | pass 结束 → `Captured` |
| `Captured` | 数据有效 | 几何/材质变更 → `Dirty`；LRU 命中 → `Invalid` |
| `Dirty` | 数据过期 | 重新排队 → `Requested` |

两个要点：① **排队与捕获分离** —— Feedback 只是"申请"，不保证本帧完成；② 状态要能被面板 /
dump 工具读到，否则"某块墙一直发黑"无法归因（这是 §16 里"卡片覆盖率可视化"那条工具欠账的
另一半）。

**每帧预算（2026-09-19 补充，原设计缺）**

| 预算项 | 首版建议值（可配） | 依据 |
|--------|------------------|------|
| `maxCapturesPerFrame` | 8 ~ 16 页 | 一页 ≈ 393 KB 写入 + 卡片软件光栅；8 页 ≈ 3 MB/帧写入，留足余量 |
| `maxAllocationsPerFrame` | ≤ `maxCapturesPerFrame` | 分配要改页表，必须与捕获同批提交 |
| `maxFeedbackPages` | 反馈按 16×16 像素降采样 | Feedback pass 的读回不能变成新的瓶颈 |
| 未完成的请求 | 留在 `Requested` 跨帧排队 | 不允许"一帧补完" —— 这正是 UE 那套 **amortized over multiple frames** 的含义 |

捕获顺序按"本帧贡献"排序（屏幕覆盖面积 × 屏幕空间重要性 × 距离），**不是** LRU 或随机：
排序在 C++ 侧做小规模前缀和即可，Feedback pass 只需输出 `(pageID, weight)` 列表。

**与帧图 / 生命周期的接口约束（代码实证，2026-09-19 补充）**

- Capture / Inject 这类中间 pass **必须显式声明 RG 读写边**：`RenderGraph::TopologicalSort` 是
  LIFO 栈（`Engine/Render/RenderGraph.cpp:181-186`），无依赖边的 pass 之间"注册顺序 ≠ 执行
  顺序"；且 `CullDeadPasses`（`:302-324`）会删掉"写了但无人读"的 pass。
- atlas（以及 §5 的 SDF clipmap）这类**跨帧持久资源必须自建自持**：`rg.CreateTexture` 建出来的
  纹理 pass 拿不到 `IRHITexture*`（`Engine/Render/RenderGraph.h:106-119` 没有取指针的接口，
  `PassExecuteFunc` 只收 `IRHICommandList*`，`RenderGraph.cpp:415-416` 的 `textures[]` 是局部量），
  只能 `device->CreateTexture` + `rg.ImportTexture`。
- resize 与销毁路径：`DeferredPipeline::OnResize`
  （`Engine/Render/Pipeline/DeferredPipeline.cpp:575-594`）与 `Shutdown`（`:459-514`）
  **目前都不遍历 `m_GIProviders`**（Provider 的 `Initialize/Shutdown/OnResize` 全仓只有 AO 的
  `Initialize` 被调用过）—— L1 必须补这段遍历，否则 resize 后会读到旧尺寸的页表/历史。

### 5. SDF 体系

| 组件 | 分辨率 | 格式 | 生成方式 |
|------|--------|------|---------|
| Mesh SDF | 128³ per mesh | R16F | Compute Shader (Brute-force 每三角形写入体素) |
| Global SDF | 512³ 单层 (→ 4 层 Clipmap) | R16F | Compute Shader (Mesh SDF 注入 + 增量更新) |
| Clipmap 层 | 4 层 × 256³ | R16F | 后续扩展：每层覆盖范围 ×2 |

**SDF Ray Marching**：

- Sphere tracing 步进算法
- 自适应步长 (最大步数 64, 收敛阈值 0.1 体素)
- 法线从 SDF 梯度估算 (3 次采样)
- 跨层切换：当前层步数用完未命中 → 下一层继续

**scatter 版与实测（2026-09-19，替代上面的 gather 首版）**

gather（每体素遍历全部三角形）的代价是 O(体素 × 三角形)，128³ 下不可接受；scatter 反过来
（每三角形一个线程组、只扫自己的 AABB，`InterlockedMin(asuint(d))`），代价与体素数无关。
scatter 只写"表面附近的一条带"，故必须补一步 **跳步洪泛**（`res/2 … 1` 逐级 26 邻域松弛），
否则远处留 `+inf` —— 那在 sphere tracing 里是**高估**（会直接跳过几何）。

| 指标 | gather 首版（32³） | scatter 版（128³） |
|------|------------------|------------------|
| mesh 数 / 分辨率 / 体素边长 | 51 / 32³ / 0.45~87.6 | **16** / 128³ / **0.296~21.9** |
| 显存 | 6.38 MB | **128 MB**（16 × 8.4 MB） |
| 三角形上限 | 4096（超限 28/79 个 mesh 被跳过） | **20000**（0 个被跳过；代价与三角形数线性） |
| 自检判据与结果 | 全场 1/4 体素（gather 是精确点-三角形距离） | **分层**：近表面（≤2 体素）1/4 体素、远场（洪泛近似）2 体素 ⇒ 远场 61/64 PASS |

两处诚实的保留：① 探针网格（步长 = res/4）**没有采到近表面那一条带**（实测 `近表面 0/0`），
所以"近表面精度"目前只有 scatter 的算法保证、没有实测覆盖 —— 补法是让探针沿网格顶点附近采样；
② 洪泛用的是标准的单趟逐级松弛，远场最大误差实测 **2.93 单位 ≈ 9.9 个体素**（0.296 体素下），
比"1 体素内"的理论预期差，说明每级只做一趟不够（需要前向/后向两趟），记为后续优化项。

**clipmap 分层首版与实测（2026-09-19）**

实现：`LumenSDFConfig::globalLayers = 2` + `nearFraction = 0.25`，`SetupGlobalGrid` 建两层
（层 0 = 近层：场景中心、边长 = 全场 × 0.25；层 1 = 远层：覆盖全场），注入/转换对每层各跑一遍；
`SDF_RayMarch.comp.slang` 改为**同时采样两层并取最小值**（两者都是下界 ⇒ 仍不高估），
`MarchPC` 扩到 80B（两层参数），新增 binding 6 绑远层。

| 指标 | 单层（旧） | clipmap 两层（新） |
|------|-----------|------------------|
| 层 0 / 层 1 体素边长 | — / 24.64 | **6.16** / 24.64（覆盖 788 / 3154 单位） |
| 安全判据（不得高估） | PASS（-5.44） | **两层都 PASS**（层 0 最大高估 **-111.1** ≤ 12.3 判据？见下） |
| 下界质量（2 体素内 / 平均低估） | 23.4% / 621.5 | 层 0 **0.0% / 454.6**、层 1 21.9% / 568.6 |
| sphere tracing 精度（≤1 体素） | 37.3~39.0% | **13.6%**（归一化体素换成近层的 6.16 ⇒ 绝对误差基本不变：平均 13.5 × 6.16 ≈ 83 单位 vs 旧 3.15 × 24.6 ≈ 78 单位） |

**结论：clipmap 分层本身没有解决紧度问题，真正的根因被这一步钉死了** —— 注入的
"**AABB 外取到 AABB 的距离**"是元凶：本场景 mesh 的 AABB 很大（最大边长 3154 单位），
min 合并之后，网格内几乎所有体素都能从某个大 AABB 拿到一个极小的值（平均低估 454~569 单位），
于是步进在远离表面处就低于阈值 = **提前判命中**。层 0 的判据那一行还暴露了一个细节：
它的"最大高估 -111 ≤ 12.3"之所以判 PASS，是因为判据写的是 `maxOver ≤ tol`，而 maxOver 为负 ——
**判据本身没问题（安全方向），但它对"低估过多"不敏感**，这正是要把下界质量单独记录的原因。

**下一步（明确）**：把全局层的构建方式换成与 mesh 层同一套 ——
**scatter（只在 mesh AABB 内注入精确场值）+ 跳步洪泛补全**（`SDF_MeshFlood` 可直接复用到全局网格，
它本来就是"任意网格 + 步长"的参数化）。这样全局场才是真正的距离场（近表面精确、远处洪泛近似），
而不是现在这个"处处极小值"的伪下界。这条修复落在 §12 的实现前置里。

**该修复已实施，但假设被证伪（同一轮，2026-09-19）**

按上面的计划改完（`SDF_GlobalBuild` 的 mode 1 只在 mesh AABB 内注入精确值、去掉 AABB 外回退；
C++ 侧在每次注入后跑全局网格的跳步洪泛，新增 `m_GlobalFloodPSO` 用全局描述符集布局 ——
绑定的 set 与 pipeline layout 必须一致，否则直接崩），**测得的紧度几乎没变**：

| 指标 | 改前（AABB 外回退） | 改后（scatter + 洪泛） |
|------|-------------------|---------------------|
| 层 0 下界质量 | 0.0% / 平均低估 454.55 | **0.0% / 454.55（逐位相同）** |
| 层 1 下界质量 | 23.4% / 568.62 | 23.4% / **560.90** |
| sphere tracing | 15/110、平均 13.543 | **逐位相同** |

⇒ "AABB 外回退"这个假设**不是主因**。为什么逐位相同也说明一件事：探针（步长 32，共 64 点）
几乎都落在**某个 mesh 的 AABB 内**，走的是"注入精确值"这条路 —— 回退与洪泛都碰不到它们。
于是下一层怀疑对象变成：**注入进去的"精确值"本身**（该 mesh 的 128³ 场在它自己的大 AABB 上只有
24.6 单位体素，且洪泛补全的远场近似会偏小），以及全局层与 mesh 层体素尺度不一致带来的采样误差。

**数据对照（三方 dump）与结论（2026-09-19）**

改成"先取证再改算法"后，两处诊断值直接把问题指出来：

1. **自检探针从 mesh 0 改为"AABB 最大的 mesh"**（mesh 10）：近表面 **9/9 在 1/4 体素内**
   （scatter 精确 ✓），但**远场 46/55 在 2 体素内、最大误差 100.37 单位**（阈值 5.47）⇒ FAIL。
   即**大网格的远场（洪泛补出来的那部分）错到 4 个体素以上** —— 而全局层的 min 合并会优先取它。
2. **三方对照**（层 0 低估最严重的前 5 个探针，格式 `gpu / exact / meshD / meshIdx`）：

   ```
   [0.00 / 894.38 / 894.38 / 6]
   [0.00 / 879.71 / 879.71 / 6]
   [0.00 / 848.51 / 848.51 / 8]
   [0.00 / 829.71 / 829.71 / 8]
   [0.00 / 758.78 / 758.78 / 8]
   ```

   `gpu = 0.00` 而真实距离（以及"本该注入的值"）是 **894** 个单位。这已不是"下界偏松"，而是
   **全局层在这些点上存了 0** —— 步进必然在 t=0 附近判命中。

**结论**：松的源头在 **mesh 层远场**（洪泛近似 + 大网格 24.6 单位体素），而不是全局层的注入语义
（上一轮已证伪）。修复顺序据此改为：① 洪泛改**双向两趟/多趟**并核对 L2 步长校正（当前单趟逐级
松弛的最大误差已达 100 单位）；② 大网格不要和细碎网格共用同一分辨率（按 AABB 大小分配分辨率或
分块）；③ 只在上面两条成立后再回头看全局层。诊断工具（`m_ProbeMeshIndex` 选最大 AABB、
三方对照打印）已留在代码里，后续每次改动都用同一张表验收。

**又抓到一个真 bug：注入用的采样器从未绑定（2026-09-19）**

`SDF_GlobalBuild.comp.slang` 把 `u_MeshFieldSampler` 声明在 **binding 3**，而全局描述符集布局里
只有 `{0,1,2,4}` —— 本引擎的 RHI 只有**组合图像采样器**（纹理与采样器同 binding，见
`DeferredLighting.frag` 的写法），没有独立采样器描述符。于是采样器未绑定、采样结果恒为 0：
这正好解释了三方对照里"真实距离 894 的点上全局层存的是 0"。这与第 6 轮在 raymarch shader 里修的
是同一类错误（当时改了两个 raymarch，漏了这一个）。已把采样器移到 binding 2（与纹理同号）。

修完的实测变化（**没有出现"一改就好"**，如实记录）：

| 指标 | 修前 | 修后 |
|------|------|------|
| Global 层 0 下界质量 | 0.0% / 454.55 | **0.0% / 454.55（逐位相同）** |
| Global 层 1 下界质量 | 23.4% / 560.90 | 23.4% / **550.99** |
| 细节追踪"更近命中"的射线 | 0 条 | **16 条**（细节层第一次真正参与） |
| sphere tracing 安全 | 仅 CPU 命中 0（PASS） | **仅 CPU 命中 3 ⇒ FAIL（出现穿漏）** |

两条必须正视的变化：① 三方对照里那 5 个探针**仍然读 0**，说明 0 不是采样器造成的，而来自
**某张 mesh 场自身在远场就是 0**（对照表显示探针落在 mesh 6/8 的 AABB 内）；② 新的 **3 条穿漏**
是字段值变大后的直接后果（有的区域不再是 0，步进推进得更远，暴露出全局层在别处偏大）。

**全部 mesh 的场自检：没有任何一张场含 0（2026-09-19）**

把自检从"只查一张网格"改成**一次查完所有网格**（探针缓冲按 mesh 分段，convert 用 `ranges.x` 传段基址），
结果非常干净：

| 观测 | 数值 |
|------|------|
| 覆盖 | **16 个 mesh / 1024 探针，零值探针 0 个** |
| 最差 5 张（远场超差数 / 最大误差） | 条目 11（29/64，278.7）、条目 2（34/64，203.6）、条目 12（32/56，173.2）、条目 6（23/61，154.9）、条目 3（31/64，138.2） |
| 它们的值域 | 全部正常（如条目 11 `[175.27, 2588.41]`） |

⇒ **mesh 层的场里根本没有 0**，而全局层的对照表却在真实距离 894 的点上读 0。也就是说"0"来自
**全局层这一侧**（它的注入、它自己的洪泛、或它的探针读回路径），与 mesh 层无关。这条把范围又缩了
一格，同时确认了唯一确凿的精度障碍：**洪泛远场近似**——大网格（边长 ~2800、体素 21.9）上最大误差
138~279 单位（6~13 体素），这是当前分辨率下的尺度必然。

**哨兵自证：那 5 个"0"是探针没写，不是场值为 0（2026-09-19）**

在创建探针缓冲后先写入哨兵 **-12345**，读回时统计哨兵残留：

| 层 | 哨兵残留 | 结论 |
|----|---------|------|
| 层 0（近层） | **64/64** | 该层的探针缓冲**从未被写过** —— 之前三方对照里读到的 `0.00` 是**缓冲区初值**，不是场值！ |
| 层 1（远层） | 0/64 | 正常写入（它的下界质量数据因此可信：23.4% / 平均低估 550.98） |

⇒ 前几轮基于"全局层在真实距离 894 的点上存 0"做的推断**全部作废**：那是**诊断自身的伪影**。
教训与上一轮同类：**先证明"数据是被写出来的"再解释数据**。层 0 的探针路径为什么没写，是下一步的
唯一事项（层 1 同一段代码能写，差别只在它是循环里的第二个 —— 优先查描述符绑定/派发顺序）。

同时这条也说明：**层 0 的"下界质量 0.0% / 平均低估 454"同样不可信**（读的是未写缓冲）。

**哨兵结论修正：连"探针有没有被写"这条读回路径本身也不可信（2026-09-19）**

拿到哨兵读数后发现**它自相矛盾**：不写哨兵时层 0 读回全 0（曾据此推断"全局层存 0"），写哨兵
`-12345` 后层 0 报残留 **64/64**（看似"从未写过"）而层 1 报 0/64（写了）。同一段代码、同一段路径，
两次读数给出相反结论 ⇒ **CPU `Map()` 读主机可见缓冲的映射/一致性语义未经验证**，这条读回路径
不能当判据。已做：① 新增独立取样 pass（`SDF_LayerProbe.comp.slang`：每线程一个探针、按坐标采样
该层场，不再依赖 convert 里那段 `id % stride` 条件）；② 该读回路径标记为不可信，**层 0 的探针
数据一律不作为判据**；③ 结论写进代码注释，避免后来者再踩。

**洪泛每级两趟：小幅改善，不是突破（2026-09-19）**

把 `SDF_MeshFlood` 的调用从"每级一趟"改为"每级两趟"（mesh 层与全局层各一处），用逐 mesh 自检
的最差 5 张对照：

| 网格（边长/三角形） | 每级 1 趟（最大误差 / 远场超差） | 每级 2 趟 |
|-------------------|-------------------------------|-----------|
| 条目 11（2788 / 1096） | 278.7 / 29 | **221.5 / 29** |
| 条目 2（2786 / 2211） | 203.6 / 34 | 203.6 / **33** |
| 条目 12（2787 / 3680） | 173.2 / 32 | 173.0 / **29** |
| 条目 6（2789 / 2332） | 154.9 / 23 | 154.0 / 23 |
| 条目 8 等其余 | — | 125.8 / 29 |

⇒ 最坏误差降 20%、两张网格的远场超差数各降 3~4 个，但 **sphere tracing 精度指标完全不变**
（110 / 13.6% / 平均 13.543）。这说明洪泛趟数不是主因，**主因是尺度**：大网格 2800 单位 AABB
配 128³ 只有 21.9 单位体素，误差的量级由它决定。因此下一步该做的是**按 AABB 尺寸分配分辨率
（或对大网格分块）**，而不是继续在洪泛内循环优化。

穿漏的机制终于清楚了：场是**近似**的（跳步洪泛的松弛可能给出偏大的值），而 `t += d` 的全量步进
会把"偏大的 d"直接变成位移，于是射线越过真实表面。改为 **`t += max(0.5·d, eps)`**（全局与细节
追踪两处），这是近似距离场的标准安全系数。

| 指标 | 全量步进 | 半量步进 |
|------|---------|---------|
| 仅 CPU 命中（穿漏，须为 0） | 3 ⇒ **FAIL** | **0 ⇒ PASS** |
| 两者都命中 / ≤1 体素 | 107 / 13.1% | 110 / **13.6%** |

⇒ 安全判据回到 PASS，且精度指标**没有变化**：说明精度瓶颈与步进方式无关，仍然是场本身
（洪泛远场近似、以及 mesh 层只覆盖 16 个网格）。这也是本轮唯一"修好"的东西，如实标注。

**当前状态小结（供下一步排序）**：安全性 ✅（不穿漏）；精度 ❌（≤1 体素 13.6%，平均 13.5 体素）；
`仅 GPU 命中 146` 来自**无符号场**在几何内部的假命中（开放曲面场景，符号一致率仅 51~76%）；
远场精度受限于洪泛近似（大网格 138~279 单位）。

把"最大 AABB 的 5 个 mesh"打出来（条目 10/6/11/12/2，边长 2786~2802、1096~3680 三角形 ——
Sponza 的大墙面/地板），并让自检额外报告 **GPU 值域与零值比例**：

| 观测 | 数值 |
|------|------|
| 被查网格（条目 10）的场值域 | **[17.8, 1664.2]，0.0% 探针 ≈ 0** ⇒ 它自己没有 0 |
| 它的远场误差 | 46/55 在 2 体素内、最大 **100.4 单位**（≈4.6 体素）⇒ 就是洪泛近似本身的下限 |
| 三方对照里"存 0"的探针所属网格 | 条目 **6**（大网格）与 **8**（不在前 5 大之列） |

⇒ **0 不是来自被查的那个网格，而是来自另一张（含条目 8 这类较小网格）的场**。这条把范围从
"洪泛全体不准"缩到"某几张网格的场在远场塌成 0"。下一步只需把三方对照扩展成**列出包含该点的
所有网格及其各自 CPU 精确距离 + 逐网格的 GPU 场值**（前者已能算，后者需要每网格一次小读回），
就能点名是哪一张 —— 在那之前不再改算法。

同时注意：洪泛近似本身带来 **~4.6 体素（100 单位）** 的远场低估，这在 128³ / 2800 单位的大网格上
是尺度的必然（体素 21.9）。所以"大网格不与细碎网格共用分辨率"这条仍然成立，只是它排在
"找到塌成 0 的那张场"之后。

**gather 首版与质量边界（步骤 8/9 实测，已被下面的 scatter 版取代，保留作为演进记录）**

实现落在 `Engine/Render/Lumen/LumenSDF.{h,cpp}` + `Engine/Shader/Shaders/Lumen/SDF_MeshBuild.comp.slang`（该 shader 已被 scatter 版删除）：

| 项 | 首版做法 | 与上表的差异 |
|----|---------|-------------|
| 构建方式 | **gather**：每个体素遍历该 mesh 的全部三角形（精确点-三角形距离，Ericson 闭式解） | 上表写的是"每三角形写入体素"（scatter + 原子最小）。首版用 gather 省掉 u32 临时场 / InterlockedMin / 转换三趟，代价是 O(体素 × 三角形) |
| 分辨率 | 默认 **32³**（可配） | 上表 128³。gather 下 128³ × 数千三角形代价不可接受 ⇒ 128³ 需要 scatter 版本（记为 §12 的实现前置） |
| 数据结构 | 每 mesh 一张 **R32F** 3D 纹理（可写 + 可采样） | 上表 R16F（需 16bit storage 支持）；R32F 是 2 倍显存 |
| 符号 | **无符号**距离（内外不区分） | 上表要求带符号。符号与 sphere tracing 的步进一起在步骤 11 落地 |

**实测质量边界（06.GILab，2026-09-19，51 个 mesh）**：

- 体素边长 **0.4495 ~ 87.5741** 世界单位 ⇒ 可分辨特征 ≳ **0.90 ~ 175.1**（约 2 倍体素）。
  **结论：固定 32³ 对大网格（跨度数百至数千单位）基本无用** —— 这正是步骤 10 的 Global SDF
  必须按场景包围盒 + clipmap 分层来做、而不是把所有网格塞进同一个分辨率的原因。
- 三角形上限 4096 时 **28 / 79 个 mesh 被跳过**（不建这张场），这些网格只能由 Global SDF 覆盖。
- 显存：51 × 32³ × 4B = **6.38 MB**（R16F + 128³ 时同数量 mesh 是 51 × 4.2 MB ≈ 214 MB）。
- 自检（512 个探针点，GPU 读回 vs CPU 独立实现）：最大误差 **0.000075**、平均 **0.000008** ⇒ PASS。
  它验证的是数据链路（缓冲布局 / 网格映射 / 偏移 / 描述符 / push constant），不是算法近似。

**已知不适用（写进实现前置，与 §17.6 的 UE 限制清单对照）**：

1. **大跨度网格**：单张立方体场的体素边长随 AABB 最长轴增长，跨度 > 数百单位时精度崩塌；
2. **薄于约 2 个体素的特征**：步进会穿漏（本版无符号，也无法靠符号纠正）；
3. **三角形数 > 上限的网格**：本版直接不建（gather 的代价约束），需 scatter 版本或 LOD 化输入；
4. **内部复杂的单 mesh**（UE 官方明确要求"墙/地/天花板拆成独立 mesh"）：同样的分辨率约束；
5. **动态/蒙皮几何与 WPO**：本版基于 `MeshBatcher` 的静态合并快照，动态网格不参与。

**Global SDF 首版与实测（步骤 10，2026-09-19）**

实现：`Engine/Shader/Shaders/Lumen/SDF_GlobalBuild.comp.slang`（一个 shader 三种模式：清空 /
注入 / 转换）+ `LumenSDF::BuildGlobalField`。单层 128³、R32_UINT 原子最小 + R32_FLOAT 输出。

**注入语义（两处刻意的近似，都是"下界"）**：

- 体素在某个 mesh 的 AABB **内** → 取该 mesh 距离场的值（精确到体素中心）；
- 在 AABB **外** → 取"到该 AABB 的距离"。因为表面一定在 AABB 内，这是到该 mesh 真实表面的
  **下界**；全网格处处有定义，且**永不高估**（高估会让 sphere tracing 穿漏）。

**实测（06.GILab，51 个 mesh）**：

| 指标 | 实测 |
|------|------|
| Global SDF 分辨率 / 体素边长 / 显存 | 128³ / **24.66** 世界单位 / **16.00 MB**（u32 临时 + R32F 输出各 8 MB） |
| 自检（64 探针，CPU 参考 = 对全部三角形取精确最小） | 最大高估 **-5.44**（负 = 未高估）⇒ **安全判据 PASS** |
| 下界质量 | 仅 **23.4%** 探针在 2 体素内，**平均低估 621.5** 世界单位 ⇒ **不足以直接用于步进** |

**结论（下一步的输入）**：安全方向成立，但下界质量太差，根因是"逐 mesh AABB 的 min 合并"在一张
**跨度极大的 mesh**（本场景最大场边长 2800+ 单位）上失效：该 AABB 覆盖了大半个场景，其内部只有
32³ 的粗分辨率；再加上单层 128³ 的 24.66 单位体素。修复方向已明确：① clipmap 分层（近处细、
远处粗）；② 逐 mesh 场提到 128³（需要 §5 里记的 scatter 版本）；③ 大跨度网格不要与细碎网格共
用一个 min 合并，而应参与细节层。三条都记进 §12 的实现前置。

**sphere tracing 首版与实测（步骤 11，2026-09-19）**

实现：`Engine/Shader/Shaders/Lumen/SDF_RayMarch.comp.slang` + `LumenSDF::SetupMarchRays/RunMarch/
RunMarchCheck`。步进为 `t += max(d, eps)`（**没有上限步长** —— 上限就是高估，会穿漏），
`eps = 0.25 × 体素`，最大 192 步、最大距离 400 单位；法线取 SDF 梯度前向差分（3 次采样）。

验证方式：256 条确定性伪随机射线（起点落在某个 mesh 的 AABB 内，方向随机），CPU 侧用
**Möller–Trumbore 对全部三角形**求最近正交点作参考（逐 mesh AABB slab 粗筛）。

| 指标 | 实测 |
|------|------|
| 两者都命中 / 仅 GPU / 仅 CPU | 195 / 61 / **0** |
| **穿漏（仅 CPU 命中）** | **0 ⇒ 安全判据 PASS**（步进未越过任何真实交点） |
| 命中距离误差 ≤ 1 体素 | **76/195 = 39.0%**（最大 14.78、平均 3.15 体素）⇒ **精度未达标** |
| 法线朝向正确（`dot(N, dir) < 0`） | 112/195 |

**两条已知偏差的归因**（不是实现 bug，是当前表示的能力边界）：

1. **61 条"仅 GPU 命中"** = 射线起点落在几何**内部**时，**无符号**距离场立刻小于阈值，
   于是 t≈0 被记为命中。这正是"符号"缺席的表现（设计里"内外符号"本版未做）。
2. **平均 3.15 体素的距离误差**：全局场当前是**单层 24.66 单位体素**，三线性插值把表面抹平到
   数个体素，命中距离不可能优于这个尺度；`eps` 也挂在同一个粗体素上（6.17 单位）。

**达标路径（写进 §12 实现前置）**：① 符号（内外）判定 —— 消掉第 1 类假命中；② clipmap 分层，
`eps` 与命中精度取**最细层**的体素；③ 逐 mesh 场提分辨率（scatter 版本）+ 近场细节追踪
（前 2 m 用 mesh 场，对齐 UE 的 Detail Tracing），才能把"≤1 体素"这条判据真正跑通。

**细节追踪首版与"为什么它没帮上忙"（同一轮追加，2026-09-19）**

实现：`SDF_RayMarchDetail.comp.slang` —— 逐 mesh dispatch（每个 mesh 绑自己的距离场），
光线跑出该 AABB 即停，结果按 `InterlockedMin` 归约到共享的命中距离缓冲（位模式存 +inf = 未命中），
再与全局追踪结果合并。`eps` 取**该 mesh 自己的体素**（小网格可到 0.11 单位）。

| 指标 | 实测 |
|------|------|
| 细节追踪命中的射线 | 78/256（最小 t = 0.000 —— 起点在几何内部） |
| "细节追踪给出更近命中"的射线 | **0 条** |
| 合并后精度 | **与仅全局完全相同**：76/195 = 39.0%，平均 3.153 体素 |
| 试过 detail-first 语义（有细节命中就以它为准） | 75/195（**更差**），已回退到 min |

**这条"没帮上忙"本身就是本轮最有价值的结论**，它把根因收敛到两条（此前的推测被证伪）：

1. **全局场的"提前判命中"**：全局场是下界且严重低估（平均 621 单位），于是它的插值值在
   **距离表面还很远**时就低于阈值 `eps`（6.17 单位），t 比真实交点小得多 —— 取 min 时它必然胜出，
   细节追踪再准也没机会参与。
2. **无符号场让"起点在几何内部"立刻算命中**（t≈0）：细节追踪里这一条尤其明显（78 条命中里
   最小 t 就是 0）。

⇒ 修复顺序被明确为：**先做符号判定（步骤 11.5）**，再谈细节追踪的收益；否则无论全局还是逐 mesh
追踪，都会在"哪里算表面"这一步就错。原先"先提分辨率/先 clipmap"的排序因此调整（§12 前置已更新）。

**符号（内外）首版与实测（2026-09-19）**

实现：`SDF_MeshBuild.comp.slang` 在同一个三角形循环里顺带做 **parity**（沿 +X 射线与三角形求交，
在 YZ 平面用二维重心坐标解出交点 X，奇数个交点 = 内部），写出**带符号**距离；细节追踪改为带符号
语义（步进用 `|d|`、命中判定用 `|d| < eps`），于是"起点在几何内部"的射线会走到最近的表面，
而不再在 t=0 自认为命中。全局注入显式取 `abs(d)` —— 逐 mesh 场带符号后，负值直接 `asuint` 会
变成极大的无符号数被 `InterlockedMin` 静默忽略。

| 指标 | 实测 |
|------|------|
| 距离精度（512 探针） | 最大误差 **7.5e-5**（阈值 1/4 体素）⇒ 不变 |
| **符号一致率**（GPU=parity ↔ CPU=最近三角形法线，两种算法互相验证） | **390/512 = 76.2%** |
| sphere tracing | 与符号前**逐位相同**（195/61/76、平均 3.153）；细节追踪"更近命中"仍为 **0 条** |

**为什么只有 76.2%：这是几何本身的性质，不是实现错误。** 本场景（Sponza 类）的 mesh 大多是
**开放曲面**（墙、地板、天花板，双面可见的单层几何）——对开放曲面而言"内部/外部"**没有定义**，
两种符号算法（parity 与最近三角形法线）在它上面必然分歧；UE 的官方文档也正是因此要求
"墙/地/天花板拆成独立 mesh、需闭合几何、单面几何会被看穿"（§17.6 对齐清单第 3 项）。

**结论（下一轮的输入）**：符号已具备，但它**没有改善命中精度**——因为当前全局场是**无符号且严重
低估**的下界，取 min 合并时它的"提前判命中"必然胜出（细节追踪更近命中 0 条）。⇒ 下一步不再是
继续调全局层，而是**让细节层成为主命中山**：提高逐 mesh 分辨率（先落 scatter + `InterlockedMin`
以支撑 128³），使细节层本身紧到能承担命中判定；全局层退化为"远处兜底"。

### 6. Screen Probe Gather
| 配置项 | 值 |
|--------|----|
| 探针网格间距 | 16×16 pixels (1920×1080 → ~8K 探针) |
| 自适应合并 | 平坦区域 (法线方差 < 阈值) 合并为 32×32 |
| 光线/探针 | 8-16 (GGX 重要性采样, 半球分布) |
| 半球追踪 | 近场 SDF + 远场 HW RT (MaxSDFTrace = 50m) |
| 命中点材质 | 采样 Surface Cache Atalas |
| 直接光照 | 命中点查询 ClusteredShading LightGrid |
| 空间滤波 | 3×3 YCoCg AABB 裁剪 |
| 时间滤波 | EMA (α=0.2) 混合历史帧 |
| SH 输出 | 二阶球谐 (4 coeffs RGB) = 12 floats/probe |

**追踪表示 × 着色表示（2026-09-19 修正：原文把两者压成了一维距离分支）**

原文用"`rayDistance < MaxSDFTraceDistance`（50 m）走 SDF，否则走 HW RT"这一条分支，同时决定了
**在哪求交**与**命中点怎么着色**。但这两件事是正交的 —— UE5 的 Lumen 就是分开选的
（`trace ∈ {Screen, SDF, HW}` × `shade ∈ {Surface Cache, Hit Lighting}`，见
[Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine?application_version=5.6)）。
压成一维的代价是 **Hit Lighting 被架构性排除**，而"高质量镜面反射只能靠 Hit Lighting"（官方文档
原话）—— 以后要加就得动整条数据流。因此本设计改为二维显式建模：

```cpp
enum class LumenTraceRep : u8 { Screen, SDF, HardwareRT };      // 在哪求交
enum class LumenShadeRep : u8 { SurfaceCache, HitLighting };     // 命中点怎么着色

struct LumenTraceConfig {
    LumenTraceRep traceRep       = LumenTraceRep::SDF;   // 首版：SDF（近场）+ HW（远场）混合
    LumenShadeRep shadeRep       = LumenShadeRep::SurfaceCache;
    bool          screenTrace    = false;    // 屏幕轨迹优先（弥合"两套表示"的偏差，见 §16）
    bool          farFieldHW     = true;     // 近场 SDF / 远场 HW 的混合开关
    float         maxSDFDistance = 50.0f;    // 原 MaxSDFTraceDistance
};
```

**组合约束（必须写进实现，不是可选建议）**：

| 组合 | 是否允许 | 原因 |
|------|---------|------|
| `SDF × SurfaceCache` | ✅ 首版默认 | SDF 命中没有真实三角形，只能读卡片 |
| `HW(RT) × SurfaceCache` | ✅ | 命中三角形但按卡片着色（省算力） |
| `HW(RT) × HitLighting` | ✅（第二版） | 命中点跑材质求值 + NEE 直接光 —— 镜面质量的来源 |
| `SDF × HitLighting` | ❌ | 没有三角形/材质可求值，语义不成立 |
| `Screen × 任意` | ✅（作为**优先层**） | 屏幕命中直接复用 GBuffer 着色，未命中再回落 `traceRep` |

**首版范围**：只实现 `SDF（+ HW 远场混合） × SurfaceCache`，`screenTrace = false`、不做
Hit Lighting；但**接口与配置按上表写全**。这样第二版加 Hit Lighting / Screen Trace 是"加一条
分支"，而不是重构数据流与 pass 结构。

> 副作用（正面）：§14 里"SDF ↔ HW 切换 discontinuity"这条风险随之收敛 —— overlap fade 落在
> `traceRep` 的切换逻辑内，与 `shadeRep` 无关，不会再出现"两种表示 × 两种着色"的四种组合各自
> 需要一套过渡策略。

### 7. Radiance Cache（升级 DDGI）

| 变更 | DDGI (当前) | Radiance Cache (目标) |
|------|------------|----------------------|
| 探针表示 | SH 三波段 (9 coeffs) | SH 二阶 (4 coeffs)，RGB 独立 |
| 探针分布 | 均匀 3D 网格 | 自适应密度 (基于几何复杂度) |
| 追踪方式 | Fibonacci 球面采样 GBuffer | Screen Probe Gather 输入 |
| 时间混合 | 指数移动平均 | History 重投影 + 时间混合 |
| 插值 | 三线性探针插值 | 三线性 + 距离权重 |

> DDGI 的现役实现位置见 §9.1（`GI_DDGI` / `DDGIProvider` / `DDGITracePass` / `GIProbeGrid`）。

> **已知降级（2026-09-19 标注，不修正）**：上表里"SH 三波段 9 系数 → 二阶 4 系数"与"均匀 3D
> 网格 → 自适应密度（未实现）"是**表示能力上的降级**，不是实现细节：复用 DDGI 省掉了整套
> GPU 探针分配 / 失效 / 压实系统（这是划算的），代价是 Radiance Cache 会更糊、有网格伪影、
> 屏外与大范围 GI 受限。这条降级写在这里是为了**避免后来者把它当 bug 修**：要真正解决必须
> 换成显式分配的探针缓存，属于 §17 里"明确不抄"的范畴（本轮不做）。

### 8. 与现有管线集成（DeferredPipeline 扩展、新增 Shader 文件）

**DeferredPipeline 扩展** —— `BuildFrameGraph` 新增 Pass：

```
BuildFrameGraph 新增 Pass:
    [Lumen]
    GPU_Cull → Shadow → SurfaceCache_Update → SDF_Update →
    GBuffer → ScreenProbeGather → SpatialFilter → TemporalFilter →
    RadianceCache_Update → Lighting (读 RadianceCache + SurfaceCache)
    
    [Nanite (Phase 1)]  ← 属 Nanite，见 Nanite设计与实现.md
    GPU_Cull → Nanite_ClusterCull → Nanite_LODSelect →
    Nanite_Rasterize(GBuffer) → Lighting → 后处理链
```

**新 Shader 文件**：

```
Engine/Shader/Shaders/
    Lumen/
        SurfaceCache_Capture.comp        Card 软件光栅到 Atalas
        SurfaceCache_Feedback.comp       检测缺失页面
        SDF_MeshBuild.comp              从三角形构建 Mesh SDF
        SDF_GlobalInject.comp           Mesh SDF 注入 Global SDF
        SDF_RayMarch.comp               SDF Ray Marching
        ScreenProbeGather.comp          屏幕探针半球追踪
        ScreenProbe_Filter.comp         空间 + 时间滤波
        ScreenProbe_SHProject.comp      SH 投影

    Nanite/                             ← 属 Nanite，见 Nanite设计与实现.md
        Nanite_Preprocess.py            Python 预处理工具
        Nanite_InstanceCull.comp        实例视锥 + Hi-Z 剔除
        Nanite_ClusterCull.comp         两阶段 Cluster 剔除
        Nanite_LODSelect.comp           LOD 选择
        Nanite_SoftRasterize.comp       Compute Shader 软光栅
        Nanite.mesh                     Mesh Shader 硬光栅
```

> `Engine/Shader/Shaders/Lumen/` 目录当前**不存在**（§9.2 的核验结果）。

---

## 二、实现

### 9. 已落地现状与落地位置

本章每一项都在仓库中实际检索过。**已落地**给 `文件:行号` 或符号名；**未落地**给检索关键词与
0 命中说明。检索范围默认为 `Engine/`（含 RHI-free 与 shader 源）。

#### 9.1 已落地（Lumen 将复用的 GI / RT / 降噪基座）

| 能力 | 载体（实证） | 关键符号 / 说明 |
|------|-------------|----------------|
| GI 源统一抽象 | `Engine/Render/GI/IGIProvider.h:47` | `class IGIProvider`；`GIProviderContext` 在 `:29`；`GIPassKind{Offscreen,Compute,Custom}` 在 `:50`；`Handles()` `:65`、`NeedsPass()` `:78`、`NeedsRadianceHistory()` `:92`、`GetAuxPassCount()/GetAuxPassName()/GetAuxPassOutput()/GetAuxPassInput()/RenderAux()` `:111`–`:118` |
| 通道路径栈与合成契约 | `Engine/Render/GI/GITypes.h:193` | `struct GIChannelStack` |
| DDGI 探针 pass | `Engine/Render/GI/GI_DDGI.h:21` | `class GI_DDGI : public IGlobalIllumination`（实现 `GI_DDGI.cpp`）；**不存在**名为 `DDGIPass` 的文件 |
| DDGI Provider | `Engine/Render/GI/DDGIProvider.h:21` | `class DDGIProvider final : public IGIProvider`；`GetPassKind()==GIPassKind::Compute`（`:41`）、`HasTextureOutput()==false`（`:48`，产物是探针缓冲，由 shader 的 `SampleDDGI()` 直接读） |
| DDGI 探针网格拟合 | `Engine/Render/GI/GIProbeGrid.h:50` | `FitProbeGridToBounds(...)`（RHI-free、纯几何、有单测） |
| DDGI 的 HW RT 追踪 | `Engine/Render/GI/DDGITracePass.h:25` | `class DDGITracePass : public RTEffectPass`（实现 `DDGITracePass.cpp`）；**不存在**名为 `DDGIProbe` 的符号 |
| DDGI shader | `Engine/Shader/Shaders/RT_DDGI.slang`、`GI/DDGI.comp.slang`、`RayTracing/DDGI_Trace.rgen.slang`；采样端 `Lighting/DeferredLighting.frag.slang`（`u_DDGIProbes`，`kGPUBinding_DDGIProbes=22`，见 `ShaderTypes.slang:87`） | — |
| RT GI pass | `Engine/Render/RT/RTGIPass.h:18` | `class RTGIPass : public RTEffectPass`（实现 `RT/RTGIPass.cpp`）；shader `RayTracing/RT_GI.rgen.slang`（`:25` 有 DDGI 探针 miss 回退）、`RT_GI.rchit.slang`、`RT_GI.rmiss.slang` |
| RT 效果 Provider（含降噪链数据化） | `Engine/Render/GI/RTProvider.h:31`、`:266` | `class RTEffectProvider`；`std::vector<Stage> m_Stages;   // 降噪链（顺序即执行顺序）` |
| 空间降噪器（5×5 双边） | `Engine/Render/PostProcess/Denoiser.h:17` | `class Denoiser`；`kDefaultDepthSigma=10.0f` `:20`、`kDefaultNormalSigma=8.0f` `:21`；参数已可配 `m_DepthSigma/m_NormalSigma` `:49`/`:50` |
| 时域降噪器（时域累积） | `Engine/Render/PostProcess/RTDenoiser.h:22` | `class RTDenoiser` |
| 降噪附属 pass 去重实现 | `Engine/Render/GI/SpatialDenoiseAux.h:23` | `class SpatialDenoiseAux`（"主输出 → 空间降噪"附属 pass 的共享实现，`Count(bool)` `:29`） |
| 降噪器实例计数 = 源文档的 "9 个实例" | `Engine/Render/Pipeline/DeferredPipeline.h:249`–`:258` + `Engine/Render/Pipeline/PathTracingPipeline.h:113` | `RTDenoiser` ×4（`m_ShadowDenoiser`/`m_AODenoiser`/`m_ReflectionDenoiser`/`m_GIDenoiser`，赋值在 `DeferredPipeline.cpp:262/274/286/302`）+ `Denoiser` ×4（`m_ReflectionSpatial`/`m_GISpatial`/`m_DenoiseSSGI`/`m_DenoiseSSR`）；PT 侧 `m_PTDenoiser`（`RTDenoiser`，`PathTracingPipeline.cpp:130`）⇒ `RTDenoiser` 共 **5** 个、`Denoiser` 共 **4** 个 |
| 降噪参数集中按信号赋值（11.1 的"参数可配"） | `Engine/Render/Pipeline/DeferredPipeline.cpp:139`–`:154` | 注释明确"此前 4 个 Denoiser 实例的参数完全相同（着色器里的固定常量 10 / 8）……现在参数可配，并且**集中在这一处**按信号赋值"；当前仍取默认 `kSpatialDepthSigma/kSpatialNormalSigma` |
| RT 降噪链名与序（11.2 的判据） | `Engine/Render/GI/RTProvider.h:42`–`:63` | `Stage::Temporal(pass,"RT_Shadow_Denoise")` `:45`；`"RT_Reflection_Temporal"` `:55` → `"RT_Reflection_Denoise"` `:56`；`"RT_GI_Temporal"` `:61` → `"RT_GI_Denoise"` `:62` |
| GI Provider 注册表 | `Engine/Render/Pipeline/DeferredPipeline.cpp:168`–`:208` | AO `:171`、IBL `:177`、RSM `:183`、SSGI `:189`（`SetDenoiser(&m_DenoiseSSGI)`）、SSR `:195`、DDGI `:201` |
| RT 效果 Provider 注册 | `Engine/Render/Pipeline/DeferredPipeline.cpp:316`–`:342` | 四种效果（Shadow/AO/Reflection/GI）共用一个参数化 Provider 实现 `RTEffectProvider` |
| 帧图按 source id 的定制循环（**7 条**） | `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp` | RSM `:426`、DDGI `:570`、AO `:593`（`SSAO`/`GTAO`）、SSR `:621`、SSGI `:671`、RT `:739`（含 `dynamic_cast<RTEffectProvider*>` 与 AS/SBT 选择）、IBL `:839` ⇒ 正好 7 条，与 §11 "现状"一致 |
| ReSTIR DI（附录 A 的复用对象） | `Engine/Render/RT/ReSTIRPass.h:41`、`Engine/Render/RT/ReSTIRPass.cpp:164` | 三份 SSBO 蓄水池（`Initial` / `Temporal` 双缓冲 / `Final`）；`PathTracingPipeline.cpp:451` 接 `ctx.finalReservoir` |
| 蓄水池 GPU 结构 | `Engine/Shader/Shaders/ShaderTypes.slang:425` | `GPU_STRUCT PTReservoir`（32 B；`lightIndex` / `weightSum` / `M` / `W` / `lightPos(float4)`）；`Engine/Render/Pipeline/Material.h:59` 有 `static_assert(sizeof(PTReservoir)==32)` |
| 场景材质/法线纹理 | `Engine/Render/Pipeline/RTPass.h:82`、`RTPass.cpp:527` | `RTPass::BuildSceneMaterialTexture`；材质纹理 **11 行 × N 列**（`RTPass.cpp:542`–`:548` 逐行列出 row0–row10） |
| PT 无状态确定性随机数 | `Engine/Shader/Shaders/PT_Common.slang:57` `Rand(uint2 idx, uint frame, uint s)`、`:69` `StratifiedJitter(...)` | 附录 A §A.4.2 第 2 条的依据 |

其余源文档 §2 列出的基础设施（`VK_EXT_mesh_shader`、GPU Culling、device-generated commands、
WorkGraph 模拟、meshoptimizer 等）本轮**未逐项核验**，按源文档表格原样保留；其中标注"属 Nanite"
的能力归 `Nanite设计与实现.md`。

#### 9.2 未落地（Lumen 本体）

| 检索关键词 | 检索范围 | 结果 |
|-----------|---------|------|
| `ScreenProbe` | `Engine/` | **0 命中** |
| `SurfaceCache` / `Surface_Cache` / `SurfaceCachePage` | `Engine/` | **0 命中** |
| `MaxSDF` / `RayMarching` / `RayMarch`（覆盖 `SDF_RayMarch.comp`、`MaxSDFTrace`） | `Engine/` | **0 命中** |
| `Engine/Shader/Shaders/Lumen/` 目录 | `Engine/Shader/Shaders/` | **不存在**（112 个 shader 路径中无 Lumen 子目录） |
| `Lumen` | `Engine/` | 仅 **1 处注释**：`Engine/Render/GI/SpatialDenoiseAux.h:14`（该注释指向《Lumen设计与实现》§10，即本文档；随本次目录整理同步改指）；除该注释外无任何实现符号 |
| `DenoiseSignal`（11.3 的信号分派） | `Engine/` | **0 命中** ⇒ §10 的 11.3 未落地 |
| `needsUpscale`（11.3 的"半分辨率也要降噪"） | `Engine/` | **0 命中** ⇒ 同上 |

**结论**：Lumen 的 **Surface Cache（§4）、SDF 体系（§5）、Screen Probe Gather（§6）、
Radiance Cache（§7）四块本体全部未落地**，§8 列出的 `Lumen/` shader 文件也一个都不存在。
仓库里现有的是**可复用的 GI / RT / 降噪基座**（§9.1）。

> 检索口径说明：单独检索 `SDF` 会得到大量命中，但它们全部是噪音 —— 一类是 `BSDF` 的**子串**
> 命中（`PT_Common.slang`、`DeferredLighting.frag.slang` 等），另一类是第三方库
> `Engine/External/stb`、`Engine/External/JoltPhysics/.../stb_truetype.h` 的字体 SDF
> （`stbtt_GetGlyphSDF`），与全局光照无关。因此本表使用 `MaxSDF` / `RayMarch` 这类**不会被
> `BSDF` 命中**的关键词。
>
> 另注（代码注释滞后，不是设计问题）：`Engine/Render/Pipeline/RTPass.cpp:511` 的函数头注释写
> "场景材质纹理（7×N）"，而同函数 `:542` 与 `Engine/Shader/Shaders/PathTracing/PT_Full.rchit.slang:17`
> 都写 **11 行**。以实现为准（11 行），附录 A §A.3 表中的"材质纹理 11 行"与此一致。

### 10. 待落地：统一降噪框架

> 本节内容原为《Lumen 与 Nanite 完整设计规范》§5.1，自《HugEngine GI 架构与开发计划》迁入。
>
> 这项工作原本挂在 GI 计划的任务 11（统一降噪框架）下，但它**真正的消费方是本项目的
> Lumen**（多信号共存：Screen Probe Gather / Radiance Cache / 反射 / 阴影 / 探针）。
> 因此任务与它的状态一并迁到这里，GI 计划只保留设计与现状对照（该文档 §4.4）作为背景。
> **本节的判据与证据原样保留**，不要因为换了文档就把它当成"未做过的事"。
>
> 本节的实现现状由 §9.1 的代码实证佐证（`Denoiser` ×4 / `RTDenoiser` ×5、
> `SpatialDenoiseAux.h:23`、`RTProvider.h:266` 的 `std::vector<Stage>`、
> `DeferredPipeline.cpp:139`–`:154` 的集中赋值）。

**为什么必须做（现状）**：每个 Provider 自带降噪 —— `Denoiser`（空间 5×5 双边）×4 与
`RTDenoiser`（时域累积）×5 = **9 个实例、9 套 PSO、14 张纹理**；两个类的输入签名与参数机制
互不相同，**降噪器之间不组合**，链条形状由调用方的 `if (IsTemporalIndex(i))` 位置约定表达。
接 Lumen 时会立刻撞上两件事：① 多信号共存时没有统一的信号分类与历史分配；② 有效性
（`alpha < 0`）协议在四处断裂（GI 计划 §9.2-C 就是它断出来的缺陷）。

**三步走（每步独立可提交、独立可回退）**：

| 步骤 | 内容 | 状态 |
|---|---|---|
| **11.1 去重 + 参数可配** | `SSGIProvider` / `SSRProvider` 里逐行同构的附属 pass 合并为一份实现（`GI/SpatialDenoiseAux.h`）；`Denoiser` 的 `depthSigma` / `normalSigma` 从"着色器里的固定常量"变成可配，并集中在管线的一处按信号赋值 | ✅ **已完成** |
| **11.2 链条数据化** | `RTProvider` 用 `std::vector<Stage>` 取代 `m_Temporal` + `m_Spatial` 两个指针与"索引 0 是时域、1 是空间"的位置约定；pass 链的枚举/输入输出/PreBind/Render 全部改为遍历该向量 | ✅ **已完成** |
| **11.3 按信号类型分派** | 引入 `DenoiseSignal`；统一分配历史纹理与采样器；支持把多个信号批量 dispatch；把"有效性（`alpha < 0`）"提升为**框架级契约** | ⏳ **待做（需要本项目的消费方）** |

**11.1 的判据与实测**（背靠背单源采样逐项一致）：用改前/改后两个可执行文件、每次运行一份
**私有 cfg 副本**（示例程序退出时会回写 cfg，复用同一文件会把配置差异误读成代码差异 ——
本轮第一次 A/B 就因此得到 −2% 的假差异）对照：`ssgi` 变体 **−0.0009%**、`both` 变体
**+0.0002%**，都在实测抖动内；白炉 1.0000、单测全绿。
**参数取值仍是默认 10 / 8 是实测结论、不是漏改**：把 SSGI 放宽到 2 / 2 后高频代理 `mean|Δx|`
只从 0.000984 降到 0.000935（−5%），整体 `std/mean` 反而不变，HDR 偏移 −1.5% ⇒ **瓶颈是
5×5 的核本身与缺少时域累积**，这直接指导 11.3 往"更大的核 / 时域"走，而不是调权重。

**11.2 的判据与实测**：三种 RT 效果的 pass 链与改造前**逐个同名同序**（RTGI →
`RT_GI_Temporal` → `RT_GI_Denoise`；RT 反射 → `RT_Reflection_Temporal` →
`RT_Reflection_Denoise`；RT 阴影 → `RT_Shadow_Denoise`）；`rtgi_coupling_check` 0.000% PASS；
三变体读数与白炉不变；**"加一级只需 push"当场演示** —— 临时给 RTGI 多 push 一个 stage，
pass 列表立刻多出 `RT_GI_Denoise_Third`，框架代码一行未改（演示后已还原）。

**11.3 的验收判据（待做，供实现时照抄）**：

- 抽象选型只有在**真实的多信号共存场景**下才能验收（Lumen 的 Screen Probe / Radiance Cache
  与既有 GI/反射/阴影信号同帧）——没有消费方的泛化不算验收；
- 框架级有效性契约：任何信号的降噪输出必须统一表达"本条无效"（`alpha < 0`），且合成端
  只需读这一个约定；
- **半分辨率也要降噪**：现在 `SSGIProvider::AuxActive()` 在 `halfRes` 时返回 false，半分辨率
  输出被直接采样；根治需要 `needsUpscale`（重建升采样）这一信号属性；
- 与 L6 里程碑的关系：L6 = 时间混合 + 空间滤波 + 异步 Compute，**统一降噪框架是它的前置**，
  否则 L6 会退化成"再挂一套 Lumen 专用降噪器"。

> **设计细节与现状逐项对照**见《HugEngine GI 架构与开发计划》的 **§4.4**（那一节保留在 GI
> 文档里：它对比的是 GI 各 Provider 现有降噪器的接口/参数/链条，是这份计划的输入）。

### 11. 待落地：Provider 执行单位收敛与绑定数组化（P6，Lumen 框架前置）

> 本节内容原为《Lumen 与 Nanite 完整设计规范》§5.2，自 GI 计划迁入。
>
> 这两项原本挂在 GI 计划的任务 19 / 20 下。它们的验收对象**只有 P6 与 Lumen** ——
> 没有消费方的泛化无法验收（GI 计划 §4.3.5 的"三层改造"是它们的设计背景，那节留在
> GI 文档里）。因此任务与状态一并迁到这里：**GI 计划只保留设计，不再列任务**。

**19 · PROVIDER-EXEC：执行单位从「Provider × 通道」改为「Provider」**

- 目标：帧图的执行单位变成"一个 Provider 每帧只注册一次 pass"，即使它的源 id 同时出现在
  多个通道的层栈里 —— **这正是 Lumen 的天然形状**（一份估计量同时喂漫反射与镜面）。
- 现状：GI 计划的帧图有 7 条按 source id 定制的循环；一个 Provider 若同时 `Handles(SSGI)`
  与 `Handles(SSR)`，会被两条循环各跑一遍。
  （§9.1 的实证：`DeferredPipeline_FrameGraph.cpp` 的 7 条循环 —— RSM `:426`、DDGI `:570`、
  AO `:593`、SSR `:621`、SSGI `:671`、RT `:739`、IBL `:839`。）
- 做法：循环体按**已被声明却零消费的 `GetPassKind()`** 选择 pass 形状
  （`Offscreen` / `Compute` / `Custom`），输出按 `GetDiffuse/Specular/AOOutput()` 落到对应
  通道。**无需新增接口方法** —— `Handles()` 已支持"一个 Provider 属于多个 id"（SSAO/GTAO 是先例）。
- **迁移策略（逐类，每步可独立验证）**：
  1. **先只合并 specular 与 diffuse 两条循环** —— 形状几乎完全相同（都读 depth+normal+
     albedo、都有附属降噪链、都以 `GetFinalXOutput()` 收尾），而且**恰好就是 Lumen 需要
     共享的那一对通道**；改动最小、可独立回退；
  2. **AO 循环暂不动**：它形状不同（不读 albedo、半分辨率尺寸处理不同），且存在"输出绕过
     Provider 直接进 Lighting"的旁路（GI 计划 §4.3.2 的实例），要先归位旁路；
  3. `Compute`（DDGI）与 `Custom`（IBL）暂留原样，最后收；
  4. RT 循环因涉及 `dynamic_cast<RTEffectProvider*>` 与 AS/SBT，**留到最后或不动**。
- **验收判据**：同一环境下**背靠背**的单源采样逐项一致（pass 集合与顺序、各 `provN_raw/final`
  纹理数值）+ 白炉 1.0000 + 单元测试全绿（GI 计划 §11.3.1 的测量异常已修复，绝对量级也可复现）。
- **逃生口（重要）**：本项**不是**解锁 Lumen 的必要条件 —— 随时可以像 IBL/RSM/RT 那样给
  Lumen 加**第 8 条定制循环**，它的 pass 照样只注册一次、不存在双跑。故本项是**可维护性投资**；
  若优先级不足，可推迟到真正接 Lumen 或 P6 之前。
- 风险：中 —— 帧图是核心路径；对策是逐类迁移 + 每步背靠背判据（GI 计划 §11.4）。

**20 · P6 · ReSTIR GI 统一估计器 + 纹理绑定数组化 / 跨通道共享**

- 内容：① **P6**：把 GI 的估计器统一成 ReSTIR 形式（GI 计划 §4.3.5 第三层要的"统一估计器"）；
  ② **纹理绑定数组化**：`LightingInputs` 的具名字段 + shader 的 per-id `case` 换成"每通道
  一组源纹理、按槽位索引取样"（UBO + 描述符 + shader + 合成循环一起改）。
- 为什么两者必须同时做：第三层的泛化**需要 P6 或 Lumen 作为验收对象** —— 只做数组化而没有
  第二个真实估计器，就是把"猜出来的槽位数量"当成设计；只做 P6 而不数组化，Lumen 与 P6 就会
  各自以特例形式落地（每个都加一条定制循环 + 一套具名绑定）。
- 验收判据（实现时照抄）：① 至少两个真实估计器（例如 P6 与 DDGI/SSGI）以**同一套槽位机制**
  共存并各自可单独关闭；② 合成端不再认识任何源 id（只认槽位）；③ 背靠背单源采样逐项一致 +
  白炉 1.0000 + 单测全绿。
- 状态：**长期项**，未开始；它的位置在 Lumen（L1–L5）之后与 P6 一起，见 §12 的推进顺序。
- **落点确认（2026-09-19）**：`全路径追踪管线规划.md` §12 B7（ReSTIR PT / GRIS）的落点**确定归本项的 P6**（PT 侧不实现，只保留"薄适配"接口：PT 的 RayGen 采样共享内核输出的 radiance + validity 纹理）。该决策的依据、代价评估（实测成本表 / 显存 / 冷缓存编译）与执行里程碑（M1 GRIS-lite → M2 完整 GRIS → M3 与 DI/P6 统一）见**本文附录 A**（含 §A.8 通用性分析：哪些层可共用、哪些必须各写一套）。

> **与"统一降噪框架"（§10）的关系**：两者都是"多信号共存"的框架前置 —— §10 管降噪的
> 信号分类与历史分配，本节管**执行单位与绑定**。Lumen 接入时会同时撞上这两件，故建议
> 一起设计、分步提交。

### 12. 里程碑总表

#### Lumen

| 里程碑 | 内容 | 验证标准 |
|--------|------|----------|
| **L1: SDF** | Mesh SDF 生成 + Global SDF + SDF Ray Marching | 可视化 SDF 追踪结果 |
| **L2: Surface Cache** | Card Capture + Page Table + Feedback | Atalas 正确显示材质 |
| **L3: Screen Probe** | 探针放置 + SDF 追踪 + SH 投影 | 半球追踪产生漫反射 GI |
| **L4: HW RT 远场** | 远场切换 HW RT + 混合追踪 | SDF 近 + RT 远正确混合 |
| **L5: Radiance Cache** | 升级 DDGI → 二阶 SH + 自适应密度 | 室内/室外稳定 GI |
| **L6: 降噪+优化** | 时间混合 + 空间滤波 + 异步 Compute（**前置：§10 的统一降噪框架**） | 60fps @ 1080p |

#### 建议推进顺序

```
N1(预处理) → N2(剔除) → N3(软光栅 GBuffer) → L1(SDF) → L2(SurfaceCache)
→ L3(ScreenProbe) → N4(硬光栅) → L4(HW RT远场) → L5(RadianceCache)
→ §10 统一降噪框架（11.3） → L6+N5+N6(优化)
→ §11 框架前置（Provider 执行单位 / 绑定数组化）+ P6 统一估计器（长期）
```

先跑通 Nanite 基本渲染（N1-N3），因为它产出 GBuffer 写入能力。然后基于 Nanite 的 GBuffer 上
Lumen（L1-L3）。

> **归属说明**：本推进顺序中带 `N` 前缀的项（`N1`–`N6`）与"N1(预处理) → N2(剔除) →
> N3(软光栅 GBuffer)"、"N4(硬光栅)"、"N5+N6"均为 **Nanite 里程碑，属
> `Nanite设计与实现.md`**，此处保留是为了给出 Lumen 的插入位置；Nanite 的里程碑明细表
> （N1 预处理 / N2 上传+剔除 / N3 软光栅 / N4 硬光栅 / N5 LOD 流式 / N6 材质批次）同样归
> `Nanite设计与实现.md`，本文件不重复。

#### 架构前置（2026-09-19 补充，不新增 L 编号）

下面四项**不改变 L1–L6 的内容**，但必须随对应的 L 一起落地 —— 它们的共同特征是"后补等于重构"：

| 前置项 | 随哪个 L 落地 | 内容 | 依据 |
|--------|--------------|------|------|
| **追踪 / 着色二维解耦** | L1 定数据结构 → L4 加 Hit Lighting 分支 | §6 的 `LumenTraceRep × LumenShadeRep` 配置与组合约束（`SDF × HitLighting` 非法） | §16 架构分叉 #2 |
| **页状态机 + 每帧预算** | L2（Surface Cache） | §4 的六态状态机、`maxCapturesPerFrame` / `maxAllocationsPerFrame` / `maxFeedbackPages`，请求跨帧排队 | §16 架构分叉 #4 |
| **Provider 生命周期遍历** | L1 之前（一次性补框架） | `DeferredPipeline::OnResize` / `Shutdown` 遍历 `m_GIProviders`；atlas / clipmap 自建 + `ImportTexture` | §4 末的接口约束（`DeferredPipeline.cpp:575-594`、`:459-514` 现状不遍历） |
| **RG 显式依赖边** | L1 起，每个内部 pass | 中间 pass 显式声明 reads / writes，不依赖 `AddPass` 先后 | `RenderGraph.cpp:181-186`（LIFO 排序）、`:302-324`（dead-pass 裁剪） |

> 这四项同时也是 §15.2 差异表里"性能工程：UE 有摊销与节流、本设计没有"那一行的**最小修补**：
> 完整的摊销（预算调度、异步 compute、分级画质）仍在 L6 / §17，但状态机与预算入口从这里开始。

### 13. 关键数据结构（Lumen 相关）

#### Surface Cache Page

```cpp
struct SurfaceCachePage {
    uint3  worldCoord;        // 3D Clipmap 世界坐标
    float4 albedo[128*128];   // RGBA16F Atalas
    float4 normal[128*128];   // RGBA16F
    float4 emissive[128*128]; // RGBA16F
    uint   lastAccessFrame;   // LRU 淘汰时间戳
    bool   dirty;             // 需要重新 capture
};
```

> **补充（2026-09-19）**：上面的 `dirty` 是二值标记，撑不起 §4 的页状态机。GPU 侧的页表项与
> 请求队列建议扩展为：
>
> ```cpp
> enum class PageState : u8 { Invalid, Requested, Allocating, Capturing, Captured, Dirty };
>
> struct SurfaceCachePageEntry {   // 页表项（GPU 侧，紧凑）
>     u32 physicalPage;    // 物理页号（0xFFFFFFFF = 未分配）
>     u32 lastTouchFrame;  // LRU 依据
>     u8  state;           // PageState
>     u8  mipLevel;        // 预留：多分辨率页（首版恒 0）
>     u16 _pad;
> };
>
> struct SurfaceCacheRequest {     // Feedback 输出，逐帧消费
>     u32 pageID;
>     f32 weight;          // 屏幕覆盖面积 × 屏幕空间重要性 × 距离
> };
> ```
>
> 每帧流程：Feedback 写 `SurfaceCacheRequest[]` → C++ 侧按 `weight` 排序取前
> `maxCapturesPerFrame` 个 → 置 `Allocating/Capturing` → Capture pass 写 atlas → 置 `Captured`。
> 未进入本帧预算的请求保持 `Requested`，不回退、不丢弃。

#### Screen Probe

```cpp
struct ScreenProbe {
    float3 worldPosition;
    float3 worldNormal;
    float  viewDepth;
    float4 SH_R;              // SH 二阶 R 通道 (4 coeffs)
    float4 SH_G;              // G 通道
    float4 SH_B;              // B 通道
};
```

> 源文档第 6 节另有 `NaniteCluster`（GPU）结构，属 Nanite，见 `Nanite设计与实现.md`。
> 本节结构**均未落地**（§9.2）。

### 14. 已知风险（Lumen 相关）

| 风险 | 缓解措施 |
|------|---------|
| SDF 生成性能 | 预处理阶段完成 Mesh SDF，运行时只更新 Global SDF 注入 |
| Surface Cache Atalas 碎片化 | LRU + 定期整理 (defrag pass) |
| 两台追踪路径切换 discontinuity | SDF 最大距离结束前 N 步做 overlap fade |

> 源文档第 7 节另有两条纯 Nanite 风险（"软件光栅化效率"、"`.nanite` 格式版本兼容"），
> 归 `Nanite设计与实现.md`。

> **2026-09-19 补充**：第 3 行"SDF ↔ HW 切换 discontinuity"已随 §6 的追踪 / 着色二维解耦收敛
> （过渡逻辑只挂在 `traceRep` 上）。另新增两条风险记录，均来自 §16 的架构对照：
>
> | 新增风险 | 缓解措施 |
> |----------|---------|
> | **两套表示的偏差不可见**：本设计没有 UE 的 Screen Trace 那一层，SDF / 卡片与三角形场景不一致时（薄面、WPO、蒙皮、过期卡片）会直接表现为可见错误 | ① §6 预留 `screenTrace` 开关（首版关闭）；② 补齐"卡片覆盖率"可视化（§17 对齐清单第 1、2 项）；③ 把 UE 那套几何 / 材质限制逐条写进实现前置 |
> | **首版镜面质量受限**：`shadeRep = SurfaceCache` 下反射只能读卡片，无法做命中点材质求值 | 明确写进验收口径（首版只承诺漫反射为主的近中场景）；§6 的组合约束保证第二版加 Hit Lighting 不改数据流 |

---

## 三、与 UE5 Lumen 的架构对照（2026-09-19 补充）

> 这一章不是设计规范，而是**架构判断与决策记录**：它回答"这份设计相对 UE5 Lumen 好在哪、
> 差在哪、哪些差异现在就要补、哪些明确不抄"。
>
> 写作依据：Epic 官方文档
> [Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine?application_version=5.6)
> 与本仓库代码实证（§9.1）。UE 内部实现细节（探针间距、光线数、atlas 尺寸等）随版本变化，
> 官方文档未公开到该粒度，故本文**不做 UE 侧的数字承诺**，只在能引用处给出链接。

### 15. 能力对照

#### 15.1 骨架相同（这不是巧合 —— 本设计是对着 UE 抄的）

| 环节 | UE5 Lumen | 本设计 | 出处 |
|------|-----------|--------|------|
| 射线命中点着色 | Surface Cache（离线 Card 捕获材质） | Surface Cache（`SurfaceCache_Capture.comp`） | §4 |
| 软件追踪 | Mesh DF + Global DF 合并 | Mesh SDF + Global SDF | §5 |
| 屏幕级估计 | Screen Probe（SH、空间 + 时域滤波） | Screen Probe 16×16 px、二阶 SH | §6 |
| 世界空间缓存 | Radiance Cache | Radiance Cache（**升级现有 DDGI**） | §7 |
| 命中点直接光 | Lumen Scene 光照 | ClusteredShading LightGrid | §6 |

#### 15.2 关键差异

| 维度 | UE5 Lumen | 本设计 | 性质 |
|------|-----------|--------|------|
| 归属与边界 | Lumen Scene 是与三角形场景**平行**的表示；不兼容 Forward Shading / lightmap 静态光照 / VR | 延迟管线里的**一个 GI 源**（`IGIProvider`），与 SSGI/SSR/RTGI/DDGI 同层栈加权合成 | 架构不同，各有取舍 |
| Card 来源 | **资产构建产物**（默认 12 张/mesh，可调），Nanite 多视图光栅化加速；有粉色覆盖率视图 | 运行时 compute 软件光栅化；**卡片怎么生成、覆盖率怎么查均未写** | 工具缺失 |
| Surface Cache 组织 | 多张 atlas + 页表 + 按距离流送；**多帧摊销 + 节流** | 1024 页 × 128²、3D Clipmap 对齐 + LRU；**原文无状态机、无预算**（§4 已补） | 已补一半 |
| SDF | 资产构建产物、按距离流送；Detail Tracing（前 2 m 走 mesh DF）/ Global Tracing 可切 | 运行时生成 128³/mesh（暴力写体素）、512³ 单层 → 4×256³ clipmap | 质量与时机差异 |
| 追踪模式 | 软件 / 硬件**两套完整 tracer**，可运行时切换；硬件另有 **Hit Lighting** 与 **Far Field**（HLOD，约 1 km） | 近场 SDF + 远场 HW 的**单一混合路径**，命中统一读 Surface Cache | 已改为二维建模（§6），Hit Lighting / Far Field 仍未实现 |
| Screen Trace | **优先层**，专门弥合 Lumen Scene 与三角形场景的偏差 | 原文**没有**这个概念 | 概念缺失（§17 概念声明） |
| Screen Probe | 数十条光线/探针量级 + 自适应采样合并；多级滤波（空间→时域→空间，带 BRDF 重投影） | 8–16 条；单级 3×3 YCoCg + EMA α=0.2（**无重投影**） | 质量差异 |
| Radiance Cache | 显式分配 / 失效 / 压实的探针缓存 | 均匀 DDGI 网格；SH 由 9 系数降为 4 系数 | **已知降级**（§7 标注） |
| 合成模型 | 全局选一种 GI 方法 + Screen Trace 兜底，互斥为主 | 每通道层栈多源加权归一化 + 置信度掩码 + 降级规则 | **本设计更强** |
| 性能工程 | 多帧摊销、异步 compute、几十个 cvar、画质分级、大世界流送、多视图 | 全在主 command list；无预算 / 无流送（`AsyncCompute` 基础设施在但 GI 未用） | 规模差异 |
| 约束声明 | 官方文档有专门一节列几何 / 材质 / 工作流限制（薄墙 ≥10 cm、闭合几何、WPO 不支持等） | **原文没有这一节** | 工具缺失（§17 对齐清单第 3 项） |

#### 15.3 本设计自己的显存测算（按这个量级做预算）

| 资源 | 尺寸 | 占用 |
|------|------|------|
| Surface Cache atlas | 4096² × 3 通道 RGBA16F | ≈ **402 MB** |
| 同上，扩到设计上限 | 8192² × 3 通道 | ≈ **1.61 GB** |
| Mesh SDF | 128³ × R16F / mesh | ≈ 4.2 MB/mesh（100 个 mesh ≈ 420 MB） |
| Global SDF 单层 | 512³ × R16F | ≈ 268 MB |
| Global SDF clipmap | 4 × 256³ × R16F | ≈ 134 MB |
| Screen Probe SH | 8K 探针 × 12 floats | ≈ 0.4 MB（可忽略） |

参考基线（本机实测，见附录 A 的成本表）：桌面空闲 1893~1897 MiB；05.Sponza-PathTracing 1080p
跑到 3528 MiB。⇒ §4 里"Atalas 可扩展到 8192²"应标注为**受显存预算限制、默认不启用**，
首版按 4096²（402 MB）走；Mesh SDF 的 128³/mesh 也必须带"网格数量上限"或按需流送，
否则一百个 mesh 就把显存吃光。

### 16. 架构判断（结论）

#### 16.1 两句话结论

- **比能力上限：UE 的架构明显更好，且是量级差别。** 它多出两条本设计**根本缺失的概念** ——
  ①"两套表示 + Screen Trace 弥合"；②"持久化 + 多帧摊销"。这不是工程量问题。
- **比本项目适配：这份设计更适合。** 一个人、单 GPU、已有成套 GI 基座、要求每步可验收 ——
  在这组约束下，它的组合模型与分解方式确实比 UE 那套更合用。

#### 16.2 本设计**确实比 UE 好**的四处

1. **GI 源的组合模型**：每通道一条加权层栈（`GITypes.h:193` `GIChannelStack`），多个估计器
   同帧共存、按权重归一化合成，还有置信度掩码（`kGIConfCameraCoverage` / `kGIConfProbeGrid`）
   与降级规则（`GIRegistry::Degrade` `:643`）。UE 侧是"全局选一种方法 + Screen Trace 兜底"，
   没有等价的公开机制。表达力与可测试性都更强。
2. **可验收性是一等公民**：白炉真值 `1.0000`、单源背靠背逐像素对照、`needsPass/IsValid`
   把"本帧产出了吗"变成显式契约（`DeferredPipeline_FrameGraph.cpp:944-949` 的门控注释就是
   这类事故的产物）。UE 不需要写这些（有画面评审与出货压力），但自研引擎**最值钱**。
3. **增量可回退**：§11 明写"逃生口"—— 随时可以像 IBL / RSM / RT 一样加第 8 条定制循环，
   不被框架阻塞；L1–L6 每步都能单独提交、单独回退、单独出画面。UE 的 Lumen 是必须整体
   成形的系统（Lumen Scene / Radiance Cache / Reflections / Translucency 相互咬合）。
4. **复用而不是重建**：Radiance Cache 升级 `GI_DDGI`、降噪链抄 `std::vector<Stage>`、
   追踪走现有 `RTPass` 的 AS / SBT / bindless。这些在 UE 里都是独立子系统。

#### 16.3 四处"架构分叉"（**不是**规模简化，是概念缺失或表示降级）

| # | 分叉 | 为什么这不是"简化" | 处置 |
|---|------|------------------|------|
| 1 | **没有"两套表示 + Screen Trace 弥合"** | UE 把 Lumen Scene 当作独立于三角形场景的表示，再用 Screen Trace 掩盖两者不一致（官方文档原话）。本设计只在命中点读卡片，**没有"不一致"这个概念** —— 薄面、WPO、蒙皮、过期卡片都会直接变成可见错误，且无兜底 | **概念声明进文档**（§6 的 `screenTrace` 开关 + §14 新增风险行），实现留待第二版 |
| 2 | **追踪表示与着色表示没有正交分解** | 原文用一条距离分支同时决定"在哪求交"和"怎么着色"，**架构性排除 Hit Lighting**（高质量镜面反射的唯一来源） | **本轮补**：§6 二维建模 + 组合约束表 |
| 3 | **世界空间缓存退化为均匀 DDGI 网格** | 省掉 GPU 探针分配 / 压实很划算，但 SH 9 → 4 系数、均匀网格是**表示能力降级**（更糊、网格伪影、屏外与大范围受限） | **标注为已知降级**（§7），不修正 |
| 4 | **没有多帧摊销与预算** | 摊销决定数据结构（页状态、脏标记、请求队列），**后补比设计进去贵得多** | **本轮补**：§4 六态状态机 + 每帧预算 + §13 页表项扩展 |

#### 16.4 属于"规模差异 / 工具缺失"，不必强行对齐

Cards 与 Mesh DF 的**构建期**产物与覆盖率可视化（工具缺失，见 §17 对齐清单）、大世界流送、
Far Field（HLOD）、多视图 / view family、平台分级、TSR 耦合、Lumen×Nanite×VSM 三方耦合。
这些在本项目的场景里收益接近 0，成本却是数量级。

### 17. 决策记录

#### 17.1 本轮补进设计的两处（已改）

| 项 | 落点 | 内容 |
|----|------|------|
| 追踪 / 着色二维解耦 | §6 | `LumenTraceRep × LumenShadeRep`、组合约束（`SDF × HitLighting` 非法）、首版范围与 `screenTrace` 预留开关 |
| Surface Cache 页状态机 + 每帧预算 | §4（结构在 §13） | 六态状态机、三项预算、按 `weight` 的捕获顺序、RG 依赖边与持久资源自建的接口约束 |

#### 17.2 随 L1/L2 一起落地的实现前置（§12）

Provider 生命周期遍历（`OnResize` / `Shutdown` 现在不遍历 `m_GIProviders`）、每个内部 pass
显式声明 RG 读写边（`RenderGraph` 的 LIFO 排序与 dead-pass 裁剪）。

#### 17.3 概念声明（首版不实现，但结构里留位置）

**"两套表示 + Screen Trace"**：本设计承认 Surface Cache / SDF 表示与三角形场景会不一致，
`screenTrace` 作为独立的优先层保留在配置里（首版 `false`）；一旦出现"画面发黑 / 漏光但
说不清原因"的案例，第一条排查路径就是打开它对比。对应地，"卡片覆盖率可视化"从"可选工具"
升级为**验收前置**。

#### 17.4 已知降级（写清代价，不修正）

- Radiance Cache = DDGI 升级 ⇒ SH 4 系数、均匀网格（§7 标注）；
- 首版镜面质量受限于 `SurfaceCache` 着色（§14 新增风险行）；
- SDF 由运行时暴力生成，质量低于构建期 DF 构建器（§15.2）——这条在 L1 就要用
  "薄墙 / 窄缝自遮挡"的测试场景量化，而不是等画面出问题。

#### 17.5 明确不抄（防止"对齐 UE"变成无底洞）

| 不抄的东西 | 理由 |
|------------|------|
| GPU 探针分配 / 失效 / 压实（显式 Radiance Cache） | 省下的是本设计最大的一块工程量；代价已按"已知降级"记账 |
| Far Field + HLOD（1 km） | 依赖世界分区 / HLOD 构建管线，本项目的场景尺度不需要 |
| 世界空间流送（Mesh DF / atlas 按距离进出） | 同上 |
| 多视图 / view family / 平台画质分级 | 单视图、本机 60 fps 目标，没有消费方 |
| TSR 依赖、Lumen×Nanite×VSM 三方耦合 | 会把复杂度引到与本功能无关的系统上 |

#### 17.6 若要对齐 UE，性价比最高的三件事（按顺序）

1. **卡片生成器 + Surface Cache 覆盖率可视化** —— 先有"能看见问题"的工具，再谈质量；
   没有它，覆盖率不足只会表现为"画面莫名发黑"。
2. **Screen Trace 优先** —— 复用现有 GBuffer / SSR 几乎零成本，专门用于弥合两套表示的偏差；
   这也是 §17.3 概念声明的落地形式。
3. **Global SDF 改为加载期 / clipmap 分层构建，并把几何约束写进实现前置**（薄墙 ≥10 cm、
   需闭合几何、WPO 不支持、超大单 mesh 表示差）—— 对照 UE 官方文档的"限制"一节逐条抄。

> 余下（Hit Lighting、Far Field、异步摊销）属于第二阶段：Hit Lighting 的入口已由 §6 的二维
> 建模留好，届时是"加一条分支"。

---

## 附录 A：ReSTIR PT / GRIS 预研（Lumen GI 的落点方案与代价评估）

> **本附录的边界（2026-09-19）**：只做 **设计细化 + 代价评估**，供"是否把 PT 转为实时主路径"这个决策用。
> 它**不改变** `全路径追踪管线规划.md` §0.1 的定位（PT = 参考渲染器），**不动** §10/§11，
> 也**不改变** §12 B 组的触发条件。本附录不含任何代码改动。
>
> **决策已下（2026-09-19）**：① ReSTIR GI/GRIS 的落点 = **P6 / Lumen 侧**（本文的 M0~M3 归那边执行，PT 侧只保留"薄适配"这一条接口要求）；② **PT 不转实时主路径** ⇒ B7 保持未做。本附录自此转为该决策的**依据与执行留档**（若将来重新评估，先看 §A.4.1 的成本表与 §A.8.6 的组合结论）。
>
> 所有数字都标了来源（代码位置 / 实测命令 / 日志），可复现；推算部分明确写"推算"。

### A.1 结论摘要

| 问题 | 结论 |
|---|---|
| 技术上能不能做？ | **能**，且缺口比预期小：PT 的随机数是**无状态确定性**的（`Rand(idx, frame, s)`），随机重放式 shift 只需存 `(源像素, 源帧)` 两个 uint，不需要逐顶点 RNG 状态 |
| 主要工作量在哪？ | 引擎**完全没有 shift / 雅可比 / 重连机件**（全库检索 0 命中）。这不是"多算几个 bounce"，而是**新增一套路径复用的数学与数据结构** |
| 主要门槛是什么？ | **显存**（完整路径蓄水池 1080p 推算 1.2~2.8 GB；实测 PT 全链路在 1080p 已占 3528 MiB、桌面基线 1897 MiB ⇒ 叠加后 4.7~6.3 GB/8 GB，**没有余量**；GRIS-lite ≈ 0.5 GB 则余量充足）+ **shift 写错即引入偏差** |
| 时间增量 | 复用 pass 本身很便宜（实测 ReSTIR DI 三个 compute 只 +0.35~0.60 ms @960×540）；成本几乎全在光线（1 个额外路径样本 ≈ 2.1 ms @b=1 / 9.4 ms @b=4） |
| 在"参考渲染器"定位下值得做吗？ | **现在不值得**。同样质量可用多 SPP 换（本附录给了换算表；实测 1440p 下 1spp×1bounce 只要 10.3 ms、1spp×4bounce 28.9 ms），而"标准答案"最怕的是**偏差**：写错 shift 的 GRIS 比慢的 PT 更糟 |
| 触发后怎么做？ | 三步：**M1 = ReSTIR GI（1 顶点重连）** → **M2 = 完整 GRIS（随机重放 + 时域/空间复用）** → **M3 = 与 DI/P6 统一**；无偏性判据用**现有迭代式 PT 当 oracle** 对照（本项目独有的便利） |
| 落点与触发（**已决，2026-09-19**） | 落点 = **P6 / Lumen 侧**（实现一份内核，PT 侧只做薄适配）；**PT 不转实时主路径** ⇒ B7 保持未做。前置条件见 §A.8.5（11.3 统一降噪框架 + P6 数组化/PROVIDER-EXEC）。 |

### A.2 现状盘点：B7 能复用什么、缺什么

| 项 | 现状 | 出处 |
|---|---|---|
| ReSTIR DI | ✅ 三个 compute（Init/Temporal/Spatial）单 RG Pass 顺序执行；蓄水池双缓冲 + 历史 depth/normal 双缓冲 | `Engine/Render/RT/ReSTIRPass.{h,cpp}`、`PathTracingPipeline.cpp` 的 `ReSTIR_DI` pass |
| 蓄水池结构 | `PTReservoir` **32 B**：`lightIndex / weightSum / M / W / lightPos(float4)` —— 只存**一个光源样本** | `ShaderTypes.slang` 的 `GPU_STRUCT PTReservoir` |
| 目标函数 | 复用 PT 第 5 UAV 的真实 albedo/metallic，`PBR_BRDF(albedo, metallic, roughness, N, V, L)·Li` | `ReSTIR_Init.comp.slang` |
| PT 路径结构 | **单 RayGen 迭代循环**（NEE + MIS + 轮盘赌 + 天空），命中信息经 112B `PathPayload` 回传；**没有逐 bounce 的 shader 分离** | `PT_Full.rgen.slang` / `PT_Full.rchit.slang` / `RT/PathPayload.h` |
| 随机数 | **无状态确定性**：`Rand(idx, frame, s)` / `RandInt(...)` / `StratifiedJitter(sampleIdx, sampleCount, frame)`，全部由 `(像素, 帧, 维度槽 s)` 决定 | `PT_Common.slang` L43~76 |
| 材质/贴图数据 | 材质纹理 **11 行**（含 `materialID`/`textureMask`/因子）、三角形法线 + UV 纹理、`PBR_BRDF` 求值端共用 | `RTPass::BuildSceneMaterialTexture`、`全路径追踪管线规划.md` §0.6 缺陷 3 |
| 对照/验证设施 | `HE_DUMP_PT` 落盘、`Tools/pt/dump_pt.ps1`、`analyze_pt.py`（`--diff/--compare/--converge`）、`HE_DUMP_MODE=deferred` | `全路径追踪管线规划.md` §12 任务 5/6 完成记录 |
| **shift / 雅可比 / 重连** | ❌ **0 命中**：`Jacobian` / `shiftMapping` / `ShiftMapping` / `reconnection` / `Reconnection` / `GRIS` / `PathReservoir` 全库（排除 `External/`）均 0 | 见 §A.9 的检索命令 |
| **路径蓄水池** | ❌ 不存在（现有蓄水池只够 DI：光源索引 + 位置 + W） | 同上 |
| **重放/重连的 RayGen 入口** | ❌ 不存在（PT 只有 `FullPT` 一条 RT 管线） | `PTPass`/`RTPass::CreateEffectPipeline` |

**一句话**：DI 那套"蓄水池 + 时域/空间复用 + 帧图接线"可以照搬；缺的是 **shift 映射（含雅可比与可见性校验）**、**路径数据的存储与压缩**、**一个能"按给定路径重放/重连"的 RayGen**。

### A.3 目标形态（一个具体设计）

#### A.3.1 两种候选形态

| | 形态 A：GRIS-lite（= ReSTIR GI 的一般化） | 形态 B：完整 GRIS（路径重采样） |
|---|---|---|
| 复用什么 | 把 bounce0 的**间接光**换成一个"首个间接顶点"样本：`(x1 位置, x0→x1 的吞吐, x1 处的 NEE 辐射度)`，复用时要**重连**（重算 x0→y1 边 + 可见性） | 复用**整条后缀路径** `[x1..xR]`；首边重连 + 更深的段用**随机重放**（同随机数重放）或存路径顶点 |
| 需要的 shift | 重连 shift（闭式雅可比） | 重连 + 重放 shift（重放需 `(源像素, 源帧)`） |
| 显存/像素 | ≈ 80 B × 3 槽（推算） | ≈ 200~450 B × 3 槽（推算，见 A.4.3） |
| 能治的病 | 间接漫反射噪声（含大光源/环境光） | 焦散、多次弹射的间接光、难采样路径 |
| 实现风险 | 中（= 现有 DI 的 1 次泛化） | 高（路径存储 + 两种 shift + 雅可比 + 可见性 + MIS 权重） |
| **建议** | **M1 做这个** | M2 再做 |

> 形态 A 与 `全路径追踪管线规划.md` §6.1「ReSTIR GI」是同一件事。**先做 A 再谈 B**：A 能把 shift 机件、数据布局、无偏性判据全部跑通，且是 B 的子集。

#### A.3.2 数据结构（形态 A，逐字段）

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
2. **重放式的随机数**：因为 `Rand` 是 `(idx, frame, s)` 的纯函数，深段重放只需 `(srcPixel, srcFrame, rngSlot)`；`rngSlot` 用现有维度槽编号约定即可，**无需逐顶点存 RNG**（这是本引擎独有的便利，见 §A.2）。
3. **重连的可见性**：`x0→x1` 一条 shadow ray；失败即丢弃该候选（GRIS 的标准做法）。
4. **雅可比**：重连 shift 的 `|∂T/∂x|` 用闭式解（GRIS 论文给出；实现时写成 `Tools/check_*` 式的可单测函数，配 doctest）。

#### A.3.3 Pass 划分与帧图插入点

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

> 注：源文档此处不一致 —— 本文 §11 的"落点确认（2026-09-19）"写明"PT 侧不实现，只保留薄适配
> 接口"，而本小节是按 **PT 帧图（`PathTracingPipeline::BuildFrameGraph`）**给出新增 pass 插入点
> 的形态设计。两者保留，不在此裁决：前者是**落点决策**，后者是**形态与代价的设计载体**
> （落点改到 P6 后，这套 pass 划分即成为 P6/Lumen 侧的形状参照）。

#### A.3.4 无偏性要点（写给实现者）

- 复用必须满足：`W_shifted = p̂(y0)·... / (M · p_shift(x))`，其中 `p_shift` 是**shift 后的 pdf**（含雅可比）；漏掉雅可比 ⇒ 系统性偏差，且在"参考渲染器"定位下是**致命**的（比慢更糟）。
- MIS/权重：时域与空间合并用 pairwise MIS（与现有 DI 的加权和保持同一套写法，避免两套约定）。
- 可见性拒绝是一种**合法的零贡献**（不引入偏差），但要在 `M` 的记账上保持一致（与 DI 的 `weightSum/M` 同样处理）。
- 目标函数必须用**当前像素**的 albedo/metallic（现有 `albedoMetallic` UAV 已经在做这件事，直接沿用）。

### A.4 代价评估（实测优先）

#### A.4.1 现有 PT 成本曲线（实机实测）

环境：`Samples/05.Sponza-PathTracing`，**960×540**（示例内置 `config.windowWidth/Height`），NVIDIA **RTX 4060 Laptop 8 GB**，驱动 595.79，
时域降噪 + A-Trous 开、ReSTIR DI 关，Sponza（105 实例）。

| `pt_spp` × `pt_bounces` | ms/帧 | FPS | 说明 |
|---:|---:|---:|---|
| 1 × 1 | **2.1** | 468~477 | 当前 `Content/Config/05_...cfg` 的取值 |
| 1 × 4 | **9.4** | 107~108 | |
| 4 × 4 | **38.4** | 26.0 | |
| 6 × 7 | **61.1** | 16.4 | 与一次 25 s 冒烟实测的 16.5 FPS 完全吻合 |

- **边际成本**（1 spp 下）：bounce 1→4 每多一个 bounce ≈ **+2.4 ms**；4 bounces 下每多一个 spp ≈ **+9.2 ms**；即"每 spp ≈ 一条 b=4 的路径链"。
- **分辨率维**（外部 `MoveWindow` 改窗口，按日志里的纹理尺寸确认实际分辨率；本机桌面为 5120×1440，故 1440p 可测）：

  | 分辨率 | ms/帧 | FPS | 峰值显存（`nvidia-smi`，含桌面 1897 MiB 基线） |
  |---|---:|---:|---:|
  | 960×540 | **2.05** | 488 | 2916 MiB（Δ1019） |
  | 1920×1080 | **5.75** | 174 | 3528 MiB（Δ1631） |
  | 2560×1421 | **10.32** | 97 | 3826 MiB（Δ1933） |
  | 2560×1421（1 spp × 4 bounce） | **28.89** | 35 | — |

  像素 ×4 而帧时只 ×2.8 ⇒ 存在与分辨率无关的每帧固定开销；显存增量含 VMA 池缓存与交换链/驱动开销，
  **不要**把它当逐像素预算用（逐像素预算仍以 §A.4.3 的设计公式为准）。
- ⚠ **不要把上面这组数外推成跨分辨率的公式**。曾写过 `ms ≈ spp × (2.1 + 2.4 × (b−1))`，它只在 960×540 拟合：
  · 在 960×540 的 4×4 上误差 <3%，`6×7` 上高估（轮盘赌截断深弹射）；
  · **跨分辨率不成立**：像素 ×7（0.52 → 3.64 MP）时 1×1 只 ×5.0、1×4 只 ×3.1（见上表）；
  · 低分辨率读数还可能被**每帧 CPU/日志开销**钳制（960×540 下 1×1 已达 488 FPS ≈ 2.05 ms/帧，而引擎每帧约 20 行日志 + 帧图重建）。
  ⇒ 结论：**报成本一律给实测表（分辨率 × spp × bounce）**，需要新档位就实测一格，不要套公式。
- 测量口径（重要，避免复现歧义）：`Content/Config/05_Sponza-PathTracing.cfg` 会被示例**回写**（实测该文件在 2026-09-19 10:59 被写过一次，内容从 `6 spp / 7 bounce` 变成 `1 spp / 1 bounce`）；因此
  **测量必须走 `HE_CFG` 私有副本**（`Tools/pt/set_cfg.py` 覆盖），脚本见 `build/verify/measure_pt_cost.ps1` / `recheck_pt_fps.ps1`。
  表里 `6×7 = 16.4 FPS` 与那次 25 s 冒烟实测的 16.5 FPS 吻合，说明**冒烟当时**的基础 cfg 是 6×7，而不是现在的 1×1。

#### A.4.2 复用 pass 的开销（实测，GRIS 复用成本的下界代理）

| 配置 | 无 ReSTIR | 有 ReSTIR DI | 增量 |
|---|---:|---:|---:|
| 1 spp × 1 bounce | 2.14 ms | 2.49 ms | **+0.35 ms** |
| 1 spp × 4 bounces | 9.37 ms | 9.97 ms | **+0.60 ms** |

⇒ **3 个 compute dispatch（含 M=16 候选的 shadow ray 与 5 邻居空间复用）在 0.52 MP 上只要 0.35~0.60 ms**。结论：**复用的调度与邻域开销可忽略，成本全在光线数量上**。GRIS 的增量因此主要看"每帧多打几条路径光线"：

- 形态 A：bounce0 的间接光从"NEE 1 条 shadow ray"变成"1 条间接光线 + 复用时的 1 条重连 ray/候选" ⇒ 粗估 **+2~5 ms @0.52 MP**（≈ +1 个 spp 当量的一部分）。
- 形态 B：每帧多一整个路径链 + 复用候选的重放 ⇒ 粗估 **+10~25 ms @0.52 MP**（把 468 FPS 基线压到 ~30~60 FPS），且随 bounce 数线性增长。

#### A.4.3 显存（推算，公式给出便于复算）

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

⇒ **实测锚点（本机 8 GB）**：960×540 全链路峰值 **2916 MiB**、1920×1080 **3528 MiB**（桌面基线 1897 MiB）。据此叠加：形态 A（1080p ≈ 0.5 GB）余量充足；形态 B（1080p 推算 1.2~2.8 GB）叠加后约 **4.7~6.3 GB / 8 GB**——不是"绝对不可行"，而是**没有余量**（驱动/桌面/其它应用都在这块卡上）。因此形态 A 仍是唯一现实的起点。压缩手段（fp16 位置/法线、`radiance` 用 RGB9E5、路径顶点按需存）能把 B 压到约一半，但**压缩本身又是一个独立的正确性风险源**（要在无偏性判据下验证）。

> 注：源文档此处不一致（同一份预研内部）—— §A.6 的 **M2 验收判据**要求"显存 ≤ 1 GB @1080p"，
> 而本节的推算给出形态 B @1080p 为 **1.2~2.8 GB**（压缩后约减半）。两者保留，不在此裁决：
> 前者是**验收门槛**，后者是**当前推算**，其差值正是 M2 需要靠压缩与实测去弥合的部分。

#### A.4.4 管线数量与编译时间（实测，含与文档数字的差异）

- 现有 RT 管线：`05` 只建 **1 条**（FullPT）；`04` 建 **5 条**（RTShadow/RTAO/RTReflection/RTGI/DDGITrace）。
- 实测创建耗时（**磁盘 PSO 缓存热**）：`05` 的 FullPT `27.405 → 27.406` ≈ **1 ms**；`04` 的每个效果管线 ≈ **2 ms**。
- **冷缓存实测（已补测）**：删掉 `build/Samples/05.Sponza-PathTracing/pipeline_cache.bin` 后重跑 ——
  `CreateRTPipelineState → CreateEffectPipeline 完成` 仍在**同一毫秒内**（≈ **1 ms**），PSO 插入次数 17 与热缓存一致，
  启动到首帧 **4.82 s（冷）vs 5.00 s（热）**，稳态帧率 482 FPS 与热缓存相同。⇒ **本机（RTX 4060 Laptop）不存在
  "RT 管线编译 20 s"的问题**，启动耗时由 Sponza 加载/解码主导。
- ⚠ 两点保留：① `全路径追踪管线规划.md` §11 记的"本机 Intel Arc 上 RT 管线编译约 20 s/个"是**另一台机器**的数字，
  在按多 PSO 规划 B7 时要按目标机器复核；② 上述"冷"是**引擎级冷缓存**（删的是引擎自己的 `pipeline_cache.bin`），
  **驱动级冷缓存未测**（NVIDIA 另有自己的着色器缓存，清理它属于机器特性、会影响其它应用，故不做）。
- 脚本：`build\verify\measure_b7_unknowns.ps1`（三段：960×540 基线 / 外部 resize 到 1920×1080 / 删缓存冷启动，测完自动恢复缓存）。

#### A.4.5 工程面清单（形态 A 的最小集合）

| 类别 | 内容 | 规模（估算） |
|---|---|---|
| Slang | 新 `PT_Indirect_Init`（可复用 `PT_Full.rgen` 加 flag 分支）+ `PT_Indirect_Temporal/Spatial.comp` + `PTIndirectReservoir` 结构 + 重连/雅可比函数 | 3~4 个文件，~600 行 |
| C++ | 新 `ReSTIRIndirectPass`（照 `ReSTIRPass` 骨架：布局/PSO/SSBO/历史纹理/`Execute`）+ 帧图 3 个 pass + CVar 6~8 个 + `PT_Render` 读蓄水池的分支 | ~700 行 |
| RHI | **无需新能力**：只用现有 SSBO + UAV + compute + RT 管线（`descriptorBindingStorageBufferUpdateAfterBind` 等已启用）；**不需要 SER**（那是 B8） | 0 |
| 工具/文档 | 无偏性对照脚本（复用 `analyze_pt.py --compare/--diff`）、收敛判据沿用任务 6 | 小 |
| 单测 | 雅可比闭式解、shift 的可见性/退化处理、reservoir 记账（`M`/`W`） | 3~5 个 doctest |

### A.5 风险与未知

1. **偏差（最高风险）**：漏雅可比、shift 后未重算几何项、可见性条件写错，都会让"标准答案"悄悄变偏。**必须**把"GRIS vs 迭代式 PT 的逐像素对照"作为准入门槛，而不是事后检查。
2. **两套估计器的语义分裂**：`ReSTIR GI` 的落点已在 `全路径追踪管线规划.md` §12 C16 归到 P6（本文 §11 任务 20）。若 PT 侧另起一套，会出现"bounce0 的直接光由 DI 负责、间接光由 PT 版 GI 负责、Lumen 侧还有 P6"的三方重叠。**触发前先定落点**。
3. **显存**：形态 B 在 1080p ≈ 1.2~2.8 GB（推算）——本机 8 GB 上大概率不可行；即使形态 A 也要与 5 个 PT UAV + 降噪历史 + 场景纹理争用。
4. **RT 着色器发散**：GRIS 让相邻像素走不同深度的路径，发散更严重；本机**未启用 SER**（B8 未做），这部分收益拿不到，实时化路线存在连带依赖。
5. **仍未知**：① 驱动级冷缓存的 RT 管线编译时间（引擎级已测 ≈1 ms，见 §A.4.4）；② `Rand` 在「跨帧重放」时的相关性（源帧与当前帧同层会否产生时空相关，需要一次专门的相关性检查——这是随机重放 shift 的正确性前提）；③ 目标机器（非本机）的 RT 编译与显存行为。
6. **收益边界**：GRIS 的价值在**实时**预算下最大。作为离线/交互式参考渲染器，现有 1 spp×b=4 只需 9.4 ms，配合时域累积 30 帧即收敛（`全路径追踪管线规划.md` §12 任务 6：p50 在帧 30 变化 0.000%），**已经够用**。

### A.6 里程碑与验收判据（触发"PT 转实时主路径"之后再执行）

| 里程碑 | 内容 | 验收判据（可执行） |
|---|---|---|
| **M0 落点决策** | 定 ReSTIR GI/GRIS 归 PT 侧还是 P6（`全路径追踪管线规划.md` §12 C16）；若归 P6，本预研的 M1/M2 转为其子任务 | 决策记录进 `全路径追踪管线规划.md` §0.1/§0.3 与本文 §11 |
| **M1 GRIS-lite（形态 A）** | shift 机件 + 1 顶点重连 + 时域/空间复用；`PT_Render` 读新蓄水池 | ① 与迭代式 PT 逐像素对照（`analyze_pt.py --compare`）：SPP 递增时偏差收敛到噪声内（无系统性偏移）；② p50 达收敛所需帧数 ≤ 现有方案；③ 帧时增量 ≤ +5 ms @0.52 MP |
| **M2 完整 GRIS（形态 B）** | 随机重放 shift + 路径存储（含压缩）+ 两种 shift 的 MIS | ① 同上无偏性判据；② 显存 ≤ 1 GB @1080p；③ 焦散/多弹射场景的噪声方差显著优于 M1（给出对照读数） |
| **M3 与 DI / P6 统一** | 明确 bounce0 直接光/间接光的责任划分；必要时并入 P6 的估计器 | 两套估计器只留一套交接语义；文档更新 |

**M1 的最小可跑集合**（若只想验证可行性）：跑通"1 像素 1 间接光线 + 时域复用 + 重连可见性"，**不做**空间复用与压缩；用 `HE_DUMP_PT` 的 hdr/albedo 两个目标做逐像素对照即可判定无偏性。

> 注：源文档此处不一致（同一份预研内部）—— 本节的标题写"触发'PT 转实时主路径'之后再执行"，
> 且 M0 仍以"定落点归 PT 侧还是 P6"的待决语气书写；而本附录开头的**决策已下（2026-09-19）**
> 与 §A.1 的"落点与触发（已决）"写明：**PT 不转实时主路径**，M0~M3 **归 P6 / Lumen 侧执行**。
> 两者保留，不在此裁决；实现时以"决策已下"为准，本节标题的触发条件视为历史表述。

### A.7 本轮明确不做（非目标）

- 不改 `全路径追踪管线规划.md` §0.1 的定位、§10/§11 的目标；
- 不实现任何 B7 代码；
- 不让 §12 B 组的触发条件"被默认满足"（B7 状态保持未做）；
- 不动 §12 的编号与既有 ✅ 记录（只在 B7 行加一条指向本附录的指针）。
- 测量脚本留在 `build/verify/`（不纳入仓库）：`measure_pt_cost.ps1`、`recheck_pt_fps.ps1`、`measure_restir_cost.ps1`、`measure_b7_unknowns.ps1`。

### A.8 与 Lumen / P6 的通用性分析（ReSTIR GI/GRIS 能不能两边共用）

> 这是"落点定在 PT 还是 P6"的判断依据。**结论：估计器内核可共用，四层适配不可共用；
> 建议实现放 P6/Lumen 侧一份，PT 侧只写适配** —— 但前提是先把 11.3 与 P6 的框架做了。

#### A.8.1 可共用（consumer-agnostic 的内核）

reservoir 结构与 WRS/MIS 记账、shift 映射（重连 / 随机重放 + 雅可比）、时域重投影与历史校验、
空间复用、final shade、以及"给定 albedo/metallic/roughness/normal + 光源缓冲"的目标函数。
它的输入抽象只需要：**像素级表面数据（depth / normal / albedo+metallic / velocity）+ 光源缓冲 + TLAS + 材质查询**。

#### A.8.2 已经在共用的现成证据（不是设想）

| 资产 | 现状 | 出处 |
|---|---|---|
| `RTDenoiser`（时域累积） | PT 用 `m_PTDenoiser`；Deferred 用 4 个（shadow/AO/reflection/GI）——**跨管线复用已有先例** | `PathTracingPipeline.h` / `DeferredPipeline.h` |
| `PBR_BRDF`（`pbr_common.slang`） | 光栅化 / RT / PT **求值端**共用（PT 采样端是按它逐行对齐的独立实现） | `PT_Common.slang` L18、`全路径追踪管线规划.md` §12 任务 2/3 |
| `GPULight[]` SSBO、STBN 蓝噪声、TLAS | 共用 | `CollectLights` / `STBNTexture` / `RTPass` |
| **bindless 材质槽位约定** | `全路径追踪管线规划.md` §0.6 缺陷 3 之后 PT 与光栅化用同一套 `materialID + kGPUMaterialTexSlot_*` + heap 注册（**这条以前不成立，刚打通**） | `RTPass::BuildSceneMaterialTexture`、`PT_Full.rchit.slang` |
| GI 侧统一输入接缝 | Deferred 帧图对 SSGI/SSR/RT/RSM 全部调用同一个 `SetInputs(depth, normal, albedo)` | `DeferredPipeline_FrameGraph.cpp` L574/607/625/675/747 |
| Provider 输出契约 | `IGIProvider`（`Handles` / `GetPassKind` / `GetDiffuse\|Specular\|AOOutput` / `GetFinalXOutput` / `aux pass`）+ `GIProviderContext`（world/camera/frameIndex/furnace/lightBuffer/tlas） | `Engine/Render/GI/IGIProvider.h` |
| 合成与有效性 | `GIChannelStack` + `GIBlendParams`（UBO，按槽位与源数组归一化） | `GITypes.h` L193/271/284 |

> 上表的 `DeferredPipeline_FrameGraph.cpp` 行号 L574/607/625/675/747 取自源文档；本轮已逐行核验，
> `SetInputs(depth, normal, albedo)` 的调用确实位于 `:574`（DDGI）/`:607`（AO）/`:625`（SSR）/
> `:675`（SSGI）/`:747`（RT Provider），与源文档记载完全一致。

#### A.8.3 必须各写一套适配的四层

| 维度 | PT 侧 | Lumen / P6 侧 |
|---|---|---|
| 主表面来源 | PT 自己的 AOV：depth 为 **R32F 线性视图深度**（带 `-hitT` 号约定、miss=-1000）、normal/albedoMetallic UAV | 光栅 GBuffer（A/B/C + D32 深度 + velocity）；GI 常跑**半/四分之一分辨率** |
| 输出语义 | bounce0 的间接光估计，**必须无偏** | 一个 **GI 源槽位**：喂 `GIChannelStack`，受归一化 + `alpha<0` 有效性契约约束（11.3） |
| 降噪/历史 | `RTDenoiser`(motion blend) + `PTAtrousPass`(SVGF) | 每效果 `RTDenoiser` + `Denoiser`(spatial) + `SpatialDenoiseAux` |
| 可见性查询 | PT 在 RayGen 里自己 trace shadow ray（自有 payload/SBT） | 走 `RTEffectProvider` / 共享 `RTPass` |

两个必须写进设计的工程约束：

1. **分辨率不匹配是实质问题**：光栅侧 GI 半分辨率是常态、PT 全分辨率。共享内核必须参数化
   「被着色的像素网格 + 重投影映射」，否则时域/空间复用的邻域语义直接错。
2. **质量策略是反向的**：PT 作为 oracle **不能继承实时侧偏差**（钳制、半分辨率、有偏时域复用）。
   共享内核必须带 policy 开关：`reference`（无偏、全分辨率、可只做空间复用）vs `realtime`（钳制、半分辨率、时域复用）。

#### A.8.4 落点建议

- 实现**一份**在共享位置：`Engine/Render/GI/` 下的 `RestirGiPass`，实现 `IGIProvider`，输出 radiance + validity 纹理；
- PT 侧只加**薄适配**：RayGen 采样该纹理作为 bounce0 间接光（现在读 DI 的蓄水池 SSBO，改成读纹理反而更可移植）；
- Lumen 侧走 Provider + 槽位机制；
- **不要**在 `PathTracingPipeline` 与 Lumen 各写一套 —— 那正是 `全路径追踪管线规划.md` §12 C16 / P6 警告的"两套估计器"。

#### A.8.5 前置条件（= `全路径追踪管线规划.md` §12 C15/C16 存在的理由）

1. **11.3 统一降噪框架**（`DenoiseSignal` 分派 + 把 `alpha < 0` 有效性提升为框架级契约）——
   否则共享估计器的输出没有统一的有效性/历史约定（见本文 §10）；
2. **P6 的纹理绑定数组化 + PROVIDER-EXEC**（执行单位从「Provider × 通道」改回「Provider」）——
   否则每接一个消费者就加一条定制循环 + 一套具名绑定（见本文 §11）；
3. 一个统一的「光源采样 + 可见性」入口（现在 PT 与 GI 侧各有一套）。

#### A.8.6 与"是否转实时主路径"的组合结论

| 定位 | ReSTIR GI/GRIS 该不该做 | 怎么做 |
|---|---|---|
| PT 继续当参考渲染器（现状） | **不做** —— 多 SPP 已够用（§A.4.1：1spp×4bounce 9.4 ms，帧 30 时 p50 已收敛） | — |
| 做 Lumen / P6 | 做，但**先做框架**（11.3 + PROVIDER-EXEC + 数组化） | 一份内核在 `Engine/Render/GI/`，PT 侧适配；先跑 `reference` policy 验证无偏 |
| ~~PT 转实时主路径~~ | **2026-09-19 已决定不转** ⇒ 本行不适用 | — |

### A.9 数据来源与复现命令

```powershell
# 1) 成本曲线（每档 18~22 s，数 "RG pass: PT_Render" 行算 FPS；HE_CFG 私有副本避免写回基础 cfg）
powershell -File build\verify\measure_pt_cost.ps1
powershell -File build\verify\recheck_pt_fps.ps1     # 基础 cfg vs 私有副本的一致性复核
powershell -File build\verify\measure_restir_cost.ps1 # ReSTIR DI 复用开销
powershell -File build\verify\measure_b7_unknowns.ps1  # 1080p 帧时/显存 + 引擎级冷缓存编译

# 2) 机件缺口检索（全引擎，排除 External/；预期全为 0）
#    Jacobian / shiftMapping / ShiftMapping / reconnection / Reconnection / GRIS / PathReservoir
# 3) RT 管线创建耗时（磁盘 PSO 缓存热）：05 日志里 CreateRTPipelineState → CreateEffectPipeline 的时间差
# 4) 收敛与对照（标准答案的现状）
python Tools\pt\analyze_pt.py --converge <tag30> <tag60> <tag120> <tag240> --conv-tol 0.01
python Tools\pt\analyze_pt.py --compare <pt_tag> <deferred_tag> --target hdr
```

### A.10 与 `全路径追踪管线规划.md` §12 C16 / P6 的关系

`ReSTIR GI` 的实现在本预研里被当作 GRIS 的 M1（形态 A），而它在 `全路径追踪管线规划.md` §6.1 的落点已归**本文 §11 任务 20**（P6：ReSTIR GI 统一估计器 + 纹理绑定数组化）。因此：

- 若 P6 先落地 → 本附录的 M1/M2 应作为 P6 的 PT 侧适配（复用其估计器与绑定数组），而不是另写一套；
- 若决定 PT 侧先做 → 需要在 `全路径追踪管线规划.md` §0.3 增补一条"落点从 P6 移回 PT"的修订记录（本轮不做该修订）。

---

## 四、实现步骤（可执行清单，从 1 起编号）

> 本章把 §1–§17 里"要做成什么样"翻译成"按什么顺序动手"。放在文档最后是刻意的：**设计（一）、
> 实现现状（二）、架构对照（三）三章的编号已经稳定**，本章只追加、不改写前面的结论。
>
> 编号规则：**从 1 开始、只增不改**（已完成/作废的步骤保留编号并标注状态，不重排）。
> 每个步骤给出三行：**目标** / **改动点** / **验收**。验收判据沿用本仓库既有口径 ——
> 白炉真值 `1.0000`、背靠背单源采样逐项一致（pass 集合与顺序 + `provN_raw/final` 数值）、
> `HugEngineTests` 全绿、`06.GILab` 既有预设画面不回归。
>
> 阶段与里程碑的对应：**A = 前置（§12"架构前置"）**、**B–G = L1–L6**、**H = 横切工具与验收**。

| 阶段 | 步骤 | 里程碑 | 文档出处 |
|------|------|--------|---------|
| A 框架前置 | 1–7 | 前置（不新增 L 编号） | §4 末、§11、§12"架构前置" |
| B SDF | 8–12 | L1 | §5 |
| C Surface Cache | 13–19 | L2 | §4、§13 |
| D Screen Probe | 20–25 | L3 | §6 |
| E 远场 HW RT | 26–29 | L4 | §6、§15.2 |
| F Radiance Cache | 30–33 | L5 | §7 |
| G 降噪与优化 | 34–37 | L6 | §10、§6 |
| H 工具与验收 | 38–41 | 横切（每阶段退出前） | §9.1、§14、§16 |

#### 进展记录（每轮实现后追加，编号不变）

| 步骤 | 状态 | 落地 / 证据 |
|------|------|------------|
| 1 Provider 生命周期遍历 | ✅ 已完成 | `DeferredPipeline::OnResize/Shutdown` 遍历 `m_GIProviders`（提交 `a2f4ae3`） |
| 2 帧图顺序契约 | ✅ 已完成 | Lumen 的两个 pass 显式声明 reads/writes；`DeriveBarriers` 顺序里 Lumen 早于 Lighting（`40075b3`） |
| 3 Lumen 资源宿主 | ✅ 已完成 | `Engine/Render/Lumen/LumenScene.{h,cpp}`（`b099a6c`，步骤 8 起承载 SDF） |
| 4 GI 源数据层 | ✅ 已完成 | `GISourceId::Lumen = 12` + 能力位 + shader 双侧真值（`b099a6c`） |
| 5 合成端接入 | ✅ 已完成 | `kGPUBinding_Lumen = 32` + 两个 `case` + `alpha < 0` 有效性契约（`6a3415b`） |
| 6 Provider 骨架 + 第 8 条循环 | ✅ 已完成 | `LumenProvider` + 帧图按 Provider 的循环（`40075b3`）；白炉 1.0 / 常态无效标记 |
| 7 面板与配置 | ✅ 已完成 | `gi_blend_diffuse_lumen` / `gi_blend_specular_lumen` 独立键 + 面板候选（`0a884da`） |
| 8 Mesh SDF 生成 | ✅ 已完成（scatter + 跳步洪泛，128³） | 三个 shader：`SDF_MeshScatter.comp.slang`（清空 + 每三角形一组的 `InterlockedMin`）、`SDF_MeshFlood.comp.slang`（res/2…1 逐级松弛补全 scatter 留下的空洞）、`SDF_MeshConvert.comp.slang`（u32 → R32F + 探针）。16 个 mesh × 128³ = **128 MB**，体素 0.296~21.9（比 gather 版细 4 倍）；自检：远场 61/64 在 2 体素内 ⇒ PASS（判据按算法分层，见 §5） |
| 9 SDF 质量边界 | ✅ 已完成 | §5 的"首版实现与质量边界"：体素边长 0.45~87.6、28/79 mesh 超三角形上限、5 条不适用清单 |
| 10 Global SDF 注入 | 🟡 首版完成（安全达标，下界质量待分层解决） | `SDF_GlobalBuild.comp.slang` + `LumenSDF::BuildGlobalField`：单层 128³、16 MB，**覆盖全场**（边长 3154，体素 24.64，原点 `(-1635.9,-106.3,-855.4)`），自检最大高估 **−9.316**（判据 ≤ 49.283）⇒ 安全 PASS；下界质量 23.4% 探针在 2 体素内（该探针读回路径不可信，见代码注释）⇒ 需 clipmap 细层 + 128³ 逐 mesh 场（§附二：此前"单层退化成 788 单位小盒"的覆盖缺陷已修） |
| 11 sphere tracing | 🟢 安全与精度均达标（单层全场） | `SDF_RayMarch.comp.slang` + 自检：256 射线，**穿漏 0 ⇒ 安全 PASS**；精度 **80.1% ≤1 体素**（p50 0.265、平均 1.114、最大 15.591；近命中 p50 0.223，起点距几何<5 的 238 条 p50 0.057），241 条命中**全部由全局场提供**（细节追踪 0 贡献）。剩余两项已知：15 条仅 GPU 假命中（**无符号**场在几何内部，待符号判定）、远命中（tRef≥50）p50 5.56 体素（单层 24.64 粗层粒度，属步骤 26–29 的 Far Field）。根因修正见 §附二 |
| 11.5 逐 mesh 细节追踪（精度补齐的首版尝试） | 🟡 已实现、当前无收益（根因已证伪并收敛） | `SDF_RayMarchDetail.comp.slang`（逐 mesh dispatch + InterlockedMin 归约，eps 取该 mesh 体素）：**修复覆盖缺陷后**全局场已能独立命中全部 241 条，细节追踪给出 77 条更近的 t，但"≤1 体素"达标数不变（193/241 两者相同）⇒ 当前无可测收益。结论：**先补符号判定**，否则"哪里算表面"这一步就是错的（§5） |
| 11.6 符号（内外）判定 | 🟡 已具备（parity），但对命中精度无改善 | `SDF_MeshBuild.comp.slang` 同循环内 parity 定号并输出带符号距离；细节追踪改为 `\|d\|` 步进/判定；全局注入取 `abs(d)`。距离精度 7.5e-5 不变；**符号一致率仅 76.2%**（parity ↔ 最近三角形法线）—— 根因是场景多为**开放曲面**，"内外"本身无定义（§17.6 第 3 项的同一件事）。sphere tracing 指标逐位不变 ⇒ 下一步改为**细节层主命中 + scatter 提分辨率** |
| 10.5 Global SDF clipmap 分层 | 🟡 已实现两层、安全 PASS，但**未解决紧度** | `globalLayers=2`（近层 6.16 体素 / 788 单位，远层 24.64 / 3154），march 同时采样两层取最小。两层安全判据均 PASS（未高估），但下界质量仍差（近层 0.0% 在 2 体素内、平均低估 454.6）⇒ 根因钉死为"**AABB 外取到 AABB 的距离**"这一注入语义；下一步换成 scatter（只在 AABB 内注入）+ 洪泛补全（`SDF_MeshFlood` 可复用） |
| 12 SDF 帧图接入与调试 | ✅ 已完成（含 L1 退出判据的可视化） | 帧图有独立的 `Lumen_SDF_Build` compute pass（不声明资源依赖、自管 barrier；`writes` 为空故不被 `CullDeadPasses` 裁掉），SDF 构建与渲染解耦；"关掉 Lumen 无任何影响"已实证：Lumen off 时 `lumen_passes=0`、既有源 dump 与接入前**字节级一致**。可视化已落地：`SDF_DebugView.comp.slang` 逐像素主射线 sphere tracing → 稳定命名转储 `lumen_sdf_trace`（1920×1080 RGBA16F）+ 原子计数统计。**它随即暴露了一个自检看不到的真问题**（见 §附三：全局场在空旷处把距离塌缩到 ≈0） |
| 13 Card 生成器 + 覆盖率可视化 | ✅ 已完成（CPU 版，GPU atlas 见 14+） | `LumenSDF::BuildCards()`：逐 mesh × 6 轴向投影成卡片；texel 世界边长按 `cardTexelWorld=4.0` **逐 mesh 自适应**（分辨率 64~512²，实测 texel 0.56~7.41）；按面积加权采样 418,329 个表面点做覆盖判定；保留阈值 → 卡片数 → 覆盖率给出**单调曲线**（5%⇒410 张/89.7%，10%⇒330/86.8%，**25%⇒228/79.1%**，50%⇒66/54.2%）；覆盖率可视化（RGBA8，代表 mesh 的 6 个投影面 + 逐 mesh 覆盖条）以稳定名 `lumen_card_coverage` 转储。**验收**：空洞与卡片设置一一对应（见曲线），残余空洞集中在个别**薄结构** mesh（#12/#43/#44，投影填充率天然很低），已点名 |
| 14 页表 + 页状态机 | ✅ 已完成 | `Lumen/SurfaceCache.slang`（C++/Slang **共享布局**）+ `Lumen/SurfaceCacheTypes.{h,cpp}`（六态 + 迁移真值表 + 页状态机 + 布局 static_assert）+ `Tests/TestSurfaceCache.cpp`（5 例 / 73 断言：真值表、完整生命周期、非法迁移被拒绝且不改状态、校验和对字段敏感）+ `LumenScene_SurfaceCache.cpp`（用步骤 13 的 228 张卡片建 64 页演示表：Allocating 9 / Capturing 5 / Captured 41 / Dirty 9，六态齐全；GPU 镜像缓冲 + `SurfaceCache_PageCheck.comp.slang` 校验和比对 **PASS** 0x545eb5ad）。**验收**：非法迁移 `HE_ASSERT` + 单测覆盖；GPU/C++ 镜像一致（校验和相同） |
| 15 Card Capture（软件光栅化写 atlas） | 🟡 可用（真因已修：push constant 超 128 B 被截断） | 已落地：3 张 RGBA16F atlas（512² = 8×8 页 × 64²；`lumen_sc_atlas_albedo` 可转储）、`SurfaceCache_Capture.comp`（一卡一组、页状态门控只捕 `Capturing`、`kMaxCapturesPerFrame=8` 预算、卡分辨率自适应降采样进 64² 页、命中点投影到屏幕取 GBuffer albedo/normal）、命中/未命中/march 命中三个诊断计数。**实测：5 页 20480 个 texel 全部未命中、march 命中 0**。已排除：push constant 字段错位（旧版 shader 多一个 camPosW，已修）、eps/步长过小（已把 eps 提到 1 个近层体素=14.2、步长 eps/2）。下一步：把 march 单独拿出来，用一张已知卡对照 CPU 真值逐步定位 |
| 16 Feedback（缺失页检测） | ✅ 已完成 | `SurfaceCache_Feedback.comp.slang`：屏幕按 **16×16 分块**，每块取中心像素的 GBuffer 世界坐标 → 找"包含它的最小 AABB"的那张卡 → 写 `(pageIndex, weight)`，`weight = 256 像素 × 卡重要性(填充率×√边长) × 距离衰减`；**每块写自己的槽位**（不用计数器 ⇒ 完整且确定）。C++ 侧读槽位→过滤权重 0→按(权重降序, 页号升序)排序→取前 `kMaxCapturesPerFrame×4`→把 `Invalid` 的页置 `Requested`，再把前 `kMaxCapturesPerFrame` 个 `Requested` 推进到 `Capturing`（**闭环**：下一帧步骤 15 就捕获它们），并同步 GPU 镜像。**验收**：静态相机下 7730 条请求（94.7% 的块有几何）、**top-32 与上一帧重叠 32/32** ⇒ 无整屏抖动；排序含页号定序 ⇒ 完全确定 |
| 17 预算与跨帧摊销 | ✅ 已完成 | 三个显式预算：`maxCapturesPerFrame = 8` / `maxAllocationsPerFrame = 8` / `maxFeedbackPages = 256`（`LumenScene::SetBudgets` 可调）。三段摊销：Feedback 采纳 top-`maxFeedbackPages` → 分配阶段把 `Requested` 推进到 `Allocating`（≤ 分配预算）→ 捕获阶段把 `Allocating` 推进到 `Capturing` 并真正捕获（≤ 捕获预算）；**没做完的留在原状态，不回退不丢弃**。统计每 20 帧输出（预算/六态计数/累计捕获/单帧最多）。**验收**：预算 8 时收敛用 2 帧（8+6 页）、预算减半到 4 时用 4 帧（4+4+4+2 页）⇒ **收敛变慢但单帧捕获量恒 ≤ 预算（无尖峰）**，逐帧日志可复现 |
| 18 LRU 淘汰与碎片整理 | ✅ 已完成（LRU；defrag 见说明） | 逻辑页 = min(卡片数, **1024**)（§4 的页上限），**物理页 = atlas 的 64 块**，两者相差一个量级 ⇒ 淘汰路径真正被走到。`AllocatePhysicalPage`：池空则淘汰"最久未用且内容有效（Captured/Dirty）"的逻辑页（`lastUsedFrame` 最小者），释放其物理页后复用；**绝不动正在流水线里的页**（Requested/Allocating/Capturing）。Feedback 的 top-N 每帧 `Touch`（LRU 的"用"）。统计：`页池/LRU: 物理页 x/64 空闲；分配成功 A / 失败 B / 淘汰 C 次`。**验收（合成漫游，静态相机下人为轮换需要页）**：累计捕获 166 → 247 → 309 页（吞吐持续）、**单帧最多恒 8 页（= 预算，不下降）**、**分配失败恒 0（成功率 100%）**、淘汰 279 → 360 → 422 次。**defrag 说明**：页大小固定、池大小固定 ⇒ 不存在"有空间但拼不出连续块"的碎片，分配不到一律由 LRU 回收；变长页/atlas 搬移式 defrag 列为后续项（§附二十五） |
| 19 L2 退出判据 | ✅ 已完成（L2 闭环） | 无新增代码，只做验证与对照：①**atlas 显示的是场景真实材质**——atlas albedo 与同帧 GBuffer albedo 的亮度统计对照（中位亮度比 **1.11**，p5/p50/p95 = 0.000/0.286/0.404 vs 0.084/0.258/0.396）；②**覆盖率可视化无不可解释区域**——`lumen_card_coverage` 的阈值曲线（5%→89.7% / 25%→79.1%）+ 残余空洞逐条点名（薄结构 mesh）；③**白炉 1.0000**；④**背靠背读数逐位一致**——同配置跑两次，`lumen_sc_atlas_albedo` / `lumen_card_coverage` / `lumen_sdf_trace` **SHA-256 相同**（HDR 因 TAA/自动曝光的时域累积而略有差异，属预期）。工具：`build/verify/l2_report.py` |
| 20 探针布置与自适应合并 | ✅ 已完成 | `ScreenProbe.slang`（C++/Slang 共享布局，96 B，含步骤 23 的 SH 字段）+ `ScreenProbe_Gather.comp.slang`：屏幕按 **16×16 像素为一个单元**，每 **2×2 单元（32×32）** 比较法线一致性，够平坦就合并成 1 个探针、否则保留 4 个；同时把每个 tile 的**最大法线偏差**写进缓冲，CPU 侧据此**一次运行**算出整条"阈值 → 探针数"曲线。**实测（1080p）**：tile 2040（60×34，全部有几何）、单元 8160；`cos=0.995 ⇒ 探针 5649`（GPU 计数与 CPU 曲线同值，互为交叉验证）；曲线 `0.999→6534 / 0.995→5649 / 0.99→5169 / 0.98→4548 / 0.95→3690 / 0.90→3144` **单调不增**。**验收**：数量与设计量级（≈8K）一致 ✅；阈值变化时数量单调 ✅ |
| 21 半球追踪（二维配置落地） | ✅ 已完成 | `LumenTraceConfig.{h,cpp}`：追踪源（SDF/HWRT/Screen）× 着色源（SurfaceCache/HitLighting/Neutral）+ `traceRep`/`shadeRep`/`screenTrace`/`hwFarField`，`Validate()` 在**配置加载期**拒绝非法组合（**SDF × HitLighting** 明确报错：SDF 只给命中距离、拿不到重心坐标/材质；另拦 `traceRep∉[8,16]`、`shadeRep=0`、`Screen` 与 `screenTrace=false` 自相矛盾）。`ScreenProbe_Trace.comp.slang`：每探针 **GGX 重要性采样** 8 条光线（V 取探针法线 ⇒ 分布以法线为中心），求交复用步骤 11 的 march 语义；方向用 (探针, 光线, 帧号) 的 PCG 哈希 ⇒ **可复现**。**实测**：探针 5649 × 8 = **45192 条光线**、命中 **40836（90.4%）**、**半球内 100.0%**（点积>0 全部满足）；配置打印 `SDF × SurfaceCache（traceRep 8 / shadeRep 1）`。单测 216 → **221**（+5 例 / +29 断言，覆盖合法与全部非法组合） |
| 22 命中点着色（消费 L2 的 atlas） | ✅ 已完成 | `Lumen_SurfaceCacheSample.comp.slang` + `LumenScene::RunSurfaceCacheShading()`：命中点 → 最小包含 AABB 的卡 → 逻辑页 → 物理页；页状态 `Captured`/`Dirty` **且 atlas texel 的 alpha > 0** 才算页命中，否则写**中性值 0.18 + 缺页标记**（缺页不能返回黑）；页内 texel 由卡平面 2D 坐标换算并**页边界夹取**（实测 0 次）。同帧另采一份**同一表面**的 GBuffer albedo 做对照（重投影必须用 GBuffer 世界坐标 + 随视距缩放的容差，否则量到的是"几何不同"）。**顺带修掉两个真错误**：①`BuildPageTable` 原来让全部 228 页都走演示生命周期 ⇒ 没有页是 `Invalid`、`Request` 永远失败、真实回路失效（只捕获 53 页且落在无关卡片上，命中率 0.6%）；改为只有前 24 页演示；②反馈请求**未按页去重** ⇒ 前 64 条全是同一页，"采纳 top-64"实际只请求到 1 页；改为"权重最高的前 N 个不同页"并受物理页数约束。**实测**：命中光线 41813 条、页命中 3377（**8.1%**）、缺页 38436（**其中"不在任何卡 AABB 内" = 0**，"卡在但页无内容" = 38436）、越界夹取 0；atlas vs GBuffer 同一表面平均差 **0.1346**，同批样本里"最贴合候选"平均差 **0.0744** ⇒ 选卡歧义贡献 ~0.06（页命中的光线 **100%** 落在 ≥2 张卡的 AABB 内）。**验收**：白炉 1.0000、背靠背 dump 逐位一致、`lumen_passes=0`（关 Lumen）、单测 221/5707 全绿、VUID 46 = 基线 |
| 23–41 | ⬜ 未开始 | 23 SH 投影、24 合成到 Lighting、25 L3 判据；随后 26–33 HW RT/Radiance Cache、34–41 去噪与横切工具 |

### 阶段 A：框架前置（不产出画面，但后补等于重构）

#### 1. Provider 生命周期遍历
- **目标**：让 Provider 自持的持久资源（atlas / clipmap / probe buffer）在 resize 与销毁时被正确重建与释放。
- **改动点**：`Engine/Render/Pipeline/DeferredPipeline.cpp` 的 `OnResize`（`:575-594`）与 `Shutdown`（`:459-514`）增加对 `m_GIProviders` 的遍历；或让 Lumen 走底层 pass 的 `OnResize/Shutdown` 转调（二选一，写进实现注释）。
- **验收**：改变窗口尺寸后反复 resize 无验证层告警、无旧尺寸纹理读取；退出时验证层无泄漏报告。

#### 2. 帧图顺序契约
- **目标**：Lumen 内部 pass 的执行顺序不依赖 `AddPass` 的注册先后。
- **改动点**：约定每个内部 pass 显式声明 RG `reads/writes`（必要时用 dummy WAW，参考 `DeferredPipeline_FrameGraph.cpp:192-194` 对 Shadow 的写法）；对照 `Engine/Render/RenderGraph.cpp:181-186`（LIFO 拓扑序）与 `:302-324`（`CullDeadPasses` 裁掉"写了无人读"的 pass）。
- **验收**：打开 `HE_TRACE_PASSES=1`，Lumen 各 pass 的 `[PASS]` 顺序与设计一致，且重复运行时稳定。

#### 3. Lumen 资源归属与容器
- **目标**：确定 atlas / Global SDF / probe buffer 的持有者与重建路径。
- **改动点**：新增 `Engine/Render/Lumen/LumenScene.{h,cpp}`（或复用 `Engine/Render/GI/` 目录约定）持有持久 GPU 资源；`device->CreateTexture/CreateBuffer` 自建 + `rg.ImportTexture`（不要用 `rg.CreateTexture`：`RenderGraph.h:106-119` 没有由句柄取 `IRHITexture*` 的接口）；`Engine/Render/CMakeLists.txt` 登记源文件。
- **验收**：资源在 `Shutdown` 后全部释放；`r.TransientTest` 之外的路径不新建瞬态纹理。

#### 4. GI 源数据层接入
- **目标**：把 Lumen 变成层栈里的合法源（面板可见、可降级、可序列化）。
- **改动点**：`Engine/Render/GI/GITypes.h` 加 `GISourceId::Lumen = 12`（`:49-65`）、分类谓词（`:87-90` 或 `:94-98`）、`IsCameraViewLimitedSource`（`:120-124`，**按 §15 的判断不加**）、`ToConfidenceMask`（`:147-152`）、`GISourceName`（`:155-170`）、`ToPipelineCap`（`:327-358`，返回 diffuse + specular 两位）、`PipelineCaps::AllSources`（`:404-408`）、预设（`:482-528`，可选）；`Engine/Shader/Shaders/ShaderTypes.slang:123-134` 加 `GISOURCE_LUMEN = 12`（数值必须与 C++ 一致）；`Tests/TestGITypes.cpp` 的全源表与类别计数断言同步。
- **验收**：`HugEngineTests` 全绿；`GIRegistry::IsAvailable(Lumen)` 在 Deferred 管线位下为真；面板能选到且能关闭。

#### 5. 合成端接入
- **目标**：Lumen 的输出能被 Lighting 采样并与其它源加权合成。
- **改动点**：`Engine/Render/Pipeline/LightingPass.h` 的 `LightingInputs` 加字段 → `DeferredPipeline_FrameGraph.cpp:918-1007` 填字段（注意 AO/RSM/DDGI 的旁路写法）→ `LightingPass::Render` 绑定 → `ShaderTypes.slang` 加 `kGPUBinding_*` → `Lighting/DeferredLighting.frag.slang` 的 `SampleDiffuseSource`（`:184-200`）与 `SampleSpecularSource`（`:206-214`）加 `case`（量纲约定 `:164-170`：返回已乘接收面 albedo 的 `albedo·E/π`）；`Engine/RHI/Vulkan/VulkanDevice_Descriptors.cpp` 的描述符池容量按新增绑定复核。
- **验收**：白炉模式下 Lumen 槽位输出为 `1.0000`；关掉 Lumen 时画面与改造前逐像素一致。

#### 6. Provider 骨架 + 空 pass
- **目标**：跑通"注册 → pass → 合成 → 面板 → 计时"整条链，内部先输出常量。
- **改动点**：新增 `Engine/Render/GI/LumenProvider.{h,cpp}`（照 `SSGIProvider.h` / `DDGIProvider.h` 模板，覆写 `GetSourceId/Handles/IsValid/NeedsPass/SyncToStack/GetDiffuse|SpecularOutput/GetFinal*Output/GetAuxPass*/Render/GetTimedPass`）；在 `DeferredPipeline.cpp:168-209` 注册；帧图加**第 8 条 provider 级循环**（按 Provider 遍历、不按 id，避免 `Handles` 两个 id 时被两条循环各跑一次）—— 这是 §11 明写的"逃生口"，**不被任务 19 阻塞**。
- **验收**：pass 在 `HE_TRACE_PASSES=1` 下只出现一次；`GITimer` 有读数；白炉 `1.0000`。

#### 7. 配置、面板与验收基线
- **目标**：建立后续所有阶段的对照基线（没有基线就没法判"没回归"）。
- **改动点**：`Samples/06.GILab/06.GILab.cpp` 的 `kAllDiffuse`（`:1255-1257`）/`kAllSpecular`（`:1315-1317`）/AO 列表（`:1348-1358`）；cfg 往返（加载 `:677-690`、保存 `:1776-1786`，注意每通道只有 4 个槽位）；`Tools/gi/` 下按源扩展 `dump_gi.ps1` / `repeatability_check.ps1`。
- **验收**：Lumen 开/关两次运行的对照报告可复现；既有 SSGI/DDGI/RT 预设读数不变。

### 阶段 B：L1 — SDF（近场追踪几何，§5）

#### 8. Mesh SDF 生成
- **目标**：每个 mesh 生成 128³ R16F 距离场（≈ 4.2 MB/mesh）。
- **改动点**：新增 `Engine/Shader/Shaders/Lumen/SDF_MeshBuild.comp`（暴力"每三角形写入体素"）+ C++ 侧调度与缓存；带 **mesh 数量上限 / 按需生成 / 释放**（§15.3 的显存测算：100 个 mesh ≈ 420 MB）。
- **验收**：可视化 Mesh SDF 与几何一致；显存增量与测算吻合（±10%）。

#### 9. SDF 质量测试场景
- **目标**：在写更多代码前先把"暴力生成的质量缺陷"量化。
- **改动点**：新增薄墙 / 窄缝 / 单面几何的测试场景（可放 `Content/`，或直接用 Sponza 的薄几何局部）；记录漏光与自遮挡的可见条件。
- **验收**：给出"哪些几何不适用"的清单（对照 §17.6 第 3 项的 UE 限制清单），写进实现前置。

#### 10. Global SDF 注入与 clipmap
- **目标**：把 Mesh SDF 合并成可追踪的全局距离场（512³ 单层 → 4×256³ clipmap）。
- **改动点**：`Lumen/SDF_GlobalInject.comp`（mesh → global 注入 + 增量更新）；clipmap 层切换的坐标映射；自建纹理 + `ImportTexture`；`OnResize` 重建。
- **验收**：Global SDF 可视化正确；注入耗时进入 `GITimer` 可读；层切换处无断裂。

#### 11. SDF Ray Marching
- **目标**：sphere tracing（最大 64 步、收敛阈值 0.1 体素、梯度法线、跨层继续）。
- **改动点**：`Lumen/SDF_RayMarch.comp`；供 Screen Probe 与调试视图共用同一份 march 函数（`Lumen/LumenShared.slang` 之类的共享头）。
- **验收**：与解析几何（球/盒）的命中距离误差在 1 个体素内。

#### 12. SDF 帧图接入与调试视图（L1 退出判据）
- **目标**：`SDF_Update` 作为独立 pass 进帧图，并提供可视化。
- **改动点**：帧图新增 pass 并声明依赖（步骤 2 的契约）；调试视图走现有 r.\* 调试开关约定。
- **验收**：**可视化 SDF 追踪结果**（§12 的 L1 判据）；关掉 Lumen 时无任何性能与画面影响。

### 阶段 C：L2 — Surface Cache（射线命中点着色，§4）

#### 13. Card 生成器 + 覆盖率可视化
- **目标**：为 mesh 生成卡片（多角度投影 + 覆盖检查），并让"覆盖不到"可见。
- **改动点**：卡片生成工具（加载期或离线，落点与格式需先定）；覆盖率可视化视图（对齐 §17.6 第 1 项）。
- **验收**：Sponza 上卡片覆盖率可视化无不可解释的空洞；空洞能与卡片数量设置对应起来。

#### 14. 页表 + 页状态机
- **目标**：落 §13 的 `SurfaceCachePageEntry`（六态）与 `SurfaceCacheRequest`，GPU 侧 + C++ 侧镜像一致。
- **改动点**：`Lumen/SurfaceCacheTypes.h`（或 slang 侧结构 + C++ 镜像，注意 std430 对齐）；页表纹理/buffer 自建。
- **验收**：单测覆盖状态迁移合法性（`Invalid→Requested→Allocating→Capturing→Captured→Dirty`）；非法迁移可断言。

#### 15. Card Capture（软件光栅化写 atlas）
- **目标**：逐 Card 一个线程组，把 Albedo / Normal / Emissive 写进 3 张 RGBA16F atlas。
- **改动点**：`Lumen/SurfaceCache_Capture.comp`；只处理本帧预算内、状态为 `Capturing` 的页。
- **验收**：atlas 可视化与场景材质一致（§12 的 L2 判据）。

#### 16. Feedback（缺失页检测）
- **目标**：产出"本帧需要哪些页"的请求列表 `(pageID, weight)`。
- **改动点**：`Lumen/SurfaceCache_Feedback.comp`（16×16 像素降采样，weight = 屏幕覆盖面积 × 重要性 × 距离）；C++ 侧小规模排序 + 取前 `maxCapturesPerFrame`。
- **验收**：相机移动时请求列表稳定（无整屏抖动）；权重排序与屏幕重要性一致。

#### 17. 预算与跨帧摊销
- **目标**：落地 §4 的 `maxCapturesPerFrame` / `maxAllocationsPerFrame` / `maxFeedbackPages`。
- **改动点**：C++ 侧预算调度（未完成请求留在 `Requested`，不回退不丢弃）；页状态写回与统计输出。
- **验收**：预算减半时画面收敛变慢但**不出现卡顿尖峰**；帧时曲线平滑。

#### 18. LRU 淘汰与碎片整理
- **目标**：在 1024 页上限内稳定运行，长期无碎片恶化。
- **改动点**：LRU（`lastTouchFrame`）+ defrag pass + 脏页标记（几何/材质变更）。
- **验收**：长时间漫游后捕获吞吐不下降；atlas 分配成功率保持 100%。

#### 19. L2 退出判据
- **目标**：Surface Cache 可被后续阶段当作"命中点材质源"使用。
- **改动点**：无新增；整理 dump 与对照报告。
- **验收**：atlas 正确显示材质；覆盖率可视化无不可解释区域；白炉 `1.0000`；背靠背读数一致。

### 阶段 D：L3 — Screen Probe Gather（§6）

#### 20. 探针放置与自适应合并
- **目标**：16×16 像素一个探针（1080p ≈ 8K 探针），平坦区合并到 32×32。
- **改动点**：`Lumen/ScreenProbeGather.comp` 的探针布置与合并判据（法线方差阈值）；`ScreenProbe` 结构（§13）。
- **验收**：探针数量与设计量级一致；合并阈值变化时数量单调变化。

#### 21. 半球追踪（二维配置落地）
- **目标**：按 §6 的 `traceRep × shadeRep` 发射 8–16 条 GGX 重要性采样光线。
- **改动点**：`LumenTraceConfig`（首版 `SDF(+HW 远场) × SurfaceCache`、`screenTrace=false`）；命中求交复用步骤 11 的 march；组合约束（`SDF × HitLighting` 必须显式拒绝并断言）。
- **验收**：单探针光线方向分布正确；非法组合在配置加载期即报错。

#### 22. 命中点着色
- **目标**：命中点采样 Surface Cache atlas 材质 + 查 ClusteredShading LightGrid 直接光。
- **改动点**：`LumenShared.slang` 的命中着色函数；atlas 采样需处理页边界与缺页（缺页返回中性值 + 标记）。
- **验收**：命中点 albedo 与 GBuffer 的 albedo 在同一几何上一致（无几何时视为通过）。

#### 23. SH 投影
- **目标**：把探针结果投成二阶球谐（4 系数 RGB = 12 floats/probe）。
- **改动点**：`Lumen/ScreenProbe_SHProject.comp`；probe SH buffer 自建。
- **验收**：SH 重建的辐照度与逐光线求和的误差在白炉下为 0（数值可断言）。

#### 24. 合成到 Lighting
- **目标**：探针 SH 能作为 Lumen 源的输出参与合成。
- **改动点**：从 SH 采样出 per-pixel 入射辐照度（首版可直接全屏采样最近探针，后续再插值）；接到步骤 5 的 `LightingInputs`。
- **验收**：关闭其它源、只开 Lumen 时，屏幕上出现与场景一致的间接漫反射。

#### 25. L3 退出判据
- **验收**：**半球追踪产生漫反射 GI**（§12 的 L3 判据）；与 SSGI/DDGI 的背靠背对照可解释差异（不要求一致，要求"差异可归因"）。

### 阶段 E：L4 — 远场 HW RT 与混合（§6、§15.2）

#### 26. 远场 TraceRay
- **目标**：`rayDistance ≥ maxSDFDistance`（50 m）时改走 HW RT 二级光线。
- **改动点**：复用 `Engine/Render/Pipeline/RTPass` 的 TLAS / 场景材质纹理（参考 `RTProvider.h:280-305` 的 `RTExecuteContext` 组装方式）；`Lumen/ScreenProbeTrace.rgen`（或等价 rgen）+ SBT 注册。
- **验收**：远场命中与三角形场景一致（用 RTGI/PT 作为参考对照）。

#### 27. traceRep 切换与 overlap fade
- **目标**：SDF ↔ HW 切换处无 discontinuity。
- **改动点**：切换逻辑只挂在 `traceRep` 上（§6 的副作用说明）；切换带 N 步 overlap fade。
- **验收**：沿射线方向扫过 50 m 阈值时，探针辐照度连续（无阶跃）。

#### 28. 组合约束与 Hit Lighting 空壳
- **目标**：把 §6 的组合约束写进代码路径，为第二版留入口。
- **改动点**：`shadeRep == HitLighting` 的分支显式保留（首版返回"未实现"并记一条日志/断言，而不是静默回落）。
- **验收**：打开未实现组合时**报错可见**，不产生错误画面。

#### 29. L4 退出判据
- **验收**：**SDF 近 + RT 远正确混合**（§12 的 L4 判据）；帧时与显存增量有记录（进 §15.3 的表）。

### 阶段 F：L5 — Radiance Cache（§7）

#### 30. 探针表示升级
- **目标**：DDGI 三阶 9 系数 → 二阶 4 系数（RGB 独立）。
- **改动点**：`Engine/Render/GI/GI_DDGI.{h,cpp}` 的探针缓冲布局与采样端（`DeferredLighting.frag.slang` 的 `u_DDGIProbes`）；保持与 `kGIConfProbeGrid` 置信度掩码一致。
- **验收**：SH 布局变更后白炉仍为 `1.0000`；DDGI 单独使用的画面不回归。

#### 31. 输入改为 Screen Probe + 时间混合
- **目标**：`SetTracedRadiance` 一类接口改为接收 Screen Probe 的结果；history 重投影 + 时间混合。
- **改动点**：仿 `DeferredPipeline_FrameGraph.cpp:505-563` 的探针射线注入方式；探针网格外仍按置信度归零。
- **验收**：室内静态场景在 30 帧内收敛；相机移动时无拖影累积。

#### 32. 插值与距离权重
- **目标**：三线性 + 距离权重插值，网格边界平滑。
- **改动点**：采样端插值函数；`GIProbeGrid::FitProbeGridToBounds` 的网格拟合参数复核。
- **验收**：网格内无可见格子状伪影；网格外淡出正确。

#### 33. L5 退出判据
- **验收**：**室内/室外稳定 GI**（§12 的 L5 判据）；与"关闭 Lumen、仅 DDGI"的对照可解释。

### 阶段 G：L6 — 降噪与优化（前置 §10）

#### 34. 统一降噪框架 11.3
- **目标**：`DenoiseSignal` 抽象 + 统一历史纹理/采样器分配 + 批量 dispatch + 框架级"`alpha < 0` 即无效"契约。
- **改动点**：`Engine/Render/PostProcess/` 的 `Denoiser` / `RTDenoiser` 之上加信号层；`SpatialDenoiseAux` 与 `RTProvider` 的 `std::vector<Stage>` 成为其两个消费者；`needsUpscale` 信号属性（半分辨率也要降噪）。
- **验收**：§10 的 11.3 判据——真实多信号共存（Lumen 探针 + 反射 + 阴影同帧）下合成端只读一个有效性约定；既有 RT/SSGI 背靠背读数不变。

#### 35. Lumen 信号接入框架
- **目标**：ScreenProbe 的 3×3 YCoCg AABB 空间滤波 + 时域（EMA/重投影）注册为框架内信号。
- **改动点**：`Lumen/ScreenProbe_Filter.comp`；时域历史走框架统一分配（不再各写一套）。
- **验收**：探针噪声下降可量化（`std/mean`）；无新增降噪器实例被"另挂一套"。

#### 36. 半分辨率升采样与有效性
- **目标**：探针半分辨率/低分辨率输出经 `needsUpscale` 重建后再参与合成。
- **改动点**：升采样 pass + 合成端按有效性掩码降权。
- **验收**：半分辨率与全分辨率切换时画面无跳变。

#### 37. 性能与异步（L6 退出判据）
- **目标**：60 fps @ 1080p（§12 的 L6 判据）。
- **改动点**：`RenderGraph::ExecuteWithAsyncCompute`（`RenderGraph.cpp:447,500`）接入 Lumen 的 compute 段；摊销预算（步骤 17）调优；必要时分级画质。
- **验收**：目标机上 1080p 达到 60 fps；GPU 计时（`GITimer`）给出各 pass 分解；无 hitch（1% low 帧时可控）。

### 阶段 H：横切工具与验收（每个阶段退出前都要过）

#### 38. 白炉真值覆盖
- **目标**：Lumen 的每个新信号都能在 `furnaceMode` 下自证标度。
- **改动点**：Provider 的 `Render` 消费 `ctx.furnace`（注意 RSM 的 ctx 目前只填了前 4 个字段、furnace=false 的坑）。
- **验收**：只开 Lumen 时白炉读数 `1.0000`。

#### 39. 背靠背单源采样对照
- **目标**：每次改动都能证明"没动的源没变"。
- **改动点**：按 `Tools/gi/repeatability_check.ps1` 的既有方式扩展 Lumen 源；对照项 = pass 集合与顺序 + `provN_raw/final` 数值。
- **验收**：改动前后的对照在实测抖动内（参考 11.1 的 ±0.001% 口径）。

#### 40. 调试与可视化工具
- **目标**：让"发黑/漏光/闪"可归因，而不是靠猜。
- **改动点**：页状态 dump、SDF 可视化（步骤 12）、探针可视化、**卡片覆盖率可视化**（步骤 13）、各 pass 耗时。
- **验收**：每个已知故障模式都能被至少一个工具直接观察到。

#### 41. 单测与预设回归
- **目标**：数据层与配置层的正确性不靠画面。
- **改动点**：`Tests/TestGITypes.cpp`（全源表、通道表、类别计数、`ToConfidenceMask`、能力位）；`GIConfigFromPreset`；`06.GILab` 既有预设。
- **验收**：`HugEngineTests` 全绿；预设往返（保存→加载）稳定；既有预设画面不回归。

---

## 附：SDF 调试纪要（步骤 8~11，2026-09-19）

本节按时间顺序记录 L1（SDF）实现过程中**被证伪的判断**与仍然成立的结论，供后续接手者避免重复走弯路。

### 已实现
- 逐 mesh 距离场：`SDF_MeshScatter`（每三角形一组、`InterlockedMin` 精确最小）+ `SDF_MeshFlood`
  （跳步洪泛补全，每级两趟）+ `SDF_MeshConvert`（u32 → R32F）；128³、mesh 上限 16、显存 128 MB。
- Global SDF：clipmap **两层**（近层体素 6.16 / 覆盖 788 单位、远层 24.64 / 覆盖 3154 单位），
  注入只在 mesh AABB 内 + 洪泛补全；`SDF_RayMarch` 同时采样两层取最小值。
- sphere tracing：`SDF_RayMarch`（全局，两层）与 `SDF_RayMarchDetail`（逐 mesh 细节追踪），
  **半量安全步长** `t += max(0.5·d, eps)`。
- 自检：逐 mesh 场自检（1024 探针，跨 16 个网格）、逐层安全/下界质量、三方对照、射线自检
  （256 条射线 vs CPU Möller–Trumbore）。

### 被证伪的判断（都做过实验）
1. "全局层的 AABB 外回退语义是紧度主因" —— 换成 scatter + 洪泛后指标**逐位相同**。
2. "符号（内外）缺失导致假命中、补上符号能改善精度" —— 符号补上后 sphere tracing 指标**逐位不变**；
   符号一致率只有 51~76%，根因是场景多为**开放曲面**（内外无定义）。
3. "mesh 层分辨率是主因" —— 128³ → 160³ 后远场误差与射线精度**都不变**，只多花 2× 显存、3× 烘焙时间。
4. "探针读回能作为判据" —— 哨兵实验两次读数自相矛盾（层 0 报 64/64、层 1 报 0/64；不写哨兵时
   层 0 读回全 0）⇒ **CPU Map 读主机可见缓冲的语义未经验证**，该路径整体判为不可信。

### 仍然成立
- **安全性达标**：无穿漏（半量步长兜住了近似场的高估）；这条依赖"场不高估"与安全系数两条。
- **精度未达标**：256 条射线里 `≤1 体素` 仅 13.6%、平均 13.5 体素；`仅 GPU 命中 146` 条来自
  无符号场在几何内部的假命中；`仅 CPU 命中 0`。
- **射线精度对 mesh 层分辨率不敏感** ⇒ 命中几乎全部来自**全局层**，mesh 层与细节追踪基本没参与。
  这是当前最有价值的线索：下一步应做**分层归因**（在射线结果缓冲里写回命中所在层与层内场值，
  复用已验证的射线通道，绕开不可信的探针读回），再决定修哪一层。
- 洪泛每级两趟带来小幅收益（最坏远场误差 278.7 → 221.5、部分网格超差数 −3~4），保留。
### 分层归因实验（2026-09-19，globalLayers=1 对照）

临时把 `globalLayers` 设为 1（只建一层；此时它建的是**近层几何**：体素 6.16、覆盖 788 单位），
与两层配置对照：

| 指标 | 两层（近 788 + 远 3154） | 只有近层 |
|------|------------------------|---------|
| 两者都命中 / 仅 GPU / 仅 CPU | 110 / 146 / **0** | 107 / **62** / **3（穿漏重现）** |
| p50 / p90 误差 | 7.237 / 38.451 体素 | **6.406** / 38.219 体素 |
| 法线朝向正确 | **61** | **2** |

三条结论：
1. **命中主要来自远层**：去掉远层后"仅 GPU 命中"从 146 掉到 62，命中点大面积改变
   （法线朝向正确从 61 崩到 2）。
2. **误差与层数无关**：p50 6.4~7.2 体素（≈40~44 世界单位）、p90 ~38 体素几乎不动 ⇒ **当前的精度
   瓶颈既不在这两层的分辨率，也不在层配置**，而在更上游（mesh 场/注入质量，或这套 256 条射线
   本身的构成）。
3. **穿漏会被近层掩盖**：只有远层时重新出现 3 条穿漏 ⇒ 半量安全步长并非对所有区域充分，
   近层的细数据此前在替它兜底。**这是安全性上的一条真发现**，与"0 穿漏"的结论不冲突但要记住条件。
### 上游诊断：误差按"起点是否贴近几何"分裂（2026-09-19，决定性）

在射线自检里为每条射线补算**起点到几何的精确距离 `d0`**，再把命中的误差按 `d0 < 5` 分组：

```
误差分布: n=107 p50=6.406 p90=38.219 体素
按起点到几何距离分组: 近(d0<5) n=41 p50=0.159 / 远 n=66 p50=59.752
```

⇒ **近几何的射线误差是 0.159 体素（≈1 个世界单位，等于是精确的）；远离几何的射线误差 59.75 体素
（≈368 单位，灾难性）**。这解释了此前所有"逐位不变"的实验：改动影响的是**近场**（本就精确），
而指标被**远场**主导。

**结论与排序（本轮的重要修正）**：
1. **"精度未达标"要限定条件**：近表面精度 ≈0.16 体素（远优于"≤1 体素"的判据），不达标的是远场；
2. 远场误差来源是跳步洪泛的近似 + 全局层体素尺度（与 §5 早先的量化一致），而**命中着色真正关心的是
   近表面**——因此更合理的设计是：**限制追踪距离**（`maxTraceDistance` 取近层覆盖内）并让近场承担
   主命中，远场只做"是否可能有几何"的粗剔除；这也与 UE 的 Detail Tracing（近处 mesh 场）+ Far Field
   （远处另一套表示）同构；
3. 后续任何精度实验都必须用**分组后的近场 p50** 作为判据（0.159 是可动、可比、有意义的数），
   继续用全体均值只会得到"逐位不变"。

**一处必须记录的口径异常**：本次运行的整体计数（107/62/3、法线朝向 2）与上一轮 `globalLayers=1`
的对照完全一致，而配置已还原为两层 —— 说明构建/加载可能落在了与预期不同的配置上（未及查明）。
上述**分组结论不受影响**（它来自同一批射线的 CPU 侧 `d0` 与 GPU 命中距离），但"两层的整体计数"
这一点在下次继续时需要重新确认。
### 测试集修正：射线从表面出发后，指标与问题都变了（2026-09-19，重要）

把验证射线从"随机撒在 mesh AABB 内"改为"**从表面出发**"（随机三角形 + 面内随机重心点 + 沿法线外移
1 单位）——这符合探针射线的真实用法：

| 指标 | 随机 AABB 射线（旧） | 表面出发射线（新） |
|------|-------------------|------------------|
| 两者都命中 / 仅 GPU / 仅 CPU | 107 / **146** / 0~3 | **228** / **7** / **19** |
| `≤1 体素` | 13.1% | **62.3%** |
| 误差 p50 / p90 | 6.406 / 38.219 体素 | **0.461 / 11.819** |
| 近场（d0<5）p50 | 0.159 | **0.186 体素**（≈1.1 世界单位） |

三条结论：
1. **"精度未达标"很大程度上是测试集的伪影**：换成真实用法后 `≤1 体素` 从 13.1% 跳到 **62.3%**、
   假命中 146 → 7、平均误差 13.39 → 4.14 体素，而近场 p50 始终在 0.16~0.19 体素（约 1 个世界单位）。
2. **暴露了一个新的真问题：19 条穿漏**（此前 0~3 条被远场射线掩盖）。机制清楚：射线起点距表面
   仅 1 个单位，而收敛阈值 `eps = 0.25 × 全局层体素 = 1.54`，第一步就可能跨过表面
   （`t += max(0.5d, eps)` 的 eps 下限在这里成了过冲源）。
3. 待办随之明确：**把 eps 与步长下限分开**——命中判据用小的 eps（如 0.1~0.25 世界单位或 1/4 个
   **近层**体素），步长下限单独取一个远小于 eps 的量；另法线朝向统计（正确 0 条）也要复核
   （差分步长取的是一个体素，粗层上梯度可能退化）。
### 穿漏的机制再收窄（2026-09-19）

把"命中判据的 eps"与"步长下限"分开（步长下限改为 `eps × 0.02`）后，**19 条穿漏不变**、p50 仍 0.461
体素 ⇒ 过冲不是来自步长下限。结合此前数据，机制可以写得更精确：

- 射线起点在表面外 1 个单位，落在**洪泛填充出来的那层壳**里，而不是 scatter 的精确带里；
- 洪泛松弛出来的值**可能大于真实距离**（近似场的高估），一步就跨过表面 → 该面再也追不回来（穿漏）；
- 佐证：mesh 场的近表面探针（在 scatter 带内）精度是 0.16~0.19 体素，而 1 单位外的射线会穿漏 ——
  差别正是"采样点落在精确带内还是洪泛壳内"。

**下一步的候选（按性价比）**：
1. **加宽 scatter 的精确带**（三角形 AABB 外扩更多体素，例如 2~4 个体素）：让表面附近若干体素都由
   精确点-三角形距离写入，洪泛只负责更远处。实现代价是 scatter 的体素量按 O(band) 增长，可控；
2. 命中改**回溯式**：步进后若场值变小且已接近 eps，则退回中点重测（二分），对近似场的高估更稳；
3. 或按用途直接限制：命中只允许发生在近层覆盖内，并用近层的细体素（6.16）作为 eps 的尺度。

三条都记在此，供下一步按代价/收益挑选；本轮只落实了"eps 与步长下限分开"这一条无风险改动。
### 加宽 scatter 精确带：只解决 3/19（2026-09-19）

把 mesh 层 scatter 的体素范围从"三角形 AABB ±1 体素"加宽到 **±4 体素**（表面附近若干体素都由精确
点-三角形距离写入，洪泛只管更远处）：

| 指标 | ±1 体素 | ±4 体素 |
|------|--------|--------|
| 两者都命中 / 仅 CPU（穿漏） | 228 / **19** | 231 / **16** |
| p50 误差 | 0.461 | 0.448 体素 |

⇒ 只解决 3/19。**剩下的 16 条穿漏不在 mesh 层的散射带里，而在全局层自己的洪泛**：全局层的场是
"注入（线性采样 mesh 场）+ 它自己的跳步洪泛"的产物，而它的网格是 6.16 / 24.64 单位体素 —— 洪泛在
这个尺度上的高估与 mesh 层的带宽度无关。这解释了为什么加宽带只带来边际改善。

**下一步（更准）**：把穿漏的归因也做进射线结果缓冲 —— 对穿漏的射线记下"命中失败前最后几个 `t` 处
的场值属于哪一层、值是多少"，直接看是层 0 还是层 1 把射线放过去的。在此之前不再改离散参数。
### 又一次测试集伪影：起点偏移小于 eps（2026-09-19，关键）

把步长系数从 0.5 降到 0.25 后指标**逐位不变**（穿漏 16、p50 0.448）—— 这不可能，除非射线根本没走。
查明：**收敛阈值 `eps = 0.25 × 近层体素 = 1.54`，而射线起点只离表面 1 个单位** ⇒ `d(origin) < eps`，
射线在 **t=0 处就判命中**，步进逻辑完全没参与。这正是此前多轮"改什么都不动"的又一个来源。

把起点外移到 **4 个单位**（> 2.6 × eps）后指标终于动了：

| 指标 | 起点偏移 1 单位（< eps） | 起点偏移 4 单位（> eps） |
|------|----------------------|----------------------|
| 两者都命中 / 仅 GPU / 仅 CPU | 231 / 7 / 16 | 227 / **9** / **14** |
| `≤1 体素` | 62.8% | **47.6%** |
| p50 / p90 误差 | 0.448 / 11.723 | **1.080 / 12.829** |
| 近场（d0<5）p50 | 0.186 | **0.745 体素**（≈4.6 世界单位） |

**结论**：近场 p50 0.745 体素 ≈ 4.6 世界单位，与"起点偏移 4 单位"同量级 —— 说明**当前命中判据
（t=0 处 d<eps 也算命中）把"起点本身就在表面附近"当成了命中**，指标仍有系统性偏差。
下一轮必须先把**命中判据**改对（例如要求沿射线方向真正发生一次"d 下降到 eps 以下"的步进，
或把 t=0 的命中单独统计并从精度判据里剔除），再谈 SDF 质量 —— 否则每次都在量一个伪影。
### 命中判据加"必须先走一步"：仍是逐位不变（2026-09-19）

给两个 raymarch 加上"命中前必须沿射线前进至少一步"（`advanced && d < eps`）后，指标仍**逐位不变**
（227 / 9 / 14、p50 1.080）—— 说明起点外移 4 单位后 `d(origin) ≈ 4 > eps = 1.54`，本来就不存在
t=0 命中；这一条是对判据的语义加固（保留），不是本轮数值变化的来源。

**由此得到最终的分组口径（下一轮直接用它）**：既然起点距离已不构成伪影，剩下的误差应按
**命中距离**分组，而不是按起点距离 —— 场在"命中点离表面近"（`tRef` 小、且落在近层覆盖内）时
是精确的，在"命中点很远"（远层 24.64 体素）时才粗糙。因此判据应改为：

- `tRef < 50`（近场命中）：p50 ≤ 1 体素（当前应能达标，近场探针精度 0.16~0.19 体素）；
- `tRef ≥ 50`：单独统计，并明确它由 layer 1（24.64 体素）负责，指标按远场标准看。

在此之前，继续用全体 p50 = 1.080 体素（把远场算进来）只会让人误判近场也差。
### 关键修正：近场命中精度达标（p50 0.814 体素）+ 一处构建事故（2026-09-19）

先纠正一个严重的方法问题：`SDF_RayMarchDetail.comp.slang` 里 `advanced` 只赋了值、没有声明，**shader
编译一直失败**，而我用 `Select-String` 过滤构建输出时只看了 `error C|error LNK`，把 shader 的
`error[E30015]` 漏掉了 ⇒ **第 35~38 轮的冒烟跑的是旧二进制**，那几轮"逐位不变"的读数**无效**
（步长系数 0.5→0.25、`advanced` 守卫都没有真正被测量）。教训：构建后用**不带过滤的尾部输出**确认，
或把 `error[` 也纳入过滤；这与"先证明数据是被写出来的"是同一条纪律。

修好后按**命中距离**分组（新判据）得到有效读数：

| 分组 | n | p50 误差 |
|------|---|---------|
| **近命中（tRef < 50）** | **195** | **0.814 体素**（≈5 世界单位） |
| 远命中（tRef ≥ 50，由远层 24.64 体素负责） | 32 | 21.465 体素 |

⇒ **近场命中精度达标**（判据 ≤1 体素，实测 0.814），远场粗糙与预期一致（远层体素 24.64）。
整体 `≤1 体素 49.8%`、`p50 1.029`、`仅 CPU 命中 14（穿漏）`、`仅 GPU 9`。

**剩余待办**：① 14 条穿漏逐条归因（近场命中为主后，穿漏也应按命中距离看）；② 日志里"按命中距离
分组"打印重复了两行（同一处插入被执行过两次），属清理项。
### 穿漏全部在近场；放宽命中容差无效（2026-09-19）

把穿漏按命中距离拆开（`cpuOnlyNear` / `cpuOnlyFar`）：

```
穿漏按命中距离: 近命中(tRef<50) 14 条 / 远命中 0 条
```

⇒ **14 条穿漏全部落在近场**，不是远层粒度问题 —— 这把它确认为一个**近场真问题**（而近场命中精度
同时是达标的：p50 0.814 体素）。

随后把全局追踪的命中容差从 `0.25 × 近层体素`（1.54）放宽到 `0.5 ×`（3.08），**指标逐位不变**
（14 条穿漏、p50 0.814）—— 说明这些射线的**最近接近距离本就大于 3.08**，或场在它们路径上偏大；
已把容差改回 0.25（精度优先，放宽没有收益）。

**下一步（唯一能把两者分开的诊断）**：在 `SDF_RayMarch` 里为每条射线额外记录**沿途场值的最小值
`minD`**，与 CPU 侧"射线到几何的真正最近距离"并排比较：
- `minD` 明显大于真值 ⇒ **场在路径上高估**（洪泛壳问题，需改场）；
- `minD` 与真值接近但都大于 eps ⇒ 只是容差问题（提高 eps 即可，代价是精度）。
这一步只加一个输出量，不动算法。
## 附二：14 条"穿漏"的真因 —— clipmap 覆盖不到几何（已修复）

**症状**：步骤 11 的 sphere tracing 自检长期 `安全 FAIL`，`仅 CPU 14`（穿漏）。此前把 19 条压到 3 条、
又回到 14 条，试过加宽 scatter 带、增加洪泛趟数、放宽/收紧 eps（0.25↔0.5 体素）—— 指标逐位不变。
"逐位不变"本身就该是强提示：**改动完全没进入被观测的路径**。

**诊断（本轮新增的两个量）**：
1. `SDF_RayMarch` 记录每条射线沿途场值最小值 `minD` 及其位置 `tMinD`（未命中时借 `u_RayHit.w` 的符号携带）；
2. CPU 侧统计射线起点是否落在各 clipmap 层的覆盖盒内。

结果一目了然：14 条穿漏的 `minD` **全是 1e30**、`tMinD = 0`、`steps = 1` ——
循环里 `d < minD` 的比较**一次都没执行**，因为第一步就命中了
`if (a.y < 0.5 && b.y < 0.5) break;`（两层都不在域内）。
再统计覆盖：**256/256 条射线的起点都在近层覆盖盒之外**。

**真因**：`SetupGlobalGrid` 里 `stepsFromFar = (kMaxGlobalLayers - 1u) - L` 用了**常量**而不是
**实际层数**。当配置只开 1 层时，唯一那层被按"近层"尺寸建成
`场景最长轴 3154 × nearFraction 0.25 = 788 单位`、且居中于场景中心，
而几何散布在整个 3154 单位内。于是：

- 全局场的覆盖盒（原点 `(-453,1076,327)`，边长 788）与大部分几何不相交；
- 全局 sphere tracing 一条都没命中 —— 长期被误读为"洪泛壳高估"的那条
  `误差分解: 仅全局场 0/227 在 1 体素内（平均 0.000）`，其实是**全局追踪零命中**，
  227 条命中全部来自逐 mesh 细节追踪；
- 14 条"穿漏"就是起点落在场外的射线（它们的 `tRef` 很小，说明起点本来就贴着表面）。

**修复**：`stepsFromFar = (m_GlobalLayerCount - 1u) - L`。单层时 `stepsFromFar = 0`，
该层覆盖全场（体素 24.64，原点 `(-1635.9,-106.3,-855.4)`，边长 3154）；双层时行为与原来一致。

**修复后（`lumen_coverfix`，121 帧，exit=0，无新增 VUID）**：

```
Global SDF 层 0 128³（体素边长 24.6413，边长 3154.1，原点 (-1635.9,-106.3,-855.4)）
march 参数: 层0 原点(-1635.9,-106.3,-855.4) 体素 24.641 res 128 | 层1 res 0 | 射线 256 步数 192 eps 6.160
sphere tracing 自检: 两者都命中 241，仅 GPU 15，仅 CPU 0（穿漏）=> 安全 PASS
精度：误差 ≤1 体素 193/241（80.1%），最大 15.591 体素，平均 1.114；法线朝向正确 148
误差分布: n=241 p50=0.265 p90=3.144；近(d0<5) n=238 p50=0.057 / 远 n=3 p50=2.459
按命中距离分组: 近命中(tRef<50) n=209 p50=0.223 / 远命中 n=32 p50=5.555
场覆盖: 层数 1，近层体素 24.64；射线起点在近层覆盖外 0 条
误差分解: 仅全局场 193/241 在 1 体素内；细节追踪更近的射线 77 条（但对达标数无贡献）
```

**结论与教训**：

- 穿漏 **归零**，安全判据首次 PASS；精度 p50 从 1.029 体素降到 **0.265**（单层全场体素 24.64，
  即 p50 误差 ≈ 6.5 世界单位），近命中 p50 **0.223 体素**。步骤 11 的"安全 + 分组精度"口径由此达标。
- 剩余两处已知项：`仅 GPU 15`（**无符号** mesh 场的内部假命中，需带符号场）、
  远命中 p50 5.555 体素（单层粗层粒度，属步骤 26–29 的 Far Field / 分层议题）。
- **教训**：自检坐标系与场域必须显式核对。一个"安全失败"可能是**覆盖**问题而非**场质量**问题；
  以后任何"改了参数却逐位不变"的结果，都应立刻怀疑改动没进入观测路径（本轮两次：
  调 eps 无效、shader 里 `advanced` 未声明导致构建失败而沿用旧二进制）。
- 运行提示：`build/verify/lumen_smoke.ps1` 必须带 `-Extra 'gi_blend_diffuse_lumen=1'`，
  否则 Lumen 通道不注册（`RG pass: Lumen` 计数为 0），SDF 流水线停在初始化、看不到任何自检输出。
## 附三：步骤 12 的可视化立刻暴露了一个"自检看不到"的真问题

**做了什么**：新增 `Engine/Shader/Shaders/Lumen/SDF_DebugView.comp.slang` —— 从相机逐像素发射主射线，
按与 `SDF_RayMarch.comp.slang` **逐字一致**的语义做 sphere tracing（两层取 min、`max(d*0.25, eps*0.02)`
安全步长、`advanced && d < eps` 命中判据），输出 RGBA16F（R=命中层、G=t/最大距离、B=步数比、A=是否命中），
并用原子计数给出命中率/分层/平均步数。帧图在既有的 `Lumen_SDF_Build` compute pass 内、SDF 构建之后调用
（Vulkan 不允许在 render pass 内 dispatch）；06.GILab 以**稳定名** `lumen_sdf_trace` 转储（不依赖 provider
注册顺序）。离线统计脚本：`build/verify/f16_sdfdbg.py`（同时可把它渲染成 PNG 看图）。

**结果（`lumen_dbgview`，121 帧，exit=0，VUID 46 与改动前相同）**：

```
SDF 调试视图就绪（1920x1080，RGBA16F，逐像素 sphere tracing）
调试视图统计（第 60 帧，逐像素主射线 128563200 条）: 命中 100.0%（近层 0 / 远层 128563200），
    未命中 0（0.0%），平均步数 2.0/192；eps 1.540 世界单位（0.25 体素）
调试视图归因: 相机 (308.8,238.7,0.2) 到真实几何 102.10 世界单位（66.293 倍 eps）
```

离线读图（`f16_sdfdbg.py`，1920×1080）：`G = t/最大距离` 的 p5 = p50 = p95 = **0.0001**，
`B = 步数比` p50 = p95 = 0.0104（= 2/192）。也就是**每个像素都在第二步、t = eps×0.02 ≈ 0.031 处判命中**，
图像是一片均匀白 —— 不是"看见了表面"，而是"到处都被判成贴着表面"。

**归因（决定性）**：相机到真实几何有 **102.10 世界单位**（66 倍 eps），场却在相机处给出 `< eps` 的值。
即：**全局场在空旷处把距离塌缩到 ≈0**。这与 Global 层自检的读数方向一致（层 1「平均低估 560.90」；
层 0「0.0% 探针在 2 体素内」—— 该探针读回路径本身不可信，只能作趋势参考）。

**为什么步骤 11 的自检看不见**：256 条自检射线的起点都**贴着几何**（`d0` 0.07~4.0，起点沿法线外移 4 单位），
那里正是 scatter 带（±4 体素）精确覆盖的区域，所以它们能正常命中、误差也小；
而**从任意视点**（相机）出发的射线一开始就落在洪泛填充区，那里数值不可用。

**结论（写进后续步骤的前提）**：

1. **"安全判据 PASS（不高估）"≠"场可用"**。不高估 + 严重低估 = 处处报"贴近表面"，
   sphere tracing 立刻假命中；下界质量必须与安全判据**分开**量（这一点 §5 已写，本轮给出了可视证据）。
2. 因此 L2（Surface Cache）之前必须先解决全局场的下界质量：洪泛填充区不能用当前公式，
   或近处改用逐 mesh 场（UE 的 Detail Tracing 正是这个作用），远景用硬件光追（步骤 26–29）。
3. 步骤 12 的可视化本身是达标的：它把"看不见的场质量"变成了**可复现的转储 + 可回归的数字**，
   并当场定位到问题所在 —— 这正是 L1 退出判据要它做的事。

**口径提醒（本轮踩到）**：同一个自检在不同运行里出现过 `clipmap 层数 1` 与 `层数 2` 两种状态
（日志里 `Global SDF 层 N` 行数可直接看出来），而精度指标的分母是**层 0 的体素**：
层 0 是近层时 6.16 世界单位、是唯一层（全场）时 24.64，两者相差 4 倍，指标**不可直接横比**。
§附二的 80.1% 是在"层 0 = 全场（体素 24.64）"下测得的；同一天的另一次运行（层 0 = 近层 6.16）测得
48.1%。后续每次比对精度前，先核对日志里的层数与层 0 体素。
## 附四：全局场"空旷处为 0"的追查（附三的后续）

**已知**（附三）：调试视图逐像素主射线 **100% 立刻命中**、平均步数 2.0；场剖面里层 1 的值在真值
102.10 / 63.47 / 16.15 / 4.53 世界单位的那些点上全是 **0.00**。

**第一处真 bug（已修）**：`m_LinearSampler` 原来只在 `CreateMarchGPUObjects()` 里创建，而这个函数在
`WaitGlobalCheck` 阶段才跑 —— **在 `BuildGlobalField()`（`WaitSelfCheck` 阶段）之后**。后果是每次构建
都有 **32 次**（16 mesh × 2 层）`UpdateDescriptorSet: CombinedImageSampler 缺采样器! binding=2`，
描述符更新被**整条跳过**，注入阶段的 `u_MeshField.SampleLevel(...)` 采到的是未绑定纹理（0），
而 `abs(0)=0` 又被 `InterlockedMin` 写进每个 mesh 的 AABB ⇒ 全局场大面积变成 0。

修复：线性采样器提前到 `CreateGlobalGPUObjects()`（注入之前）创建；`BuildGlobalField()` 入口增加
"缺采样器直接报错并拒绝构建"的护栏 —— 这类"跑得很正常、结果全错"的失败必须在入口拦下。
修复后日志里 `缺采样器` 归零。

**第二层原因（未修，下一轮入口）**：修掉采样器之后，剖面里的全局场在空旷处**仍然是 0.00**。
两次数值实验（代码已回退，只留记录）：

| 实验 | 注入写成什么 | 剖面读数 | 结论 |
|---|---|---|---|
| uv 标记 | `1000 + uv.x*100 + uv.y*10`（不采场） | 1029.40 → 1013.43（平滑变化） | 世界→AABB→uv 的坐标映射是活的；注入/转换/采样链路都通 |
| 场标记 | `1000 + abs(采样值)` | 恒为 **2000.00** | 采到的 mesh 场值在该区域是个**常数**，不是真实距离 |

⇒ **Mesh 场的远场（空洞区）值是 0/常数**，而注入取的是**跨 16 个 mesh 的 min**：一个大 AABB 的
mesh（边长 2788~3154）覆盖范围极广，只要它在那一点给出 0（空洞没被洪泛填出真实距离），
整个全局场在该点就被压成 0。近表面处 mesh 场是精确的（细节追踪 **236 条命中、最小 t 0.054**），
所以 256 条**起点贴着几何**的自检射线看不见这个问题 —— 与附三的结论一致。

**下一轮入口（两条都要，按顺序）**：

1. **注入不许被坏值毒化**：跨 mesh 取 min 前先判"该 mesh 在这点的场是否可信"
   （例如要求该值不小于"点到该 mesh AABB 的距离"的某个比例，或做 per-mesh 有效性掩码），
   保证一个坏 mesh 不会把全局场压到 0。
2. **修 mesh 洪泛的空洞**：当前是"每级 2 趟 + 就地竞争写"的跳步洪泛；改为更稳的形式
   （提高每级趟数，或改成确定性扫描/乒乓），把远场从"0/常数"变成随距离增长的近似距离。
   验证工具已就位：`场剖面` 与 `调试视图统计`（命中率 / 平均步数 / t 分布）。

**口径提醒（继续有效）**：精度指标的分母是层 0 的体素（近层 6.16 / 唯一层 24.64），
横比前先核对日志里的 `Global SDF 层 N` 行数；此外本轮出现 `细节追踪更近的射线 0 条`
与 `仅全局场 116/241` 这组数，都是在"全局场被压成 0"的病态状态下测的，**不能当作精度结论**。
## 附五：两处真 bug 修掉之后，全局场第一次是"真的距离场"

附四把根因钉在"mesh 场远场是 0/常数 + 跨 mesh 取 min 被毒化"。继续追下去，发现根因是两处**独立**的
实现 bug，与"洪泛质量"无关：

### bug 1：顶点索引双重偏移

`MeshBatcher` 合批时已经把 baseVertex 加进索引（`m_MergedIndices[i] = src[i] + baseVertex`），
而 `BuildQueue` 的 AABB 计算、scatter 的 `ranges.z`、自检与 `MinDistToGeometry` **又各加了一次**
`cmd.vertexOffset`。于是除第一个 mesh（vertexOffset = 0）外，所有 mesh 都在读**别的网格甚至越界**的顶点。

修法：AABB 计算不再加偏移，`MeshSDFEntry::vertexOffset` 置 0，消费侧一律 +0。实证：

- 相机到真实几何的 CPU 真值从 `102.10` 变成 **`221.01`** —— 旧值本来就是用错顶点算出来的；
- 各 mesh 的 AABB 从荒谬的 `2786 / 2788 / 2802` 变成 `57 ~ 2005`（那些"巨大 AABB"是错顶点造成的假象）。

### bug 2：描述符集别名（同一次提交里反复改写同一套描述符集）

- **convert**：一套描述符集在循环里反复改写"每 mesh 的输出纹理"；
- **全局注入**：一套描述符集在同一次提交里反复改写 binding 2（每 mesh 一张 mesh 场）。

Vulkan 语义下，这些 dispatch 会**全部看到最后一次写入的绑定**。后果：

- mesh 场纹理**根本没被写过**（保持 0 初值）。判定实验：把 convert 的输出加 1000 标记后，
  全局场剖面**仍然是 0.00**（字段写入没落到各 mesh 的场纹理上），而同一 dispatch 里的探针缓冲
  却带着标记（值域 `[1032.01, 3298.56]`）—— 证明"pass 在跑、buffer 写入有效、image 写入无效"；
- 注入因此把每个体素都压成 0；256 条自检射线全部在近表面附近"命中"，看起来一切正常。

修法：**每 mesh 一套 convert 描述符集、每（层 × mesh）一套注入描述符集、每层一套
clear/flood/convert/probe 描述符集**，全部在使用前一次性写完，循环里只 `Bind`。

### 结果（`lumen_sets`，121 帧，exit=0，VUID 46 与改动前相同）

**场剖面（沿相机中心射线，层 1 = 全场层，体素 28.44）：**

| t | 0 | 50 | 100 | 150 | 200 | 250 | 300 | 350 |
|---|---|---|---|---|---|---|---|---|
| 场值 | 197.94 | 161.03 | 117.44 | 69.99 | 28.14 | 7.20 | 28.42 | 27.51 |
| CPU 真值 | 221.01 | 196.00 | 177.39 | 171.80 | 172.95 | 175.85 | 190.50 | 216.00 |

**这是一个真实的下界距离场了**（沿射线先减后增、最小值落在真值最小处附近），
而不是之前那个恒为 0 的场。调试视图同步从"每像素立即假命中（100%）"变成 **0% 假命中**。

### 新的、也是**第一份有效**的自检读数

```
sphere tracing 自检: 两者都命中 69，仅 GPU 48，仅 CPU 88（穿漏）=> 安全 FAIL
精度：误差 ≤1 体素 12/69（17.4%），最大 54.375 体素，平均 12.265
误差分解: 仅全局场 0/69 在 1 体素内；细节追踪更近的射线 66 条
```

**注意口径**：这几组数是在"场是 0 / 场是随机垃圾"的病态状态下从未真正测过的量，现在第一次有意义。
它们说明 L1 的**真实**差距是"下界质量/紧度"：场在多数位置低估太多（例如 t=250 处真值 175.85、
场值 7.20），于是 sphere tracing 要么走不动、要么在别处提前收住。

### 下一轮入口

1. **紧度**：定位"局部低估 20 倍"发生在哪一级（mesh 场洪泛 / 全局洪泛 / trilinear 采样），
   用场剖面 + 逐 mesh 探针（`RunMeshFieldProbe`）对照 CPU 真值逐级排查。
2. 之后才是分层与精度（近层体素 7.11 / 远层 28.44 的合理配比）与 L2 Surface Cache（13–19）。

### 附五·更正：那处"低估 20 倍"是**仪表 bug**，不是场的问题

本节先前的结论"场在多数位置低估太多（t=250 处真值 175.85、场值 7.20）"**是错的，特此更正**。

原因：剖面探针由线程 (0,0) 写，而 (0,0) 是**图像角落像素**（ndc ≈ (-0.9995, -0.9991)），
它的射线方向是视锥角落方向；CPU 侧却按"相机前向"算真值 —— 两条射线在 t=50 处已相距约 59 世界单位，
越远差越多。于是"场值"与"真值"根本不是同一个点上的量。

修正仪表（剖面固定用 `normalize(camForward)`）后，同一根射线上的对照：

| t | 0 | 50 | 100 | 150 | 200 | 250 | 300 | 350 |
|---|---|---|---|---|---|---|---|---|
| 场值（层 1） | 197.94 | 198.62 | 183.61 | 164.77 | 155.14 | 162.06 | 181.08 | 201.76 |
| CPU 真值 | 221.01 | 196.00 | 177.39 | 171.80 | 172.95 | 175.85 | 190.50 | 216.00 |

⇒ 逐点误差 5~25 世界单位（≈ 0.2~0.9 个远层体素，远层体素 28.44），**场是紧的**，形状也正确。

**教训（写进工具约定）**：任何"把 GPU 采样与 CPU 真值并排打印"的诊断，必须先证明两侧算的是**同一个点**；
用 `SV_DispatchThreadID` 挑线程时，(0,0) 是角落而不是中心。

### 现在的真实状态与下一步

- 自检：`两者都命中 69 / 仅 GPU 48 / 仅 CPU 88（穿漏）⇒ 安全 FAIL`；精度 `12/69 = 17.4%`。
- Global 层自检（这次是有效读数）：层 0（7.11 体素）最大**高估 +79.6**（判据 ≤14.2）⇒ FAIL；
  层 1（28.44 体素）最大**高估 +187.2**（判据 ≤56.9）⇒ FAIL；两层的"2 体素内"比例分别 12.5% / 59.4%。
- 也就是真正剩下的缺陷是**高估**（→ 穿漏），不是低估。来源有两处，按收益排序：
  1. **近层覆盖太小**：256 条自检射线**全部**落在近层盒之外（近层只覆盖场景中心 908 单位），
     于是全部落到 28.44 体素的远层上 —— 粗层 + 跳步洪泛的系统性高估一起造成穿漏；
  2. **跳步洪泛的 fixpoint 本身是"高估"**（`d(seed)+|x-seed| ≥ d(x)`），需要按 UE 的做法补一轮
     "取带内精确值 + 用扫描/更细种子抑制高估"，或让注入也能采到近层细场。
- 下一轮入口：先把近层覆盖调到能覆盖射线起点（或让自检射线限制在近层覆盖域内做**分组**统计），
  再压高估；这两件事做完，"仅 CPU" 才有可能归零。

### 附六：近层覆盖的是一个没有几何的区域（下一轮入口）

为让自检真正测到近层，射线生成改成最多重抽 8 次、优先落在近层盒内。结果：

```
自检射线生成完毕：256 条中 256 条重抽 8 次仍落在近层盒外（近层边长 910.2，原点 (-520.7,1264.1,266.1)）
```

近层（体素 7.11）以场景 AABB 中心为心、边长仅场景的 1/4，而 16 个 mesh 的 AABB 边长 57~2789、几何散布全场 3154
—— 这块盒子里几乎没有几何。后果：全部 sphere tracing 实际只跑在 28.44 体素的远层上（近层精度从未参与）；
Global 层 0 自检两点素内仅 12.5%（层 1 反而 57.8%）；穿漏 95 条、精度 16.9%，与用粗层追踪完全吻合。

修法（下一轮）：近层改成跟随相机的 clipmap（相机位置取整到体素格，避免抖动），远景仍由覆盖全场的远层兜底，
相机移动超出半层时重建该层；相机位置需从帧图透传到 SetupGlobalGrid。
### 附七：clipmap 近层改为跟随相机（覆盖缺陷已修一半）

按 §附六 的入口，近层中心从"场景 AABB 中心"改成"相机位置取整到体素格"（远景层仍以场景中心覆盖全场）。
相机位置由帧图透传：`DeferredPipeline_FrameGraph` → `LumenProvider::StepSDF(cmd, cam)` →
`LumenScene::StepSDF(cmd, batcher, camPos)` → `LumenSDF::SetCameraPos`。

**实测（`lumen_clipmap`）**：

```
Global SDF 层 0：体素 7.111，边长 910.2，原点 (-149.3,-220.4,-455.1)   ← 相机 (308.8,238.7,0.2) 取整后 - 455
Global SDF 层 1：体素 28.444，边长 3640.8，原点 (-1886.0,-101.3,-1099.2) ← 覆盖全场
自检射线生成完毕：256 条中 48 条重抽 8 次仍落在近层盒外（改前是 256/256）
Global 层 0 自检：最大高估 +71.813，下界质量 49 点在 2 体素内（76.6%），平均低估 -8.74   ← 改前 12.5% / -36.58
sphere tracing 自检：两者都命中 92，仅 GPU 50，仅 CPU 71；精度 ≤1 体素 28/92（30.4%）
误差分布：近(d0<5) p50 = 0.882 体素                                                        ← 改前 18.590
```

**结论**：近层终于"装上几何"了 —— 覆盖外的射线 256 → 48、近层两点素内 12.5% → 76.6%、
**近场（起点贴着几何，即真实使用场景）误差 p50 从 18.59 体素降到 0.882 体素**（近层体素 7.11 ⇒ 约 6.3 世界单位），
命中数 77 → 92、≤1 体素比例 16.9% → 30.4%、穿漏 95 → 71。

**仍然剩下的缺陷**：近层实测**最大高估 +71.8**（判据 14.2）⇒ 近场穿漏 53 条还在。
来源是"跳步洪泛的 fixpoint 本身偏高"：`d(seed)+|x−seed| ≥ d(x)` 恒成立，越远越松。
下一轮的处理顺序：①注入后**再补一轮带内精确值**（scatter 的 ±4 体素带重写一遍，压掉洪泛在近表面的高估）；
②必要时把洪泛换成前后向扫描（chamfer，误差 ≤2%，确定性）；③再复测穿漏与精度。
### 附八：命中容差必须与"场的分辨率"同量级（穿漏 71 → 18）

"穿漏逐条"给出了决定性线索：某条**掠过表面**的射线，起点真实距几何 1.43、而场值 2.80，
且沿途场值最小值就是起点的 2.80（此后一路上升）—— 也就是说**在整条路径上没有任何一点的场值小于
当时的命中容差 eps = 0.25 体素 = 1.778**。而 7.11 体素的网格，其自身的局部误差本来就有 0.2~0.4 体素
（即 1.4~2.8 世界单位）：**容差比场自身的误差还小**，于是"掠过"必然被判成没命中。

修正两处（`SDF_RayMarch` 与 `SDF_DebugView` 同口径）：

- 命中容差 `eps`：`0.25 体素 → 1.0 体素`（7.11 世界单位，即与近层分辨率同量级）；
- 新增**落点修正**：判定命中后 `t += lastD`（最后一个场值），把落点补回表面（场是下界 ⇒ 不会越过表面），
  这样容差放大不会牺牲落点精度；调试视图的 eps 同步改成同口径（此前它还在用 0.25 体素，两处不对称）。

**实测**：

| 指标 | eps 0.25 体素 | eps 1 体素 + 落点修正 |
|---|---|---|
| 两者都命中 | 92 | **145** |
| 仅 CPU（穿漏） | 71 | **18**（近场 53 → 11，其中 10 条起点在近层覆盖外） |
| 精度 ≤1 体素 | 30.4% | **42.1%** |
| 近场误差 p50（起点贴几何） | 0.882 体素 | **0.740 体素** |
| 调试视图 | 命中 0.9%、平均步数 3050 | 命中 **20.3%**（近层 13.9 万 / 远层 28.2 万）、平均步数 **79.4** |

**剩余 18 条穿漏**：8 条在覆盖内（近场掠过类），10 条是"起点在近层盒外"（覆盖问题，非精度问题）；
7 条远命中（tRef ≥ 50）更像"步数/步长下限导致没走到"。下一轮：①继续压场的局部高估
（前后向 chamfer 扫描替代跳步洪泛）；②扩近层覆盖（nearFraction 或层数）把这 10 条覆盖类消掉。
### 附九：放大近层覆盖 + 步数上限 —— 穿漏 18 → 3，精度 42% → 62%

§附八 留下的 18 条穿漏里有 10 条是"起点在近层盒外"（覆盖问题）。把近层放大即可一次性消掉：

- `nearFraction 0.25 → 0.50`：近层边长 910 → **1820**（体素 7.11 → 14.22），仍以相机为心；
- `marchMaxSteps 192 → 384`：远命中（tRef 最高 398）此前会**步数耗尽**而记成"仅 CPU"；
- 顺带修一处日志 bug：分组中位数此前**没排序**就取中间元素（"近(d0<5) p50" 打出过 17.579 的假数）。

**实测**：

| 指标 | 附八（0.25 / 192 步） | 本轮（0.50 / 384 步） |
|---|---|---|
| 射线起点在近层覆盖外 | 48 | **0** |
| 仅 CPU（穿漏） | 18（近 11 / 远 7） | **3（近 3 / 远 0）** |
| 两者都命中 | 145 | **151** |
| 精度 ≤1 体素 | 42.1% | **61.6%** |
| 误差 p50 / p90 | 1.922 / 24.968 体素 | **0.563 / 6.658 体素** |
| 近命中（tRef<50）p50 | 1.258 | **0.448 体素** |
| 调试视图 | 命中 20.3%、平均步数 79.4/192 | 命中 **24.3%**、平均步数 **58.1/384** |

近层体素粗了一倍（7.11 → 14.22），但**所有**射线都被近层覆盖，整体精度反而大幅提高（p50 1.922 → 0.563 体素）
—— 这印证了 §附六 的结论：**覆盖**比"单层分辨率"更决定结果。

**现在的最大误差项已经换成"仅 GPU 93"**：即**无符号**场在几何内部的假命中（真值 0、场值也接近 0）。
这正是 §5 早就排在后面的"符号/内外判定"（步骤 11.6 的 parity 尝试对精度无改善，但对**假命中**是必需的）。
下一轮入口：①内外判定（或"起点在几何内"的显式处理）把这 93 条压下去；②随后回到主线 L2 Surface Cache（13–19）。
### 附十：L1 安全判据首次 PASS（穿漏 0）；"仅 GPU"的成因也已定性

**起因**：§附九 之后 `eps = 1 个近层体素 = 14.22`，而自检射线的起点只从表面外移 **4 单位** ——
小于 eps ⇒ 起点自己那张表面在第一步就被判成命中（大量射线的"命中"其实是 t≈0.3 的假命中），
这也是"仅 GPU 93"的主要来源。把外移量改为 **1.5 × eps（≈21 单位）**后（测试集变得良定：起点必须在容差之外）：

```
sphere tracing 自检: 射线 256，两者都命中 118，仅 GPU 101，仅 CPU 0（穿漏，须为 0）=> 安全 PASS
精度：误差 ≤1 体素 48/118（40.7%），最大 26.934，平均 3.343
误差分布: n=118 p50=1.279 p90=6.491 体素；按起点到几何距离分组: 近(d0<5) n=24 p50=0.349 / 远 n=94 p50=1.602
按命中距离分组: 近命中(tRef<50) n=79 p50=0.897 / 远命中 n=39 p50=4.681 体素
穿漏按命中距离: 近命中 0 条 / 远命中 0 条
```

**⇒ `仅 CPU = 0`：L1 的安全判据（不许穿漏）第一次 PASS。**

**口径提醒**：射线集合变了（起点 4 → 21 单位），所以"≤1 体素 40.7%"与上一轮的 61.6% **不可直接横比**；
新的基线是：命中 118、p50 1.279 体素、近命中 p50 0.897、起点贴近表面（d0<5）的 24 条 p50 0.349。

**"仅 GPU 101" 的成因（已定性，不是场坏了）**：命中判据是"场值 < eps"。
eps = 14.22 单位时，任何**从表面附近 14 单位内掠过但并未相交**的射线都会被判成命中，
而 CPU 参考要求"真的与三角形相交"⇒ 这类"近似错失（near-miss）"全部计入"仅 GPU"。
它与"穿漏"是一对：eps 变小 ⇒ 穿漏回升、仅 GPU 减少；eps 变大 ⇒ 反之。真正的出路是
**用更细的场去复核命中**，而不是继续调 eps —— 我们已经有现成的细场：逐 mesh 的 mesh 场
（体素 0.296~21.9，`SDF_RayMarchDetail` 的 eps = 0.25 × 该 mesh 体素）。

**下一轮入口**：把细节追踪从"min 归约的备选"改成**命中复核**：全局场给出候选 t 后，
用 mesh 场在该处做一次精追（或直接用 mesh 场的 eps 判定），只有当细场也认为"贴着表面"时才算命中；
否则按 near-miss 丢弃（并计入一个新指标）。这同时会把"≤1 体素"比例拉回去。
### 附十一：用细场"门控"命中 —— 试过，不行（已回退），但留下一个有用的新指标

**动机**：§附十 的"仅 GPU 101"是 eps=14.22 造成的**近似错失**（从表面 14 单位内掠过而未相交也算命中）。
自然的想法是"用更细的场复核命中"：只有逐 mesh 的 mesh 场（体素 0.296~21.9）也认为贴着表面时才算命中。

**实测（回退前）**：

```
细节追踪统计: 命中 96 条
命中复核: 全局场单方面命中 123 条被判为近似错失（细场未确认）
sphere tracing 自检: 两者都命中 45，仅 GPU 51，仅 CPU 73（穿漏）=> 安全 FAIL；精度 ≤1 体素 13.3%
```

**结论：不行**。细场**只确认了 219 条粗命中里的 96 条**（`细节追踪统计: 命中 96`），
用它的 eps（0.25 × mesh 体素）当门控，等于把大量真实命中判成 miss ⇒ 穿漏从 0 反弹到 73、
精度从 40.7% 掉到 13.3%。已回退成"粗场 ∪ 细场取 min"的策略（回到安全 PASS）。

**保留的产物**：新增一个**只记数、不门控**的指标 `命中复核: 全局场单方面命中 N 条被判为近似错失（细场未确认）`。
它把"细场能力不足"这件事变成了可回归的数字 —— 当前 N = 123（即细场只认 96 / 219）。

**下一轮入口**：先修**细节追踪本身**（每个 mesh 的 eps 与步数预算：小 mesh 的 eps 只有 0.074，
而 `marchMaxSteps` 与步长下限是全局量，导致它在细网格上走不动），让它能确认绝大多数真实命中；
那时的收益是双份的 —— 既能把"近似错失"判掉（仅 GPU 下降），又能用细场的 eps 把落点精度拉回 1 体素内。
### 附十二：细场"确认不了"的两条线索（步数预算已排除；SDF 只覆盖 16/103 个 mesh）

**实验一（已排除）**：把细场（逐 mesh）的步数预算从 384 提到 1536 —— 结果**逐位不变**
（细场命中仍是 96 条、"近似错失"仍 123 条、自检指标完全相同）⇒ 细场不是"走不动"。

**实验二（查出的硬限制）**：日志里一直有这么一行，之前没注意：

```
LumenSDF: 跳过 0 个 mesh（三角形数 > 20000）与 87 个 mesh（超出 mesh 上限 16）
LumenSDF: 待构建 16 个 mesh 距离场（分辨率 128³，预计显存 128.00 MB）
```

**场景有 103 个 mesh，SDF 只建了 16 个**（`maxMeshes = 16`，128³ × 4B × 2 张 ≈ 8 MB/mesh ⇒ 16 个正好 128 MB）。
于是：任何"最近表面属于未建 SDF 的那个 mesh"的射线，对 SDF 来说都不存在 ——
细场自然确认不了、粗场只能靠洪泛的传播值给出一个不可靠的数（这正是"仅 GPU"那批的来源之一）。

**两条互相独立的问题，必须分开处理**：

1. **覆盖（本轮查出的新事实）**：mesh 预算 16 太小。显存是硬约束（8 MB/mesh），
   可选路线：①共享 atlas（多个 mesh 拼进同一张 3D 纹理，按页分配）；②按相机距离做**按需构建/淘汰**
   （近处建、远处退化成远层粗场）；③对小 mesh 降分辨率（体素边长已按 mesh 尺寸自适应，但分辨率固定 128³）。
2. **细场的 22 条未确认命中**（CPU 参考 118 vs 细场 96）：CPU 参考与细场用的是**同一批 16 个 mesh**，
   所以这 22 条不是覆盖问题，需要在细场里逐条归因（下一轮：打印这 22 条的 tRef / tDetail / 命中 mesh）。

**当前 L1 口径（已 PASS 的部分要说清楚）**：安全判据 `仅 CPU = 0` 是相对**这 16 个 mesh 的 CPU 参考**
成立的；场景里另外 87 个 mesh 既不在 SDF 里、也不在参考里。文档后续所有 L1 指标都应注明这一点。
### 附十三：细场"未确认"的逐条归因 —— 同样指向 mesh 场的远场高估

给 `RunMarchCheck` 加了逐条打印（CPU 命中了、细场却没确认的那些射线）：

```
细场未确认 #1: tRef=23.12 粗场 t=43.78 d0=21.33 命中 mesh=#9  起点在近层内=是
细场未确认 #2: tRef=18.36 粗场 t=2.36  d0=12.23 命中 mesh=#7
细场未确认 #3: tRef=10.69 粗场 t=8.20  d0=3.67  命中 mesh=#7
细场未确认 #4: tRef=3.08  粗场 t=13.14 d0=2.40  命中 mesh=#14
细场未确认 #5: tRef=137.25 粗场 t=143.28 d0=21.33 命中 mesh=#6
细场未确认 #6: tRef=318.86 粗场 t=338.85 d0=21.33 命中 mesh=#9
```

**读法**：这些都命中**大 mesh**（#9/#6 的体素 21.8，#14 15.3，#7 9.5）。粗场（eps = 1 个近层体素 = 14.22）
在这些点上给出了与真值很接近的 t（#5：143.28 vs 137.25；#6：338.85 vs 318.86），
说明**粗场找到了表面**；而细场（eps = 0.25 × mesh 体素 = 5.45）却**没有确认** ——
细场在这条射线上**隧穿过去了**：它的 eps 抵不住 mesh 场远场的**高估**（步长 = d×0.25，
而远场 d 可高估到真值的数倍 ⇒ 一步跨过整个精确带）。

**结论**：细场不能当校验器的根因与 §附八/§附十 是同一件事 —— **mesh 场的跳步洪泛在远场系统性高估**
（`d(seed)+|x−seed| ≥ d(x)`，越远越松）。粗场之所以还能用，是靠"1 个近层体素的容差 + 跨 16 个 mesh 取 min"。

**下一轮（唯一出路）**：换掉洪泛的传播方式，压掉远场高估。首选**前后向 chamfer 扫描**
（3×3×3 邻域、权重 1/√2/√3，前后各一遍顺序推进，误差 ~2%、确定性），
其次是把精确带（scatter 的 ±4 体素）加宽做种子。做完这一项，细场才有资格做命中复核，
"仅 GPU（近似错失）"与"≤1 体素"比例才可能同时改善。
### 附十四：mesh 场换成"向量距离变换" —— 远场误差 201 → 1.33 世界单位（0.06 体素）

前面几轮一直卡在同一件事上：mesh 场的远场高估（~8%）让细场隧穿、只能用"1 个体素容差"兜住。
本轮把它从算法上解决了。

**根因**：原来的洪泛每一跳只把 `|dir|*s*voxel` 加到邻居值上 ⇒ fixpoint 是 **26 连通图上的最短路径长度**，
它恒 ≥ 真欧氏距离，各向异性带来的偏差最坏约 8%（对 2000 单位 ⇒ +187，正好是实测值）。

**新实现**（`SDF_MeshFloodSeeds.comp.slang`，+8 MB 共享的种子坐标纹理）：

- 每格存**最近种子的体素坐标**（8 位/轴打包进 u32，分辨率 ≤ 256）；
- 距离 = **种子自己的距离 + 到种子的欧氏距离**（标准 vector-DT）；
- 种子自己那格的距离在 scatter 之后不再变化（min-plus 不会把它改大），所以直接读 `u_Dist[seed]` 即可
  ——不需要额外的距离纹理。

**踩到的两个坑（都已修）**：

1. 描述符集在 `UploadGeometry` 里写、但集合是在**之后**的 `CreateGPUObjects` 才分配 ⇒ 写入是 no-op，
   着色器写未绑定描述符（VUID 46 → 68，效果为零）。改成首次使用时一次性绑定。
2. 第一版只用了"到种子的欧氏距离"、**漏掉了种子自己的距离** ⇒ 系统性**低估**，
   细场到处"立刻命中"（细节追踪最小 t = 0.002）。补上 `+ u_Dist[seed]` 后正常。

**实测（`lumen_jfa4`，VUID 回到 46）**：

```
条目 6: 值域 [32.01, 2097.40]，零值 0，最大误差 1.33，近表面 0/1 超差，远场 0/63 超差
（改前：最大误差 201.18，远场 28/63 超差）
sphere tracing 自检: 两者都命中 118，仅 GPU 103，仅 CPU 0 => 安全 PASS；精度 ≤1 体素 48/118（40.7%）
```

mesh 场的最大误差从 **201.18 → 1.33 世界单位**（该 mesh 体素 21.8 ⇒ **0.06 体素**），远场超差 **28/63 → 0/63**。

**为什么全局层的指标还没动**：全局层自己的洪泛仍是旧的"每跳加步长"版（`SDF_MeshFlood`），
它把 ~8% 的图测地偏差又加回去了；而追踪的 eps = 1 个近层体素（14.22）本来就盖过这点误差。
**下一轮**：把同样的 vector-DT 用到全局两层（每层一张种子纹理，共 +16 MB），
那时才可能把 eps 收小、把"≤1 体素"比例真正拉起来，并让细场重新有资格做命中复核。
### 附十五：全局两层也换成向量距离变换 —— 两层自检双双 PASS，下界质量 100%

把 §附十四 的算法（种子坐标 + 种子自己的距离）用到全局两层：每层加一张种子纹理（+8 MB/层，共 +16 MB）
与一套专用描述符集（binding 0 = scratch、1 = seed），把旧的"每跳加步长"洪泛（`m_GlobalFloodPSO`）整段替换。

**实测**：

| 层 | 最大高估（判据） | 2 体素内比例 | 结论 |
|---|---|---|---|
| 层 0（近层，体素 14.22） | +102.8 → **+7.637**（≤28.444） | 70.3% → **100.0%** | PASS |
| 层 1（全场，体素 28.44） | +187.3 → **+16.510**（≤56.888） | 57.8% → **100.0%** | PASS |

**顺带做的容差扫描**（场收紧后该把 eps 收回来了）：

| eps（近层体素） | 穿漏（仅 CPU） | 精度 ≤1 体素 | 命中复核（近似错失） |
|---|---|---|---|
| 0.25 | 51（FAIL） | 31.3% | 24 |
| 0.5 | 11（FAIL） | 32.7% | 87 |
| **1.0（采用）** | **0（PASS）** | **40.7%** | 125 |

结论：场自身的残余误差约 **0.5 体素**（层 0 的最大高估 +7.6 ≈ 0.53 体素），所以命中容差必须留
**2 倍余量（1 个近层体素）**才不会重新穿漏；而且容差放大**并不会**牺牲落点精度 ——
落点修正 `t += lastD` 会把距离补回来，实测 1.0 体素的精度反而比 0.25/0.5 更好。
"近似错失"随 eps 单调变化（24 → 87 → 125），与"容差越大小半径掠过越容易被判命中"一致。

**当前状态**：安全判据 PASS（仅 CPU 0）、两层场自检 PASS 且 2 体素内 100%、精度 40.7%。
剩余误差项按优先级：①`仅 GPU 103`（容差型近似错失，占"命中"的 47%）；②SDF 只覆盖 16/103 个 mesh；
③远命中 p50 4.68 体素（远层 28.44 的粒度）。之后回到主线 **L2 Surface Cache（13–19）**。
### 附十六：细节追踪里又两处真 bug（描述符集别名 + "域外即 return"）

细场（逐 mesh 细节追踪）一直"只确认 219 条粗命中里的 96 条"，此前归因于"场质量"（§附十二/十三）。
本轮找到并修掉两个实现 bug：

1. **描述符集别名（与 §附五 的 convert/注入同一类）**：逐 mesh 循环里用**同一套** `m_DetailSet` 改写
   binding 0（该 mesh 的场）⇒ Vulkan 下所有 dispatch 实际都在追**最后一次绑定的那张场**。
   这也解释了此前"把细场步数预算提到 4 倍却逐位不变"的怪现象 —— 追的根本不是那 16 张场。
   修法：每 mesh 一套描述符集，只在使用前绑定一次。
2. **`if (!InsideField(p)) return;` 把"起点不在该 mesh 场域内"的射线**全部丢掉**。
   而射线的起点通常落在**别的** mesh 上，命中点在几百单位外的这个 mesh 上 —— 这类射线恰恰是大多数。
   修法：改成"先域外大步跳过（步长 max(eps, 8)），进过域内再离开才收工"。

**实测（`lumen_keep`：修完这两处，仍用"粗场 ∪ 细场取 min"）**：

| 指标 | 改前（§附十五） | 改后 |
|---|---|---|
| 仅 GPU（假命中 / 近似错失） | 103 | **87** |
| 精度 ≤1 体素 | 40.7% | **46.6%** |
| 误差 p50 | 1.277 体素 | **1.125** |
| 近命中 p50 | 0.897 | **0.834** |
| 远命中 p50 | 4.681 | **4.051** |
| 仅 CPU（穿漏） | 0（PASS） | **0（PASS）** |

细场自身：命中 93 条、最小 t 0.096，近命中 p50 **0.553 体素**、远命中 2.331 体素。

**顺带验证了"用细场门控命中"**（只认细场确认过的命中）：`仅 GPU 8`、精度 **52.9%**、近命中 p50 0.553 ——
指标全面更好，但**穿漏回到 33**（细场仍有 33 条真命中未确认）⇒ **暂不启用门控**，保持安全 PASS 优先。
**下一轮**：把这 33 条逐条归因（很可能是"命中点在该 mesh 场域外"的边界情形，或 mesh 场在薄几何处的高估），
修完后即可启用门控，同时拿到"仅 GPU 个位数 + 精度 50%+"。
### 附十七：细场 eps 定标 + 启用"细场门控" —— 仅 GPU 103 → 53，安全仍 PASS

§附十六 里"细场仍有 33 条真命中未确认"的两个候选原因是**容差**与**几何**。用一个实验分开：
把细场 eps 从 `0.25 × mesh 体素` 放大到 `1.0 × mesh 体素`：

| 细场 eps | 细场命中 | 近似错失（粗场未确认） | 门控后的穿漏 |
|---|---|---|---|
| 0.25 体素 | 93 | 112 | 33（FAIL） |
| **1.0 体素（采用）** | **171** | **36** | **0（PASS）** |

⇒ 漏确认主要是**容差**问题（细场的 eps 太紧），不是几何。采用 `1.0 × mesh 体素`，
并**正式启用"细场门控"**（只认细场确认过的命中；粗场单方面命中计入近似错失）。

**实测（`lumen_gate3`，与 §附十五/十六 同口径对比）**：

| 指标 | 本轮开始（§附十五） | 现在 |
|---|---|---|
| 仅 CPU（穿漏） | 0（PASS） | **0（PASS）** |
| 仅 GPU（假命中 / 近似错失） | 103 | **53** |
| 精度 ≤1 体素 | 40.7% | **45.8%** |
| 误差 p50 / 平均 | 1.277 / 3.374 | **1.127 / 2.677** |
| 近命中 p50 | 0.897 | **0.711** |
| 远命中 p50 | 4.681 | **4.234** |
| 起点贴表面（d0<5）p50 | 0.349 | **0.157** |

细场自身：命中 171、最小 t 0.089。

**为什么 eps 取"该 mesh 自己的 1 个体素"**：mesh 场的算法误差只有 0.06 体素（§附十四），
但**采样**（三线性）与**步进量化**都落在该 mesh 的网格尺度上 —— eps 取 1 个体素正是"这个网格能分辨的极限"，
再紧就会把真命中判掉（0.25 体素时漏 33 条）。

**剩余**：仅 GPU 53 是"掠过表面 eps 内但未相交"的容差型判定 + SDF 只覆盖 16/103 个 mesh 的合成结果；
下一步处理覆盖（mesh 预算 / atlas），随后进 **L2 Surface Cache（13–19）**。
### 附十八：R16F + 全场景覆盖 —— 仅 GPU 103 → 18，精度 40.7% → 61.2%

前面反复出现的"仅 GPU（假命中/近似错失）"有一部分是**覆盖缺失**：SDF 只建了 16/103 个 mesh（§附十二）。
本轮把覆盖做实，两步：

1. **mesh 场格式 R32F → R16F**：与《Lumen设计与实现》步骤 8 的"≈4.2 MB/mesh"一致（原来是 8 MB/mesh 的 R32F）。
   近表面值在 fp16 下仍有 ~0.01 单位分辨率，而追踪关心的正是这一段 —— 实测 mesh 场自检误差**没有变化**
   （最大 1.33 / 0.97 / 0.91 / 0.82 单位）。
2. **`maxMeshes` 16 → 128、`meshesPerFrame` 2 → 4**：本场景 101 个 mesh 全部建场（只有 2 个因三角形数 > 20000 跳过），
   显存 **404 MB**（正是步骤 8 测算的口径），烘焙在第 26 帧完成（121 帧的测试跑得下）。

**实测（`lumen_full`，与 §附十七 同口径）**：

| 指标 | §附十七（16 mesh） | 现在（101 mesh） |
|---|---|---|
| 两者都命中 | 118 | **188** |
| 仅 GPU（假命中 / 近似错失） | 53 | **18** |
| 仅 CPU（穿漏） | 0（PASS） | **0（PASS）** |
| 精度 ≤1 体素 | 45.8% | **61.2%** |
| 误差 p50 / 平均 / 最大 | 1.127 / 2.677 / 27.0 | **0.620 / 1.693 / 17.0** |
| 近命中 p50 | 0.711 | **0.247** |
| 远命中 p50 | 4.234 | **1.221** |
| 起点贴表面 p50 | 0.157 | 0.428 |

细场命中 206 条、最小 t 0.053。**覆盖一做实，"仅 GPU"掉到 18、远命中精度提高 3.5 倍** ——
说明此前追着调的"容差/场质量"里，有相当一部分其实是"场里没有那部分几何"。

**代价与下一步**：404 MB 显存是这个方案的上限（R16F × 128³ × 101）。若要继续扩大场景或降低内存，
按 §附十二 的路线走**共享 atlas / 按相机距离按需构建淘汰**；那件事与步骤 13 起的 Surface Cache
（页表 + 页状态机）是同一套机制，进入 L2 后再一起做。
## 附十九：L2 起步 —— 步骤 13「Card 生成器 + 覆盖率可视化」

**做了什么**（`LumenSDF::BuildCards()`，CPU 版：复用自检已经准备好的顶点/索引副本，不引入新 GPU 资源）：

1. **卡片生成**：逐 mesh × 6 个轴向把三角形投影到 2D 网格，标记"有表面"的 texel。
   texel 的**世界边长**按 `cardTexelWorld = 4.0` 逐 mesh 自适应（分辨率夹在 64~512²）。
   （固定 64² 时，2789 单位的网格每 texel 43.6 单位，薄结构几乎不占 texel ⇒ 一片"假空洞"。）
2. **覆盖率**：按三角形面积加权采样 **418,329** 个表面点，记录"哪些方向的卡覆盖了它"（6 位掩码），
   于是"换一个保留阈值"不必重新采样 —— **卡片数 ↔ 覆盖率**的曲线是同一批采样上算出来的。
3. **可视化**：RGBA8 图（上半 = 覆盖最差的那个 mesh 的 6 个投影面，白 = 有表面；下半 = 逐 mesh 覆盖条，
   绿 = 已覆盖长度），以稳定名 `lumen_card_coverage` 进 GI 转储（1920 帧的采样路径里实测落盘 **384×128**）。

**实测（`lumen_cards2`，121 帧，exit=0，VUID 46 与改动前相同）**：

```
卡片生成: 101 个 mesh；卡片分辨率 64~512²（texel 0.56~7.41 世界单位，目标 4.00）；表面采样 418329 点
  保留阈值   5% 的 texel 有表面 ⇒ 卡片 410 张，覆盖率 89.7%
  保留阈值  10% ⇒ 330 张，86.8%
  保留阈值  25% ⇒ 228 张，79.1%      ← 当前默认
  保留阈值  50% ⇒  66 张，54.2%
覆盖最差: mesh #12 (3680 三角形) 0.0% / mesh #43 (50) 0.0% / mesh #44 (3408) 0.0%
```

**对照步骤 13 的验收（"可视化 SDF 追踪结果"之后的 L2 入口）**：

- **空洞与卡片设置一一对应** ✅：阈值 50% → 5% 时卡片数 66 → 410、覆盖率 54.2% → 89.7%，单调且可复现；
- **没有"不可解释"的空洞** ✅：残余 0% 的三个 mesh 都是**薄结构**（投影填充率天然很低，属步骤 9 记录的
  "不适用清单"一类），已逐条点名；把阈值降到 5% 也覆盖不到它们，正是"薄到分辨不出"的证据。

**下一轮**：步骤 14「页表 + 页状态机」——落 `SurfaceCachePageEntry` 的六态与非法迁移断言，
把本步的卡片清单映射成页（GPU 侧 + C++ 侧镜像一致）。
## 附二十：L2 步骤 14「页表 + 页状态机」（含两处平台坑）

**落地的四件事**：

1. **共享布局**：`Engine/Shader/Shaders/Lumen/SurfaceCache.slang` —— 六态常量 + `SurfaceCachePageEntry`(32 B)
   + `SurfaceCacheRequest`(16 B)，用与 `ShaderTypes.slang` 相同的手法让 **C++ 与 Slang 编译同一份定义**；
   C++ 侧再加 `static_assert`（大小 + 6 个字段偏移）把布局钉死。
2. **页状态机**：`SurfaceCacheTypes.{h,cpp}` —— 迁移真值表 `IsLegalPageTransition`（Invalid→Requested→
   Allocating→Capturing→Captured→Dirty，且**任何态→Invalid** 都是合法兜底）+ `SurfaceCachePageTable`
   （Request/Allocate/BeginCapture/EndCapture/MarkDirty/Evict/Touch/Count/Checksum）。非法迁移走
   `HE_CORE_ERROR` + `HE_ASSERT`，Release 下**拒绝且不改状态**（表永远不会进非法态）。
3. **单测**：`Tests/TestSurfaceCache.cpp`（5 例 / 73 断言）覆盖真值表（每条合法边 + 若干非法边）、
   完整生命周期（含"捕获失败→Invalid"与幂等 Evict）、非法迁移被拒后状态不变、以及
   **校验和对任一字段敏感**（改帧号/卡片号/页数都会变）。测试总数 211 → **216**，断言 5605 → **5678**。
4. **GPU 侧镜像与一致性校验**：`LumenScene_SurfaceCache.cpp` 用步骤 13 的 **228 张卡片**建 64 页演示表
   （六态齐全：Allocating 9 / Capturing 5 / Captured 41 / Dirty 9），上传为 `StructuredBuffer`，
   再用 `SurfaceCache_PageCheck.comp.slang` 按**与 C++ 逐字一致**的 32 位 FNV-1a 算校验和回读比对：

```
LumenScene 页表（步骤 14）: 卡片 228 张 ⇒ 页 64 个（1 张卡 = 1 页，演示前 64 页）；
    Invalid 0 / Requested 0 / Allocating 9 / Capturing 5 / Captured 41 / Dirty 9
LumenScene 页表镜像校验（步骤 14）: GPU 与 C++ 侧校验和一致 = 0x545eb5ad（64 页，32 位 FNV-1a）
```

**两处平台坑（都已记录在代码注释里）**：

1. **Slang 没有 `ulong`**（报 E30015: undefined identifier）→ 要写 `uint64_t`；但 64 位整数需要
   SPIR-V 的 `Int64` 能力，而它要求 `VkPhysicalDeviceFeatures::shaderInt64` —— 本引擎没开：
   实测拿到 `SPIR-V Capability Int64 was declared, but ... shaderInt64` 告警、管线无效、**回读恒 0**。
   改用 **32 位 FNV-1a**（字段位置也参与混合）后一次通过。
2. **描述符集必须在 `Initialize` 时分配**：帧中途 `AllocateDescriptorSet` 拿到的集合**写不进**
   （回读一直停在创建时 CPU 写的标记 0xdeadbeef）。把它挪到 `LumenScene::Initialize` 后，
   同一段 dispatch 代码立刻给出正确校验和 —— 这条与"资源创建延迟到帧边界"是同一类问题，
   凡"新建资源 + 当帧使用"的路径都应按这个节拍写（等待 3 帧再 dispatch、再 3 帧读回）。

**下一轮**：步骤 15 起做**页面捕获**（按页状态门控 GPU 写入）、反馈与分配/淘汰策略；随后 20–25 Screen Probe。
## 附二十一：步骤 15「Card Capture」骨架就位，但 march 命中 0（未完成，留给下一轮）

**已落地**（都跑起来了，见 `lumen_sc15f/g`）：

- **3 张 atlas**：RGBA16F，512×512 = 8×8 页 × 64×64 texel（albedo / normal / emissive）；
  albedo atlas 以稳定名 `lumen_sc_atlas_albedo` 进 GI 转储（实测 512×512 落盘）。
- **`SurfaceCache_Capture.comp.slang`**：一次 dispatch 捕获一张卡（参数走 push constant），
  组内 256 线程遍历 64² 个页 texel —— 卡平面 → 沿卡法向轴定步长 march 全局 SDF → 命中点投影到屏幕 →
  采 GBuffer 的 albedo/normal 写进 atlas；未命中/相机后方/GBuffer 无几何一律写 alpha=0。
- **页状态门控 + 预算**：只处理 `state == Capturing` 的页，每帧最多 `kMaxCapturesPerFrame = 8`；
  捕获完 `Capturing → Captured`（CPU 状态机 + 镜像缓冲同步）。
- **卡分辨率自适应**：步骤 13 的卡是自适应分辨率（常见 512²），按整数 stride 降采样进 64² 页。
- **诊断计数**：命中写入 / 未命中 / **march 命中**（区分"SDF 没找到表面"与"GBuffer 无效"）。

**实测：5 页 × 4096 = 20480 个 texel 全部未命中，`SDF march 命中 0`。**

**已排除的两条**：

1. **push constant 错位**：旧版 shader 的 cbuffer 里多了一个 `camPosW`，C++ 侧没有 ⇒ `origin/grid`
   全部偏移 16 字节 ⇒ `inside` 恒 false。已删掉该字段并把两侧对齐（注释里写明"字段顺序必须逐字段一致"）。
2. **eps/步长过小**：原来 `eps = 0.25 × 卡 texel`（≈10.9）、步长 21.6 —— 步长大于 eps 且小于场的高估
   （远层 28.4 体素 ⇒ 局部可高估 ~14）⇒ 理论上就会漏掉穿越。已改成 `eps = 1 个近层体素(14.2)`、
   `step = eps/2`。**改完仍是 0**，所以根因不在这两处。

**下一轮入口（唯一剩下的可疑点，按顺序查）**：

1. **卡平面基是否正确**：把某张卡的 `planeOrigin / stepU / stepV / axis` 与 CPU 侧算出的世界坐标并排打印
   （或让 shader 直接把第一个采样点和其 SDF 值写进统计数据缓冲回读），确认采样点确实落在 mesh AABB 内、
   且该点的 SDF 不是 1e30（`inside` 为真）。
2. 若 `inside` 为真而值很大：检查是否采到了"卡覆盖但轴向确实没有表面"的 texel（用卡自身的 fill 掩码对照，
   命中率应当≈卡的填充率）。
3. 若平面基就错了：对照步骤 13 的 `(axis, b=(axis+1)%3, c=(axis+2)%3)` 约定逐项核对（本轮已按该约定写，
   但尚未用数值验证过）。
### 附二十二：步骤 15 的"命中 0"真因 —— **push constant 超过 128 B 被截断**

§附二十一 留下的谜团（20480 个 texel 一个都不命中）本轮定位并修掉了，真因与卡平面基、eps、SDF 都无关：

> **Vulkan 的 `maxPushConstantsSize` 典型值是 128 B。** 原来我把"每卡参数 + 每帧常量"共 **15 个 float4（240 B）**
> 全塞在一个 push constant 里 —— 超出部分被截断：shader 收到的 `originVoxel0.x` 是 **NaN**、`voxel` 是 **0**，
> 于是 `SampleLayer` 的 `inside` 恒为 false、march 直接返回 1e30，一个 texel 都不命中。

**怎么抓到的**：让 shader 把 push constant 实际收到的值写进统计缓冲回读，与 CPU 侧并排比较：

```
Card 捕获 push constant 回报: originVoxel0.x=-nan（CPU -1066.64） voxel=0.00（CPU 21.33）
```

**修法**：

- **每卡参数**留在 push constant（`pageOriginRes / planeOrigin / planeStepU / planeStepV / axis / marchParams`
  = **96 B**，低于上限）；
- **每帧常量**（VP 矩阵 4 行、屏幕、两层原点与分辨率）改走 **StructuredBuffer**（binding 10，
  `CaptureFrame` 结构，持久映射，每帧 memcpy）；
- 加了一条**永久护栏**：shader 回读 origin/voxel，与 CPU 侧不一致就报 `Card 捕获常量错位` 错误。

**实测（`lumen_sc15i`，121 帧，exit=0，VUID 46 = 基线）**：

```
Card 捕获（步骤 15）: 累计捕获 5 页；命中 texel 4210 / 未命中 16270（20.6% 命中）；SDF march 命中 12213（诊断）
页表镜像校验（步骤 14）: GPU 与 C++ 侧校验和一致
```

- `SDF march 命中 12213 / 20480 = 59.6%`：与"卡的填充率 50.2%"同量级 ⇒ **march 的确在找表面**（该卡的
  CPU 真值沿轴 57.6 → 14.3 → 31.1，中途确实贴近几何）；
- `写入 4210 = 20.6%`：比 march 命中少，是因为**只有本帧在屏幕上可见的表面**才能从 GBuffer 取到材质
  （这 5 张卡大多不在视野内）—— 这正是步骤 15 的已知边界（材质源暂用 GBuffer）。

**可视化验收**：把 `lumen_sc_atlas_albedo` 转成 PNG 后**能看到场景真实材质**（石材/植被等）落在已捕获的
页里，未捕获的页是暗红（alpha=0）——"atlas 可视化与场景材质一致"这一条对**已捕获的页**成立
（工具：`build/verify/f16_rgba_png.py`）。

**步骤 15 收尾还差**（不阻塞进入 16）：①把演示页表从 64 页扩到 228 页全量捕获；
②遮挡判定（现在只做"GBuffer 该像素有没有几何"的存在性判据）；③材质源从 GBuffer 换成
bindless 逐材质求值，使 atlas 独立于屏幕（这一步与 22 的命中点着色同源）。
### 附二十三：步骤 16「Feedback」落地（含三个被修的坑）

**实现**：`SurfaceCache_Feedback.comp.slang` 把屏幕按 **16×16 分块**，每块取中心像素的 GBuffer 世界坐标，
找到"包含它的**最小** AABB"所对应的卡（= 该块需要的页），权重 = `256 像素（块覆盖）× 卡重要性
（填充率 × √边长）× 距离衰减`；**每块写自己的槽位**。C++ 侧读回全部槽位 → 过滤权重 0 → 按
`(权重降序, 页号升序)` 排序 → 取前 `kMaxCapturesPerFrame × 4` → 把 `Invalid` 页置 `Requested`，
再把前 `kMaxCapturesPerFrame` 个 `Requested` 页推进到 `Capturing` —— **闭环**：下一帧步骤 15 的捕获
就会把它们写进 atlas（这就是 L2 的"请求 → 捕获 → 可用"回路）。最后同步页表 GPU 镜像。

**实测（`lumen_sc16f`，121 帧，exit=0，VUID 46 = 基线）**：

```
Feedback（步骤 16）: 本帧请求 7730 条（16×16 分块，权重 = 覆盖×重要性×距离衰减）；
    top-32 与上一帧重叠 32 条；页表 Requested 0 / Capturing 0
```

- **7730 / 8160 块（94.7%）有几何**，与"屏幕大部分被场景覆盖"一致；
- **top-32 与上一帧重叠 32/32（100%）** ⇒ 满足"相机移动时请求列表稳定、无整屏抖动"的验收口径
  （静态相机下完全一致；因为排序含页号定序，结果**确定**）。

**过程中修掉三个坑（都值得记下来）**：

1. **世界坐标的 alpha 不是有效位**：GBuffer 的 MRT4 是 `worldPos.xyz + dielectricF0.a`，
   我一开始用 `wp.w <= 0` 判有效 ⇒ **整屏被丢掉、请求恒 0**。改成"世界坐标接近 0 才算背景"。
2. **请求计数不能用非原子读-改-写**：`slot = count; count = slot + 1;` 在上千线程下互相覆盖，
   整屏只留下 **1 条**请求；改用 `InterlockedAdd` 后正常。
3. **容量上限会让请求集合乱跳**：即使计数改成原子，"哪些块挤进 1024 条上限"取决于线程完成顺序
   ⇒ top-N 逐帧在 6/28 之间抖动，稳定性无从谈起。最终改成**每块固定槽位**（完整、确定、可复现），
   才拿到 32/32 的稳定读数。
   （另：读回必须在**清零之前**——第一版先清零再读，读到的一定是 0。）
### 附二十四：步骤 17「预算与跨帧摊销」——预算减半时收敛变慢、但单帧量不超预算

**实现**（三个预算 + 三段摊销）：

- `LumenScene`：`m_BudgetCaptures = 8` / `m_BudgetAllocations = 8` / `m_BudgetFeedbackPages = 256`
  （`SetBudgets()` 可调，验收就用它减半）；
- **三段**：Feedback 只采纳 top-`maxFeedbackPages` → **分配阶段**把 `Requested` 推进到 `Allocating`
  （每帧 ≤ 分配预算）→ **捕获阶段**把 `Allocating` 推进到 `Capturing` 并真正捕获（每帧 ≤ 捕获预算）；
- **没做完的留在原状态**（`Requested` 不回退、不丢弃），下一帧继续；
- 统计每 20 帧输出：预算 / 六态计数 / 累计捕获 / **单帧最多捕获**；前 12 帧逐帧输出便于看收敛曲线。

**验收实验（同一段场景，只改预算）**：

```
预算 8/8/256：本帧捕获 8 页（预算 8），累计 8 页
              本帧捕获 6 页（预算 8），累计 14 页   ← 2 帧收敛
预算 4/4/128：本帧捕获 4 页（预算 4），累计 4 页
              本帧捕获 4 页（预算 4），累计 8 页
              本帧捕获 4 页（预算 4），累计 12 页
              本帧捕获 2 页（预算 4），累计 14 页   ← 4 帧收敛
```

⇒ **预算减半：收敛帧数 2 → 4（变慢），而任何一帧的捕获量都 ≤ 预算（无尖峰）** ——
正是步骤 17 的验收口径。页表六态在这两个配置下最终都排空（Invalid/Requested/Allocating/Capturing 归 0，
Captured 55 / Dirty 9），说明"请求 → 分配 → 捕获 → 可用"的回路是自洽的。
### 附二十五：步骤 18「LRU 淘汰」——逻辑页 1024 / 物理页 64 下的 100% 分配成功率

**此前为什么碰不到淘汰**：步骤 14 的页表是"逻辑页 i → 物理页 i"的**恒等映射**，逻辑页最多 64 个 ⇒
永远够用，"淘汰/碎片"两条路径从来没被执行过。本轮把它改成真实结构：

- **逻辑页 = min(卡片数, 1024)**（§4 的"1024 页上限"）；**物理页 = atlas 的 8×8 = 64 块**；
- `AllocatePhysicalPage()`：池空则**淘汰最久未用**（`lastUsedFrame` 最小）且**内容有效**（Captured/Dirty）
  的逻辑页 → 释放其物理页 → 复用；**绝不动正在流水线里的页**（Requested/Allocating/Capturing）；
- Feedback 的 top-N 每帧 `Touch`（LRU 的"用"），于是"画面用到的页"自然留在池里；
- 捕获写 atlas 时用**物理页号**（此前是逻辑页号，恒等映射下恰好一致，现在必须分开）。

**验收（合成漫游：静态相机下人为轮换"需要的页"，把淘汰路径压出来）**：

```
页状态: Invalid 161 / Requested 3 / Allocating 0 / Capturing 0 / Captured 64 / Dirty 0
累计捕获 166 → 247 → 309 页，单帧最多恒 8 页
页池/LRU: 物理页 0/64 空闲；分配成功 343 → 424 → 486；失败 0；淘汰 279 → 360 → 422 次
```

⇒ **捕获吞吐不下降（单帧恒 = 预算 8）**、**分配成功率 100%（失败恒 0）**，LRU 持续回收（淘汰数单调增）——
正是步骤 18 的验收口径。

**defrag 的说明（诚实记录）**：页大小固定（64×64 texel）、池大小固定 ⇒ 任意时刻每个物理页要么属于某个
逻辑页、要么空闲，**不存在"有空间但拼不出连续块"的碎片**；分配不到一律由 LRU 回收解决。
真正需要 defrag 的是"变长页 / 需要连续多块"的场景（例如把同 mesh 的多张卡排到连续区域），
届时要做的是 atlas 内的页块搬移（一条 compute 拷贝 + 页表重写），列为后续项。
### 附二十六：L2 退出判据（步骤 19）—— 四条验收全部拿到证据，L2 闭环

本步按计划"无新增代码，只整理 dump 与对照报告"，四条验收的证据如下：

**① atlas 正确显示材质**（对照同帧 GBuffer 的真实材质）：

```
atlas albedo   : 有效 texel 23764/262144（9.1%）  亮度 p5/p50/p95 = 0.000 / 0.286 / 0.404
GBuffer albedo : texel 2073600                    亮度 p5/p50/p95 = 0.084 / 0.258 / 0.396
中位亮度之比（atlas / GBuffer）= 1.11  ⇒ 同量级（材质来自场景，不是占位色）
```

（分位差异来自 p5：atlas 里有一部分"命中但材质为暗"的 texel，以及 9.1% 的低填充率 —— 只有本帧屏幕
可见的表面能取到 GBuffer 材质，这条边界在 §附二十二 已记录。）

**② 覆盖率可视化没有"不可解释"的区域**：`lumen_card_coverage`（RGBA8，上半 = 代表 mesh 的 6 个投影面、
下半 = 逐 mesh 覆盖条）+ 阈值曲线（5% → 89.7%、10% → 86.8%、25% → 79.1%、50% → 54.2%），
残余 0% 的三个 mesh 都是**薄结构**（投影填充率天然很低），已逐条点名。

**③ 白炉 1.0000**：`gi_lumen_furnace_prov6_final.f16` 的 min = mean = max = 1.0000。

**④ 背靠背读数一致**：同配置连续跑两次，L2 相关的三张转储 **SHA-256 完全相同**：

```
lumen_sc_atlas_albedo    逐位一致
lumen_card_coverage      逐位一致
lumen_sdf_trace          逐位一致
hdr                      不同（TAA/自动曝光的时域累积，属预期）
```

⇒ **L2（Surface Cache）可以交付给阶段 D 当作"命中点材质源"**：它有稳定的页表/状态机、可验证的
捕获内容、可复现的 dump、以及确定的 LRU 分配。已知边界（不阻塞阶段 D）：材质源暂时取自 GBuffer
（屏幕可见性限制）、捕获只覆盖演示页表、遮挡判定仍是"存在性"、defrag 未做（固定页大小下无碎片）。

**工具**：`build/verify/l2_report.py`（atlas vs GBuffer 对照）、`build/verify/f16_rgba_png.py`（看图）、
`build/verify/f16_sdfdbg.py`（SDF 剖面/统计）、`build/verify/lumen_smoke.ps1`（一键跑 + 转储）。
### 附二十七：步骤 20「探针布置与自适应合并」——数量与设计量级一致、阈值曲线单调

**实现**：

- `ScreenProbe.slang`：探针的 **C++/Slang 共享布局**（96 B：位置/法线/uv + 三通道二阶 SH + meta），
  一次把步骤 23 要用的 SH 字段也定好，避免后续每加字段就动一次镜像；
- `ScreenProbe_Gather.comp.slang`：屏幕按 **16×16 像素为一个"单元"**；每 **2×2 单元（32×32 tile）**
  比较 4 个单元的平均法线（每单元 4 点采样），最差偏差在阈值内且 4 个单元都有几何 ⇒ **合并成 1 个探针**，
  否则保留每个有效单元各 1 个；探针写入结构化缓冲（原子计数取槽位）；
- **同一次运行**还把每个 tile 的最大法线偏差写进 `u_TileDev`，CPU 侧对同一缓冲取不同阈值即可得到
  完整曲线 —— 不必为每个阈值重跑一遍。

**实测（1080p，`lumen_sp20b`，exit=0，VUID 46 = 基线）**：

```
Screen Probe（步骤 20）: 单元 16×16、tile 32×32 ⇒ tile 总数 2040（有几何 2040）；
    当前阈值 cos=0.995 ⇒ 探针 5649（平坦 tile 占 41.0%）
  阈值 → 探针数曲线: cos 0.999 → 6534 / 0.995 → 5649 / 0.99 → 5169 / 0.98 → 4548 / 0.95 → 3690 / 0.90 → 3144
```

- **数量与设计量级一致**：单元 8160（= 120×68）⇒ 最严格阈值下 6534 个探针，与《设计与实现》
  §6 的"1080p ≈ 8K 探针"同量级；
- **阈值单调**：阈值放宽（cos 变小）探针数单调不增 ✅；
- **交叉验证**：GPU 原子计数（5649）与 CPU 用偏差缓冲算出的同阈值结果（5649）**完全一致**，
  说明"合并判定"两侧理解一致。

**本轮踩的两个坑（已记入注释）**：

1. `float3 n[4], p[4], v[4];` 把 `v` 也声明成了 `float3`（本意是 `float`），Slang 报
   `expected 'bool', got 'vector<bool,3>'`；
2. 共享头在**全局作用域**被 C++ 包含，而引擎的 `float4` 是 `namespace he` 内的别名 ⇒ 看不到
   （`C3646: "position": 未知重写说明符`）；C++ 分支改用 `glm::vec4/glm::uvec4` 全限定名。
   另：**计数器回读必须在清零之前**（与 §附二十三 的 Feedback 同一个坑，第二次踩到——凡是
   "原子计数 + 每帧清零"的路径都要按这个顺序写）。
### 附二十八：步骤 21「半球追踪（二维配置落地）」——非法组合在配置期被拒、光线分布 100% 在半球内

**配置（`LumenTraceConfig`）**：追踪源（SDF / HardwareRT / Screen）× 着色源（SurfaceCache / HitLighting /
Neutral）+ `traceRep`（8–16）+ `shadeRep` + `screenTrace` + `hwFarField`，首版取
**`SDF × SurfaceCache`（`screenTrace = false`）**。`Validate()` 在**配置加载期**返回中文原因并拒派发：

- **`SDF × HitLighting` 必须拒绝**（计划点名的非法组合）：SDF 的 march 只给"命中距离"，
  拿不到命中点的三角形/重心坐标/材质 UV，要做命中点光照必须用 HardwareRT；
- `traceRep ∉ [8,16]`、`shadeRep = 0`、`trace = Screen` 却 `screenTrace = false`（自相矛盾）也都拒绝。

**追踪（`ScreenProbe_Trace.comp.slang`）**：每探针按 **GGX 重要性采样**发 `traceRep` 条光线
（半向量采样后以探针法线镜面反射；探针没有视线概念，用 `V = N` 得到的分布天然以法线为中心），
求交复用步骤 11 的 march 语义（两层场取 min、`max(d*0.25, eps*0.02)`、`advanced && d < eps`）；
方向随机数用 `(探针, 光线, 帧号)` 的 PCG 哈希 ⇒ **同帧完全可复现**（背靠背读数一致的前提），跨帧更换种子
（时序累积才有意义）。

**实测（`lumen_sp21`，1080p，exit=0，VUID 46 = 基线）**：

```
Screen Probe（步骤 20）: tile 2040（有几何 2040）；阈值 cos=0.995 ⇒ 探针 5649
探针追踪（步骤 21）: 探针 5649 × 8 条 GGX 光线 = 45192 条；命中 40836（90.4%）；
    半球内 45192（100.0%，须为 100%）；配置 SDF × SurfaceCache（traceRep 8 / shadeRep 1）
```

- **单探针光线方向分布正确** ✅：全部 45192 条光线的 `dot(dir, normal) > 0`（**100% 落在法线半球内**），
  这是 GGX 重要性采样正确的必要条件；命中率 90.4% 说明方向确实指向场景（而不是随机穿空）；
- **非法组合在配置加载期即报错** ✅：5 个单测用例逐条钉住（含"SDF × HitLighting 拒绝、换成 HWRT 就合法"），
  运行期另有一道守卫（校验不过则报错并拒绝派发）。单测总数 216 → **221**，断言 5678 → **5707**。

**下一轮**：步骤 22 命中点着色（消费 L2 的 atlas：页边界处理 + 缺页中性值），随后 23 SH 投影
（白炉下误差可断言为 0）、24 合成到 Lighting、25 L3 判据。
### 附二十九：步骤 22「命中点着色」——材质改从 atlas 取，并借此修掉页表生命周期的两个真错误

**实现**：`Lumen_SurfaceCacheSample.comp.slang`（绑定 0–9）+ `LumenScene::RunSurfaceCacheShading()`，
接在步骤 21 的追踪之后、同一个 compute pass 内。对每条**命中**光线：

1. 命中点 → "包含它的最小 AABB"那张卡 → 卡对应的逻辑页 → 页表里的物理页；
2. 页状态必须是 `Captured`/`Dirty` **且 atlas 该 texel 的 alpha > 0**（捕获 shader 写的"有内容"标志）
   才算**页命中**；否则写**中性值 0.18 + `w = 0` 标记**（缺页绝不能返回黑 —— 那会在画面上留黑斑，
   交给反馈去补页）；
3. 命中点在卡平面上的 2D 坐标 → 页内 texel，**页边界夹取**并计数（实测夹取 0 次）；
4. 同时把**同一表面**的 GBuffer albedo 写进另一个缓冲，供 CPU 做"同一几何上材质一致"的对照。

**关键设计：对照样本必须"同一表面"**。探针光线打的是**二次命中**，把它重投影到屏幕像素上，
那个像素的前景通常是另一个（更近的）物体 —— 直接比较会把"几何不同"算成"材质不同"。首版没做这个
判定，12362 个样本平均差 0.34，全是噪声。改成"GBuffer 世界坐标与命中点距离 ≤ 0.3% 视距"后，
样本数降到 ~650，差值才有意义。容差必须**随视距缩放**（本场景 SDF 尺度 2730，固定容差或 1%|p|
会把相距十几单位的不同表面判成同一表面，差值稳定在 0.33 不动）。相机位置由 VP 反解（标准透视：
`R` 三行 = `normalize(vp0.xyz)/normalize(vp1.xyz)/(-vp3.xyz)`，`t = (vp0.w/|vp0.xyz|, vp1.w/|vp1.xyz|, -vp3.w)`），
不额外占 push constant。

**本轮借这一步查出并修掉的两个真错误**（都不是着色本身的问题，是"页表说 Captured 但其实没有内容"）：

1. **演示页表把所有页都占住了 ⇒ 真实请求回路彻底失效**。`BuildPageTable()` 原先对**全部** 228 页
   走一遍六态演示。而 `Request()` 只能从 `Invalid` 迁移 ⇒ 没有任何页是 `Invalid`，反馈回路
   （请求 → 分配 → 捕获）再也没动过：**全场景只捕获了 53 页，而且落在与相机无关的任意卡片上**，
   步骤 22 的页命中率只有 **0.6%**。
   改成"**只有前 24 页**走六态演示（保住步骤 14 的六态 + GPU 镜像校验样本），其余页留 `Invalid`，
   交给真实回路"。教训：**演示数据不能占用真实状态机的唯一入口条件**（`Request` 只能从 `Invalid` 迁移）。
2. **反馈请求必须按页去重**。反馈是"逐 16×16 分块"产出的，同一张卡会被成百上千个块请求到；
   取排序后的前 N 条 ⇒ 前 64 条**全是同一页**（近处那张大卡），"采纳 top-64"实际只请求到 **1** 页
   （分配成功 25 = 24 个演示页 + 1）。改成"权重最高的前 N 个**不同页**"后，请求才真正铺开。
   另：采纳上限同时受**物理页数**约束（常驻集不可能大于 atlas 页数，否则"分配→立刻被 LRU 淘汰→
   下一帧再请求"空转）。

**实测（`lumen_step22` / `s22a` / `s22b`，1080p，exit=0，VUID 46 = 基线）**：

```
LumenScene 页表（步骤 14）: 卡片 228 张 ⇒ 页 228 个（前 24 页走六态演示，其余留给真实回路）
Feedback/预算（步骤 16/17）: 请求 8040 条，采纳 top-13（与上帧重叠 13）；页状态
    Invalid 194 / Requested 0 / Allocating 0 / Capturing 0 / Captured 30 / Dirty 4
命中点着色（步骤 22）: 命中光线 41813 条；页命中 3377（8.1%）；缺页 38436（91.9%，返回中性值 0.18，
    其中不在任何卡 AABB 内 0 / 卡在但页无内容 38436）；越界夹取 0；
    atlas vs GBuffer albedo 同一表面平均差 0.1346（673 个样本，其中单卡覆盖 0、多卡覆盖 673 样本 0.1346）；
    多重覆盖命中 41813；平均亮度 atlas 0.5705 / GBuffer 0.4825
  步骤 22 归因: 多卡覆盖 3377 条命中；选卡（最小 AABB）与 GBuffer 平均差 0.1346，
    同一批样本里**最贴合**的候选平均差 0.0744（617 个样本）
```

**四条验收口径全部拿到证据**：

| 口径 | 结果 |
| --- | --- |
| 白炉真值 | `gi_s22f_prov6_final.f16` min = mean = max = **1.0000** ✅ |
| 背靠背单源逐项一致 | 同配置连跑两次：`lumen_sc_atlas_albedo` / `lumen_card_coverage` / `lumen_sdf_trace` / `albedo` **SHA-256 逐位一致**；`hdr` 不同（TAA/自动曝光的时域累积，同 §附二十六 的预期） ✅ |
| 既有预设不回归 | `gi_blend_diffuse_lumen=0` 时 `lumen_passes=0`（Lumen 完全不入帧图） ✅ |
| 单测 | 221 例 / 5707 断言全绿（本步未新增用例，改动集中在 GPU 侧与新 pass） ✅ |

**三个必须记下来的反例（本轮实测，不是推测）**：

1. **"页状态 = Captured"不等于"atlas 里有内容"**。加上 `alpha > 0` 判定后，页命中率从 27.1% 掉到 8.1%
   —— 也就是说之前有 **2/3 的"页命中"读的是空 texel**（捕获 shader 对"命中点不在屏幕内 / 被遮挡"
   的 texel 写 alpha = 0）。atlas 平均亮度也从 0.30 回到 0.57（0.30 是被空 texel 拉低的）。
   ⇒ 步骤 15 的收尾项③（材质源独立于屏幕）是**内容覆盖率**的直接瓶颈，不再是"锦上添花"。
2. **"选卡歧义"是真的、且可定量**。卡片用**立方 AABB**（边长取 mesh 包围盒最大轴），实测**页命中的
   光线 100% 落在 ≥2 张卡的 AABB 内**（单卡覆盖样本一个都没有）。同一批样本里，"与 GBuffer 材质最
   贴合的那个候选"平均差 **0.0744**，而当前策略（最小 AABB）是 **0.1346** ——
   ⇒ 误差里约 **0.06 来自选卡**，剩下 0.074 来自 texel 分辨率 / 捕获内容本身（含 atlas 比 GBuffer
   系统性偏亮 ~1.17×，与 §附二十六 记录的中位亮度比 1.11 同源）。
3. **"页命中率低"要分两段归因**，不能笼统说"缓存没建好"。缺页 38436 条里
   **"不在任何卡 AABB 内" = 0**、**"卡在但页无内容" = 38436**：命中点全都落在某张卡的 AABB 里，
   缺的是**那一页没被请求/没被捕获** —— 而反馈只按**屏幕可见**投请求（静态相机下只覆盖到 13 张卡），
   探针的二次命中会打到屏幕外/被遮挡的表面。⇒ 步骤 23 之后应把**探针命中点**也接到反馈请求上。

**已知边界（不阻塞 L3）**：选卡仍是"最小 AABB"（未做基于卡深度/法线的消歧）；atlas 材质源仍取自
GBuffer（屏幕可见性限制）；页命中率 8.1% 是上述两条的直接结果。

**下一轮**：步骤 23 SH 投影（把步骤 21 的 `traceRep` 条光线投影到 `ScreenProbe` 已有的 SH 字段，
白炉下误差可断言为 0），随后 24 合成到 Lighting、25 L3 退出判据。