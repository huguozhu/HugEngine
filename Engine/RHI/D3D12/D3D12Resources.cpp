#include <cstdio>
// ============================================================================
//  D3D12Resources.cpp —— Buffer / Texture / Sampler / PipelineState 的实现
// ============================================================================
#include "D3D12Resources.h"
#include "D3D12Device.h"
#include "D3D12RayTracing.h"   // CreateEngineRootSignature（图形 / 计算 / 光追共用）
#include "D3D12Converters.h"
#include <d3dcompiler.h>
#include <dxcapi.h>       // IDxcUtils::CreateReflection（DXIL 反射）
#include <string>
#include <algorithm>
#include <cmath>
#include <memory>
#include <cstring>
#include <cstdlib>

#if defined(_WIN32)

namespace he::rhi::d3d12 {

// ===========================================================================
//  Buffer
// ===========================================================================
D3D12Buffer::~D3D12Buffer() {
    if (m_Device) m_Device->UnregisterBufferAddress(m_GpuAddress);   // 注销 GPU 地址登记
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
        DumpDeviceRemovedInfo(dev);
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

    // 光追缓冲的创建说明（T5）：加速结构本身由 D3D12AccelerationStructure 自己创建，
    // 不经过本类；经过本类的是 SBT / scratch / TLAS 实例缓冲，按下面 T5.3 的分支区分堆类型。
    // 【引擎给所有顶点/索引缓冲都带 AccelerationStruct 标志】以便后续建 BLAS；
    // 它们走「非 Storage + initialData ⇒ UPLOAD 堆」的同一条普通路径。

    // 实际分配大小：至少 256 字节（D3D12 的常量缓冲/间接参数都要求 256 对齐）
    const u64 allocSize = (desc.size == 0) ? 256ull
                                          : ((static_cast<u64>(desc.size) + 255ull) & ~255ull);

    // ------------------------------------------------------------------
    //  堆类型
    //
    //    · Storage（UAV）⇒ **必须** DEFAULT 堆。D3D12 禁止 UPLOAD 堆的缓冲带
    //      ALLOW_UNORDERED_ACCESS（实测 CreateCommittedResource 返回 E_INVALIDARG）。
    //      CPU 更新走「暂存缓冲 + CopyBufferRegion」，见下方 m_CpuStaging。
    //      （本后端实际用 CPU 可见的自定义堆承载 Storage 缓冲，见 cpuVisibleUav。）
    //    · 其余 cpuAccess ⇒ UPLOAD 堆（可以直接 Map 写）
    //    · 回读 ⇒ READBACK 堆
    //
    //  【光追缓冲的堆类型（手册 T5.3）】引擎给光追创建的三类缓冲用途位几乎相同，
    //  但 D3D12 的存放位置要求完全不同：
    //    · SBT（带 ShaderBindingTable）：CPU 写一次、GPU 只读 ⇒ UPLOAD 堆 + GENERIC_READ；
    //    · scratch（带 RayTracingScratch，由调用方显式标注）：GPU 只写 ⇒
    //      DEFAULT 堆 + ALLOW_UNORDERED_ACCESS（构建前转到 UNORDERED_ACCESS）；
    //    · TLAS 实例缓冲（Storage | AccelerationStruct，不带上面两位）：CPU 每帧 Map
    //      写入、GPU 只读 ⇒ 同样放 UPLOAD 堆（GPU 地址 64KB 对齐，满足 16 字节要求）。
    // ------------------------------------------------------------------
    const bool isStorage    = HasAny(desc.usage, BufferUsage::Storage);
    const bool isSbt        = HasAny(desc.usage, BufferUsage::ShaderBindingTable);
    const bool isRtScratch  = HasAny(desc.usage, BufferUsage::RayTracingScratch);
    const bool isRtInstance = HasAny(desc.usage, BufferUsage::AccelerationStruct) && !isSbt &&
                              !isRtScratch && HasAny(desc.usage, BufferUsage::Storage);
    // 只有可被着色器写的缓冲才需要 UAV；SBT 与实例缓冲只被 GPU 读
    const bool allowUav     = isStorage && !isSbt && !isRtInstance;

    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    if (isSbt || isRtInstance) {
        // CPU 写、GPU 只读：UPLOAD 堆可以直接映射（也避开 UPLOAD 堆不能带 UAV 标志的限制）
        heapType = D3D12_HEAP_TYPE_UPLOAD;
    } else if (!isStorage && (HasAny(desc.usage, BufferUsage::TransferDst) || desc.cpuAccess ||
        desc.initialData != nullptr || HasAny(desc.usage, BufferUsage::Uniform))) {
        // 【关键】带 initialData 的非 UAV 缓冲（顶点/索引/常量）直接用 UPLOAD 堆：
        //   · UPLOAD 堆可以立即 Map 写入，数据当场就有效
        //   · 之前走 DEFAULT 堆 + 中转缓冲，但**从未执行拷贝** ⇒ 顶点数据全 0
        //     ⇒ 三角形退化 ⇒ 画面全黑（实测就是这个问题）
        // 常量缓冲（Uniform）一律放 UPLOAD 堆：引擎把它们当 CPU 每帧写入的数据，即使没置
        // cpuAccess 也会直接 Map（Vulkan 下它们本就在主机可见内存）；DEFAULT 堆不可映射，
        // Map 会直接失败（调试层 id=822），调用方随后写空指针崩溃（实测 04.Sponza-Deferred）。
        heapType = D3D12_HEAP_TYPE_UPLOAD;
    }
    // UAV 缓冲需要一份等大的 UPLOAD 暂存来承载 CPU 写入
    // UAV 缓冲一律配一份 UPLOAD 暂存：D3D12 的 DEFAULT 堆资源**不能** Map，
    // 而引擎对所有 SSBO 都会调用 Map()/Unmap() 写数据（不管有没有置 cpuAccess）
    // ⇒ 没有暂存就会在 Map 时失败（实测 02.Cube 初始化最后一步卡在这里）。
    // 【Storage 缓冲一律 CPU 可读写】Vulkan 后端给所有缓冲分配主机可见、持久映射的内存
    //（VMA HOST_ACCESS + MAPPED），引擎因此对任何 Storage 缓冲都直接 Map 读写，GPU 与 CPU 访问
    // 同一块内存、不发生拷贝。此前 D3D12 用「DEFAULT 堆 + UPLOAD 暂存」：Map 拿到的是暂存区，
    // 读不到 GPU 结果；Unmap 还会把暂存区拷回 GPU 缓冲，覆盖 GPU 刚写入的数据（实测 06.GILab
    // 自动曝光卡在初始值；04 的粒子系统每帧调试回读后整个粒子池被清零，粒子永不死亡）。
    // 改为放在 CPU 可访问的自定义堆上（属性按 READBACK 堆取，自动适配集显/独显），直接映射。
    // 【scratch 例外】光追 scratch 只由 GPU 写、CPU 从不访问，必须留在 DEFAULT 堆（DXR 要求），
    // 既不能映射也不需要暂存。
    const bool cpuVisibleUav = allowUav && !isRtScratch;
    const bool needStaging   = allowUav && !cpuVisibleUav && !isRtScratch;

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

    // 允许 UAV 的缓冲需要显式声明（SBT / 实例缓冲只被 GPU 读，不带该标志）
    if (allowUav) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heapType;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;
    if (cpuVisibleUav) {
        hp = dev->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_READBACK);   // 返回 CUSTOM 类型 + 适配器对应的页属性/内存池
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask  = 1;
    }

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
    // 登记 GPU 地址范围：DGC 等只传地址的接口据此反查资源与偏移
    if (m_Device) m_Device->RegisterBufferAddress(m_GpuAddress, m_Size, m_Resource.Get());

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
        if (heapType == D3D12_HEAP_TYPE_UPLOAD || cpuVisibleUav) {   // CPU 可映射的堆：直接写
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

void* D3D12Buffer::Map() {

    if (!m_Resource) return nullptr;
    if (m_MappedPtr) return m_MappedPtr;

    D3D12_RANGE range{ 0, 0 };   // 声明 CPU 不读，驱动可以避免不必要的同步
    void* p = nullptr;

    // UAV 缓冲不能直接 Map，用暂存缓冲
    ID3D12Resource* target = m_NeedsStagingCopy ? m_CpuStaging.Get() : m_Resource.Get();
    if (!target) return nullptr;
    const HRESULT mhr = target->Map(0, &range, &p);
    if (FAILED(mhr)) {
        // 带上 HRESULT 与缓冲信息：映射 UPLOAD/暂存缓冲失败通常意味着设备已被移除
        HE_CORE_ERROR("D3D12Buffer::Map 失败 hr=0x{:08x} size={} staging={}",
                      static_cast<u32>(mhr), static_cast<u64>(m_Size), m_NeedsStagingCopy ? 1 : 0);
        if (m_Device && m_Device->GetDevice()) CheckDeviceAlive(m_Device->GetDevice(), "D3D12Buffer::Map");
        return nullptr;
    }
    m_MappedPtr = p;
    m_PersistentlyMapped = true;
    return p;
}

void D3D12Buffer::Unmap() {
    // 记录最近一次被 CPU 写入的物体缓冲（多飞行帧错位修正用）
    // 只有「确实写入了有效矩阵」的缓冲才记为当前物体缓冲：
    // 三缓冲里有些槽本帧没写（全 0），绑到它们会让世界矩阵为 0。
    if (m_Size == 212992u && m_Device && m_MappedPtr) {
        const float* fchk = static_cast<const float*>(m_MappedPtr);
        if (fchk[15] != 0.0f || fchk[52 * 1 + 15] != 0.0f) {
            m_Device->SetLastObjectBuffer(this);
        }
    }
    // 保存前 32 个物体矩阵的缩放/平移副本，供绘制时核对 objIdx 是否指到了正确的物体
    if (m_Size == 212992u && m_Device && m_MappedPtr) {
        const float* fm = static_cast<const float*>(m_MappedPtr);
        for (unsigned i = 0; i < 32u; ++i) {
            const float* e = fm + i * 52u;   // 208 字节 = 52 个 float
            m_Device->m_LastObjM[i][0] = e[0];
            m_Device->m_LastObjM[i][1] = e[5];
            m_Device->m_LastObjM[i][2] = e[10];
            m_Device->m_LastObjM[i][3] = e[12];
            m_Device->m_LastObjM[i][4] = e[13];
            m_Device->m_LastObjM[i][5] = e[14];
        }
    }

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
    // 【必须无条件上传】此前被 HE_D3D12_STAGING_COPY 环境变量门控，默认不执行
    if (m_NeedsStagingCopy && m_CpuStaging && m_Device && m_Size > 0) {
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
            // 【必须等上一次拷贝执行完再 Reset 分配器】否则 D3D12 会在 GPU 还在用这块
        // 分配器时被重置，拷贝被丢弃（实测物体缓冲一直是 0 ⇒ 世界矩阵为 0 ⇒ 全黑）。
        if (s_ready && s_fence && s_value > 0) {
            if (s_fence->GetCompletedValue() < s_value) {
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (ev) {
                    s_fence->SetEventOnCompletion(s_value, ev);
                    WaitForSingleObject(ev, 5000);
                    CloseHandle(ev);
                }
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
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
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

void D3D12Texture::UploadInitialData(D3D12Device* device, const TextureDesc& desc,
                                     D3D12_RESOURCE_STATES curState) {
    ID3D12Device* dev = device->GetDevice();
    ID3D12CommandQueue* q = device->GetQueue();
    if (!dev || !q || !m_Resource) return;
    const D3D12_RESOURCE_DESC rd = m_Resource->GetDesc();
    // 3D 纹理的子资源只按 mip 划分：DepthOrArraySize 是深度切片数而非数组层，整个体积作为一个子资源上传
    //（拷贝足迹的 Depth 即切片数，下面逐行拷贝时 numRows × Depth 行连续覆盖所有切片）
    const bool is3D = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const u32 layers = is3D ? 1u : static_cast<u32>(rd.DepthOrArraySize);

    // 【mip 链】Vulkan 后端上传 mip 0 后用 blit 逐级生成其余 mip；D3D12 没有 blit，
    // 这里在 CPU 上用 2×2 盒式滤波生成。只传 mip 0 时其余级是未初始化显存，远处/斜视表面
    // 采样到低级 mip 会发黑（实测 03.Sponza 的墙体、柱子大片发黑）。
    // 支持 8 位四通道格式；sRGB 格式先转线性空间再平均，与 Vulkan 的线性 blit 一致。
    const bool isSrgb = rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool is8x4  = isSrgb || rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    const u32 mips = (m_MipLevels > 1 && is8x4 && !is3D) ? m_MipLevels : 1u;   // 3D 纹理不在 CPU 生成 mip
    if (m_MipLevels > 1 && (!is8x4 || is3D)) {
        HE_CORE_WARN("D3D12Texture: 格式 {} 不支持 CPU 生成 mip，只上传 mip 0（{}x{}）",
                     static_cast<u32>(rd.Format), m_Width, m_Height);
    }

    // 各子资源的源数据：mip 0 直接取 initialData（各层紧密排列），其余级在 CPU 上生成
    const u8* src0 = static_cast<const u8*>(desc.initialData);
    std::vector<std::vector<u8>> generated(static_cast<size_t>(layers) * mips);   // [layer*mips + mip]，mip>=1 有效
    if (mips > 1) {
        float toLinear[256];
        for (int i = 0; i < 256; ++i) {
            const float c = i / 255.0f;
            toLinear[i] = isSrgb ? (c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f)) : c;
        }
        auto encode = [isSrgb](float c) -> u8 {
            c = std::min(std::max(c, 0.0f), 1.0f);
            if (isSrgb) c = (c <= 0.0031308f) ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            return static_cast<u8>(c * 255.0f + 0.5f);
        };
        const size_t layerBytes0 = static_cast<size_t>(m_Width) * m_Height * 4;
        for (u32 layer = 0; layer < layers; ++layer) {
            const u8* prev = src0 + layerBytes0 * layer;
            u32 pw = m_Width, ph = m_Height;
            for (u32 mip = 1; mip < mips; ++mip) {
                const u32 w = std::max(1u, pw / 2), h = std::max(1u, ph / 2);
                std::vector<u8>& out = generated[static_cast<size_t>(layer) * mips + mip];
                out.resize(static_cast<size_t>(w) * h * 4);
                for (u32 y = 0; y < h; ++y) {
                    const u32 y0 = std::min(y * 2, ph - 1), y1 = std::min(y * 2 + 1, ph - 1);
                    for (u32 x = 0; x < w; ++x) {
                        const u32 x0 = std::min(x * 2, pw - 1), x1 = std::min(x * 2 + 1, pw - 1);
                        const u8* t[4] = { prev + (static_cast<size_t>(y0) * pw + x0) * 4, prev + (static_cast<size_t>(y0) * pw + x1) * 4,
                                           prev + (static_cast<size_t>(y1) * pw + x0) * 4, prev + (static_cast<size_t>(y1) * pw + x1) * 4 };
                        u8* o = &out[(static_cast<size_t>(y) * w + x) * 4];
                        for (int c = 0; c < 3; ++c)   // 颜色通道（sRGB 时在线性空间平均）
                            o[c] = encode(0.25f * (toLinear[t[0][c]] + toLinear[t[1][c]] + toLinear[t[2][c]] + toLinear[t[3][c]]));
                        o[3] = static_cast<u8>((t[0][3] + t[1][3] + t[2][3] + t[3][3] + 2) / 4);   // alpha 线性
                    }
                }
                prev = out.data();
                pw = w; ph = h;
            }
        }
    }

    // 子资源号 = layer * m_MipLevels + mip
    const u32 count = layers * mips;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(count);
    std::vector<UINT> numRows(count);
    std::vector<UINT64> rowBytes(count);
    UINT64 total = 0;
    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < mips; ++mip) {
            const u32 i = layer * mips + mip;
            UINT64 sz = 0;
            dev->GetCopyableFootprints(&rd, layer * m_MipLevels + mip, 1, 0, &fps[i], &numRows[i], &rowBytes[i], &sz);
            fps[i].Offset = total;
            total = (total + sz + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        }
    }

    D3D12_HEAP_PROPERTIES uh{};
    uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    uh.CreationNodeMask = 1;
    uh.VisibleNodeMask  = 1;
    D3D12_RESOURCE_DESC ud{};
    ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ud.Width = total;
    ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.SampleDesc = { 1, 0 };
    ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    if (FAILED(dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&staging)))) {
        HE_CORE_ERROR("D3D12Texture: 创建上传缓冲失败（{} 字节）", static_cast<u64>(total));
        return;
    }

    // 源数据行紧密排列；目标行距按 D3D12 的 256 字节对齐，逐行拷贝
    u8* dst = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(staging->Map(0, &noRead, reinterpret_cast<void**>(&dst))) || !dst) return;
    const u8* layerSrc = src0;   // mip 0 按层顺序连续读取
    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < mips; ++mip) {
            const u32 i = layer * mips + mip;
            const u8* src = (mip == 0) ? layerSrc : generated[i].data();
            const UINT rows = numRows[i] * fps[i].Footprint.Depth;
            for (UINT r = 0; r < rows; ++r) {
                std::memcpy(dst + fps[i].Offset + static_cast<UINT64>(r) * fps[i].Footprint.RowPitch,
                            src + static_cast<size_t>(r) * rowBytes[i], static_cast<size_t>(rowBytes[i]));
            }
            if (mip == 0) layerSrc += static_cast<size_t>(rows) * rowBytes[i];
        }
    }
    staging->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator>    a;
    ComPtr<ID3D12GraphicsCommandList> l;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l)))) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = m_Resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (curState != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.StateBefore = curState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        l->ResourceBarrier(1, &b);
    }
    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < mips; ++mip) {
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource = m_Resource.Get();
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d.SubresourceIndex = layer * m_MipLevels + mip;
            D3D12_TEXTURE_COPY_LOCATION s{};
            s.pResource = staging.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint = fps[layer * mips + mip];
            l->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        }
    }
    // 与 Vulkan 的 UploadInitialData 一致：上传后停在着色器只读状态
    const D3D12_RESOURCE_STATES readState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = readState;
    l->ResourceBarrier(1, &b);
    l->Close();
    ID3D12CommandList* lists[1] = { l.Get() };
    q->ExecuteCommandLists(1, lists);

    // 同步等待：上传缓冲与分配器都是局部对象，必须等 GPU 用完再析构
    ComPtr<ID3D12Fence> f;
    if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) {
        if (HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS)) {
            q->Signal(f.Get(), 1);
            f->SetEventOnCompletion(1, ev);
            WaitForSingleObject(ev, 5000);
            CloseHandle(ev);
        }
    }
    device->SetResourceState(m_Resource.Get(), readState);
}

// ===========================================================================
//  QueryPool
// ===========================================================================
bool D3D12QueryPool::Initialize(D3D12Device* device, u32 queryCount, QueryType type) {
    if (!device || !device->GetDevice() || queryCount == 0) return false;
    ID3D12Device* dev = device->GetDevice();
    m_Count  = queryCount;
    m_Type   = type;
    m_Stride = (type == QueryType::PipelineStatistics)
             ? static_cast<u32>(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)) : 8u;

    D3D12_QUERY_HEAP_DESC hd{};
    hd.Type  = (type == QueryType::PipelineStatistics) ? D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS
                                                       : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    hd.Count = queryCount;
    if (!HE_D3D12_CHECK(dev->CreateQueryHeap(&hd, IID_PPV_ARGS(&m_Heap)), "CreateQueryHeap")) return false;

    // 回读缓冲：READBACK 堆资源必须一直处于 COPY_DEST（正好是 ResolveQueryData 的目标状态）
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type             = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = static_cast<UINT64>(queryCount) * m_Stride;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc       = { 1, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!HE_D3D12_CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_Readback)),
            "CreateCommittedResource(QueryReadback)")) {
        return false;
    }
    // 持久映射：只在围栏确认 GPU 写完之后才读取
    void* p = nullptr;
    if (FAILED(m_Readback->Map(0, nullptr, &p)) || !p) return false;
    m_Mapped = static_cast<const u8*>(p);
    m_Avail.resize(queryCount);
    return true;
}

void D3D12QueryPool::MarkSubmitted(u32 index, ID3D12Fence* fence, u64 value) {
    if (index >= m_Count) return;
    m_Avail[index].fence = fence;
    m_Avail[index].value = value;
}

void D3D12QueryPool::ResetAll() {
    for (auto& a : m_Avail) { a.fence.Reset(); a.value = 0; }
}

bool D3D12QueryPool::IsAvailable(u32 index, bool wait, u32 timeoutMs) const {
    if (index >= m_Count || !m_Avail[index].fence) return false;   // 尚未写入并提交
    ID3D12Fence* f = m_Avail[index].fence.Get();
    const u64    v = m_Avail[index].value;
    if (f->GetCompletedValue() >= v) return true;
    if (!wait) return false;
    HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!ev) return false;
    if (SUCCEEDED(f->SetEventOnCompletion(v, ev))) WaitForSingleObject(ev, timeoutMs);
    CloseHandle(ev);
    return f->GetCompletedValue() >= v;
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
    // 【等 GPU 空闲再释放】引擎在运行期高频创建/销毁渲染目标（实测 12 秒 40 张
    // 1920x1080 的 HDR 目标）。若直接释放，GPU 可能仍在读写它，且它的 RTV 描述符堆
    // 也被释放 —— 后续帧用同一个地址重建的句柄会指向已释放的堆，表现就是
    // 「清屏与绘制都合法、但都不生效、画面恒黑」。这里先等队列空闲再释放。
    if (m_OwnerDevice) m_OwnerDevice->WaitIdle();
    // 注销视图句柄映射，避免地址被复用时查到错误的资源
    if (m_OwnerDevice) {
        if (m_Rtv.ptr) m_OwnerDevice->UnregisterViewResource(static_cast<UINT64>(m_Rtv.ptr));
        if (m_Dsv.ptr) m_OwnerDevice->UnregisterViewResource(static_cast<UINT64>(m_Dsv.ptr));
        if (m_Dsv.ptr) m_OwnerDevice->UnregisterReadOnlyDsv(static_cast<UINT64>(m_Dsv.ptr));
        for (auto& s : m_Srvs) if (s.ptr) m_OwnerDevice->UnregisterViewResource(static_cast<UINT64>(s.ptr));
        for (auto& v : m_FaceViews) if (v.ptr) m_OwnerDevice->UnregisterViewResource(static_cast<UINT64>(v.ptr));
        if (m_ViewKey.ptr) m_OwnerDevice->UnregisterViewResource(static_cast<UINT64>(m_ViewKey.ptr));
    }
}

bool D3D12Texture::Initialize(D3D12Device* device, const TextureDesc& desc, bool transient,
                              u32 firstPass, u32 lastPass) {
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

    // 深度 > 1 的非立方体纹理是真正的 3D 纹理（Lumen SDF 等，着色器里是 Texture3D / RWTexture3D）。
    // 此前两个分支都建成 2D（深度被当成数组层），3D 视图与着色器声明对不上
    const bool is3D = desc.depth > 1 && !HasAny(desc.usage, TextureUsage::Cubemap);
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = is3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Alignment        = 0;
    rd.Width            = desc.width;
    rd.Height           = desc.height;
    rd.DepthOrArraySize = static_cast<UINT16>(is3D ? desc.depth : desc.arrayLayers * desc.depth);
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
    HRESULT crh = E_FAIL;
    if (transient) {
        // 瞬态纹理：放在本帧瞬态堆上，生命周期不重叠的纹理共用内存（帧槽位轮换回来时整堆重置）。
        // 所在内存可能刚被别的资源用过：登记「待激活」，命令列表首次使用时发别名屏障；渲染目标 /
        // 深度还要 DiscardResource 初始化（D3D12 要求复用内存上的这两类资源先清除 / 丢弃再使用）
        ID3D12Heap* heap = nullptr;
        UINT64 offset = 0;
        if (!device->AllocateTransient(rd, heap, offset, firstPass, lastPass)) return false;   // 堆不够：调用方回退普通纹理
        crh = dev->CreatePlacedResource(heap, offset, &rd, initState, hasClear ? &clearValue : nullptr,
                                        IID_PPV_ARGS(&m_Resource));
        if (SUCCEEDED(crh)) {
            device->TrackTransient(m_Resource);   // 保活到堆重置（GPU 用完之后）
            const D3D12_RESOURCE_STATES discardState =
                m_IsDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                : (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? D3D12_RESOURCE_STATE_RENDER_TARGET
                : D3D12_RESOURCE_STATE_COMMON;
            device->RegisterAliasActivation(m_Resource.Get(), discardState);
        }
    } else {
        crh = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initState,
                                           hasClear ? &clearValue : nullptr,
                                           IID_PPV_ARGS(&m_Resource));
    }
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
            if (is3D) {   // 3D 渲染目标：整个 mip 0 的全部深度切片
                vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                vd.Texture3D.MipSlice = 0;
                vd.Texture3D.FirstWSlice = 0;
                vd.Texture3D.WSize = static_cast<UINT>(-1);
            } else {
                vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipSlice = 0;
                vd.Texture2D.PlaneSlice = 0;
            }
            dev->CreateRenderTargetView(m_Resource.Get(), &vd, m_Rtv);
    device->RegisterViewResource(static_cast<UINT64>(m_Rtv.ptr), m_Resource.Get());

        }
    }

    // ---- 创建 DSV ----
    if (m_IsDepth) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 2;   // [0] 可写 DSV，[1] 只读 DSV
        if (SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_DsvHeap)))) {
            m_Dsv = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_DEPTH_STENCIL_VIEW_DESC vd{};
            vd.Format = fmt;
            vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = 0;
            dev->CreateDepthStencilView(m_Resource.Get(), &vd, m_Dsv);
    device->RegisterViewResource(static_cast<UINT64>(m_Dsv.ptr), m_Resource.Get());
            // 只读 DSV：深度测试但不写深度的 pass（如软粒子）同时把这张深度图当 SRV 采样时使用，
            // 对应 Vulkan 的 DEPTH_STENCIL_READ_ONLY 布局
            m_DsvReadOnly = m_Dsv;
            m_DsvReadOnly.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            vd.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
            if (fmt == DXGI_FORMAT_D24_UNORM_S8_UINT || fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
                vd.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
            dev->CreateDepthStencilView(m_Resource.Get(), &vd, m_DsvReadOnly);
            device->RegisterReadOnlyDsv(static_cast<UINT64>(m_Dsv.ptr), static_cast<UINT64>(m_DsvReadOnly.ptr));
        }
    }

    // ---- 逐层视图（立方体图 / 纹理数组）----
    // 引擎用 GetNativeHandle(face) 取「某一层」的附件视图，逐面渲染立方体图
    //（IBL 辐照度烘焙、点光源阴影）。此前 D3D12 从未创建这些视图、一律返回空，
    // 这些 pass 因此整体被跳过 ⇒ IBL 贴图与点光源阴影都是空的。
    const u32 layers = desc.arrayLayers * (desc.depth > 1 ? desc.depth : 1);
    const bool isColorRt = !m_IsDepth && HasAny(desc.usage, TextureUsage::RenderTarget);
    if (!is3D && layers > 1 && (isColorRt || m_IsDepth)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = m_IsDepth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = layers;
        if (SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_FaceViewHeap)))) {
            const UINT inc = dev->GetDescriptorHandleIncrementSize(hd.Type);
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_FaceViewHeap->GetCPUDescriptorHandleForHeapStart();
            m_FaceViews.resize(layers);
            for (u32 i = 0; i < layers; ++i) {
                if (m_IsDepth) {
                    D3D12_DEPTH_STENCIL_VIEW_DESC vd{};
                    vd.Format = fmt;
                    vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                    vd.Texture2DArray.MipSlice = 0;
                    vd.Texture2DArray.FirstArraySlice = i;
                    vd.Texture2DArray.ArraySize = 1;
                    dev->CreateDepthStencilView(m_Resource.Get(), &vd, h);
                } else {
                    D3D12_RENDER_TARGET_VIEW_DESC vd{};
                    vd.Format = fmt;
                    vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    vd.Texture2DArray.MipSlice = 0;
                    vd.Texture2DArray.FirstArraySlice = i;
                    vd.Texture2DArray.ArraySize = 1;
                    dev->CreateRenderTargetView(m_Resource.Get(), &vd, h);
                }
                m_FaceViews[i] = h;
                device->RegisterViewResource(static_cast<UINT64>(h.ptr), m_Resource.Get());
                h.ptr += inc;
            }
        }
    }

    // 【登记真实初始状态】命令列表做 RENDER_TARGET / DEPTH_WRITE 屏障时要靠它。
    // 不登记就会默认成 COMMON，从而跳过屏障，导致 OMSetRenderTargets 收到
    // 一个状态不对的资源而让驱动静默卡死。
    device->SetResourceState(m_Resource.Get(), initState);

    // 【无 RTV/DSV 的纹理也要有视图句柄】引擎用 GetNativeHandle() 当整图 ImageView 交给
    // UpdateDescriptorSetWithImageView（Lumen 图集 / 辐照度等只做 UAV 的 compute 目标）。
    // 此前这类纹理返回空，描述符从未写入。这里放一个 CPU 侧的 SRV 作句柄键并登记到视图表。
    if (!m_Rtv.ptr && !m_Dsv.ptr) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_ViewKeyHeap)))) {
            m_ViewKey = m_ViewKeyHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            BuildTextureViewDescs(m_Resource->GetDesc(), 0, 0, false, sd, ud);
            dev->CreateShaderResourceView(m_Resource.Get(), &sd, m_ViewKey);
            device->RegisterViewResource(static_cast<UINT64>(m_ViewKey.ptr), m_Resource.Get());
        }
    }

    // 【初始数据】此前 D3D12 纹理完全忽略 initialData：占位白图、天空盒立方体图等
    // 内容都是未初始化显存，且一直停在 COPY_DEST（GPU 验证层 id=942）⇒ 采样结果与 Vulkan 不一致。
    // 3D 纹理的子资源按 mip 划分（深度切片不是独立子资源），上传时整个体积作为 mip 0 一次拷贝
    if (desc.initialData) {
        UploadInitialData(device, desc, initState);
    }

    // 大纹理创建的诊断：引擎运行期会频繁创建全屏目标，用 TRACE 级别避免刷日志（此前误用 ERROR）
    if (m_Width >= 1920) HE_CORE_TRACE("[TEX-BIG] res={} {}x{} dxgi={} depth={} rtvPtr={}",
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
    sd.Filter   = ToD3D12Filter(desc.minFilter, desc.magFilter, desc.mipFilter,
                                desc.maxAnisotropy > 1, desc.enableCompare);
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

// PSO 描述哈希（FNV-1a）：键 PSO 缓存块。与 Vulkan 的 HashPipelineStateDesc 同口径，但着色器取 DXIL，
// 并纳入 D3D12 PSO 实际用到的光栅 / 混合状态（同一着色器不同混合状态是不同的 PSO）
static u64 HashPipelineStateDescDxil(const PipelineStateDesc& desc) {
    u64 h = 0xcbf29ce484222325ULL;
    auto bytes = [&h](const void* data, size_t size) {
        const u8* b = static_cast<const u8*>(data);
        for (size_t i = 0; i < size; ++i) { h ^= b[i]; h *= 0x100000001b3ULL; }
    };
    auto u = [&bytes](u32 v) { bytes(&v, sizeof(v)); };
    u(static_cast<u32>(desc.bindPoint));
    for (const ShaderBytecode* bc : { desc.vertexShader, desc.pixelShader, desc.computeShader,
                                      desc.meshShader, desc.amplificationShader }) {
        u(bc ? static_cast<u32>(bc->dxil.size()) : 0u);
        if (bc && !bc->dxil.empty()) bytes(bc->dxil.data(), bc->dxil.size());
    }
    u(desc.vertexLayout.stride);
    for (const auto& a : desc.vertexLayout.attributes) { u(a.location); u(a.binding); u(static_cast<u32>(a.format)); u(a.offset); }
    u(static_cast<u32>(desc.topology)); u(static_cast<u32>(desc.cullMode)); u(static_cast<u32>(desc.frontFace));
    u(static_cast<u32>(desc.fillMode)); u(desc.depthTest ? 1u : 0u); u(desc.depthWrite ? 1u : 0u);
    u(static_cast<u32>(desc.depthCompare)); u(desc.colorAttachmentCount);
    for (u32 i = 0; i < desc.colorAttachmentCount && i < kMaxColorAttachments; ++i) {
        u(static_cast<u32>(desc.colorFormats[i]));
        const ColorBlendDesc& b = desc.colorBlend[i];
        u(b.blendEnable ? 1u : 0u); u(static_cast<u32>(b.srcColorBlendFactor)); u(static_cast<u32>(b.dstColorBlendFactor));
        u(static_cast<u32>(b.colorBlendOp)); u(static_cast<u32>(b.srcAlphaBlendFactor));
        u(static_cast<u32>(b.dstAlphaBlendFactor)); u(static_cast<u32>(b.alphaBlendOp)); u(static_cast<u32>(b.writeMask));
    }
    u(static_cast<u32>(desc.depthFormat)); u(desc.sampleCount);
    return h;
}

bool D3D12PipelineState::Initialize(D3D12Device* device, const PipelineStateDesc& desc) {
    // PSO 预编译的缓存块：描述相同则作为 CachedPSO 传入（驱动直接取编译结果）
    const u64 psoHash = HashPipelineStateDescDxil(desc);
    std::vector<u8> cachedBlob;
    const bool haveBlob = device && device->FindPsoBlob(psoHash, cachedBlob);
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
    //  所有资源都落在 space<描述符集号>，寄存器按下表的惯例分配：
    //      t0..t63  : SRV（StructuredBuffer / Texture2D 数组）
    //      s0..s15  : Sampler
    //      b0..b7   : root CBV（push constant）
    //      b8..     : 描述符集里的常量缓冲（CBV，与 SRV 共用同一段表）
    //  根签名只要少声明一类，PSO 就会被 D3D12 拒绝（E_INVALIDARG）。
    //
    //  【与光追共用】这段布局已抽到 D3D12RayTracing.cpp 的 CreateEngineRootSignature：
    //  图形 / 计算 PSO 与 RT 状态对象的全局根签名必须**是同一份**，否则同一套
    //  ShaderToDxil 产物在两条路径上会有一边绑不上（详见实施手册 T5.6 第 5 条）。
    // ------------------------------------------------------------------
    if (!CreateEngineRootSignature(dev, &m_RootSig)) {
        HE_CORE_ERROR("D3D12PipelineState: 根签名创建失败（name={}）", desc.debugName.c_str());
        return false;
    }

    m_ColorLoadOp = desc.colorLoadOp;
    m_DepthLoadOp = desc.depthLoadOp;
    m_DepthWrite  = desc.depthWrite;

    // ---- 登记结构化缓冲步长 ----
    // 从各阶段 DXIL 反射出结构化缓冲的真实步长，按「描述符集布局 + binding」登记到设备，
    // 写描述符时据此设置 StructureByteStride（此前按寄存器号在全局表里猜，不同 pass 同号
    // 寄存器的结构体不同，延迟渲染等路径大量错位，GPU 验证 id=1387）。
    {
        ComPtr<IDxcUtils> utils;
        if (SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) && utils) {
            for (const ShaderBytecode* bc : { desc.vertexShader, desc.pixelShader, desc.computeShader,
                                              desc.meshShader, desc.amplificationShader }) {
                if (!bc || bc->dxil.empty()) continue;
                DxcBuffer db{};
                db.Ptr = bc->dxil.data();
                db.Size = bc->dxil.size();
                db.Encoding = DXC_CP_ACP;
                ComPtr<ID3D12ShaderReflection> r;
                if (FAILED(utils->CreateReflection(&db, IID_PPV_ARGS(&r))) || !r) continue;
                device->NoteSetLayoutStrides(r.Get(), desc.descriptorSetLayouts);
                // 记下着色器读写的描述符集资源（只看引擎描述符集对应的 space；bindless 的 space2 是静态资源）
                D3D12_SHADER_DESC sd{};
                if (FAILED(r->GetDesc(&sd))) continue;
                for (UINT i = 0; i < sd.BoundResources; ++i) {
                    D3D12_SHADER_INPUT_BIND_DESC bd{};
                    if (FAILED(r->GetResourceBindingDesc(i, &bd)) || bd.Space >= desc.descriptorSetLayouts.size()) continue;
                    bool uav = false;
                    switch (bd.Type) {
                        case D3D_SIT_TEXTURE: case D3D_SIT_STRUCTURED: case D3D_SIT_BYTEADDRESS: uav = false; break;
                        case D3D_SIT_UAV_RWTYPED: case D3D_SIT_UAV_RWSTRUCTURED: case D3D_SIT_UAV_RWBYTEADDRESS:
                        case D3D_SIT_UAV_APPEND_STRUCTURED: case D3D_SIT_UAV_CONSUME_STRUCTURED:
                        case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER: uav = true; break;
                        default: continue;   // CBV / 采样器不涉及资源状态
                    }
                    m_ResourceUses.push_back({ bd.Space, bd.BindPoint, uav });
                }
            }
        }
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
        if (haveBlob) cd.CachedPSO = { cachedBlob.data(), cachedBlob.size() };
        cd.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        HRESULT chr = dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_Pso));
        if (FAILED(chr) && haveBlob) {   // 缓存块与驱动 / 设备不匹配：不用缓存重建
            cd.CachedPSO = {};
            chr = dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_Pso));
        }
        if (SUCCEEDED(chr) && !haveBlob) device->StorePsoBlob(psoHash, m_Pso.Get());
        if (FAILED(chr)) {
            HE_CORE_ERROR("D3D12PipelineState: 计算 PSO 创建失败 name={} hr=0x{:08x} CS={}B",
                          desc.debugName.c_str(), static_cast<u32>(chr),
                          static_cast<u32>(cd.CS.BytecodeLength));
            return false;
        }
        m_IsCompute = true;
        if (!desc.debugName.empty()) m_Pso->SetName(Utf8ToWide(desc.debugName.c_str()).c_str());   // RenderDoc / DRED 里显示管线名
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
    // 网格管线（meshShader 非空）没有顶点着色器，PSO 走下面的管线状态流分支
    const bool isMesh = desc.meshShader && !desc.meshShader->dxil.empty();
    if (desc.vertexShader && !desc.vertexShader->dxil.empty()) {
        pd.VS = { desc.vertexShader->dxil.data(), desc.vertexShader->dxil.size() };
    } else if (!isMesh) {
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
    if (std::getenv("HE_NO_DEPTH")) pd.DepthStencilState.DepthEnable = FALSE;
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
            static std::mutex s_semanticPoolMutex;   // PSO 可能在预编译线程上并行创建
            std::lock_guard<std::mutex> poolLock(s_semanticPoolMutex);
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
                // 【T6】旧绕行遗留的 HE_INSTANCEINDEX 不再产生（顶点着色器已按 SM 6.8 编译，
                // 实例号 = SV_InstanceID + SV_StartInstanceLocation）。这里仍保险地跳过这类
                // 非顶点属性语义，避免旧 DXIL 被反射成输入元素让 PSO 创建失败。
                if (p.SemanticName && std::strcmp(p.SemanticName, "HE_INSTANCEINDEX") == 0) {
                    continue;
                }

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

    // Prefer the engine-provided vertex layout: it is the authoritative
    // description of the vertex buffer (StaticVertex = pos/normal/uv).
    // Synthesizing a larger layout from the shader signature makes the IA
    // read past the real stride (e.g. joints/weights at 40/44 of a 48-byte
    // vertex) which yields garbage transform data.
    // 着色器的真实输入签名（上面从 DXIL 反射得到），下面用来补齐引擎布局缺失的属性
    const std::vector<D3D12_INPUT_ELEMENT_DESC> shaderInputs = elems;
    // 引擎布局的语义名：输入布局在本函数末尾创建 PSO 时才被读取，必须活到函数结束。用函数局部数组
    // （预编译线程并行创建 PSO 时静态数组会互相覆盖），按属性数预留，保证 c_str() 指针不因扩容失效
    std::vector<std::string> s_names;
    if (!desc.vertexLayout.attributes.empty()) {
        elems.clear();
        s_names.reserve(desc.vertexLayout.attributes.size());
        static const char* kSemByLoc[8] = { "POSITION", "NORMAL", "TEXCOORD",
            "BLENDINDICES", "BLENDWEIGHT", "TANGENT", "COLOR", "TEXCOORD" };
        for (const auto& a : desc.vertexLayout.attributes) {
            D3D12_INPUT_ELEMENT_DESC e{};
            const u32 loc = a.location;
            s_names.push_back(kSemByLoc[loc < 8u ? loc : 2u]);
            e.SemanticName = s_names.back().c_str();
            e.SemanticIndex = 0;
            e.Format = ToDxgiVertexFormat(a.format);
            e.InputSlot = 0;
            e.AlignedByteOffset = static_cast<UINT>(a.offset);
            e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            e.InstanceDataStepRate = 0;
            elems.push_back(e);
        }
        if (desc.vertexLayout.stride > 0) m_VertexStride = static_cast<u32>(desc.vertexLayout.stride);

        // 【补齐缺失属性】着色器声明了、引擎布局没提供的输入（如静态 PSO 共用 PBR.vert 时的
        // 关节索引/权重）：D3D12 要求输入签名的每个元素都在布局里，否则 PSO 创建失败。
        // 把它们挂到零缓冲槽（步长 0 的全零缓冲），读到的值恒为 0，与 Vulkan 未提供属性时一致。
        for (const auto& si : shaderInputs) {
            bool found = false;
            for (const auto& e : elems) {
                if (e.SemanticIndex == si.SemanticIndex && std::strcmp(e.SemanticName, si.SemanticName) == 0) {
                    found = true;
                    break;
                }
            }
            if (found) continue;
            D3D12_INPUT_ELEMENT_DESC e = si;
            e.InputSlot = kZeroVertexSlot;
            e.AlignedByteOffset = 0;
            elems.push_back(e);
            m_NeedsZeroVB = true;
        }
    }


    if (std::getenv("HE_LAYOUT_DBG")) {
        std::printf("[LAYOUT] pso=%s engineAttrs=%zu engineStride=%u elems=%zu stride=%u\n",
            desc.debugName.c_str(), desc.vertexLayout.attributes.size(),
            static_cast<u32>(desc.vertexLayout.stride), elems.size(), m_VertexStride);
        for (size_t k = 0; k < elems.size() && k < 6; ++k)
            std::printf("[LAYOUT]   e%zu sem=%s idx=%u fmt=%d off=%u\n", k, elems[k].SemanticName,
                elems[k].SemanticIndex, (int)elems[k].Format, elems[k].AlignedByteOffset);
        std::fflush(stdout); }
    // Validate the vertex layout: every attribute must fit inside the stride.
    // Reading past the stride feeds garbage (position/joints/weights) to the shader.
    {
        auto fmtBytes = [](DXGI_FORMAT f) -> u32 {
            switch (f) {
            case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
            case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_FLOAT: return 4u;
            case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R16G16B16A16_UINT:
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8u;
            case DXGI_FORMAT_R32G32B32_FLOAT: return 12u;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16u;
            default: return 16u;
            }
        };
        for (size_t k = 0; k < elems.size(); ++k) {
            if (elems[k].InputSlot == kZeroVertexSlot) continue;   // 零缓冲槽不受顶点步长约束
            const u32 sz = fmtBytes(elems[k].Format);
            if (m_VertexStride > 0 && elems[k].AlignedByteOffset + sz > m_VertexStride) {
                HE_CORE_ERROR("vertex layout overflow: pso={} attr[{}] {}{} offset={} size={} stride={}",
                    desc.debugName.c_str(), static_cast<u32>(k), elems[k].SemanticName,
                    elems[k].SemanticIndex, elems[k].AlignedByteOffset, sz, m_VertexStride);
                return false;
            }
        }
        HE_CORE_INFO("vertex layout ok: pso={} attrs={} stride={}",
            desc.debugName.c_str(), static_cast<u32>(elems.size()), m_VertexStride);
    }

    // 【T6】实例号不再经输入布局传入：顶点着色器按 SM 6.8 编译，用 SV_StartInstanceLocation
    // 直接拿到起始实例号（此前这里会往 14 号槽挂一条逐实例的 HE_INSTANCEINDEX 元素）
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
    if (std::getenv("HE_PSO_DUMP")) {
        HE_CORE_ERROR("[PSO-D] name={} rtv0={} numRT={} dsv={} topoType={} fill={} cull={} depthClip={} frontCCW={}",
            desc.debugName.c_str(), (u32)pd.RTVFormats[0], pd.NumRenderTargets, (u32)pd.DSVFormat,
            (int)pd.PrimitiveTopologyType, (int)pd.RasterizerState.FillMode,
            (int)pd.RasterizerState.CullMode, pd.RasterizerState.DepthClipEnable ? 1 : 0,
            pd.RasterizerState.FrontCounterClockwise ? 1 : 0);
        HE_CORE_ERROR("[PSO-D2] name={} blend0={} src={} dst={} op={} writeMask={} alphaCov={}",
            desc.debugName.c_str(), (int)pd.BlendState.RenderTarget[0].BlendEnable,
            (int)pd.BlendState.RenderTarget[0].SrcBlend, (int)pd.BlendState.RenderTarget[0].DestBlend,
            (int)pd.BlendState.RenderTarget[0].BlendOp, (u32)pd.BlendState.RenderTarget[0].RenderTargetWriteMask,
            (int)pd.BlendState.AlphaToCoverageEnable);
    }
    pd.NodeMask = 0;
    pd.CachedPSO = {};
    if (haveBlob) pd.CachedPSO = { cachedBlob.data(), cachedBlob.size() };
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

    // 网格管线：D3D12_GRAPHICS_PIPELINE_STATE_DESC 没有 MS / AS 字段，必须用管线状态流
    //（ID3D12Device2::CreatePipelineState）。光栅 / 混合 / 深度 / 目标格式直接沿用上面填好的 pd
    if (isMesh) return InitializeMeshPso(device, desc, pd, psoHash, haveBlob ? &cachedBlob : nullptr);

    HRESULT psoHr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_Pso));
    if (FAILED(psoHr) && haveBlob) {   // 缓存块与驱动 / 设备不匹配：不用缓存重建
        pd.CachedPSO = {};
        psoHr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_Pso));
    }
    if (SUCCEEDED(psoHr) && !haveBlob) device->StorePsoBlob(psoHash, m_Pso.Get());
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
    if (!desc.debugName.empty()) m_Pso->SetName(Utf8ToWide(desc.debugName.c_str()).c_str());   // RenderDoc / DRED 里显示管线名
    HE_CORE_INFO("D3D12PipelineState: PSO 创建成功 name={}", desc.debugName.c_str());
    return true;
}

namespace {
/// 管线状态流的一个子对象：类型标签 + 数据，按指针大小对齐（与 d3dx12.h 的 CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT 同布局）
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
struct alignas(void*) PsoSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T                                   value{};
};
/// 网格管线的状态流：放大着色器可为空（字节码长度 0 表示不使用）
struct MeshPsoStream {
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,        ID3D12RootSignature*>          rootSig;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS,                    D3D12_SHADER_BYTECODE>         as;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS,                    D3D12_SHADER_BYTECODE>         ms;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,                    D3D12_SHADER_BYTECODE>         ps;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,                 D3D12_BLEND_DESC>              blend;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,           UINT>                          sampleMask;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,            D3D12_RASTERIZER_DESC>         rasterizer;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,         D3D12_DEPTH_STENCIL_DESC>      depthStencil;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,    D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>         rtvFormats;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,  DXGI_FORMAT>                   dsvFormat;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,           DXGI_SAMPLE_DESC>              sampleDesc;
    PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO,            D3D12_CACHED_PIPELINE_STATE>   cachedPso;
};
} // namespace

// ---------------------------------------------------------------------------
// InitializeMeshPso —— 用管线状态流创建网格管线（MS + 可选 AS + PS）
// 除着色器外的全部状态取自已按图形管线规则填好的 gfx（光栅 / 混合 / 深度 / 目标格式 / 采样），
// 输入布局与 IB 截断值在网格管线里不存在，不放进流里。
// ---------------------------------------------------------------------------
bool D3D12PipelineState::InitializeMeshPso(D3D12Device* device, const PipelineStateDesc& desc,
                                           const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gfx,
                                           u64 psoHash, const std::vector<u8>* cachedBlob) {
    ComPtr<ID3D12Device2> dev2;
    if (!device || !device->GetDevice() || FAILED(device->GetDevice()->QueryInterface(IID_PPV_ARGS(&dev2)))) {
        HE_CORE_ERROR("D3D12PipelineState: 设备不支持 ID3D12Device2，无法创建网格管线 name={}", desc.debugName.c_str());
        return false;
    }
    MeshPsoStream s{};
    s.rootSig.value = m_RootSig.Get();
    if (desc.amplificationShader && !desc.amplificationShader->dxil.empty())
        s.as.value = { desc.amplificationShader->dxil.data(), desc.amplificationShader->dxil.size() };
    s.ms.value           = { desc.meshShader->dxil.data(), desc.meshShader->dxil.size() };
    s.ps.value           = gfx.PS;
    s.blend.value        = gfx.BlendState;
    s.sampleMask.value   = gfx.SampleMask;
    s.rasterizer.value   = gfx.RasterizerState;
    s.depthStencil.value = gfx.DepthStencilState;
    s.topology.value     = gfx.PrimitiveTopologyType;
    s.rtvFormats.value.NumRenderTargets = gfx.NumRenderTargets;
    for (u32 i = 0; i < 8; ++i) s.rtvFormats.value.RTFormats[i] = gfx.RTVFormats[i];
    s.dsvFormat.value    = gfx.DSVFormat;
    s.sampleDesc.value   = gfx.SampleDesc;
    if (cachedBlob) s.cachedPso.value = { cachedBlob->data(), cachedBlob->size() };

    D3D12_PIPELINE_STATE_STREAM_DESC sd{};
    sd.SizeInBytes                   = sizeof(s);
    sd.pPipelineStateSubobjectStream = &s;
    HRESULT hr = dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&m_Pso));
    if (FAILED(hr) && cachedBlob) {   // 缓存块与驱动 / 设备不匹配：不用缓存重建
        s.cachedPso.value = {};
        hr = dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&m_Pso));
    }
    if (FAILED(hr)) {
        HE_CORE_ERROR("D3D12PipelineState: 网格管线创建失败 name={} hr=0x{:08x} MS={}B AS={}B PS={}B",
                      desc.debugName.c_str(), static_cast<u32>(hr),
                      static_cast<u32>(s.ms.value.BytecodeLength), static_cast<u32>(s.as.value.BytecodeLength),
                      static_cast<u32>(s.ps.value.BytecodeLength));
        // 调试层消息里有被拒的确切原因
        if (auto* iq = device->GetInfoQueue()) {
            const UINT64 n = iq->GetNumStoredMessages();
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                if (!len) continue;
                std::vector<char> buf(len);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (SUCCEEDED(iq->GetMessage(i, msg, &len)) && msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                    HE_CORE_ERROR("  [D3D12 msg] id={} {}", static_cast<u32>(msg->ID), msg->pDescription);
            }
            iq->ClearStoredMessages();
        }
        return false;
    }
    if (!cachedBlob) device->StorePsoBlob(psoHash, m_Pso.Get());
    m_IsMesh = true;
    m_HasPs  = gfx.PS.BytecodeLength != 0;
    if (!desc.debugName.empty()) m_Pso->SetName(Utf8ToWide(desc.debugName.c_str()).c_str());
    HE_CORE_INFO("D3D12PipelineState: 网格管线创建成功 name={} MS={}B AS={}B",
                 desc.debugName.c_str(), static_cast<u32>(s.ms.value.BytecodeLength),
                 static_cast<u32>(s.as.value.BytecodeLength));
    return true;
}

} // namespace he::rhi::d3d12

#endif // _WIN32
