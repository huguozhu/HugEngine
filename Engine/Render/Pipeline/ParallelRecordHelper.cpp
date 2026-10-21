#include "Pipeline/ParallelRecordHelper.h"

#include "RHI/RHI.h"
#include "RHI/ThreadAffinity.h"   // rhi::SecondaryRecordScope（两级归属的显式声明）
#include "Threading/JobSystem.h"

#include <algorithm>

namespace he::render {

bool ParallelRecordHelper::Initialize(rhi::IRHIDevice* device, u32 listCount, const char* debugName) {
    if (!device || listCount == 0u) return false;
    m_Device    = device;
    m_DebugName = debugName ? debugName : "SecRecord";

    m_Lists.clear();
    m_Lists.reserve(listCount);
    for (u32 i = 0; i < listCount; ++i) {
        auto cl = device->CreateSecondaryCommandList();
        if (!cl) break;   // 建不出来就少一条（Record 会按实际条数分块）
        m_Lists.push_back(std::move(cl));
    }
    return !m_Lists.empty();
}

void ParallelRecordHelper::Shutdown() {
    m_Lists.clear();
    m_Device = nullptr;
}

void ParallelRecordHelper::Record(rhi::IRHICommandList* mainCmd, u32 drawCount,
                                  rhi::IRHIPipelineState* pso, const RecordChunkFn& recordChunk) {
    if (!mainCmd || !pso || drawCount == 0u || m_Lists.empty()) return;

    const u32 listCount = static_cast<u32>(m_Lists.size());
    const u32 numChunks = std::min(listCount, drawCount);
    const u32 chunkSize = (drawCount + numChunks - 1u) / numChunks;

    std::vector<std::function<void()>> tasks;
    tasks.reserve(numChunks);
    for (u32 t = 0; t < numChunks; ++t) {
        tasks.push_back([&, t]() {
            // 【两级归属】worker 必须先声明"我是次级命令缓冲录制者"：`BeginSecondary`/`End` 上的
            // 断言允许"拥有线程或已声明的录制者"，不声明的线程照样命中。作用域结束自动撤销。
            rhi::SecondaryRecordScope recordScope;

            const u32 start = t * chunkSize;
            const u32 end   = std::min(start + chunkSize, drawCount);
            if (start >= end) return;

            auto& secCmd = m_Lists[t];
            secCmd->BeginSecondary(pso);
            recordChunk(*secCmd, start, end);   // 管线自己的 draw 循环（含 viewport/scissor/绑定）
            secCmd->End();
        });
    }

    JobSystem::Instance().ParallelInvoke(tasks);

    // 按**块序**合并：绘制顺序确定，与线程完成序无关
    for (u32 t = 0; t < numChunks; ++t) {
        if (t * chunkSize >= drawCount) continue;
        mainCmd->ExecuteSecondary(m_Lists[t].get());
    }
}

} // namespace he::render
