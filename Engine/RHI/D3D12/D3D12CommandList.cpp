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

    if (!HE_D3D12_CHECK(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&m_Allocator)),
                        "CreateCommandAllocator")) {
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
    { static int s_n = 0; if (s_n < 3) { ++s_n; if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] Begin\n"); std::fflush(stdout); } } }

    if (!m_List || !m_Allocator) { std::printf("[BGN] no list/alloc\n"); std::fflush(stdout); return; }
    // 每次录制前必须 Reset 分配器与列表
    std::printf("[BGN] before alloc Reset\n"); std::fflush(stdout);
    if (FAILED(m_Allocator->Reset())) {
        HE_CORE_ERROR("D3D12CommandList::Begin: 分配器 Reset 失败（可能仍被 GPU 使用）");
        return;
    }
    std::printf("[BGN] before list Reset\n"); std::fflush(stdout);
    if (FAILED(m_List->Reset(m_Allocator.Get(), nullptr))) {
        HE_CORE_ERROR("D3D12CommandList::Begin: 列表 Reset 失败");
        return;
    }
    m_Recording = true;
    // 为整帧申请一块推送常量槽位，并清零
    m_PushGpu = 0;
    m_PushCpu = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE dummyCbv{};
    if (m_Device && m_Device->AllocateConstants(m_PushGpu, dummyCbv)) {
        m_PushCpu = m_Device->GetConstantSlotPtr(m_PushGpu);
        if (m_PushCpu) std::memset(m_PushCpu, 0, 256);
    }
}

void D3D12CommandList::End() {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] End %d\n", s); std::fflush(stdout); } }
    if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] End\n"); std::fflush(stdout); }
    { static int s_n = 0; if (s_n < 3) { ++s_n; if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] End\n"); std::fflush(stdout); } } }

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
    { static int s_n = 0; if (s_n < 3) { ++s_n; if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] BeginRenderPass\n"); std::fflush(stdout); } } }

    if (!m_List) return;

    // 后缓冲的 RTV：从交换链取（引擎传的 colorFormat 是后缓冲格式）
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    bool hasRtv = false;
    if (m_SwapChain) {
        // 引擎的 IRHISwapChain 只有 D3D12 交换链会走到这里；
        // 用 static_cast 而不是 dynamic_cast，避免依赖 RTTI。
        auto* d3dSc = static_cast<D3D12SwapChain*>(m_SwapChain);
        rtv = d3dSc->GetCurrentRtv();
        hasRtv = true;
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
    { static int s_n = 0; if (s_n < 3) { ++s_n; if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] EndRenderPass\n"); std::fflush(stdout); } } }

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
                const bool needs = (from == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
                                     from == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (needs) {
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
                const bool needs = (from == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
                                     from == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ||
                                     from == D3D12_RESOURCE_STATE_DEPTH_READ);
                if (needs) {
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
    m_List->OMSetRenderTargets(hasRtv ? 1u : 0u, hasRtv ? &rtv : nullptr, FALSE, hasDsv ? &dsv : nullptr);
    std::printf("[BOPC] after OMSetRT\n"); std::fflush(stdout);
    m_HasRtv = hasRtv;

    if (hasRtv && clear) {
        m_List->ClearRenderTargetView(rtv, clear->color, 0, nullptr);
        std::printf("[BOPD] after ClearRTV\n"); std::fflush(stdout);
    }
    if (hasDsv) {
        m_List->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
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

    m_List->OMSetRenderTargets(n, rtvs, FALSE, hasDsv ? &dsv : nullptr);

    if (clears) {
        for (u32 i = 0; i < n; ++i) {
            if (colorImageViews[i]) m_List->ClearRenderTargetView(rtvs[i], clears[i].color, 0, nullptr);
        }
    }
    if (hasDsv) {
        m_List->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
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
    { static int s_n = 0; if (s_n < 3) { ++s_n; if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] SetPipeline\n"); std::fflush(stdout); } } }

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
}

void D3D12CommandList::SetVertexBuffer(IRHIBuffer* buffer, u32 binding) {
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

    ID3D12DescriptorHeap* heaps[2] = { m_Device->GetCbvSrvUavHeap(), m_Device->GetSetSamplerHeap() };
    if (!heaps[0] || !heaps[1]) return;
    m_List->SetDescriptorHeaps(2, heaps);
    std::printf("[BS] heaps ok\n"); std::fflush(stdout);

    const u32 baseSlot = m_Device->GetSetBaseSlot(set);
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
    m_List->SetGraphicsRootDescriptorTable(8  + setIndex, srv);
    std::printf("[BS] g-srv ok\n"); std::fflush(stdout);
    m_List->SetGraphicsRootDescriptorTable(16 + setIndex, uav);
    std::printf("[BS] g-uav ok\n"); std::fflush(stdout);
    m_List->SetGraphicsRootDescriptorTable(24 + setIndex, smp);
    std::printf("[BS] g-smp ok\n"); std::fflush(stdout);
    // 【只有计算管线才绑计算表】图形管线没有绑定过 SetComputeRootSignature，
    // 此时调用 SetComputeRootDescriptorTable 会让 D3D12 静默卡死
    //（实测 02.Cube 第一帧阴影 pass 绑定描述符集时死在这里）。
    if (m_IsComputePipeline) {
        m_List->SetComputeRootDescriptorTable(8  + setIndex, srv);
        m_List->SetComputeRootDescriptorTable(16 + setIndex, uav);
        m_List->SetComputeRootDescriptorTable(24 + setIndex, smp);
    }
    std::printf("[BS] tables ok\n"); std::fflush(stdout);
}

void D3D12CommandList::SetPushConstants(u32 offset, u32 size, const void* data) {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] PushConst %d\n", s); std::fflush(stdout); } }
    if (!m_List || !m_Device || !data || size == 0) return;

    // 常量：从设备的常量缓冲环取一块 256B，写入数据，然后用
    // SetGraphicsRootConstantBufferView 直接绑到 b0（根签名参数 0）。
    // 这样不依赖描述符堆，也不受 cbuffer 寄存器编号影响。
    // 复用 Begin() 里申请的整帧槽位：多次调用按 offset 累加（Vulkan 语义）
    if (m_PushGpu == 0 || !m_PushCpu) return;
    if (offset >= 256u) return;
    const u32 copySize = ((offset + size) > 256u) ? (256u - offset) : size;
    std::memcpy(static_cast<unsigned char*>(m_PushCpu) + offset, data, copySize);

    // 参数 0 = b0 的 root CBV；其余 b1..b7 目前也指向同一块（引擎的 push constant
    // 语义里只有一个块，着色器的其他 cbuffer 由 UpdateDescriptorSet 提供的常量缓冲填充）
    if (m_IsComputePipeline) {
        // 计算管线的根签名是独立的一套，绑定计算侧时必须走 Compute 版本；
        // 图形管线下调用 Compute 版本的任何 Set*Root* 都会让 D3D12 静默卡死。
        m_List->SetComputeRootConstantBufferView(0, m_PushGpu);
    } else {
        m_List->SetGraphicsRootConstantBufferView(0, m_PushGpu);
    }
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
    m_List->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void D3D12CommandList::DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                                   i32 vertexOffset, u32 firstInstance) {
    if (std::getenv("HE_D3D12_NO_DRAW")) return;   // 实验开关：跳过实际绘制
    if (!m_List) return;
    m_List->DrawIndexedInstanced(indexCount, instanceCount, firstIndex,
                                 static_cast<INT>(vertexOffset), firstInstance);
}

void D3D12CommandList::DrawIndexedIndirect(IRHIBuffer* buffer, u64 offset, u32 drawCount, u32 stride) {
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);
    // D3D12 用 ExecuteIndirect 表达间接绘制：需要一个「参数缓冲 + MaxCommandCount」
    // 以及一个已保存的签名（阶段 3 创建）。这里先用 D3D12 内置的签名。
    ComPtr<ID3D12CommandSignature> sig;
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC sd{};
    sd.ByteStride = stride ? stride : sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &arg;
    sd.NodeMask = 0;
    if (FAILED(m_Device->GetDevice()->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&sig)))) {
        HE_CORE_ERROR("D3D12 DrawIndexedIndirect: 创建命令签名失败");
        return;
    }
    m_List->ExecuteIndirect(sig.Get(), drawCount, b->GetResource(), offset, nullptr, 0);
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
    m_List->ExecuteIndirect(sig.Get(), maxDrawCount, b->GetResource(), offset,
                            cb->GetResource(), countOffset);
}

void D3D12CommandList::Dispatch(u32 x, u32 y, u32 z) {
    { static int s=0; if (s<4) { ++s; std::printf("[STP] Dispatch %d\n", s); std::fflush(stdout); } }
    if (!m_List) return;
    m_List->Dispatch(x, y, z);
}

void D3D12CommandList::DispatchIndirect(IRHIBuffer* buffer, u64 offset) {
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);
    m_List->ExecuteIndirect(nullptr, 1, b->GetResource(), offset, nullptr, 0);
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
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    m_List->ResourceBarrier(1, &b);
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
