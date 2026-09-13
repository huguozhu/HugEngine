# HugEngine GI 分层合成——后续开发计划（Wave 0 – Wave 5）

> 日期：2026-09-13（第三轮修订：环境已通 + 编译/运行基线实证完成）
> 依据：`HugEngine GI分层合成架构设计.md`（设计）+ `HugEngine GI优化开发计划.md`（里程碑）
> 方法：先**代码审计核对**两文档的「已落地/待做」声明，再排后续波次
> 本计划的关键输入：
> 1. **验证基线此前不存在**——`06.GILab` 从未被编译、从未被运行过（§1.3，实证）；**该门槛现已打通**：工具链齐备、编译 `BUILD EXIT: 0`、连续运行到 Frame 431–530（§1.3.1）
> 2. 文档未记录的 **3 处结构性缺口**（§1.4），不修则白炉测试必然失败
> 3. **M5.1（RTGI 时域累积）实际已被 S1 的降噪链覆盖**（§6），应从待办中划掉，避免重复劳动
> 4. **运行存在偶发崩溃**（5 次启动 1 次 `0xc0000005`，§1.3.1③）→ 提升为 **Wave 0.1**，先于白炉测试

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
| ~~M5.1 RTGI 时域累积~~ | ✅ **实际已被 S1 覆盖，应从待办划掉**（详见 §6）：`RTDenoiser` 已有 history + 基于 velocity 的重投影 + 时域混合 + 去遮挡（`PostProcess/RTDenoiser.h:18,75`、`RT_DenoiseTemporal.frag.slang:44-68`），且 Deferred 帧图已把它挂在 RTGI 输出上（`DeferredPipeline.h:193` `m_GIDenoiser`）。rgen 内 SPP=1、无 history 是**正确设计**（累积归降噪器）。剩余价值仅为降噪参数调优 / SPP 提升 |

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

三次运行数字完全相同 → 这些是**确定性、周期性**发生的问题（约每 N 帧一轮，每轮 1 barrier + 1 renderPass + 1 semaphore + 11 invalidState），
与启动 churn 无关，需单独定位（见 Wave 0.7）。它们属于"命令缓冲引用的对象在提交完成前被销毁"这一类，
是**长期运行稳定性与 GPU 同步正确性**的隐患，也让偶发崩溃持续可行。

#### 1.3.3 Wave 0.1 当前状态（诚实口径）

- 合并统计：**55 次启动 / 1 次崩溃**（唯一一次发生在"新链接二进制的首次启动"，约 5 秒处）。
- 崩溃点已定位到函数级；两项修复已落地（一项已验证消除 churn，一项为防御+诊断，未命中）。
- **根因尚未被证明**——因此 Wave 0.1 **不算完成**，验收判据保持"连续 10 次冷启动零崩溃"（当前已连续 18 次干净）。
- 下一步更有把握的路径：**修掉 §1.3.2③ 的周期性校验违规**（确定性、可验证），并考虑给 06.GILab 加一个
  `SetUnhandledExceptionFilter` + `StackWalk64/DbgHelp` 的崩溃处理器，让下次偶发崩溃直接打出**完整调用栈**。

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
| **0.7 周期性校验违规（确定性，必现）** | `RenderingPass`/RHI 资源生命周期 + RenderGraph barrier | 实测每轮稳定出现：`command buffer … invalid state`（110 次/8 秒，因 framebuffer 被销毁）、`barrier oldLayout`（10）、`render pass 附件布局`（10）、`swapchain semaphore pending`（10），三次运行数字完全相同 ⇒ 确定性、周期性（§1.3.2③）。方向：① 命令缓冲引用的 framebuffer/texture **延迟到帧可复用后再销毁**（按 frame-in-flight 回收）；② RenderGraph 的布局模型要纳入 render pass 自身的 `finalLayout`（阴影贴图 `VulkanPipeline.cpp:203` 声明结束时为 `DEPTH_STENCIL_READ_ONLY`，而模型记为 `ATTACHMENT`）与跨帧真实布局（导入资源不能假设 `Undefined`）；③ 交换链信号量按 image 索引持有、避免复用未完成者 |
| **0.8 崩溃处理器（诊断基建）** | `06.GILab`（Debug 可选开关） | `SetUnhandledExceptionFilter` + `StackWalk64`/`DbgHelp`，下次偶发崩溃直接输出**完整调用栈**（比 `/MAP` 反查更直接）。配合已有 `VulkanTextureLiveness` 守卫，可把偶发问题的定位从"猜"变成"读栈" |

**验收**：
- [x] **（0.0 门槛）** HEAD 能 configure + 编译通过，产出 `06.GILab.exe`（§1.3.1②）。
- [x] 启动期尺寸 churn 消除（`1920x1080` 纹理创建 26 → 0；§1.3.2②）。
- [ ] **（0.1 门槛）** 连续 10 次冷启动零崩溃；白炉测试期间可长时间稳定运行。→ 崩溃点已定位到函数（§1.3.2①），
      防御守卫已加，但**根因未证明**，故此项**未通过**（当前连续干净 18 次，不足以在 1/40 概率下断言已修）。
- [ ] **（0.7 门槛）** 上述 4 类周期性校验违规归零（这是确定性、可验证的硬判据，比偶发崩溃更适合作门槛）。
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
| "已实测确认"的结论缺少证据支撑 | 排期与优先级建立在未验证的状态上 | 统一状态口径（§8 约定 6）；环境已通，后续逐项补验 |
| 合成改造后"画面变了" | 回归难判断 | 单源配置逐像素截图对比；白炉数值判据 |
| C++/slang 双侧同步漏改 | 静默错值（F1 老痛点） | `static_assert(sizeof)` + 双侧常量同源 + 改一处清单化 |
| 白炉读回设施工程量被低估 | 阻塞 Wave 0 验收 | 先做面板数值断言，自动化截图对比后置 |
| P4 抽象过度/爆改 | 大范围回归 | AO 通道单通道试点；适配器包装而非重写 |
| 频率分离的 `LowPass` 选型不当 | 噪声/振铃/跳变 | 先做离线小实验（降采样 vs SH 低阶）；保留 Normalized 作对照 |
| 文档与代码持续漂移 | 后续照文档实现出错 | 完成每个 Wave 时同步回写设计文档 3.x 数据模型与状态表 |
