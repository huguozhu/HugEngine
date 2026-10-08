#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include "Pipeline/Camera.h"

#include <cstddef>     // offsetof
#include <cstring>
#include <type_traits>
#include <vector>

// ============================================================
// FrameSceneSnapshot —— 游戏线程产出的**不可变渲染输入**（方案 B §4.2 / 阶段 1 任务 T1.1）
//
// 【它解决什么问题】渲染侧现在直接读 ECS：`DeferredPipeline::CollectLights(pc, world, sg, camera)`、
// `GPUScene::Collect(world, sg, camera)`、`ForwardPipeline::CollectLights(...)`。一旦渲染跑到另一根
// 线程，这些遍历就会与游戏线程的 tick 竞争（方案 §2 的 C1，本方案最大的障碍）。
// 快照把"渲染真正需要的输入"在**游戏线程 tick 结束后**一次取齐，交接后视为只读。
//
// 【两条硬约束】
//   1. **只放值，不放指针**：不能出现 `Entity` / 组件指针 / `World*` / `SceneGraph*`，
//      否则"不可变"就无从谈起（渲染线程仍会顺着指针读世界）。需要资源时放**索引/ID**，
//      由渲染线程自己查表（`meshIndex` / `materialID` 就是这个用途）。
//   2. **布局与着色器一致**：能直接 memcpy 进 SSBO 的，就不要在快照里再抄一份字段定义 ——
//      抄一份必然漂移。因此：
//      · 光源（小、要逐元素写进光源 SSBO）用**逐字段镜像** + `static_assert` 逐字段锁偏移；
//      · 物体数据（大、已经是着色器布局）直接**内嵌 `GPUObjectData`**，从根上杜绝漂移。
// ============================================================

namespace he::render {

// C++/Slang 共享的 GPU 布局单一真源。
// 【必须在命名空间内包含】这些结构（GPULight / GPUObjectData …）定义在 `he::render` 里，
// 且它们的字段用 `float4` / `float4x4`（同样在 `he` 命名空间）—— 与 `Pipeline/Material.h`
// 的用法完全一致；放到全局作用域包含会变成"未知类型"。
#include "ShaderTypes.slang"

/// 单个可见物体的渲染输入（与 ECS 完全解耦：只有值，没有指针）
struct SnapshotDrawItem {
    /// 世界矩阵 + 材质参数 + 世界 AABB（与着色器逐字段一致，可直接 memcpy 进对象 SSBO）
    GPUObjectData object{};
    /// 上一帧世界矩阵：TAA / 运动矢量 / 重投影需要（骨骼物体由渲染线程按 skinMatrices 重算）
    float4x4 prevWorldMatrix{1.0f};

    /// 网格资源索引：**不是指针** —— 渲染线程据此去查自己的顶点/索引缓冲表
    u32 meshIndex = 0;
    /// 场景物体唯一 ID（调试、剔除统计、与 GPU Culling 的 objectID 对应）
    u32 objectID = 0;
    /// 可见性/实例化位标记（与 `GPUSceneObject::visibilityFlags` 同源）
    u32 visibilityFlags = 0;

    /// 间接绘制参数（与 `GPUSceneObject` 的三元组一致；**由 MeshBatcher 填充**，收集阶段恒为 0）
    u32 indexCount = 0;
    u32 firstIndex = 0;
    i32 vertexOffset = 0;
    /// 材质索引（与 `GPUSceneObject::materialIndex` 对应；组件里的 `materialID`）。
    /// 【与 `object.materialID` 不是一回事】后者是 bindless 纹理数组的**基索引**（着色器用它采样纹理），
    /// 前者是 GPU 剔除/间接绘制路径里的材质槽。两条路径都要，故都留在快照里。
    u32 materialIndex = 0;
};

// 【布局钉子】物体数据必须能整块拷贝、且 16B 对齐（std430 要求）
static_assert(std::is_trivially_copyable_v<SnapshotDrawItem>,
              "SnapshotDrawItem 必须可平凡拷贝：渲染线程要整块上传");
static_assert(alignof(SnapshotDrawItem) % 16 == 0, "SnapshotDrawItem 必须满足 std430 的 16B 对齐");

/// 光源条目：**逐字段镜像** `GPULight`（64B，std430）。
/// 为什么不用内嵌（与物体数据相反）：光源要按元素写进光源 SSBO，逐字段定义能让下面的
/// `static_assert` 把每个字段的偏移都钉死 —— 一旦着色器侧改了字段顺序或类型，这里立刻编译失败。
struct SnapshotLight {
    float4 colorIntensity{0.0f};    // xyz=颜色, w=强度（物理模式下为照度/光强）
    float4 directionType{0.0f};     // xyz=方向, w=类型（0=Dir, 1=Point, 2=Spot）
    float4 positionRange{0.0f};     // xyz=位置, w=范围（**负值** = 物理模式标记，着色器取 abs）
    float2 coneAngles{0.0f};        // x=内锥角, y=外锥角（Spot 专用）
    i32    shadowIndex = -1;        // -1 = 无阴影；>=0 指向阴影数据数组
    float  shadowRadius = 0.0f;     // 光源半径（软阴影用，0=硬阴影）

    /// 转成 GPU 结构：两者布局由下面的 static_assert 保证一致，故整块拷贝即可
    [[nodiscard]] GPULight ToGpu() const {
        GPULight gpu{};
        std::memcpy(&gpu, this, sizeof(GPULight));
        return gpu;
    }
    [[nodiscard]] static SnapshotLight FromGpu(const GPULight& gpu) {
        SnapshotLight out{};
        std::memcpy(&out, &gpu, sizeof(GPULight));
        return out;
    }
};

// 【布局钉子】逐字段锁死：大小 + 每个字段的偏移都必须与 GPULight 完全一致。
// 只断言 sizeof 不够 —— 两个字段互换位置时大小不变，但着色器会读到错的数据。
static_assert(sizeof(SnapshotLight) == sizeof(GPULight), "SnapshotLight 必须与 GPULight 同大小");
static_assert(offsetof(SnapshotLight, colorIntensity) == offsetof(GPULight, colorIntensity), "colorIntensity 偏移漂移");
static_assert(offsetof(SnapshotLight, directionType)  == offsetof(GPULight, directionType),  "directionType 偏移漂移");
static_assert(offsetof(SnapshotLight, positionRange)  == offsetof(GPULight, positionRange),  "positionRange 偏移漂移");
static_assert(offsetof(SnapshotLight, coneAngles)     == offsetof(GPULight, coneAngles),     "coneAngles 偏移漂移");
static_assert(offsetof(SnapshotLight, shadowIndex)    == offsetof(GPULight, shadowIndex),    "shadowIndex 偏移漂移");
static_assert(offsetof(SnapshotLight, shadowRadius)   == offsetof(GPULight, shadowRadius),   "shadowRadius 偏移漂移");
static_assert(std::is_trivially_copyable_v<SnapshotLight>, "SnapshotLight 必须可平凡拷贝");

/// 一帧的完整渲染输入。游戏线程在 tick 结束后构造，交接后**只读**（铁律 2）。
struct FrameSceneSnapshot {
    u64        frameIndex = 0;              // 与 CommandQueue 的帧号对应（对账用）
    u32        frameSlot  = 0;              // 帧槽位（按帧轮换的资源用）
    CameraData camera{};                    // 本帧相机（TAA 抖动在渲染线程叠加，见 §5 阶段 2）
    f32        deltaTime  = 0.0f;
    u32        viewportWidth  = 0;
    u32        viewportHeight = 0;

    /// 空中透视（大气）参数：xyz = 太阳方向（指向太阳），w = 浑浊度（0 = 关闭）。
    /// 与 `PushConstantData::atmosphere` / `DeferredLightingPushConstant::atmosphere` 逐字段一致；
    /// 由游戏线程从物理天空组件取（T1.4），渲染期因此不必再读世界。
    float4     atmosphere{0.0f, 1.0f, 0.0f, 0.0f};

    std::vector<SnapshotDrawItem> draws;
    std::vector<SnapshotLight>    lights;
    /// 骨骼矩阵（扁平数组：`boneOffset..boneOffset+boneCount` 属于某个 draw，
    /// 这样快照里不需要任何指针/容器嵌套，渲染线程按偏移切片即可）
    std::vector<float4x4>         skinMatrices;

    /// 世界版本号（游戏线程每次结构性改动 +1）：渲染线程可据此判断"快照是否落后于世界"，
    /// 流式/缓存类模块（Lumen 表面缓存、Nanite 页表）用它做失效判断，避免又去读世界。
    u64 sourceWorldVersion = 0;

    [[nodiscard]] bool IsEmpty() const { return draws.empty() && lights.empty(); }

    /// 复用快照（下一帧重新填充）：清空内容但**保留容量**，避免每帧重新分配。
    void Clear() {
        draws.clear();
        lights.clear();
        skinMatrices.clear();
    }

    /// 预留容量（首帧/场景规模变化时调用一次，之后每帧 `Clear()` 复用）
    void Reserve(u32 maxDraws, u32 maxLights, u32 maxSkinMatrices = 0) {
        draws.reserve(maxDraws);
        lights.reserve(maxLights);
        skinMatrices.reserve(maxSkinMatrices);
    }
};

/// 快照本身**允许**带容器（它由游戏线程构造、整份移交），但不能含指针 —— 这一点由上面两个
/// 元素类型的静态断言 + 代码评审保证：元素里只有值、索引与矩阵。
static_assert(std::is_trivially_copyable_v<CameraData>, "CameraData 必须是值类型（快照要按值携带它）");

} // namespace he::render
