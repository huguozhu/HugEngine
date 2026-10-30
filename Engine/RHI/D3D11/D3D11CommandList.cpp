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

D3D11CommandList::~D3D11CommandList() {
    // 上下文所有者只做指针比较：本列表销毁后若有新列表恰好分配到同一地址，会误以为状态仍在上下文里
    if (m_Device && m_Device->GetContextOwner() == this) m_Device->SetContextOwner(nullptr);
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
    // 让下一次改动上下文时重新应用（即使上下文的上一个使用者就是本列表的上一轮录制）
    if (m_Device->GetContextOwner() == this) m_Device->SetContextOwner(nullptr);
}

void D3D11CommandList::End() {
    // 未配对的调试分组补齐（与 D3D12 一致）
    while (m_LabelDepth > 0) EndDebugLabel();
}

// ---------------------------------------------------------------------------
//  上下文所有权与状态应用
// ---------------------------------------------------------------------------
void D3D11CommandList::EnsureOwner() {
    if (m_Device->GetContextOwner() == this) return;
    m_Device->SetContextOwner(this);
    ApplyPipeline();
    ApplyRenderTargets();
    ApplyInputAssembler();
    ApplyViewportScissor();
    // 描述符集的资源在每次绘制 / 派发前都会重新下发，不需要在这里处理
}

void D3D11CommandList::ApplyPipeline() {
    if (!m_Pipeline || !m_Pipeline->IsValid()) return;
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
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
    m_Device->GetContext()->OMSetRenderTargets(m_RtvCount, m_RtvCount ? m_Rtvs : nullptr, dsv);
}

void D3D11CommandList::ApplyInputAssembler() {
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
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
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
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
    ID3D11DeviceContext4* ctx = m_Device->GetContext();

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

    if (clearColor) {
        static const float kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (u32 i = 0; i < m_RtvCount; ++i) {
            if (m_Rtvs[i]) ctx->ClearRenderTargetView(m_Rtvs[i], colorClears ? colorClears[i].color : kZero);
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
    // 主通道的清屏值默认黑色、alpha = 1（与 D3D12 一致）
    ClearValue c = clear ? *clear : ClearValue{};
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
    m_Device->GetContext()->OMSetRenderTargets(0, nullptr, nullptr);
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

void D3D11CommandList::SetPushConstants(u32 offset, u32 size, const void* data) {
    if (!data || offset >= sizeof(m_PushData)) return;
    const u32 n = std::min<u32>(size, static_cast<u32>(sizeof(m_PushData)) - offset);
    std::memcpy(m_PushData + offset, data, n);
    m_PushDirty = true;
}

void D3D11CommandList::BindPushConstants() {
    if (!m_Pipeline->UsesPushConstants()) return;
    // 数据改过，或常量环转了一圈（旧槽位已作废）：写进新槽
    if (m_PushDirty || m_PushGeneration != m_Device->GetPushRingGeneration()) {
        if (!m_Device->AllocatePushConstants(m_PushData, m_PushFirstConstant, m_PushGeneration)) return;
        m_PushDirty = false;
    }
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    ID3D11Buffer* ring = m_Device->GetPushConstantRing();
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
    for (const auto& [fence, value] : m_PendingSignals) {
        if (ID3D11Fence* f = m_Device->FindFence(fence)) m_Device->GetContext()->Signal(f, value);
    }
    m_PendingSignals.clear();
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
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    const bool compute = m_Pipeline->IsCompute();

    // 图形管线里所有阶段的 UAV 要和渲染目标一起设置（OMSetRenderTargetsAndUnorderedAccessViews）
    ID3D11UnorderedAccessView* graphicsUavs[dxbc::kDxbcMaxUAVSlots] = {};
    u32 graphicsUavMin = dxbc::kDxbcMaxUAVSlots, graphicsUavMax = 0;

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
                    for (u32 i = 0; i < b.count && e; ++i) {
                        if (b.texture) {
                            if (e->view) {
                                if (const D3D11ViewInfo* vi = m_Device->FindView(e->view)) {
                                    views[i] = vi->texture->GetSrv(vi->mip, vi->layer, vi->single);
                                }
                            } else if (i < e->textures.size() && e->textures[i]) {
                                views[i] = static_cast<D3D11Texture*>(e->textures[i])->GetSrv();
                            }
                        } else if (i < e->buffers.size() && e->buffers[i]) {
                            auto* buf = static_cast<D3D11Buffer*>(e->buffers[i]);
                            views[i] = b.rawBuffer ? buf->GetSrv(D3D11Buffer::ViewKind::Raw, 0)
                                                   : buf->GetSrv(D3D11Buffer::ViewKind::Structured, b.stride);
                        }
                    }
                    if (!views[0]) ReportMissing(b.set, b.binding, b.texture ? "纹理" : "缓冲");
                    if (stage == D3D11PipelineState::VS)      ctx->VSSetShaderResources(b.slot, b.count, views.data());
                    else if (stage == D3D11PipelineState::PS) ctx->PSSetShaderResources(b.slot, b.count, views.data());
                    else                                      ctx->CSSetShaderResources(b.slot, b.count, views.data());
                    break;
                }
                case DxbcBindType::UAV: {
                    std::vector<ID3D11UnorderedAccessView*> views(b.count, nullptr);
                    for (u32 i = 0; i < b.count && e; ++i) {
                        if (b.texture) {
                            if (e->view) {
                                if (const D3D11ViewInfo* vi = m_Device->FindView(e->view)) {
                                    views[i] = vi->texture->GetUav(vi->mip, vi->layer, vi->single);
                                }
                            } else if (i < e->textures.size() && e->textures[i]) {
                                views[i] = static_cast<D3D11Texture*>(e->textures[i])->GetUav();
                            }
                        } else if (i < e->buffers.size() && e->buffers[i]) {
                            auto* buf = static_cast<D3D11Buffer*>(e->buffers[i]);
                            views[i] = b.rawBuffer ? buf->GetUav(D3D11Buffer::ViewKind::Raw, 0)
                                                   : buf->GetUav(D3D11Buffer::ViewKind::Structured, b.stride);
                            // GPU 可能写它：CPU 下次读取前要回读（规划 3.10）
                            if (views[i]) buf->MarkGpuWritten();
                        }
                    }
                    if (!views[0]) ReportMissing(b.set, b.binding, "可写资源");
                    if (compute) {
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
                    for (u32 i = 0; i < b.count && e; ++i) {
                        if (i < e->samplers.size() && e->samplers[i]) {
                            samplers[i] = static_cast<D3D11Sampler*>(e->samplers[i])->GetState();
                        }
                    }
                    if (!samplers[0]) ReportMissing(b.set, b.binding, "采样器");
                    if (stage == D3D11PipelineState::VS)      ctx->VSSetSamplers(b.slot, b.count, samplers.data());
                    else if (stage == D3D11PipelineState::PS) ctx->PSSetSamplers(b.slot, b.count, samplers.data());
                    else                                      ctx->CSSetSamplers(b.slot, b.count, samplers.data());
                    break;
                }
                case DxbcBindType::CBuffer: {
                    ID3D11Buffer* cb = nullptr;
                    if (e && !e->buffers.empty() && e->buffers[0]) {
                        cb = static_cast<D3D11Buffer*>(e->buffers[0])->GetConstantBuffer();
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
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
                                                       graphicsUavMin, graphicsUavMax - graphicsUavMin,
                                                       graphicsUavs + graphicsUavMin, nullptr);
    }
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
    // 持久映射缓冲里 CPU 改过的内容先上传，再执行读它的命令（规划 3.10）
    m_Device->FlushOpenMaps();
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
    m_Device->GetContext()->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void D3D11CommandList::DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                                   i32 vertexOffset, u32 firstInstance) {
    if (!PrepareDrawOrDispatch(false)) return;
    m_Device->NoteExecuted(m_Pipeline->GetName());   // 诊断：GPU 等待超时时打印最近的 PSO
    m_Device->GetContext()->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void D3D11CommandList::DrawIndexedIndirect(IRHIBuffer* buffer, u64 offset, u32 drawCount, u32 stride) {
    auto* args = static_cast<D3D11Buffer*>(buffer);
    if (!args) return;
    // 既是 Storage（CS 写入）又是 Indirect 的缓冲不能直接作间接参数（D3D11 的参数缓冲不能是结构化缓冲），
    // 需要 M3.5 的参数整形 CS；在那之前跳过并提示一次
    if (HasAny(args->GetUsage(), BufferUsage::Storage)) {
        if (!m_WarnedIndirectStorage) {
            m_WarnedIndirectStorage = true;
            HE_CORE_WARN("[D3D11] 由计算着色器写入的间接参数缓冲暂不支持（M3.5 实现参数整形），间接绘制被跳过");
        }
        return;
    }
    if (!PrepareDrawOrDispatch(false)) return;
    m_Device->NoteExecuted(m_Pipeline->GetName());   // 诊断：GPU 等待超时时打印最近的 PSO
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    const u32 step = stride ? stride : static_cast<u32>(sizeof(IndirectDrawIndexedCommand));
    for (u32 i = 0; i < drawCount; ++i) {
        ctx->DrawIndexedInstancedIndirect(args->GetBuffer(), static_cast<UINT>(offset + u64(i) * step));
    }
}

void D3D11CommandList::DrawIndexedIndirectCount(IRHIBuffer*, u64, IRHIBuffer*, u64, u32, u32) {
    if (!m_WarnedIndirectCount) {
        m_WarnedIndirectCount = true;
        HE_CORE_WARN("[D3D11] DrawIndexedIndirectCount 需要 M3.5 的参数整形，暂时跳过");
    }
}

void D3D11CommandList::Dispatch(u32 x, u32 y, u32 z) {
    if (!PrepareDrawOrDispatch(true)) return;
    m_Device->NoteExecuted(m_Pipeline->GetName());   // 诊断：GPU 等待超时时打印最近的 PSO
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    ctx->Dispatch(x, y, z);
    // 派发后解绑 CS 的 UAV：同一资源下一步常被当 SRV 读，留着 UAV 绑定会被运行时强制解绑并告警
    ID3D11UnorderedAccessView* nullUavs[dxbc::kDxbcMaxUAVSlots] = {};
    ctx->CSSetUnorderedAccessViews(0, dxbc::kDxbcMaxUAVSlots, nullUavs, nullptr);
}

void D3D11CommandList::DispatchIndirect(IRHIBuffer* buffer, u64 offset) {
    auto* args = static_cast<D3D11Buffer*>(buffer);
    if (!args) return;
    if (HasAny(args->GetUsage(), BufferUsage::Storage)) {
        if (!m_WarnedIndirectStorage) {
            m_WarnedIndirectStorage = true;
            HE_CORE_WARN("[D3D11] 由计算着色器写入的间接参数缓冲暂不支持（M3.5 实现参数整形），间接派发被跳过");
        }
        return;
    }
    if (!PrepareDrawOrDispatch(true)) return;
    m_Device->NoteExecuted(m_Pipeline->GetName());   // 诊断：GPU 等待超时时打印最近的 PSO
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    ctx->DispatchIndirect(args->GetBuffer(), static_cast<UINT>(offset));
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
    m_Device->GetContext()->CopySubresourceRegion(d->GetBuffer(), 0, static_cast<UINT>(dstOffset), 0, 0,
                                                  s->GetBuffer(), 0, &box);
    d->MarkGpuWritten();
}

void D3D11CommandList::CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) {
    auto* s = static_cast<D3D11Texture*>(src);
    auto* d = static_cast<D3D11Texture*>(dst);
    if (!s || !d || s == d) return;
    m_Device->GetContext()->CopyResource(d->GetResource(), s->GetResource());
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
    m_Device->GetContext()->ClearDepthStencilView(t->GetDsv(0, 0, false), flags, depth, 0);
}

// ---------------------------------------------------------------------------
//  调试标签
// ---------------------------------------------------------------------------
void D3D11CommandList::BeginDebugLabel(const char* name, const float*) {
    if (auto* a = m_Device->GetAnnotation()) {
        a->BeginEvent(Utf8ToWide(name).c_str());
        ++m_LabelDepth;
    }
}

void D3D11CommandList::EndDebugLabel() {
    if (m_LabelDepth == 0) return;
    if (auto* a = m_Device->GetAnnotation()) a->EndEvent();
    --m_LabelDepth;
}

void D3D11CommandList::InsertDebugLabel(const char* name, const float*) {
    if (auto* a = m_Device->GetAnnotation()) a->SetMarker(Utf8ToWide(name).c_str());
}

} // namespace he::rhi::d3d11
