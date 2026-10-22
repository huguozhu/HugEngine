#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "RHI/RHI.h"
#include "Math/Math.h"

#include <memory>

// ============================================================
// DecalPass — GBuffer 投影贴花 Pass（任务 24）
//
// 旧 MVP：贴花是一张半透明四边形（DecalComponent 的网格），靠 Transform 摆放去"贴"表面 ——
//   平面凑合，曲面/台阶/斜坡上会穿模、悬空。
// 本 Pass：把贴花当成**投影体积**（盒子）投到已有 GBuffer 上：
//   1. 逐贴花画一个包裹影响范围的盒子（顶点着色器），只覆盖可能受影响的屏幕区域；
//   2. 片段着色器读 GBuffer 的世界坐标（MRT4）拿到该像素真实表面点，落在盒外则 discard；
//   3. 盒内点按局部 xy 反算 UV 采样贴花纹理，用 alpha 混合写回 MRT0（albedo+metallic）
//      与 MRT1（normal+roughness），其余 6 个 MRT 的 writeMask = None（保持 GBuffer 原值）。
//
// 帧图位置：必须排在 GBuffer **之后**、Lighting **之前**（Lighting 要看到贴花改过的 albedo/法线）。
// 设计取舍（默认项）：
//   · 只作用于 Deferred 路径；Forward 仍是投射片卡片（无 GBuffer 可投影），见文档"已知边界"。
//   · 不写 emissive/velocity/worldPos/disney/lightmapKey（贴花不改变几何与光照参数之外的量）。
//   · 逐贴花一次 DrawIndexed（36 索引的盒子），贴花数量通常是几十个量级，不做实例化。
// ============================================================

namespace he::render {

struct FrameSceneSnapshot;   // 贴花数据来自快照（引用参数只需前置声明）

class GBufferRenderer;
struct CameraData;   // Pipeline/Camera.h

/// 逐贴花 push constant（与 DecalProject.vert/frag.slang 的 cbuffer 逐字段对应，共 240 字节）
struct DecalPushConstants {
    float4x4 viewProj      = float4x4(1.0f);
    float4   rotRow0       = float4(1, 0, 0, 0);   // 贴花局部 → 世界（正交基，可含实体缩放归一化）
    float4   rotRow1       = float4(0, 1, 0, 0);
    float4   rotRow2       = float4(0, 0, 1, 0);
    float4   invRotRow0    = float4(1, 0, 0, 0);   // 世界 → 贴花局部（上面的转置）
    float4   invRotRow1    = float4(0, 1, 0, 0);
    float4   invRotRow2    = float4(0, 0, 1, 0);
    float4   decalOrigin   = float4(0.0f);         // 贴花世界原点
    float4   halfExtents   = float4(0.5f, 0.5f, 0.25f, 0.0f);  // xy = 半宽/半高，z = 半厚度
    float4   colorOpacity  = float4(1.0f);         // rgb = 颜色，a = 不透明度
    float4   normalMetal   = float4(0, 1, 0, 0);   // xyz = 世界投影法线，w = 金属度
    float4   params        = float4(0.8f, 0.0f, 0.0f, 0.0f);   // 粗糙度, 纹理句柄, 1/宽, 1/高
    u32      flags         = 0;                    // bit0 = 使用贴花纹理
    u32      _pad0         = 0;
    u32      _pad1         = 0;
    u32      _pad2         = 0;
};

class DecalPass {
public:
    DecalPass() = default;
    ~DecalPass() = default;

    /// 创建立方体几何 + PSO（8 个 GBuffer 颜色附件、无深度附件、Load 保留、per-MRT 混合）
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 width, u32 height);

    /// 执行投影 Pass（须在 GBuffer 之后、Lighting 之前调用）
    /// @param gb GBuffer 渲染器（提供 MRT 视图与 worldPos/depth 采样源）
    /// 【阶段 1 T1.4】贴花数据来自**快照**（`FrameSceneSnapshot::decals`）⇒ 本 Pass 不再读 ECS：
    /// 签名去掉 `he::World&` / `he::SceneGraph&`（附录 B1 因此下降）。
    void Render(rhi::IRHICommandList* cmd, const FrameSceneSnapshot& snapshot,
                const CameraData& camera, GBufferRenderer& gb);

    /// 上一帧实际绘制的贴花数（调试/判据）
    u32 GetLastDecalCount() const { return m_LastDecalCount; }
    /// 累计执行的帧数（判据：证明 Pass 真的在跑）
    u64 GetFrameCount() const { return m_FrameCount; }

    /// PSO（供测试/调试查询）
    rhi::IRHIPipelineState* GetPSO() const { return m_Device->ResolvePipelineState(m_PSO); }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;

    /// 【T2.6】本帧的渲染线程上下文（可空；空时解析回退 `m_Device`）
    const RenderThreadContext* m_FrameCtx = nullptr;

public:
    /// 【T2.6】设置本帧的渲染线程上下文（搬迁过渡期用）
    void SetFrameContext(const RenderThreadContext* ctx) { m_FrameCtx = ctx; }

private:
    // 【T2.6】统一解析入口（回退分支直接调 m_Device->Resolve*，避免被批量替换成递归）
    [[nodiscard]] rhi::IRHIBuffer* ResB(rhi::RHIBufferHandle h) const {
        if (m_FrameCtx) return m_FrameCtx->ResolveBuffer(h);
        return m_Device ? m_Device->ResolveBuffer(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* ResT(rhi::RHITextureHandle h) const {
        if (m_FrameCtx) return m_FrameCtx->ResolveTexture(h);
        return m_Device ? m_Device->ResolveTexture(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHIPipelineState* ResP(rhi::RHIPipelineStateHandle h) const {
        if (m_FrameCtx) return m_FrameCtx->ResolvePipelineState(h);
        return m_Device ? m_Device->ResolvePipelineState(h) : nullptr;
    }

private:

    rhi::RHIBufferHandle m_CubeVB{};   // A-3：单位立方体（36 顶点 / 36 索引）
    rhi::RHIBufferHandle m_CubeIB{};   // A-3

    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    rhi::RHIPipelineStateHandle m_PSO{};   // A-3

    rhi::RHISamplerHandle m_PointSampler{};   // A-3

    u32 m_LastDecalCount = 0;
    u64 m_FrameCount     = 0;
};

} // namespace he::render
