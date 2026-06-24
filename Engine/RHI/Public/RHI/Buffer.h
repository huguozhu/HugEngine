#pragma once

#include "RHI/Types.h"
#include "Core/Types.h"

#include <span>

// ============================================================
// RHI Buffer & Texture creation
// ============================================================

namespace he::rhi {

// --- Buffer descriptor ---
struct BufferDesc {
    usize       size        = 0;          // Size in bytes
    BufferUsage usage       = BufferUsage::None;
    bool        cpuAccess   = false;      // Mappable from CPU
    const void* initialData = nullptr;    // Optional initial upload
    usize       stride      = 0;          // For vertex/index buffers
};

// --- Texture descriptor ---
struct TextureDesc {
    u32         width       = 1;
    u32         height      = 1;
    u32         depth       = 1;
    u32         mipLevels   = 1;
    u32         arrayLayers = 1;
    Format      format      = Format::RGBA8_UNORM;
    TextureUsage usage      = TextureUsage::ShaderResource;
    const void* initialData = nullptr;
};

// --- GPU buffer handle ---
class IRHIBuffer {
public:
    virtual ~IRHIBuffer() = default;

    virtual usize  GetSize()  const = 0;
    virtual void*  Map()            = 0;  // Map for CPU read/write
    virtual void   Unmap()          = 0;
    virtual u64    GetDeviceAddress() const = 0; // For bindless
};

// --- GPU texture handle ---
class IRHITexture {
public:
    virtual ~IRHITexture() = default;

    virtual u32 GetWidth()  const = 0;
    virtual u32 GetHeight() const = 0;
    virtual Format GetFormat() const = 0;
};

} // namespace he::rhi
