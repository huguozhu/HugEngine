// ============================================================
// DecalPass.cpp — GBuffer 投影贴花 Pass 实现（任务 24）
// ============================================================

#include "Pipeline/DecalPass.h"
#include "Pipeline/GBufferRenderer.h"
#include "Pipeline/Camera.h"
#include "Scene/DecalComponent.h"
#include "Scene/SceneGraph.h"
#include "Scene/World.h"
#include "Core/Log.h"

#include "DecalProject.vert.spv.h"
#include "DecalProject.frag.spv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace he::render {

namespace {
// 自定义绑定号（避开 ShaderTypes 里已占用的 2/5/6/7/8/30）
constexpr u32 kDecalBindGBufferWorldPos = 20;   // GBuffer MRT4（世界坐标）
constexpr u32 kDecalBindGBufferDepth    = 21;   // GBuffer 深度（天空判定）
constexpr u32 kDecalBindPointSampler    = 22;   // 逐像素精确采样（不带插值）

/// 单位立方体（[-0.5, 0.5]^3）：24 顶点 / 36 索引。法线/UV 都不需要（UV 由世界坐标反算）
void BuildUnitCube(std::vector<float3>& outVerts, std::vector<u32>& outIndices) {
    const float h = 0.5f;
    // 6 个面 × 4 顶点（顶点顺序：面内逆时针）
    const float3 faceVerts[6][4] = {
        { {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h} },   // +Z
        { { h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h} },   // -Z
        { { h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h} },   // +X
        { {-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h} },   // -X
        { {-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h} },   // +Y
        { {-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h} },   // -Y
    };
    outVerts.clear();
    outIndices.clear();
    for (u32 f = 0; f < 6; ++f) {
        const u32 base = (u32)outVerts.size();
        for (u32 v = 0; v < 4; ++v) outVerts.push_back(faceVerts[f][v]);
        outIndices.push_back(base + 0); outIndices.push_back(base + 1); outIndices.push_back(base + 2);
        outIndices.push_back(base + 0); outIndices.push_back(base + 2); outIndices.push_back(base + 3);
    }
}

/// 取世界矩阵的旋转基（列向量归一化；退化列回退到单位轴，避免 0 缩放产生 NaN）
float3 NormalizedColumn(const float4x4& m, u32 axis) {
    float3 c(m[axis]);
    const float len = glm::length(c);
    if (len < 1e-6f) return float3(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
    return c / len;
}
} // namespace

bool DecalPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    if (!m_Device) return false;

    // ── 1. 单位立方体几何（盒子 = 贴花投影体积）──
    std::vector<float3> verts;
    std::vector<u32>    indices;
    BuildUnitCube(verts, indices);

    rhi::BufferDesc vbDesc;
    vbDesc.size        = verts.size() * sizeof(float3);
    vbDesc.usage       = rhi::BufferUsage::Vertex;
    vbDesc.initialData = verts.data();
    vbDesc.stride      = sizeof(float3);
    m_CubeVB = m_Device->CreateBuffer(vbDesc);

    rhi::BufferDesc ibDesc;
    ibDesc.size        = indices.size() * sizeof(u32);
    ibDesc.usage       = rhi::BufferUsage::Index;
    ibDesc.initialData = indices.data();
    ibDesc.stride      = sizeof(u32);
    m_CubeIB = m_Device->CreateBuffer(ibDesc);

    // ── 2. 点采样器（GBuffer 世界坐标/深度逐像素精确取值，不做插值）──
    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = sd.mipFilter = rhi::FilterMode::Nearest;
    sd.addressU = sd.addressV = sd.addressW = rhi::AddressMode::ClampToEdge;
    m_PointSampler = m_Device->CreateSampler(sd);

    // ── 3. 描述符集布局：bindless 纹理/采样器/SSBO（common.slang 需要）+ GBuffer worldPos/depth ──
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        { rhi::kBindingBindlessTextures, rhi::DescriptorType::SampledImage, 4096,
          rhi::kStageMaskFragment, true },
        { rhi::kBindingBindlessSamplers, rhi::DescriptorType::Sampler, 4096,
          rhi::kStageMaskFragment, true },
        { rhi::kBindingBindlessSSBO, rhi::DescriptorType::StorageBuffer, 4096,
          rhi::kStageMaskFragment, true },
        { kDecalBindGBufferWorldPos, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskFragment },
        { kDecalBindGBufferDepth,    rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskFragment },
        { kDecalBindPointSampler,    rhi::DescriptorType::Sampler,      1, rhi::kStageMaskFragment },
    };
    m_Layout = m_Device->CreateDescriptorSetLayout(layout);
    m_Set    = m_Device->AllocateDescriptorSet(m_Layout);

    // 注册到 bindless 堆：Flush 时把纹理/采样器/SSBO 数组一起推到本集（贴花纹理句柄与全局一致）
    m_Device->GetBindlessHeap()->RegisterDescriptorSet(
        m_Set, rhi::kBindingBindlessTextures, rhi::kBindingBindlessSamplers, rhi::kBindingBindlessSSBO);

    // ── 4. PSO：8 个 GBuffer 颜色附件（Load 保留 + 只写 MRT0/1）+ **无深度附件** ──
    // 无深度附件是刻意的：本 Pass 要把深度当纹理采样（天空判定），
    // 同一图像既做附件又做采样 = feedback loop（校验层会直接报错）。
    rhi::ShaderBytecode vs, fs;
    vs.stage = rhi::ShaderStage::Vertex; vs.spirv = k_DecalProject_vert_spv; vs.entryPoint = "main";
    fs.stage = rhi::ShaderStage::Pixel;  fs.spirv = k_DecalProject_frag_spv; fs.entryPoint = "main";

    rhi::VertexInputLayout vl;
    vl.stride = sizeof(float3);
    vl.attributes = { { 0, 0, rhi::VertexFormat::Float3, 0 } };

    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment;
    pc.size      = sizeof(DecalPushConstants);   // 240 字节 ≤ kMaxPushConstantSize(256)

    rhi::PipelineStateDesc desc;
    desc.vertexShader  = &vs;
    desc.pixelShader   = &fs;
    desc.vertexLayout  = vl;
    desc.topology      = rhi::PrimitiveTopology::TriangleList;
    desc.cullMode      = rhi::CullMode::None;     // 盒子双面都要（盒子可能被相机从内部看）
    desc.depthTest     = false;                   // 无深度附件：遮挡关系由世界坐标盒子裁剪决定
    desc.depthWrite    = false;
    desc.depthFormat   = rhi::Format::Unknown;    // 关键：不建深度附件
    desc.colorLoadOp   = rhi::LoadOp::Load;       // 关键：保留 GBuffer 已有内容
    desc.depthLoadOp   = rhi::LoadOp::Load;
    desc.colorAttachmentCount = kGBufferAttachmentCount;
    desc.colorFormats[0] = rhi::Format::RGBA16_FLOAT;   // Albedo+Metallic（写）
    desc.colorFormats[1] = rhi::Format::RGBA16_FLOAT;   // Normal+Roughness（写）
    desc.colorFormats[2] = rhi::Format::RGBA16_FLOAT;   // Emissive+AO（不写）
    desc.colorFormats[3] = rhi::Format::RG16_FLOAT;     // Velocity（不写）
    desc.colorFormats[4] = rhi::Format::RGBA16_FLOAT;   // WorldPos（不写）
    desc.colorFormats[5] = rhi::Format::RGBA16_FLOAT;   // DisneyA（不写）
    desc.colorFormats[6] = rhi::Format::RGBA16_FLOAT;   // DisneyB（不写）
    desc.colorFormats[7] = rhi::Format::RGBA16_FLOAT;   // 光照图键（不写）

    // 混合：dst = src * srcAlpha + dst * (1 - srcAlpha)（颜色与 alpha 通道同规则）
    // MRT2~7：writeMask = None —— 与 GBuffer 共用 8 附件但绝不改动其余通道
    for (u32 i = 0; i < kGBufferAttachmentCount; ++i) {
        rhi::ColorBlendDesc& b = desc.colorBlend[i];
        if (i <= 1) {
            b.blendEnable         = true;
            b.srcColorBlendFactor = rhi::BlendFactor::SrcAlpha;
            b.dstColorBlendFactor = rhi::BlendFactor::OneMinusSrcAlpha;
            b.colorBlendOp        = rhi::BlendOp::Add;
            b.srcAlphaBlendFactor = rhi::BlendFactor::SrcAlpha;
            b.dstAlphaBlendFactor = rhi::BlendFactor::OneMinusSrcAlpha;
            b.alphaBlendOp        = rhi::BlendOp::Add;
            b.writeMask           = rhi::ColorWriteMask::All;
        } else {
            b.blendEnable = false;
            b.writeMask   = rhi::ColorWriteMask::None;
        }
    }
    desc.pushConstantRanges    = { pc };
    desc.descriptorSetLayouts  = { m_Layout };
    desc.debugName             = "DecalProject";
    m_PSO = m_Device->CreatePipelineState(desc);
    if (!m_PSO) {
        HE_CORE_ERROR("DecalPass: PSO 创建失败");
        return false;
    }

    HE_CORE_INFO("DecalPass: 初始化完成（{}x{}，8 附件 Load-only + 无深度附件，只写 MRT0/1）",
                 width, height);
    return true;
}

void DecalPass::Shutdown() {
    m_PSO.reset();
    m_PointSampler.reset();
    m_CubeIB.reset();
    m_CubeVB.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_Layout);
    }
    m_Layout = rhi::kInvalidLayout;
    m_Set    = rhi::kInvalidSet;
    m_Device = nullptr;
    m_Width = m_Height = 0;
}

void DecalPass::OnResize(u32 width, u32 height) {
    m_Width  = width;
    m_Height = height;
    // 纹理与 PSO 都不依赖尺寸（屏幕尺寸走 push constant），无需重建
}

void DecalPass::Render(rhi::IRHICommandList* cmd, he::World& world, he::SceneGraph& sg,
                       const CameraData& camera, GBufferRenderer& gb) {
    m_LastDecalCount = 0;
    if (!cmd || !m_PSO || !m_CubeVB || !m_CubeIB || !m_Set) return;
    if (m_Width == 0 || m_Height == 0) return;

    // 贴花是否为空：空则完全不录 pass（避免每帧一个空 render pass）
    bool hasDecal = false;
    world.ForEach<he::DecalComponent>([&](he::Entity, he::DecalComponent& d) {
        if (d.opacity > 0.0f) hasDecal = true;
    });
    if (!hasDecal) return;

    // 1. 绑定 GBuffer 的世界坐标/深度（每帧写一次，尺寸变化后指针也会变）
    if (auto* device = m_Device) {
        device->UpdateDescriptorSet(m_Set, kDecalBindGBufferWorldPos,
            rhi::DescriptorType::CombinedImageSampler, gb.GetWorldPos(), m_PointSampler.get());
        device->UpdateDescriptorSet(m_Set, kDecalBindGBufferDepth,
            rhi::DescriptorType::CombinedImageSampler, gb.GetDepth(), m_PointSampler.get());
        device->UpdateDescriptorSet(m_Set, kDecalBindPointSampler,
            rhi::DescriptorType::Sampler, nullptr, m_PointSampler.get());
    }

    // 2. 起 render pass：SetPipeline 决定 render pass（Load 版本 + 8 附件 + 无深度）
    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);

    void* colorViews[kGBufferAttachmentCount] = {
        gb.GetAlbedo()->GetNativeHandle(),
        gb.GetNormal()->GetNativeHandle(),
        gb.GetEmissive()->GetNativeHandle(),
        gb.GetVelocity()->GetNativeHandle(),
        gb.GetWorldPos()->GetNativeHandle(),
        gb.GetDisneyA()->GetNativeHandle(),
        gb.GetDisneyB()->GetNativeHandle(),
        gb.GetLightmapKey()->GetNativeHandle(),
    };
    // 深度附件传 nullptr：深度是**采样**输入，不是本 Pass 的附件（见 Initialize 说明）
    cmd->BeginOffscreenPassMRT(colorViews, kGBufferAttachmentCount, nullptr,
                               m_Width, m_Height, nullptr, false);
    cmd->SetViewport({ 0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1 });
    cmd->SetScissor({ 0, 0, m_Width, m_Height });

    // 3. 逐贴花绘制
    const float4x4 viewProj = camera.GetViewProjMatrix();
    world.ForEach<he::DecalComponent>([&](he::Entity e, he::DecalComponent& d) {
        const float alpha = std::clamp(d.opacity, 0.0f, 1.0f);
        if (alpha <= 0.0f) return;
        if (d.size.x <= 0.0f || d.size.y <= 0.0f) return;

        const float4x4 wm = sg.GetWorldMatrix(e);

        // 旋转基：从世界矩阵取列并归一化（贴花尺寸来自组件自身，不乘 Transform 缩放）
        float3 r0 = NormalizedColumn(wm, 0);
        float3 r1 = NormalizedColumn(wm, 1);
        float3 r2 = NormalizedColumn(wm, 2);

        // 组件 rotation = 绕贴花轴（局部 +Z）自转；把它折进基：R' = R * Rz(rotation)
        const float c = std::cos(d.rotation), s = std::sin(d.rotation);
        float3 q0 = r0 * c + r1 * s;
        float3 q1 = r1 * c - r0 * s;
        r0 = q0; r1 = q1;

        DecalPushConstants pcs;
        pcs.viewProj    = viewProj;
        pcs.rotRow0     = float4(r0, 0.0f);
        pcs.rotRow1     = float4(r1, 0.0f);
        pcs.rotRow2     = float4(r2, 0.0f);
        // 逆旋转 = 转置（正交基）
        pcs.invRotRow0  = float4(r0.x, r1.x, r2.x, 0.0f);
        pcs.invRotRow1  = float4(r0.y, r1.y, r2.y, 0.0f);
        pcs.invRotRow2  = float4(r0.z, r1.z, r2.z, 0.0f);
        pcs.decalOrigin = float4(float3(wm[3]), 1.0f);
        pcs.halfExtents = float4(d.size.x * 0.5f, d.size.y * 0.5f,
                                 std::max(d.projectionDepth, 0.01f) * 0.5f, 0.0f);
        pcs.colorOpacity = float4(d.baseColorFactor.x, d.baseColorFactor.y, d.baseColorFactor.z, alpha);
        pcs.normalMetal  = float4(r2, d.metallicFactor);   // 投影方向 = 贴花局部 +Z 的世界方向
        const bool hasTex = !d.baseColorTexture.empty() && d.materialID > 0;
        pcs.params       = float4(d.roughnessFactor,
                                  hasTex ? (float)d.materialID : 0.0f,
                                  m_Width  > 0 ? 1.0f / (float)m_Width  : 0.0f,
                                  m_Height > 0 ? 1.0f / (float)m_Height : 0.0f);
        pcs.flags        = hasTex ? 1u : 0u;

        char label[64];
        snprintf(label, sizeof(label), "Decal Project #%u", m_LastDecalCount);
        cmd->SetDrawDebugLabel(label);
        cmd->SetPushConstants(0, sizeof(pcs), &pcs);
        cmd->SetVertexBuffer(m_CubeVB.get(), 0);
        cmd->SetIndexBuffer(m_CubeIB.get());
        cmd->DrawIndexed(36, 1, 0, 0);

        // 首帧打印逐贴花参数：贴花盒必须**包住**要贴的表面，参数错位时这里是第一现场
        if (m_FrameCount == 0 && m_LastDecalCount == 0) {
            HE_CORE_INFO("[任务 24] 贴花 #{}：中心 ({:.2f}, {:.2f}, {:.2f})，半尺寸 ({:.2f}, {:.2f}, {:.2f})，"
                         "投影法线 ({:.2f}, {:.2f}, {:.2f})，不透明度 {:.2f}，纹理槽 {}（{}）",
                         m_LastDecalCount, pcs.decalOrigin.x, pcs.decalOrigin.y, pcs.decalOrigin.z,
                         pcs.halfExtents.x, pcs.halfExtents.y, pcs.halfExtents.z,
                         pcs.normalMetal.x, pcs.normalMetal.y, pcs.normalMetal.z,
                         pcs.colorOpacity.w, (u32)pcs.params.y,
                         hasTex ? "有贴花纹理" : "纯色贴花");
        }
        ++m_LastDecalCount;
    });

    cmd->EndOffscreenPass();
    ++m_FrameCount;

    // 冒烟证据：首帧 + 每 300 帧打印一次（贴花数量、世界坐标裁剪是否在生效）
    if (m_FrameCount == 1 || (m_FrameCount % 300) == 0) {
        HE_CORE_INFO("[任务 24] GBuffer 投影贴花：本帧 {} 个贴花（累计 {} 帧，"
                     "读 MRT4 世界坐标裁剪 + 深度天空判定，只写 MRT0/1）",
                     m_LastDecalCount, m_FrameCount);
    }
}

} // namespace he::render
