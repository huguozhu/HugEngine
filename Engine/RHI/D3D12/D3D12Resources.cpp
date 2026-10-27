#include <cstdio>
// ============================================================================
//  D3D12Resources.cpp —— Buffer / Texture / Sampler / PipelineState 的实现
// ============================================================================
#include "D3D12Resources.h"
#include "D3D12Device.h"
#include "D3D12Converters.h"
#include <d3dcompiler.h>
#include <dxcapi.h>       // IDxcUtils::CreateReflection（DXIL 反射）
#include <string>
#include <algorithm>
#include <memory>
#include <cstring>
#include <cstdlib>

#if defined(_WIN32)

namespace he::rhi::d3d12 {

// ===========================================================================
//  Buffer
// ===========================================================================
D3D12Buffer::~D3D12Buffer() {
    // 持久映射的缓冲要先解除映射再释放
    if (m_PersistentlyMapped && m_Resource) {
        m_Resource->Unmap(0, nullptr);
        m_PersistentlyMapped = false;
    }
}

// 【设备丢失即时定位】每次建缓冲后立刻查移除原因 —— 之前都是"事后"才发现设备丢了，
// 只能靠猜是哪一步。把检查放到每个创建点，就能一次锁定元凶。
static void CheckDeviceAlive(ID3D12Device* dev, const char* where) {
    if (!dev) return;
    const HRESULT r = dev->GetDeviceRemovedReason();
    if (FAILED(r)) {
        HE_CORE_ERROR("[设备丢失] 发生在 {} 之后, reason=0x{:08x}", where, static_cast<u32>(r));
    }
}

bool D3D12Buffer::Initialize(D3D12Device* device, const BufferDesc& desc) {
    // 每次建缓冲前先确认设备还活着 —— 之前只在创建后检查，结果发现设备在更早就已被移除
    if (m_Device && m_Device->GetDevice())
        CheckDeviceAlive(m_Device->GetDevice(), "进入 D3D12Buffer::Initialize 之前");
    if (!device || !device->GetDevice()) {
        HE_CORE_ERROR("D3D12Buffer: 设备为空");
        return false;
    }
    // 【D3D12 比 Vulkan 严格】CreateCommittedResource 拒绝 0 字节缓冲
    //（返回 E_INVALIDARG），而引擎在某些路径下会创建空缓冲（例如可选特性
    // 未启用时的占位缓冲）。这里按 256 字节对齐做最小分配，逻辑大小仍记 0，
    // 上层行为不变。
    ID3D12Device* dev = device->GetDevice();
    m_Device = device;   // Unmap 的暂存拷贝需要它
    m_Size   = desc.size;
    m_Stride = static_cast<u32>(desc.stride);

    // 光追缓冲（BLAS/TLAS 的存储、SBT）需要 D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE，
    // 而本后端的 BLAS/TLAS/SBT 属阶段 4，尚未实现。这里直接拒绝，
    // 避免 CreateCommittedResource 抛 E_INVALIDARG 噪音。
    // 【只跳过"纯光追"缓冲】引擎给**所有**顶点/索引缓冲都额外带上了
    // AccelerationStruct 标志（`Vertex | AccelerationStruct`），以便后续建 BLAS。
    // 之前在此无条件跳过，导致所有网格的顶点/索引缓冲都变成无效句柄 ⇒
    // 注册表里 mesh 的 buffer 为 null ⇒ 一个网格都画不出来 ⇒ 画面全黑。
    // 现在只在「除了光追标志之外没有任何可用用途」时才跳过。
    constexpr u32 kRtOnly = static_cast<u32>(BufferUsage::AccelerationStruct) |
                            static_cast<u32>(BufferUsage::ShaderBindingTable);
    const u32 usageBits = static_cast<u32>(desc.usage);
    if ((usageBits & kRtOnly) != 0 && (usageBits & ~kRtOnly) == 0) {
        HE_CORE_INFO("D3D12Buffer: 跳过纯光追缓冲（阶段 4 未实现）size={}", static_cast<u64>(desc.size));
        return false;
    }

    // 实际分配大小：至少 256 字节（D3D12 的常量缓冲/间接参数都要求 256 对齐）
    const u64 allocSize = (desc.size == 0) ? 256ull
                                          : ((static_cast<u64>(desc.size) + 255ull) & ~255ull);

    // 堆类型：
    //   · Storage（UAV）⇒ **必须** DEFAULT 堆。D3D12 禁止 UPLOAD 堆的缓冲带
    //     ALLOW_UNORDERED_ACCESS（实测 CreateCommittedResource 返回 E_INVALIDARG）。
    //     CPU 更新走「暂存缓冲 + CopyBufferRegion」，见下方 m_CpuStaging。
    //   · 其余 cpuAccess ⇒ UPLOAD 堆（可以直接 Map 写）
    //   · 回读 ⇒ READBACK 堆
    const bool isStorage = HasAny(desc.usage, BufferUsage::Storage);
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    // 【关键】带 initialData 的非 UAV 缓冲（顶点/索引/常量）直接用 UPLOAD 堆：
    //   · UPLOAD 堆可以立即 Map 写入，数据当场就有效
    //   · 之前走 DEFAULT 堆 + 中转缓冲，但**从未执行拷贝** ⇒ 顶点数据全 0
    //     ⇒ 三角形退化 ⇒ 画面全黑（实测就是这个问题）
    if (!isStorage && (HasAny(desc.usage, BufferUsage::TransferDst) || desc.cpuAccess ||
                       desc.initialData != nullptr)) {
        heapType = D3D12_HEAP_TYPE_UPLOAD;
    }
    // UAV 缓冲需要一份等大的 UPLOAD 暂存来承载 CPU 写入
    // UAV 缓冲一律配一份 UPLOAD 暂存：D3D12 的 DEFAULT 堆资源**不能** Map，
    // 而引擎对所有 SSBO 都会调用 Map()/Unmap() 写数据（不管有没有置 cpuAccess）
    // ⇒ 没有暂存就会在 Map 时失败（实测 02.Cube 初始化最后一步卡在这里）。
    const bool needStaging = isStorage;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment        = 0;
    rd.Width            = allocSize;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc       = { 1, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = D3D12_RESOURCE_FLAG_NONE;

    // 允许 UAV 的缓冲需要显式声明
    if (HasAny(desc.usage, BufferUsage::Storage)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heapType;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    const D3D12_RESOURCE_STATES initState =
        (heapType == D3D12_HEAP_TYPE_UPLOAD) ? D3D12_RESOURCE_STATE_GENERIC_READ
                                             : D3D12_RESOURCE_STATE_COMMON;
    m_State = initState;

    // 【设备丢失定位】每次创建**之前**先确认设备还活着，并打印本次参数。
    // 这样日志里「最后一次正常打印」与「第一次失败」之间的那一次就是元凶。
    CheckDeviceAlive(dev, "下一次 CreateCommittedResource 之前");
    HE_CORE_INFO("[建缓冲] size={} alloc={} usage=0x{:x} heap={} flags=0x{:x} cpuAccess={}",
                 static_cast<u64>(desc.size), allocSize, static_cast<u32>(desc.usage),
                 static_cast<int>(heapType), static_cast<u32>(rd.Flags), desc.cpuAccess);

    if (!HE_D3D12_CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initState,
                                                     nullptr, IID_PPV_ARGS(&m_Resource)),
                        "CreateCommittedResource(Buffer)")) {
        HE_CORE_ERROR("D3D12Buffer 失败: size={} alloc={} usage=0x{:x} cpuAccess={}",
                      static_cast<u64>(desc.size), allocSize,
                      static_cast<u32>(desc.usage), desc.cpuAccess);
        return false;
    }

    m_GpuAddress = m_Resource->GetGPUVirtualAddress();

    // UAV 缓冲的 CPU 暂存：Map() 返回它的映射指针，Unmap() 用 CopyBufferRegion 拷到 GPU
    if (needStaging) {
        D3D12_RESOURCE_DESC srd = rd;
        srd.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES shp{};
        shp.Type = D3D12_HEAP_TYPE_UPLOAD;
        shp.CreationNodeMask = 1;
        shp.VisibleNodeMask  = 1;
        if (FAILED(dev->CreateCommittedResource(&shp, D3D12_HEAP_FLAG_NONE, &srd,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&m_CpuStaging)))) {
            HE_CORE_WARN("D3D12Buffer: UAV 暂存缓冲创建失败，CPU 更新将不可用");
        }
        m_NeedsStagingCopy = (m_CpuStaging != nullptr);
    }

    // 初始数据：UPLOAD 堆可以直接 Map 写入；DEFAULT 堆需要中转缓冲 + 一次拷贝
    if (desc.initialData) {
        if (heapType == D3D12_HEAP_TYPE_UPLOAD) {
            void* dst = nullptr;
            D3D12_RANGE range{ 0, 0 };
            if (SUCCEEDED(m_Resource->Map(0, &range, &dst)) && dst) {
                std::memcpy(dst, desc.initialData, desc.size);
                m_Resource->Unmap(0, nullptr);
            }
        } else {
            // DEFAULT 堆（UAV 缓冲）：建 UPLOAD 中转 → 立刻用引擎队列拷一次。
            // 不留在"下次提交时再拷"是因为缓冲可能在本帧就被读，晚一拍就会读到全零。
            D3D12_RESOURCE_DESC ud = rd;
            // 【必须清掉 UAV 标志】UPLOAD 堆的资源不能带 ALLOW_UNORDERED_ACCESS，
            // 否则 CreateCommittedResource 直接返回 E_INVALIDARG（中转缓冲只是拷贝源，
            // 不需要 UAV）。rd 是从目标缓冲复制的，标志必须显式清掉。
            ud.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES uh{};
            uh.Type = D3D12_HEAP_TYPE_UPLOAD;
            uh.CreationNodeMask = 1;
            uh.VisibleNodeMask = 1;
            ComPtr<ID3D12Resource> staging;
            if (SUCCEEDED(dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &ud,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&staging)))) {
                void* dst = nullptr;
                D3D12_RANGE range{ 0, 0 };
                if (SUCCEEDED(staging->Map(0, &range, &dst)) && dst) {
                    std::memcpy(dst, desc.initialData, desc.size);
                    staging->Unmap(0, nullptr);
                }
                // 用引擎队列做拷贝（同队列 FIFO，避免跨队列争用导致 DEVICE_HUNG）
                ID3D12CommandQueue* q = device->GetQueue();
                ComPtr<ID3D12CommandAllocator>    a;
                ComPtr<ID3D12GraphicsCommandList> l;
                if (q &&
                    SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) &&
                    SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr,
                                                     IID_PPV_ARGS(&l)))) {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = m_Resource.Get();
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = initState;
                    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                    l->ResourceBarrier(1, &b);
                    l->CopyBufferRegion(m_Resource.Get(), 0, staging.Get(), 0, desc.size);
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    b.Transition.StateAfter  = initState;
                    l->ResourceBarrier(1, &b);
                    l->Close();
                    ID3D12CommandList* lists[1] = { l.Get() };
                    q->ExecuteCommandLists(1, lists);
                    ComPtr<ID3D12Fence> f;
                    if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) {
                        HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
                        if (ev) {
                            q->Signal(f.Get(), 1);
                            f->SetEventOnCompletion(1, ev);
                            WaitForSingleObject(ev, 5000);
                            CloseHandle(ev);
                        }
                    }
                }
            }
        }
    }
    HE_CORE_INFO("D3D12Buffer: 创建 {} 字节 heap={} usage=0x{:x}",
                 static_cast<u64>(desc.size),
                 (heapType == D3D12_HEAP_TYPE_UPLOAD ? "UPLOAD" : "DEFAULT"),
                 static_cast<u32>(desc.usage));
    return true;
}

void* D3D12Buffer::Map() {    std::printf("[BUF] Map enter\n"); std::fflush(stdout);

    if (!m_Resource) return nullptr;
    if (m_MappedPtr) return m_MappedPtr;

    D3D12_RANGE range{ 0, 0 };   // 声明 CPU 不读，驱动可以避免不必要的同步
    void* p = nullptr;

    // UAV 缓冲不能直接 Map，用暂存缓冲
    ID3D12Resource* target = m_NeedsStagingCopy ? m_CpuStaging.Get() : m_Resource.Get();
    if (!target) return nullptr;
    if (FAILED(target->Map(0, &range, &p))) {
        HE_CORE_ERROR("D3D12Buffer::Map 失败");
        return nullptr;
    }
    m_MappedPtr = p;
    m_PersistentlyMapped = true;
    return p;
}

void D3D12Buffer::Unmap() {    std::printf("[BUF] Unmap enter\n"); std::fflush(stdout);

    if (!m_Resource || !m_MappedPtr) return;

    ID3D12Resource* target = m_NeedsStagingCopy ? m_CpuStaging.Get() : m_Resource.Get();
    if (target) {
        D3D12_RANGE range{ 0, 0 };
        target->Unmap(0, &range);
    }

    // 暂存 → GPU 的实际拷贝（用引擎队列）。
    //
    // 【同步策略：单槽 + 无条件等待】
    // 多槽轮转时若上一代拷贝还没跑完就 Reset 分配器，D3D12 会直接判设备丢失
    //（实测 02.Cube 初始化阶段出现 0x887a0005）。这里改成只有一个槽，
    // 每次 Reset 前**必须**等上一次的围栏完成，正确性优先。
    HE_CORE_INFO("[Unmap 进入] size={} staging={}", static_cast<u64>(m_Size), m_NeedsStagingCopy ? 1 : 0);
    if (m_NeedsStagingCopy && m_CpuStaging && m_Device && m_Size > 0 &&
        std::getenv("HE_D3D12_STAGING_COPY") != nullptr) {
        const u32 copySize = static_cast<u32>((m_Size + 3) & ~3ull);

        ID3D12Device* dev = (m_Device->GetDevice()) ? m_Device->GetDevice() : nullptr;
        ID3D12CommandQueue* queue = m_Device->GetQueue();
        static ComPtr<ID3D12CommandAllocator>    s_alloc;
        static ComPtr<ID3D12GraphicsCommandList> s_list;
        static ComPtr<ID3D12Fence>               s_fence;
        static u64                               s_value = 0;
        static bool                              s_ready = false;

        if (dev && queue && !s_ready) {
            s_ready = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_alloc))) &&
                      SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_alloc.Get(),
                                                       nullptr, IID_PPV_ARGS(&s_list))) &&
                      SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_fence)));
            if (s_ready) { s_list->Close(); HE_CORE_INFO("D3D12Buffer: 暂存拷贝通道就绪"); }
        }

        if (dev && queue && s_ready) {
            // 等上一次拷贝完成（无条件，避免非法 Reset）
            if (s_value != 0 && s_fence->GetCompletedValue() < s_value) {
                HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
                if (ev) {
                    s_fence->SetEventOnCompletion(s_value, ev);
                    WaitForSingleObject(ev, 5000);   // 上限 5 秒，避免死等
                    CloseHandle(ev);
                }
            }
            if (SUCCEEDED(s_alloc->Reset()) && SUCCEEDED(s_list->Reset(s_alloc.Get(), nullptr))) {
                // 【必须转成 COPY_DEST】CopyBufferRegion 要求目标资源处于 COPY_DEST
                // 状态；UAV 缓冲当前是 UNORDERED_ACCESS，直接拷贝会让 D3D12
                // 判定非法操作并移除设备（实测 0x887a0005 就来自这里）。
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = m_Resource.Get();
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = m_State;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                s_list->ResourceBarrier(1, &b);

                s_list->CopyBufferRegion(m_Resource.Get(), 0, m_CpuStaging.Get(), 0, copySize);

                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter  = m_State;
                s_list->ResourceBarrier(1, &b);
                s_list->Close();
                ID3D12CommandList* lists[1] = { s_list.Get() };
                queue->ExecuteCommandLists(1, lists);
                s_value += 1;
                queue->Signal(s_fence.Get(), s_value);
            }
        }
    }

    m_MappedPtr = nullptr;
    m_PersistentlyMapped = false;
}

D3D12_VERTEX_BUFFER_VIEW D3D12Buffer::GetVbv(u32 stride) const {
    D3D12_VERTEX_BUFFER_VIEW v{};
    v.BufferLocation = m_GpuAddress;
    v.SizeInBytes    = static_cast<UINT>(m_Size);
    v.StrideInBytes  = stride;
    return v;
}

D3D12_INDEX_BUFFER_VIEW D3D12Buffer::GetIbv() const {
    D3D12_INDEX_BUFFER_VIEW v{};
    v.BufferLocation = m_GpuAddress;
    v.SizeInBytes    = static_cast<UINT>(m_Size);
    v.Format         = DXGI_FORMAT_R32_UINT;   // 调用方若用 16 位索引需另行指定
    return v;
}

// ===========================================================================
//  Texture
// ===========================================================================
D3D12Texture::~D3D12Texture() {
    // 注销视图句柄映射，避免地址被复用时查到错误的资源
    if (m_OwnerDevice) {
        if (m_Rtv.ptr) m_OwnerDevice->UnregisterViewResource(static_cast<UINT64>(m_Rtv.ptr));
        if (m_Dsv.ptr) m_OwnerDevice->UnregisterViewResource(static_cast<UINT64>(m_Dsv.ptr));
        for (auto& s : m_Srvs) if (s.ptr) m_OwnerDevice->UnregisterViewResource(static_cast<UINT64>(s.ptr));
    }
}

bool D3D12Texture::Initialize(D3D12Device* device, const TextureDesc& desc) {
    m_OwnerDevice = device;
    if (!device || !device->GetDevice()) {
        HE_CORE_ERROR("D3D12Texture: 设备为空");
        return false;
    }
    // 入口诊断：看清每个失败点到底卡在哪
    HE_CORE_INFO("[D3D12Texture 入口] {}x{} fmt={} mips={} layers={} samples={} usage=0x{:x}",
                 desc.width, desc.height, static_cast<u32>(desc.format),
                 desc.mipLevels, desc.arrayLayers, desc.sampleCount,
                 static_cast<u32>(desc.usage));
    ID3D12Device* dev = device->GetDevice();

    m_Width       = desc.width;
    m_Height      = desc.height;
    m_Depth       = desc.depth;
    m_MipLevels   = desc.mipLevels;
    m_ArrayLayers = desc.arrayLayers;
    m_Format      = desc.format;
    m_IsDepth     = IsDepthFormat(desc.format);

    const DXGI_FORMAT fmt = ToDxgiFormat(desc.format);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        HE_CORE_ERROR("D3D12Texture: 不支持的格式 {}", static_cast<u32>(desc.format));
        return false;
    }

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = (desc.depth > 1 || HasAny(desc.usage, TextureUsage::Cubemap))
                          ? D3D12_RESOURCE_DIMENSION_TEXTURE2D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Alignment        = 0;
    rd.Width            = desc.width;
    rd.Height           = desc.height;
    rd.DepthOrArraySize = static_cast<UINT16>(desc.arrayLayers * desc.depth);
    rd.MipLevels        = static_cast<UINT16>(desc.mipLevels);
    rd.Format           = fmt;
    rd.SampleDesc       = { desc.sampleCount, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_NONE;

    if (m_IsDepth) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        // 深度纹理同时当 SRV 用时不能加 DENY_SHADER_RESOURCE
        if (!HasAny(desc.usage, TextureUsage::ShaderResource)) {
            rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }
    } else if (HasAny(desc.usage, TextureUsage::RenderTarget)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (HasAny(desc.usage, TextureUsage::UnorderedAccess)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    // 渲染目标/深度需要显式清屏值（D3D12 要求创建时给优化清除值）
    D3D12_CLEAR_VALUE clearValue{};
    bool hasClear = false;
    if (m_IsDepth) {
        clearValue.Format = fmt;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;
        hasClear = true;
    } else if (HasAny(desc.usage, TextureUsage::RenderTarget)) {
        clearValue.Format = fmt;
        clearValue.Color[0] = 0.0f; clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f; clearValue.Color[3] = 1.0f;
        hasClear = true;
    }

    const D3D12_RESOURCE_STATES initState = ToInitialState(m_IsDepth, desc.usage);
    HE_CORE_INFO("[D3D12Texture] 即将创建: dimension={} {}x{} depthOrArray={} mips={} fmt={} samples={} flags=0x{:x}",
                 static_cast<u32>(rd.Dimension), static_cast<u32>(rd.Width), rd.Height,
                 static_cast<u32>(rd.DepthOrArraySize), rd.MipLevels,
                 static_cast<u32>(rd.Format), rd.SampleDesc.Count, static_cast<u32>(rd.Flags));
    HRESULT crh = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initState,
                                               hasClear ? &clearValue : nullptr,
                                               IID_PPV_ARGS(&m_Resource));
    if (!HE_D3D12_CHECK(crh, "CreateCommittedResource(Texture)")) {
        HE_CORE_ERROR("[D3D12Texture] CreateCommittedResource 失败 hr=0x{:08x}", static_cast<u32>(crh));
        return false;
    }

    // ---- 创建 RTV ----
    if (!m_IsDepth && HasAny(desc.usage, TextureUsage::RenderTarget)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        if (SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_RtvHeap)))) {
            m_Rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_RENDER_TARGET_VIEW_DESC vd{};
            vd.Format = fmt;
            vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = 0;
            vd.Texture2D.PlaneSlice = 0;
            dev->CreateRenderTargetView(m_Resource.Get(), &vd, m_Rtv);
    device->RegisterViewResource(static_cast<UINT64>(m_Rtv.ptr), m_Resource.Get());

        }
    }

    // ---- 创建 DSV ----
    if (m_IsDepth) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        if (SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_DsvHeap)))) {
            m_Dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_DEPTH_STENCIL_VIEW_DESC vd{};
            vd.Format = fmt;
            vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = 0;
            dev->CreateDepthStencilView(m_Resource.Get(), &vd, m_Dsv);
    device->RegisterViewResource(static_cast<UINT64>(m_Dsv.ptr), m_Resource.Get());
        }
    }

    // 【登记真实初始状态】命令列表做 RENDER_TARGET / DEPTH_WRITE 屏障时要靠它。
    // 不登记就会默认成 COMMON，从而跳过屏障，导致 OMSetRenderTargets 收到
    // 一个状态不对的资源而让驱动静默卡死。
    device->SetResourceState(m_Resource.Get(), initState);

    if (m_Width >= 1920) HE_CORE_ERROR("[TEX-BIG] res={} {}x{} dxgi={} depth={} rtvPtr={}",
        (void*)m_Resource.Get(), m_Width, m_Height, (u32)rd.Format, m_IsDepth ? 1 : 0, (void*)m_Rtv.ptr);
    HE_CORE_INFO("D3D12Texture: 创建 {}x{} mip={} layers={} fmt={}{}",
                 m_Width, m_Height, m_MipLevels, m_ArrayLayers, static_cast<u32>(fmt),
                 m_IsDepth ? " (深度)" : "");
    return true;
}

// ===========================================================================
//  Sampler
// ===========================================================================
D3D12Sampler::~D3D12Sampler() = default;

bool D3D12Sampler::Initialize(D3D12Device* device, const SamplerDesc& desc) {
    if (!device || !device->GetDevice()) return false;
    ID3D12Device* dev = device->GetDevice();

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;   // 阶段 1 先做非 shader-visible（走描述符表）
    if (!HE_D3D12_CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_Heap)), "CreateDescriptorHeap(Sampler)")) {
        return false;
    }
    m_Descriptor = m_Heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SAMPLER_DESC sd{};
    sd.Filter   = ToD3D12Filter(desc.minFilter, desc.maxAnisotropy > 1);
    sd.AddressU = ToD3D12AddressMode(desc.addressU);
    sd.AddressV = ToD3D12AddressMode(desc.addressV);
    sd.AddressW = ToD3D12AddressMode(desc.addressW);
    sd.MipLODBias = desc.mipLodBias;
    sd.MaxAnisotropy = desc.maxAnisotropy;
    sd.ComparisonFunc = desc.enableCompare ? ToD3D12CompareFunc(desc.compareFunc)
                                           : D3D12_COMPARISON_FUNC_NEVER;
    sd.MinLOD = desc.minLod;
    sd.MaxLOD = desc.maxLod;
    dev->CreateSampler(&sd, m_Descriptor);
    return true;
}

// ===========================================================================
//  PipelineState
// ===========================================================================
D3D12PipelineState::~D3D12PipelineState() = default;

bool D3D12PipelineState::Initialize(D3D12Device* device, const PipelineStateDesc& desc) {
    if (!device || !device->GetDevice()) return false;

    // 入口诊断：一次看清 VS/PS 字节码来自哪、dxil 是否非空、attributes 是否为空
    HE_CORE_INFO("[PSO 入口] name='{}' vs={} ps={} vsDxil={}B psDxil={}B attrs={} bindPoint={}",
                 desc.debugName.c_str(),
                 static_cast<const void*>(desc.vertexShader),
                 static_cast<const void*>(desc.pixelShader),
                 desc.vertexShader ? desc.vertexShader->dxil.size() : 0u,
                 desc.pixelShader  ? desc.pixelShader->dxil.size()  : 0u,
                 desc.vertexLayout.attributes.size(),
                 static_cast<int>(desc.bindPoint));
    ID3D12Device* dev = device->GetDevice();
    m_Topology = ToD3D12Topology(desc.topology);

    // ---- 根签名 ----
    //
    //  【必须与 DXIL 的寄存器布局严格对齐】ShaderToDxil 产出的 HLSL 里，
    //  所有资源都落在 **space0**，寄存器按下表的惯例分配：
    //      t0..t63  : SRV（StructuredBuffer / Texture2D 数组）
    //      s0..s15  : Sampler
    //      b0       : root constants（push constant）
    //  根签名只要少声明一类，PSO 就会被 D3D12 拒绝（E_INVALIDARG）。
    // ------------------------------------------------------------------
    //  参数布局（顺序很关键，CommandList 按编号绑定）
    //
    //    0 .. 7   : 8 个 root CBV（b0..b7，space0）
    //    8 .. 11  : SRV 表，space 0..3
    //   12 .. 15  : UAV 表，space 0..3
    //   16 .. 19  : Sampler 表，space 0..3
    //
    //  【为什么是 4 个 space】引擎的描述符集索引被 Slang/DXC 翻译成
    //  `register(tN, space<setIndex>)`（实测 PBR.vert 里
    //   u_InstanceTransforms 在 space1、u_VisibleInstanceIndices 在 space2、
    //   u_BoneMatrices 在 space3）。只声明 space0 会被拒绝：
    //  "Root Signature doesn't match Vertex Shader: Shader SRV descriptor range
    //   (... RegisterSpace=2) is not fully bound in root signature"。
    //
    //  【为什么还要 UAV 表】计算着色器往 SSBO/RWTexture 写数据，用的是
    //  `register(uN, spaceN)`；缺 UAV 范围会让计算 PSO 直接 E_INVALIDARG。
    //
    //  表都用**无界**（NumDescriptors = UINT_MAX）：着色器里既有定长数组
    //  （u_Textures[64]）也有无界数组（u_InstanceTransforms[]）。
    // ------------------------------------------------------------------
    static constexpr u32 kRootCbvCount = 8;
    // 【space 数量】实测引擎最多用到 space5（GBuffer 用到 space5），
    // 取 8 留余量。注意根签名的参数总数上限是 64 DWORD：
    // 8 个 root CBV + 3 类表 × 8 = 32，安全。
    static constexpr u32 kSpaceCount   = 8;
    static constexpr u32 kTableCount   = kSpaceCount * 3;   // SRV + UAV + Sampler

    D3D12_ROOT_PARAMETER  params[kRootCbvCount + kTableCount]{};
    D3D12_DESCRIPTOR_RANGE ranges[kTableCount]{};

    // 0..7：root CBV
    for (u32 i = 0; i < kRootCbvCount; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[i].Descriptor.ShaderRegister = i;   // b0..b7
        params[i].Descriptor.RegisterSpace  = 0;
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    // 三组表：SRV(0..3) / UAV(4..7) / SAMPLER(8..11)
    struct TableSpec { D3D12_DESCRIPTOR_RANGE_TYPE type; u32 rangeBase; u32 paramBase; };
    const TableSpec tables[3] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV,     0,                  kRootCbvCount                    },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV,     kSpaceCount,        kRootCbvCount + kSpaceCount      },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSpaceCount * 2,    kRootCbvCount + kSpaceCount * 2  },
    };

    for (const auto& spec : tables) {
        for (u32 s = 0; s < kSpaceCount; ++s) {
            const u32 ri = spec.rangeBase + s;
            ranges[ri].RangeType  = spec.type;
            ranges[ri].NumDescriptors = UINT_MAX;   // 无界
            ranges[ri].BaseShaderRegister = 0;
            ranges[ri].RegisterSpace = s;           // 描述符集索引 → 寄存器空间
            ranges[ri].OffsetInDescriptorsFromTableStart = 0;

            const u32 pi = spec.paramBase + s;
            params[pi].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[pi].DescriptorTable.NumDescriptorRanges = 1;
            params[pi].DescriptorTable.pDescriptorRanges = &ranges[ri];
            params[pi].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
    }

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = kRootCbvCount + kTableCount;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 0;
    rsd.pStaticSamplers = nullptr;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &sigBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            HE_CORE_ERROR("D3D12PipelineState: 根签名序列化失败: {}",
                          static_cast<const char*>(errBlob->GetBufferPointer()));
        }
        return false;
    }
    if (!HE_D3D12_CHECK(dev->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                                 sigBlob->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_RootSig)),
                        "CreateRootSignature")) {
        return false;
    }

    // ---- 计算管线 ----
    //
    // D3D12 的计算 PSO 与图形 PSO 是**两套完全不同的描述结构**，
    // 用 CreateGraphicsPipelineState 建计算管线会直接返回 E_INVALIDARG。
    // 之前只实现了图形分支，导致所有 compute pass（GPUCull / DDGI / 粒子…）
    // 的 PSO 都失败。
    if (desc.bindPoint == PipelineBindPoint::Compute ||
        (desc.computeShader && !desc.computeShader->dxil.empty())) {
        const ShaderBytecode* cs = desc.computeShader;
        if (!cs || cs->dxil.empty()) {
            HE_CORE_ERROR("D3D12PipelineState: 计算管线缺少 DXIL（name={}）", desc.debugName.c_str());
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = m_RootSig.Get();
        cd.CS = { cs->dxil.data(), cs->dxil.size() };
        cd.NodeMask = 0;
        cd.CachedPSO = {};
        cd.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        HRESULT chr = dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_Pso));
        if (FAILED(chr)) {
            HE_CORE_ERROR("D3D12PipelineState: 计算 PSO 创建失败 name={} hr=0x{:08x} CS={}B",
                          desc.debugName.c_str(), static_cast<u32>(chr),
                          static_cast<u32>(cd.CS.BytecodeLength));
            return false;
        }
        m_IsCompute = true;
        HE_CORE_INFO("D3D12PipelineState: 计算 PSO 创建成功 name={} CS={}B",
                     desc.debugName.c_str(), static_cast<u32>(cd.CS.BytecodeLength));
        return true;
    }

    // ---- PSO ----
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_RootSig.Get();

    // 着色器：从引擎的 ShaderBytecode 取 DXIL 字节
    // 引擎的 ShaderBytecode 有两个后端字节码：spirv（Vulkan）与 dxil（D3D12）。
    // D3D12 的 PSO 需要 DXIL 字节流 ⇒ 取 dxil 这个 vector。
    if (desc.vertexShader && !desc.vertexShader->dxil.empty()) {
        pd.VS = { desc.vertexShader->dxil.data(), desc.vertexShader->dxil.size() };
    } else {
        HE_CORE_WARN("D3D12PipelineState: 顶点着色器缺少 DXIL 字节码（name={}）", desc.debugName.c_str());
    }
    if (desc.pixelShader && !desc.pixelShader->dxil.empty()) {
        pd.PS = { desc.pixelShader->dxil.data(), desc.pixelShader->dxil.size() };
    }

    // 光栅化
    pd.RasterizerState.FillMode = ToD3D12FillMode(desc.fillMode);
    pd.RasterizerState.CullMode = ToD3D12CullMode(desc.cullMode);
    pd.RasterizerState.FrontCounterClockwise = ToD3D12FrontCCW(desc.frontFace);
    pd.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pd.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pd.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.RasterizerState.MultisampleEnable = FALSE;
    pd.RasterizerState.AntialiasedLineEnable = FALSE;
    pd.RasterizerState.ForcedSampleCount = 0;
    pd.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // 深度模板
    pd.DepthStencilState.DepthEnable = desc.depthTest ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL
                                                          : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = ToD3D12CompareFunc(desc.depthCompare);
    pd.DepthStencilState.StencilEnable = FALSE;

    // 输入布局
    // ------------------------------------------------------------------
    //  输入布局
    //
    //  两条路径：
    //    A. 引擎显式给了 attributes（少数样例）⇒ 直接按它建布局
    //    B. attributes 为空（多数样例，与 Vulkan 一致）⇒ **从 DXIL 反射**
    //
    //  为什么必须有 B：Vulkan 后端的输入布局是从 SPIR-V 反射出来的，样例因此
    //  只写 `.vertexLayout = {.stride = sizeof(Vertex)}` 而不填 attributes。
    //  D3D12 不做反射就会得到 0 个输入元素，而顶点着色器声明了 VERTEX_IN_0..N
    //  ⇒ CreateGraphicsPipelineState 返回 E_INVALIDARG（实测 hr=0x80070057）。
    // ------------------------------------------------------------------
    std::vector<D3D12_INPUT_ELEMENT_DESC> elems;

    // 【优先级】DXIL 反射 > 引擎显式 attributes。
    // 理由：attributes 是引擎侧的抽象（location/binding/format/offset），
    // 不保证与着色器的真实输入签名一致；D3D12 要求输入布局**严格匹配**着色器
    // 声明的语义与类型，否则 CreateGraphicsPipelineState 返回 E_INVALIDARG。
    // Vulkan 也是从 SPIR-V 反射的，所以以反射为准才与 Vulkan 行为一致。
    const bool haveDxil = (desc.vertexShader && !desc.vertexShader->dxil.empty());

    if (false) {
        elems.reserve(desc.vertexLayout.attributes.size());
        for (const auto& a : desc.vertexLayout.attributes) {
            D3D12_INPUT_ELEMENT_DESC e{};
            e.SemanticName   = "VERTEX_IN";
            e.SemanticIndex  = a.location;
            e.Format         = ToDxgiVertexFormat(a.format);
            e.InputSlot      = a.binding;
            e.AlignedByteOffset = a.offset;
            e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            e.InstanceDataStepRate = 0;
            elems.push_back(e);
        }
    }
    if (haveDxil && elems.empty()) {
        // 从 DXIL 反射输入签名。
        //
        // 【为什么不用 D3DReflect】`d3dcompiler.lib` 的 D3DReflect 是为 DXBC 设计的，
        // 对 SM 6.x 的 DXIL 解析不出输入签名（实测 InputParameters 恒为 0）。
        // 必须用 DXC 配套的 `IDxcUtils::CreateReflection`（dxcompiler.dll）。
        ComPtr<IDxcUtils> dxcUtils;
        ComPtr<ID3D12ShaderReflection> refl;
        HRESULT rh = E_FAIL;
        if (SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils))) && dxcUtils) {
            DxcBuffer dbuf{};
            dbuf.Ptr      = desc.vertexShader->dxil.data();
            dbuf.Size     = desc.vertexShader->dxil.size();
            dbuf.Encoding = DXC_CP_ACP;
            rh = dxcUtils->CreateReflection(&dbuf, IID_PPV_ARGS(&refl));
        }
        if (SUCCEEDED(rh) && refl) {
            D3D12_SHADER_DESC sd{};
            refl->GetDesc(&sd);

            // 【坑】语义名指针必须**稳定**：早先用 `static std::vector<std::string>`
            // 存名字，vector 扩容会让之前取出的 c_str() 指针全部失效
            // ⇒ D3D12 读到垃圾指针 ⇒ CreateGraphicsPipelineState 返回 E_INVALIDARG
            //（实测日志里语义名显示成「葺葺葺…」的乱码就是这个原因）。
            // 改用「固定容量的字符串数组」，指针永久稳定。
            static std::vector<std::unique_ptr<char[]>> s_semanticPool;
            s_semanticPool.reserve(256);

            uint32_t offset = 0;
            for (UINT i = 0; i < sd.InputParameters; ++i) {
                D3D12_SIGNATURE_PARAMETER_DESC p{};
                if (FAILED(refl->GetInputParameterDesc(i, &p))) continue;

                // 【必须跳过系统值语义】SV_VertexID / SV_InstanceID / SV_PrimitiveID
                // 这类由硬件自动提供，**不能**出现在输入布局里。
                // 早先没过滤，导致 PBR 的输入布局多出一个 VERTEX_IN_5 之外的
                // 系统值元素 ⇒ CreateGraphicsPipelineState 返回 E_INVALIDARG。
                if (p.SystemValueType != D3D_NAME_UNDEFINED) continue;

                // 由 ComponentType + Mask 推导 DXGI 格式。
                // 【要点】必须区分 32/16/8 位分量：早先只处理 32 位，遇到
                // FLOAT16 属性（例如切线/关节索引的打包格式）就给了 16 字节的
                // R32G32B32A32_FLOAT，导致 offset 累加越界
                // ⇒ CreateGraphicsPipelineState 返回 E_INVALIDARG。
                const UINT n = (p.Mask & 0x8) ? 4 : (p.Mask & 0x4) ? 3 : (p.Mask & 0x2) ? 2 : 1;
                DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
                u32 compBytes = 4;
                switch (p.ComponentType) {
                    case D3D_REGISTER_COMPONENT_FLOAT32:
                        compBytes = 4;
                        fmt = (n == 1) ? DXGI_FORMAT_R32_FLOAT : (n == 2) ? DXGI_FORMAT_R32G32_FLOAT
                            : (n == 3) ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
                        break;
                    case D3D_REGISTER_COMPONENT_FLOAT16:
                        compBytes = 2;
                        fmt = (n == 1) ? DXGI_FORMAT_R16_FLOAT : (n == 2) ? DXGI_FORMAT_R16G16_FLOAT
                            : DXGI_FORMAT_R16G16B16A16_FLOAT;
                        break;
                    case D3D_REGISTER_COMPONENT_UINT32:
                        compBytes = 4;
                        fmt = (n == 1) ? DXGI_FORMAT_R32_UINT : (n == 2) ? DXGI_FORMAT_R32G32_UINT
                            : (n == 3) ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
                        break;
                    case D3D_REGISTER_COMPONENT_UINT16:
                        compBytes = 2;
                        fmt = (n == 1) ? DXGI_FORMAT_R16_UINT : (n == 2) ? DXGI_FORMAT_R16G16_UINT
                            : DXGI_FORMAT_R16G16B16A16_UINT;
                        break;
                    case D3D_REGISTER_COMPONENT_SINT32:
                        compBytes = 4;
                        fmt = (n == 1) ? DXGI_FORMAT_R32_SINT : (n == 2) ? DXGI_FORMAT_R32G32_SINT
                            : (n == 3) ? DXGI_FORMAT_R32G32B32_SINT : DXGI_FORMAT_R32G32B32A32_SINT;
                        break;
                    case D3D_REGISTER_COMPONENT_SINT16:
                        compBytes = 2;
                        fmt = (n == 1) ? DXGI_FORMAT_R16_SINT : (n == 2) ? DXGI_FORMAT_R16G16_SINT
                            : DXGI_FORMAT_R16G16B16A16_SINT;
                        break;
                    default:
                        compBytes = 4;
                        fmt = DXGI_FORMAT_R32G32B32A32_FLOAT;
                        break;
                }

                // 拷贝到独立堆内存，指针稳定
                const char* semSrc = p.SemanticName ? p.SemanticName : "TEXCOORD";
                const size_t semLen = std::strlen(semSrc);
                auto buf = std::make_unique<char[]>(semLen + 1);
                std::memcpy(buf.get(), semSrc, semLen + 1);
                s_semanticPool.push_back(std::move(buf));

                D3D12_INPUT_ELEMENT_DESC e{};
                e.SemanticName   = s_semanticPool.back().get();
                e.SemanticIndex  = p.SemanticIndex;
                e.Format         = fmt;
                e.InputSlot      = 0;
                e.AlignedByteOffset = offset;
                e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                e.InstanceDataStepRate = 0;
                elems.push_back(e);

                offset += compBytes * n;
            }
            HE_CORE_INFO("D3D12PipelineState: 从 DXIL 反射出 {} 个顶点输入（stride={}）",
                         elems.size(), static_cast<u32>(desc.vertexLayout.stride));
            for (const auto& e : elems) {
                HE_CORE_INFO("  语义 {}{} 格式 {} slot={} offset={}",
                             e.SemanticName, e.SemanticIndex,
                             static_cast<u32>(e.Format), e.InputSlot, e.AlignedByteOffset);
            }
        } else {
            HE_CORE_WARN("D3D12PipelineState: DXIL 反射失败 hr=0x{:08x}（name={}）",
                         static_cast<u32>(rh), desc.debugName.c_str());
        }
    }
    // 记录输入布局的总步长：每个元素的 offset + 元素大小，取最大值
    {
        u32 maxEnd = 0;
        for (const auto& e : elems) {
            const u32 comps = (e.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16
                            : (e.Format == DXGI_FORMAT_R32G32B32_FLOAT)    ? 12
                            : (e.Format == DXGI_FORMAT_R32G32_FLOAT)       ? 8
                            : 4;
            maxEnd = std::max(maxEnd, e.AlignedByteOffset + comps);
        }
        m_VertexStride = (desc.vertexLayout.stride > 0) ? static_cast<u32>(desc.vertexLayout.stride) : maxEnd;
    }

    // 【实验开关】HE_D3D12_NO_INPUT_LAYOUT=1：清空输入布局，用于判定
    // PSO 的 E_INVALIDARG 是否来自输入布局（着色器仍要求顶点输入，
    // 清空后绘制会失败，但 PSO 若能创建成功就说明问题在输入布局）。
    if (std::getenv("HE_D3D12_NO_INPUT_LAYOUT")) {
        elems.clear();
        HE_CORE_WARN("D3D12PipelineState: 按 HE_D3D12_NO_INPUT_LAYOUT 清空输入布局（实验）");
    }

    // 【按引擎的顶点结构重排输入属性】反射出来的顺序是**着色器里的声明顺序**
    //（PBR 是 joints/weights/position/normal/uv，offset 一路累到 72 字节），
    // 而引擎的顶点缓冲是 position/normal/uv/tangent（48 字节步长）。
    // 直接用声明顺序会让位置读到错误的偏移 ⇒ 几何跑到屏幕外 ⇒ 画面全黑。
    // 这里按语义优先级重排并重算 offset，与引擎的 Vertex 结构对齐。
    if (!elems.empty()) {
        auto prio = [](const std::string& n) -> int {
            if (n.find("POSITION") != std::string::npos) return 0;
            if (n.find("NORMAL") != std::string::npos)   return 1;
            if (n.find("TEXCOORD") != std::string::npos) return 2;
            if (n.find("TANGENT") != std::string::npos)  return 3;
            if (n.find("COLOR") != std::string::npos)    return 4;
            return 5;   // 其余（蒙皮等）排在最后
        };
        std::sort(elems.begin(), elems.end(),
                  [&](const D3D12_INPUT_ELEMENT_DESC& x, const D3D12_INPUT_ELEMENT_DESC& y) {
                      const int px = prio(x.SemanticName), py = prio(y.SemanticName);
                      if (px != py) return px < py;
                      return x.SemanticIndex < y.SemanticIndex;
                  });
        u32 cursor = 0;
        for (auto& e : elems) {
            e.AlignedByteOffset = cursor;
            u32 comp = 1;
            switch (e.Format) {
                case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
                case DXGI_FORMAT_R32G32B32A32_SINT:  comp = 4; break;
                case DXGI_FORMAT_R32G32B32_FLOAT:    case DXGI_FORMAT_R32G32B32_UINT:
                case DXGI_FORMAT_R32G32B32_SINT:     comp = 3; break;
                case DXGI_FORMAT_R32G32_FLOAT:       case DXGI_FORMAT_R32G32_UINT:
                case DXGI_FORMAT_R32G32_SINT:        comp = 2; break;
                default: comp = 1; break;
            }
            cursor += comp * 4;
        }
        // 引擎明确给了步长就信引擎；否则用重排后的累计值
        if (desc.vertexLayout.stride == 0) m_VertexStride = cursor;
        HE_CORE_INFO("D3D12PipelineState: 输入布局按引擎顶点结构重排，共 {} 个元素，步长 {}",
                     static_cast<u32>(elems.size()), static_cast<u32>(cursor));
    }

    pd.InputLayout.pInputElementDescs = elems.empty() ? nullptr : elems.data();
    pd.InputLayout.NumElements = static_cast<UINT>(elems.size());

    // 输出合并
    pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pd.PrimitiveTopologyType = ToD3D12TopologyType(desc.topology);
    // 【关键】深度-only 的 pass（阴影贴图）colorFormats 是 Unknown。
    // D3D12 要求「NumRenderTargets 对应的每个 RTV 格式都必须是有效格式」，
    // 声明 1 个 RTV 却给 DXGI_FORMAT_UNKNOWN ⇒ CreateGraphicsPipelineState
    // 返回 E_INVALIDARG（实测 CSMDepth 就卡在这里）。
    // 因此只统计**有效**的颜色格式个数。
    UINT validRts = 0;
    for (u32 i = 0; i < desc.colorAttachmentCount && i < kMaxColorAttachments; ++i) {
        const DXGI_FORMAT cf = ToDxgiFormat(desc.colorFormats[i]);
        pd.RTVFormats[i] = cf;
        if (cf != DXGI_FORMAT_UNKNOWN) ++validRts;
    }
    // 【实验开关】HE_D3D12_NO_PS=1：去掉像素着色器与所有渲染目标，
    // 只保留顶点着色器，用于判定 E_INVALIDARG 是否来自 PS。
    if (std::getenv("HE_D3D12_NO_PS")) {
        pd.PS = {};
        for (u32 i = 0; i < kMaxColorAttachments; ++i) pd.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
        pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pd.NumRenderTargets = 0;
        HE_CORE_WARN("D3D12PipelineState: 按 HE_D3D12_NO_PS 去掉 PS 与 RT（实验）");
    } else {
        pd.NumRenderTargets = validRts;
    }
    pd.DSVFormat = ToDxgiFormat(desc.depthFormat);
    pd.SampleDesc = { desc.sampleCount, 0 };
    pd.SampleMask = UINT_MAX;
    pd.BlendState.AlphaToCoverageEnable = FALSE;
    pd.BlendState.IndependentBlendEnable = FALSE;
    for (u32 i = 0; i < kMaxColorAttachments; ++i) {
        auto& rt = pd.BlendState.RenderTarget[i];
        rt.BlendEnable = desc.colorBlend[i].blendEnable ? TRUE : FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend  = ToD3D12Blend(desc.colorBlend[i].srcColorBlendFactor);
        rt.DestBlend = ToD3D12Blend(desc.colorBlend[i].dstColorBlendFactor);
        rt.BlendOp   = ToD3D12BlendOp(desc.colorBlend[i].colorBlendOp);
        rt.SrcBlendAlpha  = ToD3D12Blend(desc.colorBlend[i].srcAlphaBlendFactor);
        rt.DestBlendAlpha = ToD3D12Blend(desc.colorBlend[i].dstAlphaBlendFactor);
        rt.BlendOpAlpha   = ToD3D12BlendOp(desc.colorBlend[i].alphaBlendOp);
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pd.NodeMask = 0;
    pd.CachedPSO = {};
    pd.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // 诊断：把 PSO 的关键参数打出来（E_INVALIDARG 最常见的原因是格式非法/采样数非法）
    HE_CORE_INFO("[D3D12 PSO] name={} VS={}B PS={}B RTV0={} DSV={} samples={} topoType={} elems={}",
                 desc.debugName.c_str(),
                 static_cast<u32>(pd.VS.BytecodeLength), static_cast<u32>(pd.PS.BytecodeLength),
                 static_cast<u32>(pd.RTVFormats[0]), static_cast<u32>(pd.DSVFormat),
                 pd.SampleDesc.Count, static_cast<u32>(pd.PrimitiveTopologyType),
                 pd.InputLayout.NumElements);

    // 采样数必须 >= 1；引擎在非 MSAA 路径上可能传 0
    if (pd.SampleDesc.Count == 0) pd.SampleDesc.Count = 1;

    HRESULT psoHr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_Pso));
    if (FAILED(psoHr)) {
        HE_CORE_ERROR("D3D12PipelineState: PSO 创建失败 name={} hr=0x{:08x}",
                      desc.debugName.c_str(), static_cast<u32>(psoHr));
        // 立刻把调试层的**全部**消息打出来（旧实现只收 ERROR 级，
        // 但 PSO 校验失败的原因常常落在 WARNING 里）
        if (auto* iq = device->GetInfoQueue()) {
            UINT64 n = iq->GetNumStoredMessages();
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                if (!len) continue;
                std::vector<char> buf(len);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (SUCCEEDED(iq->GetMessage(i, msg, &len))) {
                    if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                        HE_CORE_ERROR("  [D3D12 msg] sev={} id={} {}", static_cast<int>(msg->Severity),
                                      static_cast<u32>(msg->ID), msg->pDescription);
                    }
                }
            }
            iq->ClearStoredMessages();
        }
        // 打印关键参数便于人工核对
        HE_CORE_ERROR("  VS={} PS={} RTV0={} DSV={} NumRT={} samples={} topoType={} elems={} stride={}",
                      static_cast<u32>(pd.VS.BytecodeLength), static_cast<u32>(pd.PS.BytecodeLength),
                      static_cast<u32>(pd.RTVFormats[0]), static_cast<u32>(pd.DSVFormat),
                      pd.NumRenderTargets, pd.SampleDesc.Count,
                      static_cast<u32>(pd.PrimitiveTopologyType), pd.InputLayout.NumElements, m_VertexStride);
        return false;
    }
    HE_CORE_INFO("D3D12PipelineState: PSO 创建成功 name={}", desc.debugName.c_str());
    return true;
}

} // namespace he::rhi::d3d12

#endif // _WIN32
