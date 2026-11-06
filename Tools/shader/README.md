# Tools/shader — 着色器构建期工具

## binding_audit.py — 绑定契约对账（Vulkan / DX12 / DX11）

### 为什么要这个工具

`Engine/Shader/Shaders/ShaderTypes/GPUBindings.slang` 里的 `kGPUBinding_*` 只描述
**Vulkan 的 (set, binding)**。D3D 两端的落点由编译器决定，而且不是同一组数字：

| | Vulkan | DX12 | DX11 |
|---|---|---|---|
| 编号来源 | `[[vk::binding(N, M)]]` 显式 | Slang **按声明顺序**自动分配 `register`，`space` 也被紧凑重排 | 同 DX12，但**没有 space** |
| 分组 | set | space + root signature 的 descriptor table | 每 stage 一张平坦槽位表 |
| 上限 | 实际不限 | root signature 32 个 range | b0–b13 / s0–s15 / u0–u7 / t0–t127 |

实测（`DeferredLighting.frag.slang`，本仓库真实数据）：

| 资源 | Vulkan binding | DX12 register |
|---|---:|---|
| `u_GBufferA` | 0 | t0（巧合） |
| `u_GBufferE` | 23 | **t4** |
| `u_RSMIndirect` | 5 | **t17** |
| `u_Lights` | 17 | **t18** |
| `u_Lumen` | 32 | **t23** |
| `GIBlendParamsCB` | 31 | b2 |

所以「一份源码三后端共用」成立的前提是：**D3D 侧按反射绑、不按 `kGPUBinding_*` 猜**。
本脚本就是这条前提的验收器。

### 用法

```powershell
# 全量：146 个入口 × 3 个后端
python Tools/shader/binding_audit.py

# 只跑某个 shader / 某个后端
python Tools/shader/binding_audit.py --only DeferredLighting
python Tools/shader/binding_audit.py --targets vk,dx12

# CMake 目标（不参与 ALL，需显式调用）
cmake --build Build --target AuditShaderBindings
```

| 参数 | 说明 |
|---|---|
| `--shaders <dir>` | Shaders 根目录，默认 `Engine/Shader/Shaders` |
| `--slangc <path>` | 默认按 `VULKAN_SDK` → `PATH` 查找 |
| `--targets vk,dx12,dx11` | 要跑的后端（`vk`=`-target spirv`，`dx12`=`-target hlsl`，`dx11`=`-target dxbc`） |
| `--only <substr>` / `--limit N` | 子集（调试用） |
| `--jobs N` | 并行编译数，默认 `CPU/2` |
| `--out <dir>` | 报告目录，默认 `Build/shader_audit` |
| `--no-rhi-mirror` | 跳过 `RHI/Types.h` 镜像常量比对 |

### 产出

| 文件 | 用途 |
|---|---|
| `contract.md` | 人读：三后端并排的表 + 问题清单 |
| `contract.csv` | 机读：同一份数据（UTF-8 with BOM，Excel 可直接开） |
| `summary.txt` | 计数：每个后端编译成功数、问题条数 |

退出码：`0` = 无契约问题；`1` = 有问题（可直接接 CI）。

### 检查项

1. **契约不一致**：源码 `[[vk::binding(常量, 常量)]]` 解析出的 `(set, binding)` 与
   Vulkan 反射结果不符（常量改了 shader 没同步、或常量拼错）；
2. **槽位重复**：同一 shader 里两个资源占用同一 `(set, binding)`。
   *例外*：纹理 + `SamplerState` 共用一槽是合法的（Vulkan 侧是 CombinedImageSampler，
   本仓库的 `u_GBufferA` + `u_GBufferSampler` 就是这样）；
3. **无显式 `vk::binding`**：资源在反射里有绑定、源码里却没有属性 → Slang 自动分配，
   跨编译器/版本不稳定；
4. **DX11 超限**：落点超出 `b0–b13 / s0–s15 / u0–u7 / t0–t127`；
5. **RHI 镜像漂移**：`Engine/RHI/RHI/Types.h` 的 `kBinding* / kDescSet* / kMax*`
   与共享契约的 `kGPU*` 数值不一致（这层原本没有任何编译期检查）。

### 当前已知结果（2026-11-06 全量跑）

- **Vulkan 列 0 问题**：所有入口的 `(set, binding)` 与契约一致，没有槽位冲突；
- **DX12 列**：全部可编，但 register/space 与 Vulkan 完全不同（见上表），
  后端必须按反射建根签名；
- **DX11 列**：RT / compute 之外的图形 shader 有一批编不过，当前两类典型错误：
  - `error X3502: 'fragmentMain': input parameter 'input_0' missing semantics`
  - `error X3503: 'vertexMain': function return value missing semantics`
  这是全屏三角形 VS/FS 的**接口语义**问题（Slang 编 SPIR-V 时不需要语义、按 location
  自动分配；fxc 编 DXBC 必须显式给 `TEXCOORD0` 之类），与绑定无关。DX11 后端接入前
  要先补这类语义，或按后端拆一套 VS/FS 签名。

### 局限

- 只对照到「每个 shader 的落点」，不校验 DX12 **根签名**本身（那需要后端代码/反射
  里的 root signature 定义）；
- 不判断某个 shader 是否**应该**进该后端（例如 DXR / bindless 在 DX11 上不存在）；
  这层需要后端自己的能力表；
- `-target dxbc` 由 Slang 内部调用 fxc，部分 HLSL 特性（如 `Texture2D[]` 无界数组）
  在 DX11 上不可用，会表现为编译失败而不是绑定问题。
