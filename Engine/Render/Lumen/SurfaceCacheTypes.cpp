// ============================================================
// Lumen/SurfaceCacheTypes.cpp — 页状态机的实现（步骤 14）
// ============================================================

#include "Lumen/SurfaceCacheTypes.h"

#include "Core/Log.h"

namespace he::render {

const char* SurfaceCachePageStateName(u32 state) {
    switch (state) {
        case kSCPageState_Invalid:    return "Invalid";
        case kSCPageState_Requested:  return "Requested";
        case kSCPageState_Allocating: return "Allocating";
        case kSCPageState_Capturing:  return "Capturing";
        case kSCPageState_Captured:   return "Captured";
        case kSCPageState_Dirty:      return "Dirty";
        default:                      return "Unknown";
    }
}

bool IsLegalPageTransition(u32 from, u32 to) {
    if (to == kSCPageState_Invalid) return true;   // 释放/淘汰永远合法（兜底）
    switch (from) {
        case kSCPageState_Invalid:    return to == kSCPageState_Requested;
        case kSCPageState_Requested:  return to == kSCPageState_Allocating;
        case kSCPageState_Allocating: return to == kSCPageState_Capturing;
        case kSCPageState_Capturing:  return to == kSCPageState_Captured;
        case kSCPageState_Captured:   return to == kSCPageState_Dirty;
        case kSCPageState_Dirty:      return to == kSCPageState_Capturing;
        default:                      return false;
    }
}

void SurfaceCachePageTable::Resize(u32 pageCount) {
    m_Entries.assign(pageCount, SurfaceCachePageEntry{});
    for (auto& e : m_Entries) {
        e.state     = kSCPageState_Invalid;
        e.pageIndex = kSCInvalidPage;
    }
}

void SurfaceCachePageTable::Reset(std::vector<SurfaceCachePageEntry> entries) {
    m_Entries = std::move(entries);
}

bool SurfaceCachePageTable::Transition(u32 page, u32 to) {
    if (page >= m_Entries.size()) {
        HE_ASSERT(false, "SurfaceCachePageTable: 页号越界");
        return false;
    }
    SurfaceCachePageEntry& e = m_Entries[page];
    if (!IsLegalPageTransition(e.state, to)) {
        // 非法迁移：Debug 直接断言（验收要求"非法迁移可断言"）；Release 下只记录并拒绝
        HE_CORE_ERROR("SurfaceCachePageTable: 非法迁移 page={} {} -> {}", page,
                      SurfaceCachePageStateName(e.state), SurfaceCachePageStateName(to));
        HE_ASSERT(false, "SurfaceCachePageTable: 非法页状态迁移");
        return false;
    }
    e.state = to;
    return true;
}

bool SurfaceCachePageTable::Request(u32 page, u32 cardIndex, u32 frame) {
    if (!Transition(page, kSCPageState_Requested)) return false;
    SurfaceCachePageEntry& e = m_Entries[page];
    e.cardIndex     = cardIndex;
    e.lastUsedFrame = frame;
    e.pageIndex     = kSCInvalidPage;
    e.flags         = 0u;
    return true;
}

bool SurfaceCachePageTable::Allocate(u32 page, u32 physicalPage) {
    if (!Transition(page, kSCPageState_Allocating)) return false;
    m_Entries[page].pageIndex = physicalPage;
    return true;
}

bool SurfaceCachePageTable::BeginCapture(u32 page) {
    return Transition(page, kSCPageState_Capturing);
}

bool SurfaceCachePageTable::EndCapture(u32 page, bool ok) {
    if (!ok) return Transition(page, kSCPageState_Invalid);
    if (!Transition(page, kSCPageState_Captured)) return false;
    ++m_Entries[page].captureCount;
    return true;
}

bool SurfaceCachePageTable::MarkDirty(u32 page) {
    return Transition(page, kSCPageState_Dirty);
}

bool SurfaceCachePageTable::Evict(u32 page) {
    if (page >= m_Entries.size()) return false;
    if (m_Entries[page].state == kSCPageState_Invalid) return true;   // 幂等
    const bool ok = Transition(page, kSCPageState_Invalid);
    m_Entries[page].pageIndex = kSCInvalidPage;
    m_Entries[page].flags     = 0u;
    return ok;
}

void SurfaceCachePageTable::Touch(u32 page, u32 frame) {
    if (page < m_Entries.size()) m_Entries[page].lastUsedFrame = frame;
}

u32 SurfaceCachePageTable::Count(u32 state) const {
    u32 n = 0;
    for (const auto& e : m_Entries) if (e.state == state) ++n;
    return n;
}

bool SurfaceCachePageTable::AnyLive() const {
    for (const auto& e : m_Entries) if (e.state != kSCPageState_Invalid) return true;
    return false;
}

u32 SurfaceCachePageTable::Checksum() const {
    // 与 SurfaceCache_PageCheck.comp.slang **逐字一致**（32 位 FNV-1a；字段位置也参与混合，
    // 所以任一字段的偏移错了、或项的顺序变了，混出来的值都会变）。
    // 【为什么不用 64 位】SPIR-V 的 Int64 能力要求 VkPhysicalDeviceFeatures::shaderInt64，
    // 本引擎没开 ⇒ 64 位版 shader 会拿到 "Capability Int64 declared but not enabled" 告警、
    // 管线无效、回读恒 0（实测）。32 位足够做"镜像一致性"闸门。
    u32 h = 2166136261u;
    for (const auto& e : m_Entries) {
        const u32 f[6] = { e.state, e.pageIndex, e.cardIndex, e.lastUsedFrame, e.captureCount, e.flags };
        for (u32 k = 0; k < 6u; ++k) {
            h ^= f[k] * (k + 1u);
            h *= 16777619u;
        }
    }
    return h;
}

// ── 布局闸门：两侧（C++ / Slang）不一致时**编译期**就报错 ──
static_assert(sizeof(SurfaceCachePageEntry) == 32, "SurfaceCachePageEntry 必须是 8 × u32（与 Slang 镜像一致）");
static_assert(sizeof(SurfaceCacheRequest)   == 16, "SurfaceCacheRequest 必须是 4 × u32");
static_assert(offsetof(SurfaceCachePageEntry, state)          ==  0, "字段偏移必须与 shader 一致");
static_assert(offsetof(SurfaceCachePageEntry, pageIndex)      ==  4, "字段偏移必须与 shader 一致");
static_assert(offsetof(SurfaceCachePageEntry, cardIndex)      ==  8, "字段偏移必须与 shader 一致");
static_assert(offsetof(SurfaceCachePageEntry, lastUsedFrame)  == 12, "字段偏移必须与 shader 一致");
static_assert(offsetof(SurfaceCachePageEntry, captureCount)   == 16, "字段偏移必须与 shader 一致");
static_assert(offsetof(SurfaceCachePageEntry, flags)          == 20, "字段偏移必须与 shader 一致");

} // namespace he::render
