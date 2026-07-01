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

    // ---- 光源 → 阴影数据索引 ----

    /// 返回光源在阴影 SSBO 中的索引，-1 表示不投射阴影
    virtual i32 GetShadowIndex(Entity light) const = 0;

    // ---- PSO 创建（ForwardPipeline 创建主布局后调用） ----

    virtual void CreateShadowPSO(rhi::DescriptorSetLayoutHandle layout) = 0;

    // ---- 状态查询 ----

    virtual bool HasActiveShadows() const = 0;
};

} // namespace he::render
