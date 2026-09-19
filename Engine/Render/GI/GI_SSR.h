#pragma once

#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

struct CameraData;   // 前向声明（与 GI_SSGI / GI_DDGI 同做法，避免头文件循环）

// ============================================================
// GI_SSR — 屏幕空间反射
//
// 每像素沿反射向量在深度缓冲中 Ray Marching，
// 命中的像素 albedo 作为间接镜面反射贡献。
// ============================================================
class GI_SSR : public IGlobalIllumination {
public:
    GI_SSR() = default;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList*) const override {}
    void OnResize(u32 w, u32 h) override;
    const char* GetName() const override { return "GI_SSR"; }
    bool IsReady() const override { return m_Ready; }
    bool IsEnabled() const override { return m_Settings.enabled; }
    void SetEnabled(bool e) override { m_Settings.enabled = e; }

    GIMode GetMode() const override { return GIMode::SSGI; }  // 暂用 SSGI mode
    rhi::IRHITexture* GetIndirectSpecularTexture() const override { return m_Output.get(); }

    /// 【步骤 34（11.3）】消费端（全分辨率）尺寸：半分辨率信号的升采样级以它为重建目标
    u32 GetFullWidth()  const { return m_Width; }
    u32 GetFullHeight() const { return m_Height; }

    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo);
    /// 让输出纹理尺寸与当前设置一致（`halfRes` 是运行时开关）。
    /// 必须在帧图**构图之前**调用（Provider::SyncToStack 正是这个时机）：否则本帧导入渲染图
    /// 的句柄会指向旧尺寸纹理，而设置要等到下次 OnResize 才生效（§9.2-G 的第三重真值）。
    void SyncOutputSize();
    /// 设置 Hi-Z 深度金字塔（层次追踪加速：大步长跳过低空区域，替代线性 march）
    void SetHiZ(rhi::IRHITexture* hiZ, rhi::IRHISampler* sampler);
    /// 注入真实相机（每帧由帧图给出）。屏幕空间重建必须用**渲染深度图时的那套**投影参数：
    /// 此前用硬编码的 kDefaultFOV/0.1/2000 自行拼投影矩阵，非默认相机（PhysicalCamera 会由
    /// 焦距反算 fov）下 viewPos 重建与采样点投影同时错位（§9.2-E，SSGI 已修，此处是同类实例）。
    /// 传 nullptr 时退化为默认投影，保证独立运行该 pass 也不会拿到未初始化矩阵。
    void SetCamera(const CameraData* camera) { m_Camera = camera; }
    rhi::IRHISampler* GetOutputSampler() const { return m_Sampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd) const { if (m_Ready) cmd->SetPipeline(m_PSO.get()); }

    float maxSteps   = 64;
    float stepSize   = 0.5f;
    float maxDistance = 50.0f;
    float thickness   = 0.1f;
    /// 按**场景尺度**自动推导 march 参数（任务 32 / §9.2-W 的顺带项）。
    ///
    /// 【为什么必须自动】上面三个默认值是给"1 单位 ≈ 1 米"的世界写的（`maxDistance=50` 米、
    /// `thickness=0.1` 米），而本仓库的场景是 Sponza：3720 单位宽、对角线 4636 —— 射线走 50
    /// 单位就停、命中容差只有 0.1 单位（在 600 单位距离处一个像素的足迹就有约 0.4 单位），
    /// 于是**几乎不可能命中**。这与任务 30 的 RSM 光锥是同一类缺陷：把一个场景尺度假设写成常数。
    /// 现在由帧图按场景包围盒对角线推导：`maxDistance = 对角线`、
    /// `thickness = 对角线 × 0.0025`（≈1080p/fov60 下 2~3 个像素足迹）、
    /// `stepSize = maxDistance / maxSteps`（线性回退路径按步数均分）。
    /// 置 0 可关闭自动推导（cfg 给出显式值时用显式值）。
    bool  autoScaleMarch = true;
    /// 是否走 Hi-Z 层次 march（默认 1 = 有金字塔时走层次追踪）。
    /// 置 0 强制走线性 march：两条路径的**有效性占比**必须同量级（任务 25 / §9.2-W 的判据），
    /// 保留这个开关就是为了能随时把两者摆在一起对照，而不是靠临时改代码重编。
    bool  useHiZ      = true;

private:
    void CreateOutputTex(u32 w, u32 h);
    // 半分辨率尺寸（GISettings.halfRes 时输出纹理降半——省约 3/4 像素着色）
    u32 halfResW(u32 w) const { return m_Settings.halfRes ? std::max(w / 2, 1u) : w; }
    u32 halfResH(u32 h) const { return m_Settings.halfRes ? std::max(h / 2, 1u) : h; }

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_Sampler;
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;

    /// 矩阵 UBO（binding 3）：invProj（clip→view）+ proj（view→clip）
    ///
    /// 为什么矩阵必须进 UBO：引擎保证的 push constant 范围只有 128B，而两个 mat4
    /// 恰好用满。此前只往 push constant 里传了**逆**矩阵，而 shader 还拿它当**正**投影
    /// 用（§9.2-B）——正投影无处安放正是该缺陷的成因。
    /// SSAO 早已采用「矩阵进 UBO」的同一做法（`SSAO.cpp` 的 projInv + proj）。
    std::unique_ptr<rhi::IRHIBuffer> m_UniformBuffer;

    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_HiZTex = nullptr;      // Hi-Z 金字塔（不持有所有权）
    rhi::IRHISampler* m_HiZSampler = nullptr;  // Hi-Z 点采样器
    const CameraData* m_Camera = nullptr;      // 非拥有；帧图每帧注入（见 SetCamera）
};

} // namespace he::render
