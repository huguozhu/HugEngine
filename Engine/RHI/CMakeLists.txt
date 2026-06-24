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
        Vulkan/VulkanResources.cpp
    )
    target_link_libraries(HugEngineRHI PUBLIC Vulkan::Vulkan)
    target_compile_definitions(HugEngineRHI PUBLIC HE_RHI_VULKAN)

    message(STATUS "Vulkan backend: ENABLED (${Vulkan_INCLUDE_DIRS})")
endif()

# --- D3D12 backend (Windows SDK) ---
if(HUGENGINE_RHI_D3D12)
    if(WIN32)
        target_sources(HugEngineRHI PRIVATE D3D12/D3D12Device.cpp)
        target_compile_definitions(HugEngineRHI PUBLIC HE_RHI_D3D12)

        # D3D12 headers are part of Windows SDK — no find_package needed
        # DirectX-Headers from FetchContent for dxcapi.h etc.
        message(STATUS "D3D12 backend: ENABLED (Windows SDK)")
    else()
        message(WARNING "D3D12 backend requires Windows — disabled")
        set(HUGENGINE_RHI_D3D12 OFF)
    endif()
endif()
