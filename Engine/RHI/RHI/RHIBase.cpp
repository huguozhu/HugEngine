#include <cstdlib>
#include <string>
#include <cctype>
#include "RHI/RHI.h"
#include "Core/Log.h"

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
