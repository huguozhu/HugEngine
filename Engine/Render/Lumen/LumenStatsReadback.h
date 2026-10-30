#pragma once

#include "RHI/RHI.h"

#include <cstring>
#include <vector>

// ============================================================
// LumenStatsReadback —— GPU 原子累加统计的确定性回读
//
// 【要解决什么】Lumen 的诊断统计（覆盖像素、命中数、噪声、远场对照…）由计算着色器原子累加到
// 一块 CPU 可映射的缓冲里，此前 CPU 在录制下一帧时**直接读、直接 memset 清零**这块缓冲：
//   · 读：GPU 未必执行完上一帧（飞行帧最多 kMaxFramesInFlight 帧）⇒ 读到半截或更早的值；
//   · 清：CPU 的清零与 GPU 的累加没有先后关系 ⇒ 可能清在上一帧执行之前，两帧叠加。
// 实测 D3D12 下统计持续累积、与 Vulkan 读数差两个数量级。
//
// 【做法】与屏幕探针计数（LumenScene_SurfaceCache.cpp）同一套：
//   · 计数器本体只给 GPU 用；清零在 GPU 时间线上做（从全零缓冲拷贝）；
//   · 每次"读 + 清"的位置先把当前累计值拷进本槽位的回读副本，再清零；
//   · CPU 读的是 kMaxFramesInFlight 次之前拷出的副本：那一帧的命令列表 Begin 时已等过它的围栏，
//     GPU 一定执行完了 ⇒ 读数完整、确定，代价是统计晚几帧。
// 用法（每帧在原来"先读上一帧、再清零"的位置）：
//     if (stats.Read(st)) { ...用上一帧的统计... }
//     stats.RecordSnapshotAndClear(cmd);   // 之后再派发本帧
// ============================================================

namespace he::render {

class LumenStatsReadback {
public:
    /// 创建计数器（GPU 用）、全零源与 kMaxFramesInFlight 份回读副本；bytes 为统计总字节数
    bool Create(rhi::IRHIDevice* device, u32 bytes) {
        Destroy();
        if (!device || bytes == 0u) return false;
        m_Device = device;
        m_Bytes  = bytes;
        rhi::BufferDesc lb;
        lb.size  = bytes;
        lb.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst;
        m_Live = device->CreateBufferHandle(lb);
        // 全零源：从它拷贝即可在 GPU 时间线上清零
        std::vector<u8> zeros(bytes, 0u);
        rhi::BufferDesc zb;
        zb.size = bytes; zb.usage = rhi::BufferUsage::TransferSrc; zb.cpuAccess = true; zb.initialData = zeros.data();
        m_Zero = device->CreateBufferHandle(zb);
        // 回读副本：拷贝目标且要 CPU 读，带 Storage 才能放进 GPU 可写、CPU 可读的堆（D3D12 的 UPLOAD 堆 GPU 不可写）
        rhi::BufferDesc rb;
        rb.size = bytes; rb.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst; rb.cpuAccess = true;
        for (u32 k = 0; k < rhi::kMaxFramesInFlight; ++k) {
            m_Readback[k] = device->CreateBufferHandle(rb);
            rhi::IRHIBuffer* b = device->ResolveBuffer(m_Readback[k]);
            m_Mapped[k] = b ? b->Map() : nullptr;
            if (m_Mapped[k]) std::memset(m_Mapped[k], 0, bytes);
            m_Written[k] = false;
        }
        m_Slot = 0;
        return m_Live.IsValid() && m_Zero.IsValid();
    }

    void Destroy() {
        if (!m_Device) return;
        for (u32 k = 0; k < rhi::kMaxFramesInFlight; ++k) {
            if (m_Mapped[k]) {
                if (rhi::IRHIBuffer* b = m_Device->ResolveBuffer(m_Readback[k])) b->Unmap();
                m_Mapped[k] = nullptr;
            }
            m_Device->DestroyBufferHandle(m_Readback[k]);
            m_Readback[k] = {};
            m_Written[k] = false;
        }
        m_Device->DestroyBufferHandle(m_Live);
        m_Device->DestroyBufferHandle(m_Zero);
        m_Live = {};
        m_Zero = {};
        m_Device = nullptr;
    }

    [[nodiscard]] bool IsValid() const { return m_Live.IsValid(); }
    /// 着色器绑定的计数器本体
    [[nodiscard]] rhi::IRHIBuffer* Live() const { return m_Device ? m_Device->ResolveBuffer(m_Live) : nullptr; }

    /// 读出 kMaxFramesInFlight 次之前拷出的统计（out 至少 bytes 字节）；该槽位还没写过时返回 false
    bool Read(void* out) const {
        if (!out || !m_Written[m_Slot] || !m_Mapped[m_Slot]) return false;
        std::memcpy(out, m_Mapped[m_Slot], m_Bytes);
        return true;
    }

    /// 把当前累计值拷进本槽位的回读副本，再在 GPU 时间线上清零（替代 CPU 的"读后 memset"）
    void RecordSnapshotAndClear(rhi::IRHICommandList* cmd) {
        rhi::IRHIBuffer* live = Live();
        rhi::IRHIBuffer* zero = m_Device ? m_Device->ResolveBuffer(m_Zero) : nullptr;
        rhi::IRHIBuffer* rb   = m_Device ? m_Device->ResolveBuffer(m_Readback[m_Slot]) : nullptr;
        if (!cmd || !live || !zero || !rb) return;
        // 之前的计算着色器累加 → 拷贝读
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::Transfer,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySrc);
        cmd->CopyBuffer(live, rb, m_Bytes, 0, 0);
        // 拷贝读完 → 清零写 → 之后的计算着色器累加
        cmd->PipelineBarrier(rhi::PipelineStage::Transfer, rhi::PipelineStage::Transfer,
                             rhi::ResourceState::CopySrc, rhi::ResourceState::CopyDst);
        cmd->CopyBuffer(zero, live, m_Bytes, 0, 0);
        cmd->PipelineBarrier(rhi::PipelineStage::Transfer, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::CopyDst, rhi::ResourceState::UnorderedAccess);
        m_Written[m_Slot] = true;
        m_Slot = (m_Slot + 1u) % rhi::kMaxFramesInFlight;
    }

private:
    rhi::IRHIDevice*      m_Device = nullptr;
    u32                   m_Bytes  = 0;
    rhi::RHIBufferHandle  m_Live{};
    rhi::RHIBufferHandle  m_Zero{};
    rhi::RHIBufferHandle  m_Readback[rhi::kMaxFramesInFlight]{};
    void*                 m_Mapped[rhi::kMaxFramesInFlight]{};
    bool                  m_Written[rhi::kMaxFramesInFlight]{};
    u32                   m_Slot = 0;   // 下一次快照写入、也是本次 Read 读取的槽位
};

} // namespace he::render
