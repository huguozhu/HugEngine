# HybridRT 核心功能补齐实施计划（软阴影 / 反射分级 / GI 回退）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 完成混合 RayTracing 管线规划的 3 项核心功能：软阴影（面光源采样）、反射粗糙度分级（阈值跳过 + IBL 回退）、RT GI miss 回退 DDGI 探针。

**Architecture:** 软阴影贯穿 CPU→GPU 数据链（LightComponent.shadowRadius → GPULight → ShadowLight → RT_Shadow.rgen 面光源采样）；反射分级在 RTRayEffectPushConstant 加 maxRoughness + rgen 阈值跳过 + Lighting 有效性守卫；GI 回退提取 SampleDDGI 到共享 RT_DDGI.slang + RT_GI set0 绑定探针 + miss 回退查询。全部接入现有 CVar 体系热更新。

**Tech Stack:** C++ / Vulkan / Slang / CMake

## Global Constraints

- 编译命令：`cmake --build build --target 02.Cube --config Debug`（RT shader 由 CMake/slangc 自动重编译）
- 验证 = 编译 + 运行（无单测框架）
- 新代码注释用中文（CLAUDE.md）
- **commit 需用户确认后执行**；但本计划按 SDD 流程每任务 commit（用户已批准该模式）
- `GPU_STRUCT`/`GPU_UINT` 宏在 `ShaderTypes.slang`：C++ 侧 `struct alignas(16)`，Slang 侧 `struct`；C++ 经 `#include "Pipeline/Material.h"` 引入共享结构
- push constant 结构改动后，C++ 与 Slang 两侧同步，且各 RT Pass Initialize 的 `pc.size` 需覆盖新结构
- 新 CVar 定义在 `Engine/Render/Pipeline/RTQualityCVars.h/cpp`

---

### Task 1: 注册 3 个新 CVar

**Files:**
- Modify: `Engine/Render/Pipeline/RTQualityCVars.h`
- Modify: `Engine/Render/Pipeline/RTQualityCVars.cpp`

**Interfaces:**
- Produces: `he::render::cvRTShadowSoft`（`CVar<bool>`）、`cvRTShadowSPP`（`CVar<i32>`）、`cvRTReflectionMaxRoughness`（`CVar<float>`）。后续任务 include `"Pipeline/RTQualityCVars.h"` 访问。

- [ ] **Step 1: RTQualityCVars.h 加声明**

在降噪区之前（采样/追踪区附近）追加：
```cpp
// 软阴影 / 反射分级
extern CVar<bool>  cvRTShadowSoft;         // r.RT.Shadow.Soft          软阴影开关
extern CVar<i32>   cvRTShadowSPP;          // r.RT.Shadow.SPP           软阴影每光源采样数（1=硬阴影）
extern CVar<float> cvRTReflectionMaxRoughness; // r.RT.Reflection.MaxRoughness 反射最大粗糙度（超过用 IBL）
```

- [ ] **Step 2: RTQualityCVars.cpp 加定义**

```cpp
// 软阴影 / 反射分级
CVar<bool>  cvRTShadowSoft("r.RT.Shadow.Soft", false, "RT 软阴影开关");
CVar<i32>   cvRTShadowSPP("r.RT.Shadow.SPP", 4, "RT 软阴影每光源采样数（1=硬阴影）");
CVar<float> cvRTReflectionMaxRoughness("r.RT.Reflection.MaxRoughness", 0.6f, "RT 反射最大粗糙度，超过用 IBL prefilter");
```

- [ ] **Step 3: 编译验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无错误。

- [ ] **Step 4: Commit**（中文消息，如 "注册软阴影/反射分级 CVar"）

---

### Task 2: 软阴影数据链（CPU → GPU 结构）

**Files:**
- Modify: `Engine/Scene/Scene/LightComponent.h`
- Modify: `Engine/Shader/Shaders/ShaderTypes.slang`（GPULight + RTShadowPushConstant）
- Modify: `Engine/Render/Pipeline/HybridRTPipeline.cpp`（CollectLights）
- Modify: `Engine/Render/RT/RTShadowPass.cpp`（ShadowLightGPU + UB + FillLightBuffer + Execute）

**Interfaces:**
- Consumes: Task 1 的 `cvRTShadowSoft`/`cvRTShadowSPP`
- Produces: `LightComponent::shadowRadius`（float，默认 0）；`GPULight::shadowRadius`（float，替换 `_padLight`，保持 64B）；`RTShadowPushConstant::softSPP`（GPU_UINT）+ `shadowFlags` bit2=软阴影；`ShadowLightGPU` 64B 布局

- [ ] **Step 1: LightComponent.h 加光源半径**

基类 `LightComponent` 加字段（约在 color/intensity 声明处）：
```cpp
    float shadowRadius = 0.0f;   // 光源半径（软阴影用；0=硬阴影）
```

- [ ] **Step 2: ShaderTypes.slang 改 GPULight + RTShadowPushConstant**

`GPULight` 的 `GPU_INT _padLight;` 改为：
```slang
    float  shadowRadius;      // 光源半径（软阴影用，0=硬阴影）
```
（保持 64B：16×3 + 8 + 4 + 4 = 64）

`RTShadowPushConstant` 尾部（`lightCount` 后）追加：
```slang
    GPU_UINT softSPP;         // 软阴影每光源采样数
```
并把 `shadowFlags` 注释改为 `// bit0=硬阴影, bit1=半分辨率, bit2=软阴影`。

- [ ] **Step 3: CollectLights 填充 radius**

`HybridRTPipeline.cpp` 的 `CollectLights` 中，`GPULight gl{};` 初始化后（约 `gl.shadowIndex = -1;` 附近）加：
```cpp
        gl.shadowRadius = lc.shadowRadius;  // 光源半径（软阴影）
```

- [ ] **Step 4: RTShadowPass.cpp 改 ShadowLightGPU + UB + FillLightBuffer**

`ShadowLightGPU` 结构尾部追加（48B→64B）：
```cpp
    float4 radius;            // x=光源半径（软阴影）
```

`FillLightBuffer` 拷贝（`sl.spotDir_angle = ...` 后）：
```cpp
        sl.radius.x = src[i].shadowRadius;  // 光源半径（软阴影）
```

- [ ] **Step 5: RTShadowPass::Execute 写软阴影参数**

`RTShadowPushConstant pc{};` 填充处加：
```cpp
    pc.softSPP    = clamp(cvRTShadowSPP.Get(), 1, 16);          // 软阴影采样数
    pc.shadowFlags = (m_HalfRes ? 2u : 0u)
                   | (cvRTShadowSoft.Get() ? 4u : 0u);          // bit1=半分辨率, bit2=软阴影
```

- [ ] **Step 6: 编译验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无错误。

- [ ] **Step 7: Commit**（中文消息，如 "软阴影数据链：光源半径贯穿 CPU→GPU 共享结构"）

---

### Task 3: 软阴影 shader（面光源采样）

**Files:**
- Modify: `Engine/Shader/Shaders/RT_Shadow.rgen.slang`

**Interfaces:**
- Consumes: Task 2 的 `ShadowLight.radius.x`、`RTShadowPushConstant::softSPP` + `shadowFlags` bit2

- [ ] **Step 1: TraceShadowRay 支持软阴影**

把 `RT_Shadow.rgen` 的 `TraceShadowRay(float3 worldPos, float3 N, ShadowLight light)` 改为带软阴影参数的重载，主函数里调用软阴影版本。核心逻辑：

```hlsl
// 软阴影：光源面上采样 spp 个发射点，发射 spp 条射线平均（硬阴影 spp<=1 时退化）
float TraceShadowRaySoft(float3 worldPos, float3 N, ShadowLight light, uint spp) {
    float3 L; float tMax;
    if (light.pos_type.w == 0.0) { L = normalize(light.pos_type.xyz); tMax = 10000.0; }
    else { float3 lp = light.pos_type.xyz; float3 toL = lp - worldPos; float dist = length(toL); L = toL / dist; tMax = dist; }

    float shadow = 0.0;
    for (uint s = 0; s < spp; s++) {
        float3 dir = L;
        if (light.pos_type.w == 0.0) {
            // 方向光：光锥内按角度半径抖动（radius.x 弧度）
            float3 up = abs(L.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
            float3 T = normalize(cross(up, L));
            float3 B = cross(L, T);
            float  ang = light.radius.x * sqrt(Rand(idx, g_PC.frameIdx, s*2));
            float  phi = 6.2831853 * Rand(idx, g_PC.frameIdx, s*2+1);
            dir = normalize(L + (T*cos(phi) + B*sin(phi)) * tan(ang));
        } else {
            // 点/聚光：面向 shade point 的圆盘上采样发射点（半径 radius.x）
            float3 toL = normalize(light.pos_type.xyz - worldPos);
            float3 up = abs(toL.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
            float3 T = normalize(cross(up, toL));
            float3 B = cross(toL, T);
            float r = light.radius.x * sqrt(Rand(idx, g_PC.frameIdx, s*2));
            float phi = 6.2831853 * Rand(idx, g_PC.frameIdx, s*2+1);
            float3 offset = (T*cos(phi) + B*sin(phi)) * r;
            float3 lp = light.pos_type.xyz + offset;   // 采样后的光源发射点
            float3 toP = lp - worldPos; float dist = length(toP);
            dir = toP / dist; tMax = dist;
        }
        RayDesc ray;
        ray.Origin = worldPos + N * 0.01;
        ray.Direction = dir;
        ray.TMin = 0.01;
        ray.TMax = tMax - 0.01;
        ShadowPayload payload; payload.blocked = false;
        TraceRay(g_TLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                 0xFF, 0, 0, 0, ray, payload);
        shadow += payload.blocked ? 0.0 : 1.0;
    }
    return shadow / float(spp);
}
```

- [ ] **Step 2: 主函数用软阴影**

`main()` 的光源循环内，`shadow *= TraceShadowRay(...)` 改为：
```hlsl
        bool  soft = (g_PC.shadowFlags & 4u) != 0u;   // bit2=软阴影
        uint  spp  = soft ? g_PC.softSPP : 1u;
        shadow *= TraceShadowRaySoft(worldPos, N, light, spp);
```

- [ ] **Step 3: 编译验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无错误（slangc 重编译 RT_Shadow.rgen）。

- [ ] **Step 4: Commit**（中文消息，如 "RT 阴影面光源采样实现软阴影"）

---

### Task 4: 反射粗糙度分级

**Files:**
- Modify: `Engine/Shader/Shaders/ShaderTypes.slang`（RTRayEffectPushConstant 加 maxRoughness）
- Modify: `Engine/Shader/Shaders/RT_Reflection.rgen.slang`（阈值跳过）
- Modify: `Engine/Shader/Shaders/DeferredLighting.frag.slang`（a<0 守卫）
- Modify: `Engine/Render/RT/RTReflectionPass.cpp`（Execute 写 maxRoughness）

**Interfaces:**
- Consumes: Task 1 的 `cvRTReflectionMaxRoughness`
- Produces: `RTRayEffectPushConstant::maxRoughness`（float）

- [ ] **Step 1: ShaderTypes.slang 加 maxRoughness**

`RTRayEffectPushConstant` 的 `sampleCount` 后追加：
```slang
    float    maxRoughness;        // 反射最大粗糙度（超过回退 IBL prefilter）
```

- [ ] **Step 2: RT_Reflection.rgen 阈值跳过**

`main()` 中读取 `float roughness = gbB.w;` 后、发射射线前加：
```hlsl
    // 粗糙度超过阈值：不发射 RT 反射线，回退 IBL prefilter（a=-1 标记无效）
    if (roughness > g_PC.maxRoughness) {
        g_Output[idx] = float4(0, 0, 0, -1);
        return;
    }
```

- [ ] **Step 3: DeferredLighting.frag 反射有效性守卫**

`float4 rtRefl = ...; color += rtRefl.rgb;` 改为：
```hlsl
    float4 rtRefl = (rtSpecularSource != 0u) ? u_RT_Reflection.Sample(u_GBufferSampler, uv) : ssr;
    // a<0（高粗糙度跳过或 miss）不叠加 RT 反射，specular IBL prefilter 已提供回退
    if (rtRefl.a >= 0.0) color += rtRefl.rgb;
```

- [ ] **Step 4: RTReflectionPass::Execute 写 maxRoughness**

`pc.sampleCount = ...` 附近加：
```cpp
    pc.maxRoughness = max(cvRTReflectionMaxRoughness.Get(), 0.01f);  // 反射最大粗糙度（超过用 IBL）
```

- [ ] **Step 5: 编译验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无错误。**注意**：若 RTRayEffectPushConstant 结构变大导致 push constant range 不足，需同步增大 `RTReflectionPass::Initialize` 的 `pc.size`（当前 128）以覆盖新结构。

- [ ] **Step 6: Commit**（中文消息，如 "RT 反射粗糙度分级：高粗糙度回退 IBL prefilter"）

---

### Task 5: RT GI 回退 DDGI

**Files:**
- Create: `Engine/Shader/Shaders/RT_DDGI.slang`
- Modify: `Engine/Shader/Shaders/DeferredLighting.frag.slang`（include + 删本地 SampleDDGI）
- Modify: `Engine/Shader/Shaders/RT_GI.rgen.slang`（include + 声明 u_DDGIProbes + miss 回退）
- Modify: `Engine/Render/RT/RTGIPass.cpp`（set0 加 b7 + Execute 绑定）
- Modify: `Engine/Render/RT/RTEffectPass.h`（RTExecuteContext 加 ddgiProbeBuffer）
- Modify: `Engine/Render/Pipeline/HybridRTPipeline.cpp`（RT_GI lambda 传 ddgiProbeBuffer）

**Interfaces:**
- Produces: `RT_DDGI.slang` 纯函数 `SampleDDGI(float3 worldPos, float3 normal)` + `EvalDDGI_SH` + 探针常量（无绑定声明）；`RTExecuteContext::ddgiProbeBuffer`

- [ ] **Step 1: 新建 RT_DDGI.slang**

从 `DeferredLighting.frag.slang` 提取（探针常量 + `EvalDDGI_SH` + `SampleDDGI`，**不含** `u_DDGIProbes` 绑定声明——由各 shader 自声明）：
```slang
// RT_DDGI.slang — DDGI 探针查询共享库（纯函数 + 常量，无资源绑定）
// 由 DeferredLighting.frag（binding 22）与 RT_GI.rgen（binding 7）各自声明
// [[vk::binding(N, 0)]] StructuredBuffer<float4> u_DDGIProbes 后 include 本文件使用。

// ---- DDGI 探针网格参数（与 GI_DDGI 默认值保持同步）----
static const uint   kDDGI_GridX    = 8;
static const uint   kDDGI_GridY    = 4;
static const uint   kDDGI_GridZ    = 8;
static const float  kDDGI_CellSize = 3.0;
static const float3 kDDGI_Origin   = float3(-10.0, -2.0, -10.0);
static const uint   kDDGI_Stride   = 16;

// SH 基函数常量
static const float kDDGI_SH_Y00 = 0.28209479177387814;
static const float kDDGI_SH_B1  = 0.4886025119029199;

// 在方向 dir 处评估 9 系数 SH 的辐照度（bands 0/1/2 完整版）
float3 EvalDDGI_SH(float4 sh[9], float3 dir) { /* 从 DeferredLighting.frag 原样复制 */ }

// 三线性插值采样 DDGI 探针网格，返回间接漫反射辐照度
float3 SampleDDGI(float3 worldPos, float3 normal) { /* 从 DeferredLighting.frag 原样复制 */ }
```

- [ ] **Step 2: DeferredLighting.frag 改用共享库**

- 删除本地 `kDDGI_GridX` 到 `SampleDDGI` 的全部定义（第 71-145 行）
- 保留 `u_DDGIProbes` 绑定声明（binding 22）
- 文件顶部加 `#include "RT_DDGI.slang"`

- [ ] **Step 3: RT_GI set0 加探针绑定**

`RTGIPass.cpp` 的 bindings 数组追加：
```cpp
        {7, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskRayGen},  // DDGI 探针
```
`Execute()` 里描述符更新处加：
```cpp
    if (ctx.ddgiProbeBuffer)
        m_Device->UpdateDescriptorSet(m_RayGenSet, 7,
            rhi::DescriptorType::StorageBuffer, ctx.ddgiProbeBuffer);
```

- [ ] **Step 4: RTExecuteContext 加字段**

`Engine/Render/RT/RTEffectPass.h` 的 `RTExecuteContext` 加：
```cpp
    rhi::IRHIBuffer* ddgiProbeBuffer = nullptr;  // DDGI 探针 SSBO（GI miss 回退用）
```

- [ ] **Step 5: HybridRTPipeline RT_GI lambda 传探针**

`BuildFrameGraph` 的 RT_GI lambda 中 `ctx.sceneTriangleNormals = ...` 后加：
```cpp
                ctx.ddgiProbeBuffer = m_DDGI.GetProbeBuffer();  // DDGI 探针（GI miss 回退）
```

- [ ] **Step 6: RT_GI.rgen miss 回退探针**

文件顶部加 `#include "RT_DDGI.slang"`，并在 RT_GI 资源区声明：
```slang
[[vk::binding(7, 0)]] StructuredBuffer<float4> u_DDGIProbes;  // DDGI 探针（miss 回退）
```
主函数循环内射线 TraceRay 后：
```hlsl
        radiance += payload.radianceT.rgb;
        // miss（a<0）回退 DDGI 探针查询（远距离低频间接光），替代纯天空色
        if (payload.radianceT.a < 0.0)
            radiance += SampleDDGI(worldPos, dir);
```

- [ ] **Step 7: 编译验证**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
```
Expected: 无错误。

- [ ] **Step 8: Commit**（中文消息，如 "RT GI miss 回退 DDGI 探针查询"）

---

### Task 6: 集成运行验证

**Files:** 无代码改动（验证任务）

**Interfaces:**
- Consumes: Task 1-5 全部

- [ ] **Step 1: 全量编译 + 启动**

```bash
cd /d/Source/HugEngine/build && cmake --build . --target 02.Cube --config Debug 2>&1 | grep -iE "error|LNK|fatal"
cd bin/Debug && ./02.Cube.exe &
```
确认：无编译错误；应用初始化（HybridRT 管线 + RT Passes + 降噪器）正常；运行数秒无崩溃后关闭。

- [ ] **Step 2: 记录启动日志关键行**

从日志确认：`RTShadowPass/RTAOPass/RTReflectionPass/RTGIPass 初始化完成`、无 `fatal/assert`。

- [ ] **Step 3: 汇报**

说明：控制台 `set r.RT.Shadow.Soft 1`、`set r.RT.Shadow.SPP 8`、`set r.RT.Reflection.MaxRoughness 0.3`、`set r.RT.GI.MaxDistance 5` 的视觉验证需用户手动（02.Cube 的 `~` 控制台为交互式）。

- [ ] **Step 4: Commit**（无代码改动则跳过；如验证中发现小修则并入）

---

## 自审查

**Spec 覆盖检查：**
- 软阴影数据链（LightComponent→GPULight→ShadowLight→FillLightBuffer）→ Task 2 ✓
- 软阴影面光源采样 shader + soft/spp 参数 → Task 3 ✓
- 反射 maxRoughness + 阈值跳过 + Lighting a<0 守卫 → Task 4 ✓
- RT_DDGI.slang 提取（纯函数无绑定）→ Task 5 ✓
- RT_GI set0 探针绑定 + miss 回退 → Task 5 ✓
- 3 个新 CVar → Task 1 ✓
- 运行验证 → Task 6 ✓

**类型一致性：**
- `cvRTShadowSoft`/`cvRTShadowSPP`/`cvRTReflectionMaxRoughness` 在 Task 1 定义、Task 2/4 引用，签名一致 ✓
- `ShadowLight.radius.x` 在 Task 2 数据链填充、Task 3 shader 读取，一致 ✓
- `RTRayEffectPushConstant::maxRoughness` 在 Task 4 定义（ShaderTypes）与写入（RTReflectionPass）一致 ✓
- `RTExecuteContext::ddgiProbeBuffer` 在 Task 5 定义（RTEffectPass.h）、填充（HybridRT）、使用（RTGIPass）一致 ✓

**注：**
- push constant 结构变大后，各 Pass Initialize 的 `pc.size` 需覆盖；Task 2/4 已提示，实现时如编译报 push constant 越界需同步增大。
- DeferredLighting.frag 的 SampleDDGI 提取必须行为一致（原样复制），避免 Deferred 管线回归。
