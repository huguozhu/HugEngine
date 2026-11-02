#pragma once

// ============================================================================
//  DXBC 绑定表（D3D11 后端）
//
//  D3D11 / SM 5.0 没有寄存器空间（space），且采样器只有 s0..s15、常量缓冲只有
//  b0..b13，所以构建期工具 ShaderToDxil --dxbc 会把着色器里的 vk::binding(B, S)
//  重新分配到 D3D11 的寄存器槽位。运行时 D3D11 后端需要知道
//  「描述符集 (set, binding) → D3D11 槽位」才能把描述符集里的资源绑对位置，
//  这张对照表就是本文件描述的「绑定表」。
//
//  【存放位置】绑定表附在 DXBC 容器之后，和字节码放在同一个字节数组里：
//
//      [ DXBC 容器 ][ DxbcBindTableHeader ][ DxbcBindEntry × entryCount ]
//
//  DXBC 容器自身的总长度记录在容器头的第 24 字节处（u32，小端），D3D11 创建
//  着色器时只传容器那一段；容器之后若跟着本表（以 kDxbcBindTableMagic 开头）即可解析。
//  这样 ShaderBytecode 只需要一个 dxbc 字段，生成的头文件也只有一个数组。
//
//  本头文件不依赖引擎其它头文件：构建工具（Tools/ShaderToDxil）也直接包含它。
// ============================================================================

#include <cstdint>
#include <cstring>

namespace he::rhi::dxbc {

/// 绑定表魔数："HEBT"（HugEngine Bind Table）
constexpr uint32_t kDxbcBindTableMagic   = 0x54424548u;
/// 绑定表格式版本，改动 DxbcBindEntry 布局时递增
constexpr uint32_t kDxbcBindTableVersion = 1u;

/// 资源类别，对应 D3D11 的四类寄存器（推送常量单列，便于后端识别常量环）
enum class DxbcBindType : uint32_t {
    SRV          = 0,   // tN：纹理 / 结构化缓冲 / 原始缓冲
    UAV          = 1,   // uN：可读写资源（像素着色器里已平移到渲染目标之后）
    Sampler      = 2,   // sN
    CBuffer      = 3,   // bN：描述符集里的常量缓冲
    PushConstant = 4,   // b0：[[vk::push_constant]] 块，由常量环提供
};

/// 推送常量块固定使用的常量缓冲寄存器
constexpr uint32_t kDxbcPushConstantSlot = 0u;
/// 描述符集常量缓冲的起始寄存器（b0 留给推送常量，D3D11 共 14 个：b0..b13）
constexpr uint32_t kDxbcFirstCBufferSlot = 1u;
constexpr uint32_t kDxbcMaxCBufferSlots  = 14u;
/// 采样器槽位上限（s0..s15）
constexpr uint32_t kDxbcMaxSamplerSlots  = 16u;
/// SRV 槽位上限（t0..t127）
constexpr uint32_t kDxbcMaxSRVSlots      = 128u;
/// UAV 槽位上限（特性级别 11_1：u0..u63）
constexpr uint32_t kDxbcMaxUAVSlots      = 64u;
/// 像素着色器的 UAV 与渲染目标共用编号空间，UAV 从这里开始（最多 8 个渲染目标）
constexpr uint32_t kDxbcPixelUAVBase     = 8u;

struct DxbcBindTableHeader {
    uint32_t magic      = kDxbcBindTableMagic;
    uint32_t version    = kDxbcBindTableVersion;
    uint32_t entryCount = 0;
};

struct DxbcBindEntry {
    uint32_t set     = 0;   // 源码 vk::binding 的描述符集号（推送常量为 0）
    uint32_t binding = 0;   // 源码 vk::binding 的绑定号（推送常量为 0）
    uint32_t type    = 0;   // DxbcBindType
    uint32_t slot    = 0;   // D3D11 寄存器槽位（首个元素）
    uint32_t count   = 1;   // 数组元素个数（非数组为 1），占用 [slot, slot + count)
};

/// 读取 DXBC 容器头里记录的容器总长度；数据不是 DXBC（魔数不是 "DXBC"）时返回 0
inline uint32_t GetDxbcContainerSize(const uint8_t* data, size_t size) {
    if (size < 28 || std::memcmp(data, "DXBC", 4) != 0) return 0;
    uint32_t total = 0;
    std::memcpy(&total, data + 24, sizeof(total));
    return total <= size ? total : 0;
}

/// 定位容器之后的绑定表。成功时返回表头指针，entries 指向第一项；没有表或格式不符时返回 nullptr
inline const DxbcBindTableHeader* FindDxbcBindTable(const uint8_t* data, size_t size,
                                                    const DxbcBindEntry** entries) {
    const uint32_t containerSize = GetDxbcContainerSize(data, size);
    if (containerSize == 0 || size - containerSize < sizeof(DxbcBindTableHeader)) return nullptr;
    const auto* header = reinterpret_cast<const DxbcBindTableHeader*>(data + containerSize);
    if (header->magic != kDxbcBindTableMagic || header->version != kDxbcBindTableVersion) return nullptr;
    const size_t need = sizeof(DxbcBindTableHeader) + size_t(header->entryCount) * sizeof(DxbcBindEntry);
    if (size - containerSize < need) return nullptr;
    if (entries) *entries = reinterpret_cast<const DxbcBindEntry*>(header + 1);
    return header;
}

} // namespace he::rhi::dxbc
