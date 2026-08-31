# ============================================================
# Engine/AI — AI 模块（L2.5 AI 运行时层最小实现）
#
# LLM 场景生成：SceneBuilder(JSON→World) + TypeSchema + LLM 客户端。
# 依赖：HugEngineScene / HugEngineCore / nlohmann_json(本地 vendor)
# ============================================================

add_library(HugEngineAI STATIC)

set(AI_SOURCES
    LLMClient.h
    DeepSeekClient.h
    DeepSeekClient.cpp
    PromptToScene.h
    PromptToScene.cpp
    SceneBuilder.h
    SceneBuilder.cpp
    TypeSchema.h
    TypeSchema.cpp
    WorldModel/Observation.h
    WorldModel/WorldModel.h
    WorldModel/WorldModel.cpp
    WorldModel/Action.h
    WorldModel/Action.cpp
    Agent/AgentComponent.h
    Agent/MemoryComponent.h
    Agent/MemoryComponent.cpp
    Agent/GoalComponent.h
    Agent/GoalComponent.cpp
    Agent/Brain.h
    Agent/LLMBrain.h
    Agent/LLMBrain.cpp
    Agent/MockBrain.h
    Agent/ToolUse.h
    Agent/ToolUse.cpp
    Agent/AgentSystem.h
    Agent/AgentSystem.cpp
    Agent/AgentReflect.cpp
    Runtime/AIDevice.h
    Runtime/AIDevice.cpp
    Runtime/InferenceScheduler.h
    Runtime/InferenceScheduler.cpp
    Runtime/AIModule.h
    Runtime/AIModule.cpp
    Runtime/Backend/IAIBackend.h
    Runtime/Backend/RemoteBackend.h
    Runtime/Backend/RemoteBackend.cpp
    AIGC/AICommand.h
    AIGC/AICommand.cpp
    AIGC/GenerativeAssetFactory.h
    AIGC/GenerativeAssetFactory.cpp
    AIGC/AIGCProvider.h
    AIGC/AIGCProvider.cpp
    AIGC/AIPipeline.h
    AIGC/AIPipeline.cpp
)

target_sources(HugEngineAI PRIVATE ${AI_SOURCES})
huge_source_group(${AI_SOURCES})

# 约定：include 根为 Engine/（如 "AI/TypeSchema.h"、"Scene/World.h"、"Core/Log.h" 均可用）
target_include_directories(HugEngineAI
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(HugEngineAI PUBLIC
    HugEngineScene
    HugEngineCore
    HugEngineEditor   # 复用 he::Command / CommandHistory（GenerateSceneCommand）
    nlohmann_json
)
target_link_libraries(HugEngineAI PRIVATE winhttp)
