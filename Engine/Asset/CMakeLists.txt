# Asset — 资源加载, glTF, 注册表 (Phase 1)
add_library(HugEngineAsset STATIC)

set(ASSET_SOURCES
    Asset/glTFLoader.h
    Asset/BindlessTextureManager.h
    Asset/glTFLoader.cpp
    Asset/BindlessTextureManager.cpp
)

target_sources(HugEngineAsset PRIVATE ${ASSET_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${ASSET_SOURCES})

target_include_directories(HugEngineAsset
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine/External/meshoptimizer/extern  # cgltf 单头文件库
)

target_link_libraries(HugEngineAsset PUBLIC HugEngineScene HugEngineReflect)
