#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// RTDenoiser — RT 效果时域累积降噪器
//
// 对所有 RT 效果输出（阴影/反射/AO/GI）做时域累积：
//   当前帧噪声 + 上一帧历史 → GBuffer Velocity 重投影 → 去遮挡检测 → 自适应混合
//
// 空间滤波不在此类内：反射/GI 可另行复用 PostProcess/Denoiser（5×5 双边模糊）
// 作为独立 RenderGraph Pass 串联在本 Pass 之后。
//
// 历史管理：m_History（上一帧累积结果）+ m_Output（当前帧写入目标），
// Render 末尾 swap 两者角色。GetOutput() 恒定返回当前帧写入目标
// （BuildFrameGraph 导入句柄时即为最终 Lighting 采样到的纹理）。
// ============================================================
class RTDenoiser {
    HE_DECLARE_NON_COPYABLE(RTDenoiser);

public:
    // 降噪参数配置（每效果独立实例化）
    struct Config {
        rhi::Format format = rhi::Format::RGBA16_FLOAT;  // 输出/历史纹理格式（与 RT Pass 输出一致）
        u32 width  = 0;                                   // 输出分辨率（与 RT Pass 输出一致）
        u32 height = 0;
        float temporalBlend    = 0.1f;  // 当前帧混合权重（0=纯历史, 1=纯当前帧）
        float depthThreshold   = 0.05f; // 去遮挡深度容差
        float normalThreshold  = 0.85f; // 去遮挡法线 dot 阈值
        const char* debugName  = "RTDenoiser";
    };

    RTDenoiser()  = default;
    ~RTDenoiser() = default;

    // 初始化：创建历史 + 输出纹理 + PSO + 描述符集 + 采样器
    bool Initialize(rhi::IRHIDevice* device, const Config& cfg);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    // 每帧绑定输入纹理（Render 前调用，外部不持有所有权）
    void SetInputs(rhi::IRHITexture* noisyColor, rhi::IRHITexture* depth,
                   rhi::IRHITexture* normal, rhi::IRHITexture* velocity);

    // 运行时更新时域混合因子（CVar 热更新用；Render 时写入 push constant）
    void SetTemporalBlend(float blend) { m_Cfg.temporalBlend = blend; }

    // 执行时域累积降噪。内部管理离屏 Pass + 历史角色交换。
    void Render(rhi::IRHICommandList* cmd);

    // ── 访问器 ──
    // 返回当前帧写入目标（Render 前导入 RG；Render 后即最新累积结果）
    rhi::IRHITexture* GetOutput() const { return m_Output.get(); }
    u32 GetWidth()  const { return m_Width; }
    u32 GetHeight() const { return m_Height; }
    bool IsReady()  const { return m_Ready; }

private:
    void CreateTextures(u32 w, u32 h);
    void CreatePSO();

    Config m_Cfg;
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    // 历史（上一帧累积结果，只读）+ 输出（当前帧累积结果，只写）
    // Render 末尾 swap 角色：m_Output → 下帧 m_History
    std::unique_ptr<rhi::IRHITexture> m_History;
    std::unique_ptr<rhi::IRHITexture> m_Output;

    // 采样器（点采样：深度/法线/速度/噪声/历史均用最近邻，避免插值模糊信号）
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;

    // PSO + 描述符
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    // 外部注入的输入（不持有所有权）
    rhi::IRHITexture* m_NoisyColor = nullptr;
    rhi::IRHITexture* m_Depth      = nullptr;
    rhi::IRHITexture* m_Normal     = nullptr;
    rhi::IRHITexture* m_Velocity   = nullptr;

    u32 m_FrameIndex = 0;  // 帧索引（首帧无历史数据）
};

} // namespace he::render
