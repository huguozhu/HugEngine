#include <cstdlib>
#include <string>
#include <cctype>
#include "RHI/RHI.h"
#include "RHI/ThreadAffinity.h"   // 创建设备的线程记为 RHI 拥有线程
#include "Core/Log.h"
#if defined(HE_RHI_VULKAN)
#include "Vulkan/VulkanDevice.h"
#endif
#if defined(HE_RHI_D3D12)
#include "D3D12/D3D12Device.h"
#endif
#if defined(HE_RHI_D3D11)
#include "D3D11/D3D11Device.h"
#endif

namespace he::rhi {

// Singleton accessor
IRHIDevice* g_Device = nullptr;

IRHIDevice* GetDevice() {
    return g_Device;
}

void SetDevice(IRHIDevice* device) {
    g_Device = device;
}

// IRHIDevice default implementation
IRHIDevice::IRHIDevice()  = default;
IRHIDevice::~IRHIDevice() = default;

void IRHIDevice::Initialize(const DeviceInitDesc& desc) {
    HE_CORE_INFO("Initializing RHI device...");
    HE_CORE_INFO("  Backend: {}", GetBackendName(GetBackend()));
    HE_CORE_INFO("  Adapter: {}", desc.preferredAdapter >= 0
        ? std::to_string(desc.preferredAdapter) : "default");
}

void IRHIDevice::Shutdown() {
    HE_CORE_INFO("Shutting down RHI device");
}

// Debug Object Naming — 默认空实现
void IRHIDevice::SetResourceDebugName(IRHIBuffer*, const char*) {}
void IRHIDevice::SetResourceDebugName(IRHITexture*, const char*) {}
void IRHIDevice::SetResourceDebugName(IRHIPipelineState*, const char*) {}
void IRHIDevice::SetResourceDebugName(IRHISampler*, const char*) {}
void IRHIDevice::SetResourceDebugName(IRHIAccelerationStructure*, const char*) {}

// ---------------------------------------------------------------------------
//  工厂：按后端创建设备
//  放在后端无关的 RHIBase.cpp 里（原先写在 VulkanDevice.cpp，导致 D3D 后端
//  依附于 Vulkan 的编译开关）。每个后端由各自的 HE_RHI_* 宏决定是否参与。
// ---------------------------------------------------------------------------
const char* GetBackendName(Backend backend) {
    switch (backend) {
        case Backend::Vulkan: return "Vulkan";
        case Backend::D3D12:  return "D3D12";
        case Backend::D3D11:  return "D3D11";
        default:              return "Unknown";
    }
}

/// 设备无法创建时的统一出口：打印原因后退出进程（见 RHI.h 中 CreateDevice 的说明）
[[noreturn]] static void FailCreateDevice(Backend backend, const char* reason) {
    HE_CORE_ERROR("CreateDevice: 无法创建 {} 设备：{}。进程退出。", GetBackendName(backend), reason);
    std::exit(EXIT_FAILURE);
}

std::unique_ptr<IRHIDevice> CreateDevice(Backend backend) {
#if defined(HE_RHI_D3D12)
    // D3D12 后端：与 Vulkan 并列，接口完全一致（IRHIDevice）
    if (backend == Backend::D3D12) {
        auto device = std::make_unique<d3d12::D3D12Device>();
        if (!device->Initialize()) {
            FailCreateDevice(backend, "设备初始化失败");
        }
        GetThreadAffinity().Claim();
        return device;
    }
#endif
#if defined(HE_RHI_D3D11)
    // D3D11 后端：不满足最低运行环境时 Initialize 返回 false，原因已打日志
    if (backend == Backend::D3D11) {
        auto device = std::make_unique<d3d11::D3D11Device>();
        if (!device->Initialize()) {
            FailCreateDevice(backend, "设备初始化失败（原因见上方日志）");
        }
        GetThreadAffinity().Claim();
        return device;
    }
#endif
#if defined(HE_RHI_VULKAN)
    if (backend == Backend::Vulkan) {
        auto device = std::make_unique<VulkanDevice>();
        // T0.1：把**创建设备的线程**记为 RHI 拥有线程（阶段 0 即主线程；阶段 2 起是渲染线程）。
        // 由于认领的是"创建线程"，阶段 2 的 T2.2 必须保证设备在渲染线程上创建与销毁。
        GetThreadAffinity().Claim();
        return device;
    }
#endif
    FailCreateDevice(backend, "该后端未编译进本构建");
}

// ---------------------------------------------------------------------------
//  后端选择：读取 HE_RHI_BACKEND 环境变量
//  Vulkan 是默认后端；D3D12 只有 Windows 构建才可用（其他平台回退 Vulkan）。
// ---------------------------------------------------------------------------
Backend ResolveBackendFromEnv() {
    const char* env = std::getenv("HE_RHI_BACKEND");
    if (!env || !*env) return Backend::Vulkan;

    std::string s(env);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (s == "d3d12" || s == "dx12") {
#if defined(_WIN32)
        return Backend::D3D12;
#else
        HE_CORE_WARN("HE_RHI_BACKEND=d3d12 只在 Windows 可用，回退 Vulkan");
        return Backend::Vulkan;
#endif
    }
    if (s == "d3d11" || s == "dx11") {
#if defined(_WIN32)
        return Backend::D3D11;
#else
        HE_CORE_WARN("HE_RHI_BACKEND=d3d11 只在 Windows 可用，回退 Vulkan");
        return Backend::Vulkan;
#endif
    }
    if (s != "vulkan") {
        HE_CORE_WARN("未知的 HE_RHI_BACKEND='{}'，回退 Vulkan", s.c_str());
    }
    return Backend::Vulkan;
}

} // namespace he::rhi
