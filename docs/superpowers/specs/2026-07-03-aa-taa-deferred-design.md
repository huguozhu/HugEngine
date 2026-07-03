# AA_TAA：延迟渲染时域抗锯齿（含 Velocity Buffer）

> 日期：2026-07-03 | 状态：设计完成 | Phase：P1
> 架构文档引用：M98 AntiAliasing_TAA | M35 DeferredPipeline

---

## 1. 目标

在 DeferredPipeline 中实现完整的 TAA（Temporal Anti-Aliasing），一步到位包含 Velocity Buffer，使引擎在静态和动画场景下均无 ghosting，并为后续 MotionBlur / TAAU / DLSS 铺好基础。

---

## 2. 架构

### 2.1 管线位置

```
GBuffer (4 MRT)                     Lighting (HDR)
  MRT0: albedo+metallic (RGBA16)        │
  MRT1: normal+roughness (RGBA16)       ▼
  MRT2: emissive+ao (RGBA16)       TAA_Resolve ◄── HDR + Depth + Normal + Velocity
  MRT3: velocity (RG16)                 │
  Depth: D32                          ▼
                                   ToneMap → BackBuffer
```

TAA 放在 HDR 空间，ToneMap 之前。继承 `IAntiAliasing` + `IPostProcessPass`，自拥有历史缓冲（`OwnsOutput=true`）。

### 2.2 数据流

```
每帧：
  OnBeginFrame()  → 推进 Halton 抖动序列、交换历史 read/write index
  BuildFrameGraph →
    GB_Clear:      写入 4 MRT + Depth（含 velocity）
    Lighting:     采样 GBuffer → HDR 离屏纹理
    TAA_Resolve:  采样 HDR + Depth + Normal + Velocity + HistoryColor
                  输出到 m_HistoryColor[write]
    ToneMap:      采样 TAA::GetOutputTexture() → BackBuffer
  帧末:           m_PrevViewProj ← m_CurrViewProj
```

---

## 3. 文件清单

### 3.1 新增文件

| 文件 | 内容 |
|------|------|
| `Engine/Render/AntiAliasing/AA_TAA.h` | AA_TAA 类声明 |
| `Engine/Render/AntiAliasing/AA_TAA.cpp` | AA_TAA 实现（初始化、PSO、SetInput、Render、抖动序列） |
| `Engine/Shader/Shaders/TAA_Resolve.vert.slang` | 全屏三角形 VS（3 顶点无 VB） |
| `Engine/Shader/Shaders/TAA_Resolve.frag.slang` | TAA resolve FS（重投影 + 邻域裁剪 + 混合） |

### 3.2 修改文件

| 文件 | 改动内容 |
|------|---------|
| `Engine/Shader/Shaders/GBuffer.vert.slang` | 新增双矩阵投影：输出 `prevClipPos` 到 location 3 |
| `Engine/Shader/Shaders/GBuffer.frag.slang` | 新增 MRT3：输出 `float2 velocity`（UV 空间运动矢量） |
| `Engine/Render/Pipeline/DeferredPipeline.h` | 添加 `m_AntiAliasing`、`m_GBufferD`、`m_PrevViewProj`、`m_CurrViewProj` |
| `Engine/Render/Pipeline/DeferredPipeline.cpp` | 4 MRT 创建 + PSO 修改 + TAA Pass 集成 + 矩阵管理 |
| `Engine/Shader/CMakeLists.txt` | 添加 TAA_Resolve shader 编译目标 |

---

## 4. AA_TAA 类设计

### 4.1 类声明要点

```
IAntiAliasing
  ├── SupportsDeferred() → true
  ├── SupportsForward()  → false
  ├── GetMode()         → AAMode::TAA
  ├── GetJitterOffset() → 当前帧 Halton(2,3) 抖动值（NDC 空间）
  ├── OnBeginFrame()    → 推进 jitterIndex，交换 history read/write index
  │
IPostProcessPass
  ├── SetInput()         → 绑定 HDR 颜色 + sampler
  ├── SetGBufferInputs() → 绑定 Depth + Normal + Velocity（TAA 特有扩展）
  ├── GetOutputFormat()  → RGBA16_FLOAT（HDR 空间）
  ├── OwnsOutput()       → true
  ├── GetOutputTexture() → 返回 m_HistoryColor[read]
  └── GetOutputSampler() → 返回 m_HistorySampler
```

### 4.2 描述符布局

```
set=0:
  binding 0: u_CurrentColor    (CombinedImageSampler)  — Lighting HDR 输出
  binding 1: u_HistoryColor    (CombinedImageSampler)  — 上一帧 TAA 结果
  binding 2: u_Depth           (CombinedImageSampler)  — GBuffer Depth
  binding 3: u_Normal          (CombinedImageSampler)  — GBuffer B
  binding 4: u_Velocity        (CombinedImageSampler)  — GBuffer D
  binding 5: u_TAAUniforms     (UniformBuffer)          — TAAUniforms 结构体

Push constant (16 bytes):
  float2 jitterOffset    — 当前帧子像素抖动（NDC 坐标）
```

### 4.3 TAAUniforms 结构体

```cpp
struct TAAUniforms {
    float4x4 prevViewProj;       // offset 0,  64B — 上一帧 ViewProj
    float4x4 invCurrViewProj;    // offset 64, 64B — 当前帧 InvViewProj（反算世界坐标）
    float2   resolution;         // offset 128, 8B — 视口尺寸
    float    blendFactor;        // offset 136, 4B — 基础混合系数 (0.05)
    float    unused;             // offset 140, 4B — 对齐
};  // 总计 144B
```

### 4.4 历史缓冲

- `m_HistoryColor[2]`：RGBA16_FLOAT，double-buffered（避免读写同一张纹理）
- `m_HistorySampler`：Linear + ClampToEdge
- `m_HistoryRead / m_HistoryWrite`：每帧 swap
- `OnResize` 时销毁重建

### 4.5 抖动序列

Halton(2, 3) 序列，8 样本后循环：

```
index: 0      1      2      3      4      5      6      7
x:     0.500 -0.500  0.250 -0.750  0.875 -0.875 -0.125  0.125
y:     0.333 -0.333 -0.111  0.778 -0.556 -0.222  0.444 -0.444
```

返回值通过 `texelSize = 1.0 / resolution` 缩放为 pixel offset，再转为 NDC offset 给投影矩阵用。

---

## 5. 着色器设计

### 5.1 TAA_Resolve.vert.slang

全屏三角形，3 顶点无 VB/IB，`SV_VertexID` 生成 UV 覆盖 [0,1]。

### 5.2 TAA_Resolve.frag.slang 算法

```
1. 采样 CurrentColor（点采样，无滤波）
2. 采样 Velocity → historyUV = currentUV - velocity
3. 采样 Depth + Normal（当前帧 + history 位置）
4. Disocclusion 检测（三个信号取 max）：
   a. 深度差异：|depth - depthHistory| / depth > 阈值 → disoccluded
   b. 法线差异：1 - dot(N, N_history) > 阈值 → disoccluded
   c. 速度过大：|velocity| > 阈值 → disoccluded
   d. historyUV 越界 → disocclusion = 1.0
5. 若 disocclusion > 0.95：输出纯当前帧
6. 否则：
   a. 采样 HistoryColor
   b. YCoCg 空间 3×3 邻域 AABB 裁剪（消除 ghosting）
   c. blend = lerp(0.05, 0.3, disocclusion)
   d. output = lerp(clampedHistory, currentColor, blend)
```

---

## 6. GBuffer 着色器改动

### 6.1 GBuffer.vert.slang

- push constant 新增 `prevViewProjMatrix`（维持 256B 限制内）
- VSOutput 新增 `[[vk::location(3)]] float4 prevClipPos`
- 每顶点同时做当前帧和上一帧两次投影

### 6.2 GBuffer.frag.slang

- FSOutput 新增 `[[vk::location(3)]] float2 velocity`
- velocity 计算：`currUV = currNDC.xy * 0.5 + 0.5`，`prevUV = prevNDC.xy * 0.5 + 0.5`，`motion = currUV - prevUV`
- MRT 0-2 逻辑完全不变

---

## 7. DeferredPipeline 改动

### 7.1 成员变量新增

```cpp
// GBuffer 第 4 张 MRT
std::unique_ptr<rhi::IRHITexture> m_GBufferD;  // velocity (RG16_FLOAT)

// AA
std::unique_ptr<IAntiAliasing> m_AntiAliasing;

// 相机矩阵（每帧记录）
float4x4 m_PrevViewProj = float4x4(1.0f);
float4x4 m_CurrViewProj = float4x4(1.0f);
```

### 7.2 Initialize 改动

- GBuffer 创建：新增 `m_GBufferD`
- PSO：`colorAttachmentCount = 4`，`colorFormats[3] = RG16_FLOAT`
- push constant range：`size = 256`
- AA_TAA init：`m_AntiAliasing = std::make_unique<AA_TAA>()` → `Initialize(device, w, h)`

### 7.3 BuildFrameGraph 改动

GB_Clear pass 伪代码：

```
BeginOffscreenPassMRT({GB_A, GB_B, GB_C, GB_D}, Depth, clears[5])

// 每 mesh 绘制前：
pc.viewProjMatrix = m_CurrViewProj;
pc.prevViewProjMatrix = m_PrevViewProj;
pc.objectIndex = di.objectIndex;
cmd->SetPushConstants(0, 256, &pc);
// ... DrawIndexed ...

EndOffscreenPass()
```

新增 TAA pass（Lighting 之后、ToneMap 之前）：

```
m_AntiAliasing->SetInput(m_HDRTarget, m_HDRSampler);
m_AntiAliasing->SetGBufferInputs(m_GBufferDepth, m_GBufferB, m_GBufferD);
m_AntiAliasing->Render(cmd);

// 然后 ToneMap 读 TAA 输出：
m_ToneMap->SetInput(m_AntiAliasing->GetOutputTexture(),
                    m_AntiAliasing->GetOutputSampler());
```

### 7.4 帧首/帧末

```cpp
// Render() 开始：
m_AntiAliasing->OnBeginFrame();

// 如果是首帧，m_PrevViewProj = m_CurrViewProj（无运动）

// BuildFrameGraph 最后，ToneMap 做完后：
m_PrevViewProj = m_CurrViewProj;
```

### 7.5 OnResize 改动

新增 `m_GBufferD` 重建 + `m_AntiAliasing->OnResize(w, h)`。

---

## 8. 兼容性注意事项

### 8.1 RHI 先修项（阻塞）

**BeginOffscreenPassMRT 附件数组溢出**：`VulkanCommandList.cpp` 当前 `VkImageView attachments[4]` 栈数组仅能容纳最多 4 个附件（颜色 + 深度）。4 MRT + D32 = 5 附件，会越界写入 VkFramebufferCreateInfo。需要：

1. 将 `attachments[4]` 改为 `attachments[5]`（或使用 `std::vector` 动态分配）
2. 同时修正循环约束 `i < colorCount && attachmentCount < 4` → `attachmentCount < 5`

这是一个**低风险单行改动**，在本任务中一并修复。

### 8.2 RHI 格式支持

- `RG16_FLOAT` 对应 `VK_FORMAT_R16G16_SFLOAT`，在现代 GPU 上普遍支持
- 确认 `rhi::Format::RG16_FLOAT` 在 `RHI/Types.h` 枚举中已定义。若不存在，需新增枚举值 + Vulkan 映射

### 8.3 Push Constant 容量

引擎已设置 `caps.maxPushConstantsSize = 256`（`VulkanDevice.cpp:61`）。GBuffer push constant 从 128B 扩到 256B 刚好在限制内，无需 uniform buffer 备选方案。

### 8.2 ForwardPipeline 兼容

`SupportsForward()` 返回 `false`，AA_TAA 不在 ForwardPipeline 中注册。Forward 走 MSAA (M97) 或 FXAA (M99)。

### 8.3 首帧行为

首帧时 `m_PrevViewProj == m_CurrViewProj`，velocity 全为 0，TAA 等价于当前帧直接输出（无 ghosting）。

---

## 9. 测试验证

| 测试 | 方法 | 通过标准 |
|------|------|---------|
| 基础渲染 | Sponza 场景运行 04.Deferred | 画面无崩溃、无黑屏 |
| 抗锯齿效果 | 静止相机对比 TAA on/off | 几何边缘明显平滑 |
| Ghosting | 快速旋转/平移相机后停止 | 静止后无拖影残留 |
| 邻域裁剪 | 相机快速移动时观察高对比度边缘 | 无明显 ghost 色彩 |
| Disocclusion | 相机突然转向新的场景区域 | 新区域无历史残影 |
| 内存 | RenderDoc 检查帧资源 | 4 MRT + history 内存增量在预期内 |
| Resize | 运行时拉伸窗口 | 无崩溃、历史缓冲正确重建 |

---

## 10. 后续扩展路径

- **MotionBlur (M89)**：直接绑定 `m_GBufferD` 作为 velocity 输入
- **TAAU (M96)**：修改抖动偏移范围 + 输出分辨率参数即可
- **DLSS/FSR/XeSS (M73)**：Color + Depth + Velocity 三输入全齐，Streamline 集成无阻
- **NRD 降噪器 (M71)**：法线 + 深度输入已有，velocity 可做 motion vector guided filtering
