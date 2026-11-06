#include "PostProcess/PTAtrousPass.h"
#include <iterator>
#include "Core/Log.h"
#include "Core/Assert.h"
#include "PT_Atrous.comp.spv.h"
#include "PT_Atrous.comp.dxil.h"   // k_PT_Atrous_comp_spv
#include "PT_Atrous.comp.dxbc.h"

#include <algorithm>   // std::clamp

namespace he::render {

// PT 脌-Trous 闄嶅櫔鎻忚堪绗﹂泦缁戝畾鍙穈r
static constexpr u32 kAtrousBindColor  = 0;
static constexpr u32 kAtrousBindDepth  = 1;
static constexpr u32 kAtrousBindNormal = 2;
static constexpr u32 kAtrousBindOutput = 3;

// A-Trous 空间滤波 push constant（与 PT_Atrous.comp.slang 的 AtrousPC 逐字段一致）
// 布局：uint2(8) + u32(4) + 4×float(16) + 2×float pad(8) = 36B
struct PTAtrousPushConstant {
    uint2 dispatchDim;    // 分辨率
    u32   iterations;     // 1-5
    float sigmaDepth;     // 深度边权重 σ（米）
    float normalPower;    // 法线边权重指数（如 128）
    float sigmaColor;     // 颜色边权重 σ 系数
    float clampThreshold; // 火萤钳制阈值（0=关）
    float pad1, pad2;     // 对齐填充（与着色器布局保持一致）
};

bool PTAtrousPass::Initialize(rhi::IRHIDevice* device, const Config& cfg) {
    m_Device = device;
    m_Cfg    = cfg;
    m_Width  = cfg.width;
    m_Height = cfg.height;
    HE_ASSERT(m_Device, "PTAtrousPass: null device");

    // set0：b0=color(SampledImage), b1=depth, b2=normal, b3=output(StorageImage)
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
        {1, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
        {2, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskCompute},
        {3, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute},
    };
    m_Layout = device->CreateDescriptorSetLayout(layout);
    if (m_Layout == rhi::kInvalidLayout) {
        HE_CORE_ERROR("PTAtrousPass: set0 布局创建失败");
        return false;
    }
    // 描述符集按飞行帧各一份（见头文件 SetFrameSlot 的说明）
    for (u32 i = 0; i < rhi::kMaxFramesInFlight; ++i) {
        m_Set[i] = device->AllocateDescriptorSet(m_Layout);
        if (m_Set[i] == rhi::kInvalidSet) {
            HE_CORE_ERROR("PTAtrousPass: set0 描述符集创建失败（槽位 {}）", i);
            return false;
        }
    }

    // compute PSO（36B push constant）
    rhi::ShaderBytecode cs;
    cs.stage = rhi::ShaderStage::Compute;
    cs.spirv = k_PT_Atrous_comp_spv;
    cs.dxil.assign(std::begin(k_PT_Atrous_comp_dxil), std::end(k_PT_Atrous_comp_dxil));
    cs.dxil.assign(std::begin(k_PT_Atrous_comp_dxil), std::end(k_PT_Atrous_comp_dxil));
    cs.dxil.assign(std::begin(k_PT_Atrous_comp_dxil), std::end(k_PT_Atrous_comp_dxil));
    cs.dxbc.assign(std::begin(k_PT_Atrous_comp_dxbc), std::end(k_PT_Atrous_comp_dxbc));
    cs.entryPoint = "main";
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskCompute;
    pc.size      = sizeof(PTAtrousPushConstant);   // AtrousPC 共 36B（uint2+u32+4×float+pad×2）
    rhi::PipelineStateDesc d;
    d.bindPoint = rhi::PipelineBindPoint::Compute;
    d.computeShader = &cs;
    d.pushConstantRanges = {pc};
    d.descriptorSetLayouts = {m_Layout};
    d.debugName = "PTAtrous";
    m_PSO = device->CreatePipelineStateHandle(d);
    if (!m_PSO.IsValid()) { HE_CORE_ERROR("PTAtrousPass: compute PSO 创建失败"); return false; }

    CreateTextures(m_Width, m_Height);
    m_Ready = true;
    HE_CORE_INFO("PTAtrousPass: 初始化完成 ({}x{}, iters={})", m_Width, m_Height, m_Cfg.iterations);
    return true;
}

void PTAtrousPass::CreateTextures(u32 w, u32 h) {
    rhi::TextureDesc d;
    d.format = rhi::Format::RGBA16_FLOAT;
    d.width = w;
    d.height = h;
    d.mipLevels = 1;
    // TransferSrc：供 05 的 HE_DUMP_PT 把"A-Trous 输出"整幅拷回 host 核对（A3 的验收证据）
    d.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource
            | rhi::TextureUsage::TransferSrc;
    m_Output = m_Device->CreateTexture(d);
}

void PTAtrousPass::Shutdown() {
    // 【空保护】m_Device 可能为空（Shutdown 被调用两次，或 Initialize 失败后进入清理路径），
    // 直接解引用会崩溃（实测：02.Cube 退出时连续崩在多个 Shutdown 里）。
    if (!m_Device) return;

    m_Output.reset();
    m_Device->DestroyPipelineStateHandle(m_PSO); m_PSO = {};
    for (auto& set : m_Set) set = rhi::kInvalidSet;   // 集由 RHI 池持有，这里只断句柄
    if (m_Device && m_Layout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_Layout);
        m_Layout = rhi::kInvalidLayout;
    }
    m_Device = nullptr;
    m_Ready = false;
}

void PTAtrousPass::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width = w;
    m_Height = h;
    m_Output.reset();
    CreateTextures(w, h);
    m_FirstUseDone = false;   // 新图重新从 UNDEFINED 开始，首用转换要再做一次
}

void PTAtrousPass::SetInputs(rhi::IRHITexture* color, rhi::IRHITexture* depth, rhi::IRHITexture* normal) {
    m_Color = color;
    m_Depth = depth;
    m_Normal = normal;
    if (!m_Device) return;
    m_Device->UpdateDescriptorSet(CurrentSet(), kAtrousBindColor, rhi::DescriptorType::SampledImage, color, nullptr);
    m_Device->UpdateDescriptorSet(CurrentSet(), kAtrousBindDepth, rhi::DescriptorType::SampledImage, depth, nullptr);
    m_Device->UpdateDescriptorSet(CurrentSet(), kAtrousBindNormal, rhi::DescriptorType::SampledImage, normal, nullptr);
}

void PTAtrousPass::SetParams(u32 iterations, float sigmaDepth, float normalPower,
                             float sigmaColor, float clampThreshold) {
    m_Cfg.iterations     = std::clamp(iterations, 1u, 5u);
    m_Cfg.sigmaDepth     = sigmaDepth;
    m_Cfg.normalPower    = normalPower;
    m_Cfg.sigmaColor     = sigmaColor;
    m_Cfg.clampThreshold = clampThreshold;
}

void PTAtrousPass::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Color || !m_Depth || !m_Normal) return;

    m_Device->UpdateDescriptorSetWithImageView(CurrentSet(), 3,
        rhi::DescriptorType::StorageImage, m_Output->GetNativeHandle());

    PTAtrousPushConstant pc;
    pc.dispatchDim     = uint2(m_Width, m_Height);
    pc.iterations      = m_Cfg.iterations;
    pc.sigmaDepth      = m_Cfg.sigmaDepth;
    pc.normalPower     = m_Cfg.normalPower;
    pc.sigmaColor      = m_Cfg.sigmaColor;
    pc.clampThreshold  = m_Cfg.clampThreshold;
    pc.pad1 = pc.pad2 = 0.0f;

    // ── 首次使用（含重建后首次）补一次 UNDEFINED → GENERAL 的布局转换 ──
    // 本 pass 用 **StorageImage 描述符**（期望 GENERAL 布局）写 m_Output，而 RenderGraph 对
    // "只写"的导入纹理**不补** Undefined→UAV 的转换（其注释写明"写用途交给 render pass / UAV
    // 自己初始化"—— 这对 D3D12 成立，渲染目标也确实由 render pass 的 initialLayout 覆盖，
    // 但 compute 的 storage image 没有任何环节做这一步）。
    // 结果是首帧 dispatch 时该图仍是 UNDEFINED，校验层报
    //   vkQueueSubmit: … expects VkImage … to be in layout VK_IMAGE_LAYOUT_GENERAL
    //   --instead, current layout is VK_IMAGE_LAYOUT_UNDEFINED（VUID-vkCmdDraw-None-09600）
    // 从 UNDEFINED 转换永远合法，且写用途本来就不需要保留旧内容 ⇒ 在这里补最省事也最准确。
    if (!m_FirstUseDone) {
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::Undefined, rhi::ResourceState::UnorderedAccess,
                             m_Output.get());
        m_FirstUseDone = true;
    }

    cmd->SetPipeline(ResP(m_PSO));
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, CurrentSet());
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch((m_Width + 7) / 8, (m_Height + 7) / 8, 1);
}

} // namespace he::render
