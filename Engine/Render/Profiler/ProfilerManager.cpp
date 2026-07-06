// Profiler/ProfilerManager.cpp — GPU 时间戳 Profiler 实现
#include "ProfilerManager.h"
#include "Core/Log.h"
#include <cstring>
#include <algorithm>

namespace he::render {

ProfilerManager::~ProfilerManager() { Shutdown(); }

void ProfilerManager::Initialize(rhi::IRHIDevice* device, u32 maxPasses, u32 maxFramesInFlight) {
    m_Device    = device;
    m_MaxPasses = maxPasses;
    m_MaxFramesInFlight = maxFramesInFlight;
    m_TimestampPeriod = device->GetTimestampPeriod() * 1e-6f;  // ns → ms

    m_Frames.resize(maxFramesInFlight);
    for (u32 i = 0; i < maxFramesInFlight; ++i) {
        m_Frames[i].pool = device->CreateQueryPool(maxPasses * 2);  // start + end per pass
        m_Frames[i].timestamps.resize(maxPasses * 2);
        m_Frames[i].names.resize(maxPasses);
    }
    m_LastProfiled.resize(maxPasses);
    m_FrameIndex    = 0;
    HE_CORE_INFO("ProfilerManager 初始化完成: {} passes, {} frames", maxPasses, maxFramesInFlight);
}

void ProfilerManager::Shutdown() {
    m_Frames.clear();
    m_Device = nullptr;
}

void ProfilerManager::BeginFrame(rhi::IRHICommandList* cmd) {
    auto& frame = m_Frames[m_FrameIndex];
    frame.passCount = 0;
    cmd->ResetQueryPool(frame.pool.get());

    // 读回 2 帧前的结果（延迟 2 帧，确保 GPU 已完成写入）
    u32 rbIdx = (m_FrameIndex + m_MaxFramesInFlight - 2) % m_MaxFramesInFlight;
    auto& rbFrame = m_Frames[rbIdx];
    if (rbFrame.passCount > 0) {
        cmd->GetQueryResults(rbFrame.pool.get(), 0, rbFrame.passCount * 2,
                              rbFrame.timestamps.data());
        float total = 0;
        for (u32 i = 0; i < rbFrame.passCount; ++i) {
            u64 start = rbFrame.timestamps[i * 2];
            u64 end   = rbFrame.timestamps[i * 2 + 1];
            float ms  = float(end - start) * m_TimestampPeriod;
            m_LastProfiled[i].name   = rbFrame.names[i].empty() ? "?" : rbFrame.names[i];
            m_LastProfiled[i].gpuMs  = ms;
            total += ms;
        }
        for (u32 i = rbFrame.passCount; i < m_MaxPasses; ++i)
            m_LastProfiled[i].gpuMs = -1;  // 标记未使用
    }
}

void ProfilerManager::BeginPass(rhi::IRHICommandList* cmd, u32 passIndex, StringView name) {
    auto& frame = m_Frames[m_FrameIndex];
    if (passIndex >= m_MaxPasses) return;
    cmd->WriteTimestamp(frame.pool.get(), passIndex * 2);
    frame.names[passIndex] = name;
    frame.passCount = std::max(frame.passCount, passIndex + 1);
}

void ProfilerManager::EndPass(rhi::IRHICommandList* cmd, u32 passIndex) {
    auto& frame = m_Frames[m_FrameIndex];
    if (passIndex >= m_MaxPasses) return;
    cmd->WriteTimestamp(frame.pool.get(), passIndex * 2 + 1);
}

void ProfilerManager::EndFrame(rhi::IRHIDevice* device) {
    m_FrameIndex = (m_FrameIndex + 1) % m_MaxFramesInFlight;
}

} // namespace he::render
