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

        rhi::PipelineStateDesc desc;
        desc.vertexShader         = &m_DepthResolveVS;
        desc.pixelShader          = &m_DepthResolveFS;
        desc.topology             = rhi::PrimitiveTopology::TriangleList;
        desc.cullMode             = rhi::CullMode::None;   // 全屏大三角形只看覆盖，不挑绕序
        desc.depthTest            = false;                 // 深度值由 shader 决定 ⇒ 不做硬件比较
        desc.depthWrite           = true;                  // 但要**写**深度
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
    for (u32 i = 0; i < 8u; ++i) {
        cmd->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                             rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::RenderTarget,
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
    m_Device->UpdateDescriptorSet(m_SoftDepthSet, 0, rhi::DescriptorType::StorageBuffer, asset.clusters);
    m_Device->UpdateDescriptorSet(m_SoftDepthSet, 1, rhi::DescriptorType::StorageBuffer, asset.vertices);
    m_Device->UpdateDescriptorSet(m_SoftDepthSet, 2, rhi::DescriptorType::StorageBuffer, asset.indices);
    m_Device->UpdateDescriptorSet(m_SoftDepthSet, 3, rhi::DescriptorType::StorageBuffer, visibleRefs);
    m_Device->UpdateDescriptorSet(m_SoftDepthSet, 4, rhi::DescriptorType::StorageBuffer, visibleCount);
    m_Device->UpdateDescriptorSet(m_SoftDepthSet, 5, rhi::DescriptorType::StorageBuffer, instances);
    m_Device->UpdateDescriptorSet(m_SoftDepthSet, 6, rhi::DescriptorType::StorageBuffer, m_DepthKey.get());
    m_Device->UpdateDescriptorSet(m_SoftDepthSet, 7, rhi::DescriptorType::StorageBuffer, m_SoftStats.get());

    m_Device->UpdateDescriptorSet(m_SoftColorSet, 0, rhi::DescriptorType::StorageBuffer, asset.clusters);
    m_Device->UpdateDescriptorSet(m_SoftColorSet, 1, rhi::DescriptorType::StorageBuffer, asset.vertices);
    m_Device->UpdateDescriptorSet(m_SoftColorSet, 2, rhi::DescriptorType::StorageBuffer, asset.indices);
    m_Device->UpdateDescriptorSet(m_SoftColorSet, 3, rhi::DescriptorType::StorageBuffer, visibleRefs);
    m_Device->UpdateDescriptorSet(m_SoftColorSet, 4, rhi::DescriptorType::StorageBuffer, visibleCount);
    m_Device->UpdateDescriptorSet(m_SoftColorSet, 5, rhi::DescriptorType::StorageBuffer, instances);
    m_Device->UpdateDescriptorSet(m_SoftColorSet, 6, rhi::DescriptorType::StorageBuffer, m_DepthKey.get());
    m_Device->UpdateDescriptorSet(m_SoftColorSet, 7, rhi::DescriptorType::StorageBuffer, m_SoftStats.get());
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
    cmd->SetPushConstants(0, (u32)sizeof(NaniteSoftRasterParams), &params);
    cmd->SetDrawDebugLabel("Nanite_SoftRasterDepth (atomic depth key)");
    cmd->Dispatch(visibleCapacity, 1, 1);

    // ── ④ 第 1 趟 → 第 2 趟：深度键的 RAW（全局内存屏障）──
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess);

    cmd->SetPipeline(m_SoftColorPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_SoftColorSet);
    cmd->SetPushConstants(0, (u32)sizeof(NaniteSoftRasterParams), &params);
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
    cmd->BeginOffscreenPass(nullptr, targets.depth->GetNativeHandle(), w, h, &depthClear, false);
    cmd->SetViewport({ 0.0f, (float)h, (float)w, -(float)h, 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, w, h });
    cmd->SetDrawDebugLabel("Nanite_DepthResolve (key -> SV_Depth)");
    cmd->Draw(3);
    cmd->EndOffscreenPass();
}

void NaniteRaster::LogSoftRasterReadback() {
    // 真实 GPU 读回（与其它读数同一约定：只 Map、不等待；调用方已 WaitIdle）
    if (!m_SoftStats) return;
    u32 s[kNaniteSoftStatsCapacity] = {};
    if (void* p = m_SoftStats->Map()) {
        std::memcpy(s, p, sizeof(s));
        m_SoftStats->Unmap();
    }
    HE_CORE_INFO("[Nanite] soft_raster clusters={} soft={} skipped_big={} triangles={} pixels_written={} "
                 "degenerate={} neutral_material_pixels={} material_pixels={} fallback_pixels={} "
                 "materials={} distinct_materials={} textured_materials={} multi_mesh_clusters={} "
                 "depth_written={} "
                 "depth_storage_image_supported={} depth_src=key+SV_Depth max_triangles={} "
                 "instances={} depth_key_pixels={} covered_px={} diag_screenw={} diag_screenh={} "
                 "diag_maxtri={} diag_extent_milli={} tested_px={}",
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
                 1,        // 深度解析恒执行（全屏片元写 SV_Depth）⇒ depth_written 恒 1
                 m_DepthStorageImageSupported ? 1 : 0,
                 m_SoftLastMaxTriangles,
                 m_SoftLastInstanceCount,
                 m_DepthKeyPixels,
                 s[6], s[7], s[8], s[9], s[10], s[11]);
}

} // namespace he::render
