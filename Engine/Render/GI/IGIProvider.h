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
#include "GI/GITypes.h"
#include "PostProcess/DenoiseSignal.h"   // 步骤 34（11.3）：DenoiseSignal / 统一历史池 / 有效性契约
#include "RHI/RHI.h"

namespace he::render {

struct CameraData;   // 前向声明（避免头文件循环）

/// Provider 执行上下文（帧图在调用 Render/RenderAux 时注入）
/// 有些源只依赖 GBuffer（如屏幕空间 AO），有些需要场景数据（RSM/DDGI 需要光源与几何），
/// 光追类源还需要加速结构与光照缓冲。
struct GIProviderContext {
    he::World*       world      = nullptr;   // 场景（RSM/DDGI 生成探针用）
    he::SceneGraph*  sceneGraph = nullptr;
    const CameraData* camera    = nullptr;
    u32              frameIndex = 0;

    /// 白炉数值测试是否开启（GIConfig::furnaceMode）。源用它把自己的输入也切到
    /// 白炉条件（全白环境 + albedo=1），否则「各源真值 = 1」这条判据只能靠短路，
    /// 量纲错误就永远抓不到（§11.4；SSGI-CAL 就靠它把 SSGI 的标度真正测出来）。
    bool furnace = false;

    // ── 光追类源所需（由帧图注入）──
    rhi::IRHIBuffer* lightBuffer = nullptr;   // 光照缓冲（射线命中着色用）
    u32              lightCount  = 0;
    rhi::IRHIAccelerationStructure* tlas = nullptr;   // 顶层加速结构（AS_Build 产物）
};

/// GI Provider 接口
class IGIProvider {
public:
    /// pass 类型：决定帧图如何注册本源的 pass
    enum class GIPassKind : u8 {
        Offscreen,   // 全屏 offscreen pass（屏幕空间类：SSGI/SSR/SSAO/GTAO）
        Compute,     // 计算着色器 pass（探针更新类：DDGI）
        Custom,      // 自定义（如仅重建资源：IBL 烘焙）
    };

    virtual ~IGIProvider() = default;

    // ── 身份：这个 Provider 代表哪个源 ──
    /// 当前生效的源标识（同 pass 多模式时返回「当前模式」对应的源）
    [[nodiscard]] virtual GISourceId GetSourceId() const = 0;
    [[nodiscard]] virtual const char* GetName() const { return GISourceName(GetSourceId()); }

    /// 该 Provider 还能代表哪些源（用于「同 pass 多模式」：如 SSAO / GTAO 共用一个 pass）
    /// 默认只代表 GetSourceId()；返回 true 的 id 参与面板候选与层栈匹配
    [[nodiscard]] virtual bool Handles(GISourceId id) const { return id == GetSourceId(); }

    /// 本源的 pass 类型（帧图据此选择注册方式）
    [[nodiscard]] virtual GIPassKind GetPassKind() const { return GIPassKind::Offscreen; }
    /// 本源的输出是否需要「通道纹理」供 Lighting 采样。
    /// 探针/RSM 类源的产物由 shader 直接读取内部资源（缓冲/贴图集），故返回 false，
    /// 帧图据此不做 ImportTexture 与输出依赖声明。
    [[nodiscard]] virtual bool HasTextureOutput() const { return true; }

    // ── 状态与调度 ──
    /// 该源当前是否有效（可承载置信度语义：SSGI 全屏外、RTGI 未收敛、AO 关闭等）
    [[nodiscard]] virtual bool IsValid() const = 0;
    /// 是否需要在本帧注册 pass：层栈要求该源 ∧ 源有效
    [[nodiscard]] virtual bool NeedsPass(const GIChannelStack& stack) const {
        return stack.Has(GetSourceId()) && IsValid();
    }
    /// 在层栈「要求了但模式不同」时的同步钩子（如层栈选 GTAO → 切换 pass 模式）
    virtual void SyncToStack(const GIChannelStack& /*stack*/) {}

    /// 【步骤 34/35】本帧**是否真的产出了内容**（由 `SyncToStack` 按层栈写入）。
    ///
    /// 与 `IsValid()` 分开是必须的：`IsValid()` 的语义是"这个 pass 对象在"（RT 四种效果一创建就
    /// 恒真、Lumen 只要有输出纹理就恒真），**它不代表本帧的层栈要了这个源**。把两者混用会得到
    /// 两类假读数：①信号登记把"当帧根本没跑"的源报成待降噪信号；②转储把上一帧/未使用的纹理
    /// 当成"本帧产出"（实测：默认配置下 4 条 RT 信号全被登记，而 RG pass 列表里一个 RT pass 都没有）。
    /// 默认返回 `IsValid()`：对没有"按层栈门控"语义的源保持原行为。
    [[nodiscard]] virtual bool ProducedThisFrame() const { return IsValid(); }

    /// 该 Provider 是否需要「前帧 HDR 辐射度」这一共享输入（`GIRadianceHistory`）。    /// 声明为真即表示：它会在 pass 里采样**上一帧的 Lighting 结果**当作入射辐射度
    /// （DDGI 的探针辐射度回退、SSGI 的 L_in）。
    ///
    /// 【为什么要声明而不是在帧图里写死】该输入的**捕获**必须与消费者一致：捕获写漏 ⇒
    /// 消费者采样到一张从未写入的纹理，而且**表面一切正常、输出恒为 0**。此前帧图里只写了
    /// `m_DDGI.IsEnabled()`，于是「diffuse = {SSGI}」这一配置下从不捕获，SSGI 恒为 0
    /// ——与 §9.2-Q（IBL 烘焙门控只看漫反射栈）属于同一类"消费者门控写漏"。
    /// 统一走这个谓词，新增消费者不会再被漏掉。
    [[nodiscard]] virtual bool NeedsRadianceHistory() const { return false; }

    /// 计时读数落点：本源对应的 `IGlobalIllumination` 实现（没有则 nullptr）。
    /// 帧图的 GPU 计时器把测得的耗时写回它，面板上那四行「每源耗时」才有真数
    /// （任务 29 / §9.2-Z）。屏幕空间 AO 与光追效果不走 `IGlobalIllumination`，返回 nullptr。
    [[nodiscard]] virtual IGlobalIllumination* GetTimedPass() const { return nullptr; }

    // ── 通道输出（不适用则返回 nullptr）──
    [[nodiscard]] virtual rhi::IRHITexture* GetDiffuseOutput()  const { return nullptr; }
    [[nodiscard]] virtual rhi::IRHITexture* GetSpecularOutput() const { return nullptr; }
    [[nodiscard]] virtual rhi::IRHITexture* GetAOOutput()       const { return nullptr; }
    /// 该通道的最终输出（主 pass 之后可能还有降噪等附属 pass；默认 = 主输出）
    [[nodiscard]] virtual rhi::IRHITexture* GetFinalDiffuseOutput() const { return GetDiffuseOutput(); }
    [[nodiscard]] virtual rhi::IRHITexture* GetFinalSpecularOutput() const { return GetSpecularOutput(); }
    [[nodiscard]] virtual rhi::IRHITexture* GetFinalAOOutput() const { return GetAOOutput(); }

    // ── 附属 pass（降噪/累积等；帧图在主线之后依次注册）──
    // 让 Provider 自报「我还需要哪些后续 pass」，使降噪链也纳入注册表驱动，
    // 而不必在帧图里为每种源手写。
    /// 【步骤 34 / §10 的 11.3】把本 Provider 当帧产出的**降噪信号**登记到统一框架里。
    /// 默认不登记（下游自己决定）；有降噪链的源（SSGI / SSR / RT 各效果）实现它。
    /// 登记之后，"当帧有哪几条信号、各需不需要升采样、参数是多少"就只有一个视角。
    virtual void DescribeSignals(DenoiseSignalRegistry& /*registry*/,
                                 rhi::IRHITexture* /*depth*/, rhi::IRHITexture* /*normal*/,
                                 rhi::IRHITexture* /*velocity*/) {}

    [[nodiscard]] virtual u32 GetAuxPassCount() const { return 0; }
    [[nodiscard]] virtual const char* GetAuxPassName(u32 /*i*/) const { return ""; }
    /// 附属 pass 的输出纹理（供后续 pass / 帧图声明依赖）
    [[nodiscard]] virtual rhi::IRHITexture* GetAuxPassOutput(u32 /*i*/) const { return nullptr; }
    /// 附属 pass 的输入（通常是主 pass 输出，或前一附属 pass 输出）
    [[nodiscard]] virtual rhi::IRHITexture* GetAuxPassInput(u32 /*i*/) const { return nullptr; }
    /// 执行第 i 个附属 pass（须已完成 PreBindAux/BeginOffscreenPass）
    virtual void RenderAux(rhi::IRHICommandList* /*cmd*/, u32 /*i*/, const GIProviderContext& /*ctx*/) {}

    // ── 生命周期 ──
    /// 初始化（Provider 可自行创建/包装底层 pass）
    virtual bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) = 0;
    virtual void Shutdown() = 0;
    virtual void OnResize(u32 width, u32 height) = 0;

    /// 执行该源的主 pass（帧图在 NeedsPass 为真时调用；附属 pass 见 GetAuxPass*）
    virtual void Render(rhi::IRHICommandList* cmd, const GIProviderContext& ctx) = 0;

    /// 绑定该源 pass 的管线状态（帧图在 BeginOffscreenPass 之前调用）
    virtual void PreBind(rhi::IRHICommandList* /*cmd*/) {}
    /// 绑定附属 pass 的管线状态
    virtual void PreBindAux(rhi::IRHICommandList* /*cmd*/, u32 /*i*/) {}

    /// 可选：由帧图注入 GBuffer 输入（深度/法线/反照率）；不使用的 Provider 可忽略
    virtual void SetInputs(rhi::IRHITexture* /*depth*/, rhi::IRHITexture* /*normal*/,
                           rhi::IRHITexture* /*albedo*/) {}
};

} // namespace he::render
