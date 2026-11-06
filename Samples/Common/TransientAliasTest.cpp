// ============================================================
// TransientAliasTest.cpp — P6：瞬态纹理 / 内存别名的真实用例自检实现
//
// 只使用 IRHIDevice / IRHITexture / IRHICommandList 三个 RHI 接口，
// D3D12 与 Vulkan 走同一份代码，判据一致。
// ============================================================
#include "TransientAliasTest.h"

#include "RHI/RHI.h"
#include "Core/Log.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace he::samples {

namespace {

struct AliasCase {
    const char* name;
    u32         firstPass;
    u32         lastPass;
};

/// 把字节数格式化成人类可读的 KB/MB（日志用）
const char* BytesToText(u64 bytes, char* buf, usize bufSize) {
    if (bytes >= 1024ull * 1024ull)
        std::snprintf(buf, bufSize, "%.2f MB (%llu B)", double(bytes) / (1024.0 * 1024.0),
                      static_cast<unsigned long long>(bytes));
    else if (bytes >= 1024ull)
        std::snprintf(buf, bufSize, "%.2f KB (%llu B)", double(bytes) / 1024.0,
                      static_cast<unsigned long long>(bytes));
    else
        std::snprintf(buf, bufSize, "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

} // namespace

bool RunTransientAliasTest(he::rhi::IRHIDevice* device) {
    if (!device) {
        HE_CORE_ERROR("[P6] 设备为空，无法运行瞬态别名自检");
        return false;
    }

    // 生命周期两两重叠关系：A↔B 重叠（pass 2）、B↔C 重叠（pass 3）、A↔C 不重叠
    const AliasCase cases[3] = {
        { "A[1,2]", 1, 2 },
        { "B[2,3]", 2, 3 },
        { "C[3,4]", 3, 4 },
    };

    // 三张同规格纹理：256x256 RGBA8 渲染目标（同规格才能让"共用内存"的偏移直接可比）
    he::rhi::TextureDesc desc;
    desc.width  = 256;
    desc.height = 256;
    desc.format = he::rhi::Format::RGBA8_UNORM;
    desc.usage  = he::rhi::TextureUsage::RenderTarget | he::rhi::TextureUsage::ShaderResource;

    HE_CORE_INFO("[P6] ===== 瞬态纹理内存别名自检开始（三张 256x256 RGBA8 瞬态纹理）=====");

    // 【必须先推进一帧的瞬态资源】：D3D12 的瞬态堆是在 AdvanceTransientResources() 里
    // 创建/重置的（RenderGraph::Execute 每帧开头调用它）；样例正常路径里这一步由第一帧完成，
    // 而本自检在第一帧之前跑，所以这里显式调用一次。
    device->AdvanceTransientResources();

    std::vector<std::unique_ptr<he::rhi::IRHITexture>> textures;
    textures.reserve(3);
    u64 offsets[3] = { 0, 0, 0 };
    u64 sizes[3]   = { 0, 0, 0 };

    for (u32 i = 0; i < 3; ++i) {
        auto tex = device->CreateTransientTexture(desc, cases[i].firstPass, cases[i].lastPass);
        if (!tex) {
            HE_CORE_ERROR("[P6] 瞬态纹理 {} 创建失败：后端不支持瞬态分配（D3D12 需要 "
                          "ResourceHeapTier>=2）或瞬态堆不足", cases[i].name);
            return false;
        }
        u64 offset = 0, size = 0;
        if (!tex->GetTransientPlacement(offset, size)) {
            HE_CORE_ERROR("[P6] 后端未提供瞬态纹理 {} 的放置信息（GetTransientPlacement）",
                          cases[i].name);
            return false;
        }
        offsets[i] = offset;
        sizes[i]   = size;
        textures.push_back(std::move(tex));
        HE_CORE_INFO("[P6] 创建瞬态纹理 {}：生命周期 [{},{}]", cases[i].name,
                     cases[i].firstPass, cases[i].lastPass);
    }

    // ------------------------------------------------------------------
    // 让三张图各自"首次被使用"：命令列表的首次使用是别名激活点。
    //   D3D12：Aliasing barrier（Before=null）+ 转到 RENDER_TARGET + DiscardResource
    //   Vulkan：UNDEFINED → COLOR_ATTACHMENT 的布局转换（UNDEFINED 语义 = 丢弃原内容）
    // 按 A→B→C 的顺序使用，正好覆盖 C 复用 A 那段内存的时序。
    // 设置 HE_TRACE_BARRIERS=1 可看到每次别名激活的日志。
    // ------------------------------------------------------------------
    auto cmd = device->CreateCommandList();
    if (!cmd) {
        HE_CORE_ERROR("[P6] 命令列表创建失败");
        return false;
    }
    cmd->Begin();
    for (u32 i = 0; i < 3; ++i) {
        cmd->PipelineBarrier(he::rhi::PipelineStage::ColorAttachmentOutput,
                             he::rhi::PipelineStage::ColorAttachmentOutput,
                             he::rhi::ResourceState::Undefined,
                             he::rhi::ResourceState::RenderTarget,
                             textures[i].get());
    }
    cmd->End();
    HE_CORE_INFO("[P6] 录制完成，提交屏障命令…");
    device->Submit(cmd.get());
    HE_CORE_INFO("[P6] 提交返回，等待 GPU 完成（WaitIdle）…");
    device->WaitIdle();   // 等 GPU 真正执行完：别名屏障 / Discard 的合法性由调试层与验证层判定
    HE_CORE_INFO("[P6] WaitIdle 返回");

    // ------------------------------------------------------------------
    // 打印证据
    // ------------------------------------------------------------------
    u64 naiveTotal = 0;   // 逐张独立分配（不复用）的总字节
    u64 aliasedUse = 0;   // 共用内存后实际占用的字节（偏移 + 大小 的最大值）
    char bufNaive[64], bufAliased[64], bufSave[64];
    for (u32 i = 0; i < 3; ++i) {
        naiveTotal += sizes[i];
        aliasedUse  = std::max(aliasedUse, offsets[i] + sizes[i]);
        HE_CORE_INFO("[P6] 瞬态纹理 {}：堆内偏移 = {} 字节，分配大小 = {} 字节",
                     cases[i].name,
                     static_cast<unsigned long long>(offsets[i]),
                     static_cast<unsigned long long>(sizes[i]));
    }

    bool ok = true;

    // 判据 1：A 与 C 生命周期不重叠 ⇒ 偏移相同（共用同一段内存）
    if (offsets[0] != offsets[2]) {
        HE_CORE_ERROR("[P6] 失败：A[1,2] 与 C[3,4] 生命周期不重叠，应共用内存，"
                      "但偏移不同（{} vs {}）",
                      static_cast<unsigned long long>(offsets[0]),
                      static_cast<unsigned long long>(offsets[2]));
        ok = false;
    } else {
        HE_CORE_INFO("[P6] 通过：A[1,2] 与 C[3,4] 生命周期不重叠 ⇒ 共用同一块内存（偏移都是 {} 字节）",
                     static_cast<unsigned long long>(offsets[0]));
    }

    // 判据 2：B 与 A、C 都重叠 ⇒ 必须独立分配（偏移与二者都不同）
    if (offsets[1] == offsets[0] || offsets[1] == offsets[2]) {
        HE_CORE_ERROR("[P6] 失败：B[2,3] 与 A、C 的生命周期都重叠，必须独立分配，"
                      "但偏移与它们相同（B={}，A={}，C={}）",
                      static_cast<unsigned long long>(offsets[1]),
                      static_cast<unsigned long long>(offsets[0]),
                      static_cast<unsigned long long>(offsets[2]));
        ok = false;
    } else {
        HE_CORE_INFO("[P6] 通过：B[2,3] 与 A、C 都重叠 ⇒ 单独分配（偏移 {} 字节，与 A/C 的 {} 字节不同）",
                     static_cast<unsigned long long>(offsets[1]),
                     static_cast<unsigned long long>(offsets[0]));
    }

    // 判据 3：三张图都拿到了非零大小（同规格应当同大小）
    if (sizes[0] == 0 || sizes[0] != sizes[1] || sizes[0] != sizes[2]) {
        HE_CORE_ERROR("[P6] 失败：三张同规格瞬态纹理的分配大小不一致（{} / {} / {}）",
                      static_cast<unsigned long long>(sizes[0]),
                      static_cast<unsigned long long>(sizes[1]),
                      static_cast<unsigned long long>(sizes[2]));
        ok = false;
    }

    // 判据 4：量化节省
    if (ok) {
        const u64 saved = naiveTotal > aliasedUse ? naiveTotal - aliasedUse : 0;
        HE_CORE_INFO("[P6] 瞬态堆占用：逐张独立分配 = {}，内存别名后 = {}，节省 = {}（{:.1f}%）",
                     BytesToText(naiveTotal, bufNaive, sizeof(bufNaive)),
                     BytesToText(aliasedUse, bufAliased, sizeof(bufAliased)),
                     BytesToText(saved, bufSave, sizeof(bufSave)),
                     naiveTotal > 0 ? 100.0 * double(saved) / double(naiveTotal) : 0.0);
        // 期望值：3 张同大小纹理，A/C 复用 ⇒ 只需 2 张的空间
        const u64 expectAliased = sizes[0] * 2;
        if (aliasedUse > expectAliased) {
            HE_CORE_WARN("[P6] 注意：别名后占用 {} 字节，高于理论值 {} 字节"
                         "（对齐/堆首地址不为 0 时会略高）",
                         static_cast<unsigned long long>(aliasedUse),
                         static_cast<unsigned long long>(expectAliased));
        }
    }

    HE_CORE_INFO("[P6] ===== 瞬态纹理内存别名自检{} =====", ok ? "通过" : "失败");
    return ok;
}

} // namespace he::samples
