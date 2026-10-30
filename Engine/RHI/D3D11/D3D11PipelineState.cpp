// ============================================================================
//  D3D11PipelineState.cpp —— D3D11 的「管线状态」（说明见 D3D11Resources.h）
// ============================================================================
#include "D3D11Resources.h"
#include "D3D11Converters.h"
#include "D3D11Device.h"
#include "RHI/DxbcBindTable.h"

#include <d3dcompiler.h>   // D3DReflect：DXBC 的输入签名与资源绑定
#include <algorithm>
#include <cstring>

namespace he::rhi::d3d11 {

using dxbc::DxbcBindEntry;
using dxbc::DxbcBindType;

/// 反射出的资源类型 → 绑定表里的寄存器类别（推送常量与描述符集常量缓冲都在 b 寄存器）
static bool SameRegisterClass(D3D_SHADER_INPUT_TYPE sit, u32 bindType) {
    const auto t = static_cast<DxbcBindType>(bindType);
    switch (sit) {
        case D3D_SIT_CBUFFER: case D3D_SIT_TBUFFER:
            return t == DxbcBindType::CBuffer || t == DxbcBindType::PushConstant;
        case D3D_SIT_SAMPLER:
            return t == DxbcBindType::Sampler;
        case D3D_SIT_TEXTURE: case D3D_SIT_STRUCTURED: case D3D_SIT_BYTEADDRESS:
            return t == DxbcBindType::SRV;
        default:   // 各种 UAV
            return t == DxbcBindType::UAV;
    }
}

/// 顶点输入签名元素 → DXGI 格式（由分量类型与掩码推导，与 D3D12 后端一致）
static DXGI_FORMAT SignatureFormat(const D3D11_SIGNATURE_PARAMETER_DESC& p, u32& bytes) {
    const UINT n = (p.Mask & 0x8) ? 4 : (p.Mask & 0x4) ? 3 : (p.Mask & 0x2) ? 2 : 1;
    bytes = 4u * n;
    switch (p.ComponentType) {
        case D3D_REGISTER_COMPONENT_UINT32:
            return (n == 1) ? DXGI_FORMAT_R32_UINT : (n == 2) ? DXGI_FORMAT_R32G32_UINT
                 : (n == 3) ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
        case D3D_REGISTER_COMPONENT_SINT32:
            return (n == 1) ? DXGI_FORMAT_R32_SINT : (n == 2) ? DXGI_FORMAT_R32G32_SINT
                 : (n == 3) ? DXGI_FORMAT_R32G32B32_SINT : DXGI_FORMAT_R32G32B32A32_SINT;
        default:
            return (n == 1) ? DXGI_FORMAT_R32_FLOAT : (n == 2) ? DXGI_FORMAT_R32G32_FLOAT
                 : (n == 3) ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
}

/// 顶点属性格式的字节数（检查属性是否超出顶点步长）
static u32 VertexFormatBytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
        case DXGI_FORMAT_R32G32B32_FLOAT:    case DXGI_FORMAT_R32G32B32_UINT:    case DXGI_FORMAT_R32G32B32_SINT:    return 12;
        case DXGI_FORMAT_R32G32_FLOAT:       case DXGI_FORMAT_R32G32_UINT:       case DXGI_FORMAT_R32G32_SINT:       return 8;
        default: return 4;
    }
}

bool D3D11PipelineState::Initialize(D3D11Device* device, const PipelineStateDesc& desc) {
    m_Name        = desc.debugName.empty() ? std::string("(未命名)") : std::string(desc.debugName.c_str());
    m_IsCompute   = (desc.bindPoint == PipelineBindPoint::Compute) || (desc.computeShader && !desc.vertexShader);
    m_ColorLoadOp = desc.colorLoadOp;
    m_DepthLoadOp = desc.depthLoadOp;
    m_DepthWrite  = desc.depthWrite;
    m_Topology    = ToD3D11Topology(desc.topology);

    if (desc.meshShader || desc.amplificationShader) {
        // mesh shader 管线：D3D11 不支持（supportsMeshShaders=false，引擎正常不会创建）
        HE_CORE_WARN("[D3D11] PSO {}：D3D11 不支持 mesh shader，该管线不可用", m_Name);
        return true;
    }

    if (m_IsCompute) {
        m_Valid = LoadStage(device, desc.computeShader, CS);
        return true;
    }

    m_Valid = LoadStage(device, desc.vertexShader, VS);
    // 没有像素着色器的图形管线（只写深度的阴影 pass）是合法的
    if (m_Valid && desc.pixelShader) m_Valid = LoadStage(device, desc.pixelShader, PS);
    if (!m_Valid) return true;

    if (!BuildInputLayout(device, desc)) { m_Valid = false; return true; }
    CreateFixedFunctionStates(device, desc);
    return true;
}

void D3D11PipelineState::ReportUnusableOnce() {
    if (m_Reported) return;
    m_Reported = true;
    HE_CORE_WARN("[D3D11] PSO {} 在 D3D11 下不可用（着色器 DXBC 编译失败，见 generated_dxbc/dxbc_report.md），"
                 "使用它的绘制 / 派发被跳过", m_Name);
}

bool D3D11PipelineState::LoadStage(D3D11Device* device, const ShaderBytecode* bc, Stage stage) {
    if (!bc) return false;
    const u32 size = dxbc::GetDxbcContainerSize(bc->dxbc.data(), bc->dxbc.size());
    if (size == 0) return false;   // 编译失败的占位（不是 DXBC）

    // 绑定表：ShaderToDxil 附在容器之后
    const DxbcBindEntry* entries = nullptr;
    const auto* header = dxbc::FindDxbcBindTable(bc->dxbc.data(), bc->dxbc.size(), &entries);
    if (!header) {
        HE_CORE_ERROR("[D3D11] PSO {}：DXBC 之后没有绑定表（字节码不是 ShaderToDxil --dxbc 生成的？）", m_Name);
        return false;
    }

    ID3D11Device5* dev = device->GetDevice();
    const u8* code = bc->dxbc.data();
    HRESULT hr = E_FAIL;
    switch (stage) {
        case VS: hr = dev->CreateVertexShader(code, size, nullptr, &m_VS);
                 m_VsBytecode.assign(code, code + size); break;
        case PS: hr = dev->CreatePixelShader(code, size, nullptr, &m_PS); break;
        case CS: hr = dev->CreateComputeShader(code, size, nullptr, &m_CS); break;
        default: break;
    }
    if (!HE_D3D11_CHECK(hr, "Create*Shader")) {
        HE_CORE_ERROR("[D3D11] PSO {}：着色器对象创建失败（阶段 {}）", m_Name, static_cast<u32>(stage));
        return false;
    }

    // 反射：只绑定着色器实际用到的资源，并取视图形态（结构化步长、纹理维度）
    ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(code, size, IID_PPV_ARGS(&refl)))) {
        HE_CORE_ERROR("[D3D11] PSO {}：D3DReflect 失败", m_Name);
        return false;
    }
    D3D11_SHADER_DESC sd{};
    refl->GetDesc(&sd);
    auto& out = m_Bindings[stage];
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b{};
        if (FAILED(refl->GetResourceBindingDesc(i, &b))) continue;
        // 在绑定表里找同类寄存器、同槽位的那一项（得到它在描述符集里的位置）
        const DxbcBindEntry* match = nullptr;
        for (u32 k = 0; k < header->entryCount; ++k) {
            if (SameRegisterClass(b.Type, entries[k].type) && entries[k].slot == b.BindPoint) {
                match = &entries[k];
                break;
            }
        }
        if (!match) {
            HE_CORE_WARN("[D3D11] PSO {}：资源 {}（槽位 {}）在绑定表里找不到，忽略", m_Name, b.Name, b.BindPoint);
            continue;
        }
        Binding x;
        x.set     = match->set;
        x.binding = match->binding;
        x.type    = match->type;
        x.slot    = b.BindPoint;
        x.count   = std::max<u32>(b.BindCount, 1u);
        x.texture = (b.Type == D3D_SIT_TEXTURE || b.Type == D3D_SIT_UAV_RWTYPED) &&
                    b.Dimension != D3D_SRV_DIMENSION_BUFFER && b.Dimension != D3D_SRV_DIMENSION_BUFFEREX;
        x.rawBuffer = (b.Type == D3D_SIT_BYTEADDRESS || b.Type == D3D_SIT_UAV_RWBYTEADDRESS);
        // 结构化缓冲的元素字节数记在 NumSamples 里（D3D11 反射的约定）
        if (b.Type == D3D_SIT_STRUCTURED || b.Type == D3D_SIT_UAV_RWSTRUCTURED ||
            b.Type == D3D_SIT_UAV_APPEND_STRUCTURED || b.Type == D3D_SIT_UAV_CONSUME_STRUCTURED ||
            b.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER) {
            x.stride = b.NumSamples;
        }
        x.dimension = b.Dimension;
        if (static_cast<DxbcBindType>(x.type) == DxbcBindType::PushConstant) m_UsesPushConstants = true;
        out.push_back(x);
    }
    return true;
}

bool D3D11PipelineState::BuildInputLayout(D3D11Device* device, const PipelineStateDesc& desc) {
    ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(m_VsBytecode.data(), m_VsBytecode.size(), IID_PPV_ARGS(&refl)))) return false;
    D3D11_SHADER_DESC sd{};
    refl->GetDesc(&sd);

    // 着色器的顶点输入（跳过系统值；实例号单独挂到实例序号槽）。
    // 【location ↔ 签名元素】引擎着色器的顶点输入结构都按 vk::location 0,1,2… 顺序声明
    // （M2.4 实测 6 个结构全部如此），所以「第 n 个非系统值输入」就是 location n，
    // 不需要像 D3D12 后端那样按语义名猜顺序。
    struct ShaderInput { D3D11_SIGNATURE_PARAMETER_DESC p; DXGI_FORMAT fmt; u32 bytes; };
    std::vector<ShaderInput> inputs;
    for (UINT i = 0; i < sd.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};
        if (FAILED(refl->GetInputParameterDesc(i, &p))) continue;
        if (p.SystemValueType != D3D_NAME_UNDEFINED) continue;
        if (p.SemanticName && std::strcmp(p.SemanticName, "HE_INSTANCEINDEX") == 0) {
            m_NeedsInstanceIndexVB = true;
            continue;
        }
        ShaderInput si{ p, DXGI_FORMAT_UNKNOWN, 0 };
        si.fmt = SignatureFormat(p, si.bytes);
        inputs.push_back(si);
    }
    std::sort(inputs.begin(), inputs.end(),
              [](const ShaderInput& a, const ShaderInput& b) { return a.p.Register < b.p.Register; });

    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
    const auto& attrs = desc.vertexLayout.attributes;
    u32 packed = 0;   // 引擎没给 attributes 时按声明顺序紧密排列（与 Vulkan 从 SPIR-V 反射一致）
    for (u32 loc = 0; loc < inputs.size(); ++loc) {
        const ShaderInput& si = inputs[loc];
        D3D11_INPUT_ELEMENT_DESC e{};
        e.SemanticName   = si.p.SemanticName;   // 指针指向反射对象内部，CreateInputLayout 之前 refl 一直存活
        e.SemanticIndex  = si.p.SemanticIndex;
        e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        const VertexAttribute* a = nullptr;
        for (const auto& cand : attrs) if (cand.location == loc) { a = &cand; break; }
        if (a) {
            e.Format = ToDxgiVertexFormat(a->format);
            e.InputSlot = a->binding;
            e.AlignedByteOffset = a->offset;
        } else if (attrs.empty()) {
            e.Format = si.fmt;
            e.InputSlot = 0;
            e.AlignedByteOffset = packed;
            packed += si.bytes;
        } else {
            e.Format = DXGI_FORMAT_UNKNOWN;   // 引擎布局没提供 ⇒ 下面挂到零缓冲
        }
        // 引擎布局没提供、或超出顶点步长的属性（静态网格共用带蒙皮属性的顶点着色器）：
        // 挂到零缓冲槽读 0，与 Vulkan 未提供属性时一致
        const u32 stride = static_cast<u32>(desc.vertexLayout.stride);
        const bool overflow = (e.Format != DXGI_FORMAT_UNKNOWN) && e.InputSlot == 0 && stride > 0 &&
                              e.AlignedByteOffset + VertexFormatBytes(e.Format) > stride;
        if (e.Format == DXGI_FORMAT_UNKNOWN || overflow) {
            e.Format = si.fmt;
            e.InputSlot = kZeroVertexSlot;
            e.AlignedByteOffset = 0;
            m_NeedsZeroVB = true;
        }
        elems.push_back(e);
    }
    m_VertexStride = desc.vertexLayout.stride > 0 ? static_cast<u32>(desc.vertexLayout.stride) : packed;

    if (m_NeedsInstanceIndexVB) {
        D3D11_INPUT_ELEMENT_DESC e{};
        e.SemanticName         = "HE_INSTANCEINDEX";
        e.Format               = DXGI_FORMAT_R32_UINT;
        e.InputSlot            = kInstanceIndexSlot;
        e.InputSlotClass       = D3D11_INPUT_PER_INSTANCE_DATA;
        e.InstanceDataStepRate = 1;
        elems.push_back(e);
    }
    if (elems.empty()) return true;   // 无顶点输入（全屏三角形等用 SV_VertexID）

    if (!HE_D3D11_CHECK(device->GetDevice()->CreateInputLayout(elems.data(), static_cast<UINT>(elems.size()),
                                                               m_VsBytecode.data(), m_VsBytecode.size(),
                                                               &m_InputLayout), "CreateInputLayout")) {
        HE_CORE_ERROR("[D3D11] PSO {}：输入布局创建失败（{} 个元素，步长 {}）", m_Name,
                      static_cast<u32>(elems.size()), m_VertexStride);
        return false;
    }
    return true;
}

void D3D11PipelineState::CreateFixedFunctionStates(D3D11Device* device, const PipelineStateDesc& desc) {
    ID3D11Device5* dev = device->GetDevice();

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = ToD3D11FillMode(desc.fillMode);
    rs.CullMode = ToD3D11CullMode(desc.cullMode);
    // Vulkan 默认顺时针为正面；D3D 的 FrontCounterClockwise 语义相反（与 D3D12 后端一致）
    rs.FrontCounterClockwise = (desc.frontFace == FrontFace::CounterClockwise) ? TRUE : FALSE;
    rs.DepthClipEnable   = TRUE;
    rs.ScissorEnable     = TRUE;   // 命令列表在开始渲染通道时把裁剪矩形设为整个目标
    rs.MultisampleEnable = desc.sampleCount > 1 ? TRUE : FALSE;
    HE_D3D11_CHECK(dev->CreateRasterizerState(&rs, &m_Rasterizer), "CreateRasterizerState");

    // 深度：与 Vulkan 一样，关闭深度测试时深度写入也不生效
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable    = desc.depthTest ? TRUE : FALSE;
    ds.DepthWriteMask = desc.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc      = ToD3D11CompareFunc(desc.depthCompare);
    ds.StencilEnable  = FALSE;
    HE_D3D11_CHECK(dev->CreateDepthStencilState(&ds, &m_DepthStencil), "CreateDepthStencilState");

    // 混合：逐附件独立（与 Vulkan 的 per-attachment 混合一致）
    D3D11_BLEND_DESC bd{};
    bd.IndependentBlendEnable = TRUE;
    for (u32 i = 0; i < kMaxColorAttachments; ++i) {
        const ColorBlendDesc& c = desc.colorBlend[i];
        auto& rt = bd.RenderTarget[i];
        rt.BlendEnable    = c.blendEnable ? TRUE : FALSE;
        rt.SrcBlend       = ToD3D11Blend(c.srcColorBlendFactor);
        rt.DestBlend      = ToD3D11Blend(c.dstColorBlendFactor);
        rt.BlendOp        = ToD3D11BlendOp(c.colorBlendOp);
        rt.SrcBlendAlpha  = ToD3D11Blend(c.srcAlphaBlendFactor);
        rt.DestBlendAlpha = ToD3D11Blend(c.dstAlphaBlendFactor);
        rt.BlendOpAlpha   = ToD3D11BlendOp(c.alphaBlendOp);
        // 引擎的写入掩码位（R=1 G=2 B=4 A=8）与 D3D11_COLOR_WRITE_ENABLE_* 相同
        rt.RenderTargetWriteMask = static_cast<UINT8>(c.writeMask);
    }
    HE_D3D11_CHECK(dev->CreateBlendState(&bd, &m_Blend), "CreateBlendState");
}

} // namespace he::rhi::d3d11
