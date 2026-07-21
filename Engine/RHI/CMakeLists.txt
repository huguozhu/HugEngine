# RHI — Rendering Hardware Interface (L2)
# Phase 1 · Weeks 5-8

add_library(HugEngineRHI STATIC)

set(RHI_SOURCES
    RHI/RHI.h
    RHI/Types.h
    RHI/Shader.h
    RHI/CommandList.h
    RHI/Buffer.h
    RHI/SwapChain.h
    RHI/QueryPool.h
    RHI/MeshShader.h
    RHI/RayTracing.h
    RHI/RHIBase.cpp
    Vulkan/VulkanDevice.h
    Vulkan/VulkanSwapChain.h
    Vulkan/VulkanCommandList.h
    Vulkan/VulkanResources.h
    Vulkan/VulkanRT.h
    Vulkan/VulkanConverters.h
    Vulkan/VulkanPipelineState.h
    Vulkan/VulkanQueryPool.h
    Vulkan/VulkanDGC.h
)

target_sources(HugEngineRHI PRIVATE ${RHI_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${RHI_SOURCES})

target_include_directories(HugEngineRHI
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(HugEngineRHI PUBLIC HugEngineCore)

# --- Vulkan backend (system SDK, not FetchContent) ---
if(HUGENGINE_RHI_VULKAN)
    # Find Vulkan SDK from system (VULKAN_SDK env var or standard install path)
    find_package(Vulkan REQUIRED)

    set(RHI_VULKAN_SOURCES
        Vulkan/VulkanDevice.cpp
        Vulkan/VulkanRT.cpp
        Vulkan/VulkanDevice_MeshShader.cpp
        Vulkan/VulkanDevice_Descriptors.cpp
        Vulkan/VulkanDevice_DebugUtils.cpp
        Vulkan/VulkanSwapChain.cpp
        Vulkan/VulkanCommandList.cpp
        Vulkan/VulkanCommandList_RenderPass.cpp
        Vulkan/VulkanCommandList_Submit.cpp
        Vulkan/VulkanResources.cpp
        Vulkan/VulkanConverters.cpp
        Vulkan/VulkanPipeline.cpp
        Vulkan/VulkanDGC.cpp
    )

    target_sources(HugEngineRHI PRIVATE ${RHI_VULKAN_SOURCES})

    # IDE 目录树：Vulkan 实现文件按磁盘目录分组
    huge_source_group(${RHI_VULKAN_SOURCES})

    target_link_libraries(HugEngineRHI PUBLIC Vulkan::Vulkan)
    # 暴露 Vulkan 桥接头文件给 Sample（Phase 1 临时方案）
    # 使得 #include "Vulkan/VulkanInternal.h" 可解析
    target_include_directories(HugEngineRHI PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

    target_include_directories(HugEngineRHI PUBLIC
        ${CMAKE_SOURCE_DIR}/Engine/External/VulkanMemoryAllocator   # VMA 单头文件
    )

    target_compile_definitions(HugEngineRHI PUBLIC HE_RHI_VULKAN)

    message(STATUS "Vulkan backend: ENABLED (${Vulkan_INCLUDE_DIRS})")
endif()
