# ============================================================
# Tests — doctest 单元测试目标
#
# 所有测试用例文件集中在这里，链接被测的 HugEngine 模块。
# ============================================================

add_executable(HugEngineTests
    Main.cpp
    TestSceneBuilder.cpp
    TestPrimaryCamera.cpp
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
