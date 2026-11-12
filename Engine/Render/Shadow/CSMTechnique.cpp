#include "Shadow/CSMTechnique.h"
#include "Shadow/ShadowCasterCull.h"
#include "Shadow.mesh.spv.h"   // T10：阴影 MS 变体
#include "Shadow.mesh.dxil.h"   // T10：逐级联剔除
#include <iterator>
#include "Pipeline/Camera.h"
#include "Shadow.vert.spv.h"
#include "Shadow.vert.dxil.h"
#include "Shadow.vert.dxbc.h"
#include "Shadow.frag.spv.h"
#include "Shadow.frag.dxil.h"
#include "Shadow.frag.dxbc.h"
// 第③段第 2 批：按 meshIndex 从注册表取顶点/索引缓冲（不再遍历 ECS）
#include "Threading/MeshRegistry.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Math/Math.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cstdio>

namespace he::render {

float4x4 CSMTechnique::ComputeCascadeViewProj(const float3& ld,const CameraData& cam,float sn,float sf){
    // 计算相机视锥体的级联子视锥体 8 个角点（世界空间）
    float3 f=glm::normalize(cam.forward),r=glm::normalize(glm::cross(f,cam.up)),u=glm::cross(r,f);
    float th=glm::tan(glm::radians(cam.fov)*.5f);
    float fh_near=th*sn, fw_near=fh_near*cam.aspectRatio;  // 近平面半高/半宽
    float fh_far =th*sf, fw_far =fh_far *cam.aspectRatio;  // 远平面半高/半宽
    float3 nc=cam.position+f*sn;  // 近平面中心
    float3 fc=cam.position+f*sf;  // 远平面中心

    // 8 个视锥体角点
    float3 corners[8]={
        nc-r*fw_near-u*fh_near, nc+r*fw_near-u*fh_near,
        nc-r*fw_near+u*fh_near, nc+r*fw_near+u*fh_near,
        fc-r*fw_far -u*fh_far,  fc+r*fw_far -u*fh_far,
        fc-r*fw_far +u*fh_far,  fc+r*fw_far +u*fh_far,
    };

    // 场景中心 = 视锥体角点平均值（动态计算，替代硬编码）
    float3 scn=float3(0,0,0);
    for(int i=0;i<8;++i) scn+=corners[i];
    scn/=8.0f;

    // 光源视图矩阵：从光源方向观察场景中心（虚拟光源置于 500m 外，对 20m 级场景近似平行光）
    float3 lu=(abs(ld.y)<.999f)?float3(0,1,0):float3(1,0,0);
    float4x4 lv=glm::lookAtRH(scn-ld*500.f,scn,lu);

    // 将所有角点变换到光源视图空间，计算包围盒
    float mnX=FLT_MAX,mxX=-FLT_MAX,mnY=FLT_MAX,mxY=-FLT_MAX;
    for(auto&c:corners){float4 ls=lv*float4(c,1.f);mnX=glm::min(mnX,ls.x);mxX=glm::max(mxX,ls.x);mnY=glm::min(mnY,ls.y);mxY=glm::max(mxY,ls.y);}
    float h=glm::max(glm::max(-mnX,mxX),glm::max(-mnY,mxY));
    h=glm::max(h,5.f);
    // h 基于视锥包围盒实际（下限 5m，避免 200m 大视口导致分辨率低自影）;

    // 正交深度范围：动态适配视锥沿光源方向的深度跨度（世界单位，带余量）。
    // 修复：原先固定 near=0.1/far=8000 时，光源置于 4000m 外导致物体深度挤在
    // ndc 0.5 附近，深度分辨率 1/8000 米，bias(0.003) 折算约 24 米 → 阴影被偏置吞掉。
    float mnZ=FLT_MAX,mxZ=-FLT_MAX;
    for(auto&c:corners){
        float4 ls=lv*float4(c,1.f);
        mnZ=glm::min(mnZ,ls.z);
        mxZ=glm::max(mxZ,ls.z);
    }
    const float kDepthPad=50.f;                      // 深度方向余量（米）
    float zn=glm::max(-mxZ-kDepthPad,0.1f);          // 近裁剪（最近角点 - 余量）
    float zf=glm::max(-mnZ+kDepthPad,zn+1.f);        // 远裁剪（最远角点 + 余量）
    return glm::orthoRH_ZO(-h,h,-h,h,zn,zf)*lv;
}

bool CSMTechnique::Initialize(rhi::IRHIDevice* device){
    m_Device=device;
    m_ShadowVS.stage=rhi::ShaderStage::Vertex;
    m_ShadowVS.spirv=k_Shadow_vert_spv;
    m_ShadowVS.dxil.assign(std::begin(k_Shadow_vert_dxil), std::end(k_Shadow_vert_dxil));
    m_ShadowVS.dxil.assign(std::begin(k_Shadow_vert_dxil), std::end(k_Shadow_vert_dxil));
    m_ShadowVS.dxil.assign(std::begin(k_Shadow_vert_dxil), std::end(k_Shadow_vert_dxil));
    m_ShadowVS.dxbc.assign(std::begin(k_Shadow_vert_dxbc), std::end(k_Shadow_vert_dxbc));
    m_ShadowVS.entryPoint="main";
    m_ShadowFS.stage=rhi::ShaderStage::Pixel;
    m_ShadowFS.spirv=k_Shadow_frag_spv;
    m_ShadowFS.dxil.assign(std::begin(k_Shadow_frag_dxil), std::end(k_Shadow_frag_dxil));
    m_ShadowFS.dxil.assign(std::begin(k_Shadow_frag_dxil), std::end(k_Shadow_frag_dxil));
    m_ShadowFS.dxil.assign(std::begin(k_Shadow_frag_dxil), std::end(k_Shadow_frag_dxil));
    m_ShadowFS.dxbc.assign(std::begin(k_Shadow_frag_dxbc), std::end(k_Shadow_frag_dxbc));
    m_ShadowFS.entryPoint="main";
    // 每个飞行帧 × 每个级联各一张：阴影贴图每帧清空重画且同帧被采样，单份会被在飞帧采到中间态
    //（代价：2048² D32 ≈ 16.8 MB/张 ⇒ 3 级联 × 3 飞行帧 ≈ 151 MB）
    for(u32 s=0;s<MAX_FRAMES_IN_FLIGHT;++s){
        for(u32 c=0;c<CASCADE_COUNT;++c){
            rhi::TextureDesc d;
            d.format=rhi::Format::D32_FLOAT;
            d.width=m_ShadowMapSize;
            d.height=m_ShadowMapSize;
            d.depth=1;
            d.mipLevels=1;
            d.arrayLayers=1;
            d.usage=rhi::TextureUsage::DepthStencil|rhi::TextureUsage::ShaderResource;
            m_ShadowMaps[s][c]=device->CreateTextureHandle(d);   // A-3
            HE_ASSERT(m_ShadowMaps[s][c].IsValid(),"CSMTechnique: 阴影贴图句柄创建失败");
        }
    }
    rhi::SamplerDesc sd;
    sd.minFilter=rhi::FilterMode::Linear;
    sd.magFilter=rhi::FilterMode::Linear;
    sd.addressU=sd.addressV=sd.addressW=rhi::AddressMode::ClampToEdge;
    m_ShadowSampler=device->CreateSamplerHandle(sd);
    return true;
}

void CSMTechnique::CreatePSO(rhi::DescriptorSetLayoutHandle layout){
    rhi::VertexInputLayout vl;
    vl.stride=sizeof(he::StaticVertex);
    vl.attributes={{0,0,rhi::VertexFormat::Float3,offsetof(he::StaticVertex,position)}};
    rhi::PushConstantRange pcr;
    pcr.stageMask=rhi::kStageMaskVertex|rhi::kStageMaskFragment;
    pcr.offset=0;
    pcr.size=sizeof(ShadowPushConstant);
    rhi::PipelineStateDesc d;
    d.vertexShader=&m_ShadowVS;
    d.pixelShader=&m_ShadowFS;
    d.vertexLayout=vl;
    d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=d.depthWrite=true;
    d.depthCompare=rhi::CompareFunc::LessEqual;
    d.depthFormat=rhi::Format::D32_FLOAT;
    d.colorAttachmentCount=0;
    d.pushConstantRanges={pcr};d.descriptorSetLayouts={layout};d.debugName="CSMDepth";
    m_ShadowPSO=m_Device->CreatePipelineStateHandle(d);   // A-3
    HE_ASSERT(m_ShadowPSO.IsValid(),"CSMTechnique: PSO 句柄创建失败");

    // ── 【T10】阴影的 mesh 管线：与 IA 版**同附件、同深度状态、同 push constant**，
    //   只把几何入口换成 mesh shader（顶点/索引走 SRV，见布局 binding 27/28）。
    if (m_Device->GetCaps().supportsMeshShaders) {
        rhi::ShaderBytecode mesh{};
        mesh.stage = rhi::ShaderStage::Mesh;
        mesh.spirv = k_Shadow_mesh_spv;
        mesh.dxil.assign(std::begin(k_Shadow_mesh_dxil), std::end(k_Shadow_mesh_dxil));

        rhi::PipelineStateDesc md = d;          // 其余状态照抄 IA 版
        md.vertexShader = nullptr;
        md.meshShader   = &mesh;
        md.vertexLayout = rhi::VertexInputLayout{};   // MS 不用 IA 布局
        md.debugName    = "CSMDepthMS";
        m_ShadowMeshPSO = m_Device->CreatePipelineStateHandle(md);
        if (!m_ShadowMeshPSO.IsValid()) {
            HE_CORE_WARN("CSMTechnique: T10 阴影 mesh PSO 创建失败 ⇒ 保持 IA 路径");
        }
    }
}

void CSMTechnique::SetMeshShadowEnabled(bool enable){
    // 【门控】设备不支持 MS 或 mesh PSO 没建起来 ⇒ 保持 IA 路径（不静默切换）
    const bool msOk = m_Device != nullptr && m_Device->GetCaps().supportsMeshShaders
                   && m_ShadowMeshPSO.IsValid();
    const bool want = enable && msOk;
    if (want != m_UseMeshPath) {
        HE_CORE_INFO("CSMTechnique: T10 阴影绘制路径 → {}", want ? "mesh" : "IA");
    }
    m_UseMeshPath = want;
}

bool CSMTechnique::SetRemoveIaPipeline(bool remove){
    if (!remove) return false;
    const bool msOk = m_Device != nullptr && m_Device->GetCaps().supportsMeshShaders
                   && m_ShadowMeshPSO.IsValid();
    if (!msOk) {
        HE_CORE_WARN("CSMTechnique: T11 —— 本设备无可用 mesh 阴影管线 ⇒ 保留 IA 管线（回退路径）");
        return false;
    }
    // 强制走 mesh，并把 IA 管线真的销毁（不是"不再使用"）
    m_UseMeshPath = true;
    if (m_ShadowPSO.IsValid()) {
        m_Device->DestroyPipelineStateHandle(m_ShadowPSO);
        m_ShadowPSO = {};
        HE_CORE_INFO("CSMTechnique: T11 —— 已销毁阴影的 IA PSO（阴影改走 mesh 管线）");
    }
    return true;
}

void CSMTechnique::Shutdown(){
    // 【退出崩溃】先关掉渲染再销毁资源：收尾阶段可能还有在飞帧执行到
    //   `BeginOffscreenPass`，此时阴影贴图已销毁 ⇒ 驱动内崩溃。
    m_Active = false;
    if (m_Device) m_Device->WaitIdle();
    // 【空保护】m_Device 可能为空（Shutdown 被调用两次，或 Initialize 失败后进入清理路径），
    // 直接解引用会崩溃（实测：02.Cube 退出时连续崩在多个 Shutdown 里）。
    if (!m_Device) return;

    for(u32 s=0;s<MAX_FRAMES_IN_FLIGHT;++s)
        for(u32 c=0;c<CASCADE_COUNT;++c){   // A-3：二维贴图句柄必须显式销毁
            m_Device->DestroyTextureHandle(m_ShadowMaps[s][c]);
            m_ShadowMaps[s][c] = {};
        }
    m_Device->DestroySamplerHandle(m_ShadowSampler); m_ShadowSampler = {};   // A-3
    m_Device->DestroyPipelineStateHandle(m_ShadowPSO); m_ShadowPSO = {};
}
void CSMTechnique::SetRenderResources(rhi::IRHIBuffer* ob,rhi::DescriptorSetHandle ds){m_ExternalObjectBuffer=ob;m_ExternalDescSet=ds;}

u32 CSMTechnique::CollectLights(const FrameSceneSnapshot& snapshot,const CameraData& cam,
                                 std::vector<GPUShadowData>& out,std::vector<he::Entity>& ent){
    u32 start=(u32)out.size();
    // 只过滤自己那一类（方向光）；同类型内保持实体顺序 ⇒ 序列与旧 `world.ForEach<DirectionalLight>` 一致
    for(const SnapshotShadowLight& lc : snapshot.shadowLights){
        if(lc.type!=static_cast<u32>(he::LightType::Directional))continue;
        if(out.size()-start>=MAX_SHADOWS)break;
        float3 ld=glm::normalize(lc.direction);GPUShadowData sd{};float la=.5f;
        // CSM 有效阴影距离：限制在近处（避免用 farPlane=2000 导致级联覆盖巨大、
        // 贴图分辨率低、地板自影出现风车状伪影）
        float shDist=glm::min(cam.farPlane,120.f);
        for(u32 c=0;c<CASCADE_COUNT;++c){
            float p=(c+1)/(float)CASCADE_COUNT;
            float ls=cam.nearPlane*std::pow(shDist/cam.nearPlane,p);
            float us=cam.nearPlane+(shDist-cam.nearPlane)*p;
            sd.splitDistances[c]=la*ls+(1.f-la)*us;
            sd.lightViewProj[c]=ComputeCascadeViewProj(ld,cam,c==0?cam.nearPlane:sd.splitDistances[c-1],sd.splitDistances[c]);
        }
        sd.splitDistances[3]=shDist;
        sd.cameraForward=float4(glm::normalize(cam.forward),0);
        sd.shadowParams=float4(lc.shadowBias,lc.shadowNormalBias,lc.shadowStrength,0);
        {
            // 【T10 诊断】一次性打印级联矩阵的产出与相机输入
            static bool s_Logged = false;
            if (!s_Logged) {
                s_Logged = true;
                HE_CORE_INFO("[CSMTechnique] CollectLights 相机 near={:.3f} far={:.3f} fov={:.1f} aspect={:.3f} "
                             "split=[{:.1f},{:.1f},{:.1f},{:.1f}] VP0[3][3]={:.3f}",
                             cam.nearPlane, cam.farPlane, cam.fov, cam.aspectRatio,
                             sd.splitDistances[0], sd.splitDistances[1], sd.splitDistances[2],
                             sd.splitDistances[3], (float)sd.lightViewProj[0][3][3]);
            }
        }
        out.push_back(sd);
        ent.push_back(he::Entity{lc.sourceEntity});
    }
    return (u32)(out.size()-start);
}

void CSMTechnique::Render(rhi::IRHICommandList* cmd,const FrameSceneSnapshot& snapshot,
                           const MeshRegistry& registry,
                           const std::vector<GPUShadowData>& sd,u32 start){
    if(!m_Active)return;
    if(sd.empty()||!m_ExternalObjectBuffer||m_ExternalDescSet==rhi::kInvalidSet)return;
    // 【T10 修复】`start` 是本技术在本帧 `m_AllShadowData` 里的**起始偏移**；
    //   若本技术收集到 0 个光源（例如场景没有方向光），`start == sd.size()`
    //   ⇒ 再取 `sd[start]` 就是越界读（实测拿到的"零矩阵"正是这么来的：
    //   级联 VP 全 0 ⇒ 剔除退化、阴影矩阵也是零）。
    if(start>=sd.size())return;
    for(u32 c=0;c<CASCADE_COUNT;++c)RenderCascade(cmd,c,snapshot,registry,sd[start]);
    for(u32 c=0;c<CASCADE_COUNT;++c)
        cmd->PipelineBarrier(rhi::PipelineStage::LateFragmentTests,rhi::PipelineStage::FragmentShader,
            rhi::ResourceState::DepthStencilRead,rhi::ResourceState::ShaderResource,ResT(m_ShadowMaps[m_FrameSlot][c]));
}

void CSMTechnique::RenderCascade(rhi::IRHICommandList* cmd,u32 ci,const FrameSceneSnapshot& snapshot,
                                  const MeshRegistry& registry,const GPUShadowData& sd){
    if(!m_Active)return;
    m_LightVPs[ci] = sd.lightViewProj[ci];  // 缓存供 RSM 查询
    {
        // 【T10 诊断】打印本级 VP 的两个元素：若全 0 ⇒ CollectLights 产出的矩阵就是零
        static bool s_VpLogged = false;
        if (!s_VpLogged) {
            s_VpLogged = true;
            HE_CORE_INFO("[CSMTechnique] 级联 {} VP[0][0]={:.4f} VP[1][1]={:.4f} VP[2][2]={:.4f} VP[3][3]={:.4f}",
                         ci, (float)m_LightVPs[ci][0][0], (float)m_LightVPs[ci][1][1],
                         (float)m_LightVPs[ci][2][2], (float)m_LightVPs[ci][3][3]);
        }
    }
    void*dv=ResT(m_ShadowMaps[m_FrameSlot][ci])->GetNativeHandle();
    if(!dv)return;
    rhi::ClearValue cv{};cv.depth=1.f;
    // pass 级调试标记：RenderDoc 中可识别阴影级联 pass
    char passLabel[32];
    snprintf(passLabel,sizeof(passLabel),"CSM Shadow C%u",ci);
    cmd->SetDrawDebugLabel(passLabel);
    cmd->BeginOffscreenPass(nullptr,dv,m_ShadowMapSize,m_ShadowMapSize,&cv);
    cmd->SetFullscreenViewport(static_cast<u32>(m_ShadowMapSize), static_cast<u32>(m_ShadowMapSize));
    // 【T11 防御】两版 PSO 都可能在重载/尺寸变化后失效；此时**不录制**本级联，
    //   而不是把空句柄交给 `SetPipeline`（实测会崩在 `VulkanPipelineState::GetPipeline`）。
    rhi::IRHIPipelineState* cascadePso = m_UseMeshPath ? ResP(m_ShadowMeshPSO) : ResP(m_ShadowPSO);
    if (cascadePso == nullptr) {
        cascadePso = ResP(m_ShadowMeshPSO);
        if (cascadePso == nullptr) cascadePso = ResP(m_ShadowPSO);
    }
    if (cascadePso == nullptr) {
        static bool s_Warned = false;
        if (!s_Warned) {
            s_Warned = true;
            HE_CORE_WARN("CSMTechnique: 两级阴影 PSO 均不可用 ⇒ 本帧跳过级联绘制");
        }
        return;
    }
    cmd->SetPipeline(cascadePso);
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame,m_ExternalDescSet);
 auto*objData=static_cast<GPUObjectData*>(m_ExternalObjectBuffer->Map());
    u32 oi=0;
    static u32 s_MeshObjects = 0u;
    static u32 s_MeshTriangles = 0u;
    // 【第③段第 2 批】遍历**快照**里的阴影投射者（顺序 = 旧的 Mesh→Cube→Sphere 相对顺序），
    // 顶点/索引缓冲与索引数从注册表按 meshIndex 取。
    // 【T10】先收集本帧所有阴影投射者的**世界 AABB**，用本级的 lightViewProj 做一次剔除，
    //   只对可见的物体填对象数据并 DrawIndexed。级联之间只差裁剪矩阵，
    //   但物体集合应当随级联不同 ⇒ 这才是"逐光源/逐级联剔除"的意义。
    std::vector<ShadowCasterBounds> casterBounds;
    std::vector<u32>                casterDrawIndex;
    casterBounds.reserve(snapshot.draws.size());
    casterDrawIndex.reserve(snapshot.draws.size());
    for (u32 di = 0; di < (u32)snapshot.draws.size(); ++di) {
        const SnapshotDrawItem& it = snapshot.draws[di];
        if (!it.bShadowCaster) continue;
        ShadowCasterBounds b{};
        b.min[0] = it.object.boundsMin.x; b.min[1] = it.object.boundsMin.y; b.min[2] = it.object.boundsMin.z;
        b.max[0] = it.object.boundsMax.x; b.max[1] = it.object.boundsMax.y; b.max[2] = it.object.boundsMax.z;
        casterBounds.push_back(b);
        casterDrawIndex.push_back(di);
    }
    const NaniteFrustumPlanes cascadeFrustum =
        NaniteExtractFrustumPlanes(&m_LightVPs[ci][0][0]);
    std::vector<u32> visibleIdx(casterBounds.size());
    const u32 visibleCount = CullShadowCasters(cascadeFrustum, casterBounds.data(),
                                               (u32)casterBounds.size(),
                                               visibleIdx.data(), (u32)visibleIdx.size());
    if (visibleCount < casterBounds.size()) visibleIdx.resize(visibleCount);
    {
        // 【T10 诊断】一次性打印本级视锥的 6 个平面常数项与首个法线：
        //   若法线全 0 ⇒ 矩阵口径不对，剔除退化成"永远可见"（当前 105/105 的可疑点）。
        static bool s_PlaneLogged = false;
        if (!s_PlaneLogged) {
            s_PlaneLogged = true;
            HE_CORE_INFO("[CSMTechnique] T10 级联 {} 平面 d=[{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f}] "
                         "n0=({:.3f},{:.3f},{:.3f})",
                         ci, cascadeFrustum.planes[0][3], cascadeFrustum.planes[1][3],
                         cascadeFrustum.planes[2][3], cascadeFrustum.planes[3][3],
                         cascadeFrustum.planes[4][3], cascadeFrustum.planes[5][3],
                         cascadeFrustum.planes[0][0], cascadeFrustum.planes[0][1],
                         cascadeFrustum.planes[0][2]);
        }
    }

    for (u32 vi = 0; vi < visibleIdx.size(); ++vi) {
        const SnapshotDrawItem& it = snapshot.draws[casterDrawIndex[visibleIdx[vi]]];
        if (oi >= MAX_OBJECTS) break;
        const MeshRegistryEntry* me=registry.Find(it.meshIndex);
        if(!me||!me->vertexBuffer||!me->indexBuffer)continue;
        objData[oi].worldMatrix=it.object.worldMatrix;
        // 光照图展开用的世界 AABB（任务 31）：阴影路径不读它，但对象缓冲的同一元素会被
        // 别的消费者看到，留成未初始化会让"读 AABB"变成读垃圾 ⇒ 一并填上（成本可忽略）。
        objData[oi].boundsMin=it.object.boundsMin;
        objData[oi].boundsMax=it.object.boundsMax;
        // DrawCall 调试 marker：标记当前级联与物体（RenderDoc 定位用）
        char label[64];
        snprintf(label,sizeof(label),"Shadow C%u Obj#%u",ci,oi);
        cmd->SetDrawDebugLabel(label);
        ShadowPushConstant pc{};pc.lightViewProj=sd.lightViewProj[ci];pc.objectIndex=oi++;
        if (m_UseMeshPath) {
            // ── 【T10】mesh 路径：顶点/索引走 SRV（布局 binding 27/28），
            //   一个工作组 = 64 个三角形 ⇒ 按本物体三角形数算 Y 向组数 ──
            const u32 triCount = me->indexCount / 3u;
            m_Device->UpdateDescriptorSet(m_ExternalDescSet, 27,
                                          rhi::DescriptorType::StorageBuffer, me->vertexBuffer);
            m_Device->UpdateDescriptorSet(m_ExternalDescSet, 28,
                                          rhi::DescriptorType::StorageBuffer, me->indexBuffer);
            // 【T10 关键】mesh 版 push constant 与 IA 版**同长**（80B），但后 3 个 u32 的语义不同：
            //   IA 只用 `objectIndex`；MS 还要 `firstTriangle` / `triangleCount`。
            //   直接复用 `ShadowPushConstant` 会把这两个字段留成 0 ⇒ 着色器
            //   `SetMeshOutputCounts(0,0)`，阴影图整帧不被写入（实测与 IA 版差异 5.4%）。
            struct ShadowMeshPush {
                float4x4 lightViewProj;
                u32      objectIndex;
                u32      firstTriangle;
                u32      triangleCount;
                u32      pad;
            };
            static_assert(sizeof(ShadowMeshPush) == sizeof(ShadowPushConstant),
                          "两版 push constant 必须同长，否则同一 PSO 布局会错位");
            ShadowMeshPush mp{};
            mp.lightViewProj = sd.lightViewProj[ci];
            mp.objectIndex   = pc.objectIndex;
            mp.firstTriangle = 0u;
            mp.triangleCount = triCount;
            mp.pad           = 0u;
            ++s_MeshObjects;
            s_MeshTriangles += triCount;
            const u32 groupsY = (triCount + 63u) / 64u;
            cmd->SetPushConstants(0, sizeof(mp), &mp);
            cmd->DrawMeshTasks(1u, groupsY, 1u);
        } else {
            cmd->SetPushConstants(0,sizeof(ShadowPushConstant),&pc);
            cmd->SetVertexBuffer(me->vertexBuffer,0);
            cmd->SetIndexBuffer(me->indexBuffer);
            cmd->DrawIndexed(me->indexCount);
        }
    }
    m_ExternalObjectBuffer->Unmap();
    // 一次性诊断日志：确认阴影级联实际绘制了物体数（RenderDoc 复核用）
    // 【T10】每级一次：剔除前后条数（RenderDoc 之外的量化对照）
    {
        static bool s_CullLogged[CASCADE_COUNT] = { false };
        if (ci < CASCADE_COUNT && !s_CullLogged[ci]) {
            s_CullLogged[ci] = true;
            HE_CORE_INFO("[CSMTechnique] T10 级联 {} 剔除后 {} / 全部 {}（绘制物体 {}）",
                         ci, visibleCount, (u32)casterBounds.size(), oi);
        }
    }
    cmd->EndOffscreenPass();
    {
        // 【T10 对比】每级一次：mesh 路径实际派发的物体数 / 三角形数（与 IA 版的物体数对照）
        static bool s_Logged[CASCADE_COUNT] = { false };
        if (ci < CASCADE_COUNT && !s_Logged[ci]) {
            s_Logged[ci] = true;
            HE_CORE_INFO("[CSMTechnique] T10 级联 {} 路径={} 物体={} 三角形={}",
                         ci, m_UseMeshPath ? "mesh" : "IA",
                         m_UseMeshPath ? s_MeshObjects : oi,
                         m_UseMeshPath ? s_MeshTriangles : 0u);
            s_MeshObjects = 0u;
            s_MeshTriangles = 0u;
        }
    }
}

rhi::IRHITexture* CSMTechnique::GetShadowMap(u32 i)const{   // A-3：句柄按需解析
    return m_Device?ResT(m_ShadowMaps[m_FrameSlot][(i<CASCADE_COUNT)?i:0]):nullptr;}
rhi::IRHITexture* CSMTechnique::GetShadowMapAt(u32 slot,u32 i)const{   // A-3
    return m_Device?ResT(m_ShadowMaps[(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u][(i<CASCADE_COUNT)?i:0]):nullptr;
}

} // namespace he::render
