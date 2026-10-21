#pragma once

#include "Core/Types.h"

#include <functional>
#include <memory>
#include <vector>

// ============================================================
// ParallelRecordHelper —— 并行命令录制（MTCR）的公共骨架（T3.1）
//
// 【为什么要抽出来】这套骨架原先只写在 `ForwardPipeline::RenderScene` 里：
//   ① 一组 secondary command list 池（复用；每次 BeginSecondary 前清空）
//   ② 按 draw 数分块（每块一条列表）
//   ③ 每块在 `rhi::SecondaryRecordScope` 里录制（两级归属：worker 必须显式声明自己是录制者）
//   ④ `JobSystem::ParallelInvoke` 并行
//   ⑤ 主线程按块序 `ExecuteSecondary` 合并
// Deferred / PathTracing 接并行录制时要重复这五步 ⇒ 抽成一份，避免各管线各写一套
// （"归属声明"与"按块序合并"这两处写错就是跨线程竞争或顺序不确定）。
//
// 【调用方只负责什么】`recordChunk`：给定一条 secondary 列表与 [start,end) 的 draw 区间，
// 自己设 viewport/scissor、绑描述符集、推 push constant、发 Draw。管线特有的东西（PSO、bindless 集、
// 顶点索引缓冲来源）全留在回调里 ⇒ 本文件不依赖任何具体管线。
//
// 【顺序纪律】合并按**块序**（0..n-1），与录制完成顺序无关 ⇒ 绘制顺序确定、可逐位复现。
// ============================================================

namespace he::rhi {
class IRHIDevice;
class IRHICommandList;
class IRHIPipelineState;
} // namespace he::rhi

namespace he::render {

class ParallelRecordHelper {
public:
    /// 每块录制的回调：`secCmd` 是分给该块的次级列表；只录 [start,end) 这个区间。
    using RecordChunkFn = std::function<void(rhi::IRHICommandList& secCmd, u32 start, u32 end)>;

    ParallelRecordHelper() = default;
    ~ParallelRecordHelper() { Shutdown(); }

    /// 建池（管线初始化时调用一次）。`listCount` 会被裁到实际可用数。
    /// 【为什么没有 layout 参数】次级列表的来源是 `IRHIDevice::CreateSecondaryCommandList()`，
    /// 其描述符集布局随 `BeginSecondary(pso)` 的 PSO 走（与既有 Forward 用法一致）。
    bool Initialize(rhi::IRHIDevice* device, u32 listCount, const char* debugName);

    void Shutdown();

    [[nodiscard]] u32  ListCount() const { return static_cast<u32>(m_Lists.size()); }
    [[nodiscard]] bool IsReady()   const { return !m_Lists.empty(); }

    /// 并行录制 + 合并。`mainCmd` 是主线程列表（在其上 ExecuteSecondary）。
    /// @param drawCount 本帧要录制的 draw 数（0 或池未就绪时直接返回，调用方自行回退）
    void Record(rhi::IRHICommandList* mainCmd, u32 drawCount, rhi::IRHIPipelineState* pso,
                const RecordChunkFn& recordChunk);

private:
    rhi::IRHIDevice*                                  m_Device = nullptr;
    std::vector<std::unique_ptr<rhi::IRHICommandList>> m_Lists;
    const char*                                       m_DebugName = "SecRecord";
};

} // namespace he::render
