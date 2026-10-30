// ============================================================================
//  D3D11CommandList.cpp —— D3D11 命令列表（说明见头文件）
// ============================================================================
#include "D3D11CommandList.h"
#include "D3D11Device.h"
#include "D3D11Resources.h"
#include "RHI/DxbcBindTable.h"

#include <algorithm>
#include <vector>

namespace he::rhi::d3d11 {

using dxbc::DxbcBindType;

void D3D11CommandList::Begin() {
    // 每次录制从干净的绑定开始（与 Vulkan 新开命令缓冲时没有任何绑定一致）
    m_Pipeline = nullptr;
    for (auto& s : m_Sets) s = kInvalidSet;
}

// ---------------------------------------------------------------------------
//  管线与描述符集
// ---------------------------------------------------------------------------
void D3D11CommandList::SetPipeline(IRHIPipelineState* pso) {
    m_Pipeline = static_cast<D3D11PipelineState*>(pso);
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

void D3D11CommandList::BindDescriptorSet(u32 setIndex, DescriptorSetHandle set) {
    if (setIndex < kMaxSets) m_Sets[setIndex] = set;
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

    const D3D11PipelineState::Stage stages[] = { D3D11PipelineState::VS, D3D11PipelineState::PS };
    const D3D11PipelineState::Stage* first = compute ? nullptr : stages;
    const u32 stageCount = compute ? 1u : 2u;
    for (u32 si = 0; si < stageCount; ++si) {
        const auto stage = compute ? D3D11PipelineState::CS : first[si];
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
    // 持久映射缓冲里 CPU 改过的内容先上传，再执行读它的命令（规划 3.10）
    m_Device->FlushOpenMaps();
    FlushBindings();
    return true;
}

// ---------------------------------------------------------------------------
//  绘制与派发
// ---------------------------------------------------------------------------
void D3D11CommandList::Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) {
    if (!PrepareDrawOrDispatch(false)) return;
    m_Device->GetContext()->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void D3D11CommandList::DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                                   i32 vertexOffset, u32 firstInstance) {
    if (!PrepareDrawOrDispatch(false)) return;
    m_Device->GetContext()->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void D3D11CommandList::Dispatch(u32 x, u32 y, u32 z) {
    if (!PrepareDrawOrDispatch(true)) return;
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    ctx->Dispatch(x, y, z);
    // 派发后解绑 CS 的 UAV：同一资源下一步常被当 SRV 读，留着 UAV 绑定会被运行时强制解绑并告警
    ID3D11UnorderedAccessView* nullUavs[dxbc::kDxbcMaxUAVSlots] = {};
    ctx->CSSetUnorderedAccessViews(0, dxbc::kDxbcMaxUAVSlots, nullUavs, nullptr);
}

} // namespace he::rhi::d3d11
