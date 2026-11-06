// ============================================================================
//  D3D12RayTracing.cpp —— DXR 光追对象与根签名的实现
//
//  参考实现：Engine/RHI/Vulkan/VulkanRT.cpp（AS / RTPSO 的构造与句柄取法）。
//  实施手册：docs/计划实现功能/D3D12剩余任务实施指南.md 的 T5.1 / T5.4 / T5.6。
// ============================================================================
#include "D3D12RayTracing.h"
#include "D3D12Device.h"
#include "D3D12Converters.h"

#include <dxcapi.h>     // IDxcUtils::CreateReflection（库反射）

#include <cstring>
#include <string>

#if defined(_WIN32)

namespace he::rhi::d3d12 {

// ===========================================================================
//  引擎统一根签名
//
//  【为什么必须是同一份】光追管线的全局根签名与图形 / 计算管线共用同一套寄存器
//  布局（引擎的 ShaderToDxil 把描述符集索引编成 space、binding 编成寄存器号）。
//  三处各写一份迟早会漂移，故收敛到这里，图形 / 计算 PSO 与 RT 状态对象都调用它。
// ===========================================================================
bool CreateEngineRootSignature(ID3D12Device* device, ID3D12RootSignature** outRootSig) {
    if (!device || !outRootSig) return false;

    //  参数布局（顺序很关键，CommandList 按编号绑定）
    //    0 .. 7   : 8 个 root CBV（b0..b7，space0）
    //    8 .. 15  : SRV 表，space 0..7（额外附带 b8.. 的 CBV 范围）
    //   16 .. 23  : UAV 表，space 0..7
    //   24 .. 31  : Sampler 表，space 0..7
    static constexpr u32 kRootCbvCount = 8;
    static constexpr u32 kSpaceCount   = 8;
    static constexpr u32 kTableCount   = kSpaceCount * 3;   // SRV + UAV + Sampler

    D3D12_ROOT_PARAMETER  params[kRootCbvCount + kTableCount]{};
    D3D12_DESCRIPTOR_RANGE ranges[kTableCount]{};

    // 0..7：root CBV（推送常量走常量环，见 D3D12CommandList::AllocateConstants）
    for (u32 i = 0; i < kRootCbvCount; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[i].Descriptor.ShaderRegister = i;   // b0..b7
        params[i].Descriptor.RegisterSpace  = 0;
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    // 三组表：SRV / UAV / SAMPLER，各按 space 展开 8 份
    struct TableSpec { D3D12_DESCRIPTOR_RANGE_TYPE type; u32 rangeBase; u32 paramBase; };
    const TableSpec tables[3] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV,     0,                  kRootCbvCount                    },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV,     kSpaceCount,        kRootCbvCount + kSpaceCount      },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSpaceCount * 2,    kRootCbvCount + kSpaceCount * 2  },
    };

    // 【描述符集里的常量缓冲】SRV 表额外带一段 CBV 范围：寄存器 b(kSetCbvRegisterBase + binding)，
    // 与 SRV 共用同一段描述符（偏移都从表头起），即 b(8+k) 对应「set 基址 + k」——
    // 正是 UpdateDescriptorSet(UniformBuffer) 写 CBV 的位置。寄存器从 b8 起是为了避开
    // b0..b7 的 root CBV（推送常量）；ShaderToDxil 按同一规则给带 vk::binding 的 cbuffer 分配寄存器。
    static constexpr u32 kSetCbvRegisterBase = 8;
    static constexpr u32 kSetCbvCount        = 64;
    D3D12_DESCRIPTOR_RANGE srvTableRanges[kSpaceCount][2]{};

    for (const auto& spec : tables) {
        for (u32 s = 0; s < kSpaceCount; ++s) {
            const u32 ri = spec.rangeBase + s;
            ranges[ri].RangeType  = spec.type;
            ranges[ri].NumDescriptors = UINT_MAX;   // 无界：着色器里既有定长数组也有无界数组
            ranges[ri].BaseShaderRegister = 0;
            ranges[ri].RegisterSpace = s;           // 描述符集索引 → 寄存器空间
            ranges[ri].OffsetInDescriptorsFromTableStart = 0;

            const u32 pi = spec.paramBase + s;
            params[pi].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[pi].DescriptorTable.NumDescriptorRanges = 1;
            params[pi].DescriptorTable.pDescriptorRanges = &ranges[ri];
            params[pi].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            if (spec.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
                // 有界的 CBV 范围在前，无界的 SRV 范围必须放在最后
                D3D12_DESCRIPTOR_RANGE& cbv = srvTableRanges[s][0];
                cbv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                cbv.NumDescriptors = kSetCbvCount;
                cbv.BaseShaderRegister = kSetCbvRegisterBase;
                cbv.RegisterSpace = s;
                cbv.OffsetInDescriptorsFromTableStart = 0;
                srvTableRanges[s][1] = ranges[ri];
                params[pi].DescriptorTable.NumDescriptorRanges = 2;
                params[pi].DescriptorTable.pDescriptorRanges = srvTableRanges[s];
            }
        }
    }

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = kRootCbvCount + kTableCount;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 0;
    rsd.pStaticSamplers = nullptr;
    // 输入装配标志只对图形 PSO 有意义；光追的全局根签名不需要它
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    const HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   &sigBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            HE_CORE_ERROR("CreateEngineRootSignature: 根签名序列化失败: {}",
                          static_cast<const char*>(errBlob->GetBufferPointer()));
        }
        return false;
    }
    if (!HE_D3D12_CHECK(device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                                    sigBlob->GetBufferSize(),
                                                    IID_PPV_ARGS(outRootSig)),
                        "CreateRootSignature")) {
        return false;
    }
    return true;
}

// ===========================================================================
//  标志 / 几何描述转换
// ===========================================================================
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS ToD3D12ASBuildFlags(ASBuildFlags flags) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS f =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
    if (HasAny(flags, ASBuildFlags::AllowUpdate))     f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    if (HasAny(flags, ASBuildFlags::AllowCompaction)) f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
    if (HasAny(flags, ASBuildFlags::PreferFastTrace)) f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (HasAny(flags, ASBuildFlags::PreferFastBuild)) f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    if (HasAny(flags, ASBuildFlags::MinimizeMemory))  f |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
    return f;
}

void FillD3D12GeometryDesc(const RTGeometryDesc& src, D3D12_RAYTRACING_GEOMETRY_DESC& dst) {
    dst = {};
    dst.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    // 与 Vulkan 侧保持一致：三角形几何一律按不透明处理（AnyHit 需要穿透时由调用方处理）
    dst.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& tri = dst.Triangles;
    tri.Transform3x4   = src.transformBuffer ? src.transformBuffer->GetDeviceAddress() : 0;
    // 引擎的 Format 目前只有 R32_UINT 一种整数索引格式（16 位索引尚未使用）
    tri.IndexFormat    = (src.indexFormat == Format::R32_UINT) ? DXGI_FORMAT_R32_UINT
                                                              : DXGI_FORMAT_R16_UINT;
    tri.VertexFormat   = DXGI_FORMAT_R32G32B32_FLOAT;
    tri.IndexCount     = src.maxPrimitiveCount * 3u;   // 索引数 = 图元数 × 3
    tri.VertexCount    = src.maxVertex;
    tri.IndexBuffer    = src.indexBuffer ? src.indexBuffer->GetDeviceAddress() : 0;
    tri.VertexBuffer.StartAddress  = src.vertexBuffer ? src.vertexBuffer->GetDeviceAddress() : 0;
    tri.VertexBuffer.StrideInBytes = src.vertexStride ? src.vertexStride : 12u;
}

// ===========================================================================
//  D3D12AccelerationStructure
// ===========================================================================
bool D3D12AccelerationStructure::Initialize(D3D12Device* device, AccelerationStructureType type,
                                            u64 size) {
    if (!device || !device->GetDevice()) {
        HE_CORE_ERROR("D3D12AccelerationStructure: 设备为空");
        return false;
    }
    m_Type = type;
    m_Size = size;
    if (size == 0) {
        HE_CORE_ERROR("D3D12AccelerationStructure: 尺寸为 0（Get*BuildSizes 是否失败？）");
        return false;
    }

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment        = 0;
    rd.Width            = size;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc       = { 1, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // 加速结构必须允许 UAV（构建 = GPU 写），并放在 DEFAULT 堆
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type                 = D3D12_HEAP_TYPE_DEFAULT;
    hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask     = 1;
    hp.VisibleNodeMask      = 1;

    // 初始状态就是 RAYTRACING_ACCELERATION_STRUCTURE：规范允许直接以该状态创建，
    // 且加速结构**只能**停在该状态（构建前后发 UAV 屏障即可），后续不再做状态转换。
    if (!HE_D3D12_CHECK(device->GetDevice()->CreateCommittedResource(
                            &hp, D3D12_HEAP_FLAG_NONE, &rd,
                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                            nullptr, IID_PPV_ARGS(&m_Resource)),
                        "CreateCommittedResource(AccelerationStructure)")) {
        return false;
    }
    m_Address = m_Resource->GetGPUVirtualAddress();
    HE_CORE_INFO("D3D12AccelerationStructure: {} size={}KB addr={:#x}",
                 (type == AccelerationStructureType::BottomLevel) ? "BLAS" : "TLAS",
                 size / 1024, m_Address);
    return true;
}

// ===========================================================================
//  D3D12RayTracingPipelineState
// ===========================================================================
bool D3D12RayTracingPipelineState::Initialize(D3D12Device* device, const RTPipelineStateDesc& desc) {
    if (!device) return false;
    ID3D12Device5* dev5 = device->GetDevice5();
    if (!dev5) {
        HE_CORE_ERROR("D3D12RayTracingPipelineState: 取不到 ID3D12Device5（无 DXR）");
        return false;
    }
    if (desc.shaders.empty() || desc.shaderGroups.empty()) {
        HE_CORE_ERROR("D3D12RayTracingPipelineState: shader / 组为空");
        return false;
    }

    const u32 shaderCount = static_cast<u32>(desc.shaders.size());
    const u32 groupCount  = static_cast<u32>(desc.shaderGroups.size());

    // ---- 0. 全局根签名（与图形 / 计算同一布局）----
    if (!CreateEngineRootSignature(device->GetDevice(), &m_RootSig)) return false;

    // ---- 1. 每个 shader 一个 DXIL 库子对象，导出名 S<i> ----
    //
    //  名字必须**活到 CreateStateObject 返回**（D3D12_EXPORT_DESC / HIT_GROUP_DESC 里
    //  全是裸 LPCWSTR）。用 reserve 好的 vector<wstring> 保证元素地址稳定。
    std::vector<std::wstring> shaderNames;
    shaderNames.reserve(shaderCount);
    for (u32 i = 0; i < shaderCount; ++i) shaderNames.push_back(L"S" + std::to_wstring(i));

    std::vector<D3D12_EXPORT_DESC>       exports(shaderCount);
    std::vector<D3D12_DXIL_LIBRARY_DESC> libDescs(shaderCount);
    for (u32 i = 0; i < shaderCount; ++i) {
        const ShaderBytecode& bc = desc.shaders[i];
        if (bc.dxil.empty()) {
            HE_CORE_ERROR("D3D12RayTracingPipelineState: 第 {} 个 shader（stage={}）缺少 DXIL 字节码",
                          i, static_cast<int>(bc.stage));
            return false;
        }
        exports[i].Name           = shaderNames[i].c_str();
        exports[i].ExportToRename = L"main";     // 源码入口统一叫 main，状态对象内改名保证唯一
        exports[i].Flags          = D3D12_EXPORT_FLAG_NONE;

        libDescs[i] = {};
        // 注意字段名：Windows SDK 里叫 DXILLibrary（Agility SDK 新增了 DXILT 变体，
        // 字段改名为 DXILOrDXILTBytecode；这里用系统 SDK 的名字）
        libDescs[i].DXILLibrary.pShaderBytecode = bc.dxil.data();
        libDescs[i].DXILLibrary.BytecodeLength  = bc.dxil.size();
        libDescs[i].NumExports   = 1;
        libDescs[i].pExports     = &exports[i];
    }

    // ---- 2. 命中组：H<k>（k 为命中组的序号）----
    u32 hitGroupCount = 0;
    for (const auto& g : desc.shaderGroups) if (g.type == RTShaderGroupType::Hit) ++hitGroupCount;

    std::vector<std::wstring>             hitNames;
    hitNames.reserve(hitGroupCount);
    for (u32 k = 0; k < hitGroupCount; ++k) hitNames.push_back(L"H" + std::to_wstring(k));

    std::vector<D3D12_HIT_GROUP_DESC> hitDescs(hitGroupCount);
    {
        u32 k = 0;
        for (const auto& g : desc.shaderGroups) {
            if (g.type != RTShaderGroupType::Hit) continue;
            D3D12_HIT_GROUP_DESC& h = hitDescs[k];
            h = {};
            h.HitGroupExport = hitNames[k].c_str();
            // 有 intersection shader ⇒ 程序化图元；否则三角形命中组
            if (g.intersectionShader != kRTShaderUnused && g.intersectionShader < shaderCount) {
                h.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
                h.IntersectionShaderImport = shaderNames[g.intersectionShader].c_str();
            } else {
                h.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
            }
            if (g.closestHitShader != kRTShaderUnused && g.closestHitShader < shaderCount)
                h.ClosestHitShaderImport = shaderNames[g.closestHitShader].c_str();
            if (g.anyHitShader != kRTShaderUnused && g.anyHitShader < shaderCount)
                h.AnyHitShaderImport = shaderNames[g.anyHitShader].c_str();
            ++k;
        }
    }

    // ---- 3. 着色器配置 / 管线配置 / 全局根签名 ----
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
    shaderConfig.MaxPayloadSizeInBytes   = desc.maxPayloadSize ? desc.maxPayloadSize : 4u;
    shaderConfig.MaxAttributeSizeInBytes = desc.maxHitAttributeSize;

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
    pipelineConfig.MaxTraceRecursionDepth = desc.maxRecursionDepth ? desc.maxRecursionDepth : 1u;

    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSig{};
    globalRootSig.pGlobalRootSignature = m_RootSig.Get();

    // ---- 4. 组装子对象 ----
    std::vector<D3D12_STATE_SUBOBJECT> subobjects;
    subobjects.reserve(shaderCount + hitGroupCount + 3);
    auto addSubobject = [&subobjects](D3D12_STATE_SUBOBJECT_TYPE type, const void* pDesc) {
        D3D12_STATE_SUBOBJECT so{};
        so.Type  = type;
        so.pDesc = pDesc;
        subobjects.push_back(so);
    };
    for (u32 i = 0; i < shaderCount; ++i)
        addSubobject(D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDescs[i]);
    for (u32 k = 0; k < hitGroupCount; ++k)
        addSubobject(D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitDescs[k]);
    addSubobject(D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig);
    addSubobject(D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig);
    addSubobject(D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRootSig);

    D3D12_STATE_OBJECT_DESC soDesc{};
    soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    soDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
    soDesc.pSubobjects   = subobjects.data();

    if (!HE_D3D12_CHECK(dev5->CreateStateObject(&soDesc, IID_PPV_ARGS(&m_StateObject)),
                        "CreateStateObject")) {
        HE_CORE_ERROR("D3D12RayTracingPipelineState: 状态对象创建失败（name={}, shaders={}, groups={}）",
                      desc.debugName.c_str(), shaderCount, groupCount);
        return false;
    }
    m_StateObject.As(&m_Properties);
    if (!m_Properties) {
        HE_CORE_ERROR("D3D12RayTracingPipelineState: 取不到 ID3D12StateObjectProperties");
        return false;
    }
    if (!desc.debugName.empty())
        m_StateObject->SetName(Utf8ToWide(desc.debugName.c_str()).c_str());

    // ---- 5. 组句柄：按 shaderGroups 顺序拼接，每组 32 字节 ----
    //
    //  RayGen / Miss / Callable 取 generalShader 对应的导出标识符；
    //  Hit 取命中组的导出标识符。引擎的 BuildSBT 再按类型分段写进 SBT 缓冲。
    m_HandleSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    m_GroupCount = groupCount;
    m_Handles.assign(static_cast<size_t>(groupCount) * m_HandleSize, 0u);
    {
        u32 hitOrdinal = 0;
        for (u32 g = 0; g < groupCount; ++g) {
            const RTShaderGroup& group = desc.shaderGroups[g];
            const wchar_t* exportName = nullptr;
            if (group.type == RTShaderGroupType::Hit) {
                exportName = (hitOrdinal < hitGroupCount) ? hitNames[hitOrdinal].c_str() : nullptr;
                ++hitOrdinal;
            } else if (group.generalShader != kRTShaderUnused && group.generalShader < shaderCount) {
                exportName = shaderNames[group.generalShader].c_str();
            }
            if (!exportName) {
                HE_CORE_WARN("D3D12RayTracingPipelineState: 第 {} 组没有对应导出，句柄填 0", g);
                continue;
            }
            const void* id = m_Properties->GetShaderIdentifier(exportName);
            if (!id) {
                HE_CORE_ERROR("D3D12RayTracingPipelineState: GetShaderIdentifier 失败（组 {}）", g);
                continue;
            }
            std::memcpy(m_Handles.data() + static_cast<size_t>(g) * m_HandleSize, id, m_HandleSize);
        }
    }

    // ---- 6. 资源状态反射（库反射）----
    //
    //  普通 PSO 用 ID3D12ShaderReflection 反射出 SRV/UAV 的 space+binding，命令列表在
    //  绘制/派发前据此补状态转换。库是一个容器里含多个入口函数，必须走
    //  ID3D12LibraryReflection 逐函数遍历，填**同样口径**的 ResourceUses。
    //  dxc 若拒绝为库出反射（返回失败），ResourceUses 保持为空，命令列表会退回到
    //  「按描述符类型保守转换」的路径（见 D3D12CommandList::EnsureBoundResourceStates）。
    {
        auto appendUses = [this](D3D12_SHADER_INPUT_BIND_DESC& bd) {
            switch (bd.Type) {
                case D3D_SIT_TEXTURE: case D3D_SIT_STRUCTURED: case D3D_SIT_BYTEADDRESS:
                    m_ResourceUses.push_back({ bd.Space, bd.BindPoint, false });
                    break;
                case D3D_SIT_UAV_RWTYPED: case D3D_SIT_UAV_RWSTRUCTURED:
                case D3D_SIT_UAV_RWBYTEADDRESS: case D3D_SIT_UAV_APPEND_STRUCTURED:
                case D3D_SIT_UAV_CONSUME_STRUCTURED: case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
                    m_ResourceUses.push_back({ bd.Space, bd.BindPoint, true });
                    break;
                default:
                    break;   // CBV / 采样器 / 加速结构（TLAS）不涉及资源状态转换
            }
        };

        bool reflected = false;
        ComPtr<IDxcUtils> utils;
        if (SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) && utils) {
            // 逐个 shader 都反射：库反射也能拿到**结构化缓冲的步长**，
            // 必须登记到描述符集布局上（否则 SRV 的 StructureByteStride 退化成 4，
            // GPU 验证报 id=1387，着色器按错误步长读 SSBO）。
            for (u32 i = 0; i < shaderCount; ++i) {
                DxcBuffer db{};
                db.Ptr      = desc.shaders[i].dxil.data();
                db.Size     = desc.shaders[i].dxil.size();
                db.Encoding = DXC_CP_ACP;
                ComPtr<ID3D12LibraryReflection> libRefl;
                if (FAILED(utils->CreateReflection(&db, IID_PPV_ARGS(&libRefl))) || !libRefl) continue;

                device->NoteSetLayoutStridesFromLibrary(libRefl.Get(), desc.descriptorSetLayouts);

                D3D12_LIBRARY_DESC ld{};
                if (FAILED(libRefl->GetDesc(&ld))) continue;
                for (UINT f = 0; f < ld.FunctionCount; ++f) {
                    ID3D12FunctionReflection* fr = libRefl->GetFunctionByIndex(f);
                    if (!fr) continue;
                    D3D12_FUNCTION_DESC fd{};
                    if (FAILED(fr->GetDesc(&fd))) continue;
                    for (UINT r = 0; r < fd.BoundResources; ++r) {
                        D3D12_SHADER_INPUT_BIND_DESC bd{};
                        if (FAILED(fr->GetResourceBindingDesc(r, &bd))) continue;
                        appendUses(bd);
                    }
                }
                reflected = true;
            }
        }
        if (!reflected) {
            HE_CORE_WARN("D3D12RayTracingPipelineState: 库反射不可用（name={}），"
                         "光追资源状态改由描述符类型推断", desc.debugName.c_str());
        } else {
            HE_CORE_INFO("D3D12RayTracingPipelineState: {}（{} shaders, {} 组）资源用途 {} 项",
                         desc.debugName.c_str(), shaderCount, groupCount, m_ResourceUses.size());
        }
    }

    HE_CORE_INFO("D3D12RayTracingPipelineState: 创建成功 name={} groups={} recursion={} payload={}B",
                 desc.debugName.c_str(), groupCount, desc.maxRecursionDepth, desc.maxPayloadSize);
    return true;
}

} // namespace he::rhi::d3d12

#endif // _WIN32
