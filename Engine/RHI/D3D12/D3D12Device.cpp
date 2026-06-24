#include "RHI/RHI.h"
#include "Core/Log.h"

namespace he::rhi {

class D3D12Device final : public IRHIDevice {
public:
    Backend    GetBackend() const override { return Backend::D3D12; }
    DeviceCaps GetCaps()    const override;

    void Initialize(const DeviceInitDesc& desc) override;
    void Shutdown() override;
};

DeviceCaps D3D12Device::GetCaps() const {
    DeviceCaps caps;
    caps.maxBindlessResources = 1000000;
    caps.maxPushConstantsSize = 256;
    caps.supportsRayTracing   = true;
    caps.supportsMeshShaders  = true;
    return caps;
}

void D3D12Device::Initialize(const DeviceInitDesc& desc) {
    IRHIDevice::Initialize(desc);

    // TODO: Full D3D12 initialization
    // 1. Create IDXGIFactory
    // 2. Select adapter
    // 3. Create ID3D12Device
    // 4. Create command queue, swap chain
    // 5. Create descriptor heaps

    HE_CORE_INFO("D3D12 device stub initialized");
}

void D3D12Device::Shutdown() {
    HE_CORE_INFO("D3D12 device stub shutdown");
    IRHIDevice::Shutdown();
}


} // namespace he::rhi
