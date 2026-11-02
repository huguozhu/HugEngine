// ============================================================================
//  D3D11CommandList.cpp —— D3D11 命令列表（说明见头文件）
// ============================================================================
#include "D3D11CommandList.h"
#include "D3D11Converters.h"
#include "D3D11Device.h"
#include "D3D11Resources.h"
#include "D3D11SwapChain.h"
#include "RHI/DxbcBindTable.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace he::rhi::d3d11 {

using dxbc::DxbcBindType;

D3D11CommandList::D3D11CommandList(D3D11Device* device)
    : m_Device(device), m_State(&device->GetImmediateState()) {}

D3D11CommandList::D3D11CommandList(D3D11Device* device, ComPtr<ID3D11DeviceContext4> deferred)
    : m_Device(device), m_IsSecondary(true), m_RecordDeferred(true), m_Deferred(std::move(deferred)),
      m_OwnState(std::make_unique<D3D11ContextState>()) {
    device->InitContextState(*m_OwnState, m_Deferred.Get());
    m_State = m_OwnState.get();
}

bool D3D11CommandList::EnsureDeferredContext() {
    if (m_Deferred) return true;
    ComPtr<ID3D11DeviceContext3> ctx3;
    if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateDeferredContext3(0, &ctx3), "CreateDeferredContext3")) return false;
    if (FAILED(ctx3.As(&m_Deferred))) return false;
    m_OwnState = std::make_unique<D3D11ContextState>();
    m_Device->InitContextState(*m_OwnState, m_Deferred.Get());
    return true;
}

D3D11CommandList::~D3D11CommandList() {
    // 上下文所有者只做指针比较：本列表销毁后若有新列表恰好分配到同一地址，会误以为状态仍在上下文里
    if (m_State && m_State->owner == this) m_State->owner = nullptr;
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------
void D3D11CommandList::Begin() {
    // 每次录制从干净的绑定开始（与 Vulkan 新开命令缓冲时没有任何绑定一致）
    m_Pipeline = nullptr;
    for (auto& s : m_Sets) s = kInvalidSet;
    for (auto& vb : m_VertexBuffers) vb = nullptr;
    m_IndexBuffer = nullptr;
    m_IndexOffset = 0;
    for (auto& r : m_Rtvs) r = nullptr;
    m_RtvCount = 0;
    m_DepthTexture = nullptr;
    m_DepthReadOnly = false;
    m_Viewport = {};
    m_Scissor  = {};
    m_LabelDepth = 0;
    std::memset(m_PushData, 0, sizeof(m_PushData));
    m_PushDirty = true;
    m_PushGeneration = 0;
    m_PendingSignals.clear();
    m_NonBindless.clear();
    // 【模式 2（M7.4）】装了提交路由时，主列表改录到自己的 deferred context：RHI 线程稍后在
    // immediate context 上执行它，渲染线程与 RHI 线程不会同时往 immediate context 里录命令
    if (!m_IsSecondary) {
        m_RecordDeferred = (m_Device->GetSubmitRouter() != nullptr) && EnsureDeferredContext();
        m_State = m_RecordDeferred ? m_OwnState.get() : &m_Device->GetImmediateState();
        if (m_RecordDeferred) {
            m_State->ResetBindings();          // 上一轮 FinishCommandList 后 deferred context 已回到默认状态
            m_State->pushNeedsDiscard = true;  // deferred context 每一轮录制的第一次 Map 必须是 DISCARD
        }
    }
    // 让下一次改动上下文时重新应用（即使上下文的上一个使用者就是本列表的上一轮录制）
    if (m_State->owner == this) m_State->owner = nullptr;
}

void D3D11CommandList::End() {
    // 未配对的调试分组补齐（与 D3D12 一致）
    while (m_LabelDepth > 0) EndDebugLabel();
    // 二级列表：把 deferred context 里录下的命令封成命令列表，等主列表 ExecuteSecondary。
    // FALSE：不保存 deferred context 的状态，下一轮录制从默认状态开始（BeginSecondary 会重新应用）
    if (m_IsSecondary) {
        m_Recorded.Reset();
        HE_D3D11_CHECK(m_Deferred->FinishCommandList(FALSE, &m_Recorded), "FinishCommandList");
    } else if (m_RecordDeferred) {
        // 模式 2 的主列表：连同本帧的时间线信号一起排队，Submit（RHI 线程）时执行
        RecordedFrame frame;
        if (HE_D3D11_CHECK(m_Deferred->FinishCommandList(FALSE, &frame.list), "FinishCommandList")) {
            frame.signals = std::move(m_PendingSignals);
            m_PendingSignals.clear();
            std::lock_guard<std::mutex> lock(m_ReadyMutex);
            m_Ready.push_back(std::move(frame));
        }
    }
}

// ---------------------------------------------------------------------------
//  二级列表（M7.3）
// ---------------------------------------------------------------------------
void D3D11CommandList::BeginSecondary(IRHIPipelineState* pso) {
    Begin();                       // 本列表记下的绑定全部清空（与 Vulkan 新开次级命令缓冲一致）
    m_Recorded.Reset();
    m_State->ResetBindings();      // deferred context 在上一轮 FinishCommandList 后已回到默认状态
    m_State->pushNeedsDiscard = true;   // deferred context 每一轮录制的第一次 Map 必须是 DISCARD
    // 继承主列表当前通道的渲染目标与视口（Vulkan 由继承信息里的 render pass 决定，D3D11 要显式设）
    if (const D3D11CommandList* primary = m_Device->GetActivePassList()) {
        m_RtvCount = primary->m_RtvCount;
        for (u32 i = 0; i < kMaxColorAttachments; ++i) m_Rtvs[i] = primary->m_Rtvs[i];
        m_DepthTexture = primary->m_DepthTexture;
        m_DepthMip     = primary->m_DepthMip;
        m_DepthLayer   = primary->m_DepthLayer;
        m_Viewport     = primary->m_Viewport;
        m_Scissor      = primary->m_Scissor;
    }
    SetPipeline(pso);              // 第一次改动上下文时把上面的状态整体应用进 deferred context
}

void D3D11CommandList::ExecuteSecondary(IRHICommandList* secondary) {
    auto* sec = static_cast<D3D11CommandList*>(secondary);
    if (!sec || !sec->m_Recorded) return;
    m_Device->FlushOpenMaps();   // 二级列表录制期间没有上传持久映射缓冲的改动（见 PrepareDrawOrDispatch）
    // FALSE：执行后上下文回到默认状态（不恢复执行前的状态，省一次保存 / 恢复）
    m_State->context->ExecuteCommandList(sec->m_Recorded.Get(), FALSE);
    sec->m_Recorded.Reset();
    // 上下文已被清空：跟踪表与所有者作废，本列表下次使用时重新应用渲染目标等全部状态
    m_State->ResetBindings();
}

// ---------------------------------------------------------------------------
//  上下文所有权与状态应用
// ---------------------------------------------------------------------------
void D3D11CommandList::EnsureOwner() {
    if (m_State->owner == this) return;
    m_State->owner = this;
    ApplyPipeline();
    ApplyRenderTargets();
    ApplyInputAssembler();
    ApplyViewportScissor();
    // 描述符集的资源在每次绘制 / 派发前都会重新下发，不需要在这里处理
}

void D3D11CommandList::ApplyPipeline() {
    if (!m_Pipeline || !m_Pipeline->IsValid()) return;
    ID3D11DeviceContext4* ctx = m_State->context;
    if (m_Pipeline->IsCompute()) {
        ctx->CSSetShader(m_Pipeline->GetCS(), nullptr, 0);
        return;
    }
    ctx->VSSetShader(m_Pipeline->GetVS(), nullptr, 0);
    ctx->PSSetShader(m_Pipeline->GetPS(), nullptr, 0);
    ctx->IASetInputLayout(m_Pipeline->GetInputLayout());
    ctx->IASetPrimitiveTopology(m_Pipeline->GetTopology());
    ctx->RSSetState(m_Pipeline->GetRasterizerState());
    ctx->OMSetDepthStencilState(m_Pipeline->GetDepthStencilState(), 0);
    const float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ctx->OMSetBlendState(m_Pipeline->GetBlendState(), blendFactor, 0xFFFFFFFFu);
}

void D3D11CommandList::ApplyRenderTargets() {
    // 不写深度的 PSO 配只读 DSV：同一张深度图可以同时被采样（规划 1.3）
    const bool readOnly = m_Pipeline && !m_Pipeline->IsCompute() && !m_Pipeline->GetDepthWrite();
    ID3D11DepthStencilView* dsv = m_DepthTexture ? m_DepthTexture->GetDsv(m_DepthMip, m_DepthLayer, readOnly) : nullptr;
    m_DepthReadOnly = readOnly;
    // 冲突解绑（规划 3.7）：要作渲染目标 / 可写深度的资源若仍以 SRV 绑着（上一个 pass 采样过它），
    // 先清掉那些 SRV 槽，否则运行时会强制置空并告警。只读 DSV 与同一深度图的 SRV 可以共存
    ID3D11Resource* outputs[kMaxColorAttachments + 1] = {};
    u32 outputCount = 0;
    for (u32 i = 0; i < m_RtvCount; ++i) {
        if (!m_Rtvs[i]) continue;
        m_Rtvs[i]->GetResource(&outputs[outputCount]);
        if (outputs[outputCount]) outputs[outputCount++]->Release();   // 只取指针作比较
    }
    if (m_DepthTexture && !readOnly) outputs[outputCount++] = m_DepthTexture->GetResource();
    if (outputCount) m_State->UnbindSrvsOf(outputs, outputCount);
    m_State->context->OMSetRenderTargets(m_RtvCount, m_RtvCount ? m_Rtvs : nullptr, dsv);
}

void D3D11CommandList::ApplyInputAssembler() {
    ID3D11DeviceContext4* ctx = m_State->context;
    ID3D11Buffer* buffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    UINT offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    UINT count = 0;
    for (u32 i = 0; i < kMaxVertexSlots; ++i) {
        auto* vb = static_cast<D3D11Buffer*>(m_VertexBuffers[i]);
        if (!vb) continue;
        buffers[i] = vb->GetBuffer();
        // 步长以 PSO 的顶点布局为准（引擎的 vertexLayout.stride），未给时用缓冲创建时的步长
        const u32 psoStride = (i == 0 && m_Pipeline) ? m_Pipeline->GetVertexStride() : 0u;
        strides[i] = psoStride ? psoStride : vb->GetStride();
        count = i + 1;
    }
    if (m_Pipeline && !m_Pipeline->IsCompute()) {
        if (m_Pipeline->NeedsInstanceIndexBuffer()) {
            const u32 s = D3D11PipelineState::kInstanceIndexSlot;
            buffers[s] = m_Device->GetInstanceIndexBuffer();
            strides[s] = 4;
            count = std::max<UINT>(count, s + 1);
        }
        if (m_Pipeline->NeedsZeroVertexBuffer()) {
            const u32 s = D3D11PipelineState::kZeroVertexSlot;
            buffers[s] = m_Device->GetZeroVertexBuffer();
            strides[s] = 0;   // 步长 0：每个顶点都读同一段全零数据
            count = std::max<UINT>(count, s + 1);
        }
    }
    if (count) ctx->IASetVertexBuffers(0, count, buffers, strides, offsets);
    auto* ib = static_cast<D3D11Buffer*>(m_IndexBuffer);
    // 引擎的索引缓冲都是 32 位（与 D3D12 后端一致）
    ctx->IASetIndexBuffer(ib ? ib->GetBuffer() : nullptr, DXGI_FORMAT_R32_UINT, m_IndexOffset);
}

void D3D11CommandList::ApplyViewportScissor() {
    ID3D11DeviceContext4* ctx = m_State->context;
    if (m_Viewport.Width > 0.0f && m_Viewport.Height > 0.0f) ctx->RSSetViewports(1, &m_Viewport);
    if (m_Scissor.right > m_Scissor.left && m_Scissor.bottom > m_Scissor.top) ctx->RSSetScissorRects(1, &m_Scissor);
}

// ---------------------------------------------------------------------------
//  Render pass
// ---------------------------------------------------------------------------
ID3D11RenderTargetView* D3D11CommandList::ResolveRtv(void* view) const {
    if (!view) return nullptr;
    const D3D11ViewInfo* vi = m_Device->FindView(view);
    if (!vi) return nullptr;
    return vi->texture->GetRtv(vi->mip, vi->layer);
}

void D3D11CommandList::BeginPassTargets(ID3D11RenderTargetView* const* rtvs, u32 rtvCount, void* depthView,
                                        u32 width, u32 height, bool clearColor, const ClearValue* colorClears,
                                        bool clearDepth, float depth, u8 stencil) {
    EnsureOwner();
    ID3D11DeviceContext4* ctx = m_State->context;

    m_RtvCount = std::min<u32>(rtvCount, kMaxColorAttachments);
    for (u32 i = 0; i < kMaxColorAttachments; ++i) m_Rtvs[i] = (i < m_RtvCount) ? rtvs[i] : nullptr;
    m_DepthTexture = nullptr;
    if (const D3D11ViewInfo* vi = depthView ? m_Device->FindView(depthView) : nullptr) {
        if (vi->texture->IsDepth()) {
            m_DepthTexture = vi->texture;
            m_DepthMip     = vi->mip;
            m_DepthLayer   = vi->layer;
        }
    }
    ApplyRenderTargets();
    if (!m_IsSecondary) m_Device->SetActivePassList(this);   // 二级列表从它继承渲染目标（M7.3）

    if (clearColor) {
        for (u32 i = 0; i < m_RtvCount; ++i) {
            // 不传 clear：用三后端统一的默认清除色（见 RHI/CommandList.h 的 kDefaultClearValue）
            if (m_Rtvs[i]) ctx->ClearRenderTargetView(m_Rtvs[i], colorClears ? colorClears[i].color : kDefaultClearValue.color);
        }
    }
    if (clearDepth && m_DepthTexture) {
        // 清除总是用可写 DSV；带模板分量的格式才清模板（只传 DEPTH 对纯深度格式也合法）
        UINT flags = D3D11_CLEAR_DEPTH;
        if (HasStencil(m_DepthTexture->GetFormat())) flags |= D3D11_CLEAR_STENCIL;
        ctx->ClearDepthStencilView(m_DepthTexture->GetDsv(m_DepthMip, m_DepthLayer, false), flags, depth, stencil);
    }

    // 视口与裁剪矩形设为整个目标（光栅状态开了 ScissorEnable，不设裁剪矩形就什么都画不出来）
    m_Viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    m_Scissor  = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    ApplyViewportScissor();
}

void D3D11CommandList::BeginRenderPass(u32 colorCount, Format, Format, const ClearValue* clear, LoadOp loadOp) {
    // 主 render pass 的目标是交换链的当前后缓冲
    auto* sc = static_cast<D3D11SwapChain*>(m_SwapChain);
    ID3D11RenderTargetView* rtv = sc ? sc->GetRtv() : nullptr;
    const u32 w = sc ? sc->GetWidth() : 0u, h = sc ? sc->GetHeight() : 0u;
    // 主通道的清屏值：不传 clear 时用三后端统一的默认清除色（与 Vulkan / D3D12 一致）
    ClearValue c = clear ? *clear : kDefaultClearValue;
    BeginPassTargets(&rtv, (rtv && colorCount) ? 1u : 0u, sc ? sc->GetDepthBufferView() : nullptr, w, h,
                     loadOp == LoadOp::Clear, &c, false, 1.0f, 0);
}

void D3D11CommandList::EndRenderPass() {
    EndOffscreenPass();
}

void D3D11CommandList::BeginOffscreenPass(void* colorImageView, void* depthImageView, u32 width, u32 height,
                                          const ClearValue* clear, bool) {
    // 是否清空由当前 PSO 的 loadOp 决定（与 Vulkan 的 render pass 一致）；clear 只提供清屏值
    const bool gfxPso = m_Pipeline && m_Pipeline->IsValid() && !m_Pipeline->IsCompute();
    const bool clearColor = gfxPso ? (m_Pipeline->GetColorLoadOp() == LoadOp::Clear) : (clear != nullptr);
    const bool clearDepth = gfxPso ? (m_Pipeline->GetDepthLoadOp() == LoadOp::Clear) : (clear != nullptr);
    ID3D11RenderTargetView* rtv = ResolveRtv(colorImageView);
    BeginPassTargets(&rtv, rtv ? 1u : 0u, depthImageView, width, height,
                     clearColor, clear, clearDepth, clear ? clear->depth : 1.0f, clear ? clear->stencil : 0);
}

void D3D11CommandList::BeginOffscreenPassMRT(void* const* colorImageViews, u32 colorCount, void* depthImageView,
                                             u32 width, u32 height, const ClearValue* clears, bool) {
    const bool gfxPso = m_Pipeline && m_Pipeline->IsValid() && !m_Pipeline->IsCompute();
    const bool clearColor = gfxPso ? (m_Pipeline->GetColorLoadOp() == LoadOp::Clear) : (clears != nullptr);
    const bool clearDepth = gfxPso ? (m_Pipeline->GetDepthLoadOp() == LoadOp::Clear) : (clears != nullptr);
    ID3D11RenderTargetView* rtvs[kMaxColorAttachments] = {};
    const u32 n = std::min<u32>(colorCount, kMaxColorAttachments);
    for (u32 i = 0; i < n; ++i) rtvs[i] = ResolveRtv(colorImageViews ? colorImageViews[i] : nullptr);
    // clears 的长度契约：colorCount 个颜色项 + 末尾一个深度项（见 CommandList.h）
    BeginPassTargets(rtvs, n, depthImageView, width, height, clearColor, clears,
                     clearDepth, clears ? clears[colorCount].depth : 1.0f, clears ? clears[colorCount].stencil : 0);
}

void D3D11CommandList::EndOffscreenPass() {
    // 解绑渲染目标：下一步常把它当 SRV 采样，仍绑在输出上时运行时会把 SRV 强制置空（读到全黑）
    EnsureOwner();
    for (auto& r : m_Rtvs) r = nullptr;
    m_RtvCount = 0;
    m_DepthTexture = nullptr;
    m_State->context->OMSetRenderTargets(0, nullptr, nullptr);
    if (m_Device->GetActivePassList() == this) m_Device->SetActivePassList(nullptr);
}

// ---------------------------------------------------------------------------
//  管线与资源绑定
// ---------------------------------------------------------------------------
void D3D11CommandList::SetPipeline(IRHIPipelineState* pso) {
    EnsureOwner();
    m_Pipeline = static_cast<D3D11PipelineState*>(pso);
    ApplyPipeline();
}

void D3D11CommandList::SetVertexBuffer(IRHIBuffer* buffer, u32 binding) {
    if (binding < kMaxVertexSlots) m_VertexBuffers[binding] = buffer;   // 步长依赖 PSO，绘制前统一下发
}

void D3D11CommandList::SetIndexBuffer(IRHIBuffer* buffer, u32 offset) {
    m_IndexBuffer = buffer;
    m_IndexOffset = offset;
}

void D3D11CommandList::SetViewport(const Viewport& vp) {
    EnsureOwner();
    m_Viewport = { vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth };
    // 引擎传负高度表示「原点在左下」（Vulkan 翻转 Y 的写法），换算成 D3D 的原点左上（与 D3D12 一致）
    if (vp.height < 0.0f) {
        m_Viewport.TopLeftY = vp.y + vp.height;
        m_Viewport.Height   = -vp.height;
    }
    ApplyViewportScissor();
}

void D3D11CommandList::SetScissor(const ScissorRect& sc) {
    EnsureOwner();
    m_Scissor = { sc.x, sc.y, sc.x + static_cast<LONG>(sc.width), sc.y + static_cast<LONG>(sc.height) };
    ApplyViewportScissor();
}

void D3D11CommandList::SetFullscreenViewport(u32 width, u32 height) {
    Viewport vp{};
    vp.width  = static_cast<float>(width);
    vp.height = static_cast<float>(height);
    SetViewport(vp);
    ScissorRect sc{};
    sc.width  = width;
    sc.height = height;
    SetScissor(sc);
}

void D3D11CommandList::BindDescriptorSet(u32 setIndex, DescriptorSetHandle set) {
    if (setIndex < kMaxSets) m_Sets[setIndex] = set;
}

void D3D11CommandList::SetNonBindlessHandle(u32 binding, u32 handle) {
    for (auto& [b, h] : m_NonBindless) {
        if (b == binding) { h = handle; return; }
    }
    m_NonBindless.emplace_back(binding, handle);
}

bool D3D11CommandList::ResolveNonBindless(u32 binding, u32 element, ID3D11ShaderResourceView*& srv,
                                          ID3D11SamplerState*& smp, D3D11Buffer*& buf) const {
    auto find = [&](u32 b, u32& out) {
        for (const auto& [k, h] : m_NonBindless) if (k == b) { out = h; return true; }
        return false;
    };
    auto* heap = static_cast<D3D11BindlessHeap*>(m_Device->GetBindlessHeap());
    // 纹理数组桶（M6）：与本次绘制无关，由堆直接提供；先把新登记的纹理拷进桶
    if (binding >= kBindingNBTexBuckets && binding < kBindingNBTexBuckets + kBindingNBTexBucketCount) {
        heap->EnsureBuckets();
        srv = heap->GetBucketSrv(binding - kBindingNBTexBuckets);
        return true;
    }
    if (binding == kBindingNBTexBucketMap) {
        heap->EnsureBuckets();
        srv = heap->GetBucketMapSrv();
        return true;
    }
    if (binding == kBindingNBTexBucketSampler) {
        heap->EnsureBuckets();
        smp = heap->GetBucketSampler();
        return true;
    }
    u32 handle = 0;
    // 材质纹理 / 采样器：都以「材质纹理基址」为准，第 k 个取 基址 + k
    if (binding >= kBindingNBMaterialTextures && binding < kBindingNBMaterialTextures + kBindingNBMaterialSlots) {
        if (find(kBindingNBMaterialTextures, handle)) {
            srv = heap->GetTextureSrv(handle + (binding - kBindingNBMaterialTextures));
        }
        return true;
    }
    if (binding >= kBindingNBMaterialSamplers && binding < kBindingNBMaterialSamplers + kBindingNBMaterialSlots) {
        if (find(kBindingNBMaterialTextures, handle)) {
            smp = heap->GetTextureSampler(handle + (binding - kBindingNBMaterialSamplers));
        }
        return true;
    }
    // 其余替代绑定是缓冲：句柄即 bindless 堆里的缓冲句柄（数组元素按句柄依次往后取）；
    // kNonBindlessNone 表示本次不用，绑空
    if (find(binding, handle)) {
        if (handle != kNonBindlessNone) {
            buf = const_cast<D3D11CommandList*>(this)->LiveBuffer(heap->GetBuffer(handle + element), 0, binding);
        }
        return true;
    }
    return false;
}

void D3D11CommandList::SetPushConstants(u32 offset, u32 size, const void* data) {
    if (!data || offset >= sizeof(m_PushData)) return;
    const u32 n = std::min<u32>(size, static_cast<u32>(sizeof(m_PushData)) - offset);
    std::memcpy(m_PushData + offset, data, n);
    m_PushDirty = true;
}

void D3D11CommandList::BindPushConstants() {
    if (!m_Pipeline->UsesPushConstants()) return;
    // 数据改过，或常量环转了一圈（旧槽位已作废）：写进新槽
    if (m_PushDirty || m_PushGeneration != m_State->pushGeneration) {
        if (!m_Device->AllocatePushConstants(*m_State, m_PushData, m_PushFirstConstant, m_PushGeneration)) return;
        m_PushDirty = false;
    }
    ID3D11DeviceContext4* ctx = m_State->context;
    ID3D11Buffer* ring = m_State->pushRing.Get();
    const UINT first = m_PushFirstConstant;
    const UINT count = D3D11Device::kPushSlotSize / 16u;
    // 同一缓冲只换偏移重新绑定时，部分运行时会把它当冗余调用过滤掉：先绑空再绑（规划 3.5）
    ID3D11Buffer* none = nullptr;
    const u32 slot = dxbc::kDxbcPushConstantSlot;
    if (m_Pipeline->IsCompute()) {
        ctx->CSSetConstantBuffers(slot, 1, &none);
        ctx->CSSetConstantBuffers1(slot, 1, &ring, &first, &count);
    } else {
        ctx->VSSetConstantBuffers(slot, 1, &none);
        ctx->VSSetConstantBuffers1(slot, 1, &ring, &first, &count);
        ctx->PSSetConstantBuffers(slot, 1, &none);
        ctx->PSSetConstantBuffers1(slot, 1, &ring, &first, &count);
    }
}

void D3D11CommandList::SetTimelineSignal(RHIFenceHandle fence, u64 value) {
    m_PendingSignals.emplace_back(fence, value);
}

void D3D11CommandList::Submit() {
    ID3D11DeviceContext4* immediate = m_Device->GetContext();
    // 模式 2：执行最早录好的一帧，再发它的时间线信号（信号排在命令之后）
    RecordedFrame frame;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(m_ReadyMutex);
        if (!m_Ready.empty()) {
            frame = std::move(m_Ready.front());
            m_Ready.pop_front();
            queued = true;
        }
    }
    if (queued) {
        immediate->ExecuteCommandList(frame.list.Get(), FALSE);
        m_Device->GetImmediateState().ResetBindings();   // 执行后 immediate context 回到默认状态
        for (const auto& [fence, value] : frame.signals) {
            if (ID3D11Fence* f = m_Device->FindFence(fence)) immediate->Signal(f, value);
        }
        return;
    }
    // 录在 immediate context 上的列表：命令早已发出，只发信号
    for (const auto& [fence, value] : m_PendingSignals) {
        if (ID3D11Fence* f = m_Device->FindFence(fence)) immediate->Signal(f, value);
    }
    m_PendingSignals.clear();
}

// ---------------------------------------------------------------------------
//  查询（M3.7）
// ---------------------------------------------------------------------------
void D3D11CommandList::WriteTimestamp(IRHIQueryPool* pool, u32 queryIndex) {
    if (pool) static_cast<D3D11QueryPool*>(pool)->WriteTimestamp(m_State->context, queryIndex);
}

void D3D11CommandList::ResetQueryPool(IRHIQueryPool* pool) {
    if (pool) static_cast<D3D11QueryPool*>(pool)->Reset();
}

void D3D11CommandList::GetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) {
    if (pool) static_cast<D3D11QueryPool*>(pool)->Read(first, count, data, true);
}

bool D3D11CommandList::TryGetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) {
    return pool && static_cast<D3D11QueryPool*>(pool)->Read(first, count, data, false);
}

void D3D11CommandList::BeginQuery(IRHIQueryPool* pool, u32 queryIndex) {
    if (pool) static_cast<D3D11QueryPool*>(pool)->Begin(m_State->context, queryIndex);
}

void D3D11CommandList::EndQuery(IRHIQueryPool* pool, u32 queryIndex) {
    if (pool) static_cast<D3D11QueryPool*>(pool)->End(m_State->context, queryIndex);
}

ID3D11DeviceContext4* D3D11CommandList::PrepareExternalRendering() {
    EnsureOwner();
    return m_State->context;
}

void D3D11CommandList::ReportStrideConflict(D3D11Buffer* buffer, const D3D11PipelineState::Binding& b) {
    if (b.rawBuffer || buffer->GetStructStride() == 0 || buffer->GetStructStride() == b.stride) return;
    const std::string key = std::string("stride:") + m_Pipeline->GetName() + "#" + std::to_string(b.set) + "/" +
                            std::to_string(b.binding);
    if (std::find(m_ReportedDangling.begin(), m_ReportedDangling.end(), key) != m_ReportedDangling.end()) return;
    m_ReportedDangling.push_back(key);
    HE_CORE_ERROR("[D3D11] PSO {} 的 (set {}, binding {}) 要以步长 {} 读缓冲 {}，而它已按步长 {} 建成结构化缓冲"
                  "（同一缓冲被不同元素类型的绑定共用），该槽绑空",
                  m_Pipeline->GetName(), b.set, b.binding, b.stride, static_cast<const void*>(buffer), buffer->GetStructStride());
}

D3D11Buffer* D3D11CommandList::LiveBuffer(IRHIBuffer* buffer, u32 set, u32 binding) {
    if (!buffer) return nullptr;
    if (m_Device->IsLiveBuffer(buffer)) return static_cast<D3D11Buffer*>(buffer);
    const std::string key = std::string(m_Pipeline ? m_Pipeline->GetName() : "?") + "#" + std::to_string(set) +
                            "/" + std::to_string(binding);
    if (std::find(m_ReportedDangling.begin(), m_ReportedDangling.end(), key) == m_ReportedDangling.end()) {
        m_ReportedDangling.push_back(key);
        HE_CORE_ERROR("[D3D11] PSO {} 的 (set {}, binding {}) 指向已销毁的缓冲 {}（描述符集 / bindless 句柄没有随缓冲重建更新），按空资源绑定",
                      m_Pipeline ? m_Pipeline->GetName() : "?", set, binding, static_cast<const void*>(buffer));
    }
    return nullptr;
}

void D3D11CommandList::ReportMissing(u32 set, u32 binding, const char* what) {
    std::string key = std::string(m_Pipeline->GetName()) + "#" + std::to_string(set) + "." + std::to_string(binding);
    if (key == m_LastMissing) return;
    static u32 s_Reported = 0;
    if (++s_Reported > 50) return;   // 全局上限：缺资源通常是成批出现的
    m_LastMissing = std::move(key);
    HE_CORE_WARN("[D3D11] PSO {} 的 (set {}, binding {}) 没有可绑定的{}，按空资源绑定",
                 m_Pipeline->GetName(), set, binding, what);
}

void D3D11CommandList::FlushBindings() {
    ID3D11DeviceContext4* ctx = m_State->context;
    const bool compute = m_Pipeline->IsCompute();

    // 图形管线里所有阶段的 UAV 要和渲染目标一起设置（OMSetRenderTargetsAndUnorderedAccessViews）
    ID3D11UnorderedAccessView* graphicsUavs[dxbc::kDxbcMaxUAVSlots] = {};
    u32 graphicsUavMin = dxbc::kDxbcMaxUAVSlots, graphicsUavMax = 0;
    // 本次已绑为 UAV 的资源：D3D11 不允许同一资源同时占两个 UAV 槽（运行时会把先绑的那个置空），
    // 而 Vulkan 允许别名（如 Nanite 不开流式时，反馈缓冲槽位借用读数缓冲占位）。保留先出现的槽，后面的置空
    std::vector<ID3D11Resource*> uavResources;

    const D3D11PipelineState::Stage graphicsStages[] = { D3D11PipelineState::VS, D3D11PipelineState::PS };
    const u32 stageCount = compute ? 1u : 2u;
    for (u32 si = 0; si < stageCount; ++si) {
        const auto stage = compute ? D3D11PipelineState::CS : graphicsStages[si];
        for (const auto& b : m_Pipeline->GetBindings(stage)) {
            const auto type = static_cast<DxbcBindType>(b.type);
            if (type == DxbcBindType::PushConstant) continue;   // 常量环（M2.7）

            const D3D11Device::DescriptorSet* set = (b.set < kMaxSets) ? m_Device->FindDescriptorSet(m_Sets[b.set]) : nullptr;
            const D3D11Device::DescriptorEntry* e = nullptr;
            if (set) {
                auto it = set->entries.find(b.binding);
                if (it != set->entries.end()) e = &it->second;
            }

            switch (type) {
                case DxbcBindType::SRV: {
                    std::vector<ID3D11ShaderResourceView*> views(b.count, nullptr);
                    bool byHandle = false;   // 由 bindless 替代句柄决定（含有意不绑的 kNonBindlessNone）
                    for (u32 i = 0; i < b.count; ++i) {
                        ID3D11ShaderResourceView* nbSrv = nullptr; ID3D11SamplerState* nbSmp = nullptr; D3D11Buffer* nbBuf = nullptr;
                        if (ResolveNonBindless(b.binding, i, nbSrv, nbSmp, nbBuf)) {
                            byHandle = true;
                            if (nbSrv) views[i] = nbSrv;
                            else if (nbBuf) views[i] = b.rawBuffer ? nbBuf->GetSrv(D3D11Buffer::ViewKind::Raw, 0)
                                                                   : nbBuf->GetSrv(D3D11Buffer::ViewKind::Structured, b.stride);
                            continue;
                        }
                        if (!e) continue;
                        if (b.texture) {
                            if (i < e->srvs.size()) views[i] = e->srvs[i].Get();   // 写入描述符集时已建好并持有
                        } else if (i < e->buffers.size() && e->buffers[i]) {
                            auto* buf = LiveBuffer(e->buffers[i], b.set, b.binding);
                            if (!buf) continue;
                            views[i] = b.rawBuffer ? buf->GetSrv(D3D11Buffer::ViewKind::Raw, 0)
                                                   : buf->GetSrv(D3D11Buffer::ViewKind::Structured, b.stride);
                            if (!views[i]) ReportStrideConflict(buf, b);
                        }
                    }
                    // 句柄决定的绑定为空是调用方的意图（如非实例化绘制不用实例变换），不报告
                    if (!views[0] && !byHandle) ReportMissing(b.set, b.binding, b.texture ? "纹理" : "缓冲");
                    if (stage == D3D11PipelineState::VS)      ctx->VSSetShaderResources(b.slot, b.count, views.data());
                    else if (stage == D3D11PipelineState::PS) ctx->PSSetShaderResources(b.slot, b.count, views.data());
                    else                                      ctx->CSSetShaderResources(b.slot, b.count, views.data());
                    m_State->TrackSrvs(stage, b.slot, b.count, views.data());   // 冲突解绑要知道 SRV 指向谁
                    break;
                }
                case DxbcBindType::UAV: {
                    std::vector<ID3D11UnorderedAccessView*> views(b.count, nullptr);
                    bool byHandle = false;
                    for (u32 i = 0; i < b.count; ++i) {
                        ID3D11ShaderResourceView* nbSrv = nullptr; ID3D11SamplerState* nbSmp = nullptr; D3D11Buffer* nbBuf = nullptr;
                        if (ResolveNonBindless(b.binding, i, nbSrv, nbSmp, nbBuf)) {
                            byHandle = true;
                            if (nbBuf) {
                                views[i] = b.rawBuffer ? nbBuf->GetUav(D3D11Buffer::ViewKind::Raw, 0)
                                                       : nbBuf->GetUav(D3D11Buffer::ViewKind::Structured, b.stride);
                                if (views[i]) nbBuf->MarkGpuWritten();
                            }
                            continue;
                        }
                        if (!e) continue;
                        if (b.texture) {
                            if (i < e->uavs.size()) views[i] = e->uavs[i].Get();   // 写入描述符集时已建好并持有
                        } else if (i < e->buffers.size() && e->buffers[i]) {
                            auto* buf = LiveBuffer(e->buffers[i], b.set, b.binding);
                            if (!buf) continue;
                            views[i] = b.rawBuffer ? buf->GetUav(D3D11Buffer::ViewKind::Raw, 0)
                                                   : buf->GetUav(D3D11Buffer::ViewKind::Structured, b.stride);
                            if (!views[i]) ReportStrideConflict(buf, b);
                            // GPU 可能写它：CPU 下次读取前要回读（规划 3.10）
                            if (views[i]) buf->MarkGpuWritten();
                        }
                    }
                    for (u32 i = 0; i < b.count; ++i) {
                        if (!views[i]) continue;
                        ID3D11Resource* r = nullptr;
                        views[i]->GetResource(&r);
                        if (r) r->Release();   // 只取指针作比较
                        if (std::find(uavResources.begin(), uavResources.end(), r) != uavResources.end()) {
                            if (!m_WarnedUavAlias) {
                                m_WarnedUavAlias = true;
                                HE_CORE_WARN("[D3D11] PSO {} 的 (set {}, binding {}) 与前面的 UAV 是同一资源，D3D11 不允许别名，该槽绑空",
                                             m_Pipeline->GetName(), b.set, b.binding);
                            }
                            views[i] = nullptr;
                        } else {
                            uavResources.push_back(r);
                        }
                    }
                    if (!views[0] && !byHandle) ReportMissing(b.set, b.binding, "可写资源");
                    if (compute) {
                        UnbindSrvsOfUavs(views.data(), b.count);
                        ctx->CSSetUnorderedAccessViews(b.slot, b.count, views.data(), nullptr);
                    } else {
                        for (u32 i = 0; i < b.count && b.slot + i < dxbc::kDxbcMaxUAVSlots; ++i) {
                            graphicsUavs[b.slot + i] = views[i];
                        }
                        graphicsUavMin = std::min(graphicsUavMin, b.slot);
                        graphicsUavMax = std::max(graphicsUavMax, b.slot + b.count);
                    }
                    break;
                }
                case DxbcBindType::Sampler: {
                    std::vector<ID3D11SamplerState*> samplers(b.count, nullptr);
                    bool byHandle = false;
                    for (u32 i = 0; i < b.count; ++i) {
                        ID3D11ShaderResourceView* nbSrv = nullptr; ID3D11SamplerState* nbSmp = nullptr; D3D11Buffer* nbBuf = nullptr;
                        if (ResolveNonBindless(b.binding, i, nbSrv, nbSmp, nbBuf)) {
                            byHandle = true;
                            samplers[i] = nbSmp;
                            continue;
                        }
                        if (!e) continue;
                        if (i < e->samplers.size()) samplers[i] = e->samplers[i].Get();
                    }
                    if (!samplers[0] && !byHandle) ReportMissing(b.set, b.binding, "采样器");
                    if (stage == D3D11PipelineState::VS)      ctx->VSSetSamplers(b.slot, b.count, samplers.data());
                    else if (stage == D3D11PipelineState::PS) ctx->PSSetSamplers(b.slot, b.count, samplers.data());
                    else                                      ctx->CSSetSamplers(b.slot, b.count, samplers.data());
                    break;
                }
                case DxbcBindType::CBuffer: {
                    ID3D11Buffer* cb = nullptr;
                    if (e && !e->buffers.empty() && e->buffers[0]) {
                        if (D3D11Buffer* live = LiveBuffer(e->buffers[0], b.set, b.binding)) cb = live->GetConstantBuffer();
                    }
                    if (!cb) ReportMissing(b.set, b.binding, "常量缓冲");
                    if (stage == D3D11PipelineState::VS)      ctx->VSSetConstantBuffers(b.slot, 1, &cb);
                    else if (stage == D3D11PipelineState::PS) ctx->PSSetConstantBuffers(b.slot, 1, &cb);
                    else                                      ctx->CSSetConstantBuffers(b.slot, 1, &cb);
                    break;
                }
                default: break;
            }
        }
    }

    if (graphicsUavMax > graphicsUavMin) {
        // 保留当前渲染目标，只设置 UAV（像素着色器的 UAV 已由 ShaderToDxil 平移到 u8 之后）
        UnbindSrvsOfUavs(graphicsUavs + graphicsUavMin, graphicsUavMax - graphicsUavMin);
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
                                                       graphicsUavMin, graphicsUavMax - graphicsUavMin,
                                                       graphicsUavs + graphicsUavMin, nullptr);
    }
}

void D3D11CommandList::UnbindSrvsOfUavs(ID3D11UnorderedAccessView* const* uavs, u32 count) {
    std::vector<ID3D11Resource*> resources;
    for (u32 i = 0; i < count; ++i) {
        if (!uavs[i]) continue;
        ID3D11Resource* r = nullptr;
        uavs[i]->GetResource(&r);
        if (r) { resources.push_back(r); r->Release(); }   // 只取指针作比较
    }
    if (!resources.empty()) m_State->UnbindSrvsOf(resources.data(), static_cast<u32>(resources.size()));
}

bool D3D11CommandList::PrepareDrawOrDispatch(bool compute) {
    if (!m_Pipeline) return false;
    if (!m_Pipeline->IsValid()) {
        m_Pipeline->ReportUnusableOnce();
        return false;
    }
    if (m_Pipeline->IsCompute() != compute) return false;
    EnsureOwner();
    if (!compute) {
        // PSO 是否写深度决定用普通还是只读 DSV
        const bool wantReadOnly = !m_Pipeline->GetDepthWrite();
        if (m_DepthTexture && wantReadOnly != m_DepthReadOnly) ApplyRenderTargets();
        ApplyInputAssembler();
    }
    // 持久映射缓冲里 CPU 改过的内容先上传，再执行读它的命令（规划 3.10）。
    // 二级列表不在这里做：工作线程并发录制时它会让所有线程在同一把锁上逐条比较映射内容，
    // 改由主列表在 ExecuteSecondary 执行前做一次（上传立即生效，早于二级列表的命令执行）
    if (!m_IsSecondary) m_Device->FlushOpenMaps();
    FlushBindings();
    BindPushConstants();
    return true;
}

// ---------------------------------------------------------------------------
//  绘制与派发
// ---------------------------------------------------------------------------
void D3D11CommandList::Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) {
    if (!PrepareDrawOrDispatch(false)) return;
    m_Device->NoteExecuted(m_Pipeline->GetName());   // 诊断：GPU 等待超时时打印最近的 PSO
    m_State->context->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void D3D11CommandList::DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                                   i32 vertexOffset, u32 firstInstance) {
    if (!PrepareDrawOrDispatch(false)) return;
    m_Device->NoteExecuted(m_Pipeline->GetName());   // 诊断：GPU 等待超时时打印最近的 PSO
    m_State->context->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void D3D11CommandList::DrawIndexedIndirect(IRHIBuffer* buffer, u64 offset, u32 drawCount, u32 stride) {
    auto* args = static_cast<D3D11Buffer*>(buffer);
    if (!args || drawCount == 0) return;
    const u32 step = stride ? stride : static_cast<u32>(sizeof(IndirectDrawIndexedCommand));
    // 由计算着色器写入的参数缓冲先拷进影子参数缓冲（规划 3.3）；拷贝在 PrepareDrawOrDispatch 之前，
    // 不影响已下发的绘制状态
    ID3D11Buffer* argBuf = args->PrepareIndirectArgs(m_State->context, offset, u64(drawCount - 1) * step + sizeof(IndirectDrawIndexedCommand));
    if (!argBuf || !PrepareDrawOrDispatch(false)) return;
    m_Device->NoteExecuted(m_Pipeline->GetName());   // 诊断：GPU 等待超时时打印最近的 PSO
    // D3D11 一次只能画一条间接命令：逐条调用
    ID3D11DeviceContext4* ctx = m_State->context;
    for (u32 i = 0; i < drawCount; ++i) {
        ctx->DrawIndexedInstancedIndirect(argBuf, static_cast<UINT>(offset + u64(i) * step));
    }
}

void D3D11CommandList::DrawIndexedIndirectCount(IRHIBuffer* buffer, u64 offset, IRHIBuffer* countBuffer,
                                                u64 countOffset, u32 maxDrawCount, u32 stride) {
    auto* args  = static_cast<D3D11Buffer*>(buffer);
    auto* count = static_cast<D3D11Buffer*>(countBuffer);
    if (!args || !count || maxDrawCount == 0) return;
    const u32 step = stride ? stride : static_cast<u32>(sizeof(IndirectDrawIndexedCommand));
    // 参数整形（规划 3.3）：拷进影子参数缓冲，再由内置 CS 把序号 ≥ GPU 计数的命令 instanceCount 置 0，
    // 然后逐条绘制（条数见 EstimateIndirectCount，多出来的是空绘制）。整形 CS 会改动 CS 阶段状态，
    // 所以放在 PrepareDrawOrDispatch 之前，后者会重新应用本列表的状态
    ID3D11Buffer* argBuf = args->PrepareIndirectArgs(m_State->context, offset, u64(maxDrawCount - 1) * step + sizeof(IndirectDrawIndexedCommand));
    if (!argBuf) return;
    if (!m_Device->ClampIndirectCount(*m_State, count, countOffset, args->GetIndirectArgsUav(), offset, step, maxDrawCount)) {
        if (!m_WarnedIndirectCount) {
            m_WarnedIndirectCount = true;
            HE_CORE_WARN("[D3D11] DrawIndexedIndirectCount 的参数整形失败（计数缓冲或影子参数缓冲不可用），间接绘制被跳过");
        }
        return;
    }
    if (!PrepareDrawOrDispatch(false)) return;
    m_Device->NoteExecuted(m_Pipeline->GetName());   // 诊断：GPU 等待超时时打印最近的 PSO
    ID3D11DeviceContext4* ctx = m_State->context;
    // 逐条调用的次数按前几帧回读的计数估计（maxDrawCount 常是容量，如 Nanite 的 1M 条）
    const u32 loops = m_Device->EstimateIndirectCount(*m_State, count, countOffset, maxDrawCount);
    for (u32 i = 0; i < loops; ++i) {
        ctx->DrawIndexedInstancedIndirect(argBuf, static_cast<UINT>(offset + u64(i) * step));
    }
}

void D3D11CommandList::Dispatch(u32 x, u32 y, u32 z) {
    if (!PrepareDrawOrDispatch(true)) return;
    m_Device->NoteExecuted(m_Pipeline->GetName());   // 诊断：GPU 等待超时时打印最近的 PSO
    ID3D11DeviceContext4* ctx = m_State->context;
    // D3D11 每一维最多 65535 个工作组。引擎有「按容量派发、着色器按实际计数早退」的写法
    // （如 Nanite 软光栅按可见簇容量 1M 派发），超出部分截断：只有实际数量也超过上限时才会漏算
    constexpr u32 kMaxGroups = D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    if (x > kMaxGroups || y > kMaxGroups || z > kMaxGroups) {
        if (!m_WarnedDispatchClamp) {
            m_WarnedDispatchClamp = true;
            HE_CORE_WARN("[D3D11] PSO {} 的派发 ({}, {}, {}) 超过每维 {} 个工作组的上限，截断到上限",
                         m_Pipeline->GetName(), x, y, z, kMaxGroups);
        }
        x = std::min(x, kMaxGroups); y = std::min(y, kMaxGroups); z = std::min(z, kMaxGroups);
    }
    ctx->Dispatch(x, y, z);
    // 派发后解绑 CS 的 UAV：同一资源下一步常被当 SRV 读，留着 UAV 绑定会被运行时强制解绑并告警
    ID3D11UnorderedAccessView* nullUavs[dxbc::kDxbcMaxUAVSlots] = {};
    ctx->CSSetUnorderedAccessViews(0, dxbc::kDxbcMaxUAVSlots, nullUavs, nullptr);
}

void D3D11CommandList::DispatchIndirect(IRHIBuffer* buffer, u64 offset) {
    auto* args = static_cast<D3D11Buffer*>(buffer);
    if (!args) return;
    // 由计算着色器写入的参数缓冲先拷进影子参数缓冲（规划 3.3）
    ID3D11Buffer* argBuf = args->PrepareIndirectArgs(m_State->context, offset, 3u * sizeof(u32));
    if (!argBuf) return;
    if (!PrepareDrawOrDispatch(true)) return;
    m_Device->NoteExecuted(m_Pipeline->GetName());   // 诊断：GPU 等待超时时打印最近的 PSO
    ID3D11DeviceContext4* ctx = m_State->context;
    ctx->DispatchIndirect(argBuf, static_cast<UINT>(offset));
    ID3D11UnorderedAccessView* nullUavs[dxbc::kDxbcMaxUAVSlots] = {};
    ctx->CSSetUnorderedAccessViews(0, dxbc::kDxbcMaxUAVSlots, nullUavs, nullptr);
}

// ---------------------------------------------------------------------------
//  拷贝与清除
// ---------------------------------------------------------------------------
void D3D11CommandList::CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst, u64 size, u64 srcOffset, u64 dstOffset) {
    auto* s = static_cast<D3D11Buffer*>(src);
    auto* d = static_cast<D3D11Buffer*>(dst);
    if (!s || !d || size == 0) return;
    m_Device->FlushOpenMaps();   // 源缓冲里 CPU 未上传的改动先上传
    // 结构化缓冲的拷贝区间（源区间宽度、源起点、目标起点）必须是结构步长的整数倍。
    // 引擎按字节拷贝（如把 12 字节写进 32 字节步长的缓冲中间），不对齐时改走 CPU：
    // 读出源区间（必要时同步回读）再写进目标缓冲（正确优先，性能问题记录不修）
    auto aligned = [](u32 stride, u64 v) { return stride == 0 || v % stride == 0; };
    const u32 ss = s->GetStructStride(), ds = d->GetStructStride();
    if (!aligned(ss, srcOffset) || !aligned(ss, size) || !aligned(ds, dstOffset) || !aligned(ds, size) ||
        (ss && ds && ss != ds)) {
        std::vector<u8> tmp(static_cast<size_t>(size));
        s->ReadRegion(srcOffset, tmp.data(), size);
        d->WriteRegion(dstOffset, tmp.data(), size);
        return;
    }
    D3D11_BOX box{ static_cast<UINT>(srcOffset), 0, 0, static_cast<UINT>(srcOffset + size), 1, 1 };
    m_State->context->CopySubresourceRegion(d->GetBuffer(), 0, static_cast<UINT>(dstOffset), 0, 0,
                                                  s->GetBuffer(), 0, &box);
    d->MarkGpuWritten();
}

void D3D11CommandList::CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) {
    auto* s = static_cast<D3D11Texture*>(src);
    auto* d = static_cast<D3D11Texture*>(dst);
    if (!s || !d || s == d) return;
    m_State->context->CopyResource(d->GetResource(), s->GetResource());
}

void D3D11CommandList::CopyTextureToBuffer(IRHITexture* src, IRHIBuffer* dst, u32 x, u32 y,
                                           u32 width, u32 height, u64 bufferOffset) {
    auto* t = static_cast<D3D11Texture*>(src);
    auto* b = static_cast<D3D11Buffer*>(dst);
    if (!t || !b || t->Is3D() || t->GetSampleCount() > 1) return;
    const u32 w = width ? width : t->GetWidth();
    const u32 h = height ? height : t->GetHeight();
    if (w == 0 || h == 0 || x + w > t->GetWidth() || y + h > t->GetHeight()) return;

    // 暂存纹理：只覆盖要读的区域，格式与源资源相同（CopySubresourceRegion 要求格式兼容）
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(t->GetResource()->QueryInterface(IID_PPV_ARGS(&srcTex)))) return;
    D3D11_TEXTURE2D_DESC sd{};
    srcTex->GetDesc(&sd);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = sd.Format;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateTexture2D(&td, nullptr, &staging), "CreateTexture2D(回读)")) return;

    // 同步回读只能在 immediate context 上做（deferred context 不能 Map 读）：二级列表里调用时
    // 拷贝会立刻执行，早于该列表之前录下的命令（引擎只在主列表里调用它）
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    D3D11_BOX box{ x, y, 0, x + w, y + h, 1 };
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, t->GetResource(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;   // 同步等待 GPU
    const u32 rowBytes = w * FormatByteSize(t->GetFormat());
    std::vector<u8> packed(static_cast<size_t>(rowBytes) * h);
    for (u32 row = 0; row < h; ++row) {
        std::memcpy(packed.data() + static_cast<size_t>(row) * rowBytes,
                    static_cast<const u8*>(ms.pData) + static_cast<size_t>(row) * ms.RowPitch, rowBytes);
    }
    ctx->Unmap(staging.Get(), 0);
    b->WriteRegion(bufferOffset, packed.data(), packed.size());
}

void D3D11CommandList::ClearDepthStencil(IRHITexture* texture, float depth) {
    auto* t = static_cast<D3D11Texture*>(texture);
    if (!t || !t->IsDepth()) return;
    UINT flags = D3D11_CLEAR_DEPTH;
    if (HasStencil(t->GetFormat())) flags |= D3D11_CLEAR_STENCIL;
    m_State->context->ClearDepthStencilView(t->GetDsv(0, 0, false), flags, depth, 0);
}

// ---------------------------------------------------------------------------
//  调试标签
// ---------------------------------------------------------------------------
void D3D11CommandList::BeginDebugLabel(const char* name, const float*) {
    if (auto* a = m_State->annotation.Get()) {
        a->BeginEvent(Utf8ToWide(name).c_str());
        ++m_LabelDepth;
    }
}

void D3D11CommandList::EndDebugLabel() {
    if (m_LabelDepth == 0) return;
    if (auto* a = m_State->annotation.Get()) a->EndEvent();
    --m_LabelDepth;
}

void D3D11CommandList::InsertDebugLabel(const char* name, const float*) {
    if (auto* a = m_State->annotation.Get()) a->SetMarker(Utf8ToWide(name).c_str());
}

} // namespace he::rhi::d3d11
