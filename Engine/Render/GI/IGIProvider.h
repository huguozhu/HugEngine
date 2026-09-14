#pragma once

// ============================================================
// GI/IGIProvider.h — GI 源统一抽象（P4 / Wave 2）
//
// 设计目标：**新增一种 GI 只需实现本接口 + 注册**，帧图按注册表遍历构建 pass，
// 不再手写 `ShouldRunXXX` 门控、也不改 UBO / 合成循环 / 面板候选列表。
//
// 与层栈的关系：
//   · 层栈（GIChannelStack）描述「用户想用哪些源、权重多少」
//   · Provider 描述「这个源怎么算、输出到哪、当前是否有效」
//   帧图只需问 `NeedsPass(stack)`，即可决定是否注册 pass。
//
// 试点顺序（Wave 2 建议）：先 AO 通道（SSAO/GTAO/RTAO，源少且互斥），
// 验证抽象成立后再推广到 diffuse / specular。
// ============================================================

#include "Pipeline/LightingPass.h"
#include "GI/GIConfig.h"
#include "RHI/RHI.h"

namespace he::render {

/// GI Provider 接口
class IGIProvider {
public:
    virtual ~IGIProvider() = default;

    // ── 身份：这个 Provider 代表哪个源 ──
    /// 当前生效的源标识（同 pass 多模式时返回「当前模式」对应的源）
    [[nodiscard]] virtual GISourceId GetSourceId() const = 0;
    [[nodiscard]] virtual GIBand     GetBand() const { return GIBandOf(GetSourceId()); }
    [[nodiscard]] virtual const char* GetName() const { return GISourceName(GetSourceId()); }

    /// 该 Provider 还能代表哪些源（用于「同 pass 多模式」：如 SSAO / GTAO 共用一个 pass）
    /// 默认只代表 GetSourceId()；返回的列表用于面板候选与层栈匹配
    [[nodiscard]] virtual bool Handles(GISourceId id) const { return id == GetSourceId(); }

    // ── 状态与调度 ──
    /// 该源当前是否有效（可承载置信度语义：SSGI 全屏外、RTGI 未收敛、AO 关闭等）
    [[nodiscard]] virtual bool IsValid() const = 0;
    /// 是否需要在本帧注册 pass：层栈要求该源 ∧ 源有效
    [[nodiscard]] virtual bool NeedsPass(const GIChannelStack& stack) const {
        return stack.Has(GetSourceId()) && IsValid();
    }
    /// 在层栈「要求了但模式不同」时的同步钩子（如层栈选 GTAO → 切换 pass 模式）
    virtual void SyncToStack(const GIChannelStack& /*stack*/) {}

    // ── 通道输出（不适用则返回 nullptr）──
    [[nodiscard]] virtual rhi::IRHITexture* GetDiffuseOutput()  const { return nullptr; }
    [[nodiscard]] virtual rhi::IRHITexture* GetSpecularOutput() const { return nullptr; }
    [[nodiscard]] virtual rhi::IRHITexture* GetAOOutput()       const { return nullptr; }

    // ── 生命周期 ──
    /// 初始化（Provider 可自行创建/包装底层 pass）
    virtual bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) = 0;
    virtual void Shutdown() = 0;
    virtual void OnResize(u32 width, u32 height) = 0;

    /// 执行该源的 pass（帧图在 NeedsPass 为真时调用；附属 pass 由 Provider 自行提交）
    virtual void Render(rhi::IRHICommandList* cmd) = 0;

    /// 绑定该源 pass 的管线状态（帧图在 BeginOffscreenPass 之前调用）
    virtual void PreBind(rhi::IRHICommandList* /*cmd*/) {}

    /// 可选：由帧图注入 GBuffer 输入（深度/法线）；不使用的 Provider 可忽略
    virtual void SetInputs(rhi::IRHITexture* /*depth*/, rhi::IRHITexture* /*normal*/) {}
};

} // namespace he::render
