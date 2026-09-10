#pragma once

// ============================================================
// GI/GIRegistry.h — GI Provider 注册表 + 自动降级（M3 → P3 层栈）
//
// 可用性判断 = 管线能力（PipelineCaps：该管线是否提供此 GI 源）
//            ∧ 设备能力（rtSupported：光追源需硬件光追）
//
//   1. IsAvailable：查询某 GI 源在当前管线 + 设备下是否可用
//   2. Degrade：对 GIConfig 的四个通道层栈逐源裁剪（移除不可用源），
//               并保证每通道至少保留一个可用兜底源
// ============================================================

#include "Pipeline/LightingPass.h"
#include "GI/GIConfig.h"

namespace he::render {

class GIRegistry {
public:
    /// 某 GI 源是否可用（管线能力 ∧ 设备能力）
    static bool IsAvailable(GISourceId id, u32 pipelineCaps, bool rtSupported) {
        if (id == GISourceId::None) return false;
        const u32 cap = ToPipelineCap(id);
        if (cap == kPipelineGINone) return false;                 // 未注册的源
        if ((pipelineCaps & cap) != cap) return false;            // 管线不提供
        if (IsRayTracingSource(id) && !rtSupported) return false; // 设备无光追
        return true;
    }

    /// 对单个通道层栈逐源裁剪（移除不可用源；weight<=0 的源也一并清理）
    static void DegradeStack(GIChannelStack& st, u32 pipelineCaps, bool rtSupported) {
        for (u32 i = 0; i < st.count; ) {
            const GISourceId id = st.sources[i].id;
            if (st.sources[i].weight <= 0.0f || !IsAvailable(id, pipelineCaps, rtSupported)) {
                st.Remove(id);   // Remove 会前移后续元素，故索引不递增
            } else {
                i++;
            }
        }
    }

    /// 把 GIConfig 四个通道层栈中的不可用源全部裁剪
    static GIConfig Degrade(const GIConfig& c, u32 pipelineCaps, bool rtSupported) {
        GIConfig out = c;
        DegradeStack(out.diffuse,  pipelineCaps, rtSupported);
        DegradeStack(out.specular, pipelineCaps, rtSupported);
        DegradeStack(out.ao,       pipelineCaps, rtSupported);
        DegradeStack(out.shadow,   pipelineCaps, rtSupported);

        // ── 兜底：通道被裁空时补一个管线支持的源，避免该通道完全丢失 ──
        // 环境源（IBL）几乎所有管线都支持，作为最后兜底
        if (out.diffuse.count == 0 && IsAvailable(GISourceId::IBL, pipelineCaps, rtSupported)) {
            out.diffuse.Set(GISourceId::IBL, 1.0f);
        }
        if (out.specular.count == 0 && IsAvailable(GISourceId::IBL, pipelineCaps, rtSupported)) {
            out.specular.Set(GISourceId::IBL, 1.0f);
        }
        if (out.ao.count == 0 && IsAvailable(GISourceId::SSAO, pipelineCaps, rtSupported)) {
            out.ao.Set(GISourceId::SSAO, 1.0f);
        }
        if (out.shadow.count == 0 && IsAvailable(GISourceId::RasterShadow, pipelineCaps, rtSupported)) {
            out.shadow.Set(GISourceId::RasterShadow, 1.0f);
        }
        return out;
    }
};

} // namespace he::render
