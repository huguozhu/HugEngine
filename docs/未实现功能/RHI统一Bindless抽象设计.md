# RHI 层统一 Bindless 抽象设计

> 日期：2026-08-16 | 状态：设计草案（未实现）| 目标层：RHI + Vulkan
>
> 把当前散落在 Asset 层、以纹理/采样器为限的 bindless 实现，下沉为 RHI 层的统一描述符堆抽象，
> 覆盖纹理 + 采样器 + StorageBuffer（SSBO），为 GPU Driven / RT / Nanite / Work Graph 铺路。

---

## 1. 背景与目标

Bindless 的本质是：**把「资源绑定」从 CPU 端 per-draw 描述符集，变成 GPU 端按 uint 索引直接取**。
shader 里 `u_Textures[idx]` / `u_SSBO[idx]` 任意非均匀索引，draw 只带一个 material/object 索引。

它是以下高级渲染功能的共同地基：

| 功能 | 依赖的 bindless 资源 | 引擎现状 |
|------|---------------------|---------|
| GPU Driven Rendering | 纹理 + 采样器 + buffer（per-object） | GPU Culling + ExecuteIndirect + DGC 已有，缺 buffer bindless |
| Ray Tracing（PT / ReSTIR / Lumen 风格） | 纹理 + buffer（顶点/索引）+ AS | HybridRT + PT Phase 1-3；「无纹理采样」即 bindless 未接 RT |
| Mesh Shading / Nanite | buffer（per-meshlet 顶点/索引） | P3 高级几何，~5% 完成度 |
| GPU Work Graph | 纹理 + buffer | 软件模拟框架已有 |
| 材质系统（Disney BSDF） | buffer（per-material 参数） | 参数塞 GBuffer/GPUObjectData，bindless buffer 更干净 |

**目标**：在 RHI 层提供一个统一的 `IRHIBindlessHeap`，一个堆管所有资源类型的索引分配与描述符写入，
调用方只 `Register* → handle`，不碰描述符细节；Vulkan 侧对应实现 `VulkanBindlessHeap`。

---

## 2. 现状分析

当前 bindless **纹理 + 采样器已能跑**，但存在以下问题：

1. **分层错位**：bindless 机制（描述符堆、索引分配、写描述符）本质是 RHI 的活，却住在
   `Engine/Asset/Asset/BindlessTextureManager`（全局单例）。Render/RT 要 bindless buffer 或 AS，
   只能再抄一遍「注册 → 给 index → 写描述符 → flush」。
2. **只支持纹理 + 采样器**：`m_Textures[]` / `m_Samplers[]` 两类。SSBO/UBO/AS 无处安放。
3. **索引只增不减、无生命周期安全**：`m_TextureCount` 单调递增，无 free-list，无 generation 校验，
   资源销毁后旧 index 是悬空索引。
4. **设备上限写死**：描述符池容量（8192/4096）硬编码在 `VulkanDevice_Descriptors.cpp`，
   不查询设备实际的 `maxUpdateAfterBindDescriptors` 等。
5. **三缓冲同步泄漏到 Asset 层**：`RegisterDescriptorSet()` 手动登记 N 帧 set、`FlushPending()` 逐个写。
6. **多后端不可移植**：binding 5/6、Vulkan flag 全写在 Asset 层，D3D12/Metal 无法映射。

现状关键事实（已核实）：

- `Engine/Asset/Asset/BindlessTextureManager.{h,cpp}` — 全局单例，`RegisterTexture/RegisterMaterial/FlushPending`。
- `Engine/RHI/RHI/Types.h` — `DescriptorSetLayoutBinding.bindless` 布尔标志。
- `Engine/RHI/Vulkan/VulkanDevice_Descriptors.cpp` — bindless binding 打
  `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | PARTIALLY_BOUND_BIT | VARIABLE_DESCRIPTOR_COUNT_BIT`，
  池带 `UPDATE_AFTER_BIND_POOL_BIT`。
- `Engine/RHI/Vulkan/VulkanDevice.cpp` — descriptor indexing 已启用
  （`shaderSampledImageArrayNonUniformIndexing` / `runtimeDescriptorArray`）。
- `Engine/Shader/Shaders/common.slang` — `u_Textures[]`（binding 5）、`u_Samplers[]`（binding 6），set=0。
- `Engine/Shader/Shaders/ShaderTypes.slang` — `kGPUDescSet_Bindless = 2` 已定义（现用于 TLAS 等），但纹理 bindless 仍在 set=0。

---

## 3. 关键设计决策

### 3.1 单一 `BindlessHeap`（方案 A）

一个 RHI 对象统一管理所有资源类型。内部是**多个描述符数组**（Vulkan 里纹理和 buffer 是不同
descriptor type，物理上不能混一个数组），对外是一个堆、统一入口、统一 flush、统一句柄域。

- 对比「分类型 heap」：避免调用方跨堆协调（一个 material 要 texture 索引 + buffer 索引）。
- 对比「最小增量」：避免延续分层错位 + 每类型复制一套逻辑。

### 3.2 Handle：最小版 = 纯索引

`BindlessHandle = u32`，即该类型数组内的索引，shader 直接当索引用（`u_Textures[idx]`）。

**不现在做**：generation（高 bits，用于 Free 后检测悬空句柄）与 Free 索引复用——静态场景无需求，
留到动态资源（粒子/流式纹理）时再加。

### 3.3 描述符集组织：保持 set=0（最小改动）

bindless 数组继续待在 **set=0**（per-frame），binding 5=纹理、6=采样器，**新增 7=SSBO**。
堆在构造时登记「要写哪些描述符集（N 帧）+ 各自 binding 号」，`Flush()` 内聚同步。

> 备选：移到专用 `kGPUDescSet_Bindless=2` 绑一次（更「真 bindless」、更利于 D3D12/Metal 映射），
> 但要改每个管线的 layout + 每个 shader 的 set 声明，纯 churn 无功能收益。**记为后续优化**。

---

## 4. 接口设计

```cpp
// Engine/RHI/RHI/Bindless.h（新增）
namespace he::rhi {

using BindlessHandle = u32;  // 最小版 = 数组内索引

class IRHIBindlessHeap {
public:
    virtual ~IRHIBindlessHeap() = default;

    // 注册资源 → 返回句柄（该类型数组内的索引）
    virtual BindlessHandle RegisterTexture(IRHITexture* tex, IRHISampler* sampler) = 0;
    virtual BindlessHandle RegisterSampler(IRHISampler* sampler) = 0;
    virtual BindlessHandle RegisterBuffer(IRHIBuffer* ssbo) = 0;   // StorageBuffer

    // 把 pending 变更写入所有已注册的帧描述符集（替代现有 FlushPending）
    virtual void Flush() = 0;

    // 查询（调试 / shader 常量对齐）
    virtual u32 GetTextureCount() const = 0;
    virtual u32 GetBufferCount() const = 0;
};

} // namespace he::rhi
```

创建方式（挂在 `IRHIDevice` 上）：

```cpp
class IRHIDevice {
    // 返回设备级 bindless 堆（懒创建；Vulkan 侧为 VulkanBindlessHeap）
    virtual IRHIBindlessHeap* GetBindlessHeap() = 0;
};
```

描述符集登记（替代现有 `BindlessTextureManager::RegisterDescriptorSet`）：

```cpp
// 构造/初始化时由管线调用，登记「堆要写哪些帧描述符集 + 各类型 binding 号」
struct BindlessHeapDesc {
    u32 frameCount = 0;                        // 三缓冲帧数
    // 每帧的 (set, textureBinding, samplerBinding, bufferBinding)
    // 由堆内部维护；或提供 AddFrameDescriptorSet(set, tBinding, sBinding, bBinding)
};
```

> 具体登记 API 的形态（构造传 desc vs `RegisterDescriptorSet` 方法）在实现计划里定，
> 倾向保留 `RegisterDescriptorSet` 风格的显式登记，改动最小。

---

## 5. Vulkan 实现（VulkanBindlessHeap）

- 内部维护三种 CPU 侧指针数组：`m_Textures[]` / `m_Samplers[]` / `m_SSBOs[]`。
- `Register*` 只 push 指针 + 标 pending；`Flush()` 用现有数组版 `UpdateDescriptorSet` 写全部帧 set。
- 描述符池：纹理/采样器沿用 8192/4096；**新增 StorageBuffer 的 pool size**（估算一个合理初值，后续按需调）。
- SSBO 数组同样打 `VARIABLE_COUNT | PARTIALLY_BOUND | UPDATE_AFTER_BIND`，且 VARIABLE_COUNT 只能设在
  **最后一个 bindless binding** 上（现有 `CreateDescriptorSetLayout` 已处理该约束，SSBO binding=7 需作为最后 bindless binding）。
- descriptor indexing 特性已启用，无需新增扩展。

---

## 6. 迁移路径

1. 删 `Engine/Asset/Asset/BindlessTextureManager.{h,cpp}`，逻辑迁入 `VulkanBindlessHeap`。
2. 调用点改造（`RegisterMaterial/RegisterTexture/RegisterDescriptorSet/FlushPending` →
   `device->GetBindlessHeap()->...`）：
   - `Engine/Asset/Private/glTFLoader.cpp`（纹理加载注册）
   - `Engine/Render/Pipeline/ForwardPipeline.cpp`（默认占位 + 注册 set + FlushPending）
   - `Engine/Render/Pipeline/GBufferRenderer.cpp` / `GBufferRenderer_CPU.cpp` / `GBufferRenderer_GPU.cpp`
3. `Samples/03.Sponza/03.Sponza.cpp`（占位纹理注册）。

---

## 7. SSBO 接入

- shader：`common.slang` 加 `[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_SSBO[];`
- `ShaderTypes.slang` 加 `kGPUBinding_BindlessSSBO = 7`。
- 堆加 `RegisterBuffer(IRHIBuffer*)`。
- **具体消费者**（per-material 数据 / per-meshlet / RT 顶点）留到后续单独接——本次只给基础设施，不给假消费者（YAGNI）。

---

## 8. 分阶段范围

| 阶段 | 内容 | 本次 |
|------|------|:---:|
| 1 | `IRHIBindlessHeap` + `VulkanBindlessHeap`，迁移纹理/采样器，SSBO 数组 + `RegisterBuffer` 就位 | ✅ |
| 2 | SSBO 具体消费者（per-material / meshlet / RT 顶点） | 后续 |
| 3 | 专用 set=2 绑一次、generation/Free、AS bindless、多后端 | 后续 |

---

## 9. 影响面（本设计解锁的能力）

- **统一资源堆**：纹理/采样器/SSBO 共用一套索引分配与 flush，后续加 AS 只需堆内加一个数组。
- **分层正确**：调用方只 `Register* → handle`，不碰 descriptor set / binding / Vulkan flag。
- **可扩展**：RT（纹理+buffer+AS）、Nanite（meshlet buffer）、Work Graph、材质系统共用同一 bindless API。
- **可移植**：`IRHIBindlessHeap` 是干净抽象，D3D12 descriptor heap / Metal argument buffer 可映射。

---

## 10. 风险与权衡

| 风险 | 说明 | 应对 |
|------|------|------|
| 索引只增不减 | 静态场景 OK；动态资源需 Free + generation | 阶段 3 再加 |
| SSBO 无消费者（dead code） | 阶段 1 只给数组 + API | 明确标注，阶段 2 接消费者前不宣称可用 |
| 描述符池容量写死 | 8192 等不感知设备 | 后续查 `maxUpdateAfterBindDescriptors` 按需分配（可并入阶段 1 或 3） |
| set=0 vs set=2 | set=0 最小改动但非「绑一次」 | 保持 set=0，set=2 记为后续 |

---

> 生成日期：2026-08-16 | 状态：设计草案
> HugEngine 渲染引擎
