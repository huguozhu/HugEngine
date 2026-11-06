# Samples
add_subdirectory(01.Triangle)
add_subdirectory(02.Cube)
add_subdirectory(03.Sponza-Forward)
add_subdirectory(04.Sponza-Deferred)
add_subdirectory(05.Sponza-PathTracing)
add_subdirectory(06.GILab)
add_subdirectory(07.AISamples)
add_subdirectory(07.Nanite)
add_subdirectory(Editor)

# D3D12 Agility SDK：每个可执行文件导出 D3D12SDKVersion / D3D12SDKPath 并带上运行时 DLL
# （见 cmake/AgilitySDK.cmake；未下载到 SDK 时为空操作，D3D12 退回系统运行时）
if(COMMAND huge_enable_d3d12_agility)
    foreach(_sample 01.Triangle 02.Cube 03.Sponza-Forward 04.Sponza-Deferred 05.Sponza-PathTracing
                    06.GILab 07.AISamples 07.Nanite HugEditor)
        huge_enable_d3d12_agility(${_sample})
    endforeach()
endif()
