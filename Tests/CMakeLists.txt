# ============================================================
# Tests — doctest 单元测试目标
#
# 所有测试用例文件集中在这里，链接被测的 HugEngine 模块。
# ============================================================

add_executable(HugEngineTests
    Main.cpp
    TestSceneBuilder.cpp
    TestPrimaryCamera.cpp
    # 2026-09 画质阶段 0 第②项：TAA 抖动接进投影矩阵的符号/量级断言（纯 CPU，只需
    # `Pipeline/Camera.h` 的包含路径；它只依赖 Core/Types.h 与 Math/）。
    TestCameraJitter.cpp
    # 渲染线程化方案 T0.1：线程归属（`RHI/ThreadAffinity.h`，头文件实现）的三态断言——
    # "未 Claim 不设限 / Claim 后只有认领线程放行 / Release 后重新不设限"。
    TestThreadAffinity.cpp
    # 阶段 0 T0.5：三态渲染线程模式（默认单线程 / 判据真值表 / 解析口径 / 非法输入不静默退回）。
    TestRenderThreadingMode.cpp
    # 阶段 0 T0.3/T0.4：命令队列、渲染线程壳、帧票据与背压。这三个 .cpp 是 **RHI-free** 的，
    # 直接编译进测试目标（与 NaniteUpload.cpp 同一手法）—— 一旦有人把 RHI 依赖塞进
    # Engine/Render/Threading/*，本目标会立刻编译失败。
    TestRenderCommandQueue.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Render/Threading/RenderCommandQueue.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Render/Threading/RenderThread.cpp
    # 阶段 0 T0.6：RHI 命令流记录端契约（`RHI/RHICommandList.h` 是纯头文件实现，
    # 只需包含路径，不链接 HugEngineRHI）。
    TestRHICommandList.cpp
    # 阶段 0 T0.7：资源句柄与句柄表（generation 校验）。同样是纯头文件实现，不链接 HugEngineRHI。
    TestRHIHandles.cpp
    # 阶段 1（附录 E / E-1）：mesh 注册表（`meshIndex → 渲染侧资源`，快照不带指针的前提）。
    TestMeshRegistry.cpp
    # 阶段 1 T1.1：`FrameSceneSnapshot`（布局与 `ShaderTypes.slang` 一致 + 值语义）。
    TestFrameSceneSnapshot.cpp
    # 阶段 1 T1.2a：光源收集（快照口径）。`SceneSnapshotBuilder.cpp` 是 RHI-free 的（只依赖 Scene 与
    # PhysicalLight 的纯函数），直接编译进测试目标 —— 一旦收集逻辑引入 RHI 依赖，本目标立刻编译失败。
    TestSceneSnapshotBuilder.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Render/Threading/SceneSnapshotBuilder.cpp
    # 开关路径的钉子：`HE_DISABLE_THREAD_ASSERT` 必须在包含头文件之前定义，故单独一个 TU。
    TestThreadAffinityDisabled.cpp
    TestGameplayComponents.cpp
    TestBillboard.cpp
    TestTextRender.cpp
    TestDecal.cpp
    TestCollision.cpp
    TestCharacterMovement.cpp
    TestAbility.cpp
    TestSpline.cpp
    TestSplineMesh.cpp
    TestNavMesh.cpp
    TestInstancedMesh.cpp
    TestBindlessHeap.cpp
    TestSkeletonLoader.cpp
    TestSkeletalMesh.cpp
    TestJoltSmoke.cpp
    TestJoltConversions.cpp
    TestPhysicsBasics.cpp
    TestDeepSeekProtocol.cpp
    TestPromptToScene.cpp
    TestWorldModel.cpp
    TestInferenceScheduler.cpp
    TestAIModule.cpp
    TestAICommand.cpp
    TestAIGCProvider.cpp
    TestAIPipeline.cpp
    TestAgentAction.cpp
    TestAgentBrain.cpp
    TestGITypes.cpp
    TestSurfaceCache.cpp
    TestLumenTraceConfig.cpp
    TestLumenSH.cpp
    TestDenoiseSignal.cpp
    TestGIProbeGrid.cpp
    TestRSMFrustum.cpp
    TestPathPayload.cpp
    TestPTMaterialParams.cpp
    TestPTVolumeMath.cpp
    TestNaniteTypes.cpp
    TestNaniteBuilder.cpp
    # §14.8 任务 25 回归：簇 → 源网格 → 材质映射（真实 Sponza 资产上断言 unmapped==0 +
    # 与"原始三角形真值"逐簇一致）。单独一文件：要用 cgltf 离线合并真实几何，与
    # `TestNaniteBuilder.cpp` 的合成网格用例互补。
    TestNaniteMaterialMap.cpp
    # §14.8 任务 24：页划分（`BuildNanitePagePlan`，纯函数）的不变式/守卫/确定性用例。
    # 单独一文件：`TestNaniteBuilder.cpp` 已 2500+ 行且按任务分段累加。
    TestNaniteStream.cpp
    # §14.8 任务 25：Material Bin（`BuildNaniteMaterialBin`，纯函数）的正确性用例 ——
    # "bin 是按材质非降序的、且是 0..clusterCount-1 的一个排列"（任务 27 明确要求补的那一项），
    # 外加"生成 bin 不改动资产"与"真实 Sponza 资产上按 bin 顺序的 material_switches 收益"。
    # 单独一文件：与前两个 Nanite 测试文件同一个理由（避免单文件无限膨胀）。
    TestNaniteMaterialBin.cpp
    # §14.8 任务 26：每簇 BVH 节点深度（`ComputeNaniteClusterBVHDepths`，纯函数）的正确性用例 ——
    # "根 = 1、长度恒等于簇数、不在任何叶里的簇保持 0、损坏镜像不越界、退化的 400 层深链不爆栈"，
    # 以及与真实 `BuildNaniteClusterBVH` 报出的 `depth` 的交叉核对。
    # 单独一文件：与上面三个 Nanite 测试文件同一个理由（避免单文件无限膨胀）。
    TestNaniteBvhDepth.cpp
    # §14.8 任务 8：离线簇切分的实现落在 Engine/Render/Nanite/NaniteUpload.cpp。
    # 该翻译单元（连同 NaniteUpload.h）是 RHI-free 的（只前置声明 rhi::IRHIDevice），
    # 所以单测可以**直接编译它**，而不必链接 HugEngineRender（后者会拉入 RHI/Vulkan/SPV）。
    # 这同时是一条纪律钉子：任何 RHI 依赖闯进 NaniteUpload.cpp 都会让单测目标编译失败。
    ${CMAKE_SOURCE_DIR}/Engine/Render/Nanite/NaniteUpload.cpp
)

target_include_directories(HugEngineTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/External/doctest
    ${CMAKE_SOURCE_DIR}/Engine/External
    # GI 数据模型（GI/GITypes.h）、探针网格拟合（GI/GIProbeGrid.h）与路径追踪载荷镜像
    # （RT/PathPayload.h）——**只加包含路径、不链接 HugEngineRender**：这些头只依赖
    # Core/Types.h 与 Math/Math.h，链接 Render 会连带拉入 RHI/Vulkan 以及 slangc 生成的
    # SPV 头，对单元测试是不必要的重负担。
    ${CMAKE_SOURCE_DIR}/Engine/Render
    # 渲染线程化方案 T0.1：`RHI/ThreadAffinity.h` 是**头文件实现**（不含 Vulkan 依赖、也不需要
    # 链接 HugEngineRHI），这里只为单元测试补上 RHI 的包含路径。
    ${CMAKE_SOURCE_DIR}/Engine/RHI
    # 阶段 1 T1.1：`Pipeline/Camera.h` 与 `ShaderTypes.slang`（C++/Slang 共享 GPU 布局）
    # 让快照的单测能在**不链接 HugEngineRender/Shader** 的前提下验证布局一致性。
    ${CMAKE_SOURCE_DIR}/Engine/Shader/Shaders
    # §14.8 任务 25 回归用例用 cgltf 直接读真实 glTF（实现体已由 HugEngineAsset 的
    # glTFLoader.cpp 展开 `CGLTF_IMPLEMENTATION`，这里只需要头文件的包含路径）。
    ${CMAKE_SOURCE_DIR}/Engine/External/meshoptimizer/extern
)

target_compile_definitions(HugEngineTests PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

target_link_libraries(HugEngineTests PRIVATE
    HugEngineAI
    HugEngineScene
    HugEngineAsset
    HugEngineCore
    HugEnginePhysics
    Jolt
    # §14.8 任务 8：meshopt_buildMeshlets / optimizeMeshlet / computeMeshletBounds 的符号与
    # <meshoptimizer.h> 的包含路径都由这个 target 提供（v0.22，Engine/External/meshoptimizer）。
    meshoptimizer
)
