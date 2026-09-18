# HugEngine 架构可扩展性分析

> 分析日期：2026-08-04
> 范围：Engine 全模块（Core/Reflect/Serialize/RHI/Shader/Scene/Asset/Render/Editor）+ Samples，聚焦**可扩展性**
> 依据：全库 include 依赖扫描、各模块 CMake 链接关系、渲染管线/资源/序列化代码通读

---

## 总体结论

**架构骨架是健康的**——分层单向无环、接口化程度高、无 god class，这个规模的引擎该有的抽象大多都有；**但存在两个结构性硬伤**（Vulkan 泄漏到业务层、场景组件与 GPU 资源职责混叠）和一批"扩展成本随类型数量线性增长"的样板代码点，且反射驱动的自动化只完成了一半。

---

## 一、模块分层与依赖

### 1.1 分层图（CMake 链接级）

```
Core (L0 平台/工具)
  ← Reflect (L1) / Serialize (L1)
  ← RHI (L2, 抽象接口 + Vulkan 实现)
    ← Shader (L3, INTERFACE 库)
      ← Scene (L5 组件层)  ← Asset (L4)
        ← Render (L4)      ← Editor (L8) ← Samples
```

- 各模块链接关系见各子目录 CMakeLists.txt（Core 仅外部库；Reflect→Core；Serialize→Reflect；RHI→Core；Shader(INTERFACE)→RHI；Scene→Reflect+Core+RHI；Asset→Scene+Reflect；Render→RHI+Shader+Scene+Asset；Editor→Render+Serialize+imgui）
- 全量 include 扫描确认：**无循环依赖**，Render 是引擎内依赖顶端，纯消费者

### 1.2 各层职责评价

| 模块 | 职责 | 评价 |
|---|---|---|
| Core (L0) | 窗口/JobSystem/CVar/日志/内存 | 最干净的一层，无反向依赖 |
| Reflect (L1) | 宏驱动反射（HE_CLASS → TypeRegistry） | 小巧、依赖面单一 |
| Serialize (L1) | Archive/BinaryArchive 基于反射的序列化 | 仅依赖 ReflectionAPI，解耦良好 |
| RHI (L2) | IRHIDevice 纯虚接口 + Vulkan 实现 + 瞬态资源 | 抽象设计良好，但有 void* 逃逸口（见 三.1） |
| Shader (L3) | slangc→SPIR-V→内嵌 .spv.h 编译管线 | C++/Slang 共享常量设计巧妙 |
| Scene (L5) | 自制组件系统：World + Entity + Component + SceneGraph | 职责明确，但组件直接持有 GPU 资源（见 三.2） |
| Asset (L4) | glTF 加载 + BindlessTextureManager | 加载与 ECS 强耦合，无加载器注册表 |
| Render (L4) | 5 条管线 + IRenderSubsystem 子系统 + RenderGraph | 架构意图最好的一层 |
| Editor (L8) | EditorContext/ImGui 集成/SceneSerializer | 模块边界存在，但有 Vulkan 泄漏（见 三.1） |

### 1.3 全局单例模式

- `rhi::g_Device`（RHIBase.cpp:7）：裸全局指针，最危险——跨层可见、生命周期无主
- 其余单例（JobSystem/Logger/TypeRegistry/BindlessTextureManager/Allocator）均为标准成对或 Meyers 单例，合理

### 1.4 通信方式

以直接函数调用为主，配以良好的接口层（`IRenderSubsystem`/`IGlobalIllumination`/`IShadowSystem`/`IRenderPipeline`/`IRHIDevice`）+ 少量回调（窗口 resize、选中变化、shader 热重载）。**没有真正的中枢 god class**——真正的全局中枢是 `rhi::GetDevice()` 而非某个类。

---

## 二、扩展路径评估：加"新 X"要改几处

| 扩展路径 | 改动量 | 评价 |
|---|---|---|
| 新独立 Pass | 4-6 处（RG AddPass lambda） | 好，RG 自动推导依赖/Barrier/别名 |
| 新渲染管线（如全 PT 生产管线） | 5-6 处，全在应用层 | 差：模式切换是每个 Sample 里复制 if/else，引擎无管线注册表 |
| 新光源类型（如面光源） | **8-12 处** | **最痛**：CollectLights 序列化 switch 复制 4 份 + GPU 侧魔数编码 |
| 新材质模式 | 4-6 处 | 标志位方案，shader 分支随标志位增多 |
| 新阴影技术 | 2-3 处 | 最好，管线端阴影索引魔数化是隐患 |
| 新后端（D3D12/Metal） | **受阻** | void* 泄漏 + 描述符模型 Vulkan 化 |
| 新组件类型（仅运行时） | 4 处 ≈30 行 | 好，World::AddComponent/ForEach 天然支持 |
| 新组件类型（全链路：渲染+存盘+编辑器） | **8-12 处硬编码点** | 差：Render 模块 8 处逐类型 ForEach，漏改一处即静默不渲染 |
| 新资源格式 | 单个成本低，规模化路径断 | 无加载器注册表、无引擎级缓存、全同步 |
| 新 CVar | 1-2 处 | 好（RTQualityCVars/PTQualityCVars 集中声明） |

### 2.1 值得保留的好模式

1. **RenderGraph**（RenderGraph.h）：pass 只声明 reads/writes，Barrier/别名/裁剪/异步计算调度自动，瞬态资源别名复用
2. **IRenderSubsystem 子系统框架**（Subsystem/RenderSubsystem.h）：Shadow/GI/AA/后处理均实现同一生命周期接口，管线可注入替换（已实现 6 种 GI），配 Null Object（GI_None/ShadowNone）
3. **IShadowTechnique + ShadowSystem 组合器**：新阴影技术 = 新类 + 注册一行
4. **RTEffectPass 基类**：RTShadow/AO/Reflection/GI 共享管线创建/SBT/屏障逻辑，子类只实现 Execute
5. **每 Pass 每帧上下文结构体**（RTExecuteContext/PTRenderContext/ReSTIRDispatchContext）：管线与 Pass 显式数据契约
6. **ShaderTypes.slang 单一数据源 + static_assert**：C++/Slang 共享 GPU 布局，尺寸不符编译期报错
7. **RHI 集中式生命周期**：DeferredDestructionQueue 三槽轮转 + PSO 哈希缓存（历史 5 次 crash 修复记录）
8. **反射 + 序列化驱动编辑器属性面板**（骨架）

---

## 三、关键问题清单（按优先级）

### P0-1：后端抽象纪律失守 —— Vulkan 泄漏到 Render/Editor

- `Engine/Render/GI/GI_RSM.cpp:5` 直接 `#include "Vulkan/VulkanResources.h"`（还是死依赖）
- `Engine/Editor/Editor/ImGuiIntegration.cpp:14` include `Vulkan/VulkanDevice.h`，用 `static_cast<VulkanDevice*>` 下行转换拿 `VkInstance`（VulkanDevice.cpp:1129）；Editor 的 CMake 为此专门暴露后端头路径（Editor/CMakeLists.txt:23）
- RHI 接口自身有 `void*` 逃逸口：`GetNativeHandle()` 返回 VkImageView（Buffer.h:59）、`BeginOffscreenPass(void*...)`（CommandList.h:62）、`UpdateDescriptorSetWithImageView`、`GetBackendFormat()` 返回 VkFormat 数值；Types.h:52 注释自认 StageMask 常量"当前映射 Vulkan VkShaderStageFlagBits"
- **事实修正**：当前后端是 **Vulkan**（D3D12 代码已在 commit `5c62523` 删除），`Backend` 枚举里的 D3D12/Metal/WebGPU 只是占位
- **后果**：加 D3D12/Metal 后端时，GI_RSM 和 ImGuiIntegration 直接编译失败；`BeginOffscreenPass` 等泄漏点需逐处后端化

### P0-2：Scene 组件层反向依赖 RHI —— 数据层被 GPU 类型污染

- `MeshComponent.h:4,35` 直接持有 `unique_ptr<rhi::IRHIBuffer>`，Skybox/Particle 组件同样；Scene/CMakeLists.txt:43 链接 RHI
- **后果**：ECS 无法脱离 GPU 复用（服务器模拟、离线烘焙、headless 测试全不可行）；组件生命周期与 GPU 资源纠缠
- 正确做法：组件存 CPU 数据 + 句柄，GPU 缓冲归 Render/Asset 层持有

### P0-3：序列化链路"半残" —— 反射自动化只完成 40%

- `HE_REGISTER_PROPERTY` 宏**定义但零使用**（ReflectionMacros.h:51）→ SceneSerializer::Save 遍历属性为空 → **.hescene 存盘丢光所有属性值**，Load 出来全是默认值
- `ClassInfo::factory`（ReflectionAPI.h:66，宏自动生成）建好了没人用——反序列化靠 SceneSerializer.cpp:125 / LevelLoader.cpp:41 两份名字 if-else
- 类型分派表复制了 3 份（Archive.h:72 / SceneSerializer.cpp:25 / LevelLoader.cpp:26），加一个 `u8` 属性类型要改 3 处
- 好消息：基础设施齐全，接线成本低

### P1：全局设备指针 + 应用层样板复制

- `rhi::GetDevice()` 裸全局（RHIBase.cpp:7），Scene/Render 到处取，多设备/多上下文/单测不可行
- 渲染模式切换链（02.Cube.cpp:450-491）在 4 个 Sample 里各复制一份，`r.Pipeline.Mode` 定义在应用层而非引擎；管线注册表/工厂缺失
- Sample 层 ~50% 样板重复：配置解析、相机主循环、skybox、stb_image 纹理加载各复制一份；01.Triangle 还链接了 Editor（复制粘贴事故）
- Core 缺口：无输入/时间/文件系统封装，glfwGetKey 直接散落在 Sample

### P2：Render 层细节债

- `LightingPass::Render` **20+ 裸指针参数**（LightingPass.h:69-97）——"新效果→新参数"扩散点
- 三缓冲 SSBO 在 4 条管线各声明一份（ForwardPipeline.h:125 / DeferredPipeline.h:134 / HybridRTPipeline.h:164 / PathTracingPipeline.h:91），创建代码同构复制；SSBO 完全绕过 RenderGraph（依赖单一队列提交序，ReSTIRPass.h:43 自认）
- 魔数：阴影索引硬编码 `GetShadowMap(4)`（DeferredPipeline_FrameGraph.cpp:157）、物理模式用 `positionRange.w < 0` hack、`static bool firstFrame` 函数级静态变量（DeferredPipeline_FrameGraph.cpp:50）
- 两个裸 `static int32_t` 未走 CVar（DeferredPipeline_FrameGraph.cpp:24-27）；RT 着色器（.rgen/.rchit）不触发热重载（ShaderHotReload.cpp:67-80 只认 vert/frag/comp）

---

## 四、改进建议（按投入产出排序）

1. **① 先还 P0-2**：把 MeshComponent 的 GPU 缓冲上移到 Render 侧资源管理器，组件只留路径/句柄——收益最大，同时消除"8 处逐类型 ForEach"的根因之一（组件实现统一 Renderable 接口后枚举自然收敛）
2. **② 封住后端泄漏**：GI_RSM 的下沉调用补进 RHI 接口；ImGuiIntegration 全走 RHI 已有的 CreateImGui* 虚函数；void* 接口逐个换强类型抽象——这是未来 D3D12 的必经之路
3. **③ 打通序列化**（成本最低、见效快）：SceneSerializer::Load 改用 ClassInfo::factory 替代 if-else；在组件上启用 HE_REGISTER_PROPERTY，3 份类型分派表收敛到 Archive::SerializeObject 一处——完成后"新组件全链路"从 8-12 处降到 5 处
4. **④ 收编应用层样板**：引擎内加 PipelineRegistry（注册表 + 工厂，替代 Sample 里的模式 if/else）和 AppBase（设备/交换链/主循环/配置），4 个 Sample 去重，新增 Sample 成本从"复制 700 行"降到"继承一个基类"
5. **⑤ 抽公共渲染组件**：4 份 CollectLights switch 抽成单一 LightSerializer；三缓冲 SSBO 封装成可复用类；LightingPass 参数收敛为上下文结构体

---

## 一句话总结

> 分层正确、接口有章法、但自动化不彻底——**加新渲染特性（Pass/阴影/着色器）的路径已经打通且成本可控**（历史已验证 4 次），主要债务集中在三处：后端抽象纪律（Vulkan 泄漏）、组件与 GPU 资源耦合、反射序列化半成品。这三处不还，D3D12 后端、多设备、完整编辑器存档三条扩展路径都会被卡住。
