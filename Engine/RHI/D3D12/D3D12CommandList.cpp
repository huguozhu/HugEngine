// ============================================================================
//  D3D12CommandList.cpp —— D3D12 命令录制的实现
// ============================================================================
#include "D3D12CommandList.h"
#include "D3D12Device.h"
#include "D3D12Resources.h"
#include "D3D12SwapChain.h"
#include "D3D12Converters.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(_WIN32)

namespace he::rhi::d3d12 {

D3D12CommandList::~D3D12CommandList() = default;

bool D3D12CommandList::Initialize(D3D12Device* device, QueueType queue) {
    if (!device || !device->GetDevice()) return false;
    m_Device = device;
    ID3D12Device* dev = device->GetDevice();

    // 图形的 DIRECT 队列 / 计算的 COMPUTE 队列
    const D3D12_COMMAND_LIST_TYPE type =
        (queue == QueueType::Compute) ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                                      : D3D12_COMMAND_LIST_TYPE_DIRECT;

    for (auto& alloc : m_Allocators) {
        if (!HE_D3D12_CHECK(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&alloc)),
                            "CreateCommandAllocator")) {
            return false;
        }
    }
    m_Allocator = m_Allocators[0];
    if (!HE_D3D12_CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_SlotFence)),
                        "CreateFence(CommandList)")) {
        return false;
    }
    if (!HE_D3D12_CHECK(dev->CreateCommandList(0, type, m_Allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&m_List)),
                        "CreateCommandList")) {
        return false;
    }
    // 创建后默认是开着的，先关掉，等 Begin() 再 Reset
    m_List->Close();
    return true;
}

void D3D12CommandList::Begin() {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] Begin %d\n", s); std::fflush(stdout); } }

    if (!m_List || !m_Allocator) { std::printf("[BGN] no list/alloc\n"); std::fflush(stdout); return; }
    // 轮换到下一个帧槽位，并等待该槽位上一次提交的命令在 GPU 上执行完，再复用其分配器
    m_FrameSlot = (m_FrameSlot + 1) % kMaxFramesInFlight;
    m_Allocator = m_Allocators[m_FrameSlot];
    if (m_SlotFence && m_SlotFence->GetCompletedValue() < m_SlotFenceValues[m_FrameSlot]) {
        HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (ev) {
            m_SlotFence->SetEventOnCompletion(m_SlotFenceValues[m_FrameSlot], ev);
            WaitForSingleObject(ev, 8000);   // 有上限：设备挂起时不至于让进程永久卡住
            CloseHandle(ev);
        }
    }
    // 每次录制前必须 Reset 分配器与列表
    std::printf("[BGN] before alloc Reset\n"); std::fflush(stdout);
    if (FAILED(m_Allocator->Reset())) {
        // 【关键】分配器仍被 GPU 使用时 Reset 会失败。此前直接 return，
        // 列表停留在 Close 状态，之后所有录制（清屏、绘制、屏障）都被 D3D12 丢弃，
        // 调试层报 "This API cannot be called on a closed command list"，
        // 表现就是「引擎在出帧、几何也在提交，但画面全黑」。
        // 这里等 GPU 空闲后重试一次。
        HE_CORE_WARN("D3D12CommandList::Begin: 分配器 Reset 失败，等待 GPU 空闲后重试");
        if (m_Device) m_Device->WaitIdle();
        if (FAILED(m_Allocator->Reset())) {
            HE_CORE_ERROR("D3D12CommandList::Begin: 分配器 Reset 仍失败");
            return;
        }
    }
    std::printf("[BGN] before list Reset\n"); std::fflush(stdout);
    if (FAILED(m_List->Reset(m_Allocator.Get(), nullptr))) {
        HE_CORE_ERROR("D3D12CommandList::Begin: 列表 Reset 失败");
        return;
    }
    m_Recording = true;
    // 新的录制：列表的根参数与描述符堆都已失效，清空缓存
    for (auto& t : m_RootTables) t.ptr = 0;
    m_RootArgsDirty = true;
    m_HeapsSet = false;
    // 为整帧申请一块推送常量槽位，并清零
    m_PushGpu = 0;
    m_PushCpu = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE dummyCbv{};
    if (m_Device && m_Device->AllocateConstants(m_PushGpu, dummyCbv)) {
        m_PushCpu = m_Device->GetConstantSlotPtr(m_PushGpu);
        if (m_PushCpu) std::memset(m_PushCpu, 0, 256);
    }
}

void D3D12CommandList::OnSubmitted(ID3D12CommandQueue* queue) {
    if (!queue || !m_SlotFence) return;
    const u64 v = m_NextFenceValue++;
    queue->Signal(m_SlotFence.Get(), v);
    m_SlotFenceValues[m_FrameSlot] = v;
}

void D3D12CommandList::PrepareForExternalRendering() {
    if (!m_List || !m_Device || !m_Device->EnsureDescriptorHeaps()) return;
    ID3D12DescriptorHeap* heaps[2] = { m_Device->GetCbvSrvUavHeap(), m_Device->GetSetSamplerHeap() };
    if (heaps[0] && heaps[1]) {
        m_List->SetDescriptorHeaps(2, heaps);
        m_HeapsSet = true;
    }
    m_Pipeline = nullptr;      // 外部会设置自己的 PSO / 根签名
    m_RootArgsDirty = true;
}

void D3D12CommandList::End() {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] End %d\n", s); std::fflush(stdout); } }
    if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] End\n"); std::fflush(stdout); }

    if (!m_List || !m_Recording) return;
    if (FAILED(m_List->Close())) {
        HE_CORE_ERROR("D3D12CommandList::End: Close 失败");
    }
    m_Recording = false;
}

void D3D12CommandList::BeginRenderPass(u32 colorCount, Format colorFormat,
                                       Format depthFormat, const ClearValue* clear,
                                       LoadOp loadOp) {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] BRP %d\n", s); std::fflush(stdout); } }

    if (!m_List) return;

    // 后缓冲的 RTV：从交换链取（引擎传的 colorFormat 是后缓冲格式）
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    bool hasRtv = false;
    // 【兜底】渲染线程创建的命令列表未必带 m_SwapChain；此时若不兜底，
    // hasRtv 会是 false ⇒ 后缓冲根本没被绑定 ⇒ 最终画面永远黑，
    // 且后缓冲状态停在 PRESENT/COMMON ⇒ 截图时的屏障与拷贝也无效。
    if (!m_SwapChain && m_Device && m_Device->GetSwapChainPtr()) {
        m_SwapChain = m_Device->GetSwapChainPtr();
    }
    if (m_SwapChain) {
        // 引擎的 IRHISwapChain 只有 D3D12 交换链会走到这里；
        // 用 static_cast 而不是 dynamic_cast，避免依赖 RTTI。
        auto* d3dSc = static_cast<D3D12SwapChain*>(m_SwapChain);
        rtv = d3dSc->GetCurrentRtv();
        hasRtv = true;
    }

    // 【交换链后缓冲必须显式屏障】D3D12 不会把 PRESENT 隐式提升为 RENDER_TARGET，
    // 不做转换就以 RTV 使用会被判非法、渲染内容被丢弃（调试层 id=538 实测 1669 条，
    // 这也是 02.Cube 画面全黑的根因）。
    if (hasRtv && m_SwapChain) {
        auto* sc = static_cast<D3D12SwapChain*>(m_SwapChain);
        ID3D12Resource* bb = sc->GetCurrentBackBufferResource();
        if (bb && sc->GetBackBufferState() != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER tb{};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition.pResource   = bb;
            tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            tb.Transition.StateBefore = sc->GetBackBufferState();
            tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            m_List->ResourceBarrier(1, &tb);
        }
    }

    m_List->OMSetRenderTargets(colorCount, hasRtv ? &rtv : nullptr, FALSE, nullptr);
    m_HasRtv = hasRtv;
    // 告诉交换链：后缓冲现在处于 RENDER_TARGET 状态（截图做屏障时要用真实状态）
    if (hasRtv && m_SwapChain) {
        static_cast<D3D12SwapChain*>(m_SwapChain)->MarkBackBufferRendered();
    }

    if (hasRtv && loadOp == LoadOp::Clear) {
        const float c[4] = {
            clear ? clear->color[0] : 0.0f,
            clear ? clear->color[1] : 0.0f,
            clear ? clear->color[2] : 0.0f,
            clear ? clear->color[3] : 1.0f,
        };
        m_List->ClearRenderTargetView(rtv, c, 0, nullptr);
    }

    // 视口与裁剪默认铺满整个后缓冲
    if (m_SwapChain) {
        D3D12_VIEWPORT vp{};
        vp.Width  = static_cast<float>(m_SwapChain->GetWidth());
        vp.Height = static_cast<float>(m_SwapChain->GetHeight());
        vp.MaxDepth = 1.0f;
        m_List->RSSetViewports(1, &vp);
        D3D12_RECT rc{ 0, 0,
                       static_cast<LONG>(m_SwapChain->GetWidth()),
                       static_cast<LONG>(m_SwapChain->GetHeight()) };
        m_List->RSSetScissorRects(1, &rc);
    }
    (void)depthFormat;
}

void D3D12CommandList::EndRenderPass() {
    if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] EndRenderPass\n"); std::fflush(stdout); }

    // D3D12 没有 pass 结束的概念；资源状态转换由显式 PipelineBarrier 负责
}

void D3D12CommandList::BeginOffscreenPass(void* colorImageView, void* depthImageView,
                                          u32 width, u32 height,
                                          const ClearValue* clear,
                                          bool allowSecondary) {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] BOP %d\n", s); std::fflush(stdout); } }
    (void)allowSecondary;
    if (!m_List) return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ToCpuHandle(colorImageView);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ToCpuHandle(depthImageView);
    const bool hasRtv = (colorImageView != nullptr);
    const bool hasDsv = (depthImageView != nullptr);

    // RTV 数量必须与指针一致：只有深度附件的 pass 传 1+nullptr 是非法调用（设备会被移除）
    // 【必须做资源屏障】D3D12 要求绑定为 RTV/DSV 的资源处于 RENDER_TARGET /
    // DEPTH_WRITE 状态。引擎只把 CPU 句柄传进来，所以先按句柄反查资源，
    // 再按记录的状态做一次转换（否则 OMSetRenderTargets / Clear 会静默卡死）。
    std::printf("[BOPA] barrier begin\n"); std::fflush(stdout);
    if (m_Device) {
        if (hasRtv) {
            if (auto* res = m_Device->FindViewResource(static_cast<UINT64>(rtv.ptr))) {
                const auto from = m_Device->GetResourceState(res);
                // 【只要不是目标状态就显式转换】此前只在「当前是着色器资源」时才转，
                // 从 COMMON 依赖所谓隐式提升 —— 但调试层明确判定 COMMON 不能直接当
                // 渲染目标使用（id=538 实测 1118 条），渲染内容被丢弃。
                if (from != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = res;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = from;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    m_List->ResourceBarrier(1, &b);
                    m_Device->SetResourceState(res, D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }
        }
        if (hasDsv) {
            if (auto* res = m_Device->FindViewResource(static_cast<UINT64>(dsv.ptr))) {
                const auto from = m_Device->GetResourceState(res);
                if (from != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = res;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = from;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    m_List->ResourceBarrier(1, &b);
                    m_Device->SetResourceState(res, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                }
            }
        }
    }

    std::printf("[BOPB] rtv=%p rtvRes=%p dsv=%p\n", (void*)rtv.ptr, (void*)m_Device->FindViewResource(static_cast<UINT64>(rtv.ptr)), (void*)dsv.ptr); std::fflush(stdout);
    // 【HDR 中间结果转储】进入宽屏离屏 pass 时，把该目标回读并打印首像素。
    // 此刻里面是**上一帧**完整渲染后的结果，可判断几何是否真的画进了 HDR。
    // 【任意尺寸的渲染目标回读扫描】HE_DUMP_HDR=1 时，把每个离屏 pass 的目标
    // 拷到回读缓冲，下一帧等 GPU 完成后扫描整张图，统计非零像素与最大值。
    // 用途：定位"哪个 pass 真正产出了内容"，从而找出黑屏发生在哪一步。
    if (std::getenv("HE_DUMP_HDR") && hasRtv && m_Device && m_Device->GetDevice() && m_Device->GetQueue()) {
        if (ID3D12Resource* hres = m_Device->FindViewResource(static_cast<UINT64>(rtv.ptr))) {
            const D3D12_RESOURCE_DESC hd = hres->GetDesc();
            const u32 bpp = 8;                       // 只处理 16F/RGBA 类
            const u32 rowPitch = static_cast<u32>((hd.Width * bpp + 255) & ~255ull);
            const u64 need = static_cast<u64>(rowPitch) * hd.Height;
            static ComPtr<ID3D12Resource> s_rb;
            static int s_pending = 0;
            static u64 s_key = 0;
            const u64 key = (static_cast<u64>(hd.Width) << 32) | static_cast<u64>((u32)hd.Format);
            if (!s_rb || s_key != key) {
                s_rb.Reset();
                D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = need; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
                rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc = {1,0};
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                m_Device->GetDevice()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s_rb));
                s_key = key; s_pending = 0;
            }
            if (s_rb && s_pending) {
                m_Device->WaitIdle();
                void* mp = nullptr; D3D12_RANGE rr{0,0};
                if (SUCCEEDED(s_rb->Map(0, &rr, &mp)) && mp) {
                    const unsigned char* raw = static_cast<const unsigned char*>(mp);
                    u64 nonZero = 0, total = 0; float mx = 0.0f;
                    for (u32 yy = 0; yy < static_cast<u32>(hd.Height); yy += 8) {
                        const float* row = reinterpret_cast<const float*>(raw + static_cast<size_t>(yy) * rowPitch);
                        for (u32 xx = 0; xx < static_cast<u32>(hd.Width); xx += 8) {
                            const float r0 = row[xx*4+0], g0 = row[xx*4+1], b0 = row[xx*4+2];
                            ++total;
                            if (r0 != 0.0f || g0 != 0.0f || b0 != 0.0f) ++nonZero;
                            if (r0 > mx) mx = r0; if (g0 > mx) mx = g0; if (b0 > mx) mx = b0;
                        }
                    }
                    HE_CORE_ERROR("[RT-SCAN] {}x{} fmt={} 采样={} 非零={} max={}",
                        static_cast<u64>(hd.Width), static_cast<u32>(hd.Height),
                        static_cast<u32>(hd.Format), static_cast<u64>(total),
                        static_cast<u64>(nonZero), mx);
                    s_rb->Unmap(0, nullptr);
                }
                s_pending = 0;
            }
            if (s_rb) {
                D3D12_RESOURCE_BARRIER bb{};
                bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bb.Transition.pResource = hres;
                bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb.Transition.StateBefore = m_Device->GetResourceState(hres);
                bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                if (bb.Transition.StateBefore != bb.Transition.StateAfter) m_List->ResourceBarrier(1, &bb);
                D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = s_rb.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Footprint.Format = hd.Format;
                dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(hd.Width);
                dst.PlacedFootprint.Footprint.Height = hd.Height;
                dst.PlacedFootprint.Footprint.Depth = 1;
                dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
                D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = hres;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
                m_List->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                D3D12_RESOURCE_BARRIER rb = bb;
                rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                rb.Transition.StateAfter  = m_Device->GetResourceState(hres);
                if (rb.Transition.StateBefore != rb.Transition.StateAfter) m_List->ResourceBarrier(1, &rb);
                s_pending = 1;
            }
        }
    }

    m_List->OMSetRenderTargets(hasRtv ? 1u : 0u, hasRtv ? &rtv : nullptr, FALSE, hasDsv ? &dsv : nullptr);
    std::printf("[BOPC] after OMSetRT\n"); std::fflush(stdout);
    m_HasRtv = hasRtv;

    if (hasRtv && clear) {
        m_List->ClearRenderTargetView(rtv, clear->color, 0, nullptr);
        std::printf("[BOPD] after ClearRTV\n"); std::fflush(stdout);
    }
    if (hasDsv) {
        // 【只有带模板的格式才能传 STENCIL 标志】D32_FLOAT 没有模板位，
        // 同时传 DEPTH|STENCIL 会让整个 ClearDepthStencilView 被拒，
        // 深度缓冲留在未初始化状态 ⇒ 所有物体（含天空盒）都过不了深度测试 ⇒ 画面全黑。
        D3D12_CLEAR_FLAGS dflags = D3D12_CLEAR_FLAG_DEPTH;
        if (m_Device) {
            if (ID3D12Resource* dres = m_Device->FindViewResource(static_cast<UINT64>(dsv.ptr))) {
                const DXGI_FORMAT df = dres->GetDesc().Format;
                if (df == DXGI_FORMAT_D24_UNORM_S8_UINT || df == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
                    dflags |= D3D12_CLEAR_FLAG_STENCIL;
            }
        }
        m_List->ClearDepthStencilView(dsv, dflags,
                                       clear ? clear->depth : 1.0f,
                                       clear ? clear->stencil : 0, 0, nullptr);
    }

    D3D12_VIEWPORT vp{};
    vp.Width  = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    std::printf("[BOPY] before VP\n"); std::fflush(stdout);
    m_List->RSSetViewports(1, &vp);
    D3D12_RECT rc{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    m_List->RSSetScissorRects(1, &rc);

    HE_CORE_INFO("[D3D12 离屏] rtv={} dsv={} {}x{}",
                 hasRtv ? "有" : "无", hasDsv ? "有" : "无", width, height);
}

void D3D12CommandList::BeginOffscreenPassMRT(void* const* colorImageViews, u32 colorCount,
                                             void* depthImageView, u32 width, u32 height,
                                             const ClearValue* clears,
                                             bool allowSecondary) {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] BOPM %d\n", s); std::fflush(stdout); } }
    (void)allowSecondary;
    if (!m_List || colorCount == 0) return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8]{};
    const u32 n = (colorCount < 8) ? colorCount : 8;
    for (u32 i = 0; i < n; ++i) rtvs[i] = ToCpuHandle(colorImageViews[i]);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ToCpuHandle(depthImageView);
    const bool hasDsv = (depthImageView != nullptr);

    // 【MRT 也必须做状态转换】此前只有单目标路径做了屏障，多目标 pass（GBuffer 等）
    // 的渲染目标停在 COMMON/PRESENT，被调试层判定非法、绘制被丢弃（id=538）。
    for (u32 i = 0; i < n; ++i) {
        if (!colorImageViews[i] || !m_Device) continue;
        if (ID3D12Resource* res = m_Device->FindViewResource(static_cast<UINT64>(rtvs[i].ptr))) {
            const auto from = m_Device->GetResourceState(res);
            if (from != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = res;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = from;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                m_List->ResourceBarrier(1, &tb);
                m_Device->SetResourceState(res, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    }
    if (hasDsv && m_Device) {
        if (ID3D12Resource* dres = m_Device->FindViewResource(static_cast<UINT64>(dsv.ptr))) {
            const auto from = m_Device->GetResourceState(dres);
            if (from != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = dres;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = from;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                m_List->ResourceBarrier(1, &tb);
                m_Device->SetResourceState(dres, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            }
        }
    }
    m_List->OMSetRenderTargets(n, rtvs, FALSE, hasDsv ? &dsv : nullptr);

    if (clears) {
        for (u32 i = 0; i < n; ++i) {
            if (colorImageViews[i]) m_List->ClearRenderTargetView(rtvs[i], clears[i].color, 0, nullptr);
        }
    }
    if (hasDsv) {
        // 【只有带模板的格式才能传 STENCIL 标志】D32_FLOAT 没有模板位，
        // 同时传 DEPTH|STENCIL 会让整个 ClearDepthStencilView 被拒，
        // 深度缓冲留在未初始化状态 ⇒ 所有物体（含天空盒）都过不了深度测试 ⇒ 画面全黑。
        D3D12_CLEAR_FLAGS dflags = D3D12_CLEAR_FLAG_DEPTH;
        if (m_Device) {
            if (ID3D12Resource* dres = m_Device->FindViewResource(static_cast<UINT64>(dsv.ptr))) {
                const DXGI_FORMAT df = dres->GetDesc().Format;
                if (df == DXGI_FORMAT_D24_UNORM_S8_UINT || df == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
                    dflags |= D3D12_CLEAR_FLAG_STENCIL;
            }
        }
        m_List->ClearDepthStencilView(dsv, dflags,
                                       clears ? clears[0].depth : 1.0f,
                                       clears ? clears[0].stencil : 0, 0, nullptr);
    }

    D3D12_VIEWPORT vp{};
    vp.Width  = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    m_List->RSSetViewports(1, &vp);
    D3D12_RECT rc{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    m_List->RSSetScissorRects(1, &rc);
}

void D3D12CommandList::EndOffscreenPass() {}

void D3D12CommandList::SetPipeline(IRHIPipelineState* pso) {

    m_Pipeline = static_cast<D3D12PipelineState*>(pso);
    if (!m_List || !m_Pipeline) return;
    m_List->SetPipelineState(m_Pipeline->GetPso());
    // 【图形/计算根签名是两套独立绑定】计算管线必须用 SetComputeRootSignature，
    // 否则后面 SetComputeRootDescriptorTable 没有根签名可用，D3D12 会静默卡死。
    m_IsComputePipeline = m_Pipeline->IsCompute();
    if (m_IsComputePipeline) {
        m_List->SetComputeRootSignature(m_Pipeline->GetRootSignature());
    } else {
        m_List->SetGraphicsRootSignature(m_Pipeline->GetRootSignature());
    }
    m_List->IASetPrimitiveTopology(m_Pipeline->GetTopology());
    // 引擎布局缺失的属性挂在零缓冲槽上，这里把零缓冲绑上
    if (m_Pipeline->NeedsZeroVertexBuffer() && m_Device) {
        const D3D12_VERTEX_BUFFER_VIEW zv = m_Device->GetZeroVertexBufferView();
        if (zv.BufferLocation) m_List->IASetVertexBuffers(D3D12PipelineState::kZeroVertexSlot, 1, &zv);
    }
    // 设置根签名会清空所有根参数，下一次绘制/派发前需要重新下发
    m_RootArgsDirty = true;
}

void D3D12CommandList::SetVertexBuffer(IRHIBuffer* buffer, u32 binding) {
    { static int n4=0; if (n4++ < 30 && buffer) { std::printf("[VBSET] %p\n", (void*)buffer); std::fflush(stdout); } }
    if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] SetVertexBuffer\n"); std::fflush(stdout); }
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);
    // 【关键】StrideInBytes 必须非零：D3D12 的 IASetVertexBuffers 收到 0 步长时
    // 会静默卡住（实测 01.Triangle 就卡在这个调用里，CPU 0%、无错误日志）。
    // 引擎的 Buffer 不一定填 stride ⇒ 用当前 PSO 从输入布局算出的步长兜底。
    u32 stride = static_cast<u32>(b->GetStride());
    if (stride == 0 && m_Pipeline) stride = m_Pipeline->GetVertexStride();
    if (stride == 0) {
        HE_CORE_WARN("D3D12 SetVertexBuffer: 步长为 0（buffer 与 PSO 都没提供），跳过绑定");
        return;
    }
    D3D12_VERTEX_BUFFER_VIEW vbv = b->GetVbv(stride);
    m_List->IASetVertexBuffers(binding, 1, &vbv);
}

void D3D12CommandList::SetIndexBuffer(IRHIBuffer* buffer, u32 offset) {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] SetIB %d\n", s); std::fflush(stdout); } }
    if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] SetIndexBuffer\n"); std::fflush(stdout); }
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);
    D3D12_INDEX_BUFFER_VIEW ibv = b->GetIbv();
    ibv.BufferLocation += offset;
    m_List->IASetIndexBuffer(&ibv);
}

void D3D12CommandList::SetViewport(const Viewport& vp) {
    if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] SetViewport\n"); std::fflush(stdout); }
    if (!m_List) return;
    D3D12_VIEWPORT v{};
    v.TopLeftX = vp.x;
    v.TopLeftY = vp.y;
    v.Width    = vp.width;
    v.Height   = vp.height;
    v.MinDepth = vp.minDepth;
    v.MaxDepth = vp.maxDepth;
    // D3D12 的 Y 轴向下；引擎传负高度表示「原点在左下」，这里换算成 D3D12 的原点左上
    if (vp.height < 0.0f) {
        v.TopLeftY = vp.y + vp.height;
        v.Height   = -vp.height;
    }
    m_LastVp = v;
    m_List->RSSetViewports(1, &v);
}

void D3D12CommandList::SetScissor(const ScissorRect& sc) {
    if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] SetScissor\n"); std::fflush(stdout); }
    if (!m_List) return;
    D3D12_RECT r{};
    r.left   = sc.x;
    r.top    = sc.y;
    r.right  = sc.x + static_cast<i32>(sc.width);
    r.bottom = sc.y + static_cast<i32>(sc.height);
    m_List->RSSetScissorRects(1, &r);
}

void D3D12CommandList::SetFullscreenViewport(u32 width, u32 height) {
    Viewport vp{};
    vp.width  = static_cast<float>(width);
    vp.height = static_cast<float>(height);
    SetViewport(vp);
    ScissorRect sc{};
    sc.width  = width;
    sc.height = height;
    SetScissor(sc);
}

void D3D12CommandList::BindDescriptorSet(u32 setIndex, DescriptorSetHandle set) {
    static int s_call=0; std::printf("[BSN] call#%d setIndex=%u set=%llu\n", ++s_call, setIndex, (unsigned long long)set); std::fflush(stdout);
    { static int s=0; if (s<4) { ++s; std::printf("[STP] BindSet %d\n", s); std::fflush(stdout); } }
    if (!m_List || !m_Device || set == kInvalidSet || setIndex >= 8) return;
    if (!m_Device->EnsureDescriptorHeaps()) return;
    m_Device->FlushPendingBindlessBuffers();   // 补写初始化早期被丢弃的 bindless 缓冲描述符

    ID3D12DescriptorHeap* heaps[2] = { m_Device->GetCbvSrvUavHeap(), m_Device->GetSetSamplerHeap() };
    if (!heaps[0] || !heaps[1]) return;
    m_List->SetDescriptorHeaps(2, heaps);
    m_HeapsSet = true;
    std::printf("[BS] heaps ok\n"); std::fflush(stdout);

    const u32 baseSlot = m_Device->GetSetBaseSlot(set);
    m_LastBoundSet = set; m_LastBaseSlot = baseSlot;
    { static int n8=0; if (++n8 % 60 == 1) { std::printf("[LISTID-B] list=%p set=%llu base=%u\n", (void*)this, (unsigned long long)set, baseSlot); std::fflush(stdout); } }
    std::printf("[BS] base ok\n"); std::fflush(stdout);
    if (baseSlot == 0xFFFFFFFFu) return;
    std::printf("[BS] baseret ok\n"); std::fflush(stdout);

    // 根签名：0..7 = root CBV，8..11 = SRV 表(space0..3)，12..15 = Sampler 表(space0..3)
    const D3D12_GPU_DESCRIPTOR_HANDLE srv  = m_Device->GetSetGpuHandle(baseSlot);
    // 采样器堆只有 2048 槽，统一从槽 0 起绑定（见 UpdateDescriptorSet 的说明）
    const D3D12_GPU_DESCRIPTOR_HANDLE smp  = m_Device->GetSetSamplerGpuHandle(0);

    // 根签名：0..7 CBV，8..11 SRV，12..15 UAV，16..19 Sampler
    const u32 uavSlot  = baseSlot + m_Device->GetSetUavOffset(set);
    const D3D12_GPU_DESCRIPTOR_HANDLE uav = m_Device->GetSetGpuHandle(uavSlot);

    // 【参数偏移必须与根签名一致】根签名布局：8 个 root CBV 之后依次是
    //   SRV 表 8..15、UAV 表 16..23、Sampler 表 24..31（每类 8 个寄存器空间）。
    // 之前这里用 12/16 是老布局（当时 kSpaceCount=4）的偏移，把 UAV/Sampler
    // 表绑到了 SRV 的槽位上，描述符类型对不上会让 D3D12 直接卡死。
    std::printf("[BS] srv=%llu uav=%llu\n",(unsigned long long)srv.ptr,(unsigned long long)uav.ptr); std::fflush(stdout);
    { static int n=0; if (++n <= 8) { const auto h0=m_Device->GetSrvHeapGpuStart();
        std::printf("[GPUDIFF] setIndex=%u base=%u gpuOff=%llu\n", setIndex, baseSlot,
            (unsigned long long)(srv.ptr - h0.ptr)); std::fflush(stdout); } }
    if (std::getenv("HE_COPY0") && setIndex == 0) {
        // diag: copy this set SRV range to slot 0 and bind heap base
        m_Device->GetDevice()->CopyDescriptorsSimple(32,
            m_Device->GetSlotCpuHandle(0), m_Device->GetSlotCpuHandle(baseSlot),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        SetRootTable(8, m_Device->GetSrvHeapGpuStart());
    } else {
        SetRootTable(8  + setIndex, srv);
    }
    // 只记录到缓存，由 FlushRootArgs 在绘制/派发前按当前管线类型（图形/计算）下发，
    // 这样 BindDescriptorSet 与 SetPipeline 的先后顺序不再影响结果
    SetRootTable(16 + setIndex, uav);
    std::printf("[BS] g-uav ok\n"); std::fflush(stdout);
    SetRootTable(24 + setIndex, smp);
    std::printf("[BS] g-smp ok\n"); std::fflush(stdout);
    std::printf("[BS] tables ok\n"); std::fflush(stdout);
}

void D3D12CommandList::SetRootTable(u32 param, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    if (param >= 32u) return;
    m_RootTables[param] = handle;
    m_RootArgsDirty = true;
}

void D3D12CommandList::FlushRootArgs() {
    if (!m_List || !m_Pipeline || !m_RootArgsDirty) return;
    // 描述符表必须在 SetDescriptorHeaps 之后才能设置
    if (!m_HeapsSet && m_Device && m_Device->EnsureDescriptorHeaps()) {
        ID3D12DescriptorHeap* heaps[2] = { m_Device->GetCbvSrvUavHeap(), m_Device->GetSetSamplerHeap() };
        if (heaps[0] && heaps[1]) {
            m_List->SetDescriptorHeaps(2, heaps);
            m_HeapsSet = true;
        }
    }
    // 推送常量（root CBV b0/b1）：Begin 已为本次录制分配了槽位，
    // 即使本次绘制没有推送也要绑上，否则着色器读到未初始化的根参数会让 GPU 挂起
    if (m_PushGpu) {
        if (m_IsComputePipeline) {
            m_List->SetComputeRootConstantBufferView(0, m_PushGpu);
        } else {
            m_List->SetGraphicsRootConstantBufferView(0, m_PushGpu);
            m_List->SetGraphicsRootConstantBufferView(1, m_PushGpu);
        }
    }
    if (m_HeapsSet) {
        for (u32 i = 8; i < 32u; ++i) {
            if (!m_RootTables[i].ptr) continue;
            if (m_IsComputePipeline) m_List->SetComputeRootDescriptorTable(i, m_RootTables[i]);
            else                     m_List->SetGraphicsRootDescriptorTable(i, m_RootTables[i]);
        }
    }
    m_RootArgsDirty = false;
}

void D3D12CommandList::SetPushConstants(u32 offset, u32 size, const void* data) {
    if (!m_List || !m_Device || !data) return;

    // 【累积式推送】引擎一次"常量集"可能分多次推送：先 offset=0 推相机/帧常量，
    // 再用 offset>0 补逐物体数据。它们必须落在**同一块**常量缓冲里。
    // 只有当 offset==0（新的一组常量开始）时才另开槽位；其余情况继续写当前槽位。
    // 一次绘制的多次推送（相机 offset=0、逐物体 offset=84 等）必须共享同一槽位；
    // 只有「上一次绘制结束」后才另开槽位，否则后一次推送会覆盖掉前一次写入的字段
    //（实测：物体索引被相机推送覆盖成 0xCCCCCCCC ⇒ 所有绘制都画同一个物体）。
    if (m_PushDirty || m_PushGpu == 0 || !m_PushCpu) {
        D3D12_GPU_VIRTUAL_ADDRESS slot = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle{};
        if (!m_Device->AllocateConstants(slot, cbvHandle)) return;
        void* cpu = m_Device->GetConstantSlotPtr(slot);
        if (!cpu) return;
        // 新槽位继承上一槽位的内容：Vulkan 的推送常量在被覆盖前一直有效，
        // 引擎会依赖这一点（例如只补推逐物体字段）；本次录制的第一个槽位从 0 开始
        if (m_PushCpu) std::memcpy(cpu, m_PushCpu, 256);
        else           std::memset(cpu, 0, 256);
        m_PushGpu = slot;
        m_PushCpu = cpu;
        m_PushDirty = false;
        if (m_IsComputePipeline) {
            m_List->SetComputeRootConstantBufferView(0, slot);
        } else {
            // 生成的 HLSL 里 FrameConstants 在 register(b0)/(b1)，两者都绑同一块
            m_List->SetGraphicsRootConstantBufferView(0, slot);
            m_List->SetGraphicsRootConstantBufferView(1, slot);
        }
    }

    // 记录本次推常量里的 objectIndex（偏移 84 = 第 21 个 u32），供绘制时核对
    if (offset <= 84u && offset + size >= 88u && data) {
        const u8* raw = static_cast<const u8*>(data);
        m_LastObjIdx = *reinterpret_cast<const u32*>(raw + (84u - offset));
    }
    if (size >= 160u && data) {
        static int n1=0; if (++n1 % 300 == 1) {
            const u32* w = static_cast<const u32*>(data);
            std::printf("[PCSET] list=%p light=%u obj=%u useInst=%u w24..31=%u,%u,%u,%u,%u,%u,%u,%u\n",
                (void*)this, w[20], w[21], w[22],
                w[24],w[25],w[26],w[27],w[28],w[29],w[30],w[31]);
            std::fflush(stdout);
        }
    }
    if (offset >= 256u) return;
    const u32 copySize = ((offset + size) > 256u) ? (256u - offset) : size;
    std::memcpy(static_cast<unsigned char*>(m_PushCpu) + offset, data, copySize);
}

void D3D12CommandList::Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] Draw %d\n", s); std::fflush(stdout); } }
    // 诊断：每 60 次绘制打印一次（D3D12 的 GraphicsCommandList 没有 Get* 查询接口，
    // 所以这里打印我们自己记录的状态）
    if (std::getenv("HE_D3D12_TRACE")) {
        static int s_draw = 0;
        if (++s_draw % 60 == 1) {
            std::printf("[DRAW] n=%d verts=%u inst=%u pso=%p rtvSet=%d vp=%.0fx%.0f\n",
                        s_draw, vertexCount, instanceCount,
                        static_cast<void*>(m_Pipeline),
                        m_HasRtv ? 1 : 0, m_LastVp.Width, m_LastVp.Height);
            std::fflush(stdout);
        }
    }
    if (!m_List) return;
    FlushRootArgs();
    m_List->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    // 本次绘制已消费当前常量槽：之后的推送必须写新槽位，否则 GPU 执行时
    // 前面的绘制会读到后写入的值（实测天空盒读到后续全屏 pass 的矩阵，天空成纯色）
    m_PushDirty = true;
}

void D3D12CommandList::DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                                   i32 vertexOffset, u32 firstInstance) {
    { static int n7=0; if (++n7 % 200 == 1) {
        const unsigned k = (m_LastObjIdx < 32u) ? m_LastObjIdx : 0u;
        std::printf("[OBJIDX] idx=%u scale=(%.2f,%.2f,%.2f) trans=(%.2f,%.2f,%.2f)\n",
            m_LastObjIdx, m_Device->m_LastObjM[k][0], m_Device->m_LastObjM[k][1], m_Device->m_LastObjM[k][2],
            m_Device->m_LastObjM[k][3], m_Device->m_LastObjM[k][4], m_Device->m_LastObjM[k][5]);
        std::fflush(stdout); } }
    { static int n8=0; if (++n8 % 400 == 1) { std::printf("[OBJFIX] obj=%p base=%u\n",
        (void*)m_Device->GetLastObjectBuffer(), m_LastBaseSlot); std::fflush(stdout); } }
    // 绘制前重新绑定 space0 的 SRV 表：引擎只在 pass 开始绑一次，若期间有别的
    // BindDescriptorSet（同样是 setIndex=0）覆盖了同一张表，绘制时就会读到别的 set。
    if (m_Device && m_LastBaseSlot != 0xFFFFFFFFu) {
        const D3D12_GPU_DESCRIPTOR_HANDLE s0 = m_Device->GetSetGpuHandle(m_LastBaseSlot);
        if (s0.ptr) SetRootTable(8, s0);
    }
    { static int n=0; if (indexCount == 1728 && ++n <= 4) { std::printf("[DRAWSET] set=%llu base=%u idx=%u\n", (unsigned long long)m_LastBoundSet, m_LastBaseSlot, indexCount); std::fflush(stdout); } }
    // space2 绑定分测：分别用 HE_BIND2_SRV / HE_BIND2_SMP 控制
    if (m_Device && m_List) {
        if (true) {   // space2 的 bindless 表必须总是绑定
            const D3D12_GPU_DESCRIPTOR_HANDLE h1 = m_Device->GetBindlessSrvGpuHandle();
            if (h1.ptr) SetRootTable(10, h1);
        }
        if (true) {
            const D3D12_GPU_DESCRIPTOR_HANDLE h2 = m_Device->GetBindlessSmpGpuHandle();
            if (h2.ptr) SetRootTable(26, h2);
        }
    }
    if (std::getenv("HE_D3D12_NO_DRAW")) return;   // 实验开关：跳过实际绘制
    if (!m_List) return;
    { static int n6=0; if (++n6 % 90 == 1) { std::printf("[DRAWLOG] idx=%u inst=%u\n", indexCount, instanceCount); std::fflush(stdout); } }
    { static int n2=0; if (++n2 % 150 == 1 && m_PushCpu) {
        const u32* w = static_cast<const u32*>(m_PushCpu);
        std::printf("[PCFIELDS] idx=%u light=%u obj=%u useInst=%u ibl=%.2f w24..31=%u,%u,%u,%u,%u,%u,%u,%u\n",
            indexCount, w[20], w[21], w[22], *reinterpret_cast<const float*>(&w[23]),
            w[24],w[25],w[26],w[27],w[28],w[29],w[30],w[31]);
        std::fflush(stdout); } }
        { static u32* w=nullptr; static int n=0;
          if (m_PushCpu) w = static_cast<u32*>(m_PushCpu);
          if (w && w[22] >= 2u && n++ < 3) {
            std::printf("[INSTH] useInst=%u w24..29=%u,%u,%u,%u,%u,%u\n",
                w[22], w[24],w[25],w[26],w[27],w[28],w[29]); std::fflush(stdout); } }
        m_PushDirty = true;   // 下一次推送开始新的常量槽位
    // rebind space0 SRV table right before the draw: a later BindDescriptorSet
    // (also setIndex 0) can overwrite the table with a different set.
    if (m_Device && m_LastBaseSlot != 0xFFFFFFFFu) {
        const D3D12_GPU_DESCRIPTOR_HANDLE s0 = m_Device->GetSetGpuHandle(m_LastBaseSlot);
        if (s0.ptr) SetRootTable(8, s0);
    }
    FlushRootArgs();
    m_List->DrawIndexedInstanced(indexCount, instanceCount, firstIndex,
                                 static_cast<INT>(vertexOffset), firstInstance);
}

void D3D12CommandList::DrawIndexedIndirect(IRHIBuffer* buffer, u64 offset, u32 drawCount, u32 stride) {
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);

    // 命令签名按 stride 缓存：必须活到 GPU 执行完，否则间接绘制无效（id=921）
    const u32 want = stride ? stride : static_cast<u32>(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
    ID3D12CommandSignature* sig = nullptr;
    auto it = m_CmdSigs.find(want);
    if (it != m_CmdSigs.end()) {
        sig = it->second.Get();
    } else {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC sd{};
        sd.ByteStride = want;
        sd.NumArgumentDescs = 1;
        sd.pArgumentDescs = &arg;
        sd.NodeMask = 0;
        ComPtr<ID3D12CommandSignature> created;
        if (FAILED(m_Device->GetDevice()->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&created)))) {
            HE_CORE_ERROR("D3D12 DrawIndexedIndirect: 创建命令签名失败");
            return;
        }
        sig = created.Get();
        m_CmdSigs[want] = created;
    }

    FlushRootArgs();
    m_List->ExecuteIndirect(sig, drawCount, b->GetResource(), offset, nullptr, 0);
    m_PushDirty = true;   // 同 Draw：常量槽已被本次绘制消费
}

void D3D12CommandList::DrawIndexedIndirectCount(IRHIBuffer* buffer, u64 offset,
                                                IRHIBuffer* countBuffer, u64 countOffset,
                                                u32 maxDrawCount, u32 stride) {
    if (!m_List || !buffer || !countBuffer) return;
    auto* b  = static_cast<D3D12Buffer*>(buffer);
    auto* cb = static_cast<D3D12Buffer*>(countBuffer);

    ComPtr<ID3D12CommandSignature> sig;
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC sd{};
    sd.ByteStride = stride ? stride : sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &arg;
    sd.NodeMask = 0;
    if (FAILED(m_Device->GetDevice()->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&sig)))) {
        HE_CORE_ERROR("D3D12 DrawIndexedIndirectCount: 创建命令签名失败");
        return;
    }
    // D3D12 的 ExecuteIndirect 原生支持「计数缓冲」，等价 Vulkan 的 *IndirectCount
    FlushRootArgs();
    m_List->ExecuteIndirect(sig.Get(), maxDrawCount, b->GetResource(), offset,
                            cb->GetResource(), countOffset);
    m_PushDirty = true;   // 同 Draw：常量槽已被本次绘制消费
}

void D3D12CommandList::Dispatch(u32 x, u32 y, u32 z) {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] Dispatch %d\n", s); std::fflush(stdout); } }
    if (!m_List) return;
    FlushRootArgs();
    m_List->Dispatch(x, y, z);
    m_PushDirty = true;   // 同 Draw：常量槽已被本次派发消费
}

void D3D12CommandList::DispatchIndirect(IRHIBuffer* buffer, u64 offset) {
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);
    FlushRootArgs();
    m_List->ExecuteIndirect(nullptr, 1, b->GetResource(), offset, nullptr, 0);
    m_PushDirty = true;   // 同 Draw：常量槽已被本次派发消费
}

void D3D12CommandList::PipelineBarrier(PipelineStage, PipelineStage,
                                       ResourceState, ResourceState) {
    // 无资源参数的全局屏障：D3D12 要求屏障绑定到具体资源，
    // 引擎里这种调用通常用于「pass 之间的全局同步」，D3D12 下由 UAV 屏障近似。
    if (!m_List) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.UAV.pResource = nullptr;   // nullptr = 全局 UAV 屏障（所有 UAV）
    m_List->ResourceBarrier(1, &b);
}

void D3D12CommandList::PipelineBarrier(PipelineStage, PipelineStage,
                                       ResourceState srcState, ResourceState dstState,
                                       IRHITexture* texture) {
    if (!m_List || !texture) return;
    auto* t = static_cast<D3D12Texture*>(texture);
    const D3D12_RESOURCE_STATES from = ToD3D12ResourceState(srcState, t->IsDepth());
    const D3D12_RESOURCE_STATES to   = ToD3D12ResourceState(dstState, t->IsDepth());
    if (from == to) return;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = t->GetResource();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        // 【用我们登记的真实状态做屏障】引擎传的 srcState 未必等于资源的实际状态（它与引擎自己的跟踪有关，D3D12 侧未必一致），直接照用会让 ResourceBarrier 被判非法（调试层 id=527，实测 3600 条），屏障无效。 这里以登记状态为准。
        ID3D12Resource* bres = t->GetResource();
        const D3D12_RESOURCE_STATES actualFrom =
            m_Device ? m_Device->GetResourceState(bres) : from;
        if (actualFrom == to) return;
        b.Transition.StateBefore = actualFrom;
    b.Transition.StateAfter  = to;
    m_List->ResourceBarrier(1, &b);
        if (m_Device) m_Device->SetResourceState(bres, to);
}

void D3D12CommandList::CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst, u64 size,
                                  u64 srcOffset, u64 dstOffset) {
    if (!m_List || !src || !dst) return;
    auto* s = static_cast<D3D12Buffer*>(src);
    auto* d = static_cast<D3D12Buffer*>(dst);
    m_List->CopyBufferRegion(d->GetResource(), dstOffset, s->GetResource(), srcOffset, size);
}

void D3D12CommandList::CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) {
    if (!m_List || !src || !dst) return;
    auto* s = static_cast<D3D12Texture*>(src);
    auto* d = static_cast<D3D12Texture*>(dst);
    m_List->CopyResource(d->GetResource(), s->GetResource());
}

void D3D12CommandList::SetTimelineSignal(RHIFenceHandle fence, u64 value) {
    if (m_Device) m_Device->SignalFenceOnQueue(QueueType::Graphics, fence, value);
}

void D3D12CommandList::SetTimelineWait(RHIFenceHandle fence, u64 value) {
    if (m_Device) m_Device->WaitFenceOnQueue(QueueType::Graphics, fence, value);
}

void D3D12CommandList::Submit() {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] Submit %d\n", s); std::fflush(stdout); } }
    if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] Submit\n"); std::fflush(stdout); }
    if (m_Device) m_Device->Submit(this, nullptr);
}

} // namespace he::rhi::d3d12

#endif // _WIN32
