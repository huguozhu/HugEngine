#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>
#include <vector>

// ============================================================
// SSAO — 屏幕空间环境光遮蔽
//
// 输入：深度 + 法线缓冲（GBuffer）
// 输出：单通道 AO 纹理（应用到环境光分量）
// 流程：SSAO Pass（写 m_RawAOTexture）→ 屏障 → Blur Pass（读 m_RawAOTexture、写 m_AOTexture）
//       → 应用到 Lighting。两趟是**两个独立的 render pass**，Blur 绝不能采样自己正在写的附件
//       （attachment feedback loop），详见 SSAO.cpp 的 Render。
// ============================================================

namespace he::render {

struct CameraData;   // 前向声明（与 GI_SSGI / GI_SSR 同做法，避免头文件循环）

class SSAO {
public:
    bool enabled = true;
    float radius      = 1.0f;   // 采样半径
    float bias        = 0.025f; // 深度偏移
    float intensity   = 1.0f;   // AO 强度
    int   sampleCount = 16;     // 每像素采样数（SSAO 模式）
    bool  halfRes     = false;  // 半分辨率计算（性能优先，省约 3/4 像素着色）

    // ── GTAO 模式（M6.3）──
    // GTAO 与 SSAO 是同类互斥算法（都估计「环境光遮蔽」这一物理量），故共用同一 pass：
    // 切换 useGTAO 即改用 GTAO 着色器（地平线切片 + 解析积分）。
    // 层栈中二者是独立的 GI 源（GISourceId::SSAO / GISourceId::GTAO），同一时刻启用其一。
    bool  useGTAO    = false;
    int   sliceCount = 4;       // GTAO 方位角切片数（每像素）

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    /// 设置输入（GBuffer Depth + Normal）
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal);

    /// 注入真实相机（每帧由帧图给出）。重建 view-space 位置与把采样点投影回屏幕，都必须用
    /// **渲染深度图时的那套**投影参数；此前用硬编码的 kDefaultFOV/0.1/2000 自行拼投影矩阵，
    /// 非默认相机（PhysicalCamera 由焦距反算 fov）下 AO 的采样位置系统性错位（§9.2-E）。
    /// 传 nullptr 时退化为默认投影，保证独立运行该 pass 也不会拿到未初始化矩阵。
    void SetCamera(const CameraData* camera) { m_Camera = camera; }

    /// 注入当前帧在飞槽位（由 `ScreenAOProvider` 每帧从 `GIProviderContext::frameIndex` 转交）。
    /// 【为什么必需】参数 UBO 与描述符集按槽位分开，写第 N+1 帧时不能覆盖第 N 帧正在读的那一份。
    void SetFrameSlot(u32 slot) { m_FrameSlot = slot % rhi::kMaxFramesInFlight; }

    /// 执行 SSAO + Blur Pass（两趟独立 render pass），最终结果写入 m_AOTexture
    void Render(rhi::IRHICommandList* cmd);

    // 输出（`m_AOTexture` = 模糊后的最终 AO，供光照消费）
    rhi::IRHITexture* GetAOTexture() const { return m_AOTexture.get(); }
    /// 模糊前的原始 SSAO/GTAO 输出（诊断/对比用；光照消费的是 GetAOTexture）
    rhi::IRHITexture* GetRawAOTexture() const { return m_RawAOTexture.get(); }
    rhi::IRHISampler* GetAOSampler() const { return m_AOSampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd);  // 惰性创建 PSO 并绑定

private:
    void CreateAOTexture(u32 w, u32 h);
    void CreateRawAOTexture(u32 w, u32 h);
    void GenerateKernel();
    void GenerateNoise(u32 size);
    // 半分辨率尺寸（halfRes 时 AO/Blur 纹理降半）
    u32 halfResW(u32 w) const { return halfRes ? std::max(w / 2, 1u) : w; }
    u32 halfResH(u32 h) const { return halfRes ? std::max(h / 2, 1u) : h; }

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    // PSOs（惰性创建：首次 PreBind/Render 时由 CreatePipelineState 生成）
    std::unique_ptr<rhi::IRHIPipelineState> m_SSAO_PSO;
    std::unique_ptr<rhi::IRHIPipelineState> m_GTAO_PSO;   // GTAO 模式（M6.3）
    std::unique_ptr<rhi::IRHIPipelineState> m_Blur_PSO;
    // PSO 描述符 + ShaderBytecode 副本（供惰性创建 + 预热队列使用）
    rhi::PipelineStateDesc m_SSAO_PsoDesc;
    rhi::PipelineStateDesc m_GTAO_PsoDesc;
    rhi::PipelineStateDesc m_Blur_PsoDesc;
    rhi::ShaderBytecode    m_SSAO_VS, m_SSAO_FS;   // ShaderBytecode 副本（生命周期与 SSAO 对象一致）
    rhi::ShaderBytecode    m_GTAO_FS;              // GTAO 片段着色器（顶点着色器复用 SSAO 的）
    rhi::ShaderBytecode    m_Blur_VS,  m_Blur_FS;

    // 描述符集
    //   【按帧在飞分槽（2026-09 画质阶段 0 修复）】SSAO 的参数 UBO（1168 B =
    //   64×16 kernel + 16 params + 2×64 投影矩阵）与承载它的描述符集
    //   此前都是**单份**，却每帧 Map 后整块重写 ⇒ 在 `kMaxFramesInFlight = 3` 的多帧在飞下，
    //   CPU 为第 N+1 帧写这块缓冲时，GPU 可能仍在读第 N 帧的同一块 ⇒ 参数（u_InvProj/u_Proj/
    //   u_Samples[64]）被读到写到一半 ⇒ **AO 逐帧不确定**。实测同一构建两次运行 `prov0_ao_*`
    //   差 2.5~23.9 万像素，且量级随运行时机浮动（典型的竞争特征）；把 AO 从层栈关掉后
    //   全部 20 个转储逐位相同。现改为与 `LightingPass::m_BlendUBO/m_Sets` 同款的**按槽数组**，
    //   槽位由帧图经 Provider 注入（见 `SetFrameSlot`）。
    //   `m_BlurSet` 保持单份：Blur pass 的参数走 push constant，且它的输入（原始 AO 纹理）
    //   在两次重建之间地址不变，故只在 `CreateRawAOTexture` 里写一次描述符，不每帧写。
    rhi::DescriptorSetLayoutHandle m_SSAOLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_SSAOSets[rhi::kMaxFramesInFlight] = {};
    rhi::DescriptorSetLayoutHandle m_BlurLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_BlurSet    = rhi::kInvalidSet;

    // 纹理
    //   【两张而非一张（2026-09 画质阶段 0 修复）】`m_RawAOTexture` 承接 SSAO/GTAO 原始输出，
    //   `m_AOTexture` 承接 Blur 结果。此前 Blur 直接采样并写回同一张 `m_AOTexture`
    //   （在同一个 render pass 内）⇒ attachment feedback loop，结果不确定。
    std::unique_ptr<rhi::IRHITexture> m_RawAOTexture;  // SSAO/GTAO 原始输出（Blur 的输入）
    std::unique_ptr<rhi::IRHITexture> m_AOTexture;     // 模糊后的最终 AO（对外输出）
    std::unique_ptr<rhi::IRHISampler> m_AOSampler;
    std::unique_ptr<rhi::IRHISampler> m_PointSampler; // 点采样（深度/法线）

    // 输入（不持有所有权）
    rhi::IRHITexture* m_DepthTex  = nullptr;
    rhi::IRHITexture* m_NormalTex = nullptr;
    const CameraData* m_Camera    = nullptr;   // 非拥有；帧图每帧注入（见 SetCamera）

    // 随机采样内核 + 噪声
    static constexpr u32 kKernelSize = 64;  // SSAO 采样核大小
    std::vector<float4> m_Kernel;
    std::unique_ptr<rhi::IRHITexture> m_NoiseTex;

    // SSAO 参数 Uniform Buffer（对应 shader SSAOParams: kernel[64] + params + proj）
    //   【按帧在飞分槽】见上面描述符集处的说明：单份缓冲 + 每帧重写 = CPU/GPU 竞争。
    std::unique_ptr<rhi::IRHIBuffer> m_ParamUBO[rhi::kMaxFramesInFlight];

    /// 当前帧在飞槽位（`SetFrameSlot` 注入；默认 0）
    u32 m_FrameSlot = 0;

    /// 输入描述符是否已写入（`SetInputs` 用它把"每帧写"降为"输入变化时写一次"）
    bool m_InputsBound = false;
};

} // namespace he::render
