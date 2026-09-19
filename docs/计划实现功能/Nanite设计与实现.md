# Nanite 设计与实现

> 最后更新: 2026-09-19
> 状态: 设计规范已定稿；N1-N3 实现计划待执行（预处理 + 剔除 + 软光栅 GBuffer）

## 0. 本文件怎么读

- **第一~九章是设计**：回答"做成什么样" —— 目标与定位、现有基础设施、数据流、预处理与
  运行时的形状、运行时 GPU 资源、与现有管线的集成点、关键数据结构、里程碑与已知风险。
- **第十章起是 N1-N3 实现计划**：回答"怎么按任务落地" —— 全局约束、文件结构、Task 1-10 的
  逐个步骤（含代码骨架与验收标准）、完成标准。Task 里的 `- [ ]` 复选框原样保留，用于逐任务跟踪。
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

### 0.2 源文档不一致清单（保留双方说法，不裁决）

合并时发现下列不一致。**两处都保留**，并在正文对应位置就地标注。

| # | 条目 | 说法 A | 说法 B | 位置 |
|---|---|---|---|---|
| 1 | `NaniteCluster` 第 2 个字段命名 | 设计：`float4 coneData`（"normal cone（法线锥剔除）"） | 计划：`float4 coneAxisAngle`（"xyz=coneAxis, w=coneAngle(cos)"） | §8.1 |
| 2 | Python 预处理工具的落点 | 设计 §4：`Engine/Shader/Shaders/Nanite/Nanite_Preprocess.py` | 计划：`Tools/NanitePreprocess/NanitePreprocess.py`（另拆 5 个模块） | §4.3 / §11 |
| 3 | 软光栅 Shader 文件名 | 设计 §4：`Nanite_SoftRasterize.comp` | 计划：`Nanite_SoftRaster.comp` | §7.2 / §11 |
| 4 | Shader 扩展名规范 | 计划 Global Constraints："Shader 统一使用 Slang `.comp`/`.mesh` 命名规范"；设计 §4 与计划 File Structure 均写 `.comp`/`.mesh` | 仓库实际：`*.comp.slang` / `*.mesh.slang`（`Engine/Shader/CMakeLists.txt:125-127`） | §10 / §11 / §12 Task 6、8 |
| 5 | 软/硬光栅分流阈值 | 设计 §3.3：`triCount > 16` → Mesh Shader，`<= 16` → Compute 软光栅 | 计划 N1-N3：只有软光栅，`Nanite_SoftRaster.comp` 对 cluster 最多 64 个三角形统一处理，无 16 三角形阈值 | §5.2 |
| 6 | 索引编码 | 计划 `NaniteTypes.slang` 注释："3×u16 打包到一个 u32[2]" | 计划 `NanitePack.py` 按 `indexCount × 4B` 写 u32/索引；`Nanite_SoftRaster.comp` 逐 u32 取 3 个索引 | §8.5 |
| 7 | 量化顶点步长 | 计划 `NaniteVertex` = 4×u32（含 `_pad`）= 16B | 计划 `NanitePack.py` 每顶点写 12B；`NaniteUpload.cpp` 按 `vertexCount*3*sizeof(u32)` = 12B/顶点读 | §8.4 |
| 8 | `.nanite` 文件头大小 | 计划 `pack_nanite` docstring：`[NaniteFileHeader 128B]` | 按字段累加 = 96B（`8+4×6+4+12+12+4+32`），Python 写 `<32x>` reserved | §8.3 |
| 9 | 量化/反量化对称性 | 计划 `quantize_vertices`：`(vertices-bbox_min)*scale`，打包无符号 0…1023 | 计划 `decodeVertexPosition`：`int(packed & 0x3FF) - 512`，按 SNORM 有符号解码 | §8.4 |
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

**逐字段**（按 `alignas(16)` / std430 推算偏移；两边字段顺序完全一致，总计 64B，与
`NanitePack.py` 的 `clusterCount × 64B` 相符）：

| 偏移 | 字段 | 类型 | 含义（两边注释合并） | 差异 |
|---|---|---|---|---|
| 0 | `boundingSphere` | `float4` | xyz=center, w=radius（cluster 包围球） | 一致 |
| 16 | `coneData` / `coneAxisAngle` | `float4` | 设计："normal cone（法线锥剔除）"；计划："xyz=coneAxis, w=coneAngle(cos)" | **注：源文档此处不一致（#1）**——字段名与语义描述都不同，实现时必须二选一并同步 C++ 与 slang |
| 32 | `triangleOffset` | `u32` / `uint` | index buffer 中的偏移；计划注释另写"(三角形数)" | 一致（注释措辞差异） |
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
| 64 | `_reserved[8]` | `u32[8]` | 保留 |
| — | 合计 | — | **96 字节** |

**注：源文档此处不一致（#8）**：`NanitePack.pack_nanite` 的 docstring 写
`[NaniteFileHeader 128B]`，但按上表字段累加是 **96B**（Python 侧写的是 `<32x>` 保留区，
与 `u32 _reserved[8]` 一致）。实现时以 96B 为准（或统一改成 128B，但两边必须同时改）。

#### 8.4 量化顶点与打包格式

实现计划 Task 1 的 slang 定义（原文）：

```hlsl
// 量化顶点 (R10G10B10A2 + 量化范围)
struct NaniteVertex {
    uint packedPosition;   // R10G10B10A2_SNORM (xyz) + w=1
    uint packedNormal;     // R10G10B10A2_SNORM (xyz)
    uint packedUV;         // R16G16_UNORM (uv)
    uint _pad;
};
```

| 偏移 | 字段 | 内容 |
|---|---|---|
| 0 | `packedPosition` | R10G10B10A2_SNORM（xyz）+ w=1 |
| 4 | `packedNormal` | R10G10B10A2_SNORM（xyz） |
| 8 | `packedUV` | R16G16_UNORM（uv） |
| 12 | `_pad` | 对齐填充（结构体 16B） |

打包/解包实现（Task 3 `quantize_vertices`、Task 8 `NaniteShared.slang`）：

```python
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

```hlsl
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
```

**注：源文档此处不一致（#7、#9）**：
- #7 步长：`NaniteVertex` = 4×`u32` = **16B**（含 `_pad`），但 `NanitePack.py` 每顶点只写
  12B，`NaniteUpload.cpp` 也按 `vertexCount * 3 * sizeof(u32)`（= 12B/顶点）读取。
- #9 对称性：`quantize_vertices` 产出的是**无符号** `0…1023`（`(v-bbox_min)*511/maxExtent`），
  而 `decodeVertexPosition` 用 `int(...) - 512` 按 **SNORM 有符号**解码。两边差一个 512 偏置；
  且 `quantize_vertices` 未打包 `packedNormal` / `packedUV`（见 #13）。

#### 8.5 三角形索引编码

实现计划 Task 1 的注释（原文）：

```hlsl
// 三角形索引 (3×u16 打包到一个 u32[2])
// indices[0]: i0 | (i1 << 16)
// indices[1]: i2 | (padding << 16)
```

**注：源文档此处不一致（#6）**：`NanitePack.py` 按 `indices (indexCount × 4B)` 逐索引写
`u32`，`NaniteUpload.cpp` 按 `std::vector<u32> indices(header.indexCount)` 读，
`Nanite_SoftRaster.comp` 也按 `u_Indices[idxBase + 0/1/2]` 逐个 u32 取三个索引 —— 三处都是
"1 索引 1 个 u32"，与上面"3×u16 打包进 `u32[2]`"的注释不符。实现时二选一（打包版省一半带宽，
但 pack / upload / shader 三处必须同时改）。

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
> - §8.4 的量化偏置不对称（#9）与 normal/UV 未打包（#13）需在 Step 3 内解决，否则 Task 8 的
>   `decodeVertexPosition/Normal/UV` 无法与编码对上。
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

```python
# Tools/NanitePreprocess/NanitePack.py

import struct

def pack_nanite(output_path: str, header: dict, clusters: list,
                vertices: np.ndarray, indices: np.ndarray,
                materials: list, lod_offsets: list):
    """
    打包 .nanite 二进制文件:
        [NaniteFileHeader 128B]
        [NaniteCluster[]      (clusterCount × 64B)]
        [quantized vertices[] (vertexCount × 12B)]
        [indices[]            (indexCount × 4B)]
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
        f.write(struct.pack('<32x'))  # reserved[8]

        # Clusters (64B each)
        for c in clusters:
            f.write(struct.pack('<4f', *c['bounds_center'], c['bounds_radius']))
            f.write(struct.pack('<4f', *c['cone_axis'], c['cone_cutoff']))
            f.write(struct.pack('<4I', c['triangle_offset'], c['triangle_count'],
                                c['vertex_offset'], c['material_id']))
            f.write(struct.pack('<f', c['max_parent_lod_error']))
            f.write(struct.pack('<2I', c['child_cluster_offset'], c['child_count']))
            f.write(struct.pack('<I', 0))  # _pad

        # Quantized vertices (12B each: position(4B) + normal(4B) + uv(4B))
        for v in vertices:
            f.write(struct.pack('<I', v['packed_position']))
            f.write(struct.pack('<I', v['packed_normal']))
            f.write(struct.pack('<I', v['packed_uv']))

        # Indices (4B each)
        f.write(indices.astype('<u4').tobytes())

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

> 落地提示（本文件补充，非源文档正文）：docstring 里的 `[NaniteFileHeader 128B]` 与实际
> 96B（§8.3 / #8）以及"顶点 12B vs `NaniteVertex` 16B"（#7）、"indexCount × 4B vs 3×u16 打包"
> （#6）需要在 Step 1 内统一；本文件 Task 5 的 `NaniteUpload.cpp` 读取口径（按 12B/顶点、
> 4B/索引、96B 头）与 docstring 的 128B 说法不一致。

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
