# Lumen + Nanite 完整实现设计规范

> HugEngine 延迟渲染管线集成动态全局光照 (Lumen) 与虚拟化几何 (Nanite)

## 1. 现有基础设施

| 能力 | 状态 | 用途 |
|------|:---:|------|
| VK 1.3 + RT (AS + RT PSO + SBT) | ✅ | Lumen 远场 HW RT 追踪、Nanite BVH 遍历 |
| VK_EXT_mesh_shader | ✅ | Nanite Cluster 硬光栅 |
| GPU Culling (Hi-Z + Two-Phase + PTG) | ✅ | Nanite Instance/Cluster 剔除 |
| VK_EXT_device_generated_commands | ✅ | Nanite 间接绘制生成 |
| GPU WorkGraph (软件模拟) | ✅ | Nanite 剔除链 → Draw 链 |
| Bindless Textures | ✅ | Surface Cache Atalas、Nanite 材质 |
| AsyncCompute | ✅ | SDF 更新、Surface Cache 更新 |
| DDGI (探针 GI) | ✅ | 升级为 Radiance Cache |
| GBuffer DeferredPipeline | ✅ | Nanite Phase 1 写入目标 |
| ClusteredShading LightGrid | ✅ | Lumen 命中点直接光照 |
| Denoiser (5×5 双边) | ✅ | Screen Probe Gather 空间滤波；**统一降噪框架**见 §5.1 |
| meshoptimizer | ✅ | Nanite 预处理 Cluster/LOD |
| VMA | ✅ | GPU 内存管理 |

---

## 2. Lumen — 动态全局光照

### 2.1 数据流

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

### 2.2 Surface Cache

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

### 2.3 SDF 体系

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

### 2.4 Screen Probe Gather

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

**两个追踪路径切换**：
```
if (rayDistance < MaxSDFTraceDistance)    // 50m 内
    Compute Shader SDF Trace (spawn from screen probe CS)
else
    Indirect TraceRay (HW RT, secondary rays from screen probe CS)
```

### 2.5 Radiance Cache (升级 DDGI)

| 变更 | DDGI (当前) | Radiance Cache (目标) |
|------|------------|----------------------|
| 探针表示 | SH 三波段 (9 coeffs) | SH 二阶 (4 coeffs)，RGB 独立 |
| 探针分布 | 均匀 3D 网格 | 自适应密度 (基于几何复杂度) |
| 追踪方式 | Fibonacci 球面采样 GBuffer | Screen Probe Gather 输入 |
| 时间混合 | 指数移动平均 | History 重投影 + 时间混合 |
| 插值 | 三线性探针插值 | 三线性 + 距离权重 |

---

## 3. Nanite — 虚拟化几何

### 3.1 数据流

```
预处理 (Python)
─────────────────
  Input Mesh (.gltf/.obj)
    → Cluster 划分 (64 tri/cluster, 128 vertices max)
    → LOD 生成 (edge collapse, 50% tri/level, ~6 levels)
    → DAG 去重 (共享相同 cluster)
    → 压缩编码 (顶点量化 R10G10B10A2, 位压缩)
    → .nanite 文件

运行时 (C++/VK)
─────────────────
  Upload .nanite → GPU
    → Instance Culling (Frustum + Hi-Z)
    → Persistent Cluster Culling (BVH 遍历 + 两阶段)
    → LOD Selection (projected error < pixel threshold)
    → Software Raster (Compute, small tris) / Hardware Raster (Mesh Shader, large tris)
    → GBuffer (Phase 1) / Visibility Buffer (Phase 2)
```

### 3.2 预处理 (Python)

```python
# 工具链：meshoptimizer + 自定义 Python 脚本

class NanitePreprocessor:
    def process(self, mesh: Mesh) -> NaniteAsset:
        # 1. Cluster 划分
        clusters = self.build_clusters(mesh, max_tris=64, max_verts=128)
        
        # 2. LOD 生成 (边折叠，每级 ~50% 三角形)
        lods = [clusters]  # LOD0 = 原始
        for level in range(5):
            simplified = self.edge_collapse(lods[-1], ratio=0.5)
            lods.append(simplified)
        
        # 3. DAG 去重 (跨 LOD 共享相同 cluster)
        dag, dedup_table = self.build_dag(lods)
        
        # 4. 量化编码
        quantized = self.quantize(dag, bits=10)  # R10G10B10A2
        
        # 5. 打包 .nanite
        return self.pack(quantized, dedup_table, mesh.materials)
```

| 步骤 | 工具 | 输出 |
|------|------|------|
| Cluster 划分 | meshoptimizer `meshopt_buildMeshlets` | Cluster[] (索引 + bounds) |
| LOD 生成 | meshoptimizer `meshopt_simplify` | 每级 clusters |
| DAG 去重 | 自定义 (哈希 cluster 内容, 查找相同) | dedup map |
| 顶点量化 | 自定义 (Q10.10.10.2 per axis) | quantized vertex buffer |
| 打包 | 自定义 binary format | `.nanite` 文件 |

### 3.3 运行时 (C++)

#### Cluster 剔除 (两阶段)

```
Phase 1: Instance Culling
    Frustum cull → Hi-Z occlusion → output visible instances

Phase 2: Persistent Cluster Culling (per-instance)
    For each visible instance:
        BVH traverse (cluster tree, depth-first)
        Frustum cull cluster bounds (compute shader, 64 threads)
        Hi-Z occlusion cull (sample Hi-Z pyramid)
        → Indirection Buffer (compact visible clusters)

Phase 3: LOD Selection (per surviving cluster)
    projectedError = cluster.maxError / distance
    selectedLOD = selectLevel(projectedError, threshold=1 pixel)
```

#### 混合光栅化

```
Hardware (Mesh Shader):
    cluster.triCount > 16 → Mesh Shader (VK_EXT_mesh_shader)
    硬件处理大 cluster，高效

Software (Compute Shader):
    cluster.triCount <= 16 → Compute Shader 软件光栅化
    每个 cluster 一个 wave，interlock 写 GBuffer
    小三角形群硬件开销大，软件更优

写入目标:
    Phase 1: GBuffer (复用现有 DeferredPipeline)
    Phase 2: Visibility Buffer (triangleID + depth, 延迟材质评估)
```

#### GBuffer 路线 (Phase 1)

```
优势:
  - 复用现有 DeferredPipeline 全部后处理
  - 直接可以验证 Cluster 渲染正确性
  - 不需要改 Lighting Pass

做法:
  - Nanite 光栅化直接写现有 5×MRT GBuffer
  - Lighting Pass 照常从 GBuffer 采样
```

#### Material Bin

```
按材质分组 cluster → 绑定 Bindless 纹理 array
一次 Draw/Dispatch 处理同一材质的多个 cluster
减少 bindless descriptor 切换
```

### 3.4 运行时 GPU 资源

| 资源 | 大小 (估算) | 说明 |
|------|-----------|------|
| Cluster Buffer (SSBO) | ~16MB per mesh | cluster bounds + BVH + indices |
| Vertex Buffer (SSBO) | ~32MB per mesh | 量化顶点 (R10G10B10A2) |
| Index Buffer (SSBO) | ~8MB per mesh | cluster 索引 |
| Indirection Buffer (SSBO) | ~512KB | 紧凑化后的可见 cluster 列表 |
| LOD Error Buffer (SSBO) | ~64KB | 每个 cluster 的最大几何误差 |

---

## 4. 与现有管线集成

### DeferredPipeline 扩展

```
BuildFrameGraph 新增 Pass:
    [Lumen]
    GPU_Cull → Shadow → SurfaceCache_Update → SDF_Update →
    GBuffer → ScreenProbeGather → SpatialFilter → TemporalFilter →
    RadianceCache_Update → Lighting (读 RadianceCache + SurfaceCache)
    
    [Nanite (Phase 1)]
    GPU_Cull → Nanite_ClusterCull → Nanite_LODSelect →
    Nanite_Rasterize(GBuffer) → Lighting → 后处理链
```

### 新 Shader 文件

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

    Nanite/
        Nanite_Preprocess.py            Python 预处理工具
        Nanite_InstanceCull.comp        实例视锥 + Hi-Z 剔除
        Nanite_ClusterCull.comp         两阶段 Cluster 剔除
        Nanite_LODSelect.comp           LOD 选择
        Nanite_SoftRasterize.comp       Compute Shader 软光栅
        Nanite.mesh                     Mesh Shader 硬光栅
```

---

## 5. 分阶段里程碑

### 5.1 统一降噪框架（自《HugEngine GI 架构与开发计划》迁入）

> 这项工作原本挂在 GI 计划的任务 11（统一降噪框架）下，但它**真正的消费方是本项目的
> Lumen**（多信号共存：Screen Probe Gather / Radiance Cache / 反射 / 阴影 / 探针）。
> 因此任务与它的状态一并迁到这里，GI 计划只保留设计与现状对照（该文档 §4.4）作为背景。
> **本节的判据与证据原样保留**，不要因为换了文档就把它当成"未做过的事"。

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

---

### 5.2 Provider 执行单位收敛与绑定数组化（P6 / Lumen 的框架前置，自 GI 计划迁入）

> 这两项原本挂在 GI 计划的任务 19 / 20 下。它们的验收对象**只有 P6 与 Lumen** ——
> 没有消费方的泛化无法验收（GI 计划 §4.3.5 的"三层改造"是它们的设计背景，那节留在
> GI 文档里）。因此任务与状态一并迁到这里：**GI 计划只保留设计，不再列任务**。

**19 · PROVIDER-EXEC：执行单位从「Provider × 通道」改为「Provider」**

- 目标：帧图的执行单位变成"一个 Provider 每帧只注册一次 pass"，即使它的源 id 同时出现在
  多个通道的层栈里 —— **这正是 Lumen 的天然形状**（一份估计量同时喂漫反射与镜面）。
- 现状：GI 计划的帧图有 7 条按 source id 定制的循环；一个 Provider 若同时 `Handles(SSGI)`
  与 `Handles(SSR)`，会被两条循环各跑一遍。
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
- 状态：**长期项**，未开始；它的位置在 Lumen（L1–L5）之后与 P6 一起，见 §5.3 的推进顺序。
- **落点确认（2026-09-19）**：`全路径追踪管线规划.md` §12 B7（ReSTIR PT / GRIS）的落点**确定归本项的 P6**（PT 侧不实现，只保留"薄适配"接口：PT 的 RayGen 采样共享内核输出的 radiance + validity 纹理）。该决策的依据、代价评估（实测成本表 / 显存 / 冷缓存编译）与执行里程碑（M1 GRIS-lite → M2 完整 GRIS → M3 与 DI/P6 统一）见 `docs/计划实现功能/ReSTIRPT_GRIS预研.md`（含 §8 通用性分析：哪些层可共用、哪些必须各写一套）。

> **与"统一降噪框架"（§5.1）的关系**：两者都是"多信号共存"的框架前置 —— §5.1 管降噪的
> 信号分类与历史分配，本节管**执行单位与绑定**。Lumen 接入时会同时撞上这两件，故建议
> 一起设计、分步提交。

---

### 5.3 里程碑总表

#### Lumen

| 里程碑 | 内容 | 验证标准 |
|--------|------|----------|
| **L1: SDF** | Mesh SDF 生成 + Global SDF + SDF Ray Marching | 可视化 SDF 追踪结果 |
| **L2: Surface Cache** | Card Capture + Page Table + Feedback | Atalas 正确显示材质 |
| **L3: Screen Probe** | 探针放置 + SDF 追踪 + SH 投影 | 半球追踪产生漫反射 GI |
| **L4: HW RT 远场** | 远场切换 HW RT + 混合追踪 | SDF 近 + RT 远正确混合 |
| **L5: Radiance Cache** | 升级 DDGI → 二阶 SH + 自适应密度 | 室内/室外稳定 GI |
| **L6: 降噪+优化** | 时间混合 + 空间滤波 + 异步 Compute（**前置：§5.1 的统一降噪框架**） | 60fps @ 1080p |

#### Nanite

| 里程碑 | 内容 | 验证标准 |
|--------|------|----------|
| **N1: 预处理** | Python Cluster 划分 + LOD + 打包 | 输出 .nanite 文件 |
| **N2: 上传+剔除** | GPU Buffer + Instance/Cluster 剔除 | 可见 cluster 正确 |
| **N3: 软光栅** | Compute Shader 写入 GBuffer | Sponza 正确渲染 |
| **N4: 硬光栅** | Mesh Shader 处理大 cluster | 混合光栅性能提升 |
| **N5: LOD 流式** | 运行时 LOD 选择 + 反馈 | 帧率稳定，无 pop |
| **N6: 材质批次** | Material Bin + Bindless | 多材质场景无 Draw 爆炸 |

#### 建议推进顺序

```
N1(预处理) → N2(剔除) → N3(软光栅 GBuffer) → L1(SDF) → L2(SurfaceCache)
→ L3(ScreenProbe) → N4(硬光栅) → L4(HW RT远场) → L5(RadianceCache)
→ §5.1 统一降噪框架（11.3） → L6+N5+N6(优化)
→ §5.2 框架前置（Provider 执行单位 / 绑定数组化）+ P6 统一估计器（长期）
```

先跑通 Nanite 基本渲染（N1-N3），因为它产出 GBuffer 写入能力。然后基于 Nanite 的 GBuffer 上 Lumen（L1-L3）。

---

## 6. 关键数据结构

### Nanite Cluster (GPU)

```cpp
struct alignas(16) NaniteCluster {
    float4 boundingSphere;       // xyz=center, w=radius
    float4 coneData;             // normal cone (法线锥剔除)
    uint   triangleOffset;       // index buffer 中的偏移
    uint   triangleCount;        // 三角形数 (≤64)
    uint   vertexOffset;         // vertex buffer 中的偏移
    uint   materialID;           // 指向 bindless 材质
    float  maxParentLODError;    // 切换到父级 LOD 的误差阈值
    uint   childClusterOffset;   // BVH 子节点索引
    uint   childCount;           // BVH 子节点数
    uint   _pad;
};
```

### Surface Cache Page

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

### Screen Probe

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

---

## 7. 已知风险

| 风险 | 缓解措施 |
|------|---------|
| SDF 生成性能 | 预处理阶段完成 Mesh SDF，运行时只更新 Global SDF 注入 |
| Surface Cache Atalas 碎片化 | LRU + 定期整理 (defrag pass) |
| 软件光栅化效率 | 仅对小 cluster (≤16 tri)，大的走 Mesh Shader |
| 两台追踪路径切换 discontinuity | SDF 最大距离结束前 N 步做 overlap fade |
| .nanite 格式版本兼容 | 文件头版本号 + semantic versioning |
