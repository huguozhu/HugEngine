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

## Phase 2：03.Sponza 几何体 RT 渲染 ⬜ 待实施

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

## 执行状态

```
Phase 1 ✅ 已完成并验证 (提交 93225b5)
    ↓
Phase 2 ⬜ 待实施 (2.1→2.2→2.3→2.4)
```
