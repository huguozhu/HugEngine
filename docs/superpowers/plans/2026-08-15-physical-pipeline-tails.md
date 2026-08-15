# 物理渲染管线剩余缺口（尾巴）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 完成 `基于物理的渲染管线剩余缺口.md` 末尾列出的 3 项剩余尾巴：① 物理天空「太阳盘→方向光 illuminance 同步」；②「空中透视（大气消光）」；③ HDR 输出接入 Forward/HybridRT/PathTracing 全部管线（当前仅 Deferred）。

**Architecture:** 太阳同步在 Scene 层新增一个自由函数，由各管线帧入口在阴影/光照收集前调用一次；空中透视在 Slang 侧新增自包含的 `Atmosphere.slang`（瑞利+米氏单散射消光），把 `sunDir+turbidity`（一个 float4）塞进 Deferred/Forward 两套 push constant，在光照结算末尾对场景几何应用；HDR 复用 Deferred 已落地的 `IRHISwapChain::GetColorFormat()` + `ToneMapPass::SetOutputFormat()/SetHDREnabled()` 机制，逐处替换其余管线硬编码的 `BGRA8_UNORM`。

**Tech Stack:** C++17 / Slang shader / Vulkan（CMake MSVC 2026）。

**Spec:** `docs/未实现功能/基于物理的渲染管线剩余缺口.md`（「尚未完成的尾巴」一节，末尾两条）。

## Global Constraints

- **逐功能验证门控**：每个任务只实现一个功能，编译 + 运行验证通过后才进入下一个。
- **Commit 规则（CLAUDE.md）**：不自动 `git commit`；提交前征得用户确认。中文 commit log，不含 AI 信息（无 Co-Authored-By）。
- **代码注释**：新增代码附中文注释。
- **构建命令**：`cmake --build build --config Debug`（含 Slang→SPIR-V 编译；shader 语法错误在编译期暴露）。新增 `.slang` **仅当它是独立编译单元（含 entry point）时才需**加入 `Engine/Shader/CMakeLists.txt` 的 `FRAG_SLANG`；纯 include 头（如 `common.slang`、`pbr_common.slang`）**不加入**。
- **运行验证**：`build/bin/Debug/02.Cube.exe`（Forward/Deferred/HybridRT/PathTracing 四模式切换）与 `build/bin/Debug/04.Deferred.exe`（Deferred）。自动化验证 = 编译通过 + 运行无崩溃/无 shader 报错；视觉确认由用户完成。
- **push constant 上限**：Vulkan 规范保证 `maxPushConstantsSize ≥ 128` 字节；桌面 GPU（NVIDIA/AMD/Intel）均为 256。本计划 Forward push constant 将扩到 144 字节，桌面环境安全（若未来需 128 硬约束，改用 uniform buffer 承载空中透视参数）。
- **C++/Slang push constant 双源同步**：`ShaderTypes.slang` 的 `GPU_STRUCT`（C++ 侧）与各 `.frag.slang` 内手写的 `[[vk::push_constant]] cbuffer`（Slang 侧）是两处独立定义，必须逐字段同步；本计划只做「在末尾追加字段」，不改动既有偏移。

---

### Task 1: 太阳盘 → 方向光 illuminance 自动同步

**Files:**
- Modify: `Engine/Scene/Scene/PhysicalSkyComponent.h`（新增 `sunIlluminance` 字段 + 两个自由函数声明）
- Modify: `Engine/Scene/Scene/PhysicalSkyComponent.cpp`（实现两个自由函数）
- Modify: `Engine/Scene/Scene/LightComponent.h`（`DirectionalLight` 新增 `syncWithPhysicalSky` 开关）
- Modify: `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp`（帧入口调用同步）
- Modify: `Engine/Render/Pipeline/ForwardPipeline_FrameGraph.cpp`（帧入口调用同步）
- Modify: `Engine/Render/Pipeline/ForwardPipeline.cpp`（非 RG 分支帧入口调用同步）
- Modify: `Engine/Render/Pipeline/HybridRTPipeline.cpp`（帧入口调用同步）
- Modify: `Engine/Render/Pipeline/PathTracingPipeline.cpp`（帧入口调用同步）
- Modify: `Samples/02.Cube/02.Cube.cpp`（`mainDL->syncWithPhysicalSky = true`）

**Interfaces:**
- Consumes: `World::ForEach<T>`（`Scene/World.h`）、`PhysicalSkyComponent::sunDirection/sunIntensity`、`DirectionalLight::direction/illuminance`。
- Produces: `he::SyncPhysicalSkyToSun(World&)`、`he::GetPhysicalSkySun(World&, float3&, float&)`（后者供 Task 3 复用）。

- [ ] **Step 1: `LightComponent.h` 给 `DirectionalLight` 增加同步开关**

在 `class DirectionalLight`（`LightComponent.h:50-56`）内 `direction` 字段后追加：

```cpp
    bool   syncWithPhysicalSky = false;   // true=由物理天空太阳同步 direction/illuminance（空中透视与光照自动一致）
```

- [ ] **Step 2: `PhysicalSkyComponent.h` 增加字段与自由函数声明**

`sunIntensity` 字段（`:24`）后追加字段：

```cpp
    float  sunIlluminance = 120000.0f; // 太阳照度基准（lux，sunIntensity=1 时同步到方向光 illuminance）
```

文件末尾（`namespace he` 内、`};` 之前）追加前置声明与函数声明：

```cpp
class World;

// 将物理天空太阳同步到标记了 syncWithPhysicalSky 的方向光（方向 + 照度）
// 由各渲染管线帧入口在阴影/光照收集前调用（幂等，可每帧调用）
void SyncPhysicalSkyToSun(World& world);

// 查询第一个启用中的物理天空，返回太阳方向（指向太阳）与浑浊度；无则返回 false
bool GetPhysicalSkySun(World& world, float3& sunDirection, float& turbidity);
```

- [ ] **Step 3: `PhysicalSkyComponent.cpp` 实现两个自由函数**

`#include "Scene/PhysicalSkyComponent.h"` 之后追加 `#include "Scene/World.h"` 与 `#include "Scene/LightComponent.h"`，并在文件末尾（`OnCreate` 之后、`namespace he` 内）追加：

```cpp
void SyncPhysicalSkyToSun(World& world) {
    // 取第一个启用中的物理天空
    const PhysicalSkyComponent* sky = nullptr;
    world.ForEach<PhysicalSkyComponent>([&](Entity, PhysicalSkyComponent& ps) {
        if (ps.enabled && !sky) sky = &ps;
    });
    if (!sky) return;

    // 方向光 direction 指向光线传播方向（shader 内 L = normalize(-direction)），
    // 太阳 sunDirection 指向太阳本身，故光线方向取反
    float3 lightDir = -glm::normalize(sky->sunDirection);
    float  illuminance = sky->sunIntensity * sky->sunIlluminance;

    world.ForEach<DirectionalLight>([&](Entity, DirectionalLight& dl) {
        if (dl.syncWithPhysicalSky) {
            dl.direction   = lightDir;      // 太阳方向同步（负方向）
            dl.illuminance = illuminance;   // 照度同步（lux，>0 进入物理模式）
        }
    });
}

bool GetPhysicalSkySun(World& world, float3& sunDirection, float& turbidity) {
    bool found = false;
    world.ForEach<PhysicalSkyComponent>([&](Entity, PhysicalSkyComponent& ps) {
        if (ps.enabled && !found) {
            sunDirection = ps.sunDirection;
            turbidity    = ps.turbidity;
            found        = true;
        }
    });
    return found;
}
```

- [ ] **Step 4: 四个管线帧入口调用同步（阴影/光照收集之前）**

每个管线文件顶部（已有 Scene include 区）追加 `#include "Scene/PhysicalSkyComponent.h"`，并在对应函数**第一行**插入 `he::SyncPhysicalSkyToSun(world);`：

| 文件 | 插入位置 |
|------|---------|
| `DeferredPipeline_FrameGraph.cpp` | `DeferredPipeline::BuildFrameGraph(...)` 函数体第一行（Shadow pass 之前） |
| `ForwardPipeline_FrameGraph.cpp` | `ForwardPipeline::BuildFrameGraph(...)` 函数体第一行 |
| `ForwardPipeline.cpp` | `ForwardPipeline::Render(...)` 的 `if (!m_UseRenderGraph)` 分支第一行（`:700` 附近，非 RG 路径） |
| `HybridRTPipeline.cpp` | `HybridRTPipeline::Render(...)` 函数体第一行（`CollectLights` `:532` 之前） |
| `PathTracingPipeline.cpp` | `PathTracingPipeline::Render(...)` 函数体第一行（`CollectLights` `:372` 之前） |

> 说明：同步必须发生在 Shadow 与 Lighting 收集**之前**，保证阴影贴图（CSM 的 `lightViewProj`）与光照使用同一太阳方向，避免一帧错位。函数幂等，多管线并存时重复调用无害。

- [ ] **Step 5: `02.Cube` 示例启用太阳同步**

`Samples/02.Cube/02.Cube.cpp` 方向光创建处（`:205` 附近，`mainDL->castShadow = true;` 之后）追加：

```cpp
        mainDL->syncWithPhysicalSky = true;   // 由物理天空太阳驱动方向与照度
```

- [ ] **Step 6: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（Scene 层新函数 + 5 处调用 + 示例字段）。

Run: `build/bin/Debug/02.Cube.exe`（Forward 模式 + `r.PhysicalSky.Enable 1`）
Expected: 天空太阳盘方向与方向光阴影方向一致（光照方向 = 太阳方向相反）；方向光切换到物理模式（illuminance>0）后亮度随 `sunIlluminance`/`sunIntensity` 变化。视觉确认由用户完成。若亮度整体过曝/过暗，调 `PhysicalSkyComponent::sunIlluminance`（`02.Cube.cpp:292` 附近）适配自动曝光参考白点。

- [ ] **Step 7: Commit（先向用户确认）**

```bash
git add Engine/Scene/Scene/PhysicalSkyComponent.h Engine/Scene/Scene/PhysicalSkyComponent.cpp Engine/Scene/Scene/LightComponent.h Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp Engine/Render/Pipeline/ForwardPipeline_FrameGraph.cpp Engine/Render/Pipeline/ForwardPipeline.cpp Engine/Render/Pipeline/HybridRTPipeline.cpp Engine/Render/Pipeline/PathTracingPipeline.cpp Samples/02.Cube/02.Cube.cpp
git commit -m "Render: 物理天空太阳→方向光 illuminance 自动同步"
```

---

### Task 2: HDR 输出全管线接入（Forward / HybridRT / PathTracing）

> 复用 Deferred 已落地的机制：`m_SwapChain->GetColorFormat()`（HDR 时返回 `A2B10G10R10_UNORM_PACK32`，否则 `BGRA8_UNORM`）+ `ToneMapPass::SetOutputFormat()/SetHDREnabled()`。LDR 后处理（FXAA）与 A2B10G10R10 后备缓冲不兼容，HDR 下需跳过。

**Files:**
- Modify: `Engine/Render/Pipeline/ForwardPipeline_FrameGraph.cpp`（RG 路径 `:216` 硬编码 → 交换链格式）
- Modify: `Engine/Render/Pipeline/HybridRTPipeline.cpp`（`:913/:930` 硬编码 + ToneMap 接线 + FXAA 门控）
- Modify: `Engine/Render/Pipeline/PathTracingPipeline.cpp`（`:562/:578` 硬编码 + ToneMap 接线 + FXAA 门控）
- Modify: `Samples/02.Cube/02.Cube.cpp`（`SwapChainDesc` 增加 HDR 开关 + 4 处 backbuffer `BeginRenderPass` 硬编码 → 交换链格式）

**Interfaces:**
- Consumes: `IRHISwapChain::GetColorFormat()`（`RHI/SwapChain.h:39`）、`ToneMapPass::SetOutputFormat()/SetHDREnabled()`（`PostProcess/ToneMapPass.h:46-48`）、`SwapChainDesc.hdr`（`RHI/SwapChain.h:17`）。
- Produces: 各管线 ToneMap 输出格式与交换链一致（HDR 时 `A2B10G10R10` + PQ，SDR 时 `BGRA8` + sRGB），FXAA 在 HDR 下自动关闭。

- [ ] **Step 1: `ForwardPipeline_FrameGraph.cpp` 解除 BGRA8 硬编码**

`BuildFrameGraph` 入口（`:40` 附近，`m_GI` 等成员读取前）新增交换链格式 + ToneMap 接线：

```cpp
    // 交换链颜色格式（SDR=BGRA8，HDR=A2B10G10R10），同步到 ToneMap 输出格式与 HDR 开关
    rhi::Format swapFmt = m_SwapChain ? m_SwapChain->GetColorFormat() : rhi::Format::BGRA8_UNORM;
    m_ToneMap->SetOutputFormat(swapFmt);
    m_ToneMap->SetHDREnabled(swapFmt == rhi::Format::A2B10G10R10_UNORM_PACK32);
```

`ToneMap` Pass（`:216`）的 `c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);` 改为：

```cpp
            c->BeginRenderPass(1, swapFmt);
```

- [ ] **Step 2: `HybridRTPipeline.cpp` 解除 BGRA8 硬编码 + FXAA 门控**

`BuildFrameGraph`/`Render` 入口（ToneMap 编排前）新增：

```cpp
    // 交换链颜色格式同步（HDR=A2B10G10R10，SDR=BGRA8）
    rhi::Format swapFmt = m_SwapChain ? m_SwapChain->GetColorFormat() : rhi::Format::BGRA8_UNORM;
    m_PostProcess.GetToneMap()->SetOutputFormat(swapFmt);
    m_PostProcess.GetToneMap()->SetHDREnabled(swapFmt == rhi::Format::A2B10G10R10_UNORM_PACK32);
    bool isHDR = (swapFmt == rhi::Format::A2B10G10R10_UNORM_PACK32);   // HDR 下禁用 LDR 的 FXAA
```

将 FXAA 的启用条件 `if (useFXAA)`（`:922`）改为 `if (useFXAA && !isHDR)`；`ToneMap` Pass 的 `c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);`（`:913`）与 FXAA 内的（`:930`）改为 `c->BeginRenderPass(1, swapFmt);`。

- [ ] **Step 3: `PathTracingPipeline.cpp` 解除 BGRA8 硬编码 + FXAA 门控**

同 Step 2：入口新增 `swapFmt`/`isHDR` 与 ToneMap 接线；`if (useFXAA && m_PostProcess.GetFXAA())`（`:570`）改为 `if (useFXAA && !isHDR && m_PostProcess.GetFXAA())`；`BeginRenderPass(1, BGRA8_UNORM)`（`:562`、`:578`）改为 `BeginRenderPass(1, swapFmt)`。

- [ ] **Step 4: `02.Cube` SwapChain 增加 HDR 开关 + 解除 backbuffer 硬编码**

SwapChain 创建处（`:157`）增加 CVar 开关（新增一个静态 CVar，仿照文件内既有 CVar 声明）：

```cpp
    // HDR 开关：开启时 SwapChain 优先选 A2B10G10R10 + HDR10 ST.2084（需显示器/扩展支持，否则自动回退 SDR）
    auto swapchain = device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width  = engine.GetWindow()->GetWidth(),
        .height = engine.GetWindow()->GetHeight(),
        .vsync  = true,
        .hdr    = cvHDR.Get() == 1,
    });
```

主循环中 4 处 backbuffer `BeginRenderPass(1, rhi::Format::BGRA8_UNORM[, ...])`（`:486`、`:494`、`:502`、`:510`）统一改为取交换链实际颜色格式：

```cpp
    rhi::Format backFmt = swapchain->GetColorFormat();  // 主循环开头取一次
    ...
    cmdList->BeginRenderPass(1, backFmt);                       // :486 Forward
    cmdList->BeginRenderPass(1, backFmt, rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);  // :494/:502/:510
```

- [ ] **Step 5: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（新增 swapFmt/isHDR 引用与 backFmt 变量）。

Run: `build/bin/Debug/02.Cube.exe`（默认 SDR，`cvHDR=0`）
Expected: 四种模式（Forward/Deferred/HybridRT/PathTracing）画面与改造前一致（无回归，swapFmt==BGRA8）。

Run: 切换 `cvHDR=1`（需 HDR10 显示器；不支持则回退 SDR 无崩溃）
Expected: Deferred 之外的三管线也输出 10-bit PQ；FXAA 自动关闭；无 shader 报错。视觉确认由用户完成。

- [ ] **Step 6: Commit（先向用户确认）**

```bash
git add Engine/Render/Pipeline/ForwardPipeline_FrameGraph.cpp Engine/Render/Pipeline/HybridRTPipeline.cpp Engine/Render/Pipeline/PathTracingPipeline.cpp Samples/02.Cube/02.Cube.cpp
git commit -m "Render: HDR 输出接入 Forward/HybridRT/PathTracing 管线"
```

---

### Task 3: 空中透视（大气消光 + 入射散射）

> 自包含的瑞利+米氏单散射消光，与 `PhysicalSky.frag.slang` 的 Preetham 模型共用浑浊度；只对**场景几何**（Deferred/HybridRT 的 DeferredLighting、Forward 的 PBR）生效，天空盒由 SkyboxPass 独立绘制不受影响。PathTracing 使用独立 RT 着色器，本轮不纳入。

**Files:**
- Create: `Engine/Shader/Shaders/Atmosphere.slang`（纯 include 头，无 entry point，**不加入** CMake）
- Modify: `Engine/Shader/Shaders/ShaderTypes.slang`（两个 push constant 结构末尾追加 `float4 atmosphere`）
- Modify: `Engine/Shader/Shaders/DeferredLighting.frag.slang`（cbuffer 追加字段 + 应用空中透视）
- Modify: `Engine/Shader/Shaders/PBR.frag.slang`（cbuffer 追加字段 + 应用空中透视）
- Modify: `Engine/Render/Pipeline/LightingPass.h`（`SetAtmosphere` 成员 + setter）
- Modify: `Engine/Render/Pipeline/LightingPass.cpp`（填充 push constant）
- Modify: `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp`（查询物理天空 → `SetAtmosphere`）
- Modify: `Engine/Render/Pipeline/HybridRTPipeline.cpp`（同上）
- Modify: `Engine/Render/Pipeline/ForwardPipeline.cpp`（`CollectLights` 填充 `pc.atmosphere`）

**Interfaces:**
- Consumes: `he::GetPhysicalSkySun(World&, float3&, float&)`（Task 1 产出）、`LightingPass::Render`（既有 push constant 填充点）、`PushConstantData`/`DeferredLightingPushConstant`（`ShaderTypes.slang`）。
- Produces: `Atmosphere.slang::ApplyAerialPerspective(inout float3 color, float3 camPos, float3 worldPos, float3 sunDir, float turbidity)`；`LightingPass::SetAtmosphere(float3, float)`。

- [ ] **Step 1: 新建 `Atmosphere.slang`（纯 include 头）**

```hlsl
// ============================================================
// Atmosphere.slang — 空中透视（大气消光 + 入射散射）
// 与 PhysicalSky.frag.slang 的 Preetham 模型共用浑浊度参数，
// 为场景对象提供基于距离 + 高度的瑞利/米氏单散射消光。
// ============================================================
#ifndef HUGENGINE_ATMOSPHERE_SLANG
#define HUGENGINE_ATMOSPHERE_SLANG

// 海平面散射系数（单位 1/m）
static const float3 kAerialBetaR = float3(5.802e-6, 13.558e-6, 33.100e-6); // 瑞利 λ⁻⁴
static const float  kAerialBetaM = 3.996e-6;                                // 米氏 λ⁰（×浑浊度）
static const float  kAerialScaleHeightR = 8000.0f;   // 瑞利标高（米）
static const float  kAerialScaleHeightM = 1200.0f;   // 米氏标高（米）

// 空中透视：对线性 HDR 颜色施加距离/高度相关大气消光 + 入射散射。
// camPos / worldPos 世界空间（单位米）；sunDir 指向太阳；turbidity 与 Preetham 一致。
// 高度 y 相对海平面（地面视为 y=0），低于海平面按 0 处理。
void ApplyAerialPerspective(inout float3 color,
                            float3 camPos, float3 worldPos,
                            float3 sunDir, float turbidity)
{
    if (turbidity <= 0.0) return;  // 浑浊度 <= 0 表示空中透视关闭（物理范围 1~10，0 为关闭哨兵）

    float dist = length(worldPos - camPos);
    if (dist < 1e-3) return;  // 距离过近无大气效应

    float camHeight = max(camPos.y, 0.0);
    float objHeight = max(worldPos.y, 0.0);

    // 平均密度 × 路径长度（单散射简化，忽略地球曲率）
    float optR = (exp(-camHeight / kAerialScaleHeightR) + exp(-objHeight / kAerialScaleHeightR)) * 0.5 * dist;
    float optM = (exp(-camHeight / kAerialScaleHeightM) + exp(-objHeight / kAerialScaleHeightM)) * 0.5 * dist;

    float3 betaR = kAerialBetaR;
    float3 betaM = float3(kAerialBetaM) * turbidity;
    float3 transmittance = exp(-(betaR * optR + betaM * optM));

    // 入射散射色：瑞利蓝天随太阳高度亮化、随浑浊度灰化
    float sunElevation = clamp(sunDir.y, 0.0, 1.0);
    float3 inscatter = float3(0.38, 0.52, 0.72);  // 地平线瑞利蓝
    inscatter = lerp(inscatter, float3(0.72, 0.72, 0.74), clamp(turbidity / 10.0, 0.0, 1.0));
    inscatter *= (0.6 + 1.4 * sunElevation);

    color = color * transmittance + inscatter * (1.0 - transmittance);
}

#endif // HUGENGINE_ATMOSPHERE_SLANG
```

- [ ] **Step 2: `ShaderTypes.slang` 两个 push constant 结构末尾追加 `float4 atmosphere`**

`DeferredLightingPushConstant`（`:205`）在 `rtDiffuseSource`（`:218`）之后追加：

```cpp
    float4   atmosphere;        // [64..80] sunDir.xyz（指向太阳）+ turbidity（空中透视）
```

`PushConstantData`（`:172`，Forward 用）在 `useClustered`（`:184`）之后追加：

```cpp
    float4   atmosphere;        // sunDir.xyz（指向太阳）+ turbidity（空中透视；C++ 自动 16B 对齐，结构从 128→144B）
```

- [ ] **Step 3: `DeferredLighting.frag.slang` 应用空中透视**

`#include "RT_DDGI.slang"`（`:4`）之后追加：

```hlsl
#include "Atmosphere.slang"
```

`LightingPC` cbuffer（`:57-71`）末尾（`rtDiffuseSource` 之后）追加字段：

```hlsl
    float4   atmosphere;        // sunDir.xyz（指向太阳）+ turbidity（空中透视）
```

`fragmentMain` 末尾 `color += gbC.rgb;`（`:281`）之后、`return float4(color, 1.0);`（`:282`）之前插入：

```hlsl
    // 空中透视：对场景几何应用距离/高度相关大气消光 + 入射散射
    ApplyAerialPerspective(color, cameraPosition.xyz, worldPos, atmosphere.xyz, atmosphere.w);
```

- [ ] **Step 4: `PBR.frag.slang` 应用空中透视**

`#include "pbr_common.slang"`（`:7`）之后追加：

```hlsl
#include "Atmosphere.slang"
```

`FrameConstants` cbuffer（`:68-81`）末尾（`useClustered` 之后）追加：

```hlsl
    float4   atmosphere;        // sunDir.xyz（指向太阳）+ turbidity（空中透视）
```

`fragmentMain` 末尾 `color += obj.emissiveFactor.rgb;`（`:228`）之后、`return float4(color, alpha);`（`:230`）之前插入：

```hlsl
    // 空中透视：对场景几何应用距离/高度相关大气消光 + 入射散射
    ApplyAerialPerspective(color, cameraPosition.xyz, input.worldPos, atmosphere.xyz, atmosphere.w);
```

- [ ] **Step 5: `LightingPass.h/.cpp` 新增 SetAtmosphere**

`LightingPass.h` 公开区（`SetIBLTextures` 声明 `:109` 之后）追加：

```cpp
    // 设置空中透视参数（太阳方向 + 浑浊度，来自 PhysicalSkyComponent）
    void SetAtmosphere(float3 sunDir, float turbidity);
```

成员区（`m_MSAAEnabled` 之后）追加：

```cpp
    float3 m_AtmSunDir    = float3(0, 1, 0);  // 空中透视太阳方向（默认朝天）
    float  m_AtmTurbidity = 0.0f;             // 空中透视浑浊度（0=关闭哨兵；物理范围 1~10）
```

`LightingPass.cpp` 中 `Render` 的 `lpc.rtDiffuseSource = ...;`（`:170`）之后追加：

```cpp
    lpc.atmosphere = float4(m_AtmSunDir, m_AtmTurbidity);
```

并在文件末尾（`SetIBLTextures` 之后、`namespace` 内）追加实现：

```cpp
void LightingPass::SetAtmosphere(float3 sunDir, float turbidity) {
    m_AtmSunDir    = sunDir;
    m_AtmTurbidity = turbidity;
}
```

- [ ] **Step 6: Deferred 与 HybridRT 查询物理天空并喂入 LightingPass**

`DeferredPipeline_FrameGraph.cpp` 与 `HybridRTPipeline.cpp` 的 Lighting pass 编排 lambda 前（二者均已有 `world` 可见）追加：

```cpp
    // 空中透视参数：从物理天空组件读取太阳方向 + 浑浊度（无物理天空时保持 0=关闭）
    float3 atmSunDir = float3(0, 1, 0); float atmTurbidity = 0.0f;
    if (he::GetPhysicalSkySun(world, atmSunDir, atmTurbidity))
        m_Lighting.SetAtmosphere(atmSunDir, atmTurbidity);
```

（两文件均需 `#include "Scene/PhysicalSkyComponent.h"`，Task 1 已加。）

- [ ] **Step 7: `ForwardPipeline.cpp` 填充 `pc.atmosphere`**

`CollectLights`（`:475`）开头（`pc.lightCount = 0;` 之后）追加：

```cpp
    // 空中透视参数：从物理天空组件读取太阳方向 + 浑浊度（无物理天空时保持 0=关闭）
    float3 atmSunDir = float3(0, 1, 0); float atmTurbidity = 0.0f;
    he::GetPhysicalSkySun(world, atmSunDir, atmTurbidity);
    pc.atmosphere = float4(atmSunDir, atmTurbidity);
```

（Forward 走 `CollectLights` 填充 push constant，RG 与非 RG 路径共用，无需额外接线。）

- [ ] **Step 8: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（`Atmosphere.slang` 被 DeferredLighting/PBR 引用编译；push constant 双源结构一致；`LightingPass`/`ForwardPipeline` 新成员与赋值）。

Run: `build/bin/Debug/04.Deferred.exe` 与 `build/bin/Debug/02.Cube.exe`（Forward 模式 + 物理天空开启）
Expected: 远处几何随距离向地平线天空色（瑞利蓝）过渡，近处基本不变；`turbidity` 越高消光越强、越灰。视觉确认由用户完成。

- [ ] **Step 9: Commit（先向用户确认）**

```bash
git add Engine/Shader/Shaders/Atmosphere.slang Engine/Shader/Shaders/ShaderTypes.slang Engine/Shader/Shaders/DeferredLighting.frag.slang Engine/Shader/Shaders/PBR.frag.slang Engine/Render/Pipeline/LightingPass.h Engine/Render/Pipeline/LightingPass.cpp Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp Engine/Render/Pipeline/HybridRTPipeline.cpp Engine/Render/Pipeline/ForwardPipeline.cpp
git commit -m "Render: 空中透视（瑞利+米氏大气消光）"
```

---

## 验证清单（全部完成后复验）

1. 全量编译 `cmake --build build --config Debug` 零错误。
2. 默认参数下 `02.Cube`（四模式）与 `04.Deferred` 渲染与改造前一致（HDR 关、无同步、无物理天空时空中透视 turbidity=0 自动关闭，无回归）。
3. 太阳同步后方向光阴影与太阳盘方向一致；HDR 开启后四管线均 10-bit PQ 输出；空中透视远处几何向蓝天过渡。
4. `HugEngine开发进度.md` 与 `基于物理的渲染管线剩余缺口.md` 由用户另行确认是否同步（本计划不含主文档同步）。
