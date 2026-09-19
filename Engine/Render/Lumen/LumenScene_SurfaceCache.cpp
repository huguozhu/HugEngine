// ============================================================
// LumenScene_SurfaceCache.cpp — 步骤 14：页表 + 页状态机（GPU 镜像与一致性校验）
//
// 【分成单独一个 .cpp】LumenScene.cpp 管"输出纹理 + 占位 pass"，这里管 L2 的页表；
// 两者生命周期不同（页表跟场景走，输出纹理跟视口走），放一起会让改动互相牵动。
// ============================================================

#include "Lumen/LumenScene.h"

#include "Core/Log.h"
#include "Lumen/SurfaceCache.slang"     // 共享布局（与 C++ 镜像同源）
#include "SurfaceCache_PageCheck.comp.spv.h"

#include <cstring>

namespace he::render {

void LumenScene::BuildPageTable() {
    if (m_PageTableBuilt || !m_Device) return;
    // 卡片还没生成（步骤 13 在自检之后才跑）⇒ 等下一帧再建，否则页数会退化成 1
    if (m_SDF.GetCardCoverage().cards == 0) return;
    m_PageTableBuilt = true;

    // 页 = 卡片（步骤 13 的清单）：一张卡一页。这里只演示前 N 页（真实分配策略在步骤 15~19）。
    const u32 cardCount = std::max(1u, m_SDF.GetCardCoverage().cards);
    const u32 pageCount = std::min(cardCount, 64u);
    m_PageTable.Resize(pageCount);

    // 走一遍六态：前面的页走完整生命周期，后面的停在中间态（覆盖率越高，状态越丰富）
    for (u32 i = 0; i < pageCount; ++i) {
        m_PageTable.Request(i, /*card*/ i, /*frame*/ 0);
        m_PageTable.Allocate(i, /*physical*/ i);
        if (i % 7u == 3u) continue;                     // 停在 Allocating
        m_PageTable.BeginCapture(i);
        if (i % 11u == 5u) continue;                    // 停在 Capturing
        m_PageTable.EndCapture(i, /*ok*/ true);
        if (i % 5u == 4u) m_PageTable.MarkDirty(i);     // Captured → Dirty
    }

    // GPU 侧镜像：把条目原样上传（StructuredBuffer，与 shader 同布局）
    rhi::BufferDesc bd;
    bd.size        = m_PageTable.Size() * sizeof(SurfaceCachePageEntry);
    bd.usage       = rhi::BufferUsage::Storage;
    bd.cpuAccess   = true;
    bd.initialData = m_PageTable.Entries().data();
    m_PageTableBuf = m_Device->CreateBuffer(bd);

    HE_CORE_INFO("LumenScene 页表（步骤 14）: 卡片 {} 张 ⇒ 页 {} 个（{} 张卡/页 1:1，演示前 64 页）；"
                 "Invalid {} / Requested {} / Allocating {} / Capturing {} / Captured {} / Dirty {}",
                 cardCount, pageCount, m_PageTable.Size(),
                 m_PageTable.Count(kSCPageState_Invalid), m_PageTable.Count(kSCPageState_Requested),
                 m_PageTable.Count(kSCPageState_Allocating), m_PageTable.Count(kSCPageState_Capturing),
                 m_PageTable.Count(kSCPageState_Captured), m_PageTable.Count(kSCPageState_Dirty));
}

void LumenScene::CreatePageCheckGPUObjects() {
    if (m_PageCheckPSO) return;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
        {1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
    };
    m_PageCheckLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_PageCheckSet    = m_Device->AllocateDescriptorSet(m_PageCheckLayout);

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskCompute;
    pcr.offset    = 0;
    pcr.size      = sizeof(u32) * 4u;   // uint4 dims

    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_SurfaceCache_PageCheck_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc pso;
    pso.bindPoint            = rhi::PipelineBindPoint::Compute;
    pso.computeShader        = &cs;
    pso.descriptorSetLayouts = {m_PageCheckLayout};
    pso.pushConstantRanges   = {pcr};
    pso.debugName            = "Lumen_SurfaceCache_PageCheck";
    m_PageCheckPSO = m_Device->CreatePipelineState(pso);
    if (!m_PageCheckPSO) HE_CORE_ERROR("LumenScene: 页表校验 PSO 创建失败");
}

void LumenScene::StepSurfaceCache(rhi::IRHICommandList* cmd) {
    if (!m_Device || !cmd) return;
    BuildPageTable();
    if (m_PageCheckDone || !m_PageTableBuf) return;

    if (!m_PageCheckBound) {
        rhi::BufferDesc ob;
        ob.size      = sizeof(u32);
        ob.usage     = rhi::BufferUsage::Storage;
        ob.cpuAccess = true;
        m_PageCheckOut = m_Device->CreateBuffer(ob);
        // 持久映射：探测器那套"创建即 Map、之后只读写指针"的做法实测可靠；临时 Map 读不到 GPU 的写入
        m_PageCheckOutMapped = m_PageCheckOut->Map();

        m_Device->UpdateDescriptorSet(m_PageCheckSet, 0, rhi::DescriptorType::StorageBuffer,
                                      m_PageTableBuf.get());
        m_Device->UpdateDescriptorSet(m_PageCheckSet, 1, rhi::DescriptorType::StorageBuffer,
                                      m_PageCheckOut.get());
        m_PageCheckBound = true;
    }
    if (!m_PageCheckPSO) { m_PageCheckDone = true; return; }

    // 发射一次（读回要等几帧，故每帧都发也无妨；只在未完成时进入本函数）
    // 【为什么要等几帧】缓冲在"页表建好那一帧"才创建，先等 3 帧让资源落地再 dispatch，
    // 再等 3 帧读回（与探针缓冲同一套飞行帧节拍）。
    // 【另一处踩坑】描述符集**必须在 Initialize 时分配**：帧中途 AllocateDescriptorSet 拿到的集合
    // 实测写不进（回读一直是创建时的 CPU 标记）；提前分配后同一段代码立刻正确。
    if (++m_PageCheckFrame < 4) return;

    struct { u32 x, y, z, w; } pc{ m_PageTable.Size(), 0u, 0u, 0u };
    cmd->SetPipeline(m_PageCheckPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_PageCheckSet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch(1, 1, 1);

    // 再等 3 帧（飞行帧）读回
    if (m_PageCheckFrame < 7) return;

    const u32 cpu = m_PageTable.Checksum();
    u32 gpu = 0;
    if (m_PageCheckOutMapped) std::memcpy(&gpu, m_PageCheckOutMapped, sizeof(gpu));
    m_PageCheckDone   = true;
    m_PageCheckPassed = (gpu == cpu);
    if (m_PageCheckPassed) {
        HE_CORE_INFO("LumenScene 页表镜像校验（步骤 14）: GPU 与 C++ 侧校验和一致 = {:#010x}（{} 页，32 位 FNV-1a）",
                     cpu, m_PageTable.Size());
    } else {
        HE_CORE_ERROR("LumenScene 页表镜像校验失败: CPU={:#010x} GPU={:#010x} —— 两侧布局/字段不一致",
                      cpu, gpu);
    }
}

} // namespace he::render
