// ============================================================
// Nanite/NaniteRenderer.cpp — 模块门面的实现
//   §14.8 任务 1：骨架 + 独立开关 + `Nanite_Noop` 占位 pass
//   §14.8 任务 3：占位 pass 换成「计数 → 间接绘制」链的 `Nanite_Cull` + `Nanite_Raster`
//
// 【开启档的 pass 集合】既有 12 个 pass（相对顺序不变）+ `Nanite_InstanceCull`（任务 13）
//   + `Nanite_Cull` + `Nanite_Raster`（任务 3）= 15 个。
//   任务 4 追加一个**可选**的第三个 pass `Nanite_TestWrite`（仅在 `NaniteSettings::testWrite`
//   为真时注册；它写既有 GBuffer albedo 的 UAV，必须在 GBuffer 之后注册才能被 Lighting 同帧读到）。
//   任务 6 追加另一个**可选**的第四个 pass `Nanite_MeshTest`（仅在 `NaniteSettings::meshTest`
//   为真且设备支持 `VK_EXT_mesh_shader` 时注册；它只画模块自建的 1×1 R8 目标 + 写模块自持计数缓冲，
//   因此与 GBuffer 无关，注册在第一处挂钩即可）。
//   两个 pass 的 `reads/writes` 复刻 `GB_Clear` 对 `gbDepth/gbWorldPos` 的那组 WAW 声明：
//   `Shadow` 用 `RG_WRITE(gbDepth)/RG_WRITE(gbWorldPos)` 这条**假 WAW 依赖**
//   把自己定序在 GBuffer 写入者之前（DeferredPipeline_FrameGraph.cpp:213-215）。
//   模块将来接管 GBuffer 写入时必须声明同一组依赖，否则 Shadow/Lighting 的排序会
//   静默变化（§14.5 第一条硬约束）。
//
// 【可见画面零影响】`Nanite_Raster` 渲染到模块自建的 1×1 R8 小目标（不是 GBuffer 附件），
//   `Nanite_Cull` 只写模块自持缓冲 ⇒ 开启档转储与关闭档逐位相同。
//   【任务 12 追加】资产上传/读回也不注册任何 pass：它在帧图构建期做一次"一次性命令表 +
//   WaitIdle"，只写模块自建的资产缓冲与读回缓冲 ⇒ 开启档的 pass 集合仍是
//   `Nanite_Cull` + `Nanite_Raster`（判据 ⑥ 的 pass 列表因此不变）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。故本文件不 include 任何 GI/Lumen/GPUCulling 头；
//   `MeshBatcher` 只以 `const&` 参数出现在 `EnsureAssetUploaded` 里一次。
// ============================================================

#include "Nanite/NaniteRenderer.h"

#include "Core/CVar.h"
#include "Core/Log.h"

// 【§14.8 任务 12】合并几何的一次性来源。只在本 .cpp include：`NaniteRenderer.h` 只用前置声明，
// 这样模块门面不把 MeshBatcher 的依赖（Scene/GPUScene/RHI）带给所有使用者。
// §14.3 的禁令是"不得依赖它的**运行时状态**"——本文件只把 `const MeshBatcher&` 当参数读一次，
// 不持有指针、不在每帧回读它的内部表（与 `LumenSDF::Step(cmd, batcher)` 同款口径）。
#include "Pipeline/MeshBatcher.h"

// 【§14.8 任务 13】相机的真实定义（view-proj / 世界坐标）。头文件里只有前置声明。
#include "Pipeline/Camera.h"

#include <algorithm>   // std::sort（实例剔除读回：GPU 原子压缩列表的顺序不定，比较前排序）
#include <cstdio>      // std::snprintf（实例剔除读回的 first= 样本串）
#include <cstring>     // 【任务 15】std::memcpy（三阶段读数整块读回）
#include <vector>      // 任务 12：SoA 转换的临时数组（positions / normals / uvs）

// CVar: Nanite 独立开关（§14.4 的"配置"层，默认 0）。
// 与 `r.Decal.Project` 同风格（DeferredPipeline_FrameGraph.cpp:40）。它只是**配置载体**：
// 真值在 `NaniteSettings::enabled`，由 cfg 键 `nanite_enable` 与面板共同写入。
static he::CVar<int> cvNaniteEnable("r.Nanite.Enable", 0,
    "Nanite 虚拟几何模块（§14.8 任务 3）：0=关闭（默认，帧图与转储与今天逐位相同），1=开启");

// CVar: 任务 3 的假簇数量（默认 6）。与 cfg 键 `nanite_fake_clusters` 同含义，
// 只作为**启动默认**；运行期真值在 `NaniteSettings::fakeClusters`。
static he::CVar<int> cvNaniteFakeClusters("r.Nanite.FakeClusters", 6,
    "Nanite 任务 3 假簇数量（1 个实例、N 个簇；验证'计数为 k ⇒ 恰好画 k 次'）");

// CVar: 任务 13 的合成实例网格条数（默认 64，= kNaniteDefaultTestInstances）。与 cfg 键
// `nanite_instance_test_count` 同含义，只作为**启动默认**；运行期真值在 `NaniteSettings::instanceTestCount`。
static he::CVar<int> cvNaniteInstanceTestCount("r.Nanite.InstanceTestCount",
    (int)he::render::kNaniteDefaultTestInstances,
    "Nanite 任务 13 合成实例网格条数（视锥剔除的 GPU vs CPU 逐项对照样本）");

namespace he::render {

bool NaniteRenderer::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    // 任务 12：重新初始化 ⇒ 资产门闩复位（旧资产缓冲已在 Shutdown/Scene::Initialize 里作废）
    m_AssetUploaded = false;

    // 配置层 → 真值：只在启动时读一次 CVar 作为默认值。之后 cfg 与面板都可以覆盖它，
    // 而 CVar 不再每帧回写（否则面板的勾选会被控制台默认值每帧抹掉）。
    m_Settings.enabled = (cvNaniteEnable.Get() != 0);
    if (cvNaniteFakeClusters.Get() > 0)
        m_Settings.fakeClusters = (u32)cvNaniteFakeClusters.Get();
    // 【任务 13】合成实例网格条数：允许 0（0 = 不生成样本，该 pass 仍注册但派发 0 线程）
    if (cvNaniteInstanceTestCount.Get() >= 0)
        m_Settings.instanceTestCount = (u32)cvNaniteInstanceTestCount.Get();

    // 各段生命周期。`NaniteCull` 必须先建（它持有"已光栅化簇计数缓冲"，绘制端要引用它）。
    const bool sceneOk  = m_Scene.Initialize(device, width, height);
    const bool uploadOk = m_Upload.Initialize(device, width, height);
    const bool cullOk   = m_Cull.Initialize(device, width, height);
    const bool rasterOk = m_Raster.Initialize(device, width, height,
                                              m_Cull.GetRasterCountBuffer());

    // "骨架就绪"判据：设备有效且四段都建起来了。任务 3 起剔除/绘制段还要求
    // 自持缓冲与 PSO 真正建成（IsReady），否则开启档不会注册任何 pass。
    m_Ready = (m_Device != nullptr) && sceneOk && uploadOk
              && m_Cull.IsReady() && m_Raster.IsReady();

    HE_CORE_INFO("NaniteRenderer: 初始化完成（ready={}，开关默认={}，假簇数={}，合成实例数={}，档位={}）—— "
                 "模块自持「计数 → 间接绘制」链（任务 3 的假簇链 + 任务 16 的可见簇链，"
                 "默认由可见簇列表驱动绘制）",
                 m_Ready, m_Settings.enabled ? 1 : 0, m_Settings.fakeClusters,
                 m_Settings.instanceTestCount,
                 m_Settings.rasterMode == NaniteRasterMode::Hybrid ? "混合光栅" : "软光栅");

    // ── 任务 5：一次性启动日志 —— 打印 objectIndex 分区表与本次容量，便于人工核对 ──
    // 只打一行、只在启动时打（不是每帧），因此关闭档的每帧开销与日志都与基线一致。
    HE_CORE_INFO("NaniteRenderer: objectIndex 分区 = 普通段[{}, {}) 容量 {} | Nanite 段[{}, {}) 容量 {} | "
                 "哨兵 {} | MRT7(gb_lightmapkey) 页号 float16 精确上限 {}",
                 kNormalObjectIndexBegin, kNaniteObjectIndexBegin, kNormalObjectIndexCapacity,
                 kNaniteObjectIndexBegin, kObjectIndexTotalCapacity, kNaniteObjectIndexCapacity,
                 kInvalidObjectIndex, kLightmapKeyExactObjectIndexLimit);
    return m_Ready;
}

void NaniteRenderer::Shutdown() {
    // 四段自持资源必须先于本类清指针之前释放（绘制端引用了剔除段的计数缓冲，
    // 故先释放绘制端再释放剔除段）
    m_Raster.Shutdown();
    m_Cull.Shutdown();
    m_Upload.Shutdown();
    m_Scene.Shutdown();

    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
    m_Ready  = false;
    // 【故意不重置 m_Settings】样例在 `DeferredPipeline::Shutdown()` **之后**才回写 cfg，
    // 清掉真值会让 `nanite_enable` / `nanite_fake_clusters` 的往返有损。
}

void NaniteRenderer::Resize(u32 width, u32 height) {
    m_Width  = width;
    m_Height = height;
    m_Scene.OnResize(width, height);
    m_Upload.OnResize(width, height);
    m_Cull.OnResize(width, height);
    m_Raster.OnResize(width, height);
}

void NaniteRenderer::AddPasses(RenderGraph& rg, const NaniteGBufferHandles& gb,
                               const CameraData& camera) {
    // 门控在调用方（DeferredPipeline_FrameGraph.cpp）已经判过一次；这里再判一次是兜底，
    // 保证"关闭 ⇒ 本模块一个 pass 都不注册"这条不变式不依赖调用方的正确性。
    if (!m_Settings.enabled || !m_Ready) return;

    // 唯一真值 → 模块内部：把本帧的假簇数量交给剔除段（cfg/面板只写 NaniteSettings）
    m_Cull.SetFakeClusterCount(m_Settings.fakeClusters);
    // 【任务 16】绘制容量（cfg `nanite_draw_capacity`；0 ⇒ 容量上界）。它同时决定
    //   "间接命令写多少条"与"`DrawIndexedIndirectCount` 的 maxDrawCount"，两处同一个数。
    m_Cull.SetDrawCapacity(m_Settings.drawCapacity);

    // ── 【任务 16】定"谁来画"（每帧一次，两条链互斥）──
    // 可见链可用 ⇔ 资产 + BVH 已入库（那时 Phase 2/3 才会派发、才会写出命令）。
    // 【为什么"实例数为 0"不算退化】0 实例正是"零可见簇 ⇒ 零绘制"的边界场景，必须走可见链
    //   （走假簇链会画出 6 条，边界验收就失去意义 —— 见 §14.8 任务 16 的验收③）。
    m_DrawFromFakeChain = m_Settings.fakeChain || !m_Cull.IsClusterBVHReady();

    // ── 【任务 13/15】三阶段剔除的每帧输入：视锥 / 合成实例表 / 像素焦距 / 屏幕尺寸 ──
    // 【输入】本帧的 view-proj 与相机（`camera`）。模块据此提取 6 平面、生成**合成实例网格**
    //   （来源与坐标系见 `NaniteCull::SetCullChainFrame` 的注释：在 NDC 摆网格再反投影到世界空间
    //   —— 它们**不是**场景实例，本任务还没有"场景 → 模块实例表"的接入点，这里只验证
    //   "GPU 与 CPU 参考逐项一致"这条可判定的等价性），并用 **fov + 屏幕高**算 Phase 3 的像素焦距。
    // 【pass 注册点不在本函数】三阶段链（Phase 1 → Phase 2/3）必须在 `GB_Clear` **之后**注册
    //   （Phase 2 要采样本帧深度建出来的 Hi-Z 金字塔）⇒ 见 `AddPostGBufferPasses`。
    m_Cull.SetCullChainFrame(camera.GetViewProjMatrix(), camera.position,
                             m_Width, m_Height, camera.fov,
                             m_Settings.instanceTestCount);

    // ── 【任务 18】软光栅两趟的 push constant（本帧算一次；执行期推给 GPU）──
    // 【view-proj 拆成 4 个"行"】与任务 15 同一口径：`vpRows[r*4+c] = viewProj[c][r]`
    //   （glm 列主序的 row r），shader 侧用 4 次显式点积 ⇒ 两侧乘的是同一个表达式。
    {
        const float4x4& vp = camera.GetViewProjMatrix();
        const float* m = &vp[0][0];
        for (u32 row = 0u; row < 4u; ++row) {
            for (u32 col = 0u; col < 4u; ++col) {
                m_SoftParams.vpRows[row * 4u + col] = m[col * 4u + row];
            }
        }
        m_SoftParams.screenWidth   = m_Width;
        m_SoftParams.screenHeight  = m_Height;
        m_SoftParams.maxTriangles  = m_Settings.softMaxTriangles;
        m_SoftParams.instanceCount = std::min(m_Settings.instanceTestCount,
                                              NaniteCull::MaxBVHInstances());
        m_SoftParams.meshMaxExtent = m_MeshMaxExtent;
        m_SoftParams.depthKeyEpsilon = 0.0f;   // 两趟的等值复检实测严格逐位相等即可
    }
    // 软光栅资源懒建（不注册 pass；只保证执行期资源就绪）。资产未入库时它内部直接返回。
    EnsureSoftRasterReady(gb);

    // ── Nanite_Cull：compute 逐簇写间接命令 + 原子累加计数（任务 3 的**假簇链**）──
    // writes = {gbDepth, gbWorldPos} 是复刻 `GB_Clear` 的 WAW 声明（§14.5），
    // 本 pass 并不真的写它们；它真正的输出是模块自持的命令/计数缓冲（不走帧图资源）。
    // 【任务 15】本 pass 的三个每帧重置（命令计数 / 光栅化簇计数 / 哨兵填充）已全部改成命令缓冲内
    //   的拷贝（源是常驻 0 与常驻 0xFF 的 TransferSrc 缓冲）⇒ 不再有"主机写 vs 派发"的竞态。
    // 【任务 16 的改动：`Nanite_Raster` **不再是一个独立 pass**】
    //   绘制必须"紧跟产出命令的那次派发"，而帧图对两个零资源 pass 的排序**不可依赖**
    //   （`RenderGraph::TopologicalSort` 对 inDegree=0 的 pass 按 LIFO 处理 ⇒ 注册顺序 ≠ 执行
    //   顺序；任务 15 已经为 Phase 1→Phase 2 踩过这条）。因此把绘制录在**同一个 pass 体**内，
    //   顺序由 `NaniteRaster::RecordRasterPass` 开头的 `Compute → DrawIndirect` 屏障显式给出：
    //     · 假簇链是绘制来源 ⇒ 本 pass 里 `RecordCullPass` 之后紧接着录制绘制；
    //     · 可见链是绘制来源 ⇒ 本 pass 只算不画（绘制录在 `Nanite_CullChain3` 体内）。
    //   代价（如实）：开启档 pass 数 15 → **14**（少一个 `Nanite_Raster`）；判据 ⑥b 只要求
    //   "既有 12 个 pass 的集合与顺序一字不变"，模块自己的 pass 组合不在此约束内（任务 15
    //   也做过同类合并：16 → 15）。
    const bool drawFromFakeChain = m_DrawFromFakeChain;   // 按值捕获：帧图执行在注册之后
    rg.AddPass("Nanite_Cull",
        {},
        {RG_WRITE(gb.depth), RG_WRITE(gb.worldPos)},
        [this, drawFromFakeChain](rhi::IRHICommandList* cmd) {
            m_Cull.RecordCullPass(cmd);
            if (drawFromFakeChain) {
                // 假簇链的绘制端点：消费 `Nanite_Cull` 刚写出的命令 + 计数
                m_Raster.RecordRasterPass(cmd,
                                          m_Cull.GetIndirectCmdBuffer(),
                                          m_Cull.GetCountBuffer(),
                                          m_Cull.GetMaxFakeClusters());
            }
        });

    // ── §14.8 任务 6：Nanite_MeshTest（最小 mesh PSO 通道）──    // 【为什么注册在**这一处**（GBuffer 之前的第一处挂钩），而不是 GBuffer 之后那一处】
    //   1. 本 pass **完全不碰 GBuffer**：它画进模块自建的 1×1 R8 小目标、写模块自持的计数缓冲，
    //      因此对 GBuffer 没有任何读写依赖 —— 第二处挂钩（`AddPostGBufferPasses`）的语义是
    //      "必须在 GBuffer 几何段之后、Lighting 之前注册的那些 pass"（任务 4 的 UAV 自证），
    //      把不碰 GBuffer 的 pass 放进去只会让那处挂钩的语义变模糊。
    //   2. 三处模块私有 pass（Cull / Raster / MeshTest）集中在同一个注册点，便于对照与回退。
    // 【为什么不声明 gbDepth/gbWorldPos 的那组假 WAW】那组声明是 `GB_Clear` 写入者之间的排序
    //   契约（§14.5）。本 pass 没有任何帧图资源，声明空 reads/writes 反而是**最保守**的选择：
    //   `RenderGraph::CullDeadPasses` 明确不裁剪 `writes.empty()` 的 pass，而
    //   `TopologicalSort` 把它放在所有 inDegree=0 的 pass 之前 —— 也就是说它既不改变任何既有
    //   pass 之间的相对顺序，也不会被裁剪（既有 pass 顺序逐位不变）。
    // 【相对顺序】它注册在 `Nanite_Cull`/`Nanite_Raster` 之后，既有 12 个 pass 的注册位置不变。
    // 【门控】`meshTest`（默认 0）**且**设备支持 `VK_EXT_mesh_shader`；两者任一不满足就
    //   一个 pass 都不注册（关闭档的 pass 集合与今天逐位一致）。
    if (m_Settings.meshTest && m_Raster.IsMeshTestSupported()) {
        rg.AddPass("Nanite_MeshTest",
            {},
            {},
            [this](rhi::IRHICommandList* cmd) {
                m_Raster.RecordMeshTestPass(cmd);
            });
    }
}

void NaniteRenderer::AddPostGBufferPasses(RenderGraph& rg, const NaniteGBufferHandles& gb,
                                          const NaniteHiZSource& hiz) {
    // 门控在调用方（DeferredPipeline_FrameGraph.cpp）已经判过一次；这里再判一次是兜底，
    // 保证"关闭 ⇒ 本模块一个 pass 都不注册"这条不变式不依赖调用方的正确性。
    // 【与 AddPasses 是同一个真值】不是新门控：`enabled`（独立开关）+ `IsReady()`（模块就绪）。
    if (!m_Settings.enabled || !m_Ready) return;

    // ── 【任务 15】Nanite_CullChain3：Phase 1 → Phase 2（BVH + Hi-Z）→ Phase 3（LOD 选择）──
    // 【reads = {gbDepth}】**真的**读：Phase 2 的 Hi-Z 金字塔由本帧深度下采样得到（复用
    //   `GPUCulling::BuildHiZPyramid`）。这条读同时给帧图一条 RAW 依赖，把它定序在 `GB_Clear`
    //   之后（`AddPasses` 那一处注册点会被排到 GB_Clear **之前** ⇒ 那个位置拿不到本帧深度）。
    // 【writes 为空】本 pass 只写模块自持缓冲 + Hi-Z 金字塔纹理；Hi-Z 纹理的布局转换由 pass
    //   自己发（整图 GENERAL ↔ 只读），**不** import 进帧图 —— 因为既有 `HiZ_Build`（SSR 档）
    //   也在同一张纹理上做无声明的存储写入，让帧图只跟踪其中一条会与另一条的真实布局打架。
    // 【三段顺序】不靠帧图，靠同一 pass 体内命令缓冲的屏障（见 `NaniteCull::RecordCullChainPass`）。
    // 【回调】**执行期**才取纹理与构建函数：第 1 帧构建期纹理还不存在，且尺寸变化会重建它。
    const bool drawFromVisibleChain = !m_DrawFromFakeChain;   // 按值捕获（见 `Nanite_Cull` 的说明）
    // ── 【任务 18】软光栅的写入声明 ──
    // 【为什么声明这 4 张颜色为 UAV 写】任务 18 起模块是 GBuffer 段的几何写入者：帧图据此
    //   ①把本 pass 排在 `GB_Clear`（写这些资源）之后（WAW）、②把 Lighting/SSAO 等读 albedo/
    //   normal/worldPos 的 pass 排在本 pass 之后（RAW）、③在本 pass 前插入
    //   `RenderTarget → UnorderedAccess` 的转换（pass 体内还会补一条 ComputeShader 阶段的显式屏障，
    //   因为帧图推导的 dstStage 是保守映射，不含 ComputeShader —— 任务 4 的教训）。
    // 【为什么不声明深度】深度由本 pass 体内的"深度解析"pass 作为**附件**写，且 RHI 的 render pass
    //   结束时会把它还原成 READ_ONLY（与帧图在 `GB_Clear` 之后记录的模型一致）⇒ 不需要也不应该
    //   在帧图里再声明一次（声明成 Write 会把深度转成 ATTACHMENT 布局，破坏 Hi-Z 对本帧深度的采样）。
    const bool softRasterOn = m_Settings.softRaster;
    std::vector<PassResource> cullWrites;
    if (softRasterOn) {
        cullWrites.push_back({gb.albedo,      ResourceAccess::UAV});
        cullWrites.push_back({gb.normal,      ResourceAccess::UAV});
        cullWrites.push_back({gb.worldPos,    ResourceAccess::UAV});
        cullWrites.push_back({gb.lightmapKey, ResourceAccess::UAV});
    }
    rg.AddPass("Nanite_CullChain3",
        {{gb.depth, ResourceAccess::Read}},
        std::move(cullWrites),
        [this, hiz, drawFromVisibleChain, softRasterOn, gb](rhi::IRHICommandList* cmd) {
            rhi::IRHITexture* hizTexture = hiz.texture ? hiz.texture() : nullptr;
            rhi::IRHITexture* depthTex   = hiz.depth ? hiz.depth() : nullptr;
            m_Cull.RecordCullChainPass(cmd, hizTexture, depthTex, m_Settings.hiz);
            // 【任务 16】可见链是绘制来源 ⇒ 在**同一个 pass 体**内紧接着录制间接绘制：
            //   命令与绘制计数刚由上面的派发写出，`RecordRasterPass` 开头的
            //   `Compute → DrawIndirect` 屏障把可见性定序到绘制之前。
            //   `maxDrawCount` = 间接命令缓冲容量（CPU 侧把绘制容量钳到它以内 ⇒ 恒不越界）。
            if (drawFromVisibleChain) {
                m_Raster.RecordRasterPass(cmd,
                                          m_Cull.GetIndirectDrawBuffer(),
                                          m_Cull.GetDrawCountBuffer(),
                                          m_Cull.GetMaxIndirectDraws());
            }

            // ── 【任务 18】软光栅三趟（深度键 → 写 GBuffer → 深度解析）──
            // 【为什么录在同一个 pass 体内】它必须排在"本帧的剔除链"之后（要读它写出的可见簇
            //   列表与计数），而帧图无法为两个"零帧图资源"的模块 pass 表达这条顺序（inDegree=0
            //   的 pass 按 LIFO 处理 —— 任务 15/16 的教训）。录进同一个 pass 体 + 命令缓冲里的
            //   显式屏障，是唯一稳的写法（任务 16 的间接绘制同理）。
            if (!softRasterOn) return;
            const NaniteScene::AssetBuffers& asset = m_Scene.GetAssetBuffers();
            if (!asset.clusters || !asset.vertices || !asset.indices) return;   // 资产未入库

            NaniteRaster::GBufferTargets targets;
            targets.albedo      = gb.colorTextures[0];
            targets.normal      = gb.colorTextures[1];
            targets.emissive    = gb.colorTextures[2];
            targets.velocity    = gb.colorTextures[3];
            targets.worldPos    = gb.colorTextures[4];
            targets.disneyA     = gb.colorTextures[5];
            targets.disneyB     = gb.colorTextures[6];
            targets.lightmapKey = gb.colorTextures[7];
            targets.depth       = gb.depthTexture;
            if (!targets.HasSoftRasterTargets()) return;

            NaniteRaster::AssetViews views;
            views.clusters = asset.clusters.get();
            views.vertices = asset.vertices.get();
            views.indices  = asset.indices.get();
            views.header   = asset.header.get();

            m_Raster.RecordSoftRasterPass(cmd, targets, views, m_SoftParams,
                                          m_Cull.GetVisibleClusterBuffer(),
                                          m_Cull.GetVisibleClusterCountBuffer(),
                                          m_Cull.GetInstanceBuffer(),
                                          m_Cull.GetBVHVisibleCapacity(),
                                          1.0f /* 深度解析的清屏值：远平面 */);
        });

    // 任务 4 的 UAV 自证通道：默认关闭（`nanite_test_write=0`）⇒ 这里什么都不注册。
    if (!m_Settings.testWrite) return;

    // 需要 albedo 的帧图句柄（排序 + 读写状态）与纹理对象（UAV 绑定 + 分辨率）两者都在。
    if (gb.albedo == kInvalidHandle || !gb.albedoTexture) return;

    rhi::IRHITexture* albedo = gb.albedoTexture;   // 按值捕获：帧图执行发生在 BuildFrameGraph 返回之后

    // ── Nanite_TestWrite：compute 用 RWTexture2D 直接写既有 GBuffer albedo ──
    // 【writes 用 UAV 而不是 Write】`ResourceAccess::Write` 映射到 `RenderTarget`（颜色附件布局），
    // 而本 pass 是**存储图像写入**，必须映射到 `UnorderedAccess`（VK_IMAGE_LAYOUT_GENERAL）——
    // 见 RenderGraph::AccessToState。这样帧图会插入 `颜色附件 → GENERAL`（本 pass 前）与
    // `GENERAL → ShaderResource`（Lighting 读 albedo 前）两条转换，这正是"同帧被 Lighting 读到"
    // 所依赖的排序；pass 内部还会补两条 ComputeShader 阶段的显式屏障（见 NaniteRaster）。
    // 【顺序约束】与 `GB_Clear` 同为 gbAlbedo 的写入者 ⇒ 帧图的 WAW 依赖天然把本 pass 排在
    // `GB_Clear` 之后（也正是"模块的写入不会被 GB_Clear 覆盖"的保证）。
    rg.AddPass("Nanite_TestWrite",
        {},
        {{gb.albedo, ResourceAccess::UAV}},
        [this, albedo](rhi::IRHICommandList* cmd) {
            m_Raster.RecordTestWritePass(cmd, albedo);
        });
}

// ============================================================
// §14.8 任务 12：资产构建 + 一次性上传 + 读回校验
//
// 门控与次数的口径写在 `NaniteRenderer.h` 的同名小节里，这里只留与代码逐句对应的短注释。
// 三步：① 读一次合并几何并转扁平 SoA；② RHI-free 的 `BuildNaniteAssetFromGeometry` 出字节镜像；
// ③ `NaniteScene::UploadPackedAsset` 建缓冲 + 上传 + 真实 GPU 读回逐字节比较（它打印那一行）。
// ============================================================
void NaniteRenderer::EnsureAssetUploaded(const MeshBatcher& batcher) {
    // 【关闭 / 未就绪 ⇒ 一个资源都不建】§14.2 不变式 1：关闭档不产生新的 GPU 资源、不打日志。
    if (!m_Settings.enabled || !m_Ready) return;
    // 【只做一次】门闩在 Initialize 时复位；此后无论这个函数被调用多少次都只走一次上传。
    if (m_AssetUploaded) return;
    m_AssetUploaded = true;

    // ── ① 合并几何只读一次（§14.3：MeshBatcher 只当一次性输入）──
    // 两个 getter 都是 `MeshBatcher.h:60-61` 上**已有**的 const 访问器 ⇒ 本任务没有为几何来源
    // 改过 `MeshBatcher` 一行。索引在合批时已经加过 baseVertex（`MeshBatcher.cpp:53`），
    // 所以它们直接就是合并顶点表的下标，调用方**不得**再加 `vertexOffset`。
    const std::vector<StaticVertex>& merged = batcher.GetMergedVertices();
    const std::vector<u32>&          indices = batcher.GetMergedIndices();
    if (indices.empty() || merged.empty()) {
        HE_CORE_WARN("NaniteRenderer: 合并几何为空（{} 顶点 / {} 索引）⇒ 跳过资产构建与上传；"
                     "本帧仍照常注册模块 pass（模块不写 GBuffer，画面不受影响）",
                     merged.size(), indices.size());
        return;
    }

    // ── ② SoA 转换：打包器只吃扁平数组（`StaticVertex` 是 AoS，且它属于 Scene 头）──
    // 这是一次性拷贝（O(V)），不是每帧路径；之后再不触碰 `batcher`。
    std::vector<float> positions(merged.size() * 3u);
    std::vector<float> normals(merged.size() * 3u);
    std::vector<float> uvs(merged.size() * 2u);
    for (usize i = 0; i < merged.size(); ++i) {
        positions[i * 3u + 0u] = merged[i].position.x;
        positions[i * 3u + 1u] = merged[i].position.y;
        positions[i * 3u + 2u] = merged[i].position.z;
        normals[i * 3u + 0u]   = merged[i].normal.x;
        normals[i * 3u + 1u]   = merged[i].normal.y;
        normals[i * 3u + 2u]   = merged[i].normal.z;
        uvs[i * 2u + 0u]       = merged[i].uv.x;
        uvs[i * 2u + 1u]       = merged[i].uv.y;
    }

    // ── ③ CPU 字节镜像（任务 9 的 DAG + 任务 10 的量化打包）──
    // 材质段本任务**留空**：合并几何不携带材质 ID，`NaniteClusterRecord::materialID` 逐簇解析
    // 属任务 19；这里传空 span ⇒ `materialCount = 0`（日志里的 `materials=0` 就是这个事实，
    // 不是失败）。伪造 ID 会掩盖"材质还没接上"这件事。
    NanitePackedAsset asset;
    if (!BuildNaniteAssetFromGeometry(positions, normals, uvs, indices, {}, asset)) {
        HE_CORE_ERROR("NaniteRenderer: 资产构建失败（{} 顶点 / {} 索引）—— 跳过上传，"
                      "不做部分上传；本帧仍照常注册模块 pass",
                      merged.size(), indices.size());
        return;
    }

    // ── ④ GPU 上传 + 读回校验（失败时 NaniteScene 会打错误行，成功时打验收行）──
    if (!m_Scene.UploadPackedAsset(asset)) {
        HE_CORE_ERROR("NaniteRenderer: 资产上传/读回校验失败（镜像 {} 字节）",
                      (unsigned long long)asset.bytes.size());
    }

    // ── ⑤ 【任务 14/15】按同一份簇记录构建 per-instance cluster BVH + 每簇 LOD 元数据 ──
    // 【为什么收在这里】簇记录（`asset.clusters`）与 LOD 段（`asset.lodOffsets`）的唯一产地就是
    //   本函数；构建器是 RHI-free 的 CPU 侧步骤（`BuildNaniteClusterBVH` /
    //   `BuildNaniteClusterLODInfo`），上传到模块自持缓冲由 `NaniteCull` 负责。
    // 【为什么在资产上传之后】两者互不依赖，但放在后面能让日志顺序与"先有资产、后有加速结构"
    //   的语义一致；构建失败 ⇒ `NaniteCull` 内的门闩保持关闭，`Nanite_CullChain3` 的 Phase 2/3
    //   直接跳过（不派发、读回打印 0），不会用半成品数据骗过验收。
    if (!m_Cull.SetClusterBVH(asset.clusters, asset.lodOffsets)) {
        HE_CORE_ERROR("NaniteRenderer: cluster BVH / LOD 元数据构建上传失败（簇 {}）—— "
                      "Nanite_CullChain3 的 Phase 2/3 本帧起跳过派发", (u32)asset.clusters.size());
    }

    // ── ⑥ 【任务 16】把"占位索引缓冲必须覆盖的索引位置上界"交给绘制端 ──
    // 【为什么用资产的索引总数】每个簇是索引段里的一个连续三角形区间 ⇒ 任意可见簇的
    //   `firstIndex + indexCount ≤ 索引总数`；绘制端按这个上界建占位索引缓冲，间接命令里的
    //   真实 firstIndex 就不会越界读索引（本设备未启用 `robustBufferAccess`）。
    // 【为什么在这里调用是安全的】上面 `UploadPackedAsset` 内部已经 `WaitIdle()` ⇒ 没有任何在飞
    //   的命令缓冲引用旧缓冲，替换/扩容不会造成 use-after-free；之后各帧只读新缓冲。
    m_Raster.SetPlaceholderIndexCapacity(asset.header.indexCount);

    // ── ⑦ 【任务 18】软光栅要的量化尺度：`meshMaxExtent`（整网格最大轴长）──
    // 【为什么由 CPU 传而不是 shader 从头部算】打包器已经把真值算在 `stats.meshMaxExtent`
    //   （与任务 9 的 DAG 哈希、任务 10 的顶点词同一个函数），直接用它 ⇒ 解码口径与编码口径
    //   同源；shader 侧从头部 bbox 现算会和它差一次浮点舍入（虽然理论上同值，但"同一份比特"
    //   才是可验证的口径）。
    m_MeshMaxExtent = asset.stats.meshMaxExtent;
}

// ============================================================
// §14.8 任务 18：软光栅的接线（让位 + 清屏 + 三趟派发 + 读数）
// ============================================================

void NaniteRenderer::EnsureSoftRasterReady(const NaniteGBufferHandles& gb) {
    // 【关闭 / 未就绪 / 未开软光栅 / 纹理不全 ⇒ 什么都不做】
    //   本函数**不注册 pass**，只保证执行期的资源就绪；真正的 pass 体在 `Nanite_CullChain3`
    //   与 `GB_Clear` 的 lambda 里（都按同一个开关门控）。
    if (!m_Settings.enabled || !m_Ready || !m_Settings.softRaster) return;
    if (!gb.HasFullGBufferTextures()) return;
    const NaniteScene::AssetBuffers& asset = m_Scene.GetAssetBuffers();
    if (!asset.clusters || !asset.vertices || !asset.indices) return;   // 资产未入库

    NaniteRaster::GBufferTargets targets;
    targets.albedo      = gb.colorTextures[0];
    targets.normal      = gb.colorTextures[1];
    targets.emissive    = gb.colorTextures[2];
    targets.velocity    = gb.colorTextures[3];
    targets.worldPos    = gb.colorTextures[4];
    targets.disneyA     = gb.colorTextures[5];
    targets.disneyB     = gb.colorTextures[6];
    targets.lightmapKey = gb.colorTextures[7];
    targets.depth       = gb.depthTexture;
    m_Raster.EnsureSoftRasterResources(targets);
}

void NaniteRenderer::RecordGBufferClearPass(rhi::IRHICommandList* cmd, const NaniteGBufferHandles& gb) {
    if (!m_Settings.enabled || !m_Ready || !m_Settings.softRaster) return;
    if (!gb.HasFullGBufferTextures()) {
        // 【一次性告警】纹理句柄不全 ⇒ 本帧无法清屏（不静默）
        static bool warned = false;
        if (!warned) {
            warned = true;
            HE_CORE_ERROR("NaniteRenderer: GBuffer 纹理句柄不全，模块无法清屏"
                          "（albedo={} normal={} emissive={} velocity={} worldPos={} disneyA={} disneyB={} "
                          "lightmapKey={} depth={}）",
                          (const void*)gb.colorTextures[0], (const void*)gb.colorTextures[1],
                          (const void*)gb.colorTextures[2], (const void*)gb.colorTextures[3],
                          (const void*)gb.colorTextures[4], (const void*)gb.colorTextures[5],
                          (const void*)gb.colorTextures[6], (const void*)gb.colorTextures[7],
                          (const void*)gb.depthTexture);
        }
        return;
    }

    NaniteRaster::GBufferTargets targets;
    targets.albedo      = gb.colorTextures[0];
    targets.normal      = gb.colorTextures[1];
    targets.emissive    = gb.colorTextures[2];
    targets.velocity    = gb.colorTextures[3];
    targets.worldPos    = gb.colorTextures[4];
    targets.disneyA     = gb.colorTextures[5];
    targets.disneyB     = gb.colorTextures[6];
    targets.lightmapKey = gb.colorTextures[7];
    targets.depth       = gb.depthTexture;
    // 【一次性诊断】确认清屏真的被录制（这是"模块接管写入"的第一步，值得留一行）
    static bool logged = false;
    if (!logged) {
        logged = true;
        HE_CORE_INFO("NaniteRenderer: 任务 18 清屏已接管（8×MRT UAV + ClearDepthStencil；"
                     "既有 GBufferRenderer::Render 本帧起让位）");
    }
    m_Raster.RecordGBufferClearPass(cmd, targets);
}

void NaniteRenderer::LogSoftRasterReadback() {
    // 关闭档 / 未就绪 / 未开软光栅：不打印（关闭档日志与基线逐字一致）
    if (!m_Settings.enabled || !m_Ready || !m_Settings.softRaster) return;
    m_Raster.LogSoftRasterReadback();
}

void NaniteRenderer::LogFakePipelineReadback() {
    // 关闭档（或未就绪）不打印：关闭档的日志与转储必须与基线逐位一致。
    if (!m_Settings.enabled || !m_Ready) return;
    // 【任务 16】只在**假簇链是绘制来源**时打印：默认档走可见簇链，那一行由
    //   `LogVisibleWiringReadback` 给出；两条都打印会让同一帧出现两个互相矛盾的"画了多少"。
    if (!m_DrawFromFakeChain) return;

    // X = 计数缓冲的值（GPU 原子累加"实际写入的命令条数"）
    u32 x = 0;
    if (auto* b = m_Cull.GetCountBuffer()) {
        if (void* p = b->Map()) { x = *static_cast<const u32*>(p); b->Unmap(); }
    }

    // Y = 间接命令缓冲里真正被写过的命令条数：CPU 每帧把整块填成 0xFFFFFFFF 哨兵，
    // 因此"字段合法的条目数"就是 GPU 实际写过的条数（与 X 互为独立证据）。
    u32 y = 0;
    if (auto* b = m_Cull.GetIndirectCmdBuffer()) {
        if (void* p = b->Map()) {
            const auto* cmds = static_cast<const NaniteIndirectCommand*>(p);
            for (u32 i = 0; i < kNaniteMaxFakeClusters; ++i) {
                if (cmds[i].indexCount == kNaniteFakeClusterIndexCount &&
                    cmds[i].instanceCount == 1u) {
                    ++y;
                }
            }
            b->Unmap();
        }
    }

    // Z = 绘制端"每个绘制恰好一次"的原子计数（`SV_PrimitiveID == 0`）
    const u32 z = m_Raster.ReadbackRasterCount();

    // 【恰好一行】任务 3 的验收出口：X == Y == Z == N
    HE_CORE_INFO("[Nanite] fake_clusters={} count_buffer={} indirect_cmds={} rasterized_clusters={}",
                 m_Settings.fakeClusters, x, y, z);
}

void NaniteRenderer::LogVisibleWiringReadback() {
    // 关闭档 / 未就绪：不打印（关闭档的日志必须与基线逐字一致）。
    if (!m_Settings.enabled || !m_Ready) return;

    // ── ① V：可见簇计数（剔除端 Phase 3 的原子计数；GPU 读回）──
    u32 visible = 0u;
    if (auto* b = m_Cull.GetVisibleClusterCountBuffer()) {
        if (void* p = b->Map()) { visible = *static_cast<const u32*>(p); b->Unmap(); }
    }

    // ── ② D：绘制计数（= `DrawIndexedIndirectCount` 用的 count；GPU 读回）──
    u32 drawCount = 0u;
    if (auto* b = m_Cull.GetDrawCountBuffer()) {
        if (void* p = b->Map()) { drawCount = *static_cast<const u32*>(p); b->Unmap(); }
    }
    // ── ③ T：因绘制容量不足而未写命令的簇数（截断自证；GPU 读回）──
    u32 stats[kNaniteCullStatsCapacity] = { 0u };
    if (auto* b = m_Cull.GetCullStatsBuffer()) {
        if (void* p = b->Map()) { std::memcpy(stats, p, sizeof(stats)); b->Unmap(); }
    }
    const u32 truncated = stats[kNaniteCullStatDrawTruncated];

    // ── ④ R：绘制端"每个绘制恰好一次"的原子计数（GPU 读回）──
    const u32 rasterized = m_Raster.ReadbackRasterCount();

    // ── ⑤ C：逐条核验间接命令缓冲（合法 + 与 CPU 参考逐字段一致）──
    // 【为什么读的是 [0, visible)】命令与可见簇引用同槽位写入（容量也相同）⇒ 有效条数就是
    //   可见簇计数；`visible` 超过缓冲容量时按容量截断（防御脏计数，绝不越界读）。
    const auto& drawRanges = m_Cull.GetClusterDrawRanges();
    const u32 clusterCount = m_Cull.GetBVHClusterCount();
    const u32 readable = (visible < kNaniteMaxIndirectDraws) ? visible : kNaniteMaxIndirectDraws;
    u32 indirectOk = 0u;
    u32 fieldMismatch = 0u;
    if (auto* b = m_Cull.GetIndirectDrawBuffer()) {
        if (void* p = b->Map()) {
            const auto* cmds = static_cast<const NaniteIndirectCommand*>(p);
            for (u32 i = 0u; i < readable; ++i) {
                const NaniteIndirectCommand& cmd = cmds[i];
                if (!NaniteIsIndirectCommandLegal(cmd, clusterCount)) continue;   // 坏命令/未写槽位
                const u32 cluster = cmd.firstInstance;
                if (cluster >= (u32)drawRanges.size()) { ++fieldMismatch; continue; }
                if (!NaniteIndirectCommandMatchesRange(cmd, drawRanges[cluster], cluster)) {
                    ++fieldMismatch;
                    continue;
                }
                ++indirectOk;
            }
            b->Unmap();
        }
    }

    // ── ⑥ CPU 参考：同一份输入（同一个视锥/实例表/BVH/LOD 元数据/焦距），同一条打包约定 ──
    // 【为什么再算一遍】它给出"应有"的可见簇集合与命令，是 ⑤ 的比对基准；这里与 `LogCull3Readback`
    //   的口径完全一致（同一帧、同一份比特）。dump 帧只跑一次，成本可忽略。
    std::vector<NaniteVisibleClusterRef> cpuVisible;
    (void)m_Cull.RunCullChainCPUReference(cpuVisible);
    u32 cpuTruncated = 0u;
    std::vector<NaniteIndirectCommand> cpuCommands(cpuVisible.size());
    const u32 cpuWritten = NanitePackVisibleIndirectCommands(
        cpuVisible.empty() ? nullptr : cpuVisible.data(), (u32)cpuVisible.size(),
        drawRanges.empty() ? nullptr : drawRanges.data(), (u32)drawRanges.size(),
        cpuCommands.empty() ? nullptr : cpuCommands.data(), (u32)cpuCommands.size(),
        &cpuTruncated);
    cpuCommands.resize(cpuWritten);

    // ── ⑦ 汇总判据 ──
    // `empty_draws` = 命令条数里"没产生任何片元"的条数（命令有效但几何被丢弃 ⇒ 空转）。
    const u32 emptyDraws = (visible > rasterized) ? (visible - rasterized) : 0u;
    // `mismatch` = 逐条字段不一致数 + 四个核心数的两两偏差（任一非 0 都说明接线有问题）。
    u32 mismatch = fieldMismatch;
    const auto absDiff = [](u32 a, u32 b) -> u32 { return (a > b) ? (a - b) : (b - a); };
    mismatch += absDiff(visible, indirectOk);
    mismatch += absDiff(visible, drawCount);
    mismatch += absDiff(drawCount, rasterized);

    // 【恰好一行】任务 16 的验收出口：正常档 V == C == D == R、E == 0、M == 0。
    HE_CORE_INFO("[Nanite] visible_wiring visible={} indirect_count={} draws={} rasterized={} "
                 "empty_draws={} mismatch={} src={} truncated={} max_draws={} cpu_cmds={} "
                 "placeholder_indices={}",
                 visible, indirectOk, drawCount, rasterized, emptyDraws, mismatch,
                 m_DrawFromFakeChain ? "fake" : "visible",
                 truncated, m_Cull.GetMaxIndirectDraws(), cpuWritten,
                 m_Raster.GetPlaceholderIndexCount());
}

void NaniteRenderer::LogCull3Readback() {
    // 关闭档 / 未就绪：不打印（关闭档的日志必须与基线逐位一致）。
    if (!m_Settings.enabled || !m_Ready) return;

    const u32 nodes = m_Cull.GetBVHNodeCount();
    const u32 depth = m_Cull.GetBVHDepth();
    const u32 capacity = m_Cull.GetBVHVisibleCapacity();
    const u32 hizMips = m_Cull.GetFrameHiZMipCount();
    const bool hizOn = (hizMips >= 2u);   // 层数 < 2 ⇒ shader 第一句就不测遮挡 ⇒ 等价于关闭

    // ── GPU 读数①：Phase 1 的可见实例计数（GPU 原子累加；任务 13 的那个计数器）──
    u32 gpuInstanceCount = 0u;
    if (auto* b = m_Cull.GetVisibleInstanceCountBuffer()) {
        if (void* p = b->Map()) { gpuInstanceCount = *static_cast<const u32*>(p); b->Unmap(); }
    }

    // ── GPU 读数②：可见实例列表的 [0, min(计数, 实例数)) —— 只取本帧条数以内的条目（防御脏计数）──
    const std::vector<u32>& cpuInstanceList = m_Cull.GetCpuVisibleInstances();
    const u32 cpuInstanceCount = (u32)cpuInstanceList.size();
    const u32 testCount = m_Cull.GetTestInstanceCount();
    std::vector<u32> gpuInstances;
    if (auto* b = m_Cull.GetVisibleInstanceBuffer()) {
        if (void* p = b->Map()) {
            const auto* list = static_cast<const u32*>(p);
            const u32 readable = (gpuInstanceCount < testCount) ? gpuInstanceCount : testCount;
            gpuInstances.reserve(readable);
            for (u32 i = 0u; i < readable; ++i) gpuInstances.push_back(list[i]);
            b->Unmap();
        }
    }
    // 【比较口径】GPU 用原子槽位压缩 ⇒ 顺序不定；CPU 参考是升序。排序后再逐项比较（集合等价）。
    std::sort(gpuInstances.begin(), gpuInstances.end());
    u32 instanceMismatch = (gpuInstanceCount > cpuInstanceCount)
                        ? (gpuInstanceCount - cpuInstanceCount)
                        : (cpuInstanceCount - gpuInstanceCount);
    const u32 commonInstances = std::min<u32>((u32)gpuInstances.size(), cpuInstanceCount);
    for (u32 i = 0u; i < commonInstances; ++i) {
        if (gpuInstances[i] != cpuInstanceList[i]) ++instanceMismatch;
    }

    // ── GPU 读数③：三阶段读数（通过视锥 / 被遮挡 / 级直方图 / 访问节点数）──
    u32 stats[kNaniteCullStatsCapacity] = { 0u };
    if (auto* b = m_Cull.GetCullStatsBuffer()) {
        if (void* p = b->Map()) {
            std::memcpy(stats, p, sizeof(stats));
            b->Unmap();
        }
    }
    const u32 gpuFrustumPass = stats[kNaniteCullStatFrustumPass];
    const u32 gpuOccluded    = stats[kNaniteCullStatOccluded];
    const u32 gpuVisited     = stats[kNaniteCullStatVisited];
    // Phase 2 的输出 = 通过视锥 − 被遮挡（**不截断的计数**；饱和减法防御脏数据）
    const u32 gpuPhase2 = (gpuFrustumPass > gpuOccluded) ? (gpuFrustumPass - gpuOccluded) : 0u;

    // ── GPU 读数④：Phase 3 之后（= 最终）的可见簇计数与列表 ──
    u32 gpuClusterCount = 0u;
    if (auto* b = m_Cull.GetVisibleClusterCountBuffer()) {
        if (void* p = b->Map()) { gpuClusterCount = *static_cast<const u32*>(p); b->Unmap(); }
    }
    const u32 gpuReadable = (gpuClusterCount < capacity) ? gpuClusterCount : capacity;
    std::vector<NaniteVisibleClusterRef> gpuVisible;
    gpuVisible.reserve(gpuReadable);
    if (auto* b = m_Cull.GetVisibleClusterBuffer()) {
        if (void* p = b->Map()) {
            const auto* list = static_cast<const NaniteVisibleClusterRef*>(p);
            for (u32 i = 0u; i < gpuReadable; ++i) gpuVisible.push_back(list[i]);
            b->Unmap();
        }
    }

    // ── CPU 参考（同帧同输入；Hi-Z 恒关闭，理由见头文件）──
    std::vector<NaniteVisibleClusterRef> cpuVisible;
    const NaniteClusterBVHTraversalStats cpuStats = m_Cull.RunCullChainCPUReference(cpuVisible);

    const auto lessRef = [](const NaniteVisibleClusterRef& a, const NaniteVisibleClusterRef& b) {
        if (a.instance != b.instance) return a.instance < b.instance;
        return a.cluster < b.cluster;
    };
    std::sort(gpuVisible.begin(), gpuVisible.end(), lessRef);
    std::sort(cpuVisible.begin(), cpuVisible.end(), lessRef);

    // ── 真正的**集合差**（两遍归并），而不是"按下标比 + 条数差" ──
    // 两个列表都已按 (instance, cluster) 升序 ⇒ 一次归并即可得到 |gpu \ cpu| 与 |cpu \ gpu|。
    usize gi = 0u, ci = 0u;
    u32 extraGpu = 0u;        // gpu \ cpu：Hi-Z 只能"少"不能"多" ⇒ 这一项必须恒为 0
    u32 missingFromGpu = 0u;  // cpu \ gpu：Hi-Z 打开时就是"被遮挡剔除"的那批
    while (gi < gpuVisible.size() || ci < cpuVisible.size()) {
        const bool takeGpu = (ci >= cpuVisible.size())
                          || (gi < gpuVisible.size()
                              && ((gpuVisible[gi].instance < cpuVisible[ci].instance)
                                  || (gpuVisible[gi].instance == cpuVisible[ci].instance
                                      && gpuVisible[gi].cluster < cpuVisible[ci].cluster)));
        if (takeGpu) { ++extraGpu; ++gi; continue; }
        const bool takeCpu = (gi >= gpuVisible.size())
                          || ((cpuVisible[ci].instance < gpuVisible[gi].instance)
                              || (cpuVisible[ci].instance == gpuVisible[gi].instance
                                  && cpuVisible[ci].cluster < gpuVisible[gi].cluster));
        if (takeCpu) { ++missingFromGpu; ++ci; continue; }
        ++gi;
        ++ci;   // 两边都有 ⇒ 相同元素
    }
    const u32 mismatch = extraGpu + missingFromGpu;

    // ── 差异的**量化解释**：被遮挡剔除的那批簇各自落在金字塔的哪一层 ──
    // 【为什么这能解释差异】Hi-Z 遮挡测试作用在 GPU 侧；CPU 拿不到金字塔的逐 texel 内容
    //   （RHI 的 `CopyTextureToBuffer` 只读 mip0，而 `BuildHiZPyramid` 从不写 mip0）⇒ CPU 参考
    //   恒为"Hi-Z 关闭"口径，两者之差**只可能**来自遮挡剔除。把差集按"投影盒大小 → 选层"
    //   归类之后，差异就落在"这些簇的投影盒越大、采样层越深、覆盖到的遮挡物越多"这条可核对的
    //   解释上（`extra_gpu` 必须为 0：Hi-Z 只能少不能多）。
    u32 occludedMip[kNaniteMaxHiZMips] = { 0u };
    u32 projectedOffscreen = 0u;
    const u32 missingCheck = m_Cull.CountOccludedClustersByMip(cpuVisible, gpuVisible,
                                                              occludedMip, &projectedOffscreen);
    if (missingCheck != missingFromGpu) {
        // 两条独立的差集统计不一致 ⇒ 说明列表/口径出了问题（不静默：打到日志里）
        HE_CORE_WARN("[Nanite] cull3 差异统计不自洽：归并差 {} vs 选层统计 {}",
                     missingFromGpu, missingCheck);
    }
    (void)projectedOffscreen;   // 正常情况下为 0（在屏幕上才可能被遮挡剔除）

    // ── first=<前若干个可见实例下标>：可核对的样本（排序后 ⇒ 跨运行可比）──
    char first[96];
    if (gpuInstances.empty()) {
        std::snprintf(first, sizeof(first), "-");
    } else {
        int written = 0;
        const u32 sample = std::min<u32>((u32)gpuInstances.size(), 6u);
        for (u32 i = 0u; i < sample && written >= 0 && (usize)written < sizeof(first); ++i) {
            written += std::snprintf(first + written, sizeof(first) - (usize)written,
                                     (i == 0u) ? "%u" : ",%u", gpuInstances[i]);
        }
        first[sizeof(first) - 1u] = '\0';
    }

    // 【恰好一行】任务 15 的验收出口。三个 phase、Hi-Z 档位、GPU/CPU 逐项差异、LOD 级分布、
    // 以及"被遮挡那批簇的选层分布"（差异的量化解释）都在这一行里。
    HE_CORE_INFO("[Nanite] cull3 phase1={} phase2={} phase3={} hiz={} gpu_clusters={} "
                 "cpu_clusters={} mismatch={} lod=[{},{},{},{},{},{},{},{}] "
                 "cpu_lod=[{},{},{},{},{},{},{},{}] extra_gpu={} occluded={} frustum={} "
                 "occl_mip=[{},{},{},{},{},{},{},{}] inst_mismatch={} nodes={} depth={} visited={} "
                 "hiz_req={} hiz_mips={} first={}",
                 gpuInstanceCount, gpuPhase2, gpuClusterCount, hizOn ? "on" : "off",
                 gpuClusterCount, cpuStats.visibleClusters, mismatch,
                 stats[kNaniteCullStatLodBase + 0u], stats[kNaniteCullStatLodBase + 1u],
                 stats[kNaniteCullStatLodBase + 2u], stats[kNaniteCullStatLodBase + 3u],
                 stats[kNaniteCullStatLodBase + 4u], stats[kNaniteCullStatLodBase + 5u],
                 stats[kNaniteCullStatLodBase + 6u], stats[kNaniteCullStatLodBase + 7u],
                 cpuStats.lodHistogram[0], cpuStats.lodHistogram[1],
                 cpuStats.lodHistogram[2], cpuStats.lodHistogram[3],
                 cpuStats.lodHistogram[4], cpuStats.lodHistogram[5],
                 cpuStats.lodHistogram[6], cpuStats.lodHistogram[7],
                 extraGpu, gpuOccluded, gpuFrustumPass,
                 occludedMip[0], occludedMip[1], occludedMip[2], occludedMip[3],
                 occludedMip[4], occludedMip[5], occludedMip[6], occludedMip[7],
                 instanceMismatch, nodes, depth, gpuVisited,
                 m_Cull.GetFrameHiZRequested() ? 1 : 0, hizMips, first);
}

void NaniteRenderer::LogMeshTestReadback() {    // 关闭档 / 未就绪 / 未开 mesh 自证：不打印（关闭档与"只开 enabled"的日志必须与基线一致）。
    if (!m_Settings.enabled || !m_Ready) return;
    if (!m_Settings.meshTest) return;

    // 两个数都是**真实 GPU 读回**（样例在 dump 帧已 `WaitIdle()`，与白炉探针同一套同步做法）：
    //   n = 片元原子计数（被光栅化的 mesh 图元数），v = 1×1 R8 目标的像素值。
    const u32 n = m_Raster.ReadbackMeshTestOutputs();
    const u32 v = m_Raster.ReadbackMeshTestTargetMax();

    // 【恰好一行】任务 6 的验收出口：mesh_pso=ok 且 n >= 1 / v > 0 即"输出非空"。
    HE_CORE_INFO("[Nanite] mesh_pso={} meshlet_outputs={} target_max={}",
                 m_Raster.IsMeshTestPSOReady() ? "ok" : "fail", n, v);
}

} // namespace he::render
