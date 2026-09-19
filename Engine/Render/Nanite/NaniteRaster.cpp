// ============================================================
// Nanite/NaniteRaster.cpp — 绘制端：消费「计数 → 间接绘制」链（§14.8 任务 3）
//
// 【本文件由 §14.8 任务 1 建立骨架，任务 3 填充 "绘制端"】
//   绘制 = `IRHICommandList::DrawIndexedIndirectCount`（Vulkan: vkCmdDrawIndexedIndirectCount），
//   实际条数由 `NaniteCull` 的计数缓冲决定 —— CPU 不读回、不参与条数决定。
//   目标 = 模块自建的 1×1 R8 小目标；片元每光栅化一个簇把计数原子加一。
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

    // ── 2. 占位顶点/索引缓冲 ──
    // 顶点着色器只吃 SV_VertexID/SV_InstanceID，不读任何属性；但 Vulkan 的
    // DrawIndexedIndirectCount 仍要求绑定索引/顶点缓冲（命令里的 firstIndex 会去索引它）。
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

        rhi::BufferDesc id;
        // 3 个 u32 索引（12 B ≥ 4 ⇒ SetIndexBuffer 选 UINT32，与命令里的 indexCount=3 匹配）
        id.size        = sizeof(u32) * kNaniteFakeClusterIndexCount;
        id.usage       = rhi::BufferUsage::Index;
        id.cpuAccess   = true;
        m_DummyIB = m_Device->CreateBuffer(id);
        if (!m_DummyIB) { HE_CORE_ERROR("NaniteRaster: 占位索引缓冲创建失败"); return false; }
        u32 idx[3] = { 0u, 1u, 2u };
        if (void* p = m_DummyIB->Map()) { std::memcpy(p, idx, sizeof(idx)); m_DummyIB->Unmap(); }
    }

    // ── 3. 片元描述符集：显式绑定"已光栅化簇计数"SSBO（binding 0）──
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

    m_Target.reset();
    m_DummyVB.reset();
    m_DummyIB.reset();

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

    m_LastMaxDrawCount = maxDrawCount;

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
    // 1×1 视口 + 剪裁：全屏三角形 ⇒ 每条间接命令恰好 1 个片元（= 一个被光栅化的簇）
    cmd->SetViewport({ 0.0f, (float)kNaniteRasterTargetSize,
                       (float)kNaniteRasterTargetSize, -(float)kNaniteRasterTargetSize, 0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, kNaniteRasterTargetSize, kNaniteRasterTargetSize });

    // 绘制端点：`DrawIndexedIndirectCount`。maxDrawCount 只是命令缓冲容量上限，
    // 真正画几条由 countBuffer 的值决定 —— 这正是任务 3 要验证的"计数驱动绘制"。
    cmd->SetVertexBuffer(m_DummyVB.get(), 0);
    cmd->SetIndexBuffer(m_DummyIB.get(), 0);
    cmd->SetDrawDebugLabel("Nanite_Raster (indirect count)");
    cmd->DrawIndexedIndirectCount(indirectCmdBuffer, 0, countBuffer, 0,
                                  maxDrawCount, (u32)sizeof(NaniteIndirectCommand));

    cmd->EndOffscreenPass();
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

} // namespace he::render
