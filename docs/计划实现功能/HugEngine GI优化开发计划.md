# HugEngine GI 开发计划（总纲）

> 日期：2026-09-13
> **本文件由两份文档合并而成，此后作为 GI 工作的唯一计划入口**：
> - `HugEngine GI优化开发计划.md`（原 M0–M6 里程碑计划 + 提交级演进记录）→ 其内容**逐字保留**在 **附录（§历史记录）**；
> - `HugEngine GI分层合成后续开发计划.md`（代码审计 + Wave 0–5 后续波次）→ 构成下面的主线。
> 依据：`HugEngine GI分层合成架构设计.md`（设计）；`docs/技术分析文档/HugEngine GI全局光照实现分析与架构优化方案.md`（分析）
> 当前基线：HEAD `6a24b15`；`06.GILab` 已可 configure / 编译 / 运行（实证见 §1.3）
> **合并原则：信息不丢**——已完成里程碑的原始任务描述、验收标准、提交哈希全部保留在附录；
> 主线只回答"接下来做什么、按什么判据验收"。

---

## 〇、总览与执行约定

### 0.1 核心结论

1. **骨架已存在、问题在"接线"**（原基线结论）：`LightingSource`/`LightingInputs` 等已定义并接线，
   GI 的短板不在"缺算法"，而在**合成正确性**与**抽象统一** → 路线是**增量改造，不重写管线**。
2. **分层合成已落地**：层栈（`GIChannelStack`）+ 归一化加权合成（P1/P2/P3）、光追归入 GI 源（S1/S1.5）、
   管线维度收敛（S2/S3）、PT 定位为参考渲染器——均已在代码中确认（§1.1）。
3. **4 档档位 / `PipelineCaps` 降级 / 通道枚举体系保留复用**：后续波次改的是"层栈内容与精度"，
   不改档位体系，更不动管线架构。
4. **当前真正的瓶颈是"验证能力"而非"算法"**：`06.GILab` 在此前从未被编译运行过（§1.3），
   导致 9/8–9/11 的 12 个 GI 提交处于"代码已写、验证待补"状态 → 故 Wave 0 以**建立判据**为第一优先。

### 0.2 本计划的四项关键输入（决定优先级）

1. **验证基线此前不存在**：`06.GILab` 从未编译、从未运行（§1.3）；**该门槛现已打通**——
   工具链齐备、编译 `BUILD EXIT: 0`、连续运行到 Frame 431–530。
2. **文档未记录的 3 处结构性缺口**（§1.4）：不修则白炉测试必然失败、P4/P5 也无从验证。
3. **M5.1（RTGI 时域累积）实际已被 S1 的降噪链覆盖**（§六）：应从待办划掉，避免重复劳动。
4. **运行曾出现偶发崩溃**（首轮 5 次启动 1 次 `0xc0000005`）→ 提升为 **Wave 0.1**，先于白炉测试；
   已定位到函数级（§1.3.2），并加了崩溃处理器（Wave 0.8）以便下次直接读到调用栈。

### 0.3 执行约定

1. 每个任务**编译通过 ≠ 完成**——必须在 `06.GILab` 跑 exe 冒烟 + 用户实测确认。
   ⚠️ 这条约定在 9/7–9/11 期间实际未被执行过（§1.3）。**现在基线已具备，恢复执行**，
   并额外要求：**跑的时间要够长**（偶发崩溃曾 5 次里出现 1 次，短测会漏）。
2. 每个里程碑**停下等用户确认**再进下一个。
3. 不自动 commit；合入点由用户确认；commit log 中文、无 AI 相关字样、无格式垃圾字符。
4. 开工前**复核文档引用的行号**（本次审计已发现行号漂移与数据模型漂移）。
5. 添加代码**必须附中文注释**。

### 0.4 状态标记口径（统一）

| 标记 | 含义 |
|---|---|
| `✅ 已落地` | 代码已写 **且** 在当前 HEAD 编译通过 **且** `06.GILab` 实测确认 |
| `🟡 代码已写，验证待补` | 代码存在但缺编译/运行验证（9/8–9/11 那批按此口径，见 §1.1） |
| `⏳ 未做` | 尚无实现 |

### 0.5 测试载体与构建/运行命令

**测试载体**：`06.GILab` sample（Sponza / Cornell Box，GI 对比实验室；三管线 + 四通道面板 + GPU Profiler）。

```powershell
# 关键：把 anaconda 加进 PATH，否则 Slang 之后的 SPV→头文件 步骤会以 MSB8066/9009 失败
$env:PATH = "C:\anaconda3;C:\anaconda3\Scripts;C:\anaconda3\Library\bin;$env:PATH"
cmake -S D:\Source\HugEngine -B D:\Source\HugEngine\Build      # 或 cmake --preset default
cmake --build D:\Source\HugEngine\Build --config Debug --target 06.GILab -j 8
& D:\Source\HugEngine\Build\bin\Debug\06.GILab.exe              # 运行目录即 Build\bin\Debug

# 崩溃处理器自检（Wave 0.8）：第 3 帧主动解引用空指针，验证能否打出完整调用栈
$env:HE_CRASH_TEST = "1"; & D:\Source\HugEngine\Build\bin\Debug\06.GILab.exe
```

**运行期产物**：崩溃日志 `Content/Config/06_GILab_crash.log`（目录已 gitignore）、
minidump `Build/bin/Debug/06.GILab_crash.dmp`（可用 Visual Studio 打开）、
面板状态 `Content/Config/06_GILab.cfg` 与 `06_GILab_imgui.ini`。

### 0.6 原里程碑（M0–M6）与本次波次（Wave 0–5）的映射

> M 系列按"技术能力"切，Wave 系列按"验收依赖顺序"切；未完成部分已全部并入 Wave。

| 原里程碑 | 状态 | 现归属 |
|---|---|---|
| **M0** 正确性 Bug 修复 | ✅（`5712721`/`55d1038`） | 附录（历史） |
| **M1** 接口收敛 + shader 通道化 | ✅（`59c3382`/`65d9cf5`） | 附录 |
| **M2** 数据驱动 + 帧图自动编排 | ✅（`2b06045`） | 附录 |
| **M3** Provider 注册表 + 自动降级 | ✅（`66faf5c`；后被 P3 层栈化重写为逐源裁剪） | 附录 |
| **M4** 性能优化 | 🔄 部分（4.1 halfRes / 4.2 SSR Hi-Z / 4.3 DDGI 1/4 HDR 已完成） | 余项 **M4.4 / M4.5 → Wave 4（§六）** |
| **M5** 质量提升 | ⏳ 部分（M5.2-B 完成；**M5.1 已划掉**，S1 覆盖） | **M5.3 / M5.2-A → Wave 4（§六）** |
| **M6** 工业界进阶 | ⏳ 按需 | **M6.3 GTAO → Wave 4**（兼作 Wave 2 零侵入验证载体）；**M6.1 ReSTIR GI → Wave 5（§七）** |
| **P1/P2/P3** 数据模型 / 归一化合成 / 源层栈 | ✅（`105911b`） | 附录 |
| **S1 / S1.5** 光追归入 Deferred / 两类源同时参与 | ✅（`90649ba`/`aac5690`/`5c2b84b`） | 附录 |
| **S2 / S3** 管线维度收敛 / 移除 `HybridRTPipeline` | ✅（`06c8580`/`0f8aca8`） | 附录 |
| **PT** 参考渲染器定位与加速结构共享 | ✅（`3301040`） | 附录 |
| **P4** Provider 抽象 | ⏳ 未做（无 `IGIProvider`） | **Wave 2（§四）** |
| **P5** 频率分离 | ⏳ 未做（无 `FrequencySplit`） | **Wave 3（§五）** |
| **P6** 统一估计器（ReSTIR GI） | ⏳ 未做 | **Wave 5（§七）** |
| —— 本次审计新增 —— | | |
| 验证基线（工具链 / 编译 / 运行） | ✅ 已打通 | **Wave 0.0（§二）** |
| 偶发崩溃 / 周期性校验违规 | ⏳ 定位完成、修复部分落地 | **Wave 0.1 / 0.7（§二）** |
| 白炉测试（能量守恒判据） | ⏳ 未做 | **Wave 0.2（§二）** |
| 崩溃处理器 | ✅ 已完成并自检通过 | **Wave 0.8（§二）** |

**一句话顺序**：
`白炉判据 → IBL/RSM 归位（能量守恒）→ DDGI SH 修正 → 层栈 UBO 按源数组化 → IGIProvider（AO 试点）→ 频率分离`；
其中 0.1/0.7/0.8 为前置门槛，Wave 4 小项穿插并行。

---

## 一、现状核对（文档声明 ∧ 代码实证）

### 1.1 文档声明的已落地项——代码中确认存在 🟡（编译/运行验证待补，见 §1.3）

| 项 | 证据 |
|---|---|
| P1 数据模型（`GIBand` / 权重 / `falloffDistance` / `blendMode`） | `GI/GIConfig.h:58-76`、`Pipeline/LightingPass.h:50-68` |
| P2 归一化合成（保留 Additive 作对照） | `DeferredLighting.frag.slang:347-353` |
| P3 层栈架构（`GISourceId` 12 源 + 4 个 `GIChannelStack`） | `GIConfig.h:37-55, 119-187, 251-295` |
| P3 帧图门控由层栈派生（`ShouldRun*`） | `GIConfig.h:265-282` |
| P3 逐源裁剪降级 | `GI/GIRegistry.h:32-66` |
| P3 混合参数走 UBO（binding 31） | `LightingPass.cpp:157-179, 258-270`、`ShaderTypes.slang:91-113` |
| P3 06 源列表 UI + 层栈序列化 | `06.GILab.cpp:915-1052`、`1167-1186` |
| S1 光追归入 Deferred（4 个 RT 源） | `DeferredPipeline_FrameGraph.cpp:452-640` |
| S2/S3 管线收敛、`HybridRTPipeline` 已删 | 06 下拉仅 Forward/Deferred（`06.GILab.cpp:856`）；无 `HybridRTPipeline` 文件 |
| S1.5 两类源同时参与 | shader `:322-337`（SSGI+RTGI 各自独立采样）、`:366-385`（SSR+RT 反射）、`:293-300`（SSAO+RTAO） |

### 1.2 文档声明的待做项——同样确认未做 ⏳

| 项 | 实证 |
|---|---|
| **白炉测试** | 全仓无 furnace/能量守恒测试；`Samples` 无截图/读回设施；`Tests/` 为 doctest 且**不链接 Render 模块**（`Tests/CMakeLists.txt:48-54`） |
| **P4 Provider 抽象** | 无 `IGIProvider`（glob 无结果）；`IGlobalIllumination` 与 SSGI/DDGI/SSR/RSM 仍是管线独立成员，帧图手写 ~40 个 `AddPass` |
| **P5 频率分离** | `GIBlendMode` 仅 `Additive` / `Normalized`（`LightingPass.h:50-53`），无 `FrequencySplit` |
| M4.4 RSM VPL 降采样 / M4.5 GBuffer 通道合并 | 未做 |
| M5.2-A DDGI 光追 march / M5.3 DDGI SH 修正 | 未做 |
| M6.3 GTAO | 未做 |
| ~~M5.1 RTGI 时域累积~~ | ✅ **实际已被 S1 覆盖，应从待办划掉**（详见 §六）：`RTDenoiser` 已有 history + 基于 velocity 的重投影 + 时域混合 + 去遮挡（`PostProcess/RTDenoiser.h:18,75`、`RT_DenoiseTemporal.frag.slang:44-68`），且 Deferred 帧图已把它挂在 RTGI 输出上（`DeferredPipeline.h:193` `m_GIDenoiser`）。rgen 内 SPP=1、无 history 是**正确设计**（累积归降噪器）。剩余价值仅为降噪参数调优 / SPP 提升 |

### 1.3 🚨 验证基线实证：`06.GILab` 从未编译、从未运行，且本机无任何工具链

> 这是本计划的**第一优先级输入**——它决定"验收"这一环当前根本无从执行。

**证据链（全部可复现）**：

| 事实 | 证据 |
|---|---|
| `06.GILab` 到 **9/7 23:56** 才加入工程 | `630db2b` 首次提交；`Samples/CMakeLists.txt:6` 已含 `add_subdirectory(06.GILab)` |
| 现有 `Build/` 树**早于**该 sample | 构建产物时间停在 `2026/9/7 01:21`（`Build/bin/Debug/02.Cube.exe`）；`Build/Samples/` 下**没有** `06.GILab` 工程目录 |
| 全盘无 `06.GILab` 产物 | 在 `D:\Source` 递归搜索 `06.GILab.exe` / `06.GILab.vcxproj` → **零结果** |
| 该 sample **从未运行过** | 它在退出时写 `Content/Config/06_GILab.cfg`（`06.GILab.cpp:52, 1196`）。`Content/Config/` 下只有 `02_Cube.cfg`（9/7 01:21）——**无任何** `06_GILab.cfg` / `06_GILab_imgui.ini` |
| 本机**没有 C++ 工具链** | 无 `C:\Program Files\Microsoft Visual Studio`（VS 完全未安装：无目录、无注册表 `SxS\VS7`、无 `vswhere`）；无 `C:\gcc16`（`gcc` preset 所指 MinGW）；无 LLVM/msys64；PATH 无 `cl`/`msbuild`/`ninja`/`make`。cache 记录的编译器 `.../Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231` **该路径不存在** |
| 本机**无 Vulkan SDK** | `VULKAN_SDK` 为空、无 `C:\VulkanSDK`；而 `CMakeLists.txt:43` 是 `find_package(Vulkan REQUIRED)`，且 shader 编译需要 SDK 内的 `slangc`（`cmake/VulkanSDK.cmake`） |

**本轮实测复现**：直接 `cmake -S . -B Build` 会失败——
`Generator Visual Studio 18 2026 could not find specified instance of Visual Studio: C:/Program Files/Microsoft Visual Studio/18/Community`（既有 cache 的 `CMAKE_GENERATOR_INSTANCE` 指向已不存在的路径）。

**因此必须正视的推论**：9/8–9/11 这批共 12 个 GI 提交（P1/P2/P3、S1/S2/S3、S1.5）**在本机不可能经过"编译 + 06.GILab 冒烟"**，
开发计划里"每完成一个里程碑必须跑 exe 冒烟 + 用户实测确认才合入"的约定在这批工作中**未能执行**。
这批工作的准确状态应记为：**代码已写，编译与运行验证待补**——而不是"已完成、已验证"。

**重建验证基线需要（= Wave 0.0）**：
1. **C++ 工具链**（二选一）：
   - **MSVC 首选**：Visual Studio 2026 (v18) Community 或 Build Tools，勾选"使用 C++ 的桌面开发"（MSVC 14.5x + Windows SDK）——工程 preset 与既有 cache 都走这条路径；
   - 或 **MinGW GCC 16** 安装到 `C:\gcc16\mingw64`（`CMakePresets.json` 的 `gcc` preset 所指路径）。
2. **Vulkan SDK**（1.4.x 即可）：提供 `vulkan-1.lib` / headers / `slangc`；`cmake/VulkanSDK.cmake` 会自动探测并写入 `VULKAN_SDK`。
3. 第三方依赖**已全部内置于 `Engine/External`**（glfw / glm / imgui / Jolt / meshoptimizer / spdlog / stb / taskflow / VMA / doctest / nlohmann），无需额外安装。
4. 装好后的命令：
   ```powershell
   cmake -S D:\Source\HugEngine -B D:\Source\HugEngine\Build      # 或 cmake --preset default
   cmake --build D:\Source\HugEngine\Build --config Debug --target 06.GILab -j 8
   ```

#### 1.3.1 第二轮复验结果（环境已通 · 构建通过 · 运行偶发崩溃）

**① 环境已补齐并实测通过**

| 项 | 状态 |
|---|---|
| MSVC | ✅ VS 2026 (v18) Community，工具集 **14.51.36231**（正是 cache 期待的那个版本）+ Windows SDK 10.0.26100.0 |
| Vulkan SDK | ✅ 1.4.357.0，`slangc` 可用，configure 自动探测成功 |
| JoltPhysics 子模块 | ❌→✅ **原本未初始化（目录为空），导致 configure 直接失败**；已用可达镜像 `ghfast.top` 克隆，并**校验 `HEAD == 仓库钉住的 a45f543`** |
| Python | ⚠️ `C:\anaconda3`（3.14.6）。构建规则 `Engine/Shader/CMakeLists.txt:271` 调用的是**裸 `python`**（跑 `spv_to_header.py`，仅用标准库）→ **必须把 `C:\anaconda3` 加进 PATH**，否则 Slang 之后的 SPV→头文件步骤以 `MSB8066 / 9009` 失败，且报错文案会误导成"去 Microsoft Store 装 Python" |
| 网络 | ⚠️ `github.com:443` **不可达**（连接被重置，DNS 正常）；`ghfast.top` / `ghproxy.net` / `gh-proxy.com` / `gitee.com` 可达 |

**② 构建已通过（对首轮预判的修正）**

```
cmake -S D:\Source\HugEngine -B D:\Source\HugEngine\Build   → 成功
cmake --build ... --config Debug --target 06.GILab -j 8     → BUILD EXIT: 0
产出：Build\bin\Debug\06.GILab.exe      错误行：0
```

> **修正首轮风险判断**：首轮风险表写"9/8–9/11 共 12 个提交从未编译、可能成批报错"——**实测编译干净通过**，
> 包括 S3 删除 `HybridRTPipeline` + 02.Cube 迁移这个高风险改动。这批代码的**编译风险撤销**，只剩**运行/正确性风险**。

**③ 运行：能跑，但存在偶发崩溃（新增待办）**

- 5 次启动中 **4 次稳定**：连续存活 25–60 s，推进到 **Frame 431–530**（Debug 构建 + 全量日志），日志中**零** `error/exception/assert`。
- **1 次崩溃**（首次启动，约 5 秒、Frame 1 附近）：WER `APPCRASH`、异常码 `0xc0000005`（访问违例）、偏移 `0x38216b`，故障模块是 `06.GILab.exe` 自身（非驱动、非 `vulkan-1.dll`）。
- 崩溃前 stdout 最后一条是 Vulkan 校验告警：
  `vkCmdPipelineBarrier(): ... cannot transition ... from DEPTH_STENCIL_ATTACHMENT_OPTIMAL when the previous known layout is DEPTH_STENCIL_READ_ONLY_OPTIMAL`（`VUID-VkImageMemoryBarrier-oldLayout-01197`）
  → 指向 RenderGraph 对**深度图 barrier 的 `oldLayout` 推导**问题；该告警本身不致命，但可能连带到后续 pass 的非法状态。
- **为什么必须先修**：白炉测试需要长时间稳定运行 + 读回数值，偶发崩溃会**污染判据**（分不清是 GI 数值错，还是进程崩了）。

> **口径修正（诚实起见）**：首轮把"没有 `06_GILab.cfg`"当作"从未运行"的主要证据。实测发现**强制结束进程同样不写 cfg**
> （该文件只在正常退出路径写），所以它只是**佐证**；决定性证据仍是：**全盘无 `06.GILab.exe`/`.vcxproj`** + **既有 `Build/` 树早于该 sample**。

**④ 本机可用的构建/运行命令（可直接复用）**

```powershell
# 关键：把 anaconda 加进 PATH，否则 SPV→头文件 步骤失败
$env:PATH = "C:\anaconda3;C:\anaconda3\Scripts;C:\anaconda3\Library\bin;$env:PATH"
cmake -S D:\Source\HugEngine -B D:\Source\HugEngine\Build
cmake --build D:\Source\HugEngine\Build --config Debug --target 06.GILab -j 8
& D:\Source\HugEngine\Build\bin\Debug\06.GILab.exe      # 运行目录即 Build\bin\Debug
```

#### 1.3.2 崩溃定位与已实施修复（Wave 0.1 进展）

**① 崩溃点已符号化到具体函数（硬证据）**

用 `/MAP` 重新链接产出 `06.GILab.map`（155082 个符号），对 WER 偏移 `0x38216b` 做**全量线性扫描**（注意：map 文件内符号列表并非全局有序，二分查找会给出错误答案）：

```
0x382160  he::rhi::VulkanTexture::GetImageView()     ← 崩溃指令在函数体 +0xB
```

`GetImageView()` 的函数体就是 `return m_ImageView;`，+0xB 处正是"通过 `this` 读取成员"的指令
→ **结论：某处用一个无效（非空）的 `IRHITexture*` 调用了 getter**。
这解释了偶发性：野指针有时仍指向可读内存（静默读到脏 `VkImageView`），偶尔该内存已被解映射 → 访问违例。
全仓 `GetImageView()` 调用点只有 3 处，全在 `RHI/Vulkan/VulkanDevice_Descriptors.cpp`（描述符集更新）。

**② 已实施的两项修复**

| 修复 | 内容 | 状态 |
|---|---|---|
| **纹理存活登记 + 描述符更新守卫** | 新增 `RHI/Vulkan/VulkanTextureLiveness.{h,cpp}`：`VulkanTexture` 构造/析构时登记/注销存活；3 处 `GetImageView()` 调用点在**解引用之前**校验，命中野指针则打印含 binding 与指针值的错误并安全跳过（把"无法定位的访问违例"变成"可定位的错误日志"） | ✅ 已实现、编译通过；15 次冒烟中**未命中**（假说未被证实，如实记录） |
| **启动期尺寸 churn 消除** | `IRenderPipeline::Initialize` 增加 `width/height`（默认 0 = 沿用默认，其他调用点行为不变）；`Deferred/Forward/PathTracing` 三管线按初始尺寸建资源；`06.GILab` 传入 `swapchain` 真实尺寸（1920x**1061**，而请求的窗口是 1920x1080）。原来 Initialize 按 1920x1080 建全套、紧接着 `OnResize(1920,1061)` 把**整套资源销毁重建** | ✅ 已验证：`1920x1080` 纹理创建 **26 → 0**，资源只在正确尺寸建一次 |

**③ 但启动 churn 并非那些校验违规的成因（实测否证）**

修复前后同样的 8 秒运行，校验告警数量**完全一致**：

| 违规类型 | 修复前 | 修复后 |
|---|---|---|
| `command buffer ... which is now in an invalid state`（framebuffer 被销毁导致） | 110 | **110** |
| `vkCmdPipelineBarrier ... cannot transition the layout`（`oldLayout` 不符） | 10 | **10** |
| `vkCmdBeginRenderPass ... You cannot start a render pass`（附件布局不符） | 10 | **10** |
| `vkAcquireNextImageKHR ... Semaphore must not have any pending operations` | 10 | **10** |

三次运行数字完全相同 → 与启动 churn 无关，需单独定位（见 Wave 0.7 与 §1.3.4）。
⚠️ **本节此处的性质判断已被两次更正**：先记为"周期性（约每 N 帧一轮）"，后改为"启动阶段一次性"，
**两次都不对**——根因是校验层的**重复消息去重（上限 10）**掩盖了真实次数；关闭去重后实测为
**每帧复发**（barrier 438 / renderPass 146 / semaphore 145，12 秒）。完整证据链与教训见 §1.3.4。
它仍属于"命令缓冲引用的对象在提交完成前被销毁"这一类，是需要修的 GPU 同步正确性问题。

#### 1.3.3 Wave 0.1 当前状态（诚实口径）

- 合并统计：**约 100 次启动 / 1 次真实崩溃**（唯一一次发生在"新链接二进制的首次启动"，约 5 秒处，且**早于两项修复**）。
- 崩溃点已定位到函数级；两项修复已落地（尺寸 churn 已验证消除；"纹理存活守卫"为防御 + 诊断，冒烟中未被命中）。
- **根因尚未被证明**——因此 Wave 0.1 **不算完成**。修复后累计 **58 次连续启动零崩溃**（18 + 40 次 soak），
  但在 1/55 量级的概率下，"零崩溃"仍可能只是运气，不足以断言已修。
- **崩溃处理器（Wave 0.8）已完成并自检通过**：下次偶发崩溃会直接产出"函数名 + 源文件:行号"的调用栈与 minidump
  （见 §二 的 0.8 验收项），届时可一步定位根因，无需再靠偏移反查。
- 另一条更有把握的路径：**修掉 §1.3.2③ 的周期性校验违规**（确定性、可验证，见 §二 Wave 0.7）。

#### 1.3.4 Wave 0.7 实测进展（每帧违规 · 修正两次性质误判 · invalidState 已修复）

> **两次判断更正（测量方法本身出过问题，记录下来）**：
> 1. 曾记为"周期性（约每 N 帧一轮）"；
> 2. 随后改为"启动阶段一次性"——理由是 8 秒 / 25 秒运行的计数完全相同（110/10/10/10）且时间戳集中在
>    约 20 毫秒的启动窗口内。**这一条同样是错的**。
>
> 真相：**校验层启用了重复消息去重**（日志中可见 "This VUID has now been reported 10 times,
> which is the duplicate_message_limit value"）。因此 `10` 是**去重上限**而非真实次数；
> `110 = 11 个不同 VUID × 上限 10`（不同命令类型各有一个 VUID）。在运行目录放 `vk_layer_settings.txt`
> 并设置 `khronos_validation.duplicate_message_limit = 0`（配 `VK_LAYER_SETTINGS_PATH`）后，
> 12 秒运行（最大帧号 146）得到**真实计数**：`invalidState = 0`（已修）、`barrier = 438`（≈3/帧）、
> `renderPass = 146`（1/帧）、`semaphore = 145`（≈1/帧）⇒ 后三类是**每帧复发**的。
>
> **方法论教训**：校验层的"报告数"不能直接当指标——要么关闭去重，要么用独立仪器
> （如 `HE_TRACE_FB` 的 `delay=0` 计数）交叉验证。

| 违规（含 VUID） | 真实次数（12 秒 / 146 帧，去重已关） | 已定位的成因 | 状态 |
|---|---|---|---|
| `command buffer ... was in an invalid state ... VkFramebuffer ... was destroyed` | **0**（修复前每帧复发） | **`VulkanCommandList::~VulkanCommandList()` 里的 `GetDeferredDestroy().FlushAll()`**：`RenderGraph::ExecuteWithAsyncCompute` 每帧创建一个**临时** Compute 命令列表，其析构发生在**主命令缓冲仍在录制**时，却清空了**设备级**延迟销毁队列——把本帧离屏 pass 刚创建、已绑定且待延迟销毁的 framebuffer **立即销毁**（`HE_TRACE_FB` 实测 `delay=0`） | ✅ **已修并验证**：移除析构里的 `FlushAll()`（队列由 `VulkanDevice::Shutdown()` 在 `vkDeviceWaitIdle` 后统一清理）→ `delay=0` 计数 **0**、该 VUID 真实计数 **0** |
| `VUID-VkImageMemoryBarrier-oldLayout-01197`：barrier 声明 `oldLayout=DEPTH_STENCIL_ATTACHMENT` 而实际为 `DEPTH_STENCIL_READ_ONLY` | **438**（≈3/帧） | RenderGraph 布局模型与 render pass 实际 `finalLayout` 不一致；显式调用点声明的 `srcState` 与实际不符；跨帧持久资源（导入纹理）起始状态被当作 `Undefined` | 🟡 部分已修（`fixLayout` + 深度写后布局修正）；**待做：RHI 侧布局追踪** |
| `VUID-vkCmdBeginRenderPass-initialLayout-00900`：深度附件声明 `initialLayout=DEPTH_STENCIL_ATTACHMENT` 而实际为 `DEPTH_STENCIL_READ_ONLY` | **146**（1/帧） | 同源：深度图被采样后停在 READ_ONLY，下次作为附件开始 pass 前缺少 `READ_ONLY → ATTACHMENT` 的纠正 barrier | 🟡 同上（措辞已从 `SHADER_READ_ONLY` 改善为 `DEPTH_STENCIL_READ_ONLY`） |
| `VUID-vkAcquireNextImageKHR-semaphore-01779`：acquire 信号量有未完成操作 | **145**（≈1/帧） | 交换链 acquire 信号量的持有/复用策略（未单独定位） | ⏳ |

**本轮已实施的修改**：

1. ✅ **移除 `VulkanCommandList::~VulkanCommandList()` 中的 `FlushAll()`**（本轮关键修复）：队列由 `VulkanDevice`
   持有，`VulkanDevice::Shutdown()` 已在 `vkDeviceWaitIdle` 之后统一清理；命令列表析构代它清理是错的，
   因为临时命令列表会在**主命令缓冲录制期间**析构。
2. ✅ `VulkanCommandList::PipelineBarrier` 的 `fixLayout` 补上 **`SHADER_READ_ONLY` → `DEPTH_STENCIL_READ_ONLY`**
   重映射（此前只拦 `COLOR_ATTACHMENT`）→ 深度图不再被套**颜色布局**，render pass 告警措辞随之改善。
3. ✅ `RenderGraph::DeriveBarriers`：写深度资源之后把追踪布局修正为 `DepthStencilRead`（与 render pass 实际
   `finalLayout` 一致），使后续 barrier 不再凭空声称 `ATTACHMENT`。
4. 🟡 `DeferredDestructionQueue`：槽位 3 → **2 × kMaxFramesInFlight**。对计数无影响（不是本因），
   但"延迟销毁经过的帧数必须大于飞行帧数并留余量"本身是对的，保留为安全边界。

**新增诊断设施（保留，关闭时零开销）**：

- **`HE_TRACE_FB=1`**：追踪 framebuffer 创建/销毁，输出句柄与 `enqueueFrame / execFrame / delay`。
  `delay=0` 即"同帧销毁"，是本类问题的直接指标（本次即由它定位根因）。实现见
  `VulkanCommandList_RenderPass.cpp` 顶部；`VulkanCommandList` 析构与交换链重建路径同样打了标记。
- **`vk_layer_settings.txt` + `VK_LAYER_SETTINGS_PATH`**：关闭校验层重复消息上限（`duplicate_message_limit = 0`），
  得到违规**真实次数**——这是本轮修正测量方法的关键。

**已被实测否证的假说（记录以免重走弯路）**：

| 假说 | 实测证据 | 结论 |
|---|---|---|
| 队列槽位 3 对 3 帧在飞没有余量，导致同帧销毁 | 槽位 3 → 6 后计数不变 | ❌ |
| `AdvanceFrame()` 挂在 `CommandList::Begin()`，一帧内多命令列表推进多次 | 仪器化：推进 141 次 / 最大帧号 140 ⇒ 严格一帧一次 | ❌ |
| 四类违规是"启动阶段一次性" | 关闭去重后真实计数 **438 / 146 / 145**（每帧复发） | ❌（去重假象） |

**第二轮尝试与实测（同一"先量基线 → 改 → 用真实计数验证"流程）**：

| 尝试 | 思路 | 实测结果 | 结论 |
|---|---|---|---|
| 让深度 pass 结束时保持 `ATTACHMENT`（改 `VulkanPipeline.cpp` 两处 `finalLayout`），并把 `CSM`/`PointShadow` 的采样 barrier `srcState` 改成 `DepthStencilWrite` | 让现实与模型（"写深度=ATTACHMENT"）一致 | barrier 3.0→2.0/帧，但 **renderPass 1.0→2.0/帧（变差）**，且**新增** `VUID-vkCmdDraw-None-09600` 1/帧（采样方期望 READ_ONLY） | ❌ **方向相反，已回退**。正确方向是"现实保持 READ_ONLY，模型学会它" |
| 把 `RectLight`/`SpotShadow` 采样 barrier 的 `srcState` 由 `DepthStencilWrite` 改为 `DepthStencilRead` | 与 `CSM`/`PointShadow` 保持一致 | 计数**无变化** | 🟡 语义上更自洽，保留；但不是本因 |
| 把 GBuffer depth 采样 barrier（`DeferredPipeline_FrameGraph.cpp:700`）的 `srcState` 由 `DepthStencilWrite` 改为 `DepthStencilRead` | GBuffer 的 RP 结束时深度就是 READ_ONLY，声明 Write 与现实不符 | **barrier 3.0 → 2.0/帧**（每帧少 1 次） | ✅ **有效** |
| 纹理创建日志增加 `image=<handle>` | 与校验层报的 `VkImage 0x…` 对账 | 定位出问题 image 只有**两个**：均为 **1920x1061、aspect=2** 的全屏深度图（GBuffer depth 与 Lighting HDR depth） | ✅ 保留（长期可用的定位手段） |

**当前残留（每帧 1 次 × 2 个深度图）的机制推断**：RenderGraph 对**导入纹理**在每帧开始时假设布局为
`Undefined`，于是"首次使用不发射 barrier"——但这两个深度图在**上一帧结束时实际停在 `READ_ONLY`**。
当本帧首次使用是"写深度"时：模型记为 `ATTACHMENT` 且不发 barrier ⇒ 既触发
`render pass initialLayout-00900`（1/帧），也触发紧随其后的 `barrier oldLayout-01197`（1/帧）。
⇒ 正解即计划中的 **"跨帧真实布局"** 一项：需要 RHI 记录每个 `VkImage` 的当前布局（barrier 与
render pass 的 initial/final 都更新），并让图的导入资源初始化查询它，而不是假设 `Undefined`。

**第三轮：跨帧真实布局追踪（已实现，但实测尚未生效——原因已查明）**

- 新增 `RHI/TextureLayoutTracker.{h,cpp}`：按**纹理视图句柄**记录每个图的当前布局；
  barrier 路径写入、纹理销毁时清理；`RenderGraph::DeriveBarriers` 在"本帧首次使用"时查询它来替代 `Undefined` 假设。
- **实测无变化**（bar 仍 2/帧、rp 仍 1/帧）⇒ 说明该路径没被命中。
- **原因已查明（重要）**：`gbDepth` **不是**通过 `rg.ImportTexture()` 注册的，而是 `gbDepth = gb.depth`
  （来自 GBuffer 上下文里预先注册的句柄），因此 seeding 的判断 `m_ImportedTextures.count(h)` 对它**不成立**；
  `HDR_C` 才是 `ImportTexture` 注册的。⇒ 下一步应把 seeding 条件从"是否在 imported 表里"改为
  **直接按句柄查询追踪器**（对图的持久资源一律查询），并为 GBuffer 上下文这类预注册资源建立同样的登记。
- **另一条强线索**：`DeferredPipeline_FrameGraph.cpp:183-184` 为了让 Shadow 先于 GB_Clear 执行，
  给 `gbDepth` 声明了**假的 WAW 写**（`shadowWrites.push_back(RG_WRITE(gbDepth))`）——该 pass 并不真的写深度。
  这会让布局模型在一个"未被真实写入"的图上推进状态，是残留 barrier 报错的重要嫌疑。
  修法方向：把假依赖改为真实的执行顺序约束（图内显式边），而不是借用一个写依赖。

**第四轮：pass 级定位 + 以真实布局纠正 `oldLayout`（已验证再降 1 次/帧）**

- 新增诊断 `HE_TRACE_PASSES=1`：打印每个 pass 的开始，把 pass 名与校验层报错在时间上对齐。
  **定位结果**：barrier 违规发生在 **Lighting**（2/帧）与 **TAA_Resolve**；render pass 违规发生在 **Skybox**（1/帧）。
- 据此在 RHI 的纹理 barrier 里**用追踪到的真实布局纠正 `oldLayout`**（未记录过才回退到调用方声明）：
  **barrier 违规 2/帧 → 1/帧**（151/151，可复现）。
- 剩余两类计数**完全相等**（各 1/帧）⇒ 同源，机制已明确：**Skybox pass 以 `Load` 方式开始深度 pass，
  而深度此时实际停在 `READ_ONLY`**（Lighting pass 的 `finalLayout`）；由于追踪器**尚未记录 render pass 自身
  的布局转变**，纠正用的 `oldLayout` 仍是旧值，于是既报 `initialLayout-00900` 又报 `oldLayout-01197`。
- ⇒ 收尾只差一步：**在 render pass 边界更新追踪器**（begin 时深度记为 ATTACHMENT、end 时记为 READ_ONLY，
  与引擎 render pass 的 initial/final 声明一致），并在 begin 前按需补一次 `→ ATTACHMENT` 的纠正 barrier
  （需要 视图→VkImage 的登记，因为 barrier 只能作用于 image 而非 view）。

**下一步（Wave 0.7 收尾）**：

- **RHI 布局追踪**（治 438 + 146）：给 `VulkanTexture` 记录**当前布局**（每次 barrier、每次 render pass 的
  initial/final 都更新）；`PipelineBarrier` 用**真实布局**作为 `oldLayout`；在 `BeginRenderPass` /
  `BeginOffscreenPass` 之前按需自动补一次 `→ 附件 initialLayout` 的纠正 barrier。
  这同时覆盖"同一帧内"与"跨帧持久资源"两种场景。
- **交换链 acquire 信号量**（治 145）：按 swapchain image 索引持有信号量，避免复用未完成者。
- **判据**：关闭去重后**真实计数全部为 0**（不再采用会被去重掩盖的"报告数 ≤ 10"）。

---

### 1.4 ⚠️ 文档未记录的缺口（本次审计新发现，是后续计划的真正起点）

> 这三条都直接落在设计文档自己写的「关键不变量」上，**不修则白炉测试必然失败、P4/P5 也无从验证**。

**(A) IBL 漫反射环境项在归一化之外 → 双重计数 + 层栈 IBL 权重是死参数**
- `DeferredLighting.frag.slang:254`：`color += kD * IrradianceMap.Sample(N) * albedo * iblIntensity;` —— **无条件加法**，在 `:346-354` 的归一化合成之外。
- 同时 diffuse 层栈的 `IBL` 权重**无人读取**：`DeferredPipeline_FrameGraph.cpp:771` 只把 `GISourceId::DDGI` 传给 `probeId`，`:340-344` 的 probe 分支只调 `SampleDDGI()`，而 `RT_DDGI.slang:37` 的 `SampleDDGI` 是**纯探针插值、无 IBL 回退**。
- 后果：① 层栈里给 IBL 权重完全不生效（06 面板上是"假开关"）；② DDGI/SSGI 与 IBL 环境项叠加 → 能量 > 1，**设计文档"不变量 3（多开一个源不会变亮）"在环境源上不成立**；③ 白炉测试必失败。
- 补充：M5.2-B 之后 DDGI 探针辐射度本身来自 RSM/IBL 回退 → 「DDGI 与 IBL 环境项」天然是同一份能量的两条路径，**必须二选一或归一化，不能叠加**。

**(B) RSM 间接光仍是归一化之外的加法 + 魔法系数 0.03**
- `DeferredLighting.frag.slang:266-283`：`color += rsmIndirect * (0.03 * giIntensity) * iblIntensity;`，且附带 4 个前置条件（`iblIntensity>0`、`lightCount>0`、方向光、阴影强度>0）。
- 但 06 面板把 `RSM` 列为 **diffuse 层栈的候选源**（`06.GILab.cpp:961-964`），`GIConfig::ShouldRunRSM()` 也从层栈派生 → **层栈说"RSM 参与合成"，shader 走的却是另一条加法路径**，违反设计文档「不变量 1：层栈与子系统同源」。
- M1 声称"魔法系数全移除"，实际 `0.03` 仍在。

**(C) 层栈实际只有 3 个"抽象槽位"，`kMaxSources = 4` 是表象**
- `GIChannelBlendParams`（`ShaderTypes.slang:99-107`）只有 `screenSpaceWeight / rayTracingWeight / probeWeight` 三个槽，且映射**硬编码**：diffuse = {SSGI, RTGI, DDGI}、specular = {SSR, RTReflection, IBL}、ao = {SSAO, RTAO}（`DeferredPipeline_FrameGraph.cpp:762-776`）。
- 后果：`GISourceId::RSM` 在 diffuse 层栈里**根本进不了 shader**；`Lightmap` 预留在 UBO 里无处安放；任何第 4 个源都只能加"硬编码分支"。P4/P5 若在此结构上做，会立刻撞墙。
- 附：`DeferredLighting.frag.slang:342` 的 `* ddgiScale`（来自 `DDGI.debugScale`）在归一化**之内**，是一个可破坏能量守恒的非物理缩放。

**TODO（文档同步）**：设计文档 3.1 的数据模型已与实现漂移——`GILayerDesc`→`GISourceDesc`、`GIBand` 不进层描述、`GIBlendMode::Fallback` 未实现、`blend` 为通道级 `mode`。建议按实现回写文档，避免后续照文档实现出错。

---

## 二、Wave 0 · 建立验证基线 + 正确性基线（最高优先）

> 理由：设计文档与开发计划都把白炉测试列为下一步第①项。它既是**验收判据**，也是 §1.4 三个缺口的探针——
> 先立判据，再修合成，才能证明"修对了"而不是"看起来不亮了"。
> 环境门槛（0.0）已实测通过（§1.3.1）：**能编译、能跑**；但运行存在**偶发崩溃**，故 0.1 先把它按死，再进白炉。

| 任务 | 位置 | 做法 |
|---|---|---|
| **0.0 重建验证基线（工具链）** | 环境 | ✅ **已完成（§1.3.1）**：VS 2026 v18 已装、Vulkan SDK 1.4.357.0 已装、JoltPhysics 子模块已补齐、Python 走 `C:\anaconda3`（构建前需入 PATH）；configure + 编译 `06.GILab` **BUILD EXIT: 0**、0 错误 |
| **0.1 偶发崩溃定位与修复** | `06.GILab` 首帧 / RenderGraph 深度 barrier | 5 次启动 1 次崩（`0xc0000005 @0x38216b`，约 5 s、Frame 1，§1.3.1③）。做法：① 用 `/Zi` 的 `06.GILab.pdb` 把偏移 `0x38216b` 解析到函数（`dumpbin`/调试器）；② 复现手段——**反复冷启动**（首启更易触发，疑似初始化/首帧竞态）；③ 同时修 RenderGraph 深度图 `oldLayout` 推导（校验告警 `VUID-…-oldLayout-01197`）；④ 判据：连续 10 次启动零崩溃 |
| **0.2 白炉测试设施** | `06.GILab`（新） | ① 场景"白炉模式"：封闭白炉 + 全白环境 + albedo=1 + 关闭直接光（或能量归一），正确结果 = **白球消失**；② 数值判据：读回中心/背景像素亮度，比值 ∈ [0.98, 1.02]；③ 需**新增读回设施**（当前无截图/readback 代码）：离屏 RT → buffer → 面板显示数值（先做面板数值断言，自动化截图对比可后置） |
| **0.3 纯数学单测（可先落）** | `Tests/` | 层栈权重归一化的 CPU 单测（`Σw` 归一、`WeightOf/FalloffOf/Set/Remove` 边界、`Degrade` 逐源裁剪 + 兜底）。现行障碍已定位：`GIConfig.h:18` → `LightingPass.h:3` → `RHI/RHI.h`，即层栈数学**挂在 RHI 依赖链上**；而 `Tests/CMakeLists.txt:48-54` 只链接 AI/Scene/Asset/Core/Physics。解法：把 `GISourceId`/`GIBand`/`GIBlendMode`/`GISourceDesc`/`GIChannelStack`/`PipelineGICap`/`ToPipelineCap`/`IsRayTracingSource`/`PipelineCaps` 抽到新的 **RHI-free `GI/GITypes.h`**（只 `#include "Core/Types.h"`，实测该约定是轻量头），`GIConfig.h` 再 include 它；改动的引用面很小（全仓仅 4 个 .cpp 引用 `GIConfig.h`/`GIRegistry.h`） |
| **0.4 IBL 归位** | `DeferredLighting.frag.slang:250-259` + `DeferredPipeline_FrameGraph.cpp:762-776` | 把 IBL 漫反射环境项**移入 diffuse 层栈归一化**（作为低频 probe/env 源）；层栈 `IBL` 权重真正生效；明确"DDGI 内部已含 IBL 回退"时二者的互斥或权重语义 |
| **0.5 RSM 归位** | `DeferredLighting.frag.slang:261-284` | RSM 间接作为 diffuse 层栈的一个源参与归一化；去掉 `0.03` 魔法系数（或显式定义为 VPL 能量归一常数、纳入层栈权重）；面板 RSM 开关与 shader 路径同源 |
| **0.6 `ddgiScale` 语义收敛** | `:342` + `GI_DDGI.h` | `debugScale` 只影响调试可视化路径，**合成路径恒 1.0**（或折算进层栈权重），消除能量守恒破坏项 |
| **0.7 每帧校验违规** | `RenderingPass`/RHI 资源生命周期 + RenderGraph barrier + 交换链信号量 | 真实计数（关闭校验层去重后，12 秒 / 146 帧）：`invalidState` **0 ✅ 已修**（根因：`VulkanCommandList` 析构擅自清空设备级延迟销毁队列 → 同帧销毁主命令缓冲已绑定的 framebuffer，见 §1.3.4）；`barrier oldLayout` **438**、`render pass initialLayout` **146**、`swapchain semaphore pending` **145** —— 后三类每帧复发，**待做**：① RHI 侧布局追踪（`VulkanTexture` 记当前布局 + `Begin*Pass` 前补纠正 barrier）；② 交换链 acquire 信号量按 image 索引持有 |
| **0.8 崩溃处理器（诊断基建）** | `06.GILab/CrashHandler.{h,cpp}` | ✅ **已完成**：`SetUnhandledExceptionFilter` + `StackWalk64`/DbgHelp，崩溃时打印**函数名 + 源文件:行号**的完整调用栈，并写出 minidump；记录后返回 `EXCEPTION_CONTINUE_SEARCH`，**故意让 WER 继续记录 APPCRASH**，两种证据都不丢。自检方式：`HE_CRASH_TEST=1` 启动会在第 3 帧主动解引用空指针 |

**验收**：
- [x] **（0.0 门槛）** HEAD 能 configure + 编译通过，产出 `06.GILab.exe`（§1.3.1②）。
- [x] 启动期尺寸 churn 消除（`1920x1080` 纹理创建 26 → 0；§1.3.2②）。
- [ ] **（0.1 门槛）** 连续 10 次冷启动零崩溃；白炉测试期间可长时间稳定运行。→ 崩溃点已定位到函数（§1.3.2①），
      防御守卫已加，但**根因未证明**，故此项**未通过**（当前连续干净 18 次，不足以在 1/40 概率下断言已修）。
- [ ] **（0.7 门槛）** 上述 4 类校验违规在"完整启动 + 稳定运行"窗口内**归零**
      （110 / 10 / 10 / 10 → 0 / 0 / 0 / 0）——确定性、可量化的硬判据，比偶发崩溃更适合作门槛。
      当前进展与已修/待修项见 §1.3.4。
- [x] **（0.8）** 崩溃处理器已安装并自检通过——自检输出示例（`HE_CRASH_TEST=1` 主动崩溃）：
      `#0 06.GILab!main + 0x41C2 [06.GILab.cpp:724]` + 完整调用栈 + 9.7 MB minidump；
      同时 WER 仍记录 `APPCRASH 0xc0000005`（两种证据并存）。产物位置：崩溃日志
      `Content/Config/06_GILab_crash.log`（该目录已在 .gitignore），dump 在 exe 同目录。
- [ ] 白炉模式下球体消失，中心/背景亮度比 ∈ [0.98, 1.02]。
- [ ] **Low/Medium/High/Ultra 四档 + 单源/多源组合（IBL / DDGI / SSGI / RTGI 任意权重）比值不变** —— 即归一化不变量成立。
- [ ] 单源配置画面与改造前**逐像素级一致**（回归截图对比）。
- [ ] `06.GILab` 各档位冒烟 + 用户实测确认后才进入 Wave 1。

**风险**：中高（比首轮评估上调）。除"改完画面变了"之外，新增两条：
① **0.0 的工具链安装与首次全量编译本身可能暴露 9/8–9/11 这批未验证提交里的编译错误**——那属于"先还债"，会挤占 Wave 0 的排期；
② 白炉读回设施是本波新增工程量的大头。

---

## 三、Wave 1 · 层栈与合成路径对齐（P4 的前置）

> 理由：§1.4(C) 的"3 槽位"限制必须先拆掉，否则 P4 的 Provider 抽象无处落地、P5 的按频段分离也无从表达。

| 任务 | 位置 | 做法 |
|---|---|---|
| **1.1 UBO 按源数组化** | `ShaderTypes.slang:99-113` + `LightingPass.h:56-68` + `LightingPass.cpp:157-179` | `GIChannelBlendParams` 改为「**源描述数组**」（如 `4 × {id, weight, falloff}` + count + mode），与 `GIChannelStack::kMaxSources=4` 一一对应；C++/slang 双侧同步 + `static_assert(sizeof) ` 防漂移（这是文档强调的 F1 痛点） |
| **1.2 shader 按源 id 分派** | `DeferredLighting.frag.slang:312-353` | 合成改为 `for (层) { c = SampleSource(id, worldPos, N); w = ComputeWeight(...); }`（对齐设计文档 3.3），删掉三处硬编码分支；`SampleSource` 内按 `GISourceId` 分派 |
| **1.3 去掉 `useScreenGI` 耦合** | `LightingPass.h:129`、`LightingPass.cpp:161-171`、`FrameGraph:777` | 权重直接由层栈表达（`WeightOf` 已足够），删除 `screenSourceValid` 这一个布尔同时清零 screen/RT 双权重的隐式耦合 |
| **1.4 06 面板与 UBO 同源** | `06.GILab.cpp:915-1052` | 层栈 UI 直接驱动数组（含 RSM/Lightmap 占位），消除"面板能选但 shader 收不到"的假开关 |

**验收**：
- [ ] RSM / Lightmap（可先用 IBL 替代验证）等**第 4 个源**能真正参与合成并在画面生效。
- [ ] 白炉测试（Wave 0 的判据）在改造后仍通过。
- [ ] 任一层栈组合的画面与 Wave 0 后逐像素一致（纯结构改造，行为等价）。

---

## 四、Wave 2 · P4 · `IGIProvider` 抽象（新增 GI 不改管线/shader）

> 理由：设计文档收益承诺是"新增 GI 只需实现接口 + 注册"。当前 SSGI/DDGI/SSR/RSM/4 个 RT 效果都是管线成员，帧图 1106 行里手写 ~40 个 `AddPass` + 手写门控。

| 任务 | 位置 | 做法 |
|---|---|---|
| **2.1 `IGIProvider` 接口** | `GI/IGIProvider.h`（新） | 按设计文档 3.4：`GetSourceId/GetBand/GetRange/IsValid/GetDiffuse|Specular|AOOutput/Initialize/Render` |
| **2.2 现有源适配** | `GI_SSGI/GI_SSR/GI_DDGI/GI_RSM/GI_IBL` + 4 个 `RT*Pass` | 包装为 Provider（尽量不改现有类，用适配器避免大爆改）；`IsValid()` 表达"SSGI 全屏外 / RTGI 未收敛"等置信度语义 |
| **2.3 帧图按注册表构建** | `DeferredPipeline_FrameGraph.cpp` | pass 注册/门控从手写 `ShouldRunXXX` 改为**遍历已注册 Provider**（`provider->IsValid() && stack.WeightOf(id)>0`）；降噪/空间滤波等附属 pass 由 Provider 自报"需要哪些后续 pass" |
| **2.4 `GIRegistry` 扩容** | `GI/GIRegistry.h` | `Register/Create/IsAvailable/FallbackOf`（现有 `IsAvailable/Degrade` 复用，降级表 RTGI→SSGI→DDGI→IBL） |
| **2.5 验证承诺** | — | **实测新增一个源**（建议 M6.3 GTAO 或 Lightmap stub）：只实现接口 + 注册 + 面板自动出现，**不动管线/shader/面板代码**。做不到就说明抽象没到位 |

**验收**：2.5 的"新增源零侵入"实测通过；白炉测试仍通过；帧图节点数随层栈变化（RenderDoc）。
**风险**：**高**（本计划最大重构）。建议**单通道试点**（先 AO 通道：只有 SSAO/RTAO 两个源，最小代价验证抽象），再推 diffuse/specular。

---

## 五、Wave 3 · P5 · 频率分离（归一化之外的第二种合法合成）

> 前提：Wave 0 的白炉判据 + Wave 1 的按源合成。
> 物理依据：**归一化**解决"多源估同一量"的双重计数；**频率分离**解决"低频基底 + 高频细节"不重叠叠加。

| 任务 | 位置 | 做法 |
|---|---|---|
| **3.1 合成模式扩展** | `LightingPass.h:50-53` + `ShaderTypes.slang:100` | `GIBlendMode` 增 `FrequencySplit`；层栈 `mode` 已是通道级，直接可用 |
| **3.2 低频基底 + 高频残差** | `DeferredLighting.frag.slang` | 低频 = DDGI/IBL（可按降采样/低阶 SH 提取）；高频 = SSGI/RTGI **去低频残差**（`detail = c - LowPass(c)`），`gi = base + detail`（设计文档 3.3） |
| **3.3 过渡与时序稳定** | 同上 + `LightingPass` | 权重/层启用变化做时域平滑，避免模式切换跳变（设计文档 §4） |
| **3.4 面板 A/B** | `06.GILab` | 三模式（Additive 对照 / Normalized / FrequencySplit）同场景截图对比 |

**验收**：白炉测试在 FrequencySplit 下同样守恒（频段不重叠的数学前提）；Sponza 对比**无过亮、细节保留优于纯归一化**；模式切换无跳变。
**风险**：高（高频提取本身会引入噪声/振铃；`LowPass` 的实现方式需先做小实验选型）。

---

## 六、Wave 4 · 遗留质量/性能项（独立小项，可随时插入主线之间）

| 任务 | 位置 | 说明 | 规模 |
|---|---|---|---|
| M4.4 RSM VPL 降采样 25→16 | `DeferredLighting.frag.slang:272-281` | Poisson 盘替代 5×5；注意与 Wave 0.5 的 RSM 归位**同一处代码**，建议合并做 | 小 |
| M4.5 GBuffer 通道合并 | `DeferredLighting.frag.slang:82-94` | metallic/roughness 打包 R8G8B8A8 | 小 |
| M5.3 DDGI SH 修正 | `DDGI.comp.slang:155-159` + `RT_DDGI.slang:33` | 投影乘 `cos` + 评估端去 `max(result,0)` 截断（"均匀变蓝"根因）；**直接影响白炉测试的 DDGI 项正确性** | 中 |
| M5.2-A DDGI 光追射线 march | `DDGI.comp.slang:133` | 方案 A + 按 `supportsRayTracing` 自动选择（M5.2-B 已解决视角相关） | 中 |
| M5.1 ~~RTGI 时域累积~~ | `RT_GI.rgen.slang` | ✅ **已划掉——S1 已覆盖**（见 §1.2）：rgen 保持 SPP=1 无 history，累积与重投影由 `RTDenoiser`（velocity + 去遮挡）在帧图里完成。剩余可做项仅"降噪参数调优 / SPP 提升"，非缺口 | 小 |
| M6.3 SSAO→GTAO | `SSAO` | **建议作为 Wave 2.5 的"零侵入新增源"验证载体**，一石二鸟 | 中 |

> **注意**：M5.3（DDGI SH 修正）与白炉测试强相关——DDGI 是低频兜底源，其 SH 评估的 `max(...,0)` 截断会系统性丢失能量，白炉测试会把它暴露为"低频源偏暗"。**建议 M5.3 提前到 Wave 0 之后、Wave 1 之前**。

---

## 七、Wave 5 · 长期（按需）

| 任务 | 说明 |
|---|---|
| P6 统一估计器 | ReSTIR GI 把各源统一为「重采样 + 回退」框架（设计文档 P6，高风险） |
| Lightmap 源落地 | `GISourceId::Lightmap` 已预留；烘焙静态低频 |
| NRC / VXGI / LPV / SVOGI | 不在近期路线 |

---

## 八、依赖、顺序与执行约定

```
Wave 0.0（工具链 + 验证基线）  ✅ 已完成：能 configure、能编译、能跑
   └─→ Wave 0.1（偶发崩溃按死）        ← 新门槛：不修则"长跑 + 读回数值"的白炉判据不可信
          └─→ Wave 0（白炉判据 + IBL/RSM 归位）
                 └─→ M5.3（DDGI SH 修正，影响低频源能量）   ← 可与 Wave 0 并行/紧随
                 └─→ Wave 1（UBO 按源数组化 + shader 按源分派）
                        └─→ Wave 2（IGIProvider：先 AO 通道试点 → diffuse/specular）
                               └─→ Wave 3（FrequencySplit）
Wave 4 独立小项：随时插入（尤其 M4.4 与 Wave 0.5 合并、M6.3 与 Wave 2.5 合并）
```

**执行约定（延续现有，并按实测现实修订）**：
1. 每个任务**编译通过 ≠ 完成**——必须在 `06.GILab` 跑 exe 冒烟 + 用户实测确认。
   ⚠️ 这条约定在 9/7–9/11 期间实际未被执行过（§1.3）。**自 §1.3.1 起基线已具备**，恢复执行，并额外要求：**跑的时间要够长**（偶发崩溃 5 次里出现 1 次，短测会漏）。
2. 每个里程碑**停下等用户确认**再进下一个。
3. 不自动 commit；合入点由用户确认；commit log 中文、无 AI 相关字样。
4. 开工前**复核文档引用的行号**（本次审计已发现行号漂移与模型漂移）。
5. 添加代码**必须附中文注释**。
6. **状态标记口径统一**：`✅ 已落地` = 代码已写**且**在当前 HEAD 编译通过 + 06.GILab 实测确认；
   仅代码已写 → 标 `🟡 代码已写，验证待补`。本计划 §1.1 表中 9/8–9/11 的项按此口径应为 🟡。

**一句话顺序**：
`装工具链重建基线 → 白炉判据 → IBL/RSM 归位（能量守恒）→ DDGI SH 修正 → 层栈 UBO 按源数组化 → IGIProvider（AO 试点）→ 频率分离`，
Wave 4 小项穿插并行。

---

## 九、风险总表

| 风险 | 影响 | 对策 |
|---|---|---|
| **本机无工具链、无 Vulkan SDK** | ~~当前完全无法编译/运行~~ → **已解决**（§1.3.1） | ✅ 已装 VS 2026 v18 + Vulkan SDK 1.4.357.0；遗留坑：JoltPhysics 子模块需初始化、`python` 需在 PATH |
| **偶发崩溃（55 次 1 崩）** | 白炉等"长跑 + 读回数值"的验收被污染，可能误判为 GI 数值错误 | 崩溃点已定位到 `VulkanTexture::GetImageView()`（野指针，§1.3.2①）；已加存活守卫（防御+诊断）与尺寸 churn 修复；**根因待证**，下一步走 0.7（周期性违规）+ 0.8（崩溃处理器出调用栈） |
| **周期性 Vulkan 校验违规（确定性必现）** | 命令缓冲引用已销毁对象 + 布局推导错误 → GPU 同步正确性隐患，也是偶发崩溃的温床 | Wave 0.7：按 frame-in-flight 延迟销毁、布局模型纳入 pass 的 `finalLayout` 与跨帧状态、交换链信号量按 image 持有；判据=违规数归零 |
| ~~9/8–9/11 共 12 个提交从未编译过~~ | ~~首次编译可能成批报错~~ | ✅ **已证伪**：实测编译 0 错误（含 S3 删管线 + 02.Cube 迁移） |
| "已实测确认"的结论缺少证据支撑 | 排期与优先级建立在未验证的状态上 | 统一状态口径（§〇·0.4）；环境已通，后续逐项补验 |
| 合成改造后"画面变了" | 回归难判断 | 单源配置逐像素截图对比；白炉数值判据 |
| C++/slang 双侧同步漏改 | 静默错值（F1 老痛点） | `static_assert(sizeof)` + 双侧常量同源 + 改一处清单化 |
| 白炉读回设施工程量被低估 | 阻塞 Wave 0 验收 | 先做面板数值断言，自动化截图对比后置 |
| P4 抽象过度/爆改 | 大范围回归 | AO 通道单通道试点；适配器包装而非重写 |
| 频率分离的 `LowPass` 选型不当 | 噪声/振铃/跳变 | 先做离线小实验（降采样 vs SH 低阶）；保留 Normalized 作对照 |
| 文档与代码持续漂移 | 后续照文档实现出错 | 完成每个 Wave 时同步回写设计文档 3.x 数据模型与状态表 |

## 十、附录：历史里程碑与演进记录（原文保留）

> 本附录为原 `HugEngine GI优化开发计划.md` 的正文**逐字保留**（仅把小节标题层级下调一级以适配本文件结构）。
> 原文标题：《HugEngine GI 优化——后续开发计划》（日期 2026-09-08）；
> 原文依据：`docs/技术分析文档/HugEngine全局光照GI实现分析与架构优化方案.md`（分析 → 架构 → 落地 三部分合并文档）。
> 读法提示：其中"待做项"若已在主线（§一～§九）重新编排，**请以主线为准**（映射见 §〇·0.6）。
> 保留原文的价值在于：完整保存已完成工作的**原始任务描述、验收标准与提交哈希**，以及当时的判断依据。

---

### 一、总览

**核心结论**（来自基线 F1）：骨架已存在（`LightingSource`/`LightingInputSources` 已定义未接线），**问题在"接线"**，不是缺算法。所以路线是**增量改造，不重写管线**。

**执行约定**（结合项目规则）：
- 每个任务**编译通过 ≠ 完成**，必须在 `06.GILab`（Sponza GI 对比 sample）跑 exe 冒烟测试通过才算完成。
- 每完成一个里程碑，**停下等用户实测确认**后再进入下一个。
- 不自动 commit，每个合入点由用户确认。

**测试载体**：`06.GILab` sample（文档明确要求用它验证各档位组合；Sponza 场景是理想对比样本）。

---

### 二、里程碑地图（一览）

| 里程碑 | 内容 | 对应文档 | 优先级 | 规模 | 是否阻塞后续 |
|---|---|---|---|---|---|
| **M0** | 正确性 Bug 修复（3 处）✅ 已完成 | 第一部分·三·🔴 | 最高 | 小 | 否，但应先做 |
| **M1** | 接口收敛 + shader 通道化（Phase 1+2） | 第三部分·2/3 | 高 | 中 | 是（地基）|
| **M2** | 数据驱动 + 帧图自动编排（Phase 3+4） | 第三部分·4/5 | 高 | 中 | 否 |
| **M3** | Provider 注册表 + 自动降级（Phase 5，可选） | 第三部分·6 | 中 | 小 | 否 |
| **M4** | 性能优化（halfRes 等 5 项） | 第一部分·三·🟠 | 中 | 中 | 否 |
| **M5** | 质量提升（RTGI 累积、DDGI 修正） | 第一部分·三·🟡 | 中 | 中 | 否 |
| **M6** | 工业界进阶（ReSTIR GI 等） | 第一部分·五 | 按需 | 大 | 否 |

---

### 三、各里程碑详细任务

#### M0 · 正确性 Bug 修复（先行，风险最低）✅ 已完成（2026-09-08）

> 理由：两份文档一致把"先修 bug"放最前——低成本、消除隐性错误，且这些错误会在后续重构验证时干扰判断（例如 SSGI"半残"会让 M1 的行为等价验证失真）。
>
> **完成说明**：3 处 bug 已修复并提交（`5712721`、`55d1038`）。
> - M0.1 SSGI 投影矩阵：实测通过。
> - M0.3 RSM 守卫字段：编译验证（Deferred 管线无 RSM，防御性修复）。
> - M0.2 DDGI 参数：实测通过，并连带修复 3 个预先存在的 DDGI bug（深度绑定错误、开关门控缺失、参数未接线）。
> - 遗留：DDGI「均匀变蓝」（屏幕空间采样 + SH 余弦/截断，分析文档 bug #13/#14）已定位，归入 **M5**。

| 任务 | 位置 | 做法 |
|---|---|---|
| 0.1 SSGI 投影矩阵 | `SSGI.frag.slang:34` + `GI_SSGI.cpp:93` | 正向投影矩阵一并传入 UBO，参考 `SSAO.cpp:198-205` 同时传 `u_InvProj`+`u_Proj` |
| 0.2 DDGI 参数统一 | `RT_DDGI.slang:5-11` + `GI_DDGI.h:47` | 网格参数改 UBO/SSBO 传递，消除 C++/shader 三处不同步 |
| 0.3 RSM 守卫字段 | `DeferredLighting.frag.slang:245` | 校验 `shadowParams.w` 光源类型 + `lightCount>0`，参考同文件 210-211 行 |

**验收**：`06.GILab` 分别开关 SSGI/DDGI/RSM，无错位/越界/脏数据；SSGI 视觉恢复正常。**→ 暂停等用户测试。**

---

#### M1 · 接口收敛 + shader 通道化（Phase 1+2，地基）

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

#### M2 · 数据驱动 + 帧图自动编排（Phase 3+4）

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

#### M3 · Provider 注册表 + 自动降级（Phase 5，可选）

| 任务 | 文件 | 要点 |
|---|---|---|
| 3.1 注册表 + 降级链 | `GI/GIRegistry.h`（新增） | `Register/Create/IsAvailable` + `FallbackOf` 降级表（RTGI→DDGI→SSGI→None） |

**验收**：不支持的设备上 High 档自动落到 Medium 组合。**→ 暂停等用户测试。**

> M3 可独立，不影响 M0–M2；若时间紧可后置或跳过。

---

#### M4 · 性能优化（性价比从高到低）

| 任务 | 位置 | 杠杆 |
|---|---|---|
| 4.1 `halfRes` 落地 | SSGI/SSR/RSM/DDGI/SSAO | **最大**，省约 3/4 像素着色开销 |
| 4.2 SSR Hi-Z 层级追踪 | `GI_SSR.cpp:59` + `SSR.frag.slang` | 步数 64→~log2，质量反升 |
| 4.3 DDGI 1/4 分辨率 HDR | `DDGI.comp.slang:147` + `GI_DDGI.cpp:229` | 8192 次全分辨率采样带宽立减 |
| 4.4 RSM VPL 降采样 | `DeferredLighting.frag.slang:249-251` | 25→16 点 / Poisson 盘 |
| 4.5 GBuffer 通道合并 | `DeferredLighting.frag.slang:82-94` | metallic/roughness 打包 R8G8B8A8 |

**验收**：帧时间对比，各 halfRes/优化项下视觉无损。**→ 每项可单独暂停测试（4.1 优先单独验证）。**

---

#### M5 · 质量提升

| 任务 | 位置 | 要点 |
|---|---|---|
| 5.1 RTGI 时域累积/降噪 | `RT_GI.rgen.slang` | 补 history buffer + 重投影 + 降噪（当前 SPP=1 噪点严重）|
| 5.2 DDGI 探针屏幕外更新 | `DDGI.comp.slang:133` | 探针射线 march 或宽范围采样 + 更低 `blendAlpha` |
| 5.3 DDGI SH 修正 | `DDGI.comp.slang:155-159` + `RT_DDGI.slang:33` | 投影乘 `cos` + 评估端去 `max(result,0)` 截断 |

**验收**：噪点/鬼影/辐照度畸变对比改善。**→ 暂停等用户测试。**

---

#### M6 · 工业界进阶（按需 / 长期）

| 任务 | 说明 | 备注 |
|---|---|---|
| 6.1 ReSTIR GI | 复用现有 ReSTIR DI 基础设施推广到间接光 | **性价比最高**，实现 `GIMode::ReSTIR` |
| 6.2 DDGI→RTXGI | 探针 relocation/可见性/无限滚动 | 不动现有架构 |
| 6.3 SSAO→GTAO | 成本低、画面提升明显（UE 默认）| |
| 6.4 长期方向 | Lightmap、VXGI/LPV/SVOGI、NRC 神经缓存 | 不在近期路线 |

---

### 四、依赖与顺序说明

1. **M0 必须先做**：三处 bug 是隐性错误，且会影响 M1「行为等价」的验证基准。
2. **M1 是地基**：文档明确 Phase 1+2 先合；先把 `LightingPass` 签名和 shader 通道化稳定，后续 M4 性能工作才不会返工（M4 会触碰帧图与 pass 尺寸，若在 M2 前做会重复改动）。
3. **M2 紧随 M1**：M2 依赖 M1 的结构体与枚举（`GIConfig.ToInputSources()` → `LightingInputSources`）。
4. **M3 独立**：随时可做，不影响主线。
5. **M4/M5/M6 在架构稳定后**：都是增量，可并行推进，但每项需单独实测。

**一句话顺序**：`M0 修 bug → M1 打地基 → M2 数据驱动 → M3(可选) 降级 → M4 性能 → M5 质量 → M6 进阶`。

---

### 五、风险与验证策略

- **重构等价性风险**（M1）：最大风险是"改了但视觉变了"。用 `06.GILab` 重构前后截图/帧时间对比，逐项确认无回归。
- **枚举/结构体跨文件改动**：`LightingSource` 枚举、push constant 是 C++/slang 双侧同步，改一处必须两侧一起改（这是文档强调的 F1 痛点）。
- **每个任务开工前**先复核文档引用的行号是否仍准确（代码可能已漂移），再动手。

---

### 六、进度状态更新（2026-09-10）

> 依据执行约定：每里程碑在 `06.GILab`（Sponza GI 对比）冒烟验证，用户实测确认后合入。

| 里程碑 | 状态 | 提交 |
|---|---|---|
| **M0** 正确性 Bug | ✅ | `5712721` / `55d1038` |
| **M1** 接口收敛 + shader 通道化 | ✅ | `59c3382`（LightingInputs 33→结构体）+ `65d9cf5`（giIntensity/aoIntensity push constant） |
| **M2** 数据驱动 + 帧图自动编排 | ✅ | `2b06045`（GIConfig + 4 质量档位 + 帧图条件注册） |
| **M3** Provider 注册表 + 自动降级 | ✅ | `66faf5c`（GIRegistry IsAvailable/FallbackOf/Degrade） |
| 命名/语义优化 | ✅ | `9f09c87`（ShadowChannel::CSM→Raster）、4 通道枚举拆分 + `ddgiOverlay` 叠加语义（`66faf5c`） |
| 能力查询修正 | ✅ | `da2bbb4`（GIRegistry::IsAvailable 基于 device->GetCaps().supportsRayTracing，非硬编码 RT 不可用） |
| CSM 阴影越界修复 | ✅ | `2d2db2a`（SampleShadowPCF 超出 splitDistances[2] 返回无阴影，消除方形暗区） |
| **M4** 性能优化 | 🔄 部分 | 4.1（SSGI/SSR/SSAO halfRes）+ 4.2（SSR Hi-Z）+ 4.3（DDGI 1/4 HDR）完成，见下 |
| **M5** 质量提升 | ⏳ 部分 | M5.2-B（探针辐射度视角无关）完成，见下 |
| **M6** 工业界进阶 | ⏳ 按需 | — |

#### M4 进展说明

- **4.1 halfRes**（最大杠杆，SSGI/SSR/SSAO 完成——`52d8280` + `9b6e612` + `5441d2e`）：
  - SSGI/SSR/SSAO：`halfResW/H` helper + Initialize/OnResize 半分辨率纹理；帧图 viewport 用纹理实际尺寸。
  - halfRes 时跳过 Denoise（省开销、避免尺寸不匹配）；AO/Blur 半分辨率。
  - 06 档位应用：Low 档 `halfRes=true` 同步 `GISettings.halfRes` + 帧边界延迟重建纹理（ImGui 回调内重建会死锁）。
  - 待做：RSM VPL 的 halfRes（见 4.4）。
- **4.2 SSR Hi-Z 层次追踪**（`5441d2e`）：
  - 复用 GPUCulling 深度金字塔（`GetHiZTexture`，min 深度 Reverse-Z）；SSR.frag 层次 march + 线性回退；binding 4 + push constant useHiZ。
- **4.3 DDGI 1/4 分辨率 HDR 捕获**（`afb7587`）：
  - m_PrevHDR 改 1/4 分辨率 + FullscreenCopy 下采样渲染，降低探针采样带宽。
- **4.4 RSM VPL 降采样（25→16 点）/ 4.5 GBuffer 通道合并（R8G8B8A8 打包）**：未开始。

#### 代码可读性整理（本批）

- `GI_SSGI.cpp` / `GI_SSR.cpp`：每语句一行 + 分段注释 + 变量命名。
- 遍历引擎自有 cpp（排除 External 第三方库）：把一行多语句拆为每语句一行（拆 80 文件 558 行，`c536951`；保守跳过 for 头/宏/块/lambda）。

#### M5 进展说明

- **M5.2-B DDGI 探针辐射度视角无关**（✅ `afb7587`，用户实测确认——不再随视角/相机位置变化）：
  - 根因：`DDGI.comp` 探针辐射度来自**屏幕 HDR**（`u_PrevHDR` + `u_ViewProj`）——视锥外采样点被丢弃 → 视角转动探针变化。
  - 解决：探针辐射度来源重构——**RSM 世界辐射度**（有方向阴影时优先，固定光源视锥）+ **IBL 辐照度回退**（纯 GI 场景，Cubemap 按方向采样），**彻底移除屏幕 HDR 采样**（世界空间、视角无关）。
  - 配套修复：RSM 用固定光源视锥（CSM 的 lightViewProj 拟合相机视锥会引入视角相关）；DDGI_Update 改 Graphics 队列（与 RSM_Generate 顺序执行，避免跨队列竞态）。
  - 方案 A（硬件光追射线 march）+ 按 `supportsRayTracing` 自动选择：未开始。
- **M5.3 DDGI SH 修正**（cos 投影 / 去截断——M0.2 遗留"均匀变蓝"）：未开始。

#### 命名与结构演进（本批）

- `LightingSource`（大枚举混 4 通道）→ **4 个独立枚举**：`ShadowChannel`（Raster/RT）/ `AOChannel`（SSAO/RTAO）/ `SpecularChannel`（SSR/RT）/ `DiffuseChannel`（SSGI/RTGI）——类型安全。
- `LightingInputSources` → `GIChannels`；`useDDGI`/`ddgiEnabled` → `ddgiOverlay`（DDGI 叠加语义）；`ShadowChannel::CSM` → `Raster`（光栅化阴影统称）。

#### GI 通道按管线区分（`922b3fe`）

- `PipelineGICap` 能力位 + `PipelineCaps::Forward/Deferred/HybridRT` 预设（各管线支持的通道子集；Forward 实际能力 = 光栅阴影 + IBL + RSM）。
- `GIRegistry::IsAvailable/Degrade` 加管线维度：可用性 = **管线能力 ∧ 设备光追能力**；降级链 RTGI→SSGI→None，管线不支持 DDGI 时关叠加。
- `IRenderPipeline::GetGIConfig()/GetGIPipelineCaps()` 统一接口；Forward/Deferred/HybridRT 各持独立 `m_GIConfig`（Initialize 按各自能力降级）。
- Forward 帧图接入 GIConfig（`rsmIndirect` 门控 RSM_Generate）。
- 修复 **Deferred RSM 间接光未绑定**（layout 声明 15/16 但 Render 从未绑定 → shader 采样未初始化纹理）；修复聚光灯阴影 binding 冲突（`kGPUBinding_SpotShadow_DL=9`）（`aa0de11`）。

#### HybridRT 接入 IBL（`237eb6a`）

- 此前 HybridRT 无 `m_GI`（GI_IBL），Lighting 采样占位纹理 → 环境光/粗糙面反射错误。
- 现 Initialize 创建 GI_IBL + 帧图 Lighting 前 `IsDirty` 生成并 `SetIBLTextures`。必要性：RT 反射的粗糙面回退 IBL prefilter（`RT_Reflection.rgen:87`）+ DeferredLighting 基础 IBL + DDGI 的 IBL 回退。

#### binding 魔法数字 → 常量重构（多提交）

- 共享 PerFrame 集（LightingPass/ForwardPipeline/GBufferRenderer）用 `kGPUBinding_*`；38 个独立描述符集模块各自定义绑定常量：GI（`kSSGIBind*/kSSRBind*/kDDGIBind*/kRSMBind*/kIBLBind*`）、GPUCulling（4 集）、后处理/AA、RT（`kRT*Bind*`）、ReSTIR（3 pass）、粒子（6 阶段）——`beab15a` `667dcbb` `f32d62e` `19e206d` `aa4d657` `1c3ea76` `f561977` `5977357` `f1aa7ce`。
- 常量按对应 shader 声明语义命名（需查 shader 的文件已逐个核对）。RTPass 因 layout 由调用者传入而保持数字。

#### 06.GILab GI 对比测试平台（本批）

- **三管线切换**（Forward/Deferred/HybridRT，面板下拉菜单——`237eb6a`；设备不支持光追时 HybridRT 回退 Deferred）——06 内直接对比各管线 GI。
- **独立面板**：GI 控制台（渲染管线 → GI 质量档位 → GI 四通道 + 可用性标记 + 自动降级 + 各通道参数）与 GPU Profiler（`dd7c1b9`）。
- **面板状态序列化**：管线/档位/只看 GI/四通道/强度/SSAO 参数/相机速度（`b9349dd`）；面板几何（位置/大小/折叠）经 ImGui ini（`Content/Config/06_GILab_imgui.ini`，退出前显式保存 + 绝对路径——`dd7c1b9` `8767837`）。
- **主面板光源信息**：方向光/点光/聚光/矩形光列表（开关/颜色/强度/方向/范围，可编辑）+ 场景新增 2 个点光源（X 轴 ±20 冷暖双色，供点光 GI/阴影测试）（`fb1e7c8`）。
- 移除主面板重复的 SSAO 选项（SSAO 统一在 GI 控制台 AO 通道）；修复档位切换卡死（纹理重建延迟帧边界 + WaitIdle）。

---

### 七、GI 分层合成架构演进（2026-09-10 晚，本批）

> 起因：多个 diffuse GI（SSGI/DDGI/RTGI）描述的是**同一个物理量**（间接入射辐射度），
> 简单相加会**双重计数**（能量翻倍、白炉测试失败）；而"单值枚举选一个"无法表达
> 「远场探针管低频 + 屏幕空间管中频 + 光追管高频」的分工。
> 详见设计文档 `HugEngine GI分层合成架构设计.md`。

#### P1/P2 · 分层合成基础（`105911b`）

- **GIBand**（低频/中频/高频）+ **GIBlendMode**（相加 / 归一化加权）。
- **间接光改为归一化加权** `Σ(源×w)/Σw`，修掉多源相加的双重计数。
- **权重以各源置信度为主**（屏幕边缘可见性），用户权重为辅；**距离让位默认关闭**
  ——它是性能/艺术控制，不是物理判据（远处但屏幕内清晰可见的物体，屏幕空间 GI 依然可信）。
- **参数按「源类型」命名**：`screenSpaceWeight` / `rayTracingWeight` / `probeWeight`
  + 可选 `...FalloffDistance`——不再绑定 SSGI/RTGI 等具体技术名，diffuse/specular/AO 三通道同构。
- **specular 同构化**：修掉 SSR 与 IBL prefilter 的双重计数（`105911b`）。
- 移除与 `probeWeight` 语义重复的 `ddgiOverlay`（统一由权重表达参与程度）。

#### P3 · 源层栈架构（`105911b`）

- **`GIConfig` 的 4 个单值枚举 → 4 个 `GIChannelStack`（源集合）**：
  「选技术」= 该源 `weight>0`；「融合」= 多个源同时 `weight>0`。
- 新增 **`GISourceId`**（12 种源）+ `GIBandOf` / `GISourceName` / `IsRayTracingSource`。
- **帧图门控全部由层栈派生**（`ShouldRunSSGI/DDGI/AO/RTReflection/...`）。
- **`GIRegistry::Degrade` 改为逐源裁剪**（管线能力 ∧ 设备能力）+ 通道裁空兜底。
- **混合参数改经 `GIBlendParams` UBO（binding 31）**传递——3 通道 × 32B 超出
  push constant 128B 上限，且便于后续层栈扩展。
- 06.GILab：GI 通道改为**源列表 UI**（源 / 频段 / 权重 / 让位距离 / 合成方式），
  层栈逐源权重写入 cfg。

#### 回归修复（`105911b`）

- **DeferredPipeline 此前未初始化 `m_GIConfig`**（默认构造 = 空层栈 → 无 GI → 画面发黑）；
  现以 Medium 档位为默认基线并逐源裁剪，且**按层栈同步 GI 子系统开关**（层栈说"参与"就必须真的跑）。
- Forward/HybridRT 默认基线同样改为 Medium 预设。

#### S1 · 光追归入 Deferred（`90649ba` `aac5690`）

> **关键认识**：光追是「GI 源」，**不是「管线类型」**——HybridRT 与 Deferred 共享
> GBuffer/Lighting/后处理，差异仅在效果来源。

- DeferredPipeline 新增 RT 基础设施：`RTPass`（AS/TLAS）+ `RTShadowPass`/`RTAOPass`/
  `RTReflectionPass`/`RTGIPass` + 4 个时域降噪器 + 反射/GI 空间滤波。
- 帧图：`AnyRTSource()` 为真 → `AS_Build` + 场景材质纹理（首帧）+ 各 RT 效果 + 降噪链，
  **全部以 `ShouldRunRT*()` 层栈条件门控**（不再依赖 CVar）。
- Lighting 接入：`in.rtGI` / `in.rtShadowMask` / `in.rtAO` / `in.rtReflection` → shader 走光追路径。
- **四个 RT 源全部可用**（RTGI + RT 反射 + RTAO + RT 阴影）。

#### S2 · 管线维度收敛（`06c8580`）

- 06.GILab 管线下拉收敛为 **Forward / Deferred** 两项，移除 `hybridPipeline` 实例
  （少一份 GBuffer/Lighting/后处理资源）；旧配置 `pipeline_mode=2` 自动映射到 Deferred。

#### S3 · 移除 HybridRTPipeline（`0f8aca8`，净删 1245 行）

- 删除 `Engine/Render/Pipeline/HybridRTPipeline.h/.cpp` 及 CMakeLists 条目。
- 02.Cube 迁移：移除实例/初始化/GPU Culling 同步/OnResize/Shutdown/粒子注册/RT 效果开关面板；
  模式 2 改为走 Deferred（光追经层栈 RT 源）。

#### PT · 参考渲染器定位与资源复用（`3301040`）

- **明确 PT 的定位**：参考渲染器（ground truth）——不参与实时渲染、**不使用 GI 层栈**
  （自己求解完整渲染方程），收敛结果作为其他近似 GI 的判定基准。
- 与 HybridRT 的区别写明：后者只是效果配置差异（已并入层栈），PT 是完全不同的渲染范式。
- **加速结构共享**：`DeferredPipeline::GetRTPass()` + `PathTracingPipeline::SetSharedRTPass()`
  ——PT 复用 Deferred 的 BLAS/TLAS，加速结构内存减半。

#### S1.5 · 各通道两类源可真正同时参与（`5c2b84b`）

- 此前屏幕空间源与光追源是「二选一」（`rtDiffuseSource`/`rtSpecularSource`/`rtAOSource` 切换），
  层栈里两个源的权重实际只有一个能生效。
- 现改为**各自独立采样、同时参与归一化合成**：
  - **Diffuse**：SSGI + RTGI + DDGI 三者可同时融合
  - **Specular**：SSR + RT 反射 + IBL prefilter
  - **AO**：SSAO + RTAO（由二选一改为归一化加权）
- 每源各自计算置信度与可选距离让位，权重和归一化 → 多开一个源不会变亮；单源行为不变。

#### 终态架构

```
渲染管线（架构差异）              GI 源层栈（效果差异，自由组合）
├─ ForwardPipeline               低频： IBL / Lightmap(预留) / DDGI
├─ DeferredPipeline              中频： SSGI / SSR / SSAO / RSM
└─ PathTracingPipeline（参考）    高频： RTGI / RT 反射 / RTAO / RT 阴影
        ↓                                   ↓
   架构不可合并                       归一化加权合成（物理正确）
```

**待办**：① 白炉测试（用 PT 做基准验证层栈能量守恒）② P5 频率分离
③ M4.4 RSM VPL 降采样 ④ M4.5 GBuffer 通道合并 ⑤ M5.3 DDGI SH 修正 ⑥ M6.3 GTAO。
