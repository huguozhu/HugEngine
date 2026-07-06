#pragma once

#include "RHI/RHI.h"
#include "RHI/CommandList.h"
#include "RHI/QueryPool.h"
#include "Core/Types.h"
#include <vector>
#include <string>
#include <string_view>
#include <memory>

namespace he::render {

// ============================================================
// ProfilerManager — GPU 时间戳 Profiler
//
// 每 Pass 2 个 timestamp（start + end）× N Passes × 3 帧槽位
// 帧 N 写入 timestamp，帧 N+1 读回结果，帧 N+2 ImGui 显示
// ============================================================
class ProfilerManager {
public:
    ProfilerManager() = default;
    ~ProfilerManager();

    void Initialize(rhi::IRHIDevice* device, u32 maxPasses, u32 maxFramesInFlight);
    void Shutdown();

    /// 每帧开始时调用（重置 query pool + 读回上一帧结果）
    void BeginFrame(rhi::IRHICommandList* cmd);
    /// Pass 开始前插入 start timestamp
    void BeginPass(rhi::IRHICommandList* cmd, u32 passIndex, StringView name);
    /// Pass 结束后插入 end timestamp
    void EndPass(rhi::IRHICommandList* cmd, u32 passIndex);
    /// 帧结束后读回结果
    void EndFrame(rhi::IRHIDevice* device);

    struct PassProfile {
        std::string name;
        float       gpuMs = 0.0f;
    };
    const std::vector<PassProfile>& GetLastFrameData() const { return m_LastProfiled; }

private:
    struct PerFrame {
        std::unique_ptr<rhi::IRHIQueryPool> pool;
        std::vector<u64>  timestamps;   // [2*passIdx+0]=start, [2*passIdx+1]=end
        std::vector<std::string> names;  // pass names
        u32 passCount = 0;
    };

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_MaxPasses = 0;
    u32 m_MaxFramesInFlight = 3;
    u32 m_FrameIndex = 0;
    u32 m_ReadbackFrame = 1;  // 延迟 1 帧读回（等待 GPU 完成）
    float m_TimestampPeriod = 0.0f;  // ns per tick
    std::vector<PerFrame> m_Frames;
    std::vector<PassProfile> m_LastProfiled;
};

} // namespace he::render
