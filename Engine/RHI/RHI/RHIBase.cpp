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

} // namespace he::rhi
