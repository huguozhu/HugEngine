// SurfaceCache_PageCheck.comp.slang — 页表 GPU/C++ 镜像一致性校验（步骤 14）
//
// 读 GPU 侧的页表缓冲，按**与 C++ 侧逐字一致**的公式算一个 FNV-1a 校验和写进输出缓冲。
// CPU 侧对同一份页表算 `SurfaceCachePageTable::Checksum()`，两者相等才说明"GPU 读到的布局/字段"
// 与 C++ 侧完全一致 —— 这比逐字段回读更能抓住"偏移错位"（任一字段位置变了，混出来的值都会变）。
//
// 【为什么是 32 位】64 位版（uint64_t）需要 SPIR-V 的 Int64 能力，而它要求
// VkPhysicalDeviceFeatures::shaderInt64 —— 本引擎没开：实测拿到
// "SPIR-V Capability Int64 was declared, but ... shaderInt64" 告警、管线无效、回读恒 0。
// 32 位 FNV-1a 足够当"镜像一致"的闸门，且不需要任何扩展能力。
#include "Lumen/SurfaceCache.slang"

[[vk::binding(0, 0)]] StructuredBuffer<SurfaceCachePageEntry> u_Pages;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>                 u_Out;   // 校验和（32 位）

[[vk::push_constant]] cbuffer PageCheckPC {
    uint4 dims;   // x = 页数
};

[numthreads(1, 1, 1)]
void main() {
    uint h = 2166136261u;   // 与 C++ 的 FNV-1a 偏移一致
    for (uint i = 0; i < dims.x; ++i) {
        const SurfaceCachePageEntry e = u_Pages[i];
        const uint f[6] = { e.state, e.pageIndex, e.cardIndex, e.lastUsedFrame, e.captureCount, e.flags };
        for (uint k = 0; k < 6u; ++k) {
            h = h ^ (f[k] * (k + 1u));
            h = h * 16777619u;
        }
    }
    u_Out[0] = h;
}
