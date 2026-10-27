# D3D12 与 Vulkan 差异清单（HugEngine）

> 本文只记录**在本引擎里真实存在、且已经遇到或已经适配**的差异点。
> 依据标注：
> - ✅ 已在本实现中验证（有实测证据）
> - ⚠️ 已知差异，但本项目尚未验证
> - ❓ 待确认（有迹象但没有结论）
>
> 反向的疑问（比如"Vulkan 里为什么这样写"）不在本文范围。

---

## 1. 着色器与描述符绑定

### 1.1 绑定声明语法 ✅

| | Vulkan | D3D12 |
|---|---|---|
| 声明 | `[[vk::binding(B, S)]]` / `layout(set=S, binding=B)` | `register(tN, spaceS)`（`t`=SRV、`s`=Sampler、`u`=UAV、`b`=CBV）|

本引擎的约定：**同一行同时写两种**，让一份 Slang 源码服务两个后端：

```slang
[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]]
StructuredBuffer<GPUObjectData> u_Objects : register(t2, space0);
```

`[[vk::binding]]` 决定 Vulkan 的 `set/binding`，`: register(...)` 决定 D3D12 的 `space/register`。
**光靠 `[[register(...)]]` 不行** —— Slang 不认这个属性，会报 `unknown attribute 'register'`；必须用 HLSL 的 `: register(...)` 后缀。

### 1.2 绑定维度的"顺序方向" ✅

- Vulkan 用 **set** 分组：`set` 是外层，`binding` 是内层
- D3D12 用 **space** 分组：`space` 是外层，`tN` 是内层

两者可以一一对应（`set`↔`space`、`binding`↔`N`），但**书写顺序相反**，写反了不会报错，只会读错数据。

### 1.3 无界数组（bindless）✅

- **Vulkan**：一个 `binding` 配 `VARIABLE_DESCRIPT_COUNT` 就能承载任意长度的 bindless 数组
- **D3D12**：同一 `space` 内**不允许出现多个无界资源**，`dxc` 直接报
  `error: more than one unbounded resource (...) in space 2`

本引擎的落法：把 bindless 数组拆成**四段定长区间**，全部放在 `space2`：

| 区间 | 用途 | 着色器侧 |
|---|---|---|
| `t0 .. t63` | 纹理数组 | `u_Textures[64]` |
| `t64 ..` | 实例变换 | `u_InstanceTransforms[64]` |
| `t128 ..` | 可见实例索引 | `u_VisibleInstanceIndices[64]` |
| `t192 ..` | 骨骼矩阵 | `u_BoneMatrices[64]` |
| `t256 ..` | 材质 | `u_Materials[64]` |

配套：D3D12 后端把每个注册的缓冲**同时写入四段**（`64+h / 128+h / 192+h / 256+h`），
这样着色器按句柄 `h` 索引哪个数组都能取到同一份数据。

### 1.4 顶点输入的语义命名（本项目踩过的大坑）✅

- **只写 `layout(location = N) in ...` 时**，Slang 生成 DXIL 会**按变量名字母序**编造语义名：

  | 声明 | location | Slang 实际生成 |
  |---|---|---|
  | `inJoints` | 3 | `VERTEX_IN_0` |
  | `inNormal` | 1 | `VERTEX_IN_1` |
  | `inPosition` | 0 | `VERTEX_IN_2` |
  | `inUV` | 2 | `VERTEX_IN_3` |
  | `inWeights` | 4 | `VERTEX_IN_4` |

- D3D12 的输入布局按「语义名 + 索引」匹配，于是顶点缓冲开头的 12 字节（位置）被当成 `inJoints` 解释 ⇒ 顶点整体塌缩、画面全黑。
- **Vulkan 不受影响**（用 `location`，不看语义名），所以这个问题只在 D3D12 上暴露。

**当前统一写法**（两者都留）：

```slang
struct PBRVertexInput {
    [[vk::location(0)]] float3 inPosition : POSITION0;   // vk 用 location，D3D 用语义
    [[vk::location(1)]] float3 inNormal   : NORMAL0;
    [[vk::location(2)]] float2 inUV       : TEXCOORD0;
    [[vk::location(3)]] uint8_t4 inJoints : BLENDINDICES0;
    [[vk::location(4)]] float4 inWeights  : BLENDWEIGHT0;
};
```

Slang 没有能同时固定 SPIR-V location 的 HLSL 原生属性，所以 `[[vk::location]]` 不能省；
只留语义也能编过（SPIR-V 按声明顺序分配 location），但那依赖"成员顺序恰好等于 0..4"的隐含约定。

### 1.5 结构化缓冲的元素步长 ✅

- **D3D12**：SRV 的 `StructureByteStride` **必须**等于着色器结构体的 `sizeof(T)`。写错（例如一律写 4）会让着色器按错误步长读数组，表现为"数据都在，但读出来全是 0/垃圾"。
- **Vulkan**：由 buffer view 的 range 与着色器类型决定，不需要前端显式给 stride。

本引擎的补齐方式：DXIL 反射 + 引擎已知类型的兜底表

| 类型 | 大小 | 对应寄存器 |
|---|---|---|
| `GPULight` | 64 | `t1` |
| `GPUObjectData` | 208 | `t2` |
| `GPUShadowData` | 256 | `t3` |
| `LightGridCell` | 8 | `t7` |
| `uint` | 4 | `t8` |

注意：D3D12 运行期反射需要 `dxcompiler.dll` 随程序分发；`DxcCreateInstance` 失败时反射拿不到尺寸，只能走兜底表。

---

## 2. 资源与内存

### 2.1 可映射内存 ✅

| | Vulkan | D3D12 |
|---|---|---|
| CPU 可写内存 | `HOST_VISIBLE\|HOST_COHERENT` 内存直接 `Map` | `DEFAULT` 堆**不能** `Map`，必须用 `UPLOAD` 堆 + `CopyBufferRegion` |

- `UPLOAD` 堆资源**不能**带 `ALLOW_UNORDERED_ACCESS`，否则 `CreateCommittedResource` 返回 `E_INVALIDARG`
- `DEFAULT` 堆资源做 UAV 时，CPU 写入要经过一份等大的 `UPLOAD` 暂存

### 2.2 资源状态 / 屏障 ✅

- **Vulkan**：image layout + access mask
- **D3D12**：显式 `ResourceBarrier` 状态转换，`StateBefore` 必须等于 GPU 的**真实**状态。
  用"软件跟踪值"当 `StateBefore` 且与实际不符时，D3D12 会**静默丢弃**这次转换对应的拷贝/使用 —— 表现为"CPU 明明写了数据，GPU 上仍是 0"。

### 2.3 交换链 ✅

D3D12 的交换链缓冲不会自动进入可渲染状态，需要在 `BeginRenderPass` / `Present` 前后显式做
`PRESENT ↔ RENDER_TARGET` 转换。

### 2.4 描述符堆（D3D12 独有）✅

- `CBV_SRV_UAV` 堆与 `SAMPLER` 堆**必须分开**；`SetDescriptorHeaps` 一次只能绑 **一个 SRV 堆 + 一个采样器堆**
- RTV / DSV 需要单独的非 shader-visible 堆
- bindless 的写入必须落在**实际被绑定的那张堆**上；写到另一张堆不会报错，但着色器永远读不到

### 2.5 UAV 描述符数量 ✅

`NumElements = 0` 的 SRV/UAV 是非法描述符，创建时会让设备直接移除（`0x887a0005`）。
缓冲区小于 stride 时必须把元素数钳到 1。

---

## 3. 命令与同步

### 3.1 推常量 ✅

| | Vulkan | D3D12 |
|---|---|---|
| 机制 | `push_constant`（本引擎 160 字节）| 没有对应物，用 **root CBV** 指向常量环 |
| 绑定 | 一次 `vkCmdPushConstants` | `SetGraphicsRootConstantBufferView` |

本引擎根签名的前 8 个参数就是 root CBV（`b0..b7`），着色器的 `FrameConstants` 落在 `b0`/`b1`，两者绑同一块内存。
**同一批常量必须落在同一块内存**：引擎一次"常量集"可能分多次推送（相机 offset=0、逐物体 offset>0），若每次推送都另开槽位，后一次会覆盖前一次。

### 3.2 围栏与资源释放 ✅

- D3D12 的拷贝/资源释放需要显式围栏等待；分配器在被 GPU 使用期间 `Reset` 会丢弃这次拷贝
- 资源销毁要延迟到 GPU 完成（本引擎用 `DeferRetire` / `FlushRetired`）

---

## 4. 管线与状态

### 4.1 根签名参数布局（本引擎）✅

```
0 .. 7   root CBV  (b0..b7, space0)
8 .. 15  SRV 表    (space 0..7)
16 .. 23 UAV 表    (space 0..7)
24 .. 31 Sampler 表(space 0..7)
```

`CommandList` 按这个编号绑定；绑定编号与创建顺序必须成对维护，错一位就会把 UAV/Sampler 表绑到 SRV 槽位（D3D12 会直接卡死，不是报错）。

### 4.2 深度缓冲清屏 ✅

`D32_FLOAT` 没有模板位，`ClearDepthStencilView` 带 `D3D12_CLEAR_FLAG_STENCIL` 会被拒绝，
深度缓冲保持未初始化 ⇒ 所有物体过不了深度测试。只有 `D24S8` / `D32S8` 才能带模板标志。

### 4.3 纹理维度与格式 ✅

- `arrayLayers == 6` 的纹理按 `TEXTURECUBE` 建 SRV
- `D32_FLOAT` / `R24_UNORM_X8_TYPELESS` / `R16_UNORM` 在 SRV 上要映射成可采样的格式（如 `R32_FLOAT`），否则 `Format=UNKNOWN` 的 SRV 非法

### 4.4 输入布局的字节偏移 ✅

引擎上传的顶点结构按 **16 字节对齐**（向量后留 4 字节填充），步长 64：

```
POSITION      @0    R32G32B32_FLOAT
NORMAL        @16   R32G32B32_FLOAT
TEXCOORD      @32   R32G32_FLOAT
BLENDINDICES  @40   R8G8B8A8_UINT     (uint8_t4，4 字节)
BLENDWEIGHT   @44   R32G32B32A32_FLOAT
```

按"紧密打包"（0/12/24/32/36）算会让关节索引与蒙皮权重整体错位。

### 4.5 剔除与采样器

- 采样器堆容量有限（本引擎 `kSetSamplerCapacity = 2048`），描述符范围不能超过堆大小

---

## 5. 工具链

| | Vulkan | D3D12 |
|---|---|---|
| 着色器编译 | `slangc -target spirv` | `slangc -target hlsl` → `dxc -T vs_6_6/ps_6_6` → DXIL |
| 内嵌形式 | `*.spv.h` | `*.dxil.h` |
| 运行期反射 | 不需要 | 需要 `dxcompiler.dll`（`IDxcUtils::CreateReflection`）|
| 调试层 | Vulkan Validation Layers | D3D12 Debug Layer（`SetBreakOnSeverity(CORRUPTION, TRUE)` 在无调试器时会挂起）|

**构建陷阱**：DXIL 是以头文件形式嵌进 `HugEngineRender` 的，改完着色器必须重建该库，
否则程序里跑的还是旧着色器（RenderDoc 里会看到旧的寄存器布局）。

---

## 6. 已知未解决问题（截至本文写作时）

**现象**：蒙皮网格（`useInstanceID == 3`）在 D3D12 下几何塌缩成细条；Vulkan 正常。

**当前临时处理**：`PBR.vert.slang` 里三条 GPU-driven 分支被 `if (false && ...)` 短路，
全部回退到 `u_Objects[objIndex].worldMatrix`（静态物体路径），此时画面可见。

**已排除**：

- bindless 句柄不一致 —— 实测 `instanceSSBOHandle = 10/11`，与引擎注册的骨骼缓冲（1536B = 24×64）完全对应
- 骨骼缓冲内容 —— GPU 回读为单位阵（`m00=m11=m22=m33=1`），与"骨骼没有运动"一致
- 实例缓冲内容 —— GPU 回读为 `0.5` 缩放、平移为 0 的有效矩阵
- 描述符写入/绑定、根签名映射、深度测试、视锥剔除、命令列表主次不一致

**待查**：

1. `inJoints` / `inWeights` 在着色器里的实际取值（是否合理的小整数索引 + 归一化权重）
2. 顶点布局在 `BLENDINDICES` / `BLENDWEIGHT` 段的两种候选：
   - 当前：`BLENDINDICES@40 (4B)` + `BLENDWEIGHT@44 (16B)`
   - 备选：`BLENDINDICES@32 (16B, uint32×4)` + `BLENDWEIGHT@48 (16B)`

---

## 7. 速查对照表

| 维度 | Vulkan | D3D12 |
|---|---|---|
| 绑定声明 | `[[vk::binding(B,S)]]` | `: register(tN, spaceS)` |
| 分组方向 | set → binding（外层→内层）| space → register（外层→内层）|
| bindless | 单个无界 binding | 同 space 只能一个无界；用分段定长区间 |
| 顶点输入 | `location` 生效 | 语义名+索引生效（必须显式写）|
| 结构体步长 | 隐式 | SRV 的 `StructureByteStride` 必须显式且等于 `sizeof(T)` |
| CPU 写显存 | 直接 Map 可见内存 | DEFAULT 不能 Map；UPLOAD + Copy |
| 状态转换 | layout / access | 显式 `ResourceBarrier`，StateBefore 必须真实 |
| 推常量 | `push_constant` | root CBV + 常量环 |
| 描述符堆 | 由 `VkDescriptorSet` 管理 | 手动 `SetDescriptorHeaps`（SRV 堆 + 采样器堆）|
| 交换链 | 自动进入可渲染布局 | 显式 `PRESENT ↔ RENDER_TARGET` |
| 着色器产物 | SPIR-V | DXIL（需 `dxcompiler.dll` 做反射）|
