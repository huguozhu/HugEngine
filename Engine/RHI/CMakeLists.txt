# RHI — Rendering Hardware Interface (L2)
# Phase 1 · Weeks 5-8

add_library(HugEngineRHI STATIC)

set(RHI_SOURCES
    RHI/RHI.h
    RHI/Types.h
    RHI/Shader.h
    RHI/CommandList.h
    RHI/Buffer.h
    RHI/Bindless.h
    RHI/BindlessSlotRing.h
    RHI/FrameRetireQueue.h
    RHI/SwapChain.h
    RHI/ThreadAffinity.h
    RHI/RHICommandList.h
    # A-1 翻译层（命令流 -> RHI 调用；只在 RHI 线程上调用）
    RHI/RHICommandReplay.h
    RHI/RHICommandReplay.cpp
    RHI/RHIHandles.h
    RHI/QueryPool.h
    RHI/MeshShader.h
    RHI/RayTracing.h
    RHI/TextureLayoutTracker.h
    RHI/TextureLayoutTracker.cpp
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
    TransientResourceAllocator.h
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
    # Vulkan SDK 路径已由 cmake/VulkanSDK.cmake 自动检测并写入 ENV{VULKAN_SDK}
    find_package(Vulkan REQUIRED)

    set(RHI_VULKAN_SOURCES
        Vulkan/VulkanDevice.cpp
        Vulkan/VulkanBindlessHeap.h
        Vulkan/VulkanBindlessHeap.cpp
        Vulkan/VulkanRT.cpp
        Vulkan/VulkanDevice_MeshShader.cpp
        Vulkan/VulkanDevice_GPL.cpp
        Vulkan/VulkanDevice_Descriptors.cpp
        Vulkan/VulkanDevice_DebugUtils.cpp
        Vulkan/VulkanSwapChain.cpp
        Vulkan/VulkanCommandList.cpp
        Vulkan/VulkanCommandList_RenderPass.cpp
        Vulkan/VulkanCommandList_Submit.cpp
        Vulkan/VulkanResources.cpp
        Vulkan/VulkanTextureLiveness.h
        Vulkan/VulkanTextureLiveness.cpp
        Vulkan/VulkanConverters.cpp
        Vulkan/VulkanPipeline.cpp
        Vulkan/VulkanPipelineLibrary.h
        Vulkan/VulkanPipelineLibrary.cpp
        Vulkan/VulkanDGC.cpp
        Vulkan/PSOPrecompileManager.h
        Vulkan/PSOPrecompileManager.cpp
        Vulkan/DeferredDestructionQueue.h
        TransientResourceAllocator.cpp
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

    # ========================================================================
    #  D3D12 后端（Windows 专用）
    #
    #  与 Vulkan 后端并列：同一份 IRHIDevice/IRHICommandList 接口，
    #  由 CreateDevice(Backend::D3D12) 选择。全部实现放在 Engine/RHI/D3D12/。
    # ========================================================================
    if(WIN32)
        set(RHI_D3D12_SOURCES
            D3D12/D3D12Common.h
            D3D12/D3D12Converters.h
            D3D12/D3D12Converters.cpp
            D3D12/D3D12Device.h
            D3D12/D3D12Device.cpp
            D3D12/D3D12Resources.h
            D3D12/D3D12Resources.cpp
            D3D12/D3D12RayTracing.h
            D3D12/D3D12RayTracing.cpp
            D3D12/D3D12SwapChain.h
            D3D12/D3D12SwapChain.cpp
            D3D12/D3D12CommandList.h
            D3D12/D3D12CommandList.cpp
            D3D12/D3D12BindlessHeap.h
            D3D12/D3D12BindlessHeap.cpp
        )
        target_sources(HugEngineRHI PRIVATE ${RHI_D3D12_SOURCES})
        huge_source_group(${RHI_D3D12_SOURCES})

        # 需要 DXGI / D3D12 的导入库。d3d12.lib 与 dxgi.lib 在 Windows SDK 里。
        # d3dcompiler 提供 D3DReflect（从 DXIL 反射输入签名与绑定信息）
        target_link_libraries(HugEngineRHI PUBLIC d3d12 dxgi dxguid)

        # DXC：DXIL 反射（IDxcUtils::CreateReflection）需要 dxcompiler。
        # 优先用 Vulkan SDK 自带的（与 slangc 使用的同一份，版本匹配）。
        if(DEFINED ENV{VULKAN_SDK})
            target_include_directories(HugEngineRHI PRIVATE "$ENV{VULKAN_SDK}/Include/dxc")
            target_link_directories(HugEngineRHI PRIVATE "$ENV{VULKAN_SDK}/Lib")
            target_link_libraries(HugEngineRHI PUBLIC dxcompiler)
            message(STATUS "D3D12 DXIL reflection: dxcompiler from $ENV{VULKAN_SDK}")
        else()
            message(WARNING "VULKAN_SDK 未设置 —— DXIL 反射可能失败")
        endif()
        target_compile_definitions(HugEngineRHI PUBLIC HE_RHI_D3D12)

        message(STATUS "D3D12 backend: ENABLED")
    endif()

# ---------------------------------------------------------------------------
# 把 dxcompiler.dll 复制到输出目录：D3D12 后端在运行期用 IDxcUtils::CreateReflection
# 读取结构化缓冲的元素大小（StructuredBuffer<T> 的 SRV stride 必须等于 sizeof(T)）。
# 若该 DLL 不在进程可搜索路径里，DxcCreateInstance 会失败，stride 只能退化成 4，
# 导致着色器按错误步长读取（实测表现为顶点塌缩、画面全黑）。
# ---------------------------------------------------------------------------
find_program(HE_DXC_EXE NAMES dxc)
if(HE_DXC_EXE)
    get_filename_component(HE_DXC_DIR "${HE_DXC_EXE}" DIRECTORY)
    if(EXISTS "${HE_DXC_DIR}/dxcompiler.dll")
        add_custom_command(TARGET HugEngineRHI POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HE_DXC_DIR}/dxcompiler.dll"
                    "$<TARGET_FILE_DIR:HugEngineRHI>/dxcompiler.dll"
            COMMENT "Copy dxcompiler.dll for runtime shader reflection")
        message(STATUS "dxcompiler.dll for runtime reflection: ${HE_DXC_DIR}")
    endif()
endif()
