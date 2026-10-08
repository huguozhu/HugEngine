#pragma once

#include "Core/Types.h"

#include <unordered_map>
#include <vector>

namespace he::rhi { class IRHIBuffer; }   // 前向声明：注册表只**借**指针，不需要缓冲定义

// ============================================================
// MeshRegistry —— 渲染侧的网格注册表（阶段 1，方案 §14 附录 E）
//
// 【它解决什么】快照里**不允许带指针**（§4.2 硬约束），所以 `SnapshotDrawItem` 只能带 `meshIndex`；
// 而渲染侧目前是通过 `MeshComponent*` 拿顶点/索引缓冲与材质（`SceneRenderer::Prepare` 的 `DrawItem`、
// `ForwardPipeline` 的骨骼上传）。注册表就是 `meshIndex → 渲染侧资源` 的那张表 —— 附录 B1 里
// "渲染期世界依赖 82 处"的主要来源都要靠它收敛。
//
// 【只借不拥有】条目里的缓冲指针由组件/资产持有；注册表不负责释放。注册方必须遵守 §14.3 的
// 生命周期规则（尤其"**禁止帧内注销**"，否则当帧已录制的绘制会指到空记录）。
//
// 【与 T0.7 的关系】本版是**裸指针过渡版**：索引复用后，旧索引会指向新条目（见 `Find` 注释）。
// 将来把条目换成 `RHIBufferHandle`（带 generation）后，悬挂引用会被**代次**检出 ——
// 这正是 §12 附录 C 里 A-3 的做法。
// ============================================================

namespace he::render {

/// 一条网格记录（只存渲染侧可达的值）
struct MeshRegistryEntry {
    rhi::IRHIBuffer* vertexBuffer = nullptr;   // 顶点缓冲（所有权在组件/资产）
    rhi::IRHIBuffer* indexBuffer  = nullptr;   // 索引缓冲
    u32  indexCount = 0;                       // 索引数（间接绘制参数的兜底来源）
    u32  vertexCount = 0;                      // 顶点数（MeshBatcher 合批要用：它按顶点数搬运与累加 baseVertex）
    u32  materialID = 0;                       // bindless 纹理基索引（与组件同源）
    bool instanced  = false;                   // 实例化网格：顶点由实例路径提供
};

class MeshRegistry {
public:
    /// 注册或**更新**（同一 key 重复注册 = 更新既有条目，用于组件重建缓冲的场景）；返回从 1 起的索引
    u32 Register(const void* key, const MeshRegistryEntry& entry) {
        auto it = m_KeyToIndex.find(key);
        if (it != m_KeyToIndex.end()) {
            m_Records[it->second].entry = entry;   // 更新：索引不变（调用方缓存的 meshIndex 继续有效）
            return it->second;
        }

        u32 index = 0;
        if (!m_FreeIndices.empty()) {
            index = m_FreeIndices.back();
            m_FreeIndices.pop_back();
        } else {
            m_Records.push_back(Record{});         // [0] 是哨兵，故首个真实索引 = 1
            index = static_cast<u32>(m_Records.size()) - 1u;
        }
        m_Records[index].key   = key;
        m_Records[index].entry = entry;
        m_KeyToIndex.emplace(key, index);
        ++m_LiveCount;
        return index;
    }

    /// 注销（索引回空闲表）。**禁止在帧内调用**（§14.3）：当帧已录制的绘制会指到空记录。
    void Unregister(const void* key) {
        auto it = m_KeyToIndex.find(key);
        if (it == m_KeyToIndex.end()) return;
        const u32 index = it->second;
        m_Records[index].key   = nullptr;          // 置空 ⇒ Find(index) 从此返回 nullptr
        m_Records[index].entry = MeshRegistryEntry{};
        m_FreeIndices.push_back(index);
        m_KeyToIndex.erase(it);
        if (m_LiveCount > 0u) --m_LiveCount;
    }

    /// 解析：越界或**已注销**返回 nullptr。
    /// 【注意索引复用】注销后该索引可能被新注册的网格复用 ⇒ 旧索引会指向**新条目**。
    /// 这正是"下一步把条目换成带代次的 `RHIBufferHandle`"的理由（见文件头说明）。
    [[nodiscard]] const MeshRegistryEntry* Find(u32 meshIndex) const {
        if (meshIndex == 0u || meshIndex >= m_Records.size()) return nullptr;
        const Record& r = m_Records[meshIndex];
        return r.key ? &r.entry : nullptr;
    }

    [[nodiscard]] u32 Count() const { return m_LiveCount; }

    void Clear() {
        m_Records.clear();
        m_Records.push_back(Record{});             // 保留哨兵
        m_KeyToIndex.clear();
        m_FreeIndices.clear();
        m_LiveCount = 0u;
    }

private:
    struct Record {
        const void*       key = nullptr;           // nullptr = 该索引空闲/已注销
        MeshRegistryEntry entry{};
    };

    std::vector<Record>                  m_Records{Record{}};   // 索引 0 = 哨兵（未注册）
    std::unordered_map<const void*, u32> m_KeyToIndex;
    std::vector<u32>                     m_FreeIndices;
    u32                                  m_LiveCount = 0;
};

} // namespace he::render
