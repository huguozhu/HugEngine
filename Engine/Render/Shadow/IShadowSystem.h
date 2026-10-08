#pragma once

#include "Subsystem/RenderSubsystem.h"
#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include "Core/Types.h"
#include "Scene/Entity.h"

namespace he::render {

// ============================================================================
// IShadowSystem — 阴影子系统抽象接口
//
// 所有阴影实现（CSM、RT、None）的统一入口：
//   - ForwardPipeline 通过此接口绑定阴影纹理/缓冲区到 PBR 描述符集
//   - 调用方通过 IRenderPipeline::GetShadowSystem() 获取
//
// 继承树：
//   IShadowSystem
//     ├── ShadowNone    空实现（关闭阴影）
//     ├── ShadowSystem  CSM + Point Cubemap（当前）
//     └── ShadowRT      光线追踪阴影（未来）
// ============================================================================
class IShadowSystem : public IRenderSubsystem {
public:
    ~IShadowSystem() override = default;

    // ---- 阴影模式 ----

    enum class Mode : u8 { None = 0, Traditional = 1, RayTraced = 2 };
    virtual Mode GetMode() const = 0;

    // ---- 每帧注入 ----

    virtual void NextFrame() = 0;

    /// 【T2.4 / 多槽快照的前置】把**本帧**快照交给阴影系统（在帧图构建期、`Render` 之前调用）。
    /// 【为什么要它】阴影的收集（`Update`）与绘制（`Render`）分别在两处：原先快照指针只在 `Update`
    /// 里缓存，于是"本帧没调 Update 却调了 Render"就会用到**上一帧**的指针 —— 单份快照时代这只是
    /// "读到当前数据"，**多飞行帧快照**时代却会读到别的槽位（这正是第 10 轮多槽试验回归的机制类）。
    /// 现在由管线每帧**无条件**绑一次（与是否 Update 无关）⇒ 指针永远是本帧那一份。
    virtual void SetFrameSnapshot(const FrameSceneSnapshot& snapshot, const MeshRegistry& registry) {
        (void)snapshot; (void)registry;
    }
    virtual void SetRenderResources(rhi::IRHIBuffer* objBuf,
                                     rhi::IRHIBuffer* shadowBuf,
                                     rhi::DescriptorSetHandle descSet) = 0;

    // ---- Shadow Map 纹理访问（供管线绑定 PBR 描述符集） ----

    /// 阴影贴图数量（CSM=3 级联，RT=0 或 1 个 Atlas）
    virtual u32 GetShadowMapCount() const = 0;
    /// 获取第 i 张阴影贴图（i ∈ [0, GetShadowMapCount())）
    virtual rhi::IRHITexture* GetShadowMap(u32 index) const = 0;
    /// 阴影贴图采样器
    virtual rhi::IRHISampler* GetShadowSampler() const = 0;

    // ---- 点光源阴影（可选覆写，默认 nullptr） ----

    virtual rhi::IRHITexture* GetPointShadowMap() const { return nullptr; }
    virtual rhi::IRHISampler* GetPointShadowSampler() const { return nullptr; }
    virtual rhi::IRHITexture* GetSpotShadowMap() const { return nullptr; }
    virtual rhi::IRHISampler* GetSpotShadowSampler() const { return nullptr; }
    virtual rhi::IRHITexture* GetRectShadowMap() const { return nullptr; }
    virtual rhi::IRHISampler* GetRectShadowSampler() const { return nullptr; }

    // ---- 光源 → 阴影数据索引 ----

    /// 返回光源在阴影 SSBO 中的索引，-1 表示不投射阴影
    virtual i32 GetShadowIndex(Entity light) const = 0;

    // ---- PSO 创建（ForwardPipeline 创建主布局后调用） ----

    virtual void CreateShadowPSO(rhi::DescriptorSetLayoutHandle layout) = 0;

    // ---- 状态查询 ----

    virtual bool HasActiveShadows() const = 0;

    /// 第 index 张阴影贴图本帧是否**真的被写入过**（index 语义同 GetShadowMap）。
    ///
    /// 返回值是「绑定方该不该用这张图」的唯一真值：false 表示本帧没有产生它
    /// （无该类光源 / 阴影被关闭），此时消费方必须绑占位纹理。否则会采到未初始化显存
    /// —— 这类故障是静默的（不报错、画面只是偏暗），且读数随显存布局变化而不可复现
    /// （§9.2-T）。默认 false：未实现该查询的实现方一律按「本帧未产出」处理。
    virtual bool WasShadowMapWritten(u32 index) const { (void)index; return false; }

    /// 获取 cascade i 的光源 VP 矩阵（默认返回 identity，CSM override）
    virtual float4x4 GetLightViewProj(u32 cascade) const { (void)cascade; return float4x4(1.0f); }
};

} // namespace he::render
