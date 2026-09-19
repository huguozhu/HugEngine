#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include "PostProcess/DenoiseSignal.h"   // 步骤 34（11.3）：统一历史分配 + 框架级有效性契约
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
    // 初始化：创建输出纹理 + PSO + 描述符集 + 采样器。
    // `historyPool` 非空时，**历史纹理从池里取**（统一分配，同名同尺寸同格式只建一次）；
    // 为空时保持旧行为（自己建一张）。
    bool Initialize(rhi::IRHIDevice* device, const Config& cfg,
                    DenoiseHistoryPool* historyPool = nullptr);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    // 每帧绑定输入纹理（Render 前调用，外部不持有所有权）
    void SetInputs(rhi::IRHITexture* noisyColor, rhi::IRHITexture* depth,
                   rhi::IRHITexture* normal, rhi::IRHITexture* velocity);

    // 运行时更新时域混合因子（CVar 热更新用；Render 时写入 push constant）
    void SetTemporalBlend(float blend) { m_Cfg.temporalBlend = blend; }

    // 相机运动自适应混合权重（0=关闭，静止时沿用 temporalBlend；运动时抬升以缩短历史拖影）
    void SetMotionBlend(float blend) { m_MotionBlend = blend < 0.0f ? 0.0f : (blend > 1.0f ? 1.0f : blend); }

    // 【必须在 BeginOffscreenPass 之前调用】绑定本 Pass 的管线。
    // 原因：BeginOffscreenPass 是拿「当前已绑定的 PSO」去取 RenderPass 来建 Framebuffer 的，
    // 若此刻绑着的是别的 PSO（例如上一 Pass 的 RTGI 光线追踪管线，或带深度附件的图形 PSO），
    // 建出来的 Framebuffer 附件数与 RenderPass 不匹配 → 校验层报
    // VUID-VkFramebufferCreateInfo-attachmentCount-00876，且实测会让设备挂住。
    // 帧图会先调 PreBind、再 BeginOffscreenPass，故管线状态由此函数负责。
    void PreBind(rhi::IRHICommandList* cmd);

    // 执行时域累积降噪（在调用方已 BeginOffscreenPass 之后调用）+ 历史角色交换。
    void Render(rhi::IRHICommandList* cmd);

    // ── 访问器 ──
    // 返回当前帧写入目标（Render 前导入 RG；Render 后即最新累积结果）
    rhi::IRHITexture* GetOutput() const { return m_Output; }
    u32 GetWidth()  const { return m_Width; }
    u32 GetHeight() const { return m_Height; }
    bool IsReady()  const { return m_Ready; }

private:
    void CreateTextures(u32 w, u32 h);
    DenoiseHistoryPool* m_HistoryPool = nullptr;   // 非拥有；为空则自建历史
    void CreatePSO();

    Config m_Cfg;
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    // 历史（上一帧累积结果，只读）+ 输出（当前帧累积结果，只写）
    // Render 末尾 swap 角色：m_Output → 下帧 m_History。
    // 【步骤 34】历史可能来自 `DenoiseHistoryPool`（**非拥有**）⇒ 两者都用裸指针 + 各自的自有槽位。
    std::unique_ptr<rhi::IRHITexture> m_OwnedHistory;   // 无池时自建的历史
    std::unique_ptr<rhi::IRHITexture> m_OwnedOutput;    // 本类自己的输出（始终自有）
    rhi::IRHITexture* m_History = nullptr;   // 指向 m_OwnedHistory 或池中的纹理
    rhi::IRHITexture* m_Output  = nullptr;   // 指向 m_OwnedOutput 或池中的纹理（swap 后可能互换）

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
    float m_MotionBlend = 0.0f;  // 相机运动自适应混合权重（默认 0=关闭，仅运动检测者设置）
};

} // namespace he::render
