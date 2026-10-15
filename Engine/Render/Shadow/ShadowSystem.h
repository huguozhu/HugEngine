#pragma once

#include "Shadow/IShadowSystem.h"
#include "Shadow/IShadowTechnique.h"
// 第③段第 2 批：`Update` 改吃快照（`SubsystemContext::snapshot`）
#include "Threading/FrameSceneSnapshot.h"
#include <memory>
#include <vector>

namespace he { class World; class SceneGraph; }

namespace he::render {

class MeshRegistry;   // 第③段第 2 批：技术渲染时按 meshIndex 取顶点/索引缓冲

// ============================================================================
// ShadowSystem — 阴影子系统组合器
//
// 组合多个 IShadowTechnique（CSM / Point / RT），统一管理：
//   - ShadowData SSBO（三缓冲）
//   - Entity → ShadowIndex 映射
//   - 对外暴露 IShadowSystem 接口
// ============================================================================
class ShadowSystem : public IShadowSystem {
    HE_DECLARE_NON_COPYABLE(ShadowSystem);
public:
    ShadowSystem()=default;
    ~ShadowSystem()override=default;

    // IRenderSubsystem
    bool Initialize(rhi::IRHIDevice* device,u32 width,u32 height)override;
    void Shutdown()override;
    void Update(const SubsystemContext& ctx)override;
    /// 见 `IShadowSystem::SetFrameSnapshot`：管线每帧无条件绑定本帧快照
    void SetFrameSnapshot(const FrameSceneSnapshot& snapshot, const MeshRegistry& registry)override;
    void Render(rhi::IRHICommandList* cmdList)override;
    void Bind(rhi::IRHICommandList* cmdList)const override;
    void OnResize(u32 width,u32 height)override;
    const char* GetName()const override{return"ShadowSystem";}
    bool IsReady()const override{return m_Ready;}
    bool IsEnabled()const override{return m_Enabled;}
    void SetEnabled(bool e)override{m_Enabled=e;}

    // IShadowSystem
    Mode GetMode()const override{return Mode::Traditional;}
    void NextFrame()override;
    void SetRenderResources(rhi::IRHIBuffer* objBuf,rhi::IRHIBuffer* shadowBuf,rhi::DescriptorSetHandle descSet)override;
    void CreateShadowPSO()override;
    u32 GetShadowMapCount()const override;
    rhi::IRHITexture* GetShadowMap(u32 i)const override;
    rhi::IRHITexture* GetShadowMapAt(u32 slot,u32 i)const override;
    rhi::IRHISampler* GetShadowSampler()const override;
    rhi::IRHITexture* GetPointShadowMap(u32 slot)const override;
    rhi::IRHISampler* GetPointShadowSampler()const override;
    rhi::IRHITexture* GetSpotShadowMap()const;    // 聚光灯 2D 阴影贴图（当前飞行帧槽位）
    rhi::IRHITexture* GetSpotShadowMapAt(u32 slot)const;
    rhi::IRHISampler* GetSpotShadowSampler()const;
    rhi::IRHITexture* GetRectShadowMap()const;    // 矩形面光阴影贴图（当前飞行帧槽位）
    rhi::IRHITexture* GetRectShadowMapAt(u32 slot)const;
    rhi::IRHISampler* GetRectShadowSampler()const;
    i32 GetShadowIndex(Entity light)const override;
    bool HasActiveShadows()const override{return m_ActiveCount>0;}
    bool WasShadowMapWritten(u32 index)const override;
    float4x4 GetLightViewProj(u32 cascade)const override;

private:
    std::vector<std::unique_ptr<IShadowTechnique>> m_Techniques;

    // ============================================================
    // 阴影 pass 自持的对象数据绑定（独立布局 + 每飞行帧一份描述符集）
    //
    // 【为什么不能用主管线的 binding 2】阴影 pass 的 `objectIndex` 由各技术自己遍历
    // `snapshot.draws` 里 `bShadowCaster` 的条目编号（= "全部投影物体"），而场景绘制用的
    // `m_ObjectBuffers` 是 `SceneRenderer::Prepare` **视锥剔除后**的可见列表编号 —— 两套编号
    // 不同。旧实现靠"渲染前把 binding 2 换成阴影缓冲、渲染后换回来"，但两次 `UpdateDescriptorSet`
    // 都在提交之前完成，**GPU 执行整条命令缓冲时只会看到最终状态** ⇒ 阴影 pass 实际读的是场景
    // 缓冲：索引错位（读到别的物体的矩阵），且下标超过可见数时读到的是**未写入的尾部**。
    // 现在阴影 pass 用自己这一份常驻绑定（`kBindingShadowObjects`），与场景那份互不干扰；
    // 且只在缓冲指针真的变化时更新本槽位的集 ⇒ **每帧零描述符写入**。
    // ============================================================
    rhi::DescriptorSetLayoutHandle m_ShadowObjLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_ShadowObjSets[MAX_FRAMES_IN_FLIGHT] = {};
    rhi::IRHIBuffer*               m_ShadowObjBuffers[MAX_FRAMES_IN_FLIGHT] = {};   // 逐槽位缓存（脏判据）

    // ShadowData SSBO（三缓冲，ForwardPipeline 自有）
    rhi::IRHIBuffer* m_ExternalShadowBuffer=nullptr;

    // 所有光源的 Entity→Index 映射（每帧 Update 重建）
    std::vector<he::Entity>    m_AllEntities;
    std::vector<GPUShadowData> m_AllShadowData;       // 缓存（供 Render 复用）
    std::vector<u32>           m_PerTechniqueCounts;  // 每个 Technique 的条目数

    u32 m_CurrentFrameSlot=0,m_ActiveCount=0;
    bool m_Ready=false;
    he::World* m_CachedWorld=nullptr;
    he::SceneGraph* m_CachedSceneGraph=nullptr;
    /// 【第③段第 2 批】本帧快照与网格注册表：`Update` 时从 `SubsystemContext` 取得，
    /// `Render` 时交给各技术（两者都在同一帧内有效；快照归管线所有，这里只借指针）。
    const FrameSceneSnapshot* m_CachedSnapshot=nullptr;
    const MeshRegistry*       m_CachedRegistry=nullptr;
};

} // namespace he::render
