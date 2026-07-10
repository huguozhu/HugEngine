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

**提交**：`138ac62` Phase 2: Sponza 几何体 RT 渲染 — BLAS/TLAS + TraceRays

### Phase 2 已修改文件

| 文件 | 操作 | 说明 |
|------|:---:|------|
| `Engine/RHI/Public/RHI/Types.h` | 修改 | `DescriptorType::AccelerationStructure` |
| `Engine/RHI/Public/RHI/RHI.h` | 修改 | `UpdateDescriptorSet(AS*)` 虚方法 |
| `Engine/RHI/Vulkan/VulkanInternal.h` | 修改 | VulkanDevice 声明 |
| `Engine/RHI/Vulkan/VulkanDevice.cpp` | 修改 | ToVkDescType/EnsureDescriptorPool/UpdateDescriptorSet(AS*)/GetTLASBuildSizes |
| `Engine/RHI/Vulkan/VulkanCommandList.cpp` | 修改 | `SetPushConstants` RT 绑定点修复 |
| `Engine/RHI/Vulkan/VulkanResources.cpp` | 修改 | `ToVkBufferUsage` 添加 AS input flag |
| `Engine/Scene/Private/MeshComponent.cpp` | 修改 | 缓冲添加 `AccelerationStruct` 用法 |
| `Engine/Render/Pipeline/RTPass.h` | 修改 | 双描述符集 + BindDescriptorSets |
| `Engine/Render/Pipeline/RTPass.cpp` | 修改 | 描述符管理 + 材质数据 |
| `Engine/Shader/Shaders/RT_Sponza.rgen.slang` | 新建 | 相机光线生成 |
| `Engine/Shader/Shaders/RT_Sponza.rmiss.slang` | 新建 | 天空渐变 |
| `Engine/Shader/Shaders/RT_Sponza.rchit.slang` | 新建 | 命中着色 |
| `Engine/Shader/CMakeLists.txt` | 修改 | 注册 6 个 RT 着色器 |
| `Samples/03.Sponza/03.Sponza.cpp` | 修改 | RT 模式集成 |
| `Samples/02.Cube/02.Cube.cpp` | 修改 | RT 模式集成 |

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

---

## Phase 4：逐顶点法线 + Bindless 纹理 + 阴影射线

**目标**：在 ClosestHit shader 中实现逐顶点法线（重心插值）、bindless 纹理采样、以及阴影射线（二次光线）。

### 4.0 前置条件：修复 slangc SSBO NonUniform 兼容性

#### 问题诊断（2026-07-10）

| 项目 | 详情 |
|------|------|
| slangc 版本 | `2026.1-52-gc8ddf20bb` (VulkanSDK 1.4.341.1) |
| GPU | Intel Arc B370 (Battlemage), driver 101.8509 |
| `shaderStorageBufferArrayNonUniformIndexingNative` | **false** |
| `shaderUniformBufferArrayNonUniformIndexingNative` | **false** |
| `VK_KHR_ray_tracing_position_fetch` | **不支持** |

**根因**：slangc 的 `-emit-spirv-directly`（默认）路径在 ClosestHit shader 中，**不为从非均匀来源（InstanceID/PrimitiveIndex）派生的索引生成 `NonUniform` SPIR-V 装饰**。即使显式使用 `NonUniformResourceIndex()` 也无法生成。

在 Intel Arc B370 上，`shaderStorageBufferArrayNonUniformIndexingNative = false` 意味着硬件不原生支持发散索引访问。缺少 `NonUniform` 装饰时，驱动/硬件假定索引在子组内一致，但 RT shader 中 `InstanceID()`/`PrimitiveIndex()` 天然发散 → **GPU fault**。

**解决方案**：切换到 `-emit-spirv-via-glsl` 编译路径（glslang v11:16.2.0 已随 VulkanSDK 附送）

| 对比项 | `-emit-spirv-directly` | `-emit-spirv-via-glsl` |
|--------|----------------------|----------------------|
| `NonUniform` 装饰生成 | ❌ 不生成 | ✅ 正确生成 (6/6) |
| `OpCapability ShaderNonUniform` | ❌ 缺失 | ✅ 包含 |
| `SPV_EXT_descriptor_indexing` | ❌ 缺失 | ✅ 包含 |
| SPIR-V 版本 | 1.5 | 1.4 |
| glslangValidator 依赖 | 不需要 | 需要 (已集成在 VulkanSDK) |

**验证命令**：
```bash
slangc shader.rchit.slang -target spirv -entry main -stage closesthit \
       -emit-spirv-via-glsl -o shader.spv
spirv-dis shader.spv | grep NonUniform  # 应输出多行 NonUniform 装饰
```

#### CMake 编译变更

`Engine/Shader/CMakeLists.txt` 中 RT 着色器编译需要添加 `-emit-spirv-via-glsl`：

```cmake
# RT 着色器：使用 via-glsl 路径确保 NonUniform 装饰正确生成
COMMAND ${SLANGC} ${SRC} -target spirv -entry main -stage ${SLANGC_STAGE}
        -emit-spirv-via-glsl -I "${SHADER_DIR}" -o ${SPV} -Wno-39001
```

---

### 4.1 逐顶点法线（顶点拉取 + 重心插值）

**目标**：在 ClosestHit 中从 BLAS 绑定的顶点/索引缓冲读取顶点数据，计算重心插值后的逐顶点法线。

#### 4.1.1 ClosestHit shader 新增 binding

```hlsl
// set=1 — 新增顶点拉取 binding
[[vk::binding(2, 1)]] StructuredBuffer<GPUVertex> u_Vertices;  // 每顶点: position + normal + uv
[[vk::binding(3, 1)]] ByteAddressBuffer u_Indices;              // 索引缓冲 (uint32)
```

#### 4.1.2 顶点格式（与 C++ StaticVertex 保持一致）

```hlsl
// std430 布局: float3(12) + float3(12) + float2(8) = 32 字节，16 字节对齐
struct GPUVertex {
    float3 position;  // offset 0,  size 12
    float3 normal;    // offset 16, size 12 (因 float3 需对齐到 16)
    float2 uv;        // offset 32, size 8
};
```

> ⚚ **注意**：C++ 端 `StaticVertex` 字节布局与 std430 不同（C++ offset: position=0, normal=12, uv=24）。需要验证或使用 `-force-glsl-scalar-layout`。最简单的方案是在 RTPass 上传时显式按 std430 布局打包。

#### 4.1.3 重心插值

Vulkan RT 不直接提供命中点重心坐标。当前方案使用 `PrimitiveIndex()` + 重心近似：

```hlsl
[shader("closesthit")]
void main(inout RayPayload payload) {
    uint primId = NonUniformResourceIndex(PrimitiveIndex());
    
    // 读取三角形 3 个顶点索引
    uint baseOff = primId * 3;
    uint i0 = u_Indices.Load<int>(baseOff * 4);
    uint i1 = u_Indices.Load<int>((baseOff + 1) * 4);
    uint i2 = u_Indices.Load<int>((baseOff + 2) * 4);
    
    // 读取顶点数据
    GPUVertex v0 = u_Vertices[NonUniformResourceIndex(i0)];
    GPUVertex v1 = u_Vertices[NonUniformResourceIndex(i1)];
    GPUVertex v2 = u_Vertices[NonUniformResourceIndex(i2)];
    
    // 命中点位置（由 RayOrigin + RayDirection * HitT 计算）
    float3 P = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    
    // 基于面积的近似重心插值（三角形 3 个子三角形面积比）
    float3 e0 = v2.position - v1.position;
    float3 e1 = v0.position - v2.position;
    float3 e2 = v1.position - v0.position;
    float3 d0 = P - v0.position;
    float3 d1 = P - v1.position;
    float3 d2 = P - v2.position;
    float area = length(cross(e0, e1));
    float w0 = length(cross(d1, d2)) / area;  // v0 权重
    float w1 = length(cross(d2, d0)) / area;  // v1 权重
    float w2 = 1.0 - w0 - w1;                  // v2 权重
    
    // 插值法线
    float3 N = normalize(v0.normal * w0 + v1.normal * w1 + v2.normal * w2);
}
```

> ⚚ **备选方案（更稳定）**：使用 `HitTriangleVertexPosition()` 通过 `VK_KHR_ray_tracing_position_fetch` 获取顶点位置。但 Intel Arc B370 不支持此扩展，故采用顶点拉取方案。

#### 4.1.4 C++ 端：创建顶点/索引 SSBO 描述符集

在 `RTPass` 中新增方法：

```cpp
// 为指定 MeshComponent 的 BLAS 创建顶点/索引描述符绑定
bool CreateVertexDataBuffers(rhi::IRHIDevice* device, he::MeshComponent* mesh);

// 更新 set=1 的 b=2 (Vertices) 和 b=3 (Indices)
void UpdateVertexDescriptorSet(rhi::IRHIDevice* device);
```

> ⚚ **约束**：当前 set=1 中 binding 0/1 已用于材质纹理和光源 UB。binding 2/3 不能与其他 binding 冲突。`VkDescriptorPool` 需要包含 `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`。

#### 4.1.5 RHI 层变更

| 文件 | 变更 |
|------|------|
| `RHI/Types.h` | `DescriptorType::StorageBuffer` 枚举（如未定义） |
| `RHI/RHI.h` | `UpdateDescriptorSet(set, binding, StorageBuffer, buffer*)` |
| `VulkanDevice.cpp` | `ToVkDescType` 映射 `StorageBuffer → VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` |
| | `EnsureDescriptorPool` 添加 StorageBuffer pool size |

---

### 4.2 Bindless 纹理采样

**目标**：在 ClosestHit 中通过 bindless 纹理数组采样真实纹理（base color / normal / metallic-roughness 贴图等）。

#### 4.2.1 架构方案

当前 Phase 3 使用 `Texture2D::Load(id,0)` 读取材质色（无过滤）。Phase 4 升级为：

```
set=2 (bindless): 纹理数组 → SampledImage[] + Sampler
set=1 (rchit):    materialID + UV (来自顶点插值)
```

**shader 端**：
```hlsl
[[vk::binding(0, 2)]] Texture2D  u_Textures[];   // bindless 纹理数组
[[vk::binding(1, 2)]] SamplerState u_Sampler;    // 共享采样器
```

**ClosestHit**：
```hlsl
uint texId = u_MatData[materialID].baseColorTex;  // 非均匀索引
float4 baseColor = u_Textures[NonUniformResourceIndex(texId)].Sample(u_Sampler, uv);
```

#### 4.2.2 数据流

```
AssetLoader → 加载纹理 → IRHITexture (per-texture)
                  ↓
ForwardPipeline → BindlessManager → 注册/更新纹理数组
                  ↓
RTPass → 从 BindlessManager 获取纹理数组 → UpdateDescriptorSet(set=2)
                  ↓
ClosestHit → u_Textures[NonUniformResourceIndex(id)].Sample(sampler, uv)
```

#### 4.2.3 描述符索引特性（已启用）

```cpp
// VulkanDevice.cpp 中已配置：
descIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
descIndexing.runtimeDescriptorArray = VK_TRUE;
descIndexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
```

> ⚚ **注意**：Intel Arc B370 `shaderSampledImageArrayNonUniformIndexingNative = false`，非均匀纹理采样为软件模拟，可能有一定性能开销。但对于 RT（非性能首要目标）可以接受。

#### 4.2.4 实施步骤

1. 创建 `BindlessTextureManager` 类（或扩展现有纹理管理器）
2. 在 `RTPass` 中添加 set=2 描述符集布局（纹理数组 + 采样器）
3. 在 `MaterialData` 中添加 bindless 纹理索引字段
4. 更新 `RT_Sponza.rchit.slang` → `RT_Common.rchit.slang` 支持纹理采样

---

### 4.3 阴影射线

**目标**：在 ClosestHit 中为每个光源发射阴影射线（Shadow Ray），实现 RT 硬阴影。

#### 4.3.1 阴影 RayGen + Miss + 复用 rchit

当前已有占位 shader：
- `RT_Shadow.rgen.slang` — 占位 RayGen
- `RT_Common.rmiss.slang` — Miss 返回可见（payload=1）
- `RT_Common.rchit.slang` — Hit 返回遮挡（payload=0，命中即被遮挡）

**更新方案**：阴影射线直接从主 RT 的 ClosestHit 中发射（递归深度 1）：

```
Main RayGen → TraceRay(primary) → ClosestHit
  ├─ 计算光照
  └─ for each light:
       TraceRay(shadow ray, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH) → Miss/Hit
       └─ 如果 Miss: light 可见，累加光照
       └─ 如果 Hit: 被遮挡，跳过此光源
```

#### 4.3.2 着色器组配置

```
Group 0: RayGen      (RT_Sponza.rgen)
Group 1: Miss        (RT_Sponza.rmiss) → 天空
Group 2: ClosestHit  (RT_Common.rchit) → 主命中 + 阴影发射
Group 3: Miss        (RT_Common.rmiss) → 阴影 Miss (可见)
Group 4: ClosestHit  (RT_Common.rchit) → 阴影 Hit (遮挡，仅返回 payload=0)
```

#### 4.3.3 主 rchit shader 中的阴影逻辑

```hlsl
// 在 ClosestHit 主着色器中
float3 visibility = 1.0;
for (uint li = 0; li < u_LightCount; li++) {
    float3 L = u_Lights[li].dt.xyz;
    float dist = u_Lights[li].dt.w;  // 光源距离或 1e6
    
    RayDesc shadowRay;
    shadowRay.Origin    = P + N * 0.01;           // 偏移避免自交
    shadowRay.Direction = L;
    shadowRay.TMin      = 0.001;
    shadowRay.TMax      = dist;
    
    uint shadowPayload = 1;  // 1=可见, 0=遮挡
    TraceRay(g_TLAS, 
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFF, 
             1,  // miss index = 1 (shadow miss group)
             0,  // hit group index = 1 (shadow hit group)
             0,  // SBT offset
             shadowRay, shadowPayload);
    
    float NdotL = max(dot(N, L), 0.0);
    total += u_Lights[li].ci.rgb * u_Lights[li].ci.w * NdotL * shadowPayload;
}
```

或者更简单：使用 `RAY_FLAG_SKIP_CLOSEST_HIT_SHADER` 让命中自动终止（payload 由系统预设为 0），Miss shader 设置 payload=1。只需一个 Miss 组，无需 Shadow Hit 组。

#### 4.3.4 SBT 偏移管理

```
SBT:
[stride=32B] RayGen     (handleOffset=0)          → Group 0
[stride=32B] SkyMiss    (handleOffset=32)         → Group 1
[stride=32B] MainCHit   (handleOffset=64)         → Group 2
[stride=32B] ShadowMiss (handleOffset=96)         → Group 3
```

TraceRay 参数：
- 主光线: `missIndex=0, sbtOffset=0` → Group 1 (SkyMiss), Group 2 (MainCHit)
- 阴影光线: `missIndex=1, sbtOffset=0` → Group 3 (ShadowMiss), 无 ClosestHit

#### 4.3.5 递归深度限制

当前 `maxRecursionDepth = 1`，仅支持主光线 + 阴影光线（一层间接）。如需反射/折射 → 需要开到 2+。

---

### 4.4 文件变更清单

#### 新增文件

| 文件 | 状态 | 说明 |
|------|:--:|------|
| `Engine/Shader/Shaders/RT_PBR.rchit.slang` | ✅ | 顶点拉取 + 重心插值法线 + bindless 纹理采样 |
| `Engine/Shader/CMakeLists.txt` | ✅ | RT 着色器 `-emit-spirv-via-glsl -force-glsl-scalar-layout` |
| `Engine/Render/Pipeline/RTPass.h` | ✅ | CreateVertexPullBuffer + IndexPullBuffer + BindlessDescriptorSet |
| `Engine/Render/Pipeline/RTPass.cpp` | ✅ | 48B→32B 解包 + SSBO 绑定 + set=2 描述符管理 |
| `Samples/02.Cube/02.Cube.cpp` | ✅ | 启用 RT_PBR (set=0+1+2, 单 mesh) |
| `Samples/03.Sponza/03.Sponza.cpp` | ✅ | 保持 Phase 3 shader (多 mesh 兼容), set=0+1 |
| `Engine/Shader/Shaders/ShaderTypes.slang` | ↔ | GPUVertex 改在 shader 内定义（C++/GPU 布局不同） |
| `Engine/Shader/Shaders/RT_Shadow.rgen.slang` | ⬜ | 待完善 |
| `Engine/Shader/Shaders/RT_Common.rchit.slang` | ⬜ | 待合并到 RT_PBR |
| `Engine/Shader/Shaders/RT_Common.rmiss.slang` | ⬜ | 待实现阴影 Miss |
| `Engine/Render/Pipeline/BindlessTextureManager.h` | ⬜ | bindless 纹理注册逻辑已内嵌到 RTPass，独立类待提取 |

---

### 4.5 实施顺序

```
Phase 4.0  ✅ 已完成   slangc via-glsl 路径 NonUniform 装饰（7 个）
Phase 4.0a ✅ 已完成   修改 CMakeLists.txt → RT 着色器 via-glsl + scalar-layout
Phase 4.0b ✅ 已完成   端到端验证：StructuredBuffer 在 ClosestHit 中工作
    ↓
Phase 4.1a ✅ 已完成   RHI 层 StorageBuffer 已有（Phase 3 已实现，无需修改）
Phase 4.1b ✅ 已完成   RTPass::CreateVertexPullBuffer (48B→32B) + IndexPullBuffer
Phase 4.1c ✅ 已完成   RT_PBR.rchit.slang 顶点拉取 + 重心插值法线
    ↓
Phase 4.2a ✅ 已完成   RTPass::CreateBindlessDescriptorSet (set=2, 256 纹理) + RegisterBindlessTexture
Phase 4.2b ⬜ 待实施   glTF 纹理加载 + 注册到 bindless 数组
Phase 4.2c ⬜ 待实施   ClosestHit PBR 完整管线（baseColor + metallic-roughness + normal map）
    ↓
Phase 4.3a ⬜ 待实施   阴影光线 SBT 配置
Phase 4.3b ⬜ 待实施   Miss shader 阴影逻辑
Phase 4.3c ⬜ 待实施   ClosestHit 阴影发射 + 累积
```

---

## 执行状态

```
Phase 1 ✅ 已完成并验证
    ↓
Phase 2 ✅ 已完成（含 02.Cube RT 模式）
    ↓
Phase 3 ✅ 已完成（Texture2D 材质 + UB 光源）
    ↓
Phase 4 🔄 进行中 → 4.0 ✅ → 4.1 ✅ → 4.2a ✅ → 4.2b/c ⬜ → 4.3 ⬜

**最新提交**：`820d2cb` Phase 4: RT 逐顶点法线 + Bindless 纹理基础设施
- 02.Cube: RT_PBR shader (set=0 + set=1 + set=2) 顶点拉取 + bindless ✅
- 03.Sponza: RT_Sponza shader (set=0 + set=1) Phase 3 兼容 ✅

---

## Phase 4.1 实施笔记（2026-07-10）

### 关键技术发现

1. **StaticVertex 布局**：`GLM_FORCE_DEFAULT_ALIGNED_GENTYPES` + struct 对齐 → `sizeof(StaticVertex) = 48` 字节。
   - 每顶点内偏移：pos(0), normal(16), uv(32) — 与 32B GPU 布局一致，仅尾部有 8B struct padding
   - 转换公式已验证：`v[0..2]→pos, v[4..6]→normal, v[8..9]→uv`

2. **多 mesh 限制**：Sponza 是多 mesh 场景，当前 Phase 4 顶点拉取仅支持单 mesh。
   - 单 mesh 场景（02.Cube）：可直接使用 RT_PBR.rchit
   - 多 mesh 场景（03.Sponza）：保持使用 Phase 3 着色器（`-WorldRayDirection()` 近似）
   - 未来方案：合并所有 mesh 到一个缓冲，或使用 per-instance buffer offset

3. **BufferUsage::Storage** 不需要在 MeshComponent 中添加（`CreateVertexPullBuffer` 创建独立 SSBO）

### Phase 4.1 文件变更

| 文件 | 状态 | 说明 |
|------|:---:|------|
| `Engine/Shader/CMakeLists.txt` | ✅ 已改 | RT 着色器: `-emit-spirv-via-glsl` + `-force-glsl-scalar-layout` |
| `Engine/Shader/Shaders/RT_PBR.rchit.slang` | ✅ 新建 | 顶点拉取 + 重心插值法线（单 mesh 用） |
| `Engine/Render/Pipeline/RTPass.h` | ✅ 已改 | `CreateVertexPullBuffer()` + `UpdateVertexDataDescriptorSet()` |
| `Engine/Render/Pipeline/RTPass.cpp` | ✅ 已改 | 48B→32B 解包 + SSBO 绑定 |
| `Samples/03.Sponza/03.Sponza.cpp` | ✅ 已改 | 保持 Phase 3 着色器（多 mesh 兼容） |
| `Engine/Scene/Private/MeshComponent.cpp` | ↔ 未改 | 保持原有 buffer usage |
