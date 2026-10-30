// ============================================================================
//  D3D12AgilityExports.cpp —— D3D12 Agility SDK 的导出符号
//
//  D3D12 加载器在进程启动时查找 **exe** 导出的这两个符号：
//    · D3D12SDKVersion：期望的运行时版本（必须等于包内 d3d12.h 的 D3D12_SDK_VERSION）
//    · D3D12SDKPath   ：D3D12Core.dll 所在目录，相对 exe
//  找到且该目录下有对应版本的 D3D12Core.dll 时使用它，否则退回系统运行时。
//  符号放在静态库里不会被链接进 exe，因此本文件由 huge_enable_d3d12_agility()
//  （cmake/AgilitySDK.cmake）直接加进每个可执行目标。
// ============================================================================
#if defined(_WIN32) && defined(HE_D3D12_AGILITY_SDK_VERSION)

extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = HE_D3D12_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char*        D3D12SDKPath    = ".\\D3D12\\";
}

#endif
