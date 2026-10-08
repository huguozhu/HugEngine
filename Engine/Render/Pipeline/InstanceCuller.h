#pragma once

#include "RHI/RHI.h"
#include "RHI/FrameRetireQueue.h"
#include "Math/Math.h"

#include <memory>
#include <unordered_map>
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
//   · 命令缓冲**每 meshIndex 一份**（20 字节，cpuAccess）：绑定时用 bindless SSBO 句柄传进 shader，
//     不做逐组件描述符集更新（Vulkan 描述符是执行时读取，边录边改会串台）。
//   · 供调试/判据：命令缓冲可 Map，读回"本网格可见实例数"（`InstanceState::visibleInstanceCount`）。
//   · 【阶段 1 第①段】实例缓冲与命令缓冲的**状态与生命周期**都在本类里（按 `meshIndex` 索引），
//     实例变换数据由快照按值送达 ⇒ 渲染期不再需要 `InstancedMeshComponent`。
// ============================================================

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

    // ------------------------------------------------------------
    // 逐 meshIndex 的实例缓冲状态（阶段 1 第①段 / §15.1）
    //
    // 【为什么在这里】这些字段原先**整份挂在 `InstancedMeshComponent` 上**：实例变换 SSBO、
    // 容量、脏标记（现已换成版本号）、退役队列，以及逐实例剔除的命令缓冲。只要它们还在组件上，
    // 渲染期就必须回读组件（B1 与组件指针两项闸门都降不下来）。搬到渲染侧之后：
    //   · **数据**（矩阵、版本号、开关、局部包围盒）由快照按值送达；
    //   · **生命周期**（缓冲创建/扩容/退役/回收）留在渲染侧，按 `meshIndex` 索引。
    // 与骨骼缓冲 E-2② 同一条原则。
    // ------------------------------------------------------------
    struct InstanceState {
        std::unique_ptr<rhi::IRHIBuffer> buffer;   // 实例变换 SSBO（随本表存活）
        u32  capacity        = 0;                  // 已分配缓冲可容纳的实例数
        u32  ssboHandle      = 0;                  // 实例 SSBO 的 bindless 句柄（容量不变则句柄不变）
        u32  uploadedVersion = 0;                  // 已上传的变换版本（= 快照里的 transformVersion）
        u32  lastCount       = 0;                  // 上次上传的实例数（统计/判据）
        u64  ownerEntity     = 0;                  // 来源实体 id（索引复用后据此重置）
        bool hasUpload       = false;              // 是否已成功上传过至少一次

        /// 退役缓冲（任务 23：**有界** N 帧延迟释放，替代原来的无界 vector 保活）
        rhi::FrameRetireQueue<std::unique_ptr<rhi::IRHIBuffer>> retired;

        /// 间接绘制命令缓冲（20 字节，cpuAccess）：**每飞行帧一份** —— 单份会让"本帧 CPU 清零
        /// 命令"与"上帧 GPU 仍在读该命令做间接绘制"打架。
        std::unique_ptr<rhi::IRHIBuffer> cullCmd[rhi::kMaxFramesInFlight];
        u32  cullCmdHandle[rhi::kMaxFramesInFlight] = { 0, 0, 0 };  // 命令缓冲的 bindless SSBO 句柄
        /// 上一次读回的可见实例数（GPU 异步写入，可能滞后一帧；仅统计/调试用）
        u32  visibleInstanceCount = 0;

        // 本帧/上帧是否在快照里出现过：用于回组件销毁后的缓冲回收（见 BeginInstancesFrame）
        bool seenThisFrame = false;
        bool seenLastFrame = false;
    };

    /// 实例缓冲的只读统计 —— 与"搬离组件前那几个字段"一一对应，
    /// 供样例调试面板/判据读取（渲染侧状态不该再由组件暴露）。
    struct InstanceStats {
        bool valid        = false;   // 该 meshIndex 是否有状态（未注册/未上传过为 false）
        u32  ssboHandle   = 0;
        u32  capacity     = 0;
        u32  retired      = 0;       // 待释放的退役缓冲数（验证"有界"）
        u32  visibleCount = 0;       // 上次读回的可见实例数（滞后一帧）
        u32  count        = 0;       // 上次上传的实例数
    };

    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown();

    /// 为某个实例化网格准备命令缓冲（首次/扩容时调用）
    /// @return 命令缓冲（20 字节，cpuAccess）；失败返回 nullptr
    std::unique_ptr<rhi::IRHIBuffer> CreateCommandBuffer(u32 indexCount, u32 firstIndex, i32 vertexOffset);

    /// **帧边界**：渲染侧每帧调用一次（在遍历本帧实例之前）
    /// · 推进各实例缓冲的退役队列（有界释放）；
    /// · 回收"上一帧起就不再出现"的条目（组件已销毁）⇒ 释放实例 SSBO、命令缓冲与 bindless 槽位，
    ///   避免缓冲表随场景反复增删而无界增长。
    /// 【为什么把回收挂在这里】快照**每帧都包含全部实例化组件**（含实例数为 0 的），
    /// 因此"本帧又见到这个 meshIndex"就是该条目仍然有效的证据 —— 不需要渲染侧持有组件指针。
    void BeginInstancesFrame(rhi::IRHIDevice* device);

    /// 上传/复用**某个 meshIndex** 的实例变换缓冲（按值吃数据 ⇒ 渲染侧不再读组件）
    /// · 版本号未变且容量够 → 直接返回既有句柄（不重传）
    /// · 容量不够 → 新建 + 旧缓冲进有界退役队列 + 释放旧 bindless 槽位（任务 23）
    /// · 容量够但版本变了 → Map 原地更新（句柄与槽位都不变）
    /// · 同一 `meshIndex` 换了来源实体（注册表索引被回收后复用）→ 丢弃旧缓冲并强制重传
    /// @param transforms 快照里的实例变换切片（交接后只读；本函数只做拷贝）
    /// @param transformVersion 组件侧的变换版本号（`SnapshotInstance::transformVersion`）
    /// @param ownerEntity 来源实体 id（识别索引复用；0 = 不校验）
    /// @return 实例变换 SSBO 的 bindless 句柄（0 = 不可用）
    u32 UploadInstanceTransforms(rhi::IRHIDevice* device, u32 meshIndex,
                                 const float4x4* transforms, u32 count,
                                 u32 transformVersion, u64 ownerEntity);

    /// 取某个 meshIndex 的实例状态（渲染侧自用：cull 命令缓冲、实例 SSBO、可见数）。
    /// 【为什么允许返回可写指针】它是渲染侧自己的状态表（与组件无关）；
    /// 绘制路径要落"可见实例数"并懒创建命令缓冲，故按指针返回。越界/不存在返回 nullptr。
    [[nodiscard]] InstanceState* FindInstanceState(u32 meshIndex);

    /// 只读统计（样例调试面板 / 判据；不改变任何状态）
    [[nodiscard]] InstanceStats GetInstanceStats(u32 meshIndex) const;

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

    /// 实例缓冲状态表（按 `meshIndex` 索引）——阶段 1 第①段从组件搬过来的那一份状态。
    /// 【为什么用 unordered_map】`meshIndex` 由网格注册表分配（稀疏、可能复用），
    /// 用表比"按索引开数组"省内存；unordered_map 的元素**地址稳定**，因此 `FindInstanceState`
    /// 返回的指针在插入/删除其它条目后依然有效（回收发生在 `BeginInstancesFrame` 里，
    /// 那时还没有人持有本帧的指针）。
    std::unordered_map<u32, InstanceState> m_Instances;

    u64 m_CullCount         = 0;
    u32 m_LastInstanceCount = 0;
};

} // namespace he::render
