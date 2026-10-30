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
    HE_CORE_INFO("  Backend: Vulkan");
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
std::unique_ptr<IRHIDevice> CreateDevice(Backend backend) {
#if defined(HE_RHI_D3D12)
    // D3D12 后端：与 Vulkan 并列，接口完全一致（IRHIDevice）
    if (backend == Backend::D3D12) {
        auto device = std::make_unique<d3d12::D3D12Device>();
        if (!device->Initialize()) {
            HE_CORE_ERROR("CreateDevice: D3D12 设备初始化失败");
            return nullptr;
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
    HE_CORE_ERROR("CreateDevice: 后端 {} 未编译进本构建", static_cast<int>(backend));
    return nullptr;
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
    if (s != "vulkan") {
        HE_CORE_WARN("未知的 HE_RHI_BACKEND='{}'，回退 Vulkan", s.c_str());
    }
    return Backend::Vulkan;
}

} // namespace he::rhi
