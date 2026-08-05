#pragma once

#include "RHI/RHI.h"
#include "Pipeline/RTPass.h"
#include "Pipeline/Camera.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// ReSTIRDispatchContext — ReSTIR Pass 每帧执行上下文
// 由 PathTracingPipeline 在 BuildFrameGraph 的 ReSTIR lambda 中填充
// ============================================================
struct ReSTIRDispatchContext {
    float4x4 invViewProj = float4x4(1.0f);   // 逆 ViewProj（深度 → 世界坐标）
    float3   cameraPos   = float3(0.0f);     // 相机世界坐标
    u32      frameIndex  = 0;                // 帧索引（随机种子）
    rhi::IRHIBuffer* lightBuffer = nullptr;  // GPULight[] SSBO（当前帧槽位）
    u32              lightCount  = 0;        // 有效光源数
    bool             historyValid = false;   // 蓄水池历史有效（首帧/光源变化后 false）
    rhi::IRHITexture* ptDepth    = nullptr;  // PT 输出：线性视图深度
    rhi::IRHITexture* ptNormal   = nullptr;  // PT 输出：世界法线(0~1)+roughness
    rhi::IRHITexture* ptVelocity = nullptr;  // PT 输出：屏幕速度
    rhi::IRHITexture* ptAlbedo  = nullptr;   // PT 输出：albedo(rgb)+metallic(a)
    u32  candidateCount = 16;                // 初始采样候选数 M
    u32  spatialRadius  = 3;                 // 空间复用采样半径（像素）
    u32  spatialSamples = 5;                 // 空间复用采样数
    float maxDistance   = 1.0f;              // 去遮挡/邻域验证容差（米）
};

// ============================================================
// ReSTIRPass — ReSTIR DI 时空重采样 Pass
//
// 三个 compute dispatch 在同一 RenderGraph Pass 内按提交序顺序执行：
//   Init（WRS 初始采样）→ Temporal（速度重投影 + 历史合并）
//   → Spatial（邻域共享）→ FinalReservoir（PT_RayGen 下帧读取）
//
// 数据：
//   InitialReservoir / TemporalReservoir[2]（双缓冲）/ FinalReservoir（SSBO）
//   历史 depth/normal 纹理 [2]（双缓冲，Temporal Pass 写入下一帧槽）
//   槽位由 EndFrame() 在帧末交换（CPU 指针，规避同帧跨像素读写竞争）
//
// 注意：FinalReservoir 为 PT_Render 下帧读取（跨帧），SSBO 不进
// RenderGraph —— 同队列提交序可见性（与 DDGI 探针同款约定）。
// ============================================================
class ReSTIRPass {
    HE_DECLARE_NON_COPYABLE(ReSTIRPass);

public:
    ReSTIRPass()  = default;
    ~ReSTIRPass() = default;

    // 初始化：创建 3 个 compute PSO + 蓄水池 SSBO + 历史纹理（始终分配，
    // 开关由 PathTracingPipeline 控制是否 dispatch）
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();

    // 每帧执行：Init → Temporal → Spatial（顺序 dispatch）
    void Execute(rhi::IRHICommandList* cmd, const ReSTIRDispatchContext& ctx);

    // 帧末交换历史槽位（读取槽 ↔ 写入槽）
    void EndFrame() { m_HistorySlot ^= 1; }

    // ── 访问器 ──
    rhi::IRHIBuffer* GetFinalReservoir() const { return m_Final.get(); }
    u32 GetWidth()  const { return m_Width; }
    u32 GetHeight() const { return m_Height; }
    bool IsValid()  const { return m_Ready; }

private:
    // 单个 compute 管线（PSO + 布局 + 描述符集）
    struct ComputePipe {
        std::unique_ptr<rhi::IRHIPipelineState> pso;
        rhi::DescriptorSetLayoutHandle layout = rhi::kInvalidLayout;
        rhi::DescriptorSetHandle       set    = rhi::kInvalidSet;
    };
    // 创建 compute PSO + set0 布局 + 描述符集
    bool CreatePipeline(ComputePipe& pipe,
                        const std::vector<rhi::DescriptorSetLayoutBinding>& bindings,
                        const std::vector<u32>& spirv, StringView name);
    void DestroyPipeline(ComputePipe& pipe);

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    ComputePipe m_Init;      // 初始采样（WRS）
    ComputePipe m_Temporal;  // 时域复用
    ComputePipe m_Spatial;   // 空间复用

    // 蓄水池 SSBO（PTReservoir × W×H，32B/个）
    std::unique_ptr<rhi::IRHIBuffer> m_Initial;            // InitialReservoir
    std::unique_ptr<rhi::IRHIBuffer> m_TemporalBuf[2];     // TemporalReservoir（双缓冲）
    std::unique_ptr<rhi::IRHIBuffer> m_Final;              // FinalReservoir（PT 下帧读）

    // 历史纹理（双缓冲：读 slot A / 写 slot B，EndFrame 交换）
    std::unique_ptr<rhi::IRHITexture> m_HistDepth[2];      // 历史深度 R32_FLOAT
    std::unique_ptr<rhi::IRHITexture> m_HistNormal[2];     // 历史法线 RGBA16_FLOAT
    u32 m_HistorySlot = 0;
};

} // namespace he::render
