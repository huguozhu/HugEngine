# Nanite 设计与实现

> 最后更新: 2026-09-19（含 §14 独立模块化架构与重编号任务清单）
> 状态: 设计规范已定稿；**实现方案已按"独立模块 + 独立开关"重新定形**（§14），
> 任务从 1 重新编号（§14.8，共 27 项，覆盖 N0 前置 + N1–N6 + 横切）；旧 §12 Task 1-10 的
> 详细字段与判据仍有效，对应关系见 §14.9
>
> **另起会话实施 Nanite 时：先读 §14.12「接手须知」**（分支、配置漂移、验收基线、构建纪律、第一条任务）。

## 0. 本文件怎么读

- **第一~九章是设计**：回答"做成什么样" —— 目标与定位、现有基础设施、数据流、预处理与
  运行时的形状、运行时 GPU 资源、与现有管线的集成点、关键数据结构、里程碑与已知风险。
- **第十章起是 N1-N3 实现计划**：回答"怎么按任务落地" —— 全局约束、文件结构、Task 1-10 的
  逐个步骤（含代码骨架与验收标准）、完成标准。Task 里的 `- [ ]` 复选框原样保留，用于逐任务跟踪。
- **§14 是本次评审新增**（放在文件末尾）：把 Nanite 定形为**独立模块 + 独立开关**
  （模块边界、开关三层、帧图接入契约、与现有代码的接触面），并给出**从 1 重新编号的完整任务清单**
  （§14.8，27 项）。**新任务按 §14.8 的编号引用**；旧 §12 的 Task 1-10 保留其字段/格式细节，
  编号对应关系见 §14.9。§14.1 还给出了设计章"可复用设施"的**代码实况校正**（哪些假设已不成立）。
- **合并来源**：本文件由两份旧文档合并重写 ——
  ① 旧《Lumen + Nanite 完整设计规范》中与 **Nanite / 虚拟化几何**有关的全部内容（原 §1 现有
  基础设施中与几何/网格处理相关者、原 §3 全部小节、原 §4 的 Nanite 部分、原 §5.3 的 Nanite
  里程碑与推进顺序、原 §6 的 Nanite Cluster、原 §7 的 Nanite 风险），以及
  ② 旧《Nanite N1-N3 实现计划》全文。
  **Lumen 内容不在本文件**（由 `Lumen设计与实现.md` 承接）；原文里同时涉及两者的表述
  （如 BuildFrameGraph 的 pass 链）一律保留，并逐条注明哪个 Pass / 哪一行属 Lumen。
- **"现状 / 实证"的写法**：设计章里凡是声称"现有基础设施"的地方，都给了代码实证
  （`文件:行号` 或符号名）；检索为 0 命中的也写明关键词。核验时间：2026-09-19，
  核验结果见 §2.2 与 §2.3。
- **"注：源文档此处不一致"**：两份源文档互相打架、或同一份源文档内部自相矛盾时，本文件
  **保留两种说法并显式标注**，不私自裁决。明细见 §0.2。
- **术语**：本文件统一使用 Nanite 的准确术语**虚拟化几何**（virtualized geometry）——
  把"整网格一次绘制"改成"可见 cluster 集合的绘制"。"体素化（voxelization）"在
  HugEngine 里属于 Lumen 的 SDF 体系，不是 Nanite。

### 0.1 章节索引

| 章 | 内容 | 性质 |
|---|---|---|
| §1 | 目标与定位（虚拟化几何 / cluster 层级 / 软硬光栅分工） | 设计 |
| §2 | 现有基础设施（可复用部分 + 事实核验） | 设计（含代码实证） |
| §3 | 数据流 | 设计 |
| §4 | 预处理（Python 工具链：cluster 划分、LOD/DAG、量化、打包） | 设计 |
| §5 | 运行时（C++：NaniteComponent、GPU 上传、剔除、光栅化） | 设计 |
| §6 | 运行时 GPU 资源 | 设计 |
| §7 | 与现有管线集成（DeferredPipeline 扩展、新增 Shader 文件） | 设计 |
| §8 | 关键数据结构（Nanite Cluster GPU 布局等，逐字段） | 设计 |
| §9 | 里程碑与已知风险 | 设计 |
| §10 | 全局约束（Global Constraints） | 实现计划 |
| §11 | 文件结构（File Structure） | 实现计划 |
| §12 | 任务清单（Task 1 … Task 10） | 实现计划 |
| §13 | 完成标准 | 实现计划 |
| §14 | **独立模块化架构 + 重编号任务清单（27 项，从 1 开始）** | 实现计划（本次评审新增，以 §14.8 编号为准） |
| §14.12 | **接手须知（新会话从这里开始）**：分支/配置漂移/验收基线/构建纪律/第一条任务 | 实现计划（**另起会话时先读这一节**） |

### 0.2 源文档不一致清单（保留双方说法，不裁决）

合并时发现下列不一致。**两处都保留**，并在正文对应位置就地标注。

| # | 条目 | 说法 A | 说法 B | 位置 |
|---|---|---|---|---|
| 1 | `NaniteCluster` 第 2 个字段命名 | 设计：`float4 coneData`（"normal cone（法线锥剔除）"） | 计划：`float4 coneAxisAngle`（"xyz=coneAxis, w=coneAngle(cos)"） | §8.1；**任务 7 定稿：取 `coneAxisAngle`** |
| 2 | Python 预处理工具的落点 | 设计 §4：`Engine/Shader/Shaders/Nanite/Nanite_Preprocess.py` | 计划：`Tools/NanitePreprocess/NanitePreprocess.py`（另拆 5 个模块） | §4.3 / §11 |
| 3 | 软光栅 Shader 文件名 | 设计 §4：`Nanite_SoftRasterize.comp` | 计划：`Nanite_SoftRaster.comp` | §7.2 / §11 |
| 4 | Shader 扩展名规范 | 计划 Global Constraints："Shader 统一使用 Slang `.comp`/`.mesh` 命名规范"；设计 §4 与计划 File Structure 均写 `.comp`/`.mesh` | 仓库实际：`*.comp.slang` / `*.mesh.slang`（`Engine/Shader/CMakeLists.txt:125-127`） | §10 / §11 / §12 Task 6、8 |
| 5 | 软/硬光栅分流阈值 | 设计 §3.3：`triCount > 16` → Mesh Shader，`<= 16` → Compute 软光栅 | 计划 N1-N3：只有软光栅，`Nanite_SoftRaster.comp` 对 cluster 最多 64 个三角形统一处理，无 16 三角形阈值 | §5.2 |
| 6 | 索引编码 | 计划 `NaniteTypes.slang` 注释："3×u16 打包到一个 u32[2]" | 计划 `NanitePack.py` 按 `indexCount × 4B` 写 u32/索引；`Nanite_SoftRaster.comp` 逐 u32 取 3 个索引 | §8.5；**任务 7 定稿：取 3×u16 进 `u32[2]`（8B/三角形，簇内局部下标）** |
| 7 | 量化顶点步长 | 计划 `NaniteVertex` = 4×u32（含 `_pad`）= 16B | 计划 `NanitePack.py` 每顶点写 12B；`NaniteUpload.cpp` 按 `vertexCount*3*sizeof(u32)` = 12B/顶点读 | §8.4；**任务 7 定稿：取 16B，第 4 个 u32 改为 `quantBias`** |
| 8 | `.nanite` 文件头大小 | 计划 `pack_nanite` docstring：`[NaniteFileHeader 128B]` | 按字段累加 = 96B（`8+4×6+4+12+12+4+32`），Python 写 `<32x>` reserved | §8.3；**任务 7 定稿：取 96B** |
| 9 | 量化/反量化对称性 | 计划 `quantize_vertices`：`(vertices-bbox_min)*scale`，打包无符号 0…1023 | 计划 `decodeVertexPosition`：`int(packed & 0x3FF) - 512`，按 SNORM 有符号解码 | §8.4；**任务 7 定稿：编码端补 `+512`，与解码互逆** |
| 10 | 软光栅入参 | 计划 Task 8 Interfaces："Consumes: visible clusters" | `Nanite_SoftRaster.comp` 只绑 `u_Clusters` 并按 `tid.x` 直接索引，未绑 Task 7 的可见 cluster 列表 | §12 Task 8 |
| 11 | `RasterParams` 字段名 | 结构体声明 `uint materialID; // bindless base` | 函数体使用 `u_Params.materialBase` | §12 Task 8 |
| 12 | "Cluster 剔除（两阶段）" | 设计 §3.3 小标题写"两阶段" | 同一小节正文列了 Phase 1 / Phase 2 / **Phase 3**（LOD Selection） | §5.1 |
| 13 | 顶点量化的覆盖范围 | 设计 §3.1/§3.2：压缩编码含"顶点量化 R10G10B10A2、位压缩"，`NaniteVertex` 定义了 position/normal/UV 三个 packed 字段 | 计划 Task 3 的 `quantize_vertices` 只打包 position，normal/UV 的打包无实现 | §4.2 |
| 14 | GBuffer MRT 数量 | 设计 §3.3："Nanite 光栅化直接写现有 **5×MRT** GBuffer"；计划 Task 8 写 6 个目标 | 代码实际：`kGBufferAttachmentCount = 8`（`Engine/Render/Pipeline/GBufferRenderer.h:16`），`BeginOffscreenPassMRT(cv, 8, ...)` | §2.3 / §7.1 |
| 15 | 里程碑覆盖范围 | 设计 §5.3：Nanite 里程碑 N1-N6 | 计划：只覆盖 N1-N3，完成标准表只列 N1/N2/N3 | §9.1 / §13 |

### 0.3 自引用链接改写记录

为避免指向即将删除的旧文件名，正文中对下列自引用做了改写（指向仍存在的文档时保留原链接）：

| 原引用 | 改写为 |
|---|---|
| 计划文档开头 `**Spec:**` 指向旧《Lumen + Nanite 完整设计规范》 | `**Spec:** docs/计划实现功能/Nanite设计与实现.md`（本文件） |
| 设计 §5.3 推进顺序中的 `§5.1 统一降噪框架（11.3）` | "统一降噪框架（Lumen / GI 框架相关，见 `docs/已实现功能/Lumen设计与实现.md`）" |
| 设计 §5.3 推进顺序中的 `§5.2 框架前置（Provider 执行单位 / 绑定数组化）+ P6 统一估计器（长期）` | "框架前置（Provider 执行单位 / 绑定数组化）+ P6 统一估计器（长期；均为 Lumen 相关，见 `Lumen设计与实现.md`）" |
| 设计 §1 表格 Denoiser 行的 `**统一降噪框架**见 §5.1` | "统一降噪框架属 Lumen，见 `Lumen设计与实现.md`" |
| 设计 §5.1/§5.2 内的《HugEngine GI 架构与开发计划》§4.4 与《ReSTIR PT / GRIS 预研》引用 | 随 §5.1/§5.2 一并归入 Lumen 文档，本文件不展开（相关判据按 §0.2 的口径只做归属说明） |

保留的、指向仍存在文档的链接：`docs/已实现功能/DeferredPipeline实现规范.md`、
`docs/HugEngine引擎介绍/HugEngine技术全景与实施计划.md`（见 §2、§7）。

---

## 一、设计

### 1. 目标与定位（虚拟化几何 / cluster 层级 / 软硬光栅分工）

**定位**：Nanite 是 HugEngine 延迟渲染管线的虚拟化几何前端。它不改材质与光照的评估方式，
而是改**几何的提交方式**：预处理把网格切成固定上限的 cluster 并生成多级 LOD / DAG，运行时
每帧只提交"当前相机能看到、且 LOD 合适"的 cluster，光栅化后写入现有 GBuffer，交给既有的
Lighting 与后处理链。

**三条主线**：

| 主线 | 内容 | 落点 |
|---|---|---|
| cluster 层级 | 每个 cluster ≤64 三角形 / ≤128 顶点；LOD 由边折叠逐级减半生成；跨 LOD 去重成 DAG | 离线（Python，§4） |
| 剔除链 | Instance Culling（视锥 + Hi-Z）→ Persistent Cluster Culling（BVH 遍历 + 两阶段）→ LOD Selection（projected error < 1 pixel） | 运行时 GPU（§5.1） |
| 软硬光栅分工 | `triCount > 16` → Mesh Shader（`VK_EXT_mesh_shader`）；`triCount <= 16` → Compute 软光栅 + interlock 写 GBuffer | 运行时 GPU（§5.2） |

**两条写目标的路线**：

| 路线 | 写目标 | 优势 / 代价 |
|---|---|---|
| Phase 1（N3 采用） | 复用现有 DeferredPipeline 的 GBuffer | 复用全部后处理；直接验证 cluster 渲染正确性；不需要改 Lighting Pass |
| Phase 2（后续） | Visibility Buffer（triangleID + depth，延迟材质评估） | 材质评估与几何解耦；需要新增材质解析 Pass |

**Material Bin**：按材质分组 cluster → 绑定 Bindless 纹理 array；一次 Draw/Dispatch 处理同一材质的
多个 cluster，减少 bindless descriptor 切换。

### 2. 现有基础设施（可复用部分：meshoptimizer、DeferredPipeline、间接绘制等）

#### 2.1 能力清单（归属 + 实证）

下表逐行来自旧设计文档 §1；"归属"列标明该能力在本项目中的使用方。**同时服务 Lumen 与 Nanite
的行保留**，并注明两者各自的用途。

| 能力 | 状态 | 用途（源文档原文） | 归属 | 实证 |
|---|:---:|---|---|---|
| VK 1.3 + RT (AS + RT PSO + SBT) | ✅ | Lumen 远场 HW RT 追踪、Nanite BVH 遍历 | Lumen + Nanite | `Engine/RHI/Vulkan/VulkanDevice.cpp:195`（`apiVersion = VK_API_VERSION_1_3`）；`Engine/RHI/RHI/RayTracing.h:91` `IRHIAccelerationStructure`、`:135` `IRHIRayTracingPipelineState`；`Engine/RHI/RHI/Types.h:152` `ShaderBindingTable`；`Engine/RHI/Vulkan/VulkanRT.h:44,80`；`Engine/Render/Pipeline/RTPass.h:167,172,178`；caps `Engine/RHI/RHI/Types.h:292` |
| VK_EXT_mesh_shader | ✅ | Nanite Cluster 硬光栅 | Nanite | 启用：`Engine/RHI/Vulkan/VulkanDevice.cpp:460`（`meshFeature.meshShader = VK_TRUE`）、`:464`；管线：`Engine/RHI/Vulkan/VulkanPipeline.cpp:485-486`；RHI 描述：`Engine/RHI/RHI/MeshShader.h:18` `MeshPipelineStateDesc`、`Engine/RHI/RHI/Shader.h:64` `ShaderBytecode* meshShader`；已在用 shader：`Engine/Shader/Shaders/GBuffer/GBuffer.mesh.slang`、`Engine/Shader/Shaders/Utility/Triangle.mesh.slang` |
| GPU Culling (Hi-Z + Two-Phase + PTG) | ✅ | Nanite Instance/Cluster 剔除 | Nanite | `Engine/Render/Pipeline/GPUCulling.h:32`、`GPUCulling.cpp:434` `DispatchPhase1`、`:549` `DispatchPhase2`、`:478` `BuildHiZPyramid`、`:630` `InitializePTG`、`:701` `SignalPTG`；shader：`Engine/Shader/Shaders/Culling/{GPUCull,GPUCull_Phase1,GPUCull_TwoPhase,HiZDownsample,PersistentCull}.comp.slang` |
| VK_EXT_device_generated_commands | ✅ | Nanite 间接绘制生成 | Nanite | 启用：`Engine/RHI/Vulkan/VulkanDevice.cpp:503,508`；函数加载：`Engine/RHI/Vulkan/VulkanDevice_MeshShader.cpp:139-141`；封装：`Engine/RHI/Vulkan/VulkanDGC.h:14,25,27,39`、`VulkanDGC.cpp`；执行入口：`Engine/RHI/Vulkan/VulkanCommandList.cpp:397`；RHI 抽象：`Engine/RHI/RHI/CommandList.h:124` |
| GPU WorkGraph (软件模拟) | ✅ | Nanite 剔除链 → Draw 链 | Nanite | `Engine/Render/Pipeline/GPUWorkGraph.h:9`（自述"软件模拟框架"）、`GPUWorkGraph.cpp:71` `AddNode`、`:294` `Execute`；默认 Entry shader `Engine/Shader/Shaders/Utility/WorkGraph_Entry.comp.slang`；caps `Engine/RHI/RHI/Types.h:295` |
| Bindless Textures | ✅ | Surface Cache Atalas、Nanite 材质 | Lumen（Surface Cache Atalas）+ Nanite（材质） | `Engine/RHI/RHI/Bindless.h:22` `IRHIBindlessHeap::RegisterTexture`；`Engine/RHI/RHI/Types.h:25` `kDescSetMaterial`、`:30-32` `kBindingBindlessTextures/Samplers/SSBO` |
| AsyncCompute | ✅ | SDF 更新、Surface Cache 更新 | **Lumen** | `Engine/Render/RenderGraph.cpp:447,500` `ExecuteWithAsyncCompute`；`Engine/Render/Pipeline/DeferredPipeline.cpp:610`（`HasAsyncComputeQueue()`）；caps `Engine/RHI/RHI/Types.h:303` |
| DDGI (探针 GI) | ✅ | 升级为 Radiance Cache | **Lumen** | `Engine/Render/GI/DDGIProvider.h:21`、`Engine/Render/GI/DDGITracePass.h`、`Engine/Render/GI/GI_DDGI.{h,cpp}` |
| GBuffer DeferredPipeline | ✅ | Nanite Phase 1 写入目标 | Nanite | `Engine/Render/Pipeline/DeferredPipeline.{h,cpp}`、`DeferredPipeline_FrameGraph.cpp:44` `BuildFrameGraph`；GBuffer 附件常量 `Engine/Render/Pipeline/GBufferRenderer.h:16-26` |
| ClusteredShading LightGrid | ✅ | Lumen 命中点直接光照 | **Lumen** | `Engine/Shader/Shaders/Lighting/DeferredLighting.frag.slang:67-69`；`Engine/RHI/RHI/Types.h:33` `kBindingLightGrid = 7`；`Engine/Shader/Shaders/ShaderTypes.slang:51` |
| Denoiser (5×5 双边) | ✅ | Screen Probe Gather 空间滤波；统一降噪框架属 Lumen（见 `Lumen设计与实现.md`） | **Lumen** | `Engine/Render/PostProcess/Denoiser.h:10,17`（"5×5 双边模糊降噪"）、`Engine/Render/PostProcess/RTDenoiser.h:22` |
| meshoptimizer | ✅ | Nanite 预处理 Cluster/LOD | Nanite（离线，尚未接线） | 源码 `Engine/External/meshoptimizer/`（v0.22，`CMakeLists.txt:15`），由 `Engine/External/CMakeLists.txt:56-57` `add_subdirectory(meshoptimizer)` 构建；关键 API `src/clusterizer.cpp:538` `meshopt_buildMeshlets`、`src/simplifier.cpp:2056` `meshopt_simplify` |
| VMA | ✅ | GPU 内存管理 | 共用 | `Engine/External/VulkanMemoryAllocator/vk_mem_alloc.h`；`Engine/RHI/Vulkan/VulkanDevice.cpp:690` `vmaCreateInfo.vulkanApiVersion`；`Engine/RHI/Vulkan/VulkanResources.cpp:127` `vmaCreateBuffer` |

#### 2.2 与 Nanite 直接相关的可复用集成点（实证）

| 集成点 | 现状 | 实证 |
|---|---|---|
| 间接绘制 | 已有 RHI 抽象 + 两级实现（传统 `vkCmdDrawIndexedIndirect` / DGC `vkCmdExecuteGeneratedCommandsEXT`） | `Engine/RHI/RHI/CommandList.h:104` `DrawIndexedIndirect`、`:124` `ExecuteGeneratedCommands`；`Engine/RHI/Vulkan/VulkanCommandList.cpp:386,397`；实际使用：`Engine/Render/Pipeline/GBufferRenderer_GPU.cpp:113,119,193`、`Engine/Render/Pipeline/ForwardPipeline.cpp:1173,1319`；命令结构 `Engine/Render/Pipeline/MeshBatcher.h:21`（20B，`VkDrawIndexedIndirectCommand`） |
| DeferredPipeline 帧图 | 已有 `GPU_Cull` / `GB_Clear` / `GBuffer` / `Lighting` / 后处理链 | `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp:44` `BuildFrameGraph`、`:119,134` `GPU_Cull`、`:214` `GB_Clear`、`:875` `Lighting`、`:1163` `TAA_Resolve`、`:1207` `ToneMap`；Nanite 的插入点就是 `GB_Clear`/GBuffer 所在的这一段 |
| GPUCulling | 已有单阶段 / 两阶段（Hi-Z）/ PTG 三种模式 | 见 §2.1 对应行 |
| GPUScene | 已有场景对象 SSBO（128B/对象） | `Engine/Render/Pipeline/GPUScene.h:26` `GPUSceneObject`、`:40` `static_assert(sizeof(GPUSceneObject) == 128)`、`GPUScene.cpp:52` `Collect`、`:115` `Upload` |
| InstanceCuller | 已有逐实例 GPU 视锥剔除（可见列表 + 间接命令） | `Engine/Render/Pipeline/InstanceCuller.h:60`、`InstanceCuller.cpp:120` `UploadInstanceTransforms`、`:159` `Cull`；接入点 `DeferredPipeline.h:175`、`DeferredPipeline_FrameGraph.cpp:224` `SetInstanceCuller`、`Engine/Render/Pipeline/ForwardPipeline.cpp:380,1120,1161` |
| Mesh Shader 管线创建路径 | 走 `PipelineStateDesc::meshShader`（不是 `MeshPipelineStateDesc`） | `Engine/RHI/RHI/Shader.h:64`；`Engine/RHI/RHI/RHI.h:49` `CreatePipelineState`（`Engine/RHI/RHI/MeshShader.h:18` 的 `MeshPipelineStateDesc` 已定义但未被 `CreatePipelineState` 使用）；`Engine/Render/Pipeline/ForwardPipeline.cpp:388-391` 注释说明了该用法 |
| GBuffer 写入面 | **8** 个颜色附件（不是 5） | `Engine/Render/Pipeline/GBufferRenderer.h:16` `kGBufferAttachmentCount = 8`、`:17-26` 槽位定义；`GBufferRenderer_GPU.cpp:63` `BeginOffscreenPassMRT(cv, 8, ...)`；`DeferredPipeline.h:65` 的注释仍写"5×MRT"（陈旧注释） |
| CMake / Shader 构建约定 | 头文件列 `target_sources` + `huge_source_group()`；shader 逐个列进编译表 | `Engine/Render/CMakeLists.txt:184,187`、`Engine/Scene/CMakeLists.txt:88`、`Engine/Shader/CMakeLists.txt:336`；shader 编译列表 `Engine/Shader/CMakeLists.txt:125-127`、共享 include 列表 `:189` `SLANG_INCLUDES` |
| 组件与反射约定 | 组件用 `HE_COMPONENT()` 注册；`SceneReflect.cpp` 集中注册 | `Engine/Scene/Scene/Component.h:12,27`；`Engine/Scene/Scene/SceneReflect.cpp` 存在 |

**注：源文档此处不一致（#14）**：设计 §3.3 说"直接写现有 5×MRT GBuffer"，代码实际是 8 个附件。
合并时保留原句，实际实现应按 `kGBufferAttachmentCount` 与各槽位语义接线（MRT0 Albedo+Metallic、
MRT1 Normal+Roughness、MRT2 Emissive+AO、MRT3 Velocity、MRT4 WorldPos、MRT5/6 Disney、MRT7 LightmapKey）。

#### 2.3 Nanite 相关代码当前是否存在（结论：不存在，仅设计/计划）

| 检索关键词 | 范围 | 结果 |
|---|---|---|
| `Nanite` | `Engine/`（排除 `Engine/External`） | **0 命中** |
| `NaniteComponent\|NaniteRenderer\|NaniteUpload\|NaniteCulling\|NaniteSoftRaster\|NaniteCluster` | `*.{h,cpp,py,slang,comp,mesh,txt,cmake}` 全仓库 | **0 命中** |
| `Nanite`（含小写 `nanite`） | `Engine/Scene/` | **0 命中** |
| `\.nanite`（文件扩展名） | `*.{h,cpp,py,slang,comp,mesh,txt,cmake}` 全仓库 | **0 命中** |
| `nanite\|NANITE` | `Engine/` | 仅第三方：`Engine/External/meshoptimizer/{Makefile,CMakeLists.txt,demo/main.cpp,demo/nanite.cpp}`（meshoptimizer 自带的 Nanite 实验 demo，未接入引擎） |
| `meshopt_` | `Engine/` 排除 `Engine/External` | **0 命中** —— meshoptimizer 已 vendor 并构建成 target，但**引擎代码未调用**，且没有任何引擎 target `target_link_libraries(... meshoptimizer)` |
| `Tools/NanitePreprocess/` | `Tools/` | **不存在**（`Tools/` 现有 `gen_stbn.py`、`check_path_payload.py`、`gi/`、`pt/`） |
| `Engine/Render/Pipeline/Nanite*` | 目录 | **不存在** |
| `Engine/Shader/Shaders/Nanite/` | 目录 | **不存在** |
| `Engine/Scene/Scene/NaniteComponent.*` | 目录 | **不存在** |

**结论**：Nanite 在 HugEngine 中**只有设计与计划，没有任何实现**。设计里说的"现有基础设施"是
"存在的、可复用的通用设施"（GPU Culling / DGC / Bindless / DeferredPipeline / mesh 管线路径），
**不是**"已有 Nanite 代码"。另外两点需在实现时注意：

1. `meshoptimizer` 的 Python 绑定不来自 vendor 副本 —— 计划 Task 2 Step 1 的
   `pip install meshoptimizer numpy` 装的是外部 PyPI 包，与 `Engine/External/meshoptimizer` 的
   C++ 库是两套东西（C++ 侧当前也未被任何引擎 target 链接）。
2. `Engine/RHI/RHI/MeshShader.h:18` 的 `MeshPipelineStateDesc` 目前是**声明未接入**状态，
   N4（硬光栅）落地时要按 `PipelineStateDesc::meshShader` 的实际路径写，或先把该结构接进
   `CreatePipelineState`。

#### 2.4 Shader 命名与路径约定（实证）

| 项目 | 仓库实际 | 实证 |
|---|---|---|
| 扩展名 | `*.vert.slang` / `*.frag.slang` / `*.comp.slang` / `*.mesh.slang` / `*.rt.slang` | `Engine/Shader/CMakeLists.txt:125-127`（`Culling/GPUCull.comp.slang` 等）；`Engine/Shader/Shaders/**/*.comp` 单独 glob **0 文件** |
| 共享结构体 | 用 `.slang` include（列进 `SLANG_INCLUDES`），例如 `ShaderTypes.slang` | `Engine/Shader/CMakeLists.txt:189`、`Engine/Shader/Shaders/ShaderTypes.slang` |
| 目录 | 按用途分子目录（`GBuffer/`、`Lighting/`、`Culling/`、`Utility/`、`Particles/`…） | `Engine/Shader/Shaders/` |

**注：源文档此处不一致（#4）**：源文档写"Slang `.comp`/`.mesh` 命名规范"并给出 `.comp`/`.mesh`
文件名与 `slangc ... -o build/.../X.comp.spv` 命令；仓库实际是 `.comp.slang`/`.mesh.slang`。
§10、§11、§12 中源文档的原文一律保留，实现时按 §2.4 的实际约定命名。

### 3. 数据流

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

对应到实现计划的任务编号：预处理 = Task 2 / 3 / 4；上传 = Task 5；Instance Culling = Task 6；
Cluster Culling = Task 7；软光栅 = Task 8；帧图接线 = Task 9；回归 = Task 10（详见 §12）。

### 4. 预处理（Python 工具链：cluster 划分、LOD/DAG、量化、打包）

#### 4.1 设计层流程（源文档 §3.2 原文）

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

**注：源文档此处不一致（#13）**：本节的量化只声明"顶点量化 Q10.10.10.2"，而实现计划的
`NaniteVertex` 同时定义 `packedPosition` / `packedNormal` / `packedUV` 三个字段，但 Task 3 的
`quantize_vertices` **只实现 position 打包**；normal（R10G10B10A2_SNORM）与 UV（R16G16_UNORM）
的打包在实现计划中没有对应代码。实现 N1 时需补齐。

#### 4.2 模块划分与文件落点

设计层的 `NanitePreprocessor` 在实现计划里被拆成 6 个模块（详见 §11 与 §12）：

| 模块 | 职责 | 对应任务 |
|---|---|---|
| `NanitePreprocess.py` | 主入口：Mesh → `.nanite` | Task 2 Step 2 / Task 4 Step 2 |
| `NaniteCluster.py` | Cluster 划分（meshoptimizer） | Task 2 |
| `NaniteLOD.py` | 边折叠 LOD 生成 | Task 3 |
| `NaniteDAG.py` | DAG 去重 | Task 3 |
| `NaniteQuantize.py` | 顶点量化 R10G10B10A2 | Task 3 |
| `NanitePack.py` | `.nanite` 二进制打包 + 头部写入 | Task 4 |

**注：源文档此处不一致（#2）**：设计 §4 的"新 Shader 文件"清单把 `Nanite_Preprocess.py` 放在
`Engine/Shader/Shaders/Nanite/` 下；实现计划把它拆到 `Tools/NanitePreprocess/`。两处都保留；
按实现计划落地（Python 工具链独立于引擎运行时，见 §10）。

#### 4.3 工具链依赖

- Python 3.11+，`meshoptimizer` + `numpy`（§10 全局约束）。
- `meshopt_buildMeshlets` / `meshopt_simplify` 的 C++ 实现在
  `Engine/External/meshoptimizer/src/{clusterizer.cpp:538,simplifier.cpp:2056}`，但**引擎未链接**该
  target（见 §2.3），因此预处理脚本走 PyPI 包，与 C++ 库互不影响。

### 5. 运行时（C++：NaniteComponent、GPU 上传、剔除、光栅化）

运行时的四段职责与实现计划的对应关系：

| 职责 | 设计要点 | 实现计划落点 |
|---|---|---|
| NaniteComponent | 场景组件，持有 `.nanite` 资源路径 + GPU 数据句柄 + `instanceID` | Task 5 Step 1（§12） |
| GPU 上传 | 读 `.nanite` 文件头 + Cluster/顶点/索引 + 创建 SSBO | Task 5 Step 2（§12） |
| 剔除 | Instance Culling → Persistent Cluster Culling → LOD Selection | Task 6 / Task 7（§12） |
| 光栅化 | 软光栅写 GBuffer（N3）；硬光栅（N4，未在 N1-N3 范围） | Task 8（§12） |

#### 5.1 Cluster 剔除（两阶段）

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

**注：源文档此处不一致（#12）**：小标题写"两阶段"，正文列了 Phase 1/2/3（LOD Selection 为
第 3 段）。实现计划里 Task 6 = Phase 1、Task 7 = Phase 2，Phase 3（`Nanite_LODSelect.comp`）
属 N5，不在 N1-N3 范围（§9.1 的 N5 里程碑）。

**可复用的既有剔除设施**（实证见 §2.2）：本节的 Instance Culling 与既有 `InstanceCuller`
（逐实例视锥剔除 + 可见列表 + 间接命令）形状一致；Persistent Cluster Culling 的 BVH 遍历可
参照既有 `GPUCulling` 的两阶段（Phase 1 粗筛 → Hi-Z → Phase 2 精筛）与 PTG（Persistent Thread
Group）模式的接线方式。

#### 5.2 混合光栅化

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

**注：源文档此处不一致（#5）**：设计规定 `triCount > 16` 才走 Mesh Shader、`<= 16` 走软光栅；
实现计划的 N1-N3 **只做软光栅**，`Nanite_SoftRaster.comp` 对 cluster 的全部（最多 64 个）
三角形统一处理，没有 16 三角形阈值，`Nanite.mesh` 硬光栅属 N4。两处都保留。

#### 5.3 GBuffer 路线（Phase 1）

```
优势:
  - 复用现有 DeferredPipeline 全部后处理
  - 直接可以验证 Cluster 渲染正确性
  - 不需要改 Lighting Pass

做法:
  - Nanite 光栅化直接写现有 5×MRT GBuffer
  - Lighting Pass 照常从 GBuffer 采样
```

**注：源文档此处不一致（#14）**：GBuffer 实际是 8 个颜色附件（`kGBufferAttachmentCount = 8`，
`Engine/Render/Pipeline/GBufferRenderer.h:16`），不是 5 个。实现计划 Task 8 的软光栅绑定了
6 个写目标（A/B/C/Vel/WorldPos/Depth），与 8 附件的实际布局也不是一一对应。落地时以代码为准。

#### 5.4 Material Bin

```
按材质分组 cluster → 绑定 Bindless 纹理 array
一次 Draw/Dispatch 处理同一材质的多个 cluster
减少 bindless descriptor 切换
```

Material Bin 属 N6（§9.1），不在 N1-N3 范围。相关既有设施：Bindless 堆
（`Engine/RHI/RHI/Bindless.h:22`）与逐材质描述符集（`Engine/RHI/RHI/Types.h:25` `kDescSetMaterial`）。

### 6. 运行时 GPU 资源

| 资源 | 大小 (估算) | 说明 |
|------|-----------|------|
| Cluster Buffer (SSBO) | ~16MB per mesh | cluster bounds + BVH + indices |
| Vertex Buffer (SSBO) | ~32MB per mesh | 量化顶点 (R10G10B10A2) |
| Index Buffer (SSBO) | ~8MB per mesh | cluster 索引 |
| Indirection Buffer (SSBO) | ~512KB | 紧凑化后的可见 cluster 列表 |
| LOD Error Buffer (SSBO) | ~64KB | 每个 cluster 的最大几何误差 |

实现计划 Task 5 的 `NaniteGPUData` 把上述资源落成 4 个 `rhi::IRHIBuffer` 句柄 + 5 个计数/范围
字段（详见 §12 Task 5）：

| `NaniteGPUData` 字段 | 对应设计资源 |
|---|---|
| `clusterBuffer`（`NaniteCluster[]`） | Cluster Buffer (SSBO) |
| `vertexBuffer`（量化顶点） | Vertex Buffer (SSBO) |
| `indexBuffer`（三角形索引） | Index Buffer (SSBO) |
| `materialBuffer`（材质 bindless ID） | 设计表中未列，Materal Bin（N6）的前置 |
| `clusterCount` / `vertexCount` / `indexCount` / `materialCount` / `lodLevelCount` / `maxLODError` / `bboxMin` / `bboxMax` | 来自 `.nanite` 文件头（§8.3） |

**注**：设计表把 "Indirection Buffer" 与 "LOD Error Buffer" 列为独立资源；实现计划的
`NaniteGPUData` 里**没有**这两个 Buffer 字段（可见列表在 Task 6/7 的 `NaniteCulling` 内部
自建 `m_VisibleInstBuf` / `m_VisibleCountBuf` / `u_VisibleClusters`，误差字段只有文件头级的
`maxLODError`）。两处都保留：N2 需要自建紧致可见列表 Buffer，per-cluster 的 LOD Error Buffer
属 N5。

RHI 创建方式（实证，实现计划的骨架与之一致）：`rhi::BufferDesc` + `rhi::BufferUsage::Storage`
+ `device->CreateBuffer(desc)`（`Engine/RHI/RHI/Buffer.h:15-21`、`Engine/RHI/RHI/Types.h:147`、
`Engine/RHI/RHI/RHI.h:46`）；`Map()/Unmap()`（`Engine/RHI/RHI/Buffer.h:42-43`）。Vulkan 侧
`VulkanBuffer` 始终按 HOST_VISIBLE + 持久映射创建（`Engine/RHI/Vulkan/VulkanResources.cpp:121-135`），
`Map()` 会 invalidate、`Unmap()` 会 flush（`:172-193`），因此骨架里 `usage = Storage` 后直接
`Map()` 写数据是可行的；`BufferDesc::cpuAccess` 在 Vulkan 路径中未被读取。

### 7. 与现有管线集成（DeferredPipeline 扩展、新增 Shader 文件）

#### 7.1 DeferredPipeline 扩展

源文档给出的 `BuildFrameGraph` 增量（**保留原文**；方括号标注归属，Lumen 部分不在本文件展开）：

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

即：`[Lumen]` 那一行的 5 个 Pass（`SurfaceCache_Update`、`SDF_Update`、`ScreenProbeGather`、
`SpatialFilter`、`TemporalFilter`、`RadianceCache_Update`）与 `Lighting` 的 Lumen 输入**属 Lumen**；
`[Nanite (Phase 1)]` 那一行与共用的 `GPU_Cull` / `Lighting` / 后处理链属 Nanite。

代码侧的实际插入点（实证，见 §2.2）：`DeferredPipeline::BuildFrameGraph`
（`Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp:44`），Nanite 的剔除 + 光栅化 Pass
注册在 `GB_Clear`（`:214`）/ GBuffer 绘制（`:213` 注释处委托给 `IGBufferRenderer`）这一段之前，
`Lighting`（`:875`）之前完成。

实现计划 Task 9 的接线条件（原文）：

```
if (m_UseNanite)  // 新增 Nanite GBuffer 模式
    Nanite_InstanceCull → Nanite_ClusterCull → Nanite_SoftRaster → Lighting
else
    原有 GPU_Cull → GB_Clear → Lighting
```

延伸阅读（仍存在的文档）：`docs/已实现功能/DeferredPipeline实现规范.md`（GBuffer/帧图现状）、
`docs/HugEngine引擎介绍/HugEngine技术全景与实施计划.md`（总体计划）。

#### 7.2 新 Shader 文件（源文档 §4 原文，含 Lumen 条目）

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

`Lumen/` 下的 8 个条目**属 Lumen**（由 `Lumen设计与实现.md` 承接），此处保留只为呈现原始
目录规划；`Nanite/` 下的 6 个条目属 Nanite。

**注：源文档此处不一致**：
- #2：`Nanite_Preprocess.py` 在实现计划里移到 `Tools/NanitePreprocess/`。
- #3：软光栅文件名 `Nanite_SoftRasterize.comp`（设计）vs `Nanite_SoftRaster.comp`（计划）。
- #4：扩展名 `.comp`/`.mesh`（两文档）vs 仓库实际 `.comp.slang`/`.mesh.slang`（§2.4）。
- 实现计划的 File Structure 另列出设计清单里没有的 `NaniteTypes.slang`、`NaniteShared.slang`，
  并且**没有**创建 `Nanite.mesh`（属 N4）与 `Nanite_LODSelect.comp`（属 N5，仅出现在文件结构里，
  无对应 Task）。

### 8. 关键数据结构（Nanite Cluster GPU 布局等，逐字段）

#### 8.1 `NaniteCluster`（GPU）

源文档 §6 的结构体（原文）：

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

实现计划 Task 1 的结构体（原文，`Engine/Render/Pipeline/NaniteRenderer.h` 与
`Engine/Shader/Shaders/Nanite/NaniteTypes.slang` 需保持一致）：

```cpp
struct alignas(16) NaniteCluster {
    float4 boundingSphere;       // xyz=center, w=radius
    float4 coneAxisAngle;        // xyz=coneAxis, w=coneAngle(cos)
    u32    triangleOffset;       // index buffer 偏移 (三角形数)
    u32    triangleCount;        // 三角形数 (≤64)
    u32    vertexOffset;         // vertex buffer 偏移
    u32    materialID;           // bindless 材质 ID
    float  maxParentLODError;    // 父级 LOD 误差阈值
    u32    childClusterOffset;   // BVH 子节点起始索引 (0 表示叶子)
    u32    childCount;
    u32    _pad;
};
```

**定稿（任务 7，2026-09-20）**：上两块的字段名分歧取第二块（计划侧）的 `coneAxisAngle`
（xyz=单位轴，w=cos 锥半角）；第一块的 `coneData` 作废。两个"原文"块保留为历史引用，
**权威定义以本表 + `Engine/Render/Nanite/NaniteTypes.h` 的 `NaniteClusterRecord` /
`NaniteConeAxisAngle` 为准**（`static_assert` 钉住 64B / 16B 与逐字段偏移）。

**逐字段**（按 `alignas(16)` / std430 推算偏移；两边字段顺序完全一致，总计 64B，与
`NanitePack.py` 的 `clusterCount × 64B` 相符）：

| 偏移 | 字段 | 类型 | 含义（两边注释合并） | 差异 |
|---|---|---|---|---|
| 0 | `boundingSphere` | `float4` | xyz=center, w=radius（cluster 包围球） | 一致 |
| 16 | `coneAxisAngle` | `float4` | xyz=单位锥轴, w=cos(锥半角)；`w = -1` 是"无锥"哨兵（半角 180°，恒不可剔除，此时轴允许为 0） | 已裁决（原注 #1：设计写 `coneData`、计划写 `coneAxisAngle`）——**定稿（任务 7，2026-09-20）**：取 `coneAxisAngle`，理由：`coneData` 只有名字、没有字段语义（据此写不出 C++/Slang 一致的解码器），两者同为 float4 / 16B、尺寸与省法都不分高下 ⇒ 取"能唯一确定解码、不依赖外部约定"的那个（规则②的"自包含"意图）。与之冲突的旧名 `coneData` 作废 |
| 32 | `triangleOffset` | `u32` / `uint` | **单位是三角形**（不是索引个数）：字节偏移 = `triangleOffset × 8`（索引定稿为 8B/三角形，见 §8.5） | 一致（注释措辞差异）；**定稿（任务 7，2026-09-20）**：单位取**三角形**，因为 §8.5 的索引段按三角形打包 |
| 36 | `triangleCount` | `u32` | 三角形数（≤64） | 一致 |
| 40 | `vertexOffset` | `u32` | vertex buffer 中的偏移 | 一致 |
| 44 | `materialID` | `u32` | 指向 bindless 材质 / bindless 材质 ID | 一致 |
| 48 | `maxParentLODError` | `float` | 切换到父级 LOD 的误差阈值 | 一致 |
| 52 | `childClusterOffset` | `u32` | BVH 子节点索引 / 起始索引（`0` 表示叶子） | 一致（计划多"0 表示叶子"约定） |
| 56 | `childCount` | `u32` | BVH 子节点数 | 一致 |
| 60 | `_pad` | `u32` | 对齐填充 | 一致 |

#### 8.2 `NaniteInstance`（GPU，per-mesh-instance）

实现计划 Task 1 原文（设计文档 §6 未给出该结构，只有 Cluster）：

```cpp
// GPU 实例 (per-mesh-instance)
struct alignas(16) NaniteInstance {
    float4x4 worldMatrix;
    float4x4 normalMatrix;
    float4   boundsCenterRadius;  // xyz=center, w=radius (world space)
    u32      clusterBase;         // cluster buffer 中的起始索引
    u32      clusterCount;
    u32      vertexBase;
    u32      indexBase;
    u32      materialBase;        // bindless 材质起始索引
    u32      flags;               // bit0: visible
    float    lodScale;            // LOD 缩放因子
    float    _pad0;
};
```

| 偏移 | 字段 | 类型 | 含义 |
|---|---|---|---|
| 0 | `worldMatrix` | `float4x4` | 世界变换 |
| 64 | `normalMatrix` | `float4x4` | 法线矩阵 |
| 128 | `boundsCenterRadius` | `float4` | xyz=center, w=radius（world space） |
| 144 | `clusterBase` | `u32` | cluster buffer 中的起始索引 |
| 148 | `clusterCount` | `u32` | 该实例的 cluster 数 |
| 152 | `vertexBase` | `u32` | 顶点缓冲基址 |
| 156 | `indexBase` | `u32` | 索引缓冲基址 |
| 160 | `materialBase` | `u32` | bindless 材质起始索引 |
| 164 | `flags` | `u32` | bit0: visible |
| 168 | `lodScale` | `float` | LOD 缩放因子 |
| 172 | `_pad0` | `float` | 对齐填充（结构体合计 176B，按 alignas(16) 推算） |

slang 侧同名字段一份（`Engine/Shader/Shaders/Nanite/NaniteTypes.slang`，`uint` 对应 `u32`）。

#### 8.3 `NaniteFileHeader`（`.nanite` 文件头，C++ / Python 共享）

实现计划 Task 1 原文：

```cpp
// .nanite 文件头 (C++ / Python 共享)
struct NaniteFileHeader {
    char     magic[8];          // "NANITE01"
    u32      version;           // 1
    u32      clusterCount;
    u32      vertexCount;       // 量化后顶点数
    u32      indexCount;        // 三角形索引数
    u32      materialCount;
    u32      lodLevelCount;     // LOD 层数
    u32      flags;             // bit0: hasDAG
    float    bboxMin[3];
    float    bboxMax[3];
    float    maxLODError;       // 最大几何误差
    u32      _reserved[8];
};
```

| 偏移 | 字段 | 类型 | 含义 |
|---|---|---|---|
| 0 | `magic[8]` | `char[8]` | `"NANITE01"` |
| 8 | `version` | `u32` | `1` |
| 12 | `clusterCount` | `u32` | cluster 数 |
| 16 | `vertexCount` | `u32` | 量化后顶点数 |
| 20 | `indexCount` | `u32` | 三角形索引数 |
| 24 | `materialCount` | `u32` | 材质数 |
| 28 | `lodLevelCount` | `u32` | LOD 层数 |
| 32 | `flags` | `u32` | bit0: `hasDAG` |
| 36 | `bboxMin[3]` | `float[3]` | 包围盒最小角（量化范围下界） |
| 48 | `bboxMax[3]` | `float[3]` | 包围盒最大角（量化范围上界） |
| 60 | `maxLODError` | `float` | 最大几何误差 |
| 64 | `_reserved[8]` | `u32[8]` | 保留（**写 0**；段的偏移不落盘，见下） |
| — | 合计 | — | **96 字节**（定稿） |

**定稿（任务 7，2026-09-20）**：文件头取 **96B**（原注 #8 的 96B vs 128B 之争），理由：
① §8.3 已写"实现时以 96B 为准"（规则①）；② 96 = 6×16，天然 16B 对齐且自包含（规则②）；
③ `NanitePack.pack_nanite` docstring 里的 `[NaniteFileHeader 128B]` 是**笔误** —— 它实际写的是
`struct.pack('<32x')`（32B 保留区），按上表字段累加正好 96B。与之冲突的旧表述（128B）作废，
`§12 Task 4` 的 docstring 已同步改成 96B。

**段偏移不落盘**：`.nanite` 的文件布局 = 头部 + 5 个段，**每段的偏移与长度都是「头部计数 + 固定
步长」的纯函数**（落盘只会制造两份可以互相矛盾的真相）。定稿布局（每段起点 16B 对齐、每段长度
向上取整到 16B；文件尾允许有额外字节）：

| 序 | 段 | 条数 | 单条 | 定稿说明 |
|---|---|---|---|---|
| 0 | `NaniteFileHeader` | 1 | 96B | 本节的头部 |
| 1 | `NaniteClusterRecord[]` | `clusterCount` | 64B | = §8.1 的 GPU `NaniteCluster`（二进制同构） |
| 2 | `NaniteVertex[]` | `vertexCount` | **16B** | 量化顶点（含量化偏置，见 §8.4 定稿） |
| 3 | `NanitePackedTriangle[]` | `indexCount / 3` | **8B** | 3×u16 进 `u32[2]`（见 §8.5 定稿） |
| 4 | `NaniteMaterialRecord[]` | `materialCount` | 8B | bindless 纹理 ID 对（字段语义由任务 10/12 细化） |
| 5 | `u32[]` | `lodLevelCount` | 4B | 每个 LOD 一个偏移 |

C++ 侧的权威实现：`NaniteFileLayout` + `TryBuildNaniteFileLayout()` + `ValidateNaniteFile()`
（`Engine/Render/Nanite/NaniteTypes.h`，RHI-free、可单测），段表与上表逐项一致。

#### 8.4 量化顶点与打包格式

实现计划 Task 1 的 slang 定义（**已定稿的字段，任务 7 后以此为准**）：

```hlsl
// 量化顶点（16B：3×u32 位域 + 量化偏置）
struct NaniteVertex {
    uint packedPosition;   // R10G10B10A2_SNORM (xyz) + w=1
    uint packedNormal;     // R10G10B10A2_SNORM (xyz)
    uint packedUV;         // R16G16_UNORM (uv)
    int  quantBias;        // 量化偏置（默认 +512）：有符号量 = 10 位 raw - quantBias
};
```

| 偏移 | 字段 | 内容 |
|---|---|---|
| 0 | `packedPosition` | R10G10B10A2_SNORM（xyz）+ w=1 |
| 4 | `packedNormal` | R10G10B10A2_SNORM（xyz） |
| 8 | `packedUV` | R16G16_UNORM（uv） |
| 12 | `quantBias` | 量化偏置（默认 `+512`；**不是** `_pad`，见下方定稿） |

**定稿（任务 7，2026-09-20）**：
- **#7 步长取 16B**（原注：slang 结构体 16B vs `NanitePack.py` / `NaniteUpload.cpp` 的 12B）。
  理由：规则②优先"16 字节对齐且自包含"——量化偏置落在记录内（`quantBias`），解码不再依赖外部
  常量表；12B 版本既不 16B 对齐、也没有偏置的落点。旧表述（12B/顶点、
  `vertexCount * 3 * sizeof(u32)`、`vertexCount × 12B`）作废，`§12 Task 4` 已同步改成 16B。
- **#9 量化偏置在编码端补上 `+512`**（原注：`quantize_vertices` 产出无符号 `0…511`，而
  `decodeVertexPosition` 按 `int(raw) - 512` 的有符号 SNORM 解码，两边差一个偏置）。
  定稿：单轴 `raw = clamp(round((v - bboxMin) / maxExtent × 511)) + quantBias`，
  解码 `v = bboxMin + (raw - quantBias) / 511 × maxExtent`，**严格互逆**（误差 ≤ maxExtent/1022）。
  C++ 落点：`NaniteQuantizePositionAxis()` / `NaniteDequantizePositionAxis()`（含往返单测）。
- **`quantBias` 的语义**：10 位字段按**有符号** SNORM 解释（`raw - 512 ∈ [-512, 511]`）；
  盒内顶点编码后 `raw ∈ [512, 1023]`（`0…511` 属于盒下方，这正是旧无符号编码的 bug）。
  偏置写进每条顶点记录 ⇒ C++/Slang 双方都从记录里取值，不需要额外约定。
  **已知取舍（1 位精度）**：本节保留了既有解码式里的 `bboxMin` 基准（最小改动、与 §8.4 原文
  一致），因此盒内只用到 10 位有符号范围的上半段（512 级）。若要吃满 1024 级，可改成以盒
  **中心**为基准（`center = (bboxMin+bboxMax)/2`、`halfExtent = maxExtent/2`）——那需要同时改
  `bboxMin` 的语义与解码式，留给任务 10 按量化误差验收决定，本任务不动。
- 仍留给任务 10 的：#13 `packedNormal` / `packedUV` 的量化函数（本节只定稿它们的位域与偏移）。

打包/解包实现（Task 3 `quantize_vertices`、Task 8 `NaniteShared.slang`；**已按定稿 #9 修正**）：

```python
def quantize_vertices(vertices: np.ndarray, bbox_min: np.ndarray,
                      bbox_max: np.ndarray, bias: int = 512) -> np.ndarray:
    """
    将顶点量化到 R10G10B10A2_SNORM 空间。
    每轴 10 位有符号（[-512, 511]）；编码 raw = signed + bias（默认 512），
    与 decodeVertexPosition 的 `int(raw) - quantBias` 严格互逆（任务 7 裁决 #9）。
    精度 ~0.1%（半步 = maxExtent/1022）。
    """
    extent = bbox_max - bbox_min
    max_extent = np.max(extent)
    signed = np.rint((vertices - bbox_min) / max_extent * 511.0).astype(np.int32)
    signed = np.clip(signed, -512, 511)
    raw = (signed + bias).astype(np.uint32)          # 盒内 ⇒ raw ∈ [512, 1023]
    # 打包到 uint32: x[9:0] | y[19:10] | z[29:20] | w[31:30]=1
    packed = np.zeros(len(vertices), dtype=np.uint32)
    packed |= ((raw[:, 0] & 0x3FF))        # x bits 0-9
    packed |= ((raw[:, 1] & 0x3FF) << 10)  # y bits 10-19
    packed |= ((raw[:, 2] & 0x3FF) << 20)  # z bits 20-29
    packed |= (1 << 30)                    # w = 1（显式写入，解码不再"补"）
    return packed
```

```hlsl
// 量化顶点解码（与上面的编码互逆；quantBias 取自顶点记录的 quantBias 字段）
float3 decodeVertexPosition(uint packed, int quantBias, float3 bboxMin, float3 bboxMax) {
    float3 extent = bboxMax - bboxMin;
    float invScale = max(extent.x, max(extent.y, extent.z)) / 511.0;
    int3 raw = int3(packed & 0x3FF, (packed >> 10) & 0x3FF, (packed >> 20) & 0x3FF);
    float3 q = float3(raw) - float(quantBias);       // 有符号 SNORM 量化值 [-512, 511]
    float3 pos;
    pos.x = q.x * invScale + bboxMin.x;
    pos.y = q.y * invScale + bboxMin.y;
    pos.z = q.z * invScale + bboxMin.z;
    return pos;
}
```

#### 8.5 三角形索引编码

实现计划 Task 1 的注释（**已定稿的编码，任务 7 后以此为准**）：

```hlsl
// 三角形索引 (3×u16 打包到一个 u32[2] = 8B/三角形)
// indices[0].lo: i0 | (i1 << 16)
// indices[0].hi: i2 | (padding << 16)
```

**定稿（任务 7，2026-09-20）**：索引取 **3×u16 打包进 `u32[2]`（= 8B/三角形，
`NanitePackedTriangle`）**，不是"1 索引 1 个 u32"（12B/三角形）。理由：
① 两个候选都**不是** 16B 对齐 ⇒ 规则②不裁决，落到规则③"取更省方案"⇒ 8B < 12B，索引带宽 −33%；
② §8.4 的"每簇 ≤128 顶点"让簇内局部下标只需 7 位，u16 绰绰有余；③ 一簇 64 三角形 = 512B，
天然 16B 对齐。**索引语义同时定稿**：`i0/i1/i2` 是**簇内局部**顶点下标（合法区间 `[0, 127]`），
全局顶点下标 = `NaniteClusterRecord::vertexOffset + local`；`indexCount` 仍是**索引总数**
（必须是 3 的倍数），索引段字节数 = `ceil(indexCount / 3) × 8` 再向上取整到 16B。

与之冲突的旧表述作废：`indices (indexCount × 4B)`、`std::vector<u32> indices(header.indexCount)`、
`u_Indices[idxBase + 0/1/2]` 逐个 u32（§12 Task 4 的打包草图已同步改成按三角形打包）。
C++ 落点：`NanitePackedTriangle` + `NanitePackTriangle()` / `NaniteTriangleIndex0/1/2()` /
`IsValidClusterLocalVertexIndex()`（含位边界与语义边界单测）。

### 9. 里程碑与已知风险

#### 9.1 Nanite 里程碑

| 里程碑 | 内容 | 验证标准 |
|--------|------|----------|
| **N1: 预处理** | Python Cluster 划分 + LOD + 打包 | 输出 .nanite 文件 |
| **N2: 上传+剔除** | GPU Buffer + Instance/Cluster 剔除 | 可见 cluster 正确 |
| **N3: 软光栅** | Compute Shader 写入 GBuffer | Sponza 正确渲染 |
| **N4: 硬光栅** | Mesh Shader 处理大 cluster | 混合光栅性能提升 |
| **N5: LOD 流式** | 运行时 LOD 选择 + 反馈 | 帧率稳定，无 pop |
| **N6: 材质批次** | Material Bin + Bindless | 多材质场景无 Draw 爆炸 |

**注：源文档此处不一致（#15）**：上表覆盖 N1-N6；实现计划只覆盖 **N1-N3**（§12 / §13）。
两处都保留 —— N4-N6 目前只有设计里的里程碑，没有任务分解。

Lumen 的里程碑（L1-L6）不属本文件，见 `Lumen设计与实现.md`。

#### 9.2 建议推进顺序（源文档 §5.3 原文）

```
N1(预处理) → N2(剔除) → N3(软光栅 GBuffer) → L1(SDF) → L2(SurfaceCache)
→ L3(ScreenProbe) → N4(硬光栅) → L4(HW RT远场) → L5(RadianceCache)
→ 统一降噪框架（Lumen / GI 框架相关，见 docs/已实现功能/Lumen设计与实现.md）
→ L6+N5+N6(优化)
→ 框架前置（Provider 执行单位 / 绑定数组化）+ P6 统一估计器（长期；均为 Lumen 相关，见 Lumen设计与实现.md）
```

先跑通 Nanite 基本渲染（N1-N3），因为它产出 GBuffer 写入能力。然后基于 Nanite 的 GBuffer
上 Lumen（L1-L3）。

#### 9.3 已知风险

Nanite 相关风险（源文档 §7 原文）：

| 风险 | 缓解措施 |
|------|---------|
| 软件光栅化效率 | 仅对小 cluster (≤16 tri)，大的走 Mesh Shader |
| .nanite 格式版本兼容 | 文件头版本号 + semantic versioning |

源文档 §7 中另外 3 行**属 Lumen**，不在本文件展开（保留在此以便对账，内容见
`Lumen设计与实现.md`）：

| 风险（Lumen） | 缓解措施（Lumen） |
|------|---------|
| SDF 生成性能 | 预处理阶段完成 Mesh SDF，运行时只更新 Global SDF 注入 |
| Surface Cache Atalas 碎片化 | LRU + 定期整理 (defrag pass) |
| 两台追踪路径切换 discontinuity | SDF 最大距离结束前 N 步做 overlap fade |

**注**：§9.3 的两条 Nanite 风险里，"软件光栅化效率"的缓解措施与 §5.2 的软硬分流阈值一致
（`≤16 tri` 走软光栅）；但 N1-N3 只做软光栅、无阈值分流（见 #5），因此该缓解措施在 N1-N3
范围内**尚未生效**。

---

## 二、实现计划（N1-N3）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 实现 Nanite 虚拟几何从预处理到 GBuffer 渲染的完整路径

**Architecture:** N1(Python)预处理 → 生成.nanite(Cluster/LOD/DAG/量化) → N2(C++)GPU上传+两阶段剔除(Instance→Cluster) → N3(Compute Shader)软光栅化写入GBuffer

**Tech Stack:** Python 3.11+ / meshoptimizer / C++20 / Slang Compute / VK 1.3 / DeferredPipeline

**Spec:** `docs/计划实现功能/Nanite设计与实现.md`（本文件；原文指向旧《Lumen + Nanite 完整设计规范》，
见 §0.3）

### 10. 全局约束（Global Constraints）

- 所有新加代码添加中文注释
- Commit log 使用中文，不含 AI 信息
- Python 预处理脚本独立于引擎运行时，依赖仅 meshoptimizer + numpy
- C++ 运行时集成到 HugEngineRender 模块
- Shader 统一使用 Slang .comp/.mesh 命名规范
- 头文件列入 CMakeLists target_sources，使用 huge_source_group()

**注：源文档此处不一致（#4）**：第 5 条写的 `.comp`/`.mesh` 与仓库实际
`*.comp.slang`/`*.mesh.slang` 不符（实证见 §2.4）。第 6 条已在仓库落实：
`Engine/Render/CMakeLists.txt:184,187`、`Engine/Scene/CMakeLists.txt:88`、
`Engine/Shader/CMakeLists.txt:336`。

### 11. 文件结构（File Structure）

```
新增:
  Tools/NanitePreprocess/
    NanitePreprocess.py           # 主入口：Mesh → .nanite
    NaniteCluster.py              # Cluster 划分 (meshoptimizer)
    NaniteLOD.py                  # 边折叠 LOD 生成
    NaniteDAG.py                  # DAG 去重
    NaniteQuantize.py             # 顶点量化 R10G10B10A2
    NanitePack.py                 # .nanite 二进制打包 + 头部写入

  Engine/Scene/Scene/NaniteComponent.h   # NaniteMesh 场景组件
  Engine/Scene/Scene/NaniteComponent.cpp # 上传.nanite→GPU Buffer

  Engine/Render/Pipeline/
    NaniteRenderer.h              # 渲染器：剔除 + LOD + 光栅化调度
    NaniteRenderer.cpp
    NaniteUpload.h                # GPU 资源创建 (Cluster/VB/IB/Indirection Buffer)
    NaniteUpload.cpp
    NaniteCulling.h               # 两阶段剔除 (Instance + Cluster)
    NaniteCulling.cpp
    NaniteLODSelect.h             # LOD 选择
    NaniteLODSelect.cpp
    NaniteSoftRaster.h            # Compute 软光栅化到 GBuffer
    NaniteSoftRaster.cpp

  Engine/Shader/Shaders/Nanite/
    Nanite_InstanceCull.comp      # 视锥 + Hi-Z 实例剔除
    Nanite_ClusterCull.comp       # BVH 遍历 + Cluster 剔除
    Nanite_LODSelect.comp         # LOD 选择 (projected error)
    Nanite_SoftRaster.comp        # 计算着色器软件光栅化
    NaniteTypes.slang             # GPU 端 NaniteCluster 等共享结构体
    NaniteShared.slang            # 光栅化公用函数

修改:
  Engine/Render/Pipeline/DeferredPipeline.h       # 新增 Nanite GBuffer 模式
  Engine/Render/Pipeline/DeferredPipeline.cpp      # Nanite 路径集成
  Engine/Render/CMakeLists.txt                     # 新增源文件
  Engine/Shader/CMakeLists.txt                     # 新增 Nanite Shader
```

**注：源文档此处不一致（#2、#3、#4）** 与两点缺口：

- 软光栅 shader 名在设计 §7.2 是 `Nanite_SoftRasterize.comp`，此处是 `Nanite_SoftRaster.comp`。
- Python 落点在设计 §7.2 是 `Engine/Shader/Shaders/Nanite/`，此处是 `Tools/NanitePreprocess/`。
- 扩展名此处写 `.comp`，仓库实际为 `.comp.slang`（§2.4）。
- 本清单里的 `NaniteRenderer.*`、`NaniteLODSelect.*`、`Nanite_LODSelect.comp` 在
  Task 1-10 中**没有对应的创建步骤**：Task 1 只用 `NaniteRenderer.h` 放数据格式，
  Task 7 Step 2 只是"扩展 NaniteCulling C++ 端"。N3 结束时应重新核对文件清单。
- 设计 §7.2 里的 `Nanite.mesh`（硬光栅）不在本清单（属 N4）。

### 12. 任务清单（Task 1 … Task 10，逐个保留，含代码骨架、验收标准）

> 复选框语法（`- [ ]`）原样保留，用于逐任务跟踪。所有任务当前均为未开始状态。

---

#### Task 1: Nanite 数据格式定义

**Files:**
- Create: `Engine/Shader/Shaders/Nanite/NaniteTypes.slang`
- Create: `Engine/Render/Pipeline/NaniteRenderer.h` (数据格式部分)

**Interfaces:**
- Produces: `NaniteCluster` GPU 结构体, `NaniteInstance` GPU 结构体, `.nanite` 文件头

```cpp
// Engine/Render/Pipeline/NaniteRenderer.h

#pragma once
#include "RHI/RHI.h"
#include <vector>
#include <memory>

namespace he::render {

// ── GPU 端结构体 (与 NaniteTypes.slang 保持一致) ──
struct alignas(16) NaniteCluster {
    float4 boundingSphere;       // xyz=center, w=radius
    float4 coneAxisAngle;        // xyz=coneAxis, w=coneAngle(cos)
    u32    triangleOffset;       // index buffer 偏移 (三角形数)
    u32    triangleCount;        // 三角形数 (≤64)
    u32    vertexOffset;         // vertex buffer 偏移
    u32    materialID;           // bindless 材质 ID
    float  maxParentLODError;    // 父级 LOD 误差阈值
    u32    childClusterOffset;   // BVH 子节点起始索引 (0 表示叶子)
    u32    childCount;
    u32    _pad;
};

// GPU 实例 (per-mesh-instance)
struct alignas(16) NaniteInstance {
    float4x4 worldMatrix;
    float4x4 normalMatrix;
    float4   boundsCenterRadius;  // xyz=center, w=radius (world space)
    u32      clusterBase;         // cluster buffer 中的起始索引
    u32      clusterCount;
    u32      vertexBase;
    u32      indexBase;
    u32      materialBase;        // bindless 材质起始索引
    u32      flags;               // bit0: visible
    float    lodScale;            // LOD 缩放因子
    float    _pad0;
};

// .nanite 文件头 (C++ / Python 共享)
struct NaniteFileHeader {
    char     magic[8];          // "NANITE01"
    u32      version;           // 1
    u32      clusterCount;
    u32      vertexCount;       // 量化后顶点数
    u32      indexCount;        // 三角形索引数
    u32      materialCount;
    u32      lodLevelCount;     // LOD 层数
    u32      flags;             // bit0: hasDAG
    float    bboxMin[3];
    float    bboxMax[3];
    float    maxLODError;       // 最大几何误差
    u32      _reserved[8];
};

} // namespace he::render
```

```hlsl
// Engine/Shader/Shaders/Nanite/NaniteTypes.slang

// GPU 端 NaniteCluster — 与 C++ NaniteRenderer.h 保持一致
struct NaniteCluster {
    float4 boundingSphere;       // xyz=center, w=radius
    float4 coneAxisAngle;
    uint   triangleOffset;
    uint   triangleCount;
    uint   vertexOffset;
    uint   materialID;
    float  maxParentLODError;
    uint   childClusterOffset;
    uint   childCount;
    uint   _pad;
};

struct NaniteInstance {
    float4x4 worldMatrix;
    float4x4 normalMatrix;
    float4   boundsCenterRadius;
    uint     clusterBase;
    uint     clusterCount;
    uint     vertexBase;
    uint     indexBase;
    uint     materialBase;
    uint     flags;
    float    lodScale;
    float    _pad0;
};

// 量化顶点 (R10G10B10A2 + 量化范围)
struct NaniteVertex {
    uint packedPosition;   // R10G10B10A2_SNORM (xyz) + w=1
    uint packedNormal;     // R10G10B10A2_SNORM (xyz)
    uint packedUV;         // R16G16_UNORM (uv)
    uint _pad;
};

// 三角形索引 (3×u16 打包到一个 u32[2])
// indices[0]: i0 | (i1 << 16)
// indices[1]: i2 | (padding << 16)
```

- [ ] **Step 1: 创建头文件 + Shader 结构体**

创建 `Engine/Render/Pipeline/NaniteRenderer.h` 和 `Engine/Shader/Shaders/Nanite/NaniteTypes.slang`，包含上述全部代码。

- [ ] **Step 2: 编译验证**

```bash
cmake -B build && cmake --build build --target HugEngineRender --config Debug
```

预期：编译通过 (仅结构体定义，无链接依赖)。

> 落地提示（本文件补充，非源文档正文）：
> - 新增头文件需列入 `Engine/Render/CMakeLists.txt` 的 `RENDER_SOURCES`（`target_sources` +
>   `huge_source_group()`，见 §2.2 实证）。
> - `NaniteTypes.slang` 依仓库约定应命名为 `NaniteTypes.slang` 并列入
>   `Engine/Shader/CMakeLists.txt:189` 的 `SLANG_INCLUDES`（共享 include，不作独立编译单元）。
> - §8.1 的字段命名不一致（#1）必须在 Step 1 内一次性裁决，C++ 与 slang 同步。

---

#### Task 2: Python 预处理 — Cluster 划分

**Files:**
- Create: `Tools/NanitePreprocess/NanitePreprocess.py` (主入口骨架)
- Create: `Tools/NanitePreprocess/NaniteCluster.py`

**Interfaces:**
- Consumes: meshoptimizer `meshopt_buildMeshlets`
- Produces: `cluster_bounds: list[(center, radius, cone_axis, cone_angle)]`, `cluster_indices: list[list[u16]]`

```python
# Tools/NanitePreprocess/NaniteCluster.py

import meshoptimizer
import numpy as np

# 三角形索引 (glTF 兼容)
# indices: np.ndarray shape=(N,3) dtype=uint32
# vertices: np.ndarray shape=(V,3) dtype=float32

def build_clusters(indices: np.ndarray, vertices: np.ndarray,
                   max_vertices: int = 128, max_triangles: int = 64,
                   cone_weight: float = 0.5) -> list:
    """
    使用 meshoptimizer meshopt_buildMeshlets 将网格划分为 cluster。
    返回:
        clusters: list[dict] 每个 cluster 包含:
            - vertices: list[u32] 顶点索引列表
            - indices: list[u32] 三角形索引 (3×triCount)
            - bounds_center: float3
            - bounds_radius: float
            - cone_axis: float3 (法线锥轴)
            - cone_cutoff: float (法线锥角度 cos)
    """
    # 1. 调用 meshoptimizer 划分
    # meshopt_buildMeshlets 返回 meshlet 数组
    max_meshlets = 4096  # 初始容量
    meshlets = meshoptimizer.build_meshlets(
        indices, vertices, max_vertices, max_triangles, cone_weight
    )

    # 2. 计算每个 cluster 的包围球 + 法线锥
    clusters = []
    for m in meshlets:
        # 从 meshopt_Meshlet 提取数据
        vert_indices = list(m.vertices[:m.vertex_count])
        tri_indices = list(m.indices[:m.triangle_count * 3])

        # 计算包围球 (简单遍历)
        verts = vertices[vert_indices]
        center = verts.mean(axis=0)
        radius = np.max(np.linalg.norm(verts - center, axis=1))

        # 法线锥: meshopt 已计算 (m.cone_apex, m.cone_axis, m.cone_cutoff)
        cone_axis = np.array([m.cone_axis[0], m.cone_axis[1], m.cone_axis[2]])
        cone_cutoff = m.cone_cutoff

        clusters.append({
            'vertices': vert_indices,
            'indices': tri_indices,
            'bounds_center': center,
            'bounds_radius': float(radius),
            'cone_axis': cone_axis,
            'cone_cutoff': float(cone_cutoff),
            'triangle_count': m.triangle_count,
            'vertex_count': m.vertex_count,
        })

    return clusters
```

- [ ] **Step 1: 安装 meshoptimizer Python bindings**

```bash
pip install meshoptimizer numpy
```

- [ ] **Step 2: 编写 Cluster 划分代码**

写入 `Tools/NanitePreprocess/NaniteCluster.py` 和主入口 `NanitePreprocess.py`。

- [ ] **Step 3: 准备测试网格并验证**

```bash
python Tools/NanitePreprocess/NaniteCluster.py --input test_cube.obj
```

预期输出: cluster 数量 > 0，每个 cluster 顶点数 ≤ 128，三角形数 ≤ 64。

- [ ] **Step 4: 验证 Cluster 正确性**

在 Python 中重建 cluster 三角形，检查所有顶点索引合法。

> 落地提示（本文件补充，非源文档正文）：vendor 的 `Engine/External/meshoptimizer` 是 C++ 库、
> 不带 Python 绑定，且引擎未链接（§2.3）；Step 1 装的是 PyPI 包，与 C++ 副本相互独立。

---

#### Task 3: Python 预处理 — LOD 生成 + DAG + 量化

**Files:**
- Create: `Tools/NanitePreprocess/NaniteLOD.py`
- Create: `Tools/NanitePreprocess/NaniteDAG.py`
- Create: `Tools/NanitePreprocess/NaniteQuantize.py`

**Interfaces:**
- Consumes: Task 2 clusters, meshoptimizer `meshopt_simplify`
- Produces: LOD 层级 clusters, DAG 去重表, 量化顶点/索引缓冲

```python
# Tools/NanitePreprocess/NaniteLOD.py

def generate_lods(indices: np.ndarray, vertices: np.ndarray,
                  base_clusters: list, max_levels: int = 6) -> list[list]:
    """
    使用边折叠生成 LOD 层级。
    返回: lods[level] = clusters_at_level
    LOD0 = base_clusters (原始)
    LOD1 = 50% triangles
    LOD2 = 25% triangles
    ...
    """
    lods = [base_clusters]

    for level in range(1, max_levels):
        # 简化：目标三角形数约为上一级的 50%
        target_triangles = len(indices) // (2 ** level)
        if target_triangles < 64:
            break  # 已经足够简化，不再生成更多层级

        # meshopt_simplify
        simplified_indices = meshoptimizer.simplify(
            indices, vertices, target_triangles
        )

        # 对简化后的网格重新划分 cluster
        simplified_clusters = build_clusters(
            np.array(simplified_indices).reshape(-1, 3), vertices
        )
        lods.append(simplified_clusters)

    return lods


# Tools/NanitePreprocess/NaniteDAG.py

def build_dag(lods: list[list]) -> tuple[list, list[tuple[int, int]]]:
    """
    跨 LOD 层级去重相同的 cluster，构建 DAG。
    返回:
        unique_clusters: 去重后的所有 cluster (按 LOD 排列)
        dedup_map: [(lod, cluster_idx) → unique_idx] 映射表
    """
    cluster_hashes = {}  # hash → unique_idx
    unique_clusters = []
    dedup_map = []

    for lod_idx, lod in enumerate(lods):
        for cluster_idx, c in enumerate(lod):
            # 哈希 cluster 内容 (顶点 + 索引 + bounds)
            content = (
                tuple(sorted(c['vertices'])),
                tuple(sorted(c['indices'])),
                tuple(c['bounds_center']),
                c['bounds_radius'],
            )
            h = hash(content)

            if h in cluster_hashes:
                dedup_map.append((lod_idx, cluster_idx, cluster_hashes[h]))
            else:
                unique_idx = len(unique_clusters)
                cluster_hashes[h] = unique_idx
                unique_clusters.append({**c, 'lod': lod_idx, 'unique_id': unique_idx})
                dedup_map.append((lod_idx, cluster_idx, unique_idx))

    return unique_clusters, dedup_map


# Tools/NanitePreprocess/NaniteQuantize.py

def quantize_vertices(vertices: np.ndarray, bbox_min: np.ndarray,
                      bbox_max: np.ndarray, bits: int = 10) -> np.ndarray:
    """
    将顶点量化到 R10G10B10A2_SNORM 空间。
    bits=10: 每轴 [-512, 511] 范围，精度 ~0.1%
    """
    extent = bbox_max - bbox_min
    scale = (2 ** (bits - 1) - 1) / np.max(extent)
    quantized = ((vertices - bbox_min) * scale).astype(np.int32)
    # 打包到 uint32: x[9:0] | y[19:10] | z[29:20] | w[31:30]
    packed = np.zeros(len(vertices), dtype=np.uint32)
    packed |= ((quantized[:, 0].astype(np.uint32) & 0x3FF))       # x bits 0-9
    packed |= ((quantized[:, 1].astype(np.uint32) & 0x3FF) << 10) # y bits 10-19
    packed |= ((quantized[:, 2].astype(np.uint32) & 0x3FF) << 20) # z bits 20-29
    # w=1 (implicit, decode 时补)
    return packed
```

- [ ] **Step 1: 编写 LOD 生成代码**

写入 `NaniteLOD.py`。在 `NanitePreprocess.py` 主流程中串联。

- [ ] **Step 2: 编写 DAG 去重**

写入 `NaniteDAG.py`。使用哈希表去重 cluster。

- [ ] **Step 3: 编写顶点量化**

写入 `NaniteQuantize.py`。量化精度 10 bits per axis。

- [ ] **Step 4: 集成测试**

```bash
python Tools/NanitePreprocess/NanitePreprocess.py --input Content/gltf/Sponza/glTF/Sponza.gltf --output Sponza.nanite
```

预期: 输出 .nanite 文件，cluster 数量 > 0，LOD 层级 > 0，DAG 去重率 > 10%。

> 落地提示（本文件补充，非源文档正文）：
> - §8.1 的 LOD 级数表述（设计 `range(5)` vs 本任务 `max_levels=6` + `range(1, ...)`）两边都为
>   6 级；LOD1/LOD2 的 docstring 与 `len(indices)//(2**level)` 的算式一致（相对原始网格减半）。
> - §8.4 的量化偏置不对称（#9）**已由任务 7 定稿**（2026-09-20）：编码端 `raw = clamp(round(...)) + quantBias`
>   （默认 +512），与 `decodeVertexPosition` 的 `int(raw) - quantBias` 严格互逆 —— Step 3 只需按此实现，
>   不要再自行二选一。normal/UV 未打包（#13）仍需在 Step 3 内解决，否则 Task 8 的
>   `decodeVertexPosition/Normal/UV` 无法与编码对上（§8.4 已定稿它们的位域与偏移）。
> - 测试资材：`Content/gltf/Sponza/glTF/Sponza.gltf` 存在；Task 10 用的
>   `Content/gltf/Cube/Cube.gltf` 实际路径为 `Content/gltf/cube/cube.gltf`（大小写不一致，
>   Windows 下可工作）。

---

#### Task 4: Python 预处理 — .nanite 打包

**Files:**
- Create: `Tools/NanitePreprocess/NanitePack.py`

**Interfaces:**
- Consumes: Task 2 clusters + Task 3 LODs + DAG + quantized vertices
- Produces: `.nanite` 二进制文件

> **定稿（任务 7，2026-09-20）**：下面的打包草图已按 §8 的四处裁决改写 ——
> 头部 **96B**（不是 128B）、顶点 **16B**（含 `quantBias`，不是 12B）、
> 索引 **8B/三角形**（3×u16 进 `u32[2]`，不是 4B/索引）、cone 字段用 `cone_axis + cone_cutoff`
> （= §8.1 的 `coneAxisAngle`）。段偏移由计数推导、每段 16B 对齐，C++ 侧权威实现见
> `Engine/Render/Nanite/NaniteTypes.h` 的 `NaniteFileLayout` / `ValidateNaniteFile()`。

```python
# Tools/NanitePreprocess/NanitePack.py

import struct

def pack_nanite(output_path: str, header: dict, clusters: list,
                vertices: np.ndarray, indices: np.ndarray,
                materials: list, lod_offsets: list):
    """
    打包 .nanite 二进制文件（任务 7 定稿布局；每段起点 16B 对齐、长度向上取整到 16B）:
        [NaniteFileHeader 96B]
        [NaniteCluster[]      (clusterCount × 64B)]
        [quantized vertices[] (vertexCount × 16B：3×u32 位域 + int quantBias)]
        [indices[]            ((indexCount/3) × 8B：3×u16 打包进 u32[2]，簇内局部下标)]
        [materials[]          (materialCount × 8B, bindless IDs)]
        [LOD offsets[]        (lodLevelCount × 4B)]
    """
    with open(output_path, 'wb') as f:
        # Header
        f.write(b'NANITE01')
        f.write(struct.pack('<I', header['version']))
        f.write(struct.pack('<I', header['cluster_count']))
        f.write(struct.pack('<I', header['vertex_count']))
        f.write(struct.pack('<I', header['index_count']))
        f.write(struct.pack('<I', header['material_count']))
        f.write(struct.pack('<I', header['lod_level_count']))
        f.write(struct.pack('<I', header['flags']))
        f.write(struct.pack('<3f', *header['bbox_min']))
        f.write(struct.pack('<3f', *header['bbox_max']))
        f.write(struct.pack('<f', header['max_lod_error']))
        f.write(struct.pack('<32x'))  # reserved[8]（写 0；段偏移不落盘）

        # Clusters (64B each)
        for c in clusters:
            f.write(struct.pack('<4f', *c['bounds_center'], c['bounds_radius']))
            f.write(struct.pack('<4f', *c['cone_axis'], c['cone_cutoff']))  # = coneAxisAngle(xyz, w=cos)
            f.write(struct.pack('<4I', c['triangle_offset'], c['triangle_count'],
                                c['vertex_offset'], c['material_id']))
            f.write(struct.pack('<f', c['max_parent_lod_error']))
            f.write(struct.pack('<2I', c['child_cluster_offset'], c['child_count']))
            f.write(struct.pack('<I', 0))  # _pad

        # Quantized vertices (16B each: position(4B) + normal(4B) + uv(4B) + quantBias(4B))
        for v in vertices:
            f.write(struct.pack('<I', v['packed_position']))   # 10 位有符号字段已含 +bias
            f.write(struct.pack('<I', v['packed_normal']))
            f.write(struct.pack('<I', v['packed_uv']))
            f.write(struct.pack('<i', v.get('quant_bias', 512)))  # 解码用：signed = raw - quantBias

        # Indices (8B per triangle: u32[2]，i0 | (i1 << 16) 与 i2)
        tri = indices.reshape(-1, 3).astype('<u4')
        packed_lo = (tri[:, 0] & 0xFFFF) | ((tri[:, 1] & 0xFFFF) << 16)
        packed_hi = (tri[:, 2] & 0xFFFF)
        f.write(np.stack([packed_lo, packed_hi], axis=1).astype('<u4').tobytes())

        # Materials (8B each: bindless texture ID)
        for m in materials:
            f.write(struct.pack('<2I', m.get('albedo_tex', 0), m.get('normal_tex', 0)))

        # LOD offsets (4B each)
        for off in lod_offsets:
            f.write(struct.pack('<I', off))

    print(f"[NanitePack] {output_path}: {len(clusters)} clusters, "
          f"{len(vertices)} vertices, {header['lod_level_count']} LOD levels")
```

- [ ] **Step 1: 编写二进制打包**

写入 `NanitePack.py`。

- [ ] **Step 2: 端到端验证**

```bash
python Tools/NanitePreprocess/NanitePreprocess.py --input Sponza.gltf --output Sponza.nanite
python -c "
import struct
with open('Sponza.nanite', 'rb') as f:
    magic = f.read(8)
    version = struct.unpack('<I', f.read(4))[0]
    assert magic == b'NANITE01'
    assert version == 1
    print('.nanite header OK')
"
```

预期: magic="NANITE01", version=1。

> 落地提示（本文件补充，非源文档正文）：docstring 与读取口径的四处分歧已由**任务 7 定稿**
> （2026-09-20）：头部 **96B**（§8.3 / #8）、顶点 **16B**（§8.4 / #7，第 4 个 u32 = `quantBias`）、
> 索引 **8B/三角形**（3×u16 进 `u32[2]`，§8.5 / #6）、量化偏置**编码端补 `+512`**（§8.4 / #9）。
> 本文件 Task 5 的 `NaniteUpload.cpp` 读取口径请按此实现（96B 头 / 16B 顶点 / 8B 三角形），
> 不要再按 128B 头、12B 顶点、4B 索引读。C++ 权威定义与校验函数见
> `Engine/Render/Nanite/NaniteTypes.h`（`static_assert` 钉住布局，`ValidateNaniteHeader()` 校验文件）。

---

#### Task 5: C++ 运行时 — NaniteComponent + GPU 上传

**Files:**
- Create: `Engine/Scene/Scene/NaniteComponent.h`
- Create: `Engine/Scene/Scene/NaniteComponent.cpp`
- Create: `Engine/Render/Pipeline/NaniteUpload.h`
- Create: `Engine/Render/Pipeline/NaniteUpload.cpp`

**Interfaces:**
- Consumes: Task 1 data structures, RHI buffer creation
- Produces: `NaniteComponent` (存放 GPU buffers + instance data)
- Produces: `NaniteUpload::UploadNaniteFile(path) → NaniteGPUData`

```cpp
// Engine/Scene/Scene/NaniteComponent.h
#pragma once
#include "Scene/Component.h"
#include "Scene/Transform.h"
#include "Render/Pipeline/NaniteRenderer.h"
#include <memory>
#include <string>

namespace he {

class NaniteComponent : public Component {
    HE_COMPONENT()
public:
    NaniteComponent() = default;

    // 关联的 .nanite 资源路径
    std::string naniteAsset;
    // GPU 数据句柄 (上传后的 cluster/vertex/index buffers)
    bool gpuReady = false;
    u32    instanceID = ~0u;  // GPU Instance 数组中的索引
};

} // namespace he


// Engine/Render/Pipeline/NaniteUpload.h
#pragma once
#include "Pipeline/NaniteRenderer.h"
#include "RHI/RHI.h"
#include <vector>

namespace he::render {

// 上传 .nanite 文件到 GPU 的返回数据
struct NaniteGPUData {
    std::unique_ptr<rhi::IRHIBuffer> clusterBuffer;   // NaniteCluster[]
    std::unique_ptr<rhi::IRHIBuffer> vertexBuffer;    // 量化顶点
    std::unique_ptr<rhi::IRHIBuffer> indexBuffer;     // 三角形索引
    std::unique_ptr<rhi::IRHIBuffer> materialBuffer;  // 材质 bindless ID
    u32 clusterCount;
    u32 vertexCount;
    u32 indexCount;
    u32 materialCount;
    u32 lodLevelCount;
    float maxLODError;
    float3 bboxMin, bboxMax;
};

// 从 .nanite 文件加载并上传到 GPU
class NaniteUpload {
public:
    static NaniteGPUData UploadNaniteFile(
        rhi::IRHIDevice* device,
        const std::string& filePath
    );
};

} // namespace he::render
```

```cpp
// Engine/Render/Pipeline/NaniteUpload.cpp

NaniteGPUData NaniteUpload::UploadNaniteFile(
    rhi::IRHIDevice* device, const std::string& filePath)
{
    // 1. 读取 .nanite 文件
    std::ifstream file(filePath, std::ios::binary);
    HE_ASSERT(file.is_open(), "NaniteUpload: 无法打开文件");

    NaniteFileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    HE_ASSERT(std::strncmp(header.magic, "NANITE01", 8) == 0,
              "NaniteUpload: 无效的 .nanite 文件头");

    // 2. 读取 Cluster 数据
    std::vector<NaniteCluster> clusters(header.clusterCount);
    file.read(reinterpret_cast<char*>(clusters.data()),
              header.clusterCount * sizeof(NaniteCluster));

    // 3. 读取顶点 + 索引
    std::vector<u32> vertices(header.vertexCount * 3);  // 3 u32 per vertex
    file.read(reinterpret_cast<char*>(vertices.data()),
              header.vertexCount * 3 * sizeof(u32));

    std::vector<u32> indices(header.indexCount);
    file.read(reinterpret_cast<char*>(indices.data()),
              header.indexCount * sizeof(u32));

    // 4. 创建 GPU Buffers
    NaniteGPUData data;
    data.clusterCount = header.clusterCount;
    data.vertexCount  = header.vertexCount;
    data.indexCount   = header.indexCount;
    data.lodLevelCount = header.lodLevelCount;
    data.maxLODError  = header.maxLODError;

    // Cluster Buffer
    rhi::BufferDesc clusterDesc;
    clusterDesc.size = sizeof(NaniteCluster) * header.clusterCount;
    clusterDesc.usage = rhi::BufferUsage::Storage;
    data.clusterBuffer = device->CreateBuffer(clusterDesc);
    std::memcpy(data.clusterBuffer->Map(), clusters.data(), clusterDesc.size);
    data.clusterBuffer->Unmap();

    // Vertex Buffer
    rhi::BufferDesc vtxDesc;
    vtxDesc.size = header.vertexCount * 3 * sizeof(u32);
    vtxDesc.usage = rhi::BufferUsage::Storage;
    data.vertexBuffer = device->CreateBuffer(vtxDesc);
    std::memcpy(data.vertexBuffer->Map(), vertices.data(), vtxDesc.size);
    data.vertexBuffer->Unmap();

    // Index Buffer
    rhi::BufferDesc idxDesc;
    idxDesc.size = header.indexCount * sizeof(u32);
    idxDesc.usage = rhi::BufferUsage::Storage;
    data.indexBuffer = device->CreateBuffer(idxDesc);
    std::memcpy(data.indexBuffer->Map(), indices.data(), idxDesc.size);
    data.indexBuffer->Unmap();

    HE_CORE_INFO("NaniteUpload: {} clusters, {} vertices, {} indices",
                 header.clusterCount, header.vertexCount, header.indexCount);
    return data;
}
```

- [ ] **Step 1: 创建 NaniteComponent**

写入 `NaniteComponent.h/cpp`。添加反射注册到 `SceneReflect.cpp`。

- [ ] **Step 2: 创建 NaniteUpload**

写入 `NaniteUpload.h/cpp`。实现二进制读取 + GPU buffer 创建。

- [ ] **Step 3: 编译验证**

```bash
cmake --build build --target HugEngineRender --config Debug
```

预期: 编译通过，NaniteUpload 可被调用。

> 落地提示（本文件补充，非源文档正文）：
> - `NaniteComponent` 属 `HugEngineScene`，`NaniteUpload` 属 `HugEngineRender`；后者已经
>   `target_link_libraries(... PUBLIC HugEngineScene)`（`Engine/Render/CMakeLists.txt:195`），
>   因此 `NaniteComponent.h` include `Render/Pipeline/NaniteRenderer.h` 会形成**反向依赖**
>   （Scene → Render），需要在实现时把数据格式头下移或前向声明。
> - 反射注册：组件用 `HE_COMPONENT()`（`Engine/Scene/Scene/Component.h:12,27`），集中注册在
>   `Engine/Scene/Scene/SceneReflect.cpp`。
> - RHI API 一致：`rhi::BufferDesc` / `rhi::BufferUsage::Storage` / `CreateBuffer` /
>   `Map`/`Unmap` 均存在（实证见 §6 末段），骨架无需改写。
> - `header.maxLODError` / `header.lodLevelCount` 的字段名与 Task 1 的 `NaniteFileHeader`
>   （`maxLODError` / `lodLevelCount`）一致，但 Task 4 的 Python 侧用的是
>   `max_lod_error` / `lod_level_count`（snake_case）——字典键与 C++ 字段名不同属正常，
>   需保证 `header` dict 的键在 `NanitePreprocess.py` 里正确填充。

---

#### Task 6: GPU 剔除 — Instance Culling

**Files:**
- Create: `Engine/Render/Pipeline/NaniteCulling.h`
- Create: `Engine/Render/Pipeline/NaniteCulling.cpp`
- Create: `Engine/Shader/Shaders/Nanite/Nanite_InstanceCull.comp`

**Interfaces:**
- Consumes: NaniteInstance[] SSBO, camera VP matrix
- Produces: visible instance bitmask / compact list

```hlsl
// Engine/Shader/Shaders/Nanite/Nanite_InstanceCull.comp
#include "NaniteTypes.slang"

[[vk::binding(0, 0)]] StructuredBuffer<NaniteInstance> u_Instances;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> u_VisibleInstances;  // compact output
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> u_VisibleCount;      // atomic counter

struct CullParams {
    float4x4 viewProj;
    float4   frustumPlanes[6];  // 视锥 6 平面 (normal.xyz, distance)
    uint     instanceCount;
    float    nearPlane;
    float    farPlane;
    uint     _pad;
};
[[vk::push_constant]] CullParams u_Params;

// 视锥 AABB 测试
bool frustumCull(float3 center, float radius, float4 planes[6]) {
    [unroll]
    for (int i = 0; i < 6; i++) {
        float dist = dot(planes[i].xyz, center) + planes[i].w;
        if (dist < -radius) return false;  // 所有 8 个角都在平面外
    }
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= u_Params.instanceCount) return;

    NaniteInstance inst = u_Instances[tid.x];
    float3 center = inst.boundsCenterRadius.xyz;
    float  radius = inst.boundsCenterRadius.w;

    // Phase 1: 视锥剔除
    if (!frustumCull(center, radius, u_Params.frustumPlanes)) return;

    // Phase 2: Hi-Z 遮挡剔除 (后续 Task 添加)
    // 对齐到下一阶段

    // Compact 写入
    uint slot;
    InterlockedAdd(u_VisibleCount[0], 1, slot);
    u_VisibleInstances[slot] = tid.x;  // 原始 instance 索引
}
```

```cpp
// Engine/Render/Pipeline/NaniteCulling.h
#pragma once
#include "RHI/RHI.h"
#include "Pipeline/NaniteRenderer.h"
#include <memory>

namespace he::render {

class NaniteCulling {
public:
    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown();

    // 设置实例数据 (每帧调用)
    void SetInstances(rhi::IRHIDevice* device,
                      const std::vector<NaniteInstance>& instances);

    // Instance Culling (Compute dispatch)
    void DispatchInstanceCull(rhi::IRHICommandList* cmd,
                              const float4x4& viewProj,
                              u32 instanceCount);

    // Cluster Culling (phase 2, 后续 Task)
    void DispatchClusterCull(rhi::IRHICommandList* cmd);

    rhi::IRHIBuffer* GetInstanceBuffer()    const { return m_InstanceBuf.get(); }
    rhi::IRHIBuffer* GetVisibleInstances()  const { return m_VisibleInstBuf.get(); }
    rhi::IRHIBuffer* GetVisibleCount()      const { return m_VisibleCountBuf.get(); }

private:
    bool m_Initialized = false;

    std::unique_ptr<rhi::IRHIBuffer> m_InstanceBuf;
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleInstBuf;
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleCountBuf;

    std::unique_ptr<rhi::IRHIPipelineState> m_InstanceCullPSO;
    rhi::DescriptorSetLayoutHandle m_InstanceCullLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_InstanceCullSet    = rhi::kInvalidSet;

    rhi::ShaderBytecode m_InstanceCullCS;
};

} // namespace he::render
```

- [ ] **Step 1: 编写 Instance Cull Compute Shader**

写入 `Nanite_InstanceCull.comp`。

- [ ] **Step 2: 编写 C++ 端 NaniteCulling**

写入 `NaniteCulling.h/cpp`。创建 PSO + DescriptorSet + Buffer。

- [ ] **Step 3: 编译 Shader + 编译 C++**

```bash
slangc Engine/Shader/Shaders/Nanite/Nanite_InstanceCull.comp -target spirv -entry main -stage compute -I Engine/Shader/Shaders/ -o build/Engine/Shader/Shaders/Nanite_InstanceCull.comp.spv
cmake --build build --target HugEngineRender --config Debug
```

> 落地提示（本文件补充，非源文档正文）：
> - 仓库既有 shader 扩展名是 `.comp.slang`，且编译由 `Engine/Shader/CMakeLists.txt` 的显式列表
>   + `add_custom_command` 驱动（`:125-127`、`:304-317`），不是手敲 `slangc -o`；Step 3 的
>   命令需按仓库约定改为把文件列入 `COMP_SLANG` 列表。
> - `rhi::DescriptorSetLayoutHandle` / `kInvalidLayout` / `kInvalidSet` 均存在
>   （`Engine/RHI/RHI/Types.h:366-369`），骨架可用。
> - 可见列表 + 原子计数的形状与既有 `InstanceCuller`（`Engine/Render/Pipeline/InstanceCuller.cpp:159`
>   `Cull`、`InstancedCull.comp.slang`）一致，可直接对照其描述符集布局与屏障写法。

---

#### Task 7: GPU 剔除 — Cluster Culling + BVH 遍历

**Files:**
- Create: `Engine/Shader/Shaders/Nanite/Nanite_ClusterCull.comp`

**Interfaces:**
- Consumes: visible instances (Task 6 output), NaniteCluster[] buffer
- Produces: compact cluster list + draw commands

```hlsl
// Engine/Shader/Shaders/Nanite/Nanite_ClusterCull.comp
#include "NaniteTypes.slang"

[[vk::binding(0, 0)]] StructuredBuffer<NaniteInstance> u_Instances;
[[vk::binding(1, 0)]] StructuredBuffer<NaniteCluster>  u_Clusters;
[[vk::binding(2, 0)]] StructuredBuffer<uint> u_VisibleInstances;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> u_VisibleClusters;  // compact
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> u_VisibleClusterCount;

struct ClusterCullParams {
    float4x4 viewProj;
    float4   frustumPlanes[6];
    uint     instanceCount;
    uint     totalClusterCount;
    uint     _pad0;
    uint     _pad1;
};
[[vk::push_constant]] ClusterCullParams u_Params;

// BVH 深度优先遍历 + 视锥剔除
void traverseBVH(uint instanceIdx, uint clusterBase,
                 float4 frustumPlanes[6], float4x4 viewProj) {
    // 栈式 BVH 遍历 (最大深度 16)
    uint stack[16];
    uint stackPtr = 0;
    stack[stackPtr++] = clusterBase;  // 根 cluster

    while (stackPtr > 0) {
        uint clusterIdx = stack[--stackPtr];
        NaniteCluster c = u_Clusters[clusterIdx];

        // 视锥剔除 cluster bounds
        if (!frustumCull(c.boundingSphere.xyz, c.boundingSphere.w, frustumPlanes))
            continue;

        // 法线锥背面剔除
        float3 viewDir = normalize(c.boundingSphere.xyz - /*camPos*/ float3(0,0,0));
        if (dot(viewDir, c.coneAxisAngle.xyz) < c.coneAxisAngle.w)
            continue;

        // 叶子节点: 输出可见 cluster
        if (c.childCount == 0) {
            uint slot;
            InterlockedAdd(u_VisibleClusterCount[0], 1, slot);
            u_VisibleClusters[slot] = clusterIdx;
            continue;
        }

        // 内部节点: 压栈子节点
        for (uint i = 0; i < c.childCount && stackPtr < 16; i++) {
            stack[stackPtr++] = c.childClusterOffset + i;
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= u_Params.instanceCount) return;

    uint instanceIdx = u_VisibleInstances[tid.x];  // 从 Phase 1 输出读取
    NaniteInstance inst = u_Instances[instanceIdx];

    // 从根 cluster 开始 BVH 遍历
    traverseBVH(instanceIdx, inst.clusterBase,
                 u_Params.frustumPlanes, u_Params.viewProj);
}
```

- [ ] **Step 1: 编写 Cluster Cull Shader**

写入 `Nanite_ClusterCull.comp`。

- [ ] **Step 2: 扩展 NaniteCulling C++ 端**

添加 Cluster Cull PSO + Dispatch 方法。

- [ ] **Step 3: 编译验证**

> 落地提示（本文件补充，非源文档正文）：
> - 本 shader 使用了 `frustumCull(...)`，但只 `#include "NaniteTypes.slang"`；该函数定义在
>   Task 6 的 `Nanite_InstanceCull.comp` 里，**不在** `NaniteTypes.slang`。共享函数应放进
>   `NaniteShared.slang`（Task 8 创建）并在此 include。
> - `traverseBVH` 里的相机位置是占位（`/*camPos*/ float3(0,0,0)`），需补进 push constant。
> - 设计 §5.1 的 Phase 2 还要求"Hi-Z occlusion cull (sample Hi-Z pyramid)"与
>   "→ Indirection Buffer"；本 shader 只做了视锥 + 法线锥剔除与紧致化，Hi-Z 与间接绘制命令
>   生成未在本任务给出。既有 Hi-Z 金字塔可复用 `GPUCulling::BuildHiZPyramid`
>   （`Engine/Render/Pipeline/GPUCulling.cpp:478`）。

---

#### Task 8: Compute Shader 软光栅化 — GBuffer 写入

**Files:**
- Create: `Engine/Shader/Shaders/Nanite/Nanite_SoftRaster.comp`
- Create: `Engine/Shader/Shaders/Nanite/NaniteShared.slang`
- Create: `Engine/Render/Pipeline/NaniteSoftRaster.h`
- Create: `Engine/Render/Pipeline/NaniteSoftRaster.cpp`

**Interfaces:**
- Consumes: visible clusters, vertex/index buffers, GBuffer MRT
- Produces: GBuffer (Albedo/Normal/Emissive/Velocity/WorldPos/Depth)

```hlsl
// Engine/Shader/Shaders/Nanite/NaniteShared.slang

// 量化顶点解码
float3 decodeVertexPosition(uint packed, float3 bboxMin, float3 bboxMax) {
    float3 extent = bboxMax - bboxMin;
    float invScale = max(extent.x, max(extent.y, extent.z)) / 511.0;
    float3 pos;
    pos.x = float(int(packed & 0x3FF) - 512) * invScale + bboxMin.x;
    pos.y = float(int((packed >> 10) & 0x3FF) - 512) * invScale + bboxMin.y;
    pos.z = float(int((packed >> 20) & 0x3FF) - 512) * invScale + bboxMin.z;
    return pos;
}

float3 decodeVertexNormal(uint packed) {
    float3 n;
    n.x = float(int(packed & 0x3FF) - 512) / 511.0;
    n.y = float(int((packed >> 10) & 0x3FF) - 512) / 511.0;
    n.z = float(int((packed >> 20) & 0x3FF) - 512) / 511.0;
    return normalize(n);
}

float2 decodeVertexUV(uint packed) {
    return float2(
        float(packed & 0xFFFF) / 65535.0,
        float((packed >> 16) & 0xFFFF) / 65535.0
    );
}
```

```hlsl
// Engine/Shader/Shaders/Nanite/Nanite_SoftRaster.comp
#include "NaniteTypes.slang"
#include "NaniteShared.slang"

[[vk::binding(0, 0)]] StructuredBuffer<NaniteCluster> u_Clusters;
[[vk::binding(1, 0)]] StructuredBuffer<uint> u_Vertices;      // 3 u32 per vertex
[[vk::binding(2, 0)]] StructuredBuffer<uint> u_Indices;       // packed u16×3 per tri

[[vk::binding(3, 0)]] RWTexture2D<float4> u_GBufferA;  // Albedo+Metallic
[[vk::binding(4, 0)]] RWTexture2D<float4> u_GBufferB;  // Normal+Roughness
[[vk::binding(5, 0)]] RWTexture2D<float4> u_GBufferC;  // Emissive+AO
[[vk::binding(6, 0)]] RWTexture2D<float2> u_GBufferVel;
[[vk::binding(7, 0)]] RWTexture2D<float4> u_GBufferWorldPos;
[[vk::binding(8, 0)]] RWTexture2D<float>  u_GBufferDepth;

struct RasterParams {
    float4x4 viewProj;
    float4x4 prevViewProj;
    float3   bboxMin;
    float3   bboxMax;
    uint     clusterCount;
    float2   screenSize;
    uint     materialID;  // bindless base
    uint     _pad;
};
[[vk::push_constant]] RasterParams u_Params;

// 三角形光栅化 (每个 cluster 64 线程, 每线程处理 1 个三角形)
// 使用 atomicMin 写深度进行 interlock
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= u_Params.clusterCount) return;

    NaniteCluster c = u_Clusters[tid.x];
    uint materialID = c.materialID + u_Params.materialBase;

    // 遍历 cluster 内三角形
    for (uint t = 0; t < c.triangleCount; t++) {
        uint idxBase = (c.triangleOffset + t) * 3;
        uint i0 = u_Indices[idxBase];
        uint i1 = u_Indices[idxBase + 1];
        uint i2 = u_Indices[idxBase + 2];

        // 解码顶点
        uint vBase = (c.vertexOffset + i0) * 3;
        float3 p0 = decodeVertexPosition(u_Vertices[vBase], u_Params.bboxMin, u_Params.bboxMax);
        float3 n0 = decodeVertexNormal(u_Vertices[vBase + 1]);
        float2 uv0 = decodeVertexUV(u_Vertices[vBase + 2]);

        vBase = (c.vertexOffset + i1) * 3;
        float3 p1 = decodeVertexPosition(u_Vertices[vBase], u_Params.bboxMin, u_Params.bboxMax);
        float3 n1 = decodeVertexNormal(u_Vertices[vBase + 1]);
        float2 uv1 = decodeVertexUV(u_Vertices[vBase + 2]);

        vBase = (c.vertexOffset + i2) * 3;
        float3 p2 = decodeVertexPosition(u_Vertices[vBase], u_Params.bboxMin, u_Params.bboxMax);
        float3 n2 = decodeVertexNormal(u_Vertices[vBase + 1]);
        float2 uv2 = decodeVertexUV(u_Vertices[vBase + 2]);

        // 投影到屏幕
        float4 clip0 = mul(u_Params.viewProj, float4(p0, 1.0));
        float4 clip1 = mul(u_Params.viewProj, float4(p1, 1.0));
        float4 clip2 = mul(u_Params.viewProj, float4(p2, 1.0));

        // 背面剔除
        float2 edge0 = clip1.xy / clip1.w - clip0.xy / clip0.w;
        float2 edge1 = clip2.xy / clip2.w - clip0.xy / clip0.w;
        if (edge0.x * edge1.y - edge0.y * edge1.x <= 0.0) continue;

        // 屏幕空间 bounding box
        float2 ndc0 = clip0.xy / clip0.w;
        float2 ndc1 = clip1.xy / clip1.w;
        float2 ndc2 = clip2.xy / clip2.w;
        int2 bboxMin = int2(min(min(ndc0, ndc1), ndc2) * 0.5 + 0.5) * u_Params.screenSize;
        int2 bboxMax = int2(max(max(ndc0, ndc1), ndc2) * 0.5 + 0.5) * u_Params.screenSize;

        // 逐像素遍历 + barycentric 插值 + depth test
        for (int py = bboxMin.y; py <= bboxMax.y; py++) {
            for (int px = bboxMin.x; px <= bboxMax.x; px++) {
                if (px < 0 || px >= int(u_Params.screenSize.x) ||
                    py < 0 || py >= int(u_Params.screenSize.y)) continue;

                float2 pixel = float2(float(px) + 0.5, float(py) + 0.5);
                float2 ndc = (pixel / u_Params.screenSize) * 2.0 - 1.0;

                // Barycentric 坐标计算
                float2 v0 = ndc - ndc0;
                float2 v1 = ndc - ndc1;
                float2 v2 = ndc - ndc2;
                float area = edge0.x * edge1.y - edge0.y * edge1.x;
                float w0 = (v1.x * v2.y - v1.y * v2.x) / area;
                float w1 = (v2.x * v0.y - v2.y * v0.x) / area;
                float w2 = 1.0 - w0 - w1;

                if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;

                // 深度插值
                float depth = w0 * clip0.z + w1 * clip1.z + w2 * clip2.z;
                float depthNDC = depth / (w0 * clip0.w + w1 * clip1.w + w2 * clip2.w);

                // Atomic depth test (interlock)
                float prevDepth;
                InterlockedMin(u_GBufferDepth[uint2(px, py)], asuint(depthNDC), prevDepth);
                if (asfloat(prevDepth) <= depthNDC) continue;  // 被遮挡

                // 写入 GBuffer
                float3 worldPos = w0 * p0 + w1 * p1 + w2 * p2;
                float3 normal   = normalize(w0 * n0 + w1 * n1 + w2 * n2);
                float2 uv       = w0 * uv0 + w1 * uv1 + w2 * uv2;

                u_GBufferA[uint2(px, py)]       = float4(1.0, 1.0, 1.0, 1.0); // placeholder albedo
                u_GBufferB[uint2(px, py)]       = float4(normal * 0.5 + 0.5, 0.5); // roughness=0.5
                u_GBufferC[uint2(px, py)]       = float4(0.0, 0.0, 0.0, 1.0); // emissive+AO
                u_GBufferWorldPos[uint2(px, py)] = float4(worldPos, 1.0);
                u_GBufferDepth[uint2(px, py)]    = depthNDC;
            }
        }
    }
}
```

- [ ] **Step 1: 编写 NaniteShared.slang (解码函数)**

写入 `NaniteShared.slang`。

- [ ] **Step 2: 编写 SoftRaster Compute Shader**

写入 `Nanite_SoftRaster.comp`。包含三角形遍历 + barycentric 插值 + atomic depth write。

- [ ] **Step 3: 编写 C++ SoftRaster 调度代码**

写入 `NaniteSoftRaster.h/cpp`。绑定 GBuffer MRT 为 UAV。

- [ ] **Step 4: 编译验证**

```bash
slangc Engine/Shader/Shaders/Nanite/Nanite_SoftRaster.comp -target spirv -entry main -stage compute -I Engine/Shader/Shaders/ -o build/Engine/Shader/Shaders/Nanite_SoftRaster.comp.spv
cmake --build build --target HugEngineRender --config Debug
```

**注入本文件的不一致标注（原文保留，见 §0.2）**：

- **#10**：Interfaces 声明 "Consumes: visible clusters"，但 shader 只绑 `u_Clusters` 并按
  `tid.x` 索引 —— Task 7 产出的 `u_VisibleClusters` / `u_VisibleClusterCount` **未接线**。
- **#11**：结构体声明 `uint materialID;  // bindless base`，函数体使用 `u_Params.materialBase`。
- **#6 / #7**：`u_Indices` 的"per tri"注释与逐 u32 取三索引的代码是两套编码；
  `u_Vertices` 的 "3 u32 per vertex" 与 `NaniteVertex`（4×u32，含 `_pad`）不一致。
- **#5**：本 shader 对 cluster 全部三角形统一处理，没有设计 §5.2 的 `triCount > 16` 硬光栅分流。
- **#14**：写入目标是 6 个（A/B/C/Vel/WorldPos/Depth），而现有 GBuffer 是 8 个附件
  （`kGBufferAttachmentCount = 8`）；Albedo 目前是 placeholder、Roughness 固定 0.5，
  Task 9 的"与标准 GBuffer 像素级一致"验收前必须接入真实材质（bindless 采样）。

---

#### Task 9: DeferredPipeline 集成

**Files:**
- Modify: `Engine/Render/Pipeline/DeferredPipeline.h` (新增 Nanite 成员)
- Modify: `Engine/Render/Pipeline/DeferredPipeline.cpp` (新增 Nanite 路径)
- Modify: `Engine/Render/CMakeLists.txt` (新增所有源文件)
- Modify: `Engine/Shader/CMakeLists.txt` (新增 Nanite shader)

**Interfaces:**
- Consumes: All Task 5-8 APIs
- Produces: 完整 Nanite GBuffer 渲染路径

在 DeferredPipeline::BuildFrameGraph 中新增 Nanite 路径：

```
if (m_UseNanite)  // 新增 Nanite GBuffer 模式
    Nanite_InstanceCull → Nanite_ClusterCull → Nanite_SoftRaster → Lighting
else
    原有 GPU_Cull → GB_Clear → Lighting
```

- [ ] **Step 1: 扩展 DeferredPipeline**

添加 `m_UseNanite` 标志、`NaniteCulling` / `NaniteSoftRaster` 成员。

- [ ] **Step 2: BuildFrameGraph 新增 Nanite GBuffer Pass**

在 GBuffer Pass 位置插入 Nanite 剔除+光栅化 Pass。

- [ ] **Step 3: 更新两个 CMakeLists.txt**

添加所有新技术 .cpp/.h/.comp 文件。

- [ ] **Step 4: 全量编译 + 运行验证**

```bash
cmake --build build --target 04.Sponza-Deferred --config Debug
./build/bin/Debug/04.Sponza-Deferred.exe
```

预期: Nanite 模式下的 Sponza 渲染与标准 GBuffer 渲染画面一致。

> 落地提示（本文件补充，非源文档正文）：
> - `BuildFrameGraph` 的实际位置是 `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp:44`
>   （不是 `DeferredPipeline.cpp`）；Nanite 的插入点是 `GB_Clear`（`:214`）/ GBuffer 绘制
>   （`:213`）这一段，`Lighting`（`:875`）之前。Task 9 的 Files 列表可加上该文件。
> - 帧图按 `RGPassQueue::Compute` 注册的 Pass 会走 AsyncCompute 队列
>   （`DeferredPipeline_FrameGraph.cpp:130,1056`）；剔除链适合放 Compute 队列。
> - CMakeLists 的既有条目位置：`Engine/Render/CMakeLists.txt:184`（`target_sources`）、
>   `Engine/Shader/CMakeLists.txt:125-127`（`COMP_SLANG` 区段）。
> - `Samples/04.Sponza-Deferred/04.Sponza-Deferred.cpp` 与
>   `build/bin/Debug/04.Sponza-Deferred.exe` 均存在，Step 4 的命令可用。

---

#### Task 10: 集成测试与回归

- [ ] **Step 1: 预处理测试**

```bash
# 测试小网格
python Tools/NanitePreprocess/NanitePreprocess.py --input Content/gltf/Cube/Cube.gltf --output Cube.nanite
# 验证 .nanite 文件正确性
python -c "verify_nanite('Cube.nanite')"
```

- [ ] **Step 2: CPU 剔除对比测试**

在 C++ 端创建 100 个实例，CPU 端模拟视锥剔除，与 GPU Compute 结果对比。

- [ ] **Step 3: 软光栅化正确性测试**

渲染单三角形 cube.nanite，对比标准 GBuffer 渲染的像素级差异。

- [ ] **Step 4: Sponza 全量测试**

```bash
python Tools/NanitePreprocess/NanitePreprocess.py --input Content/gltf/Sponza/glTF/Sponza.gltf --output Sponza.nanite
# 在应用中加载 Sponza.nanite，比较 Nanite GBuffer vs 标准 GBuffer
```

> 落地提示（本文件补充，非源文档正文）：
> - `Content/gltf/Cube/Cube.gltf` 实际为 `Content/gltf/cube/cube.gltf`（大小写不一致）；
>   `Content/gltf/Sponza/glTF/Sponza.gltf` 存在。
> - Step 1 的 `verify_nanite('Cube.nanite')` 在 `Tools/NanitePreprocess/` 下**没有对应的
>   实现任务**，需在 Task 4 或本任务内补齐校验函数。
> - Step 2/3 的"CPU 剔除对比"与"像素级一致"是 N2/N3 的核心验收项，与 §13 的完成标准一一对应。

---

### 13. 完成标准

| 里程碑 | 核心交付 | 验证标准 |
|--------|----------|----------|
| N1 | Python 预处理工具链 | .nanite 文件正确生成，LOD 层级 > 0 |
| N2 | GPU 上传 + 两阶段剔除 | 可见 cluster 与 CPU 剔除一致 |
| N3 | 软光栅化 GBuffer | Sponza Nanite 路径与标准 GBuffer 像素级一致 |

**注：源文档此处不一致（#15）**：设计 §9.1 的 Nanite 里程碑到 N6（硬光栅 / LOD 流式 /
材质批次），本完成标准只覆盖 N1-N3。N4-N6 目前没有任务分解与验收标准。

---

## 14. 独立模块化架构与任务清单（本次更新；任务从 1 重新编号）

> **本节是本次评审后的新增内容**，回答两件事：① 把 Nanite 做成**一个相对独立的功能**
> —— 有一个独立的开关（永远可以开启/关闭），架构流程全部收在一个**独立模块**里；
> ② 给出**从 1 开始的完整任务清单**（覆盖 N0 前置 + N1–N6 + 横切，不再只覆盖 N1-N3）。
>
> 第 1–13 章（设计 + 旧 N1-N3 计划）**原样保留**：其中的字段布局、格式细节、验收判据仍然有效，
> 任务重编号与旧编号的对应关系见 **§14.9**，不要按旧编号去找新任务。

### 14.0 结论先行

1. **落点只有两段代码**：剔除段与 GBuffer 段 —— Lighting 及之后完全不动（设计 §5.3 的定位：
   Nanite 只改"几何提交方式"，不是另起一条管线）。
2. **但设计文档假设可复用的「GPU 驱动绘制链」在当前代码里是断的/死的**（证据见 §14.1）
   ⇒ 独立模块的第一件事不是做 cluster，而是**在模块内自建一条真的能跑的 `计数 → 间接绘制` 链**，
   并且**不去修改**既有那条链（保证"关闭即逐位不变"）。
3. **开关关闭时，模块不注册任何 pass、不改任何既有 pass 的读写声明、不产生每帧 CPU 开销**
   ⇒ "既有预设画面不回归"这条验收口径自动成立（§14.2 不变式 1）。

### 14.1 代码实况校正（设计章的"可复用设施"逐条核对，核验时间见文末）

| 设计文档假设可复用 | 代码实况（证据） | 对模块设计的影响 |
|---|---|---|
| GPU 驱动"剔除 → 间接绘制"链（§2.1） | `GPUCulling::Dispatch` 末尾把可见计数**清零**（`GPUCulling.cpp:427`；`SignalPTG` 同 `:738`）；`GBufferRenderer_GPU` 的间接分支要求 `visCount>0`（`:78`）⇒ CPU/GPU **两种模式都退化为逐物体 `DrawIndexed`**，且回退分支不做可见性过滤；唯一真在跑的 GPU 驱动绘制是实例化网格（`InstanceCuller.cpp:200-206`） | 模块**自建**计数/间接链（任务 3），不依赖既有链 |
| GPU WorkGraph 承载剔除→绘制（§2.1） | **死代码**：`GPUWorkGraph` 只出现在自身 .h/.cpp，无任何管线成员或调用点 | 模块**不用** WorkGraph（任务 3 用普通 compute + `DrawIndexedIndirectCount`） |
| mesh shader 处理大 cluster（§2.1） | **桩**：`GBuffer.mesh.slang:30-31` 直接 `SetMeshOutputCounts(0,0)`；`PipelineStateDesc::meshShader` 在 Vulkan 后端存在（`VulkanPipeline.cpp:485-486`）但**从未创建 mesh PSO** | N4 之前必须先真正接入（任务 6） |
| GPUScene（128B/对象）作实例剔除输入（§2.2） | 有，但与渲染用的 `GPUObjectData`（**208B**，断言 `Material.h:52`，上限 1024）是**两套不同步的对象缓冲**（`GPUScene.h:40`，上限 2048） | 实例剔除沿用 `GPUSceneObject` 契约（任务 13），材质字段沿用 208B（任务 19） |
| DGC 间接绘制生成（§2.1） | 有，但门控默认关（`cvDGC_Enable=0`）且位于上述**不可达分支**内 | 暂不用 DGC（任务 3 用传统间接 count） |
| Hi-Z 金字塔（§2.2） | `BuildHiZPyramid` 已实现（`GPUCulling.cpp:478-543`） | **复用**（任务 15） |
| GBuffer 8×MRT 可复用（§2.2/§5.3） | 8 颜色附件 + `D32_FLOAT`（`GBufferRenderer.h:16-26`），usage **只有 `RenderTarget\|ShaderResource`，无 `UnorderedAccess`**（`GBufferRenderer.cpp:150-186`） | 软光栅写 GBuffer 必须先加 UAV（任务 4） |
| 现有逐物体提交成本 | 阴影 pass 逐 cascade × 逐 mesh `snprintf`+`SetDrawDebugLabel`+`DrawIndexed`（`CSMTechnique.cpp:170-196`），实测 **28–33 ms/帧**；GBuffer 逐物体循环同构（`GBufferRenderer_CPU.cpp:100-126`）；整帧 CPU 受限 19–29 fps（`docs/已实现功能/Lumen设计与实现.md` 附四十四） | 这是"GPU 驱动几何"的**动机证据**，但属另一条线（任务 3 只服务 Nanite） |
| 文档行号可用 | 设计 §7.1 引用的 `DeferredPipeline_FrameGraph.cpp:214/875` 与当前实际（`:235`、`:1210`）**已漂移** | 照抄前先核对；本节的引用均为**本次核对**后的行号 |

### 14.2 三条不变式（其余设计由它们推出）

1. **开关关闭 ⇒ 与今天逐位相同**：模块 pass 只在该开关开启时注册；不修改任何既有 pass 的
   `reads/writes`；关闭时不产生新的每帧 CPU 开销。
2. **模块只依赖"已存在的契约"，不要求既有代码为它让路**：对象/材质契约（`GPUObjectData` 208B +
   bindless `materialID`）、GBuffer 输出格式（8×RGBA16F + D32）、Hi-Z 金字塔。
3. **开关打开时，GBuffer 段的"几何写入者"唯一**：要么既有逐物体路径写、要么模块写，
   **二者互斥**（不是同一 pass 里各写一半），避免深度/排序契约被拆成两处维护。

### 14.3 模块边界

```
Engine/Render/Nanite/                 # 新目录，与 Engine/Render/Lumen/ 同构
  NaniteTypes.h                       # 纯 POD（与 Slang 共享；RHI-free，可被 Scene 侧 include）
  NaniteSettings.h                    # 开关与档位（真值）
  NaniteScene.{h,cpp}                 # 数据宿主：实例表、cluster 表、几何/量化缓冲、LOD 错误
  NaniteUpload.{h,cpp}                # .nanite 资产 + MeshBatcher 合并几何 → GPU 缓冲
  NaniteCull.{h,cpp}                  # 实例剔除 + cluster BVH 剔除 + Hi-Z 遮挡
  NaniteRaster.{h,cpp}                # 软光栅（后加硬光栅分支）
  NaniteRenderer.{h,cpp}              # 模块门面：帧图接入、资源生命周期、耗时读数、诊断
Engine/Shader/Shaders/Nanite/         # shader 独立子目录（须登记进 COMP_SLANG 显式列表）
Tests/TestNaniteTypes.cpp             # 数据格式/边界单测（RHI-free）
```

**公共面只有三个**：`NaniteRenderer`（生命周期 + 帧图接入）、`NaniteSettings`（开关/档位）、
`NaniteTypes.h` 的 POD。**依赖禁令**（写进模块文件头，作为架构约定）：模块内不得出现
`GI_*` / `Lumen*` / `GPUCulling` 的**内部结构**（可借其 Hi-Z 纹理句柄与描述符写法）；
不得引用 `MeshBatcher` 的运行时状态（只当**一次性输入**）。

### 14.4 独立开关（三层）

| 层 | 载体 | 说明 |
|---|---|---|
| 真值 | `NaniteSettings::enabled` | 模块自持，唯一真值 |
| 配置 | CVar `r.Nanite.Enable` + cfg 键 `nanite_enable`（**默认 0**） | 与既有 `r.Decal.Project` / `gi_*` 同风格；样例退出时回写 cfg（沿用 `gi_half_res` 的往返写法） |
| 面板 | 样例 ImGui 勾选框 + 档位下拉（软光栅 / 混合光栅） | 改动即写回 `NaniteSettings` |

**门控点只有一个**：`DeferredPipeline_FrameGraph.cpp` 的 GBuffer 段选择
（现状 `GB_Clear` 是唯一 GBuffer 写入者，`:235-268`）：

```cpp
if (m_Nanite.GetSettings().enabled && m_Nanite.IsReady()) {
    m_Nanite.AddPasses(rg, gbHandles..., m_GITimer);   // 模块自注册：InstanceCull → ClusterCull → Raster
} else {
    /* 既有 GPU_Cull / GB_Clear / ... 原样，一行不动 */
}
```

**开关粒度**：全局开关 × **每网格参与位**。每网格参与性由**资产是否存在**决定 ——
`MeshComponent` 上只加一个"Nanite 资产路径 + 不透明 `u64` 句柄"（**不含 Render 类型**），
有资产且全局开启 ⇒ 该网格由模块绘制，否则走既有路径。

### 14.5 帧图接入契约（模块化最容易出错的地方）

- **深度与排序**：`GB_Clear` 独占写深度，并被 `gbDepth/gbWorldPos` 的 **WAW 假依赖**用来给
  Shadow 定序（`:213-215`）。模块的 `Nanite_Raster` 必须声明**同一组** reads/writes（含这条 WAW），
  否则 Shadow/Lighting 排序会静默变化 —— 这是"独立模块"与"不回归"之间唯一的硬约束。
- **GBuffer 契约**：模块**自己**建 PSO/附件布局，直接写既有 GBuffer 纹理句柄。
  这样**不需要**给 `GBufferRenderer` 加 `Mode::Nanite`（比"改渲染器"更独立），
  代价是模块内要复刻 `BeginOffscreenPassMRT(cv,8,...)` 的用法（`GBufferRenderer_CPU.cpp:60`）。
- **软光栅与深度**：compute 写 GBuffer 需要纹理带 `UnorderedAccess`。**已裁决（任务 4，2026-09-20）走 (A1)**：
  GBuffer 的 8 张颜色目标已加 UAV（`usage` 只增；既有路径转储逐位不变，已实测），软光栅用
  `RWTexture2D` 写颜色目标；模块 pass 声明与 `GB_Clear` 同组的深度 WAW 以维持排序。
  **深度不改成"存储图像写入"**：实测本机 NVIDIA RTX 4060 上 `D32_SFLOAT` 支持
  `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT`，但**同机 AMD 核显不支持**（`D32_SFLOAT_S8_UINT` 在 NVIDIA 上也不支持）
  ⇒ compute 写深度无法跨厂商；深度继续走既有附件路径（或片元 `SV_Depth`）。只有将来确需跨厂商解耦
  材质/深度时才升级到备选 **(A2)**（模块内自建 VisBuffer `triangleID+depth` + 材质解析 pass）。
  实现与证据见 §14.14。
- **objectIndex 分区**：模块实例占**独立 index 空间**（自持 `NaniteInstance` 缓冲；
  实例内沿用 208B 的材质部分）。既有的三处枚举一致性契约
  （`SceneRenderer.cpp:102-142`、`MeshBatcher.cpp:75-86`、`GPUScene.cpp:66-82`）**不受影响** ——
  这正是独立编号空间的价值。
  **定稿（任务 5，2026-09-20）**：普通段 `[0,1024)`（= `MAX_OBJECTS`，与 `GPUObjectData` 缓冲容量一致）、
  Nanite 段 `[1024,2048)`（容量 **1024**）、哨兵 `0xFFFFFFFF`。容量上限不是随便定的：
  `gb_lightmapkey` 是 **RGBA16_FLOAT**，binary16 的精确整数上限是 **2^11 = 2048**，页号 2049 会被量化成 2048
  —— 任务 1 注释里写的"2^24"是 float32 的上限，**单位错了**。实测编码 = `float4(页内uv.xy, objectIndex, 0)`
  （`GBuffer.frag.slang:134`）；今天**没有任何着色器**解码页号（`DeferredLighting.frag.slang:30` 只声明绑定、从未采样），
  唯一解码点是离线工具 `Tools/gi/lightmap_key_check.py`（硬编码 `page < 1024`）⇒ 当天不存在显存越界路径。
  该工具与"混排场景运行时校验"一起，随任务 18（软光栅真正写 GBuffer）按段分类修。详见 §14.15。
- **不用死代码**：不用 WorkGraph（死代码）、不用 DGC（默认关且在不可达分支）、
  不依赖 `GPUCulling` 的可见计数（`:427` 被清零）。模块自持 `DrawIndexedIndirectCount` 链。

### 14.6 与现有代码的接触面（越少越好，逐条可回退）

| 文件 | 改动 | 规模 |
|---|---|---|
| `Engine/Render/CMakeLists.txt` | 登记 `Nanite/*` 源文件 | 小 |
| `Engine/Shader/CMakeLists.txt` | 把 `Nanite_*.slang` 列进 `COMP_SLANG`（**显式列表，不能靠 glob**） | 小 |
| `Engine/Render/Pipeline/DeferredPipeline.{h,cpp}` | 持有 `NaniteRenderer`、生命周期转发、`Set/GetSettings` | 小 |
| `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp` | GBuffer 段的 `if/else`（**唯一逻辑改动**） | 小 |
| `Engine/Scene/Scene/MeshComponent.h` | 加 `std::string naniteAsset; u64 naniteHandle`（**只放字符串/handle**） | 1 处 |
| `Samples/06.GILab/06.GILab.cpp` | 面板开关 + cfg 读写 | 小 |
| `Tests/CMakeLists.txt` | 登记 `TestNaniteTypes.cpp` | 1 行 |

**明确不改**：`GBufferRenderer.*`、`LightingPass`、`GPUCulling.*`、`InstanceCuller`、
四个 Shadow 技术、`RTPass`（虚拟化几何如何进 BLAS 单独裁决，见任务 25 之后）。

### 14.7 场景侧接口（绕开设计 §12 Task5 点名的反向依赖）

旧计划 Task5 L1446-1450 指出 `NaniteComponent.h`（Scene）include `NaniteRenderer.h`（Render）
会形成 **Scene → Render 反向依赖**。**解法**：`MeshComponent` 只存资产路径 + 不透明 handle；
`NaniteTypes.h` 只放 POD（不含 RHI 类型）并可被 Scene include；"路径 → 资产 → GPU 缓冲"的解析
全部发生在 Render 侧模块内。`HugEngineScene` 不需要知道 Render 的任何类型。

### 14.8 任务清单（从 1 开始；每项：目标 / 改动点 / 验收）

> 依赖关系：阶段 0 是**硬前置**（没有它，N2/N3 产出的可见簇与间接参数没有消费者）。
>
> **进度（2026-09-20）**：**阶段 0（任务 1–6）与阶段 1 前两项（任务 7 数据格式定稿、任务 8 离线簇切分）已完成**并通过验收：模块骨架与独立开关、
> 开关不变式判据 ⑥、模块自持的「计数 → 间接绘制」链（含最小 RHI 扩展）、GBuffer UAV（A1 裁决）、
> objectIndex 分区契约与单测、mesh PSO 真正接入。**任务 7（`.nanite` 数据格式定稿）也已完成**
> （§8 的四处不一致已裁决并写回 §8，见 §14.17）；**任务 8/9/10/11/12/13/14 均已完成**——
> 任务 9–13 的证据见 §14.19–§14.22，**任务 14（per-instance cluster BVH：构建 + 深度优先遍历）见 §14.23**；
> **下一步从任务 15（三阶段簇剔除 + Hi-Z）开始**（它要做的第一件事就是把任务 13 的实例剔除与任务 14 的
> BVH 遍历接成同一条链，见 §14.23⑥）。
> 每一步的证据分别见 §14.11、§14.13–§14.17、§14.19–§14.23，且都有对应的中文提交。
>
> **进度更新（任务 15/16 已完成）**：任务 15（三阶段簇剔除 + Hi-Z）见 §14.24；**任务 16（可见簇列表 +
> 间接参数接线）见 §14.25** —— 开启档默认档的绘制由**可见簇列表**写出的真实间接命令驱动
> （`visible_wiring visible=C=D=R`、`empty_draws=0`、`mismatch=0`），任务 3 的假簇链保留为
> `nanite_fake_chain` 自证/退化开关；开启档 pass 数为 **14**（既有 12 + `Nanite_Cull` + `Nanite_CullChain3`，
> 绘制录在后者体内）。**下一步从任务 17（CPU 参考对照工具）开始**。
>
> **进度更新（2026-09-21）**：任务 17–19 已完成（§14.26–§14.29），任务 15 的 P0（Hi-Z UV 镜像）见 §14.28；
> **任务 20（深度与排序契约）见 §14.30** —— 本轮修掉两处 P0：① 深度解析 PSO 的 `depthTest=false`
> 让 Vulkan 规范**丢弃全部 `SV_Depth` 写入**；② `StructuredBuffer::GetDimensions` 被当成二维宽高用，
> 使深度解析只写了第 0 行。两处合起来导致"模块接管后深度附件恒为远平面"、判据 ⑦ 的 `hiz1` 档变空转；
> 现已修复且判据 ⑦ 恢复 PASS（未改档位、未放宽守卫）。**判据 ⑦ 的 hiz1 档在默认阈值 16 下读数偏薄
> （`occluded=4`），原因是任务 18 把 >16 三角形的簇留给任务 22 —— 任务 22 落地后该档会自然变强。
> **任务 20 与任务 21 均已完成、全量八条判据 `ACCEPTANCE SWEEP: PASS`。**
> **进度更新（2026-09-21）**：**任务 22（mesh shader 硬光栅 + 软硬分流）已完成**，见 §14.31。
> 硬光栅为独立开关 `nanite_hard_raster`（**默认关**），关闭时冻结指纹与任务 21 逐字相同。
> **下一步 = 任务 23（混合光栅分配策略 + 性能读数）**：任务 22 已暴露两处性能事实 ——
> 硬光栅 341× 过绘、mesh 线程利用率约 50%（§14.31 的"存疑未做"），正是任务 23 的输入。
>
> **进度更新（2026-09-21，任务 23 已完成）**：**任务 23（混合光栅分配策略 + 性能读数）已完成**，见 §14.36。
> 新增两条读数：`size_dist`（可见簇的簇大小分布五桶；dump 帧一行，与 `soft_raster`/`hard_raster` 同门控）
> 与 `perf`（把软硬分流计数与**帧时**绑在同一行；受既有 `HE_CPU_PASSES` 门控，**默认关**）。
> **关键实测结论（与"分流能省帧时"的直觉相反，以实测为准）**：分流是**同覆盖下的 GPU 成本杠杆**、**不是**
> 帧时杠杆 —— 全软光栅档（阈值 64，写标记覆盖 44.43%）GPU 合计 **781 ms**，同覆盖的软硬分流档（阈值 16）
> **42.7 ms** ⇒ **15~23×**；而本样例整帧 **CPU 受限**（GPU 9~43 ms vs 墙钟 126~146 ms），
> 换算到帧时只值约 11%。另：分布**双峰**（`[61,0,0,0,31587]`）⇒ **阈值 ∈[4,32] 等价、=64 退化**；
> `base16` 只是"少画 99.8% 的簇"，**不能**当"更省"的基线。冻结指纹与判据 ⑥⑦⑧ 未动（sweep PASS）。
> 另修掉一个真问题：`LogFrameBudget()` 在 07.Nanite 里**从不执行**（唯一调用点在 Lumen 段，而该 cfg 未请求 Lumen）。
> **下一步 = 任务 24（LOD 流式）**——注意 §14.10 第 1 条：**文档空白，须先补设计再实现**（§14.32 是起草稿）。

**阶段 0：模块化前置（独立开关先落地）**

| # | 目标 | 改动点 | 验收 |
|---|---|---|---|
| 1 | 模块骨架 + 独立开关（N0） | 建 `Nanite/` 目录与 6 个文件；`NaniteSettings`；`DeferredPipeline` 持有时机与生命周期；帧图 `if/else`（开启时只注册一个 `Nanite_Noop` pass） | 开关关闭 ⇒ 转储逐位一致；开启 ⇒ pass 列表出现 `Nanite_Noop`、画面不变 |
| 2 | 开关不变式守卫 | 把"关闭 ⇒ pass 集合与转储逐位一致"写进 `build/verify/acceptance_sweep.ps1`（新增判据 ⑥） | 一条命令输出该判据 PASS；人为破坏开关门控时能 FAIL |
| 3 | 模块自持的「计数 → 间接绘制」链 | `NaniteCull` 内写计数缓冲 + `IndirectCmdBuf`；绘制端用 `DrawIndexedIndirectCount`（**不改** `GPUCulling`） | 用假数据（1 个实例、N 个簇）验证"计数为 k ⇒ 恰好画 k 次"，且读回计数与绘制一致 |
| 4 | GBuffer UAV 变体（A1）落地 | GBuffer 纹理加 `UnorderedAccess`；确认既有渲染通道路径不受影响（usage 只增不改语义） | 既有路径画面逐位不变；compute 能写一张测试 GBuffer 并在同一帧被 Lighting 正确读到 |
| 5 | objectIndex 分区契约 | 定 `objectIndex` 分区表（Nanite 段 vs 普通段），写进 `NaniteTypes.h` 注释与单测 | 混排场景（Nanite + 普通网格）下 `gb_lightmapkey` 解析正确、无越界 |
| 6 | mesh shader 真正接入 PSO | 按 `PipelineStateDesc::meshShader`（`VulkanPipeline.cpp:485-486`）建一个最小 mesh PSO 并渲染一帧 | 校验层无新增 VUID；能输出非空画面（为任务 22 铺路） |

**阶段 1：N1 预处理（离线管线）**

| # | 目标 | 变动点 | 验收 |
|---|---|---|---|
| 7 | `.nanite` 数据格式定稿 | 裁决设计 §8 的四处不一致：文件头 96B vs 128B、顶点 16B vs 12B（含量化偏置）、索引 3×u16 vs u32、`coneData` vs `coneAxisAngle`；裁决结果写回 §8 并同步 C++/Slang | 单测覆盖头部字段与尺寸；`static_assert` 钉住布局 |
| 8 | 离线 cluster 切分 | `meshopt_buildMeshlets`（§4.1 L246-247）；≤64 tri / ≤128 vert | 每网格簇数、每簇三角形上限、无退化簇 |
| 9 | LOD 与 DAG | 边折叠逐级减半 + 哈希去重（§4.1 L248） | LOD 层级 > 0；DAG 去重率 > 10%（旧 Task3 判据） |
| 10 | 量化与打包 | 顶点/法线/UV 量化、索引编码、材质（8B） | 量化往返误差在阈值内（单测）；pack/upload/shader 三处一致 |
| 11 | `NaniteTypes` 单测 | `Tests/TestNaniteTypes.cpp`（RHI-free） | 尺寸/偏移/量化往返/边界全绿 |
| 12 | 资产加载与 GPU 上传 | `NaniteUpload` + `NaniteScene` 资源宿主；只从 `MeshBatcher` 合并几何**读**一次 | 上传后 GPU 缓冲字节数与 CPU 侧一致（读回校验） |

**阶段 2：N2 剔除**

| # | 目标 | 改动点 | 验收 |
|---|---|---|---|
| 13 | 实例剔除 | 沿用 `GPUSceneObject`（128B）契约（§5.1 Phase 1） | 与 CPU 实例剔除逐项一致 |
| 14 | per-instance cluster BVH | 构建 + 深度优先遍历（§5.1 Phase 2） | BVH 节点数与遍历访问数可复现 |
| 15 | 三阶段簇剔除 + Hi-Z | Phase1 视锥 → Phase2 持久化 BVH + Hi-Z 遮挡 → Phase3 LOD 选择（§5.1）；复用 `BuildHiZPyramid` | 与 CPU 参考剔除**逐簇一致**；Hi-Z 打开/关闭差异可解释 |
| 16 | 可见簇列表 + 间接参数接线 | `u_VisibleClusters` 真正接到光栅端（旧计划 Task8 的缺口） | 绘制次数 = 可见簇数；无空转 —— **已完成（§14.25，2026-09-20）**：`visible=indirect_count=draws=rasterized`、`empty_draws=0`、`mismatch=0`；零可见簇 ⇒ 零绘制；容量不足 ⇒ 截断并计数（不越界） |
| 17 | CPU 参考对照工具 | 一个可复现脚本/命令，输出"可见簇集合差异" | 与任务 15 的验收判据同源、可回归 |

**阶段 3：N3 软光栅**

| # | 目标 | 改动点 | 验收 |
|---|---|---|---|
| 18 | 软光栅写 GBuffer | compute（≤16 tri/簇）+ interlock 写 GBuffer（§5.2） | 与既有路径**同场景同相机**对照（均值/相关系数）+ 白炉 1.0000 —— **已完成（§14.27，2026-09-20）** |
| 19 | 真实材质接入 | 去掉旧计划 Task8 的 placeholder 与固定 roughness（L1881-1891） | 材质字段与既有 GBuffer 路径逐项可比 —— **已完成（§14.29，2026-09-21）**；另附带修掉任务 15 的 P0（Hi-Z UV 镜像，§14.28） |
| 20 | 深度与排序契约 | 复刻 `GB_Clear` 的 WAW 声明（`:213-215`） | Shadow/Lighting 排序不变（pass 顺序与转储一致） |
| 21 | 画面级对照验收 | 开关 ON/OFF 两档对照 | 差异可解释（几何覆盖/材质），且关闭档与基线逐位一致 |

**阶段 4–6：N4 / N5 / N6（设计文档只有里程碑名，需先补设计）**

| # | 目标 | 备注 | 验收 |
|---|---|---|---|
| 22 | mesh shader 硬光栅 + 分流 | 复用任务 6 的管线；`triCount > 16` 走硬光栅（§5.2 L322-334） | 混合光栅画面一致、软硬占比可读 —— **已完成（§14.31，2026-09-21）**：硬光栅接手 31587/31587 个大簇（100%）、软硬占比读数齐备、A/B 覆盖 99.62% 且 `worldpos corr=0.9989`；附带宽带修掉 3 个既有 RHI 潜伏缺陷 |
| 23 | 混合光栅分配策略 + 性能读数 | 阈值/簇大小分布对帧时的影响（接入 `HE_CPU_PASSES` 与 `LogFrameBudget`） | 帧时读数可复现；无回归 —— **已完成（§14.36，2026-09-21）**：新增 `size_dist` 五桶分布（25 个日志零违反）与 `perf` 行（`frame_ms` 与【帧预算】逐位同源）；同覆盖对照 soft64 vs hard16 = **GPU 15~23× / 墙钟 5.5~7×**；阈值 ∈[4,32] 等价、=64 退化；数据类读数逐位可复现、时序类给中位数+极差口径；`-OnlyNanite` sweep **PASS**、冻结指纹不变 |
| 24 | LOD 流式（反馈 + 页池） | **文档空白**（无 cluster page / page pool / 流式设计），需先补设计再实现 —— 设计由 §14.32 补齐（含 6 处修正） | 先补设计评审，再定验收 —— **已完成（§14.37，2026-09-21）**：阶段一（页池 + 页表 + 间接层 + 反馈 + 驻留管理 + 读数 + 开关）落地；页 = 「簇段 + 顶点段 + 三角形段各取一段」（对齐整份共享内容边界，**不改 .nanite 格式**）；判据 (a)(b)(c)(d1)(d2)(d3)(e) 全部有实测（(c) 池 8 槽 `page_misses=122` 且读数与 `visible` 精确对账；(d1) 数据类读数逐位相同 + `requests_total>0` 非空洞守卫；(d2) 信号 729 ≤ 噪声底 734）；单测 **316/71,095 全绿**；**判据 (d) 的原始形式（4 张 GBuffer 逐位相同）不可达**（§14.31 ⑩ 的既有不确定性），已按 (d1)/(d2)/(d3) 改口径并如实记录；**存疑**：预取/LOD 选择、磁盘 I/O（阶段二） |
| 25 | Material Bin | 按材质分组 + bindless 材质数组（§5.4） | 多材质场景无 draw 爆炸；描述符切换次数可读 —— **两条都已闭合（§14.33 ⑬，2026-09-21）**：前半条是**结构事实**（bindless 数组 + 单描述符集 ⇒ 任意多材质只需一次 dispatch/draw，§14.33 ①），后半条由新读数行 `[Nanite] material_bin` 给出（`descriptor_switches=1`、`material_switches` 361（GPU 可见簇顺序）/ `material_switches_bin` 41 / 资产自然顺序 1608）；只读 bin（`u32[簇数]` 按材质非降序）在**上传期一次**生成、不动资产/BVH/DAG/`.nanite` 格式；开关 `NaniteSettings::materialBin` 默认 false，关闭档**逐行相同**（唯一差异是配置回显多一个键）；单测 **322/71,192 全绿**；**明确不做**：把光栅遍历顺序改成 bin 顺序（深度键平局由 UAV 写序决定 ⇒ 会改变画面，§14.31 ⑩） |

**阶段 7：横切**

| # | 目标 | 验收 |
|---|---|---|
| 26 | 调试与可视化 | 簇/BVH/LOD/软硬光栅占比/可见簇数可视化；每个已知故障模式都能被至少一个工具观察到（沿用 Lumen 的 40 号任务口径） |
| 27 | 单测与预设回归收口 | `HugEngineTests` 全绿；默认预设抖动族之外 0 项差异；开关不变式（任务 2）常跑 |

### 14.9 与旧 §12 任务编号的映射（避免按旧编号找新任务）

| 旧编号（§12） | 新编号 |
|---|---|
| Task 1 数据格式 | → 7（+ 11 单测） |
| Task 2 簇划分 | → 8 |
| Task 3 LOD+DAG+量化 | → 9、10 |
| Task 4 打包 | → 10 |
| Task 5 组件+上传 | → 12（+ 场景侧接口见 §14.7） |
| Task 6 实例剔除 | → 13 |
| Task 7 簇剔除+BVH | → 14、15 |
| Task 8 软光栅 GBuffer | → 18、19 |
| Task 9 帧图集成 | → 1、5、16、20 |
| Task 10 集成测试回归 | → 17、21、27 |
| （旧计划无对应） | → 2、3、4、6、22–26 |

### 14.10 文档空白与风险（实现前必须补）

1. **页面/流式**：全文无 cluster page / page pool / 分页设计（只有里程碑名 N5）⇒ 任务 24 必须先补设计。
2. **位移（displacement）**：全文无 ⇒ 明确不做，或单独立项。
3. **非 Nanite 网格 fallback**：全文无逐网格 fallback（只有 `if (m_UseNanite)` 整体二选一）⇒
   本引擎场景**必然混排**（动态/实例化/阴影几何），任务 5 的分区契约就是它的落点。
4. **`m_BatchBuilt` 只建一次**（`DeferredPipeline.h:219`）：场景增删网格既不重建也不重排 ⇒
   模块自己必须支持**增量重建**，不能沿用这个假设。
5. **虚拟化几何 → 光追 BLAS**：`RTPass::BuildAS` 每帧遍历全部 mesh（`RTPass.cpp:323-407`）⇒
   与 Nanite 的关系需单独裁决（另一条线）。

### 14.11 验收口径（沿用仓库既有）

每一步都要过：白炉 `prov6_final` **1.0000**；背靠背同配置两次运行在既有抖动族
（`prov0_ao_*`/`hdr`/`radiance`）之外 **≤2 个 f16 ULP**；关 Lumen 时 `lumen_passes=0`；
默认预设抖动族之外 **0 项差异**；`HugEngineTests` 全绿；**判据 ⑥ 开关不变式**。
一条命令：
`powershell -NoProfile -ExecutionPolicy Bypass -File build\verify\acceptance_sweep.ps1`
（`-OnlyNanite` 只跑判据 ⑥，用于迭代开关不变式与做负向验证）。

**判据 ⑥ = 开关不变式（§14.8 任务 2，2026-09-20 落地）**
- ⑥a（关闭档）：`07.Nanite` 的 pass 集合必须**不含任何 Nanite pass**；连续两次关闭档必须给出
  **同一个指纹**；且该指纹必须等于冻结值
  `1C15AB72E688B5302332AEC391C41A5FE2B4D9512258CCDCD5D3E9D7E8F5390D`
  （12 个 pass：GPU_Cull, Shadow, GB_Clear, Decal_Project, SSAO, IBL_Bake, Lighting, Skybox,
  CaptureRadiance, AutoExposure, TAA_Resolve, ToneMap）。
- ⑥b（开启档）：pass 集合必须**恰好**多出 `Nanite_Noop`，其余 pass 与顺序一字不变。
- ⑥c（转储）：开启档与关闭档在抖动族之外**逐位一致**；并以"关闭档 vs 关闭档"作对照，
  证明差异集合确实是基线自带抖动。
- **负向验证（必须能 FAIL）**：2026-09-20 实测——把 07.Nanite 的开关真值强制为 `true`
  （cfg 写 `nanite_enable=0` 也不生效）后，判据输出 `ACCEPTANCE SWEEP: FAIL` 并给出三条理由
  （关闭档泄漏 1 个 Nanite pass、关闭档指纹漂移、开启档相对既有集合发生变化）；还原后立即恢复
  `PASS`。**同时发现**：只破坏帧图外层门控而不动真值时**不会**泄漏，因为
  `NaniteRenderer.cpp` 的 `AddPasses` 自带第二层守卫 `if (!enabled || !m_Ready) return;`
  —— 这是刻意的纵深防御，不要为了"单点门控"把它删掉。
- 脚本位置：`build\verify\nanite_smoke.ps1`（单次冒烟，含 pass 集合归约）+ `acceptance_sweep.ps1`
  的判据 ⑥ 段。两者都在被 gitignore 的 `build/` 下（与 Lumen 的验收脚本同处），
  仓库内的**权威记录是本节的判据定义与冻结指纹**；指纹变化时先改这里再改脚本。

**任务 22 的分流验收口径（2026-09-21 拟定；数值为改动前基线，实现后需回填实测）**

> 本节是任务 22（mesh shader 硬光栅 + 软硬分流）的验收定义。定这些口径的依据是**既有脚本的实际
> 判据**（`build\verify\nanite_takeover_cmp.ps1` / `nanite_smoke.ps1`）与 **2026-09-21 的实测读数**，
> 不是设计意图的复述。

- **三档语义（新开关必须默认关闭）**
  - `enabled=0`：逐位不变（判据 ⑥）。
  - `enabled=1`（硬光栅**默认关**）：**必须与任务 21 完全一致** —— 关闭档指纹
    `1C15AB72E688B530…`、开启档 14 个 pass / `nanite_passes=2` / sha `750CC247BF8B9C3D…` 一字不变。
  - `enabled=1 && hard_raster=1`：任务 22 的新路径，本节的 A/B 读数只在这一档取。
  - **为什么硬光栅必须默认关（这条有硬证据，不是保守）**：判据 8b 逐文件比对开关两档转储，只允许
    `$gbTargets = albedo/gb_normal/gb_worldpos/gb_lightmapkey` 与抖动族
    `hdr|prov0_ao_final|prov0_ao_raw|radiance` 变化，其余任何差异即 `unexpected != 0` → FAIL。
    而默认阈值 16 档模块**只覆盖极小面积**（`pixels_written=3264` 是**写次数**口径 ≈0.16%；
    按**去重像素**口径只有 `depth_written=106` ≈ **0.0051%**。两个口径差 30 倍，引用时须注明是哪一个
    —— 此点由任务 23 实测纠正，见 §14.36），所以 `prov1_*` / `rsm_*` /
    `ssr` / `ibl_irr` 这些 **GBuffer 下游**转储仍在 f16 容差内。一旦分流让覆盖率跳到几十 %，
    这些转储必然变化 ⇒ 8b 立刻红。故硬光栅只能是**独立开关 + 默认关**（与任务 4 的 `testWrite`、
    任务 6 的 `meshTest` 同款）。

- **A/B 配对（"混合光栅画面一致"的量化形式）**
  任务 22 的本质是"**同一批几何，一部分换了光栅化路径**"，所以正确的参考不是"模块 vs 引擎"，
  而是"全软光栅 vs 软硬混合"：
  | 档 | cfg（经 `HE_SMOKE_EXTRA` 传入） | 语义 |
  |---|---|---|
  | A 参考 | `nanite_enable=1;nanite_soft_max_triangles=64` | 全部簇走软件光栅 |
  | B 受测 | `nanite_enable=1;nanite_hard_raster=1`（阈值默认 16） | 小簇软件、大簇硬件 |
  ```powershell
  $env:HE_SMOKE_EXTRA = 'nanite_enable=1;nanite_soft_max_triangles=64'
  powershell -NoProfile -ExecutionPolicy Bypass -File build\verify\nanite_smoke.ps1 -Tag t22_soft64
  $env:HE_SMOKE_EXTRA = 'nanite_enable=1;nanite_hard_raster=1'
  powershell -NoProfile -ExecutionPolicy Bypass -File build\verify\nanite_smoke.ps1 -Tag t22_hard16
  python build\verify\nanite_takeover_cmp.py t22_soft64 t22_hard16 --dir build\verify
  ```
  （覆盖必须走 `HE_SMOKE_EXTRA`：脚本第 9-10 行注明 `-Extra` 作为 `-File` 参数在本环境被拒。）

- **期望数值（改动前基线 → 实现后须逐项回填）**
  | 量 | 基线（`enabled=1`，阈值 16） | 实现后期望 | 不符时的含义 |
  |---|---|---|---|
  | `soft` | **61** | 仍为 61 | 小簇被硬光栅抢走 ⇒ 早退条件写反 |
  | `skipped_big` | **31587** | 字段名与语义**不许改**（判据 8a 依赖）；由硬光栅行新字段表达"被接手" | — |
  | 硬光栅簇数（新字段） | — | **≈31587** | 只有 0/几百 ⇒ 网格任务数用了 CPU 常量 |
  | 覆盖（写标记像素） | 3264 写次数（≈0.16%）／**106 去重像素（0.0051%）** | **≈921399（0.4443）**，即与 A 档同量级 | 显著偏低 ⇒ 大簇被丢，分流未真正生效 |
  | A vs B `gb_worldpos` corr | — | **≈1.0，应远高于 0.9290** | 只有 0.92x ⇒ 两套顶点投影不一致（很可能用了 Slang 矩阵乘法而非 vpRow 点积） |
  | A vs B `metallic` 直方图 corr | — | ≥0.99 | — |
  | A vs B neutral-material pixels | — | 0 | 硬光栅用中性常数而非资产材质 |
  参考：A 档（= 阈值 64 全覆盖档）的既有实测为写标记 `921399 px`、覆盖 `0.4443`、
  `worldpos corr=0.9290`（**该值是"模块 vs 引擎"，含几何差异**，故只有 0.93）、
  `metallic corr=0.9992`、`material_pixels == pixels_written == 44318207`、`skipped_big=0`。

- **实测回填（2026-09-21，任务 22 落地后；详见 §14.31）**
  | 量 | 期望 | **实测** |
  |---|---|---|
  | `soft` | 61 | **61**（与基线逐位相同） |
  | 硬光栅簇数 | ≈31587 | **31587**（== `skipped_big`，100% 接手） |
  | 覆盖（写标记像素） | ≈921399（0.4443） | **917903（0.4427，= A 的 99.62%）** |
  | A vs B `gb_worldpos` corr | ≈1.0 且远高于 0.9290 | **0.9989** |
  | A vs B `metallic` 直方图 corr | ≥0.99 | **1.0000**（双方都写 / 整幅对整幅两种口径都是） |
  | A vs B neutral-material pixels | 0 | **0** |
  两条口径提醒（实测踩到，务必沿用）：① 既有 `nanite_takeover_cmp.py` 的 `metallic` 直方图是
  "**参考帧整幅** vs **受测帧被写像素**"，为"模块 vs 引擎"设计；用于**模块 vs 模块**的 A/B 会给出
  假的 FAIL（A 整幅有 ~55% 清屏值 `metallic=1.0`）⇒ A/B 必须改用"双方都写"或"整幅对整幅"口径。
  ② `hardRaster=1` 档的 `nanite_passes` 仍是 **2**（硬光栅录在既有 pass 体内，帧图零新增），
  **不是** 3 —— 本文档上一版把这里写成 3 是错的。

- **尺寸约束（2026-09-21 实测本机）**
  - `meshInvocations=128, meshVertices=256, meshPrimitives=256` ⇒ **`[numthreads(N,1,1)]` 的 N ≤ 128**；
    建议按 `DeviceCaps::maxMeshOutputVertices/maxMeshOutputPrimitives` 与
    `VulkanDevice::m_MaxMeshWorkGroupInvocations` 钳制，不要硬编码。
  - 簇上限 `kNaniteMaxClusterTriangles=64` / `kNaniteMaxClusterVertices=128` ⇒ mesh shader 输出数组
    必须声明到 **64 图元 / 128 顶点**，靠 `SetMeshOutputCounts` 报实际值。

- **深度次序的既有约束（决定硬光栅能插在哪一步）**
  深度解析 PSO（`NaniteRaster.cpp`）是 `depthTest=true` + `depthCompare=**Always**` + `depthWrite=true`，
  且 `depthLoadOp` 取默认 **Clear**，配合 `BeginOffscreenPass(..., &depthClear, ...)`
  ⇒ **它每帧把整个深度附件清掉再全屏重写**。因此"硬光栅排在深度解析之前"按原样不成立
  （深度会被清掉/覆盖），除非把解析 PSO 改成 `Load` + `LessEqual`（回归面大）。
  推荐次序（两个遮挡方向都正确，且不动 §14.30 的解析 PSO）：
  `GB_Clear → 硬光栅(LessEqual+depthWrite，写 4 张 GBuffer + 深度) → 从 D32 点采样播种深度键
   → 软光栅第 1 趟(InterlockedMin) → 第 2 趟(等值复检写色) → 既有深度解析`。

- **判据 ⑦/⑧ 有已知抖动，单次结果不足以定罪**
  同一份未改动代码上实测过三次完整 sweep：`log_baseline_r20.txt`（判据 ⑦ FAIL）、
  `log_r20_final.txt`（判据 ⑧ FAIL，但 8b 仍 `unexpected=0 missing=0`、`PIC CMP: PASS`）、
  `log_r20_final2.txt`（全 PASS）。⇒ 复验必须**跑 ≥2 次**，并把失败落在 8a/8b/8c/8e 的哪一小节
  原文贴出来，再判断是否由本次改动引起。

- **校验层基线**：`vuid_lines off=41 on=42 delta=1`、`distinct VUID types new=0`
  ⇒ 新 mesh 管线**不得引入新的 VUID 类型**。

**判据 ④ 的一处既有漂移裁决（2026-09-20，与 Nanite 无关）**
- 现状：`aq_def` 与基线 `s37fin2` 相比，**17 项转储逐位一致**，只有
  `lumen_irradiance` 与下游 `prov6_*` 共 5 项不同（`maxULP=6`、`maxAbs=1.5e-4`、
  `meanAbs=2.8e-8`、0.5% 像素）。
- 判定依据：① 判据 ② 显示**同构建背靠背 0 项差异** ⇒ 不是运行噪声；② 开关关闭档 pass 集合与
  指纹冻结、且关闭/开启转储逐位一致 ⇒ Nanite 模块不注册任何 pass，不可能改动这 5 项；
  ③ 本次工作**未触碰任何 Lumen 文件**。结论：这是 s37fin2 时代构建与当前构建之间，Lumen 屏幕探针 6
  辐照度路径的**既有数值残差**（很可能是该路径的未初始化/时序相关读回，另立项追）。
- 处理：判据 ④ 对**且仅对** `lumen_irradiance`/`prov6_*` 这一族给出硬上界容差
  （`maxULP ≤ 8` 且 `meanAbs ≤ 1e-6`）；其余转储仍要求**逐位一致**，所以真正的几何/光照回归
  一定会 FAIL。④ 的输出会显式打印"容差族里有几项"，不允许静默放过。

**核验时间**：§14.1 的代码引用为 **2026-09-19（本次评审）** 逐条核对；§14.11 的判据 ⑥ 与
④ 漂移裁决为 **2026-09-20（任务 1–2 实施时）** 实测；§14.11 的任务 22 分流验收口径为
**2026-09-21** 依既有脚本判据与当日实测读数拟定，其中"实现后期望"一列须在任务 22 落地后回填实测值。

### 14.12 接手须知（新会话从这里开始）

> 本节专为"清空对话上下文、另起一个新会话来实施 Nanite"而写：**凡是只存在于上一轮对话里、
> 重启就会丢的环境状态都记在这里**。开始前请按 ①②③ 逐条过一遍。

**① 分支与起点**
- `nanite` 分支已存在，起点 `main`（`f839bc6`）。提交历史（按时间）：
  `a453d50` 新增 07.Nanite 示例（自 06.GILab 拷贝基线）→ `b0c6fe9` Nanite 模块骨架 →
  `19d088d` 延迟管线门控与独立开关 → `8a6ec0c` 07.Nanite 样例面板与 cfg 往返。
- 沿用 lumen 的做法：**按逻辑拆分中文提交**，不自动 push。

**② 配置状态：`Content/Config/06_GILab.cfg` 会漂移（**必须处理**）**
- 该文件**不被 git 跟踪**（`git ls-files Content/Config` 为空），所以这种漂移**不会**在 `git status` 里报警。
- 成因：任何一次**未设 `HE_GILAB_CONFIG`** 的直接运行，退出时都会把相机位姿与 GI 权重回写进去。
  2026-09-20 01:23 的一次这样的运行把它改成了 **Lumen solo 且相机移位**。
- **修复办法（2026-09-20 已执行）**：`build/verify/chk_s37fin2.cfg` 就是产出基线转储的那份 cfg，
  直接覆盖回基础 cfg 即可（`Copy-Item` 一条命令）。当时实际不同的只有 5 个键：
  `ae_enabled`、`cam_pitch`、`cam_yaw`、`gi_blend_diffuse_w0`、`gi_blend_diffuse_lumen`。
- **注意**：早期版本的本节表格曾把验收基线写成 `gi_blend_diffuse_lumen=0.0`，**那是错的** ——
  基线 `s37fin2` 实际用的是 `lumen=1.000000`（`chk_s37fin2.cfg` 为准）。另外**相机位姿也是口径的一部分**：
  相机不一致时判据 ④ 会报 11 项差异（几何全错位），必须用上面的办法复原，不要靠"改权重"猜。
- **纪律**：永远用 `HE_GILAB_CONFIG` / `HE_NANITE_CONFIG` 指向私有副本跑 exe
  （`build/verify/lumen_smoke.ps1`、`nanite_smoke.ps1` 已经这么做）；直接运行 exe 会回写基础文件。
- `Content/Config/06_GILab_imgui.ini` 同时被回写（面板布局，无害）；若希望新 Nanite 面板出现在
  默认位置，删掉它即可。


**③ 验收与基线**
- 一条命令：`powershell -NoProfile -ExecutionPolicy Bypass -File build\verify\acceptance_sweep.ps1`
  → 期望 `ACCEPTANCE SWEEP: PASS`（判据 ①白炉 ②背靠背 ③关 Lumen ④默认预设 ⑤单测 **⑥开关不变式**；
  `-OnlyNanite` 只跑 ⑥）。判据 ⑥ 已于任务 2 落地，定义与冻结指纹见 §14.11。
- `-Baseline` 默认 `s37fin2`，其转储在 `build/verify/gi_s37fin2_*` —— **不要删**，删了判据④会"跳过"而不是判定。
- 抖动族（**允许不同**）：`prov0_ao_*`（SSAO）、`hdr`、`radiance`；判据是"抖动族之外 ≤2 个 f16 ULP"。
- 磁盘：`build/verify` 曾达 **92.5 GB / 16668 文件**（其中 `gi_*` 转储 88.95 GB）；清理时保留
  `s37fin2` 那组；`chk_*.cfg` 可整批删（脚本每次重建）。

**④ 构建与缓存的纪律（不需要清缓存）**
- CMake 缓存、`build/Engine/Shader/Shaders/*.spv(.h)`、MSBuild/IFC 缓存都会随 `CMakeLists.txt`
  变更自动重跑/重建；新增 `Engine/Render/Nanite/` 只需把文件加进 **`Engine/Render/CMakeLists.txt`
  的显式列表**。
- **新 shader 必须登记进 `Engine/Shader/CMakeLists.txt` 的 `COMP_SLANG`**（仓库用显式列表、
  不能用 glob）——否则"看起来编了、其实没编"（本仓库已有过同类教训）。
- 重建命令：`cmake --build build --config Release --target 06.GILab`；单测：`--target HugEngineTests`
  然后 `build\bin\Release\HugEngineTests.exe`。

**⑤ 已完成的任务与下一步**
- **任务 1（模块骨架 + 独立开关，N0）已完成**：`Engine/Render/Nanite/` 12 个文件；开关三层
  （CVar `r.Nanite.Enable` 默认 0 → cfg 键 `nanite_enable` → `NaniteSettings::enabled` 真值）；
  帧图 GBuffer 段的唯一门控开启时只注册 `Nanite_Noop`。验收实测：关闭档 pass 集合与指纹不变，
  开启档只多一项、画面逐位不变。
- **任务 2（开关不变式守卫）已完成**：判据 ⑥ 进 `acceptance_sweep.ps1`，并做过负向验证
  （破坏开关 ⇒ FAIL，还原 ⇒ PASS）。
- **下一步 = 任务 3**（模块自持的"计数 → 间接绘制"链）：在 `NaniteCull` 内写计数缓冲 +
  `IndirectCmdBuf`，绘制端用 `DrawIndexedIndirectCount`，**不改** `GPUCulling`；
  验收 = 假数据下"计数为 k ⇒ 恰好画 k 次"且读回计数与绘制一致。
- **纪律**：先测量再改（§14.1 的每一项都是可复核的代码事实）；**不要**先动 cluster/软光栅，
  阶段 0 是硬前置（否则 N2/N3 的产物没有消费者）；每一轮结束前跑一次
  `acceptance_sweep.ps1`（至少 `-OnlyNanite`）确认没有回归。

### 14.13 任务 3 实施记录与一处计划修正（2026-09-20）

**① 计划盲点（§14.1 应补一行）**：§14.5 与任务 3 都要求"模块自持 `DrawIndexedIndirectCount` 链"，
但 2026-09-20 实测：**RHI 层没有这个入口**（`Engine/RHI/RHI/CommandList.h:104` 只有
`DrawIndexedIndirect(buffer, offset, drawCount, stride)`，`Engine/RHI/Vulkan/VulkanCommandList.cpp` 里
也只有 `vkCmdDrawIndexedIndirect`），且**设备创建未启用** `VkPhysicalDeviceVulkan12Features::drawIndirectCount`
（全仓库零引用 `VkPhysicalDeviceVulkan12Features`）。结论：**这条链不是"接线"就能通的，必须先扩展 RHI**。

**② 授权后的最小 RHI 扩展（已落地，§14.6 接触面因此 +3 文件）**
- `Engine/RHI/RHI/CommandList.h`：新增纯虚
  `DrawIndexedIndirectCount(buffer, offset, countBuffer, countOffset, maxDrawCount, stride)`。
- `Engine/RHI/Vulkan/VulkanCommandList.{h,cpp}`：实现 `vkCmdDrawIndexedIndirectCount`；特性缺失或缓冲为空时
  打印中文告警并跳过（不崩）。
- `Engine/RHI/Vulkan/VulkanDevice.{h,cpp}`：查询并启用 `drawIndirectCount`，新增 `SupportsDrawIndirectCount()`。
- **踩到的两个 Vulkan 规则（后来者别再踩）**：① `VkPhysicalDeviceDescriptorIndexingFeatures` /
  `BufferDeviceAddressFeatures` / `TimelineSemaphoreFeatures` / `ShaderFloat16Int8Features` 与
  `VkPhysicalDeviceVulkan12Features` 是**别名结构体**，同时入 pNext 链触发 `VUID-VkDeviceCreateInfo-pNext-02830`；
  启用 `VK_EXT_descriptor_indexing` 时还必须 `descriptorIndexing=VK_TRUE`（`-02833`）。做法：把这 4 个结构体
  **合并进一个 `Vulkan12` 结构**，逐字段沿用同一份查询结果（启用集合不变）。② 片元着色器写 SSBO 需要
  `fragmentStoresAndAtomics`（否则 `VUID-RuntimeSpirv-NonWritable-06340`）。修完 `vuid_lines` 回到基线 41。
- `IRHICommandList` 的实现者只有 `he::rhi::VulkanCommandList`（Tests 里没有 mock 后端），所以接口扩展只需补一处。

**③ 任务 3 交付（模块自持的「计数 → 间接绘制」链）**
- 新增 `Engine/Shader/Shaders/Nanite/{Nanite_Cull.comp, Nanite_Raster.vert, Nanite_Raster.frag}.slang`，
  三个都**显式登记**进 `Engine/Shader/CMakeLists.txt`（`COMP_SLANG`/`VERT_SLANG`/`FRAG_SLANG`），
  `build/Engine/Shader/Shaders/Nanite/*.spv.h` 已生成。
- `NaniteTypes.h` 加与 Slang 共享的 POD：`NaniteIndirectCommand`（20B，与 `VkDrawIndexedIndirectCommand`
  二进制兼容）、`NaniteFakeCluster`（16B），`static_assert` 钉住尺寸/偏移。
- `NaniteCull` 自持四个缓冲（假簇 / 间接命令 / 计数 / 已光栅化计数）+ compute PSO + 每帧重置；
  `NaniteRaster` 自建 1×1 R8 目标（**不碰可见画面**）+ 图形 PSO + `DrawIndexedIndirectCount`；
  `NaniteRenderer` 注册 `Nanite_Cull` 与 `Nanite_Raster`，**移除占位的 `Nanite_Noop`**。
- 配置：`NaniteSettings::fakeClusters`（默认 6）+ cfg 键 `nanite_fake_clusters`；`r.Nanite.FakeClusters` 只作启动默认。

**④ 验收证据（2026-09-20，本人复跑）**
- 关闭档：`passes_per_frame=12`、`nanite_passes=0`、`vuid_lines=41`、
  指纹 `1C15AB72E688B5302332AEC391C41A5FE2B4D9512258CCDCD5D3E9D7E8F5390D`（**与冻结值一致**）。
- 开启档：`passes_per_frame=14` = 既有 12 个（相对顺序不变）+ `Nanite_Cull` + `Nanite_Raster`；
  去掉 `^Nanite` 行后与关闭档逐行相同（判据 ⑥b `preexisting_set_changed=False`）。
- **核心验收"计数为 k ⇒ 恰好画 k 次"**（真实 GPU 读回，一行日志）：
  `fake_clusters=0/3/17 → count_buffer=0/3/17、indirect_cmds=0/3/17、rasterized_clusters=0/3/17`（**X=Y=Z=N**，含 k=0 边界）。
- 画面不变：开启/关闭档转储逐位比较 `same=17`、`must_same_diff=0`（仅 3 个抖动族文件不同）。
- `vuid_lines=41`（= 基线，无新增）；`HugEngineTests` 237/5792 全绿；
  全量六条判据 `ACCEPTANCE SWEEP: PASS`（①白炉 1.0000 ②`max ULP=0` ③`lumen_passes=0` ④严格差异 0、缓存留族 5 ⑤SUCCESS ⑥PASS）。

**⑤ 判据修正**：判据 ⑥b 原先把"开启档去掉 `^Nanite_Noop`"与关闭档比较，任务 3 新增了更多 `Nanite_*` pass
后该写法会误报，已泛化为"去掉所有 `^Nanite` 开头的 pass 行后比较"，并要求开启档至少 1 个 `Nanite` pass。
（脚本仍在被 gitignore 的 `build/verify/` 下，**权威记录是本节与 §14.11**；指纹或判据变化时先改文档再改脚本。）

**⑥ 风险**：设备不支持 `drawIndirectCount` 或 `fragmentStoresAndAtomics` 时，只打印中文告警并跳过绘制端，
该档会失去"已光栅化簇数"计数（不崩溃、不影响既有画面）；本机已确认两项均已启用。

### 14.14 任务 4 实施记录：GBuffer UAV（A1）与同帧读到（2026-09-20）

**① A1 落地**：`GBufferRenderer.cpp` 的 8 张颜色目标各**只增** `UnorderedAccess`（实际是 7×RGBA16F + 1×RG16F velocity）。
**深度不加 UAV**：实测本机 NVIDIA RTX 4060 的 `D32_SFLOAT` 支持 `STORAGE_IMAGE_BIT`，但同机 AMD 核显不支持
（`D32_SFLOAT_S8_UINT` 在 NVIDIA 上也不支持）⇒ "compute 写深度"不可跨厂商。裁决：颜色走 A1，深度继续走附件路径；
A1/A2 的完整裁决已写回 §14.5（A2 = 自建 VisBuffer，仅在确需跨厂商解耦材质/深度时升级）。

**② 同帧读到怎么证的**：新增 `Nanite_TestWrite.comp.slang`（8×8 棋盘写 GBuffer albedo，深格 0.25/浅格 0.75,0.50,0.25），
由 cfg 键 `nanite_test_write`（默认 **0**）+ 面板勾选框控制；帧图新增**同一开关**守卫的第二处注册点
`m_Nanite.AddPostGBufferPasses(rg, naniteGB)`，位置在 `GB_Clear` **之后**、所有 GBuffer 消费者之前
（将来也是任务 26 调试可视化的落点；关闭档一个 pass 都不注册——已实测"只开 `nanite_test_write`、主开关关"时指纹仍是冻结值）。

**③ 证据（本人复跑）**
- 关闭档：12 pass、指纹 `1C15AB72E688B530…`（冻结值）、`vuid_lines=41`。
- 开启档（测试写关）：14 pass，与关闭档逐位比较 `same=17`、`must_same_diff=0`（仅 3 个抖动族文件不同）。
- 测试写档：15 pass，`Nanite_TestWrite` 位于索引 5（`GB_Clear` 之后、`Decal_Project/SSAO/Lighting` 之前）；
  `gi_nanite_tw_albedo.f16` 统计 `min=0.0000 mean=0.2812 max=0.7500 rgb_mean=(0.5000,0.3750,0.2500)` —— 与写入图案逐位吻合。
- **Lighting 同帧读到了**：`cmp_dumps.py nanite_on nanite_tw` 的 `hdr` 为
  `diff_px=6218585 maxAbs=1.66 meanAbs=0.0317`，而 `nanite_off` vs `nanite_on` 的运行间抖动只有
  `hdr meanAbs=1.32e-06`（相差约 **24000 倍**）。若 Lighting 读的是写入前的内容，差异必然落回抖动量级。
- 单测 237/5792 全绿；全量六条判据 `ACCEPTANCE SWEEP: PASS`。

**④ 判据②也扩了 probe-6 有界容差（同一族、同一签名）**：任务 4 期间判据② 从 `0 ULP` 变成
`6 ULP / 5 个文件`，而这 5 个文件正是 §14.11 记录过的 `lumen_irradiance` + `prov6_*`
（`maxULP=6 maxAbs=1.5e-4 meanAbs=2.6e-8`，0.5% 像素）⇒ 该族在**运行间非确定**（同一轮内先 0 后 6），并非 Nanite 引入。
处理与判据④ 一致：仅对该族给出硬上界容差（`maxULP <= 8` 且 `meanAbs <= 1e-6`）并**显式打印命中项数**，
其余转储仍严格 `<= 2 ULP`。**若该族幅度超过上界，一律按回归处理。**

### 14.15 任务 5 实施记录：objectIndex 分区契约（2026-09-20）

**① 测量结论（`gb_lightmapkey` 到底编码了什么）**
- 目标：MRT7 `kGBufferSlotLightmapKey`（RGBA16_FLOAT）。唯一写入点 `Engine/Shader/Shaders/GBuffer/GBuffer.frag.slang:134`，
  编码 = `float4(页内uv.xy, objectIndex, 0)`（`.z` 就是 objectIndex，来自 push constant）。
- **全部解码点**：① `Tools/gi/lightmap_key_check.py:64/82-92`（唯一真正解析页号者，硬编码 `page < 1024`）；
  ② `Engine/Shader/Shaders/Lighting/DeferredLighting.frag.slang:30` 只声明 `u_LightmapKey` 绑定、**从未采样**；
  其余（`DecalPass`、`GBufferRenderer_CPU/GPU`、帧图）只是搬运附件，不解码。
- **越界风险**：按普通段容量索引 `u_Objects[...]` 的着色器有 7 处，但索引全部来自 push constant / `SV_InstanceID`，
  **没有一处来自 `gb_lightmapkey`** ⇒ 今天不存在显存越界路径。真正的风险点是离线检查工具 ①：任务 18 一旦出现
  Nanite 页，它会直接 FAIL —— 最小修法（按段分类：`page<1024` 普通段 / `[1024,2048)` 取 Nanite 局部索引 / 其余 FAIL）
  已写进注释，留给任务 18 与混排运行时校验一起做。

**② 分区契约定稿**（`NaniteTypes.h`，含 `static_assert`）

| 段 | 起止 | 容量 | 依据 |
|---|---|---|---|
| 普通段 | `[0, 1024)` | 1024 | `= kGPUMaxObjects = MAX_OBJECTS`，与 `GPUObjectData` 缓冲一致 |
| Nanite 段 | `[1024, 2048)` | **1024**（任务 1 预置的 16384 已修正） | binary16 精确整数上限 2^11 = 2048 |
| 哨兵 | `0xFFFFFFFF` | — | 超出两段、段判定 Invalid、binary16 为 NaN，三重不冲突 |

**③ 一处契约修正（必须记住）**：Nanite 段容量 `16384 → 1024`。理由：`gb_lightmapkey` 是 RGBA16_FLOAT，
页号 2049 会被量化成 2048，若不收窄则 Nanite 段 94% 的槽位无法作为合法页号。要突破 2048 必须换 MRT7 格式
（RGBA32F 或拆通道），那属于 GBuffer 的任务。回退只需改这一个常量（单测按常量自适应）。

**④ 验收证据（本人复跑）**
- `Tests/TestNaniteTypes.cpp` 新建并登记进 `Tests/CMakeLists.txt`；单测 **237 → 246 例**（断言 5792 → 22289），全绿。
  覆盖：段边界逐点（0/1023/1024/2047/2048/0xFFFFFFFF）、局部↔全局往返、哨兵不冲突、分配器容量与回收复用、
  共享 POD 尺寸偏移、以及"Nanite 段索引不被普通段解码接受"的硬约束。
- 关闭档 12 pass、指纹 `1C15AB72E688B530…` 不变；开启档 14 pass；两档 `gb_lightmapkey` 转储
  **sha256 完全相同**（`E74ED2A042E54E089811B8C3DE524C588D01927D012DDC3FE6B1AAF007CA1C6B`），非抖动族 `must_same_diff=0`。
- 启动日志新增一行中文分区说明；`vuid_lines=41`；全量六条判据 `ACCEPTANCE SWEEP: PASS`。

**⑤ 未完成部分（明确留到任务 18）**：真正的"混排场景运行时校验"（Nanite 真的往 MRT7 写页号后逐位核对）
与 `Tools/gi/lightmap_key_check.py` 的按段分类修改。本任务只保证契约、边界函数与分配器行为可测，
以及既有解码路径逐位不变。
### 14.16 任务 6 实施记录：mesh PSO 真正接入（阶段 0 收口，2026-09-20）

**① 前置核实（任务 6 的硬前置：mesh 特性确实启用）**：`VulkanDevice_MeshShader.cpp:24-45` 判扩展、
`:48-70` 查 `maxMeshOutputVertices/Primitives=256、maxMeshWorkGroupInvocations=128`、`:77-87` 加载
`vkCmdDrawMeshTasksEXT`；`VulkanDevice.cpp:457-465` 构造 `VkPhysicalDeviceMeshShaderFeaturesEXT{taskShader,meshShader}`
并在支持时 push 扩展、`:615-616` 链入 pNext、`:155` 写入 `caps.supportsMeshShaders`；
`VulkanPipeline.cpp:485-486` 即 `PipelineStateDesc::meshShader` 分支；`VulkanCommandList.cpp:932-940` `DrawMeshTasks`。
运行期日志：`Mesh Shader 扩展已启用: VK_EXT_mesh_shader` / `Mesh Shader 扩展函数加载成功`。

**② 交付**：新增 `Nanite_MeshTest.mesh.slang`（`[numthreads(4,1,1)]`、`[outputtopology("triangle")]`，
**真调 `SetMeshOutputCounts(4,2)`**，覆盖 NDC 的四边形两个三角形，不是 §14.1 里那个 `0,0` 桩）+
`Nanite_MeshTest.frag.slang`（原子计数并写 1.0）；`.mesh.slang` 走 **`MESH_SLANG` 显式列表**（`Engine/Shader/CMakeLists.txt:218-224`）。
模块内建**最小 mesh PSO**（`NaniteRaster::EnsureMeshTestResources`，写模块自己的 1×1 R8 目标，**从不碰 GBuffer**）；
新 pass `Nanite_MeshTest` 由 cfg 键 `nanite_mesh_test`（默认 **0**）控制，注册在 `AddPasses`（GBuffer 段前）。

**③ 途中修掉的一处新增 VUID（值得记住）**：原实现 mesh 档 `vuid_lines=52`，多出 10 条
`VUID-VkImageMemoryBarrier-oldLayout-01211` —— 原因是 `CopyTextureToBuffer` 拷完会**无条件**把真实布局还原成
`SHADER_READ_ONLY_OPTIMAL`（`VulkanCommandList.cpp:756-766`），而模块的小目标缺 `SAMPLED` 位。
修法：给该私有目标 usage **只增**一个 `ShaderResource` 位（目标从不被采样、与可见画面无关）⇒ 回到 41 行、`01211` 计数 0。

**④ 验收证据（本人复跑）**
- 关闭档：12 pass、指纹 `1C15AB72E688B530…`（冻结值）、`vuid_lines=41`。
- mesh 自证档（`nanite_enable=1;nanite_mesh_test=1`）：15 pass，`Nanite_MeshTest` 在 `Nanite_Raster` 之后、
  `GB_Clear` 之前，既有 12 pass 相对顺序不变；日志恰好一行
  `[Nanite] mesh_pso=ok meshlet_outputs=2 target_max=255`（两个独立读数都 > 0 ⇒ **非空**，分别来自片元原子计数与
  1×1 目标的真实 GPU 读回）；`01211` 计数 0、`vuid_lines=41`。
- off vs mesh 转储逐位比较：`same=17`、`must_same_diff=0`（仅 3 个抖动族文件不同）。
- 单测 246/22289 全绿；全量六条判据 `ACCEPTANCE SWEEP: PASS`。

**⑤ 阶段 0 收口**：任务 1–6 全部完成。这意味着后续 N1/N2/N3（任务 7 起）要用的四件基础设施都已就位：
独立开关与不变式判据、模块自持的计数→间接绘制链、GBuffer UAV（A1）、objectIndex 分区契约、
以及 mesh 光栅的 PSO 通路（任务 22 直接复用）。

### 14.17 任务 7 实施记录：`.nanite` 数据格式定稿（2026-09-20）

**① 四处不一致的裁决（逐条给出规则依据；已在 §8 就地标注"定稿（任务 7，2026-09-20）"）**

| # | 分歧（原文位置） | 裁决 | 规则依据 |
|---|---|---|---|
| 8 | 文件头 96B vs 128B（§8.3 表格 vs `NanitePack` docstring `[NaniteFileHeader 128B]`） | **96B** | ① §8.3 已写"实现时以 96B 为准"；② 96 = 6×16，天然 16B 对齐且自包含；③ Python 实际写的是 `<32x>`（= `u32 _reserved[8]`），字段累加正好 96B ⇒ 128B 是 docstring 笔误 |
| 7 | 顶点 16B vs 12B（§8.4 `NaniteVertex` 4×u32 vs `NanitePack` 12B/顶点、`NaniteUpload` 按 12B 读） | **16B** | ② 规则②优先"16 字节对齐且自包含"：量化偏置落在记录内（第 4 个 u32 命名 `quantBias`，不是 `_pad`）；12B 版本既不 16B 对齐、偏差量也无处安放 |
| 9 | 量化偏置（编码端无符号 0…1023 vs 解码端 `-512` SNORM） | **编码端补 `+512`** | ③ 单轴 `raw = clamp(round(...)) + quantBias`、解码 `v = bboxMin + (raw - quantBias)/511 × maxExtent`，严格互逆（误差 ≤ maxExtent/1022）；偏置取自记录内的 `quantBias` ⇒ 自包含 |
| 1 | `coneData` vs `coneAxisAngle`（§8.1 两版结构体） | **`coneAxisAngle`**（xyz=单位轴，w=cos 锥半角；`w=-1` = 无锥哨兵） | ① 两案都未被标"权威"；② 两者同为 float4/16B，尺寸与省法都不分高下 ⇒ 按规则②的"自包含"意图取"能唯一确定解码、不依赖外部约定"的那个；`coneData` 只有名字、写不出解码器 |
| 6 | 索引 3×u16 进 `u32[2]` vs 1 索引 1 个 u32（§8.5 注释 vs `NanitePack`/`NaniteUpload`/`Nanite_SoftRaster` 三处 u32） | **3×u16 进 `u32[2]`（8B/三角形）** | ② 两个候选都**不是** 16B 对齐 ⇒ 规则②不裁决；③ 落到"取更省方案"⇒ 8B < 12B（索引带宽 −33%），且 §8.4 的"每簇 ≤128 顶点"让簇内局部下标只需 7 位、u16 绰绰有余；一簇 64 tri = 512B 天然 16B 对齐 |

**② 派生定稿（为了自洽必须一起定的两件语义）**
- `indexCount` 保持"索引**总数**"（必须是 3 的倍数）；索引段字节数 = `ceil(indexCount/3) × 8` 再向上
  取整到 16B；`cluster.triangleOffset` 的单位是**三角形**（× 8B = 索引段字节偏移）。
- 索引是**簇内局部**顶点下标（`[0,127]`），全局顶点下标 = `cluster.vertexOffset + local` —— 这也解释了
  §8.1 里 `vertexOffset` 为什么必需。
- 段偏移**不落盘**：`[头部 96B][簇 64B×n][顶点 16B×n][三角形 8B×n][材质 8B×n][LOD 4B×n]`，
  每段起点 16B 对齐、长度向上取整到 16B，由计数纯函数推导（`NaniteFileLayout`）。

**③ 交付**
- `Engine/Render/Nanite/NaniteTypes.h`（RHI-free、可被 Scene 侧 include）新增：
  `NaniteFileHeader`(96B) / `NaniteConeAxisAngle`(16B) / `NaniteClusterRecord`(64B) /
  `NaniteVertex`(16B，含 `quantBias`) / `NanitePackedTriangle`(8B) / `NaniteMaterialRecord`(8B) +
  段对齐与步长常量；量化编解码 `NaniteQuantizePositionAxis`/`NaniteDequantizePositionAxis`、
  位域 `NanitePackR10G10B10A2`/`NaniteUnpackR10G10B10A2`、索引 `NanitePackTriangle`/
  `NaniteTriangleIndex0..2`/`IsValidClusterLocalVertexIndex`、cone `IsValidConeAxisAngle`/
  `NaniteConeHalfAngleRadians`。**每个结构体都有 `sizeof` / `offsetof` 的 `static_assert`**，
  并写明"与将来 Slang 的 `NaniteTypes.slang` 共享、改布局必须同步 §8 与本文件"。
- **校验函数**（RHI-free、可单测）：`NaniteFileLayout` + `TryBuildNaniteFileLayout()`（计数 → 段表、
  溢出/对齐兜底）+ `ValidateNaniteFile()`（魔数/版本/索引数是 3 的倍数/各段不越界/截断）
  + 布尔外壳 `ValidateNaniteHeader(const void*, size_t)`；失败原因枚举 `NaniteFileError` 可读。
- `Tests/TestNaniteTypes.cpp` **扩展**（未新建第二套测试文件）8 个 `TEST_CASE`，覆盖：头部逐字段与尺寸、
  簇记录/cone 字段尺寸偏移、顶点记录与偏置落点、量化偏置往返（盒内逐点 + 半步误差上界 + 越界夹取 +
  退化轴）、索引编码位边界与簇内下标语义边界、cone 解码（单位轴/cos 半角/无锥哨兵/反例）、
  段表推导（含 36 组计数的对齐属性循环）、校验函数正例与反例（空指针/截断/魔数错/版本错/索引数错/越界/尾部多余）。
- `Tests/CMakeLists.txt` **无需改动**（`TestNaniteTypes.cpp` 早在任务 5 已登记）。
- 设计文档：§8.1 / §8.3 / §8.4 / §8.5 写回裁决并改写冲突旧表述；§0.2 不一致清单 #1/#6/#7/#8/#9 标注定稿；
  §12 Task 3/4 的落地提示与打包草图同步（128B→96B、12B→16B、4B/索引→8B/三角形）。

**④ 验收证据（本人复跑）**
- `cmake --build build --config Release --target HugEngineTests` ⇒ `EXIT=0`；
  `build\bin\Release\HugEngineTests.exe` ⇒ **254 例 / 23195 断言全绿**（任务 5/6 时为 246 例 / 22289 断言，+8 例）。
- `cmake --build build --config Release --target 07.Nanite` ⇒ `EXIT=0`。
- `build\verify\nanite_smoke.ps1 -Tag nanite_off` ⇒ `passes_per_frame=12`、`vuid_lines=41`、
  `passlist_sha=1C15AB72E688B5302332AEC391C41A5FE2B4D9512258CCDCD5D3E9D7E8F5390D`（= 冻结值）。
- `build\verify\acceptance_sweep.ps1 -OnlyNanite` ⇒ 6a off=12 pass、`nanite_leak=0`、sha `1C15AB72E688B530`；
  6b on=14 pass、既有集合未变；6c 抖动族外 `differing_outside_jitter=0`；**`ACCEPTANCE SWEEP: PASS`**。

**⑤ 边界声明**：本任务**只定稿格式**（布局/位域/编解码约定），不产出任何离线工具或运行时数据：
任务 8 的 cluster 切分、任务 10 的量化打包（含 #13 normal/UV）、任务 12 的上传/加载均未开始；
`NaniteUpload.cpp` 仍是任务 1 的生命周期桩。渲染路径一行未改（关档 12 pass 指纹逐位不变）。
### 14.18 任务 8 实施记录：离线簇切分（meshopt_buildMeshlets，2026-09-20）

**① 前置**：`meshoptimizer v0.22` 已 vendored（`Engine/External/meshoptimizer`，根 `CMakeLists.txt:57 add_subdirectory`），
但 `HugEngineRender`/`HugEngineTests` 都**没有链接过它** —— 本任务补了两处 `PRIVATE` 链接。仓库**没有**离线打包脚本宿主
（设计里提到的 `NanitePack.py` 不存在），故按默认项把 CPU 侧构建代码放进模块的 `NaniteUpload.{h,cpp}`（资产准备侧），
**不新增文件、不新建 Python 工具**，保持 §14.3 的模块边界。

**② meshopt 真实接口约束（核实自 vendored 源码 `src/clusterizer.cpp:538-550`，写进代码注释与 static_assert）**
- `meshopt_buildMeshlets(meshlets, meshlet_vertices, meshlet_triangles, indices, index_count, positions, vertex_count, stride, max_vertices, max_triangles, cone_weight)`；
- assert 约束：`max_vertices ∈ [3,255]`（**不是 256**）、`max_triangles ∈ [1,512]` 且**必须被 4 整除**、`stride` 为 4 的倍数。
  设计给的 128/64 恰好合法（64 是 4 的倍数），直接传 128/64。
- 缓冲容量必须用官方最坏情况 `meshopt_buildMeshletsBound()` 推导（已照做）。
- `meshlet_Meshlet.triangle_offset` 是**字节**偏移且每簇 4B 对齐；`meshlet_triangles` 是 u8（每三角形 3 字节）。
- `meshopt_computeMeshletBounds()` 的 `cone_cutoff` 实现就是 `sqrt(1-cos²)`，**正是**任务 7 `NaniteConeAxisAngle::cosHalfAngle`
  要的量，直接落盘无需角度换算。**任务 15 的锥剔除测试请沿用 `dot(dir, axis) >= cosHalfAngle`**（哨兵 −1 表示恒不剔除）。

**③ 落盘口径（不伪造语义）**：填真值的是 `boundsCenterRadius`、`cone`、`triangleOffset/triangleCount/vertexOffset`；
留 0 并逐行注明归属的是 `materialID`（任务 10/12）、`maxParentLODError`（任务 9/10）、`childClusterOffset/childCount`（任务 9）。
顶点表跨簇允许重复（"局部下标 + vertexOffset"契约的直接代价），每簇局部顶点数另放平行数组。`cone_weight` 取 0（锥质量调参属任务 15）。
"无锥"统一按任务 7 哨兵编码（`axis=0, cosHalfAngle=-1`）并单列 `noConeClusterCount`，不伪造单位轴。

**④ 验收证据（本人复跑）**
- 单测 **254 → 264 例**（断言 30199），新增 `Tests/TestNaniteBuilder.cpp` 10 个用例（测试目标直接编译 `NaniteUpload.cpp`，
  顺带成为"RHI 依赖闯进该 .cpp 就编译失败"的纪律钉子）。
- 簇统计（测试 MESSAGE 原文）：6×6 网格（72 tri）→ 簇数 2、最大每簇 64 tri / 44 vert；**32×32 网格（2048 tri）→ 簇数 32、
  最大每簇 64 tri / 51 vert、退化簇 0**；1 tri / 18 tri / 64 tri / 立方体 12 tri 各 1 簇、退化簇 0。
  覆盖完整性用例证明 2048 个输入三角形的有序三元组多重集与产物**逐项相等**（不丢不重、绕序保留）；可复现用例两次切分逐位一致。
- 关闭档 12 pass、指纹冻结 `1C15AB72E688B530…`、`vuid_lines=41`；全量六条判据 `ACCEPTANCE SWEEP: PASS`。

**⑤ 遗留（交给后续任务）**：顶点表跨簇重复（32×32 → 2048 tri 产生 1471 条顶点表条目），去重/共享属任务 9 的 DAG；
量化打包属任务 10；`materialID` 与 LOD 误差字段属任务 9/10/12。
### 14.19 任务 9 实施记录：LOD 链与 DAG 去重（2026-09-20）

**① meshopt 简化接口的真实约束**（核实自 vendored `src/meshoptimizer.h:375` 与 `src/simplifier.cpp:1814-1819`，v0.22 为 `MESHOPTIMIZER_API` 无需宏）
`meshopt_simplify(dst, indices, index_count, positions, vertex_count, stride, target_index_count, target_error, options, result_error)`：
`dst` 必须装下**最坏情况 `index_count`** 个索引（不是 target）；返回值恒为 3 的倍数且**可能达不到目标**（拓扑/误差所限）——
本实现据此终止链；`result_error` 是**相对**误差，取绝对值须乘 `meshopt_simplifyScale()`。选 `options = 0`
（**刻意不锁边界**：`meshopt_SimplifyLockBorder` 会让平面/薄壳网格减半失败，直接掐死 LOD 链）、`target_error = 1.0`。

**② LOD 链**：级 L 的簇组 → 合并回索引表 → `meshopt_simplify` 目标 = 上一级三角形数的一半 → 对结果重走任务 8 的切分。
三条终止条件任一命中即停：① 下一级目标 < 64（一个满簇 `kNaniteMinLODTriangles`）；② 简化未减少（返回 0 / ≥ 输入 / 非 3 的倍数，防死循环）；
③ 已达 6 级（`kNaniteMaxLODLevels`，与 §4.1 `max_levels=6` 对齐）。实测 2048 tri → **5 级**、1024 tri → 5 级、576 tri → 4 级、
128 tri → 2 级、72 tri / 1 tri → 只有 LOD0（显式终止①）。

**③ DAG 去重口径（关键设计决定）**
- **内容哈希用"簇内局部"几何**：位置词由任务 7 的量化编码（R10G10B10A2）+ 三角形打包构成，**原点取簇 AABB 中心**
  （而非 `meshopt_computeMeshletBounds` 的球心——实测该球心随簇内顶点顺序漂移，会让明明相同的平移副本去重不了），
  世界放置由 `NaniteClusterRecord::boundsCenterRadius` 承载 ⇒ **平移副本可共享内容**，这是 (a) 能过 10% 的前提。
- 规范键流顺序无关：`[顶点数][排序的位置词][三角形数][排序的三角形键（循环旋转字典序最小、保绕序）]`，
  FNV-1a 64 哈希 + **命中后再做全量键流比对**（杜绝碰撞误共享）。
- **共享的是内容**（`uniqueVertexWords`/`uniqueTriangles` 各一份），每条**出现**记录仍保留自己的包围球与顶点下标；
  父/子链接按出现记录给出（每个非根簇恰有一个父，父级更粗 ⇒ 天然无环）。

**④ 去重率（本人复跑，MESSAGE 原文）**：(a) 平铺的相同子网格 16×64 tri → **51.61%**；(b) 一般网格 32×32（2048 tri）→
**0.00%**（如实报告：单一连通、几何本身无任何重复内容，不是通路问题）；(c) 混合网格（2048 tri 地面 + 36 根重复柱）→ **27.21%**；
3×3 平铺 576 tri → 42.11%（其中 `unique=11 < 各级 unique 之和 12`，证明还存在跨级共享）。每级三角形严格减半
（2048→1023→510→255→126）、每簇 ≤64 tri、最大每簇顶点 110 ≤128、**退化簇 0**。

**⑤ 验收证据**：单测 **264 → 270 例**（断言 34413）全绿，新增 6 个 `NaniteDAG:` 用例（LOD 链、两类网格去重率、
链接自洽含"无孤儿/无环/每子一父"、共享内容与误差字段、两次构建逐位可复现、空网格与非法输入与 128 tri 边界）；
关闭档 12 pass、指纹冻结 `1C15AB72E688B530…`、`vuid_lines=41`；全量六条判据 `ACCEPTANCE SWEEP: PASS`。

**⑥ 必须记住的两条约束（后续任务会用）**
1. **任务 10 的解码口径必须沿用"簇内局部量化 + `boundsCenterRadius` + 网格最大范围"**，否则共享的 `vertexOffset`
   会让不同位置的簇读到同一份坐标（已写进 `NaniteUpload.h` 头注释）。
2. `maxParentLODError` = 该次简化的绝对误差（`result_error × simplifyScale`，同级共用，属保守上界），
   meshopt 报 0 时退几何兜底；**偏保守**（32 单位网格实测 22.19），方向安全（LOD 切换偏晚 = 偏细）。任务 15 的 LOD 选择直接用它。

**⑦ 已知限制（如实记录）**：LOD1 以上几乎不再去重 —— 因为本实现是**全局简化**（忠实 §4.1 的 `edge_collapse(lods[-1], 0.5)`），
meshopt 对互不相连的相同副本会给出逐副本不同的折叠顺序；真正 Nanite 的"按簇组分别简化"收益更大，属后续可选优化。
任务 8 的 `BuildNaniteClusters` 一行未改（仍只切簇、留 0 字段），DAG 版本在 `BuildNaniteClusterDAG` 里填真值。
### 14.20 任务 10 实施记录：量化与打包（2026-09-20）

**① 位置量化基准裁决（任务 7 的已知取舍在此结案）**：改为**簇 AABB 中心基准 + 吃满 10 位**。
编码 `signed = clamp(lround((v-origin)/range×1022), -512, 511)`、`raw = clamp10(signed + quantBias)`，解码 `v = origin + (raw-quantBias)/1022×range`。
- 乘数由任务 7 的 511 改为 **1022**（新增 `kNaniteVertexQuantFullScale`，`static_assert == 1022`），把 `[origin-range/2, origin+range/2]`
  映到有符号 `[-512,511]` ⇒ 1024 个码点全可用（实测极端簇 `raw ∈ [1,1023]`）；任务 7 的 `bboxMin` 口径只用上半段（等效 ~9 位）。
- **不会 clamp，且可证**：簇是网格子集 ⇒ 每轴 `|v-origin| ≤ 簇半轴长 ≤ range/2` ⇒ `|signed| ≤ 511`。函数内保留夹取作防御，
  并新增 `NanitePositionQuantizeClamps()` 把"无 clamp"变成可测读数：4 个测试网格全部 `positionClampCount = 0`。
- 精度步长 `range/1022`、往返误差 ≤ `range/2044`（比任务 7 再小一半），实测**恰好压在上界**：6→0.002935、32→0.015656、48→0.023483、68→0.033268。
- 与 §8.4 建议的 `center=(bboxMin+bboxMax)/2`、`halfExtent=maxExtent/2` **完全等价**（`signed/511×halfExtent == signed/1022×range`）；
  **§8.4 中"沿用 bboxMin 基准"的旧表述就此作废**（任务 7 的取舍项结案）。
- 口径钉死：抽出共用的 `ComputeMeshBounds()`，任务 9 的 DAG 哈希与任务 10 的打包共用同一函数；打包器还会把 DAG 的位置词**逐位重算比对**（`positionMismatchCount` 实测恒 0）。

**② 其它属性**：法线取**八面体 10+10 位**落在 `packedNormal` 的 x/y 域（z/w 留 0，不改 §8.4 的 R10G10B10A2 位域），
实测最坏 **0.227282°**（阈值 0.5°），不采用"重解释成 R16G16"（省 4 倍精度但需改三处位运算，收益不足）；
UV 取 **unorm16**（实测最大误差 7.689e-06 < 1/65535；half 在 (0.5,1) 的间距是本方案 32 倍），越界 UV 按 clamp 并单列 `uvClampCount`；
索引沿用任务 7 的 **3×u16 进 `u32[2]`**，新增 `IsNaniteTriangleIndexCount()` 把"必须 3 个一组"显式化；
材质 8B = 两个 bindless 纹理 ID（word0=albedo、word1=normal），补 `NanitePackMaterial/NaniteUnpackMaterial` 显式化字节位序。

**③ 打包产物** `PackNaniteClusters`：完整 `.nanite` 镜像 `[96B 头][簇×64B][顶点×16B][索引×8B/三角形][材质×8B][LOD×4B]`，
段偏移不落盘、由 `TryBuildNaniteFileLayout` 推导（每段起点 16B 对齐、长度向上取整），返回前用 `ValidateNaniteFile()` 自校验。
实测（单测断言逐项相等）：32×32（2048 tri、5 级）= 头 96 + 簇 3968 + 顶点 45376 + 索引 31696 + 材质 0 + LOD 32 = **81168 B**；
6×6（72 tri）= **1680 B**；平铺 3×3 = **14272 B**。LOD 段语义本任务定义为"该级第一个出现簇的下标"（任务 7 只定了 4B 步长与条数），任务 15 可直接用。

**④ pack / upload / shader 三处一致的达成程度**：C++（`NaniteTypes.h`）与**新建的仅供 include** 的
`Engine/Shader/Shaders/Nanite/NaniteTypes.slang` 已写同一套位域/公式/常量（位置乘数 1022、八面体折叠与符号约定、unorm16、
材质字段顺序、段步长与对齐），两侧互写同步指针注释；该 Slang 镜像**语法已用临时 harness 通过 `slangc` 编译验证**（未写进仓库）。
它**不在 `COMP_SLANG`**（不是 shader 入口，Engine/Shader/CMakeLists.txt 一行未改），**任务 18 接入时必须把它加进 `SLANG_INCLUDES`**（已写入文件注释）。
真正的端到端三处一致要到**任务 12**（CPU 字节 vs GPU 读回逐字节）与**任务 18**（shader 解码对照，需 push constant 传 `meshMaxExtent`）才能验证。

**⑤ 验收证据**：单测 **270 → 279 例**（断言 61954）全绿；关闭档 12 pass、指纹冻结 `1C15AB72E688B530…`、`vuid_lines=41`；
全量六条判据 `ACCEPTANCE SWEEP: PASS`（本次运行前遇到一次环境异常：`06.GILab` 进程卡在启动、日志 0 字节，终止后干净重跑即全绿，与本次改动无关）。

**⑥ 已知缺陷（重要，任务 12/18/19 必读）**：DAG 的内容哈希是**顺序无关**的，而任务 8 的 `meshopt_optimizeMeshlet` 会就地重排簇内顶点
⇒ 两个"内容相同"的簇可能有**不同的簇内顶点顺序**。共享的顶点/三角形段是首份出现的一致配对，**几何完全正确**（点集 + 三角形），
但法线/UV 是**按局部下标**关联的 ⇒ 顺序不一致的出现会取到别的顶点的属性。实测平铺 3×3：19 个出现簇中 **8 个**顺序与首份不一致，
属性冲突计数 1，下标一一对应最大误差 7.00587，而**点集匹配最大误差仅 0.0298**（≤ √3×半步长）。
彻底修法二选一：① 把法线/UV 词混进规范键流（更严格，但会降低去重率）；② 按位置而非下标关联属性。
**必须在任务 19（真实材质接入）与任务 21（画面级对照验收）之前修掉**，否则属性保真度无法通过验收。
### 14.21 任务 11/12 实施记录：单测收口与资产 GPU 上传读回校验（2026-09-20）

**① 任务 11（单测收口）**：逐条核对 §14.8 任务 11 的验收（尺寸/偏移/量化往返/边界），此前已被任务 7/10 的具名用例覆盖；
**只发现一个真缺口** —— 段对齐原语 `NaniteAlignUpFile` 与 LOD 段步长 `kNaniteLodOffsetBytes` 从未被直接钉过边界（只被段表用例间接使用），
补 1 个用例（`Tests/TestNaniteTypes.cpp:980`，417 断言：0/1/15/16/17/31/32/33 的最小对齐值、无截断、`0xFFFFFFFF×64` 不溢出、
LOD 级数 0/1/2/4/5/8 的段长取整）。**没有为凑数加用例**；完整"验收项 → 用例名"对照表见本轮提交说明与测试文件。

**② 任务 12（资产加载与 GPU 上传）**
- **几何来源（只读一次，未改 `MeshBatcher`）**：`MeshBatcher.h:60-61` **已存在** `GetMergedVertices()/GetMergedIndices()`
  （LumenSDF 早已当一次性输入用），故**无需新增 getter**。读取点唯一：`NaniteRenderer::EnsureAssetUploaded` 的一次性门闩内；
  之后只碰这份快照，模块不持有 `MeshBatcher*`。**索引口径**：合批时 `baseVertex` 已加进索引 ⇒ 它们是绝对合并顶点下标，
  消费侧**不得**再加 `vertexOffset`（LumenSDF 的同类教训）。`MeshBatcher.cpp:75-86` 的三处一致性契约未动。
- **GPU 资源划分**：按任务 10 的镜像切成 **1 个头 + 5 段 = 6 个 `StorageBuffer`**（头/簇/顶点/索引/材质/LOD），
  usage = `Storage | TransferSrc`，空段不建（Vulkan 不接受 0 长度）。6 片无缝覆盖整份镜像 ⇒ 读回校验可覆盖全部字节。
- **上传与读回（本任务验收）**：一次性命令表（用 `BeginLightweight()` 而非 `Begin()`，避免在帧内推进全局帧计数），
  6 条 `CopyBuffer` 拷进一张覆盖整份镜像的 host 可见读回缓冲，`Submit` + `WaitIdle`，然后与 `asset.bytes` **逐字节比较**。
  实测一行（三次运行数值逐位相同）：
  `[Nanite] upload_bytes=13382544 readback_match=1 mismatch_bytes=0 clusters=8287 vertices=541404 materials=0 lod_levels=6`
  派生核对：索引 4,189,584/8 = 523,698 三角形；输入合并几何 192,496 顶点/786,801 索引（262,267 三角形），LOD0 + 逐级减半去重后约 52 万 ✓。
- **关闭档零开销**：`EnsureAssetUploaded` 立刻返回，**一个资源都不建**（实测关闭档日志 `upload_bytes` 出现 0 行）。
- **触发点**：几何的唯一持有者是 `DeferredPipeline`，样例拿不到 ⇒ 触发点放在 `DeferredPipeline_FrameGraph.cpp:110/114/127`
  （`if (enabled && ready) m_Nanite.EnsureAssetUploaded(m_MeshBatcher);`），**样例零改动**，且不注册任何 pass（开启档 pass 集合仍是 12+2）。
- **最小 RHI 扩展（1 行，必须记录）**：`Engine/RHI/Vulkan/VulkanResources.cpp:88` 增加 `TransferSrc` 位映射 ——
  过去只硬编码了 `TRANSFER_DST`，没有它 `vkCmdCopyBuffer` 的源缓冲会报 `VUID-srcBuffer-00119`。该位**只增**且此前无任何调用方请求，
  既有缓冲的 `VkBufferUsageFlags` 一位不变（关闭档指纹与转储不动即为证据）。性质同任务 3 的 `DrawIndexedIndirectCount`。

**③ 验收证据（本人复跑）**：单测 **279 → 281 例**（断言 62400）全绿；关闭档 12 pass、指纹冻结 `1C15AB72E688B530…`、
`nanite_passes=0`、`vuid_lines=41`；开启档 14 pass、`vuid_lines=41`、`on vs off` 逐位比较 `must_same_diff=0`；
全量六条判据 `ACCEPTANCE SWEEP: PASS`。

**④ 如实记录的偏差与债务**
1. **首帧一次性卡顿 ≈ 11.4 s**（开启档）：成本是任务 9/10 的 DAG+打包在**整份合并几何**上跑一遍（`MeshBatcher: 103 meshes → 192496 verts` 到上传行之间）。
   不影响判据（转储在帧 120），但体感明显；优化（按网格分资产、或优化任务 9/10）会改到任务 9/10 的代码，留作性能债务（任务 23 的性能读数一并处理）。
2. **`materials=0` 是事实**：合并几何不携带材质 ID，`MeshBatcher` 的 draw command 也没有逐簇来源映射，
   强行反查会引入 §14.3 禁止的依赖 ⇒ 传空 span，逐簇 `materialID` 仍写 0（归属任务 19）。
3. **资产语义是"整份合并几何 = 一个 `.nanite`"**（不做逐网格拆分、不施加逐物体变换），一个簇可能横跨两个网格；
   §14.4 的"每网格资产路径 ⇒ 该网格由模块绘制"要等后续任务。
4. **任务 10 的共享内容属性错配缺陷未修未掩盖**（本任务只比字节，故自洽通过）——仍需在任务 19/21 之前修。
5. **`NaniteUnpackR10G10B10A2(packed, channel)` 在 `channel ≥ 4` 时是 UB**（`packed >> 40`，C++ 与 Slang 镜像同病）；
   现有调用方只用 0..3，属越出前置条件的输入；修它要三处同步，留给后续任务决定。
### 14.22 任务 13 实施记录：实例剔除（GPU 与 CPU 参考逐项一致，2026-09-20）

**① 契约核实**：`Engine/Render/Pipeline/GPUScene.h:26-40` 定义 `GPUSceneObject`（`localToWorld` 64B + `boundsMin/boundsMax` 各 16B + 8×u32 + pad，`static_assert(sizeof==128)` 在同文件 `:40`）。
`NaniteTypes.h` 放 RHI-free 同布局镜像 `NaniteInstanceGpuObject`（`alignas(16)` + 12 条 `offsetof` 断言）；**跨契约钉子在 `NaniteCull.cpp`**（include 真身头，`sizeof` 与逐字段 `offsetof` 双重 `static_assert`）——任一边漂移即编译失败。

**② 口径**：视锥平面与 `Math/Geometry.h` 的 `he::Frustum` **逐字同源**（`dot(n,p)+d>=0` 在内侧、顺序左右下上近远、Gribb/Hartmann + 归一化、Vulkan `[0,1]` 深度取 row2）；
球判据 `dot(n,c)+d < -radius ⇒ 不可见`（无 epsilon，与 `Frustum::Intersects(Sphere)` 一致）；包围球由 128B 契约的 `boundsMin/Max` 推出，
**CPU 与 GPU 读同一张 16B 球表的同一份比特**（避免 GPU 现推 sqrt/FMA 的末位差异翻转"恰切平面"的可见性）。

**③ 实例来源（如实）**：cfg 键 `nanite_instance_test_count`（默认 64，钳 [0,256]）生成的**合成实例网格**（NDC 网格经 `inverse(viewProj)` 反投影到世界空间、
深度 5 层，另含 i=0/i=2 视锥外、i=1 恰跨右平面、i=3 空实例）——**不是场景实例**；验收的是"GPU 与 CPU 参考的可判定等价性"。
新 pass `Nanite_InstanceCull`（开启档 pass 变为 15 个 = 既有 12 + 3），读回打印恰好一行。

**④ 发现并修掉一个真 bug（值得全仓借鉴）**：最初照任务 3 假簇链在**录制期主机写 0** 清计数 ⇒ 读回恰为 CPU 参考的 **2 倍**、列表同批下标连续出现两次
（`n=8: gpu=10 cpu=5 mismatch=9 first=1,1,4,4,5,5,6,7`；`n=64: gpu=122 cpu=61`）。对照实验钉死根因：**主机写与派发之间没有排序**，
引擎允许多帧在飞、CPU 领先 GPU ⇒ 第 N+1 帧写的 0 落到第 N 帧派发**之前**，两帧原子累加叠加。
- 对照 A（临时）：同位置先 `WaitIdle()` 再写 ⇒ 立刻 `gpu=5 cpu=5 mismatch=0`（已撤，只留注释）；
- **对照 B（最终实现）**：清零改为**命令缓冲内的 4B 拷贝**（常驻 0 的 `TransferSrc` 源 + `Transfer→Compute` 屏障，末尾屏障补 `Transfer/CopyDst` 消 WAR）⇒ 零停顿且逐项一致。
可见列表**不再逐帧 memset**（读回只取 `[0,count)`，这些槽位必由同一次派发写入）。
**同一潜在竞争在任务 3 的假簇链里依然存在**（本次未在改动面内，已写入注释）——**列为必须在任务 16/21 之前修掉的债务**。

**⑤ 验收证据（本人复跑）**：单测 **281 → 286 例**（断言 62481）全绿（新增 5 例：128B 镜像与 16B 球偏移、球由 bounds 推导含退化 AABB、
视锥提取与 `he::Frustum` 同值、CPU 参考已知进/出、空表/空指针/容量截断/6 平面缺一不可）；
关闭档 12 pass、指纹冻结 `1C15AB72E688B530…`、`vuid_lines=41`；开启档 15 pass、`vuid_lines=41`；
`[Nanite] instance_cull gpu=61 cpu=61 mismatch=0 first=1,4,5,6,7,8,9,10`（`n=8 → 5/5`、`n=256 → 253/253`、`n=0 → 0/0`，均 mismatch=0）；
on vs off 转储逐位 `must_same_diff=0`。

**⑥ 验收脚手架的三处修正（都是本次踩出来的，必须记住）**
1. **样例偶发卡在启动**：`06.GILab` 当天两次卡死（日志 0 字节、CPU 近 0）。两个冒烟脚本已改为 `Start-Process + WaitForExit(300s)`，
   超时即杀并报 `TIMEOUT`（该档视为失败），避免一条验收被无限拖住。
2. **`-File` 传 `-Extra` 在本环境被子进程拒绝**（与脚本 param 块无关，直接命令行调用却正常）⇒ 冒烟脚本额外接受 **`HE_SMOKE_EXTRA`/`HE_SMOKE_FURNACE`** 环境变量，验收脚本改用环境变量 + **进程内调用**。
3. **`.ps1` 必须 ASCII-only 这条纪律再次被踩**：我在无 BOM 的 `.ps1` 里写中文注释，PS 5.1 按 ANSI 解码后**中文末字节吃掉了换行**，
   使下一行 `if ($env:HE_SMOKE_EXTRA)` 被并进注释 ⇒ 环境变量静默失效（表现为"Extra 不生效"）。已把三个脚本清成纯 ASCII（非 ASCII 行已删除）。
   附带发现并修掉一个**空转判据**：判据 ③ 原先从样例日志里找 `lumen_passes=`，而该字样只在冒烟脚本自己的输出里 ⇒ 恒为 0、永远"通过"；
   现在解析冒烟输出、解析失败返回 -1 即判失败，并显式用 `gi_blend_diffuse_lumen=0` 真正关灯。

**⑦ 未完成的脚手架问题（下一轮第一件事）**：修完上述三处后，判据 ①（白炉）报 `gi_aq_furnace_prov6_final.f16 MISSING` ——
即**白炉档这次没有产出转储**（日志 212 KB、正常退出）。判据 ②③④⑤⑥ 均 PASS，且同一配置的直接探针能产出白炉转储，
故判定为脚手架/环境问题而非产品回归；**必须先把判据 ① 恢复成能真正产出并校验白炉转储，再继续后续任务**。

### 14.23 任务 14 实施记录：per-instance cluster BVH（构建 + 深度优先遍历，2026-09-20）

**① 分裂策略：最长轴中点分裂 + n/3 平衡护栏（每条由单测钉住）**
- **轴向**：取结点内全部簇球**质心** AABB 上跨度最大的轴（标准 BVH 启发式：沿最长轴分裂最可能把体积真正分开）。
- **切点**：该轴质心范围的**空间中点**（质心 < 中点的归左）。**为什么不用 SAH**：SAH 要对每个候选分裂
  算面积代价（或做分桶），既有浮点分箱又有"桶边界 vs 精确坐标"的对比；本任务的验收是**可复现**与
  CPU/GPU 逐项一致，中点分裂只有"一次排序 + 一次扫描"，确定性与可解释性都更强，且沿分裂轴产生
  **互不重叠**的孩子体积 —— 对"节点不可见 ⇒ 整棵子树跳过"的早退最有利。
- **回退（保证终止 + 保证平衡）**：一侧为空（质心全相同 / 极密集 / NaN）**或**任一侧不足 `ceil(n/3)` 时，
  退回**按数量中位数**（前半 n/2）分裂。
  **护栏不是预防性设计，是实测踩出来的**：先只做"一侧为空才回退"时，Sponza 合并几何（8287 簇）的树
  **深度恰好顶到上限 24、叶子 3103、节点 6205**（大量 1~2 簇的叶子）—— 根因是中点分裂遇到
  "少量离群簇 + 一大团"的分布时会一次只切掉 1~2 个簇。加 n/3 护栏后同一资产变成
  **节点 5345 / 叶子 2673 / 深度 15**，且深度有了**闭式上界**：每次分裂规模 ≤ `ceil(2n/3)` ⇒
  `depth ≤ 1 + log₁.₅(n / 叶子容量)`，对 `n ≤ kNaniteMaxBVHClusters`(16384) 恒 ≤ 22 < 24
  ⇒ 深度上限退化回**安全网**而不是树形的决定因素（单测直接断言这条上界）。
- **叶子容量 4**：每簇球很小（≤64 tri），4 个簇的叶子球仍然紧，叶子内至多 4 次球测试。
- **深度硬上限 24 / 显式栈 32**：见②。**确定性**：不含随机数、不读时间、不并行；排序比较器带
  **下标兜底**（坐标相同时按下标）⇒ 全序唯一；两次构建逐位一致（单测直接比节点表字节）。

**② 遍历实现（CPU 参考 + GPU 显式栈，逐条同构）**
- **CPU 参考**（`NaniteTraverseClusterBVHCPU`，放 `NaniteTypes.h` 的 inline 纯函数）：逐实例做一次 DFS，
  每弹出一个结点即 `visitedNodes += 1`（**先计数、后判可见**），结点球不可见 ⇒ 整棵子树跳过；叶子对
  `[left, left+count)` 逐个簇做球测试。判据复用任务 13 的 `NaniteSphereVisibleInFrustum`（`dot(n,c)+d < -r`
  ⇒ 不可见，无 epsilon），与 GPU 同一表达式。**显式栈**：`u32 stack[kNaniteBVHMaxStackDepth]`（**无分配**）。
- **GPU**（`Nanite_ClusterBVH.comp.slang`）：`[numthreads(64,1,1)]`，**一个线程一个实例**（本任务的实例域
  上限 64 ⇒ 1 个 workgroup 就够；PTG / work-stealing 属性能任务，不在本任务）。栈是 shader 私有数组
  `uint stack[32]`，先压右、再压左 ⇒ 下一次弹出左孩子，**访问顺序与 CPU 参考逐字一致**。本线程的访问数
  用一次 `InterlockedAdd` 汇总（整数加法可交换 ⇒ 与线程调度无关，这正是"访问数可复现"的实现前提）。
- **栈深度上界为什么是 32**：DFS 栈里同时存在的条目数 = "每层至多一个待访问的右兄弟" ≤ 树高，而构建器
  把树高硬限制在 24（n/3 护栏后实测 15）⇒ 32 有充分余量。真溢出不静默：参考实现有 `stackOverflows`
  计数（单测用手搭 41 层退化树证明它会 +1 而不是越界）。
- **实例域**：本任务把 Phase 2 挂在**全部非空实例**上（`indexCount != 0`，与任务 13 的跳过规则同一判据），
  钳到 `kNaniteMaxBVHInstances` = 64（可见簇引用表必须一次分配、容量恒定）。**为什么不用 Phase 1 的可见列表**：
  `Nanite_InstanceCull` 与 `Nanite_ClusterBVH` 在帧图里都**不声明任何帧图资源**，`RenderGraph::TopologicalSort`
  对 inDegree=0 的 pass 按 LIFO 处理 ⇒ **帧图无法表达**"本 pass 必须排在实例剔除之后"（同任务 3 的假簇链
  的同类隐患）。把 Phase 2 接到 Phase 1 的 GPU 计数上会引入一条不可由帧图表达的顺序假设 —— 三阶段接线
  正是**任务 15 的正文**（见⑥）。

**③ 两处位置的取舍（POD 与参考遍历 → `NaniteTypes.h`；构建器 → `NaniteUpload.{h,cpp}`）**
- `NaniteTypes.h`：`NaniteBVHNode`（32B：`float4 centerRadius` + `uint4 link`）、`NaniteClusterSphere`（16B）、
  `NaniteVisibleClusterRef`（8B）、`NaniteClusterBVHView`、读数结构、容量常量与 **CPU 参考遍历**。
  理由：它们是"与 Slang 共享的 GPU 布局"+"GPU/CPU 逐项一致的参考实现"，与任务 13 把
  `NaniteCullInstancesCPU` 放在这里**完全同构**；本文件仍是 RHI-free、纯头实现、可单测。
- `NaniteUpload.{h,cpp}`：`NaniteClusterBVH`（容器 + 读数）与 `BuildNaniteClusterBVH`。理由：它消费的是
  `.nanite` 的**簇记录**（任务 9/10 的产物），属于"资产 → 加速结构"的构建阶段，与 `PackNaniteClusters`
  同一层；且该翻译单元已被 `Tests/TestNaniteBuilder.cpp` 直接编译（`Tests/CMakeLists.txt:50-54` 的纪律钉子），
  构建器因此天然可单测。

**④ GPU 侧接线（缓冲 / 描述符 / pass / 清零）**：`NaniteCull` 自持 7 个缓冲 —— 节点（32768 条）、叶子簇表
（16384 条）、簇球（16384 条）、可见簇引用（1048576 条 = 64 × 16384，8MB）、可见簇计数、已访问节点计数、
8B 常驻 0 的清零源；7 个显式 SSBO 绑定（复用任务 13 的 128B 实例表缓冲）+ 112B push constant
（6 平面 + instanceCount/clusterCount/nodeCount/visibleCapacity）。**两个计数每帧在命令缓冲内用 4B 拷贝
清 0**（同一个 8B 零源的前后两半），照任务 13 的修法，**不用主机写**。新 pass `Nanite_ClusterBVH` 注册在
`Nanite_Raster` 之后、`reads/writes` 为空（只碰模块自持缓冲）⇒ 既有 12 个 pass 的相对顺序与集合不变。
`SetClusterBVH` 只在任务 12 的一次性资产构建路径上被调用一次（此缓冲还没有被任何已提交的 GPU 工作引用
⇒ 主机 `Map` 上传不存在竞争）。

**⑤ 验收证据（本人复跑）**
- 单测 **286 → 295 例**（断言 63707）全绿；新增 9 例：`NaniteBVH:` 四种网格规模/边界/复现/DFS 访问数
  （`TestNaniteBuilder.cpp`）+ 布局契约/空表/手搭 7 节点树/栈溢出防御/纯 POD（`TestNaniteTypes.cpp`）。
  关键 MESSAGE：`6x6 网格（72 tri）: 簇=2 节点=1 叶子=1 深度=1 最大叶子簇数=2`、
  `32x32 网格（2048 tri）: 簇=62 节点=37 叶子=19 深度=6 最大叶子簇数=4`、
  `3x3 平铺（9×64 tri）: 簇=19 节点=11 叶子=6 深度=4`；
  `DFS 全部在内：节点 7，访问 7，可见簇 16`、`DFS 全部在外：节点 7，访问 1，可见簇 0`、
  `DFS 部分相交：节点 7，访问 5，可见簇 6（下标 0..5）`、`栈溢出防御：链深 40，访问 63，溢出 1，可见 31`。
- 关闭档 12 pass、`nanite_passes=0`、指纹冻结 `1C15AB72E688B530…`、`vuid_lines=41`。
- 开启档 **16 pass** = 既有 12 + `Nanite_InstanceCull` / `Nanite_Cull` / `Nanite_Raster` / `Nanite_ClusterBVH`，
  `vuid_lines=41`；dump 帧恰好一行（三个配置各跑两次，同参数两次逐位相同）：
  - 默认（64 实例、相机 A）两次：`cluster_bvh nodes=5345 depth=15 gpu_visited=145095 cpu_visited=145095 gpu_clusters=124513 cpu_clusters=124513 mismatch=0`；
  - 相机 B（`cam_yaw=3.5; cam_pos_x=120`）两次：`… gpu_visited=97985 … gpu_clusters=43410 … mismatch=0`；
  - 8 实例（`nanite_instance_test_count=8`）两次：`… gpu_visited=16117 … gpu_clusters=13832 … mismatch=0`。
- `acceptance_sweep.ps1 -OnlyNanite`：`[6a] off passes=12 nanite_leak=0 sha=1C15AB72E688B530`、
  `[6b] on passes=16 nanite_passes=4 preexisting_set_changed=False`、
  `[6c] pairs=20 differing_outside_jitter=0 jitter_family_ondiff=3` ⇒ **ACCEPTANCE SWEEP: PASS**
  （6c 的 0 差异 = 任务 14 的 on/off 转储逐位一致）。

**⑥ 如实记录的偏差与风险（都不掩盖）**
1. **Phase 1 → Phase 2 的接线留到任务 15**（理由见②）。本任务的实例域是"全部非空实例"，
   不是 Phase 1 的可见列表；任务 15 必须把两个 pass 接成同一条链（或让实例剔除把结果落到帧图资源上），
   这是它的第一件事。
2. **实例域上限 64**：`nanite_instance_test_count=256` 的档位只会遍历前 64 个实例（CPU 与 GPU 同一口径，
   故 mismatch 仍为 0）。上限来自"可见簇引用表要一次分配"这一硬约束；把上限提到 256 需要 4M 条引用
   （32MB），属任务 15/16 决定是否值得。
3. **本任务的 CPU/GPU 逐项一致继承了任务 13 已知的"实例表上传是主机写"债务**：相机静止时实例表逐位相同，
   实测 6 次运行 mismatch=0；相机运动时理论上存在"第 N+1 帧主机写 vs 第 N 帧派发"的交错（修法同任务 13：
   随帧轮转的暂存环）。
4. **本任务不含 Hi-Z 遮挡与 LOD 选择**（§5.1 Phase 2 的后半与 Phase 3 是任务 15），也不消费可见簇列表
   （接光栅端是任务 16）—— 遍历的产物目前只用于验收读数。
5. **BVH 只在资产构建时建一次**（整份合并几何 = 一个资产，与任务 12 的口径一致）；逐网格资产/实例级
   动态增删属后续任务。
6. 新增 GPU 常驻约 9.3MB（节点 1MB + 叶子 64KB + 簇球 256KB + 可见引用 8MB + 计数/零源），
   **关闭档同样分配**（`NaniteCull::Initialize` 与任务 13 一样不区分开关）。关闭档的判据（pass 集合与
   转储逐位一致）不受影响，但"关闭档零新增 GPU 资源"这条口径从任务 13 起就不再成立（如实记录）。
7. **契约三处镜像**：`BVHNode`/`ClusterSphere`/`ClusterRef` 的布局在 `NaniteTypes.h`（static_assert 钉住）、
   `Nanite_ClusterBVH.comp.slang`（逐字段注释）与 `NaniteTypes.slang`（尚未收纳本节）三处需要人工同步；
   本次没有把 `NaniteTypes.slang` 作为公共 include（它仍未进 `SLANG_INCLUDES`，那是任务 18 的事），
   故新 shader 里重复声明了镜像结构 —— 这是**有意的**：把 `NaniteTypes.slang` 加进 `SLANG_INCLUDES`
   会让所有 shader 因 `DEPENDS` 全量重编译。
### 14.24 任务 15 实施记录：三阶段簇剔除 + Hi-Z（2026-09-20）

**① 最重要的一条发现（计划盲点，比任务本身更有价值）：`GPUCulling::BuildHiZPyramid` 在本引擎里构建不出正确金字塔，且从未被执行过。**
- 位置：`GPUCulling::BuildHiZPyramid` = `Engine/Render/Pipeline/GPUCulling.cpp:478-543`；纹理 `GetHiZTexture()` = `GPUCulling.h:97`；层数上限 `kHiZMips=8` = `GPUCulling.h:130`；
  格式 `R32_FLOAT`、层 L 存 `2^L×2^L` 足迹的**最小深度**（`HiZDownsample.comp.slang:31`）；深度为标准 Vulkan `[0,1]`（近=0）——三处交叉确认。
- 实测：直接复用 ⇒ Hi-Z 剔掉 87% 的簇 ⇒ 逐层采样发现 **mip0/1/4/7 全为 0**（而深度纹理本身实测 ≈0.9998）。
- **根因（用两个对照实验钉死）**：该函数在循环里**逐 mip 更新同一个描述符集**，而本引擎的 GPU 在**执行期**读描述符、**最后一次主机写对整段命令缓冲生效**：
  ① 同一个 UB 先写 A、录 Dispatch、再写 B ⇒ GPU 读到 **B**；② 在 Dispatch **之后**改绑深度纹理 ⇒ 该次派发采样读到深度值 **0.9999**。
  于是 7 次派发全用最后一个状态（都写进 mip7），mip1..6 从未被写 ⇒ 金字塔全 0。
- 另外：`useTwoPhase` 在所有配置里恒 false ⇒ `HiZ_Build` pass 从不注册 ⇒ 这个函数在本仓库**从未真正执行过**（所以缺陷一直没被发现）。
- **上游修法（3 行级，已写进代码注释）**：为每个目标 mip 分配**专属描述符集**（或循环内 `vkCmdPushDescriptorSet`）。
- **本次裁决**：`GPUCulling.*` 不在改动面内 ⇒ **复用其纹理资源与"2×2 取最小深度"口径，构建改由模块自己完成**（每个目标 mip 一个专属描述符集，
  各绑定每帧只写一次，从根上避开次序依赖；新增 `Nanite_HiZDownsample.comp.slang`，因写目标需 GENERAL、采样源需只读，同一张图不能同时满足，故 mip>0 的源改用存储图像读取，整条链只在开始/结束各一次整图转换）。
  副作用（正面）：模块不再调用它 ⇒ `m_HiZMipCount` 不被改写 ⇒ 既有 `GPU_Cull`/PTG 路径行为一位不变。

**② Phase1→Phase2 的顺序保证（不靠注册顺序）**：`Nanite_InstanceCull` 额外写一张**可见实例掩码**（每个在范围内的实例都显式写 ⇒ 无需清零、无竞态；用掩码而非压缩列表是因为遍历域按实例下标寻址，
钳到 64 后子集**确定**，而压缩列表"取前 64 个"是不确定子集、会让逐项比较失去意义）；Phase1 派发 + Hi-Z 构建 + Phase2/3 派发录在**同一个帧图 pass** 内
（新 `Nanite_CullChain3`，注册在 `AddPostGBufferPasses`，声明 `reads={gbDepth}` 以被定序在 `GB_Clear` 之后），顺序由命令缓冲里的 `PipelineBarrier` **显式**给出 —— 比声明一条帧图依赖更强。
代价（如实）：开启档 pass 数 16→**15**（`Nanite_InstanceCull` + `Nanite_ClusterBVH` 合并为 `Nanite_CullChain3`，`nanite_passes` 4→3）；判据 ⑥b 只要求"多出 Nanite pass 且既有集合与顺序不变"，仍 PASS。

**③ LOD 选择（含一处量纲修正）**：设计 §5.1 的 `projectedError = maxError / distance`、阈值 1 像素**量纲不自洽**，必须乘像素焦距：
`projectedErrorPixels = maxError / distance × focalPixels`，`focalPixels = 0.5×screenH/tan(fovY/2)`（从 `CameraData::fov` 算，**不**反解 viewProj 的 m11，它被视图旋转污染）；阈值取原文 `1.0` 像素，判据写成乘法形式减少舍入。
DAG 割用任务 9 的 `maxParentLODError`：`ownError` = 孩子记录的该值（叶子 0）、`parentError` = 本簇自己的该值（根 0）；选本簇 ⇔ `ownError ≤ 阈值` 且（根 或 `parentError > 阈值`），
**根必须靠显式根位判定**（只看数值会把根永远筛掉）。误差随级单调 ⇒ 每条链至多一个交点、实测恰好选中一级（单测断言）。LOD 元数据由新增
`BuildNaniteClusterLODInfo()` 从簇记录 + LOD 段推出（16B/条，CPU 与 GPU 读同一份比特）。

**④ 验收证据（本人复跑）**：单测 **295 → 303 例**（断言 63894）全绿；关闭档 12 pass、指纹冻结 `1C15AB72E688B530…`、`vuid_lines=41`；开启档 15 pass、`vuid_lines=41`；
- `nanite_hiz=0`（**默认**）两次同参数读数**逐位相同**且 **GPU 与 CPU 参考逐簇一致**：
  `cull3 phase1=61 phase2=120561 phase3=31648 hiz=off gpu_clusters=31648 cpu_clusters=31648 mismatch=0 lod=[7553,23973,122,0,0,0,0,0] cpu_lod=[同] extra_gpu=0 occluded=0 inst_mismatch=0 nodes=5345 depth=15 visited=140495`。
- `nanite_hiz=1`：`phase2=64215 phase3=18888 gpu_clusters=18888 cpu_clusters=31648 mismatch=12760 extra_gpu=0 occluded=56346 occl_mip=[0,0,0,0,0,244,3000,9516] hiz_req=1 hiz_mips=8`，
  且 **12760 = 244+3000+9516**（差集恰好是投影盒落在金字塔 5/6/7 层的那批簇）⇒ 差异**可量化解释**；`extra_gpu=0` 证明 Hi-Z 只会"少"不会"多"（保守方向）。
- `off vs on`（含 `hiz=on`）转储逐位：`must_same_diff=0`（仅 3 个抖动族文件不同）。
- 任务 3 假簇读数仍 `6/6/6/6`（**顺手修掉了它的同类竞态**：三处每帧主机写改为命令缓冲内拷贝，含 20KB 常驻 0xFF 哨兵整块拷贝，末尾屏障 srcStage 补 `Transfer`）。

**⑤ 偏差与风险（不掩盖）**
1. 最大偏差 = 未直接复用 `BuildHiZPyramid`（理由与证据见 ①）。
2. Hi-Z 依赖 GBuffer 深度 ⇒ 需 `gpu_cull=1`；无 Hi-Z 纹理时自动退化为 `hiz=off`（读数用 `hiz_req=1 hiz_mips=0` 区分"没开"与"开了但没纹理"）。
3. `nanite_hiz` **默认 0**：默认档必须保住"与 CPU 参考逐簇一致"这条硬验收（CPU 不可能复现 Hi-Z）。
4. CPU 参考恒为"Hi-Z 关闭"；打开档差异只做**统计可解释**（三条独立证据自洽 + 单测用合成金字塔把两档判据本身测全）。
5. 逐帧主机写（实例表 / 三阶段参数 / 假簇表）仍是任务 13/14/15 的既有债（相机运动时理论上跨帧交错）；真正修法是随帧轮转暂存环。
6. 64 实例域钳制保留（CPU/GPU 同口径）。
7. 逐簇局部 LOD 判据在"父子距离跨过阈值"时可能同时选中父子两级（局部判据固有余量，真实 Nanite 用 DAG 遍历消掉）；CPU/GPU 同一判据 ⇒ 不影响逐簇一致。

### 14.25 任务 16 实施记录：可见簇列表 → 间接绘制参数（2026-09-20）

**① 补掉的缺口与最终数据流**：任务 14/15 把可见簇算出来了却没有消费者（光栅端吃的是任务 3 的假簇链，
`fake_clusters=6 → count_buffer=6 indirect_cmds=6 rasterized_clusters=6`）。任务 16 把两段接成一条链：

```
Nanite_CullChain3（单个帧图 pass 体）
  Phase1 实例剔除（掩码）→ Hi-Z 构建 → Phase2/3 三阶段剔除
    └─ 接受一个可见簇时（Nanite_ClusterBVH.comp.slang）：
         slot = InterlockedAdd(u_VisibleClusterCount, 1)          // 可见簇素（不截断）
         if (slot < visibleCapacity) u_VisibleClusters[slot] = ref
         if (slot < drawCapacity)  { u_IndirectCommands[slot] = cmd; InterlockedAdd(u_DrawCount, 1) }
         else                        InterlockedAdd(u_Stats[kStatDrawTruncated], 1)
  → 屏障 Compute → DrawIndirect
  → NaniteRaster::RecordRasterPass(DrawIndexedIndirectCount(indirect, count=u_DrawCount,
                                                             maxDrawCount=容量, stride=20))
```

**② 字段映射（`NaniteMakeClusterDrawRange` / `NaniteMakeIndirectCommand`，CPU/GPU 同一份定义）**

| 命令字段 | 取值 | 依据 |
|---|---|---|
| `indexCount` | `triangleCount × 3` | 簇内**索引个数**（`NaniteClusterRecord::triangleCount` ≤ 64） |
| `firstIndex` | `triangleOffset × 3` | 簇在打包索引段里的**首个索引位置**（索引段是 3×u16 进 u32[2] = 8B/三角形，故"索引位置"与索引宽度无关） |
| `vertexOffset` | `vertexOffset`（原样搬运） | 簇的顶点段起始**记录下标** |
| `instanceCount` | `1` | 一个簇 = 一次绘制 |
| `firstInstance` | **簇号**（簇表下标） | 光栅端用 `SV_InstanceID` 收它；也是"每条命令归属哪个簇"的唯一标识 |

**③ "无空转"的三重保证（逐条可查）**
1. **同一次派发写出**：命令与可见簇引用写在同一原子槽位 `slot`；绘制计数只在"真的写了命令"时 +1
   ⇒ `u_DrawCount` 恒等于"命令缓冲里 `[0, count)` 的有效条数"，不存在"没写就画"。
2. **只画 `[0, count)`**：`DrawIndexedIndirectCount` 的条数由 GPU 写出（CPU 不参与），
   `maxDrawCount` 只是容量上界；`drawCapacity ≤ 容量` 是 CPU 侧钳制 ⇒ `count ≤ maxDrawCount` 恒成立
   （`IRHICommandList::DrawIndexedIndirectCount` 的硬约束）。
3. **不残留上一帧命令**：绘制计数每帧在**命令缓冲内**用 4B 拷贝清 0（常驻 0 源，任务 13/15 的修法）；
   零可见簇时计数为 0 ⇒ 画 0 条。**全链路没有一处主机写清零**（任务 13 实测主机写会错读成两倍）。

**④ 假簇链的取舍（明确裁决：保留为"自证 + 退化"开关，默认不参与绘制）**
- 新增 cfg 键 `nanite_fake_chain`（默认 0）：`0` = 绘制由可见簇列表驱动（默认档，任务 16 的验收对象）；
  `1` = 退回任务 3 的固定命令通道（自证/回归）。
- 退化路径：可见链**尚未就绪**（资产/BVH 未入库 ⇒ Phase 2/3 不派发、一条命令都产不出来）时自动走假簇链。
  **"实例数为 0"不算退化** —— 那正是"零可见簇 ⇒ 零绘制"的边界，必须走可见链（走假簇链会画出 6 条，
  把边界验收掩盖掉）。
- **绘制录在产出命令的那个 pass 体内**（假簇链录在 `Nanite_Cull`、可见链录在 `Nanite_CullChain3`），
  顺序由命令缓冲里的 `Compute → DrawIndirect` 屏障给出 —— 帧图对两个零资源 pass 的排序不可依赖
  （`TopologicalSort` 对 inDegree=0 的 pass 按 LIFO 处理；任务 15 已为 Phase1→Phase2 踩过这条）。
  代价（如实）：开启档 pass 数 **15 → 14**（`Nanite_Raster` 不再单独注册），既有 12 个 pass 的集合与顺序
  一字不变（判据 ⑥b 仍 PASS）。
- `LogFakePipelineReadback` 只在**假簇链是绘制来源**时打印（否则同一帧会有两条互相矛盾的"画了多少"）。

**⑤ 绘制端的两处必要改动**
- **占位索引缓冲必须覆盖整个索引位置空间**：本通道仍是"数次数"的占位光栅（真实软光栅是任务 18），
  但 `DrawIndexedIndirectCount` 会拿命令里的真实 `firstIndex/indexCount` 去**绑定索引缓冲**取索引。
  本设备**未启用** `robustBufferAccess`，越界读索引不是定义行为 ⇒ 缓冲容量按**资产的索引总数**
  （`header.indexCount`，实测 1,571,091）分配、并钳到可证上界 `簇数上限 × 每簇三角形上限 × 3`。
  内容 = `0,1,2` 周期模式：任意 `[firstIndex, firstIndex+indexCount)`（两端都是 3 的倍数）都读出
  `{0,1,2}` 周期序列 ⇒ 顶点着色器按 `SV_VertexID % 3` 取角 ⇒ **每个三角形都非退化**、每个绘制至少
  1 个片元（退化三角形会被光栅器整块丢弃，计数就不可信）。
- **计数语义改为"每个绘制恰好一次"**：任务 16 起 `indexCount` 是簇的真实索引数（最多 192）⇒ 一条命令
  会产生多个片元，"每个片元 +1"不再等于绘制次数。改用 `SV_PrimitiveID == 0`（**绘制内**图元序号，
  每条命令都从 0 开始）。
  - **为什么不用"按簇号去重位图"**：可见簇引用是 (实例, 簇) 二元组，同一簇会被同一份资产的多个实例
    各引用一次，而命令的 `firstInstance` 只带簇号 ⇒ 按簇号去重会把它们的绘制错误地折叠成一条。
    `SV_PrimitiveID` 是**逐次执行**的量，天然不受影响（这也是它能同时服务"空转=0"判据的原因）。
  - **设备依赖（如实记录，且已实测）**：Slang 对 HLSL 拼写的 `SV_PrimitiveID` 会让 SPIR-V 声明
    `OpCapability Geometry`，而 Vulkan 的 SPIR-V 环境规定 `Geometry ⇒ 必须启用
    VkPhysicalDeviceFeatures::geometryShader`。故在 `VulkanDevice.cpp` **按支持情况启用该特性**
    （3 行，带中文注释与本条依据）；本引擎不建任何几何着色器管线，开启它对渲染结果零影响。
    实测开启后 `vuid_lines` 仍是 41、VUID 组成逐条不变。

**⑥ 可验证读数（dump 帧恰好一行，四个数来自四条独立的真实 GPU 路径）**

```
[Nanite] visible_wiring visible=31648 indirect_count=31648 draws=31648 rasterized=31648
         empty_draws=0 mismatch=0 src=visible truncated=0 max_draws=1048576
         cpu_cmds=31648 placeholder_indices=1571091
```

- `visible` = 可见簇计数缓冲（剔除端原子）——"应该画多少条"；
- `indirect_count` = 间接命令缓冲 `[0, visible)` 里**字段合法且与 CPU 参考逐字段一致**的条数
  （CPU 逐字节读回 GPU 内存核验）——"命令缓冲里真的有这么多条"；
- `draws` = 绘制计数缓冲（= `DrawIndexedIndirectCount` 实际用的 count）——"间接参数条数"；
- `rasterized` = 绘制端片元 `SV_PrimitiveID == 0` 的原子计数——"GPU 真的执行了这么多次绘制"；
- `empty_draws = visible − rasterized`（画了却没出片元的条数，必须 0）；
- `mismatch` = 逐条字段不一致数 + |V−C| + |V−D| + |D−R|（正常运行必须 0）。
- 两个"边界"自证开关：`nanite_draw_capacity`（截断）与 `nanite_instance_test_count=0`（零可见簇）。

**⑦ 验收证据（本人复跑）**
- 两个 target 构建 `EXIT=0`（含 3 个改动过的 shader 经 slangc 编译通过）。
- 单测 **303 → 307 例**（断言 63894 → 64010）全绿；新增 4 例：字段映射（首/末簇、簇内索引范围、
  `firstInstance=簇号`、合法性反例）、容量截断/零可见簇/空指针/越界簇下标、与 CPU 参考遍历串起来
  的逐条一致 + 两次打包逐位可复现、布局契约与占位索引上界。关键 MESSAGE：
  `可见簇=8 → 命令=8 截断=0；首条 indexCount=12 firstIndex=0 vertexOffset=0 firstInstance(簇号)=0；两次打包逐位一致`。
- 关闭档：12 pass、`nanite_passes=0`、`vuid_lines=41`、指纹冻结 `1C15AB72E688B530…`（未动）。
- 开启档：14 pass = 既有 12 + `Nanite_Cull` / `Nanite_CullChain3`；`vuid_lines=41`（VUID 组成逐条不变）；
  两次同参数运行的 `visible_wiring` 行与 `passlist_sha` 逐位相同。
- 各档读数（同一行口径）：
  - 默认（64 实例）：`visible=31648 indirect_count=31648 draws=31648 rasterized=31648 empty_draws=0 mismatch=0`；
  - `nanite_hiz=1`：`…=18888` 四个数一致、`empty_draws=0 mismatch=0`（与任务 15 的 `gpu_clusters=18888` 吻合）；
  - `nanite_instance_test_count=8`：`…=2593`（8 实例 vs 64 实例 ⇒ 2593 vs 31648，比例关系正确）；
  - `nanite_draw_capacity=1000`（截断自证）：`visible=31648 indirect_count=1000 draws=1000 rasterized=1000
    truncated=30648`，`max_draws=1048576` ⇒ **截断但不越界、不崩**（`empty_draws/mismatch` 非 0 是**故意的**
    截断后果，由 `truncated` 解释）；
  - `nanite_instance_test_count=0`（零可见簇边界）：`visible=0 indirect_count=0 draws=0 rasterized=0
    empty_draws=0 mismatch=0` ⇒ 零绘制、不崩、不残留上一帧命令；
  - `nanite_fake_chain=1`（自证/退化档）：任务 3 的读数仍 `6/6/6/6`，`visible_wiring … src=fake` 如实标注。
- `on vs off` 转储逐位：**仅 3 个抖动族文件不同**（`hdr` / `prov0_ao_final` / `prov0_ao_raw`），
  其余 15 个目标 0 差异；并以 `off vs off` 作对照，差异集合**完全相同**（同一族三个文件）⇒ 差异是基线
  自带抖动，与本改动无关。`on` 的 pass 列表去掉两个 Nanite pass 后与 `off` **逐行相同**。

**⑧ 偏差与风险（不掩盖）**
1. **"rasterized" 用 `SV_PrimitiveID` 而非"簇号去重位图"**：位图方案在多实例下会把多个实例的绘制
   折叠（理由见 ⑤），故放弃；代价是引入 `geometryShader` 特性的启用（已实测无新 VUID）。
2. **`cpu_cmds` 在 `hiz=1` 档与 `visible` 不相等**（31648 vs 18888）：CPU 参考恒为"Hi-Z 关闭"口径
   （任务 15 的既定口径，CPU 拿不到金字塔逐 texel 内容）；判据只用 GPU 的四个数，`cpu_cmds` 仅作对照。
3. **占位索引缓冲 ≈ 6.3 MB（Sponza）**：本通道仍是占位光栅，任务 18 的真实软光栅改从 SSBO 读索引/顶点后
   它即可废弃（或转给任务 22 的 mesh 管线复用）；关闭档不创建（懒建在首次录制时）。
4. **可见簇为 0 的实测入口是"实例数为 0"**：合成实例网格是**按相机 NDC 反投影**摆出来的，永远在相机前方，
   因此"相机完全背对"无法在合成场景里构造（如实记录；真实场景接入后才有意义）。
5. **绘制端每帧要跑 ~3.2 万次间接绘制**（`indexCount` 是簇的真实索引数，最多 192）⇒ 顶点/片元调用数
   是"可见簇数 × 平均三角形数 × 3"（百万量级）。这是"命令字段用真实索引范围"的必然代价，属占位通道；
   性能读数与优化归任务 23。
6. 逐帧主机写（实例表 / 三阶段参数 / 假簇表 / 绘制参数表的**一次性**上传）仍是任务 13/14/15 的既有债。
7. **新增 GPU 常驻约 21.3 MB**（间接命令 20 B × 1048576 = 21 MB + 每簇绘制参数 16 B × 16384 = 256 KB
   + 绘制计数 4 B），且**关闭档同样分配**（`NaniteCull::Initialize` 与任务 13/14 一样不区分开关）——
   与任务 14 §6 记录的"关闭档零新增 GPU 资源这条口径从任务 13 起就不再成立"同一条既有偏差。
   占位索引缓冲（≈6.3 MB）是**懒建**的，关闭档不创建。
8. 绘制端的两处"占位"性质必须记住：索引缓冲是模块自建的 `0,1,2` 周期模式、几何是覆盖全 NDC 的
   占位三角形 —— **本任务没有做真实簇光栅化**（那是任务 18），本任务交付的是"可见簇数 → 绘制条数"
   这条接线与它的可读回证据。
### 14.26 任务 17 实施记录：CPU 参考对照工具（2026-09-20）

**① 交付物**：新增 `build\verify\nanite_cull_diff.ps1`（纯 ASCII、无 BOM），并把它作为**判据 ⑦** 接进
`build\verify\acceptance_sweep.ps1`。工具**不重算任何数字**：只重跑既有 `nanite_smoke.ps1`，再解析引擎在 dump 帧打印的
恰好一行 `[Nanite] cull3 …`（任务 15 的验收出口）与恰好一行 `[Nanite] visible_wiring …`（任务 16 的验收出口），
判据与任务 15/16 **同源**（不另立阈值）。一条命令：
`powershell -NoProfile -ExecutionPolicy Bypass -File build\verify\nanite_cull_diff.ps1`
（参数：`-Tag <前缀>`、`-SkipHeavy`（跳过复跑，五档覆盖不变）、`-RepeatTiers <逗号表>`，默认 `default,hiz1`；退出码 PASS=0/FAIL=1，整体 ≈2.4 min）。

**② 五档判据（与任务 15/16 同源）**

| 档 | Extra | 判据 |
|---|---|---|
| default | `nanite_enable=1` | `mismatch=0`（GPU 与 CPU 参考逐簇一致）、`extra_gpu=0`、`occl_mip` 八项全 0、`gpu=cpu`；接线 `V=C=D=R`、`empty_draws=mismatch=truncated=0`、`src=visible`、`cpu_cmds=V`；非空转守卫 `visible>0` |
| hiz1 | `+nanite_hiz=1` | `extra_gpu=0` 且 `mismatch == sum(occl_mip)`；`hiz=on`、`hiz_req=1`、`hiz_mips>=2`；守卫 `sum>0`、`gpu<cpu`、`gpu+mismatch=cpu` |
| ic8 | `+nanite_instance_test_count=8` | 同 default 档 |
| ic0 | `+nanite_instance_test_count=0` | `phase1=2=3=0`、`gpu=cpu=0`、`mismatch=0`；接线 `V=C=D=R=0` 且 `src=visible`（零可见簇必须仍走可见链，走假簇链会把边界掩盖掉） |
| cap1000 | `+nanite_draw_capacity=1000` | `mismatch=0`（容量不影响剔除输出）；`draws=rasterized=indirect_count=1000=容量`、`truncated>0`、`truncated=empty_draws=visible-rasterized`、`mismatch==|V-C|+|V-D|+|D-R|` |

每档另查三件事（都属既有口径）：① 引擎日志 `[Nanite] 配置恢复:` 必须逐键等于本档请求（防 Extra 被静默丢弃——此坑已踩两次）；
② 本档开启列表去掉 `Nanite*` 行后逐字节哈希 == 冻结关闭档指纹 `1C15AB72E688B530…`，且五档 `passlist_sha` 唯一；
③ 跨档关系：`hiz1` 的 CPU 参考与 default 完全相同（CPU 参考恒为 Hi-Z 关闭口径）、`hiz1.gpu<default.gpu`、`0<ic8.gpu<default.gpu`、`cap1000` 剔除输出与 default 逐位相同、`ic0.gpu=0`。

**③ 复现性**：`default` 与 `hiz1` 在同一次调用内各跑两遍，去掉时间戳后两行读数必须**逐字符相同**、`passlist_sha` 相同。

**④ 实测读数（本人复跑，并已作为 N2 阶段收口跑过全量七条判据）**
```
[1/5] default  hiz=off visible= 31648 mismatch=     0 extra_gpu=    0 occl_mip_sum=     0 gpu/cpu=31648/31648 draws= 31648 rasterized= 31648 truncated=     0 => OK
[2/5] hiz1     hiz=on  visible= 18888 mismatch= 12760 extra_gpu=    0 occl_mip_sum= 12760 gpu/cpu=18888/31648 draws= 18888 rasterized= 18888 truncated=     0 => OK
[3/5] ic8      hiz=off visible=  2593 mismatch=     0 extra_gpu=    0 occl_mip_sum=     0 gpu/cpu=2593/2593 draws=  2593 rasterized=  2593 truncated=     0 => OK
[4/5] ic0      hiz=off visible=     0 mismatch=     0 extra_gpu=    0 occl_mip_sum=     0 gpu/cpu=0/0 draws=     0 rasterized=     0 truncated=     0 => OK
[5/5] cap1000  hiz=off visible= 31648 mismatch=     0 extra_gpu=    0 occl_mip_sum=     0 gpu/cpu=31648/31648 draws=  1000 rasterized=  1000 truncated= 30648 => OK
CULL DIFF: PASS
```
`hiz1` 的 `12760 = 244+3000+9516 = sum(occl_mip)`（与任务 15 逐位一致）；`cap1000` 的 `30648 = 31648-1000`。
**N2 阶段收口实测**：`ACCEPTANCE SWEEP: PASS`（七条判据：白炉 1.0000、背靠背严格 0、关 Lumen 0、默认预设严格 0、单测全绿、开关不变式、cull diff）。

**⑤ 判据 ⑦ 接入方式与理由**：放在判据 ⑥ 之后、`if (-not $OnlyNanite)` **之外** ⇒ 全量与 `-OnlyNanite` 都跑
（⑥⑦ 同属 Nanite 不变式，排除 ⑦ 会让"一条命令覆盖 Nanite"失真；代价 `-OnlyNanite` 从 ~50 s 变 ~4 min）；
用**子进程**调用（工具以 `exit 0/1` 收尾，进程内调用会把整个验收脚本一起结束）；**不加 `-SkipHeavy`**，让复现性证据进入一条命令的验收。
另做了**负向验证**（副本把 hiz 档判据改成 `mismatch == sum(occl_mip)+1` ⇒ 仅该档 FAIL、`CULL DIFF: FAIL`、exit 1，副本在 `%TEMP%`，未入仓库）⇒ 判据非空转。

**⑥ 偏差与风险（不掩盖）**
1. 判据不重算可见簇集合，只消费引擎已打印的读数（这正是"同源"的实现方式）；读数行被改坏时只能由"字段缺失/配置回显不符/passlist 指纹不符"间接发现。
2. **既有脚手架缺陷（本次发现，未修，不在改动面内）**：`nanite_smoke.ps1`/`lumen_smoke.ps1` 打印的 `exit=` **恒为空** ——
   `Start-Process -PassThru` + `WaitForExit()` 在本机不填 `Process.ExitCode`（实测 `cmd /c exit 7`：`-Wait` 得 7，`-PassThru`+`WaitForExit` 得空）。
   此前没有判据解析它，故一直未暴露。工具把空 `exit=` 视为"未报告"（只对数字非 0 判失败），失败判定由读数/回显/指纹承担。
   **最小修法**：改用 `[System.Diagnostics.Process]::Start` + 异步读输出（保留 300 s 超时守卫）——属脚手架改动，建议单独一条提交。
3. 仍是**合成实例网格**上的对照（§14.22③），不是场景实例；"相机完全背对 ⇒ 零可见簇"仍无法构造（§14.25⑧），零可见簇入口仍是 `nanite_instance_test_count=0`。
### 14.27 任务 18 实施记录：软光栅写 GBuffer（含 P0 的共享内容属性错配修复，2026-09-20）

**① P0：把属性词纳入内容键流（修掉 §14.20⑥ 的缺陷）**
- 缺陷：DAG 内容哈希**顺序无关**，而 `meshopt_optimizeMeshlet` 会就地重排簇内顶点 ⇒
  位置/拓扑同构但**局部顺序不同**的簇共享顶点段，按局部下标取法线/UV 就会读到别的顶点。
- 修法（改动最小、语义最直白）：`BuildClusterCanonicalKeys` 的键流追加第四段
  `[属性标记 'NAAT'][顶点数][逐局部下标的法线词、UV 词]`（法线 = 八面体 10+10 位、
  UV = unorm16，与打包器**同一组函数**）。前三段仍是顺序无关的（平移副本照常命中），
  第四段刻意**按局部下标有序** ⇒ "共享 ⇒ 逐下标属性逐位相同"由哈希保证。
- 接口：新增五参数 `BuildNaniteClusterDAG(positions, normals, uvs, indices, out)`；
  三参数重载 = 空属性（属性段退化为常量 ⇒ 等价关系与任务 9 当时**逐位相同**，
  既有调用方/单测的去重率不受影响）。`BuildNaniteAssetFromGeometry` 恒传属性。
- **打包器变成硬门**：`attributeConflictCount != 0` ⇒ `PackNaniteClusters` **返回 false**
  （不再"如实报告后照写"）：绕过五参数重载手工拼的 DAG 会被拒，不再可能静默产出错配资产。
- 去重率重测（单测 MESSAGE 原文，`Tests/TestNaniteBuilder.cpp`）：
  | 网格 | 任务 9 口径（属性不进键流） | 修复后（属性进键流） |
  |---|---|---|
  | 平铺 16×64 tri（每片 UV 平移 = 属性真不同） | 51.61%（unique 15/31） | **6.45%**（unique 29/31） |
  | 平铺 16×64 tri（逐片属性完全相同的真副本） | 51.61% | **32.26%**（unique 21/31，仍显著 > 10%） |
  | 一般网格 32×32（2048 tri） | 0.00% | 0.00%（如实） |
  | 3×3 平铺 576 tri（真副本，走资产路径） | 42.11% | 26.32%（去重照常命中） |
  | 每片 UV 平移（真冲突） | 42.11% | 6.45%（"每片 UV 平移"的 4×4 档见上表第一行） |
- 属性一致性：`attributeConflictCount == 0`、逐"出现 × 局部下标"重算属性词与落盘值
  **1663 个全一致、0 个不一致**；负向回归用例（三参数 DAG + 带属性打包）**必须被拒**。

**② 软光栅（设计 §5.2）：** **两趟"原子深度键 + 等值复检"**，不是"一趟 + ROV/interlock"**
- **为什么不能用 ROV（实测证据，重要）**：Slang 2026.13 对 compute 入口里的
  `RasterizerOrderedTexture2D` **静默降级**成普通 `RWTexture2D`（exit 0、`-warnings-as-errors all`
  下零诊断、SPIR-V 里既没有 `OpBeginInvocationInterlockEXT` 也没有任何 `OpExtension`）；
  而 SPIR-V 规定 interlock 的 execution mode 只对 **Fragment** 入口合法（手工汇编后 `spirv-val`
  报 "Execution mode can only be used with the Fragment execution model."）。⇒ 单趟写法
  （"原子最小深度后紧接着写颜色"）**有竞态**：更近的三角形赢了深度，更远的那个的颜色写入
  可能后落地。设计 §5.2 的原文与 §12 的伪码按此裁决**修正为两趟**。
- 第 1 趟 `Nanite_SoftRasterDepth.comp.slang`：每簇一个工作组（`[numthreads(16,1,1)]`，
  dispatch 按可见簇容量取整、shader 按可见计数早退），逐三角形投影 → 2D 包围盒 → 重心覆盖 →
  `InterlockedMin(u_DepthKey[py*W+px], key)`；`key = (asuint(ndcZ) & 0xFFFFFF00) | (triLocal & 0xFF)`
  （高 24 位深度位模式，非负浮点与无符号整数同序；低 8 位三角形下标给出平局全序）。
  深度键是 `RWStructuredBuffer<uint>`（不是 R32_UINT 存储图像）：结构化缓冲上的 `InterlockedMin`
  同样零额外设备特性，而且**每帧清屏就是一次 `CopyBuffer`**（常驻 0xFF 源，GPU 有序，不用主机写）。
- 第 2 趟 `Nanite_SoftRaster.comp.slang`（任务 18 要求的文件）：**重跑同一段光栅化**
  （同代码路径 ⇒ 覆盖集合与深度逐位一致），对每个像素做 `u_DepthKey[idx] == 自己的 key`
  **等值复检**，相等才写 albedo / normal / worldPos / lightmapKey ⇒ 每像素恰好一个三角形写一次，
  多目标天然一致。**实测自洽**：无重叠场景下第 1 趟覆盖像素数 == 第 2 趟写入像素数
  （3264 == 3264）；满覆盖档 3.14 亿次覆盖 → 4431 万次写入（重叠由等值复检去重）。
- **只对 `triangleCount <= maxTriangles`（默认 16 = §5.2）的簇走软光栅**，超过的簇跳过并计数
  （`skipped_big`），留给任务 22 的 mesh 硬光栅；阈值可配 `nanite_soft_max_triangles`（1..64）。
- **材质**：资产材质段为空（任务 19 才解析）⇒ 中性常数 `albedo = 0.8`、`metallic = 0`、
  `roughness = 0.5`，并用 `neutral_material_pixels` 如实标出（不假装是真材质）。
- **深度**：compute **不写** `D32_SFLOAT` —— 本机 NVIDIA 支持它做存储图像、同机 AMD 核显不支持
  （§14.14），且 GBuffer 深度纹理按任务 4 的 A1 裁决**没有** `UnorderedAccess`，本次改动面又
  禁止改 `GBufferRenderer`。故走 §14.5 裁决里的另一条路：模块自持深度键 + 全屏片元
  `Nanite_DepthResolve.*`（`SV_Depth`：键的高 24 位还原成 NDC 深度，哨兵 ⇒ 1.0 远平面）
  写**既有深度附件**。运行时仍调新增的 `IRHIDevice::SupportsStorageImage(Format)`
  （`vkGetPhysicalDeviceFormatProperties` + `STORAGE_IMAGE_BIT`，按格式缓存）把
  "该格式能不能做存储图像"查一次并打进读数（本机 `depth_storage_image_supported=1`）——
  降级是**被报告**的，不是静默的。

**③ 接线（让位）与取舍**
- **让位的落点不是"换掉 GB_Clear"**：pass 的**名字、reads/writes 声明、在帧图里的位置一个都没变**
  （判据 ⑥ 的 pass 集合与指纹判据因此不受影响），变的是 `GB_Clear` 这个 pass 体内"谁写几何"：
  `enabled && IsReady() && softRaster` ⇒ 只清屏（模块 compute 写 8 张颜色目标 UAV + 深度由深度解析
  全屏重写），既有 `m_GBuffer->Render(...)` 让位（帧图侧门控）；否则既有路径原样执行。
- **清屏为什么用 compute 而不是渲染通道**：`BeginOffscreenPassMRT` 的 loadOp 取自 PSO，而 render pass
  在 RHI 里按格式组合复用（Decal 用同一组 8 格式 + `Load`）⇒ 实测**清不掉**（未覆盖像素读出
  (0,0,0,0)，albedo.a 均值 0.0000 而清除值是 1.0）。改用 `Nanite_GBufferClear.comp.slang`
  （8 张颜色目标都是任务 4 已加 UAV 的存储图像）后 albedo.a 均值 = **0.9999**（清除值生效，见④）。
  深度不经 `ClearDepthStencil`（它在本引擎的 GBuffer 深度上会多出 10+10+10 条校验行），
  由深度解析通道逐像素写。
- **软光栅录在 `Nanite_CullChain3` 的同一个 pass 体内**（紧随剔除链的派发之后）：它要读同帧剔除链
  写出的可见簇列表/计数，而帧图无法为两个"零帧图资源"的模块 pass 表达这条顺序（inDegree=0 按 LIFO
  处理 —— 任务 15/16 的教训）。该 pass 因此**新增**四条 `UAV` 写声明（albedo/normal/worldPos/
  lightmapKey）⇒ 帧图据此把它排在 `GB_Clear` 之后、Lighting 之前；深度**不声明**（它在 pass 体内
  作为附件被写，RHI 的 render pass 结束时会还原成只读，与帧图模型一致，声明成 Write 反而会破坏
  Hi-Z 对本帧深度的采样）。
- 三趟的**内部顺序全部由命令缓冲里的显式屏障给出**（清键 → 第 1 趟 → 第 2 趟 → 深度解析），
  不依赖帧图。
- 开启档 pass 数仍是 **14**（既有 12 + `Nanite_Cull` + `Nanite_CullChain3`），
  `passlist_sha` 与任务 16 完全一致（`750CC247BF8B9C3D…`）。

**④ 验收证据（本人复跑，2026-09-20）**
- 两个 target 构建 `EXIT=0`；4 个新 `.spv.h` 全部生成（`Nanite_SoftRasterDepth.comp` 60,286 B、
  `Nanite_SoftRaster.comp` 103,897 B、`Nanite_DepthResolve.vert/frag`、`Nanite_GBufferClear.comp`）。
- 单测 **308 → 308 例**（62142 断言）全绿；新增 1 例（`NanitePack: 属性错配的资产被拒绝`）、
  重写 1 例（`NaniteDAG: 共享簇必须属性逐位一致`）。
- 关闭档：`passes_per_frame=12`、`nanite_passes=0`、`vuid_lines=41`、
  指纹 `1C15AB72E688B5302332AEC391C41A5FE2B4D9512258CCDCD5D3E9D7E8F5390D`（= 冻结值）。
- 开启档（默认 `nanite_soft_max_triangles=16`）：14 pass、`vuid_lines=46`、
  `passlist_sha=750CC247BF8B9C3D…`（与任务 16 相同）、读数一行：
  `[Nanite] soft_raster clusters=31648 soft=61 skipped_big=31587 triangles=61 pixels_written=3264
   degenerate=0 neutral_material_pixels=3264 depth_written=1 depth_storage_image_supported=1
   depth_src=key+SV_Depth max_triangles=16 instances=64 depth_key_pixels=2073600 covered_px=3264
   diag_screenw=1920 diag_screenh=1080 diag_maxtri=16 diag_extent_milli=3720854 tested_px=9767`
  （**Sponza 的簇绝大多数是满簇 64 tri ⇒ 阈值 16 下只有 61 个簇、3264 个像素**；
   `covered_px == pixels_written` 证明两趟逐位一致）。
- 满覆盖档（`nanite_soft_max_triangles=64`）：`soft=31648 skipped_big=0 triangles=1758608
   pixels_written=44318207 degenerate=262899 covered_px=314423307 tested_px=1112509107`，
  整档 117 s（121 帧）。
- **同场景同相机对照**（`build/verify/nanite_soft_cmp.py`，AI 面板 1920×1080，转储帧 120；
  参考 = `nanite_off`（既有路径），测试 = 模块开）：

  | 对照 | 目标 | 覆盖率(参考/测试) | 均值(参考/测试) | 相关系数(全图) | 相关系数(双方覆盖) |
  |---|---|---|---|---|---|
  | off vs on(16) | albedo | 1.0000 / 1.0000 | 0.2009 / 0.2500 | −0.5749 | −0.5749 |
  | off vs on(16) | gb_normal | 1.0000 / 1.0000 | 0.6777 / 0.2500 | 0.2255 | 0.2255 |
  | off vs on(16) | gb_worldpos | 1.0000 / **0.0001** | −45.24 / −0.0037 | 0.0045 | **0.9987**(106 px) |
  | off vs on(64) | albedo | 1.0000 / 1.0000 | 0.2009 / 0.4055 | −0.0678 | −0.0678 |
  | off vs on(64) | gb_normal | 1.0000 / 1.0000 | 0.6777 / 0.2252 | 0.2564 | 0.2564 |
  | off vs on(64) | gb_worldpos | 1.0000 / **0.4443** | −45.24 / −18.44 | 0.7935 | **0.9292**(921,399 px) |
  | off vs on(64) | gb_lightmapkey | 1.0000 / 0.4443 | 5.17 / 119.29 | 0.3038 | 0.7133 |

  **差异来源（逐条解释，不要求逐位一致）**：
  1. **覆盖范围**：`worldpos` 的覆盖率就是模块真正写了几何的比例 —— 阈值 16 时只有 0.01%（61 簇），
     阈值 64 时 44.43%。剩下的是"清屏值"（worldPos=0、albedo=(0,0,0,1)）⇒ 不能与既有路径逐位比。
  2. **几何不同**：模块画的是剔除链的**合成实例网格**（同一份合并几何的 61 个平移副本，
     §14.22③），既有路径画的是场景本体 ⇒ 双方都覆盖的像素上世界坐标相差一个实例平移
     （RMS 296），`gb_worldpos` 的"全图相关"因此被背景主导（0.79），而"双方覆盖"上是 0.93。
  3. **材质是中性常数**：`albedo` 恒 0.8（任务 19 才接真实材质）⇒ 与既有路径的贴图 albedo 相关为负，
     这是预期的。
  4. **法线**：模块写的是**量化法线**（八面体 10+10 位，单测实测最大角误差 0.2017°）+ 透视校正插值，
     既有路径用插值后的世界法线 ⇒ 分布相近但逐像素不同（相关 0.23~0.26）。
- **"白炉 1.0000" 的等价自洽检验**（07.Nanite 的炉子路径是 GI 源自身的标度，与"谁写 GBuffer"无关，
  不能用来判软光栅）⇒ 在**软光栅自己的输出**上做三条逐位自洽检验（`nanite_on64` 的 921,399 个
  被写像素）：
  1. **albedo 逐位等于中性材质常数（f16 位相等）= 1.000000**、`metallic == 0` = 1.000000；
  2. **法线单位长度**（|n| ∈ [0.999,1.001]）= **1.000000**，最大 |len−1| = 8.3e-4；
     `roughness == 0.5` = 1.000000；
  3. **光照图键**：`uv ∈ [0,1]` = 0.999995；**页号 = 1024 + 实例下标、整数、落在 Nanite 段** = **1.000000**
     （页号 1025..1087，61 个不同实例）。
  这三条正是"模块自己写的内容与其解码来源（中性常数 / 量化法线 / UV / 实例号）一致"，
  等价于设计里"白炉 1.0000"这种**逐位自洽**判据。
- **P2 的按段分类得到真实生产者**：`python Tools/gi/lightmap_key_check.py build/verify nanite_on64`
  ⇒ 4/5 判据 PASS，其中两条正是本次要修的：
  `page values are exact integers -- 100.0000%`、
  `page values fall inside the object-index segments -- normal 0 px + nanite 921399 px,
  no out-of-range page`，并打印 `nanite 61 page(s), 921399 pixels, global page [1025,1087],
  local index [1,63]`。第 5 条（`key uniqueness @128x128`，17.17% > 10%）是**既有的报告项**：
  模块一帧里叠了 61 个平移副本 ⇒ 同一 texel 上必然有多层表面，是**场景属性**而非键编码回归。

**⑤ 偏差与风险（不掩盖）**
1. **`vuid_lines` 41 → 46（+5）**：全部是 `VUID-vkCmdDraw-None-09600` 的**启动期布局告警**
   （与基线 41 行里的 6 条同一 VUID/同一类）；开档因为多了"颜色目标在 UAV/附件布局之间往返"，
   命中校验层"同一 VUID 最多 10 条"的封顶后**构成变了**（8 条 COLOR_ATTACHMENT + 2 条 SHADER_READ，
   基线是 6 条 SHADER_READ）⇒ 行数 +4，另 +1 是封顶提示行。**没有新的 VUID 类型**，
   类目集合与基线逐条相同；`nanite_soft_raster=0` 档（模块开、软光栅关）实测仍是 **41**，
   证明增量确实来自软光栅这条路径。**未修**（需要动 RHI 的 render pass 缓存/布局追踪，超出本任务
   改动面），列为后续项。
2. **软光栅的深度精度**：深度键的高 24 位 = NDC 深度位模式（截掉低 8 位 ≈ 256 ulp）；
   深度解析把它还原进深度附件 ⇒ 深度比既有路径略粗（本任务不逐位对照深度，未量化）。
3. **阈值 16 下覆盖率 ~0.01%**：Sponza 的簇绝大多数是满簇（64 tri），这是 §5.2 阈值的直接后果，
   不是通路问题（阈值 64 档覆盖率 44.43% 即证据）；混合光栅分流是任务 22。
4. **模块不写 velocity/emissive/disneyA/disneyB**：这 4 张只有清屏值（与既有路径的清屏值相同），
   于是 TAA 的 motion vector 恒 0、emissive/AO/Disney 参数为默认 —— 本任务的明确边界（任务 19/21）。
5. **深度解析恒执行**（全屏片元，每帧 1 次全屏片元调用）；`depth_written=1` 是恒真的读数，
   "能不能用 compute 写深度"由 `depth_storage_image_supported` 单独如实报告。
6. **本次踩到并修掉的三个真 bug（值得全仓借鉴）**：
   ① 边函数**手性写反**（`(p-pk).x*e.y - (p-pk).y*e.x` 与 `area2 = e.x*b.y - e.y*b.x` 不同向）
   ⇒ 三角形内部的三个边函数全为负、一个像素都不覆盖（实测 61 三角形 / 9767 次像素测试 / 0 覆盖）；
   ② 帧图 lambda **按引用捕获局部变量** `naniteGB`（lambda 在 `BuildFrameGraph` 返回后才执行）
   ⇒ 纹理句柄是悬空垃圾指针（albedo 有效、normal/depth 为 0、其余像栈地址）；
   这两个都是"能编译、能跑、结果全 0/全错"的静默失败。
   ③ 渲染通道清屏在本引擎**不可靠**（render pass 按格式组合复用，Decal 的 `Load` 变体会被复用）。
7. **一处观察（不在改动面内）**：剔除链的 Hi-Z 采样用 `s = ndc.xy*0.5+0.5`（`s.y` 当纹理 V 用），
   而本引擎的离屏通道用**负高度视口**（NDC y=+1 落在帧缓冲第 0 行）⇒ `s.y` 与纹理行是**镜像**的。
   软光栅因此用 `row = (0.5 − 0.5*ndc.y)*H`（已按真实行约定写，并在注释里写明）。
   任务 15 的 Hi-Z 遮挡判据疑似受同一问题影响（深度以 0.99 背景为主时不易暴露），
   **本次不改**（会改动任务 15 的已验收读数），列为后续项。
   → **已在 §14.28（P0）核实并修掉**（结论：确实镜像）。

### 14.28 任务 15 的 P0 修复：Hi-Z 采样 UV 的 y 镜像（2026-09-21）

**① 结论**：`Nanite_ClusterBVH.comp.slang` 的 `hizOccluded` 用 `s = ndc.xy*0.5+0.5` 采样 Hi-Z 纹理，
其中 `s.y` 当纹理 V 用。本引擎的离屏通道用**负高度视口**（`GBufferRenderer_CPU.cpp:61`
`SetViewport({0,h,w,-h,0,1})`；`GBufferRenderer_GPU.cpp:64`、`LightingPass.cpp:165` 同款）
⇒ NDC y=+1 落在帧缓冲**第 0 行**、纹理 V 向下增长 ⇒ **确实镜像**，正确写法是
`s = float2(ndc.x*0.5+0.5, 0.5-0.5*ndc.y)`。

**② 只读证据**（三条独立来源）：
   · 同引擎内**已被实测验证**的同一条约定：`GI/SSR.frag.slang:55-68` 的 `NdcToUv` 就是翻转版，
     注释记录了那次真实镜像 bug 的实测（重建 viewPos.y=+70.3 vs 真值 −70.7，改正后射线 2 步命中）；
   · 同模块软光栅自己用的行约定 `Nanite_SoftRasterCommon.slang:126-134`（`row=(0.5-0.5*ndc.y)*H`）；
   · 既有 GPU 剔除 `GPUCull_TwoPhase.comp.slang:32` 有**同一个错**，但 `useTwoPhase` 恒 false
     ⇒ 该 pass 从不注册（§14.24①），不能当"正确写法"的对照。

**③ 改法**：`hizOccluded` 按 `CullChainParams.misc.w`（`hiz_flip`）选择 v 方向，**默认 1 = 翻转（已修）**；
`NaniteSettings::hizFlip` + cfg 键 `nanite_hiz_flip`（默认 1）保留 0 档，专供"镜像是否真的存在"的
可复现 A/B 对照。新增读数：`hiz_flip=<0|1>`、`occl_uv=[上半屏,下半屏]`（分类用**投影包围盒中心的
原始 ndc.y**，与采样 UV 约定无关，避免"用错的约定自证对的结论"）、
`hiz_half=[上半屏均值,下半屏均值]`（Hi-Z mip1 的 32×32 采样均值 ×1e6，用来证明"遮挡物只在一侧"）。
CPU 侧 `NaniteProjectSphereToScreen` 同步改成同一条约定（生产路径 Hi-Z 恒关，行为不变）。

**④ 实测（07.Nanite，只切换 `nanite_hiz_flip`，其余逐位相同）**
   · 单侧遮挡物构造（`cam_near=20; cam_pos_y=60; cam_pitch=-0.3` ⇒ 下半屏近、上半屏远）：
     `hiz_half=[0.956015,0.825267]`（Δ=0.131，单侧成立）；
     `flip=1`：`occluded=18904 occl_uv=[18894,10] occl_mip=[0,0,0,0,0,26,1177,3373] mismatch=4576`；
     `flip=0`：`occluded=63560(+236%) occl_uv=[63552,8] occl_mip=[0,0,0,0,3,140,2668,15834] mismatch=18645`
     ⇒ 多出的 44656 个"被遮挡"正是"zNear 落在 (0.825,0.956]"的簇（远半屏），只有在采样深度从
     自己那半被换成**镜像那半**时才可能出现；这些簇所在屏幕位置没有近遮挡物 ⇒ **误剔除**。
   · 其他相机复跑（同一 A/B）：`cam_pos_z=±400 pitch=0`：94032→65565、95164→70905；
     `pitch=+1.2 & near=20`：36005→49251（上半屏占比 25%→35%）。
   · 默认相机（`nanite_soft_raster=0`）：32031→56346，且 `flip=0` **逐位复现 §14.24 的冻结读数**
     （`phase2=64215 phase3=18888 occluded=56346 occl_mip=[0,0,0,0,0,244,3000,9516]`）
     ⇒ 开关切换的正是历史约定本身。
   · 对照：`nanite_hiz=0` 时 flip=0/1 的 cull3 行**逐字相同**（`mismatch=0`）⇒ 开关只影响 Hi-Z 采样。

**⑤ 自证**：两 target 构建 EXIT=0；单测 308/308；关闭档 12 pass / `vuid_lines=41` / 指纹
`1C15AB72E688B530…`（冻结）；开启档默认 14 pass / `vuid_lines=46`（= 任务 18 现状，无新增）/
`passlist_sha=750CC247BF8B9C3D…` / `cull3 … mismatch=0 … hiz_flip=1`。

**⑥ 偏差与风险（不掩盖）**
   1. 本引擎 `near=0.1 / far=2000` 的 ZO 投影把整场景压到深度 0.999x ⇒ 遮挡判定本身工作在
      深度精度边界附近（`hiz_half` 两半差异只有 1e-5~1e-1 量级，取决于相机）；本任务的判据
      因此用"**只切换 UV 约定**"的 A/B（同一帧、同一深度场），而不是绝对遮挡数量。
   2. 场景无法造出"上下半屏深度差足够大 + 簇本身两半都有"的教科书式反转（合成实例本质是
      同一份 Sponza 网格的微小平移副本 ⇒ 簇的世界位置与深度场强相关），故证据形态是
      "镜像把远半屏的簇按近半屏的深度判掉"，而不是"遮挡分布整体上下互换"。
   3. `hiz_flip=0` 档保留是**为可复现对照**，不是可选项的推荐值；默认恒为 1。

### 14.29 任务 19 实施记录：真实材质接入（2026-09-21）

**① 簇 → 源网格 → 材质的映射（本任务的关键）**
   · 源区间来自 `MeshBatcher::GetDrawCommands()`（`firstIndex/indexCount`；索引已加 baseVertex，
     `MeshBatcher.cpp:53`）⇒ 三角形区间 `[firstIndex/3,(firstIndex+indexCount)/3)`，首尾相接、升序；
     簇的区间 = `NaniteClusterRecord::triangleOffset/triangleCount`。
   · 新规则函数 `NaniteAssignClusterMaterials`（`NaniteUpload.{h,cpp}`，RHI-free、可单测）：
     **三角形多数票**归属；**平票取下标更小的网格**；一个三角形都落不进任何区间 ⇒ `unmappedClusters`
     并兜底 0 号材质；跨 ≥2 个网格 ⇒ `multiMeshClusters`（如实计数，不隐藏）。二分查找，
     复杂度 O(簇×三角形×log 网格)，无哈希容器遍历序 ⇒ 同输入逐位一致。
   · 逐网格材质快照新增在 `MeshBatcher`（`MergedMeshMaterial`，与绘制命令**同序同长**，
     在同一个 `collect` 调用里产出）：字段与 `SceneRenderer.cpp:110-130` 填 `GPUObjectData`
     时同一批来源（因子取组件字段、纹理路径按 `ComputeMaterialTextureMask` 压成掩码、
     `bindlessTextureBase = MeshComponent::materialID`）。**只增不改**：`IndirectDrawCommand`
     一个字节未动。
   · 【与任务书的偏差，如实报告】任务书写"`MeshBatcher` 的合并几何带逐网格材质索引"——实测
     `MeshBatcher.cpp:58` 只写 5 个绘制参数，**没有材质字段**（材质索引原本只存在于 GPUScene 的
     `materialIndex`，而 `u_Objects` 那条缓冲还被视锥剔除压缩过、下标与合并几何不对应）。
     因此按"最小侵入"在 `MeshBatcher` 加一份**只读快照**（它本来就是合并几何的唯一产地），
     而不是去依赖 GPUScene 的下标对齐。

**② 材质段：8B → **32B**（先报告后扩展，最小可行）**
   §8/任务 7 的 8B 只有两个 bindless 纹理 ID，放不下 GBuffer 路径逐项对照所需的字段
   （`baseColorFactor.rgb` 15B 语义 + 两个因子 + 纹理掩码）。定稿为
   `{float4 baseColorFactor; float metallicFactor; float roughnessFactor; uint textureMask; uint bindlessTextureBase;}`
   （16B 对齐，`static_assert` 钉住 5 个偏移）。软光栅按掩码决定是否采样：
   BaseColor 槽 0、MetallicRoughness 槽 2（与 `GBuffer.frag.slang:32` 的槽位约定一致）。

**③ 软光栅取真实材质（不再有中性常数）**
   · `softRasterEvaluateMaterial`（`Nanite_SoftRasterCommon.slang`）逐句复制
     `GBuffer.frag.slang:57-81` 的公式：`albedo = baseColorFactor.rgb × Sample(BaseColor,uv)`、
     `metallic = metallicFactor × Sample(MR,uv).b`、`roughness = clamp(roughnessFactor × Sample(MR,uv).g,0.04,1)`；
   · 采样必须用 `SampleLevel(...,0)`：compute 入口 `[numthreads(16,1,1)]` 没有 2×2 派生组，
     `Sample` 会报 `E31210`（实测）。**代价如实记**：只采 mip0，既有片元路径有隐式 LOD。
   · 材质段用**普通 SSBO（binding 12）**绑定；纹理/采样器数组（binding 13/14）走
     `heap->RegisterDescriptorSet(set,13,14,0)` 接到**引擎同一个 bindless 堆**。
   · 【踩到并修掉的一个真 bug】软光栅开启时 `GBufferRenderer_CPU::Render`（唯一每帧
     `heap->Flush()` 的地方，`:29`）**让位不执行** ⇒ 本集合的 bindless 数组永远是未绑定，
     实测症状是"albedo 全 0、roughness 落到下限 0.04"（不是崩溃，是静默错值）。修法：
     登记后模块**自己 Flush 一次**（先 `RegisterTexture(nullptr,nullptr)` 标 pending）。
   · 读数：`neutral_material_pixels`（任务 18 的口径）**恒 0**；新增
     `material_pixels`（真从材质段取到材质的像素）、`fallback_pixels`（越界/缺失而兜底，正常 0）、
     `materials`（材质段条数）、`distinct_materials`、`textured_materials`、`multi_mesh_clusters`；
     另加一行一次性 `materials_sample m0=(…) m1=(…) cluster_material_id=[min,max,distinct]`。

**④ 验收证据（真实 GPU 读回 + 同场景同相机转储对照，参考档 = `nanite_off`）**
   · 阈值 16：`soft=61 pixels_written=3264 material_pixels=3264 neutral_material_pixels=0 fallback_pixels=0`；
     阈值 64：`soft=31648 pixels_written=44318207 material_pixels=44318207 neutral=0 fallback=0`
     （`material_pixels == pixels_written` 逐档成立）。
   · 映射读数：`materials=103 distinct_materials=103 textured_materials=103 multi_mesh_clusters=97`；
     资产上传 `upload_bytes=13406096 readback_match=1 mismatch_bytes=0`（材质段 103 条 ×32B）。
   · 材质**逐项对照**（`build/verify/p1_matcmp.py`，阈值 64 的 919956 个写入像素；
     参考 = 既有路径全屏）：
     | 字段 | 参考 mean/std/min/max | 测试 mean/std/min/max | 直方图相关(32 bin) |
     |---|---|---|---|
     | albedo RGB | 0.2556 / 0.1310 / 0 / 0.5874 | 0.1771 / 0.0862 / 0 / **0.5879** | 0.5302 |
     | metallic | 0.0368 / 0.1454 / 0 / 1.0 | 0.0108 / 0.0799 / 0 / 1.0 | **0.9992** |
     | roughness | 0.7709 / 0.1752 / 0.04 / 1.0 | 0.4477 / 0.2304 / **0.04** / **1.0** | 0.3112 |
     · 极值一致（albedo max 0.5879 vs 0.5874；metallic/roughness 的 0/1 与 clamp 下限 0.04 完全一致）
       与 metallic 直方图相关 0.9992 是"同一批来源"的直接证据；
     · 均值差异来自**覆盖率与几何**（模块只画 LOD 选中的 44.37% 像素、且画的是合成实例的
       平移副本，§14.27 已记录的偏差），不是材质来源不同；逐像素相关为负(-0.3452) 同理。
     · 中性常数已消失的硬判据：三字段恰为 (0.8,0,0.5) 的像素 = **0**（阈值 64 全图）。
   · 既有口径对照（`nanite_soft_cmp.py`）：`gb_worldpos corr(双方覆盖)=0.9293`（与任务 18 的
     0.9292 同量级）、`gb_lightmapkey` 页号全部落在 Nanite 段 `[1025,1087]`。

**⑤ 自证**：两 target 构建 EXIT=0；单测 **309 例**（新增"材质记录 32B"重写 + "簇→源网格映射"
用例；62169 断言）全绿；关闭档 12 pass / `vuid_lines=41` / 指纹 `1C15AB72E688B530…`；
开启档 14 pass / `vuid_lines=**46**`（与任务 18 相同，**无新增**）/ `passlist_sha=750CC247BF8B9C3D…`。

**⑥ 偏差与风险（不掩盖）**
   1. 只采 **mip0**（compute 无派生组）⇒ 与片元路径的隐式 LOD 在贴图高频区域会有差异。
   2. 合成实例是"整份合并网格的平移副本"（§14.22③）⇒ 与既有路径**不能逐像素比材质**；
      本任务给的是分布/极值/直方图对照 + 映射规则单测 + 像素计数闭合。
   3. `alphaCutoff`、法线贴图、ostex/emissive 仍未接入（模块不写 MRT2、不做 alpha 测试）——
      保持任务 18 的通道边界；`NaniteMaterialRecord` 里因此**没有**这两个字段（如实报告）。
   4. `MeshBatcher` 的快照是"新增只读数组"，但它是本任务唯一改到的**非 Nanite 模块**文件
      （只增成员/getter + 在既有 `collect` 里多填一条记录，未改任何既有结构体与行为）。
   5. bindless 纹理数组因"强制一次 Flush"永久多一个占位槽（不影响任何已分配的材质 ID）。

### 14.30 任务 20 实施记录：深度与排序契约（两处 P0 修复，2026-09-21）

> 本节记录**任务 20（深度与排序契约）**。开工时的第一件事是复核基线，结果发现**判据 ⑦ 的 `hiz1`
> 档在任务 18/19 之后已经变红**（`occluded=0`）—— 基线不是绿的，所以本轮先把它修绿，再谈契约。

**① 开工基线（本人复跑，任务 19 之后的状态）**
- `acceptance_sweep.ps1`：判据 ①–⑥ 与 ⑧ 全 PASS，**判据 ⑦ FAIL**，理由是
  `tier hiz1 : occl_mip sum=0 (Hi-Z removed nothing: the criterion would be vacuous)` 与
  `gpu_clusters=31648 >= cpu_clusters=31648`。
- 读数（`07.Nanite`，`nanite_enable=1;nanite_hiz=1`）：
  `cull3 … occluded=0 occl_mip=[0,0,0,0,0,0,0,0] hiz_half=[1.000000,1.000000] hiz_mips=8`。
- 对照（`nanite_enable=1;nanite_hiz=1;nanite_soft_raster=0`，即模块在场但既有 GBuffer 路径仍写几何）：
  `occluded=32031 hiz_half=[0.999821,0.999795]`（**与 §14.28④ 记录的 32031 逐位一致**）。
  ⇒ 差异只在"谁写 GBuffer"：**模块接管后，深度附件恒为远平面 1.0**，Hi-Z 因此是一座空金字塔。

**② P0-A：`depthTest=false` 会让 `SV_Depth` 被整块丢弃（规范级错误）**
- 位置：`NaniteRaster.cpp` 的深度解析 PSO 建参处，原文 `desc.depthTest = false;` + 注释
  "深度值由 shader 决定 ⇒ 不做硬件比较"。
- **Vulkan 规范原文**（`VkPipelineDepthStencilStateCreateInfo::depthWriteEnable`）：
  "controls whether depth writes are enabled **when `depthTestEnable` is `VK_TRUE`**.
  Depth writes are **always disabled when `depthTestEnable` is `VK_FALSE`**."
  ⇒ 关掉深度测试就**同时关掉深度写入**，`SV_Depth` 一点也写不进去。
- 修法：`depthTest = true` + `depthCompare = CompareFunc::Always`（`Always` 恒通过 ⇒ 与"shader 写什么
  就是什么"语义完全等价，只是让写入真正生效）。
- 实测（只改这一处）：`occluded=0 → 12024`、`occl_mip=[0,0,0,0,0,0,122,3696]`、`mismatch=3818`。
  **但 `hiz_half` 仍是 `[1.000000,1.000000]`** ⇒ 还有第二个 bug。

**③ P0-B：`StructuredBuffer::GetDimensions` 返回的是"元素个数 + 1"，不是二维宽高**
- 位置：`Nanite_DepthResolve.frag.slang` 原文
  `u_DepthKey.GetDimensions(width, height);` 之后
  `if (pixel.x >= width || pixel.y >= height) { depth = 1.0; return; }`。
- 深度键是**一维** `RWStructuredBuffer<uint>`（长度 = 宽×高）⇒ `GetDimensions` 给出
  `width = 宽×高`、`height = 1`。于是 `pixel.y >= 1` 对**除第 0 行以外的所有像素**成立，
  深度解析只写了第 0 行，其余全部写成远平面 1.0。
- **三个判定实验（各自单独可复核，缺一不可）**：
  1. **把解析输出强制成常量 `0.5`**（临时改 shader，`.spv.h` 时间戳已确认重编译）⇒ 读数
     **逐字不变**。说明解析的输出根本没到达 Hi-Z；且那行常量写在第 0 行之后、大多数像素早已在
     早退里返回 1.0 ⇒ 与"只有第 0 行被写"完全一致。
  2. **把深度附件从 `Clear` 换成 `Load`**（临时改 PSO）⇒ 读数**逐字不变**。说明那个 1.0
     **不是清除值**造成的（若是，`Load` 会保留上一帧的真实深度）。
  3. **把 Hi-Z 的深度源临时换成 albedo**（临时改 `DeferredPipeline_FrameGraph.cpp` 一行）⇒
     `hiz_half=[0.000000,0.000000]`、`occluded=104579`。**证明 Hi-Z 构建确实在读它的输入纹理**
     （排除"构建器/绑定坏了"这条怀疑），于是问题只可能在"深度附件里到底是什么"。
     三个实验合起来把根因唯一地钉在 `GetDimensions` 上。
- 修法：屏幕尺寸由 CPU 用**显式 push constant** 传进来（新 POD `NaniteDepthResolveParams`，8B，
  `static_assert` 钉住尺寸/偏移），shader 不再从一维缓冲反推二维形状；顺带保留"宽高为 0 就按
  没有几何处理"的防御分支（不越界读）。

**④ 顺手修掉的第三、第四条**

**(4a) `depth_written` 是个恒真常量，正是它掩盖了 P0-B**
- 旧实现里 `depth_written` 在 C++ 侧**硬编码为 1**，注释写"深度解析恒执行（全屏片元写 SV_Depth）
  ⇒ depth_written 恒 1"。一个恒真读数把一个"一列都没写进去"的 bug 掩盖了一整个任务。
- 修法：深度解析通道自己 `InterlockedAdd` 计数（新增绑定 1 = 软光栅读数缓冲；新槽位
  `kNaniteSoftStatDepthResolvedPixels = 14`，C++/Slang 两侧同步 + 注释对齐）。
  语义是**去重后的像素数**（全屏片元对每个像素只访问一次），与第 2 趟的 `pixels_written`
  （通过等值复检的"簇×三角形×像素"写次数）不是同一个量；可核对的不变式是
  `depth_written <= pixels_written <= covered_px`（实测两档都成立）。
  交叉核对：阈值 64 档 `depth_written=921399`，与判据 ⑧c 的写标记计数
  （`gb_lightmapkey` 落在 Nanite 段的像素）**同一个数** —— 两条互不相干的路径给出同一个值。

**(4b) 判据 ⑧d 的启动期布局告警：接管档 46 → 42 行（模块内一行级修法）**
- 判据 ⑧d 的豁免口径里已经写明这条修法（`build\verify\nanite_takeover_cmp.ps1` 头部），
  但**代码里并不存在**（`git status` 干净、`NaniteRaster.cpp` 当时写的是
  `from = ResourceState::RenderTarget`）⇒ 判据 ⑧d 事实上卡在 `delta <= 1` 上。
- 修法：模块的清屏屏障把 8 张颜色目标的**源状态**从 `RenderTarget` 改成 `Undefined`。
  语义精确 —— 本 pass 是全屏 compute 清屏，8 张目标的每个像素都会被覆盖，"丢弃旧内容"
  正是要表达的；而写 `RenderTarget` 会在纹理刚（重）建、RHI 布局追踪器还没记录的那一帧被当真，
  让校验层记下"该命令缓冲期望 COLOR_ATTACHMENT_OPTIMAL"。实测 `vuid_lines 46 → 42`
  （`startup layout warnings` off=6 / on=7，`delta=1`），VUID 类目集合不变（`new=0`）。

**⑤ 验收证据（本人复跑，2026-09-21）**

| 档位 | `occluded` | `mismatch` | `occl_mip` | `hiz_half` |
|---|---|---|---|---|
| 修前（基线，接管 + 阈值 16） | 0 | 0 | 全 0 | `[1.000000,1.000000]` |
| 只修 P0-A | 12024 | 3818 | `[0,0,0,0,0,0,122,3696]` | `[1.000000,1.000000]` |
| **P0-A + P0-B（阈值 16，默认档）** | **4** | **4** | `[0,0,0,0,0,0,4,0]` | `[1.000000,1.000000]` |
| **P0-A + P0-B（阈值 64，全覆盖）** | **48150** | 11678 | `[0,0,0,0,0,183,2739,8756]` | `[0.999845,1.000000]` |
| 对照：`nanite_soft_raster=0` | 32031 | 7498 | `[0,0,0,0,0,61,1950,5487]` | `[0.999821,0.999795]` |

- **默认档（阈值 16）下 Hi-Z 恢复为"真的在按深度剔除"**：`extra_gpu=0`、
  `mismatch == sum(occl_mip)`（4 == 4）、`gpu_clusters + mismatch == cpu_clusters`（31644+4=31648）、
  `gpu < cpu`、`sum > 0` —— 判据 ⑦ `hiz1` 档的全部守卫**现在都成立**（修前 `sum=0` 直接判空转）。
- **阈值 64 档**给出"深度场完整时"的量级（48150），与对照档（32031）同量级，且 `hiz_half`
  不再是 1.0（`0.999845`）⇒ 深度附件里确实有几何深度。
- 真实读数（不再恒 1）：阈值 16 ⇒ `pixels_written=3053 depth_written=101`；
  阈值 64 ⇒ `pixels_written=44318207 depth_written=921399`（**不变式成立**：
  `101 ≤ 3053 ≤ 3053`、`921399 ≤ 44318207 ≤ 314423307`）。
- **判据 ⑧（任务 20/21 的画面级对照）随之回到 PASS**：`(8e)` 开启档去掉 Nanite pass 后与关闭档
  **逐行相同**、sha 命中冻结值 `1C15AB72E688B530…`；`(8a)` `V=C=D=R`、`material_pixels ==
  pixels_written`、`neutral_material_pixels=0`；`(8b)` 差异**恰好**是 4 张 GBuffer 目标 + 3 个抖动族
  （`unexpected=0 missing=0`）；`(8c)` 阈值 64 档 `gb_worldpos corr=0.9290 ≥ 0.90`、
  `metallic 直方图 corr=0.9993 ≥ 0.99`、roughness 边界与参考相同、写标记 **921399 px**；
  `(8d)` `vuid_lines off=41 on=42 delta=1`、`new VUID type=0`。
- **判据 ⑦（任务 17）**：`CULL DIFF: PASS`，五档全 OK，其中 `hiz1` 档
  `visible=31644 mismatch=4 gpu/cpu=31644/31648`（修前是 `sum=0` 判空转）。
- **全量八条判据**（`powershell -NoProfile -ExecutionPolicy Bypass -File build\verify\acceptance_sweep.ps1`）
  一次跑完 ⇒ **`ACCEPTANCE SWEEP: PASS`**：
  ① 白炉 `prov6_final` `min=mean=max=1.0000`、`rgb_mean=(1.0000,1.0000,1.0000)`；
  ② 背靠背抖动族之外 **0 项差异**、`max ULP = 0`（有界漂移族 5 项，与既有口径一致）；
  ③ `lumen_passes=0`；④ 默认预设 vs `s37fin2` 抖动族之外 **0 项差异**；
  ⑤ `HugEngineTests` **309 例 / 62169 断言全绿**；
  ⑥ 关闭档 12 pass、`nanite_leak=0`、指纹 `1C15AB72E688B530…`（= 冻结值）、开启档 14 pass 且既有集合与顺序未变；
  ⑦ `CULL DIFF: PASS`；⑧ `TAKEOVER CMP: PASS`。
- **本轮的改动面**（4 个文件，逐条可回退）：`NaniteRaster.cpp`（PSO 深度状态 + 清屏屏障源状态 +
  深度解析 push constant + 读数）、`NaniteTypes.h`（`NaniteDepthResolveParams` + 新读数槽位）、
  `Nanite_DepthResolve.frag.slang`（push constant 取代 `GetDimensions` + 真实原子计数）、
  `Nanite_SoftRasterCommon.slang`（新读数槽位常量）。**渲染路径的 pass 集合、声明与顺序一个都没动**
  （判据 ⑥ 的冻结指纹与判据 ⑧e 的逐行对比即证据）。

**⑥ 一处明确的裁决：本次**没有**改判据 ⑦ 的档位定义**
- 一开始的判断是"`hiz1` 档在接管下测不出东西，应该改成 `nanite_soft_raster=0`"（与判据 ⑥c 的
  结构性理由相同）。**修完 P0-A/P0-B 后这个改法不再必要**：接管档自己就能给出
  `sum>0`、等式成立、`gpu<cpu` 的读数 ⇒ 判据保持"测真正在生产的那条路径"，比换档更严格。
  **不改档位、不放宽任何守卫。**
- 如实记录的弱点是：默认阈值 16 下只有 61 个簇（≈101 个像素）进入软光栅，
  所以 `occluded` 只有 **4** —— 数字薄，但**非空且三条等式逐项成立**。它偏薄的原因正是
  任务 18 的通道边界（>16 三角形的簇留给任务 22 的硬光栅、当前一个都不画）；
  **任务 22 落地后模块会补全整个深度场，该档会自然变成强判据**（阈值 64 档的 48150 就是预演）。

**⑦ 偏差与风险（不掩盖）**
1. **`pixels_written` 的平局口径未动**：它是"通过等值复检的（簇×三角形×像素）写次数"，
   而等值复检的键低 8 位只到"簇内三角形下标" ⇒ **两个不同簇的三角形若深度位模式与簇内下标
   都相同，会同时通过复检并各自写一次颜色**（最终颜色是竞态）。合成实例网格里同一深度层有多个
   平移副本 ⇒ 阈值 16 档实测 `pixels_written=3053` 而**去重像素只有 101**（30 倍）。
   这是 §14.27④ 已记录的"平局全序只在簇内成立"的直接后果，**本次不改**
   （修它要动键编码或加簇号位，属任务 21/22 的改动面）。
2. **`hiz_half` 在阈值 16 档仍是 `1.000000`**：32×32 网格采样 mip1 时命不中那 101 个像素，
   属采样粒度问题，不是深度没写（阈值 64 档的 `0.999845` 即反证）。
3. **深度精度**：深度键高 24 位 = NDC 深度位模式（截掉低 8 位 ≈ 256 ulp），现在真正落进
   深度附件 ⇒ 接管档的深度比既有路径略粗（与 §14.27⑤-2 同一条偏差，量级未变）。
4. 判据 ⑧ 的 `vuid_lines` 上界与类目判据在本次改动后**未新增类型**（见本轮验收读数）；
   若接管档转储里出现新的差异目标，属"深度变真实"的**预期后果**，按判据 ⑧b 的"四张 GBuffer
   目标 + 抖动族"口径核对（本轮实测见提交说明）。

**⑧ 任务 21（画面级对照验收）的收口**
- 任务 21 的验收是"开关 ON/OFF 两档对照：差异可解释（几何覆盖/材质），且关闭档与基线逐位一致"。
  它的落点是判据 ⑧（上一轮已建好的 `build\verify\nanite_takeover_cmp.ps1`），本轮**逐项复核并全绿**：
  - `(8a)` 读数闭合：`visible=indirect_count=draws=rasterized=31648`、`empty_draws=0`、
    `wiring_mismatch=0`、`material_pixels == pixels_written=3264`、`neutral_material_pixels=0`；
  - `(8b)` 差异范围：18 个转储里 11 个逐位相同，差异**恰好**落在 4 张 GBuffer 目标 + 3 个抖动族，
    `unexpected=0 missing=0` —— 这就是"差异可解释"的可核对形式；
  - `(8c)` 画面质量（阈值 64 档，模块写入的 921399 个像素）：`gb_worldpos` 双方覆盖相关系数
    **0.9290**（≥ 0.90）、`metallic` 32 桶直方图相关 **0.9992**（≥ 0.99）、roughness 边界与参考相同、
    中性材质像素 **0**；
  - `(8d)` 校验层：`vuid_lines off=41 on=42 delta=1`、`new VUID type=0`；
  - `(8e)` 关闭档与基线逐位一致：开启档 pass 列表去掉 `Nanite*` 行后与关闭档**逐行相同**
    且 sha 命中冻结指纹 `1C15AB72E688B530…`。
- 因此 **§14.8 的任务 20 与任务 21 均已完成**；本轮同时修掉了它们下面的两处 P0 深度缺陷。

### 14.31 任务 22 实施记录：mesh shader 硬光栅 + 软硬分流（2026-09-21）

**目标**：按 §5.2 让 `triangleCount > softMaxTriangles`（默认 16）的簇走 mesh shader 硬光栅，
`<= 16` 的仍走既有 compute 软光栅；验收 = 混合光栅画面一致 + 软硬占比可读。

**① 分流契约（两侧用同一份判据，故可证"并集全覆盖 + 交集为空"）**
- 软光栅：`Nanite_SoftRaster.comp.slang` 的 `if (triangleCount > maxTriangles) return;`（既有，任务 18）。
- 硬光栅：`Nanite_HardRaster.mesh.slang` 的 `if (triangleCount <= maxTriangles) HARD_RASTER_BAILOUT();`
  —— **同一个 push constant 字段、同一份 `triangleCount`**，只是判据取反 ⇒ 两集合互补。
- 新开关 **`NaniteSettings::hardRaster`（默认 `false`）**，cfg 键 `nanite_hard_raster`（默认 0）。
  **默认关是硬要求**，理由见 §14.11 的新增段（判据 8b 只允许 4 个 GBuffer 目标 + 抖动族变化）。

**② 深度次序：选方案 (b)（硬光栅排在软光栅三趟之后），两个方向遮挡都正确**
次序（`Nanite_CullChain3` 同一个 pass 体内，命令缓冲内显式屏障定序；**帧图零新增 pass**）：
`GB_Clear → 剔除链 + 间接绘制 → 软光栅两趟 → 深度解析 → 硬光栅(mesh)`。
硬光栅 PSO：`depthTest=true + depthCompare=LessEqual + depthWrite=true`、
**`colorLoadOp=Load` + `depthLoadOp=Load`**（Load 是关键：Clear 会抹掉软光栅刚写的结果）、
8 个颜色附件 + per-MRT `writeMask`（只写 MRT0/1/4/7）。
关键推导（**已复核并纠正了本文档上一轮的判断**）：
- `dh < ds` ⇒ 深度测试通过 ⇒ 硬颜色**覆盖**软颜色、深度改写为 `dh` ⇒ **硬遮软 ✓**；
- `dh > ds` ⇒ 测试失败 ⇒ 硬片元丢弃、软颜色保留 ⇒ **软遮硬 ✓**；
- 无软几何的像素 `ds = 1.0` ⇒ 硬照常写入 ✓。
⇒ **遮挡正确性只取决于"后写者是否带深度测试"，与"谁先写"无关**。方案 (a)（硬在前）才是真的只成立
一半 —— 软颜色趟的等值复检只对照**软自己的深度键**，看不见硬几何。方案 (c)（从 D32 播种深度键）
在 (b) 已两向正确时属纯增量风险，未采纳。

**③ 已知边界（如实记录，不是"完全等价"）**
1. 软深度键截断到 **24 位尾数**（`asfloat(key & 0xFFFFFF00)`）；`near=0.1/far=2000` 下远处 1 个桶
   ≈ 1.2e3 世界单位 —— 这是**软光栅既有精度**，不是本任务引入。实测受测帧在 **69.6%** 的像素上
   选中比全软档**更近**的面、4.2% 更远 ⇒ 硬光栅在中远处反而更准。
2. 深度恰好相等时 `LessEqual` 让**硬**胜出。
3. 硬光栅写深度附件 ⇒ 下游 Hi-Z / SSAO / SSR 结果变化 —— 这是"默认必须关"的第二个理由。
4. 覆盖率缺口 **3496 px 100% 落在远平面之外**（轴向距离 2070~2162 > `far=2000`）：软光栅按
   "不做近/远平面裁剪"照画，硬件按规范裁掉 ⇒ 这是**两套光栅的既有语义差**，不是缺陷。

**④ 软硬占比读数（真实 GPU 回读；受测档 `nanite_enable=1; nanite_hard_raster=1`，阈值 16）**
```
[Nanite] hard_raster clusters=31587 prims=2021446 pixels=308810322 fallback_pixels=0
         soft_clusters=61 soft_pixels=3264 skipped_big=31587
         hard_share_permille=999 soft_share_permille=0 max_triangles=16
         visible_capacity=1048576 mesh_supported=1 pso=ok
```
- `soft_clusters=61` / `skipped_big=31587` 与**改前基线逐位相同**（基线见 §14.11 的期望数值表）；
- 硬光栅 `clusters=31587` **== `skipped_big`** ⇒ 大簇 **100% 被接手**（不是"几百"）；
- `fallback_pixels=0` ⇒ 硬光栅写的是**资产材质**、没有退化成中性常数；
- `mesh_supported=1 pso=ok` ⇒ 走的是**真硬件路径**，不是"设备不支持"的降级；
- **交叉核对**：硬 `prims=2021446` vs 全软档 `triangles+degenerate=1758608+262899=2021507`（差 61 =
  小簇）；硬 `pixels=308.8M` vs 全软档 `covered_px=314.4M`（差 1.8%）。
- `hard_share_permille` 是**片段数**之比（含过绘），**不是屏幕覆盖率**，口径已在报告里写明。
- 关闭档（`hardRaster=0`）**不打印该行** ⇒ 关闭档日志与基线一致（既有纪律）。

**⑤ 画面一致性（A/B；A = 阈值 64 全软光栅，B = 阈值 16 + 硬光栅）**
用既有 `nanite_takeover_cmp.py` 与自制 `t22_ab_diag.py`（脚本在被 gitignore 的 `build/verify/` 下）：
| 指标 | A vs B | 噪声底（A 两次） | 对照（硬光栅开但 0 簇） |
|---|---|---|---|
| 写标记像素 | 917903（**99.62%** of A 的 921399） | 100% | 100% |
| `gb_worldpos` 相关（双方都写） | **0.9989** | 1.0000 | 1.0000 |
| `metallic` 直方图（双方都写） | **1.0000** | 1.0000 | 1.0000 |
| `metallic` 直方图（整幅 vs 整幅） | **1.0000** | 1.0000 | 1.0000 |
- **工具原文的 `[2] metallic hist = 0.6222 FAIL` 是口径不适用，不是回归**：该脚本的 hist 是
  "**参考帧整幅** vs **受测帧被写像素**"（为"模块 vs 引擎"设计），而 A 档整幅有 ~55% 是清屏值
  （`metallic=1.0`）⇒ 与"模块 vs 模块"不匹配。同口径与整幅对整幅重算均为 **1.0000**。
- **逐像素几何身份不成立**（如实记录）：92% 的像素由**不同实例**胜出 —— 本样例是 **64 份几乎同深度
  的重叠整场景拷贝**；在两条路径挑到**同一实例**的像素上，77.5% 的世界坐标差 < 0.01。
- 三组亮度/材质平均差（A/B、噪声底、对照）：`albedo 0.0228 / metallic 0.0048 / hdr 0.0252`
  vs 噪声底 `0.0095 / 0.0053 / 0.0042` ⇒ 分别 2.4× / 0.9× / 6×；`hdr` 的 6× 属抖动族。

**⑥ 构建与验收**
- 构建：`06.GILab` / `07.Nanite` / `HugEngineTests` 全 `exit 0`；单测 `Status: SUCCESS!`。
  （独立复建 `07.Nanite` 亦 `exit 0`。）
- `acceptance_sweep.ps1 -OnlyNanite`：改前基线 **PASS**；改后 **run1 PASS / run2 FAIL / run3 PASS**，
  独立复跑一次 **PASS**。
- **run2 的 FAIL 不是本任务引入**：它是 `tier on: smoke run TIMEOUT (sample killed after 300s)`，
  而该档 cfg 为 `nanite_hard_raster=0` ⇒ **根本不走新代码**（日志跑到第 117 帧、
  2.56 s/帧 vs 正常 0.2 s/帧，机器侧 10× 变慢命中脚本 300 s 上限）；同一次运行的判据 ⑥/⑦ 全 PASS。
- PASS 档冻结指纹逐项一致：`off passes=12 sha=1C15AB72E688B530`、
  `on passes=14 nanite_passes=2 sha=750CC247BF8B9C3D`、`on-minus-Nanite==off True`；
  8a `soft=61 skipped_big=31587`；8b `unexpected=0 missing=0`；8c `corr 0.9289 / hist 0.9993`；
  8d `vuid on=42 new=0`；判据 ⑦ `CULL DIFF: PASS`（五档全 OK）。
- **纠正一处预期**：`hardRaster=1` 档的 `nanite_passes` 仍是 **2**（不是 3）—— 硬光栅录在既有
  `Nanite_CullChain3` pass **体内**（与任务 16 的间接绘制、任务 18 的三趟同手法），帧图零新增，
  pass 列表 sha 与 on 档**逐字相同**。

**⑦ 顺带修掉的 3 个既有 RHI 潜伏缺陷（均为加法/纠错；冻结指纹逐字未动即"既有行为未变"的证据）**
1. **`kStageMaskMesh` / `kStageMaskAmplification` 值互换**（`Engine/RHI/RHI/Types.h`）：原为
   Mesh=64 / Amplification=128，而 Vulkan 真值是 `VK_SHADER_STAGE_TASK_BIT_EXT=0x40`(64) /
   `VK_SHADER_STAGE_MESH_BIT_EXT=0x80`(128)（已对照本机 SDK `vulkan_core.h:3150-3151` 核实）。
   任务 22 是**第一个**把 `kStageMaskMesh` 放进描述符集布局的调用者 ⇒ 立刻
   `vkCreateGraphicsPipelines` 失败并随后 `EXCEPTION_ACCESS_VIOLATION` 崩溃。已按真值改正。
2. **`SetPushConstants` 对图形管线只发 `VS|FS`** ⇒ mesh 阶段读不到 push constant；改为
   布局与发送使用同一份并集 `VS|FS|Mesh|Task`（`VulkanCommandList.cpp` + `VulkanPipeline.cpp`）。
3. 新增 `PipelineStage::{MeshShader, TaskShader}`，让"清零 → mesh 原子累加"的屏障**显式**表达
   （此前只能靠掩码为空退化成 `ALL_COMMANDS`）。

**⑧ 交付文件**
新增 `Engine/Shader/Shaders/Nanite/Nanite_HardRaster.{mesh,frag}.slang`（分别登记进
`MESH_SLANG` / `FRAG_SLANG`，构建日志确认真编了）；改 `NaniteSettings.h`、`NaniteTypes.h`、
`NaniteRaster.{h,cpp}`、`NaniteRenderer.{h,cpp}`、`Samples/07.Nanite/07.Nanite.cpp`（解析 / 面板 /
回写 / 日志四处齐备，cfg 往返已验证）、RHI 三文件。**未新增 C++ 文件**（`Engine/Render/CMakeLists.txt`
无需改）；**未改 `GBufferRenderer`**；模块内未引用 GI/Lumen/GPUCulling。

**⑨ 存疑未做（明确留给后续）**
- 方案 (c)（从 D32 播种深度键）、独立的帧图 pass、`DrawMeshTasksIndirectCount`（RHI 已就绪但本轮
  未接线，网格任务数仍由 CPU 给出上界 + shader 早退）；
- 深度附件的逐像素比对（转储清单里没有深度目标，只能给间接证据）；
- 软光栅 24 位深度键截断未改（属既有精度口径）；
- **性能优化属任务 23**：本轮实测硬光栅 **341× 过绘**、mesh 线程利用率约 **50%**；
- 屏幕上的软硬占比可视化属任务 26。

**⑩ 一条既有问题（非本任务引入，但影响判据 ⑧ 稳定性）**
模块接管档**两次相同运行并非逐位可复现**：20 个转储里 7 个不同（恰好 4 个 GBuffer + `hdr` +
`prov0_ao` ×2），同档两次之间 `lightmapkey.page` 平均差 18.8。根因是软光栅"深度键相同 ⇒ 多个
三角形都通过等值复检 ⇒ 由 UAV 写入顺序决定"，在 64 份重叠拷贝下被放大。这与 §14.11 记录的
判据 ⑦/⑧ 历史抖动方向一致，**建议另立一项**（会影响判据 ⑧ 的可复现性）。

### 14.32 任务 24 前置设计：LOD 流式（反馈 + 页池）（2026-09-21 起草，待评审）

> **为什么单列一节**：§14.10 第 1 条记录「页面/流式：全文无 cluster page / page pool / 分页设计
> （只有里程碑名 N5）⇒ 任务 24 **必须先补设计**」，§14.8 任务 24 的验收也写明「先补设计评审，再定
> 验收」。本节即那份设计，**其验收条款需评审后才定稿**（§14.8 任务 24 的原话）。
> 本节在任务 23 实施期间起草，避免与任务 23 的代码改动面重叠。

**① 问题陈述（为什么需要它）**
- 现状：`NaniteScene::UploadPackedAsset` 把**整个**资产的 4 个段一次性上传并**常驻**显存
  （实测 Sponza：`upload_bytes=13406096`、`clusters=8287`、`vertices=542190`）。
- 目标态（N5 里程碑原文）：「运行时 LOD 选择 + 反馈」，判据是「帧率稳定，无 pop」。
- ⇒ 需要的机制是：**按需驻留 + 反馈驱动 + 有界页池**，而不是"一次性全驻留"。

**② 页粒度：以「簇区间」定义页，页的字节范围由头部计数推出（无需新增落盘结构）**
- §8.3 定稿：`.nanite` 的段偏移**不落盘**，每段偏移与长度都是「头部计数 + 固定步长」的纯函数。
  ⇒ 任意簇下标区间在**簇段**里的字节范围算术可得：`96 + start×64`。
- 顶点段与索引段的范围**不能**由簇下标直接推出（每簇的 `vertexOffset` 是打包时分配的）。

> **⚠ 2026-09-21 修正（起草后复核代码得出，推翻了本设计的初始假设）**
> 本节初稿要求"离线打包保证 `cluster.vertexOffset` 随簇下标单调不减"，并据此把"簇下标区间"当作页。
> **该假设在正式路径上是假的**，复核过程与结论如下：
> - `cluster.vertexOffset` 有**两处**赋值：
>   - `NaniteUpload.cpp:387`（非去重路径）：`= result.vertexIndices.size()` 累计推进 ⇒ **单调**；
>   - `NaniteUpload.cpp:588`（**去重/DAG 路径**）：`= result.uniqueVertexOffset[uniqueIndex]` ⇒
>     `uniqueVertexOffset[]` 本身单调（`:572` 用 `uniqueVertexWords.size()` 追加），
>     但 `uniqueIndex` 来自哈希去重查找（`:540-567`），**命中时会把 `uniqueIndex` 指回一个更早的
>     共享内容** ⇒ 相邻簇的 `vertexOffset` **可以变小** ⇒ **簇下标区间不映射到连续的顶点区间**。
> - ⇒ 按初稿的守卫，正式资产（走去重路径）会直接判 `nonmonotonic` 并**永远禁用流式** ——
>   设计等同不可用。**必须改页的定义。**
>
> **修正后的页定义（默认项）：页 = 共享（去重后）数组上的一段，且对齐到"整份共享内容"边界。**
> - 因为 `uniqueVertexOffset[]` / `uniqueTriangleOffset[]` **本身是单调的**，所以"共享内容下标区间"
>   总能映射到连续的顶点/三角形区间。
> - 页 = `[共享内容下标 s, s+K)`（K 默认 512 份共享内容），其顶点/三角形字节范围由
>   `uniqueVertexOffset[s]` 与 `uniqueVertexOffset[s+K]` 直接给出（两端都在页内）；
>   **要求离线侧把每份共享内容按页边界对齐**（不足则补 padding），使任何一份共享内容**不跨页**。
> - 簇 → 页的映射用**它已经在用的那个 `vertexOffset`**：落在这个页的顶点区间内的簇就属于这个页。
>   于是**着色器侧不必新增索引空间**，只把 `cluster.vertexOffset` 经页表从"共享数组的绝对偏移"
>   换算成"页池内的偏移"（`poolOffset + (vertexOffset - pageBeginOffset)`）。
> - `clusterUnique`（`NaniteUpload.cpp:598` 的平行数组）**不参与**页的划分，也不要求上传。
> - **仍需的守卫**：上传时校验"每份共享内容不跨页"（对齐或 padding 是否真的做到了），
>   不成立则 `stream=off reason=page_straddle` 并退化到整段常驻 —— 与初稿"不静默、不崩"的口径一致，
>   但**判据换成了可成立的那一条**。
> - **权衡（如实记录）**：按"共享内容"分页会比按簇分页产生更小的页（簇的记录段仍可按簇分页，
>   但顶点/三角形段按共享内容分页）⇒ 页表要能同时描述两类页，或**统一把页定义为"簇段 + 顶点段 +
>   三角形段三段各取一段"**（即一个页 = 三个区间）。实现时应选后者，避免两套页语义。
>
> **页对齐后的一个必要补充：着色器怎么知道"我在哪一页"（默认项）**
> 因为页对齐到"整份共享内容"边界后页的顶点条数是**可变的**，`vertexOffset / recordsPerPage`
> 这类除法**不成立**，也不该在着色器里做二分查找。默认做法：
> **上传期在 CPU 侧构建一份 `u32 pageOfCluster[clusterCount]`**（每簇一个页号；Sponza 实测
> 8287 簇 ⇒ 约 33 KB），与资产缓冲一起上传。着色器只做两次查表：
> `page = pageOfCluster[clusterIndex]` → `slot = pageTable[page]` → 偏移换算。
> 该数组是**纯函数**（由页划分唯一决定），所以可单测、可离线校验，且**不影响**任何既有缓冲的语义。
>
> **⚠ 2026-09-21 追加：页表项必须携带"页起点"，否则着色器算不出页内偏移（设计缺口）**
> 本节初稿把页表写成 `slot[pageIndex] = { resident, poolSlot, lastRequestedFrame }` —— **缺一个字段**。
> 因为页的可变长度（对齐到共享内容边界 ⇒ 每页顶点条数不同），着色器拿到
> `cluster.vertexOffset`（**共享内容数组的记录下标**，见 `NaniteTypes.h:530` 与
> `NaniteTypes.slang:143`「共享内容的下标」）后，**只有知道该页的起点**才能算出页内偏移：
> ```
> 页内偏移 = cluster.vertexOffset − pageBeginVertex
> 池内地址 = entry.poolBase + 页内偏移          （三角形段同理，用 pageBeginTriangle）
> ```
> 默认做法：**页表项改为携带 `poolBase` / `pageBeginVertex` / `pageBeginTriangle`**（页数很小，
> Sponza 实测 `ceil(8287/512)` ≈ 17 页 ⇒ 表本身可忽略）。**不要**退化成"等分页 + 乘法"：
> 那要求每份共享内容不跨页且页长固定，等于给每次打包加填充约束（改离线产物），成本高于多两个 u32。
> 若实现者选择别的等价方案（例如另开一个 `pageBegin[]` 平行数组），请在 §14.37 里说明并给出理由。
>
> **⚠ 2026-09-21 追加（第二次）：侵入点不止"软/硬光栅里的几行"，实测为 8 处、跨 4 个着色器文件**
> 初稿写"这是本任务唯一侵入既有着色器的地方：软光栅与硬光栅取顶点/三角形的那几行"，**低估了**。
> 逐处清点（`Engine/Shader/Shaders/Nanite/`）：
> | # | 位置 | 内容 |
> |---|---|---|
> | 1 | `Nanite_SoftRasterCommon.slang:244` | `u_Triangles[cluster.triangleOffset + triLocal]` |
> | 2 | `Nanite_SoftRasterCommon.slang:246` | `uint base = cluster.vertexOffset;` |
> | 3 | `Nanite_SoftRaster.comp.slang:73` | 第 2 趟**再次**取三角形（未复用上面的公共函数） |
> | 4-6 | `Nanite_SoftRaster.comp.slang:75-77` | 三个顶点 `u_Vertices[cluster.vertexOffset + local.x/y/z]` |
> | 7 | `Nanite_HardRaster.mesh.slang:142` | `u_Triangles[cluster.triangleOffset + triLocal]` |
> | 8 | `Nanite_HardRaster.mesh.slang:144` | `uint base = cluster.vertexOffset;` |
> **另有一条初稿完全没提的链路（重要）**：
> - `Nanite_ClusterBVH.comp.slang:453` 把 `range.vertexOffset` **原样写进间接绘制命令**；
> - `Nanite_Raster.vert.slang:13-14,38-39` 的占位光栅**从命令里的 `vertexOffset` 推导三角形顶点**
>   （`v = vertexOffset + 3k`）。
> ⇒ **间接绘制命令里的 `vertexOffset` 也是偏移空间的一部分**。任务 16 起 `visible=indirect_count=draws`
> 说明这条链**每帧都在真跑**（不是只有假簇链才用），所以流式必须同时决定：
> **(i)** 命令里写"共享数组的绝对偏移"再让顶点着色器翻译，还是 **(ii)** cull 阶段就写"池内偏移"。
> 两者都可行，但**必须显式选一个并写进 §14.37** —— 只改 1–8 而漏掉命令链，会出现"软/硬光栅正确、
> 但占位光栅用错偏移"的静默错误（画面看不出来，只有读数与判据 8a 的 `rasterized` 会露出来）。
> **建议的默认项**：选 **(ii)**（cull 阶段按页表写出池内偏移），因为命令是一次性写出的，
> 在那里翻译只影响 1 处，而 (i) 要在顶点着色器里再翻译一次、多一处可能不一致的地方。
>
> **页边界从资产本身即可推出（2026-09-21 追加，去掉了对新增落盘数据的依赖）**
> 复核 `NanitePackedAsset`（`NaniteUpload.h`）的字段后确认：它保留了
> `clusters`（64B/出现）、`vertices`（**每个唯一内容一份**）、`triangles`、`materials`、`lodOffsets`
> —— 但**没有**保留构建期的 `uniqueVertexOffset[]` / `uniqueTriangleOffset[]`。
> 不过页边界**不需要**它们：把 `clusters[i].vertexOffset` 取**去重后的升序集合**，
> 相邻两个不同值之间的区间就是一份共享内容的范围（最后一份到 `vertices.size()` 为止）；
> 每份内容至少被一个簇引用（内容是**因簇的需要**才创建的）⇒ 该集合完整。
> ⇒ **`pageOfCluster` 与页起始偏移都能在上传期由资产自身算出**，不必改 `.nanite` 格式、
> 也不必保留新的构建期临时数组。**这是本设计不需要动文件格式的关键依据。**

- 页大小默认 **512 簇/页**（可配 `kNanitePageClusters`）；页数 = `ceil(clusterCount / 512)`。
- LOD 维度：`lodLevelCount` + LOD 偏移段（§8.3 段 5）给出每个 LOD 的簇下标范围 ⇒
  "某 LOD 的某几页"是一次**页区间查询**，不需要额外索引。

**③ 页表与页池**
- **页池**：固定容量 `kNanitePagePoolSlots`（默认 64 页）的物理页数组，每槽容纳"一页的簇记录 +
  其覆盖的顶点 + 索引"三段的字节数上界（上界由 `512 × 64B` + `512 × 128 × 16B` + `512 × 64 × 3 × 2B`
  推出；实际按资产的每簇均值更省，但**上界**是容量计算与越界防护的依据）。
- **页表**：`slot[pageIndex] = { resident:bool, poolSlot:u16, lastRequestedFrame:u32 }`。
- **间接层**：光栅端**不直接**用资产的 `cluster.vertexOffset`，而是经页表换算到页池内的偏移。
  ⇒ 这是本任务唯一**侵入既有着色器**的地方：软光栅与硬光栅取顶点/三角形的那几行要改成
  "经页表换算"，并且**必须与既有"整段常驻"路径给出逐位相同的结果**（见 ⑥ 的验收）。

**④ 反馈通路（GPU → CPU）**
- GPU 侧：剔除链在选簇时，若目标页**未驻留**则把 `pageIndex` 追加进 `u_PageRequests`
  （定长环形缓冲，去重由"页表里的 requestedFrame == 当前帧"判定 ⇒ 无锁、无原子竞争放大）。
- CPU 侧：每 `kNaniteFeedbackLatency`（默认 2）帧读回一次请求集合（复用既有 `Map()` 读回路径），
  合并去重后进入上传队列。**读回不得阻塞**：只在到达延迟周期的那一帧读，且必须已 `WaitIdle`
  或处于既有 dump 路径的同步点（沿用任务 3 以来的口径）。
- 上传：按"优先级 = 本帧请求顺序 + 屏幕误差贡献"排序，每帧上传上限 `kNanitePageUploadsPerFrame`
  （默认 4）页，经既有 `JobSystem` 异步拷贝到页池槽位。

**⑤ 淘汰与优先级**
- 池满时淘汰 `lastRequestedFrame` 最旧且**未被本帧引用**的页（LRU）；淘汰前必须确认没有在飞命令引用
  它（复用既有 `DeferredDestructionQueue` 的"延迟 N 帧"思想，或直接按 3 帧在飞上限保守延迟）。
- **不做**预取/预测（无"下一帧大概率要看什么"的模型）；不做磁盘 I/O（见 ⑦ 的分阶段）。

**⑥ 页缺失时的渲染行为（必须先定，否则会崩或闪）**
- 未驻留的簇**跳过不画**，并按页计进读数 `page_misses`；**不做**占位几何、不做 fallback 到更粗 LOD
  （后者属 LOD 选择策略，任务 24 只做"有没有得用"，不做"用哪一级更好"）。
- 因此**"无 pop"这条 N5 判据在本任务只做到"没有未定义行为"**，真正的"无 pop"依赖 LOD 选择与
  预取，**明确留给后续任务**（见 ⑨）。

**⑦ 分阶段落地（本任务只做第 1 阶段；这是"默认项"的选择理由）**
- **阶段 1（本任务的范围）**：页池 + 页表 + 间接层 + 反馈 + 驻留管理 + 读数 + 开关。
  页数据源为**已在内存的完整资产**（把对应区段拷进页池）⇒ 不需要任何磁盘 I/O，
  这也回避了"Core 层无文件系统封装"这个既有缺口（扩展性分析已记录该缺口）。
  - **⚠ 2026-09-21 追加：这个前提现在**不成立**，阶段一必须先补"资产留存"（一条硬前置）**
    复核生命周期后确认：`NaniteRenderer::EnsureAssetUploaded` 里的
    `NanitePackedAsset asset;`（`NaniteRenderer.cpp:458`）是**局部变量**，只以 `const&` 传给
    `NaniteScene::UploadPackedAsset`（`NaniteScene.h:108`，该函数**不保留**它），且整条路径由
    `m_AssetUploaded` 门闩保证**只执行一次**（`NaniteRenderer.h:369` + `NaniteCull.h:322`）。
    ⇒ **上传之后 CPU 侧资产数据即被销毁**，`NaniteScene::AssetBuffers` 里只有 GPU 缓冲与
    `totalBytes/mismatchBytes/verified` 元数据（`NaniteScene.h:87-97`），**没有任何 CPU 字节镜像**。
    ⇒ 阶段一做不了"从已在内存的资产拷页"。
    **默认修法**：把资产**留存为成员**（`NanitePackedAsset` 或至少它需要的三段 + 页划分所需的
    `clusters`），由 `NaniteRenderer`（或 `NaniteScene`）持有；流式默认关时**不额外建 GPU 资源**，
    但这份 CPU 留存会常驻 —— 请在 §14.37 里如实标出它的内存代价（Sponza 实测
    `upload_bytes=13406096` ⇒ 约 13 MB），并说明它是否是"只开 enabled 也不变"这条不变式的例外。
    **若判定"常驻 13 MB 不可接受"**，替代做法是让 `NaniteScene` 在关闭流式时释放、开启时按需重建
    （但重建需要原始几何，而 `MeshBatcher` 只被 `const&` 用一次）—— **两条路都请显式裁决并写清后果**。
- **阶段 2（不在本任务）**：真正的磁盘流式（异步文件读、.nanite 分段 mmap/随机读、LOD 选择与预取）。
- 理由：阶段 1 就能把"页表/池/反馈/不变式"这套**最容易出错又最难测**的部分做完并用小页池
  （例如 8 页）造出大量页缺失，从而**在可控条件下**验证缺页行为；阶段 2 再换数据源即可。

**⑧ 开关与不变式**
- 新开关 `NaniteSettings::streaming`（**默认 false**，cfg 键 `nanite_streaming`），
  生效条件 `enabled && softRaster && streaming`。
- **关闭时必须与任务 23 的档位逐位相同**（冻结指纹 `1C15AB72…` / `750CC247…` 不变），
  且不新建任何 GPU 资源、不产生每帧新开销（§14.2 不变式 1）。
- 页池容量 0 或资产非单调 ⇒ 自动退化并**在读数里报告**（不静默）。

**⑨ 读数（供"无 pop 与缺页可观测"用）**
- `[Nanite] stream pages_total=P resident=R pool=K uploads_this_frame=U evicted=E page_misses=M
   pages_requested=Q stream=on/off reason=<...>`
- 其中 `page_misses` 必须是**真实 GPU 读回**（不是推测量），沿用既有"原子累加 + Map 读回"口径。

**⑩ 与其它任务的边界（避免重复劳动）**
- **任务 25（Material Bin）**：与本任务正交 —— 材质分组不改变页的划分，但页池的**槽位布局**要给
  任务 25 的材质数组留出扩位空间（本设计不预留，只记录该约束）。
- **任务 26（调试可视化）**：本任务只要求读数可读；**画面上的页驻留/缺页可视化属任务 26**。
- **LOD 选择策略**（选哪一级、误差阈值、切换时机）**不属于本任务** —— 任务 24 只解决"驻留"，
  这一点必须在评审时确认，否则范围会失控。

**⑪ 验收草案（待评审后定稿）**
- (a) **不变式**：`streaming=0` 时冻结指纹与任务 23 逐位相同；`=1` 时仅多出预期的页池资源，pass 集合不变。
- (b) **页表自洽**：`resident + 非驻留 == pages_total`；同一页不会被两个槽位同时标记驻留。
- (c) **缺页可观测**：把页池调到 8 页跑一帧，`page_misses > 0` 且**画面无未定义行为**（不崩、不越界、
  读数与 `visible` 的差额可核对）。
- (d) **驻留正确性（关键）**：页池足够大（≥ `pages_total`）时，`streaming=1` 的画面必须与
  `streaming=0` **在 4 张 GBuffer 上逐位相同** —— 这是"间接层没有算错偏移"的唯一硬证据。
  - **⚠ 2026-09-21 判据修正（本条按字面不可达，必须改口径）**：上式的"逐位相同"**做不到**，
    而且原因与本任务无关 —— §14.31 ⑩（原始证据见 `build/verify/t22_report.md:269-273`）已实测：
    **模块接管档两次「完全相同」的运行之间就不是逐位可复现的**，20 个转储里 7 个不同，
    且**恰好是 `albedo / gb_normal / gb_worldpos / gb_lightmapkey` 这 4 张 GBuffer**
    （+ `hdr` + `prov0_ao_{final,raw}` 抖动族）；像素级量级为 `lightmapkey.page` 平均差 18.8、
    worldpos 平均差 0.104、**87.6% 的像素胜出实例不同**（根因是软光栅深度键平局由 UAV 写序决定）。
    ⇒ 照字面执行只会得到两种坏结果：**误判自己的间接层错了**，或**悄悄放宽判据不说清**。
  - **改用三分口径（默认项，已在实现期以消息送达实现者）**：
    - **(d1) 数据类读数逐位相同**：`soft_clusters` / `hard_clusters` / `soft_pixels` / `hard_pixels` /
      `prims` / `size_dist` 五桶 / `page_misses` / `resident` / `pages_total` 在
      `streaming=1`（页池足够大）与 `streaming=0` 之间**完全一致**；其中
      **`page_misses == 0` 且"全部页驻留"是必要条件**。这类读数任务 23 已证明**逐位可复现**
      （8 个日志的 `size_dist` 取值唯一）⇒ 它是"页表/间接层没算错"的**最强证据**。
    - **⚠ 2026-09-21 (d1) 的非空洞化修正（关键，防"空洞通过"）**：仅写"`page_misses == 0`"这条
      判据**本身没有证明力**，因为读数缓冲是**按 C++ 容量分配并每帧清零**的 ——
      `NaniteRaster.cpp:834` 按 `sizeof(u32) * kNaniteSoftStatsCapacity` 分配
      （任务 24 定稿为 **22 个 u32**：槽 0..21，见 `NaniteTypes.h:2307`），
      并由 `m_SoftStatsZeroSrc`（创建于 `:837-849`）**每帧整段清零** ——
      清零函数是 `NaniteRaster::RecordSoftStatsClear`（`NaniteRaster.cpp:906-912`），
      它按 `kNaniteSoftStatsCapacity * sizeof(u32)`（任务 24 定稿 **22 个 u32 = 88 字节**）
      整段 `CopyBuffer` 覆盖 `m_SoftStats`，并紧接一条 Transfer→ComputeShader 屏障。
      ⇒ **清零范围覆盖槽位 20~21**，而写权限随后整段交给 compute（UAV）——
      没被 shader 写的槽位就**稳定读回 0**，不是"未初始化"而是"被明确清零"，
      所以这个空洞通过是**确定会发生的**，不是随机现象。
      ⇒ **若 shader 不写流式槽位 20/21，C++ 读回的就是 0**，
      "`page_misses == 0`"会在**流式功能完全没工作**时同样成立。
      【这条风险在实现期被实测确认为真实】起草时 `Nanite_SoftRasterCommon.slang` 的
      `kSoftStatCapacity` 仍是 `20u`（与当时的 C++ 不同步），且**全部 shader 零处**写入流式槽位；
      实现者随后补齐为 `22u` + 槽 20/21 的原子写入（`Nanite_SoftRaster.comp.slang` 写 `page_requests`、
      `Nanite_SoftRasterDepth.comp.slang` 写 `page_misses`，详见 §14.37）。
      **本条修正不因"已补齐"而失效** —— 它防范的是后续改动把它退回"不写"的回归，
      而 (c) 就是那条必须真跑一次的非空洞守卫。
      **因此 (d1) 必须加强为**：流式开启且页池充足时**同时**满足
      **`page_requests > 0` 且 `page_misses == 0`** ——
      `page_requests > 0` 证明"光栅→反馈回读"链路真的在跑，(d1) 才具备证明力。
      配套两条硬要求：
      1. Slang `kSoftStatCapacity` 必须与 C++ 的 `kNaniteSoftStatsCapacity` **相等**
         （当前两侧都是 **22u**，槽 0..21 吃满；且**必须只有一处定义** ——
         实现期曾出现 `Nanite_SoftRasterCommon.slang` 同作用域重复定义 `20u`/`22u` 的缺陷，
         已修），且流式槽位的累加写入必须发生在
         本帧清零**之后**（即本帧光栅之内），否则读数会被清零或丢失。
      2. **(c) 是 (d1) 的非空洞守卫**：必须在"页池 8 页"下**真拿到一次 `page_misses > 0`**，并把读数原文
         写进记录。若 (c) 拿不到 `page_misses > 0`，说明写入链路本身有问题，
         **(d1) 的通过无效，不得据此验收**。
    - **(d2) 像素类差异不得超过"噪声底"**：用**同配置跑两次**（`streaming=0` 两次）作噪声底，
      与 `streaming=1` vs `streaming=0` 的逐文件 `diff_px` / `maxULP` **并列比较**
      （工具即仓库既有的 `build/verify/cmp_dumps.py <A> <B>`，验收判据 ② 正是这么用的）。
      **某一项明显超出噪声底才是间接层真错了。**
    - **(d3) 结构性证据**：`resident == pages_total`、`uploads_this_frame ≤ 每帧上限`、
      请求到驻留延迟与 `kNaniteFeedbackLatency` 一致（都在读数里）。
  - **要求**：实现者必须在 §14.37 里**写出"判据 (d) 原始形式不可达"及实际采用的口径与实测数值**，
    不允许悄悄放宽。
- (e) **反馈时延**：`uploads_this_frame ≤ kNanitePageUploadsPerFrame`；请求到驻留的延迟可读且与
  `kNaniteFeedbackLatency` 一致。
- (f) 判据 ⑦/⑧ 保持 PASS（跑 ≥2 次，注意既有抖动史）。

**⑫ 明确不做（防止范围蔓延）**
- 磁盘 I/O 与分段读取（阶段 2）；LOD 选择与预取；页压缩/去重；多资产共享页池的装箱优化；
  画面上的页可视化（任务 26）。

### 14.33 任务 25 前置核查：Material Bin 的现状事实（2026-09-21 起草）

> 按 §14.1「先测量再改」的惯例，任务 25（Material Bin，§5.4）动手前先把"现在到底是什么样"钉死。
> 下面的每一条都是可复核的代码事实，不是设计意图的复述。

**① 现状：延迟路径**已经**没有逐材质描述符切换**
- 光栅端读材质的方式是**索引进一个 SSBO**：`softRasterEvaluateMaterial(cluster.materialID, uv, …)`
  → `u_Materials[materialIndex]`（`Nanite_SoftRasterCommon.slang`），纹理经 **bindless 数组**
  `u_MaterialTextures[]/u_MaterialSamplers[]` 取（同一个堆）。
- 硬光栅同源：`Nanite_HardRaster.mesh.slang:198` 把 `cluster.materialID` 透传给片元，
  片元调**同一个** `softRasterEvaluateMaterial`。
- ⇒ **一次 dispatch / 一次 draw 就能处理任意多材质的簇**，"draw 爆炸"与"描述符切换"在当前结构下
  **不可能发生**（不是"优化掉了"，而是结构上不存在这个变量）。§5.4 想减少的那种切换，
  在本仓库的延迟路径里**从未出现**；它对应的是"每个材质一个描述符集"的旧式实现。

**② 那么 §5.4「按材质分组 cluster」还剩什么价值**
- 剩下的价值只有**访存局部性**：同材质的簇相邻处理 ⇒ bindless 纹理采样更可能命中同页/同 cache。
- 而"一次 Draw/Dispatch 处理同一材质的多个 cluster"这条**已经成立**（①），不需要 bin 来实现。

**③ 由此推出的任务 25 范围建议（默认项）**
1. **读数**（验收明文要求"描述符切换次数可读"）：新增一行报告
   `descriptor_switches`（预期恒为 0～1）、`material_switches`（相邻处理的簇换材质的次数，
   作为局部性代理）、`clusters_per_material` 的分布摘要。
2. **只读的材质 bin**：上传期用 `JobSystem` 生成一份 `u32[clusterCount]` 的"按材质排序的簇下标"
   辅助数组（不动资产本体、不动 BVH/DAG 引用，故不可能破坏既有路径），并给出"按 bin 顺序遍历时
   `material_switches` 降到多少"的对照数字。
3. **开关** `NaniteSettings::materialBin`（默认 **false**），关闭时逐位不变。
4. **明确不做**：把光栅的遍历顺序真的改成 bin 顺序。理由是一条硬约束 —— 软光栅的最终像素在
   **深度键平局**时由 UAV 写入顺序决定（§14.31 ⑩ 已实测：模块接管档两次相同运行并非逐位可复现，
   `lightmapkey.page` 平均差 18.8）。**在平局确定性修好之前改遍历顺序会改变画面**，
   属"先修根因再谈优化"。这一点必须在任务 25 的报告里写清，不能含糊。

**④ 起草时已取到的实测数（供任务 25 的对照基线）**
- 资产：`clusters=8287 vertices=542190 materials=103 lod_levels=6`（Sponza）。
- 簇的材质映射：`cluster_material_id=[min=0 max=101 distinct=90] multi_mesh=97 unmapped=4103`。

**⑤ ⚠ 起草时发现的一处异常：`unmapped=4103`（占 8287 个簇的 49.5%）**
- `NaniteUpload.h:427` 对这个计数器的定义是「一个三角形都落不进任何源网格区间的簇数
  （**防御：正常必须 0**）」；而实测是 **4103**。按代码自己的口径，这是一个**缺陷候选**，不是正常值。

**⑥ 根因（2026-09-21 代码走查得出，比初稿的"两种可能"更具体）**
> 初稿只列了"区间表未排序/有空洞"与"期望值写错"两种可能。走查后可以**排除"未排序/有空洞"**，
> 并定位到真正的原因 —— **投票用错了索引空间**：
1. `meshes[]` 描述的是**原始合并**三角形空间：`NaniteRenderer.cpp:452` 用
   `range.firstTriangle = commands[i].firstIndex / 3`，而 `MeshBatcher.cpp:99-100` 让 `baseIndex += idxCount`
   逐个推进 ⇒ 该数组**天然按 `firstTriangle` 升序且区间连续无空洞**。所以
   `FindSourceMeshForTriangle`（`NaniteUpload.cpp:1165`，二分查找）的**排序前提是满足的**。
2. 但投票传进去的 `tri`（`NaniteUpload.cpp:1206`：`cluster.triangleOffset + k`）落在**去重后的**三角形空间：
   去重路径下 `record.triangleOffset = result.uniqueTriangleOffset[uniqueIndex]`（`:589`），
   而 `uniqueTriangles` 是按**首次出现顺序**追加唯一内容的（`:576-579`）⇒ 一旦去重真的合并了内容，
   簇的 `triangleOffset` 就**不再等于**该三角形在原始合并几何里的下标。
3. ⇒ **用"去重空间的下标"去查"原始空间的区间表"**，命中与否取决于去重是否改变了顺序。
   非去重路径（`:385` 顺序追加）两个空间一致 ⇒ 投票正常；**去重路径 ⇒ 大量簇的三角形
   全部落在空档里 ⇒ `mappedTriangles == 0` ⇒ 计入 `unmapped` 并回落成 0 号网格材质。**
4. 这同时解释了为什么不是"全错"：`multi_mesh=97`、`distinct=90` 说明**部分**簇（其内容恰好未被打乱）
   仍能正确映射 —— 与"取决于去重是否改变顺序"的机制一致。
- **证据强度（如实标注）**：本条的推导链每一环都有 `文件:行` 依据，但**尚未用插桩实测确认**
  （例如在构建器里同时打印"原始下标"与"去重下标"看它们从第几个簇开始分叉）。
  ⇒ 任务 25 的**第一步应当是这条插桩确认**，然后再谈修法。
- **旁证（2026-09-21 追加，强化了缺陷定性）**：`NaniteUpload.h` 的映射规则注释自己写明了本设计**假设**
  `triangleOffset` 就是合并空间的三角形下标 —— 原文「簇的三角形区间 =
  `NaniteClusterRecord::triangleOffset/triangleCount`（单位是三角形）」以及
  「一个三角形都落不进任何区间（越界/空洞）⇒ 归属 0 号网格并计入 `unmappedClusters`
  （**正常必须 0：合并几何是连续拼接的，不存在空洞**）」。
  ⇒ 去重路径让 `triangleOffset` 落在**另一个空间**，等于**违反了文档写明的契约**，
  因此这不是"注释里的期望值写错"，而是**实现与契约不一致** ⇒ 定性为**缺陷**，修法应让实现回到契约。

**⑦ 修法方向（供任务 25 裁决，默认项）**
- **⚠ 2026-09-21 追加：首选修法在当前调用点上"做不到"，必须先补管道。** 复核调用点
  （`NaniteUpload.cpp:1237-1260`）后确认：
  - 该重载先调内层构建器产出**完整资产**（`:1245-1248`），再调
    `NaniteAssignClusterMaterials(asset.clusters, meshes, clusterMaterial)`（`:1256-1257`）；
  - ⇒ 材质映射函数**只拿到"已打包的簇数组 + 原始空间的网格区间表"**，
    **既没有原始 `indices`，也没有"每簇三角形 → 原始下标"的映射**。
  - 因此"让投票改用原始空间的三角形下标"**不是换一个变量就能做的事**，它要求上游**新增一份
    per-簇（或 per-出现）的原始三角形区间**。初稿把这一步写成"首选、成本低"是**低估**了，特此更正。
  - **候选做法（按成本排序，供裁决）**：
    1. **在内层构建器里、去重之前就完成材质投票**（那时簇的三角形区间还在原始空间）——
       改动集中在构建器内部，**不改文件格式、不加运行期数据**，但要把 `meshes` 传进内层、
       或把"逐簇材质"作为构建产物之一返回；
    2. 内层构建器**额外产出**一份 `clusterOriginalTriangleOffset[出现数]`（构建期临时数组，
       不落盘、不上传）供投票使用；
    3. 在 `NaniteClusterRecord` 的**空闲字段**里记原始区间（**会改格式**，最不推荐；需先确认是否真有
       足够且语义干净的空位，且要与 `static_assert` 和 `ValidateNaniteFile` 同步）。
  - **首选做法的默认项 = 1**（构建器内部、去重前投票）：它同时满足"回到文档契约"与"不动格式"。
- **备选**：改为按**顶点位置**归属源网格（几何判据，不依赖索引空间），但代价是引入空间判定与
  边界情形（跨网格簇的定义），**不建议**。
- **次选（若判定不值得修）**：保留回落行为但**把它变成可见的问题**：`unmapped > 0` 时打印
  **告警级**日志（而非仅 info 读数），并把 `NaniteUpload.h:427` 的注释从「正常必须 0」
  改成「在去重路径上当前可能非 0，原因见 §14.33 ⑥」。
- **在裁决之前，任何 Material Bin 的收益数字都不可信**（近半数簇的材质归属是错的）。
- 这条也解释了为什么任务 19 的验收（材质字段与既有 GBuffer 路径逐项可比）能通过：
  它比的是**材质公式**，不涉及"每个簇的材质归属是否正确"。

> 处置建议（默认项）：把本项列为**任务 25 的前置缺陷**，在其报告里给出裁决与修法；
> 若判定为"期望值写错"（即该资产合法地存在未覆盖三角形），则只更正 `NaniteUpload.h` 的注释与
> 读数语义，并把「正常必须 0」改成「在覆盖完整的资产上必须 0」，同时给出该资产的覆盖统计。

**⑨ 实测确认（2026-09-21；真实资产离线插桩）**

> 第 ⑥ 条的推导链此前只做到"代码走查 + 文件行依据"（证据强度见 ⑦）。现已用**真实 Sponza 资产离线跑一遍**
> `BuildNaniteClusterDAG` + `PackNaniteClusters` + `NaniteAssignClusterMaterials`，把各索引空间**逐簇对照** ⇒ **根因确认**，
> 并且发现原判断**低估了缺陷范围**（见 ⑩）。

可复现的实测数据（插桩载体为 `Tests/TestNaniteMaterialMapRepro.cpp`；确认后已按"把插桩转正"的要求
变成常驻回归测试 `Tests/TestNaniteMaterialMap.cpp`，临时文件已删除）：

| 量 | 实测值 |
|---|---|
| 原始合并空间 | `triangles=262267`、`meshes=103`；区间表 `contiguous=true covered=262267` ⇒ **第 ⑥ 条排除"未排序/有空洞"是对的** |
| 去重空间 | `clusters=8287 uniqueContents=8202 dedupRate=0.010257 uniqueTriangles=524657` |
| **决定性一项** | **`triangleOffset` 越出原始空间 `[0,262267)` 的簇 = 4103**，与线上 `unmapped=4103` **精确相等** |
| 机理①（决定 4103 这个数） | 去重后的三角形段**按 LOD 级序**追加，装着全部 6 级的唯一内容，共 524657 条 ≈ 原始 262267 的 **2 倍** ⇒ **级数 ≥1** 的簇偏移越出区间表覆盖范围，必然全部落空 |
| 机理②（影响 LOD0） | **与去重无关**（去重率仅 1.03%，LOD0 有 4015/4099 份不同内容，远不足以解释）：是 `meshopt_buildMeshlets` 按**簇内局部性重排**三角形 ⇒ LOD0 **全部 4099 个**簇的偏移都与原始下标不符（**首个分叉簇下标 = 0**） |
| 可归属性 | **三空间都未命中的簇 = 0** ⇒ 用对索引空间后**没有不可归属的簇**（不是资产损坏、也不是覆盖缺口） |
| 影响面 | **材质归属会变的簇 = 8086 / 8287（97.57%）** |

**⑩ 原判断的一处低估（必须更正）**

那 4103 个"未映射"的簇**恰好全是非 LOD0**。LOD0 上另有 **3957 / 4099（96.5%）的簇被静默映射到了错误的网格** ——
它们的 `triangleOffset` 虽落在原始区间内，但**簇内三角形已被按局部性重排**（机理②，**不是去重**——
去重率仅 1.03%）。LOD0 是近距离可见的那一级
⇒ 这不只是"计数缺陷"，而是**可见的正确性缺陷**。
⇒ 本节上方"近半数簇的材质归属是错的"应更正为「**绝大多数（97.57%）簇的材质归属是错的；其中 LOD0 的 96.5% 是"选错网格"而非"未映射"**」。

**⑪ 修法裁决的更新**

第 ⑧ 段（候选做法）把"按**顶点归属**"列为**不建议**的备选，理由是"会引入空间判定与边界情形"。
实测显示该顾虑**在 Sponza 上不成立**：`顶点归属空间 unmapped=0`、`顶点归属选错网格=0`、
`全簇顶点同属一个源网格的簇=5996`、`有顶点查不到归属的簇=0`，样本簇 `混合三角形数=0 无归属顶点数=0`，
且与"原始三角形真值"**逐簇一致**（样本簇 #4183：真值网格 68 / 顶点归属 68 / 旧值 0）。
⇒ 顶点归属不再是"退而求其次"，而是**已被实测验证与真值一致**的做法。
首选仍可取 ⑧ 的"构建器内部、去重前投票"（成本更低、不引入空间判定）；两者都满足"回到契约"，
裁决以**改动面**与**能否逐簇对齐真值**两条并列为准。

**⑫ 修复记录（2026-09-21 落地）**

采用 ⑪ 中被实测验证的**顶点归属**口径，新增 `NaniteAssignClusterMaterialsByVertexOwner`：
1. 由「原始合并索引 + `meshes[]` 区间」反推「顶点 → 源网格」归属表 —— 区间首尾相接 ⇒
   每个原始三角形的 3 个顶点必落在其所属网格那一段内，**零新增输入**；
2. 逐簇用 DAG 的 `clusterVertexIndexOffset/clusterVertexIndices` 把三角形还原成**原始合并顶点下标**，
   每个三角形投给"3 个顶点里占多数的源网格"（平票取小下标），簇再取多数票。

**没有改动的东西**：`triangleOffset` 的语义、页划分、DAG 引用、`.nanite` 格式**一个字节未动**；
归属表是**构建期临时数据**（顶点数 × 4B，Sponza 770 KB，用完即弃，不进段表）；
改完簇段后仍重建字节镜像并重跑 `ValidateNaniteFile`。全量 8287 个簇都会写 `materialID`。

**被否决的方案**：① 改 `triangleOffset` 语义 ⇒ 波及页划分（`triangleOffset×3`）与格式；
② 每簇落一份「去重 → 原始」三角形映射 ⇒ LOD1+ 的三角形是简化**新生**的、没有对应的原始三角形，到不了 `unmapped == 0`；
③ 按顶点**位置**做几何判定 ⇒ 更重且边界情形多，被精确的归属表取代。

**独立正确性证据（不只"计数归零"）**：

| 证据 | 结果 |
|---|---|
| 未映射簇数 | **4103 → 0** |
| **LOD0 精确真值**（按排序顶点三元组反查原始三角形 ⇒ 逐簇真值） | **4099/4099 全查、错 0 个**（同一批旧口径错 **3957** 个） |
| **与算法无关的几何核对**（所选网格 AABB 与簇包围球距离 ≤ 半径） | 全部 **8287 个簇越界 0 个** |
| 无归属顶点的簇 | **0** |
| 合成最小复现（3 份间距 100 的平移副本、去重率 13%） | 旧口径 8 个未映射 / 14 个选错 ⇒ 新口径 **0 / 0** |

**回归守卫**：常驻测试 `Tests/TestNaniteMaterialMap.cpp` 在真实资产上断言 `unmapped == 0`、
LOD0 真值逐簇一致、无归属顶点簇 0、几何核对越界 0，并断言材质归属**确实发生了变化**
（⇒ 把口径改回去会立刻失败）；它与 `NaniteRenderer` 里 `unmapped > 0` 的告警（任务 26 那一项）形成闭环。

**影响面（画面会变，属修复而非回归）**：`unmapped` 4103 → 0；`multi_mesh` 97 → 2291；
`cluster_material_id` 的 `distinct` 90 → 87；**8086/8287（97.6%）的簇换了材质** ⇒
软光栅写出的 albedo/metallic/roughness 像素值改变。`material_pixels` 数量不变、`fallback_pixels` 仍为 0
（`materialID` 始终在合法范围内，兜底是合法的 0 号记录）。任务 19 的验收当初比的是材质**公式**、
不涉及"逐簇归属是否正确"，所以没能发现这一层。

> ⚠ `multi_mesh` 由 97 涨到 2291 **不是新缺陷**：实测 `meshopt_buildMeshlets` 在簇内没有未发射的**连通**邻居时，
> 会退化成"按空间最近挑三角形、**不管连通性**"（`meshoptimizer/src/clusterizer.cpp` 原注释
> "we currently just pick the closest triangle irrespective of connectivity"）⇒ 一个簇**合法地**可横跨多个源网格。
> 旧的 97 是"用错索引空间下偶然落在同一网格内"的产物，**2291 才是真实值**。

> 保留说明：三角形空间的低层函数 `NaniteAssignClusterMaterials` **保留未删**（真实路径已不再调用它），
> 其头文件已写明"使用前提：资产满足三角形偏移与源网格区间同空间"，以免后来者误用。

**⑬ 任务 25 剩余部分的实施记录（2026-09-21 落地）**

按 ③ 的四条范围实现，**没有扩面**（特别是没有真的改光栅遍历顺序）：

1. **三条读数**（新增**一行** `[Nanite] material_bin …`，字段名与既有的
   `materials_sample` / `soft_raster` / `size_dist` 三行刻意不重名）：

| 字段 | 口径 | Sponza 实测（`soft_max_triangles=16` 档，dump 帧） |
|---|---|---|
| `descriptor_switches` | 本帧**材质切换导致的描述符集切换次数** | **1** |
| `material_switches` | **相邻处理的簇换材质的次数**；顺序 = **GPU 可见簇列表缓冲的槽位顺序** | **361** |
| `material_switches_bin` | **若按 bin 顺序遍历**（同一批可见簇按材质分组重排）的同一个数 | **41** |
| `material_switches_asset_order` | 资产**自然顺序**（簇下标 `0..N-1`）下的同一个数（与单测同口径的参考） | **1608** |
| `clusters_per_material` | 资产侧分布摘要 `[distinct max min mean]` | `[87 1083 2 95.25]` |
| `order_src` + `visible_refs` + `bin_clusters` | 顺序来源与其分母（可核对） | `gpu_visible_cluster_buffer` + `31648` + `31648` |

2. **`descriptor_switches` 为什么恒为 0～1（如实说明，不是"没测到"）**
   - 材质是按**索引进一个 SSBO** 取（`u_Materials[cluster.materialID]`），纹理经 **bindless 数组**
     取；硬光栅同源（把 `materialID` 透传给片元、调同一个求值函数）。
   - 软/硬光栅各自只建**一对**描述符集，帧内**不重绑** ⇒ 真实切换只有"进入软光栅趟（1 次）"与
     "进入硬光栅趟（0/1 次）"，**上界 1**。§5.4 想减少的"每材质一个描述符集"那种切换在本仓库的
     延迟路径里**从未存在**（① 已论证）。
   - 因此这里报**结构上界**，而**不伪造**一个永远读不到的"切换计数器"。

3. **只读的材质 bin**：`NaniteMaterialBin`（`u32[clusterCount]` 的"按材质非降序的簇下标"排列，
   即 §5.4 的"按材质分组"的数据形态）。生成时机 = **资产上传时一次**（与 `m_AssetUploaded` 门闩
   同处的 `NaniteScene::StoreAssetCPUCopy`），数据源 = **已被留存的 CPU 资产**（不另持第二份）。
   **不动的东西**：资产本体（`clusters/vertices/triangles/materials/lodOffsets/bytes`）、
   BVH/DAG 引用、页池/页表、`.nanite` 格式**一个字节都不动** —— 它是**新增**的派生数组，
   没有任何回写；单测用"生成前后簇段逐字节相同"把这条落成断言。
   **默认档零新增资源**：`materialBin=false` 时既不分配那个数组、也不打印任何字符。

4. **明确不做（硬约束）**：**没有**把光栅的遍历顺序改成 bin 顺序。理由是软光栅在**深度键平局**时
   像素由 **UAV 写入顺序**决定（§14.31 ⑩ 实测：模块接管档两次相同运行并非逐位可复现，
   `lightmapkey.page` 平均差 18.8）⇒ **在平局确定性修好之前改遍历顺序会改变画面**，属"先修根因
   再谈优化"。bin 因此是**只读的收益证据**，不是一条已生效的路径。

5. **关闭档零影响（实测）**：同一命令带 `nanite_material_bin=0` 与改动前的基线日志
   （`build/verify/nan_t26_selfcheck2.log`）逐行比较：**总行数完全相同（6391 = 6391）**，
   唯一的差异是配置回显行**末尾多了一个新键** `nanite_material_bin=0`；pass 列表指纹
   (`passlist_sha`) 与 `vuid_lines` 逐字相同。`soft_raster` / `size_dist` 两行的每个字段**逐字未变**
   （新读数**没有**写进软光栅读数缓冲，因此不会触发 `SelfCheckSoftStats` 的"恒真读数"误报）。

6. **回归测试**：`Tests/TestNaniteMaterialBin.cpp`（4 个用例）钉住"排列 + 按材质非降序 + 同材质稳定"
   三条不变量、越界 `materialID` 不 clamp、空输入与确定性、以及真实 Sponza 上
   `material_switches` 由 **1608 → 86**（= 出现材质数 − 1 的**理论下界**）。

### 14.34 任务 26 前置核查：已知故障模式清单与可观测性矩阵（2026-09-21 起草）

> 任务 26 的验收原文是「簇/BVH/LOD/软硬光栅占比/可见簇数可视化；**每个已知故障模式都能被至少一个
> 工具观察到**（沿用 Lumen 的 40 号任务口径）」。⇒ 动手前必须先有"已知故障模式"的**清单**，
> 否则"每个"无从核对。本节把计划文档 §14.13–§14.33 里**实际记录过**的故障逐个列出，
> 并标出今天是否已可观测、缺口在哪。

| # | 已知故障模式（出处） | 今天的可观测手段 | 缺口 |
|---|---|---|---|
| 1 | **深度解析不写深度**（§14.30 ②③：`depthTest=false` 丢 `SV_Depth`；`GetDimensions` 一维/二维混用） | `soft_raster` 的 `depth_written`（已改成真实原子计数）+ `cull3` 的 `hiz_half` / `occl_mip` | 无（已可观测；**但转储里没有深度目标**，只能间接看） |
| 2 | **恒真读数掩盖缺陷**（§14.30 4a：`depth_written` 曾硬编码 1） | **新增读数自检**（`size_dist` 行追加 `stat_ok` / `const_suspect`，可疑时打告警级日志）：① 三条**无需新增输入**的已知关系式判定"读数是否真来自本帧 push constant"——`diag_screenw × diag_screenh == depth_key_pixels`、`diag_maxtri == 上次送下去的 maxTriangles`、场景非空时 `diag_extent_milli > 0`；② 逐槽记录"是否曾经变化过"，检出**非 0 且从未变过**的槽 | **已闭合**（任务 26 本轮）。已验证**非空转**：把其中一条关系式故意反置 ⇒ `stat_ok=0` 且告警按预期打出实测值，还原后复测 `stat_ok=1`、无告警。**如实标注一处局限**：② 在"本次读回里一个槽都没变"（相机固定、整轮只转储一帧）时**无法判定**，此时不报警（宁可漏报、不误报） |
| 3 | **Hi-Z UV y 镜像**（§14.28：`mip0/1/4/7` 全 0；且 `useTwoPhase` 恒 false ⇒ `HiZ_Build` 从未执行） | 判据 ⑦ 的 `hiz1` 档（`occl_mip`、`occl_uv`、`hiz_flip` 开关对照） | **pass 是否真的执行过**没有通用观测（本例靠"关掉就该变"的对照才发现） |
| 4 | **DAG 内容哈希顺序无关 + `meshopt` 原地重排簇内顶点**（§14.20⑥、§14.22①） | `Tests/TestNaniteTypes.cpp` 的属性保真度用例 | 运行时无读数（只在离线/单测可见） |
| 5 | **共享内容属性错配**（§14.27 的 P0、任务 18） | 离线检查工具按 `objectIndex` 分区分类解码页号（`Tools/gi/lightmap_key_check.py`） | 无 |
| 6 | **深度与排序契约两处 P0**（§14.30 ①：`depthTest`/`depthLoadOp`） | 判据 ⑧e 的 pass 列表与冻结指纹；校验层 VUID 行 | 无 |
| 7 | **软光栅深度键平局 ⇒ 由 UAV 写序决定像素**（§14.31 ⑩，实测接管档两次运行 7/20 转储不同） | 判据 ⑧b 的差异范围；**间接**：`pixels_written` 与 `depth_written` 的差额就是"同一像素被多个三角形通过等值复检"的次数（§14.30 已把它写成可核对不变式 `depth_written ≤ pixels_written`） | **无专门读数**把平局次数直接报出来（只有差额可反推）⇒ 任务 26 可补一条显式平局计数 |
| 8 | **材质映射 `unmapped=4103`**（§14.33 ⑤） | `materials_sample` 读数行 + **`unmapped > 0` 的告警级日志** | **已闭合**：任务 25 修掉根因（§14.33 ⑫，实测 4103 → 0），任务 26 把它升级为告警级日志，并由常驻测试 `Tests/TestNaniteMaterialMap.cpp` 钉住（`unmapped == 0` + LOD0 真值逐簇一致）⇒ 一旦回归，告警会跳出来、单测会变红 |
| 9 | **RHI 阶段掩码互换**（§14.31 ⑦①）：错值 ⇒ PSO 创建失败 + `EXCEPTION_ACCESS_VIOLATION` | 校验层 + 崩溃日志（`07_Nanite_crash.log`） | 无（崩溃本身可观测） |
| 10 | **`SetPushConstants` 缺 mesh 阶段**（§14.31 ⑦②） | 只有"着色器读到的值与期望不符"这种症状 | **无工具**：缺"push constant 实际收到什么"的回读（软光栅有 `diag_*` 回读，硬光栅没有） |
| 11 | **资产 `vertexOffset` 非单调**（§14.32 ②） | **任务 24 的页划分本身就是按"非单调"设计的** —— 簇段取"收集序"而不是区间，正是为此；并且单测在**真实资产**上把它钉成可执行事实（`前 4 个 vertexOffset=[0 45 0 90 …] 单调=0`，由 `Tests/TestNaniteStream.cpp` 断言），页划分在其上照常成立 | **已可观测**（**本轮改正**：原标注"待任务 24"，而任务 24 已落地）。**如实标注**：它是"已被正确处理的事实"、不是需要告警的异常，因此**没有**专门的运行时告警读数 |
| 12 | **页缺失 / 页池满**（§14.32 ⑨） | **任务 24 的 `stream` 读数行**：`pages_total / resident / nonresident / pool / uploads_this_frame / evicted / page_misses / pages_requested / overflow_total / reason`；`stream_setup` 行给出页划分与池足迹；池 8 槽档实测真跑到 `page_misses=122`、`evicted=58`，且读数与 `visible` 精确对账（`soft + skipped_big + page_misses == visible`） | **已可观测**（**本轮改正**：原标注"待任务 24"，而任务 24 已落地并经两轮 `-OnlyNanite` 验收） |
| 13 | **`objectIndex` 分区越界**（§14.15：今天不存在越界路径，风险在离线工具） | 离线工具 + `static_assert` 分区断言 | 无 |
| 14 | **覆盖率语义差**：软光栅不做近/远平面裁剪、硬件按规范裁掉（§14.31 ③④，实测 3496 px 100% 在远平面外） | A/B 覆盖像素差 | **无工具**把"缺口落在哪"分类（需按深度分桶） |
| 15 | **硬光栅过绘 / mesh 线程利用率**（§14.31 ⑨，实测 336.4× / 50.0%） | **任务 23 已落地**：`size_dist` 五桶分布 + `perf` 行（`soft_clusters / hard_clusters / soft_pixels / hard_pixels / nanite_pass_ms / frame_ms`），并给出**同覆盖**对照（soft64 对 hard16：GPU **21.24×**、墙钟 **6.38×**） | **已可观测**（**本轮改正**：原标注"任务 23 落地后复核"，复核已完成，证据见 §14.36） |
| 16 | **关闭档泄漏 / pass 顺序漂移**（判据 ⑥、8e） | `acceptance_sweep` 判据 ⑥（含负向验证）+ 8e 冻结指纹 | 无 |
| 17 | **验收脚本自身的抖动 / 脚本自身的脆弱**（§14.11：判据 ⑦/⑧ 在未改动代码上各失败过一次） | 只在人肉对比历史日志时可见 | **仍未闭合 —— 如实标注**：没有"这次 FAIL 是不是抖动"的自动判定。**本轮实测到四个具体脆弱点**（都值得后续修）：① **验收脚本必须从仓库根目录运行** —— 样例用相对路径 `build/verify/` 写转储（`Samples/07.Nanite/07.Nanite.cpp` 的 `const String dir = "build/verify/"`），从 `build\verify` 下运行会把转储写进 `build\verify\build\verify\`，脚本**不报错**、只静默给出 `dumps compared=0` ⇒ 判据 ⑧b/⑧c 假 FAIL（本人据此误报过一次"材质修复回归"）；② 判据 ⑦ 的判定式 `extra_gpu == 0 且 mismatch == sum(occl_mip)` **只有 `sum > 0` 这一条空转守卫**，对量级不敏感（`occluded=4` 与 `occluded=56346` 同样 PASS）⇒ 建议加量级下限或与记录的期望值比对；③ **整套验收脚手架未纳入版本控制** —— `build/verify/` 被 `.gitignore` 忽略（`Build/`），全仓库**没有任何被跟踪的验收脚本副本**，且 7 个脚本（`acceptance_sweep.ps1`、`cmp_dumps.py`、`nanite_smoke.ps1`、`nanite_takeover_cmp.ps1`、`nanite_cull_diff.ps1`、`final_acceptance.ps1`、`final_acceptance2.py`）含硬编码绝对路径 `D:\Source…` ⇒ **判据无法从克隆重现**（期望值即冻结指纹已进本文档，但产生与核对它们的**机制**没有）⇒ 建议把脚手架移进受跟踪位置（如 `Tools/nanite_acceptance/`）并把路径改为由仓库根推导；④ **样例自动退出后常残留进程** —— `07.Nanite.exe` 在 dump 后自动退出时仍可能驻留，占住 exe 导致后续链接 `LNK1104`、或与下一档争用窗口/GPU ⇒ 构建或跑档前先 `Get-Process 07.Nanite \| Stop-Process -Force`；⑤ **判据 ④ 只扫 `diff_px` 行，对 `MISSING`/`SIZE` 视而不见** ⇒ 若某档转储根本没写出来（例如上一条的相对路径问题），`cmp_dumps.py` 会打出 `MISSING on B` 而判据 ④ 仍报 "抖动族之外 0 项差异 = PASS" —— **与 ⑧b/⑧c 同属"缺转储即空洞通过"**，建议把 `MISSING`/`SIZE` 也计入失败；⑥ **两套 smoke 脚本不可混用** —— `acceptance_sweep.ps1:42` 的判据 ①–④ 走 `lumen_smoke.ps1`（默认预设、Lumen 打开），而 Nanite 各判据走 `nanite_smoke.ps1`（以 `Content/Config/07_Nanite.cfg` 为基座、Lumen 关闭）；手工复现某一档时用错脚本会得到**另一个预设**（实测 `lumen_passes=0`、8 个 lumen/prov6 转储缺失、`gb_worldpos` 平均差 159）⇒ 巨量假差异（本人先后据此误报过两次：一次"⑧c 回归"、一次"判据 ④ FAIL"） |

**由本表推出的任务 26 最小范围（默认项）**
1. **屏幕可视化**（验收明文点名的四项）：可见簇数 / 软硬光栅占比 / LOD 层级 / BVH 深度。
   落点建议沿用本模块既有做法：模块自建一张小目标（与任务 6 的 1×1 目标同款、**不改 GBuffer**），
   用 `nanite_debug_view` 档位切换；**默认关**时一个 pass 都不注册。
2. **补上表中 3 个"今天完全没有观测手段"的缺口**（优先顺序按本表编号）：
   - #7 平局计数（软光栅等值复检命中次数 > 1 的像素数）；
   - #10 硬光栅的 push constant / 读数回读（对齐软光栅既有的 `diag_*` 做法）；
   - #2 读数常量性检查（一条"该读数在本帧是否为常量/恒真"的自检）。
3. **#8 从"打印了"升级为"可见的问题"**：`unmapped > 0` 时打印**告警级**日志（而非仅 info 读数）。
4. 明确不做：#11/#12（待任务 24）、#15（任务 23）、#14 的深度分桶（若时间不允许，则如实列为未做）。

> **本节的作用**：任务 26 的报告必须**逐行回应本表**（每行给"已可观测 / 本轮补了 / 明确不做"），
> 这样"每个已知故障模式都能被至少一个工具观察到"才是一句可核对的话，而不是自我评价。

### 14.35 任务 27 前置核查：收口清单（2026-09-21 起草）

> 任务 27 的验收原文：「`HugEngineTests` 全绿；默认预设抖动族之外 0 项差异；开关不变式（任务 2）常跑」。
> 三条都已有现成载体，本节只把"跑什么、期望什么、哪里有既有豁免"钉死，避免收口时把既有噪声当成回归。

**① 单测全绿**
- 命令：`cmake --build build --config Release --target HugEngineTests` 然后 `build\bin\Release\HugEngineTests.exe`，
  期望 `Status: SUCCESS!`。
- 起草时盘点（2026-09-21）：**42 个测试文件 / 310 个用例 / 2857 处断言**。
  其中 Nanite 两块：`TestNaniteTypes.cpp`（44 用例 / 807 断言）、`TestNaniteBuilder.cpp`（29 用例 / 594 断言）。
  **注意**：任务 23 正在给 `TestNaniteTypes.cpp` 追加用例（起草时 +64 行），所以收口时应以**当时的实测数字**为准，
  不要照抄本节。
- 任务 27 还要求**为任务 23–26 新增的机制补测**（否则"收口"只是跑一遍旧的）：至少应有
  `size_dist` 桶边界与"五桶之和 == 可见簇数"、材质 bin 的排序正确性、以及任务 24 的页表自洽
  （若任务 24 已落地）。**若某项没做，必须在报告里列为未覆盖，不要含糊。**

**② 默认预设抖动族之外 0 项差异**
- 载体：`acceptance_sweep.ps1` 的**判据 ④**（默认预设 `aq_def` vs 基线 `s37fin2`）。
- **既有豁免（不是 Nanite 引起，也不要试图修）**：§14.11 的「判据 ④ 的一处既有漂移裁决」——
  `lumen_irradiance` 与下游 `prov6_*` 共 5 项给了**硬上界容差**（`maxULP ≤ 8` 且 `meanAbs ≤ 1e-6`），
  其余转储仍要求**逐位一致**。⇒ 收口报告要写清"容差族里有几项"，而不是笼统说"0 项差异"。
- 抖动的判定口径：`maxULP > 2`（抖动族之外）才算失败；抖动族 = `prov0_ao_*` / `hdr` / `radiance`。
- **基线目录不要删**：`build\verify\gi_s37fin2_*`；删了判据 ④ 会"跳过"而不是判定。

**③ 开关不变式常跑**
- 载体：判据 ⑥（`acceptance_sweep.ps1 -OnlyNanite` 也跑它）。关闭档冻结指纹
  `1C15AB72E688B5302332AEC391C41A5FE2B4D9512258CCDCD5D3E9D7E8F5390D`（12 pass）；开启档
  `passes=14 nanite_passes=2 sha=750CC247BF8B9C3D…`。
- **常跑的落点**：本仓库无 CI（扩展性分析已记录"无 CI 执行"），所以"常跑"目前只能是**每次改动收尾时人肉跑一次**
  —— 这一点任务 27 应在报告里**如实说明**（把它列为"已知缺口：无常驻守卫"），而不是宣称已常态化。

**④ 必须继承的既有事实（避免收口时误判）**
- 判据 ⑦/⑧ **有已知抖动**（§14.11：同一份未改动代码上各失败过一次；任务 22 那次是 300 s 超时且
  超时档 `hard_raster=0` 不走新代码）⇒ **跑 ≥2 次**。
- 模块接管档**两次运行并非逐位可复现**（§14.31 ⑩，软光栅深度键平局由 UAV 写序决定）⇒
  这是**既有问题**，会影响判据 ⑧ 的稳定性，**不要把它当成任务 23–26 的回归**；任务 27 若要让
  "0 项差异"这句话在接管档也成立，**必须先解决平局确定性**，否则只能在关闭档口径下声明。

### 14.36 任务 23 实施记录：混合光栅分配策略 + 性能读数（2026-09-21）

**目标**：让"阈值 / 簇大小分布对帧时的影响"**可读**。交付 = ① 可见簇的**簇大小分布**读数；
② 把**软硬分流计数**与**帧时**绑在同一行的性能读数（复用既有 `HE_CPU_PASSES` 与 `LogFrameBudget`，默认关）；
③ 可复现的帧时读数表 + 分配策略结论。完整报告：`build/verify/t23_report.md`（含全部原始读数与复现命令）。

**① 两条新读数（都不改既有行的字段与语义）**
```
[Nanite] size_dist buckets=[61,0,0,0,31587] total=31648 clusters=31648 visible=31648 sum_eq_clusters=1 sum_eq_visible=1 max_triangles=16
[Nanite] perf max_triangles=16 hard_raster=0 soft_clusters=61 hard_clusters=0 soft_pixels=3264 hard_pixels=0 nanite_pass_ms=8.265 nanite_pass_count=2 frame_ms=10.084 frame_fps_equiv=99.2 frame_ms_src=gpu_pass_sum
```
- **槽位方案（对任务书的一处偏离，已按"以代码为准"处理）**：任务 22 之后 0..14 已占用、**只剩槽 15**，
  而 5 桶需要 5 个连续槽 ⇒ 把**同一个** `u_Stats` 缓冲从 **16 条扩到 20 条**、五桶占 **15..19**：
  不新建 GPU 资源、不加 pass、清零/读回路径一行未改。C++（`NaniteTypes.h`）与 Slang
  （`Nanite_SoftRasterCommon.slang`）两侧各三个常量一一对应，并加了 `static_assert`
  （"紧跟 14 号槽 + 连续 + 吃满容量"）与一条单测（`TestNaniteTypes.cpp` 的 `NaniteSizeDist` 用例，
  1..64 全枚举与 shader 的 5 级阶梯逐分支等价、>64 夹取、全覆盖互斥）。
- **计数位置**：`Nanite_SoftRasterDepth.comp.slang`（第 1 趟）里 `triangleCount == 0` 早退之后、
  **分流判据之前** ⇒ 五桶覆盖"软 + 硬"两侧 ⇒ 不变式 `五桶之和 == soft + skipped_big == visible`。
- **不变式核对**：`build/verify/nan_*.log` 里**全部 25 个**含该行的日志（6 档多次测量 + 判据 ⑦ 的 5 个可视性档
  + 判据 ⑧ 的接管档）**零违反**；分布随可见集合变化（`hiz1 [57,0,0,0,31587]`、`ic8 [5,0,0,0,2588]`、`ic0 [0,0,0,0,0]`）。
- **`size_dist` 与 `soft_raster`/`hard_raster` 同门控**（`enabled && IsReady && softRaster`），
  **`perf` 额外受 `HE_CPU_PASSES` 门控**（默认关：不打印、连 Map 都不做）。`nanite_enable=0` 的关闭档一行不打。

**② 一个真问题：`LogFrameBudget()` 在 07.Nanite 里从不执行（本轮修掉）**
- **核实结果**：profiler 与帧预算**确实**覆盖 Nanite（`nanite_pass_count=2`；硬光栅录在
  `Nanite_CullChain3` 体内，其成本就在这条既有 pass 的 ms 上：实测 **8.26 → 41.19 ms**）⇒
  **未加任何 per-pass 计时、未加 pass**。
- 但 `LogFrameBudget()` 全仓库唯一的调用点在 **Lumen 段**（`DeferredPipeline_FrameGraph.cpp:1079`，
  被只在 Lumen 路径递增的 120 帧计数器门控），而 07.Nanite 的 cfg **未请求 Lumen**
  （`gi_blend_diffuse_lumen=0`，实测 `lumen_passes=0`）⇒ **【帧预算】行永远不会打印**
  （样例「帧率读数」行里那句"pass 合计见【帧预算】行"一直指不到东西）。
- **最小修法**：`07.Nanite.cpp` 在 **dump 帧、`WaitIdle()` 之后**补一次 `LogFrameBudget()`
  （门控 `enabled && HE_CPU_PASSES`）。`frame_ms` 与【帧预算】行的合计**逐位相同**（同一帧、同一来源），
  并加 `frame_ms_src=gpu_pass_sum` 标注口径；`frame_fps_cap` 改名 `frame_fps_equiv`
  （它是 GPU 等效帧率，与真实帧率差一个数量级）。

**③ 帧时读数表（6 档 × 2 次 + base16 补 2 次；两轮独立数据集；`HE_NO_VSYNC=1`）**

| 档 | soft簇 | hard簇 | nanite_pass_ms 中位[极差] | GPU 合计 中位[极差] | 墙钟 中位[极差] |
|---|---|---|---|---|---|
| base16（阈值 16、硬光栅关） | 61 | 0 | 7.75 [7.33–9.54] | 9.62 [9.10–11.57] | 131.1 [126.4–140.3] |
| base32 | 61 | 0 | 7.76 [6.87–8.01] | 9.57 [8.51–9.87] | 128.5 [127.0–131.7] |
| hard8 | 61 | 31587 | 40.57 [39.80–40.78] | 42.31 [41.20–42.86] | 143.5 [141.4–145.9] |
| hard16 | 61 | 31587 | 40.94 [40.63–41.47] | 42.68 [42.04–43.56] | 145.8 [145.3–146.4] |
| hard32 | 61 | 31587 | 40.19 [39.60–41.68] | 41.94 [41.65–43.06] | 144.7 [141.8–148.7] |
| **soft64（全软、全几何）** | 31648 | 0 | **779.8** [643.2–916.1] | **781.3** [644.5–917.4] | **830.3** [803.0–1010.9] |

**可复现性判定（两类读数分开）**
- **数据类：逐位可复现**（`size_dist` / `soft_clusters` / `hard_clusters` / `hard_pixels` 在各档多次运行里完全一致）。
  唯一例外是软光栅 `pixels_written` 在两轮数据集之间差 **0.02%**（`44309305` vs `44318207`）——
  根因是 §14.31 ⑩ 的**既有**平局问题（同一轮内逐位相同），不影响任何结论。
- **时序类：量级可复现，幅度受同档抖动限制**。判定口径：**跨档差异必须大于两档同档极差中较大者**。
  同档相对极差：轻负载档 10–26%、`hard*` 档 0.7–4.8%、`soft64` 档 25–35%。已识别一个离群点
  （`t23_base16_r2` 墙钟 212.5 ms 而该次 GPU 仅 9.10 ms ⇒ 机器侧干扰；已保留日志、统计中位数时剔除）。
- **可判"有差异"**：`hard*` vs `base*` 的 GPU（42 vs 9.6 ms，区间不相交）；下条的 soft64 vs hard16。
- **可判"无差异"**：阈值 8/16/32 三档之间（区间重叠）；base16 vs base32。

**④ 关键结论（**与"分流能省帧时"的直觉相反**，以实测为准）**
1. **同覆盖对照 `soft64` vs `hard16`**（同一批几何、同一覆盖：写标记像素 921399 vs 917903，
   交集 917903、`only_B=0`、`only_A=3496` 全在远平面外；`worldpos corr=0.9989`、`metallic hist corr=1.0000`）：
   **GPU 中位 779.8 vs 40.9 ms = 19.1×（好/坏情形 22.6× / 15.5×）；墙钟中位 830.3 vs 145.8 ms = 5.7×（5.5× / 7.0×）**
   ⇒ 混合光栅的价值是**同覆盖下 15~23× 的 GPU 成本**，这才是本任务成立的性能结论。
2. **`base16` 不能当"更省"的基线**：它只是**少画**了 31587/31648 = 99.8% 的可见簇
   （唯一覆盖 **106 px = 0.0051%**）。口径纠正：§14.11 写的"0.16% 屏幕（`pixels_written=3264`）"是**片元数**之比。
3. **阈值在本资产上是二值的**：分布双峰 `[61,0,0,0,31587]` ⇒ 实测阈值 **8/16/32 分流逐项相同**、
   GPU/帧时都在噪声内 ⇒ **阈值 ∈[4,32] 等价；= 64 退化**（全部软光栅，GPU 781 ms / 帧时 830 ms）。
   真正决定成本的是"**大簇走不走硬件路径**"。
4. **帧时侧不是分流杠杆**：本样例整帧 **CPU 受限**（GPU 合计 9~43 ms vs 墙钟 126~146 ms；
   `CPU 侧 ≈ 墙钟`）⇒ 把 GPU 从 781 ms 降到 42 ms 在同覆盖下只值约 11% 的墙钟。
5. **本轮量化了两项任务 22 只给了估计的量**：硬光栅**过绘 336.4×**
   （`hard_pixels 308810322 ÷ 本帧唯一写标记像素 917903`；任务 22 的"341×"用的是另一档的分母 921399，两者都对）；
   **mesh 线程利用率 50.0%**（`prims/clusters = 2021446/31587 = 63.996` ⇒ 交接的簇几乎都是满 64 三角形，
   而工作组 128 线程）。
6. **建议（只建议，默认阈值按现状保持 16）**：① 默认项不动；② 要提速，先修 CPU 侧（约 130 ms）与
   **软光栅逐像素路径**（44.3M 次像素写要 644~917 ms ≈ 48~69k 写/ms，而硬光栅 308.8M 片元只用 42 ms ≈ 7.3G 片元/s
   ⇒ **瓶颈在软光栅的逐像素竞争/原子，不在硬光栅的过绘**）；③ mesh 线程利用率 50% 与
   1,048,576 个工作组的提交成本可单独立项。

**⑤ 构建与验收**
- 构建 `07.Nanite` / `HugEngineTests` 全 **exit 0**；单测 **310 用例 / 62326 断言全绿**（含新增 `NaniteSizeDist`）。
- `acceptance_sweep.ps1 -OnlyNanite` **两次都 PASS**（本轮无抖动）；冻结指纹逐位复核：
  关闭档 12 pass = `1C15AB72E688B5302332AEC391C41A5FE2B4D9512258CCDCD5D3E9D7E8F5390D`（= 冻结值），
  开启档 14 pass / `nanite_passes=2` = `750CC247BF8B9C3DA2DEA6B7E893BED91E611145663CC037D0F198699F5C3E6F`；
  判据 ⑧d `new VUID types=0`。
- **未改**：`GBufferRenderer.*`（一行未动）、`NaniteSettings` 默认值、模块内无 GI/Lumen/GPUCulling 引用、
  CMake 无需改动（未新增文件；改动的三个 shader 早已登记）。

**⑥ 存疑未做**
- `执行(录制+提交)` 段 `base16 28.75 ms → hard16 40.32 ms` 的 **+11.6 ms 未定位**：逐 pass 的 CPU 录制读数显示
  `Nanite_CullChain3` 的 CPU 仅 **0.20–0.33 ms**（该行最大项是 Shadow 的 22.7–25.3 ms）⇒ 已排除"模块录制循环"，
  更可能是 1,048,576 个 mesh 工作组的**提交/驱动侧成本**或 CPU 等待更慢 GPU 帧的时间，**未定位到具体归属**。
- 帧时的样本数受机器非独占限制（`soft64` 两轮之间 GPU 相差 42%）；未追查该差异是负载还是状态相关。
- 未优化过绘与 mesh 线程利用率（本轮只量化）；未改软光栅 24 位深度键截断与平局口径；
  未接线 `DrawMeshTasksIndirectCount`；未把硬光栅做成独立帧图 pass（沿用任务 22 裁决）。
- 只测了 07.Nanite 的 1920×1080 单场景单相机；未测多分辨率与极端阈值（1 / 4）。

### 14.37 任务 24 实施记录：LOD 流式（反馈 + 页池，阶段一）（2026-09-21）

**目标**：落地 §14.32 的**阶段一** = 页池 + 页表 + 间接层 + 反馈通路 + 驻留管理 + 读数 + 开关，
页数据源为**留存在内存里的完整资产**（不做磁盘 I/O）。新增 `Engine/Render/Nanite/NaniteStream.{h,cpp}`
（已进 `Engine/Render/CMakeLists.txt` 显式列表），改动 6 个既有 C++ 文件、4 个着色器、样例与单测；
未新增着色器文件（因此 `Engine/Shader/CMakeLists.txt` 无需改动）。
**明确不做**（§14.32 ⑫）：磁盘 I/O 与分段读取、LOD 选择与预取、页压缩/去重、多资产共享池装箱、画面页可视化。

**① 最终页口径（与 §14.32 的对应关系）**

- **页 = 簇段 + 顶点段 + 三角形段三段各取一段**（§14.32 ② 的默认项；实现选的是"统一成三段"那条）：
  - **顶点段 / 三角形段** = "**连续 K 份共享内容**"的记录区间，端点由**去重升序**的
    `cluster.vertexOffset` / `cluster.triangleOffset` 集合给出（最后一份到段尾）⇒
    **页边界完全由资产自身推出**：不改 `.nanite` 格式、不保留构建期临时数组（§14.32 末段的结论成立）；
  - **簇段** = 引用了这些内容的那些"簇出现"记录的**收集**（按出现下标升序密集打包）+
    每簇一个**页内相对**下标（`NaniteClusterPageRef::local`）。
- **K 的口径变更（任务书 512 簇/页 → 实现 512 份共享内容/页）**：§14.32 修正后页必须对齐到"整份共享内容"
  边界，K 的含义相应变化。Sponza 实测 `contentCount=8202`、`pageCount=17`（K=512）。
- **为什么簇段是"收集"而不是"区间"**：去重路径下 `cluster.vertexOffset` **不单调**（§14.32 的修正）。
  单测在**真实资产**上把这条钉成可执行事实：`前 4 个 vertexOffset=[0 45 0 90 …] 单调=0`
  （造法：4 片 64 tri 的网格里第 0/2 片形状逐位相同、第 1/3 片各起一条脊 ⇒ 第 2 片去重命中回第 0 片）。
  代价是每簇 8B 映射表（Sponza 66 KB），换来"顶点/三角形段是真正的区间"⇒ 偏移换算只有一次减法 + 一次乘法。
- **纯函数 + 单测**：`BuildNanitePagePlan`（RHI-free，`NaniteUpload.cpp`）。新增 `Tests/TestNaniteStream.cpp`
  （文件头按仓库体例写了覆盖范围，编号续 `TestNaniteBuilder.cpp` 的 22 条、从 23 起）：
  **6 用例 / 8,766 断言**，覆盖空资产、K=0、段空、段首空洞、**两张共享表不同源**、不变式
  （页数 / 双射 / 不跨页 / 槽步长 / 槽字节）、前缀和自洽、`PoolBytes`、确定性、K 的影响、真实资产路径。

**② 判据 (d) 的改口径（原始形式不可达；不悄悄放宽）**

§14.32 ⑪(d) 原文要求"页池足够大时 `streaming=1` 与 `streaming=0` 在 4 张 GBuffer 上**逐位相同**"。
**该形式不可达，原因与任务 24 无关**：§14.31 ⑩ 已实测"模块接管档两次**同配置**运行并非逐位可复现"
（软光栅深度键平局 ⇒ 多个三角形都通过等值复检 ⇒ 由 UAV 写入顺序决定）。本轮重新量了这条噪声底，实际采用三条：

**(d1) 数据类读数逐位相同（最强证据）** —— `nan_t24_off_a`（`nanite_enable=1`）vs
`nan_t24_on`（`nanite_enable=1;nanite_streaming=1`）；**两档只差 `nanite_streaming` 一个键**（逐键核对：差异键数 = 1）：

```
soft_raster   soft=61 skipped_big=31587 pixels_written=3264 degenerate=0 neutral_material_pixels=0
              material_pixels=3264 fallback_pixels=0 multi_mesh_clusters=97 max_triangles=16
              instances=64 depth_written=106 covered_px=3264 tested_px=9767
              diag_screenw=1920 diag_screenh=1080 diag_maxtri=16 diag_extent_milli=3720854
size_dist     buckets=[61,0,0,0,31587] total=31648 clusters=31648 visible=31648
              sum_eq_clusters=1 sum_eq_visible=1
visible_wiring visible=indirect_count=draws=rasterized=31648 empty_draws=0 mismatch=0
```
两档**逐字段相同**；开启档的 `stream` 行另有 `page_misses=0`、`table_ok=1`、`dup_slots=0`、
`gpu_resident=resident=9`、`uploads_this_frame=0 ≤ uploads_limit=4`。
**非空洞守卫必须用累计量**：稳态下当帧请求数天然为 0（需要的页都已驻留），所以断言的是
**`requests_total=24 > 0`**（`pages_requested_total=9 > 0`、`max_requests_per_frame=9`）——
若拿当帧 `pages_requested=0` 去判，功能完全正常时也会误报失败。

**(d2) 像素类差异不超过噪声底**（工具 `build/verify/cmp_dumps.py`）：

| 对照 | 总差异像素 | 受影响文件 | 其余文件 |
|---|---|---|---|
| 噪声底 `off_a` vs `off_b`（同配置两次） | **734** | albedo 238 / hdr 208 / gb_lightmapkey 158 / gb_normal 64 / gb_worldpos 66 | 13 个**逐位相同** |
| 信号 `off_a` vs `on` | **729** | albedo 240 / hdr 205 / gb_lightmapkey 206 / gb_normal 64 / gb_worldpos 14 | 13 个**逐位相同** |

⇒ 信号 ≤ 噪声底，且**受影响文件集合完全相同**（恰好是 §14.31 ⑩ 记录的那 4 张 GBuffer + `hdr`）。
**如实标注两点**：① 逐文件的 `maxULP`/`maxRel` 是"差异像素上的最大值"、本身抖动很大
（albedo `maxRel` 28.17 → 79.33、gb_normal `maxULP` 2914 → 2081），稳健判据是 `diff_px` 与 `total`；
② 噪声底取自**一次**配对运行 ⇒ 结论是"差异与同档抖动同阶、受影响集合一致"，
**不是**"已证明统计意义上无差异"。

**(d3) 结构性证据（口径修正）** —— `resident=9`、`pages_total=17`、`page_misses=0`、`gpu_resident=9`。
**`resident == pages_total` 按字面不可达**：阶段一**不做预取**（§14.32 ⑫），只有被请求过的页才驻留；
当前视角只需要 9 页，另外 8 页从未被任何可见簇引用（由 `page_misses=0` 反证）。
⇒ 实际口径是 **`resident ≤ pages_total` 且 `resident == gpu_resident`（CPU/GPU 两本账一致）
且 `page_misses == 0`（所有**被需要的**页都已驻留）**。

**③ 实施中做出的显式选择与偏离（设计允许但要求说明）**

1. **命令链：选"命令保持资产空间"，即 §14.32 第二处追加的 (i)，而非它建议的 (ii)**。理由：
   ① 间接命令**唯一**的消费者是占位光栅（`Nanite_Raster.vert/frag`），它只用 `vid % 3` 与
   `SV_PrimitiveID` —— 对任意整数 `vertexOffset` 都给出 `{0,1,2}` 的一个排列（该文件自己的注释证明了这点）
   ⇒ 命令里的偏移**不被当作地址使用**；② 把 `firstIndex` 换成池内偏移会让 `firstIndex + indexCount`
   冲出占位索引缓冲的覆盖范围（本设备未启用 `robustBufferAccess`）⇒ 引入真实越界读风险、收益为零；
   ③ 命令字段还被 `LogVisibleWiringReadback` 与 CPU 参考逐字段比对，改口径就要同时改参考。
   **证据**：流式开启档 `visible = indirect_count = draws = rasterized = 31648`、`mismatch=0`
   ⇒ 命令链没有被间接层弄坏（设计担心的"占位光栅静默用错偏移"因此不成立）。
2. **页请求由光栅第 1 趟产生，而不是剔除链**（§14.32 ④ 的原文是剔除链）。这样**剔除链一行未改**
   ⇒ 任务 16 的 `V=C=D=R` 与 CPU 参考交叉核对在流式档仍然成立；代价是缺页判定在光栅端。
3. **去重 = GPU 侧戳记数组 + CPU 读回侧合并，页表仍然"只由 CPU 写"**。§14.32 ④ 原写"去重由页表的
   `requestedFrame == 当前帧` 判定"，那要求 **GPU 写页表**，而页表同时被 CPU 每帧重写 ⇒ 同一块内存
   无同步双边写。改成"反馈缓冲尾部一段**只由 GPU 写**的戳记（CPU 只在 `WaitIdle` 之后清 0）"后，
   页表保持单写者。**实测必要性**：不去重时单帧请求数 = 可见簇数（`max_requests_per_frame=1024` 撞满、
   `overflow_total=119476`，且有"某页的请求恰好全落在环外 ⇒ 永不驻留"的风险）；去重后
   `max_requests_per_frame=9`、`overflow_total=0`。
4. **页表项 = `slot` + 三个池槽步长（push constant），而不是设计建议的 `poolBase`**：三段各有各的步长，
   "一个 poolBase"表达不了三段；带三个 base 又等于把 push constant 里的步长抄三遍，而
   `slot × stride` 只是一次整数乘法。**"页起点"字段一个不少**（`vertexBegin` / `triangleBegin`）。
5. **每帧一次 `WaitIdle()`**（`NaniteStream::BeginFrame` 开头）。页表与页池都是 **CPU 写、GPU 读**，
   而本引擎的缓冲是持久映射的 host-visible 内存（`VulkanResources.cpp` 的 VMA 参数）⇒
   只有"没有在飞命令"时重写它们才无竞态。有了它，"淘汰前确认没有在飞命令引用它"
   **在物理上不可能违反**；`kNanitePageEvictionSafetyFrames`（3 帧）仍保留，用来挡住"刚请求就被踢掉"的抖动。
   代价：流式档把 CPU/GPU 并行度压成串行（本样例整帧本就 CPU 受限，§14.36 实测墙钟 126–146 ms
   vs GPU 9–43 ms，对功能验收无影响）。**真正的流水线化（双缓冲页表 + 帧龄延迟写入）留给阶段二**。
6. **资产留存为 CPU 副本**（§14.32 ⑦ 的第三处追加）：`NaniteScene::StoreAssetCPUCopy` 收下资产的
   簇/顶点/三角形/材质四段并**丢掉 `bytes` 字节镜像**（它的唯一消费者——上传时的逐字节读回校验——已跑完）。
   **实测**：`asset_retained clusters=8287 vertices=542190 triangles=524657 materials=103
   retained_bytes=13405960 dropped_mirror_bytes=13406096` ⇒ 留存 **13,405,960 字节（12.78 MiB）**；
   丢掉镜像省下一半（否则约 26.8 MB）。它**不改变帧图 / pass 集合 / 转储**（不变式 1 管的是这三件事），
   但确实是"只开 `enabled` 也不变"在**内存侧**的一个例外，如实记录在此。
7. **`NaniteClusterPageRef::local` 的口径统一为"页内相对"**（**单测抓出来的真缺陷**）。实现初版把它写成
   "整条收集序里的绝对下标"，而 GPU 侧按"页内相对"用（`slot*clusterStride + local`）、CPU 侧装页时
   又把 `pageClusterBegin + local` 当绝对位置 ⇒ 两处都不对：第 0 页（begin==0）碰巧正确，第 1 页起
   **读到位移错的簇记录**（画面错但不崩），同时 CPU 侧反查表**越界写**。修法：`cursor` 从 0 起计数
   （`local ∈ [0, pageClusterCount)`）。三个消费者现在一致（生产者 `NaniteUpload.cpp`、
   CPU 消费者 `NaniteStream.cpp`、GPU 消费者 `Nanite_SoftRasterCommon.slang`）。
8. **两处防御性加固（都在实测的驱动下补上）**：
   - `UploadPage` 改为"**三段都写成功才标记驻留**"（`Map()` 失败时保持非驻留并打一条错误）。
     此前"先标记驻留、Map 失败就静默跳过"会让着色器去读**未初始化**的池内存 ⇒ 里面的
     `triangleCount`/包围盒是任意值 ⇒ 最坏情形是 GPU 看门狗超时（CPU 侧只表现为 `WaitIdle` 永久阻塞、
     **零错误输出**）。**这正是池 8 槽档从"卡死在第 97 帧"变成"完整跑完 121 帧"的那处改动**（见 ⑥）。
   - 软光栅第 1 趟把 `triangleCount` **夹到资产契约上限 64**（不是夹到 `maxTriangles`！）。
     **这里踩过一次坑并如实记录**：先写成"夹到 `maxTriangles`"，结果 64 三角形的簇被"变成"16 个、
     **不再被分流判据跳过**，实测 `soft=31526 / skipped_big=0 / tested_px=285M`
     （健康档是 `soft=61 / skipped_big=31587 / tested_px=9767`）—— 那是**语义破坏**而不是加固。
     夹到 64 则**语义不可见**：>64 的簇在夹取前后都走"超阈值跳过"，分桶也都是最后一桶，任何读数都不变。

**④ 读数（默认关闭时不打印）**

```
[Nanite] stream pages_total=17 resident=9 pool=64 uploads_this_frame=0 evicted=0 page_misses=0
         pages_requested=0 stream=on reason=ok gpu_resident=9 table_ok=1 nonresident=8 dup_slots=0
         contents=8202 K=512 strides=(c=589 v=55256 t=32768) slot_bytes=(c=37696 v=884096 t=262144)
         pool_bytes=75771904 requests_total=24 pages_requested_total=9 max_requests_per_frame=9
         overflow_total=0 latency=2 uploads_limit=4
```
- 门控：`enabled && streaming` 才打印 ⇒ **默认档与关闭档的日志逐字不变**（判据 (a)）。
- `page_misses` / `pages_requested` 是**真实 GPU 读回**（软光栅读数缓冲第 20/21 槽，第 1 趟原子累加，
  且写入在同一帧那次清零**之后**）。**这不是形式要求**：读数缓冲每帧被清 0，着色器若不写这两个槽，
  读回就恒为 0 ⇒ `page_misses == 0` 会在"流式完全没工作"时也成立（**空洞通过**）。
  C++ 与 Slang 的容量现在都是 22（`static_assert` 钉住连续性与"不得越界/不得恒 0"的注释约定）。
- **退化不静默**：`stream=off reason=<…>` 取值为 `requires_soft_raster` / `pool_zero_slots` / `no_asset` /
  `plan_failed` / `page_straddle` / `resource_failed` / `no_device`；`plan_failed` 时另有一条
  `HE_CORE_WARN` 带上"第一个秩不一致的簇下标与两个秩"（避免留一个"看起来能读、实际恒 0"的字段）。
- **不变式的推广（任务 23 那条要延拓）**：五桶之和 == `soft + skipped_big`（`sum_eq_clusters`）**依旧成立**；
  而 `sum_eq_visible` 在流式档会因为**缺页被跳过的簇**而变 0。正确形式是
  **`Σ五桶 + page_misses == visible`**（池 8 槽档实测 `31526 + 122 = 31648` ✓ 精确成立）；
  关闭档 `page_misses` 恒为 0 ⇒ 退化成既有形式。

**⑤ 页池足迹与调参建议（只建议，默认值不动）**

Sponza 实测（同一资产、同一相机，只改 K）：

| K | pages_total | 每槽字节 | 池@64 槽 | 池@`slots = pages_total` | 全驻留最小槽数 |
|---|---|---|---|---|---|
| **512（默认）** | 17 | 1,183,936 | **75,771,904**（72.26 MiB） | 20,126,912（19.19 MiB） | 17 |
| 128 | 65 | 301,648 | 19,305,472（18.41 MiB） | 19,607,120（18.70 MiB） | 65 |
| 64 | 129 | 155,184 | 9,931,776（9.47 MiB） | 20,018,736（19.09 MiB） | 129 |

- **恒等式**：`每槽字节 × pages_total` 在三档里稳定在 **19.6–20.1 MB（±1.5%）≈ 资产的总内容字节**；
  于是 **池膨胀倍数 = `pool_slots / pages_total`**（64/17=3.76×、64/65=0.98×、64/129=0.50×，与实测吻合）。
  ⇒ 75.77 MB 的来源**不是 K**，而是"64 槽 / 17 页"这个比值。
- **建议（后续优化，不改本任务默认值）**：`pool_slots = pages_total`（默认 K=512 时是 17 槽）可同时做到
  "全部页可同时驻留 + 稳态零驱逐 + 池约 20 MB"；更强的做法是让槽数**由页划分自动推导**（cfg 值退化为上限）。
  **注意**：`pool_slots < pages_total` 会**强制驱逐**，而驱逐路径正是本轮唯一卡死过的那一档（见 ⑥）——
  不能靠调参绕过去。
- K 只影响池足迹、**不影响画面**：K=512/128/64 三档的 `soft_raster` / `size_dist` 读数**逐字段相同**，
  `passlist_sha` 都是 `750CC247…`、`passes_per_frame=14`。

**⑥ 构建与验收**

- 构建 `07.Nanite` / `HugEngineTests` / `HugEngineRender` 全 **exit 0**。
- 单测 **316 用例 / 71,095 断言全绿**（`Status: SUCCESS!`；任务 23 基线 310 / 62,326 ⇒ 新增 6 用例 / 8,769 断言）。
- **判据 (a)**：`nanite_enable=1`（`nanite_streaming` 缺省 = 0）的 `nan_t24_off_a/off_b`：
  `passes_per_frame=14`、`passlist_sha=750CC247BF8B9C3DA2DEA6B7E893BED91E611145663CC037D0F198699F5C3E6F`
  —— 与任务 23 的开启档**逐位相同**；关闭档 12 pass 的冻结指纹 `1C15AB72…` 由 `acceptance_sweep` 判据 ⑥ 复核。
  【这一档还**首次真正执行了 bindings 15~20 的占位绑定分支**（此前只做过静态检查）：VUID 行数与类型分布
  （42 行 / 6 类）与任务 23 基线 `nan_takeover_on64` **完全一致** ⇒ 占位绑定不产生任何新校验错误。】
- **判据 (b)**：`table_ok=1`、`dup_slots=0`、`nonresident=8`（9+8=17=`pages_total`）、
  **`gpu_resident == resident`**（页表缓冲里真实标成驻留的条数与 CPU 的账一致）。
- **判据 (c)（页池 8 槽）**：完整跑完 121 帧、`vuid_lines=42`（与基线一致）、
  `page_misses=122 > 0`、`resident=8 = gpu_resident=8`、`evicted=58`、`table_ok=1`、`dup_slots=0`、
  `overflow_total=0`，且**读数与 `visible` 的差额精确可核对**：
  `soft(61) + skipped_big(31465) + page_misses(122) = 31648 = visible`、
  `Σ五桶(31526) + page_misses(122) = 31648 = visible`。⇒ 不崩、不越界、表自洽、账目对得上。
  **本档最初是卡死的**（第 97 帧、`WaitIdle` 永久阻塞、零错误输出）：定位结论是"页被标成驻留但槽里
  还是未初始化内存"（见 ③ 条 8 的第一条），修好后连续两次完整跑完。**驱逐/槽位复用这条路径
  此前从未被任何档走到过**（64 槽档预热后零驱逐），本轮是它的第一次验证。
- **判据 (d)**：按 (d1)/(d2)/(d3) 三条执行，全部成立（见 ②）。
- **判据 (e)**：`uploads_this_frame=0 ≤ uploads_limit=4`（8 槽档为持续驱逐，同一字段仍受上限约束）；
  反馈延迟 `latency=2` 由环槽 `frameIndex % latency` 实现 ⇒ "请求到驻留"的延迟**恰好**是常量。
- **判据 (f)**：`acceptance_sweep.ps1 -OnlyNanite` **两次都 PASS**（跑前清掉 `HE_NO_VSYNC`，
  原始输出见 `build/verify/t24_sweep.txt`）：
  ```
  [6a] off passes=12 nanite_leak=0 sha=1C15AB72E688B530            ← 冻结指纹（完整值见下）
  [6b] module-on passes=14 nanite_passes=2 preexisting_set_changed=False
  [6c] tier=nanite_soft_raster=0 pairs=20 differing_outside_jitter=0 jitter_family_ondiff=3
  (7)  CULL DIFF: PASS      (5 档：default/hiz1/ic8/ic0/cap1000 全 OK)
  (8)  TAKEOVER CMP: PASS   (8a V=C=D=R=31648、wiring_mismatch=0；8c 阈值 64 档也成立)
  ACCEPTANCE SWEEP: PASS    （两次）
  ```
  冻结指纹逐位复核：关闭档 12 pass =
  `1C15AB72E688B5302332AEC391C41A5FE2B4D9512258CCDCD5D3E9D7E8F5390D`（= 冻结值）；
  开启档 14 pass / `nanite_passes=2` =
  `750CC247BF8B9C3DA2DEA6B7E893BED91E611145663CC037D0F198699F5C3E6F`。本轮**无抖动**（两次结果逐字相同）。
- **未改**：`GBufferRenderer.*`（一行未动）、`NaniteSettings` 的既有默认值、模块内无 GI/Lumen/GPUCulling 引用；
  `Engine/Shader/CMakeLists.txt` 无需改动（未新增着色器文件，4 个改动的 shader 早已登记）；
  `Engine/Render/CMakeLists.txt` 与 `Tests/CMakeLists.txt` 各加了一条显式登记（新文件）。

**⑦ 存疑未做 / 已知问题**

1. **LRU 在"池 < 工作集"时无法收敛**（设计层面的观察，不是实现 bug）：LRU 的键是"最近一次被请求的帧"，
   而静态视角下所有驻留页**每帧都被请求** ⇒ 池小于工作集时**找不到任何可驱逐的页**……
   实测 8 槽档确实发生了 58 次驱逐（因为缺页页的请求会把 `lastRequestedFrame` 推进，
   驱逐发生在"某些页这一帧没被请求"的间隙），但稳态仍是"永久缺页"（`page_misses=122` 持续存在）。
   就 (c) 的字面要求（`page_misses > 0` 且无未定义行为）这是可接受的，但**"池小于工作集"不是优雅降级
   而是长期少画** —— 真正的降级策略（回退更粗 LOD）属 LOD 选择，**明确留给阶段二**。
2. **流式档每帧一次 `WaitIdle()`**（③ 条 5）：功能正确但不流水线化；替代方案（双缓冲页表 +
   延迟 N 帧写入 + GPU 侧清环）留给阶段二。
3. **不做预取 / LOD 选择**（§14.32 ⑫）：N5 的"无 pop"判据**未触及** —— 本任务只做到"缺页时不崩、
   不越界、可观测"。(d3) 的口径已按"按需分页"修正。
4. **页池不压缩、不去重、不多资产共享**（§14.32 ⑫）；默认档池足迹 75.77 MB 明显大于资产的 12.78 MB，
   调参建议见 ⑤，但**默认值按任务书保持不变**。
5. **只测了 07.Nanite 的 1920×1080 单场景单相机**；相机固定 ⇒ 可见集固定 ⇒ 稳态缺页为 0，
   测不到"相机移动时的 pop 与缺页轨迹"。未测多分辨率、多资产。
6. **`page_misses` 的口径**是"被跳过的**可见簇数**"（不是页数）——它能对上
   `soft + skipped_big + page_misses == visible`；"请求了多少"由 `pages_requested` /
   `pages_requested_total`（**累计入队页次数**，同页可重复计）表达。两者并列打印，不互相冒充。

### 14.38 任务 27 收口记录：单测 / 默认预设 / 开关不变式（2026-09-21）

> 任务 27 的验收原文：「`HugEngineTests` 全绿；默认预设抖动族之外 0 项差异；开关不变式（任务 2）常跑」。
> 本节按 §14.35 的三条载体逐条给出**实测**，并把**未覆盖**与**已知缺口**如实列出（不含糊）。

**① 单测全绿 —— ✅**

- `cmake --build build --config Release --target HugEngineTests` → **exit 0**；
  `build\bin\Release\HugEngineTests.exe` → **exit 0**，`test cases: 322 | 322 passed | 0 failed`、
  `assertions: 71192 | 71192 passed`、**`Status: SUCCESS!`**。
- **为任务 23–26 新增机制补测**（§14.35 ① 的硬要求）：三项点名要求**全部已有覆盖** ——

| 点名要求 | 覆盖用例 | 实测 |
|---|---|---|
| `size_dist` 桶边界与"五桶之和 == 可见簇数" | `NaniteSizeDist*` | **1/1 passed** |
| **材质 bin 的排序正确性**（任务 25） | `NaniteMaterialBin*`（合成 3 + 真实资产 1） | **4/4 passed** |
| 任务 24 页表自洽 | `NanitePage*` | **6/6 passed** |
| （另）材质映射正确性（任务 25 的缺陷修复） | `NaniteMaterialMap*` | **3/3 passed** |

- **必须分清的口径（如实标注）**：`NanitePage*` 覆盖的是**页划分纯函数**（`BuildNanitePagePlan`）；
  运行时**页表**的自洽需要 RHI、**无法单测**，它由 `stream` 读数行的 `table_ok=1` / `dup_slots=0` /
  `gpu_resident == resident` 覆盖（见 §14.37 判据 (b)）。同理，读数自检（§14.34 第 2 行）、软光栅深度键
  平局计数（第 7 行）、硬光栅 push constant 回读（第 10 行）都在 RHI / 着色器侧，其证据是**运行期**的
  ——读数自检另有**负向验证**证明非空转——而不是单测。

**② 默认预设抖动族之外 0 项差异 —— ✅ PASS（容差族 5 项）**

- 载体：`acceptance_sweep.ps1` 判据 ④（`aq_def` 默认预设、**Nanite 关闭**、`lumen_passes=242`
  对基线 `s37fin2`）。**本次为新鲜实测**，不是复读旧记录：

| 类别 | 项数 |
|---|---|
| 逐位相同 | **18** |
| 抖动族（跳过：`hdr` / `prov0_ao_final` / `prov0_ao_raw`） | 3 |
| **容差族（既有、与 Nanite 无关）** | **5**（`lumen_irradiance` + 4 个 `prov6_*`，`maxULP=1`、`meanAbs=1.52e-09`） |
| **抖动族之外差异** | **0** ⇒ PASS |
| `MISSING` / `SIZE` | 0 |

- 按 §14.35 ② 的要求，这里写明「**容差族里有 5 项**」而不是笼统的"0 项差异"；这 5 项是 §14.11 记录的
  **既有豁免**（与 Nanite 无关——判据 ⑥ 已证明关闭档一个 pass 都不注册），**不要试图修**。
- 基线目录 `build\verify\gi_s37fin2_*`（28 个文件）**不要删**，删了判据 ④ 会"跳过"而不是判定。

**③ 开关不变式常跑 —— 载体 ✅，但"常跑"不成立（如实标注）**

- 载体：判据 ⑥（`acceptance_sweep.ps1 -OnlyNanite` 也跑它）。实测：关闭档 12 pass、指纹
  `1C15AB72E688B530…`（**等于冻结值**）、`nanite_leak=0`；开启档 14 pass / `nanite_passes=2` /
  sha `750CC247BF8B9C3D…`。本轮两次 `-OnlyNanite` 均 `ACCEPTANCE SWEEP: PASS`。
- **已知缺口**：本仓库**无 CI**（扩展性分析已记录"无 CI 执行"），所以"常跑"目前**只能是每次改动收尾时
  人肉跑一次**，**没有常驻守卫**。更关键的是：**整套验收脚手架自身未纳入版本控制** —— `build/verify/`
  被 `.gitignore` 忽略，全仓库**没有任何被跟踪的验收脚本副本**，且 7 个脚本含硬编码绝对路径
  ⇒ **判据无法从克隆重现**（期望值即冻结指纹已进本文档，但产生与核对它们的**机制**没有）。
  建议后续把脚手架移进受跟踪位置（如 `Tools/nanite_acceptance/`）并把路径改为由仓库根推导。

**④ 必须继承的既有事实（本次执行中已遵守）**

- 判据 ⑦/⑧ **有已知抖动**（§14.11：同一份未改动代码上各失败过一次）⇒ **已跑 ≥2 次**（两次均 PASS）。
- 模块接管档**两次运行并非逐位可复现**（软光栅深度键平局由 UAV 写序决定，§14.31 ⑩）⇒ 这是**既有问题**，
  因此"0 项差异"这句话**只能在关闭档口径下声明**；判据 ④ 恰是关闭档（`nanite_enable=0`），口径一致。
- 判据 ⑦/⑧/④ 的**脚本脆弱点**已集中记在 §14.34 表格第 17 行（共六条，含"相对路径转储""判据 ⑦ 量级盲区"
  "脚手架未纳入版本控制""样例残留进程""判据 ④ 对缺转储空洞通过""两套 smoke 脚本不可混用"），后续修时不必再翻历史。

**⑤ 收口结论**

| 载体 | 结论 |
|---|---|
| ① 单测全绿 | ✅ 322 用例 / 71192 断言 / `SUCCESS!`；任务 23–26 的三项点名补测**全部有覆盖** |
| ② 默认预设抖动族之外 0 项差异 | ✅ **PASS**（逐位相同 18、抖动族 3、**容差族 5**、抖动族之外 **0**） |
| ③ 开关不变式常跑 | 载体 ✅ 且实测通过；但"**常跑**"**不成立** —— 无 CI，且脚手架未纳入版本控制 |

**未覆盖 / 已知缺口（如实列出）**：
1. **无 CI** ⇒ 没有常驻守卫，"常跑"依赖人工。
2. **验收脚手架未纳入版本控制且含硬编码绝对路径** ⇒ 判据不可从克隆重现。
3. **运行时页表**与三项 RHI/着色器侧机制（读数自检、平局计数、硬光栅回读）只有**运行期证据**、无单测。
4. **判据 ④ 对缺转储（`MISSING` / `SIZE`）空洞通过** —— 与判据 ⑧b/⑧c 同属"缺转储即通过"，建议一并修。
5. **只测 07.Nanite 的 1920×1080 单场景单相机**；未测多分辨率、多资产、相机移动。
