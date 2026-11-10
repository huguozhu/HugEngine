# HugEngine on HarmonyOS NEXT（Vulkan）

把 `Samples/02.Cube` 的 **立方体 + 球体 + 天空盒 + 触摸相机** 子集搬到鸿蒙手机（HarmonyOS NEXT，arm64），渲染后端只用 **Vulkan**。

> 当前状态：**P3 代码完成（立方体 + 球体 + 地面 + 天空盒 + Forward 管线 + 触摸相机 + ImGui）**。
> `Samples/02.Cube/App/02.Cube.App.cpp` 与 ImGui 字体/后端改动都已在 Windows 侧
> （`02.Cube.Subset`）实跑验证；鸿蒙侧（XComponent + rawfile 解压 + napi 生命周期 +
> imgui_impl_ohos）已接线，**待真机验证**。
> 本机的验证范围只到 Windows：引擎侧平台层（IWindow / IInput / IVfs / VulkanPlatform）已在 Windows 上全量构建 + 实跑通过；
> 鸿蒙侧代码（`entry/src/main/cpp/*`、ArkTS）需要在 DevEco + NDK 环境里编译与真机验证。

---

## 1. 目录

```
Platforms/OHOS/
├── build-profile.json5 / oh-package.json5 / hvigorfile.ts   # 应用级工程配置
└── entry/
    ├── build-profile.json5            # 指定 native CMake 入口
    ├── src/main/module.json5          # Ability / deviceTypes / 页面
    ├── src/main/resources/…           # 图标、字符串、页面路由
    ├── src/main/ets/
    │   ├── entryability/EntryAbility.ets
    │   └── pages/Index.ets            # XComponent(surface) + 触摸转发
    └── src/main/cpp/
        ├── CMakeLists.txt             # 把仓库根的 HugEngine 作为子目录构建（只留 Vulkan）
        ├── NativeEntry.cpp            # napi: init/resize/touch/shutdown
        ├── AppMain.cpp                # 渲染线程：Engine → RHI → 三角形
        └── shaders/*.spv.h            # 预编译 SPIR-V（设备上不编译着色器）
```

引擎侧的平台抽象位于 `Engine/Core/Platform/`：

| 接口 | Windows | 鸿蒙 |
|---|---|---|
| `IWindow` | `WindowWin32`（GLFW） | `WindowOHOS`（XComponent 的 OHNativeWindow） |
| `IInput`（`PollInput`） | `InputWin32`（GLFW 轮询） | `InputOHOS`（触摸事件队列，单指=右键拖动） |
| `IVfs`（`Resolve`/`ResolveWritable`） | `Content/`（`HUGENGINE_CONTENT_DIR`） | 沙箱：资产 `filesDir/content/`、可写 `filesDir/` |
| Vulkan 表面 | `vkCreateWin32SurfaceKHR` | `vkCreateSurfaceOHOS`（`VK_OHOS_surface`） |

---

## 2. 前置条件

- DevEco Studio 5.0 及以上，HarmonyOS NEXT SDK（API 12+）
- 真机（arm64-v8a）开启开发者模式；模拟器不支持 Vulkan 时请用真机
- 仓库子模块已初始化：`git submodule update --init --recursive`
- 资产：`Content/` 下的 `gltf/`、`Textures/`、`Models/`（P2 会打进 `rawfile`）

---

## 3. 构建

### 3.1 推荐：DevEco Studio

1. 用 DevEco 打开 `Platforms/OHOS`（不是仓库根）
2. `File → Project Structure → Signing Configs` 勾选自动签名
3. 连接真机，点 Run

DevEco 会自动调用 `entry/src/main/cpp/CMakeLists.txt`，进而构建仓库根的引擎静态库 + `libentry.so`。

### 3.2 命令行（hvigor）

```bash
cd Platforms/OHOS
hvigorw clean --no-daemon
hvigorw assembleHap --mode module -p product=default --no-daemon
# 产物：entry/build/default/outputs/default/entry-default-signed.hap
hdc install entry/build/default/outputs/default/entry-default-signed.hap
```

### 3.3 只编 native（排查 CMake 问题）

```bash
cmake -S Platforms/OHOS/entry/src/main/cpp -B /tmp/hugengine-ohos \
      -DCMAKE_TOOLCHAIN_FILE=$HUGENGINE_ROOT/cmake/OHOS.toolchain.cmake \
      -DOHOS_NDK=$OHOS_SDK/native
cmake --build /tmp/hugengine-ohos -j
```

---

## 4. P2 验收（真机）

- 应用启动后出现：**地面 + 红色立方体 + 金色球体 + 天空盒**（Forward 管线 + 阴影）
- 单指拖动：旋转相机（触摸被映射为"右键拖动"）；双指暂未实现
- 日志（`hdc shell hilog | grep HugEngine`）出现：
  - `[OHOS] libentry.so 已加载（init/resize/touch/shutdown）`
  - `Vulkan 表面已创建（OHOS）`
  - `[OHOS] 渲染循环启动: WxH`
- 旋转屏幕：交换链跟随重建，画面不花
- 首次启动日志出现 `[OHOS] 解压 rawfile 'content' → '<沙箱>/content'`，之后再启动走 `沙箱资产已存在，跳过解压`
- 退出应用：日志出现 `[OHOS] 渲染循环退出`

> 注意：P1 只验证"surface → 交换链 → Present"。P2 才接 02 的场景与资产（rawfile 解压到沙箱）。

---

## 5. 已知风险 / 需要在真机 SDK 上核对

| 项 | 说明 |
|---|---|
| `OH_NativeWindow_CreateNativeWindowFromSurfaceId` | 较新 SDK 才有；老版本是 `OH_NativeWindow_CreateNativeWindow(void* surface)`。若编译失败按 SDK 头文件改 `NativeEntry.cpp` 一处即可 |
| `XComponentController.getXComponentSurfaceId()` | 需要 `XComponent` 传 `controller`；若 API 变动，改 `Index.ets` 的 `onLoad` 取 surface id |
| 系统库名 | `libace_napi.z.so` / `libhilog_ndk.z.so` / `libnative_window.so` 随 SDK 版本略有差异，见 `cpp/CMakeLists.txt` 的 `target_link_libraries` |
| `VK_OHOS_surface` | 需要 NDK 的 `<vulkan/vulkan_ohos.h>` 且设备驱动支持；`Engine/RHI/Vulkan/VulkanPlatform.cpp` 只在这一处使用它 |
| 验证层 | 设备上没有 `VK_LAYER_KHRONOS_validation`，`AppMain.cpp` 里固定 `enableValidation=false` |
| TBDR 性能 | 手机 GPU 是 tile-based；P2 只跑 Forward 管线，分辨率可在 `NativeEntry.cpp` 的 init 参数里调 |
| 字体（ImGui） | 随包只有 Roboto（无中文）：把 `NotoSansSC-Regular.otf` 放到 `entry/src/main/resources/rawfile/content/font/` 即可显示中文（`ImguiIntegration` 会优先尝试它） |

---

## 6. Windows 回归（每次改平台层都要跑）

```powershell
cmake --build build --config Debug
# 实跑（Vulkan）：应输出截图且不崩溃
cd build\bin\Debug
$env:HE_RHI_BACKEND='vulkan'; $env:HE_CAPTURE_FRAME='150'
.\01.Triangle.exe; .\02.Cube.exe
```

当前基线：01 ≈ 37 KB、02 ≈ 370 KB 截图，均无提前退出。

---

## 7. 鸿蒙侧代码的编译自检（不装 NDK 也能做）

鸿蒙分支的源码（`WindowOHOS` / `InputOHOS` / `imgui_impl_ohos` / `VulkanPlatform` 的 OHOS 分支 /
`NativeEntry` / `RawfileUnpack` / `AppMain`）平时在 Windows 构建里是**整段跳过**的，写错了也不会被发现。
`Platforms/OHOS/stubcheck` 用最小 stub 头 + `HE_PLATFORM_OHOS_STUBCHECK` 把这些文件在桌面编译器上真编一遍：

```powershell
cmake --build build --config Debug --target OhosStubCheck
```

- stub 头在 `Platforms/OHOS/stubcheck/include/`（`napi/native_api.h`、`native_window/external_window.h`、
  `rawfile/*`、`vulkan/vulkan_ohos.h`），只声明用到的符号；
- 该目标**只编译不链接**（`EXCLUDE_FROM_ALL`），因此不会影响正常构建与产物；
- 它已经抓出过几类真实问题：`u32/f64` 在全局命名空间未限定、`__attribute__((constructor))` 在 MSVC 下非法、
  OHOS 头文件里 `HE_PLATFORM_OHOS` 在 `#include "Core/Platform.h"` 之前使用导致整段被裁掉。

改了鸿蒙侧代码后，建议把它和 Windows 全量构建一起跑：

```powershell
cmake --build build --config Debug               # Windows 回归
cmake --build build --config Debug --target OhosStubCheck   # 鸿蒙分支语法自检
```
---

## 5.5 生命周期与旋转（native 侧已处理）

| 场景 | 行为 |
|---|---|
| 首次 surface 就绪 | `native.init` → `AttachNativeWindow` → `AppStart` 起渲染线程 |
| 旋转 / 分屏 | `native.resize(w,h)` → `OH_NativeWindow_NativeWindowHandleOpt(SET_BUFFER_GEOMETRY)` + 交换链重建 + 相机宽高比同步 |
| 切后台再回来（**新 surface**） | `native.init` 再次调用 → 检测到渲染线程仍在运行会**先停旧实例**（`AppStop`）→ 销毁旧 `OHNativeWindow` → 挂新句柄 → 重启；不会出现两个线程抢同一窗口 |
| surface 销毁 | `native.shutdown` → 停线程 → `OH_NativeWindow_DestroyNativeWindow` 释放句柄（避免每次重建泄漏一个原生窗口） |

`native.init` 是**幂等**的：重复调用只会重建实例，不会叠加线程或泄漏窗口。

## 5.6 排障清单（按现象）

| 现象 | 先查什么 |
|---|---|
| 启动即闪退/黑屏，日志没有 `Vulkan 表面已创建（OHOS）` | `surfaceId` 是否为空、`OH_NativeWindow_CreateNativeWindowFromSurfaceId` 是否返回 0；SDK 较老时改用 `OH_NativeWindow_CreateNativeWindow` |
| 有日志、有交换链，但画面全黑 | 先确认资产：日志里 `[02.App] 场景就绪: N 实体`；若 `N=0` 说明 rawfile 解压/路径不对（看 `[OHOS] 解压 rawfile ...` 与 `[VFS] 资产根`）。**相机位姿**也会造成"看起来黑"（样例退出时会保存配置） |
| 画面花屏/上下颠倒 | 检查 swapchain 的 `preTransform` 与 `SetFullscreenViewport` 是否一致；Vulkan 侧用负高度视口翻 Y |
| 旋转后画面拉伸 | `native.resize` 是否被 `onAreaChange` 调到；`SET_BUFFER_GEOMETRY` 与交换链宽高是否同一组值 |
| 触摸没反应 / UI 不响应 | `native.touch` 是否被调用（日志或临时加一行打印）；`InputOHOS` 只跟踪 `id==0` 的第一个手指 |
| 中文显示成方块 | `rawfile/content/font/NotoSansSC-Regular.otf` 是否存在（随包只有 Roboto，无中文） |
| 构建报找不到 `libace_napi.z.so` 等 | 系统库名随 SDK 版本变化，按 `cpp/CMakeLists.txt` 的注释逐个对齐 |
---

## 8. 真机实测记录（2026-11-09，HarmonyOS NEXT）

设备：**ALN-AL80**（Mate 60 Pro），GPU **Maleoon 910**，API 26；DevEco Studio 6.x + SDK `6.0.0(20)`。

已完成：`libentry.so`（54.5 MB debug / aarch64）→ 签名 hap（14.6 MB）→ 安装 → 启动 → 出画面
（地面 + 立方体 + 球体 + 天空盒 + 阴影 + ImGui），Vulkan 后端为 0。

### 8.1 真机上才暴露、桌面看不到的 5 个问题

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | 启动即 SIGSEGV（`spdlog::logger::should_log`，空指针） | `HE_CORE_*` 在 `Logger::Initialize()` 之前调用：① napi 模块 `Init`（`import 'libentry.so'` 时）② `NativeInit` 入口 | 两处都先 `he::Logger::Initialize()`；`Logger::Initialize` 改为幂等 |
| 2 | 同上，另一条路径 | `NativeEntry` 的 surfaceId 参数用了 `const char*` | 真实 SDK 是 `OH_NativeWindow_CreateNativeWindowFromSurfaceId(uint64_t, OHNativeWindow**)`，native 侧 `std::stoull` 转换 |
| 3 | 静态断言 `sizeof(GIBlendParams)==368` / `StaticVertex==48` 在 clang 下失败 | glm 的 aligned gentypes 只对 **MSVC 语言模式**启用（`GLM_LANG_CXXMS_FLAG`），clang 下 `vec3` 退化为 12 字节、`vec4/mat` 4 字节对齐 | `cmake/PatchDeps.cmake` 在配置期幂等修补 glm 对齐门限（MSVC 行为不变） |
| 4 | 立方体/球体被"打碎"成三角形碎片（阴影正常） | `PBR.vert` 的 SPIR-V 声明 location 0~4（含 joints/weights），非蒙皮管线只声明 0~2。桌面驱动把缺失属性读成 0；Maleoon 驱动报 `PROGRAM: The index of attributeMap is out of its size` 并把属性映射整体错位 | `ForwardPipeline` 的非蒙皮顶点布局补齐 location 3(`UInt4`)/4(`Float4`)，指向偏移 0（蒙皮分支不执行） |
| 5 | hvigor 报 `api version parameter is illegal` | `compatibleSdkVersion` 写成 `6.0.0(26)`（26 是 **native 包**版本，不是 API 版本） | 改成 `6.0.0(20)` |

### 8.2 仍需处理

- **ImGui 未按屏幕密度缩放**：1260×2506 的屏上 16px 字太小（需按 `densityPixels` 设 `io.FontGlobalScale` 或按 vp 布局）
- **材质颜色与桌面不一致**（球体青色、立方体淡紫，桌面是金色/红色）→ 疑与材质 SSBO 读取/纹理绑定有关，待排查
- **帧率 17 FPS（debug 构建）**：需用 `-p buildMode=release` 再测
- 触摸相机：`uinput` 语法受限未能自动验证，需手指实测
### 8.3 触摸与高 DPI（2026-11-09 第二轮修复）

| 现象 | 根因 | 修法 |
|---|---|---|
| ImGui 面板在手机上小到看不清（16px 字 / 1260px 屏） | 引擎没有内容缩放概念 | `IWindow::GetContentScale()`（Windows=1，鸿蒙取 `display.densityPixels`=3.25）；`native.setUiScale()` 从 ArkTS 传入；`ImGuiIntegration` 用 `ScaleAllSizes` + 按缩放字号栅格化 |
| 放大字号后 ImGui **整块消失** | 52px × 全 CJK 区段（2 万字形）图集超出纹理上限，字体纹理创建失败 | 放大时改用 `GetGlyphRangesChineseSimplifiedCommon()`（约 2500 字），图集可容纳 |
| 面板标题被裁 | ImGui 自动尺寸不把标题宽度算进最小宽度 | 样例按 `ImGui::GetFontSize()/16` 给初始尺寸 |
| 中文显示成 `??` | 随包只有 Roboto，无 CJK | 优先加载**设备自带** `/system/fonts/HarmonyOS_Sans_SC.ttf`（免分发、无授权问题） |
| 触摸完全没反应（`native.touch` 从未被调用） | `XComponent(SURFACE)` 自己消费触摸，其 `onTouch` 不触发 | `XComponent.hitTestBehavior(HitTestMode.None)`，改由外层 `Column.onTouch` 统一接收 |
| 触摸到了 native 但坐标恒为 (0,0) | ArkTS 传**归一化**坐标，`AppTouch` 又除了一次窗口尺寸 | `AppTouch` 按归一化坐标直接转发并 clamp 到 [0,1] |
| 手指 id 过滤掉事件 | native 只跟踪 `id==0`，而 ArkTS 的 `TouchObject.id` 不保证是 0 | ArkTS 侧统一传 `0`（第一根手指） |

验证方式（无需手指）：`hdc shell "uinput -T -g 300 1600 900 1300 600 1500"` 模拟单指拖拽，
对比前后截图可见视角变化；日志 `[OHOS] 首次收到触摸: id=0 phase=0 (0.238, 0.589)`。
### 8.4 01.Triangle 迁移与两个真机专属坑（2026-11-09 第三轮）

- 把 `01.Triangle` 的渲染部分抽成 `Samples/01.Triangle/01.Triangle.App.{h,cpp}`
  （与 `02.Cube.App` 同接口），桌面 `01.Triangle` 与鸿蒙入口共用。
- 2026-11-10 调整：桌面入口改成 `Main.Win32.cpp`（~15 行配置）+ 通用宿主
  `Platforms/Win32/DesktopHost.cpp`；`01.Triangle.Subset` 目标删除（`01.Triangle` 本身就用这份 App）。
- 页面顶部加了两个按钮切换样例：`native.start(1|2)` → 停旧实例、起新实例（XComponent 窗口不变）。

| 现象 | 根因 | 修法 |
|---|---|---|
| 启动即 SIGABRT（`vkEnumeratePhysicalDevices` / `vkGetPhysicalDeviceProperties`） | `vkCreateInstance` 返回 `VK_ERROR_LAYER_NOT_PRESENT(-6)`：手机上**没有** `VK_LAYER_KHRONOS_validation`，而 01 的 App 沿用桌面的"首次开校验层"；Release 下 `HE_ASSERT` 被裁掉，空 instance 继续往下走 | 引擎侧先 `vkEnumerateInstanceLayerProperties`，层存在才启用；`vkCreateInstance` 失败改为记错误并返回；01 的 App 在鸿蒙上直接关校验层 |
| 颜色整体红蓝颠倒（红三角形变蓝、金球变青、天空发黄） | 交换链把 `B8G8R8A8_UNORM` 写死，只在"表面确实报 BGRA8"时才覆盖；手机只报 `R8G8B8A8(37)`，驱动静默替换后与 PSO/RenderPass 声明的格式不一致 | `VulkanSwapChain` 改为**从表面报告的格式里选**（HDR10 > BGRA8 sRGB > RGBA8 sRGB > 第一个），并打印选用结果 |
| 面板里"渲/栅"显示成问号 | 高 DPI 下为压图集只用了 ImGui 常用字表（约 2500 字） | 用 `ImFontGlyphRangesBuilder` 在常用字表上补进本工程面板用字 |

真机结果：`01.Triangle` 红色三角形 + 旋转 + 面板（FPS ~118），`02.Cube` 金色球 + 红色立方体 + 天空盒（FPS ~21），
顶部按钮可来回切换。
### 8.6 样例平台代码收口（2026-11-10，以 01 为试点）

样例层只保留「场景与渲染」，平台相关的生命周期进独立模块：

| 层 | 位置 | 职责 |
|---|---|---|
| 样例（跨平台） | `Samples/<样例>/<样例>.App.{h,cpp}` | 场景、管线、面板，实现 `Samples/Common/App/IApp.h` |
| 桌面宿主 | `Platforms/Win32/DesktopHost.{h,cpp}` | Engine/窗口、主循环、输入、后端热切换、退出 |
| 鸿蒙宿主 | `Platforms/OHOS/entry/src/main/cpp/` | XComponent + napi + 生命周期（代码未变） |

`01.Triangle` 已按此迁移：桌面入口就在 `Samples/01.Triangle/01.Triangle.App.cpp` 末尾的
`HE_DESKTOP_ENTRY` 段里（只做配置 + `RunApp<App>(cfg)`）——

- 桌面 target 定义 `HE_DESKTOP_ENTRY=1` ⇒ 该 .cpp 编出 `main`；
- 鸿蒙 entry 不定义该宏 ⇒ 同一个 .cpp 编进 `libentry.so` 但不含 `main`，入口仍是 `AppMain.cpp`。

所以样例目录里只有一个 `.cpp`（App 与桌面入口同文件），没有单独的 `Main.Win32.cpp`。
02-04 仍走 `Samples/HMOS_App` + `0X.*.Subset` 的旧布局，后续按同一模式迁移。
