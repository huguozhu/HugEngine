// ============================================================
// Nanite/NaniteRaster.cpp — 绘制端：消费「计数 → 间接绘制」链（§14.8 任务 3）
//   + 任务 16：默认消费**可见簇列表**写出的间接命令（假簇链保留为自证/退化路径）
//
// 【本文件由 §14.8 任务 1 建立骨架，任务 3 填充 "绘制端"】
//   绘制 = `IRHICommandList::DrawIndexedIndirectCount`（Vulkan: vkCmdDrawIndexedIndirectCount），
//   实际条数由 `NaniteCull` 的计数缓冲决定 —— CPU 不读回、不参与条数决定。
//   目标 = 模块自建的 1×1 R8 小目标；片元把"被光栅化的**绘制条数**"原子加一
//   （`SV_PrimitiveID == 0` ⇒ 每个绘制恰好一次，与簇号/实例无关）。
//
// 【§14.8 任务 16 的三处改动（逐条）】
//   ① 占位索引缓冲必须覆盖**整个索引位置空间**：命令里的 `firstIndex/indexCount` 是簇的真实值，
//      光栅器会去绑定的索引缓冲里取这些索引 ⇒ 缓冲不够大就是越界读（本设备未启用
//      `robustBufferAccess`）。缓冲内容 = `0,1,2` 周期模式，使任意簇区间都能凑出非退化三角形
//      （退化三角形不产生片元 ⇒ 计数会失真）。容量由 `SetPlaceholderIndexCapacity` 按资产给。
//   ② 计数语义改为"每个**绘制**恰好 +1"：`indexCount` 现在是簇的真实三角形索引数（最多 192），
//      一条命令会光栅化出多个片元 ⇒ 用绘制内的图元序号 `SV_PrimitiveID == 0` 判定"本条命令的
//      第一个三角形"，因此计数恒等于"产生了片元的绘制条数"（见 `Nanite_Raster.frag.slang`）。
//   ③ 绘制计数缓冲的清零仍由 `NaniteCull` 在命令缓冲内完成（4B 拷贝，绝不用主机写）；本文件只
//      在 dump 帧**读回**它。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态。
// ============================================================

#include "Nanite/NaniteRaster.h"

#include "Nanite/NaniteTypes.h"     // kNaniteRasterTargetSize / kNaniteFakeClusterIndexCount
#include "Core/Log.h"

#include "Nanite_Raster.vert.spv.h"   // k_Nanite_Raster_vert_spv
#include "Nanite_Raster.frag.spv.h"   // k_Nanite_Raster_frag_spv
#include "Nanite_TestWrite.comp.spv.h"   // k_Nanite_TestWrite_comp_spv（§14.8 任务 4 的 UAV 自证通道）
#include "Nanite_MeshTest.mesh.spv.h"    // k_Nanite_MeshTest_mesh_spv（§14.8 任务 6 的 mesh 通道）
#include "Nanite_MeshTest.frag.spv.h"    // k_Nanite_MeshTest_frag_spv
// 【§14.8 任务 18】软光栅三趟：深度键 / 写 GBuffer / 深度解析
#include "Nanite_SoftRasterDepth.comp.spv.h"   // k_Nanite_SoftRasterDepth_comp_spv
#include "Nanite_SoftRaster.comp.spv.h"        // k_Nanite_SoftRaster_comp_spv
#include "Nanite_DepthResolve.vert.spv.h"      // k_Nanite_DepthResolve_vert_spv
#include "Nanite_DepthResolve.frag.spv.h"      // k_Nanite_DepthResolve_frag_spv
#include "Nanite_GBufferClear.comp.spv.h"     // k_Nanite_GBufferClear_comp_spv（compute 清屏 8 张颜色目标）
// 【§14.8 任务 22】硬光栅（mesh shader 分流）
#include "Nanite_HardRaster.mesh.spv.h"       // k_Nanite_HardRaster_mesh_spv
#include "Nanite_HardRaster.frag.spv.h"       // k_Nanite_HardRaster_frag_spv
#include "Nanite_DebugView.comp.spv.h"        // k_Nanite_DebugView_comp_spv（§14.8 任务 26 的可视化）

#include <cstring>   // std::memcpy（占位顶点/索引缓冲的初值）

namespace he::render {

bool NaniteRaster::Initialize(rhi::IRHIDevice* device, u32 width, u32 height,
                              rhi::IRHIBuffer* rasterCountBuffer) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    if (!m_Device) return false;
    if (!rasterCountBuffer) {
        HE_CORE_ERROR("NaniteRaster: 光栅化簇计数缓冲为空（NaniteCull 未就绪？）");
        return false;
    }
    // 【任务 16】只记录引用（不持有）：dump 帧读回"绘制条数"要用它
    m_RasterCountRef = rasterCountBuffer;

    // ── 0. 设备能力：mesh shader 是否可用（§14.8 任务 6）──
    // 【为什么先判、且不做替代方案】任务 6 的验收是"mesh PSO 真的建出来并画出一帧"。
    //   若设备没有 `VK_EXT_mesh_shader`，帧图侧连 `Nanite_MeshTest` pass 都不注册
    //   （`IsMeshTestSupported()`），模块其余部分照旧 —— 与任务 3 对
    //   `DrawIndexedIndirectCount` 的处理口径一致：能力不足就明说，不自造替代路径。
    m_MeshShaderSupported = m_Device->GetCaps().supportsMeshShaders;
    if (!m_MeshShaderSupported) {
        HE_CORE_WARN("NaniteRaster: 设备不支持 VK_EXT_mesh_shader ⇒ 任务 6 的 Nanite_MeshTest "
                     "不会注册（其余链路不受影响）");
    }

    // ── 0b. 【任务 18】GBuffer 深度格式能否做**存储图像**（运行时能力查询）──
    // 【为什么要查】软光栅是 compute：写颜色（8 张颜色目标在任务 4 已加 `UnorderedAccess`）
    //   没问题，但"compute 写深度"只在 `D32_SFLOAT` 带 `STORAGE_IMAGE_BIT` 时成立 ——
    //   实测本机 NVIDIA RTX 4060 支持、同机 AMD 核显不支持（§14.14 的 A1 裁决）。
    //   本实现的深度走 `SV_Depth` + 既有深度附件（跨厂商），这个查询的结果只进读数：
    //   让"为什么没走 compute 写深度"成为一行**被报告**的事实，而不是静默的取舍。
    m_DepthStorageImageSupported = m_Device->SupportsStorageImage(rhi::Format::D32_FLOAT);
    if (!m_DepthStorageImageSupported) {
        HE_CORE_WARN("NaniteRaster: 本设备不支持 D32_SFLOAT 作为存储图像 ⇒ 软光栅不写深度 UAV，"
                     "深度改走全屏片元的 SV_Depth（跨厂商路径；读数里 depth_written=1 / "
                     "depth_storage_image_supported=0）");
    } else {
        HE_CORE_INFO("NaniteRaster: D32_SFLOAT 支持存储图像（compute 写深度可用），但本实现按 "
                     "§14.5 的 A1 裁决仍走 SV_Depth + 既有深度附件（GBuffer 深度纹理没有 "
                     "UnorderedAccess usage，且跨厂商一致性更好）");
    }

    // ── 0c. 【任务 22】硬光栅的**静态可判据**：mesh shader 可用 + 上限容得下编译期声明 ──
    // 【为什么在建 PSO 之前就把上限核对掉】mesh 的输出数组尺寸、`[numthreads]` 的线程数都是
    //   **编译期常量**（192 顶点 / 64 图元 / 128 线程），如果设备的物理上限比它们小，
    //   `vkCreateGraphicsPipelines` 会直接失败（而不是降级）。与其让它失败在 PSO 创建里，
    //   不如在这里算出一个明确的"能不能走这条通道"，为假就不建 PSO、不录绘制
    //   （与任务 6 对 `VK_EXT_mesh_shader` 的处理口径一致：能力不足就明说，不造替代方案）。
    {
        const rhi::DeviceCaps caps = m_Device->GetCaps();
        const bool enoughThreads = caps.maxMeshWorkGroupInvocations >= kNaniteHardRasterThreads;
        const bool enoughVerts   = caps.maxMeshOutputVertices >= kNaniteHardRasterMaxVertices;
        const bool enoughPrims   = caps.maxMeshOutputPrimitives >= kNaniteHardRasterMaxPrimitives;
        m_HardRasterCapable = m_MeshShaderSupported && enoughThreads && enoughVerts && enoughPrims;
        if (m_MeshShaderSupported && !m_HardRasterCapable) {
            HE_CORE_WARN("NaniteRaster: 设备支持 mesh shader，但上限不足以承载任务 22 的硬光栅声明"
                         "（需要 invocations>={} / vertices>={} / primitives>={}；"
                         "实测 {}/{}/{}）⇒ 硬光栅通道不会创建",
                         kNaniteHardRasterThreads, kNaniteHardRasterMaxVertices,
                         kNaniteHardRasterMaxPrimitives, caps.maxMeshWorkGroupInvocations,
                         caps.maxMeshOutputVertices, caps.maxMeshOutputPrimitives);
        } else if (m_HardRasterCapable) {
            HE_CORE_INFO("NaniteRaster: 任务 22 硬光栅上限核对通过（mesh invocations={} vertices={} "
                         "primitives={} ≥ 本通道声明 {}/{}/{}）",
                         caps.maxMeshWorkGroupInvocations, caps.maxMeshOutputVertices,
                         caps.maxMeshOutputPrimitives, kNaniteHardRasterThreads,
                         kNaniteHardRasterMaxVertices, kNaniteHardRasterMaxPrimitives);
        }
    }

    // ── 1. 模块自建的 1×1 R8 目标 ──
    // 【为什么是 1×1】见 NaniteTypes.h 的 kNaniteRasterTargetSize 说明：这一条链只数"画了几次"。
    // 它绝不能是 GBuffer 附件，否则就违反了"任务 3 不改可见画面"的验收。
    {
        rhi::TextureDesc td;
        td.width  = kNaniteRasterTargetSize;
        td.height = kNaniteRasterTargetSize;
        td.format = rhi::Format::R8_UNORM;
        td.usage  = rhi::TextureUsage::RenderTarget;
        m_Target = m_Device->CreateTexture(td);
        if (!m_Target) { HE_CORE_ERROR("NaniteRaster: 1×1 R8 目标创建失败"); return false; }
    }

    // ── 2. 占位顶点缓冲 ──
    // 顶点着色器只吃 SV_VertexID/SV_InstanceID，不读任何属性；但 Vulkan 的
    // DrawIndexedIndirectCount 仍会绑定顶点缓冲（命令里的 vertexOffset 属于"顶点取址"的一部分）。
    // 【为什么只有顶点缓冲在这里建、索引缓冲懒建】索引缓冲的容量取决于**资产**（见
    //   `SetPlaceholderIndexCapacity`），而 `Initialize` 时资产还没上传 ⇒ 懒建 + 一次性放大。
    {
        rhi::BufferDesc vd;
        vd.size        = sizeof(float) * 3;   // 一个不参与取值的顶点
        vd.usage       = rhi::BufferUsage::Vertex;
        vd.cpuAccess   = true;
        vd.initialData = nullptr;
        m_DummyVB = m_Device->CreateBuffer(vd);
        if (!m_DummyVB) { HE_CORE_ERROR("NaniteRaster: 占位顶点缓冲创建失败"); return false; }
        float zeros[3] = { 0.0f, 0.0f, 0.0f };
        if (void* p = m_DummyVB->Map()) { std::memcpy(p, zeros, sizeof(zeros)); m_DummyVB->Unmap(); }
    }

    // ── 3. 片元描述符集：显式绑定"已光栅化的绘制条数"SSBO（binding 0）──
    // 【只有一个绑定】片元用 `SV_PrimitiveID` 判定"本条命令的第一个三角形" ⇒ 每个绘制恰好
    //   加一，不需要任何按簇号去重的辅助结构（可见簇引用是 (实例, 簇) 二元组，同一个簇会被
    //   多个实例各引用一次，按簇号去重会把它们错误地折叠 —— 见 Nanite_Raster.frag.slang）。
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskFragment, false },
    };
    m_Layout = m_Device->CreateDescriptorSetLayout(layout);
    m_Set    = m_Device->AllocateDescriptorSet(m_Layout);
    m_Device->UpdateDescriptorSet(m_Set, 0, rhi::DescriptorType::StorageBuffer, rasterCountBuffer);

    // ── 4. 图形 PSO（单颜色附件 R8，无深度）──
    m_VS.stage      = rhi::ShaderStage::Vertex;
    m_VS.spirv      = k_Nanite_Raster_vert_spv;
    m_VS.entryPoint = "main";
    m_FS.stage      = rhi::ShaderStage::Pixel;
    m_FS.spirv      = k_Nanite_Raster_frag_spv;
    m_FS.entryPoint = "main";

    rhi::PipelineStateDesc desc;
    desc.vertexShader        = &m_VS;
    desc.pixelShader         = &m_FS;
    desc.topology            = rhi::PrimitiveTopology::TriangleList;
    desc.cullMode            = rhi::CullMode::None;
    desc.depthTest           = false;   // 不写深度：与 GBuffer 深度无关
    desc.depthWrite          = false;
    // Unknown ⇒ 渲染通道只有 1 个颜色附件（与 BeginOffscreenPass(nullptr 深度) 一致，
    // 见 LumenScene.cpp:136 的同款写法）
    desc.depthFormat         = rhi::Format::Unknown;
    desc.colorAttachmentCount = 1;
    desc.colorFormats[0]     = rhi::Format::R8_UNORM;
    desc.descriptorSetLayouts = { m_Layout };
    desc.debugName           = "NaniteRaster";
    m_PSO = m_Device->CreatePipelineState(desc);
    if (!m_PSO) { HE_CORE_ERROR("NaniteRaster: 图形 PSO 创建失败"); return false; }

    HE_CORE_INFO("NaniteRaster: 初始化完成（DrawIndexedIndirectCount → {}×{} R8 目标）",
                 kNaniteRasterTargetSize, kNaniteRasterTargetSize);
    return true;
}

void NaniteRaster::Shutdown() {
    m_PSO.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_Layout);

    // 【§14.8 任务 4】UAV 自证通道的懒建资源（从未开启时它们是空的，这里自然是空操作）
    m_TestWritePSO.reset();
    if (m_Device && m_TestWriteLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_TestWriteLayout);
    m_TestWriteLayout = rhi::kInvalidLayout;
    m_TestWriteSet    = rhi::kInvalidSet;

    // 【§14.8 任务 6】mesh PSO 自证通道的懒建资源（从未开启时它们是空的，这里自然是空操作）
    m_MeshTestPSO.reset();
    if (m_Device && m_MeshTestLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_MeshTestLayout);
    m_MeshTestLayout = rhi::kInvalidLayout;
    m_MeshTestSet    = rhi::kInvalidSet;
    m_MeshTestTarget.reset();
    m_MeshTestCount.reset();
    m_MeshTestReadback.reset();

    // ── 【§14.8 任务 18】软光栅的懒建资源（未开启软光栅时它们是空的，这里自然是空操作）──
    m_SoftRasterPSO.reset();
    m_SoftColorPSO.reset();
    m_DepthResolvePSO.reset();
    if (m_Device && m_SoftDepthLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_SoftDepthLayout);
    if (m_Device && m_SoftColorLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_SoftColorLayout);
    if (m_Device && m_DepthResolveLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_DepthResolveLayout);
    m_SoftDepthLayout       = rhi::kInvalidLayout;
    m_SoftDepthSet          = rhi::kInvalidSet;
    m_SoftColorLayout       = rhi::kInvalidLayout;
    m_SoftColorSet          = rhi::kInvalidSet;
    m_DepthResolveLayout    = rhi::kInvalidLayout;
    m_DepthResolveSet       = rhi::kInvalidSet;
    m_DepthKey.reset();
    m_DepthKeyZeroSrc.reset();
    m_DepthKeyPixels        = 0u;
    m_DepthKeyZeroSrcPixels = 0u;
    m_SoftStats.reset();
    m_SoftStatsZeroSrc.reset();

    // ── 【§14.8 任务 22】硬光栅的懒建资源（未开启分流时它们是空的，这里自然是空操作）──
    m_HardRasterPSO.reset();
    if (m_Device && m_HardRasterLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_HardRasterLayout);
    m_HardRasterLayout     = rhi::kInvalidLayout;
    m_HardRasterSet        = rhi::kInvalidSet;
    m_HardBindlessRegistered = false;
    m_HardStats.reset();
    m_HardStatsZeroSrc.reset();
    m_HardLastMaxTriangles     = 0u;
    m_HardLastVisibleCapacity  = 0u;

    m_Target.reset();
    m_DummyVB.reset();
    m_PlaceholderIB.reset();
    m_PlaceholderIndexCount = 0u;
    m_RasterCountRef = nullptr;

    m_Layout = rhi::kInvalidLayout;
    m_Set    = rhi::kInvalidSet;
    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
    m_LastMaxDrawCount = 0;
}

void NaniteRaster::OnResize(u32 width, u32 height) {
    // 目标是固定 1×1（只数次数），不随视口变化；这里只记录尺寸保持骨架语义一致。
    m_Width  = width;
    m_Height = height;
}

void NaniteRaster::RecordRasterPass(rhi::IRHICommandList* cmd,
                                    rhi::IRHIBuffer* indirectCmdBuffer,
                                    rhi::IRHIBuffer* countBuffer,
                                    u32 maxDrawCount) {
    if (!cmd || !m_PSO || !m_Target || !indirectCmdBuffer || !countBuffer) return;
    // 【任务 16】占位索引缓冲懒建：它的容量按资产给（见 SetPlaceholderIndexCapacity），
    //   而 Initialize 时资产还没上传 ⇒ 首次录制时才建。
    if (!EnsurePlaceholderIndexBuffer()) return;

    m_LastMaxDrawCount = maxDrawCount;

    // ── ① 命令与计数由 compute 写出 → 本绘制（DrawIndirect）──
    // 【为什么必须有这条屏障】命令缓冲与绘制计数是**compute** 写的（可见链写在本 pass 体的前
    //   一段、假簇链写在 `Nanite_Cull` pass 里），而绘制端要在 `DrawIndirect` 阶段读它们；
    //   帧图不会为模块自持缓冲插入依赖（它们不是帧图资源）⇒ 同步必须在这里显式给出。
    //   【为什么用内存屏障】`PipelineBarrier` 无资源重载发的是 `VkMemoryBarrier`（全局），
    //   覆盖两个缓冲，不必逐资源写。
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader,
                         rhi::PipelineStage::DrawIndirect,
                         rhi::ResourceState::UnorderedAccess,
                         rhi::ResourceState::IndirectArgument);

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);

    // 模块自建目标：1×1 R8。清成 0（内容无意义，只是让 RenderDoc 里可辨识）。
    rhi::ClearValue clear{};
    clear.color[0] = 0.0f;
    clear.color[1] = 0.0f;
    clear.color[2] = 0.0f;
    clear.color[3] = 1.0f;
    cmd->BeginOffscreenPass(m_Target->GetNativeHandle(), nullptr,
                            kNaniteRasterTargetSize, kNaniteRasterTargetSize, &clear, false);
    // 1×1 视口 + 剪裁：全屏三角形 ⇒ 每条间接命令的第一个三角形恰好产生 1 个片元，
    // 片元里用 `SV_PrimitiveID == 0` 判定"本条命令的第一个三角形" ⇒ 每个绘制恰好计一次。
    cmd->SetViewport({ 0.0f, (float)kNaniteRasterTargetSize,
                       (float)kNaniteRasterTargetSize, -(float)kNaniteRasterTargetSize, 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, kNaniteRasterTargetSize, kNaniteRasterTargetSize });

    // 绘制端点：`DrawIndexedIndirectCount`。maxDrawCount 只是命令缓冲容量上限，
    // 真正画几条由 countBuffer 的值决定 —— 这正是"可见簇数驱动绘制条数"的实现。
    cmd->SetVertexBuffer(m_DummyVB.get(), 0);
    cmd->SetIndexBuffer(m_PlaceholderIB.get(), 0);
    cmd->SetDrawDebugLabel("Nanite_Raster (indirect count)");
    cmd->DrawIndexedIndirectCount(indirectCmdBuffer, 0, countBuffer, 0,
                                  maxDrawCount, (u32)sizeof(NaniteIndirectCommand));

    cmd->EndOffscreenPass();
}

// ============================================================
// §14.8 任务 16：占位索引缓冲（覆盖资产的整个索引位置空间；内容 = 0,1,2 周期模式）
// ============================================================

void NaniteRaster::SetPlaceholderIndexCapacity(u32 indexCount) {
    if (!m_Device) return;
    // 至少放下假簇链的一条命令（indexCount = 3）；至多不超过可证上界（防止损坏资产撑爆显存）。
    u32 want = (indexCount < kNaniteFakeClusterIndexCount) ? kNaniteFakeClusterIndexCount : indexCount;
    if (want > kNanitePlaceholderIndexCountMax) {
        HE_CORE_WARN("NaniteRaster: 占位索引缓冲的请求容量 {} 超过可证上界 {} ⇒ 按其钳制"
                     "（资产索引数不该超过它；超过说明簇记录异常）",
                     want, kNanitePlaceholderIndexCountMax);
        want = kNanitePlaceholderIndexCountMax;
    }
    if (m_PlaceholderIB && want <= m_PlaceholderIndexCount) return;   // 现有缓冲已经够大

    // 【替换的安全性】本函数只在 `EnsureAssetUploaded` 的一次性路径上被调用，而那条路径里的
    //   `NaniteScene::UploadPackedAsset` 已经 `WaitIdle()` ⇒ 没有任何在飞的命令缓冲还引用旧缓冲。
    //   之后的帧只读新缓冲，不再重建。
    if (!CreatePlaceholderIndexBuffer(want)) {
        HE_CORE_ERROR("NaniteRaster: 占位索引缓冲扩容失败（请求 {} 个索引位置）", want);
        return;
    }
    HE_CORE_INFO("NaniteRaster: 占位索引缓冲就绪（{} 个索引位置 = {} B；内容为 0,1,2 周期模式，"
                 "覆盖资产的整个索引位置空间）",
                 m_PlaceholderIndexCount,
                 (unsigned long long)m_PlaceholderIndexCount * sizeof(u32));
}

bool NaniteRaster::EnsurePlaceholderIndexBuffer() {
    if (m_PlaceholderIB) return true;
    // 还没被告知资产规模（例如资产为空/上传失败）⇒ 建一条命令的最小容量，够假簇链用。
    return CreatePlaceholderIndexBuffer(kNaniteFakeClusterIndexCount);
}

bool NaniteRaster::CreatePlaceholderIndexBuffer(u32 indexCount) {
    if (!m_Device || indexCount == 0u) return false;

    rhi::BufferDesc d;
    d.size      = sizeof(u32) * indexCount;
    // Index：`SetIndexBuffer` 会按这个 usage 建索引缓冲视图（缓冲 ≥ 4B ⇒ 索引类型为 UINT32）。
    d.usage     = rhi::BufferUsage::Index;
    d.cpuAccess = true;   // 创建时写入周期模式（内容每帧不变 ⇒ 之后只被 GPU 读）
    auto buffer = m_Device->CreateBuffer(d);
    if (!buffer) return false;

    // 内容 = 0,1,2,0,1,2,…：让任意 `[firstIndex, firstIndex+indexCount)`（firstIndex 是 3 的倍数、
    // indexCount 是 3 的倍数）都读出 `{0,1,2}` 的周期序列 ⇒ 每个三角形的三个 `SV_VertexID`
    // 取模 3 后是 `{0,1,2}` 的一个排列 ⇒ 顶点着色器画出的全屏三角形**非退化** ⇒ 每个三角形
    // 都产生片元（否则退化三角形会被光栅器整块丢弃，"绘制条数"读数就不可信）。
    if (void* p = buffer->Map()) {
        auto* words = static_cast<u32*>(p);
        for (u32 i = 0; i < indexCount; ++i) words[i] = i % kNaniteIndicesPerTriangle;
        buffer->Unmap();
    } else {
        HE_CORE_ERROR("NaniteRaster: 占位索引缓冲映射失败（{} 个索引位置）", indexCount);
        return false;
    }

    m_PlaceholderIB = std::move(buffer);
    m_PlaceholderIndexCount = indexCount;
    return true;
}

u32 NaniteRaster::ReadbackRasterCount() {
    // 真实 GPU 读回：片元里"每个绘制恰好 +1"的原子计数（= 绘制端实际画出的条数）
    u32 n = 0;
    if (auto* b = m_RasterCountRef) {
        if (void* p = b->Map()) { n = *static_cast<const u32*>(p); b->Unmap(); }
    }
    return n;
}

// ============================================================
// §14.8 任务 4：UAV 自证通道（往既有 GBuffer albedo 写可识别图案）
// ============================================================

bool NaniteRaster::EnsureTestWriteResources() {
    if (m_TestWritePSO) return true;
    if (!m_Device) return false;

    // ── 描述符集布局：唯一的绑定是 GBuffer albedo 的存储图像（set=0 / binding=0）──
    // 用 StorageImage 而不是 CombinedImageSampler：RWTexture2D 是"可写图像"，与采样器无关。
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        { 0, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute, false },
    };
    m_TestWriteLayout = m_Device->CreateDescriptorSetLayout(layout);
    if (m_TestWriteLayout == rhi::kInvalidLayout) {
        HE_CORE_ERROR("NaniteRaster: Nanite_TestWrite 描述符集布局创建失败");
        return false;
    }
    m_TestWriteSet = m_Device->AllocateDescriptorSet(m_TestWriteLayout);

    // ── compute PSO（无 push constant：分辨率由着色器 GetDimensions 从纹理自身取）──
    m_TestWriteCS.stage      = rhi::ShaderStage::Compute;
    m_TestWriteCS.spirv      = k_Nanite_TestWrite_comp_spv;
    m_TestWriteCS.entryPoint = "main";

    rhi::PipelineStateDesc desc;
    desc.bindPoint            = rhi::PipelineBindPoint::Compute;
    desc.computeShader        = &m_TestWriteCS;
    desc.descriptorSetLayouts = { m_TestWriteLayout };
    desc.debugName            = "NaniteTestWrite";
    m_TestWritePSO = m_Device->CreatePipelineState(desc);
    if (!m_TestWritePSO) {
        HE_CORE_ERROR("NaniteRaster: Nanite_TestWrite compute PSO 创建失败");
        return false;
    }

    HE_CORE_INFO("NaniteRaster: 任务 4 UAV 自证通道就绪（Nanite_TestWrite → GBuffer albedo）");
    return true;
}

void NaniteRaster::RecordTestWritePass(rhi::IRHICommandList* cmd, rhi::IRHITexture* albedo) {
    if (!cmd || !albedo) return;
    if (!EnsureTestWriteResources()) return;

    // 描述符指向**本帧的**那张 albedo：GBuffer 纹理在视口变化时会重建，故每次录制都更新绑定。
    // 用 ImageView 直接绑定（与 LumenSDF / RT 各 pass 的存储图像写法同构）。
    m_Device->UpdateDescriptorSetWithImageView(m_TestWriteSet, 0,
        rhi::DescriptorType::StorageImage, albedo->GetNativeHandle());

    const u32 w = albedo->GetWidth();
    const u32 h = albedo->GetHeight();
    if (w == 0 || h == 0) return;

    cmd->SetPipeline(m_TestWritePSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_TestWriteSet);

    // ── 屏障①：GBuffer 写入 → 本 compute 的 UAV 写入 ──
    // 帧图已经为这条 pass 推导过一次 `RenderTarget → UnorderedAccess`（见 RenderGraph::DeriveBarriers），
    // 但那条 barrier 的 dstStage 取自 RHI 的保守映射（UAV ⇒ RayTracingShader），**不含 ComputeShader**，
    // 对 compute 派发并不构成执行依赖。这里补一条显式的 `ColorAttachmentOutput → ComputeShader`
    // 屏障，把布局转换与内存可见性都明确地定序到本次派发之前（RHI 会用追踪到的真实布局纠正
    // oldLayout，因此这条 barrier 与帧图那条不会冲突）。
    cmd->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                         rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::RenderTarget,
                         rhi::ResourceState::UnorderedAccess,
                         albedo);

    cmd->SetDrawDebugLabel("Nanite_TestWrite (GBuffer albedo UAV)");
    cmd->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1);

    // ── 屏障②：本 compute 的 UAV 写入 → 之后所有采样 albedo 的 pass（Lighting / GI）──
    // 帧图同样会在 Lighting 前推导一条 `UnorderedAccess → ShaderResource`，但它的 srcStage 同样是
    // 保守映射（RayTracingShader）。这条显式的 `ComputeShader → FragmentShader` 才是
    // "compute 写的内容对同帧 Lighting 可见" 的直接依据 —— 也就是任务 4 验收的同步基础。
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader,
                         rhi::PipelineStage::FragmentShader,
                         rhi::ResourceState::UnorderedAccess,
                         rhi::ResourceState::ShaderResource,
                         albedo);
}

// ============================================================
// §14.8 任务 6：mesh PSO 自证通道
//   `PipelineStateDesc::meshShader`（VulkanPipeline.cpp:485-713 的 mesh 分支）→ 最小 mesh 管线
//   → `DrawMeshTasks(1,1,1)` → 1×1 R8 目标 + 片元原子计数（两个独立的"非空"GPU 读数）。
// ============================================================

bool NaniteRaster::EnsureMeshTestResources() {
    if (m_MeshTestPSO) return true;
    if (!m_Device || !m_MeshShaderSupported) return false;

    // ── 1. 模块自建的 1×1 R8 目标 ──
    // usage = RenderTarget（当颜色附件）| TransferSrc（任务 6 要 `CopyTextureToBuffer` 读回）
    //         | ShaderResource。
    // 【为什么必须带 ShaderResource】`CopyTextureToBuffer` 拷完会把真实布局**无条件**还原成
    //   `SHADER_READ_ONLY_OPTIMAL`（见 VulkanCommandList.cpp:756-766）；图像没有 SAMPLED 位时
    //   校验层报 VUID-VkImageMemoryBarrier-oldLayout-01211（实测每帧一条、封顶 10 条）。
    //   这里只是**多给一个 usage 位**（usage 只增、不改变任何既有链路），本目标从不被采样。
    // 【为什么不复用任务 3 的目标】见 NaniteTypes.h 的 kNaniteMeshTestTargetSize 说明：
    //   给已验收的 task 3 目标加 usage 会改动那条链路，故另建一张独立小目标。
    {
        rhi::TextureDesc td;
        td.width  = kNaniteMeshTestTargetSize;
        td.height = kNaniteMeshTestTargetSize;
        td.format = rhi::Format::R8_UNORM;
        td.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc
                  | rhi::TextureUsage::ShaderResource;
        m_MeshTestTarget = m_Device->CreateTexture(td);
        if (!m_MeshTestTarget) { HE_CORE_ERROR("NaniteRaster: mesh 通道 1×1 R8 目标创建失败"); return false; }
    }

    // ── 2. 片元原子计数缓冲（dump 帧 CPU Map 读回）──
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32);
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // 每帧清零 + dump 帧读回（与 NaniteCull 的计数缓冲同做法）
        m_MeshTestCount = m_Device->CreateBuffer(d);
        if (!m_MeshTestCount) { HE_CORE_ERROR("NaniteRaster: mesh 通道计数缓冲创建失败"); return false; }
    }

    // ── 3. 1×1 R8 → host 的读回缓冲 ──
    // `BufferUsage::Storage` 这条路径恒定带 TRANSFER_DST（可作 `CopyTextureToBuffer` 的目标），
    // 与 07.Nanite.cpp 里白炉/落盘读回缓冲的写法一致（见该文件对 dd.usage 的注释）。
    {
        rhi::BufferDesc d;
        // 紧凑行距（`CopyTextureToBuffer` 按 width×纹素字节数排布）⇒ 宽×高×1 字节。
        d.size      = kNaniteMeshTestTargetSize * kNaniteMeshTestTargetSize;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;
        m_MeshTestReadback = m_Device->CreateBuffer(d);
        if (!m_MeshTestReadback) { HE_CORE_ERROR("NaniteRaster: mesh 通道读回缓冲创建失败"); return false; }
    }

    // ── 4. 片元描述符集：binding 0 = 计数 SSBO（显式绑定，不走 bindless）──
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskFragment, false },
    };
    m_MeshTestLayout = m_Device->CreateDescriptorSetLayout(layout);
    if (m_MeshTestLayout == rhi::kInvalidLayout) {
        HE_CORE_ERROR("NaniteRaster: mesh 通道描述符集布局创建失败");
        return false;
    }
    m_MeshTestSet = m_Device->AllocateDescriptorSet(m_MeshTestLayout);
    m_Device->UpdateDescriptorSet(m_MeshTestSet, 0, rhi::DescriptorType::StorageBuffer,
                                  m_MeshTestCount.get());

    // ── 5. 最小 mesh PSO：等价的 `PipelineStateDesc`，把 `meshShader` 填上即可 ──
    // 【关键】`desc.meshShader` 非空即走 `VulkanPipeline.cpp:485` 的 mesh 分支：不建 IA、
    //   不建顶点输入，渲染通道只有 1 个颜色附件（`depthFormat = Unknown` ⇒ 无深度附件）。
    //   push constant 留空：mesh 分支会自动补一条 128B 的 Mesh|Task|Fragment 范围（既有行为）。
    m_MeshTestMS.stage      = rhi::ShaderStage::Mesh;
    m_MeshTestMS.spirv      = k_Nanite_MeshTest_mesh_spv;
    m_MeshTestMS.entryPoint = "main";
    m_MeshTestFS.stage      = rhi::ShaderStage::Pixel;
    m_MeshTestFS.spirv      = k_Nanite_MeshTest_frag_spv;
    m_MeshTestFS.entryPoint = "main";

    rhi::PipelineStateDesc desc;
    desc.meshShader           = &m_MeshTestMS;   // ← §14.8 任务 6 的入口
    desc.pixelShader          = &m_MeshTestFS;
    desc.cullMode             = rhi::CullMode::None;   // 1×1 目标只看"有没有画出来"，不挑绕序
    desc.depthTest            = false;                 // 无深度附件
    desc.depthWrite           = false;
    desc.depthFormat          = rhi::Format::Unknown;
    desc.colorAttachmentCount = 1;
    desc.colorFormats[0]      = rhi::Format::R8_UNORM;
    desc.descriptorSetLayouts = { m_MeshTestLayout };
    desc.debugName            = "NaniteMeshTest";
    m_MeshTestPSO = m_Device->CreatePipelineState(desc);
    if (!m_MeshTestPSO) {
        HE_CORE_ERROR("NaniteRaster: 最小 mesh PSO 创建失败");
        return false;
    }

    HE_CORE_INFO("NaniteRaster: 任务 6 mesh PSO 就绪（PipelineStateDesc::meshShader → "
                 "DrawMeshTasks → {}×{} R8 目标）", kNaniteMeshTestTargetSize, kNaniteMeshTestTargetSize);
    return true;
}

void NaniteRaster::RecordMeshTestPass(rhi::IRHICommandList* cmd) {
    if (!cmd) return;
    if (!EnsureMeshTestResources()) return;

    // 每帧把计数清零：GPU 的 InterlockedAdd 从 0 开始，最终值 = 本帧被光栅化的 mesh 图元数。
    // 【为什么 CPU Map 清零】与 `NaniteCull::ResetFrameBuffers` 完全同做法（模块内一致的既有口径），
    // 不为此新造 GPU 清零路径。
    if (void* p = m_MeshTestCount->Map()) {
        *static_cast<u32*>(p) = 0u;
        m_MeshTestCount->Unmap();
    }

    cmd->SetPipeline(m_MeshTestPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_MeshTestSet);

    // 模块自建目标：1×1 R8，清成 0（内容本身无意义，只为让 RenderDoc 里可辨识）。
    rhi::ClearValue clear{};
    clear.color[0] = 0.0f;
    clear.color[1] = 0.0f;
    clear.color[2] = 0.0f;
    clear.color[3] = 1.0f;
    cmd->BeginOffscreenPass(m_MeshTestTarget->GetNativeHandle(), nullptr,
                            kNaniteMeshTestTargetSize, kNaniteMeshTestTargetSize, &clear, false);
    // 视口/剪裁与任务 3 的 1×1 pass 同款（y 方向翻转与 Vulkan 约定一致）
    cmd->SetViewport({ 0.0f, (float)kNaniteMeshTestTargetSize,
                       (float)kNaniteMeshTestTargetSize, -(float)kNaniteMeshTestTargetSize, 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, kNaniteMeshTestTargetSize, kNaniteMeshTestTargetSize });

    // 绘制端点：1 个 mesh 工作组（`Nanite_MeshTest.mesh.slang` 输出 4 顶点 / 2 图元）。
    cmd->SetDrawDebugLabel("Nanite_MeshTest (1 meshlet group, 2 prims)");
    cmd->DrawMeshTasks(1, 1, 1);

    cmd->EndOffscreenPass();

    // 把 1×1 R8 目标拷进 host 可见缓冲（dump 帧 Map 读回 target_max）。
    // 【必须录在 render pass 之外】与 07.Nanite.cpp 的白炉探针/落盘读回同一约定；
    // `CopyTextureToBuffer` 内部自行做 TRANSFER_SRC 布局往返，并回写布局追踪器。
    cmd->CopyTextureToBuffer(m_MeshTestTarget.get(), m_MeshTestReadback.get(),
                             0, 0, kNaniteMeshTestTargetSize, kNaniteMeshTestTargetSize, 0);
}

u32 NaniteRaster::ReadbackMeshTestOutputs() {
    // 真实 GPU 读回：片元 `InterlockedAdd` 累加的"被光栅化图元数"。
    u32 n = 0;
    if (auto* b = m_MeshTestCount.get()) {
        if (void* p = b->Map()) { n = *static_cast<const u32*>(p); b->Unmap(); }
    }
    return n;
}

u32 NaniteRaster::ReadbackMeshTestTargetMax() {
    // 真实 GPU 读回：`CopyTextureToBuffer` 拷回来的 R8 像素。当前目标是 1×1（故"最大值"就是
    // 那唯一一个像素），这里仍按宽×高取最大值，尺寸常量将来变大也不会读错语义。
    // 写入的是 1.0 ⇒ R8_UNORM 读回 255；若 mesh 通道没画出来（清屏后即拷贝）则为 0。
    u32 v = 0;
    if (auto* b = m_MeshTestReadback.get()) {
        if (void* p = b->Map()) {
            const auto* px = static_cast<const u8*>(p);
            const u32 count = kNaniteMeshTestTargetSize * kNaniteMeshTestTargetSize;
            for (u32 i = 0; i < count; ++i)
                v = (px[i] > v) ? (u32)px[i] : v;
            b->Unmap();
        }
    }
    return v;
}

// ============================================================
// §14.8 任务 18：软光栅（两趟"原子深度键 + 等值复检"写 GBuffer）
//
// 【三趟的顺序与同步】全部录在同一个命令缓冲里、用**显式屏障**定序（帧图不跟踪模块自持资源，
//   也排不动这些内部段；与任务 15/16 的 CullChain 是同一套做法）：
//     清深度键（CopyBuffer）→ 屏障 → 第 1 趟 compute → 屏障 → 第 2 趟 compute
//     → 屏障（颜色 UAV → 可采样）→ 深度解析（全屏片元写 SV_Depth）
// ============================================================

bool NaniteRaster::EnsureSoftRasterResources(const GBufferTargets& targets) {
    if (m_SoftColorPSO) return true;
    if (!m_Device || !targets.HasSoftRasterTargets()) return false;

    // ── 1. 第 1 趟（深度键）的描述符集布局：bindings 0..7 ──
    // 【为什么不走 bindless】模块私有缓冲、生命周期清晰；显式绑定最简单也最稳（与任务 3/13/15 同口径）。
    {
        rhi::DescriptorSetLayoutDesc layout;
        layout.bindings = {
            { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 簇记录
            { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 量化顶点
            { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 打包三角形
            { 3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 可见簇引用
            { 4, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 可见簇计数
            { 5, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 实例表
            { 6, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 深度键（读写）
            { 7, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 读数（读写）
            // 【任务 24】流式的五个只读视图 + 一个反馈环。**关闭档也声明**（布局是 PSO 期固定的），
            //   但关闭档把它们绑到**既有**资产/读数缓冲上作占位、着色器一个字节都不读
            //   ⇒ 不新建任何 GPU 资源（§14.2 不变式 1 的可核对形式：关闭档的 pass 集合与
            //   冻结指纹逐位不变，且本部件一个缓冲都不创建）。
            {15, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 簇→页
            {16, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 页表
            {17, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 池·簇段
            {18, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 池·顶点段
            {19, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 池·三角形段
            {20, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },  // 反馈环（读写）
        };
        m_SoftDepthLayout = m_Device->CreateDescriptorSetLayout(layout);
        if (m_SoftDepthLayout == rhi::kInvalidLayout) {
            HE_CORE_ERROR("NaniteRaster: 软光栅第 1 趟描述符集布局创建失败");
            return false;
        }
        m_SoftDepthSet = m_Device->AllocateDescriptorSet(m_SoftDepthLayout);

        m_SoftDepthCS.stage      = rhi::ShaderStage::Compute;
        m_SoftDepthCS.spirv      = k_Nanite_SoftRasterDepth_comp_spv;
        m_SoftDepthCS.entryPoint = "main";

        rhi::PushConstantRange pc;
        pc.stageMask = rhi::kStageMaskCompute;
        pc.size      = sizeof(NaniteSoftRasterParams);   // 96B

        rhi::PipelineStateDesc desc;
        desc.computeShader        = &m_SoftDepthCS;
        desc.bindPoint            = rhi::PipelineBindPoint::Compute;
        desc.pushConstantRanges   = { pc };
        desc.descriptorSetLayouts = { m_SoftDepthLayout };
        desc.debugName            = "NaniteSoftRasterDepth";
        m_SoftRasterPSO = m_Device->CreatePipelineState(desc);
        if (!m_SoftRasterPSO) { HE_CORE_ERROR("NaniteRaster: 软光栅第 1 趟 PSO 创建失败"); return false; }
    }

    // ── 2. 第 2 趟（写 GBuffer）的描述符集布局：bindings 0..7 同第 1 趟 + 8..11 = 4 张颜色目标 ──
    // 【为什么是两个集合】引擎的 GPU 在**执行期**读描述符、最后一次主机写对整段命令缓冲生效
    //   （任务 15 踩过的坑）⇒ 两次派发的绑定必须在不同集合上，且各自每帧只写一次。
    {
        rhi::DescriptorSetLayoutDesc layout;
        layout.bindings = {
            { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },
            { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },
            { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },
            { 3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },
            { 4, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },
            { 5, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },
            { 6, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },
            { 7, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute, false },
            { 8, rhi::DescriptorType::StorageImage,  1, rhi::kStageMaskCompute, false },  // MRT0 albedo
            { 9, rhi::DescriptorType::StorageImage,  1, rhi::kStageMaskCompute, false },  // MRT1 normal
            {10, rhi::DescriptorType::StorageImage,  1, rhi::kStageMaskCompute, false },  // MRT4 worldPos
            {11, rhi::DescriptorType::StorageImage,  1, rhi::kStageMaskCompute, false },  // MRT7 lightmapKey
            // 【任务 19】材质：12 = 资产材质段（32B/条，普通 SSBO，不走 bindless —— 它每帧只绑一次）；
            //   13/14 = **bindless 纹理/采样器数组**，与既有 GBuffer 路径注册在**同一个堆**上
            //   （`heap->RegisterDescriptorSet(..., 13, 14, 0)`）⇒ 模块采样到的是同一批材质贴图。
            //   `bindless = true` 的两个绑定：堆 Flush 时按"已注册的槽位数"写入（PARTIALLY_BOUND）。
            {12, rhi::DescriptorType::StorageBuffer, 1,    rhi::kStageMaskCompute, false },
            {13, rhi::DescriptorType::SampledImage,  4096, rhi::kStageMaskCompute, true  },
            {14, rhi::DescriptorType::Sampler,       4096, rhi::kStageMaskCompute, true  },
            // 【任务 24】流式视图（与第 1 趟同槽位；关闭档绑既有资产/读数缓冲作占位）
            {15, rhi::DescriptorType::StorageBuffer, 1,    rhi::kStageMaskCompute, false },
            {16, rhi::DescriptorType::StorageBuffer, 1,    rhi::kStageMaskCompute, false },
            {17, rhi::DescriptorType::StorageBuffer, 1,    rhi::kStageMaskCompute, false },
            {18, rhi::DescriptorType::StorageBuffer, 1,    rhi::kStageMaskCompute, false },
            {19, rhi::DescriptorType::StorageBuffer, 1,    rhi::kStageMaskCompute, false },
            {20, rhi::DescriptorType::StorageBuffer, 1,    rhi::kStageMaskCompute, false },
        };
        m_SoftColorLayout = m_Device->CreateDescriptorSetLayout(layout);
        if (m_SoftColorLayout == rhi::kInvalidLayout) {
            HE_CORE_ERROR("NaniteRaster: 软光栅第 2 趟描述符集布局创建失败");
            return false;
        }
        m_SoftColorSet = m_Device->AllocateDescriptorSet(m_SoftColorLayout);

        // ── 【任务 19】把第 2 趟的集合登记到 bindless 堆，并**立刻自己 Flush 一次** ──
        // 【为什么必须"登记 + 强制一次 Flush"】堆只在**有 pending** 时把纹理数组写进已登记的集合，
        //   而材质贴图在场景加载期就注册完了（此后 m_Pending 恒 false）⇒ 若只登记不触发，
        //   本集合的 bindless 数组会一直是"未绑定"（采样读到 0 —— 实测：albedo 全 0、
        //   roughness 落到下限 0.04，正是这条路径的症状）。
        // 【为什么不能指望既有的 Flush】唯一每帧调 `heap->Flush()` 的地方是
        //   `GBufferRenderer_CPU::Render:29`，而软光栅开启时该渲染器**让位、根本不执行**
        //   （§14.27③）⇒ 模块必须自己推一次。触发方式沿用既有做法：
        //   `RegisterTexture(nullptr, nullptr)` 占一个槽位（默认占位纹理）并把堆标成 pending，
        //   随后马上 `Flush()` 把**完整数组**写进所有已登记集合（含本集合）。
        //   代价：bindless 纹理数组永久多一个占位槽（不影响任何材质 ID —— 那些 ID 在此调用之前
        //   就已分配完毕）。Flush 是纯主机侧描述符写（UPDATE_AFTER_BIND），与既有每帧 Flush 同一性质。
        if (!m_BindlessRegistered) {
            if (auto* heap = m_Device->GetBindlessHeap()) {
                heap->RegisterDescriptorSet(m_SoftColorSet, 13u, 14u, 0u);
                heap->RegisterTexture(nullptr, nullptr);
                heap->Flush();
                m_BindlessRegistered = true;
            }
        }

        m_SoftColorCS.stage      = rhi::ShaderStage::Compute;
        m_SoftColorCS.spirv      = k_Nanite_SoftRaster_comp_spv;
        m_SoftColorCS.entryPoint = "main";

        rhi::PushConstantRange pc;
        pc.stageMask = rhi::kStageMaskCompute;
        pc.size      = sizeof(NaniteSoftRasterParams);

        rhi::PipelineStateDesc desc;
        desc.computeShader        = &m_SoftColorCS;
        desc.bindPoint            = rhi::PipelineBindPoint::Compute;
        desc.pushConstantRanges   = { pc };
        desc.descriptorSetLayouts = { m_SoftColorLayout };
        desc.debugName            = "NaniteSoftRasterWrite";
        m_SoftColorPSO = m_Device->CreatePipelineState(desc);
        if (!m_SoftColorPSO) { HE_CORE_ERROR("NaniteRaster: 软光栅第 2 趟 PSO 创建失败"); return false; }
    }

    // ── 3. 深度解析：无颜色附件 + D32 深度附件（写法与 CSMTechnique 的深度专用 PSO 同款）──
    {
        rhi::DescriptorSetLayoutDesc layout;
        layout.bindings = {
            { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskFragment, false },  // 深度键（只读）
            // 【P0 修复（§14.30）】读数缓冲（可写）：本通道原子计数"真正写入非远平面深度的像素数"。
            //   旧实现把 `depth_written` 在 C++ 侧硬编码成 1，正是那个恒真读数掩盖了本 bug。
            { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskFragment, false },  // 软光栅读数
        };
        m_DepthResolveLayout = m_Device->CreateDescriptorSetLayout(layout);
        if (m_DepthResolveLayout == rhi::kInvalidLayout) {
            HE_CORE_ERROR("NaniteRaster: 深度解析描述符集布局创建失败");
            return false;
        }
        m_DepthResolveSet = m_Device->AllocateDescriptorSet(m_DepthResolveLayout);

        m_DepthResolveVS.stage      = rhi::ShaderStage::Vertex;
        m_DepthResolveVS.spirv      = k_Nanite_DepthResolve_vert_spv;
        m_DepthResolveVS.entryPoint = "main";
        m_DepthResolveFS.stage      = rhi::ShaderStage::Pixel;
        m_DepthResolveFS.spirv      = k_Nanite_DepthResolve_frag_spv;
        m_DepthResolveFS.entryPoint = "main";

        rhi::PushConstantRange resolvePc;
        resolvePc.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment;
        resolvePc.size      = sizeof(NaniteDepthResolveParams);

        rhi::PipelineStateDesc desc;
        desc.vertexShader         = &m_DepthResolveVS;
        desc.pixelShader          = &m_DepthResolveFS;
        desc.topology             = rhi::PrimitiveTopology::TriangleList;
        desc.cullMode             = rhi::CullMode::None;   // 全屏大三角形只看覆盖，不挑绕序
        // 【P0 修复（§14.30）：屏幕尺寸必须显式传进来】
        //   深度键是**一维** `RWStructuredBuffer<uint>`，它的 `GetDimensions` 返回的是
        //   "元素个数 + 1"（宽=元素数、高=1），**不是**二维宽高。过去 shader 按二维用，
        //   于是"除第 0 行外全部像素"都被当成越界并写成远平面 ⇒ 模块接管时深度附件恒为 1.0
        //   （Hi-Z 因此永远是空金字塔，见 `NaniteDepthResolveParams` 的注释与实施记录）。
        desc.pushConstantRanges   = { resolvePc };
        // ════════════════════════════════════════════════════════════════════════════════
        // 【P0 修复（§14.30）】这里过去写的是 `depthTest = false`，本意是"深度值完全由 shader 的
        //   `SV_Depth` 决定、不做硬件比较"。**那是错的**：Vulkan 规范对
        //   `VkPipelineDepthStencilStateCreateInfo::depthWriteEnable` 的原文是
        //     "controls whether depth writes are enabled **when depthTestEnable is VK_TRUE**.
        //      Depth writes are **always disabled when depthTestEnable is VK_FALSE**."
        //   ⇒ 深度测试关闭时 `SV_Depth` 被**整块丢弃**，深度附件永远停在 `depthLoadOp`
        //   （`desc.depthLoadOp` 默认 `Clear`）清出来的远平面上。
        // 【实测症状（修前，07.Nanite）】模块接管几何写入后：
        //   · Hi-Z 金字塔恒为远平面 ⇒ `cull3 … occluded=0 occl_mip=[0,0,0,0,0,0,0,0]
        //     hiz_half=[1.000000,1.000000]`（阈值 16 档如此，**阈值 64 全覆盖档也一样**）；
        //   · 阈值 64 档说明它不是"覆盖率不足"的问题，而是深度根本没写进附件；
        //   · 后果不止剔除：接管期间 GBuffer 深度对所有消费者（SSR / 贴花 / 任何深度重建）
        //     都是"全是天空"。判据 ⑦ 的 `hiz1` 档正是被它打红的（§14.30 有完整证据）。
        // 【修法】启用深度测试并取 `Always` —— 语义上与"shader 写什么就是什么"完全等价
        //   （`Always` 恒通过 ⇒ 没有任何片元会被比较丢弃），只是让深度**写入**真正生效。
        // ════════════════════════════════════════════════════════════════════════════════
        desc.depthTest            = true;                  // 必须开：关掉会让下面的 depthWrite 失效
        desc.depthCompare         = rhi::CompareFunc::Always;   // 恒通过 ⇒ 深度值仍由 SV_Depth 决定
        desc.depthWrite           = true;                  // 真正把 SV_Depth 写进深度附件
        desc.depthFormat          = rhi::Format::D32_FLOAT;
        desc.colorAttachmentCount = 0;                     // 深度专用通道（与 CSM/Spot 阴影同款）
        desc.descriptorSetLayouts = { m_DepthResolveLayout };
        desc.debugName            = "NaniteDepthResolve";
        m_DepthResolvePSO = m_Device->CreatePipelineState(desc);
        if (!m_DepthResolvePSO) { HE_CORE_ERROR("NaniteRaster: 深度解析 PSO 创建失败"); return false; }
    }

    // ── 4. 读数缓冲（16×u32）+ 常驻 0 源（每帧在命令缓冲内清）──
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32) * kNaniteSoftStatsCapacity;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // dump 帧读回
        m_SoftStats = m_Device->CreateBuffer(d);
        if (!m_SoftStats) { HE_CORE_ERROR("NaniteRaster: 软光栅读数缓冲创建失败"); return false; }
    }
    if (!m_SoftStatsZeroSrc) {
        rhi::BufferDesc d;
        d.size      = sizeof(u32) * kNaniteSoftStatsCapacity;
        d.usage     = rhi::BufferUsage::TransferSrc;   // 只当拷贝源
        d.cpuAccess = true;
        m_SoftStatsZeroSrc = m_Device->CreateBuffer(d);
        if (!m_SoftStatsZeroSrc) { HE_CORE_ERROR("NaniteRaster: 软光栅读数清零源创建失败"); return false; }
        if (void* p = m_SoftStatsZeroSrc->Map()) {
            std::memset(p, 0, sizeof(u32) * kNaniteSoftStatsCapacity);
            m_SoftStatsZeroSrc->Unmap();
        }
    }

    HE_CORE_INFO("NaniteRaster: 任务 18 软光栅就绪（两趟：原子深度键 + 等值复检写 GBuffer；"
                 "深度走 SV_Depth + 既有深度附件；depth_storage_image_supported={}）",
                 m_DepthStorageImageSupported ? 1 : 0);
    return true;
}

bool NaniteRaster::EnsureDepthKeyBuffers(u32 width, u32 height) {
    if (!m_Device || width == 0u || height == 0u) return false;
    const u32 pixels = width * height;
    if (m_DepthKey && m_DepthKeyPixels == pixels) return true;

    // 【替换的安全性】本函数在 `RecordSoftRasterPass` 里被调用（帧内录制期），而深度键只被
    //   模块自己的三趟使用、且这三趟都录在**当前**命令缓冲里。视口变化时引擎会 OnResize →
    //   此处重建；旧缓冲可能仍被上一帧在飞的命令缓冲引用 —— 与 `NaniteRaster` 的占位索引缓冲
    //   同一取舍（引擎在本设备上是"提交后即弃"的帧模型，重建发生在帧边界）。为避免在飞引用，
    //   录制期不重建：尺寸不一致时**跳过本帧软光栅**并告警一次（由 IsReady 之外的门控承担）。
    rhi::BufferDesc d;
    d.size      = sizeof(u32) * pixels;
    d.usage     = rhi::BufferUsage::Storage;   // Storage 路径恒定带 TRANSFER_DST（每帧的 CopyBuffer 目标）
    d.cpuAccess = true;
    m_DepthKey = m_Device->CreateBuffer(d);
    if (!m_DepthKey) { HE_CORE_ERROR("NaniteRaster: 深度键缓冲创建失败（{} 像素）", pixels); return false; }
    m_DepthKeyPixels = pixels;

    // 【为什么用"常驻 0xFF 源 + CopyBuffer"而不是主机写】任务 13 的教训：录制期的主机写会与
    //   GPU 派发竞争（CPU 领先 GPU ⇒ 第 N+1 帧的写落到第 N 帧派发之前）。命令缓冲内的拷贝
    //   由 GPU 有序执行，零停顿。
    if (!m_DepthKeyZeroSrc || m_DepthKeyZeroSrcPixels != pixels) {
        rhi::BufferDesc z;
        z.size      = d.size;
        z.usage     = rhi::BufferUsage::TransferSrc;
        z.cpuAccess = true;
        m_DepthKeyZeroSrc = m_Device->CreateBuffer(z);
        if (!m_DepthKeyZeroSrc) { HE_CORE_ERROR("NaniteRaster: 深度键清零源创建失败"); return false; }
        if (void* p = m_DepthKeyZeroSrc->Map()) {
            std::memset(p, 0xFF, d.size);
            m_DepthKeyZeroSrc->Unmap();
        }
        m_DepthKeyZeroSrcPixels = pixels;
    }
    HE_CORE_INFO("NaniteRaster: 深度键缓冲就绪（{}×{} = {} B；每帧由命令缓冲内的拷贝清成 0xFFFFFFFF）",
                 width, height, (unsigned long long)d.size);
    return true;
}

void NaniteRaster::RecordDepthKeyClear(rhi::IRHICommandList* cmd) {
    if (!cmd || !m_DepthKey || !m_DepthKeyZeroSrc) return;
    cmd->CopyBuffer(m_DepthKeyZeroSrc.get(), m_DepthKey.get(), (u64)m_DepthKeyPixels * sizeof(u32), 0, 0);
    // 拷贝 → 第 1 趟 compute：显式内存屏障（模块自持缓冲不在帧图里，同步必须自己给）
    cmd->PipelineBarrier(rhi::PipelineStage::Transfer, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::CopyDst, rhi::ResourceState::UnorderedAccess);
}

void NaniteRaster::RecordSoftStatsClear(rhi::IRHICommandList* cmd) {
    if (!cmd || !m_SoftStats || !m_SoftStatsZeroSrc) return;
    cmd->CopyBuffer(m_SoftStatsZeroSrc.get(), m_SoftStats.get(),
                    (u64)kNaniteSoftStatsCapacity * sizeof(u32), 0, 0);
    cmd->PipelineBarrier(rhi::PipelineStage::Transfer, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::CopyDst, rhi::ResourceState::UnorderedAccess);
}

bool NaniteRaster::EnsureGBufferClearResources() {
    if (m_ClearPSO) return true;
    if (!m_Device) return false;

    // 8 张颜色目标的存储图像绑定（任务 4 的 A1 裁决已给全部 8 张加了 UnorderedAccess）
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute, false},
        {1, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute, false},
        {2, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute, false},
        {3, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute, false},
        {4, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute, false},
        {5, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute, false},
        {6, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute, false},
        {7, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute, false},
    };
    m_ClearLayout = m_Device->CreateDescriptorSetLayout(layout);
    if (m_ClearLayout == rhi::kInvalidLayout) {
        HE_CORE_ERROR("NaniteRaster: 清屏描述符集布局创建失败");
        return false;
    }
    m_ClearSet = m_Device->AllocateDescriptorSet(m_ClearLayout);

    m_ClearCS.stage      = rhi::ShaderStage::Compute;
    m_ClearCS.spirv      = k_Nanite_GBufferClear_comp_spv;
    m_ClearCS.entryPoint = "main";

    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskCompute;
    pc.size      = sizeof(float) * 4u * 8u;   // 8 × float4 = 128B（引擎的 push constant 上限）

    rhi::PipelineStateDesc desc;
    desc.computeShader        = &m_ClearCS;
    desc.bindPoint            = rhi::PipelineBindPoint::Compute;
    desc.pushConstantRanges   = { pc };
    desc.descriptorSetLayouts = { m_ClearLayout };
    desc.debugName            = "NaniteGBufferClear";
    m_ClearPSO = m_Device->CreatePipelineState(desc);
    if (!m_ClearPSO) { HE_CORE_ERROR("NaniteRaster: 清屏 compute PSO 创建失败"); return false; }
    return true;
}

void NaniteRaster::RecordGBufferClearPass(rhi::IRHICommandList* cmd, const GBufferTargets& targets) {
    if (!cmd || !targets.HasClearTargets()) return;
    if (!EnsureGBufferClearResources()) return;

    const u32 w = targets.albedo->GetWidth();
    const u32 h = targets.albedo->GetHeight();
    if (w == 0u || h == 0u) return;

    // 【清除值必须与既有路径逐位相同】复刻 `GBufferRenderer_CPU.cpp:38-54`，一个数都不改：
    //   这样"模块接管"与"既有路径"的起点（未被几何覆盖的像素）完全一致，
    //   两档转储的差异才只可能来自几何写入本身。
    float clears[32] = { 0.0f };
    clears[0 * 4 + 3] = 1.0f;   // MRT0 albedo.a  = metallic 1
    clears[1 * 4 + 3] = 1.0f;   // MRT1 normal.a  = roughness 1
    clears[2 * 4 + 3] = 1.0f;   // MRT2 emissive.a= ao 1
    clears[3 * 4 + 0] = 0.0f;   // MRT3 velocity  = 0
    clears[3 * 4 + 1] = 0.0f;
    clears[5 * 4 + 2] = 0.5f;   // MRT5 disneyA: specular = 0.5
    clears[5 * 4 + 3] = 0.0f;
    clears[6 * 4 + 1] = 1.0f;   // MRT6 disneyB: clearcoatGloss = 1
    clears[6 * 4 + 2] = 1.0f;
    clears[6 * 4 + 3] = 1.0f;
    clears[7 * 4 + 3] = 0.0f;   // MRT7 lightmapKey: 天空页号无效

    // 每帧重写绑定：GBuffer 纹理在视口变化时会重建（与任务 4 的 UAV 自证通道同款处理）
    rhi::IRHITexture* colors[8] = { targets.albedo, targets.normal, targets.emissive, targets.velocity,
                                    targets.worldPos, targets.disneyA, targets.disneyB, targets.lightmapKey };
    for (u32 i = 0; i < 8u; ++i) {
        m_Device->UpdateDescriptorSetWithImageView(m_ClearSet, i, rhi::DescriptorType::StorageImage,
                                                   colors[i]->GetNativeHandle());
    }

    // 颜色目标：帧图把它们当 RenderTarget（GB_Clear 的声明），本 pass 要当 UAV 写
    // ⇒ 显式屏障（帧图推导的 dstStage 是保守映射，不含 ComputeShader —— 任务 4 的教训）。
    // 【§14.8 任务 20：源状态取 `Undefined` 而不是 `RenderTarget`】
    //   本 pass 是**全屏 compute 清屏**：8 张颜色目标的**每个像素都会被覆盖**，所以"丢弃上一份
    //   内容"在语义上是精确的，不需要把旧内容当作有效数据。而声明 `from = RenderTarget` 会在
    //   纹理刚（重）建、RHI 布局追踪器还没有记录的那一帧被当真，让校验层记下
    //   "该命令缓冲期望 COLOR_ATTACHMENT_OPTIMAL" ⇒ 接管档的**启动期**布局告警多出 7 条
    //   （实测 `vuid_lines 46 → 42`）。这正是判据 ⑧d 的豁免口径里那条"模块内便宜的修法"。
    for (u32 i = 0; i < 8u; ++i) {
        cmd->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                             rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::Undefined,
                             rhi::ResourceState::UnorderedAccess,
                             colors[i]);
    }

    cmd->SetPipeline(m_ClearPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_ClearSet);
    cmd->SetPushConstants(0, (u32)sizeof(clears), clears);
    cmd->SetDrawDebugLabel("Nanite_GBufferClear (8 MRT via UAV)");
    cmd->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1);

    // 清完即可被后续 pass 采样（帧图还会推导一次，但它的 dstStage 同样偏保守）
    for (u32 i = 0; i < 8u; ++i) {
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader,
                             rhi::PipelineStage::FragmentShader,
                             rhi::ResourceState::UnorderedAccess,
                             rhi::ResourceState::ShaderResource,
                             colors[i]);
    }

    // 【深度不在这里清】`ClearDepthStencil` 在本引擎的 GBuffer 深度上会报
    //   `vkCmdClearDepthStencilImage-pRanges-02659/02660` 与 `VkImageMemoryBarrier-oldLayout-01213`
    //   （实测 10+10+10 条校验行）；而模块的"深度解析"通道在同一个 pass 体内**逐像素**写
    //   `SV_Depth`（未覆盖像素写 1.0 = 远平面）⇒ 深度本来就是全屏重写的，不需要预先清。
}
void NaniteRaster::RecordSoftRasterPass(rhi::IRHICommandList* cmd,
                                        const GBufferTargets& targets,
                                        const AssetViews& asset,
                                        const NaniteSoftRasterParams& params,
                                        rhi::IRHIBuffer* visibleRefs,
                                        rhi::IRHIBuffer* visibleCount,
                                        rhi::IRHIBuffer* instances,
                                        u32 visibleCapacity,
                                        const float clearDepth) {
    if (!cmd || !targets.HasSoftRasterTargets() || !asset.valid()) return;
    if (!visibleRefs || !visibleCount || !instances) return;
    if (!EnsureSoftRasterResources(targets)) return;

    const u32 w = targets.albedo->GetWidth();
    const u32 h = targets.albedo->GetHeight();
    if (!EnsureDepthKeyBuffers(w, h)) return;

    m_SoftLastMaxTriangles  = params.maxTriangles;
    m_SoftLastInstanceCount = params.instanceCount;

    // ── 绑定：每帧重写易变项（资产/可见簇/实例缓冲在资产上传后就不再变，但重写只是几个
    //    vkUpdateDescriptorSets，且引擎的 GPU 在执行期读描述符 ⇒ "每帧写一次"是最稳的口径）──
    // 【任务 24：0/1/2 号槽一槽两用】流式开启时这三槽指向**页池**的三段（着色器经页表换算到
    //   池内偏移），关闭时指向资产段（着色器直读、与任务 23 逐字相同）。15..20 号槽同理，
    //   关闭档把它们绑到**既有**缓冲作占位（着色器不读）⇒ 关闭档不新建任何 GPU 资源。
    const bool streamOn = asset.stream.enabled && asset.stream.valid();
    rhi::IRHIBuffer* clusterSrc  = streamOn ? asset.stream.poolClusters  : asset.clusters;
    rhi::IRHIBuffer* vertexSrc   = streamOn ? asset.stream.poolVertices  : asset.vertices;
    rhi::IRHIBuffer* triangleSrc = streamOn ? asset.stream.poolTriangles : asset.indices;
    // 占位绑定（关闭档）：全部指向**已经存在**的缓冲，且类型合法（structured storage / RW）
    rhi::IRHIBuffer* clusterPageSrc = streamOn ? asset.stream.clusterPage  : asset.clusters;
    rhi::IRHIBuffer* pageTableSrc   = streamOn ? asset.stream.pageTable    : asset.header;
    rhi::IRHIBuffer* poolClusterSrc = streamOn ? asset.stream.poolClusters : asset.clusters;
    rhi::IRHIBuffer* poolVertexSrc  = streamOn ? asset.stream.poolVertices : asset.vertices;
    rhi::IRHIBuffer* poolTriSrc     = streamOn ? asset.stream.poolTriangles: asset.indices;
    rhi::IRHIBuffer* feedbackSrc    = streamOn ? asset.stream.feedback     : m_SoftStats.get();

    // ── 【任务 24】push constant 的流式四个数**由绑定侧唯一决定** ──
    // 【为什么不在调用方填】`pagesEnabled` 必须与"0/1/2 号槽绑的是谁"逐位一致：两者由不同的
    //   代码路径产出时（调用方算 push constant、本函数绑描述符）就有分叉的余地 ——
    //   首次实测的症状正是"着色器按页池取址、描述符却指着资产段"：`page_misses` 很大而
    //   `resident` 恒为 0。这里改成从 `streamOn` 反推，结构上不可能不一致。
    // 【同步约定】调用方只负责 `assetClusterCount`（那是资产元数据，与绑定无关）。
    const u32 callerPagesEnabled = params.pagesEnabled;
    NaniteSoftRasterParams paramsEff = params;
    paramsEff.pagesEnabled   = streamOn ? 1u : 0u;
    paramsEff.clusterStride  = streamOn ? asset.stream.clusterStride  : 0u;
    paramsEff.vertexStride   = streamOn ? asset.stream.vertexStride   : 0u;
    paramsEff.triangleStride = streamOn ? asset.stream.triangleStride : 0u;
    if (streamOn != (callerPagesEnabled != 0u)) {
        // 一次性告警（不静默）：调用方与本函数对"这一帧走不走页池"的判断不一致
        static bool warned = false;
        if (!warned) {
            warned = true;
            HE_CORE_ERROR("NaniteRaster: 流式档位不一致（调用方 pagesEnabled={}，绑定侧 {}）"
                          "—— 已按**绑定侧**执行；请检查 `AddPostGBufferPasses` 是否设置了 "
                          "`views.stream`", callerPagesEnabled, streamOn ? 1 : 0);
        }
    }

    for (rhi::DescriptorSetHandle set : { m_SoftDepthSet, m_SoftColorSet }) {
        m_Device->UpdateDescriptorSet(set, 0, rhi::DescriptorType::StorageBuffer, clusterSrc);
        m_Device->UpdateDescriptorSet(set, 1, rhi::DescriptorType::StorageBuffer, vertexSrc);
        m_Device->UpdateDescriptorSet(set, 2, rhi::DescriptorType::StorageBuffer, triangleSrc);
        m_Device->UpdateDescriptorSet(set, 3, rhi::DescriptorType::StorageBuffer, visibleRefs);
        m_Device->UpdateDescriptorSet(set, 4, rhi::DescriptorType::StorageBuffer, visibleCount);
        m_Device->UpdateDescriptorSet(set, 5, rhi::DescriptorType::StorageBuffer, instances);
        m_Device->UpdateDescriptorSet(set, 6, rhi::DescriptorType::StorageBuffer, m_DepthKey.get());
        m_Device->UpdateDescriptorSet(set, 7, rhi::DescriptorType::StorageBuffer, m_SoftStats.get());
        m_Device->UpdateDescriptorSet(set, 15, rhi::DescriptorType::StorageBuffer, clusterPageSrc);
        m_Device->UpdateDescriptorSet(set, 16, rhi::DescriptorType::StorageBuffer, pageTableSrc);
        m_Device->UpdateDescriptorSet(set, 17, rhi::DescriptorType::StorageBuffer, poolClusterSrc);
        m_Device->UpdateDescriptorSet(set, 18, rhi::DescriptorType::StorageBuffer, poolVertexSrc);
        m_Device->UpdateDescriptorSet(set, 19, rhi::DescriptorType::StorageBuffer, poolTriSrc);
        m_Device->UpdateDescriptorSet(set, 20, rhi::DescriptorType::StorageBuffer, feedbackSrc);
    }
    // 【任务 19】材质段（bindings 13/14 由 bindless 堆负责，这里不写）
    if (asset.materials) {
        m_Device->UpdateDescriptorSet(m_SoftColorSet, 12, rhi::DescriptorType::StorageBuffer,
                                      asset.materials);
    }
    m_Device->UpdateDescriptorSetWithImageView(m_SoftColorSet, 8,  rhi::DescriptorType::StorageImage,
                                               targets.albedo->GetNativeHandle());
    m_Device->UpdateDescriptorSetWithImageView(m_SoftColorSet, 9,  rhi::DescriptorType::StorageImage,
                                               targets.normal->GetNativeHandle());
    m_Device->UpdateDescriptorSetWithImageView(m_SoftColorSet, 10, rhi::DescriptorType::StorageImage,
                                               targets.worldPos->GetNativeHandle());
    m_Device->UpdateDescriptorSetWithImageView(m_SoftColorSet, 11, rhi::DescriptorType::StorageImage,
                                               targets.lightmapKey->GetNativeHandle());
    m_Device->UpdateDescriptorSet(m_DepthResolveSet, 0, rhi::DescriptorType::StorageBuffer, m_DepthKey.get());
    // 【P0 修复（§14.30）】读数缓冲：深度解析通道自己原子累加"真实写入深度的像素数"
    m_Device->UpdateDescriptorSet(m_DepthResolveSet, 1, rhi::DescriptorType::StorageBuffer, m_SoftStats.get());

    // ── ① 清深度键 + 清读数（命令缓冲内拷贝，GPU 有序）──
    RecordDepthKeyClear(cmd);
    RecordSoftStatsClear(cmd);

    // ── ② 颜色目标的布局：帧图推导的 `RenderTarget → UnorderedAccess` 的 dstStage 是保守映射
    //      （RayTracingShader），对 compute 派发不构成执行依赖 ⇒ 这里补显式屏障（与任务 4 同款）──
    for (rhi::IRHITexture* tex : { targets.albedo, targets.normal, targets.worldPos, targets.lightmapKey }) {
        cmd->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                             rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::RenderTarget,
                             rhi::ResourceState::UnorderedAccess,
                             tex);
    }

    // ── ③ 第 1 趟：逐簇一个工作组，dispatch 条数 = 可见簇容量（着色器按可见计数提前返回）──
    // 【为什么要按容量而不是按可见数派发】可见计数是**同一帧 GPU 刚写出的**值，CPU 读不到（读回
    //   要等 GPU ⇒ 破坏无停顿）；按容量派发、由 shader 按 `slot >= visibleCount` 早退，是本引擎
    //   既有的"GPU 驱动"写法。容量 = 可见引用表容量（`NaniteCull::GetBVHVisibleCapacity`）。
    cmd->SetPipeline(m_SoftRasterPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_SoftDepthSet);
    cmd->SetPushConstants(0, (u32)sizeof(NaniteSoftRasterParams), &paramsEff);
    cmd->SetDrawDebugLabel("Nanite_SoftRasterDepth (atomic depth key)");
    cmd->Dispatch(visibleCapacity, 1, 1);

    // ── ④ 第 1 趟 → 第 2 趟：深度键的 RAW（全局内存屏障）──
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess);

    cmd->SetPipeline(m_SoftColorPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_SoftColorSet);
    cmd->SetPushConstants(0, (u32)sizeof(NaniteSoftRasterParams), &paramsEff);
    cmd->SetDrawDebugLabel("Nanite_SoftRaster (equality test -> GBuffer)");
    cmd->Dispatch(visibleCapacity, 1, 1);

    // ── ⑤ 第 2 趟 → 深度解析：深度键 UAV → 只读；4 张颜色 UAV → 可采样（Lighting 要读）──
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::FragmentShader,
                         rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
    for (rhi::IRHITexture* tex : { targets.albedo, targets.normal, targets.worldPos, targets.lightmapKey }) {
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader,
                             rhi::PipelineStage::FragmentShader,
                             rhi::ResourceState::UnorderedAccess,
                             rhi::ResourceState::ShaderResource,
                             tex);
    }

    // ── ⑥ 深度解析：全屏片元，把深度键还原成 NDC 深度写既有深度附件（SV_Depth）──
    // 【布局转换由 RHI 的 render pass 自己完成】`BeginOffscreenPass` 内部会把深度从当前布局
    //   （帧图模型里 GB_Clear 之后的 DEPTH_STENCIL_READ_ONLY）转到 ATTACHMENT，并在结束时
    //   还原成 READ_ONLY —— 于是帧图的模型与真实布局继续一致，不需要额外的深度屏障。
    rhi::ClearValue depthClear{};
    depthClear.depth = clearDepth;
    cmd->SetPipeline(m_DepthResolvePSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DepthResolveSet);
    // 【P0 修复（§14.30）】把屏幕尺寸推给片元：深度键是一维结构化缓冲，shader 不能从它反推
    //   二维宽高（`GetDimensions` 只会给出"元素个数 + 1"）。这里传的就是深度键的二维形状。
    NaniteDepthResolveParams resolveParams{ w, h };
    cmd->SetPushConstants(0, (u32)sizeof(resolveParams), &resolveParams);
    cmd->BeginOffscreenPass(nullptr, targets.depth->GetNativeHandle(), w, h, &depthClear, false);
    cmd->SetViewport({ 0.0f, (float)h, (float)w, -(float)h, 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, w, h });
    cmd->SetDrawDebugLabel("Nanite_DepthResolve (key -> SV_Depth)");
    cmd->Draw(3);
    cmd->EndOffscreenPass();
}

// ============================================================
// §14.8 任务 22：硬光栅（mesh shader 分流，大簇一侧）
//
// 【次序的正确性推导（本任务的关键裁决）】硬光栅录在**软光栅全部三趟之后**（颜色两趟 +
//   深度解析都已把结果写进既有 GBuffer 与 D32 深度附件），PSO 上是
//   `depthTest=LessEqual + depthWrite=true + depthLoadOp=Load + colorLoadOp=Load`：
//     · 软几何已把颜色与深度写进附件；
//     · 硬片元 `dh < ds`（硬更近）⇒ 通过深度测试 ⇒ 硬颜色**覆盖**软颜色、深度改写成 dh
//       ⇒ **硬遮软成立**；
//     · 硬片元 `dh > ds`（软更近）⇒ 深度测试失败 ⇒ 硬片元被丢弃、软颜色与软深度原样保留
//       ⇒ **软遮硬成立**；
//     · 没有软几何的像素 ds = 1.0（深度解析写的远平面）⇒ 硬片元照常写入。
//   也就是说**两个方向的遮挡都正确**，而且不依赖"谁先写"——关键只在"**后写者**是否带深度测试"。
//   反过来把硬光栅排在软光栅**之前**（或在软光栅第 1 趟之前）是不成立的：软光栅第 2 趟的
//   等值复检只对照**软自己的**深度键，看不见硬几何，会无条件覆盖更近的硬颜色。
//
// 【已知边界（如实记录，不是"完全等价"）】
//   ① 深度解析把软深度**截断到 24 位尾数**（`asfloat(key & 0xFFFFFF00)`），所以附件里的软深度
//      比数学值略小；硬片元深度落在"截断值 ~ 真值"这条 < 256 ULP 的极窄带里时，本会比较出
//      "软更近"（实际硬更近）。深度 24 位尾数的相对误差约 6e-8 量级，屏幕上不可见。
//   ② 深度**恰好相等**时 `LessEqual` 让硬片元胜出（平局口径；两侧都可辩护，这里选硬的）。
//   ③ 硬光栅会**写深度附件** ⇒ 它之后的下游（Hi-Z / SSAO / SSR / 任何深度重建）看到的是
//      "软 + 硬"的合成深度，而不再是软光栅时代那个"大簇像素 = 远平面"的深度。
//      这正是 `NaniteSettings::hardRaster` 必须默认关闭的直接原因（判据 ⑧b 只允许 4 个
//      GBuffer 目标变化，而深度变化会牵动 prov1_* / rsm_* / ssr / ibl_irr 等下游转储）。
// ============================================================

bool NaniteRaster::EnsureHardRasterResources(const GBufferTargets& targets) {
    if (m_HardRasterPSO) return true;
    if (!m_Device || !m_HardRasterCapable) return false;
    // 【为什么要求 HasClearTargets（8 张颜色 + 深度）而不是只要 4 张】本通道是**渲染通道**：
    //   PSO 的附件数必须与实际帧缓冲一致（8 张颜色 + 深度），未被写入的 4 张靠
    //   per-MRT `writeMask=None` 保护。少一张就没法建出合法的帧缓冲/渲染通道。
    if (!targets.HasClearTargets()) return false;

    // ── 1. 描述符集布局：与软光栅**同槽位同语义**（bindings 0..7 + 12/13/14）──
    // 【为什么要同槽位】本通道的两个着色器 include 的是同一份 `Nanite_SoftRasterCommon.slang`
    //   （材质求值 `softRasterEvaluateMaterial` 与几何解码都从那里来，不另写一份公式），
    //   那个文件把绑定写死在 0..7/12/13/14 上 ⇒ 布局必须逐条对上。
    // 【binding 7 指向另一个缓冲】软光栅的 7 是那 16 槽读数缓冲；本通道指模块自持的 4 槽
    //   硬光栅读数缓冲（`kNaniteHardStat*`）。shader 源码里的 `u_Stats[...]` 是同一行，
    //   "写到哪里"完全由描述符指向谁决定 —— 这正是把两者的槽位语义隔开的最小手法。
    // 【binding 6（深度键）本通道不用】仍声明并绑定它：Slang 若把它从 SPIR-V 里削掉，
    //   多声明的绑定是合法的；若没削掉，绑定了也不会读到未定义描述符。两边都安全。
    {
        // 阶段掩码：0..5 是 mesh 阶段读、7/12/13/14 是片元阶段用；这里统一写成
        // `Mesh|Fragment` 的并集 —— 布局的 stageFlags 是"允许访问的阶段"，宽声明永远合法。
        const u32 kStages = rhi::kStageMaskMesh | rhi::kStageMaskFragment;
        rhi::DescriptorSetLayoutDesc layout;
        layout.bindings = {
            { 0, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 簇记录
            { 1, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 量化顶点
            { 2, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 打包三角形
            { 3, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 可见簇引用
            { 4, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 可见簇计数
            { 5, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 实例表
            { 6, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 深度键（未使用，占位）
            { 7, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 硬光栅读数（本通道自己的缓冲）
            {12, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 资产材质段
            {13, rhi::DescriptorType::SampledImage,  4096, kStages, true  },  // bindless 纹理数组
            {14, rhi::DescriptorType::Sampler,       4096, kStages, true  },  // bindless 采样器数组
            // 【任务 24】流式视图（与软光栅同槽位；关闭档绑既有资产/硬光栅读数缓冲作占位）
            {15, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 簇→页
            {16, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 页表
            {17, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 池·簇段
            {18, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 池·顶点段
            {19, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 池·三角形段
            {20, rhi::DescriptorType::StorageBuffer, 1,    kStages, false },  // 反馈环（未使用，占位）
        };
        m_HardRasterLayout = m_Device->CreateDescriptorSetLayout(layout);
        if (m_HardRasterLayout == rhi::kInvalidLayout) {
            HE_CORE_ERROR("NaniteRaster: 硬光栅描述符集布局创建失败");
            return false;
        }
        m_HardRasterSet = m_Device->AllocateDescriptorSet(m_HardRasterLayout);
    }

    // ── 2. 读数缓冲（4 槽）+ 常驻 0 源（每帧在命令缓冲内清）──
    // 【为什么不用主机写】任务 13 的教训：录制期的主机写会与 GPU 派发竞争（CPU 领先 GPU 时
    //   第 N+1 帧的写落到第 N 帧派发之前）。命令缓冲内的 `CopyBuffer` 由 GPU 有序执行。
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32) * kNaniteHardStatsCapacity;
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;   // dump 帧读回
        m_HardStats = m_Device->CreateBuffer(d);
        if (!m_HardStats) { HE_CORE_ERROR("NaniteRaster: 硬光栅读数缓冲创建失败"); return false; }

        rhi::BufferDesc z;
        z.size      = d.size;
        z.usage     = rhi::BufferUsage::TransferSrc;   // 只当拷贝源
        z.cpuAccess = true;
        m_HardStatsZeroSrc = m_Device->CreateBuffer(z);
        if (!m_HardStatsZeroSrc) { HE_CORE_ERROR("NaniteRaster: 硬光栅读数清零源创建失败"); return false; }
        if (void* p = m_HardStatsZeroSrc->Map()) {
            std::memset(p, 0, d.size);
            m_HardStatsZeroSrc->Unmap();
        }
    }

    // ── 3. mesh PSO：8 个 GBuffer 颜色附件（Load + 只写 MRT0/1/4/7）+ D32 深度附件（Load + 测试 + 写）──
    m_HardRasterMS.stage      = rhi::ShaderStage::Mesh;
    m_HardRasterMS.spirv      = k_Nanite_HardRaster_mesh_spv;
    m_HardRasterMS.entryPoint = "main";
    m_HardRasterFS.stage      = rhi::ShaderStage::Pixel;
    m_HardRasterFS.spirv      = k_Nanite_HardRaster_frag_spv;
    m_HardRasterFS.entryPoint = "main";

    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskMesh | rhi::kStageMaskFragment;
    pc.size      = sizeof(NaniteSoftRasterParams);   // 96B：与软光栅**同一个**参数结构（同一阈值/同一 vpRows）

    rhi::PipelineStateDesc desc;
    desc.meshShader           = &m_HardRasterMS;   // ← 走 `VulkanPipeline.cpp` 的 mesh 分支（无 IA、无顶点输入）
    desc.pixelShader          = &m_HardRasterFS;
    desc.topology             = rhi::PrimitiveTopology::TriangleList;
    // 与软光栅一致的双面光栅（软光栅的 `softRasterCovered` 明确按面积符号翻转 ⇒ 不挑绕序）
    desc.cullMode             = rhi::CullMode::None;
    desc.depthTest            = true;
    // ── 【本任务最关键的两个状态】──
    // `LessEqual`：排在软光栅之后 ⇒ 只画"不比软几何更远"的片元（两个方向的遮挡都正确，见上面的推导）。
    // `depthWrite = true`：硬的深度也要写回附件，否则后续硬片元之间无法正确互相遮挡。
    desc.depthCompare         = rhi::CompareFunc::LessEqual;
    desc.depthWrite           = true;
    desc.depthFormat          = rhi::Format::D32_FLOAT;
    // ── 【保留软光栅的成果：两个 loadOp 都必须是 Load】──
    // `Load` 而不是 `Clear`：深度解析刚把软深度写进附件、软光栅两趟刚把颜色写进 4 张 MRT；
    // 任何一处用 Clear 都会把 (b) 方案的前提直接抹掉（颜色被清成清除值、深度被清成远平面）。
    desc.colorLoadOp          = rhi::LoadOp::Load;
    desc.depthLoadOp          = rhi::LoadOp::Load;
    desc.colorAttachmentCount = 8;
    // 格式必须与**实际帧缓冲**逐附件一致（否则 vkCreateFramebuffer 与渲染通道不匹配）。
    // 这 8 个格式与 `DecalPass` 用的是同一组（它已在同一批 GBuffer 纹理上验证过）。
    desc.colorFormats[0] = rhi::Format::RGBA16_FLOAT;   // Albedo + Metallic   （写）
    desc.colorFormats[1] = rhi::Format::RGBA16_FLOAT;   // Normal + Roughness  （写）
    desc.colorFormats[2] = rhi::Format::RGBA16_FLOAT;   // Emissive + AO       （不写）
    desc.colorFormats[3] = rhi::Format::RG16_FLOAT;     // Velocity            （不写）
    desc.colorFormats[4] = rhi::Format::RGBA16_FLOAT;   // WorldPos            （写）
    desc.colorFormats[5] = rhi::Format::RGBA16_FLOAT;   // DisneyA             （不写）
    desc.colorFormats[6] = rhi::Format::RGBA16_FLOAT;   // DisneyB             （不写）
    desc.colorFormats[7] = rhi::Format::RGBA16_FLOAT;   // LightmapKey         （写）
    for (u32 i = 0u; i < 8u; ++i) {
        rhi::ColorBlendDesc& b = desc.colorBlend[i];
        b.blendEnable = false;   // 不做混合：本通道的语义是"覆盖"（混合会让硬软颜色互相污染）
        const bool written = (i == 0u || i == 1u || i == 4u || i == 7u);
        // 【未写的 4 张必须显式关掉写入】不能靠"片元没声明这个 location"—— 只写一部分 location
        //   是允许的，但没有 writeMask 保护时驱动可以写未定义值进 emissive/velocity/disney，
        //   那会直接打红判据 ⑧b（"差异只允许 4 个 GBuffer 目标"）。
        b.writeMask = written ? rhi::ColorWriteMask::All : rhi::ColorWriteMask::None;
    }
    desc.pushConstantRanges   = { pc };
    desc.descriptorSetLayouts = { m_HardRasterLayout };
    desc.debugName            = "NaniteHardRaster";
    m_HardRasterPSO = m_Device->CreatePipelineState(desc);
    if (!m_HardRasterPSO) {
        HE_CORE_ERROR("NaniteRaster: 硬光栅 mesh PSO 创建失败（8 附件 + D32，LessEqual + Load）");
        return false;
    }

    // ── 4. bindless 登记 + **强制一次 Flush**（与软光栅第 2 趟集合同一手法）──
    // 【为什么必须自己推一次】堆只在有 pending 时把纹理数组写进已登记的集合，而材质贴图在
    //   场景加载期就注册完了（此后 m_Pending 恒 false）；唯一每帧调 `heap->Flush()` 的
    //   `GBufferRenderer_CPU::Render` 在模块接管时**根本不执行**（§14.27③）⇒ 不自己推一次的话
    //   本集合的 bindless 数组会一直是"未绑定"，硬光栅的 albedo 会全 0（软光栅踩过同样的坑）。
    // 【代价】`RegisterTexture(nullptr, nullptr)` 占一个占位槽位并标 pending；它在本通道的
    //   首次且仅此一次的懒建路径上发生（不在每帧路径里），且不影响任何已分配的材质 ID。
    if (!m_HardBindlessRegistered) {
        if (auto* heap = m_Device->GetBindlessHeap()) {
            heap->RegisterDescriptorSet(m_HardRasterSet, 13u, 14u, 0u);
            heap->RegisterTexture(nullptr, nullptr);
            heap->Flush();
            m_HardBindlessRegistered = true;
        }
    }

    HE_CORE_INFO("NaniteRaster: 任务 22 硬光栅就绪（mesh 管线：1 工作组/可见簇，"
                 "`triangleCount > maxTriangles` 的簇走这里；LessEqual + Load 保留软光栅结果；"
                 "只写 MRT0/1/4/7）");
    return true;
}

void NaniteRaster::RecordHardStatsClear(rhi::IRHICommandList* cmd) {
    if (!cmd || !m_HardStats || !m_HardStatsZeroSrc) return;
    cmd->CopyBuffer(m_HardStatsZeroSrc.get(), m_HardStats.get(),
                    (u64)kNaniteHardStatsCapacity * sizeof(u32), 0, 0);
    // 拷贝 → mesh/片元着色器的原子累加：显式内存屏障。
    // 【为什么 dstStage 必须显式写出 mesh 阶段】清 0 与 `InterlockedAdd` 之间必须有定义的顺序，
    //   而 `InterlockedAdd` 发生在 **mesh** 与 **fragment** 两个阶段。RHI 的 PipelineStage 为此
    //   新增了 `MeshShader`/`TaskShader` 两位（见 `RHI/Types.h`），不再依赖"掩码为空时退化成
    //   ALL_COMMANDS"这种碰巧成立的写法。
    cmd->PipelineBarrier(rhi::PipelineStage::Transfer,
                         rhi::PipelineStage::MeshShader | rhi::PipelineStage::FragmentShader,
                         rhi::ResourceState::CopyDst,
                         rhi::ResourceState::UnorderedAccess);
}

void NaniteRaster::RecordHardRasterPass(rhi::IRHICommandList* cmd,
                                        const GBufferTargets& targets,
                                        const AssetViews& asset,
                                        const NaniteSoftRasterParams& params,
                                        rhi::IRHIBuffer* visibleRefs,
                                        rhi::IRHIBuffer* visibleCount,
                                        rhi::IRHIBuffer* instances,
                                        u32 visibleCapacity) {
    if (!cmd || !targets.HasClearTargets() || !asset.valid()) return;
    if (!visibleRefs || !visibleCount || !instances) return;
    if (!EnsureHardRasterResources(targets)) return;

    const u32 w = targets.albedo->GetWidth();
    const u32 h = targets.albedo->GetHeight();
    if (w == 0u || h == 0u) return;

    m_HardLastMaxTriangles    = params.maxTriangles;
    m_HardLastVisibleCapacity = visibleCapacity;
    // ── 绑定：每帧重写（与软光栅同一口径：资产/可见簇/实例缓冲在资产上传后就不再变，
    //    但重写只是几个 vkUpdateDescriptorSets，且引擎的 GPU 在执行期读描述符）──
    // 【任务 24：0/1/2 与 15..20 一槽两用】与软光栅第 1/2 趟**逐条同口径**（同一个 include 里
    //   的同一段取值表达式 ⇒ 两条光栅路径不可能分叉）。关闭档的占位绑定指向既有缓冲。
    const bool streamOn = asset.stream.enabled && asset.stream.valid();
    // 【任务 24】与软光栅**同一口径**：流式的四个数由绑定侧唯一决定（见 `RecordSoftRasterPass`
    //   的说明），避免"push constant 说走页池、描述符却指着资产段"这种分叉。
    NaniteSoftRasterParams paramsEff = params;
    paramsEff.pagesEnabled   = streamOn ? 1u : 0u;
    paramsEff.clusterStride  = streamOn ? asset.stream.clusterStride  : 0u;
    paramsEff.vertexStride   = streamOn ? asset.stream.vertexStride   : 0u;
    paramsEff.triangleStride = streamOn ? asset.stream.triangleStride : 0u;
    // ── 【任务 26 / §14.34 第 10 行】把**这一份**（`paramsEff`）的 7 个字段记成 CPU 侧真值 ──
    // 【为什么在这一行而不是函数开头】`pagesEnabled` 只在这里定型（由绑定侧唯一决定）；
    //   记在它之前会把"回读该等于谁"记错，回读判据随即变成自欺。
    //   它们与 mesh shader 写回的 `kHardStatDiag*` 槽逐项对应（见 `LogHardRasterReadback`）。
    m_HardLastScreenW       = paramsEff.screenWidth;
    m_HardLastScreenH       = paramsEff.screenHeight;
    m_HardLastExtentMilli   = (u32)std::max(0.0f, paramsEff.meshMaxExtent * 1000.0f);
    m_HardLastInstanceCount = paramsEff.instanceCount;
    m_HardLastMaterialCount = paramsEff.materialCount;
    m_HardLastPagesEnabled  = paramsEff.pagesEnabled;
    rhi::IRHIBuffer* clusterSrc  = streamOn ? asset.stream.poolClusters  : asset.clusters;
    rhi::IRHIBuffer* vertexSrc   = streamOn ? asset.stream.poolVertices  : asset.vertices;
    rhi::IRHIBuffer* triangleSrc = streamOn ? asset.stream.poolTriangles : asset.indices;
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 0, rhi::DescriptorType::StorageBuffer, clusterSrc);
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 1, rhi::DescriptorType::StorageBuffer, vertexSrc);
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 2, rhi::DescriptorType::StorageBuffer, triangleSrc);
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 3, rhi::DescriptorType::StorageBuffer, visibleRefs);
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 4, rhi::DescriptorType::StorageBuffer, visibleCount);
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 5, rhi::DescriptorType::StorageBuffer, instances);
    // binding 6：本通道不读深度键，但布局声明了它 ⇒ 有就绑上（避免"声明了却从未更新"的告警）
    if (m_DepthKey) {
        m_Device->UpdateDescriptorSet(m_HardRasterSet, 6, rhi::DescriptorType::StorageBuffer,
                                      m_DepthKey.get());
    }
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 7, rhi::DescriptorType::StorageBuffer,
                                  m_HardStats.get());
    if (asset.materials) {
        m_Device->UpdateDescriptorSet(m_HardRasterSet, 12, rhi::DescriptorType::StorageBuffer,
                                      asset.materials);
    }
    // 【任务 24】流式视图（关闭档绑既有缓冲作占位；反馈环绑本通道自己的读数缓冲 —— 它合法且可写，
    //   而本通道从不写它：缺页上报只发生在软光栅第 1 趟，每簇只记一次）
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 15, rhi::DescriptorType::StorageBuffer,
                                  streamOn ? asset.stream.clusterPage : asset.clusters);
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 16, rhi::DescriptorType::StorageBuffer,
                                  streamOn ? asset.stream.pageTable : asset.header);
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 17, rhi::DescriptorType::StorageBuffer,
                                  streamOn ? asset.stream.poolClusters : asset.clusters);
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 18, rhi::DescriptorType::StorageBuffer,
                                  streamOn ? asset.stream.poolVertices : asset.vertices);
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 19, rhi::DescriptorType::StorageBuffer,
                                  streamOn ? asset.stream.poolTriangles : asset.indices);
    m_Device->UpdateDescriptorSet(m_HardRasterSet, 20, rhi::DescriptorType::StorageBuffer,
                                  streamOn ? asset.stream.feedback : m_HardStats.get());

    // ── ① 清读数（命令缓冲内拷贝；与软光栅的 `RecordSoftStatsClear` 同一惯例）──
    RecordHardStatsClear(cmd);

    // ── ② 8 张颜色目标：可采样 → 渲染目标附件 ──
    // 【为什么 8 张都要转】本通道的渲染通道把**全部 8 张**当作颜色附件（未被写的 4 张靠
    //   writeMask=None 保护），而 Vulkan 要求每个附件的真实布局等于渲染通道声明的
    //   initialLayout（`LoadOp::Load` ⇒ COLOR_ATTACHMENT_OPTIMAL）。软光栅第 2 趟结束时把它们
    //   留在"可采样"（`UnorderedAccess → ShaderResource`），清屏通道结束时也是可采样
    //   ⇒ 8 张都得显式转过去，否则 `vkCmdBeginRenderPass` 报 initialLayout 不符。
    // 【帧图为什么不管这件事】帧图只跟踪**它 import 的资源在 pass 边界**的状态，模块在这一段
    //   里插了自己的渲染通道，帧图完全看不见 ⇒ 这一段的布局往返只能由本函数负责。
    rhi::IRHITexture* colors[8] = { targets.albedo, targets.normal, targets.emissive,
                                    targets.velocity, targets.worldPos, targets.disneyA,
                                    targets.disneyB, targets.lightmapKey };
    for (u32 i = 0u; i < 8u; ++i) {
        cmd->PipelineBarrier(rhi::PipelineStage::FragmentShader,
                             rhi::PipelineStage::ColorAttachmentOutput,
                             rhi::ResourceState::ShaderResource,
                             rhi::ResourceState::RenderTarget,
                             colors[i]);
    }

    // ── ③ 渲染：mesh 绘制 ──
    cmd->SetPipeline(m_HardRasterPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_HardRasterSet);
    cmd->SetPushConstants(0, (u32)sizeof(NaniteSoftRasterParams), &paramsEff);

    void* views[8] = {};
    for (u32 i = 0u; i < 8u; ++i) views[i] = colors[i]->GetNativeHandle();
    // clears = nullptr：两个 loadOp 都是 Load，清除值不参与（`BeginOffscreenPassMRT` 的
    // 清除值数组契约是"颜色数 + 1 个深度"，传 nullptr 时它的内部数组是零初始化的）。
    cmd->BeginOffscreenPassMRT(views, 8u, targets.depth->GetNativeHandle(), w, h, nullptr, false);
    // 【视口必须与软光栅/深度解析同款（负高度）】本引擎的离屏通道统一用 `-h` 抵消"NDC y 向上、
    //   帧缓冲 y 向下"的差异；软光栅的 `softRasterNdcToPixel` 也是按这条约定把 NDC 映射到行列的
    //   ⇒ 只有用同一个负高度视口，两条光栅路径才会落在**同一批像素**上。
    cmd->SetViewport({ 0.0f, (float)h, (float)w, -(float)h, 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, w, h });
    // 任务数 = 可见簇容量（与软光栅同一条既有口径：可见计数是**同一帧 GPU 刚写出**的、CPU 读不到，
    // 读回会破坏无停顿）⇒ 由 shader 按 `slot >= visibleCount` 与分流判据早退收口。
    cmd->SetDrawDebugLabel("Nanite_HardRaster (mesh, triangleCount > maxTriangles)");
    cmd->DrawMeshTasks(visibleCapacity, 1u, 1u);
    cmd->EndOffscreenPass();

    // ── ④ 画完转回可采样（Lighting / Decal 等下游按帧图模型把它们当 ShaderResource）──
    // 【为什么 8 张都要转回】渲染通道结束时 8 张附件都停在 COLOR_ATTACHMENT（RHI 的
    //   `TrackColorAttachmentsAfterPass` 也是这么记账的）⇒ 全部转回可采样，帧图后续推导的
    //   `RenderTarget → ShaderResource` 才与追踪器一致。
    for (u32 i = 0u; i < 8u; ++i) {
        cmd->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                             rhi::PipelineStage::FragmentShader,
                             rhi::ResourceState::RenderTarget,
                             rhi::ResourceState::ShaderResource,
                             colors[i]);
    }
    // 【深度不在这里转】深度附件在渲染通道结束时由 RHI 记成"只读"（`EndOffscreenPass`），
    //   与深度解析通道之后的状态一致 ⇒ 与帧图对 gbDepth 的模型继续吻合；下一次以 Load 开始的
    //   深度通道（若有）由 `EnsureDepthAttachmentLayout` 自己补布局往返。
}

// ============================================================
// 【§14.8 任务 26 / §14.34 末尾最小范围第 1 条】屏幕可视化的资源 / 录制 / 读回
//
// 【三件事】① 懒建一张 64×32 R32_UINT 小目标 + 读回缓冲 + PSO + 描述符集（只一次）；
//   ② 每帧录两趟（清屏 + 按档位累加）并把目标拷进 host 可见缓冲；
//   ③ dump 帧把目标读回、打印一行统计（"不是黑屏"的判据）。
// 【门控】全部挂在 `debugView != 0` 上：默认档**一个资源都不建、一次派发都不录、一行都不打**。
// 【为什么不碰 GBuffer】可视化写的是模块自建目标（与任务 6 的 1×1 目标同一做法），
//   因此它对可见画面零影响 —— 它证明的是"可视化真的产出了非空画面"，而不是"改了画面"。
// ============================================================
bool NaniteRaster::EnsureDebugViewResources(u32 mode,
                                            rhi::IRHIBuffer* assetClusters,
                                            rhi::IRHIBuffer* spheres,
                                            rhi::IRHIBuffer* lodInfo,
                                            std::span<const u32> bvhDepths) {
    // 【默认关：一个字节都不建】档位为 0 直接返回（调用方本来也不会调，这里再兜一次）
    if (!m_Device || mode == kNaniteDebugViewOff || mode > kNaniteDebugViewMaxMode) return false;
    // 四张只读输入表缺一不可（缺了就没有"非空洞"的可视化数据可用，宁可跳过也不画假图）
    if (!assetClusters || !spheres || !lodInfo || bvhDepths.empty()) return false;

    if (m_DebugViewPSO) {
        // 已经建好：只需在档位变化时刷新 push constant 的镜像（PSO/资源与档位无关，
        // 四种模式共用同一个 PSO 与同一张目标 —— 模式只决定累加时的编码）
        return true;
    }

    // ── 1. 模块自建的 64×32 R32_UINT 小目标（8 KB）──
    {
        rhi::TextureDesc td;
        td.width  = kNaniteDebugViewWidth;
        td.height = kNaniteDebugViewHeight;
        td.format = rhi::Format::R32_UINT;   // 整数目标：原子累加/取最大值直接写计数，不做浮点量化
        td.usage  = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::TransferSrc
                  | rhi::TextureUsage::ShaderResource;   // 见头文件对 ShaderResource 的说明
        m_DebugTarget = m_Device->CreateTexture(td);
        if (!m_DebugTarget) {
            HE_CORE_ERROR("NaniteRaster: 可视化目标创建失败（{}×{} R32_UINT）",
                          kNaniteDebugViewWidth, kNaniteDebugViewHeight);
            return false;
        }
    }

    // ── 2. 目标 → host 的读回缓冲（紧凑行距：宽 × 高 × 4B）──
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32) * kNaniteDebugViewPixels;
        d.usage     = rhi::BufferUsage::Storage;   // Storage 路径恒定带 TRANSFER_DST（拷贝目标）
        d.cpuAccess = true;
        m_DebugReadback = m_Device->CreateBuffer(d);
        if (!m_DebugReadback) { HE_CORE_ERROR("NaniteRaster: 可视化读回缓冲创建失败"); return false; }
    }

    // ── 3. 描述符集布局：与 `Nanite_DebugView.comp.slang` 的绑定逐条对应 ──
    {
        const u32 kStage = rhi::kStageMaskCompute;
        rhi::DescriptorSetLayoutDesc layout;
        layout.bindings = {
            { 0, rhi::DescriptorType::StorageBuffer, 1, kStage, false },  // 可见簇引用
            { 1, rhi::DescriptorType::StorageBuffer, 1, kStage, false },  // 可见簇计数
            { 2, rhi::DescriptorType::StorageBuffer, 1, kStage, false },  // 实例表（128B 契约）
            { 3, rhi::DescriptorType::StorageBuffer, 1, kStage, false },  // 簇包围球
            { 4, rhi::DescriptorType::StorageBuffer, 1, kStage, false },  // LOD 元数据
            { 5, rhi::DescriptorType::StorageBuffer, 1, kStage, false },  // 每簇 BVH 深度
            { 6, rhi::DescriptorType::StorageBuffer, 1, kStage, false },  // 资产簇记录（三角形数）
            { 7, rhi::DescriptorType::StorageImage,  1, kStage, false },  // 可视化目标（64×32 R32_UINT）
        };
        m_DebugLayout = m_Device->CreateDescriptorSetLayout(layout);
        if (m_DebugLayout == rhi::kInvalidLayout) {
            HE_CORE_ERROR("NaniteRaster: 可视化描述符集布局创建失败");
            return false;
        }
        m_DebugSet = m_Device->AllocateDescriptorSet(m_DebugLayout);
    }

    // ── 4. 一次性绑定"不变的"四项（目标 + 三张 CPU 一次性上传的只读表）──
    // 【为什么"不变量"只绑一次】引擎的 GPU 在**执行期**读描述符、最后一次主机写对整段命令缓冲
    //   生效（任务 15 的教训）⇒ 同一个集合每帧只应该写一次。可见簇引用/计数/实例表每帧都可能
    //   换缓冲（流式/重建），故它们在**录制期**写；这里写的是与资产同生共死的三张表与目标。
    m_Device->UpdateDescriptorSet(m_DebugSet, 3, rhi::DescriptorType::StorageBuffer, spheres);
    m_Device->UpdateDescriptorSet(m_DebugSet, 4, rhi::DescriptorType::StorageBuffer, lodInfo);
    m_Device->UpdateDescriptorSetWithImageView(m_DebugSet, 7, rhi::DescriptorType::StorageImage,
                                               m_DebugTarget->GetNativeHandle());

    // ── 5. compute PSO（push constant = 96B 的 `NaniteDebugViewParams`）──
    // 【每簇 BVH 深度表】它是 CPU 侧数组，需要一个 GPU 缓冲承载 ⇒ 模块自建一个小缓冲并在这里
    //   一次性上传（长度 = 参与 BVH 的簇数，实测 8287 条 = 33 KB；默认档不建）。
    {
        rhi::BufferDesc d;
        d.size      = sizeof(u32) * bvhDepths.size();
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;
        m_DebugDepths = m_Device->CreateBuffer(d);
        if (!m_DebugDepths) { HE_CORE_ERROR("NaniteRaster: 可视化 BVH 深度表创建失败"); return false; }
        if (void* p = m_DebugDepths->Map()) {
            std::memcpy(p, bvhDepths.data(), d.size);
            m_DebugDepths->Unmap();
        }
        m_Device->UpdateDescriptorSet(m_DebugSet, 5, rhi::DescriptorType::StorageBuffer,
                                      m_DebugDepths.get());
    }
    m_Device->UpdateDescriptorSet(m_DebugSet, 6, rhi::DescriptorType::StorageBuffer, assetClusters);

    m_DebugCS.stage      = rhi::ShaderStage::Compute;
    m_DebugCS.spirv      = k_Nanite_DebugView_comp_spv;
    m_DebugCS.entryPoint = "main";

    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskCompute;
    pc.size      = sizeof(NaniteDebugViewParams);

    rhi::PipelineStateDesc desc;
    desc.computeShader        = &m_DebugCS;
    desc.bindPoint            = rhi::PipelineBindPoint::Compute;
    desc.pushConstantRanges   = { pc };
    desc.descriptorSetLayouts = { m_DebugLayout };
    desc.debugName            = "NaniteDebugView";
    m_DebugViewPSO = m_Device->CreatePipelineState(desc);
    if (!m_DebugViewPSO) {
        HE_CORE_ERROR("NaniteRaster: 可视化 compute PSO 创建失败");
        return false;
    }

    HE_CORE_INFO("NaniteRaster: 任务 26 屏幕可视化就绪（{}×{} R32_UINT 小目标 = 两个 {}×{} 面板；"
                 "四种模式：1=可见簇数 2=软硬光栅占比 3=LOD 层级 4=BVH 深度；"
                 "**不碰任何 GBuffer**，dump 帧读回并打印统计）",
                 kNaniteDebugViewWidth, kNaniteDebugViewHeight,
                 kNaniteDebugViewPanel, kNaniteDebugViewPanel);
    return true;
}

void NaniteRaster::RecordDebugViewPass(rhi::IRHICommandList* cmd,
                                       rhi::IRHIBuffer* visibleRefs,
                                       rhi::IRHIBuffer* visibleCount,
                                       rhi::IRHIBuffer* instances,
                                       u32 visibleCapacity,
                                       const NaniteDebugViewParams& params) {
    if (!cmd || !m_DebugViewPSO || !visibleRefs || !visibleCount || !instances) return;
    if (params.mode == kNaniteDebugViewOff || params.mode > kNaniteDebugViewMaxMode) return;
    m_DebugLastMode = params.mode;
    m_DebugLastVisibleCapacity = visibleCapacity;

    // 【每帧重写易变项】可见簇列表/计数与实例表都可能换缓冲（资产重建/流式），
    //   每个集合每帧只写一次（同一集合、两次派发只用不同的 push constant ⇒ 无那个陷阱）。
    m_Device->UpdateDescriptorSet(m_DebugSet, 0, rhi::DescriptorType::StorageBuffer, visibleRefs);
    m_Device->UpdateDescriptorSet(m_DebugSet, 1, rhi::DescriptorType::StorageBuffer, visibleCount);
    m_Device->UpdateDescriptorSet(m_DebugSet, 2, rhi::DescriptorType::StorageBuffer, instances);

    // ── ① 目标布局：可采样/上一帧的状态 → 存储图像（UAV）──
    // 【为什么 from 取 `Undefined`】本入口每次都会先把整张目标清 0（清屏趟），
    //   丢弃旧内容是语义精确的；而纹理刚建好时布局追踪器也没有记录（与清屏通道同一手法，
    //   那里正是为了消掉启动期的那 7 条布局告警才从 RenderTarget 改成 Undefined）。
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::Undefined, rhi::ResourceState::UnorderedAccess,
                         m_DebugTarget.get());

    // ── ② 清屏趟（mode == 0 的内部趟；见 shader 文件头的说明）──
    NaniteDebugViewParams clearParams = params;
    clearParams.mode = kNaniteDebugViewClearMode;
    cmd->SetPipeline(m_DebugViewPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DebugSet);
    cmd->SetPushConstants(0, (u32)sizeof(clearParams), &clearParams);
    cmd->SetDrawDebugLabel("Nanite_DebugView (clear target)");
    cmd->Dispatch((kNaniteDebugViewPixels + 63u) / 64u, 1u, 1u);

    // 清屏 → 累加：显式内存屏障（两者都是对**同一张目标**的原子/普通写，必须定序）
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess);

    // ── ③ 累加趟（一个线程 = 一条可见簇引用；按容量派发、由 shader 按可见计数早退）──
    cmd->SetPushConstants(0, (u32)sizeof(params), &params);
    cmd->SetDrawDebugLabel("Nanite_DebugView (cluster -> tile)");
    cmd->Dispatch((visibleCapacity + 63u) / 64u, 1u, 1u);

    // ── ④ 目标 → host 可见缓冲（dump 帧 Map 读回；`CopyTextureToBuffer` 自己管布局往返）──
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::Transfer,
                         rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySrc,
                         m_DebugTarget.get());
    cmd->CopyTextureToBuffer(m_DebugTarget.get(), m_DebugReadback.get(),
                             0, 0, kNaniteDebugViewWidth, kNaniteDebugViewHeight, 0);
}

void NaniteRaster::LogDebugViewReadback(u32 visible) {
    // 【门控】档位为 0（默认）或资源没建 ⇒ 直接返回：不 Map、不打一个字符
    if (m_DebugLastMode == kNaniteDebugViewOff || !m_DebugReadback) return;

    u32 px[kNaniteDebugViewPixels] = {};
    if (void* p = m_DebugReadback->Map()) {
        std::memcpy(px, p, sizeof(u32) * kNaniteDebugViewPixels);
        m_DebugReadback->Unmap();
    }

    // ── 统计（"不是黑屏"的判据全在这里；两种面板分别统计）──
    u32 nonzero = 0u;
    // 【面板 A 的取值集合】只需容纳"面板 A 的格子数"（32×32 = 1024）个不同取值，
    //   所以按面板尺寸开数组而不是按整张目标（省一半栈）
    constexpr u32 kPanelCells = kNaniteDebugViewPanel * kNaniteDebugViewPanel;
    u32 distinctVals[kPanelCells] = {};
    u32 distinctCount = 0u;
    u64 sumA = 0u, sumB = 0u;
    u32 maxA = 0u, maxB = 0u;
    u32 nonzeroA = 0u, nonzeroB = 0u;
    for (u32 ty = 0u; ty < kNaniteDebugViewHeight; ++ty) {
        for (u32 tx = 0u; tx < kNaniteDebugViewPanel; ++tx) {
            const u32 a = px[ty * kNaniteDebugViewWidth + tx];
            const u32 b = px[ty * kNaniteDebugViewWidth + kNaniteDebugViewPanelBColumn + tx];
            if (a != 0u) { ++nonzero; ++nonzeroA; }
            if (b != 0u) { ++nonzero; ++nonzeroB; }
            sumA += (u64)a;
            sumB += (u64)b;
            if (a > maxA) maxA = a;
            if (b > maxB) maxB = b;
            // 面板 A 的"不同取值个数"：线性查重（1024 个像素，O(n²) 也只是一百万次比较）
            //   【为什么把 0 排除在外】0 在本图里表示"这个 tile 上没有簇"、是**空值**而不是一个取值。
            //   把 0 计进去会让 `distinct_vals` 恒 ≥ 1 —— 一张**全黑**的面板也会报 1，
            //   于是它作为"不是黑屏"的非空转信号就失效了（正是 §14.34 第 2 行要防的"恒真读数"）。
            //   排除 0 之后：全黑面板 ⇒ `distinct_vals=0`，与 `px_nonzero`/`nonzeroA` 三个信号同向。
            bool seen = false;
            for (u32 i = 0u; i < distinctCount; ++i) {
                if (distinctVals[i] == a) { seen = true; break; }
            }
            if (a != 0u && !seen && distinctCount < kPanelCells) distinctVals[distinctCount++] = a;
        }
    }
    const u32 totalCells = kNaniteDebugViewPixels;
    const u32 nonzeroPermille = (totalCells > 0u) ? (u32)((u64)nonzero * 1000u / totalCells) : 0u;

    // 【模式独有的量】把"这张图到底编码了什么"落到几个可核对数字上：
    //   · `tiled` = 落在某个 tile 上的可见簇数（模式 1 = 面板 A 之和；模式 2 = A + B；
    //     模式 3/4 = 面板 B 之和）。`visible - tiled` 就是"球心投影到屏幕外"的簇数。
    //   · 模式 2：软/硬簇数与按簇数的硬占比（分母 = 两侧之和；与 `hard_raster` 行的
    //     **按像素**占比口径不同，两者都打印、不互相冒充）。
    //   · 模式 3：平均 LOD = A 之和 / B 之和（定点 ×1000）；模式 4：最大 BVH 深度 = maxA。
    //     ⚠ 这两个量**只在各自的模式里填真值**，其余模式打印 0（理由见下）。
    //
    // 【口径纪律：**字段名不许说谎**】`tiled` 之所以能跨模式共用一个名字，是因为它在四个模式里
    //   **语义始终一样**（"落在 tile 上的可见簇数"，只是取的加法项随编码位置而变）。
    //   而 `maxA` / `sumA / sumB` 的**语义是随模式变的**：模式 1 的 `maxA = 701` 是**簇数**，
    //   模式 4 的 `maxA = 15` 才是**深度**。若把 maxA 无条件冒充成 `max_bvh_depth`，
    //   按字段名 grep 跨档就会读到"701 与 15 两个深度"——那正是 §14.34 第 2 行
    //   （读数说谎/掩盖缺陷）要防的事。⇒ 只有**定义它的那个模式**才填真值，
    //   其余模式一律填 0，与同行的 `hard_share_clusters_permille` 采用**完全相同的口径**
    //   （「本模式不适用」 = 0，而不是「随便报一个看着像的数」）。
    const u64 tiled = (m_DebugLastMode == kNaniteDebugViewRasterShare) ? (sumA + sumB)
                    : (m_DebugLastMode == kNaniteDebugViewVisibleClusters) ? sumA
                                                                          : sumB;
    const u64 offscreen = ((u64)visible > tiled) ? ((u64)visible - tiled) : 0u;
    const u32 hardSharePermille = (sumA + sumB > 0u)
        ? (u32)((m_DebugLastMode == kNaniteDebugViewRasterShare ? sumB : 0u) * 1000u / (sumA + sumB))
        : 0u;
    // 平均 LOD 只在模式 3 有意义（模式 3 的面板 A = LOD 之和、面板 B = 簇计数）
    const u32 meanLodMilli =
        (m_DebugLastMode == kNaniteDebugViewLodLevel && sumB > 0u)
            ? (u32)(sumA * 1000u / sumB) : 0u;
    // 最大 BVH 深度只在模式 4 有意义（该模式的面板 A 才编码 BVH 深度）
    const u32 maxBvhDepth =
        (m_DebugLastMode == kNaniteDebugViewBvhDepth) ? maxA : 0u;

    // 【恰好一行】任务 26 的可视化出口。字段名与其它读数行刻意不重名（便于按字段名 grep）。
    HE_CORE_INFO("[Nanite] debug_view mode={} name={} target={}x{} panels={} tiles={} "
                 "px_nonzero={} px_nonzero_permille={} distinct_vals={} "
                 "panelA=[sum={} max={} nonzero={}] panelB=[sum={} max={} nonzero={}] "
                 "tiled={} visible={} offscreen={} hard_share_clusters_permille={} mean_lod_milli={} "
                 "max_bvh_depth={} visible_capacity={}",
                 m_DebugLastMode, NaniteDebugViewModeName(m_DebugLastMode),
                 kNaniteDebugViewWidth, kNaniteDebugViewHeight, 2u,
                 kNaniteDebugViewPanel * kNaniteDebugViewPanel,
                 (unsigned long long)nonzero, nonzeroPermille, distinctCount,
                 (unsigned long long)sumA, maxA, nonzeroA,
                 (unsigned long long)sumB, maxB, nonzeroB,
                 (unsigned long long)tiled, visible, (unsigned long long)offscreen,
                 hardSharePermille, meanLodMilli, maxBvhDepth,
                 m_DebugLastVisibleCapacity);
}

void NaniteRaster::ReadbackSoftStats(u32 (&out)[kNaniteSoftStatsCapacity]) {    // 【清零语义】缓冲不存在（软光栅未就绪）时全部写 0：调用方不必先自己清，
    //   也不会读到未初始化的栈内存（`size_dist` / `perf` 行在未就绪档下会打印全 0 而不是垃圾）。
    for (u32 i = 0u; i < kNaniteSoftStatsCapacity; ++i) out[i] = 0u;
    if (!m_SoftStats) return;
    // 真实 GPU 读回（与其它读数同一约定：只 Map、不等待；调用方已 WaitIdle）
    if (void* p = m_SoftStats->Map()) {
        std::memcpy(out, p, sizeof(u32) * kNaniteSoftStatsCapacity);
        m_SoftStats->Unmap();
    }
}

void NaniteRaster::ReadbackHardStats(u32 (&out)[kNaniteHardStatsCapacity]) {
    // 与 `ReadbackSoftStats` 同款：缓冲不存在（`hardRaster=0` 或设备不支持）⇒ 全 0
    for (u32 i = 0u; i < kNaniteHardStatsCapacity; ++i) out[i] = 0u;
    if (!m_HardStats) return;
    if (void* p = m_HardStats->Map()) {
        std::memcpy(out, p, sizeof(u32) * kNaniteHardStatsCapacity);
        m_HardStats->Unmap();
    }
}

void NaniteRaster::LogHardRasterReadback() {
    // 【为什么保留这条早退】任务 22 的读数行只在"硬光栅资源真的建起来了"时打印
    //   （关闭档 / 设备不支持时一行都不打，日志与基线逐字一致）。
    //   而 `ReadbackHardStats` 本身对"缓冲不存在"是**清零**语义 —— 那是给任务 23 的 `perf` 行
    //   用的（基线档要打印 `hard_clusters=0` 而不是缺字段），两者的口径不要混。
    if (!m_HardStats) return;
    u32 hs[kNaniteHardStatsCapacity] = {};
    ReadbackHardStats(hs);
    // 软光栅侧的两个对照量（同一个缓冲、同一帧的读数）：用来算"软/硬占比"。
    // 【为什么占比按**像素**算而不是按簇数】簇数只说明"分流判据生效了"，而"谁来画屏幕"
    //   是像素量的对比；两者都打印出来，读的人不必自己猜口径。
    u32 ss[kNaniteSoftStatsCapacity] = {};
    ReadbackSoftStats(ss);
    const u32 hardPixels = hs[kNaniteHardStatPixels];
    const u32 softPixels = ss[kNaniteSoftStatPixels];
    const u64 totalPixels = (u64)hardPixels + (u64)softPixels;
    const u32 hardPermille = (totalPixels > 0u) ? (u32)((u64)hardPixels * 1000u / totalPixels) : 0u;
    const u32 softPermille = (totalPixels > 0u) ? (u32)((u64)softPixels * 1000u / totalPixels) : 0u;

    // 【恰好一行】任务 22 的验收出口：
    //   · `clusters` 应当与软光栅行的 `skipped_big` 相等（同一条分流判据的两侧计数）；
    //   · `soft_clusters` 应当仍是软光栅的 `soft`（小簇不该被硬光栅抢走）；
    //   · `hard_share_permille / soft_share_permille` 就是"软硬占比"（按像素、千分比）。
    //
    // ── 【§14.8 任务 26 / §14.34 表格第 10 行】**追加** push constant 回读字段 ──
    // 【为什么追加到本行而不另起一行】本行已经是"硬光栅这一趟到底发生了什么"的出口；
    //   回读回答的是同一类问题（这一趟的输入对不对），追加字段对任何按行或按字段名解析的
    //   既有脚本都是纯增量（既有字段名一个都没改，与任务 24/26-B 的同一做法）。
    // 【口径】`diag_*` 是 **mesh shader 实际收到**并原样写回的值；`diag_cpu_*` 是 CPU 侧
    //   **真正推下去**的那一份（`paramsEff`）对应的真值 —— 两组逐项相等才说明"没有漏阶段、
    //   没有错位"。`diag_match` = 逐项相等的判定（1 = 七项全等）。
    //   `diag_extent_milli` 是浮点字段的定点回读（×1000），用来证明浮点字段没被读成 0。
    const u32 diagScreenW = hs[kNaniteHardStatDiagScreenW];
    const u32 diagScreenH = hs[kNaniteHardStatDiagScreenH];
    const u32 diagMaxTri  = hs[kNaniteHardStatDiagMaxTri];
    const u32 diagExtent  = hs[kNaniteHardStatDiagExtent];
    const u32 diagInst    = hs[kNaniteHardStatDiagInstances];
    const u32 diagMats    = hs[kNaniteHardStatDiagMaterials];
    const u32 diagPages   = hs[kNaniteHardStatDiagPages];
    const bool diagMatch =
        (diagScreenW == m_HardLastScreenW) && (diagScreenH == m_HardLastScreenH)
     && (diagMaxTri  == m_HardLastMaxTriangles) && (diagExtent == m_HardLastExtentMilli)
     && (diagInst    == m_HardLastInstanceCount) && (diagMats == m_HardLastMaterialCount)
     && (diagPages   == m_HardLastPagesEnabled);

    HE_CORE_INFO("[Nanite] hard_raster clusters={} prims={} pixels={} fallback_pixels={} "
                 "soft_clusters={} soft_pixels={} skipped_big={} hard_share_permille={} "
                 "soft_share_permille={} max_triangles={} visible_capacity={} mesh_supported={} pso={} "
                 "diag_screenw={} diag_screenh={} diag_maxtri={} diag_extent_milli={} "
                 "diag_instances={} diag_materials={} diag_pages={} diag_match={} "
                 "diag_cpu=[{},{},{},{},{},{},{}]",
                 hs[kNaniteHardStatClusters],
                 hs[kNaniteHardStatPrimitives],
                 hardPixels,
                 hs[kNaniteHardStatFallbackPixels],
                 ss[kNaniteSoftStatRasterClusters],
                 softPixels,
                 ss[kNaniteSoftStatSkippedClusters],
                 hardPermille,
                 softPermille,
                 m_HardLastMaxTriangles,
                 m_HardLastVisibleCapacity,
                 m_HardRasterCapable ? 1 : 0,
                 m_HardRasterPSO ? "ok" : "fail",
                 diagScreenW, diagScreenH, diagMaxTri, diagExtent,
                 diagInst, diagMats, diagPages,
                 diagMatch ? 1u : 0u,
                 m_HardLastScreenW, m_HardLastScreenH, m_HardLastMaxTriangles, m_HardLastExtentMilli,
                 m_HardLastInstanceCount, m_HardLastMaterialCount, m_HardLastPagesEnabled);
}

void NaniteRaster::LogSoftRasterReadback() {
    // 真实 GPU 读回（与其它读数同一约定：只 Map、不等待；调用方已 WaitIdle）
    if (!m_SoftStats) return;
    u32 s[kNaniteSoftStatsCapacity] = {};
    ReadbackSoftStats(s);
    HE_CORE_INFO("[Nanite] soft_raster clusters={} soft={} skipped_big={} triangles={} pixels_written={} "
                 "degenerate={} neutral_material_pixels={} material_pixels={} fallback_pixels={} "
                 "materials={} distinct_materials={} textured_materials={} multi_mesh_clusters={} "
                 "depth_written={} "
                 "depth_storage_image_supported={} depth_src=key+SV_Depth max_triangles={} "
                 "instances={} depth_key_pixels={} covered_px={} diag_screenw={} diag_screenh={} "
                 "diag_maxtri={} diag_extent_milli={} tested_px={} "
                 // ── 【§14.8 任务 26 / §14.34 表格第 7 行】**追加**两个字段（既有字段名一个不改）──
                 //   `depth_key_ties` = 软光栅**深度键平局次数**（第 1 趟原子取最小值时，返回的旧值
                 //     与本次写入的键相等且非哨兵的次数 —— 即"这个像素上已有一个键完全相同的三角形
                 //     先到"，真实 GPU 原子计数）。
                 //   `ties_eq_diff`  = 它与"差额口径" `pixels_written - depth_written` 是否相等的
                 //     判定（1 = 相等）。**这是信息性字段，不参与任何 PASS/FAIL**：平局计数发生在
                 //     写入**当时**（对照当时的最小值），而最小值会继续变小 ⇒ 恒有
                 //     `ties >= pixels_written - depth_written`，相等只发生在"最小键一旦写定就不再
                 //     被更小键取代"的档位（实测阈值 16 相等、阈值 64 偏大，见 `NaniteTypes.h`）。
                 //     两个数都原样打印，**不**用差额去覆盖实测值、也不为了相等而改计数逻辑。
                 "depth_key_ties={} ties_eq_diff={}",
                 s[kNaniteSoftStatRasterClusters] + s[kNaniteSoftStatSkippedClusters],
                 s[kNaniteSoftStatRasterClusters],
                 s[kNaniteSoftStatSkippedClusters],
                 s[kNaniteSoftStatTriangles],
                 s[kNaniteSoftStatPixels],
                 s[kNaniteSoftStatDegenerate],
                 s[kNaniteSoftStatNeutralPixels],
                 s[kNaniteSoftStatMaterialPixels],
                 s[kNaniteSoftStatFallbackPixels],
                 m_MaterialCount,
                 m_DistinctMaterials,
                 m_TexturedMaterials,
                 m_MultiMeshClusters,
                 // 【P0 修复（§14.30）】深度解析通道的**真实原子计数**（不再是恒 1 常量）：
                 //   全屏片元逐像素访问一次 ⇒ 语义是**去重后的像素数**，与 `pixels_written`
                 //   （通过等值复检的"簇×三角形×像素"写次数）不是同一个量，不变式是本项 ≤ 它。
                 //   旧实现硬编码 1，正是这个恒真读数掩盖了"深度一列都没写进去"的真 bug。
                 s[kNaniteSoftStatDepthResolvedPixels],
                 m_DepthStorageImageSupported ? 1 : 0,
                 m_SoftLastMaxTriangles,
                 m_SoftLastInstanceCount,
                 m_DepthKeyPixels,
                 s[6], s[7], s[8], s[9], s[10], s[11],
                 s[kNaniteSoftStatDepthKeyTies],
                 (s[kNaniteSoftStatPixels] >= s[kNaniteSoftStatDepthResolvedPixels]
                      && s[kNaniteSoftStatDepthKeyTies]
                         == (s[kNaniteSoftStatPixels] - s[kNaniteSoftStatDepthResolvedPixels]))
                     ? 1u : 0u);
}

} // namespace he::render
