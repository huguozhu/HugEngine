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
)

target_include_directories(HugEngineTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/External/doctest
    ${CMAKE_SOURCE_DIR}/Engine/External
    # GI 数据模型（GI/GITypes.h）——**只加包含路径、不链接 HugEngineRender**：
    # 该头文件只依赖 Core/Types.h，链接 Render 会连带拉入 RHI/Vulkan
    # 以及 slangc 生成的 SPV 头，对单元测试是不必要的重负担。
    ${CMAKE_SOURCE_DIR}/Engine/Render
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
)
