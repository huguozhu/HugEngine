// ============================================================================
//  D3D11Resources.h —— D3D11 的资源与管线对象
//
//  M2 骨架阶段：先提供可构造的占位对象，保证设备的所有创建接口都返回非空对象
//  （引擎不检查空指针，D3D12 后端曾因返回空而静默挂起）。
//  Buffer / Texture / Sampler 在 M2.3 实现，PipelineState 在 M2.4，QueryPool 在 M3.7。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D11Common.h"
#include "RHI/Shader.h"
#include "RHI/QueryPool.h"
#include <vector>

namespace he::rhi::d3d11 {

class D3D11Device;

// ---------------------------------------------------------------------------
//  Buffer（占位：只记大小，Map 返回 CPU 侧内存）
// ---------------------------------------------------------------------------
class D3D11Buffer final : public IRHIBuffer {
public:
    explicit D3D11Buffer(const BufferDesc& desc) : m_Cpu(desc.size) {}
    usize GetSize() const override { return m_Cpu.size(); }
    void* Map() override { return m_Cpu.data(); }
    void  Unmap() override {}
    u64   GetDeviceAddress() const override { return 0; }   // D3D11 没有缓冲 GPU 地址
private:
    std::vector<u8> m_Cpu;
};

// ---------------------------------------------------------------------------
//  Texture（占位：只记尺寸与格式）
// ---------------------------------------------------------------------------
class D3D11Texture final : public IRHITexture {
public:
    explicit D3D11Texture(const TextureDesc& desc) : m_Desc(desc) {}
    u32    GetWidth()  const override { return m_Desc.width; }
    u32    GetHeight() const override { return m_Desc.height; }
    u32    GetDepth()  const override { return m_Desc.depth; }
    u32    GetMipLevels()   const override { return m_Desc.mipLevels; }
    u32    GetArrayLayers() const override { return m_Desc.arrayLayers; }
    Format GetFormat() const override { return m_Desc.format; }
    void*  GetNativeHandle() const override { return nullptr; }
    void*  GetNativeHandle(u32) const override { return nullptr; }
private:
    TextureDesc m_Desc;
};

// ---------------------------------------------------------------------------
//  Sampler / PipelineState / QueryPool（占位）
// ---------------------------------------------------------------------------
class D3D11Sampler final : public IRHISampler {};

class D3D11PipelineState final : public IRHIPipelineState {};

class D3D11QueryPool final : public IRHIQueryPool {
public:
    D3D11QueryPool(u32 count, QueryType type) : m_Count(count), m_Type(type) {}
    u32       GetQueryCount() const override { return m_Count; }
    QueryType GetQueryType()  const override { return m_Type; }
private:
    u32       m_Count = 0;
    QueryType m_Type  = QueryType::Timestamp;
};

} // namespace he::rhi::d3d11

#endif // _WIN32
