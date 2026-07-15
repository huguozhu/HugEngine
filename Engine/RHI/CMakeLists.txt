# RHI — Rendering Hardware Interface (L2)
# Phase 1 · Weeks 5-8

add_library(HugEngineRHI STATIC)

target_sources(HugEngineRHI PRIVATE
    Private/RHIBase.cpp
)

target_include_directories(HugEngineRHI
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(HugEngineRHI PUBLIC HugEngineCore)

# --- Vulkan backend (system SDK, not FetchContent) ---
if(HUGENGINE_RHI_VULKAN)
    # Find Vulkan SDK from system (VULKAN_SDK env var or standard install path)
    find_package(Vulkan REQUIRED)

    target_sources(HugEngineRHI PRIVATE
        Vulkan/VulkanDevice.cpp
        Vulkan/VulkanSwapChain.cpp
        Vulkan/VulkanCommandList.cpp
        Vulkan/VulkanResources.cpp
        Vulkan/VulkanPipeline.cpp
        Vulkan/VulkanRayTracing.cpp
        Vulkan/VulkanDGC.cpp
    )
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
