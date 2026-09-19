// SurfaceCache.slang — Surface Cache 页表的数据布局（C++ / Slang **共享**，步骤 14）
//
// 【为什么放在共享头里】页表的每一项都要被两侧同时理解：C++ 侧是"页状态机 + 分配/淘汰"，
// GPU 侧是"捕获 pass / 着色 pass 按页读写"。两侧布局一旦不一致，症状是"页号对不上、捕获写错页"
// 这类**静默**错误 —— 所以这里用与 ShaderTypes.slang 相同的手法：一份定义、两边编译，
// 再用 C++ 侧的 static_assert 把大小钉死，另有一个 GPU 回读校验 pass 交叉验证（见 SurfaceCache_PageCheck）。
//
// 【六态】对照《Lumen设计与实现》§13：
//   Invalid → Requested → Allocating → Capturing → Captured → Dirty →（回到 Invalid 或 Capturing）
#ifndef HUGENGINE_SURFACE_CACHE
#define HUGENGINE_SURFACE_CACHE

#ifdef __cplusplus
    #include <cstdint>
    #define SC_UINT  uint32_t
    #define SC_STRUCT struct alignas(16)   // Slang 自动对齐，C++ 需显式声明
    #define SC_CONST(decl) static constexpr decl
#else
    #define SC_UINT  uint
    #define SC_STRUCT struct
    #define SC_CONST(decl) static const decl
#endif

// ── 页状态（六态）──
SC_CONST(SC_UINT kSCPageState_Invalid    = 0u);   // 未分配 / 已释放
SC_CONST(SC_UINT kSCPageState_Requested  = 1u);   // 已收到请求，等待分配物理页
SC_CONST(SC_UINT kSCPageState_Allocating = 2u);   // 正在分配（写页表/清零）
SC_CONST(SC_UINT kSCPageState_Capturing  = 3u);   // 正在捕获（GPU 写入 atlas）
SC_CONST(SC_UINT kSCPageState_Captured   = 4u);   // 捕获完成，内容有效
SC_CONST(SC_UINT kSCPageState_Dirty      = 5u);   // 内容过期（几何/光照变了），待重捕
SC_CONST(SC_UINT kSCPageState_Count      = 6u);

// ── 页表项（32 B：8 × u32，std430 下两侧一致）──
SC_STRUCT SurfaceCachePageEntry {
    SC_UINT state;          // 六态之一
    SC_UINT pageIndex;      // atlas 内的物理页号；Invalid/Requested 时为 0xFFFFFFFF
    SC_UINT cardIndex;      // 对应步骤 13 的卡片下标
    SC_UINT lastUsedFrame;  // 最近使用的帧号（LRU 淘汰用）
    SC_UINT captureCount;   // 被捕获过多少次（调试/反馈用）
    SC_UINT flags;          // 位标志（0x1 = 请求重捕获）
    SC_UINT reserved0;
    SC_UINT reserved1;
};

// ── 分配请求（16 B）──
SC_STRUCT SurfaceCacheRequest {
    SC_UINT cardIndex;      // 要哪张卡
    SC_UINT priority;       // 越大越先分配
    SC_UINT frame;          // 产生请求的帧号
    SC_UINT pageIndex;      // 目标页（-1 = 由分配器决定）
};

SC_CONST(SC_UINT kSCInvalidPage = 0xFFFFFFFFu);   // "没有物理页"

#endif // HUGENGINE_SURFACE_CACHE
