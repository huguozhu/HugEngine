# HugEngine GI 优化——后续开发计划

> 日期：2026-09-08
> 依据：`docs/技术分析文档/HugEngine全局光照GI实现分析与架构优化方案.md`（分析 → 架构 → 落地 三部分合并文档）

---

## 一、总览

**核心结论**（来自基线 F1）：骨架已存在（`LightingSource`/`LightingInputSources` 已定义未接线），**问题在"接线"**，不是缺算法。所以路线是**增量改造，不重写管线**。

**执行约定**（结合项目规则）：
- 每个任务**编译通过 ≠ 完成**，必须在 `06.GILab`（Sponza GI 对比 sample）跑 exe 冒烟测试通过才算完成。
- 每完成一个里程碑，**停下等用户实测确认**后再进入下一个。
- 不自动 commit，每个合入点由用户确认。

**测试载体**：`06.GILab` sample（文档明确要求用它验证各档位组合；Sponza 场景是理想对比样本）。

---

## 二、里程碑地图（一览）

| 里程碑 | 内容 | 对应文档 | 优先级 | 规模 | 是否阻塞后续 |
|---|---|---|---|---|---|
| **M0** | 正确性 Bug 修复（3 处） | 第一部分·三·🔴 | 最高 | 小 | 否，但应先做 |
| **M1** | 接口收敛 + shader 通道化（Phase 1+2） | 第三部分·2/3 | 高 | 中 | 是（地基）|
| **M2** | 数据驱动 + 帧图自动编排（Phase 3+4） | 第三部分·4/5 | 高 | 中 | 否 |
| **M3** | Provider 注册表 + 自动降级（Phase 5，可选） | 第三部分·6 | 中 | 小 | 否 |
| **M4** | 性能优化（halfRes 等 5 项） | 第一部分·三·🟠 | 中 | 中 | 否 |
| **M5** | 质量提升（RTGI 累积、DDGI 修正） | 第一部分·三·🟡 | 中 | 中 | 否 |
| **M6** | 工业界进阶（ReSTIR GI 等） | 第一部分·五 | 按需 | 大 | 否 |

---

## 三、各里程碑详细任务

### M0 · 正确性 Bug 修复（先行，风险最低）

> 理由：两份文档一致把"先修 bug"放最前——低成本、消除隐性错误，且这些错误会在后续重构验证时干扰判断（例如 SSGI"半残"会让 M1 的行为等价验证失真）。

| 任务 | 位置 | 做法 |
|---|---|---|
| 0.1 SSGI 投影矩阵 | `SSGI.frag.slang:34` + `GI_SSGI.cpp:93` | 正向投影矩阵一并传入 UBO，参考 `SSAO.cpp:198-205` 同时传 `u_InvProj`+`u_Proj` |
| 0.2 DDGI 参数统一 | `RT_DDGI.slang:5-11` + `GI_DDGI.h:47` | 网格参数改 UBO/SSBO 传递，消除 C++/shader 三处不同步 |
| 0.3 RSM 守卫字段 | `DeferredLighting.frag.slang:245` | 校验 `shadowParams.w` 光源类型 + `lightCount>0`，参考同文件 210-211 行 |

**验收**：`06.GILab` 分别开关 SSGI/DDGI/RSM，无错位/越界/脏数据；SSGI 视觉恢复正常。**→ 暂停等用户测试。**

---

### M1 · 接口收敛 + shader 通道化（Phase 1+2，地基）

> 理由：文档明确"Phase 1+2 一起合，先打地基"。纯内部重构、行为等价、风险低。这一阶段也顺带解决分析文档的 **Bug#10（魔法系数）与 #12（双重计入）**，不必单独另做。

| 任务 | 文件 | 要点 |
|---|---|---|
| 1.1 收敛 `Render` 签名 | `LightingPass.h/.cpp` | 扩展 `LightingSource` 枚举、`LightingInputSources`（ambient/强度/fallback）、`LightingInputs` 结构；`Render` 33→6 参数 |
| 1.2 shader 去魔法系数 | `ShaderTypes.slang` + `DeferredLighting.frag.slang` | push constant 加 `giIntensity`/`aoIntensity`；移除 `*0.03`/`*0.5`；DDGI 改由 `diffuseFallback` 控制 |
| 1.3 消除双重计入 | 同上 | `rtDiffuseSource` 布尔升级为 `diffuseSource` 枚举；关 DDGI 后贡献严格为 0 |

**验收**（文档第三部分·2/3）：
- [ ] `Render` 参数 33→6；`DeferredPipeline_FrameGraph.cpp` 与 `HybridRTPipeline.cpp` 调用点编译通过、**行为不变（仅重构）**。
- [ ] 魔法系数全移除，由 push constant 强度驱动；关 DDGI 贡献严格 0。

**→ 暂停等用户测试（重点对比重构前后画面应完全一致）。**

---

### M2 · 数据驱动 + 帧图自动编排（Phase 3+4）

| 任务 | 文件 | 要点 |
|---|---|---|
| 2.1 `GIConfig` + 质量档位 | `GI/GIConfig.h/.cpp`（新增） | 配置结构 + `kGIPresets[4]`（Low/Medium/High/Ultra）+ `ToInputSources()` |
| 2.2 ImGui 面板 | 对应 UI | 4 通道 `Combo` + 档位 `Combo` + 每通道强度 `SliderFloat` |
| 2.3 帧图条件注册 | `DeferredPipeline_FrameGraph.cpp` | 各 GI pass 用 `shouldRun` 判定，未选中的不注册不分配 |

**验收**：
- [ ] 切换档位只改一个 `GIConfig`，无需重建 PSO；ImGui 任意组合 4 通道实时生效。
- [ ] RenderDoc 确认未选中的 pass 不注册；切换 provider 后帧图节点数随之变化。

**→ 暂停等用户测试（在 `06.GILab` 里遍历各档位组合）。**

---

### M3 · Provider 注册表 + 自动降级（Phase 5，可选）

| 任务 | 文件 | 要点 |
|---|---|---|
| 3.1 注册表 + 降级链 | `GI/GIRegistry.h`（新增） | `Register/Create/IsAvailable` + `FallbackOf` 降级表（RTGI→DDGI→SSGI→None） |

**验收**：不支持的设备上 High 档自动落到 Medium 组合。**→ 暂停等用户测试。**

> M3 可独立，不影响 M0–M2；若时间紧可后置或跳过。

---

### M4 · 性能优化（性价比从高到低）

| 任务 | 位置 | 杠杆 |
|---|---|---|
| 4.1 `halfRes` 落地 | SSGI/SSR/RSM/DDGI/SSAO | **最大**，省约 3/4 像素着色开销 |
| 4.2 SSR Hi-Z 层级追踪 | `GI_SSR.cpp:59` + `SSR.frag.slang` | 步数 64→~log2，质量反升 |
| 4.3 DDGI 1/4 分辨率 HDR | `DDGI.comp.slang:147` + `GI_DDGI.cpp:229` | 8192 次全分辨率采样带宽立减 |
| 4.4 RSM VPL 降采样 | `DeferredLighting.frag.slang:249-251` | 25→16 点 / Poisson 盘 |
| 4.5 GBuffer 通道合并 | `DeferredLighting.frag.slang:82-94` | metallic/roughness 打包 R8G8B8A8 |

**验收**：帧时间对比，各 halfRes/优化项下视觉无损。**→ 每项可单独暂停测试（4.1 优先单独验证）。**

---

### M5 · 质量提升

| 任务 | 位置 | 要点 |
|---|---|---|
| 5.1 RTGI 时域累积/降噪 | `RT_GI.rgen.slang` | 补 history buffer + 重投影 + 降噪（当前 SPP=1 噪点严重）|
| 5.2 DDGI 探针屏幕外更新 | `DDGI.comp.slang:133` | 探针射线 march 或宽范围采样 + 更低 `blendAlpha` |
| 5.3 DDGI SH 修正 | `DDGI.comp.slang:155-159` + `RT_DDGI.slang:33` | 投影乘 `cos` + 评估端去 `max(result,0)` 截断 |

**验收**：噪点/鬼影/辐照度畸变对比改善。**→ 暂停等用户测试。**

---

### M6 · 工业界进阶（按需 / 长期）

| 任务 | 说明 | 备注 |
|---|---|---|
| 6.1 ReSTIR GI | 复用现有 ReSTIR DI 基础设施推广到间接光 | **性价比最高**，实现 `GIMode::ReSTIR` |
| 6.2 DDGI→RTXGI | 探针 relocation/可见性/无限滚动 | 不动现有架构 |
| 6.3 SSAO→GTAO | 成本低、画面提升明显（UE 默认）| |
| 6.4 长期方向 | Lightmap、VXGI/LPV/SVOGI、NRC 神经缓存 | 不在近期路线 |

---

## 四、依赖与顺序说明

1. **M0 必须先做**：三处 bug 是隐性错误，且会影响 M1「行为等价」的验证基准。
2. **M1 是地基**：文档明确 Phase 1+2 先合；先把 `LightingPass` 签名和 shader 通道化稳定，后续 M4 性能工作才不会返工（M4 会触碰帧图与 pass 尺寸，若在 M2 前做会重复改动）。
3. **M2 紧随 M1**：M2 依赖 M1 的结构体与枚举（`GIConfig.ToInputSources()` → `LightingInputSources`）。
4. **M3 独立**：随时可做，不影响主线。
5. **M4/M5/M6 在架构稳定后**：都是增量，可并行推进，但每项需单独实测。

**一句话顺序**：`M0 修 bug → M1 打地基 → M2 数据驱动 → M3(可选) 降级 → M4 性能 → M5 质量 → M6 进阶`。

---

## 五、风险与验证策略

- **重构等价性风险**（M1）：最大风险是"改了但视觉变了"。用 `06.GILab` 重构前后截图/帧时间对比，逐项确认无回归。
- **枚举/结构体跨文件改动**：`LightingSource` 枚举、push constant 是 C++/slang 双侧同步，改一处必须两侧一起改（这是文档强调的 F1 痛点）。
- **每个任务开工前**先复核文档引用的行号是否仍准确（代码可能已漂移），再动手。
