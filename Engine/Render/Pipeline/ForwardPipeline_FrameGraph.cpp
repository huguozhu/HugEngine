#include "Pipeline/ForwardPipeline.h"
#include "GI/GI_IBL.h"
#include "GI/GI_RSM.h"
#include "Shadow/ShadowSystem.h"
#include "Shadow/ShadowNone.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "SceneRenderer.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Threading/JobSystem.h"
#include "PBR.vert.spv.h"
#include "PBR.frag.spv.h"
#include "GBuffer.mesh.spv.h"
#include "AntiAliasing/AA_None.h"
#include "AntiAliasing/AA_FXAA.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>  // orthoRH_ZO (Vulkan Z [0,1])
#include <unordered_set>

#include <chrono>
#include <mutex>


namespace he::render {

// 从 ForwardPipeline.cpp 提取 — BuildFrameGraph 渲染图定义

void ForwardPipeline::BuildFrameGraph(RenderGraph& rg, he::World& world,
                                       he::SceneGraph& sg, const CameraData& camera)
{
    he::SyncPhysicalSkyToSun(world);  // 物理天空太阳→方向光同步（阴影/光照收集前）
    if (m_SwapChain) rg.SetSwapChain(m_SwapChain);
    // 交换链颜色格式（SDR=BGRA8，HDR=A2B10G10R10），同步到 ToneMap 输出格式与 HDR 开关
    rhi::Format swapFmt = m_SwapChain ? m_SwapChain->GetColorFormat() : rhi::Format::BGRA8_UNORM;
    m_ToneMap->SetOutputFormat(swapFmt);
    m_ToneMap->SetHDREnabled(swapFmt == rhi::Format::A2B10G10R10_UNORM_PACK32);
    u32 sw = m_SwapChain->GetWidth(), sh = m_SwapChain->GetHeight();
    if (m_ToneMap) m_ToneMap->OnResize(sw, sh);
    if (m_Skybox)  m_Skybox->OnResize(sw, sh);
    u32 w = m_HDRWidth, h = m_HDRHeight;
    auto hdrColor = rg.ImportTexture("HDR_Color", m_HDRTarget.get());
    auto hdrDepth = rg.ImportTexture("HDR_Depth", m_HDRDepth.get());
    auto backBuf  = rg.ImportBackBuffer();

    // --- Pass 0: Shadow — CSM 级联 + Point Cubemap 阴影贴图渲染 ---
    // 声明写入所有阴影贴图 + hdrDepth（WAW 确保 Shadow 先于 FullScene）
    if (m_ShadowSystem && m_ShadowSystem->HasActiveShadows()) {
        ResourceHandle csmMaps[CASCADE_COUNT];
        for (u32 c = 0; c < CASCADE_COUNT; ++c) {
            auto* tex = m_ShadowSystem->GetShadowMap(c);
            if (tex) {
                char name[32];
                snprintf(name, sizeof(name), "CSM_ShadowMap_C%u", c);
                csmMaps[c] = rg.ImportTexture(name, tex);
            } else {
                csmMaps[c] = kInvalidHandle;
            }
        }

        std::vector<PassResource> shadowWrites;
        for (u32 c = 0; c < CASCADE_COUNT; ++c)
            if (csmMaps[c] != kInvalidHandle)
                shadowWrites.push_back(RG_WRITE(csmMaps[c]));

        // Point Shadow Cubemap
        if (auto* ptTex = m_ShadowSystem->GetPointShadowMap()) {
            auto ptHandle = rg.ImportTexture("PointShadow_CubeMap", ptTex);
            shadowWrites.push_back(RG_WRITE(ptHandle));
        }

        // WAW 依赖：声明写入 hdrDepth 确保 Shadow → FullScene 的执行顺序
        shadowWrites.push_back(RG_WRITE(hdrDepth));

        rg.AddPass("Shadow", {}, std::move(shadowWrites),
            [this](rhi::IRHICommandList* c) {
                // 切换描述符集 binding 2 到阴影专用 Object Buffer
                // （仅更新 set=0 per-frame 集，per-mesh set=1 不包含 buffer 绑定）
                u32 slot = m_CurrentFrameSlot;
                m_Device->UpdateDescriptorSet(m_DescSets[slot], rhi::kBindingObjectData,
                    rhi::DescriptorType::StorageBuffer,
                    m_ShadowObjBuffers[slot].get());

                m_ShadowSystem->Render(c);

                // 恢复 binding 2 到场景 Object Buffer（FullScene 使用）
                m_Device->UpdateDescriptorSet(m_DescSets[slot], rhi::kBindingObjectData,
                    rhi::DescriptorType::StorageBuffer,
                    m_ObjectBuffers[slot].get());
            });
    }

    // --- Pass 1: IBL 生成（仅在天空盒脏时执行）---
    // 【任务 26 补的一处】必须先**把场景的天空盒交给 GI_IBL**，再判脏、再烘焙。
    // 此前这里直接 `giIBL->IsDirty()` 就烘焙：Forward 走 RenderGraph 时没有任何地方调
    // `SetIBLSkybox`（只有不走 RG 的 `PrepareGI` 里有），于是烘焙的是"未设置的天空盒"——
    // 辐照度/预滤波图接近全黑，PBR 里的 IBL 漫反射与镜面**恒为 0**，而层栈、能力位、面板
    // 全都显示正常。这与 §9.2-Q（IBL 从未烘焙、消费者照采）是同一类失效，只是发生在 Forward。
    // 实测指纹：`pipeline_mode=0` 下把 diffuse 层栈从 {IBL} 改成 {IBL,RSM} 甚至只放 {RSM}，
    // HDR 读数**逐位相同**（0.1836214），而把 UBO 的 count 直接画到颜色上又能看到 1/2 之差
    // ——证明配置与 UBO 都是通的，是这两个源的贡献本身为 0。
    auto* giIBL = dynamic_cast<GI_IBL*>(m_GI.get());
    if (giIBL && giIBL->IsEnabled()) {
        world.ForEach<he::SkyboxComponent>([&](he::Entity, he::SkyboxComponent& sc) {
            if (sc.enabled && sc.GetCubemap()) {
                giIBL->SetIBLSkybox(sc.GetCubemap(), sc.GetCubemapSampler());
            }
        });
    }
    bool iblNeedsUpdate = false;
    if (giIBL && giIBL->IsDirty()) {
        auto iblIrr  = rg.ImportTexture("IBL_Irradiance", giIBL->GetIrradianceMap());
        auto iblPref = rg.ImportTexture("IBL_Prefilter",  giIBL->GetPrefilterMap());
        auto iblLUT  = rg.ImportTexture("IBL_BRDF_LUT",   giIBL->GetBRDF_LUT());
        rg.AddPass("IBL_Generate", {},
            {{iblIrr, ResourceAccess::Write}, {iblPref, ResourceAccess::Write}, {iblLUT, ResourceAccess::Write}},
            [giIBL](rhi::IRHICommandList* c) { giIBL->Render(c); });
        iblNeedsUpdate = true;
    }

    // --- Pass 2: RSM 生成（Reflective Shadow Maps）---
    // GIConfig 门控：rsmIndirect=false 时不注册（Forward 的间接漫反射来源）
    // 【任务 34 / §9.2-AD 已修】此前本分支在 06.GILab 的 Forward 模式下恒不注册，三层原因：
    //   ① Forward 管线的阴影系统要由**调用方**先 SetRenderResources + Update（02.Cube /
    //      03.Sponza / AISamples 都这么做，06.GILab 没有）⇒ `HasActiveShadows()` 恒为 false。
    //      已修：示例在 Forward 分支里照 02.Cube 驱动阴影系统（顺带让 Forward 画面第一次有阴影）。
    //   ② 本 pass 用的是 **CSM 级联 0** 的 VP，而它由 Shadow pass 的 `RenderCascade` 才写进
    //      `m_LightVPs`；帧图里两个 pass 声明的是**互不相干的纹理**（阴影图 vs RSM 三张图），
    //      **没有依赖边** ⇒ 执行顺序不受保证。已修：改用下面这份**自己算的**固定光锥，
    //      与 Shadow pass 再无隐式顺序要求。
    //   ③ CSM 的 VP 拟合**相机视锥** ⇒ RSM 内容随视角变化（探针/世界空间使用它的前提被破坏）。
    //      已修：`ForwardPipeline::RefreshRSMFrustum` 按场景包围盒拟合（与 Deferred 同一份
    //      `FitRSMFrustumToBounds`），并且**同一个** VP 经 `GIBlendParams` 交给 PBR 的内联查表
    //      —— 写入 UV 与查找 UV 同源。
    if (m_RSMFrustumValid && m_RSM) {
        const float4x4 lightVP = m_RSMLightViewProj;   // 按值捕获：帧图 lambda 不得读失效栈帧
        auto rsmPos  = rg.ImportTexture("RSM_Position",  m_RSM->GetRSMPositionMap());
        auto rsmNrm  = rg.ImportTexture("RSM_Normal",    m_RSM->GetRSMFluxMap());
        auto rsmRad  = rg.ImportTexture("RSM_Radiance",  m_RSM->GetRSMRadianceMap());
        rg.AddPass("RSM_Generate", {},
            {{rsmPos, ResourceAccess::Write}, {rsmNrm, ResourceAccess::Write},
             {rsmRad, ResourceAccess::Write}},
            [this, &world, &sg, lightVP](rhi::IRHICommandList* c) {
                m_RSM->SetLightViewProj(lightVP,
                    m_RSM->GetRSMPositionMap()->GetWidth(),
                    m_ShadowSystem->GetShadowSampler(),
                    m_DescSets[m_CurrentFrameSlot]);
                // 通量要读方向光的颜色/强度：不绑光源缓冲就会读到对象缓冲（§9.2-AA ①）。
                // 这条此前只加在 PrepareGI（非 RG 路径）里，RG 路径漏了 —— 同一个坑两处。
                m_RSM->SetLightBuffer(GetCurrentLightBuffer());
                m_RSM->RenderRSMPass(c, world, sg);
            });
    }

    // --- Pass 2.5: Forward+ 光源剔除（Compute Pass，仅在开启时）---
    if (m_UseForwardPlus && m_ClusteredShading.enabled) {
        rg.AddPass("ForwardPlus_LightCull",
            {}, {},
            [this, &world, &sg, &camera, w, h](rhi::IRHICommandList* c) {
                // 收集光源到 GPU 缓冲区
                PushConstantData pc;
                CollectLights(pc, world, sg, camera);

                // 缓存光源到 CPU（供 Cluster 剔除使用）
                m_CachedLights.resize(pc.lightCount);
                auto* gpuLights = static_cast<const GPULight*>(
                    m_LightBuffers[m_CurrentFrameSlot]->Map());
                if (gpuLights && pc.lightCount > 0) {
                    memcpy(m_CachedLights.data(), gpuLights,
                           pc.lightCount * sizeof(GPULight));
                }
                m_LightBuffers[m_CurrentFrameSlot]->Unmap();

                // 构建 Cluster AABB + CPU 端光源剔除
                float4x4 invVP = glm::inverse(camera.GetViewProjMatrix());
                m_ClusteredShading.BuildClusters(invVP, w, h,
                    camera.nearPlane, camera.farPlane);
                m_ClusteredShading.CullLights(m_CachedLights.data(), pc.lightCount);

                // 上传 LightGrid 到 GPU（binding 7）
                auto& grid = m_ClusteredShading.GetLightGrid();
                if (!m_LightGridBuffer ||
                    m_LightGridBuffer->GetSize() < grid.size() * sizeof(ClusteredShading::LightGridCell)) {
                    rhi::BufferDesc d;
                    d.size = grid.size() * sizeof(ClusteredShading::LightGridCell);
                    d.usage = rhi::BufferUsage::Storage;
                    d.cpuAccess = true;
                    m_LightGridBuffer = m_Device->CreateBuffer(d);
                }
                void* mappedGrid = m_LightGridBuffer->Map();
                if (mappedGrid && !grid.empty()) {
                    memcpy(mappedGrid, grid.data(),
                           grid.size() * sizeof(ClusteredShading::LightGridCell));
                }
                m_LightGridBuffer->Unmap();

                // 上传 LightIndexList 到 GPU（binding 8）
                auto& list = m_ClusteredShading.GetLightIndexList();
                u32 listByteSize = (u32)(list.size() * sizeof(u32));
                if (!m_LightIndexListBuffer ||
                    m_LightIndexListBuffer->GetSize() < listByteSize) {
                    rhi::BufferDesc d;
                    d.size = std::max<usize>(listByteSize, 64u);
                    d.usage = rhi::BufferUsage::Storage;
                    d.cpuAccess = true;
                    m_LightIndexListBuffer = m_Device->CreateBuffer(d);
                }
                void* mappedList = m_LightIndexListBuffer->Map();
                if (mappedList && !list.empty()) {
                    memcpy(mappedList, list.data(), listByteSize);
                }
                m_LightIndexListBuffer->Unmap();

                // 更新全部三缓冲描述符集的 LightGrid / LightIndexList 绑定
                for (u32 si = 0; si < MAX_FRAMES_IN_FLIGHT; ++si) {
                    m_Device->UpdateDescriptorSet(m_DescSets[si], rhi::kBindingLightGrid,
                        rhi::DescriptorType::StorageBuffer, m_LightGridBuffer.get());
                    m_Device->UpdateDescriptorSet(m_DescSets[si], rhi::kBindingLightIndexList,
                        rhi::DescriptorType::StorageBuffer, m_LightIndexListBuffer.get());
                }
            },
            RGPassQueue::Compute);
    }

    // --- Pass 3: Scene — HDR 几何 + 天空盒渲染 ---
    rg.AddPass("Scene",
        {{hdrDepth, ResourceAccess::Read}},  // 读深度确保在 Shadow 之后
        {{hdrColor, ResourceAccess::Write}, {hdrDepth, ResourceAccess::Write}},
        [this, w, h, &world, &sg, &camera, iblNeedsUpdate](rhi::IRHICommandList* c) {
            // IBL bindings 更新（IBL pass 之后纹理已变化）
            if (iblNeedsUpdate && m_GI) {
                auto* gi = dynamic_cast<GI_IBL*>(m_GI.get());
                if (gi) UpdateIBLBindings(gi);
            }
            BeginHDRPass(c, w, h);
            BeginFrame(c, w, h);
            RenderScene(c, world, sg, camera);
            RenderSkybox(c, world, camera);
            EndHDRPass(c);
        });

    // --- Pass 4: ToneMap — HDR → LDR 色调映射 ---
    rg.AddPass("ToneMap", {{hdrColor, ResourceAccess::Read}}, {{backBuf, ResourceAccess::Write}},
        [this, swapFmt](rhi::IRHICommandList* c) {
            m_ToneMap->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
            m_ToneMap->PreBind(c);
            c->BeginRenderPass(1, swapFmt);
            m_ToneMap->Render(c);
            c->EndRenderPass();
        });
}

} // namespace he::render
