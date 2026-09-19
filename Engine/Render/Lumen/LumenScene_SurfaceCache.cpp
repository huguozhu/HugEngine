// ============================================================
// LumenScene_SurfaceCache.cpp — 步骤 14：页表 + 页状态机（GPU 镜像与一致性校验）
//
// 【分成单独一个 .cpp】LumenScene.cpp 管"输出纹理 + 占位 pass"，这里管 L2 的页表；
// 两者生命周期不同（页表跟场景走，输出纹理跟视口走），放一起会让改动互相牵动。
// ============================================================

#include "Lumen/LumenScene.h"

#include "Core/Log.h"
#include "Lumen/SurfaceCache.slang"     // 共享布局（与 C++ 镜像同源）
#include "SurfaceCache_Capture.comp.spv.h"
#include "SurfaceCache_Feedback.comp.spv.h"
#include "SurfaceCache_PageCheck.comp.spv.h"

#include <algorithm>
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

// ============================================================
// 步骤 15：Card 捕获（atlas = 8×8 页 × 64×64 texel = 512×512，共 3 张）
// ============================================================
void LumenScene::CreateCaptureGPUObjects() {
    if (m_CapturePSO) return;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // 近层场
        {6, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // 远层场
        {1, rhi::DescriptorType::StorageImage,         1, rhi::kStageMaskCompute},   // atlas albedo
        {7, rhi::DescriptorType::StorageImage,         1, rhi::kStageMaskCompute},   // atlas normal
        {8, rhi::DescriptorType::StorageImage,         1, rhi::kStageMaskCompute},   // atlas emissive
        {2, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // GBuffer albedo
        {3, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // GBuffer normal
        {4, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // GBuffer depth
        {9, rhi::DescriptorType::StorageBuffer,        1, rhi::kStageMaskCompute},   // 统计
        {10, rhi::DescriptorType::StorageBuffer,       1, rhi::kStageMaskCompute},   // 每帧常量
    };
    m_CaptureLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_CaptureSet    = m_Device->AllocateDescriptorSet(m_CaptureLayout);

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskCompute;
    pcr.offset    = 0;
    // 【为什么只留 96 B】Vulkan 的 maxPushConstantsSize 通常是 **128 B**；原先放 15 个 float4（240 B）
    // 被截断：实测 origin 收到 NaN、voxel 收到 0 ⇒ SampleLayer 的 inside 恒 false ⇒ march 一个都不命中。
    // 每帧常量（VP 矩阵/屏幕/两层原点与分辨率）改走 StructuredBuffer（binding 10）。
    pcr.size      = 6u * 16u;    // pageOriginRes / planeOrigin / planeStepU / planeStepV / axis / marchParams

    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_SurfaceCache_Capture_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc pso;
    pso.bindPoint            = rhi::PipelineBindPoint::Compute;
    pso.computeShader        = &cs;
    pso.descriptorSetLayouts = {m_CaptureLayout};
    pso.pushConstantRanges   = {pcr};
    pso.debugName            = "Lumen_SurfaceCache_Capture";
    m_CapturePSO = m_Device->CreatePipelineState(pso);
    if (!m_CapturePSO) HE_CORE_ERROR("LumenScene: Card 捕获 PSO 创建失败");

    const auto makeAtlas = [&]() {
        rhi::TextureDesc td;
        td.width  = kAtlasSize; td.height = kAtlasSize; td.depth = 1;
        td.format = rhi::Format::RGBA16_FLOAT;
        td.usage  = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource |
                    rhi::TextureUsage::TransferSrc;
        return m_Device->CreateTexture(td);
    };
    m_AtlasAlbedo   = makeAtlas();
    m_AtlasNormal   = makeAtlas();
    m_AtlasEmissive = makeAtlas();

    rhi::BufferDesc sb;
    sb.size      = sizeof(u32) * 8u;   // 0=命中写入 1=未命中 2=march 命中 3=未用 4..7=push constant 回报(诊断)
    sb.usage     = rhi::BufferUsage::Storage;
    sb.cpuAccess = true;
    m_CaptureStats = m_Device->CreateBuffer(sb);
    m_CaptureStatsMapped = m_CaptureStats->Map();

    rhi::BufferDesc fb;                       // 每帧常量（与 shader 的 CaptureFrame 同布局：10 × 16B）
    fb.size      = 10u * 16u;
    fb.usage     = rhi::BufferUsage::Storage;
    fb.cpuAccess = true;
    m_CaptureFrameBuf = m_Device->CreateBuffer(fb);
    m_CaptureFrameMapped = m_CaptureFrameBuf->Map();
    m_Device->UpdateDescriptorSet(m_CaptureSet, 10, rhi::DescriptorType::StorageBuffer,
                                  m_CaptureFrameBuf.get());
}

void LumenScene::RunCardCapture(rhi::IRHICommandList* cmd, rhi::IRHITexture* gbAlbedo,
                                rhi::IRHITexture* gbNormal, rhi::IRHITexture* gbDepth,
                                const float4x4& viewProj) {
    if (!m_Device || !cmd || !m_SDF.GetGlobalField(0)) return;
    BuildPageTable();
    if (m_PageTable.Size() == 0 || !gbAlbedo || !gbNormal || !gbDepth) return;
    if (!m_CaptureBound) {
        CreateCaptureGPUObjects();
        if (!m_CapturePSO || !m_AtlasAlbedo) return;
        m_Device->UpdateDescriptorSet(m_CaptureSet, 0, rhi::DescriptorType::CombinedImageSampler,
                                      m_SDF.GetGlobalField(0), m_SDF.GetLinearSampler());
        m_Device->UpdateDescriptorSet(m_CaptureSet, 6, rhi::DescriptorType::CombinedImageSampler,
                                      m_SDF.GetGlobalField(1), m_SDF.GetLinearSampler());
        m_Device->UpdateDescriptorSetWithImageView(m_CaptureSet, 1,
            rhi::DescriptorType::StorageImage, m_AtlasAlbedo->GetNativeHandle());
        m_Device->UpdateDescriptorSetWithImageView(m_CaptureSet, 7,
            rhi::DescriptorType::StorageImage, m_AtlasNormal->GetNativeHandle());
        m_Device->UpdateDescriptorSetWithImageView(m_CaptureSet, 8,
            rhi::DescriptorType::StorageImage, m_AtlasEmissive->GetNativeHandle());
        m_Device->UpdateDescriptorSet(m_CaptureSet, 9, rhi::DescriptorType::StorageBuffer,
                                      m_CaptureStats.get());
        m_CaptureBound = true;
    }
    // GBuffer 可能随 resize 换纹理 ⇒ 这三张每帧重绑
    m_Device->UpdateDescriptorSet(m_CaptureSet, 2, rhi::DescriptorType::CombinedImageSampler,
                                  gbAlbedo, m_SDF.GetLinearSampler());
    m_Device->UpdateDescriptorSet(m_CaptureSet, 3, rhi::DescriptorType::CombinedImageSampler,
                                  gbNormal, m_SDF.GetLinearSampler());
    m_Device->UpdateDescriptorSet(m_CaptureSet, 4, rhi::DescriptorType::CombinedImageSampler,
                                  gbDepth, m_SDF.GetLinearSampler());

    const auto& cards = m_SDF.GetCards();
    u32 captured = 0;
    for (u32 page = 0; page < m_PageTable.Size() && captured < kMaxCapturesPerFrame; ++page) {
        if (m_PageTable.Get(page).state != kSCPageState_Capturing) continue;   // 只处理 Capturing 的页
        const u32 cardIndex = m_PageTable.Get(page).cardIndex;
        if (cardIndex >= cards.size()) continue;
        const auto& card = cards[cardIndex];
        // 卡是自适应分辨率（常见 512²），页固定 64² ⇒ 按整数 stride 降采样（每个页 texel 采一个卡 texel）
        const u32 stride = std::max(1u, card.res / kAtlasPageRes);

        const u8 a = card.axis;
        const u8 b = (u8)((a + 1u) % 3u);
        const u8 c = (u8)((a + 2u) % 3u);
        const float loB = (&card.aabbLo.x)[b];
        const float loC = (&card.aabbLo.x)[c];
        const float loA = (&card.aabbLo.x)[a];
        const float hiA = loA + card.side;
        const float start   = (card.dir > 0) ? (loA - 1.0f) : (hiA + 1.0f);   // 从 AABB 外侧起步
        const float dirSign = (card.dir > 0) ? 1.0f : -1.0f;

        float3 planeOrigin(0.0f), stepU(0.0f), stepV(0.0f), axis(0.0f);
        (&planeOrigin.x)[b] = loB;
        (&planeOrigin.x)[c] = loC;
        (&planeOrigin.x)[a] = start;
        (&stepU.x)[b] = card.texelWorld * (float)stride;
        (&stepV.x)[c] = card.texelWorld * (float)stride;
        (&axis.x)[a]  = dirSign;

        // 【eps 必须大于"场的误差"】全局场是 28.4 体素的远层 + 14.2 体素的近层，局部高估可达半个远层体素
        // （≈14 单位）。取 eps = 1 个近层体素（14.2），步长取 eps/2 ⇒ 任何一次穿越都至少有一个采样落进 eps 内。
        // （此前步长 21.6 > eps 10.9 且 eps 只按卡 texel 算，导致 20480 个 texel 一个都没命中。）
        // 【诊断】第一张卡：把 march 起点/中点/终点的世界坐标与 **CPU 真值**并排打出来。
        // 若真值沿 march 明显变小（接近 0）⇒ 几何就在这条轴上，问题在 GPU 侧采样；
        // 若真值一直很大 ⇒ 卡平面基（axis/planeOrigin）本身就错了。
        if (m_CapturePages == 0) {
            const float3 p0 = planeOrigin + axis * 0.0f;
            const float3 p1 = planeOrigin + axis * (card.side * 0.5f);
            const float3 p2 = planeOrigin + axis * (card.side + 1.0f);
            HE_CORE_INFO("Card 捕获诊断（卡 #{}）: mesh #{} axis={} dir={} 边长 {:.1f} texelW {:.2f} stride {} | "
                         "平面原点 ({:.1f},{:.1f},{:.1f})",
                         cardIndex, card.mesh, (int)card.axis, (int)card.dir, (double)card.side,
                         (double)card.texelWorld, stride,
                         (double)planeOrigin.x, (double)planeOrigin.y, (double)planeOrigin.z);
            HE_CORE_INFO("   该轴 CPU 真值: t=0 → {:.2f}，t=side/2 → {:.2f}，t=side → {:.2f}；卡填充率 {:.1f}%",
                         (double)m_SDF.QueryTrueDistance(p0), (double)m_SDF.QueryTrueDistance(p1),
                         (double)m_SDF.QueryTrueDistance(p2),
                         100.0 * (double)card.filled / (double)(card.res * card.res));
        }        const float eps   = std::max(1.0f * m_SDF.GetGlobalVoxelSize(0), 0.5f * card.texelWorld * (float)stride);
        const float step  = std::max(0.5f * eps, 1.0f);
        const float steps = std::ceil((card.side + 2.0f) / step);

        // 【字段顺序必须与 shader 的 CapturePC 完全一致】（96 B，低于 128 B 的 push constant 上限）
        struct CapturePC {
            float4 pageOriginRes, planeOrigin, planeStepU, planeStepV, axis, marchParams;
        } pc{};
        // 每帧常量写进 StructuredBuffer（VP/屏幕/两层原点与分辨率）
        if (m_CaptureFrameMapped) {
            struct CaptureFrame {
                float4 vp0, vp1, vp2, vp3;
                uint4  screen;
                float4 origin0, origin1;
                uint4  grid0, grid1;
            } cf{};
            cf.vp0 = float4(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
            cf.vp1 = float4(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
            cf.vp2 = float4(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);
            cf.vp3 = float4(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);
            cf.screen  = uint4(m_Width, m_Height, 0u, 0u);
            cf.origin0 = float4(m_SDF.GetGlobalOrigin(0), m_SDF.GetGlobalVoxelSize(0));
            cf.origin1 = float4(m_SDF.GetGlobalOrigin(1), m_SDF.GetGlobalVoxelSize(1));
            const u32 gr = m_SDF.GetGlobalResolution();
            cf.grid0 = uint4(gr, gr, gr, 0u);
            cf.grid1 = cf.grid0;
            std::memcpy(m_CaptureFrameMapped, &cf, sizeof(cf));
        }
        pc.pageOriginRes = float4((float)((page % kAtlasGridDim) * kAtlasPageRes),
                                  (float)((page / kAtlasGridDim) * kAtlasPageRes),
                                  (float)kAtlasPageRes, card.texelWorld);
        pc.planeOrigin = float4(planeOrigin, 0.0f);
        pc.planeStepU  = float4(stepU, 0.0f);
        pc.planeStepV  = float4(stepV, 0.0f);
        pc.axis        = float4(axis, 0.0f);
        pc.marchParams = float4(steps, eps, step, 0.0f);

        cmd->SetPipeline(m_CapturePSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_CaptureSet);
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->Dispatch(1, 1, 1);

        m_PageTable.EndCapture(page, true);      // Capturing → Captured
        if (m_PageTableBuf) {
            if (void* p = m_PageTableBuf->Map()) {
                std::memcpy(p, m_PageTable.Entries().data(),
                            m_PageTable.Size() * sizeof(SurfaceCachePageEntry));
                m_PageTableBuf->Unmap();
            }
        }
        ++captured;
        ++m_CapturePages;
    }

    if (captured > 0) m_CaptureStatsPending = true;   // 统计要等 GPU 写完（下一帧再读）
    if (m_CaptureStatsPending && m_CaptureStatsMapped && !m_CaptureStatsLogged) {
        u32 st[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        std::memcpy(st, m_CaptureStatsMapped, sizeof(st));
        m_CardCaptureHits   = st[0];
        m_CardCaptureMisses = st[1];
        m_CardCaptureMarchHits = st[2];
        if (m_CardCaptureHits + m_CardCaptureMisses + m_CardCaptureMarchHits == 0) return;   // 还没读到有效读数
        // 【永久护栏】确认"每帧常量"真的到达 shader：voxel 对不上就说明 push/常量缓冲的布局又错位了
        float fPC[4]; std::memcpy(fPC, &st[4], sizeof(fPC));
        if (std::fabs((double)fPC[2] - (double)m_SDF.GetGlobalVoxelSize(0)) > 1e-3 ||
            std::fabs((double)fPC[1] - (double)m_SDF.GetGlobalOrigin(0).x) > 1e-2) {
            HE_CORE_ERROR("Card 捕获常量错位: shader 收到 origin.x={:.2f}/voxel={:.2f}，CPU 侧 {:.2f}/{:.2f}",
                          (double)fPC[1], (double)fPC[2],
                          (double)m_SDF.GetGlobalOrigin(0).x, (double)m_SDF.GetGlobalVoxelSize(0));
        }
        m_CaptureStatsLogged = true;
        HE_CORE_INFO("LumenScene Card 捕获（步骤 15）: 累计捕获 {} 页；命中 texel {} / 未命中 {}（{:.1f}% 命中）；SDF march 命中 {}（诊断）；atlas {}² × 3",
                     m_CapturePages, m_CardCaptureHits, m_CardCaptureMisses,
                     (m_CardCaptureHits + m_CardCaptureMisses)
                         ? 100.0 * (double)m_CardCaptureHits / (double)(m_CardCaptureHits + m_CardCaptureMisses) : 0.0,
                     m_CardCaptureMarchHits, kAtlasSize);
    }
}
// ============================================================
// 步骤 16：Feedback（缺失页检测）—— 16×16 分块 → 请求列表 → CPU 排序 → 写回页表
// ============================================================
void LumenScene::CreateFeedbackGPUObjects() {
    if (m_FeedbackPSO) return;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},   // GBuffer worldpos
        {1, rhi::DescriptorType::StorageBuffer,        1, rhi::kStageMaskCompute},   // 卡片清单
        {2, rhi::DescriptorType::StorageBuffer,        1, rhi::kStageMaskCompute},   // 请求计数
        {3, rhi::DescriptorType::StorageBuffer,        1, rhi::kStageMaskCompute},   // 请求表
    };
    m_FeedbackLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_FeedbackSet    = m_Device->AllocateDescriptorSet(m_FeedbackLayout);

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskCompute;
    pcr.offset    = 0;
    pcr.size      = 3u * 16u;   // uint4 dims + uint4 pages + float4 camPosDist

    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_SurfaceCache_Feedback_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc pso;
    pso.bindPoint            = rhi::PipelineBindPoint::Compute;
    pso.computeShader        = &cs;
    pso.descriptorSetLayouts = {m_FeedbackLayout};
    pso.pushConstantRanges   = {pcr};
    pso.debugName            = "Lumen_SurfaceCache_Feedback";
    m_FeedbackPSO = m_Device->CreatePipelineState(pso);
    if (!m_FeedbackPSO) HE_CORE_ERROR("LumenScene: Feedback PSO 创建失败");
}

void LumenScene::RunFeedback(rhi::IRHICommandList* cmd, rhi::IRHITexture* gbWorldPos, const float3& camPos) {
    if (!m_Device || !cmd || !gbWorldPos) return;
    BuildPageTable();
    if (m_PageTable.Size() == 0) return;

    CreateFeedbackGPUObjects();
    if (!m_FeedbackPSO) return;

    if (!m_FeedbackBound) {
        // 卡片清单（与 shader 的 FeedbackCard 同布局：aabbLo(w=边长) + axisDirPage(重要性)）
        const auto& cards = m_SDF.GetCards();
        struct FeedbackCard { float4 aabbLo; float4 axisDirPage; };
        std::vector<FeedbackCard> gpuCards(cards.size());
        for (size_t i = 0; i < cards.size(); ++i) {
            const auto& c = cards[i];
            // 重要性：卡覆盖的表面积越大越重要（用填充率 × 边长近似），下限 0.01 防止全 0
            const float fill = (c.res > 0) ? (float)c.filled / (float)(c.res * c.res) : 0.0f;
            const float importance = std::max(0.01f, fill * std::sqrt(std::max(1.0f, c.side)));
            gpuCards[i].aabbLo      = float4(c.aabbLo, c.side);
            gpuCards[i].axisDirPage = float4((float)c.axis, (float)c.dir, (float)i, importance);
        }
        rhi::BufferDesc cb;
        cb.size        = gpuCards.size() * sizeof(FeedbackCard);
        cb.usage       = rhi::BufferUsage::Storage;
        cb.initialData = gpuCards.data();
        m_CardBuf = m_Device->CreateBuffer(cb);

        rhi::BufferDesc rcb;
        rcb.size = sizeof(u32); rcb.usage = rhi::BufferUsage::Storage; rcb.cpuAccess = true;
        m_ReqCountBuf = m_Device->CreateBuffer(rcb);
        m_ReqCountMapped = m_ReqCountBuf->Map();

        rhi::BufferDesc rb;
        rb.size = (usize)kMaxFeedbackTiles * sizeof(u32) * 2u;
        rb.usage = rhi::BufferUsage::Storage; rb.cpuAccess = true;
        m_ReqBuf = m_Device->CreateBuffer(rb);
        m_ReqMapped = m_ReqBuf->Map();

        m_Device->UpdateDescriptorSet(m_FeedbackSet, 0, rhi::DescriptorType::CombinedImageSampler,
                                      gbWorldPos, m_SDF.GetLinearSampler());
        m_Device->UpdateDescriptorSet(m_FeedbackSet, 1, rhi::DescriptorType::StorageBuffer, m_CardBuf.get());
        m_Device->UpdateDescriptorSet(m_FeedbackSet, 2, rhi::DescriptorType::StorageBuffer, m_ReqCountBuf.get());
        m_Device->UpdateDescriptorSet(m_FeedbackSet, 3, rhi::DescriptorType::StorageBuffer, m_ReqBuf.get());
        m_FeedbackBound = true;
        return;   // 本帧只建资源；下一帧开始派发（"新建资源当帧使用"的教训见 §附二十）
    }

    // ── ① 先处理**上一帧**的结果（每块一个槽位：完整且确定）──
    // 【为什么必须在清零之前读】计数由 GPU 原子加写入；若本帧先清零再读，读到的一定是 0
    // （第一版就是这样：请求恒 0 条）。按"引擎在录 N+1 帧时 N 帧已执行完"的节拍，先读后清是对的。
    if (m_FeedbackFrame >= 1) {
        const u32 tilesX = (m_Width + 15u) / 16u;
        const u32 tilesY = (m_Height + 15u) / 16u;
        const u32 tileCount = std::min(kMaxFeedbackTiles, tilesX * tilesY);
        std::vector<uint2> req;
        req.reserve(tileCount);
        if (m_ReqMapped) {
            const uint2* slots = static_cast<const uint2*>(m_ReqMapped);
            for (u32 i = 0; i < tileCount; ++i)
                if (slots[i].y > 0u) req.push_back(slots[i]);   // 权重 0 = 该块没几何
        }
        const u32 count = (u32)req.size();
        m_FeedbackRequests = count;
        std::sort(req.begin(), req.end(), [](const uint2& a, const uint2& b) {
            return (a.y != b.y) ? (a.y > b.y) : (a.x < b.x);   // 权重降序；权重相同按页号定序 ⇒ **确定性**
        });

        const u32 topN = std::min(count, kMaxCapturesPerFrame * 4u);
        std::vector<u32> top;
        top.reserve(topN);
        for (u32 i = 0; i < topN; ++i) {
            const u32 page = req[i].x;
            if (page >= m_PageTable.Size()) continue;
            top.push_back(page);
            // 需要但还没有的页 ⇒ 置为 Requested（未完成请求留在 Requested，不回退不丢弃 —— 步骤 17 的口径）
            if (m_PageTable.Get(page).state == kSCPageState_Invalid)
                m_PageTable.Request(page, page, m_FeedbackFrame);
        }
        // 本帧把前 kMaxCapturesPerFrame 个"已请求"页推进到 Capturing（分配 + 开始捕获），
        // 步骤 15 的捕获 pass 下一帧就会把它们写进 atlas —— 这就是 L2 的"请求 → 捕获 → 可用"闭环。
        u32 promoted = 0;
        for (u32 page : top) {
            if (promoted >= kMaxCapturesPerFrame) break;
            if (m_PageTable.Get(page).state != kSCPageState_Requested) continue;
            if (!m_PageTable.Allocate(page, page)) continue;
            m_PageTable.BeginCapture(page);
            ++promoted;
        }
        if (!m_LastTopPages.empty()) {   // 稳定性指标：与上一帧 top-N 的交集
            u32 inter = 0;
            for (u32 a : top) for (u32 b : m_LastTopPages) if (a == b) { ++inter; break; }
            m_FeedbackTopOverlap = inter;
        }
        m_FeedbackTopCount = (u32)top.size();
        m_LastTopPages = top;

        if (m_PageTableBuf) {   // 状态变了 ⇒ 同步 GPU 镜像
            if (void* p = m_PageTableBuf->Map()) {
                std::memcpy(p, m_PageTable.Entries().data(),
                            m_PageTable.Size() * sizeof(SurfaceCachePageEntry));
                m_PageTableBuf->Unmap();
            }
        }
        if ((m_FeedbackFrame % 20u) == 0u) {
            HE_CORE_INFO("LumenScene Feedback（步骤 16）: 本帧请求 {} 条（16×16 分块，权重 = 覆盖×重要性×距离衰减）；"
                         "top-{} 与上一帧重叠 {} 条；页表 Requested {} / Capturing {}",
                         count, m_FeedbackTopCount, m_FeedbackTopOverlap,
                         m_PageTable.Count(kSCPageState_Requested), m_PageTable.Count(kSCPageState_Capturing));
        }
    }

    // ── ② 派发本帧的 Feedback（每块写自己的槽位，无需清零计数）──
    struct { u32 x, y, z, w; u32 p; u32 pad0, pad1, pad2; float4 cam; } pc{};
    pc.x = m_Width; pc.y = m_Height;
    pc.z = (u32)m_SDF.GetCards().size();
    pc.w = kMaxFeedbackTiles;
    pc.p = m_PageTable.Size();
    pc.cam = float4(camPos, 512.0f);

    const u32 tilesX = (m_Width + 15u) / 16u;
    const u32 tilesY = (m_Height + 15u) / 16u;
    cmd->SetPipeline(m_FeedbackPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_FeedbackSet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch((tilesX + 7u) / 8u, (tilesY + 7u) / 8u, 1);
    ++m_FeedbackFrame;
}

} // namespace he::render
