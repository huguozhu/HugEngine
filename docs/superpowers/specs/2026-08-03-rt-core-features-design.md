# HybridRT 核心功能补齐设计（软阴影 / 反射分级 / GI 回退）

> **日期**: 2026-08-03
> **状态**: 已批准
> **目标**: 完成混合 RayTracing 管线规划文档中的 3 项核心功能缺口：软阴影扩展、反射粗糙度分级（核心阈值跳过）、RT GI 超出范围回退 DDGI。

## 背景

规划文档（`docs/未实现功能/混合RayTracing管线规划.md`）中 5 项核心功能缺口，本次完成其中 3 项（透明阴影、NRD 集成因价值低/工作量大延后）：

1. **软阴影扩展**（4.4 节）：面光源随机采样软阴影，替代硬阴影
2. **反射粗糙度分级**（5.3 节）：高粗糙度像素不发射 RT 反射线，回退 IBL prefilter
3. **RT GI 回退 DDGI**（7.4 节）：射线 miss 时用 DDGI 探针查询，替代纯程序化天空色

所有新增参数接入现有 CVar 体系（`RTQualityCVars.h/cpp`），控制台热更新。

---

## 一、软阴影扩展

### 现状

- `RT_Shadow.rgen` 的 `TraceShadowRay` 每光源发射**单条**射线 → 硬阴影
- `ShadowLight.color_radius.w`（光源半径）在 `FillLightBuffer` 里只用于传 intensity，半径字段闲置
- `shadowFlags` bit1=半分辨率已用，bit2 空闲可作软阴影标志

### 数据链（CPU → GPU）

| 层 | 改动 |
|----|------|
| `LightComponent` | 新增 `float shadowRadius = 0.0f`（光源半径；0=硬阴影） |
| `GPULight`（ShaderTypes.slang） | `_padLight` 字段替换为 `float shadowRadius`（保持 64B 布局） |
| `CollectLights`（HybridRTPipeline.cpp） | 填充 `gl.shadowRadius = lc.shadowRadius` |
| `ShadowLight`（RT_Shadow.rgen）+ `ShadowLightGPU`（RTShadowPass.cpp） | 各加 `float4 pad_radius`（x=光源半径）→ 48B→64B，UB 大小同步 16×64B |
| `FillLightBuffer`（RTShadowPass.cpp） | 从 GPULight 拷贝 `shadowRadius` |

### Shader 逻辑

`RT_Shadow.rgen` 的 `TraceShadowRay` 改为面光源采样：

- `shadowFlags` bit2=软阴影时，对每个光源发射 `g_PC.spp` 条射线（`r.RT.Shadow.SPP`）
- 方向光（type 0）：半径作为角度锥，采样时在光锥内随机抖动方向
- 点/聚光（type 1/2）：半径作为光源球面半径，在面向 shade point 的圆盘上随机选发射点
- 汇总：`blockedCount / spp` → 软阴影值；SPP=1 时退化为硬阴影
- 采样点用现有 `Rand(idx, frame, s)` 确定性随机

### CVar

| CVar | 类型 | 默认 | 说明 |
|------|------|------|------|
| `r.RT.Shadow.Soft` | bool | false | 软阴影开关 |
| `r.RT.Shadow.SPP` | i32 | 4 | 软阴影每光源采样数（1=硬阴影） |

`RTShadowPass::Execute` 中 `pc.shadowFlags |= cvRTShadowSoft.Get() ? 2u<<1 : 0`（bit2）；`pc.spp = clamp(cvRTShadowSPP.Get(), 1, 16)`。

---

## 二、反射粗糙度分级（核心阈值跳过）

### 现状

- `RT_Reflection.rgen` 对每个像素发射一条反射射线（`roughness>0.02` 用 GGX 采样方向）
- 高粗糙度像素也发射线，浪费（粗糙面反射本可用 IBL prefilter 近似）

### 方案

按规划 5.3 最后一条规则（`粗糙度 > 0.6: 不发射 RT 光线，直接用 IBL prefilter`）：

- `RTRayEffectPushConstant`（ShaderTypes.slang）加 `float maxRoughness`（位于 sampleCount 后）
- `RT_Reflection.rgen` 主函数在发射前：
  ```hlsl
  if (roughness > g_PC.maxRoughness) {
      g_Output[idx] = float4(0, 0, 0, -1);  // 无效标记，不发射线
      return;
  }
  ```
- `DeferredLighting.frag` 反射叠加处加有效性守卫：
  ```hlsl
  float4 rtRefl = (rtSpecularSource != 0u) ? u_RT_Reflection.Sample(u_GBufferSampler, uv) : ssr;
  if (rtRefl.a >= 0.0) color += rtRefl.rgb;   // a<0（含高粗糙度跳过）时不叠加 RT 反射，IBL prefilter 已提供 specular
  ```
- `RTReflectionPass::Execute` 写 `pc.maxRoughness = cvRTReflectionMaxRoughness.Get()`

**范围说明**：规划 5.3 完整版是"全/半/四分之一 3-pass 自适应分辨率"，本次实现其性能收益核心（粗糙度阈值跳过 + IBL 回退）。多分辨率分级留作后续增强。

### CVar

| CVar | 类型 | 默认 | 说明 |
|------|------|------|------|
| `r.RT.Reflection.MaxRoughness` | f32 | 0.6 | 反射最大粗糙度，超过用 IBL prefilter（不发射 RT 线） |

---

## 三、RT GI 回退 DDGI

### 现状

- `RT_GI.rgen` 射线 miss 时由 `RT_GI.rmiss` 返回**程序化天空色**（`lerp` 天顶/地平线）
- DDGI 探针在 Lighting 独立叠加，非规划要求的"miss 回退探针查询"

### 方案

1. **提取 SampleDDGI 到共享 shader**：把 `DeferredLighting.frag` 内的 `SampleDDGI` + 探针常量（`kDDGI_GridX/Y/Z`、`CellSize`、`Origin`、`Stride`、SH 系数）+ `EvalDDGI_SH` 提取到新文件 `Engine/Shader/Shaders/RT_DDGI.slang`。**该文件只含纯函数 + 常量，不声明纹理/SSBO 绑定**（`SampleDDGI` 引用全局 `u_DDGIProbes`，由各 shader 各自声明该全局）。`DeferredLighting.frag` 改为 include 并删除本地副本（保持行为不变）。
2. **RT_GI 绑定探针**：`RT_GI` set0 追加 binding 7 = DDGIProbes SSBO（`GI_DDGI::GetProbeBuffer()`）。`RT_GI.rgen` 内声明 `[[vk::binding(7, 0)]] StructuredBuffer<float4> u_DDGIProbes;` 并 include `RT_DDGI.slang`。`DeferredLighting.frag` 保持其 binding 22 声明（两 shader 各自声明，绑定号互不冲突）。
3. **miss 回退**：`RT_GI.rgen` 循环内，射线 miss（`payload.radianceT.a < 0`）时：
   ```hlsl
   radiance += SampleDDGI(worldPos, dir);   // 探针 SH 按出射方向评估（远距离低频光）
   ```
   替代纯天空色（天空色仅在探针区外仍由 rmiss 提供，但 miss 回退探针更物理准确）。

### CVar

无新增（探针网格参数由 `GI_DDGI` 配置，与 Lighting 共用）。

---

## 四、CVar 汇总（新增 3 个）

| CVar | 类型 | 默认 | 功能 |
|------|------|------|------|
| `r.RT.Shadow.Soft` | bool | false | 软阴影开关 |
| `r.RT.Shadow.SPP` | i32 | 4 | 软阴影采样数 |
| `r.RT.Reflection.MaxRoughness` | f32 | 0.6 | 反射粗糙度阈值（超过用 IBL） |

---

## 五、文件变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `Engine/Scene/Scene/LightComponent.h` | 修改 | `DirectionalLight`/`PointLight`/`SpotLight` 加 `shadowRadius` |
| `Engine/Shader/Shaders/ShaderTypes.slang` | 修改 | GPULight `_padLight`→`shadowRadius`；RTShadowPushConstant 加 `spp`+`softFlag`；RTRayEffectPushConstant 加 `maxRoughness` |
| `Engine/Shader/Shaders/RT_Shadow.rgen.slang` | 修改 | 面光源采样软阴影 |
| `Engine/Shader/Shaders/RT_Reflection.rgen.slang` | 修改 | 粗糙度阈值跳过 |
| `Engine/Shader/Shaders/DeferredLighting.frag.slang` | 修改 | include RT_DDGI.slang 移除本地 SampleDDGI；反射 a<0 守卫 |
| `Engine/Shader/Shaders/RT_DDGI.slang` | 新建 | 共享 SampleDDGI + 探针常量 + 探针绑定声明 |
| `Engine/Shader/Shaders/RT_GI.rgen.slang` | 修改 | miss 回退探针查询 |
| `Engine/Render/RT/RTShadowPass.cpp` | 修改 | ShadowLightGPU 加半径、UB 大小、FillLightBuffer、Execute 写 soft/spp |
| `Engine/Render/RT/RTReflectionPass.cpp` | 修改 | Execute 写 maxRoughness |
| `Engine/Render/RT/RTGIPass.cpp` | 修改 | set0 加 DDGIProbes 绑定（b7） |
| `Engine/Render/Pipeline/RTQualityCVars.cpp` | 修改 | 注册 3 个新 CVar |
| `Engine/Render/Pipeline/RTQualityCVars.h` | 修改 | 声明 3 个新 CVar |
| `Engine/Render/Pipeline/HybridRTPipeline.cpp` | 修改 | CollectLights 填 shadowRadius |

---

## 六、验证

1. 编译通过（02.Cube + 全部 RT shader 重新编译）
2. 运行 02.Cube：
   - 控制台 `set r.RT.Shadow.Soft 1` + `set r.RT.Shadow.SPP 8` → 阴影边缘变软（半影）
   - `set r.RT.Reflection.MaxRoughness 0.3` → 粗糙球反射消失（IBL 回退），GPU 时间下降
   - `set r.RT.GI.MaxDistance 5` → 5m 外 GI 回退 DDGI 探针，远距离仍有关键光（非天空色）
3. 关闭软阴影（SPP=1）回归硬阴影；反射 a<0 守卫不影响镜面反射
4. 确认 Deferred 管线无回归（SampleDDGI 提取后行为一致）
