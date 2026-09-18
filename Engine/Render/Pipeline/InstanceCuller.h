#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"

#include <memory>
#include <vector>

// ============================================================
// InstanceCuller — 逐实例 GPU 视锥剔除（任务 25）
//
// 背景：实例化网格是"一个组件 = 上万个实例"。原来的绘制路径是
//   `DrawIndexed(indexCount, instanceCount)` —— 只要组件整体过视锥（或干脆不过），
//   10000 个实例就会全被顶点着色器处理，背对相机/在画面外的那部分纯属浪费。
//
// 本类把剔除粒度下推到实例：
//   ① Dispach 前 CPU 把该组件的间接命令填好（indexCount / firstIndex / vertexOffset，
//      instanceCount 清零）；
//   ② compute（InstancedCull.comp.slang）逐实例变换局部包围盒 → 六平面测试 →
//      通过者压缩写入共享可见列表，并原子累加命令里的 instanceCount；
//   ③ 全局内存屏障后 `DrawIndexedIndirect(cmd, 0, 1, 20)`，顶点着色器按
//      `可见列表[SV_InstanceID]` 取真正的实例变换。
//
// 设计要点：
//   · 可见索引列表**全局共用一份**：cull/draw 在同一命令缓冲里按组件顺序交替，
//     同队列顺序执行 + 屏障保证 draw 读到的是自己那次 cull 的结果。
//   · 命令缓冲**每组件一份**（20 字节，cpuAccess）：绑定时用 bindless SSBO 句柄传进 shader，
//     不做逐组件描述符集更新（Vulkan 描述符是执行时读取，边录边改会串台）。
//   · 供调试/判据：组件命令缓冲可 Map，读回"本组件可见实例数"。
// ============================================================

namespace he {
class InstancedMeshComponent;
}

namespace he::render {

/// 与 InstancedCull.comp.slang 的 IndirectCmd 一致（= VkDrawIndexedIndirectCommand，20 字节）
struct alignas(4) InstanceIndirectCommand {
    u32 indexCount    = 0;
    u32 instanceCount = 0;
    u32 firstIndex    = 0;
    i32 vertexOffset  = 0;
    u32 firstInstance = 0;
};
static_assert(sizeof(InstanceIndirectCommand) == 20, "间接命令必须是 20 字节（匹配 Vulkan/DGC 布局）");

/// 逐实例剔除的 push constant（与 InstancedCull.comp.slang 的 InstancedCullParams 逐字段对应）
struct InstancedCullParams {
    float4 planes[6];               // 世界空间视锥平面（0..95）
    float4 localBoundsMin;          // (96)  网格局部包围盒 min
    float4 localBoundsMax;          // (112) 网格局部包围盒 max
    u32    instanceCount;           // (128)
    u32    instanceBufferHandle;    // (132) u_Instances[] 句柄
    u32    visibleBufferHandle;     // (136) u_VisibleIndices[] 句柄
    u32    commandHandle;           // (140) u_Commands[] 句柄
};
static_assert(sizeof(InstancedCullParams) == 144, "InstancedCullParams 必须与 Slang cbuffer 一致");

class InstanceCuller {
public:
    /// 逐实例剔除的最大实例数（可见列表容量；超过则截断，绘制仍正确但会少画）
    static constexpr u32 kMaxInstances = 100000;

    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown();

    /// 为某个实例化网格准备命令缓冲（首次/扩容时调用）
    /// @return 命令缓冲（20 字节，cpuAccess）；失败返回 nullptr
    std::unique_ptr<rhi::IRHIBuffer> CreateCommandBuffer(u32 indexCount, u32 firstIndex, i32 vertexOffset);

    /// 上传/复用实例变换缓冲（Forward / Deferred 两条路径共用同一份逻辑）
    /// · 容量够 → Map 原地更新（句柄不变，任务 23 起不再"每次新建 + 旧缓冲保活"）
    /// · 需要扩容 → 新建 + 旧缓冲进有界退役队列 + 释放旧 bindless 槽位
    /// · 顺带推进退役队列（每帧调用一次）
    /// @return 实例变换 SSBO 的 bindless 句柄（0 = 不可用）
    u32 UploadInstanceTransforms(rhi::IRHIDevice* device, he::InstancedMeshComponent& im);

    /// 执行逐实例剔除（会写入 shared 可见列表与 commandBuffer 的 instanceCount）
    /// 【调用约定】必须紧接在对应组件的 `DrawIndexedIndirect` **之前**，中间不要插入其它剔除
    ///（可见列表是共享的；顺序 = cull 后立刻 draw 才成立）
    /// @param frameSlot 飞行帧槽位（0..kMaxFramesInFlight-1）：可见列表按槽位分开，
    ///        避免"本帧 cull 写列表"与"上帧 draw 还在读列表"跨帧打架
    /// @return 上一帧该命令缓冲里的可见实例数（GPU 已写完；用于统计，滞后一帧）
    u32 Cull(rhi::IRHICommandList* cmd,
             rhi::IRHIBuffer* instanceBuffer, u32 instanceSSBOHandle,
             rhi::IRHIBuffer* commandBuffer,  u32 commandSSBOHandle,
             u32 instanceCount, u32 frameSlot,
             const float3& localBoundsMin, const float3& localBoundsMax,
             const float4x4& viewProj);

    /// 指定飞行帧槽位的共享可见列表 bindless 句柄（顶点着色器按它查"第 i 个可见实例是哪个"）
    u32 GetVisibleIndicesHandle(u32 frameSlot) const {
        return m_VisibleSSBOHandle[frameSlot % rhi::kMaxFramesInFlight];
    }

    /// 累计剔除次数与最近一次的实例统计（调试/判据）
    u64 GetCullCount()           const { return m_CullCount; }
    u32 GetLastInstanceCount()   const { return m_LastInstanceCount; }

    rhi::IRHIPipelineState* GetPSO() const { return m_PSO.get(); }

private:
    rhi::IRHIDevice* m_Device = nullptr;

    // 共享可见实例索引列表（u32[]）——**每飞行帧一份**：
    // 单份会让"帧 N 的 cull 写"与"帧 N-1 仍在执行的 draw 读"互相覆盖
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleBuf[rhi::kMaxFramesInFlight];
    u32 m_VisibleSSBOHandle[rhi::kMaxFramesInFlight] = { 0, 0, 0 };

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::ShaderBytecode m_CS;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    u64 m_CullCount         = 0;
    u32 m_LastInstanceCount = 0;
};

} // namespace he::render
