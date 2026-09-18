#pragma once

// ============================================================
// GI/RTProvider.h — 光追效果 Provider（Wave 2 阶段 5）
//
// 四个 RT 效果（RT 阴影 / RTAO / RT 反射 / RTGI）结构相同：
//   主 pass（对 GBuffer 像素发射射线）→ 时域累积降噪 →（反射/GI 追加空间滤波）
// 因此用一个参数化的 Provider 覆盖四种效果，避免四份重复代码。
//
// 两处与屏幕空间源不同的地方：
//   · 依赖共享的加速结构：由 ASBuildProvider 先行构建 TLAS（注册顺序保证）
//   · RT 阴影不属于三个层栈通道（阴影是独立枚举）→ 覆写 NeedsPass 判断
// ============================================================

#include "GI/IGIProvider.h"
#include "RT/RTShadowPass.h"
#include "RT/RTAOPass.h"
#include "RT/RTReflectionPass.h"
#include "RT/RTGIPass.h"
#include "RT/RTEffectPass.h"
#include "PostProcess/RTDenoiser.h"
#include "PostProcess/Denoiser.h"
#include "Pipeline/RTPass.h"
#include <vector>

namespace he::render {

/// 光追效果的 Provider（四种效果共用一个实现）
class RTEffectProvider final : public IGIProvider {
public:
    enum class Effect : u8 { Shadow, AO, Reflection, GI };

    explicit RTEffectProvider(Effect effect) : m_Effect(effect) {}

    // ── 注入底层 pass（均非拥有，由管线管理生命周期）──
    //
    // 【降噪链是数据，不是位置约定】每一级滤波 = `m_Stages` 里的一个元素，**顺序即执行顺序**。
    // 此前是两个固定指针（`m_Temporal` / `m_Spatial`）加一条"索引 0 是时域、1 是空间"的
    // 隐式约定：加第三级就必须改 `GetAuxPassInput` / `RenderAux` 里那串 if/else。
    // 现在加一级只需 push 一个 stage（§4.4.2 的第 2 条不统一）。
    void SetAS(RTPass* as) { m_AS = as; }
    void SetShadowPass(RTShadowPass* pass, RTDenoiser* temporal) {
        m_Shadow = pass;
        m_Stages.clear();
        if (temporal) m_Stages.push_back(Stage::Temporal(temporal, "RT_Shadow_Denoise"));
    }
    void SetAOPass(RTAOPass* pass, RTDenoiser* temporal) {
        m_AO = pass;
        m_Stages.clear();
        if (temporal) m_Stages.push_back(Stage::Temporal(temporal, "RT_AO_Denoise"));
    }
    void SetReflectionPass(RTReflectionPass* pass, RTDenoiser* temporal, Denoiser* spatial) {
        m_Reflection = pass;
        m_Stages.clear();
        if (temporal) m_Stages.push_back(Stage::Temporal(temporal, "RT_Reflection_Temporal"));
        if (spatial)  m_Stages.push_back(Stage::SpatialPass(spatial, "RT_Reflection_Denoise"));
    }
    void SetGIPass(RTGIPass* pass, RTDenoiser* temporal, Denoiser* spatial) {
        m_GI = pass;
        m_Stages.clear();
        if (temporal) m_Stages.push_back(Stage::Temporal(temporal, "RT_GI_Temporal"));
        if (spatial)  m_Stages.push_back(Stage::SpatialPass(spatial, "RT_GI_Denoise"));
    }
    /// RTGI 的 miss 回退需要 DDGI 探针（由帧图注入）
    void SetDDGIFallback(rhi::IRHIBuffer* probeBuffer, rhi::IRHIBuffer* gridUniform) {
        m_DDGIProbe = probeBuffer; m_DDGIGrid = gridUniform;
    }

    // ── 身份 ──
    /// RT 阴影的「源」不在 GI 层栈内（阴影是独立枚举，不进层栈）→ 返回 None。
    /// 帧图对本 Provider 单独以 ShouldRunRTShadow() 判断（见 NeedsPass）。
    [[nodiscard]] GISourceId GetSourceId() const override {
        switch (m_Effect) {
        case Effect::Shadow:     return GISourceId::None;
        case Effect::AO:         return GISourceId::RTAO;
        case Effect::Reflection: return GISourceId::RTReflection;
        default:                 return GISourceId::RTGI;
        }
    }
    [[nodiscard]] bool Handles(GISourceId id) const override { return id == GetSourceId(); }
    [[nodiscard]] bool IsValid() const override { return MainPassValid(); }

    /// RT 阴影不属于层栈的通道（阴影为独立枚举）→ 单独判断
    [[nodiscard]] bool NeedsPass(const GIChannelStack& stack) const override {
        if (!IsValid()) return false;
        if (m_Effect == Effect::Shadow) return m_RTShadowWanted;
        return stack.Has(GetSourceId());
    }
    /// 由帧图每帧告知「阴影枚举当前是否选择 RT 阴影」
    void SetRTShadowWanted(bool wanted) { m_RTShadowWanted = wanted; }
    /// 由帧图每帧告知「DDGI 是否自己也是漫反射层栈的源」。
    /// 为真时 GI 的 miss 分支不得回退 DDGI，否则 DDGI 信息被用两次、归一化失去无偏性（§9.2-I）。
    void SetDDGIInStack(bool inStack) { m_DDGIInStack = inStack; }
    [[nodiscard]] bool IsShadowEffect() const { return m_Effect == Effect::Shadow; }
    /// pass 名：RT 阴影不在 GI 源枚举内（阴影是独立枚举），单独给名
    [[nodiscard]] const char* GetName() const override {
        switch (m_Effect) {
        case Effect::Shadow:     return "RT Shadow";
        case Effect::AO:         return "RTAO";
        case Effect::Reflection: return "RT Reflection";
        default:                 return "RTGI";
        }
    }
    /// RT 阴影输出（不在层栈三通道内，供帧图单独取给 Lighting）
    [[nodiscard]] rhi::IRHITexture* GetShadowOutput() const {
        if (m_Effect != Effect::Shadow) return nullptr;
        return FinalOutput();
    }

    // ── 通道输出 ──
    [[nodiscard]] rhi::IRHITexture* GetDiffuseOutput() const override {
        return m_Effect == Effect::GI ? m_GI->GetOutput() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetSpecularOutput() const override {
        return m_Effect == Effect::Reflection ? m_Reflection->GetOutput() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetAOOutput() const override {
        return m_Effect == Effect::AO ? m_AO->GetOutput() : nullptr;
    }

    // ── 附属 pass：降噪链（时域累积 →（反射/GI）空间滤波）──
    // 索引在**已就绪的** stage 上紧凑编号（与改造前一致：时域未就绪时空间滤波前移为 0），
    // 输入取自链上前一级的输出，第一级取主 pass 输出。
    [[nodiscard]] u32 GetAuxPassCount() const override { return ActiveStageCount(); }
    [[nodiscard]] const char* GetAuxPassName(u32 i) const override {
        const Stage* st = ActiveStageAt(i);
        return st ? st->name : "";
    }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassInput(u32 i) const override {
        return StageInput(i);
    }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassOutput(u32 i) const override {
        const Stage* st = ActiveStageAt(i);
        return st ? StageOutput(*st) : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetFinalDiffuseOutput() const override {
        return m_Effect == Effect::GI ? FinalOutput() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetFinalSpecularOutput() const override {
        return m_Effect == Effect::Reflection ? FinalOutput() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetFinalAOOutput() const override {
        return m_Effect == Effect::AO ? FinalOutput() : nullptr;
    }

    void PreBindAux(rhi::IRHICommandList* cmd, u32 i) override {
        // 【每一级都必须在这里绑管线】帧图的附属 pass 会紧接着调 BeginOffscreenPass，而它是用
        // 「当前绑定的 PSO」去取 RenderPass 建 Framebuffer 的。若此刻还绑着上一 Pass 的 PSO
        // （例如 RTGI 的光线追踪管线、或带深度附件的图形 PSO），Framebuffer 附件数就会与
        // RenderPass 不匹配：校验层报 VUID-VkFramebufferCreateInfo-attachmentCount-00876，
        // 实测更严重 —— 设备直接挂住，进程再也不会推进（启用 RTGI 时稳定复现，见 §9.2-V）。
        const Stage* st = ActiveStageAt(i);
        if (!st) return;
        if (st->kind == Stage::Kind::Temporal) st->temporal->PreBind(cmd);
        else                                   st->spatial->PreBind(cmd);
    }
    void RenderAux(rhi::IRHICommandList* cmd, u32 i, const GIProviderContext& /*ctx*/) override {
        const Stage* st = ActiveStageAt(i);
        if (!st) return;
        rhi::IRHITexture* input = StageInput(i);
        if (st->kind == Stage::Kind::Temporal) {
            st->temporal->SetInputs(input, m_Depth, m_Normal, m_Velocity);
            st->temporal->Render(cmd);
        } else {
            st->spatial->SetInputs(input, m_Depth, m_Normal);
            st->spatial->Render(cmd);
        }
    }

    // ── 生命周期 ──
    bool Initialize(rhi::IRHIDevice*, u32, u32) override { return IsValid(); }
    void Shutdown() override {}
    void OnResize(u32, u32) override {}
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal,
                   rhi::IRHITexture* albedo) override {
        m_Depth = depth; m_Normal = normal; m_Albedo = albedo;
    }
    void SetVelocity(rhi::IRHITexture* velocity) { m_Velocity = velocity; }

    /// 主 pass：向 GBuffer 有效像素发射射线
    void Render(rhi::IRHICommandList* cmd, const GIProviderContext& ctx) override;

private:
    /// 降噪链的一级。两种滤波器（时域累积 / 空间双边）用同一个结构表示，
    /// 顺序即执行顺序 —— 这就是「链条是数据」的全部含义。
    struct Stage {
        enum class Kind : u8 { Temporal, Spatial };
        Kind        kind     = Kind::Temporal;
        RTDenoiser* temporal = nullptr;   // kind == Temporal 时有效
        Denoiser*   spatial  = nullptr;   // kind == Spatial 时有效
        const char* name     = "";

        static Stage Temporal(RTDenoiser* d, const char* n) {
            return Stage{ Kind::Temporal, d, nullptr, n };
        }
        static Stage SpatialPass(Denoiser* d, const char* n) {
            return Stage{ Kind::Spatial, nullptr, d, n };
        }
    };

    /// 该级是否可用（滤波器就绪才注册它的 pass）
    [[nodiscard]] static bool StageReady(const Stage& s) {
        return (s.kind == Stage::Kind::Temporal) ? (s.temporal && s.temporal->IsReady())
                                                 : (s.spatial  && s.spatial->IsReady());
    }
    [[nodiscard]] u32 ActiveStageCount() const {
        u32 n = 0;
        for (const Stage& s : m_Stages) if (StageReady(s)) n++;
        return n;
    }
    /// 第 i 个**已就绪**的级（未就绪的级被跳过，索引因此始终紧凑）
    [[nodiscard]] const Stage* ActiveStageAt(u32 i) const {
        u32 n = 0;
        for (const Stage& s : m_Stages) {
            if (!StageReady(s)) continue;
            if (n == i) return &s;
            n++;
        }
        return nullptr;
    }
    [[nodiscard]] static rhi::IRHITexture* StageOutput(const Stage& s) {
        return (s.kind == Stage::Kind::Temporal) ? s.temporal->GetOutput() : s.spatial->GetOutput();
    }
    /// 第 i 级的输入：链上前一级的输出，第一级取主 pass 输出
    [[nodiscard]] rhi::IRHITexture* StageInput(u32 i) const {
        if (i == 0u) return MainOutput();
        const Stage* prev = ActiveStageAt(i - 1u);
        rhi::IRHITexture* t = prev ? StageOutput(*prev) : nullptr;
        return t ? t : MainOutput();
    }

    [[nodiscard]] bool MainPassValid() const {
        switch (m_Effect) {
        case Effect::Shadow:     return m_Shadow != nullptr;
        case Effect::AO:         return m_AO != nullptr;
        case Effect::Reflection: return m_Reflection != nullptr;
        default:                 return m_GI != nullptr;
        }
    }
    [[nodiscard]] rhi::IRHITexture* MainOutput() const {
        switch (m_Effect) {
        case Effect::Shadow:     return m_Shadow->GetOutput();
        case Effect::AO:         return m_AO->GetOutput();
        case Effect::Reflection: return m_Reflection->GetOutput();
        default:                 return m_GI->GetOutput();
        }
    }
    /// 最终输出：链上最后一级的输出 → 主输出
    /// （顺序由 `m_Stages` 决定，不再由"空间优先于时域"这种硬编码规则决定）
    [[nodiscard]] rhi::IRHITexture* FinalOutput() const {
        const u32 n = ActiveStageCount();
        if (n == 0u) return MainOutput();
        const Stage* last = ActiveStageAt(n - 1u);
        rhi::IRHITexture* t = last ? StageOutput(*last) : nullptr;
        return t ? t : MainOutput();
    }

    Effect m_Effect;
    bool   m_RTShadowWanted = false;

    RTPass*           m_AS         = nullptr;
    RTShadowPass*     m_Shadow     = nullptr;
    RTAOPass*         m_AO         = nullptr;
    RTReflectionPass* m_Reflection = nullptr;
    RTGIPass*         m_GI         = nullptr;
    std::vector<Stage> m_Stages;   // 降噪链（顺序即执行顺序）

    rhi::IRHITexture* m_Depth    = nullptr;
    rhi::IRHITexture* m_Normal   = nullptr;
    rhi::IRHITexture* m_Albedo   = nullptr;
    rhi::IRHITexture* m_Velocity = nullptr;
    rhi::IRHIBuffer*  m_DDGIProbe = nullptr;
    rhi::IRHIBuffer*  m_DDGIGrid  = nullptr;
    bool              m_DDGIInStack = false;   // 由帧图每帧告知（见 SetDDGIInStack）
};

// ── 主 pass 实现（向 GBuffer 有效像素发射射线）──
// 四个效果的 Execute 调用形式一致，仅 pass 类型不同；
// 场景材质纹理、三角形法线、TLAS 均取自共享的 RTPass。
inline void RTEffectProvider::Render(rhi::IRHICommandList* cmd, const GIProviderContext& ctx) {
    if (!ctx.camera || !m_AS || !MainPassValid()) return;

    RTExecuteContext rc;
    rc.invViewProj = glm::inverse(ctx.camera->GetViewProjMatrix());
    rc.cameraPos   = ctx.camera->position;
    rc.frameIndex  = ctx.frameIndex;
    rc.gbDepth     = m_Depth;
    rc.gbNormal    = m_Normal;
    rc.lightBuffer = ctx.lightBuffer;
    rc.lightCount  = ctx.lightCount;
    rc.sceneMaterialTex     = m_AS->GetSceneMaterialTexture();
    rc.sceneTriangleNormals = m_AS->GetSceneTriangleNormals();
    rc.ddgiProbeBuffer = m_DDGIProbe;   // GI 的 miss 回退：DDGI 探针（仅当 DDGI 不是层栈源时使用）
    rc.ddgiGridUniform = m_DDGIGrid;
    rc.ddgiIsStackSource = m_DDGIInStack;   // 为真 ⇒ rgen 的 miss 不回退 DDGI（避免双重计数）
    rc.furnace           = ctx.furnace;     // 白炉：命中/未命中都返回理想值（§9.2-AC / 任务 33）

    rhi::IRHIAccelerationStructure* tlas = ctx.tlas ? ctx.tlas : m_AS->GetTLAS();
    switch (m_Effect) {
    case Effect::Shadow:     m_Shadow->Execute(cmd, tlas, rc);     break;
    case Effect::AO:         m_AO->Execute(cmd, tlas, rc);         break;
    case Effect::Reflection: m_Reflection->Execute(cmd, tlas, rc); break;
    default:                 m_GI->Execute(cmd, tlas, rc);         break;
    }
}

} // namespace he::render
