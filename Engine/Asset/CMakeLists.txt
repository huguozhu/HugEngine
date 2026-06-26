# Asset — 资源加载, glTF, 注册表 (Phase 1)
add_library(HugEngineAsset STATIC)

target_sources(HugEngineAsset PRIVATE
    Private/glTFLoader.cpp
)

target_include_directories(HugEngineAsset
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Private
        ${CMAKE_SOURCE_DIR}/Engine/External/meshoptimizer/extern  # cgltf 单头文件库
)

target_link_libraries(HugEngineAsset PUBLIC HugEngineScene HugEngineReflect)
