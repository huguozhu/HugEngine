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
    TestInstancedMesh.cpp
    TestSkeletonLoader.cpp
    TestSkeletalMesh.cpp
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
)

target_include_directories(HugEngineTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/External/doctest
    ${CMAKE_SOURCE_DIR}/Engine/External
)

target_compile_definitions(HugEngineTests PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

target_link_libraries(HugEngineTests PRIVATE
    HugEngineAI
    HugEngineScene
    HugEngineAsset
    HugEngineCore
)
