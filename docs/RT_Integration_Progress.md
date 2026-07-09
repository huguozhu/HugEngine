# Ray Tracing 接入渲染管线 — 实施进度

## Phase 1：01.Triangle RT 过程化三角形 ✅ 已完成

**目标**：在 `01.Triangle` 中用 RayGen 着色器直接渲染到 BackBuffer，RT 全链路验证通过。

**提交**：`93225b5` Phase 1: 01.Triangle RT 过程化三角形 — RayGen 直接渲染到 BackBuffer

### Phase 1 已完成工作

| 文件 | 操作 | 说明 |
|------|:---:|------|
| `Engine/Shader/Shaders/RT_Triangle.rgen.slang` | 新建 | RayGen 过程化红色三角形 + 深蓝背景 |
| `Engine/Shader/Shaders/RT_Background.rmiss.slang` | 新建 | 占位 Miss 着色器 |
| `Engine/Shader/Shaders/Fullscreen.vert.slang` | 新建 | 全屏三角形（SV_VertexID，备用） |
| `Engine/Shader/Shaders/FullscreenCopy.frag.slang` | 新建 | 纹理拷贝片元着色器（备用） |
| `Engine/Shader/CMakeLists.txt` | 修改 | 注册 RT + Fullscreen 着色器 |
| `Engine/RHI/Public/RHI/Types.h` | 修改 | `stageMask` u8→u32；新增 `PipelineStage::RayTracingShader` / `AccelerationStructureBuild` |
| `Engine/RHI/Public/RHI/RHI.h` | 修改 | 新增 `UpdateDescriptorSetWithImageView` 虚方法 |
| `Engine/RHI/Vulkan/VulkanInternal.h` | 修改 | VulkanDevice 声明新方法 |
| `Engine/RHI/Vulkan/VulkanDevice.cpp` | 修改 | 实现 `UpdateDescriptorSetWithImageView`（直接绑定 VkImageView 为 StorageImage） |
| `Engine/RHI/Vulkan/VulkanCommandList.cpp` | 修改 | ToVkPipelineStageFlags 添加 RT 阶段映射 |
| `Engine/RHI/Vulkan/VulkanSwapChain.cpp` | 修改 | `imageUsage` 添加 `STORAGE_BIT | TRANSFER_DST_BIT` |
| `Samples/01.Triangle/01.Triangle.cpp` | 修改 | RT 路径 + ImGui 切换 |
| `Samples/01.Triangle/CMakeLists.txt` | 修改 | 链接 `HugEngineEditor` |

### RT 模式渲染流程（最终版）

```
AcquireNextImage
  → UpdateDescriptorSetWithImageView(BackBuffer as StorageImage)
  → PipelineBarrier (BottomOfPipe → RayTracingShader)
  → BindRTPipeline + BindDescriptorSet + TraceRays (直写 BackBuffer)
  → PipelineBarrier (RayTracingShader → ColorAttachmentOutput)
  → SetPipeline (设置 m_CurrentRenderPass)
  → BeginRenderPass(Load) → ImGui → EndRenderPass
  → Present
```

### Phase 1 踩坑记录

1. **每帧创建/销毁纹理** → 崩溃（VkImage 在命令缓冲中引用后被销毁）。修复：持久化纹理。
2. **RT 模式未调用 SetPipeline** → `m_CurrentRenderPass` 为空 → `BeginRenderPass` 失败。修复：RT 模式也调用 `SetPipeline`。
3. **缺少 post-barrier** → 全屏 Pass 采样时纹理布局不正确 → 黑屏。修复：添加 `UAV → ShaderResource` 屏障。
4. **PipelineStage 用 ComputeShader 代替 RT** → 屏障不等待 RT 工作完成。修复：添加 `RayTracingShader` 阶段。
5. **SwapChain 缺少 STORAGE_BIT** → BackBuffer 不能作为 StorageImage。修复：`imageUsage` 添加 `STORAGE_BIT`。
6. **最终方案**：移除全屏拷贝 Pass，RT 直接写 BackBuffer。架构最简。

---

## Phase 2：03.Sponza 几何体 RT 渲染 ✅ 已完成

**目标**：加载 Sponza glTF 资源，用 RT 渲染（创建 BLAS/TLAS + TraceRays 击中三角面片）。

### Phase 2 步骤

#### 2.1 添加 AccelerationStructure 描述符类型 + UpdateDescriptorSet

**修改：** `Engine/RHI/Public/RHI/Types.h`
- `DescriptorType` 枚举添加 `AccelerationStructure`
- `stageMask` 已在 Phase 1 改为 u32 ✅

**修改：** `Engine/RHI/Public/RHI/RHI.h`
```cpp
virtual void UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                  IRHIAccelerationStructure* as) = 0;
```

**修改：** `Engine/RHI/Vulkan/VulkanInternal.h` + `VulkanDevice.cpp`
- 声明 + 实现 override
- `ToVkDescType` 映射 `AccelerationStructure` → `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`
- `EnsureDescriptorPool` 添加 AS pool size
- 实现：用 `VkWriteDescriptorSetAccelerationStructureKHR` pNext 链

#### 2.2 RT 着色器 — 几何体 Hit/Miss

**新建：**
- `RT_Sponza.rgen.slang`：从相机发射光线 → 输出颜色到 BackBuffer
- `RT_Sponza.rmiss.slang`：天空颜色
- `RT_Sponza.rchit.slang`：采样命中点材质

着色器绑定：
| Binding | 类型 | 用途 |
|---------|------|------|
| 0 | AccelerationStructure | TLAS |
| 1 | StorageImage | 输出颜色 (BackBuffer) |
| 2 | UniformBuffer | 相机参数 (invViewProj + pos) |

#### 2.3 RTPass 增强 — 支持描述符集 + 推送常量

**修改：** `Engine/Render/Pipeline/RTPass.h` + `.cpp`
- `Initialize()` 接受 `DescriptorSetLayoutHandle` + `PushConstantRange`
- 新增 `UpdateRTDescriptorSet()` 方法
- BuildAS 已实现（BLAS/TLAS 构建）

#### 2.4 03.Sponza 示例集成

**修改：** `Samples/03.Sponza/03.Sponza.cpp`
- ImGui 切换光栅化/RT 模式
- RT 模式：BuildAS → Bind BackBuffer → TraceRays → Present
- 光栅化模式：现有 ForwardPipeline

---

### Phase 2 踩坑记录

1. **`SetPushConstants` 未处理 RT 绑定点** → push constant 发送到 Vertex/Fragment 而非 RayGen → 黑屏。修复：添加 `VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR` 分支，使用全部 RT stage flags。
2. **顶点/索引缓冲缺 `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT`** → BLAS 构建静默失败 → 全 Miss。修复：`ToVkBufferUsage` 添加 `AccelerationStruct` 映射，`MeshComponent::SetMeshData` 添加该用法标志。
3. **`GetTLASBuildSizes` 中 `pGeometries=nullptr` 但 `geometryCount=1`** → 违反 VUID → 崩溃。修复：提供 dummy `VK_GEOMETRY_TYPE_INSTANCES_KHR` 几何体。
4. **`StructuredBuffer` / `ByteAddressBuffer` 在 ClosestHit shader 中崩溃** → slangc SPIR-V 兼容性问题（GPU fault）。Phase 3 修复。当前方案：InstanceID 伪随机色 + `-WorldRayDirection()` 法线近似。

### Phase 2 已知限制

| 限制 | 原因 | 计划 |
|------|------|------|
| 无法读取 GPUObjectData（材质色） | SSBO 在 ClosestHit 中不兼容 | ✅ Phase 3 用 Uniform Buffer 修复 |
| 法线近似（`-WorldRayDirection`） | 无顶点法线数据 | Phase 3 引入顶点缓冲（依赖 SSBO 修复） |
| InstanceID 与 ObjectData 索引不匹配 | 视锥剔除等差异 | 当前 Sponza 全可见时无影响 |

---

## Phase 3：RT 材质 + 多光源 ✅ 已完成

**目标**：RT ClosestHit 读取真实材质色 + 引擎光源数据。

**提交**：`b4b19be` Phase 3: RT 材质色 + 多光源 — UB/纹理方案

### Phase 3 技术突破

#### 3.1 SSBO 替代方案
- **问题**：`StructuredBuffer`/`ByteAddressBuffer` 在 RT ClosestHit shader 中导致 GPU fault（slangc SPIR-V 兼容性）。
- **方案 A（光源）**：使用 `cbuffer` (Uniform Buffer) — 在 ClosestHit 中兼容。
- **方案 B（材质）**：使用 `Texture2D::Load()` — 天然支持非均匀动态索引，无需 sampler。

#### 3.2 cbuffer 数组非均匀索引
- **问题**：cbuffer 数组用 InstanceID 动态索引时 GPU fault。
- **根因**：slangc 未生成 `NonUniform` SPIR-V 装饰词。
- **修复**：`VkPhysicalDeviceDescriptorIndexingFeatures::shaderUniformBufferArrayNonUniformIndexing = VK_TRUE`。
- **最终选择**：纹理方案更稳定，避免 `NonUniform` 依赖。

#### 3.3 数据流
```
MeshComponent.baseColorFactor → Texture2D(1×256 RGBA32F) → ClosestHit::Load(id)
GPULight SSBO → Uniform Buffer(cbuffer) → ClosestHit::u_Lights[li]
ForwardPipeline → GetCurrentLightBuffer() → RTPass::UpdateLightBuffer
```

### Phase 3 踩坑记录

1. **cbuffer 内存布局不匹配** → std140 顺序数组 ≠ C++ 交错写入。修复：改为 struct 交错布局。
2. **cbuffer 数组动态索引 GPU fault** → 需要 `NonUniform` 装饰。修复：改用 `Texture2D::Load()`。
3. **光源强度缩放** → 引擎 intensity ~2-5，需 `*0.15` 映射到合理范围。
4. **Vulkan12Features struct 不存在** → `shaderUniformBufferArrayNonUniformIndexing` 在 `DescriptorIndexingFeatures` 中。

### Phase 3 已知限制

| 限制 | 原因 | 方案 |
|------|------|------|
| 无纹理采样 | `SampledImage` + sampler 需扩展 | Phase 4 bindless 纹理 |
| 法线近似（`-WorldRayDirection`） | SSBO 不可用 | slangc 更新或 position_fetch |
| 无阴影射线 | SSBO 不可用（GPUShadowData[]） | 同上 |

---

## 执行状态

```
Phase 1 ✅ 已完成并验证
    ↓
Phase 2 ✅ 已完成（含 02.Cube RT 模式）
    ↓
Phase 3 ✅ 已完成（Texture2D 材质 + UB 光源）
    ↓
Phase 4 ⬜ 待规划（纹理采样 + 逐顶点法线 + 阴影）
```
