# RHI Ray Tracing + Mesh Shader 支持方案

> 最后更新: 2026-07-09

## 一、现状分析

RHI 已有部分骨架代码，但核心接口缺失：

| 已有 | 缺失 |
|------|------|
| `ShaderStage::RayGen/AnyHit/ClosestHit/Miss` | RT Pipeline State（shader group、递归深度、payload 大小） |
| `ShaderStage::Mesh/Amplification` | Mesh PSO（无 IA 阶段，无 vertex input） |
| `DeviceCaps::supportsRayTracing/MeshShaders` | Acceleration Structure 接口（BLAS/TLAS 创建/更新） |
| `BufferUsage::AccelerationStruct` | Shader Binding Table（SBT） |
| `PipelineStateDesc::meshShader/amplificationShader` | `TraceRays()` / `DrawMeshTasks()` 命令 |

---

## 二、Ray Tracing 接口设计

### 2.1 新文件：`Engine/RHI/Public/RHI/RayTracing.h`

#### Acceleration Structure

```cpp
enum class AccelerationStructureType : u8 {
    BottomLevel,  // BLAS — 每个 mesh 的几何数据
    TopLevel,     // TLAS — 实例集合（BLAS + transform）
};

enum class ASBuildFlags : u8 {
    None              = 0,
    AllowUpdate       = 1 << 0,  // 允许增量更新（需要更多内存）
    AllowCompaction   = 1 << 1,  // 允许压缩
    PreferFastTrace   = 1 << 2,  // 优化追踪性能
    PreferFastBuild   = 1 << 3,  // 优化构建速度
    MinimizeMemory    = 1 << 4,  // 最小化内存占用
};

enum class RTGeometryType : u8 {
    Triangles,   // 三角形几何
    AABBs,       // 包围盒（用于程序化几何）
    Instances,   // TLAS 实例
};

struct RTGeometryDesc {
    RTGeometryType  type = RTGeometryType::Triangles;
    IRHIBuffer*     vertexBuffer   = nullptr;
    Format          vertexFormat   = Format::RGB32_FLOAT;
    u64             vertexStride   = 12;
    u32             maxVertex      = 0;
    IRHIBuffer*     indexBuffer    = nullptr;
    Format          indexFormat    = Format::R32_UINT;
    u32             maxPrimitiveCount = 0;
    IRHIBuffer*     transformBuffer   = nullptr;  // 4x3 行主序变换矩阵
};

struct BLASBuildDesc {
    std::vector<RTGeometryDesc> geometries;
    ASBuildFlags flags = ASBuildFlags::PreferFastTrace;
    u32          maxVertexCount = 0;
};

struct TLASInstanceDesc {
    float3x4 transform;           // 3x4 行主序仿射变换
    u32      instanceID      : 24;
    u32      instanceMask    : 8;
    u32      sbtOffset       : 24;
    u32      flags           : 8;
    u64      blasAddress;          // BLAS GPU 地址
};

struct TLASBuildDesc {
    u32          maxInstanceCount = 0;
    ASBuildFlags flags = ASBuildFlags::PreferFastTrace;
};

struct ASBuildSizes {
    u64 accelerationStructureSize = 0;
    u64 buildScratchSize         = 0;
    u64 updateScratchSize        = 0;
};

class IRHIAccelerationStructure {
public:
    virtual ~IRHIAccelerationStructure() = default;
    virtual u64 GetDeviceAddress() const = 0;
    virtual u64 GetSize() const = 0;
};
```

#### RT Pipeline State

```cpp
enum class RTShaderGroupType : u8 {
    RayGen,        // 光线生成（每组 1 个）
    Miss,          // 未命中（每组 1 个）
    Hit,           // 命中组（ClosestHit + AnyHit + Intersection）
    Callable,      // 可调用着色器
};

struct RTShaderGroup {
    RTShaderGroupType type = RTShaderGroupType::Hit;
    u32 generalShader    = ~0u;  // RayGen/Miss 索引
    u32 closestHitShader = ~0u;  // ClosestHit 索引
    u32 anyHitShader     = ~0u;  // AnyHit 索引（可选）
    u32 intersectionShader = ~0u; // Intersection 索引（可选，默认 AABB）
    String name;
};

struct RTPipelineStateDesc {
    std::vector<ShaderBytecode> shaders;        // 所有 RT shader
    std::vector<RTShaderGroup>  shaderGroups;   // 着色器组

    u32 maxRecursionDepth   = 1;     // 最大递归深度
    u32 maxPayloadSize      = 32;    // 光线 payload 最大字节
    u32 maxHitAttributeSize = 8;     // 命中属性最大字节

    std::vector<PushConstantRange> pushConstantRanges;
    std::vector<DescriptorSetLayoutHandle> descriptorSetLayouts;
    String debugName;
};

class IRHIRayTracingPipelineState {
public:
    virtual ~IRHIRayTracingPipelineState() = default;
    virtual u32 GetShaderGroupCount() const = 0;
    virtual u32 GetShaderGroupHandleSize() const = 0;
    virtual std::vector<u8> GetShaderGroupHandles() const = 0;
};
```

#### Shader Binding Table

```cpp
struct SBTSlot {
    u64 handleOffset = 0;  // 着色器组句柄偏移（字节）
    u64 dataOffset   = 0;  // 着色器记录数据偏移
    u64 dataSize     = 0;  // 着色器记录数据大小
    u64 stride       = 0;  // 每 SBT 条目步长
};

struct SBTDesc {
    SBTSlot rayGen;
    SBTSlot miss;
    SBTSlot hit;
    SBTSlot callable;
    IRHIBuffer* buffer = nullptr;
};
```

### 2.2 Device 新增接口

```cpp
// IRHIDevice:
virtual std::unique_ptr<IRHIAccelerationStructure>
    CreateBLAS(const BLASBuildDesc& desc) = 0;
virtual std::unique_ptr<IRHIAccelerationStructure>
    CreateTLAS(const TLASBuildDesc& desc) = 0;
virtual ASBuildSizes GetBLASBuildSizes(const BLASBuildDesc& desc) = 0;
virtual ASBuildSizes GetTLASBuildSizes(u32 maxInstanceCount) = 0;
virtual std::unique_ptr<IRHIRayTracingPipelineState>
    CreateRTPipelineState(const RTPipelineStateDesc& desc) = 0;
```

### 2.3 CommandList 新增接口

```cpp
// IRHICommandList:
virtual void BuildBLAS(IRHIAccelerationStructure* blas, IRHIBuffer* scratchBuffer,
                       const BLASBuildDesc& desc, bool update = false) = 0;
virtual void BuildTLAS(IRHIAccelerationStructure* tlas, IRHIBuffer* scratchBuffer,
                       IRHIBuffer* instanceBuffer, u32 instanceCount,
                       bool update = false) = 0;
virtual void BindRTPipeline(IRHIRayTracingPipelineState* rtPSO) = 0;
virtual void TraceRays(const SBTDesc& sbt, u32 width, u32 height, u32 depth = 1) = 0;
```

---

## 三、Mesh Shader 接口设计

### 3.1 新文件：`Engine/RHI/Public/RHI/MeshShader.h`

```cpp
struct MeshPipelineStateDesc {
    ShaderBytecode* meshShader            = nullptr;  // 必须
    ShaderBytecode* amplificationShader   = nullptr;  // 可选
    ShaderBytecode* pixelShader           = nullptr;  // 可选（片段着色）

    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    bool outputPoints = false;   // 输出点
    bool outputLines  = false;   // 输出线

    bool depthTest   = true;
    bool depthWrite  = true;
    CompareFunc depthCompare = CompareFunc::LessEqual;

    u32   colorAttachmentCount = 1;
    Format colorFormats[8] = {Format::RGBA8_UNORM};
    Format depthFormat = Format::D32_FLOAT;
    u32   sampleCount  = 1;

    std::vector<PushConstantRange> pushConstantRanges;
    std::vector<DescriptorSetLayoutHandle> descriptorSetLayouts;
    String debugName;
};
```

### 3.2 CommandList 新增接口

```cpp
// IRHICommandList:
virtual void DrawMeshTasks(u32 groupCountX, u32 groupCountY = 1, u32 groupCountZ = 1) = 0;
virtual void DrawMeshTasksIndirect(IRHIBuffer* buffer, u64 offset,
                                   u32 drawCount, u32 stride) = 0;
```

---

## 四、实现步骤

| 阶段 | 内容 | 工作量 |
|:---:|------|:---:|
| P1 | `RayTracing.h` / `MeshShader.h` 接口头文件 | 1d |
| P2 | `DeviceCaps` 查询扩展（Vulkan RT/Mesh 功能检测） | 0.5d |
| P3 | Vulkan 后端：`VulkanAccelerationStructure`（BLAS/TLAS 创建+构建） | 3d |
| P4 | Vulkan 后端：`VulkanRTPipelineState`（RT PSO + Shader Group + SBT） | 2d |
| P5 | Vulkan 后端：`TraceRays` + `BuildBLAS`/`BuildTLAS` 命令 | 2d |
| P6 | Vulkan 后端：`DrawMeshTasks` + `DrawMeshTasksIndirect` 命令 | 1d |
| P7 | Slang 编译器配置（RT shader target: `-stage raygeneration/closesthit/miss`） | 0.5d |
| P8 | 示例：基础 Ray Tracing 场景（硬阴影/反射/AO） | 3d |
| P9 | 示例：Mesh Shader 替代传统 VS+IA 管线 | 2d |
| P10 | 压力测试 + 性能调优 | 2d |

**总计约 17 个工作日。**

---

## 五、Vulkan 扩展依赖

```
VK_KHR_acceleration_structure    // BLAS/TLAS
VK_KHR_ray_tracing_pipeline      // RT PSO + TraceRays
VK_KHR_deferred_host_operations  // AS 构建异步
VK_KHR_ray_query                 // 可选：Compute Shader 内 Ray Query
VK_EXT_mesh_shader               // Mesh + Task Shaders
```

---

## 六、管线架构预览

### RT 管线

```
TLAS（场景级）
  ├── Instance[0] → BLAS[Sponza]  (transform)
  ├── Instance[1] → BLAS[Sphere]  (transform)
  └── Instance[2] → BLAS[Cube]    (transform)

RayGen Shader
  └─ TraceRay() ─→ ClosestHit Shader ─→ Miss Shader
                                        └→ Skybox 采样
                   AnyHit Shader
                   └→ Alpha Test 裁剪

SBT 布局:
  [RayGen entry] [Miss entries...] [Hit entries per geometry...]
```

### Mesh Shader 管线替换

```
传统管线:
  IA → VS → HS → Tess → DS → GS → Raster → PS

Mesh Shader 管线:
  Amplification Shader (可选) → Mesh Shader → Raster → PS
  └─ 预剔除 LOD 选择           └─ 直接输出顶点+三角形
```

---

## 七、前向渲染中使用 Ray Tracing 和 Mesh Shader

### 7.1 Ray Tracing 在前向管线中的应用

```
ForwardPipeline::BuildFrameGraph
  │
  ├── "BLAS_Build" Pass（仅在几何变更时）
  │     └── 收集所有 MeshComponent → 创建/更新 BLAS
  ├── "TLAS_Build" Pass（每帧）
  │     └── 收集场景 Entity transform → 构建 TLAS 实例数组 → BuildTLAS
  │
  ├── "RT_Shadow" Pass（替代传统 Shadow Pass，可选）
  │     └── RayGen: 对每个像素发射 shadow ray → 采样光源可见性
  │         Miss:   命中天空，返回 1.0（无遮挡）
  │         Hit:    命中几何体，返回 0.0（被遮挡）
  │         输出: 屏幕空间阴影遮罩纹理 (R16_FLOAT)
  │
  ├── "RT_Reflection" Pass（替代 SSR，可选）
  │     └── RayGen: 对粗糙度 < 阈值的像素发射反射光线
  │         Hit:    采样命中点 GBuffer → 继续追踪或返回颜色
  │         Miss:   回退到 IBL Cubemap 采样
  │         输出: 反射颜色纹理 (RGBA16_FLOAT)
  │
  ├── "RT_AO" Pass（替代 SSAO，可选）
  │     └── RayGen: 对每个像素发射 AO 采样光线（半球随机方向）
  │         Hit:    短距离命中 → 遮蔽 = 1
  │         Miss:   未命中 → 遮蔽 = 0
  │         输出: AO 纹理 (R16_FLOAT)
  │
  ├── "FullScene" Pass（传统光栅化 + RT 输出）
  │     └── PS: directLight * RT_Shadow + indirectSpecular * RT_Reflection + AO * RT_AO
  │
  └── "ToneMap" Pass → BackBuffer
```

**RT 替代传统技术的对比：**

| 效果 | 传统 | RT 替代 | 优势 |
|------|------|---------|------|
| 阴影 | CSM Shadow Maps | RT Shadow (TraceRay) | 无级联过渡、无 shadow acne、天然软阴影 |
| 反射 | SSR (屏幕空间) | RT Reflection | 无屏幕空间限制、可反射屏幕外物体 |
| AO | SSAO (半球采样) | RTAO | 更精确、无采样噪声 halo |
| GI | DDGI 探针 | RT GI (Path Trace) | 无限反弹、动态场景不需要探针更新 |

### 7.2 Mesh Shader 在前向管线中的应用

前向管线的 GBuffer 写入和 HDR 场景渲染**全部改用 Mesh Shader**：

```
传统 Forward：
  Vertex Shader → Raster → Pixel Shader → HDR Target

Mesh Shader Forward：
  Amplification Shader → Mesh Shader → Raster → Pixel Shader → HDR Target
  ├─ 预剔除（视锥/遮挡）    ├─ 直接输出顶点+三角形
  ├─ LOD 选择               ├─ 无 Vertex Buffer 绑定
  └─ Instance Culling       └─ 动态拓扑（可变三角形数）
```

**改造点：**
- `PBR.vert.slang` → `PBR.mesh.slang`（顶点着色 → Mesh Shader）
- 移除 `SetVertexBuffer` / `SetIndexBuffer`（mesh shader 用 storage buffer 自行 fetch 顶点）
- 移除传统 `DrawIndexed` → 改用 `DrawMeshTasks`
- Amplification Shader 可选启用：大场景时做 GPU 视锥剔除 + LOD 选择

---

## 八、延迟渲染中使用 Ray Tracing 和 Mesh Shader

### 8.1 Ray Tracing 在延迟管线中的应用

```
DeferredPipeline::BuildFrameGraph
  │
  ├── "BLAS_Build" Pass
  ├── "TLAS_Build" Pass
  │
  ├── "Shadow" Pass（如果不用 RT Shadow，继续用传统 CSM）
  │
  ├── "GB_Clear" Pass（光栅化 GBuffer 5 MRT）
  │     └── 使用 Mesh Shader 或传统 VS → 写入 GBuffer
  │
  ├── "RT_Reflection" Pass（替代 SSR，可选）
  │     └── RayGen: 从 GBuffer 读取 worldPos + normal + roughness
  │         对粗糙度 < 阈值的像素发射反射光线
  │         Hit:    采样命中点 albedo → 返回反射颜色
  │         Miss:   回退到 IBL Prefilter Cubemap
  │         输出: 反射颜色 (RGBA16_FLOAT)
  │
  ├── "RT_GI" Pass（替代 DDGI/SSGI，可选）
  │     └── RayGen: 半分辨率发射间接漫反射光线
  │         Hit:    采样命中点 emissive + albedo
  │         Miss:   采样 IBL Irradiance Cubemap
  │         输出: 间接漫反射 (RGBA16_FLOAT) + 降噪
  │
  ├── "RT_Shadow" Pass（可选，替代传统 Shadow）
  │     └── 对每个 GBuffer 像素，向每个光源发射 shadow ray
  │         输出: 逐光源阴影遮罩纹理数组
  │
  ├── "Lighting" Pass（全屏 PBR）
  │     └── 从 GBuffer 读 worldPos / normal / albedo / roughness
  │         直接光: RT_Shadow 替代 CSM PCF
  │         间接镜面: RT_Reflection 替代 SSR
  │         间接漫反射: RT_GI 替代 SSGI + DDGI
  │
  └── "ToneMap" Pass → BackBuffer
```

**RT 替代延迟管线中现有技术的映射：**

| 现有 Pass | RT 替代 Pass | GBuffer 数据利用 |
|-----------|-------------|-----------------|
| Shadow Maps (CSM) | RT_Shadow | worldPos → 光线原点, normal → 偏移方向 |
| SSR | RT_Reflection | worldPos + normal → 反射光线, roughness → LOD |
| SSGI + DDGI | RT_GI | worldPos + normal → 漫反射光线 |
| SSAO | RT_AO (归入 RT_Shadow) | worldPos + normal → AO 光线 |

**延迟管线使用 RT 的优势：**
- GBuffer 提供了完整的光线起点数据（worldPos + normal），不需要在 Hit Shader 中重建
- Lighting Pass 只需读取 RT 输出纹理，无需在 shader 中调用 TraceRay（解耦光栅化和光追）
- 渐进式替换：可以逐效果切换（先 RT Shadow → 再 RT Reflection → 最后 RT GI）

### 8.2 Mesh Shader 在延迟管线中的应用

延迟管线的 GBuffer Pass 改用 Mesh Shader：

```
传统 GBuffer：
  VS (GBuffer.vert) → PS (GBuffer.frag) → 5 MRT + D32

Mesh Shader GBuffer：
  Amplification Shader → Mesh Shader → PS (GBuffer.frag) → 5 MRT + D32
  ├─ 视锥剔除             ├─ 直接输出顶点
  ├─ LOD 选择             ├─ 动态生成三角形
  └─ Occlusion Culling    └─ 写入 per-primitive 属性
```

**改造点：**
- `GBuffer.vert.slang` → `GBuffer.mesh.slang`
- Mesh Shader 内直接调用 `SetMeshOutputsEXT(vertexCount, primCount)`
- 输出 `worldPos` / `worldNormal` / `uv` 等 per-vertex 属性供 Pixel Shader 插值
- 移除 Vertex Buffer / Index Buffer 绑定（改用 bindless storage buffer fetch）
- Lighting Pass 的全屏三角形保持不变（本身没有 IA 阶段）

---

## 九、渐进式集成路线

```
Phase 1: RHI 接口（RT + Mesh Shader 头文件 + Vulkan 后端）
   ↓
Phase 2: RT Shadow（先替代 CSM，收益最明显）
   ↓
Phase 3: RT Reflection（替代 SSR，消除屏幕空间限制）
   ↓
Phase 4: RT GI（替代 SSGI+DDGI，消除探针布局限制）
   ↓
Phase 5: Mesh Shader GBuffer（替代传统 VS，减少 CPU draw call 开销）
   ↓
Phase 6: Full RT Pipeline（RayGen 直接输出最终颜色，完全替代光栅化）
