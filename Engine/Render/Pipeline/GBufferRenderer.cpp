// ============================================================
// GBufferRenderer.cpp — GBuffer 纹理所有权 + 渲染实现
// 负责 5 MRT + Depth 纹理创建、PSO 创建、描述符集管理、
// RenderGraph 导入接口。内部委托给 IGBufferRenderer（CPU/GPU 策略）。
// ============================================================
#include "Pipeline/GBufferRenderer.h"
#include <iterator>
#include "Pipeline/GBufferRenderer_CPU.h"
#include "Pipeline/GBufferRenderer_GPU.h"
#include "Pipeline/GBufferMeshMode.h"   // 【P9】HE_GBUFFER_MESH 开关（一次性读取）
#include "Scene/MeshComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cstdlib>

// SPIR-V 嵌入头文件
#include "GBuffer.vert.spv.h"
#include "GBuffer.vert.dxil.h"
#include "GBuffer.frag.spv.h"
#include "GBuffer.frag.dxil.h"
// 【P9】网格着色器版 GBuffer（DXIL 由 Engine/Shader/CMakeLists.txt 的 ms_6_6 分支生成）
#include "GBuffer.mesh.spv.h"
#include "GBuffer.mesh.dxil.h"

namespace he::render {

// ============================================================
// 【P9】`GBuffer.mesh.slang` 的 GBufferMeshVertex 与 C++ `StaticVertex` 必须逐字节一致
//
// 网格着色器是按"绝对索引 + 固定步长"从合并顶点缓冲里直接 Load 的（没有输入装配阶段，
// 也没有属性偏移表可用），所以结构体布局一旦漂移就会读出垃圾坐标 ⇒ 画面撕裂。
// 这里用编译期断言把布局钉死（Math.h 打开了 GLM_FORCE_DEFAULT_ALIGNED_GENTYPES：
// vec3 = 16B 对齐、vec2 = 8B 对齐 ⇒ 偏移 0/16/32、总大小 48B）。
// ============================================================
static_assert(sizeof(he::StaticVertex) == 48, "GBuffer.mesh 的顶点布局按 48 字节硬编码，改 StaticVertex 必须同步改 shader");
static_assert(offsetof(he::StaticVertex, position) == 0,  "GBuffer.mesh 假设 position 在偏移 0");
static_assert(offsetof(he::StaticVertex, normal)   == 16, "GBuffer.mesh 假设 normal 在偏移 16");
static_assert(offsetof(he::StaticVertex, uv)       == 32, "GBuffer.mesh 假设 uv 在偏移 32");

bool GBufferRenderer::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Width  = width;
    m_Height = height;

    // ── 1. 创建 GBuffer 纹理 ──
    CreateTextures(device);

    // ── 2. 创建 GBuffer PSO ──
    CreatePSO(device);

    // ── 3. 创建描述符集 ──
    CreateDescriptorSet(device);

    // ── 4. 填充 GBufferContext ──
    m_Ctx.device    = device;
    m_Ctx.width     = width;
    m_Ctx.height    = height;
    m_Ctx.gbA       = ResT(m_A);
    m_Ctx.gbB       = ResT(m_B);
    m_Ctx.gbC       = ResT(m_C);
    m_Ctx.gbVel     = ResT(m_D);
    m_Ctx.gbDepth   = ResT(m_Depth);
    m_Ctx.gbWorldPos = ResT(m_E);
    m_Ctx.gbDisneyA = ResT(m_F);
    m_Ctx.gbDisneyB = ResT(m_G);
    m_Ctx.gbLightmapKey = ResT(m_H);
    m_Ctx.pso       = ResP(m_PSO);
    m_Ctx.meshPso   = ResP(m_MeshPSO);   // 【P9】为空时 GPU 路径仍走原来的 IA 绘制
    m_Ctx.descSet   = m_Set;

    // ── 5. 创建策略模式渲染器 ──
    SetMode(m_Mode);

    HE_CORE_INFO("GBufferRenderer: 初始化完成 ({}x{}, mode={})",
                 width, height, m_Mode == Mode::CPU ? "CPU" : "GPU");
    return true;
}

void GBufferRenderer::Shutdown() {
    // 先关闭渲染器（使用 GBufferContext 的裸指针）
    if (m_Renderer) {
        m_Renderer->Shutdown();
        m_Renderer.reset();
    }

    // 销毁描述符集
    if (m_Set != rhi::kInvalidSet && m_Ctx.device) {
        // DescriptorSet 由设备管理，随设备销毁自动释放
    }
    if (m_Layout != rhi::kInvalidLayout && m_Ctx.device) {
        m_Ctx.device->DestroyDescriptorSetLayout(m_Layout);
        m_Layout = rhi::kInvalidLayout;
    }

    // 销毁 PSO + 纹理
    m_Ctx.device->DestroyPipelineStateHandle(m_MeshPSO); m_MeshPSO = {};   // 【P9】网格版 GBuffer
    m_Ctx.device->DestroyPipelineStateHandle(m_PSO); m_PSO = {};
    m_Ctx.device->DestroyTextureHandle(m_Depth); m_Depth = {};
    m_Ctx.device->DestroyTextureHandle(m_H); m_H = {};
    m_Ctx.device->DestroyTextureHandle(m_G); m_G = {};
    m_Ctx.device->DestroyTextureHandle(m_F); m_F = {};
    m_Ctx.device->DestroyTextureHandle(m_E); m_E = {};
    m_Ctx.device->DestroyTextureHandle(m_D); m_D = {};
    m_Ctx.device->DestroyTextureHandle(m_C); m_C = {};
    m_Ctx.device->DestroyTextureHandle(m_B); m_B = {};
    m_Ctx.device->DestroyTextureHandle(m_A); m_A = {};

    m_Ctx.device = nullptr;
    m_Width = m_Height = 0;
}

void GBufferRenderer::OnResize(u32 width, u32 height) {
    if (width == m_Width && height == m_Height) return;
    m_Width  = width;
    m_Height = height;

    // 重建所有纹理（旧纹理通过 unique_ptr 自动释放）
    CreateTextures(m_Ctx.device);

    // 更新 GBufferContext 中的裸指针
    m_Ctx.width     = width;
    m_Ctx.height    = height;
    m_Ctx.gbA       = ResT(m_A);
    m_Ctx.gbB       = ResT(m_B);
    m_Ctx.gbC       = ResT(m_C);
    m_Ctx.gbVel     = ResT(m_D);
    m_Ctx.gbDepth   = ResT(m_Depth);
    m_Ctx.gbWorldPos = ResT(m_E);
    m_Ctx.gbDisneyA = ResT(m_F);
    m_Ctx.gbDisneyB = ResT(m_G);
    m_Ctx.gbLightmapKey = ResT(m_H);
}

GBufferRenderer::Handles GBufferRenderer::ImportToRenderGraph(RenderGraph& rg) {
    Handles h;
    h.albedo   = rg.ImportTexture("GB_A",        ResT(m_A));
    h.normal   = rg.ImportTexture("GB_B",        ResT(m_B));
    h.emissive  = rg.ImportTexture("GB_C",        ResT(m_C));
    h.velocity = rg.ImportTexture("GB_Vel",      ResT(m_D));
    h.worldPos = rg.ImportTexture("GB_WorldPos", ResT(m_E));
    h.disneyA  = rg.ImportTexture("GB_DisneyA",  ResT(m_F));
    h.disneyB  = rg.ImportTexture("GB_DisneyB",  ResT(m_G));
    h.lightmapKey = rg.ImportTexture("GB_LightmapKey", ResT(m_H));
    h.depth    = rg.ImportTexture("GB_Depth",    ResT(m_Depth));
    return h;
}

void GBufferRenderer::Render(rhi::IRHICommandList* cmd, const FrameSceneSnapshot& snapshot,
                              const CameraData& camera) {
    if (m_Renderer) {
        m_Renderer->Render(cmd, m_Ctx, snapshot, camera);
    }
}

void GBufferRenderer::SetMode(Mode mode) {
    m_Mode = mode;
    if (m_Renderer) m_Renderer->Shutdown();

    if (mode == Mode::GPU)
        m_Renderer = std::make_unique<GBufferRenderer_GPU>();
    else
        m_Renderer = std::make_unique<GBufferRenderer_CPU>();

    // 如果 context 已初始化（device 不为空），则初始化渲染器
    if (m_Ctx.device) {
        m_Renderer->Initialize(m_Ctx);
    }
}

// ============================================================
// 内部实现
// ============================================================

void GBufferRenderer::CreateTextures(rhi::IRHIDevice* device) {
    // GBuffer A/B/C/E/F/G/H: RGBA16_FLOAT（RenderTarget + ShaderResource + UnorderedAccess）
    // 【§14.8 任务 4（A1 落地）】这里只**增加** `UnorderedAccess`（UAV）标志：
    //   模块的 compute 通道要直接写既有 GBuffer（A1 路线），而 RHI 的纹理在创建时就固定了
    //   `VK_IMAGE_USAGE_STORAGE_BIT`；宽高 / 格式 / 其它 usage 一律不改 —— 任务的硬验收是
    //   "既有路径画面逐位不变"，只增标志不改变任何既有渲染语义（渲染目标用法与布局转换都不变）。
    //   本机存储图像格式支持实测见 §14.8 任务 4 的 A1 裁决依据（R16G16B16A16_SFLOAT 的
    //   optimalTiling 含 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT）。
    auto createRGBA16F = [&]() {
        rhi::TextureDesc d;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.width  = m_Width;
        d.height = m_Height;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource
                 | rhi::TextureUsage::UnorderedAccess;
        auto h = device->CreateTextureHandle(d);   // A-3
        HE_ASSERT(h.IsValid(), "GBufferRenderer: GBuffer 纹理句柄创建失败");
        return h;
    };
    m_A = createRGBA16F();  // Albedo.rgb + Metallic.a
    m_B = createRGBA16F();  // Normal.xyz + Roughness.a
    m_C = createRGBA16F();  // Emissive.rgb + AO.a
    m_E = createRGBA16F();  // WorldPos.xyz
    m_F = createRGBA16F();  // DisneyA（anisotropic/subsurface/specular/sheen）
    m_G = createRGBA16F();  // DisneyB（clearcoat/clearcoatGloss/specularTint.rg）
    m_H = createRGBA16F();  // 光照图键（uv0.xy + objectIndex，任务 31）

    // GBuffer D: velocity（RG16_FLOAT，屏幕空间运动矢量；同样只增 UAV 标志）
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::RG16_FLOAT;
        d.width  = m_Width;
        d.height = m_Height;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource
                 | rhi::TextureUsage::UnorderedAccess;
        m_D = device->CreateTextureHandle(d);   // A-3
    }

    // GBuffer Depth: D32_FLOAT（DepthStencil + ShaderResource）
    // 【§14.8 任务 4 的明确决定】**不给深度加 UAV**。深度是否可做存储图像是 A1 深度方案的
    //   裁决输入：本机 NVIDIA RTX 4060 的 D32_SFLOAT 支持 STORAGE_IMAGE，但同机 AMD 核显
    //   不支持（详见任务 4 答复与 §14.5 的裁决记录），故深度写入方案必须先裁决再接。
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::D32_FLOAT;
        d.width  = m_Width;
        d.height = m_Height;
        d.usage  = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
        m_Depth = device->CreateTextureHandle(d);   // A-3
    }
}

void GBufferRenderer::CreatePSO(rhi::IRHIDevice* device) {
    // 描述符集布局：set=0 = per-frame GPUObjectData[] + bindless 纹理/采样器数组
    // 【P9】另加两个网格着色器几何 SSBO（合并 VB/IB）：只有 `GBuffer.mesh.slang` 引用它们，
    // 顶点/片元着色器不引用 ⇒ 不开网格路径时不写这两个描述符也不影响既有绘制。
    const u32 kGeomStageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment | rhi::kStageMaskMesh;
    rhi::DescriptorSetLayoutDesc gbLayout;
    gbLayout.bindings = {
        {kGPUBinding_ObjectData, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskVertex | rhi::kStageMaskFragment},
        {kGPUBinding_BindlessTextures, rhi::DescriptorType::SampledImage, 4096, rhi::kStageMaskFragment, true},   // bindless 纹理
        {kGPUBinding_BindlessSamplers, rhi::DescriptorType::Sampler, 4096, rhi::kStageMaskFragment, true},         // bindless 采样器
        {kGPUBinding_BindlessSSBO, rhi::DescriptorType::StorageBuffer, 4096, rhi::kStageMaskVertex | rhi::kStageMaskFragment, true},  // u_SSBO[] bindless
        // 【P9】网格着色器的几何来源（MeshBatcher 的合并 VB/IB）。写描述符在 GPU 路径每帧做。
        {kGPUBinding_MeshVertices, rhi::DescriptorType::StorageBuffer, 1, kGeomStageMask},
        {kGPUBinding_MeshIndices,  rhi::DescriptorType::StorageBuffer, 1, kGeomStageMask},
    };
    m_Layout = device->CreateDescriptorSetLayout(gbLayout);

    // 着色器
    rhi::ShaderBytecode gbVS, gbFS;
    gbVS.stage = rhi::ShaderStage::Vertex;
    gbVS.spirv = k_GBuffer_vert_spv;
    gbVS.dxil.assign(std::begin(k_GBuffer_vert_dxil), std::end(k_GBuffer_vert_dxil));
    gbVS.dxil.assign(std::begin(k_GBuffer_vert_dxil), std::end(k_GBuffer_vert_dxil));
    gbVS.dxil.assign(std::begin(k_GBuffer_vert_dxil), std::end(k_GBuffer_vert_dxil));
    gbVS.entryPoint = "main";
    gbFS.stage = rhi::ShaderStage::Pixel;
    gbFS.spirv = k_GBuffer_frag_spv;
    gbFS.dxil.assign(std::begin(k_GBuffer_frag_dxil), std::end(k_GBuffer_frag_dxil));
    gbFS.dxil.assign(std::begin(k_GBuffer_frag_dxil), std::end(k_GBuffer_frag_dxil));
    gbFS.dxil.assign(std::begin(k_GBuffer_frag_dxil), std::end(k_GBuffer_frag_dxil));
    gbFS.entryPoint = "main";

    // 顶点布局
    rhi::VertexInputLayout vl;
    vl.stride = sizeof(he::StaticVertex);
    vl.attributes = {
        {0, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, position)},
        {1, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, normal)},
        {2, 0, rhi::VertexFormat::Float2, offsetof(he::StaticVertex, uv)},
    };

    // Push Constant
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment;
    pc.size = rhi::kMaxPushConstantSize;

    // PSO 描述
    rhi::PipelineStateDesc gbDesc;
    // DGC 会把本管线当作执行集的 initialPipeline，那种用法要求管线创建时带
    // VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT（否则
    // VUID-VkIndirectExecutionSetPipelineInfoEXT-initialPipeline-11153）。
    // 该位只是"允许被执行集引用"，不影响普通 vkCmdDraw*，故设备支持 DGC 时直接带上。
    gbDesc.indirectBindable = device->GetCaps().supportsDGC;
    gbDesc.vertexShader = &gbVS;
    gbDesc.pixelShader = &gbFS;
    gbDesc.vertexLayout = vl;
    gbDesc.depthTest  = true;
    gbDesc.depthWrite = true;
    gbDesc.depthFormat = rhi::Format::D32_FLOAT;
    gbDesc.colorAttachmentCount = kGBufferAttachmentCount;
    gbDesc.colorFormats[0] = rhi::Format::RGBA16_FLOAT;  // Albedo+Metallic
    gbDesc.colorFormats[1] = rhi::Format::RGBA16_FLOAT;  // Normal+Roughness
    gbDesc.colorFormats[2] = rhi::Format::RGBA16_FLOAT;  // Emissive+AO
    gbDesc.colorFormats[3] = rhi::Format::RG16_FLOAT;    // Velocity
    gbDesc.colorFormats[4] = rhi::Format::RGBA16_FLOAT;  // WorldPos
    gbDesc.colorFormats[5] = rhi::Format::RGBA16_FLOAT;  // DisneyA（anisotropic/subsurface/specular/sheen）
    gbDesc.colorFormats[6] = rhi::Format::RGBA16_FLOAT;  // DisneyB（clearcoat/clearcoatGloss/specularTint.rg）
    gbDesc.colorFormats[7] = rhi::Format::RGBA16_FLOAT;  // 光照图键（uv0.xy + objectIndex）
    gbDesc.pushConstantRanges = {pc};
    gbDesc.descriptorSetLayouts = {m_Layout};
    gbDesc.debugName = "GBuffer";

    m_PSO = device->CreatePipelineStateHandle(gbDesc);   // A-3
    HE_ASSERT(m_PSO.IsValid(), "GBufferRenderer: PSO 句柄创建失败");

    // ============================================================
    // 【P9】网格着色器版 GBuffer 管线（`HE_GBUFFER_MESH=1`）
    //
    // 与上面那条 PSO 的**唯一区别**是几何段：`meshShader` 非空 ⇒ 走网格管线
    //（无输入装配、无顶点缓冲）；片元着色器仍是同一个 `GBuffer.frag`，8 个颜色附件 /
    // 深度 / push constant / 描述符集布局全部沿用，所以两条路径的像素结果可以直接对比。
    //
    // 【为什么门控在 D3D12】本项验收以 D3D12（DXIL / 调试层）为准；Vulkan 侧的
    // `GBuffer.mesh` SPIR-V 仍会编译，但驱动差异不在本次范围内 ⇒ 不在 Vulkan 上建这条 PSO
    //（避免在既有 Vulkan 路径上引入一条未验证的管线）。
    // ============================================================
    const bool wantMesh = GBufferMeshPathEnabled();
    if (!wantMesh) return;
    if (device->GetBackend() != rhi::Backend::D3D12) {
        HE_CORE_WARN("GBufferRenderer: HE_GBUFFER_MESH=1 但当前后端不是 D3D12，跳过网格版 GBuffer 管线");
        return;
    }
    if (!device->GetCaps().supportsMeshShaders) {
        HE_CORE_WARN("GBufferRenderer: 设备不支持网格着色器，跳过网格版 GBuffer 管线");
        return;
    }

    rhi::ShaderBytecode gbMS;
    gbMS.stage      = rhi::ShaderStage::Mesh;
    gbMS.spirv      = k_GBuffer_mesh_spv;   // Vulkan 侧不使用，但字节码字段必须合法（空指针会崩）
    gbMS.dxil.assign(std::begin(k_GBuffer_mesh_dxil), std::end(k_GBuffer_mesh_dxil));
    gbMS.entryPoint = "main";

    rhi::PipelineStateDesc meshDesc = gbDesc;   // 深度/混合/目标格式/描述符布局逐项继承
    meshDesc.vertexShader   = nullptr;          // 网格管线没有 VS
    meshDesc.meshShader     = &gbMS;            // ← 走 InitializeMeshPso（管线状态流）
    meshDesc.vertexLayout   = {};               // 无输入装配
    // push constant 必须覆盖 **Mesh** 阶段：D3D12 的根签名可见性是 ALL（不区分阶段），
    // Vulkan 侧 PipelinePipeline 会自动补 Mesh|Task 位（见 VulkanPipeline.cpp 的 mesh 分支）。
    meshDesc.pushConstantRanges = {pc};
    meshDesc.debugName      = "GBufferMesh";

    m_MeshPSO = device->CreatePipelineStateHandle(meshDesc);   // A-3
    if (!m_MeshPSO.IsValid()) {
        HE_CORE_ERROR("GBufferRenderer: 网格版 GBuffer 管线创建失败（HE_GBUFFER_MESH=1），本帧回退 IA 路径");
        return;
    }
    HE_CORE_INFO("GBufferRenderer: 网格版 GBuffer 管线就绪 name=GBufferMesh（{} 三角形/工作组）",
                 kGBufferMeshTrianglesPerGroup);
}

void GBufferRenderer::CreateDescriptorSet(rhi::IRHIDevice* device) {
    m_Set = device->AllocateDescriptorSet(m_Layout);

    // 创建 bindless 占位纹理和采样器（bindless 数组回退用）
    u8 white[4] = {255, 255, 255, 255};
    rhi::TextureDesc defDesc;
    defDesc.format = rhi::Format::RGBA8_UNORM;
    defDesc.width = 1;
    defDesc.height = 1;
    defDesc.mipLevels = 1;
    defDesc.arrayLayers = 1;
    defDesc.usage = rhi::TextureUsage::ShaderResource;
    defDesc.initialData = white;
    auto placeholderTex = device->CreateTexture(defDesc);

    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU = sd.addressV = rhi::AddressMode::Repeat;
    auto placeholderSamp = device->CreateSampler(sd);

    // 设置 bindless 堆默认纹理（占位纹理/采样器回退用）
    auto* heap = device->GetBindlessHeap();
    heap->SetDefaultTexture(placeholderTex.get(), placeholderSamp.get());

    // 预填充 bindless 绑定
    rhi::IRHITexture* texPtrs[]  = { placeholderTex.get() };
    rhi::IRHISampler*  sampPtrs[] = { placeholderSamp.get() };
    device->UpdateDescriptorSet(m_Set, rhi::kBindingBindlessTextures,
        rhi::DescriptorType::SampledImage, texPtrs, nullptr, 1);
    device->UpdateDescriptorSet(m_Set, rhi::kBindingBindlessSamplers,
        rhi::DescriptorType::Sampler, nullptr, sampPtrs, 1);

    // 注册到 bindless 堆（纹理加载后 Flush 自动推送）
    heap->RegisterDescriptorSet(m_Set, rhi::kBindingBindlessTextures,
        rhi::kBindingBindlessSamplers, rhi::kBindingBindlessSSBO);
}

} // namespace he::render
