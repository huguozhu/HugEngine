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
#include "Nanite_MeshTest.mesh.spv.h"    // k_Nanite_MeshTest_mesh_spv（§14.8 任务 6 的 mesh 通道）
#include "Nanite_MeshTest.frag.spv.h"    // k_Nanite_MeshTest_frag_spv

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

    // 【§14.8 任务 6】mesh PSO 自证通道的懒建资源（从未开启时它们是空的，这里自然是空操作）
    m_MeshTestPSO.reset();
    if (m_Device && m_MeshTestLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_MeshTestLayout);
    m_MeshTestLayout = rhi::kInvalidLayout;
    m_MeshTestSet    = rhi::kInvalidSet;
    m_MeshTestTarget.reset();
    m_MeshTestCount.reset();
    m_MeshTestReadback.reset();

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

} // namespace he::render
