// ============================================================
// SkinnedMeshBuffers.cpp — 骨骼矩阵缓冲的渲染侧状态表（阶段 1 §15.1 第③段）
//
// 与 `InstanceCuller` 的实例缓冲状态表同构：同样的"版本号判据 + 有界退役 + 按帧回收 +
// 索引复用识别"。两份一起看，就是这个仓库"逐网格 GPU 缓冲状态搬离组件"的标准做法。
// ============================================================

#include "Pipeline/SkinnedMeshBuffers.h"
#include "Core/Log.h"

#include <cstring>

namespace he::render {

void SkinnedMeshBuffers::BeginFrame(rhi::IRHIDevice* device) {
    for (auto it = m_States.begin(); it != m_States.end(); ) {
        State& st = it->second;

        // 「上一帧起就没再出现」⇒ 组件已销毁（或该索引不再使用）：整条状态连同 GPU 资源回收。
        // 【为什么以"上一帧"为准】快照每帧都包含全部骨骼网格；本帧刚创建的条目在 `Upload` 里
        // 置 `seenThisFrame`。若以"本帧未见"为准，就会把"本帧还没轮到绘制"的条目误回收。
        if (!st.seenLastFrame) {
            if (device && st.ssboHandle != 0) device->GetBindlessHeap()->ReleaseBuffer(st.ssboHandle);
            st.retired.FlushAll();
            it = m_States.erase(it);
            continue;
        }

        // 帧边界推进退役队列（有界释放；先推进本帧再入队本帧退役的资源）
        st.retired.Advance();
        st.seenLastFrame = st.seenThisFrame;
        st.seenThisFrame = false;
        ++it;
    }
}

u32 SkinnedMeshBuffers::Upload(rhi::IRHIDevice* device, u32 meshIndex, u64 ownerEntity,
                               const float4x4* matrices, u32 count, u32 version) {
    if (!device || meshIndex == 0u) return 0;      // 0 = 未注册（与注册表的哨兵口径一致）
    if (count == 0u || !matrices) return 0;

    // 首次出现即建条目（两个 seen 都置真：本帧已见 ⇒ 下一帧的回收判定不该把它算作"未见"）
    auto [it, inserted] = m_States.try_emplace(meshIndex);
    State& st = it->second;
    if (inserted) {
        st.ownerEntity   = ownerEntity;
        st.seenThisFrame = true;
        st.seenLastFrame = true;
    }
    st.seenThisFrame = true;

    // 索引复用识别：同一个 meshIndex 换了来源实体 ⇒ 这是**另一个网格**，上一份缓冲的内容与
    // 版本号都不可信（旧 `uploadedVersion` 可能恰好等于新组件的版本号 ⇒ 漏传 ⇒ 用上一个
    // 组件的骨骼矩阵蒙皮）。直接丢弃旧缓冲并强制重传（本索引换了主人 ⇒ 无人再引用它）。
    if (st.ownerEntity != ownerEntity) {
        if (st.ssboHandle != 0) device->GetBindlessHeap()->ReleaseBuffer(st.ssboHandle);
        st.retired.FlushAll();
        st.buffer.reset();
        st.ssboHandle      = 0;
        st.capacity        = 0;
        st.hasUpload       = false;
        st.uploadedVersion = 0;
        st.boneCount       = 0;
        st.ownerEntity     = ownerEntity;
    }

    // 版本没变 ⇒ 数据就是上次上传的那一份（跨帧幂等：同一帧被消费多次也不会重复上传）
    if (st.hasUpload && st.uploadedVersion == version) return st.ssboHandle;

    const bool needGrow = (!st.buffer || st.capacity < count);
    if (needGrow) {
        rhi::BufferDesc desc;
        desc.size        = sizeof(float4x4) * count;
        desc.usage       = rhi::BufferUsage::Storage;
        desc.initialData = matrices;
        desc.cpuAccess   = true;
        // 旧缓冲退役（N 帧延迟释放）+ 释放旧 bindless 槽位（任务 23 的槽位回收）
        if (st.buffer) {
            device->GetBindlessHeap()->ReleaseBuffer(st.ssboHandle);
            st.retired.Retire(std::move(st.buffer));
        }
        st.buffer = device->CreateBuffer(desc);
        if (!st.buffer) {
            st.ssboHandle = 0;
            st.capacity   = 0;
            st.hasUpload  = false;
            return 0;
        }
        st.capacity   = count;
        st.ssboHandle = device->GetBindlessHeap()->RegisterBuffer(st.buffer.get());
    } else {
        // 复用缓冲：Map 原地写入最新矩阵（容量可能大于矩阵数，只写前 count 个）
        void* mapped = st.buffer->Map();
        if (mapped) {
            std::memcpy(mapped, matrices, sizeof(float4x4) * count);
            st.buffer->Unmap();
        }
    }
    st.uploadedVersion = version;
    st.hasUpload       = true;
    st.boneCount       = count;
    return st.ssboHandle;
}

SkinnedMeshBuffers::State* SkinnedMeshBuffers::Find(u32 meshIndex) {
    auto it = m_States.find(meshIndex);
    return it == m_States.end() ? nullptr : &it->second;
}

SkinnedMeshBuffers::Stats SkinnedMeshBuffers::GetStats(u32 meshIndex) const {
    Stats out;
    auto it = m_States.find(meshIndex);
    if (it == m_States.end()) return out;
    const State& st = it->second;
    out.valid      = st.hasUpload;
    out.ssboHandle = st.ssboHandle;
    out.capacity   = st.capacity;
    out.retired    = st.retired.GetPendingCount();
    out.boneCount  = st.boneCount;
    return out;
}

void SkinnedMeshBuffers::Shutdown(rhi::IRHIDevice* device) {
    for (auto& kv : m_States) {
        State& st = kv.second;
        if (device && st.ssboHandle != 0) device->GetBindlessHeap()->ReleaseBuffer(st.ssboHandle);
        st.retired.FlushAll();   // 设备即将销毁：不再需要 N 帧延迟
        st.buffer.reset();
    }
    m_States.clear();
}

} // namespace he::render
